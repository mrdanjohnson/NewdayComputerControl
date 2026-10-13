import SwiftUI
import MacControlCore

struct SetupPairingView: View {
    @ObservedObject var agent: AppModel
    @ObservedObject var setup: DeviceSetupModel
    @ObservedObject var discovery: Discovery
    var showOverview: () -> Void
    var confirmMigration: (String) -> Void
    @State private var route: SetupRoute = .newDevice
    @State private var chosenRoute = false

    private var serviceReady: Bool {
        agent.serviceStatus == "Enabled" && !["inactive", "unavailable"].contains(agent.state.phase)
    }
    private var endpointMatches: Bool {
        route == .existingDevice ? !agent.state.configuration.hostname.isEmpty : selectedRecordIsLoaded && setup.matchesEndpoint(agent.state.configuration.hostname)
    }
    private var selectedRecordIsLoaded: Bool { route != .savedDevice || (setup.prepared != nil && setup.prepared?.deviceID == setup.selectedSavedID) }
    private var journey: SetupJourney {
        let useThisMac = route == .existingDevice || setup.pairThisMac
        let working: SetupStep? = setup.busy || setup.error != nil || setup.paused ? setup.step :
            agent.busy ? (agent.currentOperation == "register" ? .mac : agent.currentOperation == "pair" ? .pairing : .connection) : nil
        let livePairing = endpointMatches && (route != .newDevice || setup.pairingSaved)
        return SetupJourney(route: route, pairThisMac: useThisMac, serviceReady: serviceReady,
                            usbComplete: selectedRecordIsLoaded && (setup.prepared.map { record in ["READ", "CONTROL", "ADMIN"].allSatisfy { record.keys[$0] != nil } } ?? false),
                            networkVerified: selectedRecordIsLoaded && setup.networkVerified, pairingSaved: selectedRecordIsLoaded && setup.pairingSaved || livePairing,
                            connected: serviceReady && livePairing && agent.state.phase == "connected",
                            handoffReady: (setup.completed || (route == .savedDevice && selectedRecordIsLoaded && !(setup.prepared?.ip.isEmpty ?? true))) && !setup.pairThisMac,
                            workingOn: working,
                            needsAttention: setup.error != nil || setup.paused || (route == .existingDevice && agent.error != nil))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Picker("What would you like to do?", selection: $route) {
                Text("New ESP32-S3").tag(SetupRoute.newDevice)
                Text("Resume saved setup").tag(SetupRoute.savedDevice)
                Text("Configured ESP32").tag(SetupRoute.existingDevice)
            }.pickerStyle(.segmented).disabled(setup.busy || agent.busy)
            journeyCard
            if route == .existingDevice { manualPairing }
            else { DeviceSetupView(setup: setup, agent: agent, resumeOnly: route == .savedDevice) }
            DisclosureGroup("Background service details") {
                VStack(alignment: .leading, spacing: 10) {
                    Text("Registration: \(agent.serviceStatus) · Service: \(serviceReady ? "Responding" : "Not ready")")
                    Text("The service stays available when this window is closed. Use the MacControl copy in Applications.").foregroundStyle(.secondary)
                    HStack { Button("Check / repair service") { agent.register() }; Button("Open Login Items") { agent.openLoginSettings() } }
                }.padding(.vertical, 10)
            }.disabled(setup.busy || agent.busy)
            DisclosureGroup("Migrate an existing Python agent") {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Import agent.json to preserve pairing and permissions. Python continues running until you choose Take over.").foregroundStyle(.secondary)
                    HStack {
                        Button("Import Python settings…") { agent.importPython() }.disabled(!serviceReady)
                        Button("Take over from Python") { confirmMigration("cutover") }.disabled(agent.state.configuration.legacyStatePath == nil)
                        Button("Restore Python") { confirmMigration("rollback") }.disabled(agent.state.configuration.legacyStatePath == nil)
                    }
                }.padding(.vertical, 10)
            }.disabled(setup.busy || agent.busy)
        }
        .task {
            guard !chosenRoute else { return }
            await agent.refresh()
            guard !chosenRoute else { return }
            route = !agent.state.configuration.hostname.isEmpty ? .existingDevice : setup.savedDeviceIDs.isEmpty ? .newDevice : .savedDevice
        }
        .onChange(of: route) { value in
            chosenRoute = true
            if value == .newDevice { setup.resetForNextDevice() }
        }
    }

    private var journeyCard: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                Text(journey.current == nil ? "Setup complete" : journey.needsAttention ? "Setup needs attention" : "Your setup progress").font(.headline)
                Spacer()
                if let current = journey.current, let index = journey.steps.firstIndex(of: current) { Text("Step \(index + 1) of \(journey.steps.count)").font(.caption).foregroundStyle(.secondary) }
            }
            HStack(alignment: .top, spacing: 0) {
                ForEach(Array(journey.steps.enumerated()), id: \.element) { index, step in
                    VStack(spacing: 7) {
                        Image(systemName: symbol(for: journey.status(of: step), number: index + 1))
                            .font(.title2).foregroundStyle(color(for: journey.status(of: step)))
                            .frame(height: 32)
                        Text(title(for: step)).font(.caption.weight(.medium)).multilineTextAlignment(.center)
                        Text(statusLabel(journey.status(of: step))).font(.caption2).foregroundStyle(.secondary)
                    }.frame(maxWidth: .infinity)
                        .accessibilityElement(children: .ignore)
                        .accessibilityLabel("Step \(index + 1): \(title(for: step)), \(statusLabel(journey.status(of: step)))")
                    if index < journey.steps.count - 1 {
                        Rectangle().fill(journey.completed.contains(step) ? Color.teal : Color.secondary.opacity(0.25)).frame(width: 18, height: 2).padding(.top, 15)
                    }
                }
            }
            Divider()
            nextAction
            if setup.busy {
                Text(setup.status).font(.callout).textSelection(.enabled)
                if let fraction = setup.fraction { ProgressView("Firmware upload", value: fraction).accessibilityValue("\(Int(fraction * 100)) percent") }
                else { ProgressView().progressViewStyle(.linear).accessibilityLabel("Setup in progress") }
                Text("Complete any macOS Keychain or Local Network prompt. Keep the S3 powered while setup runs.").font(.caption).foregroundStyle(.secondary)
            }
        }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
            .background((journey.needsAttention ? Color.orange : Color.teal).opacity(0.07))
            .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    @ViewBuilder private var nextAction: some View {
        if setup.busy || agent.busy {
            Label(setup.busy ? "Setup is running" : "Working on this Mac…", systemImage: "hourglass").font(.headline)
        } else if journey.current == nil {
            Label(setup.pairThisMac || route == .existingDevice ? "Connected — this Mac is ready" : "USB setup complete — ready for handoff", systemImage: "checkmark.circle.fill").font(.headline).foregroundStyle(.teal)
            Text(setup.pairThisMac || route == .existingDevice ? "The ESP32 has confirmed the agent session. You can now choose allowed apps and command permissions." : "Export the saved credentials. Finish pairing on the client's Mac and network.").font(.callout)
            if setup.pairThisMac || route == .existingDevice { Button("View connection") { showOverview() } }
        } else if journey.current == .mac {
            Text("Next: enable this Mac's background service").font(.headline)
            if agent.serviceStatus.contains("Approval required") {
                Text("Open Login Items and enable MacControl under background activity, then return here.")
                Button("Open Login Items") { agent.openLoginSettings() }.buttonStyle(.borderedProminent)
            } else {
                Text(agent.serviceStatus == "Enabled" ? "Registration exists, but the service is not responding. Repair it before continuing." : "Register the service first. If macOS asks, approve MacControl in Login Items.")
                Button(agent.serviceStatus == "Enabled" ? "Repair background service" : "Register this Mac") { agent.register() }.buttonStyle(.borderedProminent)
            }
        } else if journey.current == .connection && endpointMatches {
            Text("Next: start and verify the agent").font(.headline)
            Text("Pairing is saved. Allow Local Network access for MacControl Agent if requested. You can retry the connection without pairing again.")
            Button(agent.state.phase == "halted" ? "Repair connection" : "Start agent") { Task { await agent.operate(agent.state.phase == "halted" ? "repair" : "start") } }.buttonStyle(.borderedProminent)
        } else if route == .savedDevice {
            Text(!selectedRecordIsLoaded ? "Next: load your saved USB setup" : "Next: resume network pairing").font(.headline)
            Text(!selectedRecordIsLoaded ? "Choose the S3 below and load its credentials from Keychain. USB setup is already saved." : !setup.pairThisMac ? "Export the saved credentials below. Verify Wi-Fi and finish pairing on the client's Mac before handoff." : "Use the same network as the S3. Allow Keychain and Local Network prompts, then click Resume pairing below.")
        } else if route == .existingDevice {
            Text("Next: pair the configured controller").font(.headline)
            Text("Open its Controller page, create a pairing code, and enter the address and code below. After pairing, this panel will offer Start agent.")
        } else if journey.needsAttention {
            Text("Next: fix the highlighted step and retry").font(.headline)
            Text(setup.step == .usb ? "Check the COM/UART cable and power. If flashing finished but the console did not respond, press RESET once with BOOT released. Successful uploads leave Flash firmware off for the retry." : "Your saved credentials are retained. Check 2.4 GHz Wi-Fi and allow Local Network access for MacControl and MacControl Agent. Resume from saved setup when an address is available.")
        } else {
            Text("Next: connect the S3 and enter its details").font(.headline)
            Text("Use the COM/UART port and a data cable. Enter 2.4 GHz Wi-Fi and a controller admin password below, then review the setup. Firmware upload, credentials and pairing run in order.")
        }
        if !setup.busy && journey.current != nil {
            Text("Permissions: approve Keychain access to saved credentials and Local Network access for both MacControl and MacControl Agent when macOS asks.").font(.caption).foregroundStyle(.secondary)
        }
    }

    private var manualPairing: some View {
        GroupBox("Configured controller") {
            VStack(alignment: .leading, spacing: 12) {
                if !agent.state.configuration.hostname.isEmpty {
                    Text("Saved endpoint: \(agent.state.configuration.hostname)").textSelection(.enabled)
                    Text("Use Start agent above to reconnect. Enter a new pairing code here only when pairing is required.").font(.caption).foregroundStyle(.secondary)
                } else { Text("Open the pairing window in the Controller, then enter its eight-character code.").foregroundStyle(.secondary) }
                HStack { TextField("Hostname or IP address", text: $agent.endpoint); TextField("Port", text: $agent.port).frame(width: 70) }
                HStack { SecureField("Pairing code", text: $agent.code).frame(width: 180); Button("Pair controller") { agent.pair() }.disabled(!serviceReady || agent.code.isEmpty) }
                HStack { Button("Discover controllers") { discovery.start() }; Text(discovery.message).font(.caption).foregroundStyle(.secondary) }
                ForEach(discovery.hosts, id: \.self) { host in Button(host) { agent.endpoint = host } }
            }.padding(12).frame(maxWidth: .infinity, alignment: .leading)
        }.disabled(setup.busy || agent.busy)
    }
    private func title(for step: SetupStep) -> String {
        switch step {
        case .mac: return "This Mac"
        case .usb: return route == .savedDevice ? "Saved USB setup" : "USB setup"
        case .network: return setup.pairThisMac ? "Verify Wi-Fi" : "Save Wi-Fi"
        case .pairing: return "Pair this Mac"
        case .connection: return "Connected"
        case .handoff: return "Ready to deliver"
        }
    }
    private func statusLabel(_ status: SetupStepStatus) -> String {
        switch status { case .pending: return "Waiting"; case .active: return setup.busy || agent.busy ? "In progress" : "Next"; case .complete: return "Done"; case .attention: return "Needs attention" }
    }
    private func symbol(for status: SetupStepStatus, number: Int) -> String {
        switch status { case .complete: return "checkmark.circle.fill"; case .attention: return "exclamationmark.circle.fill"; case .active: return "\(number).circle.fill"; case .pending: return "\(number).circle" }
    }
    private func color(for status: SetupStepStatus) -> Color {
        switch status { case .complete: return .teal; case .attention: return .orange; case .active: return .accentColor; case .pending: return .secondary }
    }
}
