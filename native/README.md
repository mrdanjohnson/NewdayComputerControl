# MacControl native macOS app

Swift replacement for the Python Mac agent, with a SwiftUI settings window and menu-bar frontend. The app targets macOS 13 or later. The packaged development build contains Apple silicon and Intel binaries.

The existing ESP32 still owns the command ledger, verifies outcomes, and performs USB HID wake/lock/unlock. This app uses its existing protocol-v2 agent interface. It does not require changing the ESP32 firmware or existing controllers. Python remains available for legacy Macs; no Python source, installer, configuration, or firmware was changed by this implementation.

## Open or build

Open `native/MacControl.xcodeproj` in Xcode and select the **MacControl** scheme. The local Swift package has no third-party dependencies. From the project root:

```sh
native/Scripts/build.sh
```

Outputs:

- `native/build/MacControl.app`
- `native/build/MacControl-2.0.0.zip`
- `native/build/release-build.log`

The default build uses an ad-hoc signature and Hardened Runtime. It is a local development build. Developer ID signing and notarization are still needed for distribution to other users. Code-signing settings can be overridden as Xcode build arguments; distribution credentials are not included.

## First installation

1. Copy the built `MacControl.app` into `/Applications` and open it.
2. In **Setup & pairing**, choose **Register service**. If macOS requires approval, open System Settings and enable the MacControl background item in Login Items.
3. For a new installation, open the pairing window in the ESP32 Web UI. Discover the endpoint or enter its hostname/address and pairing code, then choose **Pair**.
4. Add applications under **Allowed applications**, enable the desired **Command permissions**, and save. New installations start with every action disabled.
5. Choose **Start agent**. Overview shows the connection, last heartbeat sent, known lock state, and latest action. Closing or quitting the frontend leaves an active background agent running.

Choose **Open Controller** from the MacControl menu-bar icon to open the ESP32 Web UI in your default browser for macros, presets, and other controller settings. It uses the agent's saved hostname/IP and port and is disabled until an endpoint is configured. The shortcut is available even when the agent session is stopped.

The service runs as the logged-in user. Allow local-network access when macOS requests it. Restart and shutdown may require Automation permission for System Events. Permission attribution in the supervised service still needs hardware acceptance testing.

## Replace an existing Python installation

Register the native service first, then choose **Import Python settings…** and select the existing `~/.maccontrol/agent.json`. Import stages the token in Keychain and creates a private, exact backup of the original file. It preserves endpoint, identity, command flags, allowlist, transport, interval, and compatible boot identity. It does not connect the native agent or stop Python.

Choose **Take over from Python** when ready. The app requires the existing `com.maccontrol.agent` LaunchAgent plist, verifies that the imported file has not changed, unloads the old service, and starts native operation. It checks hello, the initial outbound burst, and two transmitted heartbeats. A failed cutover stops native operation before restoring Python. A manually started Python process must be stopped separately; the app will not kill it.

**Restore Python** stops native operation and reloads the retained legacy service using its existing settings. Native settings changes are not copied into Python; the app reports differences. The Python runtime and configuration are never uninstalled. A mixed fleet can use Python and Swift independently on different Macs, each paired to its own ESP32.

This first release accepts at most 24 allowlisted applications and enforces a byte budget for the firmware's 4 KB capability report. An oversized legacy allowlist is refused without truncating or changing the original. Older protocol-v1 firmware is outside this release's verified baseline.

## Runtime and data

- `MacControl.app` owns the frontend. Its embedded `MacControlAgentService` owns the session, observers, credentials, and configuration.
- The bundled per-user LaunchAgent is `dev.maccontrol.native.agent`, registered through `SMAppService`. It remains available while the frontend is closed.
- XPC checks both executable signing identities and the connecting user. Its interface contains status and settings operations; it has no generic command-execution operation.
- Native state lives in `~/Library/Application Support/MacControlNative/`, separate from Python. The directory is private and configuration/backups are mode 0600. Tokens live in Keychain. Legacy backups still contain the original plaintext credential and remain protected rollback data.
- The five actions are launch, graceful quit, sleep, restart, and shutdown. The allowlist and enable switches are checked on every dispatch. No force quit, shell command, or endpoint-supplied script is accepted.
- Transport selection is explicit: WebSocket or HTTP polling. Session queues are bounded; reconnect discards old-session events. WebSocket heartbeats use a bounded ping/pong probe to detect silent drops. Permanent refusal remains halted across service restart until deliberate repair. Repeated crashes trigger a recovery cooldown.
- Screen-lock observation uses the same legacy key/notification family as Python. Missing observations remain **Unknown** and do not generate false unlocked evidence. This needs validation on each supported macOS version.
- Power initiation reports are sent before execution. The ESP32 decides whether the transition occurred. If the pre-power drain fails, native refuses the action. Graceful quit and launch require observed app state; launch callbacks have a deadline.

Use **Stop & unregister** in Background operation to remove native background registration. **Remove native pairing…** removes local native credentials without revoking ESP32 pairing or deleting Python files. Restore Python before unregistering if it should resume control.

## Verification

```sh
native/Scripts/test.sh
```

This runs Swift tests with synthetic executors, checks their captured events with the unchanged shipping firmware parser, builds Debug, and performs signed/unsigned XPC checks using temporary unpaired state. It briefly registers a temporary isolated native test job and removes it afterward. It refuses to run if the native service is already loaded. It never sends commands to a real ESP32 or changes the Python service.

The firmware parser check uses the existing PlatformIO native ArduinoJson dependency in `firmware/.pio/libdeps/native/ArduinoJson/src`; the script reports if it is absent. See [VERIFICATION.md](VERIFICATION.md) for completed checks and remaining release gates.
