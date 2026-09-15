#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mc_clock.h"
#include "mc_log.h"
#include "mc_mutex.h"

class ConfigStore;

// In-RAM ring buffer of the last 512 log entries (spec 15.2 schema:
// {seq,ts,category,event,level,command_id,request_id,session_id:null,actor,
// detail}). The seq counter is monotonic, 32-bit, persisted in NVS — loaded
// at boot, re-saved periodically and when dirty. Thread-safe.
//
// Phase 1 note: the spec calls for flash persistence and a GET /api/v1/logs
// endpoint; the brief scopes Phase 1 to the in-RAM ring plus entries_since()
// for later phases, so that is what is implemented here.
class RamLogSink : public mcco::ILog {
public:
    RamLogSink(ConfigStore& config, mcco::IClock& clock);
    ~RamLogSink() override;

    void write(mcco::LogCategory cat, mcco::LogLevel level, const char* event,
               const char* command_id, const char* request_id, const char* actor,
               const char* detail_json) override;

    // Entries with seq strictly greater than `since`, ascending, at most limit.
    std::vector<std::string> entries_since(uint32_t since, size_t limit) const;
    size_t count() const;

    // Called after ConfigStore::load() so the persisted seq counter is honored
    // (the sink may be constructed before NVS is readable).
    void restoreSeqFromConfig();

    static constexpr size_t kCapacity = 512;

private:
    struct Entry {
        uint32_t seq;
        std::string json;
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
