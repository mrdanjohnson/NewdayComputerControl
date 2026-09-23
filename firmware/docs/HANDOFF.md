# MacControl — Phase 2 Handoff (resume document)

Read this first in the new session. It contains everything needed to finish
Phase 2 without re-deriving context. Project root:
`/Users/danieljohnson/Public/ESP32-MCA Command Loop Design/`

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
- Native test suite: **85/85 green** (`cd firmware && ./.venv/bin/pio test -e native`)
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
./.venv/bin/pio test -e native          # 85/85 expected
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
- `firmware/docs/PHASE1.md`, `PHASE2.md`, `PHASE3.md`, `PHASE4.md` —
  build/verify guides + bring-up logs
