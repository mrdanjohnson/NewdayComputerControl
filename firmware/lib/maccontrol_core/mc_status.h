#pragma once
#include <cstdint>
#include <string>
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

// Mode A status document (spec 7, AT-01): every leaf is a five-tuple; mac.*
// and applications.* are all-unknown nulls; connection.agent is false /
// esp32_direct. `identity_observed_at` timestamps the esp32_direct config
// facts; freshness is recomputed on read, so this is cheap (<50 ms, 7.1.1).
void build_status_mode_a(JsonDocument& doc, const Identity& id, bool usb_up,
                         bool network_up, uint64_t network_probe_at,
                         uint64_t identity_observed_at, uint64_t now,
                         uint32_t cache_epoch);

// Mode A capabilities document (spec 12.3.1 + 17.1.1 schema). Exactly the
// eight closed command types; every entry verified:false; app_launch/app_quit
// available:false; agent paired:false, connected:false. `macro_ids` lists the
// executable macros (spec 12.3.1 commands.macro_execute.macro_ids).
void build_capabilities_mode_a(JsonDocument& doc, const Identity& id,
                               const std::vector<std::string>& macro_ids);

} // namespace mcco
