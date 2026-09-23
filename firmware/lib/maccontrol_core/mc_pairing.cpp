#include "mc_pairing.h"
#include "mc_ids.h"
#include "mc_sha256.h"
#include <ArduinoJson.h>

namespace mcco {

const char* pairing_state_to_string(PairingState s) {
    switch (s) {
        case PairingState::Unpaired: return "unpaired";
        case PairingState::PairingWindow: return "pairing_window";
        case PairingState::Active: return "active";
        case PairingState::Revoked: return "revoked";
    }
    return "unpaired";
}

bool pairing_state_from_string(const char* s, PairingState& out) {
    const std::string v(s ? s : "");
    if (v == "unpaired") out = PairingState::Unpaired;
    else if (v == "pairing_window") out = PairingState::PairingWindow;
    else if (v == "active") out = PairingState::Active;
    else if (v == "revoked") out = PairingState::Revoked;
    else return false;
    return true;
}

PairingStore::PairingStore(IRandom& rng, IClock& clock, std::string device_id)
    : rng_(rng), clock_(clock), device_id_(std::move(device_id)) {}

void PairingStore::expireWindowIfNeeded() {
    if (state_ != PairingState::PairingWindow) return;
    if (clock_.millis() >= window_opened_ms_ + uint64_t(window_duration_s_) * 1000) {
        state_ = PairingState::Unpaired;
        code_.clear();
    }
}

void PairingStore::purgeRevokedIfNeeded() {
    if (state_ != PairingState::Revoked) return;
    if (clock_.epoch_seconds() >= revoked_at_ + kRevokedRetentionS) {
        state_ = PairingState::Unpaired;
        record_ = PairingRecord{};
        revoked_at_ = 0;
    }
}

PairingState PairingStore::state() const {
    // Lazy expiry keeps the lifecycle deterministic without a timer task.
    const_cast<PairingStore*>(this)->expireWindowIfNeeded();
    const_cast<PairingStore*>(this)->purgeRevokedIfNeeded();
    return state_;
}

bool PairingStore::openWindow(uint32_t duration_s) {
    if (duration_s < kMinWindowS || duration_s > kMaxWindowS) return false;
    state_ = PairingState::PairingWindow;
    code_ = make_pairing_code(rng_);
    window_duration_s_ = duration_s;
    window_opened_ms_ = clock_.millis();
    last_attempt_ms_ = 0;
    has_attempted_ = false;
    failed_ = 0;
    return true;
}

void PairingStore::closeWindow() {
    if (state_ == PairingState::PairingWindow) state_ = PairingState::Unpaired;
    code_.clear();
}

bool PairingStore::windowOpen() const { return state() == PairingState::PairingWindow; }

uint32_t PairingStore::windowSecondsRemaining() const {
    if (state() != PairingState::PairingWindow) return 0;
    uint64_t elapsed_ms = clock_.millis() - window_opened_ms_;
    if (elapsed_ms >= uint64_t(window_duration_s_) * 1000) return 0;
    return uint32_t((uint64_t(window_duration_s_) * 1000 - elapsed_ms + 999) / 1000);
}

PairCodeResult PairingStore::validateCode(const std::string& code) {
    if (state() != PairingState::PairingWindow) return PairCodeResult::NoWindow;
    // One validation attempt per second is accepted (spec 3.2.1); extra
    // attempts in the same second are refused without counting as failures.
    // Monotonic clock: an SNTP jump must not throttle legitimate attempts.
    const uint64_t now_ms = clock_.millis();
    if (has_attempted_ && now_ms - last_attempt_ms_ < kAttemptIntervalS * 1000)
        return PairCodeResult::RateLimited;
    has_attempted_ = true;
    last_attempt_ms_ = now_ms;
    if (code != code_) {
        if (++failed_ >= kMaxFailedAttempts) closeWindow(); // 5 bad codes close
        return PairCodeResult::WrongCode;
    }
    return PairCodeResult::Ok;
}

std::string PairingStore::completePairing(const std::string& agent_instance_id) {
    if (state() != PairingState::PairingWindow) return {};
    const uint64_t now = clock_.epoch_seconds();
    const std::string token = make_pairing_token(rng_);
    uint8_t tok_raw[64];
    size_t tok_len = 0;
    if (!base64url_decode(token, tok_raw, tok_len)) return {}; // cannot happen
    PairingRecord rec;
    rec.pairing_id = make_pairing_id(rng_);
    rec.device_id = device_id_;
    rec.agent_instance_id = agent_instance_id;
    rec.token_sha256 =
        Sha256::hex_digest(std::string(reinterpret_cast<char*>(tok_raw), tok_len));
    rec.created_at = now;
    rec.last_used_at = now;
    record_ = rec;
    state_ = PairingState::Active; // implicit incumbent revocation (exactly-one)
    code_.clear();
    return token;
}

bool PairingStore::revoke() {
    if (state() != PairingState::Active) return false;
    state_ = PairingState::Revoked;
    revoked_at_ = clock_.epoch_seconds();
    return true;
}

bool PairingStore::tokenMatches(const std::string& raw_token_b64url) const {
    if (state() != PairingState::Active) return false;
    return tokenDigestKnown(raw_token_b64url);
}

bool PairingStore::tokenDigestKnown(const std::string& raw_token_b64url) const {
    if (record_.token_sha256.empty()) return false;
    uint8_t tok[64];
    size_t tok_len = 0;
    if (!base64url_decode(raw_token_b64url, tok, tok_len)) return false;
    return Sha256::hex_digest(std::string(reinterpret_cast<char*>(tok), tok_len)) ==
           record_.token_sha256;
}

bool PairingStore::instanceMatches(const std::string& agent_instance_id) const {
    return state() == PairingState::Active &&
           record_.agent_instance_id == agent_instance_id;
}

void PairingStore::touchLastUsed() { record_.last_used_at = clock_.epoch_seconds(); }

const PairingRecord* PairingStore::activeRecord() const {
    return state() == PairingState::Active ? &record_ : nullptr;
}

std::string PairingStore::dump() const {
    JsonDocument doc;
    doc["state"] = pairing_state_to_string(state_);
    if (state_ == PairingState::Active || state_ == PairingState::Revoked) {
        JsonObject r = doc["record"].to<JsonObject>();
        r["pairing_id"] = record_.pairing_id;
        r["device_id"] = record_.device_id;
        r["agent_instance_id"] = record_.agent_instance_id;
        r["token_sha256"] = record_.token_sha256;
        r["created_at"] = record_.created_at;
        r["last_used_at"] = record_.last_used_at;
    }
    if (state_ == PairingState::Revoked) doc["revoked_at"] = revoked_at_;
    std::string out;
    serializeJson(doc, out);
    return out;
}

bool PairingStore::restore(const std::string& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    if (!doc["state"].is<const char*>()) return false;
    PairingState s;
    if (!pairing_state_from_string(doc["state"].as<const char*>(), s)) return false;
    if (s == PairingState::Active || s == PairingState::Revoked) {
        JsonObjectConst r = doc["record"].as<JsonObjectConst>();
        if (r.isNull()) return false;
        for (const char* k :
             {"pairing_id", "device_id", "agent_instance_id", "token_sha256"}) {
            if (!r[k].is<const char*>()) return false;
        }
        record_.pairing_id = r["pairing_id"].as<std::string>();
        record_.device_id = r["device_id"].as<std::string>();
        record_.agent_instance_id = r["agent_instance_id"].as<std::string>();
        record_.token_sha256 = r["token_sha256"].as<std::string>();
        record_.created_at = r["created_at"] | 0;
        record_.last_used_at = r["last_used_at"] | 0;
    }
    if (s == PairingState::Revoked) {
        revoked_at_ = doc["revoked_at"] | 0;
        if (revoked_at_ == 0) return false;
    }
    // A stored window never survives reboot: the code is displayed once.
    state_ = (s == PairingState::PairingWindow) ? PairingState::Unpaired : s;
    return true;
}

} // namespace mcco
