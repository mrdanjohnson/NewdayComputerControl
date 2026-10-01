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

    // Dispatch completion: appends `dispatched`, then either the immediate
    // terminal verdict or — in Mode B — `confirming`. Mode A: `unconfirmed`/
    // `hid_only` on success (never `completed` without MCA evidence),
    // `failed`/`dispatch_error` on HID failure. Mode B: ALL command types
    // stay `confirming` after successful dispatch (spec 5.2.1, PRD §17.2.1);
    // the terminal verdict arrives via agent_event()/on_agent_hello()/
    // sweep_windows()/on_shutdown_probes() or the deadline sweep.
    // `deadline_override_s` lets the dispatcher set the authoritative
    // per-macro deadline (timeout_ms/1000 + 5, spec 10.3.1); 0 keeps the
    // per-type default.
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
    // app_launch/app_quit record on exact bundle_id; the five ambient types
    // (agent_hello, system_state_changed, user_session_changed,
    // screen_lock_changed, application_*) feed the spec 5.3.1/§8 power/lock/
    // wake and macro expected_event predicates. The app predicates complete
    // only on command_ack PLUS the matching application event (either order);
    // command_result(ok) alone never completes (spec 5.3.1). Evidence pointing
    // at a record not in `confirming` is ignored, never an error (spec 6.1.1).
    // Returns true if a ledger record advanced.
    bool agent_event(const AgentEvent& ev);

    // A new MCA session announced itself (agent_hello). `prev_boot_id` is the
    // boot_id the glue held before this hello (empty/`had_boot=false` when no
    // baseline exists); the engine falls back to its own cached copy for the
    // restart identity comparison. Drives the wake/restart/sleep/shutdown
    // reconnect predicates (spec §8). Returns the number of records advanced.
    size_t on_agent_hello(const char* boot_id, bool had_boot, const char* prev_boot_id);

    // Seed the engine's cached agent boot_id (glue hydrates from NVS at boot
    // so the restart comparison survives an ESP32 reboot mid-window, §8.2.1).
    // Updated automatically by on_agent_hello()/agent_hello frames.
    void set_known_boot_id(const char* boot_id);

    // Terminate confirming records whose deadline has passed
    // (timed_out/deadline_exceeded, spec 5.2.1). Called periodically by the
    // glue. Returns the number of records advanced.
    size_t sweep_deadlines();

    // Evaluate the spec 5.3.2 expected-offline windows of confirming
    // sleep/restart/shutdown records against the monotonic clock: an offline
    // onset inside [open, close] held through close completes sleep
    // (sleep_confirmed). `channel_offline` is the glue's current liveness
    // verdict, a backstop for silence onsets the offline notification missed.
    // Returns the number of records advanced.
    size_t sweep_windows(bool channel_offline);

    // The evidence channel was lost (30 s of silence / session closed): every
    // confirming record without an expected-offline window terminates
    // unconfirmed/evidence_lost (spec 5.2.1); windowed records absorb the
    // onset — inside the window it is positive evidence, before open it is a
    // pre-existing channel fault (§8). Returns the number advanced.
    size_t on_agent_offline();

    // The agent declared a sleep/restart/shutdown via agent_goodbye: every
    // confirming record gets the spec 5.3.2 expected-offline window (3/60 s),
    // so a subsequent on_agent_offline() sweep spares it; windowed power
    // records absorb the declared onset as positive offline evidence (inside
    // the window) or evidence_lost (before open). Returns the number of
    // records revised.
    size_t on_agent_declared_offline();

    // Corroborating ICMP probe result for a confirming shutdown record
    // (spec 8.2.2, driven by the glue's probe state machine): any reachable
    // reply fails the command host_still_reachable; the third consecutive
    // failure completes it shutdown_confirmed. Returns the number advanced.
    size_t on_shutdown_probes(bool any_reachable);

    // ---- OTA apply (spec 15.3) ----------------------------------------------
    // Number of records whose latest revision is non-terminal. `apply` uses
    // this for the 409 ota_in_progress gate while commands are in flight.
    // Const; the caller holds engine_mutex.
    size_t count_non_terminal() const;
    // apply {force:true} termination (spec 15.3): EVERY non-terminal record
    // is terminated failed/esp32_restarted BEFORE the reboot, reusing the
    // exact verdict reconcile_boot() assigns non-resumable in-flight records —
    // so the post-update ledger is identical to what boot reconciliation
    // would have produced, and no command is silently abandoned by the
    // firmware swap. The caller holds engine_mutex. Returns records advanced.
    size_t terminate_all_non_terminal();

    // Glue support for the spec 8.2.2 corroboration probe (src/power_probe):
    // true while the probe phase is open — at least one shutdown record is
    // confirming with an expected-offline window whose close boundary has
    // passed on the monotonic clock and the evidence channel is offline.
    // Const; the caller holds engine_mutex.
    bool shutdown_probe_open(bool channel_offline) const;

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
    // Macro interpretation finished without abort: in Mode B, a macro whose
    // definition declares an expected_event (resolved via the macro_resolver
    // hook) stays `confirming` until a matching ambient event completes it
    // (macro_confirmed) or the deadline sweep times it out; every other macro
    // gets the honest `unconfirmed`/`hid_only` (spec 10.3.1). Replaces
    // terminate_mode_a on the success path.
    bool macro_interpret_done(const std::string& command_id);

    // Macro store hook (spec 10.3): the HTTP layer resolves macro_id against
    // the MacroStore before the engine persists a macro_execute submission;
    // an unknown id terminates pre-ledger as 404 not_found.
    struct MacroResolution {
        bool found = false;
        uint32_t timeout_ms = 10000;
        bool has_expected_event = false;   // spec 10.1.1 Mode B verification
        std::string expected_event_type;   // one of the five ambient types
        std::string expected_match_json;   // exact-match object over payload keys
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

    // Predicate progress for one in-flight (confirming) command. Field set per
    // spec 5.3.1/§8; boot_id_at_dispatch caches the agent boot_id observed at
    // dispatch (restart identity comparison, §8.2.1), probe_fails counts
    // consecutive unreachable probe rounds (§8.2.2).
    struct PredProgress {
        bool ack = false;                 // command_ack seen (app predicates)
        bool app_event = false;           // matching application_* seen
        bool hello = false;               // qualifying post-dispatch hello seen
        bool offline_evidence = false;    // windowed offline onset absorbed
        bool declared = false;            // onset came from a declared goodbye
        uint8_t probe_fails = 0;          // shutdown probe counter
        std::string boot_id_at_dispatch;  // restart baseline (empty = unknown)
    };

    // Monotonic-clock sidecar for interval arithmetic (spec: intervals on
    // IClock::millis(); the ledger's epoch fields stay the wire format and go
    // garbage across the SNTP sync jump). Entries are seeded at accept /
    // dispatch and erased alongside agent_pred_ on terminal transitions.
    struct MonoDeadline {
        uint64_t accepted_mono_ms = 0;
        uint64_t dispatched_mono_ms = 0;
        uint64_t deadline_mono_ms = 0;
    };

    void drop_side(const std::string& command_id) {
        agent_pred_.erase(command_id);
        mono_.erase(command_id);
    }
    size_t handle_hello(const char* boot_id, bool had_boot, const char* prev_boot_id,
                        const char* event_id);
    bool match_macro_event(const AgentEvent& ev);

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
    std::map<std::string, PredProgress> agent_pred_;
    std::map<std::string, MonoDeadline> mono_;
    std::string known_boot_id_; // last agent boot_id (NVS-backed by the glue)
    static constexpr uint64_t kCoalesceWindowS = 60; // spec 5.1.1
};

} // namespace mcco
