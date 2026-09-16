#!/usr/bin/env python3
"""MacControl Phase 2 acceptance tests AT-03 and AT-04 (spec section 16.1).

AT-03: macro creation and execution via API + HTTP trigger.
AT-04: persistence across reboot (identity, macros, triggers, API keys) and
ledger boot reconciliation of an in-flight record to failed/esp32_restarted.

The board is rebooted over the serial port (DTR/RTS pulse) — pass
--serial-port. On boards without a USB HID link, use --allow-dispatch-error to
accept failed/dispatch_error as the honest Mode A terminal verdict.

Usage:
  python3 at03_at04.py --hostname mac-b11650.local \
      --read-key mck_... --control-key mck_... --admin-key mck_... \
      --serial-port /dev/cu.usbserial-8320 [--allow-dispatch-error]
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


def http(method, base, path, key=None, body=None, timeout=10):
    headers = {}
    if key:
        headers["Authorization"] = f"Bearer {key}"
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}
    except Exception as e:
        return None, {"transport_error": str(e)}


def _ensure_serial():
    try:
        import serial  # noqa: F401
        return
    except ImportError:
        import glob
        here = os.path.dirname(os.path.abspath(__file__))
        for sp in glob.glob(os.path.join(here, "..", ".venv", "lib", "python*", "site-packages")):
            sys.path.insert(0, sp)


def reset_board(port):
    _ensure_serial()
    import serial
    p = serial.Serial(port, 115200, timeout=1)
    p.setDTR(False)
    p.setRTS(True)
    time.sleep(0.15)
    p.setRTS(False)
    p.close()


def wait_for_host(hostname, timeout_s=45):
    import socket
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            socket.getaddrinfo(hostname, 80, proto=socket.IPPROTO_TCP)
            return True
        except Exception:
            time.sleep(1)
    return False


def at03(checker, base, control_key, admin_key, allow_dispatch_error):
    checker.section("AT-03 Macro creation and execution")

    # Create a 3-step macro (combo, delay, text) per spec 16.1. The name is
    # unique per run so the suite is re-runnable (names are endpoint-unique).
    import random
    suffix = f"{random.randrange(0x10000):04X}"
    macro_def = {
        "name": f"AT03 Spotlight {suffix}",
        "timeout_ms": 10000,
        "steps": [
            {"order": 1, "type": "key_combo", "modifiers": ["cmd"], "key": "space"},
            {"order": 2, "type": "delay", "delay_ms": 100},
            {"order": 3, "type": "text", "value": "AT03"},
        ],
    }
    s, doc = http("POST", base, "/api/v1/macros", key=admin_key, body=macro_def)
    checker.check("POST /api/v1/macros -> 201", s == 201, f"status={s} body={doc}")
    if s != 201:
        return None
    mid = doc.get("macro_id")
    checker.check("macro_id assigned", bool(mid), json.dumps(doc))
    checker.check("revision == 1", doc.get("revision") == 1, json.dumps(doc))

    # Invalid macro rejected with macro_invalid_step (spec 10.1.1).
    bad = {"name": f"AT03 bad {suffix}", "steps": [{"order": 1, "type": "mouse_move"}]}
    s, doc = http("POST", base, "/api/v1/macros", key=admin_key, body=bad)
    checker.check("invalid step type -> 400 macro_invalid_step",
                  s == 400 and (doc.get("error") or {}).get("code") == "macro_invalid_step",
                  f"status={s} body={doc}")

    # Capabilities advertise the macro (spec 12.3.1).
    # (READ key check happens in AT-01; here just verify via control-visible surface.)
    # HTTP trigger: POST /api/v1/macros/{id}/execute (CONTROL).
    t0 = time.monotonic()
    s, doc = http("POST", base, f"/api/v1/macros/{mid}/execute", key=control_key)
    checker.check("macro execute -> 202 with command_id",
                  s == 202 and bool(doc.get("command_id")), f"status={s} body={doc}")
    if s != 202:
        return mid
    cid = doc["command_id"]
    elapsed_accept = time.monotonic() - t0

    deadline = time.monotonic() + 25
    rec = None
    while time.monotonic() < deadline:
        s, rec = http("GET", base, f"/api/v1/commands/{cid}", key=control_key)
        if s == 200 and rec.get("state") in ("completed", "failed", "timed_out",
                                             "unconfirmed"):
            break
        rec = None
        time.sleep(0.5)
    checker.check("macro command reaches terminal state", rec is not None)
    if rec:
        if allow_dispatch_error and rec.get("state") == "failed" and \
                rec.get("error_code") in ("dispatch_error", "usb_disconnected"):
            checker.check(f"macro terminal failed/{rec.get('error_code')} (no USB HID link; "
                          "honest Mode A verdict)", True)
        else:
            checker.check("macro terminal state == unconfirmed",
                          rec.get("state") == "unconfirmed", json.dumps(rec))
            checker.check('macro result == "hid_only"',
                          rec.get("result") == "hid_only", json.dumps(rec))
        checker.check("never completed (Mode A)", rec.get("state") != "completed")
        checker.check("ledger type == macro_execute", rec.get("type") == "macro_execute",
                      json.dumps(rec))
    checker.check(f"accept-to-202 latency sane ({elapsed_accept:.2f} s)",
                  elapsed_accept < 2.0)
    return mid


def at04(checker, base, read_key, control_key, admin_key, serial_port, hostname,
         mid, allow_dispatch_error):
    checker.section("AT-04 Persistence across reboot")

    s, id_before = http("GET", base, "/api/v1/status", key=read_key)
    checker.check("status readable pre-reboot", s == 200, f"status={s}")
    device_id = id_before.get("device", {}).get("device_id", {}).get("value") if s == 200 else None

    # Bind a webui_button trigger to the AT-03 macro.
    if mid:
        trig = {"source": "webui_button", "macro_id": mid, "enabled": True}
        s, tdoc = http("POST", base, "/api/v1/triggers", key=admin_key, body=trig)
        checker.check("create trigger binding", s in (200, 201), f"status={s} body={tdoc}")

    # Start a command that stays in-flight long enough to survive the reset:
    # with no HID link the dispatcher polls re-enumeration ~11 s before failing,
    # so a fresh lock command is non-terminal for several seconds.
    s, doc = http("POST", base, "/api/v1/commands", key=control_key, body={"type": "lock"})
    inflight_id = doc.get("command_id") if s == 202 else None
    checker.check("in-flight command accepted pre-reboot", s == 202, f"status={s} body={doc}")

    print("  ... resetting board over serial ...")
    reset_board(serial_port)
    if not wait_for_host(hostname):
        checker.check("board back online after reset", False, "host did not resolve")
        return
    checker.check("board back online after reset", True)

    s, id_after = http("GET", base, "/api/v1/status", key=read_key)
    checker.check("status readable post-reboot", s == 200, f"status={s}")
    if s == 200:
        dev = id_after.get("device", {}).get("device_id", {}).get("value")
        checker.check("device_id persists across reboot", dev == device_id and bool(dev),
                      f"before={device_id} after={dev}")

    s, doc = http("GET", base, "/api/v1/macros", key=read_key)
    ok = s == 200 and any(m.get("macro_id") == mid for m in doc.get("macros", []))
    checker.check("macros persist across reboot", ok, f"status={s} body={doc}")

    s, doc = http("GET", base, "/api/v1/triggers", key=read_key)
    ok = s == 200 and len(doc.get("triggers", [])) >= 1
    checker.check("trigger bindings persist across reboot", ok, f"status={s} body={doc}")

    s, doc = http("GET", base, "/api/v1/commands?limit=1", key=read_key)
    checker.check("ledger persists across reboot (list works)", s == 200, f"status={s}")

    # Authentication still works (keys persisted): an unauthenticated request
    # must still 401, an authenticated one 200.
    s, _ = http("GET", base, "/api/v1/status")
    checker.check("keys persist (unauth still 401)", s == 401, f"status={s}")

    # Boot reconciliation: the in-flight record resolves failed/esp32_restarted.
    if inflight_id:
        deadline = time.monotonic() + 15
        rec = None
        while time.monotonic() < deadline:
            s, rec = http("GET", base, f"/api/v1/commands/{inflight_id}", key=read_key)
            if s == 200 and rec and rec.get("state") in ("failed", "timed_out",
                                                         "unconfirmed", "completed"):
                break
            rec = None
            time.sleep(0.5)
        checker.check("in-flight record resolved by boot reconciliation", rec is not None)
        if rec:
            checker.check("reconciled failed/esp32_restarted",
                          rec.get("state") == "failed" and
                          rec.get("error_code") == "esp32_restarted", json.dumps(rec))
    else:
        checker.check("in-flight record resolved by boot reconciliation", False,
                      "no in-flight command id")


def main():
    ap = argparse.ArgumentParser(description="MacControl Phase 2 acceptance AT-03/AT-04")
    ap.add_argument("--hostname", required=True)
    ap.add_argument("--read-key", required=True)
    ap.add_argument("--control-key", required=True)
    ap.add_argument("--admin-key", required=True)
    ap.add_argument("--serial-port", required=True, help="serial device for board reset")
    ap.add_argument("--allow-dispatch-error", action="store_true")
    args = ap.parse_args()

    checker = Checker()
    base = f"http://{args.hostname}:80"

    mid = at03(checker, base, args.control_key, args.admin_key, args.allow_dispatch_error)
    at04(checker, base, args.read_key, args.control_key, args.admin_key,
         args.serial_port, args.hostname, mid, args.allow_dispatch_error)

    print("\n=== SUMMARY ===")
    if checker.failures:
        print(f"{len(checker.failures)} check(s) failed:")
        for f in checker.failures:
            print(f"  - {f}")
        return 1
    print("AT-03 and AT-04: all checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
