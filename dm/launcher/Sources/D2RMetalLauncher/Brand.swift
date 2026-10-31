import AppKit
import CoreText
import SwiftUI

/// The project's brand art (dm/brand): the Runestone emblem, the "D2R METAL" wordmark and the Cinzel typeface (SIL OFL). The app bundle carries
/// them in Contents/Resources/Brand, laid out as dm/brand (launcher_app.sh copies them); a development run reads dm/brand itself
enum Brand {
    static let folder: URL? = {
        if let r = Bundle.main.resourceURL?.appendingPathComponent("Brand"), FileManager.default.fileExists(atPath: r.path) { return r }
        #if DEBUG   // `swift run` from the checkout; a release build never carries the build Mac's source path
        let dev = URL(fileURLWithPath: #filePath).deletingLastPathComponent()   // dm/launcher/Sources/D2RMetalLauncher → dm/brand
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("brand")
        return FileManager.default.fileExists(atPath: dev.path) ? dev : nil
        #else
        return nil
        #endif
    }()

    static let emblem = image("out/emblem.png")
    static let wordmark = image("out/wordmark.png")

    private static func image(_ path: String) -> Image? {
        guard let url = folder?.appendingPathComponent(path), let ns = NSImage(contentsOf: url) else { return nil }
        return Image(nsImage: ns)
    }

    /// Makes Cinzel available to this process (once, at launch); without it the headings fall back to Optima
    static func registerFonts() {
        guard let url = folder?.appendingPathComponent("fonts/Cinzel-wght.ttf") else { return }
        CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
    }
    static var hasCinzel: Bool { NSFont(name: "CinzelRoman-Bold", size: 12) != nil }
}

extension Font {
    /// Cinzel at a weight (400 regular, 700 bold, 900 black), else Optima: the brand's lettering for titles, captions and the big buttons
    static func cinzel(_ size: CGFloat, _ weight: Font.Weight = .bold, relativeTo style: Font.TextStyle = .body) -> Font {
        guard Brand.hasCinzel else { return .custom(weight == .regular ? "Optima" : "Optima-Bold", size: size, relativeTo: style) }
        let name = weight == .black || weight == .heavy ? "CinzelRoman-Black" : weight == .regular || weight == .light ? "Cinzel-Regular" : "CinzelRoman-Bold"
        return .custom(name, size: size, relativeTo: style)
    }
}
