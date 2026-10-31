// Signs a release's manifest.json for the D2R Metal Launcher (docs/LAUNCHER.md "Trust"): an Ed25519 signature over the file's exact bytes, written
// beside it as manifest.json.sig (base64). The launcher checks it with the public keys compiled into LauncherCore (ManifestSignature.publicKeys)
// before it reads any field. The private key lives only in the maintainer's login keychain (a generic password item, service SERVICE), never in a
// file or in git. Run on the release Mac with `swift dm/tools/manifest_sign.swift <command>` (dm/tools/release_mac.sh does):
//   keygen               makes the key (refuses when one exists) and prints its public key, to add to ManifestSignature.publicKeys
//   public               prints the public key of the key in the keychain
//   sign <file> [<out>]  writes <out> (default <file>.sig)
//   verify <file> <sig> <public key>   exit 0 when the signature is good (tests)
// The keychain may ask once whether this tool may read the key (after an Xcode update changes the Swift interpreter); choose Always Allow.
import CryptoKit
import Foundation
import Security

let SERVICE = "org.d2rmetal.manifest-signing", ACCOUNT = "ed25519"

func fail(_ s: String) -> Never { FileHandle.standardError.write(Data("manifest_sign: \(s)\n".utf8)); exit(1) }

func storedKey() -> Curve25519.Signing.PrivateKey? {
    var out: CFTypeRef?
    let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: SERVICE, kSecAttrAccount as String: ACCOUNT,
                            kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
    let st = SecItemCopyMatching(q as CFDictionary, &out)
    if st == errSecItemNotFound { return nil }
    guard st == errSecSuccess, let raw = out as? Data else { fail("cannot read the signing key from the keychain (OSStatus \(st))") }
    guard let key = try? Curve25519.Signing.PrivateKey(rawRepresentation: raw) else { fail("the keychain item \(SERVICE) is not an Ed25519 key") }
    return key
}

let args = CommandLine.arguments.dropFirst()
switch args.first {
case "keygen":
    if storedKey() != nil { fail("a signing key already exists in the keychain (service \(SERVICE)); `public` prints it") }
    let key = Curve25519.Signing.PrivateKey()
    let item: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: SERVICE, kSecAttrAccount as String: ACCOUNT,
                               kSecAttrLabel as String: "D2R Metal release manifest signing key", kSecValueData as String: key.rawRepresentation,
                               kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlocked]
    let st = SecItemAdd(item as CFDictionary, nil)
    guard st == errSecSuccess else { fail("cannot store the key in the keychain (OSStatus \(st))") }
    print(key.publicKey.rawRepresentation.base64EncodedString())
case "public":
    guard let key = storedKey() else { fail("no signing key in the keychain: run keygen once") }
    print(key.publicKey.rawRepresentation.base64EncodedString())
case "sign":
    guard args.count >= 2 else { fail("usage: sign <file> [<out>]") }
    let file = args[args.startIndex + 1], out = args.count >= 3 ? args[args.startIndex + 2] : file + ".sig"
    guard let key = storedKey() else { fail("no signing key in the keychain: run keygen once") }
    guard let data = FileManager.default.contents(atPath: file) else { fail("cannot read \(file)") }
    let sig = try key.signature(for: data)
    guard key.publicKey.isValidSignature(sig, for: data) else { fail("the signature does not verify") }
    do { try Data((sig.base64EncodedString() + "\n").utf8).write(to: URL(fileURLWithPath: out)) } catch { fail("cannot write \(out): \(error)") }
case "verify":
    guard args.count == 4 else { fail("usage: verify <file> <sig> <public key>") }
    let a = Array(args)
    guard let data = FileManager.default.contents(atPath: a[1]), let sigText = try? String(contentsOfFile: a[2], encoding: .utf8),
          let sig = Data(base64Encoded: sigText.trimmingCharacters(in: .whitespacesAndNewlines)), let pub = Data(base64Encoded: a[3]),
          let key = try? Curve25519.Signing.PublicKey(rawRepresentation: pub) else { fail("unreadable input") }
    exit(key.isValidSignature(sig, for: data) ? 0 : 1)
default:
    fail("usage: keygen | public | sign <file> [<out>] | verify <file> <sig> <public key>")
}
