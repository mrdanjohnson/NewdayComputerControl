#pragma once
#include <string>

namespace mcco {

// Deterministic error codes (spec 4.1.1 / 12.3.1). The HTTP status mapping is
// closed and total: the endpoint MUST NOT emit a status outside this table.
enum class ErrCode {
    BadRequest,
    Unauthorized,
    Forbidden,
    NotFound,
    Conflict,
    AgentNotPaired,
    AgentOffline,     // 409 agent_offline (spec 12.3.1)
    CommandDisabled,  // 409 command_disabled (spec 12.3.1)
    AppNotAllowlisted,// 409 app_not_allowlisted (spec 12.3.1)
    AppNotRegistered, // 409 app_not_registered (Phase 6 registry, spec 12.3.1)
    AppControlDisabled, // 409 app_control_disabled (Phase 6, spec 12.3.1)
    ValidationFailed, // 400 validation_failed (spec 4.3.1 agent envelope)
    MacroInvalidStep, // 400 macro_invalid_step (spec 10.1.1)
    MacroQueueFull,   // 409 macro_queue_full (spec 10.3.1)
    StoreCorrupt,     // 409 store_corrupt (spec 15.1)
    RateLimited,
    InternalError,
    LedgerUnavailable,
    NetworkUnavailable
};

struct ApiError {
    ErrCode code;
    std::string message;
};

int error_http_status(ErrCode c);
const char* error_code_string(ErrCode c);
const char* default_error_message(ErrCode c);

} // namespace mcco
