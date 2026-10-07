# MacControlAgent (MCA)

On-Mac evidence provider for a MacControl endpoint (ESP32-S3). The MCA pairs
with the endpoint, reports macOS state over WebSocket (or an HTTP polling
fallback), and executes a closed action set (`launch_app` / `quit_app` for
allowlisted applications, plus the power actions `sleep` / `restart` /
`shutdown` in software) — protocol v2. It is **optional**: the endpoint
remains fully functional in Mode A (agentless) without it.

See `../maccontrol_spec.agent.final.md` for the normative protocol (chapters
4, 6, and 9 in particular).

## Install

Requires macOS with Python 3.9+. The installer builds the venv with the
**newest** Python it can find (python.org, Homebrew, or Xcode CLT, in that
order of preference) because the 3.9-compatible dependency wheels are fragile
on PyPI (pyobjc-core 12.0 is yanked; newer pyobjc/websockets require Python
3.10+). On a bare CLT-only Mac the venv falls back to the system Python 3.9
and `requirements.txt` resolves pyobjc 11.x / websockets 15.x via
`python_version` markers.

**One installer (flash + provision + pair + install).** With an ESP32-S3
connected over USB, a single run flashes the committed firmware bundle
(`../firmware/dist/esp32-s3`), sets WiFi + API keys + admin password over the
serial console, discovers the device on the network, opens the pairing
window, pairs this Mac, and finishes with the normal config + LaunchAgent
steps. The flashed firmware is stock: the device keeps its factory
`mac-<serial>` hostname, and WiFi is the network you provide during setup.

Run interactively and the installer asks whether to flash + set up an
endpoint (answering no — or passing `--no-flash` — gives an agent-only
install). After the credential prompts it asks you to connect the board's
'com' (CH343) cable and **reboot the board** before the serial console
phase — provisioning only works against a freshly booted board; one that
has been sitting plugged in may sit silent and fail. To script it end to
end, pass the flags explicitly:

```sh
cd agent
./install.sh --flash --provision \
    --allow com.apple.Terminal --enable-launch --enable-quit
```

Endpoint flags (all idempotent — a failed phase says what to fix and re-running
continues safely):

- `--flash` — flash the firmware bundle to an ESP32-S3, then stop (unless
  `--provision` is also given). Auto-detects the serial port
  (`/dev/cu.usbmodem*`, `/dev/cu.wchusbserial*`, `/dev/cu.usbserial*`; pick
  from a numbered list when several match, override with `--port P`). The
  board may reboot when the port opens (driver/cable dependent) — harmless
  either way.
- `--no-flash` — skip the interactive flash/provision offer (agent-only
  install when run interactively).
- `--provision` — full endpoint setup on an already-flashed board: serial
  phase (WiFi + keys + admin password), then network phase (discover via
  mDNS, open the pairing window via the Web UI API, run the agent pairing
  ceremony, verify the agent session comes up).
- `--wifi-ssid S` / `--wifi-pass P` / `--admin-password PW` — endpoint
  credentials (prompted interactively, echo off, when missing; the admin
  password must be 10+ chars). Secrets are never printed.
- `--no-keys` — skip API key creation (serial phase does WiFi + admin only).
- `--re-pair` — required when `~/.maccontrol/agent.json` already holds a
  pairing token and you want the network phase to replace it.

