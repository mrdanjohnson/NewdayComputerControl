# MacControl — project README

MacControl is a deterministic ESP32-based controller for a Mac. An ESP32-S3
sits on the Mac's USB (as a HID keyboard) and on the local Wi-Fi (as a
self-contained HTTP/WebSocket endpoint); a small Python agent runs on the
Mac and reports ground truth over WebSocket. Together they give scripts,
a Web UI, and physical triggers a single closed API for launching and
quitting apps, typing keystrokes, running macros, locking the screen —
with every command durably recorded in an append-only ledger and every
reported value carrying provenance and freshness.

The normative protocol and rationale are
`MacControl_Deterministic_Architecture_Spec.md` (chapters cited as §N
below). Phase status and bring-up records live in `firmware/docs/`.

## The two components

### 1. The endpoint (ESP32 firmware, `firmware/`)

PlatformIO project, Arduino-ESP32 core 2.0.17, primarily targeted at the
ESP32-S3-DevKitC-1 (N8, 320 KB RAM, no PSRAM). A classic ESP32-WROOM-32
env also builds but has no USB HID, so commands honestly terminate
`failed/dispatch_error` there.

The firmware is self-contained: it joins Wi-Fi, advertises itself over
mDNS (`_maccontrol._tcp`, TXT `mode=A|B`, `pair=active|inactive`), serves:

- **Controller REST API** `http://<hostname>.local/api/v1/*` — RBAC-gated
  (READ/CONTROL/ADMIN API keys, or a Web UI session cookie); command
  submission, ledger queries, logs, status, capabilities, macros,
  triggers, identity, key management.
- **Web UI** at `http://<hostname>.local/` — login, dashboard, macros,
  triggers, security, logs, pairing.
- **Agent surface** `/agent/v1/*` — authenticated by the pairing token,
  not API keys: `/pair` ceremony, `/ws` WebSocket, `/events`, `/status`,
  `/commands/pending` (polling fallback).
- **Serial CLI** (115200) — provisioning and administration
  (`wifi set`, `key create`, `agent pair`, `status`, …).

Architecture: `lib/maccontrol_core/` is pure C++17 spec logic (engine,
ledger, RBAC, pairing, agent session, status builders) that also compiles
on the host for the 144-case native test suite; `src/` is the Arduino
glue (HTTP server on its own task, RFC 6455 WebSocket server, agent link
+ liveness timer, command dispatcher, NVS/LittleFS persistence, USB HID).
The HTTP server is single-threaded with keep-alive + backlog preemption;
every blocking call has a deadline; the task watchdog is fed inside long
loops — all earned the hard way, see `firmware/docs/DEBUG-PHASE4-AT11.md`.

The S3 devkit exposes **two USB ports with different roles** (the most
common operational trap):

- **UART bridge port** (`/dev/cu.usbmodem…` / `cu.usbserial…`) — flashing
  and serial console. macOS asserts DTR/RTS on **every open and close**,
  which drives EN/IO0 through the CH343: **opening or closing any serial
  session reboots the board**. Diagnose live faults over the network.
- **USB-OTG port** — the HID keyboard to the Mac. Once `USB.begin()`
  runs, this port belongs to HID.

### 2. The agent (Mac app, `agent/`)

`maccontrol_agent` — Python 3.9+, no UI in headless mode. It pairs with
the endpoint (8-character code, valid ~120 s, 5 wrong codes close the
window), then keeps a session open: WebSocket preferred, HTTP polling
fallback. Over it the agent streams an initial status burst, heartbeats
(every 5 s), macOS state changes (system state, screen lock, user
session, app start/quit, front-app), and a `capability_report`; it
executes the closed action set (`launch_app`, `quit_app`) for allowlisted
apps and reports results with per-session sequence numbers. A launchd
plist is provided for always-on operation.

