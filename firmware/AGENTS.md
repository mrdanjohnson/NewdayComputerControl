# AGENTS.md — firmware/ (MacControl ESP32 endpoint)

Guidance for AI/coding sessions working in this repo. The authoritative
status/landmine reference is `docs/HANDOFF.md` — read it when resuming
work. This file captures what changes day-to-day; keep it in sync.

## Commands

```bash
./.venv/bin/pio test -e native                 # 119 host tests — run before EVERY flash
./.venv/bin/pio run -e esp32-s3-devkitc-1      # primary target (S3)
./.venv/bin/pio run -e esp32-wroom-32          # classic ESP32 (no USB HID)
./.venv/bin/pio run -e esp32-s3-devkitc-1 -t upload   # flash (uses UART port)
./.venv/bin/python scripts/serial_cli.py --port /dev/cu.usbmodem… --cmd "status"
```

`pio`/`python3 scripts/*.py` from the repo root; AT scripts locate pyserial
in `.venv` themselves. The Mac agent runs from `../agent` with its own venv
(`agent/.venv/bin/python -m maccontrol_agent`).

## Layout

- `lib/maccontrol_core/` — pure C++17 spec logic; must compile on the host
  AND the device. No Arduino headers here, ever.
- `src/` — Arduino glue. `http_api.cpp` (single-threaded HTTP + routing +
  agent surface), `agent_link.cpp` (WS task, polling ingress, liveness),
  `ws_server.cpp` (RFC 6455 codec), `command_dispatcher.cpp`,
  `mc_engine` wrapper in `main.cpp`, `nvs_config.cpp` (NVS + LittleFS),
  `log_sink.*` (static 128-slot RAM ring), `status_cache.cpp`.
- `test/native/` — host tests; extend when you change `lib/` behavior.
- `scripts/` — acceptance runners (`at*.py`, `mc_http.py` keep-alive helper,
  `serial_cli.py`). Gates need **two consecutive green runs on one boot**.
- `docs/` — PHASE1–4.5 logs, `HANDOFF.md`, `DEBUG-PHASE4-AT11.md`,
  `DEBUG-PHASE45-AT11.md`.

## Hard-won constraints (violating these has cost reboots)

- **320 KB RAM, no PSRAM.** Heap discipline is a correctness issue: steady
  state ~109 KB free (30 s `[heap]` serial line: free/min/largest + per-task
  stack HWM). Don't add per-request heap growth, unbounded retained
  containers, or fresh large `std::string`s in hot paths. New persistent
  data structures go in BSS/static arenas, not the heap.
- **Never let an allocation throw in a request path** — check
  `heap_caps_get_largest_free_block()` up front and refuse/serve-partial
  allocation-free (`kMinLargestFreeBlock` watermark pattern in
  `http_api.cpp`). `bad_alloc` during unwind terminates even with a catch.
- **Task watchdog**: every task loop feeds the TWDT *inside* long loops;
  every blocking primitive has a deadline. The lwIP tcpip task is NOT
  WDT-covered — a wedge there is silent.
- **Single-threaded HTTP server** with keep-alive: the idle wait must keep
  preempting to the accept backlog (`preempt_client_`) or a poller starves
  new connections.
- **Intervals on `IClock::millis()`** (monotonic); epoch only for display.
  SNTP sync jumps the epoch clock forward mid-session.
- **Liveness/freshness semantics** (spec 4.2.2): freshness is computed
  from the last admitted agent FRAME (`last_frame_at`), never from a
  value's `observed_at`. Don't conflate provenance with liveness.
- **Ledger**: RAM mirror holds latest-revision-only; the flash file stays
  append-only — `compact()` filters evicted command_ids, it does not
  rewrite history. `test_ledger` pins this.
- **Honesty invariants**: Mode A commands NEVER terminate `completed`;
  only `mcco::ErrCode` values may be emitted; the agent token and admin
  password never appear in logs.

## Hardware landmines (S3 bench, device mac-b53478)

- **Any serial port open or close reboots the board** (macOS DTR/RTS →
  EN/IO0 via the CH343). Serial access is a deliberate reset: the boot
  banner + fresh WiFi association on open is the tell. Never diagnose a
  live network fault by opening serial — you destroy the evidence.
- UART-bridge port = flashing/console; USB-OTG port = HID to the Mac.
- If the radio goes silent while the CLI is alive: check `[heap]` first
  (fragmentation presents exactly like an AP problem). Do NOT add
  outbound-probe "supervisors" — tried, reverted, documented in
  `docs/DEBUG-PHASE4-AT11.md`.

## Docs to update when you change things

- Behavior/spec semantics → `docs/PHASE<n>.md` deviations + spec amendment
  note in `lib/maccontrol_core/mc_openapi.cpp`.
- Status/progress → `docs/HANDOFF.md` top banner.
- Any new failure mode → its own `docs/DEBUG-*.md`, linked from HANDOFF.
