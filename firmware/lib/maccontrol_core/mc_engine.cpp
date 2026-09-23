#include "mc_engine.h"
#include <ArduinoJson.h>
#include <cstring>

namespace mcco {

CommandEngine::CommandEngine(Ledger& ledger, IClock& clock, IRandom& rng, ILog& log)
    : ledger_(ledger), clock_(clock), rng_(rng), log_(log) {}

bool CommandEngine::init() {
    if (!ledger_.load()) return false;
    size_t n = reconcile_boot();
    log_.write(LogCategory::System, LogLevel::Warn, "boot_reconciliation", nullptr, nullptr,
               nullptr, nullptr);
    initialized_ = true;
    (void)n;
    return true;
}

bool CommandEngine::validate_parameters(CommandType t, const std::string& params_json) {
    JsonDocument doc;
    if (deserializeJson(doc, params_json)) return false;
    if (!doc.is<JsonObject>()) return false;
    JsonObjectConst o = doc.as<JsonObjectConst>();
    switch (t) {
        case CommandType::Wake:
        case CommandType::Sleep:
        case CommandType::Restart:
        case CommandType::Shutdown:
        case CommandType::Lock:
            return o.size() == 0; // power commands carry no parameters
        case CommandType::MacroExecute:
            // The schema is validated here (400) before the 404 availability
            // check against the wired macro store in submit().
            return o.size() == 1 && o["macro_id"].is<const char*>();
        case CommandType::AppLaunch:
        case CommandType::AppQuit:
            return o.size() == 1 && o["bundle_id"].is<const char*>();
    }
    return false;
}

SubmissionOutcome CommandEngine::submit(const Submission& sub) {
    SubmissionOutcome out;
    if (!initialized_) {
        out.error = ErrCode::LedgerUnavailable;
        return out;
    }

    // Schema validation first: unknown keys / wrong shapes are 400 bad_request.
    if (!validate_parameters(sub.type, sub.parameters_json)) {
        out.error = ErrCode::BadRequest;
        return out;
    }

    // Availability: the surface is closed per phase (spec 12.1.1, 13.3, 16.3).
    // app_launch/app_quit are agent-dependent: the glue's gate decides the
    // deterministic pre-ledger 409 (agent_not_paired / agent_offline /
    // command_disabled / app_not_allowlisted) or accepts in Mode B.
    if (sub.type == CommandType::AppLaunch || sub.type == CommandType::AppQuit) {
        if (agent_gate_) {
            std::optional<ErrCode> gate_err = agent_gate_(sub);
            if (gate_err) {
                out.error = *gate_err;
                return out;
            }
        } else {
            out.error = ErrCode::AgentNotPaired; // no agent wired: Mode A
            return out;
        }
    }
    // macro_execute resolves against the macro store via the resolver hook
    // (spec 10.3): unknown macro_id terminates pre-ledger as 404 not_found.
    if (sub.type == CommandType::MacroExecute) {
        if (macro_resolver_) {
            JsonDocument pmd;
            if (deserializeJson(pmd, sub.parameters_json)) return out; // validated earlier
            const char* mid = pmd["macro_id"] | "";
            MacroResolution res = macro_resolver_(mid);
            if (!res.found) {
                out.error = ErrCode::NotFound;
                return out;
            }
        } else {
            out.error = ErrCode::NotFound; // no macro store wired
            return out;
        }
    }

    const uint64_t now = clock_.epoch_seconds();

    // Explicit idempotency key: replay (200, existing record, no re-dispatch)
    // or 409 conflict on divergent body (spec 5.1.1).
    if (!sub.idempotency_key.empty()) {
        auto it = explicit_keys_.find(sub.idempotency_key);
        if (it != explicit_keys_.end()) {
            const CommandRecord* existing = ledger_.latest(it->second.command_id);
            if (existing) {
                if (it->second.body_hash != sub.body_hash) {
                    out.error = ErrCode::Conflict;
                    return out;
                }
                out.ok = true;
                out.http_status = 200;
                out.replay = true;
                out.record = *existing;
                return out;
            }
            explicit_keys_.erase(it); // evicted: fall through to fresh accept
        }
    }

    // Derived-hash coalescing: identical in-flight submission within 60 s
    // returns the existing command_id with 202 (spec 5.1.1). Hashes live in
    // memory only; the dedup window resets across reboot by design.
    if (sub.idempotency_key.empty()) {
        for (const CommandRecord* rec : ledger_.list_newest_first()) {
            if (is_terminal(rec->state)) continue;
            if (rec->requested_at + kCoalesceWindowS < now) continue;
            auto h = hash_by_command_.find(rec->command_id);
            if (h != hash_by_command_.end() && h->second == sub.body_hash) {
                out.ok = true;
                out.http_status = 202;
                out.replay = true;
                out.record = *rec;
                return out;
            }
        }
    }

    CommandRecord rec;
    rec.command_id = make_command_id(rng_);
    rec.idempotency_key = sub.idempotency_key;
    rec.type = sub.type;
    rec.parameters_json = sub.parameters_json;
    rec.requested_by = sub.requested_by;
    rec.requested_at = now;
    rec.mode_at_accept = mode_; // pinned at acceptance (spec 5.1.1)
    rec.state = CommandState::Accepted;
    rec.deadline_at = now + default_deadline_s(sub.type);
    rec.has_window = (sub.type == CommandType::Sleep || sub.type == CommandType::Restart ||
                      sub.type == CommandType::Shutdown);
    if (rec.has_window) {
        rec.window_open_after_s = 3;   // spec 5.3.2 defaults
        rec.window_close_after_s = 60;
    }

    if (!ledger_.append_revision(rec)) {
        out.error = ErrCode::LedgerUnavailable; // nothing dispatched (spec 5.1.2)
        return out;
    }

    if (!sub.idempotency_key.empty())
        explicit_keys_[sub.idempotency_key] = {rec.command_id, sub.body_hash, now};
    hash_by_command_[rec.command_id] = sub.body_hash;

    out.ok = true;
    out.http_status = 202;
    out.record = rec;
    out.dispatch_pending = true;
    return out;
}

bool CommandEngine::complete_dispatch(const std::string& command_id, bool dispatch_ok,
                                      uint32_t deadline_override_s) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted) return false;

    const uint64_t now = clock_.epoch_seconds();

    CommandRecord disp = *cur;
    disp.state = CommandState::Dispatched;
    disp.dispatched_at = now;
    // Authoritative deadline at dispatch (spec 5.3.1, 10.3.1).
    disp.deadline_at =
        now + (deadline_override_s ? deadline_override_s : default_deadline_s(disp.type));
    if (!ledger_.append_revision(disp)) return false;

    // Mode B agent commands (spec 5.2.1): dispatch queues the action with the
    // MCA; the record stays `confirming` until agent_event() satisfies the
    // predicate or the deadline sweep / evidence-loss rules terminate it.
    if (dispatch_ok && mode_ == 'B' &&
        (disp.type == CommandType::AppLaunch || disp.type == CommandType::AppQuit)) {
        disp.state = CommandState::Confirming;
        return ledger_.append_revision(disp);
    }

    // Mode A (spec 5.2.2): dispatch success terminates immediately as
    // unconfirmed/hid_only. `completed` is unreachable without MCA evidence.
    CommandRecord term = disp;
    if (dispatch_ok) {
        term.state = CommandState::Unconfirmed;
        term.result = "hid_only";
    } else {
        term.state = CommandState::Failed;
        term.error_code = "dispatch_error";
    }
    return ledger_.append_revision(term);
}

