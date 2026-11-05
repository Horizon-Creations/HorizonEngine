# HorizonCode: Get Engine Settings, quality enums, shader quality, automatic quality (topic 336)

Builds on `docs/engine-quality-settings-2026-11-05.md` (topic 324: the quality model and the
player's settings store). Branch `claude/hc-engine-settings-nodes`, no PR opened.

## 1. What exists now

* **Settings object.** `Get Engine Settings` returns a fixed Ref (`settings::kEngineSettingsRef`
  = `0xE5000001`). `Get/Set Quality Of` (ref, `QualitySetting` enum, `QualityLevel` enum) and
  `Get/Set Quality Level` (the overall level) work on it. `Switch on Enum` handles the result.
* **Built-in enums.** `engine:QualityLevel` (Low, Medium, High, Ultra, Custom) and
  `engine:QualitySetting` (the ten areas) are seeded into `HE::TypeRegistry` (`isBuiltin`;
  `registerEnum/removeType/clear` leave them alone). Pickers, `horizon.enums.*` in Lua/Python and
  the C++ header generator read the registry, so they show up everywhere with no extra wiring.
* **Compiled graphs.** Rows with a typed enum pin stay on the `hc::callApi` Value path
  (`HcCodegen.cpp` `directRow()` returns nullptr for them); the direct path would hand an
  `int` to a function taking the generated enum. Parity test `fxQualityLevels` covers it.
* **Shader quality.** Material node `Quality Level` and a quality lane in `heLight.counts.y`
  (level + 1, 0 = unfilled = High) written by `FillMaterialWind` for all five backends.
  `heFbm*` noise helpers use fewer octaves at Low/Medium; High keeps the fixed 4-loop. The baked
  GLSL in the shipped `M_AutoLandscape*.hasset` was regenerated (`landscape_tex_gen`).
* **Automatic quality.** `QualityGovernor` (HE_Core, no clock of its own): smoothed frame time
  (tau 0.5 s), one frame weighs at most 4 targets, frames >= 0.5 s ignored; miss (avg > target
  x 1.12) for 2 s steps down, meet (<= x 1.05) for 15 s steps up, 3 s cooldown after a step, a level
  that was left is blocked for 60 s. Game only (`Host::autoQualityAvailable`), fed the raw
  `deltaTime` in `GameApplication::OnRender`, never saved. Layering: project values -> governor
  level -> player preset -> player single values. Picking a preset turns it off.
  Rows `Get/Set Auto Quality`, `Get Auto Quality Level`. Project block `autoQuality`
  (enabled=false, targetFps=60, minLevel, maxLevel) with a section on the Quality page.

## 2. Witness on Catania (copy in the scratchpad, Metal, `HE_DUMP_QUALITYPRESET`)

`docs/engine-quality-settings-2026-11-05/catania_{high,low}_30f.jpg`: same camera, 30 frames.
Low has cruder, blotchier landscape layer noise and coarser shadows. This is the whole Low
preset, not the material lane alone, so it shows the combination, not a per-feature A/B.
With only 8 frames the terrain was sometimes still missing (async terrain generation): use 30.

## Handoff

**Verified**
* he_tests build; ctest full run green except `test_jobsystem`, which failed once under -j4
  and passed alone (flaky, not touched). New: `test_quality_enums` (8), `test_quality_governor`
  (17 cases: controller rules, store layering, JSON, rows, project block), material tests, parity
  fixture. `editor_help_audit.py --check`: 1277/1277. HorizonEditor and HorizonGame build.
* The governor test caught a real flaw: one 0.3 s hitch kept the average above the limit for
  ~2 s and lowered the level. Fixed by capping one frame at 4 targets (`QualityGovernor.cpp`).
* `tests/fixtures/game_build_config/HEGameBuildConfig.h` regenerated (new `autoQuality` JSON
  block); `test_project_settings_ui` bottom-of-page index moved by one.

**Seen in the editor (headless UI shots, `HE_UI_DUMP_DIR`)**
* Quality page with Automatic quality on (checkbox, target frame rate, lowest/highest level),
  `test_project_settings_ui` "automatic quality switch" case; nodes on the real HC canvas
  (`test_ui_shot.cpp` "engine-settings nodes": Get Engine Settings -> Set Quality Of, Get Quality
  Level -> Switch on Enum with one exec pin per level + Default, enum pins green); the add menu
  searched for "quality" lists all Settings rows and "Switch on QualityLevel" under Enums.

**Governor in a running game (Metal, exported with `ProjectExporter`)**
Target 1000 fps, unthrottled hidden run: `level 2 -> 1` at frame 181 (~2 s), `1 -> 0` 5 s later
(3 s cooldown + 2 s), then it holds the floor. Log line "Automatic quality: level A -> B (...)" is
new (`PlayerSettings.cpp` `tickAutoQuality`). Found on the way: a throttled background window
(15 fps) would have counted as a miss, so `GameApplication` skips the tick while
`IsInBackground()` and the throttle is on. The export did not carry `Config/ProjectSettings.json`
unless `ExportSettings::projectSettingsFile` is set; copied by hand for the run.

**Manual:** Website `rendering.html` has "Material quality", "Quality by area, with enums" (image
`hc_engine_settings_nodes.png`) and "Automatic quality"; `docs-index.json` rebuilt. Committed in
the Website repo (b35661a), NOT pushed, NOT deployed.

**Not verified**
* D3D11/D3D12/Vulkan/OpenGL: see Hive steps 7-10 of topic 336 (hardware or CI).
* A pixel A/B of the material lane alone; the Catania shots show the whole Low preset.
* The scripting-reference page for the new rows (generated page, not touched).
* A player's menu flow (pick preset in the game turns auto off) beyond the unit tests.

**Decisions taken:** enums are built-in registry types, not project assets; `Custom` is
returned for mixed areas; governor stays out of the editor viewport.

**Next steps, in order**
1. Push and deploy the Website docs commit once the human approves (docs files only).
2. Backends: Hive steps 7-10 (D3D, Vulkan, OpenGL, real display).
3. Pixel A/B of material quality alone (a project copy with only `materialQuality` changed).
4. Decide the target-above-refresh-rate question (cap the target or warn in the dialog).
