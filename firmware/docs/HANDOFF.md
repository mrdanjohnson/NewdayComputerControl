# MacControl — Phase 2 Handoff (resume document)

Read this first in the new session. It contains everything needed to finish
Phase 2 without re-deriving context. Project root:
`/Users/danieljohnson/Public/ESP32-MCA Command Loop Design/`

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
  macros, RBAC, docs, errors) — host-testable, 85 tests in `firmware/test/native/`
- `firmware/src/` — Arduino glue; Phase 2 additions: `macro_runner.*`,
  `trigger_store.*`, `web_ui.*`, `web_ui_page.h`
- `firmware/scripts/at01_at02.py`, `at03_at04.py` — acceptance runners
- `firmware/docs/PHASE1.md`, `PHASE2.md` — build/verify guides + bring-up logs
