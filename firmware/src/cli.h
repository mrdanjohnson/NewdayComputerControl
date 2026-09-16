#pragma once
#include <string>
#include <vector>
#include "app_context.h"

// Serial provisioning console at 115200. Physical access implies ADMIN (the
// Web UI ships in Phase 2), so this console can manage identity, API keys,
// WiFi, and NTP. Line-based; `help` lists commands. The raw key of a newly
// created API key is printed exactly once and never logged (spec 13.1.1/15.2).
class Cli {
public:
    void begin(AppContext* ctx);
    void poll(); // call frequently from the Arduino loop

private:
    void handleLine(const std::string& line);
    void printBanner();
    void cmdHelp();
    void cmdIdentity(const std::vector<std::string>& args);
    void cmdKey(const std::vector<std::string>& args);
    void cmdWifi(const std::vector<std::string>& args);
    void cmdNtp(const std::vector<std::string>& args);
    void cmdStatus();
    void cmdReboot();
    void cmdAdmin(const std::vector<std::string>& args);

    AppContext* ctx_ = nullptr;
    std::string line_buf_;
};
