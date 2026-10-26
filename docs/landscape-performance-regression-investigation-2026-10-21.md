# Landscape in the game: general FPS drop, broad investigation (Thema 237, Schritt 1)

21.10.2026, MacBook Air M5 (10 cores, 24 GB), Metal; OpenGL where marked. **Investigation only, no fix, no source change.**
Raw data: `perf-audit/raw-landscape-regression/` (`*.summary.json`, `*.scopes.json`, `*.conditions.txt`, `*.log`, `binaries.txt`;
the 3 MB `*.profile.json` are not kept, like in the Thema 180 audit). Tools: `scripts/perf/landscape_regression.py` (one run),
`scripts/perf/landscape_regression_table.py` (tables), `scripts/perf/landscape_regression_tools/` (the shell glue used).

## 0. Result in six lines

1. **The drop reproduces, but it is not one commit.** The player's exported game (Catania, `Export/Development`, GI on, 2880x1806) runs **40.2 FPS on Metal and 29.4 FPS on OpenGL**
   (one run each). Per-pass GPU on Metal (current build): **Scene 13.7 ms (12.7 ms of it volumetric clouds), SSAO 8.6 ms**, rest ~3 ms, sum 25.2 ms.
2. **None of the three suspects carries the drop on Metal.** Thema 180 is the only one that costs anything measurable: **+1.5 ms (+6 %, ~2 FPS), all in the SSAO pass**,
   and it is the new `M_AutoLandscape.hasset` (asset swap A/B, section 4). GI diffuse costs ~0.5 to 1 ms. Water in view costs ~0 ms here. Thema 191 is **not in this tree** and GI reflections are **off** in the player's config (on: +3.5 ms).
3. **Bisect (14 builds, Oct 8 to now):** Scene pass flat at 13.6 to 13.9 ms from Oct 10 to now; sum of passes 23.6 to 23.9 ms from Oct 10 to Oct 19, **25.2 ms from the Thema 180 merge**;
   Oct 8 builds 21.0 to 21.5 ms. So since Oct 8 the scene got 17 % dearer on the GPU: ~3 ms between Oct 8 and Oct 10 (before all three suspects), 1.5 ms from 180.
4. **Why "in the game" and not in the editor:** the editor viewport renders 1718x874 (1.5 MP), the game 2880x1806 (5.2 MP), 3.5x the pixels. Everything above is per-pixel work (clouds, SSAO, the landscape material).
5. **The game's backend is OpenGL** in the player's export (`Config/ProjectSettings.json` `renderDefaults.backend = "OpenGL"` -> `GameBackend: OpenGL` in the exported `config.json`). Whether that is what the player runs
   when the FPS are bad is **unknown** (question sent to the queen, request 146, unanswered). OpenGL is CPU/driver bound (`glBufferSubData` flushes the Metal queue) and its numbers scatter 27 to 50 FPS for the same binary: no trend readable.
6. **Thema 235 (water close-up <10 FPS) was not reproduced** with this scene; water at 12 to 130 m costs <= 0.1 ms, 2 m above the surface the water frame is cheaper than the same view without water (section 6). No shared cause found, none excluded.

Measured vs inferred is marked per section ("measured" = a number from a run in `raw-landscape-regression/`; "inferred" = reading of code or of a pattern).

## 1. How it was measured

* **Scene:** the player's own project `~/HorizonEngineProjects/Catania` (copied, read-only use of the original): 2000 m landscape, 1025 grid, **Auto Landscape material** (`0x412`), 11 water bodies, Sky with volumetric clouds
  (`cloudCoverage 0.31`, quality 1), Weather, 2 meshes. No foliage, no layer meshes (so the 180 layer-mesh numbers of the audit do not apply here).
