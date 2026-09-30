---
name: maccontrol
description: Control a MacControl ESP32 endpoint over its HTTP API — discover and trigger macros, send power commands (wake/sleep/restart/shutdown/lock/unlock), launch/quit allowlisted apps via the paired agent, read status/capabilities/agent telemetry/logs, detect host sleep/offline/login-screen
type: prompt
whenToUse: When the user asks to trigger a macro, lock/wake/sleep/restart/shutdown a Mac, launch or quit an app on the paired Mac, query the MacControl device or its agent telemetry, or check whether the managed Mac is awake/asleep/offline
arguments:
  - action
---

# MacControl Endpoint Operator's Guide

MacControl is a deterministic ESP32 Mac controller on the local network. It
presents a closed REST API (`/api/v1/*`, plain HTTP, ArduinoJson envelopes)
plus an embedded Web UI. This skill teaches an agent to discover the device,
authenticate, enumerate what it can do, and act safely.

## 1. Discovery

The endpoint advertises `_maccontrol._tcp` over mDNS as `<hostname>.local`
(hostname is `mac-` + last 6 hex of the device MAC, e.g. `mac-b53478.local`).

- macOS/Linux: resolve `<hostname>.local` directly (Linux needs avahi).
- Windows 10/11: `.local` usually resolves; otherwise use the device's IPv4.
- Fallback if mDNS fails: ask the operator for the IP, or browse:
  `dns-sd -B _maccontrol._tcp local`

Use `$MACCONTROL_HOST` below for the base. **Current lab unit (renamed
2026-09): `http://control-graphics.local` (also `http://10.10.40.242`).**
The old name `mac-b53478.local` is a stale mDNS cache entry that macOS
intermittently resolves to a dead/wrong IP — do not use it; check
`/api/v1/status → device.hostname` for ground truth if a name stops
resolving (see firmware/docs/DEBUG-MDNS-STALE-NAME.md).

## 2. Authentication

Every `/api/v1/*` call needs an API key in the `Authorization` header:

```bash
curl -s -H "Authorization: Bearer $MACCONTROL_KEY" $MACCONTROL_HOST/api/v1/status
```

- Key format: `mck_` + base64url (issued by the device's ADMIN; the Web UI
  Security tab mints them, raw shown once with a Copy button). **Never invent
  a key — ask the operator for one.** A keys file may exist at
  `/tmp/mc_keys.env` on the provisioning Mac (READ_KEY/CONTROL_KEY/ADMIN_KEY)
  — check there first.
- Roles (strict order READ < CONTROL < ADMIN):
  - **READ** (default 60 req/min): GET status, capabilities, agent/status,
    macros, commands, logs.
  - **CONTROL** (30 req/min): READ + execute macros + power commands +
    app launch/quit.
  - **ADMIN** (10 req/min): + macro/trigger CRUD, identity, key management.
- Guard rails: wrong/missing key → 401 `unauthorized`; right key, wrong role →
  403 `forbidden`; **10 consecutive failed auths from one source IP locks that
  IP out for 60 s** — test with a valid key, never probe. Rate limit excess →
  429 `rate_limited` (retryable with backoff); 400/403/404/409 are permanent,
  do not retry unchanged. Poll records no faster than once every 2–3 s — a
  1/s poller rides the READ bucket's ceiling and starts seeing 429s.

## 3. Capabilities first

Before acting, read what the device says it can do **right now**:

```bash
curl -s -H "Authorization: Bearer $KEY" $MACCONTROL_HOST/api/v1/capabilities
```

- **Mode A** (no agent paired): `mode: "A"`, `capability_level: "L1"`, every
  command has `verified: false`; `app_launch`/`app_quit` are
  `available: false` (calling them → 409 `agent_not_paired`, no record
  created). Drive all behavior from this document; never act on endpoints it
  doesn't list. Full contract: `GET /api/v1/openapi.json`.
