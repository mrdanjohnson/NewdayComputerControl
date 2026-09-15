#pragma once
#include <cstdint>
#include <string>

namespace mcco {

// Log categories and levels (spec 15.2, closed enums).
enum class LogCategory : uint8_t { Command, Session, Auth, Config, Ota, System };
enum class LogLevel : uint8_t { Info, Warn, Error };

const char* log_category_string(LogCategory c);
const char* log_level_string(LogLevel l);

class ILog {
public:
    virtual ~ILog() = default;
    virtual void write(LogCategory cat, LogLevel level, const char* event,
                       const char* command_id, const char* request_id,
                       const char* actor, const char* detail_json) = 0;
};

// No-op sink for minimal builds/tests.
class NullLog : public ILog {
public:
    void write(LogCategory, LogLevel, const char*, const char*, const char*,
               const char*, const char*) override {}
};

} // namespace mcco