* **Settings:** the CustomConfig of the exported game's `config.json` laid over the private editor config (`landscape_regression.py --export-config`): GI **on**, GI reflections **off**, SSAO on, AA 1, Bloom on, RenderPath 1 (deferred), RenderScale 1.
* **Editor loop = game pixels:** `RenderScale 1.86` makes the editor viewport render 3195x1626 (5.2 MP, the game: 2880x1806 = 5.2 MP). Camera `0,2,8,0,0` = the game's default camera when the scene has none (`GameApplication::ensureDefaultCamera`).
  Check: the editor loop gives 41.8 FPS (Metal, whole frame), the exported game 40.2 FPS.
* **Runs:** `he_perf_capture.py` (real editor loop, vsync off, 200 frames warm-up, 600 recorded). `-det` = `--detailed` (each pass its own command buffer, exclusive GPU time per pass; FPS of these runs is not quoted),
  `-fps` = whole frame, counters off. Repetitions: 3 for the current build base and GI-off cells, 2 for b1018/b1019w/b1021a, otherwise 1 (marked `n`).
* **Binaries:** no build was made. Fourteen existing editor deploys of other worktrees (Oct 8 to Oct 21) were copied to `/tmp/he237/<name>/Editor`, made self-contained (`selfcontain_deploy_copy.sh`) and stripped of
  `EngineContent/Textures/Landscape`, so all use the same EngineContentCache arrays (V = 1, RGBA8, Oct 14). `binaries.txt` lists hashes, source worktree and build time.
  **The commit of a deploy is inferred** (worktree HEAD at build time); uncommitted work in that worktree is unknown. One was off by a day (`b1019w`: the branch tip commit is later than the build).
* **Conditions** (`*.conditions.txt`): battery power (not the adapter; the audits ran on the adapter), `lowpowermode 0`, display awake (`caffeinate -d -u` around every run), load 1.9 to 4.3, no compiler running (the driver waits).
  **A display that is asleep or locked made the first run hang for 300 s** (editor and game alike): the driver keeps the display awake now. Absolute values are comparable only inside this series.
* **Not measured:** D3D11/D3D12/Vulkan (the Mac cannot), the Thema 191 branch, a GPU-counter capture of the game itself (the game has the scripted capture `HE_PROFILE_CAPTURE` in its dev build; only FPS/CPU were read from it), AC power.

## 2. The game as the player exports it (measured, one run each)

`Export/Development/HEProject.app` (engine built Oct 20 11:08), copied to `/tmp`, run with `HE_PROFILE_CAPTURE=600 HE_PROFILE_WARMUP=200 HE_PROFILE_VSYNC=off`, 2880x1806, 75 draws, 388k triangles, 261 objects.

| Backend | FPS | frame ms (p50) | where the time is |
|---|---|---|---|
| OpenGL 4.1 (as exported) | **29.4** | 34.1 | `Render` 32.7 ms (CPU, driver); main thread inside `glBufferSubData` -> `flushContext` -> command-queue submit (`sample`); `OcclusionCuller::refine` 3.7 ms |
| Metal (`GameBackend` set to Metal in the copy) | **40.2** | 24.9 | `Render` 6.1 ms CPU; `Metal::NextDrawable` 18.4 ms = waiting for the GPU, the frame is GPU bound |

OpenGL note: "GL < 4.3 - GI unavailable (CSM/AO fallback)": the player's GI setting does nothing on OpenGL on a Mac.

## 3. Per pass, current build (measured; `landscape_regression_table.py`)

Detailed GPU time p50 in ms, 3 repetitions (sum 25.17 to 25.30), camera `0,2,8`:

