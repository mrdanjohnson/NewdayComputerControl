#include "log_sink.h"
#include <stdio.h>
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

        Entry& slot = ring_[(head_ + size_) % kCapacity];
        slot.seq = next_seq_;
        slot.json = std::move(json);
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
    Guard g(mutex_);
    std::vector<std::string> out;
    for (size_t i = 0; i < size_ && out.size() < limit; i++) {
        const Entry& e = ring_[(head_ + i) % kCapacity];
        if (e.seq > since) out.push_back(e.json);
    }
    return out;
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
