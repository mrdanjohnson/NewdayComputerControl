#include "mc_engine.h"
#include <ArduinoJson.h>
#include <cstring>

namespace mcco {

// Append an event id to a record's evidence exactly once (spec 5.1.1: the
// evidence array lists the MCA event ids that advanced the command).
static void push_evidence(CommandRecord& rec, const std::string& event_id) {
    if (event_id.empty()) return;
    for (const std::string& e : rec.evidence)
        if (e == event_id) return;
    rec.evidence.push_back(event_id);
}

// Value equality over parsed JSON scalars, for the macro expected_event
// exact-match rule (spec 10.1.1): bools compare as bools, numbers as
// integers, everything else as strings.
static bool json_value_equal(JsonVariantConst a, JsonVariantConst b) {
    if (a.is<bool>() || b.is<bool>())
        return a.is<bool>() && b.is<bool>() && a.as<bool>() == b.as<bool>();
    if (a.is<long long>() || b.is<long long>())
        return a.is<long long>() && b.is<long long>() && a.as<long long>() == b.as<long long>();
    const char* as = a.as<const char*>();
    const char* bs = b.as<const char*>();
    return as && bs && !strcmp(as, bs);
}

// True when the event payload satisfies the macro's expected_event match
// object: every declared key must be present with an equal value (spec
// 10.1.1 — e.g. {"bundle_id":"com.x"} or {"state":"awake"}).
static bool payload_matches(const std::string& payload_json, const std::string& match_json) {
    if (match_json.empty()) return true;
    JsonDocument mdoc;
    if (deserializeJson(mdoc, match_json) || !mdoc.is<JsonObject>()) return false;
    JsonDocument pdoc;
    if (deserializeJson(pdoc, payload_json) || !pdoc.is<JsonObject>()) return false;
    JsonObjectConst mo = mdoc.as<JsonObjectConst>();
    JsonObjectConst po = pdoc.as<JsonObjectConst>();
    for (JsonPairConst kv : mo) {
        JsonVariantConst pv = po[kv.key().c_str()];
        if (pv.isNull() || !json_value_equal(pv, kv.value())) return false;
    }
    return true;
}

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
        case CommandType::Unlock:
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

// True when the command type needs a live agent session at submit in Mode B
// (spec 12.3.1 gate). wake is deliberately absent: at wake time the Mac is
// asleep and the session is necessarily dead (spec §8.1.2) — wake is gated
// on pairing only, which the gate hook still enforces for app commands.
static bool is_session_gated(CommandType t) {
    return t == CommandType::Lock || t == CommandType::Sleep || t == CommandType::Restart ||
           t == CommandType::Shutdown;
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
    // command_disabled / app_not_allowlisted) or accepts in Mode B. The same
    // gate locks sleep/restart/shutdown/lock to a live session in Mode B
    // (§5.3.1 verification requires the evidence channel); in Mode A no gate
    // is wired and power commands accept, terminating honestly unconfirmed.
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
    } else if (is_session_gated(sub.type)) {
        if (agent_gate_) {
            std::optional<ErrCode> gate_err = agent_gate_(sub);
            if (gate_err) {
                out.error = *gate_err;
                return out;
            }
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
    const uint64_t now_mono = clock_.millis();

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
    // returns the existing command_id with 202 (spec 5.1.1). The window is
    // measured on the monotonic sidecar (epoch goes garbage across the SNTP
    // jump); records restored by boot reconciliation fall back to epoch.
    if (sub.idempotency_key.empty()) {
        for (const CommandRecord* rec : ledger_.list_newest_first()) {
            if (is_terminal(rec->state)) continue;
            auto m = mono_.find(rec->command_id);
            const bool in_window =
                m != mono_.end()
                    ? (m->second.accepted_mono_ms + kCoalesceWindowS * 1000 >= now_mono)
                    : (rec->requested_at + kCoalesceWindowS >= now);
            if (!in_window) continue;
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

    MonoDeadline md;
    md.accepted_mono_ms = now_mono;
    md.deadline_mono_ms = now_mono + uint64_t(default_deadline_s(sub.type)) * 1000;
    mono_[rec.command_id] = md;

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
    const uint64_t now_mono = clock_.millis();
    const uint32_t deadline_s = deadline_override_s ? deadline_override_s : default_deadline_s(cur->type);

    CommandRecord disp = *cur;
    disp.state = CommandState::Dispatched;
    disp.dispatched_at = now;
    // Authoritative deadline at dispatch (spec 5.3.1, 10.3.1).
    disp.deadline_at = now + deadline_s;
    if (!ledger_.append_revision(disp)) return false;

    auto md = mono_.find(command_id);
    if (md != mono_.end()) {
        md->second.dispatched_mono_ms = now_mono;
        md->second.deadline_mono_ms = now_mono + uint64_t(deadline_s) * 1000;
    } else {
        MonoDeadline fresh;
        fresh.accepted_mono_ms = now_mono;
        fresh.dispatched_mono_ms = now_mono;
        fresh.deadline_mono_ms = now_mono + uint64_t(deadline_s) * 1000;
        mono_[command_id] = fresh;
    }

    // Mode B (spec 5.2.1): dispatch queues the action; ALL command types stay
    // `confirming` after successful dispatch — the terminal verdict arrives
    // via agent_event()/on_agent_hello()/sweep_windows()/on_shutdown_probes()
    // or the deadline sweep. Mode A is unchanged below.
    if (dispatch_ok && mode_ == 'B') {
        disp.state = CommandState::Confirming;
        // Restart phase 2 compares the hello boot_id against the value cached
        // at dispatch (spec §8.2.1); empty means no baseline was ever seen.
        if (disp.type == CommandType::Restart)
            agent_pred_[command_id].boot_id_at_dispatch = known_boot_id_;
        return ledger_.append_revision(disp);
    }

    // Mode B wake with failed actuation: the HID keypress is best-effort —
    // a deeply sleeping host powers the USB port off, so no keypress can
    // succeed, yet the wake itself remains verifiable via the §8.1.2
    // evidence (post-dispatch new-session hello + awake burst). Advance to
    // confirming so that evidence can still complete the record; without
    // this the honest actuation failure terminated it dispatch_error
    // seconds before the human/early-wake evidence arrived (AT-08 wake
    // rounds, 2026-09-30). If the evidence never arrives the deadline sweep
    // still ends it honestly. Other types and Mode A keep the immediate
    // honest failure below.
    if (mode_ == 'B' && !dispatch_ok && cur->type == CommandType::Wake) {
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
    if (!ledger_.append_revision(term)) return false;
    drop_side(command_id);
    return true;
}

bool CommandEngine::fail_dispatch(const std::string& command_id, const char* error_code) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted && cur->state != CommandState::Dispatched)
        return false;
    CommandRecord term = *cur;
    term.state = CommandState::Failed;
    term.error_code = error_code;
    if (!ledger_.append_revision(term)) return false;
    drop_side(command_id);
    return true;
}

bool CommandEngine::mark_dispatched(const std::string& command_id, uint32_t deadline_s) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted) return false;

    const uint64_t now_mono = clock_.millis();
    CommandRecord disp = *cur;
    disp.state = CommandState::Dispatched;
    disp.dispatched_at = clock_.epoch_seconds();
    // Authoritative deadline at dispatch (spec 5.3.1, 10.3.1).
    disp.deadline_at = disp.dispatched_at + deadline_s;
    if (!ledger_.append_revision(disp)) return false;

    auto md = mono_.find(command_id);
    if (md != mono_.end()) {
        md->second.dispatched_mono_ms = now_mono;
        md->second.deadline_mono_ms = now_mono + uint64_t(deadline_s) * 1000;
    }
    return true;
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
    if (!ledger_.append_revision(term)) return false;
    drop_side(command_id);
    return true;
}

bool CommandEngine::macro_interpret_done(const std::string& command_id) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Dispatched) return false;

    // Mode B + expected_event (spec 10.3.1): the macro stays `confirming`
    // until a matching ambient event completes it (macro_confirmed) or the
    // deadline sweep ends it timed_out. Everything else — Mode A, or a macro
    // without an expected_event — is unverifiable and terminates honestly.
    bool verifiable = false;
    if (mode_ == 'B' && macro_resolver_) {
        JsonDocument pmd;
        if (deserializeJson(pmd, cur->parameters_json)) return false;
        const char* mid = pmd["macro_id"] | "";
        MacroResolution res = macro_resolver_(mid);
        verifiable = res.found && res.has_expected_event;
    }
    if (verifiable) {
        CommandRecord next = *cur;
        next.state = CommandState::Confirming;
        return ledger_.append_revision(next);
    }
    CommandRecord term = *cur;
    term.state = CommandState::Unconfirmed;
    term.result = "hid_only";
    if (!ledger_.append_revision(term)) return false;
    drop_side(command_id);
    return true;
}

