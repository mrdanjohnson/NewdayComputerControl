#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <optional>
#include "app_context.h"
#include "agent_link.h"
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
#include "mc_macro.h"
#include "mc_pairing.h"
#include "mc_rate_limit.h"
#include "mdns_service.h"
#include "nvs_config.h"
#include "status_cache.h"
#include "trigger_store.h"
#include "usb_link.h"
#include "web_ui.h"
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
mcco::MacroStore g_macros;
mcco::PairingStore* g_pairing = nullptr; // constructed in setup (needs device_id)
TriggerStore g_triggers;
WebUi g_web_ui;
HidKeyboard g_hid;
CommandDispatcher g_dispatcher;
StatusCache g_status_cache;
MdnsService g_mdns;
WifiMgr g_wifi;
HttpApi g_http;
AgentLink g_agent_link;
Cli g_cli;

// Single point of truth for the pairing side effects (spec 3.2): engine mode
// and the mDNS TXT ride the same pairing_dirty flag the routes set, so no
// route has to do mDNS or engine work under engine_mutex.
// Spec 3.3 TXT vocabulary: pair=unpaired|active|revoked.
static const char* pair_txt(mcco::PairingState s) {
    switch (s) {
        case mcco::PairingState::Active: return "active";
        case mcco::PairingState::Revoked: return "revoked";
        default: return "unpaired";
    }
}

// Classifies the PREVIOUS boot's termination: the RAM log ring dies with the
// chip, so without this an external reset (serial DTR/RTS landmine) and an
// internal abort (WDT panic, brownout) are indistinguishable after the fact.
static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "ext";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

void apply_pairing_mode() {
    mcco::PairingState st;
    {
        Guard g(ctx.engine_mutex);
        st = g_pairing->state();
        g_config.persistPairing(*g_pairing);
    }
    g_engine.set_mode(st == mcco::PairingState::Active ? 'B' : 'A');
    g_mdns.setAgentTxt(st == mcco::PairingState::Active ? "B" : "A", pair_txt(st));
}

} // namespace

