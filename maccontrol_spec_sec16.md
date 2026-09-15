## 16. MVP Definition and Acceptance Tests

The minimum viable product is defined by two milestones that together prove the authority model end to end. Milestone 1 proves Mode A usefulness without any MacControlAgent (MCA): deterministic human-interface-device (HID) dispatch with honestly labeled, unverified terminal states. Milestone 2 proves Mode B: MCA evidence closes the verification loop, and only then may the ESP32 record `completed`. Every acceptance test is deterministic, independently executable, and observable from the controller-facing REST surface, the ledger record, or the USB bus; each traces to the chapters defining the mechanism under test. A test MUST run against a clean installation unless its preconditions state otherwise; a milestone is complete only when all its tests pass twice consecutively with no code change between runs.

Tests are numbered AT-01 through AT-12. "Ledger check" in an expected result means `GET /api/v1/commands/{command_id}` returns a record whose `state`, `result`, and `error_code` fields match exactly; partial matches are failures.

### 16.1 Milestone 1: agentless HID endpoint

#### 16.1.1 Prove mDNS reachability, system commands, macro creation/execution, persistence, and hid_only/unconfirmed semantics

Milestone 1 MUST be executed with no MCA installed and no pairing record on the endpoint, so that every terminal state is produced by Mode A rules alone.

| ID | Test | Preconditions | Steps | Expected result | Traces |
|----|------|---------------|-------|-----------------|--------|
| AT-01 | Discovery and reachability | Endpoint on test LAN; READ-scope API key configured per Chapter 13.1 | Browse mDNS for `_maccontrol._tcp.local`; resolve hostname; `GET /api/v1/status` and `GET /api/v1/capabilities`, each with header `Authorization: Bearer <READ key>`; repeat both requests without the header | Advertisement resolves within 5 s; authenticated status document returns 200 with provenance metadata and `connection.agent.value` `false` (source `esp32_direct`); authenticated capabilities report mode `A` with `paired: false`; unauthenticated requests return 401 `unauthorized` | Ch. 3.3, 7.1, 12.1, 13.1 |
| AT-02 | Mode A system command | AT-01 passed; CONTROL-scope API key configured | `POST /api/v1/commands` with `lock` and header `Authorization: Bearer <CONTROL key>`; repeat with the READ key; poll record to terminal | 202 with `command_id`; USB HID report observed on target; terminal state `unconfirmed` with `result = "hid_only"`, never `completed`; READ-key submission rejected with 403 `forbidden` | Ch. 4.1, 5.2.2, 12.2, 13.1 |
| AT-03 | Macro creation and execution | ADMIN key configured | Create a 3-step macro (combo, delay, text) via Web UI/API; bind to HTTP trigger; dispatch | Ledger record reaches `unconfirmed`/`hid_only`; step timing within Chapter 10 bounds; keystrokes observed on target in order | Ch. 10, 5.2.2 |
| AT-04 | Persistence across reboot | AT-03 passed | Power-cycle the endpoint | Identity, macros, trigger bindings, and API keys load from flash; ledger boot reconciliation resolves any in-flight record to `failed`/`esp32_restarted` | Ch. 5.1.1, 15.1 |
| AT-05 | Capabilities honesty in Mode A | AT-01 passed; READ and CONTROL keys configured | `GET /api/v1/capabilities` with the READ key; submit `app_launch` with the CONTROL key | Every command type is present in `commands` and reports `verified: false`; `app_launch` reports `available: false` and submission fails with the deterministic error envelope (409 `agent_not_paired`) | Ch. 12.3, 5.2.2, 17.1 |

The consequence of this table is architectural: Milestone 1 ships a complete product — deterministic dispatch, honest status — without a single line of macOS code. AT-02 and AT-03 are the core invariant tests: a build that ever records `completed` in Mode A fails regardless of observed behavior on the Mac, because Chapter 5.2.2 confines Mode A to unverified terminal states. AT-05 forbids capability advertisement drift, so controllers can render Mode A UIs directly from the capabilities document.

### 16.2 Milestone 2: agent-enhanced verification

#### 16.2.1 Prove pairing, verified status, verified restart/wake, expected-offline behavior, allowlist enforcement, RBAC, reconnect, fallback, provenance, idempotency, logging, and OTA

Milestone 2 adds the MCA and MUST demonstrate that evidence — and only evidence — advances commands to `completed`.

