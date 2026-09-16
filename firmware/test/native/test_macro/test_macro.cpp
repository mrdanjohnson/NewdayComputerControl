// Macro model and store per spec 10.1 (model, validation), 10.3 (store hook),
// and 15.1 (persistence lines).
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_macro.h"
#include <ArduinoJson.h>
#include <string>
#include <vector>

namespace {

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x42;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

// The spec 10.1.1 worked example (input document form).
const char* kWorkedExample = R"({
  "name": "Launch ProPresenter",
  "timeout_ms": 10000,
  "expected_event": {
    "type": "application_started",
    "match": { "bundle_id": "com.renewedvision.propresenter" }
  },
  "steps": [
    {"order": 1, "type": "key_combo", "modifiers": ["cmd"], "key": "space"},
    {"order": 2, "type": "delay", "delay_ms": 500},
    {"order": 3, "type": "text", "value": "ProPresenter"},
    {"order": 4, "type": "delay", "delay_ms": 500},
    {"order": 5, "type": "key_press", "key": "enter"}
  ]
})";

mcco::Macro parse_ok() {
    mcco::Macro m;
    mcco::MacroError err = mcco::MacroError::Ok;
    EXPECT_TRUE(mcco::macro_from_json(kWorkedExample, m, err)) << mcco::macro_error_detail(err);
    return m;
}

// Semantic equality for the round-trip invariant: macro_to_json emits every
// step field, so from_json round-trips fill irrelevant fields with defaults;
// compare only the fields meaningful per step type.
bool same_macro(const mcco::Macro& a, const mcco::Macro& b) {
    if (a.name != b.name || a.timeout_ms != b.timeout_ms ||
        a.has_expected_event != b.has_expected_event)
        return false;
    if (a.has_expected_event &&
        (a.expected_event.type != b.expected_event.type ||
         a.expected_event.match_json != b.expected_event.match_json))
        return false;
    if (a.steps.size() != b.steps.size()) return false;
    for (size_t i = 0; i < a.steps.size(); i++) {
        const mcco::MacroStep& x = a.steps[i];
        const mcco::MacroStep& y = b.steps[i];
        if (x.order != y.order || x.type != y.type) return false;
        switch (x.type) {
            case mcco::StepType::KeyPress:
            case mcco::StepType::KeyRelease:
                if (x.key != y.key) return false;
                break;
            case mcco::StepType::KeyCombo:
                if (x.key != y.key || x.modifiers != y.modifiers) return false;
                break;
            case mcco::StepType::ModifierDown:
            case mcco::StepType::ModifierUp:
                if (x.modifiers != y.modifiers) return false;
                break;
            case mcco::StepType::Text:
                if (x.value != y.value) return false;
                break;
            case mcco::StepType::Delay:
                if (x.delay_ms != y.delay_ms) return false;
                break;
        }
    }
    return true;
}

// ---- HID key table (spec 10.1.1) -------------------------------------------

TEST(MacroHidKeys, ValidNamesAndCodes) {
    for (const char* name : {"space", "enter", "f1", "f12", "a", "z", "0", "9",
                             "esc", "backspace", "tab", "minus", "equal", "caps_lock",
                             "left", "right", "up", "down", "home", "end",
                             "page_up", "page_down", "insert", "delete"})
        EXPECT_TRUE(mcco::hid_key_valid(name)) << name;
    EXPECT_EQ(mcco::hid_key_code("a"), 0x04);
    EXPECT_EQ(mcco::hid_key_code("z"), 0x1D);
    EXPECT_EQ(mcco::hid_key_code("1"), 0x1E);
    EXPECT_EQ(mcco::hid_key_code("0"), 0x27);
    EXPECT_EQ(mcco::hid_key_code("enter"), 0x28);
    EXPECT_EQ(mcco::hid_key_code("esc"), 0x29);
    EXPECT_EQ(mcco::hid_key_code("space"), 0x2C);
    EXPECT_EQ(mcco::hid_key_code("caps_lock"), 0x39);
    EXPECT_EQ(mcco::hid_key_code("f1"), 0x3A);
    EXPECT_EQ(mcco::hid_key_code("f12"), 0x45);
    EXPECT_EQ(mcco::hid_key_code("insert"), 0x49);
    EXPECT_EQ(mcco::hid_key_code("delete"), 0x4C);
    EXPECT_EQ(mcco::hid_key_code("right"), 0x4F);
    EXPECT_EQ(mcco::hid_key_code("left"), 0x50);
    EXPECT_EQ(mcco::hid_key_code("down"), 0x51);
    EXPECT_EQ(mcco::hid_key_code("up"), 0x52);
    // Contiguity of the letter/digit ranges.
    for (int i = 0; i < 26; i++) {
        std::string name(1, char('a' + i));
        EXPECT_EQ(mcco::hid_key_code(name.c_str()), 0x04 + i);
    }
    const char* digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(mcco::hid_key_code(digits[i]), 0x1E + i);
}

