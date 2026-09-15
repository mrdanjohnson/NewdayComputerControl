#pragma once
#include <cstddef>
#include <cstdint>
#include "mc_random.h"

// mcco::IRandom via the ESP32 hardware RNG (esp_fill_random).
class EspRandom : public mcco::IRandom {
public:
    void bytes(uint8_t* out, size_t len) override;
};
