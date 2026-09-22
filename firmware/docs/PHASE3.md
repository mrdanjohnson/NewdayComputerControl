# MacControl Firmware — Phase 3 (identity, security, integration contracts)

Phase 3 of spec §17.2.1: mDNS discovery hardening, device identity, RBAC,
`/capabilities` honesty, **`/openapi.json`**, and **logging**, closing
**Milestone 1 (AT-01 through AT-05 all green on target hardware)**.

## What was built

**`GET /api/v1/logs` (spec 15.2)**
- `RamLogSink` now records category/level per entry and answers filtered
  queries (`entries_since(since, limit, category, level, &dropped)`); the
  route parses `?category=`, `?level=`, `?since_seq=`, `?limit=`
  (default 100, max 512, clamped — out-of-enum filter values are 400, not
  empty results). Response is `{"entries":[...],"dropped":N}` in ascending
  `seq`; `dropped` counts entries overwritten since the cursor
  (`log_dropped_count()` in `mc_log.h`, unit-tested).

**`GET /api/v1/openapi.json` (spec 12.1.1 / 17.1.1)**
- Static OpenAPI 3.1 document (`mcco::kOpenApiJson` in
  `lib/maccontrol_core/mc_openapi.cpp`) served verbatim; lives in the core
  library so `test_openapi` validates it natively (parses, 3.1.x, all
  Chapter 12 paths, amendment paths, error table, schemas).
- `info.description` carries the **amendment note**: `/api/v1/triggers*`,
  `/api/v1/device/identity`, `/api/v1/keys*`, and `/ui/*` are ESP32-owned
  additions beyond spec ch. 12 (Web UI support, spec ch. 14); the session
  cookie acts as ADMIN on `/api/v1/*`; the OTA endpoints are intentionally
  absent until Phase 6 (no partial command types).

**API key management over HTTP (spec 13.1.1)**
- `GET/POST /api/v1/keys`, `DELETE /api/v1/keys/{id}` — Web UI **session
  only** (an API key, even ADMIN, gets 403; spec: keys are managed "only
  through the ESP32 Web UI by an ADMIN-scoped session"). Raw key returned
  exactly once at creation; 8-active cap → 409; create/revoke logged to the
  `auth` category with `key_id` only.

**Closed-surface agent stubs (spec 12.1.1)**
- `GET /api/v1/agent/status` and `POST /api/v1/apps/{bundle_id}/launch|quit`
  answer 409 `agent_not_paired` in Mode A with no ledger records — matching
  the generic submission path's pre-ledger refusal.

**Web UI (spec 14.1.1)**
- **Security tab**: key list/create/revoke, admin password change
  (`POST /ui/password`, session-only, ≥ 10 chars, PBKDF2 storage reused),
  persistent **cleartext warning banner** (spec 13.1.1 MUST), rate-limit and
  lockout notes.
- **Logs tab**: category/level filters, limit, seq-cursor paging, `dropped`
  truncation indicator.

**mDNS hardening (spec 3.1)**
- Re-announce on every Wi-Fi (re)connect, not only at boot/identity change
  ("on joining a network MUST issue an unsolicited announcement"). The AAAA
  fix from the Phase 2 tail (link-local IPv6 formed after mDNS is up so the
  responder answers macOS's AAAA-first lookups) remains.

**Macro/trigger persistence fix (spec 15.1, capacity 64)**
- Found during the regression gate: macro persistence used a single NVS
  string whose ~4 KB per-entry limit silently broke the lazy persist at
  ~5 macros (47 KB at full capacity). Macro and trigger stores now use the
  same double-slot atomic-commit scheme on **LittleFS** (slot files
  `/mstore.0|1`, `/tstore.0|1`; NVS uchar commit marker written last after
  readback verification; per-slot CRC32; committed-slot-first fallback).
  Verified: 7 macros (~5 KB payload) survive reboot; the 64-macro capacity
  now actually persists. Note: existing NVS-resident macro/trigger data is
  not migrated (development device; test residue only).

## Deviations reconciled in `/openapi.json`

1. `/api/v1/triggers*` + `/api/v1/device/identity` — amendment note ✔
2. Session-as-ADMIN on `/api/v1/*` — amendment note ✔
3. First `/ui/login` with no password set creates it — documented on the
   login form and in the Security tab.
4. UI-login vs API-key lockouts tracked per failure class — as built.
5. `GET /api/v1/logs` — implemented (this phase).

New deviations introduced this phase (flagged for Phase 6 / spec amendment):
- Key management endpoints are session-only (spec 13.1.1 reads as Web UI
  only; no API-key ADMIN path exists).
- Macro/trigger stores persist on LittleFS rather than NVS (spec says
  "flash, same atomic-commit scheme" — kept the scheme, changed the medium,
  because NVS entries cap at ~4 KB).
- HTTPS serving (spec SHOULD, with cert install) deferred to Phase 6; the
  MUST warning banner ships now. Rate limits are fixed defaults surfaced
  read-only, not yet UI-configurable.

## Verification status

- `pio test -e native`: **91/91** (+5 `test_openapi`, +1 dropped-count).
- Both firmware envs compile clean.
- Milestone 1 gate on the ESP32-S3 (2026-09-21), final Phase 3 binary:
  - `at01_at02.py` — all checks passed (real `unconfirmed`/`hid_only`)
  - `at03_at04.py` — all checks passed, **twice consecutively**
  - `at05.py` — capabilities honesty + openapi/logs/keys contracts, all passed
- Live curl smoke: key lifecycle (create → authenticate → revoke → 403),
  logs filters/limit/clamp/dropped, agent/apps 409s, openapi parses with
  25 paths.
- Web UI Security/Logs tabs: browser-checked (key create/revoke, password
  change, log paging). Two latent Web UI bugs found and fixed by visual
  verification: `showApp()` revealed the app shell with `style.display=''`,
  which cannot override the `.tabs{display:none}` class rule — the shell was
  invisible after login since Phase 2 (header/footer rendered, everything
  else blank; DOM dumps masked it). Now `display:'block'`. Also added `for`
  attributes to all 18 `<label>` elements and restyled the spec 13.1.1
  cleartext notice as a warning banner (amber) rather than error styling.

## Fixed / deferred to later phases

- HTTPS + certificate install (Phase 6 hardening).
- Applications / Pairing / OTA Web UI pages (Phases 4/6 backends).
- `/agent/v1/*` currently answers 401 (Phase 4 pairing transport).
- Rate-limit configurability (defaults per spec 13.1.1 are enforced).
