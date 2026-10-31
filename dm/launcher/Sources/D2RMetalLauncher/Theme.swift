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
    static let ok = Color(hex: 0x6FBF7F)            // a file that passed its check
    static let warn = Color(hex: 0xE0A458)
    static let bad = Color(hex: 0xE5675B)
}

extension Color {
    init(hex: UInt32) { self.init(red: Double(hex >> 16 & 0xFF) / 255, green: Double(hex >> 8 & 0xFF) / 255, blue: Double(hex & 0xFF) / 255) }
}

/// The main action: a solid red button, the only one on screen
struct PrimaryButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold)).foregroundStyle(enabled ? .white : Theme.text3)
            .frame(minWidth: 150, minHeight: 40).padding(.horizontal, 18)
            .background(RoundedRectangle(cornerRadius: 8).fill(enabled ? (configuration.isPressed ? Theme.accentPressed : Theme.accent) : Theme.raised))
            .contentShape(RoundedRectangle(cornerRadius: 8))
    }
}

/// Every other button: a quiet grey one
struct QuietButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    var large = false   // the size of the main button (Stop takes its place while a job runs)
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: large ? 15 : 13, weight: large ? .semibold : .medium)).foregroundStyle(enabled ? Theme.text : Theme.text3)
            .frame(minWidth: large ? 150 : nil, minHeight: large ? 40 : 28).padding(.horizontal, large ? 18 : 12)
            .background(RoundedRectangle(cornerRadius: 7).fill(configuration.isPressed ? Theme.line : Theme.raised))
            .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(Theme.line))
            .contentShape(RoundedRectangle(cornerRadius: 7))
    }
}

/// A plain text action in the side column (Repair, Show Log…)
struct LinkButtonStyle: ButtonStyle {
    @Environment(\.isEnabled) private var enabled
    var destructive = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13)).foregroundStyle(!enabled ? Theme.text3 : destructive ? Theme.bad : configuration.isPressed ? Theme.text2 : Theme.text)
            .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
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

/// A small heading over a card or a group of rows
struct SectionTitle: View {
    let text: String
    var body: some View { Text(text).font(.system(size: 13, weight: .semibold)).foregroundStyle(Theme.text2).accessibilityAddTraits(.isHeader) }
}
