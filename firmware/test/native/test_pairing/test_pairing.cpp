#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_pairing.h"
#include "../../lib/maccontrol_core/mc_sha1.h"
#include "../../lib/maccontrol_core/mc_ids.h"
#include <string>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800; // 2025-01-14T09:20:00Z
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
};

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x11;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

bool code_charset_ok(const std::string& code) {
    if (code.size() != 8) return false;
    for (char c : code) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9');
        if (!ok) return false;
        // ambiguous glyphs excluded (spec 3.2.1)
        if (c == '0' || c == 'O' || c == '1' || c == 'I') return false;
    }
    return true;
}

} // namespace

TEST(Pairing, Sha1AndBase64Vectors) {
    // RFC 3174 test vector: SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    mcco::Sha1 s;
    s.update("abc");
    uint8_t d[20];
    s.final(d);
    EXPECT_EQ(mcco::hex_encode(d, 20), "a9993e364706816aba3e25717850c26c9cd0d89d");
    // RFC 6455 worked example: base64(SHA-1(nonce + GUID)) with the nonce
    // "dGhlIHNhbXBsZSBub25jZQ==" yields "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
    mcco::Sha1 s2;
    s2.update("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    s2.final(d);
    EXPECT_EQ(mcco::base64_encode(d, 20), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_EQ(mcco::base64_encode(reinterpret_cast<const uint8_t*>("f"), 1), "Zg==");
}

TEST(Pairing, WindowLifecycleAndCode) {
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    EXPECT_EQ(store.state(), mcco::PairingState::Unpaired);
    EXPECT_FALSE(store.openWindow(30));   // below the 60 s bound
    EXPECT_FALSE(store.openWindow(601));  // above the 600 s bound
    ASSERT_TRUE(store.openWindow(120));
    EXPECT_EQ(store.state(), mcco::PairingState::PairingWindow);
    EXPECT_TRUE(code_charset_ok(store.pairingCode()));
    EXPECT_GT(store.windowSecondsRemaining(), 0);

    clock.mono_ms += 121000; // window expires (monotonic lifetime)
    EXPECT_EQ(store.state(), mcco::PairingState::Unpaired);
    EXPECT_EQ(store.validateCode("WHATEVER"), mcco::PairCodeResult::NoWindow);
}

TEST(Pairing, WindowSurvivesEpochJump) {
    // SNTP first sync after boot jumps epoch forward ~months; the pre-sync
    // fallback timeline starts at 2025-01-01. A window opened pre-sync must
    // NOT insta-expire when the jump lands (intervals are monotonic).
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store.openWindow(120));
    clock.epoch += 90ULL * 24 * 3600; // simulated SNTP jump (+90 days)
    EXPECT_EQ(store.state(), mcco::PairingState::PairingWindow);
    EXPECT_GT(store.windowSecondsRemaining(), 100);
    ASSERT_EQ(store.validateCode(store.pairingCode()), mcco::PairCodeResult::Ok);
}

TEST(Pairing, ValidateThrottleAndFiveFailuresClose) {
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store.openWindow(120));
    const std::string good = store.pairingCode();

    // One attempt per second: the second immediate attempt is rate-limited
    // and does NOT count as a failure.
    EXPECT_EQ(store.validateCode("ZZZZZZZZ"), mcco::PairCodeResult::WrongCode);
    EXPECT_EQ(store.validateCode("ZZZZZZZZ"), mcco::PairCodeResult::RateLimited);
    EXPECT_EQ(store.windowFailedAttempts(), 1);

    for (int i = 0; i < 3; i++) { // total 4 failures
        clock.mono_ms += 1000;
        EXPECT_EQ(store.validateCode("ZZZZZZZZ"), mcco::PairCodeResult::WrongCode);
    }
    clock.mono_ms += 1000;
    EXPECT_EQ(store.validateCode("ZZZZZZZZ"), mcco::PairCodeResult::WrongCode);
    EXPECT_EQ(store.windowFailedAttempts(), 5);
    // Window closed by the 5th failure; the code is dead even though it was
    // the correct one.
    EXPECT_FALSE(store.windowOpen());
    EXPECT_EQ(store.validateCode(good), mcco::PairCodeResult::NoWindow);
}

