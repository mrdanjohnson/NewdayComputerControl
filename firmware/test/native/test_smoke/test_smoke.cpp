#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_engine.h"
#include "../../lib/maccontrol_core/mc_sha256.h"
#include "../../lib/maccontrol_core/mc_status.h"
#include "../../lib/maccontrol_core/mc_iso8601.h"
#include <vector>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800; // 2025-01-14T09:20:00Z
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
};

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x42;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

struct MemStorage : mcco::ILedgerStorage {
    std::vector<std::string> lines;
    bool append(const std::string& line) override {
        lines.push_back(line);
        return true;
    }
    bool read_all(const std::function<void(const std::string&)>& cb) override {
        for (auto& l : lines) cb(l);
        return true;
    }
    bool replace_all(const std::vector<std::string>& new_lines) override {
        lines = new_lines;
        return true;
    }
};

} // namespace

TEST(Smoke, Sha256KnownVector) {
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    EXPECT_EQ(mcco::Sha256::hex_digest("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Smoke, Iso8601RoundTrip) {
    uint64_t t = 0;
    ASSERT_TRUE(mcco::iso8601_parse("2025-01-14T09:41:12Z", t));
    EXPECT_EQ(mcco::iso8601_format(t), "2025-01-14T09:41:12Z");
    EXPECT_FALSE(mcco::iso8601_parse("2025-01-14 09:41:12", t));
}

TEST(Smoke, ModeALifecycleTerminatesUnconfirmed) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger(storage, clock, log, 64);
    mcco::CommandEngine engine(ledger, clock, rng, log);
    ASSERT_TRUE(engine.init());

    mcco::Submission sub;
    sub.type = mcco::CommandType::Lock;
    sub.requested_by = "apikey:key-01";
    sub.body_hash = "h1";
    auto out = engine.submit(sub);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.http_status, 202);
    EXPECT_TRUE(out.dispatch_pending);

    ASSERT_TRUE(engine.complete_dispatch(out.record.command_id, true));
    const mcco::CommandRecord* rec = engine.get(out.record.command_id);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->state, mcco::CommandState::Unconfirmed);
    EXPECT_EQ(rec->result, "hid_only");
    EXPECT_EQ(rec->revision, 3);
}

TEST(Smoke, CapabilitiesModeAHonesty) {
    mcco::Identity id{"ProPresenter Mac", "mac-a1b2c3", "", "", "a1b2c3d4e5f6"};
    JsonDocument doc;
    mcco::build_capabilities_mode_a(doc, id, {});
    EXPECT_EQ(std::string(doc["mode"] | "?"), "A");
    EXPECT_EQ(std::string(doc["capability_level"] | "?"), "L1");
    JsonObject cmds = doc["commands"];
    for (JsonPair kv : cmds) {
        EXPECT_FALSE(kv.value()["verified"].as<bool>()) << kv.key().c_str();
    }
    EXPECT_FALSE(cmds["app_launch"]["available"].as<bool>());
    EXPECT_TRUE(cmds["lock"]["available"].as<bool>());
    EXPECT_EQ(cmds["lock"]["deadline_s"].as<int>(), 15);
    EXPECT_FALSE(doc["agent"]["paired"].as<bool>());
}

TEST(Smoke, StatusModeAShape) {
    mcco::Identity id{"ProPresenter Mac", "mac-a1b2c3", "", "", "a1b2c3d4e5f6"};
    JsonDocument doc;
    mcco::build_status_mode_a(doc, id, true, true, 1736848800, 1736848800, 1736848812, 7);
    EXPECT_EQ(doc["cache_epoch"].as<int>(), 7);
    EXPECT_FALSE(doc["connection"]["agent"]["value"].as<bool>());
    EXPECT_EQ(std::string(doc["connection"]["agent"]["source"] | "?"), "esp32_direct");
    EXPECT_TRUE(doc["mac"]["state"]["value"].isNull());
    EXPECT_EQ(std::string(doc["mac"]["state"]["freshness"] | "?"), "unknown");
    EXPECT_EQ(doc["connection"]["network"]["ttl_s"].as<int>(), 30);
}

