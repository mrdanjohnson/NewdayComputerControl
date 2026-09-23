"""Telemetry producers: initial status burst, heartbeat, and macOS state monitors.

Best-effort by design (spec 9): with pyobjc available we use NSWorkspace for
application state and CoreGraphics for screen lock; without it we fall back to
sparse ``pgrep`` / ``mdfind`` polling. Sleep/wake OS notifications are a later
phase — the reported system state is always "awake" while the process runs.
"""
from __future__ import annotations

import getpass
import platform
import subprocess
import time

from . import protocol
from .state import AgentState

AGENT_REQUESTED_QUIT_WINDOW_S = 30.0

# --------------------------------------------------------------------------
# boot_id
# --------------------------------------------------------------------------

def get_boot_key():
    """kern.boottime in seconds since epoch, or None if unavailable."""
    try:
        out = subprocess.run(
            ["sysctl", "-n", "kern.boottime"],
            capture_output=True, text=True, timeout=5,
        ).stdout
        # '{ sec = 1700000000, usec = 0 }'
        for part in out.replace("{", "").replace("}", "").split(","):
            k, _, v = part.strip().partition("=")
            if k.strip() == "sec":
                return int(v.strip())
    except Exception:
        pass
    return None


def derive_boot_id(boot_key) -> str:
    """Format 'b_XXXXXX' from the boot time; random fallback handled by caller."""
    if boot_key:
        return "b_%06X" % (boot_key & 0xFFFFFF)
    import secrets
    return "b_%s" % secrets.token_hex(3).upper()


def resolve_boot_id(state: AgentState) -> str:
    """boot_id regenerated when the macOS boot time changes; persisted in state."""
    key = get_boot_key()
    if key is not None and state.boot_key == key and state.boot_id:
        return state.boot_id
    state.boot_key = key
    state.boot_id = derive_boot_id(key)
    try:
        state.save()
    except OSError:
        pass
    return state.boot_id


# --------------------------------------------------------------------------
# macOS state probes
# --------------------------------------------------------------------------

def get_macos_version() -> str:
    try:
        return platform.mac_ver()[0] or "unknown"
    except Exception:
        return "unknown"


def get_user():
    try:
        return getpass.getuser() or None
    except Exception:
        return None


def get_screen_locked():
    """True/False when detectable, None when unknown (no pyobjc).

    Uses the well-known CGSessionCopyCurrentDictionary snippet via pyobjc.
    """
    try:
        import Quartz  # pyobjc-framework-Quartz
        d = Quartz.CGSessionCopyCurrentDictionary()
        if not d:
            return None
        return bool(d.get("CGSSessionScreenIsLocked", 0))
    except Exception:
        return None


# --------------------------------------------------------------------------
# Application monitor
# --------------------------------------------------------------------------

class AppMonitor:
    """Tracks running state of allowlisted apps and emits start/exit deltas.

    Preferred source is NSWorkspace (pyobjc). Fallback: resolve each bundle
    ID to an app path via mdfind (cached) and pgrep -f the path. The fallback
    is best-effort and can miss apps; the gap is documented in README.
    """

    def __init__(self, allowlist, logger):
        self.allowlist = list(allowlist)
        self.log = logger
        self._use_nsworkspace = False
        self._path_cache = {}  # bundle_id -> app path or None
        try:
            import AppKit  # noqa: F401
            self._use_nsworkspace = True
        except Exception:
            self.log("AppMonitor: pyobjc not available; using pgrep fallback "
                     "(app state detection is best-effort)")

    def update_allowlist(self, allowlist):
        self.allowlist = list(allowlist)

    def _probe_nsworkspace(self):
        import AppKit
        running = {}
        for app in AppKit.NSWorkspace.sharedWorkspace().runningApplications():
            bid = app.bundleIdentifier()
            if bid in self.allowlist:
                try:
                    running[bid] = int(app.processIdentifier())
                except Exception:
                    running[bid] = 0
        return running

    def _resolve_path(self, bundle_id):
        if bundle_id in self._path_cache:
            return self._path_cache[bundle_id]
        path = None
        try:
            out = subprocess.run(
                ["mdfind", "kMDItemCFBundleIdentifier == '%s'" % bundle_id],
                capture_output=True, text=True, timeout=5,
            ).stdout
            for line in out.splitlines():
                line = line.strip()
                if line.endswith(".app"):
                    path = line
                    break
        except Exception:
            pass
        self._path_cache[bundle_id] = path
        return path

    def _probe_pgrep(self):
        running = {}
        for bid in self.allowlist:
            path = self._resolve_path(bid)
            if not path:
                continue
            try:
                out = subprocess.run(
                    ["pgrep", "-f", path],
                    capture_output=True, text=True, timeout=5,
                ).stdout.strip()
            except Exception:
                continue
            if out:
                try:
                    running[bid] = int(out.splitlines()[0])
                except (ValueError, IndexError):
                    running[bid] = 0
        return running

    def probe(self):
        """Return {bundle_id: pid or 0} for allowlisted apps currently running."""
        if self._use_nsworkspace:
            try:
                return self._probe_nsworkspace()
            except Exception as exc:
                self.log("AppMonitor: NSWorkspace probe failed: %s" % exc)
        return self._probe_pgrep()

    def is_running(self, bundle_id) -> bool:
        return bundle_id in self.probe()