size_t CommandEngine::sweep_deadlines() {
    const uint64_t now_mono = clock_.millis();
    const uint64_t now_epoch = clock_.epoch_seconds();
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state != CommandState::Confirming) continue;
        auto m = mono_.find(rec->command_id);
        const bool expired = m != mono_.end()
                                 ? (m->second.deadline_mono_ms <= now_mono)
                                 : (rec->deadline_at <= now_epoch);
        if (expired) ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;
        CommandRecord next = *cur;
        next.state = CommandState::TimedOut;
        next.error_code = "deadline_exceeded";
        if (ledger_.append_revision(next)) {
            drop_side(id);
            n++;
        }
    }
    return n;
}

size_t CommandEngine::sweep_windows(bool channel_offline) {
    const uint64_t now_mono = clock_.millis();
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state != CommandState::Confirming || !rec->has_window) continue;
        if (rec->type != CommandType::Sleep && rec->type != CommandType::Restart &&
            rec->type != CommandType::Shutdown)
            continue; // windowed non-power records only survive channel loss
        ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;
        auto m = mono_.find(id);
        if (m == mono_.end()) continue;
        const uint64_t open_ms =
            m->second.dispatched_mono_ms + uint64_t(cur->window_open_after_s) * 1000;
        const uint64_t close_ms =
            m->second.dispatched_mono_ms + uint64_t(cur->window_close_after_s) * 1000;

        PredProgress& pred = agent_pred_[id];
        if (!pred.offline_evidence && channel_offline && now_mono >= open_ms &&
            now_mono <= close_ms) {
            // Backstop for silence onsets the offline notification missed;
            // the primary path is on_agent_offline()/on_agent_declared_offline().
            pred.offline_evidence = true;
        }

        // Sleep completes when the window closes with no reconnect (spec
        // §8.1.1); restart waits for a changed-boot hello + awake burst, and
        // shutdown for the probe corroboration — both via their own inputs.
        if (cur->type == CommandType::Sleep && pred.offline_evidence && now_mono >= close_ms) {
            CommandRecord next = *cur;
            next.state = CommandState::Completed;
            next.result = "sleep_confirmed";
            if (ledger_.append_revision(next)) {
                drop_side(id);
                n++;
            }
        }
    }
    return n;
}