void setup() {
    // Heap-fragmentation canary: hold a large contiguous block through
    // bring-up (WiFi/lwIP/mDNS allocate around it), then release it before
    // the HTTP surface opens so response paths always have a big hole for
    // pbuf clusters and ArduinoJson pools. Under AT poll volume the heap
    // otherwise fragments until lwIP cannot allocate TX buffers — write()
    // EAGAIN storms and multi-second stalls (observed on hardware: min
    // largest-free-block 2292 B, then an mc_http task-WDT abort).
    void* heap_canary = heap_caps_malloc(24 * 1024, MALLOC_CAP_8BIT);

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

    // Phase 4: pairing store (spec 3.2). Constructed after the config load
    // because the record pins the device_id; hydrated from the same
    // double-slot NVS record machinery as the keys (digest only, never the
    // raw agent token).
    g_pairing = new mcco::PairingStore(g_rng, g_clock, g_config.identity().device_id);
    if (g_config.hydratePairing(*g_pairing)) {
        g_log.write(mcco::LogCategory::Auth, mcco::LogLevel::Info, "pairing_restored", nullptr,
                    nullptr, nullptr, nullptr);
    }
    ctx.pairing = g_pairing;

    // Phase 2: macro store + trigger bindings (spec ch. 10). Loaded before
    // the HTTP surface begins; any skipped corrupt line latches store_corrupt
    // (spec 15.1) until a successful persistence write clears it.
    ctx.macros = &g_macros;
    {
        std::vector<std::string> lines;
        if (g_config.loadMacroLines(lines)) {
            const size_t skipped = g_macros.load(lines);
            if (skipped > 0) {
                ctx.store_corrupt = true;
                g_log.write(mcco::LogCategory::Config, mcco::LogLevel::Error, "macro_store_corrupt",
                            nullptr, nullptr, nullptr,
                            (std::string("{\"skipped\":") + std::to_string(skipped) + "}")
                                .c_str());
            }
        }
    }
    ctx.triggers = &g_triggers;
    {
        std::vector<std::string> lines;
        if (g_config.loadTriggerLines(lines)) {
            const size_t skipped = g_triggers.load(lines);
            if (skipped > 0) {
                g_log.write(mcco::LogCategory::Config, mcco::LogLevel::Warn,
                            "trigger_store_lines_skipped", nullptr, nullptr, nullptr,
                            (std::string("{\"skipped\":") + std::to_string(skipped) + "}")
                                .c_str());
            }
        }
    }
    ctx.web_ui = &g_web_ui;
    g_web_ui.begin(&ctx);

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
    // Macro resolver (spec 10.3): the engine checks macro existence pre-ledger
    // through this hook. submit() is invoked with engine_mutex already held
    // at every call site, so the resolver itself MUST NOT take the lock
    // (FreeRTOS mutexes are not recursive).
    g_engine.set_macro_resolver([](const std::string& macro_id) {
        mcco::CommandEngine::MacroResolution res;
        if (ctx.macros) {
            const mcco::Macro* m = ctx.macros->get(macro_id);
            if (m) {
                res.found = true;
                res.timeout_ms = m->timeout_ms;
            }
        }
        return res;
    });
    // Phase 4 (spec 5.3.1/11.2.1): pre-ledger gate for agent-dependent
    // submissions. submit() runs with engine_mutex already held, so the gate
    // must not lock engine_mutex again — pairing is already covered by the
    // caller's lock; the session flag is a lock-free atomic and the evidence
    // snapshot takes only the status-cache mutex (engine -> cache order).
    g_engine.set_mode(g_pairing->paired() ? 'B' : 'A');
    g_engine.set_agent_gate([](const mcco::Submission& sub) -> std::optional<mcco::ErrCode> {
        using mcco::ErrCode;
        if (sub.type != mcco::CommandType::AppLaunch && sub.type != mcco::CommandType::AppQuit)
            return std::nullopt;
        if (!ctx.pairing->paired()) return ErrCode::AgentNotPaired;
        if (!ctx.agent_link->sessionActive()) return ErrCode::AgentOffline;
        const mcco::AgentStatus st = ctx.status_cache->snapshotAgent();
        std::string bundle;
        {
            JsonDocument p;
            if (!deserializeJson(p, sub.parameters_json) && p.is<JsonObjectConst>()) {
                const char* b = p["bundle_id"].as<const char*>();
                if (b) bundle = b;
            }
        }
        bool allowlisted = false;
        for (const auto& ba : st.allowlisted_apps) {
            if (ba.first == bundle) {
                allowlisted = true;
                break;
            }
        }
        if (!allowlisted) return ErrCode::AppNotAllowlisted;
        const char* action =
            sub.type == mcco::CommandType::AppLaunch ? "launch_app" : "quit_app";
        bool enabled = false;
        for (const std::string& c : st.enabled_commands) {
            if (c == action) {
                enabled = true;
                break;
            }
        }
        if (!enabled) return ErrCode::CommandDisabled;
        return std::nullopt;
    });
    char recon_detail[64];
    snprintf(recon_detail, sizeof(recon_detail), "{\"ms\":%u,\"commands\":%u}",
             (unsigned)(millis() - boot_start_ms), (unsigned)g_ledger.command_count());
    g_log.write(mcco::LogCategory::System, mcco::LogLevel::Warn, "boot_reconciliation", nullptr,
                nullptr, nullptr, recon_detail);
    g_log.write(mcco::LogCategory::System, mcco::LogLevel::Warn, "boot", nullptr, nullptr,
                nullptr,
                (std::string("{\"device_id\":\"") + g_config.identity().device_id +
                 "\",\"reset\":\"" + reset_reason_str(esp_reset_reason()) + "\"}")
                    .c_str());
    Serial.printf("reset reason: %s\n", reset_reason_str(esp_reset_reason()));

    // USB HID (keyboard-only descriptor per build flag). The link tracker
    // registers first so the initial enumeration is captured as baseline.
    usb_link_begin(&g_clock);
    g_hid.begin();
    ctx.hid = &g_hid;

    // Watchdog: 10 s timeout, panic on missed feeds (spec 15.1). Subscribe the
    // loop task here; server/dispatcher/wifi tasks subscribe at creation.
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(nullptr); // loopTask

    ctx.status_cache = &g_status_cache;
    g_status_cache.begin(&ctx);
    ctx.status_cache->onMacrosChanged(); // advertise the loaded macro ids (12.3.1)

    ctx.wifi = &g_wifi;
    g_wifi.begin(&ctx);

    ctx.mdns = &g_mdns;
    if (!g_mdns.begin(g_config.identity())) {
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Error,
                    "hostname_collision_exhausted", nullptr, nullptr, nullptr, nullptr);
    }
    if (g_pairing->state() != mcco::PairingState::Unpaired) {
        // TXT reflects the restored pairing record (spec 3.3 vocabulary).
        g_mdns.setAgentTxt(g_pairing->paired() ? "B" : "A", pair_txt(g_pairing->state()));
    }

    // Phase 4: MCA evidence channel (spec 4.2/4.3). Owns the mc_agent_ws task
    // and the 1 s liveness timer.
    ctx.agent_link = &g_agent_link;
    if (!g_agent_link.begin(&ctx)) {
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Error, "agent_link_init_failed",
                    nullptr, nullptr, nullptr, nullptr);
    }

    ctx.dispatcher = &g_dispatcher;
    g_dispatcher.begin(&ctx);

    // GPIO bindings attach after everything they invoke is wired.
    g_triggers.begin(&ctx);

    // Release the canary: the 24 KB contiguous hole is now free for the
    // server's TX/JSON allocations (see setup() top).
    if (heap_canary) heap_caps_free(heap_canary);

    g_http.begin(&ctx, 80);

    Serial.printf("init complete in %u ms\n", (unsigned)(millis() - boot_start_ms));
}