TEST(MacroHidKeys, InvalidNames) {
    for (const char* name : {"mouse", "F13", "", "f0", "shift", "return", "Space", "A"})
        EXPECT_FALSE(mcco::hid_key_valid(name)) << name;
    EXPECT_EQ(mcco::hid_key_code("mouse"), 0);
    EXPECT_EQ(mcco::hid_key_code(""), 0);
}

TEST(MacroHidKeys, TableShape) {
    // Names are unique and codes are non-zero (0 is the unknown sentinel).
    for (size_t i = 0; i < mcco::hid_key_table_size(); i++) {
        EXPECT_NE(mcco::hid_key_table()[i].code, 0);
        for (size_t j = i + 1; j < mcco::hid_key_table_size(); j++)
            EXPECT_STRNE(mcco::hid_key_table()[i].name, mcco::hid_key_table()[j].name);
    }
}

// ---- Modifiers and step types ----------------------------------------------

TEST(MacroModifiers, BitsAndValidity) {
    EXPECT_EQ(mcco::modifier_hid_mask("ctrl"), 0x01);
    EXPECT_EQ(mcco::modifier_hid_mask("shift"), 0x02);
    EXPECT_EQ(mcco::modifier_hid_mask("alt"), 0x04);
    EXPECT_EQ(mcco::modifier_hid_mask("cmd"), 0x08);
    for (const char* m : {"ctrl", "shift", "alt", "cmd"}) EXPECT_TRUE(mcco::modifier_valid(m));
    for (const char* m : {"windows", "meta", "", "Cmd", "control"})
        EXPECT_FALSE(mcco::modifier_valid(m)) << m;
    EXPECT_EQ(mcco::modifier_hid_mask("windows"), 0);
    EXPECT_EQ(mcco::modifier_hid_mask(nullptr), 0);
}

TEST(MacroStepTypes, ClosedSetRoundTrip) {
    const mcco::StepType all[] = {mcco::StepType::KeyPress,   mcco::StepType::KeyCombo,
                                  mcco::StepType::ModifierDown, mcco::StepType::ModifierUp,
                                  mcco::StepType::KeyRelease, mcco::StepType::Text,
                                  mcco::StepType::Delay};
    const char* names[] = {"key_press", "key_combo", "modifier_down",
                           "modifier_up", "key_release", "text", "delay"};
    for (size_t i = 0; i < 7; i++) {
        mcco::StepType t;
        ASSERT_TRUE(mcco::step_type_from_string(names[i], t));
        EXPECT_EQ(t, all[i]);
        EXPECT_STREQ(mcco::step_type_to_string(all[i]), names[i]);
    }
    mcco::StepType t;
    EXPECT_FALSE(mcco::step_type_from_string("mouse_click", t));
    EXPECT_FALSE(mcco::step_type_from_string("", t));
    EXPECT_FALSE(mcco::step_type_from_string(nullptr, t));
}

// ---- macro_from_json ---------------------------------------------------------

