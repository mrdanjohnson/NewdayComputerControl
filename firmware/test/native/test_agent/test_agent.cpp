#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_agent_events.h"
#include "../../lib/maccontrol_core/mc_agent_session.h"
#include <ArduinoJson.h>
#include <string>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800;
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
};

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x77;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

mcco::AgentEvent hello(int seq = 1, int proto = 2) {
    mcco::AgentEvent ev;
    ev.event_id = "evt_1";
    ev.agent_instance_id = "ag-7e21";
    ev.seq = seq;
    ev.timestamp = "2025-01-14T09:31:10Z";
    ev.type = mcco::AgentEventType::Hello;
    ev.payload_json = std::string("{\"protocol_version\":") + std::to_string(proto) +
                      ",\"agent_version\":\"1.0.0\",\"boot_id\":\"b_3F8A11\","
                      "\"hostname\":\"mac-propresenter\"}";
    return ev;
}

mcco::AgentEvent heartbeat(uint64_t seq, const char* id = "evt_2") {
    mcco::AgentEvent ev;
    ev.event_id = id;
    ev.agent_instance_id = "ag-7e21";
    ev.session_id = "s_XXXX";
    ev.seq = seq;
    ev.timestamp = "2025-01-14T09:31:15Z";
    ev.type = mcco::AgentEventType::Heartbeat;
    ev.payload_json = "{\"boot_id\":\"b_3F8A11\",\"uptime_s\":5}";
    return ev;
}

mcco::AgentEvent parse_ok(const char* json) {
    mcco::AgentEvent ev;
    EXPECT_EQ(mcco::parse_agent_event(json, ev), mcco::AgentEventError::Ok);
    return ev;
}

} // namespace

TEST(AgentEvents, HelloRoundTrip) {
    const char* json =
        "{\"event_id\":\"evt_1\",\"agent_instance_id\":\"ag-7e21\",\"seq\":1,"
        "\"timestamp\":\"2025-01-14T09:31:10Z\",\"type\":\"agent_hello\",\"command_id\":null,"
        "\"payload\":{\"protocol_version\":2,\"agent_version\":\"1.0.0\","
        "\"boot_id\":\"b_3F8A11\",\"hostname\":\"mac-propresenter\"}}";
    mcco::AgentEvent ev;
    ASSERT_EQ(mcco::parse_agent_event(json, ev), mcco::AgentEventError::Ok);
    EXPECT_EQ(ev.type, mcco::AgentEventType::Hello);
    EXPECT_EQ(ev.seq, 1);
    EXPECT_TRUE(ev.session_id.empty());
}

