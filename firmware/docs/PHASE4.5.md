# MacControl — Phase 4.5 (MCA telemetry completion: the dynamic half of the §6.3 report)

Status: **planned** (scoped 2026-09-23 from the feedback-gap survey; runs
before Phase 5). No acceptance-test gate of its own: exit = the verification
list below plus AT-06/AT-11 staying green. The gate that matters (AT-07)
depends on item A5 and the folded-in honesty fix.

## Why

The 2026-09-23 gap survey (see conversation / HANDOFF watch items) found
that the MCA → endpoint feedback channel is solid for liveness and
allowlisted-app state, but the PRD's composite MCA report (§6.3, served at
`GET /api/v1/agent/status`) is skeletal: its `system_info` section —
cpu/memory/disk/network samples, true uptime, macOS version, hardware
model, Mac IP — is specified but unimplemented on **both** sides, and
sleep/wake is undetected (system_state is always "awake" while the agent
runs). Concretely, today the endpoint cannot distinguish an unexpected
sleep from a crash, and cannot report anything about Mac load or version.

## Scope A — spec §6.3 completion (both sides)

- **A1. Dynamic load samples** — agent: extend the heartbeat payload (or a
  sampling event at most once per `heartbeat_interval`, per §6.3.1) with
  `cpu_utilization_pct`, `memory_utilization_pct`, `disk_free_bytes`,
  `network` reachability; endpoint: new `AgentStatus` fields (static
  structs, no heap growth), `mc_agent_events` payload validation,
  `applyEvidence` storage, `/api/v1/agent/status` renders the
  `system_info` section. Samplers: `psutil` if present, else `host_statistics`/
  `vm_stat`/`df` subprocess fallbacks (same best-effort policy as
  AppMonitor).
- **A2. True Mac uptime** — the agent already reads `kern.boottime` for
  `boot_id` and discards the value; send it (`boot_time`) plus derived
  `uptime_s` (Mac, not agent-process — the current heartbeat `uptime_s` is
  agent uptime and is mislabeled for the report). Endpoint stores
  `boot_time`; report uses Mac uptime.
- **A3. macOS version + hardware model** — `os_version` already arrives in
  the `capability_report` and is validated then dropped; store and surface
  it. `hardware_model` is new: `sysctl -n hw.model` probe in the agent,
  added to the capability_report payload (update `payload_keys_exact`
  validation).
- **A4. Mac network `{reachable, ip}`** — agent probe (default-route
  interface via `route get default`, address via `ipconfig getifaddr`,
  reachability via a zero-cost routing-table check — no outbound traffic,
  per the no-probe-supervisor lesson); endpoint stores and surfaces.
- **A5. Sleep/wake detection** — NSWorkspace notifications (pyobjc;
  `NSWorkspaceWillSleepNotification` / `NSWorkspaceDidWakeNotification`) →
  `system_state_changed(sleeping|waking|awake)` deltas, plus
  `declare_expected_offline(sleep)` via the existing goodbye path so a
  commanded/declared sleep reads as `expected_offline`, not fault. Without
  pyobjc the behavior stays as today (always "awake") and the gap remains
  documented. **This unblocks AT-07's sibling concern**: AT-07 itself also
  needs `screen_lock_changed`, which requires pyobjc installed on the bench
  Mac — install `pyobjc-framework-Quartz` (+ AppKit) in `agent/.venv` as
  part of this phase.

**Folded-in correctness fix** (HANDOFF watch item): `/api/v1/agent/status`
currently renders `screen_locked: false` / `user.logged_in: false` when the
evidence is absent — its own comment says "absent fields are null, never
invented" (§9 honesty). Render `null` when `has_lock`/`has_user` are false.

## Scope B — spec amendment candidates (decide accept/defer per item)

Each requires amendment-note text in `mc_openapi.cpp` and, where the PRD
closes the catalog, a spec amendment — they are **not** silently buildable:

