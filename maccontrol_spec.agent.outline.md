# MacControl Deterministic Architecture and MVP Specification

## 1. Scope, Conventions, and System Components (~600 words; 1 component diagram, 1 capability matrix)
### 1.1 Purpose and normative language
#### 1.1.1 Define the document as build-ready engineering guidance using MUST/SHOULD/MAY and deterministic acceptance criteria
#### 1.1.2 State that MacControl contains no LLM, natural-language parser, planner, or inference-driven behavior anywhere in the product
### 1.2 Component inventory
#### 1.2.1 Identify the four components: controller, ESP32-S3 endpoint, target Mac, and optional MacControlAgent
#### 1.2.2 Require all controllers to communicate only with the ESP32 REST/API surface and never directly with the MCA
### 1.3 Operating modes and capability levels
#### 1.3.1 Define Mode A as agentless HID control with sent-only or unconfirmed terminal states
#### 1.3.2 Define Mode B as agent-enhanced operation where MCA evidence completes the verification loop
#### 1.3.3 Define capability levels L1 HID, L2 agent visibility, and L3 verified automation with one testable criterion each

## 2. Authority Model and Responsibility Split (~800 words; 2 tables, 1 data-flow diagram)
### 2.1 ESP32 as final source of truth
#### 2.1.1 Assign command initiation, command ledger ownership, status cache publication, and terminal verdicts exclusively to the ESP32
#### 2.1.2 Classify MCA output as evidence that can advance a command but cannot independently mark it complete
### 2.2 MCA as loop-closing evidence provider
#### 2.2.1 Specify that MCA supplies hello/goodbye, heartbeat, system state, user state, application state, and command acknowledgements
#### 2.2.2 Require the ESP32 to reject unauthenticated, out-of-sequence, stale, or unpaired MCA evidence
### 2.3 UI ownership split
#### 2.3.1 Assign shortcut trigger setup, macros, device identity, controller credentials, and dashboard publication to the ESP32 Web UI
#### 2.3.2 Assign pairing initiation, command enablement, response endpoints, and application allowlist management to the MCA UI

## 3. Device Identity, Pairing, and Discovery (~700 words; 1 identity table, 1 pairing schema, 1 pairing state machine)
### 3.1 Endpoint identity
#### 3.1.1 Define device name, hostname, location, description, generated device ID, and mDNS naming rules
### 3.2 ESP32-MCA pairing
#### 3.2.1 Define a time-boxed pairing ceremony initiated from the ESP32 Web UI and completed in the MCA UI
#### 3.2.2 Define stored credentials, revocation, re-pairing, and exactly-one-pairing-per-activation rules
### 3.3 Discovery
#### 3.3.1 Define mDNS advertisement and deterministic controller discovery without any central manager dependency

## 4. Connectivity and Agent Transport (~850 words; 1 transport state machine, 3 parameter tables)
### 4.1 Controller transport
#### 4.1.1 Define REST/HTTP access to the ESP32 with versioned paths and deterministic error responses
### 4.2 MCA-to-ESP32 WebSocket
#### 4.2.1 Specify MCA-initiated connection, agent_hello, hello_ack, close codes, session assignment, and sequence numbers
#### 4.2.2 Define heartbeat every 5 seconds, stale after 15 seconds, offline after 30 seconds
### 4.3 Polling fallback
#### 4.3.1 Define POST /agent/v1/events and GET /agent/v1/commands/pending as semantically identical fallback transport
#### 4.3.2 Define deterministic reconnect backoff with 1/2/4/8/30-second caps and bounded jitter

## 5. Command Ledger and Deterministic Command Lifecycle (~1,200 words; 1 schema, 1 state machine, 1 transition table, 1 predicate/timeout table)
### 5.1 Command ledger
#### 5.1.1 Define a flash-backed append-only ledger with bounded capacity, FIFO eviction, idempotency keys, and boot reconciliation
#### 5.1.2 Require every mutating request to create a ledger record before dispatch and return 202 with command_id
### 5.2 Lifecycle states
#### 5.2.1 Define accepted, dispatched, confirming, completed, failed, timed_out, and unconfirmed with terminal-state rules
#### 5.2.2 Define Mode A terminal behavior as unconfirmed with result=hid_only and prohibit completed without MCA evidence
### 5.3 Verification predicates
#### 5.3.1 Define per-command predicates and default deadlines for wake, sleep, restart, shutdown, lock, macro execution, app launch, and app quit
#### 5.3.2 Define expected-offline windows as positive evidence for sleep, restart, and shutdown when they occur inside declared windows

