#include <Arduino.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "app_context.h"
#include "cli.h"
#include "command_dispatcher.h"
#include "esp_clock.h"
#include "esp_rng.h"
#include "fs_ledger_storage.h"
#include "hid_keyboard.h"
#include "http_api.h"
#include "log_sink.h"
#include "mc_auth.h"
#include "mc_engine.h"
#include "mc_log.h"
#include "mc_rate_limit.h"
#include "mdns_service.h"
#include "nvs_config.h"
#include "status_cache.h"
#include "wifi_mgr.h"

namespace {

AppContext ctx;

ConfigStore g_config;
EspClock g_clock;
EspRandom g_rng;
RamLogSink g_log(g_config, g_clock);
FsLedgerStorage g_ledger_storage;
mcco::Ledger g_ledger(g_ledger_storage, g_clock, g_log);
mcco::CommandEngine g_engine(g_ledger, g_clock, g_rng, g_log);
mcco::KeyStore g_keys;
mcco::RateLimiter g_limiter(g_clock);
HidKeyboard g_hid;
CommandDispatcher g_dispatcher;
StatusCache g_status_cache;
MdnsService g_mdns;
WifiMgr g_wifi;
HttpApi g_http;
Cli g_cli;

} // namespace

void setup() {
    // Static context pointers first: the CLI banner reads ctx->config.
    ctx.config = &g_config;
    ctx.clock = &g_clock;
    ctx.rng = &g_rng;
    ctx.log = &g_log;

    // NVS config first: the CLI banner reads the loaded identity.
    if (!g_config.load()) {
        Serial.println("FATAL: NVS unavailable");
        delay(1000);
        ESP.restart();
    }
    if (g_config.firstBoot()) {
        Serial.printf("first boot: generated identity %s / %s\n",
                      g_config.identity().hostname.c_str(),
                      g_config.identity().device_id.c_str());
    } else if (!g_config.fallbackNote().empty()) {
        Serial.printf("config fallback: %s\n", g_config.fallbackNote().c_str());
    }

    // Serial + CLI banner (physical console = ADMIN provisioning).
    g_cli.begin(&ctx);
    g_log.restoreSeqFromConfig();

    // LittleFS + ledger storage (torn-tail discard happens here).
    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS mount failed");
        delay(1000);
        ESP.restart();
    }
    if (!g_ledger_storage.begin()) {
        Serial.println("FATAL: ledger storage unavailable");
        delay(1000);
        ESP.restart();
    }
    ctx.ledger_storage = &g_ledger_storage;
    ctx.ledger = &g_ledger;

    // Key store hydration (digests only; raw keys never touch flash/log).
    if (!g_config.hydrateKeys(g_keys)) {
        Serial.println("note: no persisted API keys (use 'key create')");
    }
    ctx.keys = &g_keys;
    ctx.limiter = &g_limiter;

    // Wall clock: SNTP against the NVS-configured server (UTC).
    g_clock.beginSntp(g_config.ntpServer());

    // Command engine: ledger load + boot reconciliation (<= 2 s, spec 5.1.1).
    const uint32_t boot_start_ms = millis();
    if (!g_engine.init()) {
        Serial.println("FATAL: ledger load failed");
        delay(1000);
        ESP.restart();
    }
    ctx.engine = &g_engine;
    char recon_detail[64];
    snprintf(recon_detail, sizeof(recon_detail), "{\"ms\":%u,\"commands\":%u}",
             (unsigned)(millis() - boot_start_ms), (unsigned)g_ledger.command_count());
    g_log.write(mcco::LogCategory::System, mcco::LogLevel::Warn, "boot_reconciliation", nullptr,
                nullptr, nullptr, recon_detail);
    g_log.write(mcco::LogCategory::System, mcco::LogLevel::Warn, "boot", nullptr, nullptr,
                nullptr,
                (std::string("{\"device_id\":\"") + g_config.identity().device_id + "\"}")
                    .c_str());

    // USB HID (keyboard-only descriptor per build flag).
    g_hid.begin();
    ctx.hid = &g_hid;

    // Watchdog: 10 s timeout, panic on missed feeds (spec 15.1). Subscribe the
    // loop task here; server/dispatcher/wifi tasks subscribe at creation.
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(nullptr); // loopTask

    ctx.status_cache = &g_status_cache;
    g_status_cache.begin(&ctx);

    ctx.wifi = &g_wifi;
    g_wifi.begin(&ctx);

    ctx.mdns = &g_mdns;
    if (!g_mdns.begin(g_config.identity())) {
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Error,
                    "hostname_collision_exhausted", nullptr, nullptr, nullptr, nullptr);
    }

    ctx.dispatcher = &g_dispatcher;
    g_dispatcher.begin(&ctx);

    g_http.begin(&ctx, 80);

    Serial.printf("init complete in %u ms\n", (unsigned)(millis() - boot_start_ms));
}

void loop() {
    esp_task_wdt_reset();
    g_cli.poll();

    // Lazily persist key last_used_at mutations (bounds NVS flash wear).
    if (ctx.keys_dirty.exchange(false)) {
        g_config.persistKeys(g_keys);
    }
    delay(10);
}
