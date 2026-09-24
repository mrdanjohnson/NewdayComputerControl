// Macro model and store (spec 10.1, 10.3, 15.1). Pure C++17; persistence is
// opaque definition lines the glue dumps/loads.
#include "mc_macro.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>
#include <map>

namespace mcco {

// ---- USB HID key name table (spec 10.1.1) ----------------------------------
// Standard USB HID keyboard usage IDs, lowercase names. 0 is reserved as the
// "unknown" sentinel (all real usages here are >= 0x04).
static const HidKeyName kHidKeys[] = {
    {"a", 0x04}, {"b", 0x05}, {"c", 0x06}, {"d", 0x07}, {"e", 0x08}, {"f", 0x09},
    {"g", 0x0A}, {"h", 0x0B}, {"i", 0x0C}, {"j", 0x0D}, {"k", 0x0E}, {"l", 0x0F},
    {"m", 0x10}, {"n", 0x11}, {"o", 0x12}, {"p", 0x13}, {"q", 0x14}, {"r", 0x15},
    {"s", 0x16}, {"t", 0x17}, {"u", 0x18}, {"v", 0x19}, {"w", 0x1A}, {"x", 0x1B},
    {"y", 0x1C}, {"z", 0x1D},
    {"1", 0x1E}, {"2", 0x1F}, {"3", 0x20}, {"4", 0x21}, {"5", 0x22}, {"6", 0x23},
    {"7", 0x24}, {"8", 0x25}, {"9", 0x26}, {"0", 0x27},
    {"enter", 0x28},   {"esc", 0x29},      {"backspace", 0x2A}, {"tab", 0x2B},
    {"space", 0x2C},   {"minus", 0x2D},    {"equal", 0x2E},
    {"caps_lock", 0x39},
    {"f1", 0x3A},  {"f2", 0x3B},  {"f3", 0x3C},  {"f4", 0x3D},  {"f5", 0x3E},  {"f6", 0x3F},
    {"f7", 0x40},  {"f8", 0x41},  {"f9", 0x42},  {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
    {"insert", 0x49}, {"home", 0x4A}, {"page_up", 0x4B},   {"delete", 0x4C},
    {"end", 0x4D},    {"page_down", 0x4E},
    {"right", 0x4F},  {"left", 0x50}, {"down", 0x51},      {"up", 0x52},
};

const HidKeyName* hid_key_table() { return kHidKeys; }
size_t hid_key_table_size() { return sizeof(kHidKeys) / sizeof(kHidKeys[0]); }

static const HidKeyName* find_key(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < hid_key_table_size(); i++)
        if (!strcmp(kHidKeys[i].name, name)) return &kHidKeys[i];
    return nullptr;
}

bool hid_key_valid(const char* name) { return find_key(name) != nullptr; }

uint8_t hid_key_code(const char* name) {
    const HidKeyName* k = find_key(name);
    return k ? k->code : 0;
}

// ---- Step types and modifiers (spec 10.1.1) --------------------------------

static const struct {
    const char* name;
    StepType type;
} kStepTypes[] = {
    {"key_press", StepType::KeyPress},       {"key_combo", StepType::KeyCombo},
    {"modifier_down", StepType::ModifierDown}, {"modifier_up", StepType::ModifierUp},
    {"key_release", StepType::KeyRelease},   {"text", StepType::Text},
    {"delay", StepType::Delay},
};

bool step_type_from_string(const char* s, StepType& out) {
    if (!s) return false;
    for (const auto& e : kStepTypes)
        if (!strcmp(e.name, s)) {
            out = e.type;
            return true;
        }
    return false;
}

const char* step_type_to_string(StepType t) {
    for (const auto& e : kStepTypes)
        if (e.type == t) return e.name;
    return "";
}

// Closed modifier set, USB HID report bits.
static const struct {
    const char* name;
    uint8_t bit;
} kModifiers[] = {
    {"ctrl", 0x01}, {"shift", 0x02}, {"alt", 0x04}, {"cmd", 0x08},
};

bool modifier_valid(const char* s) {
    if (!s) return false;
    for (const auto& m : kModifiers)
        if (!strcmp(m.name, s)) return true;
    return false;
}

uint8_t modifier_hid_mask(const char* s) {
    for (const auto& m : kModifiers)
        if (s && !strcmp(m.name, s)) return m.bit;
    return 0;
}

// ---- Field validators --------------------------------------------------------

static bool printable_ascii(const std::string& s, size_t min_len, size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) return false;
    for (char c : s)
        if (uint8_t(c) < 0x20 || uint8_t(c) > 0x7E) return false;
    return true;
}

