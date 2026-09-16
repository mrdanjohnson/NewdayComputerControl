#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "mc_error.h"
#include "mc_random.h"

namespace mcco {

// ---- USB HID key name table (spec 10.1.1) --------------------------------
// Single source of truth for named keyboard usages: core validation and the
// firmware's HID glue both consult this. Codes are standard USB HID usage IDs
// (same numbering the Arduino Keyboard constants use).
struct HidKeyName {
    const char* name;
    uint8_t code;
};
const HidKeyName* hid_key_table();
size_t hid_key_table_size();
bool hid_key_valid(const char* name);
uint8_t hid_key_code(const char* name); // 0 if unknown

// ---- Macro model (spec 10.1) ----------------------------------------------

// Closed step-type set (spec 10.1.1).
enum class StepType : uint8_t { KeyPress, KeyCombo, ModifierDown, ModifierUp, KeyRelease, Text, Delay };
bool step_type_from_string(const char* s, StepType& out);
const char* step_type_to_string(StepType t);

// Closed modifier set {ctrl, shift, alt, cmd}.
bool modifier_valid(const char* s);
uint8_t modifier_hid_mask(const char* s); // bitmask of HID modifier bits

struct MacroStep {
    uint8_t order = 0;                     // 1..64, executed ascending
    StepType type = StepType::KeyPress;
    std::string key;                       // named usage, for key_* types
    std::vector<std::string> modifiers;    // subset of {ctrl,shift,alt,cmd}
    std::string value;                     // text steps: 1-256 ASCII
    uint32_t delay_ms = 0;                 // delay steps: 10..5000
};

struct ExpectedEvent {
    std::string type;      // one of the five ambient evidence types (spec 10.1.1)
    std::string match_json; // exact-match object over the event's payload keys
};

struct Macro {
    std::string macro_id;  // "mac_" + 4 uppercase hex, ESP32-assigned
    uint32_t revision = 1; // edits create a new revision (spec 10.1)
    std::string name;      // 1-32 printable, unique per endpoint
    uint32_t timeout_ms = 10000; // 500..60000
    bool has_expected_event = false;
    ExpectedEvent expected_event;
    std::vector<MacroStep> steps; // 1..64
};

enum class MacroError {
    Ok,
    InvalidStep,        // 400 macro_invalid_step
    InvalidName,        // 400 bad_request
    DuplicateName,      // 400 bad_request
    InvalidTimeout,     // 400 bad_request
    InvalidExpectedEvent, // 400 macro_invalid_step (forbidden/malformed event type)
    StoreFull,          // 400 bad_request (capacity 64)
    NotFound            // 404 not_found
};

// HTTP error mapping for store/validation outcomes (spec 4.1.1, 10, 15.1).
ErrCode macro_error_code(MacroError e);
const char* macro_error_detail(MacroError e);

// Validates an input document (macro definition WITHOUT macro_id/revision —
// those are endpoint-assigned) and fills `out`. Input JSON shape per spec
// 10.1.1: {name, timeout_ms?, expected_event?, steps[]}.
bool macro_from_json(const std::string& json, Macro& out, MacroError& err);
// Full document per spec 10.1.1 including macro_id and revision.
std::string macro_to_json(const Macro& m);

// ---- Store (spec 10.1.1, 15.1) --------------------------------------------
// In-memory store with deterministic CRUD. Persistence is the glue's job:
// dump()/load() round-trip opaque definition lines (one JSON per macro).
// Capacity 64; corrupt persisted lines are skipped by load() and reported.
class MacroStore {
public:
    static constexpr size_t kCapacity = 64;

    // Validates, assigns macro_id ("mac_" + 4 hex from rng) and revision 1.
    bool add(const Macro& def, Macro& out, MacroError& err, IRandom& rng);
    // Validates; on success bumps the stored revision and replaces the
    // definition (trigger bindings resolve to the latest revision, 10.1).
    bool update(const std::string& macro_id, const Macro& def, MacroError& err);
    bool remove(const std::string& macro_id);

    const Macro* get(const std::string& macro_id) const;
    const Macro* find_by_name(const std::string& name) const;
    std::vector<const Macro*> list() const; // by macro_id, ascending
    size_t size() const { return macros_.size(); }

    // Persistence round-trip. load() returns the number of skipped corrupt
    // lines; the glue maps any skip to a store_corrupt refusal (spec 15.1).
    std::vector<std::string> dump() const;
    size_t load(const std::vector<std::string>& json_lines);

private:
    bool validate(const Macro& m, MacroError& err, bool for_update) const;
    std::vector<Macro> macros_;
};

} // namespace mcco
