import Foundation
import Security
import Darwin

public enum NativeIdentity {
    public static let app = "dev.maccontrol.native"
    public static let service = "dev.maccontrol.native.agent"
    public static func currentExecutableURL() throws -> URL {
        // SMAppService can supply a bundle-relative argv[0]. Ask the kernel
        // for the running image instead of resolving argv against launchd's cwd.
        var path = [CChar](repeating: 0, count: 4 * Int(MAXPATHLEN))
        let count = path.withUnsafeMutableBytes {
            proc_pidpath(getpid(), $0.baseAddress, UInt32($0.count))
        }
        guard count > 0 else { throw AgentFailure("Cannot locate the running executable") }
        return URL(fileURLWithPath: String(cString: path)).resolvingSymlinksInPath()
    }
    // Pin the exact peer code in local ad-hoc builds; Developer ID builds use
    // the bundle identifier + team certificate to survive signed upgrades.
    public static func requirement(for executable: URL, identifier: String) throws -> String {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(executable as CFURL, [], &code) == errSecSuccess, let code else { throw AgentFailure("Peer executable has no code signature") }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let d = info as? [String: Any] else { throw AgentFailure("Cannot inspect peer signature") }
        if let team = d[kSecCodeInfoTeamIdentifier as String] as? String, !team.isEmpty {
            return "identifier \"\(identifier)\" and anchor apple generic and certificate leaf[subject.OU] = \"\(team)\""
        }
        guard let digest = d[kSecCodeInfoUnique as String] as? Data else { throw AgentFailure("Peer signature has no code hash") }
        return "identifier \"\(identifier)\" and cdhash H\"\(digest.map { String(format: "%02x", $0) }.joined())\""
    }
}
@objc public protocol AgentIPC {
    func snapshot(_ reply: @escaping (Data) -> Void)
    func perform(_ operation: String, payload: Data, reply: @escaping (Data?, String?) -> Void)
}

public enum TokenStore {
    public static func read(_ id: String) throws -> String? {
        var q = query(id); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let rc = SecItemCopyMatching(q as CFDictionary, &result)
        if rc == errSecItemNotFound { return nil }
        guard rc == errSecSuccess, let data = result as? Data, let token = String(data: data, encoding: .utf8) else { throw AgentFailure("Keychain access failed (\(rc))") }
        return token
    }
    public static func write(_ token: String, id: String) throws {
        let q = query(id), changes = [kSecValueData as String: Data(token.utf8)]
        var rc = SecItemUpdate(q as CFDictionary, changes as CFDictionary)
        if rc == errSecItemNotFound { var add = q; add[kSecValueData as String] = Data(token.utf8); rc = SecItemAdd(add as CFDictionary, nil) }
        guard rc == errSecSuccess else { throw AgentFailure("Cannot store pairing credential (\(rc))") }
    }
    public static func remove(_ id: String) { SecItemDelete(query(id) as CFDictionary) }
    private static func query(_ id: String) -> [String: Any] { [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: NativeIdentity.service, kSecAttrAccount as String: id] }
}
