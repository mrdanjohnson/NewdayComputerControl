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
