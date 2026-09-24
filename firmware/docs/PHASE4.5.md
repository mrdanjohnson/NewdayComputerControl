# MacControl — Phase 4.5 (MCA telemetry completion: the dynamic half of the §6.3 report)

Status: **COMPLETE and hardware-verified 2026-09-23** (A1–A5, folded-in
honesty fix, and B1 accepted; B2/B3/B4 deferred). No acceptance-test gate of
its own: exit = the verification list below plus AT-06/AT-11 staying green —
all met (see the verification section). The gate that matters (AT-07) is
unblocked: pyobjc is installed in `agent/.venv` (its `screen_lock_changed`
needs it) and declared-offline windows are wired end-to-end.

## Implementation log (2026-09-23)

- **Wire contract (both sides, exact keys):** heartbeat now carries
  `{boot_id, uptime_s, boot_time, mac_uptime_s, cpu_utilization_pct,
  memory_utilization_pct, disk_free_bytes, network{reachable, ip}}`
  (load samples nullable; `uptime_s` stays AGENT uptime, `mac_uptime_s`
  is Mac uptime). `capability_report` gains required `hardware_model`.
  New 12th event `front_app_changed{bundle_id: string-or-null}` (B1).
  `agent_goodbye` payload unchanged — reasons sleep/restart/shutdown now
  also mark windowless confirming records with the spec 5.3.2
  expected-offline window (3/60 s) via new
  `CommandEngine::on_agent_declared_offline()`, so a declared sleep is not
  swept to `unconfirmed` at offline onset.
- **Agent** (`agent/maccontrol_agent/`): `SystemSampler` (psutil first,
  `top`/`vm_stat`+`sysctl hw.memsize`/`df -k` fallbacks, `route -n get
  default` + `ipconfig getifaddr` for network — zero outbound traffic,
  null on failure); `get_hardware_model()`; `power.py` `SleepWatcher`
  (NSWorkspace WillSleep/DidWake on a dedicated NSRunLoop thread, all
  effects bounced via `loop.call_soon_threadsafe`; will-sleep →
  `system_state_changed(sleeping)` + `declare_expected_offline("sleep")`;
  did-wake → `waking` then `awake` ~5 s later; no-op without pyobjc);
  frontmost-app delta on the 3 s monitor cadence; agent version 1.1.0.
  `agent/.venv` now has psutil 7.2.2 + pyobjc 12.2.2 (cp314 wheels) —
  this unblocks AT-07's `screen_lock_changed`.
- **Firmware:** `AgentStatus` system_info fields (POD + change-only string
  assignment — no per-heartbeat heap churn); validation in
  `mc_agent_events.cpp`; `applyEvidence` storage incl. Goodbye →
  declared-offline marking; `build_agent_system_info()` in `mc_status.cpp`
  (renders all-null when evidence absent); `/api/v1/agent/status` honesty
  fix (`logged_in`/`screen_locked` now `null`, never invented `false`) and
  the `system_info` section; OpenAPI amendment note (incl. the A2
  agent-uptime vs Mac-uptime relabeling and the B1 §11 amendment).
  114 → **119 native tests** (`TwelveTypesAllValidate` replaced
  `ElevenTypesAllValidate`; new heartbeat/capability/front_app validation,
  system_info rendering, declared-offline engine tests).
- **Known residual:** if the agent's `kern.boottime` probe itself fails it
  sends `boot_time`/`mac_uptime_s` as null and the strict-int firmware
  validation rejects the heartbeat (agent then reads stale — reverts to
  pre-4.5 behavior for that frame class). Probe failure is essentially
  impossible on macOS; noted rather than relaxed.
- Real OS sleep/wake was exercised end-to-end on the bench (2026-09-23,
  agent 1.1.2) and works: the pre-sleep declaration reaches the endpoint
  before the network drops and the wake converges to `awake`. Getting there
  required a watcher rewrite: NSWorkspace sleep notifications **never
  deliver** in a headless python process (verified — the endpoint saw plain
  silence, no deltas). The watcher now uses IOKit system power
  notifications via ctypes (`IORegisterForSystemPower` on a dedicated
  CFRunLoop thread; needs nothing installed), with the
  `sleeping`+goodbye frames transmitted before `IOAllowPowerChange` acks.
  A clock-divergence detector (Mac uptime counts sleep, process monotonic
  does not) is the wake safety net. Known minor residual: Power Nap dark
  wakes can double-report `waking`/`awake` (both paths fire); cosmetic,
  deferred to Phase 5.

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

## Verification (exit criteria) — ALL MET 2026-09-23

- `pio test -e native` green (new: heartbeat-payload validation, AgentStatus
  serialization of the new fields, honesty fix in the agent/status builder).
  **119/119.**
