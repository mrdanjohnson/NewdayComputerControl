#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <string>
#include <vector>
#include "mc_auth.h"
#include "mc_status.h"

// Persistent configuration on ESP32 NVS (spec 15.1): double-slot, CRC32
// checked, atomic write-then-commit. Each logical record exists as two slots
// ("<key>.s0"/"<key>.s1") holding "<crc32-hex><payload>"; a commit marker
// ("<key>.cur") names the last fully written slot. A write lands the payload
// in the other slot, verifies the readback, then flips the marker. On load,
// the committed slot is tried first and the previous copy is the fallback;
// both invalid -> factory default for that record (noted for logging).
//
// Records: identity (mcco::Identity as JSON), WiFi credentials, API key
// records (mcco::KeyRecord vector as JSON, digests only — never raw keys),
// NTP server, log sequence counter, first-boot flag.
class ConfigStore {
public:
    // Loads all records; generates the factory identity on first boot.
    // Returns false only if NVS itself cannot be opened.
    bool load();

    bool loaded() const { return loaded_; }
    bool firstBoot() const { return first_boot_; }
    // Human-readable note about factory fallbacks taken during load ("" if none).
    const std::string& fallbackNote() const { return note_; }

    const mcco::Identity& identity() const { return identity_; }
    bool saveIdentity(const mcco::Identity& id);

    const std::string& wifiSsid() const { return wifi_ssid_; }
    const std::string& wifiPass() const { return wifi_pass_; }
    bool saveWifi(const std::string& ssid, const std::string& pass);

    const std::string& ntpServer() const { return ntp_server_; }
    bool saveNtp(const std::string& server);

    uint32_t logSeq() const { return log_seq_; }
    bool setLogSeq(uint32_t seq);

    // KeyStore <-> NVS. hydrateKeys restores records (digests) into the store
    // via mcco::KeyStore::restore; persistKeys serializes the current records
    // back.
    bool hydrateKeys(mcco::KeyStore& ks);
    bool persistKeys(const mcco::KeyStore& ks);

    // Generates the factory identity: device_id from the eFuse MAC
    // (mcco::hex12), hostname "mac-" + last 6 lowercase hex of the MAC,
    // device_name "MacControl".
    static mcco::Identity factoryIdentity();

private:
    bool readRecord(const char* key, std::string& payload);
    bool writeRecord(const char* key, const std::string& payload);

    Preferences prefs_;
    bool loaded_ = false;
    bool first_boot_ = false;
    std::string note_;
    mcco::Identity identity_;
    std::string wifi_ssid_;
    std::string wifi_pass_;
    std::string ntp_server_ = "pool.ntp.org";
    uint32_t log_seq_ = 0;
};