TEST(Pairing, CompleteStoresDigestOnlyAndExactlyOneActive) {
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store.openWindow(120));
    const std::string code = store.pairingCode();
    ASSERT_EQ(store.validateCode(code), mcco::PairCodeResult::Ok);
    std::string token = store.completePairing("ag-7e21");
    ASSERT_FALSE(token.empty());
    EXPECT_EQ(store.state(), mcco::PairingState::Active);
    EXPECT_FALSE(store.tokenMatches("bogus-token"));
    EXPECT_TRUE(store.tokenMatches(token));
    EXPECT_TRUE(store.instanceMatches("ag-7e21"));
    EXPECT_FALSE(store.instanceMatches("ag-other"));
    const mcco::PairingRecord* rec = store.activeRecord();
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->device_id, "a1b2c3d4e5f6");
    EXPECT_EQ(rec->agent_instance_id, "ag-7e21");
    EXPECT_EQ(rec->token_sha256.size(), 64);
    // The raw token must not appear anywhere in the persisted dump.
    EXPECT_EQ(store.dump().find(token), std::string::npos);

    // Re-pairing implicitly revokes the incumbent (exactly-one rule).
    ASSERT_TRUE(store.openWindow(120));
    ASSERT_EQ(store.validateCode(store.pairingCode()), mcco::PairCodeResult::Ok);
    std::string token2 = store.completePairing("ag-9f00");
    EXPECT_FALSE(token2.empty());
    EXPECT_NE(token2, token);
    EXPECT_FALSE(store.tokenMatches(token)); // old token dead immediately
    EXPECT_TRUE(store.tokenMatches(token2));
    EXPECT_TRUE(store.instanceMatches("ag-9f00"));
}

TEST(Pairing, RevokeAndPurgeAfterThirtyDays) {
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store.openWindow(120));
    ASSERT_EQ(store.validateCode(store.pairingCode()), mcco::PairCodeResult::Ok);
    std::string token = store.completePairing("ag-7e21");
    ASSERT_TRUE(store.revoke());
    EXPECT_EQ(store.state(), mcco::PairingState::Revoked);
    EXPECT_FALSE(store.tokenMatches(token)); // revoked token rejected same second
    // Spec 4.3.1: known-but-revoked is distinguishable from unknown (403 vs
    // 401 on the polling transport).
    EXPECT_TRUE(store.tokenDigestKnown(token));
    EXPECT_FALSE(store.tokenDigestKnown("totally-unknown-token"));
    EXPECT_FALSE(store.revoke());            // only active records revoke
    clock.epoch += 30ULL * 24 * 3600 + 1;    // purge after 30 days
    EXPECT_EQ(store.state(), mcco::PairingState::Unpaired);
    EXPECT_EQ(store.activeRecord(), nullptr);
}

TEST(Pairing, RestoreNeverResurrectsAWindow) {
    FakeClock clock;
    FakeRandom rng;
    mcco::PairingStore store(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store.openWindow(120));
    std::string dump = store.dump();
    mcco::PairingStore store2(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store2.restore(dump));
    EXPECT_EQ(store2.state(), mcco::PairingState::Unpaired); // window dies at boot

    // Active record round-trips.
    ASSERT_TRUE(store.openWindow(120));
    ASSERT_EQ(store.validateCode(store.pairingCode()), mcco::PairCodeResult::Ok);
    std::string token = store.completePairing("ag-7e21");
    std::string dump2 = store.dump();
    mcco::PairingStore store3(rng, clock, "a1b2c3d4e5f6");
    ASSERT_TRUE(store3.restore(dump2));
    EXPECT_EQ(store3.state(), mcco::PairingState::Active);
    EXPECT_TRUE(store3.tokenMatches(token));
    EXPECT_EQ(store3.activeRecord()->agent_instance_id, "ag-7e21");
}