## 6. MCA Protocol, Events, and Status Schemas (~1,000 words; 1 envelope schema, 1 event catalog, 1 status schema)
### 6.1 Event envelope
#### 6.1.1 Define event_id, device_id, session sequence, timestamp, type, payload, and optional command_id correlation
### 6.2 Event catalog
#### 6.2.1 Define agent_hello, agent_goodbye, heartbeat, system_state_changed, user_session_changed, screen_lock_changed, application_started, application_exited, command_ack, command_result, and capability_report
### 6.3 MCA status report
#### 6.3.1 Define system state, user state, system information, and monitored application map as MCA evidence only

## 7. Status Cache, Provenance, and Freshness (~900 words; 1 status schema, 1 provenance table, 1 TTL table, 3 worked examples)
### 7.1 ESP32-owned status document
#### 7.1.1 Serve GET /api/v1/status only from the ESP32 cache with per-field provenance and freshness metadata
### 7.2 Provenance rules
#### 7.2.1 Define esp32_direct, network_probe, agent_reported, inferred, and unknown sources with precedence rules
#### 7.2.2 Mark agent-derived fields stale after heartbeat loss and expected_offline during declared power windows

## 8. Power Transitions and Expected-Offline Windows (~700 words; 4 sequence flows, 1 window table)
### 8.1 Sleep and wake
#### 8.1.1 Define sleep as HID dispatch followed by graceful agent_goodbye or expected offline within the sleep window
#### 8.1.2 Define verified wake only when the MCA reconnects and reports awake inside the wake deadline
### 8.2 Restart and shutdown
#### 8.2.1 Define restart confirmation as goodbye/offline followed by hello with changed boot_id and ready state
#### 8.2.2 Define shutdown completion through expected agent offline plus network unreachability inside the shutdown window

## 9. MacControlAgent Specification (~900 words; 1 lifecycle state machine, 2 schemas, 1 telemetry table)
### 9.1 Process model and privileges
#### 9.1.1 Define launchd-based background operation, automatic restart, minimal privileges, and no inbound control except paired ESP32 traffic
### 9.2 MCA UI
#### 9.2.1 Define pairing status, command enablement, response endpoint configuration, application allowlist, and local log view
### 9.3 Telemetry and control
#### 9.3.1 Enumerate closed agent actions: report status, report events, launch allowlisted app, quit allowlisted app, acknowledge command

## 10. USB HID Macro Engine and Shortcut Triggers (~750 words; 2 schemas, 1 interpreter algorithm, 1 worked example)
### 10.1 Macro model
#### 10.1.1 Define ordered steps for key press, key combo, modifier down/up, key release, text entry, and delay with macro timeout
#### 10.1.2 Exclude mouse, scroll, application actions, and conditional logic from MVP
### 10.2 Trigger bindings
#### 10.2.1 Define ESP32-owned trigger mappings from HTTP button, Web UI button, or optional GPIO input to macro_id
### 10.3 Execution semantics
#### 10.3.1 Define sequential interpretation, queue policy, deterministic timing bounds, abort behavior, and ledger integration

## 11. Application Monitoring and Control (~650 words; 1 registry schema, 1 status object, 1 state table)
### 11.1 Monitored application registry
#### 11.1.1 Define user-marked monitored applications and reconcile ESP32 expected list with MCA allowlist
### 11.2 Application control
#### 11.2.1 Define launch and quit only through allowlisted names and prohibit arbitrary shell execution as a closed invariant