size_t CommandEngine::on_agent_offline() {
    const uint64_t now_mono = clock_.millis();
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state == CommandState::Confirming) ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;

        if (cur->has_window) {
            // Only the §8 power predicates evaluate silence onsets; records
            // of other types windowed by a declared goodbye (Phase 4.5) are
            // spared — the declaration already accounts for the absence.
            if (cur->type != CommandType::Sleep && cur->type != CommandType::Restart &&
                cur->type != CommandType::Shutdown)
                continue;
            // Windowed records absorb the silence onset: inside the window it
            // is positive offline evidence; before open it is a pre-existing
            // channel fault that cannot be attributed to the dispatch (§8).
            auto m = mono_.find(id);
            if (m == mono_.end()) continue;
            PredProgress& pred = agent_pred_[id];
            if (pred.offline_evidence) continue; // declared onset already absorbed
            const uint64_t open_ms =
                m->second.dispatched_mono_ms + uint64_t(cur->window_open_after_s) * 1000;
            const uint64_t close_ms =
                m->second.dispatched_mono_ms + uint64_t(cur->window_close_after_s) * 1000;
            if (now_mono < open_ms) {
                CommandRecord next = *cur;
                next.state = CommandState::Unconfirmed;
                next.result = "evidence_lost";
                if (ledger_.append_revision(next)) {
                    drop_side(id);
                    n++;
                }
            } else if (now_mono <= close_ms) {
                pred.offline_evidence = true; // declared = false: silence onset
            }
            // Late onset (past close) is not qualifying; the deadline sweep
            // ends the record timed_out (§8).
            continue;
        }

        // Records inside an expected-offline window survive channel loss;
        // nothing else does (spec 5.2.1).
        CommandRecord next = *cur;
        next.state = CommandState::Unconfirmed;
        next.result = "evidence_lost";
        if (ledger_.append_revision(next)) {
            drop_side(id);
            n++;
        }
    }
    return n;
}

