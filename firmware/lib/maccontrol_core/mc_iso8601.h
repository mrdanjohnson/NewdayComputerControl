#pragma once
#include <cstdint>
#include <string>

namespace mcco {

// Epoch seconds <-> "YYYY-MM-DDTHH:MM:SSZ" (UTC, no fractional seconds).
// Self-contained civil-time conversion so behavior is identical on host and
// firmware (no gmtime dependency).
std::string iso8601_format(uint64_t epoch_s);
// Returns false on any deviation from the strict format above.
bool iso8601_parse(const std::string& s, uint64_t& epoch_s);

} // namespace mcco