TEST(MacroFromJson, WorkedExample) {
    mcco::Macro m;
    mcco::MacroError err = mcco::MacroError::Ok;
    ASSERT_TRUE(mcco::macro_from_json(kWorkedExample, m, err));
    EXPECT_EQ(err, mcco::MacroError::Ok);
    EXPECT_TRUE(m.macro_id.empty());  // endpoint-assigned, not in input
    EXPECT_EQ(m.revision, 1u);
    EXPECT_EQ(m.name, "Launch ProPresenter");
    EXPECT_EQ(m.timeout_ms, 10000u);
    ASSERT_TRUE(m.has_expected_event);
    EXPECT_EQ(m.expected_event.type, "application_started");
    EXPECT_EQ(m.expected_event.match_json, R"({"bundle_id":"com.renewedvision.propresenter"})");
    ASSERT_EQ(m.steps.size(), 5);

    EXPECT_EQ(m.steps[0].order, 1);
    EXPECT_EQ(m.steps[0].type, mcco::StepType::KeyCombo);
    EXPECT_EQ(m.steps[0].key, "space");
    ASSERT_EQ(m.steps[0].modifiers.size(), 1);
    EXPECT_EQ(m.steps[0].modifiers[0], "cmd");

    EXPECT_EQ(m.steps[1].order, 2);
    EXPECT_EQ(m.steps[1].type, mcco::StepType::Delay);
    EXPECT_EQ(m.steps[1].delay_ms, 500u);

    EXPECT_EQ(m.steps[2].order, 3);
    EXPECT_EQ(m.steps[2].type, mcco::StepType::Text);
    EXPECT_EQ(m.steps[2].value, "ProPresenter");

    EXPECT_EQ(m.steps[3].order, 4);
    EXPECT_EQ(m.steps[3].type, mcco::StepType::Delay);
    EXPECT_EQ(m.steps[3].delay_ms, 500u);

    EXPECT_EQ(m.steps[4].order, 5);
    EXPECT_EQ(m.steps[4].type, mcco::StepType::KeyPress);
    EXPECT_EQ(m.steps[4].key, "enter");
}

TEST(MacroFromJson, DefaultsAndMinimalDoc) {
    mcco::Macro m;
    mcco::MacroError err;
    ASSERT_TRUE(mcco::macro_from_json(R"({"name":"x","steps":[{"order":1,"type":"key_press","key":"a"}]})",
                                      m, err));
    EXPECT_EQ(m.timeout_ms, 10000u); // default
    EXPECT_FALSE(m.has_expected_event);
    ASSERT_EQ(m.steps.size(), 1);
    EXPECT_EQ(m.steps[0].key, "a");
}

TEST(MacroFromJson, RejectsBadStepType) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"mouse_click","key":"a"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"key":"a"}]})", m, err)); // type missing
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsBadKey) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"mouse"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Missing key entirely.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsBadModifiers) {
    mcco::Macro m;
    mcco::MacroError err;
    // Unknown modifier.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_combo","key":"a","modifiers":["windows"]}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Duplicate modifier.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_combo","key":"a","modifiers":["cmd","cmd"]}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Missing modifiers array on a combo.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_combo","key":"a"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsModifierStepProblems) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"modifier_down","modifier":"hyper"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"modifier_up"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Happy path: single modifier stored in the modifiers vector.
    ASSERT_TRUE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"modifier_down","modifier":"shift"},
                               {"order":2,"type":"key_press","key":"a"},
                               {"order":3,"type":"modifier_up","modifier":"shift"}]})",
        m, err));
    ASSERT_EQ(m.steps.size(), 3);
    EXPECT_EQ(m.steps[0].type, mcco::StepType::ModifierDown);
    ASSERT_EQ(m.steps[0].modifiers.size(), 1);
    EXPECT_EQ(m.steps[0].modifiers[0], "shift");
}

