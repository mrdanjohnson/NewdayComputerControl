import SwiftUI
import ServiceManagement
import Network
import MacControlCore

@MainActor final class AppModel: ObservableObject {
    @Published var state = AgentSnapshot()
    @Published var draft = AgentConfiguration()
    @Published var error: String?
    @Published var notice: String?
    @Published var busy = false
    @Published var dirty = false
    @Published var serviceStatus = "Not registered"
    @Published var endpoint = ""
    @Published var port = "80"
    @Published var code = ""
    private var connection: NSXPCConnection?
    private let service = SMAppService.agent(plistName: "dev.maccontrol.native.agent.plist")
    private var refreshTask: Task<Void, Never>?
    init() {
        refreshTask = Task { [weak self] in
            while !Task.isCancelled { await self?.refresh(); try? await Task.sleep(nanoseconds: 2_000_000_000) }
        }
    }
    #if DEBUG
    func ipcSmoke() async throws {
        let initial = try await fetch()
        guard initial.phase == "stopped", !initial.paired else { throw AgentFailure("Smoke state must be unpaired and stopped") }
        var fixture = initial.configuration; fixture.allowlist = ["com.example.Fixture"]
        await operate("configure", payload: fixture)
        guard error == nil, state.configuration.allowlist == fixture.allowlist else { throw AgentFailure("Configuration XPC failed") }
        var tampered = fixture; tampered.instanceID = "ag-tampered"
        await operate("configure", payload: tampered)
        guard error == "Use Pair to change the endpoint" else { throw AgentFailure("Identity mutation was not refused") }
        print("IPC_SMOKE stopped paired=false configuration=verified identity_tampering=refused")
    }
    #endif
    func refresh() async {
        switch service.status {
        case .enabled: serviceStatus = "Enabled"
        case .requiresApproval: serviceStatus = "Approval required in System Settings"
        case .notRegistered: serviceStatus = "Not registered"
        case .notFound: serviceStatus = "Background service configuration not found"
        @unknown default: serviceStatus = "Unknown"
        }
        guard service.status == .enabled else {
            connection?.invalidate(); connection = nil
            state.phase = "inactive"; state.sessionID = nil
            state.detail = "Background service: \(serviceStatus). Use Setup & pairing to register or approve it."
            return
        }
        do { setState(try await fetch()) } catch {
            connection?.invalidate(); connection = nil
            state.phase = "unavailable"; state.detail = "Background service unavailable. Check Login Items approval or register the service again."
        }
    }
    private func proxy() throws -> AgentIPC {
        if connection == nil {
            let c = NSXPCConnection(machServiceName: NativeIdentity.service)
            c.remoteObjectInterface = NSXPCInterface(with: AgentIPC.self)
            let helper = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/MacControlAgentService")
            c.setCodeSigningRequirement(try NativeIdentity.requirement(for: helper, identifier: NativeIdentity.service))
            c.invalidationHandler = { [weak self] in Task { @MainActor in self?.connection = nil } }
            c.interruptionHandler = { [weak self] in Task { @MainActor in self?.connection = nil } }
            c.resume(); connection = c
        }
        guard let p = connection?.remoteObjectProxy as? AgentIPC else { throw AgentFailure("Background service unavailable") }
        return p
    }
    func fetch() async throws -> AgentSnapshot {
        let p = try proxy()
        return try await withCheckedThrowingContinuation { reply in
            let once = IPCReply(reply)
            p.snapshot { bytes in do { once.finish(.success(try JSONDecoder().decode(AgentSnapshot.self, from: bytes))) } catch { once.finish(.failure(error)) } }
            Task { try? await Task.sleep(nanoseconds: 4_000_000_000); once.finish(.failure(AgentFailure("Background service did not respond"))) }
        }
    }
    private func setState(_ value: AgentSnapshot) {
        state = value
        if !dirty { draft = value.configuration }
        if endpoint.isEmpty { endpoint = value.configuration.hostname; port = String(value.configuration.port) }
    }
    func register() {
        busy = true; error = nil
        do {
            try service.register(); notice = "Background service registered. Approve it in System Settings if requested."
        } catch { self.error = error.localizedDescription }
        busy = false; Task { await refresh() }
    }
    func unregister() {
        Task {
            await operate("stop")
            guard error == nil else { return }
            do { try await service.unregister(); connection?.invalidate(); connection = nil; serviceStatus = "Not registered"; notice = "Native background service removed. Python and ESP32 pairing were not changed." }
            catch { self.error = error.localizedDescription }
        }
    }
    func openLoginSettings() { SMAppService.openSystemSettingsLoginItems() }
    func operate<T: Encodable>(_ operation: String, payload: T) async {
        guard !busy else { return }
        busy = true; error = nil; notice = nil
        defer { busy = false }
        do {
            let p = try proxy(), data = try JSONEncoder().encode(payload)
            let result: AgentSnapshot = try await withCheckedThrowingContinuation { reply in
                let once = IPCReply(reply)
                p.perform(operation, payload: data) { bytes, message in
                    if let message { once.finish(.failure(AgentFailure(message))); return }
                    do { guard let bytes else { throw AgentFailure("Empty service response") }; once.finish(.success(try JSONDecoder().decode(AgentSnapshot.self, from: bytes))) }
                    catch { once.finish(.failure(error)) }
                }
                Task { try? await Task.sleep(nanoseconds: 45_000_000_000); once.finish(.failure(AgentFailure("Operation timed out. Check status before retrying migration."))) }
            }
            dirty = false; setState(result)
            notice = result.logs.last?.message ?? "Settings saved."
            if operation == "pair" { code = "" }
        } catch { self.error = error.localizedDescription }
    }
    func operate(_ operation: String) async { await operate(operation, payload: [String: String]()) }
    func save() { Task { await operate("configure", payload: draft) } }
    func pair() { Task { await operate("pair", payload: ["hostname": endpoint.trimmingCharacters(in: .whitespacesAndNewlines), "port": port, "code": code]) } }
    func importPython() {
        let picker = NSOpenPanel(); picker.allowedContentTypes = [.json]; picker.canChooseDirectories = false
        picker.directoryURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".maccontrol")
        picker.message = "Choose the existing Python agent.json. It will be backed up and imported without changing the original."
        if picker.runModal() == .OK, let url = picker.url { Task { await operate("import", payload: url.path) } }
    }
    func addApp() {
        let picker = NSOpenPanel(); picker.allowedContentTypes = [.applicationBundle]; picker.canChooseDirectories = false
        picker.directoryURL = URL(fileURLWithPath: "/Applications")
        if picker.runModal() == .OK, let url = picker.url, let bid = Bundle(url: url)?.bundleIdentifier { addBundle(bid) }
    }
    func addBundle(_ bid: String) {
        guard Dispatch.validBundleID(bid), !draft.allowlist.contains(bid), draft.allowlist.count < 24 else { error = "Enter a unique valid bundle ID (up to 24 applications)."; return }
        draft.allowlist.append(bid); draft.allowlist.sort(); dirty = true
    }
    func exportLog() {
        let picker = NSSavePanel(); picker.nameFieldStringValue = "MacControl-diagnostics.json"; picker.allowedContentTypes = [.json]
        if picker.runModal() == .OK, let url = picker.url {
            do {
                // Export status and logs, not settings, usernames, tokens or legacy backup paths.
                let summary: [String: JSONValue] = ["agent_version": .string(EventFactory.version), "phase": .string(state.phase), "detail": .string(state.detail), "transport": .string(state.configuration.transport), "screen_lock_known": .bool(state.screenLocked != nil), "logs": .array(state.logs.map { .object(["at": .string(ISO8601DateFormatter().string(from: $0.at)), "message": .string($0.message)]) })]
                try JSONEncoder().encode(summary).write(to: url, options: .atomic)
            } catch { self.error = error.localizedDescription }
        }
    }
}
final class IPCReply<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var reply: CheckedContinuation<T, Error>?
    init(_ reply: CheckedContinuation<T, Error>) { self.reply = reply }
    func finish(_ result: Result<T, Error>) { lock.lock(); let c = reply; reply = nil; lock.unlock(); c?.resume(with: result) }
}

