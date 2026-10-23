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
