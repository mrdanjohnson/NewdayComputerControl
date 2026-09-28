#!/usr/bin/env python3
"""MacControl Phase 5 acceptance test AT-09 (spec section 16.2): expected-
offline provenance. Two modes.

AT-09 (spec): submit `sleep`; while the declared expected-offline window is
open, GET /api/v1/status renders the agent-derived tuples
freshness=expected_offline — positive evidence, not the ambiguous `stale` —
even past the 15 s stale mark; connection.agent renders value=false with
expected_offline freshness; mac.boot_id retains its value with
expected_offline freshness. After the window closes (60 s) with no
reconnect, the TTL-bearing fields degrade to stale. Traces: Ch. 7.2.2, 8.1.

The in-window/post-window samples are the worked-example-3 flow (spec 7.2.2):
sleep dispatches at t=0, the declared goodbye lands ~t=4 s, the window opens
at the GOODBYE (onset), not at dispatch. The window stays open until
onset+60 s (~t=64 s past dispatch), so the in-window sample sits at ~t=20 s:
past the 15 s stale mark, still inside the window; the post-window sample at
~t=70 s.

One spec subtlety pinned against the flashed firmware (mc_status.cpp): the
window's declared_offline_until is onset+60, and mac.boot_id's TTL is null
(spec 7.2.2 table: stale trigger "never; retained through the window") — so
after the window closes boot_id renders value retained + freshness FRESH,
not stale. This script asserts exactly that; the TTL-bearing tuples
(mac.state/locked/user_logged_in, applications.*) do degrade to stale.

MODE 1 — in-process (default; future two-Mac topology): the harness runs on
a DIFFERENT Mac than the HID target, so it survives the target's sleep and
samples the status document itself. N consecutive rounds in one invocation
(--rounds, default 2).

MODE 2 — co-located human-observed (--phase; THIS bench today): the HID
cable is on the SAME Mac as the harness, so the harness freezes the moment
the Mac sleeps and CANNOT take the in-window sample. A human observes from a
SECOND device on the same Wi-Fi instead. Two invocations per run:

  python3 at09.py ... --phase dispatch
      Precondition check, then prints the exact observation instructions
      (sample times as wall clock, the exact curl with the READ key, and the
      Web UI alternative), dispatches sleep, saves state, exits. The Mac
      sleeps ~2 s later; the harness dies with it.
  python3 at09.py ... --phase verify
      Run after the human woke the Mac and logged back in. Asserts the sleep
      record terminal completed/sleep_confirmed (deadline bound post-hoc via
      the command log), session back + awake (WARN only), then asks the human
      to confirm their two observations interactively (skipped with the
      questions printed as outstanding items when stdin is not a TTY).

The freshness observations are recorded as HUMAN-OBSERVED evidence lines in
the gate summary — the device-side ledger/deadline checks plus the human
samples together satisfy the AT. The spec-16 two-consecutive-runs gate is
satisfied by running the whole dispatch/verify chain twice (separate
--state-file per run, e.g. /tmp/at09_state1.json, /tmp/at09_state2.json) —
the verify phase prints a per-run gate summary and reminds you.

Cleanup wake: per spec 16 the wake RECORD is AT-08's job — this script only
waits for session_active + awake again (WARN, not a check). The co-located
verify phase does NOT dispatch wake itself: the human already woke the Mac
to run the phase.

Never open a serial port in these scripts — it reboots the bench S3.

Usage (mode 1):
  python3 at09.py --hostname control-graphics.local \
      --read-key mck_... --control-key mck_...

Usage (mode 2): see the two-phase chain above.

Exit code 0 = all checks pass (all rounds / the invoked phase).
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

WINDOW_CLOSE_S = 60.0     # spec 8 close_after_s
STALE_MARK_S = 15.0       # spec 4 stale_threshold
SAMPLE1_AT_S = 20.0       # past the stale mark, inside the window
SAMPLE2_AT_S = 70.0       # window closed
SLEEP_DEADLINE_S = 90.0
WAKE_WAIT_S = 120.0

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


def status_doc(base, read_key):
    s, doc = poll_get(base, "/api/v1/status", read_key)
    return doc if s == 200 else None


def wait_session(base, read_key, active, timeout_s):
    t0 = time.monotonic()
    doc = None
    while time.monotonic() - t0 < timeout_s:
        doc = agent_status(base, read_key)
        if doc is not None and doc.get("session_active") is active:
            return time.monotonic() - t0, doc
        time.sleep(POLL_STATUS_S)
    return None, doc


def dispatch_system(checker, base, control_key, name):
    """POST /api/v1/system/{name}; returns command_id or None."""
    s, doc = http("POST", base, f"/api/v1/system/{name}", key=control_key)
    checker.check(f"POST /api/v1/system/{name} -> 202",
                  s == 202 and bool(doc.get("command_id")), f"status={s}")
    return doc.get("command_id") if s == 202 else None


def poll_command_terminal(base, key, command_id, timeout_s):
    deadline = time.monotonic() + timeout_s
    rec = None
    while time.monotonic() < deadline:
        s, rec = poll_get(base, f"/api/v1/commands/{command_id}", key)
        if s == 200 and isinstance(rec, dict) and rec.get("state") in TERMINAL_STATES:
            return rec
        time.sleep(2.5)   # READ bucket: <= 1 request / 2-3 s
    return rec if isinstance(rec, dict) else None


def tuples_for_freshness(doc):
    """Collect the agent-derived tuples AT-09 pins: mac.state/locked/
    user_logged_in, every applications.<bundle>.state (and pid when the
    value is non-null), plus mac.boot_id and connection.agent separately."""
    mac = doc.get("mac") or {}
    out = {}
    for k in ("state", "locked", "user_logged_in"):
        out[f"mac.{k}"] = mac.get(k)
    apps = doc.get("applications") or {}
    for bundle, entry in apps.items():
        if isinstance(entry, dict):
            out[f"applications.{bundle}.state"] = entry.get("state")
            pid = entry.get("pid")
            if isinstance(pid, dict) and pid.get("value") is not None:
                out[f"applications.{bundle}.pid"] = pid
    return out


def check_freshness_group(checker, label, doc, expected):
    """Assert every agent-derived tuple AT-09 pins (mac.state/locked/
    user_logged_in, every applications.<bundle>.state, and pid when its
    value is non-null) reads `expected` freshness."""
    tuples = tuples_for_freshness(doc)
    bad = {k: (t or {}).get("freshness") for k, t in tuples.items()
           if (t or {}).get("freshness") != expected}
    checker.check(f"{label}: mac.* and applications.* freshness == {expected}",
                  not bad, f"mismatched: {bad}")


def run_round(checker, base, read_key, control_key, rnd):
    print(f"\n########## ROUND {rnd} ##########")
    checker.section(f"Round {rnd}: preconditions")
    _, doc = wait_session(base, read_key, True, 15)
    st = doc or {}
    baseline_boot = st.get("boot_id")
    checker.check("precondition: live session, system awake, known boot_id",
                  st.get("session_active") is True and
                  (st.get("system") or {}).get("state") == "awake" and
                  bool(baseline_boot),
                  f"session_active={st.get('session_active')} "
                  f"system.state={(st.get('system') or {}).get('state')} "
                  f"boot_id={baseline_boot!r}")
    if not baseline_boot:
        return

    checker.section(f"Round {rnd}: dispatch sleep, wait for the declared goodbye")
    command_id = dispatch_system(checker, base, control_key, "sleep")
    if not command_id:
        return
    t_disp = time.monotonic()
    print(f"  ... sleep dispatched at t=0 (command_id={command_id})")
    # The window opens at the GOODBYE, not at dispatch (spec 7.2.2), so the
    # sampling clock needs the goodbye: observable as agent/status going
    # offline. Worked example 3 has it at ~t=4 s; if it is not observable by
    # ~t=15 s the Mac did not sleep and the scenario cannot run.
    t_offline = None
    while time.monotonic() - t_disp < 20.0:
        st = agent_status(base, read_key)
        if st is not None and st.get("session_active") is False:
            t_offline = time.monotonic() - t_disp
            break
        time.sleep(POLL_STATUS_S)
    checker.check("declared goodbye observable (session offline by ~20 s)",
                  t_offline is not None,
                  "agent/status stayed online; the target did not sleep")
    if t_offline is None:
        return
    print(f"  ... offline (goodbye) at t={t_offline:.1f} s; "
          f"window open until ~t={t_offline + WINDOW_CLOSE_S:.1f} s")

    # Sample 1: past the 15 s stale mark, inside the window. Anchored to the
    # goodbye too, so a slow goodbye can't push the sample past the close.
    t_sample1 = max(t_disp + SAMPLE1_AT_S, t_disp + t_offline + STALE_MARK_S + 1.0)
    delay = t_sample1 - time.monotonic()
    if delay > 0:
        print(f"  ... sampling in-window status at t={t_sample1 - t_disp:.1f} s "
              f"(stale mark {STALE_MARK_S:.0f} s, window close "
              f"{t_offline + WINDOW_CLOSE_S:.1f} s)")
        time.sleep(delay)
    doc = status_doc(base, read_key)
    checker.check("GET /api/v1/status during window -> 200", doc is not None,
                  "status unreadable")
    if doc is None:
        return
    at = time.monotonic() - t_disp
    check_freshness_group(checker, f"in-window sample (t={at:.1f} s)", doc,
                          "expected_offline")
    conn_agent = (doc.get("connection") or {}).get("agent") or {}
    checker.check("connection.agent value == false during window",
                  conn_agent.get("value") is False, json.dumps(conn_agent))
    checker.check("connection.agent freshness == expected_offline during window",
                  conn_agent.get("freshness") == "expected_offline",
                  json.dumps(conn_agent))
    boot = (doc.get("mac") or {}).get("boot_id") or {}
    checker.check("mac.boot_id value RETAINED during window",
                  boot.get("value") == baseline_boot, json.dumps(boot))
    checker.check("mac.boot_id freshness == expected_offline during window",
                  boot.get("freshness") == "expected_offline", json.dumps(boot))

    # Sample 2: after the window closes with no reconnect. The TTL-bearing
    # tuples degrade to stale; boot_id has no TTL (spec 7.2.2: stale trigger
    # "never") and renders value retained + fresh; connection.agent is
    # event-driven (spec: may read false/fresh) — logged, not asserted.
    t_sample2 = max(t_disp + SAMPLE2_AT_S, t_disp + t_offline + WINDOW_CLOSE_S + 5.0)
    delay = t_sample2 - time.monotonic()
    if delay > 0:
        print(f"  ... sampling post-window status at t={t_sample2 - t_disp:.1f} s "
              f"(window closed at ~{t_offline + WINDOW_CLOSE_S:.1f} s)")
        time.sleep(delay)
    doc2 = status_doc(base, read_key)
    checker.check("GET /api/v1/status after window close -> 200", doc2 is not None,
                  "status unreadable")
    if doc2 is not None:
        at2 = time.monotonic() - t_disp
        check_freshness_group(checker, f"post-window sample (t={at2:.1f} s)", doc2,
                              "stale")
        boot2 = (doc2.get("mac") or {}).get("boot_id") or {}
        checker.check("mac.boot_id value retained after window close",
                      boot2.get("value") == baseline_boot, json.dumps(boot2))
        checker.check("mac.boot_id freshness == fresh after window close "
                      "(no TTL; spec 7.2.2 'never' stale)",
                      boot2.get("freshness") == "fresh", json.dumps(boot2))
        conn2 = (doc2.get("connection") or {}).get("agent") or {}
        print(f"  ... post-window connection.agent: value={conn2.get('value')} "
              f"freshness={conn2.get('freshness')} (event-driven, not asserted)")

    # Corroboration: the sleep record itself completed at window close.
    rec = poll_command_terminal(base, read_key, command_id,
                                max(1.0, t_disp + SLEEP_DEADLINE_S - time.monotonic()))
    checker.check("sleep record completed/sleep_confirmed",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "sleep_confirmed" and
                  rec.get("error_code") is None,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')}")

    # Cleanup: wake the target back up. The wake RECORD belongs to AT-08 —
    # here we only wait for the session to return (informational).
    checker.section(f"Round {rnd}: cleanup wake (record not asserted here — AT-08)")
    wake_id = dispatch_system(checker, base, control_key, "wake")
    if wake_id:
        elapsed, st = wait_session(base, read_key, True, WAKE_WAIT_S)
        awake = (st or {}).get("system") or {}
        if elapsed is not None and awake.get("state") == "awake":
            print(f"  ... session active + awake again at t=+{elapsed:.1f} s")
        else:
            print(f"  [WARN] target did not come back within {WAKE_WAIT_S:.0f} s "
                  f"(session_active={(st or {}).get('session_active')} "
                  f"system.state={awake.get('state')}); next round's preconditions "
                  "will fail if it stays down")


# ---------------------------------------------------------------------------
# Mode 2: co-located human-observed mode.
#
# The HID cable is on the SAME Mac as the harness, so the sleep freezes the
# harness and it cannot take the in-window samples. A human on a second
# device (phone/tablet, same Wi-Fi) reads GET /api/v1/status at the printed
# wall times and reports the values in the verify phase.
# ---------------------------------------------------------------------------

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


def curl_status_command(args, base):
    hostport = base.split("://", 1)[1]
    return (f"curl -s -H 'Authorization: Bearer {args.read_key}' "
            f"http://{hostport}/api/v1/status")


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
            # evidence notes (e.g. an unexpected_wake logged when the host
            # reconnects after sleep_confirmed) also carry the command_id
            # and would otherwise inflate the bound past the deadline.
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


def ask_human(question):
    """y/n prompt; None when stdin is not a TTY or on EOF."""
    if not sys.stdin.isatty():
        return None
    try:
        return input(question + " [y/n] ").strip().lower()
    except EOFError:
        return None


def phase_dispatch(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase dispatch: print human instructions, dispatch sleep")
    state = load_state(args.state_file)
    _, doc = wait_session(base, args.read_key, True, 15)
    st = doc or {}
    baseline_boot = st.get("boot_id")
    checker.check("precondition: live session, system awake, known boot_id",
                  st.get("session_active") is True and
                  (st.get("system") or {}).get("state") == "awake" and
                  bool(baseline_boot),
                  f"session_active={st.get('session_active')} "
                  f"system.state={(st.get('system') or {}).get('state')} "
                  f"boot_id={baseline_boot!r}")
    if not baseline_boot:
        return 1

    now = time.time()
    sample1_at = now + SAMPLE1_AT_S            # ~20 s: past stale mark, in window
    sample2_at = now + SAMPLE2_AT_S            # ~70 s: window closed
    hostport = base.split("://", 1)[1]
    curl = curl_status_command(args, base)

    print("\n" + "=" * 72)
    print("  HUMAN OBSERVATION PROTOCOL — read BEFORE the Mac sleeps:
  0. Bench setup (once): `sudo pmset -a powernap 0` and
     `sudo pmset -a tcpkeepalive 0` — otherwise Power Nap /
     wake-for-network dark wakes keep the agent's heartbeats flowing
     during sleep and the freshness transitions never render (verified
     2026-09-27).")
    print(f"  1. This Mac will SLEEP in ~2 s. The harness dies with it.")
    print(f"  2. FROM ANOTHER DEVICE on the same Wi-Fi (phone/tablet), at")
    print(f"     {fmt_wall(sample1_at)} (+20 s) run this exact command (or open")
    print(f"     the Web UI status view at http://{hostport}/ ):")
    print(f"       {curl}")
    print(f"     Record the freshness values. EXPECT: mac.state, mac.locked,")
    print(f"     mac.user_logged_in and every applications.* entry =")
    print(f"     expected_offline; connection.agent value=false +")
    print(f"     expected_offline; mac.boot_id value retained + expected_offline.")
    print(f"  3. At {fmt_wall(sample2_at)} (+70 s) run it AGAIN. EXPECT: those")
    print(f"     tuples now stale; mac.boot_id value retained + fresh;")
    print(f"     connection.agent may read false/fresh (event-driven).")
    print(f"  4. Then press any key to wake this Mac and LOG IN.")
    print(f"  5. Run the verify phase:")
    print(f"     {build_invocation(args, 'verify')}")
    print("=" * 72)

    command_id = dispatch_system(checker, base, args.control_key, "sleep")
    if not command_id:
        return 1
    state["sleep"] = {
        "command_id": command_id,
        "dispatched_wall": time.time(),
        "sample1_wall": sample1_at,
        "sample2_wall": sample2_at,
        "baseline_boot_id": baseline_boot,
        "curl": curl,
        "web_ui": f"http://{hostport}/",
    }
    ok = record_phase_result(state, "dispatch", checker, fb,
                             {"command_id": command_id})
    save_state(args.state_file, state)
    print(f"\n  sleep dispatched (command_id={command_id}); state saved to "
          f"{args.state_file}. This Mac sleeps shortly.")
    return 0 if ok else 1


def phase_verify(checker, args, base):
    fb = len(checker.failures)
    checker.section("Phase verify: device-side checks + human observations")
    state = load_state(args.state_file)
    sleep_st = state.get("sleep") or {}
    command_id = sleep_st.get("command_id")
    if not command_id:
        print("state file has no dispatch; run --phase dispatch first",
              file=sys.stderr)
        return 1

    rec = get_record(base, args.read_key, command_id)
    checker.check("sleep record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    if rec is not None and rec.get("state") == "failed" and \
            rec.get("result") == "unexpected_wake":
        checker.check("sleep record completed/sleep_confirmed/error_code null", False,
                      "the host reconnected before window close — the Mac was woken "
                      "too early. Rerun from --phase dispatch")
    else:
        checker.check("sleep record completed/sleep_confirmed/error_code null",
                      rec is not None and rec.get("state") == "completed" and
                      rec.get("result") == "sleep_confirmed" and
                      rec.get("error_code") is None,
                      f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                      f"error_code={rec and rec.get('error_code')}")
        check_deadline_posthoc(checker, base, args.read_key, command_id, rec,
                               SLEEP_DEADLINE_S, "sleep")

    # Session back + awake: the human already woke the Mac to run this
    # phase, so this is a WARN-only liveness note (not a gate check).
    elapsed, st = wait_session(base, args.read_key, True, WAKE_WAIT_S)
    awake = ((st or {}).get("system") or {}).get("state")
    if elapsed is not None and awake == "awake":
        print(f"  ... session active + awake again (t=+{elapsed:.1f} s)")
    else:
        print(f"  [WARN] session/awake not restored within {WAKE_WAIT_S:.0f} s "
              f"(session_active={(st or {}).get('session_active')} "
              f"system.state={awake}); device-side record checks above still stand")

    # Human-observed freshness samples. Recorded as evidence lines in the
    # gate summary; non-TTY stdin prints them as outstanding items instead.
    human = {}
    print()
    ans = ask_human("In-window sample (~+20 s): mac.state/locked/user_logged_in/"
                    "applications freshness == expected_offline?")
    human["in_window_expected_offline"] = ans
    ans2 = ask_human("Post-window sample (~+70 s): those tuples stale, mac.boot_id "
                     "retained + fresh?")
    human["post_window_stale"] = ans2

    ok = record_phase_result(state, "verify", checker, fb,
                             {"human_observations": human,
                              "deadline_checked": True})
    save_state(args.state_file, state)
    print_gate_summary(state, args, human)
    return 0 if ok else 1


def print_gate_summary(state, args, human):
    print("\n=== AT-09 CO-LOCATED GATE SUMMARY ===")
    print(f"  state file: {args.state_file}")
    for phase in ("dispatch", "verify"):
        entry = state.get(phase) or {}
        status = entry.get("status", "not run")
        mark = PASS if status == "passed" else FAIL
        print(f"  [{mark}] {phase} (device-side checks): {status}")
        for f in entry.get("failures") or []:
            print(f"        - {f}")
    print("  HUMAN-OBSERVED evidence (recorded, not device-verifiable):")
    labels = {"in_window_expected_offline":
              f"in-window sample (~+{SAMPLE1_AT_S:.0f} s) showed expected_offline",
              "post_window_stale":
              f"post-window sample (~+{SAMPLE2_AT_S:.0f} s) showed stale + boot_id retained/fresh"}
    for key, label in labels.items():
        ans = (human or {}).get(key)
        if ans is None:
            print(f"  [??] {label}: NO ANSWER RECORDED (stdin not a TTY) — "
                  "outstanding item; record it in the run notes")
        else:
            mark = PASS if ans.startswith("y") else FAIL
            print(f"  [{mark}] {label}: human answered {ans!r}")
    print()
    chain_ok = all((state.get(p) or {}).get("status") == "passed"
                   for p in ("dispatch", "verify"))
    if chain_ok:
        print("  THIS RUN PASSED (device side). The spec-16 gate needs TWO")
        print("  consecutive runs: re-run the chain with a fresh --state-file:")
        print(f"    {build_invocation(args, 'dispatch').replace(args.state_file, args.state_file + '.2')}")
    else:
        print("  THIS RUN FAILED device-side checks — see failures above.")


def run_phased(args, base):
    checker = Checker()
    if args.phase == "dispatch":
        return phase_dispatch(checker, args, base)
    if args.phase == "verify":
        return phase_verify(checker, args, base)
    print(f"unknown phase {args.phase!r}", file=sys.stderr)
    return 2


def main():
    ap = argparse.ArgumentParser(
        description="MacControl Phase 5 acceptance AT-09: expected-offline provenance")
    ap.add_argument("--hostname", default="control-graphics.local",
                    help="bench endpoint mDNS name (default: control-graphics.local)")
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--base", default=None,
                    help="default: http://<hostname> (.local appended if absent)")
    ap.add_argument("--rounds", type=int, default=2,
                    help="consecutive rounds required to pass (default: 2; "
                         "in-process mode only)")
    ap.add_argument("--state-file", default="/tmp/at09_state.json",
                    help="co-located mode: run state file "
                         "(default: /tmp/at09_state.json)")
    ap.add_argument("--phase", choices=("dispatch", "verify"),
                    help="co-located mode: run one phase of the two-phase chain")
    args = ap.parse_args()

    host = args.hostname
    if not host.endswith(".local"):
        host += ".local"
    base = args.base or f"http://{host}"

    if args.phase:
        return run_phased(args, base)

    checker = Checker()
    checker.section("AT-09 preconditions")
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
    print(f"AT-09: all checks passed in all {args.rounds} round(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
