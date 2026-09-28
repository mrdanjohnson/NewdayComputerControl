"""WebSocket transport (spec 4.2): connect, agent_hello, hello_ack, session loop.

Reconnect backoff per spec 4.3.2; close-code handling per spec 4.2.1:
  - 1000/1001/4002 / network loss  -> BACKOFF loop
  - 1008/4001/4003                 -> HALTED (no reconnect; caller exits 2)
  - a 4002 reconnect starts a NEW session with seq reset (via end_session)
  - during a declared expected-offline window (agent_goodbye for
    sleep/restart/shutdown) the agent does not reconnect before close_after_s.
"""
from __future__ import annotations

import asyncio
import json
import time

import websockets

from . import protocol

HELLO_TIMEOUT_S = 5.0


class TransportError(Exception):
    pass


class Halted(Exception):
    def __init__(self, code, reason):
        super().__init__("halted by close code %s: %s" % (code, reason))
        self.code = code
        self.reason = reason


def _auth_headers(token):
    return {"Authorization": "Bearer %s" % token}


class WSTransport:
    def __init__(self, rt):
        self.rt = rt
        self.hostname = rt.state.hostname
        self.uri = "ws://%s:80/agent/v1/ws" % self.hostname

    # -- one connection attempt ---------------------------------------------

    async def _connect_once(self):
        """Open the socket, send agent_hello, wait up to 5 s for hello_ack."""
        rt = self.rt
        rt.factory.end_session()
        ws = await websockets.connect(
            self.uri,
            additional_headers=_auth_headers(rt.state.agent_token),
            open_timeout=HELLO_TIMEOUT_S,
        )
        try:
            hello = rt.factory.next_event(
                "agent_hello", protocol.hello_payload(rt.boot_id, self.hostname))
            await ws.send(json.dumps(hello, separators=(",", ":")))
            raw = await asyncio.wait_for(ws.recv(), timeout=HELLO_TIMEOUT_S)
            try:
                ack = protocol.parse_hello_ack(json.loads(raw))
            except (ValueError, json.JSONDecodeError) as exc:
                raise TransportError("bad hello_ack: %s" % exc)
            rt.factory.begin_session(ack["session_id"])
            return ws, ack
        except Exception:
            await ws.close()
            raise

    # -- session ------------------------------------------------------------

    async def _sender(self, ws):
        rt = self.rt
        while True:
            try:
                item = await asyncio.wait_for(rt.outbox.get(), timeout=0.5)
            except asyncio.TimeoutError:
                if rt.stop_event.is_set():
                    return
                continue
            # task_done in finally (not just on success): outbox.join() must
            # not hang when a frame fails — the session tears down anyway.
            try:
                envelope = rt.factory.next_event(item.etype, item.payload, item.command_id)
                await ws.send(json.dumps(envelope, separators=(",", ":")))
            finally:
                rt.outbox.task_done()

    async def _flush_outbox(self):
        """Returns once every frame enqueued so far has left the socket."""
        await self.rt.outbox.join()

    async def _receiver(self, ws):
        rt = self.rt
        async for raw in ws:
            try:
                dispatch = protocol.parse_dispatch(json.loads(raw))
            except (protocol.MalformedDispatch, ValueError) as exc:
                rt.log("ignoring malformed server frame: %s" % exc)
                continue
            from .actions import handle_dispatch
            await handle_dispatch(rt, dispatch)

    async def _session(self, ws, ack):
        rt = self.rt
        rt.on_session_start(ack)
        rt.log("session %s active (heartbeat %ss)" % (
            ack["session_id"], ack.get("heartbeat_interval_s", 5)))
        sender = asyncio.ensure_future(self._sender(ws))
        receiver = asyncio.ensure_future(self._receiver(ws))
        stopper = asyncio.ensure_future(rt.stop_event.wait())
        rt.flush_now = self._flush_outbox
        try:
            done, pending = await asyncio.wait(
                {sender, receiver, stopper}, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            for task in done:
                exc = task.exception() if not task.cancelled() else None
                if exc and not isinstance(exc, websockets.ConnectionClosed):
                    rt.log("session task failed: %r" % (exc,))
        finally:
            for task in (sender, receiver, stopper):
                task.cancel()
            try:
                await asyncio.gather(sender, receiver, stopper,
                                     return_exceptions=True)
            except Exception:
                pass
            rt.flush_now = None
        # Grace period so a queued agent_goodbye flushes before close.
        try:
            while not self.rt.outbox.empty():
                await asyncio.sleep(0.1)
            await asyncio.wait_for(ws.close(code=1000, reason="agent closing"),
                                   timeout=2.0)
        except Exception:
            pass
        return ws.close_code if ws.close_code is not None else 1000

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
                return 0  # stop requested while sleeping

            try:
                ws, ack = await self._connect_once()
                attempt = 0  # reset only on successful hello_ack (spec 4.3.2)
            except Halted as h:
                rt.halt_reason = h.reason
                rt.log("HALTED: %s" % h.reason)
                return 2
            except asyncio.TimeoutError:
                attempt += 1
                rt.log("hello_ack timeout; backing off")
                if not await self._backoff(attempt):
                    return 0
                continue
            except websockets.exceptions.InvalidStatusCode as exc:
                # HTTP-level refusal of the upgrade (e.g. 401/403/404).
                code = getattr(exc, "status_code", None)
                attempt += 1
                rt.log("upgrade refused (HTTP %s); backing off" % code)
                if not await self._backoff(attempt):
                    return 0
                continue
            except (OSError, websockets.WebSocketException, TransportError) as exc:
                attempt += 1
                rt.log("connect failed: %s" % exc)
                if not await self._backoff(attempt):
                    return 0
                continue

            try:
                close_code = await self._session(ws, ack)
            except Halted as h:
                rt.halt_reason = h.reason
                rt.log("HALTED: %s" % h.reason)
                return 2
            except websockets.ConnectionClosed as exc:
                close_code = exc.code
            except (OSError, websockets.WebSocketException) as exc:
                rt.log("session ended with network error: %s" % exc)
                close_code = 1006
            finally:
                rt.on_session_end()

            if rt.stop_event.is_set():
                return 0
            if rt.halt_reason:
                return 2

            if protocol.classify_close(close_code) == "halted":
                reason = protocol.HALT_REASONS.get(
                    close_code, "halted_close_%s" % close_code)
                rt.halt_reason = reason
                rt.log("HALTED by close %s (%s); no reconnect" % (close_code, reason))
                return 2
            if close_code == 4002:
                rt.log("close 4002 (sequence violation); new session will reset seq")
            attempt += 1
            if not await self._backoff(attempt):
                return 0

    async def _respect_offline_window(self):
        """Sleep out a declared expected-offline window before reconnecting."""
        rt = self.rt
        while not rt.stop_event.is_set():
            until = rt.expected_offline_until
            if until is None:
                return True
            remaining = until - time.monotonic()
            if remaining <= 0:
                rt.expected_offline_until = None
                return True
            rt.log("expected-offline window: holding reconnect for %.1f s"
                   % remaining)
            try:
                await asyncio.wait_for(rt.stop_event.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                pass
        return False

    async def _backoff(self, attempt):
        delay = protocol.backoff_delay(attempt)
        self.rt.log("reconnect attempt %d in %.1f s" % (attempt, delay))
        try:
            await asyncio.wait_for(self.rt.stop_event.wait(), timeout=delay)
            return False  # stop requested
        except asyncio.TimeoutError:
            return True
