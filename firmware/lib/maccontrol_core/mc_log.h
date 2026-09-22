#pragma once
#include <cstdint>
#include <string>

namespace mcco {

// Log categories and levels (spec 15.2, closed enums).
enum class LogCategory : uint8_t { Command, Session, Auth, Config, Ota, System };
enum class LogLevel : uint8_t { Info, Warn, Error };

const char* log_category_string(LogCategory c);
const char* log_level_string(LogLevel l);
bool log_category_from_string(const char* s, LogCategory& out);
bool log_level_from_string(const char* s, LogLevel& out);

// Number of log entries no longer retained between a `since_seq` cursor and
// the oldest retained entry (spec 15.2 `dropped` semantics): a query for
// seq > since_seq can no longer deliver seqs since_seq+1 .. oldest_seq-1
// once the ring has wrapped past them. 0 when nothing was lost.
inline uint32_t log_dropped_count(uint32_t since_seq, uint32_t oldest_seq) {
    return (oldest_seq > 0 && since_seq + 1 < oldest_seq) ? oldest_seq - since_seq - 1 : 0;
}

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
