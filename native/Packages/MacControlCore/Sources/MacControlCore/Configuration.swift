import Foundation

public struct AgentConfiguration: Codable, Equatable, Sendable {
    public var schemaVersion = 1
    public var hostname = ""
    public var port = 80
    public var instanceID = "ag-" + UUID().uuidString.prefix(8).lowercased()
    public var allowlist: [String] = []
    public var enabledCommands: [String: Bool] = Dictionary(uniqueKeysWithValues: AgentAction.allCases.map { ($0.rawValue, false) })
    public var transport = "websocket"
    public var pollInterval = 5
    public var enabled = false
    public var credentialID = UUID().uuidString
    public var legacyStatePath: String?
    public var legacyBackupPath: String?
    public var legacyDigest: String?
    public var importedBootID: String?
    public var importedBootKey: Int64?
    public init() {}
    public var endpoint: URL? { var c = URLComponents(); c.scheme = "http"; c.host = hostname; c.port = port; return c.url }
    public func validate() throws {
        guard schemaVersion == 1 else { throw AgentFailure("Unsupported configuration version") }
        guard hostname.isEmpty || (hostname.count <= 253 && hostname.rangeOfCharacter(from: CharacterSet(charactersIn: "/\\@?# \n\r\t")) == nil),
              (1...65535).contains(port), !instanceID.isEmpty, instanceID.count <= 64,
              ["websocket", "polling"].contains(transport), (2...30).contains(pollInterval),
              allowlist.count <= 24, Set(allowlist).count == allowlist.count,
              allowlist.allSatisfy(Dispatch.validBundleID), Set(enabledCommands.keys).isSubset(of: Set(AgentAction.allCases.map(\.rawValue))) else { throw AgentFailure("Invalid endpoint, transport, interval or allowlist (maximum 24 applications)") }
        // Reserve room for the envelope and OS metadata in the 4 KB report.
        let apps = allowlist.map { ["bundle_id": $0, "state": "not_running"] }
        guard try JSONEncoder().encode(apps).count <= 2800 else { throw AgentFailure("Allowlist is too large for the firmware's 4 KB capability report") }
    }
    public static func importLegacy(_ data: Data) throws -> (AgentConfiguration, String) {
        guard data.count <= 1_048_576 else { throw AgentFailure("Legacy configuration is too large") }
        let d = try JSONDecoder().decode([String: JSONValue].self, from: data)
        guard d["schema_version"]?.number == 1, let host = d["hostname"]?.string,
              let id = d["agent_instance_id"]?.string, let token = d["agent_token"]?.string, !token.isEmpty else { throw AgentFailure("A valid paired Python configuration is required") }
        var c = AgentConfiguration(); c.hostname = host; c.instanceID = id
        let allow = d["allowlist"]?.array ?? []
        guard allow.allSatisfy({ $0.string != nil }) else { throw AgentFailure("Invalid legacy allowlist") }
        c.allowlist = allow.compactMap(\.string)
        let flags = d["enabled_commands"]?.object ?? [:]
        for a in AgentAction.allCases { c.enabledCommands[a.rawValue] = flags[a.rawValue]?.bool ?? false }
        c.transport = d["transport"]?.string ?? "websocket"
        let interval = d["poll_interval_s"]?.number ?? 5
        guard let pollInterval = Int(exactly: interval), (2...30).contains(pollInterval) else { throw AgentFailure("Invalid legacy polling interval") }
        c.pollInterval = pollInterval
        c.importedBootID = d["boot_id"]?.string
        if let key = d["boot_key"]?.number {
            guard let bootKey = Int64(exactly: key), bootKey > 0 else { throw AgentFailure("Invalid legacy boot identity") }
            c.importedBootKey = bootKey
        }
        try c.validate(); return (c, token)
    }
}

public struct LogEntry: Codable, Sendable, Identifiable {
    public let id: UUID
    public let at: Date
    public let message: String
    public init(_ message: String) { id = UUID(); at = Date(); self.message = String(message.prefix(1000)) }
}
public struct AgentSnapshot: Codable, Sendable {
    public var configuration = AgentConfiguration()
    public var phase = "stopped"
    public var detail = "Start the background service to configure this Mac."
    public var paired = false
    public var sessionID: String?
    public var lastHeartbeat: Date?
    public var lastAction: String?
    public var runningApps: [String: Int32] = [:]
    public var screenLocked: Bool?
    public var logs: [LogEntry] = []
    public var canRestorePython = false
    public init() {}
}
public enum NativePaths {
    public static var root: URL {
        #if DEBUG
        if let path = ProcessInfo.processInfo.environment["MACCONTROL_TEST_ROOT"] { return URL(fileURLWithPath: path, isDirectory: true) }
        #endif
        return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/MacControlNative", isDirectory: true) }
    public static var configuration: URL { root.appendingPathComponent("configuration.json") }
    public static var legacyPlist: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/com.maccontrol.agent.plist") }
    public static func prepare(_ directory: URL) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
    }
    public static func save<T: Encodable>(_ value: T, to url: URL) throws {
        try prepare(url.deletingLastPathComponent())
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(value).write(to: url, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }
    public static func load(from url: URL = configuration) throws -> AgentConfiguration {
        guard FileManager.default.fileExists(atPath: url.path) else { return AgentConfiguration() }
        let c = try JSONDecoder().decode(AgentConfiguration.self, from: Data(contentsOf: url)); try c.validate(); return c
    }
}
