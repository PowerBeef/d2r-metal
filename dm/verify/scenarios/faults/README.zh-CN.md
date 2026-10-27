# 故障场景

`fork_gate.sh full` 跑（见 `dm/verify/FORK_GATE.zh-CN.md`）。每例用控制口 `fault` 命令（或 `env` 行里的 `DM_TEST_FS_FAILAT` / `DM_TEST_SAVE_DELAY`）注入一个存档故障，结尾必须：

- 有注入点的 `fault` 事件（故障没打到就判这例红，不会悄悄过）；
- 写失败时告诉了玩家（`save_alert`），提交被拒（`save result=rejected`）；
- 之后的存盘照常提交，已提交的存档合法（`saves valid`，`dm/verify/d2save.py`）；
- 没有 `save_stuck`。

这些路线上到不了的点：`guestname`、`renameback`（正常玩游戏不改存档文件名）；它们仍由 `dm/host/tests/t_fs.c` 覆盖。

English: [README.md](README.md)
