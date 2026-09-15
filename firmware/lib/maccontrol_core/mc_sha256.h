#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace mcco {

// Compact SHA-256 (FIPS 180-4). Shared by firmware and host tests so API-key
// digest semantics are identical on both.
class Sha256 {
public:
    Sha256();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) { update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    // Writes 32 digest bytes and resets the state for reuse.
    void final(uint8_t out[32]);

    // Convenience: hex digest of a single buffer, lowercase.
    static std::string hex_digest(const std::string& data);

private:
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_len_ = 0;
    void process(const uint8_t* block);
};

} // namespace mcco
