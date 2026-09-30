#include "mc_ledger.h"
#include "mc_iso8601.h"
#include "mc_log.h"
#include <ArduinoJson.h>

namespace mcco {

Ledger::Ledger(ILedgerStorage& storage, IClock& clock, ILog& log, size_t capacity)
    : storage_(storage), clock_(clock), log_(log), capacity_(capacity) {
    if (capacity_ < 64) capacity_ = 64;
    if (capacity_ > 1024) capacity_ = 1024;
}

std::string Ledger::record_to_json(const CommandRecord& rec) {
    JsonDocument doc;
    doc["command_id"] = rec.command_id;
    if (rec.idempotency_key.empty()) doc["idempotency_key"] = nullptr;
    else doc["idempotency_key"] = rec.idempotency_key;
    doc["revision"] = rec.revision;
    doc["type"] = command_type_to_string(rec.type);
    JsonDocument params;
    if (!rec.parameters_json.empty()) deserializeJson(params, rec.parameters_json);
    doc["parameters"] = params;
    doc["requested_by"] = rec.requested_by;
    doc["requested_at"] = iso8601_format(rec.requested_at).c_str();
    doc["mode_at_accept"] = std::string(1, rec.mode_at_accept).c_str();
    doc["state"] = command_state_to_string(rec.state);
    if (rec.dispatched_at == 0) doc["dispatched_at"] = nullptr;
    else doc["dispatched_at"] = iso8601_format(rec.dispatched_at).c_str();
    doc["deadline_at"] = iso8601_format(rec.deadline_at).c_str();
    if (rec.has_window) {
        JsonObject w = doc["expected_offline_window"].to<JsonObject>();
        w["open_after_s"] = rec.window_open_after_s;
        w["close_after_s"] = rec.window_close_after_s;
    } else {
        doc["expected_offline_window"] = nullptr;
    }
    JsonArray ev = doc["evidence"].to<JsonArray>();
    for (const auto& e : rec.evidence) ev.add(e);
    if (rec.result.empty()) doc["result"] = nullptr;
    else doc["result"] = rec.result;
    if (rec.error_code.empty()) doc["error_code"] = nullptr;
    else doc["error_code"] = rec.error_code;
    std::string out;
    serializeJson(doc, out);
    return out;
}

bool Ledger::record_from_json(const std::string& line, CommandRecord& rec) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    if (!doc["command_id"].is<const char*>()) return false;
    rec.command_id = doc["command_id"].as<const char*>();
    if (doc["idempotency_key"].is<const char*>())
        rec.idempotency_key = doc["idempotency_key"].as<const char*>();
    rec.revision = doc["revision"] | 1;
    const char* type = doc["type"] | "";
    if (!command_type_from_string(type, rec.type)) return false;
    if (doc["parameters"].is<JsonObjectConst>()) {
        std::string p;
        serializeJson(doc["parameters"], p);
        rec.parameters_json = p;
    } else {
        rec.parameters_json = "{}";
    }
    rec.requested_by = doc["requested_by"] | "";
    uint64_t t = 0;
    if (!iso8601_parse(doc["requested_at"] | "", t)) return false;
    rec.requested_at = t;
    std::string mode = doc["mode_at_accept"] | "A";
    rec.mode_at_accept = mode.empty() ? 'A' : mode[0];
    const char* state = doc["state"] | "";
    if (!command_state_from_string(state, rec.state)) return false;
    rec.dispatched_at = 0;
    if (doc["dispatched_at"].is<const char*>()) iso8601_parse(doc["dispatched_at"] | "", rec.dispatched_at);
    if (!iso8601_parse(doc["deadline_at"] | "", rec.deadline_at)) rec.deadline_at = 0;
    rec.has_window = false;
    if (doc["expected_offline_window"].is<JsonObjectConst>()) {
        JsonObjectConst w = doc["expected_offline_window"];
        rec.has_window = true;
        rec.window_open_after_s = w["open_after_s"] | 0;
        rec.window_close_after_s = w["close_after_s"] | 0;
    }
    rec.evidence.clear();
    if (doc["evidence"].is<JsonArrayConst>()) {
        for (JsonVariantConst e : doc["evidence"].as<JsonArrayConst>())
            if (e.is<const char*>()) rec.evidence.emplace_back(e.as<const char*>());
    }
    if (doc["result"].is<const char*>()) rec.result = doc["result"].as<const char*>();
    else rec.result.clear();
    if (doc["error_code"].is<const char*>()) rec.error_code = doc["error_code"].as<const char*>();
    else rec.error_code.clear();
    return true;
}