## 12. REST API Contract (~1,100 words; 1 endpoint inventory, 4 schemas, 1 worked restart interaction)
### 12.1 Endpoint inventory
#### 12.1.1 Define controller endpoints for status, capabilities, commands, macros, agent status, logs, OTA, and OpenAPI
#### 12.1.2 Define agent-facing endpoints for WebSocket, event ingestion, pending command polling, and heartbeat semantics
### 12.2 Command API
#### 12.2.1 Define POST /api/v1/commands, convenience routes, GET /api/v1/commands/{id}, filtering, pagination, and idempotency
### 12.3 Error and capability models
#### 12.3.1 Define deterministic error envelope and mode-aware capabilities document

## 13. Security Model (~850 words; 1 RBAC matrix, 1 API key schema, 1 prohibited capability list)
### 13.1 Controller security
#### 13.1.1 Define API keys, admin password, HTTPS where practical, rate limiting defaults, and READ/CONTROL/ADMIN scopes
### 13.2 Agent security
#### 13.2.1 Define paired credentials, command allowlist, application allowlist, credential rotation, and revocation
### 13.3 Closed action invariant
#### 13.3.1 Prohibit generic execution endpoints and enumerate the complete set of privileged actions

## 14. User Interfaces (~800 words; 1 responsibility matrix, 2 wireframes, 1 provenance display table)
### 14.1 ESP32 Web UI
#### 14.1.1 Define dashboard, device configuration, macro editor, shortcut trigger setup, pairing control, security, logs, and OTA pages
### 14.2 MCA UI
#### 14.2.1 Define pairing, command configuration, response endpoint setup, application allowlist, and connection status pages
### 14.3 Status display rules
#### 14.3.1 Require UI to distinguish verified, unverified, stale, expected-offline, and unknown states without using completion language for unverified commands

## 15. Reliability, Persistence, Logging, and OTA (~900 words; 1 reliability table, 1 log schema, 1 OTA flow)
### 15.1 Reliability mechanisms
#### 15.1.1 Define watchdog, Wi-Fi reconnect, USB HID recovery, persistent configuration/macros, command timeouts, and heartbeat monitoring
### 15.2 Logging
#### 15.2.1 Define ring-buffer event log schema, categories, correlation IDs, retention, and retrieval endpoint
### 15.3 OTA
#### 15.3.1 Define signed dual-partition updates, automatic rollback, ADMIN-only update control, and in-flight command handling

## 16. MVP Definition and Acceptance Tests (~900 words; 3 acceptance tables)
### 16.1 Milestone 1: agentless HID endpoint
#### 16.1.1 Prove mDNS reachability, system commands, macro creation/execution, persistence, and sent-only/unconfirmed semantics
### 16.2 Milestone 2: agent-enhanced verification
#### 16.2.1 Prove pairing, verified status, verified restart/wake, expected-offline behavior, allowlist enforcement, RBAC, reconnect, fallback, provenance, idempotency, logging, and OTA
### 16.3 Explicit exclusions
#### 16.3.1 Exclude LLM/natural-language control, arbitrary shell execution, conditional macros, mouse control, and central manager from MVP

## 17. Deterministic Integration and Development Phases (~700 words; 1 phase table, 1 capabilities schema)
### 17.1 Deterministic machine-readable integration
#### 17.1.1 Define /capabilities and /openapi.json as integration contracts for Q-SYS, Companion, browsers, and scripts only
### 17.2 Development phases
#### 17.2.1 Map phases to deliverables, dependencies, risks, and acceptance-test exit criteria with the former AI phase removed

# References
## user_pasted_clipboard_long_content_as_file_Absolutely._I’d_revise_the_PRD_so_MacCon1.txt
- **Type**: User-uploaded PRD draft
- **Description**: Source product requirements and architecture framing for MacControl
- **Path**: /mnt/agents/upload/user_pasted_clipboard_long_content_as_file_Absolutely._I’d_revise_the_PRD_so_MacCon1.txt

## plan.md
- **Type**: Execution plan
- **Description**: Staged plan for producing this specification
- **Path**: /mnt/agents/output/plan.md

## maccontrol_spec.agent.outline.md
- **Type**: Report outline
- **Description**: This outline file
- **Path**: /mnt/agents/output/maccontrol_spec.agent.outline.md
