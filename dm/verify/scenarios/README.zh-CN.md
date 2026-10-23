# 场景

English: [README.md](README.md)

场景经[控制口](../control_port.zh-CN.md)一步一步驱动正在跑的游戏，按事件、状态和末尾的检查项判这一局。每一步都等它的效果生效才走下一步，不靠估时间。用 `dm/tools/scenario.py` 跑：

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn                     # Mac 命令行宿主（开窗）
python3 dm/tools/scenario.py run dm/verify/scenarios/*.scn --headless                   # 全部场景，不开窗
python3 dm/tools/scenario.py run dm/verify/scenarios/background.scn --backend ios-mac   # 本机上跑的 iOS 包
python3 dm/tools/scenario.py check dm/verify/scenarios/*.scn                           # 只解析
```

每个场景打 `STEP` 行、`CHECK` 行、`SCENARIO <名> OK|RED <原因>` 判定行，末行 `RESULT {json}`。结果目录（`$DM_ROOT/run/scenario/<名>_<后端>_<时刻>/`）里有 `host.log`、`events.ndjson`（命令行宿主）、`port.log`、`steps.tsv`、`result.json` 和截图。

## 后端

| 后端 | 跑的是什么 | 控制 | 需要 |
|---|---|---|---|
| `cli`（缺省） | `dm/host/dmhost`，用场景存档的副本 | `DM_CTL_PORT`（宿主连运行器） | `./setup.sh` 跑完、游戏资源 |
| `mac-app` | 玩家装的 Mac App（测试用副本 `$DM_ROOT/out/macapp_test`，没有就用 `dm/tools/mac_app.sh` 出；`DM_MAC_APP` 可另指），当子进程起 | `DM_CTL_PORT` | `DM_APP_SUPPORT` 指到结果目录：存档副本、设置、日志都在那里，iCloud 不同步，玩家自己的存档和日志不动 |
| `ios-mac` | 本机上跑的 iOS 包（`.iosdd_mac/DmMacSmoke.app`，由 `dm/ios/tools/smoke_mac.sh` 构建） | 日志端口 | 与 Mac 冒烟同一把锁、同一个 iCloud 总闸、同样的新测试存档（`dm/ios/tools/scenario_launch.sh`） |
| `device` | `dm/ios/local.env` 里那台设备上装着的 App | 经 `iproxy` 的日志端口 | 设备解锁着；设备锁 |

## 文件格式

一行一步，按顺序做。`#` 起注释。参数可以按位置给，也可以写 `名=值`；控件名里 `+` 代表空格，同触屏脚本。

表头行（写在文件哪里都行）：

| 行 | 含义 |
|---|---|
| `name <名>` | 场景名（缺省用文件名）。 |
| `save <名>` | 起始存档：`$DM_ROOT/data/switch/` 下的 `save_ref_<名>`（缺省 `warlock`）、`empty`，或绝对路径。运行器只用它的副本。 |
| `env K=V …` | 给游戏另加的环境变量。 |
| `timeout <秒>` | 整个场景的上限（缺省 300）。 |
| `window 0` | 命令行宿主不开窗。 |
| `requires <后端> …` | 别的后端上跳过。 |

步骤：

| 步骤 | 参数 | 做什么 |
|---|---|---|
| 任一控制口命令 | 同 [control_port.zh-CN.md](../control_port.zh-CN.md)；按位置的写法：`press 键 [帧]`、`stick X Y [帧]`、`tap X Y`、`msg 消息`、`world_tap X Y`、`walk_to X Y [超时]`、`screenshot 路径`（别名 `shot`）、`ui_dump [want]`、`step [n]`、`save_now`、`tap_ui 名`、`await_ui 名`、`tap_unit 类型 类号`、`app 动作`、`mark 名`、`quit` | 发这条命令；回执 `ok:false` 即红。截图的相对路径落在结果目录（命令行宿主）或 `Documents/`（iOS）。 |
| `expect <事件> [字段=值 …] [timeout=30]` | | 等上一步开始以来出现这个事件（所以上一步触发的事件不会漏掉）。 |
| `expect_no <事件> [字段=值 …] [for=3]` | | `for` 秒内出现这个事件即红。 |
| `expect_state 字段=值 … [timeout=5]` | 也可写 `字段>=n`、`字段<=n`、`字段!=v` | 反复读 `state`，直到每一条都成立。 |
| `sleep <秒>`、`log <文字>` | | |
| `remember <字段> [as=名]` | | 记下 `state` 的一个字段；后面的值可写 `$名`、`$名+N`、`$名-N`（如 `expect_state tick<=$tick+30`）。 |
| `gpu_capture [frames=1] [path=frame.gputrace] [keep=1]` | | `cli` / `mac-app`：用 `gpucapture` 截 GPU 帧，结果给 `gpudebug` 的摘要（命令缓冲、编码器、绘制、计算派发）。截帧文件（一帧约 4 GB）缺省删掉，`keep=1` 才留。这一局自动带 `MTL_CAPTURE_ENABLED=1`。截帧时出图会停，要加 `check no_red allow=e6db5cb6`。 |
| `export_save <名> [overwrite=1]` | | `cli` / `mac-app`：存盘并等提交，把这局已提交的存档拷到 `$DM_ROOT/data/switch/save_ref_<名>/`（只在本机，不入库）。`warlock` 永不替换。 |
| `enter_game [slot=0]` | | 从主菜单进局：主菜单上按 `X`，载入 `slot` 那个角色，等 `loading_done`。已在局内时什么都不做。 |
| `open_pause`、`close_pause` | | `+` 开总面板，`L` 三次切到暂停页（`pause_open`）；`B` 关掉。 |
| `background [for=3]` | | 进后台再回来：iOS 走与 `DM_TEST_BG` 相同的场景事件；Mac 上最小化再还原。 |
| `system_quit` | | 走系统退出（Mac `[NSApp terminate:]`，iOS `applicationWillTerminate`：先存盘再退），等进程结束。 |
| `swipe_away` | | iOS：先失活，再杀进程（在多任务界面划掉）。命令行宿主：直接杀。 |

末尾检查（`check …`，跑完后判）：

| 检查 | 通过的条件 |
|---|---|
| `check no_red [allow=码,…]` | 没有 `red`、`fatal`、`crash`、`watchdog` 事件（列出的红标码除外）。 |
| `check no_red_log [allow=正则]` | 游戏日志（`host.log` / `port.log`）里没有红标行（`【红】`、`🔴`、`FATAL`、`CRASH`、看门狗），连不经事件流的行（如 iOS 外壳的 `plog`）也算。`allow` 放行已知的那一行。 |
| `check saved` | 至少提交过一次存档。 |
| `check exit <码>` | 进程退出码（命令行宿主）。 |
| `check event <事件> [字段=值 …]` / `check no_event …` | 出现过 / 没出现过这样的事件。 |

## 生成的存档

场景可以从另一个场景生成的存档起。`save <名>` 指的存档不在、而 `saves/<名>.scn` 在时，运行器先在命令行宿主上跑这个生成场景，它以 `export_save <名>` 结尾。例：`saves/belt_two_drunk.scn` 喝掉腰带里两瓶药、导出 `save_ref_belt_two_drunk`，`belt_persists.scn` 核新的一局读回来的腰带就是那样。iOS 后端用的是构建时打进 App 的存档，所以这类场景写 `requires cli`。

触屏脚本的动作对得上步骤：`tap_ui`、`await ui`（`await_ui`）、`walk_to`、`tap_unit`、`shot`、`pad`（`press` / `stick`）。按时刻的手指行（`down` / `move` / `up`）留在 `DM_TOUCH_SCRIPT` 里，它们测的是触屏操作层本身。
