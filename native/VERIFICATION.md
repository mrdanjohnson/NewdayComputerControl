# Native verification — October 8, 2026

This is a built local development implementation. Hardware acceptance and distribution signing remain release gates.

## Local test build — October 11, 2026

MacControl **2.1.1 (build 3)** was rebuilt from the current workspace for the newly inspected ESP32-S3 with 16 MiB flash. Setup now accepts exactly 8 MB or 16 MB reported by esptool, rejects missing/ambiguous/unsupported capacity, and retains the existing 8 MB firmware header and partition layout. The additional space on 16 MB boards remains unused. Firmware and flash-mode settings are unchanged.

- **33 Swift tests passed**, including 8/16 MB acceptance, malformed/ambiguous capacity rejection, and preservation of the 8 MB flash arguments.
- **158 firmware host tests passed** before the user's planned flash.
- Universal Release build succeeded for Apple silicon and Intel. Deep/strict signatures, embedded tools, firmware checksums, source archive/license, and execution of the packaged esptool 5.5.0 version command passed.
- Outputs: `build/MacControl.app`, `build/MacControl-2.1.1.zip`, `build/release-build.log`, `build/tests.log`, `build/firmware-host-tests.log`, and `build/local-build-verification.json`.

This refresh did not flash or provision hardware, install/register the app, change any live agent, or repeat the earlier XPC smoke checks. It is ad hoc signed for local testing. Physical USB flashing/provisioning on the 16 MB board remains pending.

## Completed checks

- Swift package tests cover event envelopes/nulls, session sequence reset, UTF-8 size rejection, dispatch shape and permission gates, backoff, legacy boot identity, both hello contracts, preserved migration settings, private exact backups, changed-file protection, corrupt configuration preservation, oversized capability reports, extreme legacy numeric values, and crash recovery cooldown.
- Runtime fixture tests cover disabled/malformed dispatch refusal without OS execution, duplicate suppression, ack/result/goodbye ordering before power initiation, already-running launch evidence, PID/provenance retention during a slow quit, and external exit provenance after a refused quit, unknown-lock behavior, persistent HALT, and token redaction.
- Foundation HTTP and WebSocket implementations connect to a loopback fixture that checks authorization, hello, polling session headers, and inbound dispatch. A missing-pong scenario confirms bounded channel failure. Credentials and action executors are synthetic.
- A full runtime capture covers all twelve protocol-v2 event types, including rejected malformed dispatches. The unchanged `firmware/lib/maccontrol_core/mc_agent_events.cpp` parser accepts the captured frames. This confirms wire schema compatibility, not the ESP32's hardware command verdicts.
- A temporary per-user service accepts status/configuration from the signed frontend, rejects an identity-changing settings request, and remains running after that frontend exits. A differently signed executable is refused XPC access. Temporary state and launchd registration are removed afterward.
- Release packaging verifies both executable architectures (`arm64`, `x86_64`), embedded LaunchAgent and icon, Hardened Runtime, and deep/strict signatures. The default signatures are ad hoc.
- A read-only host probe confirms actual boot, application/OS sampling, and telemetry APIs can run on this development Mac. Its initial lock observation is unknown, so no unlocked evidence is inferred.

- Provisioning tests cover lossless JSON credential encoding, control-character rejection, serial-path restrictions, firmware digests/fixed offsets, key-by-key retention, retry reuse of active keys, technician preparation without client Wi-Fi, process EOF/deadline/cancellation, and fragmented replies/deadlines using a test-owned pseudo-terminal. No physical serial port is opened by these tests.
- Loopback setup HTTP tests verify READ authentication, physical/network identity matching, refusal of a different device, admin login cookie handling, pairing-window parameters, and the expected active agent instance. All credentials are synthetic.
- Firmware host tests pass **158/158**. S3 and WROOM builds succeed. S3 distribution images were refreshed to **1.6.1-phase6**, built but not flashed, with bounded additive JSON setup commands. The existing protocol-v2 agent parser is unchanged.
- The packaged Espressif 5.5.0 host-architecture binary executes its read-only version command from inside the signed app. Both tool architectures/signatures, firmware digests, exact source archive digest, and license/provenance are checked during release verification.

The final Swift run passed **32 tests with zero failures**. The shipping firmware parser accepted **19 captured frames** covering all twelve event types. The signed frontend and untrusted-peer XPC checks passed. Logs are generated under `native/build/`: `tests.log`, `verification.log`, `ipc-smoke.log`, and `bundle-verification.json`.

## Required before production replacement

1. Complete the USB wizard on a spare ESP32-S3 N8: chip/flash-size refusal, BOOT/reset recovery, flash verification, SSIDs/passwords with spaces, key-store-full and watchdog recovery, Wi-Fi failure, technician preparation, client pairing, credential export/reload, and cancel/disconnect. **No real board was flashed or provisioned during this implementation.**
2. Install and register through the actual Login Items approval flow. Exercise all frontend views, discovery permission denial, service stop/unregister, and app upgrade. The UI overview was read through accessibility; the USB wizard could not be visually exercised because the development Mac was locked (CUA reported no accessible native windows).
3. Test the service's Keychain access while locked, after logout/login, and after a signed upgrade. Verify Automation attribution and denial for the embedded service's fixed System Events calls.
4. Validate lock observations, pre-sleep/wake timing, telemetry freshness, graceful quit with unsaved work, and launch/quit on real allowlisted apps on macOS 13 and current macOS. Intel was cross-built; execution on an Intel Mac has not been verified.
5. Run the relevant AT-06 through AT-11 hardware scenarios and consecutive-run gates with a spare ESP32 and test Mac. Check actual ledger verdicts for sleep, wake, restart, shutdown, lock/unlock, network loss, revoked pairing, blocked quit, and transport changes.
6. Exercise Python → Swift → Python on a test installation, including failed cutover and settings differences. Verify a mixed fleet with existing controllers against the same supported protocol-v2 firmware. Unit tests validate migration data; a real legacy service has not been unloaded during development.
7. Obtain Developer ID signatures and notarization, then verify install/update/rollback with that stable signing identity. Ad-hoc builds pin XPC peers to their code hashes and are intended for local development.

Existing Python installations and the live ESP32 were left untouched. Firmware source and distribution images were updated locally. No Developer ID identity is available on this Mac; the release package is ad hoc signed, not notarized. No hardware power actions, real pairing/import, production service cutover, or firmware flash was performed.
