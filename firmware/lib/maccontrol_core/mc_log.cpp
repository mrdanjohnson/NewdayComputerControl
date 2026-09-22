#include "mc_log.h"
#include <string.h>

namespace mcco {

const char* log_category_string(LogCategory c) {
    switch (c) {
        case LogCategory::Command: return "command";
        case LogCategory::Session: return "session";
        case LogCategory::Auth: return "auth";
        case LogCategory::Config: return "config";
        case LogCategory::Ota: return "ota";
        case LogCategory::System: return "system";
    }
    return "system";
}

const char* log_level_string(LogLevel l) {
    switch (l) {
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

bool log_category_from_string(const char* s, LogCategory& out) {
    if (!s) return false;
    for (uint8_t c = 0; c <= (uint8_t)LogCategory::System; c++) {
        const LogCategory cat = (LogCategory)c;
        if (strcmp(s, log_category_string(cat)) == 0) {
            out = cat;
            return true;
        }
    }
    return false;
}

bool log_level_from_string(const char* s, LogLevel& out) {
    if (!s) return false;
    for (uint8_t l = 0; l <= (uint8_t)LogLevel::Error; l++) {
        const LogLevel lvl = (LogLevel)l;
        if (strcmp(s, log_level_string(lvl)) == 0) {
            out = lvl;
            return true;
        }
    }
    return false;
}

} // namespace mcco
