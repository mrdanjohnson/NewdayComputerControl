#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <WiFiServer.h>
#include "app_context.h"

// Synchronous HTTP/1.1 server on WiFiServer port 80: Connection: close, one
// request at a time, served from its own FreeRTOS task (12 KB stack). The
// closed controller surface of spec 12.1.1 / 13.3 (power commands + macros/
// triggers/identity + Phase 4 pairing administration) is implemented here;
// everything else — including /api/v1/exec, /api/v1/ota*, unversioned paths —
// is 404 not_found. The /agent/v1/* surface (spec 4.2/4.3) authenticates with
// the pairing token, not API keys, and is routed before the auth-key path;
// an upgraded /agent/v1/ws connection is handed to AgentLink by reference and
// must not be stopped here. GET / and /ui serve the single-page Web UI
// (always 200; the login form lives in the document), and a valid mc_session
// cookie is accepted as the ADMIN role for /api/v1/* (spec ch. 14).
// DEVIATIONS (Phase 3 reconciles with /openapi.json): GET/POST/PUT/DELETE
// /api/v1/triggers and POST /api/v1/device/identity are not in the spec
// ch.12 inventory; both are ADMIN-gated and exist because the Web UI owns
// trigger bindings and device identity (spec ch. 14 matrix).
class HttpApi {
public:
    void begin(AppContext* ctx, uint16_t port = 80);
    TaskHandle_t taskHandle() const { return task_; }

private:
    static void taskEntry(void* arg);
    void handleClient(WiFiClient& client, uint32_t header_timeout_ms);

    AppContext* ctx_ = nullptr;
    WiFiServer* server_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool ws_handed_off_ = false; // upgraded /agent/v1/ws client now owned by AgentLink
    bool last_keepalive_ = false; // last request asked for connection reuse
};
