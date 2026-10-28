# Fault scenarios

Run by `fork_gate.sh full` (`dm/verify/FORK_GATE.md`). Each case injects a save fault with the control port's `fault` command (or `DM_TEST_FS_FAILAT` / `DM_TEST_SAVE_DELAY` in its `env` line) and must end with:

- the `fault` event for the injected point (a fault that never fired fails its case instead of passing silently);
- the player told when a write failed (`save_alert`), and a refused commit (`save result=rejected`);
- later saves committing, and a valid committed save (`saves valid`, `dm/verify/d2save.py`);
- no `save_stuck`.

Points not reached on these routes: `guestname` and `renameback` (the game doesn't rename save files in a normal run); they stay covered by `dm/host/tests/t_fs.c`.

