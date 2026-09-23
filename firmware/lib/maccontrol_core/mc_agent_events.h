#pragma once
#include <cstdint>
#include <string>
#include "mc_types.h"

namespace mcco {

// Closed event catalog (spec 6.2.1): exactly twelve types exist; the twelfth
// (FrontAppChanged) is the Phase 4.5 spec amendment (frontmost bundle ID).
enum class AgentEventType : uint8_t {
    Hello, Goodbye, Heartbeat, SystemStateChanged, UserSessionChanged,
    ScreenLockChanged, ApplicationStarted, ApplicationExited,
    CommandAck, CommandResult, CapabilityReport, FrontAppChanged
};

bool agent_event_type_from_string(const char* s, AgentEventType& out);
const char* agent_event_type_to_string(AgentEventType t);

// Parse outcome. UnknownType and SchemaViolation both count toward the
// consecutive-schema-violation threshold (spec 6.1.1); everything else about
// the connection (seq, session) is still tracked for such frames.
enum class AgentEventError : uint8_t {
    Ok,
    TooLarge,        // serialized frame over 4 KB (spec 6.1)
    MalformedJson,   // not a JSON object at all
    UnknownType,     // type missing or outside the closed enum
    SchemaViolation  // envelope or payload schema violation
};

// One validated MCA frame (spec 6.1 envelope). `payload_json` is the raw
// payload object text, validated against the per-type key table.
struct AgentEvent {
    std::string event_id;
    std::string agent_instance_id;
    std::string session_id;   // empty == JSON null
    uint64_t seq = 0;
    std::string timestamp;
    AgentEventType type = AgentEventType::Heartbeat;
    bool has_command_id = false;
    std::string command_id;
    std::string payload_json;
};

// Two-phase parse: the envelope is extracted whenever possible (so the glue
// can still apply seq/session accounting to rejected frames), and the payload
// is validated against the per-type table of spec 6.2.
AgentEventError parse_agent_event(const std::string& json, AgentEvent& out);

} // namespace mcco