- **Mode B** (agent paired): `capability_level: "L2"` while paired, **"L3"
  while an agent session is live** (verified automation, Phase 5).
  `app_launch`/`app_quit` report `available: true` and `verified: true` with
  a live session. Power/lock/macro commands are dispatchable and — with a
  live session — verifiable: their `verified` flags track the connection and
  their records can terminate honestly `completed` (see §5). Everything
  collapses back to L2/all-false within the 30 s offline threshold.

## 4. Macros (the main actuation surface)

```bash
# Enumerate macros (READ)
curl -s -H "Authorization: Bearer $KEY" $MACCONTROL_HOST/api/v1/macros
# -> {"macros":[{"macro_id":"mac_XXXX","name":"...","revision":1,"steps":[...]}]}

# Execute by id (CONTROL) — always confirm with the operator first; this types
# real keystrokes into the Mac the ESP32's USB port is attached to.
curl -s -X POST -H "Authorization: Bearer $KEY" \
  $MACCONTROL_HOST/api/v1/macros/mac_XXXX/execute
# -> 202 {"command_id":"...","state":"accepted","record_url":"..."}

# Follow the ledger record to its terminal state (READ)
curl -s -H "Authorization: Bearer $KEY" \
  $MACCONTROL_HOST/api/v1/commands/COMMAND_ID
```

Pick the macro by matching `name` against what the operator asked for; if
ambiguous, list candidates and ask. After executing, poll the record URL until
terminal (`unconfirmed`, `failed`, or `timed_out`).

**Mode A semantics (important):** a successfully dispatched macro ends
`unconfirmed` with `result: "hid_only"` — that is the *correct* verdict, not an
error. `completed` is impossible in Mode A and would indicate a bug. Terminal
failures to expect honestly: `dispatch_error`/`usb_disconnected` (HID link
down), `macro_timeout`.

**Mode B (Phase 5):** a macro defined with an `expected_event` (restricted to
the five ambient event types — anything else is rejected at macro
create/update) stays `confirming` after dispatch and terminates
`completed`/`macro_confirmed` when the matching event arrives within the
deadline (macro timeout + 5 s). A macro without `expected_event` still ends
`unconfirmed`/`hid_only` even in Mode B — honest, not an error.

## 5. Power commands

```bash
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" \
  $MACCONTROL_HOST/api/v1/system/lock        # wake|sleep|restart|shutdown|lock|unlock
```

These are **real**: they HID-type Ctrl+Cmd+Q / power chords into the attached
Mac. `lock` and `sleep` are usually safe to demo; **`restart` and `shutdown`
interrupt whatever a person is doing on that Mac — always get explicit
confirmation first**. Same 202 → poll-the-record pattern as macros.

**Unlock / Wake & Log In (device-side password, 2026-09-30 amendment):**
`POST /api/v1/system/unlock` types a password stored ON THE DEVICE at the
lock/login screen (device HID-types it + Enter). It needs no agent session
(the agent is offline at the login window) and completes on the agent's
`screen_lock_changed {locked:false}` evidence (`unlock_confirmed`, 60 s
deadline); without a paired agent it ends `unconfirmed`/`hid_only`. Check or
manage the stored password (ADMIN role):

```bash
curl -s -H "Authorization: Bearer $READ_KEY"  $MACCONTROL_HOST/api/v1/system/unlock_password   # {"set":true|false} — value NEVER returned
curl -s -X PUT -H "Authorization: Bearer $ADMIN_KEY" -H 'Content-Type: application/json' \
  -d '{"password":"..."}' $MACCONTROL_HOST/api/v1/system/unlock_password   # 1-64 printable ASCII
curl -s -X DELETE -H "Authorization: Bearer $ADMIN_KEY" $MACCONTROL_HOST/api/v1/system/unlock_password
```

