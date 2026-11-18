# Landscape 3.0 Folge (Thema 329), step 6: pack real textures, acceptance, handoff

Branch `claude/landscape-3-0-folge-path-layer-prozeduraler-schnee-sand-mate`, head `db52d112` + this
doc. Read `docs/landscape-3-0-folge-overview-2026-10-30.md` first (steps 1-7 summary); this doc only
covers step 6's second run.

## Goal

Real textures for Forest/Grassland/Desert packed, five-backend witness, `he_tests` green, Topic 327
merge-tree re-check, handoff. No code or asset changed in this run — see "Not done" below.

## What changed

Nothing in the repository. This run only re-verified the branch and fixed a **local build
environment** problem (not a code bug, nothing committed for it):

* My first configure on this device picked up VS2022's `cl.exe` (toolset 14.44) instead of VS 18's
  (14.51.36231). 14.44 hits a deterministic MSVC `C2026` ("string too long") on
  `src/HE_Rendering/src/material/MaterialShaderLibrary.cpp`'s `kLightingPreamble` raw-string literal
  (~37 KB, well under the documented 65535-byte limit — reproduces standalone in a throwaway `.cpp`
  with just that literal, consistently at ~16 KB-aligned offsets: an internal lexer buffer-boundary
  bug in that specific toolset, not a real length violation, and unrelated to this topic's own files
  — the file is byte-identical to `origin/main`). VS 18's 14.51 does not have this bug. Fixed by
  deleting `CMakeCache.txt`/`CMakeFiles` (kept `_deps/`) and reconfiguring under VS 18's
  `vcvars64.bat` (`C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`).
  Logged as a Hive lesson (id 653) for the next Windows bee — this matches the existing
  `build-test-cli-workflow` memory's "use VS18's vcvars, never 2022's" warning, which was about a
  different symptom (linker/STL-symbol mismatch) of the same root cause.
* Also hit and fixed (Hive lesson 652): Git-Bash's `cmd /c "..."` silently mangles the `/c` into a
  path (MSYS argument conversion) and runs nothing, printing only the version banner. `cmd //c` (or
  a `.bat` file invoked by path) avoids it.

## Decisions

* **Still not guessing the texture-picker mapping.** Checked (again) everywhere the picker result
  could plausibly land: `~/Downloads` (only unrelated UI icons + the 42 unmapped Poly Haven zips the
  last run already found), `scripts/landscape-textures/`'s staging output (empty),
  `EditorDeps/EngineContent/Textures/Landscape` (does not exist in the repo at all), Hive posts and
  `hive_requests` (nothing addressed to this run). Same finding as the run 19 days ago. Posted
  `hive_request` #183 to the queen rather than guess or stay idle.
* **Used placeholder arrays for Forest/Grassland/Desert in the witness**, generated into a scratch
  dir (`landscape_tex_gen <scratch-dir> --group <G>`, no mode flag → `writeGroupArrays`), never into
  `EditorDeps`. This is the only thing possible without the real source PNGs; it establishes a clean
  Windows D3D11/D3D12/Vulkan/OpenGl baseline for all four groups, which the branch did not have
  before (NN-WS03 was offline for the run 19 days ago).
* **Mountains source**: copied (read-only) the three real `T_Landscape_*_Array.hasset` files out of
  this machine's own `%APPDATA%\HorizonEngine\EngineContentCache\Textures\Landscape` into a scratch
  dir, the same thing `cap233_win.ps1`'s own header describes. Never touched the real cache itself.

## Verified

* **Build** (`C:/hb6`, VS 18 toolset, Release): `he_tests`, `landscape_tex_gen`, `HorizonEditor` all
  built and deployed clean, `cmake --build C:/hb6 --target he_tests landscape_tex_gen HorizonEditor -j 8`.
* **Tests**: `ctest --test-dir C:/hb6 -j4 --timeout 180` — **348/348 passed** (173 s), scratch
  `APPDATA`/`LOCALAPPDATA`, `HE_NET_LOOPBACK_ONLY=1 HE_HIDDEN_WINDOW=1`. Matches the "348+" baseline
  from `docs/landscape-3-0-pack-real-textures-2026-10-29.md`.
