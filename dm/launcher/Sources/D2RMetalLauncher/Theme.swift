import AppKit
import SwiftUI

// The launcher's look: dark only, neutral greys, the system's typeface, and one restrained red for what matters now (the main button, the
// progress bar, the step running). No backdrop art, effects or display lettering: the window is a tool, the brand is the icon.
enum Theme {
    static let bg = Color(hex: 0x111113)            // the window
    static let surface = Color(hex: 0x18181B)       // cards
    static let raised = Color(hex: 0x222226)        // controls on a card
    static let line = Color(hex: 0x2B2B30)          // hairlines and card borders
    static let text = Color(hex: 0xECECEE)
    /// Secondary text: about 7:1 on the cards; with Increase Contrast, nearly the primary text
    static let text2 = Color(nsColor: NSColor(name: nil) { a in
        a.bestMatch(from: [.accessibilityHighContrastDarkAqua]) != nil ? NSColor(white: 0.86, alpha: 1) : NSColor(white: 0.66, alpha: 1)
    })
    static let text3 = Color(hex: 0x76767E)
    static let accent = Color(hex: 0xA8342A)        // the red: the main button's fill, progress, the running step; nothing else
    static let accentPressed = Color(hex: 0x8A2B23)
    /// The page and its side column never grow wider than this (centered beyond it): line lengths stay readable on a large display
    static let maxContentWidth: CGFloat = 1320
    static let ok = Color(hex: 0x6FBF7F)            // a file that passed its check
    static let warn = Color(hex: 0xE0A458)
    static let bad = Color(hex: 0xE5675B)
}

extension Color {
    init(hex: UInt32) { self.init(red: Double(hex >> 16 & 0xFF) / 255, green: Double(hex >> 8 & 0xFF) / 255, blue: Double(hex & 0xFF) / 255) }
}

/// The window's content layer: near-black, with a faint warm light low in the window and a cooler lift at the top. Not decoration: Liquid Glass
/// bends what is behind it, and over a flat black field the floating glass would read as flat grey
struct Backdrop: View {
    var body: some View {
        ZStack {
            Theme.bg
            RadialGradient(colors: [Theme.accent.opacity(0.16), .clear], center: UnitPoint(x: 0.42, y: 1.08), startRadius: 0, endRadius: 760)
            LinearGradient(colors: [.white.opacity(0.035), .clear], startPoint: .top, endPoint: .init(x: 0.5, y: 0.35))
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

extension View {
    /// A card: the surface color with a hairline border
    func card(padding: CGFloat = 18) -> some View {
        self.padding(padding).frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 12).fill(Theme.surface))
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Theme.line))
    }
}

/// A separator one device pixel thick, whatever the display's scale (a 1-point line is 2 pixels on a Retina display)
struct Hairline: View {
    var vertical = false
    @Environment(\.displayScale) private var scale
    var body: some View {
        Theme.line.frame(width: vertical ? 1 / scale : nil, height: vertical ? nil : 1 / scale).accessibilityHidden(true)
    }
}

/// A small heading over a card or a group of rows
struct SectionTitle: View {
    let text: String
    var body: some View { Text(text).font(.headline).foregroundStyle(Theme.text2).accessibilityAddTraits(.isHeader) }
}