Prerequisite for the serial phases: the WCH CH34x/CH343 USB-serial driver
(https://www.wch.cn/downloads/CH341SER_MAC_ZIP.html) — without it no
`/dev/cu.wchusbserial*` port appears. Flashing/provisioning use `esptool` and
`pyserial`, installed into `agent/.venv` on demand.

Key output: the three endpoint API keys (READ/CONTROL/ADMIN) are printed
exactly once at install time, written to `~/.maccontrol/endpoint.keys`
(chmod 600, `ENDPOINT_HOSTNAME=` + `READ_KEY=`/`CONTROL_KEY=`/`ADMIN_KEY=`),
and never stored anywhere else. The raw keys are shown once by the device and
never logged; if the key store is full (8 active), revoke old ones over the
serial console (`key list` / `key revoke <key_id>`) or use `--no-keys`.

If the device crashes/reboots mid-phase (task-watchdog under load — known on
factory-fresh devices, see `../firmware/docs/DEBUG-PROVISION-TWDT.md`), the
serial phase detects the boot banner and retries automatically (up to 3
attempts); a command the device replayed from its serial buffer may spend an
extra key slot, which is harmless.

The provisioner itself is `mc_provision.py` (stdlib + pyserial, driven by
install.sh); its `serial` and `network` subcommands are independently
runnable for debugging — see `./mc_provision.py --help`.

**Agent-only install** (endpoint already provisioned): the installer creates
the venv (recreating it automatically if a copied/broken `.venv` is found),
installs dependencies, optionally pairs, writes your configuration, and
installs a LaunchAgent so the agent starts at login:

```sh
cd agent
./install.sh --hostname maccontrol-01 --pair-code ABC234XY \
    --allow com.apple.Terminal com.apple.Safari \
    --enable-launch --enable-quit
```

Re-running `./install.sh` reconfigures the agent and restarts the LaunchAgent;
`./install.sh --help` lists all options (allowlist, action enable/disable,
transport, headless, uninstall). The steps below remain for manual setups.

**Actions are fail-closed disabled by default** (including the Mode B power
actions the endpoint's Web UI buttons trigger). Enable what this Mac should
allow — for a full AV-control endpoint typically:

```sh
./install.sh --enable-launch --enable-quit \
    --enable-sleep --enable-restart --enable-shutdown
```

Each action also has a `--disable-<action>` form; power actions are executed
by the agent in software (`pmset` / System Events), not via HID chords.

**Scheduled power-on failsafe:** the installer also sets
`sudo pmset repeat poweron MTWRFSU 06:00:00` (once, when no repeat schedule
exists) so a Mac that got shut down is back on at 6:00 AM without human
intervention. It deliberately uses `poweron` rather than `wakeorpoweron`: a
sleeping Mac is left alone — wake those over HID from the endpoint's Web UI
or API (`POST /api/v1/system/wake`). The event lives in the real-time clock,
so it works from a full shutdown with nothing running on the Mac. It needs
sudo: with passwordless sudo or an interactive terminal it applies directly,
otherwise the installer prints the exact command to run by hand. Change the
schedule with `--poweron "MTWRF 07:30:00"` (forces a replacement) or remove
it with `--no-poweron`. Pair this with `sudo pmset -a autorestart 1` (start
up after power loss) for full coverage; note that neither recovers a hung
machine — only a power relay does.

**Flags added for this client-install round** (all re-runnable;
`./install.sh --help` has one-line forms of everything):

- `--no-flash` — skip the interactive "flash an ESP32-S3 now?" offer
  (agent-only install; the offer only appears on an interactive terminal
  with no endpoint flags given).
- `--enable-sleep` / `--disable-sleep`, `--enable-restart` /
  `--disable-restart`, `--enable-shutdown` / `--disable-shutdown` — the Mode B
  power actions the endpoint's Web UI buttons trigger. Fail-closed disabled
  by default; executed by the agent in software (`pmset` / System Events).
  Typical AV endpoint: `./install.sh --enable-sleep --enable-restart
  --enable-shutdown --enable-launch --enable-quit`.
- `--poweron "DAYS HH:MM:SS"` / `--no-poweron` — the scheduled power-on
  failsafe described above (`DAYS` = MTWRFSU subset, e.g. `MTWRFSU`).

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
| `enabled_commands` | `{"launch_app": false, "quit_app": false, "sleep": false, "restart": false, "shutdown": false}` — fail-closed defaults |
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
--enable-sleep          enable the sleep power action (pmset sleepnow)
--enable-restart        enable the restart power action (osascript System Events)
--enable-shutdown       enable the shutdown power action (osascript System Events)
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
(lock state is reported as undetectable) and `front_app_changed` is skipped.
Sleep/wake does NOT use pyobjc: it is IOKit system power notifications via
ctypes (power.py) — the daemon-grade mechanism (Mach port on a CFRunLoop,
delivered pre-sleep, no LaunchServices/distributed-notification dependency),
needing nothing installed. App detection does NOT use pyobjc either: it is
psutil-primary (exe-path prefix match against the mdfind-resolved .app path),
falling back to sparse `mdfind`/`pgrep` polling only when psutil is missing.

## Transport fallback

- **WebSocket** (default): `ws://<hostname>:80/agent/v1/ws` with
  `Authorization: Bearer <agent_token>`; `agent_hello` then `hello_ack`
  (5 s timeout). Close-code handling: `1000/1001/4002`/network loss → backoff
  loop (1/2/4/8/30 s + 0–20 % jitter, forever at the 30 s cap); `1008/4001/4003`
  → HALTED (stops reconnecting; exit code 2). A `4002` reconnect opens a new
  session with `seq` reset. During a declared expected-offline window
  (agent_goodbye for sleep/restart/shutdown) the agent holds reconnects until
  `close_after_s` (default 60 s).
  **Caution:** the retry loop logs *any* failure between socket open and
  hello_ack — DNS errors, connect timeouts, upgrade hangs — as
  `hello_ack timeout; backing off`; only an HTTP-status refusal gets its own
  message. Seeing that line on every attempt means "check name resolution
  and TCP reachability first" (a stale mDNS hostname after a device rename
  produces exactly this; see `firmware/docs/DEBUG-MDNS-STALE-NAME.md`).
- **Polling** (`--transport polling`): `POST /agent/v1/events` with the
  identical envelope (first `agent_hello` returns `session_id`; every
  subsequent request carries `X-Session-Id`) and
  `GET /agent/v1/commands/pending` every `poll_interval_s`. Error mapping:
  `401` → backoff, `403` → halted (terminal), `409 agent_not_paired` → backoff,
  `409 agent_offline` → open a new session, `400 validation_failed` → backoff.
  Switching transports mid-session is prohibited; a switch starts a new session.

## Endpoint-side scope / known gaps (Phase 4.5 + Phase 5)

- **Sleep/wake detection (Phase 4.5, IOKit rewrite 2026-09-23)**: power.py
  registers for IOKit system power notifications via ctypes
  (`IORegisterForSystemPower`, Mach notification port on a dedicated
  thread's CFRunLoop) and emits `system_state_changed`
  `sleeping` / `waking` / `awake` deltas, declaring the sleep an
  expected-offline via the goodbye path so a commanded sleep reads as
  `expected_offline`, not fault. Pre-sleep frames are guaranteed enqueued
  before `IOAllowPowerChange` (bounded cross-thread handshake). The earlier
  NSWorkspace-based observer never fired in the headless process (no
  NSRunLoop) and was removed. Needs nothing installed — pure ctypes — but
  any setup failure degrades to a logged no-op, agent unaffected. As a
  safety net, the Telemetry loop also detects wakes from clock divergence
  (Mac uptime counts sleep, process monotonic does not) and emits `waking`
  if the IOKit path missed an episode; it deliberately never fabricates a
  retroactive `sleeping`/goodbye — pre-sleep declaration is only possible
  from the IOKit path.
- **Composite heartbeat (spec 6.3.1)**: heartbeats now carry `boot_time`,
  Mac `mac_uptime_s`, `cpu_utilization_pct`, `memory_utilization_pct`,
  `disk_free_bytes`, and a `network` `{reachable, ip}` object (routing-table
  check only — no outbound probe traffic). Samples are taken at heartbeat
  emission time via psutil, with `vm_stat` / `df` / `top` subprocess
  fallbacks; any sample that fails comes through as `null` rather than being
  omitted (spec 9 honesty). `capability_report` additionally carries
  `hardware_model` (`sysctl hw.model`, cached per process).
- **Foreground app tracking** (Phase 4.5 B1): `front_app_changed` deltas
  (frontmost bundle ID via NSWorkspace) on the 3 s monitor cadence, first
  detection included; deltas only, never in the initial burst. Requires
  pyobjc; silently skipped without it. Relies on NSWorkspace and may lag in
  headless (no NSRunLoop) contexts — best-effort, unlike app detection.
- **Without pyobjc**, screen-lock detection and `front_app_changed` are
  unavailable and load samples depend on the subprocess fallbacks above
  (sleep/wake is unaffected — it uses ctypes/IOKit, no pyobjc needed).
  **Without psutil**, app state detection degrades to best-effort
  `mdfind`/`pgrep` polling (misses are possible). Install
  `pyobjc-framework-Quartz` / `pyobjc-framework-Cocoa` (and `psutil`) into
  the venv for the full experience.
- **Agent-executed power actions (protocol v2, 2026-09-27)**: `sleep`,
  `restart`, and `shutdown` are executed by the agent in software — modern
  macOS ignores USB-HID system-sleep/power chords (a HID System Control
  report enumerates and parses but is ignored; the old Cmd+Alt+Power chord
  only display-sleeps). Commands are `pmset sleepnow` (sleep) and
  `osascript -e 'tell application "System Events" to restart|shut down'`
  (restart/shutdown), each fail-closed behind its `enabled_commands` flag
  (`--enable-sleep` / `--enable-restart` / `--enable-shutdown`, default off)
  and a subprocess timeout. Wake and lock stay on the HID path — both proven
  against a real Mac. Ordering inside the power handler is fixed:
  (1) `command_ack`; (2) `command_result(ok)` best-effort — reported BEFORE
  acting because the power transition may kill the process, and safe because
  `command_result(ok)` never completes an endpoint record (only evidence
  does); (3) the expected-offline `agent_goodbye{reason}` with the matching
  reason (`sleep`/`restart`/`shutdown`), flushed ahead of the power command
  via the transport's outbox join (WebSocket) / synchronous event-POST drain
  (polling); (4) execution. Residual risk: restart/shutdown may outrun the
  flush — the host can halt before the peer reads the goodbye frame. That is
  acceptable: the endpoint's evidence channel also sees the TCP close, which
  qualifies the same expected-offline window, so the record completes from
  window evidence either way; the declared reason simply makes the window
  deterministic when the flush lands. If initiation fails (host stays up,
  nonzero rc) a superseding `command_result(failed, action_timeout)` is sent.
  Power dispatches carry `{action, command_id}` only — no `bundle_id` — and
  the allowlist does not apply to them.
- **Endpoint Phase 5 (2026-09-27, firmware-side — no agent changes needed)**:
  the endpoint now *verifies* power/lock/wake and `expected_event` macros in
  Mode B using ambient frames the MCA already emits — `screen_lock_changed`
  (lock), new-session `agent_hello` boot_id + `system_state_changed{awake}`
  (wake, restart), in-window offline + silence through window close (sleep),
  in-window offline + ICMP-unreachability corroboration (shutdown), and the
  five ambient event types (macros). Records terminate
  `completed`/`*_confirmed` or the honest negatives `unexpected_wake` /
  `unexpected_reconnect` / `host_still_reachable`; terminal records are never
  reopened by later contradictory events. Two known asymmetries, both
  acceptable to the protocol: (1) the agent cannot distinguish a *commanded*
  sleep from a user-initiated sleep — both declare `reason: "sleep"` (a
  pending-command marker in the agent is optional future hardening); (2)
  pre-v2 the `restart`/`shutdown` goodbye reasons were never emitted — with
  protocol v2 the agent declares them before executing a commanded
  restart/shutdown, making the expected-offline window deterministic (the
  boot_id-based verification remains as the fallback when the goodbye flush
  is outrun).
- **MCA UI is minimal** (status + code entry + log view). The full 5-view UI
  of spec chapter 14 is a later phase; command enablement and the allowlist
  are currently CLI/state-file only.
- `--show-log` reads the bounded `~/.maccontrol/agent.log` written alongside
  the state file (the live 500-entry ring buffer is in-process).
