// Keys, roles, and rate limiting per spec 13.1.1.
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_auth.h"
#include "../../lib/maccontrol_core/mc_clock.h"
#include "../../lib/maccontrol_core/mc_ids.h"
#include "../../lib/maccontrol_core/mc_rate_limit.h"
#include "../../lib/maccontrol_core/mc_sha256.h"
#include <set>
#include <string>

namespace {

struct FakeClock : mcco::IClock {
    uint64_t epoch = 1736848800; // 2025-01-14T09:20:00Z
    uint64_t mono_ms = 0;
    uint64_t epoch_seconds() const override { return epoch; }
    uint64_t millis() const override { return mono_ms; }
    void advance_seconds(uint64_t s) { epoch += s; }
    void advance_ms(uint64_t ms) { mono_ms += ms; }
};

struct FakeRandom : mcco::IRandom {
    uint8_t state = 0x42;
    void bytes(uint8_t* out, size_t len) override {
        for (size_t i = 0; i < len; i++) out[i] = state++;
    }
};

const char* kRawKey1 = "mck_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; // 47 chars
const char* kRawKey2 = "mck_BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";

TEST(KeyStore, AddAuthenticateAndRole) {
    mcco::KeyStore ks;
    std::string id;
    ASSERT_TRUE(ks.add(kRawKey1, mcco::Role::Control, "ops", 1736848800, id));
    EXPECT_EQ(id, "key-01");

    mcco::Principal p;
    EXPECT_EQ(ks.authenticate(kRawKey1, 1736848800, p), mcco::AuthResult::Ok);
    EXPECT_EQ(p.key_id, "key-01");
    EXPECT_EQ(p.role, mcco::Role::Control);

    // A different raw key matches no digest.
    EXPECT_EQ(ks.authenticate(kRawKey2, 1736848800, p), mcco::AuthResult::MissingOrInvalid);
    // Empty credential is missing, not invalid-by-content.
    EXPECT_EQ(ks.authenticate("", 1736848800, p), mcco::AuthResult::MissingOrInvalid);
}

TEST(KeyStore, RevokedKeyAuthenticatesAsRevoked) {
    mcco::KeyStore ks;
    std::string id;
    ASSERT_TRUE(ks.add(kRawKey1, mcco::Role::Admin, "root", 1736848800, id));
    ASSERT_TRUE(ks.revoke(id));
    EXPECT_FALSE(ks.revoke(id));       // already revoked
    EXPECT_FALSE(ks.revoke("key-99")); // unknown

    mcco::Principal p;
    EXPECT_EQ(ks.authenticate(kRawKey1, 1736848800, p), mcco::AuthResult::Revoked);
}

TEST(KeyStore, MaxEightActiveKeys) {
    mcco::KeyStore ks;
    std::string id;
    for (int i = 0; i < 8; i++) {
        std::string raw = "mck_key" + std::to_string(i) +
                          std::string(40, char('a' + i));
        ASSERT_TRUE(ks.add(raw, mcco::Role::Read, "k", 1736848800, id)) << i;
    }
    EXPECT_EQ(ks.active_count(), 8u);
    EXPECT_EQ(id, "key-08");

    // 9th active key is refused.
    EXPECT_FALSE(ks.add(kRawKey1, mcco::Role::Read, "overflow", 1736848800, id));
    EXPECT_EQ(ks.active_count(), 8u);

    // Revoking one frees a slot.
    ASSERT_TRUE(ks.revoke("key-03"));
    ASSERT_TRUE(ks.add(kRawKey1, mcco::Role::Read, "replacement", 1736848800, id));
    EXPECT_EQ(id, "key-09"); // ids are assigned monotonically
    EXPECT_EQ(ks.active_count(), 8u);
}

TEST(KeyStore, ExpiredKeyNeverDowngraded) {
    mcco::KeyStore ks;
    std::string id;
    ASSERT_TRUE(ks.add(kRawKey1, mcco::Role::Read, "temp", 1736848800, id));

    // KeyStore::add takes no expires_at; the glue sets it on the record.
    const_cast<mcco::KeyRecord&>(ks.records().front()).expires_at = 1736848900;

    mcco::Principal p;
    EXPECT_EQ(ks.authenticate(kRawKey1, 1736848899, p), mcco::AuthResult::Ok);
    EXPECT_EQ(ks.authenticate(kRawKey1, 1736848900, p), mcco::AuthResult::Expired);
    // Expiry yields Expired (401), never a role downgrade to Ok.
    EXPECT_NE(ks.authenticate(kRawKey1, 1736848900, p), mcco::AuthResult::Ok);
}

TEST(KeyStore, OnlyDigestIsStored) {
    mcco::KeyStore ks;
    std::string id;
    ASSERT_TRUE(ks.add(kRawKey1, mcco::Role::Admin, "root", 1736848800, id));

    const mcco::KeyRecord& rec = ks.records().front();
    EXPECT_EQ(rec.key_sha256, mcco::Sha256::hex_digest(kRawKey1));
    EXPECT_NE(rec.key_sha256, kRawKey1); // raw key itself is not stored
    EXPECT_EQ(rec.key_sha256.size(), 64u);
    // No record field carries the raw key.
    EXPECT_NE(rec.label, kRawKey1);
    EXPECT_NE(rec.key_id, kRawKey1);
}

TEST(Ids, ApiKeyFormatAndDecode) {
    FakeRandom rng;
    const std::string key = mcco::make_api_key(rng);

    ASSERT_EQ(key.size(), 47u); // "mck_" + 43 base64url chars (32 bytes unpadded)
    EXPECT_EQ(key.substr(0, 4), "mck_");

    uint8_t decoded[32];
    size_t decoded_len = 0;
    ASSERT_TRUE(mcco::base64url_decode(key.substr(4), decoded, decoded_len));
    EXPECT_EQ(decoded_len, 32u);

    // Distinct calls yield distinct keys.
    const std::string key2 = mcco::make_api_key(rng);
    EXPECT_NE(key, key2);
}

TEST(Ids, CommandIdFormatAndDistinctness) {
    // Crockford Base32: 0-9 and A-Z without I, L, O, U.
    const std::string alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

    FakeRandom rng;
    std::set<std::string> seen;
    for (int i = 0; i < 64; i++) {
        const std::string id = mcco::make_command_id(rng);
        ASSERT_EQ(id.size(), 8u);
        EXPECT_NE(alphabet.find(id[0]), std::string::npos) << id;
        for (char c : id) EXPECT_NE(alphabet.find(c), std::string::npos) << id;
        EXPECT_TRUE(seen.insert(id).second) << "duplicate command_id " << id;
    }
}

TEST(RateLimiter, BucketRefillsAfterWindow) {
    FakeClock clock;
    mcco::RateLimiter limiter(clock);

    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_FALSE(limiter.allow_custom("key-01", 3)); // 4th in-window request denied

    clock.advance_ms(60000); // window elapsed -> bucket refills
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_FALSE(limiter.allow_custom("key-01", 3));

    // Just before the window rolls over it is still denied.
    clock.advance_ms(59999);
    EXPECT_FALSE(limiter.allow_custom("key-01", 3));
    clock.advance_ms(1);
    EXPECT_TRUE(limiter.allow_custom("key-01", 3));
}

TEST(RateLimiter, BucketsAreIndependentPerKey) {
    FakeClock clock;
    mcco::RateLimiter limiter(clock);

    for (int i = 0; i < 3; i++) EXPECT_TRUE(limiter.allow_custom("key-01", 3));
    EXPECT_FALSE(limiter.allow_custom("key-01", 3));

    // A different key has its own bucket.
    EXPECT_TRUE(limiter.allow_custom("key-02", 3));
    EXPECT_TRUE(limiter.allow_custom("key-02", 3));

    // Role defaults: READ 60, CONTROL 30, ADMIN 10 per minute.
    EXPECT_EQ(mcco::RateLimiter::default_per_minute(mcco::Role::Read), 60u);
    EXPECT_EQ(mcco::RateLimiter::default_per_minute(mcco::Role::Control), 30u);
    EXPECT_EQ(mcco::RateLimiter::default_per_minute(mcco::Role::Admin), 10u);
    EXPECT_TRUE(limiter.allow("key-01", mcco::Role::Read));
}

TEST(Roles, StrictTotalOrderReadControlAdmin) {
    using mcco::Role;
    EXPECT_TRUE(mcco::role_at_least(Role::Read, Role::Read));
    EXPECT_FALSE(mcco::role_at_least(Role::Read, Role::Control));
    EXPECT_FALSE(mcco::role_at_least(Role::Read, Role::Admin));
    EXPECT_TRUE(mcco::role_at_least(Role::Control, Role::Read));
    EXPECT_TRUE(mcco::role_at_least(Role::Control, Role::Control));
    EXPECT_FALSE(mcco::role_at_least(Role::Control, Role::Admin));
    EXPECT_TRUE(mcco::role_at_least(Role::Admin, Role::Read));
    EXPECT_TRUE(mcco::role_at_least(Role::Admin, Role::Control));
    EXPECT_TRUE(mcco::role_at_least(Role::Admin, Role::Admin));

    // String round trip.
    Role r;
    ASSERT_TRUE(mcco::role_from_string("READ", r));
    EXPECT_EQ(r, Role::Read);
    ASSERT_TRUE(mcco::role_from_string("CONTROL", r));
    EXPECT_EQ(r, Role::Control);
    ASSERT_TRUE(mcco::role_from_string("ADMIN", r));
    EXPECT_EQ(r, Role::Admin);
    EXPECT_FALSE(mcco::role_from_string("SUPER", r));
    EXPECT_STREQ(mcco::role_to_string(Role::Admin), "ADMIN");
}

} // namespace
