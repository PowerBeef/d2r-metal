# Scenarios

中文：[README.zh-CN.md](README.zh-CN.md)

A scenario drives a running game step by step through the [control port](../control_port.md) and judges the run by events, state and final checks. Each step waits until its effect has gone through, so there are no timing guesses. `dm/tools/scenario.py` runs them:

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn                     # Mac command-line host (window)
python3 dm/tools/scenario.py run dm/verify/scenarios/*.scn --headless                   # every scenario, no window
python3 dm/tools/scenario.py run dm/verify/scenarios/background.scn --backend ios-mac   # the iOS package on this Mac
python3 dm/tools/scenario.py check dm/verify/scenarios/*.scn                           # parse only
```

Each scenario prints `STEP` lines, `CHECK` lines, a `SCENARIO <name> OK|RED <reason>` verdict and a final `RESULT {json}`. The result folder (`$DM_ROOT/run/scenario/<name>_<backend>_<time>/`) holds `host.log`, `events.ndjson` (CLI host), `port.log`, `steps.tsv`, `result.json` and the screenshots.

## Backends

| Backend | What runs | Control | Needs |
|---|---|---|---|
| `cli` (default) | `dm/host/dmhost` with a copy of the scenario's save | `DM_CTL_PORT` (the host connects to the runner) | `./setup.sh` done, game resources |
| `mac-app` | The Mac App players install (a test copy, `$DM_ROOT/out/macapp_test`, built with `dm/tools/mac_app.sh` when missing; `DM_MAC_APP` points elsewhere), run as a child process | `DM_CTL_PORT` | `DM_APP_SUPPORT` points at the result folder: the save copy, settings and logs live there, iCloud does not sync, and the player's own saves and logs are untouched |
| `ios-mac` | The iOS package on this Mac (`.iosdd_mac/DmMacSmoke.app`, built by `dm/ios/tools/smoke_mac.sh`) | The log port | The same lock, iCloud gate and fresh test save as the Mac smoke test (`dm/ios/tools/scenario_launch.sh`) |
| `device` | The installed app on the device in `dm/ios/local.env` | The log port through `iproxy` | An unlocked device; the device lock |

## File format

One step per line, run in order. `#` starts a comment. Arguments are positional or `name=value`; in widget names `+` stands for a space, as in the touch scripts.

Header lines (anywhere in the file):

| Line | Meaning |
|---|---|
| `name <name>` | Scenario name (default: the file name). |
| `save <name>` | Save to start from: `save_ref_<name>` under `$DM_ROOT/data/switch/` (default `warlock`), `empty`, or an absolute path. The runner always uses a copy. |
| `env K=V …` | Extra environment for the game. |
| `timeout <s>` | Limit for the whole scenario (default 300). |
| `window 0` | Run the CLI host without a window. |
| `requires <backend> …` | Skip on other backends. |

Steps:

| Step | Arguments | Does |
|---|---|---|
| Any control-port command | as in [control_port.md](../control_port.md); positional forms: `press KEYS [frames]`, `stick X Y [frames]`, `tap X Y`, `msg SPEC`, `world_tap X Y`, `walk_to X Y [timeout]`, `screenshot PATH` (alias `shot`), `ui_dump [want]`, `step [n]`, `save_now`, `tap_ui NAME`, `await_ui NAME`, `tap_unit TYPE CLS`, `app DO`, `mark NAME`, `crash KIND`, `quit` | Sends the command; red if the reply is `ok:false`. A relative screenshot path lands in the result folder (CLI) or in `Documents/` (iOS). |
| `expect <event> [field=value …] [timeout=30]` | | Waits for the event since the previous step started (so an event triggered by that step is not missed). |
| `expect_no <event> [field=value …] [for=3]` | | Red if the event appears within `for` seconds. |
| `expect_state field=value …  [timeout=5]` | `field>=n`, `field<=n`, `field!=v` too | Polls `state` until every condition holds. |
| `sleep <s>`, `log <text>` | | |
| `remember <field> [as=name]` | | Keeps a `state` field; later values can say `$name`, `$name+N` or `$name-N` (e.g. `expect_state tick<=$tick+30`). |
| `gpu_capture [frames=1] [path=frame.gputrace] [keep=1]` | | `cli` / `mac-app`: captures GPU frames with `gpucapture` and returns the `gpudebug` summary (command buffers, encoders, draws, dispatches). The trace (about 4 GB a frame) is deleted unless `keep=1`. The run gets `MTL_CAPTURE_ENABLED=1` automatically. Capturing stalls presentation, so add `check no_red allow=e6db5cb6`. |
| `export_save <name> [overwrite=1]` | | `cli` / `mac-app`: saves, waits for the commit and copies this run's committed save to `$DM_ROOT/data/switch/save_ref_<name>/` (local only, never committed). `warlock` is never replaced. |
| `enter_game [slot=0]` | | From the main menu into a run: `X` on the main menu, load the character in `slot`, wait for `loading_done`. Does nothing when already in a run. |
| `open_pause`, `close_pause` | | `+` opens the combined panel and `L` three times reaches the pause page (`pause_open`); `B` closes it. |
| `background [for=3]` | | To the background and back: iOS scene events as `DM_TEST_BG`; on the Mac, minimize and restore. |
| `system_quit` | | Quits through the system path (Mac `[NSApp terminate:]`, iOS `applicationWillTerminate`: save, then quit) and waits for the process to end. |
| `swipe_away` | | iOS: resign active, then kill the process (swiping it away in the app switcher). CLI: kill. |

Final checks (`check …`, evaluated after the run):

| Check | Passes when |
|---|---|
| `check no_red [allow=code,…]` | No `red`, `fatal`, `crash` or `watchdog` event (except listed red codes). |
| `check no_red_log [allow=regex]` | No red-marker line (`【红】`, `🔴`, `FATAL`, `CRASH`, watchdog) in the game log (`host.log` / `port.log`), including lines that bypass the event stream such as the iOS shell's `plog`. `allow` lets a known line through. |
| `check saved` | At least one save was committed. |
| `check exit <code>` | The process exit code (CLI). |
| `check event <event> [field=value …]` / `check no_event …` | Such an event appeared / did not appear. |

## Generated saves

A scenario can start from a save that another scenario produced. When `save <name>` names a save that does not exist and `saves/<name>.scn` does, the runner first runs that generator on the CLI host; it ends with `export_save <name>`. Example: `saves/belt_two_drunk.scn` drinks two belt potions and exports `save_ref_belt_two_drunk`, and `belt_persists.scn` checks that a new run reads that belt back. The iOS backends use the saves bundled into the app at build time, so such scenarios say `requires cli`.

Touch-script actions map onto steps: `tap_ui`, `await ui` (`await_ui`), `walk_to`, `tap_unit`, `shot`, `pad` (`press` / `stick`). Raw finger lines (`down` / `move` / `up` at fixed times) stay in `DM_TOUCH_SCRIPT`, because they exercise the touch overlay itself.
