import Foundation
import CryptoKit
import Darwin

public struct SetupCredentials: Sendable {
    public var ssid: String
    public var wifiPassword: String
    public var adminPassword: String
    public init(ssid: String, wifiPassword: String, adminPassword: String) { self.ssid = ssid; self.wifiPassword = wifiPassword; self.adminPassword = adminPassword }
    public func validate() throws {
        let values = [ssid, wifiPassword, adminPassword]
        guard values.allSatisfy({ !$0.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) } }),
              (1...32).contains(ssid.utf8.count), wifiPassword.isEmpty || (8...63).contains(wifiPassword.utf8.count),
              (10...128).contains(adminPassword.utf8.count) else {
            throw AgentFailure("Use a Wi-Fi name of 1–32 bytes, an empty password for an open network or 8–63 bytes, and an admin password of 10–128 bytes. Control characters are not allowed.")
        }
    }
    public func command(wifi: Bool) throws -> String {
        try validate()
        let data = try JSONEncoder().encode(wifi ? ["ssid": ssid, "password": wifiPassword] : ["password": adminPassword])
        let line = (wifi ? "wifi set-json " : "admin set-json ") + String(decoding: data, as: UTF8.self)
        guard line.utf8.count <= 512 else { throw AgentFailure("Credentials exceed the device console limit") }
        return line
    }
}
public struct EndpointKey: Codable, Sendable, Equatable { public var id: String; public var raw: String }
public struct PreparedEndpoint: Codable, Sendable {
    public var deviceID: String
    public var hostname: String
    public var ip: String
    public var firmwareVersion: String
    public var ssid: String
    public var adminPassword: String
    public var keys: [String: EndpointKey] = [:]
    public init(deviceID: String, hostname: String, ip: String, firmwareVersion: String, ssid: String, adminPassword: String) {
        self.deviceID = deviceID; self.hostname = hostname; self.ip = ip; self.firmwareVersion = firmwareVersion; self.ssid = ssid; self.adminPassword = adminPassword
    }
    public var credentialAccount: String { "provisioning." + deviceID }
}
extension PreparedEndpoint {
    public func export(to url: URL) throws {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(self)
        var path = Array(url.deletingLastPathComponent().appendingPathComponent(".maccontrol-credentials-XXXXXX").path.utf8CString)
        let handle = mkstemp(&path)
        guard handle >= 0 else { throw AgentFailure("Could not create a private credential export") }
        let temporary = String(cString: path)
        defer { Darwin.close(handle); unlink(temporary) }
        guard fchmod(handle, 0o600) == 0 else { throw AgentFailure("Could not protect credential export") }
        var offset = 0
        while offset < data.count {
            let count = data.withUnsafeBytes { Darwin.write(handle, $0.baseAddress!.advanced(by: offset), data.count - offset) }
            if count < 0 && errno == EINTR { continue }
            guard count > 0 else { throw AgentFailure("Could not write credential export") }
            offset += count
        }
        guard fsync(handle) == 0, rename(temporary, url.path) == 0 else { throw AgentFailure("Could not save credential export") }
    }
}
public struct FirmwareFile: Codable, Sendable { public var name: String; public var offset: Int; public var size: Int; public var sha256: String }
public struct FirmwareManifest: Codable, Sendable { public var version: String; public var chip: String; public var flashBytes: Int; public var files: [FirmwareFile] }
public struct FirmwareBundle: Sendable {
    public let directory: URL
    public let manifest: FirmwareManifest
    public init(directory: URL) throws {
        let m = try JSONDecoder().decode(FirmwareManifest.self, from: Data(contentsOf: directory.appendingPathComponent("manifest.json")))
        guard m.chip == "esp32s3", m.flashBytes == 8 * 1024 * 1024,
              m.files.map(\.name) == ["bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"],
              m.files.map(\.offset) == [0, 0x8000, 0xe000, 0x10000] else { throw AgentFailure("Unsupported bundled firmware profile") }
        for (index, file) in m.files.enumerated() {
            let bytes = try Data(contentsOf: directory.appendingPathComponent(file.name))
            guard bytes.count == file.size, file.size > 0, file.size <= ([0x8000, 0x9000, 0x10000, 0x200000][index] - file.offset),
                  SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == file.sha256 else { throw AgentFailure("Bundled firmware integrity check failed") }
        }
        self.directory = directory; manifest = m
    }
    public func flashArguments(port: String) throws -> [String] {
        guard SerialPorts.valid(port) else { throw AgentFailure("Choose a USB serial port") }
        return ["--chip", "esp32s3", "--port", port, "--baud", "460800", "--before", "default-reset", "--after", "hard-reset", "write-flash", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB"] + manifest.files.flatMap { [String(format: "0x%x", $0.offset), directory.appendingPathComponent($0.name).path] }
    }
}
public enum SerialPorts {
    public static func valid(_ path: String) -> Bool {
        path.hasPrefix("/dev/cu.") && !path.dropFirst(5).contains("/") && ["usbmodem", "usbserial", "wchusbserial", "SLAB_USBtoUART"].contains(where: { path.dropFirst(8).hasPrefix($0) })
    }
    public static func available() -> [String] { ((try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []).map { "/dev/" + $0 }.filter(valid).sorted() }
}
public protocol ProvisionConsole: Sendable {
    func ready() async throws
    func command(_ line: String, endings: [String], timeout: Double) async throws -> String
    func close() async
}
public actor NativeSerialConsole: ProvisionConsole {
    private var fd: Int32 = -1
    public init(port: String) throws {
        guard SerialPorts.valid(port) else { throw AgentFailure("Choose a USB serial port") }
        let handle = Darwin.open(port, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard handle >= 0 else { throw AgentFailure("Cannot open USB serial port. Close other serial tools and check the cable/driver.") }
        var attributes = termios(), st = stat()
        guard fstat(handle, &st) == 0, (st.st_mode & S_IFMT) == S_IFCHR,
              ioctl(handle, TIOCEXCL) == 0, tcgetattr(handle, &attributes) == 0 else { Darwin.close(handle); throw AgentFailure("USB serial port is unavailable or busy") }
        cfmakeraw(&attributes); cfsetispeed(&attributes, speed_t(B115200)); cfsetospeed(&attributes, speed_t(B115200)); attributes.c_cflag |= tcflag_t(CLOCAL | CREAD)
        guard tcsetattr(handle, TCSANOW, &attributes) == 0 else { Darwin.close(handle); throw AgentFailure("Cannot configure the USB serial port") }
        var mask = Int32(TIOCM_DTR | TIOCM_RTS); _ = ioctl(handle, TIOCMBIC, &mask)
        fd = handle
    }
    #if DEBUG
    // Test-owned pseudo-terminal only; release builds expose no arbitrary FD path.
    init(testHandle: Int32) { fd = testHandle }
    #endif
    deinit { if fd >= 0 { Darwin.close(fd) } }
    public func close() async { if fd >= 0 { Darwin.close(fd); fd = -1 } }
    private func read() throws -> String {
        guard fd >= 0 else { throw AgentFailure("USB serial connection closed") }
        var buffer = [UInt8](repeating: 0, count: 4096)
        let count = Darwin.read(fd, &buffer, buffer.count)
        if count < 0, errno != EAGAIN && errno != EINTR { throw AgentFailure("USB device disconnected") }
        return count > 0 ? String(decoding: buffer.prefix(count), as: UTF8.self) : ""
    }
    private func write(_ line: String) async throws {
        let data = Data((line + "\n").utf8), until = ProcessInfo.processInfo.systemUptime + 3
        var offset = 0
        while offset < data.count {
            try Task.checkCancellation()
            guard fd >= 0, ProcessInfo.processInfo.systemUptime < until else { throw AgentFailure("USB write timed out") }
            let count = data.withUnsafeBytes { Darwin.write(fd, $0.baseAddress!.advanced(by: offset), data.count - offset) }
            if count > 0 { offset += count } else if count < 0 && errno != EAGAIN && errno != EINTR { throw AgentFailure("USB write failed") }
            else { try await Task.sleep(nanoseconds: 20_000_000) }
        }
    }
    public func ready() async throws {
        // Opening the bridge can reset the board. Wait before poking the CLI;
        // a running board without a new banner is also handled by help.
        try await Task.sleep(nanoseconds: 2_000_000_000)
        for attempt in 0..<3 {
            do {
                _ = try read()
                _ = try await command("help", endings: ["agent pair", "unknown command"], timeout: 10)
                return
            } catch {
                try Task.checkCancellation()
                if attempt == 2 { throw error }
                try await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }
    public func command(_ line: String, endings: [String], timeout: Double = 20) async throws -> String {
        guard !line.contains("\n"), !line.contains("\r"), line.utf8.count <= 512 else { throw AgentFailure("Invalid console command") }
        _ = try read(); try await write(line)
        let until = ProcessInfo.processInfo.systemUptime + timeout
        var text = "", matchedAt: Double?
        while ProcessInfo.processInfo.systemUptime < until {
            try Task.checkCancellation(); text += try read()
            guard text.utf8.count <= 32768 else { throw AgentFailure("Unexpectedly large device reply") }
            if text.contains("ESP-ROM:esp32s3") || text.contains("Physical console = ADMIN") { throw AgentFailure("Device restarted during setup. Completed credentials were retained; retry setup to continue.") }
            if endings.contains(where: text.contains), matchedAt == nil { matchedAt = ProcessInfo.processInfo.systemUptime }
            if let matchedAt, ProcessInfo.processInfo.systemUptime - matchedAt >= 0.8 { return text }
            try await Task.sleep(nanoseconds: 30_000_000)
        }
        // Never include the sent command or raw reply: either can hold secrets.
        throw AgentFailure("Device did not finish the setup command. Check the COM cable, power, and serial driver; retry to continue.")
    }
}
public enum ProvisionParser {
    public static func value(_ name: String, in text: String) -> String? {
        text.components(separatedBy: .newlines).first(where: { $0.hasPrefix(name + ": ") }).map { String($0.dropFirst(name.count + 2)).trimmingCharacters(in: .whitespacesAndNewlines) }
    }
    public static func key(_ text: String) throws -> EndpointKey {
        guard !text.contains("persist failed"),
              let raw = text.components(separatedBy: .newlines).map({ $0.trimmingCharacters(in: .whitespacesAndNewlines) }).first(where: { $0.range(of: "^mck_[A-Za-z0-9_-]{20,}$", options: .regularExpression) != nil }),
              let range = text.range(of: "(?m)^created (key-[0-9]+) ", options: .regularExpression) else { throw AgentFailure("Could not securely capture an API key. Check the device key store before retrying.") }
        let id = text[range].split(separator: " ")[1]
        return EndpointKey(id: String(id), raw: raw)
    }
    public static func ip(_ text: String) -> String? {
        guard let range = text.range(of: "connected ip=[0-9.]+", options: .regularExpression) else { return nil }
        let ip = text[range].dropFirst(13), octets = ip.split(separator: ".", omittingEmptySubsequences: false).compactMap { Int($0) }
        return ip.split(separator: ".", omittingEmptySubsequences: false).count == 4 && octets.count == 4 && octets.allSatisfy({ (0...255).contains($0) }) ? String(ip) : nil
    }
}
public struct SerialProvisioner: Sendable {
    public typealias Progress = @Sendable (String) async -> Void
    public typealias Save = @Sendable (PreparedEndpoint) async throws -> Void
    private let console: any ProvisionConsole
    private let load: @Sendable (String) throws -> PreparedEndpoint?
    private let save: Save
    public init(console: any ProvisionConsole,
                load: @escaping @Sendable (String) throws -> PreparedEndpoint? = { id in
                    guard let value = try TokenStore.read("provisioning." + id) else { return nil }
                    return try JSONDecoder().decode(PreparedEndpoint.self, from: Data(value.utf8))
                }, save: @escaping Save = { try TokenStore.write(String(decoding: JSONEncoder().encode($0), as: UTF8.self), id: $0.credentialAccount) }) {
        self.console = console; self.load = load; self.save = save
    }
    public func run(credentials: SetupCredentials, firmwareVersion: String, waitForWiFi: Bool = true,
                    step: @escaping @Sendable (SetupStep) async -> Void = { _ in }, progress: @escaping Progress) async throws -> PreparedEndpoint {
        do { let result = try await setup(credentials, firmwareVersion, waitForWiFi, step, progress); await console.close(); return result }
        catch { await console.close(); throw error }
    }
    private func setup(_ credentials: SetupCredentials, _ version: String, _ waitForWiFi: Bool,
                       _ step: @Sendable (SetupStep) async -> Void, _ progress: Progress) async throws -> PreparedEndpoint {
        await step(.usb)
        try credentials.validate(); try await console.ready()
        let help = try await console.command("help", endings: ["agent pair"], timeout: 10)
        guard help.contains("wifi set-json"), help.contains("admin set-json") else { throw AgentFailure("This firmware lacks the native setup commands. Select Flash firmware and retry.") }
        let identity = try await console.command("identity show", endings: ["device_id:"], timeout: 10)
        guard let id = ProvisionParser.value("device_id", in: identity), let hostname = ProvisionParser.value("hostname", in: identity),
              id.range(of: "^[A-Za-z0-9_-]{1,64}$", options: .regularExpression) != nil,
              hostname.range(of: "^[a-z0-9](?:[a-z0-9-]{0,55}[a-z0-9])?$", options: .regularExpression) != nil else { throw AgentFailure("Cannot identify this MacControl device") }
        var record = try load(id) ?? PreparedEndpoint(deviceID: id, hostname: hostname, ip: "", firmwareVersion: version, ssid: credentials.ssid, adminPassword: credentials.adminPassword)
        record.hostname = hostname; record.firmwareVersion = version; record.ssid = credentials.ssid; record.adminPassword = credentials.adminPassword
        await progress("Setting the controller admin password…")
        let admin = try await console.command(credentials.command(wifi: false), endings: ["admin password set", "admin password NOT set", "invalid provisioning", "unknown command"], timeout: 20)
        guard admin.contains("admin password set") else { throw AgentFailure("Controller admin password was not saved") }
        try await save(record)
        // Key creation precedes Wi-Fi association to avoid the known fresh-board
        // Wi-Fi/key-create watchdog overlap. Persist each raw key immediately.
        let listing = try await console.command("key list", endings: ["(no keys)", "READ", "CONTROL", "ADMIN"], timeout: 10)
        for role in ["READ", "CONTROL", "ADMIN"] {
            try Task.checkCancellation()
            if let key = record.keys[role], listing.range(of: "(?m)^\\s*" + NSRegularExpression.escapedPattern(for: key.id) + "\\s+role=" + role + "\\s+[^\\r\\n]*active=yes\\b", options: .regularExpression) != nil { continue }
            await progress("Creating \(role) API key…")
            let response = try await console.command("key create \(role) native-setup", endings: ["created key-", "key store full", "usage:"], timeout: 20)
            if response.contains("key store full") { throw AgentFailure("The device key store is full. Revoke unused keys in the Controller, then retry without flashing.") }
            record.keys[role] = try ProvisionParser.key(response); try await save(record)
        }
        await step(.network)
        await progress(waitForWiFi ? "Saving Wi-Fi and waiting for the device to connect…" : "Saving Wi-Fi for the client's network…")
        let wifi = try await console.command(credentials.command(wifi: true), endings: ["wifi credentials saved", "persist failed", "invalid provisioning", "unknown command"], timeout: 20)
        guard wifi.contains("wifi credentials saved") else { throw AgentFailure("Wi-Fi credentials were not saved") }
        if !waitForWiFi {
            record.ip = ""
            try await save(record)
            return record
        }
        let until = ProcessInfo.processInfo.systemUptime + 90
        while ProcessInfo.processInfo.systemUptime < until {
            try Task.checkCancellation()
            let status = try await console.command("wifi status", endings: ["connected ip=", "not connected"], timeout: 10)
            if let ip = ProvisionParser.ip(status) { record.ip = ip; try await save(record); return record }
            try await Task.sleep(nanoseconds: 1_000_000_000)
        }
        throw AgentFailure("The ESP32 did not join Wi-Fi. Check the network name/password and 2.4 GHz availability; retry without flashing.")
    }
}
public actor ControllerSetupClient {
    private let base: URL
    private let session: URLSession
    private var cookie: String?
    public init(host: String, port: Int = 80) throws {
        var c = AgentConfiguration(); c.hostname = host; c.port = port; try c.validate()
        guard !host.isEmpty, let base = c.endpoint else { throw AgentFailure("Invalid controller address") }; self.base = base
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 6; configuration.timeoutIntervalForResource = 8
        configuration.httpCookieStorage = nil; configuration.urlCredentialStorage = nil
        session = URLSession(configuration: configuration)
    }
    private func request(_ path: String, method: String = "GET", body: [String: JSONValue]? = nil, key: String? = nil) async throws -> [String: JSONValue] {
        var request = URLRequest(url: base.appendingPathComponent(path)); request.httpMethod = method
        request.setValue("close", forHTTPHeaderField: "Connection")
        if let key { request.setValue("Bearer " + key, forHTTPHeaderField: "Authorization") }
        if let cookie { request.setValue(cookie, forHTTPHeaderField: "Cookie") }
        if let body { request.httpBody = try JSONEncoder().encode(body); request.setValue("application/json", forHTTPHeaderField: "Content-Type") }
        let (data, response) = try await session.data(for: request)
        guard data.count <= 65536, let http = response as? HTTPURLResponse, http.statusCode == 200 else { throw AgentFailure("Controller refused the setup request. Check network access and credentials.") }
        if let header = http.value(forHTTPHeaderField: "Set-Cookie"), let value = header.split(separator: ";").first, value.hasPrefix("mc_session=") { cookie = String(value) }
        return try JSONDecoder().decode([String: JSONValue].self, from: data)
    }
    public func verify(_ endpoint: PreparedEndpoint, timeout: Double = 90) async throws {
        guard let key = endpoint.keys["READ"]?.raw else { throw AgentFailure("READ key missing") }
        let until = ProcessInfo.processInfo.systemUptime + timeout
        while ProcessInfo.processInfo.systemUptime < until {
            try Task.checkCancellation()
            do {
                let response = try await request("api/v1/status", key: key)
                let identity = response["device"]?.object?["device_id"]
                guard (identity?.string ?? identity?.object?["value"]?.string) == endpoint.deviceID else { throw AgentFailure("The network address belongs to a different device", halted: true) }
                return
            } catch { if (error as? AgentFailure)?.halted == true { throw error } }
            try await Task.sleep(nanoseconds: 1_000_000_000)
        }
        throw AgentFailure("The controller is not reachable on this Mac's network. Check Wi-Fi, routing, and Local Network permission; retry network verification.")
    }
    public func pairingCode(adminPassword: String) async throws -> String {
        let login = try await request("ui/login", method: "POST", body: ["password": .string(adminPassword)])
        guard login["ok"]?.bool == true, cookie != nil else { throw AgentFailure("Controller login failed") }
        let window = try await request("api/v1/pairing/window", method: "POST", body: ["duration_s": .number(300)])
        guard let code = window["pairing_code"]?.string, code.count == 8 else { throw AgentFailure("Controller did not open a pairing window") }
        _ = try? await request("ui/logout", method: "POST"); cookie = nil
        return code
    }
    public func verifyAgent(instanceID: String, readKey: String, timeout: Double = 35) async throws {
        let until = ProcessInfo.processInfo.systemUptime + timeout
        while ProcessInfo.processInfo.systemUptime < until {
            try Task.checkCancellation()
            do {
                let report = try await request("api/v1/agent/status", key: readKey)
                if report["session_active"]?.bool == true && report["agent_instance_id"]?.string == instanceID { return }
            } catch { }
            try await Task.sleep(nanoseconds: 1_000_000_000)
        }
        throw AgentFailure("Pairing is saved; the controller has not confirmed an active agent session. Check the connection before repeating pairing.")
    }
}
