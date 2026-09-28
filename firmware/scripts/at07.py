#!/usr/bin/env python3
"""MacControl Phase 5 acceptance test AT-07 (spec section 16.2): verified lock.

AT-07 (spec): submit `lock`; the MCA emits screen_lock_changed(locked) within
the 15 s deadline; the ESP32 records completed/lock_confirmed. Hardware path:
the S3 HID dispatches Ctrl+Cmd+Q to the TARGET Mac (the graphics computer);
the MCA running there observes the real transition. Traces: Ch. 5.2, 5.3.1,
6.2, 2.1.

The whole flow runs N consecutive rounds inside one invocation (spec 16 rule:
--rounds, default 2); every check must pass in every round. Rounds use fresh
command ids and are otherwise independent.

Preconditions: AT-06 has run on this bench (paired, MCA connected, Mode B).
The harness runs on Mac A; the target Mac is observed only through the
endpoint REST surface — never open a serial port here (it reboots the bench
S3 and destroys the evidence).

Round shape:
  A. Precondition: agent connected; capabilities lock available+verified;
     /api/v1/agent/status user.screen_locked == false (the lock predicate
     completes on a TRANSITION to locked; if the screen is already locked
     there is no transition and the record would time out). Fail fast with
     a clear message if locked or undetectable.
  B. POST /api/v1/system/lock (CONTROL key) -> 202; poll the record (READ
     key) to terminal; assert completed/lock_confirmed within 15 s of
     dispatch.
  C. Corroboration: the agent observes screen_locked == true within 15 s.

Between rounds: the target screen is now LOCKED and there is no API unlock
(Ctrl+Cmd+Q only locks). The script waits — up to --unlock-timeout s — for a
human to unlock the target (type the password), then proceeds to the next
round. At the end it prints a prominent reminder that the screen is LOCKED.

Usage:
  python3 at07.py --hostname control-graphics.local \
      --read-key mck_... --control-key mck_...

Exit code 0 = all checks pass (all rounds).
"""

import argparse
import json
import sys
import time

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

TERMINAL_STATES = ("completed", "failed", "timed_out", "unconfirmed")

LOCK_DEADLINE_S = 15.0
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


def wait_screen_locked(base, read_key, locked, timeout_s):
    """Poll agent/status user.screen_locked until it equals `locked`.
    Returns (elapsed, last_doc) or (None, last_doc)."""
    t0 = time.monotonic()
    doc = None
    while time.monotonic() - t0 < timeout_s:
        doc = agent_status(base, read_key)
        if doc is not None and (doc.get("user") or {}).get("screen_locked") is locked:
            return time.monotonic() - t0, doc
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


