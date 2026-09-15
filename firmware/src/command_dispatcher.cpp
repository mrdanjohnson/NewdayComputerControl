#include "command_dispatcher.h"
#include <esp_task_wdt.h>
#include <string.h>
#include "hid_keyboard.h"
#include "log_sink.h"
#include "mc_engine.h"
#include "mc_log.h"
#include "mc_types.h"

namespace {
struct DispatchItem {
    char command_id[12]; // 8-char Crockford Base32 + NUL
};
} // namespace

bool CommandDispatcher::begin(AppContext* ctx) {
    ctx_ = ctx;
    queue_ = xQueueCreate(kDepth, sizeof(DispatchItem));
    if (!queue_) return false;
    if (xTaskCreate(taskEntry, "mc_dispatch", 4096, this, 6, &task_) != pdPASS) return false;
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

void CommandDispatcher::taskEntry(void* arg) {
    CommandDispatcher* self = static_cast<CommandDispatcher*>(arg);
    AppContext* ctx = self->ctx_;
    for (;;) {
        esp_task_wdt_reset();
        DispatchItem item;
        // Timed wait so the watchdog is fed even when idle.
        if (xQueueReceive(self->queue_, &item, pdMS_TO_TICKS(5000)) != pdTRUE) continue;

        mcco::CommandType type;
        {
            Guard g(ctx->engine_mutex);
            const mcco::CommandRecord* rec = ctx->engine->get(item.command_id);
            if (!rec || rec->state != mcco::CommandState::Accepted) continue;
            type = rec->type;
        }

        const bool ok = ctx->hid->send_chord(type);
        {
            Guard g(ctx->engine_mutex);
            ctx->engine->complete_dispatch(item.command_id, ok);
        }
        ctx->log->write(mcco::LogCategory::Command,
                        ok ? mcco::LogLevel::Info : mcco::LogLevel::Error,
                        ok ? "dispatch" : "dispatch_failed", item.command_id, nullptr, nullptr,
                        nullptr);
    }
}
