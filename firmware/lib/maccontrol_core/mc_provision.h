#pragma once
#include <string>

namespace mcco {
struct ProvisionCredentials { std::string ssid, password; };
// Physical-console-only JSON commands. Existing token-based commands remain.
bool parse_provision_credentials(const std::string& json, bool wifi,
                                 ProvisionCredentials& out);
}
