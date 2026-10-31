import AppKit
import SwiftUI

/// The project's mark in the window: the app's own icon (dm/brand, compiled into the bundle by dm/brand/icon.sh), as macOS shows it in the Dock.
/// A development run outside a bundle (`swift run`) has no icon of its own and reads dm/brand's flat render instead
@MainActor enum Brand {
    static let icon: Image = {
        if Bundle.main.bundleURL.pathExtension == "app" { return Image(nsImage: NSApp.applicationIconImage) }
        #if DEBUG   // the checkout's dm/brand; a release build never carries the build Mac's source path
        let flat = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("brand/out/icon-flat.png")
        if let ns = NSImage(contentsOf: flat) { return Image(nsImage: ns) }
        #endif
        return Image(nsImage: NSApp.applicationIconImage)
    }()
}
