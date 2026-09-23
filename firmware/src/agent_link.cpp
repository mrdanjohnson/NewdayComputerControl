#include "agent_link.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <string.h>
#include "esp_clock.h"
#include "esp_rng.h"
#include "log_sink.h"
#include "mc_engine.h"
#include "mc_log.h"
#include "mc_pairing.h"
#include "status_cache.h"
#include "ws_server.h"

namespace {
constexpr uint32_t kWsReadTimeoutS = 1;
constexpr uint32_t kHelloTimeoutS = 5;
constexpr size_t kFrameCap = 4096;

const char* parse_error_string(mcco::AgentEventError e) {
    switch (e) {
        case mcco::AgentEventError::Ok: return "ok";
        case mcco::AgentEventError::TooLarge: return "too_large";
        case mcco::AgentEventError::MalformedJson: return "malformed_json";
        case mcco::AgentEventError::UnknownType: return "unknown_type";
        case mcco::AgentEventError::SchemaViolation: return "schema_violation";
    }
    return "schema_violation";
}
} // namespace

bool AgentLink::begin(AppContext* ctx) {
    ctx_ = ctx;
    if (xTaskCreate(taskEntry, "mc_agent_ws", 8192, this, 5, &task_) != pdPASS) return false;
    esp_task_wdt_add(task_);
    timer_ = xTimerCreate("mc_agent", pdMS_TO_TICKS(1000), pdTRUE, this,
                          [](TimerHandle_t h) { static_cast<AgentLink*>(pvTimerGetTimerID(h))->tick(); });
    if (timer_) xTimerStart(timer_, 0);
    return true;
}

bool AgentLink::canAcceptWs() const {
    Guard g(const_cast<Mutex&>(handoff_mutex_));
    return handoff_q_.size() < kHandoffCap;
}

bool AgentLink::offerWsClient(WiFiClient client, const std::string& bearer) {
    {
        Guard g(handoff_mutex_);
        if (handoff_q_.size() >= kHandoffCap) return false;
        WsOffer offer;
        offer.client = client;  // shared_ptr share; every copy is ctor-counted
        offer.bearer = bearer;
        handoff_q_.push_back(std::move(offer));
    }
    if (task_) xTaskNotifyGive(task_);
    return true;
}

bool AgentLink::admitLocked(const std::string& bearer, const std::string& instance_id) const {
    return ctx_->pairing->tokenMatches(bearer) &&
           (instance_id.empty() || ctx_->pairing->instanceMatches(instance_id));
}

mcco::AgentSession* AgentLink::beginSessionLocked(uint8_t transport) {
    if (session_) {
        // A new hello on any transport supersedes the incumbent session
        // (spec 4.2.1: exactly one session; the new session_id wins).
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_superseded",
                         nullptr, nullptr, nullptr, nullptr);
        delete session_;
    }
    session_ = new mcco::AgentSession(*ctx_->rng, *ctx_->clock);
    transport_ = transport;
    // Polling has no socket to lose; the liveness timer reclaims the session
    // at OFFLINE. WS sessions set this when the socket dies mid-session.
    socket_dead_ = (transport == 2);
    pending_close_ = 0; // a queued close can only have targeted the incumbent
    last_conn_state_ = mcco::AgentConnState::AwaitingHello;
    return session_;
}

void AgentLink::applyHello(const mcco::AgentEvent& ev) {
    {
        Guard g(state_mutex_);
        last_conn_state_ = mcco::AgentConnState::Active;
        active_atomic_ = true;
    }
    mcco::AgentStatus st = ctx_->status_cache->snapshotAgent();
    const uint64_t now = ctx_->clock->epoch_seconds();
    st.paired = true;
    st.session_live = true;
    st.last_frame_at = now;
    JsonDocument p;
    if (!deserializeJson(p, ev.payload_json) && p.is<JsonObjectConst>()) {
        const char* boot = p["boot_id"].as<const char*>();
        if (boot) {
            st.has_boot = true;
            st.boot_id = boot;
            st.boot_at = now;
        }
    }
    ctx_->status_cache->setAgentStatus(st);
    ctx_->status_cache->onAgentChanged();
    ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Info, "agent_hello", nullptr,
                     nullptr, ev.agent_instance_id.c_str(), nullptr);
}

