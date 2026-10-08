import Foundation

public protocol AgentTransport: Sendable {
    func open(hello: Data) async throws -> [String: JSONValue]
    func send(_ data: Data, sessionID: String) async throws
    func receive() async throws -> [String: JSONValue]
    func close() async
}
public actor EndpointTransport: AgentTransport {
    private let config: AgentConfiguration
    private let token: String
    private let session: URLSession
    private var socket: URLSessionWebSocketTask?
    private var sessionID: String?
    private var pending: [[String: JSONValue]] = []
    private let heartbeatProbeTimeout: Double
    public init(configuration: AgentConfiguration, token: String, heartbeatProbeTimeout: Double = 5) {
        config = configuration; self.token = token; self.heartbeatProbeTimeout = max(0.05, heartbeatProbeTimeout)
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 10; c.timeoutIntervalForResource = 12
        c.httpMaximumConnectionsPerHost = 2; c.httpCookieStorage = nil; c.urlCredentialStorage = nil
        session = URLSession(configuration: c)
    }
    private func request(_ path: String, method: String, body: Data? = nil, sid: String? = nil) throws -> URLRequest {
        guard var c = config.endpoint.flatMap({ URLComponents(url: $0, resolvingAgainstBaseURL: false) }) else { throw AgentFailure("Invalid endpoint") }
        c.path = "/agent/v1" + path
        if path == "/ws" { c.scheme = "ws" }
        guard let url = c.url else { throw AgentFailure("Invalid endpoint URL") }
        var req = URLRequest(url: url); req.httpMethod = method; req.httpBody = body
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        if body != nil { req.setValue("application/json", forHTTPHeaderField: "Content-Type") }
        if let sid { req.setValue(sid, forHTTPHeaderField: "X-Session-Id") }
        return req
    }
    private func http(_ request: URLRequest) async throws -> [String: JSONValue] {
        let (bytes, response) = try await session.data(for: request)
        guard bytes.count <= 65536 else { throw AgentFailure("Endpoint response exceeds 64 KB") }
        guard let response = response as? HTTPURLResponse else { throw AgentFailure("Invalid HTTP response") }
        let object = (try? JSONDecoder().decode([String: JSONValue].self, from: bytes)) ?? [:]
        guard response.statusCode == 200 else {
            // Never include remote message, code or body in diagnostic text.
            throw AgentFailure("HTTP \(response.statusCode): endpoint refused the request", halted: response.statusCode == 403)
        }
        return object
    }
    public func open(hello: Data) async throws -> [String: JSONValue] {
        if config.transport == "polling" {
            let ack = try await http(request("/events", method: "POST", body: hello))
            sessionID = try EventFactory.helloAck(ack, websocket: false); return ack
        }
        let task = session.webSocketTask(with: try request("/ws", method: "GET"))
        task.maximumMessageSize = 4096; socket = task; task.resume()
        // Explicit cancellation gives hello/upgrade a deadline even if receive hangs.
        let deadline = Task { try await Task.sleep(nanoseconds: 5_000_000_000); task.cancel(with: .goingAway, reason: nil) }
        defer { deadline.cancel() }
        do {
            try await task.send(.data(hello))
            let ack = try await receiveSocket(task)
            sessionID = try EventFactory.helloAck(ack, websocket: true); return ack
        } catch { throw socketError(task, error: error) }
    }
    private func receiveSocket(_ task: URLSessionWebSocketTask) async throws -> [String: JSONValue] {
        let data: Data
        switch try await task.receive() { case .data(let b): data = b; case .string(let s): data = Data(s.utf8); @unknown default: throw AgentFailure("Unknown WebSocket frame") }
        guard data.count <= 4096 else { throw AgentFailure("WebSocket frame exceeds 4 KB") }
        return try JSONDecoder().decode([String: JSONValue].self, from: data)
    }
    private func socketError(_ task: URLSessionWebSocketTask, error: Error) -> Error {
        let code = task.closeCode.rawValue
        if [1008, 4001, 4003].contains(code) { return AgentFailure("ESP32 closed the agent channel (\(code)); repair pairing or protocol", halted: true) }
        if code != 0 { return AgentFailure("WebSocket closed (\(code))") }
        return error
    }
    public func send(_ data: Data, sessionID: String) async throws {
        if config.transport == "polling" { _ = try await http(request("/events", method: "POST", body: data, sid: sessionID)) }
        else {
            guard let socket else { throw AgentFailure("WebSocket not open") }
            do {
                try await socket.send(.data(data))
                if (try? JSONDecoder().decode([String: JSONValue].self, from: data)["type"]?.string) == EventType.heartbeat.rawValue {
                    try await confirmHeartbeat(socket)
                }
            } catch { throw socketError(socket, error: error) }
        }
    }
    private func confirmHeartbeat(_ socket: URLSessionWebSocketTask) async throws {
        // Foundation does not schedule Python websockets' automatic keepalive.
        // A bounded ping detects a black-holed connection even while local
        // writes appear successful. Pong is transport liveness, not a verdict.
        try await withCheckedThrowingContinuation { (reply: CheckedContinuation<Void, Error>) in
            let once = PingReply(reply), timeout = heartbeatProbeTimeout
            let deadline = Task {
                try await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                once.finish(.failure(AgentFailure("WebSocket heartbeat probe timed out")))
                socket.cancel(with: .goingAway, reason: nil)
            }
            socket.sendPing { error in
                deadline.cancel()
                if let error { once.finish(.failure(error)) } else { once.finish(.success(())) }
            }
        }
    }
    public func receive() async throws -> [String: JSONValue] {
        if let socket {
            do { return try await receiveSocket(socket) } catch { throw socketError(socket, error: error) }
        }
        while pending.isEmpty {
            try Task.checkCancellation()
            let response = try await http(request("/commands/pending", method: "GET", sid: sessionID))
            pending = (response["commands"]?.array ?? []).compactMap(\.object)
            if pending.isEmpty { try await Task.sleep(nanoseconds: UInt64(config.pollInterval) * 1_000_000_000) }
        }
        return pending.removeFirst()
    }
    public func close() async {
        socket?.cancel(with: .normalClosure, reason: nil); socket = nil
        session.invalidateAndCancel(); pending.removeAll(); sessionID = nil
    }
    public static func pair(configuration: AgentConfiguration, code: String) async throws -> (String, String) {
        guard code.count == 8, code.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber) }) else { throw AgentFailure("Enter the eight-character pairing code") }
        let client = EndpointTransport(configuration: configuration, token: "")
        let bytes = try JSONEncoder().encode(["pairing_code": code, "agent_instance_id": configuration.instanceID, "agent_version": EventFactory.version])
        let response = try await client.http(client.request("/pair", method: "POST", body: bytes))
        await client.close()
        guard let token = response["agent_token"]?.string, !token.isEmpty else { throw AgentFailure("Pairing response has no credential") }
        return (token, response["agent_instance_id"]?.string ?? configuration.instanceID)
    }
}

private final class PingReply: @unchecked Sendable {
    private let lock = NSLock()
    private var reply: CheckedContinuation<Void, Error>?
    init(_ reply: CheckedContinuation<Void, Error>) { self.reply = reply }
    func finish(_ result: Result<Void, Error>) { lock.lock(); let c = reply; reply = nil; lock.unlock(); c?.resume(with: result) }
}
