#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string>
#include "app_context.h"

// FreeRTOS dispatcher: a queue (depth 8) of accepted command_ids. The HTTP
// handler enqueues after a successful 202 submit; the task pops, performs the
// USB HID dispatch (never inside the HTTP request path, spec 5.1.2), then
// reports the outcome via engine.complete_dispatch(id, ok).
class CommandDispatcher {
public:
    bool begin(AppContext* ctx);
    bool enqueue(const std::string& command_id);

    TaskHandle_t taskHandle() const { return task_; }

private:
    static void taskEntry(void* arg);

    AppContext* ctx_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    static constexpr UBaseType_t kDepth = 8;
};
