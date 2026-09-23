#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "mc_agent_events.h"
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
    // `completed` without MCA evidence), `failed`/`dispatch_error` on HID
    // failure. Exception: app_launch/app_quit in Mode B stay `confirming`
    // after dispatch — the terminal verdict arrives via agent_event() or the
    // deadline sweep (spec 5.2.1). `deadline_override_s` lets the dispatcher
    // set the authoritative per-macro deadline (timeout_ms/1000 + 5, spec
    // 10.3.1); 0 keeps the per-type default.
    bool complete_dispatch(const std::string& command_id, bool dispatch_ok,
                           uint32_t deadline_override_s = 0);

    // ---- Mode B agent commands (spec 5.3.1 app predicates) -------------------
    // Pre-ledger gate for agent-dependent submissions, wired by the glue in
    // main.cpp (same hook pattern as set_macro_resolver: the gate reads
    // pairing/session/capability state and MUST NOT lock engine_mutex —
    // submit() runs under the caller's lock). Returns nullopt to accept,
    // or the deterministic pre-ledger 409 to reject (agent_not_paired,
    // agent_offline, command_disabled, app_not_allowlisted).
    void set_agent_gate(std::function<std::optional<ErrCode>(const Submission& sub)> g) {
        agent_gate_ = std::move(g);
    }

    // Current operating mode pinned onto new records as mode_at_accept
    // (spec 5.1.1). The glue flips it when the pairing state changes.
    void set_mode(char m) { mode_ = m; }
    char mode() const { return mode_; }

    // Correlate one admitted MCA frame with an in-flight agent command.
    // command_ack/command_result address a record by command_id;
    // application_started/application_exited match the newest confirming
    // app_launch/app_quit record on exact bundle_id. The app predicates
    // complete only on command_ack PLUS the matching application event
    // (either order); command_result(ok) alone never completes (spec 5.3.1).
    // Evidence pointing at a record not in `confirming` is ignored, never an
    // error (spec 6.1.1). Returns true if a ledger record advanced.
    bool agent_event(const AgentEvent& ev);

    // Terminate confirming records whose deadline has passed
    // (timed_out/deadline_exceeded, spec 5.2.1). Called periodically by the
    // glue. Returns the number of records advanced.
    size_t sweep_deadlines();

    // The evidence channel was lost (30 s of silence / session closed): every
    // confirming record without an expected-offline window terminates
    // unconfirmed/evidence_lost (spec 5.2.1). Returns the number advanced.
    size_t on_agent_offline();

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
    char mode_ = 'A'; // flipped by the glue when a pairing becomes active

    struct IdemEntry {
        std::string command_id;
        std::string body_hash;
        uint64_t accepted_at = 0;
    };
    std::map<std::string, IdemEntry> explicit_keys_;
    std::map<std::string, std::string> hash_by_command_; // derived-coalescing hashes
    std::function<MacroResolution(const std::string&)> macro_resolver_;
    std::function<std::optional<ErrCode>(const Submission&)> agent_gate_;
    // app predicate progress per in-flight command: {ack_seen, app_event_seen}
    std::map<std::string, std::pair<bool, bool>> agent_pred_;
    static constexpr uint64_t kCoalesceWindowS = 60; // spec 5.1.1
};

} // namespace mcco
