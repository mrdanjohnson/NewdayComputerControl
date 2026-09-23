"""MacControlAgent CLI: pair, serve, show log.

Usage:
  python -m maccontrol_agent [--hostname H] [--pair-code CODE]
      [--transport websocket|polling] [--poll-interval S]
      [--allow BUNDLE_ID ...] [--enable-launch] [--enable-quit]
      [--state-file PATH] [--headless] [--daemon] [--show-log]

Exit codes: 0 clean, 2 halted (close 4001/4003/1008 or HTTP 403), 1 other error.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.request
from collections import deque

from . import __version__, protocol
from .events import AppMonitor, Telemetry, resolve_boot_id
from .polling import PollingTransport
from .state import AgentState, DEFAULT_STATE_PATH
from .transport import WSTransport

LOG_RING_MAX = 500
EXPECTED_OFFLINE_CLOSE_AFTER_S = 60.0  # spec 5.3.2 default close_after_s


# --------------------------------------------------------------------------
# Logging: stderr with timestamps + in-memory ring (+ bounded file for
# --show-log across invocations).
# --------------------------------------------------------------------------

class Logger:
    def __init__(self, state_dir=None):
        self.ring = deque(maxlen=LOG_RING_MAX)
        self._path = None
        if state_dir:
            self._path = os.path.join(state_dir, "agent.log")

    def __call__(self, message):
        line = "%s %s" % (protocol.utcnow_iso(), message)
        self.ring.append(line)
        print(line, file=sys.stderr, flush=True)
        if self._path:
            try:
                with open(self._path, "a", encoding="utf-8") as fh:
                    fh.write(line + "\n")
            except OSError:
                pass

    def tail(self, n=LOG_RING_MAX):
        return list(self.ring)[-n:]


# --------------------------------------------------------------------------
# Runtime shared by transports, telemetry, and actions.
# --------------------------------------------------------------------------

class Runtime:
    def __init__(self, state, log):
        self.state = state
        self.log = log
        self.factory = protocol.EventFactory(state.ensure_instance_id())
        self.outbox = asyncio.Queue()
        self.stop_event = asyncio.Event()
        self.halt_reason = None
        self.expected_offline_until = None  # monotonic timestamp
        self.hello_params = {}
        self.boot_id = resolve_boot_id(state)
        self.started_monotonic = time.monotonic()
        self.app_monitor = AppMonitor(state.allowlist, log)
        self.telemetry = Telemetry(self)
        self.ui = None

    def enqueue(self, etype, payload, command_id=None):
        try:
            self.outbox.put_nowait(protocol.OutboxItem(etype, payload, command_id))
        except asyncio.QueueFull:
            self.log("outbox full; dropped event %s" % etype)

    def on_session_start(self, hello_ack):
        self.hello_params = dict(hello_ack)
        # Mandatory initial status burst within 2 s of hello_ack (spec 6.3).
        self.telemetry.initial_burst()
        # First heartbeat one interval after the burst frames (spec 4.2.2
        # cadence holds from session start, not from telemetry-loop start).
        self.telemetry.anchor_heartbeat_timer()
        if self.ui:
            self.ui.notify()

    def on_session_end(self):
        self.factory.end_session()
        self.hello_params = {}
        if self.ui:
            self.ui.notify()

    def declare_expected_offline(self, reason,
                                 close_after_s=EXPECTED_OFFLINE_CLOSE_AFTER_S):
        """agent_goodbye for sleep/restart/shutdown: hold reconnects (spec 8)."""
        self.log("declaring expected-offline (%s); holding %.0f s before reconnect"
                 % (reason, close_after_s))
        self.enqueue("agent_goodbye", {"reason": reason})
        self.expected_offline_until = time.monotonic() + close_after_s

    def request_stop(self):
        """SIGINT/SIGTERM: send agent_goodbye(agent_stop), close 1000 cleanly."""
        if not self.stop_event.is_set():
            self.log("stop requested; sending agent_goodbye(agent_stop)")
            self.enqueue("agent_goodbye", {"reason": "agent_stop"})
            self.stop_event.set()
            if self.ui:
                self.ui.notify()


# --------------------------------------------------------------------------
# Pairing
# --------------------------------------------------------------------------

def post_pair(hostname, code, agent_instance_id):
    """POST /agent/v1/pair. Returns (status, response_obj)."""
    body = json.dumps({
        "pairing_code": code,
        "agent_instance_id": agent_instance_id,
        "agent_version": protocol.AGENT_VERSION,
    }).encode("utf-8")
    req = urllib.request.Request(
        "http://%s:80/agent/v1/pair" % hostname,
        data=body, method="POST",
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, (json.loads(raw) if raw else {})
        except json.JSONDecodeError:
            return exc.code, {}
    except (OSError, asyncio.TimeoutError) as exc:
        return None, {"error": {"code": "network_error", "message": str(exc)}}


def do_pair(state, state_path, hostname, code, log):
    """Returns (ok, message). On success the state file is persisted."""
    if not hostname:
        return False, ("no endpoint hostname: pass --hostname or pair again "
                       "against a previously paired state file")
    state.ensure_instance_id()
    status, obj = post_pair(hostname, code, state.agent_instance_id)
    if status == 200 and isinstance(obj, dict) and obj.get("agent_token"):
        state.hostname = hostname
        state.agent_token = obj["agent_token"]
        state.agent_instance_id = obj.get("agent_instance_id",
                                          state.agent_instance_id)
        try:
            state.save()
        except OSError as exc:
            return False, "pairing succeeded but state save failed: %s" % exc
        log("paired with %s (pairing_id %s, instance %s)" % (
            hostname, obj.get("pairing_id"), state.agent_instance_id))
        return True, "paired"
    err = (obj.get("error") or {}) if isinstance(obj, dict) else {}
    if status == 409 and err.get("code") == "agent_not_paired":
        return False, "pairing window closed or absent on %s" % hostname
    if status == 429:
        return False, "rate limited; retry in a moment"
    if status == 400:
        return False, "validation failed: %s" % err.get("message", "bad request")
    if status is None:
        return False, "could not reach %s: %s" % (hostname, err.get("message"))
    return False, "pairing failed (HTTP %s): %s %s" % (
        status, err.get("code"), err.get("message", ""))


# --------------------------------------------------------------------------
# Serve loop
# --------------------------------------------------------------------------

async def serve(rt):
    from .power import SleepWatcher
    rt.sleep_watcher = SleepWatcher(rt, asyncio.get_event_loop())
    rt.sleep_watcher.start()
    telemetry_task = asyncio.ensure_future(rt.telemetry.run())
    try:
        if rt.state.transport == "polling":
            rc = await PollingTransport(rt).run()
        else:
            rc = await WSTransport(rt).run()
    finally:
        rt.sleep_watcher.stop()
        rt.stop_event.set()
        telemetry_task.cancel()
        await asyncio.gather(telemetry_task, return_exceptions=True)
    return rc


def run_serve(state, log, headless):
    rt = Runtime(state, log)
    if not headless:
        from .ui import start_ui
        rt.ui = start_ui(rt, lambda code: do_pair(
            state, state.path, state.hostname, code, log))

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.add_signal_handler(signal.SIGINT, rt.request_stop)
        loop.add_signal_handler(signal.SIGTERM, rt.request_stop)
    except (NotImplementedError, OSError, RuntimeError):
        pass
    try:
        rc = loop.run_until_complete(serve(rt))
    finally:
        loop.close()
    if rt.halt_reason:
        log("halted: %s (re-pair or reconfigure to resume)" % rt.halt_reason)
    return rc


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(prog="maccontrol_agent",
                                description="MacControlAgent (MCA) for macOS")
    p.add_argument("--hostname", help="endpoint hostname (bare label or FQDN)")
    p.add_argument("--pair-code", help="8-character pairing code from the ESP32 Web UI")
    p.add_argument("--transport", choices=("websocket", "polling"),
                   help="transport for the serve loop")
    p.add_argument("--poll-interval", type=int,
                   help="GET /agent/v1/commands/poll interval, seconds (2-30)")
    p.add_argument("--allow", nargs="+", metavar="BUNDLE_ID",
                   help="set the application allowlist (bundle IDs)")
    p.add_argument("--enable-launch", action="store_true",
                   help="enable the launch_app agent action")
    p.add_argument("--enable-quit", action="store_true",
                   help="enable the quit_app agent action")
    p.add_argument("--state-file", default=DEFAULT_STATE_PATH,
                   help="state file path (default: %(default)s)")
    p.add_argument("--headless", action="store_true",
                   help="no UI of any kind; requires prior pairing or --pair-code")
    p.add_argument("--daemon", action="store_true",
                   help="after pairing, keep serving instead of exiting")
    p.add_argument("--show-log", action="store_true",
                   help="print the recent log and exit")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    state_dir = os.path.dirname(os.path.abspath(args.state_file))
    log = Logger(state_dir)

    if args.show_log:
        log_path = os.path.join(state_dir, "agent.log")
        try:
            with open(log_path, "r", encoding="utf-8") as fh:
                lines = fh.readlines()[-LOG_RING_MAX:]
        except OSError:
            lines = []
        for line in lines:
            sys.stdout.write(line)
        return 0

    state = AgentState.load(args.state_file)
    state.ensure_instance_id()
    changed = False

    if args.transport:
        state.transport = args.transport
        changed = True
    if args.poll_interval is not None:
        state.poll_interval_s = max(2, min(30, args.poll_interval))
        changed = True
    if args.allow is not None:
        state.allowlist = list(args.allow)
        changed = True
    if args.enable_launch:
        state.enabled_commands["launch_app"] = True
        changed = True
    if args.enable_quit:
        state.enabled_commands["quit_app"] = True
        changed = True
    if changed or not os.path.exists(args.state_file):
        try:
            state.save()
        except OSError as exc:
            log("could not save state file: %s" % exc)
            return 1

    if args.pair_code:
        hostname = args.hostname or state.hostname
        ok, message = do_pair(state, args.state_file, hostname,
                              args.pair_code, log)
        if ok:
            print("PAIRED %s" % state.hostname)
        else:
            print("PAIR FAILED: %s" % message, file=sys.stderr)
            return 1
        if not args.daemon:
            return 0
        # --daemon: fall through into the serve loop
        state = AgentState.load(args.state_file)

    if not state.paired:
        if args.headless:
            print("unpaired: run with --pair-code CODE (and --hostname H "
                  "if never paired)", file=sys.stderr)
            return 1
        # Headful unpaired start: the UI handles code entry.
        log("unpaired; waiting for pairing code in the UI")

    try:
        return run_serve(state, log, headless=args.headless)
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        log("fatal: %r" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
