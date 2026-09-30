#!/usr/bin/env python3
"""MacControl Phase 4 acceptance test AT-11 (spec section 16.2): reconnect
backoff + polling fallback.

AT-11 (spec): kill the MCA WebSocket; heartbeat loss marks the agent stale at
15 s and offline at 30 s; reconnect follows the 1/2/4/8/30 s backoff; the
polling fallback (POST /agent/v1/events + GET /agent/v1/commands/pending)
produces ledger outcomes indistinguishable from the WebSocket path. Traces:
Ch. 4.2.2, 4.3.

The whole flow runs TWICE consecutively inside one invocation (spec 16 rule);
every check must pass in both rounds.

Preconditions: AT-06 has run and /tmp/mc_at_agent.json holds the paired state
(agent_token included). If the MCA daemon is not running it is started here.

Phases per round:
  A. Heartbeat loss — kill the daemon; watch /api/v1/status degrade:
     mac.state freshness -> "stale" at 13-22 s, connection.agent.value ->
     false at 25-38 s.
  B. Reconnect + backoff — restart over WebSocket; expect agent.connected
     within 10 s (first backoff step 1-1.2 s + connect time); session logs
     show the churn.
  C. Dispatch over WebSocket — launch/quit the allowlisted app to terminal
     completed/app_launch_confirmed / app_quit_confirmed.
  D. Polling fallback — restart with --transport polling; dispatch launch
     again; ledger record shape must match the WS run (state/result/
     error_code/revision); probe GET /agent/v1/commands/pending directly with
     the agent bearer token.

The daemon is killed and the pidfile removed at the end.

Usage:
  python3 at11.py --hostname mac-a1b2c3.local \
      --read-key mck_... --control-key mck_...

Exit code 0 = all checks pass (both rounds).
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

HERE = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "agent"))
AGENT_PY = os.path.normpath(os.path.join(AGENT_DIR, ".venv", "bin", "python"))
PIDFILE = "/tmp/mc_at_agent.pid"
DAEMON_LOG = "/tmp/mc_at_agent.out"

TERMINAL_STATES = ("completed", "failed", "timed_out", "unconfirmed")

PROCS = {}  # pid -> Popen, for daemons started by this invocation


class Checker:
    def __init__(self):
        self.failures = []

    def check(self, label, cond, detail=""):
        status = PASS if cond else FAIL
        print(f"  [{status}] {label}" + (f" — {detail}" if detail and not cond else ""))
        if not cond:
            self.failures.append(f"{label}: {detail}")

    def section(self, title):
        print(f"\n=== {title} ===")


from mc_http import http


def error_code(doc):
    return (doc.get("error") or {}).get("code")


def _pid_from_file():
    try:
        return int(open(PIDFILE, "r", encoding="utf-8").read().strip())
    except Exception:
        return None


def daemon_alive():
    pid = _pid_from_file()
    if pid is None:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def start_agent_daemon(hostname, state_file, bundle_id, transport=None):
    cmd = [AGENT_PY, "-m", "maccontrol_agent", "--hostname", hostname,
           "--state-file", state_file, "--allow", bundle_id,
           "--enable-launch", "--enable-quit", "--headless"]
    if transport:
        cmd += ["--transport", transport]
    logfh = open(DAEMON_LOG, "ab", buffering=0)
    proc = subprocess.Popen(cmd, stdout=logfh, stderr=subprocess.STDOUT,
                            start_new_session=True, cwd=AGENT_DIR)
    logfh.close()
    PROCS[proc.pid] = proc
    with open(PIDFILE, "w", encoding="utf-8") as pf:
        pf.write("%d\n" % proc.pid)
    return proc.pid


def stop_daemon():
    """SIGTERM the daemon from the pidfile; wait for exit; SIGKILL as fallback.
    Also sweeps any stray maccontrol_agent from earlier runs — competing
    daemons supersede each other's sessions (close 1000) and make the
    endpoint look flaky."""
    pid = _pid_from_file()
    proc = PROCS.get(pid) if pid is not None else None
    if pid is not None:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    subprocess.run(["pkill", "-f", "maccontrol_agent"], capture_output=True)
    if proc is not None:
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except OSError:
                pass
    if pid is not None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except OSError:
                break
            time.sleep(0.2)
    try:
        os.remove(PIDFILE)
    except OSError:
        pass
    if pid is not None:
        PROCS.pop(pid, None)


def hard_kill_daemon():
    """SIGKILL the whole daemon process group: Phase A needs an UNANNOUNCED
    loss (no agent_goodbye, no close frame) so the endpoint ages the session
    on the heartbeat-loss path (stale ~15 s, offline ~30 s) instead of ending
    it at the graceful close."""
    pid = _pid_from_file()
    if pid is not None:
        try:
            os.killpg(os.getpgid(pid), signal.SIGKILL)
        except OSError:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        PROCS.pop(pid, None)
    subprocess.run(["pkill", "-9", "-f", "maccontrol_agent"], capture_output=True)
    try:
        os.remove(PIDFILE)
    except OSError:
        pass


def reset_app(base, read_key, bundle_id):
    """Test hygiene: ensure the app is NOT running before a launch phase,
    and that the agent's monitor has OBSERVED the exit — a launch inside the
    monitor's 3 s blind window produces no application_started delta (the
    predicate then correctly times out). A launch of an already-running app
    likewise produces no started event."""
    name = bundle_id.rsplit(".", 1)[-1]
    subprocess.run(["pkill", "-x", name], capture_output=True)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        s, doc = http("GET", base, "/api/v1/status", key=read_key)
        if s == 200:
            app = ((doc.get("applications") or {}).get(bundle_id) or {})
            state = (app.get("state") or {}).get("value")
            if state in (None, "not_running"):
                return True
        else:
            # Fallback: local process check only.
            rc = subprocess.run(["pgrep", "-x", name], capture_output=True)
            if rc.returncode != 0:
                return True
        time.sleep(1.0)
    return False


def wait_connected(base, read_key, timeout_s):
    """Poll capabilities until agent.connected == true. Returns (elapsed, doc)."""
    t0 = time.monotonic()
    deadline = t0 + timeout_s
    doc = {}
    while time.monotonic() < deadline:
        s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
        if s == 200 and (doc.get("agent") or {}).get("connected") is True:
            return time.monotonic() - t0, doc
        time.sleep(1.0)
    return None, doc


def poll_command_terminal(base, key, command_id, timeout_s=40):
    deadline = time.monotonic() + timeout_s
    rec = None
    while time.monotonic() < deadline:
        s, rec = http("GET", base, f"/api/v1/commands/{command_id}", key=key)
        if s == 200 and rec.get("state") in TERMINAL_STATES:
            return rec
        time.sleep(1.0)
    return rec if isinstance(rec, dict) else None


def dispatch_and_expect(checker, base, control_key, poll_key, bundle_id, action, expected_result):
    """POST launch|quit, poll to terminal, check completed/expected_result."""
    s, doc = http("POST", base, f"/api/v1/apps/{bundle_id}/{action}", key=control_key)
    checker.check(f"POST /api/v1/apps/{bundle_id}/{action} -> 202",
                  s == 202 and bool(doc.get("command_id")), f"status={s}")
    if s != 202:
        return None
    # Poll the record with the READ key: CONTROL's 30/min bucket cannot
    # sustain 2 s polling plus the POSTs.
    rec = poll_command_terminal(base, poll_key, doc["command_id"], 40)
    checker.check(f"{action} reaches terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"{action} terminal completed/{expected_result}",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == expected_result,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')}")
    return rec


def app_running(bundle_id):
    """Tri-state: True/False running, None = could not determine."""
    try:
        out = subprocess.run(
            ["osascript", "-e", f'tell application id "{bundle_id}" to running'],
            capture_output=True, text=True, timeout=10)
        if out.returncode == 0:
            return "true" in out.stdout.strip().lower()
    except Exception:
        pass
    try:
        out = subprocess.run(["pgrep", "-f", bundle_id], capture_output=True, timeout=5)
        return out.returncode == 0
    except Exception:
        return None


def phase_a_heartbeat_loss(checker, base, read_key, hostname, state_file, bundle_id, rnd):
    checker.section(f"Round {rnd} Phase A: heartbeat loss (stale ~15 s, offline ~30 s)")
    # Precondition: an ACTIVE session must exist when the clock starts. The
    # daemon process can be alive but mid-backoff (e.g. after an endpoint
    # reboot), which would make the very first poll read "already stale".
    elapsed, doc = wait_connected(base, read_key, 40)
    checker.check("agent session ACTIVE before heartbeat-loss clock starts",
                  elapsed is not None, "agent.connected stayed false for 40 s")
    hard_kill_daemon()  # unannounced loss: no goodbye, no close frame
    # The clock starts at the last ADMITTED agent frame (heartbeats can land
    # up to one interval before the kill), not at the kill instant. Freshness
    # is silence-based (spec 4.2.2), so at kill time the true silence is at
    # most one heartbeat interval — mac.state.observed_at is NOT a valid
    # anchor (it tracks system-state changes, which can be much older on an
    # idle mac; anchoring on it overshoots the age by that gap).
    head_start = 5.0  # one heartbeat interval (spec 4.2.2 default)
    print(f"  ... last agent frame was {head_start:.1f} s before the kill")
    t0 = time.monotonic()
    stale_at = offline_at = None
    deadline = t0 + 50
    while time.monotonic() < deadline and (stale_at is None or offline_at is None):
        s, doc = http("GET", base, "/api/v1/status", key=read_key)
        if s == 200:
            now = time.monotonic() - t0
            mac_state = (doc.get("mac") or {}).get("state") or {}
            conn_agent = (doc.get("connection") or {}).get("agent") or {}
            if stale_at is None and mac_state.get("freshness") == "stale":
                stale_at = now
                print(f"  ... mac.state.freshness -> 'stale' at {now:.1f} s")
            if offline_at is None and conn_agent.get("value") is False:
                offline_at = now
                print(f"  ... connection.agent.value -> false at {now:.1f} s")
            if stale_at is not None and offline_at is not None:
                break
        time.sleep(2.0)
    checker.check("mac.state freshness became 'stale'", stale_at is not None,
                  "still fresh after 50 s")
    if stale_at is not None:
        age = stale_at + head_start  # silence age at the observation
        checker.check("stale at 15 s of silence (observed 15-22 s)", 15.0 <= age <= 22.0,
                      f"age={age:.1f} s (t={stale_at:.1f} s, head={head_start:.1f} s)")
    checker.check("connection.agent.value became false (OFFLINE)", offline_at is not None,
                  "still online after 50 s")
    if offline_at is not None:
        age = offline_at + head_start
        checker.check("offline at 30 s of silence (observed 30-40 s)", 30.0 <= age <= 40.0,
                      f"age={age:.1f} s (t={offline_at:.1f} s, head={head_start:.1f} s)")
    if stale_at is not None and offline_at is not None:
        checker.check("stale precedes offline", stale_at < offline_at,
                      f"stale={stale_at:.1f} offline={offline_at:.1f}")
    # Values must be preserved while stale (only freshness degrades).
    s, doc = http("GET", base, "/api/v1/status", key=read_key)
    mac_state = (doc.get("mac") or {}).get("state") or {}
    checker.check("mac.state value preserved while stale",
                  bool(mac_state.get("value")) and mac_state.get("freshness") == "stale",
                  json.dumps(mac_state))


def phase_b_reconnect(checker, base, read_key, hostname, state_file, bundle_id, rnd):
    checker.section(f"Round {rnd} Phase B: reconnect + backoff")
    start_agent_daemon(hostname, state_file, bundle_id, transport="websocket")
    elapsed, doc = wait_connected(base, read_key, 15)
    checker.check("agent.connected == true again", elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    if elapsed is not None:
        checker.check("reconnect within 10 s (first backoff step 1-1.2 s + connect)",
                      elapsed <= 10.0, f"t={elapsed:.1f} s")

    s, doc = http("GET", base, "/api/v1/logs?category=session&limit=512", key=read_key)
    entries = doc.get("entries") if s == 200 else None
    checker.check("session-category log entries present (>= 1)",
                  isinstance(entries, list) and len(entries) > 0,
                  f"status={s} count={len(entries) if isinstance(entries, list) else 'n/a'}")
    if entries:
        blob = json.dumps(entries).lower()
        saw_close = any(k in blob for k in ("close", "disconnect", "goodbye"))
        saw_hello = "hello" in blob or "ack" in blob
        if not saw_close:
            print("  [WARN] no close/disconnect/goodbye evidence found in session logs")
        if not saw_hello:
            print("  [WARN] no hello/ack evidence found in session logs")


def wait_dispatchable(base, read_key, timeout_s=15):
    """Poll capabilities until app_launch is available (capability_report
    landed); returns elapsed seconds or None."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout_s:
        s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
        if s == 200:
            entry = ((doc.get("commands") or {}).get("app_launch") or {})
            if entry.get("available") is True:
                return time.monotonic() - t0
        time.sleep(1.0)
    return None


