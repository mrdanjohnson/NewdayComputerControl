#include "wifi_mgr.h"
#include <WiFi.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include "log_sink.h"
#include "mc_log.h"
#include "mdns_service.h"
#include "nvs_config.h"
#include "status_cache.h"

namespace {
// 1/2/4/8/30 s + 0-20% jitter (spec 4.3.2). No cap on attempts.
constexpr uint32_t kBackoffS[] = {1, 2, 4, 8, 30};
constexpr size_t kBackoffCount = sizeof(kBackoffS) / sizeof(kBackoffS[0]);
constexpr uint32_t kConnectAttemptTimeoutMs = 15000;
} // namespace

void WifiMgr::begin(AppContext* ctx) {
    ctx_ = ctx;
    ssid_ = ctx_->config->wifiSsid();
    pass_ = ctx_->config->wifiPass();
    WiFi.mode(WIFI_STA);
    std::string host = ctx_->config->identity().hostname;
    if (!host.empty()) WiFi.setHostname(host.c_str());
    WiFi.setSleep(false);
    xTaskCreate(taskEntry, "mc_wifi", 3072, this, 4, &task_);
    esp_task_wdt_add(task_);
}

bool WifiMgr::connected() const { return WiFi.status() == WL_CONNECTED; }

void WifiMgr::setCredentials(const std::string& ssid, const std::string& pass) {
    ssid_ = ssid;
    pass_ = pass;
    force_reconnect_ = true;
    WiFi.disconnect();
}

void WifiMgr::taskEntry(void* arg) {
    WifiMgr* self = static_cast<WifiMgr*>(arg);
    AppContext* ctx = self->ctx_;
    size_t backoff_idx = 0;
    bool was_connected = false;

    for (;;) {
        esp_task_wdt_reset();

        if (self->ssid_.empty()) {
            if (was_connected) {
                was_connected = false;
                ctx->status_cache->setNetworkUp(false);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (WiFi.status() == WL_CONNECTED) {
            if (!was_connected) {
                was_connected = true;
                backoff_idx = 0;
                ctx->status_cache->setNetworkUp(true);
                // Form the IPv6 link-local address now, after mDNS is running,
                // so the responder learns the address and answers AAAA queries
                // (macOS stalls ~5 s on an unanswered AAAA for .local names).
                static bool ipv6_started = false;
                if (!ipv6_started) {
                    ipv6_started = true;
                    WiFi.enableIpV6();
                }
                // Spec 3.1: an unsolicited announcement MUST be issued on
                // joining a network. Reconnects (not just boot) therefore
                // re-announce; harmless if the label is unchanged.
                if (ctx->mdns) ctx->mdns->reannounce(ctx->config->identity());
                ctx->log->write(mcco::LogCategory::System, mcco::LogLevel::Info,
                                "wifi_connected", nullptr, nullptr, nullptr, nullptr);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (was_connected) {
            was_connected = false;
            ctx->status_cache->setNetworkUp(false);
            ctx->log->write(mcco::LogCategory::System, mcco::LogLevel::Warn,
                            "wifi_disconnected", nullptr, nullptr, nullptr, nullptr);
        }

        if (self->force_reconnect_) {
            self->force_reconnect_ = false;
            backoff_idx = 0;
        }

        WiFi.disconnect();
        WiFi.begin(self->ssid_.c_str(), self->pass_.c_str());
        bool ok = false;
        for (uint32_t waited = 0; waited < kConnectAttemptTimeoutMs; waited += 250) {
            if (WiFi.status() == WL_CONNECTED) {
                ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (ok) continue; // success path logs via the connected branch next loop

        uint32_t base_s = kBackoffS[backoff_idx < kBackoffCount ? backoff_idx : kBackoffCount - 1];
        ++backoff_idx; // never capped: retries forever at the 30 s step
        uint32_t jitter_ms = esp_random() % (base_s * 200 + 1);
        vTaskDelay(pdMS_TO_TICKS(base_s * 1000 + jitter_ms));
    }
}