@MainActor final class Discovery: NSObject, ObservableObject, NetServiceDelegate {
    @Published var hosts: [String] = []
    @Published var message = "Find MacControl endpoints on this network."
    private var browser: NWBrowser?
    private var services: [NetService] = []
    func start() {
        stop(); hosts = []; message = "Searching the local network…"
        let b = NWBrowser(for: .bonjour(type: "_maccontrol._tcp", domain: nil), using: .tcp)
        b.stateUpdateHandler = { [weak self] state in Task { @MainActor in
            if case .failed = state { self?.message = "Discovery unavailable. Check Local Network permission or enter an address." }
            if case .waiting = state { self?.message = "Waiting for network access. You can also enter an address." }
        } }
        b.browseResultsChangedHandler = { [weak self] results, _ in Task { @MainActor in
            guard let self else { return }
            for r in results {
                if case let .service(name, type, domain, _) = r.endpoint, self.services.count < 64, !self.services.contains(where: { $0.name == name && $0.domain == domain }) {
                    let s = NetService(domain: domain, type: type, name: name); s.delegate = self; self.services.append(s); s.resolve(withTimeout: 5)
                }
            }
        } }
        browser = b; b.start(queue: .main)
    }
    func stop() { browser?.cancel(); browser = nil; services.forEach { $0.stop() }; services.removeAll() }
    nonisolated func netServiceDidResolveAddress(_ sender: NetService) {
        let resolved = sender.hostName
        Task { @MainActor in
        if let host = resolved { let normalized = host.hasSuffix(".") ? String(host.dropLast()) : host; if !hosts.contains(normalized) { hosts.append(normalized); hosts.sort() }; message = "Select an endpoint or enter its address." }
        }
    }
}
