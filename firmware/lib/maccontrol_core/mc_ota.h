#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mcco {

// Signed OTA container layout (spec 15.3), shared by the device uploader
// (src/ota.cpp), the host signer (scripts/ota_sign.py) and the native tests.
// The container is the 64-byte raw ECDSA P-256 signature (r||s, each 32 bytes
// big-endian, zero-padded) concatenated with the firmware image bytes.
constexpr size_t kOtaSignatureLen = 64;
// A container must hold at least one image byte.
constexpr size_t kOtaContainerMinLen = kOtaSignatureLen + 1;

inline size_t ota_image_offset() { return kOtaSignatureLen; }
inline size_t ota_image_len(size_t container_len) {
    return container_len >= kOtaSignatureLen ? container_len - kOtaSignatureLen : 0;
}

// Container size gate: the declared Content-Length must hold a non-empty
// image and fit the inactive OTA slot (spec 15.3).
inline bool ota_container_size_valid(size_t container_len, size_t slot_size) {
    return container_len >= kOtaContainerMinLen && container_len <= slot_size;
}

// Version marker embedded in every firmware image (see src/ota.cpp and
// scripts/ota_sign.py): scanned in the freshly written slot so the upload
// response can report the TARGET version truthfully. Magic is the literal
// bytes "MCV1" (little-endian uint32 on the device).
constexpr uint32_t kOtaVersionMarkerMagic = 0x3156434D; // "MCV1"
struct OtaVersionMarker {
    uint32_t magic;
    char version[32];
};
static_assert(sizeof(OtaVersionMarker) == 36, "marker layout is part of the signer contract");

// Dotted version compare for the regression/downgrade log (spec 15.3:
// "Version regression is permitted but logged"). Parses the leading numeric
// triple of strings like "1.6.0-phase6"; a missing component is 0.
struct OtaVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    bool numeric = false; // at least one leading numeric component parsed
};

OtaVersion ota_parse_version(const char* v);
int ota_compare_version(const OtaVersion& a, const OtaVersion& b);
// True when `new_v` orders strictly before `cur_v` (both must parse).
bool ota_is_downgrade(const char* new_v, const char* cur_v);

} // namespace mcco
