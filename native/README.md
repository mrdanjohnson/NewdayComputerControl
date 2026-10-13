# MacControl native macOS app

Swift replacement for the Python Mac agent, with a SwiftUI settings window and menu-bar frontend. The app targets macOS 13 or later. The packaged development build contains Apple silicon and Intel binaries.

The existing ESP32 still owns the command ledger, verifies outcomes, and performs USB HID wake/lock/unlock. This app uses its existing protocol-v2 agent interface. Existing configured protocol-v2 controllers continue to work with the native agent. The USB setup wizard bundles firmware 1.6.1-phase6 with additive JSON console commands for credentials containing spaces and quotes. Python remains available for legacy Macs; its source, installer, and configuration are unchanged.

## Open or build

Open `native/MacControl.xcodeproj` in Xcode and select the **MacControl** scheme. The local Swift package has no third-party dependencies. The build downloads checksum-pinned Espressif standalone esptool 5.5.0 for Apple silicon and Intel and embeds it, firmware, provenance, license, and the exact esptool source archive. The client needs no Python, PlatformIO, Xcode, or Terminal installation. See [Espressif’s standalone installation documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html). The initial build needs network access; subsequent builds use verified cached archives. From the project root:

```sh
native/Scripts/build.sh
```

Outputs:

- `native/build/MacControl.app`
- `native/build/MacControl-2.1.5.zip`
- `native/build/release-build.log`

Use an Apple-issued identity for persistent background-service testing. Ad hoc signatures change identity across rebuilds and can leave SMAppService enforcing an old code hash. Sign both the frontend and helper with the same identity:

```sh
MACCONTROL_SIGNING_IDENTITY='Apple Development: Your Name (CERTIFICATEID)' native/Scripts/build.sh
```

For repeated local builds, put that certificate name or SHA-1 fingerprint in the ignored `native/build/local-signing-identity` file. This Mac is configured that way. Without an environment variable, local identity file, or explicit Xcode signing argument, the build falls back to ad hoc signing for isolated tests. Hardened Runtime remains enabled. Developer ID signing and notarization are still needed for distribution to other users; signing keys are not included in this project.

## Sign a client distribution

This development Mac currently has Apple Development identities but **no Developer ID Application identity**. The current 2.1.5 ZIP is signed with Apple Development and is not notarized. Once you install your Developer ID Application certificate and configure a `notarytool` Keychain profile, run:

```sh
export MACCONTROL_DEVELOPER_ID='Developer ID Application: Your Name (TEAMID)'
export MACCONTROL_TEAM_ID='TEAMID'
export MACCONTROL_NOTARY_PROFILE='your-existing-profile'
native/Scripts/distribute.sh
```

The script rebuilds both architectures and embedded flashers with that identity, submits the ZIP for notarization, staples/validates the ticket, checks Gatekeeper acceptance, and recreates the client ZIP. It refuses Apple Development/ad hoc identities. Account credentials are read from your existing Keychain profile, never stored in this project. This distribution path has not been run because the required identity/profile are unavailable here. Retest install, Keychain access, service upgrade, and a spare-board flash using the final signed package.

## Resume setup after the 2.1.1 background-service timeout

Use 2.1.5 or later, installed in Applications, and quit older frontend copies. In Setup & pairing, click Register service to repair an enabled but unresponsive registration; approve the background item if macOS requests it. Choose Resume saved setup, select the board, and click Load saved setup. Keep Pair and start on this Mac selected, then click Resume pairing when available. This reuses the saved admin/API credentials and IP without flashing or rewriting USB credentials. Confirm Overview reports Connected. If no saved network IP is available, complete USB setup with flashing disabled.

## Setup progress and recovery

Setup & pairing separates **New ESP32-S3**, **Resume saved setup**, and **Configured ESP32**. New setup shows five steps: **This Mac → USB setup → Verify Wi-Fi → Pair this Mac → Connected**. The panel names the next action, marks completed steps, and keeps a failed or cancelled step highlighted. Firmware upload shows its measured progress; other operations show their current stage rather than an estimated total percentage.

Saved credentials alone do not mark Wi-Fi or connection as verified. Resume saved setup loads the selected device from Keychain without flashing. If that device is already paired to this Mac, resuming starts and verifies the existing pairing. Preparing for another Mac shows a separate USB/Wi-Fi/handoff sequence and does not claim this Mac is connected.

## First installation

1. Copy the built `MacControl.app` into `/Applications` and open it.
2. In **Setup & pairing**, choose **Register service**. If macOS requires approval, open System Settings and enable the MacControl background item in Login Items.
3. Choose New ESP32-S3 for the USB setup workflow below. Choose Configured ESP32 for an already configured controller, open its pairing window in the ESP32 Web UI, enter its hostname/address and pairing code, and choose **Pair**.
4. Add applications under **Allowed applications**, enable the desired **Command permissions**, and save. New installations start with every action disabled.

On macOS 15 or later, allow Local Network access for MacControl and MacControl Agent when requested. The first request may fail before the permission dialog is answered; retry network pairing after allowing access. Review permission under System Settings → Privacy & Security → Local Network. Quit older app copies and use the installed Applications copy.

