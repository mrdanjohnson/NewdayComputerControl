# MacControl Firmware — Phase 4 (MacControlAgent: pairing, WebSocket transport, heartbeat)

Phase 4 of spec §17.2.1: the MacControlAgent (MCA) and the ESP32-side agent
surface — pairing ceremony (spec ch. 3.2), WebSocket channel (spec 4.2),
polling fallback (spec 4.3), heartbeat liveness (5 s / 15 s / 30 s), and the
first verified command path: `app_launch`/`app_quit` completed by correlated
MCA evidence (spec 5.3.1). Exit criteria: **AT-06 and AT-11** (plus the
AT-01–AT-05 regression).

## What was built

**Pairing ceremony (spec 3.2)**
- `mcco::PairingStore` (core): state machine `unpaired → pairing_window →
  active → revoked → unpaired` (30-day purge); one-time 8-char code
  (alphanumeric minus `0/O/1/I`); window 60–600 s (default 120); 1 validation
  attempt/s (429, not counted); 5 failures close the window; raw 32-byte
  `agent_token` returned exactly once, only its SHA-256 digest persisted;
  re-pair implicitly revokes the incumbent (exactly-one rule); revocation is
  immediate (live sessions closed 4001). Stored in a double-slot NVS record
  (`pairing.s0/.1`); a stored window never survives reboot.
- `POST /agent/v1/pair` (pre-credential, window-only); `GET/POST/DELETE
  /api/v1/pairing[/window|/revoke]` (Web UI session only, ADMIN). Revocation
  closes in-flight sessions with close 4001 and flips the endpoint back to
  Mode A (commands again terminate `unconfirmed`).
- mDNS TXT `mode=`/`pair=` follow the pairing state (full reannounce — this
  ESPmDNS core has no runtime TXT removal).

**WebSocket channel (spec 4.2) — hand-rolled RFC 6455, no new deps**
- `ws://<hostname>:80/agent/v1/ws` (spec-exact URL): the existing hand-rolled
  HTTP server detects the Upgrade, answers 101 (SHA-1 + padded base64 added to
  the core as `mcco::Sha1`/`mcco::base64_encode`, natively tested), then hands
  the live `WiFiClient` to a dedicated `mc_agent_ws` FreeRTOS task via a
  queue. The 101 is written **before** the handoff so the task never reads the
  handshake bytes as a frame.
- Credential admission at `agent_hello` (spec 4.2.1): bad/revoked token or
  instance mismatch → close **4001**; unsupported `protocol_version` → 4003;
  `hello_ack` carries the server-assigned `session_id` and the heartbeat
  parameters. Mask enforcement (1002), 4 KB cap (1009), ping→pong, close
  codes 1000/1001/1008/4001/4002/4003.
- Session liveness owned by a 1 s timer for both transports: any valid frame
  resets the clock; STALE at 15 s (3×5), OFFLINE at 30 s (6×5); OFFLINE
  closes the evidence channel — confirming records without an
  expected-offline window terminate `unconfirmed`/`evidence_lost` (spec
  5.2.1). Power-command windows/predicates are Phase 5.

**Polling fallback (spec 4.3)**
- `POST /agent/v1/events` (identical envelope; first `agent_hello` returns
  `session_id`, then `X-Session-Id` header required) and `GET
  /agent/v1/commands/pending` (5 s poll default; drains the dispatch queue).
  Deterministic mapping: missing/unknown token → 401; **known-but-revoked →
  403** (`PairingStore::tokenDigestKnown` distinguishes them, spec 4.3.1);
  instance mismatch → 409 `agent_not_paired`; unknown/inactive session → 409
  `agent_offline`; schema violation → 400 `validation_failed`.
- Exactly one live session across transports: a new hello supersedes the
  incumbent (WS closed 1000).

**Event pipeline (spec ch. 6)**
- `mcco::parse_agent_event`: the closed 11-type catalog with per-type payload
  key tables (unknown keys are schema violations); 4 KB cap; `command_id`
  restricted to `command_ack`/`command_result`; schema-violating frames still
  consume their `seq` (spec 6.1.1). 3 consecutive violations close 4003.
- `mcco::AgentSession`: per-session `seq` (gap > 10 → 4002, regression →
  4002), liveness ratios fixed at 3×/6×.
