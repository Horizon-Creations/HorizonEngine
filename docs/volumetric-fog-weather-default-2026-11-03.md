# Volumetric fog, step 7: weather hooks, new-scene default, manual, perf (Thema 225)

Plan: `docs/volumetric-fog-plan.md` §11 and §12 step 7. Earlier steps: `volumetric-fog-metal-2026-11-02.md`, `volumetric-fog-vulkan-2026-11-02.md`.

## What changed

| Piece | Where |
|---|---|
| `WeatherPreset::fogBank` / `fogNoise` (appended to the aggregate, so old rows still compile) | `src/HE_Scene/include/HorizonScene/Components/WeatherComponent.h:50-57`, runtime `fromFogBank/fromFogNoise/lastFogBank/lastFogNoise` next to the existing ones |
| Preset table + `drive()` for `env->fogBankDensity` / `env->fogNoiseAmount` | `src/HE_Scene/src/WeatherSystem.cpp` (table at the top, `tBank/tNoise`, `from*` snapshot on reclaim, two `drive()` calls) |
| New Sky starts at `fogMode = 1` | `src/HE_Scene/src/HorizonWorld.cpp` `addSky()` |
| New project's `StartupScene` Sky carries `"fogMode": 1` | `src/HE_Tools/src/FileOps/ProjectManager.cpp` `startupSceneJson` |
| Headless `HE_DUMP_SKYTEST` keeps `fogMode 0` for a sky it adds itself | `src/HE_Editor/EditorApplication.cpp` ~5509 |
| Help text (no new controls, four entries reworded) | `src/HE_Editor/EditorHelp.cpp` (`Mode##fogvol`, `Bank Density`, `Noise Amount`, `Fog Density##wx`) |
| In-engine manual (Rendering > Sky bullet "Volumetric fog", Weather bullet "Fog") | `EditorDeps/Docs/he-docs.json`, patched in place, same dump format as the generator |
| Tests | `tests/test_weather.cpp` (3 new cases), `tests/test_fog_volume.cpp` (new-Sky subcase), `tests/test_third_person_template.cpp` (template Sky loads at 1) |

## Decisions taken
* **Preset values** (suggested by the plan, kept): Foggy bank 0.15 / noise 0.7, Rain and Storm bank 0.02, everything else bank 0. Noise 0.5 = the Environment default, so
  only Foggy moves it. Bank scales with `intensity`; noise is `0.5 + (p - 0.5) * intensity` (intensity 0 lands on the default, like windSpeed lands on 1).
  Snow gets no bank (the volume already adds `0.008 * snowAmount` haze, `FogVolume.h`).
* **Default for new scenes = 1, and only through `addSky()` and the project template.** `EnvironmentComponent{}.fogMode` stays 0 on purpose: the loader
  keeps the struct value for a missing key (`SceneSerializer.cpp` ~1695), so changing the struct default would flip every old scene. A test pins both (`test_fog_volume.cpp`,
  existing "saved before volumetric fog" case in `test_scene_serializer.cpp:1424` still green).
* A new Sky at mode 1 draws nothing until density or bank is above 0 (`enabled` needs one of them), so adding a Sky changes no picture by itself. On D3D11/D3D12
  (`supportsVolumetricFog` false) `MakeFogVolumeData` gives `enabled = false` and the analytic fog runs, so fog is not silently lost; the Inspector greys the combo and says so
  (`InspectorPanel.cpp:1283-1289`, unchanged).
* **Underwater rule (plan §11)** was already satisfied: `MakeFogVolumeData` (`FogVolume.h:~196`) sets `enabled = false` when `cameraUnderwater`; test "camera underwater overrides" exists. Not re-added.
* Weather writes bank/noise even in fogMode 0 (the fields are read only by `MakeFogVolumeData`, verified by grep), so switching a scene to Volumetric later shows the preset's bank.

## Verified
* `he_tests` built; run: `test_fog_volume` 15/15, `test_weather` 37/37, `test_engine_materials` 15/15, `test_scene_serializer` 48/48, `test_third_person_template` 15/15, `test_hc_node_docs` 6/6, `test_engine_api` 130/130.
  Full `he_tests` run: 6145 test cases passed, 0 failed, 20 skipped (2,151,647 assertions).
