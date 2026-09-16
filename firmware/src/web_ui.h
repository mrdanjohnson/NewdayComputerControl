#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "app_context.h"
#include "mc_mutex.h"

// ESP32 Web UI support (spec 13.1.1, ch. 14): admin-password verification
// (PBKDF2-HMAC-SHA256, 10000 iterations, 32-byte output, 16-byte salt, via
// mbedTLS), in-RAM session tokens (32 random bytes, base64url, 8 h expiry with
// sliding renewal), and the failed-login lockout (5 consecutive failures ->
// 60 s lockout per source IP, logged). The password itself is never logged
// or printed after entry.
//
// A valid session is treated as the ADMIN role for /api/v1/* calls made by
// the UI (the "Web UI is bound to the ADMIN role" rule of spec ch. 14).
class WebUi {
public:
    void begin(AppContext* ctx);

    bool passwordSet() const { return password_set_; }

    // Sets (or replaces) the admin password. Enforces the 10-character
    // minimum. On success the digest pair is persisted via ConfigStore.
    // Returns false and fills `err` (human-readable, safe to print).
    bool setPassword(const char* password, std::string& err);

    enum class LoginResult { Ok, Invalid, LockedOut };

    // Verifies a login attempt from `ip`. When no password is set yet, the
    // first login attempt IS the one-time setup: a >= 10 char password in the
    // `password` field creates it (documented decision — the endpoint ships
    // with no password and the physical console can also provision one via
    // `admin set`). On success mints a session token (8 h, sliding).
    LoginResult login(const char* password, const std::string& ip, std::string& token_out);

    // Validates a session token; sliding-renews it on success.
    bool validateSession(const std::string& token);
    void logout(const std::string& token);

    // The single-page UI document (inline CSS/JS, no external dependencies).
    static const char* pageHtml();

private:
    bool verifyPassword(const char* password);
    std::string newToken();

    struct Session {
        uint64_t expires_at_s;
    };
    struct LoginTrack {
        char ip[40] = {0};
        uint32_t consecutive_fails = 0;
        uint64_t lockout_until_ms = 0;
        bool used = false;
    };
    LoginTrack& trackFor(const char* ip);

    AppContext* ctx_ = nullptr;
    bool password_set_ = false;
    std::string salt_b64_;
    std::string hash_b64_;
    std::map<std::string, Session> sessions_;
    LoginTrack track_[8];
    Mutex mutex_;
};
