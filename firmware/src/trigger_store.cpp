#include "trigger_store.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "log_sink.h"
#include "mc_log.h"
#include "mc_macro.h"
#include "macro_runner.h"

std::string trigger_to_json_pub(const Trigger& t) {
    JsonDocument doc;
    doc["trigger_id"] = t.trigger_id;
    doc["source"] = t.source;
    if (t.source == "gpio") {
        doc["gpio_pin"] = t.gpio_pin;
        doc["edge"] = t.edge;
        doc["debounce_ms"] = t.debounce_ms;
    }
    doc["macro_id"] = t.macro_id;
    doc["enabled"] = t.enabled;
    std::string out;
    serializeJson(doc, out);
    return out;
}

namespace {

struct GpioEvent {
    int pin;
    int level;
};

// Per-pin ISR trampoline contexts (attachInterruptArg arg). One store exists
// per firmware, so file-static storage is fine.
struct PinCtx {
    TriggerStore* self;
    uint8_t pin;
};
PinCtx g_pin_ctx[22];

std::string trigger_to_json(const Trigger& t) { return trigger_to_json_pub(t); }

bool trigger_from_json(const std::string& line, Trigger& t) {
    JsonDocument doc;
    if (deserializeJson(doc, line) || !doc.is<JsonObject>()) return false;
    JsonVariantConst v;
    v = doc["trigger_id"];
    if (!v.is<const char*>()) return false;
    t.trigger_id = v.as<const char*>();
    v = doc["source"];
    if (!v.is<const char*>()) return false;
    t.source = v.as<const char*>();
    if (t.source != "webui_button" && t.source != "gpio") return false;
    t.gpio_pin = -1;
    t.edge = "falling";
    t.debounce_ms = 50;
    if (t.source == "gpio") {
        v = doc["gpio_pin"];
        if (!v.is<int>()) return false;
        t.gpio_pin = v.as<int>();
        if (t.gpio_pin < 0 || t.gpio_pin > 21) return false;
        v = doc["edge"];
        if (!v.is<const char*>()) return false;
        t.edge = v.as<const char*>();
        if (t.edge != "falling" && t.edge != "rising") return false;
        v = doc["debounce_ms"];
        if (!v.is<uint32_t>()) return false;
        t.debounce_ms = v.as<uint32_t>();
        if (t.debounce_ms < 10 || t.debounce_ms > 500) return false;
    }
    v = doc["macro_id"];
    if (!v.is<const char*>()) return false;
    t.macro_id = v.as<const char*>();
    v = doc["enabled"];
    if (!v.is<bool>()) return false;
    t.enabled = v.as<bool>();
    return !t.trigger_id.empty() && !t.macro_id.empty();
}

} // namespace

bool TriggerStore::begin(AppContext* ctx) {
    ctx_ = ctx;
    for (uint8_t pin = 0; pin <= 21; pin++) {
        g_pin_ctx[pin].self = this;
        g_pin_ctx[pin].pin = pin;
    }
    gpio_queue_ = xQueueCreate(16, sizeof(GpioEvent));
    if (!gpio_queue_) return false;
    if (xTaskCreate([](void* arg) { static_cast<TriggerStore*>(arg)->taskLoop(); },
                    "mc_trig", 3072, this, 5, &task_) != pdPASS)
        return false;
    applyGpio();
    return true;
}

