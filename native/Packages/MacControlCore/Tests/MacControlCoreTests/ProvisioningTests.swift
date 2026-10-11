import XCTest
import CryptoKit
import Darwin
@testable import MacControlCore

final class ProvisioningTests: XCTestCase {
    private let credentials = SetupCredentials(ssid: "Client Wi-Fi", wifiPassword: " a \"quoted\" password ", adminPassword: "  Client admin password  ")
    func testJSONCredentialsPreserveWhitespaceAndRejectCommandInjection() throws {
        let line = try credentials.command(wifi: true)
        let values = try JSONDecoder().decode([String: String].self, from: Data(line.dropFirst(14).utf8))
        XCTAssertEqual(values["ssid"], credentials.ssid); XCTAssertEqual(values["password"], credentials.wifiPassword)
        XCTAssertThrowsError(try SetupCredentials(ssid: "Client\nreboot", wifiPassword: "password", adminPassword: "long password").validate())
        XCTAssertThrowsError(try SetupCredentials(ssid: String(repeating: "é", count: 17), wifiPassword: "password", adminPassword: "long password").validate())
        XCTAssertNoThrow(try SetupCredentials(ssid: "Open Wi-Fi", wifiPassword: "", adminPassword: "long password").validate())
    }
    func testCredentialExportIsPrivateAndDoesNotChangeParentPermissions() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o755]); defer { try? FileManager.default.removeItem(at: directory) }
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: directory.path)
        let record = PreparedEndpoint(deviceID: "mac-fixture", hostname: "mac-fixture", ip: "", firmwareVersion: "fixture", ssid: "Client Wi-Fi", adminPassword: "synthetic admin password")
        let path = directory.appendingPathComponent("client.json")
        try Data("old record".utf8).write(to: path); try record.export(to: path)
        let attributes = try FileManager.default.attributesOfItem(atPath: path.path), parent = try FileManager.default.attributesOfItem(atPath: directory.path)
        XCTAssertEqual((attributes[.posixPermissions] as? NSNumber)?.intValue, 0o600)
        XCTAssertEqual((parent[.posixPermissions] as? NSNumber)?.intValue, 0o755)
        let saved = try JSONDecoder().decode(PreparedEndpoint.self, from: Data(contentsOf: path)); XCTAssertEqual(saved.deviceID, record.deviceID)
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["client.json"])
    }
    func testPortSelectionNeverOpensArbitraryFiles() {
        XCTAssertTrue(SerialPorts.valid("/dev/cu.wchusbserial123")); XCTAssertTrue(SerialPorts.valid("/dev/cu.usbmodem123"))
        XCTAssertFalse(SerialPorts.valid("/tmp/port")); XCTAssertFalse(SerialPorts.valid("/dev/cu.usbserial/../../file")); XCTAssertFalse(SerialPorts.valid("/dev/cu.Bluetooth"))
    }
    func testFlashCapacityAcceptsEightAndSixteenMBAndRefusesUnverifiedSizes() {
        for size in [8, 16] {
            XCTAssertNoThrow(try FlashTool.validateFlashCapacity("Chip type: ESP32-S3\r\nDetected flash size: \(size)MB\r\nHard resetting via RTS pin...\r\n"))
        }
        for output in ["", "Detected flash size: 4MB", "Detected flash size: 18MB",
                       "Detected flash size: 128MB", "Detected flash size: 16KB",
                       "Warning: Detected flash size: 16MB", "Detected flash size: 16MB (unknown)",
                       "Detected flash size: 8MB\nDetected flash size: 16MB"] {
            XCTAssertThrowsError(try FlashTool.validateFlashCapacity(output), output)
        }
    }
    func testBundledFirmwareChecksIntegrityAndClosedFlashAddresses() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + " firmware")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true); defer { try? FileManager.default.removeItem(at: directory) }
        let names = ["bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"], offsets = [0, 0x8000, 0xe000, 0x10000]
        var files: [FirmwareFile] = []
        for (name,offset) in zip(names,offsets) {
            let bytes = Data("synthetic firmware \(name)".utf8); try bytes.write(to: directory.appendingPathComponent(name))
            files.append(FirmwareFile(name: name, offset: offset, size: bytes.count, sha256: SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()))
        }
        var manifest = FirmwareManifest(version: "fixture", chip: "esp32s3", flashBytes: 8*1024*1024, files: files)
        let path = directory.appendingPathComponent("manifest.json"); try JSONEncoder().encode(manifest).write(to: path)
        let package = try FirmwareBundle(directory: directory); let args = try package.flashArguments(port: "/dev/cu.usbserial123")
        XCTAssertTrue(args.contains(directory.appendingPathComponent("firmware.bin").path)); XCTAssertTrue(args.contains("0x10000")); XCTAssertFalse(args.contains("erase-flash"))
        let sizeIndex = try XCTUnwrap(args.firstIndex(of: "--flash-size"))
        XCTAssertEqual(args[sizeIndex + 1], "8MB")
        try Data("tampered".utf8).write(to: directory.appendingPathComponent("firmware.bin")); XCTAssertThrowsError(try FirmwareBundle(directory: directory))
        manifest.files[0].name = "../../private"; try JSONEncoder().encode(manifest).write(to: path); XCTAssertThrowsError(try FirmwareBundle(directory: directory))
    }
    func testSerialSetupPersistsEveryKeyAndClosesBeforeNetwork() async throws {
        let console = SetupConsole(), vault = SetupVault()
        let provisioner = SerialProvisioner(console: console, load: { vault.get($0) }, save: { vault.put($0) })
        let endpoint = try await provisioner.run(credentials: credentials, firmwareVersion: "fixture", progress: { _ in })
        XCTAssertEqual(endpoint.ip, "192.168.1.30"); XCTAssertEqual(endpoint.keys.count, 3)
        let closed = await console.didClose(); XCTAssertTrue(closed)
        XCTAssertEqual(vault.savedKeyCounts(), [0,1,2,3,3])
        let commands = await console.commands()
        let firstKey = commands.firstIndex(where: { $0.hasPrefix("key create") })!, wifi = commands.firstIndex(where: { $0.hasPrefix("wifi set-json") })!
        XCTAssertLessThan(firstKey, wifi)
        let cached = try JSONEncoder().encode(endpoint); XCTAssertFalse(String(decoding: cached, as: UTF8.self).contains(credentials.wifiPassword))
    }
    func testFailureRetainsCapturedKeysAndRetryReusesThem() async throws {
        let vault = SetupVault(), first = SetupConsole(failControl: true)
        let failed = SerialProvisioner(console: first, load: { vault.get($0) }, save: { vault.put($0) })
        do { _ = try await failed.run(credentials: credentials, firmwareVersion: "fixture", progress: { _ in }); XCTFail("Expected reboot") } catch { }
        XCTAssertEqual(vault.get("mac-fixture")?.keys.count, 1); let closed = await first.didClose(); XCTAssertTrue(closed)
        let next = SetupConsole(existingRead: true)
        let retry = SerialProvisioner(console: next, load: { vault.get($0) }, save: { vault.put($0) })
        let record = try await retry.run(credentials: credentials, firmwareVersion: "fixture", progress: { _ in })
        XCTAssertEqual(record.keys.count, 3)
        let retryCommands = await next.commands(); XCTAssertFalse(retryCommands.contains { $0.hasPrefix("key create READ") })
    }
    func testParsersRejectMalformedSecretsAndInvalidAddresses() throws {
        XCTAssertThrowsError(try ProvisionParser.key("created key-1 (label)\nmck_short\n"))
        XCTAssertThrowsError(try ProvisionParser.key("created key-1 (label)\nmck_1234567890123456789012345\nWARNING: persist failed"))
        XCTAssertNil(ProvisionParser.ip("connected ip=1..2.3.4")); XCTAssertNil(ProvisionParser.ip("connected ip=999.1.1.1")); XCTAssertEqual(ProvisionParser.ip("connected ip=192.168.1.30\r\n"), "192.168.1.30")
    }
    func testPrepareForAnotherMacDoesNotRequireClientWiFi() async throws {
        let console = SetupConsole(), vault = SetupVault()
        let record = try await SerialProvisioner(console: console, load: { vault.get($0) }, save: { vault.put($0) }).run(credentials: credentials, firmwareVersion: "fixture", waitForWiFi: false, progress: { _ in })
        XCTAssertEqual(record.keys.count, 3); XCTAssertEqual(record.ip, "")
        let commands = await console.commands(); XCTAssertFalse(commands.contains("wifi status"))
        let closed = await console.didClose(); XCTAssertTrue(closed)
    }
    func testProcessCapturesFinalOutputAndCancellation() async throws {
        let output = try await ToolProcess.run(URL(fileURLWithPath: "/usr/bin/printf"), arguments: [String(repeating: "x", count: 20000) + "FINAL"], timeout: 5)
        XCTAssertTrue(output.hasSuffix("FINAL")); XCTAssertEqual(output.count, 20005)
        let task = Task { try await ToolProcess.run(URL(fileURLWithPath: "/bin/sleep"), arguments: ["30"], timeout: 40) }
        try await Task.sleep(nanoseconds: 200_000_000); task.cancel()
        do { _ = try await task.value; XCTFail("Must cancel") } catch is CancellationError { } catch { XCTFail("Wrong cancellation error") }
    }
    func testNativeSerialReadsFragmentedRepliesAndTimesOutWithoutSecretLeak() async throws {
        var master: Int32 = -1, slave: Int32 = -1
        XCTAssertEqual(openpty(&master, &slave, nil, nil, nil), 0)
        defer { Darwin.close(master) }
        var attributes = termios(); XCTAssertEqual(tcgetattr(slave, &attributes), 0); cfmakeraw(&attributes)
        XCTAssertEqual(tcsetattr(slave, TCSANOW, &attributes), 0); _ = fcntl(slave, F_SETFL, O_NONBLOCK)
        let console = NativeSerialConsole(testHandle: slave), writer = master
        let reply = Task.detached {
            try await Task.sleep(nanoseconds: 100_000_000)
            for fragment in ["created key-1 (native-setup)\n", "API key (shown once, store it now):\n", "mck_" + String(repeating: "a", count: 30) + "\n"] {
                let bytes = Array(fragment.utf8); _ = bytes.withUnsafeBytes { Darwin.write(writer, $0.baseAddress!, bytes.count) }
                try await Task.sleep(nanoseconds: 100_000_000)
            }
        }
        let response = try await console.command("key create READ native-setup", endings: ["created key-"], timeout: 3)
        _ = try await reply.value; XCTAssertEqual(try ProvisionParser.key(response).id, "key-1")
        do { _ = try await console.command("synthetic-secret", endings: ["never"], timeout: 0.1); XCTFail("Must time out") }
        catch { XCTAssertFalse(error.localizedDescription.contains("synthetic-secret")) }
        await console.close()
    }
    func testControllerVerifiesPhysicalIdentityAndAuthenticatedPairing() async throws {
        let fixture = try SetupHTTPFixture(); defer { fixture.stop() }
        let client = try ControllerSetupClient(host: "127.0.0.1", port: fixture.port)
        var record = PreparedEndpoint(deviceID: "mac-fixture", hostname: "mac-fixture", ip: "127.0.0.1", firmwareVersion: "fixture", ssid: "Client Wi-Fi", adminPassword: "synthetic admin password")
        record.keys["READ"] = EndpointKey(id: "key-1", raw: "synthetic-read")
        try await client.verify(record, timeout: 3)
        let code = try await client.pairingCode(adminPassword: record.adminPassword); XCTAssertEqual(code, "12345678")
        try await client.verifyAgent(instanceID: "ag-fixture", readKey: "synthetic-read", timeout: 3)
    }
    func testControllerRefusesDifferentPhysicalDevice() async throws {
        let fixture = try SetupHTTPFixture(wrongDevice: true); defer { fixture.stop() }
        let client = try ControllerSetupClient(host: "127.0.0.1", port: fixture.port)
        var record = PreparedEndpoint(deviceID: "mac-fixture", hostname: "mac-fixture", ip: "127.0.0.1", firmwareVersion: "fixture", ssid: "Client Wi-Fi", adminPassword: "synthetic admin password")
        record.keys["READ"] = EndpointKey(id: "key-1", raw: "synthetic-read")
        do { try await client.verify(record, timeout: 3); XCTFail("Must refuse mismatched device") }
        catch { XCTAssertEqual((error as? AgentFailure)?.halted, true) }
    }
    func testToolDeadlineStopsItsOwnProcess() async throws {
        do { _ = try await ToolProcess.run(URL(fileURLWithPath: "/bin/sleep"), arguments: ["30"], timeout: 0.1); XCTFail("Must time out") }
        catch { XCTAssertTrue(error.localizedDescription.contains("timed out")) }
    }
}
private final class SetupVault: @unchecked Sendable {
    private let lock = NSLock(); private var values: [String: PreparedEndpoint] = [:]; private var counts: [Int] = []
    func get(_ id: String) -> PreparedEndpoint? { lock.lock(); defer { lock.unlock() }; return values[id] }
    func put(_ record: PreparedEndpoint) { lock.lock(); values[record.deviceID] = record; counts.append(record.keys.count); lock.unlock() }
    func savedKeyCounts() -> [Int] { lock.lock(); defer { lock.unlock() }; return counts }
}
private actor SetupConsole: ProvisionConsole {
    let failControl: Bool, existingRead: Bool
    var sent: [String] = []; var closed = false
    init(failControl: Bool = false, existingRead: Bool = false) { self.failControl = failControl; self.existingRead = existingRead }
    func ready() async throws { }
    func close() async { closed = true }
    func commands() -> [String] { sent }
    func didClose() -> Bool { closed }
    func command(_ line: String, endings: [String], timeout: Double) async throws -> String {
        sent.append(line)
        if line == "help" { return "wifi set-json\nadmin set-json\nagent pair" }
        if line == "identity show" { return "hostname: mac-fixture\ndevice_id: mac-fixture\n" }
        if line == "key list" { return existingRead ? "  key-1  role=READ    native-setup active=yes created=fixture\n1 active of 8 max" : "(no keys)" }
        if line.hasPrefix("admin set-json") { return "admin password set" }
        if line.hasPrefix("wifi set-json") { return "wifi credentials saved; reconnecting" }
        if line == "wifi status" { return "connected ip=192.168.1.30" }
        if line.hasPrefix("key create CONTROL"), failControl { throw AgentFailure("Device restarted") }
        if line.hasPrefix("key create") {
            let id = line.contains("READ") ? 1 : line.contains("CONTROL") ? 2 : 3
            return "created key-\(id) (native-setup)\nAPI key (shown once, store it now):\nmck_" + String(repeating: "a", count: 30) + "\(id)\n"
        }
        throw AgentFailure("Unexpected test command")
    }
}

private final class SetupHTTPFixture {
    let process = Process(), port: Int
    init(wrongDevice: Bool = false) throws {
        let script = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("Tests/setup_fixture.py")
        let pipe = Pipe(); process.executableURL = URL(fileURLWithPath: "/usr/bin/python3"); process.arguments = [script.path] + (wrongDevice ? ["wrong-device"] : [])
        process.standardOutput = pipe; process.standardError = FileHandle.nullDevice; try process.run()
        guard let line = String(data: pipe.fileHandleForReading.availableData, encoding: .utf8), let parsed = Int(line.trimmingCharacters(in: .whitespacesAndNewlines)) else { process.terminate(); throw AgentFailure("Setup fixture could not start") }; port = parsed
    }
    func stop() { if process.isRunning { process.terminate(); process.waitUntilExit() } }
}
