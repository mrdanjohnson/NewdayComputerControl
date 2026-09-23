#pragma once
#include <string>
#include "mc_status.h"

// ESPmDNS advertisement (spec 3.3.1): service _maccontrol._tcp on port 80
// with TXT id=<device_id>, api=1, mode=A, pair=unpaired. Hostname conflicts
// are handled best-effort: on MDNS.begin failure the responder retries with
// suffixes -2..-99 (each conflict logged); the configured hostname in the
// identity record is never changed. Identity hostname changes re-announce via
// end()+begin(), completing in well under the 2 s budget.
class MdnsService {
public:
    bool begin(const mcco::Identity& id);
    void reannounce(const mcco::Identity& id);
    const std::string& advertisedLabel() const { return label_; }

    // Phase 4 (spec 3.3.1): reflect the pairing/mode state in the TXT record.
    void setAgentTxt(const char* mode, const char* pair);

private:
    bool startWithLabel(const std::string& label);
    std::string label_;
    std::string device_id_;
    std::string txt_mode_ = "A";
    std::string txt_pair_ = "unpaired";
};
