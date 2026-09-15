## 7. Status Cache, Provenance, and Freshness

This chapter defines the status cache: the MacControl Endpoint's (ESP32) authoritative representation of the controlled Mac's most recent known state. The cache exists because the ESP32 is the final source of truth for controller-facing state: a controller MUST be able to learn everything the system knows — and how well it knows it — without the endpoint ever blocking on the Mac, the MacControlAgent (MCA), or the network. Two rules organize the chapter: every field carries explicit provenance and freshness metadata (Section 7.2.1), and staleness and expected-offline conditions are represented as data rather than inferred by the client (Section 7.2.2).

### 7.1 ESP32-owned status document

#### 7.1.1 Serve GET /api/v1/status only from the ESP32 cache with per-field provenance and freshness metadata

`GET /api/v1/status` (READ role) MUST be answered exclusively from an ESP32-resident cache document. The handler MUST NOT perform a synchronous MCA round-trip, network probe, or ledger scan at request time; cache updates arrive only from asynchronous inputs (MCA events per Chapter 6, probe results, ESP32 self-observation, and timer-driven freshness transitions), and the handler MUST respond within 50 ms. The cache SHOULD persist across reboots alongside the command ledger; on boot, every `agent_reported` field is restored with its original `observed_at` and its freshness recomputed, so a restarted endpoint never presents aged data as fresh.

Every leaf field is a five-member tuple. A field never yet observed MUST be present with `value: null`, `source: "unknown"`, `observed_at: null`, and `freshness: "unknown"` — omission is prohibited, because a missing key is indistinguishable from a schema error.

```json
{
  "generated_at": "2025-01-14T09:41:12Z",
  "cache_epoch": 42,
  "device": {
    "name":     { "value": "ProPresenter Mac", "source": "esp32_direct", "observed_at": "2025-01-10T08:00:00Z", "ttl_s": null, "freshness": "fresh" },
    "hostname": { "value": "maccontrol-01", "source": "esp32_direct", "observed_at": "2025-01-10T08:00:00Z", "ttl_s": null, "freshness": "fresh" }
  },
  "connection": {
    "usb":     { "value": true,  "source": "esp32_direct",   "observed_at": "2025-01-14T09:41:12Z", "ttl_s": null, "freshness": "fresh" },
    "network": { "value": true,  "source": "network_probe",  "observed_at": "2025-01-14T09:41:07Z", "ttl_s": 30,   "freshness": "fresh" },
    "agent":   { "value": true,  "source": "esp32_direct",   "observed_at": "2025-01-14T09:41:10Z", "ttl_s": null, "freshness": "fresh" }
  },
  "mac": {
    "state":           { "value": "awake", "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
    "locked":          { "value": false,   "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
    "user_logged_in":  { "value": true,    "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
    "user":            { "value": "production", "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
    "boot_id":         { "value": "b_3F8A11", "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": null, "freshness": "fresh" }
  },
  "applications": {
    "com.renewedvision.propresenter": {
      "state": { "value": "running", "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
      "pid":   { "value": 812,       "source": "agent_reported", "observed_at": "2025-01-14T09:41:10Z", "ttl_s": 15, "freshness": "fresh" },
      "warning": { "value": null, "source": "unknown", "observed_at": null, "ttl_s": null, "freshness": "unknown" }
    }
  }
}
```

The schema preserves the PRD's four top-level groups (`device`, `connection`, `mac`, `applications`) and adds two cache-level fields: `generated_at`, the ESP32's clock at serialization, and `cache_epoch`, a monotonically increasing integer bumped on every mutation so controllers can cheaply detect change. Each `applications` entry is an object with `state` and `pid` provenance tuples plus a `warning` provenance tuple (Chapter 11.1.1) — no leaf in the cache escapes the tuple invariant: `warning.value` is null or one of the closed reconciliation warning strings, with `source: "unknown"`, `observed_at: null`, and `freshness: "unknown"` while null, and `source: "agent_reported"` with a real `observed_at` when registry reconciliation against the latest MCA `capability_report` has raised it; `warning.ttl_s` is always null because warnings clear on the next reconciliation, not by aging. `state` is the enum `not_running`/`running`/`launching`/`quitting`; a boolean `running` flag MUST NOT be used anywhere in the cache schema. Hostname values are stored and served as bare DNS labels; the `.local` suffix is applied only for mDNS advertisement and serialized FQDN contexts (Chapter 3.1). `mac.boot_id` carries the boot identity provenance used by the restart predicate (Chapter 8.2.1) and MUST persist across reboots so the comparison survives an endpoint restart during an open window. Updates MUST be atomic per source event: one MCA status report refreshes all fields it carries with a single shared `observed_at`, preventing torn reads in which `mac.state` and `mac.locked` describe different moments. All `observed_at` values derive from the ESP32's clock; MCA-supplied timestamps are evidence metadata (Chapter 6) and MUST NOT set `observed_at`, because the ESP32 only vouches for when it accepted the observation.

### 7.2 Provenance rules

#### 7.2.1 Define esp32_direct, network_probe, agent_reported, inferred, and unknown sources with precedence rules

