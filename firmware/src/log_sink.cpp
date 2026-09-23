#include "log_sink.h"
#include <stdio.h>
#include <string.h>
#include "mc_iso8601.h"
#include "nvs_config.h"

RamLogSink::RamLogSink(ConfigStore& config, mcco::IClock& clock)
    : config_(config), clock_(clock), next_seq_(config.logSeq() + 1) {}

RamLogSink::~RamLogSink() { persistSeqIfDue(true); }

void RamLogSink::write(mcco::LogCategory cat, mcco::LogLevel level, const char* event,
                       const char* command_id, const char* request_id, const char* actor,
                       const char* detail_json) {
    const uint64_t epoch = clock_.epoch_seconds();
    const uint32_t ms = (uint32_t)(clock_.millis() % 1000);
    // Spec 15.2 schema carries millisecond timestamps: inject ".%03u" before Z.
    std::string iso = mcco::iso8601_format(epoch);
    std::string ts = iso.substr(0, iso.size() - 1) + ".";

    char num[8];
    snprintf(num, sizeof(num), "%03u", ms);
    ts += num;
    ts += "Z";

    auto quoted = [](const char* s) -> std::string {
        if (!s) return "null";
        return std::string("\"") + s + "\"";
    };

    {
        Guard g(mutex_);
        std::string json;
        json.reserve(160);
        json += "{\"seq\":";
        json += std::to_string(next_seq_);
        json += ",\"ts\":\"";
        json += ts;
        json += "\",\"category\":\"";
        json += mcco::log_category_string(cat);
        json += "\",\"event\":";
        json += quoted(event);
        json += ",\"level\":\"";
        json += mcco::log_level_string(level);
        json += "\",\"command_id\":";
        json += quoted(command_id);
        json += ",\"request_id\":";
        json += quoted(request_id);
        json += ",\"session_id\":null,\"actor\":";
        json += quoted(actor);
        json += ",\"detail\":";
        json += detail_json ? detail_json : "{}";
        json += "}";
        if (json.size() >= kEntryBytes) {
            // Static slots cannot grow: re-emit without the detail payload
            // (correlation ids and the event name are preserved).
            json.clear();
            json += "{\"seq\":";
            json += std::to_string(next_seq_);
            json += ",\"ts\":\"";
            json += ts;
            json += "\",\"category\":\"";
            json += mcco::log_category_string(cat);
            json += "\",\"event\":";
            json += quoted(event);
            json += ",\"level\":\"";
            json += mcco::log_level_string(level);
            json += "\",\"command_id\":";
            json += quoted(command_id);
            json += ",\"request_id\":";
            json += quoted(request_id);
            json += ",\"session_id\":null,\"actor\":";
            json += quoted(actor);
            json += ",\"detail\":{}}";
        }
        // Hard safety clamp: a slot overflow must never corrupt the ring.
        if (json.size() > kEntryBytes - 1) json.resize(kEntryBytes - 1);

        Entry& slot = ring_[(head_ + size_) % kCapacity];
        slot.seq = next_seq_;
        slot.cat = cat;
        slot.level = level;
        slot.len = (uint16_t)json.size();
        memcpy(slot.json, json.data(), json.size() + 1);
        if (size_ < kCapacity) {
            ++size_;
        } else {
            head_ = (head_ + 1) % kCapacity;
        }
        ++next_seq_;
        ++writes_since_persist_;
    }
    // persistSeqIfDue() takes the mutex itself; run it after releasing ours.
    persistSeqIfDue(false);
}

std::vector<std::string> RamLogSink::entries_since(uint32_t since, size_t limit) const {
    return entries_since(since, limit, nullptr, nullptr, nullptr);
}

std::vector<std::string> RamLogSink::entries_since(uint32_t since, size_t limit,
                                                   const mcco::LogCategory* category,
                                                   const mcco::LogLevel* level,
                                                   uint32_t* dropped_out) const {
    Guard g(mutex_);
    if (dropped_out) *dropped_out = mcco::log_dropped_count(since, size_ == 0 ? 0 : ring_[head_].seq);
    std::vector<std::string> out;
    for (size_t i = 0; i < size_ && out.size() < limit; i++) {
        const Entry& e = ring_[(head_ + i) % kCapacity];
        if (e.seq <= since) continue;
        if (category && e.cat != *category) continue;
        if (level && e.level != *level) continue;
        out.emplace_back(e.json, e.len);
    }
    return out;
}

uint32_t RamLogSink::oldest_seq() const {
    Guard g(mutex_);
    return size_ == 0 ? 0 : ring_[head_].seq;
}

size_t RamLogSink::count() const {
    Guard g(mutex_);
    return size_;
}

void RamLogSink::restoreSeqFromConfig() {
    Guard g(mutex_);
    if (config_.logSeq() + 1 > next_seq_) next_seq_ = config_.logSeq() + 1;
}

void RamLogSink::persistSeqIfDue(bool force) {
    mutex_.lock();
    bool due = force || writes_since_persist_ >= 32 ||
               (clock_.millis() - last_persist_ms_) >= 60000;
    uint32_t seq = next_seq_;
    mutex_.unlock();
    if (!due) return;
    if (config_.setLogSeq(seq)) {
        mutex_.lock();
        writes_since_persist_ = 0;
        last_persist_ms_ = clock_.millis();
        mutex_.unlock();
    }
}