TEST(AgentEvents, TwelveTypesAllValidate) {
    struct Case { const char* type; const char* payload; };
    Case cases[] = {
        {"agent_hello", "{\"protocol_version\":2,\"agent_version\":\"1.0.0\",\"boot_id\":\"b\",\"hostname\":\"h\"}"},
        {"agent_goodbye", "{\"reason\":\"sleep\"}"},
        {"heartbeat", "{\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":86400,"
                      "\"boot_time\":1736848800,\"cpu_utilization_pct\":12.5,"
                      "\"memory_utilization_pct\":45,\"disk_free_bytes\":200000000000,"
                      "\"network\":{\"reachable\":true,\"ip\":\"192.168.1.20\"}}"},
        {"heartbeat", "{\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":86400,"
                      "\"boot_time\":1736848800,\"cpu_utilization_pct\":null,"
                      "\"memory_utilization_pct\":null,\"disk_free_bytes\":null,"
                      "\"network\":{\"reachable\":false,\"ip\":null}}"},
        {"system_state_changed", "{\"state\":\"awake\"}"},
        {"user_session_changed", "{\"user_logged_in\":true,\"user\":\"prod\"}"},
        {"user_session_changed", "{\"user_logged_in\":false,\"user\":null}"},
        {"screen_lock_changed", "{\"locked\":true}"},
        {"application_started", "{\"bundle_id\":\"com.x\",\"pid\":812}"},
        {"application_exited", "{\"bundle_id\":\"com.x\",\"pid\":812,\"reason\":\"quit\"}"},
        {"command_ack", "{\"command_id\":\"8F31A2C4\",\"action\":\"launch_app\"}"},
        {"command_ack", "{\"command_id\":\"8F31A2C4\",\"action\":\"sleep\"}"},
        {"command_ack", "{\"command_id\":\"8F31A2C4\",\"action\":\"restart\"}"},
        {"command_ack", "{\"command_id\":\"8F31A2C4\",\"action\":\"shutdown\"}"},
        {"command_result", "{\"command_id\":\"8F31A2C4\",\"outcome\":\"ok\",\"error_code\":null}"},
        {"command_result", "{\"command_id\":\"8F31A2C4\",\"outcome\":\"failed\",\"error_code\":\"launch_failed\"}"},
        {"command_result", "{\"command_id\":\"8F31A2C4\",\"outcome\":\"failed\",\"error_code\":\"command_disabled\"}"},
        {"capability_report", "{\"agent_version\":\"1.0.0\",\"protocol_version\":2,"
         "\"os_version\":\"14.3\",\"hardware_model\":\"Mac14,9\","
         "\"enabled_commands\":[\"launch_app\",\"sleep\"],"
         "\"allowlisted_apps\":[{\"bundle_id\":\"com.x\",\"state\":\"running\"}]}"},
        {"front_app_changed", "{\"bundle_id\":\"com.x\"}"},
        {"front_app_changed", "{\"bundle_id\":null}"},
    };
    for (auto& c : cases) {
        std::string json = std::string("{\"event_id\":\"evt_1\",\"agent_instance_id\":\"ag-7e21\","
                                       "\"seq\":1,\"timestamp\":\"t\",\"type\":\"") +
                           c.type + "\",\"payload\":" + c.payload + "}";
        mcco::AgentEvent ev;
        EXPECT_EQ(mcco::parse_agent_event(json, ev), mcco::AgentEventError::Ok) << c.type;
        EXPECT_EQ(std::string(mcco::agent_event_type_to_string(ev.type)), c.type) << c.type;
    }
}

TEST(AgentEvents, HeartbeatPhase45Validation) {
    // Full payload round-trips and the realistic >2^31 disk value survives.
    mcco::AgentEvent ev = parse_ok(
        "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
        "\"type\":\"heartbeat\",\"payload\":{\"boot_id\":\"b\",\"uptime_s\":5,"
        "\"mac_uptime_s\":86400,\"boot_time\":1736848800,\"cpu_utilization_pct\":12.5,"
        "\"memory_utilization_pct\":45,\"disk_free_bytes\":200000000000,"
        "\"network\":{\"reachable\":true,\"ip\":\"10.0.0.2\"}}}");
    JsonDocument p;
    ASSERT_FALSE(deserializeJson(p, ev.payload_json));
    EXPECT_EQ((long long)p["disk_free_bytes"].as<long long>(), 200000000000LL);

    // Violations: missing key, extra key, network missing reachable,
    // network.ip wrong type, cpu as string.
    auto violation = [&](const char* payload) {
        std::string json = std::string("{\"event_id\":\"e\",\"agent_instance_id\":\"a\","
                                       "\"seq\":1,\"timestamp\":\"t\",\"type\":\"heartbeat\","
                                       "\"payload\":{") +
                           payload + "}}";
        mcco::AgentEvent e2;
        EXPECT_EQ(mcco::parse_agent_event(json, e2), mcco::AgentEventError::SchemaViolation)
            << payload;
    };
    violation("\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":1,\"boot_time\":1,"
              "\"cpu_utilization_pct\":null,\"memory_utilization_pct\":null,"
              "\"disk_free_bytes\":null");  // missing network
    violation("\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":1,\"boot_time\":1,"
              "\"cpu_utilization_pct\":null,\"memory_utilization_pct\":null,"
              "\"disk_free_bytes\":null,\"network\":{\"reachable\":true,\"ip\":null},"
              "\"extra\":1");  // extra key
    violation("\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":1,\"boot_time\":1,"
              "\"cpu_utilization_pct\":null,\"memory_utilization_pct\":null,"
              "\"disk_free_bytes\":null,\"network\":{\"ip\":null}");  // no reachable
    violation("\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":1,\"boot_time\":1,"
              "\"cpu_utilization_pct\":null,\"memory_utilization_pct\":null,"
              "\"disk_free_bytes\":null,\"network\":{\"reachable\":true,\"ip\":7}");  // ip type
    violation("\"boot_id\":\"b\",\"uptime_s\":5,\"mac_uptime_s\":1,\"boot_time\":1,"
              "\"cpu_utilization_pct\":\"busy\",\"memory_utilization_pct\":null,"
              "\"disk_free_bytes\":null,\"network\":{\"reachable\":true,\"ip\":null}");  // cpu string
}

