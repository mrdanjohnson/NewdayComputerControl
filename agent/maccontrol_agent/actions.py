"""Inbound dispatch handling: launch_app / quit_app (spec 9.3.1, 6.2) and
the agent-executed power actions sleep/restart/shutdown (protocol v2).

Every dispatch is answered with command_ack within 2 s (the internal ack
action is always permitted), then gated by command enablement and the
application allowlist, then executed via the closed action set only.
"""
from __future__ import annotations

import asyncio
import subprocess

from . import protocol

ACTION_TIMEOUT_S = 10.0
# Bound the pre-power goodbye flush (the WS drain wait); the flush itself is
# best-effort — see _power for why a missed flush is still safe.
POWER_FLUSH_TIMEOUT_S = 5.0

# Power actions are executed in software: modern macOS ignores USB-HID
# system-sleep/power chords, so the endpoint routes them to the MCA in Mode B.
POWER_COMMANDS = {
    "sleep": ["pmset", "sleepnow"],
    "restart": ["osascript", "-e",
                'tell application "System Events" to restart'],
    "shutdown": ["osascript", "-e",
                 'tell application "System Events" to shut down'],
}


def _send_result(rt, command_id, outcome, error_code=None):
    rt.enqueue("command_result", {
        "command_id": command_id,
        "outcome": outcome,
        "error_code": error_code,
    }, command_id=command_id)


async def handle_dispatch(rt, dispatch: dict) -> None:
    """Handle one ESP32 dispatch {"action", "bundle_id"?, "command_id"}.

    Never raises: any failure path produces a correlated command_result.
    """
    command_id = dispatch.get("command_id")

    # command_ack within 2 s ALWAYS; the internal ack action is always
    # permitted even when the dispatch itself will be refused.
    if isinstance(command_id, str) and command_id:
        rt.enqueue("command_ack",
                   {"command_id": command_id, "action": dispatch.get("action")},
                   command_id=command_id)
    else:
        rt.log("dispatch without usable command_id; dropped: %r" % (dispatch,))
        return

    # Shape validation: unknown action or unexpected/missing keys ->
    # command_result(failed, app_not_allowlisted) + log, never execute.
    try:
        action, bundle_id = protocol.validate_dispatch_shape(dispatch)
    except ValueError as exc:
        rt.log("refusing malformed dispatch %r: %s" % (dispatch, exc))
        _send_result(rt, command_id, "failed", "app_not_allowlisted")
        return

    # Command enablement (fail-closed; spec 9.3.1).
    if not rt.state.enabled_commands.get(action, False):
        rt.log("dispatch %s refused: command disabled" % action)
        _send_result(rt, command_id, "failed", "command_disabled")
        return

    # Application allowlist (app actions only — power actions have no target).
    if bundle_id is not None and bundle_id not in rt.state.allowlist:
        rt.log("dispatch %s %s refused: app not allowlisted" % (action, bundle_id))
        _send_result(rt, command_id, "failed", "app_not_allowlisted")
        return

    if action in POWER_COMMANDS:
        await _power(rt, command_id, action)
    elif action == "launch_app":
        await _launch(rt, command_id, bundle_id)
    else:
        await _quit(rt, command_id, bundle_id)


async def _run(cmd, rt):
    """Run a subprocess with the action timeout; returns (rc, timed_out)."""
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, ValueError) as exc:
        rt.log("failed to spawn %r: %s" % (cmd, exc))
        return 127, False
    try:
        rc = await asyncio.wait_for(proc.wait(), timeout=ACTION_TIMEOUT_S)
        return rc, False
    except asyncio.TimeoutError:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        return -1, True


