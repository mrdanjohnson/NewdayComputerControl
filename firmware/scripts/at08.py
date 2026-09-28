#!/usr/bin/env python3
"""MacControl Phase 5 acceptance test AT-08 (spec section 16.2): verified
restart and wake. Two modes.

AT-08 (spec): submit `restart`; agent_goodbye or expected offline inside the
restart window [3 s, 60 s]; new-session agent_hello with CHANGED boot_id plus
initial burst reporting system_state=awake; terminal completed/
restart_confirmed within 180 s. Then submit `sleep` (completes
sleep_confirmed at window close, <= 90 s), then submit `wake` — the ENDPOINT
wakes the sleeping target Mac with a HID key-tap (spec 8.1.2); at wake the
MCA session is necessarily down, so wake's evidence is a POST-DISPATCH
new-session hello + awake burst (heartbeat alone must NOT complete it) ->
completed/wake_confirmed within 120 s. Traces: Ch. 8, 5.3.

MODE 1 — in-process (default; future two-Mac topology): this harness runs on
Mac A while the ESP32's HID cable attaches to a DIFFERENT target Mac, so the
harness survives the target's sleep/reboot and the whole restart -> sleep ->
wake cycle runs unattended. The flow runs N consecutive rounds inside one
invocation (spec 16 rule: --rounds, default 2). Never open a serial port
here — it reboots the bench S3.

MODE 2 — co-located phased (--phase; THIS bench today): the HID cable
attaches to the SAME Mac that runs the harness, so sleep/restart freeze the
harness. Human-in-the-loop: the harness dispatches, prints the exact next
command, and exits; the Mac sleeps/reboots; a human observes/wakes/reports;
the harness verifies post-hoc in the next phase. One cycle per invocation
chain:

  python3 at08.py ... --phase restart-dispatch    # harness freezes at reboot
  python3 at08.py ... --phase restart-verify      # run after logging back in
  python3 at08.py ... --phase sleep-dispatch      # harness freezes at sleep
  python3 at08.py ... --phase sleep-verify        # run after waking + login
  python3 at08.py ... --phase wake                # Mac already awake

The spec-16 two-consecutive-runs gate is satisfied by running the WHOLE CHAIN
twice, with a separate --state-file per chain (e.g. /tmp/at08_state1.json,
/tmp/at08_state2.json) — the wake phase prints a per-chain gate summary and
reminds you. `--resume` prints the next phase the state file says to run.

Post-hoc deadline checks (phased mode): the record's `dispatched_at` comes
from the ledger; the terminal transition time is bounded by the newest
command-category log entry carrying the command_id
(GET /api/v1/logs?category=command, page with since_seq). Both timestamps
are the ESP32's own clock, so they agree even pre-SNTP; precision is bounded
by log write timing (~1 s; the ms field is nominal since epoch_seconds() is
1 s-granular) and an SNTP re-sync jump mid-window would shift the bound —
the device-side verdict (terminal state/result, evaluated on monotonic
sidecars) remains the authoritative pass/fail.

All window/deadline math in in-process mode is measured from the dispatch
timestamp on time.monotonic(); the key timestamps (dispatch, offline onset,
hello, terminal) are logged so the [3, 60] window and deadline headroom are
visible.

Usage (mode 1):
  python3 at08.py --hostname control-graphics.local \
      --read-key mck_... --control-key mck_...

Usage (mode 2): see the command chain above; --resume reprints it.

Exit code 0 = all checks pass (all rounds / the invoked phase, with prior
phases in the state file already passing).
"""

import argparse
import json
import os
import shlex
import sys
import time
from datetime import datetime, timezone

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

TERMINAL_STATES = ("completed", "failed", "timed_out", "unconfirmed")

WINDOW_OPEN_S = 3.0     # spec 8: offline onset earlier than this is never
WINDOW_CLOSE_S = 60.0   #   attributed to the dispatch
RESTART_DEADLINE_S = 180.0
SLEEP_DEADLINE_S = 90.0
WAKE_DEADLINE_S = 120.0

POLL_RECORD_S = 2.5   # READ bucket: <= 1 request / 2-3 s
POLL_STATUS_S = 2.0
BACKOFF_429_S = 5.0
BACKOFF_429_MAX = 8   # consecutive 429s before giving up on a request


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


