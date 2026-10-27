# 调试

English: [debugging.md](debugging.md)

查一局出了什么错用的工具：崩溃记录、游戏程序的符号、调试器、校验与消毒器构建、自动探索。

## 崩溃

宿主带 `DM_CRASH_JSON=<路径>` 起时，崩溃处理（`dm/host/crash.c`）在打平常那几行崩溃日志之前先往那里写一份结构化记录；场景运行器把它设成结果目录里的 `crash.json`。处理函数只用异步信号安全的调用，日志那段卡住也写得出来。

| 字段 | 含义 |
|---|---|
| `sig`、`code`、`addr` | 信号、`si_code`、出错地址 |
| `pc`、`lr`、`sp`、`fp`、`x[0..28]` | 寄存器 |
| `pc_where`、`lr_where` | 在游戏模块里时是 `main+0x…` / `sdk+0x…`，否则 `null`（宿主代码） |
| `bt` | 帧指针回溯，每帧 `{pc, where}` |
| `modules` | 各游戏模块的装载范围 |
| `host_base` | 宿主可执行文件的装载地址，`atos -l` 用 |
| `tick`、`stage` | 看门狗记下的最后拍号与阶段 |

打完之后，所有致命信号都恢复缺省处理再发一次：进程照旧按这个信号结束，系统写自己的崩溃报告（`.ips`），shell 看到的退出码仍是 128 + 信号。macOS 会压掉同一进程反复出现的报告，所以要靠的是 `crash.json`。

```bash
python3 dm/tools/crashlog.py show crash.json [--bin dm/host/dmhost]     # 宿主帧用 atos（文件:行），游戏帧查符号表
python3 dm/tools/crashlog.py collect --process dmhost --since <秒>      # 把系统的 .ips 拷过来并出摘要
```

场景里的进程崩了时，运行器把记录符号化成 `crash.txt`，收进系统的 `.ips`，判定行直接说崩在哪（`crash SIGSEGV at cmd_crash (dm_ctl_cmds.c:280)`）。控制口的 `crash kind=segv|abort|trap` 命令故意让宿主崩，用来测这一路。

## 游戏程序的符号

```bash
python3 dm/tools/guestsyms.py build                       # → $DM_ROOT/build/cache/dm/symbols/<产物>.tsv（只在本机，不入库）
python3 dm/tools/guestsyms.py lookup main+0x455aa4 sdk+0x2b0470
```

- **函数边界**取自各模块的 `.eh_frame`（`main` 约 57000 个函数，`sdk` 29000 个）。
- **名字**取自动态符号表，还原成可读形式；大多来自 `sdk` 导出的名字。
- **说明**取自源码注释里提到 `模块+0x…` 的那几行。例：`main+0x455aa4` 是 `fn_455a80` 里第 0x24 字节，注释说这个函数在拼血条信息。

`crashlog.py` 以及别的打 `模块+偏移` 的地方都能用这张表。

- **引擎函数名**来自引擎分析库（有的话；[dm/tools/re](../tools/re/README.zh-CN.md)）：`main` 的一份私有 Ghidra 工程，带本机查询服务（`re.sh serve`、`re.sh q decompile main+0x…`）。它的导出 `$DM_ROOT/re/export/main.names.tsv` 给动态符号表没起名的函数起名，用在本表的查询、`crashlog.py`、`dm bt` 和 `sampagg.py` 里。它的输出只留在本机。

## 调试器

### 停在某一拍

`DM_BREAK_AT_TICK=<拍>[:<等待秒>]`（缺省等 300 秒）让游戏的拍线程在那一拍的入口停下：

1. 宿主在日志里打出 pid，并发 `break` 事件（`tick`、`pid`、`wait_s`；[event_stream.zh-CN.md](event_stream.zh-CN.md)）。
2. 拍线程等调试器挂上（这期间游戏拍冻住）。
3. 一挂上就执行 `__builtin_debugtrap()`，调试器停在拍线程的拍入口。`continue` 或 `process detach` 之后游戏照常往下走。
4. 等待时间内没有调试器来，游戏自己往下走。

看门狗（`DM_WATCH`、`DM_WATCH_STALL`）把等待和停在调试器里的时间都当作没有进展。调试时手动起宿主（`CLAUDE.md` 里的命令行宿主命令），这两个都别设。场景运行器总会设 `DM_WATCH`，并因此开着 15 秒的停顿上限，在它下面停的时间要短。