| Cell | Scene | SSAO | Shadow | GI diff | Bloom | Tonemap | Present | Sum |
|---|---|---|---|---|---|---|---|---|
| current, player's settings | 13.68 | 8.61 | 0.75 | 0.96 | 0.73 | 0.29 | 0.26 | **25.24** |
| GI diffuse off | 13.62 | 9.09 | 0.73 | 0 | 0.73 | 0.29 | 0.26 | 24.76 (-0.5) |
| GI reflections on (191-like feature, Thema 173 code) | 13.77 | 12.07 | 0.77 | 0.99 | 0.72 | 0.29 | 0.26 | 28.86 (**+3.6**; FPS 36.3 vs 41.8) |
| clouds off (`cloudCoverage 0`) | **0.96** | 15.27 | 0.94 | 1.53 | 1.41 | 0.52 | 0.48 | 21.12 (clock drop, see below) |
| terrain with the built-in material | 14.04 | **2.14** | 0.70 | 0.97 | 0.71 | 0.25 | 0.26 | 19.08 |
| SSAO off (the setting) | 13.55 | 8.51 | 0.70 | 0.94 | 0.72 | 0.28 | 0.26 | 24.96 (no effect: see below) |
| water bodies removed | 13.61 | 8.53 | 0.72 | 0.96 | 0.72 | 0.28 | 0.26 | 25.08 (+/- 0) |

* **Clouds:** the Scene pass drops from 13.7 to 1.0 ms without clouds: the volumetric clouds cost ~12.7 ms of the pass at 5.2 MP. The sum only falls by 4 ms because a lighter frame lets the GPU clock down (the audit's trap:
  Bloom/Tonemap/Present double). Clouds are not new (they stand at 12.2 ms already in the Oct 8 build), so they are not a regression, but they are the biggest single cost.
* **SSAO depends on the terrain material** (8.6 -> 2.1 ms with the built-in material) and **the `SSAOEnabled=false` setting changes nothing** in this run: with the player's config the pass still ran at 8.5 ms.
  Possibly the editor-loop capture takes `SSAOEnabled` from the editor config key differently, or the pass booked as "SSAO" is the pre-pass the deferred path always needs; **not investigated** (inferred, open question).
* FPS without counters: current base 41.8 (41.5 to 42.2, n=2), GI off 41.0 (40.1 to 41.8, n=2), GI reflections on 36.3 (n=1).

## 4. Bisect over builds (measured; sum of the detailed passes, ms)

| Build (source, inferred commit, built) | Sum | Scene | SSAO | `M_AutoLandscape.hasset` |
|---|---|---|---|---|
| b1008b auto-landscape-texturen 0af31ea0, Oct 8 19:03 | 21.00 | 12.15 | 5.92 | b218 |
| b1008 gi-reflexionen 587a910d, Oct 8 13:14 | 21.54 | 12.18 | 6.38 | f09b |
| b1010a entities-pro-zelle f6e7452f, Oct 10 14:29 | 24.73 | 14.46 | 7.56 | ac7f |
| b1010 instanced-foliage 7692ad97, Oct 10 19:56 | 23.89 | 13.88 | 7.08 | ac7f |
| b1012 / b1013 / b1015 / b1017 (Oct 12, 13, 15, 17) | 23.83 / 23.59 / 23.75 / 23.55 | 13.80 / 13.60 / 13.75 / 13.57 | 7.10 / 7.05 / 7.03 / 7.04 | ac7f |
| b1018 landscape-sechseck 40c68ec7, Oct 18 16:00 (n=2) | 23.73 | 13.67 | 7.07 | ac7f |
| b1019w water 206 up to step 6, Oct 19 23:32 (n=2) | 23.69 | 13.68 | 7.07 | ac7f |
| **b1021a Thema 180 final 6479c3d1, Oct 21 07:34 (n=2)** | **25.21** | 13.66 | **8.56** | dd4d |
| **current 12be6adc (n=3)** | **25.24** | 13.68 | **8.61** | dd4d |
| b1019w + the **new** material asset (n=2) | 23.70 | 13.69 | 7.25 | dd4d |
| current + the **old** material asset (n=2) | 23.73 | 13.92 | 7.03 | ac7f |

