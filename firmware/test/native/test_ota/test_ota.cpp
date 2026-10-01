// Signed OTA container helpers and the spec 15.3 apply termination path
// (container validation, version regression compare, force-terminate).
// The mbedTLS signature verify itself is device-only (src/ota.cpp);
// scripts/ota_sign.py covers it host-side.
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_engine.h"
#include "../../lib/maccontrol_core/mc_ota.h"
#include <string>
#include <vector>

namespace {

TEST(OtaContainer, SizeGate) {
    // 64-byte signature + at least one image byte, bounded by the slot size.
    EXPECT_FALSE(mcco::ota_container_size_valid(0, 0x1F0000));
    EXPECT_FALSE(mcco::ota_container_size_valid(64, 0x1F0000));   // empty image
    EXPECT_TRUE(mcco::ota_container_size_valid(65, 0x1F0000));
    EXPECT_TRUE(mcco::ota_container_size_valid(0x1F0000, 0x1F0000));
    EXPECT_FALSE(mcco::ota_container_size_valid(0x1F0001, 0x1F0000)); // > slot
    // image split: 64-byte header, rest is the image
    EXPECT_EQ(mcco::ota_image_offset(), 64u);
    EXPECT_EQ(mcco::ota_image_len(64 + 1394944), 1394944u);
    EXPECT_EQ(mcco::ota_image_len(10), 0u); // underflow guard
}

TEST(OtaVersion, Parse) {
    const mcco::OtaVersion v = mcco::ota_parse_version("1.6.0-phase6");
    EXPECT_TRUE(v.numeric);
    EXPECT_EQ(v.major, 1u);
    EXPECT_EQ(v.minor, 6u);
    EXPECT_EQ(v.patch, 0u);

    const mcco::OtaVersion short_v = mcco::ota_parse_version("2.1");
    EXPECT_TRUE(short_v.numeric);
    EXPECT_EQ(short_v.major, 2u);
    EXPECT_EQ(short_v.minor, 1u);
    EXPECT_EQ(short_v.patch, 0u);

    const mcco::OtaVersion vprefixed = mcco::ota_parse_version("v1.2.3");
    EXPECT_TRUE(vprefixed.numeric);
    EXPECT_EQ(vprefixed.major, 1u);

    const mcco::OtaVersion junk = mcco::ota_parse_version("dev");
    EXPECT_FALSE(junk.numeric);

    const mcco::OtaVersion null_v = mcco::ota_parse_version(nullptr);
    EXPECT_FALSE(null_v.numeric);
}

TEST(OtaVersion, CompareAndDowngrade) {
    EXPECT_TRUE(mcco::ota_is_downgrade("1.5.9", "1.6.0"));
    EXPECT_TRUE(mcco::ota_is_downgrade("1.6.0", "1.6.1"));
    EXPECT_TRUE(mcco::ota_is_downgrade("0.9.0", "1.0.0"));
    EXPECT_FALSE(mcco::ota_is_downgrade("1.6.0", "1.6.0"));
    EXPECT_FALSE(mcco::ota_is_downgrade("1.7.0", "1.6.0"));
    EXPECT_FALSE(mcco::ota_is_downgrade("1.6.0", "dev"));  // unknown: not logged
    EXPECT_FALSE(mcco::ota_is_downgrade("dev", "1.6.0"));  // unknown: not logged
}

// ---- engine force-terminate (spec 15.3) -----------------------------------

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800;
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

mcco::Submission lock_sub(const std::string& hash) {
    mcco::Submission sub;
    sub.type = mcco::CommandType::Lock;
    sub.parameters_json = "{}";
    sub.requested_by = "apikey:key-01";
    sub.body_hash = hash;
    return sub;
}

TEST(OtaApply, TerminateAllNonTerminalMatchesReconcileVerdict) {
    FakeClock clock;
    FakeRandom rng;
    MemStorage storage;
    mcco::NullLog log;
    mcco::Ledger ledger{storage, clock, log, 64};
    mcco::CommandEngine engine{ledger, clock, rng, log};
    engine.init();

    mcco::SubmissionOutcome o1 = engine.submit(lock_sub("h1"));
    mcco::SubmissionOutcome o2 = engine.submit(lock_sub("h2"));
    ASSERT_TRUE(o1.ok && o2.ok);
    EXPECT_EQ(engine.count_non_terminal(), 2u);

    // Mode A lock terminates immediately on successful dispatch
    // (unconfirmed/hid_only) — already terminal, apply must not touch it.
    ASSERT_TRUE(engine.complete_dispatch(o1.record.command_id, /*ok=*/true));
    EXPECT_EQ(engine.count_non_terminal(), 1u);

    // apply {force:true}: the remaining non-terminal record gets exactly the
    // verdict reconcile_boot() assigns in-flight records across a reboot.
    const size_t n = engine.terminate_all_non_terminal();
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(engine.count_non_terminal(), 0u);
    const mcco::CommandRecord* r1 = engine.get(o1.record.command_id);
    const mcco::CommandRecord* r2 = engine.get(o2.record.command_id);
    ASSERT_TRUE(r1 && r2);
    EXPECT_EQ(r1->state, mcco::CommandState::Unconfirmed); // untouched
    EXPECT_TRUE(r1->error_code.empty());
    EXPECT_EQ(r2->state, mcco::CommandState::Failed);
    EXPECT_EQ(r2->error_code, "esp32_restarted");
    // Terminal records are never touched again.
    EXPECT_EQ(engine.terminate_all_non_terminal(), 0u);
}

TEST(OtaApply, TerminatedRecordsStayTerminalAcrossReload) {
    // The force-terminate verdict must be durable: a reload + reconcile_boot
    // (the same path a post-OTA boot runs) leaves the record terminal.
    MemStorage storage;
    {
        FakeClock clock;
        FakeRandom rng;
        mcco::NullLog log;
        mcco::Ledger ledger{storage, clock, log, 64};
        mcco::CommandEngine engine{ledger, clock, rng, log};
        engine.init();
        mcco::SubmissionOutcome o = engine.submit(lock_sub("h1"));
        ASSERT_TRUE(o.ok);
        EXPECT_EQ(engine.terminate_all_non_terminal(), 1u);
    }
    {
        FakeClock clock;
        FakeRandom rng;
        mcco::NullLog log;
        mcco::Ledger ledger{storage, clock, log, 64};
        mcco::CommandEngine engine{ledger, clock, rng, log};
        ASSERT_TRUE(engine.init()); // boot reconciliation runs here
        EXPECT_EQ(engine.count_non_terminal(), 0u);
    }
}

} // namespace
