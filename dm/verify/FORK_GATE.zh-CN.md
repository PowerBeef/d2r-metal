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

## 触屏用例与夹具存档

触屏用例从 `save_ref_warlock`（或 `DM_TOUCH_SAVE`）起跑。有几例是照上游的夹具存档写的：背包 (5,3) 格有物品、(8,0) 是赫拉迪克方块。对起跑状态的检查是**前提**（`lib.precondition`）：存档里没有用例要的东西时，这一例判 ⚪ 不可用（`assert.py` 退出码 3，`touchchain.sh` 退出码 3）而不是红，其余检查不算数。缺前缀录制的用例同样判 ⚪。

## 不做的

- 不比真值：`accept.sh` 的 ①–⑧ 项要原版游戏的录制。
- 不上传：结果留在 `$DM_ROOT/run/gate/`。
