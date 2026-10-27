# 分支闸门：`fork_gate.sh`

English: [FORK_GATE.md](FORK_GATE.md)

d2r-metal 自己的验证。上游的验收（`accept.sh`）靠打了补丁的模拟器里原版游戏的真值录制；本分支没有这些录制，所以它的大多数项在这里是 ⚪ 不可用。分支闸门只跑这台 Mac 自己能查的：源码检查、各套单测、在我们自己的构建上跑的场景、触屏用例、故障注入、消毒器与基准。`accept.sh` 留给合并上游时用。

```bash
dm/verify/fork_gate.sh quick            # < 1 分钟：源码检查
dm/verify/fork_gate.sh quick unit       # 约 6 分钟：加上各套单测
dm/verify/fork_gate.sh core             # ≤ 15 分钟：命令行宿主上的场景 + 触屏用例
dm/verify/fork_gate.sh full             # 约 25 分钟：加上 Mac App、Mac 上的 iOS 包、Metal 校验层、故障注入
dm/verify/fork_gate.sh long             # 按需，1.5–2.5 小时：基准、消毒器宿主、模糊测试
dm/verify/fork_gate.sh device           # iPhone 连得上时
```

可以一次给几层，按给的顺序跑。

## 结果

- 每一步经 `dm/tools/dmtest.py` 跑，`result.json` 与 JUnit 文件写进 `$DM_ROOT/run/gate/<时间>_<层>/` 下这一步的目录；这一步的控制台输出在旁边的 `<步>.console`。
- `steps.tsv` 每步一行：名字、结论（OK / RED / ERROR / UNAVAIL）、秒数、不可用项数。
- `summary.json` 与最后一行 `FORKGATE OK|RED {json}`：层、提交、用时、红的步、不可用项数。
- `$DM_ROOT/run/gate/latest` 指向最近一次。
- 不可用项（⚪）不算红：缺录制、夹具存档里没有用例要的东西、手机不在。

## 各层

| 层 | 步 |
|---|---|
| quick | `scan_literals.py`、`dm_i18n.py --check`、`switches.py check`、全部场景的 `scenario.py check`、`check_hud_gen.sh` |
| unit | 宿主、图形、图形旧 API、iOS 各套单测；`regress.sh` |
| core | 编宿主；不含前端步的场景无窗跑、三个并行；要最小化 / 进后台 / 退出的开窗一个一个跑；再跑触屏用例 |
| full | 按当前提交重出 Mac App 测试副本、在它上面跑场景；`smoke_mac.sh 1`（同时按当前提交重出 Mac 上的 iOS 包）、在那个包上跑场景；一个场景开 Metal API 校验；故障场景 |
| long | `bench.py` 跑城镇路线（最先跑，机器还凉）；`explore.py` 在 ASan、UBSan 宿主上；TSan 单独跑；`fnfuzz.py` |
| device | iPhone 上 `smoke.sh`、`lifecycle.sh`；连不上为 ⚪ |

## 守的规矩

- **Mac 游戏锁。** 开窗的运行互相抢帧。闸门在要开窗的层只取一次锁（`dm/ios/tools/mlock.sh`），并设 `XT_LOCK_HELD=1`，嵌套的工具不再取。`scenario.py`（开窗的 `cli` 后端与 `mac-app`）、`bench.py`、`explore.py`、`replay.py` 现在没人持锁时自己取；无窗运行不需要。
- **测试用的应用按当前提交出。** `mac-app` 测试副本（`$DM_ROOT/out/macapp_test`，bundle ID `org.d2rmobile.mac.test`）的标记（`.gate_head`）与提交和工作区状态对不上就重出。绝不用 `out/macapp` 里玩家的那份。
- **红标。** `assertlib.RED_MARKERS` 也认 Metal 命令缓冲出错与 GPU 故障（`logred.sh` 的那几条），先拿现有的场景日志核过没有误报。

## 懂引擎的检查

`dm/host/dm_engine.c` 每帧在游戏主线程上读一遍游戏自己的界面与存盘状态（只读；控件按名字与界面代号跟，不留指针），发布出去：

- **控制口：** `state` 多了 `panels`（屏上面板，最上面的在前）、`focus`、`focus_panel`、`cursor_mode`、`save_busy`、`save_pending`、`save_mounted`、`save_containers`、`exp_permille`、`quest_unread`；`engine` 给整份快照，加最近的手柄动作与界面消息。
- **事件：** `action`（手柄动作，名字运行时从游戏自己的动作名表读）、`focus`、`save_request`、`save_stuck`（红：请了存盘一直没有提交或拒绝，或容器挂着太久）、`save_alert`、`fault`。
- **场景检查：** `expect_state panels~=名字 focus=控件`、`check actions seq=…`、`check save_pairs`、`check saves_valid`（`dm/verify/d2save.py`，游戏载入时的规则：头、版本、大小字段、校验和、共享箱各段、金币上限）。`cli` / `mac-app` 每局都核已提交的存档。
- **红时：** 运行器把宿主的 `engine` 快照存成结果目录里的 `engine.json`。
- **lldb：** `dm ui`、`dm focus`、`dm saves`、`dm unit <id>`（`dm/verify/debugging.zh-CN.md`）。

## 故障矩阵

`dm/verify/scenarios/faults/`（`full` 跑）：局内写方式打开失败、提交被拒后重新克隆也失败、开机克隆失败（这一局只读）、退出落在存盘中间，以及 `save_stuck` 的正对照。故障用控制口 `fault` 命令挂（`arm` 带 `skip` / `count`、`delay`），或 `DM_TEST_FS_FAILAT=<点>[@<跳过>][:<次数>]` / `DM_TEST_SAVE_DELAY`；每次注入发 `fault` 事件，用例要求它出现，故障没打到就过不了。`guestname` / `renameback` 正常玩到不了，留给 `dm/host/tests/t_fs.c`。

做的时候发现：游戏的存盘任务跑在它的异步任务池上，一个任务卡住时，每帧排进去的任务把任务池的内存用光，约 8 秒后 任务池的入队函数（main+0x14f0590）拿到空指针照写，游戏崩溃。存盘卡上几秒（存储慢、宿主钩子占着存档锁）就是崩溃，不只是存晚了。

## 触屏用例与夹具存档

触屏用例从 `save_ref_warlock`（或 `DM_TOUCH_SAVE`）起跑。有几例是照上游的夹具存档写的：背包 (5,3) 格有物品、(8,0) 是赫拉迪克方块。对起跑状态的检查是**前提**（`lib.precondition`）：存档里没有用例要的东西时，这一例判 ⚪ 不可用（`assert.py` 退出码 3，`touchchain.sh` 退出码 3）而不是红，其余检查不算数。缺前缀录制的用例同样判 ⚪。

## 不做的

- 不比真值：`accept.sh` 的 ①–⑧ 项要原版游戏的录制。
- 不上传：结果留在 `$DM_ROOT/run/gate/`。
