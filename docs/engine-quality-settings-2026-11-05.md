# Engine quality settings (Hive topic 324)

Central quality presets (Low, Medium, High, Ultra, Custom) over the quality options that
are today spread over the renderer settings, the project settings and a few environment
variables. Branch `claude/engine-quality-settings`, merges into `release/0.8.0`.

Steps: 1 inventory + data model (this doc), 2 renderer/runtime wiring, 3 `HE::api` for
HorizonCode/Lua/Python/C++, 4 editor UI, 5 verification + docs.

## 1. Inventory: where a quality option lives today

| Area | Stored in | Reaches the renderer by |
|---|---|---|
| Anti-aliasing, render scale, specular AA, SSAO, bloom, DoF, motion blur, SSR, GI, GI reflections, render path, occlusion culling, GPU particles | `HE::ProjectGraphics` (`Config/ProjectSettings.json`, key `graphics`); one X-macro `HE_PROJECT_GRAPHICS_FIELDS` in `ProjectSettings.h`, twin of the `EditorConfig` members | editor: `EditorApplication::syncGraphicsSource` (copies into `m_editorConfig` while `graphics.enabled`), then the per-frame `IRenderer::Set*Settings` pushes; game: `GameApplication.cpp` ~3470-3640 reads the exported `config.json` keys |
| Shadows (distance, cascades, resolution, split lambda, biases) | `HE::ProjectShadowSettings`, key `shadows`, always on (no enabled switch) | `EditorApplication::projectShadowSettings()`, game `GameApplication.cpp` ~3546, `IRenderer::SetShadowSettings` |
| Cloud / nebula quality | per scene, `EnvironmentComponent::cloudQuality`/`nebulaQuality` (already in `HE::api env`) | `EnvironmentPush.cpp` |
| Foliage draw distance, LOD scale | per component (`FoliageComponent::drawDistance`, `TerrainComponent::lodDistanceScale`, `MeshComponent::lodBias`) | scene systems |
| Debug knobs | env vars `HE_RENDER_PATH`, `HE_GI_*`, `HE_GIREFL_*`, `HE_DUMP_*` | read ad hoc; not settings, left alone |
| Texture quality, material quality | **nowhere** (no consumer found in `src/`) | new in this step, see below |

`config.json` is the editor's (`EditorConfig`) key store; `ExportDialogPanel.cpp` writes the
graphics half into the exported build (`put("Key", cfg.Member)`), the game reads it back.
When `ProjectGraphics::enabled` is false the exporting editor's own values ship and the
viewport shows the user's preferences, which is the pre-presets behaviour.

## 2. The data model (built in this step)

A preset is **not** a second set of values. `HE::QualityPreset { Low, Medium, High, Ultra,
Custom }` plus three functions in `ProjectSettings.h/.cpp`:

* `applyQualityPreset(preset, graphics, shadows)` writes a table row (below). Low..Ultra
  also set `graphics.enabled = true`; `Custom` changes nothing.
* `detectQualityPreset(graphics, shadows)` returns the preset whose row the values hold now,
  else `Custom`. `enabled` and the stored tag are not looked at.
* `qualityPresetName` / `qualityPresetFromName` (case-insensitive) for file, editor, API.

`ProjectGraphics::preset` stores the tag (file key `graphics.qualityPreset`, string).
`ProjectSettings::clamp()` (also run by `fromJson`) reconciles it: a tag whose values no
longer match its row becomes `Custom`. Missing key = `Custom`, so an old file loads with
every value untouched.

Two new fields in `HE_PROJECT_GRAPHICS_FIELDS` with `EditorConfig` twins and config.json
keys: `textureQuality`, `materialQuality`, each 0 Low / 1 Medium / 2 High, default 2 = full
detail = today. **No consumer yet**; step 2 decides per backend what 0/1 do (texture:
mips skipped at upload; material: optional features such as parallax or detail layers off)
and a backend that does nothing ignores the value and says so in the table here.

