"""Sleep/wake detection via IOKit system power notifications (Phase 4.5 A5).

Daemon-grade mechanism, rewritten after the NSWorkspace observer failed its
first real sleep/wake test: distributed notifications never reached the
headless asyncio process (no NSRunLoop; endpoint log showed a bare
agent_offline, no sleeping/goodbye frames).

This version talks to IOKit directly over ctypes — no pyobjc, no
LaunchServices. IORegisterForSystemPower() gives us a Mach notification
port whose CFRunLoopSource runs on a dedicated thread's CFRunLoop; the
kernel delivers kIOMessageSystemWillSleep pre-sleep (must be acknowledged
with IOAllowPowerChange, or the sleep stalls on a 30 s timeout) and
kIOMessageSystemWillPowerOn at wake.

Message constants verified against the macOS SDK / xnu IOMessage.h
(2026-09-23): note 0xE0000270 is kIOMessageCanSystemSleep, NOT WillSleep —
older sample code gets this wrong.

Threading rule unchanged: the watcher thread never touches the asyncio
outbox or Runtime state directly — everything is bounced with
loop.call_soon_threadsafe. A belt-and-suspenders clock-divergence detector
lives in events.py (Telemetry.detect_sleep_from_clocks) in case a wake
frame is ever missed.
"""
from __future__ import annotations

import ctypes
import threading
import time

WAKE_FOLLOWUP_DELAY_S = 5.0
SLEEP_ACK_DEADLINE_S = 3.0  # bound the pre-ack wait; kernel timeout is 30 s

# IOKit.framework/IOMessage.h (iokit_common_msg, sys_iokit = 0xE0000000).
MSG_CAN_SYSTEM_SLEEP = 0xE0000270       # kIOMessageCanSystemSleep (ack only)
MSG_SYSTEM_WILL_SLEEP = 0xE0000280      # kIOMessageSystemWillSleep
MSG_SYSTEM_WILL_POWER_ON = 0xE0000320   # kIOMessageSystemWillPowerOn
MSG_SYSTEM_HAS_POWERED_ON = 0xE0000300  # kIOMessageSystemHasPoweredOn

# void (*)(void *refcon, io_service_t service, uint32_t messageType,
#          void *messageArgument)
_POWER_CALLBACK = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)

_IOKIT_PATH = "/System/Library/Frameworks/IOKit.framework/IOKit"
_CF_PATH = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"


def _load_frameworks():
    """Bind the IOKit/CoreFoundation symbols we need; raises if any missing."""
    iokit = ctypes.CDLL(_IOKIT_PATH)
    cf = ctypes.CDLL(_CF_PATH)

    iokit.IORegisterForSystemPower.restype = ctypes.c_uint32  # io_connect_t
    iokit.IORegisterForSystemPower.argtypes = [
        ctypes.c_void_p,                    # refcon
        ctypes.POINTER(ctypes.c_void_p),    # IONotificationPortRef *thePortRef
        _POWER_CALLBACK,                    # IOServiceInterestCallback
        ctypes.POINTER(ctypes.c_void_p),    # io_object_t *notifier
    ]
    iokit.IONotificationPortGetRunLoopSource.restype = ctypes.c_void_p
    iokit.IONotificationPortGetRunLoopSource.argtypes = [ctypes.c_void_p]
    iokit.IOAllowPowerChange.restype = ctypes.c_int  # IOReturn
    iokit.IOAllowPowerChange.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    iokit.IODeregisterForSystemPower.restype = ctypes.c_int
    iokit.IODeregisterForSystemPower.argtypes = [
        ctypes.POINTER(ctypes.c_void_p)]   # io_object_t *notifier

    cf.CFRunLoopGetCurrent.restype = ctypes.c_void_p
    cf.CFRunLoopGetCurrent.argtypes = []
    cf.CFRunLoopAddSource.argtypes = [ctypes.c_void_p] * 3
    cf.CFRunLoopRunInMode.restype = ctypes.c_int32
    cf.CFRunLoopRunInMode.argtypes = [ctypes.c_void_p, ctypes.c_double,
                                      ctypes.c_bool]
    return iokit, cf


