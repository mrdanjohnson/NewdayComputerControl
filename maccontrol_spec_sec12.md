## 12. REST API Contract

This chapter fixes the complete HTTP surface of the MacControl Endpoint (ESP32): every path, method, role requirement, and Mode A availability a controller or MacControlAgent (MCA) may rely on. Two invariants organize the contract. First, all controller access is versioned under `/api/v1` and all agent traffic under `/agent/v1`; unversioned paths MUST be rejected with HTTP 404 (Chapter 4.1.1). Second, every command-producing controller endpoint — power commands, macro execution, and application launch/quit — creates a command ledger record **before** dispatch and returns HTTP 202 with a `command_id` (Chapter 5.1.2); non-command-producing mutations (macro CRUD, API key management, OTA upload/apply) return HTTP 200/201 synchronously and create no ledger record. No endpoint on this surface executes an action synchronously, and none accepts arbitrary shell or free-form execution requests (Chapter 13.3). The API is an ordinary deterministic HTTP interface: external AI clients, if any, consume it exactly as Q-SYS, Companion, browsers, and scripts do.

### 12.1 Endpoint inventory

#### 12.1.1 Controller endpoints

The table below is the master inventory of the controller surface. Roles are the RBAC scopes READ, CONTROL, and ADMIN (Chapter 13.1); "Mode A" states behavior when no MCA is paired.

| Method and path | Role | Mode A | Function |
|---|---|---|---|
| `GET /api/v1/status` | READ | full | Status cache document with provenance/freshness (Chapter 7) |
| `GET /api/v1/capabilities` | READ | full | Mode-aware capabilities document (12.3.1) |
| `POST /api/v1/commands` | CONTROL | full | Generic command submission (12.2.1) |
| `GET /api/v1/commands` | READ | full | Ledger listing with filtering and pagination (12.2.1) |
| `GET /api/v1/commands/{id}` | READ | full | Single ledger record, latest revision |
| `POST /api/v1/system/{wake\|sleep\|restart\|shutdown\|lock}` | CONTROL | full | Convenience routes; expand to `POST /api/v1/commands` |
| `GET /api/v1/macros` | READ | full | Macro list with revisions |
| `POST /api/v1/macros`, `PUT/DELETE /api/v1/macros/{id}` | ADMIN | full | Macro configuration (mirrors ESP32 Web UI) |
| `POST /api/v1/macros/{id}/execute` | CONTROL | full | `http_button` trigger (Chapter 10.2.1) |
| `POST /api/v1/apps/{bundle_id}/launch`, `.../quit` | CONTROL | 409 `agent_not_paired` | Agent action against MCA allowlist (Chapter 11) |
| `GET /api/v1/agent/status` | READ | 409 `agent_not_paired` | Latest MCA status report (Chapter 6.3) |
| `GET /api/v1/logs` | READ | full | Ring-buffer log, `?category=`, `?level=`, `?since_seq=`, `?limit=` (Chapter 15) |
| `GET /api/v1/ota/status`, `POST /api/v1/ota/upload`, `POST /api/v1/ota/apply` | READ / ADMIN | full | Signed dual-partition OTA (Chapter 15.3) |
| `GET /api/v1/openapi.json` | READ | full | Machine-readable copy of this contract (Chapter 17.1) |

Three consequences follow for implementers. First, the surface is closed: a request to any path not listed, including plausible extras such as `/api/v1/exec`, MUST return 404 `not_found`, which is the API-level enforcement of the closed action invariant. Second, the two Mode-A-denied endpoints fail *before* ledger write: because app launch/quit are dispatchable only through the MCA, accepting them in Mode A would manufacture commands doomed to `timed_out`, so they MUST be rejected with 409 `agent_not_paired` — or 409 `agent_offline` when an agent is paired but its session is not ACTIVE — and MUST NOT create records. Third, convenience routes are pure aliases: `POST /api/v1/system/restart` with an empty body MUST produce a ledger record identical in every field to `POST /api/v1/commands` with `{"type": "restart"}`, so controllers can be tested against either path interchangeably.

