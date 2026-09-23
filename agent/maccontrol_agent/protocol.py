"""Event envelope construction/parsing for the MCA <-> ESP32 wire protocol.

Implements the Chapter 6 envelope and the Chapter 4.3.2 reconnect backoff
schedule as pure, network-free functions so they can be unit-tested.
"""
from __future__ import annotations

import json
import random
import re
from collections import namedtuple
from datetime import datetime, timezone

MAX_EVENT_BYTES = 4096
PROTOCOL_VERSION = 1
AGENT_VERSION = "1.1.2"

# Closed catalog of the twelve MCA event types (spec 6.2.1 + Phase 4.5
# amendment: front_app_changed, foreground-app deltas only).
EVENT_TYPES = (
    "agent_hello",
    "agent_goodbye",
    "heartbeat",
    "system_state_changed",
    "user_session_changed",
    "screen_lock_changed",
    "application_started",
    "application_exited",
    "command_ack",
    "command_result",
    "capability_report",
    "front_app_changed",
)

# Closed enum for command_result.error_code (spec 6.2.1).
COMMAND_RESULT_ERROR_CODES = (
    "app_not_allowlisted",
    "app_not_running",
    "launch_failed",
    "quit_failed",
    "command_disabled",
    "action_timeout",
)

GOODBYE_REASONS = ("shutdown", "restart", "sleep", "user_logout", "agent_stop")
SYSTEM_STATES = ("awake", "sleeping", "waking", "shutting_down", "restarting", "booting")
APP_EXIT_REASONS = ("quit", "crashed", "requested_by_agent")

AGENT_ACTIONS = ("launch_app", "quit_app")
DISPATCH_KEYS = frozenset(("action", "bundle_id", "command_id"))
BUNDLE_ID_RE = re.compile(r"^[A-Za-z0-9.\-]+$")

# WebSocket close-code handling (spec 4.2.1 / 9.1.1).
CLOSE_HALTED = frozenset((1008, 4001, 4003))
CLOSE_BACKOFF = frozenset((1000, 1001, 4002))
HALT_REASONS = {
    1008: "policy_violation",
    4001: "unpaired_or_revoked",
    4003: "protocol_mismatch",
}

# Queue item between producers (telemetry, actions) and the active transport.
# The transport calls EventFactory.next_event at send time so seq assignment
# happens in wire order.
OutboxItem = namedtuple("OutboxItem", ("etype", "payload", "command_id"))


def utcnow_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --------------------------------------------------------------------------
# Reconnect backoff (spec 4.3.2): base 1/2/4/8/30 s + uniform 0-20% jitter.
# --------------------------------------------------------------------------

def backoff_base(attempt: int) -> float:
    """Base delay in seconds for the given 1-based consecutive-failed-attempt count."""
    if attempt <= 1:
        return 1.0
    if attempt == 2:
        return 2.0
    if attempt == 3:
        return 4.0
    if attempt == 4:
        return 8.0
    return 30.0


def backoff_delay(attempt: int, rng=None) -> float:
    """Base delay plus uniform jitter drawn from [0, 0.2 * base]."""
    rng = rng or random
    base = backoff_base(attempt)
    return base + rng.uniform(0.0, 0.2 * base)


def classify_close(code) -> str:
    """Map a WebSocket close code (or None for network loss) to 'halted' or 'backoff'."""
    if code in CLOSE_HALTED:
        return "halted"
    return "backoff"  # 1000/1001/4002, 1006 (abnormal loss), or unknown


# --------------------------------------------------------------------------
# Envelope factory
# --------------------------------------------------------------------------

