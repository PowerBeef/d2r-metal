import CryptoKit
import Foundation
import Security

/// What the launcher trusts (docs/LAUNCHER.md "Trust"). A release launcher carries the project's Developer ID signature: it then requires that same
/// team's signature on every program it installs (the packs' binaries and a new launcher), whatever the manifest says, and a manifest signed with
/// the project's release key (ManifestSignature). A development or test launcher (ad-hoc signed, no team) takes the team the manifest names, and
/// checks a manifest signature only when one is published beside it.
public struct Trust: Sendable, Equatable {
    /// The Developer ID team that signed this launcher; nil for an ad-hoc (development or test) build
    public var team: String?
    public init(team: String?) { self.team = team }

    public static let current = Trust(team: ownTeam())
    public var isRelease: Bool { team != nil }

    /// The team that must have signed what a release names: the launcher's own when it has one, else the manifest's (a test feed)
    public func team(for m: Manifest) -> String? { team ?? m.teamID }

    /// The team identifier in this process's own code signature (nil when ad-hoc or unsigned)
    static func ownTeam() -> String? {
        var me: SecCode?, stat: SecStaticCode?, info: CFDictionary?
        guard SecCodeCopySelf([], &me) == errSecSuccess, let me, SecCodeCopyStaticCode(me, [], &stat) == errSecSuccess, let stat,
              SecCodeCopySigningInformation(stat, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let dict = info as? [String: Any] else { return nil }
        return (dict[kSecCodeInfoTeamIdentifier as String] as? String).flatMap { $0.isEmpty ? nil : $0 }
    }

    /// A code requirement: an Apple-issued Developer ID certificate of this team (and, when given, this bundle identifier). The values are
    /// quoted for the requirement language, so a manifest cannot inject a clause
    public static func requirement(team: String, identifier: String? = nil) -> String {
        func q(_ s: String) -> String { "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\"" }
        return "anchor apple generic" + (identifier.map { " and identifier \(q($0))" } ?? "") +
            " and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists" +
            " and certificate leaf[subject.OU] = \(q(team))"
    }
}

/// The release key's signature over a manifest's exact bytes (manifest.json.sig beside manifest.json: base64 of an Ed25519 signature), made by
/// dm/tools/manifest_sign.swift on the release Mac, whose keychain alone holds the private key
public enum ManifestSignature {
    /// Every public key whose signatures are accepted. A new key is added here in a launcher release signed with the current one, and the old one
    /// removed a release later (docs/LAUNCHER.md "Rotating the release key")
    public static let publicKeys = ["JZye1bCazr0+vl8CNhlJAJorsMJROp2mDHUZcViR1m8="]

    public static func isValid(_ signature: Data, for data: Data, keys: [String] = publicKeys) -> Bool {
        guard let text = String(data: signature, encoding: .utf8), let sig = Data(base64Encoded: text.trimmingCharacters(in: .whitespacesAndNewlines)) else { return false }
        return keys.contains { k in
            guard let raw = Data(base64Encoded: k), let key = try? Curve25519.Signing.PublicKey(rawRepresentation: raw) else { return false }
            return key.isValidSignature(sig, for: data)
        }
    }
}
