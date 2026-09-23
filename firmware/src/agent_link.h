#pragma once
#include <atomic>
#include <deque>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <string>
#include <WiFiClient.h>
#include "app_context.h"
#include "mc_agent_events.h"
#include "mc_agent_session.h"
#include "mc_error.h"
#include "mc_status.h"
#include "mc_mutex.h"

// MCA evidence-channel owner (spec 4.2/4.3). Runs the WebSocket session task
// (`mc_agent_ws`) and the polling ingress used by the HTTP task, both feeding
// exactly one live mcco::AgentSession at a time. A new hello on either
// transport supersedes the incumbent session (new session_id wins). A 1 s
// software timer owns ALL liveness transitions (Active->Stale->Offline and
// session teardown side effects); the WS task only moves frames and socket
// state. Status evidence is mirrored into StatusCache for the builders and
// the engine's pre-ledger agent gate.
//
// Lock order (never reversed): state_mutex_ -> engine_mutex -> cache mutex.
// The engine agent gate runs under engine_mutex and therefore may only touch
// the lock-free sessionActive() / StatusCache snapshot, never state_mutex_.
//
// Session lifetime discipline: `session_` is dereferenced ONLY under
// state_mutex_ (onFrame/liveness/field reads). It is deleted only under
// state_mutex_ too — by beginSessionLocked (supersede), teardownSession, or
// requestClose — so a stale raw pointer passed after a failed identity check
// is never dereferenced and never double-freed.
class AgentLink {
public:
    bool begin(AppContext* ctx); // creates the WS task + liveness timer
    TaskHandle_t taskHandle() const { return task_; }

    // HTTP task: hand over an upgraded /agent/v1/ws client. Non-blocking;
    // false means the handoff queue is full (caller closes the connection).
    bool offerWsClient(WiFiClient client, const std::string& bearer);
    // True when the handoff queue has a free slot. Only the HTTP task offers,
    // so checking then offering is race-free. Callers write the 101 response
    // between canAcceptWs() and offerWsClient() so the agent task never sees
    // the handshake bytes as a frame.
    bool canAcceptWs() const;
    // HTTP task: POST /agent/v1/events (spec 4.3.1). Caller does not hold
    // engine_mutex; this takes it internally where needed.
    bool pollEvent(const std::string& auth_bearer, const std::string& session_hdr,
                   const std::string& body, std::string& response_json, mcco::ErrCode& err);

    // HTTP task: GET /agent/v1/commands/pending.
    bool pollPending(const std::string& auth_bearer, const std::string& session_hdr,
                     std::string& response_json, mcco::ErrCode& err);

    // Dispatcher task: queue one Mode B dispatch for the agent. False = full.
    bool enqueueDispatch(const std::string& action, const std::string& bundle_id,
                         const std::string& command_id);

    // Engine agent gate support: session liveness == Active (lock-free).
    bool sessionActive() const { return active_atomic_.load(); }

    // Set when a session reached OFFLINE: the dispatcher's 1 s sweep drains
    // engine.on_agent_offline() (never the timer task — see tick()).
    bool drainOfflinePending() { return offline_pending_.exchange(false); }

    // Active session id for operator inspection ("" when none).
    std::string sessionId() const {
        Guard g(const_cast<Mutex&>(state_mutex_));
        return session_ && session_->helloReceived() ? session_->sessionId() : "";
    }

    // ADMIN revocation (spec 3.2.2): terminate the live session immediately
    // with the given WS close code. Safe from any task context.
    void requestClose(uint16_t close_code);

private:
    struct WsOffer {
        WiFiClient client;
        std::string bearer;
    };
    static constexpr size_t kHandoffCap = 2;
    struct PendingDispatch {
        std::string action;
        std::string bundle_id;
        std::string command_id;
    };

    static void taskEntry(void* arg);
    static void timerEntry(TimerHandle_t h);
    void tick(); // 1 s liveness (all transports)

    void runWsSession(WsOffer offer, uint8_t* buf);
    void teardownSession(mcco::AgentSession* mine, const char* event, uint16_t close_code,
                         bool offline_effect);
    void applyEvidence(const mcco::AgentEvent& ev);
    void applyHello(const mcco::AgentEvent& ev);

    // Admission shared by both transports (spec 2.2.2); caller holds engine_mutex.
    bool admitLocked(const std::string& bearer, const std::string& instance_id) const;

    // State-mutex helpers. beginSessionLocked supersedes any incumbent.
    mcco::AgentSession* session_locked() const { return session_; }
    mcco::AgentSession* beginSessionLocked(uint8_t transport); // returns new session
    bool sessionIsCurrentLocked(const mcco::AgentSession* mine) const {
        return session_ == mine;
    }

    AppContext* ctx_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TimerHandle_t timer_ = nullptr;

    // Upgraded-socket handoff from the HTTP task. A plain FreeRTOS queue
    // cannot hold WiFiClient (xQueueSend memcpys the object, bypassing the
    // shared_ptr copy ctor and corrupting the handle accounting) — so the
    // handoff is a mutex-protected deque plus a task notification.
    mutable Mutex handoff_mutex_;
    std::deque<WsOffer> handoff_q_;

    Mutex state_mutex_;
    mcco::AgentSession* session_ = nullptr; // owned; exactly one live session
    uint8_t transport_ = 0;                 // 0 none, 1 ws, 2 polling
    bool socket_dead_ = false; // no reader left: the timer reclaims at OFFLINE
    mcco::AgentConnState last_conn_state_ = mcco::AgentConnState::AwaitingHello;
    uint16_t pending_close_ = 0; // WS close to deliver to the live session

    std::atomic<bool> active_atomic_{false};
    std::atomic<bool> offline_pending_{false};

    Mutex pending_mutex_;
    std::deque<PendingDispatch> pending_; // cap 4
    static constexpr size_t kPendingCap = 4;
};
