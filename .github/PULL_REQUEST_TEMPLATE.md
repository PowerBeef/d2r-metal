<!-- Read CONTRIBUTING.md first. -->

## What changed

<!-- What this change does, in a few sentences. -->

## Why

<!-- The problem it solves. -->

## Checks run

<!-- Tick what you actually ran on this change and paste the last line of output. Leave unticked what you did not run. -->

- [ ] `dm/verify/fork_gate.sh auto` (runs `quick unit`, about 4 min, and prints the short checks this change touches)
- [ ] The short checks `auto` printed (`dm/tools/gate_pick.py explain`): list them below
- [ ] `dm/verify/fork_gate.sh core` (only when asked for by name, about 9 min)
- [ ] `dm/verify/fork_gate.sh full` (only when asked for by name, about 30 min; the whole gate `quick unit core full` runs on the commit a release or an app rebuild is cut from)
- [ ] `dm/verify/regress.sh --fresh` (if `dm/tools/patch_nso.py` changed)
- [ ] `python3 dm/verify/golden.py replay check` / `shots check`

Anything else you ran, and on what (Mac host, Mac App, iOS package on a Mac, which device):

## Visible changes

- [ ] Nothing on screen changes
- [ ] Something on screen changes — screenshots taken after the change are attached below
- [ ] New on-screen text is in `dm/i18n/ui_text.tsv`, and `dm/host/dm_i18n_tab.h` was regenerated

## Engine changes

- [ ] Nothing the engine does changes
- [ ] A rewrite entry, shader rule, shader key, engine field or take-over changes, and its row in `docs/ENGINE_CHANGES.md` is added or updated in the same commit (`python3 dm/tools/enginemap.py check` → `ENGINEMAP OK`)

## No game content

- [ ] No game files, nothing generated from game files, no keys, no built package
- [ ] No engine-analysis output (decompiled code, disassembly, database files)
- [ ] Not the local rewrites `./setup.sh` makes to `dm/gfx/tools/dmsh_chain.lock`, `dm/gfx/nvn/dmsh_chain.h` and `dm/gfx/nvn/c1_full.tsv`
