#pragma once
#include <cstdint>
#include <cstddef>

namespace mcco {

// Randomness source, injected for host testing (ESP32 glue: esp_random()).
class IRandom {
public:
    virtual ~IRandom() = default;
    virtual void bytes(uint8_t* out, size_t len) = 0;
};

} // namespace mcco
