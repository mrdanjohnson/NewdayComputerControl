#pragma once
#include <cstdint>

namespace mcco {

// Source of wall-clock time, injected for host testing (spec: all observed_at
// values derive from the ESP32 clock; SNTP-backed in the firmware glue).
class IClock {
public:
    virtual ~IClock() = default;
    virtual uint64_t epoch_seconds() const = 0;
    virtual uint64_t millis() const = 0;
};

} // namespace mcco
