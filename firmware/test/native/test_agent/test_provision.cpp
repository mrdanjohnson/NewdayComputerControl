#include <gtest/gtest.h>
#include "mc_provision.h"

TEST(Provision, PreservesSpacesQuotesAndUnicode) {
    mcco::ProvisionCredentials c;
    ASSERT_TRUE(mcco::parse_provision_credentials(R"({"ssid":"Client Wi-Fi","password":"a \"long\" password"})", true, c));
    EXPECT_EQ(c.ssid, "Client Wi-Fi"); EXPECT_EQ(c.password, "a \"long\" password");
    ASSERT_TRUE(mcco::parse_provision_credentials(R"({"password":"  client password  "})", false, c));
    EXPECT_EQ(c.password, "  client password  ");
    ASSERT_TRUE(mcco::parse_provision_credentials(R"({"ssid":"Café","password":""})", true, c));
}
TEST(Provision, RejectsInjectionMalformedAndTruncation) {
    mcco::ProvisionCredentials c;
    for (auto input : {R"({"ssid":"Client","password":"short"})", R"({"ssid":"Client","password":"long password","extra":true})", R"({"ssid":"Client\u0000other","password":"long password"})", R"({"ssid":"Client","password":"long\npassword"})", "broken"})
        EXPECT_FALSE(mcco::parse_provision_credentials(input, true, c));
    EXPECT_FALSE(mcco::parse_provision_credentials(R"({"password":"short"})", false, c));
}
