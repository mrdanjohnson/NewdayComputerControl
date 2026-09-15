## 11. Application Monitoring and Control

Application visibility and control in MacControl are exercised only through two explicit, user-curated lists: the monitored application registry owned by the MacControl Endpoint (ESP32), and the application allowlist owned by MacControlAgent (MCA). There is no discovery of arbitrary processes, no path-based launch, and no general execution channel; the closed action set of Chapter 9.3.1 contains exactly two application actions — `launch_app` and `quit_app` — and both resolve exclusively through allowlisted bundle identifiers.

### 11.1 Monitored application registry

#### 11.1.1 Define user-marked monitored applications and reconcile ESP32 expected list with MCA allowlist

The registry is ESP32-owned flash configuration, edited only in the ESP32 Web UI (Chapter 2 ownership split). It declares which applications the operator expects the endpoint to track and, optionally, to control.

```json
{
  "registry_revision": 7,
  "apps": [
    {
      "bundle_id": "com.renewedvision.propresenter",
      "display_name": "ProPresenter",
      "monitor": true,
      "control_enabled": true
    }
  ]
}
```

`bundle_id` is the canonical key: reverse-DNS, unique, matched exactly and case-sensitively; per the Chapter 9.2 rule, matching by display name is prohibited because names are decorative and collision-prone. `display_name` (1–64 printable characters) is presentation-only and MUST NOT appear in any dispatch or evidence frame. Capacity is bounded at 32 entries; `registry_revision` increments on every mutation so controllers can detect change. An entry with `monitor: false` is retained but produces no status object; an entry with `control_enabled: false` may be observed but MUST reject `launch_app`/`quit_app` dispatches with `error_code = "app_control_disabled"`.

Reconciliation runs on every MCA `capability_report` (Chapter 6.2) and on every registry edit. The ESP32 computes the set difference between registry `bundle_id`s with `monitor: true` and the `bundle_id`s carried in the report's `allowlisted_apps` entries, and MUST surface both mismatch directions as provenance warnings rather than silent gaps:

- **Registered but not allowlisted** (`warning.value = "registry_not_allowlisted"`): the application keeps a status object with `source: "unknown"`, `value: null`, `freshness: "unknown"`, because the MCA will never report it. Control dispatches fail with `app_not_allowlisted`.
- **Allowlisted but not registered** (`warning.value = "allowlist_not_registered"`): the MCA may report and control the application, but the ESP32 MUST NOT create a status object for it, keeping the controller-facing surface limited to operator-marked applications.

Both warnings are written to the Chapter 15 event log and attached to the status document as provenance tuples until the next reconciliation clears them, so a controller can always distinguish "application absent" from "monitoring misconfigured".

The per-application status object lives in the status cache `applications` map (Chapter 7), keyed by `bundle_id`:

```json
{
  "applications": {
    "com.renewedvision.propresenter": {
      "state": { "value": "running", "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
      "pid":   { "value": 812,       "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
      "warning": { "value": null, "source": "unknown", "observed_at": null, "ttl_s": null, "freshness": "unknown" }
    }
  }
}
```

Like every cache leaf, `warning` is a provenance tuple, never a bare scalar: `value` is null or one of the two closed reconciliation strings, `source` is `unknown` while the value is null and `agent_reported` once reconciliation against an accepted `capability_report` raises it, and `ttl_s` is null because warnings are cleared by the next reconciliation rather than by aging.

| State | Class | Entered by | Exited by |
|---|---|---|---|
| `not_running` | stable | `application_exited` evidence; a `capability_report` `allowlisted_apps` entry reporting `state: "not_running"` (including the mandatory initial burst, Chapter 6.3) | `application_started` evidence |
| `running` | stable | `application_started` evidence | `application_exited` evidence |
| `launching` | transitional | MCA evidence correlated to the in-flight launch (e.g., `command_ack(action=launch_app)`); MAY additionally be projected at dispatch when the stored value is `stale`/`unknown` | `application_started` before deadline → `running`; deadline expiry → `not_running` with command `timed_out` |
| `quitting` | transitional | MCA evidence correlated to the in-flight quit (e.g., `command_ack(action=quit_app)`); MAY additionally be projected at dispatch when the stored value is `stale`/`unknown` | `application_exited` before deadline → `not_running`; deadline expiry → `running` with command `timed_out` |

