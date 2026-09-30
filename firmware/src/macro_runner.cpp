#include "macro_runner.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <algorithm>
#include <vector>
#include "command_dispatcher.h"
#include "esp_clock.h"
#include "hid_keyboard.h"
#include "log_sink.h"
#include "mc_iso8601.h"
#include "mc_ledger.h"
#include "mc_log.h"
#include "mc_macro.h"
#include "mc_sha256.h"
#include "mc_types.h"

namespace {

constexpr uint32_t kInterKeyMs = 10;       // spec 10.3.1 default inter-key interval
constexpr uint32_t kReportGapMs = 10;      // settle gap around press/release pairs
constexpr uint64_t kDispatchDelayBoundS = 30; // spec 10.3.1 dispatch_delayed

// Aborts the interpretation: all-keys-up report first (spec 10.3.1), then the
// terminal failed revision under the engine mutex.
void abort_macro(AppContext* ctx, const std::string& command_id, const char* error_code) {
    ctx->hid->allKeysUp();
    {
        Guard g(ctx->engine_mutex);
        ctx->engine->fail_dispatch(command_id, error_code);
    }
    ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Warn, "macro_aborted",
                    command_id.c_str(), nullptr, nullptr, nullptr);
}

} // namespace

MacroExecOutcome submit_macro_execute(AppContext* ctx, const std::string& macro_id,
                                      const std::string& requested_by) {
    MacroExecOutcome out;
    // Pre-ledger refusals (spec 10.3.1, 15.1) — no record is created.
    if (ctx->store_corrupt.load()) {
        out.error = mcco::ErrCode::StoreCorrupt;
        return out;
    }
    {
        Guard g(ctx->engine_mutex);
        if (!ctx->macros || !ctx->macros->get(macro_id)) {
            out.error = mcco::ErrCode::NotFound;
            return out;
        }
    }
    if (ctx->dispatcher->macro_queue_full()) {
        out.error = mcco::ErrCode::MacroQueueFull;
        return out;
    }

    const std::string canonical = std::string("{\"type\":\"macro_execute\",\"parameters\":{"
                                              "\"macro_id\":\"") +
                                  macro_id + "\"}}";
    mcco::Submission sub;
    sub.type = mcco::CommandType::MacroExecute;
    sub.parameters_json = std::string("{\"macro_id\":\"") + macro_id + "\"}";
    sub.requested_by = requested_by;
    sub.body_hash = mcco::Sha256::hex_digest(canonical);
    {
        Guard g(ctx->engine_mutex);
        out.submit = ctx->engine->submit(sub);
    }
    if (!out.submit.ok) {
        out.error = out.submit.error;
        return out;
    }
    out.ok = true;
    ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "command_accepted",
                    out.submit.record.command_id.c_str(), nullptr, requested_by.c_str(),
                    nullptr);
    // Best effort: the queue was checked above, but the shared FreeRTOS queue
    // (depth 8 overall) could theoretically be full of power commands.
    if (!ctx->dispatcher->enqueue_macro(out.submit.record.command_id)) {
        Guard g(ctx->engine_mutex);
        ctx->engine->fail_dispatch(out.submit.record.command_id, "dispatch_error");
        ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Error, "dispatch_failed",
                        out.submit.record.command_id.c_str(), nullptr, nullptr, nullptr);
    }
    return out;
}

