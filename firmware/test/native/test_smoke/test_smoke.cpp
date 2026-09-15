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
    mcco::build_capabilities_mode_a(doc, id);
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
