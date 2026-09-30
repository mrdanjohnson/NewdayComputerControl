# DEBUG-PAIRING-ADMIN-404 — pairing admin sub-routes always 404'd

**Symptom:** Web UI Pairing tab → "Revoke pairing" (or "Open pairing window")
fails with `not_found: Unknown path, command_id, or version`. The pairing
state card (`GET /api/v1/pairing`) worked fine.

**Root cause:** off-by-one in the pairing-administration route in
`src/http_api.cpp` (present since commit 318ad8b, the Phase 4 first attempt).
The prefix `"/api/v1/pairing/"` is **16** characters, but the handler used
`compare(0, 17, …)` and `req.path.substr(17)`. The `compare` accidentally
still behaved as a prefix test (C-string length semantics), so sub-paths
entered the block — but `substr(17)` then stripped the first character of
the sub-resource:

- `POST /api/v1/pairing/revoke` → `rest == "evoke"` → fell through to
  `sendError(NotFound)`
- `POST /api/v1/pairing/window` → `rest == "indow"` → same

Every other prefix/substr pair in the router was already consistent
(`keys/` 13→13, `macros/` 15→15, `triggers/` 17→17, `commands/` 17→17,
`system/` 15→15, `apps/` 13→13); only `pairing/` was wrong.

**Why it survived:** the pairing ceremony itself (`POST /agent/v1/pair`) is a
different route and always worked, so pairing succeeded end-to-end. The
broken sub-routes are Web-UI-session-only, which no AT script exercises.

**Fix (2026-09-28):** `src/http_api.cpp` — 17 → 16 in both the `compare`
length and the `substr` offset. Native 146/146, both envs compile,
`dist/esp32-s3/` refreshed (VERSION built 2026-09-28T14:49:33Z).

**Lesson:** when adding a prefixed route, the prefix length in `compare`
and the `substr` offset must both be `strlen(prefix)` — count it, don't
copy the neighbor's number.