void AgentLink::applyEvidence(const mcco::AgentEvent& ev) {
    {
        Guard g(ctx_->engine_mutex);
        ctx_->pairing->touchLastUsed();
        ctx_->engine->agent_event(ev);
    }
    const uint64_t now = ctx_->clock->epoch_seconds();
    mcco::AgentStatus st = ctx_->status_cache->snapshotAgent();
    st.paired = true;
    st.session_live = true;
    st.last_frame_at = now;

    JsonDocument p;
    if (!deserializeJson(p, ev.payload_json) && p.is<JsonObjectConst>()) {
        JsonObjectConst o = p.as<JsonObjectConst>();
        switch (ev.type) {
            case mcco::AgentEventType::Hello: {
                const char* boot = o["boot_id"].as<const char*>();
                if (boot) {
                    st.has_boot = true;
                    st.boot_id = boot;
                    st.boot_at = now;
                }
                break;
            }
            case mcco::AgentEventType::SystemStateChanged: {
                const char* s = o["state"].as<const char*>();
                if (s) {
                    st.has_system = true;
                    st.system_state = s;
                    st.system_at = now;
                }
                break;
            }
            case mcco::AgentEventType::ScreenLockChanged:
                if (o["locked"].is<bool>()) {
                    st.has_lock = true;
                    st.locked = o["locked"].as<bool>();
                    st.lock_at = now;
                }
                break;
            case mcco::AgentEventType::UserSessionChanged:
                if (o["user_logged_in"].is<bool>()) {
                    st.has_user = true;
                    st.user_logged_in = o["user_logged_in"].as<bool>();
                    st.user = o["user"].is<const char*>() ? o["user"].as<const char*>() : "";
                    st.user_at = now;
                }
                break;
            case mcco::AgentEventType::ApplicationStarted: {
                const char* b = o["bundle_id"].as<const char*>();
                if (b) {
                    mcco::AgentAppState& a = st.apps[b];
                    a.running = true;
                    a.pid = o["pid"].is<int>() ? (int64_t)o["pid"].as<int>() : 0;
                    a.observed_at = now;
                }
                break;
            }
            case mcco::AgentEventType::ApplicationExited: {
                const char* b = o["bundle_id"].as<const char*>();
                if (b) {
                    mcco::AgentAppState& a = st.apps[b];
                    a.running = false;
                    a.pid = o["pid"].is<int>() ? (int64_t)o["pid"].as<int>() : 0;
                    a.observed_at = now;
                }
                break;
            }
            case mcco::AgentEventType::CapabilityReport: {
                st.has_capability = true;
                st.capability_at = now;
                st.enabled_commands.clear();
                for (JsonVariantConst c : o["enabled_commands"].as<JsonArrayConst>()) {
                    if (c.is<const char*>()) st.enabled_commands.emplace_back(c.as<const char*>());
                }
                st.allowlisted_apps.clear();
                for (JsonVariantConst e : o["allowlisted_apps"].as<JsonArrayConst>()) {
                    JsonObjectConst a = e.as<JsonObjectConst>();
                    if (a.isNull() || !a["bundle_id"].is<const char*>()) continue;
                    const char* b = a["bundle_id"].as<const char*>();
                    const bool running = a["state"].is<const char*>() &&
                                         strcmp(a["state"].as<const char*>(), "running") == 0;
                    st.allowlisted_apps.emplace_back(b, running);
                    if (st.apps.find(b) == st.apps.end()) {
                        mcco::AgentAppState s;
                        s.running = running;
                        s.observed_at = now;
                        st.apps[b] = s;
                    }
                }
                break;
            }
            default:
                break; // heartbeat/goodbye/command_ack/command_result: no status fields
        }
    }
    ctx_->status_cache->setAgentStatus(st);
    ctx_->status_cache->onAgentChanged();
    // Heartbeats are liveness-only (spec 6.2): they refresh the cache but
    // are not log-worthy — logging one per heartbeat churns the ring and
    // buries signal under noise.
    if (ev.type == mcco::AgentEventType::Heartbeat) return;
    std::string detail = std::string("{\"type\":\"") + mcco::agent_event_type_to_string(ev.type) +
                         "\"}";
    ctx_->log->write(mcco::LogCategory::Command, mcco::LogLevel::Info, "agent_evidence",
                     ev.has_command_id ? ev.command_id.c_str() : nullptr, nullptr, nullptr,
                     detail.c_str());
}