* **Topic 327 merge-tree re-check**: `git merge-tree <merge-base> HEAD
  claude/neuer-asset-typ-landscapefoliagelayer-foliage-spawn-pro-land` — **0 `CONFLICT` lines**; all
  seven shared files (`MaterialGraph.cpp/.h`, `TerrainTools.cpp`, `EditorApplication.cpp`,
  `EditorHelp.cpp`, `SceneSerializer.cpp`, `test_material_graph.cpp`, `test_terrain_tools_ui.cpp`)
  show as a clean "changed in both" with non-overlapping hunks. Still holds, not built (same as the
  prior run's finding).
* **Five-backend baseline witness**, `scripts/landscape-3-0-accept/cap233_win.ps1` (private
  `APPDATA`/`HE_CONFIG_DIR` per shot, 16 settle frames, quits after dump), 16/16 combos — Mountains
  (real cached arrays) × {D3D11, D3D12, Vulkan, OpenGL}, Forest/Grassland/Desert (fresh placeholder
  arrays) × the same four:
  * All 16 rendered and dumped a BMP; `dump counters` sane and **consistent per backend family**
    across all four groups: D3D11/Vulkan/OpenGL report `draws=4 tris=34816 visible=4/4`, D3D12
    reports `draws=16 tris=139264 visible=4/4` (D3D12's own batching granularity, not a bug — see
    below).
  * D3D11 and D3D12 are **pixel-identical (same MD5)** within every group despite the different
    draw/tri counts — matches the pixel-parity pattern topic 327's step 6 doc already recorded
    between these two backends on this project.
  * Every one of the 16 shots logs exactly **one** `Cannot load asset` line, always the same asset:
    `T_Landscape_<Group>_Path_Albedo.hasset`. Confirmed harmless and pre-existing, not introduced by
    this run: `EditorDeps/EngineContent/Textures/Landscape` has no committed files at all (checked
    with Glob), so the editor's own deploy bundle never had this file for *any* group on *any*
    backend; the Path layer is paint-only and nothing paints it in this synthetic scene, so the
    missing texture is never sampled and never shows in the render.
  * Full per-shot md5/missing-logs/counters: `C:\hb6\witness\summary.txt`, attached to this topic.

## Not verified / not done

* **Real Forest/Grassland/Desert textures.** The human's picker assignment does not exist on this
  device. Nothing was packed into `EditorDeps/EngineContent/Textures/Landscape`, nothing uploaded to
  EngineContent. This is the one thing step 6 cannot finish without external input.
* **Metal** on this run (Windows device; Mac witness for Mountains' rock-by-size already exists from
  step 5 — `docs/landscape-3-0-rock-by-size-2026-10-30.md`). Forest/Grassland/Desert have never been
  witnessed on Metal/GL with their own placeholder/real arrays as a set; only Mountains has.
* The queen's "2.jpg" screenshot (hard grass/rock edge along a height line on a steep slope) — asked
  the queen in `hive_request` #183 whether that belongs in this step; not touched either way.

## Next steps

1. Human delivers the picker assignment → `landscape_tex_gen EditorDeps/EngineContent/Textures/Landscape
   --pack <png-dir> --group <G> --size 2048` per group (Forest/Grassland/Desert), then `--material
   --group <G>` only if `test_material_graph`'s shipped-hasset tripwire says the graph changed, then
   re-run this step's witness recipe with the **real** arrays instead of placeholders, then publish
   via the editor's "Publish Engine Content to Server". Season textures are Topic 330, summer only
   here.
2. Resolve `hive_request` #183 (texture assignment status; whether the 2.jpg slope-banding issue is
   this step's job or a new one).
3. Optional: a Mac bee repeats the Forest/Grassland/Desert placeholder witness on Metal/OpenGL for a
   complete five-backend baseline set (Mountains' Metal/GL already exists from step 5).
