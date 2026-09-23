#pragma once
#include <cstdint>
#include <string>
#include "mc_agent_events.h"
#include "mc_clock.h"
#include "mc_random.h"

namespace mcco {

// ESP32-side observation of one MCA session (spec 4.2.1/4.2.2). These states
// drive status-cache freshness; they never terminate commands by themselves.
enum class AgentConnState : uint8_t { AwaitingHello, Active, Stale, Offline };

// WebSocket close codes (spec 4.2.1). On the polling transport the glue maps
// rejections to the deterministic HTTP rules of spec 4.3.1 instead.
enum class AgentClose : uint16_t {
    Normal = 1000,          // clean shutdown after agent_goodbye
    GoingAway = 1001,       // expected-offline window
    PolicyViolation = 1008, // generic auth/policy failure (terminal)
    Unpaired = 4001,        // credential unknown/revoked or instance mismatch
    SequenceViolation = 4002,
    ProtocolMismatch = 4003
};

// Server-side session logic shared by the WebSocket and polling transports.
// Transport-agnostic: the glue feeds parsed frames in and receives either
// "accept" (0) or a close code to send.
class AgentSession {
public:
    AgentSession(IRandom& rng, IClock& clock, uint32_t heartbeat_interval_s = 5,
                 uint32_t max_seq_gap = 10, uint32_t max_schema_violations = 3);

    // First frame of the session; must be the agent_hello (spec 4.2.1). The
    // glue validates the pairing credential and agent_instance_id BEFORE
    // calling (closing 4001 itself on failure). Returns 0 on success, else a
    // close code: 1008 malformed hello, 4002 seq != 1, 4003 unsupported
    // protocol_version.
    uint16_t acceptHello(const AgentEvent& ev);

    // Every subsequent frame, valid or not. Schema-violating frames still
    // consume their seq (spec 6.1.1), so seq accounting runs before the
    // rejection check. Returns 0 = accepted, else close (4002/4003).
    uint16_t onFrame(const AgentEvent& ev, AgentEventError perr);

    bool helloReceived() const { return hello_received_; }
    const std::string& sessionId() const { return session_id_; }
    // Age of the last admitted frame, in milliseconds (monotonic clock).
    uint64_t lastFrameAgeMs() const { return clock_.millis() - last_frame_ms_; }
    uint32_t heartbeatIntervalS() const { return heartbeat_interval_s_; }
    uint32_t staleThresholdS() const { return heartbeat_interval_s_ * 3; }
    uint32_t offlineThresholdS() const { return heartbeat_interval_s_ * 6; }
    uint32_t consecutiveViolations() const { return consec_violations_; }

    // Liveness (spec 4.2.1/4.2.2) on the monotonic clock: epoch time can
    // jump when SNTP first syncs after boot, which must not age a healthy
    // session into STALE/OFFLINE.
    AgentConnState liveness() const;

private:
    IRandom& rng_;
    IClock& clock_;
    uint32_t heartbeat_interval_s_;
    uint32_t max_seq_gap_;
    uint32_t max_schema_violations_;
    bool hello_received_ = false;
    std::string session_id_;
    uint64_t last_seq_ = 0;
    uint64_t last_frame_ms_ = 0;
    uint32_t consec_violations_ = 0;
};

} // namespace mcco
