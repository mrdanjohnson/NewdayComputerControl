#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "mc_clock.h"
#include "mc_error.h"
#include "mc_ids.h"
#include "mc_ledger.h"
#include "mc_log.h"
#include "mc_random.h"
#include "mc_types.h"

namespace mcco {

struct Submission {
    CommandType type;
    std::string parameters_json = "{}";  // raw JSON object, schema-validated
    std::string idempotency_key;         // may be empty
    std::string requested_by;            // "apikey:<key_id>"
    std::string body_hash;               // sha256 over the normalized submission
};

struct SubmissionOutcome {
    bool ok = false;
    int http_status = 500;               // 202 new, 200 explicit-key replay
    ErrCode error = ErrCode::InternalError;
    CommandRecord record;
    bool replay = false;
    bool dispatch_pending = false;       // caller must queue HID dispatch
};

// Deterministic command pipeline (spec ch. 5, Mode A). The ESP32 alone owns
// this engine: every command-producing request is persisted in `accepted`
// before any HID dispatch, and only this engine assigns terminal verdicts.
class CommandEngine {
public:
    CommandEngine(Ledger& ledger, IClock& clock, IRandom& rng, ILog& log);

    // Load ledger and run boot reconciliation. Must finish within 2 s of boot
    // start (spec 5.1.1); the ledger scan is linear and small, so it does.
    bool init();
    bool initialized() const { return initialized_; }

    // Validate + dedup + persist. Never dispatches: on success the caller
    // receives dispatch_pending=true and must later call complete_dispatch().
    SubmissionOutcome submit(const Submission& sub);

    // Mode A dispatch completion: appends `dispatched`, then immediately the
    // terminal verdict — `unconfirmed`/`hid_only` on success (never
    // `completed` in Mode A), `failed`/`dispatch_error` on HID failure.
    bool complete_dispatch(const std::string& command_id, bool dispatch_ok);

    // Resolve non-terminal records after reboot (spec 5.1.1). Returns count.
    size_t reconcile_boot();

    const CommandRecord* get(const std::string& command_id) const;

    struct ListFilter {
        bool has_state = false;
        CommandState state = CommandState::Accepted;
        bool has_type = false;
        CommandType type = CommandType::Lock;
        bool has_since = false;
        uint64_t since = 0; // epoch seconds
    };
    std::vector<const CommandRecord*> list(const ListFilter& f) const;

private:
    static bool validate_parameters(CommandType t, const std::string& params_json);

    Ledger& ledger_;
    IClock& clock_;
    IRandom& rng_;
    ILog& log_;
    bool initialized_ = false;

    struct IdemEntry {
        std::string command_id;
        std::string body_hash;
        uint64_t accepted_at = 0;
    };
    std::map<std::string, IdemEntry> explicit_keys_;
    std::map<std::string, std::string> hash_by_command_; // derived-coalescing hashes
    static constexpr uint64_t kCoalesceWindowS = 60; // spec 5.1.1
};

} // namespace mcco
