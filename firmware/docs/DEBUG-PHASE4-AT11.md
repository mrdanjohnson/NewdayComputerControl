# Phase 4 AT-11 gate — debug log (status: **GATE PASSED 2026-09-23** —
# AT-06 all green + AT-11 all green twice consecutively on one boot; the
# "degrading bench" below turned out to be two more firmware defects, both
# fixed — see "2026-09-23 session")

This document is the working record of the AT-06 ceremony flake / AT-11
instability that consumed the Phase 4 bring-up, and of what the 2026-09-22
debug session (re)found and fixed. Read `docs/PHASE4.md` for the
feature/verification picture.

## TL;DR

The "AT-06 ceremony flake" was **never a pairing bug and mostly not the
serial DTR/RTS landmine**. The reboots that destroyed RAM-only pairing
windows were `mc_http` **task-watchdog aborts** (ground truth: `reset:
task_wdt` in the boot log, serial backtrace naming `mc_http`), caused by
four compounding firmware defects, all now fixed and flashed:

1. **WDT starvation under busy keep-alive** — the HTTP task fed the task
   watchdog only at the top of its accept loop; ~10 s of back-to-back
   requests on one connection (exactly what the AT harness's keep-alive
   polling does) starved it → IDF TWDT panic → abort → reboot.
2. **Unbounded socket writes** — `writeFully`/`ws::write_fully` could pin a
   task past the WDT (or drop WS frames outright on a momentarily full
   socket buffer — the "hello_ack timeout" symptom).
3. **Keep-alive head-of-line blocking** — the single-threaded server held a
   keep-alive connection for up to 3 s after every request, so a poller
   re-requesting faster than the idle bound starved the accept backlog
   **indefinitely** (at06's 1 Hz capabilities polling blocked the MCA
   WebSocket handshake for the whole 90 s connect window).
4. **Uncaught `bad_alloc` → abort** — under heap pressure a
   `std::string::reserve` in `sendJson` threw; nothing caught it →
   `std::terminate` → abort → reboot (decoded from a panic backtrace).
   A throw *during* exception unwinding terminates even with a catch, so
   the fix is preventing the throw: master heap watermark + heap-aware log
   page sizing + allocation-free fast paths.

## What was fixed (all flashed on the S3)

- `src/http_api.cpp` — WDT feeds inside `handleClient`, the keep-alive
  loop, and the write spin; 4 s absolute `kWriteTimeoutMs` on writes
  (logs `write_timeout`, drops the client); **backlog preemption** (a
  keep-alive idle wait peeks `accept()` ~10×/s and yields to a waiting
  client via `preempt_client_`); **master heap guard**
  (`kMinLargestFreeBlock` 2048 B — refuse allocation-free below this);
  heap-aware `sendJson` (static 2.5 KB scratch for small responses);
  heap-aware `/api/v1/logs` page sizing (serves a partial page that fits
  instead of refusing); `try/catch` around all per-client servicing with an
  allocation-free static 500; reusable member `JsonDocument`s for
  `/status` + `/capabilities` (pool survives `clear()`, no per-request
  pool churn).
- `src/ws_server.cpp` — `write_fully` retries with a 4 s deadline instead
  of dropping the frame on the first full buffer; WDT feeds in the WS
  read/write spins.
- `src/agent_link.cpp`, `src/command_dispatcher.cpp`, `src/main.cpp` —
  OOM `try/catch` firewalls around the WS session, dispatch processing,
  and CLI polling (a failed allocation logs and skips, never aborts).
- `src/main.cpp` — boot log now records `esp_reset_reason()` (boot entry
  gains `"reset":"poweron|ext|sw|panic|int_wdt|task_wdt|brownout|..."`;
  also printed to the serial banner). **Every future reboot is
  classifiable after the fact** — the RAM ring dies with the chip, so
  without this an external reset and an internal abort were
  indistinguishable (the original misdiagnosis).
- `scripts/mc_http.py` — up to 3 attempts with a 1 s pause on a dropped
  socket (the Wi-Fi link stalls mid-response occasionally; the firmware
  cuts stalled transfers after ~4 s, and a fresh connection then succeeds
  immediately).

Verification on the fixed binary: `pio run` both envs clean; native
**114/114**; keep-alive soak (1 Hz × 90, the old starvation pattern)
clean; 11 write-stall events all survived as ~4 s hiccups; **AT-06 ran
fully green** (all checks, one continuous boot); AT-11 run 1: every
transport/ledger check green (WS dispatch → `completed/
app_launch_confirmed`, quit → `app_quit_confirmed`, polling fallback
record identical shape, pending-endpoint probes) — its Phase-A timing
checks were polluted by the bench issue below, not by firmware behavior.

## Bench state — RESOLVED 2026-09-23 (it was never the AP)

