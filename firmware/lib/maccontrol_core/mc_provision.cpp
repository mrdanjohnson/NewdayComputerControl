#include "mc_provision.h"
#include <ArduinoJson.h>
#include <cstring>

namespace mcco {
static bool text_valid(const char* value) {
    if (!value) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
bool parse_provision_credentials(const std::string& json, bool wifi,
                                 ProvisionCredentials& out) {
    if (json.size() > 490) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json) || !doc.is<JsonObjectConst>()) return false;
    JsonObjectConst o = doc.as<JsonObjectConst>();
    if (o.size() != (wifi ? 2u : 1u)) return false;
    for (JsonPairConst field : o)
        if (strcmp(field.key().c_str(), "password") != 0 &&
            (!wifi || strcmp(field.key().c_str(), "ssid") != 0)) return false;
    if (!o["password"].is<const char*>()) return false;
    const char* password = o["password"].as<const char*>();
    if (!text_valid(password)) return false;
    // Compare decoded JSON length too: embedded NUL must never be truncated.
    if (o["password"].as<JsonString>().size() != strlen(password)) return false;
    const size_t length = strlen(password);
    if (wifi) {
        if (!o["ssid"].is<const char*>()) return false;
        const char* ssid = o["ssid"].as<const char*>();
        if (!text_valid(ssid) || strlen(ssid) == 0 || strlen(ssid) > 32 ||
            o["ssid"].as<JsonString>().size() != strlen(ssid)) return false;
        if (length != 0 && (length < 8 || length > 63)) return false;
        out.ssid = ssid;
    } else if (length < 10 || length > 128) return false;
    out.password = password;
    return true;
}
}