- Admitted evidence updates the Mode B status cache (`AgentStatus` snapshot:
  system/lock/user/boot_id, per-app running/pid, capability_report) with
  `agent_reported` provenance and 15 s TTL freshness (never erased, spec
  7.2.2), and correlates commands in `confirming` via
  `CommandEngine::agent_event`.

**Verified app launch/quit (spec 5.3.1, 9.3, 11.2)**
- Engine gate (wired in main.cpp, hook pattern like the macro resolver):
  pre-ledger 409s in spec order — `agent_not_paired` → `agent_offline` →
  `app_not_allowlisted` → `command_disabled`.
- Dispatch: `{"action","bundle_id","command_id"}` pushed over the WS (or into
  the pending queue); the record stays `confirming` (deadline 30 s) and
  completes **only** on correlated `command_ack` + matching
  `application_started`/`application_exited` (either order) →
  `completed`/`app_launch_confirmed`|`app_quit_confirmed`;
  `command_result(failed)` terminates with the closed error code;
  `command_result(ok)` alone never completes; deadline sweep →
  `timed_out`/`deadline_exceeded`.
- `GET /api/v1/agent/status` serves the composite MCA report (spec 6.3) plus
  the live `session_id` (operator inspection; also what AT-11 uses to probe
  the polling endpoint).

**MacControlAgent (../agent/) — Python, launchd LaunchAgent**
- `websockets` client on port 80, bearer token, 5 s `hello_ack` timeout,
  backoff 1/2/4/8/30 s + uniform 0–20 % jitter (counter resets on
  `hello_ack`, retries forever at the cap), close-code table (1000/1001/4002/
  network → backoff; 1008/4001/4003 → HALTED, exit 2), expected-offline aware.
- Polling transport with identical envelope/seq semantics; transport switch ⇒
  new session. Mandatory initial status burst ≤ 2 s after `hello_ack`
  (system/user/lock/capability_report + running apps), 5 s heartbeats
  (`boot_id`, `uptime_s`), app launch/exit deltas (NSWorkspace via pyobjc,
  mdfind+pgrep fallback), screen lock via Quartz (best-effort — omitted
  rather than asserted when undetectable).
- Closed action set: `command_ack` ≤ 2 s always; `launch_app` (`open -b`) /
  `quit_app` (guarded `osascript`) gated by fail-closed enablement + bundle
  allowlist; `command_result` uses only the closed error enum. No shell,
  files, or input injection.
- State at `~/.maccontrol/agent.json` (0600): hostname, `agent_instance_id`,
  raw token, allowlist, enablement, transport. Tkinter status/code-entry UI
  (osascript fallback), `--headless --pair-code` for scripted ceremonies.
  `launchd/com.maccontrol.agent.plist` (RunAtLoad, KeepAlive,
  ThrottleInterval 10).

**Web UI**
- Seventh tab, **Pairing**: open/close the window (60–600 s), the one-time
  code rendered large with a live countdown (code is only displayed at
  window-open time — it is not re-fetchable), pairing state + record fields
  (never the token), Revoke with confirm, and a Mode A/B indicator.

## Also fixed this phase

- **Key store persistence ceiling (same NVS ~4 KB blob disease as the Phase 3
  macro store)**: the 8-key record outgrew the NVS string limit once revoked
  keys accumulated (`NOT_ENOUGH_SPACE` on `key create`; new keys lost on
  reboot). Keys now persist as a LittleFS double-slot store (`/kstore.0/.1`,
  commit marker `keys.cur`), hydrating with fallback to the legacy NVS record
  so pre-Phase-4 devices keep their keys. Found on hardware during AT
  provisioning.
- **SNTP timeline jump killed fresh pairing windows** (found on hardware):
  `EspClock::epoch_seconds()` serves a fallback timeline (2025-01-01 +
  uptime) until the first SNTP sync, then jumps forward to wall time. Any
  interval computed across the jump is garbage — a pairing window opened
  pre-sync "expired" the instant the sync landed (`now >= opened_at + 120 s`
  across a ~20-million-second jump). Pairing window lifetime/attempt throttle
  and session liveness now use the **monotonic** clock (`IClock::millis`,
  `esp_timer`); the AgentStatus `last_frame_at` epoch value is re-derived from
  the monotonic age at snapshot time so the cache stays on the wall-clock
  timeline. Native tests cover the jump (`WindowSurvivesEpochJump`). The
  command engine's deadline/coalesce arithmetic is still epoch-based — a
  command accepted in the first seconds after boot, before SNTP syncs, can
  get a wrong deadline; harmless in practice (ATs run post-sync) but flagged
  here as a Phase 5 cleanup item.