async def _launch(rt, command_id, bundle_id):
    rc, timed_out = await _run(["open", "-b", bundle_id], rt)
    if timed_out:
        rt.log("launch %s timed out" % bundle_id)
        _send_result(rt, command_id, "failed", "action_timeout")
    elif rc == 0:
        rt.log("launched %s" % bundle_id)
        # Launching an already-running app produces no Telemetry
        # application_started delta, which the engine needs to confirm the
        # command — if the probe already sees it, emit the evidence here.
        # Duplicates are safe: the engine's predicate is ack+app-event, and
        # a delta double against an already-confirmed record is idempotent.
        for _ in range(10):
            running = rt.app_monitor.probe()
            if bundle_id in running:
                rt.enqueue("application_started",
                           {"bundle_id": bundle_id,
                            "pid": running.get(bundle_id, 0)})
                break
            await asyncio.sleep(0.5)
        _send_result(rt, command_id, "ok", None)
    else:
        rt.log("launch %s failed (rc=%d)" % (bundle_id, rc))
        _send_result(rt, command_id, "failed", "launch_failed")


async def _quit(rt, command_id, bundle_id):
    if not rt.app_monitor.is_running(bundle_id):
        rt.log("quit %s refused: app not running" % bundle_id)
        _send_result(rt, command_id, "failed", "app_not_running")
        return
    # bundle_id was validated against ^[A-Za-z0-9.-]+$ before interpolation.
    script = 'tell application id "%s" to quit' % bundle_id
    rc, timed_out = await _run(["osascript", "-e", script], rt)
    if timed_out:
        rt.log("quit %s timed out" % bundle_id)
        _send_result(rt, command_id, "failed", "action_timeout")
    elif rc == 0:
        rt.log("quit %s requested" % bundle_id)
        rt.telemetry.mark_agent_quit(bundle_id)
        _send_result(rt, command_id, "ok", None)
    else:
        rt.log("quit %s failed (rc=%d)" % (bundle_id, rc))
        _send_result(rt, command_id, "failed", "quit_failed")


async def _power(rt, command_id, action):
    """Execute sleep/restart/shutdown: result + goodbye FIRST, then act.

    A power action is terminal for the connection (and possibly the whole
    process), so unlike app actions the correlated command_result(ok) is
    enqueued BEFORE acting: it reports the dispatch being honored, never the
    power transition itself — the endpoint record completes solely on
    evidence (the declared expected-offline goodbye and the offline window),
    and command_result(ok) MUST NOT complete it.

    The goodbye is flushed ahead of the power command: rt.flush_now is the
    outbox join installed by the active transport (WebSocket: every frame
    enqueued so far has left the socket; polling: a synchronous event-POST
    drain). Residual risk: restart/shutdown may outrun the flush — the host
    can halt before the peer reads the goodbye frame. That is acceptable:
    the endpoint's evidence channel also sees the TCP close, which qualifies
    the same expected-offline window, so the record completes from window
    evidence either way (the reason=restart/shutdown paths exist precisely
    so the window is deterministic when the flush DOES land).
    """
    _send_result(rt, command_id, "ok", None)
    rt.declare_expected_offline(action)
    if rt.flush_now is not None:
        try:
            await asyncio.wait_for(rt.flush_now(), timeout=POWER_FLUSH_TIMEOUT_S)
        except Exception as exc:
            rt.log("pre-%s flush incomplete (%s); proceeding anyway"
                   % (action, exc))
    rc, timed_out = await _run(POWER_COMMANDS[action], rt)
    if timed_out:
        # The host is (presumably) still up but the initiation wedged; no
        # further result is sent beyond the ok above — the endpoint's window
        # sweep terminates the record on evidence.
        rt.log("%s initiation timed out" % action)
        return
    if rc == 0:
        rt.log("%s initiated" % action)
        # mark_agent_quit-equivalent: apps that die with the host are labeled
        # requested_by_agent if a pre-power-off exit sweep observes them.
        rt.telemetry.mark_power_action()
    else:
        # The host stayed up and refused (e.g. auth/denied): the honor-failed
        # result supersedes the optimistic ok; the record terminates failed.
        rt.log("%s initiation failed (rc=%d)" % (action, rc))
        _send_result(rt, command_id, "failed", "action_timeout")