void AgentLink::teardownSession(mcco::AgentSession* mine, const char* event, uint16_t close_code,
                                bool offline_effect) {
    uint64_t last_frame_at;
    {
        Guard g(state_mutex_);
        if (session_ != mine) return; // already superseded; owner released it
        // Re-derive the epoch timestamp from the monotonic age so the cache
        // stays on the wall-clock timeline even across an SNTP jump.
        last_frame_at =
            ctx_->clock->epoch_seconds() - mine->lastFrameAgeMs() / 1000;
        session_ = nullptr;
        transport_ = 0;
        pending_close_ = 0; // a queued close targeted this session; do not leak it
        last_conn_state_ = mcco::AgentConnState::AwaitingHello;
        active_atomic_ = false;
    }
    mcco::AgentStatus st = ctx_->status_cache->snapshotAgent();
    st.session_live = false;
    st.last_frame_at = last_frame_at;
    ctx_->status_cache->setAgentStatus(st);
    ctx_->status_cache->onAgentChanged();

    std::string detail;
    if (close_code != 0) {
        detail = std::string("{\"close_code\":") + std::to_string(close_code) + "}";
    }
    const bool clean = close_code == 1000 || close_code == 1001;
    ctx_->log->write(mcco::LogCategory::Session,
                     clean ? mcco::LogLevel::Info : mcco::LogLevel::Warn, event, nullptr, nullptr,
                     nullptr, detail.empty() ? nullptr : detail.c_str());
    if (offline_effect) {
        Guard g(ctx_->engine_mutex);
        ctx_->engine->on_agent_offline();
    }
    delete mine;
}

void AgentLink::requestClose(uint16_t close_code) {
    mcco::AgentSession* dead = nullptr;
    {
        Guard g(state_mutex_);
        if (!session_) return;
        if (transport_ == 2) {
            // Polling session: no socket to close; end it in place.
            dead = session_;
            session_ = nullptr;
            transport_ = 0;
            last_conn_state_ = mcco::AgentConnState::AwaitingHello;
            active_atomic_ = false;
        } else {
            pending_close_ = close_code; // WS loop delivers the close frame
            return;
        }
    }
    mcco::AgentStatus st = ctx_->status_cache->snapshotAgent();
    st.session_live = false;
    ctx_->status_cache->setAgentStatus(st);
    ctx_->status_cache->onAgentChanged();
    std::string detail = std::string("{\"close_code\":") + std::to_string(close_code) + "}";
    ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_revoked", nullptr,
                     nullptr, nullptr, detail.c_str());
    delete dead;
}

