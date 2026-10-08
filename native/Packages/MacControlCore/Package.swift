// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "MacControlCore", platforms: [.macOS(.v13)], products: [.library(name: "MacControlCore", targets: ["MacControlCore"])], targets: [.target(name: "MacControlCore"), .testTarget(name: "MacControlCoreTests", dependencies: ["MacControlCore"])])
