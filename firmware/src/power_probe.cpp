#include "power_probe.h"
#include <string.h>
#include "agent_link.h"
#include "app_context.h"
#include "esp_clock.h"
#include "mc_engine.h"
#include "mc_mutex.h"
#include "mc_status.h"
#include "status_cache.h"
#include "lwip/def.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"

namespace {

// Probe cadence per spec 8.2.2: 3 echoes at 5 s intervals; a round waits up
// to 2 s for its reply before reporting unreachable.
constexpr uint32_t kRoundIntervalMs = 5000;
constexpr uint32_t kReplyTimeoutMs = 2000;
constexpr uint8_t kMaxRounds = 3;
constexpr uint16_t kEchoId = 0x4D43; // "MC": marks our echoes; foreign pings ignored

enum class ProbePhase : uint8_t { Idle, AwaitReply, BetweenRounds };

ProbePhase g_phase = ProbePhase::Idle;
struct raw_pcb* g_pcb = nullptr;
ip4_addr_t g_target;
uint32_t g_deadline_ms = 0; // monotonic deadline of the current wait
uint16_t g_seq = 0;
uint8_t g_round = 0;
volatile bool g_reply = false; // written on the tcpip thread, read on the dispatcher

uint8_t icmp_recv(void*, struct raw_pcb*, struct pbuf* p, const ip_addr_t*) {
    if (p && p->len >= sizeof(struct icmp_echo_hdr)) {
        const struct icmp_echo_hdr* h = (const struct icmp_echo_hdr*)p->payload;
        if (h->type == (uint8_t)ICMP_ER && h->id == lwip_htons(kEchoId) &&
            h->seqno == lwip_htons(g_seq)) {
            g_reply = true;
        }
    }
    if (p) pbuf_free(p);
    return 1; // consumed either way — our echo replies are not for lwIP
}

bool ensure_pcb() {
    if (g_pcb) return true;
    g_pcb = raw_new(IP_PROTO_ICMP);
    if (!g_pcb) return false;
    raw_recv(g_pcb, icmp_recv, nullptr);
    return true;
}

void drop_pcb() {
    if (g_pcb) {
        raw_remove(g_pcb);
        g_pcb = nullptr;
    }
}

bool send_echo() {
    struct pbuf* p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 16, PBUF_RAM);
    if (!p) return false;
    struct icmp_echo_hdr* h = (struct icmp_echo_hdr*)p->payload;
    memset(h, 0, sizeof(*h) + 16);
    h->type = (uint8_t)ICMP_ECHO;
    h->id = lwip_htons(kEchoId);
    h->seqno = lwip_htons(g_seq);
    h->chksum = inet_chksum(p->payload, p->len);
    ip_addr_t dst;
    IP_ADDR4(&dst, ip4_addr1(&g_target), ip4_addr2(&g_target), ip4_addr3(&g_target),
             ip4_addr4(&g_target));
    const err_t err = raw_sendto(g_pcb, p, &dst);
    pbuf_free(p);
    return err == ERR_OK;
}

void to_idle() {
    g_phase = ProbePhase::Idle;
    g_round = 0;
    drop_pcb();
}

// Caller does NOT hold engine_mutex; taken here (lock order per agent_link.h:
// engine_mutex only ever nests outward to the status cache, which this path
// never reaches while holding it).
bool probe_open(AppContext* ctx) {
    Guard g(ctx->engine_mutex);
    return ctx->engine->shutdown_probe_open(!ctx->agent_link->sessionActive());
}

} // namespace

void power_probe_tick(AppContext* ctx) {
    if (!ctx->agent_link || !ctx->engine || !ctx->status_cache) return;
    const uint32_t now_ms = ctx->clock->millis();

    switch (g_phase) {
    case ProbePhase::AwaitReply: {
        // Round verdict when the reply arrived early or the timeout lapsed.
        if (!g_reply && (int32_t)(now_ms - g_deadline_ms) < 0) return;
        const bool reachable = g_reply;
        {
            Guard g(ctx->engine_mutex);
            ctx->engine->on_shutdown_probes(reachable);
        }
        if (reachable || g_round >= kMaxRounds || !probe_open(ctx)) {
            // Reply refutes the shutdown (host_still_reachable), the third
            // failure completed it, or the record left confirming — done.
            to_idle();
            return;
        }
        g_phase = ProbePhase::BetweenRounds;
        g_deadline_ms = now_ms + kRoundIntervalMs;
        return;
    }
    case ProbePhase::BetweenRounds:
        if ((int32_t)(now_ms - g_deadline_ms) < 0) return;
        if (!probe_open(ctx)) { // reconnect/deadline sweep ended the record
            to_idle();
            return;
        }
        break;
    case ProbePhase::Idle:
        if (!probe_open(ctx)) return;
        break;
    }

    // Start (Idle) or continue (BetweenRounds) the probe phase.
    if (g_phase == ProbePhase::Idle) {
        const mcco::AgentStatus st = ctx->status_cache->snapshotAgent();
        if (st.peer_ip.empty() || !ip4addr_aton(st.peer_ip.c_str(), &g_target)) {
            // No last-known peer IP: probes cannot corroborate; the record
            // ends via the deadline sweep (spec 8.2.2 outer bound).
            return;
        }
        g_round = 0;
    }
    if (!ensure_pcb()) return;
    ++g_seq; // new seq per round: a late reply for the old seq no longer matches
    ++g_round;
    g_reply = false;
    if (!send_echo()) {
        to_idle();
        return;
    }
    g_deadline_ms = now_ms + kReplyTimeoutMs;
    g_phase = ProbePhase::AwaitReply;
}
