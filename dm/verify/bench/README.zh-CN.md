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

## 路线

| 路线 | 段 | 说明 |
|---|---|---|
| `town_loop.scn` | `town`（罗格营地里走一圈）、`wild`（出南门进血腥荒野） | `save warlock`，固定地图种子（`DM_GAME_SEED`），`walk_to` 用格坐标；每遍约 32 秒。 |

加路线：写一个场景，每段之前 `mark <名>`，最后一段之后 `mark end`，并保持确定性：固定存档、固定 `DM_GAME_SEED`、移动不依赖时机。
