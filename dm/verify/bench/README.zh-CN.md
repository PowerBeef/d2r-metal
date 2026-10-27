# 性能基准

English: [README.md](README.md)

基准路线就是带 `mark <段名>` 步骤的[场景](../scenarios/README.zh-CN.md)。`dm/tools/bench.py` 把它跑几遍，读两份逐帧记录：
- `DM_FRAMES`：每帧的上屏间隔，用事件的时钟；
- `DM_NVN_TSV`（只要计时行）：每帧的帧、NVN、GPU 与出帧线程 CPU 时间，按帧号对齐；每个 `mark` 事件也带着帧号。

结果按段出统计。

```bash
python3 dm/tools/bench.py run dm/verify/bench/town_loop.scn --reps 3                 # bench.json 在 $DM_ROOT/run/bench/
python3 dm/tools/bench.py ab dm/verify/bench/town_loop.scn --reps 6 \
    --a-env DM_3D_SCALE=1.0 --b-env DM_3D_SCALE=0.5 --metric gpu_ms.p50             # 或 --a-host / --b-host：两个 dmhost 构建
python3 dm/tools/bench.py show bench.json
```

## 量什么

| 序列 | 来源 | 说明 |
|---|---|---|
| `present.*`（`fps`、`mean_ms`、`p95_ms`、`p99_ms`、`slow_share` = 超过 20.8 ms 的占比、`dropped`） | `DM_FRAMES`（`dm_perf_present`，与 `perf.txt` 同源） | 快的 Mac 上被垂直同步钉在 16.67 ms：看得出卡顿，看不出余量。 |
| `cpu_ms.*` | 出帧线程 CPU 时间 | 不受刷新率量化。 |
| `nvn_ms.*` | NVN 调用里的时间（翻译层） | A/B 缺省指标（`nvn_ms.p50`）。 |
| `gpu_ms.*` | 每帧 GPU 时间（滞后 8 帧取） | |
| `frame_ms.*`、`wait_ms.*` | 游戏帧间隔、取图等待 | |

每个序列有 `mean`、`p50`、`p95`、`p99`。`bench.json` 留着每一遍，另有每段每个指标取各遍中位数的 `summary`。

每份结果都记下环境：git 提交（与树干不干净）、机型、系统、CPU、游戏模式状态（`gamepolicyctl`）。每遍之前看 `pmset -g therm`：报温控告警或 CPU 限速时就等（最多 `--cool` 秒），每遍前后的温控状态都存下来。另设 `DM_STATEREP=1`，系统的 Metal 性能记录能按游戏状态分段（`metalperftrace overview --aggregate --domain GameState`）。

## A/B

`ab` 把两个变体按 ABBA 交替跑（发热、频率慢慢漂移时两边受的影响相同），每个变体 `--reps` 遍。每一段拿所选指标的每遍取值比较：
- **Mann-Whitney U**：双侧；排列数不超过 20000 时按置换精确算，否则用带并列校正的正态近似。
- **自助法**：B − A 中位数差的 95% 区间，固定种子。

区间整个在 0 的一侧且 p < 0.05 才报有差别。每个变体 4 遍时 p 最小只能到 2/70 ≈ 0.029，所以至少 4 遍，最好 6 遍。每次比较都追加到 `$DM_ROOT/run/bench/ledger.tsv`。

在这棵树上核过（Apple M6，macOS 27）：
- **A/A**（两个变体完全相同）：「无差别」，营地 −0.004 ms [−0.032, +0.056]，p = 1.0。
- **3D 分辨率减半**（`DM_3D_SCALE` 1.0 对 0.5）：「B 更快」，GPU p50 6.28 → 3.79 ms [−2.51, −2.46]，p = 0.029。

## 剖析一局

