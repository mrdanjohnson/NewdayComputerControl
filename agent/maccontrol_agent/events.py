"""Telemetry producers: initial status burst, heartbeat, and macOS state monitors.

Best-effort by design (spec 9): with pyobjc available we use NSWorkspace for
application state and CoreGraphics for screen lock; without it we fall back to
sparse ``pgrep`` / ``mdfind`` polling. Sleep/wake OS notifications live in
power.py (Phase 4.5 A5); system load samples for the composite heartbeat use
psutil with subprocess fallbacks, all best-effort-null per spec 9.
"""
from __future__ import annotations

import getpass
import platform
import subprocess
import time

from . import protocol
from .power import WAKE_FOLLOWUP_DELAY_S
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
# System load samplers (spec 6.3.1 composite report, sampled at heartbeat
# emission time only). psutil when importable, subprocess fallbacks, null on
# any failure — robust-and-null over clever-and-brittle (spec 9).
# --------------------------------------------------------------------------

def _cmd_output(argv, timeout=5):
    try:
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=timeout).stdout
    except Exception:
        return ""


class SystemSampler:
    """Samples cpu/memory/disk/network for the extended heartbeat payload.

    The cpu counter is primed once at construction so the first heartbeat
    carries a real delta rather than the 0.0 psutil returns on first call.
    """

    def __init__(self):
        self._psutil = None
        try:
            import psutil
            self._psutil = psutil
            psutil.cpu_percent(interval=None)  # prime the delta counter
        except Exception:
            pass

    def cpu_percent(self):
        """0-100 utilization since last call, or None if undeterminable."""
        if self._psutil:
            try:
                return round(float(self._psutil.cpu_percent(interval=None)), 1)
            except Exception:
                return None
        # Fallback: second sample of `top -l 2` (first sample is all-zero).
        out = _cmd_output(["top", "-l", "2", "-n", "0"], timeout=10)
        for line in reversed(out.splitlines()):
            if "CPU usage" not in line:
                continue
            try:
                user = float(line.split("user")[0].split(":")[1].strip().rstrip("%"))
                sys_part = line.split("sys")[0].split(",")[-1].strip().rstrip("%")
                return round(user + float(sys_part), 1)
            except Exception:
                return None
        return None

    def memory_percent(self):
        """0-100 physical memory utilization, or None."""
        if self._psutil:
            try:
                return round(float(self._psutil.virtual_memory().percent), 1)
            except Exception:
                return None
        # Fallback: vm_stat pages (active+wired+compressed) / hw.memsize.
        out = _cmd_output(["vm_stat"])
        page_size = None
        pages = {}
        for line in out.splitlines():
            line = line.strip()
            if "page size of" in line:
                try:
                    page_size = int(line.split("page size of")[1].split()[0])
                except (IndexError, ValueError):
                    return None
                continue
            name, sep, rest = line.partition(":")
            if not sep:
                continue
            try:
                pages[name.strip()] = int(rest.strip().rstrip("."))
            except ValueError:
                continue
        if not page_size:
            return None
        used = sum(pages.get(k, 0) for k in
                   ("Pages active", "Pages wired down",
                    "Pages occupied by compressor"))
        if not used:
            return None
        try:
            memsize = int(_cmd_output(["sysctl", "-n", "hw.memsize"]).strip())
            return round(used * page_size * 100.0 / memsize, 1)
        except Exception:
            return None

    def disk_free_bytes(self):
        """Free bytes on the root volume, or None."""
        if self._psutil:
            try:
                return int(self._psutil.disk_usage("/").free)
            except Exception:
                return None
        # Fallback: `df -k /` last line, available-blocks column x1024.
        out = _cmd_output(["df", "-k", "/"])
        lines = [l for l in out.splitlines() if l.strip()]
        if len(lines) < 2:
            return None
        fields = lines[-1].split()
        if len(fields) < 4:
            return None
        try:
            return int(fields[3]) * 1024
        except ValueError:
            return None

    def network(self):
        """{reachable, ip} from the routing table only — NO outbound traffic."""
        reachable = False
        ip = None
        try:
            p = subprocess.run(["route", "-n", "get", "default"],
                               capture_output=True, text=True, timeout=5)
            reachable = p.returncode == 0
            if reachable:
                iface = None
                for line in p.stdout.splitlines():
                    key, _, val = line.strip().partition(":")
                    if key.strip() == "interface":
                        iface = val.strip()
                        break
                if iface:
                    addr = _cmd_output(["ipconfig", "getifaddr", iface]).strip()
                    ip = addr or None
        except Exception:
            pass
        return {"reachable": reachable, "ip": ip}