// Per-type content check for one parsed step. Assumes order contiguity is
// checked separately. modifier_down/modifier_up carry their single modifier
// in `modifiers` (one element) — the MacroStep model has no dedicated field.
static bool step_content_valid(const MacroStep& st) {
    switch (st.type) {
        case StepType::KeyPress:
        case StepType::KeyRelease:
            return !st.key.empty() && hid_key_valid(st.key.c_str());
        case StepType::KeyCombo: {
            if (st.key.empty() || !hid_key_valid(st.key.c_str())) return false;
            uint8_t mask = 0;
            for (const std::string& mod : st.modifiers) {
                if (!modifier_valid(mod.c_str())) return false;
                uint8_t bit = modifier_hid_mask(mod.c_str());
                if (mask & bit) return false; // duplicates
                mask |= bit;
            }
            return true;
        }
        case StepType::ModifierDown:
        case StepType::ModifierUp:
            return st.modifiers.size() == 1 && modifier_valid(st.modifiers[0].c_str());
        case StepType::Text:
            return printable_ascii(st.value, 1, 256);
        case StepType::Delay:
            return st.delay_ms >= 10 && st.delay_ms <= 5000;
    }
    return false;
}

static bool steps_valid(const std::vector<MacroStep>& steps) {
    if (steps.empty() || steps.size() > 64) return false;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i].order != i + 1) return false; // contiguous ascending 1..n
        if (!step_content_valid(steps[i])) return false;
    }
    return true;
}

// The five ambient evidence types permitted as macro verification events
// (spec 10.1.1). command_ack/command_result/heartbeat/agent_hello/
// agent_goodbye/capability_report are explicitly forbidden and anything else
// is unknown — all reject as InvalidExpectedEvent.
static bool expected_event_type_valid(const char* s) {
    if (!s) return false;
    static const char* kAllowed[] = {"application_started",     "application_exited",
                                     "system_state_changed",    "user_session_changed",
                                     "screen_lock_changed"};
    for (const char* a : kAllowed)
        if (!strcmp(s, a)) return true;
    return false;
}

// ---- JSON parsing ------------------------------------------------------------

