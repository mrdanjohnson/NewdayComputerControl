#include "cli.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <sstream>
#include <vector>

// On ESP32-S3 the console output path that survives every USB configuration
// is ets_printf (ROM writes straight into the UART0 FIFO, which the USB-Serial
// JTAG peripheral mirrors to the host before TinyUSB starts, and which also
// drives the TX pin for a classic UART bridge on the board's COM header). The
// IDF UART driver TX path proved unreliable in this core build, so TX never
// goes through it. RX reads via the UART0 driver when its ISR is alive and
// falls back to polling the RX FIFO registers directly when it is not. Other
// chips keep the plain UART console.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <driver/uart.h>
#include <hal/uart_ll.h>
#define MC_CONSOLE_JTAG 1
#endif
#include "esp_clock.h"
#include "esp_rng.h"
#include "agent_link.h"
#include "log_sink.h"
#include "mc_auth.h"
#include "mc_engine.h"
#include "mc_ids.h"
#include "mc_iso8601.h"
#include "mc_log.h"
#include "mc_pairing.h"
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

#ifdef MC_CONSOLE_JTAG
// TX goes through ets_printf (ROM path into the UART0 FIFO, mirrored by the
// USB-Serial-JTAG peripheral to the host and also driven out the TX pin). The
// UART driver TX path and the usb_serial_jtag VCP write path both proved
// unreliable here. RX tries the installed UART0 driver first, then falls back
// to polling the RX FIFO directly for builds where the driver ISR is dead.
struct JtagConsole {
    void begin() {}
    int read() {
        uint8_t b;
        if (uart_read_bytes(UART_NUM_0, &b, 1, 0) == 1) return b;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        uart_dev_t* hw = &UART0;
        if (uart_ll_get_rxfifo_len(hw) > 0) {
            uart_ll_read_rxfifo(hw, &b, 1);
            return b;
        }
#endif
        return -1;
    }
};
JtagConsole g_console;

void console_println(const char* s) {
    ets_printf("%s\n", s);
}
void console_printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ets_printf("%s", buf);
}
#else
#define console_println(s) Serial.println(s)
#define console_printf(...) Serial.printf(__VA_ARGS__)
#endif

void printKeyRecord(const mcco::KeyRecord& r) {
    console_printf("  %s  role=%-7s %-10s active=%s created=%s\n", r.key_id.c_str(),
                  mcco::role_to_string(r.role), r.label.c_str(), r.active ? "yes" : "no",
                  mcco::iso8601_format(r.created_at).c_str());
}
} // namespace

void Cli::begin(AppContext* ctx) {
    ctx_ = ctx;
#ifdef MC_CONSOLE_JTAG
    // Install the UART0 driver when possible so uart_read_bytes works when
    // the driver ISR is healthy; console TX itself never uses the driver.
    Serial.begin(115200);
    g_console.begin();
#else
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && millis() - start < 3000) delay(10); // native USB CDC may be absent
#endif
    printBanner();
}

void Cli::poll() {
#ifdef MC_CONSOLE_JTAG
    for (int c = g_console.read(); c >= 0; c = g_console.read()) {
        handleChar((char)c);
    }
#else
    while (Serial.available()) {
        handleChar((char)Serial.read());
    }
#endif
}

void Cli::handleChar(char c) {
    if (c == '\r') return;
    if (c == '\n') {
        std::string line = line_buf_;
        line_buf_.clear();
        if (!line.empty()) handleLine(line);
    } else if (line_buf_.size() < 200) {
        line_buf_ += c;
    }
}

void Cli::printBanner() {
    const mcco::Identity& id = ctx_->config->identity();
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    console_printf("\nMacControl (Mode A/B) — hostname: %s  device_id: %s  mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
                  id.hostname.c_str(), id.device_id.c_str(), mac[0], mac[1], mac[2], mac[3],
                  mac[4], mac[5]);
    console_println("Physical console = ADMIN. Type 'help' for commands.");
}

void Cli::cmdHelp() {
    console_println(
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
        "  agent pair [seconds]   (open the pairing window, 60-600, default 120)\n"
        "  agent status\n"
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
    } else if (cmd == "agent") {
        cmdAgent(args);
    } else if (cmd == "reboot") {
        cmdReboot();
    } else {
        console_println("unknown command; type 'help'");
    }
}