def run_round(checker, base, read_key, control_key, rnd):
    print(f"\n########## ROUND {rnd} ##########")
    checker.section(f"Round {rnd} A: preconditions (connected, verified, screen unlocked)")
    elapsed, capa = wait_connected(base, read_key, 60)
    checker.check("agent session ACTIVE before lock dispatch",
                  elapsed is not None, "agent.connected stayed false for 60 s")
    if elapsed is None:
        return
    lock_entry = ((capa.get("commands") or {}).get("lock") or {})
    checker.check("capabilities lock available (Mode B, L3)",
                  lock_entry.get("available") is True, json.dumps(lock_entry))
    checker.check("capabilities lock verified == true while connected",
                  lock_entry.get("verified") is True, json.dumps(lock_entry))

    st = agent_status(base, read_key)
    locked = (st.get("user") or {}).get("screen_locked") if st else None
    checker.check("precondition: target screen UNLOCKED (screen_locked == false)",
                  locked is False,
                  "screen_locked=" + repr(locked) + (" — the lock predicate completes on a "
                  "TRANSITION; unlock the target Mac (there is no API unlock) and re-run"
                  if locked else " agent/status unreadable"))
    if locked is not False:
        return

    checker.section(f"Round {rnd} B: dispatch lock, expect completed/lock_confirmed <= 15 s")
    command_id = dispatch_system(checker, base, control_key, "lock")
    if not command_id:
        return
    t_disp = time.monotonic()
    print(f"  ... lock dispatched at t=0 (command_id={command_id})")
    # Poll with the READ key: CONTROL's 30/min bucket cannot sustain polling.
    rec = poll_command_terminal(base, read_key, command_id, LOCK_DEADLINE_S + 10)
    t_term = time.monotonic() - t_disp
    checker.check("lock record reached terminal state", rec is not None and
                  rec.get("state") in TERMINAL_STATES, f"record={rec}")
    checker.check(f"lock completed/lock_confirmed within {LOCK_DEADLINE_S:.0f} s",
                  rec is not None and rec.get("state") == "completed" and
                  rec.get("result") == "lock_confirmed" and
                  rec.get("error_code") is None and t_term <= LOCK_DEADLINE_S,
                  f"state={rec and rec.get('state')} result={rec and rec.get('result')} "
                  f"error_code={rec and rec.get('error_code')} terminal_at={t_term:.1f} s")
    print(f"  ... lock record terminal at t={t_term:.1f} s "
          f"(deadline {LOCK_DEADLINE_S:.0f} s)")

    checker.section(f"Round {rnd} C: agent observes the real transition")
    # Same evidence the endpoint's predicate consumes, seen from our side:
    # the agent on the target Mac reports screen_locked == true.
    elapsed, _ = wait_screen_locked(base, read_key, True, LOCK_DEADLINE_S + 5)
    checker.check("agent observed screen_locked == true within 15 s",
                  elapsed is not None and elapsed <= LOCK_DEADLINE_S,
                  f"observed_at={'never' if elapsed is None else f'{elapsed:.1f} s'}")


def wait_human_unlock(checker, base, read_key, timeout_s):
    """The target screen is LOCKED and there is no API unlock. Wait for a
    human to unlock it before the next round can start."""
    print("\n" + "=" * 72)
    print("  TARGET SCREEN IS NOW LOCKED (Ctrl+Cmd+Q was typed into the target Mac).")
    print("  There is no API unlock. A HUMAN MUST UNLOCK THE TARGET MAC")
    print(f"  (type the password) within {timeout_s:.0f} s to continue...")
    print("=" * 72)
    elapsed, _ = wait_screen_locked(base, read_key, False, timeout_s)
    checker.check(f"target unlocked by human within {timeout_s:.0f} s "
                  "(next-round precondition)", elapsed is not None,
                  "screen stayed locked; remaining rounds cannot run")


def main():
    ap = argparse.ArgumentParser(
        description="MacControl Phase 5 acceptance AT-07: verified lock")
    ap.add_argument("--hostname", default="control-graphics.local",
                    help="bench endpoint mDNS name (default: control-graphics.local)")
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--base", default=None,
                    help="default: http://<hostname> (.local appended if absent)")
    ap.add_argument("--rounds", type=int, default=2,
                    help="consecutive rounds required to pass (default: 2)")
    ap.add_argument("--unlock-timeout", type=float, default=180.0,
                    help="seconds to wait for a human unlock between rounds "
                         "(default: 180)")
    args = ap.parse_args()

    host = args.hostname
    if not host.endswith(".local"):
        host += ".local"
    base = args.base or f"http://{host}"

    checker = Checker()
    checker.section("AT-07 preconditions")
    elapsed, doc = wait_connected(base, args.read_key, 60)
    checker.check("agent connected at start (within 60 s)", elapsed is not None,
                  f"agent={doc.get('agent') if isinstance(doc, dict) else doc}")
    if elapsed is None:
        print("agent never connected; cannot continue", file=sys.stderr)
        return 1

    for rnd in range(1, args.rounds + 1):
        run_round(checker, base, args.read_key, args.control_key, rnd)
        if rnd < args.rounds:
            wait_human_unlock(checker, base, args.read_key, args.unlock_timeout)

    print("\n=== SUMMARY ===")
    print("\n" + "=" * 72)
    print("  REMINDER: the target Mac's screen is now LOCKED.")
    print("  Ctrl+Cmd+Q was typed into the target Mac; there is NO API unlock.")
    print("  A human must unlock the target Mac before any further ATs.")
    print("=" * 72)
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print(f"AT-07: all checks passed in all {args.rounds} round(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
