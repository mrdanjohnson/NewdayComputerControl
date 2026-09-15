## 13. Security Model

Security in MacControl is structural, not optional: every privileged capability is gated by an explicit credential, an explicit role, and a closed action set. The model has three layers — controller authentication and role-based access control (RBAC) owned by the MacControl Endpoint (ESP32), agent trust anchored in the Chapter 3 pairing credential, and a closed action invariant that bounds what any authenticated party can cause the system to do. Consistent with the authority model of Chapter 2, controller credentials are ESP32-owned configuration, while command enablement and the application allowlist are MCA-owned policy; neither side can rewrite the other's trust state.

### 13.1 Controller security

#### 13.1.1 Define API keys, admin password, HTTPS where practical, rate limiting defaults, and READ/CONTROL/ADMIN scopes

Controllers authenticate to every `/api/v1/` endpoint with an API key presented in the `Authorization: Bearer` header. Keys are created, listed, and revoked only through the ESP32 Web UI by an ADMIN-scoped session. The ESP32 MUST store at most 8 active keys and MUST persist only a SHA-256 digest of each key; the raw key is displayed exactly once at creation, mirroring the pairing-token rule of Chapter 3.

```json
{
  "key_id": "key-04",
  "label": "Companion production controller",
  "role": "CONTROL",
  "key_sha256": "b91e…7a",
  "created_at": "2025-01-14T09:31:02Z",
  "last_used_at": "2025-01-14T10:12:03Z",
  "expires_at": null,
  "state": "active"
}
```

A raw key MUST be 32 random bytes, base64url-encoded with the prefix `mck_` so accidental commits and log captures are greppable. `expires_at` MAY be null (no expiry); when set, an expired key MUST be rejected with HTTP 401 `unauthorized`, never silently downgraded. The three roles form a strict total order READ < CONTROL < ADMIN, and every controller endpoint is assigned exactly one minimum role:

| Endpoint class | READ | CONTROL | ADMIN |
|---|---|---|---|
| `GET /api/v1/status`, `/capabilities`, `/openapi.json`, `/agent/status` | ✓ | ✓ | ✓ |
| `GET /api/v1/commands/{id}`, `GET /api/v1/macros`, `GET /api/v1/logs` | ✓ | ✓ | ✓ |
| `POST /api/v1/commands` and convenience power routes (`/api/v1/system/wake`, `/sleep`, `/restart`, `/shutdown`, `/lock`) | — | ✓ | ✓ |
| `POST /api/v1/macros/{id}/execute`, app launch/quit | — | ✓ | ✓ |
| Macro create/edit/delete, trigger bindings, device identity edits | — | — | ✓ |
| API key management, pairing window, controller security settings | — | — | ✓ |
| OTA upload/apply, factory reset, log clear | — | — | ✓ |

The matrix has two deliberate asymmetries. First, CONTROL can execute but cannot *define*: a production controller (or an external AI client, which is an ordinary HTTP consumer and receives no special role) can run existing macros and power commands but cannot introduce new keystroke sequences — the blast radius of a leaked CONTROL key is the operator's pre-approved action set, not the keyboard itself. Second, log *reading* is READ but log *clearing* is ADMIN, so evidence of misuse cannot be erased by the role that caused it. An external AI client SHOULD be issued CONTROL, never ADMIN, and SHOULD be issued its own labeled key so `last_used_at` and the event log attribute actions correctly.

The ESP32 Web UI is additionally protected by an ADMIN password (minimum 10 characters, stored as a salted hash, PBKDF2 or stronger). After 5 consecutive failed logins or 10 failed API authentications from one source IP, the endpoint MUST impose a 60-second lockout and log the event. Transport encryption is handled as an explicit, implementable rule: the endpoint serves plain HTTP by default and SHOULD additionally serve HTTPS (TLS 1.2+, port 443) when the operator installs a certificate through the Security page; when HTTPS is enabled, the endpoint MUST reject credential-bearing cleartext requests (any request carrying an `Authorization` header or ADMIN session cookie over plain HTTP) with HTTP 403 `forbidden` and a `https_required` message. When no certificate is installed, the Web UI MUST display a persistent warning that API keys traverse the LAN unencrypted, and operators SHOULD restrict the endpoint to a trusted management network. Rate limiting is per key with deterministic defaults (configurable in the Web UI): READ 60 requests/minute, CONTROL 30 requests/minute, ADMIN 10 requests/minute, enforced as a token bucket; excess requests return HTTP 429 `rate_limited` per the Chapter 4 error envelope and MUST NOT be queued.