**Pairing persists** `{hostname, agent_instance_id, agent_token}` in
`~/.maccontrol/agent.json`; the token is the agent surface credential and
is never logged.

### How they work together

```
 triggers/Web UI/curl
        │  /api/v1/* (API keys, RBAC)
        ▼
  ┌───────────┐   mDNS _maccontrol._tcp    ┌────────────────┐
  │  ESP32    │ ◄────────────────────────► │  Mac (MCA)     │
  │ endpoint  │   /agent/v1/ws  (paired)   │  python agent  │
  │           │   hello → session,         │                │
  │  ledger   │   capability_report,       │  heartbeats 5s │
  │  engine   │   heartbeats, evidence,    │  app monitors  │
  │  HID USB ─┼────── keystrokes ─────────►│  launch/quit   │
  └───────────┘                            └────────────────┘
```

1. **Provision** the endpoint over serial: Wi-Fi credentials, READ/
   CONTROL/ADMIN keys, admin password.
2. **Pair** (once): open a pairing window (Web UI or `agent pair` on the
   serial CLI), run the agent with `--pair-code`; the endpoint stores the
   pairing record and flips to **Mode B**.
3. **Connect**: the agent opens `/agent/v1/ws` (or polls
   `/agent/v1/commands/pending`), sends `hello`, receives a `session_id`
   and the liveness contract: heartbeat 5 s → **stale at 15 s of silence,
   offline at 30 s** (3×/6× the interval, §4.2.2). An unannounced TCP
   death ages out on that clock instead of flipping offline instantly
   (§7.2.2); announced shutdowns (`agent_goodbye`, Phase 5) are different.
4. **Command flow**: `POST /api/v1/commands` → the engine validates,
   rate-limits, appends an *accepted* revision to the append-only ledger
   (LittleFS, survives reboots) → dispatch: Mode B verified commands go
   to the agent (launch/quit) and end `completed/app_launch_confirmed`
   when the agent's evidence confirms them; HID-only commands (macros,
   keystrokes) can never be observed back, so they terminate honestly as
   `unconfirmed`/`hid_only` — the endpoint never fabricates a success.
5. **Status**: `GET /api/v1/status` is a five-tuple document
   (value/source/observed_at/ttl_s/freshness) per leaf; agent-reported
   leaves go `stale` on agent silence; `connection.agent` flips false at
   the offline threshold.

## Installing (end-user guide)

This is the full setup for one **controlled Mac**: flash the ESP32, put it on
Wi-Fi, create API keys, pair the agent — one installer command. Repeat the
whole procedure per Mac you want to control (one ESP32 each).

### What you need

- An **ESP32-S3-DevKitC-1** (N8) per controlled Mac, with **two USB cables**.
- The **Mac to be controlled**, with admin access (the agent runs as the
  logged-in user; screen-lock/sleep detection needs it).
- A **Wi-Fi network** both the ESP32 and any controlling machines share.
- **macOS 13+** with `python3` (install Xcode Command Line Tools with
  `xcode-select --install` if `python3 --version` fails).
- The **WCH CH34x USB-serial driver** on the Mac you flash from. Install it
  from wch.cn (CH34XSER) before the first flash; without it the flash cable
  doesn't appear as `/dev/cu.usbmodem…`.

### Wiring

The S3 devkit has **two USB ports with different jobs** — the only trap that
matters at install time:

- **"USB" port (USB-OTG, usually labeled USB on the silk)** → plug into the
  **Mac being controlled**. The ESP32 is a USB keyboard to that Mac; all
  keystrokes, lock/wake/sleep chords go to *that* Mac, no matter who sends
  the command.
- **"com"/UART port (CH343 bridge)** → the Mac you run the installer on
  (can be the controlled Mac itself). Flashing and provisioning only.

Both ports feed the board's 5 V — **one powered cable is enough**; pulling
the cable that powers it reboots the board.

### One-shot install

