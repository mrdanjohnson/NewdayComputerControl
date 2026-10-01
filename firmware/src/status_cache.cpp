#include "status_cache.h"
#include <WiFi.h>
#include "esp_clock.h"
#include "hid_keyboard.h"
#include "mc_clock.h"
#include "mc_macro.h"
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

void StatusCache::onMacrosChanged() {
    std::vector<std::string> ids;
    {
        Guard g(ctx_->engine_mutex);
        if (ctx_->macros) {
            for (const mcco::Macro* m : ctx_->macros->list()) ids.push_back(m->macro_id);
        }
    }
    Guard g(mutex_);
    macro_ids_ = std::move(ids);
    ++cache_epoch_;
}

void StatusCache::onAgentChanged() {
    Guard g(mutex_);
    ++cache_epoch_;
}

void StatusCache::setAgentStatus(const mcco::AgentStatus& st) {
    Guard g(mutex_);
    agent_ = st;
    ++cache_epoch_;
}

mcco::AgentStatus StatusCache::snapshotAgent() const {
    Guard g(mutex_);
    return agent_;
}

void StatusCache::buildStatus(JsonDocument& doc) const {
    mcco::Identity id;
    bool usb, net;
    uint64_t probe_at, id_at, now;
    uint32_t epoch;
    mcco::AgentStatus agent;
    {
        Guard g(mutex_);
        id = identity_;
        usb = usb_up_;
        net = network_up_;
        probe_at = network_probe_at_;
        id_at = identity_observed_at_;
        epoch = cache_epoch_;
        agent = agent_;
    }
    now = ctx_->clock->epoch_seconds();
    // Mode B is a property of the PAIRING RECORD (spec 3.3/12.3.1), not of
    // the current session: a paired endpoint with the agent offline is still
    // mode B with agent.connected false.
    bool paired;
    {
        Guard g(ctx_->engine_mutex);
        paired = ctx_->pairing && ctx_->pairing->paired();
    }
    mcco::build_status(doc, id, usb, net, probe_at, id_at, paired ? &agent : nullptr, now, epoch);
    // Firmware version (spec 15.3: the running version is recorded in
    // GET /api/v1/status so controllers can attribute behavior to revisions).
    // Build-defined constant, so the tuple is esp32_direct/fresh by construction.
#ifndef MC_FW_VERSION
#define MC_FW_VERSION "dev"
#endif
    mcco::tuple_str(doc["device"].as<JsonObject>(), "firmware_version", MC_FW_VERSION,
                    mcco::Source::Esp32Direct, now, -1, mcco::Freshness::Fresh);
}

void StatusCache::buildCapabilities(JsonDocument& doc) const {
    mcco::Identity id;
    std::vector<std::string> macro_ids;
    mcco::AgentStatus agent;
    {
        Guard g(mutex_);
        id = identity_;
        macro_ids = macro_ids_;
        agent = agent_;
    }
    const mcco::AgentStatus* ap;
    {
        Guard g(ctx_->engine_mutex);
        ap = (ctx_->pairing && ctx_->pairing->paired()) ? &agent : nullptr;
    }
    mcco::build_capabilities(doc, id, macro_ids, ap, ctx_->clock->epoch_seconds());
}
