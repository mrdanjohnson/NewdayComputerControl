#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <WiFiServer.h>
#include "app_context.h"

// Synchronous HTTP/1.1 server on WiFiServer port 80: Connection: close, one
// request at a time, served from its own FreeRTOS task (12 KB stack). The
// closed controller surface of spec 12.1.1 / 13.3 (Phase 1 subset) is
// implemented here; everything else — including /api/v1/exec, /api/v1/macros*,
// /api/v1/ota*, unversioned paths — is 404 not_found, and every /agent/v1/*
// path is 401 unauthorized (no pairing exists in Phase 1, spec 4.3.1).
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
