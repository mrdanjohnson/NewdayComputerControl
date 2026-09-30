#include "command_dispatcher.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <esp_task_wdt.h>
#include <string.h>
#include "agent_link.h"
#include "hid_keyboard.h"
#include "log_sink.h"
#include "mc_engine.h"
#include "mc_log.h"
#include "mc_types.h"
#include "macro_runner.h"
#include "power_probe.h"

namespace {
struct DispatchItem {
    char command_id[12]; // 8-char Crockford Base32 + NUL
};

// bundle_id out of a validated app_launch/app_quit parameters object.
std::string parse_bundle_id(const std::string& params_json) {
    JsonDocument doc;
    if (deserializeJson(doc, params_json) || !doc.is<JsonObjectConst>()) return "";
    const char* b = doc["bundle_id"].as<const char*>();
    return b ? b : "";
}
} // namespace

bool CommandDispatcher::begin(AppContext* ctx) {
    ctx_ = ctx;
    queue_ = xQueueCreate(kDepth, sizeof(DispatchItem));
    if (!queue_) return false;
    if (xTaskCreate(taskEntry, "mc_dispatch", 6144, this, 6, &task_) != pdPASS) return false;
    esp_task_wdt_add(task_);
    return true;
}

bool CommandDispatcher::enqueue(const std::string& command_id) {
    if (!queue_ || command_id.size() >= sizeof(DispatchItem::command_id)) return false;
    DispatchItem item;
    memset(&item, 0, sizeof(item));
    memcpy(item.command_id, command_id.data(), command_id.size());
    return xQueueSend(queue_, &item, 0) == pdTRUE;
}

bool CommandDispatcher::enqueue_macro(const std::string& command_id) {
    if (macro_queue_full()) return false;
    if (!enqueue(command_id)) return false;
    ++macro_pending_;
    return true;
}

bool CommandDispatcher::macro_queue_full() const {
    return macro_pending_.load() + (macro_inflight_.load() ? 1 : 0) >= kMacroQueueDepth;
}