TEST(AgentEvents, CapabilityReportHardwareModel) {
    mcco::AgentEvent ev;
    // hardware_model is now a required capability_report key.
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"capability_report\",\"payload\":{\"agent_version\":\"1.0.0\","
                  "\"protocol_version\":1,\"os_version\":\"14.3\",\"enabled_commands\":[],"
                  "\"allowlisted_apps\":[]}}", ev),
              mcco::AgentEventError::SchemaViolation);
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"capability_report\",\"payload\":{\"agent_version\":\"1.0.0\","
                  "\"protocol_version\":1,\"os_version\":\"14.3\",\"hardware_model\":\"Mac14,9\","
                  "\"enabled_commands\":[],\"allowlisted_apps\":[]}}", ev),
              mcco::AgentEventError::Ok);
}

TEST(AgentEvents, FrontAppChangedValidation) {
    mcco::AgentEvent ev;
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"front_app_changed\",\"payload\":{\"bundle_id\":\"com.x\"}}", ev),
              mcco::AgentEventError::Ok);
    EXPECT_EQ(ev.type, mcco::AgentEventType::FrontAppChanged);
    EXPECT_EQ(std::string(mcco::agent_event_type_to_string(ev.type)), "front_app_changed");
    // Extra key is a schema violation.
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"front_app_changed\",\"payload\":{\"bundle_id\":\"com.x\",\"title\":\"x\"}}", ev),
              mcco::AgentEventError::SchemaViolation);
}

TEST(AgentEvents, CommandAckPowerActionEnum) {
    // Protocol v2: the command_ack action enum widens to the agent-executed
    // power actions; the payload stays {command_id, action} on both shapes.
    mcco::AgentEvent ev;
    for (const char* action : {"sleep", "restart", "shutdown"}) {
        std::string json = std::string(
            "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
            "\"type\":\"command_ack\",\"payload\":{\"command_id\":\"8F31A2C4\",\"action\":\"") +
            action + "\"}}";
        EXPECT_EQ(mcco::parse_agent_event(json, ev), mcco::AgentEventError::Ok) << action;
    }
    // Unknown action is a schema violation (closed enum).
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"command_ack\",\"payload\":{\"command_id\":\"c\",\"action\":\"hibernate\"}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // command_id must be a string.
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"command_ack\",\"payload\":{\"command_id\":7,\"action\":\"sleep\"}}", ev),
              mcco::AgentEventError::SchemaViolation);
}

