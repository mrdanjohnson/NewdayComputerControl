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

// Connection liveness derived from the snapshot (spec 4.2.2): STALE after
// stale_threshold_s of silence, OFFLINE after offline_threshold_s or when no
// transport session is open.
static bool agent_connected(const AgentStatus* a, uint64_t now) {
    if (!a || !a->paired || !a->session_live || a->last_frame_at == 0) return false;
    return now - a->last_frame_at <= a->offline_threshold_s;
}

static Freshness agent_freshness(const AgentStatus* a, uint64_t at, uint64_t now) {
    if (!a || at == 0) return Freshness::Unknown;
    // Spec 7.2.2: an open declared expected-offline window freezes the
    // agent-reported groups at expected_offline — positive evidence, not the
    // ambiguous `stale` — until it expires, then normal aging resumes.
    if (a->declared_offline_until > now) return Freshness::ExpectedOffline;
    // Spec 4.2.2: agent-reported tuples go STALE after stale_threshold_s of
    // AGENT SILENCE, not after the last state change — heartbeats keep them
    // fresh while the session lives (observed_at stays the value's true
    // provenance). Last frame unknown: fall back to the observation age.
    const uint64_t anchor = a->last_frame_at != 0 ? a->last_frame_at : at;
    return (now - anchor > a->stale_threshold_s) ? Freshness::Stale : Freshness::Fresh;
}

void build_status(JsonDocument& doc, const Identity& id, bool usb_up,
                  bool network_up, uint64_t network_probe_at,
                  uint64_t identity_observed_at, const AgentStatus* agent,
                  uint64_t now, uint32_t cache_epoch) {
    doc.clear();
    doc["generated_at"] = iso8601_format(now).c_str();
    doc["cache_epoch"] = cache_epoch;

    JsonObject device = doc["device"].to<JsonObject>();
    tuple_str(device, "name", id.device_name.c_str(), Source::Esp32Direct,
              identity_observed_at, -1, Freshness::Fresh);
    tuple_str(device, "hostname", id.hostname.c_str(), Source::Esp32Direct,
              identity_observed_at, -1, Freshness::Fresh);
    tuple_str(device, "device_id", id.device_id.c_str(), Source::Esp32Direct,
              identity_observed_at, -1, Freshness::Fresh);

    JsonObject conn = doc["connection"].to<JsonObject>();
    tuple_bool(conn, "usb", usb_up, Source::Esp32Direct, now, -1, Freshness::Fresh);
    tuple_bool(conn, "network", network_up, Source::NetworkProbe, network_probe_at, 30,
               Freshness::Fresh);

    const bool connected = agent_connected(agent, now);
    // Spec 7.2.2 table: during a declared window connection.agent is SET
    // false with expected_offline freshness (the endpoint directly observed
    // the agent leave); the onset is the declaring goodbye (~60 s before the
    // stored expiry).
    const bool declared = agent && agent->declared_offline_until > now;
    if (declared) {
        tuple_bool(conn, "agent", false, Source::Esp32Direct,
                   agent->declared_offline_until - 60, -1, Freshness::ExpectedOffline);
    } else {
        // connection.agent is esp32_direct event-driven: the value flips at the
        // OFFLINE observation itself (spec 7.2.2 worked example 2), never ages.
        tuple_bool(conn, "agent", connected, Source::Esp32Direct, now, -1, Freshness::Fresh);
    }

    JsonObject mac = doc["mac"].to<JsonObject>();
    if (!agent || !agent->paired || !agent->has_system) tuple_null(mac, "state");
    else tuple_str(mac, "state", agent->system_state.c_str(), Source::AgentReported,
                   agent->system_at, int(agent->stale_threshold_s),
                   agent_freshness(agent, agent->system_at, now));
    if (!agent || !agent->paired || !agent->has_lock) tuple_null(mac, "locked");
    else tuple_bool(mac, "locked", agent->locked, Source::AgentReported, agent->lock_at,
                    int(agent->stale_threshold_s),
                    agent_freshness(agent, agent->lock_at, now));
    if (!agent || !agent->paired || !agent->has_user) {
        tuple_null(mac, "user_logged_in");
        tuple_null(mac, "user");
    } else {
        tuple_bool(mac, "user_logged_in", agent->user_logged_in, Source::AgentReported,
                   agent->user_at, int(agent->stale_threshold_s),
                   agent_freshness(agent, agent->user_at, now));
        if (agent->user.empty()) {
            JsonObject t = mac["user"].to<JsonObject>();
            t["value"] = nullptr;
            t["source"] = source_to_string(Source::AgentReported);
            t["observed_at"] = iso8601_format(agent->user_at).c_str();
            t["ttl_s"] = agent->stale_threshold_s;
            t["freshness"] = freshness_to_string(agent_freshness(agent, agent->user_at, now));
        } else {
            tuple_str(mac, "user", agent->user.c_str(), Source::AgentReported, agent->user_at,
                      int(agent->stale_threshold_s),
                      agent_freshness(agent, agent->user_at, now));
        }
    }
    if (!agent || !agent->paired || !agent->has_boot) tuple_null(mac, "boot_id");
    else tuple_str(mac, "boot_id", agent->boot_id.c_str(), Source::AgentReported,
                   agent->boot_at, -1,
                   declared ? Freshness::ExpectedOffline
                            : Freshness::Fresh); // retained through the window (7.2.2)

    JsonObject apps = doc["applications"].to<JsonObject>();
    if (agent && agent->paired) {
        for (const auto& kv : agent->apps) {
            JsonObject e = apps[kv.first.c_str()].to<JsonObject>();
            const AgentAppState& st = kv.second;
            Freshness fr = agent_freshness(agent, st.observed_at, now);
            tuple_str(e, "state", st.running ? "running" : "not_running",
                      Source::AgentReported, st.observed_at, int(agent->stale_threshold_s), fr);
            if (st.running) {
                JsonObject t = e["pid"].to<JsonObject>();
                t["value"] = st.pid;
                t["source"] = source_to_string(Source::AgentReported);
                t["observed_at"] = iso8601_format(st.observed_at).c_str();
                t["ttl_s"] = agent->stale_threshold_s;
                t["freshness"] = freshness_to_string(fr);
            } else {
                tuple_null(e, "pid");
            }
            tuple_null(e, "warning"); // reconciliation warnings are Phase 6
        }
    }
}

