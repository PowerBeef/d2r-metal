# Control Port (v1)

中文：[control_port.zh-CN.md](control_port.zh-CN.md)

`DM_KEYS` scripts are fixed before launch: they cannot react to what the game does. The **control port** lets a test script or an agent drive a running game step by step. It sends one JSON command per line, the host answers each one with one line, and the [event stream](event_stream.md) arrives on the same connection. The game is driven through the same host paths the key script and the front ends already use (controller read, UI messages, the touch tap, the game's own save), so a scenario exercises the real code.

The code is `dm/host/dm_ctl.c` (protocol and transport) and `dm/host/dm_ctl_cmds.c` (commands). `dm/tools/dmctl.py` is the client.

## Connecting

| Where | How |
|---|---|
| Mac (command-line `dmhost`, Mac App) | The controller listens on `127.0.0.1:<port>` and the host is started with `DM_CTL_PORT=<port>`. The host connects to it, retrying every 0.5 s, and reconnects after a drop, so either side may start first. The host itself opens no port: the iOS package check in `ios/run.sh` allows `listen(` only at the log port. Setting `DM_CTL_PORT` also turns the event stream on (no file unless `DM_EVENTS` is set too), and events are pushed to the controller. |
| iOS (device, or the iOS package on a Mac) | The existing log port `DM_LOG_PORT`. The client gets the whole log first, then live lines. Lines it sends that start with `{` are commands. The plain words `quit` and `dump` still work. Set `DM_EVENTS=log` to have the events in that stream. A device is reached through `iproxy <local port> <device port>` (add `-n` over Wi-Fi). |

Lines from the host:

```
[dm] RE {"id":4,"cmd":"press","ok":true,"frame":52,"held_frames":6}
[dm] EV {"seq":14,"t":1.297,"ev":"main_menu"}
```

Everything else on the connection (log lines on iOS) can be ignored.

## Commands

A command is a flat JSON object: `{"id":<number or string>,"cmd":"<name>",<arguments>}`. Values are strings, numbers or `true`/`false`. Arrays and nested objects are refused. The reply echoes `id` and `cmd`, and has `"ok":true` with the results or `"ok":false,"err":"<reason>"`. Each command blocks until its effect has gone through, so a client can send them one after another.

| Command | Arguments | Does | Reply fields |
|---|---|---|---|
| `ping` | | Liveness check. | `pong` |
| `help` | | Lists the command names. | `cmds` |
| `state` | | Snapshot. | `in_game`, `tick`, `paused`, `panel` (most recently opened, not yet closed panel), `level`, `hp`, `hp_max`, `mp`, `mp_max`, `area` (levels.txt id), `x`, `y` (cells), `dead`, `players`, `holding` (item on the cursor), `belt0`–`belt3` (potions left per belt column), `portal` (town portals), `points` (unspent stat + skill points), `exp_permille` (experience progress in the level, ‰), `quest_unread`, `ev_dropped`; from the engine snapshot (`dm_engine.c`): `panels` (visible panels, top first), `focus`, `focus_panel`, `cursor_mode` (1 = a free-cursor panel), `save_busy`, `save_pending` (commit deferred), `save_mounted`, `save_containers` |
| `press` | `keys` (`A+B`, names as in `DM_KEYS`), `frames` (6), `gap` (3) | Holds the buttons for `frames` presented frames, then waits `gap` released frames. Merged at the controller read with the same arbitration as script keys. | `frame`, `held_frames` |
| `stick` | `x`, `y` in [-1, 1] (up is positive), `frames` (30), `keys` | Pushes the left stick (optionally with buttons). | `frame`, `held_frames` |
| `tap` | `x`, `y` in [0, 1] of the rendered picture, `hold` (3 frames) | One UI tap through `dm_ui_touch`, the same path as `DM_KEYS` `C<x>:<y>` and a real finger. | |
| `wheel` | `x`, `y` in [0, 1] of the rendered picture, `dy`, `precise` (1), `view` (720), `n` (1) | One wheel or trackpad scroll through `dm_wheel_ui`, the same function as the Mac App's scroll events: `precise=1` is trackpad points, `0` is mouse-wheel lines; `view` is the view height in points. Positive `dy` moves the content down. `n` > 1 sends that many identical events back to back without waiting for a frame, as a trackpad flick does. Mac only. | `ui` (0 = not over the UI, so the App would push the right stick instead) |
| `drag` | `x0`, `y0`, `x1`, `y1` in [0, 1] of the rendered picture, `frames` (20) | Presses, drags in a straight line over `frames` frames and releases. In the Mac App it goes through the real mouse's gesture path (`dm_touch_host_live`), elsewhere through `dm_ui_touch`. | |
| `msg` | `spec` = `category:action[:argument]` | Sends one UI message into the game (as the `DM_KEYS` `MSG:` step); replies once it has gone out. | |
| `world_tap` | `x`, `y` in [0, 1] | Taps that spot in the world: walks over and interacts with what is there, like a touch tap. | `target`: `none`, `item`, `npc`, `object`, `exit` |
| `walk_to` | `x`, `y` (cells, as in `state`), `timeout` (12 s) | Walks there with the route planning of `DM_TEST_WORLD_XY`. Within 2 cells counts as arrived. | `arrived`, `x`, `y` |
| `tap_ui` | `name` (`widget[@panel]`, `+` = space), `timeout` (10 s) | Waits until the widget is visible and taps its center, like the touch script's `tap_ui`. | `x`, `y` |
| `await_ui` | `name`, `timeout` (30 s) | Only waits until the widget is visible. | `x`, `y` |
| `tap_unit` | `type` (1 monster / NPC, 2 object, 4 item, 5 exit), `cls` (class number or `any`), `timeout` (10 s) | Waits until such a unit is on screen and taps it, like the touch script's `tap_unit`. | `x`, `y`, `target` |
| `app` | `do`: `background`, `foreground`, `resign`, `active`, `minimize`, `restore`, `terminate` | Front-end lifecycle on the main thread, through the same handlers as the system events and `DM_TEST_BG` / `DM_TEST_MINI_AT`. On the Mac, `background` / `foreground` are minimize / restore and `resign` / `active` are not supported; `terminate` saves and quits as the system quit does. | |
| `screenshot` | `path`, `frames` (1) | Writes a PNG of the game output after `frames` presented frames. The path must end in `.png` and contain no `..`. On iOS it must be relative and lands under `Documents/`; on the Mac its directory must exist inside the run folder (the `DM_EVENTS` directory, the parent of `DM_APP_SUPPORT`, the current directory, `$DM_ROOT/run`) or a temporary directory. | `path` |
| `ui_dump` | `want` (`@top`, or a widget name) | Prints the UI widget tree into the log on the next frame (`UIDUMP` lines, as `DM_UI_DUMP`). | |
| `pause` | | Freezes game ticks (the `DM_TICK_HOLD` freeze). Needs a run that has ticked with a control channel on. Frozen longer than `DM_WATCH_STALL`, the watchdog gives up. | `tick` |
| `step` | `n` (1) | While paused: lets exactly `n` ticks run, then freezes again. | `tick` |
| `resume` | | Unfreezes. | `tick` |
| `mark` | `name` | Puts a `mark` event (with the current frame number) into the event stream; benchmark routes use it to cut segments. | `t`, `frame` |
| `wait` | `frames`, `ticks` or `ms`; `timeout` (60 s) | Waits on the host side. | `frame`, `tick` |
| `save_now` | `timeout` (5 s), `wait` (1) | Asks the game to save along its own timed-save path and waits for the commit (the same `dm_save_wait` the front ends use on quit and background).  With `wait=0` it only asks for a save on the next tick and replies at once (as iOS resign), so a following quit or background lands mid-save. | `commits`, `ms`, `paused` (`queued` with `wait=0`) |
| `engine` | | The whole engine snapshot (`dm_engine.c`): panel stack with `active` and `priority`, `focus_panel`, `focus`, `hover`, `cursor_mode`, `cursor_state`, `save` {`busy`, `pending`, `containers`, `mounted`, `dirty`}, the latest 48 controller `actions` and 48 UI `messages` (repeats folded, `n`). The scenario runner stores it as `engine.json` at the first red event or failed step. | as listed |
| `fault` | `op`: `arm` (`point`, `skip` 0, `count` 1), `delay` (`ms`), `list` | Test faults on saves: `arm` adds a `DM_TEST_FS_FAILAT` point while running (`mountclone`, `reclone`, `guestname`, `renameback`, `guestopen`); `delay` holds the next save transaction's first write `ms` milliseconds (as `DM_TEST_SAVE_DELAY`, once). Keep delays under ~8 s: the game's async job pool runs out of task memory and crashes in the pool's enqueue function (main+0x14f0590) while one job is stuck longer. | `faults` [{`point`, `skip`, `count`, `seen`, `fired`}] |
| `dump` | | Writes what each thread is waiting on into the log. | |
| `crash` | `kind`: `segv`, `abort`, `trap` | Crashes the host on purpose on the control-port thread (tests the crash path: event, `crash.json`, `.ips`, red verdict). | |
| `quit` | `save` (0) | Replies, then exits gracefully (`dm_host_shutdown`). With `save=1` it first saves as closing the Mac window does. | `saved` |

`state.tick` and the events' `tick` field come from the server tick; `frames` count presented frames, which keep advancing on the front end before a run.

## Python client

```python
import sys; sys.path.insert(0, 'dm/tools')
import dmctl
c = dmctl.Ctl(47110, listen=True, log='port.log')   # Mac host started with DM_CTL_PORT=47110; for iOS: Ctl(<local iproxy port>)
c.wait_for('main_menu', timeout=60)         # events already seen count; crash / fatal / watchdog raise BadEvent
c.cmd('press', keys='X')
c.wait_for('panel', name='CharacterSelectPanel', act='OpenPanel')
c.cmd('msg', spec='CharacterSelect:LoadCharacter:0')
c.wait_for('loading_done', timeout=60)
print(c.cmd('state'))                       # ok=false raises CtlError
m = c.mark(); c.cmd('press', keys='PLUS'); c.wait_for('panel', after=m, act='OpenPanel', name='PlayerInventoryExpansionLayout')
c.cmd('quit')
```

From a shell: `python3 dm/tools/dmctl.py --listen 47110 state`, `python3 dm/tools/dmctl.py --listen 47110 walk_to x=5780 y=4409`, `python3 dm/tools/dmctl.py --listen 47110 wait loading_done 60` (Mac; leave out `--listen` for an iOS log port). Each call takes the connection for its duration; the host reconnects to the next one.

## Notes

- Both directions use loopback only and have no authentication. Do not forward them beyond the machine.
- Input commands write single request slots that the game thread reads once per frame. Commands wait for their slot to be taken, so two clients issuing input at once would still interleave.
- `msg` counts as sending a message on the player's behalf in `dm/host/tests/mode_scan.py`: it runs the same way in controller and touch mode.