bool CommandEngine::fail_dispatch(const std::string& command_id, const char* error_code) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted && cur->state != CommandState::Dispatched)
        return false;
    CommandRecord term = *cur;
    term.state = CommandState::Failed;
    term.error_code = error_code;
    return ledger_.append_revision(term);
}

bool CommandEngine::mark_dispatched(const std::string& command_id, uint32_t deadline_s) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted) return false;

    CommandRecord disp = *cur;
    disp.state = CommandState::Dispatched;
    disp.dispatched_at = clock_.epoch_seconds();
    // Authoritative deadline at dispatch (spec 5.3.1, 10.3.1).
    disp.deadline_at = disp.dispatched_at + deadline_s;
    return ledger_.append_revision(disp);
}

bool CommandEngine::terminate_mode_a(const std::string& command_id, bool ok) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Dispatched) return false;

    // Mode A (spec 5.2.2): success terminates `unconfirmed`/`hid_only`;
    // `completed` is unreachable without MCA evidence.
    CommandRecord term = *cur;
    if (ok) {
        term.state = CommandState::Unconfirmed;
        term.result = "hid_only";
    } else {
        term.state = CommandState::Failed;
        term.error_code = "dispatch_error";
    }
    return ledger_.append_revision(term);
}

size_t CommandEngine::sweep_deadlines() {
    const uint64_t now = clock_.epoch_seconds();
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state == CommandState::Confirming && rec->deadline_at <= now)
            ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;
        CommandRecord next = *cur;
        next.state = CommandState::TimedOut;
        next.error_code = "deadline_exceeded";
        if (ledger_.append_revision(next)) {
            agent_pred_.erase(id);
            n++;
        }
    }
    return n;
}

size_t CommandEngine::on_agent_offline() {
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        // Records inside an expected-offline window (sleep/restart/shutdown,
        // Phase 5) survive channel loss; nothing else does (spec 5.2.1).
        if (rec->state == CommandState::Confirming && !rec->has_window)
            ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;
        CommandRecord next = *cur;
        next.state = CommandState::Unconfirmed;
        next.result = "evidence_lost";
        if (ledger_.append_revision(next)) {
            agent_pred_.erase(id);
            n++;
        }
    }
    return n;
}

size_t CommandEngine::on_agent_declared_offline() {
    // A declared sleep/restart/shutdown (agent_goodbye) means channel loss is
    // expected: mark every confirming record with the spec 5.3.2 window so the
    // offline sweep in on_agent_offline() leaves it alone.
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state == CommandState::Confirming && !rec->has_window)
            ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming || cur->has_window) continue;
        CommandRecord next = *cur;
        next.has_window = true;
        next.window_open_after_s = 3;   // spec 5.3.2 defaults
        next.window_close_after_s = 60;
        if (ledger_.append_revision(next)) n++;
    }
    return n;
}

