# Phase 4 AT-11 gate — debug status (2026-09-23)

This document captures where Phase 4 stands, the stability work that consumed
most of the hardware bring-up, and the one remaining blocker: an AT-06
ceremony flake that has resisted five fix rounds. Read `docs/PHASE4.md` for
the feature/verification picture; this file is the working debug log.

## What is DONE and verified green

- Core library (114/114 native tests): pairing store, agent event envelope,
  session state machine, mode-aware status/capabilities, engine Mode B app
  launch/quit lifecycle, OpenAPI additions.
- MCA Python agent (`../agent/`): pairing, WS transport, backoff, polling
  fallback, closed action set, launchd plist.
- Web UI Pairing tab.
- **AT-01/AT-02, AT-03/AT-04 (twice), AT-05: green on hardware.**
- **AT-06: has passed end-to-end multiple times** (ceremony, Mode B evidence,
  mDNS TXT, token-leak check) — but is currently flaky (see below).
- **AT-11: every individual check has passed at least once** across runs
  (heartbeat-loss stale ~15 s/offline ~30 s with the monotonic-clock fix,
  reconnect backoff, WS dispatch → `completed/app_launch_confirmed`, polling
  fallback → identical ledger outcome, pending-endpoint probes). It has not
  yet completed two consecutive clean runs because the device underneath it
  kept dying — see the stability saga.

## The stability saga (root causes found and fixed — all flashed)

The device rebooted or wedged ~8 times during AT-11-length runs. Each cause
was found and fixed; the fixes compound:

1. **Serial backtrace #1**: `bad_alloc` in `ConfigStore::persistKeys` from the
   unthrottled `keys_dirty` drain. → Key persistence throttled ≥5 s,
   `try/catch` around all `loop()` drains.
2. **g_body retention**: one static response `std::string` grew to ~150 KB on
   the first `logs?limit=512` response and held it forever. → removed.
3. **512-entry heap string log ring**: ~128 KB heap tenant → idle free heap
   ~34 KB. → fixed-slot static ring. First attempt (512×256 B static) starved
   the heap arena itself ("Network Event Task Start Failed!", no Wi-Fi);
   settled on **128 entries × 224 B** (spec allows 128–2048; documented
   deviation). Idle heap now ~32 KB, min ~22 KB.
4. **Task-WDT abort on `tiT`**: the 1 s liveness timer called
   `engine.on_agent_offline()` (LittleFS under locks) inside the timer daemon
   task. → deferred to the dispatcher's 1 s sweep via `offline_pending_`.
5. **WiFiClient short writes**: `client.write` drops bytes on EAGAIN;
   `serializeJson(doc, client)` single-byte Print writes silently lose data
   (invalid JSON on the wire). → `writeFully()` everywhere; responses are
   exact-size transient strings.
6. **WiFiClient through a FreeRTOS queue**: `xQueueSend` memcpys the object,
   corrupting shared_ptr handle accounting. → mutex-protected deque +
   task notification.
7. **Transport churn wedges the network stack**: ~2 fresh TCP connections/s
   (urllib per request) wedged lwIP/Wi-Fi in ~60–90 s — the firmware stayed
   alive (CLI answered) but HTTP/mDNS died. → HTTP keep-alive support on the
   server (3 s idle bound between requests; the server is single-threaded)
   + all AT scripts now share `scripts/mc_http.py` (persistent connection).
8. **Log-slot truncation**: 176-byte slots truncated entries mid-JSON. →
   224-byte slots.

After all fixes: 5-minute sustained keep-alive polling soak = **no crashes,
no wedges**, heap stable. The stability war is won.

## THE STUCK ISSUE: AT-06 ceremony flake (wrong-code burst variant)

