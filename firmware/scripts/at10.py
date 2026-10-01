#!/usr/bin/env python3
"""MacControl Phase 6 acceptance test AT-10 (spec section 16.2): allowlist
enforcement and RBAC.

AT-10 (spec): allowlist contains one bundle ID. Launch the allowlisted app;
launch a non-allowlisted app; submit a CONTROL-scope request for an ADMIN
write. Expected: the allowlisted launch completes via correlated
command_ack(action=launch_app) plus matching application_started(bundle_id)
evidence; the non-allowlisted launch fails deterministically (409
app_not_allowlisted, pre-ledger, no record created); the RBAC matrix is
enforced with 403 for scope violations. Traces: Ch. 11.2, 13.1, 13.3.

The whole flow runs TWICE consecutively inside one invocation (spec 16
milestone rule); every check must pass in both rounds.

Preconditions: AT-06 has run and /tmp/mc_at_agent.json holds the paired state
(agent_token included). Exactly ONE fresh MCA daemon is started here, the
same preconditions style as at11.py.

Phases per round:
  A. Allowlisted launch — POST /api/v1/commands {type: app_launch}; poll
     GET /api/v1/commands/{id} to terminal; require completed /
     app_launch_confirmed (FAIL on unconfirmed/hid_only — AT-10 runs paired
     and connected) and correlated command_ack(launch_app) +
     application_started(bundle_id) evidence in the record or the
     command-category log entries for that command_id.
  B. Cleanup — app_quit to terminal, so the user's Mac is left clean.
  C. Non-allowlisted launch — deterministic pre-ledger refusal: 409
     app_not_allowlisted and the ledger head is unchanged. (Spec 11.2.1
     validates the ESP32 monitored registry BEFORE the MCA allowlist, so the
     probe bundle must be registry-registered but not allowlisted to hit
     app_not_allowlisted; if the device answers app_not_registered instead,
     re-run with --non-allowlisted-bundle naming such an app.)
  D. RBAC — CONTROL key attempting an ADMIN write (PUT
     /api/v1/system/unlock_password) -> 403 forbidden; READ key attempting a
     CONTROL submission (POST /api/v1/commands) -> 403 forbidden.

The MCA daemon is stopped and the pidfile removed at the end.

Usage:
  python3 at10.py --hostname mac-a1b2c3.local \
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


def start_agent_daemon(hostname, state_file, bundle_id):
    # Same stray-sweep discipline as at11/at06: two daemons sharing one state
    # file hold the SAME pairing token and supersede each other's sessions
    # (agent_superseded/1000), killing in-flight dispatch evidence.
    subprocess.run(["pkill", "-f", "maccontrol_agent"], capture_output=True)
    time.sleep(1.0)
    cmd = [AGENT_PY, "-m", "maccontrol_agent", "--hostname", hostname,
           "--state-file", state_file, "--allow", bundle_id,
           "--enable-launch", "--enable-quit", "--headless"]
    logfh = open(DAEMON_LOG, "ab", buffering=0)
    proc = subprocess.Popen(cmd, stdout=logfh, stderr=subprocess.STDOUT,
                            start_new_session=True, cwd=AGENT_DIR)
    logfh.close()
    PROCS[proc.pid] = proc
    with open(PIDFILE, "w", encoding="utf-8") as pf:
        pf.write("%d\n" % proc.pid)
    return proc.pid


def stop_daemon():
    """SIGTERM the daemon from the pidfile; wait for exit; remove the pidfile."""
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
    try:
        os.remove(PIDFILE)
    except OSError:
        pass
    if pid is not None:
        PROCS.pop(pid, None)


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


def poll_command_terminal(base, key, command_id, timeout_s=40):
    deadline = time.monotonic() + timeout_s
    rec = None
    while time.monotonic() < deadline:
        s, rec = http("GET", base, f"/api/v1/commands/{command_id}", key=key)
        if s == 200 and rec.get("state") in TERMINAL_STATES:
            return rec
        time.sleep(1.0)
    return rec if isinstance(rec, dict) else None


def reset_app(base, read_key, bundle_id):
    """Test hygiene: ensure the app is NOT running before a launch phase,
    and that the agent's monitor has OBSERVED the exit — a launch inside the
    monitor's blind window, or of an already-running app, produces no
    application_started delta and the predicate correctly times out."""
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
            rc = subprocess.run(["pgrep", "-x", name], capture_output=True)
            if rc.returncode != 0:
                return True
        time.sleep(1.0)
    return False


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


def collect_evidence_blob(base, read_key, command_id, rec):
    """Evidence correlation surface for a command: the record's evidence
    array plus the command-category log entries carrying its command_id.
    (The ledger may store bare event identifiers, so the log entries are the
    fallback correlation source.)"""
    blobs = [json.dumps(rec.get("evidence") or [])]
    s, doc = http("GET", base, "/api/v1/logs?category=command&limit=512", key=read_key)
    if s == 200:
        matching = [e for e in (doc.get("entries") or [])
                    if e.get("command_id") == command_id]
        blobs.append(json.dumps(matching))
    return "\n".join(blobs)


def submit_app_command(checker, base, control_key, bundle_id, cmd_type):
    """POST {type: app_launch|app_quit} via the generic command API; returns
    (command_id, status, doc)."""
    s, doc = http("POST", base, "/api/v1/commands", key=control_key,
                  body={"type": cmd_type, "parameters": {"bundle_id": bundle_id}})
    checker.check(f"POST /api/v1/commands {cmd_type} -> 202",
                  s == 202 and bool(doc.get("command_id")), f"status={s} body={doc}")
    return (doc.get("command_id") if s == 202 else None), s, doc


def phase_a_allowlisted_launch(checker, base, read_key, control_key, bundle_id, rnd):
    checker.section(f"Round {rnd} Phase A: allowlisted launch ({bundle_id})")
    checker.check("test app not running before launch phase",
                  reset_app(base, read_key, bundle_id), f"{bundle_id} would not exit")
    command_id, _, _ = submit_app_command(checker, base, control_key, bundle_id,
                                          "app_launch")
    if not command_id:
        return
    rec = poll_command_terminal(base, read_key, command_id, 40)
    checker.check("launch reaches terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    # FAIL on unconfirmed/hid_only: AT-10 runs paired+connected, so Mode A
    # terminal states mean the evidence channel was lost — print the actual
    # terminal result rather than guessing.
    checker.check("terminal completed/app_launch_confirmed",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "app_launch_confirmed",
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')}")

    blob = collect_evidence_blob(base, read_key, command_id, rec or {})
    checker.check("correlated command_ack(action=launch_app) evidence",
                  "command_ack" in blob and "launch_app" in blob,
                  "no command_ack/launch_app in record evidence or command logs")
    checker.check(f"matching application_started({bundle_id}) evidence",
                  "application_started" in blob and bundle_id in blob,
                  "no application_started with matching bundle_id in evidence/logs")

    running = app_running(bundle_id)
    if running is None:
        print(f"  [WARN] could not verify {bundle_id} running "
              "(osascript/pgrep inconclusive); skipping app-running check")
    else:
        checker.check(f"{bundle_id} actually running on macOS", running is True)

    # Cleanup inside the phase: quit the app so the Mac is left clean even
    # if a later phase fails the round.
    checker.section(f"Round {rnd} Phase B: cleanup quit ({bundle_id})")
    qid, _, _ = submit_app_command(checker, base, control_key, bundle_id, "app_quit")
    if qid:
        qrec = poll_command_terminal(base, read_key, qid, 40)
        checker.check("quit reaches terminal completed/app_quit_confirmed",
                      qrec is not None and qrec.get("state") == "completed" and
                      qrec.get("result") == "app_quit_confirmed",
                      f"state={qrec and qrec.get('state')} result={qrec and qrec.get('result')}")


def run_round(checker, base, read_key, control_key, bundle_id, non_allowlisted, rnd):
    print(f"\n########## ROUND {rnd} ##########")
    phase_a_allowlisted_launch(checker, base, read_key, control_key, bundle_id, rnd)
    checker.section(f"Round {rnd} Phase C: non-allowlisted launch ({non_allowlisted})")
    _non_allowlisted_body(checker, base, read_key, control_key, non_allowlisted)
    checker.section(f"Round {rnd} Phase D: RBAC matrix (403 for scope violations)")
    _rbac_body(checker, base, read_key, control_key)


def _non_allowlisted_body(checker, base, read_key, control_key, non_allowlisted):
    # The 409 family is pre-ledger (spec 12.3.1): the refusal must not
    # create a record. Compare the ledger head before and after.
    s, before = http("GET", base, "/api/v1/commands?limit=1", key=read_key)
    before_id = (before.get("commands") or [{}])[0].get("command_id") if s == 200 else None
    s, doc = http("POST", base, "/api/v1/commands", key=control_key,
                  body={"type": "app_launch",
                        "parameters": {"bundle_id": non_allowlisted}})
    code = error_code(doc)
    checker.check("non-allowlisted launch -> 409 app_not_allowlisted",
                  s == 409 and code == "app_not_allowlisted",
                  f"status={s} code={code} body={doc}. "
                  "Spec 11.2.1 validates the ESP32 monitored registry BEFORE the "
                  "MCA allowlist: app_not_registered means the probe bundle is not "
                  "registry-registered — re-run with --non-allowlisted-bundle "
                  "naming a registered-but-not-allowlisted app to exercise "
                  "app_not_allowlisted specifically.")
    s, after = http("GET", base, "/api/v1/commands?limit=1", key=read_key)
    after_id = (after.get("commands") or [{}])[0].get("command_id") if s == 200 else None
    checker.check("no ledger record created by the refusal",
                  before_id is not None and after_id == before_id,
                  f"before={before_id} after={after_id}")


def _rbac_body(checker, base, read_key, control_key):
    # CONTROL attempting an ADMIN write (spec 13.1 matrix: unlock_password
    # set is ADMIN). The body is well-formed; role is the only failure cause.
    s, doc = http("PUT", base, "/api/v1/system/unlock_password", key=control_key,
                  body={"password": "at10-rbac-probe-01234567"})
    checker.check("CONTROL PUT /api/v1/system/unlock_password -> 403 forbidden",
                  s == 403 and error_code(doc) == "forbidden",
                  f"status={s} body={doc}")
    # READ attempting a CONTROL submission (spec 13.1 matrix: POST
    # /api/v1/commands is CONTROL minimum).
    s, doc = http("POST", base, "/api/v1/commands", key=read_key,
                  body={"type": "lock", "parameters": {}})
    checker.check("READ POST /api/v1/commands -> 403 forbidden",
                  s == 403 and error_code(doc) == "forbidden",
                  f"status={s} body={doc}")


def resolve_bundle_id(checker, base, read_key, hostname, state_file, requested):
    """Pick the allowlisted bundle id: CLI --bundle-id wins; otherwise take
    the first entry of the agent's capability_report allowlisted_apps (the
    daemon was started with --allow, so the report is the authority); fall
    back to com.apple.TextEdit. Restarts the daemon once if the fallback is
    not what the report carries."""
    s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
    allowlisted = ((doc.get("agent") or {}).get("allowlisted_apps") or []) if s == 200 else []
    checker.check("agent reports a non-empty allowlist (AT-10 precondition)",
                  len(allowlisted) >= 1, f"allowlisted_apps={allowlisted}")
    if requested:
        checker.check(f"{requested} present in capability_report allowlisted_apps",
                      requested in allowlisted, json.dumps(allowlisted))
        return requested
    if allowlisted:
        bundle_id = allowlisted[0]
        if bundle_id != "com.apple.TextEdit":
            # The daemon was started with the TextEdit default; restart it
            # so its --allow list matches the exercised bundle id.
            print(f"  ... restarting harness agent with --allow {bundle_id}")
            start_agent_daemon(hostname, state_file, bundle_id)
            wait_connected(base, read_key, 20)
        return bundle_id
    return "com.apple.TextEdit"


def pick_non_allowlisted(allowlisted, requested):
    if requested:
        return requested
    for cand in ("com.apple.Safari", "com.apple.Calculator", "com.apple.Notes",
                 "com.apple.Preview", "com.apple.Mail"):
        if cand not in allowlisted:
            return cand
    return "com.example.notallowlisted"


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 6 acceptance AT-10: allowlist enforcement + RBAC")
    ap.add_argument("--hostname", required=True)
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--bundle-id", default=None,
                    help="allowlisted bundle id to exercise (default: first entry of the agent capability_report)")
    ap.add_argument("--non-allowlisted-bundle", default=None,
                    help="bundle id expected to fail with app_not_allowlisted "
                         "(default: a common Apple app absent from the allowlist)")
    ap.add_argument("--state-file", default="/tmp/mc_at_agent.json")
    args = ap.parse_args()

    if not os.path.exists(AGENT_PY):
        print(f"MCA venv python not found at {AGENT_PY}; "
              "create ../agent/.venv per agent/README.md", file=sys.stderr)
        return 1

    checker = Checker()
    base = f"http://{args.hostname}:80"

    checker.section("AT-10 preconditions")
    try:
        with open(args.state_file, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except Exception as e:
        print(f"state file {args.state_file} unreadable: {e}", file=sys.stderr)
        print("run scripts/at06.py first (pairing ceremony)", file=sys.stderr)
        return 1
    checker.check("paired state file exists", bool(state.get("agent_token")))

    start_agent_daemon(args.hostname, args.state_file,
                       args.bundle_id or "com.apple.TextEdit")
    elapsed, doc = wait_connected(base, args.read_key, 20)
    checker.check("agent connected at start (within 20 s)", elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    if elapsed is None:
        print("agent never connected; cannot continue", file=sys.stderr)
        stop_daemon()
        return 1

    bundle_id = resolve_bundle_id(checker, base, args.read_key, args.hostname,
                                  args.state_file, args.bundle_id)
    allowlisted = []
    s, doc = http("GET", base, "/api/v1/capabilities", key=args.read_key)
    if s == 200:
        allowlisted = ((doc.get("agent") or {}).get("allowlisted_apps") or [])
    non_allowlisted = pick_non_allowlisted(allowlisted, args.non_allowlisted_bundle)

    elapsed = wait_dispatchable(base, args.read_key)
    checker.check("capability_report landed (app_launch available)", elapsed is not None,
                  "app_launch stayed unavailable for 15 s")
    if elapsed is None:
        stop_daemon()
        return 1

    for rnd in (1, 2):
        run_round(checker, base, args.read_key, args.control_key,
                  bundle_id, non_allowlisted, rnd)

    # Cleanup: daemon stopped, pidfile removed, app already quit inside each
    # round; the pkill backstop covers a quit that failed mid-round.
    stop_daemon()
    subprocess.run(["pkill", "-x", bundle_id.rsplit(".", 1)[-1]], capture_output=True)

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-10: all checks passed in both rounds.")
    print(f"Daemon stopped; pidfile {PIDFILE} removed; {bundle_id} quit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
