#!/usr/bin/env python3
# 起查询服务 pyghidra-mcp（re.sh serve 调用）。和直接跑 pyghidra-mcp 的唯一区别：缺省不建「代码向量索引」——
# pyghidra-mcp 在启动时、以及任何一次工具调用时都会排一次索引：把 5.7 万个函数全反编译一遍再算向量（还要下载向量模型），
# 一次占满 CPU 与内存一个多小时；这里只保留便宜的字符串表（search_strings 要它），search_code 不可用。
# --index 恢复原样（启动即建全量索引，search_code 可用）。其余参数原样交给 pyghidra-mcp。
# Starts the pyghidra-mcp query server (called by re.sh serve). The only difference from running pyghidra-mcp directly: no "code embedding index" by default --
# pyghidra-mcp schedules indexing at startup and on every tool call: it decompiles all 57,000 functions and computes embeddings (downloading an embedding model too),
# taking the CPU and memory for over an hour; here only the cheap string table is kept (search_strings needs it), and search_code is unavailable.
# --index restores the original behaviour (full index at startup, search_code available). All other arguments go to pyghidra-mcp unchanged.
import sys

if __name__ == '__main__':
    args = sys.argv[1:]
    if '--index' in args: args.remove('--index')
    else:
        from pyghidra_mcp.indexing_mixin import IndexingMixin
        full = IndexingMixin.schedule_indexing
        IndexingMixin.schedule_startup_indexing = lambda self, **_: None
        IndexingMixin.schedule_indexing = lambda self, name, *, code=True, strings=True: full(self, name, code=False, strings=strings)
    from pyghidra_mcp.server import main
    sys.argv = ['pyghidra-mcp'] + args
    main()
