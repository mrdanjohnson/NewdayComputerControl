#pragma once
#include <string>
#include "mc_clock.h"

// mcco::IClock on ESP32-S3: epoch from the SNTP-synced RTC via configTime
// (UTC; NTP server from NVS config, default pool.ntp.org), millis from
// esp_timer_get_time(). While SNTP has never synced, epoch_seconds() returns a
// strictly monotonic value counted from a fixed base (2025-01-01) plus uptime
// — accepted per Phase 1 brief; all recorded times are internally consistent
// but not wall-clock until the first sync.
class EspClock : public mcco::IClock {
public:
    void beginSntp(const std::string& ntp_server);
    uint64_t epoch_seconds() const override;
    uint64_t millis() const override;
    bool synced() const { return synced_; }

private:
    volatile bool synced_ = false;
};
