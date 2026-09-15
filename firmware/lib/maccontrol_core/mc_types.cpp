#include "mc_types.h"

namespace mcco {

bool command_type_from_string(const char* s, CommandType& out) {
    const std::string v(s ? s : "");
    if (v == "wake") out = CommandType::Wake;
    else if (v == "sleep") out = CommandType::Sleep;
    else if (v == "restart") out = CommandType::Restart;
    else if (v == "shutdown") out = CommandType::Shutdown;
    else if (v == "lock") out = CommandType::Lock;
    else if (v == "macro_execute") out = CommandType::MacroExecute;
    else if (v == "app_launch") out = CommandType::AppLaunch;
    else if (v == "app_quit") out = CommandType::AppQuit;
    else return false;
    return true;
}

const char* command_type_to_string(CommandType t) {
    switch (t) {
        case CommandType::Wake: return "wake";
        case CommandType::Sleep: return "sleep";
        case CommandType::Restart: return "restart";
        case CommandType::Shutdown: return "shutdown";
        case CommandType::Lock: return "lock";
        case CommandType::MacroExecute: return "macro_execute";
        case CommandType::AppLaunch: return "app_launch";
        case CommandType::AppQuit: return "app_quit";
    }
    return "unknown";
}

bool command_state_from_string(const char* s, CommandState& out) {
    const std::string v(s ? s : "");
    if (v == "accepted") out = CommandState::Accepted;
    else if (v == "dispatched") out = CommandState::Dispatched;
    else if (v == "confirming") out = CommandState::Confirming;
    else if (v == "completed") out = CommandState::Completed;
    else if (v == "failed") out = CommandState::Failed;
    else if (v == "timed_out") out = CommandState::TimedOut;
    else if (v == "unconfirmed") out = CommandState::Unconfirmed;
    else return false;
    return true;
}

const char* command_state_to_string(CommandState s) {
    switch (s) {
        case CommandState::Accepted: return "accepted";
        case CommandState::Dispatched: return "dispatched";
        case CommandState::Confirming: return "confirming";
        case CommandState::Completed: return "completed";
        case CommandState::Failed: return "failed";
        case CommandState::TimedOut: return "timed_out";
        case CommandState::Unconfirmed: return "unconfirmed";
    }
    return "unknown";
}

const char* source_to_string(Source s) {
    switch (s) {
        case Source::Esp32Direct: return "esp32_direct";
        case Source::NetworkProbe: return "network_probe";
        case Source::AgentReported: return "agent_reported";
        case Source::Inferred: return "inferred";
        case Source::Unknown: return "unknown";
    }
    return "unknown";
}

const char* freshness_to_string(Freshness f) {
    switch (f) {
        case Freshness::Fresh: return "fresh";
        case Freshness::Stale: return "stale";
        case Freshness::ExpectedOffline: return "expected_offline";
        case Freshness::Unknown: return "unknown";
    }
    return "unknown";
}

bool role_from_string(const char* s, Role& out) {
    const std::string v(s ? s : "");
    if (v == "READ") out = Role::Read;
    else if (v == "CONTROL") out = Role::Control;
    else if (v == "ADMIN") out = Role::Admin;
    else return false;
    return true;
}

const char* role_to_string(Role r) {
    switch (r) {
        case Role::Read: return "READ";
        case Role::Control: return "CONTROL";
        case Role::Admin: return "ADMIN";
    }
    return "READ";
}

uint32_t default_deadline_s(CommandType t) {
    switch (t) {
        case CommandType::Wake: return 120;
        case CommandType::Sleep: return 90;
        case CommandType::Restart: return 180;
        case CommandType::Shutdown: return 120;
        case CommandType::Lock: return 15;
        case CommandType::MacroExecute: return 65; // macro timeout 60 + 5 (spec 5.3.1)
        case CommandType::AppLaunch: return 30;
        case CommandType::AppQuit: return 30;
    }
    return 60;
}

} // namespace mcco