# --------------------------------------------------------------------------
# hardware model (capability_report; static, probed once per process)
# --------------------------------------------------------------------------

_hardware_model = None
_hardware_model_probed = False

def get_hardware_model():
    """hw.model string, probed once per process; None if unavailable."""
    global _hardware_model, _hardware_model_probed
    if not _hardware_model_probed:
        _hardware_model_probed = True
        out = _cmd_output(["sysctl", "-n", "hw.model"]).strip()
        _hardware_model = out or None
    return _hardware_model


# --------------------------------------------------------------------------
# Application monitor
# --------------------------------------------------------------------------

class AppMonitor:
    """Tracks running state of allowlisted apps and emits start/exit deltas.

    Preferred source is psutil: each bundle ID is resolved to an .app path
    via mdfind (cached) and processes are matched by exe-path prefix from a
    single process_iter scan per probe. Fallback without psutil: pgrep -f
    the app path (best-effort, can miss apps; documented in README).

    NSWorkspace is deliberately NOT used here: in a headless asyncio process
    with no NSRunLoop its runningApplications() snapshot freezes at the
    first query and apps launched later never appear (bench-verified,
    AT-11 / docs/DEBUG-PHASE4-AT11.md).
    """

    def __init__(self, allowlist, logger):
        self.allowlist = list(allowlist)
        self.log = logger
        self._psutil = None
        self._path_cache = {}  # bundle_id -> app path or None
        try:
            import psutil
            self._psutil = psutil
        except Exception:
            self.log("AppMonitor: psutil not available; using pgrep fallback "
                     "(app state detection is best-effort)")

    def update_allowlist(self, allowlist):
        self.allowlist = list(allowlist)

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

    def _probe_psutil(self):
        running = {}
        exes = []  # (exe path, pid), one process_iter scan for all bundles
        try:
            for proc in self._psutil.process_iter(["exe"]):
                try:
                    info = proc.info
                    if info.get("exe"):
                        exes.append((info["exe"], info.get("pid") or proc.pid))
                except Exception:
                    continue
        except Exception as exc:
            self.log("AppMonitor: process scan failed: %s" % exc)
            return None
        for bid in self.allowlist:
            path = self._resolve_path(bid)
            if not path:
                continue
            for exe, pid in exes:
                if exe.startswith(path):
                    running[bid] = int(pid)
                    break
        return running

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
        if self._psutil:
            running = self._probe_psutil()
            if running is not None:
                return running
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
        self._sampler = SystemSampler()
        self._boot_time = get_boot_key()  # epoch sec of Mac boot (kern.boottime)
        self._front_app = None
        self._front_app_seen = False
        self._clock_prev = None    # (mac_uptime_s, proc_uptime_s) last tick
        self._loop = None          # asyncio loop, captured in run()
        self._awake_handle = None  # pending delayed "awake" call_later
        try:
            import AppKit  # noqa: F401
            self._has_nsworkspace = True
        except Exception:
            self._has_nsworkspace = False

    # -- sleep/wake safety net ----------------------------------------------

    def detect_sleep_from_clocks(self):
        """Clock-divergence wake detector (belt-and-suspenders, Phase 4.5).

        Mac uptime (kern.boottime-derived) counts time spent asleep; the
        process monotonic clock does not. A >10 s jump in the per-tick
        delta-of-deltas means the Mac slept and woke without the SleepWatcher
        reporting it (e.g. its wake frame was lost). Deliberately does NOT
        fabricate a retroactive sleeping/goodbye — pre-sleep declaration is
        only possible from the IOKit WillSleep path.
        """
        if not self._boot_time:
            return
        mac_now = int(time.time()) - self._boot_time
        proc_now = int(time.monotonic() - self.rt.started_monotonic)
        prev = self._clock_prev
        self._clock_prev = (mac_now, proc_now)
        if prev is None:
            return
        drift = (mac_now - prev[0]) - (proc_now - prev[1])
        if drift <= 10:
            return
        last = getattr(self.rt, "last_wake_reported_at", 0.0)
        if time.monotonic() - last < 30:
            return  # SleepWatcher already reported this episode
        self.rt.last_wake_reported_at = time.monotonic()
        self.rt.log("detected sleep/wake from clock divergence (%ds); "
                    "SleepWatcher did not report it" % drift)
        self.rt.enqueue("system_state_changed", {"state": "waking"})
        self._schedule_awake()

    def _schedule_awake(self):
        """Single outstanding delayed 'awake', 5 s after a waking event."""
        if self._awake_handle is not None:
            self._awake_handle.cancel()
        if self._loop is not None:
            self._awake_handle = self._loop.call_later(
                WAKE_FOLLOWUP_DELAY_S, self._emit_awake)

    def _emit_awake(self):
        self._awake_handle = None
        self.rt.enqueue("system_state_changed", {"state": "awake"})

    # -- producers ---------------------------------------------------------

    def emit_heartbeat_if_due(self, now=None):
        now = now if now is not None else time.monotonic()
        interval = float(self.rt.hello_params.get("heartbeat_interval_s", 5))
        if now - self._last_hb >= interval:
            self._last_hb = now
            boot_time = self._boot_time
            self.rt.enqueue("heartbeat", {
                "boot_id": self.rt.boot_id,
                "uptime_s": int(now - self.rt.started_monotonic),
                "boot_time": boot_time,
                "mac_uptime_s": int(time.time() - boot_time) if boot_time else None,
                "cpu_utilization_pct": self._sampler.cpu_percent(),
                "memory_utilization_pct": self._sampler.memory_percent(),
                "disk_free_bytes": self._sampler.disk_free_bytes(),
                "network": self._sampler.network(),
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

    def emit_front_app_delta(self):
        """front_app_changed on first detection and on change (Phase 4.5 B1).

        NSWorkspace only (no meaningful fallback); silently skipped without
        pyobjc. Deltas only — never part of the initial burst.
        """
        if not self._has_nsworkspace:
            return
        try:
            import AppKit
            app = AppKit.NSWorkspace.sharedWorkspace().frontmostApplication()
            bid = app.bundleIdentifier() if app is not None else None
        except Exception as exc:
            self.rt.log("frontmost app probe failed: %s" % exc)
            return
        if not self._front_app_seen or bid != self._front_app:
            self._front_app = bid
            self._front_app_seen = True
            self.rt.enqueue("front_app_changed", {"bundle_id": bid})

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
            "hardware_model": get_hardware_model(),
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
        self._loop = asyncio.get_event_loop()
        if not self._last_hb:
            self._last_hb = time.monotonic()
        while not self.rt.stop_event.is_set():
            try:
                self.emit_heartbeat_if_due()
                self.emit_app_deltas()
                self.emit_lock_user_deltas()
                self.emit_front_app_delta()
                self.detect_sleep_from_clocks()
            except Exception as exc:  # telemetry must never kill the agent
                self.rt.log("telemetry error: %s" % exc)
            try:
                await asyncio.wait_for(self.rt.stop_event.wait(),
                                       timeout=self.monitor_interval)
            except asyncio.TimeoutError:
                pass
