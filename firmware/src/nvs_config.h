#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <string>
#include <vector>
#include "mc_auth.h"
#include "mc_pairing.h"
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

    // Pairing record <-> NVS (spec 3.2.2/15.1). The persisted JSON is
    // mcco::PairingStore::dump(); only the token digest is stored, never the
    // raw agent token. An open pairing window never survives reboot.
    bool hydratePairing(mcco::PairingStore& ps);
    bool persistPairing(const mcco::PairingStore& ps);

    // Last known agent boot_id (spec 8.2.1): the restart identity comparison
    // must survive an ESP32 reboot mid-window. The glue throttles writes
    // (change-only, >= 60 s apart) and hydrates at boot into the status cache
    // plus engine->set_known_boot_id().
    bool persistAgentBootId(const std::string& boot_id);
    bool loadAgentBootId(std::string& boot_id); // false: no record yet

    // Generates the factory identity: device_id from the eFuse MAC
    // (mcco::hex12), hostname "mac-" + last 6 lowercase hex of the MAC,
    // device_name "MacControl".
    static mcco::Identity factoryIdentity();

    // ---- Phase 2 records (spec ch. 10, 13.1.1) ------------------------------
    // Macro store and trigger bindings persist as JSON arrays of opaque
    // definition lines through the same double-slot CRC machinery.
    bool persistMacroLines(const std::vector<std::string>& lines);
    bool loadMacroLines(std::vector<std::string>& out); // false: no record yet
    bool persistTriggerLines(const std::vector<std::string>& lines);
    bool loadTriggerLines(std::vector<std::string>& out);

    // Admin password (spec 13.1.1): PBKDF2-HMAC-SHA256(10000, 32B) with a
    // 16-byte salt, both base64url. Only the digests ever touch flash.
    bool saveAdminPassword(const std::string& salt_b64, const std::string& hash_b64);
    bool loadAdminPassword(std::string& salt_b64, std::string& hash_b64); // false: unset

    // Unlock password (spec AMENDMENT 2026-09-30): stored PLAINTEXT — the
    // device must type it at the lock/login screen, so it cannot be hashed
    // like the admin password. Write-only via the API (no GET ever returns
    // it); it never appears in the ledger, logs, or UI readbacks. Exposure
    // = physical flash access. Empty string clears. Passwords are validated
    // printable ASCII at set time so the HID typer cannot silently skip a
    // byte.
    bool saveUnlockPassword(const std::string& password);
    bool loadUnlockPassword(std::string& password); // false: unset

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
