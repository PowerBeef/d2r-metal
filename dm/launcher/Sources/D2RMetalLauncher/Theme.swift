import SwiftUI

// The launcher's look: the README art's palette (~/dm-work/readme_art, original artwork, no game art or fonts) under Liquid Glass. A dark
// hellfire backdrop with rising embers is the content layer; the cards and buttons are glass floating above it, and only the primary action
// (Install, Play) carries the ember tint.
enum Palette {
    static let deep = [Color(hex: 0x5E1608), Color(hex: 0x2E0B07), Color(hex: 0x120A08), Color(hex: 0x070505)]   // the backdrop, from the glow out
    static let gold = [Color(hex: 0xFFF0C6), Color(hex: 0xE9C27A), Color(hex: 0xB27A2E), Color(hex: 0x7C4C18)]   // lettering, top to bottom
    static let parchment = Color(hex: 0xD3BB8C)      // captions on the backdrop
    static let parchmentDim = Color(hex: 0xA8916C)
    static let bronze = Color(hex: 0xA07638)
    static let ember = Color(red: 0.64, green: 0.16, blue: 0.06)   // the primary action's glass tint: deep hellfire red
    static let blood = Color(red: 0.85, green: 0.22, blue: 0.18)   // failures
    static let goldFill = LinearGradient(stops: [.init(color: gold[0], location: 0), .init(color: gold[1], location: 0.4),
                                                 .init(color: gold[2], location: 0.75), .init(color: gold[3], location: 1)], startPoint: .top, endPoint: .bottom)
}

extension Color {
    init(hex: UInt32) { self.init(red: Double(hex >> 16 & 0xFF) / 255, green: Double(hex >> 8 & 0xFF) / 255, blue: Double(hex & 0xFF) / 255) }
}

// MARK: backdrop

/// The window's background: the hellfire glow from below, a faint stone grain, a vignette and rising embers
struct Backdrop: View {
    var body: some View {
        GeometryReader { g in
            ZStack {
                RadialGradient(stops: [.init(color: Palette.deep[0], location: 0), .init(color: Palette.deep[1], location: 0.34),
                                       .init(color: Palette.deep[2], location: 0.7), .init(color: Palette.deep[3], location: 1)],
                               center: UnitPoint(x: 0.5, y: 1.02), startRadius: 0, endRadius: max(g.size.width, g.size.height) * 1.1)
                // a low haze of firelight along the bottom, which the glass above picks up
                EllipticalGradient(colors: [Color(red: 0.95, green: 0.32, blue: 0.08).opacity(0.28), .clear], center: UnitPoint(x: 0.5, y: 1.0),
                                   startRadiusFraction: 0, endRadiusFraction: 0.55)
                    .frame(height: g.size.height * 0.7).frame(maxHeight: .infinity, alignment: .bottom).blendMode(.plusLighter)
                Grain.image.resizable(resizingMode: .tile).blendMode(.overlay).opacity(0.10)
                Embers()
                RadialGradient(colors: [.clear, .black.opacity(0.55)], center: .center, startRadius: g.size.width * 0.35, endRadius: g.size.width * 0.85)
            }
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

/// A fixed tile of soft noise, made once (a stone-like grain under the glow)
enum Grain {
    static let image: Image = {
        let w = 192, h = 192
        var px = [UInt8](repeating: 0, count: w * h); var s: UInt32 = 0x9E3779B9
        for i in px.indices { s = s &* 1664525 &+ 1013904223; px[i] = UInt8(truncatingIfNeeded: 96 + (s >> 24) / 2) }
        let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w, space: CGColorSpaceCreateDeviceGray(),
                            bitmapInfo: CGImageAlphaInfo.none.rawValue)!
        return Image(decorative: ctx.makeImage()!, scale: 2)
    }()
}

/// Embers drifting up from the glow. Positions are a function of time only (no state), drawn additively; they stop while the window is
/// in the background and stay still with Reduce Motion
struct Embers: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.controlActiveState) private var active
    private static let count = 56

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: reduceMotion || active == .inactive)) { tl in
            Canvas { ctx, size in
                let t = reduceMotion ? 7.0 : tl.date.timeIntervalSinceReferenceDate
                ctx.blendMode = .plusLighter
                for i in 0..<Embers.count {
                    let r = { (k: Double) in Embers.hash(i, k) }
                    let speed = 0.035 + r(1) * 0.05, phase = r(2), life = (t * speed + phase).truncatingRemainder(dividingBy: 1)
                    let spread = (r(3) - 0.5) * (0.55 + 0.45 * r(4))          // more of them near the middle, under the glow
                    let x = size.width * (0.5 + spread) + sin(t * (0.4 + r(5)) + r(6) * 6.28) * (6 + 18 * r(7))
                    let y = size.height * (1.04 - life * 0.95)
                    let fade = sin(.pi * life) * (0.55 + 0.45 * sin(t * (5 + 4 * r(8)) + Double(i)))
                    let d = 1.0 + 2.0 * r(9) * r(9)                                // mostly sparks, a few larger cinders
                    let hue = Color(red: 1, green: 0.42 + 0.38 * r(10), blue: 0.12 + 0.25 * r(10))
                    let halo = d * 4, c = CGPoint(x: x, y: y)
                    ctx.fill(Path(ellipseIn: CGRect(x: x - halo, y: y - halo, width: halo * 2, height: halo * 2)),
                             with: .radialGradient(Gradient(colors: [hue.opacity(0.32 * fade), hue.opacity(0)]), center: c, startRadius: 0, endRadius: halo))
                    ctx.fill(Path(ellipseIn: CGRect(x: x - d / 2, y: y - d / 2, width: d, height: d)), with: .color(hue.opacity(0.9 * fade)))
                }
            }
        }
    }

    static func hash(_ i: Int, _ k: Double) -> Double {   // a stable pseudo-random number in [0, 1) per ember and property
        let v = sin(Double(i) * 12.9898 + k * 78.233) * 43758.5453
        return v - v.rounded(.down)
    }
}