void AgentLink::runWsSession(WsOffer offer, uint8_t* buf) {
    WiFiClient client = offer.client;
    client.setNoDelay(true);

    size_t len = 0;
    uint16_t cc = 0;
    // Hello wait: 5 s of silence before the first frame (spec 4.2.2).
    ws::FrameStatus fst = ws::read_frame(client, buf, kFrameCap, len, cc, kHelloTimeoutS * 1000);
    if (fst != ws::FrameStatus::Text && fst != ws::FrameStatus::Binary) {
        if (fst == ws::FrameStatus::Timeout) {
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn,
                             "agent_hello_timeout", nullptr, nullptr, nullptr, nullptr);
            ws::send_close(client, (uint16_t)mcco::AgentClose::PolicyViolation, "hello timeout");
        } else if (fst == ws::FrameStatus::Close) {
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Info, "agent_close",
                             nullptr, nullptr, nullptr, nullptr);
        } else {
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_error",
                             nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }

    mcco::AgentEvent ev;
    mcco::AgentEventError perr =
        mcco::parse_agent_event(std::string(reinterpret_cast<const char*>(buf), len), ev);
    if (perr != mcco::AgentEventError::Ok) {
        std::string detail = std::string("{\"reason\":\"") + parse_error_string(perr) + "\"}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                         nullptr, nullptr, nullptr, detail.c_str());
        ws::send_close(client, (uint16_t)mcco::AgentClose::PolicyViolation, "malformed hello");
        return;
    }

    // Admission (spec 2.2.2): token digest + instance id must both match.
    bool admitted;
    {
        Guard g(ctx_->engine_mutex);
        admitted = admitLocked(offer.bearer, ev.agent_instance_id);
    }
    if (!admitted) {
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                         nullptr, nullptr, nullptr, "{\"reason\":\"unpaired\"}");
        ws::send_close(client, (uint16_t)mcco::AgentClose::Unpaired, nullptr);
        return;
    }

    mcco::AgentSession* mine;
    uint16_t hello_cc = 0;
    {
        Guard g(state_mutex_);
        mine = beginSessionLocked(1);
        // acceptHello runs under the lock: the pointer is published and a
        // concurrent polling hello could otherwise supersede+delete it.
        hello_cc = mine->acceptHello(ev);
    }
    if (hello_cc != 0) {
        bool owned;
        {
            Guard g(state_mutex_);
            owned = (session_ == mine);
            if (owned) {
                session_ = nullptr;
                transport_ = 0;
                last_conn_state_ = mcco::AgentConnState::AwaitingHello;
            }
        }
        std::string detail = std::string("{\"close_code\":") + std::to_string(hello_cc) + "}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                         nullptr, nullptr, nullptr, detail.c_str());
        ws::send_close(client, hello_cc, nullptr);
        if (owned) delete mine; // else the superseder already released it
        return;
    }

    // hello_ack (spec 4.2.1). Session fields are read under the identity
    // check: only the current session_ is guaranteed alive.
    std::string sid;
    uint32_t hb_s, stale_s, offline_s;
    {
        Guard g(state_mutex_);
        if (session_ != mine) return; // superseded during admission
        sid = mine->sessionId();
        hb_s = mine->heartbeatIntervalS();
        stale_s = mine->staleThresholdS();
        offline_s = mine->offlineThresholdS();
    }
    {
        JsonDocument ack;
        ack["type"] = "hello_ack";
        ack["session_id"] = sid;
        ack["protocol_version"] = 1;
        ack["heartbeat_interval_s"] = hb_s;
        ack["stale_threshold_s"] = stale_s;
        ack["offline_threshold_s"] = offline_s;
        ack["max_seq_gap"] = 10;
        std::string body;
        serializeJson(ack, body);
        if (!ws::send_text(client, body)) {
            teardownSession(mine, "agent_error", 0, true);
            return;
        }
    }
    applyHello(ev);
    // Session loop: 1 s frame deadline so outbound dispatches and liveness
    // are serviced every iteration.

    for (;;) {
        esp_task_wdt_reset();

        uint16_t close_req = 0;
        bool current;
        {
            Guard g(state_mutex_);
            close_req = pending_close_;
            pending_close_ = 0;
            current = (session_ == mine);
        }
        if (close_req != 0) {
            if (!current) return; // superseded while the close was pending
            ws::send_close(client, close_req, nullptr);
            // Revocation (4001) is administrative: confirming records are left
            // to the deadline sweep; a supersede closes like a normal end.
            const bool revoked = close_req == (uint16_t)mcco::AgentClose::Unpaired;
            teardownSession(mine, revoked ? "agent_revoked" : "agent_superseded", close_req,
                            !revoked);
            return;
        }
        {
            Guard g(state_mutex_);
            if (session_ != mine) return; // a polling hello took ownership
        }
        {
            bool waiting;
            {
                Guard g(handoff_mutex_);
                waiting = !handoff_q_.empty();
            }
            if (waiting) {
                // A new WS client waits: finish this iteration, close cleanly.
                ws::send_close(client, (uint16_t)mcco::AgentClose::Normal, nullptr);
                teardownSession(mine, "agent_superseded", (uint16_t)mcco::AgentClose::Normal,
                                true);
                return;
            }
        }

        // Outbound: one dispatch per iteration while the session is ACTIVE.
        {
            mcco::AgentConnState st;
            {
                Guard g(state_mutex_);
                st = (session_ == mine) ? mine->liveness()
                                        : mcco::AgentConnState::Offline;
            }
            if (st == mcco::AgentConnState::Active) {
                PendingDispatch d;
                {
                    Guard g(pending_mutex_);
                    if (!pending_.empty()) {
                        d = pending_.front();
                        pending_.pop_front();
                    }
                }
                if (!d.command_id.empty()) {
                    JsonDocument doc;
                    doc["action"] = d.action;
                    doc["bundle_id"] = d.bundle_id;
                    doc["command_id"] = d.command_id;
                    std::string body;
                    serializeJson(doc, body);
                    if (!ws::send_text(client, body)) {
                        teardownSession(mine, "agent_send_failed", 0, true);
                        return;
                    }
                }
            }
        }

        fst = ws::read_frame(client, buf, kFrameCap, len, cc, kWsReadTimeoutS * 1000);
        if (fst == ws::FrameStatus::Timeout) continue;
        if (fst == ws::FrameStatus::Ping) {
            ws::send_pong(client, buf, len);
            continue;
        }
        if (fst == ws::FrameStatus::Pong) continue;
        if (fst == ws::FrameStatus::Close) {
            teardownSession(mine, "agent_close", cc, true);
            return;
        }
        if (fst == ws::FrameStatus::Dropped) {
            // Unannounced transport loss (spec 7.2.2 worked example 2: a
            // dead process means silence, not an instant verdict). The
            // session stays published and AGES on the liveness timer —
            // stale at 15 s, OFFLINE at 30 s — instead of flipping OFFLINE
            // at the TCP event. A graceful close frame (handled above)
            // terminates immediately, per spec 4.2.1.
            {
                Guard g(state_mutex_);
                if (session_ != mine) return;
                socket_dead_ = true;
            }
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn,
                             "agent_socket_lost", nullptr, nullptr, nullptr, nullptr);
            return; // the liveness timer owns the session from here
        }
        if (fst != ws::FrameStatus::Text && fst != ws::FrameStatus::Binary) {
            // Codec already sent a close frame for protocol errors.
            teardownSession(mine, "agent_error", 0, true);
            return;
        }

        const std::string frame(reinterpret_cast<const char*>(buf), len);
        perr = mcco::parse_agent_event(frame, ev);
        uint16_t close_out = 0;
        {
            Guard g(state_mutex_);
            if (session_ != mine) return; // superseded mid-session
            close_out = mine->onFrame(ev, perr);
        }
        if (close_out != 0) {
            std::string detail =
                std::string("{\"close_code\":") + std::to_string(close_out) + "}";
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                             nullptr, nullptr, nullptr, detail.c_str());
            ws::send_close(client, close_out, nullptr);
            teardownSession(mine, "agent_close", close_out, true);
            return;
        }
        if (perr == mcco::AgentEventError::Ok) {
            applyEvidence(ev);
        } else {
            std::string detail =
                std::string("{\"reason\":\"") + parse_error_string(perr) + "\"}";
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                             nullptr, nullptr, nullptr, detail.c_str());
        }
    }
}

