#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <ArduinoJson.h>
#include "mc_types.h"

namespace mcco {

// Persistent device identity record (spec 3.1.1).
struct Identity {
    std::string device_name;
    std::string hostname;   // bare DNS label, 1-57 chars, [a-z0-9-]
    std::string location;
    std::string description;
    std::string device_id;  // 12 hex chars, immutable
};

// Per-application agent evidence (spec 6.3/11.1).
struct AgentAppState {
    bool running = false;
    int64_t pid = 0;
    uint64_t observed_at = 0;  // epoch seconds (ESP32 receipt time)
};

// Mode B evidence snapshot (spec ch. 6/7), owned by the glue's status cache
// and updated only from admitted MCA events. `nullptr` at build time produces
// the Mode A document. Freshness is recomputed on every build (spec 7.1.1).
struct AgentStatus {
    bool paired = false;
    bool session_live = false;    // a transport session is currently open
    uint64_t last_frame_at = 0;   // last admitted frame, epoch seconds
    bool has_system = false;
    std::string system_state;
    uint64_t system_at = 0;
    bool has_lock = false;
    bool locked = false;
    uint64_t lock_at = 0;
    bool has_user = false;
    bool user_logged_in = false;
    std::string user;
    uint64_t user_at = 0;
    bool has_boot = false;
    std::string boot_id;
    uint64_t boot_at = 0;
    std::map<std::string, AgentAppState> apps;
    bool has_capability = false;
    uint64_t capability_at = 0;
    std::vector<std::string> enabled_commands;
    std::vector<std::pair<std::string, bool>> allowlisted_apps;  // bundle_id, running
    // Phase 4.5 composite telemetry (spec 6.3), all from the heartbeat frame.
    // Null samples keep the previous value but still refresh sysinfo_at.
    bool has_sysinfo = false;
    uint64_t sysinfo_at = 0;
    double cpu_pct = 0;
    double mem_pct = 0;
    uint64_t disk_free_bytes = 0;
    bool net_reachable = false;
    std::string net_ip;              // empty = unknown -> rendered null
    bool has_mac_uptime = false;     // boot_time + Mac uptime (heartbeat mac_uptime_s)
    uint64_t mac_uptime_s = 0;
    uint64_t boot_time = 0;          // epoch seconds of the Mac's kern.boottime
    std::string os_version;          // from capability_report; null when !has_capability
    std::string hardware_model;
    bool has_front_app = false;      // front_app_changed (spec amendment, scope B1)
    std::string front_app;           // empty = none -> rendered null
    uint32_t stale_threshold_s = 15;    // spec 4.2.2 defaults (3x/6x of 5 s)
    uint32_t offline_threshold_s = 30;
    // Phase 5 (spec 5.3.2/7.2.2): an agent_goodbye(reason=sleep|restart|
    // shutdown) opens a declared expected-offline window; epoch seconds,
    // 0 = no declaration. Overrides stale aging while in the future.
    uint64_t declared_offline_until = 0;
    // Last-known agent peer IP (the transport source of the current/recent
    // session), "" = unknown -> the §8.2.2 shutdown probes cannot run.
    std::string peer_ip;
};

// Hostname rule from spec 3.1.1: 1-57 chars, lowercase [a-z0-9-], start/end
// alphanumeric. Returns true if valid.
bool hostname_valid(const std::string& h);

// Serialize one provenance/freshness five-tuple (spec 7.1.1) into `parent`
// under `key`. String values are copied; bool/int overloads for typed values.
void tuple_str(JsonObject parent, const char* key, const char* value, Source src,
               uint64_t observed_at, int ttl_s, Freshness freshness);
void tuple_bool(JsonObject parent, const char* key, bool value, Source src,
                uint64_t observed_at, int ttl_s, Freshness freshness);
void tuple_null(JsonObject parent, const char* key);

// Status document (spec 7). Every leaf is a five-tuple; freshness is
// recomputed on read (spec 7.1.1, <50 ms). `agent == nullptr` (or
// `agent->paired == false`) produces the Mode A document: mac.* and
// applications.* all-unknown, connection.agent false / esp32_direct.
// `identity_observed_at` timestamps the esp32_direct config facts.
void build_status(JsonDocument& doc, const Identity& id, bool usb_up,
                  bool network_up, uint64_t network_probe_at,
                  uint64_t identity_observed_at, const AgentStatus* agent,
                  uint64_t now, uint32_t cache_epoch);

// Capabilities document (spec 12.3.1 + 17.1.1 schema), mode-aware: mode B
// when `agent->paired`, with the live agent block (paired/connected/
// enabled_commands/allowlisted_apps) and app_launch/app_quit availability from
// session liveness + the latest capability_report. `nullptr` => Mode A.
// `macro_ids` lists the executable macros (spec 12.3.1). `now` bounds the
// connected/verified flags to the 30 s offline threshold (spec 17.1.1).
void build_capabilities(JsonDocument& doc, const Identity& id,
                        const std::vector<std::string>& macro_ids,
                        const AgentStatus* agent, uint64_t now);

// Backwards-compatible Mode A entry points (thin wrappers over the builders
// above with `agent == nullptr`).
void build_status_mode_a(JsonDocument& doc, const Identity& id, bool usb_up,
                         bool network_up, uint64_t network_probe_at,
                         uint64_t identity_observed_at, uint64_t now,
                         uint32_t cache_epoch);

// /api/v1/agent/status `system_info` section (spec 6.3, Phase 4.5). Fills
// `out` with cpu/memory/disk/network samples, uptime_s (the MAC uptime, not
// the agent heartbeat's uptime_s — see the OpenAPI amendment note), boot_time,
// os_version, hardware_model, and front_app. Every leaf is null when its
// evidence is absent (§9 honesty); net_ip/front_app empty also render null.
void build_agent_system_info(JsonObject out, const AgentStatus& st);
void build_capabilities_mode_a(JsonDocument& doc, const Identity& id,
                               const std::vector<std::string>& macro_ids);

} // namespace mcco