### Preset table (`kPresetRows`, `ProjectSettings.cpp`)

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| anti-aliasing (AAMethod) | 1 FXAA | 2 SMAA | 2 SMAA | 3 TAA |
| render scale | 0.75 | 1.0 | 1.0 | 1.0 |
| SSAO | off | on | on | on |
| SSR / quality | off / 0 | off / 1 | on / 1 | on / 2 |
| GI / shadow quality | off / 0 | off / 1 | on / 1 | on / 2 |
| GI reflections / quality / bounces | off / 0 / 1 | off / 1 / 1 | off / 1 / 1 | on / 2 / 2 |
| texture / material quality | 0 / 0 | 1 / 1 | 2 / 2 | 2 / 2 |
| shadow distance / cascades / resolution | 100 m / 2 / 1024 | 200 m / 3 / 2048 | 250 m / 3 / 2048 | 500 m / 3 / 4096 |

Left out on purpose (equal cost, a look decision): bloom, DoF, motion blur, SSAO method
and radius, shadow biases and split lambda, render path, occlusion culling, GPU particles.
The numbers are tuned by reasoning, not measured.

## 3. What acts on what (step 2)

How a preset change reaches the screen:

* **Editor:** `syncGraphicsSource` copies `settings.graphics` into `EditorConfig` while
  `graphics.enabled`; the per-frame pushes read `EditorConfig`. New: `SetQualitySettings`
  pushed next to `SetAntiAliasingSettings` (`EditorApplication.cpp` ~3722 live frame, ~9130 dump
  path, where `HE_DUMP_TEXQUALITY=<0|1|2>` overrides texture quality for one capture).
