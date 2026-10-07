#!/bin/bash
# MacControlAgent one-shot installer / reconfigurator.
#
# Sets up the maccontrol_agent Python app on this Mac and installs a LaunchAgent
# so the agent starts at login and stays running. Re-running with different
# options reconfigures the agent and restarts the LaunchAgent.
#
# Usage:
#   ./install.sh --help
#
set -euo pipefail

AGENT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLIST_LABEL="com.maccontrol.agent"
PLIST_DEST="$HOME/Library/LaunchAgents/${PLIST_LABEL}.plist"
STATE_DIR="$HOME/.maccontrol"
VENV_DIR="$AGENT_DIR/.venv"

HOSTNAME_OPT=""
PAIR_CODE=""
SET_ALLOW=0
ALLOW_ARGS=()
ENABLE_LAUNCH=""
ENABLE_QUIT=""
TRANSPORT=""
POLL_INTERVAL=""
HEADLESS=0
NO_START=0
UNINSTALL=0
FLASH=0
NO_FLASH=0
PROVISION=0
PORT_OPT=""
WIFI_SSID=""
WIFI_PASS=""
ADMIN_PASSWORD=""
NO_KEYS=0
RE_PAIR=0

# Derived from the repo layout; used by --flash/--provision.
FIRMWARE_DIR="$AGENT_DIR/../firmware"
FLASH_DIST_DIR="$FIRMWARE_DIR/dist/esp32-s3"
PROVISION_JSON=""   # serial-phase JSON result (contains the API keys)
NETWORK_JSON=""     # network-phase JSON result
DISCOVERED_HOSTNAME=""

usage_raw() {
    cat <<'EOF'
MacControlAgent installer — sets up the agent and starts it at login.
Can also flash and provision a MacControl endpoint (ESP32-S3) from this Mac.

USAGE
  ./install.sh [OPTIONS]

  Run interactively with no endpoint flags and the installer asks whether to
  flash + provision an ESP32-S3 endpoint over USB. Answer no (or use
  --no-flash) for an agent-only install.

FIRST RUN (install + pair + configure)
  ./install.sh --hostname maccontrol-01 --pair-code ABC234XY \
      --allow com.apple.Terminal com.apple.Safari \
      --enable-launch --enable-quit

  1. Creates agent/.venv and installs requirements.txt (recreates it if a
     copied/broken .venv is found).
  2. Pairs with the endpoint (open the pairing window on the ESP32 Web UI
     first; the code is valid for ~120 s) and stores the token in
     ~/.maccontrol/agent.json (chmod 600).
  3. Applies the options below to the agent state file.
  4. Installs ~/Library/LaunchAgents/com.maccontrol.agent.plist and loads it,
     so the agent runs at login (RunAtLoad + KeepAlive).

ONE INSTALLER (flash + provision + pair + install, start to finish)
  ./install.sh --flash --provision \
      --allow com.apple.Terminal --enable-launch --enable-quit

  Flashes firmware/dist/esp32-s3 to an ESP32-S3 over USB, then sets WiFi +
  API keys + admin password over the serial console, discovers the device on
  the network, opens the pairing window, pairs this Mac, and finishes with
  the normal config + LaunchAgent steps. Phases are idempotent: a failure
  tells you what to fix, and re-running continues where it left off. The
  interactive run asks you to connect/reboot the board before the serial
  phase (a freshly booted board is required); scripted runs must power-cycle
  the board themselves if it has been plugged in for a while.

RE-RUNNING RECONFIGURES
  ./install.sh --allow com.apple.Terminal            # change the allowlist
  ./install.sh --disable-quit                        # turn an action off
  ./install.sh --hostname maccontrol-02 --pair-code XYZ789AB   # re-pair

ENDPOINT OPTIONS
  --flash              Flash firmware/dist/esp32-s3 to an ESP32-S3 (stops
                       after flashing unless --provision is also given).
  --no-flash           Skip the interactive flash/provision offer (agent-only
                       install when run interactively).
  --provision          Full endpoint setup on an already-flashed board:
                       serial phase (WiFi + keys + admin password) then
                       network phase (discover, pairing window, pair agent).
  --port P             Serial port for --flash/--provision (skips the
                       interactive auto-detect).
  --wifi-ssid S        WiFi network for the endpoint (prompted when missing).
  --wifi-pass P        WiFi password (prompted when missing; never printed).
  --admin-password PW  Endpoint admin password, min 10 chars (prompted when
                       missing; used for 'admin set' and the Web UI login).
  --no-keys            Skip API key creation (serial phase does WiFi+admin only).
  --re-pair            Re-pair even if ~/.maccontrol/agent.json already holds
                       a token (otherwise the network phase refuses).

AGENT OPTIONS
  --hostname H         Endpoint hostname (bare label or FQDN).
  --pair-code CODE     8-character pairing code; pairs, then continues.
  --allow B [B ...]    Replace the app allowlist (bundle IDs).
                       Use '--allow' with no IDs to empty the list.
  --enable-launch      Enable the launch_app action.
  --disable-launch     Disable the launch_app action.
  --enable-quit        Enable the quit_app action.
  --disable-quit       Disable the quit_app action.
  --transport T        websocket (default) or polling.
  --poll-interval S    Polling interval, 2-30 (default 5).
  --headless           LaunchAgent runs the agent with --headless (no UI).
  --no-start           Install and configure, but do not load the LaunchAgent.
  --uninstall          Unload and remove the LaunchAgent (venv, state file,
                       and pairing token are kept).
  -h, --help           Show this help.

OPERATING THE AGENT AFTER INSTALL
  Status:      launchctl list | grep com.maccontrol.agent
  Agent log:   __AGENT_DIR__/.venv/bin/python -m maccontrol_agent --show-log
  Launchd log: tail -f ~/.maccontrol/agent.launchd.log
  Re-pair:     ./install.sh --hostname H --pair-code CODE
  Remove:      ./install.sh --uninstall
EOF
}

