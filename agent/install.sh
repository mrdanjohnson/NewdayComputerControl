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

usage_raw() {
    cat <<'EOF'
MacControlAgent installer — sets up the agent and starts it at login.

USAGE
  ./install.sh [OPTIONS]

FIRST RUN (install + pair + configure)
  ./install.sh --hostname maccontrol-01 --pair-code ABC234XY \
      --allow com.apple.Terminal com.apple.Safari \
      --enable-launch --enable-quit

  1. Creates agent/.venv and installs requirements.txt (skipped if present).
  2. Pairs with the endpoint (open the pairing window on the ESP32 Web UI
     first; the code is valid for ~120 s) and stores the token in
     ~/.maccontrol/agent.json (chmod 600).
  3. Applies the options below to the agent state file.
  4. Installs ~/Library/LaunchAgents/com.maccontrol.agent.plist and loads it,
     so the agent runs at login (RunAtLoad + KeepAlive).

RE-RUNNING RECONFIGURES
  ./install.sh --allow com.apple.Terminal            # change the allowlist
  ./install.sh --disable-quit                        # turn an action off
  ./install.sh --hostname maccontrol-02 --pair-code XYZ789AB   # re-pair

OPTIONS
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

# --- preflight ---------------------------------------------------------------

[[ "$(uname -s)" == "Darwin" ]] || die "this installer targets macOS"
command -v python3 >/dev/null 2>&1 || die "python3 not found; install Xcode CLT (xcode-select --install)"
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' \
    || die "Python 3.9+ required (found $(python3 -V 2>&1))"
[[ -f "$AGENT_DIR/requirements.txt" ]] || die "requirements.txt not found in $AGENT_DIR"

# --- venv + dependencies -----------------------------------------------------

# The package is not pip-installed; run it from the agent directory.
cd "$AGENT_DIR"

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
    log "Creating virtualenv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi
log "Installing Python dependencies"
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet -r "$AGENT_DIR/requirements.txt"

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
        ALLOW_JSON="$(printf '%s\n' "${ALLOW_ARGS[@]}" | python3 -c 'import json,sys; print(json.dumps([l for l in sys.stdin.read().splitlines() if l.strip()]))')"
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
    <string>/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin</string>
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
