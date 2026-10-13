import XCTest
@testable import MacControlCore

final class SetupJourneyTests: XCTestCase {
    func testNewDeviceStartsAtMacAndThenUSB() {
        XCTAssertEqual(SetupJourney(route: .newDevice).current, .mac)
        let ready = SetupJourney(route: .newDevice, serviceReady: true)
        XCTAssertEqual(ready.current, .usb)
        XCTAssertEqual(ready.status(of: .mac), .complete)
    }
    func testSavedCredentialsDoNotClaimNetworkVerificationOrPairing() {
        let saved = SetupJourney(route: .savedDevice, serviceReady: true, usbComplete: true)
        XCTAssertEqual(saved.current, .network)
        XCTAssertEqual(saved.status(of: .network), .active)
        XCTAssertEqual(saved.status(of: .pairing), .pending)
        XCTAssertEqual(saved.status(of: .connection), .pending)
    }
    func testUSBFailurePreservesVerifiedMilestonesAndMarksFailedStep() {
        let failure = SetupJourney(route: .newDevice, serviceReady: true, workingOn: .usb, needsAttention: true)
        XCTAssertEqual(failure.status(of: .mac), .complete)
        XCTAssertEqual(failure.status(of: .usb), .attention)
        XCTAssertEqual(failure.status(of: .network), .pending)
    }
    func testSavedPairingStillNeedsAnActiveConnection() {
        let paired = SetupJourney(route: .existingDevice, serviceReady: true, pairingSaved: true)
        XCTAssertEqual(paired.steps, [.mac, .pairing, .connection])
        XCTAssertEqual(paired.current, .connection)
        XCTAssertEqual(paired.status(of: .connection), .active)
    }
    func testConnectedCompletesTheJourney() {
        let connected = SetupJourney(route: .savedDevice, connected: true)
        XCTAssertNil(connected.current)
        XCTAssertTrue(connected.steps.allSatisfy { connected.status(of: $0) == .complete })
    }
    func testTechnicianHandoffDoesNotClaimThisMacIsPaired() {
        let ready = SetupJourney(route: .newDevice, pairThisMac: false, handoffReady: true)
        XCTAssertEqual(ready.steps, [.usb, .network, .handoff])
        XCTAssertNil(ready.current)
        XCTAssertFalse(ready.completed.contains(.pairing))
        XCTAssertFalse(ready.completed.contains(.connection))
    }
}
