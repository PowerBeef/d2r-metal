# dm/tools/re：引擎分析库

English: [README.md](README.md)

游戏 `main` 程序的一份私有、长期保存的 Ghidra 分析库，让修改建立在引擎实际怎么做之上。会话和人通过本机的查询服务查它；它攒下的名字会回流到仓库的调试工具里。

**它产生的一切都只留在本机的 `$DM_ROOT/re`（缺省 `~/dm-work/re`）。** 反编译出来的代码、反汇编、字符串清单和 Ghidra 文件，一律不进本仓库、议题、PR 或网页（CONTRIBUTING 第 1 条，[docs/DERIVED.zh-CN.md](../../../docs/DERIVED.zh-CN.md)）。能进仓库的仍是现在就有的那些：地址加我们自己写的说明，以及像本目录这样的原创工具代码。本机的 `pre-commit` 守卫会拒收反汇编器的库文件，以及看起来像反编译输出的暂存行。`$DM_ROOT/re` 为导出和笔记留有自己的本机 git 历史：没有远端，推送一律被钩子拒绝。

## 需要

- Ghidra 12.2（`brew install ghidra`，再 `brew pin ghidra`：升级会单向转换工程）与 Java 25（`brew install openjdk@25`）。
- PyGhidra 要 Python 3.13（它带的 JPype 只到 3.13）和 `uv`。`re.sh setup` 建 `$DM_ROOT/re/venv`，不用项目的 venv。
- 已提取的游戏文件（`./setup.sh`）、工程约 3 GB 磁盘，以及内存：同一时刻只跑一个 Ghidra 进程，不和游戏运行或 Xcode 构建同时跑。

## 建库（一次）

```bash
dm/tools/re/re.sh setup       # $DM_ROOT/re、3.13 venv（pyghidra、pyghidra-mcp）、本机历史
dm/tools/re/re.sh convert     # ExeFS 的 main → re/in/main.elf（nso2elf.py）
dm/tools/re/re.sh import      # 导入工程；每个 .eh_frame 起点建函数；标出不返回的导入
dm/tools/re/re.sh check       # 重定位、重定位后字节、text 字节、eh_frame 函数 → CHECK OK
dm/tools/re/re.sh analyze     # Ghidra 自动分析，含 Decompiler Parameter ID（很久：放后台跑）
dm/tools/re/re.sh baseline    # 每个函数反编译一遍 → log/decomp_baseline.json
dm/tools/re/re.sh seed        # 仓库已知的东西（见下）
dm/tools/re/re.sh export      # 名字与类型导成文本，提交进本机历史
dm/tools/re/re.sh snap        # .gzf 快照（只留最近两份）
```

- `nso2elf.py` 把 NSO 转成标准 ELF 共享对象，让 Ghidra 自己的 ELF 加载器施加全部 63,284 条重定位、给 656 个导入起名。它补上节头（Ghidra 只对叫 `.eh_frame*` 的块分析展开表、只把 `.got*` 当只读）。一个字节都不改。
- **地址。** 程序装在基址 `0x7100000000`，工程地址的低 32 位就是 `main+0x…` 偏移：Ghidra 叫作 `FUN_71…` 后接 `005d3bc0` 的函数即 `main+0x5d3bc0`。工具的输入输出都写 `main+0x…`。

## 查询

工程同一时刻只能被一个进程打开。查询服务在跑时它就是那个进程；其余命令在 `re.sh stop` 之前都拒绝启动。

```bash
dm/tools/re/re.sh serve       # pyghidra-mcp，127.0.0.1:8000（serve.py：启动时不建全量索引；--index 才建）
dm/tools/re/re.sh q decompile main+0x5d3bc0 [--callees --xrefs --strings]
dm/tools/re/re.sh q xrefs main+0x2f05458
dm/tools/re/re.sh q callgraph main+0xb1dc60 [--called]
dm/tools/re/re.sh q strings PanelManager      # 另有：find <名字>、bytes <地址> [个数]
dm/tools/re/re.sh q name main+0x… NewName     # 函数改名（存盘）
dm/tools/re/re.sh q comment main+0x… "它做什么"
dm/tools/re/re.sh stop        # SIGINT：服务存盘后关工程
```

在主检出的 Claude Code 会话里，同一个服务就是一个 MCP 服务：`claude mcp add --transport http -s local ghidra http://127.0.0.1:8000/mcp`。worktree 会话和子代理用 `re.sh q`。

## `seed` 写些什么

全部可重复跑；手工起的名字（Ghidra 的 `USER_DEFINED`）一律不覆盖。

- `dm/gfx/nvn/nvn_slots.tsv` 里每个 NVN 函数指针槽，名为 `pfn_<名字>`，`nvn.h` 解析得了就带上函数类型。
- 每条提到 `main+0x…` 的源码注释，作为该地址上以 `[dm]` 开头的前置注释。
- 改写规则（构建产物的 `main.cuts/obs/params.tsv`），作为 `dm-rewrite` 书签。
- `dm/host/dm_widget.h` 的控件虚表，名为 `vt_<类>`；以及 `/dm` 下的结构体：控件基类与各类专有字段、单位 → 路径 → 房间 → 关卡这条链（`dm_unit.h`、`dm/verify/tick_state_format.md`）。
- 人工整理的名字表 `$DM_ROOT/re/names.tsv`：`偏移<TAB>名字<TAB>可信度<TAB>来源`，一行一个名字，`#` 起的行是注释。可信度为 `high`、`medium` 或 `low`。

## 名字去哪

`export` 写出 `$DM_ROOT/re/export/main.names.tsv`（偏移、名字、种类、可信度、来源）与 `types.h`。`dm/tools/guestsyms.py` 发现这个名字文件就读它，于是 `crashlog.py`、lldb 里的 `dm where` / `dm bt` 和 `sampagg.py` 都显示引擎函数名；可信度不是 `high` 的名字后面带括号注明。

## 文件

| 文件 | 做什么 |
|---|---|
| `re.sh` | 总入口：环境、工程锁、服务起停 |
| `nso2elf.py`、`test_nso2elf.py` | NSO → ELF；用手工拼的 NSO 做单测（不读游戏文件） |
| `ghidra_db.py` | `import`、`analyze`、`baseline`、`check` |
| `seed.py` | 把仓库里的已知信息写进工程 |
| `export.py` | 名字与类型导成文本，提交进本机历史 |
| `serve.py` | 不建启动索引地起 pyghidra-mcp |
| `q.py` | 查询服务的命令行客户端 |
