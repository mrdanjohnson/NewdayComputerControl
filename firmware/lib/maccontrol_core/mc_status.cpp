#include "mc_status.h"
#include "mc_iso8601.h"

namespace mcco {

bool hostname_valid(const std::string& h) {
    if (h.empty() || h.size() > 57) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(h.front()) || !alnum(h.back())) return false;
    for (char c : h) {
        if (!alnum(c) && c != '-') return false;
    }
    return true;
}

static void tuple_base(JsonObject parent, const char* key, Source src, uint64_t observed_at,
                       int ttl_s, Freshness freshness) {
    JsonObject t = parent[key].to<JsonObject>();
    t["source"] = source_to_string(src);
    if (observed_at == 0) t["observed_at"] = nullptr;
    else t["observed_at"] = iso8601_format(observed_at).c_str();
    if (ttl_s < 0) t["ttl_s"] = nullptr;
    else t["ttl_s"] = ttl_s;
    t["freshness"] = freshness_to_string(freshness);
}

void tuple_str(JsonObject parent, const char* key, const char* value, Source src,
               uint64_t observed_at, int ttl_s, Freshness freshness) {
    tuple_base(parent, key, src, observed_at, ttl_s, freshness);
    parent[key]["value"] = value;
}

void tuple_bool(JsonObject parent, const char* key, bool value, Source src,
                uint64_t observed_at, int ttl_s, Freshness freshness) {
    tuple_base(parent, key, src, observed_at, ttl_s, freshness);
    parent[key]["value"] = value;
}

void tuple_null(JsonObject parent, const char* key) {
    JsonObject t = parent[key].to<JsonObject>();
    t["value"] = nullptr;
    t["source"] = "unknown";
    t["observed_at"] = nullptr;
    t["ttl_s"] = nullptr;
    t["freshness"] = "unknown";
}

void build_status_mode_a(JsonDocument& doc, const Identity& id, bool usb_up,
                         bool network_up, uint64_t network_probe_at,
                         uint64_t identity_observed_at, uint64_t now,
                         uint32_t cache_epoch) {
    doc.clear();
    doc["generated_at"] = iso8601_format(now).c_str();
    doc["cache_epoch"] = cache_epoch;

    JsonObject device = doc["device"].to<JsonObject>();
    tuple_str(device, "name", id.device_name.c_str(), Source::Esp32Direct,
              identity_observed_at, -1, Freshness::Fresh);
    tuple_str(device, "hostname", id.hostname.c_str(), Source::Esp32Direct,
              identity_observed_at, -1, Freshness::Fresh);

    JsonObject conn = doc["connection"].to<JsonObject>();
    tuple_bool(conn, "usb", usb_up, Source::Esp32Direct, now, -1, Freshness::Fresh);
    tuple_bool(conn, "network", network_up, Source::NetworkProbe, network_probe_at, 30,
               Freshness::Fresh);
    tuple_bool(conn, "agent", false, Source::Esp32Direct, now, -1, Freshness::Fresh);

    JsonObject mac = doc["mac"].to<JsonObject>();
    tuple_null(mac, "state");
    tuple_null(mac, "locked");
    tuple_null(mac, "user_logged_in");
    tuple_null(mac, "user");
    tuple_null(mac, "boot_id");

    doc["applications"].to<JsonObject>(); // empty: no monitored apps in Phase 1
}

static void cmd_entry(JsonObject cmds, const char* name, bool available, int deadline_s) {
    JsonObject e = cmds[name].to<JsonObject>();
    e["available"] = available;
    e["verified"] = false; // Mode A: nothing is verifiable without the MCA
    if (deadline_s > 0) e["deadline_s"] = deadline_s;
}

void build_capabilities_mode_a(JsonDocument& doc, const Identity& id) {
    doc.clear();
    doc["api_version"] = "v1";
    doc["mode"] = "A";
    doc["capability_level"] = "L1";
    JsonObject device = doc["device"].to<JsonObject>();
    device["name"] = id.device_name.c_str();
    device["hostname"] = id.hostname.c_str();

    JsonObject cmds = doc["commands"].to<JsonObject>();
    cmd_entry(cmds, "wake", true, 120);
    cmd_entry(cmds, "sleep", true, 90);
    cmd_entry(cmds, "restart", true, 180);
    cmd_entry(cmds, "shutdown", true, 120);
    cmd_entry(cmds, "lock", true, 15);
    JsonObject macro = cmds["macro_execute"].to<JsonObject>();
    macro["available"] = false; // macro store ships in Phase 2
    macro["verified"] = false;
    macro["macro_ids"].to<JsonArray>();
    for (const char* app : {"app_launch", "app_quit"}) {
        JsonObject e = cmds[app].to<JsonObject>();
        e["available"] = false; // agent-dependent: 409 agent_not_paired in Mode A
        e["verified"] = false;
        e["apps"].to<JsonArray>();
    }

    JsonObject agent = doc["agent"].to<JsonObject>();
    agent["paired"] = false;
    agent["connected"] = false;
    agent["enabled_commands"].to<JsonArray>();
    agent["allowlisted_apps"].to<JsonArray>();
}

} // namespace mcco