def poll_get(base, path, key):
    """GET with 429 backoff; returns (status, doc). A 1/s poller rides the
    READ rate bucket — back off instead of hammering."""
    for _ in range(BACKOFF_429_MAX):
        s, doc = http("GET", base, path, key=key)
        if s != 429:
            return s, doc
        print(f"  ... 429 rate-limited on {path}; backing off {BACKOFF_429_S:.0f} s")
        time.sleep(BACKOFF_429_S)
    return 429, {}


def wait_connected(base, read_key, timeout_s):
    """Poll capabilities until agent.connected == true. Returns (elapsed, doc)."""
    t0 = time.monotonic()
    deadline = t0 + timeout_s
    doc = {}
    while time.monotonic() < deadline:
        s, doc = poll_get(base, "/api/v1/capabilities", read_key)
        if s == 200 and (doc.get("agent") or {}).get("connected") is True:
            return time.monotonic() - t0, doc
        time.sleep(POLL_STATUS_S)
    return None, doc


def agent_status(base, read_key):
    s, doc = poll_get(base, "/api/v1/agent/status", read_key)
    return doc if s == 200 else None


def wait_session(base, read_key, active, timeout_s):
    """Poll agent/status until session_active == `active`. Returns
    (elapsed, doc) or (None, last_doc)."""
    t0 = time.monotonic()
    doc = None
    while time.monotonic() - t0 < timeout_s:
        doc = agent_status(base, read_key)
        if doc is not None and doc.get("session_active") is active:
            return time.monotonic() - t0, doc
        time.sleep(POLL_STATUS_S)
    return None, doc


def wait_offline_onset(base, read_key, t_disp, timeout_s):
    """Watch for the target going down after a power dispatch: the first
    sample where the agent session drops OR the USB device link to the host
    stops signaling (DSTS.SUSPSTS — a sleeping/off host suspends the link).
    Returns (t_onset, doc) or (None, last_doc), t_onset seconds after
    t_disp."""
    deadline = time.monotonic() + timeout_s
    doc = None
    while time.monotonic() < deadline:
        doc = agent_status(base, read_key)
        if doc is not None:
            usb_down = (doc.get("usb") or {}).get("link") == "down"
            if doc.get("session_active") is False or usb_down:
                return time.monotonic() - t_disp, doc
        time.sleep(POLL_STATUS_S)
    return None, doc


def poll_command_terminal(base, key, command_id, timeout_s):
    deadline = time.monotonic() + timeout_s
    rec = None
    while time.monotonic() < deadline:
        s, rec = poll_get(base, f"/api/v1/commands/{command_id}", key)
        if s == 200 and isinstance(rec, dict) and rec.get("state") in TERMINAL_STATES:
            return rec
        time.sleep(POLL_RECORD_S)
    return rec if isinstance(rec, dict) else None


def dispatch_system(checker, base, control_key, name):
    """POST /api/v1/system/{name}; returns command_id or None."""
    s, doc = http("POST", base, f"/api/v1/system/{name}", key=control_key)
    checker.check(f"POST /api/v1/system/{name} -> 202",
                  s == 202 and bool(doc.get("command_id")), f"status={s}")
    return doc.get("command_id") if s == 202 else None


