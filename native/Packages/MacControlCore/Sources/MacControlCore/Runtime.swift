import Foundation

public actor AgentRuntime {
    public typealias TransportFactory = @Sendable (AgentConfiguration, String) -> any AgentTransport
    private var state = AgentSnapshot()
    private var factory: EventFactory
    private let system: any MacExecutor
    private let transportFactory: TransportFactory
    private let root: URL
    private let credential: @Sendable (String) throws -> String?
    private var token = ""
    private var transport: (any AgentTransport)?
    private var runTask: Task<Void, Never>?
    private var sender: Task<Void, Never>?
    private var receiver: Task<Void, Never>?
    private var actionTask: Task<Void, Never>?
    private var queue: [(EventType, [String: JSONValue], String?)] = []
    private var actions: [(Dispatch, Int)] = []
    private var seen: [String] = []
    private var epoch = 0
    private var active = false
    private var sendFailure: Error?
    private var receiveFailure: Error?
    private var sample = SystemSample()
    private var frontSeen = false
    private var started = ProcessInfo.processInfo.systemUptime
    private var offlineUntil: Double = 0
    private var quitMarks: Set<String> = []
    private var bootID = ""
    private var beatCount = 0
    private var heartbeatInterval: Double = 5
    private var cooldownUntil: Double = 0
    public init(configuration: AgentConfiguration, system: any MacExecutor, root: URL = NativePaths.root,
                credential: @escaping @Sendable (String) throws -> String? = { try TokenStore.read($0) },
                transportFactory: @escaping TransportFactory = { EndpointTransport(configuration: $0, token: $1) }) {
        state.configuration = configuration; self.system = system; self.root = root; self.credential = credential; self.transportFactory = transportFactory
        factory = EventFactory(instanceID: configuration.instanceID)
    }
    public func snapshot() -> AgentSnapshot { state }
    public func setCooldown(_ seconds: Double) { cooldownUntil = ProcessInfo.processInfo.systemUptime + seconds; state.phase = "cooldown"; state.detail = "Repeated service crashes; recovery waits 60 seconds." }
    public func stable() -> Bool { active && beatCount >= 2 && sendFailure == nil && receiveFailure == nil }
    public func note(_ message: String) {
        let safe = token.isEmpty ? message : message.replacingOccurrences(of: token, with: "[redacted]")
        state.logs.append(LogEntry(safe)); if state.logs.count > 500 { state.logs.removeFirst(state.logs.count - 500) }
        // A bounded durable log, with no credentials or request payloads.
        try? NativePaths.save(state.logs, to: root.appendingPathComponent("log.json"))
    }
    public func update(_ c: AgentConfiguration) async throws {
        try c.validate()
        let wasEnabled = state.configuration.enabled
        await stop(persist: false)
        state.configuration = c; state.configuration.enabled = wasEnabled
        factory = EventFactory(instanceID: c.instanceID)
        try save()
        if wasEnabled { try await start() }
    }
    public func installConfiguration(_ c: AgentConfiguration) async throws {
        await stop(persist: false); try c.validate(); state.configuration = c; factory = EventFactory(instanceID: c.instanceID)
        state.paired = (try credential(c.credentialID)) != nil; try save()
    }
    private func save() throws { try NativePaths.save(state.configuration, to: root.appendingPathComponent("configuration.json")) }
    public func resumeOnLogin() async {
        if let bytes = try? Data(contentsOf: root.appendingPathComponent("halt.json")), let reason = try? JSONDecoder().decode(String.self, from: bytes) {
            state.phase = "halted"; state.detail = reason; note("HALTED retained across service restart; use Repair connection."); return
        }
        guard state.configuration.enabled else { return }
        do { try await start() } catch { state.phase = "blocked"; state.detail = error.localizedDescription; note(state.detail) }
    }
    public func start() async throws {
        guard ProcessInfo.processInfo.systemUptime >= cooldownUntil else { throw AgentFailure("Service crash cooldown is still active") }
        guard runTask == nil else { return }
        try state.configuration.validate()
        guard !state.configuration.hostname.isEmpty, let credential = try credential(state.configuration.credentialID), !credential.isEmpty else { throw AgentFailure("Pair or import this Mac before starting the agent") }
        // The service's start handler also checks the legacy service/process.
        token = credential; state.paired = true; state.configuration.enabled = true; try save()
        try? FileManager.default.removeItem(at: root.appendingPathComponent("halt.json"))
        state.phase = "connecting"; state.detail = "Opening the paired ESP32 channel."
        runTask = Task { await self.run() }
    }
    public func stop(persist: Bool = true) async {
        state.configuration.enabled = false; if persist { try? save() }
        if active { enqueue(.goodbye, ["reason": .string("agent_stop")]); _ = await flush(timeout: 2) }
        runTask?.cancel(); receiver?.cancel(); sender?.cancel(); actionTask?.cancel()
        if let transport { await transport.close() }
        let oldRun = runTask, oldSender = sender, oldReceiver = receiver, oldAction = actionTask
        epoch += 1; active = false; queue.removeAll(); actions.removeAll(); factory.end()
        await oldRun?.value; await oldSender?.value; await oldReceiver?.value; await oldAction?.value
        runTask = nil; sender = nil; receiver = nil; actionTask = nil; transport = nil
        state.phase = "stopped"; state.sessionID = nil; state.detail = "Agent stopped; the frontend can stay open."
    }
    public func powerChanged(sleeping: Bool) async {
        if sleeping {
            offlineUntil = ProcessInfo.processInfo.systemUptime + 60
            enqueue(.system, ["state": .string("sleeping")]); enqueue(.goodbye, ["reason": .string("sleep")])
            _ = await flush(timeout: 2)
        } else {
            offlineUntil = 0
            enqueue(.system, ["state": .string("waking")])
            let generation = epoch
            Task { try? await Task.sleep(nanoseconds: 5_000_000_000); self.emitAwake(generation) }
        }
    }
    private func emitAwake(_ generation: Int) { if generation == epoch { enqueue(.system, ["state": .string("awake")]) } }
    private func run() async {
        var attempt = 0
        while !Task.isCancelled && state.configuration.enabled {
            while offlineUntil > ProcessInfo.processInfo.systemUptime && !Task.isCancelled { try? await Task.sleep(nanoseconds: 200_000_000) }
            if Task.isCancelled { break }
            epoch += 1; let generation = epoch; factory.end(); active = false
            queue.removeAll(); sendFailure = nil; receiveFailure = nil; beatCount = 0
            let client = transportFactory(state.configuration, token); transport = client
            do {
                state.phase = "connecting"; state.detail = "Connecting to \(state.configuration.hostname) using \(state.configuration.transport)."
                sample = await system.sample(allowlist: state.configuration.allowlist)
                guard sample.bootTime > 0 else { throw AgentFailure("Cannot observe Mac boot time; refusing to fabricate reboot evidence", halted: true) }
                if sample.bootTime > 0 {
                    bootID = state.configuration.importedBootKey == sample.bootTime ? (state.configuration.importedBootID ?? EventFactory.bootID(seconds: sample.bootTime)) : EventFactory.bootID(seconds: sample.bootTime)
                } else if bootID.isEmpty { bootID = state.configuration.importedBootID ?? "b_" + UUID().uuidString.prefix(6).uppercased() }
                let hello = try factory.event(.hello, payload: ["protocol_version": .number(2), "agent_version": .string(EventFactory.version), "boot_id": .string(bootID), "hostname": .string(state.configuration.hostname)])
                let ack = try await client.open(hello: hello.encode())
                try Task.checkCancellation(); if generation != epoch { break }
                let sid = try EventFactory.helloAck(ack, websocket: state.configuration.transport == "websocket")
                factory.begin(sid); state.sessionID = sid; active = true; attempt = 0
                heartbeatInterval = min(30, max(2, ack["heartbeat_interval_s"]?.number ?? 5))
                state.phase = "connected"; state.detail = "Paired channel active. Local reports are evidence; the ESP32 decides command outcomes."
                note("Session established (protocol 2, \(state.configuration.transport)).")
                initialBurst()
                guard await flush(timeout: 2) else { throw AgentFailure("Initial status burst could not be sent within two seconds") }
                receiver = Task { await self.receiveLoop(client, generation: generation) }
                var nextBeat = ProcessInfo.processInfo.systemUptime + heartbeatInterval
                var nextSample = ProcessInfo.processInfo.systemUptime
                while !Task.isCancelled && generation == epoch {
                    if let sendFailure { throw sendFailure }; if let receiveFailure { throw receiveFailure }
                    let now = ProcessInfo.processInfo.systemUptime
                    if now >= nextSample { await sampleDeltas(); nextSample = now + 1 }
                    if now >= nextBeat {
                        var payload = sample.heartbeat
                        payload["boot_id"] = .string(bootID); payload["uptime_s"] = .number(floor(now - started))
                        enqueue(.heartbeat, payload); nextBeat = now + heartbeatInterval
                    }
                    try await Task.sleep(nanoseconds: 100_000_000)
                }
            } catch {
                if !Task.isCancelled && generation == epoch {
                    let reason: String
                    if let e = error as? AgentFailure { reason = e.message }
                    else if let e = error as? URLError { reason = "Network failure: \(e.code.rawValue) (check DNS, connection and Local Network permission)" }
                    else { reason = "Transport or protocol failure: \(String(describing: type(of: error)))" }
                    state.detail = reason; note(reason)
                    if (error as? AgentFailure)?.halted == true {
                        state.phase = "halted"; try? NativePaths.save(reason, to: root.appendingPathComponent("halt.json"))
                        await teardown(client); runTask = nil; return
                    }
                }
            }
            await teardown(client)
            if Task.isCancelled || !state.configuration.enabled { break }
            attempt += 1; let delay = EventFactory.backoff(attempt)
            state.phase = "backoff"; note("Reconnect attempt \(attempt) in \(String(format: "%.1f", delay)) seconds.")
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
        }
    }
    private func teardown(_ client: any AgentTransport) async {
        active = false; epoch += 1
        receiver?.cancel(); sender?.cancel(); actionTask?.cancel(); await client.close()
        await receiver?.value; await sender?.value; await actionTask?.value
        receiver = nil; sender = nil; actionTask = nil; queue.removeAll(); actions.removeAll(); state.sessionID = nil; factory.end()
    }
    private func enqueue(_ type: EventType, _ payload: [String: JSONValue], commandID: String? = nil) {
        guard active else { return }
        if queue.count >= 128 {
            if let index = queue.firstIndex(where: { $0.0 == .heartbeat || $0.0 == .front }) { queue.remove(at: index) }
            else { sendFailure = AgentFailure("Event queue full; reconnecting without fabricating evidence"); return }
        }
        queue.append((type, payload, commandID))
        if sender == nil { let generation = epoch; sender = Task { await self.sendLoop(generation) } }
    }
    private func sendLoop(_ generation: Int) async {
        while generation == epoch && active && !Task.isCancelled && !queue.isEmpty {
            let item = queue.removeFirst()
            do {
                guard let transport, let sid = factory.sessionID else { break }
                let event = try factory.event(item.0, payload: item.1, commandID: item.2)
                try await transport.send(event.encode(), sessionID: sid)
                if item.0 == .heartbeat { state.lastHeartbeat = Date(); beatCount += 1 }
            } catch { if generation == epoch { sendFailure = error }; break }
        }
        if generation == epoch { sender = nil }
    }
    private func flush(timeout: Double) async -> Bool {
        let until = ProcessInfo.processInfo.systemUptime + timeout, generation = epoch
        while generation == epoch && sendFailure == nil && (sender != nil || !queue.isEmpty) && ProcessInfo.processInfo.systemUptime < until {
            if Task.isCancelled { return false }; try? await Task.sleep(nanoseconds: 10_000_000)
        }
        return generation == epoch && sendFailure == nil && queue.isEmpty && sender == nil
    }
    private func receiveLoop(_ client: any AgentTransport, generation: Int) async {
        do { while generation == epoch && !Task.isCancelled { let object = try await client.receive(); if generation == epoch { received(object, generation: generation) } } }
        catch { if generation == epoch && !Task.isCancelled { receiveFailure = error } }
    }
    private func received(_ object: [String: JSONValue], generation: Int) {
        guard let id = object["command_id"]?.string, !id.isEmpty, id.count <= 128 else { note("Dropped uncorrelatable dispatch."); return }
        // The firmware's ack action enum is closed. An unknown action gets a
        // correlated failure, without emitting an ack the firmware would reject.
        if let name = object["action"]?.string, AgentAction(rawValue: name) != nil {
            enqueue(.ack, ["command_id": .string(id), "action": .string(name)], commandID: id)
        }
        let dispatch: Dispatch
        do { dispatch = try Dispatch(object) } catch { result(id, error: "app_not_allowlisted"); note("Rejected malformed dispatch."); return }
        if let reason = dispatch.rejection(configuration: state.configuration) { result(id, error: reason); return }
        if seen.contains(id) { note("Ignored repeated dispatch for a previously received command."); return }
        guard actions.count < 16 else { result(id, error: "action_timeout"); return }
        seen.append(id); if seen.count > 256 { seen.removeFirst() }
        actions.append((dispatch, generation))
        if actionTask == nil { actionTask = Task { await self.executeActions(generation) } }
    }
    private func result(_ id: String, error: String?) {
        enqueue(.result, ["command_id": .string(id), "outcome": .string(error == nil ? "ok" : "failed"), "error_code": .optional(error)], commandID: id)
    }
    private func executeActions(_ generation: Int) async {
        while generation == epoch && active && !actions.isEmpty && !Task.isCancelled {
            let (dispatch, _) = actions.removeFirst(); state.lastAction = "\(dispatch.action.rawValue) · \(dispatch.commandID)"
            if let rejection = dispatch.rejection(configuration: state.configuration) { result(dispatch.commandID, error: rejection); continue }
            guard await flush(timeout: 2) else { result(dispatch.commandID, error: "action_timeout"); continue }
            if dispatch.action.isPower {
                result(dispatch.commandID, error: nil); offlineUntil = ProcessInfo.processInfo.systemUptime + 60
                enqueue(.goodbye, ["reason": .string(dispatch.action.rawValue)])
                guard await flush(timeout: 5) else { offlineUntil = 0; note("Pre-power messages did not drain; refused to execute power action."); result(dispatch.commandID, error: "action_timeout"); continue }
            }
            guard generation == epoch && !Task.isCancelled else { break }
            // Retain the observed PID across asynchronous execution: the
            // reconciler may already have removed it by the time quit returns.
            let originalPID = dispatch.bundleID.flatMap { sample.apps[$0] }
            if dispatch.action == .quit, let bid = dispatch.bundleID { quitMarks.insert(bid) }
            let error = await system.execute(dispatch)
            guard generation == epoch && !Task.isCancelled else { break }
            if let error { if dispatch.action.isPower { offlineUntil = 0 }; if dispatch.action == .quit, let bid = dispatch.bundleID { quitMarks.remove(bid) }; result(dispatch.commandID, error: error); note("\(dispatch.action.rawValue) refused or failed: \(error)."); continue }
            if dispatch.action.isPower { quitMarks.formUnion(sample.apps.keys); note("\(dispatch.action.rawValue) initiation requested; ESP32 verifies the transition."); continue }
            let until = ProcessInfo.processInfo.systemUptime + 10
            var observed = false
            while generation == epoch && !Task.isCancelled && ProcessInfo.processInfo.systemUptime < until {
                let s = await system.sample(allowlist: state.configuration.allowlist)
                if let bid = dispatch.bundleID, (s.apps[bid] != nil) == (dispatch.action == .launch) {
                    if dispatch.action == .launch { enqueue(.started, ["bundle_id": .string(bid), "pid": .number(Double(s.apps[bid] ?? 0))]) }
                    else if let pid = originalPID { enqueue(.exited, ["bundle_id": .string(bid), "pid": .number(Double(pid)), "reason": .string("requested_by_agent")]) }
                    observed = true; break
                }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            if generation == epoch { result(dispatch.commandID, error: observed ? nil : "action_timeout"); note("\(dispatch.action.rawValue) \(observed ? "observed" : "not observed before timeout").") }
        }
        if generation == epoch { actionTask = nil }
    }
    private func capabilities() {
        enqueue(.capabilities, ["agent_version": .string(EventFactory.version), "protocol_version": .number(2), "os_version": .string(ProcessInfo.processInfo.operatingSystemVersionString), "hardware_model": .string(sample.hardwareModel), "enabled_commands": .array(AgentAction.allCases.filter { state.configuration.enabledCommands[$0.rawValue] == true }.map { .string($0.rawValue) }), "allowlisted_apps": .array(state.configuration.allowlist.map { .object(["bundle_id": .string($0), "state": .string(sample.apps[$0] != nil ? "running" : "not_running")]) })])
    }
    private func initialBurst() {
        enqueue(.system, ["state": .string("awake")])
        if let logged = sample.loggedIn { enqueue(.user, ["user_logged_in": .bool(logged), "user": .optional(sample.user)]) }
        if let locked = sample.locked { enqueue(.lock, ["locked": .bool(locked)]) } else { note("Screen lock state unknown; no unlocked evidence emitted.") }
        capabilities()
        for (bid, pid) in sample.apps { enqueue(.started, ["bundle_id": .string(bid), "pid": .number(Double(pid))]) }
        state.runningApps = sample.apps; state.screenLocked = sample.locked; frontSeen = false
    }
    private func sampleDeltas() async {
        let old = sample, s = await system.sample(allowlist: state.configuration.allowlist)
        for (bid, pid) in s.apps where old.apps[bid] != pid { enqueue(.started, ["bundle_id": .string(bid), "pid": .number(Double(pid))]) }
        for (bid, pid) in old.apps where s.apps[bid] == nil { enqueue(.exited, ["bundle_id": .string(bid), "pid": .number(Double(pid)), "reason": .string(quitMarks.remove(bid) != nil ? "requested_by_agent" : "quit")]) }
        if let locked = s.locked, locked != old.locked { enqueue(.lock, ["locked": .bool(locked)]) }
        if let logged = s.loggedIn, logged != old.loggedIn || s.user != old.user { enqueue(.user, ["user_logged_in": .bool(logged), "user": .optional(s.user)]) }
        if !frontSeen || old.front != s.front { enqueue(.front, ["bundle_id": .optional(s.front)]); frontSeen = true }
        sample = s; state.runningApps = s.apps; state.screenLocked = s.locked
    }
}