size_t CommandEngine::on_agent_declared_offline() {
    // A declared sleep/restart/shutdown (agent_goodbye) means channel loss is
    // expected and self-attributing: mark every confirming record with the
    // spec 5.3.2 window so the offline sweep in on_agent_offline() leaves it
    // alone, and absorb the declared onset as positive offline evidence for
    // windowed power records (the goodbye names the dispatch as its cause, so
    // the open_after_s attribution floor applies to silence onsets only).
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
    // Absorb the declared onset for already-windowed power records.
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state != CommandState::Confirming || !rec->has_window) continue;
        if (rec->type != CommandType::Sleep && rec->type != CommandType::Restart &&
            rec->type != CommandType::Shutdown)
            continue;
        PredProgress& pred = agent_pred_[rec->command_id];
        if (!pred.offline_evidence) {
            pred.offline_evidence = true;
            pred.declared = true;
        }
    }
    return n;
}

void CommandEngine::set_known_boot_id(const char* boot_id) {
    known_boot_id_ = boot_id ? boot_id : "";
}

size_t CommandEngine::on_agent_hello(const char* boot_id, bool had_boot,
                                     const char* prev_boot_id) {
    return handle_hello(boot_id, had_boot, prev_boot_id, nullptr);
}

size_t CommandEngine::handle_hello(const char* boot_id, bool had_boot, const char* prev_boot_id,
                                   const char* event_id) {
    const uint64_t now_mono = clock_.millis();
    const std::string prev_engine = known_boot_id_;
    size_t n = 0;
    // Back-index traversal: no O(commands) snapshot allocation (the
    // list_newest_first() snapshot here panicked with bad_alloc under
    // reconnect churn), and safe against the append_revision() calls below.
    const size_t count = ledger_.command_count();
    for (size_t k = 0; k < count; ++k) {
        const CommandRecord* cur = ledger_.newest_from_back(k);
        if (!cur) continue;
        const std::string id = cur->command_id;

        // Terminal records never reopen; a reconnect after a completed sleep
        // or shutdown is logged as post-terminal evidence (§5.3.2) and MUST
        // NOT alter the record.
        if (is_terminal(cur->state)) {
            if (cur->result == "sleep_confirmed") {
                log_.write(LogCategory::Command, LogLevel::Warn, "unexpected_wake",
                           id.c_str(), nullptr, nullptr, "{\"detail\":\"post_terminal_reconnect\"}");
            } else if (cur->result == "shutdown_confirmed") {
                log_.write(LogCategory::Command, LogLevel::Warn, "unexpected_reconnect",
                           id.c_str(), nullptr, nullptr, "{\"detail\":\"post_terminal_reconnect\"}");
            }
            continue;
        }
        if (cur->state != CommandState::Confirming) continue;

        auto pred_it = agent_pred_.find(id);
        switch (cur->type) {
            case CommandType::Wake:
                // A post-dispatch new-session hello is the wake predicate's
                // first half (spec §8.1.2); the awake burst completes it.
                if (pred_it == agent_pred_.end()) pred_it = agent_pred_.emplace(id, PredProgress{}).first;
                if (!pred_it->second.hello) {
                    pred_it->second.hello = true;
                    if (event_id && *event_id) {
                        CommandRecord next = *cur;
                        push_evidence(next, event_id);
                        ledger_.append_revision(next);
                    }
                }
                break;

            case CommandType::Sleep: {
                // A reconnect before the window closes refutes the sleep
                // (§8.1.1). At/after close the sweep owns the verdict.
                auto m = mono_.find(id);
                if (m == mono_.end()) break;
                const uint64_t close_ms =
                    m->second.dispatched_mono_ms + uint64_t(cur->window_close_after_s) * 1000;
                if (now_mono >= close_ms) break;
                CommandRecord next = *cur;
                next.state = CommandState::Failed;
                next.error_code = "unexpected_wake";
                if (event_id) push_evidence(next, event_id);
                if (ledger_.append_revision(next)) {
                    drop_side(id);
                    n++;
                }
                break;
            }

            case CommandType::Restart: {
                if (pred_it == agent_pred_.end()) pred_it = agent_pred_.emplace(id, PredProgress{}).first;
                PredProgress& pred = pred_it->second;
                // Identity baseline: boot_id cached at dispatch, else the
                // glue-supplied previous value, else the engine's own cache.
                std::string baseline = pred.boot_id_at_dispatch;
                if (baseline.empty() && had_boot && prev_boot_id) baseline = prev_boot_id;
                if (baseline.empty()) baseline = prev_engine;
                const bool identity_changed =
                    !baseline.empty() && boot_id && *boot_id && baseline != boot_id;
                if (identity_changed) {
                    pred.hello = true; // phase 2: await the awake burst
                    pred.offline_evidence = true; // a new boot proves the host went down
                    if (event_id && *event_id) {
                        CommandRecord next = *cur;
                        push_evidence(next, event_id);
                        ledger_.append_revision(next);
                    }
                } else {
                    // Same (or unknown) boot_id: an agent-process restart
                    // voids the phase-1 window evidence; the record stays
                    // confirming and the deadline sweep ends it timed_out
                    // (§8.2.1). The contradiction is logged, not fatal.
                    pred.hello = false;
                    pred.offline_evidence = false;
                    pred.declared = false;
                    log_.write(LogCategory::Command, LogLevel::Warn, "unexpected_reconnect",
                               id.c_str(), nullptr, nullptr,
                               "{\"detail\":\"restart_boot_id_unchanged\"}");
                }
                break;
            }

            case CommandType::Shutdown: {
                // Any MCA reconnect refutes a shutdown (§8.2.2).
                CommandRecord next = *cur;
                next.state = CommandState::Failed;
                next.error_code = "unexpected_reconnect";
                if (event_id) push_evidence(next, event_id);
                if (ledger_.append_revision(next)) {
                    drop_side(id);
                    n++;
                }
                break;
            }

            default:
                break; // app/macro/lock records: a hello is not their evidence
        }
    }
    if (boot_id && *boot_id) known_boot_id_ = boot_id;
    return n;
}

