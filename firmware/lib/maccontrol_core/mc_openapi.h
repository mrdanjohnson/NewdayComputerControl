#pragma once

namespace mcco {

// OpenAPI 3.1 document for the implemented controller + Web UI surface
// (spec 12.1.1/17.1.1). Static and byte-identical to the routes registered
// in src/http_api.cpp and src/web_ui.cpp; amend both together. Kept in
// maccontrol_core so native tests can validate it.
extern const char kOpenApiJson[];

} // namespace mcco