The "device drops off WiFi within 1–3 minutes of every boot" symptom that
looked like a degrading bench/AP ghost-client was reproduced, root-caused and
fixed. Summary of the forensics (full detail in the session notes below):
idle (zero HTTP traffic) the device is rock-solid for 4+ minutes with
`free 16.7 KB / largest 10.2 KB`; under keep-alive HTTP polling at 1 Hz the
heap retained ~350–450 B per request and the largest free block collapsed
from ~4.6 KB to ~2.1 KB in ~26 requests, after which lwIP/Wi-Fi could no
longer allocate TX buffers and the radio went permanently silent (no panic,
no reboot, serial alive — `WiFiClient.cpp:429 write(): errno 11` last word).
The AT harness polls from boot, so the "1–3 minute" death was simply
~25 requests of poll load. Root causes and fixes shipped 2026-09-23:

1. **In-RAM ledger held every revision of every command** (only the latest
   is ever observed) — at 87 commands this alone consumed ~90 KB. Now
   latest-revision-only in RAM; `compact()` filters evicted ids from the
   flash file, so the durable append-only history is unchanged.
2. **`status_doc_`/`caps_doc_` were heap-pooled ArduinoJson documents** —
   now `JsonDocument` over custom 3072 B static arenas (first-fit,
   coalescing; heap fallback only on arena overflow, which
   `overflowed()` detects).
3. **Per-request malloc churn** (~15–20 allocs: fresh `header_block`
   reserve, `std::string` header concatenation) — `header_block` is now a
   cleared-not-shrunk member and all response headers are `snprintf`'d into
   a reused 384 B buffer (wire format byte-identical).
4. **API-key `touch()` dirtied keys on every request** → multi-KB LittleFS
   persist every 5 s under polling — now quantized to 60 s, dirty flag only
   on actual change.

Result: steady-state heap `free 109 KB / largest 90 KB` (was 6.8–16.7 KB),
127-request 1 Hz soak flat, 3× wrong-code bursts exact (403×4 → 409×2).
The ~400 B/request retention itself lives in the binary Wi-Fi/lwIP stack
(core 2.0.17) and is unreachable from `src/` — with 90 KB of headroom it is
harmless. If it ever bites again (e.g. a much longer soak), the ranked
fallbacks are a heap-triggered Wi-Fi circuit breaker in `WifiMgr` and, last
resort, an Arduino-core major upgrade. `[heap]` telemetry (now with per-task
stack high-water marks: http 20.9 KB of 40 KB, ws 29.3 of 32, dispatch 18.4
of 24, wifi 5.3 of 12) stays until Phase 5 soaks confirm stability.

## 2026-09-23 session: AT-11 Phase-A timing + the fixes that made the gate pass

With the radio stable, AT-11's remaining failures were Phase-A *timing*
checks — three real defects, one per component:

- **Firmware** (`lib/maccontrol_core/mc_status.cpp` `agent_freshness`):
  `mac.state.freshness` was computed from the last **system-state change**
  (`system_at`), not agent silence — on an idle Mac the state tuple read
  stale even with heartbeats flowing (AT-11 round 1 observed freshness
  already stale at kill time). Now stale after `stale_threshold_s` of
  **agent silence** (anchored at `last_frame_at`, observed_at keeps its
  value provenance) — exactly spec 4.2.2's "stale after 3 intervals of
  silence".
- **Harness** (`scripts/at11.py`): the silence clock anchored on
  `mac.state.observed_at` (`system_at`) — systematically ~12–14 s stale on
  an idle Mac, overshooting every age (offline read 42–44 s vs the 30 s
  threshold). Now anchored at the heartbeat interval (5 s): freshness is
  silence-based, so true silence at the kill is ≤ one interval, and the
  15–22 s / 30–40 s windows absorb the quantization exactly.
- **Agent** (`agent/maccontrol_agent/events.py` + `__main__.py`): the first
  heartbeat was due 5 s after the telemetry **loop** started, which begins
  only after the (slow, inline) initial status burst — a freshly connected
  agent sat silent for burst-latency + interval (~13 s observed) and read
  as prematurely stale. `on_session_start` now anchors the heartbeat timer
  right after the burst.

One residual flake, not a defect: a single `quit` timed out when macOS
stalled the `osascript` AppleEvent past the 10 s `ACTION_TIMEOUT_S` (plus
command redelivery side-effects); never recurred across the remaining gate
quits. If it recurs, look at duplicate-result handling before re-running.

Gate evidence (2026-09-23, one boot, final binary): AT-06 all checks green;
AT-11 run 1 — all checks green in both rounds (stale 16.2/14.2 s, offline
30.4/30.5 s); AT-11 run 2 — all checks green in both rounds (stale 16.3/14.3
s, offline 32.6/30.5 s). Native 114/114 throughout.

## Deviation note — FOLDED into PHASE4.md on 2026-09-23 (gate passed)

The log-ring deviation (item 7) and the two 2026-09-23 deviations (ledger
RAM residency item 8, silence-based freshness item 9) are now in
`docs/PHASE4.md` "Deviations".
