# MacControl Endpoint

This module connects Bitfocus Companion to a **MacControl ESP32 endpoint** on your
LAN, giving you Stream Deck buttons for:

- **Macros** — one button per macro from the device's live macro list, with
  feedback showing `confirming → completed / failed`.
- **Power & session commands** — Wake, Sleep, Lock, Unlock, Restart, Shutdown.
- **App control** — launch/quit an allowlisted macOS app by bundle ID.
- **Status** — live variables and feedbacks for device mode, agent session,
  host awake/asleep, screen lock, CPU/memory, and command state.

## Configuration

| Field | Meaning |
| --- | --- |
| **Hostname / IP** | The endpoint on the LAN, e.g. `control-protools.local`. A scheme (`http://`) or trailing path is optional and will be stripped. Plain HTTP port 80 is used. |
| **API Key** | A `secret-text` field. Use a **CONTROL-role** (or better) key — CONTROL suffices for every action in this module. |
| **Poll interval (ms)** | How often the module polls device + agent status. Default `5000`, minimum `2000`. |

## Rate limits — important

The device enforces per-role rate limits (CONTROL: **30 requests/min**) and
locks out an IP after **10 bad API keys for 60 s** (the module never retries a
401). The poller is budgeted to stay under the cap:

- `/status` + `/agent/status` every poll tick (24/min at the default 5 s)
- `/commands?limit=5` every 4th tick (~3/min)
- `/macros` + `/capabilities` on connect and every 60th tick (~0.4/min)

**Total ≈ 27.5/min.** If the device still answers `429 rate_limited`, the
module doubles its poll interval (up to 60 s) and restores the configured
interval after 5 clean minutes.

If the API key is rejected, the connection status shows **BadConfig** — fix the
key in the connection config; the module will not hammer the device retrying.

## Feedbacks (color legend)

| Feedback | True when | Default color |
| --- | --- | --- |
| **Endpoint online** | the device answers `/status` | green |
| **Agent session active** | a Mac agent session is active (Mode B) | green |
| **Host awake** | the Mac's system state is `awake` | green |
| **Screen locked** | the Mac screen is locked | yellow |
| **Command state** | the tracked command record is in the selected state | yellow |

**Command state** options:

- **Command ID** — `last` (newest record on the device, default),
  `last_fired` (the newest command this module itself fired), or a specific
  `command_id`.
- **State** — `accepted`, `confirming`, `completed`, `failed`, `timed_out`,
  `unconfirmed`.

Because confirmation can take ~1 minute (e.g. sleep verification), put several
Command state feedbacks on one button: yellow while `confirming`, green when
`completed`, red when `failed`.

## Variables

`mode`, `capability_level`, `device_name`, `session_active`, `system_state`,
`screen_locked`, `user_name`, `front_app`, `cpu_utilization_pct`,
`memory_utilization_pct`, `macros_count`, `last_command_id`,
`last_command_type`, `last_command_state`, `last_command_result`.

Variables are empty/`unpaired`-style defaults until the corresponding data has
been fetched; agent variables require a paired agent (Mode B). In Mode A,
`agent/status` returns `409 agent_not_paired`, which is normal and **not** a
connection failure — host awake/screen lock fall back to the device's own
sensors where available.

## Notes

- **Sleep / Restart / Shutdown put the production Mac to sleep or reboot it.**
  They only run when a human presses the button.
- `unlock` uses a password stored on the device; this module sends no
  credentials.
- App launch/quit uses `POST /api/v1/apps/<bundle_id>/launch|quit` against the
  device's allowlist; if your firmware exposes a different path, use a generic
  HTTP action instead.
