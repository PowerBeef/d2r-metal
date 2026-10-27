# 事件流格式（v1）

English: [event_stream.md](event_stream.md)

宿主日志是写给人看的中文，措辞会随版本变。测试脚本和代理需要更稳的东西：**事件流**是日志旁边的第二条结构化通道，把判一局要看的那些时刻（主菜单出来、进局、读取画面撤下、存档提交、红标、崩溃、App 进后台……）记成一行一个 JSON 对象。日志本身不变。

代码在 `dm/host/dm_events.c`（接口见 `dm_events.h`）；读取与判定用 `dm/tools/dmevents.py`。

## 开关

| 环境变量 | 作用 |
|---|---|
| `DM_EVENTS=<文件>` | 事件写进这个文件（启动时覆盖）。iOS 上相对路径落在 App 的 `Documents/` 下。 |
| `DM_EVENTS=log` | 不写文件：每个事件在日志里打一行 `[dm] EV {json}`。iOS 上这行随日志走日志端口，`ios/tools/logport.py` 读得到。 |
| `DM_EVENTS_OSLOG=1` | 另把每个事件写进系统日志（子系统 `org.d2rmobile`，类别 `events`，`%{public}`），Console 或 `log stream --predicate 'subsystem == "org.d2rmobile"'` 可见。 |

都不设时，每个事件调用点只多一次指针判断，`LOG` 与原先完全一样。

## 行格式

UTF-8，一行一个对象，以 `\n` 结尾；一次 `write()` 写出，多线程同时发也不会半行交错：

```
{"seq":14,"t":1.247,"ev":"main_menu"}
{"seq":53,"t":4.380,"ev":"loading_done","tick":16}
```

- `seq`：每个事件加一，按文件顺序递增。`crash` 事件不取锁，序号可能与下一行相同。
- `t`：进程创建以来的秒数，截到毫秒。零点与日志里 `UIMSG` 行的 `[1.25s]`、启动时间线（`DM_BOOTPROF`）相同。
- `ev`：事件类型（见下表）。
- `tick`：局内时自动带上服务端拍号。
- 其余字段随类型而定。字符串按 JSON 转义，超过 400 字节在 UTF-8 字符边界处截断。

## 事件类型