bool TriggerStore::validateAndFill(const std::string& json, Trigger& t, mcco::ErrCode& err,
                                   bool for_update, const std::string& keep_id) {
    JsonDocument doc;
    if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
        err = mcco::ErrCode::BadRequest;
        return false;
    }
    // Closed field set (spec 10.2.1 + unknown-field rejection).
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "source") != 0 && strcmp(k, "gpio_pin") != 0 &&
            strcmp(k, "edge") != 0 && strcmp(k, "debounce_ms") != 0 &&
            strcmp(k, "macro_id") != 0 && strcmp(k, "enabled") != 0 &&
            strcmp(k, "trigger_id") != 0) {
            err = mcco::ErrCode::BadRequest;
            return false;
        }
    }
    JsonVariantConst v;

    v = doc["source"];
    if (!v.is<const char*>()) {
        err = mcco::ErrCode::BadRequest;
        return false;
    }
    t.source = v.as<const char*>();
    // http_button is the implicit execute endpoint: no stored row (spec 10.2.1).
    if (t.source != "webui_button" && t.source != "gpio") {
        err = mcco::ErrCode::BadRequest;
        return false;
    }

    t.gpio_pin = -1;
    t.edge = "falling";
    t.debounce_ms = 50;
    if (t.source == "gpio") {
        v = doc["gpio_pin"];
        if (!v.is<int>()) {
            err = mcco::ErrCode::BadRequest;
            return false;
        }
        t.gpio_pin = v.as<int>();
        if (t.gpio_pin < 0 || t.gpio_pin > 21) {
            err = mcco::ErrCode::BadRequest;
            return false;
        }
        v = doc["edge"];
        if (v.isNull()) {
            // default falling
        } else if (v.is<const char*>() &&
                   (strcmp(v.as<const char*>(), "falling") == 0 ||
                    strcmp(v.as<const char*>(), "rising") == 0)) {
            t.edge = v.as<const char*>();
        } else {
            err = mcco::ErrCode::BadRequest;
            return false;
        }
        v = doc["debounce_ms"];
        if (!v.isNull()) {
            if (!v.is<uint32_t>()) {
                err = mcco::ErrCode::BadRequest;
                return false;
            }
            t.debounce_ms = v.as<uint32_t>();
            if (t.debounce_ms < 10 || t.debounce_ms > 500) {
                err = mcco::ErrCode::BadRequest;
                return false;
            }
        }
    } else {
        // webui_button rows carry no pin/edge/debounce.
        if (!doc["gpio_pin"].isNull() || !doc["edge"].isNull() ||
            !doc["debounce_ms"].isNull()) {
            err = mcco::ErrCode::BadRequest;
            return false;
        }
    }

    v = doc["macro_id"];
    if (!v.is<const char*>()) {
        err = mcco::ErrCode::BadRequest;
        return false;
    }
    t.macro_id = v.as<const char*>();
    {
        Guard g(ctx_->engine_mutex);
        if (!ctx_->macros || !ctx_->macros->get(t.macro_id)) {
            err = mcco::ErrCode::NotFound; // macro must exist (spec 10.2.1)
            return false;
        }
    }

    v = doc["enabled"];
    t.enabled = v.isNull() ? true : (v.is<bool>() && v.as<bool>());
    if (!v.isNull() && !v.is<bool>()) {
        err = mcco::ErrCode::BadRequest;
        return false;
    }

    if (for_update) t.trigger_id = keep_id;
    return true;
}

bool TriggerStore::add(const std::string& json, Trigger& out, mcco::ErrCode& err) {
    Trigger t;
    if (!validateAndFill(json, t, err, false, "")) return false;
    Guard g(mutex_);
    uint32_t next = 1;
    for (const Trigger& e : triggers_) {
        uint32_t n = 0;
        if (sscanf(e.trigger_id.c_str(), "trg_%u", &n) == 1 && n >= next) next = n + 1;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "trg_%02u", next);
    t.trigger_id = buf;
    triggers_.push_back(t);
    out = t;
    applyGpio();
    return true;
}

bool TriggerStore::update(const std::string& trigger_id, const std::string& json, Trigger& out,
                          mcco::ErrCode& err) {
    Guard g(mutex_);
    for (Trigger& e : triggers_) {
        if (e.trigger_id != trigger_id) continue;
        Trigger t;
        if (!validateAndFill(json, t, err, true, trigger_id)) return false;
        e = t;
        out = e;
        applyGpio();
        return true;
    }
    err = mcco::ErrCode::NotFound;
    return false;
}

bool TriggerStore::remove(const std::string& trigger_id) {
    Guard g(mutex_);
    for (auto it = triggers_.begin(); it != triggers_.end(); ++it) {
        if (it->trigger_id == trigger_id) {
            triggers_.erase(it);
            applyGpio();
            return true;
        }
    }
    return false;
}

const Trigger* TriggerStore::get(const std::string& trigger_id) const {
    Guard g(mutex_);
    for (const Trigger& t : triggers_) {
        if (t.trigger_id == trigger_id) return &t;
    }
    return nullptr;
}

std::vector<const Trigger*> TriggerStore::list() const {
    Guard g(mutex_);
    std::vector<const Trigger*> out;
    out.reserve(triggers_.size());
    for (const Trigger& t : triggers_) out.push_back(&t);
    std::sort(out.begin(), out.end(),
              [](const Trigger* a, const Trigger* b) { return a->trigger_id < b->trigger_id; });
    return out;
}

std::vector<std::string> TriggerStore::onMacroDeleted(const std::string& macro_id) {
    std::vector<std::string> disabled;
    Guard g(mutex_);
    for (Trigger& t : triggers_) {
        if (t.macro_id == macro_id && t.enabled) {
            t.enabled = false;
            disabled.push_back(t.trigger_id);
        }
    }
    if (!disabled.empty()) applyGpio();
    return disabled;
}

std::vector<std::string> TriggerStore::dump() const {
    Guard g(mutex_);
    std::vector<std::string> out;
    out.reserve(triggers_.size());
    for (const Trigger& t : triggers_) out.push_back(trigger_to_json(t));
    return out;
}