void build_status_mode_a(JsonDocument& doc, const Identity& id, bool usb_up,
                         bool network_up, uint64_t network_probe_at,
                         uint64_t identity_observed_at, uint64_t now,
                         uint32_t cache_epoch) {
    build_status(doc, id, usb_up, network_up, network_probe_at, identity_observed_at,
                 nullptr, now, cache_epoch);
}

void build_agent_system_info(JsonObject out, const AgentStatus& st) {
    // §6.3 system_info: absent evidence renders null, never invented. `uptime_s`
    // is the Mac uptime (heartbeat mac_uptime_s), NOT the agent heartbeat's own
    // uptime_s — see the OpenAPI amendment note (Phase 4.5 A2 relabeling).
    if (st.has_sysinfo) {
        out["cpu_utilization_pct"] = st.cpu_pct;
        out["memory_utilization_pct"] = st.mem_pct;
        out["disk_free_bytes"] = st.disk_free_bytes;
        JsonObject net = out["network"].to<JsonObject>();
        net["reachable"] = st.net_reachable;
        if (st.net_ip.empty()) net["ip"] = nullptr;
        else net["ip"] = st.net_ip.c_str();
    } else {
        out["cpu_utilization_pct"] = nullptr;
        out["memory_utilization_pct"] = nullptr;
        out["disk_free_bytes"] = nullptr;
        JsonObject net = out["network"].to<JsonObject>();
        net["reachable"] = nullptr;
        net["ip"] = nullptr;
    }
    if (st.has_mac_uptime) {
        out["uptime_s"] = st.mac_uptime_s;
        out["boot_time"] = st.boot_time;
    } else {
        out["uptime_s"] = nullptr;
        out["boot_time"] = nullptr;
    }
    if (st.has_capability && !st.os_version.empty()) {
        out["os_version"] = st.os_version.c_str();
    } else {
        out["os_version"] = nullptr;
    }
    if (st.has_capability && !st.hardware_model.empty()) {
        out["hardware_model"] = st.hardware_model.c_str();
    } else {
        out["hardware_model"] = nullptr;
    }
    if (st.has_front_app && !st.front_app.empty()) {
        out["front_app"] = st.front_app.c_str();
    } else {
        out["front_app"] = nullptr;
    }
}