TEST(AgentEvents, DispatchShapeValidation) {
    using mcco::validate_dispatch_json;
    using mcco::AgentEventError;
    // Power actions carry {action, command_id} only — no bundle_id.
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"sleep\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::Ok);
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"restart\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::Ok);
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"shutdown\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::Ok);
    // App actions still require a bundle_id.
    EXPECT_EQ(validate_dispatch_json(
                  "{\"action\":\"launch_app\",\"bundle_id\":\"com.x\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::Ok);
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"quit_app\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::SchemaViolation);                 // missing bundle_id
    EXPECT_EQ(validate_dispatch_json(
                  "{\"action\":\"launch_app\",\"bundle_id\":\"com.x\"}"),
              AgentEventError::SchemaViolation);                 // missing command_id
    EXPECT_EQ(validate_dispatch_json(
                  "{\"action\":\"launch_app\",\"bundle_id\":\"\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::SchemaViolation);                 // empty bundle_id
    // A bundle_id on a power action is a violation (extra key).
    EXPECT_EQ(validate_dispatch_json(
                  "{\"action\":\"sleep\",\"bundle_id\":\"com.x\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::SchemaViolation);
    // Unknown action / unexpected key / empty command_id / not an object.
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"hibernate\",\"command_id\":\"8F31A2C4\"}"),
              AgentEventError::SchemaViolation);
    EXPECT_EQ(validate_dispatch_json(
                  "{\"action\":\"sleep\",\"command_id\":\"8F31A2C4\",\"extra\":1}"),
              AgentEventError::SchemaViolation);
    EXPECT_EQ(validate_dispatch_json("{\"action\":\"sleep\",\"command_id\":\"\"}"),
              AgentEventError::SchemaViolation);
    EXPECT_EQ(validate_dispatch_json("not json"), AgentEventError::MalformedJson);
    EXPECT_EQ(validate_dispatch_json("[1,2]"), AgentEventError::MalformedJson);
}

TEST(AgentEvents, RejectsUnknownTypeAndSchemaViolations) {
    mcco::AgentEvent ev;
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"teleport\",\"payload\":{}}", ev),
              mcco::AgentEventError::UnknownType);
    // Unknown payload key
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"heartbeat\",\"payload\":{\"boot_id\":\"b\",\"uptime_s\":5,\"extra\":1}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // Missing required key
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"heartbeat\",\"payload\":{\"boot_id\":\"b\"}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // Out-of-enum value
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"system_state_changed\",\"payload\":{\"state\":\"frozen\"}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // command_result failed requires a concrete closed error code
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"command_result\",\"payload\":{\"command_id\":\"c\",\"outcome\":\"failed\",\"error_code\":null}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // command_id on an ambient event is a violation (spec 6.1)
    EXPECT_EQ(mcco::parse_agent_event(
                  "{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,\"timestamp\":\"t\","
                  "\"type\":\"screen_lock_changed\",\"command_id\":\"c\",\"payload\":{\"locked\":true}}", ev),
              mcco::AgentEventError::SchemaViolation);
    // Oversized frame
    std::string big = std::string("{\"event_id\":\"e\",\"agent_instance_id\":\"a\",\"seq\":1,"
                                  "\"timestamp\":\"t\",\"type\":\"heartbeat\",\"payload\":{")
                      + std::string(5000, 'x') + "}}";
    EXPECT_EQ(mcco::parse_agent_event(big, ev), mcco::AgentEventError::TooLarge);
    // Malformed
    EXPECT_EQ(mcco::parse_agent_event("not json", ev), mcco::AgentEventError::MalformedJson);
}

TEST(AgentSession, HelloAckAndLiveness) {
    FakeClock clock;
    FakeRandom rng;
    mcco::AgentSession s(rng, clock);
    EXPECT_EQ(s.liveness(), mcco::AgentConnState::AwaitingHello);
    ASSERT_EQ(s.acceptHello(hello()), 0);
    EXPECT_EQ(s.liveness(), mcco::AgentConnState::Active);
    EXPECT_FALSE(s.sessionId().empty());
    // Session id must be printable "s_" + 4 uppercase hex (it is serialized
    // into JSON frames; a lowercase-hex uppercasing bug once produced raw
    // control bytes here).
    ASSERT_EQ(s.sessionId().size(), 6);
    EXPECT_EQ(s.sessionId().substr(0, 2), "s_");
    for (char c : s.sessionId().substr(2)) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) << c;
    }
    // defaults: heartbeat 5 s, stale 15, offline 30 (spec 4.2.2)
    EXPECT_EQ(s.heartbeatIntervalS(), 5);
    EXPECT_EQ(s.staleThresholdS(), 15);
    EXPECT_EQ(s.offlineThresholdS(), 30);

    clock.mono_ms += 16000; // > stale, < offline (monotonic)
    EXPECT_EQ(s.liveness(), mcco::AgentConnState::Stale);
    clock.mono_ms += 15000; // > offline
    EXPECT_EQ(s.liveness(), mcco::AgentConnState::Offline);

    // Any valid frame returns the session to ACTIVE (spec 4.2.1); an epoch
    // jump (SNTP sync) in between must not affect the monotonic age.
    clock.epoch += 90ULL * 24 * 3600;
    ASSERT_EQ(s.onFrame(heartbeat(2), mcco::AgentEventError::Ok), 0);
    EXPECT_EQ(s.liveness(), mcco::AgentConnState::Active);
}