TEST(MacroFromJson, RejectsBadTextValue) {
    mcco::Macro m;
    mcco::MacroError err;
    auto doc_with = [&](const std::string& value) {
        return R"({"name":"m","steps":[{"order":1,"type":"text","value":")" + value + R"("}]})";
    };
    EXPECT_FALSE(mcco::macro_from_json(doc_with(""), m, err)); // empty
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_FALSE(mcco::macro_from_json(doc_with(std::string(257, 'x')), m, err)); // too long
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    std::string non_ascii = "caf";
    non_ascii += char(0xC3);
    non_ascii += char(0xA9);
    EXPECT_FALSE(mcco::macro_from_json(doc_with(non_ascii), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Boundary: 256 chars is fine.
    EXPECT_TRUE(mcco::macro_from_json(doc_with(std::string(256, 'x')), m, err));
}

TEST(MacroFromJson, RejectsBadDelay) {
    mcco::Macro m;
    mcco::MacroError err;
    auto doc_with = [&](int ms) {
        return R"({"name":"m","steps":[{"order":1,"type":"delay","delay_ms":)" +
               std::to_string(ms) + "}]}";
    };
    EXPECT_FALSE(mcco::macro_from_json(doc_with(9), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_FALSE(mcco::macro_from_json(doc_with(5001), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_TRUE(mcco::macro_from_json(doc_with(10), m, err));
    EXPECT_TRUE(mcco::macro_from_json(doc_with(5000), m, err));
}

TEST(MacroFromJson, RejectsBadStepCounts) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(R"({"name":"m","steps":[]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);

    std::string doc = R"({"name":"m","steps":[)";
    for (int i = 1; i <= 65; i++) {
        if (i > 1) doc += ",";
        doc += R"({"order":)" + std::to_string(i) + R"(,"type":"key_press","key":"a"})";
    }
    doc += "]}";
    EXPECT_FALSE(mcco::macro_from_json(doc, m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsBadOrderSequence) {
    mcco::Macro m;
    mcco::MacroError err;
    // Gap: 1 then 3.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a"},
                               {"order":3,"type":"key_press","key":"b"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Duplicate: 1 then 1.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a"},
                               {"order":1,"type":"key_press","key":"b"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    // Not starting at 1.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":2,"type":"key_press","key":"a"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsExtraStepKey) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a","app":"Finder"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsUnknownTopLevelKey) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a"}],"schedule":"daily"})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

TEST(MacroFromJson, RejectsForbiddenExpectedEventTypes) {
    mcco::Macro m;
    mcco::MacroError err;
    auto doc_with = [&](const char* type) {
        return std::string(R"({"name":"m","expected_event":{"type":")") + type +
               R"(","match":{}},"steps":[{"order":1,"type":"key_press","key":"a"}]})";
    };
    for (const char* bad : {"heartbeat", "command_ack", "command_result", "agent_hello",
                            "agent_goodbye", "capability_report", "screen_changed", ""}) {
        EXPECT_FALSE(mcco::macro_from_json(doc_with(bad), m, err)) << bad;
        EXPECT_EQ(err, mcco::MacroError::InvalidExpectedEvent) << bad;
    }
    // The five ambient evidence types are accepted, with an empty match object.
    for (const char* good : {"application_started", "application_exited", "system_state_changed",
                             "user_session_changed", "screen_lock_changed"}) {
        EXPECT_TRUE(mcco::macro_from_json(doc_with(good), m, err)) << good;
        EXPECT_EQ(err, mcco::MacroError::Ok) << good;
    }
}

TEST(MacroFromJson, RejectsMalformedExpectedEvent) {
    mcco::Macro m;
    mcco::MacroError err;
    // match missing.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","expected_event":{"type":"application_started"},
            "steps":[{"order":1,"type":"key_press","key":"a"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidExpectedEvent);
    // match not an object.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","expected_event":{"type":"application_started","match":"bundle_id"},
            "steps":[{"order":1,"type":"key_press","key":"a"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidExpectedEvent);
    // expected_event not an object.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","expected_event":"application_started",
            "steps":[{"order":1,"type":"key_press","key":"a"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidExpectedEvent);
    // type missing.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":"m","expected_event":{"match":{}},"steps":[{"order":1,"type":"key_press","key":"a"}]})",
        m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidExpectedEvent);
}

TEST(MacroFromJson, RejectsBadTimeout) {
    mcco::Macro m;
    mcco::MacroError err;
    auto doc_with = [&](const char* t) {
        return std::string(R"({"name":"m","timeout_ms":)") + t +
               R"(,"steps":[{"order":1,"type":"key_press","key":"a"}]})";
    };
    EXPECT_FALSE(mcco::macro_from_json(doc_with("499"), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidTimeout);
    EXPECT_FALSE(mcco::macro_from_json(doc_with("60001"), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidTimeout);
    EXPECT_TRUE(mcco::macro_from_json(doc_with("500"), m, err));
    EXPECT_TRUE(mcco::macro_from_json(doc_with("60000"), m, err));
    // Non-integer timeout is also InvalidTimeout.
    EXPECT_FALSE(mcco::macro_from_json(doc_with("\"5000\""), m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidTimeout);
}

TEST(MacroFromJson, RejectsBadName) {
    mcco::Macro m;
    mcco::MacroError err;
    auto doc_with = [&](const std::string& name) {
        return R"({"name":")" + name + R"(","steps":[{"order":1,"type":"key_press","key":"a"}]})";
    };
    EXPECT_FALSE(mcco::macro_from_json(doc_with(""), m, err)); // empty
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    EXPECT_FALSE(mcco::macro_from_json(doc_with(std::string(33, 'n')), m, err)); // too long
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    std::string tabbed = "bad";
    tabbed += char(0x09);
    tabbed += "name";
    EXPECT_FALSE(mcco::macro_from_json(doc_with(tabbed), m, err)); // non-printable
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    // Missing name entirely.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"steps":[{"order":1,"type":"key_press","key":"a"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    // Name not a string.
    EXPECT_FALSE(mcco::macro_from_json(
        R"({"name":42,"steps":[{"order":1,"type":"key_press","key":"a"}]})", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    // Boundaries: 1 and 32 chars are fine.
    EXPECT_TRUE(mcco::macro_from_json(doc_with("x"), m, err));
    EXPECT_TRUE(mcco::macro_from_json(doc_with(std::string(32, 'n')), m, err));
}

TEST(MacroFromJson, RejectsMalformedJson) {
    mcco::Macro m;
    mcco::MacroError err;
    EXPECT_FALSE(mcco::macro_from_json("{unclosed", m, err));
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
    EXPECT_FALSE(mcco::macro_from_json("[1,2]", m, err)); // not an object
    EXPECT_EQ(err, mcco::MacroError::InvalidStep);
}

// ---- macro_to_json round-trip -------------------------------------------------

TEST(MacroToJson, RoundTrip) {
    mcco::Macro original = parse_ok();
    original.macro_id = "mac_3F81";
    original.revision = 4;
    const std::string full = mcco::macro_to_json(original);

    // Strip the endpoint-assigned fields to get the input document form.
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, full));
    JsonObject obj = doc.as<JsonObject>();
    obj.remove("macro_id");
    obj.remove("revision");
    std::string input_form;
    serializeJson(doc, input_form);

    mcco::Macro round;
    mcco::MacroError err;
    ASSERT_TRUE(mcco::macro_from_json(input_form, round, err)) << mcco::macro_error_detail(err);
    EXPECT_TRUE(same_macro(original, round));
}

TEST(MacroToJson, OmitsExpectedEventWhenAbsent) {
    mcco::Macro m;
    mcco::MacroError err;
    ASSERT_TRUE(mcco::macro_from_json(
        R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a"}]})", m, err));
    m.macro_id = "mac_0001";
    const std::string json = mcco::macro_to_json(m);
    EXPECT_EQ(json.find("expected_event"), std::string::npos);
    EXPECT_NE(json.find("\"macro_id\":\"mac_0001\""), std::string::npos);
    EXPECT_NE(json.find("\"revision\":1"), std::string::npos);
    EXPECT_NE(json.find("\"timeout_ms\":10000"), std::string::npos);
}

// ---- Error mapping -------------------------------------------------------------

TEST(MacroErrorMap, CodesAndDetails) {
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::InvalidStep), mcco::ErrCode::MacroInvalidStep);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::InvalidExpectedEvent),
              mcco::ErrCode::MacroInvalidStep);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::InvalidName), mcco::ErrCode::BadRequest);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::DuplicateName), mcco::ErrCode::BadRequest);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::InvalidTimeout), mcco::ErrCode::BadRequest);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::StoreFull), mcco::ErrCode::BadRequest);
    EXPECT_EQ(mcco::macro_error_code(mcco::MacroError::NotFound), mcco::ErrCode::NotFound);
    for (mcco::MacroError e : {mcco::MacroError::InvalidStep, mcco::MacroError::InvalidName,
                               mcco::MacroError::DuplicateName, mcco::MacroError::InvalidTimeout,
                               mcco::MacroError::InvalidExpectedEvent, mcco::MacroError::StoreFull,
                               mcco::MacroError::NotFound})
        EXPECT_STRNE(mcco::macro_error_detail(e), "");
}

// ---- MacroStore -----------------------------------------------------------------

TEST(MacroStore, AddAssignsUniqueIds) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out1, out2;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out1, err, rng));
    EXPECT_EQ(out1.macro_id, "mac_4243"); // 0x42, 0x43 uppercase hex
    EXPECT_EQ(out1.revision, 1u);
    mcco::Macro def2 = parse_ok();
    def2.name = "Second"; // names are unique per endpoint
    EXPECT_TRUE(store.add(def2, out2, err, rng));
    EXPECT_EQ(out2.macro_id, "mac_4445");
    EXPECT_NE(out1.macro_id, out2.macro_id);
    EXPECT_EQ(store.size(), 2);
    // The stored copy is retrievable by id.
    const mcco::Macro* got = store.get(out1.macro_id);
    ASSERT_NE(got, nullptr);
    EXPECT_TRUE(same_macro(*got, def));
}

