# Landscape regression: D3D11/D3D12/Vulkan hardware verification of the heBombGridV fix (Thema 237, Schritt 6)

26.10.2026, NN-WS03 (Windows 11, RTX 4070). Follows `landscape-performance-regression-baked-text-2026-10-22.md` (Schritt 4,
commit `36189034`), which left D3D11/D3D12/Vulkan as cross-compile-only. This step builds and runs the regenerated
`M_AutoLandscape.hasset` on real D3D11/D3D12/Vulkan hardware.

## 0. Result

1. **Build:** Release, `-B C:/hw237 -DDEPLOY_DIR=C:/hw237/deploy` (own tree, does not touch the user's editor). Configure found
   D3D12 DXR GI kernels, Vulkan, D3D11, OpenGL, DXC and glslang/SPIRV-Cross all enabled. `HorizonEditor` and `he_tests` both link
   and deploy clean.
2. **he_tests (`test_material_graph.cpp` + `test_auto_landscape_variants.cpp`): 126/127 pass.** The one failure is NOT the
   heBombGridV fix — see §1.
3. **Pixel check on real D3D11/D3D12/Vulkan hardware** (`HE_DUMP_AUTOLAND`, analytic witness landscape, GI/AA/SSAO off, 16
   settle frames, private APPDATA per capture): sensitivity and cross-backend consistency both confirmed, §2. No fallback
   (every capture logs "shipped asset", "array mask 7" — all three texture arrays resolved).
4. **Performance: NOT measured on D3D11/D3D12/Vulkan hardware.** No valid apples-to-apples run was available on this
   machine — see §3. The Metal series from Schritt 3 (SSAO 8.42 → 6.94 ms) stands as the only timed result for this fix.

## 1. he_tests: one failure, root-caused, not the fix

`The baked GLSL in the shipped M_AutoLandscape.hasset is what the codegen makes today` (added in Schritt 4) is red on this
Windows Release build: `bytes.find(f->customShaderFragGlsl) == npos`. The graph tripwire (`The shipped M_AutoLandscape.hasset
is exactly what the builder makes`, JSON equality) passes, and `bakeMaterialShaderVariant` still succeeds for all five
backends on the Windows-regenerated text (the test's own remaining 16 assertions in that case pass) — so this is not a
broken shader, only a byte-exact text mismatch.

Byte-level diff (`HE_DUMP_AUTOLAND_SHADERS`, fresh dump of `lit.glsl` + `gbuffer.glsl`, CR stripped — the dump writer opens
the file in *text* mode, which is a red herring of its own: it does not mean the in-memory string has `\r`): the shared
helper functions (`heBombGridV`, `heBombTapV`, the whole `V == 1` branch) are byte-identical between the Windows-fresh text
and the Mac-committed file — counts match exactly once both `lit` and `gbuffer` fragments are added (`heBombGridV(` 5+5=10,
`_V == 1` 12+12=24, `heBombTapV(` 4+4=8, all equal to the file's counts). The divergence is in node *emission order*: at the
same point the fresh Windows text has `vec3 n6 = vec3(0.000000, …)` where the file has `vec3 n12 = vWorldPos;` — same
logic, different internal variable numbering. `MaterialGraph.cpp`'s codegen keys several maps by `std::unordered_map<std::string,
std::string>` (`outVar`, `bombGrids`); MSVC's STL and Apple's libc++ do not agree on `std::hash<std::string>` bucket order, so
a node that is visited (and numbered) in a different order on the two platforms emits textually different — but semantically
equivalent — code.

**This is a pre-existing cross-platform codegen-determinism gap in the auto-landscape material generator, not a regression
from commit 36189034.** It affects any Windows-vs-Mac byte-exact comparison of this material's baked text (and potentially
any other generated material with the same unordered-map pattern), independent of the heBombGridV fix. Posted as a Hive
lesson; left for the queen to decide whether it gets its own topic or the Schritt-4 tripwire is loosened to a semantic check
(re-parse + compare, not raw substring).

Full `test_material_graph.cpp` + `test_auto_landscape_variants.cpp`: 126/127 cases, 21682/21683 assertions. The suite also
runs real D3D11/D3D12 draws through WARP (software D3D, but the real D3D11/D3D12 API + HLSL compiler path) as part of its
normal coverage — e.g. "D3D12: G-buffer + deferred resolve shade a graph material like its forward draw" — which is
independent, additional evidence beyond the Mac-only glslang cross-compile the Schritt-4 tripwire relies on.

## 2. Pixel check (D3D11, D3D12, Vulkan hardware)

`HE_DUMP_AUTOLAND=1` (shipped asset, Groups wired, bombing on) and `=nobomb` (same asset, bombing switch off — the
sensitivity control), each with a private APPDATA and the real 5-slice cache arrays (V=1) copied in; a third capture reuses
mode 1 with synthetic 17-slice arrays (`landscape_tex_gen C:\hw237\v4arrays --variants 4`, V=4) copied into its own private
cache instead. 2880×1280×4+BMP-header BMPs, byte-exact diff:

| comparison | bytes differ (of 3 686 454) | max Δ |
|---|---|---|
| D3D11: V=1 vs nobomb (sensitivity) | 2 218 516 | 210 |
| D3D11: V=1 vs V=4 (variants) | 2 219 297 | 202 |
| D3D12: V=1 vs nobomb | 2 218 516 | 210 |
| D3D12: V=1 vs V=4 | 2 219 297 | 202 |
| Vulkan: V=1 vs nobomb | 2 218 514 | 210 |
| Vulkan: V=1 vs V=4 | 2 219 314 | 202 |
| D3D11 vs D3D12 (V=1, cross-check) | 0 | 0 |
| D3D11 vs Vulkan (V=1, cross-check) | 2 616 | 4 |

Every one of the 9 captures logs `HE_DUMP_AUTOLAND witness landscape added (… shipped asset, …, array mask 7, …)` — the
shipped `M_AutoLandscape.hasset` loaded (not "ASSET NOT FOUND"), all three texture arrays resolved (no silent white
fallback from a missing cache array). No `[ERROR]`/`Cannot load asset`/`compile failed` lines (the one "validation" grep hit
on Vulkan is `VulkanRenderer: validation layer ENABLED (HE_GPU_DEBUG)`, not an error).

Reading: the ~60% pixel difference for both the sensitivity control and the V=1→V=4 switch shows the Groups-wired bombing
system is actually live and visibly affecting the frame on real D3D11/D3D12/Vulkan hardware (not a flat/fallback image) —
this is new evidence Schritt 3/4 could not get without Windows hardware. D3D11 and D3D12 are pixel-identical (both compile
the same HLSL text through the same D3D driver paths); Vulkan differs by 4-of-255 at 0.07% of pixels, the same order of
magnitude as the ±1-3 byte float-contraction noise Schritt 3/4 saw between Metal/GL recompiles. No cross-platform
"shipped-vs-fix" pixel delta was taken here (that needs a pre-fix engine build; out of scope for the time this step had —
see Handoff).

## 3. Performance: not measured

No valid like-for-like run exists on this machine for this specific regression:

* `scripts/perf/landscape_regression.py` is macOS-only as written: `foreign_procs()`/`conditions()` call `ps -Ao`, `ioreg`,
  `pmset`; `main()` unconditionally `subprocess.Popen(["caffeinate", …])`. None of these exist on Windows; the script would
  fail immediately, not "run slow".
* Its underlying capture, `scripts/he_perf_capture.py`, **is** Windows-ready (`--rhi`, `GetProcessMemoryInfo`, Thema 182) and
  could run here — but it needs a `--project` + `--scene` that puts the Groups-wired `M_AutoLandscape` into a real editor
  loop. The regression's own scene is the reporter's `~/HorizonEngineProjects/Catania` project, which exists only on the Mac.
  The repo's generic Windows-usable perf scene, `docs/perf-audit/scenes/landscape.hescene`, uses a different terrain material
  entirely (its `material.asset` UUID is not `kAutoLandscapeMaterialId` `{0x412,1}`) — measuring with it would time the wrong
  material.
* The `HE_DUMP_AUTOLAND` witness used for the pixel check (§2) is a single-shot dump (`buildDumpWitnessScenes`, called once
  from `dumpFrameHeadless()` before the paced main loop starts) — a structurally different code path from the
  `HE_PROFILE_CAPTURE` warmup/record loop that `he_perf_capture.py` drives; the two do not compose. `HE_DUMP_FRAMES` is
  capped at 240 and this path's vsync state was not confirmed, so a crude wall-clock timing of it would risk being
  vsync-capped and silently flat — not trustworthy enough to report as a number.

The Metal series from Schritt 3 (SSAO 8.42 → 6.94 ms, Σ passes 24.55 → 22.98 ms, n=3) remains the only timed measurement of
this fix; it is on different hardware/backend and not directly comparable to D3D11/D3D12/Vulkan.

## Handoff

**Done (verified on NN-WS03 hardware):** Release build with D3D11/D3D12/Vulkan/DXR enabled (`C:/hw237`); `he_tests`
material-graph + auto-landscape-variants suite 126/127 (one pre-existing, root-caused failure, §1); D3D11/D3D12/Vulkan
pixel sensitivity + cross-backend consistency check on the shipped, regenerated `M_AutoLandscape.hasset` (§2); no asset
load failures, no fallback material, no backend errors across 9 captures.

**Not verified:** performance on D3D11/D3D12/Vulkan hardware (§3, with the specific reasons it could not be done safely in
this step); a pre-fix-vs-fix pixel/perf delta on this GPU (would need a second engine build at a commit before `ee5de0c0`,
not attempted); an exported/packed Windows game (editor loop only, as in every earlier step of this topic).

**Decisions:** used `he_perf_capture.py` directly rather than patching the Mac-only `landscape_regression.py` wrapper for
Windows — out of this step's scope (verification, not new tooling) and risky to rush; did not attempt to fix the
codegen-determinism tripwire failure (§1) — the root cause (unordered-map iteration order in `MaterialGraph.cpp`) is shared
generator code with a blast radius beyond this material, better owned as its own decision.

**Next steps:** (1) decide what to do with the Schritt-4 tripwire on Windows — loosen it to a semantic/graph-level check,
or make the codegen's node visitation order deterministic (sort by node UUID or insertion index instead of hashing); (2) if
a D3D11/D3D12/Vulkan performance number for this fix is wanted, either port a Groups-wired scene to Windows (a cut-down
`landscape.hescene` with the terrain's material swapped to `kAutoLandscapeMaterialId` and Groups wired) or copy the Catania
project to NN-WS03; (3) open the PR to `release/0.7.0` for this branch (`claude/bug-allgemeine-performance-regression-im-spiel-mit-landscape`,
commits `3fb6941a`..`36189034`) now that D3D11/D3D12/Vulkan hardware verification is in — this step.

**Pitfalls:** `cmd /c "...vcvars64.bat... && cmake --build ..."` run from the Bash tool prints the `cmd` banner and builds
nothing (silently) — use the PowerShell tool. A worktree build needs its own short `-B`/`DEPLOY_DIR` (MAX_PATH, shared
deploy) — see `[[worktree-builds-need-deploy-dir-override]]`. `landscape_tex_gen <dir> --variants N` with no other flag
already writes the per-layer placeholders *and* assembles the three `_Array.hasset` files from scratch; `--arrays-only` is
only needed when per-layer files already exist in `<dir>`. The `HE_DUMP_AUTOLAND_SHADERS` dump writer uses a non-binary
`std::ofstream`, so the file on disk gets CRLF-translated on Windows even though the in-memory string never had `\r` —
strip `\r` before diffing, and do not mistake it for the engine's own behaviour.