TEST(Smoke, StatusModeBProvenanceAndFreshness) {
    mcco::Identity id{"ProPresenter Mac", "mac-a1b2c3", "", "", "a1b2c3d4e5f6"};
    mcco::AgentStatus agent;
    agent.paired = true;
    agent.session_live = true;
    agent.last_frame_at = 1736848810;
    agent.has_system = true;
    agent.system_state = "awake";
    agent.system_at = 1736848810;
    agent.has_lock = true;
    agent.locked = false;
    agent.lock_at = 1736848810;
    agent.has_user = true;
    agent.user_logged_in = true;
    agent.user = "production";
    agent.user_at = 1736848810;
    agent.has_boot = true;
    agent.boot_id = "b_3F8A11";
    agent.boot_at = 1736848810;
    agent.apps["com.x"] = {true, 812, 1736848810};

    JsonDocument doc;
    mcco::build_status(doc, id, true, true, 1736848800, 1736848800, &agent,
                       1736848812, 8);
    EXPECT_TRUE(doc["connection"]["agent"]["value"].as<bool>());
    EXPECT_EQ(std::string(doc["mac"]["state"]["value"] | "?"), "awake");
    EXPECT_EQ(std::string(doc["mac"]["state"]["source"] | "?"), "agent_reported");
    EXPECT_EQ(std::string(doc["mac"]["state"]["freshness"] | "?"), "fresh");
    EXPECT_EQ(doc["mac"]["state"]["ttl_s"].as<int>(), 15);
    // boot_id is retained and never ages (spec 7.2.2).
    EXPECT_EQ(std::string(doc["mac"]["boot_id"]["freshness"] | "?"), "fresh");
    EXPECT_EQ(doc["mac"]["boot_id"]["ttl_s"].as<std::string>(), "null");
    EXPECT_EQ(std::string(doc["applications"]["com.x"]["state"]["value"] | "?"), "running");
    EXPECT_EQ(doc["applications"]["com.x"]["pid"]["value"].as<int>(), 812);

    // 20 s of silence: agent fields stale, connection.agent still true (value
    // only flips at the 30 s OFFLINE observation, spec worked example 2).
    mcco::build_status(doc, id, true, true, 1736848800, 1736848800, &agent,
                       1736848830, 9);
    EXPECT_TRUE(doc["connection"]["agent"]["value"].as<bool>());
    EXPECT_EQ(std::string(doc["mac"]["state"]["freshness"] | "?"), "stale");
    EXPECT_EQ(std::string(doc["mac"]["state"]["value"] | "?"), "awake"); // never erased

    // 31 s: OFFLINE — connection.agent flips false, agent fields stale.
    mcco::build_status(doc, id, true, true, 1736848800, 1736848800, &agent,
                       1736848841, 10);
    EXPECT_FALSE(doc["connection"]["agent"]["value"].as<bool>());
}