- **`make_session_id` emitted raw control bytes** (found on hardware): it
  hex-encoded then "uppercased" with `c - 'a' + 'A'`, corrupting digits into
  0x10-range control characters — the hello_ack frame then failed JSON
  parsing in the MCA. Now built from an uppercase hex alphabet directly, with
  a charset regression test.
- **mDNS TXT `pair=` vocabulary** now follows spec 3.3 exactly
  (`unpaired|active|revoked`; "paired" was emitted before).
- **Heap-exhaustion aborts under AT poll load** (three spontaneous reboots
  root-caused via the serial backtrace): `std::bad_alloc` → `std::terminate`
  → abort, first in `ConfigStore::persistKeys` (the `keys_dirty` drain ran an
  unthrottled multi-KB `std::string` serialization on every authenticated
  request). Fixes: key persistence throttled to ≥5 s intervals; one static
  response buffer reused for every HTTP serialization (per-request
  grow/shrink of ~6 KB status strings against lwIP buffers was the
  fragmentation driver); a `try/catch` safety net around the `loop()` dirty
  drains so a failed lazy persist can never abort the firmware.
- **LittleFS is wiped by `LittleFS.begin(true)` when a crash corrupts the
  filesystem** — an abort during heavy flash/LittleFS traffic cost the
  ledger, key store and macros once. The OOM fixes above remove the trigger;
  if it ever happens: keys/macros/ledger are gone (identity/pairing/Wi-Fi are
  NVS and survive), re-provision via serial.
- **Mode B gating came from the agent snapshot instead of the pairing
  record**: a paired endpoint with the agent offline reported `mode: "A"` —
  Mode B is a property of the pairing record (spec 3.3/12.3.1);
  `agent.connected` carries the session state.

## Hardware landmines added (S3 bench)

- **Closing the provisioning serial port reboots the device**: macOS
  re-asserts DTR/RTS on port close and the CH343 CDC lines drive EN/IO0 —
  the reset lands ~1–3 s after close and silently destroys RAM-only state
  (pairing windows). AT-06 therefore holds the port open for the whole test
  (class `SerialSession`); scripts that only reboot deliberately (at03/04
  `--serial-port`) are unaffected.
- **Run AT scripts against the agent venv python** (`agent/.venv/bin/python
  -m maccontrol_agent` needs `cwd=agent/` for the package import).
- **Never leave two MCA daemons running.** Every reconnect completes a fresh
  pairing-hello and the new session **supersedes the incumbent with close
  1000** — the loser daemon immediately backs off and reconnects, so two
  orphaned daemons ping-pong forever and every session dies seconds after
  establishment (this presented as an ESP32 TCP bug and burned hours).
  `at11.py`'s `stop_daemon()` now pkills strays; mind shell backgrounding:
  `cd dir && cmd &` makes `$!` a subshell — use `(cd dir && exec cmd) &` or
  pkill afterwards.
- **Unannounced transport loss ages, it does not verdict** (spec 7.2.2
  worked example 2): a SIGKILLed agent means silence, so the ESP32 keeps the
  session published after a socket death without a close frame and lets the
  liveness timer mark STALE at 15 s / OFFLINE at 30 s from the last frame;
  the timer reclaims the session at OFFLINE. A graceful close frame still
  ends the session immediately (spec 4.2.1). Verified: stale ~15.5 s,
  offline ~31.5 s after the last admitted frame. AT-11's Phase A SIGKILLs
  the daemon (`hard_kill_daemon`), not SIGTERM — a graceful goodbye would
  legitimately end the session at once.
- **Never pass `WiFiClient` through a FreeRTOS queue** (`xQueueSend`
  memcpys the object, bypassing the `shared_ptr` copy ctor and corrupting
  the socket-handle accounting — the prime suspect for one spontaneous
  reboot observed mid-AT). The HTTP→WS-task handoff is a mutex-protected
  `std::deque<WsOffer>` + task notification.
