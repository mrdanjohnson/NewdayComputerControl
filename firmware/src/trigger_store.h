#pragma once
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdint>
#include <string>
#include <vector>
#include "app_context.h"
#include "mc_error.h"
#include "mc_mutex.h"

// Trigger bindings (spec 10.2.1): ESP32-owned mappings from a webui_button or
// gpio source to exactly one macro_id. http_button is implicit (no stored
// row). Persistence is a JSON-lines dump through the ConfigStore double-slot
// machinery; the store maps skipped/malformed lines to load corruption.
//
// GPIO bindings: pin configured as input with pullup per edge, ISR queues the
// edge to a small task which debounces (debounce_ms) and fires the macro
// through the same pre-ledger queue gate as HTTP invocations. A full queue
// drops the event with a log line naming the would-be macro_id (spec 10.3.1).
//
// Storm guard: a physical input is an unauthenticated actuation surface, so a
// pin that fires more than kStormFires times within kStormWindowMs is treated
// as faulty (floating/noisy wiring) and its binding is AUTO-DISABLED with a
// warn log — it never silently hammers the ledger or flash wear. Re-enable
// requires an ADMIN write, which surfaces the fault to an operator.
static constexpr int kStormFires = 8;       // accepted fires...
static constexpr uint32_t kStormWindowMs = 10000; // ...within this window
struct Trigger {
    std::string trigger_id; // "trg_NN", ESP32-assigned
    std::string source;     // "webui_button" | "gpio"
    int gpio_pin = -1;      // gpio only, 0..21
    std::string edge;       // gpio only: "falling" | "rising"
    uint32_t debounce_ms = 50;
    std::string macro_id;
    bool enabled = true;
};

// Wire serialization of one trigger (spec 10.2.1 document shape).
std::string trigger_to_json_pub(const Trigger& t);

class TriggerStore {
public:
    bool begin(AppContext* ctx);

    // CRUD. Input documents carry the spec 10.2.1 fields WITHOUT trigger_id.
    // Returns false and fills err (an mcco::ErrCode) on rejection; the HTTP
    // layer maps that to the error envelope. Validation: closed field set,
    // source enum, gpio pin 0..21, edge enum, debounce 10..500, macro must
    // exist (NotFound), and http_button rows are rejected (implicit source).
    bool add(const std::string& json, Trigger& out, mcco::ErrCode& err);
    bool update(const std::string& trigger_id, const std::string& json, Trigger& out,
                mcco::ErrCode& err);    bool remove(const std::string& trigger_id);

    const Trigger* get(const std::string& trigger_id) const;
    std::vector<const Trigger*> list() const; // by trigger_id, ascending
    size_t size() const { return triggers_.size(); }

    // Spec 10.2.1: deleting a macro auto-disables bindings that reference it.
    // Returns the disabled trigger_ids.
    std::vector<std::string> onMacroDeleted(const std::string& macro_id);

    // Persistence round-trip: dump() emits one JSON line per trigger; load()
    // returns the number of skipped malformed lines.
    std::vector<std::string> dump() const;
    size_t load(const std::vector<std::string>& lines);

    // (Re)attaches interrupts for all enabled gpio bindings; detaches pins no
    // longer in use. Called after every mutation and at boot.
    void applyGpio();

private:
    bool validateAndFill(const std::string& json, Trigger& t, mcco::ErrCode& err,
                         bool for_update, const std::string& keep_id);
    static void isr(void* arg); // arg is this
    void taskLoop();
    void fireGpio(int pin, int level);

    AppContext* ctx_ = nullptr;
    std::vector<Trigger> triggers_;
    mutable Mutex mutex_;
    QueueHandle_t gpio_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint32_t last_fire_ms_[64] = {0}; // per-pin debounce timestamps
    uint32_t attached_mask_ = 0;      // gpio pins with an interrupt attached
    // Storm-guard ring per pin: timestamps of the last kStormFires accepted
    // fires. Index 0..21 used (22..63 stay zero — small static footprint).
    uint32_t fire_ring_[64][kStormFires] = {{0}};
    uint8_t fire_ring_n_[64] = {0};
};
