## 17. Deterministic Integration and Development Phases

### 17.1 Deterministic machine-readable integration

MacControl's integration surface is deliberately small and fully deterministic. There is no LLM, natural-language parser, planner, or inference-driven behavior anywhere in the product (Chapter 1.1.2), and no subsystem is reserved for external AI clients: if an external AI tool ever consumes the API, it does so as an ordinary deterministic HTTP client with a standard RBAC credential (Chapter 13), identical in every respect to a script. The integration contracts defined here exist for the consumers the product actually targets — Q-SYS modules, Bitfocus Companion, operator browsers, and shell scripts — and the former PRD "Phase 7 (AI)" deliverables that remain useful (OpenAPI, machine-readable capabilities, discovery) are folded into this deterministic contract layer.

#### 17.1.1 /capabilities and /openapi.json as integration contracts

Two documents constitute the complete machine-readable contract a controller needs to integrate without human-written glue code:

| Contract | Path | Role | Mode A | Content and guarantee |
|---|---|---|---|---|
| Capabilities document | `GET /api/v1/capabilities` | READ | full | Live, mode-aware enumeration of dispatchable commands, `verified` reachability, deadlines, macros, and agent state (Chapter 12.3.1) |
| API schema | `GET /api/v1/openapi.json` | READ | full | Static OpenAPI 3.1 document covering every Chapter 12 path, schema, and error code; byte-identical to the implementation's routes |
| Discovery record | mDNS `_maccontrol._tcp` | none | full | Deterministic hostname/TXT advertisement so controllers locate the endpoint without a central manager (Chapter 3.3) |

The three contracts are complementary and none is optional. Discovery answers *where* the endpoint is, `/openapi.json` answers *how* to call it, and `/capabilities` answers *what this specific endpoint can do right now*. Because `/capabilities` is generated live from pairing state, session liveness, and the latest `capability_report`, a controller that polls it can degrade gracefully: when the MCA session crosses the 30 s offline threshold, every agent-dependent command's `verified` flag collapses to `false`, and a well-behaved Q-SYS or Companion module SHOULD surface that as "unverified mode" rather than failing commands outright. Implementers MUST treat `/openapi.json` as normative for request shape and MUST treat `/capabilities` as normative for dispatch decisions; a request that is schema-valid but contradicts the live capabilities document is rejected deterministically with the Chapter 12 409 family.

Because these contracts are the whole integration story, consumer obligations are minimal and fixed. Integration consumers SHOULD be issued READ or CONTROL credentials only (Chapter 13): Q-SYS and Companion modules need CONTROL to submit commands and READ to poll status, and no integration consumer requires ADMIN, which remains reserved for human operators performing macro, OTA, and security configuration. Consumers MUST pin the `/api/v1` version prefix, MUST treat HTTP 400/409 as permanent and never retry them unchanged (Chapter 12.3.1), and MUST poll `GET /api/v1/commands/{id}` for terminal verdicts rather than assuming synchronous execution, since every command-producing request returns 202 and resolves through the ledger (Chapter 5.1.2). A Companion or Q-SYS module that follows these rules and drives itself from the two documents requires no per-endpoint custom code beyond request templating.