| `ev` | 字段 | 何时 |
|---|---|---|
| `start` | `v`（格式版本 1）、`pid`、`platform`（`mac`、`ios`）、`wall`（Unix 时间） | 事件流打开（第一行）。 |
| `boot` | `stage`：`host_loaded`（`mods` = 模块数）、`module_inits`、`nnmain`、`front_end` | 启动阶段：模块装好并重定位、各模块 init_array 跑完、进 `nnMain`、游戏第一次 `FrontEndNavigation ToFrontEnd`。 |
| `main_menu` | | `PanelManager OpenPanel MainMenuButtonRibbon`（每次打开都发）。 |
| `game_enter` | | `FrontEndNavigation ToGame`。 |
| `loading_done` | | `game_enter` 之后读取画面卸下（`UnloadPanel LoadScreenPanel`）。 |
| `pause_open` | `name` | `OpenPanel PauseLayout…`（局内那一页叫 `PauseLayoutGarden`）。 |
| `game_leave` | | 进过局之后的 `FrontEndNavigation ToFrontEnd`。 |
| `nav` | `to` | 任一 `FrontEndNavigation` 消息。 |
| `panel` | `act`（`OpenPanel`、`ClosePanel`、`UnloadPanel` 等）、`name` | 任一带面板名的 `PanelManager` 消息。 |
| `save` | `result`：`commit`（`n`、`writes`、`commit_us`、`txn_us`）、`rejected`（`errno`）、`fail`（`errno`） | 存档事务提交（`hle_fs.c`）：提交成功；本事务有写失败而拒绝提交；克隆 / 互换失败。 |
| `red` | `code`、`src`、`msg` | 带 `【红】` 或 `🔴` 的日志行。`code` 是该日志调用格式串的 8 位十六进制哈希，周围代码挪动也不变；`src` 是 `文件:行`。 |
| `fatal` | `code`、`src`、`msg` | `FATAL` 日志行（`die()`、`MDIE`），进程随即中止。 |
| `crash` | `sig`、`pc`、`where`（`模块+偏移`，只在游戏模块内才有）、`addr` | 崩溃处理（`crash.c`），在打寄存器现场之前写出。 |
| `watchdog` | `stalled_s`、`stage` | 看门狗（`DM_WATCH_STALL`）判死：这么多秒没有进展。 |
| `app` | `state` | 外壳生命周期。iOS：`resign`、`background`、`foreground`、`active`（场景回调）。Mac：`resign`、`active`（窗口主窗状态）、`minimized`、`restored`、`occluded`、`visible`。 |
| `quit` | `why` | Mac：窗口 / App 退出流程开始。 |
| `quit_save` | `saved`、`commits`、`ms`、`paused` | Mac：退出前等存盘结束。 |
| `save_request` | | 宿主请游戏存盘（`dm_request_save`：`save_now`、退出 / 进后台的存盘、`DM_TEST_SAVE_AT`），`save_stuck` 从这里起计时。前一次请求还没回音时不再发。 |
| `save_stuck` | `why`（`no_commit`、`mounted`）、`s`、`busy`、`pending`、`mounted`、`containers` | 红（`dm_engine.c`，另有一行【红】）：请了存盘，游戏没暂停地走了 `DM_SAVE_STUCK_S` 秒（20）还没有提交或拒绝；或某个存档容器连着挂了 `DM_SAVE_MOUNT_STUCK_S` 秒（60）。 |
| `save_slow` | `op`（`write`、`commit`）、`ms` | 存档挂载上的一次写或一次提交在宿主这边用了至少 `DM_SAVE_SLOW_MS`（1000）毫秒；到 4 秒另有一行【红】。存档任务卡约 7.5 秒，游戏的异步任务内存就用光、游戏崩溃，30 帧与 60 帧一样。 |
| `save_alert` | `errno` | 告诉玩家存盘没写进去（`front_save_failed`）：提交被拒、克隆 / 互换失败、挂载时克隆失败。 |
| `fault` | `point`、`n`（`savedelay` 为 `ms`） | 测试故障打到了（`DM_TEST_FS_FAILAT`、`DM_TEST_SAVE_DELAY`、控制口 `fault`）：`mountclone`、`reclone`、`guestname`、`renameback`、`guestopen`、`savedelay`。 |
| `action` | `id`、`name`、`phase`（`begin`、`end`） | 手柄动作到了界面总线（`InputMessage` `ControllerActionBegin` / `End`）；`name` 运行时从游戏自己的动作名表读（`select`、`back`、`pause`、`cycleLeft`…；读不出为 null）。只在有面板收输入时有：菜单里有，局内走动没有。 |
| `language` | `name`（`enUS`、`esES`、`zhCN`…）、`idx` | 游戏换了文字语言（SetLanguage；开机读设置文件时，或选项页里换）。 |
| `opmode` | `from`、`to`（1 主机、0 掌机） | 运行模式变了（`dm_set_opmode`：前端过热降级、`DM_OE_SCRIPT`），经通知 30 / 31 告诉游戏。 |
| `padmode` | `mode`（`controller`、`touch`）、`glyph`（`Switch`、`keyboard`、`Xbox`、`PlayStation`）、`frames`、`timeouts` | 手柄 / 触屏布局或键帽样式切换做完了（`dm_padmode.c`）；`timeouts` > 0 = 有一次界面重建没按时看到。 |
| `focus` | `panel`、`widget` | 焦点控件变了（`dm_engine.c`，游戏的焦点管理器）；null = 没有焦点。 |
| `shutdown` | `why` | `dm_host_shutdown` 开始（SIGTERM、iOS 终止 / 场景断开、Mac 退出）。 |
| `shot` | `path`、`ok` | 一张截图写完（`DM_NVN_SHOT_UI`、控制口 `screenshot`）。 |
| `tick_hold` | `on` | 控制口冻结（`pause`、`step`）或放开了游戏拍。 |
| `ctl` | `state`（`connected`） | 有控制口客户端连上（`DM_CTL_PORT`）。 |
| `game_state` | `state`：`Menu`、`Loading`、`Playing`、`Paused`、`Background` | 游戏状态变了（从界面消息与外壳的进后台事件推出）。设 `DM_STATEREP=1` 时同样的状态也报给系统的 StateReporting（`GameState` 域）。 |
| `mark` | `name`、`frame` | 控制口的 `mark` 命令（基准切段）；`frame` 与 `DM_NVN_TSV` 计时行同一帧号。 |
| `break` | `tick`、`pid`、`wait_s` | `DM_BREAK_AT_TICK` 到了那一拍：拍线程最多等 `wait_s` 秒，等调试器挂上 `pid`（`dm/verify/debugging.zh-CN.md`）。 |
| `exit` | `why`（`exit`、`ticks_full`、`watchdog`、`shutdown`、`nnMain returned`）、`code` | 进程即将结束。走 `_exit` 的路径自己先发；`exit()` 经 `atexit` 发。 |

只有经过 `LOG` / `MLOG`（以及 `die` / `MDIE`）的行才会变成 `red` / `fatal` 事件；标记写在格式串里或在 `%s` 参数里都算。用别的方式打的行（比如 iOS 外壳的 `plog`）看不到。

## 读事件流

```bash
python3 dm/tools/dmevents.py summary events.ndjson                    # 时间线、计数、坏事件；末行 RESULT {json}
python3 dm/tools/dmevents.py expect events.ndjson main_menu game_enter loading_done --no-red --within 60
python3 dm/tools/dmevents.py why events.ndjson                        # 第一个崩溃 / 致命 / 看门狗，没有就第一条红标
python3 dm/tools/dmevents.py follow events.ndjson loading_done --timeout 90   # 文件边写边等
```

四个子命令也都收夹着 `[dm] EV` 行的日志（`DM_EVENTS=log` 的那种）。`expect` 打 `EVENTS OK|RED` 判定行，`dm/tools/dmtest.py` 认得。

## 谁在用

- `ios/tools/logport.py`（`smoke.sh`、`smoke_mac.sh` 用它）：App 发了事件行就按事件认里程碑；旧构建退回 `UIMSG` 原文。两个冒烟脚本都设了 `DM_EVENTS=log`。
- `verify/touchchain.sh`：往局目录写 `events.ndjson`；宿主失败时用 `dmevents.py why` 打原因。

## 加一个事件

在包含了 `dm.h`（或 `dm_events.h`）的宿主 C / Objective-C 代码里调 `DM_EV("类型", "名:型 名:型 …", 值…)`。型：`s` 字符串、`d` int、`l` long long、`u` unsigned long long、`x` 十六进制串、`f` double、`b` 真假。类型名用英文，加进上表，原有的中文日志行照留。