size_t CommandEngine::on_shutdown_probes(bool any_reachable) {
    size_t n = 0;
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state == CommandState::Confirming && rec->type == CommandType::Shutdown)
            ids.push_back(rec->command_id);
    }
    for (const std::string& id : ids) {
        const CommandRecord* cur = ledger_.latest(id);
        if (!cur || cur->state != CommandState::Confirming) continue;
        PredProgress& pred = agent_pred_[id];
        if (any_reachable) {
            // The host answered: the shutdown did not take effect (§8.2.2).
            CommandRecord next = *cur;
            next.state = CommandState::Failed;
            next.error_code = "host_still_reachable";
            if (ledger_.append_revision(next)) {
                drop_side(id);
                n++;
            }
            continue;
        }
        if (++pred.probe_fails >= 3) {
            CommandRecord next = *cur;
            next.state = CommandState::Completed;
            next.result = "shutdown_confirmed";
            if (ledger_.append_revision(next)) {
                drop_side(id);
                n++;
            }
        }
    }
    return n;
}

bool CommandEngine::shutdown_probe_open(bool channel_offline) const {
    if (!channel_offline) return false;
    const uint64_t now_mono = clock_.millis();
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state != CommandState::Confirming || rec->type != CommandType::Shutdown ||
            !rec->has_window)
            continue;
        auto m = mono_.find(rec->command_id);
        if (m == mono_.end()) continue;
        if (now_mono >= m->second.dispatched_mono_ms +
                            uint64_t(rec->window_close_after_s) * 1000)
            return true;
    }
    return false;
}