- **Test-app lifecycle hygiene**: a `launch` of an already-running app
  produces no `application_started` (the predicate correctly times out), and
  TextEdit with open documents can hang a graceful quit past the 10 s action
  timeout — AT-11 pkills the test app before each launch phase
  (`reset_app`).
- Opened-before-SNTP windows/records carry fallback-timeline timestamps (see
  above) — provision or pair ≥ ~10 s after boot.
- AT polling hammers budgets: poll ledger records with the READ key
  (CONTROL's 30/min bucket cannot sustain 2 s polling), and wait for
  `capability_report` to land (`commands.app_launch.available == true`)
  after reconnect before dispatching.

## Deviations (flagged for the spec amendment note)

1. Power/lock/macro **verification predicates are Phase 5** — in Mode B those
   commands still terminate `unconfirmed`/`hid_only` (honest, never
   `completed`); every capabilities `verified` flag stays false except
   `app_launch`/`app_quit` with a live session. Recorded in
   `/openapi.json`'s amendment note.
2. `build_unflags = -std=gnu++11` on both ESP32 envs: the espressif32
   platform appends `-std=gnu++11` *after* `build_flags`, so src TUs were
   compiling as C++11 while the core needs C++17 (`std::optional`).
3. `board_build.partitions = min_spiffs.csv` on esp32-wroom-32: the image
   passed the default OTA slot (~103%); LittleFS is unaffected (SPIFFS shrinks
   to 64 KB). WROOM flashes need the partition table updated once.
4. WS read deadlines are enforced inside the frame codec (polled wait), not
   `WiFiClient::setTimeout()` — in core 2.x `read()` is non-blocking and
   `setTimeout` only affects `connect()`.
5. `capabilities` `app_launch.apps[]` lists the MCA allowlist with the bundle
   ID as display name — the monitored-app registry (display names,
   `app_not_registered`/`app_control_disabled`) is Phase 6.
6. MCA known gaps (README): OS sleep/wake notifications and the MCA's full
  5-view UI are later phases; screen-lock detection needs pyobjc.

## Verification status

- `pio test -e native`: **114/114** (+23 over Phase 3: pairing lifecycle +
  SNTP-jump survival, SHA-1/base64 vectors, 11-type event matrix, seq/
  liveness/close codes + session-id charset, Mode B status provenance/
  freshness, app launch/quit lifecycle incl. failure/evidence-lost/deadline
  paths, openapi agent paths).
- Both firmware envs compile clean (S3 40.3 % flash; wroom 68.9 % of the new
  partition).
- **ESP32-S3 acceptance (2026-09-22, final Phase 4 binary), device
  mac-b53478.local:**
  - AT-01/AT-02 — all checks passed (real `unconfirmed`/`hid_only`).
  - AT-03/AT-04 — all checks passed **twice consecutively**; key/macros/
    triggers persist across the script's reboot (LittleFS key store verified
    on hardware).
  - AT-05 — all checks passed (error-table expectation updated to the 19
    closed codes; `ota_in_progress` correctly absent until Phase 6).
  - AT-06 — **all checks passed**: window lifecycle + 5-failure close + 1/s
    throttle (429), real MCA ceremony over the wire (`POST /agent/v1/pair` →
    token persisted once, never logged), WS hello/ack + capability_report
    flowing (mode B / L2, app_launch+app_quit available **and** verified),
    `connection.agent` true + `mac.state` fresh, late second pair → 409,
    bogus bearer → 401, mDNS TXT `mode=B pair=active`, token-leak check.
  - AT-11 — see below.
- Web UI Pairing tab: browser-checked (window open → code + countdown →
  ceremony completes in the MCA → state flips to Paired/Mode B; revoke
  returns to Mode A).

## Fixed / deferred to later phases

- Power-command predicates + expected-offline windows (AT-07–AT-09) → Phase 5.
- Monitored-app registry, allowlist/RBAC hardening (AT-10), OTA (AT-12),
  Ethernet → Phase 6.
- HTTPS still deferred (spec SHOULD); the MUST cleartext banner ships.