class SleepWatcher:
    """Observes OS sleep/wake and emits system_state_changed deltas.

    will-sleep -> system_state_changed(sleeping) + declare_expected_offline(sleep)
    did-wake   -> system_state_changed(waking), then (awake) ~5 s later

    Interface is unchanged from the NSWorkspace version so __main__.py needs
    no edits: SleepWatcher(rt, loop), start(), stop().
    """

    def __init__(self, rt, loop):
        self.rt = rt
        self.loop = loop
        self.log = rt.log
        self._thread = None
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._wake_timer = None
        self._io = None            # (iokit, cf) once loaded
        self._callback = None      # keep the CFUNCTYPE alive
        self._root_connect = 0     # io_connect_t from IORegisterForSystemPower
        self._notify_port = ctypes.c_void_p()
        self._notifier = ctypes.c_void_p()

    # -- lifecycle ----------------------------------------------------------

    def start(self):
        """Never raises: a watcher failure must never kill the agent."""
        try:
            self._io = _load_frameworks()
        except Exception as exc:
            self.log("SleepWatcher: IOKit power notifications unavailable "
                     "(%s); sleep/wake detection disabled" % exc)
            return
        try:
            self._thread = threading.Thread(target=self._run,
                                            name="sleepwatcher", daemon=True)
            self._thread.start()
        except Exception as exc:
            self.log("SleepWatcher: failed to start: %s" % exc)

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None
        timer = self._wake_timer
        if timer is not None:
            timer.cancel()
            self._wake_timer = None

    # -- watcher thread ------------------------------------------------------

    def _run(self):
        registered = False
        try:
            iokit, cf = self._io
            self._callback = _POWER_CALLBACK(self._on_message)
            root = iokit.IORegisterForSystemPower(
                None,
                ctypes.byref(self._notify_port),
                self._callback,
                ctypes.byref(self._notifier))
            if root == 0:
                self.log("SleepWatcher: IORegisterForSystemPower failed; "
                         "sleep/wake detection disabled")
                return
            self._root_connect = root
            registered = True
            source = iokit.IONotificationPortGetRunLoopSource(
                self._notify_port)
            if not source:
                self.log("SleepWatcher: no run loop source for power "
                         "notifications; disabled")
                return
            runloop = cf.CFRunLoopGetCurrent()
            mode = ctypes.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode")
            cf.CFRunLoopAddSource(runloop, source, mode)
            self.log("SleepWatcher: IOKit system power notifications "
                     "registered")
            self._ready.set()
            while not self._stop.is_set():
                cf.CFRunLoopRunInMode(mode, 0.5, False)
        except Exception as exc:
            self.log("SleepWatcher: power-observer thread died: %s" % exc)
        finally:
            if registered:
                try:
                    self._io[0].IODeregisterForSystemPower(
                        ctypes.byref(self._notifier))
                except Exception:
                    pass

    # -- IOKit callback (watcher thread) --------------------------------------

    def _on_message(self, refcon, service, msg_type, msg_arg):
        if msg_type == MSG_SYSTEM_WILL_SLEEP:
            done = threading.Event()
            try:
                # Schedule BEFORE allowing the power change; the asyncio loop
                # runs concurrently on its own thread, so sleeping + goodbye
                # frames are queued (and transmitted) during the pre-sleep
                # window. The bounded wait makes "frames enqueued before
                # IOAllowPowerChange" an actual happens-before, not a race.
                self.loop.call_soon_threadsafe(self._handle_sleep, done)
            except RuntimeError:
                done.set()  # loop closed during shutdown; ack anyway
            done.wait(SLEEP_ACK_DEADLINE_S)
            if self._root_connect:
                self._io[0].IOAllowPowerChange(self._root_connect, msg_arg)
        elif msg_type == MSG_CAN_SYSTEM_SLEEP:
            # Permission request, not a decision: allow, no state event.
            if self._root_connect:
                self._io[0].IOAllowPowerChange(self._root_connect, msg_arg)
        elif msg_type in (MSG_SYSTEM_WILL_POWER_ON, MSG_SYSTEM_HAS_POWERED_ON):
            self.loop.call_soon_threadsafe(self._handle_wake)

    # -- handlers (asyncio loop thread) --------------------------------------

    def _handle_sleep(self, done=None):
        try:
            self.rt.enqueue("system_state_changed", {"state": "sleeping"})
            self.rt.declare_expected_offline("sleep")
        finally:
            if done is not None:
                done.set()

    def _handle_wake(self):
        # Timestamp coordinates with the clock-divergence detector in
        # Telemetry (episodes already reported here are not re-reported).
        self.rt.last_wake_reported_at = time.monotonic()
        self.rt.enqueue("system_state_changed", {"state": "waking"})
        if self._wake_timer is not None:
            self._wake_timer.cancel()
        timer = threading.Timer(WAKE_FOLLOWUP_DELAY_S, self._emit_awake)
        timer.daemon = True
        self._wake_timer = timer
        timer.start()

    def _emit_awake(self):
        self._wake_timer = None
        try:
            self.loop.call_soon_threadsafe(
                self.rt.enqueue, "system_state_changed", {"state": "awake"})
        except RuntimeError:
            pass  # loop closed during shutdown
