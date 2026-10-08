import XCTest
@testable import MacControlCore

final class CoreTests: XCTestCase {
    func testWireNullsAndSequenceReset() throws {
        var f = EventFactory(instanceID: "ag-test")
        let h = try f.event(.hello, payload: ["protocol_version": .number(2)])
        let object = try JSONDecoder().decode([String: JSONValue].self, from: h.encode())
        expectEqual(object["session_id"], .null); expectEqual(object["command_id"], .null); expectEqual(h.seq, 1)
        f.begin("s1"); expectEqual(try f.event(.heartbeat, payload: [:]).seq, 2)
        f.end(); XCTAssertThrowsError(try f.event(.heartbeat, payload: [:]))
        expectEqual(try f.event(.hello, payload: [:]).seq, 1)
    }
    func testOversizeDoesNotConsumeSequence() throws {
        var f = EventFactory(instanceID: "ag-test")
        XCTAssertThrowsError(try f.event(.hello, payload: ["x": .string(String(repeating: "é", count: 3000))]))
        expectEqual(try f.event(.hello, payload: [:]).seq, 1)
    }
    func testStrictDispatchShapeAndFailClosed() throws {
        var c = AgentConfiguration()
        let app = try Dispatch(["action": .string("launch_app"), "bundle_id": .string("com.test.App"), "command_id": .string("c1")])
        expectEqual(app.rejection(configuration: c), "command_disabled")
        c.enabledCommands["launch_app"] = true; expectEqual(app.rejection(configuration: c), "app_not_allowlisted")
        c.allowlist = ["com.test.App"]; XCTAssertNil(app.rejection(configuration: c))
        for invalid: [String: JSONValue] in [
            ["action": .string("sleep"), "bundle_id": .string("com.test.App"), "command_id": .string("c1")],
            ["action": .string("launch_app"), "bundle_id": .string("com.test.App;rm"), "command_id": .string("c1")],
            ["action": .string("shell"), "command_id": .string("c1")],
            ["action": .string("shutdown"), "command_id": .string("c1"), "script": .string("evil")]
        ] { XCTAssertThrowsError(try Dispatch(invalid)) }
    }
    func testBackoffAndLegacyBootIdentity() {
        expectEqual((1...6).map { EventFactory.backoff($0, jitter: 0) }, [1,2,4,8,30,30])
        expectEqual(EventFactory.backoff(5, jitter: 1), 36)
        expectEqual(EventFactory.bootID(seconds: 0x12345678), "b_345678")
    }
    func testHelloVersionAndPollingContract() throws {
        expectEqual(try EventFactory.helloAck(["session_id": .string("s1")], websocket: false), "s1")
        XCTAssertThrowsError(try EventFactory.helloAck(["session_id": .string("s1"), "type": .string("hello_ack"), "protocol_version": .number(1)], websocket: true))
        XCTAssertThrowsError(try EventFactory.helloAck([:], websocket: false))
    }
    func testLegacyImportPreservesFlagsAndRefusesFutureSchema() throws {
        let bytes = Data(legacy.utf8)
        let (c, token) = try AgentConfiguration.importLegacy(bytes)
        expectEqual(c.instanceID, "ag-05e1"); expectEqual(token, "synthetic-secret")
        expectEqual(c.allowlist, ["com.test.App"]); expectEqual(c.enabledCommands["launch_app"], true)
        expectEqual(c.enabledCommands["shutdown"], false); expectFalse(c.enabled)
        expectEqual(c.importedBootID, "b_345678"); expectEqual(c.transport, "polling")
        XCTAssertThrowsError(try AgentConfiguration.importLegacy(Data(legacy.replacingOccurrences(of: "schema_version\":1", with: "schema_version\":2").utf8)))
        expectFalse(String(data: try JSONEncoder().encode(c), encoding: .utf8)!.contains(token))
    }
    func testConfigurationRejectsOversizedCapabilityReportAndExtremeLegacyNumbers() throws {
        var c = AgentConfiguration()
        c.allowlist = (0..<24).map { "com.test." + String(repeating: "a", count: 220) + String($0) }
        XCTAssertThrowsError(try c.validate())
        XCTAssertThrowsError(try AgentConfiguration.importLegacy(Data(legacy.replacingOccurrences(of: "poll_interval_s\":5", with: "poll_interval_s\":1e300").utf8)))
        XCTAssertThrowsError(try AgentConfiguration.importLegacy(Data(legacy.replacingOccurrences(of: "boot_key\":305419896", with: "boot_key\":1e300").utf8)))
    }
    func testMigrationBackupOriginalAndChangedFileGuard() throws {
        let root = temporary(); defer { try? FileManager.default.removeItem(at: root) }
        try NativePaths.prepare(root)
        let original = root.appendingPathComponent("agent.json"); let bytes = Data(legacy.utf8); try bytes.write(to: original)
        let (c, _) = try Migration.stage(from: original.path, root: root)
        expectEqual(try Data(contentsOf: original), bytes)
        expectEqual(try Data(contentsOf: URL(fileURLWithPath: c.legacyBackupPath!)), bytes)
        try Migration.validateOriginal(c); expectFalse(try Migration.settingsDiffer(c))
        var changed = c; changed.enabledCommands["shutdown"] = true; expectTrue(try Migration.settingsDiffer(changed))
        try Data(legacy.replacingOccurrences(of: "com.test.App", with: "com.test.Other").utf8).write(to: original)
        XCTAssertThrowsError(try Migration.validateOriginal(c))
    }
    func testCorruptConfigRefusedRatherThanReset() throws {
        let root = temporary(); defer { try? FileManager.default.removeItem(at: root) }
        try NativePaths.prepare(root); let file = root.appendingPathComponent("configuration.json")
        try Data("broken".utf8).write(to: file)
        XCTAssertThrowsError(try NativePaths.load(from: file)); expectEqual(try Data(contentsOf: file), Data("broken".utf8))
        var c = AgentConfiguration(); c.hostname = "x.local/path"; XCTAssertThrowsError(try c.validate())
    }
    func testCrashCooldownAndNormalShutdown() {
        var history = CrashHistory(); let now = Date(timeIntervalSince1970: 1000)
        expectEqual(history.begin(at: now), 0)
        expectEqual(history.begin(at: now.addingTimeInterval(10)), 0)
        expectEqual(history.begin(at: now.addingTimeInterval(20)), 0)
        expectEqual(history.begin(at: now.addingTimeInterval(30)), 60)
        history.cleanStop(); expectEqual(history.begin(at: now.addingTimeInterval(40)), 0)
    }
    func testRuntimeRejectsDisabledAndMalformedWithoutExecution() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        let runtime = makeRuntime(transport, executor, root: root)
        try await runtime.start()
        expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        await transport.push(["action": .string("shutdown"), "command_id": .string("bad-disabled")])
        await transport.push(["action": .string("sleep"), "command_id": .string("bad-shape"), "shell": .string("evil")])
        expectTrue(await eventually { await transport.events().filter { $0.type == .result }.count >= 2 })
        expectEqual(await executor.executions(), [])
        let events = await transport.events()
        expectEqual(events.filter { $0.type == .result }.map { $0.payload["error_code"] }, [.string("command_disabled"), .string("app_not_allowlisted")])
        await runtime.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testPowerGoodbyeAndAckSentBeforeExecutionAndDuplicateIgnored() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        var c = AgentConfiguration(); c.hostname = "fixture.local"; c.enabledCommands["sleep"] = true
        let runtime = makeRuntime(transport, executor, configuration: c, root: root)
        await executor.check { await transport.events().map(\.type) }
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        let dispatch: [String: JSONValue] = ["action": .string("sleep"), "command_id": .string("power-1")]
        await transport.push(dispatch); expectTrue(await eventually { await executor.executions().count == 1 })
        let whenExecuted = await executor.eventsAtExecution()
        expectTrue(whenExecuted.contains(.ack)); expectTrue(whenExecuted.contains(.goodbye)); expectTrue(whenExecuted.contains(.result))
        await transport.push(dispatch); try await Task.sleep(nanoseconds: 100_000_000)
        expectEqual(await executor.executions(), [.sleep])
        await runtime.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testUnknownLockNoFabricatedUnlockAndAlreadyRunningLaunchEvidence() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        var c = AgentConfiguration(); c.hostname = "fixture.local"; c.allowlist = ["com.test.App"]; c.enabledCommands["launch_app"] = true
        let runtime = makeRuntime(transport, executor, configuration: c, root: root)
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        expectFalse(await transport.events().contains { $0.type == .lock })
        await transport.push(["action": .string("launch_app"), "command_id": .string("launch-1"), "bundle_id": .string("com.test.App")])
        expectTrue(await eventually { await transport.events().contains { $0.type == .result && $0.command_id == "launch-1" } })
        expectTrue(await transport.events().contains { $0.type == .started && $0.payload["bundle_id"] == .string("com.test.App") })
        await runtime.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testSlowQuitRetainsObservedPIDAndRequestProvenance() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        var c = AgentConfiguration(); c.allowlist = ["com.test.App"]; c.enabledCommands["quit_app"] = true
        await executor.delayQuitReturn()
        let runtime = makeRuntime(transport, executor, configuration: c, root: root)
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        await transport.push(["action": .string("quit_app"), "command_id": .string("slow-quit"), "bundle_id": .string("com.test.App")])
        expectTrue(await eventually { await transport.events().contains { $0.type == .result && $0.command_id == "slow-quit" } })
        let exits = await transport.events().filter { $0.type == .exited }
        expectFalse(exits.isEmpty)
        expectTrue(exits.allSatisfy { $0.payload["pid"] == .number(123) && $0.payload["reason"] == .string("requested_by_agent") })
        await runtime.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testRefusedQuitDoesNotClaimLaterExternalExit() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        var c = AgentConfiguration(); c.allowlist = ["com.test.App"]; c.enabledCommands["quit_app"] = true
        await executor.refuseQuit()
        let runtime = makeRuntime(transport, executor, configuration: c, root: root)
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        await transport.push(["action": .string("quit_app"), "command_id": .string("refused-quit"), "bundle_id": .string("com.test.App")])
        expectTrue(await eventually { await transport.events().contains { $0.type == .result && $0.payload["error_code"] == .string("quit_failed") } })
        await executor.exitExternally()
        expectTrue(await eventually { await transport.events().contains { $0.type == .exited } })
        let exits = await transport.events().filter { $0.type == .exited }
        expectTrue(exits.allSatisfy { $0.payload["reason"] == .string("quit") })
        await runtime.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testHaltSurvivesRestartAndLogRedactsCredential() async throws {
        let transport = TestTransport(halt: true); let executor = TestExecutor(); let root = temporary()
        let runtime = makeRuntime(transport, executor, root: root)
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "halted" })
        await runtime.note("synthetic-secret must not escape")
        expectFalse(await runtime.snapshot().logs.contains { $0.message.contains("synthetic-secret") })
        let c = await runtime.snapshot().configuration
        await runtime.stop(persist: false)
        let replacement = makeRuntime(TestTransport(), executor, configuration: c, root: root)
        await replacement.resumeOnLogin(); expectEqual(await replacement.snapshot().phase, "halted")
        await replacement.stop(); try? FileManager.default.removeItem(at: root)
    }
    func testRealHTTPAndWebSocketAgentSurfaces() async throws {
        let server = try FixtureServer(); defer { server.stop() }
        for mode in ["polling", "websocket"] {
            var c = AgentConfiguration(); c.hostname = "127.0.0.1"; c.port = server.port; c.transport = mode
            let t = EndpointTransport(configuration: c, token: "synthetic-secret")
            var f = EventFactory(instanceID: "ag-test")
            let hello = try f.event(.hello, payload: ["protocol_version": .number(2), "agent_version": .string(EventFactory.version), "boot_id": .string("b_123456"), "hostname": .string("127.0.0.1")])
            let ack = try await t.open(hello: hello.encode()); f.begin(try EventFactory.helloAck(ack, websocket: mode == "websocket"))
            // Foundation consumes pong frames while its receiver is active,
            // as it is in the production runtime's concurrent receive loop.
            async let incoming = t.receive()
            try await t.send(f.event(.heartbeat, payload: ["boot_id": .string("b_123456"), "uptime_s": .number(1)]).encode(), sessionID: f.sessionID!)
            let d = try await incoming; expectEqual(try Dispatch(d).action, .launch)
            await t.close()
        }
        let capture = try JSONDecoder().decode([[String: JSONValue]].self, from: Data(contentsOf: server.capture))
        expectEqual(capture.filter { $0["type"] == .string("agent_hello") }.count, 2)
        expectEqual(capture.filter { $0["type"] == .string("heartbeat") }.count, 2)
    }
    func testWebSocketMissingPongEndsChannelWithinDeadline() async throws {
        let server = try FixtureServer(ignorePings: true); defer { server.stop() }
        var c = AgentConfiguration(); c.hostname = "127.0.0.1"; c.port = server.port
        let transport = EndpointTransport(configuration: c, token: "synthetic-secret", heartbeatProbeTimeout: 0.2)
        var factory = EventFactory(instanceID: "ag-test")
        let hello = try factory.event(.hello, payload: ["protocol_version": .number(2)])
        let ack = try await transport.open(hello: hello.encode()); factory.begin(try EventFactory.helloAck(ack, websocket: true))
        let incoming = Task { try? await transport.receive() }
        let began = ProcessInfo.processInfo.systemUptime
        do { try await transport.send(factory.event(.heartbeat, payload: [:]).encode(), sessionID: factory.sessionID!); XCTFail("Missing pong must fail") }
        catch { expectTrue(ProcessInfo.processInfo.systemUptime - began < 2) }
        await transport.close(); _ = await incoming.value
    }
    func testCompleteRuntimeFramesForShippingFirmwareParser() async throws {
        let transport = TestTransport(); let executor = TestExecutor(); let root = temporary()
        var c = AgentConfiguration(); c.hostname = "fixture.local"; c.allowlist = ["com.test.App"]; c.enabledCommands["quit_app"] = true
        let runtime = makeRuntime(transport, executor, configuration: c, root: root)
        await executor.setLock(false)
        try await runtime.start(); expectTrue(await eventually { await runtime.snapshot().phase == "connected" })
        await transport.push(["action": .string("shell"), "command_id": .string("invalid-unknown")])
        await transport.push(["action": .string("sleep"), "command_id": .string("invalid-extra"), "bundle_id": .string("com.test.App")])
        await transport.push(["action": .string("quit_app"), "command_id": .string("quit-1"), "bundle_id": .string("com.test.App")])
        expectTrue(await eventually { await transport.events().contains { $0.type == .exited } })
        expectTrue(await eventually(seconds: 7) { await transport.events().contains { $0.type == .heartbeat } })
        await runtime.powerChanged(sleeping: true)
        await runtime.powerChanged(sleeping: false)
        try await Task.sleep(nanoseconds: 50_000_000)
        await runtime.stop()
        let events = await transport.events()
        let types = Set(events.map { $0.type.rawValue })
        expectEqual(types.count, 12)
        if let path = ProcessInfo.processInfo.environment["MACCONTROL_CAPTURE"] {
            let lines = try events.map { String(data: try $0.encode(), encoding: .utf8)! }.joined(separator: "\n") + "\n"
            try Data(lines.utf8).write(to: URL(fileURLWithPath: path))
        }
        try? FileManager.default.removeItem(at: root)
    }
    private func makeRuntime(_ t: TestTransport, _ s: TestExecutor, configuration: AgentConfiguration? = nil, root: URL) -> AgentRuntime {
        var c = configuration ?? AgentConfiguration(); c.hostname = "fixture.local"
        return AgentRuntime(configuration: c, system: s, root: root, credential: { _ in "synthetic-secret" }, transportFactory: { _, _ in t })
    }
}
private let legacy = #"{"schema_version":1,"hostname":"fixture.local","agent_instance_id":"ag-05e1","agent_token":"synthetic-secret","allowlist":["com.test.App"],"enabled_commands":{"launch_app":true,"quit_app":false},"transport":"polling","poll_interval_s":5,"boot_key":305419896,"boot_id":"b_345678"}"#
private func temporary() -> URL { FileManager.default.temporaryDirectory.appendingPathComponent("maccontrol-test-" + UUID().uuidString) }
private func eventually(seconds: Double = 5, _ predicate: () async -> Bool) async -> Bool {
    let deadline = ProcessInfo.processInfo.systemUptime + seconds
    while ProcessInfo.processInfo.systemUptime < deadline { if await predicate() { return true }; try? await Task.sleep(nanoseconds: 20_000_000) }
    return false
}
private actor TestTransport: AgentTransport {
    var sent: [Event] = []; var inbound: [[String: JSONValue]] = []; var closed = false
    let halt: Bool
    init(halt: Bool = false) { self.halt = halt }
    func open(hello: Data) async throws -> [String: JSONValue] {
        if halt { throw AgentFailure("revoked", halted: true) }
        sent.append(try JSONDecoder().decode(Event.self, from: hello))
        return ["session_id": .string("s-test"), "type": .string("hello_ack"), "protocol_version": .number(2)]
    }
    func send(_ data: Data, sessionID: String) async throws { try Task.checkCancellation(); sent.append(try JSONDecoder().decode(Event.self, from: data)) }
    func receive() async throws -> [String: JSONValue] {
        while inbound.isEmpty { try Task.checkCancellation(); if closed { throw AgentFailure("closed") }; try await Task.sleep(nanoseconds: 10_000_000) }
        return inbound.removeFirst()
    }
    func close() async { closed = true }
    func push(_ object: [String: JSONValue]) { inbound.append(object) }
    func events() -> [Event] { sent }
}
private actor TestExecutor: MacExecutor {
    var running = true; var lockState: Bool?; var slowQuit = false; var failedQuit = false
    func refuseQuit() { failedQuit = true }
    func exitExternally() { running = false }
    func delayQuitReturn() { slowQuit = true }
    func setLock(_ value: Bool) { lockState = value }
    var actions: [AgentAction] = []; var eventCheck: (() async -> [EventType])?; var observed: [EventType] = []
    func sample(allowlist: [String]) async -> SystemSample { SystemSample(apps: running && allowlist.contains("com.test.App") ? ["com.test.App": 123] : [:], locked: lockState, loggedIn: true, user: "fixture", bootTime: 1000, hardwareModel: "FixtureMac", heartbeat: ["boot_time": .number(1000), "mac_uptime_s": .number(10), "cpu_utilization_pct": .null, "memory_utilization_pct": .number(25), "disk_free_bytes": .number(5000000000), "network": .object(["reachable": .bool(true), "ip": .string("127.0.0.1")])]) }
    func execute(_ dispatch: Dispatch) async -> String? { if let eventCheck { observed = await eventCheck() }; actions.append(dispatch.action); if dispatch.action == .quit { if failedQuit { return "quit_failed" }; running = false; if slowQuit { try? await Task.sleep(nanoseconds: 1_200_000_000) } }; if dispatch.action == .launch { running = true }; return nil }
    func executions() -> [AgentAction] { actions }
    func check(_ f: @escaping () async -> [EventType]) { eventCheck = f }
    func eventsAtExecution() -> [EventType] { observed }
}
private final class FixtureServer {
    let process = Process(), capture: URL, port: Int
    init(ignorePings: Bool = false) throws {
        capture = temporary().appendingPathExtension("json")
        let script = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("Tests/endpoint_fixture.py")
        let pipe = Pipe(); process.executableURL = URL(fileURLWithPath: "/usr/bin/python3"); process.arguments = [script.path, capture.path] + (ignorePings ? ["ignore-pings"] : []); process.standardOutput = pipe; process.standardError = FileHandle.nullDevice
        try process.run()
        let first = pipe.fileHandleForReading.availableData
        guard let line = String(data: first, encoding: .utf8), let parsed = Int(line.trimmingCharacters(in: .whitespacesAndNewlines)) else { process.terminate(); throw AgentFailure("Fixture server failed to start") }
        port = parsed
    }
    func stop() { if process.isRunning { process.terminate(); process.waitUntilExit() }; try? FileManager.default.removeItem(at: capture) }
}

private func expectTrue(_ value: Bool, file: StaticString = #filePath, line: UInt = #line) { XCTAssertTrue(value, file: file, line: line) }
private func expectFalse(_ value: Bool, file: StaticString = #filePath, line: UInt = #line) { XCTAssertFalse(value, file: file, line: line) }
private func expectEqual<T: Equatable>(_ lhs: T, _ rhs: T, file: StaticString = #filePath, line: UInt = #line) { XCTAssertEqual(lhs, rhs, file: file, line: line) }