| Source | Rank | Definition | Example fields |
|---|---|---|---|
| `esp32_direct` | 1 (highest) | Observed by the endpoint itself: configuration, USB link state, MCA session liveness | `device.*`, `connection.usb`, `connection.agent` |
| `network_probe` | 2 | Asynchronous reachability probes (ICMP/TCP) issued by the ESP32 | `connection.network` |
| `agent_reported` | 3 | MCA status reports and events accepted per the Chapter 2 evidence rules | `mac.*`, `applications.*` |
| `inferred` | 4 | Derived deterministically from higher-ranked observations (e.g., LAN-reachable agent session implies `connection.network`) | derived `connection.*` |
| `unknown` | 5 (lowest) | No observation has ever been accepted for the field | any field pre-first-report |

Replacement precedence is total and deterministic: a stored value MUST be replaced by an observation of equal or higher rank, and by a lower-ranked observation only when the stored value's freshness is `stale` or `unknown`. This prevents two common implementation errors: an `inferred` value must never overwrite a fresh `agent_reported` fact (a live session does not prove the screen is unlocked), and a probe result must never masquerade as agent evidence (rank 2 cannot produce `mac.state`). Ranks 1–2 describe only the transport surroundings of the Mac; every claim *about* the Mac itself is rank 3 or absent — the cache-level restatement of the rule that unverified is never completed.

#### 7.2.2 Mark agent-derived fields stale after heartbeat loss and expected_offline during declared power windows

Freshness is a pure function of field age, TTL, and window state, evaluated on every mutation and at least once per second by timer: if a declared expected-offline window is open for the field's group, freshness is `expected_offline`; else if `ttl_s` is non-null and `now − observed_at > ttl_s`, freshness is `stale`; else `fresh`. Values MUST NOT be erased on staleness — a stale value with provenance is more useful than none — but staleness MUST propagate: a controller reading `stale` agent-derived fields MUST treat them as history, and Mode B commands in `confirming` still terminate per Chapter 5 regardless of what the cache shows.

| Field group | Primary source | `ttl_s` | Stale trigger | Expected-offline trigger |
|---|---|---|---|---|
| `device.*` | `esp32_direct` (config) | — | never | never |
| `connection.usb`, `connection.agent` | `esp32_direct` (event-driven) | — | never; value flips on the event itself | `connection.agent` value set `false`, freshness `expected_offline` |
| `connection.network` | `network_probe` | 30 (probe interval, default) | probe age > 30 s | probe skipped inside window, last value kept, freshness `expected_offline` |
| `mac.*` (except `mac.boot_id`) | `agent_reported` | 15 (`stale_threshold`, Chapter 4) | no MCA frame for 15 s | open `expected_offline_window` (Chapter 5.3.2) |
| `mac.boot_id` | `agent_reported` | — | never; refreshed on every `agent_hello` and retained so the restart predicate survives reboots and windows | value retained through the window; freshness `expected_offline` |
| `applications.*` | `agent_reported` | 15 | as above | as above |

The table shows that only `agent_reported` and `network_probe` groups carry TTLs at all: `esp32_direct` fields are event-driven facts the endpoint observes directly, so their values change atomically with the event and never silently expire. The 15 s agent TTL is pinned to the Chapter 4 `stale_threshold`, not chosen independently, so transport liveness and cache freshness can never disagree. Implementers should also note the asymmetry in the expected-offline column: during a declared window, `connection.agent` is *set* false with `expected_offline` freshness, while `mac.*` and `applications.*` merely freeze — the endpoint knows the agent left, but makes no claim about the Mac's applications beyond the last report.

**Worked example 1 — healthy Mode B.** The MCA heartbeats every 5 s and reports status on change. At `t = 09:41:10Z` a report sets all `mac.*` and `applications.*` fields with one shared `observed_at`; a request at `09:41:12Z` returns them `fresh`. Because any frame resets the liveness clock (Chapter 4.2.2), the 15 s TTL is never reached while the session is ACTIVE, and the document changes only when the Mac actually changes.

**Worked example 2 — unannounced heartbeat loss.** The MCA process dies at `t = 0` with no `agent_goodbye` and no open window. At `t = 15 s` every `agent_reported` field is marked `stale` with values preserved; at `t = 30 s` the session reaches OFFLINE (Chapter 4) and `connection.agent.value` flips to `false` with freshness `fresh` — the endpoint directly observed the loss, so this negative fact is the freshest data in the document. Any `confirming` command with no open window terminates `unconfirmed` / `evidence_lost` (Chapter 5.2.1). A controller reading at `t = 45 s` can state exactly: the Mac was awake and unlocked as of 45 s ago, the agent is now down, and nothing further is known.

**Worked example 3 — declared sleep window.** A `sleep` command dispatches at `t = 0` with `expected_offline_window = {open_after_s: 3, close_after_s: 60}`. The MCA sends `agent_goodbye` at `t = 4 s`; the ESP32 opens the window and marks all `agent_reported` fields `expected_offline` rather than letting them age into `stale` at `t = 15 s`. The distinction is load-bearing: `expected_offline` is positive evidence advancing the command toward `completed`, whereas `stale` would be an ambiguous fault. If the window closes at `t = 60 s` without reconnect, the fields transition to `stale`; if the MCA reconnects, its first status report refreshes all groups atomically and freshness returns to `fresh` in a single `cache_epoch` increment.