* **Game:** the per-frame push in `GameApplication.cpp` (~3480-3640) now reads
  `m_projectSettings.graphics` (`gfx`), not 34 config keys. The struct is filled once at start
  by `HE::readGraphicsFromConfig` (config.json keys = the macro's `EditorConfig` column, missing
  key = today's default), so startup behaviour is unchanged, and `applyQualityPreset(p,
  m_projectSettings.graphics, m_projectSettings.shadows)` is what the next frame draws.
  Shadows were already read from the struct. Not moved: render path (`configuredRenderPath`
  reads its key, not in presets) and `GpuParticles` (~3420).

| Level | Metal | OpenGL | D3D11 / D3D12 / Vulkan |
|---|---|---|---|
| AA, render scale, SSAO, SSR, GI, GI reflections | as before (backend gates, fallbacks at push) | as before | as before, capability-gated |
| shadow resolution/cascades/distance | acts | acts | ignored (one whole-scene map) |
| `textureQuality` | **acts**: material sampler `lodMinClamp` 2/1/0 (Low/Med/High), live, no re-upload | ignored (per-texture `glTexParameter`, no sampler objects) | ignored |
| `materialQuality` | stored (`GetQualitySettings`), no feature gated on it | same | same |

`textureQuality` only changes textures that carry a mip chain; a single-level texture (what the
packer cooks, and most loose imports) has no coarser level and looks the same.
UI, post and sky samplers are untouched on purpose.

## 4. Script API (step 3)

The player's graphics choice is a **player setting**, not a project edit: it lives in
`HE::api::settings` next to VSync, fullscreen and volume (`PlayerSettings.cpp`), persisted by
`settings.save`, handed back to the project's values by `settings.resetToDefaults`. Nothing ever
writes `currentProject().settings`, which is also why play-in-editor needs no snapshot.

Six rows in group `settings` (Free C++ form, so `HE::api::settings::...` directly; Lua and Python
get `horizon.settings.*` because `settings` was already a script group):

| Row | Meaning |
|---|---|
| `settings.setQualityPreset(preset)` -> ok | Low/Medium/High/Ultra, case-insensitive; "Custom" is accepted and changes nothing; unknown = false. Drops earlier single values. |
| `settings.qualityPreset()` -> String | what the effective values read as, so "Custom" once a value moved off a preset |
| `settings.qualityPresetNames()` -> String[] | Low, Medium, High, Ultra, Custom |
| `settings.setQualityValue(field, value)` -> ok | one regulator by name; bools 0/1, ints rounded, clamped to range; unknown name = false |
| `settings.qualityValue(field)` -> Float | current value, 0 for an unknown name |
| `settings.qualityFields()` -> String[] | the 18 names (`HE::qualityFieldName`): antiAliasing, renderScale, ssaoEnabled, ssrEnabled, ssrQuality, giEnabled, giShadowQuality, giReflectionsEnabled, giReflQuality, giReflBounces, textureQuality, materialQuality, bloomEnabled, dofEnabled, motionBlurEnabled, shadowDistance, shadowCascades, shadowResolution |

How it reaches the screen: effective values = `Host::baseGraphics/baseShadows` (the project's,
captured at install) -> preset (`applyQualityPreset`) -> single values; `enabled` is on once the
player chose anything; `preset` = `detectQualityPreset`. `Host::applyQuality(graphics, shadows)`
puts them where the renderer push reads them:

* **Game:** writes `m_projectSettings.graphics/shadows` (step 2's push reads them), base =
  the struct as `OnInit` filled it. Installed once in `installPlayerSettings`.
* **Editor (play):** writes `m_playGraphics/m_playShadows` (`EditorApplication.h`), active only
  while playing; `syncGraphicsSource` and `projectShadowSettings` read them, so the viewport shows
  the player's choice and goes back to the project's when play stops.

Persistence: `Values::qualityPreset` / `Values::quality` in the prefs JSON (`qualityPreset`,
`quality`); unknown names and fields are dropped on load, "Custom" is never stored.

`HE::qualityFieldCount/Name`, `getQualityField`, `setQualityField` (HE_Core, `ProjectSettings.h`)
are the one list the API, the step-4 page and a hardware benchmark share.

Not built: typed per-area rows (a name-keyed pair covers every regulator), a `HeSettingsServices`
table for the C++ GameLogic ABI (none exists for `settings`; the direct `HE::api::settings` calls
cover C++).

## 5. Open question from the human, out of scope here

The human's note on the topic asks for hardware detection as an extension of the hardware
benchmark, to pick the best settings for a PC. Not built. The hook it needs is exactly
`setPreset` (step 3) plus `detectQualityPreset`; a benchmark only has to choose a
`QualityPreset` and apply it.

## Handoff

- **Topic / step:** 324 Engine quality settings, step 3 of 5 (global step id 1510). Earlier: step 1
  (id 1508, 523bcac24) model + presets, step 2 (id 1509, 20938d347) renderer/game wiring.
- **Branch / commit:** `claude/engine-quality-settings` (pushed: see the Hive artifact for the sha)
- **Done:** a game or HorizonCode/Lua/Python/C++ script can set and read the quality preset and 18
  single regulators at run time, save them, and reset them; the editor preview shows them while
  playing and drops them at stop.
- **Files touched:** `Project/QualitySettings.h` (NEW, json-free: `ProjectShadowSettings`, the graphics field list,
  `ProjectGraphics`, presets, field table; `ProjectSettings.h` includes it, so nothing else changed), `EngineApi.h` (`settings::Values/Host`, row declarations ~2380-2470),
  `PlayerSettings.cpp` (effective values, rows, JSON), `EngineApi.cpp` (rows ~7190, display names
  ~8450), `HcNodeDocs.cpp` (~1048), `ProjectSettings.h/.cpp` (field table), `GameApplication.cpp`
  (`installPlayerSettings`), `EditorApplication.h/.cpp` (`m_playGraphics`, host hook, sync),
  `tests/fixtures/hcodegen/fixtures.h` (`fxQualitySettings`), `tests/CMakeLists.txt` (`QualitySettings`
  in `HCGEN_CLASSES`), tests in `test_player_settings`, `test_horizoncode_codegen`,
  `test_scripting_binding`, `test_python_scripting`.
- **Verified:** `he_tests` Release build RC 0. Passing: `test_player_settings` 12/12 (6 new),
  `test_project_settings` 41/41, `test_horizoncode_codegen` 54/54 (incl. interpreted == compiled for the
  new rows), `test_engine_api_direct` 9/9, `test_engine_api` 130/130, `test_hc_node_docs` 6/6,
  `test_scripting_binding` 33/33 and `test_python_scripting` 42/42 (one new case each). `GameApplication.cpp.o`
  built by ninja; `EditorApplication.cpp.o` compiled with its recorded command. Full `ctest -j4`: 362/362 (the first run had 2 failures, both mine and fixed: the
  checked-in `tests/fixtures/game_build_config/HEGameBuildConfig.h` embeds the graphics JSON and was regenerated with
  `HE_UPDATE_GAME_BUILD_CONFIG_FIXTURE=1`; `test_hc_codegen_export_build` failed because `EngineApi.h` pulled in
  `ProjectSettings.h` and its `nlohmann/json_fwd.hpp`, see the split below). `MetalRenderer.mm.o`, `GameApplication.cpp.o`,
  `EditorApplication.cpp.o` recompiled after the split.
- **Not verified:** nothing run at runtime (no editor/game launch, no menu, no screenshot); a preset set from
  a script during play in the editor and the viewport following it is untested; Windows/Linux not built.
- **Decisions taken:** quality is a player setting in the existing `settings` store (not a new group, not a
  project edit); name-keyed value rows instead of 18 typed pairs; "Custom" accepted as a no-op; a preset
  replaces earlier single values; the base is the application's graphics at install.
- **Decisions suggested, not taken:** typed per-area HorizonCode nodes; surfacing preset names/labels for
  localisation; exposing `qualityFields` ranges (min/max) to menus.
- **Pending:** step 4 editor UI (preset combo + single regulators in `ProjectSettingsPanel.cpp` ~1349,
  `clamp()` after edits, help entries for every control: `editor_help_audit.py --check`); step 5 verification
  (visible Low vs High in the editor and via script) and the doc for users; the hardware-benchmark idea
  from the human's note only needs `settings::setQualityPreset` / `HE::applyQualityPreset`.
- **Traps:** (0) `EngineApi.h` is SDK-facing and generated game code compiles against it without `nlohmann`: it may include
  `Project/QualitySettings.h` but never `Project/ProjectSettings.h`. Any change to the graphics JSON also needs the
  game-build-config fixture regenerated (`HE_UPDATE_GAME_BUILD_CONFIG_FIXTURE=1 he_tests --source-file=*test_game_build_config.cpp`,
  then rebuild he_tests); steps 1 and 2 left it stale and only the full ctest shows it. (1) `ninja <HorizonEditor/HorizonGame object>` runs the `HorizonSDKRelease` custom target
  first and it FAILS with "dev MISMATCH" after any public header change (stale `lib/dev` stamp), so the
  object is silently not rebuilt: compile it with `ninja -t commands <obj> | tail -1` instead, or build the
  whole editor. (2) New `Host` fields go at the END and are optional; both apps fill what they know.
  (3) `Values::quality` stores normalised numbers; read them back with `qualityValue`, not from the map.
  (4) A new graphics field means: macro row, `EditorConfig` member + load/save, export `put`, an entry in
  `kQualityFields` if scripts should reach it, and a preset-table column if presets should set it.
- **Next steps:** 1. Step 4: add a "Quality" page (preset combo from `qualityPresetNames`, regulators from
  `qualityFieldName`) bound to `ProjectGraphics` + `ProjectShadowSettings` with `applyQualityPreset` /
  `setQualityField`, then `ProjectSettings::clamp()`. 2. Step 5: headless A/B (`HE_DUMP_TEXQUALITY`,
  `scripts/he_shot.py`) on a scene with a mipped texture; a PIE run that calls the rows from a script.