```bash
git clone <this repo> && cd "<repo>/agent"
./install.sh --flash --provision \
    --allow com.apple.Terminal com.apple.Safari \
    --enable-launch --enable-quit
```

The installer will:

1. Create `agent/.venv` and install dependencies (recovering automatically
   if a `.venv` was copied from another machine — those die on a new host).
2. **Flash** the committed firmware bundle `firmware/dist/esp32-s3/`
   (bootloader, partition table, app) to the ESP32 over the UART port. If
   several serial devices are present it asks which one.
3. **Prompt** for the Wi-Fi SSID/password and a 10+ character **admin
   password** (typed blind; also passable as `--wifi-ssid … --wifi-pass …
   --admin-password …` for unattended installs).
4. Over the serial console: save Wi-Fi, create **READ / CONTROL / ADMIN API
   keys**, set the admin password.
5. Wait for the device on the network, log into its Web UI, open the
   pairing window, and **pair this Mac's agent** automatically.
6. Write `~/.maccontrol/endpoint.keys` (chmod 600) and print the three
   keys **once** — store them; they are never shown again.
7. Install the LaunchAgent so the agent runs at login and self-restarts.

Each phase is independently re-runnable; a failure says what to fix.
Re-provisioning a used device **adds** keys (8-key cap) — revoke old ones
from the Web UI Security tab or the serial console (`key revoke <id>`), or
pass `--no-keys`. Re-pairing over an existing pairing needs `--re-pair`;
removing everything agent-side is `./install.sh --uninstall`.

### After the install

```bash
launchctl list | grep com.maccontrol.agent          # agent running?
cat ~/.maccontrol/endpoint.keys                     # hostname + your 3 keys
open "http://$(grep ENDPOINT_HOSTNAME ~/.maccontrol/endpoint.keys | cut -d= -f2).local/"
```

The Web UI (log in with the admin password) has the dashboard, macros,
triggers, Security (key minting/revoke), Logs, and Pairing tabs. API checks:

```bash
source ~/.maccontrol/endpoint.keys
curl -s -H "Authorization: Bearer $READ_KEY" "http://$ENDPOINT_HOSTNAME.local/api/v1/status" | head
curl -s -H "Authorization: Bearer $READ_KEY" "http://$ENDPOINT_HOSTNAME.local/api/v1/agent/status"
```

### Everyday control

```bash
H="http://$ENDPOINT_HOSTNAME.local"
# Lock the controlled Mac's screen (verified via the agent's lock detection):
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" $H/api/v1/system/lock
# Sleep it (completes honestly when the host stays offline through the window):
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" $H/api/v1/system/sleep
# Wake it (HID key-tap to the sleeping Mac):
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" $H/api/v1/system/wake
# Launch an allowlisted app on the controlled Mac:
curl -s -X POST -H "Authorization: Bearer $CONTROL_KEY" $H/api/v1/apps/com.apple.Terminal/launch
```

Each command returns `202` with a `command_id`; follow it with
`GET /api/v1/commands/<command_id>` (READ key). Poll no faster than every
2–3 s (the READ bucket is 60 req/min). **`restart` and `shutdown` are real
— they interrupt whoever is at that Mac; use deliberately.** Full command
reference: `GET /api/v1/openapi.json`, and the operator's guide in
`.kimi-code/skills/maccontrol/SKILL.md`.

### Updating the firmware later

```bash
cd "<repo>/firmware"
./.venv/bin/pio run -e esp32-s3-devkitc-1           # build (or use your own pio)
cp .pio/build/esp32-s3-devkitc-1/{bootloader,partitions,firmware}.bin \
   ../firmware/dist/esp32-s3/                        # refresh the bundle
cd ../agent && ./install.sh --flash                 # reflash; NVS/keys/pairing survive
```

A plain reflash never touches Wi-Fi, keys, pairing, or the ledger.

### If something goes wrong

