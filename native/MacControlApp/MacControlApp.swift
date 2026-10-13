import SwiftUI
import MacControlCore

struct MacControlApp: App {
    @NSApplicationDelegateAdaptor(SetupAppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()
    init() {
        #if DEBUG
        if CommandLine.arguments.contains("--ipc-smoke") {
            Task { @MainActor in
                do {
                    let model = AppModel()
                    try await model.ipcSmoke()
                    exit(0)
                } catch { fputs("IPC smoke failed: \(error.localizedDescription)\n", stderr); exit(1) }
            }
        }
        #endif
    }
    var body: some Scene {
        WindowGroup("MacControl", id: "main") { MainView().environmentObject(model).frame(minWidth: 820, minHeight: 580) }
        MenuBarExtra("MacControl", systemImage: model.state.phase == "connected" ? "link.circle.fill" : "link.circle") { AgentMenu().environmentObject(model) }
    }
}
struct AgentMenu: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) var openWindow
    @Environment(\.openURL) var openURL
    var body: some View {
        Text("MacControl · \(model.state.phase.capitalized)")
        if !model.state.configuration.hostname.isEmpty { Text(model.state.configuration.hostname) }
        Button("Open MacControl") { openWindow(id: "main"); NSApp.activate(ignoringOtherApps: true) }
        Button("Open Controller") {
            if let url = model.state.configuration.endpoint { openURL(url) }
        }.disabled(model.state.configuration.hostname.isEmpty || model.state.configuration.endpoint == nil)
        Divider()
        Button("Start agent") { Task { await model.operate("start") } }.disabled(model.provisioningActive || model.busy || model.state.phase == "connected" || model.serviceStatus != "Enabled")
        Button("Stop agent") { Task { await model.operate("stop") } }.disabled(model.provisioningActive || model.busy || model.serviceStatus != "Enabled")
        Divider()
        Button("Quit frontend") { NSApp.terminate(nil) }.disabled(model.provisioningActive)
    }
}
enum Page: String, CaseIterable, Identifiable {
    case overview = "Overview", setup = "Setup & pairing", apps = "Allowed applications", commands = "Command permissions", connection = "Connection", logs = "Diagnostics", background = "Background operation"
    var id: String { rawValue }
    var symbol: String { switch self { case .overview: return "rectangle.3.group"; case .setup: return "link"; case .apps: return "app.badge.checkmark"; case .commands: return "checkmark.shield"; case .connection: return "network"; case .logs: return "list.bullet.rectangle"; case .background: return "gearshape.2" } }
    var subtitle: String { switch self { case .overview: return "This Mac supplies evidence. Your ESP32 verifies commands."; case .setup: return "Connect this Mac to its ESP32 or import your Python agent."; case .apps: return "Choose which applications this Mac may report, launch and quit."; case .commands: return "Allow only the actions this Mac should accept."; case .connection: return "Configure the paired channel and recovery behavior."; case .logs: return "Local connection events and action reports."; case .background: return "Keep the agent running independently of this window." } }
}
struct MainView: View {
    @EnvironmentObject var model: AppModel
    @StateObject private var discovery = Discovery()
    @State private var page: Page? = .overview
    @State private var bundleID = ""
    @State private var confirmation: String?
    var body: some View {
        NavigationSplitView {
            VStack(alignment: .leading, spacing: 20) {
                HStack { Image(systemName: "link.circle.fill").font(.title).foregroundStyle(.teal); VStack(alignment: .leading) { Text("MacControl").font(.headline); Text("Native Mac agent").font(.caption).foregroundStyle(.secondary) } }.padding([.top, .horizontal])
                List(Page.allCases, selection: $page) { p in Label(p.rawValue, systemImage: p.symbol).tag(p) }
                VStack(alignment: .leading, spacing: 5) { Label(model.state.phase.capitalized, systemImage: "circle.fill").foregroundStyle(model.state.phase == "connected" ? .green : .secondary); Text("Protocol v2 · \(EventFactory.version)").font(.caption).foregroundStyle(.secondary) }.padding()
            }.navigationSplitViewColumnWidth(230).disabled(model.provisioningActive)
        } detail: {
            VStack(alignment: .leading, spacing: 0) {
                VStack(alignment: .leading, spacing: 7) { Text((page ?? .overview).rawValue).font(.largeTitle.weight(.semibold)); Text((page ?? .overview).subtitle).foregroundStyle(.secondary) }.padding(28)
                if let error = model.error { banner(error, symbol: "exclamationmark.triangle.fill", color: .orange) }
                if let notice = model.notice { banner(notice, symbol: "checkmark.circle", color: .teal) }
                if model.busy { ProgressView().progressViewStyle(.linear).padding(.horizontal, 28) }
                ScrollView { content.padding(28).frame(maxWidth: .infinity, alignment: .leading) }
            }
        }
        .alert("Confirm \(confirmation ?? "operation")", isPresented: Binding(get: { confirmation != nil }, set: { if !$0 { confirmation = nil } })) {
            Button("Cancel", role: .cancel) { confirmation = nil }
            Button("Continue") { let operation = confirmation == "cutover" ? "takeover" : confirmation == "rollback" ? "rollback" : "forget"; confirmation = nil; Task { await model.operate(operation) } }
        } message: {
            Text(confirmation == "cutover" ? "Stop the Python service and verify the native channel. If verification fails, Python will be restored. Its configuration will be kept." : confirmation == "rollback" ? "Stop the native agent and restore Python using its original settings. Changes made in native settings will not be copied into Python." : "Remove native credentials locally. The ESP32 pairing and original Python files will remain intact.")
        }
    }
    private func banner(_ text: String, symbol: String, color: Color) -> some View {
        HStack(alignment: .top) { Image(systemName: symbol).foregroundStyle(color); Text(text).font(.callout).textSelection(.enabled); Spacer(); Button { model.error = nil; model.notice = nil } label: { Image(systemName: "xmark") }.buttonStyle(.plain) }.padding(14).background(color.opacity(0.09)).clipShape(RoundedRectangle(cornerRadius: 10)).padding(.horizontal, 28).padding(.bottom, 10)
    }
    @ViewBuilder private var content: some View {
        switch page ?? .overview {
        case .overview: overview
        case .setup: setup
        case .apps: apps.disabled(model.provisioningActive)
        case .commands: commands.disabled(model.provisioningActive)
        case .connection: connection.disabled(model.provisioningActive)
        case .logs: logs
        case .background: background.disabled(model.provisioningActive)
        }
    }
    private var overview: some View {
        VStack(alignment: .leading, spacing: 20) {
            GroupBox {
                VStack(alignment: .leading, spacing: 12) {
                    Label(model.state.phase.capitalized, systemImage: model.state.phase == "connected" ? "checkmark.circle.fill" : "circle.dashed").font(.title2).foregroundStyle(.teal)
                    Text(model.state.detail).foregroundStyle(.secondary)
                    Divider()
                    row("ESP32", model.state.configuration.hostname.isEmpty ? "Not paired" : model.state.configuration.hostname)
                    row("Transport", model.state.configuration.transport.capitalized)
                    row("Last heartbeat sent", model.state.lastHeartbeat?.formatted(date: .omitted, time: .standard) ?? "None yet")
                    row("Screen lock", model.state.screenLocked.map { $0 ? "Locked" : "Unlocked" } ?? "Unknown")
                    row("Latest action", model.state.lastAction ?? "None received")
                }.padding(12).frame(maxWidth: .infinity, alignment: .leading)
            }.disabled(model.provisioningActive)
            HStack {
                Button("Start agent") { Task { await model.operate("start") } }.buttonStyle(.borderedProminent).disabled(model.provisioningActive || model.busy || model.serviceStatus != "Enabled")
                Button("Stop agent") { Task { await model.operate("stop") } }.disabled(model.provisioningActive || model.busy || model.serviceStatus != "Enabled")
                Button("Set up this Mac") { page = .setup }
            }
            Text("Wake, lock and unlock remain on the ESP32's USB HID path. This agent performs only the actions you enable and reports observed Mac state.").font(.callout).foregroundStyle(.secondary)
        }
    }
    private func row(_ label: String, _ value: String) -> some View { HStack(alignment: .top) { Text(label).foregroundStyle(.secondary).frame(width: 145, alignment: .leading); Text(value).textSelection(.enabled); Spacer() } }
    private var setup: some View {
        SetupPairingView(agent: model, setup: model.deviceSetup, discovery: discovery,
                         showOverview: { page = .overview }, confirmMigration: { confirmation = $0 })
    }
    private var apps: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack { Button("Choose application…") { model.addApp() }; Spacer(); saveButton }
            HStack { TextField("Bundle ID, for example com.apple.Terminal", text: $bundleID); Button("Add") { model.addBundle(bundleID); bundleID = "" } }
            if model.draft.allowlist.isEmpty { Label("No applications are allowed yet.", systemImage: "app.dashed").foregroundStyle(.secondary).padding(.vertical) }
            ForEach(model.draft.allowlist, id: \.self) { bid in
                HStack(spacing: 12) {
                    if let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bid) { Image(nsImage: NSWorkspace.shared.icon(forFile: url.path)).resizable().frame(width: 32, height: 32) }
                    else { Image(systemName: "app").font(.title) }
                    VStack(alignment: .leading, spacing: 4) { Text(NSWorkspace.shared.urlForApplication(withBundleIdentifier: bid).map { $0.deletingPathExtension().lastPathComponent } ?? bid).font(.headline); Text(bid).font(.caption).foregroundStyle(.secondary) }
                    Spacer(); Text(model.state.runningApps[bid] != nil ? "Running" : "Not observed running").font(.caption).foregroundStyle(.secondary)
                    Button { model.draft.allowlist.removeAll { $0 == bid }; model.dirty = true } label: { Image(systemName: "minus.circle") }.buttonStyle(.plain)
                }.padding(12).background(.quaternary.opacity(0.3)).clipShape(RoundedRectangle(cornerRadius: 10))
            }
            Text("Apps are matched by bundle ID. Quit requests are graceful; an unsaved-work dialog can prevent completion.").font(.callout).foregroundStyle(.secondary)
        }
    }
    private var commands: some View {
        VStack(alignment: .leading, spacing: 18) {
            ForEach(AgentAction.allCases) { action in Toggle(action.title, isOn: Binding(get: { model.draft.enabledCommands[action.rawValue] == true }, set: { model.draft.enabledCommands[action.rawValue] = $0; model.dirty = true })).toggleStyle(.switch) }
            Divider(); Text("All actions are disabled on a new installation. Sleep, restart and shutdown affect this Mac when requested through the paired ESP32.").foregroundStyle(.secondary)
            saveButton
        }
    }
    private var connection: some View {
        VStack(alignment: .leading, spacing: 18) {
            row("Paired endpoint", model.state.configuration.hostname.isEmpty ? "None" : model.state.configuration.hostname)
            Picker("Transport", selection: Binding(get: { model.draft.transport }, set: { model.draft.transport = $0; model.dirty = true })) { Text("WebSocket").tag("websocket"); Text("HTTP polling").tag("polling") }.pickerStyle(.segmented)
            Stepper("Polling interval: \(model.draft.pollInterval) seconds", value: Binding(get: { model.draft.pollInterval }, set: { model.draft.pollInterval = $0; model.dirty = true }), in: 2...30)
            Text("Changing transport starts a new session. Reconnect uses the existing backoff policy; revoked credentials and incompatible protocols remain halted until repaired.").foregroundStyle(.secondary)
            HStack { saveButton; Button("Repair connection") { Task { await model.operate("repair") } }.disabled(model.busy); Button("Remove native pairing…") { confirmation = "forget" }.disabled(model.busy) }
            Text("To change the endpoint address, use Setup & pairing. An endpoint rename may require re-pairing through its Web UI.").font(.caption).foregroundStyle(.secondary)
        }
    }
    private var logs: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack { Text("Last \(model.state.logs.count) events").foregroundStyle(.secondary); Spacer(); Button("Export diagnostics…") { model.exportLog() } }
            ForEach(model.state.logs.reversed()) { entry in VStack(alignment: .leading, spacing: 4) { Text(entry.at.formatted()).font(.caption).foregroundStyle(.secondary); Text(entry.message).font(.system(.callout, design: .monospaced)).textSelection(.enabled) }.frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 6); Divider() }
        }
    }
    private var background: some View {
        VStack(alignment: .leading, spacing: 18) {
            row("Service", model.serviceStatus)
            Text("The per-user agent starts after login and is supervised independently of this frontend. Closing or quitting this window does not stop an active agent. It does not run before login or while this Mac is powered off.").foregroundStyle(.secondary)
            HStack { Button("Register service") { model.register() }; Button("Open Login Items") { model.openLoginSettings() }; Button("Stop & unregister") { model.unregister() }.disabled(model.busy) }
            Text("Legacy Macs can continue using Python. Each Mac uses its own ESP32; the native app does not require a fleet-wide upgrade.").font(.callout)
        }
    }
    private var saveButton: some View { Button(model.dirty ? "Save changes" : "Saved") { model.save() }.buttonStyle(.borderedProminent).disabled(!model.dirty || model.busy || model.serviceStatus != "Enabled") }
}
