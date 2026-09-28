"""Polling fallback transport (spec 4.3.1).

POST /agent/v1/events carries the identical Chapter 6 envelope; the first
agent_hello returns {"ok": true, "session_id": ...} and every subsequent
request carries X-Session-Id. GET /agent/v1/commands/pending returns
{"commands": [...]} — the server-push replacement.

HTTP error mapping (spec 4.3.1):
  401 unauthorized            -> failed attempt (backoff)
  403 forbidden               -> HALTED (revoked/disabled credential; terminal)
  409 agent_not_paired        -> failed attempt (backoff)
  409 agent_offline           -> open a NEW session (seq resets)
  400 validation_failed       -> failed attempt (backoff), logged loudly
A transport switch requires a new session (handled by the serve loop).
"""
from __future__ import annotations

import asyncio
import json
import time
import urllib.error
import urllib.request

from . import protocol
from .actions import handle_dispatch
from .protocol import OutboxItem

HTTP_TIMEOUT_S = 10.0


class PollFailure(Exception):
    """(action, detail) — action in {'retry', 'halt', 'new_session'}."""

    def __init__(self, action, detail):
        super().__init__(detail)
        self.action = action
        self.detail = detail


def _request(method, url, token, session_id, body=None, timeout=HTTP_TIMEOUT_S):
    """Synchronous JSON request via stdlib urllib; returns (status, obj)."""
    data = None
    headers = {
        "Authorization": "Bearer %s" % token,
        "Accept": "application/json",
    }
    if body is not None:
        data = json.dumps(body, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if session_id:
        headers["X-Session-Id"] = session_id
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            obj = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            obj = {}
        return exc.code, obj


def _map_http_error(status, obj):
    code = (obj.get("error") or {}).get("code") if isinstance(obj, dict) else None
    if status == 403:
        return PollFailure("halt", "forbidden (credential revoked/disabled): %s" % code)
    if status == 409 and code == "agent_offline":
        return PollFailure("new_session", "session inactive/unknown (409 agent_offline)")
    detail = "HTTP %s (%s)" % (status, code or "no code")
    if status == 401:
        detail = "unauthorized: %s" % code
    elif status == 409:
        detail = "agent_not_paired: %s" % code
    elif status == 400:
        detail = "validation_failed: %s" % code
    return PollFailure("retry", detail)


class PollingTransport:
    def __init__(self, rt):
        self.rt = rt
        self.hostname = rt.state.hostname
        self.base = "http://%s:80/agent/v1" % self.hostname
        self._last_hb_sent = 0.0

    # -- helpers ------------------------------------------------------------

    async def _http(self, method, path, session_id=None, body=None):
        rt = self.rt
        return await asyncio.to_thread(
            _request, method, self.base + path, rt.state.agent_token,
            session_id, body)

    async def _post_event(self, session_id, etype, payload, command_id=None):
        envelope = self.rt.factory.next_event(etype, payload, command_id)
        status, obj = await self._http("POST", "/events", session_id, envelope)
        if status != 200:
            raise _map_http_error(status, obj)
        return obj

    async def _drain_outbox(self, session_id):
        rt = self.rt
        while True:
            try:
                item = rt.outbox.get_nowait()
            except asyncio.QueueEmpty:
                return
            await self._post_event(session_id, item.etype, item.payload,
                                   item.command_id)

    async def _poll_pending(self, session_id):
        rt = self.rt
        status, obj = await self._http("GET", "/commands/pending", session_id)
        if status != 200:
            raise _map_http_error(status, obj)
        commands = obj.get("commands", []) if isinstance(obj, dict) else []
        for dispatch in commands:
            try:
                dispatch = protocol.parse_dispatch(dispatch)
            except (protocol.MalformedDispatch, ValueError) as exc:
                rt.log("ignoring malformed pending command: %s" % exc)
                continue
            await handle_dispatch(rt, dispatch)
        if commands:
            rt.log("polled %d pending command(s)" % len(commands))
        # acks/results from this batch go out immediately (within 2 s)
        await self._drain_outbox(session_id)

    # -- session ------------------------------------------------------------

    async def _hello(self):
        rt = self.rt
        rt.factory.end_session()
        hello = rt.factory.next_event(
            "agent_hello", protocol.hello_payload(rt.boot_id, self.hostname))
        status, obj = await self._http("POST", "/events", None, hello)
        if status != 200:
            raise _map_http_error(status, obj)
        session_id = obj.get("session_id") if isinstance(obj, dict) else None
        if not session_id:
            raise PollFailure("retry", "hello response missing session_id")
        rt.factory.begin_session(session_id)
        return session_id, obj

    async def _session_loop(self, session_id, hello_resp):
        rt = self.rt
        rt.on_session_start(hello_resp)
        rt.log("polling session %s active (interval %ss)" % (
            session_id, rt.state.poll_interval_s))
        interval = max(2.0, min(30.0, float(rt.state.poll_interval_s)))
        next_poll = 0.0
        # Synchronous event-POST drain: when it returns, every frame enqueued
        # so far is transmitted (actions._power awaits this before acting).
        rt.flush_now = lambda: self._drain_outbox(session_id)
        try:
            while not rt.stop_event.is_set():
                now = time.monotonic()
                try:
                    if now >= next_poll:
                        await self._drain_outbox(session_id)
                        rt.telemetry.emit_heartbeat_if_due()
                        await self._drain_outbox(session_id)
                        await self._poll_pending(session_id)
                        next_poll = time.monotonic() + interval
                    else:
                        await self._drain_outbox(session_id)
                except PollFailure as pf:
                    raise pf
                except (OSError, asyncio.TimeoutError) as exc:
                    raise PollFailure("retry", "poll cycle network error: %s" % exc)
                try:
                    await asyncio.wait_for(rt.stop_event.wait(),
                                           timeout=max(0.1, next_poll - time.monotonic()))
                except asyncio.TimeoutError:
                    pass
        finally:
            rt.flush_now = None
        # graceful stop: flush anything left (e.g. agent_goodbye)
        try:
            while not rt.outbox.empty():
                await self._post_event(session_id, *rt.outbox.get_nowait())
        except PollFailure as pf:
            rt.log("final flush failed: %s" % pf.detail)
        return 0

    # -- main loop ----------------------------------------------------------

    async def run(self):
        rt = self.rt
        attempt = 0
        while True:
            if rt.halt_reason:
                return 2
            if rt.stop_event.is_set():
                return 0
            if not await self._respect_offline_window():
                return 0
            try:
                session_id, hello_resp = await self._hello()
                attempt = 0  # reset only on a successful session hello
            except PollFailure as pf:
                if pf.action == "halt":
                    rt.halt_reason = pf.detail
                    rt.log("HALTED: %s" % pf.detail)
                    return 2
                rt.log("session open failed: %s" % pf.detail)
                attempt += 1
                if not await self._backoff(attempt):
                    return 0
                continue
            except (OSError, asyncio.TimeoutError) as exc:
                rt.log("session open failed: %s" % exc)
                attempt += 1
                if not await self._backoff(attempt):
                    return 0
                continue

            try:
                return await self._session_loop(session_id, hello_resp)
            except PollFailure as pf:
                rt.on_session_end()
                if rt.stop_event.is_set():
                    return 0
                if pf.action == "halt":
                    rt.halt_reason = pf.detail
                    rt.log("HALTED: %s" % pf.detail)
                    return 2
                if pf.action == "new_session":
                    rt.log("session lost (%s); opening a new session" % pf.detail)
                    attempt += 1  # still a failed cycle per spec 4.3.2
                else:
                    rt.log("poll cycle failed: %s" % pf.detail)
                    attempt += 1
                if not await self._backoff(attempt):
                    return 0

    async def _respect_offline_window(self):
        rt = self.rt
        while not rt.stop_event.is_set():
            until = rt.expected_offline_until
            if until is None:
                return True
            remaining = until - time.monotonic()
            if remaining <= 0:
                rt.expected_offline_until = None
                return True
            rt.log("expected-offline window: holding polling for %.1f s" % remaining)
            try:
                await asyncio.wait_for(rt.stop_event.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                pass
        return False

    async def _backoff(self, attempt):
        delay = protocol.backoff_delay(attempt)
        self.rt.log("polling retry attempt %d in %.1f s" % (attempt, delay))
        try:
            await asyncio.wait_for(self.rt.stop_event.wait(), timeout=delay)
            return False
        except asyncio.TimeoutError:
            return True
