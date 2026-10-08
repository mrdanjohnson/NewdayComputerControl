import Foundation

public struct CrashHistory: Codable, Sendable {
    public var abnormalStarts: [Date] = []
    public var wasRunning = false
    public init() {}
    public mutating func begin(at now: Date) -> TimeInterval {
        abnormalStarts = abnormalStarts.filter { now.timeIntervalSince($0) >= 0 && now.timeIntervalSince($0) < 60 }
        if wasRunning { abnormalStarts.append(now) }
        wasRunning = true
        if abnormalStarts.count >= 3 { abnormalStarts.removeAll(); return 60 }
        return 0
    }
    public mutating func cleanStop() { wasRunning = false; abnormalStarts.removeAll() }
}
