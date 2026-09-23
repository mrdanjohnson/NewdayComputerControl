#include "mc_sha1.h"
#include <cstring>

namespace mcco {

static inline uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

Sha1::Sha1() {
    h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE;
    h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
}

void Sha1::update(const uint8_t* data, size_t len) {
    total_len_ += len;
    while (len > 0) {
        size_t take = 64 - buf_len_;
        if (take > len) take = len;
        memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ == 64) {
            process(buf_);
            buf_len_ = 0;
        }
    }
}

void Sha1::process(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::final(uint8_t out[20]) {
    uint64_t bit_len = total_len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = uint8_t(bit_len >> (56 - i * 8));
    update(len_be, 8); // buf_len_ hits 64 and processes the final block(s)
    for (int i = 0; i < 5; i++) {
        out[i * 4] = uint8_t(h_[i] >> 24);
        out[i * 4 + 1] = uint8_t(h_[i] >> 16);
        out[i * 4 + 2] = uint8_t(h_[i] >> 8);
        out[i * 4 + 3] = uint8_t(h_[i]);
    }
    *this = Sha1();
}

} // namespace mcco
