#include "web_ui.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <string.h>
#include "esp_clock.h"
#include "esp_rng.h"
#include "log_sink.h"
#include "mc_ids.h"
#include "mc_log.h"
#include "nvs_config.h"
#include "web_ui_page.h"

namespace {

constexpr size_t kSaltLen = 16;
constexpr size_t kHashLen = 32;
constexpr uint32_t kPbdkf2Iters = 10000; // spec 13.1.1
constexpr uint64_t kSessionTtlS = 8 * 3600;
constexpr uint32_t kMaxFails = 5;
constexpr uint32_t kLockoutMs = 60000;

bool pbkdf2(const char* password, const uint8_t* salt, size_t salt_len, uint8_t* out) {
    // mbedTLS 2.x API (IDF 4.4 / Arduino core 2.x): an HMAC md context.
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = false;
    if (info && mbedtls_md_setup(&ctx, info, 1) == 0) {
        ok = mbedtls_pkcs5_pbkdf2_hmac(&ctx, reinterpret_cast<const uint8_t*>(password),
                                       strlen(password), salt, salt_len, kPbdkf2Iters, kHashLen,
                                       out) == 0;
    }
    mbedtls_md_free(&ctx);
    return ok;
}

} // namespace

void WebUi::begin(AppContext* ctx) {
    ctx_ = ctx;
    Guard g(mutex_);
    password_set_ = ctx_->config->loadAdminPassword(salt_b64_, hash_b64_);
}

WebUi::LoginTrack& WebUi::trackFor(const char* ip) {
    for (auto& t : track_) {
        if (t.used && strcmp(t.ip, ip) == 0) return t;
    }
    for (auto& t : track_) {
        if (!t.used) {
            t.used = true;
            strncpy(t.ip, ip, sizeof(t.ip) - 1);
            t.ip[sizeof(t.ip) - 1] = '\0';
            return t;
        }
    }
    return track_[0];
}

bool WebUi::setPassword(const char* password, std::string& err) {
    if (!password || strlen(password) < 10) {
        err = "password must be at least 10 characters";
        return false;
    }
    uint8_t salt[kSaltLen];
    uint8_t hash[kHashLen];
    ctx_->rng->bytes(salt, sizeof(salt));
    if (!pbkdf2(password, salt, sizeof(salt), hash)) {
        err = "hash failed";
        return false;
    }
    const std::string salt_b64 = mcco::base64url_encode(salt, sizeof(salt));
    const std::string hash_b64 = mcco::base64url_encode(hash, sizeof(hash));
    if (!ctx_->config->saveAdminPassword(salt_b64, hash_b64)) {
        err = "persist failed";
        return false;
    }
    Guard g(mutex_);
    salt_b64_ = salt_b64;
    hash_b64_ = hash_b64;
    password_set_ = true;
    // Any existing sessions stay valid; the lockout counters are untouched.
    return true;
}

bool WebUi::verifyPassword(const char* password) {
    uint8_t salt[kSaltLen];
    uint8_t hash[kHashLen];
    size_t salt_len = 0, hash_len = 0;
    std::string salt_b64, hash_b64;
    {
        Guard g(mutex_);
        if (!password_set_) return false;
        salt_b64 = salt_b64_;
        hash_b64 = hash_b64_;
    }
    if (!mcco::base64url_decode(salt_b64, salt, salt_len) || salt_len != kSaltLen)
        return false;
    if (!pbkdf2(password, salt, salt_len, hash)) return false;
    uint8_t stored[kHashLen];
    if (!mcco::base64url_decode(hash_b64, stored, hash_len) || hash_len != kHashLen)
        return false;
    return memcmp(hash, stored, kHashLen) == 0;
}

std::string WebUi::newToken() {
    uint8_t tok[32];
    ctx_->rng->bytes(tok, sizeof(tok));
    return mcco::base64url_encode(tok, sizeof(tok));
}

WebUi::LoginResult WebUi::login(const char* password, const std::string& ip,
                                std::string& token_out) {
    if (!password) password = "";
    auto recordFail = [&](const char* event) {
        Guard g(mutex_);
        LoginTrack& t = trackFor(ip.c_str());
        ++t.consecutive_fails;
        const char* ev = event;
        if (t.consecutive_fails >= kMaxFails) {
            t.lockout_until_ms = ctx_->clock->millis() + kLockoutMs;
            t.consecutive_fails = 0;
            ev = "ui_login_lockout";
        }
        ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Warn, ev, nullptr, nullptr,
                         (std::string("ip:") + ip).c_str(), nullptr);
    };

    {
        Guard g(mutex_);
        LoginTrack& t = trackFor(ip.c_str());
        const uint64_t now_ms = ctx_->clock->millis();
        if (t.lockout_until_ms != 0 && now_ms < t.lockout_until_ms) return LoginResult::LockedOut;
        if (t.lockout_until_ms != 0) {
            t.lockout_until_ms = 0;
            t.consecutive_fails = 0;
        }
    }
    bool need_setup;
    {
        Guard g(mutex_);
        need_setup = !password_set_;
    }
    if (need_setup) {
        // One-time setup: the first login creates the admin password
        // (documented decision; the serial console offers `admin set`).
        std::string err;
        if (!setPassword(password, err)) {
            recordFail("ui_login_failed");
            return LoginResult::Invalid;
        }
    }
    if (!verifyPassword(password)) {
        recordFail("ui_login_failed");
        return LoginResult::Invalid;
    }
    {
        Guard g(mutex_);
        LoginTrack& t = trackFor(ip.c_str());
        if (t.consecutive_fails >= kMaxFails) {
            // verify succeeded but the counter tripped concurrently: treat as
            // locked out (unreachable with the single HTTP task, defensive).
            return LoginResult::LockedOut;
        }
        t.consecutive_fails = 0;
        token_out = newToken();
        sessions_[token_out] = {ctx_->clock->epoch_seconds() + kSessionTtlS};
    }
    ctx_->log->write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "ui_login", nullptr, nullptr,
                     (std::string("ip:") + ip).c_str(), nullptr);
    return LoginResult::Ok;
}

bool WebUi::validateSession(const std::string& token) {
    if (token.empty()) return false;
    Guard g(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    const uint64_t now = ctx_->clock->epoch_seconds();
    if (it->second.expires_at_s < now) {
        sessions_.erase(it);
        return false;
    }
    it->second.expires_at_s = now + kSessionTtlS; // sliding renewal
    return true;
}

void WebUi::logout(const std::string& token) {
    Guard g(mutex_);
    sessions_.erase(token);
}

const char* WebUi::pageHtml() { return kWebUiPage; }
