#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mc_types.h"

namespace mcco {

// One stored API key record (spec 13.1.1). Only the SHA-256 digest is
// persisted; the raw key is shown exactly once at creation.
struct KeyRecord {
    std::string key_id;        // "key-01".."key-08"
    std::string label;
    Role role = Role::Read;
    std::string key_sha256;    // hex digest
    uint64_t created_at = 0;
    uint64_t last_used_at = 0;
    uint64_t expires_at = 0;   // 0 == never
    bool active = true;
};

struct Principal {
    std::string key_id;
    Role role;
};

enum class AuthResult { Ok, MissingOrInvalid, Revoked, Expired };

// Digest-matching key store. Persistence and random generation are delegated
// to the glue layer (NVS on ESP32); this class enforces the deterministic
// semantics: max 8 active keys, expired -> 401 (never downgraded), revoked ->
// 403 per the error table's "known but revoked" row.
class KeyStore {
public:
    static constexpr size_t kMaxActive = 8;

    // Adds a key; returns false if at capacity. `raw_key` is the mck_... key;
    // only its digest is stored. `expires_at` 0 == no expiry. Sets out_id to
    // the assigned key_id.
    bool add(const std::string& raw_key, Role role, const std::string& label,
             uint64_t created_at, std::string& out_id, uint64_t expires_at = 0);
    bool revoke(const std::string& key_id);

    // Replaces the store from persisted records (spec 13.1.1 persistence is
    // the glue's job; digests only). Call once at boot before authenticate().
    void restore(std::vector<KeyRecord> persisted) { keys_ = std::move(persisted); }

    // Updates last_used_at for a key (called lazily by the HTTP layer).
    // Quantized to 60 s: returns true only when the stored value actually
    // changed (never-used keys always update), so callers can skip the
    // expensive persist when nothing moved.
    bool touch(const std::string& key_id, uint64_t last_used_at);

    // Matches a presented raw key against stored digests. `now` enforces
    // expires_at: expired keys -> AuthResult::Expired (401, never downgraded).
    AuthResult authenticate(const std::string& raw_key, uint64_t now, Principal& out) const;

    const std::vector<KeyRecord>& records() const { return keys_; }
    size_t active_count() const;

private:
    static std::string digest(const std::string& raw_key);
    std::vector<KeyRecord> keys_;
};

} // namespace mcco