The capabilities document MUST validate against the following JSON Schema, which fixes the field names used throughout Chapters 5, 11, and 12:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "MacControlCapabilities",
  "type": "object",
  "required": ["api_version", "mode", "capability_level", "device", "commands", "agent"],
  "properties": {
    "api_version": { "const": "v1" },
    "mode": { "enum": ["A", "B"] },
    "capability_level": { "enum": ["L1", "L2", "L3"] },
    "device": {
      "type": "object",
      "required": ["name", "hostname"],
      "properties": {
        "name": { "type": "string", "maxLength": 32 },
        "hostname": { "type": "string", "maxLength": 63, "pattern": "^[a-z0-9]([a-z0-9-]{0,55}[a-z0-9])?\\.local$", "description": "Collision-resolved bare DNS label (1-57 chars, lowercase alnum/hyphen, start/end alnum, per Chapter 3.1) plus .local" }
      }
    },
    "commands": {
      "type": "object",
      "required": ["wake", "sleep", "restart", "shutdown", "lock", "macro_execute", "app_launch", "app_quit"],
      "propertyNames": { "enum": ["wake", "sleep", "restart", "shutdown", "lock", "macro_execute", "app_launch", "app_quit"] },
      "additionalProperties": {
        "type": "object",
        "required": ["available", "verified"],
        "properties": {
          "available": { "type": "boolean" },
          "verified": { "type": "boolean" },
          "deadline_s": { "type": "integer", "minimum": 1 },
          "macro_ids": { "type": "array", "items": { "type": "string" } },
          "apps": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["bundle_id", "display_name"],
              "properties": {
                "bundle_id": { "type": "string" },
                "display_name": { "type": "string", "maxLength": 64 }
              }
            }
          }
        }
      }
    },
    "agent": {
      "type": "object",
      "required": ["paired", "connected", "enabled_commands", "allowlisted_apps"],
      "properties": {
        "paired": { "type": "boolean" },
        "connected": { "type": "boolean" },
        "enabled_commands": { "type": "array", "items": { "type": "string" } },
        "allowlisted_apps": { "type": "array", "items": { "type": "string" }, "description": "Bundle IDs extracted from the latest MCA capability_report allowlisted_apps entries" }
      }
    }
  },
  "allOf": [
    {
      "if": { "properties": { "mode": { "const": "A" } }, "required": ["mode"] },
      "then": {
        "properties": {
          "capability_level": { "const": "L1" },
          "commands": {
            "additionalProperties": {
              "properties": { "verified": { "const": false } }
            }
          },
          "agent": {
            "properties": {
              "paired": { "const": false },
              "connected": { "const": false }
            }
          }
        }
      }
    },
    {
      "if": {
        "properties": {
          "agent": {
            "anyOf": [
              { "properties": { "paired": { "const": false } }, "required": ["paired"] },
              { "properties": { "connected": { "const": false } }, "required": ["connected"] }
            ]
          }
        },
        "required": ["agent"]
      },
      "then": {
        "properties": {
          "commands": {
            "additionalProperties": {
              "properties": { "verified": { "const": false } }
            }
          }
        }
      }
    }
  ]
}
```

Three invariants are acceptance-testable directly from this schema. First, the `commands` object is complete by construction: it MUST carry exactly one entry for every closed controller command type of Chapter 12.2.1 — `wake`, `sleep`, `restart`, `shutdown`, `lock`, `macro_execute`, `app_launch`, and `app_quit` — each with `available` and `verified` booleans, so a conformance client never has to infer the absence of a command from a missing key. Second, `verified: true` implies `mode: "B"` with `agent.paired: true` and `agent.connected: true`, because Chapter 5.2.2 confines Mode A to `unconfirmed`/`hid_only` terminal states and a disconnected agent supplies no evidence; the schema's `if`/`then` constraints make this machine-enforceable — in Mode A every command entry MUST carry `verified: false`, `capability_level` MUST be `L1`, and the agent object MUST report `paired: false, connected: false`, and in either mode an agent object reporting `paired: false` or `connected: false` forces every `verified` flag to `false` — so a conformance client can reject any violating document deterministically at validation time. Third, every entry in `commands` with `available: true` MUST be submittable per `/openapi.json` and MUST reach a terminal state within `deadline_s`; a conformance client driven purely by these two documents MUST never observe `completed` for a command reported `verified: false` (Chapter 12.3.1).

### 17.2 Development phases

#### 17.2.1 Phase deliverables, dependencies, risks, and exit criteria

The former PRD Phase 7 (AI) is removed; its reusable artifacts (OpenAPI, capabilities, discovery) are delivered in Phase 3 as deterministic integration contracts. Development proceeds in six phases, each gated by the Chapter 16 acceptance tests (AT-01 through AT-12); a phase is complete only when its exit criteria pass on the target hardware.

| Phase | Deliverables | Dependencies | Primary risks | Exit criteria (Chapter 16) |
|---|---|---|---|---|
| 1. HID control | ESP32-S3 USB HID keyboard, HTTP server, `wake`/`sleep`/`restart`/`shutdown`/`lock`, command ledger with 202 + `command_id` | Hardware, USB descriptor validation | HID enumeration variance across Macs; flash wear on ledger | AT-01–AT-02: dispatch, ledger persistence, Mode A `unconfirmed`/`hid_only` semantics |
| 2. Web UI, macros, triggers | ESP32 Web UI: identity, dashboard, macro editor, shortcut triggers, persistent storage | Phase 1 | Macro interpreter correctness; storage capacity | AT-03–AT-04: macro create/execute, state survives reboot |
| 3. Identity, security, integration contracts | mDNS discovery, device identity, RBAC (READ/CONTROL/ADMIN), `/capabilities`, `/openapi.json`, logging | Phases 1–2 | mDNS behavior on managed networks; schema drift between contract and firmware | AT-05: capabilities honesty; **Milestone 1 gate = AT-01 through AT-05** |
| 4. MacControlAgent | MCA process, pairing ceremony, WebSocket transport, heartbeat 5 s / stale 15 s / offline 30 s, capability_report | Phase 3 | macOS permission prompts; reconnect backoff (1/2/4/8/30 s, 0–20% jitter) correctness | AT-06, AT-11: pairing, heartbeat liveness, reconnect and polling fallback |
| 5. Verified lifecycle | Verification predicates, expected-offline windows, verified restart/wake/lock, expected-offline provenance, idempotency | Phase 4 | False-positive verification; window timing on slow boots | AT-07–AT-09: verified lock, verified power transitions, expected-offline provenance |
| 6. Production hardening | Q-SYS/Companion modules, Ethernet, signed dual-partition OTA, allowlist and RBAC hardening, provenance surfacing | Phase 5 | OTA rollback reliability; third-party module maintenance | AT-10, AT-12: allowlist/RBAC enforcement, idempotency, logging, OTA recovery; **Milestone 2 gate = AT-06 through AT-12** |

The mapping is cumulative rather than siloed: Phase 3 completes Milestone 1 (agentless but integratable), and Phase 6 completes Milestone 2 (agent-enhanced verification). Dependencies are strictly ordered — verified lifecycle work (Phase 5) cannot begin before MCA transport liveness (Phase 4) exists, because every verification predicate consumes MCA evidence — while Phase 6 hardening MAY overlap Phase 5 for work that does not touch the command lifecycle. Any regression in an earlier phase's acceptance tests blocks declaration of the current phase's completion.

Two risk-handling rules apply across all phases. First, no phase may ship a partial command type: a command listed in the Chapter 12 closed enum is either fully implemented through ledger, dispatch, and terminal verdict, or absent from `/capabilities` and `/openapi.json` entirely, so integration consumers never discover a half-built action. Second, the deterministic parameter defaults fixed by earlier chapters (heartbeat 5 s, stale 15 s, offline 30 s, reconnect backoff 1/2/4/8/30 s with 0–20% jitter) are acceptance-test inputs, not tunables to be settled during hardening; changing them after Phase 4 invalidates Phase 5 window timing and MUST trigger re-execution of AT-06 through AT-12.