| Symptom | First thing to check |
|---|---|
| No `/dev/cu.usbmodem…` appears | CH34x driver not installed, or the wrong USB port (use the UART/"com" port, not the HID one) |
| Flash can't connect | Another program holds the port (a monitor/IDE); the board reboots on every serial open — that's normal, retry |
| Agent log shows `hello_ack timeout` forever | The agent can't reach the endpoint: wrong/stale hostname in `~/.maccontrol/agent.json` (the device may have been renamed — `GET /api/v1/status → device.hostname` is ground truth), or the Mac changed Wi-Fi networks |
| `401 unauthorized` | Key wrong or revoked; recreate in the Web UI Security tab |
| Locked out after retries | 10 failed auths from one IP locks it out for 60 s — wait, then use a valid key |
| Forgot the admin password | Serial console (`screen /dev/cu.usbmodem… 115200`): `admin set <new>` |

```
MacControl_Deterministic_Architecture_Spec.md   the PRD (normative)
## Repository layout

```
MacControl_Deterministic_Architecture_Spec.md   the PRD (normative)
firmware/          ESP32 firmware (PlatformIO) + docs/ + scripts/ + test/native/
  docs/PHASE1.md — PHASE5.md     per-phase build/verify logs (PHASE5 is the
    current state; read HANDOFF.md first)
  docs/HANDOFF.md          resume document — status, landmines, conventions
  docs/DEBUG-*.md          post-mortems: radio death (PHASE4), stale mDNS
    hostname after device rename, USB link detection
  dist/esp32-s3/           committed flash bundle used by agent/install.sh
  scripts/at01_at02.py … at11.py   acceptance test runners
agent/             MacControlAgent (Python) — see agent/README.md
docx/              spec working documents
```

## Quickstart (ESP32-S3 bench)

```bash
cd "…/ESP32-MCA Command Loop Design/firmware"
python3 -m venv .venv && ./.venv/bin/pip install platformio   # once

./.venv/bin/pio run -e esp32-s3-devkitc-1 -t upload           # flash
./.venv/bin/pio device monitor                                # serial CLI (115200)
#   wifi set <ssid> <pass>
#   key create READ qa      (× CONTROL, × ADMIN — copy the mck_… keys)
#   admin set <password>

python3 scripts/at01_at02.py --hostname mac-XXXXXX.local \
  --read-key mck_… --control-key mck_…                        # Phase 1 gate
python3 scripts/at06.py --hostname mac-XXXXXX.local …         # pairing ceremony
python3 scripts/at11.py --hostname mac-XXXXXX.local …         # agent lifecycle gate

cd ../agent && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python -m maccontrol_agent --hostname mac-XXXXXX --pair-code <CODE>
```

## Testing

- `./.venv/bin/pio test -e native` — 144 host-side tests over
  `lib/maccontrol_core` (ledger, engine, RBAC, pairing, agent session,
  status builders). Run before every flash.
- `pio run -e esp32-s3-devkitc-1` and `-e esp32-wroom-32` must both stay
  clean.
- Acceptance: AT-01…AT-06, AT-11 per phase; each gate needs **two
  consecutive green runs on one boot** (§16.1).

## Operating notes (the short list)

- **Any serial open/close reboots the S3** (DTR/RTS strapping) — expect
  it, use it deliberately (provisioning), and never mistake a
  probe-induced reboot for a device fault.
- Heap is the scarce resource (320 KB): steady state is ~109 KB free with
  the 30 s `[heap]` serial diagnostic showing free/largest/min plus
  per-task stack high-water marks. If the radio ever goes silent with the
  CLI alive and no panic, read `[heap]` before suspecting the network —
  per-request retention/fragmentation presents exactly like an AP
  problem.
- Keep LittleFS traffic bursty and throttled; a crash during LittleFS
  traffic can silently reformat it (keys/ledger live there).
- Never log secrets: the agent token and admin password are redacted by
  construction; AT-06 includes a token-leak check.
