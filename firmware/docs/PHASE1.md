# MacControl Firmware — Phase 1 (HID control)

Phase 1 of the MacControl deterministic architecture spec (§17.2.1): an
ESP32-S3 that acts as a **USB HID keyboard** toward a Mac and as an
**authoritative REST endpoint** toward controllers. Mode A only — no
MacControlAgent, no pairing, no Web UI, no macros. Commands terminate as
`unconfirmed` / `result = "hid_only"` and can **never** be `completed`
without MCA evidence (spec §5.2.2).

Exit gate: acceptance tests **AT-01** and **AT-02** (spec §16.1).

## Layout

```
firmware/
├── platformio.ini            # envs: esp32-s3-devkitc-1, native (host tests)
├── lib/maccontrol_core/      # pure C++17 spec logic (ledger, lifecycle, RBAC,
│                             #   capabilities/status docs, errors) — no Arduino
├── src/                      # Arduino-ESP32 glue (USB HID, WiFi, HTTP, mDNS, CLI)
├── test/native/              # googletest host suites (run without hardware)
├── scripts/at01_at02.py      # acceptance test runner (run against hardware)
└── docs/PHASE1.md            # this file
```

## Hardware

- ESP32-S3 dev board (default config targets `esp32-s3-devkitc-1`; any S3 board
  works — set `board =` in `platformio.ini`).
- USB cable from the ESP32-S3 **USB/OTG port** to the target Mac (HID).
- A second USB connection (UART) or the onboard USB-serial for the provisioning
  console and flashing.
- 3.3 V logic; no other wiring is required for Phase 1.

## Build, flash, provision, verify

```bash
cd firmware
python3 -m venv .venv && ./.venv/bin/pip install platformio   # once

./.venv/bin/pio run -e esp32-s3-devkitc-1                     # build
./.venv/bin/pio run -e esp32-s3-devkitc-1 -t upload           # flash
./.venv/bin/pio device monitor                                # serial console (115200)
```

First boot generates the immutable `device_id` (from the factory MAC) and a
default hostname `mac-<last 6 hex of MAC>`, then prints the CLI banner.

Provision over the serial console (physical access = ADMIN, per spec §13 —
the Web UI provisioning path arrives with Phase 2):

```
wifi set <ssid> <password>
key create READ companion        -> prints the raw mck_... key ONCE; copy it
key create CONTROL qsys          -> likewise
identity set name ProPresenter Mac
identity set hostname mac-stage  -> [a-z0-9-], 1-57 chars (validated)
ntp set pool.ntp.org             -> optional; status timestamps need a clock
status                           -> brief health view
```

Then run the acceptance tests from any machine on the LAN:

```bash
python3 scripts/at01_at02.py --hostname mac-stage.local \
    --read-key mck_... --control-key mck_...
```

## What is implemented (and what is deliberately not)

Implemented, per spec:

- USB HID keyboard (keyboard-only USB descriptor; no mouse/scroll interface —
  spec §16.3), async dispatch task, re-enumeration poll 1 s / 5 s cap, one
  retry then `failed` / `dispatch_error` (§15.1).
- Command ledger: flash-backed append-only revision records, default capacity
  256 (bounds 64–1024), strict FIFO eviction including revisions, eviction
  guard (`failed` / `evicted_pending`), torn-tail discard, boot reconciliation
  within 2 s (`timed_out` if deadline passed, else `failed` /
  `esp32_restarted`) (§5.1.1).
- Idempotency: explicit `Idempotency-Key` replay → 200 with the existing
  `command_id`; divergent body → 409 `conflict`; keyless duplicate of an
  in-flight command within 60 s → 202 with the existing `command_id` (§5.1.1).
- REST surface: `GET /api/v1/status`, `GET /api/v1/capabilities`,
  `POST /api/v1/commands`, `GET /api/v1/commands` (filters + keyset
  pagination; offsets are unstable under FIFO eviction), `GET
  /api/v1/commands/{id}`, and the five convenience routes `POST
  /api/v1/system/{wake|sleep|restart|shutdown|lock}` — pure aliases of the
  generic command route (§12.1.1, §12.2.1).
- Status document: five-tuple provenance/freshness on every leaf, served from
  an in-memory cache (<50 ms), `connection.agent` = `false` /
  `esp32_direct`, all `mac.*` / `applications.*` tuples `unknown` nulls in
  Mode A (§7).
- Capabilities document: exactly the eight closed command types, all
  `verified: false` in Mode A, `app_launch` / `app_quit` `available: false`,
  `agent.paired/connected` false, `mode: "A"`, `capability_level: "L1"` (§12.3.1).
- RBAC: `Authorization: Bearer mck_...` keys, 32 random bytes base64url,
  only SHA-256 digests persisted, max 8 active keys, READ < CONTROL < ADMIN,
  401/403 semantics incl. revoked → 403 and expired → 401 (§13.1.1).
- Rate limiting per key: READ 60 / CONTROL 30 / ADMIN 10 req-min token bucket
  → 429 `rate_limited` (§13.1.1). 10 failed auths per IP → 60 s lockout.