bool Ledger::load() {
    entries_.clear();
    fifo_.clear();
    accept_seq_ = 0;
    bool ok = storage_.read_all([&](const std::string& line) {
        if (line.empty()) return;
        CommandRecord rec;
        if (!record_from_json(line, rec)) return; // torn/corrupt line: skip
        auto it = entries_.find(rec.command_id);
        if (it == entries_.end()) {
            accept_seq_++;
            Entry e;
            e.first_seq = accept_seq_;
            e.latest = rec;
            e.max_revision = rec.revision;
            it = entries_.emplace(rec.command_id, std::move(e)).first;
            fifo_.push_back(rec.command_id);
        } else if (rec.revision >= it->second.max_revision) {
            // Highest revision wins (a duplicate revision line replaces, as
            // before); first appearance still defines acceptance/FIFO order.
            it->second.latest = rec;
            it->second.max_revision = rec.revision;
        }
    });
    loaded_ = ok;
    // Bound the RAM mirror to capacity at boot: the durable stream is
    // append-only and grows forever, and without this trim the mirror holds
    // the entire history after every boot (285+ records ≈ most of the
    // 320 KB S3 heap — the 2026-09-29 bad_alloc panic loop). Same semantics
    // as runtime eviction (spec 5.1.1): non-terminal victims are terminated
    // (persisted) here; terminal victims just leave the observable surface.
    // Deferred compact: one durable-stream rewrite at the end, not one per
    // evicted record (flash churn mid-setup starved the task watchdog).
    while (entries_.size() > capacity_) {
        evict_oldest(entries_.size() - 1 == capacity_);
    }
    return ok;
}

bool Ledger::persist_revision(const CommandRecord& rec) {
    return storage_.append(record_to_json(rec) + "\n");
}

bool Ledger::append_revision(CommandRecord& rec) {
    auto it = entries_.find(rec.command_id);
    if (it == entries_.end()) {
        accept_seq_++;
        Entry e;
        e.first_seq = accept_seq_;
        it = entries_.emplace(rec.command_id, std::move(e)).first;
        fifo_.push_back(rec.command_id);
        rec.revision = 1;
    } else {
        rec.revision = it->second.max_revision + 1;
    }
    if (!persist_revision(rec)) {
        if (it->second.max_revision == 0) {
            entries_.erase(it);
            fifo_.pop_back();
        }
        return false;
    }
    it->second.latest = rec;
    it->second.max_revision = rec.revision;
    log_.write(LogCategory::Command, LogLevel::Info, "state_transition", rec.command_id.c_str(),
               nullptr, nullptr,
               nullptr); // detail JSON filled by the engine layer
    while (entries_.size() > capacity_) evict_oldest();
    return true;
}

const CommandRecord* Ledger::latest(const std::string& command_id) const {
    auto it = entries_.find(command_id);
    if (it == entries_.end()) return nullptr;
    return &it->second.latest;
}

std::vector<const CommandRecord*> Ledger::list_newest_first() const {
    std::vector<const CommandRecord*> out;
    out.reserve(entries_.size());
    for (auto it = fifo_.rbegin(); it != fifo_.rend(); ++it) {
        const CommandRecord* rec = latest(*it);
        if (rec) out.push_back(rec);
    }
    return out;
}

void Ledger::evict_oldest(bool run_compact) {
    if (fifo_.empty()) return;
    const std::string victim = fifo_.front();
    auto it = entries_.find(victim);
    if (it != entries_.end()) {
        const CommandRecord* cur = latest(victim);
        if (cur && !is_terminal(cur->state)) {
            // Terminate before eviction (spec 5.1.1 eviction guard). Done
            // inline (not via append_revision) to avoid re-entering eviction.
            CommandRecord term = *cur;
            term.state = CommandState::Failed;
            term.error_code = "evicted_pending";
            term.revision = it->second.max_revision + 1;
            if (persist_revision(term)) {
                log_.write(LogCategory::Command, LogLevel::Warn, "evicted_pending", victim.c_str(),
                           nullptr, nullptr, nullptr);
            }
        }
        pending_evict_.push_back(victim);
        entries_.erase(it);
    }
    fifo_.pop_front();
    if (run_compact) compact();
}

void Ledger::compact() {
    if (pending_evict_.empty()) return;
    // Rewrite the durable stream minus evicted commands: the full revision
    // history stays on disk even though RAM holds one record per command.
    // Unparseable lines are kept verbatim (load() already skips them).
    // A failed rewrite keeps pending_evict_ so a later compact retries —
    // silently dropping the list here is how the durable stream used to
    // grow past capacity (evicted records reloaded and re-mirrored at boot).
    // rewrite_filtered streams at O(1) heap on the device. The keep filter
    // is a raw substring match on the serialized command_id, not a full
    // record_from_json per line: the deep ArduinoJson parse chain per line
    // piled onto the caller's stack (measured 27.6 KB peak on the 28 KB HTTP
    // task — stack-watchpoint panic 2026-09-30). command_ids are unique
    // random tokens, so `"command_id":"<id>"` cannot false-match another
    // record; lines that don't parse (kept verbatim per load() semantics)
    // won't contain a victim's id either.
    std::vector<std::string> needles;
    needles.reserve(pending_evict_.size());
    for (const auto& id : pending_evict_) {
        needles.push_back(std::string("\"command_id\":\"") + id + "\"");
    }
    const bool ok = storage_.rewrite_filtered([&](const std::string& line) {
        for (const auto& n : needles) {
            if (line.find(n) != std::string::npos) return false;
        }
        return true;
    });
    if (ok) pending_evict_.clear();
}

} // namespace mcco