void AgentLink::taskEntry(void* arg) {
    AgentLink* self = static_cast<AgentLink*>(arg);
    uint8_t* buf = (uint8_t*)heap_caps_malloc(kFrameCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)malloc(kFrameCap);
    if (!buf) {
        self->ctx_->log->write(mcco::LogCategory::System, mcco::LogLevel::Error,
                               "agent_ws_no_memory", nullptr, nullptr, nullptr, nullptr);
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        esp_task_wdt_reset();
        // Wait for a handoff (notification) or a 1 s tick so supersede
        // checks and shutdown stay responsive.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        for (;;) {
            WsOffer offer;
            {
                Guard g(self->handoff_mutex_);
                if (self->handoff_q_.empty()) break;
                offer = std::move(self->handoff_q_.front());
                self->handoff_q_.pop_front();
            }
            try {
                self->runWsSession(offer, buf);
            } catch (const std::exception&) {
                // OOM firewall: an allocation failure inside session handling
                // must not abort the firmware. The socket dies here; the
                // liveness timer ages the session out (unannounced-loss path).
                self->ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Error,
                                       "agent_session_exception", nullptr, nullptr, nullptr,
                                       nullptr);
            } catch (...) {
                self->ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Error,
                                       "agent_session_exception", nullptr, nullptr, nullptr,
                                       nullptr);
            }
            offer.client.stop();
        }
    }
}