Support commands are available from the installed app's executable. They print only a status summary, keep credentials in Keychain, and use the same registration and network setup operations as the UI:

```sh
/Applications/MacControl.app/Contents/MacOS/MacControl --background-status
/Applications/MacControl.app/Contents/MacOS/MacControl --repair-background-service
/Applications/MacControl.app/Contents/MacOS/MacControl --resume-saved-setup DEVICE_ID
```

The last command requires an existing complete saved USB record and finishes network pairing without opening a serial port or flashing. It refuses to replace an existing native endpoint. Handle any macOS Keychain or network permission prompts directly.
5. Choose **Start agent**. Overview shows the connection, last heartbeat sent, known lock state, and latest action. Closing or quitting the frontend leaves an active background agent running.

Choose **Open Controller** from the MacControl menu-bar icon to open the ESP32 Web UI in your default browser for macros, presets, and other controller settings. It uses the agent's saved hostname/IP and port and is disabled until an endpoint is configured. The shortcut is available even when the agent session is stopped.

The service runs as the logged-in user. Allow local-network access when macOS requests it. Restart and shutdown may require Automation permission for System Events. Permission attribution in the supervised service still needs hardware acceptance testing.

## Flash and provision a client's ESP32

This wizard accepts **ESP32-S3 boards with 8 MB or 16 MB flash**. Both use the bundled DevKitC-1 N8 firmware and its 8 MB partition layout; the additional space on 16 MB boards remains unused. Physical flashing on the newly inspected 16 MB board is pending. The WROOM target still compiles for legacy use, but its firmware is not offered by this wizard.

1. Connect the ESP32's **COM/UART** port using a USB data cable. The other USB port supplies HID to the controlled Mac. Install the USB bridge driver if the port does not appear. Opening/closing serial may reset the controller.
2. Open **Setup & pairing → New ESP32-S3**. Refresh and select the correct serial port, enter the client's 2.4 GHz SSID/password, and set/confirm a controller admin password. Explicitly select the open-network switch only if the network has no password.
3. Keep **Flash bundled firmware** enabled for a new board. The app checks the chip, requires a detected flash capacity of 8 MB or 16 MB, validates SHA-256 for all four images, writes fixed addresses, and requires esptool's verification result. The review dialog identifies the port and describes partition/data replacement. Back up a used board before flashing: migrating an older partition layout can lose API keys, macros, triggers, and ledger data. This is not a general firmware-image chooser or an OTA updater.
4. Select a workflow. **Pair and start on this Mac** requires an approved native background service and access to the client's network. **Prepare for another Mac** writes credentials over USB without waiting for the client's Wi-Fi or changing the technician Mac's pairing. Existing Mac pairing replacement requires the explicit switch. Automatic pairing refuses an active Python agent before touching the board; use migration or technician preparation in that case.
5. Choose **Review and begin USB setup…**, review the device, then **Begin setup**. Setup saves the admin password, creates READ/CONTROL/ADMIN keys, stores each key immediately in Keychain, then saves Wi-Fi. Creating keys before starting Wi-Fi reduces exposure to the known fresh-board watchdog overlap; its firmware root cause remains open.
6. When pairing on this Mac, the wizard waits for a Wi-Fi IP, closes serial, verifies the network device ID against the physical-console ID using the READ key, opens an authenticated pairing window, pairs/starts the service, and confirms the matching active agent session. It tries the hostname first and the serial-reported IP as fallback. All command permissions remain at their current settings; new installations have every action disabled.
7. Choose **Export client credentials…** for handoff. The JSON includes controller identity/address, admin password and API keys; it excludes the Wi-Fi password and is written with mode 0600. **Resume saved setup → Load saved setup** retrieves captured credentials from Keychain after restarting the frontend. A partial record can also be exported after an interrupted setup. **Clear for a new S3** clears the form and enables flashing for the next board. Export creates a private temporary file before atomically replacing the chosen file; it does not change the parent folder permissions.

For technician preparation, completion means the USB credentials were saved, **not** that the client network or agent session was verified. At the client's Mac, connect the HID USB port, put the controller on the configured Wi-Fi, open `<hostname>.local` in a browser with its admin password, open the pairing window, and use **Configured ESP32**. Register the background service, configure applications/permissions, and start the agent. Verify the connection before handoff.

Cancel terminates only the wizard's own flasher process. An interrupted flash must be repeated before deployment. After a successful flash the checkbox clears, so a provisioning retry does not flash again automatically. Previously captured keys are reused only when the console reports the same role and an active key ID. A lost reply to a key-create command is not automatically replayed; inspect the key store before retrying. A full key store needs unused keys revoked in the Controller. No raw console replies or credentials are displayed in errors or logs.

The Wi-Fi password is used for serial provisioning and cleared from the form after success; it is not written to a Mac credential record. ESP32 credential storage retains its existing NVS/LittleFS behavior. The wizard's admin/API credentials are separate from the background agent's pairing token. Service/configuration controls are disabled during USB setup to prevent competing changes, and quitting the frontend is blocked until setup finishes or is cancelled.

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
