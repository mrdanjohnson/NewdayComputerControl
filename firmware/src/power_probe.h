#pragma once

class AppContext;

// Shutdown corroboration probe (spec 8.2.2): a non-blocking ICMP echo state
// machine advanced from the dispatcher's 1 s tick — no task of its own, no
// heap, all state static (the tick already feeds the TWDT). When the engine
// reports the probe phase open (a confirming, windowed shutdown record past
// its close boundary with the evidence channel offline), sends up to 3 ICMP
// echoes at 5 s intervals to the agent's last-known peer IP
// (mcco::AgentStatus::peer_ip); each round's verdict feeds
// engine->on_shutdown_probes(). Raw-pcb lwIP echo with seq matching and a
// per-round timeout. DEVIATION RISK: if this lwIP build ever stops delivering
// echo replies to raw pcbs, the fallback is a non-blocking TCP-connect probe
// to the peer IP on a closed port (reachable = established or RST); engine
// semantics are unchanged either way — this only feeds on_shutdown_probes().
void power_probe_tick(AppContext* ctx);
