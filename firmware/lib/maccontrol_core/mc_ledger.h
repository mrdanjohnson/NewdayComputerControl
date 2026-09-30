#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "mc_clock.h"
#include "mc_types.h"

namespace mcco {

class ILog;

// Persistent append-only backing for the ledger. One JSON record revision
// per line. The implementation must durably append a whole line; a torn tail
// line left by an interrupted write is discarded at boot (spec 5.1.1).
class ILedgerStorage {
public:
    virtual ~ILedgerStorage() = default;
    virtual bool append(const std::string& line) = 0;
    virtual bool read_all(const std::function<void(const std::string& line)>& cb) = 0;
    // Full compaction rewrite (used after FIFO eviction). Must be atomic
    // against power loss as far as the platform allows: write-then-commit.
    virtual bool replace_all(const std::vector<std::string>& lines) = 0;
    // Streaming compaction: rewrite the durable stream keeping only lines
    // where keep(line) is true. Default materializes via read_all +
    // replace_all (host/tests); flash backends MUST override with an
    // O(1)-heap temp-file copy — compact() at device scale (hundreds of KB)
    // OOMs with the default (bad_alloc panic, 2026-09-29).
    virtual bool rewrite_filtered(const std::function<bool(const std::string& line)>& keep) {
        std::vector<std::string> lines;
        read_all([&](const std::string& line) {
            if (keep(line)) lines.push_back(line);
        });
        return replace_all(lines);
    }
};

// Flash-backed append-only command ledger (spec 5.1.1). Records are never
// mutated in place; every state transition appends a new revision sharing the
// command_id, and the latest revision is the observable state.
class Ledger {
public:
    // capacity is counted in distinct commands, default 256 (spec bounds 64-1024).
    Ledger(ILedgerStorage& storage, IClock& clock, ILog& log, size_t capacity = 256);

    bool load();   // scan storage, rebuild index (call once at boot, before serving)
    bool loaded() const { return loaded_; }

    // Assigns the next revision number, persists, updates the index, and runs
    // FIFO eviction if over capacity. `rec` is updated in place (revision set).
    // Returns false if persistence failed (caller: 503 ledger_unavailable).
    bool append_revision(CommandRecord& rec);

    // Latest revision of a command, or nullptr if unknown / evicted.
    const CommandRecord* latest(const std::string& command_id) const;

    // All commands, newest acceptance first, for listing endpoints.
    std::vector<const CommandRecord*> list_newest_first() const;

    // Newest-first access by back-index, for mutation-safe iteration in the
    // engine: safe to call append_revision() (hence eviction) between calls
    // — a back-index keeps pointing at the same entry under fifo
    // push_back/pop_front, and entries erased by eviction simply drop out
    // (the caller's count runs past the shrunken fifo and the loop ends).
    // Hot paths (per-hello, per-tick) must use this instead of
    // list_newest_first(): that snapshot allocates O(commands) heap per
    // call, and bad_alloc there panics the device under reconnect churn
    // (observed 2026-09-29: ~9 s panic loop, backtrace handle_hello).
    const CommandRecord* newest_from_back(size_t k) const {
        if (k >= fifo_.size()) return nullptr;
        return latest(fifo_[fifo_.size() - 1 - k]);
    }

    size_t command_count() const { return entries_.size(); }
    size_t capacity() const { return capacity_; }

    // Record <-> single-line JSON (used by storage and by the HTTP layer).
    static std::string record_to_json(const CommandRecord& rec);
    static bool record_from_json(const std::string& line, CommandRecord& rec);

private:
    struct Entry {
        CommandRecord latest;       // highest-revision record (the observable state)
        uint32_t max_revision = 0;  // revision counter; the durable stream keeps history
        uint64_t first_seq = 0; // global acceptance counter, defines FIFO order
    };

    bool persist_revision(const CommandRecord& rec);
    // `run_compact` false lets batch callers (load-time trim) defer the
    // expensive full-file compact() until the last eviction — one rewrite
    // of the ledger file instead of one per evicted record (a 150+ record
    // boot trim would otherwise rewrite the file 150+ times and starve the
    // task watchdog mid-setup).
    void evict_oldest(bool run_compact = true);
    void compact();

    ILedgerStorage& storage_;
    IClock& clock_;
    ILog& log_;
    size_t capacity_;
    bool loaded_ = false;
    std::map<std::string, Entry> entries_;
    std::deque<std::string> fifo_; // command_ids in acceptance order
    std::vector<std::string> pending_evict_; // ids evicted since the last compact()
    uint64_t accept_seq_ = 0;
};

} // namespace mcco
