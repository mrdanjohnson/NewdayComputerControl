# MacControl — DEBUG: Phase 4.5 AT-11 regression (app detection blindness)

2026-09-23. Phase 4.5 hardware verification: AT-11 went red on the Phase 4.5
binary (every launch/quit `timed_out` with only ack+result evidence). Root
cause was **not** in the Phase 4.5 changes — it predates them (latent since
Phase 4), and the Phase 4 gate got lucky. Full forensics below; fix landed in
agent v1.1.1.

## Symptom

AT-11: `launch`/`quit` records reach `timed_out` (`error_code=
deadline_exceeded`, `result=null`) even though the agent executed the action
(app actually launched/quit on macOS) and both `command_ack` and
`command_result` were admitted and correlated (visible in the record's
`evidence`). The engine completes launch/quit only on **ack + the matching
application event** — and no `application_started`/`application_exited` ever
arrived.

## Root cause: NSWorkspace snapshot freezes in headless processes

`AppMonitor` used `NSWorkspace.runningApplications()`. Verified
experimentally on the bench Mac (macOS 15.7.4, pyobjc 12.2.2):

- The enumeration snapshot is frozen at the **first query** in a process
  that never spins an NSRunLoop (the MCA is a pure asyncio app).
- Query before launching an app → the app is invisible to every later query
  in that process, forever. A fresh process sees it immediately.
- First query after the launch → visible. So ordering decides everything:
  the Phase 4 gate passed because its daemons happened to first probe while
  the test app was already running; AT-11's kill/restart churn put the
  first probe in the "app dead" state and every subsequent launch was
  invisible.

Secondary observations (moot after the rewrite, recorded so nobody re-tries
NSWorkspace detection):

- The two `--transport polling` AT daemons logged `import AppKit` failing at
  `AppMonitor` init while websocket daemons and one-shot detached tests
  imported fine — never root-caused.
- The pgrep fallback (`mdfind` path resolve + `pgrep -f`) also failed to see
  agent-launched apps in some AT daemon contexts.

## Related gap: launch of an already-running app was unconfirmable

Even with working detection, relaunching an already-running app produces no
Telemetry delta (`self._apps` already contains the bundle), so no
`application_started` ever fires and the engine can never complete the
command. Fixed alongside the detection rewrite.

## Fix (agent v1.1.1 — `agent/maccontrol_agent/`)

- `events.py` `AppMonitor`: psutil is now the primary probe — one
  `psutil.process_iter(["exe"])` scan per call, allowlisted bundle resolved
  to its `.app` path via the cached `mdfind` lookup, matched by exe-path
  prefix. pgrep remains only as the no-psutil fallback. The NSWorkspace
  probing path and its `import AppKit` check are removed; class docstring
  documents why.
- `actions.py` `_launch`: after `open -b` rc 0, poll the probe for up to
  5 s; on first sighting enqueue `application_started {bundle_id, pid}`
  before the ok `command_result`. Duplicates with the Telemetry delta are
  safe: the firmware soft-ignores evidence for non-confirming records and
  the completion predicate is idempotent.
- `front_app_changed` still uses NSWorkspace `frontmostApplication()`
  (best-effort B1); documented as possibly lagging in headless contexts.

## Verification

Live against mac-b53478.local (network only): launch (app dead) →
`completed/app_launch_confirmed` ~2 s; launch (already running) →
`completed/app_launch_confirmed`; quit → `completed/app_quit_confirmed`.
AT-11 re-run green (see `docs/HANDOFF.md` banner).

## Watch item handed to Phase 5 (not fixed here)

`/api/v1/agent/status` renders `applications.<bid> = {running: false,
pid: <stale>}` after an exit — the pid is real past evidence but arguably
violates the spec-9 "absent, never invented" spirit. Decide whether exit
should clear the pid in the AgentStatus app map.
