import SwiftUI
import MacControlCore

@MainActor final class DeviceSetupModel: ObservableObject {
    static var operationIsRunning = false
    @Published var ports = SerialPorts.available()
    @Published var port = ""
    @Published var ssid = ""
    @Published var wifiPassword = ""
    @Published var openNetwork = false
    @Published var adminPassword = ""
    @Published var confirmPassword = ""
    @Published var flashFirmware = true
    @Published var pairThisMac = true
    @Published var replacePairing = false
    @Published var busy = false
    @Published var status = "Connect the ESP32-S3 COM port with a USB data cable."
    @Published var fraction: Double?
    @Published var error: String?
    @Published var completed = false
    @Published var prepared: PreparedEndpoint?
    @Published var controllerAddress = ""
    @Published var savedDeviceIDs = UserDefaults.standard.stringArray(forKey: "MacControlPreparedDeviceIDs") ?? []
    @Published var selectedSavedID = ""
    @Published var step: SetupStep?
    @Published var networkVerified = false
    @Published var pairingSaved = false
    @Published var paused = false
    private var task: Task<Void, Never>?
    let firmwareVersion: String = {
        guard let directory = Bundle.main.resourceURL?.appendingPathComponent("Firmware/manifest.json"),
              let data = try? Data(contentsOf: directory), let manifest = try? JSONDecoder().decode(FirmwareManifest.self, from: data) else { return "Firmware unavailable" }
        return manifest.version
    }()
    func firmware() throws -> FirmwareBundle {
        guard let resources = Bundle.main.resourceURL else { throw AgentFailure("App resources unavailable") }
        return try FirmwareBundle(directory: resources.appendingPathComponent("Firmware"))
    }
    func refreshPorts() { ports = SerialPorts.available(); if !ports.contains(port) { port = ports.count == 1 ? ports[0] : "" } }
    func matchesEndpoint(_ hostname: String) -> Bool {
        guard let record = prepared, !hostname.isEmpty else { return false }
        return [record.hostname + ".local", record.ip].contains(hostname)
    }
    func validate(agent: AppModel, networkOnly: Bool = false) throws {
        guard !busy, !agent.busy, !Self.operationIsRunning else { throw AgentFailure("Another setup operation is in progress") }
        if !networkOnly {
            guard openNetwork || !wifiPassword.isEmpty else { throw AgentFailure("Enter the Wi-Fi password, or explicitly select an open network") }
            try SetupCredentials(ssid: ssid, wifiPassword: openNetwork ? "" : wifiPassword, adminPassword: adminPassword).validate()
            guard adminPassword == confirmPassword else { throw AgentFailure("Admin passwords do not match") }
            guard ports.contains(port), SerialPorts.valid(port) else { throw AgentFailure("Select the ESP32 COM serial port") }
            _ = try firmware()
        }
        if networkOnly {
            guard let record = prepared, !record.ip.isEmpty, ["READ", "CONTROL", "ADMIN"].allSatisfy({ record.keys[$0] != nil }) else { throw AgentFailure("Network setup requires a connected controller. Complete USB setup on the client network first.") }
        }
        if pairThisMac {
            guard agent.serviceStatus == "Enabled" else { throw AgentFailure("Register and approve the background service before pairing this Mac, or choose Prepare for another Mac") }
            guard agent.state.configuration.hostname.isEmpty || replacePairing || (networkOnly && matchesEndpoint(agent.state.configuration.hostname)) else { throw AgentFailure("This Mac already has an endpoint. Choose Prepare for another Mac or explicitly allow replacing this Mac's pairing.") }
        }
    }
    func begin(agent: AppModel, networkOnly: Bool = false) {
        do { try validate(agent: agent, networkOnly: networkOnly) } catch { self.error = error.localizedDescription; return }
        busy = true; Self.operationIsRunning = true; error = nil; completed = false; fraction = nil
        paused = false; networkVerified = false; pairingSaved = false
        step = pairThisMac ? .mac : .usb
        let credentials = SetupCredentials(ssid: ssid, wifiPassword: openNetwork ? "" : wifiPassword, adminPassword: adminPassword)
        let selectedPort = port, shouldFlash = flashFirmware, shouldPair = pairThisMac
        task = Task {
            defer { busy = false; Self.operationIsRunning = false; task = nil; fraction = nil }
            do {
                if shouldPair {
                    status = "Checking this Mac's background service…"
                    do { _ = try await agent.fetch() }
                    catch { throw AgentFailure("The background service is not responding. Click Register service to repair its registration, then retry setup without flashing.") }
                }
                if shouldPair, await LegacyAgent.active() {
                    throw AgentFailure("The Python agent is active. Choose Prepare for another Mac, or use the existing Python migration workflow before pairing the native agent.")
                }
                if !networkOnly {
                    step = .usb
                    prepared = nil; controllerAddress = ""
                    let package = try firmware()
                    if shouldFlash {
                        status = "Checking the ESP32-S3 and flashing verified firmware…"
                        guard let resources = Bundle.main.resourceURL else { throw AgentFailure("Missing app resources") }
                        try await FlashTool(resources: resources).flash(firmware: package, port: selectedPort) { [self] value in await self.setFraction(value) }
                        flashFirmware = false
                    }
                    try Task.checkCancellation()
                    status = "Opening the MacControl setup console…"
                    let console = try NativeSerialConsole(port: selectedPort)
                    let result = try await SerialProvisioner(console: console, save: { [self] record in
                        try TokenStore.write(String(decoding: JSONEncoder().encode(record), as: UTF8.self), id: record.credentialAccount)
                        await self.remember(record)
                    }).run(credentials: credentials, firmwareVersion: package.manifest.version, waitForWiFi: shouldPair,
                           step: { [self] value in await self.setStep(value) }) { [self] message in await self.setStatus(message) }
                    prepared = result; wifiPassword = ""
                }
                guard let record = prepared else { throw AgentFailure("Complete USB setup first") }
                if !shouldPair {
                    controllerAddress = record.hostname + ".local"
                    status = "USB setup complete. Credentials are saved. Verify Wi-Fi and pair the agent on the client’s Mac before deployment."
                    completed = true
                    step = nil
                    return
                }
                step = .network; status = "Discovering and verifying this controller on the network…"
                controllerAddress = record.hostname + ".local"
                var controller = try ControllerSetupClient(host: controllerAddress)
                do { try await controller.verify(record, timeout: 10) }
                catch {
                    try Task.checkCancellation()
                    controllerAddress = record.ip; controller = try ControllerSetupClient(host: controllerAddress)
                    try await controller.verify(record)
                }
                networkVerified = true
                if shouldPair {
                    if !networkOnly || !matchesEndpoint(agent.state.configuration.hostname) {
                        try Task.checkCancellation(); step = .pairing; status = "Opening the pairing window and pairing this Mac…"
                        let code = try await controller.pairingCode(adminPassword: record.adminPassword)
                        await agent.operate("pair", payload: ["hostname": controllerAddress, "port": "80", "code": code])
                        guard agent.error == nil else { throw AgentFailure(agent.error!) }
                    }
                    pairingSaved = true
                    try Task.checkCancellation(); step = .connection; status = "Starting and verifying the native agent session…"
                    await agent.operate("start")
                    guard agent.error == nil else { throw AgentFailure(agent.error!) }
                    try await controller.verifyAgent(instanceID: agent.state.configuration.instanceID, readKey: record.keys["READ"]!.raw)
                    status = "Setup complete. The ESP32 confirmed this Mac's native agent session."
                } else { status = "Controller ready for delivery. This Mac's agent pairing was not changed." }
                completed = true
                step = nil
            } catch is CancellationError {
                paused = true
                status = "Setup cancelled. If flashing was interrupted, flash again before deployment. Completed credentials remain in Keychain."
            } catch {
                self.error = error.localizedDescription; status = "Setup stopped. Completed credentials remain in Keychain."
            }
        }
    }
    func cancel() { task?.cancel(); status = "Cancelling setup…" }
    private func remember(_ record: PreparedEndpoint) {
        prepared = record
        if !savedDeviceIDs.contains(record.deviceID) {
            savedDeviceIDs.append(record.deviceID); savedDeviceIDs.sort()
            UserDefaults.standard.set(savedDeviceIDs, forKey: "MacControlPreparedDeviceIDs")
        }
        selectedSavedID = record.deviceID
    }
    func resetForNextDevice() {
        guard !busy else { return }
        ssid = ""; wifiPassword = ""; adminPassword = ""; confirmPassword = ""; openNetwork = false
        flashFirmware = true; replacePairing = false; prepared = nil; completed = false; error = nil; controllerAddress = ""
        step = nil; networkVerified = false; pairingSaved = false; paused = false
        status = "Connect the next client's ESP32 COM port and enter its credentials."; refreshPorts()
    }
    func loadSavedCredentials() {
        guard !busy else { return }
        do {
            guard let raw = try TokenStore.read("provisioning." + selectedSavedID) else { throw AgentFailure("Saved credentials are unavailable in Keychain") }
            let record = try JSONDecoder().decode(PreparedEndpoint.self, from: Data(raw.utf8))
            remember(record); controllerAddress = record.hostname + ".local"; completed = false; error = nil
            step = nil; networkVerified = false; pairingSaved = false; paused = false
            flashFirmware = false
            status = "Saved USB setup loaded. Click Resume pairing to connect this Mac, or choose Prepare for another Mac to export the credentials."
        } catch { self.error = "Could not load the selected client credentials from Keychain" }
    }
    private func setStatus(_ value: String) { status = value; fraction = nil }
    private func setStep(_ value: SetupStep) { step = value }
    private func setFraction(_ value: Double?) { fraction = value }
    func exportCredentials() {
        guard let record = prepared else { return }
        let picker = NSSavePanel(); picker.allowedContentTypes = [.json]; picker.nameFieldStringValue = "MacControl-\(record.hostname)-credentials.json"
        picker.message = "Save the client's controller address, admin password, and API keys. The Wi-Fi password is not included."
        if picker.runModal() == .OK, let url = picker.url {
            do {
                try record.export(to: url)
            } catch { self.error = "Could not export client credentials" }
        }
    }
}
struct DeviceSetupView: View {
    @ObservedObject var setup: DeviceSetupModel
    @ObservedObject var agent: AppModel
    var resumeOnly = false
    @Environment(\.openURL) private var openURL
    @State private var review = false
    private var serviceReady: Bool { agent.serviceStatus == "Enabled" && !["inactive", "unavailable"].contains(agent.state.phase) }
    var body: some View {
        GroupBox(resumeOnly ? "Saved S3 setup" : "ESP32-S3 details") {
            VStack(alignment: .leading, spacing: 14) {
                if resumeOnly {
                    if setup.savedDeviceIDs.isEmpty {
                        Text("No saved USB setup was found. Choose New ESP32-S3 above to prepare a device.").foregroundStyle(.secondary)
                    } else {
                        HStack {
                            Picker("Saved device", selection: $setup.selectedSavedID) { Text("Select S3").tag(""); ForEach(setup.savedDeviceIDs, id: \.self) { Text($0).tag($0) } }
                            Button("Load saved setup") { setup.loadSavedCredentials() }.disabled(setup.selectedSavedID.isEmpty)
                        }.disabled(setup.busy || agent.busy)
                    }
                    Text("This reuses the saved controller credentials. Keep the S3 powered and connected to the same network as this Mac.").font(.callout).foregroundStyle(.secondary)
                } else {
                Text("Use an ESP32-S3 with 8 MB or 16 MB flash. Connect COM/UART for setup; connect its separate USB/HID port to the controlled Mac.").foregroundStyle(.secondary)
                HStack {
                    Picker("Serial port", selection: $setup.port) { Text("Select COM port").tag(""); ForEach(setup.ports, id: \.self) { Text($0).tag($0) } }
                    Button("Refresh ports") { setup.refreshPorts() }
                }.disabled(setup.busy)
                if setup.ports.isEmpty { Text("No USB serial port found. Check the data cable and the bridge driver.").font(.caption).foregroundStyle(.secondary) }
                TextField("Client Wi-Fi name (SSID, 2.4 GHz)", text: $setup.ssid).disabled(setup.busy)
                Toggle("Client network has no Wi-Fi password", isOn: $setup.openNetwork).disabled(setup.busy)
                if !setup.openNetwork { SecureField("Wi-Fi password", text: $setup.wifiPassword).disabled(setup.busy) }
                SecureField("Controller admin password (10–128 bytes)", text: $setup.adminPassword).disabled(setup.busy)
                SecureField("Confirm controller admin password", text: $setup.confirmPassword).disabled(setup.busy)
                Toggle("Flash bundled firmware: \(setup.firmwareVersion)", isOn: $setup.flashFirmware).disabled(setup.busy)
                Text(setup.flashFirmware ? "The app uploads firmware, resets the board, then saves Wi-Fi and controller credentials." : "Firmware upload will be skipped. Continue saving credentials over COM/UART.").font(.caption).foregroundStyle(.secondary)
                }
                Picker("Set up for", selection: $setup.pairThisMac) { Text("Pair and start on this Mac").tag(true); Text("Prepare for another Mac").tag(false) }.disabled(setup.busy)
                if setup.pairThisMac && !agent.state.configuration.hostname.isEmpty && (!resumeOnly || !setup.matchesEndpoint(agent.state.configuration.hostname)) {
                    Toggle("Replace this Mac's existing endpoint pairing", isOn: $setup.replacePairing).disabled(setup.busy)
                }
                if !resumeOnly { Text("Wi-Fi, admin access and three API keys are saved in order. Completed keys stay in Keychain if setup stops.").font(.caption).foregroundStyle(.secondary) }
                if let error = setup.error { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).textSelection(.enabled) }
                if !setup.busy { Text(setup.status).font(.callout).textSelection(.enabled) }
                if setup.busy {
                    Button("Cancel setup") { setup.cancel() }
                } else {
                    VStack(alignment: .leading, spacing: 10) {
                        if !resumeOnly {
                        Button(setup.error != nil || setup.paused ? "Review and retry USB setup…" : "Review and begin USB setup…") {
                            do { try setup.validate(agent: agent); review = true } catch { setup.error = error.localizedDescription }
                        }.buttonStyle(.borderedProminent).disabled(agent.busy || (setup.pairThisMac && !serviceReady))
                        }
                        if setup.prepared != nil && (!resumeOnly || setup.prepared?.deviceID == setup.selectedSavedID) {
                            if !setup.completed && !(setup.prepared?.ip.isEmpty ?? true) && setup.pairThisMac {
                                Button("Resume pairing") { setup.begin(agent: agent, networkOnly: true) }.buttonStyle(.borderedProminent).disabled(agent.busy || !serviceReady)
                            } else if resumeOnly && setup.prepared?.ip.isEmpty == true {
                                Text("No network address was saved. Choose New ESP32-S3 and retry USB setup with Flash firmware off to finish joining Wi-Fi.").font(.caption).foregroundStyle(.secondary)
                            }
                            HStack {
                            Button("Export client credentials…") { setup.exportCredentials() }
                            if !resumeOnly { Button("Clear for a new S3") { setup.resetForNextDevice() } }
                            }
                        }
                    }
                }
                if let record = setup.prepared, !resumeOnly || record.deviceID == setup.selectedSavedID {
                    Divider()
                    Text("\(record.hostname) · \(record.deviceID)").font(.headline)
                    Text("Controller: \(setup.controllerAddress.isEmpty ? (record.ip.isEmpty ? record.hostname + ".local" : record.ip) : setup.controllerAddress)").font(.caption).textSelection(.enabled)
                    if !setup.controllerAddress.isEmpty, let url = URL(string: "http://\(setup.controllerAddress)/") { Button("Open this Controller") { openURL(url) } }
                    if setup.completed && !setup.pairThisMac { Text("On the client's Mac, register the native service, open this Controller and its pairing window, then enter the pairing code in Setup & pairing. Export the credentials before handoff.").font(.callout).foregroundStyle(.secondary) }
                }
            }.padding(12).frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { setup.refreshPorts() }
        .alert("Set up the selected ESP32?", isPresented: $review) {
            Button("Cancel", role: .cancel) { }
            Button("Begin setup") { setup.begin(agent: agent) }
        } message: {
            Text("Port: \(setup.port)\nWi-Fi: \(setup.ssid)\n\(setup.flashFirmware ? "Flash firmware and partition table. This can replace existing device data; use a new board or back up an existing one first." : "Keep firmware and configure credentials.")\n\(setup.pairThisMac ? "Pair this Mac after setup." : "Prepare for delivery without changing this Mac's pairing.") Opening the COM port resets the device.")
        }
    }
}
@MainActor final class SetupAppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard DeviceSetupModel.operationIsRunning else { return .terminateNow }
        let alert = NSAlert(); alert.messageText = "Device setup is running"; alert.informativeText = "Finish or cancel setup in MacControl before quitting. Interrupting a flash can leave the board needing another flash."; alert.addButton(withTitle: "Keep setup running"); alert.runModal()
        return .terminateCancel
    }
}
