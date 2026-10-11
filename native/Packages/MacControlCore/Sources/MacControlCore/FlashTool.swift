import Foundation
import Security
import Darwin

public struct FlashTool: Sendable {
    public let executable: URL
    public init(resources: URL) throws {
        #if arch(arm64)
        let architecture = "arm64"
        #else
        let architecture = "x86_64"
        #endif
        executable = resources.appendingPathComponent("ESPTools/" + architecture + "/esptool")
        var code: SecStaticCode?
        guard FileManager.default.isExecutableFile(atPath: executable.path),
              SecStaticCodeCreateWithPath(executable as CFURL, [], &code) == errSecSuccess, let code,
              SecStaticCodeCheckValidity(code, [], nil) == errSecSuccess else { throw AgentFailure("Bundled flashing tool is missing or its signature is invalid. Reinstall the app.") }
    }
    public func flash(firmware: FirmwareBundle, port: String, progress: @escaping @Sendable (Double?) async -> Void) async throws {
        guard SerialPorts.valid(port) else { throw AgentFailure("Choose the ESP32 COM serial port") }
        let common = ["--chip", "esp32s3", "--port", port, "--before", "default-reset", "--after", "hard-reset"]
        let identity = try await ToolProcess.run(executable, arguments: common + ["flash-id"], timeout: 45)
        try Self.validateFlashCapacity(identity)
        let output = try await ToolProcess.run(executable, arguments: firmware.flashArguments(port: port), timeout: 600, progress: progress)
        guard output.contains("Hash of data verified") else { throw AgentFailure("Flashing ended without firmware verification. Do not deploy this board; retry flashing.") }
    }
    static func validateFlashCapacity(_ output: String) throws {
        let sizes = output.split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { $0.hasPrefix("Detected flash size:") }
        // Both capacities use the bundled 8 MB layout; the extra space on
        // a 16 MB board remains unused. Refuse unknown or ambiguous capacity.
        guard sizes.count == 1,
              ["Detected flash size: 8MB", "Detected flash size: 16MB"].contains(sizes[0]) else {
            throw AgentFailure("Choose an ESP32-S3 with 8 MB or 16 MB flash. Its flash size must be detected before flashing.")
        }
    }
}
public enum ToolProcess {
    public static func run(_ executable: URL, arguments: [String], timeout: Double,
                           progress: @escaping @Sendable (Double?) async -> Void = { _ in }) async throws -> String {
        let process = Process(), output = Pipe(), capture = ToolCapture()
        process.executableURL = executable; process.arguments = arguments
        process.standardOutput = output; process.standardError = output
        process.environment = ProcessInfo.processInfo.environment.merging(["NO_COLOR": "1", "TERM": "dumb"]) { _, new in new }
        output.fileHandleForReading.readabilityHandler = { handle in capture.append(handle.availableData) }
        defer { output.fileHandleForReading.readabilityHandler = nil; try? output.fileHandleForReading.close() }
        do { try process.run() } catch { throw AgentFailure("Cannot start the bundled flashing tool") }
        do {
            let until = ProcessInfo.processInfo.systemUptime + timeout
            var previous: Double?
            while process.isRunning {
                try Task.checkCancellation()
                guard ProcessInfo.processInfo.systemUptime < until else { throw AgentFailure("The flashing tool timed out. Check the COM cable and board power.") }
                let text = capture.text()
                if let range = text.range(of: "[0-9]{1,3}(?:\\.[0-9]+)?%", options: [.regularExpression, .backwards]), let value = Double(text[range].dropLast()) {
                    let percent = min(1, max(0, value / 100))
                    if percent != previous { previous = percent; await progress(percent) }
                }
                try await Task.sleep(nanoseconds: 100_000_000)
            }
            // Wait for EOF, rather than assuming a final callback has arrived.
            let drainDeadline = ProcessInfo.processInfo.systemUptime + 2
            while !capture.finished {
                try Task.checkCancellation()
                guard ProcessInfo.processInfo.systemUptime < drainDeadline else { throw AgentFailure("Flashing tool output did not finish") }
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            guard process.terminationStatus == 0 else { throw AgentFailure("Flashing tool failed. Check the selected COM port, close serial terminals, and use BOOT + RESET if automatic download mode fails.") }
            return capture.text()
        } catch {
            if process.isRunning { process.terminate(); try? await Task.sleep(nanoseconds: 300_000_000); if process.isRunning { kill(process.processIdentifier, SIGKILL) } }
            throw error
        }
    }
}
private final class ToolCapture: @unchecked Sendable {
    private let lock = NSLock(); private var data = Data(); private var eof = false
    var finished: Bool { lock.lock(); defer { lock.unlock() }; return eof }
    func append(_ chunk: Data) { lock.lock(); if chunk.isEmpty { eof = true }; data.append(chunk); if data.count > 131072 { data.removeFirst(data.count - 131072) }; lock.unlock() }
    func text() -> String { lock.lock(); let bytes = data; lock.unlock(); return String(decoding: bytes, as: UTF8.self).replacingOccurrences(of: "\u{001B}\\[[0-9;]*[A-Za-z]", with: "", options: .regularExpression) }
}
