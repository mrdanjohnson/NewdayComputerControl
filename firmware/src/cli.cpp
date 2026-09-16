#include "cli.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <sstream>
#include <vector>
#include "esp_clock.h"
#include "esp_rng.h"
#include "log_sink.h"
#include "mc_auth.h"
#include "mc_engine.h"
#include "mc_ids.h"
#include "mc_iso8601.h"
#include "mc_log.h"
#include "mdns_service.h"
#include "nvs_config.h"
#include "status_cache.h"
#include "web_ui.h"
#include "wifi_mgr.h"

namespace {
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream is(line);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

void printKeyRecord(const mcco::KeyRecord& r) {
    Serial.printf("  %s  role=%-7s %-10s active=%s created=%s\n", r.key_id.c_str(),
                  mcco::role_to_string(r.role), r.label.c_str(), r.active ? "yes" : "no",
                  mcco::iso8601_format(r.created_at).c_str());
}
} // namespace

void Cli::begin(AppContext* ctx) {
    ctx_ = ctx;
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && millis() - start < 3000) delay(10); // native USB CDC may be absent
    printBanner();
}

void Cli::poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            std::string line = line_buf_;
            line_buf_.clear();
            if (!line.empty()) handleLine(line);
        } else if (line_buf_.size() < 200) {
            line_buf_ += c;
        }
    }
}

void Cli::printBanner() {
    const mcco::Identity& id = ctx_->config->identity();
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    Serial.printf("\nMacControl Phase 1 (Mode A) — hostname: %s  device_id: %s  mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  id.hostname.c_str(), id.device_id.c_str(), mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    Serial.println("Physical console = ADMIN. Type 'help' for commands.");
}

void Cli::cmdHelp() {
    Serial.println(
        "Commands:\n"
        "  help\n"
        "  identity show\n"
        "  identity set name|hostname|location|description <value>\n"
        "  key create READ|CONTROL|ADMIN <label>\n"
        "  key list\n"
        "  key revoke <key_id>\n"
        "  wifi set <ssid> <pass>\n"
        "  wifi status\n"
        "  ntp set <server>\n"
        "  admin set <password>   (min 10 chars; never echoed to the log)\n"
        "  admin status\n"
        "  status\n"
        "  reboot");
}

void Cli::handleLine(const std::string& line) {
    std::vector<std::string> args = split(line);
    const std::string cmd = args[0]; // copy: erase() below would invalidate a reference
    args.erase(args.begin());

    if (cmd == "help") {
        cmdHelp();
    } else if (cmd == "identity") {
        cmdIdentity(args);
    } else if (cmd == "key") {
        cmdKey(args);
    } else if (cmd == "wifi") {
        cmdWifi(args);
    } else if (cmd == "ntp") {
        cmdNtp(args);
    } else if (cmd == "admin") {
        cmdAdmin(args);
    } else if (cmd == "status") {
        cmdStatus();
    } else if (cmd == "reboot") {
        cmdReboot();
    } else {
        Serial.println("unknown command; type 'help'");
    }
}

void Cli::cmdIdentity(const std::vector<std::string>& args) {
    if (args.empty()) {
        Serial.println("usage: identity show | identity set <field> <value>");
        return;
    }
    if (args[0] == "show") {
        const mcco::Identity& id = ctx_->config->identity();
        Serial.printf("device_name: %s\nhostname: %s\nlocation: %s\ndescription: %s\ndevice_id: %s\n",
                      id.device_name.c_str(), id.hostname.c_str(), id.location.c_str(),
                      id.description.c_str(), id.device_id.c_str());
        return;
    }
    if (args[0] == "set" && args.size() >= 3) {
        const std::string& field = args[1];
        // Rejoin the value (may contain spaces).
        std::string value;
        for (size_t i = 2; i < args.size(); i++) {
            if (i > 2) value += " ";
            value += args[i];
        }
        mcco::Identity id = ctx_->config->identity();
        if (field == "name") {
            id.device_name = value;
        } else if (field == "hostname") {
            if (!mcco::hostname_valid(value)) {
                Serial.println("invalid hostname: 1-57 chars [a-z0-9-], start/end alphanumeric");
                return;
            }
            id.hostname = value;
        } else if (field == "location") {
            id.location = value;
        } else if (field == "description") {
            id.description = value;
        } else {
            Serial.println("unknown field; use name|hostname|location|description");
            return;
        }
        if (!ctx_->config->saveIdentity(id)) {
            Serial.println("persist failed");
            return;
        }
        ctx_->status_cache->onIdentityChanged();
        if (field == "hostname") {
            ctx_->mdns->reannounce(id); // re-announce within 2 s (spec 3.1.1)
            Serial.println("identity saved; mDNS re-announced");
        } else {
            Serial.println("identity saved");
        }
        return;
    }
    Serial.println("usage: identity show | identity set <field> <value>");
}

void Cli::cmdKey(const std::vector<std::string>& args) {
    if (args.empty()) {
        Serial.println("usage: key create READ|CONTROL|ADMIN <label> | key list | key revoke <key_id>");
        return;
    }
    if (args[0] == "create" && args.size() >= 3) {
        mcco::Role role;
        if (!mcco::role_from_string(args[1].c_str(), role)) {
            Serial.println("role must be READ, CONTROL or ADMIN");
            return;
        }
        std::string label;
        for (size_t i = 2; i < args.size(); i++) {
            if (i > 2) label += " ";
            label += args[i];
        }
        std::string raw = mcco::make_api_key(*ctx_->rng);
        std::string key_id;
        bool added;
        {
            Guard g(ctx_->engine_mutex);
            added = ctx_->keys->add(raw, role, label, ctx_->clock->epoch_seconds(), key_id);
        }
        if (!added) {
            Serial.println("key store full (max 8 active keys)");
            return;
        }
        if (!ctx_->config->persistKeys(*ctx_->keys)) {
            Serial.println("WARNING: persist failed; key will be lost on reboot");
        }
        // The raw key is shown exactly once here and never written to the log.
        Serial.printf("created %s (%s)\n", key_id.c_str(), label.c_str());
        Serial.println("API key (shown once, store it now):");
        Serial.println(raw.c_str());
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "key_created", nullptr,
                         nullptr, nullptr,
                         (std::string("{\"key_id\":\"") + key_id + "\"}").c_str());
        return;
    }
    if (args[0] == "list") {
        mcco::KeyStore& ks = *ctx_->keys;
        Guard g(ctx_->engine_mutex);
        if (ks.records().empty()) {
            Serial.println("(no keys)");
            return;
        }
        for (const auto& r : ks.records()) printKeyRecord(r);
        Serial.printf("%u active of %u max\n", (unsigned)ks.active_count(),
                      (unsigned)mcco::KeyStore::kMaxActive);
        return;
    }
    if (args[0] == "revoke" && args.size() >= 2) {
        bool revoked;
        {
            Guard g(ctx_->engine_mutex);
            revoked = ctx_->keys->revoke(args[1]);
        }
        if (!revoked) {
            Serial.println("no such active key");
            return;
        }
        ctx_->config->persistKeys(*ctx_->keys);
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "key_revoked", nullptr,
                         nullptr, nullptr,
                         (std::string("{\"key_id\":\"") + args[1] + "\"}").c_str());
        Serial.println("key revoked");
        return;
    }
    Serial.println("usage: key create READ|CONTROL|ADMIN <label> | key list | key revoke <key_id>");
}

