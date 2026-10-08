import Foundation
@objc protocol ReadOnlyProbe { func snapshot(_ reply: @escaping (Data) -> Void) }
let c = NSXPCConnection(machServiceName: "dev.maccontrol.native.agent")
c.remoteObjectInterface = NSXPCInterface(with: ReadOnlyProbe.self)
c.resume()
let p = c.remoteObjectProxyWithErrorHandler { _ in print("UNTRUSTED_PEER_REJECTED"); exit(0) } as! ReadOnlyProbe
p.snapshot { _ in print("UNTRUSTED_PEER_ACCEPTED"); exit(1) }
DispatchQueue.global().asyncAfter(deadline: .now() + 5) { print("UNTRUSTED_PEER_TIMEOUT"); exit(2) }
RunLoop.main.run()
