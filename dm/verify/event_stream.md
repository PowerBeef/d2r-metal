# Event Stream Format (v1)

The host's log is written for people, in Chinese, and its lines change wording over time. Test scripts and agents need something steadier: the **event stream** is a second, structured channel next to the log. It records the moments a run is judged on (main menu shown, game entered, loading screen gone, save committed, red marker, crash, app sent to the background…), one JSON object per line. The log itself is unchanged.

The code is `dm/host/dm_events.c` (`dm_events.h` for the API); `dm/tools/dmevents.py` reads and checks a stream.

## Switches

| Variable | Effect |
|---|---|
| `DM_EVENTS=<file>` | Write the events to this file (truncated at start). On iOS a relative path lands under the app's `Documents/`. |
| `DM_EVENTS=log` | No file: each event is printed as one log line `[dm] EV {json}`. On iOS the line travels with the log over the log port, so `ios/tools/logport.py` sees it. |
| `DM_EVENTS_OSLOG=1` | Also send each event to the system log (subsystem `org.d2rmobile`, category `events`, logged `%{public}`), visible in Console or `log stream --predicate 'subsystem == "org.d2rmobile"'`. |

Without any of them the event calls cost one pointer check each, and `LOG` behaves exactly as before.

## Line format

UTF-8, one object per line, terminated by `\n`, written with a single `write()` so lines from different threads never interleave:

```
{"seq":14,"t":1.247,"ev":"main_menu"}
{"seq":53,"t":4.380,"ev":"loading_done","tick":16}
```

- `seq`: increases by one per event, in file order. A `crash` event takes no lock and may share its `seq` with the next line.
- `t`: seconds since the process was created, truncated to milliseconds. This is the same zero point as the `[1.25s]` stamps on `UIMSG` log lines and the boot timeline (`DM_BOOTPROF`).
- `ev`: the event type (below).
- `tick`: the server tick, added automatically while a run is live.
- Other fields depend on the type. Strings are JSON-escaped and cut at 400 bytes on a UTF-8 character boundary.

## Event types

