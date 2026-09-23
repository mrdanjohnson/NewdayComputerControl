#!/usr/bin/env python3
"""MacControl Phase 4 acceptance test AT-06 (spec section 16.2): pairing
ceremony.

AT-06 (spec): start the pairing window on the endpoint, complete the ceremony
in the MCA within the time box; exactly one stored pairing; the MCA WebSocket
connects; a late second pairing attempt is rejected. Traces: Ch. 3.2, 4.2.1.

This script additionally covers the Phase 4 wire contract around pairing:
wrong-code rejection (403 while the window is open), 5 wrong codes closing the
window (409 agent_not_paired afterwards), rate-limit spacing, Mode B
capability advertisement, heartbeat freshness over the controller API,
agent-auth rejection (401), mDNS TXT advertisement (mode=B / pair=active),
and the token-leak check (the agent bearer token must never appear in the
endpoint's logs).

The MCA daemon is left RUNNING at the end (AT-11 continues from here).

Usage:
  python3 at06.py --hostname mac-a1b2c3.local \
      --read-key mck_... --control-key mck_... \
      --serial-port /dev/cu.usbmodem83101

Exit code 0 = all checks pass.
"""

import argparse
import json
import os
import re
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

PAIR_RE = re.compile(r"PAIRING CODE:\s*([A-Za-z0-9]{8})")

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


def _ensure_serial():
    try:
        import serial  # noqa: F401
        return
    except ImportError:
        import glob
        for sp in glob.glob(os.path.join(HERE, "..", ".venv", "lib", "python*", "site-packages")):
            sys.path.insert(0, sp)


