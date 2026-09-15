#include "status_cache.h"
#include <WiFi.h>
#include "esp_clock.h"
#include "hid_keyboard.h"
#include "mc_clock.h"
#include "nvs_config.h"

void StatusCache::begin(AppContext* ctx) {
    ctx_ = ctx;
    {
        Guard g(ctx->engine_mutex);
        identity_ = ctx_->config->identity();
    }
    identity_observed_at_ = ctx_->clock->epoch_seconds();
    network_probe_at_ = identity_observed_at_;
    network_up_ = (WiFi.status() == WL_CONNECTED);
    usb_up_ = ctx_->hid->mounted();
    timer_ = xTimerCreate("mc_cache", pdMS_TO_TICKS(1000), pdTRUE, this,
                          [](TimerHandle_t h) {
                              static_cast<StatusCache*>(pvTimerGetTimerID(h))->tick();
                          });
    if (timer_) xTimerStart(timer_, 0);
}

void StatusCache::tick() {
    const bool usb = ctx_->hid->mounted();
    bool bump = false;
    {
        Guard g(mutex_);
        if (usb != usb_up_) {
            usb_up_ = usb;
            bump = true;
        }
        if (++probe_div_ >= 30) {
            probe_div_ = 0;
            const bool net = (WiFi.status() == WL_CONNECTED);
            if (net != network_up_) {
                network_up_ = net;
                bump = true;
            }
            network_probe_at_ = ctx_->clock->epoch_seconds();
        }
        if (bump) ++cache_epoch_;
    }
}

void StatusCache::setNetworkUp(bool up) {
    Guard g(mutex_);
    if (up != network_up_) {
        network_up_ = up;
        ++cache_epoch_;
    }
    network_probe_at_ = ctx_->clock->epoch_seconds();
}

void StatusCache::onIdentityChanged() {
    Guard g(mutex_);
    identity_ = ctx_->config->identity();
    identity_observed_at_ = ctx_->clock->epoch_seconds();
    ++cache_epoch_;
}

void StatusCache::buildStatus(JsonDocument& doc) const {
    mcco::Identity id;
    bool usb, net;
    uint64_t probe_at, id_at, now;
    uint32_t epoch;
    {
        Guard g(mutex_);
        id = identity_;
        usb = usb_up_;
        net = network_up_;
        probe_at = network_probe_at_;
        id_at = identity_observed_at_;
        epoch = cache_epoch_;
    }
    now = ctx_->clock->epoch_seconds();
    mcco::build_status_mode_a(doc, id, usb, net, probe_at, id_at, now, epoch);
}

void StatusCache::buildCapabilities(JsonDocument& doc) const {
    mcco::Identity id;
    {
        Guard g(mutex_);
        id = identity_;
    }
    mcco::build_capabilities_mode_a(doc, id);
}
