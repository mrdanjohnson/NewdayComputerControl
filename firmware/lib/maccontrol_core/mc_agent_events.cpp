#include "mc_agent_events.h"
#include <ArduinoJson.h>
#include <cstring>

namespace mcco {

bool agent_event_type_from_string(const char* s, AgentEventType& out) {
    const std::string v(s ? s : "");
    if (v == "agent_hello") out = AgentEventType::Hello;
    else if (v == "agent_goodbye") out = AgentEventType::Goodbye;
    else if (v == "heartbeat") out = AgentEventType::Heartbeat;
    else if (v == "system_state_changed") out = AgentEventType::SystemStateChanged;
    else if (v == "user_session_changed") out = AgentEventType::UserSessionChanged;
    else if (v == "screen_lock_changed") out = AgentEventType::ScreenLockChanged;
    else if (v == "application_started") out = AgentEventType::ApplicationStarted;
    else if (v == "application_exited") out = AgentEventType::ApplicationExited;
    else if (v == "command_ack") out = AgentEventType::CommandAck;
    else if (v == "command_result") out = AgentEventType::CommandResult;
    else if (v == "capability_report") out = AgentEventType::CapabilityReport;
    else if (v == "front_app_changed") out = AgentEventType::FrontAppChanged;
    else return false;
    return true;
}

const char* agent_event_type_to_string(AgentEventType t) {
    switch (t) {
        case AgentEventType::Hello: return "agent_hello";
        case AgentEventType::Goodbye: return "agent_goodbye";
        case AgentEventType::Heartbeat: return "heartbeat";
        case AgentEventType::SystemStateChanged: return "system_state_changed";
        case AgentEventType::UserSessionChanged: return "user_session_changed";
        case AgentEventType::ScreenLockChanged: return "screen_lock_changed";
        case AgentEventType::ApplicationStarted: return "application_started";
        case AgentEventType::ApplicationExited: return "application_exited";
        case AgentEventType::CommandAck: return "command_ack";
        case AgentEventType::CommandResult: return "command_result";
        case AgentEventType::CapabilityReport: return "capability_report";
        case AgentEventType::FrontAppChanged: return "front_app_changed";
    }
    return "heartbeat";
}

static bool str_in(const char* v, std::initializer_list<const char*> opts) {
    for (const char* o : opts) if (strcmp(v, o) == 0) return true;
    return false;
}

// Unknown keys in the envelope or payload are schema violations (spec 6.1.1).
// Checks the payload carries exactly `required` keys and nothing else.
// (Key-iteration rather than operator[] because a present-but-null value
// must still count as the key being present.)
static bool payload_keys_exact(JsonObjectConst p, std::initializer_list<const char*> required) {
    if (p.size() != required.size()) return false;
    for (JsonPairConst kv : p) {
        bool found = false;
        for (const char* k : required) {
            if (strcmp(kv.key().c_str(), k) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

static AgentEventError validate_payload(AgentEventType t, JsonObjectConst p) {
    switch (t) {
        case AgentEventType::Hello:
            if (!payload_keys_exact(p, {"protocol_version", "agent_version", "boot_id", "hostname"}))
                return AgentEventError::SchemaViolation;
            if (!p["protocol_version"].is<int>() || !p["agent_version"].is<const char*>() ||
                !p["boot_id"].is<const char*>() || !p["hostname"].is<const char*>())
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::Goodbye:
            if (!payload_keys_exact(p, {"reason"}) || !p["reason"].is<const char*>() ||
                !str_in(p["reason"].as<const char*>(),
                        {"shutdown", "restart", "sleep", "user_logout", "agent_stop"}))
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::Heartbeat:
            // Phase 4.5 composite telemetry (spec 6.3): load samples are
            // null-or-number, disk may exceed int32 (long long on the wire).
            if (!payload_keys_exact(p, {"boot_id", "uptime_s", "mac_uptime_s", "boot_time",
                                        "cpu_utilization_pct", "memory_utilization_pct",
                                        "disk_free_bytes", "network"}) ||
                !p["boot_id"].is<const char*>() || !p["uptime_s"].is<int>() ||
                !p["mac_uptime_s"].is<int>() || !p["boot_time"].is<int>())
                return AgentEventError::SchemaViolation;
            for (const char* k : {"cpu_utilization_pct", "memory_utilization_pct"}) {
                JsonVariantConst v = p[k];
                if (!v.isNull() && !v.is<int>() && !v.is<double>())
                    return AgentEventError::SchemaViolation;
            }
            if (!p["disk_free_bytes"].isNull() && !p["disk_free_bytes"].is<int>() &&
                !p["disk_free_bytes"].is<long long>())
                return AgentEventError::SchemaViolation;
            {
                JsonObjectConst n = p["network"].as<JsonObjectConst>();
                if (n.isNull() || !payload_keys_exact(n, {"reachable", "ip"}) ||
                    !n["reachable"].is<bool>())
                    return AgentEventError::SchemaViolation;
                if (!n["ip"].isNull() && !n["ip"].is<const char*>())
                    return AgentEventError::SchemaViolation;
            }
            return AgentEventError::Ok;
        case AgentEventType::FrontAppChanged:
            // Spec amendment (Phase 4.5, scope B1): frontmost bundle ID only.
            if (!payload_keys_exact(p, {"bundle_id"}) ||
                (!p["bundle_id"].isNull() && !p["bundle_id"].is<const char*>()))
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::SystemStateChanged:
            if (!payload_keys_exact(p, {"state"}) || !p["state"].is<const char*>() ||
                !str_in(p["state"].as<const char*>(),
                        {"awake", "sleeping", "waking", "shutting_down", "restarting", "booting"}))
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::UserSessionChanged:
            if (!payload_keys_exact(p, {"user_logged_in", "user"}) ||
                !p["user_logged_in"].is<bool>())
                return AgentEventError::SchemaViolation;
            if (!p["user"].is<const char*>() && !p["user"].isNull()) return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::ScreenLockChanged:
            if (!payload_keys_exact(p, {"locked"}) || !p["locked"].is<bool>())
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::ApplicationStarted:
            if (!payload_keys_exact(p, {"bundle_id", "pid"}) || !p["bundle_id"].is<const char*>() ||
                !p["pid"].is<int>())
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::ApplicationExited:
            if (!payload_keys_exact(p, {"bundle_id", "pid", "reason"}) ||
                !p["bundle_id"].is<const char*>() || !p["pid"].is<int>() ||
                !p["reason"].is<const char*>() ||
                !str_in(p["reason"].as<const char*>(), {"quit", "crashed", "requested_by_agent"}))
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::CommandAck:
            if (!payload_keys_exact(p, {"command_id", "action"}) ||
                !p["command_id"].is<const char*>() ||
                !str_in(p["action"].as<const char*>(), {"launch_app", "quit_app"}))
                return AgentEventError::SchemaViolation;
            return AgentEventError::Ok;
        case AgentEventType::CommandResult: {
            if (!payload_keys_exact(p, {"command_id", "outcome", "error_code"}) ||
                !p["command_id"].is<const char*>() ||
                !str_in(p["outcome"].as<const char*>(), {"ok", "failed"}))
                return AgentEventError::SchemaViolation;
            // error_code: null or one of the closed enum; `failed` requires a
            // concrete code (spec 6.2 command_result row).
            bool ok_outcome = strcmp(p["outcome"].as<const char*>(), "ok") == 0;
            if (p["error_code"].isNull()) {
                if (!ok_outcome) return AgentEventError::SchemaViolation;
            } else {
                if (!p["error_code"].is<const char*>() ||
                    !str_in(p["error_code"].as<const char*>(),
                            {"app_not_allowlisted", "app_not_running", "launch_failed",
                             "quit_failed", "command_disabled", "action_timeout"}))
                    return AgentEventError::SchemaViolation;
            }
            return AgentEventError::Ok;
        }
        case AgentEventType::CapabilityReport: {
            if (!payload_keys_exact(p, {"agent_version", "protocol_version", "os_version",
                                        "hardware_model", "enabled_commands",
                                        "allowlisted_apps"}) ||
                !p["agent_version"].is<const char*>() || !p["protocol_version"].is<int>() ||
                !p["os_version"].is<const char*>() || !p["hardware_model"].is<const char*>() ||
                !p["enabled_commands"].is<JsonArrayConst>() ||
                !p["allowlisted_apps"].is<JsonArrayConst>())
                return AgentEventError::SchemaViolation;
            for (JsonVariantConst c : p["enabled_commands"].as<JsonArrayConst>()) {
                if (!c.is<const char*>() ||
                    !str_in(c.as<const char*>(), {"launch_app", "quit_app"}))
                    return AgentEventError::SchemaViolation;
            }
            for (JsonVariantConst e : p["allowlisted_apps"].as<JsonArrayConst>()) {
                JsonObjectConst o = e.as<JsonObjectConst>();
                if (o.isNull() || o.size() != 2 || !o["bundle_id"].is<const char*>() ||
                    !o["state"].is<const char*>() ||
                    !str_in(o["state"].as<const char*>(), {"running", "not_running"}))
                    return AgentEventError::SchemaViolation;
            }
            return AgentEventError::Ok;
        }
    }
    return AgentEventError::SchemaViolation;
}

AgentEventError parse_agent_event(const std::string& json, AgentEvent& out) {
    if (json.size() > 4096) return AgentEventError::TooLarge;

    JsonDocument doc;
    DeserializationError derr = deserializeJson(doc, json);
    if (derr) return AgentEventError::MalformedJson;
    JsonObjectConst o = doc.as<JsonObjectConst>();
    if (o.isNull()) return AgentEventError::MalformedJson;

    // Envelope extraction: fills what we can even when the frame is rejected,
    // so seq accounting still applies to schema-violating frames (spec 6.1.1).
    out.event_id = o["event_id"] | "";
    out.agent_instance_id = o["agent_instance_id"] | "";
    out.session_id = o["session_id"] | "";
    out.seq = o["seq"] | 0;
    out.timestamp = o["timestamp"] | "";
    out.has_command_id = !o["command_id"].isNull();
    out.command_id = o["command_id"] | "";

    if (out.event_id.empty() || out.agent_instance_id.empty() || out.seq == 0 ||
        out.timestamp.empty() || !o["timestamp"].is<const char*>() ||
        !o["seq"].is<int>())
        return AgentEventError::SchemaViolation;

    const char* type = o["type"].as<const char*>();
    if (!type || !agent_event_type_from_string(type, out.type))
        return AgentEventError::UnknownType;

    // command_id is meaningful only on command_ack/command_result (spec 6.1).
    if (out.has_command_id && out.type != AgentEventType::CommandAck &&
        out.type != AgentEventType::CommandResult)
        return AgentEventError::SchemaViolation;

    JsonObjectConst p = o["payload"].as<JsonObjectConst>();
    if (p.isNull()) return AgentEventError::SchemaViolation;
    AgentEventError perr = validate_payload(out.type, p);
    if (perr != AgentEventError::Ok) return perr;

    out.payload_json.clear();
    serializeJson(p, out.payload_json);
    return AgentEventError::Ok;
}

} // namespace mcco