The stable states carry provenance `agent_reported` and obey the Chapter 7 freshness rules (TTL 15 s, frozen as `expected_offline` inside declared windows). The transitional states are entered from MCA events with provenance `agent_reported`, or — only when the stored agent-derived value is `stale` or `unknown` — as ESP32 dispatch projections with provenance `inferred`: they derive from ledger state, not agent observation, so a controller can distinguish "the agent saw it running" from "the endpoint is awaiting evidence". Dispatch MUST NOT force `launching` or `quitting` over a fresh `agent_reported` value; per the Chapter 7.2.1 precedence rule, an `inferred` projection MUST NOT overwrite a fresh `agent_reported` value. When the transition is resolved by MCA events (`application_started`/`application_exited` with exact `bundle_id` match), the resulting state is `agent_reported` like any other MCA-derived value. A transitional state MUST resolve no later than the command's `deadline_at`; when the command terminates without resolving evidence, the state reverts to the last agent-reported value in the same cache mutation. In Mode A no agent evidence exists, so monitored applications remain `unknown` and transitional states are never entered — Mode A launches happen through HID macros (Chapter 10) and terminate `unconfirmed`.

### 11.2 Application control

#### 11.2.1 Define launch and quit only through allowlisted names and prohibit arbitrary shell execution as a closed invariant

Controller requests use the Chapter 5 command API with `type ∈ {"app_launch", "app_quit"}` and a single parameter, `bundle_id`. Validation is ordered and deterministic, and every failure it can detect is **pre-ledger** — rejected with HTTP 409 before any ledger record is created (Chapter 12.3.1): (0) if no MCA is paired (Mode A), the request MUST be rejected with 409 `agent_not_paired`; if an agent is paired but its session is not ACTIVE, the request MUST be rejected with 409 `agent_offline`; neither rejection creates a ledger record; (1) `bundle_id` MUST exist in the registry with `control_enabled: true`, else the request fails with `app_not_registered` or `app_control_disabled`; (2) `bundle_id` MUST appear in the most recent `capability_report.allowlisted_apps` and the action in `enabled_commands`, else the request fails with `app_not_allowlisted` or `command_disabled` (Chapter 6.2 closed error enum). Only a request that passes all checks is persisted to the ledger in `accepted` and dispatched with the three-key payload defined in Chapter 9.3.1 — two valid examples:

```json
{"action": "launch_app", "bundle_id": "com.renewedvision.propresenter", "command_id": "8F31A2C4"}
```

```json
{"action": "quit_app", "bundle_id": "com.renewedvision.propresenter", "command_id": "7AC41E9B"}
```

The command then terminates per the Chapter 5.3 predicates — `completed` only on correlated `command_ack` plus matching `application_started`/`application_exited` evidence (exact `bundle_id` match) before the deadline; `command_result(ok)` may report the local execution outcome but MUST NOT by itself complete the predicate.

The closed invariant is structural, not policy text: the dispatch payload carries only an action enum and a `bundle_id` — no path, argument string, or script field exists in the schema, so no conforming request can express arbitrary execution. The MCA MUST resolve the bundle identifier to an application locally via LaunchServices and MUST reject any frame containing unexpected keys as a schema violation (Chapter 6.1). There is no shell, AppleScript, or file-path execution action anywhere in the MCA action set, and acceptance testing (Chapter 16.2) MUST demonstrate that launches of non-allowlisted identifiers, disabled actions, and malformed frames each produce distinguishable deterministic failures.