```bash
DM_BREAK_AT_TICK=40 DM_ROMFS=… DM_SAVE=… ./dmhost "$(../tools/product.sh path touch)" main sdk
lldb -p <日志里的 pid>
(lldb) command script import dm/tools/lldb/dm.py
(lldb) continue          # 停在拍线程的陷阱上
```

`lldb --batch` 把停在陷阱上算作类似崩溃的停止，之后的命令要用 `-k`（停下时执行）给，不能用 `-o`。

### 游戏地址的 LLDB 命令

`dm/tools/lldb/dm.py` 加一个 `dm` 命令，读宿主的模块表和上面那张客体符号表：

| 命令 | 作用 |
|---|---|
| `dm modules` | 各游戏模块的装载范围（`g_mod`） |
| `dm where <地址表达式>` | `main+0x…` / `sdk+0x…` 加客体函数，或宿主符号 |
| `dm bt [-a]` | 当前线程（`-a`：全部线程）的回溯，每帧给模块偏移和客体函数，带源码注释里的说明 |
| `dm break <模块>+0x<偏移>` | 在游戏地址下断点，按当前装载基址换算 |
| `dm tick` | 当前服务端拍号（调 `dm_cur_tick`） |
| `dm ui [-a]` | 界面根的面板：名字、可见 / 激活、优先级（`-a` 连不可见的） |
| `dm focus` | 焦点管理器：悬停控件、焦点所在面板、焦点控件；光标模式与状态 |
| `dm saves` | 存盘忙 / 提交推迟，各存档容器的挂载状态与脏标 |
| `dm unit <id>` | 按 id 找服务端单位（先物品表、再怪物表）：地址、类型、行号、模式 |

不挂调试器也读得到同样的游戏状态：控制口的 `state` / `engine` 命令（`dm/host/dm_engine.c`），以及红了之后场景结果目录里的 `engine.json`。

### 给代理用：lldb-mcp

Xcode 的 LLDB 能提供模型上下文协议（MCP）服务，代理可以驱动同一个调试会话：

1. 在 lldb 里：`(lldb) protocol-server start MCP`。`protocol-server get MCP` 显示监听地址。
2. 代理的 MCP 配置里，服务命令是 `xcrun lldb-mcp`（标准输入输出）。Claude Code：`claude mcp add lldb -- xcrun lldb-mcp`。
3. 它提供的工具是 `command`（执行一条 lldb 命令，例如 `dm bt`）和 `debugger_list`。

同一时间只能有一个 lldb 提供 MCP 服务：多了的话 `lldb-mcp` 报「too many MCP servers running」退出。被杀掉的 lldb 会留下 `~/.lldb/lldb-mcp-<pid>.json`，过时的要删掉。

`lldb-mcp` 只在启动时查找服务。那时如果一个都没有，它会自己起一个 lldb（`lldb -O 'protocol start MCP'`）：代理退出后它还在运行，下一个代理接着用，也算作那唯一的一个服务。Xcode 27.1 起的这个 lldb 的标准输入输出是关着的：能加载目标，但每次 `process attach` 都报「Bad file descriptor」失败。办法有两个：在代理启动前先自己起一个带服务的 lldb（第 1 步）；或者把 `LLDB_EXE_PATH` 指向一个包装脚本，给 `lldb-mcp` 起的 lldb 一个永不关闭的标准输入：

```bash
#!/bin/bash
fifo=$(mktemp -u); mkfifo "$fifo"; exec 3<>"$fifo"; rm -f "$fifo"
exec "$(xcrun --find lldb)" "$@" <&3 3<&- >>"${TMPDIR:-/tmp}/lldb-mcp-backend.log" 2>&1
```

用 `claude mcp add lldb -e LLDB_EXE_PATH=/path/to/wrapper -- xcrun lldb-mcp` 注册。之后代理用 `process attach -p <pid>` 附加，再用 `command script import <完整路径>/dm/tools/lldb/dm.py` 载入上面的命令。程序要自己启动再附加：经 `lldb-mcp` 执行 `process launch` 不会返回（有没有包装脚本都一样），之后的命令全被堵住，直到那个 lldb 被杀掉。要重新由自己的 lldb 提供服务，先停掉那个：`pkill -f 'lldb -O protocol start MCP'`。

## 消毒器构建

```bash
make -C dm/host dmhost_asan     # 地址消毒 + 未定义行为消毒
make -C dm/host dmhost_ubsan    # 只有未定义行为消毒
make -C dm/host dmhost_tsan     # 线程消毒
SCN_DMHOST=$PWD/dm/host/dmhost_asan python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn
```

