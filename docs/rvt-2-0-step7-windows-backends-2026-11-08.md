# RVT 2.0 step 7: Windows (D3D11/D3D12/Vulkan/OpenGL), Thema 335

Steps 1-6 were Metal-only (this Mac, docs/rvt-2-0-step{4,5,6}-*.md). This step checked whether the node
compiler/split (`RvtSplit.cpp`, `MaterialGraph.cpp`, HE_Core — backend-independent) and the page-pool driver
(`RvtLandscape.cpp`, HE_Scene — backend-independent, calls `IRenderer::RenderRvtPages()` which each backend
already implemented in Thema 333 steps 9-10) carry over to Windows without a port, and found a real test bug
the step was meant to catch.

## What was actually touched by steps 1-6 per backend

`git diff --stat c63f24228^..255b0d5bb` (the 7 RVT 2.0 commits) against `src/HE_Rendering`: Metal +10 lines,
OpenGL +6 lines, **D3D11/D3D12/Vulkan untouched**. The page pool (BC7 cold pages, RG8 pack, Thema 333 steps
9/10) is generic — it renders whatever material graph is bound into a page, so the new page/live split just
runs through the existing path. Confirmed empirically below: no backend-specific RVT code was needed for the
split, pool, journal repaint or baked base.

## Fixed: stale D3D12 root-signature test (real bug, not a renderer bug)

`tests/test_material_graph.cpp`, `onlyRvtBindings()` (~line 5827): written for Thema 333's original RVT
(table t40..t43 only). The RVT 2.0 split's Read node also samples the cold pages (`heRvtAlbedoC`/`NormalC`/
`SurfaceC`, t44..t46, `RvtLookup.h:14,43-45`) added later in Thema 333 step 9, and this D3D12-only negative
control (`"D3D12: every node's bytecode binds only registers the material root signature covers"`,
~line 6350) was never updated for them — it failed with `'RVT Sample' without the RVT range:
heRvtAlbedoC(2:44)` because the helper didn't recognise that name as an RVT binding. Added the three cold
names to the helper's allow-list. Not a D3D12 root-signature bug: the *first* check in the same test case
(the full signature covers `heRvtAlbedoC` already) was passing before the fix.

## Verified (NN-WS03, RTX 4070, Release, build `C:/hwRvt335`, `-DDEPLOY_DIR=C:/hwRvt335/deploy`)

- `he_tests.exe --source-file="*test_rvt_*,*test_material_graph*,*test_landscape*"`: **216/216 passed, 36111
  assertions**, after the fix above (was 215/216, the one failure being the stale helper). Covers D3D11/D3D12
  FXC compiles of every node shader + root-signature coverage, the Vulkan/GL codegen text checks in
  `test_rvt_shader.cpp`, and the RVT split/journal/base-bake unit tests.
- `HorizonEditor.exe` (Release) and `he_tests.exe` build clean, **0 new warnings** (only the pre-existing
  doctest `C5285` tuple-specialization note).
- End-to-end witness (`HE_RVT=1 HE_DUMP_AUTOLAND=1 HE_DUMP_RVTDRIVER=1 HE_DUMP_RVTBASE=1 HE_DUMP_RHI=<backend>
  HE_HIDDEN_WINDOW=1 HE_DUMP_QUIT=1`, scratch APPDATA per backend, 128 m Mountains landscape, placeholder
  arrays) ran clean on **all four** backends, numbers matching the Metal witness (steps 4/6) closely:

  | | GL | Vulkan | D3D11 | D3D12 | Metal (step 4/6 docs) |
  |---|---|---|---|---|---|
  | pages vs whole material (reference) | 1.06 | 1.05 | 1.06 | 1.06 | 1.07 |
  | baked base vs reference (mean/max) | 0.13/36 | 0.12/41 | 0.13/44 | 0.12/44 | 0.13/42 |
  | sculpt stroke: stale/repainted | 7/7 | 7/7 | 7/7 | 7/7 | — |
  | paint stroke: stale/repainted | 6/6 | 6/6 | 6/6 | 6/6 | — |
  | total pages painted (driver run) | 78 | 78 | 78 | 78 | — |

  Each run also showed: pool creation (`512 pages of 136 px, 108 MB`), baked-base export+pak-mount+use
  (log: `"... attached ... (pages from the export's baked base, the page graphs are not built)"`), a ground
  edit dropping the baked base (`"... leaves the export's baked base (the ground was edited)"`) and
  re-attaching from the page graphs, "fresh driver" key checks (baked when ground matches, not baked after a
  vertex edit), and the "after the Read" vs "before the Write" parameter-change split (snow line: 0 rebuilds,
  1 live-only change; rock slope: 1 rebuild, 21 pages stale, repainted) — all matching the step 4 handoff's
  description exactly.
