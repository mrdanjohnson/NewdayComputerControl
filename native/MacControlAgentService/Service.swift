import Foundation
import AppKit
import MacControlCore

actor ServiceController {
    let runtime: AgentRuntime
    private var mutationInProgress = false
    init(_ runtime: AgentRuntime) { self.runtime = runtime }
    func operate(_ operation: String, payload: Data) async throws -> AgentSnapshot {
        guard !mutationInProgress else { throw AgentFailure("Another settings or migration operation is in progress") }
        mutationInProgress = true; defer { mutationInProgress = false }
        switch operation {
        case "configure":
            var c = try JSONDecoder().decode(AgentConfiguration.self, from: payload)
            let old = await runtime.snapshot().configuration
            // The UI may edit only non-identity settings; tokens/migration state
            // remain service-owned. A new hostname requires a new pairing.
            guard c.hostname == old.hostname, c.instanceID == old.instanceID, c.credentialID == old.credentialID, c.port == old.port else { throw AgentFailure("Use Pair to change the endpoint") }
            c.legacyStatePath = old.legacyStatePath; c.legacyBackupPath = old.legacyBackupPath; c.legacyDigest = old.legacyDigest
            c.importedBootID = old.importedBootID; c.importedBootKey = old.importedBootKey
            try await runtime.update(c)
        case "pair":
            guard !(await LegacyAgent.active()) else { throw AgentFailure("Python agent is active. Import it for migration, or stop it before pairing.") }
            let p = try JSONDecoder().decode([String: String].self, from: payload)
            guard let hostname = p["hostname"], let code = p["code"] else { throw AgentFailure("Endpoint and pairing code required") }
            let old = await runtime.snapshot().configuration
            // Pairing against another endpoint gets its own credential slot.
            // Leave existing credentials intact until the new state commits.
            var c = old; c.hostname = hostname; c.port = Int(p["port"] ?? "80") ?? 80; c.enabled = false; c.credentialID = UUID().uuidString
            c.legacyStatePath = nil; c.legacyBackupPath = nil; c.legacyDigest = nil
            c.importedBootID = nil; c.importedBootKey = nil
            try c.validate(); await runtime.stop()
            let (token, id) = try await EndpointTransport.pair(configuration: c, code: code.uppercased())
            c.instanceID = id; try TokenStore.write(token, id: c.credentialID)
            try await runtime.installConfiguration(c)
            await runtime.note("Pairing saved. Start the agent when ready.")
        case "import":
            guard (await runtime.snapshot()).phase == "stopped" else { throw AgentFailure("Stop the native agent before importing Python settings") }
            let path = try JSONDecoder().decode(String.self, from: payload)
            let (c, token) = try Migration.stage(from: path)
            try TokenStore.write(token, id: c.credentialID); try await runtime.installConfiguration(c)
            await runtime.note("Python configuration staged. Original file and service are unchanged.")
        case "start", "repair":
            guard !(await LegacyAgent.active()) else { throw AgentFailure("Python agent is active. Use Take over from Python after importing its settings.") }
            try await runtime.start()
        case "stop": await runtime.stop()
        case "takeover":
            let c = await runtime.snapshot().configuration
            try Migration.validateOriginal(c)
            guard FileManager.default.fileExists(atPath: NativePaths.legacyPlist.path) else { throw AgentFailure("Python LaunchAgent plist is required for automatic cutover and rollback") }
            await runtime.stop()
            do {
                try await LegacyAgent.unload()
                try await runtime.start()
                let until = ProcessInfo.processInfo.systemUptime + 35
                while !(await runtime.stable()) && ProcessInfo.processInfo.systemUptime < until {
                    if (await runtime.snapshot()).phase == "halted" { break }
                    try await Task.sleep(nanoseconds: 200_000_000)
                }
                guard await runtime.stable() else { throw AgentFailure("Native channel did not become stable; rolling back to Python") }
                await runtime.note("Cutover verified by hello, initial burst and two transmitted heartbeats. Python retained for rollback.")
            } catch {
                await runtime.stop()
                do { try await LegacyAgent.restore() } catch { throw AgentFailure("Cutover failed; native agent stopped. Python restoration also failed: \(error.localizedDescription)") }
                throw error
            }
        case "rollback":
            let c = await runtime.snapshot().configuration
            guard c.legacyStatePath != nil else { throw AgentFailure("No imported Python installation") }
            let changed = try Migration.settingsDiffer(c)
            await runtime.stop(); try await LegacyAgent.restore()
            await runtime.note(changed ? "Python restored with its ORIGINAL settings; native changes were not copied into Python." : "Python restored. Original settings and pairing retained.")
        case "forget":
            let c = await runtime.snapshot().configuration
            await runtime.stop(); try await runtime.installConfiguration(AgentConfiguration()); TokenStore.remove(c.credentialID)
            await runtime.note("Local native pairing removed. ESP32 pairing and Python configuration were not revoked or deleted.")
        default: throw AgentFailure("Unknown local operation")
        }
        return await runtime.snapshot()
    }
}
final class IPCExport: NSObject, AgentIPC {
    let controller: ServiceController
    init(_ controller: ServiceController) { self.controller = controller }
    func snapshot(_ reply: @escaping (Data) -> Void) {
        Task { reply((try? JSONEncoder().encode(await controller.runtime.snapshot())) ?? Data()) }
    }
    func perform(_ operation: String, payload: Data, reply: @escaping (Data?, String?) -> Void) {
        guard payload.count <= 1_048_576 else { reply(nil, "Request is too large"); return }
        Task {
            do { reply(try JSONEncoder().encode(await controller.operate(operation, payload: payload)), nil) }
            catch { reply(nil, error.localizedDescription) }
        }
    }
}
final class ListenerDelegate: NSObject, NSXPCListenerDelegate {
    let export: IPCExport
    init(_ export: IPCExport) { self.export = export }
    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        guard connection.effectiveUserIdentifier == getuid() else { return false }
        connection.exportedInterface = NSXPCInterface(with: AgentIPC.self); connection.exportedObject = export
        connection.resume(); return true
    }
}

