# 控制口（v1）

English: [control_port.md](control_port.md)

`DM_KEYS` 按键脚本在启动前就定死了，跟不上游戏的实际反应。**控制口**让测试脚本或代理一步一步驱动正在跑的游戏：每行发一条 JSON 命令，宿主每条回一行，[事件流](event_stream.zh-CN.md)走同一条连接过来。游戏经宿主已有的路径驱动（取手柄、界面消息、触屏点击、游戏自己的存盘），与按键脚本和前端用的是同一批函数，所以场景测的是真实代码。

代码在 `dm/host/dm_ctl.c`（协议与传输）和 `dm/host/dm_ctl_cmds.c`（命令）；客户端是 `dm/tools/dmctl.py`。

## 连接

| 在哪 | 怎么连 |
|---|---|
| Mac（命令行 `dmhost`、Mac App） | 控制端在 `127.0.0.1:<端口>` 上监听，宿主带 `DM_CTL_PORT=<端口>` 起：宿主往那边连，每 0.5 秒重试，断了自己重连，谁先起都行。宿主自己不开端口：`ios/run.sh` 的出包核对只许日志端口一处 `listen(`。设了 `DM_CTL_PORT` 事件流也随之打开（除非另设 `DM_EVENTS`，否则不写文件），事件推给控制端。 |
| iOS（真机，或 Mac 上跑的 iOS 包） | 已有的日志端口 `DM_LOG_PORT`。客户端先收到整份日志，然后是新行。客户端发来的以 `{` 开头的行是命令；纯文本 `quit` 与 `dump` 照旧可用。要在这条流里收事件，设 `DM_EVENTS=log`。真机经 `iproxy <本机端口> <设备端口>` 连（Wi-Fi 下加 `-n`）。 |

宿主发来的行：

```
[dm] RE {"id":4,"cmd":"press","ok":true,"frame":52,"held_frames":6}
[dm] EV {"seq":14,"t":1.297,"ev":"main_menu"}
```

连接上的其余内容（iOS 上的日志行）可以不管。

## 命令

命令是一层的 JSON 对象：`{"id":<数或串>,"cmd":"<名字>",<参数>}`。值是串、数或 `true`/`false`，数组与嵌套对象不收。回执带回 `id` 和 `cmd`，成功是 `"ok":true` 加结果，失败是 `"ok":false,"err":"<原因>"`。每条命令都等到效果生效才回执，客户端可以一条接一条地发。

