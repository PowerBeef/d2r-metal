# Screenshot Pages

The screen baselines of `golden.py` compare the key screens drawn by the **host** (`dmhost` from `dm/host`, which runs the rewritten game program on a Mac) page by page with a **baseline image** recorded earlier from a known-good build, to catch whole blocks missing, lighting missing, or a distorted structure.

The criteria are deliberately not pixel-for-pixel: a baseline survives harmless changes in the GPU path (precision, ordering), and a real loss still goes red.

## Commands

```
python3 dm/verify/golden.py shots check [page…]                 # compare with the kept baselines
python3 dm/verify/golden.py shots record --reason "…" [page…]   # (re)record after a deliberate change
```

- Each page starts one host (`hostshot.sh`) from a fresh copy of the standard save, follows the key script in the page spec to that screen, and takes a screenshot at the trigger point with a deterministic clock; `shotgate.py` then compares it with the baseline. Baselines live in `$DM_ROOT/data/switch/golden/shots/<renderer>/<page>.png` with a `.meta.json` (commit, reason, time), and every recording is logged in `golden/LOG.tsv`.
- Each page prints one line `shot <page>: OK|RED …`; run artifacts and per-page `report.html` (baseline / host / difference heat map side by side, plus the criterion readings and the expected-elements table) are under `$DM_ROOT/run/golden/<time>_shots/`.
- `fork_gate.sh full` runs the check; with no baselines it is ⚪ unavailable.

| Environment variable | Effect |
|---|---|
| `DM_GATE_SAVE` | The save directory the host uses, default `$DM_ROOT/data/switch/save_ref_warlock/0000000000000001` |
| `DM_SHOT_PRODUCT` | Shoot another rewrite output (to compare rewrite variants) instead of the current `keypage` output |

## Criteria

`shotgate.py <page spec.json> <baseline.png> <host.png> <report dir>`. The two images are first scaled down to 320×180 and then compared; if any one criterion is red, the page is red. The default thresholds are in `DEF` in `shotgate.py`, and the page spec's `thresholds` can change them per page.

| # | Criterion | Default threshold | What it catches |
|---|---|---|---|
| ① | Structural similarity (SSIM): grayscale, 7×7 box window, masked areas not counted | ≥ 0.60 | Overall distortion of the picture |
| ② | Proportion of non-black pixels (luminance > 0.06) in the 3D viewport | host ≥ baseline × 0.90 | Whole blocks missing |
| ③ | 8×8 grid, cell by cell: mean luminance difference, mean color distance | ≤ 0.10, ≤ 0.16 | Local omissions and color casts; red cells are drawn into the heat map |
| ④ | Named regions (character, campfire, menu…): region luminance ratio host/baseline, region structural similarity | luminance ratio ∈ [0.75, 1/0.75], structure ≥ 0.45 | Missing lighting, located to a specific region |
| ⑤ | Fine grid (enabled only when the page spec has `fine_grid`): 640×360 grayscale divided into cells by "columns × rows", mean structural similarity per cell, looking only inside the viewport | The page spec gives the lower bound; number of red cells ≤ `fine_red_max` (default 0) | One small element not drawn |

A few more:

- **Expected-elements table** (the page spec's `elements`): each entry points to a named region and is ticked according to that region's verdict under ④; those pointing to `viewport` go by ②. An element written as 「无」 (none) means that this position must match the baseline (there is nothing there to begin with).
- **Named regions can have their own thresholds**: `{"rect": […], "ratio_min": …, "ssim_min": …}`. A `ratio_min` of 0 means only structure is compared, not luminance; this is for animated regions.
- **Animation mask** (the page spec's `dynmask`): a mask image in which white marks pixels the client itself animates (flames, flags, etc.); it is merged into the mask and takes no part in the comparison. When the mask covers more than 30% of the viewport, the report marks it 「弱判」 (weak verdict). For fine-grid cells more than half covered by the mask, the structure lower bound becomes `thresholds.dyn_ssim_min` (default 0.5).
- **Host log**: `golden.py` judges the image only. Red lines in host logs are caught by the scenario runs (`assertlib.py logred`, the keyword table in `logred.sh`).

Why ⑤ is needed: the average over a large region hides "one small element not drawn". The static pages of the UI layer (skill tree, options, stash, character stats, inventory, quest log, etc.) all carry a fine grid.

When `golden.py` runs the host it always sets `DM_BRIGHT_K=1`, turning off "3D scene brightening", a post-processing effect that deliberately deviates from the original, so that only the picture translation itself is checked.

## Page spec

`shotgate_pages/<page>.json`:

| Field | Meaning |
|---|---|
| `page` | The page's Chinese name |
| `truth` | Unused (the baseline image lives in `$DM_ROOT/data/switch/golden/shots/`) |
| `host.trigger` | When the host takes the screenshot: `<UI message substring>+<frame offset>`, i.e. how many frames after that message appears |
| `host.keys` | The host's key script (`DM_KEYS`); empty string = no key presses |
| `host.env` | Extra environment variables passed to the host (seed, fixed time, frozen tick, etc.) |
| `viewport` | The 3D viewport rectangle |
| `mask` | List of rectangles that take no part in the comparison |
| `regions` / `elements` | Named regions and the expected-elements table |
| `thresholds`, `fine_grid`, `fine_red_max`, `dynmask` | See the previous section |
| `note`, `trigger` | Explanatory text, not read by the program |

Rectangles are always written as `[x0, y0, x1, y1]`, with values 0–1, relative to the whole picture.

The syntax of `DM_KEYS` (defined in `dm/host/hle_hid.c`): `key@precondition message[%delay seconds][+hold seconds][>confirmation message]`, with multiple steps separated by commas — the key is pressed after the precondition message appears, and it counts as successful only when the confirmation message is seen. The precondition message can be written as `DM:Tick:<tick number>#`, meaning press when the server reaches that tick.

## Existing pages

| Page | Screen |
|---|---|
| `charselect` | Character select page |
| `newchar` | Class selection page for a new character |
| `loading` | Game-entry loading screen |
| `warlock_camp` | The camp after a Warlock character enters the game |
| `inventory` | Inventory |
| `char_stats` | Character stats panel |
| `skilltree` | Skill tree |
| `warlock_skilltree` | Warlock skill tree |
| `quest_log` | Quest log |
| `automap` | Automap overlay |
| `options` | Options page (pause menu) |
| `cube` | Horadric Cube (⚪ while the standard save has no Cube) |

Every page is triggered by a UI message plus a frame offset. There are no pages for combat and death screens.

Things the client itself animates (flames, ambient critters) don't line up between two runs; they are handled by animation masks or by loosening the thresholds of individual regions. `dynmask.py <a.png> <b.png> <output mask.png> [threshold=8] [dilation pixels=3]` makes a mask from two shots taken at the same frozen tick (`DM_TICK_HOLD=E:SHOT` freezes the game clock on entering tick E until the screenshot is written).

## Adding a page

1. Settle the trigger message and frame offset (a UI message from `DM_UIMSG`, see [ui_message_stream.md](ui_message_stream.md)).
2. Write `shotgate_pages/<page>.json`: mark out the viewport, the named regions and the expected elements; for static UI-layer pages add `fine_grid`.
3. Record it: `golden.py shots record --reason "new page <page>" <page>`.
4. Do a negative control: erase some element from a copy of the baseline and compare again; the page must report red. An omission the criteria fail to catch means the regions or thresholds are not set right.
