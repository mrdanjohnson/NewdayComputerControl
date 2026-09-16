#pragma once
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string>
#include "app_context.h"

// FreeRTOS dispatcher: a queue (depth 8) of accepted command_ids. The HTTP
// handler enqueues after a successful 202 submit; the task pops, performs the
// USB HID dispatch (never inside the HTTP request path, spec 5.1.2), then
// reports the outcome via engine.complete_dispatch(id, ok).
//
// Phase 2 (spec 10.3.1): macro_execute commands are routed to the macro
// interpreter instead of a power chord, and the pending macro count feeds the
// pre-ledger macro_queue_full gate (queue depth 4, in-flight included).
class CommandDispatcher {
public:
    bool begin(AppContext* ctx);
    bool enqueue(const std::string& command_id);
    // Macro variant: counts against the macro queue depth (spec 10.3.1).
    bool enqueue_macro(const std::string& command_id);

    // true when pending macro_execute commands + the in-flight macro >= 4.
    bool macro_queue_full() const;

    TaskHandle_t taskHandle() const { return task_; }

private:
    static void taskEntry(void* arg);

    AppContext* ctx_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<uint32_t> macro_pending_{0};  // accepted macro_execute ids in the queue
    std::atomic<bool> macro_inflight_{false}; // a macro is being interpreted now
    static constexpr UBaseType_t kDepth = 8;
    static constexpr uint32_t kMacroQueueDepth = 4; // spec 10.3.1 default
};
