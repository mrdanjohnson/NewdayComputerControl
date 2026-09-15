// Status and capabilities documents, hostname validation, the error table,
// and SHA-256 / ISO8601 vectors (spec 4.1.1, 7, 12.3.1).
#include <gtest/gtest.h>
#include "../../lib/maccontrol_core/mc_error.h"
#include "../../lib/maccontrol_core/mc_iso8601.h"
#include "../../lib/maccontrol_core/mc_sha256.h"
#include "../../lib/maccontrol_core/mc_status.h"
#include <set>
#include <string>

namespace {

const mcco::Identity kIdentity{"ProPresenter Mac", "mac-a1b2c3", "Stage booth", "",
                               "a1b2c3d4e5f6"};

TEST(Docs, CapabilitiesModeAExactCommandSurface) {
    JsonDocument doc;
    mcco::build_capabilities_mode_a(doc, kIdentity);

    EXPECT_EQ(std::string(doc["api_version"] | "?"), "v1");
    EXPECT_EQ(std::string(doc["mode"] | "?"), "A");
    EXPECT_EQ(std::string(doc["capability_level"] | "?"), "L1");
    EXPECT_EQ(std::string(doc["device"]["name"] | "?"), kIdentity.device_name);
    EXPECT_EQ(std::string(doc["device"]["hostname"] | "?"), kIdentity.hostname);

    JsonObject cmds = doc["commands"].as<JsonObject>();
    std::set<std::string> names;
    for (JsonPair kv : cmds) names.insert(kv.key().c_str());
    EXPECT_EQ(names, (std::set<std::string>{"wake", "sleep", "restart", "shutdown", "lock",
                                            "macro_execute", "app_launch", "app_quit"}));

    // Every entry is verified:false in Mode A (nothing verifiable without MCA).
    for (JsonPair kv : cmds)
        EXPECT_FALSE(kv.value()["verified"].as<bool>()) << kv.key().c_str();

    // Power commands available with per-type deadlines (spec 5.3.1).
    EXPECT_TRUE(cmds["wake"]["available"].as<bool>());
    EXPECT_EQ(cmds["wake"]["deadline_s"].as<int>(), 120);
    EXPECT_TRUE(cmds["sleep"]["available"].as<bool>());
    EXPECT_EQ(cmds["sleep"]["deadline_s"].as<int>(), 90);
    EXPECT_TRUE(cmds["restart"]["available"].as<bool>());
    EXPECT_EQ(cmds["restart"]["deadline_s"].as<int>(), 180);
    EXPECT_TRUE(cmds["shutdown"]["available"].as<bool>());
    EXPECT_EQ(cmds["shutdown"]["deadline_s"].as<int>(), 120);
    EXPECT_TRUE(cmds["lock"]["available"].as<bool>());
    EXPECT_EQ(cmds["lock"]["deadline_s"].as<int>(), 15);

    // Phase 1: macros and agent-dependent commands unavailable.
    EXPECT_FALSE(cmds["macro_execute"]["available"].as<bool>());
    EXPECT_FALSE(cmds["app_launch"]["available"].as<bool>());
    EXPECT_FALSE(cmds["app_quit"]["available"].as<bool>());

    // No agent in Mode A.
    EXPECT_FALSE(doc["agent"]["paired"].as<bool>());
    EXPECT_FALSE(doc["agent"]["connected"].as<bool>());
    EXPECT_EQ(doc["agent"]["enabled_commands"].as<JsonArrayConst>().size(), 0u);
    EXPECT_EQ(doc["agent"]["allowlisted_apps"].as<JsonArrayConst>().size(), 0u);
}

TEST(Docs, StatusModeAEveryLeafIsFiveTuple) {
    const uint64_t now = 1736848812;
    const uint64_t identity_observed_at = 1736848800;
    const uint64_t network_probe_at = 1736848805;

    JsonDocument doc;
    mcco::build_status_mode_a(doc, kIdentity, /*usb_up=*/true, /*network_up=*/true,
                              network_probe_at, identity_observed_at, now,
                              /*cache_epoch=*/7);

    EXPECT_EQ(std::string(doc["generated_at"] | "?"), mcco::iso8601_format(now));
    EXPECT_EQ(doc["cache_epoch"].as<int>(), 7);

    const char* kTupleKeys[] = {"value", "source", "observed_at", "ttl_s", "freshness"};
    auto expect_tuple = [&](const char* group, const char* leaf) {
        ASSERT_TRUE(doc[group][leaf].is<JsonObjectConst>()) << group << "." << leaf;
        JsonObjectConst t = doc[group][leaf].as<JsonObjectConst>();
        std::set<std::string> keys;
        for (JsonPairConst kv : t) keys.insert(kv.key().c_str());
        EXPECT_EQ(keys, (std::set<std::string>{"value", "source", "observed_at", "ttl_s",
                                               "freshness"}))
            << group << "." << leaf;
    };

    for (const char* leaf : {"name", "hostname"}) expect_tuple("device", leaf);
    for (const char* leaf : {"usb", "network", "agent"}) expect_tuple("connection", leaf);
    for (const char* leaf : {"state", "locked", "user_logged_in", "user", "boot_id"})
        expect_tuple("mac", leaf);

    // device.* : esp32_direct, no ttl, fresh, timestamped by identity_observed_at.
    JsonObjectConst device_name = doc["device"]["name"].as<JsonObjectConst>();
    EXPECT_EQ(std::string(device_name["value"] | "?"), kIdentity.device_name);
    EXPECT_EQ(std::string(device_name["source"] | "?"), "esp32_direct");
    EXPECT_TRUE(device_name["ttl_s"].isNull());
    EXPECT_EQ(std::string(device_name["observed_at"] | "?"), mcco::iso8601_format(identity_observed_at));
    EXPECT_EQ(std::string(device_name["freshness"] | "?"), "fresh");

    // connection.agent: false / esp32_direct. connection.network: ttl 30 / network_probe.
    JsonObjectConst agent = doc["connection"]["agent"].as<JsonObjectConst>();
    EXPECT_FALSE(agent["value"].as<bool>());
    EXPECT_EQ(std::string(agent["source"] | "?"), "esp32_direct");
    JsonObjectConst network = doc["connection"]["network"].as<JsonObjectConst>();
    EXPECT_TRUE(network["value"].as<bool>());
    EXPECT_EQ(std::string(network["source"] | "?"), "network_probe");
    EXPECT_EQ(network["ttl_s"].as<int>(), 30);
    EXPECT_EQ(std::string(network["observed_at"] | "?"), mcco::iso8601_format(network_probe_at));

    // mac.* : value null / source unknown / freshness unknown (honest Mode A).
    for (const char* leaf : {"state", "locked", "user_logged_in", "user", "boot_id"}) {
        JsonObjectConst t = doc["mac"][leaf].as<JsonObjectConst>();
        EXPECT_TRUE(t["value"].isNull()) << leaf;
        EXPECT_EQ(std::string(t["source"] | "?"), "unknown") << leaf;
        EXPECT_TRUE(t["observed_at"].isNull()) << leaf;
        EXPECT_TRUE(t["ttl_s"].isNull()) << leaf;
        EXPECT_EQ(std::string(t["freshness"] | "?"), "unknown") << leaf;
    }

    // applications is an empty object in Phase 1.
    ASSERT_TRUE(doc["applications"].is<JsonObjectConst>());
    size_t app_count = 0;
    for (JsonPairConst kv : doc["applications"].as<JsonObjectConst>()) (void)kv, ++app_count;
    EXPECT_EQ(app_count, 0u);
}

TEST(Docs, HostnameValidation) {
    // Accept: 1-57 chars, [a-z0-9-], start/end alphanumeric.
    EXPECT_TRUE(mcco::hostname_valid("mac-a1b2c3"));
    EXPECT_TRUE(mcco::hostname_valid("a"));
    EXPECT_TRUE(mcco::hostname_valid(std::string(57, 'a')));
    EXPECT_TRUE(mcco::hostname_valid("a-b-c-0-9"));

    // Reject: empty, too long, uppercase, bad edges, spaces.
    EXPECT_FALSE(mcco::hostname_valid(""));
    EXPECT_FALSE(mcco::hostname_valid(std::string(58, 'a')));
    EXPECT_FALSE(mcco::hostname_valid("Mac-A1B2C3"));
    EXPECT_FALSE(mcco::hostname_valid("-mac-a1b2c3"));
    EXPECT_FALSE(mcco::hostname_valid("mac-a1b2c3-"));
    EXPECT_FALSE(mcco::hostname_valid("mac a1b2c3"));
    EXPECT_FALSE(mcco::hostname_valid("mac_a1b2c3"));
}

TEST(Docs, ErrorTableIsClosedAndTotal) {
    struct Case {
        mcco::ErrCode code;
        int http_status;
        const char* code_string;
    };
    const Case cases[] = {
        {mcco::ErrCode::BadRequest, 400, "bad_request"},
        {mcco::ErrCode::Unauthorized, 401, "unauthorized"},
        {mcco::ErrCode::Forbidden, 403, "forbidden"},
        {mcco::ErrCode::NotFound, 404, "not_found"},
        {mcco::ErrCode::Conflict, 409, "conflict"},
        {mcco::ErrCode::AgentNotPaired, 409, "agent_not_paired"},
        {mcco::ErrCode::RateLimited, 429, "rate_limited"},
        {mcco::ErrCode::InternalError, 500, "internal_error"},
        {mcco::ErrCode::LedgerUnavailable, 503, "ledger_unavailable"},
        {mcco::ErrCode::NetworkUnavailable, 503, "network_unavailable"},
    };
    for (const Case& tc : cases) {
        EXPECT_EQ(mcco::error_http_status(tc.code), tc.http_status) << tc.code_string;
        EXPECT_STREQ(mcco::error_code_string(tc.code), tc.code_string);
        EXPECT_STRNE(mcco::default_error_message(tc.code), "");
    }
}

TEST(Docs, Sha256ExtraVectors) {
    EXPECT_EQ(mcco::Sha256::hex_digest(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // NOTE: the digest for 'a' * 56 was cross-checked against Python hashlib
    // (b35439a4...). A different value circulated in the task spec
    // (3a985da7...); it matches no standard input and is not used here.
    const std::string kA56 =
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a";
    EXPECT_EQ(mcco::Sha256::hex_digest(std::string(56, 'a')), kA56);

    // Streaming across the 64-byte block boundary must give the same digest.
    auto hex = [](const uint8_t* b, size_t n) {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; i++) {
            s += d[b[i] >> 4];
            s += d[b[i] & 0xf];
        }
        return s;
    };
    mcco::Sha256 h;
    h.update(std::string(40, 'a'));
    h.update(std::string(16, 'a'));
    uint8_t dig[32];
    h.final(dig);
    EXPECT_EQ(hex(dig, sizeof(dig)), kA56);
}

TEST(Docs, Iso8601RejectsNonconformingInput) {
    uint64_t t = 0;
    EXPECT_FALSE(mcco::iso8601_parse("2025-13-14T09:41:12Z", t)); // month 13
    EXPECT_FALSE(mcco::iso8601_parse("2025-1-14T09:41:12Z", t));  // unpadded month
    EXPECT_FALSE(mcco::iso8601_parse("2025-01-14T09:41:12Zjunk", t)); // trailing junk
    EXPECT_FALSE(mcco::iso8601_parse("2025-01-14 09:41:12Z", t)); // wrong separator
    EXPECT_FALSE(mcco::iso8601_parse("2025-01-14T25:41:12Z", t)); // hour 25
}

TEST(Docs, Iso8601RoundTripAcrossEpochs) {
    const char* samples[] = {
        "1970-01-01T00:00:00Z",
        "2024-02-29T12:34:56Z", // leap day
        "2025-01-14T09:20:00Z",
        "2038-01-19T03:14:07Z",
        "2100-03-01T00:00:00Z", // 2100 is not a leap year
    };
    for (const char* s : samples) {
        uint64_t epoch = 0;
        ASSERT_TRUE(mcco::iso8601_parse(s, epoch)) << s;
        EXPECT_EQ(mcco::iso8601_format(epoch), s) << s;
    }
}

} // namespace