TEST(MacroStore, CapacityIs64) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::MacroError err;
    for (int i = 0; i < 64; i++) {
        mcco::Macro def;
        ASSERT_TRUE(mcco::macro_from_json(
            R"({"name":"m)" + std::to_string(i) + R"(","steps":[{"order":1,"type":"key_press","key":"a"}]})",
            def, err));
        mcco::Macro out;
        ASSERT_TRUE(store.add(def, out, err, rng)) << i;
    }
    EXPECT_EQ(store.size(), mcco::MacroStore::kCapacity);
    mcco::Macro extra = parse_ok();
    mcco::Macro out;
    EXPECT_FALSE(store.add(extra, out, err, rng));
    EXPECT_EQ(err, mcco::MacroError::StoreFull);
}

TEST(MacroStore, DuplicateNameRejected) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out, err, rng));
    EXPECT_FALSE(store.add(def, out, err, rng)); // same name
    EXPECT_EQ(err, mcco::MacroError::DuplicateName);
    EXPECT_EQ(store.size(), 1);
}

TEST(MacroStore, AddValidatesDefinition) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    def.name = ""; // invalid even though it parsed from a good doc once
    mcco::Macro out;
    mcco::MacroError err;
    EXPECT_FALSE(store.add(def, out, err, rng));
    EXPECT_EQ(err, mcco::MacroError::InvalidName);
    def = parse_ok();
    def.timeout_ms = 10;
    EXPECT_FALSE(store.add(def, out, err, rng));
    EXPECT_EQ(err, mcco::MacroError::InvalidTimeout);
}

