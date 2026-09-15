## 10. USB HID Macro Engine and Shortcut Triggers

The macro engine executes operator-defined, deterministic sequences of USB HID keyboard reports on the MacControl Endpoint. Macros are stored in ESP32 flash, created and edited only through the ESP32 Web UI (per the Chapter 2 ownership split), and every execution is entered in the Chapter 5 command ledger as command type `macro_execute`. The engine contains no conditional evaluation, no application awareness, and no inference: a macro is a fixed ordered list of steps that always produces the same report stream for the same input.

### 10.1 Macro model

#### 10.1.1 Define ordered steps for key press, key combo, modifier down/up, key release, text entry, and delay with macro timeout

A macro is an immutable-versioned object; edits create a new revision, and trigger bindings always resolve to the latest revision.

```json
{
  "macro_id": "mac_3F81",
  "revision": 4,
  "name": "Launch ProPresenter",
  "timeout_ms": 10000,
  "expected_event": {
    "type": "application_started",
    "match": { "bundle_id": "com.renewedvision.propresenter" }
  },
  "steps": [
    {"order": 1, "type": "key_combo", "modifiers": ["cmd"], "key": "space"},
    {"order": 2, "type": "delay", "delay_ms": 500},
    {"order": 3, "type": "text", "value": "ProPresenter"},
    {"order": 4, "type": "delay", "delay_ms": 500},
    {"order": 5, "type": "key_press", "key": "enter"}
  ]
}
```

| Field | Rule |
|---|---|
| `name` | 1–32 printable characters; unique per endpoint |
| `timeout_ms` | Default 10000; configurable 500–60000; bounds total wall-clock execution |
| `expected_event` | Optional Mode B verification declaration: an event `type` restricted to the five ambient evidence types — `application_started`, `application_exited`, `system_state_changed`, `user_session_changed`, `screen_lock_changed` — plus an exact-match `match` object over that event's payload keys (e.g., `bundle_id`). `command_ack`, `command_result`, `heartbeat`, `agent_hello`, `agent_goodbye`, and `capability_report` are explicitly forbidden as macro verification events; validation MUST reject them with HTTP 400 and error-envelope `code = "macro_invalid_step"`. Absent means the macro is unverifiable and always terminates `unconfirmed` |
| `steps` | 1–64 steps, executed strictly in ascending `order` |
| `key` | Named USB HID keyboard usage (letters, digits, function keys, navigation, `enter`, `space`, `esc`) |
| `modifiers` | Subset of `{ctrl, shift, alt, cmd}` |
| `value` (text) | 1–256 ASCII characters; each character is emitted as the required modifier+key pair |

The seven step types are a closed set: `key_press` (press and release one key), `key_combo` (assert modifiers, press key, release all), `modifier_down`/`modifier_up` (hold or release a modifier across subsequent steps, enabling chorded sequences), `key_release` (explicitly release a held non-modifier key), `text` (type a literal string at a default 10 ms inter-key interval), and `delay` (suspend the interpreter for `delay_ms`, range 10–5000). The interpreter MUST reject at validation time any step whose `type`, `key`, or modifier is outside these enums, answering the HTTP request with 400 and error-envelope `code = "macro_invalid_step"`; unknown step types MUST NOT be skipped silently at execution time.

#### 10.1.2 Exclude mouse, scroll, application actions, and conditional logic from MVP

The MVP engine MUST NOT implement mouse movement, mouse buttons, scroll, per-application targeting, variable substitution, branching, or any step whose behavior depends on Mac-side state. These are excluded for two deterministic reasons: the HID boot-protocol report descriptor advertised by the endpoint is keyboard-only, and any step predicated on observed state would make the report stream non-reproducible, breaking the "same input, same stream" invariant that makes ledger verdicts auditable. Application launch and quit are Mode B agent actions (Chapter 9), not macro steps; a macro such as "Launch ProPresenter" achieves its effect purely through keyboard input (Spotlight) and its Mode A terminal state remains `unconfirmed`.

### 10.2 Trigger bindings

#### 10.2.1 Define ESP32-owned trigger mappings from HTTP button, Web UI button, or optional GPIO input to macro_id

Triggers are ESP32-owned configuration, editable only in the ESP32 Web UI; the MCA has no write path to them. Each binding maps exactly one source to exactly one `macro_id`:

```json
{
  "trigger_id": "trg_02",
  "source": "gpio",
  "gpio_pin": 4,
  "edge": "falling",
  "debounce_ms": 50,
  "macro_id": "mac_3F81",
  "enabled": true
}
```

