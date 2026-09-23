#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace mcco {

// Compact SHA-1 (FIPS 180-1), used only for the WebSocket handshake accept
// key (RFC 6455). Self-contained like mc_sha256 so firmware and host tests
// share identical semantics.
class Sha1 {
public:
    Sha1();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) { update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    // Writes 20 digest bytes and resets the state for reuse.
    void final(uint8_t out[20]);

private:
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_len_ = 0;
    void process(const uint8_t* block);
};

} // namespace mcco
