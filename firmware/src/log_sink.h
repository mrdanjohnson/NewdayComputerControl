#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mc_clock.h"
#include "mc_log.h"
#include "mc_mutex.h"

class ConfigStore;

// In-RAM ring buffer of the last log entries (spec 15.2 schema:
// {seq,ts,category,event,level,command_id,request_id,session_id:null,actor,
// detail}). The seq counter is monotonic, 32-bit, persisted in NVS — loaded
// at boot, re-saved periodically and when dirty. Thread-safe. Retrieval is
// served by GET /api/v1/logs (Phase 3) via the filtered entries_since().
//
// Entries live in fixed-size static slots (BSS, not the heap): 512 heap
// std::strings were a ~128 KB heap tenant that left only ~30 KB free on the
// 320 KB S3 and made every later allocation OOM-prone under poll load.
// Entries that would exceed the slot are stored with an empty detail object.
class RamLogSink : public mcco::ILog {
public:
    RamLogSink(ConfigStore& config, mcco::IClock& clock);
    ~RamLogSink() override;

    void write(mcco::LogCategory cat, mcco::LogLevel level, const char* event,
               const char* command_id, const char* request_id, const char* actor,
               const char* detail_json) override;

    // Entries with seq strictly greater than `since`, ascending, at most limit.
    std::vector<std::string> entries_since(uint32_t since, size_t limit) const;
    // Filtered variant (spec 15.2): null category/level = no filter. When
    // `dropped_out` is non-null it receives log_dropped_count(since, oldest).
    std::vector<std::string> entries_since(uint32_t since, size_t limit,
                                           const mcco::LogCategory* category,
                                           const mcco::LogLevel* level,
                                           uint32_t* dropped_out) const;
    // Oldest retained seq, or 0 when the ring is empty.
    uint32_t oldest_seq() const;
    size_t count() const;

    // Called after ConfigStore::load() so the persisted seq counter is honored
    // (the sink may be constructed before NVS is readable).
    void restoreSeqFromConfig();

    // Default capacity is 128 (the spec minimum; 128-2048 is the allowed
    // range): on the 320 KB S3 every KB of static RAM shrinks the heap
    // arena, and the ring must leave headroom for Wi-Fi/lwIP under poll
    // load. Documented ESP32 build default (see PHASE4.md).
    static constexpr size_t kCapacity = 128;
    static constexpr size_t kEntryBytes = 224;  // fixed slot size (see above)

private:
    struct Entry {
        uint32_t seq;
        mcco::LogCategory cat;
        mcco::LogLevel level;
        uint16_t len;               // bytes used in json (<= kEntryBytes-1)
        char json[kEntryBytes];
    };
    void persistSeqIfDue(bool force);

    ConfigStore& config_;
    mcco::IClock& clock_;
    Entry ring_[kCapacity];
    size_t head_ = 0; // index of oldest entry
    size_t size_ = 0;
    uint32_t next_seq_;
    uint32_t writes_since_persist_ = 0;
    uint64_t last_persist_ms_ = 0;
    mutable Mutex mutex_;
};