# Show usage with the real install paths filled in.
usage() { usage_raw | sed -e "s|__AGENT_DIR__|$AGENT_DIR|g"; }

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hostname)      HOSTNAME_OPT="${2:?--hostname needs a value}"; shift 2 ;;
        --pair-code)     PAIR_CODE="${2:?--pair-code needs a value}"; shift 2 ;;
        --allow)
            shift
            SET_ALLOW=1
            while [[ $# -gt 0 && "$1" != --* ]]; do ALLOW_ARGS+=("$1"); shift; done
            ;;
        --enable-launch)  ENABLE_LAUNCH=1; shift ;;
        --disable-launch) ENABLE_LAUNCH=0; shift ;;
        --enable-quit)    ENABLE_QUIT=1; shift ;;
        --disable-quit)   ENABLE_QUIT=0; shift ;;
        --transport)     TRANSPORT="${2:?--transport needs a value}"; shift 2 ;;
        --poll-interval) POLL_INTERVAL="${2:?--poll-interval needs a value}"; shift 2 ;;
        --headless)      HEADLESS=1; shift ;;
        --no-start)      NO_START=1; shift ;;
        --uninstall)     UNINSTALL=1; shift ;;
        --flash)         FLASH=1; shift ;;
        --no-flash)      NO_FLASH=1; shift ;;
        --provision)     PROVISION=1; shift ;;
        --port)          PORT_OPT="${2:?--port needs a value}"; shift 2 ;;
        --wifi-ssid)     WIFI_SSID="${2:?--wifi-ssid needs a value}"; shift 2 ;;
        --wifi-pass)     WIFI_PASS="${2:?--wifi-pass needs a value}"; shift 2 ;;
        --admin-password) ADMIN_PASSWORD="${2:?--admin-password needs a value}"; shift 2 ;;
        --no-keys)       NO_KEYS=1; shift ;;
        --re-pair)       RE_PAIR=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

if [[ "$UNINSTALL" -eq 1 ]]; then
    log "Unloading LaunchAgent $PLIST_LABEL"
    launchctl bootout "gui/$UID/$PLIST_LABEL" 2>/dev/null \
        || launchctl unload "$PLIST_DEST" 2>/dev/null || true
    rm -f "$PLIST_DEST"
    log "Removed $PLIST_DEST (venv, ~/.maccontrol, and pairing token kept)."
    exit 0
fi

[[ "$FLASH" -eq 0 || "$NO_FLASH" -eq 0 ]] \
    || die "--flash and --no-flash are mutually exclusive"

# --- interactive endpoint offer ----------------------------------------------
# With no endpoint decision on the command line and a terminal attached, offer
# to flash + provision an ESP32-S3. Flag-driven and non-TTY runs are never
# prompted (--no-flash skips the question for agent-only installs).