#### 12.1.2 Agent-facing endpoints

| Method and path | Credential | Function |
|---|---|---|
| `POST /agent/v1/pair` | none (pre-credential; one-time pairing code in body) | Completes the Chapter 3 pairing ceremony; the only pre-credential agent endpoint, rate-limited per Chapter 13 |
| `GET /agent/v1/ws` (Upgrade) | pairing bearer token | Primary MCA channel; `agent_hello`/`hello_ack`, then bidirectional frames (Chapter 4.2) |
| `POST /agent/v1/events` | pairing bearer token + `X-Session-Id` | Polling-fallback ingestion; body is the identical Chapter 6 event envelope |
| `GET /agent/v1/commands/pending` | pairing bearer token + `X-Session-Id` | Replaces server push; returns dispatched actions awaiting MCA acknowledgement |

`POST /agent/v1/pair` is the sole exception to the pairing-credential requirement: it is reachable only while an ESP32-initiated pairing window is open, accepts only the one-time pairing code displayed on the ESP32 Web UI, and is rate-limited to one validation attempt per second with the window closing after 5 failed attempts (Chapter 3.2.1).

The two transports are semantically identical (Chapter 4.3.1): the same envelope schema and sequence rules apply, and because there is no one-to-one WebSocket-close-to-HTTP mapping, fallback rejections follow the deterministic HTTP rules of Chapter 4.3.1 (401 `unauthorized` for missing/invalid credentials, 403 `forbidden` for revoked/disabled credentials, 409 `agent_not_paired`, 409 `agent_offline` for inactive sessions, 400 `validation_failed` for schema violations). Heartbeat semantics are transport-independent: the MCA MUST emit a `heartbeat` event every 5 s (default, configurable 2–30 s); any valid frame or POST resets the ESP32 liveness clock, 15 s of silence marks agent-derived status stale, and 30 s declares the session OFFLINE. `GET /agent/v1/commands/pending` defaults to a 5 s poll interval (2–30 s) and returns only actions the current `capability_report` permits, so a polling agent can never receive an action its operator disabled.

### 12.2 Command API

#### 12.2.1 Submission, record retrieval, filtering, pagination, and idempotency

`POST /api/v1/commands` accepts the closed command-type enum `wake`, `sleep`, `restart`, `shutdown`, `lock`, `macro_execute`, `app_launch`, `app_quit`. The submission schema is minimal because every command type has fixed semantics (Chapter 5.3.1):

```json
{
  "type": "app_launch",
  "parameters": { "bundle_id": "com.renewedvision.propresenter" },
  "idempotency_key": "qsys-show-preset-042"
}
```

`parameters` is type-scoped: empty for the five power/lock commands, `{"macro_id": "mac_3F81"}` for `macro_execute`, and `{"bundle_id": "<allowlisted bundle ID>"}` for `app_launch`/`app_quit`; unknown keys MUST be rejected with 400 `bad_request`. A valid request is persisted to the ledger in state `accepted` before any dispatch and answered with HTTP 202:

```json
{
  "command_id": "8F31A2C4",
  "state": "accepted",
  "deadline_at": "2025-01-14T09:34:03Z",
  "record_url": "/api/v1/commands/8F31A2C4"
}
```

The `deadline_at` in the 202 body is a projection (acceptance plus the per-type deadline); the authoritative value is set at dispatch in the ledger record, and both MUST agree within the 30 s dispatch-delay bound of Chapter 5.3.1. `GET /api/v1/commands/{id}` returns the latest record revision:

