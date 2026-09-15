#include "mc_engine.h"
#include <ArduinoJson.h>

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
            // Phase 1 has no macro store; the schema is still validated here
            // (400) before the 404 availability check in submit().
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
    // app_launch/app_quit are agent-dependent: reject pre-ledger in Mode A.
    if (sub.type == CommandType::AppLaunch || sub.type == CommandType::AppQuit) {
        out.error = ErrCode::AgentNotPaired; // pre-ledger: no record created
        return out;
    }
    // macro_execute requires the macro store, which ships in Phase 2.
    if (sub.type == CommandType::MacroExecute) {
        out.error = ErrCode::NotFound; // macro_id unknown: no macro store
        return out;
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
    rec.mode_at_accept = 'A';
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

bool CommandEngine::complete_dispatch(const std::string& command_id, bool dispatch_ok) {
    const CommandRecord* cur = ledger_.latest(command_id);
    if (!cur) return false;
    if (cur->state != CommandState::Accepted) return false;

    const uint64_t now = clock_.epoch_seconds();

    CommandRecord disp = *cur;
    disp.state = CommandState::Dispatched;
    disp.dispatched_at = now;
    disp.deadline_at = now + default_deadline_s(disp.type); // authoritative at dispatch
    if (!ledger_.append_revision(disp)) return false;

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
