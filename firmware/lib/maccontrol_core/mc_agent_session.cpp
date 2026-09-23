#include "mc_agent_session.h"
#include "mc_ids.h"
#include <ArduinoJson.h>

namespace mcco {

AgentSession::AgentSession(IRandom& rng, IClock& clock, uint32_t heartbeat_interval_s,
                           uint32_t max_seq_gap, uint32_t max_schema_violations)
    : rng_(rng), clock_(clock), heartbeat_interval_s_(heartbeat_interval_s),
      max_seq_gap_(max_seq_gap), max_schema_violations_(max_schema_violations) {}

uint16_t AgentSession::acceptHello(const AgentEvent& ev) {
    if (hello_received_) return uint16_t(AgentClose::PolicyViolation);
    if (ev.type != AgentEventType::Hello) return uint16_t(AgentClose::PolicyViolation);
    if (ev.seq != 1) return uint16_t(AgentClose::SequenceViolation);
    if (!ev.session_id.empty()) return uint16_t(AgentClose::PolicyViolation);

    JsonDocument p;
    if (deserializeJson(p, ev.payload_json)) return uint16_t(AgentClose::PolicyViolation);
    // protocol_version negotiation: only version 1 exists (spec 4.2.1 table:
    // unsupported version closes 4003, no reconnect until agent updated).
    int proto = p["protocol_version"] | 0;
    if (proto != 1) return uint16_t(AgentClose::ProtocolMismatch);

    session_id_ = make_session_id(rng_);
    hello_received_ = true;
    last_seq_ = ev.seq;
    last_frame_ms_ = clock_.millis();
    return 0;
}

uint16_t AgentSession::onFrame(const AgentEvent& ev, AgentEventError perr) {
    if (!hello_received_) return uint16_t(AgentClose::PolicyViolation);

    // Sequence accounting applies to every frame, including rejected ones —
    // a discarded frame still consumed its seq value (spec 6.1.1), so the
    // next valid frame must be exactly last+1 to avoid a phantom gap.
    if (ev.seq <= last_seq_) return uint16_t(AgentClose::SequenceViolation); // regression
    if (ev.seq - last_seq_ > max_seq_gap_) return uint16_t(AgentClose::SequenceViolation);
    last_seq_ = ev.seq;
    last_frame_ms_ = clock_.millis(); // any valid frame resets liveness

    if (perr != AgentEventError::Ok) {
        if (++consec_violations_ >= max_schema_violations_) {
            return uint16_t(AgentClose::ProtocolMismatch); // 3 strikes (default)
        }
        return 0; // rejected evidence: no command/status advance, session lives
    }
    consec_violations_ = 0;
    return 0;
}

AgentConnState AgentSession::liveness() const {
    if (!hello_received_) return AgentConnState::AwaitingHello;
    const uint64_t age_s = lastFrameAgeMs() / 1000;
    if (age_s > offlineThresholdS()) return AgentConnState::Offline;
    if (age_s > staleThresholdS()) return AgentConnState::Stale;
    return AgentConnState::Active;
}

} // namespace mcco