bool CommandEngine::agent_event(const AgentEvent& ev) {
    switch (ev.type) {
        case AgentEventType::CommandAck:
        case AgentEventType::CommandResult: {
            if (!ev.has_command_id) return false;
            const CommandRecord* rec = ledger_.latest(ev.command_id);
            // Soft handling is deliberate: evidence for a record that is not
            // confirming (e.g. after a timeout) is ignored, never an error
            // (spec 6.1.1); terminal records are never reopened (§5).
            if (!rec || rec->state != CommandState::Confirming) return false;
            if (rec->type != CommandType::AppLaunch && rec->type != CommandType::AppQuit)
                return false;

            auto pred = agent_pred_.find(rec->command_id);
            if (pred == agent_pred_.end()) pred = agent_pred_.emplace(rec->command_id, PredProgress{}).first;
            const bool ack_seen = pred->second.ack;
            const bool app_seen = pred->second.app_event;

            CommandRecord next = *rec;
            push_evidence(next, ev.event_id);
            const char* result = nullptr;

            switch (ev.type) {
                case AgentEventType::CommandAck:
                    pred->second.ack = true;
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
                default:
                    return false;
            }

            if (result) {
                // command_ack PLUS the matching application event, either order.
                next.state = CommandState::Completed;
                next.result = result;
            }
            if (next.state == CommandState::Failed || result) {
                if (!ledger_.append_revision(next)) return false;
                drop_side(rec->command_id);
                return true;
            }
            // Non-terminal progress: persist the evidence-bearing revision.
            return ledger_.append_revision(next);
        }

        case AgentEventType::Hello: {
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            const char* boot_id = p["boot_id"] | "";
            const bool had_boot = !known_boot_id_.empty();
            return handle_hello(boot_id, had_boot, known_boot_id_.c_str(),
                                ev.event_id.c_str()) > 0;
        }

        case AgentEventType::SystemStateChanged: {
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            if (strcmp(p["state"] | "", "awake") != 0) return false;
            // The mandatory initial-status awake burst completes wake (after
            // its hello) and restart (after a changed-boot hello) — §8.1.2/§8.2.1.
            bool advanced = false;
            for (const CommandRecord* rec : ledger_.list_newest_first()) {
                if (rec->state != CommandState::Confirming) continue;
                auto pred = agent_pred_.find(rec->command_id);
                const bool hello = pred != agent_pred_.end() && pred->second.hello;
                const bool offline = pred != agent_pred_.end() && pred->second.offline_evidence;
                const char* result = nullptr;
                if (rec->type == CommandType::Wake && hello) result = "wake_confirmed";
                // Restart is two-phase: in-window offline evidence, then the
                // changed-boot hello, then this awake burst (§8.2.1).
                if (rec->type == CommandType::Restart && hello && offline)
                    result = "restart_confirmed";
                if (!result) continue;
                CommandRecord next = *rec;
                push_evidence(next, ev.event_id);
                next.state = CommandState::Completed;
                next.result = result;
                if (ledger_.append_revision(next)) {
                    drop_side(rec->command_id);
                    advanced = true;
                }
                break; // one burst advances the newest qualifying record
            }
            // The five ambient types also feed the macro expected_event
            // predicates (spec 10.3.1) — fall through to the macro matcher.
            return advanced || match_macro_event(ev);
        }

        case AgentEventType::ScreenLockChanged: {
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            const bool locked = p["locked"] | false;
            bool advanced = false;
            // Amendment (2026-09-30): a lock->unlock transition completes the
            // newest confirming `unlock` record. This is the predicate at BOTH
            // the lock screen (agent alive in the locked session) and the
            // login window (agent appears only after login, reporting the
            // ScreenIsLocked false transition then).
            if (!locked) {
                for (const CommandRecord* rec : ledger_.list_newest_first()) {
                    if (rec->state != CommandState::Confirming ||
                        rec->type != CommandType::Unlock)
                        continue;
                    CommandRecord next = *rec;
                    push_evidence(next, ev.event_id);
                    next.state = CommandState::Completed;
                    next.result = "unlock_confirmed";
                    if (ledger_.append_revision(next)) {
                        drop_side(rec->command_id);
                        advanced = true;
                    }
                    break;
                }
            }
            if (advanced) return true;
            if (locked) {
                for (const CommandRecord* rec : ledger_.list_newest_first()) {
                    if (rec->state != CommandState::Confirming ||
                        rec->type != CommandType::Lock)
                        continue;
                    CommandRecord next = *rec;
                    push_evidence(next, ev.event_id);
                    next.state = CommandState::Completed;
                    next.result = "lock_confirmed";
                    if (ledger_.append_revision(next)) {
                        drop_side(rec->command_id);
                        advanced = true;
                    }
                    break;
                }
            }
            return advanced || match_macro_event(ev);
        }

        case AgentEventType::ApplicationStarted:
        case AgentEventType::ApplicationExited: {
            // Resolve the app target: ambient app events match the newest
            // confirming app_launch/app_quit record on exact bundle_id.
            JsonDocument p;
            if (deserializeJson(p, ev.payload_json)) return false;
            const char* bundle = p["bundle_id"].as<const char*>();
            if (!bundle) return false;
            bool advanced = false;
            for (const CommandRecord* rec : ledger_.list_newest_first()) {
                if (rec->state != CommandState::Confirming) continue;
                if (rec->type != CommandType::AppLaunch && rec->type != CommandType::AppQuit)
                    continue;
                JsonDocument cp;
                if (deserializeJson(cp, rec->parameters_json)) continue;
                std::string want = cp["bundle_id"] | "";
                if (want != bundle) continue;

                auto pred = agent_pred_.find(rec->command_id);
                if (pred == agent_pred_.end())
                    pred = agent_pred_.emplace(rec->command_id, PredProgress{}).first;
                const bool ack_seen = pred->second.ack;

                CommandRecord next = *rec;
                push_evidence(next, ev.event_id);
                const char* result = nullptr;

                if (ev.type == AgentEventType::ApplicationStarted) {
                    if (rec->type == CommandType::AppLaunch) {
                        pred->second.app_event = true;
                        if (ack_seen) result = "app_launch_confirmed";
                    }
                } else {
                    const char* reason = p["reason"] | "";
                    if (rec->type == CommandType::AppQuit) {
                        if (strcmp(reason, "crashed") == 0) {
                            next.state = CommandState::Failed;
                            next.error_code = "app_crashed";
                        } else if (strcmp(reason, "quit") == 0 ||
                                   strcmp(reason, "requested_by_agent") == 0) {
                            pred->second.app_event = true;
                            if (ack_seen) result = "app_quit_confirmed";
                        }
                    }
                }

                if (next.state == CommandState::Failed || result) {
                    if (result) next.result = result;
                    if (next.state != CommandState::Failed) next.state = CommandState::Completed;
                    if (ledger_.append_revision(next)) {
                        drop_side(rec->command_id);
                        advanced = true;
                    }
                } else {
                    if (ledger_.append_revision(next)) advanced = true;
                }
                break;
            }
            return advanced || match_macro_event(ev);
        }

        case AgentEventType::UserSessionChanged:
            // Never advances a command directly (spec 6.2.1); usable only as
            // a macro expected_event.
            return match_macro_event(ev);

        default:
            // Heartbeat, agent_goodbye, capability_report, front_app_changed:
            // liveness/status evidence only — MUST NOT advance any command.
            return false;
    }
}