def phase_restart(checker, base, read_key, control_key, rnd):
    checker.section(f"Round {rnd} A: verified restart (window [3, 60] s, deadline 180 s)")
    _, doc = wait_session(base, read_key, True, 5)
    st = doc or {}
    baseline_boot = st.get("boot_id")
    checker.check("precondition: live session with known boot_id for the comparison",
                  st.get("session_active") is True and bool(baseline_boot),
                  f"session_active={st.get('session_active')} boot_id={baseline_boot!r}")
    if not baseline_boot:
        return
    print(f"  ... baseline boot_id = {baseline_boot}")

    command_id = dispatch_system(checker, base, control_key, "restart")
    if not command_id:
        return
    t_disp = time.monotonic()
    print(f"  ... restart dispatched at t=0 (command_id={command_id})")

    t_onset, _ = wait_offline_onset(base, read_key, t_disp, WINDOW_CLOSE_S + 10)
    checker.check("offline onset observed (session lost or USB link down)",
                  t_onset is not None, "target still looked up after 70 s")
    if t_onset is not None:
        print(f"  ... offline onset at t={t_onset:.1f} s")
        checker.check(f"offline onset inside window [{WINDOW_OPEN_S:.0f}, {WINDOW_CLOSE_S:.0f}] s",
                      WINDOW_OPEN_S <= t_onset <= WINDOW_CLOSE_S,
                      f"onset={t_onset:.1f} s after dispatch")

    # Phase 2: new session with CHANGED boot_id + awake burst, by t_disp+180.
    deadline = t_disp + RESTART_DEADLINE_S
    t_hello = t_awake = None
    new_boot = None
    while time.monotonic() < deadline:
        st = agent_status(base, read_key)
        if st and st.get("session_active") is True:
            boot = st.get("boot_id")
            if boot and boot != baseline_boot:
                if t_hello is None:
                    t_hello = time.monotonic() - t_disp
                    new_boot = boot
                    print(f"  ... new-session hello at t={t_hello:.1f} s "
                          f"(boot_id {baseline_boot} -> {boot})")
                if (st.get("system") or {}).get("state") == "awake":
                    t_awake = time.monotonic() - t_disp
                    print(f"  ... initial burst reports system.state=awake at t={t_awake:.1f} s")
                    break
        time.sleep(POLL_STATUS_S)
    checker.check("post-restart session boot_id CHANGED from baseline",
                  new_boot is not None, f"no new-session hello with changed boot_id "
                  f"within {RESTART_DEADLINE_S:.0f} s")
    checker.check("system.state reached awake in the new session",
                  t_awake is not None, "initial burst never reported awake")

    # Ledger: terminal completed/restart_confirmed within 180 s of dispatch.
    remaining = max(1.0, deadline - time.monotonic())
    rec = poll_command_terminal(base, read_key, command_id, remaining)
    t_term = time.monotonic() - t_disp
    checker.check("restart record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"restart completed/restart_confirmed within {RESTART_DEADLINE_S:.0f} s",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "restart_confirmed" and
                  rec.get("error_code") is None and t_term <= RESTART_DEADLINE_S,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')} terminal_at={t_term:.1f} s")
    print(f"  ... restart record terminal at t={t_term:.1f} s "
          f"(deadline {RESTART_DEADLINE_S:.0f} s)")


def phase_sleep(checker, base, read_key, control_key, rnd):
    checker.section(f"Round {rnd} B: verified sleep (window [3, 60] s, deadline 90 s)")
    _, doc = wait_session(base, read_key, True, 10)
    st = doc or {}
    checker.check("precondition: session live and system awake before sleep",
                  st.get("session_active") is True and
                  (st.get("system") or {}).get("state") == "awake",
                  f"session_active={st.get('session_active')} "
                  f"system.state={(st.get('system') or {}).get('state')}")

    command_id = dispatch_system(checker, base, control_key, "sleep")
    if not command_id:
        return
    t_disp = time.monotonic()
    print(f"  ... sleep dispatched at t=0 (command_id={command_id})")

    t_onset, _ = wait_offline_onset(base, read_key, t_disp, WINDOW_CLOSE_S + 10)
    checker.check("offline onset observed (session lost or USB link down)",
                  t_onset is not None, "target still looked up after 70 s")
    if t_onset is not None:
        print(f"  ... offline onset at t={t_onset:.1f} s")
        checker.check(f"offline onset inside window [{WINDOW_OPEN_S:.0f}, {WINDOW_CLOSE_S:.0f}] s",
                      WINDOW_OPEN_S <= t_onset <= WINDOW_CLOSE_S,
                      f"onset={t_onset:.1f} s after dispatch")

    # Completion is evaluated at window close (60 s) with no reconnect; a
    # reconnect before close would already have terminated the record
    # failed/unexpected_wake, which the completed check below catches.
    deadline = t_disp + SLEEP_DEADLINE_S
    rec = poll_command_terminal(base, read_key, command_id,
                                max(1.0, deadline - time.monotonic()))
    t_term = time.monotonic() - t_disp
    checker.check("sleep record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"sleep completed/sleep_confirmed within {SLEEP_DEADLINE_S:.0f} s",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "sleep_confirmed" and
                  rec.get("error_code") is None and t_term <= SLEEP_DEADLINE_S,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')} terminal_at={t_term:.1f} s")
    print(f"  ... sleep record terminal at t={t_term:.1f} s "
          f"(window close {WINDOW_CLOSE_S:.0f} s, deadline {SLEEP_DEADLINE_S:.0f} s)")


def phase_wake(checker, base, read_key, control_key, rnd):
    checker.section(f"Round {rnd} C: verified wake (deadline 120 s)")
    # Capture the incumbent session id: wake's evidence must be a
    # POST-DISPATCH new session (spec 8.1.2 — a heartbeat on the incumbent
    # session must NOT complete it). After a real sleep the session is dead;
    # the launchd agent on the target auto-reconnects after the HID key-tap.
    st = agent_status(base, read_key) or {}
    incumbent_sid = st.get("session_id")
    print(f"  ... incumbent session_id = {incumbent_sid!r} "
          f"(session_active={st.get('session_active')})")

    command_id = dispatch_system(checker, base, control_key, "wake")
    if not command_id:
        return
    t_disp = time.monotonic()
    print(f"  ... wake dispatched at t=0 (command_id={command_id})")

    deadline = t_disp + WAKE_DEADLINE_S
    t_hello = None
    new_sid = None
    while time.monotonic() < deadline:
        st = agent_status(base, read_key)
        if st and st.get("session_active") is True:
            sid = st.get("session_id")
            boot_ok = bool(st.get("boot_id"))
            if sid and boot_ok and (incumbent_sid is None or sid != incumbent_sid):
                if (st.get("system") or {}).get("state") == "awake":
                    t_hello = time.monotonic() - t_disp
                    new_sid = sid
                    print(f"  ... post-dispatch new session {sid!r} + awake burst "
                          f"at t={t_hello:.1f} s")
                    break
        time.sleep(POLL_STATUS_S)
    checker.check("post-dispatch NEW-session hello with awake burst within 120 s",
                  t_hello is not None,
                  f"no qualifying session within {WAKE_DEADLINE_S:.0f} s "
                  f"(incumbent={incumbent_sid!r}, last new_sid={new_sid!r})")
    if t_hello is not None and incumbent_sid is not None:
        checker.check("wake hello arrived on a DIFFERENT session than pre-dispatch",
                      new_sid != incumbent_sid,
                      f"incumbent={incumbent_sid!r} new={new_sid!r}")

    remaining = max(1.0, deadline - time.monotonic())
    rec = poll_command_terminal(base, read_key, command_id, remaining)
    t_term = time.monotonic() - t_disp
    checker.check("wake record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"wake completed/wake_confirmed within {WAKE_DEADLINE_S:.0f} s",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "wake_confirmed" and
                  rec.get("error_code") is None and t_term <= WAKE_DEADLINE_S,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')} terminal_at={t_term:.1f} s")
    print(f"  ... wake record terminal at t={t_term:.1f} s "
          f"(deadline {WAKE_DEADLINE_S:.0f} s)")


def run_round(checker, base, read_key, control_key, rnd):
    print(f"\n########## ROUND {rnd} ##########")
    phase_restart(checker, base, read_key, control_key, rnd)
    phase_sleep(checker, base, read_key, control_key, rnd)
    phase_wake(checker, base, read_key, control_key, rnd)
    # Round-end state: target awake, session live — ready for the next round.


# ---------------------------------------------------------------------------
# Mode 2: co-located phased mode (human-in-the-loop).
#
# The HID cable is on the SAME Mac as the harness, so restart/sleep freeze
# the harness itself. Each --phase invocation does one slice: preconditions,
# (optional) dispatch, save state, print the exact next command, exit. The
# sleep/restart verification slices run post-hoc after a human logs back in.
# ---------------------------------------------------------------------------

PHASE_ORDER = ("restart-dispatch", "restart-verify", "sleep-dispatch",
               "sleep-verify", "wake")


def parse_iso(ts):
    """ESP32 ISO-8601 (…Z) -> epoch seconds."""
    return datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()


def fmt_wall(epoch_s):
    return datetime.fromtimestamp(epoch_s).strftime("%H:%M:%S")


def load_state(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def save_state(path, state):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh, indent=2)
    os.replace(tmp, path)


def build_invocation(args, phase):
    """The exact command to run next, keys and all (copy-paste)."""
    parts = [os.path.abspath(sys.argv[0]), "--hostname", args.hostname,
             "--read-key", args.read_key, "--control-key", args.control_key,
             "--state-file", args.state_file, "--phase", phase]
    if args.base:
        parts += ["--base", args.base]
    return shlex.join(parts)


def print_next_command(args, phase, extra_lines=()):
    print("\n" + "=" * 72)
    print(f"  NEXT: run this after the step above completes:")
    print(f"    {build_invocation(args, phase)}")
    for line in extra_lines:
        print(f"  {line}")
    print("=" * 72)


def command_log_bounds(base, read_key, command_id):
    """Page the command-category log and collect ts bounds for every entry
    carrying command_id. Returns (oldest_ts, newest_ts, dropped, found).
    The newest matching entry's ts upper-bounds the terminal transition."""
    since = 0
    oldest = newest = None
    dropped = 0
    found = False
    for _ in range(20):  # page cap: 20 x 512 entries is far beyond the ring
        s, doc = poll_get(base,
                          f"/api/v1/logs?category=command&since_seq={since}&limit=512",
                          read_key)
        if s != 200:
            break
        entries = doc.get("entries") or []
        dropped += int(doc.get("dropped") or 0)
        max_seq = since
        for e in entries:
            seq = e.get("seq") or 0
            if seq > max_seq:
                max_seq = seq
            # Only the terminal transition bounds completion: post-terminal
            # evidence notes also carry the command_id and would otherwise
            # inflate the bound past the deadline.
            if (e.get("command_id") == command_id
                    and e.get("event") == "state_transition" and e.get("ts")):
                found = True
                ts = parse_iso(e["ts"])
                oldest = ts if oldest is None else min(oldest, ts)
                newest = ts if newest is None else max(newest, ts)
        since = max_seq
        if len(entries) < 512:
            break
        since += 1  # entries_since is strictly-greater; avoid a stuck cursor
    return oldest, newest, dropped, found


def check_deadline_posthoc(checker, base, read_key, command_id, rec,
                           deadline_s, label):
    """Post-hoc deadline: terminal transition within deadline_s of dispatch.
    Anchor = record dispatched_at (ledger, ESP32 clock); bound = newest
    command-log entry ts for this command_id (same clock)."""
    dispatched_at = (rec or {}).get("dispatched_at")
    if not dispatched_at:
        checker.check(f"{label} terminal transition within {deadline_s:.0f} s of dispatch",
                      False, "record has no dispatched_at")
        return None
    oldest, newest, dropped, found = command_log_bounds(base, read_key, command_id)
    if not found or newest is None:
        checker.check(f"{label} terminal transition within {deadline_s:.0f} s of dispatch",
                      False, "no command-log entries for this command_id "
                      "(128-slot ring evicted? run the verify phase sooner)")
        return None
    bound = newest - parse_iso(dispatched_at)
    checker.check(f"{label} terminal transition within {deadline_s:.0f} s of dispatch "
                  "(log-ts bound)", bound <= deadline_s,
                  f"bound={bound:.1f} s (dispatched_at={dispatched_at}, "
                  f"log_ts<=…{newest:.3f})")
    if dropped:
        print(f"  [WARN] {dropped} log entries were evicted before the oldest "
              "retained seq; the bound still holds (newest matching entry "
              "survived) but the dispatch-side cross-check may be missing")
    if oldest is not None:
        skew = oldest - parse_iso(dispatched_at)
        print(f"  ... oldest matching log entry (command_accepted) at "
              f"dispatched_at{'+' if skew >= 0 else ''}{skew:.1f} s "
              f"(submission-to-dispatch skew, informational)")
    return bound


def record_phase_result(state, phase, checker, failures_before, evidence=None):
    new_failures = checker.failures[failures_before:]
    entry = state.setdefault(phase, {})
    entry["status"] = "failed" if new_failures else "passed"
    entry["failures"] = new_failures
    if evidence:
        entry.update(evidence)
    return not new_failures


def get_record(base, read_key, command_id):
    s, rec = poll_get(base, f"/api/v1/commands/{command_id}", read_key)
    return rec if s == 200 and isinstance(rec, dict) else None


def phase_restart_dispatch(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase restart-dispatch: cache baseline, dispatch restart")
    state = load_state(args.state_file)
    _, doc = wait_session(base, args.read_key, True, 30)
    st = doc or {}
    baseline_boot = st.get("boot_id")
    baseline_sid = st.get("session_id")
    checker.check("precondition: live session with known boot_id",
                  st.get("session_active") is True and bool(baseline_boot),
                  f"session_active={st.get('session_active')} boot_id={baseline_boot!r}")
    if not baseline_boot:
        return 1
    print(f"  ... baseline boot_id={baseline_boot} session_id={baseline_sid}")

    print_next_command(args, "restart-verify",
                       ("THE MAC WILL REBOOT ~SECONDS AFTER DISPATCH.",
                        "When you are logged back in, run the command above."))
    command_id = dispatch_system(checker, base, args.control_key, "restart")
    if not command_id:
        return 1
    state["restart"] = {
        "command_id": command_id,
        "dispatched_wall": time.time(),
        "baseline_boot_id": baseline_boot,
        "baseline_session_id": baseline_sid,
        "agent_status_snapshot": st,
    }
    ok = record_phase_result(state, "restart-dispatch", checker, fb,
                             {"command_id": command_id})
    save_state(args.state_file, state)
    print(f"\n  restart dispatched (command_id={command_id}); state saved to "
          f"{args.state_file}. This Mac reboots shortly — the harness dies with it.")
    return 0 if ok else 1


def phase_restart_verify(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase restart-verify: post-reboot ledger + identity checks")
    state = load_state(args.state_file)
    restart = state.get("restart") or {}
    command_id = restart.get("command_id")
    baseline_boot = restart.get("baseline_boot_id")
    if not command_id or not baseline_boot:
        print("state file has no restart dispatch; run --phase restart-dispatch "
              "first (--resume prints the chain)", file=sys.stderr)
        return 1

    elapsed, doc = wait_session(base, args.read_key, True, 120)
    checker.check("agent session ACTIVE again within 120 s of this phase starting",
                  elapsed is not None, "no session after 120 s")
    st = doc or {}
    post_boot = st.get("boot_id")
    checker.check("system.state awake after reboot",
                  (st.get("system") or {}).get("state") == "awake",
                  f"system.state={(st.get('system') or {}).get('state')}")

    rec = get_record(base, args.read_key, command_id)
    checker.check("restart record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check("restart record completed/restart_confirmed/error_code null",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "restart_confirmed" and
                  rec.get("error_code") is None,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')}")
    checker.check("post-restart boot_id CHANGED from baseline",
                  bool(post_boot) and post_boot != baseline_boot,
                  f"baseline={baseline_boot} post={post_boot}")
    bound = check_deadline_posthoc(checker, base, args.read_key, command_id, rec,
                                   RESTART_DEADLINE_S, "restart")

    state["restart"].update({
        "post_boot_id": post_boot,
        "post_session_id": st.get("session_id"),
        "deadline_bound_s": bound,
    })
    ok = record_phase_result(state, "restart-verify", checker, fb)
    save_state(args.state_file, state)
    if ok:
        print_next_command(args, "sleep-dispatch",
                           ("The Mac is awake; the next phase puts it to SLEEP."))
    return 0 if ok else 1


def phase_sleep_dispatch(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase sleep-dispatch: dispatch sleep, print the no-wake window")
    state = load_state(args.state_file)
    _, doc = wait_session(base, args.read_key, True, 15)
    st = doc or {}
    checker.check("precondition: session live and system awake before sleep",
                  st.get("session_active") is True and
                  (st.get("system") or {}).get("state") == "awake",
                  f"session_active={st.get('session_active')} "
                  f"system.state={(st.get('system') or {}).get('state')}")

    no_wake_before = time.time() + 65.0  # window close 60 s + 5 s margin
    print_next_command(args, "sleep-verify", (
        "!" * 66,
        "  THE MAC WILL SLEEP IN ~2 s. DO NOT TOUCH IT — no key presses, no",
        f"  mouse, no lid — until {fmt_wall(no_wake_before)} (window close +5 s",
        "  margin). Waking it earlier fails the record (unexpected_wake).",
        f"  After {fmt_wall(no_wake_before)}: press any key to wake, log in,",
        "  then run the command above.",
    ))
    command_id = dispatch_system(checker, base, args.control_key, "sleep")
    if not command_id:
        return 1
    state["sleep"] = {
        "command_id": command_id,
        "dispatched_wall": time.time(),
        "no_wake_before_wall": no_wake_before,
    }
    ok = record_phase_result(state, "sleep-dispatch", checker, fb,
                             {"command_id": command_id})
    save_state(args.state_file, state)
    print(f"\n  sleep dispatched (command_id={command_id}); state saved. This "
          f"Mac sleeps shortly — the harness dies with it.")
    print(f"  DO NOT WAKE BEFORE {fmt_wall(no_wake_before)}.")
    return 0 if ok else 1


def phase_sleep_verify(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase sleep-verify: post-wake sleep record checks")
    state = load_state(args.state_file)
    sleep_st = state.get("sleep") or {}
    command_id = sleep_st.get("command_id")
    if not command_id:
        print("state file has no sleep dispatch; run --phase sleep-dispatch first",
              file=sys.stderr)
        return 1
    no_wake_before = sleep_st.get("no_wake_before_wall") or 0
    if no_wake_before and time.time() < no_wake_before:
        print(f"  [WARN] this phase started at {fmt_wall(time.time())}, before the "
              f"no-wake deadline {fmt_wall(no_wake_before)} — if you woke the Mac "
              "early the record will read failed/unexpected_wake")

    rec = get_record(base, args.read_key, command_id)
    checker.check("sleep record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    if rec is not None and rec.get("state") == "failed" and \
            rec.get("result") == "unexpected_wake":
        checker.check("sleep record completed/sleep_confirmed/error_code null", False,
                      "the host reconnected before window close — the human woke it "
                      "too early. Rerun the whole sleep phase: --phase sleep-dispatch")
    else:
        checker.check("sleep record completed/sleep_confirmed/error_code null",
                      rec is not None and rec.get("state") == "completed" and
                      rec.get("result") == "sleep_confirmed" and
                      rec.get("error_code") is None,
                      f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                      f"error_code={rec and rec.get('error_code')}")
        check_deadline_posthoc(checker, base, args.read_key, command_id, rec,
                               SLEEP_DEADLINE_S, "sleep")

    # The human already woke the Mac to run this phase; the session should be
    # back. This wait is generous because login timing is human-paced.
    elapsed, doc = wait_session(base, args.read_key, True, 120)
    checker.check("agent session ACTIVE again within 120 s (human is logged in)",
                  elapsed is not None, "no session after 120 s")
    st = doc or {}
    checker.check("system.state awake",
                  (st.get("system") or {}).get("state") == "awake",
                  f"system.state={(st.get('system') or {}).get('state')}")

    ok = record_phase_result(state, "sleep-verify", checker, fb,
                             {"post_session_id": st.get("session_id"),
                              "post_boot_id": st.get("boot_id")})
    save_state(args.state_file, state)
    if ok:
        print_next_command(args, "wake",
                           ("The Mac is awake; the final phase HID-taps it awake "
                            "via the endpoint and closes the verification loop."))
    return 0 if ok else 1


def phase_wake_colocated(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase wake: endpoint HID wake, evidence + gate summary")
    state = load_state(args.state_file)
    restart = state.get("restart") or {}
    expected_boot = restart.get("post_boot_id")
    st = agent_status(base, args.read_key) or {}
    checker.check("precondition: session live, boot_id matches post-restart value",
                  st.get("session_active") is True and bool(st.get("boot_id")) and
                  (not expected_boot or st.get("boot_id") == expected_boot),
                  f"session_active={st.get('session_active')} "
                  f"boot_id={st.get('boot_id')!r} expected={expected_boot!r}")
    incumbent_sid = st.get("session_id")
    pre_boot = st.get("boot_id")
    print(f"  ... pre-dispatch session_id={incumbent_sid!r} boot_id={pre_boot!r}")

    command_id = dispatch_system(checker, base, args.control_key, "wake")
    if not command_id:
        return 1
    t_disp = time.monotonic()
    print(f"  ... wake dispatched at t=0 (command_id={command_id})")

    deadline = t_disp + WAKE_DEADLINE_S
    rec = poll_command_terminal(base, args.read_key, command_id,
                                max(1.0, deadline - time.monotonic()))
    t_term = time.monotonic() - t_disp
    checker.check("wake record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"wake completed/wake_confirmed within {WAKE_DEADLINE_S:.0f} s",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "wake_confirmed" and
                  rec.get("error_code") is None and t_term <= WAKE_DEADLINE_S,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')} terminal_at={t_term:.1f} s")

    # Post-terminal evidence: the engine only completes wake on a post-dispatch
    # new-session hello + awake burst (spec 8.1.2), so a changed session_id
    # with an UNCHANGED boot_id after terminal is the harness-side proof.
    st2 = agent_status(base, args.read_key) or {}
    post_sid = st2.get("session_id")
    post_boot = st2.get("boot_id")
    checker.check("session_id CHANGED across the wake dispatch (new-session hello)",
                  bool(post_sid) and bool(incumbent_sid) and post_sid != incumbent_sid,
                  f"pre={incumbent_sid!r} post={post_sid!r}")
    checker.check("boot_id UNCHANGED across wake (wake, not reboot)",
                  bool(post_boot) and post_boot == pre_boot,
                  f"pre={pre_boot!r} post={post_boot!r}")

    ok = record_phase_result(state, "wake", checker, fb,
                             {"command_id": command_id,
                              "terminal_at_s": round(t_term, 1)})
    save_state(args.state_file, state)
    print_gate_summary(state, args)
    return 0 if ok else 1


def print_gate_summary(state, args):
    print("\n=== AT-08 CO-LOCATED GATE SUMMARY ===")
    print(f"  state file: {args.state_file}")
    for phase in PHASE_ORDER:
        entry = state.get(phase) or {}
        status = entry.get("status", "not run")
        mark = PASS if status == "passed" else FAIL
        print(f"  [{mark}] {phase}: {status}")
        for f in entry.get("failures") or []:
            print(f"        - {f}")
    chain_ok = all((state.get(p) or {}).get("status") == "passed" for p in PHASE_ORDER)
    print()
    if chain_ok:
        print("  THIS CHAIN PASSED. The spec-16 gate needs TWO consecutive chains:")
        print(f"    re-run the whole phase chain with a fresh --state-file, e.g.")
        print(f"      {build_invocation(args, 'restart-dispatch').replace(args.state_file, args.state_file + '.2')}")
    else:
        print("  THIS CHAIN IS INCOMPLETE/FAILED — see failures above. Re-run the")
        print("  failing phase (or the whole chain from restart-dispatch).")


def phase_resume(args):
    state = load_state(args.state_file)
    for phase in PHASE_ORDER:
        if (state.get(phase) or {}).get("status") != "passed":
            print(f"Next phase for {args.state_file}: {phase}")
            print(f"  {build_invocation(args, phase)}")
            return 0
    print(f"All phases in {args.state_file} passed. For the second consecutive")
    print("chain, re-run from restart-dispatch with a fresh --state-file:")
    print(f"  {build_invocation(args, 'restart-dispatch').replace(args.state_file, args.state_file + '.2')}")
    return 0


def run_phased(args, base):
    checker = Checker()
    phase = args.phase
    if phase == "restart-dispatch":
        return phase_restart_dispatch(checker, args, base)
    if phase == "restart-verify":
        return phase_restart_verify(checker, args, base)
    if phase == "sleep-dispatch":
        return phase_sleep_dispatch(checker, args, base)
    if phase == "sleep-verify":
        return phase_sleep_verify(checker, args, base)
    if phase == "wake":
        return phase_wake_colocated(checker, args, base)
    print(f"unknown phase {phase!r}", file=sys.stderr)
    return 2


def main():
    ap = argparse.ArgumentParser(
        description="MacControl Phase 5 acceptance AT-08: verified restart and wake")
    ap.add_argument("--hostname", default="control-graphics.local",
                    help="bench endpoint mDNS name (default: control-graphics.local)")
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--base", default=None,
                    help="default: http://<hostname> (.local appended if absent)")
    ap.add_argument("--rounds", type=int, default=2,
                    help="consecutive rounds required to pass (default: 2; "
                         "in-process mode only)")
    ap.add_argument("--state-file", default="/tmp/at08_state.json",
                    help="co-located mode: chain state file "
                         "(default: /tmp/at08_state.json)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--phase", choices=PHASE_ORDER,
                      help="co-located mode: run one phase of the chain")
    mode.add_argument("--resume", action="store_true",
                      help="co-located mode: print the next phase the state "
                           "file says to run, then exit")
    args = ap.parse_args()

    host = args.hostname
    if not host.endswith(".local"):
        host += ".local"
    base = args.base or f"http://{host}"

    if args.resume:
        return phase_resume(args)
    if args.phase:
        return run_phased(args, base)

    checker = Checker()
    checker.section("AT-08 preconditions")
    elapsed, doc = wait_connected(base, args.read_key, 90)
    checker.check("agent connected at start (within 90 s)", elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    if elapsed is None:
        print("agent never connected; cannot continue", file=sys.stderr)
        return 1

    for rnd in range(1, args.rounds + 1):
        run_round(checker, base, args.read_key, args.control_key, rnd)

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print(f"AT-08: all checks passed in all {args.rounds} round(s).")
    print("Target Mac is awake with a live agent session.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
