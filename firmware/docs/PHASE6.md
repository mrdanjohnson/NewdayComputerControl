# PHASE6 — Production hardening (PRD §17.2.1 Phase 6)

> Repo numbering note (HANDOFF): repo "Phase 6" = PRD Phase 6 "Production
> hardening". Exit criteria per PRD §16/§17.2.1: **AT-10 + AT-12 green**, and
> the **Milestone 2 gate = AT-06 through AT-12 all green, twice consecutively**
> with no code change between runs. The PRD's old Phase 7 (AI) is removed;
> OpenAPI/capabilities/discovery were folded into Phase 3.
>
> PRD deliverables for this phase: Q-SYS/Companion modules, Ethernet, signed
> dual-partition OTA, allowlist and RBAC hardening, provenance surfacing.
> Primary PRD risk: **OTA rollback reliability**. Secondary: third-party
> module maintenance.
>
> **Hard rules:** the determinism defaults (heartbeat 5 s, stale 15 s,
> offline 30 s, backoff 1/2/4/8/30 s + 0–20 % jitter, windows 3/60 s,
> per-type deadlines) are AT inputs — changing any of them invalidates
> AT-06…AT-12 and MUST trigger a full re-run (PRD §17.2.1). Any regression
> in an earlier phase's AT blocks phase completion. Heap discipline per
> AGENTS.md (320 KB RAM, no PSRAM) governs every new path.

## Scope status

| Item | State | Notes |
|------|-------|-------|
| Phase 5 cleanup (commit + dist) | DONE 2026-10-01 | commit a481ec9 already covered web_ui_page.h + HANDOFF; dist refresh deferred to the end of this phase (flashed truth is `.pio/build`) |
| AT-11 bench-WS caveat | CARRY | re-run with bench long-lived-WS path fixed, or formally accept; not a Phase 6 blocker |
| X-API-Key acceptance + 401 usage hint | DONE 2026-10-01 | HANDOFF carry-over; only `Authorization: Bearer` parsed before |
| `protocol_version` hello validation | ALREADY DONE (pre-gate) | `AgentSession::acceptHello` closes 4003 ProtocolMismatch before any frame processing (`mc_agent_session.cpp`); added the missing close *reason* string 2026-10-01 |
| Signed dual-partition OTA (§15.3) | DONE 2026-10-01 | streamed upload (socket→flash, never RAM), mbedTLS P-256 verify after write, slot abort on failure, bootloader rollback (already enabled in the core 2.0.17 precompiled bootloader — verified at build level; sdkconfig route is a dead end for pure-Arduino, see docs/DEBUG-OTA.md), MCV1 in-image version marker, `device.firmware_version` in status |
| AT-10 script (`at10.py`) | AUTHORED 2026-10-01 | not yet live-run; needs a registry-registered-but-not-allowlisted probe bundle on the bench (spec 11.2.1 validates registry before allowlist) |
| AT-12 script (`at12.py`) | AUTHORED 2026-10-01 | not yet live-run; signed-OTA legs were an honest red gate until `ota_sign.py` landed; deep unconfirmed-slot revert is DEFERRED (structural check only) |
| `at05.py` OTA check | DONE 2026-10-01 | now pins 20 closed codes with `ota_in_progress` present; re-run in the Milestone 2 sweep |
| Q-SYS / Companion modules | DEFERRED | integration consumers only; not gate-blocking; needs target hardware (Q-SYS Core / Companion host) to acceptance-test |
| Ethernet | DEFERRED (hardware) | S3 devkitc has no onboard PHY; needs W5500-SPI or RMII add-on. Bench task + `platformio.ini` env work; documented, not gate-blocking |
| Monitored-app registry | DEFERRED | carry-over note; later batch |
| Ledger durable-stream growth policy | DEFERRED | latest-revision-only compaction needs a PRD amendment first |
| Awake/asleep dashboard badge | OPTIONAL | all data already in `/api/v1/status` |

## Firmware changes this phase

### X-API-Key + 401 usage hint (2026-10-01)