// MARK: lettering and ornaments

/// "D2R METAL" in the README plates' gold: Optima, wide tracking, a dark under-edge and an ember glow
struct GoldTitle: View {
    var text: String
    var size: CGFloat = 46
    var body: some View {
        Text(text.uppercased())
            .font(.custom("Optima-Bold", size: size, relativeTo: .largeTitle))
            .tracking(size * 0.09)
            .foregroundStyle(Palette.goldFill)
            .shadow(color: Color(hex: 0x120804), radius: 0, x: 0, y: 1.5)
            .shadow(color: Color(red: 0.86, green: 0.24, blue: 0.08).opacity(0.45), radius: 12)
    }
}

/// The README's caption style: Copperplate, small capitals, parchment
struct Caption: View {
    var text: String
    var dim = false
    var size: CGFloat = 12
    var body: some View {
        Text(text.uppercased())
            .font(.custom("Copperplate", size: size, relativeTo: .caption).weight(.semibold))
            .tracking(size * 0.09)
            .foregroundStyle(dim ? Palette.parchmentDim : Palette.parchment)
            .shadow(color: .black.opacity(0.8), radius: 1, y: 1)
    }
}

/// A bronze hairline with a small gold lozenge in the middle
struct Rule: View {
    var body: some View {
        HStack(spacing: 8) {
            LinearGradient(colors: [.clear, Palette.bronze], startPoint: .leading, endPoint: .trailing).frame(height: 1)
            Image(systemName: "diamond.fill").font(.system(size: 7)).foregroundStyle(Palette.goldFill)
            LinearGradient(colors: [Palette.bronze, .clear], startPoint: .leading, endPoint: .trailing).frame(height: 1)
        }
        .frame(maxWidth: 360)
        .accessibilityHidden(true)
    }
}

// MARK: glass

extension View {
    /// A card of regular Liquid Glass over the backdrop
    func glassCard(cornerRadius: CGFloat = 22, padding inset: CGFloat = 18) -> some View {
        padding(inset).frame(maxWidth: 600).glassEffect(.regular, in: .rect(cornerRadius: cornerRadius))
    }

    /// The one tinted action of a screen (Install, Play, Update)
    func primaryGlass() -> some View { buttonStyle(.glassProminent).tint(Palette.ember) }
}
