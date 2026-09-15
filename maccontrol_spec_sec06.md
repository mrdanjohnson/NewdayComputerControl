## 6. MCA Protocol, Events, and Status Schemas

This chapter defines the complete message vocabulary that MacControlAgent (MCA) may send to the MacControl Endpoint (ESP32) over the transports of Chapter 4 (`/agent/v1/ws`, or `POST /agent/v1/events` in polling fallback). The vocabulary is a closed set: there are exactly eleven event types, one envelope, and one composite status report. Every message is evidence. The ESP32 ingests it, validates it against the pairing record and session state, and alone decides what it advances — the command ledger (Chapter 5) or the status cache (Chapter 7). The MCA never learns whether its evidence was sufficient, and no message in this chapter carries authority to close a command.

### 6.1 Event envelope

#### 6.1.1 Define event_id, agent_instance_id, session sequence, timestamp, type, payload, and optional command_id correlation

All MCA-to-ESP32 messages — WebSocket frames and HTTP POST bodies alike — MUST use the single envelope below. Maximum serialized size is 4 KB; larger frames MUST be rejected.

```json
{
  "event_id": "evt_1047",
  "agent_instance_id": "ag-7e21",
  "session_id": "s_9A21",
  "seq": 42,
  "timestamp": "2025-01-14T09:31:10Z",
  "type": "screen_lock_changed",
  "command_id": null,
  "payload": { "locked": true }
}
```

| Field | Set by | Normative rule |
|---|---|---|
| `event_id` | MCA | `evt_` prefix plus a per-boot monotonically increasing counter starting at 1; unique within (`agent_instance_id`, `boot_id`). Referenced by ledger `evidence` arrays. |
| `agent_instance_id` | MCA | MUST equal the stored pairing record's `agent_instance_id`; mismatch closes the socket with 4001, and on the polling fallback is rejected per the deterministic rules of Chapter 4.3.1 (unpaired `agent_instance_id` → HTTP 409 `agent_not_paired`). `device_id` refers to the ESP32 endpoint only and never identifies the agent. |
| `session_id` | ESP32 | Assigned in `hello_ack`; MCA echoes it on every frame. On the polling transport it is carried as the `X-Session-Id` header; if a body value is also present the two MUST be equal. Unknown session closes with 4002 (polling fallback: HTTP 409 `agent_offline`, Chapter 4.3.1; the MCA MUST open a new session; `seq` resets). |
| `seq` | MCA | Per-session integer starting at 1, incremented by exactly 1 per frame; a gap greater than `max_seq_gap` (default 10) closes with 4002 (Chapter 4.2.1). |
| `timestamp` | MCA | Agent-local clock, ISO 8601 UTC. Advisory only: the ESP32 records its own `received_at` and MUST use `received_at` for all deadline and expected-offline-window arithmetic. |
| `type` | MCA | Closed enum from Section 6.2. Unknown types are rejected (polling fallback: HTTP 400 `validation_failed`) and counted as schema violations; the WebSocket closes with 4003 only after the configured threshold of consecutive schema-violating frames (default 3, configurable 1–10). |
| `command_id` | MCA | Optional; meaningful only on `command_ack` and `command_result`. MUST reference a ledger record; correlation to a record not in `confirming` state is ignored, never an error. |
| `payload` | MCA | Per-type object from Section 6.2; unknown keys are schema violations. |

Three rules follow for implementers. First, because `timestamp` is advisory, the agent needs no clock synchronization: an MCA with a wildly wrong clock produces evidence that is still valid, since freshness is measured against ESP32-side receipt time. Second, a frame discarded for schema violation still consumes its `seq` value, so the next valid frame is not a sequence gap; three consecutive schema-violating frames (default, configurable 1–10) close the socket with 4003. Third, the soft handling of a stale `command_id` is deliberate: a `command_result` arriving after its command timed out is logged as an event but MUST NOT reopen a terminal ledger record, which keeps terminal states immutable (Chapter 5.2.1).

### 6.2 Event catalog

#### 6.2.1 Define agent_hello, agent_goodbye, heartbeat, system_state_changed, user_session_changed, screen_lock_changed, application_started, application_exited, command_ack, command_result, and capability_report

