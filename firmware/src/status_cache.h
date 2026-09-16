#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <ArduinoJson.h>
#include <cstdint>
#include "app_context.h"
#include "mc_status.h"

// Cached status model (spec 7.1.1): identity, USB mount state, and the last
// network probe result are sampled by a 1 s software timer (WiFi is re-probed
// every 30 s; WiFi state transitions are pushed immediately by WifiMgr).
// Status documents are built from the cached model only — no I/O at request
// time. cache_epoch is bumped on every mutation.
class StatusCache {
public:
    void begin(AppContext* ctx);
    void tick(); // 1 s timer callback

    void onIdentityChanged(); // reload identity from ConfigStore, bump epoch
    void setNetworkUp(bool up); // pushed by WifiMgr on transitions
    void onMacrosChanged(); // bump epoch + refresh advertised macro ids

    void buildStatus(JsonDocument& doc) const;
    void buildCapabilities(JsonDocument& doc) const;

    bool usbUp() const { return usb_up_; }
    bool networkUp() const { return network_up_; }
    uint64_t networkProbeAt() const { return network_probe_at_; }
    uint32_t cacheEpoch() const { return cache_epoch_; }
    uint64_t identityObservedAt() const { return identity_observed_at_; }

private:
    AppContext* ctx_ = nullptr;
    TimerHandle_t timer_ = nullptr;
    mcco::Identity identity_;
    bool usb_up_ = false;
    bool network_up_ = false;
    uint64_t network_probe_at_ = 0;
    uint64_t identity_observed_at_ = 0;
    uint32_t cache_epoch_ = 0;
    uint8_t probe_div_ = 0;
    std::vector<std::string> macro_ids_; // refreshed on onMacrosChanged()
    mutable Mutex mutex_;
};
