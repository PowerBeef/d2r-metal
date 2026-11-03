# Playing on a Mac (macOS App)

On an Apple silicon Mac D2R Metal is a native macOS App, `D2R Metal.app`, with keyboard, mouse, trackpad and gamepad all working. It runs the same game and the same features as on iPhone and iPad (the Extras page, the Video tab and so on). With the mouse and keyboard it shows the game's own PC interface and plays as D2R does on PC; with a controller, the console interface. Most players install it with **D2R Metal Launcher**; it can also be built from source.

**How it has been tried**: the App is played on an M6 Mac with the keyboard, the mouse and an Xbox Wireless Controller, windowed and in full screen, and quit from the menu and the Dock. Where this page says "verified" without more, the check was an unattended run: keys and mouse events injected by a test script, quitting, full screen and resizing through test switches. What nobody has tried by hand yet is listed under [Known shortcomings](#known-shortcomings); if you try one of those, please open an Issue and tell us how it went.

## Requirements

Apple silicon and **macOS 27 or later** (Intel Macs are not supported; see "[Supported devices](../README.md#supported-devices)" in the README for every device). D2R Metal draws only through Metal 4 and uses what macOS 27 adds, without fallbacks for older systems; on an earlier system the App does not open (the system says it cannot be used with this version of macOS).

## Install with the launcher

Download **D2R Metal Launcher** from the [latest release](https://github.com/PowerBeef/d2r-metal/releases/latest), choose your game packages and `prod.keys` (or an extracted folder) and press **Install**. It builds the App on your Mac from your own files and keeps it up to date from this project's releases, with no Xcode, Homebrew or developer account; open the game from the launcher from then on. An app it installs compiles the game's shaders with macOS's own compiler: once at install (a few minutes), and once more after each macOS update, with a progress window. Everything about it is in [LAUNCHER.md](LAUNCHER.md).

The rest of this page applies to both kinds of App, except where it says otherwise.

## Build from source

Build it yourself to work on the project, or to sign it with a paid developer account for iCloud save sync. The prerequisites are the same as for iOS: game files in place and `./setup.sh` finished (see "Getting started" in the README). Then:

```bash
source ~/dm-work/build/cache/dm/venv/bin/activate
dm/tools/mac_app.sh                  # macOS 27 or later
```

The build takes ten-odd seconds and produces `~/dm-work/out/macapp/D2R Metal.app` (about 3 MB). To put the output elsewhere, add an output directory after the command.

The App contains only our own program. The rewritten game program, the shader libraries and the UI overlay are all made from your game files; they stay in your working directory (`~/dm-work`) and the App only records where they are — so **the App runs only on the Mac that built it**. Please do not copy it to anyone, and do not delete or move the working directory. Signing is local ad-hoc signing: no developer account and no device registration.

Version and build number can be set: `DM_MAC_VERSION` (default 1.0) and `DM_MAC_BUILD` (default: the current month-day-hour-minute).

The App gets the project's own icon (the layered Runestone icon from `dm/brand`). Use your own picture with `DM_MAC_ICON_SOURCE=<image>` (an opaque square that fills the whole picture; macOS 26 and later round the corners themselves, and a picture with transparent margins is shown shrunk inside a gray frame), or build without an icon with `DM_MAC_ICON_SOURCE=none`.

## First launch

Open the game from the launcher (an App it installs already knows where your game files are) or double-click an App you built. An App you built asks on its first launch for the **game resource folder**: pick `~/dm-work/data/switch/3.3.94012/Update` (or the `RomFS` inside it); it must contain a `data` folder. Choosing the folder that directly holds the `.data` files and `build_manifest.psv` also works. A wrong choice is explained and you are asked again; a correct one is remembered.

A key reference is then shown once; afterwards it is under Help in the menu bar.

Language: the first launch follows the system language (Chinese systems start in Chinese, Spanish in Spanish, Russian in Russian, everything else in English). To change it: Options → Extras, the language switch in the first row → Apply → close and reopen the App.

## Mouse and keyboard, or a controller

The App has two interfaces and switches between them by what you play with, as D2R does on PC:

- **Mouse and keyboard: the PC interface.** The PC HUD (the orbs with their angel and demon, skill buttons, belt and mini panel), the PC main menu and character screens, the game's own gauntlet pointer and D2R's PC keys.
- **A controller: the console interface,** the Switch's own, with its button prompts.

Press a button on a connected controller and the console interface comes up; move the mouse or press a key and the PC one comes back. The switch takes a moment while the game rebuilds its screens; nothing you were doing is lost. The App starts in the PC interface.

### In game with the mouse and keyboard

The keys are D2R's PC defaults:

| Keys | What they do |
|---|---|
| Left click | Walk there; on a monster attack it; on a character, chest, waypoint or item on the ground, use it. Hold to keep walking or attacking towards the pointer |
| Right click | Cast the right skill at the pointer; on an item in a panel, use it (drink, read, open) |
| Shift + left click | Attack without moving |
| Shift held | Compare the item under the pointer with what you wear |
| 1 – 4 | Drink from the belt |
| F1 – F8 | Select skills |
| Mouse wheel | Previous / next skill; over a list, scroll it |
| I or B, C or A, T, Q | Inventory, character, skill tree, quests; press again to close |
| Tab | Map |
| Alt | Show items on the ground |
| W | Swap weapons |
| R | Run / walk |
| Space | Close all panels |
| Esc | Close what is open; with nothing open, the game menu (Save and Exit, Quick Restart, Options, Chronicle); in Options, back. After death, continue |

**Panels** (inventory, character, stash, vendors, the cube) are the console ones, drawn so they end above the PC HUD, and worked with the mouse: hover an item for its description, hold Shift to compare, right click to use; press and drag to move it (between cells, onto a body slot, into the stash or cube, onto a vendor's shelf = sell, into the inventory = buy); a double click also uses, puts on or takes off. Descriptions use the PC wording ("Right Click to Use", "Hold Shift to Compare"). The PC HUD's mini panel and run / walk button take clicks.

The pointer is the game's own gauntlet in the PC interface. In the console interface it is the macOS arrow, which hides itself after it has rested over the game window for 3 seconds and comes back the moment the mouse moves; to change the delay: `defaults write org.d2rmobile.mac CursorHideSeconds -float 5` (seconds; `0` turns it off), then restart the App.

### In the menus, and with a controller

Outside a game the menus take clicks, and the keyboard works them too: the arrow keys move, Return confirms (it acts as + where the bottom of the page shows a "+" hint, such as Accept after naming), Esc goes back and `=` is + (new character). The other controller buttons are also on keys there: Space or J = A, K = B, U / I = X / Y, Q / E = L / R, Z / C = ZL / ZR, Tab = −.

A controller works as soon as it is connected, alongside the keyboard: the console interface, its buttons as on the Switch and the button prompts of the controller you use (Switch, Xbox or PlayStation).

**Creating a character, start to finish**: on the main menu click "New Character" (or press `=`) → click a class's figure (or choose one with the arrow keys and press Return) → on the mode page press Return → on the naming page press Return to open the text box, type, Return → press Return once more to create. With a controller, A on the naming page brings up the on-screen keyboard instead (see below).

### Typing names and amounts

When the game asks for text — a character name, an amount of gold to drop or move, a Chronicle search — what appears depends on what you are playing with:

- **Keyboard and mouse**: a text box slides down from the top of the game window as a sheet. Type, then Return (or click OK); Esc or Cancel backs out.
- **Gamepad**: an on-screen keyboard appears over the lower part of the picture, drawn in the game's style with its own fonts. D-pad or left stick moves, A types the highlighted key, B deletes (on an empty box it cancels), X switches between lower case, one capital and caps lock, Y types a space, + confirms and − cancels. The hint line at the bottom names the buttons of the controller you are using (Switch letters, Xbox letters by position, or PlayStation symbols). The amount keyboard is a number pad with Half and Max.
- **Switching midway**: press a key or click while the on-screen keyboard is up and it hands over to the sheet with what you typed; press a gamepad button while the sheet is up and the on-screen keyboard comes back, also keeping the text.
- **The game's rules apply in both**: a name is 2 – 15 letters (or Chinese characters) with at most one `-` or `_`, not at either end, and OK stays greyed until it fits; an empty name gets a random one, as with the game's own button. An amount can't go above what you have.
- While either is open the game waits, as on the Switch; quitting the App answers Cancel first, so the save still goes through.

## Window and Options

The menu bar has View → Full Screen (⌃⌘F), Quit (⌘Q) and, under Help, the key reference. The window can be resized freely. In game the picture fills the window or screen at its own aspect ratio (on a 16:10 MacBook both windowed and full screen are filled, with the interface along the bottom, centred while a large panel is open); the front-end pages such as the main menu and character select stay 16:9 with bars above and below.

Options has five tabs: **Video**, **Audio**, **Gameplay**, **Settings** and **Extras**. **Picture settings** are on the **Video** tab, in the **Display** and **Graphics** sections; the features this port adds (language, light beams and the rest) are on **Extras**. **Default** resets only the tab you are on. The Options pages have the same roomy rows as on iPhone, with **Apply**, **Default** and **Discard** buttons at the bottom. Click them; with a controller, press the button shown under each one (on an Xbox controller X = Apply, Y = Default, B = Discard; the Switch and PlayStation controllers get their own). Back (B, or Esc) on this page is Discard: unapplied changes are dropped and the page closes, without the "apply or discard?" prompt.
- **Display Mode** (Windowed / Fullscreen) switches as soon as you press Apply, and the App opens that way next time. Switching with View → Full Screen (⌃⌘F) updates the setting too.
- **Resolution** (1600 × 900 / 1920 × 1080 / 2560 × 1440 / Native) is the size the game renders at; on a screen narrower than 16:9 the height follows the screen. *Native* is your main screen's own width in pixels (up to 3840): the interface and text are then drawn at the screen's resolution and never enlarged. It takes effect the next time the App starts, and the page says so until then.
- **Color** (Vivid / Accurate): *Accurate* tags the picture as sRGB, the color space the game was made in, so macOS maps it to your display; on the wide-gamut (P3) screens of recent Macs *Vivid* shows the same values stretched to the display's wider primaries, which is how the App looked before. The picture itself (and every screenshot) is the same either way. It changes as soon as you press Apply.
- **Upscaling** (Standard / MetalFX) is how the picture is enlarged to the window or screen: Standard is a plain stretch, MetalFX is Apple's sharper upscaler. On an M6 with a 2560 × 1440 screen the enlarge step takes about 1.4 ms of GPU time per frame from 1600 × 900 and 1.8 ms from 1920 × 1080, against 0.3 ms for the plain stretch. It changes as soon as you press Apply.
- **Anti-Aliasing** (FXAA / SMAA) picks the game's own edge smoothing. The Switch build uses FXAA; SMAA is the sharper mode the game offers on its PC presets, and the App switches the game's renderer to it, with the game's own SMAA passes. It costs about 0.5 ms of GPU time per frame on an M6. (The PC game's temporal SMAA T2x needs a motion pass the Switch build never sets up, so it isn't offered.)
- **Anisotropic Filtering** (Original / 4x / 8x / 16x) keeps ground textures sharp as they recede from the camera. *Original* uses the filtering the game asks for; the other settings raise it on the game's trilinear textures only (never on shadows or the interface). 16x costs about 0.2 ms of GPU time per frame on an M6.
- **Environment Shadows** (Off / On): on the Switch almost all scenery (trees, walls, fences, buildings) casts no shadow; only characters do. *On* lets the scenery cast shadows too, as on PC. It costs about 0.5 ms of GPU time per frame in town on an M6 and is on in Recommended. Models take it when they load, so it takes effect the next time the App starts, and the page says so until then.
- **Shadow Quality** (Standard / High / Ultra) sets how fine shadows are, on characters (their self-shadowing) and on the ground. *Standard* is the Switch's own shadow map (2100 pixels a side); *High* (3072) and *Ultra* (4096) are the sizes the game uses on PC. Ultra is in Recommended: on an M6 its shadow passes take about 1 ms more GPU time than the old map, and the frame stays at 60 fps. It takes effect the next time the App starts, and the page says so until then.
- **Frame Rate** (60 fps / 30 fps): 30 fps is the power saver mode. It changes as soon as you press Apply.
- **Frame Drops** (Lower 3D Res / Keep 3D Res): with *Lower*, the game lowers its 3D resolution on its own when frames drop and brings it back up afterwards; *Keep* always renders at the full resolution. The Mac defaults to *Keep*: its GPU has headroom, and its rare drops are short stutters a lower 3D resolution doesn't fix (phones default to *Lower*).
- **3D Resolution** (100% / 85% / 75% / 67% / 50%) lowers only the 3D world; the interface and text stay at the Resolution above. The game's own 3D upscale step brings the world back to full size, and with MetalFX upscaling MetalFX does that step. It goes with Native: at 75% on a 2560-wide screen the world is drawn at 1920 while the interface stays at 2560. It changes as soon as you press Apply.
- **Graphic Presets**: *Recommended* is what the App picks for this Mac: full screen, Native with the 3D Resolution chosen for the chip (the world at up to 1920 pixels wide on base chips, 2560 on Pro, full size on Max and Ultra), MetalFX, SMAA, 16x filtering, Accurate color, Environment Shadows and Ultra shadows. On a screen too wide for the chip (over 2560 pixels on a base chip, 3456 on a Pro) it keeps the highest of the three fixed resolutions up to three quarters of the screen's width instead. A Mac on an iPhone chip (the MacBook Neo's A18 Pro) gets what the iPhone runs at 60 fps: the world at up to 1720 pixels wide (67% on the Neo's screen), no environment shadows, Standard shadows, and Frame Drops set to lower the 3D resolution for a while when frames drop. If you were on Recommended before this version, the App moves you to the new Recommended; settings you changed stay. *Performance* is 1600 × 900 with FXAA, no environment shadows and Standard shadows, and *Quality* Native with the 3D at 100%, SMAA, 16x filtering, environment shadows and Ultra shadows, both with MetalFX. Choosing a preset fills in the rows; changing a row shows which preset matches, or *Custom*.
- **Default** at the bottom of the panel puts the Display rows and the Graphics rows above Frame Rate back to Recommended, Frame Rate to 60 fps and Frame Drops to Keep 3D Res (Lower 3D Res on an iPhone chip); press Apply to keep it.
- The settings live in `sw_video.txt` next to the App's saves (Frame Rate and Frame Drops with the Extras options in `sw_custom.txt`). Delete `sw_video.txt` to have the App work out Recommended again.

## Saves, logs and iCloud

- Saves: `~/Library/Application Support/D2R Mobile/save` (the data folders keep the project's former name, so saves carry over from earlier builds)
- Log: `~/Library/Logs/D2R Mobile/host.log` (the previous run is `host.prev.log`). When reporting a problem use Help → Share Log… in the menu bar: it packs these two logs and a version-information file into one archive and lets you choose where to save it; nothing is uploaded, no saves are included, and the user name in the logs is replaced with `~`.
- No sound: at launch the log has a line `audio：输出设备「…」…，系统缺省输出「…」…` (the output device in use and the system default output; a mismatch is marked red), and on exit a line `声卡回调累计 N` — N = 0 means no sound was played this time. (These log lines are in Chinese.)

**iCloud save sync: off in the App the launcher installs and by default in an App you build; a paid developer account can turn it on in a build of your own.** The program has the same sync as on iPhone / iPad wired in, but it needs the App to carry the iCloud signing entitlement. By default the App is signed locally without it, so saves stay on this Mac, the log records one line saying iCloud is unavailable and only local saves are used, and the game runs as usual. With a paid developer account, download a Mac provisioning profile that carries iCloud, put the `.provisionprofile` into `~/dm-work/mac-sign` (or point `DM_MAC_SIGN_DIR` at its folder) and build with `DM_MAC_ICLOUD=1 dm/tools/mac_app.sh`: the script reads the team, bundle identifier and entitlements from the profile and signs with the matching certificate in your keychain. The bundle identifier in the profile decides the iCloud container, so a profile for your iOS App's identifier syncs with your iPhone / iPad. Contributed by [@xielh96](https://github.com/xielh96) ([#121](https://github.com/jhaizhou-ops/d2r-mobile/pull/121)). Without a paid account, bring your phone's progress over by importing it by hand as below.

### Importing saves from an iPhone / iPad

Importing is a one-time manual copy: afterwards each side goes its own way and progress diverges; to bring them together later you copy the whole thing again and let one side overwrite the other.

**1. Find the save on the iPhone / iPad**

- Quit the game completely on the phone / tablet first (swipe it away in the app switcher).
- Open the built-in Files app → "On My iPhone" (or "On My iPad") → "D2R Metal" → the `save` folder inside is the save.

**2. Copy the whole `save` folder, do not pick files**

It holds one folder per character (folder name = character name), a few shared-stash files and a `Settings` folder (game settings including the interface language — after copying, the Mac's game settings become the phone's). The switches on the Extras page are not in it; each device keeps its own.

AirDrop, a cable (the Files tab in Finder) or iCloud Drive all work for getting it to the Mac.

**3. Put it on the Mac**

1. Quit the game on the Mac completely (⌘Q).
2. In Finder press ⇧⌘G, enter `~/Library/Application Support/D2R Mobile` and press Return. (This folder exists only after the game has been started once.)
3. **Back up first**: rename the existing `save` folder there (for example `save backup`); do not delete it.
4. Put the `save` folder from the phone here. Correct looks like this: directly under `D2R Mobile/save/` are the character folders, `Settings` and a few `.d2i` files — no extra level (not `save/save/…`).
5. If `save.work`, `save.new` or `save.prev` sit next to it, leave them; the game manages them itself (it rebuilds the first two at launch, and `save.prev`, the previously saved version, is replaced at every save). A `save.work.orphan` folder holds the unsaved progress of a run that ended before it could save (kept aside, never put back into the save automatically); it can be deleted once you no longer need it.
6. Open the game; the phone's characters should appear on the main menu.

Got it wrong or want to go back: quit the game, take the `save` you just put in away, and rename the one from step 3 back to `save`.

The other direction (Mac → iPhone / iPad) uses the same files and mirrors the steps: quit the game on the Mac, copy the Mac's `save` into the game's folder in Files on the phone, replacing the one there (rename the phone's original first as a backup). Keep the App on both sides as current as you can.

Verified: on a Mac, a save placed in the save directory laid out as in step 4 shows its characters on the main menu after launch and can enter a game (one unattended run). The whole route from a real iPhone / iPad through the Files app onto the Mac, the other direction, and hardcore characters have not been operated by a person.

## Quitting and saving

Closing the window, ⌘Q and Quit in the menu all save before quitting, in any state: in game, with the pause menu open, with a panel open, and on the loading screen were each verified with one run. It waits at most 3 seconds; if saving has not finished by then, the log records a line saying it quit without saving.

After a forced quit or a crash, the system's "reopen windows?" dialog no longer comes up to block the game window on the next launch.

## Known shortcomings

- Not tried by hand yet: the resource-folder dialog on a fresh install, and PlayStation or Switch controllers on the Mac (an Xbox Wireless Controller is what has been used).
- The keys can't be rebound yet: the game's own Key Bindings page opens in the PC interface, but it doesn't take a new key, so the App doesn't offer it. The keys are D2R's PC defaults.
- The PC interface's panels are the console ones (the Switch build's own PC inventory and stash grids take no clicks), drawn a little smaller and higher than on a console so they end above the PC HUD. The panel group's tab bar keeps the tab it opened on when a key (C, T, Q) switches the page.
- Click-to-walk and clicks on monsters were verified in the camp and the Blood Moor. Clicking a distant monster through a wall may stop the character at the obstacle; step around and click again. The party, message log, help and mercenary keys (P, M, H, O) are untested.
- The Share Log menu item and save panel have not been operated by a person; what was verified is the archive it produces.
- An App you build yourself does no update check (it does not go online), and has iCloud save sync only when built with `DM_MAC_ICLOUD=1` and a paid account's profile (see "Saves, logs and iCloud"). The App the launcher installs checks the project's releases and shows a new version on the main menu, but keeps its saves on this Mac.
