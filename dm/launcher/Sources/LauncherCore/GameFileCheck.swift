import CryptoKit
import Foundation

/// What the launcher can tell about a chosen file before installing, without unpacking it or needing the keys: an .nsp's table of contents
/// names its ticket, whose rights id starts with the title id (base, update or add-on of this game); an .xci shows its card header; a key
/// file shows its key names; an extracted folder's program file has a known checksum. The values in a key file are never shown.
/// Unpacking checks everything again by content (dm/tools/extract_game.sh, dm/tools/install.py).
public enum GameFileCheck {
    public enum Verdict: Equatable, Sendable {
        case ok(String)          // the right file; what was recognized
        case note(String)        // usable, with something to know (checked again while unpacking)
        case wrong(String)       // not this file; why
        case elsewhere(PackageKind, String)   // a package of this game, but for another selector
    }

    static let base: UInt64 = 0x0100_7260_1435_2000      // the game's title id; the update is base | 0x800, add-ons (base & ~0xFFF) + 0x1000 + n
    static let mainSHA256 = "55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e"   // ExeFS/main of 1.0.34.0, as in setup.sh

    // MARK: packages

    public static func package(_ url: URL, expected: PackageKind) -> Verdict {
        guard let h = try? FileHandle(forReadingFrom: url) else { return .wrong("Can't read this file") }
        defer { try? h.close() }
        guard let head = try? h.read(upToCount: 0x200), head.count >= 16 else { return .wrong("This file is empty or too short") }
        if head.prefix(4) == Data("PFS0".utf8) { return nsp(h, head: head, expected: expected) }
        if head.count >= 0x104, head[0x100..<0x104] == Data("HEAD".utf8) {
            return expected == .dlc ? .note("A game card image; unpacking reads what it holds") : .note("A game card image (.xci); its contents are checked while unpacking")
        }
        return .wrong("Not a Switch package (.nsp or .xci)")
    }

    static func nsp(_ h: FileHandle, head: Data, expected: PackageKind) -> Verdict {
        let count = Int(head.u32(4)), strings = Int(head.u32(8))
        guard count > 0, count < 4096, strings > 0, strings < 1 << 20 else { return .wrong("This package's table of contents is damaged") }
        let tableEnd = 16 + count * 24 + strings
        try? h.seek(toOffset: 0)
        guard let table = try? h.read(upToCount: tableEnd), table.count == tableEnd else { return .wrong("This package is cut short") }
        var names: [String] = []
        for i in 0..<count {
            let off = 16 + count * 24 + Int(table.u32(16 + i * 24 + 16))
            guard off < table.count, let end = table[off...].firstIndex(of: 0) else { continue }
            names.append(String(decoding: table[off..<end], as: UTF8.self))
        }
        let titles = names.compactMap { n -> UInt64? in
            guard n.lowercased().hasSuffix(".tik"), n.count >= 36 else { return nil }
            return UInt64(n.prefix(16), radix: 16)
        }
        guard !titles.isEmpty else {
            return names.contains(where: { $0.hasSuffix(".nca") }) ? .note("No ticket to read the title from; it is checked while unpacking") : .wrong("This package holds no game content")
        }
        // every ticket counts: a package may bundle the update with an add-on, say; it fits the selector when one of its titles does
        func kind(_ tid: UInt64) -> PackageKind? { tid == base ? .base : tid == base | 0x800 ? .update : tid >> 12 == (base >> 12) + 1 ? .dlc : nil }
        func what(_ k: PackageKind) -> String { k == .base ? "Base game" : k == .update ? "Update" : "Reign of the Warlock DLC" }
        let kinds = titles.map(kind)
        guard let found = kinds.compactMap({ $0 }).first else { return .wrong(String(format: "Another game (title %016llX)", titles[0])) }
        guard let i = kinds.firstIndex(of: expected) else { return .elsewhere(found, "This is the \(what(found).lowercased()) package") }
        let also = Set(kinds.compactMap { $0 }).subtracting([expected]).map { what($0).lowercased() }.sorted()
        return .ok(String(format: "\(what(expected)), title %016llX", titles[i]) + (also.isEmpty ? "" : " (with the \(also.joined(separator: " and ")))")
                   + (expected == .update ? " · version checked at install" : ""))
    }

    // MARK: keys

    public static func keys(_ url: URL) -> Verdict {
        // at most 1 MB is read: a key file is a few KB, and a game package chosen here by mistake is many GB
        guard let h = try? FileHandle(forReadingFrom: url) else { return .wrong("Can't read this file") }
        defer { try? h.close() }
        guard let data = try? h.read(upToCount: (1 << 20) + 1), data.count <= 1 << 20 else { return .wrong("Too big to be a key file") }
        guard let text = String(data: data, encoding: .utf8) else { return .wrong("Can't read this file as a key file") }
        var names = Set<String>(), odd = 0
        for line in text.split(whereSeparator: \.isNewline) {
            let parts = line.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2, !parts[0].isEmpty, parts[1].allSatisfy(\.isHexDigit), !parts[1].isEmpty else { continue }
            names.insert(parts[0].lowercased())
            if parts[1].count % 32 != 0 { odd += 1 }   // keys are whole 16-byte blocks; unpacking skips the others
        }
        guard !names.isEmpty else { return .wrong("Not a key file (no \"name = value\" lines)") }
        guard names.contains("header_key"), names.contains(where: { $0.hasPrefix("master_key_") || $0.hasPrefix("key_area_key_application_") }) else {
            return .wrong("This key file lacks the keys games need (header_key, master keys)")
        }
        let summary = "Console keys, \(names.count) entries"
        return odd == 0 ? .ok(summary) : .note(summary + "; \(odd) line\(odd == 1 ? "" : "s") of the wrong length will be skipped")
    }

    // MARK: extracted folder

    public static func folder(_ url: URL) -> Verdict {
        let fm = FileManager.default
        let candidates = [url, url.appendingPathComponent("Update"), url.appendingPathComponent("data/switch/3.3.94012/Update")]
        guard let root = candidates.first(where: { fm.fileExists(atPath: $0.appendingPathComponent("ExeFS/main").path) }) else {
            return .wrong("No ExeFS/main here (choose the folder that holds ExeFS and RomFS)")
        }
        for name in ["sdk", "subsdk0"] where !fm.fileExists(atPath: root.appendingPathComponent("ExeFS/\(name)").path) { return .wrong("ExeFS/\(name) is missing") }
        guard fm.fileExists(atPath: root.appendingPathComponent("RomFS/data/build_manifest.psv").path) else { return .wrong("RomFS/data/build_manifest.psv is missing") }
        guard let sum = try? sha256(root.appendingPathComponent("ExeFS/main")) else { return .wrong("Can't read ExeFS/main") }
        return sum == mainSHA256 ? .ok("Version 1.0.34.0, extracted") : .wrong("ExeFS/main is not from version 1.0.34.0")
    }

    static func sha256(_ file: URL) throws -> String {
        let h = try FileHandle(forReadingFrom: file); defer { try? h.close() }
        var d = SHA256()
        while let chunk = try h.read(upToCount: 1 << 20), !chunk.isEmpty { d.update(data: chunk) }
        return d.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

extension Data {
    func u32(_ at: Int) -> UInt32 {
        UInt32(self[startIndex + at]) | UInt32(self[startIndex + at + 1]) << 8 | UInt32(self[startIndex + at + 2]) << 16 | UInt32(self[startIndex + at + 3]) << 24
    }
}
