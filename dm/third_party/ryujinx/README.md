# Ryujinx shader translator (vendored subset)

Three projects from Ryujinx, as released by Ryubing (the project's continuation, <https://git.ryujinx.app/projects/Ryubing>), unmodified:
`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`, with the repository's `Directory.Build.props` and `Directory.Packages.props`.
They are byte-identical to the tag `Canary-1.3.351` (commit `475615f043`, 2026-08-26; also unchanged in `Canary-1.3.352`), checked on
2026-11-03 by diffing them against that tag's source archive.

The shader translation tool in `dm/gfx/tools/dmsh_xlate` references them to translate the original Switch shaders to SPIR-V.
License: MIT (Copyright (c) Ryujinx Team and Contributors), see `LICENSE.txt`, identical to upstream's; `src/Spv.Generator/LICENSE` is that
subproject's own MIT text.