Full unattended flow (what the Web UI's "Wake & Log In" button does):
`POST /api/v1/system/wake` → poll `GET /api/v1/agent/status` until
`session_active:true` and `user.screen_locked:true` (the password prompt;
max ~75 s) → `POST /api/v1/system/unlock`. `GET /api/v1/status` also exposes
a synthesized `mac.login_screen` boolean (true only while the agent is live
and reports the screen locked). Never put the password in a macro's text
steps — macro definitions are readable with a READ key.

Lifecycle notes (Phase 4.5+): sleep/restart/shutdown records carry an
`expected_offline_window` (`{open_after_s: 3, close_after_s: 60}`), and when a
paired agent declares the outage (its `agent_goodbye` with reason
sleep/restart/shutdown), other in-flight confirming records inherit that
window — the offline sweep then classifies the loss `expected_offline` instead
of `evidence_lost`. A `sleep` the agent *didn't* declare reads as an ordinary
offline.

Lifecycle notes (Phase 5+): with a live agent session these commands stay
`confirming` after dispatch and terminate honestly —
`lock` → `completed`/`lock_confirmed` on the agent's `screen_lock_changed`
(15 s deadline);
`sleep` → `sleep_confirmed` when the window closes with the host still
offline;
`restart` → `restart_confirmed` on a reconnect whose `boot_id` **changed**
plus an awake burst (180 s deadline; an unchanged `boot_id` voids the
evidence and the record ends `timed_out`);
`shutdown` → `shutdown_confirmed` when the window closes, the host answers
none of 3 ICMP probes, and it stays dark (120 s).
Refuting evidence is honest too: reconnect before a sleep closes →
`failed`/`unexpected_wake`; reconnect during a shutdown →
`failed`/`unexpected_reconnect`; a shutdown host still answering probes →
`failed`/`host_still_reachable`. In Mode B, submitting `lock`/`sleep`/
`restart`/`shutdown` while no agent session is live → 409 `agent_offline`
(no record created). `wake` is exempt — the Mac is asleep and the session is
necessarily down at wake time — and completes only on a **post-dispatch new
session** whose initial burst reports `awake`; a heartbeat alone never
completes it.

## 6. App launch/quit (Mode B, via the paired agent)

```bash
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" \
  $MACCONTROL_HOST/api/v1/apps/com.apple.TextEdit/launch    # or /quit
# -> 202 {"command_id":"...","state":"accepted",...}
```

- Only allowlisted bundle IDs work; anything else → 409/`forbidden`-class
  refusal or `failed/app_not_allowlisted` on the record. Enumerate candidates
  from `agent.status.applications` (below) or the capabilities doc.
- With a live agent these terminate **honestly completed**:
  `result: "app_launch_confirmed"` / `"app_quit_confirmed"` (verified via the
  agent's ack + process observation). Agent offline or quit refused →
  `timed_out`/`failed` with the closed error codes (`app_not_running`,
  `quit_failed`, `action_timeout`, ...).
- Mode A: 409 `agent_not_paired`, no record created.

## 7. Agent status & telemetry (Mode B)

```bash
curl -s -H "Authorization: Bearer $READ_KEY" $MACCONTROL_HOST/api/v1/agent/status
```

The composite MCA report (spec 6.3, Phase 4.5 complete). Mode A → 409
`agent_not_paired`. Shape (every leaf is **null when the evidence is absent —
never invented**, spec 9 honesty):

```json
{
  "agent_instance_id": "ag-4d00",
  "boot_id": "b_578FC7",            // changes iff the MAC rebooted
  "reported_at": "...Z",
  "session_id": "s_XXXX",           // null when no live session
  "session_active": true,
  "system":  {"state": "awake"},    // awake|sleeping|waking|shutting_down|restarting|booting
  "user":    {"logged_in": true, "name": "danieljohnson", "screen_locked": false},
  "applications": {"com.apple.TextEdit": {"running": true, "pid": 12345}},
  "system_info": {
    "cpu_utilization_pct": 37.6,    // live samples, move every heartbeat
    "memory_utilization_pct": 74.1,
    "disk_free_bytes": 59629400064,
    "network": {"reachable": true, "ip": "10.10.40.99"},
    "uptime_s": 6069516,            // MAC uptime seconds (not the agent's!)
    "boot_time": 1784123335,        // Mac boot, epoch seconds
    "os_version": "15.7.4",
    "hardware_model": "Mac16,9",
    "front_app": "com.wiheads.paste"  // frontmost bundle id (may lag headless)
  },
  "usb": {"link": "up", "state": "mounted", "changed_at": "...Z"}
}
```

Reading it:
- **"Is the Mac awake?"** → `system.state` + `session_active`.
- **"Did the Mac reboot?"** → compare `boot_id` across calls; a restart also
  shows as `system.state: "booting"` then `awake` with a new `boot_id`.
- **"What's it doing?"** → `front_app`, `applications`, load samples.
- Caution: the agent's heartbeat wire field `uptime_s` is the *agent process*
  uptime; only this report's `system_info.uptime_s` is Mac uptime.

## 8. Host-outage detection (USB link)

The endpoint watches its own USB device link to the Mac, so it can tell a
sleeping/off Mac from a network fault (Phase 4.5 addendum). Log events in
`category=system`: `usb_attached` / `usb_suspended` / `usb_resumed` /
`usb_detached`; live state in `agent.status.usb` (`link: "up"` only while
`state: "mounted"`; suspended/detached = host not signaling). Classification
table for consumers:

| Observation | Conclusion |
|---|---|
| Declared `agent_goodbye` (sleep/restart/shutdown) in the logs | expected_offline — records inside the window survive the offline sweep |
| USB link down + agent silence + **same** `boot_id` on return | host slept |
| USB link down + agent returns with a **new** `boot_id` | host restarted / power-cycled |
| Agent silence while USB link stays **up** | agent or network fault (the endpoint is reachable, the agent isn't) |

The USB layer cannot distinguish sleep from power-off from unplug by
construction — sleep-vs-restart comes from `boot_id`, commanded outages from
the declared goodbye. During an open declared window, `/api/v1/status`
agent-derived tuples (`mac.*`, `applications.*`) render
`freshness: "expected_offline"` instead of going `stale`, and
`connection.agent` reads `false` with `expected_offline` freshness; when the
window closes without a reconnect, normal stale aging resumes (Phase 5, spec
7.2.2).

## 9. Status and logs

```bash
curl -s -H "Authorization: Bearer $KEY" $MACCONTROL_HOST/api/v1/status   # provenance-tagged tuples
curl -s -H "Authorization: Bearer $KEY" "$MACCONTROL_HOST/api/v1/logs?limit=20&category=command"
```

Logs (spec 15.2): ascending `seq`, filters `category` (command|session|auth|
config|ota|system), `level` (info|warn|error), `since_seq`, `limit`; `dropped`
counts entries overwritten by the RAM ring. **This S3 unit ships a 128-entry
ring** (spec default is 512; the 320 KB part can't spare the heap) — long
soaks wrap quickly, so page with `since_seq` rather than big `limit`s. Useful
filter pairs: `category=system` (boot/reset/usb/heap events — read `boot`'s
`reset` field to classify reboots: `poweron` vs watchdog/abort),
`category=command&limit=…` (agent evidence trail),
`category=session` (agent connect/offline history).

## 10. Safety rules for agents

1. **The USB keyboard target is fixed**: keystrokes go to the Mac physically
   plugged into the ESP32, regardless of which machine sends the request.
2. **Confirm before any actuation** (macro execute, power command, app
   quit — quit interrupts the person's app) unless the operator's request was
   unambiguous and explicitly named the action.
3. Never retry a 4xx; honor 429 with backoff; poll no faster than once every
   2–3 s (see §2). Never fire requests in a tight loop (flash wear on the
   ledger is a real constraint).
4. Transport is unencrypted HTTP on a trusted LAN: don't exfiltrate keys,
   don't send them anywhere off-network.
5. Admin-only mutations (macro CRUD, identity, keys) need an ADMIN key and
   should be treated as configuration changes — propose, then act on approval.
6. **Keystroke-bearing endpoints require a live USB link**: if
   `agent.status.usb.link` is `down`, the attached Mac is asleep/off/unplugged
   — HID commands will terminate `usb_disconnected`/`unconfirmed`; wake the
   Mac (§5 `wake`) rather than retrying.