@main enum ServiceMain {
    @MainActor static func main() {
        let application = NSApplication.shared; application.setActivationPolicy(.prohibited)
        let system = NativeMacSystem()
        if CommandLine.arguments.contains("--probe") {
            Task {
                let s = await system.sample(allowlist: [])
                let summary: [String: JSONValue] = ["protocol_version": .number(2), "hardware_model": .string(s.hardwareModel), "boot_id": .string(EventFactory.bootID(seconds: s.bootTime)), "screen_lock_known": .bool(s.locked != nil), "telemetry": .object(s.heartbeat)]
                print(String(data: try! JSONEncoder().encode(summary), encoding: .utf8)!)
                exit(0)
            }
            application.run(); return
        }
        let config: AgentConfiguration
        var configurationError: String?
        do { config = try NativePaths.load() }
        catch {
            config = AgentConfiguration()
            // Preserve corrupt/future-format data before an explicit repair.
            let source = NativePaths.configuration
            let backup = NativePaths.root.appendingPathComponent("configuration-before-repair-\(UUID().uuidString).json")
            if FileManager.default.fileExists(atPath: source.path) {
                do { try FileManager.default.copyItem(at: source, to: backup); try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: backup.path) }
                catch { fputs("Cannot preserve invalid native configuration. Service not started.\n", stderr); exit(1) }
            }
            configurationError = "Native configuration could not be loaded. Original preserved; import or pair explicitly to repair."
        }
        let runtime = AgentRuntime(configuration: config, system: system)
        let controller = ServiceController(runtime)
        let delegate = ListenerDelegate(IPCExport(controller))
        let listener = NSXPCListener(machServiceName: NativeIdentity.service)
        // Executable lives inside Contents/MacOS. App's signed identity must
        // match on every connection/message, enforced by Foundation/XPC.
        let root = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
        do { listener.setConnectionCodeSigningRequirement(try NativeIdentity.requirement(for: root.appendingPathComponent("MacControl"), identifier: NativeIdentity.app)) }
        catch { fputs("Cannot validate frontend signature; agent not started.\n", stderr); exit(1) }
        listener.delegate = delegate; listener.resume()
        let power = PowerObserver()
        power.changed = { sleeping in
            let drained = DispatchSemaphore(value: 0)
            Task.detached { await runtime.powerChanged(sleeping: sleeping); drained.signal() }
            // Bound the pre-sleep handoff before IOAllowPowerChange; never
            // stall the OS indefinitely if networking is already unavailable.
            if sleeping { _ = drained.wait(timeout: .now() + 2.5) }
        }
        if !power.start() { Task { await runtime.note("IOKit sleep/wake observer unavailable.") } }
        signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN)
        let historyURL = NativePaths.root.appendingPathComponent("lifecycle.json")
        var history = (try? JSONDecoder().decode(CrashHistory.self, from: Data(contentsOf: historyURL))) ?? CrashHistory()
        let cooldown = history.begin(at: Date()); try? NativePaths.save(history, to: historyURL)
        let termination = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        termination.setEventHandler { Task { await runtime.stop(persist: false); power.stop(); var clean = CrashHistory(); clean.cleanStop(); try? NativePaths.save(clean, to: historyURL); exit(0) } }; termination.resume()
        Task {
            if cooldown > 0 { await runtime.setCooldown(cooldown); try? await Task.sleep(nanoseconds: UInt64(cooldown) * 1_000_000_000) }
            if let configurationError { await runtime.note(configurationError) }
            else if await LegacyAgent.active() { await runtime.note("Python agent detected. Native session remains inactive until explicit cutover.") }
            else { await runtime.resumeOnLogin() }
        }
        withExtendedLifetime((delegate, listener, power, termination)) { application.run() }
    }
}