TEST(MacroStore, UpdateBumpsRevisionAndKeepsId) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out, err, rng));
    const std::string id = out.macro_id;

    mcco::Macro edited = parse_ok();
    edited.name = "Edited";
    edited.timeout_ms = 20000;
    ASSERT_TRUE(store.update(id, edited, err));
    const mcco::Macro* got = store.get(id);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->macro_id, id);
    EXPECT_EQ(got->revision, 2u);
    EXPECT_EQ(got->name, "Edited");
    EXPECT_EQ(got->timeout_ms, 20000u);
    EXPECT_EQ(got->steps.size(), 5);
    EXPECT_EQ(store.find_by_name("Launch ProPresenter"), nullptr);
    ASSERT_NE(store.find_by_name("Edited"), nullptr);
}

TEST(MacroStore, UpdateUnknownIdNotFound) {
    mcco::MacroStore store;
    mcco::MacroError err;
    EXPECT_FALSE(store.update("mac_FFFF", parse_ok(), err));
    EXPECT_EQ(err, mcco::MacroError::NotFound);
}

TEST(MacroStore, UpdateDuplicateNameRejected) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::MacroError err;
    mcco::Macro a = parse_ok();
    mcco::Macro b;
    ASSERT_TRUE(mcco::macro_from_json(
        R"({"name":"other","steps":[{"order":1,"type":"key_press","key":"b"}]})", b, err));
    mcco::Macro out_a, out_b;
    ASSERT_TRUE(store.add(a, out_a, err, rng));
    ASSERT_TRUE(store.add(b, out_b, err, rng));
    // Renaming b to a's name must fail, leaving b intact.
    mcco::Macro clash = parse_ok();
    EXPECT_FALSE(store.update(out_b.macro_id, clash, err));
    EXPECT_EQ(err, mcco::MacroError::DuplicateName);
    ASSERT_NE(store.find_by_name("other"), nullptr);
}