void Cli::cmdIdentity(const std::vector<std::string>& args) {
    if (args.empty()) {
        console_println("usage: identity show | identity set <field> <value>");
        return;
    }
    if (args[0] == "show") {
        const mcco::Identity& id = ctx_->config->identity();
        console_printf("device_name: %s\nhostname: %s\nlocation: %s\ndescription: %s\ndevice_id: %s\n",
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
                console_println("invalid hostname: 1-57 chars [a-z0-9-], start/end alphanumeric");
                return;
            }
            id.hostname = value;
        } else if (field == "location") {
            id.location = value;
        } else if (field == "description") {
            id.description = value;
        } else {
            console_println("unknown field; use name|hostname|location|description");
            return;
        }
        if (!ctx_->config->saveIdentity(id)) {
            console_println("persist failed");
            return;
        }
        ctx_->status_cache->onIdentityChanged();
        if (field == "hostname") {
            ctx_->mdns->reannounce(id); // re-announce within 2 s (spec 3.1.1)
            console_println("identity saved; mDNS re-announced");
        } else {
            console_println("identity saved");
        }
        return;
    }
    console_println("usage: identity show | identity set <field> <value>");
}

void Cli::cmdKey(const std::vector<std::string>& args) {
    if (args.empty()) {
        console_println("usage: key create READ|CONTROL|ADMIN <label> | key list | key revoke <key_id>");
        return;
    }
    if (args[0] == "create" && args.size() >= 3) {
        mcco::Role role;
        if (!mcco::role_from_string(args[1].c_str(), role)) {
            console_println("role must be READ, CONTROL or ADMIN");
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
            console_println("key store full (max 8 active keys)");
            return;
        }
        if (!ctx_->config->persistKeys(*ctx_->keys)) {
            console_println("WARNING: persist failed; key will be lost on reboot");
        }
        // The raw key is shown exactly once here and never written to the log.
        console_printf("created %s (%s)\n", key_id.c_str(), label.c_str());
        console_println("API key (shown once, store it now):");
        console_println(raw.c_str());
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "key_created", nullptr,
                         nullptr, nullptr,
                         (std::string("{\"key_id\":\"") + key_id + "\"}").c_str());
        return;
    }
    if (args[0] == "list") {
        mcco::KeyStore& ks = *ctx_->keys;
        Guard g(ctx_->engine_mutex);
        if (ks.records().empty()) {
            console_println("(no keys)");
            return;
        }
        for (const auto& r : ks.records()) printKeyRecord(r);
        console_printf("%u active of %u max\n", (unsigned)ks.active_count(),
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
            console_println("no such active key");
            return;
        }
        ctx_->config->persistKeys(*ctx_->keys);
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, "key_revoked", nullptr,
                         nullptr, nullptr,
                         (std::string("{\"key_id\":\"") + args[1] + "\"}").c_str());
        console_println("key revoked");
        return;
    }
    console_println("usage: key create READ|CONTROL|ADMIN <label> | key list | key revoke <key_id>");
}

void Cli::cmdWifi(const std::vector<std::string>& args) {
    if (args.empty()) {
        console_println("usage: wifi set <ssid> <pass> | wifi status");
        return;
    }
    if (args[0] == "set" && args.size() >= 3) {
        if (!ctx_->config->saveWifi(args[1], args[2])) {
            console_println("persist failed");
            return;
        }
        ctx_->wifi->setCredentials(args[1], args[2]);
        console_println("wifi credentials saved; reconnecting");
        return;
    }
    if (args[0] == "status") {
        if (ctx_->wifi->connected()) {
            console_printf("connected ip=%s\n", WiFi.localIP().toString().c_str());
        } else {
            console_println("not connected");
        }
        return;
    }
    console_println("usage: wifi set <ssid> <pass> | wifi status");
}

void Cli::cmdNtp(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[0] == "set") {
        if (!ctx_->config->saveNtp(args[1])) {
            console_println("persist failed");
            return;
        }
        static_cast<EspClock*>(ctx_->clock)->beginSntp(args[1]);
        console_println("ntp server saved");
        return;
    }
    console_println("usage: ntp set <server>");
}

