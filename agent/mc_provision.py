#!/usr/bin/env python3
"""MacControl endpoint provisioner — serial (console) and network phases.

Two independently runnable subcommands so install.sh stays thin and an
operator can debug each phase alone:

  serial  — one serial session against the device CLI (CH343 UART, 115200):
            pulse RTS into a clean boot, set WiFi, poll until connected,
            create READ/CONTROL/ADMIN API keys, set the admin password.
            Prints a JSON result on stdout for install.sh to consume.

  network — wait for the device on the LAN (mDNS), open the Web UI pairing
            window, run the agent pair ceremony, poll until the agent
            session is ACTIVE. Prints a JSON summary on stdout.

Design constraints (see firmware/docs/DEBUG-PHASE45-USB-LINK.md and the
serial landmines in firmware/AGENTS.md):
  * Opening the serial port MAY reboot the board (macOS asserts DTR/RTS,
    which drive EN/IO0 on the bridge) — but not every driver/cable combo
    wires those through, so the serial phase must not depend on it: open the
    port once, keep it open for the whole phase, deassert DTR/RTS
    immediately, and fall back to poking the CLI when no boot banner appears.
  * WiFi passwords and admin passwords are never echoed to stdout/stderr.
    API keys are parsed from the console and passed to install.sh via the
    JSON result — never printed by this script.

pyserial is imported lazily inside the serial path so this file stays
importable (py_compile / --selftest) without it. Run with the agent venv
python: agent/.venv/bin/python mc_provision.py ...
"""
import argparse
import http.client
import ipaddress
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse

KEY_LINE_RE = re.compile(r"^mck_[A-Za-z0-9_-]{20,}$")
CREATED_LINE_RE = re.compile(r"^created (\S+) \((.*)\)$")
WIFI_CONNECTED_RE = re.compile(r"^connected ip=(\d{1,3}(?:\.\d{1,3}){3})\r?$", re.MULTILINE)
KEY_ID_RE = re.compile(r"key-\d+")

# Terminal signatures that mark a complete reply for each CLI command. The
# console does NOT echo input (firmware cli.cpp handleChar only accumulates
# characters), so reply detection must match the reply text, never the
# command text. Signatures verified against firmware/src/cli.cpp.
WIFI_SET_DONE_RE = re.compile(
    r"wifi credentials saved|persist failed|usage:|unknown command")
WIFI_STATUS_DONE_RE = re.compile(
    r"connected ip=|not connected|usage:|unknown command")
KEY_CREATE_DONE_RE = re.compile(
    r"created key-|key store full|role must be|usage:|unknown command")
ADMIN_SET_DONE_RE = re.compile(
    r"admin password (?:set|NOT set)|usage:|unknown command")
# A crash/reboot mid-command (e.g. a task-watchdog panic under load) replays
# the boot banner on the console; that both proves the device is alive again
# and means the in-flight command was lost.
BOOT_SEEN_RE = re.compile(r"ESP-ROM:esp32s3|Physical console = ADMIN")


class DeviceRebooted(Exception):
    """The device rebooted while a command was in flight (crash/watchdog).
    The console buffer holds a fresh boot banner; retrying the phase is
    safe: 'wifi set'/'admin set' are idempotent and a duplicate 'key create'
    only spends one of the 8 key slots."""


class ProvisionError(Exception):
    """Fatal phase error; the message is already operator-facing."""


# --------------------------------------------------------------------------
# Serial helpers
# --------------------------------------------------------------------------

