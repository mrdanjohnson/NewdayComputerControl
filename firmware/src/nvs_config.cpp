#include "nvs_config.h"
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <stdio.h>
#include <string.h>
#include "mc_ids.h"
#include "mc_iso8601.h"

namespace {

// CRC-32 (IEEE, poly 0xEDB88320), table built at first use.
uint32_t crc32_bytes(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string crcHex(uint32_t crc) {
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)crc);
    return buf;
}

std::string slotKey(const char* key, uint8_t slot) {
    return std::string(key) + ".s" + (slot ? "1" : "0");
}

std::string curKey(const char* key) { return std::string(key) + ".cur"; }

} // namespace
mcco::Identity ConfigStore::factoryIdentity() {
    mcco::Identity id;
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    id.device_id = mcco::hex12(mac);
    std::string mac_hex = mcco::hex12(mac);
    id.hostname = "mac-" + mac_hex.substr(6, 6);
    id.device_name = "MacControl";
    return id;
}

bool ConfigStore::load() {
    if (!prefs_.begin("mccfg", false)) return false;
    loaded_ = true;
    note_.clear();

    std::string tmp;
    if (!readRecord("firstboot", tmp)) {
        // First boot: generate identity once from the eFuse MAC, store defaults.
        first_boot_ = true;
        identity_ = factoryIdentity();
        wifi_ssid_.clear();
        wifi_pass_.clear();
        ntp_server_ = "pool.ntp.org";
        log_seq_ = 0;
        saveIdentity(identity_);
        writeRecord("wifi", "{}");
        writeRecord("ntp", "{\"server\":\"pool.ntp.org\"}");
        writeRecord("logseq", "{\"seq\":0}");
        writeRecord("keys", "[]");
        writeRecord("firstboot", "{\"v\":1}");
        return true;
    }

    first_boot_ = false;

    if (!readRecord("identity", tmp)) {
        identity_ = factoryIdentity();
        note_ += "identity:factory ";
    } else {
        JsonDocument doc;
        if (deserializeJson(doc, tmp) || !doc.is<JsonObject>()) {
            identity_ = factoryIdentity();
            note_ += "identity:corrupt ";
        } else {
            mcco::Identity id;
            id.device_name = doc["device_name"] | "MacControl";
            id.hostname = doc["hostname"] | "";
            id.location = doc["location"] | "";
            id.description = doc["description"] | "";
            id.device_id = doc["device_id"] | "";
            if (id.device_id.size() != 12 || id.hostname.empty() ||
                !mcco::hostname_valid(id.hostname)) {
                mcco::Identity fresh = factoryIdentity();
                if (id.device_id.size() != 12) id.device_id = fresh.device_id;
                if (id.hostname.empty() || !mcco::hostname_valid(id.hostname))
                    id.hostname = fresh.hostname;
                note_ += "identity:repaired ";
            }
            identity_ = id;
        }
    }

    wifi_ssid_.clear();
    wifi_pass_.clear();
    if (readRecord("wifi", tmp)) {
        JsonDocument doc;
        if (!deserializeJson(doc, tmp) && doc.is<JsonObject>()) {
            wifi_ssid_ = doc["ssid"] | "";
            wifi_pass_ = doc["pass"] | "";
        }
    }

    ntp_server_ = "pool.ntp.org";
    if (readRecord("ntp", tmp)) {
        JsonDocument doc;
        if (!deserializeJson(doc, tmp) && doc.is<JsonObject>()) {
            std::string s = doc["server"] | "";
            if (!s.empty()) ntp_server_ = s;
        }
    }

    log_seq_ = 0;
    if (readRecord("logseq", tmp)) {
        JsonDocument doc;
        if (!deserializeJson(doc, tmp) || !doc.is<JsonObject>()) {
            note_ += "logseq:corrupt ";
        } else {
            log_seq_ = doc["seq"] | 0;
        }
    }
    return true;
}

bool ConfigStore::readRecord(const char* key, std::string& payload) {
    payload.clear();
    uint8_t cur = prefs_.getUChar(curKey(key).c_str(), 0);
    if (cur > 1) cur = 0;
    // Committed slot first, then the previous copy (spec 15.1 fallback).
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t slot = (attempt == 0) ? cur : (cur ^ 1);
        String v = prefs_.getString(slotKey(key, slot).c_str(), "");
        if (v.length() < 9) continue;
        const char* s = v.c_str();
        uint32_t want = (uint32_t)strtoul(s, nullptr, 16);
        std::string body = s + 8;
        if (crc32_bytes(reinterpret_cast<const uint8_t*>(body.data()), body.size()) == want) {
            payload = std::move(body);
            return true;
        }
    }
    return false;
}

bool ConfigStore::writeRecord(const char* key, const std::string& payload) {
    uint8_t cur = prefs_.getUChar(curKey(key).c_str(), 0);
    if (cur > 1) cur = 0;
    uint8_t nxt = cur ^ 1;
    std::string blob = crcHex(crc32_bytes(reinterpret_cast<const uint8_t*>(payload.data()),
                                          payload.size())) +
                       payload;
    // Write to the inactive slot, verify the readback, then commit the marker.
    prefs_.putString(slotKey(key, nxt).c_str(), blob.c_str());
    String rb = prefs_.getString(slotKey(key, nxt).c_str(), "");
    if (rb != blob.c_str()) return false;
    prefs_.putUChar(curKey(key).c_str(), nxt);
    return true;
}

