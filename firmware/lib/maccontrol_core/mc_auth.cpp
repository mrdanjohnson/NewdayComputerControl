#include "mc_auth.h"
#include "mc_sha256.h"

namespace mcco {

std::string KeyStore::digest(const std::string& raw_key) { return Sha256::hex_digest(raw_key); }

bool KeyStore::add(const std::string& raw_key, Role role, const std::string& label,
                   uint64_t created_at, std::string& out_id, uint64_t expires_at) {
    if (active_count() >= kMaxActive) return false;
    KeyRecord rec;
    char buf[16];
    snprintf(buf, sizeof(buf), "key-%02u", unsigned(keys_.size() + 1));
    rec.key_id = buf;
    rec.label = label;
    rec.role = role;
    rec.key_sha256 = digest(raw_key);
    rec.created_at = created_at;
    rec.expires_at = expires_at;
    rec.active = true;
    keys_.push_back(rec);
    out_id = rec.key_id;
    return true;
}

bool KeyStore::revoke(const std::string& key_id) {
    for (auto& k : keys_) {
        if (k.key_id == key_id && k.active) {
            k.active = false;
            return true;
        }
    }
    return false;
}

size_t KeyStore::active_count() const {
    size_t n = 0;
    for (const auto& k : keys_)
        if (k.active) n++;
    return n;
}

bool KeyStore::touch(const std::string& key_id, uint64_t last_used_at) {
    for (auto& k : keys_) {
        if (k.key_id == key_id) {
            // 60 s quantization: under polling load a per-request update would
            // dirty the store constantly and force a multi-KB persist every
            // throttle window for attribution data that only moves once a
            // minute anyway.
            const uint64_t diff = k.last_used_at > last_used_at ? k.last_used_at - last_used_at
                                                                 : last_used_at - k.last_used_at;
            if (k.last_used_at == 0 || diff >= 60) {
                k.last_used_at = last_used_at;
                return true;
            }
            return false;
        }
    }
    return false;
}

AuthResult KeyStore::authenticate(const std::string& raw_key, uint64_t now, Principal& out) const {
    if (raw_key.empty()) return AuthResult::MissingOrInvalid;
    const std::string d = digest(raw_key);
    for (const auto& k : keys_) {
        if (k.key_sha256 != d) continue;
        if (!k.active) return AuthResult::Revoked;
        if (k.expires_at != 0 && now >= k.expires_at) return AuthResult::Expired;
        out.key_id = k.key_id;
        out.role = k.role;
        return AuthResult::Ok;
    }
    return AuthResult::MissingOrInvalid;
}

} // namespace mcco
