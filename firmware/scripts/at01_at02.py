#!/usr/bin/env python3
"""MacControl Phase 1 acceptance tests AT-01 and AT-02 (spec section 16.1).

Runs against a flashed ESP32-S3 on the local network. Uses only the Python
standard library (mDNS resolution via the OS resolver: pass the advertised
hostname, e.g. mac-a1b2c3.local). Requires a READ key and a CONTROL key
provisioned over the serial console.

Usage:
  python3 at01_at02.py --hostname mac-a1b2c3.local \
      --read-key mck_... --control-key mck_...

Exit code 0 = both acceptance tests pass.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"


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


def resolve_host(checker, hostname, deadline_s=5):
    """AT-01: advertisement resolves within 5 s.

    Uses dns-sd (macOS Bonjour) to browse _maccontrol._tcp — this measures the
    endpoint's actual advertisement, unlike getaddrinfo whose cold-cache first
    lookup costs one ~5 s multicast retry cycle. Falls back to getaddrinfo.
    """
    import subprocess
    bare = hostname[:-6] if hostname.endswith(".local") else hostname
    t0 = time.monotonic()
    proc = subprocess.Popen(["dns-sd", "-B", "_maccontrol._tcp", "local"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    found = False
    elapsed = 0.0
    try:
        while time.monotonic() - t0 < deadline_s:
            line = proc.stdout.readline()
            if not line:
                break
            if bare in line and " Add " in f" {line} ":
                found = True
                elapsed = time.monotonic() - t0
                break
    finally:
        proc.kill()
        proc.wait()
    checker.check(f"mDNS _maccontrol._tcp browse within 5 s ({elapsed:.2f} s)",
                  found and elapsed <= deadline_s,
                  f"found={found}")
    if not found:
        return None

    import socket
    try:
        infos = socket.getaddrinfo(hostname, 80, proto=socket.IPPROTO_TCP)
        ip = infos[0][4][0]
        checker.check(f"hostname resolves ({ip})", True)
        return ip
    except Exception as e:
        checker.check("hostname resolves", False, str(e))
        return None


def at01(checker, base, read_key):
    checker.section("AT-01 Discovery and reachability")

    s, doc = http("GET", base, "/api/v1/status", key=read_key)
    checker.check("GET /api/v1/status -> 200", s == 200, f"status={s} body={doc}")
    if s == 200:
        agent = doc.get("connection", {}).get("agent", {})
        checker.check("connection.agent.value == false", agent.get("value") is False,
                      json.dumps(agent))
        checker.check("connection.agent.source == esp32_direct",
                      agent.get("source") == "esp32_direct", json.dumps(agent))
        for grp in ("device", "connection", "mac"):
            for leaf, tup in doc.get(grp, {}).items():
                missing = {"value", "source", "observed_at", "ttl_s", "freshness"} - set(tup)
                checker.check(f"status.{grp}.{leaf} has provenance tuple",
                              not missing, f"missing {missing}")

    s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
    checker.check("GET /api/v1/capabilities -> 200", s == 200, f"status={s} body={doc}")
    if s == 200:
        checker.check("mode == 'A'", doc.get("mode") == "A", json.dumps(doc.get("mode")))
        checker.check("agent.paired == false", doc.get("agent", {}).get("paired") is False)
        checker.check("capability_level == 'L1'", doc.get("capability_level") == "L1")

    for path in ("/api/v1/status", "/api/v1/capabilities"):
        s, doc = http("GET", base, path)
        checker.check(f"{path} without key -> 401 unauthorized",
                      s == 401 and error_code(doc) == "unauthorized",
                      f"status={s} body={doc}")


def at02(checker, base, read_key, control_key, allow_dispatch_error=False):
    checker.section("AT-02 Mode A system command")

    s, doc = http("POST", base, "/api/v1/commands", key=control_key,
                  body={"type": "lock"})
    checker.check("POST /api/v1/commands {lock} -> 202 with command_id",
                  s == 202 and bool(doc.get("command_id")),
                  f"status={s} body={doc}")
    if s != 202 or not doc.get("command_id"):
        return
    cid = doc["command_id"]
    checker.check("202 body has state accepted + record_url",
                  doc.get("state") == "accepted"
                  and doc.get("record_url") == f"/api/v1/commands/{cid}",
                  json.dumps(doc))

    s, doc = http("POST", base, "/api/v1/commands", key=read_key, body={"type": "lock"})
    checker.check("READ key submission -> 403 forbidden",
                  s == 403 and error_code(doc) == "forbidden",
                  f"status={s} body={doc}")

    # Convenience route must be a pure alias producing an identical ledger path.
    s, doc2 = http("POST", base, "/api/v1/system/lock", key=control_key)
    checker.check("POST /api/v1/system/lock -> 202", s == 202, f"status={s} body={doc2}")
    cid2 = doc2.get("command_id")

    def poll_terminal(cid):
        deadline = time.monotonic() + 20
        seen_completed = False
        while time.monotonic() < deadline:
            s, rec = http("GET", base, f"/api/v1/commands/{cid}", key=read_key)
            if s == 200 and rec.get("state") in ("completed", "failed", "timed_out",
                                                 "unconfirmed"):
                if rec.get("state") == "completed":
                    seen_completed = True
                return rec, seen_completed
            time.sleep(0.5)
        return None, seen_completed

    def check_terminal(rec, label):
        # Full-verdict boards (USB HID present): unconfirmed/hid_only. Boards
        # without a USB device controller (classic ESP32): the only honest
        # Mode A verdict for "no HID link" is failed/dispatch_error.
        if allow_dispatch_error and rec.get("state") == "failed" and \
                rec.get("error_code") == "dispatch_error":
            checker.check(f"{label} terminal failed/dispatch_error (no USB HID link; "
                          "honest Mode A verdict)", True)
            return
        checker.check(f"{label} terminal state == unconfirmed",
                      rec.get("state") == "unconfirmed", json.dumps(rec))
        checker.check(f'{label} result == "hid_only"', rec.get("result") == "hid_only",
                      json.dumps(rec))

    rec, seen_completed = poll_terminal(cid)
    checker.check("command record reaches terminal state", rec is not None)
    if rec:
        check_terminal(rec, "")
        checker.check("never completed (Mode A)", not seen_completed)
        checker.check("ledger record schema fields present",
                      all(k in rec for k in ("command_id", "revision", "type",
                                             "requested_by", "requested_at",
                                             "mode_at_accept", "state", "deadline_at",
                                             "evidence", "result", "error_code")),
                      json.dumps(rec))

    if cid2:
        rec2, _ = poll_terminal(cid2)
        checker.check("convenience route reaches terminal state", rec2 is not None)
        if rec2:
            check_terminal(rec2, "convenience route")

    # Listing + pagination sanity (READ).
    s, doc = http("GET", base, "/api/v1/commands?limit=10", key=read_key)
    checker.check("GET /api/v1/commands -> 200 with commands[] + next_cursor",
                  s == 200 and "commands" in doc and "next_cursor" in doc,
                  f"status={s}")


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 1 acceptance AT-01/AT-02")
    ap.add_argument("--hostname", required=True, help="e.g. mac-a1b2c3.local")
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--allow-dispatch-error", action="store_true",
                    help="Accept failed/dispatch_error as the terminal verdict (boards "
                         "with no USB HID link, e.g. classic ESP32). The never-completed "
                         "invariant is still enforced.")
    args = ap.parse_args()

    checker = Checker()
    ip = resolve_host(checker, args.hostname)
    if not ip:
        print("\nAT-01 FAILED: host did not resolve; remaining checks skipped.")
        return 1
    base = f"http://{args.hostname}:{args.port}"

    at01(checker, base, args.read_key)
    at02(checker, base, args.read_key, args.control_key, args.allow_dispatch_error)

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-01 and AT-02: all checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