TEST(MacroStore, RemoveGetFindByName) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out, err, rng));
    const std::string id = out.macro_id;

    ASSERT_NE(store.find_by_name("Launch ProPresenter"), nullptr);
    EXPECT_EQ(store.find_by_name("nope"), nullptr);
    EXPECT_TRUE(store.remove(id));
    EXPECT_EQ(store.size(), 0);
    EXPECT_EQ(store.get(id), nullptr);
    EXPECT_FALSE(store.remove(id)); // already gone
}

TEST(MacroStore, ListSortedByMacroId) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::MacroError err;
    // Ids increase monotonically with FakeRandom; add then verify order.
    std::vector<std::string> ids;
    for (int i = 0; i < 4; i++) {
        mcco::Macro def;
        ASSERT_TRUE(mcco::macro_from_json(
            R"({"name":"m)" + std::to_string(i) + R"(","steps":[{"order":1,"type":"key_press","key":"a"}]})",
            def, err));
        mcco::Macro out;
        ASSERT_TRUE(store.add(def, out, err, rng));
        ids.push_back(out.macro_id);
    }
    auto listed = store.list();
    ASSERT_EQ(listed.size(), ids.size());
    for (size_t i = 1; i < listed.size(); i++)
        EXPECT_LT(listed[i - 1]->macro_id, listed[i]->macro_id);
}

TEST(MacroStore, DumpLoadRoundTripSkipsCorrupt) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out, err, rng));
    mcco::Macro other;
    ASSERT_TRUE(mcco::macro_from_json(
        R"({"name":"other","timeout_ms":1500,"steps":[{"order":1,"type":"text","value":"hi"},
            {"order":2,"type":"key_combo","key":"space","modifiers":["ctrl","shift"]}]})",
        other, err));
    mcco::Macro out2;
    ASSERT_TRUE(store.add(other, out2, err, rng));

    std::vector<std::string> lines = store.dump();
    ASSERT_EQ(lines.size(), 2);

    // Reload into a fresh store with one corrupt line inserted.
    lines.insert(lines.begin() + 1, "{not json");
    mcco::MacroStore restored;
    EXPECT_EQ(restored.load(lines), 1); // one corrupt line skipped
    ASSERT_EQ(restored.size(), 2);
    ASSERT_NE(restored.get(out.macro_id), nullptr);
    ASSERT_NE(restored.get(out2.macro_id), nullptr);
    EXPECT_TRUE(same_macro(*restored.get(out.macro_id), def));
    EXPECT_TRUE(same_macro(*restored.get(out2.macro_id), other));
    // revision and ids survive the round-trip.
    EXPECT_EQ(restored.get(out.macro_id)->revision, 1u);

    // A line that parses but fails validation is corrupt too; the other two
    // lines are valid.
    lines[1] = R"({"macro_id":"mac_BEEF","revision":1,"name":"","steps":[]})";
    mcco::MacroStore restored2;
    EXPECT_EQ(restored2.load(lines), 1);
    EXPECT_EQ(restored2.size(), 2);
    EXPECT_EQ(restored2.get("mac_BEEF"), nullptr);
}

TEST(MacroStore, LoadLaterDuplicateIdReplacesEarlier) {
    mcco::MacroStore store;
    FakeRandom rng;
    mcco::Macro def = parse_ok();
    mcco::Macro out;
    mcco::MacroError err;
    ASSERT_TRUE(store.add(def, out, err, rng));
    std::vector<std::string> lines = store.dump();
    // Same macro_id but a different revision and name later in the stream.
    mcco::Macro edited = def;
    edited.name = "Edited";
    edited.macro_id = out.macro_id;
    edited.revision = 7;
    lines.push_back(mcco::macro_to_json(edited));

    mcco::MacroStore restored;
    EXPECT_EQ(restored.load(lines), 0);
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored.get(out.macro_id)->name, "Edited");
    EXPECT_EQ(restored.get(out.macro_id)->revision, 7u);
}

TEST(MacroStore, LoadMissingIdOrRevisionIsCorrupt) {
    mcco::MacroStore store;
    EXPECT_EQ(store.load({R"({"name":"m","steps":[{"order":1,"type":"key_press","key":"a"}]})"}),
              1); // no macro_id
    EXPECT_EQ(store.load({R"({"macro_id":"mac_0001","name":"m",
                           "steps":[{"order":1,"type":"key_press","key":"a"}]})"}),
              1); // no revision
    EXPECT_EQ(store.size(), 0);
}

} // namespace