* `python3 scripts/editor_help_audit.py --check`: 1264/1264, 0 missing.
* `HorizonEditor` builds (Inspector, help, dump change compile). Note: the tree's `HorizonSDKRelease` step (release lib set, `cmake/HorizonSDK.cmake:317`) failed with "dev MISMATCH"
  after the header change; I reconfigured this build dir with `-DHE_SDK_RELEASE_SET=OFF` to get the editor linked. The release SDK set is therefore not rebuilt in this tree.

## Perf (Apple M5 MacBook Air, Metal only)
Setup: `scripts/he_perf_capture.py --detailed --warmup 200 --frames 300`, Catania project copy in /tmp, 2840x1528, grid Medium, `HE_FOG_MODE=0|1 HE_FOG_DENSITY=0.02 HE_SKY_TIME=10`,
4 runs per mode, order alternating, p50 per pass, median over runs. **Conditions: AC power, Low Power Mode off, GPU idle (3 %), but the session was SCREEN-LOCKED** (window not composited,
the earlier note says clocks and absolute numbers shift), so read the deltas, not the absolutes.

| | fog off | fog on | delta | step 3 (display awake) |
|---|---|---|---|---|
| `Shadow` bucket (shadow maps + Inject + Integrate) | 1.46 | 2.10 | **+0.64** | +0.56 |
| `Scene` bucket (scene + Apply + encoder split) | 1.05 | 1.39 | **+0.34** | +0.16 |
| `SSAO` bucket | 13.29 | 12.93 | -0.37 (partition noise: it is not fog work) | - |
| whole-frame GPU p50 | 17.96 | 18.37 | **+0.40** (runs 0.0..+0.7) | +0.53 |

Per-run Shadow off 1.45/1.45/1.48/1.47, on 2.08/2.10/2.09/2.10 (tight). The two fog buckets sum to ~1.0 ms but the whole frame moves 0.4: the detailed per-encoder split over-counts
(stage counters overlap, noted in step 3). Target was <= 0.5 ms for the whole feature at Medium: the whole-frame number meets it, the bucket sum does not; which one is "true" I cannot separate here.
Raw summaries: not committed (they are in /tmp on the Mac that ran this).

**Not measured: OpenGL** (the run with `--rhi OpenGL` hung on this locked-screen Mac and was killed; no number), **Vulkan on a GPU** (only lavapipe correctness in step 5), **D3D11, D3D12**
(step 6 failed, NN-WS03 needed), **Software** (feature off by design). Do not read any of these off the Metal row.

## Open / not done
* D3D11 + D3D12 fog and their hardware acceptance (step 6 failed; Windows steps stay open until NN-WS03). Their `supportsVolumetricFog` is false, new scenes there use the analytic fog.
* Real Vulkan GPU numbers; GL numbers (retry with an unlocked screen, or on Linux).
* The website source `Website/HorizonEngineDocs/rendering.html` was NOT edited (separate repo, and `build_docs_bundle.py --check` already reported the bundle stale against it before this step). The same two bullets
  must go there or the next `build_docs_bundle.py` run drops them: Sky list, after "Atmospheric fog": "Volumetric fog - set Fog Mode to Volumetric for a real 3D fog volume ..." and Weather list, before "Environment sliders stay live": "Fog - Foggy sets ... a thick ground bank ...". Exact text is in `he-docs.json` (`rendering` > `sky` / `weather`).
* Phase 2 untouched: local fog volumes, particles fog, cluster lights, `volumetricScatter`.

## Handoff
* Branch `claude/echtes-volumetrisches-nebel-statt-hintergrund-blend`, commit `0a40ee09e` (+ this doc). Pushed at the end of the step.
* Next for the queen: merge into `release/0.8.0` only after deciding whether Metal's ~1.0 ms bucket sum matters (Low quality tier exists, grid 96x54x32); schedule the NN-WS03 session for D3D11/D3D12; sync the website HTML.
