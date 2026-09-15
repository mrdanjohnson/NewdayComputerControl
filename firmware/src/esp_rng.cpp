#include "esp_rng.h"
#include <esp_random.h>

void EspRandom::bytes(uint8_t* out, size_t len) { esp_fill_random(out, len); }
