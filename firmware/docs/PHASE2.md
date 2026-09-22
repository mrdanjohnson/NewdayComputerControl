# MacControl Firmware — Phase 2 (Web UI, macros, triggers)

Phase 2 of the spec §17.2.1: the USB HID macro engine, shortcut trigger
bindings, the ESP32 Web UI (Dashboard / Device / Macros / Triggers), and
persistent storage for all of it. Exit gate: acceptance tests **AT-03** and
**AT-04** (spec §16.1), plus no regression in AT-01/AT-02.

## What was built

**Macro engine (spec ch. 10)**
- Core model in `lib/maccontrol_core/mc_macro.*`: the seven closed step types
  (`key_press`, `key_combo`, `modifier_down`, `modifier_up`, `key_release`,
  `text`, `delay`), named USB HID usages (63-key table, single source for
  validation and dispatch), closed modifier set `{ctrl, shift, alt, cmd}`,
  `expected_event` restricted to the five ambient evidence types with the
  forbidden types rejected (`400 macro_invalid_step`), immutable-versioned
  macros (edits bump `revision`; triggers resolve latest), capacity 64.
- Interpreter (`src/macro_runner.cpp`): single-threaded sequential execution,
  ascending `order`, 10 ms inter-key interval, scheduler delays, wall-clock
  timeout (`failed`/`macro_timeout` with an all-keys-up report), USB-drop abort
  (`failed`/`usb_disconnected`, all-keys-up), 30 s dispatch-delay bound
  (`dispatch_delayed`), deleted-macro-at-dequeue guard, Mode A termination
  `unconfirmed`/`hid_only` — never `completed`.
- Ledger integration: `macro_execute` is a first-class command type — 202 +
  `command_id`, queue-full rejection is pre-ledger (`409 macro_queue_full`,
  depth 4), deadline `timeout_ms/1000 + 5 s` set at dispatch, store corruption
  → `409 store_corrupt` (spec 15.1 double-slot CRC persistence).

**Triggers (spec 10.2.1)**
- ESP32-owned bindings: `webui_button` (rendered as Dashboard RUN controls)
  and `gpio` (pin 0–21, edge, 10–500 ms debounce, pullup input) — both produce
  the same ledger record and queue entry as the implicit `http_button`
  (`POST /api/v1/macros/{id}/execute`). GPIO events on a full queue are dropped
  and logged with the would-be `macro_id`.
- Deleting a macro auto-disables its bindings and logs it.
- **Storm guard** (engineering hardening beyond the spec): a GPIO pin that
  fires >8 times in 10 s is treated as faulty (floating/noisy wiring) and its
  binding is auto-disabled with a `gpio_trigger_storm_disabled` warn log;
  re-enable requires an ADMIN write, surfacing the fault. Rationale: a
  physical input is an unauthenticated actuation surface and must never hammer
  the ledger or flash wear.

**Web UI (spec §14.1/14.3)**
- Served by the endpoint, no external dependencies: one embedded page with
  Dashboard / Device / Macros / Triggers tabs.
- Dashboard renders the status tuples with the exact §14.3 display rules
  ("Verified" / "Unverified — HID only" / "Last seen {age} ago" + stale badge /
  "Expected offline (power window)" / "No data"; palette #4A6FA5 / #7A8B99 /
  #8BA3C7 / #6B8CBB / #2E4A62; color always redundant with text; no completion
  vocabulary for unverified commands), the last ledger command, RUN buttons,
  and power-command buttons.
- Macros tab: full editor (name, timeout, step table, optional
  `expected_event`) with server validation authoritative; Triggers tab:
  binding editor; Device tab: identity editor with hostname validation and
  ≤2 s mDNS re-announce (spec 3.1.1).