`http_api.cpp`: the `x-api-key` header is now parsed into `Request::api_key`
and used as a fallback when `Authorization: Bearer` is absent. 401 responses
on the API-key path now carry an explicit message:
`"authenticate with Authorization: Bearer <key> or X-API-Key: <key>"`
(sendError message override; envelope shape unchanged — same closed
`error.code/request_id` fields, so AT-01's clean-401 checks stay valid).

### Hello close-reason (2026-10-01)

`agent_link.cpp`: the 4003 ProtocolMismatch close now carries the reason
string `"protocol_version 2 required"` instead of nullptr, so agents log
*why* they were refused without a pairing-window round trip.

### Signed dual-partition OTA (PRD §15.3)

Design (normative PRD requirements in parentheses):

- **Partition table** (`partitions_maccontrol.csv`, both device envs):
  factory recovery image + `ota_0` + `ota_1` + small LittleFS partition.
  The S3 image is already ~1.33 MB — larger than the default 1.3 MB OTA
  slot — so the default table cannot carry OTA at all; a factory slot plus
  two ~1.9 MB OTA slots is the working layout (min_spiffs-style, kept
  explicit in-repo).
- **Endpoints** (`POST /api/v1/ota/upload`, `POST /api/v1/ota/apply`,
  ADMIN-only, rate-limited; routes added to the closed surface and to
  `mc_openapi.cpp` so test_docs validates them).
- **Signature**: ECDSA P-256 over the SHA-256 digest of the image, public
  key compiled into the firmware; verified BEFORE any flash write (bad
  signature → 400 `bad_request` + `ota` log at error) (`mc_sha256` exists
  in core; mbedTLS ECDSA on device; `scripts/ota_sign.py` — one-shot keygen
  + sign using the firmware venv).
- **Upload** streams to the inactive OTA slot via the Arduino `Update`
  library; digest re-checked after the write; 200 `{"validated":true,
  "version":...}`.
- **Apply**: 409 `ota_in_progress` while any command is
  accepted/dispatched/confirming, unless `force:true` — force terminates
  every non-terminal record `failed`/`esp32_restarted` BEFORE reboot
  (matching boot-reconciliation verdicts). Concurrent OTA ops share the
  409.
- **Self-check + rollback**: 60 s window after reboot — watchdog armed,
  Wi-Fi association attempted, Web UI + `/api/v1/status` responsive;
  explicit confirmation write marks the new slot valid; otherwise the
  bootloader reverts and the device logs `ota`/`rollback` at error.
  Version regression permitted but logged.
- **Version surfacing**: running version recorded in every `ota` log entry
  and in `GET /api/v1/status` (new build-defined `MC_FW_VERSION`).

## AT work this phase

- `at10.py` — preconditions: one bundle ID allowlisted; allowlisted launch
  completes via correlated `command_ack(action=launch_app)` +
  `application_started(bundle_id)`; non-allowlisted launch fails with the
  deterministic envelope; CONTROL-scope request for an ADMIN write → 403.
  Two consecutive green runs, one boot per round (§16 rule).
- `at12.py` — ADMIN key: replay same `Idempotency-Key` + body → 200 with
  the original `command_id`, no duplicate dispatch (ledger count pinned);
  divergent body same key → 409 `conflict`; log inspection shows
  correlation/request IDs on transitions via `GET /api/v1/logs`; signed
  OTA upload/apply boots the new version; tampered/bad signature refused
  400; non-ADMIN OTA → 403; rollback path exercised (bad image or
  unconfirmed slot reverts).
- `at05.py` — flip the "ota_in_progress absent" check to expect the code
  present once OTA ships.

## Milestone 2 gate (end of phase)

AT-06, AT-07, AT-08, AT-09, AT-10, AT-11, AT-12 — each green twice
consecutively on one boot per round, on the final binary. AT-06/AT-11
steal the target pairing: re-pair + `launchctl kickstart -k
gui/$(id -u)/com.maccontrol.agent` is the permanent last step (HANDOFF).

## Log

### 2026-10-03: Flashed, re-provisioned, OTA smoke-tested on hardware — one real bug found and fixed (49a2e07)

> The device was flashed with the Phase 6 binary over the 'com' cable and
> re-provisioned. The migration went exactly as flagged: LittleFS orphaned
> (fresh kstore/mstore/tstore), NVS survived (hostname, device_id, admin
> password, **pairing** — `pairing_restored` in the boot log). New keys:
> key-09 READ / key-10 CONTROL / key-11 ADMIN (label "bench"). Pre-migration
> LittleFS backed up at `/var/tmp/maccontrol_littlefs_pre-p6-migration.bin`
> (1.5 MB; /var/tmp survives macOS /tmp wipes).
>
> OTA smoke over the air, all against the live contract: CONTROL upload →
> 403; 256-byte garbage upload → 400 `bad_request`; signed 1.4 MB upload →
> 200 `{"validated":true,"version":"1.6.0-phase6"}` in 9.2 s (slot erase +
> stream + verify); apply `{"force":false}` → 200, reboot into the pending
> slot, `pairing_restored` again, self-check confirmed on the first status
> serve, no rollback (past the 60 s window, single boot cycle).
>
> **Bug found by the smoke (fixed in 49a2e07, re-flashed, re-verified):**
> the upload intercept ignored body bytes the request parser had already
> consumed past the header terminator, so a fast client (curl --data-binary)
> desynced the streamed read — the device blocked until the client's own
> timeout dropped the TCP connection, then logged `truncated_signature` /
> `upload_rejected`. Fix: `handleUpload` receives the buffered prefix and
> drains it first (`readMixed` + buffer-aware stream loop). Post-fix:
> garbage → 400 in 0.23 s. Forensics added to `docs/DEBUG-OTA.md`.
>
> Known minor (not worth a solo flash cycle): the upload 400/409 envelopes
> reuse the generic `bad_request` default message ("Malformed JSON…") —
> correct code, cosmetically wrong text for a binary endpoint; fix the
> message next time the firmware is touched for other reasons.
>
> Agent note: ag-05e1 had not reconnected at handoff time — the churchtech
> Mac was likely asleep; the endpoint side is proven (`pairing_restored`,
> agent status surface live). Expect self-reconnect on wake, as before.
>
> Still owed for the Milestone 2 gate: live AT-10 (needs a registry-
> registered-but-not-allowlisted probe bundle on the target), AT-12 (its
> signer legs now have a proven device path), then the AT-05 (updated pin) +
> AT-06…AT-09 + AT-11 full sweep ×2.

### 2026-10-01: OTA + AT-10/AT-12 authored, host-verified, committed (5ccca55) — NOT flashed

> Signed dual-partition OTA implemented per §15.3 and committed with the AT
> scripts. Native 156/156 (5 new test_ota cases); both envs compile (S3 image
> 1,412,569 B = 69.5 % of the 0x1F0000 slot; wroom 72.2 %). `ota_sign.py`
> round-trip verified (sign→verify OK, flipped byte→FAIL). `dist/esp32-s3/`
> refreshed (VERSION built 2026-10-01T14:23Z).
>
> **CAUTION — first flash migrates the partition table; the bench device
> LOSES its LittleFS** (API keys, macros, triggers, ledger — new table moves
> spiffs 0x670000→0x5E0000 region). NVS at 0x9000 (pairing, Wi-Fi, identity,
> unlock password) survives. Re-provision keys after flashing (serial CLI or
> `install.sh --provision`), and re-create macros. Back up first if the
> ledger matters: `esptool read_flash 0x670000 0x180000 old_littlefs.bin`.
> Forensics + the full rollback enablement story: `docs/DEBUG-OTA.md`.
>
> Deviations (all documented in the mc_openapi.cpp amendment note):
> verify-after-write instead of verify-before-write (1.4 MB image vs 320 KB
> RAM — same guarantee: bad image never bootable, running image never
> erased); factory recovery slot is S3-only (4 MB wroom cannot fit three app
> slots); bootloader rollback came already-enabled in the precompiled core
> 2.0.17 bootloader — the `board_build.sdkconfig` hook does not exist for
> pure-Arduino builds (verified dead end), sdkconfig.defaults is kept for a
> future espidf-mixed build.
>
> Rollback self-check: 60 s window; confirmation = explicit
> `esp_ota_mark_app_valid_cancel_rollback()` on first successful
> `/api/v1/status` serve; timeout → `esp_ota_mark_app_invalid_rollback_and_reboot()`.
> In-flight `Update.begin` slot erase can exceed the TWDT window — a worst-case
> trip just reboots with the slot aborted and the client retries (DEBUG-OTA.md).
>
> Still owed for the Milestone 2 gate: flash the S3 (user decision — the
> LittleFS migration above), re-provision, then live-run AT-05 (updated
> 20-code pin), AT-10, AT-12, and the AT-06…AT-09 + AT-11 regression sweep ×2.
