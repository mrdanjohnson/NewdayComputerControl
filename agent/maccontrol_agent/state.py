"""Persistent MCA state: ~/.maccontrol/agent.json (chmod 600)."""
from __future__ import annotations

import json
import os
import secrets

DEFAULT_STATE_DIR = os.path.expanduser("~/.maccontrol")
DEFAULT_STATE_PATH = os.path.join(DEFAULT_STATE_DIR, "agent.json")

TRANSPORTS = ("websocket", "polling")

# Fail-closed defaults: no app actions enabled until the operator enables them.
DEFAULT_ENABLED_COMMANDS = {"launch_app": False, "quit_app": False}

SCHEMA_VERSION = 1


def new_agent_instance_id() -> str:
    """Persistent per-installation identity: 'ag-' + 4 lowercase hex chars."""
    return "ag-" + secrets.token_hex(2)


class AgentState:
    def __init__(self, path: str = DEFAULT_STATE_PATH):
        self.path = path
        self.hostname = None          # paired endpoint hostname (bare label)
        self.agent_instance_id = None  # persistent, generated once at first run
        self.agent_token = None        # base64url 32-byte token, shown once at pair
        self.allowlist = []            # list of bundle IDs
        self.enabled_commands = dict(DEFAULT_ENABLED_COMMANDS)
        self.transport = "websocket"   # 'websocket' | 'polling'
        self.poll_interval_s = 5
        self.boot_id = None            # cached boot identity (see events.derive_boot_id)
        self.boot_key = None           # kern.boottime seconds the boot_id derives from

    @property
    def paired(self) -> bool:
        return bool(self.hostname and self.agent_token and self.agent_instance_id)

    def ensure_instance_id(self) -> str:
        if not self.agent_instance_id:
            self.agent_instance_id = new_agent_instance_id()
        return self.agent_instance_id

    # -- persistence -------------------------------------------------------

    @classmethod
    def load(cls, path: str = DEFAULT_STATE_PATH) -> "AgentState":
        st = cls(path)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except FileNotFoundError:
            return st  # unpaired state
        except (OSError, json.JSONDecodeError):
            return st  # unreadable/corrupt -> treat as unpaired, never crash
        st.hostname = data.get("hostname")
        st.agent_instance_id = data.get("agent_instance_id")
        st.agent_token = data.get("agent_token")
        allowlist = data.get("allowlist", [])
        if isinstance(allowlist, list):
            st.allowlist = [b for b in allowlist if isinstance(b, str)]
        enabled = data.get("enabled_commands", {})
        if isinstance(enabled, dict):
            for k in DEFAULT_ENABLED_COMMANDS:
                st.enabled_commands[k] = bool(enabled.get(k, False))
        if data.get("transport") in TRANSPORTS:
            st.transport = data["transport"]
        try:
            st.poll_interval_s = max(2, min(30, int(data.get("poll_interval_s", 5))))
        except (TypeError, ValueError):
            st.poll_interval_s = 5
        st.boot_id = data.get("boot_id")
        st.boot_key = data.get("boot_key")
        return st

    def save(self) -> None:
        d = os.path.dirname(self.path)
        if d:
            os.makedirs(d, exist_ok=True)
        data = {
            "schema_version": SCHEMA_VERSION,
            "hostname": self.hostname,
            "agent_instance_id": self.agent_instance_id,
            "agent_token": self.agent_token,
            "allowlist": self.allowlist,
            "enabled_commands": self.enabled_commands,
            "transport": self.transport,
            "poll_interval_s": self.poll_interval_s,
            "boot_id": self.boot_id,
            "boot_key": self.boot_key,
        }
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
            fh.write("\n")
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)
