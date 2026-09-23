#include "mdns_service.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include "mc_log.h"

bool MdnsService::startWithLabel(const std::string& label) {
    if (!MDNS.begin(label.c_str())) return false;
    MDNS.addService("maccontrol", "_tcp", 80);
    MDNS.addServiceTxt("maccontrol", "_tcp", "id", device_id_.c_str());
    MDNS.addServiceTxt("maccontrol", "_tcp", "api", "1");
    MDNS.addServiceTxt("maccontrol", "_tcp", "mode", txt_mode_.c_str());
    MDNS.addServiceTxt("maccontrol", "_tcp", "pair", txt_pair_.c_str());
    label_ = label;
    return true;
}

bool MdnsService::begin(const mcco::Identity& id) {
    device_id_ = id.device_id;
    if (startWithLabel(id.hostname)) return true;
    // Collision handling: log and retry with -2..-99 (spec 3.3.1). The
    // configured hostname is retained in the identity record.
    for (int suffix = 2; suffix <= 99; suffix++) {
        std::string candidate = id.hostname + "-" + std::to_string(suffix);
        if (startWithLabel(candidate)) return true;
    }
    return false; // hostname_collision_exhausted
}

void MdnsService::reannounce(const mcco::Identity& id) {
    MDNS.end();
    begin(id); // completes in << 2 s
}

void MdnsService::setAgentTxt(const char* mode, const char* pair) {
    txt_mode_ = mode;
    txt_pair_ = pair;
    // This ESPmDNS core has no removeServiceTxt, so the TXT update rides the
    // same end()+begin() reannounce used for hostname changes (spec 3.3.1).
    MDNS.end();
    startWithLabel(label_.empty() ? "maccontrol" : label_);
}
