import Foundation

public enum SetupRoute: String, CaseIterable { case newDevice, savedDevice, existingDevice }
public enum SetupStep: Int, CaseIterable { case mac, usb, network, pairing, connection, handoff }
public enum SetupStepStatus: Equatable { case pending, active, complete, attention }

/// A saved IP is not proof of network access, and an enabled registration is
/// not proof of a responsive service. Callers supply only verified milestones.
public struct SetupJourney {
    public let steps: [SetupStep]
    public let current: SetupStep?
    public let completed: Set<SetupStep>
    public let needsAttention: Bool
    public init(route: SetupRoute, pairThisMac: Bool = true, serviceReady: Bool = false,
                usbComplete: Bool = false, networkVerified: Bool = false,
                pairingSaved: Bool = false, connected: Bool = false,
                handoffReady: Bool = false, workingOn: SetupStep? = nil,
                needsAttention: Bool = false) {
        if route == .existingDevice { steps = [.mac, .pairing, .connection] }
        else if pairThisMac { steps = [.mac, .usb, .network, .pairing, .connection] }
        else { steps = [.usb, .network, .handoff] }
        var done: Set<SetupStep> = []
        if serviceReady { done.insert(.mac) }
        if usbComplete { done.insert(.usb) }
        if networkVerified { done.insert(.network) }
        if pairingSaved { done.insert(.pairing) }
        if connected { done.formUnion([.mac, .usb, .network, .pairing, .connection]) }
        if handoffReady { done.formUnion([.usb, .network, .handoff]) }
        let visibleSteps = steps
        completed = done.intersection(Set(visibleSteps))
        current = workingOn.flatMap { visibleSteps.contains($0) ? $0 : nil } ?? visibleSteps.first { !done.contains($0) }
        self.needsAttention = needsAttention
    }
    public func status(of step: SetupStep) -> SetupStepStatus {
        if step == current { return needsAttention ? .attention : .active }
        return completed.contains(step) ? .complete : .pending
    }
}
