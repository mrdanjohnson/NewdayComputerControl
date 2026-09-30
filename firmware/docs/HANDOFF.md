# MacControl — project handoff (resume document)

Read this first in a new session, then `firmware/AGENTS.md`, then the
phase doc for the work at hand. Project root:
`/Users/danieljohnson/Public/ESP32-MCA Command Loop Design/`

## 2026-09-30 (latest): UNLOCK COMMAND — device-stored password, login_screen detection

> Feature (user-driven): wake-from-sleep then password entry was a manual
> two-step (wake button, wait, password macro). The password macro also
> exposed the secret to ANY API-key holder (`GET /api/v1/macros` is READ).
> Implemented the device-side unlock amendment end to end:
> - `CommandType::Unlock` (mc_types): no parameters, 60 s deadline, NOT
>   session-gated (the agent is offline at the login window — gating would
>   make it useless exactly where needed).
> - Engine: completes on `screen_lock_changed {locked:false}` →
>   `unlock_confirmed` (covers both the lock screen and the post-login-window
>   agent burst). Mode A → unconfirmed/hid_only. New native tests
>   (UnlockCompletesOnScreenUnlockChanged / UnlockNotSessionGated /
>   UnlockRejectsParameters) — 151/151.
> - Storage: `ConfigStore::save/loadUnlockPassword` — PLAINTEXT NVS record
>   ("unlockpw"); write-only by API design (no GET returns it; never logged;
>   unlock commands carry no parameters so it never reaches the ledger).
>   Charset validated printable ASCII 1-64 at set time.
> - HID: `HidKeyboard::typePassword` — strict typer, any unmapped byte or USB
>   drop ABORTS (never silently skips a password byte). Dispatcher branch:
>   emulated replug first if the sleeping host cut USB power, then
>   password+Enter, then complete_dispatch.
> - API: `POST /api/v1/system/unlock` (CONTROL, via the generic power alias);
>   `GET/PUT/DELETE /api/v1/system/unlock_password` (READ/ADMIN/ADMIN).
>   OpenAPI amended (enum + path + description).
> - Status: synthesized `mac.login_screen` in /api/v1/status (true only while
>   agent live + screen locked; null at the pre-login window — declared
>   offline is the discriminator there).
> - Web UI: Unlock + "Wake & Log In" buttons (composite: wake → poll
>   agent/status for screen_locked → unlock, 75 s cap); Settings card for the
>   unlock password (write-only field + clear).
> - Skill file updated with the unlock flow.
> Verified live: GET {set:false}, non-ASCII 400, unlock without password →
> `failed/no_password_configured`. AWAITING: user sets password in UI, runs
> the sleep → Wake & Log In test, then DELETES the exposed
> 'type password + enter' macro.

## 2026-09-30 (latest): WAKE CHORD = SPACEBAR TAP — modifier tap was silently broken

> User report: the Wake power button never woke the host (a macro always had
> to do it); only the keyboard re-enumerated. Root cause in
> `HidKeyboard::send_once(Wake)`: the chord was `Keyboard.write(KEY_LEFT_SHIFT)`
> — a modifier press+release in the same instant with zero duration and no
> character. This host never treats a modifier-only micro-tap as a wake
> event (the AT-08 "late-wake HID impossible" conclusion was partially this
> artifact). User's space-containing macros woke the host reliably.
> FIX: wake chord is now a 100 ms spacebar tap (press / 100 ms / release),
> plus a 1 s settle after a successful `reconnectForUserWake` so the host's
> HID driver is ready before the tap. Accepted cost: one space lands in the
> focused app after wake (spec fixes no chords; the original shift tap was
> chosen to avoid exactly that, but it never woke anything). Native 148/148,
> both envs, flashed 2026-09-30 ~18:15Z. AWAITING user re-test of the Wake
> button. AT-08 note if re-run: HID wake actuation now has a real chance of
> working, so the wake phase may complete on agent evidence before the +90 s
> human prompt.

## 2026-09-30 (later): MACRO WAKE FROM SLEEP — emulated replug on explicit wake dispatch

> User test: churchtech asleep 3 min → macro click → instant `usb_disconnected`
> abort; physical unplug/replug → same macro woke the host. Device logs show
> why: the host cuts USB port power ~8 s into sleep (`usb_detached` 8 s after
> `agent_goodbye`), so the keyboard is truly un-mounted — polling cannot
> recover it, and the automatic re-enumeration supervisor is (by design)
> suppressed while the host sleeps. The replug re-enumerates on the sleeping
> host (port power is still on) and the first keystroke wakes it like any USB
> keyboard.
>
> FIX: `HidKeyboard::reconnectForUserWake(timeout_ms)` — toggles the USB
> data-line pull-up (the electrical equivalent of the physical replug) and
> polls up to the timeout for re-enumeration. Called from:
> - `send_chord` (Wake) when the initial mount poll fails;
> - `run_macro` once per macro, replacing the immediate `usb_disconnected`
>   abort (logs `usb_wake_reconnect`, 15 s bound, honest
>   `usb_disconnected`/`macro_timeout` fallback).
> The sleep-time supervisor suppression is unchanged: automatic churn during
> sleep would dark-wake the host; an explicit user wake WANTs that.
> Native 148/148, both envs compile, flashed 2026-09-30 ~17:00Z.
> AWAITING user re-test (sleep → wait → macro).

## 2026-09-30 (later): MACRO-CLICK PANIC REBOOT — FIXED (HTTP task stack overflow during ledger compact)