- 只插桩宿主代码，游戏程序不插桩。这几个目标不跟踪头文件，改了头文件要先删掉可执行文件再编。
- 每次跑场景都会查日志里的消毒器报告。有报告就判红，按摘要行去重后汇总在结果目录的 `sanitizer.txt`。命令行：`python3 dm/verify/assertlib.py sanitizers <日志>`。
- 各消毒器需要的选项编进了可执行文件（`main.c`），不用设环境变量。

这几个构建做了哪些调整、为什么：

| 构建 | 调整 | 原因 |
|---|---|---|
| ASan | 不插桩栈帧（`-mllvm -asan-stack=0`），堆和全局变量照查 | 宿主代码跑在游戏给的栈上。红区让栈帧变大，留下的红区标记还会在游戏复用那块内存时误报。 |
| ASan | 登记表（`DM_SECT`）不加红区 | 宿主把每个表段当成紧挨着的数组读。 |
| ASan | `use_sigaltstack=0:detect_leaks=0` | 宿主每线程自己设备用信号栈（`crash.c`）；ASan 会在线程退出时 `munmap` 这块 malloc 来的栈。 |
| ASan、TSan | 游戏线程改在 8 MiB 的宿主栈上跑，不用游戏给的那块（`hle_thread.c`） | ASan 的释放路径和 TSan 报告时的符号化都在当前栈上跑，撑爆过游戏的小线程栈（有的只有十几 KiB）。 |
| TSan | 客体内存（地址空间预留块和各模块映像）登记为良性竞争范围（`DM_TSAN_GUEST`） | 它由游戏自己的同步定序，TSan 看不见。 |
| TSan | 内置抑制表（`main.c` 里的 `__tsan_default_suppressions`） | 查过的宿主侧情形：记账计数器、故意不拿锁读的现场导出、fd 表（由 `close` / `open` 定序）、单写者的值（`g_fence`、`g_done`、`g_link`、`g_lastG`、`g_layout_logged`）。新的竞争照报。 |

ASan 每次跑会打一次「ignoring requested `__asan_handle_no_return`」：游戏线程的栈不是 ASan 认识的 pthread 栈。没有害处。

查出来并已修掉的问题：

- UBSan：`nvn_mtl.m` 对 `uint8_t` 的映射表判 `lmap[j] >= 0`，而 0xff 表示「没有映射」。这个判断恒真，于是从 8 项的表里读了第 255 项的清屏色。现在改成判 `< 8`。
- UBSan：`loader.c` 里不对齐的 8 字节重定位写入，`dm_vault.c` 里一次不对齐的 2 字节读，以及 `dm_world.c` 里对紧排的图形范围的不对齐读（在 UBSan 宿主上乱按时查出，见下文）。都改为经 `memcpy`。

`inventory`、`pause_menu`、`save_while_paused`、`enter_game` 和 `bench/town_loop` 在三种构建下都没有报告。`enter_game` 在普通宿主和 ASan 宿主上约 4 秒，在 TSan 宿主上约 23 秒。

## Metal 校验

```bash
python3 dm/tools/scenario.py run dm/verify/scenarios/inventory.scn --metal-validation api
make -C dm/host dmhost_m3 && SCN_DMHOST=$PWD/dm/host/dmhost_m3 python3 dm/tools/scenario.py run dm/verify/bench/town_loop.scn --metal-validation shader
```

| 档 | 打开 | 判定 |
|---|---|---|
| `api` | 接口校验（`MTL_DEBUG_LAYER=1`）；错误记进日志、不断言，警告不报 | 有消息就判红 |
| `lint` | `api` 再加警告（`MTL_DEBUG_LAYER_WARNING_MODE=nslog`）：多余或没用上的状态设置 | 只汇总 |
| `shader` | 着色器校验，报到 stderr；停顿上限放宽到 300 秒，因为每条管线都要插桩重编（每条约 0.15 秒） | 有消息就判红 |
| `all` | `api` + `shader` | 有消息就判红 |

消息汇总在 `metal_validation.txt`；命令行：`python3 dm/verify/assertlib.py metal <日志> [--lint]`。这个选项对 `cli` 和 `mac-app` 后端有效。iOS 后端起进程的方式不同，传不进这些变量。

发现：

- 开着着色器校验时，Metal 4 宿主第一次出图提交就崩在 Metal 自己的 `-[MTL4GPUDebugCommandBuffer applyResidencySets:]` 里，当时挂着图层的驻留集（macOS 27）。着色器校验请用旧接口宿主（`dmhost_m3`）。它跑同一批着色器；运行器见到这种组合会打一行提示。
- 上面那些场景在 `api` 和 `shader` 档下都没有消息。
- `lint`：12 秒的 `enter_game` 里约有 46,600 次多余的 `setComputePipelineState`，来自贴图反交错编码（`dm/gfx/tex/nvn_deswizzle_gpu.m`）：它在同一个编码器上每个级别、每层都设一次管线。开销不大（一次读取总共几毫秒），没有改。