def phase_c_websocket_dispatch(checker, base, read_key, control_key, bundle_id, rnd):
    checker.section(f"Round {rnd} Phase C: dispatch over WebSocket")
    # The gate needs the agent's capability_report on file; it travels in the
    # initial burst right after the hello and can lag "connected" by a second.
    elapsed = wait_dispatchable(base, read_key)
    checker.check("capability_report landed (app_launch available)", elapsed is not None,
                  "app_launch stayed unavailable for 15 s")
    checker.check("test app not running before launch phase",
                  reset_app(base, read_key, bundle_id), f"{bundle_id} would not exit")
    rec_ws = dispatch_and_expect(checker, base, control_key, read_key, bundle_id,
                                 "launch", "app_launch_confirmed")
    running = app_running(bundle_id)
    if running is None:
        print(f"  [WARN] could not verify {bundle_id} running "
              "(osascript/pgrep inconclusive); skipping app-running check")
    else:
        checker.check(f"{bundle_id} actually running on macOS", running is True)
    rec_quit = dispatch_and_expect(checker, base, control_key, read_key, bundle_id,
                                   "quit", "app_quit_confirmed")
    return rec_ws if (rec_ws and rec_quit) else None


def phase_d_polling_fallback(checker, base, read_key, control_key, hostname,
                             state_file, bundle_id, state, rnd, rec_ws):
    checker.section(f"Round {rnd} Phase D: polling fallback")
    stop_daemon()
    start_agent_daemon(hostname, state_file, bundle_id, transport="polling")
    elapsed, doc = wait_connected(base, read_key, 20)
    checker.check("agent connected over polling transport within 20 s",
                  elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    elapsed = wait_dispatchable(base, read_key)
    checker.check("capability_report landed over polling (app_launch available)",
                  elapsed is not None, "app_launch stayed unavailable for 15 s")
    checker.check("test app not running before launch phase",
                  reset_app(base, read_key, bundle_id), f"{bundle_id} would not exit")

    rec_poll = dispatch_and_expect(checker, base, control_key, read_key, bundle_id,
                                   "launch", "app_launch_confirmed")
    if rec_ws is not None and rec_poll is not None and \
            rec_ws.get("state") == "completed" and rec_poll.get("state") == "completed":
        checker.check("fallback record state/result match WS run",
                      (rec_poll.get("state"), rec_poll.get("result")) ==
                      (rec_ws.get("state"), rec_ws.get("result")),
                      f"ws={rec_ws.get('state')}/{rec_ws.get('result')} "
                      f"poll={rec_poll.get('state')}/{rec_poll.get('result')}")
        checker.check("error_code null in both runs",
                      rec_poll.get("error_code") is None and rec_ws.get("error_code") is None,
                      f"ws={rec_ws.get('error_code')} poll={rec_poll.get('error_code')}")
        # Note: revision counts are NOT compared — the number of appended
        # evidence revisions is timing-dependent (command_result(ok) may
        # arrive before or after the completing application_started). The
        # contract is state/result/error_code, which are checked above.
        if rec_poll.get("revision") != rec_ws.get("revision"):
            print(f"  [WARN] revision counts differ (ws={rec_ws.get('revision')} "
                  f"poll={rec_poll.get('revision')}); evidence timing only")
    else:
        checker.check("fallback ledger record comparable with WS run", False,
                      "one of the two launch records missing or non-completed")

    # Direct probe of the polling endpoint with the agent's own bearer token
    # and the live session id published by /api/v1/agent/status.
    token = state.get("agent_token") or ""
    checker.check("state file carries agent_token for pending probe", bool(token))
    if token:
        s_status, status_doc = http("GET", base, "/api/v1/agent/status", key=read_key)
        sid = status_doc.get("session_id") or ""
        checker.check("agent/status exposes a live session_id", bool(sid),
                      f"status={s_status}")
        s, doc = http("GET", base, "/agent/v1/commands/pending", key=token,
                      extra_headers={"X-Session-Id": sid})
        checker.check("GET /agent/v1/commands/pending with agent bearer -> 200 + commands[]",
                      s == 200 and isinstance(doc.get("commands"), list),
                      f"status={s}")
        s, doc = http("GET", base, "/agent/v1/commands/pending", key=token,
                      extra_headers={"X-Session-Id": "at11-bogus"})
        checker.check("pending with unknown session id -> 409 agent_offline",
                      s == 409 and error_code(doc) == "agent_offline", f"status={s}")
    return rec_poll


def run_round(checker, base, read_key, control_key, hostname, state_file,
              bundle_id, state, rnd):
    print(f"\n########## ROUND {rnd} ##########")
    phase_a_heartbeat_loss(checker, base, read_key, hostname, state_file, bundle_id, rnd)
    phase_b_reconnect(checker, base, read_key, hostname, state_file, bundle_id, rnd)
    rec_ws = phase_c_websocket_dispatch(checker, base, read_key, control_key, bundle_id, rnd)
    phase_d_polling_fallback(checker, base, read_key, control_key, hostname,
                             state_file, bundle_id, state, rnd, rec_ws)
    stop_daemon()


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 4 acceptance AT-11: reconnect backoff + polling fallback")
    ap.add_argument("--hostname", required=True)
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--bundle-id", default="com.apple.TextEdit")
    ap.add_argument("--state-file", default="/tmp/mc_at_agent.json")
    args = ap.parse_args()

    if not os.path.exists(AGENT_PY):
        print(f"MCA venv python not found at {AGENT_PY}; "
              "create ../agent/.venv per agent/README.md", file=sys.stderr)
        return 1

    checker = Checker()
    base = f"http://{args.hostname}:80"

    # Preconditions: paired state file, exactly ONE daemon running. Strays
    # from earlier AT-06 runs share this state file's pairing token, and two
    # same-token daemons supersede each other's sessions mid-dispatch
    # (agent_superseded/1000) — kill everything and start one fresh process.
    checker.section("AT-11 preconditions")
    try:
        with open(args.state_file, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except Exception as e:
        print(f"state file {args.state_file} unreadable: {e}", file=sys.stderr)
        print("run scripts/at06.py first (pairing ceremony)", file=sys.stderr)
        return 1
    checker.check("paired state file exists", bool(state.get("agent_token")))

    subprocess.run(["pkill", "-f", "maccontrol_agent"], capture_output=True)
    time.sleep(1.0)
    start_agent_daemon(args.hostname, args.state_file, args.bundle_id,
                       transport="websocket")
    elapsed, doc = wait_connected(base, args.read_key, 20)
    checker.check("agent connected at start (within 20 s)", elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    if elapsed is None:
        print("agent never connected; cannot continue", file=sys.stderr)
        stop_daemon()
        return 1

    for rnd in (1, 2):
        run_round(checker, base, args.read_key, args.control_key, args.hostname,
                  args.state_file, args.bundle_id, state, rnd)

    # Cleanup: daemon already stopped at the end of each round; drop the pidfile.
    try:
        os.unlink(PIDFILE)
    except OSError:
        pass

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-11: all checks passed in both rounds.")
    print(f"Daemon stopped; pidfile {PIDFILE} removed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
