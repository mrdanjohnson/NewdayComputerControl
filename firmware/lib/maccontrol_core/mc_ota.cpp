#include "mc_ota.h"
#include <cstdlib>
#include <cstring>

namespace mcco {

OtaVersion ota_parse_version(const char* v) {
    OtaVersion out;
    if (!v) return out;
    // Skip nothing: the version must start numerically; a leading 'v' is
    // tolerated ("v1.6.0" -> 1.6.0).
    if (*v == 'v' || *v == 'V') ++v;
    const char* p = v;
    uint32_t* parts[3] = {&out.major, &out.minor, &out.patch};
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') break;
        char* endp = nullptr;
        const unsigned long n = strtoul(p, &endp, 10);
        *parts[i] = (uint32_t)n;
        out.numeric = true;
        p = endp;
        if (*p == '.') {
            ++p;
            continue;
        }
        break;
    }
    return out;
}

int ota_compare_version(const OtaVersion& a, const OtaVersion& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool ota_is_downgrade(const char* new_v, const char* cur_v) {
    const OtaVersion a = ota_parse_version(new_v);
    const OtaVersion b = ota_parse_version(cur_v);
    if (!a.numeric || !b.numeric) return false; // unknown ordering: not logged
    return ota_compare_version(a, b) < 0;
}

} // namespace mcco
