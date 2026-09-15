#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include "app_context.h"

// WiFi station manager: connects from NVS credentials, then keeps the
// association up with a reconnect backoff of 1/2/4/8/30 s + 0-20% jitter and
// no attempt cap (spec 4.3.2/15.1). Connectivity state is published to the
// StatusCache (and guards the HTTP accept path).
class WifiMgr {
public:
    void begin(AppContext* ctx);
    bool connected() const;
    // Applies new credentials (persisted by the caller) and restarts the
    // connect cycle immediately.
    void setCredentials(const std::string& ssid, const std::string& pass);

    TaskHandle_t taskHandle() const { return task_; }

private:
    static void taskEntry(void* arg);

    AppContext* ctx_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::string ssid_;
    std::string pass_;
    volatile bool force_reconnect_ = false;
};
