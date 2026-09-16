#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <WiFiServer.h>
#include "app_context.h"

// Synchronous HTTP/1.1 server on WiFiServer port 80: Connection: close, one
// request at a time, served from its own FreeRTOS task (12 KB stack). The
// closed controller surface of spec 12.1.1 / 13.3 (Phase 1 power commands +
// Phase 2 macros/triggers/identity) is implemented here; everything else —
// including /api/v1/exec, /api/v1/ota*, unversioned paths — is 404
// not_found, and every /agent/v1/* path is 401 unauthorized (no pairing
// exists in Mode A, spec 4.3.1). GET / and /ui serve the single-page Web UI
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
    void handleClient(WiFiClient& client);

    AppContext* ctx_ = nullptr;
    WiFiServer* server_ = nullptr;
    TaskHandle_t task_ = nullptr;
};
