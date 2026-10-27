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

## 我们自己的基线：`golden.py`

不用原版游戏的录制，分支拿自己的构建录基线，放在 `$DM_ROOT/data/switch/golden/`（绝不入库），只在有意时重录：每次 `record` 都要 `--reason`，连同提交号记进 `golden/LOG.tsv`。

```bash
python3 dm/verify/golden.py replay record --reason "…"   # dm/verify/golden/{walk,fight,belt}.scn：命令包 + 逐拍状态 + 那一局提交的存档
python3 dm/verify/golden.py replay check                 # 在 dmhost 与 dmhost_m3 上回放、逐拍比；回放存的档用 savecmp 比；负对照
python3 dm/verify/golden.py shots record --reason "…"    # shotgate 里 11 个不用录制输入的页，按渲染接口分（m4 / m3）
python3 dm/verify/golden.py shots check                  # 用 shotgate.py 比基线，阈值同页规格
python3 dm/verify/golden.py prefix                       # 触屏用例的野外前缀（golden/prefix_wild.scn）+ 存档的 MANIFEST.sha256
python3 dm/verify/golden.py list
```

- **逻辑：** 每条路线用录下的命令包回放（`replay.py`），逐拍比（`tickdiff.py`）。回放在录制请存盘的那一拍也请一次（`DM_TEST_SAVE_AT`），`savecmp.py` 比这份存档与录下的那份（校验和各自自洽、时间核各自那一局的时段、其余逐字节）。
- **负对照：** 每次检查还把路线从中间那条客户端命令包起往后全删再回放一次，必须分叉，否则比对是瞎的、判红。（只删一条移动包不够：后面的包会重新指同一处。）
- **画面：** `cube` 为 ⚪（术士存档没有赫拉迪克方块）；`newchar` 截到的是过场黑帧，只证明那一帧稳定。
- **要重录：** 有意改了路线看得到的东西之后：城镇落点（`dm_townwp.c`）、人数档、秘境或自定义设置的缺省、画面设置。
- `full` 两样都跑；没有基线时为 ⚪。

## 试玩与更广的覆盖

- **场景驱动前端输入：** 控制口 `fe` 跑一行 `DM_TEST_NSMOUSE` 的输入（虚拟手柄、按键、输入表单、真鼠标事件），走与玩家同一条前端路；`inputmode` 切手柄 / 触屏布局与键帽（做完发 `padmode` 事件）。带 `fe` 步的场景开窗跑。
- **`dm/tools/playbot.py`：** 按种子抽玩家意图，只走玩家的路（不发 `msg`，碰不到已知会崩的那几处），每个意图后查不变量：没暂停时拍在走、HUD 在面板栈上、面板关了就没有焦点；收尾查没有红、没有 `save_stuck`、存档合法。红了先重跑，再按意图缩短（`explore.py` 的 ddmin）。`playbot.py soak --minutes 30` 是 Mac 浸泡，用 `soak_report.py` 判（拍、内存斜率、句柄 / 线程、帧时间）。`long` 两样都跑。
- **覆盖场景：** `new_character_vkbd`（用手柄屏幕键盘给新角色起名，经 `fe`）、`input_mode_switch`（手柄 ↔ 触屏，Xbox / PlayStation / Switch 键帽；替 accept ⑩ 的切模式部分）、`opmode_switch`（主机 → 掌机；自给的 accept ⑩）、`lang_zh`、`lang_es`（游戏语言真的换了：`language` 事件），以及 `full` 里原有的 `ios-mac` 生命周期场景。
- 做的时候发现：「保存并退出」后 `state.in_game` 一直是真（HUD 读数只在走拍时更新）；命令行宿主上 `DM_OE_SCRIPT` 的 `@ingame` 与 `DM_TEST_LANG` 悄悄不起作用。都修了；每个场景现在都要求一个证明切换真的发生了的事件。
- 没覆盖：局内切语言（语言在挂存档时生效）。

## 能说明时间花在哪的基准

- **路线**（`dm/verify/bench/`）：`town_loop`、`combat`（在血腥荒野里 `tap_unit` 打怪）、`ui_heavy`（面板与页签）、`rejoin`（保存并退出再进局）。
- **`nvn.tsv` 新的逐帧计数：** `@binds`（录下的绑定命令：纹理、采样器、缓冲、程序、状态）与 `@cmdmem_cb`（游戏补命令内存的回调）；`bench.py` 连同 `@draws` 按段报。数*重复*绑定要每个命令缓冲的影子状态，属于 s38 那项工作。
- **`bench.py profile`：** 采样跑一遍（`DM_SAMPLE=4:*`，每条客体线程一行 `SAMPLE-THR` 给名字）：按段、按线程角色出在跑占比与最热的函数（引擎名字）；每个超过 25 ms 的帧列出那段时间里在跑什么。
- **`bench.py baseline save` / `regress`：** 对留下来的旧宿主按 ABBA 比；只有显著变慢才红；每次比较都记账本。
- `long` 跑四条路线、一次采样归因和回归（没留基线时为 ⚪）。

## 触屏用例与夹具存档

触屏用例从 `save_ref_warlock`（或 `DM_TOUCH_SAVE`）起跑。有几例是照上游的夹具存档写的：背包 (5,3) 格有物品、(8,0) 是赫拉迪克方块。对起跑状态的检查是**前提**（`lib.precondition`）：存档里没有用例要的东西时，这一例判 ⚪ 不可用（`assert.py` 退出码 3，`touchchain.sh` 退出码 3）而不是红，其余检查不算数。缺前缀录制的用例同样判 ⚪。

分支能生成的自己生成：

- `dm/verify/touch_fixture.sh inv53` 跑 `touchcases/_fixtures/inv53`（把卷轴从 (9,3) 拖到 (5,3)），让游戏存盘（`DM_TEST_SAVE_AT`），导出 `save_ref_touch_inv53`。用例目录里的 `save` 文件指向它：`inv_tap`、`inv_drag`、`inv_drag_back`、`inv_drag_out`、`inv_long_press`。
- `golden.py prefix` 给 `skill_main` 录 `prefix_wild_warlock`。
- `inv_double_tap` 仍为 ⚪：要赫拉迪克方块。

## 不做的

- 不比真值：`accept.sh` 的 ①–⑧ 项要原版游戏的录制。
- 不上传：结果留在 `$DM_ROOT/run/gate/`。