def _drain_serial(ser, seconds, buffer):
    """Read whatever arrives for `seconds`, appending decoded text to buffer."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        chunk = ser.read(4096)
        if chunk:
            buffer.append(chunk.decode("utf-8", "replace"))
        else:
            time.sleep(0.05)


def _wait_for(serial_mod, port, needle_re, total_s, reboot):
    """Open the port once, optionally pulse RTS, wait for the console.

    Readiness normally means the boot banner, but opening the port does not
    reboot every board (driver/cable dependent), so a console that has been
    up since power-on may never print one: its banner scrolled by long ago
    and the periodic [heap] line (firmware main loop) arrives on a >30 s
    cadence. If the banner has not shown shortly into the wait, poke the CLI
    with 'help' and treat any answer as readiness.
    """
    buffer = []
    ser = serial_mod.Serial(port, 115200, timeout=0.1)
    try:
        # On the S3 USB-Serial/JTAG bridge the CDC lines drive EN (RTS) and
        # GPIO0 (DTR); leaving them asserted can hold the chip in reset.
        ser.dtr = False
        ser.rts = False
        if reboot:
            ser.rts = True
            time.sleep(0.2)
            ser.rts = False
        deadline = time.monotonic() + total_s
        poke_after = min(8.0, total_s / 3.0)
        poked_len = None
        while time.monotonic() < deadline:
            _drain_serial(ser, 0.2, buffer)
            text = "".join(buffer)
            if needle_re.search(text):
                return ser, buffer
            elapsed = total_s - (deadline - time.monotonic())
            if poked_len is None and elapsed >= poke_after:
                ser.write(b"help\n")
                poked_len = len(text)
            elif poked_len is not None and len(text) > poked_len and \
                    text[poked_len:].strip(" \t\r\n\x00"):
                # The console answered the poke even though no boot banner
                # appeared: it is already sitting at the CLI prompt.
                return ser, buffer
        text = "".join(buffer).strip()
        raise ProvisionError(
            "device did not become ready within %ds: no boot banner on %s and "
            "no answer to a 'help' poke.\n"
            "Last output:\n%s\n"
            "Checklist:\n"
            "  1. Power-cycle the board (disconnect the 'com' cable and its PSU "
            "for 5-10 s, reconnect). A hung board prints nothing.\n"
            "  2. The 'com' (CH343 UART) cable must be the one plugged into this "
            "Mac; the 'USB' port is HID-only and never answers the console.\n"
            "  3. Do not hold BOOT while plugging in - it forces ROM download "
            "mode, which never prints the app banner.\n"
            "  4. If a terminal (e.g. 'screen %s 115200') cannot get a reply "
            "to typing 'help' either, the data path is broken: install the WCH "
            "CH34x/CH343 driver (https://www.wch.cn/downloads/CH341SER_MAC_ZIP.html), "
            "or try another USB cable/port. './install.sh --flash' is a quick "
            "data-path test - esptool cannot sync either if RX/TX are broken."
            % (total_s, port, text[-2000:] or "(none)", port))
    except Exception:
        ser.close()
        raise


def _run_cli_command(ser, buffer, consumed, cmd, settle_s, timeout_s=20.0,
                     done_res=()):
    """Send one CLI command and return (output_for_cmd, new_consumed_offset).

    The console never echoes input, so a complete reply is recognized by the
    per-command terminal signatures in `done_res` (matched ~settle_s after
    the first hit so multi-line replies fully arrive), never by the command
    text. `consumed` is the number of buffered characters already processed
    by earlier commands, so repeated commands (e.g. polling 'wifi status')
    parse cleanly.
    """
    if not done_res:
        raise ValueError("done_res must contain at least one reply signature")
    text_before = "".join(buffer)
    ser.write(cmd.encode("utf-8") + b"\n")
    end = time.monotonic() + timeout_s
    matched_at = None
    while True:
        _drain_serial(ser, min(settle_s, max(0.1, end - time.monotonic())), buffer)
        text = "".join(buffer)
        # A fresh boot banner after the command was sent means the device
        # crashed/rebooted mid-command; the reply is never coming (though
        # the serial buffer may replay the command after boot).
        if BOOT_SEEN_RE.search(text[len(text_before):]):
            raise DeviceRebooted()
        tail = text[consumed:]
        if matched_at is None:
            for r in done_res:
                if r.search(tail):
                    matched_at = time.monotonic()
                    break
        elif time.monotonic() - matched_at >= settle_s:
            return tail, len(text)
        if time.monotonic() >= end:
            raise ProvisionError(
                "no reply to %r within %ss; console so far:\n%s"
                % (cmd, timeout_s, text[-2000:]))


def _parse_key_output(after_cmd):
    """Extract (key_id, raw_key) from the output following 'key create ...'."""
    lines = [l.strip() for l in after_cmd.splitlines() if l.strip()]
    key_id, raw = None, None
    for line in lines:
        m = CREATED_LINE_RE.match(line)
        if m:
            key_id = m.group(1)
        if KEY_LINE_RE.match(line):
            raw = line
    if raw is None:
        joined = "\n".join(lines)
        if "key store full" in joined:
            raise ProvisionError(
                "key store full (max 8 active keys): revoke old keys over the "
                "serial console ('key list', 'key revoke <key_id>') or re-run "
                "the installer with --no-keys")
        if "usage:" in joined or "role must be" in joined:
            raise ProvisionError("device CLI rejected the command; output:\n" + joined)
        raise ProvisionError("could not find the raw mck_ key line in the CLI output")
    if key_id is None:
        m = KEY_ID_RE.search(after_cmd)
        key_id = m.group(0) if m else ""
    return key_id, raw


def cmd_serial(args):
    try:
        import serial as serial_mod  # pyserial; only needed for this subcommand
    except ImportError:
        raise ProvisionError(
            "pyserial is not installed; run: "
            "'\"$VENV_DIR/bin/pip\" install esptool pyserial' (install.sh does "
            "this automatically for --flash/--provision)")

    log = getattr(sys, "_mc_console_log", None)
    banner_re = re.compile(r"Physical console = ADMIN")
    for attempt in (1, 2, 3):
        # Only the first attempt pulses RTS: a later attempt follows a
        # crash-reboot the device did on its own, and re-pulsing would just
        # add another reboot.
        ser, buffer = _wait_for(serial_mod, args.port, banner_re,
                                args.boot_timeout, args.reboot and attempt == 1)
        try:
            if log is not None:
                log("serial: console ready on %s (attempt %d/3)" % (args.port, attempt))
            return _serial_phase(ser, buffer, args, log)
        except DeviceRebooted:
            if attempt == 3:
                raise ProvisionError(
                    "device rebooted mid-provisioning three times (crash/watchdog "
                    "under load — see firmware/docs/DEBUG-PROVISION-TWDT.md). "
                    "Completed steps persist on the device; re-run to continue.")
            if log is not None:
                log("serial: device rebooted mid-command (crash/watchdog); "
                    "retrying the phase")
        finally:
            ser.close()


def _serial_phase(ser, buffer, args, log):
    """One attempt at the full serial phase against a ready console."""
    result = {"port": args.port, "keys": {}}
    if args.no_keys and not args.wifi_ssid and not args.admin_password:
        raise ProvisionError("nothing to do: pass --wifi-ssid/--admin-password or drop --no-keys")

    if args.wifi_ssid:
        settle = args.settle
        _, consumed = _run_cli_command(
            ser, buffer, 0, "wifi set %s %s" % (args.wifi_ssid, args.wifi_pass),
            settle, done_res=(WIFI_SET_DONE_RE,))
        ip = None
        deadline = time.monotonic() + args.wifi_timeout
        while time.monotonic() < deadline:
            out, consumed = _run_cli_command(
                ser, buffer, consumed, "wifi status", settle, timeout_s=10.0,
                done_res=(WIFI_STATUS_DONE_RE,))
            m = WIFI_CONNECTED_RE.search(out)
            if m:
                ip = m.group(1)
                break
            if "not connected" in out and (time.monotonic() + settle) >= deadline:
                break
        if ip is None:
            raise ProvisionError(
                "device did not connect to WiFi '%s' within %ds; check the "
                "SSID/password and re-run (phases are idempotent)"
                % (args.wifi_ssid, args.wifi_timeout))
        result["ip"] = ip
        if log is not None:
            log("serial: WiFi connected, ip=%s" % ip)

    if not args.no_keys:
        consumed = consumed if args.wifi_ssid else 0
        for role in ("READ", "CONTROL", "ADMIN"):
            out, consumed = _run_cli_command(
                ser, buffer, consumed, "key create %s %s" % (role, args.keys_label),
                args.settle, done_res=(KEY_CREATE_DONE_RE,))
            key_id, raw = _parse_key_output(out)
            result["keys"][role] = {"key_id": key_id, "raw": raw}
            if log is not None:
                log("serial: created %s key %s (raw key captured, not printed)"
                    % (role, key_id or "?"))

    if args.admin_password:
        consumed = consumed if (args.wifi_ssid or not args.no_keys) else 0
        out, _ = _run_cli_command(ser, buffer, consumed,
                                  "admin set %s" % args.admin_password, args.settle,
                                  done_res=(ADMIN_SET_DONE_RE,))
        if "admin password set" not in out:
            tail = "\n".join(l for l in out.splitlines() if l.strip())[-500:]
            # The console does not echo input, so the password never
            # appears in the reply; scrub it anyway before it can reach
            # logs or error text.
            for token in (args.admin_password,):
                tail = tail.replace(token, "***")
            raise ProvisionError("admin set failed: " + (tail or "(no output)"))
        if log is not None:
            log("serial: admin password set")

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")
    return 0


# --------------------------------------------------------------------------
# Network helpers
# --------------------------------------------------------------------------

def _http_json(host, method, path, body=None, headers=None, timeout=8.0,
               allow_statuses=(200,)):
    """One HTTP/1.1 request (JSON in, parsed JSON out), with cookie support."""
    hdrs = dict(headers or {})
    payload = None
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        hdrs["Content-Type"] = "application/json"
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    conn = http.client.HTTPSConnection(host, 443, timeout=timeout, context=ctx)
    try:
        conn.request(method, path, body=payload, headers=hdrs)
        resp = conn.getresponse()
        raw = resp.read()
    except (OSError, http.client.HTTPException) as exc:
        conn.close()
        raise ProvisionError("HTTP %s %s failed: %s" % (method, path, exc))
    set_cookie = resp.getheader("Set-Cookie")
    conn.close()
    if resp.status not in allow_statuses:
        raise ProvisionError("HTTP %s %s -> %s %s" % (method, path, resp.status,
                                                      raw.decode("utf-8", "replace")[:300]))
    try:
        doc = json.loads(raw.decode("utf-8", "replace")) if raw else None
    except ValueError:
        raise ProvisionError("HTTP %s %s -> non-JSON body" % (method, path))
    return resp.status, doc, set_cookie


def _resolve_hostname(hostname, log):
    """mDNS (.local) resolution with retries; returns an IP string."""
    end = time.monotonic() + args_resolve_timeout
    last_err = None
    while time.monotonic() < end:
        try:
            infos = socket.getaddrinfo(hostname, 80, socket.AF_INET, socket.SOCK_STREAM)
            for info in infos:
                ip = info[4][0]
                try:
                    ipaddress.IPv4Address(ip)  # skip IPv6 / link-local oddities
                    return ip
                except ValueError:
                    continue
        except OSError as exc:
            last_err = exc
        time.sleep(2.0)
    raise ProvisionError("could not resolve '%s' via mDNS after %ds (%s). The device "
                         "advertises _maccontrol._tcp; check it is on the same network "
                         "('dns-sd -B _maccontrol._tcp local')."
                         % (hostname, args_resolve_timeout, last_err))


args_resolve_timeout = 90.0  # set from --resolve-timeout in cmd_network


def _pair_subprocess(argv):
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=120)
    out = proc.stdout or ""
    return proc.returncode, out


def cmd_network(args):
    global args_resolve_timeout
    args_resolve_timeout = args.resolve_timeout
    log = getattr(sys, "_mc_console_log", None)

    # 1. Wait for the device to answer HTTP at all (unauthenticated /api/v1/status
    #    must 401; any HTTP response means the web server is up).
    base = args.hostname if "." in args.hostname else args.hostname + ".local"
    ip = _resolve_hostname(base, log)
    if log is not None:
        log("network: %s resolves to %s" % (base, ip))
    deadline = time.monotonic() + args.http_timeout
    while True:
        try:
            status, _, _ = _http_json(ip, "GET", "/api/v1/status", timeout=6.0,
                                      allow_statuses=(200, 401, 403))
            break
        except ProvisionError as exc:
            if time.monotonic() >= deadline:
                raise ProvisionError("device at %s is not answering HTTP (%s)" % (ip, exc))
            time.sleep(2.0)
    if log is not None:
        log("network: web server is up (GET /api/v1/status -> %s)" % status)

    # 2. If a Read key is available, confirm it works and learn the
    #    authoritative hostname; otherwise fall back to the --hostname arg.
    read_key = os.environ.get("MC_READ_KEY", "")
    auth = {"Authorization": "Bearer " + read_key} if read_key else {}
    device_hostname = ""
    if read_key:
        _, status_doc, _ = _http_json(ip, "GET", "/api/v1/status", headers=auth)
        device_hostname = ((status_doc or {}).get("device") or {}).get("hostname") or ""
        if not device_hostname:
            raise ProvisionError("/api/v1/status did not include device.hostname")
    if not device_hostname:
        device_hostname = args.hostname.split(".")[0]
        if log is not None:
            log("network: no READ key; using hostname '%s' from the command line"
                % device_hostname)
    elif log is not None:
        log("network: device hostname is '%s'" % device_hostname)

    # 3. Web UI login (session cookie acts as ADMIN on /api/v1/*).
    _, login_doc, set_cookie = _http_json(ip, "POST", "/ui/login",
                                          body={"password": args.admin_password},
                                          allow_statuses=(200,))
    if not (isinstance(login_doc, dict) and login_doc.get("ok")):
        raise ProvisionError("Web UI login failed (wrong admin password?)")
    cookie = ""
    if set_cookie:
        cookie = set_cookie.split(";", 1)[0]
    if not cookie:
        raise ProvisionError("Web UI login did not return a session cookie")
    sess = {"Cookie": cookie}

    _, pairing_doc, _ = _http_json(ip, "GET", "/api/v1/pairing", headers=sess)
    if log is not None:
        state = ((pairing_doc or {}).get("state") or "?")
        window = (pairing_doc or {}).get("window") or {}
        log("network: pairing state=%s window_open=%s" % (state, window.get("open")))

    _, window_doc, _ = _http_json(ip, "POST", "/api/v1/pairing/window",
                                  body={"duration_s": args.pair_code_timeout}, headers=sess)
    code = (window_doc or {}).get("pairing_code") or ""
    if not code:
        raise ProvisionError("pairing window response had no pairing_code")
    if log is not None:
        log("network: pairing window open (%ss)" % (window_doc or {}).get("seconds_remaining"))

    # 4. Run the agent pairing ceremony (token persisted by the agent itself).
    pair_argv = [sys.executable, "-m", "maccontrol_agent",
                 "--hostname", device_hostname, "--pair-code", code]
    rc, out = _pair_subprocess(pair_argv)
    if rc != 0 or ("PAIRED" not in out):
        tail = out.strip()[-500:]
        raise ProvisionError("agent pairing failed (exit %s): %s" % (rc, tail))
    if log is not None:
        log("network: agent paired with %s" % device_hostname)

    # 5. Poll until the endpoint reports an active agent session.
    session_active = False
    if read_key:
        deadline = time.monotonic() + args.session_timeout
        while time.monotonic() < deadline:
            try:
                _, agent_doc, _ = _http_json(ip, "GET", "/api/v1/agent/status", headers=auth,
                                             timeout=6.0)
                if (agent_doc or {}).get("session_active"):
                    session_active = True
                    break
            except ProvisionError:
                pass
            time.sleep(2.0)
    elif log is not None:
        log("network: no READ key; skipping the agent-session check")

    json.dump({"hostname": device_hostname, "paired": True,
               "session_active": session_active}, sys.stdout)
    sys.stdout.write("\n")
    if not session_active:
        # Not fatal: the LaunchAgent starts the agent at login anyway.
        if log is not None:
            log("network: WARNING: agent session not ACTIVE within %ds "
                "(it will connect when the LaunchAgent loads)" % args.session_timeout)
    return 0


# --------------------------------------------------------------------------
# Self-test (parser unit checks against captured example outputs)
# --------------------------------------------------------------------------

def _selftest():
    # The console does not echo input and prints no prompt — fixtures match
    # firmware/src/cli.cpp output shapes exactly.
    out = _parse_key_output(
        "created key-01 (install)\r\n"
        "API key (shown once, store it now):\r\n"
        "mck_9f8e7d6c5b4a3-210f1e2d3c4b5a69788\r\n")
    assert out == ("key-01", "mck_9f8e7d6c5b4a3-210f1e2d3c4b5a69788"), out

    try:
        _parse_key_output("key store full (max 8 active keys)\r\n")
    except ProvisionError as exc:
        assert "key store full" in str(exc)
    else:
        raise AssertionError("expected ProvisionError for full key store")

    m = WIFI_CONNECTED_RE.search("connected ip=10.10.40.242\r\n")
    assert m and m.group(1) == "10.10.40.242", m
    assert not WIFI_CONNECTED_RE.search("not connected\r\n")
    assert not WIFI_CONNECTED_RE.search(
        "wifi credentials saved; reconnecting\r\n")  # 'wifi set' reply must not
    # trip the 'connected ip=' matcher while polling

    m = CREATED_LINE_RE.match("created key-02 (my label)")
    assert m and m.group(1) == "key-02" and m.group(2) == "my label", m
    # Real raw keys are base64url (may contain '-' and '_'), e.g. the
    # NDC install: mck_JW6v9B0jHfmPH-OrR1jsIRDFlX9RsQ0ZmJtM7yOVA2o
    assert KEY_LINE_RE.match("mck_JW6v9B0jHfmPH-OrR1jsIRDFlX9RsQ0ZmJtM7yOVA2o")
    print("selftest: ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mc_provision.py",
                                 description="MacControl endpoint provisioner "
                                             "(serial console and network phases).")
    sub = ap.add_subparsers(dest="subcommand")

    sp = sub.add_parser("serial", help="serial-console provisioning (WiFi, API keys, admin password)")
    sp.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.wchusbserial*")
    sp.add_argument("--baud", type=int, default=115200)
    sp.add_argument("--wifi-ssid", dest="wifi_ssid", default="")
    sp.add_argument("--wifi-pass", dest="wifi_pass", default="")
    sp.add_argument("--admin-password", dest="admin_password", default="")
    sp.add_argument("--keys-label", dest="keys_label", default="install")
    sp.add_argument("--no-keys", action="store_true", help="skip API key creation")
    sp.add_argument("--reboot", action="store_true",
                    help="pulse RTS to reboot into a known state first")
    sp.add_argument("--settle", type=float, default=1.2,
                    help="seconds to wait for output after each command")
    sp.add_argument("--boot-timeout", type=float, default=30.0)
    sp.add_argument("--wifi-timeout", type=float, default=60.0)
    sp.set_defaults(func=cmd_serial)

    np = sub.add_parser("network", help="network phase (pairing window + agent pairing)")
    np.add_argument("--hostname", required=True,
                    help="endpoint hostname (bare label; .local is appended)")
    np.add_argument("--admin-password", dest="admin_password", required=True)
    np.add_argument("--pair-code-timeout", type=int, default=300,
                    help="pairing window duration, 60-600 (default 300)")
    np.add_argument("--resolve-timeout", type=float, default=90.0)
    np.add_argument("--http-timeout", type=float, default=90.0)
    np.add_argument("--session-timeout", type=float, default=45.0)
    np.set_defaults(func=cmd_network)

    ap.add_argument("--selftest", action="store_true",
                    help="run parser unit checks and exit (no device needed)")
    args = ap.parse_args(argv)
    if args.selftest:
        return _selftest()
    if not getattr(args, "func", None):
        ap.print_help(sys.stderr)
        return 2
    if args.subcommand == "serial" and args.baud != 115200:
        print("WARNING: the device console is fixed at 115200; ignoring --baud",
              file=sys.stderr)
    try:
        return args.func(args)
    except ProvisionError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
