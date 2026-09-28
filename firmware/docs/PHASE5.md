# MacControl — Phase 5 (Verified lifecycle): foundation — engine predicates + glue

Status: **foundation COMPLETE, host-verified 2026-09-24** (144/144 native
tests, both device envs compile). **Not yet hardware-verified** — the first
flash waits for the rewired self-powered bench (5 V PSU task) + separate
target Mac; run AT-06/AT-11 regression first, then author/run AT-07/08/09
(see HANDOFF "Phase 5 kickoff"). AT script authoring is the next batch.

Scope delivered here (PRD Phase 4 "Verified lifecycle", repo Phase 5): the
§5.3.1/§8/§10.3.1 verification predicates for power/lock/macro commands in
Mode B, §6 result-admission conformance pins, the epoch→millis interval
cleanup, the §7.2.2 `expected_offline` status provenance (AT-09's surface),
and the mc_http stack trim. Deterministic inputs untouched (heartbeat 5 s,
stale 15 s, offline 30 s, backoff 1/2/4/8/30, window 3/60 s, per-type
deadlines) — changing them would invalidate AT-06…AT-12 (PRD §17.2.1).

## Implementation log (2026-09-24)

- **Engine** (`lib/maccontrol_core/mc_engine.{h,cpp}`):
  - Mode B dispatch of **any** command type now stays `confirming` after
    successful dispatch (was app_launch/app_quit only). Mode A is byte-for-
    byte unchanged (`unconfirmed`/`hid_only` immediately; AT-05 still pins
    the all-false/L1 surface).
  - `PredProgress` per-record predicate state replaces the
    `pair<bool,bool>` ack/app map; `agent_event()` now also accepts the
    ambient `Hello` / `SystemStateChanged` / `ScreenLockChanged` frames.
  - Predicates: **lock** ← `screen_lock_changed{locked:true}` (15 s
    deadline) → `lock_confirmed`. **wake** ← new-session `Hello` then
    `system_state_changed{awake}` (a heartbeat alone never completes) →
    `wake_confirmed`. **sleep** ← offline evidence + silence through window
    close (`sweep_windows(channel_offline)` from the dispatcher 1 s loop) →
    `sleep_confirmed`. **restart** ← windowed offline evidence + `Hello`
    with a *changed* `boot_id` + awake burst → `restart_confirmed`;
    unchanged boot_id voids phase 1 (stays confirming → `timed_out`,
    §8.2.1). **shutdown** ← windowed offline + 3 ICMP failures →
    `shutdown_confirmed`; any probe reply → `host_still_reachable`; any
    `Hello` while confirming → `unexpected_reconnect`. Reconnect before
    close on sleep → `unexpected_wake`. Post-terminal contradictory
    evidence logs `unexpected_wake`/`unexpected_reconnect` and never alters
    the record (§5.3.2).
  - New API: `on_agent_hello(boot_id, had_boot, prev_boot_id)`,
    `set_known_boot_id()`, `sweep_windows(bool)`, `on_shutdown_probes(bool)`,
    `macro_interpret_done()` (replaces the macro success-path
    `terminate_mode_a`). Restart's baseline boot_id is cached at dispatch
    (`boot_id_at_dispatch`), falling back to the glue-known id.
  - **Monotonic sidecar**: all interval arithmetic (deadline sweep, 60 s
    coalescing, window open/close) moved off `epoch_seconds()` onto a
    `command_id → MonoDeadline` RAM map seeded from `IClock::millis()` —
    SNTP sync jumps can no longer insta-expire in-flight records. Epoch
    fields stay for the wire. `reconcile_boot()` now *resumes* confirming
    wake/sleep/restart/shutdown records whose deadline is still open
    (§5.1.1 "resumable"), reseeding their sidecar; other non-terminal
    records still end `failed/esp32_restarted`.
  - **Conformance pins** (the lost-gate-run scenario): late
    `command_result` after `timed_out` is ignored; a duplicate result over
    `completed` is ignored; duplicate `event_id`s dedup in `evidence` while
    confirming.
  - **Macros** (§10.3.1): `mc_macro` already validated `expected_event`
    (closed to the 5 ambient types; anything else 400 `macro_invalid_step`
    at macro create/update — no schema change needed). In Mode B a macro
    with `expected_event` stays confirming after interpretation and
    completes on a matching ambient event within timeout+5 s →
    `macro_confirmed`; without `expected_event` (or in Mode A) macros keep
    the honest `unconfirmed`/`hid_only`. Match rule: event type equality +
    every `match` key equals the payload value; the expected_event is
    resolved through the existing `macro_resolver_` hook (no new RAM map).
- **Glue** (`src/`):
  - `command_dispatcher.cpp`: 1 s loop calls
    `sweep_windows(!agent_link->sessionActive())`; wake dispatch calls
    `agent_link->requestClose(1000)` first (§8.1.2 — forces a fresh
    hello+burst when the Mac is already awake), then the HID chord.
  - Agent gate (main.cpp): lock/sleep/restart/shutdown require a live
    session in Mode B (`409 agent_offline` pre-ledger, same pattern as app
    commands). **wake is exempt** — at wake time the Mac is asleep and the
    session is necessarily dead; wake only requires paired. Mode A power
    commands still dispatch over HID with no gate.
  - `agent_link.cpp`: every admitted `Hello` routes to
    `engine->on_agent_hello(...)` under engine_mutex with the pre-overwrite
    `boot_id` from the cache; qualifying goodbyes set
    `AgentStatus::declared_offline_until = now + 60`, cleared on hello and
    on teardown; `AgentStatus::peer_ip` captured from `remoteIP()` on both
    the WS session and the polling ingress.
  - `power_probe.{h,cpp}` (new): lwIP raw-pcb ICMP echo (id "MC",
    seq-matched, 2 s reply timeout), 3 rounds at 5 s intervals started when
    a shutdown record is confirming + windowed + offline past close; each
    round feeds `engine->on_shutdown_probes()`. Static state only, driven
    from the dispatcher tick — no new task, no heap. Raw ICMP compiled and
    worked on the first pass; the TCP-connect fallback was NOT needed (kept
    as documented fallback in the OpenAPI amendment note).
  - **AT-09 provenance** (`mc_status.cpp`): while a declared window is
    open, agent-derived tuples render `freshness: expected_offline`
    (overriding the 15 s stale mark), `connection.agent` renders
    `false` + `expected_offline`, and `mac.boot_id` keeps its value with
    `expected_offline` freshness (§7.2.2 worked example 3). Window opens at
    the goodbye (onset), not at dispatch — AT-09 must sample after the
    goodbye lands. After expiry, normal stale aging resumes.
  - **Capabilities**: wake/sleep/restart/shutdown/lock/macro_execute
    `verified = paired && connected` (collapses with the 30 s offline
    threshold automatically); `capability_level` L1 unpaired / L2 paired /
    L3 paired+connected (PRD §1.3.3). Mode A output byte-identical.
  - `nvs_config`: last agent `boot_id` persisted to NVS (change-only,
    60 s-quantized writes) and hydrated at boot into the status cache +
    `engine->set_known_boot_id()` — the restart comparison survives an
    ESP32 reboot mid-window (§8.2.1).
  - `http_api.cpp`: mc_http stack 10240 → 7168 words (28 KB; HWM was 21 KB
    under AT load). The 30 s `[heap]` diagnostic stays until the Phase 5
    soak decides its retirement (HANDOFF).
- **Tests**: 119 → **144 native** (+25 across test_engine_agent: late/
  duplicate results, evidence dedup, lock/sleep/restart/shutdown/wake
  predicates incl. negative paths, macro expected_event confirm/timeout;
  test_smoke: expected_offline provenance + capabilities L3/verified).
  `ModeBPowerCommandStillNeverCompletes` rewritten to pin the new Mode B
  confirming path (the intentional Phase 5 behavior change).

## Behavior decisions where the spec was ambiguous

- A qualifying **goodbye sets offline evidence unconditionally** (it is
  self-attributing); the 3 s `open_after_s` floor kills only
  *silence-detected* onsets (§8's pre-window rule is about ambiguous
  silence, and a real fast sleep would otherwise always die).
- A changed-boot `Hello` also sets offline evidence for restart (a new boot
  proves the host went down even if the hello beats the 30 s silence
  detector).
- Sleep reconnect **at/after close** while still confirming: the hello is
  ignored and `sweep_windows` owns the verdict at `close_after_s`.
- Unknown restart baseline (never saw a boot_id): a hello voids phase 1 —
  the honest reading of §8.2.1's "identity-unchanged reconnect voids".

## Acceptance status (live)

- **AT-07 verified lock — PASSED 2026-09-27**, both consecutive rounds on
  one boot: `completed`/`lock_confirmed` 5.2 s and 2.6 s into the 15 s
  deadline, agent-observed `screen_lock_changed`, L3/verified preconditions
  pinned, human unlock between rounds (permanent in-the-loop step).
- **Sleep chain validated 2026-09-27 (rehearsal):** a live confirming sleep
  record completed `sleep_confirmed` at window close when the Mac was put
  to sleep by `pmset sleepnow` — window logic, declared-offline evidence,
  and the 90 s budget all correct on hardware.
- **Deviation (spec amendment, OpenAPI note updated):** HID cannot perform
  system sleep on modern macOS (display-sleep chord; System Control report
  ignored) — power actions are agent-executed in Mode B (protocol v2,
  goodbye-before-act), HID fallback only in Mode A. Wake/lock stay HID.
- **AT-09 expected-offline provenance — PASSED 2026-09-27.** Five
  consecutive chains on the v2 firmware, all `completed`/`sleep_confirmed`
  within the 90 s budget; final chain witnessed live from a second
  computer: in-window sample (+19 s, past the 15 s mark) showed
  `expected_offline` on every agent tuple with `connection.agent`
  false/`expected_offline` and `boot_id` retained; post-window sample
  (+89 s) showed `stale` with `boot_id` retained + `fresh`. Human-in-the-
  loop protocol (at09 `--phase {dispatch,verify}`) worked as designed.
  **Bench requirement discovered:** `sudo pmset -a powernap 0` AND
  `sudo pmset -a tcpkeepalive 0` — Power Nap/wake-for-network dark wakes
  otherwise keep agent heartbeats flowing during sleep and freshness
  never degrades (the device is *correct* either way; the AT assumption
  of a dead-silent host is what breaks). Also fixed: the post-hoc
  deadline bound in at08/at09 now matches `state_transition` log events
  only (post-terminal evidence notes like `unexpected_wake` carry the
  command_id and had inflated the bound past the deadline).

## Verification status

- `./.venv/bin/pio test -e native`: **144/144 green.**
- `pio run -e esp32-s3-devkitc-1` and `-e esp32-wroom-32`: both compile.
- **Flashed to the bench S3 on 2026-09-27 and verified on hardware**
  (UART upload, app partition only — NVS/LittleFS pairing/keys intact).
  Boot clean (`reset: poweron`), reconciliation 901 ms / 148 records,
  HTTP + auth + Web UI serving. With the paired MCA connected:
  capabilities render **mode B / L3 with all eight commands
  available+verified**, agent/status live with boot_id, status tuples
  fresh. Only item not hardware-verified: mc_http stack HWM after the
  40→28 KB trim — the `[heap]` ring entries store empty detail by design,
  so it needs a serial soak (sanctioned session) to read. Agent-side
  reconnect debugging surfaced a bench rename gotcha (stale
  `mac-b53478.local`), not a firmware fault —
  `docs/DEBUG-MDNS-STALE-NAME.md`. AT-06/AT-11 regression on this binary
  is owed before AT-07/08/09 authoring.

## What remains for Phase 5 (next batches)

1. **AT-07/08/09 scripts — AUTHORED 2026-09-27** (`scripts/at07.py`,
   `at08.py`, `at09.py`, at11 idioms, two-consecutive-rounds each,
   py_compile/`--help` verified, NOT yet run against hardware). Remaining:
   the topology decision (see HANDOFF 2026-09-27: the HID cable attaches to
   the harness Mac itself, so sleep/restart ATs need either a second Mac or
   a post-hoc + `pmset` scheduled-wake test mode), then the AT-06/AT-11
   regression on the Phase 5 binary, then the live runs.
2. **Agent batch decision**: the PRD predicates are device-observed, so
   AT-07/08/09 pass without agent changes; the optional hardening items are
   the pending-command mark (commanded vs user sleep), dead
   `reason=restart/shutdown` goodbye paths, and agent-side power actions.
3. Re-run full regression (AT-01…AT-06, AT-11) ×2 consecutive on the final
   Phase 5 binary before declaring Phase 5.
4. Soaks: watch `[heap]` (mc_http HWM ≥ 4 KB after the stack trim) and
   decide the diagnostic's retirement.