| ID | Test | Preconditions | Steps | Expected result | Traces |
|----|------|---------------|-------|-----------------|--------|
| AT-06 | Pairing ceremony | MCA installed, unpaired | Start pairing in ESP32 Web UI; complete in MCA UI within the time box | Exactly one stored pairing; MCA WebSocket connects with `agent_hello`/`hello_ack`; late second pairing attempt rejected | Ch. 3.2, 4.2.1 |
| AT-07 | Verified lock | AT-06 passed | Submit `lock` | MCA emits `screen_lock_changed(locked)` within the 15 s deadline; ESP32 records `completed`/`lock_confirmed`; verdict recorded by ESP32, not agent | Ch. 5.2, 5.3.1, 6.2, 2.1 |
| AT-08 | Verified restart and wake | AT-06 passed | Submit `restart`; wait | `agent_goodbye` or expected offline inside restart window; new-session `agent_hello` with changed `boot_id` plus initial burst reporting `system_state=awake` inside deadline; terminal `completed`/`restart_confirmed`; submit `sleep` then wake target; wake completes only on a post-dispatch new MCA session whose initial status burst reports `awake` | Ch. 8, 5.3 |
| AT-09 | Expected-offline provenance | AT-06 passed | Submit `sleep`; `GET /api/v1/status` during window | Agent-derived fields show `expected_offline`, not `stale`; after window expiry without hello they degrade to `stale` per TTL table | Ch. 7.2.2, 8.1 |
| AT-10 | Allowlist and RBAC enforcement | Allowlist contains one bundle ID | Launch allowlisted app; launch non-allowlisted app; submit CONTROL-scope request for ADMIN write | Allowlisted launch completes via correlated `command_ack(action=launch_app)` plus matching `application_started(bundle_id)`; non-allowlisted fails with deterministic error; RBAC matrix enforced with 403 for scope violations | Ch. 11.2, 13.1, 13.3 |
| AT-11 | Reconnect and polling fallback | AT-06 passed | Kill MCA WebSocket; block WebSocket port; restart MCA | Heartbeat loss marks agent stale at 15 s, offline at 30 s; reconnect follows 1/2/4/8/30 s backoff with 0–20% jitter; MCA falls back to `POST /agent/v1/events` + `GET /agent/v1/commands/pending` with identical semantics | Ch. 4.2.2, 4.3 |
| AT-12 | Idempotency, logging, and OTA | ADMIN key | Replay same `Idempotency-Key` submission; inspect logs; perform signed OTA | Replay returns original `command_id` with no duplicate dispatch; every transition carries correlation ID in ring-buffer log retrievable via API; OTA applies on dual partitions, rolls back on bad image, and is refused without ADMIN | Ch. 5.1.1, 12.2, 15.2, 15.3 |

Two cross-cutting rules govern this table. First, AT-08 and AT-09 encode the positive-evidence model of Chapter 8: an expected-offline window is itself evidence, so `restart` and `sleep` can complete without a goodbye frame, but only inside their declared windows; offline outside a window is a fault, not evidence. Second, AT-11 is the determinism gate for transport: backoff intervals MUST be measured from connection-failure timestamps in the log, and the fallback path MUST produce ledger outcomes indistinguishable from the WebSocket path — which is what makes external AI clients ordinary HTTP consumers with no special subsystem.

### 16.3 Explicit exclusions

#### 16.3.1 Exclude LLM/natural-language control, arbitrary shell execution, conditional macros, mouse control, and central manager from MVP

The following capabilities are excluded from the MVP by construction, not merely unimplemented. Acceptance requires negative evidence: where a row says an endpoint must not exist, a valid ADMIN-key request MUST return 404 `not_found` (Chapter 13.3), not 403 or a stub.

| Excluded capability | Reason | Enforcement point | Acceptance implication |
|---------------------|--------|-------------------|------------------------|
| LLM, natural-language parsing, planner, or inference-driven behavior | Violates the determinism invariant (Ch. 1.1.2) | No inference subsystem exists in any component; external AI clients are ordinary REST consumers (Ch. 12) | Source and configuration audit finds no inference dependency; all AT-01–AT-12 pass without any such component |
| Arbitrary shell, AppleScript, or path execution | Closed action invariant (Ch. 13.3) | Dispatch schema carries only action enum + `bundle_id`; MCA rejects frames with unexpected keys (Ch. 11.2) | Negative test: every generic-execution route returns 404; malformed dispatch frames rejected as schema violations |
| Conditional/branching macros | Deterministic timing and auditability (Ch. 10.1.2) | Macro model is an ordered step list with no condition or jump step type | Schema validation rejects any macro document containing conditional fields |
| Mouse and scroll HID control | Scope bound of the macro engine (Ch. 10.1.2) | HID report descriptor exposes keyboard only | USB descriptor inspection shows no mouse/scroll interface; mouse action types rejected |
| Central manager or cloud coordination | ESP32 is final source of truth (Ch. 2.1) | Discovery is mDNS-only; no outbound manager registration exists | Network capture during AT-01–AT-12 shows no traffic to any coordination service |

These exclusions are normative: a build that passes AT-01 through AT-12 but implements any excluded capability does not conform to the MVP. The table also bounds Chapter 17's development phases — excluded items may return as future work only by amending the authority model, never by silent extension of the action set.