| 命令 | 参数 | 做什么 | 回执字段 |
|---|---|---|---|
| `ping` | | 看还活着没有。 | `pong` |
| `help` | | 列出命令名。 | `cmds` |
| `state` | | 快照。 | `in_game`、`tick`、`paused`、`panel`（最近打开、还没关的面板）、`level`、`hp`、`hp_max`、`mp`、`mp_max`、`area`（levels.txt 号）、`x`、`y`（格）、`dead`、`players`、`holding`（光标上的物品）、`belt0`–`belt3`（腰带各列剩几瓶）、`portal`（回城次数）、`points`（未分配的属性 + 技能点）、`ev_dropped` |
| `press` | `keys`（`A+B`，键名同 `DM_KEYS`）、`frames`（6）、`gap`（3） | 按住这些键 `frames` 个出图帧，再松开等 `gap` 帧。在取手柄处并进去，与脚本键同一套裁决。 | `frame`、`held_frames` |
| `stick` | `x`、`y` ∈ [-1, 1]（上为正）、`frames`（30）、`keys` | 推左摇杆（可同时按键）。 | `frame`、`held_frames` |
| `tap` | `x`、`y` ∈ [0, 1]（出图画面）、`hold`（3 帧） | 经 `dm_ui_touch` 点一下界面，与 `DM_KEYS` 的 `C<x>:<y>` 和真手指同一条路。 | |
| `wheel` | `x`、`y` ∈ [0, 1]（出图画面）、`dy`、`precise`（1）、`view`（720）、`n`（1） | 经 `dm_wheel_ui` 滚一下滚轮 / 触控板，与 Mac App 的滚动事件同一个函数：`precise=1` 是触控板点数，`0` 是鼠标滚轮行数；`view` 是视图高几点。`dy` 为正 = 内容往下。`n` > 1 时一口气连发这么多个同样的事件、中间不等帧，同触控板甩一下。只有 Mac。 | `ui`（0 = 不在界面上，App 会改推右摇杆） |
| `drag` | `x0`、`y0`、`x1`、`y1` ∈ [0, 1]（出图画面）、`frames`（20） | 按下、在 `frames` 帧里直线拖过去、抬起。Mac App 里走真鼠标的手势路（`dm_touch_host_live`），别处走 `dm_ui_touch`。 | |
| `msg` | `spec` = `类别:动作[:参数]` | 往游戏发一条界面消息（同 `DM_KEYS` 的 `MSG:` 步），发出去才回执。 | |
| `world_tap` | `x`、`y` ∈ [0, 1] | 点世界里那一处：像触屏点击一样走过去和那里的东西交互。 | `target`：`none`、`item`、`npc`、`object`、`exit` |
| `walk_to` | `x`、`y`（格，同 `state`）、`timeout`（12 秒） | 用 `DM_TEST_WORLD_XY` 的路线规划走过去，到 2 格内算到。 | `arrived`、`x`、`y` |
| `tap_ui` | `name`（`控件[@面板]`，`+` = 空格）、`timeout`（10 秒） | 等控件可见，点它的中心，同触屏脚本的 `tap_ui`。 | `x`、`y` |
| `await_ui` | `name`、`timeout`（30 秒） | 只等控件可见。 | `x`、`y` |
| `tap_unit` | `type`（1 怪 / NPC、2 物件、4 物品、5 出口）、`cls`（类号或 `any`）、`timeout`（10 秒） | 等这类单位出现在画面里，点它，同触屏脚本的 `tap_unit`。 | `x`、`y`、`target` |
| `app` | `do`：`background`、`foreground`、`resign`、`active`、`minimize`、`restore`、`terminate` | 在主线程上走前端生命周期，与系统事件、`DM_TEST_BG` / `DM_TEST_MINI_AT` 同一批处理函数。Mac 上 `background` / `foreground` 就是最小化 / 还原，不支持 `resign` / `active`；`terminate` 照系统退出那样先存盘再退。 | |
| `screenshot` | `path`、`frames`（1） | 第 `frames` 个出图帧之后把游戏输出写成 PNG。路径须以 `.png` 结尾、不含 `..`。iOS 上只收相对路径，落在 `Documents/` 下；Mac 上所在目录须已存在、且在这一局的运行目录里（`DM_EVENTS` 所在目录、`DM_APP_SUPPORT` 的上一级、当前目录、`$DM_ROOT/run`）或临时目录下。 | `path` |
| `ui_dump` | `want`（`@top` 或控件名） | 下一帧把界面控件树打进日志（`UIDUMP` 行，同 `DM_UI_DUMP`）。 | |
| `pause` | | 冻结游戏拍（`DM_TICK_HOLD` 那种冻结）。要局内跑过拍、且开着控制通道。冻着超过 `DM_WATCH_STALL` 秒看门狗会判死。 | `tick` |
| `step` | `n`（1） | 冻着时：放行正好 `n` 拍再冻上。 | `tick` |
| `resume` | | 解冻。 | `tick` |
| `mark` | `name` | 往事件流里打一个 `mark` 事件（带当前帧号），基准路线用它切段。 | `t`、`frame` |
| `wait` | `frames`、`ticks` 或 `ms`；`timeout`（60 秒） | 在宿主这头等。 | `frame`、`tick` |
| `save_now` | `timeout`（5 秒） | 请游戏照自己的定时存盘路径存一次并等提交（前端退出 / 进后台用的同一个 `dm_save_wait`）。 | `commits`、`ms`、`paused` |
| `dump` | | 把各线程在等什么写进日志。 | |
| `crash` | `kind`：`segv`、`abort`、`trap` | 在控制口线程上故意让宿主崩（测崩溃这一路：事件、`crash.json`、`.ips`、判红）。 | |
| `quit` | `save`（0） | 先回执，再优雅退出（`dm_host_shutdown`）。`save=1` 时先像 Mac 关窗那样存一次。 | `saved` |

`state.tick` 与事件的 `tick` 是服务端拍号；`frames` 数的是出图帧，进局之前在前端也一直走。

## Python 客户端

```python
import sys; sys.path.insert(0, 'dm/tools')
import dmctl
c = dmctl.Ctl(47110, listen=True, log='port.log')   # Mac 宿主带 DM_CTL_PORT=47110 起；iOS：Ctl(<本机 iproxy 端口>)
c.wait_for('main_menu', timeout=60)         # 已经出现过的事件也算；崩溃 / 致命 / 看门狗抛 BadEvent
c.cmd('press', keys='X')
c.wait_for('panel', name='CharacterSelectPanel', act='OpenPanel')
c.cmd('msg', spec='CharacterSelect:LoadCharacter:0')
c.wait_for('loading_done', timeout=60)
print(c.cmd('state'))                       # ok=false 抛 CtlError
m = c.mark(); c.cmd('press', keys='PLUS'); c.wait_for('panel', after=m, act='OpenPanel', name='PlayerInventoryExpansionLayout')
c.cmd('quit')
```

命令行：`python3 dm/tools/dmctl.py --listen 47110 state`、`python3 dm/tools/dmctl.py --listen 47110 walk_to x=5780 y=4409`、`python3 dm/tools/dmctl.py --listen 47110 wait loading_done 60`（Mac；连 iOS 日志端口时不加 `--listen`）。每次调用占着连接直到结束，宿主随后连下一个。

## 注意

- 两个方向都只走本机回环地址，没有鉴权。不要把它们转发到本机以外。
- 输入类命令写的是单槽请求，游戏线程每帧读一次。命令会等自己的请求被取走，但两个客户端同时发输入仍会交错。
- `msg` 在 `dm/host/tests/mode_scan.py` 里算「替玩家发消息」：手柄模式与触屏模式下照常发。