// Fills name/timeout_ms/expected_event/steps from a JSON object. Only those
// four keys are accepted; any other top-level key is rejected as
// MacroError::InvalidStep. This is deliberate: the HTTP layer runs its own
// bad_field check and never forwards unknown fields, so a document reaching
// the core with extra keys is treated like any other malformed step payload,
// mapped by macro_error_code() to 400.
static bool fill_from_object(JsonObjectConst o, Macro& m, MacroError& err) {
    bool has_name = false, has_steps = false;
    for (JsonPairConst kv : o) {
        const char* key = kv.key().c_str();
        JsonVariantConst v = kv.value();
        if (!strcmp(key, "name")) {
            if (!v.is<const char*>()) {
                err = MacroError::InvalidName;
                return false;
            }
            m.name = v.as<const char*>();
            has_name = true;
        } else if (!strcmp(key, "timeout_ms")) {
            if (!v.is<long>()) {
                err = MacroError::InvalidTimeout;
                return false;
            }
            long t = v.as<long>();
            if (t < 500 || t > 60000) {
                err = MacroError::InvalidTimeout;
                return false;
            }
            m.timeout_ms = uint32_t(t);
        } else if (!strcmp(key, "expected_event")) {
            if (!v.is<JsonObjectConst>()) {
                err = MacroError::InvalidExpectedEvent;
                return false;
            }
            JsonObjectConst ev = v.as<JsonObjectConst>();
            JsonVariantConst type = ev["type"];
            if (!type.is<const char*>() || !expected_event_type_valid(type.as<const char*>())) {
                err = MacroError::InvalidExpectedEvent;
                return false;
            }
            JsonVariantConst match = ev["match"];
            if (!match.is<JsonObjectConst>()) {
                err = MacroError::InvalidExpectedEvent;
                return false;
            }
            std::string compact;
            serializeJson(match, compact);
            m.has_expected_event = true;
            m.expected_event.type = type.as<const char*>();
            m.expected_event.match_json = compact;
        } else if (!strcmp(key, "steps")) {
            if (!v.is<JsonArrayConst>()) {
                err = MacroError::InvalidStep;
                return false;
            }
            JsonArrayConst arr = v.as<JsonArrayConst>();
            if (arr.size() < 1 || arr.size() > 64) {
                err = MacroError::InvalidStep;
                return false;
            }
            uint8_t next_order = 1;
            for (JsonVariantConst el : arr) {
                if (!el.is<JsonObjectConst>()) {
                    err = MacroError::InvalidStep;
                    return false;
                }
                JsonObjectConst so = el.as<JsonObjectConst>();
                MacroStep st;
                bool has_order = false, has_type = false;
                bool has_key = false, has_modifiers = false, has_modifier = false;
                bool has_value = false, has_delay = false;
                for (JsonPairConst skv : so) {
                    const char* skey = skv.key().c_str();
                    JsonVariantConst sv = skv.value();
                    if (!strcmp(skey, "order")) {
                        if (!sv.is<long>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        long ord = sv.as<long>();
                        // Strictly ascending and contiguous: 1, 2, 3, ...
                        if (ord != next_order) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        st.order = uint8_t(ord);
                        has_order = true;
                        next_order++;
                    } else if (!strcmp(skey, "type")) {
                        if (!sv.is<const char*>() ||
                            !step_type_from_string(sv.as<const char*>(), st.type)) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        has_type = true;
                    } else if (!strcmp(skey, "key")) {
                        if (!sv.is<const char*>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        st.key = sv.as<const char*>();
                        has_key = true;
                    } else if (!strcmp(skey, "modifiers")) {
                        if (!sv.is<JsonArrayConst>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        for (JsonVariantConst mv : sv.as<JsonArrayConst>()) {
                            if (!mv.is<const char*>()) {
                                err = MacroError::InvalidStep;
                                return false;
                            }
                            st.modifiers.emplace_back(mv.as<const char*>());
                        }
                        has_modifiers = true;
                    } else if (!strcmp(skey, "modifier")) {
                        if (!sv.is<const char*>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        st.modifiers = {sv.as<const char*>()};
                        has_modifier = true;
                    } else if (!strcmp(skey, "value")) {
                        if (!sv.is<const char*>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        st.value = sv.as<const char*>();
                        has_value = true;
                    } else if (!strcmp(skey, "delay_ms")) {
                        if (!sv.is<long>()) {
                            err = MacroError::InvalidStep;
                            return false;
                        }
                        st.delay_ms = uint32_t(sv.as<long>());
                        has_delay = true;
                    } else {
                        // Unknown key inside a step.
                        err = MacroError::InvalidStep;
                        return false;
                    }
                }
                if (!has_order || !has_type) {
                    err = MacroError::InvalidStep;
                    return false;
                }
                bool fields_ok = false;
                switch (st.type) {
                    case StepType::KeyPress:
                    case StepType::KeyRelease:
                        fields_ok = has_key && step_content_valid(st);
                        break;
                    case StepType::KeyCombo:
                        fields_ok = has_key && has_modifiers && step_content_valid(st);
                        break;
                    case StepType::ModifierDown:
                    case StepType::ModifierUp:
                        // One modifier, accepted either as the singular
                        // `modifier` alias or a one-element `modifiers`
                        // array (the Web UI editor sends the latter).
                        fields_ok = (has_modifier || has_modifiers) &&
                                    step_content_valid(st);
                        break;
                    case StepType::Text:
                        fields_ok = has_value && step_content_valid(st);
                        break;
                    case StepType::Delay:
                        fields_ok = has_delay && step_content_valid(st);
                        break;
                }
                if (!fields_ok) {
                    err = MacroError::InvalidStep;
                    return false;
                }
                m.steps.push_back(std::move(st));
            }
            has_steps = true;
        } else {
            // Unknown top-level field (see fill_from_object doc comment).
            err = MacroError::InvalidStep;
            return false;
        }
    }
    if (!has_name || !printable_ascii(m.name, 1, 32)) {
        err = MacroError::InvalidName;
        return false;
    }
    if (!has_steps) {
        err = MacroError::InvalidStep;
        return false;
    }
    return true;
}

bool macro_from_json(const std::string& json, Macro& out, MacroError& err) {
    err = MacroError::Ok;
    JsonDocument doc;
    if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
        err = MacroError::InvalidStep;
        return false;
    }
    Macro m; // macro_id/revision stay default; they are endpoint-assigned
    if (!fill_from_object(doc.as<JsonObjectConst>(), m, err)) return false;
    out = std::move(m);
    return true;
}

std::string macro_to_json(const Macro& m) {
    JsonDocument doc;
    doc["macro_id"] = m.macro_id;
    doc["revision"] = static_cast<unsigned>(m.revision);
    doc["name"] = m.name;
    doc["timeout_ms"] = static_cast<unsigned>(m.timeout_ms);
    if (m.has_expected_event) {
        JsonObject ev = doc["expected_event"].to<JsonObject>();
        ev["type"] = m.expected_event.type;
        // match_json is already compact text (enforced at parse time).
        ev["match"] = serialized(m.expected_event.match_json);
    }
    JsonArray steps = doc["steps"].to<JsonArray>();
    for (const MacroStep& st : m.steps) {
        JsonObject s = steps.add<JsonObject>();
        s["order"] = static_cast<unsigned>(st.order);
        s["type"] = step_type_to_string(st.type);
        s["key"] = st.key;
        JsonArray mods = s["modifiers"].to<JsonArray>();
        for (const std::string& mod : st.modifiers) mods.add(mod);
        s["value"] = st.value;
        s["delay_ms"] = static_cast<unsigned>(st.delay_ms);
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

// ---- Error mapping (spec 4.1.1, 10, 15.1) -----------------------------------

ErrCode macro_error_code(MacroError e) {
    switch (e) {
        case MacroError::InvalidStep:
        case MacroError::InvalidExpectedEvent:
            return ErrCode::MacroInvalidStep;
        case MacroError::InvalidName:
        case MacroError::DuplicateName:
        case MacroError::InvalidTimeout:
        case MacroError::StoreFull:
            return ErrCode::BadRequest;
        case MacroError::NotFound:
            return ErrCode::NotFound;
        case MacroError::Ok:
            break;
    }
    return ErrCode::InternalError; // Ok has no HTTP mapping; misuse signal
}

const char* macro_error_detail(MacroError e) {
    switch (e) {
        case MacroError::Ok: return "ok";
        case MacroError::InvalidStep: return "invalid macro step";
        case MacroError::InvalidName: return "invalid macro name";
        case MacroError::DuplicateName: return "duplicate macro name";
        case MacroError::InvalidTimeout: return "invalid timeout_ms";
        case MacroError::InvalidExpectedEvent: return "invalid expected_event";
        case MacroError::StoreFull: return "macro store full";
        case MacroError::NotFound: return "macro not found";
    }
    return "unknown";
}

// ---- Store -------------------------------------------------------------------

static std::string make_macro_id(IRandom& rng) {
    uint8_t b[2];
    rng.bytes(b, sizeof(b));
    char buf[9];
    snprintf(buf, sizeof(buf), "mac_%02X%02X", b[0], b[1]);
    return buf;
}

bool MacroStore::validate(const Macro& m, MacroError& err, bool for_update) const {
    if (!printable_ascii(m.name, 1, 32)) {
        err = MacroError::InvalidName;
        return false;
    }
    if (m.timeout_ms < 500 || m.timeout_ms > 60000) {
        err = MacroError::InvalidTimeout;
        return false;
    }
    if (!steps_valid(m.steps)) {
        err = MacroError::InvalidStep;
        return false;
    }
    if (m.has_expected_event) {
        if (!expected_event_type_valid(m.expected_event.type.c_str()) ||
            m.expected_event.match_json.empty()) {
            err = MacroError::InvalidExpectedEvent;
            return false;
        }
    }
    // Names are unique per endpoint; on update the macro's own name is fine.
    for (const Macro& x : macros_) {
        if (x.name == m.name && (!for_update || x.macro_id != m.macro_id)) {
            err = MacroError::DuplicateName;
            return false;
        }
    }
    err = MacroError::Ok;
    return true;
}

bool MacroStore::add(const Macro& def, Macro& out, MacroError& err, IRandom& rng) {
    if (!validate(def, err, false)) return false;
    if (macros_.size() >= kCapacity) {
        err = MacroError::StoreFull;
        return false;
    }
    Macro m = def;
    // "mac_" + 4 uppercase hex from two rng bytes; redraw on collision.
    for (int attempt = 0; attempt < 16; attempt++) {
        m.macro_id = make_macro_id(rng);
        if (!get(m.macro_id)) break;
    }
    if (get(m.macro_id)) {
        err = MacroError::StoreFull;
        return false;
    }
    m.revision = 1;
    macros_.push_back(m);
    out = m;
    return true;
}

bool MacroStore::update(const std::string& macro_id, const Macro& def, MacroError& err) {
    for (size_t i = 0; i < macros_.size(); i++) {
        if (macros_[i].macro_id != macro_id) continue;
        Macro m = def;
        m.macro_id = macro_id;
        if (!validate(m, err, true)) return false;
        m.revision = macros_[i].revision + 1;
        macros_[i] = m;
        return true;
    }
    err = MacroError::NotFound;
    return false;
}

bool MacroStore::remove(const std::string& macro_id) {
    for (size_t i = 0; i < macros_.size(); i++) {
        if (macros_[i].macro_id == macro_id) {
            macros_.erase(macros_.begin() + i);
            return true;
        }
    }
    return false;
}

const Macro* MacroStore::get(const std::string& macro_id) const {
    for (const Macro& m : macros_)
        if (m.macro_id == macro_id) return &m;
    return nullptr;
}

const Macro* MacroStore::find_by_name(const std::string& name) const {
    for (const Macro& m : macros_)
        if (m.name == name) return &m;
    return nullptr;
}

std::vector<const Macro*> MacroStore::list() const {
    std::vector<const Macro*> out;
    out.reserve(macros_.size());
    for (const Macro& m : macros_) out.push_back(&m);
    std::sort(out.begin(), out.end(), [](const Macro* a, const Macro* b) {
        return a->macro_id < b->macro_id;
    });
    return out;
}

std::vector<std::string> MacroStore::dump() const {
    std::vector<std::string> out;
    out.reserve(macros_.size());
    for (const Macro* m : list()) out.push_back(macro_to_json(*m));
    return out;
}

size_t MacroStore::load(const std::vector<std::string>& json_lines) {
    macros_.clear();
    size_t skipped = 0;
    std::map<std::string, Macro> by_id; // keeps insertion slot; overwritten on dup
    for (const std::string& line : json_lines) {
        JsonDocument doc;
        if (deserializeJson(doc, line) || !doc.is<JsonObject>()) {
            skipped++;
            continue;
        }
        JsonObjectConst o = doc.as<JsonObjectConst>();
        JsonVariantConst id = o["macro_id"];
        JsonVariantConst rev = o["revision"];
        if (!id.is<const char*>() || id.as<const char*>()[0] == '\0' || !rev.is<long>() ||
            rev.as<long>() < 1) {
            skipped++;
            continue;
        }
        Macro m;
        m.macro_id = id.as<const char*>();
        m.revision = uint32_t(rev.as<long>());
        // Strip the endpoint-assigned fields before field validation: the
        // input-document whitelist (name/timeout_ms/expected_event/steps)
        // rightfully rejects macro_id/revision.
        JsonDocument filtered;
        for (const char* key : {"name", "timeout_ms", "expected_event", "steps"})
            if (o[key]) filtered[key] = o[key];
        MacroError err = MacroError::Ok;
        // Field-level validation only here: capacity and duplicate names are
        // store-level concerns; a line that is well-formed but invalid content
        // is corrupt and skipped.
        if (!fill_from_object(filtered.as<JsonObjectConst>(), m, err) || err != MacroError::Ok) {
            skipped++;
            continue;
        }
        by_id[m.macro_id] = std::move(m);
    }
    for (auto& kv : by_id) macros_.push_back(std::move(kv.second));
    return skipped;
}

} // namespace mcco
