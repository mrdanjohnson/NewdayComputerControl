import Foundation

public enum JSONValue: Codable, Equatable, Sendable {
    case string(String), number(Double), bool(Bool), array([JSONValue]), object([String: JSONValue]), null
    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let v = try? c.decode(Bool.self) { self = .bool(v) }
        else if let v = try? c.decode(Double.self) { self = .number(v) }
        else if let v = try? c.decode(String.self) { self = .string(v) }
        else if let v = try? c.decode([JSONValue].self) { self = .array(v) }
        else { self = .object(try c.decode([String: JSONValue].self)) }
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .string(let v): try c.encode(v)
        case .number(let v): try c.encode(v)
        case .bool(let v): try c.encode(v)
        case .array(let v): try c.encode(v)
        case .object(let v): try c.encode(v)
        case .null: try c.encodeNil()
        }
    }
    public var string: String? { if case .string(let v) = self { return v }; return nil }
    public var object: [String: JSONValue]? { if case .object(let v) = self { return v }; return nil }
    public var array: [JSONValue]? { if case .array(let v) = self { return v }; return nil }
    public var number: Double? { if case .number(let v) = self { return v }; return nil }
    public var bool: Bool? { if case .bool(let v) = self { return v }; return nil }
    public static func optional(_ value: String?) -> Self { value.map(Self.string) ?? .null }
    public static func optional(_ value: Double?) -> Self { value.map(Self.number) ?? .null }
}

public enum AgentAction: String, Codable, CaseIterable, Sendable, Identifiable {
    case launch = "launch_app", quit = "quit_app", sleep, restart, shutdown
    public var id: String { rawValue }
    public var isPower: Bool { self != .launch && self != .quit }
    public var title: String { switch self { case .launch: return "Launch applications"; case .quit: return "Quit applications"; case .sleep: return "Sleep"; case .restart: return "Restart"; case .shutdown: return "Shut down" } }
}
public enum EventType: String, Codable, Sendable {
    case hello = "agent_hello", goodbye = "agent_goodbye", heartbeat
    case system = "system_state_changed", user = "user_session_changed", lock = "screen_lock_changed"
    case started = "application_started", exited = "application_exited", ack = "command_ack"
    case result = "command_result", capabilities = "capability_report", front = "front_app_changed"
}
public struct AgentFailure: Error, LocalizedError, Sendable {
    public let message: String
    public let halted: Bool
    public init(_ message: String, halted: Bool = false) { self.message = message; self.halted = halted }
    public var errorDescription: String? { message }
}
public struct Dispatch: Equatable, Sendable {
    public var commandID: String
    public var action: AgentAction
    public var bundleID: String?
    public init(_ object: [String: JSONValue]) throws {
        guard let id = object["command_id"]?.string, !id.isEmpty, id.count <= 128,
              let action = object["action"]?.string.flatMap(AgentAction.init(rawValue:)) else { throw AgentFailure("Malformed dispatch") }
        let keys = Set(object.keys)
        if action.isPower {
            guard keys == ["action", "command_id"] else { throw AgentFailure("Power dispatch has unexpected fields") }
            bundleID = nil
        } else {
            guard keys == ["action", "command_id", "bundle_id"], let bid = object["bundle_id"]?.string, Self.validBundleID(bid) else { throw AgentFailure("Invalid app dispatch") }
            bundleID = bid
        }
        commandID = id; self.action = action
    }
    public static func validBundleID(_ value: String) -> Bool {
        !value.isEmpty && value.count <= 255 && value.range(of: "^[A-Za-z0-9.\\-]+$", options: .regularExpression) != nil
    }
    public func rejection(configuration: AgentConfiguration) -> String? {
        if configuration.enabledCommands[action.rawValue] != true { return "command_disabled" }
        if let bundleID, !configuration.allowlist.contains(bundleID) { return "app_not_allowlisted" }
        return nil
    }
}
public struct Event: Codable, Sendable {
    public let event_id: String, agent_instance_id: String
    public let session_id: String?
    public let seq: Int
    public let timestamp: String
    public let type: EventType
    public let command_id: String?
    public let payload: [String: JSONValue]
    public func encode() throws -> Data {
        // Explicit nulls match the Python envelope; synthesized Codable omits optional nil.
        let value: [String: JSONValue] = ["event_id": .string(event_id), "agent_instance_id": .string(agent_instance_id), "session_id": .optional(session_id), "seq": .number(Double(seq)), "timestamp": .string(timestamp), "type": .string(type.rawValue), "command_id": .optional(command_id), "payload": .object(payload)]
        let bytes = try JSONEncoder().encode(value)
        guard bytes.count <= 4096 else { throw AgentFailure("Event exceeds 4 KB") }
        return bytes
    }
}
public struct EventFactory: Sendable {
    public static let version = "2.0.0-native"
    public var instanceID: String
    public private(set) var sessionID: String?
    public private(set) var sequence = 0
    private var eventNumber = 0
    public init(instanceID: String) { self.instanceID = instanceID }
    public mutating func begin(_ id: String) { sessionID = id }
    public mutating func end() { sessionID = nil; sequence = 0 }
    public mutating func event(_ type: EventType, payload: [String: JSONValue], commandID: String? = nil, at date: Date = Date()) throws -> Event {
        guard sessionID != nil || type == .hello else { throw AgentFailure("No active session") }
        let next = sequence + 1
        let e = Event(event_id: "evt_\(eventNumber + 1)", agent_instance_id: instanceID, session_id: sessionID, seq: next, timestamp: ISO8601DateFormatter().string(from: date), type: type, command_id: commandID, payload: payload)
        _ = try e.encode(); sequence = next; eventNumber += 1
        return e
    }
    public static func helloAck(_ object: [String: JSONValue], websocket: Bool) throws -> String {
        guard let id = object["session_id"]?.string, !id.isEmpty else { throw AgentFailure("Hello acknowledgment has no session ID") }
        if websocket {
            guard object["type"]?.string == "hello_ack", object["protocol_version"]?.number == 2 else { throw AgentFailure("ESP32 agent protocol v2 required", halted: true) }
        }
        return id
    }
    public static func backoff(_ attempt: Int, jitter: Double = Double.random(in: 0...1)) -> Double {
        let base: Double = attempt <= 1 ? 1 : attempt == 2 ? 2 : attempt == 3 ? 4 : attempt == 4 ? 8 : 30
        return base + base * 0.2 * min(1, max(0, jitter))
    }
    public static func bootID(seconds: Int64) -> String { String(format: "b_%06llX", seconds & 0xFFFFFF) }
}