size_t TriggerStore::load(const std::vector<std::string>& lines) {
    size_t skipped = 0;
    Guard g(mutex_);
    triggers_.clear();
    for (const std::string& line : lines) {
        Trigger t;
        if (trigger_from_json(line, t)) {
            triggers_.push_back(std::move(t));
        } else {
            ++skipped;
        }
    }
    return skipped;
}

void TriggerStore::applyGpio() {
    // Caller holds mutex_. Detach pins we attached previously, then attach
    // the enabled gpio bindings. The store is the single authority on these
    // pins. (detachInterrupt before the ISR service exists only logs an IDF
    // error, but skipping it keeps the boot log clean.)
    for (int pin = 0; pin <= 21; pin++) {
        if (attached_mask_ & (1u << pin)) detachInterrupt(pin);
    }
    attached_mask_ = 0;
    for (const Trigger& t : triggers_) {
        if (t.source != "gpio" || !t.enabled) continue;
        pinMode(t.gpio_pin, INPUT_PULLUP);
        const int mode = (t.edge == "falling") ? FALLING : RISING;
        attachInterruptArg(t.gpio_pin, &TriggerStore::isr, &g_pin_ctx[t.gpio_pin], mode);
        attached_mask_ |= (1u << t.gpio_pin);
    }
}

void IRAM_ATTR TriggerStore::isr(void* arg) {
    const PinCtx* pc = static_cast<const PinCtx*>(arg);
    GpioEvent ev;
    ev.pin = pc->pin;
    ev.level = gpio_get_level((gpio_num_t)pc->pin);
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(pc->self->gpio_queue_, &ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void TriggerStore::taskLoop() {
    GpioEvent ev;
    for (;;) {
        if (xQueueReceive(gpio_queue_, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        if (ev.pin < 0 || ev.pin > 21) continue;
        fireGpio(ev.pin, ev.level);
    }
}

void TriggerStore::fireGpio(int pin, int level) {
    std::string macro_id;
    std::string trigger_id;
    uint32_t debounce_ms = 50;
    {
        Guard g(mutex_);
        for (const Trigger& t : triggers_) {
            if (t.source != "gpio" || !t.enabled || t.gpio_pin != pin) continue;
            const bool matches = (t.edge == "falling" && level == 0) ||
                                 (t.edge == "rising" && level == 1);
            if (!matches) continue;
            const uint32_t now = millis();
            if (now - last_fire_ms_[pin] < t.debounce_ms) return; // debounced
            last_fire_ms_[pin] = now;
            macro_id = t.macro_id;
            trigger_id = t.trigger_id;
            break;
        }
    }
    if (macro_id.empty()) return;

    char requested_by[24];
    snprintf(requested_by, sizeof(requested_by), "gpio:pin%d", pin);
    MacroExecOutcome out = submit_macro_execute(ctx_, macro_id, requested_by);
    if (!out.ok && out.error == mcco::ErrCode::MacroQueueFull) {
        // Spec 10.3.1: a GPIO event against a full queue is dropped and
        // logged with the would-be macro_id. No privileged local path.
        ctx_->log->write(mcco::LogCategory::Command, mcco::LogLevel::Warn, "gpio_trigger_dropped",
                         nullptr, nullptr, nullptr,
                         (std::string("{\"trigger_id\":\"") + trigger_id +
                          "\",\"macro_id\":\"" + macro_id + "\"}")
                             .c_str());
        return;
    }
    if (out.ok) {
        ctx_->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "gpio_trigger_fired",
                         out.submit.record.command_id.c_str(), nullptr, requested_by, nullptr);
        // Storm guard: count recent accepted fires on this pin; a runaway
        // (floating/noisy input) auto-disables the binding and demands an
        // ADMIN re-enable instead of hammering the ledger/flash.
        const uint32_t now = millis();
        uint32_t* ring = fire_ring_[pin];
        ring[fire_ring_n_[pin] % kStormFires] = now;
        fire_ring_n_[pin]++;
        int recent = 0;
        for (uint8_t i = 0; i < kStormFires && i < fire_ring_n_[pin]; i++)
            if (now - ring[i] < kStormWindowMs) recent++;
        if (recent >= kStormFires) {
            fire_ring_n_[pin] = 0; // fresh start if an ADMIN re-enables
            Guard g(mutex_);
            for (Trigger& t : triggers_) {
                if (t.source == "gpio" && t.enabled && t.gpio_pin == pin) {
                    t.enabled = false;
                    ctx_->log->write(mcco::LogCategory::System, mcco::LogLevel::Warn,
                                     "gpio_trigger_storm_disabled", nullptr, nullptr, nullptr,
                                     (std::string("{\"trigger_id\":\"") + t.trigger_id +
                                      "\",\"pin\":" + std::to_string(pin) + "}")
                                         .c_str());
                    break;
                }
            }
            applyGpio();
        }
    }
}