- `scripts/perf/landscape_rvt_pool_regression.py` (new): drives the editor once per backend with the same
  witness, parses the pool/paint/baked-base log lines, writes one JSON report. Ran clean for all four
  backends (`C:/hwRvt335/rvt-pool-regression.json`); `total_paint_ms` includes first-page shader-compile cost
  (D3D11 198 ms .. Vulkan 854 ms for the same 183 painted-page count) so it is **not** a steady-state
  perf comparison, only a functional/regression check — see Not verified.

## Not verified / known gaps

- **Debug view mode (step 5) is a bigger, separate gap than "port a flag".** `UnlitViewActive()` /
  `RvtDebugViewActive()` (and the shading + deferred-resolve branches that read them) exist **only** in
  `MetalRenderer.h`/`.mm` and `OpenGLRenderer.h`/`.cpp` (`fillMatLight` sets `lit.viewMode[0]`/`[1]` at
  `OpenGLRenderer.cpp:12803-12804`). D3D11/D3D12/Vulkan have **zero** references to Unlit/Wireframe/RvtDebug
  view modes anywhere — `m_viewMode` there only drives the unrelated G-buffer-channel debug view
  (basecolor/normal/...). This pre-dates RVT 2.0 (Unlit/Wireframe were apparently never ported either) and
  is confirmed by the witness: GL's debug-view clean-vs-stale picture delta is 13.9/255 (matches Metal's
  colourised page map), D3D11/D3D12/Vulkan's is ~0.25/255 (just the ordinary picture changing because a stale
  page serves slightly different data, not the colourised overlay). **Queen's call (hive_request #170,
  confirmed):** out of scope for this step — implementing it would mean adding the whole view-mode concept to
  three 5k-13k-line renderer files, not porting RVT. Needs its own step.
- Real landscape texture arrays: this worktree's deploy has no `Engine/Textures/Landscape/*_Array.hasset`
  (known, memory `landscape-reload-repro`), so every witness run logs ~63-67 "Cannot load asset" errors
  (white-fallback texture, cosmetic) and the RVT pages paint the fallback, not real art. The numeric deltas
  above are still meaningful (pages vs whole-material, baked vs reference) because both sides use the same
  fallback.
- `total_paint_ms` in the regression script includes first-call shader-compile time (D3D11 PSO/FXC compiles
  took up to 1.3 s in the log); a real per-backend paint-time comparison needs a warm run or to subtract the
  first N page-paint events, not done here.
- D3D11/D3D12/Vulkan specific HLSL/SPIR-V negative controls beyond what `test_material_graph.cpp` already
  does (FXC accept/reject probes) were not added; the fix above only restores what was already covered.
- No real export -> HorizonGame pak run on Windows (same gap step 6 left open; the editor-side pak mount in
  the witness stands in for it, as on Metal).

## Handoff

- **Topic / step:** Thema 335, step 7 of 7
- **Branch / commit:** `claude/rvt-material-feature` @ (see this commit)
- **Done:** RVT 2.0 (split node compiler, page pool, baked base, journal-driven repaint) verified working on
  D3D11, D3D12, Vulkan and OpenGL on real hardware (RTX 4070), matching the Metal numbers closely. One stale
  test helper fixed (D3D12 root-signature coverage test). A reusable per-backend pool/paint measurement
  script added.
- **Files touched:** `tests/test_material_graph.cpp:5827-5833` (fix), `scripts/perf/landscape_rvt_pool_regression.py` (new).
- **Verified:** see above — he_tests (216/216), clean builds, 4x end-to-end witness, 4x regression script run.
- **Not verified:** see above — debug view parity on D3D11/D3D12/Vulkan (separate scope), real arrays, warm
  paint-time comparison, real export->game pak mount on Windows.
- **Decisions taken:** treated the debug-view gap as pre-existing/out-of-scope per explicit queen confirmation
  (hive_request #170) rather than implementing Unlit/Wireframe/RvtDebug on three renderers in this step.
- **Pending:** a follow-up step for Unlit/RvtDebug view-mode parity on D3D11/D3D12/Vulkan; per-layer stale
  flags (step 4 handoff, still untouched); LandscapeStore part (b) of step 6 (blocked on another branch);
  a warm (post-first-paint) perf comparison across backends with real texture arrays.
- **Traps:** `onlyRvtBindings()` in `test_material_graph.cpp` is a hand-maintained allow-list, not derived from
  `RvtLookup.h` — the next binding RVT 2.0 adds there needs the same manual update or D3D12's root-signature
  coverage test fails again with a message that looks like a renderer regression. `cmd /c` quoting from the
  PowerShell tool needs the whole `vcvars64.bat && ...` string in **single** outer quotes, not double, or it
  silently builds nothing. `python3` is not on PATH on NN-WS03 (Store alias); use `C:\Python314\python.exe` or
  bare `python`.
- **Next steps:** 1. a follow-up step/topic for Unlit/Wireframe/RvtDebug on D3D11/D3D12/Vulkan (reuse
  `OpenGLRenderer.cpp:12803-12804`/`MetalRenderer.mm:16619-16621` as the reference). 2. real landscape arrays
  in a Windows deploy for a true visual/perf A/B. 3. per-layer stale flags. 4. warm paint-time measurement.