def diff_app_states(previous: dict, current: dict):
    """Yield ('started', bid, pid) / ('exited', bid, pid) deltas between probes."""
    for bid, pid in current.items():
        if bid not in previous:
            yield ("started", bid, pid)
    for bid, pid in previous.items():
        if bid not in current:
            yield ("exited", bid, pid)


# --------------------------------------------------------------------------
# Async telemetry loop
# --------------------------------------------------------------------------

class Telemetry:
    """Runs inside the serve loop: heartbeat timer + state monitors.

    Events are pushed into rt.enqueue(); the active transport serializes them
    (assigning seq at send time, in order).
    """

    def __init__(self, rt, monitor_interval=3.0):
        self.rt = rt
        self.monitor_interval = monitor_interval
        self._last_hb = 0.0
        self._apps = {}
        self._locked = None
        self._user = get_user()
        self._quit_marks = {}  # bundle_id -> monotonic time of agent-requested quit

    # -- producers ---------------------------------------------------------

    def emit_heartbeat_if_due(self, now=None):
        now = now if now is not None else time.monotonic()
        interval = float(self.rt.hello_params.get("heartbeat_interval_s", 5))
        if now - self._last_hb >= interval:
            self._last_hb = now
            self.rt.enqueue("heartbeat", {
                "boot_id": self.rt.boot_id,
                "uptime_s": int(now - self.rt.started_monotonic),
            })
            return True
        return False

    def anchor_heartbeat_timer(self):
        """Call after a frame burst (session start): the first heartbeat is
        due one interval after those frames, not after the telemetry loop
        happens to start — otherwise a freshly connected agent sits silent
        for burst-latency + interval and reads as prematurely stale."""
        self._last_hb = time.monotonic()

    def emit_app_deltas(self):
        current = self.rt.app_monitor.probe()
        now = time.monotonic()
        for kind, bid, pid in diff_app_states(self._apps, current):
            if kind == "started":
                self.rt.enqueue("application_started",
                                {"bundle_id": bid, "pid": pid})
            else:
                marked = self._quit_marks.pop(bid, None)
                reason = ("requested_by_agent"
                          if marked and now - marked < AGENT_REQUESTED_QUIT_WINDOW_S
                          else "quit")
                self.rt.enqueue("application_exited",
                                {"bundle_id": bid, "pid": pid, "reason": reason})
        self._apps = current

    def emit_lock_user_deltas(self):
        locked = get_screen_locked()
        if locked is not None and locked != self._locked:
            self._locked = locked
            self.rt.enqueue("screen_lock_changed", {"locked": locked})
        user = get_user()
        if user != self._user:
            prev, self._user = self._user, user
            self.rt.enqueue("user_session_changed", {
                "user_logged_in": user is not None,
                "user": user,
            })

    def mark_agent_quit(self, bundle_id):
        self._quit_marks[bundle_id] = time.monotonic()

    def running_apps(self):
        if not self._apps:
            self._apps = self.rt.app_monitor.probe()
        return dict(self._apps)

    # -- burst --------------------------------------------------------------

    def initial_burst(self):
        """Mandatory initial status burst within 2 s of hello_ack (spec 6.3)."""
        rt = self.rt
        rt.enqueue("system_state_changed", {"state": "awake"})
        user = get_user()
        rt.enqueue("user_session_changed", {
            "user_logged_in": user is not None,
            "user": user,
        })
        locked = get_screen_locked()
        if locked is not None:
            self._locked = locked
            rt.enqueue("screen_lock_changed", {"locked": locked})
        else:
            rt.log("screen lock state undetectable without pyobjc; "
                   "screen_lock_changed omitted from burst")
        running = self.running_apps()
        rt.enqueue("capability_report", {
            "agent_version": protocol.AGENT_VERSION,
            "protocol_version": protocol.PROTOCOL_VERSION,
            "os_version": get_macos_version(),
            "enabled_commands": sorted(
                a for a, on in rt.state.enabled_commands.items() if on),
            "allowlisted_apps": [
                {"bundle_id": bid,
                 "state": "running" if bid in running else "not_running"}
                for bid in rt.state.allowlist
            ],
        })
        for bid, pid in running.items():
            rt.enqueue("application_started", {"bundle_id": bid, "pid": pid})

    # -- main loop ----------------------------------------------------------

    async def run(self):
        import asyncio
        if not self._last_hb:
            self._last_hb = time.monotonic()
        while not self.rt.stop_event.is_set():
            try:
                self.emit_heartbeat_if_due()
                self.emit_app_deltas()
                self.emit_lock_user_deltas()
            except Exception as exc:  # telemetry must never kill the agent
                self.rt.log("telemetry error: %s" % exc)
            try:
                await asyncio.wait_for(self.rt.stop_event.wait(),
                                       timeout=self.monitor_interval)
            except asyncio.TimeoutError:
                pass