TEST(Smoke, AgentSystemInfoRendering) {
    mcco::AgentStatus st;
    JsonDocument doc;

    // Full evidence: every leaf present, disk survives >2^31.
    st.has_sysinfo = true;
    st.sysinfo_at = 1736848810;
    st.cpu_pct = 12.5;
    st.mem_pct = 45;
    st.disk_free_bytes = 200000000000ULL;
    st.net_reachable = true;
    st.net_ip = "10.0.0.2";
    st.has_mac_uptime = true;
    st.mac_uptime_s = 86400;
    st.boot_time = 1736762400;
    st.has_capability = true;
    st.os_version = "14.3";
    st.hardware_model = "Mac14,9";
    st.has_front_app = true;
    st.front_app = "com.example.app";
    doc.clear();
    mcco::build_agent_system_info(doc["system_info"].to<JsonObject>(), st);
    EXPECT_DOUBLE_EQ(doc["system_info"]["cpu_utilization_pct"].as<double>(), 12.5);
    EXPECT_DOUBLE_EQ(doc["system_info"]["memory_utilization_pct"].as<double>(), 45.0);
    EXPECT_EQ((long long)doc["system_info"]["disk_free_bytes"].as<long long>(), 200000000000LL);
    EXPECT_TRUE(doc["system_info"]["network"]["reachable"].as<bool>());
    EXPECT_EQ(std::string(doc["system_info"]["network"]["ip"] | "?"), "10.0.0.2");
    EXPECT_EQ((long long)doc["system_info"]["uptime_s"].as<long long>(), 86400);
    EXPECT_EQ((long long)doc["system_info"]["boot_time"].as<long long>(), 1736762400);
    EXPECT_EQ(std::string(doc["system_info"]["os_version"] | "?"), "14.3");
    EXPECT_EQ(std::string(doc["system_info"]["hardware_model"] | "?"), "Mac14,9");
    EXPECT_EQ(std::string(doc["system_info"]["front_app"] | "?"), "com.example.app");

    // Partial: sysinfo only; everything else null. Empty net_ip renders null.
    mcco::AgentStatus partial;
    partial.has_sysinfo = true;
    partial.net_reachable = false;  // net_ip stays empty -> null
    doc.clear();
    mcco::build_agent_system_info(doc["system_info"].to<JsonObject>(), partial);
    EXPECT_TRUE(doc["system_info"]["network"]["reachable"].is<bool>());
    EXPECT_TRUE(doc["system_info"]["network"]["ip"].isNull());
    EXPECT_TRUE(doc["system_info"]["uptime_s"].isNull());
    EXPECT_TRUE(doc["system_info"]["boot_time"].isNull());
    EXPECT_TRUE(doc["system_info"]["os_version"].isNull());
    EXPECT_TRUE(doc["system_info"]["hardware_model"].isNull());
    EXPECT_TRUE(doc["system_info"]["front_app"].isNull());

    // None: all leaves null, network object present with null members.
    mcco::AgentStatus none;
    doc.clear();
    mcco::build_agent_system_info(doc["system_info"].to<JsonObject>(), none);
    EXPECT_TRUE(doc["system_info"]["cpu_utilization_pct"].isNull());
    EXPECT_TRUE(doc["system_info"]["memory_utilization_pct"].isNull());
    EXPECT_TRUE(doc["system_info"]["disk_free_bytes"].isNull());
    EXPECT_TRUE(doc["system_info"]["network"]["reachable"].isNull());
    EXPECT_TRUE(doc["system_info"]["network"]["ip"].isNull());
    EXPECT_TRUE(doc["system_info"]["uptime_s"].isNull());
    EXPECT_TRUE(doc["system_info"]["boot_time"].isNull());
    EXPECT_TRUE(doc["system_info"]["os_version"].isNull());
    EXPECT_TRUE(doc["system_info"]["hardware_model"].isNull());
    EXPECT_TRUE(doc["system_info"]["front_app"].isNull());

    // Frontmost-app-cleared (null bundle_id) and capability-without-strings
    // still render honest nulls.
    mcco::AgentStatus cleared;
    cleared.has_front_app = true;   // front_app empty = none
    cleared.has_capability = true;  // strings empty = null
    doc.clear();
    mcco::build_agent_system_info(doc["system_info"].to<JsonObject>(), cleared);
    EXPECT_TRUE(doc["system_info"]["front_app"].isNull());
    EXPECT_TRUE(doc["system_info"]["os_version"].isNull());
    EXPECT_TRUE(doc["system_info"]["hardware_model"].isNull());
}

TEST(Smoke, CapabilitiesModeBHonesty) {
    mcco::Identity id{"ProPresenter Mac", "mac-a1b2c3", "", "", "a1b2c3d4e5f6"};
    mcco::AgentStatus agent;
    agent.paired = true;
    agent.session_live = true;
    agent.last_frame_at = 1736848810;
    agent.has_capability = true;
    agent.enabled_commands = {"launch_app", "quit_app"};
    agent.allowlisted_apps = {{"com.x", true}};

    JsonDocument doc;
    mcco::build_capabilities(doc, id, {"mac_01"}, &agent, 1736848812);
    EXPECT_EQ(std::string(doc["mode"] | "?"), "B");
    EXPECT_EQ(std::string(doc["capability_level"] | "?"), "L2");
    EXPECT_TRUE(doc["agent"]["paired"].as<bool>());
    EXPECT_TRUE(doc["agent"]["connected"].as<bool>());
    EXPECT_TRUE(doc["commands"]["app_launch"]["available"].as<bool>());
    EXPECT_TRUE(doc["commands"]["app_launch"]["verified"].as<bool>());
    EXPECT_EQ(doc["commands"]["app_launch"]["apps"].as<JsonArrayConst>().size(), 1);
    // Power/lock predicates are Phase 5: still unverified in Mode B.
    EXPECT_FALSE(doc["commands"]["lock"]["verified"].as<bool>());
    EXPECT_FALSE(doc["commands"]["restart"]["verified"].as<bool>());

    // 31 s later (offline): connected collapses, verified collapses with it
    // (spec 17.1.1 schema constraints).
    mcco::build_capabilities(doc, id, {"mac_01"}, &agent, 1736848841);
    EXPECT_FALSE(doc["agent"]["connected"].as<bool>());
    for (JsonPair kv : doc["commands"].as<JsonObject>()) {
        EXPECT_FALSE(kv.value()["verified"].as<bool>()) << kv.key().c_str();
    }
}