if [[ "$FLASH" -eq 0 && "$PROVISION" -eq 0 && "$NO_FLASH" -eq 0 && -t 0 ]]; then
    cat >&2 <<'EOF'
==> This package can flash a MacControl ESP32-S3 over USB and set it up on
==> your WiFi (stock firmware; the device keeps its factory mac-<serial>
==> hostname). The board must be connected to this Mac's USB port.
EOF
    reply=""
    read -r -p "==> Flash and set up an endpoint now? [y/N]: " reply || true
    if [[ "$reply" =~ ^[Yy]([Ee][Ss])?$ ]]; then
        FLASH=1
        PROVISION=1
    fi
fi

# --- preflight ---------------------------------------------------------------

[[ "$(uname -s)" == "Darwin" ]] || die "this installer targets macOS"

# Pick the newest Python >= 3.9 for the venv. Xcode CLT-only Macs ship
# python3 = 3.9, and the 3.9-compatible wheels for our dependencies are
# fragile on PyPI (pyobjc-core 12.0 is yanked; pyobjc 12.1+/websockets 16+
# require Python 3.10+), so prefer a newer interpreter when one is
# installed. requirements.txt carries python_version markers so a bare 3.9
# still resolves.
pick_python() {
    local c path ver best_ver="" best_path=""
    for c in python3.13 python3.12 python3.11 python3.10 python3 \
             /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
        path="$(command -v "$c" 2>/dev/null)" || continue
        ver="$("$path" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null)" || continue
        [[ -n "$ver" ]] || continue
        # keep the highest version (skip when ver <= best_ver)
        if [[ -z "$best_ver" || "$ver" != "$(printf '%s\n%s\n' "$ver" "$best_ver" | sort -t. -k1,1n -k2,2n | head -1)" ]]; then
            best_ver="$ver"; best_path="$path"
        fi
    done
    [[ -n "$best_path" ]] || return 1
    printf '%s' "$best_path"
}

PYTHON_BIN="$(pick_python)" || true
[[ -n "$PYTHON_BIN" ]] \
    || die "no python3 found; install Xcode CLT (xcode-select --install) or Python 3.9+ from python.org/Homebrew"
"$PYTHON_BIN" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' \
    || die "Python 3.9+ required (newest found: $("$PYTHON_BIN" -V 2>&1) at $PYTHON_BIN)"
log "Using $("$PYTHON_BIN" -V 2>&1) at $PYTHON_BIN"
[[ -f "$AGENT_DIR/requirements.txt" ]] || die "requirements.txt not found in $AGENT_DIR"

# --- venv + dependencies -----------------------------------------------------

# The package is not pip-installed; run it from the agent directory.
cd "$AGENT_DIR"

venv_healthy() {
    [[ -x "$VENV_DIR/bin/python" ]] \
        && "$VENV_DIR/bin/python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' \
            >/dev/null 2>&1
}

if ! venv_healthy; then
    if [[ -e "$VENV_DIR" ]]; then
        warn "existing .venv is broken or came from another machine (copied venvs die on new hosts); recreating it"
        rm -rf "$VENV_DIR"
    fi
    log "Creating virtualenv at $VENV_DIR"
    "$PYTHON_BIN" -m venv "$VENV_DIR"
fi
log "Installing Python dependencies"
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet -r "$AGENT_DIR/requirements.txt"

# --- endpoint: flash + provision ---------------------------------------------
# Flow: preflight -> venv (above) -> pip extras -> flash -> serial -> network
# -> config -> LaunchAgent. Each phase's failure message says what to fix and
# notes that re-running is safe (phases are idempotent).

if [[ "$PROVISION" -eq 1 && -z "$WIFI_SSID" && -z "$ADMIN_PASSWORD" && ! -t 0 ]]; then
    die "--provision needs --wifi-ssid/--wifi-pass and --admin-password (they are prompted for interactively when stdin is a TTY)"
