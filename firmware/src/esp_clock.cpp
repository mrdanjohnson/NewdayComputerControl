#include "esp_clock.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <time.h>

// Fallback base: 2025-01-01T00:00:00Z. Used only until SNTP syncs.
static constexpr uint64_t kFallbackBase = 1735689600ULL;

void EspClock::beginSntp(const std::string& ntp_server) {
    // configTime() initializes SNTP in poll mode against the given server.
    if (ntp_server.empty()) {
        configTime(0, 0, "pool.ntp.org");
    } else {
        configTime(0, 0, ntp_server.c_str());
    }
    // Sync detection is a cheap heuristic: once time() reports a plausible
    // epoch (RTC retained or SNTP synced) the clock is treated as wall-clock.
}

uint64_t EspClock::epoch_seconds() const {
    if (synced_) {
        time_t t = time(nullptr);
        if (t > 0) return (uint64_t)t;
    }
    time_t t = time(nullptr);
    if ((uint64_t)t > kFallbackBase) {
        const_cast<EspClock*>(this)->synced_ = true;
        return (uint64_t)t;
    }
    return kFallbackBase + millis() / 1000;
}

uint64_t EspClock::millis() const { return (uint64_t)(esp_timer_get_time() / 1000); }
