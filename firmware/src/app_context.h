#pragma once
#include <atomic>
#include <cstdint>
#include "mc_mutex.h"

namespace mcco {
class IClock;
class IRandom;
class Ledger;
class CommandEngine;
class KeyStore;
class RateLimiter;
} // namespace mcco

class ConfigStore;
class EspClock;
class EspRandom;
class RamLogSink;
class FsLedgerStorage;
class HidKeyboard;
class CommandDispatcher;
class StatusCache;
class MdnsService;
class WifiMgr;

// Shared wiring context for all firmware modules. main.cpp fills this in and
// hands a pointer to every subsystem.
struct AppContext {
    ConfigStore* config = nullptr;
    EspClock* clock = nullptr;
    EspRandom* rng = nullptr;
    RamLogSink* log = nullptr;
    FsLedgerStorage* ledger_storage = nullptr;
    mcco::Ledger* ledger = nullptr;
    mcco::CommandEngine* engine = nullptr;
    mcco::KeyStore* keys = nullptr;
    mcco::RateLimiter* limiter = nullptr;
    HidKeyboard* hid = nullptr;
    CommandDispatcher* dispatcher = nullptr;
    StatusCache* status_cache = nullptr;
    MdnsService* mdns = nullptr;
    WifiMgr* wifi = nullptr;

    // Serializes every KeyStore/Ledger/CommandEngine access (HTTP task,
    // dispatcher task, CLI). Ledger storage has its own internal mutex.
    Mutex engine_mutex;

    // Set by the HTTP layer when a key's last_used_at changed; drained and
    // persisted by the main loop (bounds flash wear).
    std::atomic<bool> keys_dirty{false};
};