## 自动探索

`dm/tools/explore.py` 按种子随机生成一串玩家操作，经场景运行器去跑，每一局都按场景的判据判：崩溃、红标事件、日志红标行、消毒器报告和看门狗。

```bash
python3 dm/tools/explore.py run --seed 21 --episodes 3 --steps 40 --dmhost dm/host/dmhost_asan
python3 dm/tools/explore.py gen --seed 21 --steps 40      # 打出一局的操作
```

- **操作：**`press`（任意键）、`stick`、`tap`、`world_tap`、`sleep`。都是不带断言的普通步骤，所以一局红了说明是游戏或宿主出了错，不是脚本本身。会主动结束或打断这一局的步骤（`quit`、`crash`、`app`、`background`）从不生成。
- **红了：**先重跑一次，看能不能复现。再用增量调试把操作串缩到仍以同一指纹红的最短一串。指纹是崩溃地点、红标事件码，或者抹掉数字后的红标行 / 消毒器摘要。
- **结果：**写成 `minimal_<k>.scn`，这是能直接用 `scenario.py run` 重放的场景。全部结果在 `$DM_ROOT/run/explore/<时刻>_<种子>/`，判定行 `EXPLORE OK|RED`，末行 `RESULT {json}`。
- **选项：**`--budget` 限制缩短的次数（缺省 24），`--allow-red` 放行已知的红标码，`--no-shrink` 不缩。

第一次的结果：在 UBSan 宿主上，种子 21 的三局都因 `dm_world.c` 的不对齐读而红，每局都在 7–11 次之内缩到一步（某处的 `world_tap`）。修好之后同一种子跑下来没有红。

## 回放与第一个不同的拍

`dm/tools/replay.py` 在跑场景时录下客户端命令包和逐拍状态（`DM_C2S_LOG`、`DM_TICKS_FULL`），之后在任意宿主构建上回放这些包（`DM_C2S_REPLAY`），逐拍比对。它回答的是：某个改动或某个构建有没有改变游戏逻辑。

```bash
python3 dm/tools/replay.py record dm/verify/bench/town_loop.scn                  # → $DM_ROOT/run/replay/town_loop_4242/
python3 dm/tools/replay.py check ~/dm-work/run/replay/town_loop_4242 --dmhost dm/host/dmhost --dmhost dm/host/dmhost_asan
python3 dm/tools/tickdiff.py a/ticks.jsonl b/ticks.jsonl                          # 任意两份 DM_TICKS 记录
```

`tickdiff.py` 报出两份记录第一个不同的拍，以及哪几格状态不同（玩家、怪物、物件、飞行物、物品、种子）。两边都带逐单位明细时，还说出是哪几个单位、哪些字段：`player id 1 cls 1: mode 3→5, yf 65437→32768`。`host_savecmp.sh` 原先调用的比对脚本不在仓库里，现在改用它。

确定性需要什么、工具怎么处理：

- **固定种子和时刻：**`DM_GAME_SEED` 与 `DM_FIXED_TIME`。场景自己设了的话，`record` 就用场景的值；`town_loop` 设了，它的坐标靠这两个值。
- **同样的进局路径和出生点：**回放走和 `enter_game` 宏同一条路进局。宿主会把城镇里的出生点挪到小站旁边（`dm_townwp.c`），但回放时缺省不挪，因为原版录制是从原版出生点走起的。所以回放设 `DM_TOWNWP_OFF=0`，照样挪。不这样的话，回放的起点差了大约 40 格，所有移动包都落空；几次回放之间一致，只是因为什么都没发生。
- **文件里不放加入握手：**拍 0 的系统类消息不进回放文件。回放在文件首末拍之间丢弃客户端自发的消息，首拍若是 0，宿主客户端自己的加入也会被丢掉，服务端就永远等下去。

实测：

- **构建之间一致：**`town_loop`（781 拍、144 条命令包，营地加血腥荒野）在普通、ASan 和旧接口宿主上回放，都与录制逐拍相同。
- **改动能查出来：**删掉第 405–500 拍的移动命令后，第一个不同的拍是 444：玩家走完已经下达的路线就停下了。

录制由游戏程序推出，只放在本机。