- **B1. Foreground app** — `front_app_changed` event + report field
  (frontmost bundle ID via NSWorkspace; window title likely excluded — see
  B2's privacy reasoning). Small, useful for "what is the Mac doing"
  dashboards. Recommend **accept**.
- **B2. Full process inventory** — non-allowlisted apps in the report.
  PRD §11 explicitly forbids this; it expands the evidence/privacy surface
  and the RAM cost of the `applications` map on a 320 KB part. Recommend
  **defer** (revisit with the Phase 6 monitored-app registry).
- **B3. Wi-Fi SSID / interface detail** — extends the `network` field
  beyond the spec's `{reachable, ip}` (SSID, BSSID-less, interface name).
  Low risk, handy on multi-AP benches. Recommend **accept** with a
  `network_detail` sub-object so the spec-shaped `network` stays intact.
- **B4. macOS Shortcuts inventory → macro-tab pre-fill** — see "Out of
  scope" below; the name-inventory half is a possible minor amendment, the
  auto-binding half is not feasible. Recommend **defer unless the UI
  workflow justifies it**.

## Out of scope

**Running macOS Shortcuts as commands is NOT part of Phase 4.5.** It would
add event type(s) (ch. 6 closed catalog) and command type(s) (ch. 12
closed action set), i.e. amend two normative closures — and arbitrary
shortcut execution is exactly the kind of open-ended action the PRD
excludes by construction (§16.3.1). If desired, it is a deliberate spec
amendment, not a phase item.

**What works today without any of that**: the Shortcuts app lets any
shortcut bind a global keyboard shortcut (shortcut → ⓘ details → *Add
Keyboard Shortcut*). The ESP32 is a USB HID keyboard, and macros are
keystroke sequences — so a macro that types the bound hotkey triggers the
shortcut from any Web UI / trigger / API client. Caveats: hotkeys only fire
while a user session is logged in (nothing fires at the login window), and
the hotkey must not conflict with the frontmost app's own bindings.

**On auto-populating the macro tab from a shortcuts inventory**: shortcut
*names* are enumerable (`shortcuts list`), but their assigned keyboard
shortcuts have no supported API — the Shortcuts app keeps them in
undocumented, TCC-protected storage, so scraping them is fragile and
version-fragile (Full Disk Access needed, breaks silently on macOS
updates). A working auto-fill is therefore not feasible; the feasible
variant is B4: the agent sends a name inventory, and the Web UI macro tab
offers "new macro from shortcut" that pre-fills the name and prompts for
the hotkey. Deferred unless wanted.

## Verification (exit criteria)

- `pio test -e native` green (new: heartbeat-payload validation, AgentStatus
  serialization of the new fields, honesty fix in the agent/status builder).
- `pio run` both envs clean.
- Hardware: `GET /api/v1/agent/status` shows live `system_info` (cpu/mem/
  disk/network samples moving, Mac uptime growing, macOS version, hardware
  model, Mac IP), `null` lock/user when unknown; manual sleep/wake while
  paired shows `sleeping` → offline onset classified `expected_offline` →
  `awake` with fresh uptime and unchanged-vs-changed `boot_id` as
  appropriate.
- Heap: no per-sample heap growth (`[heap]` line flat over a 10 min soak);
  new AgentStatus fields are static.
- Regression: AT-06 and AT-11 each green once on the Phase 4.5 binary.
- Optional: `scripts/at13.py` — report-completeness checker (asserts every
  §6.3 field present/non-null in Mode B with pyobjc installed, honest nulls
  without).

## Files touched (expected)

- Agent: `maccontrol_agent/events.py` (samplers, sleep/wake, boot_time),
  `protocol.py` (payload shapes, event catalog if B1 accepted),
  `__main__.py` (capability_report payload), `requirements.txt` (optional
  psutil/pyobjc).
- Firmware: `lib/maccontrol_core/mc_status.h` (AgentStatus fields),
  `mc_agent_events.cpp` (payload validation), `mc_agent_session.*` if
  heartbeat parsing lives there, `src/agent_link.cpp` (`applyEvidence`,
  `tick`), `src/http_api.cpp` (`/api/v1/agent/status` honesty fix +
  `system_info` rendering), `mc_openapi.cpp` (amendment note for whatever
  Scope B accepts + report schema).

## Deviations to record

Whatever Scope B accepts (plus the A2 `uptime_s` relabeling: heartbeat
`uptime_s` stays agent-uptime; the report's `uptime_s` is Mac-uptime —
document in the amendment note to avoid the same confusion recurring).