| Type | Required payload keys | Emitted when | Ledger effect |
|---|---|---|---|
| `agent_hello` | `protocol_version`, `agent_version`, `boot_id`, `hostname` | First frame of every session | Opens a new session; a post-dispatch **new-session** `agent_hello` whose mandatory initial status burst reports `system_state=awake` satisfies the wake predicate (Chapter 8.1.2); satisfies the restart predicate only if `boot_id` differs from the pre-command value (Chapter 5.3.2) |
| `agent_goodbye` | `reason` ∈ {`shutdown`, `restart`, `sleep`, `user_logout`, `agent_stop`} | Graceful process or OS exit | Positive evidence inside the expected-offline window (defaults `open_after_s` 3 s, `close_after_s` 60 s) |
| `heartbeat` | `boot_id`, `uptime_s` | Every `heartbeat_interval` (default 5 s) | Liveness only; MUST NOT advance any command |
| `system_state_changed` | `state` ∈ {`awake`, `sleeping`, `waking`, `shutting_down`, `restarting`, `booting`} | OS power-state transition | `awake` satisfies the wake predicate only as part of a new session's mandatory initial status burst following `agent_hello` (Chapter 5.3.1); all values update the status cache |
| `user_session_changed` | `user_logged_in` (bool), `user` (string or null) | Console login or logout | Status evidence only; never completes a command |
| `screen_lock_changed` | `locked` (bool) | Screen lock or unlock | `locked: true` satisfies the lock predicate |
| `application_started` | `bundle_id`, `pid` | An allowlisted application launches | Satisfies the app launch predicate on exact `bundle_id` match |
| `application_exited` | `bundle_id`, `pid`, `reason` ∈ {`quit`, `crashed`, `requested_by_agent`} | An allowlisted application exits | `quit`/`requested_by_agent` satisfy the app quit predicate; `crashed` MUST fail an in-flight quit command with `error_code = "app_crashed"` |
| `command_ack` | `command_id`, `action` | Within 2 s of receiving a dispatched agent action | First half of the app-action predicates (`launch_app`/`quit_app` only, Chapter 5.3.1); macros are HID-only and never use `command_ack`/`command_result` |
| `command_result` | `command_id`, `outcome` ∈ {`ok`, `failed`}, `error_code` | After the attempted agent action | `ok` reports the local execution outcome only and MUST NOT by itself complete a command: the app-action predicates complete solely on correlated `command_ack` plus the matching `application_started`/`application_exited` event (Chapter 5.3.1); `failed` terminates the command with the supplied `error_code` |
| `capability_report` | `agent_version`, `protocol_version`, `os_version`, `enabled_commands[]`, `allowlisted_apps[]` (entries `{bundle_id, state}`, `state` ∈ {`running`, `not_running`}) | Immediately after `agent_hello` and on any MCA-UI configuration change or allowlisted-app state change | Gates dispatch: the ESP32 MUST NOT queue an action absent from `enabled_commands` or an application absent from `allowlisted_apps`. The per-entry `state` is the protocol representation of an allowlisted application that is not running — no separate event type exists for it — and it seeds the status cache `applications` map (Chapter 11.1.1). `enabled_commands` lists only agent-executed actions (`launch_app`, `quit_app`) — never controller command types (`app_launch`, `app_quit`, `restart`, `lock`), and never `ack_command`, which is the internal acknowledgement action and is always permitted |

The catalog enforces the authority split structurally. Only `command_ack` and `command_result` may carry `command_id`, so exactly two event types can influence a specific ledger record; every other event is ambient evidence usable only by the window-matching rules of Chapter 5.3.1. The `command_result.error_code` enum is closed: `app_not_allowlisted`, `app_not_running`, `launch_failed`, `quit_failed`, `command_disabled`, `action_timeout`. The `capability_report` is what makes MCA-side enablement enforceable: the ESP32 compares each dispatch against the most recent report, so a command the operator disabled in the MCA UI fails deterministically with `command_disabled` rather than silently executing. Events that match no open confirmation window are not wasted — they populate the status cache (Chapter 7) — but they MUST NOT retroactively alter any terminal record.

### 6.3 MCA status report

#### 6.3.1 Define system state, user state, system information, and monitored application map as MCA evidence only

Within 2 s of receiving `hello_ack` — on either transport — the MCA MUST emit an initial burst: one `system_state_changed`, one `user_session_changed`, one `screen_lock_changed`, one `capability_report` whose `allowlisted_apps` carries every allowlisted application with its current state (`running` or `not_running`), and optionally one `application_started` per running allowlisted application. The `capability_report` state entries are what give `not_running` its protocol representation in the burst (Chapter 11.1.1); the `application_started` events remain the delta stream for later launches. The ESP32 assembles these, plus subsequent deltas, into the composite MCA status report:

```json
{
  "agent_instance_id": "ag-7e21",
  "boot_id": "b_3F8A11",
  "reported_at": "2025-01-14T09:31:10Z",
  "system": { "state": "awake" },
  "user": { "logged_in": true, "name": "production", "screen_locked": false },
  "system_info": {
    "hostname": "mac-propresenter",
    "macos_version": "14.3",
    "hardware_model": "Macmini9,1",
    "uptime_s": 37210,
    "cpu_utilization_pct": 12.5,
    "memory_utilization_pct": 61.0,
    "disk_free_bytes": 241591910400,
    "network": { "reachable": true, "ip": "192.168.1.42" }
  },
  "applications": {
    "com.renewedvision.propresenter": { "state": "running", "pid": 812, "since": "evt_1031" }
  }
}
```

The report is a view over retained events, not an independent message: every scalar MUST be traceable to a specific `event_id`, and the ESP32 MUST be able to reconstruct the identical report from the events it accepted. `user.name` is the macOS short login name; the MCA MUST NOT attempt directory lookups or display-name resolution. Keys of the `applications` map are `bundle_id` values drawn from the current `capability_report.allowlisted_apps`; non-allowlisted processes MUST NOT appear, regardless of whether they are running, and application display names are presentation-only (Chapter 11). `system_info.hostname` is the bare DNS label; the `.local` suffix appears only in mDNS advertisements and serialized FQDN contexts. Dynamic `system_info` fields (`cpu_utilization_pct`, `memory_utilization_pct`, `disk_free_bytes`, `network`) are point-in-time samples refreshed at most once per `heartbeat_interval` (default 5 s), which bounds report traffic on constrained links. Critically, the report is evidence only: the ESP32 copies its fields into the status cache with provenance `agent_reported`, marks them stale after 15 s of silence, and reclassifies them as expected-offline inside declared windows (Chapter 7). The report never marks a command `completed` — that verdict remains reachable solely through the predicates of Chapter 5.
