#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace mcco {

// Closed command type enum (spec 12.2.1). Unknown types are rejected as
// 400 bad_request; app_launch/app_quit are rejected pre-ledger with 409
// agent_not_paired in Mode A (spec 12.3.1).
// `Unlock` is a spec AMENDMENT (2026-09-30): types the device-stored unlock
// password at the lock/login screen (spec fixes no such command; the ledger
// parameters stay empty so the secret never reaches the durable stream).
enum class CommandType : uint8_t {
    Wake, Sleep, Restart, Shutdown, Lock, Unlock, MacroExecute, AppLaunch, AppQuit
};

bool command_type_from_string(const char* s, CommandType& out);
const char* command_type_to_string(CommandType t);

// Lifecycle states (spec 5.2.1).
enum class CommandState : uint8_t {
    Accepted, Dispatched, Confirming, Completed, Failed, TimedOut, Unconfirmed
};

bool command_state_from_string(const char* s, CommandState& out);
const char* command_state_to_string(CommandState s);

inline bool is_terminal(CommandState s) {
    return s == CommandState::Completed || s == CommandState::Failed ||
           s == CommandState::TimedOut || s == CommandState::Unconfirmed;
}

// Provenance sources (spec 7.2.1), fixed rank order.
enum class Source : uint8_t {
    Esp32Direct = 1, NetworkProbe = 2, AgentReported = 3, Inferred = 4, Unknown = 5
};

const char* source_to_string(Source s);

// Freshness values (spec 7.2.2).
enum class Freshness : uint8_t { Fresh, Stale, ExpectedOffline, Unknown };

const char* freshness_to_string(Freshness f);

// RBAC roles, strict total order READ < CONTROL < ADMIN (spec 13.1.1).
enum class Role : uint8_t { Read = 0, Control = 1, Admin = 2 };

bool role_from_string(const char* s, Role& out);
const char* role_to_string(Role r);
inline bool role_at_least(Role have, Role need) { return uint8_t(have) >= uint8_t(need); }

// One ledger record revision (spec 5.1.1 schema). Records are never mutated
// in place; each transition appends a new revision sharing the command_id.
struct CommandRecord {
    std::string command_id;            // 8-char Crockford Base32
    std::string idempotency_key;       // may be empty
    uint32_t revision = 1;
    CommandType type = CommandType::Lock;
    std::string parameters_json;       // raw JSON object text, validated at accept
    std::string requested_by;          // "apikey:<key_id>"
    uint64_t requested_at = 0;         // epoch seconds
    char mode_at_accept = 'A';
    CommandState state = CommandState::Accepted;
    uint64_t dispatched_at = 0;        // 0 while not dispatched
    uint64_t deadline_at = 0;          // projected at accept from per-type default
    bool has_window = false;           // expected_offline_window present
    uint32_t window_open_after_s = 0;
    uint32_t window_close_after_s = 0;
    std::vector<std::string> evidence; // MCA event ids (empty in Mode A)
    std::string result;                // empty == null (non-terminal or unset)
    std::string error_code;            // empty == null
};

// Default per-type verification deadlines, seconds (spec 5.3.1 table).
uint32_t default_deadline_s(CommandType t);

} // namespace mcco