static void cmd_entry(JsonObject cmds, const char* name, bool available, bool verified,
                      int deadline_s) {
    JsonObject e = cmds[name].to<JsonObject>();
    e["available"] = available;
    e["verified"] = verified;
    if (deadline_s > 0) e["deadline_s"] = deadline_s;
}

void build_capabilities(JsonDocument& doc, const Identity& id,
                        const std::vector<std::string>& macro_ids,
                        const AgentStatus* agent, uint64_t now) {
    doc.clear();
    const bool paired = agent && agent->paired;
    // connected/verified collapse within the 30 s offline threshold
    // (spec 17.1.1), so this builder is live, not cached across transitions.
    const bool connected = agent_connected(agent, now);
    doc["api_version"] = "v1";
    doc["mode"] = paired ? "B" : "A";
    // PRD §1.3.3 capability levels: L1 = Mode A HID control; L2 = Mode B
    // agent visibility (paired); L3 = Mode B verified automation, claimed
    // only while the evidence channel is live (paired && connected, which
    // also satisfies the §17.1.1 verified:true => paired+connected rule).
    doc["capability_level"] = !paired ? "L1" : (connected ? "L3" : "L2");
    JsonObject device = doc["device"].to<JsonObject>();
    device["name"] = id.device_name.c_str();
    device["hostname"] = id.hostname.c_str();

    JsonObject cmds = doc["commands"].to<JsonObject>();
    // Phase 5 (spec 5.3.1/§8): power/lock/macro commands are verifiable in
    // Mode B exactly while the agent channel is live; they remain dispatchable
    // (available) regardless.
    const bool power_verified = paired && connected;
    cmd_entry(cmds, "wake", true, power_verified, 120);
    cmd_entry(cmds, "sleep", true, power_verified, 90);
    cmd_entry(cmds, "restart", true, power_verified, 180);
    cmd_entry(cmds, "shutdown", true, power_verified, 120);
    cmd_entry(cmds, "lock", true, power_verified, 15);
    JsonObject macro = cmds["macro_execute"].to<JsonObject>();
    macro["available"] = true;
    macro["verified"] = power_verified;
    JsonArray ids = macro["macro_ids"].to<JsonArray>();
    for (const auto& mid : macro_ids) ids.add(mid);
    // app_launch/app_quit: dispatchable (and verifiable) only with a live
    // agent session and a capability_report on file (spec 12.3.1).
    const bool apps_ok = paired && connected && agent->has_capability;
    for (const char* app : {"app_launch", "app_quit"}) {
        JsonObject e = cmds[app].to<JsonObject>();
        e["available"] = apps_ok;
        e["verified"] = apps_ok;
        JsonArray a = e["apps"].to<JsonArray>();
        if (apps_ok) {
            // The monitored-app registry (display names) is Phase 6; until
            // then the allowlisted bundle IDs are listed with the bundle ID
            // as display name (amendment note in /openapi.json).
            for (const auto& ba : agent->allowlisted_apps) {
                JsonObject entry = a.add<JsonObject>();
                entry["bundle_id"] = ba.first.c_str();
                entry["display_name"] = ba.first.c_str();
            }
        }
    }

    JsonObject ag = doc["agent"].to<JsonObject>();
    ag["paired"] = paired;
    ag["connected"] = connected;
    JsonArray en = ag["enabled_commands"].to<JsonArray>();
    JsonArray al = ag["allowlisted_apps"].to<JsonArray>();
    if (paired && agent->has_capability) {
        for (const auto& c : agent->enabled_commands) en.add(c);
        for (const auto& ba : agent->allowlisted_apps) al.add(ba.first.c_str());
    }
}

void build_capabilities_mode_a(JsonDocument& doc, const Identity& id,
                               const std::vector<std::string>& macro_ids) {
    build_capabilities(doc, id, macro_ids, nullptr, 0);
}

} // namespace mcco
