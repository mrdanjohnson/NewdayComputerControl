"""Minimal Phase 4 MCA UI (spec 9.2): pairing status, code entry, connection
status, and a local log view. Tkinter when importable, osascript fallback for
code entry. Fully headless operation is supported via --pair-code/--headless;
this module is simply not started then.

macOS constraint (hard-won, crash-reported on a CLT-Python client):
Tcl/Tk panics with abort() when Tk_Init runs off the main thread — a C-level
process kill that Python cannot catch. The agent's asyncio loop owns the main
thread and TkUI._run executes in a worker thread, so on macOS the Tk window
only ever appears if _run somehow runs on the main thread; anywhere else the
osascript fallback is used. Do NOT "try" Tk off-thread to see what happens.
"""
from __future__ import annotations

import queue
import subprocess
import threading
import time


def pair_code_via_osascript():
    """One-shot code entry dialog; returns the code or None."""
    script = (
        'set r to display dialog "Enter the 8-character MacControl pairing code" '
        'default answer "" with title "MacControl Agent Pairing"\n'
        'return text returned of r'
    )
    try:
        out = subprocess.run(["osascript", "-e", script],
                             capture_output=True, text=True, timeout=300)
        if out.returncode != 0:
            return None
        return out.stdout.strip() or None
    except (OSError, subprocess.TimeoutExpired):
        return None


class TkUI:
    """Small status window driven by a thread; communicates via thread-safe queue."""

    def __init__(self, rt, pair_callback):
        self.rt = rt
        self.pair_callback = pair_callback  # fn(code) -> (ok, message)
        self.updates = queue.Queue()
        self._root = None

    def start(self):
        threading.Thread(target=self._run, daemon=True,
                         name="maccontrol-ui").start()

    def notify(self):
        """Ask the UI thread to refresh from rt state."""
        self.updates.put(("refresh", None))

    # -- internals ----------------------------------------------------------

    def _mkstatus(self):
        rt = self.rt
        if rt.state.paired:
            pairing = "paired with %s (%s)" % (
                rt.state.hostname, rt.state.agent_instance_id)
        else:
            pairing = "unpaired"
        if rt.halt_reason:
            conn = "HALTED: %s" % rt.halt_reason
        elif rt.factory.session_id:
            conn = "connected (%s, %s)" % (rt.state.transport,
                                           rt.factory.session_id)
        else:
            conn = "connecting (%s)..." % rt.state.transport
        return pairing, conn

    def _run(self):
        import threading
        if threading.current_thread() is not threading.main_thread():
            # macOS Tk must init on the main thread; off-thread it panics and
            # abort()s the whole process (uncatchable). Fall back to the
            # osascript pairing dialog — prompt only when actually unpaired,
            # so a paired agent that keeps restarting (e.g. a halt loop)
            # never spams the dialog on every launchd respawn.
            if not self.rt.state.paired:
                code = pair_code_via_osascript()
                if code:
                    self.pair_callback(code)
            return
        try:
            import tkinter as tk
        except ImportError:
            if not self.rt.state.paired:
                code = pair_code_via_osascript()
                if code:
                    self.pair_callback(code)
            return
        root = tk.Tk()
        root.title("MacControl Agent")
        self._root = root
        root.geometry("420x260")

        pairing_var = tk.StringVar()
        conn_var = tk.StringVar()
        msg_var = tk.StringVar()
        code_var = tk.StringVar()

        tk.Label(root, text="Pairing").pack(anchor="w", padx=10, pady=(10, 0))
        tk.Label(root, textvariable=pairing_var).pack(anchor="w", padx=10)
        tk.Label(root, text="Connection").pack(anchor="w", padx=10, pady=(8, 0))
        tk.Label(root, textvariable=conn_var).pack(anchor="w", padx=10)

        row = tk.Frame(root)
        row.pack(fill="x", padx=10, pady=(12, 0))
        tk.Label(row, text="Pairing code:").pack(side="left")
        entry = tk.Entry(row, textvariable=code_var, width=12)
        entry.pack(side="left", padx=6)

        logbox = tk.Listbox(root, height=6)
        logbox.pack(fill="both", expand=True, padx=10, pady=8)

        def refresh():
            pairing, conn = self._mkstatus()
            pairing_var.set(pairing)
            conn_var.set(conn)
            logbox.delete(0, "end")
            for line in self.rt.log_tail(12):
                logbox.insert("end", line)

        def do_pair():
            code = code_var.get().strip()
            if not code:
                return
            ok, message = self.pair_callback(code)
            msg_var.set(message)
            if ok:
                code_var.set("")
            refresh()

        tk.Button(row, text="Pair", command=do_pair).pack(side="left")
        tk.Label(root, textvariable=msg_var, fg="red").pack(anchor="w", padx=10)
        tk.Button(root, text="Refresh", command=refresh).pack(anchor="e",
                                                              padx=10)

        def poll_queue():
            try:
                while True:
                    self.updates.get_nowait()
                    refresh()
            except queue.Empty:
                pass
            root.after(1000, poll_queue)

        refresh()
        poll_queue()
        root.mainloop()


def start_ui(rt, pair_callback):
    """Start the minimal UI in a background thread. Never raises.

    On macOS the thread is off the main thread, so the Tk window is skipped
    there (see module docstring) and this reduces to the osascript pairing
    dialog when unpaired.
    """
    try:
        ui = TkUI(rt, pair_callback)
        ui.start()
        return ui
    except Exception:
        return None


def prompt_code_headful_or_dialog():
    """Best-effort code prompt for interactive unpaired starts."""
    try:
        import tkinter  # noqa: F401
    except ImportError:
        return pair_code_via_osascript()
    # Tk UI handles code entry itself; nothing to do up front.
    return None
