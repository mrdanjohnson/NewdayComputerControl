# MacControlAgent (MCA)

On-Mac evidence provider for a MacControl endpoint (ESP32-S3). The MCA pairs
with the endpoint, reports macOS state over WebSocket (or an HTTP polling
fallback), and executes a closed action set (`launch_app` / `quit_app`) for
allowlisted applications. It is **optional**: the endpoint remains fully
functional in Mode A (agentless) without it.

See `../maccontrol_spec.agent.final.md` for the normative protocol (chapters
4, 6, and 9 in particular).

## Install

Requires macOS with Python 3.9+ (system python3 is fine).

```sh
cd agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Run once in the foreground to pair and verify:

```sh
.venv/bin/python -m maccontrol_agent --hostname maccontrol-01 --pair-code ABC234XY
# -> PAIRED maccontrol-01
```

Then install the LaunchAgent (edit the `CHANGE_ME` paths in the plist first):

```sh
cp launchd/com.maccontrol.agent.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.maccontrol.agent.plist
# launchctl unload ~/Library/LaunchAgents/com.maccontrol.agent.plist   # to remove
```

The LaunchAgent runs `python -m maccontrol_agent --daemon` with
`RunAtLoad` + `KeepAlive` (restart throttling via `ThrottleInterval 10`).

## Pairing flow

1. An ADMIN opens the pairing window on the ESP32 Web UI; it shows an
   8-character code (alphanumeric, no `0/O/1/I`), valid for ~120 s.
2. On the Mac:
   ```sh
   python -m maccontrol_agent --hostname <endpoint-hostname> --pair-code CODE
   ```
   The agent POSTs `/agent/v1/pair` with `{"pairing_code", "agent_instance_id",
   "agent_version"}`; on 200 it persists `{hostname, agent_instance_id,
   agent_token}` to the state file and prints `PAIRED <hostname>`.
3. Errors: `400 validation_failed`, `409 agent_not_paired` (window closed or
   absent), `429 rate_limited` (>1 attempt/s). Five wrong codes close the
   window server-side.

Pairing can also be done from the UI window (code entry field), or
programmatically with `--daemon` to pair *and* keep serving.

## Configuration

State file: `~/.maccontrol/agent.json` (chmod 600). Keys:

| Key | Meaning |
|---|---|
| `hostname` | paired endpoint hostname (bare DNS label; `.local` never stored) |
| `agent_instance_id` | `ag-XXXX`, generated once at first run, persistent |
| `agent_token` | base64url 32-byte bearer token, returned exactly once at pairing |
| `allowlist` | bundle IDs permitted for launch/quit/report (empty = deny all) |
| `enabled_commands` | `{"launch_app": false, "quit_app": false}` — fail-closed defaults |
| `transport` | `websocket` (default) or `polling` |
| `poll_interval_s` | polling fallback interval, 2–30 (default 5) |
| `boot_id` / `boot_key` | cached boot identity (`kern.boottime`-derived) |

CLI flags (persisted to the state file where applicable):

```
--hostname H            endpoint hostname
--pair-code CODE        complete the pairing ceremony, then exit 0
--daemon                with --pair-code: pair, then keep serving
--transport websocket|polling
--poll-interval S       2–30, default 5
--allow BUNDLE_ID ...   replace the application allowlist
--enable-launch         enable the launch_app action
--enable-quit           enable the quit_app action
--state-file PATH       alternate state file location
--headless              no UI; requires prior pairing or --pair-code
--show-log              print the recent log (~/.maccontrol/agent.log) and exit
```

## Permissions

No elevated entitlements. The agent runs as the logged-in console user, opens
**no** listening socket, and sends only outbound traffic to the paired
endpoint. App launch/quit use `open -b <bundle_id>` and
`osascript -e 'tell application id "<bundle_id>" to quit'`; the bundle ID is
validated against `^[A-Za-z0-9.-]+$` before any interpolation.

Screen-lock detection uses the well-known `CGSessionCopyCurrentDictionary`
snippet via pyobjc (`pip install pyobjc-framework-Quartz` into the venv). **Without
pyobjc the agent still runs**, but `screen_lock_changed` events are not emitted
(lock state is reported as undetectable) and app monitoring falls back to
sparse `mdfind`/`pgrep` polling.

## Transport fallback

- **WebSocket** (default): `ws://<hostname>:80/agent/v1/ws` with
  `Authorization: Bearer <agent_token>`; `agent_hello` then `hello_ack`
  (5 s timeout). Close-code handling: `1000/1001/4002`/network loss → backoff
  loop (1/2/4/8/30 s + 0–20 % jitter, forever at the 30 s cap); `1008/4001/4003`
  → HALTED (stops reconnecting; exit code 2). A `4002` reconnect opens a new
  session with `seq` reset. During a declared expected-offline window
  (agent_goodbye for sleep/restart/shutdown) the agent holds reconnects until
  `close_after_s` (default 60 s).
- **Polling** (`--transport polling`): `POST /agent/v1/events` with the
  identical envelope (first `agent_hello` returns `session_id`; every
  subsequent request carries `X-Session-Id`) and
  `GET /agent/v1/commands/pending` every `poll_interval_s`. Error mapping:
  `401` → backoff, `403` → halted (terminal), `409 agent_not_paired` → backoff,
  `409 agent_offline` → open a new session, `400 validation_failed` → backoff.
  Switching transports mid-session is prohibited; a switch starts a new session.

## Phase 4 scope / known gaps

- **Sleep/wake OS notifications are Phase 5+**: the agent cannot yet observe
  `system_state_changed` transitions from the OS, so it always reports
  `awake` while running. `Runtime.declare_expected_offline()` is wired
  (goodbye + reconnect hold until `close_after_s`) but nothing triggers it
  until OS power notifications land.
- **Without pyobjc**, app state detection degrades to best-effort
  `mdfind`/`pgrep` polling (misses are possible) and screen-lock detection is
  unavailable. `osascript`-based probing is used sparingly; install
  `pyobjc-framework-Quartz`/`pyobjc-framework-Cocoa` for the full experience.
- **MCA UI is minimal** (status + code entry + log view). The full 5-view UI
  of spec chapter 14 is a later phase; command enablement and the allowlist
  are currently CLI/state-file only.
- `--show-log` reads the bounded `~/.maccontrol/agent.log` written alongside
  the state file (the live 500-entry ring buffer is in-process).
- Heartbeat/system sampling extras (CPU, memory, disk, network of spec 6.3.1's
  composite report) are ESP32-assembled from events; the agent emits the
  Chapter 6 event stream only.
