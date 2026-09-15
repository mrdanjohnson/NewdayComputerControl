#include "mc_log.h"

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

} // namespace mcco