bool CommandEngine::agent_event(const AgentEvent& ev) {
    if (ev.type != AgentEventType::CommandAck && ev.type != AgentEventType::CommandResult &&
        ev.type != AgentEventType::ApplicationStarted &&
        ev.type != AgentEventType::ApplicationExited)
        return false;

    // Resolve the target record: correlated frames address it by command_id;
    // ambient app events match the newest confirming record for the bundle.
    const CommandRecord* rec = nullptr;
    if ((ev.type == AgentEventType::CommandAck || ev.type == AgentEventType::CommandResult) &&
        ev.has_command_id) {
        rec = ledger_.latest(ev.command_id);
    } else {
        JsonDocument p;
        if (deserializeJson(p, ev.payload_json)) return false;
        const char* bundle = p["bundle_id"].as<const char*>();
        if (!bundle) return false;
        for (const CommandRecord* c : ledger_.list_newest_first()) {
            if (c->state != CommandState::Confirming) continue;
            if (c->type != CommandType::AppLaunch && c->type != CommandType::AppQuit) continue;
            JsonDocument cp;
            if (deserializeJson(cp, c->parameters_json)) continue;
            std::string want = cp["bundle_id"] | "";
            if (want == bundle) { rec = c; break; }
        }
    }
    // Soft handling is deliberate: evidence for a record that is not
    // confirming (e.g. after a timeout) is ignored, never an error (6.1.1).
    if (!rec || rec->state != CommandState::Confirming) return false;
    if (rec->type != CommandType::AppLaunch && rec->type != CommandType::AppQuit) return false;

    auto pred = agent_pred_.find(rec->command_id);
    if (pred == agent_pred_.end()) pred = agent_pred_.emplace(rec->command_id, std::make_pair(false, false)).first;
    const bool ack_seen = pred->second.first;
    const bool app_seen = pred->second.second;

    CommandRecord next = *rec;
    next.evidence.push_back(ev.event_id);
    const char* result = nullptr;

    switch (ev.type) {
        case AgentEventType::CommandAck:
            pred->second.first = true;
            if (app_seen) {
                // ack arrived second: the matching application event was seen
                result = rec->type == CommandType::AppLaunch ? "app_launch_confirmed"
                                                             : "app_quit_confirmed";
            }
            break;
        case AgentEventType::CommandResult: {
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            if (strcmp(p["outcome"] | "ok", "failed") == 0) {
                // MCA-reported failure terminates with the closed error code.
                next.state = CommandState::Failed;
                next.error_code = p["error_code"] | "launch_failed";
            }
            // outcome ok reports local execution only and MUST NOT complete.
            break;
        }
        case AgentEventType::ApplicationStarted:
            if (rec->type == CommandType::AppLaunch) {
                pred->second.second = true;
                if (ack_seen) result = "app_launch_confirmed";
            }
            break;
        case AgentEventType::ApplicationExited: {
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            const char* reason = p["reason"] | "";
            if (rec->type == CommandType::AppQuit) {
                if (strcmp(reason, "crashed") == 0) {
                    next.state = CommandState::Failed;
                    next.error_code = "app_crashed";
                } else if (strcmp(reason, "quit") == 0 || strcmp(reason, "requested_by_agent") == 0) {
                    pred->second.second = true;
                    if (ack_seen) result = "app_quit_confirmed";
                }
            }
            break;
        }
        default:
            return false;
    }

    if (result) {
        // command_ack PLUS the matching application event, either order.
        next.state = CommandState::Completed;
        next.result = result;
        agent_pred_.erase(rec->command_id);
    }
    return ledger_.append_revision(next);
}

size_t CommandEngine::reconcile_boot() {
    const uint64_t now = clock_.epoch_seconds();
    size_t n = 0;
    // Snapshot ids first: append_revision may not reorder, but be safe.
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) ids.push_back(rec->command_id);
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const CommandRecord* cur = ledger_.latest(*it);
        if (!cur || is_terminal(cur->state)) continue;
        CommandRecord next = *cur;
        if (cur->deadline_at < now) {
            next.state = CommandState::TimedOut;
            next.error_code = "deadline_exceeded";
        } else {
            next.state = CommandState::Failed;
            next.error_code = "esp32_restarted";
        }
        if (ledger_.append_revision(next)) n++;
    }
    return n;
}

const CommandRecord* CommandEngine::get(const std::string& command_id) const {
    return ledger_.latest(command_id);
}

std::vector<const CommandRecord*> CommandEngine::list(const ListFilter& f) const {
    std::vector<const CommandRecord*> out;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (f.has_state && rec->state != f.state) continue;
        if (f.has_type && rec->type != f.type) continue;
        if (f.has_since && rec->requested_at < f.since) continue;
        out.push_back(rec);
    }
    return out;
}

} // namespace mcco