class EventFactory:
    """Assigns event_id / seq / session_id per the Chapter 6 envelope rules.

    - event_id: ``evt_N``, per-boot counter starting at 1.
    - seq: per-session, starts at 1 with the agent_hello frame (which is the
      only frame without a session_id) and increments by 1 per frame.
    - Serialized envelopes are capped at 4 KB.
    """

    def __init__(self, agent_instance_id: str, agent_version: str = AGENT_VERSION):
        self.agent_instance_id = agent_instance_id
        self.agent_version = agent_version
        self._event_n = 0
        self._session_id = None
        self._seq = 0

    @property
    def session_id(self):
        return self._session_id

    def begin_session(self, session_id: str) -> None:
        if not session_id:
            raise ValueError("session_id must be non-empty")
        self._session_id = session_id

    def end_session(self) -> None:
        self._session_id = None
        self._seq = 0  # a 4002-forced reconnect opens a new session, seq resets

    def next_event(self, etype: str, payload: dict, command_id=None) -> dict:
        if etype not in EVENT_TYPES:
            raise ValueError("unknown event type: %r" % (etype,))
        self._event_n += 1
        if self._session_id is None:
            if etype != "agent_hello":
                raise RuntimeError("no active session; only agent_hello may be sent")
            self._seq = 1
            session_id = None
        else:
            self._seq += 1
            session_id = self._session_id
        envelope = {
            "event_id": "evt_%d" % self._event_n,
            "agent_instance_id": self.agent_instance_id,
            "session_id": session_id,
            "seq": self._seq,
            "timestamp": utcnow_iso(),
            "type": etype,
            "command_id": command_id,
            "payload": payload,
        }
        data = json.dumps(envelope, separators=(",", ":")).encode("utf-8")
        if len(data) > MAX_EVENT_BYTES:
            raise ValueError("serialized envelope exceeds %d bytes" % MAX_EVENT_BYTES)
        return envelope


def hello_payload(boot_id: str, hostname: str) -> dict:
    return {
        "protocol_version": PROTOCOL_VERSION,
        "agent_version": AGENT_VERSION,
        "boot_id": boot_id,
        "hostname": hostname,
    }


# --------------------------------------------------------------------------
# Inbound parsing (server->agent)
# --------------------------------------------------------------------------

class MalformedDispatch(ValueError):
    """A server frame cannot be correlated (no usable command_id); drop + log."""


def parse_hello_ack(obj) -> dict:
    """Validate the bare (non-enveloped) hello_ack server frame."""
    if not isinstance(obj, dict) or obj.get("type") != "hello_ack":
        raise ValueError("expected hello_ack frame, got: %r" % (obj,))
    session_id = obj.get("session_id")
    if not isinstance(session_id, str) or not session_id:
        raise ValueError("hello_ack missing session_id")
    if obj.get("protocol_version") != PROTOCOL_VERSION:
        raise ValueError("hello_ack protocol_version != %d" % PROTOCOL_VERSION)
    return obj


def parse_dispatch(obj) -> dict:
    """Validate the shape of an ESP32->MCA dispatch enough to correlate it.

    A dispatch whose command_id is missing or not a string cannot be answered
    and MUST be dropped + logged (MalformedDispatch). Shape problems we *can*
    correlate (unknown action, unexpected keys) are handled by actions.py,
    which answers command_result(failed, app_not_allowlisted) per spec 9.3.1.
    """
    if not isinstance(obj, dict):
        raise MalformedDispatch("dispatch is not a JSON object")
    cid = obj.get("command_id")
    if not isinstance(cid, str) or not cid:
        raise MalformedDispatch("dispatch missing command_id: %r" % (obj,))
    return obj


def validate_dispatch_shape(dispatch: dict):
    """Returns (action, bundle_id) or raises ValueError with a human reason.

    Enforces: exactly the three keys action/bundle_id/command_id, action in the
    closed set {launch_app, quit_app}, bundle_id a well-formed string.
    """
    keys = frozenset(dispatch.keys())
    if keys != DISPATCH_KEYS:
        raise ValueError("unexpected keys in dispatch: %r" % (sorted(keys),))
    action = dispatch["action"]
    bundle_id = dispatch["bundle_id"]
    if action not in AGENT_ACTIONS:
        raise ValueError("unknown action: %r" % (action,))
    if not isinstance(bundle_id, str) or not BUNDLE_ID_RE.match(bundle_id):
        raise ValueError("invalid bundle_id: %r" % (bundle_id,))
    return action, bundle_id


def parse_envelope(obj: dict) -> dict:
    """Round-trip check for an envelope we constructed (used by tests/tools)."""
    required = ("event_id", "agent_instance_id", "session_id", "seq",
                "timestamp", "type", "command_id", "payload")
    for key in required:
        if key not in obj:
            raise ValueError("envelope missing key %r" % (key,))
    if obj["type"] not in EVENT_TYPES:
        raise ValueError("unknown event type %r" % (obj["type"],))
    return obj
