---
name: maccontrol
description: Control a MacControl ESP32 endpoint over its HTTP API — discover and trigger macros, send power commands (wake/sleep/restart/shutdown/lock), read status/capabilities/logs
type: prompt
whenToUse: When the user asks to trigger a macro, lock/wake/sleep/restart/shutdown a Mac, or query the MacControl device on the network
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

Use `$MACCONTROL_HOST` below for the base (default `http://mac-b53478.local`,
current lab unit: also reachable at `http://10.10.40.242`).

## 2. Authentication

Every `/api/v1/*` call needs an API key in the `Authorization` header:

```bash
curl -s -H "Authorization: Bearer $MACCONTROL_KEY" $MACCONTROL_HOST/api/v1/status
```

- Key format: `mck_` + base64url (issued by the device's ADMIN; the Web UI
  Security tab mints them, raw shown once). **Never invent a key — ask the
  operator for one.** A keys file may exist at `/tmp/mc_keys.env` on the
  provisioning Mac (READ_KEY/CONTROL_KEY/ADMIN_KEY) — check there first.
- Roles (strict order READ < CONTROL < ADMIN):
  - **READ** (default 60 req/min): GET status, capabilities, macros, commands, logs.
  - **CONTROL** (30 req/min): READ + execute macros + power commands.
  - **ADMIN** (10 req/min): + macro/trigger CRUD, identity, key management.
- Guard rails: wrong/missing key → 401 `unauthorized`; right key, wrong role →
  403 `forbidden`; **10 consecutive failed auths from one source IP locks that
  IP out for 60 s** — test with a valid key, never probe. Rate limit excess →
  429 `rate_limited` (retryable with backoff); 400/403/404/409 are permanent,
  do not retry unchanged.

## 3. Capabilities first

Before acting, read what the device says it can do **right now**:

```bash
curl -s -H "Authorization: Bearer $KEY" $MACCONTROL_HOST/api/v1/capabilities
```

In Mode A (no agent paired): `mode: "A"`, `capability_level: "L1"`, every
command has `verified: false`; `app_launch`/`app_quit` are `available: false`
(calling them → 409 `agent_not_paired`, no record created). Drive all behavior
from this document; never act on endpoints it doesn't list. Full contract:
`GET /api/v1/openapi.json`.

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

## 5. Power commands

```bash
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" \
  $MACCONTROL_HOST/api/v1/system/lock        # wake|sleep|restart|shutdown|lock
```

These are **real**: they HID-type Ctrl+Cmd+Q / power chords into the attached
Mac. `lock` and `sleep` are usually safe to demo; **`restart` and `shutdown`
interrupt whatever a person is doing on that Mac — always get explicit
confirmation first**. Same 202 → poll-the-record pattern as macros.

## 6. Status and logs

```bash
curl -s -H "Authorization: Bearer $KEY" $MACCONTROL_HOST/api/v1/status   # provenance-tagged tuples
curl -s -H "Authorization: Bearer $KEY" "$MACCONTROL_HOST/api/v1/logs?limit=20&category=command"
```

Logs (spec 15.2): ascending `seq`, filters `category` (command|session|auth|
config|ota|system), `level` (info|warn|error), `since_seq`, `limit` (max 512);
`dropped` counts entries overwritten by the 512-entry ring buffer.

## 7. Safety rules for agents

1. **The USB keyboard target is fixed**: keystrokes go to the Mac physically
   plugged into the ESP32, regardless of which machine sends the request.
2. **Confirm before any actuation** (macro execute, power command) unless the
   operator's request was unambiguous and explicitly named the action.
3. Never retry a 4xx; honor 429 with backoff. Never fire requests in a tight
   loop (flash wear on the ledger is a real constraint).
4. Transport is unencrypted HTTP on a trusted LAN: don't exfiltrate keys,
   don't send them anywhere off-network.
5. Admin-only mutations (macro CRUD, identity, keys) need an ADMIN key and
   should be treated as configuration changes — propose, then act on approval.