// Macro expected_event matching (spec 10.3.1): the newest confirming
// macro_execute record whose macro declares an expected_event of this type
// and whose match object the payload satisfies completes macro_confirmed.
bool CommandEngine::match_macro_event(const AgentEvent& ev) {
    if (!macro_resolver_) return false;
    const char* type_str = agent_event_type_to_string(ev.type);
    for (const CommandRecord* rec : ledger_.list_newest_first()) {
        if (rec->state != CommandState::Confirming || rec->type != CommandType::MacroExecute)
            continue;
        JsonDocument pmd;
        if (deserializeJson(pmd, rec->parameters_json)) continue;
        const char* mid = pmd["macro_id"] | "";
        MacroResolution res = macro_resolver_(mid);
        if (!res.has_expected_event || res.expected_event_type != type_str) continue;
        if (!payload_matches(ev.payload_json, res.expected_match_json)) continue;
        CommandRecord next = *rec;
        push_evidence(next, ev.event_id);
        next.state = CommandState::Completed;
        next.result = "macro_confirmed";
        if (!ledger_.append_revision(next)) return false;
        drop_side(rec->command_id);
        return true;
    }
    return false;
}

size_t CommandEngine::reconcile_boot() {
    const uint64_t now = clock_.epoch_seconds();
    const uint64_t now_mono = clock_.millis();
    size_t n = 0;
    // Snapshot ids first: append_revision may not reorder, but be safe.
    std::vector<std::string> ids;
    for (const CommandRecord* rec : ledger_.list_newest_first()) ids.push_back(rec->command_id);
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const CommandRecord* cur = ledger_.latest(*it);
        if (!cur || is_terminal(cur->state)) continue;
        if (cur->deadline_at < now) {
            CommandRecord next = *cur;
            next.state = CommandState::TimedOut;
            next.error_code = "deadline_exceeded";
            if (ledger_.append_revision(next)) {
                drop_side(*it);
                n++;
            }
            continue;
        }
        // Deadline open: resumable predicate commands (dispatched or still
        // confirming) resume in confirming (spec 5.1.1 — wake is the named
        // example; the §8 windowed commands survive a mid-window ESP32
        // reboot). Never-dispatched (accepted) records and non-predicate
        // commands resolve failed/esp32_restarted.
        const bool resumable =
            (cur->state == CommandState::Dispatched || cur->state == CommandState::Confirming) &&
            (cur->type == CommandType::Wake || cur->type == CommandType::Sleep ||
             cur->type == CommandType::Restart || cur->type == CommandType::Shutdown);
        if (resumable) {
            if (cur->state != CommandState::Confirming) {
                CommandRecord next = *cur;
                next.state = CommandState::Confirming;
                if (!ledger_.append_revision(next)) continue;
                n++;
            }
            // The monotonic sidecar is RAM-only and lost across reboot. The
            // epoch deadline arithmetic is spec-pinned (5.1.1, 2 s bound), but
            // window/interval math needs a valid monotonic anchor: restart the
            // window at reconciliation with the remaining epoch-approximated
            // budget. Downtime extends the window — deterministic and biased
            // toward timeout, never toward early completion.
            if (cur->has_window) {
                uint64_t remaining_ms = (cur->deadline_at > now) ? (cur->deadline_at - now) * 1000
                                                                 : 0;
                MonoDeadline md;
                md.accepted_mono_ms = now_mono;
                md.dispatched_mono_ms = now_mono;
                md.deadline_mono_ms = now_mono + remaining_ms;
                mono_[*it] = md;
            }
            continue;
        }
        CommandRecord next = *cur;
        next.state = CommandState::Failed;
        next.error_code = "esp32_restarted";
        if (ledger_.append_revision(next)) {
            drop_side(*it);
            n++;
        }
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