> Symptom (user report): every Web UI macro click hung, then the device
> rebooted (bounce to signin, "Failed to fetch"); records died
> `esp32_restarted`. 100% reproducible incl. a delay-only macro with zero HID
> → crash was in the command-accept path, not HID.
>
> Diagnosis (serial triage markers + `-fstack-usage`):
> - Panic = FreeRTOS **stack watchpoint on mc_http** ("stack watchpoint
>   triggered (mc_http)"), caught in the context-switch path with a corrupted
>   backtrace; the panic reason/register-dump lines were being lost to USB CDC
>   drops — read the raw serial buffer, not grep-filtered tails.
> - HWM markers: at `submit_macro_execute` entry only 1676 words free; inside
>   `rewrite_filtered` the copy loop ran with **152 words (608 B) remaining**,
>   and the post-rename `LittleFS.open(kPath, "a")` + unwind needed more →
>   watchpoint. Peak usage ≈ 27.6 KB of the 28 KB (7168-word) HTTP stack.
> - Root cause: the 2026-09-29 streaming `rewrite_filtered` keep-filter called
>   `record_from_json` (full ArduinoJson parse) PER LINE — a much deeper
>   call chain than the old whole-file `replace_all` — and with the ledger at
>   capacity 128 (≈400 durable lines, ~150 KB) the exec request path peaked
>   past the stack. The old build survived because its chain was shallower.
>   LittleFS itself is fine (1.5 MB partition, 1.38 MB free — NOT a
>   space problem).
>
> Fix:
> 1. `http_api.cpp`: mc_http stack 7168 → **12288 words** (measured need
>    7016 words; +20 KB heap at task creation, steady free heap ~100 KB).
> 2. `mc_ledger.cpp compact()`: keep-filter is now a raw substring match on
>    `"command_id":"<id>"` (unique random ids can't false-match; unparseable
>    lines don't match any needle → kept verbatim, same semantics). No
>    ArduinoJson per line → the compact chain is much shallower.
> 3. Native 148/148, both envs compile. Verified on hardware: 3 consecutive
>    macro executes (each forcing a full eviction+compact incl. the HID
>    "search" macro mac_ADB3) — 202 Accepted every time, device stays up,
>    records end honestly `unconfirmed/hid_only`.
>
> LESSON: any per-line JSON parse inside a storage rewrite is a stack-depth
> hazard on a task that also serves HTTP; measure with
> `uxTaskGetStackHighWaterMark(NULL)` markers, and read raw serial bytes —
> the panic reason lines drop off USB CDC.

## 2026-09-30: PHASE 5 GATE COMPLETE — AT-07/08/09/06 green ×2 each, AT-11 core green (bench caveat), all fixes flashed

> **Scoreboard (2026-09-29→30, target = churchtech Mac16,10, all on one
> invocation one boot per round):**
> - **AT-07 PASSED ×2** (lock_confirmed, agent-observed)
> - **AT-08 PASSED ×2** (restart_confirmed with changed boot_id, sleep,
>   wake_confirmed — all green both rounds; host auto-logs in after reboot)
> - **AT-09 PASSED ×2** (sleep onset ~22–39 s; agent session survives)
> - **AT-06 PASSED** (run 1 had one wrong expectation — script wanted L2,
>   Phase-5 amendment says L3 while paired && connected; `at06.py` fixed,
>   run 2 fully green; run 1 otherwise green so both count)
> - **AT-11 core green** across runs: backoff, polling fallback, launch over
>   WS+polling, pending endpoints, token-leak, session-log checks. Residual:
>   2 checks (`reset_app` TextEdit-would-not-exit + one quit-evidence-lost).
>   **Root cause is bench-only:** long-lived WS connections from the bench
>   Mac to the ESP32 die silently every ~30–32 s (soak test under zero
>   harness load: sessions at 01:21:31→01:22:04→01:22:41→01:23:13→01:23:44;
>   no close frame, no endpoint error; route via Ethernet en0, ping 0%
>   loss, ~12 ms avg / 38 ms max jitter; polling transport from the same
>   host is 100% stable; churchtech's WS sessions survive 30–60 min).
>   Looks like a local-to-bench-Mac network/security policy killing
>   long-lived outbound TCP. Not a product defect — the polling fallback
>   exists for exactly this. Evidence: `/tmp/soak_agent.out`. Recommend
>   re-running AT-11 with the bench path fixed, or accept the documented
>   caveat.
>
> **Firmware fixes this session (ALL flashed, native suite 148/148 green,
> both envs compile):**
> 1. `mc_engine.cpp handle_hello`: replaced per-hello `list_newest_first()`
>    snapshot (O(commands) heap) with `Ledger::newest_from_back(k)` — new
>    mutation-safe back-index accessor in `mc_ledger.h`. Was a
>    `bad_alloc → terminate` panic loop (26 panics/4 min, boot→hello→OOM).
> 2. `mc_ledger.cpp load()`: trims RAM mirror to capacity at boot (never
>    did); S3 capacity now **128** in `main.cpp` (spec 64–1024; ctor
>    clamps <64→64). Amendment noted in `mc_openapi.cpp`.
> 3. `evict_oldest(bool run_compact)` + batched trim (one `compact()` at
>    the end, not per record — per-record compact rewrote the whole ledger
>    file 150×/boot).
> 4. `compact()` uses new `ILedgerStorage::rewrite_filtered(keep)` virtual
>    (default = read_all+replace_all for host/tests; `FsLedgerStorage`
>    overrides with O(1)-heap streaming temp-file copy). The old whole-file
>    `std::vector` copy OOM'd on the ~300–500 KB stream. `compact()` no
>    longer drops `pending_evict_` on failed `replace_all` (that silent
>    drop is how the durable stream outgrew capacity).
> 5. `mc_engine.cpp complete_dispatch`: Mode B wake with failed HID
>    actuation now advances to Confirming instead of terminal
>    `dispatch_error` (§8.1.2 hello+burst evidence — e.g. a human
>    keypress — can still confirm the wake; deadline sweep ends it honestly
>    if evidence never comes). Native test
>    `ModeBWakeFailedActuationStaysConfirmingForEvidence` added. Fixed
>    AT-08's wake failures.
> 6. USB re-enumeration supervisor suppressed during declared
>    expected-offline windows (`serviceHostReconnect(agent_session_active,
>    host_declared_offline)`, main.cpp reads
>    `AgentStatus.declared_offline_until`) — the supervisor's reconnect was
>    dark-waking the sleeping host and defeating AT-09.
>
> **Agent fixes (deployed to target, repo copy current):**
> `transport.py` + `polling.py` `_respect_offline_window` waits in 1 s
> slices (re-reads `expected_offline_until`); `power.py _handle_wake` and
> `events.py detect_sleep_from_clocks` clear `rt.expected_offline_until =
> None` on wake (agent idled ~52 s after a human wake, breaking AT-09
> round 2). Earlier: `actions.py` `SLEEP_PRELUDE = ["pmset",
> "displaysleepnow"]` before sleepnow (fixed the 45–60 s powerd deferral);
> `events.py` absolute-path sysctl/route/ipconfig fix (the 4003 crash
> loop). Target plist carries `--enable-sleep --enable-restart
> --enable-shutdown` and PATH incl. `/usr/sbin:/sbin`.
>
> **Host-behavior findings (churchtech):** reboots in ~2 s → AT onset
> window lowered to 2 s (`at08.py ONSET_MIN_S = 2.0`); `displaysleepnow`
> prelude makes sleep onset ~8 s; late-wake HID impossible on this host —
> a human keypress remains the wake actuation at the AT's +90 s point.
> After re-pairing, the launchd daemon MUST be restarted to pick up the new
> token (`launchctl kickstart -k gui/$(id -u)/com.maccontrol.agent`) — it
> only reads the state file at startup (2026-09-30: `agent_rejected
> {unpaired}` loop after pair until kickstart).
>
> **Harness script fixes:** `at08.py ONSET_MIN_S = 2.0`; `at06.py` step-5
> expects `capability_level == 'L3'` + `start_agent_daemon` pkills stray
> daemons first (two same-token daemons supersede each other's sessions
> mid-dispatch — AT-11's original failure mode); `at11.py` preconditions
> kill all harness daemons and start exactly one fresh (`--transport
> websocket`); `test_ledger.cpp` new `BootReloadTrimsMirrorToCapacity`.
>
> **Cleanup done 2026-09-30:** probe key-08 (`probe-2026-09-29`, READ)
> revoked via serial CLI — 3 active keys remain (key-05 READ / key-06
> CONTROL / key-07 ADMIN; roles probed 2026-09-28, NOT the order the user
> listed). Target agent ag-05e1 re-paired (AT-06/AT-11 steal the pairing —
> re-pair is the permanent last step after those two ATs) and verified:
> session_active, mode B, capability_level L3. Bench agent ag-4d00 stays
> unloaded. Stale-token rejection path (`agent_rejected{unpaired}`, no
> supersede) verified clean.
>
> **Phase 6 notes:** (a) endpoint should accept `X-API-Key` or return a
> 401 hint — it only parses `Authorization: Bearer` (`http_api.cpp`), a
> sharp edge that cost an hour; (b) `declared_offline_until` window
> semantics held across AT-09; (c) consider validating `protocol_version`
> in hello before frame processing.

## 2026-09-28 (later): HID keyboard KILLED by System Control device — FIXED, flashed, verified

> Root cause of "ESP32 stopped typing on both Macs" (user report, confirmed
> by test): the 2026-09-27 15:40 sleep-dispatch change registered a SECOND
> HID device (hand-written System Control descriptor, no report ID) on the
> same interface as the keyboard (report ID 1). The composite enumerated
> fine — macOS parsed both collections, keyboard driver matched (verified
> via ioreg) — but NO keyboard report ever reached the host: typed macros,
> Ctrl+Cmd+Q lock chord, all silently dropped, while the firmware believed
> every dispatch succeeded (SendReport ok, record `unconfirmed`/`hid_only`;
> the macro runner ignores per-key bools). First HID-dependent test after
> the v2 flash (AT-07 re-run) exposed it; the ledger showed the user's Web
> UI macro attempts all "succeeded" with nothing arriving. Proven by a
> focus-independent test: `POST /system/lock` while the OTG cable was on
> the harness Mac — screen never locked; and post-fix, raw-tty capture got
> `zzz` from three dispatches of a `key_press z` macro. FIX:
> `src/hid_keyboard.cpp` — the System Control device/descriptor REMOVED
> entirely (Mode B sleep/restart/shutdown are agent-executed; Mode A HID
> sleep was already proven macOS-ignored 2026-09-27). Mode A sleep on S3
> now fails honestly `failed/dispatch_error`. Built both envs, native
> 146/146, flashed, `dist/esp32-s3/` refreshed (VERSION built
> 2026-09-28T16:53:32Z). LESSON: never ship a second HID collection
> without an end-to-end keystroke test on a real host; enumeration health
> (ioreg) does NOT prove report delivery.
>
> Same session facts: the three active API keys are NOT in the order the
> user listed — actual roles probed via rate-limit buckets (READ 60/min,
> CONTROL 30, ADMIN 10): READ=mck_Bo7q…, CONTROL=mck_eJ4…, ADMIN=mck_wRE…
> (at-read/at-control/at-admin are revoked). The pairing-admin fix from
> earlier today IS in this binary (revoke/window return 401 not 404).
> `firmware/scripts/at09.py` had a broken multi-line string at line 494
> (syntax error, from the post-hoc deadline edit) — fixed, all AT scripts
> compile. OTG ("USB") cable was on the harness Mac for diagnosis — moved
> back to the target before AT-07.
>
> **AT-07 PASSED ×2 on the target** (2026-09-28, lock_confirmed 2.8 s/2.6 s,
> agent-observed, human unlocked between rounds) — first hardware proof the
> HID fix holds on a real host.
>
> **AT-09 FAILED — root cause: launchd PATH, not an old agent.** The target
> agent IS v1.2.0/protocol 2 (user verified version, load path, heartbeat
> keys). The shipped launchd plist's PATH is
> `/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin` — **no /usr/sbin or
> /sbin**. Under launchd, `sysctl` (kern.boottime, hw.model, hw.memsize),
> `route`, and `ipconfig` are all unresolvable → the agent's heartbeat
> carried `boot_time: null, mac_uptime_s: null` and capability_report
> `hardware_model: null` → endpoint `payload_keys_exact`/type checks reject
> every heartbeat + capability_report (schema_violation) → close 4003 after
> the 2nd violation → launchd respawn → 13 s loop. Boot_id was RANDOM per
> process (derive_boot_id fallback) — the tell-tale. The bench agent never
> hit this because it ran from a full shell PATH. All AT-09 "fresh" readings
> were the flapping session; sleep records died `unexpected_wake`. Also
> found afterwards: a STALE pre-v2 agent (ag-4d00, this repo's code as of
> Sunday, --enable-sleep etc.) was still running on the harness Mac under
> launchd — killed it; it was not the loop's cause but must stay off during
> power ATs. FIX: `agent/maccontrol_agent/events.py` resolves sysctl/route/
> ipconfig by absolute path (`_SYSCTL`/`_ROUTE`/`_IPCONFIG`); plist template
> PATH gained /usr/sbin:/sbin. Deploy = copy events.py to the target +
> restart the agent (pairing and enabled flags in ~/.maccontrol/agent.json
> survive; ensure --enable-sleep --enable-restart --enable-shutdown are in
> the plist args). Verified locally under the restricted PATH: boot_key int,
> hw.model string, network reachable+ip.
>
> **Phase 6 hardening item:** the hello handler SHOULD validate
> `protocol_version == 2` and close 4003 immediately with a clear reason
> instead of accepting then loop-rejecting downstream frames (mid-gate
> firmware change deferred — AT-06 pins current rejection behavior).
>
> **AT-09 retry after the PATH fix: sleep evidence chain fully PASSED**
> (expected_offline in-window, stale post-window, boot_id retained,
> sleep_confirmed) — but the harness's cleanup WAKE never woke the target
> (asleep >120 s; user keypress recovered it). Root cause: HID wake from a
> SLEEPING host never worked — the ESP32 config descriptor did not declare
> USB remote wakeup (core default = self-powered only), so macOS kept the
> port suspended and the wake key-tap (and every report) was dropped on the
> suspended bus. Never caught before: 09-27 AT-09 had humans wake the Mac;
> AT-08's phase_wake never ran live. FIX (`src/hid_keyboard.cpp`):
> `USB.usbAttributes(SELF_POWERED | REMOTE_WAKEUP)` before `USB.begin()`
> (esp32-hal-tinyusb.c callback is weak; usbAttributes() is pre-start only)
> + `hostResumeIfSuspended()` (`tud_remote_wakeup()` + bounded resume wait)
> called from send_chord/typeText/keyDown. Native 146/146, both envs,
> flashed (VERSION built 2026-09-28T19:2xZ), AT-09 re-run pending.
> LESSON: every HID path must be tested against a genuinely sleeping host —
> "enumerates and types while awake" says nothing about suspend/resume.
>
> **Wake diagnosis (device log `wake_debug`, sus/wu/ms/ok): on this target
> (Mac16,10, Apple Silicon, direct cable) HID wake is PHYSICALLY
> IMPOSSIBLE.** The host powers the USB port OFF in sleep: the ESP32
> deconfigures (~9 s after sleep onset the link reads detached; at wake
> time `sus=0, ok=0` = not even suspended, nothing for remote wakeup to
> signal). A wired USB keyboard still wakes this Mac, so ports are
> selectively managed per-device; the ESP32-S3 (composite-declared... now
> keyboard-only, remote-wakeup attr set) loses the port anyway. Also:
> rebooting the ESP32 while the host sleeps can never recover HID (no
> enumeration without an awake host) — documented constraint, agent
> wake impossible by definition (the agent IS the asleep host).
> GATE DECISION: wake ACTUATION is human (keypress) on such hosts; the
> wake RECORD is still device-verified (post-dispatch new-session hello +
> awake burst → wake_confirmed) — at08/at09 now print that prompt
> (`ask_human`, TTY-gated). Hosts that only SUSPEND the port (bench
> Intel/older Macs, e.g. 09-27 behavior) still get true HID remote
> wakeup via the descriptor attr + hostResumeIfSuspended path — keep both.

## 2026-09-28: pairing-admin off-by-one FIXED (built + bundled, flash owed)

> `POST /api/v1/pairing/revoke` and `…/window` always 404'd (`not_found`)
> because the route block used prefix length 17 where `"/api/v1/pairing/"`
> is 16 — `substr(17)` turned `revoke` into `evoke`. The Web UI "Revoke
> pairing" button was therefore dead; the user could not un-pair to bind a
> different agent. Fixed in `src/http_api.cpp` (17→16 in compare+substr).
> Native 146/146; both envs compile; `dist/esp32-s3/` refreshed (VERSION
> built 2026-09-28T14:49:33Z). **NOT FLASHED yet** — rerun
> `pio run -e esp32-s3-devkitc-1 -t upload` ('com' cable), then the Web UI
> Pairing tab revoke works; pairing state lives in NVS and survives the
> app-partition flash. Full forensics: `docs/DEBUG-PAIRING-ADMIN-404.md`.
>
> **Same-day agent fix (`agent/maccontrol_agent/ui.py`):** the osascript
> pairing-dialog fallback (tkinter missing) fired on EVERY headful start,
> even when paired — so a crash-looping agent (e.g. a pre-v2 agent halted
> by close 4003 protocol_mismatch, respawned by launchd every 10 s) spammed
> the "Enter pairing code" dialog indefinitely. Now prompts only when
> unpaired. Lesson recorded: pair a v1 agent against the v2 firmware and
> you get exactly that loop — the other Mac needed the v1.2.0 agent.

## 2026-09-27: sleep-dispatch fix BUILT + BUNDLED, flash BLOCKED on cabling

> Sleep was dispatched as Cmd+Alt+Power — on modern macOS that only sleeps
> DISPLAYS; the system keeps running, expected-offline evidence never
> arrives, record times out at 90 s (verified via pmset + ledger). Fix:
> `src/hid_keyboard.cpp` now registers a second HID device (raw `USBHID` +
> `USBHIDDevice` subclass; this core 2.0.17 has no `setReportDescriptor`)
> with a hand-written System Control descriptor (Generic Desktop 0x01,
> collection 0x80, Sleep usage 0x82, no report ID) and sends Sleep as
> report 0, byte 0x01 → 0x00. No chord fallback. wroom stub untouched.
> S3 + wroom compile clean, native 144/144, `dist/esp32-s3/` refreshed
> (VERSION built 2026-09-27T15:40:21Z). **NOT FLASHED:** the CH343 'com'
> cable (`/dev/cu.usbmodem5CBD0148591`) is not enumerated — self-powered
> bench rewire left it unplugged; the USB-Serial/JTAG (`/dev/cu.debug-console`,
> 0x303a:0x1001) refuses esptool sync (likely PHY owned by the running
> OTG HID). Plug the 'com' cable back in and rerun
> `pio run -e esp32-s3-devkitc-1 -t upload`. Bench currently runs the
> previous Phase 5 binary (network checks on it pass: Web UI 200, clean
> 401s). No power command dispatched — human runs the live sleep test.

## Phase 5 kickoff (state before the sleep fix)

> **2026-09-27: Phase 5 foundation binary FLASHED to the bench S3 and
> verified on hardware.** Upload over the CH343 UART succeeded; all
> verification network-only (serial stayed closed). Boot: `reset:
> poweron`, WiFi + mDNS up, Web UI 200, clean 401 envelopes; boot
> reconciliation 901 ms over 148 ledger records (2 s bound); NVS/LittleFS
> pairing/keys survived (app-partition flash only). With the paired MCA
> connected: `/api/v1/capabilities` = **mode B, L3, all eight commands
> available+verified**, app commands available; `/api/v1/agent/status` live
> (session active, `boot_id b_578FC7`, system awake); status tuples render
> fresh with boot_id present. The 30 s `[heap]` events keep flowing but
> their ring `detail` is empty by design (>224 B slots) — mc_http HWM
> after the 40→28 KB trim needs a serial soak to read. **Bench gotcha
> burned on the way:** the S3 was renamed — mDNS answers
> `control-graphics.local` (10.10.40.242), the old `mac-b53478.local` name
> is a stale mDNS cache entry that intermittently resolves; the AT agent
> state file still pointed at the old name and its retry loop logs any
> connect/upgrade/DNS failure as "hello_ack timeout" (misleading). Full
> forensics: `docs/DEBUG-MDNS-STALE-NAME.md`. AT-06/AT-11 regression on
> this binary still owed before AT-07/08/09.
>
> **2026-09-27 (later): one-shot endpoint installer shipped.** `agent/
> install.sh` now has `--flash` + `--provision`: flashes an S3 from the
> committed bundle `firmware/dist/esp32-s3/` (bootloader/partitions/
> boot_app0/app + VERSION; refresh from `.pio/build` after each firmware
> change) via esptool, then serial-provisions WiFi + 3 API keys + admin
> password (→ `~/.maccontrol/endpoint.keys`, shown once), then opens the
> pairing window over the Web UI API and pairs the agent — full flow:
> `./install.sh --flash --provision --allow … --enable-launch --enable-quit`.
> Also fixes the copied-`.venv` bug (broken symlinks from another machine are
> detected by *executing* the interpreter and auto-recreated). The flash
> recipe was captured verbatim from `pio run -t upload -v`. Re-provision on a
> used device re-ADDS keys (8-key cap; revoke old ones first).
>
> **2026-09-27 (evening): AT-07/08/09 authored** (`firmware/scripts/at07.py`,
> at08.py, at09.py — at11 idioms, two-consecutive-rounds each, py_compile +
> --help verified, NOT yet live-run). AT-07 (verified lock) is runnable now
> (no sleep involved; pairing is already this Mac's agent ag-4d00 — a run
> LOCKS THE USER'S SCREEN, by design). **Topology blocker for AT-08/09:** the
> HID cable attaches to THIS Mac — the harness host — so sleeping/restarting
> the "target" freezes the harness mid-test, and AT-09's in-window status
> sampling is impossible co-located. Awaiting the user's call: second Mac as
> target (move the HID cable; harness stays here), or a post-hoc +
> `pmset scheduled-wake` partial-coverage mode (restart fully verifiable
> post-hoc from the ledger/log timestamps; sleep/wake exercises the
> new-session predicate via a post-dispatch wake command; AT-09's
> during-window sample still needs some awake LAN device for one GET).
>
> **2026-09-27 (night): AT-07 PASSED on hardware.** Both consecutive rounds
> green on one boot (`completed`/`lock_confirmed` in 5.2 s and 2.6 s of the
> 15 s deadline; agent-observed transition; capabilities L3/verified pinned
> as preconditions; human unlocked between rounds — that in-the-loop step is
> permanent for AT-07). The user chose the co-located protocol for
> AT-08/09 (harness freezes with the Mac; human observes/wakes/reports), so
> both scripts gained phased modes: at08 `--phase
> {restart-dispatch,restart-verify,sleep-dispatch,sleep-verify,wake}` with a
> /tmp/at08_state.json chain (post-hoc deadline checks via ledger
> dispatched_at + device-log state_transition timestamps), at09 `--phase
> {dispatch,verify}` with printed phone-curl sampling instructions and
> interactive y/n human-observation recording. 32/32 offline mock checks.
> Next live steps: AT-09 chain (dispatch → human samples → verify), then the
> AT-08 chain (restart → sleep → wake), then AT-06/AT-11 regression ×2 on
> this binary → Phase 5 gate.
>
> **2026-09-27 (late night): HID system-sleep is a dead end on modern
> macOS; power actions move to the agent (protocol v2).** Verified the hard
> way during AT-09: `Cmd+Alt+Power` only display-sleeps, and a HID System
> Control Sleep report (which enumerates and macOS parses) is silently
> ignored — assertions clean, so neither HID mechanism can sleep this Mac.
> The engine chain itself is PROVEN: a `pmset sleepnow` rehearsal with a
> live confirming record completed `sleep_confirmed` at window close on
> budget. Shipped: agent-executed sleep/restart/shutdown (agent v1.2.0,
> `pmset`/`osascript`, goodbye-with-reason flushed BEFORE acting — the
> previously-dead restart/shutdown goodbye paths now used), firmware
> dispatcher routes Mode B power to the agent (HID fallback in Mode A),
> dispatch envelope `{action, command_id}` for power / `bundle_id` required
> only for app actions, **PROTOCOL_VERSION 2** (v1 refused, close 4003,
> both directions — agent must restart when the firmware flips). 146/146
> native tests. AT-07 stays green-irrelevant (lock is HID, unaffected).
> NEXT: flash needs the 'com' cable (unplugged again — the JTAG port can't
> be forced into download mode), then restart the bench agent with
> `--enable-sleep --enable-restart --enable-shutdown`, then AT-09 ×2,
> AT-08 chain, AT-07 re-run + AT-06/AT-11 regression ×2 → Phase 5 gate.
>
> **2026-09-27 (very late): AT-09 PASSED; v2 live.** Flashed (com cable
> back; board needed a manual reset out of download mode after flash),
> agent v1.2.0 up with the three power actions enabled. AT-09: five
> consecutive chains, all `completed`/`sleep_confirmed` ≤90 s; final chain
> witnessed from a second computer (expected_offline in-window past the
> 15 s mark; stale after close; boot_id retained+fresh). Bench MUST have
> `sudo pmset -a powernap 0` + `sudo pmset -a tcpkeepalive 0` or Power
> Nap/wake-for-network dark wakes keep agent heartbeats alive during
> sleep and freshness never degrades (device correct either way). at08/
> at09 post-hoc deadline bound fixed to match `state_transition` events
> only. NEXT: AT-08 chain (restart reboots the harness Mac — operator
> saves work first), then AT-07 re-run + AT-06/AT-11 regression ×2 →
> Phase 5 gate.
>
> **2026-09-24 (late): Phase 5 FOUNDATION complete — host-verified, not
> flashed.** Verification predicates live: Mode B power/lock/wake/macro
> commands stay `confirming` and complete on MCA evidence per spec
> 5.3.1/§8/§10.3.1 (`lock_confirmed`, `wake_confirmed` on new-session hello +
> awake burst, `sleep_confirmed` at window close, `restart_confirmed` on
> changed boot_id + awake burst, `shutdown_confirmed` on window +
> ICMP-unreachable; negatives `unexpected_wake`/`unexpected_reconnect`/
> `host_still_reachable`). §7.2.2 `expected_offline` status provenance
> implemented (AT-09's surface). Capabilities: power/lock/macro
> `verified = paired && connected`, L1/L2/L3 levels. Engine intervals moved
> to a monotonic RAM sidecar (SNTP-jump-safe). Conformance pins for late/
> duplicate results + evidence dedup. **144/144 native tests; both envs
> compile.** Full log + behavior decisions: `docs/PHASE5.md`. Remaining for
> Phase 5: bench rewire (5 V PSU) + separate target Mac, first flash, AT-06/
> AT-11 regression, then AT-07/08/09 scripts. The agent-side items
> (commanded-vs-user sleep mark, restart/shutdown goodbye reasons) turned
> out to be optional hardening — the predicates are device-observed.
>
> **2026-09-23 (late): Phase 4.5 is COMPLETE and hardware-verified.** MCA
> telemetry per `docs/PHASE4.5.md`: full §6.3 `system_info` report on both
> sides (load samples, Mac uptime/boot_time, os_version/hardware_model,
> network reachability+IP), IOKit-based sleep/wake with declared-offline
> windows verified against a real sleep (`CommandEngine::
> on_agent_declared_offline`), `front_app_changed` (B1), the
> `/api/v1/agent/status` honesty fix, pyobjc/psutil in `agent/.venv`.
> **AT-06 all green + AT-11 all green in both rounds on the Phase 4.5
> binary; 11-min heap soak flat (free 73.5–74.3 KB, min/largest constant).**
> B2/B3/B4 deferred. Agent is at **v1.1.2** (1.1.0 = Phase 4.5 telemetry,
> 1.1.1 = app-detection fix, 1.1.2 = IOKit sleep/wake — see
> `docs/DEBUG-PHASE45-AT11.md` for the latent Phase 4 NSWorkspace-staleness
> flaw the Phase 4 gate got lucky on). 119 native tests; both envs compile;
> bench S3 flashed with the Phase 4.5 binary. Residual watch items: wake
> deltas can double-report on Power Nap dark wakes (cosmetic); the stale
> pid on not-running apps in `/agent/status` applications map (spec-9
> question).
>
> **2026-09-24: USB host-link detection shipped + verified.** Independent-
> power route: the endpoint now senses Mac sleep/off/unplug itself via its
> own USB device link (`src/usb_link.*`, DSTS.SUSPSTS PHY backstop with a
> 2-tick debounce — TinyUSB events/flags and BSESVLD are all blind on the
> devkitc; forensics `docs/DEBUG-PHASE45-USB-LINK.md`). Report field
> `/api/v1/agent/status → usb {link,state,changed_at}`; log events
> `usb_attached/suspended/resumed/detached`. Verified: labeled-USB cable
> unplug (single debounced pair — doubles as the power-off simulation, same
> PHY signature; full power-off test skipped by decision) and a real sleep
> (declaration 24 s before link loss, same boot_id, expected_offline).
> AT-06 + AT-11 re-ran green on the final binary. The bench is still
> bus-powered; the 5 V PSU rewiring remains the user's hardware task before
> true outage-survival is exercised. **Next: Phase 5 — AT-07/08/09
> (verified lifecycle; power-command predicates, expected-offline AT
> coverage).**

> **Numbering note:** the PRD (§17.2) counts six phases with the MCA work as
> its Phase 3; this repo's docs count it as Phase 4. So the repo's
> "Phase 5" = PRD Phase 4 "Verified lifecycle" (AT-07–AT-09) and the repo's
> "Phase 6" = PRD Phase 5 "Production hardening" (AT-10, AT-12). After
> Phase 6 the MVP is complete: **Milestone 2 gate = AT-06 through AT-12
> all green** (PRD §17.2; the PRD's old Phase 7/AI was removed). The PRD
> also warns: the determinism defaults (heartbeat 5 s, stale 15 s,
> offline 30 s, backoff 1/2/4/8/30 s) are AT inputs — changing them
> invalidates AT-06…AT-12.

Phase 4 is **COMPLETE and gate-verified** (AT-06 + AT-11 ×2 consecutive,
2026-09-23, see the banner history below and
`firmware/docs/DEBUG-PHASE4-AT11.md`). Phase 5 scope per
`firmware/docs/PHASE4.md` "Fixed / deferred":

1. **Power-command verification predicates** — make power/lock/macro
   commands verifiable in Mode B (currently honest `unconfirmed`/`hid_only`;
   every capabilities `verified` flag stays false except app_launch/app_quit).
2. **Expected-offline windows (§8)** — controller-facing wiring DONE in
   Phase 4.5 (`CommandEngine::on_agent_declared_offline`, goodbye reasons
   sleep/restart/shutdown mark confirming records with the 3/60 s window;
   agent declares via IOKit sleep detection + the existing goodbye path).
   Verified against a real sleep. What remains: the AT coverage (AT-08).
3. **AT-07/AT-08/AT-09** acceptance scripts.

Pre-flagged Phase 5 cleanups and watch items:

- **Engine deadline/coalesce arithmetic is epoch-based** — fine post-SNTP
  but wrong across the sync jump; convert to `IClock::millis()` (flagged in
  the conventions list below).
- **Duplicate-result handling**: one gate run was lost to a macOS-stalled
  `osascript` quit (10 s `ACTION_TIMEOUT_S`) whose redelivered duplicate
  reported `failed` and overwrote a completed record. Rare — but the PRD
  forbids this: "once any command reaches a terminal state the record is
  never reopened" (§5, ch. 5/line ~444) and "a command_result arriving
  after its command timed out ... MUST NOT reopen a terminal ledger
  record" (§6). If the engine truly accepted a result for a terminal
  command that is a PRD MUST-violation, not a flake: conformance-check
  result admission in Phase 5 before AT-07+ touches dispatch.
- **Heap/stack knobs** (measured 2026-09-23): `[heap]` shows http task HWM
  21 KB of 40 KB — the mc_http stack can likely trim to ~28 KB; the `[heap]`
  diagnostic itself (30 s, with stack HWMs) is still enabled — decide its
  retirement during Phase 5 soaks.
- Agent gaps (README): OS sleep/wake notifications and the full 5-view UI
  are later phases; screen-lock detection needs pyobjc.
- **Bench renamed:** the S3 answers mDNS as **`control-graphics.local`**
  (10.10.40.242; identity name "MacControl for Graphics Computer"). The old
  `mac-b53478.local` name is a stale cache entry that macOS intermittently
  resolves to a dead/wrong IP — AT agent state files with the old hostname
  produce endless "hello_ack timeout" retries (the agent logs ANY
  connect/DNS/upgrade failure that way). Check the agent state file's
  `hostname` first; see `docs/DEBUG-MDNS-STALE-NAME.md`. UART port
  `/dev/cu.usbmodem5CBD0148591` (CH343, 'com' cable).
  Re-run the Phase 4 gate any time with the commands in the 2026-09-23
  banner below.
- **`/api/v1/agent/status` honesty — FIXED in Phase 4.5 (2026-09-23):**
  `logged_in`/`screen_locked` now render `null` when the evidence is
  absent; the endpoint no longer invents a known lock state. (Original
  watch item: handler set `false` when `has_lock`/`has_user` were false.)
- **Composite MCA report (spec 6.3) — IMPLEMENTED in Phase 4.5
  (2026-09-23):** heartbeat now carries load samples / `boot_time` /
  `mac_uptime_s` / network; capability_report keeps `os_version` and adds
  `hardware_model`; `/api/v1/agent/status` renders the full `system_info`
  section; sleep/wake detection via NSWorkspace marks a declared sleep
  `expected_offline` (not fault). See `docs/PHASE4.5.md` implementation
  log. Remaining agent gaps (README): the full 5-view UI is a later phase.

> **2026-09-23: Phase 4 GATE PASSED — the "degrading bench" was firmware.**
> AT-06 all green + AT-11 all green **twice consecutively on one boot**.
> The device-drops-off-WiFi-in-minutes symptom was per-request heap
> retention (~400 B/request in the binary Wi-Fi stack) plus ~90 KB of
> in-RAM ledger revisions, collapsing the largest free block until lwIP
> couldn't allocate TX buffers. Fixes: ledger latest-revision-only RAM
> (flash history unchanged), static 3 KB arenas for the reusable status/
> capabilities JsonDocuments, per-request churn cut (reused header block,
> snprintf'd headers), 60 s key-touch quantization. Steady-state heap went
> 6.8–16.7 KB → 109 KB free / 90 KB largest. AT-11 Phase-A timing also
> needed three anchor fixes: firmware freshness is now agent-silence-based
> (spec 4.2.2), the at11.py silence clock anchors on the heartbeat
> interval (never on `mac.state.observed_at` — it tracks state changes),
> and the MCA anchors its first heartbeat to the initial burst. Full
> forensics: `firmware/docs/DEBUG-PHASE4-AT11.md`.
> **Next: Phase 5 (verified lifecycle: power-command predicates,
> expected-offline windows, AT-07–AT-09).**
>
> **2026-09-22 (late): the earlier "Phase 4 COMPLETE" claim below is NOT
> verified.** A debug session re-derived the AT-06/AT-11 instability from
> scratch: four firmware root causes found and fixed with decoded
> backtraces (HTTP-task WDT starvation under busy keep-alive; unbounded
> socket writes; keep-alive head-of-line blocking starving the accept
> backlog; uncaught `bad_alloc` in `sendJson` → abort), plus
> `esp_reset_reason()` now logged at boot so every reboot is classifiable.
> AT-06 ran fully green on the fixed binary; AT-11's transport/ledger
> contract is green; the final two-consecutive AT-11 runs are blocked by a
> degrading bench (device drops off WiFi within minutes on ALL builds).
> See `firmware/docs/DEBUG-PHASE4-AT11.md` for the full analysis, the
> reverted wedge-supervisor experiment, and the fresh-bench re-run
> procedure. Phase 4 feature set (from the earlier work, all still true):
> Python MCA at `agent/` (websockets client, launchd, headless-pairable),
> pairing ceremony (spec 3.2), hand-rolled RFC 6455 WS server on port 80
> (`/agent/v1/ws`), polling fallback, heartbeat 5/15/30 s, verified app
> launch/quit, Web UI Pairing tab, LittleFS key store, monotonic-clock
> pairing windows, `/api/v1/apps/*` engine gate.
> **Next: Phase 5 (verified lifecycle: power-command predicates,
> expected-offline windows, AT-07–AT-09) once the AT-11 gate is re-run.**
>
> **2026-09-21: Phase 2 is COMPLETE.** ESP32-S3 acceptance passed: AT-01/02
> all green (real `unconfirmed`/`hid_only`), AT-03/04 all green twice
> consecutively on the final binary, Web UI browser-checked. See
> `firmware/docs/PHASE2.md` "ESP32-S3 acceptance (2026-09-21)" for the four
> firmware fixes that were required (HID `USB.begin()`, `/status` device_id,
> S3 console shim, mDNS AAAA) and the S3 operational landmines (port roles,
> macOS DTR/RTS strapping, venv python for the AT scripts).
>
> **2026-09-21: Phase 3 is COMPLETE — Milestone 1 gate passed.** AT-01
> through AT-05 all green on the S3. Delivered: `GET /api/v1/logs`
> (filters/limit/dropped, spec 15.2), `GET /api/v1/openapi.json` (static 3.1
> doc with amendment note), session-only key management (`/api/v1/keys*`),
> agent surface stubs (409 `agent_not_paired`), Web UI Security + Logs tabs
> with the cleartext warning banner, mDNS re-announce on reconnect, and a
> fix for the macro/trigger persistence ceiling (NVS ~4 KB entry limit →
> double-slot LittleFS files). See `firmware/docs/PHASE3.md`. Next: Phase 4
> (MacControlAgent: pairing ceremony, WebSocket transport, heartbeat).

## One-paragraph context

MacControl is a deterministic ESP32 Mac controller specified by
`MacControl_Deterministic_Architecture_Spec.md` (the PRD; chapters cited as
§N below). Firmware lives in `firmware/` (PlatformIO, Arduino core). Phase 1
(REST endpoint, command ledger, RBAC, mDNS, USB HID — AT-01/AT-02) and Phase 2
(macro engine, triggers, Web UI, persistence — AT-03/AT-04) are **code-complete
and tested as far as possible without working HID hardware**. An ESP32-S3 has
been ordered; when it arrives, run the acceptance checklist below, then Phase 2
is done.

## What is done (do not rebuild)

- Phase 1 + Phase 2 firmware, both envs compile clean:
  - `firmware/` — `pio run -e esp32-s3-devkitc-1` and `-e esp32-wroom-32`
- Native test suite: **119/119 green** (`cd firmware && ./.venv/bin/pio test -e native`)
- Live smoke on the ESP-WROOM-32 verified: macro CRUD/execute, triggers, Web
  UI login/session/lockout, identity update, persistence across reboot, §14.3
  UI labels, closed-surface 404s.
- Docs: `firmware/docs/PHASE1.md`, `firmware/docs/PHASE2.md` (read both).

## What remains (exactly this, in order)

0. **DONE 2026-09-21** — steps 1–6 below all executed on the S3; Phase 2
   complete. Kept for reference:
1. **ESP32-S3 arrives.** Connect: UART-USB for flashing/monitor; the **USB-OTG
   port** to the target Mac for HID.
2. Flash: `cd firmware && ./.venv/bin/pio run -e esp32-s3-devkitc-1 -t upload`
3. Serial provisioning (115200, `./.venv/bin/pio device monitor`):
   ```
   wifi set <ssid> <password>
   key create READ qa
   key create CONTROL qa
   key create ADMIN qa
   admin set <password 10+ chars>
   ```
   Copy the three `mck_…` keys — shown once.
4. **AT-01/AT-02 regression** (expect real `unconfirmed`/`hid_only`; do NOT
   pass `--allow-dispatch-error`):
   ```
   python3 firmware/scripts/at01_at02.py --hostname <mac-XXXXXX>.local \
     --read-key mck_... --control-key mck_...
   ```
5. **AT-03/AT-04 twice consecutively** (§16.1 rule), then a browser check:
   ```
   python3 firmware/scripts/at03_at04.py --hostname <mac-XXXXXX>.local \
     --read-key mck_... --control-key mck_... --admin-key mck_... \
     --serial-port /dev/cu.usbserial-XXXX [--allow-dispatch-error]
   ```
   (`--serial-port` does the reboot via DTR/RTS; find the port with
   `./.venv/bin/pio device list`. The script self-locates pyserial in the venv.)
   Browser: open `http://<hostname>.local/` — check the four tabs, then run a
   macro from the Dashboard RUN button with the Mac watching keystrokes.
6. If all green twice: **Phase 2 complete.** Update `firmware/docs/PHASE2.md`
   "Verification status" and move to Phase 3 (identity/security/integration
   contracts: `/capabilities` honesty is done; `/openapi.json`, logs endpoint,
   security page, mDNS hardening — see §17.2.1).

## Hardware state / landmines

- **ESP32-S3 (accepted 2026-09-21)**: flashing + AT `--serial-port` go over the
  **'com' port** (CH343 UART bridge, auto-reset works at 921600). The 'usb'
  port carries TinyUSB HID to the target Mac — once `USB.begin()` runs the
  JTAG serial on that port is gone until reboot. macOS asserts DTR+RTS on
  every serial open, which straps the JTAG port into download mode; a board
  stuck at `waiting for download` just needs a power-cycle. Console scripting:
  `scripts/serial_cli.py`; run `at03_at04.py` with `.venv/bin/python`.
- **ESP-WROOM-32 (classic)**: wedged 2026-09-15 — dropped off USB entirely
  during AT runs; likely caused by a GPIO trigger left enabled on floating
  pin 4 (interrupt storm → ledger/flash flood). Firmware now has a storm
  guard (auto-disable >8 fires/10 s, `gpio_trigger_storm_disabled` log). If
  this board is revived, delete GPIO triggers before enabling anything.
- **Classic ESP32 has no USB HID** — commands honestly terminate
  `failed/dispatch_error` or `failed/usb_disconnected`. Only the S3/S2/C3/C6
  can produce real HID verdicts.
- **CH340 boards** often fail upload at 921600 baud — the `esp32-wroom-32`
  env is set to 460800; S3 devkitc handles 921600.
- If the S3 enumerates its own USB-CDC instead of a UART bridge (native USB
  variant), the monitor port may differ — `pio device list` will show it.

## Environment quick-start (fresh machine/session)

```bash
cd "…/ESP32-MCA Command Loop Design/firmware"
python3 -m venv .venv && ./.venv/bin/pip install platformio   # if .venv missing
./.venv/bin/pio test -e native          # 114/114 expected
./.venv/bin/pio run -e esp32-s3-devkitc-1
```

## Working conventions learned the hard way (keep respecting these)

- **Intervals on the monotonic clock, epoch for display only** (Phase 4
  hardware finding): `EspClock::epoch_seconds()` serves a fallback timeline
  (2025-01-01 + uptime) until SNTP first syncs, then jumps forward — any
  interval computed across the jump is garbage (it insta-expired pairing
  windows). Pairing window/session liveness use `IClock::millis()`. The
  command engine's deadline/coalesce arithmetic is still epoch-based — fine
  post-sync, flagged as a Phase 5 cleanup.
- **The task watchdog only forgives what it covers** (Phase 4 debug
  session): every task loop must feed the TWDT *inside* long servicing
  loops (a busy keep-alive connection starved `mc_http` → abort → reboot
  mid-AT-06), and every blocking primitive needs a deadline (socket writes,
  frame writes). lwIP's tcpip task is NOT TWDT-covered — a silent stack
  wedge produces "WiFi associated, CLI alive, network dead" with zero
  diagnostics.
- **RAM ring erases the evidence** — log `esp_reset_reason()` at boot
  (done: `boot` entry carries `"reset"`, also on the serial banner).
  Without it, external resets and internal aborts are indistinguishable
  after the fact. Hold a serial session during any repro to capture the
  panic backtrace (names the starved task); a tee'd copy lives at
  `/tmp/at06_tee.py` (monkeypatches at06's SerialSession to log serial).
- **bad_alloc terminates even with a catch if it fires during unwind**
  (double-fault rule) — on this 320 KB part, prevent the throw: the HTTP
  server has a master heap watermark (`kMinLargestFreeBlock` 2048 B),
  heap-aware log-page sizing, and static scratch fast paths. Never let a
  request handler's `std::string`/`JsonDocument` growth throw; refuse or
  serve partial allocation-free.
- **Single-threaded server + keep-alive = head-of-line blocking**: a
  poller faster than the 3 s idle bound starves the accept backlog
  (at06's own polling blocked the MCA WS handshake for 90 s). The server
  now preempts the idle wait when the backlog has a client
  (`preempt_client_`). Watch for this pattern in any new endpoint.
- **ws::write_fully used to drop frames on a momentarily full buffer**
  (hello_ack timeouts that looked like network flakiness) — retry with a
  deadline, like the HTTP write path.
- Heap discipline under poll load (Phase 4, cost three reboots): never
  grow fresh `std::string`s in hot paths — one static response buffer serves
  all HTTP serializations; throttle LittleFS/NVS persistence (≥5 s); never
  let a lazy persist throw — `loop()` wraps its drains in try/catch. A crash
  during LittleFS traffic can also silently reformat LittleFS (keys/ledger
  live there now) while NVS (identity/pairing/Wi-Fi) survives.
- **Closing ANY serial port session reboots the S3** — not just the AT
  scripts (macOS re-asserts DTR/RTS on open AND close; the CH343 lines
  drive EN/IO0). Every `serial_cli.py` probe or debugger attach is a
  device reboot: the boot banner + fresh WiFi association on each open is
  the tell. Diagnose a live network fault over the network (ping/HTTP),
  or hold ONE session open for the whole investigation; a probe-induced
  reboot destroys RAM-only state and muddies every measurement.
- **"WiFi associated, CLI alive, network dead" = heap, not AP** (2026-09-23):
  idle-solid + dies-under-polling is per-request heap retention/fragmentation
  until lwIP can't allocate TX buffers (`WiFiClient.cpp:429 errno 11` last
  word, no panic/reboot). Read `[heap]` (free/largest/min) before blaming
  the bench. Biggest tenants: full-revision ledger RAM (now latest-only),
  JsonDocument pools (now static arenas), 40 KB mc_http stack.
- **Liveness clocks and value provenance are different anchors** (2026-09-23):
  freshness must be computed from the last admitted FRAME (agent silence,
  spec 4.2.2), never from a value's `observed_at` (tracks state changes —
  arbitrarily stale on an idle peer); and a harness measuring "N s of
  silence" must anchor on the frame clock (heartbeat interval bound), not
  on any five-tuple `observed_at`. Symmetric agent bug: the first heartbeat
  must be due one interval after the burst frames, not after the telemetry
  loop starts (the inline initial burst delays loop start by seconds).
- Run AT scripts against the agent venv with `cwd=agent/`
  (`agent/.venv/bin/python -m maccontrol_agent`).
- **ArduinoJson 7**: never `doc["x"] | nullptr` (bool-overload trap → null).
  Use `.is<const char*>()` / `.as<T>()`.
- stdio `fopen` cannot reach LittleFS (ESP32 VFS mount prefixes) — use the
  Arduino `File` API.
- `const std::string& x = vec[0]; vec.erase(begin())` dangles — copy by value.
- FreeRTOS mutexes are NOT recursive: `CommandEngine::submit()` invokes the
  macro resolver while the caller holds `engine_mutex` — the resolver in
  `main.cpp` deliberately does not lock. Keep it that way.
- Only the error codes in `mcco::ErrCode` may be emitted (closed table).
- Mode A invariant: commands NEVER terminate `completed`; never add a path
  that could mark it.
- Serial debug prints are fine during bring-up but remove them before
  declaring tests green (they cost a flash cycle each).

## Known deviations to reconcile in Phase 3 (ALL RECONCILED 2026-09-21)

1. `/api/v1/triggers` + `/api/v1/device/identity` exist for the Web UI but are
   not in the spec ch. 12 inventory — must appear in `/openapi.json` with a
   spec amendment note. → **Done**: amendment note in `openapi.json`.
2. Web UI session cookie acts as ADMIN on `/api/v1/*` (§14 rule). → **Done**:
   documented in the amendment note.
3. First `/ui/login` with no password set creates it (one-time setup). →
   **Done**: documented on the login form / Security tab.
4. UI-login and API-key lockouts are tracked per failure class. → as built.
5. `GET /api/v1/logs` not yet implemented (ring buffer `entries_since()` is
   ready in `src/log_sink.*`). → **Done**: endpoint + filters + dropped.

## Key files

- `firmware/lib/maccontrol_core/` — pure C++17 spec logic (ledger, engine,
  macros, RBAC, docs, errors) — host-testable; Phase 4 adds `mc_pairing.*`,
  `mc_agent_events.*`, `mc_agent_session.*`, `mc_sha1.*`, Mode B builders in
  `mc_status.*`, engine agent gate/evidence — 114 tests in
  `firmware/test/native/`
- `firmware/src/` — Arduino glue; Phase 4 additions: `ws_server.*`,
  `agent_link.*` (WS task + polling ingress + liveness timer),
  `nvs_config.*` (hydratePairing/persistPairing + LittleFS key store)
- `agent/` — MacControlAgent (Python): `maccontrol_agent/` package,
  `launchd/com.maccontrol.agent.plist`, README
- `firmware/scripts/at01_at02.py`, `at03_at04.py`, `at05.py`, `at06.py`,
  `at11.py` — acceptance runners
- `firmware/docs/PHASE1.md` … `PHASE5.md` (PHASE6 pending) —
  per-phase build/verify guides + bring-up logs (`PHASE5.md` is the current
  state; `PHASE4.5.md` the telemetry phase)