- Deterministic error envelope `{"error":{"code","message","request_id"}}`;
  the status↔code mapping is the closed table of §4.1.1 — nothing else is
  emitted. Unknown paths (incl. `/api/v1/exec`, macros, OTA) → 404
  `not_found` (§13.3 negative-test rule).
- mDNS `_maccontrol._tcp` with TXT `id`, `api=1`, `mode=A`, `pair=unpaired`;
  collision suffixes `-2`…`-99`; re-announce ≤2 s after a hostname change (§3.3).
- Device identity per §3.1: `device_name` 1–32, `hostname` 1–57
  `[a-z0-9-]` start/end alnum, `location` 0–64, `description` 0–128,
  `device_id` 12 hex generated once, immutable.
- Reliability: TWDT 10 s, Wi-Fi reconnect backoff 1/2/4/8/30 s + 0–20 % jitter,
  `503 network_unavailable` while down, double-slot CRC32 config storage (§15.1).
- Internal ring-buffer log sink with the §15.2 entry schema (the
  `GET /api/v1/logs` endpoint is Phase 3 per the §17.2.1 phase table).

Deliberately absent (later phases): MacControlAgent, pairing, WebSocket,
polling fallback, macros and triggers, Web UI, app launch/quit,
`/api/v1/agent/status`, OTA, `/api/v1/openapi.json`, HTTPS, GPIO triggers.

## Engineering decisions on spec gaps

The spec leaves these open; the choices are recorded here (§1.1.1 permits
implementation decisions where the spec is silent):

1. **HID chords** — the spec names only "HID wake report" etc. (§8). Chords
   used (constants in `src/hid_keyboard.cpp`):

   | Command  | Chord                                   |
   |----------|-----------------------------------------|
   | wake     | Left-Shift tap (wakes without typing)   |
   | lock     | Ctrl + Cmd + Q                          |
   | sleep    | Cmd + Option + Power                    |
   | restart  | Ctrl + Cmd + Power                      |
   | shutdown | Ctrl + Option + Cmd + Power             |

   A modern Mac without an eject key maps Power-key usages; if your target
   Mac predates this, adjust the chord table.
2. **Key provisioning** — spec §13 defines Web-UI-only provisioning, but the
   Web UI is Phase 2. Phase 1 provisions keys over the serial console;
   physical access stands in for the ADMIN session.
3. **`/agent/v1/*`** — these paths are in the §12 endpoint inventory, so they
   exist but answer 401 `unauthorized` deterministically (no pairing record
   can exist in Phase 1; no pairing window ever opens).
4. **Idempotency across reboot** — dedup indexes are in-memory; a reboot
   resets the dedup window (the spec requires dedup over ledger retention,
   not across restarts).
5. **Mode A status shape** — §7's worked example is a Mode B document; in
   Mode A every `mac.*` / `applications.*` leaf is served as the
   all-`unknown` null tuple per §7.1.1's never-omit rule.

## Verification status

- `pio test -e native` — 46 core-logic tests, all passing on the host.
- `pio run -e esp32-s3-devkitc-1` and `pio run -e esp32-wroom-32` — both compile clean.
- **Hardware acceptance: AT-01 and AT-02 executed twice consecutively on a
  real ESP-WROOM-32 (spec §16.1's twice-consecutive rule) — all checks pass.**
  Run `scripts/at01_at02.py --hostname <host>.local --read-key …
  --control-key …`; on boards without a USB device controller add
  `--allow-dispatch-error`, which accepts `failed`/`dispatch_error` as the
  honest Mode A verdict for "no HID link" while still enforcing the
  never-`completed` invariant.

## Hardware bring-up log (ESP-WROOM-32, 2026-09-15)

Flashing and acceptance testing on real hardware surfaced five defects that
compile-time verification could not, all fixed:

1. **Boot crash (`LoadProhibited`)** — the CLI banner ran before `ctx.config`
   was assigned in `setup()`. Fixed init order.
2. **Ledger storage unreachable** — stdio `fopen("/maccontrol/…")` never
   reaches LittleFS: the ESP32 VFS resolves stdio paths against registered
   mount prefixes. Rewrote `fs_ledger_storage` on the Arduino `File` API.
3. **Multi-word CLI commands all "unknown"** — `const std::string& cmd =
   args[0]` dangled after `args.erase(begin())`. Fixed to a value copy.
4. **`doc["type"] | nullptr` always null** — with a `std::nullptr_t` default,
   ArduinoJson 7's `operator|` resolves to a `bool` overload, so existing
   string fields read as false→null and every `POST /api/v1/commands` was
   rejected 400. Fixed with explicit `is<const char*>()` / `as<const char*>()`.
5. **CH340 upload reliability** — 921600 baud fails on this board's USB-serial
   chip; the `esp32-wroom-32` env uses 460800.

On the WROOM-32 the REST surface, RBAC, ledger, mDNS, and honest Mode A
failure semantics are fully functional; HID dispatch is a compile-time stub
(chip has no USB device controller) and reports `dispatch_error`. Full
`unconfirmed`/`hid_only` verdicts require an ESP32-S3 (or S2/C3/C6) board —
the `esp32-s3-devkitc-1` env is unchanged and ready.