void run_macro(AppContext* ctx, const char* command_id) {
    mcco::Macro macro;
    uint64_t requested_at = 0;
    {
        Guard g(ctx->engine_mutex);
        const mcco::CommandRecord* rec = ctx->engine->get(command_id);
        if (!rec || rec->state != mcco::CommandState::Accepted) return;
        requested_at = rec->requested_at;

        // Dispatch-delay bound (spec 10.3.1): a command still queued 30 s
        // after acceptance fails without any HID report.
        if (ctx->clock->epoch_seconds() > requested_at + kDispatchDelayBoundS) {
            ctx->engine->fail_dispatch(command_id, "dispatch_delayed");
            ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Warn, "dispatch_delayed",
                            command_id, nullptr, nullptr, nullptr);
            return;
        }

        JsonDocument pmd;
        if (deserializeJson(pmd, rec->parameters_json)) {
            ctx->engine->fail_dispatch(command_id, "dispatch_error");
            return;
        }
        JsonVariantConst mid = pmd["macro_id"];
        if (!mid.is<const char*>()) {
            ctx->engine->fail_dispatch(command_id, "dispatch_error");
            return;
        }
        const mcco::Macro* m = ctx->macros ? ctx->macros->get(mid.as<const char*>()) : nullptr;
        if (!m) {
            // Deleted between accept and dequeue: the latest revision is
            // gone, so the command cannot dispatch (spec 10.3.1).
            ctx->engine->complete_dispatch(command_id, false); // failed/dispatch_error
            ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Warn,
                            "macro_missing_at_dispatch", command_id, nullptr, nullptr, nullptr);
            return;
        }
        macro = *m; // copy by value: the store may be mutated while we interpret

        // Accepted -> dispatched only (spec 10.3.1): the terminal verdict
        // follows interpretation. Deadline = timeout_ms/1000 + 5.
        if (!ctx->engine->mark_dispatched(command_id, macro.timeout_ms / 1000 + 5)) return;
    }

    ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "macro_dispatched",
                    command_id, nullptr, nullptr,
                    (std::string("{\"macro_id\":\"") + macro.macro_id + "\"}").c_str());

    // Steps execute strictly in ascending `order` (spec 10.1.1).
    std::vector<mcco::MacroStep> steps = macro.steps;
    std::sort(steps.begin(), steps.end(),
              [](const mcco::MacroStep& a, const mcco::MacroStep& b) { return a.order < b.order; });

    const uint32_t start_ms = millis();
    const uint32_t budget_ms = macro.timeout_ms;

    bool wake_reconnect_tried = false;
    for (const mcco::MacroStep& step : steps) {
        // Abort conditions checked before every step (spec 10.3.1).
        if (!ctx->hid->mounted()) {
            // A sleeping host cuts USB power (usb_detached ~8 s into sleep),
            // so a dispatched-while-asleep macro finds no keyboard. Polling
            // cannot recover that; do the emulated replug ONCE (the physical
            // equivalent wakes the host via the first keystroke), then fall
            // back to the honest abort if enumeration does not come back.
            if (!wake_reconnect_tried) {
                wake_reconnect_tried = true;
                ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info,
                                "usb_wake_reconnect", command_id, nullptr, nullptr,
                                nullptr);
                if (ctx->hid->reconnectForUserWake(15000)) continue;
            }
            abort_macro(ctx, command_id, "usb_disconnected");
            return;
        }
        if (millis() - start_ms > budget_ms) {
            abort_macro(ctx, command_id, "macro_timeout");
            return;
        }

        switch (step.type) {
            case mcco::StepType::KeyPress: {
                const uint8_t code = mcco::hid_key_code(step.key.c_str());
                ctx->hid->keyDown(code);
                vTaskDelay(pdMS_TO_TICKS(kReportGapMs));
                ctx->hid->keyUp(code);
                vTaskDelay(pdMS_TO_TICKS(kReportGapMs));
                break;
            }
            case mcco::StepType::KeyCombo: {
                for (const std::string& mod : step.modifiers) {
                    ctx->hid->keyDown(HidKeyboard::modifierUsage(mod.c_str()));
                }
                const uint8_t code = mcco::hid_key_code(step.key.c_str());
                ctx->hid->keyDown(code);
                vTaskDelay(pdMS_TO_TICKS(kReportGapMs));
                // key_combo releases everything it asserted (spec 10.1.1).
                ctx->hid->allKeysUp();
                vTaskDelay(pdMS_TO_TICKS(kReportGapMs));
                break;
            }
            case mcco::StepType::ModifierDown:
                for (const std::string& mod : step.modifiers) {
                    ctx->hid->keyDown(HidKeyboard::modifierUsage(mod.c_str()));
                }
                break;
            case mcco::StepType::ModifierUp:
                for (const std::string& mod : step.modifiers) {
                    ctx->hid->keyUp(HidKeyboard::modifierUsage(mod.c_str()));
                }
                break;
            case mcco::StepType::KeyRelease:
                ctx->hid->keyUp(mcco::hid_key_code(step.key.c_str()));
                vTaskDelay(pdMS_TO_TICKS(kReportGapMs));
                break;
            case mcco::StepType::Text:
                ctx->hid->typeText(step.value.c_str(), step.value.size(), kInterKeyMs);
                break;
            case mcco::StepType::Delay:
                // Scheduler-based; worst-case deviation well under the 5 ms
                // bound (FreeRTOS runs at 1000 Hz on this core).
                vTaskDelay(pdMS_TO_TICKS(step.delay_ms));
                break;
        }
        esp_task_wdt_reset();
    }

    // Success: release any residue and close the verdict. In Mode B a macro
    // that declares an expected_event stays `confirming` until a matching
    // ambient event completes it (macro_confirmed); every other macro ends
    // honestly unconfirmed/hid_only (spec 10.3.1).
    ctx->hid->allKeysUp();
    mcco::CommandState st = mcco::CommandState::Failed;
    {
        Guard g(ctx->engine_mutex);
        ctx->engine->macro_interpret_done(command_id);
        const mcco::CommandRecord* rec = ctx->engine->get(command_id);
        if (rec) st = rec->state;
    }
    ctx->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "macro_terminal",
                    command_id, nullptr, nullptr,
                    st == mcco::CommandState::Confirming ? "{\"state\":\"confirming\"}"
                                                         : "{\"state\":\"unconfirmed\"}");
}
