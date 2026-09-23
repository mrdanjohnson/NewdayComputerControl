"""Sleep/wake detection via NSWorkspace notifications (Phase 4.5 A5).

Runs a dedicated NSThread with its own NSRunLoop observing
NSWorkspaceWillSleepNotification / NSWorkspaceDidWakeNotification, and
bounces every effect onto the agent's asyncio loop via
loop.call_soon_threadsafe — the watcher thread never touches the outbox or
Runtime state directly. Without pyobjc the watcher logs one line at startup
and becomes a no-op: behavior stays as before this phase (always "awake").
"""
from __future__ import annotations

import threading

WAKE_FOLLOWUP_DELAY_S = 5.0


def _make_delegate():
    """Build the NSObject observer subclass (needs AppKit at call time)."""
    import AppKit
    import objc

    class SleepDelegate(AppKit.NSObject):
        def initWithWatcher_(self, watcher):
            self = objc.super(SleepDelegate, self).init()
            self._watcher = watcher
            return self

        def workspaceWillSleep_(self, notification):
            self._watcher.on_will_sleep()

        def workspaceDidWake_(self, notification):
            self._watcher.on_did_wake()

    return SleepDelegate


class SleepWatcher:
    """Observes OS sleep/wake and emits system_state_changed deltas.

    will-sleep -> system_state_changed(sleeping) + declare_expected_offline(sleep)
    did-wake   -> system_state_changed(waking), then (awake) ~5 s later
    """

    def __init__(self, rt, loop):
        self.rt = rt
        self.loop = loop
        self.log = rt.log
        self._thread = None
        self._stop = threading.Event()
        self._wake_timer = None
        try:
            import AppKit  # noqa: F401
            self._available = True
        except Exception:
            self._available = False

    # -- lifecycle ----------------------------------------------------------

    def start(self):
        """Never raises: a watcher failure must never kill the agent."""
        if not self._available:
            self.log("SleepWatcher: pyobjc not available; sleep/wake detection "
                     "disabled (system state reports as awake only)")
            return
        try:
            self._thread = threading.Thread(target=self._run,
                                            name="sleepwatcher", daemon=True)
            self._thread.start()
        except Exception as exc:
            self.log("SleepWatcher: failed to start: %s" % exc)

    def stop(self):
        self._stop.set()
        timer = self._wake_timer
        if timer is not None:
            timer.cancel()
            self._wake_timer = None

    # -- watcher thread ------------------------------------------------------

    def _run(self):
        try:
            import AppKit
            from Foundation import NSDate, NSDefaultRunLoopMode, NSRunLoop
            delegate_class = _make_delegate()
            self._delegate = delegate_class.alloc().initWithWatcher_(self)
            nc = AppKit.NSWorkspace.sharedWorkspace().notificationCenter()
            nc.addObserver_selector_name_object_(
                self._delegate, "workspaceWillSleep:",
                AppKit.NSWorkspaceWillSleepNotification, None)
            nc.addObserver_selector_name_object_(
                self._delegate, "workspaceDidWake:",
                AppKit.NSWorkspaceDidWakeNotification, None)
            runloop = NSRunLoop.currentRunLoop()
            while not self._stop.is_set():
                runloop.runMode_beforeDate_(
                    NSDefaultRunLoopMode, NSDate.dateWithTimeIntervalSinceNow_(0.5))
        except Exception as exc:
            self.log("SleepWatcher: observer thread died: %s" % exc)

    # -- delegate callbacks (watcher thread) --------------------------------

    def on_will_sleep(self):
        self.loop.call_soon_threadsafe(self._handle_sleep)

    def on_did_wake(self):
        self.loop.call_soon_threadsafe(self._handle_wake)

    # -- handlers (asyncio loop thread) --------------------------------------

    def _handle_sleep(self):
        self.rt.enqueue("system_state_changed", {"state": "sleeping"})
        self.rt.declare_expected_offline("sleep")

    def _handle_wake(self):
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
