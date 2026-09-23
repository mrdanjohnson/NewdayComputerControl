"""Inbound dispatch handling: launch_app / quit_app (spec 9.3.1, 6.2).

Every dispatch is answered with command_ack within 2 s (the internal ack
action is always permitted), then gated by command enablement and the
application allowlist, then executed via the closed action set only.
"""
from __future__ import annotations

import asyncio
import subprocess

from . import protocol

ACTION_TIMEOUT_S = 10.0


def _send_result(rt, command_id, outcome, error_code=None):
    rt.enqueue("command_result", {
        "command_id": command_id,
        "outcome": outcome,
        "error_code": error_code,
    }, command_id=command_id)


async def handle_dispatch(rt, dispatch: dict) -> None:
    """Handle one ESP32 dispatch {"action", "bundle_id", "command_id"}.

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
        rt.log("dispatch %s %s refused: command disabled" % (action, bundle_id))
        _send_result(rt, command_id, "failed", "command_disabled")
        return

    # Application allowlist.
    if bundle_id not in rt.state.allowlist:
        rt.log("dispatch %s %s refused: app not allowlisted" % (action, bundle_id))
        _send_result(rt, command_id, "failed", "app_not_allowlisted")
        return

    if action == "launch_app":
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