```json
{
  "command_id": "8F31A2C4",
  "revision": 4,
  "type": "restart",
  "parameters": {},
  "requested_by": "apikey:control-01",
  "requested_at": "2025-01-14T09:31:02Z",
  "mode_at_accept": "B",
  "state": "completed",
  "dispatched_at": "2025-01-14T09:31:03Z",
  "deadline_at": "2025-01-14T09:34:03Z",
  "expected_offline_window": { "open_after_s": 3, "close_after_s": 60 },
  "evidence": ["evt_1042", "evt_1051"],
  "result": "restart_confirmed",
  "error_code": null
}
```

`GET /api/v1/commands` lists records newest-first with exact-match filters `?state=`, `?type=`, and `?since=` (ISO 8601), and keyset pagination: `?limit=` (default 50, maximum 200) plus an opaque `?cursor=`; the response carries `commands[]` and `next_cursor` (null at the end). Keyset rather than offset pagination is mandatory because FIFO eviction (Chapter 5.1.1) makes offsets unstable. Idempotency follows Chapter 5.1.1 exactly: a replayed `Idempotency-Key` header or `idempotency_key` field with an identical body returns HTTP 200 and the existing record; the same key with a divergent body returns 409 `conflict`. A submission with no explicit idempotency key executes as a new command, with exactly one exception: a submission whose derived `(command_type, target, normalized_parameters)` hash matches a non-terminal in-flight record (`accepted`, `dispatched`, or `confirming`) accepted within the last 60 seconds is coalesced to that existing `command_id` — the ESP32 returns HTTP 202 with the existing `command_id` and current state and MUST NOT create a new record or dispatch a duplicate. Once the earlier command terminates or the 60-second window elapses, an identical no-key submission executes as a new command. Explicit-key protection spans ledger retention, never beyond it; coalescing never spans terminal records.

**Worked verified restart interaction (Mode B).** The controller issues `POST /api/v1/system/restart` with header `Idempotency-Key: nightly-recycle-2025-01-14`. The ESP32 persists the record above in `accepted` and returns 202. At `dispatched_at` + 5 s the MCA emits `agent_goodbye(reason=restart)` (`evt_1042`); the ESP32 closes the socket with 1001, the offline onset lands inside $[3\text{ s}, 60\text{ s}]$, and a `GET` at +20 s shows `state: "confirming"`. At +48 s the host returns; the MCA opens a new session with `agent_hello(boot_id = b_NEW ≠ b_OLD)` followed by the mandatory initial status burst reporting `system_state=awake` (`evt_1051`). The ESP32 — not the agent — evaluates the two-phase predicate of Chapter 8.2.1 and appends the terminal revision `completed` / `restart_confirmed`. A final `GET /api/v1/commands/8F31A2C4` returns the record exactly as shown above, with both evidence IDs listed. If the same `Idempotency-Key` is reposted, the controller receives 200 with this record and no second restart occurs.

### 12.3 Error and capability models

#### 12.3.1 Deterministic error envelope and mode-aware capabilities

Every non-2xx response on either surface uses the envelope of Chapter 4.1.1 — `{"error": {"code", "message", "request_id"}}` — with the closed HTTP/code mapping defined there. The command API adds the following codes under that envelope; `message` is human-readable and non-normative, `code` is the contract.