| 工具 | 命令 | 得到什么 |
|---|---|---|
| 时间线打点 | `xcrun xctrace record --template Logging --attach <pid> --time-limit 10s` | `XTRACE_*` 区间（子系统 `org.d2rmobile`，Points of Interest）：翻译线程的 `dm-x-submit`、`dm-x-present`、`dm-x-frame`、`dm-x-wait-sub`、`dm-game-syncwait`，以及 `dm-count` 读数。不在录制时每处只多一次判断。 |
| 实时 Metal 读数 | `python3 dm/tools/gpucap.py listen --pid <pid> --seconds 10`，或 `bench.py run … --mpt` | `metalperftrace listen`：每秒的上屏间隔、上屏迟到、跳帧。 |
| 系统常开的 Metal 记录 | `metalperftrace collect --last 10m <目录>`，再 `metalperftrace overview --aggregate --domain GameState <记录>` | 系统自己的记录；那一局设了 `DM_STATEREP=1` 的话按 `game_state` 分段。 |
| GPU 截帧 | `python3 dm/tools/gpucap.py capture --pid <pid>`（或场景步骤 `gpu_capture`） | 按图层边界截一帧，约 4 GB；进程要带 `MTL_CAPTURE_ENABLED=1` 起。`gpucap.py summary <截帧>` 列命令缓冲、编码器、绘制、计算派发数（`gpudebug --json`）。截帧时出图停几秒，宿主会记一条红标「显示节拍没回调」（码 `e6db5cb6`）。 |
| 采样归因 | `python3 dm/tools/bench.py profile <路线> [--ms 4] [--hitch-ms 25]` | 跑一遍，对全部客体线程采样（`DM_SAMPLE=<ms>:*`）。按段、按线程角色（主线程、任务线程、读写池、音频、其他）出在跑占比与最热的游戏函数，名字取本机的引擎符号表（`guestsyms.py`）；再列出每个超过 `--hitch-ms` 的帧（`nvn.tsv` 的 `@frame_ms`）那段时间里在跑的函数。写 `profile.json`。 |
| Metal HUD | `python3 dm/tools/gpucap.py hud --pid <pid> on|off` | 系统的性能叠层，运行中开关（`metalperftrace setup`）。 |

## 对留存基线的回归

```bash
python3 dm/tools/bench.py baseline save                 # 把本树的 dmhost（连同它的 gfx/nvn 各表）留成 $DM_ROOT/run/bench/baseline/<提交>，current 指向它
python3 dm/tools/bench.py regress <路线> [--reps 6] [--metrics cpu_ms.p50,nvn_ms.p50,gpu_ms.p50]
```

`regress` 让基线宿主（A）与当前宿主（B）按 ABBA 跑，各指标同 `ab` 判。只有「B 显著变慢」（p < 0.05、区间整个在 0 一侧）才红；每一行都记账本。没有基线时为 ⚪（退出码 3）。

## 启动时间历史

每一遍基准与每次 `dm/ios/tools/bootprof.sh` 都把进程起点到各启动阶段的秒数连同 git 提交追加进 `$DM_ROOT/run/bootprof/history.tsv`。阶段取自事件流（`boot`、`main_menu`、`game_enter`、`loading_done`）与 `DM_BOOTPROF` 表（⓪–⑥）。`python3 dm/tools/boothist.py show --label bench:cli` 拿最近几次的中位数和再往前几次比，慢了超过 10% 且 50 ms 的阶段标出来。

## 路线

| 路线 | 段 | 说明 |
|---|---|---|
| `combat.scn` | `walk`（走到血腥荒野）、`fight`（`tap_unit` 点怪打） | `save warlock`，固定地图种子；走法同 `golden/fight.scn` |
| `ui_heavy.scn` | `panels`（总面板、页签来回翻、暂停页） | `save warlock` |
| `rejoin.scn` | `leave`（保存并退出到前端）、`join`（再进局到读取画面撤下） | `save warlock` |
| `town_loop.scn` | `town`（罗格营地里走一圈）、`wild`（出南门进血腥荒野） | `save warlock`，固定地图种子（`DM_GAME_SEED`），`walk_to` 用格坐标；每遍约 32 秒。 |

加路线：写一个场景，每段之前 `mark <名>`，最后一段之后 `mark end`，并保持确定性：固定存档、固定 `DM_GAME_SEED`、移动不依赖时机。
