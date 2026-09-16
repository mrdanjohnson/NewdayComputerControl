#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
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
    // `deadline_override_s` lets the dispatcher set the authoritative
    // per-macro deadline (timeout_ms/1000 + 5, spec 10.3.1); 0 keeps the
    // per-type default.
    bool complete_dispatch(const std::string& command_id, bool dispatch_ok,
                           uint32_t deadline_override_s = 0);

    // ---- Macro dispatch pipeline (spec 10.3.1) ------------------------------
    // Macros interpret *after* the `dispatched` revision, unlike power chords
    // which terminate immediately in complete_dispatch. All three follow the
    // same Mode A verdict rules (spec 5.2.2): the ESP32 alone appends terminal
    // revisions; `completed` remains unreachable without MCA evidence.
    //
    // fail_dispatch: terminal `failed` with the given error_code from an
    //   `accepted` (or `dispatched`) record — used for the 30 s dispatch-delay
    //   bound (error_code "dispatch_delayed", spec 10.3.1) and for macros whose
    //   definition vanished between accept and dequeue.
    bool fail_dispatch(const std::string& command_id, const char* error_code);
    // mark_dispatched: accepted -> dispatched only, with the authoritative
    //   deadline (dispatched_at + deadline_s, the per-macro timeout_ms/1000+5
    //   of spec 10.3.1). The terminal verdict follows interpretation.
    bool mark_dispatched(const std::string& command_id, uint32_t deadline_s);
    // terminate_mode_a: dispatched -> terminal. ok=true appends
    //   `unconfirmed`/`hid_only`; ok=false appends `failed`/`dispatch_error`
    //   (mirrors the private logic of complete_dispatch).
    bool terminate_mode_a(const std::string& command_id, bool ok);

    // Macro store hook (spec 10.3): the HTTP layer resolves macro_id against
    // the MacroStore before the engine persists a macro_execute submission;
    // an unknown id terminates pre-ledger as 404 not_found.
    struct MacroResolution {
        bool found = false;
        uint32_t timeout_ms = 10000;
    };
    void set_macro_resolver(std::function<MacroResolution(const std::string& macro_id)> r) {
        macro_resolver_ = std::move(r);
    }

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
    std::function<MacroResolution(const std::string&)> macro_resolver_;
    static constexpr uint64_t kCoalesceWindowS = 60; // spec 5.1.1
};

} // namespace mcco