| `ev` | Fields | When |
|---|---|---|
| `start` | `v` (format version, 1), `pid`, `platform` (`mac`, `ios`), `wall` (Unix time) | The stream is opened (first line). |
| `boot` | `stage`: `host_loaded` (`mods` = module count), `module_inits`, `nnmain`, `front_end` | Boot stages: modules loaded and relocated, module init arrays done, entering `nnMain`, the game's first `FrontEndNavigation ToFrontEnd`. |
| `main_menu` | | `PanelManager OpenPanel MainMenuButtonRibbon` (every time it opens). |
| `game_enter` | | `FrontEndNavigation ToGame`. |
| `loading_done` | | The loading screen is unloaded after `game_enter` (`UnloadPanel LoadScreenPanel`). |
| `pause_open` | `name` | `OpenPanel PauseLayout…` (in a game the page is `PauseLayoutGarden`). |
| `game_leave` | | `FrontEndNavigation ToFrontEnd` after a game. |
| `nav` | `to` | Any `FrontEndNavigation` message. |
| `panel` | `act` (`OpenPanel`, `ClosePanel`, `UnloadPanel`, …), `name` | Any `PanelManager` message that names a panel. |
| `save` | `result`: `commit` (`n`, `writes`, `commit_us`, `txn_us`), `rejected` (`errno`), `fail` (`errno`) | A save transaction commit (`hle_fs.c`): committed, refused because a write in the transaction failed, or the clone / swap failed. |
| `red` | `code`, `src`, `msg` | A log line carrying `【红】` or `🔴`. `code` is an 8-hex-digit hash of the log call's format string, so it stays the same when the code around it moves; `src` is `file:line`. |
| `fatal` | `code`, `src`, `msg` | A `FATAL` log line (`die()`, `MDIE`). The process aborts right after. |
| `crash` | `sig`, `pc`, `where` (`module+offset`, only inside the game's modules), `addr` | The crash handler (`crash.c`), written before the register dump. |
| `watchdog` | `stalled_s`, `stage` | The watchdog (`DM_WATCH_STALL`) gave up: no progress for that many seconds. |
| `app` | `state`, `cause` (Mac `resign`: `sheet`, `app`, `minimize`, `window`), `emu` (Mac) | Shell lifecycle. iOS: `resign`, `background`, `foreground`, `active` (the scene callbacks). Mac: `resign`, `active` (window key state), `minimized`, `restored`, `occluded`, `visible`. `cause` says why the Mac window lost key status: its own sheet, another app taking the front, minimize, or another window. `emu` = emulated by a background test run (launch focus, sheets, minimize / restore, the virtual test cover). |
| `window` | `cw`, `ch` (content, pt), `px`, `py`, `pw`, `ph` (picture rect, pt), `dw`, `dh` (drawable, px), `scale`, `full`, `emu`, `fit`, `active`, `key` | Mac: the game window's layout changed (`front_mac.m` `layout_game`, beside the `窗口：内容区` log line). `full` = full screen, `emu` = emulated by a background test run, `fit` = the content equals the screen's full-screen content area; `active` / `key` are AppKit's real states. |
| `desk` | `what` (`front`, `back`, `other_app`, `space`, `screens_sleep`, `screens_wake`, `session_off`, `session_on`; in a background test run also `covered`, `uncovered`: the off-screen window's real occlusion, which is not handled), `by` (`self` or the other app's bundle ID) | Mac: the desk changed around the game, from `NSApplication` / `NSWorkspace` notifications (no polling, no screen reads): this app came to the front or left it, another app activated, the Space changed, the displays slept or woke, the login session went inactive or came back. Test harnesses judge front runs by it. |
| `sheet` | `result` (`ok`, `cancel`, `pad`), `len` | Mac: the text sheet (name / quantity box) closed: confirmed, cancelled, or handed over to the on-screen keyboard; `len` = characters in its field (never the text). |
| `quit` | `why` | Mac: the window / app quit path started. |
| `quit_save` | `saved`, `commits`, `ms`, `paused` | Mac: the save wait before quitting finished. |
| `save_request` | | The host asked the game to save (`dm_request_save`: `save_now`, quit / background saves, `DM_TEST_SAVE_AT`); starts the `save_stuck` timer. Not emitted again while an earlier request is still unanswered. |
| `save_stuck` | `why` (`no_commit`, `mounted`), `s`, `busy`, `pending`, `mounted`, `containers` | Red (`dm_engine.c`, with a `【红】` line): a requested save got no commit or rejection within `DM_SAVE_STUCK_S` seconds of unpaused play (20), or a save container stayed mounted `DM_SAVE_MOUNT_STUCK_S` seconds (60). |
| `save_slow` | `op` (`write`, `commit`), `ms` | One write or one commit on the save mount took at least `DM_SAVE_SLOW_MS` (1000) on the host side; from 4 s also a `【红】` line. A save job stalled about 7.5 s exhausts the game's async job memory and crashes it, at 30 and 60 fps alike. |
| `save_alert` | `errno` | The player is told a save was not written (`front_save_failed`): a refused commit, a failed clone / swap, a failed mount-time clone. |
| `fault` | `point`, `n` (or `ms` for `savedelay`) | A test fault fired (`DM_TEST_FS_FAILAT`, `DM_TEST_SAVE_DELAY`, the control port's `fault`): `mountclone`, `reclone`, `guestname`, `renameback`, `guestopen`, `savedelay`. |
| `action` | `id`, `name`, `phase` (`begin`, `end`) | A controller action reached the UI bus (`InputMessage` `ControllerActionBegin` / `End`); `name` is read from the game's own action table at run time (`select`, `back`, `pause`, `cycleLeft`, …; null if unreadable). Only while a panel takes input: menus, not world play. |
| `language` | `name` (`enUS`, `esES`, `zhCN`, …), `idx` | The game switched its text language (SetLanguage; at boot from the settings file, or from the options page). |
| `opmode` | `from`, `to` (1 docked, 0 handheld) | The operation mode changed (`dm_set_opmode`: the front end's overheat downgrade, `DM_OE_SCRIPT`); the game is notified through 30 / 31. |
| `padmode` | `mode` (`controller`, `touch`), `glyph` (`Switch`, `keyboard`, `Xbox`, `PlayStation`), `frames`, `timeouts` | A controller / touch layout or key cap switch finished (`dm_padmode.c`); `timeouts` > 0 means a UI rebuild was not seen in time. |
| `pc_item` | `btn` (`left`, `right`), `mods` (1 Shift, 2 Ctrl), `item` (its code, empty for none), `at` (where it is: 1 inventory, 2 belt, 3 worn, 4 stash, 5 cube, 6 vendor), `did` (`pick_up`, `put_down`, `equip`, `unequip`, `belt`, `move` (the console's held Y), `drop` (Y), `cube` (X), `take_stack`, `use`, `buy`; nothing done: `nothing`, `no_item`, `none_here`, `wearable`) | PC control mode: a click on an item area (`dm_kbm.c` `item_click`, the game's own PC item rules). |
| `focus` | `panel`, `widget` | The focused widget changed (`dm_engine.c`, the game's focus manager); null = no focus. |
| `shutdown` | `why` | `dm_host_shutdown` started (SIGTERM, iOS terminate / scene disconnect, Mac quit). |
| `shot` | `path`, `ok` | A screenshot file was written (`DM_NVN_SHOT_UI`, the control port's `screenshot`). |
| `tick_hold` | `on` | The control port froze (`pause`, `step`) or released game ticks. |
| `ctl` | `state` (`connected`) | A control-port client connected (`DM_CTL_PORT`). |
| `game_state` | `state`: `Menu`, `Loading`, `Playing`, `Paused`, `Background` | The game state changed (from the UI messages and the shells' background events). With `DM_STATEREP=1` the same states go to the system's StateReporting (`GameState` domain). |
| `mark` | `name`, `frame` | The control port's `mark` command (benchmark segments); `frame` is the frame number of the `DM_NVN_TSV` timing rows. |
| `break` | `tick`, `pid`, `wait_s` | `DM_BREAK_AT_TICK` reached its tick: the tick thread waits up to `wait_s` seconds for a debugger to attach to `pid` (`dm/verify/debugging.md`). |
| `perf` | `fps`, `n` (frames with an on-screen interval), `c25` (intervals of 20.8-29.2 ms), `c33` (over 29.2 ms), `disc` (not shown: -1), `none` (no reading: 0), `blind` (`none` > `n` + `disc`: the present times were lost), `shown` (`n` > `none` + `disc`: mostly frames on screen), `scale` (the 3D scale in effect), `ingame` | Every 10 s from the present thread (`diag.c` `dm_perf_present`), whatever `DM_PERF_FILE` says: the actual present intervals of the last window, read 8 presents late. On phones the same window also goes to `perf.txt` and the `[perf]` log line. The first window after a return from the background still holds frames from before it. |
| `scale3d` | `from`, `to` (the Frame Drops level of the previous rebuild and of this one; `to` = `lv`), `lv`, `scale` (the 3D scale applied), `pct` (`scale` × 100, rounded), `tw`, `th` (3D target size), `ow`, `oh` (output size), `r10`, `r60` (percent of frames counted as drops over the last 10 s / 60 s behind the Frame Drops step this rebuild applies; 0 when no step is behind it: the boot rebuild, a new 3D Resolution, Keep. `r60` stays 0 until the controller holds 60 s of windows, so a step by the 10 s rule often reads `r60:0`) | After each rebuild of the 3D targets (`hle_oe.c` `scale3d_tick`): a Frame Drops step, a new 3D Resolution, or the first application at boot of a configured scale below 1.0. Steps go one level at a time, so `check no_event scale3d lv=1` checks that Frame Drops never stepped down. The levels are up to three steps of ×0.93 below the configured scale, never below 0.50 (`dm_s3lv.h`): 0.86 / 0.80 / 0.75 at 1.0 (the first ×0.93 step is skipped at 95% or more), 0.70 / 0.65 / 0.60 at 0.75, none at 0.5; `DM_3D_SCALE_FLOOR` allows deeper ones for sweeps. |
| `mfx` | `who` (`pres` the present set, `grab` the screenshot set, `3d` the 3D-pass set), `op` (`build`, `drop`, `fail`, `content`); `build`: `why` (`first` no set installed, `output` another output size or format, `alloc` another input allocation, `retry` the first success after a failure), `n` (the set's builds so far), `iw`, `ih` (input allocation), `ow`, `oh` (output), `ms` (build time); `drop`: `why` (`replace`, `off` Upscaling turned off, `idle` unused), `iw`, `ih`, `ow`, `oh`; `fail`: `why` (`scaler`, `textures`: the step that failed), the requested `iw`, `ih`, `ow`, `oh`; `content`: `k` (content changes so far), `cw`, `ch` (the content now encoded), `iw`, `ih` (the set it sits in) | A MetalFX scaler set was built, put down or could not be built (`nvn_mtl.m` `mfx_ready` / `mfx_drop`), or the 3D pass encoded new content inside the same set, a Frame Drops step that needed no rebuild (`mfx3d_encode`). A build whose `why` is `alloc` or `output` at a Frame Drops step is the regression `mfx3d_dyn.scn` guards. |
| `picture` | `how` (`first`: no key in `sw_video.txt`; `read`; `filled`: missing or out-of-range keys filled; `update`: an older `rec_v`, worked out again; `rescreen`: another main screen or GPU (`rec_id`), worked out again; `unreadable`: the file could not be read, this launch uses the recommendation and leaves it alone; `newer`: written by a newer version, not rewritten; `apply`), `mode`, `res`, `up`, `aa`, `aniso`, `color`, `s3d`, `envsh`, `shq` (the values in effect), `rec_v`, `rec_id` (saved with the recommendation; -1 = none) | `dm_video.c` loaded the picture settings (once per launch, at the first read), or Apply on the Options Video tab saved a change. The CLI host and Mac test copies without `DM_TEST_VIDEO_FIRSTBOOT=1` read `first` with `mode` / `res` / `up` 0 (the original look). |
| `video_row` | `row` (`mode`, `res`, `color`, `preset`, `s3d`, `upscale`, `aa`, `aniso`, `envsh`, `shq`, `fps`, `drops`), `value` (the option picked, as the stored value: for `preset` 0 Recommended, 1 Performance, 2 Quality), `preset` (what the Graphic Presets row shows afterwards: 0 Recommended, 1 Performance, 2 Quality, 3 Custom) | The player changed a selector on the Options Video tab (`dm_video.c` `dm_video_pump`; pending until Apply). Not sent when the page repaints itself. |
| `gpuheap` | `used_mb` (bytes in live blocks), `top_mb` (where the highest live block ends: the first-fit high-water mark), `gap_mb` (the largest free gap: the largest allocation that still fits), `pool_mb` (768, the rewriter's budget: the positive control), `blocks`, `peak_used_mb`, `peak_top_mb`, `min_gap_mb` (the launch's extremes), all MiB | GPU heap 0, which holds every render target, depth and display texture and never grows (`dm_gpuheap.c`, read once a second on the game thread): the first reading, then whenever used, top or the largest gap moved by 4 MiB or more. A largest gap under 64 MiB also logs one red line (the next large target may not fit, and a failed allocation crashes the game). Readings that do not match the expected layout (pool not 768 MiB) are turned off with a log line, and no event follows. |
| `exit` | `why` (`exit`, `ticks_full`, `watchdog`, `shutdown`, `nnMain returned`), `code` | The process is about to end. Paths that use `_exit` emit it themselves; `exit()` goes through `atexit`. |

Only lines that reach `LOG` / `MLOG` (and `die` / `MDIE`) become `red` / `fatal` events. A marker in a format string or in a `%s` argument both count. Lines printed another way, such as the iOS shell's `plog`, are not seen.

## Reading a stream

```bash
python3 dm/tools/dmevents.py summary events.ndjson                    # timeline, counts, bad events; last line RESULT {json}
python3 dm/tools/dmevents.py expect events.ndjson main_menu game_enter loading_done --no-red --within 60
python3 dm/tools/dmevents.py why events.ndjson                        # first crash / fatal / watchdog, else first red
python3 dm/tools/dmevents.py follow events.ndjson loading_done --timeout 90   # wait while the file grows
```

All four also accept a log that holds `[dm] EV` lines (from `DM_EVENTS=log`). `expect` prints an `EVENTS OK|RED` verdict line, so `dm/tools/dmtest.py` picks it up.

## Who uses it

- `ios/tools/logport.py` (used by `smoke.sh`, `smoke_mac.sh`) reads the milestones from event lines when the app sends them. With older builds it falls back to the `UIMSG` text. Both smoke runners set `DM_EVENTS=log`.
- `verify/touchchain.sh` writes `events.ndjson` into the run directory and, when the host fails, prints `dmevents.py why` as the reason.

## Adding an event

Call `DM_EV("type", "name:t name:t …", values…)` from host C or Objective-C code that includes `dm.h` (or `dm_events.h`). Type letters: `s` string, `d` int, `l` long long, `u` unsigned long long, `x` hex string, `f` double, `b` boolean. Give the event an English type name, add it to the table above, and keep the existing Chinese log line next to it.