| Source | Invocation path | Notes |
|---|---|---|
| `http_button` | `POST /api/v1/macros/{id}/execute` (CONTROL scope) | Returns 202 + `command_id`, or 409 `macro_queue_full` pre-ledger when the queue is full; implicit trigger, no stored row required |
| `webui_button` | Dashboard RUN control | Requires an authenticated Web UI session; behaves identically to `http_button` |
| `gpio` | Physical input on a configured pin | `gpio_pin` 0–21, `edge` ∈ `{falling, rising}`, `debounce_ms` default 50 (range 10–500); requires an explicit stored binding |

GPIO bindings let a booth button or tally contact fire a macro without any network client, but they produce the same ledger record and queue entry as an HTTP invocation — there is no privileged local path. A trigger referencing a deleted macro MUST be disabled automatically and logged, never executed against a stale revision.

### 10.3 Execution semantics

#### 10.3.1 Define sequential interpretation, queue policy, deterministic timing bounds, abort behavior, and ledger integration

The interpreter is single-threaded; at most one macro executes at a time:

1. **Queue check.** Pending queue depth defaults to 4 (configurable 0–16). Queue depth MUST be checked before any ledger write: a full queue rejects HTTP invocations pre-ledger with HTTP 409 and error-envelope `code = "macro_queue_full"`, and no ledger record is created; a GPIO event against a full queue is dropped and logged with the would-be `macro_id`.
2. **Accept.** Only an invocation that passed the queue check is persisted: validate the macro and trigger, write a ledger record in `accepted` before any HID report, and return 202 with `command_id` (GPIO triggers record the same entry; the `command_id` is observable via `GET /api/v1/commands`).
3. **Enqueue and dispatch.** The accepted command enters the pending queue. On dequeue, transition to `dispatched`, set `dispatched_at`, and compute `deadline_at = dispatched_at + timeout_ms + 5 s` per Chapter 5. Queue wait consumes no verification budget, but a command still queued 30 s after acceptance MUST fail with `error_code = "dispatch_delayed"`.
4. **Interpret.** Execute steps in order. `delay` steps are scheduler-based with a worst-case timing deviation of 5 ms; text and key reports are emitted at the configured inter-key interval, bounded so total execution cannot exceed `timeout_ms` by more than one report interval.
5. **Abort.** On macro timeout, an explicit abort request, or USB disconnect, the interpreter MUST immediately emit an all-keys-up report (releasing every held modifier), release the queue slot, and terminate the command `failed` with `error_code` ∈ `{"macro_timeout", "macro_aborted", "usb_disconnected"}`. No partial-resume exists; a re-trigger starts a new command.
6. **Terminate.** In Mode A the command MUST end `unconfirmed` with `result = "hid_only"`. Macros are HID-only: there is no MCA `command_ack`/`command_result` correlation for macro execution, and the ESP32 MUST NOT require one. In Mode B the command ends `completed` with `result = "macro_confirmed"` only when the macro definition declares an explicit `expected_event` (10.1.1) and a matching Chapter 6 event arrives before `deadline_at`; a macro without an `expected_event` is unverifiable and MUST end `unconfirmed` with `result = "hid_only"` even in Mode B. If the deadline passes without the expected event, the command ends `timed_out` / `deadline_exceeded`. The MCA supplies evidence and never closes the command itself.

**Worked example — Launch ProPresenter.** With the macro of 10.1.1 bound to `trg_02` (GPIO 4), a falling edge produces: ledger record `7AC41E9B` `accepted` at $t_0$; `dispatched` at $t_0 + 12$ ms; step 1 emits `cmd+space` (Spotlight opens); 500 ms delay; step 3 types `ProPresenter` over ~120 ms; 500 ms delay; step 5 emits `enter`. Total execution ≈ 1.13 s, well inside the 10 s timeout. In Mode A, `7AC41E9B` terminates `unconfirmed`/`hid_only` — the operator sees the application open, but the endpoint claims only transmission. In Mode B, this macro declares `expected_event = application_started(bundle_id = com.renewedvision.propresenter)`; when the MCA's `application_started` event for that bundle ID arrives before the deadline, the ESP32 — not the agent — records `completed`/`macro_confirmed`, while the same macro without an `expected_event` declaration would remain `unconfirmed` even with the agent present. Acceptance implication: power-cycling the endpoint between steps 2 and 3 MUST leave `7AC41E9B` resolvable to `failed`/`esp32_restarted` after boot reconciliation, and the held-modifier release on abort MUST be observable as a final all-keys-up report on the USB bus.