**Symptom**: in `scripts/at06.py`, steps 1–2b pass — window opens, 5-wrong-code
burst behaves correctly (403s, then 409 when the window closes), a fresh
window opens, and the device-side check `agent status` reports
`pairing state: pairing_window, 118s remaining, 0 failed attempts`. The MCA's
`POST /agent/v1/pair` then returns **409 `agent_not_paired`** ("PAIR FAILED:
pairing window closed or absent"). Immediately afterwards `agent status`
reports **`pairing state: active`** — i.e. a pairing record exists.

**Facts established**:
- The MCA posts exactly once (`post_pair` in `__main__.py`, no retry).
- The same flow **works when run manually** (serial window open with the port
  held open, then `curl` the pair → 200 + token).
- The failing variant differs only in the preceding 6-request wrong-code
  burst, which uses the keep-alive `mc_http` connection.
- The device log from the last failure shows **a fresh boot ~30 s into the
  at06 run** (`boot`/`pairing_restored` at 00:23:30, only boot-era entries in
  the ring) — so the DTR/RTS landmine (port close/hiccup → EN/IO0 toggle →
  reboot) fires MID-TEST despite the port being held open. A reboot between
  the serial window check and the HTTP POST explains everything: the
  118-second window is RAM-only and dies with the reset; the POST is served
  after reboot → 409; the `active` state afterwards is a **restored previous
  pairing** (pairing is synchronously persisted to NVS since the last fix,
  so old records resurrect across the reboot and mask the reset).
- Why the port held open still resets the device is unproven: candidates are
  a pyserial/USB transient that momentarily reasserts DTR/RTS, or macOS USB
  power management on the CH343. The reset is silent — pyserial sees no
  error; the only tell is device-side state vanishing.

**Fixes already applied for this** (not sufficient):
- `SerialSession` holds the port for the whole test (close = certain reboot).
- Wait-for-HTTP-up after session init (the port-open reset boots the device).
- 90 s daemon-connect window; post-ceremony `agent status` diagnostic.
- Pairing persisted synchronously in the pair route (NVS).

**Hypotheses not yet tested** (next steps, in order):
1. Instrument the device to log every reset reason at boot (`esp_reset_reason()`)
   to the RAM log — distinguish DTR/RTS reset (EXT/POWERON) from crash. Cheap
   and definitive for the reset itself.
2. In at06, run the ceremony POST **immediately** after the fresh window open
   (skip nothing) and re-verify `window_state()` <1 s before the POST; if the
   state flipped to `unpaired`, reopen the window and retry once — a bounded
   retry loop makes the test robust to one mid-test reset instead of fighting
   it.
3. Move the wrong-code burst to AFTER the real ceremony (spec order doesn't
   require the burst first; window #1 vs #2 sequencing is the test's own
   construction). This sidesteps the burst/keep-alive interaction entirely.
4. USB-capture (tcpdump on the CH343 via `sudo` — needs the user) to see the
   DTR/RTS toggle directly.
5. Firmware-side resilience (optional, spec-adjacent): allow a *just-closed*
   window a 5 s grace re-open via the same code — rejected for now (weakens
   the 5-failure rule; prefer fixing the harness).

**How to resume**:
- Keys on the device (re-provision if wiped again — check
  `GET /api/v1/status` with the READ key first):
  READ `mck_rgKsmf2TbsW5j_A_0t4-IxwXm7UHfHePWIKLnhRvIP4`,
  CONTROL `mck_D0zNN8OpljPahCV55mewdRpYO5M5Za9bxlakA3_4CLY`,
  ADMIN `mck_uQ2oLEpyrOtCYyXDFPwe-ErKesKm1Vap_f2Ppwwu8Hk`
- Device: `mac-b53478.local`, serial `/dev/cu.usbmodem5CBD0148591`.
- The device currently holds a leftover ACTIVE pairing from debugging; the
  agent state file `/tmp/mc_at_agent.json` does NOT match it — delete it and
  let at06 re-pair (ceremony revokes the incumbent).
- Run: `python3 scripts/at06.py --hostname mac-b53478.local --read-key …
  --control-key … --serial-port /dev/cu.usbmodem5CBD0148591` then
  `python3 scripts/at11.py …` twice consecutively (§16 rule).
- Working conventions that MUST be respected: monotonic clock for intervals;
  `writeFully` for all socket writes; never a FreeRTOS queue of WiFiClient;
  throttle persists; no fresh std::string in hot paths; kill stray MCA
  daemons (`pkill -f maccontrol_agent`) before every run — two daemons
  ping-pong supersede sessions and look like flakiness.

## Deviation note to fold into PHASE4.md when the gate passes

Log ring default 128 entries (spec default 512; range 128–2048) on the
ESP32-S3-DevKitC-1 N8 (320 KB RAM, no PSRAM): 512 × ~250 B leaves no heap
headroom for Wi-Fi under load. Log entries that exceed the 224-byte slot are
stored with an empty `detail` object (correlation IDs preserved).