void AgentLink::tick() {
    mcco::AgentSession* s = nullptr;
    mcco::AgentConnState st = mcco::AgentConnState::AwaitingHello;
    std::string sid;
    uint64_t last_frame_at = 0;
    bool reclaim = false;
    {
        Guard g(state_mutex_);
        s = session_;
        if (!s || !s->helloReceived()) {
            active_atomic_ = false;
            return;
        }
        st = s->liveness();
        // Re-derive the epoch timestamp from the monotonic age (SNTP-jump
        // safe); single wall-clock read keeps the cache timeline consistent.
        last_frame_at = ctx_->clock->epoch_seconds() - s->lastFrameAgeMs() / 1000;
        if (st == last_conn_state_) {
            active_atomic_ = (st == mcco::AgentConnState::Active);
            return;
        }
        last_conn_state_ = st;
        active_atomic_ = (st == mcco::AgentConnState::Active);
        sid = s->sessionId();
        // A session whose reader is gone (socket loss / polling silence
        // past OFFLINE) is reclaimed here: no task owns it anymore.
        reclaim = (st == mcco::AgentConnState::Offline) && socket_dead_;
        if (reclaim) {
            session_ = nullptr;
            transport_ = 0;
            socket_dead_ = false;
            pending_close_ = 0;
            last_conn_state_ = mcco::AgentConnState::AwaitingHello;
            active_atomic_ = false;
        }
    }

    if (reclaim) {
        std::string detail = std::string("{\"session_id\":\"") + sid + "\"}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Info, "agent_reclaimed",
                         nullptr, nullptr, nullptr, detail.c_str());
        delete s; // no reader can hold this pointer: safe
    }

    // Liveness transition (spec 4.2.1): the timer owns these for both
    // transports so a polling session ages exactly like a WS one.
    mcco::AgentStatus snap = ctx_->status_cache->snapshotAgent();
    snap.session_live = (st != mcco::AgentConnState::Offline);
    if (st == mcco::AgentConnState::Offline) snap.last_frame_at = last_frame_at;
    ctx_->status_cache->setAgentStatus(snap);
    ctx_->status_cache->onAgentChanged();

    if (st == mcco::AgentConnState::Stale) {
        std::string detail = std::string("{\"session_id\":\"") + sid + "\"}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_stale",
                         nullptr, nullptr, nullptr, detail.c_str());
    } else if (st == mcco::AgentConnState::Offline) {
        std::string detail = std::string("{\"session_id\":\"") + sid + "\"}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_offline",
                         nullptr, nullptr, nullptr, detail.c_str());
        // The ledger sweep (confirming -> unconfirmed, spec 5.2.1) is
        // deferred to the dispatcher task: the timer daemon task must never
        // block on engine_mutex/LittleFS or the task watchdog fires.
        offline_pending_ = true;
    }
}