- Admin authentication per §13.1: PBKDF2-HMAC-SHA256 (10 000 iters, 16 B salt)
  password stored via the double-slot CRC machinery; `mc_session` cookie
  (32-byte token, 8 h sliding); 5 failed logins → 60 s lockout, logged. A
  valid session acts as the ADMIN role for `/api/v1/*` (the §14 "Web UI is
  bound to ADMIN" rule). Password provisioning: `admin set <pw>` on the serial
  CLI (min 10 chars; never printed/logged) or the one-time setup form on first
  login when no password exists (documented decision).

## Endpoint additions
`GET /api/v1/macros` (READ), `POST /api/v1/macros` (ADMIN, 201),
`PUT/DELETE /api/v1/macros/{id}` (ADMIN), `POST /api/v1/macros/{id}/execute`
(CONTROL), `GET/POST /api/v1/triggers` + `PUT/DELETE /api/v1/triggers/{id}`
(ADMIN), `POST /api/v1/device/identity` (ADMIN), plus `/ui/login|logout|session`
and `GET /` / `/ui` for the page.

**Deviations (to reconcile with `/openapi.json` in Phase 3):** `/api/v1/triggers`
and `/api/v1/device/identity` are not in the spec ch. 12 inventory — they exist
because the Web UI must own trigger bindings and identity per the §14
responsibility matrix. Web UI session-as-ADMIN is the §14 binding rule. UI and
API-key lockouts are tracked per failure class (spec 13.1 reads as one lockout
per class; confirm in Phase 3).

## Verification status

- `pio test -e native`: **85/85** (ledger, engine incl. macro resolver and
  deadline override, macro model 37 tests, auth/rbac, docs, smoke).
- `pio run -e esp32-s3-devkitc-1` and `-e esp32-wroom-32`: both compile clean.
- Live smoke on the ESP-WROOM-32 (before it wedged): macro CRUD round trip,
  `macro_invalid_step` rejections, execute → 202 → honest
  `failed/usb_disconnected` terminal (no USB on that chip), trigger CRUD +
  auto-disable on macro delete, Web UI login/session/lockout, session-as-ADMIN
  on the API, identity update + hostname validation, closed-surface 404s, and
  clean-reboot persistence of admin password/macros/triggers.
- **Pending — ESP32-S3 hardware (ordered):** run `scripts/at03_at04.py`
  (twice consecutively per §16.1) and re-run `scripts/at01_at02.py` as a
  regression gate. On the S3 with a Mac attached, AT-02/AT-03 will observe real
  USB HID reports and the expected `unconfirmed`/`hid_only` terminals; the
  `--allow-dispatch-error` flag will not be needed.

## ESP32-S3 acceptance (2026-09-21) — PASSED

Hardware: ESP32-S3-DevKitC-1 (16 MB flash, USB-JTAG + CH343 COM port). Final
firmware: AT-01/AT-02 **all checks passed** (real `unconfirmed`/`hid_only`
terminals, no `--allow-dispatch-error`), then AT-03/AT-04 **all checks
passed twice consecutively** (§16.1), including the mid-test serial reboot,
boot reconciliation (`failed/esp32_restarted`), and cross-reboot persistence.
Accept-to-202 latency 0.10 s / 0.18 s. Web UI checked in a browser (four
tabs, Dashboard RUN button driving real HID keystrokes to the attached Mac).

Firmware fixes that were required to get here (all in the tree now):

1. **HID enumeration**: `HidKeyboard::begin()` never called `USB.begin()`, so
   TinyUSB never started and every dispatch died `dispatch_error`. Added.
2. **`/status` schema**: `build_status_mode_a` omitted `device.device_id`
   (AT-04 asserts it persists across reboot). Added with the standard
   provenance tuple.
3. **S3 serial console** (`src/cli.cpp`): the IDF UART0 driver TX path never
   becomes operational in this core build over the USB-Serial-JTAG port
   (banner/CLI silent; ROM/`ets_printf` output works), and the
   `usb_serial_jtag` VCP write path delivers one batch per boot. The console
   shim on S3 therefore TXes via `ets_printf` and RXes via `uart_read_bytes`
   with a direct RX-FIFO-register fallback (`MC_CONSOLE_JTAG`). Other chips
   keep the plain `Serial` console.
4. **mDNS AAAA** (`src/wifi_mgr.cpp`): macOS resolves `.local` names by
   asking AAAA first and stalls a flat ~5 s on an unanswered AAAA before
   falling back to A. The STA's IPv6 link-local is now formed on first
   connect — *after* mDNS is running, or the responder never learns it — so
   AAAA is answered and first-lookup latency drops ~5 s → ~0 ms.

S3 operational notes (bring-up checklist details):

- **Port roles on this board**: the port labeled `usb` is the native USB
  (JTAG console + TinyUSB HID). Once TinyUSB starts (`USB.begin()`), the
  JTAG serial function disappears from the host — flashing and the AT
  `--serial-port` reset must use the `com` port (CH343 UART bridge), which
  auto-resets fine at 921600. Keep `usb` on the target Mac for HID.
- **macOS asserts DTR+RTS on every serial open**; on the JTAG port that
  straps the chip into download mode (GPIO0 low) at reset. If the board ever
  seems bricked into `waiting for download`, power-cycle it with a clean
  cable on the COM port.
- Use `scripts/serial_cli.py` for scripted CLI sessions (`pio device monitor`
  needs an interactive terminal). Run `at03_at04.py` with the venv python
  (`.venv/bin/python`) so pyserial resolves.
- Failed-auth attempts (10 consecutive from one IP) trigger the 60 s
  lockout answering 401 — don't probe the API with ad-hoc wrong headers
  between AT runs.

## WROOM-32 incident (2026-09-15) and fixes

During Phase 2 acceptance runs the WROOM-32 progressively degraded (~5 s
request stalls) and then dropped off USB entirely (CH340 no longer enumerated;
not recoverable in software). Contributing factor: smoke-test residue left a
GPIO trigger **enabled on pin 4**, which floats on the dev board — the
resulting interrupt storm starved the HTTP task and flooded the ledger/log.
Fixes applied: the storm guard above, plus removal of test residue. Treat any
enabled GPIO binding on an unterminated pin as a fault until proven otherwise;
the guard now enforces that automatically.

## S3 bring-up checklist
1. `pio run -e esp32-s3-devkitc-1 -t upload` (921600 baud).
2. Serial CLI: `wifi set …`, `key create READ|CONTROL|ADMIN …`, `admin set …`.
3. `python3 scripts/at01_at02.py --hostname mac-XXXXXX.local --read-key … --control-key …`
   (expect `unconfirmed`/`hid_only` — no `--allow-dispatch-error` needed).
4. `python3 scripts/at03_at04.py … --serial-port … --admin-key …` twice.
5. Open `http://mac-XXXXXX.local/` in a browser; verify the four tabs and the
   §14.3 state rendering; attach the USB-OTG port to the Mac and run a macro
   from the Dashboard.
