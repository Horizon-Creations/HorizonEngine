# Water material UUID move (0x600): acceptance on the other backends (Thema 230, Schritt 3)

Fix under test: `claude/water-uuid-collision-0x600` (32371eb7 + 0c4b823e), see
`water-material-white-uuid-collision-2026-10-21.md`. Base is 4d8772d2; `origin/release/0.7.0` is 18 commits ahead (not merged here, the PR diff is the 6 files of the fix).

## Result per backend

| Backend | Status | Evidence |
|---|---|---|
| Metal | verified (Schritt 2, re-run here as control) | `HE_DUMP_WATERBRUSH=dig` patch mean (62,107,130) |
| OpenGL (macOS GL 4.1) | **verified** | same scene, patch mean (61,107,133); waves, caustic bands and foam visible. Control: `Water.hasset` removed from the deploy gives (229,230,232) white and the new warning `Water surface: the engine water material ... is not available` |
| D3D11, D3D12 | **not verified** | NN-WS03 (the only Windows runner) offline for 12 h (`hive_devices`); Mac cannot run D3D |
| Vulkan | **not verified** | Mac build has `HE_VULKAN_ENABLED=FALSE`, no Vulkan loader/headers installed, `VulkanRenderer` has no portability enumeration, so MoltenVK is not usable without a new backend path |

The fix is in shared code (ContentManager id, DefaultAssets.h, Water.hasset), so the D3D/Vulkan result is expected to match, but it was not run.

Run: Release deploy `out/deploy/Editor` built from this branch (binary and dylibs byte-equal to the build tree, `EditorDeps` Water.hasset == deploy copy), the human's real config (project Catania, EngineContentCache present, which is the condition that made the water white). Env: `HE_DUMP_SKYTEST=1 HE_DUMP_TOD=0.5 HE_DUMP_WATERBRUSH=paint|dig|undo HE_DUMP_CAMY=340 HE_DUMP_CAMZ=44 HE_DUMP_PITCH=-58 HE_DUMP_RHI=Metal|OpenGL`.
Patch mean RGB, box x400-900 y120-300:

| mode | Metal | OpenGL |
|---|---|---|
| paint (shallow) | 211,218,220 | 217,223,225 |
| dig (deep) | 62,107,130 | 61,107,133 |
| undo | 225,227,227 | 227,229,230 |

`paint` is shallow water on the white terrain and is pale on both backends (as in Schritt 2: (132,154,178)); it is a poor discriminator against the fallback. `dig` is the decisive frame. Not run: GL on the pre-fix commit (no negative control with the old UUID; the pre-fix Metal result is in the finding doc).

## Tests (Mac, Release, `cmake-build-release/tests/he_tests`)

* `test_water_surface` 56/56 (2949 assertions), `test_engine_materials` 11/11 (832), `test_inspector_ui` 13/13 (294).
* Full `he_tests`: 5636 cases, 5634 passed, 2 failed (`test_profiler.cpp:107` and `:215`, EngineProfiler), 16 skipped. Both profiler cases pass when run alone through ctest, so they depend on test order in the full run; unrelated to water, not investigated.
* `test_hpak` "scanContentDirectory resolves UUIDs from loose content" fails here with `6 == 2` because this Mac's real EngineContentCache holds 4 `.hasset`; with an empty `HOME` it passes. Environmental, not the fix.

## CI

Run 39578163805 was still `in_progress` (all four jobs in Build, 22 min in) when this was written.

## Handoff

* **Done:** GL witness + control, Metal control, three filtered suites, full suite, backend feasibility check.
* **Not verified:** D3D11, D3D12, Vulkan (see table). Windows steps must stay open until NN-WS03 is online: run `HE_DUMP_RHI=D3D11|D3D12|Vulkan` with the same env on a Windows deploy that carries the fixed `Water.hasset`, with the EngineContentCache present, and expect the `dig` patch near (62,107,130) instead of white.
* **Decisions:** no code changes in this step; Water.hasset untouched.
* **Traps:** the editor loads Water.hasset from `out/deploy/Editor/EngineContent/Materials`, not `EditorDeps`. The bug only shows with a loaded EngineContentCache; an empty `HE_CONFIG_DIR` hides it. Run he_tests with `HOME=<empty dir>` or the cache skews `test_hpak`.
* **Next steps:** wait for CI 39578163805; merge only after the Windows job is green; re-export old paks (they keep Water under `{0x400,1}`); D3D/Vulkan witness when NN-WS03 is back.