* **Thema 180: +1.5 ms in the SSAO pass, +6 % of the pass sum.** Measured twice (builds) and pinned by the swap: the old `M_AutoLandscape.hasset` on the current engine gives 7.03 ms again.
  The new asset on the old engine does **not** raise it (7.25), so the cost needs the new engine code and the new asset together. Changed by (inferred from `git log`): `8f3a6e47` (texture variants, `heBombGridV`)
  and/or `0d7cdf09` (puddles no longer end on a slope contour); the two were not separated.
* **Oct 8 -> Oct 10: +2.9 to +3.7 ms** (Scene +2 ms, SSAO +1.2 to +1.6 ms) between the Oct 8 evening builds and the Oct 10 afternoon build. Before any of the three suspects. Material asset changed twice in that window
  (f09b -> b218 -> ac7f: weather puddles/snow, all layers paintable). **Not bisected further**, builds of other branches in between are not a clean series.
* b1009 (Oct 9 01:20, branch `editorui-footer...`) is a transient: 38 to 52 ms sum (7.8 to 11 FPS, twice), Scene 23 to 27 ms. Gone by Oct 10. Mentioned because it shows the cloud/scene cost can swing by 2x between commits.
  b1008c (Oct 8 17:40) books Scene 0.2/6.1 ms and SSAO 12 to 18 ms for the same 21.0 ms sum: pass attribution of old builds is unreliable, the sum is not.
* **OpenGL** (editor loop, 5 builds, 3 runs each, `gl` cells): 27 to 50 FPS for the same binary (b1019w 27.2, 32.7, 49.5), bimodal (CPU 19 to 23 ms vs 30 to 37 ms). **No trend can be read; a GL bisect needs far more repetitions.**

## 5. The three suspects

| Suspect | Finding | Status |
|---|---|---|
| **Thema 191** GI reflections | PR #183 (`claude/global-illumination-reflections`) is **not in `release/0.7.0` or in this branch** (`git merge-base --is-ancestor` no for 760dd4bc, a9da5068, 8eacc19c). Its diff is D3D11/D3D12/Vulkan; **22 lines in `MetalRenderer.mm`**. The Metal GI reflections come from Thema 173 (Oct 8, in the tree since). In the player's config they are **off**. On: +3.6 ms (-5.5 FPS). | measured; "not in tree" verified with git |
| **Thema 180** Auto Landscape 2.0 | +1.5 ms SSAO pass in this scene (section 4). Layer meshes/instancing: not in the Catania scene; the audit's CPU numbers (`ExtractFoliage`) stand. The BCn 440 ms/frame bug is fixed in this tree (`fbc999fd`); no BCn arrays are shipped (the cache arrays are V = 1 RGBA8, Oct 14), so it does not apply to the player's setup. | measured (scene without meshes) |
| **Thema 206** water | Bodies removed: 0.0 to 0.1 ms difference at all three distances; the 206 builds (b1018 -> b1019w -> current) are flat in Scene. | measured; caveat: that the new `Water.hasset` shader was really compiled for these surfaces was not verified from the log |

**Which one "mainly" causes the drop: none.** The measurable regression on Metal is the 180 material (+6 %); the large absolute costs (clouds 12.7 ms, SSAO 7 to 8.6 ms at 5.2 MP, GL driver stalls) predate all three.

## 6. Relation to Thema 235 (water close-up)

