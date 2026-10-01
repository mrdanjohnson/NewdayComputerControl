#include "mc_error.h"

namespace mcco {

int error_http_status(ErrCode c) {
    switch (c) {
        case ErrCode::BadRequest: return 400;
        case ErrCode::Unauthorized: return 401;
        case ErrCode::Forbidden: return 403;
        case ErrCode::NotFound: return 404;
        case ErrCode::Conflict: return 409;
        case ErrCode::AgentNotPaired: return 409;
        case ErrCode::AgentOffline: return 409;
        case ErrCode::CommandDisabled: return 409;
        case ErrCode::AppNotAllowlisted: return 409;
        case ErrCode::AppNotRegistered: return 409;
        case ErrCode::AppControlDisabled: return 409;
        case ErrCode::ValidationFailed: return 400;
        case ErrCode::MacroInvalidStep: return 400;
        case ErrCode::MacroQueueFull: return 409;
        case ErrCode::OtaInProgress: return 409;
        case ErrCode::StoreCorrupt: return 409;
        case ErrCode::RateLimited: return 429;
        case ErrCode::InternalError: return 500;
        case ErrCode::LedgerUnavailable: return 503;
        case ErrCode::NetworkUnavailable: return 503;
    }
    return 500;
}

const char* error_code_string(ErrCode c) {
    switch (c) {
        case ErrCode::BadRequest: return "bad_request";
        case ErrCode::Unauthorized: return "unauthorized";
        case ErrCode::Forbidden: return "forbidden";
        case ErrCode::NotFound: return "not_found";
        case ErrCode::Conflict: return "conflict";
        case ErrCode::AgentNotPaired: return "agent_not_paired";
        case ErrCode::AgentOffline: return "agent_offline";
        case ErrCode::CommandDisabled: return "command_disabled";
        case ErrCode::AppNotAllowlisted: return "app_not_allowlisted";
        case ErrCode::AppNotRegistered: return "app_not_registered";
        case ErrCode::AppControlDisabled: return "app_control_disabled";
        case ErrCode::ValidationFailed: return "validation_failed";
        case ErrCode::MacroInvalidStep: return "macro_invalid_step";
        case ErrCode::MacroQueueFull: return "macro_queue_full";
        case ErrCode::OtaInProgress: return "ota_in_progress";
        case ErrCode::StoreCorrupt: return "store_corrupt";
        case ErrCode::RateLimited: return "rate_limited";
        case ErrCode::InternalError: return "internal_error";
        case ErrCode::LedgerUnavailable: return "ledger_unavailable";
        case ErrCode::NetworkUnavailable: return "network_unavailable";
    }
    return "internal_error";
}

const char* default_error_message(ErrCode c) {
    switch (c) {
        case ErrCode::BadRequest: return "Malformed JSON, unknown field, or schema violation";
        case ErrCode::Unauthorized: return "API key missing or invalid";
        case ErrCode::Forbidden: return "Valid credential with insufficient role, or revoked credential";
        case ErrCode::NotFound: return "Unknown path, command_id, or version";
        case ErrCode::Conflict: return "Idempotency key replay with divergent body";
        case ErrCode::AgentNotPaired: return "Agent-dependent command or endpoint requested with no active pairing";
        case ErrCode::AgentOffline: return "Agent paired but its session is not ACTIVE";
        case ErrCode::CommandDisabled: return "Action absent from MCA capability_report enabled_commands";
        case ErrCode::AppNotAllowlisted: return "Target bundle_id absent from MCA allowlisted_apps";
        case ErrCode::AppNotRegistered: return "Target bundle_id absent from the monitored registry";
        case ErrCode::AppControlDisabled: return "Registry entry has control_enabled false";
        case ErrCode::ValidationFailed: return "Agent event envelope schema violation or unknown event type";
        case ErrCode::MacroInvalidStep: return "Invalid macro definition (step outside enums, forbidden expected_event)";
        case ErrCode::MacroQueueFull: return "Macro queue at capacity";
        case ErrCode::OtaInProgress: return "An OTA operation is in progress, or commands are in flight";
        case ErrCode::StoreCorrupt: return "Macro store CRC failure";
        case ErrCode::RateLimited: return "Rate limit exceeded";
        case ErrCode::InternalError: return "Internal error";
        case ErrCode::LedgerUnavailable: return "Ledger write failed; nothing dispatched";
        case ErrCode::NetworkUnavailable: return "Network association lost";
    }
    return "Internal error";
}

} // namespace mcco
