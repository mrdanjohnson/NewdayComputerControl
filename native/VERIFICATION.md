# Native verification — October 8, 2026

This is a built local development implementation. Hardware acceptance and distribution signing remain release gates.

## Completed checks

- Swift package tests cover event envelopes/nulls, session sequence reset, UTF-8 size rejection, dispatch shape and permission gates, backoff, legacy boot identity, both hello contracts, preserved migration settings, private exact backups, changed-file protection, corrupt configuration preservation, oversized capability reports, extreme legacy numeric values, and crash recovery cooldown.
- Runtime fixture tests cover disabled/malformed dispatch refusal without OS execution, duplicate suppression, ack/result/goodbye ordering before power initiation, already-running launch evidence, PID/provenance retention during a slow quit, and external exit provenance after a refused quit, unknown-lock behavior, persistent HALT, and token redaction.
- Foundation HTTP and WebSocket implementations connect to a loopback fixture that checks authorization, hello, polling session headers, and inbound dispatch. A missing-pong scenario confirms bounded channel failure. Credentials and action executors are synthetic.
- A full runtime capture covers all twelve protocol-v2 event types, including rejected malformed dispatches. The unchanged `firmware/lib/maccontrol_core/mc_agent_events.cpp` parser accepts the captured frames. This confirms wire schema compatibility, not the ESP32's hardware command verdicts.
- A temporary per-user service accepts status/configuration from the signed frontend, rejects an identity-changing settings request, and remains running after that frontend exits. A differently signed executable is refused XPC access. Temporary state and launchd registration are removed afterward.
- Release packaging verifies both executable architectures (`arm64`, `x86_64`), embedded LaunchAgent and icon, Hardened Runtime, and deep/strict signatures. The default signatures are ad hoc.
- A read-only host probe confirms actual boot, application/OS sampling, and telemetry APIs can run on this development Mac. Its initial lock observation is unknown, so no unlocked evidence is inferred.

The final Swift run passed **19 tests with zero failures**. The shipping firmware parser accepted **19 captured frames** covering all twelve event types. The signed frontend and untrusted-peer XPC checks passed. Logs are generated under `native/build/`: `tests.log`, `verification.log`, `ipc-smoke.log`, and `bundle-verification.json`.

## Required before production replacement

1. Install and register through the actual Login Items approval flow. Exercise all frontend views, discovery permission denial, service stop/unregister, and app upgrade. The UI overview was read through accessibility; complete visual interaction was limited by the development Mac's locked screen.
2. Test the service's Keychain access while locked, after logout/login, and after a signed upgrade. Verify Automation attribution and denial for the embedded service's fixed System Events calls.
3. Validate lock observations, pre-sleep/wake timing, telemetry freshness, graceful quit with unsaved work, and launch/quit on real allowlisted apps on macOS 13 and current macOS. Intel was cross-built; execution on an Intel Mac has not been verified.
4. Run the relevant AT-06 through AT-11 hardware scenarios and consecutive-run gates with a spare ESP32 and test Mac. Check actual ledger verdicts for sleep, wake, restart, shutdown, lock/unlock, network loss, revoked pairing, blocked quit, and transport changes.
5. Exercise Python → Swift → Python on a test installation, including failed cutover and settings differences. Verify a mixed fleet with existing controllers against the same supported protocol-v2 firmware. Unit tests validate migration data; a real legacy service has not been unloaded during development.
6. Obtain Developer ID signatures and notarization, then verify install/update/rollback with that stable signing identity. Ad-hoc builds pin XPC peers to their code hashes and are intended for local development.

Existing Python installations and ESP32 firmware were left untouched. No hardware power actions, real pairing/import, production service cutover, or firmware flash was performed.
