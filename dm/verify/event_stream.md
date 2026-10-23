# Event Stream Format (v1)

中文：[event_stream.zh-CN.md](event_stream.zh-CN.md)

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
| `pause_open` | | `OpenPanel PauseLayout`. |
| `game_leave` | | `FrontEndNavigation ToFrontEnd` after a game. |
| `nav` | `to` | Any `FrontEndNavigation` message. |
| `panel` | `act` (`OpenPanel`, `ClosePanel`, `UnloadPanel`, …), `name` | Any `PanelManager` message that names a panel. |
| `save` | `result`: `commit` (`n`, `writes`, `commit_us`, `txn_us`), `rejected` (`errno`), `fail` (`errno`) | A save transaction commit (`hle_fs.c`): committed, refused because a write in the transaction failed, or the clone / swap failed. |
| `red` | `code`, `src`, `msg` | A log line carrying `【红】` or `🔴`. `code` is an 8-hex-digit hash of the log call's format string, so it stays the same when the code around it moves; `src` is `file:line`. |
| `fatal` | `code`, `src`, `msg` | A `FATAL` log line (`die()`, `MDIE`). The process aborts right after. |
| `crash` | `sig`, `pc`, `where` (`module+offset`, only inside the game's modules), `addr` | The crash handler (`crash.c`), written before the register dump. |
| `watchdog` | `stalled_s`, `stage` | The watchdog (`DM_WATCH_STALL`) gave up: no progress for that many seconds. |
| `app` | `state` | Shell lifecycle. iOS: `resign`, `background`, `foreground`, `active` (the scene callbacks). Mac: `resign`, `active` (window key state), `minimized`, `restored`, `occluded`, `visible`. |
| `quit` | `why` | Mac: the window / app quit path started. |
| `quit_save` | `saved`, `commits`, `ms`, `paused` | Mac: the save wait before quitting finished. |
| `shutdown` | `why` | `dm_host_shutdown` started (SIGTERM, iOS terminate / scene disconnect, Mac quit). |
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
