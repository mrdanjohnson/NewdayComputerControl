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

    size_t command_count() const { return entries_.size(); }
    size_t capacity() const { return capacity_; }

    // Record <-> single-line JSON (used by storage and by the HTTP layer).
    static std::string record_to_json(const CommandRecord& rec);
    static bool record_from_json(const std::string& line, CommandRecord& rec);

private:
    struct Entry {
        std::vector<CommandRecord> revisions;
        uint64_t first_seq = 0; // global acceptance counter, defines FIFO order
    };

    bool persist_revision(const CommandRecord& rec);
    void evict_oldest();
    void compact();

    ILedgerStorage& storage_;
    IClock& clock_;
    ILog& log_;
    size_t capacity_;
    bool loaded_ = false;
    std::map<std::string, Entry> entries_;
    std::deque<std::string> fifo_; // command_ids in acceptance order
    uint64_t accept_seq_ = 0;
};

} // namespace mcco