void Cli::cmdStatus() {
    const mcco::Identity& id = ctx_->config->identity();
    console_printf("hostname: %s  device_id: %s\n", id.hostname.c_str(), id.device_id.c_str());
    console_printf("wifi: %s\n", ctx_->wifi->connected() ? "connected" : "down");
    console_printf("usb hid: %s\n", ctx_->status_cache->usbUp() ? "mounted" : "not mounted");
    {
        Guard g(ctx_->engine_mutex);
        console_printf("ledger: %u commands\n", (unsigned)ctx_->ledger->command_count());
        console_printf("keys: %u active\n", (unsigned)ctx_->keys->active_count());
    }
    console_printf("sntp synced: %s\n", static_cast<EspClock*>(ctx_->clock)->synced() ? "yes" : "no");
    console_printf("log entries: %u\n", (unsigned)ctx_->log->count());
}

void Cli::cmdAdmin(const std::vector<std::string>& args) {
    if (args.empty()) {
        console_println("usage: admin set <password> | admin status");
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
            console_printf("admin password NOT set: %s\n", err.c_str());
            return;
        }
        // The password itself is never printed or logged (spec 13.1.1).
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "admin_password_set",
                         nullptr, nullptr, nullptr, nullptr);
        console_println("admin password set");
        return;
    }
    if (args[0] == "status") {
        console_printf("admin password: %s\n",
                      ctx_->web_ui->passwordSet() ? "set" : "not set");
        return;
    }
    console_println("usage: admin set <password> | admin status");
}

void Cli::cmdAgent(const std::vector<std::string>& args) {
    if (args.empty()) {
        console_println("usage: agent pair [seconds] | agent status");
        return;
    }
    if (args[0] == "pair") {
        uint32_t duration_s = mcco::PairingStore::kDefaultWindowS;
        if (args.size() >= 2) {
            char* endp = nullptr;
            unsigned long n = strtoul(args[1].c_str(), &endp, 10);
            if (!endp || *endp != '\0' || n < mcco::PairingStore::kMinWindowS ||
                n > mcco::PairingStore::kMaxWindowS) {
                console_printf("duration must be %u-%u seconds\n",
                               (unsigned)mcco::PairingStore::kMinWindowS,
                               (unsigned)mcco::PairingStore::kMaxWindowS);
                return;
            }
            duration_s = (uint32_t)n;
        }
        bool opened;
        std::string code;
        uint32_t remaining_s = 0;
        {
            Guard g(ctx_->engine_mutex);
            opened = ctx_->pairing->openWindow(duration_s);
            if (opened) {
                code = ctx_->pairing->pairingCode();
                remaining_s = ctx_->pairing->windowSecondsRemaining();
            }
        }
        if (!opened) {
            console_println("could not open pairing window");
            return;
        }
        // Opening a window does not change the pairing state, so engine mode
        // and the mDNS TXT stay as they are; the HTTP route is authoritative
        // for the ceremony itself.
        console_printf("PAIRING CODE: %s  (expires in %us)\n", code.c_str(),
                       (unsigned)remaining_s);
        return;
    }
    if (args[0] == "status") {
        mcco::PairingState state;
        std::string pairing_id, agent_instance_id;
        uint64_t created_at = 0;
        uint32_t remaining_s = 0, failed = 0;
        {
            Guard g(ctx_->engine_mutex);
            state = ctx_->pairing->state();
            if (const mcco::PairingRecord* rec = ctx_->pairing->activeRecord()) {
                pairing_id = rec->pairing_id;
                agent_instance_id = rec->agent_instance_id;
                created_at = rec->created_at;
            }
            if (state == mcco::PairingState::PairingWindow) {
                remaining_s = ctx_->pairing->windowSecondsRemaining();
                failed = ctx_->pairing->windowFailedAttempts();
            }
        }
        console_printf("pairing state: %s\n", mcco::pairing_state_to_string(state));
        if (!pairing_id.empty()) {
            console_printf("pairing_id: %s\nagent_instance_id: %s\ncreated_at: %s\n",
                           pairing_id.c_str(), agent_instance_id.c_str(),
                           mcco::iso8601_format(created_at).c_str());
        }
        if (state == mcco::PairingState::PairingWindow) {
            console_printf("window: %us remaining, %u failed attempts\n", (unsigned)remaining_s,
                           (unsigned)failed);
        }
        console_printf("agent session: %s\n",
                       ctx_->agent_link->sessionActive() ? "ACTIVE" : "inactive");
        return;
    }
    console_println("usage: agent pair [seconds] | agent status");
}

void Cli::cmdReboot() {
    console_println("rebooting...");
    delay(200);
    ESP.restart();
}
