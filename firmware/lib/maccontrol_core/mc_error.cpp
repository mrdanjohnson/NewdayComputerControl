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
        case ErrCode::RateLimited: return "Rate limit exceeded";
        case ErrCode::InternalError: return "Internal error";
        case ErrCode::LedgerUnavailable: return "Ledger write failed; nothing dispatched";
        case ErrCode::NetworkUnavailable: return "Network association lost";
    }
    return "Internal error";
}

} // namespace mcco