Measured with the same scene, current build: camera 12 to 90 m from a pool: Scene 11.37 ms vs 11.34 ms without water; 55 to 130 m: 12.69 vs 12.81. Camera 2 m above a pool: the water frame sums to **20.96 ms against 25.20 ms** without water
(Scene 12.15 vs 8.85, but SSAO 4.9 vs 13.2: the water covers pixels the landscape would have paid AO for). So this scene does not show a water cost at all, let alone <10 FPS. Pre-206 build b1018 gives the same picture (20.35 ms).
Inferred: Thema 235 needs a condition this scene does not have (another water material setting, larger/ nearer water, the Thema-230 UUID collision turning `Water.hasset` into something else on the player's machine).
**Shared cause with this topic: not found.** If 235 is real on the player's machine, it is a second problem, not the general one.

## 7. What is open / what I would do next

See the Handoff.

## Handoff

**Done (verified by the runs above):** reproduction of the exported game (OpenGL 29.4, Metal 40.2 FPS), editor-loop emulation at the same pixel count (41.8 FPS), per-pass table, bisect over 14 builds, material-asset swap A/B, GI / GI-reflections / clouds / water / material toggles.
Commit: this doc, `scripts/perf/landscape_regression*.py`, `scripts/perf/landscape_regression_tools/`, `perf-audit/raw-landscape-regression/`.

**Not verified:** the player's real scene/backend/commit (question to the queen, request 146, unanswered at the time of stopping); AC power; D3D/Vulkan; the game itself per pass (only FPS/CPU); the SSAO setting having no effect (section 3);
whether the new water shader was compiled in the water cells; pre-Oct-8 builds (Catania's material `0x412` does not exist before Oct 7).

**Decisions taken:** deploy copies instead of building (no source change, 14 points in 3 h); `RenderScale 1.86` to match the game's pixel count; Metal first; OpenGL only as a repetition-limited side series.
**Only suggested:** none implemented.

**Next steps, in order:**
1. Ask the player (or read their last run) which backend the game runs: if OpenGL, the work is the GL path (`glBufferSubData` flushes, `Render` 32 ms), not Metal. A GL bisect needs >= 5 repetitions per build and the adapter plugged in.
2. (DONE in `landscape-performance-regression-asset-attribution-2026-10-22.md`: 8f3a6e47's asset +1.3 ms, 0d7cdf09's +0.35 ms; Oct 8 -> 10 = +1.25 asset / +1.8 engine.) Separate `8f3a6e47` from `0d7cdf09`: build only `matgen` at both commits (small tool, `src/HE_Tools/src/MatGen`), put each `M_AutoLandscape.hasset` into `/tmp/he237/cur_oldmat/Editor/EngineContent/Materials/` and run `cell.sh ... det` (3 runs). Same trick for the Oct 8 -> Oct 10 step (b1010a with the b218 asset).
3. Explain why the SSAO pass (8.6 ms) depends on the terrain material and ignores `SSAOEnabled=false` in the editor-loop capture; this is the single biggest landscape-specific pass.
4. The clouds (12.7 ms at 5.2 MP, `cloudQuality 1`) are the biggest cost; a half-res or temporally reprojected cloud pass is the lever (plan in `sky-weather-perf-plan`), independent of this regression.
5. For Thema 235: reproduce on the player's scene with the player's `Water.hasset` before assuming a link.

**Pitfalls:** a sleeping/locked display hangs the first present (300 s TIMEOUT): keep it awake (`landscape_regression.py` does). The first `cp` of an exported `.app` is fine, `rm` of a dump glob in a `cd`-ed shell is refused by the safety check: do not clean copies, overwrite.
`HOME`-based EngineContentCache supplies the landscape arrays when the deploy has none; the Oct 8 deploys carry 437 KB placeholder arrays in `EngineContent` (strip them, as `prep.sh` does). Old builds mis-book pass names, compare sums.
Commands: `scripts/perf/landscape_regression_tools/prep.sh <name> <worktree>`, then `cell.sh <label> /tmp/he237/<name>/Editor <scene> Metal det|fps [--set K=V]`, `camcell.sh` for other cameras, table: `python3 -I scripts/perf/landscape_regression_table.py docs/perf-audit/raw-landscape-regression`.
Scene variants (`catania_base/nowater/nomat/noclouds`) are made by a few lines of Python from the project's `StartupScene.hescene` (set `waterBodies=[]`, drop terrain `material`, `cloudCoverage=0`).