bool ConfigStore::saveIdentity(const mcco::Identity& id) {
    JsonDocument doc;
    doc["device_name"] = id.device_name;
    doc["hostname"] = id.hostname;
    doc["location"] = id.location;
    doc["description"] = id.description;
    doc["device_id"] = id.device_id;
    std::string out;
    serializeJson(doc, out);
    if (!writeRecord("identity", out)) return false;
    identity_ = id;
    return true;
}

bool ConfigStore::saveWifi(const std::string& ssid, const std::string& pass) {
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    std::string out;
    serializeJson(doc, out);
    if (!writeRecord("wifi", out)) return false;
    wifi_ssid_ = ssid;
    wifi_pass_ = pass;
    return true;
}

bool ConfigStore::saveNtp(const std::string& server) {
    JsonDocument doc;
    doc["server"] = server;
    std::string out;
    serializeJson(doc, out);
    if (!writeRecord("ntp", out)) return false;
    ntp_server_ = server;
    return true;
}

bool ConfigStore::setLogSeq(uint32_t seq) {
    JsonDocument doc;
    doc["seq"] = seq;
    std::string out;
    serializeJson(doc, out);
    if (!writeRecord("logseq", out)) return false;
    log_seq_ = seq;
    return true;
}

bool ConfigStore::hydrateKeys(mcco::KeyStore& ks) {
    std::string tmp;
    if (!readRecord("keys", tmp)) return false; // no persisted keys yet
    JsonDocument doc;
    if (deserializeJson(doc, tmp) || !doc.is<JsonArray>()) return false;

    std::vector<mcco::KeyRecord> recs;
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        mcco::KeyRecord r;
        r.key_id = v["key_id"] | "";
        r.label = v["label"] | "";
        mcco::Role role = mcco::Role::Read;
        if (mcco::role_from_string(v["role"] | "READ", role)) r.role = role;
        r.key_sha256 = v["key_sha256"] | "";
        r.created_at = v["created_at"] | 0ULL;
        r.last_used_at = v["last_used_at"] | 0ULL;
        r.expires_at = v["expires_at"] | 0ULL;
        r.active = v["active"] | false;
        if (r.key_id.empty() || r.key_sha256.size() != 64) continue;
        recs.push_back(std::move(r));
    }
    ks.restore(std::move(recs));
    return true;
}

bool ConfigStore::persistMacroLines(const std::vector<std::string>& lines) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const std::string& line : lines) arr.add(line);
    std::string out;
    serializeJson(doc, out);
    return writeRecord("macros", out);
}

bool ConfigStore::loadMacroLines(std::vector<std::string>& out) {
    std::string tmp;
    if (!readRecord("macros", tmp)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, tmp) || !doc.is<JsonArray>()) return false;
    out.clear();
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        if (v.is<const char*>()) out.emplace_back(v.as<const char*>());
    }
    return true;
}

bool ConfigStore::persistTriggerLines(const std::vector<std::string>& lines) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const std::string& line : lines) arr.add(line);
    std::string out;
    serializeJson(doc, out);
    return writeRecord("triggers", out);
}

bool ConfigStore::loadTriggerLines(std::vector<std::string>& out) {
    std::string tmp;
    if (!readRecord("triggers", tmp)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, tmp) || !doc.is<JsonArray>()) return false;
    out.clear();
    for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
        if (v.is<const char*>()) out.emplace_back(v.as<const char*>());
    }
    return true;
}

bool ConfigStore::saveAdminPassword(const std::string& salt_b64, const std::string& hash_b64) {
    JsonDocument doc;
    doc["salt"] = salt_b64;
    doc["hash"] = hash_b64;
    std::string out;
    serializeJson(doc, out);
    return writeRecord("adminpw", out);
}

bool ConfigStore::loadAdminPassword(std::string& salt_b64, std::string& hash_b64) {
    std::string tmp;
    if (!readRecord("adminpw", tmp)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, tmp) || !doc.is<JsonObject>()) return false;
    salt_b64 = doc["salt"] | "";
    hash_b64 = doc["hash"] | "";
    return !salt_b64.empty() && !hash_b64.empty();
}

bool ConfigStore::persistKeys(const mcco::KeyStore& ks) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const mcco::KeyRecord& rec : ks.records()) {
        JsonObject o = arr.add<JsonObject>();
        o["key_id"] = rec.key_id;
        o["label"] = rec.label;
        o["role"] = mcco::role_to_string(rec.role);
        o["key_sha256"] = rec.key_sha256;
        o["created_at"] = rec.created_at;
        o["last_used_at"] = rec.last_used_at;
        o["expires_at"] = rec.expires_at;
        o["active"] = rec.active;
    }
    std::string out;
    serializeJson(doc, out);
    return writeRecord("keys", out);
}