| HTTP | `code` | Trigger |
|---|---|---|
| 400 | `bad_request` | Unknown command type, parameter key, or schema violation on a controller request |
| 400 | `validation_failed` | Agent event envelope schema violation or unknown event type on `/agent/v1/` (Chapter 6.1.1) |
| 400 | `macro_invalid_step` | Any invalid macro definition: a step outside the closed Chapter 10.1.1 enums, or an invalid `expected_event` such as a forbidden verification event type |
| 409 | `conflict` | Idempotency-key replay with divergent body |
| 409 | `agent_not_paired` | Agent-dependent command or endpoint in Mode A; rejected pre-ledger, no record created |
| 409 | `agent_offline` | Agent paired but session not ACTIVE; rejected pre-ledger, no record created |
| 409 | `command_disabled` | Action absent from MCA `capability_report.enabled_commands` |
| 409 | `app_not_allowlisted` | Target `bundle_id` absent from `allowlisted_apps` |
| 409 | `app_not_registered` | Target `bundle_id` absent from the ESP32 monitored registry (Chapter 11.1) |
| 409 | `app_control_disabled` | Registry entry has `control_enabled: false` |
| 409 | `macro_queue_full` | Execution queue at capacity; rejected pre-ledger, no record created (Chapter 10.3.1) |
| 409 | `ota_in_progress` | OTA requested while another OTA operation is in progress, or `ota/apply` requested without `force` while commands are in flight (Chapter 15.3) |
| 409 | `store_corrupt` | Persistent macro store failed CRC validation (Chapter 15.1) |
| 503 | `ledger_unavailable` | Ledger write failed; nothing dispatched |
| 503 | `network_unavailable` | Requests arriving during association loss (Chapter 15.1) |

The 409 family is deliberately pre-ledger: each condition is knowable at request time, so the endpoint fails the request deterministically instead of accepting a command whose only possible verdict is failure. Controllers MUST treat 400/409 as permanent and MUST NOT retry them unchanged. WebSocket close codes 4001/4002/4003 are not HTTP status codes and are excluded from this mapping; there is no one-to-one mapping from close codes to HTTP statuses, and on the polling fallback agent-facing rejections use the deterministic HTTP rules of Chapter 4.3.1.

`GET /api/v1/capabilities` returns the mode-aware integration document external consumers (including Q-SYS modules and Chapter 17 tooling) use to enumerate what this specific endpoint can currently do:

```json
{
  "api_version": "v1",
  "mode": "B",
  "capability_level": "L3",
  "device": { "name": "ProPresenter Mac", "hostname": "maccontrol-01.local" },
  "commands": {
    "wake":     { "available": true, "verified": true, "deadline_s": 120 },
    "sleep":    { "available": true, "verified": true, "deadline_s": 90 },
    "restart":  { "available": true, "verified": true, "deadline_s": 180 },
    "shutdown": { "available": true, "verified": true, "deadline_s": 120 },
    "lock":     { "available": true, "verified": true, "deadline_s": 15 },
    "macro_execute": { "available": true, "verified": true, "macro_ids": ["mac_3F81"] },
    "app_launch": { "available": true, "verified": true, "apps": [ { "bundle_id": "com.renewedvision.propresenter", "display_name": "ProPresenter" } ] },
    "app_quit":   { "available": true, "verified": true, "apps": [ { "bundle_id": "com.renewedvision.propresenter", "display_name": "ProPresenter" } ] }
  },
  "agent": { "paired": true, "connected": true, "enabled_commands": ["launch_app", "quit_app"], "allowlisted_apps": ["com.renewedvision.propresenter"] }
}
```

The document is mode-aware in exactly two dimensions. The `commands` object MUST carry exactly one entry for each of the eight closed command types of Chapter 12.2.1, each with `available` and `verified` booleans, per the Chapter 17.1.1 schema. `available` reflects dispatchability *now*: in Mode A, `app_launch` and `app_quit` MUST report `available: false`, and `agent` MUST report `{"paired": false, "connected": false}` with empty lists. `verified` reports whether `completed` is reachable for that command type: in Mode A every command MUST report `verified: false`, because Chapter 5.2.2 confines Mode A to `unconfirmed`/`hid_only` terminal states. The document is generated live — from pairing state, session liveness, and the latest `capability_report` — never cached across a session transition, so a controller that polls after an MCA disconnect sees `verified` collapse to false within the 30 s offline threshold. Acceptance implication: a conformance client that drives the endpoint purely from `/api/v1/capabilities` plus `/api/v1/openapi.json` MUST be able to submit every `available: true` command and MUST never observe a terminal `completed` for a command the document reported as `verified: false`.
