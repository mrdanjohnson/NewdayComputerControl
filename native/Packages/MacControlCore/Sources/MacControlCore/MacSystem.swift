import Foundation
import AppKit
import CoreGraphics
import IOKit
import IOKit.pwr_mgt
import SystemConfiguration
import Darwin

public struct SystemSample: Sendable {
    public var apps: [String: Int32]
    public var locked: Bool?
    public var loggedIn: Bool?
    public var user: String?
    public var front: String?
    public var bootTime: Int64
    public var hardwareModel: String
    public var heartbeat: [String: JSONValue]
    public init(apps: [String: Int32] = [:], locked: Bool? = nil, loggedIn: Bool? = nil, user: String? = nil, front: String? = nil, bootTime: Int64 = 0, hardwareModel: String = "unknown", heartbeat: [String: JSONValue] = [:]) {
        self.apps = apps; self.locked = locked; self.loggedIn = loggedIn; self.user = user; self.front = front; self.bootTime = bootTime; self.hardwareModel = hardwareModel; self.heartbeat = heartbeat
    }
}
public protocol MacExecutor: Sendable {
    func sample(allowlist: [String]) async -> SystemSample
    func execute(_ dispatch: Dispatch) async -> String?
}
@MainActor public final class NativeMacSystem: MacExecutor {
    private var previousCPU: [UInt32]?
    private var notifiedLock: Bool?
    private var observers: [NSObjectProtocol] = []
    public init() {
        let c = DistributedNotificationCenter.default()
        for (name, locked) in [("com.apple.screenIsLocked", true), ("com.apple.screenIsUnlocked", false)] {
            observers.append(c.addObserver(forName: .init(name), object: nil, queue: .main) { [weak self] _ in Task { @MainActor in self?.notifiedLock = locked } })
        }
    }
    public func sample(allowlist: [String]) async -> SystemSample {
        let d = CGSessionCopyCurrentDictionary() as? [String: Any]
        // This legacy key/notification pair is best effort, not a public lock API.
        // A missing key is unknown; never fabricate an unlocked observation.
        let locked = (d?["CGSSessionScreenIsLocked"] as? NSNumber)?.boolValue ?? notifiedLock
        var apps: [String: Int32] = [:]
        for app in NSWorkspace.shared.runningApplications {
            if let bid = app.bundleIdentifier, allowlist.contains(bid), !app.isTerminated { apps[bid] = app.processIdentifier }
        }
        var time = timeval(); var size = MemoryLayout<timeval>.size
        let boot = sysctlbyname("kern.boottime", &time, &size, nil, 0) == 0 ? Int64(time.tv_sec) : 0
        var modelSize = 0; sysctlbyname("hw.model", nil, &modelSize, nil, 0)
        var model = [CChar](repeating: 0, count: max(1, modelSize)); sysctlbyname("hw.model", &model, &modelSize, nil, 0)
        var load = host_cpu_load_info(); var count = mach_msg_type_number_t(MemoryLayout<host_cpu_load_info>.size / MemoryLayout<integer_t>.size)
        let rc = withUnsafeMutablePointer(to: &load) { p in p.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, $0, &count) } }
        let ticks = [load.cpu_ticks.0, load.cpu_ticks.1, load.cpu_ticks.2, load.cpu_ticks.3]
        var cpu: Double?
        if rc == KERN_SUCCESS, let old = previousCPU {
            let deltas = zip(ticks, old).map { Double($0 &- $1) }; let total = deltas.reduce(0, +)
            if total > 0 { cpu = (total - deltas[2]) / total * 100 }
        }
        if rc == KERN_SUCCESS { previousCPU = ticks }
        var vm = vm_statistics64(); var vmCount = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
        let vmRC = withUnsafeMutablePointer(to: &vm) { p in p.withMemoryRebound(to: integer_t.self, capacity: Int(vmCount)) { host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &vmCount) } }
        let used = Double(vm.active_count) + Double(vm.wire_count) + Double(vm.compressor_page_count)
        let memory: Double? = vmRC == KERN_SUCCESS ? min(100, used * Double(vm_kernel_page_size) / Double(ProcessInfo.processInfo.physicalMemory) * 100) : nil
        let disk = (try? FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory())[.systemFreeSize] as? NSNumber)?.doubleValue
        var reachable: JSONValue = .bool(false), ip: JSONValue = .null
        if let store = SCDynamicStoreCreate(nil, "MacControl" as CFString, nil, nil), let global = SCDynamicStoreCopyValue(store, "State:/Network/Global/IPv4" as CFString) as? [String: Any], let interface = global["PrimaryInterface"] as? String {
            reachable = .bool(true)
            if let v = SCDynamicStoreCopyValue(store, "State:/Network/Interface/\(interface)/IPv4" as CFString) as? [String: Any], let addresses = v["Addresses"] as? [String], let address = addresses.first { ip = .string(address) }
        }
        return SystemSample(apps: apps, locked: locked, loggedIn: (d?[kCGSessionLoginDoneKey as String] as? NSNumber)?.boolValue, user: d?[kCGSessionUserNameKey as String] as? String, front: NSWorkspace.shared.frontmostApplication?.bundleIdentifier, bootTime: boot, hardwareModel: String(cString: model), heartbeat: ["boot_time": boot > 0 ? .number(Double(boot)) : .null, "mac_uptime_s": boot > 0 ? .number(max(0, Date().timeIntervalSince1970 - Double(boot)).rounded(.down)) : .null, "cpu_utilization_pct": .optional(cpu), "memory_utilization_pct": .optional(memory), "disk_free_bytes": .optional(disk), "network": .object(["reachable": reachable, "ip": ip])])
    }
    public func execute(_ dispatch: Dispatch) async -> String? {
        if Task.isCancelled { return "action_timeout" }
        if let bid = dispatch.bundleID {
            if dispatch.action == .launch {
                guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bid) else { return "launch_failed" }
                return await withCheckedContinuation { reply in
                    let once = LaunchReply(reply)
                    let config = NSWorkspace.OpenConfiguration(); config.activates = true
                    NSWorkspace.shared.openApplication(at: url, configuration: config) { _, error in once.finish(error == nil ? nil : "launch_failed") }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 10) { once.finish("action_timeout") }
                }
            }
            let running = NSRunningApplication.runningApplications(withBundleIdentifier: bid)
            guard !running.isEmpty else { return "app_not_running" }
            guard running.allSatisfy({ $0.terminate() }) else { return "quit_failed" }
            return nil // Only a request; the runtime waits for actual disappearance.
        }
        if dispatch.action == .sleep { _ = await FixedProcess.run("/usr/bin/pmset", ["displaysleepnow"], timeout: 3) }
        let result: Int32?
        switch dispatch.action {
        case .sleep: result = await FixedProcess.run("/usr/bin/pmset", ["sleepnow"], timeout: 10)
        case .restart: result = await FixedProcess.run("/usr/bin/osascript", ["-e", "tell application \"System Events\" to restart"], timeout: 10)
        case .shutdown: result = await FixedProcess.run("/usr/bin/osascript", ["-e", "tell application \"System Events\" to shut down"], timeout: 10)
        default: return "command_disabled"
        }
        return result == 0 ? nil : "action_timeout"
    }
}
public enum FixedProcess {
    // Callers supply only locally fixed programs/arguments; never use a shell.
    public static func run(_ executable: String, _ arguments: [String], timeout: Double = 5) async -> Int32? {
        await Task.detached {
            let p = Process(); p.executableURL = URL(fileURLWithPath: executable); p.arguments = arguments
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            do { try p.run() } catch { return nil }
            let until = ProcessInfo.processInfo.systemUptime + timeout
            while p.isRunning && ProcessInfo.processInfo.systemUptime < until { try? await Task.sleep(nanoseconds: 30_000_000) }
            if p.isRunning { p.terminate(); try? await Task.sleep(nanoseconds: 100_000_000); if p.isRunning { kill(p.processIdentifier, SIGKILL) }; p.waitUntilExit(); return nil }
            return p.terminationStatus
        }.value
    }
}
@MainActor public final class PowerObserver {
    private var root: io_connect_t = 0
    private var port: IONotificationPortRef?
    private var notifier: io_object_t = 0
    public var changed: ((Bool) -> Void)?
    public init() {}
    public func start() -> Bool {
        root = IORegisterForSystemPower(Unmanaged.passUnretained(self).toOpaque(), &port, { context, _, message, argument in
            guard let context else { return }
            let me = Unmanaged<PowerObserver>.fromOpaque(context).takeUnretainedValue()
            // Callback is on the explicitly installed main CFRunLoop source.
            if message == 0xE0000280 { me.changed?(true) }
            if message == 0xE0000300 { me.changed?(false) }
            if message == 0xE0000280 || message == 0xE0000270 { IOAllowPowerChange(me.root, Int(bitPattern: argument)) }
        }, &notifier)
        guard root != 0, let port, let source = IONotificationPortGetRunLoopSource(port)?.takeUnretainedValue() else { return false }
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .defaultMode); return true
    }
    public func stop() { if root != 0 { IODeregisterForSystemPower(&notifier); IOServiceClose(root); root = 0 }; if let port { IONotificationPortDestroy(port); self.port = nil } }
}

// LaunchServices may never call back. Complete once within the action deadline;
// a late callback cannot resume the continuation a second time.
private final class LaunchReply: @unchecked Sendable {
    private let lock = NSLock()
    private var reply: CheckedContinuation<String?, Never>?
    init(_ reply: CheckedContinuation<String?, Never>) { self.reply = reply }
    func finish(_ value: String?) { lock.lock(); let c = reply; reply = nil; lock.unlock(); c?.resume(returning: value) }
}