void loop() {
    esp_task_wdt_reset();
    try {
        g_cli.poll();
    } catch (const std::exception&) {
        // OOM firewall: a CLI command that cannot allocate must not abort.
        Serial.println("error: command failed (out of memory)");
    } catch (...) {
        Serial.println("error: command failed");
    }

    // Lazily persist key last_used_at mutations. Throttled: touch() is
    // quantized to 60 s so polling only dirties the store once a minute, and
    // the std::string built by persistKeys needs a large contiguous heap
    // block — under AT poll load an unthrottled drain can hit a fragmented
    // heap and bad_alloc aborts the firmware (observed on hardware during
    // AT-11). Leaving the flag set retries on a later pass; a failed persist
    // must not kill the device.
    static uint32_t last_keys_persist_ms = 0;
    try {
        if (ctx.keys_dirty && millis() - last_keys_persist_ms > 5000) {
            ctx.keys_dirty = false;
            if (g_config.persistKeys(g_keys)) last_keys_persist_ms = millis();
            else ctx.keys_dirty = true; // retry later
        }
        // Lazily persist macro store and trigger mutations (spec 15.1). A
        // successful write is also what clears the sticky store_corrupt latch.
        if (ctx.macros_dirty.exchange(false)) {
            if (g_config.persistMacroLines(g_macros.dump())) {
                if (ctx.store_corrupt.exchange(false)) {
                    g_log.write(mcco::LogCategory::Config, mcco::LogLevel::Warn,
                                "macro_store_recovered", nullptr, nullptr, nullptr, nullptr);
                }
            } else {
                ctx.macros_dirty = true; // retry on the next pass
            }
        }
        if (ctx.triggers_dirty.exchange(false)) {
            if (!g_config.persistTriggerLines(g_triggers.dump())) {
                ctx.triggers_dirty = true;
            }
        }
        // Pairing state changed (pair/revoke route or CLI): persist the
        // record, flip the engine mode and update the mDNS TXT.
        if (ctx.pairing_dirty.exchange(false)) {
            apply_pairing_mode();
        }
    } catch (const std::exception& e) {
        // OOM-safety net: a failed lazy persist must never abort the
        // firmware (bad_alloc in a std::string during heap fragmentation).
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Error,
                    "persist_failed", nullptr, nullptr, nullptr, nullptr);
        (void)e;
    } catch (...) {
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Error,
                    "persist_failed", nullptr, nullptr, nullptr, nullptr);
    }

    // Heap watermark telemetry: under AT poll load the device rebooted with
    // bad_alloc aborts, so track free/largest/minimum to correlate crashes
    // with the leaking/churning phase (temporary diagnostic). Task stack
    // high-water marks (bytes) expose which task is closest to its limit.
    static uint32_t last_heap_log_ms = 0;
    if (millis() - last_heap_log_ms > 30000) {
        last_heap_log_ms = millis();
        auto hwm = [](TaskHandle_t t) { return t ? uxTaskGetStackHighWaterMark(t) * 4 : 0; };
        std::string detail = std::string("{\"free\":") +
                             std::to_string(esp_get_free_heap_size()) +
                             ",\"min\":" + std::to_string(esp_get_minimum_free_heap_size()) +
                             ",\"largest\":" +
                             std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)) +
                             ",\"stacks\":{\"http\":" + std::to_string(hwm(g_http.taskHandle())) +
                             ",\"ws\":" + std::to_string(hwm(g_agent_link.taskHandle())) +
                             ",\"dispatch\":" + std::to_string(hwm(g_dispatcher.taskHandle())) +
                             ",\"wifi\":" + std::to_string(hwm(g_wifi.taskHandle())) + "}}";
        g_log.write(mcco::LogCategory::System, mcco::LogLevel::Info, "heap",
                    nullptr, nullptr, nullptr, detail.c_str());
        Serial.printf("[heap] %s\n", detail.c_str());  // TEMP diagnostic
    }
    delay(10);
}