- `pio run` both envs clean. ✅
- Hardware: `GET /api/v1/agent/status` shows live `system_info` (cpu/mem/
  disk/network samples moving, Mac uptime growing, macOS version, hardware
  model, Mac IP) — ✅ verified live (cpu 36–39 %, mem 74 %, disk 59.6 GB,
  network `{reachable, ip}`, uptime_s 6.07 Ms and counting, os_version
  15.7.4, Mac16,9). Honest lock/user: with pyobjc installed the report
  carries real values; the null-when-absent path is unit-tested (`?` — the
  old invented-`false` behavior is gone). Manual sleep/wake while paired:
  ✅ verified end-to-end (agent 1.1.2) — `system_state_changed: sleeping` +
  `agent_goodbye(sleep)` reached the endpoint 26 s before offline onset
  (20:02:00 vs 20:02:41), the declared window armed `expected_offline`,
  wake returned `awake` with unchanged `boot_id` and continuous uptime.
  Known minor residual: Power Nap dark wakes can make the clock-divergence
  safety net emit a duplicate `waking`/`awake` pair alongside the IOKit
  path's (observed 5 state frames instead of 2 on the bench mini; state
  converges correctly, no honesty violation). Deferred to Phase 5 polish.
- Heap: no per-sample heap growth — ✅ 11 min soak, `[heap]` free
  73.5–74.3 KB (no trend), min/largest/stack HWMs constant, agent
  heartbeats + 10 s HTTP poll load throughout.
- Regression: AT-06 and AT-11 each green once on the Phase 4.5 binary. ✅
  AT-06 all green; AT-11 all green in both rounds (after the app-detection
  fix, see `docs/DEBUG-PHASE45-AT11.md` — a latent Phase 4 flaw the Phase 4
  gate got lucky on, fixed in agent 1.1.1).
- Optional: `scripts/at13.py` — NOT built (optional; the live checks above
  cover the same assertions).

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

## Addendum — independent power + USB link detection

The endpoint must survive Mac outages and *detect* them on its own. The S3
devkitc is powered from an **independent 5 V supply** (not the Mac), so it
stays alive — Wi-Fi up, HTTP up, ledger intact — while the Mac sleeps,
restarts, or powers off. That makes the ESP32's own USB device link to the
Mac an outage sensor no agent message can fake.

**Wiring** (unchanged landmines):

| Cable | Port | Role |
|---|---|---|
| 5 V pin | bench PSU | Endpoint power, independent of the Mac |
| OTG "usb" port | Mac | TinyUSB HID + the link-state sensor |
| "com" port (CH343) | Mac | console/flashing only — any serial open/close still reboots the board |

**What firmware does** (`src/usb_link.*`): hooks the Arduino core's
`USB.onEvent` (the core owns the `tud_*_cb` definitions, so weak-callback
override would collide) for STARTED/SUSPEND/RESUME/STOPPED, keeps a volatile
state word + counters in BSS, and drains transitions once per second into
the log ring (`usb_attached` / `usb_suspended` / `usb_resumed` /
`usb_detached`, plus one informational `usb_link` at boot).
`/api/v1/agent/status` gains a static `usb: {link, state, changed_at}`
section (`link: "up"` only when mounted — suspended means the host stopped
signaling, i.e. link down).

**The events alone are not enough — the PHY backstop is the real sensor**
(bench-verified 2026-09-24, full forensics `docs/DEBUG-PHASE45-USB-LINK.md`):
TinyUSB events and `tud_*()` flags never fire for a mid-session host
disconnect on the devkitc, and GOTGCTL.BSESVLD is strapped to the board
rail (useless). What the hardware does report is **DSTS.SUSPSTS** (USB OTG
base `0x60080000` +`0x808`, bit 0): the DWC core sets it ~3 ms after host
SOF signaling stops — covering sleep, power-off, and unplug alike. The 1 Hz
drain therefore reconciles tracked state against `SUSPSTS` +
`tud_mounted()` and adopts divergences after a **2-tick debounce**
(re-enumeration flickers the signals one tick apart). A host that sleeps,
powers off, or unplugs is indistinguishable at this layer by construction —
classification stays with the table below.

**Host-outage classification** (consumer-side, e.g. the Web UI):

| Observation | Classification |
|---|---|
| Declared `agent_goodbye` (sleep/restart/shutdown) | expected_offline |
| USB suspend + agent silence + same boot_id on wake | host slept |
| USB down + agent returns with a **new** boot_id | host restarted / power-cycled |
| Agent silence while USB link stays up | agent or network fault |

**Bench evidence (2026-09-24):** labeled-USB cable unplug ~10 s produced a
single debounced `usb_detached` → `usb_attached` pair with correct report
transitions (this doubles as the power-off simulation — identical PHY
signature; a full Mac power-off test was skipped by decision). A real sleep
declared itself on the agent channel 24 s *before* `usb_detached`, resumed
with the same `boot_id`, and classified `expected_offline`. AT-06 and
AT-11 re-ran green on the final binary.

**Landmine:** both USB ports feed the 5V rail — if the board is powered
through the cable being pulled, the board reboots (`boot.reset:
"poweron"`, fresh log ring) and *no* link event can exist. Check the boot
entry before believing an "invisible event"; keep the board powered via
the other cable or the bench PSU when exercising link detection.

Classic ESP32 (no USB device controller) reports `no_usb` honestly.
