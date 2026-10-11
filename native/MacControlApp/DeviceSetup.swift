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
            guard agent.state.configuration.hostname.isEmpty || replacePairing else { throw AgentFailure("This Mac already has an endpoint. Choose Prepare for another Mac or explicitly allow replacing this Mac's pairing.") }
        }
    }
    func begin(agent: AppModel, networkOnly: Bool = false) {
        do { try validate(agent: agent, networkOnly: networkOnly) } catch { self.error = error.localizedDescription; return }
        busy = true; Self.operationIsRunning = true; error = nil; completed = false; fraction = nil
        let credentials = SetupCredentials(ssid: ssid, wifiPassword: openNetwork ? "" : wifiPassword, adminPassword: adminPassword)
        let selectedPort = port, shouldFlash = flashFirmware, shouldPair = pairThisMac
        task = Task {
            defer { busy = false; Self.operationIsRunning = false; task = nil; fraction = nil }
            do {
                if shouldPair, await LegacyAgent.active() {
                    throw AgentFailure("The Python agent is active. Choose Prepare for another Mac, or use the existing Python migration workflow before pairing the native agent.")
                }
                if !networkOnly {
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
                    }).run(credentials: credentials, firmwareVersion: package.manifest.version, waitForWiFi: shouldPair) { [self] message in await self.setStatus(message) }
                    prepared = result; wifiPassword = ""
                }
                guard let record = prepared else { throw AgentFailure("Complete USB setup first") }
                if !shouldPair {
                    controllerAddress = record.hostname + ".local"
                    status = "USB setup complete. Credentials are saved. Verify Wi-Fi and pair the agent on the client’s Mac before deployment."
                    completed = true
                    return
                }
                status = "Discovering and verifying this controller on the network…"
                controllerAddress = record.hostname + ".local"
                var controller = try ControllerSetupClient(host: controllerAddress)
                do { try await controller.verify(record, timeout: 10) }
                catch {
                    try Task.checkCancellation()
                    controllerAddress = record.ip; controller = try ControllerSetupClient(host: controllerAddress)
                    try await controller.verify(record)
                }
                if shouldPair {
                    try Task.checkCancellation(); status = "Opening the pairing window and pairing this Mac…"
                    let code = try await controller.pairingCode(adminPassword: record.adminPassword)
                    await agent.operate("pair", payload: ["hostname": controllerAddress, "port": "80", "code": code])
                    guard agent.error == nil else { throw AgentFailure(agent.error!) }
                    try Task.checkCancellation(); status = "Starting and verifying the native agent session…"
                    await agent.operate("start")
                    guard agent.error == nil else { throw AgentFailure(agent.error!) }
                    try await controller.verifyAgent(instanceID: agent.state.configuration.instanceID, readKey: record.keys["READ"]!.raw)
                    status = "Setup complete. The ESP32 confirmed this Mac's native agent session."
                } else { status = "Controller ready for delivery. This Mac's agent pairing was not changed." }
                completed = true
            } catch is CancellationError {
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
        status = "Connect the next client's ESP32 COM port and enter its credentials."; refreshPorts()
    }
    func loadSavedCredentials() {
        guard !busy else { return }
        do {
            guard let raw = try TokenStore.read("provisioning." + selectedSavedID) else { throw AgentFailure("Saved credentials are unavailable in Keychain") }
            let record = try JSONDecoder().decode(PreparedEndpoint.self, from: Data(raw.utf8))
            remember(record); controllerAddress = record.hostname + ".local"; completed = false; pairThisMac = false; error = nil
            status = "Saved client credentials loaded. Export them or open the Controller on the client’s network."
        } catch { self.error = "Could not load the selected client credentials from Keychain" }
    }
    private func setStatus(_ value: String) { status = value; fraction = nil }
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
    @Environment(\.openURL) private var openURL
    @State private var review = false
    var body: some View {
        GroupBox("Set up a client's ESP32 over USB") {
            VStack(alignment: .leading, spacing: 14) {
                Text("Flash and configure an ESP32-S3 with 8 MB or 16 MB flash. Connect the COM/UART port; its separate USB port supplies HID to the controlled Mac.").foregroundStyle(.secondary)
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
                Picker("After USB setup", selection: $setup.pairThisMac) { Text("Pair and start this Mac's agent").tag(true); Text("Prepare for another Mac").tag(false) }.disabled(setup.busy)
                if setup.pairThisMac && !agent.state.configuration.hostname.isEmpty {
                    Toggle("Replace this Mac's existing endpoint pairing", isOn: $setup.replacePairing).disabled(setup.busy)
                }
                if !setup.savedDeviceIDs.isEmpty {
                    HStack {
                        Picker("Saved client setup", selection: $setup.selectedSavedID) { Text("Select device").tag(""); ForEach(setup.savedDeviceIDs, id: \.self) { Text($0).tag($0) } }
                        Button("Load credentials") { setup.loadSavedCredentials() }.disabled(setup.selectedSavedID.isEmpty)
                    }.disabled(setup.busy)
                }
                Text("Creates READ, CONTROL, and ADMIN keys and saves the credentials in Keychain. Existing keys captured by this wizard are reused on retry.").font(.caption).foregroundStyle(.secondary)
                if let error = setup.error { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).textSelection(.enabled) }
                Text(setup.status).textSelection(.enabled)
                if setup.busy {
                    if let fraction = setup.fraction { ProgressView(value: fraction) } else { ProgressView().progressViewStyle(.linear) }
                    Button("Cancel setup") { setup.cancel() }
                } else {
                    HStack {
                        Button(setup.completed ? "Set up again…" : "Review and set up…") {
                            do { try setup.validate(agent: agent); review = true } catch { setup.error = error.localizedDescription }
                        }.buttonStyle(.borderedProminent).disabled(agent.busy)
                        if setup.prepared != nil {
                            if !setup.completed && !(setup.prepared?.ip.isEmpty ?? true) { Button("Finish network / pairing") { setup.begin(agent: agent, networkOnly: true) }.disabled(agent.busy) }
                            Button("Export client credentials…") { setup.exportCredentials() }
                            Button("New client device") { setup.resetForNextDevice() }
                        }
                    }
                }
                if let record = setup.prepared {
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
            Button("Set up device") { setup.begin(agent: agent) }
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
