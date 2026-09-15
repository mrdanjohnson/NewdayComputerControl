#include "mc_iso8601.h"
#include <cstdio>

namespace mcco {

static void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yy = int(yoe) + int(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = yy + (m <= 2 ? 1 : 0);
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - int(era) * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

std::string iso8601_format(uint64_t epoch_s) {
    int64_t days = int64_t(epoch_s) / 86400;
    uint32_t rem = uint32_t(epoch_s % 86400);
    int y;
    unsigned mo, d;
    civil_from_days(days, y, mo, d);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02u:%02u:%02uZ", y, mo, d,
             rem / 3600, (rem % 3600) / 60, rem % 60);
    return buf;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool iso8601_parse(const std::string& s, uint64_t& epoch_s) {
    // Strict: YYYY-MM-DDTHH:MM:SSZ (20 chars).
    if (s.size() != 20) return false;
    for (size_t i = 0; i < 20; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return false; }
        else if (i == 10) { if (s[i] != 'T') return false; }
        else if (i == 13 || i == 16) { if (s[i] != ':') return false; }
        else if (i == 19) { if (s[i] != 'Z') return false; }
        else if (!is_digit(s[i])) return false;
    }
    auto num = [&](size_t off, size_t len) -> unsigned {
        unsigned v = 0;
        for (size_t i = 0; i < len; i++) v = v * 10 + unsigned(s[off + i] - '0');
        return v;
    };
    const int y = int(num(0, 4));
    const unsigned mo = num(5, 2), d = num(8, 2);
    const unsigned h = num(11, 2), mi = num(14, 2), sec = num(17, 2);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return false;
    const int64_t days = days_from_civil(y, mo, d);
    if (days < 0) return false;
    epoch_s = uint64_t(days) * 86400 + h * 3600 + mi * 60 + sec;
    return true;
}

} // namespace mcco
