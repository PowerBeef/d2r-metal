import Foundation

/// Which of the player's packages a file is, from the title id or version tag dumpers put in its name (base 0100726014352000 / v0,
/// update 0100726014352800 / v2228224, the Reign of the Warlock add-on otherwise); nil when the name says nothing. Only a hint for drops:
/// unpacking identifies every package by its contents (dm/tools/extract_game.sh)
public enum PackageKind: Equatable, Sendable {
    case base, update, dlc

    public static func guess(_ fileName: String) -> PackageKind? {
        let n = fileName.lowercased()
        // the add-on first: it carries a [v0] tag too
        if n.range(of: #"0100726014353[0-9a-f]{3}"#, options: .regularExpression) != nil || n.contains("dlc") || n.contains("warlock") { return .dlc }
        if n.contains("352800") || n.contains("v2228224") || n.contains("update") { return .update }
        if n.contains("352000") || n.contains("[v0]") || n.contains("base") { return .base }
        return nil
    }
}
