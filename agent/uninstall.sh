#!/bin/bash
# Remove the current user's Python MCA service and stop its agent processes.
set -euo pipefail

LABEL="com.maccontrol.agent"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
SERVICE="gui/$UID/$LABEL"
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage: ./uninstall.sh [--dry-run]

Run as the user who installed the Python agent (without sudo).
Unloads the Python LaunchAgent, removes its startup plist, and stops this
user's Python processes running -m maccontrol_agent, including manual runs.
Processes get SIGTERM, then SIGKILL if still running after five seconds.

Keeps the source, .venv, ~/.maccontrol settings/logs/credentials, endpoint
pairing, and power schedules. The native MacControl service is unaffected.
  --dry-run   Show the service, plist, and matching PIDs without changing them.
  -h, --help  Show this help.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$arg" >&2; usage >&2; exit 1 ;;
    esac
done

[[ "$(uname -s)" == Darwin ]] || { echo "This uninstaller targets macOS." >&2; exit 1; }
[[ "$EUID" -ne 0 ]] || { echo "Run as the user who installed the agent, without sudo." >&2; exit 1; }

# Check both the interpreter and the exact module argument. A substring match
# alone could terminate an editor, shell, or unrelated Python program. Recheck
# each PID before signaling it, since processes may exit during uninstall.
is_agent_pid() {
    local pid="$1" owner executable command interpreter arguments
    owner="$(ps -p "$pid" -o uid= 2>/dev/null)" || return 1
    [[ "${owner//[[:space:]]/}" == "$UID" ]] || return 1
    executable="$(ps -p "$pid" -o comm= 2>/dev/null)" || return 1
    executable="${executable##*/}"
    [[ "$executable" =~ ^[Pp]ython([0-9]+(\.[0-9]+)*)?$ ]] || return 1
    command="$(ps -ww -p "$pid" -o command= 2>/dev/null)" || return 1
    # Split at the first option, allowing spaces in the interpreter path.
    # Require -m to be Python's invocation mode, not text inside -c or a
    # different script's arguments. Common interpreter switches are allowed.
    interpreter="${command%% -*}"
    [[ "$interpreter" != "$command" ]] || return 1
    [[ "${interpreter##*/}" =~ ^[Pp]ython([0-9]+(\.[0-9]+)*)?$ ]] || return 1
    arguments="${command#"$interpreter"}"
    [[ "$arguments" =~ ^[[:space:]]+(-[uBIEsSOqvb]+[[:space:]]+)*-m[[:space:]]+maccontrol_agent([[:space:]]|$) ]]
}

agent_pids() {
    local candidates pid
    candidates="$(ps -ww -U "$UID" -o pid=,command=)" || return 1
    while read -r pid; do
        [[ -n "$pid" ]] || continue
        if is_agent_pid "$pid"; then printf '%s\n' "$pid"; fi
    done < <(printf '%s\n' "$candidates" | awk '
        /(^|[[:space:]])-m[[:space:]]+maccontrol_agent([[:space:]]|$)/ {print $1}')
}

echo "Python service: $SERVICE"
echo "Startup plist: $PLIST"
if [[ "$DRY_RUN" -eq 1 ]]; then
    pids="$(agent_pids)"
    printf 'Would stop Python agent PIDs: %s\n' "${pids:-none}"
    echo "Dry run: no changes made."
    exit 0
fi

# Unload before signaling processes so KeepAlive cannot restart them. Removing
# the installed plist prevents launchd from loading it at the next login.
launchctl bootout "$SERVICE" 2>/dev/null \
    || launchctl unload "$PLIST" 2>/dev/null || true
rm -f "$PLIST"
if launchctl print "$SERVICE" >/dev/null 2>&1; then
    echo "The Python service is still registered; could not unload it." >&2
    exit 1
fi

pids="$(agent_pids)"
for pid in $pids; do
    if is_agent_pid "$pid"; then
        echo "Stopping Python agent PID $pid"
        kill -TERM "$pid" 2>/dev/null || true
    fi
done
if [[ -n "$pids" ]]; then
    for attempt in 1 2 3 4 5; do
        remaining="$(agent_pids)"
        [[ -n "$remaining" ]] || break
        sleep 1
    done
    remaining="$(agent_pids)"
    for pid in $remaining; do
        if is_agent_pid "$pid"; then
            echo "Force-stopping Python agent PID $pid"
            kill -KILL "$pid" 2>/dev/null || true
        fi
    done
    [[ -z "$remaining" ]] || sleep 1
fi

remaining="$(agent_pids)"
if [[ -n "$remaining" ]]; then
    printf 'Could not stop Python agent PIDs: %s\n' "$remaining" >&2
    exit 1
fi
echo "Python agent uninstalled: service unloaded, startup plist removed, no matching agents running."
echo "Kept source, .venv, ~/.maccontrol, endpoint credentials/pairing, and power schedules."