class SerialSession:
    """Holds the provisioning port open for the WHOLE test.

    Landmine (observed on the S3 CH343 bridge, 2026-09-22): opening the port
    resets the ESP32 (CDC lines drive EN/IO0) and — critically — CLOSING it
    asserts DTR/RTS again, rebooting the device seconds later and destroying
    the freshly opened pairing window (RAM-only state). Keep the port open
    and deasserted until the script exits; the reboot then happens after the
    ceremony, which is harmless (pairing state persists in NVS).
    """

    def __init__(self, port):
        _ensure_serial()
        import serial
        self.buf = bytearray()
        self.p = serial.Serial(port, 115200, timeout=0.1)
        # On the S3 USB-Serial/JTAG bridge the CDC lines drive EN (RTS) and
        # GPIO0 (DTR); leaving them asserted holds the chip in reset/download.
        self.p.dtr = False
        self.p.rts = False
        self._drain(3.5)  # boot after the port-open reset

    def _drain(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            chunk = self.p.read(4096)
            if chunk:
                self.buf.extend(chunk)
            else:
                time.sleep(0.05)

    def open_window(self, window_s):
        """Send `agent pair <window_s>`; return the 8-char code or None."""
        before = len(self.buf)
        self.p.write(b"agent pair %d\n" % int(window_s))
        self._drain(2.0)
        m = PAIR_RE.search(self.buf[before:].decode("utf-8", "replace"))
        return m.group(1) if m else None

    def window_state(self):
        """Device-side truth: 'pairing state: <state>' from `agent status`.
        Detects the silent DTR/RTS reboots that destroy RAM-only windows."""
        before = len(self.buf)
        try:
            self.p.write(b"agent status\n")
        except Exception:
            return "port_lost"
        self._drain(1.2)
        out = self.buf[before:].decode("utf-8", "replace")
        m = re.search(r"pairing state:\s*(\w+)", out)
        return m.group(1) if m else "unknown"

    def close(self):
        try:
            self.p.close()
        except Exception:
            pass


def step1_open_window(checker, session, window_s):
    checker.section("AT-06 step 1: open pairing window over serial CLI")
    code = session.open_window(window_s)
    checker.check("`agent pair` returned a PAIRING CODE line", bool(code),
                  "no 'PAIRING CODE: <8 chars>' in serial output")
    return code


def step2_wrong_code_robustness(checker, base):
    checker.section("AT-06 step 2: wrong-code robustness (5 wrong codes close the window)")
    body = {"pairing_code": "ZZZZZZZZ", "agent_instance_id": "ag-at06xx",
            "agent_version": "1.0.0"}
    results = []
    for i in range(6):
        if i:
            time.sleep(1.05)  # >1 attempt/s triggers 429 rate_limited
        s, doc = http("POST", base, "/agent/v1/pair", body=body)
        results.append((s, error_code(doc)))
    checker.check("1st wrong code -> 403 forbidden (window open)",
                  results[0] == (403, "forbidden"), f"result={results[0]}")
    checker.check("wrong codes 2-4 -> 403 forbidden (window still open)",
                  all(r == (403, "forbidden") for r in results[1:4]),
                  f"results={results[1:4]}")
    checker.check("5th or 6th wrong code -> 409 agent_not_paired (window closed)",
                  any(s == 409 and c == "agent_not_paired" for s, c in results[4:]),
                  f"results={results[4:]}")
    checker.check("no 429 rate_limited during the burst (1.05 s spacing)",
                  all(s != 429 for s, _ in results), f"results={results}")


def step3_pairing_ceremony(checker, hostname, code, state_file):
    checker.section("AT-06 step 3: pairing ceremony via the MCA")
    print(f"  $ {AGENT_PY} -m maccontrol_agent --hostname {hostname} "
          f"--pair-code <redacted> --state-file {state_file} --headless")
    try:
        proc = subprocess.run(
            [AGENT_PY, "-m", "maccontrol_agent", "--hostname", hostname,
             "--pair-code", code, "--state-file", state_file, "--headless"],
            capture_output=True, text=True, timeout=60, cwd=AGENT_DIR)
    except subprocess.TimeoutExpired:
        checker.check("pairing ceremony completes within 60 s", False, "timeout")
        return None
    except OSError as e:
        checker.check("MCA venv python runnable", False, str(e))
        return None
    checker.check("pairing ceremony exit 0", proc.returncode == 0,
                  f"rc={proc.returncode} stderr={proc.stderr.strip()[-300:]!r}")
    checker.check('pairing ceremony prints "PAIRED"', "PAIRED" in proc.stdout,
                  f"stdout={proc.stdout.strip()[-200:]!r}")
    if proc.returncode != 0:
        return None

    try:
        with open(state_file, "r", encoding="utf-8") as fh:
            st = json.load(fh)
    except Exception as e:
        checker.check("state file persisted and readable", False, str(e))
        return None
    checker.check("state file persisted and readable", True)
    iid = st.get("agent_instance_id")
    checker.check("state carries agent_instance_id (ag-XXXX) and agent_token",
                  isinstance(iid, str) and iid.startswith("ag-") and bool(st.get("agent_token")),
                  f"instance_id={iid!r} token_present={bool(st.get('agent_token'))}")
    return st


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


def step4_start_daemon(checker, base, read_key, hostname, state_file, bundle_id):
    checker.section("AT-06 step 4: start MCA daemon and wait for connection")
    pid = start_agent_daemon(hostname, state_file, bundle_id, transport="websocket")
    print(f"  ... daemon started (pid {pid}, log {DAEMON_LOG}); "
          f"waiting up to 90 s for agent.connected")
    # Generous window: if the serial session dropped right after the
    # ceremony, the DTR/RTS landmine reboots the device and the agent only
    # connects once it is back (~30-60 s).
    elapsed, doc = wait_connected(base, read_key, 90)
    checker.check("agent.connected == true within 90 s", elapsed is not None,
                  f"last_status={doc.get('agent') if isinstance(doc, dict) else doc}")
    return pid


def step5_mode_b_evidence(checker, base, read_key, control_key, hostname,
                          state_file, bundle_id):
    checker.section("AT-06 step 5: Mode B capability evidence")
    s, doc = http("GET", base, "/api/v1/capabilities", key=read_key)
    checker.check("GET /api/v1/capabilities -> 200", s == 200, f"status={s}")
    if s == 200:
        checker.check("mode == 'B' (paired)", doc.get("mode") == "B",
                      json.dumps(doc.get("mode")))
        checker.check("capability_level == 'L2'", doc.get("capability_level") == "L2",
                      json.dumps(doc.get("capability_level")))
        agent = doc.get("agent") or {}
        checker.check("agent.paired == true and agent.connected == true",
                      agent.get("paired") is True and agent.get("connected") is True,
                      json.dumps(agent))
        checker.check("bundle_id present in agent.allowlisted_apps",
                      bundle_id in (agent.get("allowlisted_apps") or []),
                      json.dumps(agent.get("allowlisted_apps")))
        cmds = doc.get("commands") or {}
        launch = cmds.get("app_launch") or {}
        quit_ = cmds.get("app_quit") or {}
        checker.check("app_launch available+verified true",
                      launch.get("available") is True and launch.get("verified") is True,
                      json.dumps(launch))
        checker.check("app_quit available+verified true",
                      quit_.get("available") is True and quit_.get("verified") is True,
                      json.dumps(quit_))

    checker.section("AT-06 step 5: agent-reported status tuples")
    s, doc = http("GET", base, "/api/v1/status", key=read_key)
    checker.check("GET /api/v1/status -> 200", s == 200, f"status={s}")
    if s == 200:
        conn = (doc.get("connection") or {}).get("agent") or {}
        checker.check("connection.agent.value true, source esp32_direct",
                      conn.get("value") is True and conn.get("source") == "esp32_direct",
                      json.dumps(conn))
        mac_state = (doc.get("mac") or {}).get("state") or {}
        checker.check("mac.state freshness 'fresh' (agent heartbeat flowing)",
                      mac_state.get("freshness") == "fresh", json.dumps(mac_state))

    checker.section("AT-06 step 5: late second pairing attempt rejected")
    try:
        with open(state_file, "r", encoding="utf-8") as fh:
            st = json.load(fh)
    except Exception:
        st = {}
    late = {"pairing_code": "EXPIRED1", "agent_instance_id": st.get("agent_instance_id") or "ag-late01",
            "agent_version": "1.0.0"}
    s, doc = http("POST", base, "/agent/v1/pair", body=late)
    checker.check("second pairing attempt -> 409 agent_not_paired",
                  s == 409 and error_code(doc) == "agent_not_paired",
                  f"status={s} body={doc}")

    checker.section("AT-06 step 5: agent-auth rejection")
    s, doc = http("POST", base, "/agent/v1/events", key="bogus",
                  body={"events": [{"type": "heartbeat", "seq": 1}]})
    checker.check("POST /agent/v1/events with bogus bearer -> 401 unauthorized",
                  s == 401 and error_code(doc) == "unauthorized",
                  f"status={s} body={doc}")

    checker.section("AT-06 step 5: mDNS TXT advertisement")
    instance = hostname[:-6] if hostname.endswith(".local") else hostname
    try:
        proc = subprocess.Popen(["dns-sd", "-L", instance, "_maccontrol._tcp", "local"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        txt = out.decode("utf-8", "replace")
    except OSError as e:
        txt = ""
        print(f"  [WARN] dns-sd unavailable: {e}")
    checker.check("mDNS TXT advertises mode=B", "mode=B" in txt)
    checker.check("mDNS TXT advertises pair=active", "pair=active" in txt)

    checker.section("AT-06 step 5: token-leak check (agent token never logged)")
    token = st.get("agent_token") or ""
    s, doc = http("GET", base, "/api/v1/logs?limit=512", key=read_key)
    checker.check("GET /api/v1/logs -> 200", s == 200, f"status={s}")
    if s == 200 and token:
        blob = json.dumps(doc)
        checker.check("agent_token string absent from endpoint logs", token not in blob)
    elif not token:
        checker.check("agent_token string absent from endpoint logs", False,
                      "no agent_token in state file to compare against")


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 4 acceptance AT-06: pairing ceremony")
    ap.add_argument("--hostname", required=True)
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--serial-port", required=True, help="serial device for the endpoint CLI")
    ap.add_argument("--bundle-id", default="com.apple.TextEdit")
    ap.add_argument("--state-file", default="/tmp/mc_at_agent.json")
    ap.add_argument("--window-s", type=int, default=120)
    args = ap.parse_args()

    if not os.path.exists(AGENT_PY):
        print(f"MCA venv python not found at {AGENT_PY}; "
              "create ../agent/.venv per agent/README.md", file=sys.stderr)
        return 1

    checker = Checker()
    base = f"http://{args.hostname}:80"

    session = SerialSession(args.serial_port)
    try:
        # The port-open reset boots the device; Wi-Fi association takes
        # ~10-20 s. Wait for the HTTP surface before touching the pairing
        # endpoints (otherwise the ceremony hits unresolvable mDNS).
        t_up = time.monotonic()
        up = False
        while time.monotonic() - t_up < 90:
            s_probe, _ = http("GET", base, "/api/v1/status", key=args.read_key)
            if s_probe == 200:
                up = True
                break
            time.sleep(2.0)
        checker.check("endpoint HTTP surface up after serial reset", up,
                      "no 200 from /api/v1/status within 90 s")
        code = step1_open_window(checker, session, args.window_s) if up else None
        if code:
            step2_wrong_code_robustness(checker, base)
            # The wrong-code burst closed the first window; open a fresh one.
            checker.section("AT-06 step 2b: fresh window for the real ceremony")
            code = session.open_window(args.window_s)
            checker.check("fresh `agent pair` returned a new PAIRING CODE", bool(code),
                          "no 'PAIRING CODE: <8 chars>' in serial output")
            st = session.window_state()
            checker.check("device still holds the fresh window (no silent reboot)",
                          st == "pairing_window", f"window_state={st}")
            if st != "pairing_window":
                code = None
        if code:
            state = step3_pairing_ceremony(checker, args.hostname, code, args.state_file)
            if not state:
                # Diagnose silent DTR/RTS reboots: the window is RAM-only, so
                # a reset between the serial check and the POST reads exactly
                # like a closed window.
                st = session.window_state()
                print(f"  ... post-ceremony device state: {st}", flush=True)
            if state:
                pid = step4_start_daemon(checker, base, args.read_key, args.hostname,
                                         args.state_file, args.bundle_id)
                if pid:
                    step5_mode_b_evidence(checker, base, args.read_key, args.control_key,
                                          args.hostname, args.state_file, args.bundle_id)
    finally:
        # Closing the port reboots the endpoint (DTR/RTS landmine) — do it
        # only after the ceremony, when losing RAM state is harmless.
        session.close()

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-06: all checks passed.")
    try:
        pid = int(open(PIDFILE).read().strip())
    except Exception:
        pid = None
    if pid:
        print(f"\nNOTE: MCA daemon left RUNNING for AT-11 (pid {pid}, log {DAEMON_LOG}).")
        print(f"      Stop it with: kill {pid}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
