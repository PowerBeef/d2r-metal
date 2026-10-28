<!-- Read CONTRIBUTING.md first. -->

## What changed

<!-- What this change does, in a few sentences. -->

## Why

<!-- The problem it solves. -->

## Checks run

<!-- Tick what you actually ran on this change and paste the last line of output. Leave unticked what you did not run. -->

- [ ] `dm/verify/fork_gate.sh quick unit`
- [ ] `dm/verify/fork_gate.sh core` (host or graphics changes)
- [ ] `dm/verify/fork_gate.sh full` (before an app rebuild)
- [ ] `dm/verify/regress.sh --fresh` (if `dm/tools/patch_nso.py` changed)
- [ ] `python3 dm/verify/golden.py replay check` / `shots check`

Anything else you ran, and on what (Mac host, Mac App, iOS package on a Mac, which device):

## Visible changes

- [ ] Nothing on screen changes
- [ ] Something on screen changes — screenshots taken after the change are attached below
- [ ] New on-screen text is in `dm/i18n/ui_text.tsv`, and `dm/host/dm_i18n_tab.h` was regenerated

## No game content

- [ ] No game files, nothing generated from game files, no keys, no built package
- [ ] No engine-analysis output (decompiled code, disassembly, database files)
- [ ] Not the local rewrites `./setup.sh` makes to `dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h` and `dm/gfx/nvn/c1_full.tsv`