fi
if [[ -n "$ADMIN_PASSWORD" && ${#ADMIN_PASSWORD} -lt 10 ]]; then
    die "--admin-password must be at least 10 characters"
fi
if [[ -n "$PORT_OPT" || -n "$WIFI_SSID" || -n "$WIFI_PASS" || -n "$ADMIN_PASSWORD" ]] \
    && [[ "$FLASH" -eq 0 && "$PROVISION" -eq 0 ]]; then
    die "--port/--wifi-ssid/--wifi-pass/--admin-password only apply with --flash or --provision"
fi
if [[ "$FLASH" -eq 1 || "$PROVISION" -eq 1 ]]; then
    [[ -d "$FLASH_DIST_DIR" ]] \
        || die "firmware flash bundle not found at $FLASH_DIST_DIR (expected firmware/dist/esp32-s3 from the repo)"
    log "Installing esptool + pyserial for the endpoint phase"
    "$VENV_DIR/bin/pip" install --quiet esptool pyserial
fi

detect_serial_port() {
    local ports=()
    local p
    for p in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial*; do
        [[ -e "$p" ]] && ports+=("$p")
    done
    if [[ ${#ports[@]} -eq 0 ]]; then
        die "no ESP32 serial port found. Check the USB cable to the board's 'com' (CH343 UART) port, install the WCH CH34x/CH343 driver (https://www.wch.cn/downloads/CH341SER_MAC_ZIP.html), then re-run"
    fi
    if [[ ${#ports[@]} -eq 1 ]]; then
        printf '%s\n' "${ports[0]}"
        return 0
    fi
    printf 'Multiple serial ports found:\n' >&2
    local i
    for i in "${!ports[@]}"; do
        printf '  %d) %s\n' "$((i + 1))" "${ports[$i]}" >&2
    done
    local choice=""
    if [[ -t 0 ]]; then
        read -r -p "Pick a port number: " choice
    fi
    [[ "$choice" =~ ^[0-9]+$ && "$choice" -ge 1 && "$choice" -le ${#ports[@]} ]] \
        || die "no valid port picked"
    printf '%s\n' "${ports[$((choice - 1))]}"
}

prompt_secret() {
    # $1 = prompt, $2 = confirmation label. Echo is disabled; never printed.
    local value=""
    if [[ -t 0 ]]; then
        read -r -s -p "$1: " value
        printf '\n' >&2
    else
        die "$2 was not given and stdin is not a TTY to prompt for it"
    fi
    printf '%s' "$value"
}

prompt_admin_password() {
    if [[ -n "$ADMIN_PASSWORD" ]]; then
        [[ ${#ADMIN_PASSWORD} -ge 10 ]] \
            || die "--admin-password must be at least 10 characters"
        return 0
    fi
    local attempt
    for attempt in 1 2; do
        ADMIN_PASSWORD="$(prompt_secret "Endpoint admin password (min 10 chars, input hidden)" "admin password")"
        [[ ${#ADMIN_PASSWORD} -ge 10 ]] && return 0
        warn "admin password must be at least 10 characters (attempt $attempt/2)"
    done
    die "admin password still too short; re-run with a password of 10+ characters"
}

if [[ "$FLASH" -eq 1 || "$PROVISION" -eq 1 ]]; then
    SERIAL_PORT="$PORT_OPT"
    if [[ -z "$SERIAL_PORT" ]]; then
        SERIAL_PORT="$(detect_serial_port)"
    fi
    log "Using serial port $SERIAL_PORT (opening it reboots the board — expected)"
fi

if [[ "$FLASH" -eq 1 ]]; then
    log "Flashing firmware $(cat "$FLASH_DIST_DIR/VERSION" | head -1) to ESP32-S3 on $SERIAL_PORT"
    (
        cd "$FLASH_DIST_DIR"
        "$VENV_DIR/bin/esptool.py" --chip esp32s3 --port "$SERIAL_PORT" --baud 921600 \
            --before default_reset --after hard_reset write_flash -z \
            --flash_mode dio --flash_freq 80m --flash_size 8MB \
            0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin \
            0x10000 firmware.bin
    ) || die "flashing failed (check the cable/port; power-cycle the board and re-run — re-flashing is safe)"
    log "Flashing complete"
fi

if [[ "$PROVISION" -eq 1 ]]; then
    if [[ -z "$WIFI_SSID" ]]; then
        if [[ -t 0 ]]; then
            read -r -p "WiFi network name (SSID) for the endpoint: " WIFI_SSID || true
        fi
        [[ -n "$WIFI_SSID" ]] \
            || die "--provision needs --wifi-ssid/--wifi-pass (the endpoint must join your network for the network phase)"
    fi
    if [[ -z "$WIFI_PASS" ]]; then
        WIFI_PASS="$(prompt_secret "WiFi password for '$WIFI_SSID' (input hidden)" "WiFi password")"
        [[ -n "$WIFI_PASS" ]] || die "WiFi password cannot be empty"
    fi
    prompt_admin_password

    if [[ -t 0 ]]; then
        cat >&2 <<'EOF'

  Next: the serial console session opens. It only works against a FRESHLY
  BOOTED board — one that has been sitting plugged in can sit silent and
  fail with "device did not become ready".

    - If the board's 'com' (CH343 UART) cable is not connected to this Mac,
      plug it in now (not the 'USB' port — that one is HID-only).
    - Then reboot the board: unplug/replug the cable, or press RESET.
      Do NOT hold the BOOT button (that forces download mode, no console).
EOF
        read -r -p "  Press Enter once the board is connected and rebooted... " _ || true
    fi

    SERIAL_ARGS=(--port "$SERIAL_PORT" --reboot --admin-password "$ADMIN_PASSWORD"
                 --wifi-ssid "$WIFI_SSID" --wifi-pass "$WIFI_PASS")
    if [[ "$NO_KEYS" -eq 1 ]]; then
        SERIAL_ARGS+=(--no-keys)
    else
        SERIAL_ARGS+=(--keys-label "install-$(date +%Y%m%d)")
    fi
    log "Serial phase: WiFi + keys + admin password over $SERIAL_PORT (port open may reboot the board once)"
    PROVISION_JSON="$("$VENV_DIR/bin/python" - "$AGENT_DIR" "${SERIAL_ARGS[@]}" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1])
sys.argv = ["mc_provision.py", "serial"] + sys.argv[2:]
import mc_provision
sys._mc_console_log = lambda msg: print("==> %s" % msg, file=sys.stderr)
raise SystemExit(mc_provision.main())
PYEOF
)" || die "serial provisioning failed (fix the issue and re-run with --provision; completed steps are safe to repeat)"

    if [[ "$NO_KEYS" -eq 0 ]]; then
        READ_KEY="$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin)["keys"]["READ"]["raw"])')" \
            || die "could not parse the serial phase result"
        mkdir -p "$STATE_DIR"
        KEYS_FILE="$STATE_DIR/endpoint.keys"
        {
            printf 'ENDPOINT_HOSTNAME=\n'   # filled in after the network phase
            printf 'READ_KEY=%s\n' "$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin)["keys"]["READ"]["raw"])')"
            printf 'CONTROL_KEY=%s\n' "$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin)["keys"]["CONTROL"]["raw"])')"
            printf 'ADMIN_KEY=%s\n' "$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin)["keys"]["ADMIN"]["raw"])')"
        } > "$KEYS_FILE"
        chmod 600 "$KEYS_FILE"
        log "Wrote $KEYS_FILE (chmod 600)"
        cat >&2 <<'EOF'

  The endpoint API keys are shown exactly once here and are never stored in
  the agent state or logs. Store them somewhere safe, then remove this
  terminal scrollback if the machine is shared.
EOF
        for role in READ CONTROL ADMIN; do
            key_line="$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c "import json,sys; print(json.load(sys.stdin)['keys']['$role']['raw'])")"
            printf '  %s_KEY=%s\n' "$role" "$key_line" >&2
        done
    fi

    # Network phase: discover, refuse to clobber an existing pairing unless
    # --re-pair, open the pairing window, pair the agent, verify the session.
    HOST_FOR_NETWORK="$HOSTNAME_OPT"
    if [[ -z "$HOST_FOR_NETWORK" ]]; then
        HOST_FOR_NETWORK="$(printf '%s' "$PROVISION_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin).get("ip") or "")' || true)"
    fi
    [[ -n "$HOST_FOR_NETWORK" ]] || die "no hostname or IP to reach the endpoint on"

    if [[ "$RE_PAIR" -eq 0 && -s "$STATE_DIR/agent.json" ]] \
        && "$PYTHON_BIN" -c 'import json,os,sys; d=json.load(open(os.path.expanduser("~/.maccontrol/agent.json"))); sys.exit(0 if d.get("agent_token") else 1)' >/dev/null 2>&1; then
        die "$HOME/.maccontrol/agent.json already holds a pairing token. Re-run with --re-pair to replace it, or --uninstall first"
    fi

    log "Network phase: discovering $HOST_FOR_NETWORK, opening the pairing window, pairing the agent"
    NETWORK_JSON="$(MC_READ_KEY="${READ_KEY:-}" "$VENV_DIR/bin/python" - "$AGENT_DIR" --hostname "$HOST_FOR_NETWORK" --admin-password "$ADMIN_PASSWORD" <<'PYEOF'
import os, sys
sys.path.insert(0, sys.argv[1])
sys.argv = ["mc_provision.py", "network"] + sys.argv[2:]
import mc_provision
sys._mc_console_log = lambda msg: print("==> %s" % msg, file=sys.stderr)
raise SystemExit(mc_provision.main())
PYEOF
)" || die "network provisioning failed (the device is reachable but pairing did not complete; re-run with --provision --re-pair — WiFi and keys are already set)"
    DISCOVERED_HOSTNAME="$(printf '%s' "$NETWORK_JSON" | "$PYTHON_BIN" -c 'import json,sys; print(json.load(sys.stdin)["hostname"])')" \
        || die "could not parse the network phase result"
    log "Endpoint paired as $DISCOVERED_HOSTNAME"
    # The endpoint.keys placeholder now gets the authoritative hostname.
    if [[ "$NO_KEYS" -eq 0 && -f "$STATE_DIR/endpoint.keys" ]]; then
        sed -i '' -e "s|^ENDPOINT_HOSTNAME=.*|ENDPOINT_HOSTNAME=$DISCOVERED_HOSTNAME|" "$STATE_DIR/endpoint.keys"
    fi
    # The config/pairing sections below operate on this endpoint.
    HOSTNAME_OPT="$DISCOVERED_HOSTNAME"
fi

if [[ "$FLASH" -eq 1 && "$PROVISION" -eq 0 ]]; then
    log "Flashed (--flash without --provision: done; re-run with --provision to set WiFi, keys, and pairing)"
    exit 0
fi

# --- pairing -----------------------------------------------------------------

if [[ -n "$PAIR_CODE" ]]; then
    PAIR_HOST="$HOSTNAME_OPT"
    if [[ -z "$PAIR_HOST" ]]; then
        PAIR_HOST="$("$VENV_DIR/bin/python" - "$AGENT_DIR" <<'PYEOF'
import json, os, sys
try:
    with open(os.path.expanduser("~/.maccontrol/agent.json")) as fh:
        print(json.load(fh).get("hostname") or "")
except Exception:
    print("")
PYEOF
)"
    fi
    [[ -n "$PAIR_HOST" ]] || die "pairing needs --hostname (or a previously paired state file)"
    log "Pairing with $PAIR_HOST ..."
    "$VENV_DIR/bin/python" -m maccontrol_agent --hostname "$PAIR_HOST" \
        --pair-code "$PAIR_CODE" || die "pairing failed (is the pairing window open on $PAIR_HOST?)"
else
    log "No --pair-code given; keeping any existing pairing."
fi

# --- configuration (state file; safe to run while the LaunchAgent is live) ---

ALLOW_JSON=""
if [[ "$SET_ALLOW" -eq 1 ]]; then
    if [[ ${#ALLOW_ARGS[@]} -gt 0 ]]; then
        ALLOW_JSON="$(printf '%s\n' "${ALLOW_ARGS[@]}" | "$PYTHON_BIN" -c 'import json,sys; print(json.dumps([l for l in sys.stdin.read().splitlines() if l.strip()]))')"
    else
        ALLOW_JSON="[]"
    fi
fi

log "Applying configuration to ~/.maccontrol/agent.json"
MCA_ALLOW_JSON="$ALLOW_JSON" \
MCA_ENABLE_LAUNCH="$ENABLE_LAUNCH" \
MCA_ENABLE_QUIT="$ENABLE_QUIT" \
MCA_TRANSPORT="$TRANSPORT" \
MCA_POLL_INTERVAL="$POLL_INTERVAL" \
AGENT_DIR="$AGENT_DIR" \
"$VENV_DIR/bin/python" - <<'PYEOF'
import json, os, sys

sys.path.insert(0, os.environ["AGENT_DIR"])
from maccontrol_agent.state import AgentState, DEFAULT_STATE_PATH, TRANSPORTS

state = AgentState.load(DEFAULT_STATE_PATH)
state.ensure_instance_id()

if os.environ.get("MCA_ALLOW_JSON"):
    state.allowlist = json.loads(os.environ["MCA_ALLOW_JSON"])
if os.environ.get("MCA_ENABLE_LAUNCH"):
    state.enabled_commands["launch_app"] = os.environ["MCA_ENABLE_LAUNCH"] == "1"
if os.environ.get("MCA_ENABLE_QUIT"):
    state.enabled_commands["quit_app"] = os.environ["MCA_ENABLE_QUIT"] == "1"
if os.environ.get("MCA_TRANSPORT"):
    if os.environ["MCA_TRANSPORT"] not in TRANSPORTS:
        sys.exit("ERROR: invalid --transport %r (want %s)" % (os.environ["MCA_TRANSPORT"], "/".join(TRANSPORTS)))
    state.transport = os.environ["MCA_TRANSPORT"]
if os.environ.get("MCA_POLL_INTERVAL"):
    try:
        state.poll_interval_s = max(2, min(30, int(os.environ["MCA_POLL_INTERVAL"])))
    except ValueError:
        sys.exit("ERROR: invalid --poll-interval %r (want an integer 2-30)"
                 % os.environ["MCA_POLL_INTERVAL"])

state.save()
print("state file: %s (paired with %s)" % (DEFAULT_STATE_PATH,
                                             state.hostname or "<nothing yet>"))
PYEOF

# --- LaunchAgent plist -------------------------------------------------------

mkdir -p "$STATE_DIR" "$HOME/Library/LaunchAgents"

DAEMON_ARGS="    <string>--daemon</string>"
if [[ "$HEADLESS" -eq 1 ]]; then
    DAEMON_ARGS="$DAEMON_ARGS
    <string>--headless</string>"
fi

cat > "$PLIST_DEST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>${PLIST_LABEL}</string>

  <key>ProgramArguments</key>
  <array>
    <string>${VENV_DIR}/bin/python</string>
    <string>-m</string>
    <string>maccontrol_agent</string>
${DAEMON_ARGS}
  </array>

  <key>WorkingDirectory</key>
  <string>${AGENT_DIR}</string>

  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>

  <key>StandardOutPath</key>
  <string>${STATE_DIR}/agent.launchd.log</string>
  <key>StandardErrorPath</key>
  <string>${STATE_DIR}/agent.launchd.log</string>

  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key>
    <string>/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin</string>
  </dict>
</dict>
</plist>
EOF
log "Wrote $PLIST_DEST"

# --- load / reload -----------------------------------------------------------

if [[ "$NO_START" -eq 1 ]]; then
    log "Configured (--no-start). Load it later with:"
    log "  launchctl bootstrap gui/\$UID \"$PLIST_DEST\""
else
    log "Loading LaunchAgent $PLIST_LABEL"
    launchctl bootout "gui/$UID/$PLIST_LABEL" 2>/dev/null || true
    if ! launchctl bootstrap "gui/$UID" "$PLIST_DEST" 2>/dev/null; then
        launchctl unload "$PLIST_DEST" 2>/dev/null || true
        launchctl load -w "$PLIST_DEST"
    fi
    sleep 2
    if launchctl list | awk '{print $3}' | grep -qx "$PLIST_LABEL"; then
        log "LaunchAgent is loaded and running (PID: $(launchctl list | awk -v l="$PLIST_LABEL" '$3==l {print $1}'))."
    else
        warn "LaunchAgent did not report as running; check $STATE_DIR/agent.launchd.log"
    fi
fi

cat <<EOF

MacControlAgent setup complete.

  Agent dir:    $AGENT_DIR
  State file:   $STATE_DIR/agent.json (paired with: see above)
  LaunchAgent:  $PLIST_DEST

  Status:       launchctl list | grep $PLIST_LABEL
  Agent log:    "$VENV_DIR/bin/python" -m maccontrol_agent --show-log
  Launchd log:  tail -f $STATE_DIR/agent.launchd.log

  Reconfigure:  re-run this script with new options (allowlist, actions, ...).
  Re-pair:      ./install.sh --hostname H --pair-code CODE
  Remove:       ./install.sh --uninstall

The agent now starts at login and restarts automatically if it stops.
EOF
