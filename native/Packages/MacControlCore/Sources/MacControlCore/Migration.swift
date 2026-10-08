import Foundation
import CryptoKit

public enum Migration {
    public static func stage(from path: String, root: URL = NativePaths.root) throws -> (AgentConfiguration, String) {
        let url = URL(fileURLWithPath: path)
        let bytes = try Data(contentsOf: url, options: .mappedIfSafe)
        var (configuration, token) = try AgentConfiguration.importLegacy(bytes)
        let directory = root.appendingPathComponent("Legacy Backups", isDirectory: true)
        try NativePaths.prepare(directory)
        let backup = directory.appendingPathComponent("agent-\(UUID().uuidString).json")
        try bytes.write(to: backup, options: .withoutOverwriting)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: backup.path)
        configuration.legacyStatePath = url.path; configuration.legacyBackupPath = backup.path
        configuration.legacyDigest = SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
        return (configuration, token)
    }
    public static func validateOriginal(_ c: AgentConfiguration) throws {
        guard let path = c.legacyStatePath, let digest = c.legacyDigest else { throw AgentFailure("Import a Python configuration before cutover") }
        let bytes = try Data(contentsOf: URL(fileURLWithPath: path))
        guard SHA256.hash(data: bytes).map({ String(format: "%02x", $0) }).joined() == digest else { throw AgentFailure("Python configuration changed since import. Import it again before cutover.") }
    }
    public static func settingsDiffer(_ c: AgentConfiguration) throws -> Bool {
        guard let path = c.legacyStatePath else { return false }
        let (old, _) = try AgentConfiguration.importLegacy(Data(contentsOf: URL(fileURLWithPath: path)))
        return old.hostname != c.hostname || old.allowlist != c.allowlist || old.enabledCommands != c.enabledCommands || old.transport != c.transport || old.pollInterval != c.pollInterval
    }
}
public enum LegacyAgent {
    public static func active() async -> Bool {
        // A loaded KeepAlive service can respawn; blocking only a momentary PID
        // would allow two agents to steal the same ESP32 session.
        if await FixedProcess.run("/bin/launchctl", ["print", "gui/\(getuid())/com.maccontrol.agent"]) == 0 { return true }
        // Also catch an agent started manually outside launchd; never kill it.
        return await FixedProcess.run("/usr/bin/pgrep", ["-f", "[pP]ython[^\n]* -m maccontrol_agent"]) == 0
    }
    public static func unload() async throws {
        let service = "gui/\(getuid())/com.maccontrol.agent"
        if await FixedProcess.run("/bin/launchctl", ["print", service]) == 0 {
            guard await FixedProcess.run("/bin/launchctl", ["bootout", service]) == 0 else { throw AgentFailure("Could not stop the Python LaunchAgent") }
        }
        guard !(await active()) else { throw AgentFailure("A manually started Python agent is still active. Stop it before cutover.") }
    }
    public static func restore() async throws {
        guard FileManager.default.fileExists(atPath: NativePaths.legacyPlist.path) else { throw AgentFailure("Python LaunchAgent plist is absent. Native agent is stopped; restart Python with its existing installer/configuration.") }
        if await FixedProcess.run("/bin/launchctl", ["print", "gui/\(getuid())/com.maccontrol.agent"]) == 0 { return }
        guard await FixedProcess.run("/bin/launchctl", ["bootstrap", "gui/\(getuid())", NativePaths.legacyPlist.path]) == 0 else { throw AgentFailure("Could not restore the Python LaunchAgent; native agent remains stopped") }
    }
}
