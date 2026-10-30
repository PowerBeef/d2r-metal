# D2R Metal brand

The project's logo and app icon: the **Runestone**, a forged-iron tile shaped like a silicon die (D2R Metal runs natively on Apple
silicon), engraved with ᛗ, the Elder Futhark rune for M (Metal), and filled with molten metal. Original artwork: no game art and no
game fonts. The lettering is [Cinzel](https://github.com/NDISCOVER/Cinzel) (SIL Open Font License 1.1, `fonts/OFL.txt`).

| File | What it is |
|---|---|
| `brand.html` | the source of every piece, drawn in SVG; each piece shows alone by its `#hash` |
| `render.sh` | renders the pieces with headless Chrome (needs Google Chrome and Pillow) |
| `out/emblem.png` | the emblem alone, 1024 px, transparent |
| `out/wordmark.png` | "D2R METAL", transparent |
| `out/lockup.png` | emblem, wordmark and tagline side by side, transparent |
| `out/icon-flat.png` | the app icon flattened, 1024 px, opaque (the iPhone icon, and the Mac icon where Xcode is missing) |
| `AppIcon.icon/` | the Mac app icon in Icon Composer's format: three layers (iron, bronze frame with glass, molten rune) that macOS cuts to its rounded shape and lights |
| `icon.sh` | compiles `AppIcon.icon` into an app's Resources (`Assets.car` + `AppIcon.icns`); used by `mac_app.sh`, `launcher_app.sh` and `release_mac.sh` |
| `fonts/` | Cinzel, also bundled with the launcher for its headings |

The game app, the launcher and the iPhone app share the icon. To change the art: edit `brand.html`, run `render.sh`, copy
`out/icon-flat.png` to `dm/ios/Assets.xcassets/AppIcon.appiconset/AppIcon1024.png`, and look at the result with `icon.sh` in a test app.