void Cli::cmdWifi(const std::vector<std::string>& args) {
    if (args.empty()) {
        Serial.println("usage: wifi set <ssid> <pass> | wifi status");
        return;
    }
    if (args[0] == "set" && args.size() >= 3) {
        if (!ctx_->config->saveWifi(args[1], args[2])) {
            Serial.println("persist failed");
            return;
        }
        ctx_->wifi->setCredentials(args[1], args[2]);
        Serial.println("wifi credentials saved; reconnecting");
        return;
    }
    if (args[0] == "status") {
        if (ctx_->wifi->connected()) {
            Serial.printf("connected ip=%s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("not connected");
        }
        return;
    }
    Serial.println("usage: wifi set <ssid> <pass> | wifi status");
}

void Cli::cmdNtp(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[0] == "set") {
        if (!ctx_->config->saveNtp(args[1])) {
            Serial.println("persist failed");
            return;
        }
        static_cast<EspClock*>(ctx_->clock)->beginSntp(args[1]);
        Serial.println("ntp server saved");
        return;
    }
    Serial.println("usage: ntp set <server>");
}

void Cli::cmdStatus() {
    const mcco::Identity& id = ctx_->config->identity();
    Serial.printf("hostname: %s  device_id: %s\n", id.hostname.c_str(), id.device_id.c_str());
    Serial.printf("wifi: %s\n", ctx_->wifi->connected() ? "connected" : "down");
    Serial.printf("usb hid: %s\n", ctx_->status_cache->usbUp() ? "mounted" : "not mounted");
    {
        Guard g(ctx_->engine_mutex);
        Serial.printf("ledger: %u commands\n", (unsigned)ctx_->ledger->command_count());
        Serial.printf("keys: %u active\n", (unsigned)ctx_->keys->active_count());
    }
    Serial.printf("sntp synced: %s\n", static_cast<EspClock*>(ctx_->clock)->synced() ? "yes" : "no");
    Serial.printf("log entries: %u\n", (unsigned)ctx_->log->count());
}

void Cli::cmdAdmin(const std::vector<std::string>& args) {
    if (args.empty()) {
        Serial.println("usage: admin set <password> | admin status");
        return;
    }
    if (args[0] == "set" && args.size() >= 2) {
        std::string password;
        for (size_t i = 1; i < args.size(); i++) {
            if (i > 1) password += " ";
            password += args[i];
        }
        std::string err;
        if (!ctx_->web_ui->setPassword(password.c_str(), err)) {
            Serial.printf("admin password NOT set: %s\n", err.c_str());
            return;
        }
        // The password itself is never printed or logged (spec 13.1.1).
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "admin_password_set",
                         nullptr, nullptr, nullptr, nullptr);
        Serial.println("admin password set");
        return;
    }
    if (args[0] == "status") {
        Serial.printf("admin password: %s\n",
                      ctx_->web_ui->passwordSet() ? "set" : "not set");
        return;
    }
    Serial.println("usage: admin set <password> | admin status");
}

void Cli::cmdReboot() {
    Serial.println("rebooting...");
    delay(200);
    ESP.restart();
}
