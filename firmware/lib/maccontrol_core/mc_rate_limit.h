#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "mc_clock.h"
#include "mc_types.h"

namespace mcco {

// Per-key token bucket (spec 13.1.1). Defaults: READ 60, CONTROL 30,
// ADMIN 10 requests/minute. Excess requests are rejected (429), never queued.
class RateLimiter {
public:
    explicit RateLimiter(IClock& clock) : clock_(clock) {}

    static uint32_t default_per_minute(Role r) {
        switch (r) {
            case Role::Read: return 60;
            case Role::Control: return 30;
            case Role::Admin: return 10;
        }
        return 60;
    }

    // Returns true if the request is allowed; consumes one token.
    bool allow(const std::string& key_id, Role role) {
        return allow_custom(key_id, default_per_minute(role));
    }

    // Same, with an explicit bucket size (tests / configured overrides).
    bool allow_custom(const std::string& key_id, uint32_t per_minute) {
        Bucket& b = buckets_[key_id];
        const uint64_t now_ms = clock_.millis();
        if (now_ms - b.window_start_ms >= 60000) {
            b.window_start_ms = now_ms;
            b.count = 0;
        }
        if (b.count >= per_minute) return false;
        b.count++;
        return true;
    }

    void reset(const std::string& key_id) { buckets_.erase(key_id); }

private:
    struct Bucket {
        uint64_t window_start_ms = 0;
        uint32_t count = 0;
    };
    IClock& clock_;
    std::map<std::string, Bucket> buckets_;
};

} // namespace mcco