void CommandDispatcher::taskEntry(void* arg) {
    CommandDispatcher* self = static_cast<CommandDispatcher*>(arg);
    AppContext* ctx = self->ctx_;
    for (;;) {
        esp_task_wdt_reset();
        DispatchItem item;
        // Timed wait so the watchdog is fed and the deadline sweep runs every
        // second even when idle (spec 5.2.1).
        const bool got = xQueueReceive(self->queue_, &item, pdMS_TO_TICKS(1000)) == pdTRUE;
        {
            Guard g(ctx->engine_mutex);
            ctx->engine->sweep_deadlines();
            // Spec 5.3.2 expected-offline windows: sleep completes at close,
            // silence onsets the offline notification missed become
            // backstop offline evidence. The channel verdict is the lock-free
            // session liveness (agent gate discipline, agent_link.h).
            ctx->engine->sweep_windows(ctx->agent_link && !ctx->agent_link->sessionActive());
        }
        // Agent OFFLINE transitions are deferred here (never in the timer
        // task): confirming records without a window -> unconfirmed (5.2.1).
        if (ctx->agent_link && ctx->agent_link->drainOfflinePending()) {
            Guard g(ctx->engine_mutex);
            ctx->engine->on_agent_offline();
        }
        // Spec 8.2.2 shutdown corroboration: non-blocking ICMP probe state
        // machine (no task of its own; the 1 s cadence lives here).
        power_probe_tick(ctx);
        if (!got) continue;

        // OOM firewall: engine/ledger/std::string work below can throw
        // bad_alloc under heap pressure — that must skip the command, never
        // reach std::terminate (abort -> reboot).
        try {
        mcco::CommandType type;
        bool is_macro = false;
        std::string params;
        {
            Guard g(ctx->engine_mutex);
            const mcco::CommandRecord* rec = ctx->engine->get(item.command_id);
            if (!rec) continue;
            type = rec->type;
            params = rec->parameters_json;
            is_macro = (type == mcco::CommandType::MacroExecute);
            if (rec->state != mcco::CommandState::Accepted) {
                if (is_macro && self->macro_pending_.load() > 0) --self->macro_pending_;
                continue;
            }
        }

        if (is_macro) {
            // The interpreter is single-threaded: the dispatcher serializes,
            // so at most one macro runs at a time (spec 10.3.1).
            if (self->macro_pending_.load() > 0) --self->macro_pending_;
            self->macro_inflight_ = true;
            run_macro(ctx, item.command_id);
            self->macro_inflight_ = false;
            continue;
        }

        // Agent-executed power actions (protocol v2): in Mode B with a live
        // session the MCA performs sleep/restart/shutdown in software and
        // declares the matching expected-offline goodbye before acting; the
        // record stays `confirming` and the engine completes it from
        // goodbye/window/boot_id evidence. Mode A (or a dead session) keeps
        // the HID path below.
        if ((type == mcco::CommandType::Sleep || type == mcco::CommandType::Restart ||
             type == mcco::CommandType::Shutdown) &&
            ctx->agent_link && ctx->agent_link->sessionActive()) {
            const char* action = (type == mcco::CommandType::Sleep)     ? "sleep"
                                 : (type == mcco::CommandType::Restart) ? "restart"
                                                                        : "shutdown";
            const bool ok = ctx->agent_link->enqueueDispatch(action, "", item.command_id);
            {
                Guard g(ctx->engine_mutex);
                if (ok) {
                    ctx->engine->complete_dispatch(item.command_id, true);
                } else {
                    ctx->engine->fail_dispatch(item.command_id, "dispatch_error");
                }
            }
            ctx->log->write(mcco::LogCategory::Command,
                            ok ? mcco::LogLevel::Info : mcco::LogLevel::Error,
                            ok ? "agent_dispatch" : "agent_dispatch_failed", item.command_id,
                            nullptr, nullptr, nullptr);
            continue;
        }

        if (type == mcco::CommandType::AppLaunch || type == mcco::CommandType::AppQuit) {
            // Mode B: the MCA executes the action and the terminal verdict
            // arrives as command_ack + application evidence (spec 5.3.1); the
            // record stays `confirming` after complete_dispatch(ok=true).
            const char* action =
                (type == mcco::CommandType::AppLaunch) ? "launch_app" : "quit_app";
            const std::string bundle_id = parse_bundle_id(params);
            bool ok = false;
            if (!bundle_id.empty()) {
                ok = ctx->agent_link->enqueueDispatch(action, bundle_id, item.command_id);
            }
            {
                Guard g(ctx->engine_mutex);
                if (ok) {
                    ctx->engine->complete_dispatch(item.command_id, true);
                } else {
                    ctx->engine->fail_dispatch(item.command_id, "dispatch_error");
                }
            }
            ctx->log->write(mcco::LogCategory::Command,
                            ok ? mcco::LogLevel::Info : mcco::LogLevel::Error,
                            ok ? "agent_dispatch" : "agent_dispatch_failed", item.command_id,
                            nullptr, nullptr, nullptr);
            continue;
        }

        if (type == mcco::CommandType::Wake && ctx->agent_link) {
            // Spec 8.1.2: when the Mac is already awake, closing the
            // incumbent session with 1000 forces a fresh hello + initial
            // burst — the wake predicate's evidence. offline_effect=false:
            // the wake record has no offline window, so the deliberate
            // absence must run to timed_out, never unconfirmed/evidence_lost.
            ctx->agent_link->requestClose(1000, /*offline_effect=*/false);
        }
        const bool ok = ctx->hid->send_chord(type);
        {
            Guard g(ctx->engine_mutex);
            ctx->engine->complete_dispatch(item.command_id, ok);
        }
        if (type == mcco::CommandType::Wake) {
            // Remote-wakeup visibility: a wake that fails on a suspended
            // bus is otherwise indistinguishable from a dead HID path.
            // Detail keys are single-letter: the log ring slot is 224 B
            // total and the base entry already consumes most of it.
            const HidKeyboard::WakeDebug wd = ctx->hid->wakeDebug();
            char detail[64];
            snprintf(detail, sizeof(detail), "{\"sus\":%d,\"wu\":%d,\"ms\":%u,"
                                             "\"ok\":%d}",
                     wd.was_suspended ? 1 : 0, wd.wakeup_ok ? 1 : 0,
                     wd.resume_ms, wd.sent_ok ? 1 : 0);
            ctx->log->write(mcco::LogCategory::Command,
                            ok ? mcco::LogLevel::Info : mcco::LogLevel::Warn,
                            "wake_debug", item.command_id, nullptr, nullptr,
                            detail);
        } else {
            ctx->log->write(mcco::LogCategory::Command,
                            ok ? mcco::LogLevel::Info : mcco::LogLevel::Error,
                            ok ? "dispatch" : "dispatch_failed",
                            item.command_id, nullptr, nullptr, nullptr);
        }
        } catch (const std::exception&) {
            ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Error,
                            "dispatch_exception", item.command_id, nullptr, nullptr, nullptr);
        } catch (...) {
            ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Error,
                            "dispatch_exception", item.command_id, nullptr, nullptr, nullptr);
        }
    }
}