bool AgentLink::pollEvent(const std::string& auth_bearer, const std::string& session_hdr,
                          const std::string& body, std::string& response_json,
                          mcco::ErrCode& err) {
    if (auth_bearer.empty() || !ctx_->pairing) {
        err = mcco::ErrCode::Unauthorized;
        return false;
    }
    {
        Guard g(ctx_->engine_mutex);
        if (!admitLocked(auth_bearer, "")) {
            // Spec 4.3.1: missing/unknown token -> 401; a KNOWN but revoked
            // credential -> 403 forbidden.
            err = ctx_->pairing->tokenDigestKnown(auth_bearer) ? mcco::ErrCode::Forbidden
                                                               : mcco::ErrCode::Unauthorized;
            return false;
        }
    }
    mcco::AgentEvent ev;
    mcco::AgentEventError perr = mcco::parse_agent_event(body, ev);
    if (perr != mcco::AgentEventError::Ok) {
        err = mcco::ErrCode::ValidationFailed;
        return false;
    }
    {
        Guard g(ctx_->engine_mutex);
        if (!ctx_->pairing->instanceMatches(ev.agent_instance_id)) {
            err = mcco::ErrCode::AgentNotPaired;
            return false;
        }
    }

    if (ev.type == mcco::AgentEventType::Hello) {
        mcco::AgentSession* s;
        uint16_t hello_cc = 0;
        {
            Guard g(state_mutex_);
            const uint8_t old_transport = transport_;
            s = beginSessionLocked(2);
            hello_cc = s->acceptHello(ev); // under the lock: s is published
            if (hello_cc == 0 && old_transport == 1) {
                // A polling hello over a live WS session: the WS loop closes
                // the incumbent socket with 1000 once it notices.
                pending_close_ = (uint16_t)mcco::AgentClose::Normal;
            }
        }
        if (hello_cc != 0) {
            bool owned;
            {
                Guard g(state_mutex_);
                owned = (session_ == s);
                if (owned) {
                    session_ = nullptr;
                    transport_ = 0;
                    last_conn_state_ = mcco::AgentConnState::AwaitingHello;
                }
            }
            std::string detail = std::string("{\"close_code\":") + std::to_string(hello_cc) + "}";
            ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                             nullptr, nullptr, nullptr, detail.c_str());
            if (owned) delete s; // else the superseder already released it
            err = mcco::ErrCode::ValidationFailed; // hello-level problem: request shape
            return false;
        }
        applyHello(ev);
        std::string sid;
        uint32_t hb_s, stale_s, offline_s;
        {
            Guard g(state_mutex_);
            if (session_ != s) { // superseded during admission
                err = mcco::ErrCode::AgentOffline;
                return false;
            }
            sid = s->sessionId();
            hb_s = s->heartbeatIntervalS();
            stale_s = s->staleThresholdS();
            offline_s = s->offlineThresholdS();
        }
        JsonDocument resp;
        resp["ok"] = true;
        resp["session_id"] = sid;
        resp["heartbeat_interval_s"] = hb_s;
        resp["stale_threshold_s"] = stale_s;
        resp["offline_threshold_s"] = offline_s;
        resp["max_seq_gap"] = 10;
        serializeJson(resp, response_json);
        return true;
    }

    mcco::AgentSession* s = nullptr;
    uint16_t cc = 0;
    {
        Guard g(state_mutex_);
        s = session_;
        if (!s || !s->helloReceived() || session_hdr != s->sessionId()) {
            err = mcco::ErrCode::AgentOffline; // spec 4.3.1: inactive/unknown session = 409
            return false;
        }
        cc = s->onFrame(ev, perr); // under the lock: s may be superseded any time
    }
    if (cc != 0) {
        // A session-killing problem inside a live polling session maps to the
        // HTTP rule (spec 4.3.1): the session is gone, so 409 agent_offline.
        std::string detail = std::string("{\"close_code\":") + std::to_string(cc) + "}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                         nullptr, nullptr, nullptr, detail.c_str());
        teardownSession(s, "agent_close", cc, true);
        err = mcco::ErrCode::AgentOffline;
        return false;
    }
    if (perr == mcco::AgentEventError::Ok) {
        applyEvidence(ev);
    } else {
        std::string detail = std::string("{\"reason\":\"") + parse_error_string(perr) + "\"}";
        ctx_->log->write(mcco::LogCategory::Session, mcco::LogLevel::Warn, "agent_rejected",
                         nullptr, nullptr, nullptr, detail.c_str());
    }
    JsonDocument resp;
    resp["ok"] = true;
    serializeJson(resp, response_json);
    return true;
}

bool AgentLink::pollPending(const std::string& auth_bearer, const std::string& session_hdr,
                            std::string& response_json, mcco::ErrCode& err) {
    if (auth_bearer.empty() || !ctx_->pairing) {
        err = mcco::ErrCode::Unauthorized;
        return false;
    }
    {
        Guard g(ctx_->engine_mutex);
        if (!admitLocked(auth_bearer, "")) {
            err = ctx_->pairing->tokenDigestKnown(auth_bearer) ? mcco::ErrCode::Forbidden
                                                               : mcco::ErrCode::Unauthorized;
            return false;
        }
    }
    {
        Guard g(state_mutex_);
        if (!session_ || !session_->helloReceived() || session_hdr != session_->sessionId()) {
            err = mcco::ErrCode::AgentOffline;
            return false;
        }
    }
    JsonDocument resp;
    JsonArray arr = resp["commands"].to<JsonArray>();
    {
        Guard g(pending_mutex_);
        while (!pending_.empty()) {
            const PendingDispatch& d = pending_.front();
            JsonObject o = arr.add<JsonObject>();
            o["action"] = d.action;
            o["bundle_id"] = d.bundle_id;
            o["command_id"] = d.command_id;
            pending_.pop_front();
        }
    }
    serializeJson(resp, response_json);
    return true;
}

bool AgentLink::enqueueDispatch(const std::string& action, const std::string& bundle_id,
                                const std::string& command_id) {
    Guard g(pending_mutex_);
    if (pending_.size() >= kPendingCap) {
        std::string detail = std::string("{\"command_id\":\"") + command_id + "\"}";
        ctx_->log->write(mcco::LogCategory::Command, mcco::LogLevel::Error,
                         "agent_dispatch_dropped", command_id.c_str(), nullptr, nullptr,
                         detail.c_str());
        return false;
    }
    PendingDispatch d;
    d.action = action;
    d.bundle_id = bundle_id;
    d.command_id = command_id;
    pending_.push_back(std::move(d));
    return true;
}
