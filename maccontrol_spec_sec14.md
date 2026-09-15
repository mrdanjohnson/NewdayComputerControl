## 14. User Interfaces

MacControl ships two operator-facing user interfaces with disjoint configuration authority: the ESP32 Web UI, served by the MacControl Endpoint itself, and the MacControlAgent (MCA) UI, a local macOS preferences surface. Per the ownership split of Chapter 2.3, each UI configures only the component it runs on; neither UI MAY expose a control that writes configuration owned by the other. Both UIs are consumers of the same authority model as any API client: the ESP32 Web UI is bound to the ADMIN role, and the MCA UI writes only agent-local policy. Neither UI contains any inference-driven behavior — every page renders stored configuration, command ledger records, or status cache fields exactly as the ESP32 serves them.

The responsibility matrix below is normative and supersedes any single-dashboard reading of the PRD:

| Configuration or display area | ESP32 Web UI | MCA UI |
|---|---|---|
| Dashboard and status display | Owns (renders status cache, Chapter 7) | — |
| Device identity, hostname, mDNS naming | Owns | — |
| Macro editor and shortcut trigger bindings | Owns | — |
| Monitored application registry (Chapter 11.1) | Owns (Applications page) | — |
| Pairing ceremony | Opens time-boxed pairing window | Completes ceremony, shows pairing state |
| Controller credentials and RBAC keys (READ/CONTROL/ADMIN) | Owns | — |
| Logs | Endpoint ring-buffer log (Chapter 15) | Local agent log view (read-only) |
| OTA update control | Owns (ADMIN) | — |
| Command enablement toggles | — | Owns |
| Response endpoints (transport address, poll interval) | — | Owns |
| Application allowlist (bundle IDs) | — | Owns |

The pairing row is the only shared area, and it is shared by protocol design rather than by duplicated control: the ESP32 opens the window, the MCA completes the ceremony, and exactly one active pairing results (Chapter 3). For implementers, the matrix implies a concrete test: for every row, an attempted write through the non-owning UI — or through the owning UI of the other component — MUST be impossible because no such control exists, not merely disabled.

### 14.1 ESP32 Web UI

#### 14.1.1 Define dashboard, device configuration, macro editor, shortcut trigger setup, applications registry, pairing control, security, logs, and OTA pages

The ESP32 Web UI MUST provide nine pages, served directly by the endpoint with no external dependency: Dashboard, Device, Macros, Triggers, Applications, Pairing, Security, Logs, and OTA. Dashboard renders `GET /api/v1/status` verbatim, including per-field provenance and freshness (Section 14.3). Device edits identity fields. Macros edits the Chapter 10 macro model with validation identical to the REST schema; Triggers binds HTTP/Web-UI/GPIO inputs to `macro_id`. Applications edits the Chapter 11.1 monitored application registry — the ESP32-owned list of `bundle_id` entries with `monitor` and `control_enabled` flags — and surfaces reconciliation warnings (`registry_not_allowlisted`, `allowlist_not_registered`) against the latest MCA `capability_report`; it MUST NOT edit the MCA-owned application allowlist, which lives only in the MCA UI. Pairing opens and closes the pairing window and shows pairing state. Security manages API keys, role scopes, and the admin password. Logs pages the ring buffer. OTA uploads signed firmware and reports rollback status. Every mutating page MUST surface the deterministic error envelope of Chapter 12 unchanged; client-side validation MAY pre-check but MUST NOT replace server validation.

```
+----------------------------------------------------------+
| MacControl  [ProPresenter Mac]          mode: B (paired) |
| Dashboard | Device | Macros | Triggers | Pairing | ...   |
+----------------------------------------------------------+
| CONNECTION              MAC (agent-reported)             |
|  usb      true   fresh    state   awake        fresh     |
|  network  true   fresh    locked  false        fresh     |
|  agent    true   fresh    user    production   fresh     |
| APPLICATIONS              LAST COMMAND                   |
|  ProPresenter running     restart -> completed (verified)|
+----------------------------------------------------------+
```

Each dashboard cell carries its freshness qualifier from the cache; the wireframe's parenthetical "(verified)" is governed by Section 14.3 and is prohibited for any unverified terminal state.

### 14.2 MCA UI

#### 14.2.1 Define pairing, command configuration, response endpoint setup, application allowlist, and connection status pages

The MCA UI MUST provide five views matching the Chapter 9.2 scope exactly: Pairing, Commands, Endpoints, Allowlist, and Connection/Log. It MUST NOT display or edit macros, triggers, identity, controller credentials, or OTA, and it MUST NOT present command outcomes as verdicts — the MCA observes evidence, never terminal state.

```
+----------------------------------------------------------+
| MacControlAgent                          macOS 15.2      |
| Pairing | Commands | Endpoints | Allowlist | Connection  |
+----------------------------------------------------------+
| Pairing status:  ACTIVE                                  |
| Endpoint:  maccontrol-01.local (mDNS)                    |
| Session:   ACTIVE   heartbeat 5s   last ack 2s ago       |
| [ Delete local pairing ]                                 |
+----------------------------------------------------------+
```

The MCA UI's only pairing-destructive control is **Delete local pairing**, which erases the agent's stored credential and returns the MCA to the `unpaired` state. It MUST NOT expose any control that revokes the ESP32-side pairing record or reopens the pairing window: revocation and window control are ESP32 Web UI functions (Chapter 3.2), so re-pairing after a local delete always requires an ADMIN to open a new ceremony on the endpoint.

Command enablement defaults remain fail-closed (mutating actions off); the Allowlist view matches bundle IDs, never display names; and the Connection view shows session state and close-code reasons verbatim from the Chapter 4 transport.

### 14.3 Status display rules

#### 14.3.1 Require UI to distinguish verified, unverified, stale, expected-offline, and unknown states without using completion language for unverified commands

Both UIs MUST render the five provenance/freshness states distinctly, using the muted academic palette (dark body text on neutral backgrounds) and the exact labels below; color MUST always be redundant with text so state is legible in monochrome:

| Display state | Cache/ledger input | Required label | Prohibited language | Color (academic palette) |
|---|---|---|---|---|
| Verified | terminal `completed` in Mode B with MCA evidence | "Verified" | — | `#4A6FA5` |
| Unverified | Mode A terminal, `unconfirmed` with `result = "hid_only"` | "Unverified — HID only" | "completed", "done", "successful" | `#7A8B99` |
| Stale | field freshness `stale` | "Last seen {age} ago" + stale badge | presenting value as current | `#8BA3C7` |
| Expected-offline | freshness `expected_offline` | "Expected offline (power window)" | "error", "lost" | `#6B8CBB` |
| Unknown | source `unknown` / `value: null` | "No data" | fabricated defaults (e.g., rendering null as `false`) | `#2E4A62` |

The language rule is the enforcement point of the product's core invariant: Mode A commands terminate `unconfirmed` (Chapter 5.2.2), so any UI string implying completion for an unverified command is a specification violation regardless of visual treatment. Stale values MUST be shown with age, not hidden, because a labeled stale fact is more actionable than an absent one (Chapter 7.2.2). Acceptance implication: a UI test harness drives the five input conditions into the status cache and asserts the five distinct renderings, and greps all shipped strings to confirm no completion vocabulary appears on the unverified path.
