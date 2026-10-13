import SwiftUI
import MacControlCore

// Support commands use the same service/setup code as the UI. They never
// create an NSApplication, expose credentials, or open a serial port.
@main enum MacControlMain {
    @MainActor static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        let commands = ["--background-status", "--repair-background-service", "--resume-saved-setup"]
        guard let command = arguments.first, commands.contains(command) else {
            MacControlApp.main(); return
        }
        Task {
            do {
                guard arguments.count == (command == "--resume-saved-setup" ? 2 : 1) else {
                    throw AgentFailure("Specify a saved device ID only for --resume-saved-setup")
                }
                let model = AppModel()
                if command == "--repair-background-service" {
                    model.register()
                    while model.busy { try await Task.sleep(nanoseconds: 100_000_000) }
                    if let error = model.error { throw AgentFailure(error) }
                }
                await model.refresh()
                guard model.serviceStatus == "Enabled" else {
                    throw AgentFailure("Background service: \(model.serviceStatus)")
                }
                if command == "--resume-saved-setup" {
                    let setup = model.deviceSetup
                    guard setup.savedDeviceIDs.contains(arguments[1]) else { throw AgentFailure("Unknown saved device") }
                    setup.selectedSavedID = arguments[1]; setup.loadSavedCredentials()
                    if let error = setup.error { throw AgentFailure(error) }
                    setup.pairThisMac = true
                    setup.begin(agent: model, networkOnly: true)
                    while setup.busy { try await Task.sleep(nanoseconds: 100_000_000) }
                    if let error = setup.error { throw AgentFailure(error) }
                    guard setup.completed else { throw AgentFailure("Saved setup did not finish") }
                }
                let state = try await model.fetch()
                let summary: [String: JSONValue] = [
                    "app_version": .string(EventFactory.version),
                    "service_status": .string(model.serviceStatus),
                    "phase": .string(state.phase), "paired": .bool(state.paired),
                    "has_endpoint": .bool(!state.configuration.hostname.isEmpty),
                    "heartbeat_observed": .bool(state.lastHeartbeat != nil)
                ]
                print(String(decoding: try JSONEncoder().encode(summary), as: UTF8.self))
                exit(0)
            } catch { fputs("MacControl support command: \(error.localizedDescription)\n", stderr); exit(1) }
        }
        CFRunLoopRun()
    }
}
