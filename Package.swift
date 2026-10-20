// swift-tools-version:5.9
// libsseq as a Swift package: the C API (include/sseq/sseq.h) is exposed as the module `SSeq`.
// The C++ sources are compiled by SwiftPM; no CMake is involved. Usage:
//
//     .package(url: "https://github.com/zaskar9/libsseq.git", from: "0.3.0")
//     ...
//     .target(name: "App", dependencies: [.product(name: "SSeq", package: "libsseq")])
//
// and `import SSeq` in Swift.
import PackageDescription

let package = Package(
    name: "libsseq",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [
        .library(name: "SSeq", targets: ["SSeq"]),
    ],
    targets: [
        .target(
            name: "SSeq",
            path: ".",
            sources: ["src"],
            publicHeadersPath: "swift/include",
            cxxSettings: [
                .headerSearchPath("include"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx20
)
