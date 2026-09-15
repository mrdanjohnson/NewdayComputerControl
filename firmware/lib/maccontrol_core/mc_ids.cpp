#include "mc_ids.h"

namespace mcco {

// Crockford Base32 alphabet: digits 0-9 then A-Z without I, L, O, U.
static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string make_command_id(IRandom& rng) {
    uint8_t b[5];
    rng.bytes(b, sizeof(b));
    std::string out;
    out.reserve(8);
    uint64_t v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | b[i];
    for (int i = 0; i < 8; i++) {
        out += CROCKFORD[v & 31];
        v >>= 5;
    }
    return out;
}

std::string make_request_id(IRandom& rng) {
    uint8_t b[7];
    rng.bytes(b, sizeof(b));
    std::string out = "req_";
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | b[i];
    for (int i = 0; i < 9; i++) {
        out += CROCKFORD[v & 31];
        v >>= 5;
        if (v == 0 && i >= 5) break;
    }
    // Pad deterministically to 10 chars from the 7th byte if short.
    while (out.size() < 14) out += CROCKFORD[b[6] & 31];
    out.resize(14); // "req_" + 10
    return out;
}

std::string hex12(const uint8_t mac[6]) {
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(12);
    for (int i = 0; i < 6; i++) {
        out += hexd[mac[i] >> 4];
        out += hexd[mac[i] & 0xf];
    }
    return out;
}

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += B64URL[(v >> 18) & 63];
        out += B64URL[(v >> 12) & 63];
        out += B64URL[(v >> 6) & 63];
        out += B64URL[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        out += B64URL[(v >> 18) & 63];
        out += B64URL[(v >> 12) & 63];
        if (i + 1 < len) out += B64URL[(v >> 6) & 63];
    }
    return out;
}

static int b64url_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

bool base64url_decode(const std::string& in, uint8_t* out, size_t& out_len) {
    if (in.size() % 4 == 1) return false;
    out_len = 0;
    size_t i = 0;
    while (i < in.size()) {
        int vals[4] = {0, 0, 0, 0};
        size_t n = 0;
        for (; n < 4 && i < in.size(); n++, i++) {
            int v = b64url_val(in[i]);
            if (v < 0) return false;
            vals[n] = v;
        }
        if (n < 2) return false;
        uint32_t v = (uint32_t(vals[0]) << 18) | (uint32_t(vals[1]) << 12) |
                     (uint32_t(vals[2]) << 6) | uint32_t(vals[3]);
        out[out_len++] = uint8_t(v >> 16);
        if (n > 2) out[out_len++] = uint8_t(v >> 8);
        if (n > 3) out[out_len++] = uint8_t(v);
    }
    return true;
}

std::string make_api_key(IRandom& rng) {
    uint8_t b[32];
    rng.bytes(b, sizeof(b));
    return std::string("mck_") + base64url_encode(b, sizeof(b));
}

} // namespace mcco
