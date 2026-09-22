// The OpenAPI 3.1 integration contract (spec 12.1.1/17.1.1): parses, covers
// every Chapter 12 path plus the amendment set, and carries the amendment
// note. The document must stay byte-identical to the routes registered in
// src/http_api.cpp.
#include <gtest/gtest.h>
#include <ArduinoJson.h>
#include "../../lib/maccontrol_core/mc_openapi.h"
#include <set>
#include <string>

namespace {

TEST(Openapi, ParsesAndIsVersion31) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, mcco::kOpenApiJson, DeserializationOption::NestingLimit(32)), DeserializationError::Ok);
    EXPECT_TRUE(std::string(doc["openapi"] | "").compare(0, 3, "3.1") == 0);
    EXPECT_FALSE(std::string(doc["info"]["title"] | "").empty());
}

TEST(Openapi, CoversChapter12Paths) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, mcco::kOpenApiJson, DeserializationOption::NestingLimit(32)), DeserializationError::Ok);
    JsonObject paths = doc["paths"].as<JsonObject>();
    const std::set<std::string> required = {
        "/api/v1/status",
        "/api/v1/capabilities",
        "/api/v1/commands",
        "/api/v1/commands/{command_id}",
        "/api/v1/system/{command}",
        "/api/v1/macros",
        "/api/v1/macros/{macro_id}",
        "/api/v1/macros/{macro_id}/execute",
        "/api/v1/agent/status",
        "/api/v1/logs",
        "/api/v1/openapi.json",
        "/api/v1/apps/{bundle_id}/launch",
        "/api/v1/apps/{bundle_id}/quit",
    };
    for (const std::string& p : required) {
        EXPECT_TRUE(paths[p].is<JsonObject>()) << "missing path " << p;
    }
}

TEST(Openapi, CoversAmendmentPaths) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, mcco::kOpenApiJson, DeserializationOption::NestingLimit(32)), DeserializationError::Ok);
    JsonObject paths = doc["paths"].as<JsonObject>();
    const std::set<std::string> amendments = {
        "/api/v1/triggers",
        "/api/v1/triggers/{trigger_id}",
        "/api/v1/device/identity",
        "/api/v1/keys",
        "/api/v1/keys/{key_id}",
        "/ui/session",
        "/ui/login",
        "/ui/logout",
        "/ui/password",
        "/",
    };
    for (const std::string& p : amendments) {
        EXPECT_TRUE(paths[p].is<JsonObject>()) << "missing amendment path " << p;
    }
    // The amendment note must be present and name the additions (spec 17.1.1).
    const std::string desc = doc["info"]["description"] | "";
    EXPECT_TRUE(desc.find("AMENDMENT") != std::string::npos);
    EXPECT_TRUE(desc.find("/api/v1/triggers") != std::string::npos);
    EXPECT_TRUE(desc.find("/api/v1/keys") != std::string::npos);
}

TEST(Openapi, ErrorEnvelopeAndLogEntrySchemas) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, mcco::kOpenApiJson, DeserializationOption::NestingLimit(32)), DeserializationError::Ok);
    JsonObject schemas = doc["components"]["schemas"].as<JsonObject>();
    EXPECT_TRUE(schemas["Error"].is<JsonObject>());
    EXPECT_TRUE(schemas["LogEntry"].is<JsonObject>());
    EXPECT_TRUE(schemas["KeyRecord"].is<JsonObject>());
    EXPECT_TRUE(schemas["CommandRecord"].is<JsonObject>());
    // The closed error-code table (spec 12.3.1) is enumerable from the doc.
    JsonArray codes = schemas["Error"]["properties"]["error"]["properties"]["code"]["enum"]
                          .as<JsonArray>();
    EXPECT_EQ(codes.size(), 13u);
}

TEST(Openapi, LogsParamsPresent) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, mcco::kOpenApiJson, DeserializationOption::NestingLimit(32)), DeserializationError::Ok);
    JsonObject params = doc["components"]["parameters"].as<JsonObject>();
    EXPECT_TRUE(params["category"].is<JsonObject>());
    EXPECT_TRUE(params["level"].is<JsonObject>());
    EXPECT_TRUE(params["sinceSeq"].is<JsonObject>());
    EXPECT_TRUE(params["limit"].is<JsonObject>());
}

} // namespace
