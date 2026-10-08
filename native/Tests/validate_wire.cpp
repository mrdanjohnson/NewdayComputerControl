// Compile the SHIPPING firmware parser unchanged and validate Swift-produced frames.
#include "mc_agent_events.h"
#include <iostream>
#include <string>
int main() {
    std::string line; int count=0;
    while (std::getline(std::cin,line)) {
        mcco::AgentEvent event;
        auto result=mcco::parse_agent_event(line,event);
        if (result!=mcco::AgentEventError::Ok) { std::cerr << "Rejected native event " << count << ": " << int(result) << "\n"; return 1; }
        ++count;
    }
    if (count<12) return 2;
    std::cout << "Shipping firmware admitted " << count << " native event frames.\n";
}