TEST(AgentSession, RejectsBadHello) {
    FakeClock clock;
    FakeRandom rng;
    {
        mcco::AgentSession s(rng, clock);
        EXPECT_EQ(s.acceptHello(hello(2)), 4002); // seq must start at 1
    }
    {
        mcco::AgentSession s(rng, clock);
        EXPECT_EQ(s.acceptHello(hello(1, 1)), 4003); // protocol v1 no longer supported
    }
    {
        mcco::AgentSession s(rng, clock);
        mcco::AgentEvent h = hello();
        h.session_id = "s_dead"; // hello carries no session_id
        EXPECT_EQ(s.acceptHello(h), 1008);
    }
    {
        mcco::AgentSession s(rng, clock);
        mcco::AgentEvent h = heartbeat(1); // first frame must be agent_hello
        EXPECT_EQ(s.acceptHello(h), 1008);
    }
}

TEST(AgentSession, SequenceAccounting) {
    FakeClock clock;
    FakeRandom rng;
    mcco::AgentSession s(rng, clock);
    ASSERT_EQ(s.acceptHello(hello()), 0);

    // Regression (seq reuse) closes 4002.
    EXPECT_EQ(s.onFrame(heartbeat(1), mcco::AgentEventError::Ok), 4002);
    {
        mcco::AgentSession s2(rng, clock); // fresh session for the next checks
        ASSERT_EQ(s2.acceptHello(hello()), 0);
        // Gap > max_seq_gap (10) closes 4002.
        EXPECT_EQ(s2.onFrame(heartbeat(12), mcco::AgentEventError::Ok), 4002);
    }
    {
        mcco::AgentSession s3(rng, clock);
        ASSERT_EQ(s3.acceptHello(hello()), 0);
        ASSERT_EQ(s3.onFrame(heartbeat(2), mcco::AgentEventError::Ok), 0);
        // Within-gap skip is absorbed (seq advances past the hole).
        ASSERT_EQ(s3.onFrame(heartbeat(9), mcco::AgentEventError::Ok), 0);
    }
}

TEST(AgentSession, SchemaViolationThresholdConsumesSeq) {
    FakeClock clock;
    FakeRandom rng;
    mcco::AgentSession s(rng, clock);
    ASSERT_EQ(s.acceptHello(hello()), 0);

    // A violating frame consumes its seq: the next valid frame at last+1 is
    // NOT a phantom gap (spec 6.1.1). Two violations are tolerated.
    EXPECT_EQ(s.onFrame(heartbeat(2), mcco::AgentEventError::SchemaViolation), 0);
    EXPECT_EQ(s.consecutiveViolations(), 1);
    EXPECT_EQ(s.onFrame(heartbeat(3), mcco::AgentEventError::UnknownType), 0);
    EXPECT_EQ(s.consecutiveViolations(), 2);
    // A valid frame resets the counter.
    ASSERT_EQ(s.onFrame(heartbeat(4), mcco::AgentEventError::Ok), 0);
    EXPECT_EQ(s.consecutiveViolations(), 0);
    // Three consecutive violations close 4003.
    EXPECT_EQ(s.onFrame(heartbeat(5), mcco::AgentEventError::SchemaViolation), 0);
    EXPECT_EQ(s.onFrame(heartbeat(6), mcco::AgentEventError::SchemaViolation), 0);
    EXPECT_EQ(s.onFrame(heartbeat(7), mcco::AgentEventError::SchemaViolation), 4003);
}
