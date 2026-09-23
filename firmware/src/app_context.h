#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include "mc_mutex.h"

namespace mcco {
class IClock;
class IRandom;
class Ledger;
class CommandEngine;
class KeyStore;
class RateLimiter;
class MacroStore;
class PairingStore;
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
class TriggerStore;
class WebUi;
class AgentLink;

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
    mcco::MacroStore* macros = nullptr;     // Phase 2 (spec ch. 10)
    mcco::PairingStore* pairing = nullptr;  // Phase 4 (spec 3.2)
    AgentLink* agent_link = nullptr;        // Phase 4 (spec 4.2/4.3)
    TriggerStore* triggers = nullptr;       // Phase 2 (spec 10.2)
    WebUi* web_ui = nullptr;                // Phase 2 (spec 13.1.1, ch. 14)
    HidKeyboard* hid = nullptr;
    CommandDispatcher* dispatcher = nullptr;
    StatusCache* status_cache = nullptr;
    MdnsService* mdns = nullptr;
    WifiMgr* wifi = nullptr;

    // Set when the macro store changed and needs persisting (flash wear bound).
    std::atomic<bool> macros_dirty{false};
    // Set when trigger bindings changed and need persisting.
    std::atomic<bool> triggers_dirty{false};
    // Sticky flag (spec 15.1): set at boot when MacroStore::load() skipped any
    // corrupt persisted line; all macro CRUD and execute endpoints answer 409
    // store_corrupt until a successful persistence write clears it.
    std::atomic<bool> store_corrupt{false};

    // Serializes every KeyStore/Ledger/CommandEngine access (HTTP task,
    // dispatcher task, CLI). Ledger storage has its own internal mutex.
    Mutex engine_mutex;

    // Set by the HTTP layer when a key's last_used_at changed; drained and
    // persisted by the main loop (bounds flash wear).
    std::atomic<bool> keys_dirty{false};

    // Set when the pairing state changed (completed/revoked); the main loop
    // persists the record, flips the engine mode and updates the mDNS TXT.
    std::atomic<bool> pairing_dirty{false};
};