### 13.2 Agent security

#### 13.2.1 Define paired credentials, command allowlist, application allowlist, credential rotation, and revocation

The MCA's sole credential is the Chapter 3 pairing token (`agent_token`, 32 bytes), presented on every WebSocket `agent_hello` and every polling request. There is exactly one pre-credential agent endpoint: `POST /agent/v1/pair`, which is reachable only while an ESP32-initiated pairing window is open, accepts only the one-time pairing code, and is rate-limited to one attempt per second with the window closing after 5 failed attempts (Chapter 3.2.1). Every other `/agent/v1/` request MUST be rejected per the deterministic credential rules: a missing, unknown, or expired token is HTTP 401 `unauthorized`, and a known but revoked or disabled credential is HTTP 403 `forbidden`; on the WebSocket transport both terminate the upgrade/session with close code 4001, but there is no one-to-one close-to-HTTP mapping (Chapter 4.3.1). Two allowlists then bound what a *validly authenticated* agent will do. The **command allowlist** (MCA UI "command enablement") gates the agent-executed actions — `launch_app` and `quit_app` — with fail-closed defaults, and these are the only values `capability_report.enabled_commands` may carry; the internal acknowledgement action `ack_command` is always permitted and is never an enabled command. An ESP32 dispatch of a disabled action is rejected pre-dispatch with 409 `command_disabled` wherever the current `capability_report` makes that knowable, and otherwise answered with `command_result(failed, command_disabled)` and never executed. The **application allowlist** enumerates bundle IDs eligible for launch/quit/report; matching is by bundle ID only, an empty allowlist denies all application actions, and any mismatch — including a bundle ID the ESP32's monitored registry expects but the MCA does not permit — resolves as `app_not_allowlisted` per the Chapter 11 reconciliation rules, with the divergence surfaced on both UIs.

Rotation and revocation are asymmetric by design. An ADMIN rotates or revokes the pairing credential by opening a new pairing ceremony or pressing revoke in the ESP32 Web UI; the MCA cannot self-rotate, because a compromised agent must not be able to mint fresh trust. Revocation takes effect within the same second: in-flight sessions close with code 4001, all `agent_reported` status-cache fields are marked stale, and the endpoint reverts to Mode A semantics — subsequent commands terminate `unconfirmed`, never `completed`, until a new ceremony completes. The exactly-one-active-pairing rule (Chapter 3) ensures rotation is implicitly revoking; there is no window in which two agent credentials are valid.

### 13.3 Closed action invariant

#### 13.3.1 Prohibit generic execution endpoints and enumerate the complete set of privileged actions

The defining security property of MacControl is that the complete set of privileged actions is enumerable and fixed at build time. The ESP32 MUST NOT expose, and the MCA MUST NOT implement, any generic execution capability; the following are prohibited outright:

- Arbitrary shell, AppleScript, or interpreter execution (no `POST /execute`-style endpoint on either component).
- Arbitrary file read, write, or transfer.
- HID injection outside the macro engine's declared step types; raw HID report streaming from the network.
- Mouse movement, clicks, or scroll (excluded from MVP per Chapter 10).
- Screen capture, keylogging, credential store access, or process control outside the allowlisted launch/quit actions.
- Listening sockets on the MCA; any inbound control path other than the authenticated session with the paired ESP32.
- Unsigned or unauthenticated OTA application (Chapter 15).

The complete privileged action set, and nothing else, is: (1) power commands `wake`, `sleep`, `restart`, `shutdown`, `lock`; (2) execution of stored macros composed solely of Chapter 10 step types; (3) `launch_app`/`quit_app` against allowlisted bundle IDs in Mode B; (4) ADMIN configuration writes (identity, macros, keys, security settings, pairing, OTA). Every item routes through the Chapter 5 command ledger or the ADMIN-authenticated Web UI, so each privileged act is authenticated, role-checked, rate-limited, logged with a correlation ID, and terminally adjudicated by the ESP32. Acceptance implication: an implementation MUST pass a negative test in which a valid ADMIN key requests each prohibited capability above and receives 404 `not_found` — the endpoints must not exist, not merely refuse.
