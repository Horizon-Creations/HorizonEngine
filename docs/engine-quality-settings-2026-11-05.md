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

## 3. Backend support (from the code comments, not re-verified on hardware)

* Cascaded shadows: Metal + OpenGL; D3D11/D3D12/Vulkan keep one whole-scene map and ignore
  the shadow push (`ProjectShadowSettings` comment).
* Anti-aliasing: methods a backend cannot do fall back at push time (TAA Metal + GL,
  MetalFX Metal only).
* SSR, GI, GI reflections: capability-gated per backend in the push code.
* D3D and Vulkan hardware acceptance is out of scope for the topic (not checkable on the
  Mac); a level a backend lacks is ignored silently.

## 4. Open question from the human, out of scope here

The human's note on the topic asks for hardware detection as an extension of the hardware
benchmark, to pick the best settings for a PC. Not built. The hook it needs is exactly
`setPreset` (step 3) plus `detectQualityPreset`; a benchmark only has to choose a
`QualityPreset` and apply it.

## Handoff

- **Topic / step:** 324 Engine quality settings, step 1 of 5 (global step id 1508)
- **Branch / commit:** `claude/engine-quality-settings`, off `release/0.8.0` @ 17815c9bf (pushed: see Hive artifact)
- **Done:** `QualityPreset` + apply/detect/name functions, stored tag `graphics.qualityPreset`
  (default Custom, reconciled in `clamp()`), `textureQuality`/`materialQuality` fields; old files load unchanged.
- **Files touched:** `ProjectSettings.h` (enum, field-list rows, `preset` member, 4 functions),
  `ProjectSettings.cpp` (`kPresetRows`, functions, equality/toJson/fromJson/clamp),
  `EditorConfig.h` (`TextureQuality`, `MaterialQuality`), `EditorApplication.cpp` (load/save of the two keys),
  `ExportDialogPanel.cpp` (config.json carries them), `tests/test_project_settings.cpp` (6 new cases at the end).
- **Verified:** `he_tests` Release build in `out/build/release` (RC 0); `test_project_settings` 38/38,
  `test_editor_graphics` 5/5, `test_export_profiles` 52/52, `test_mcp_tools_settings` 21/21 (run from
  the build dir); `EditorApplication.cpp.o` and `ExportDialogPanel.cpp.o` compile;
  `editor_help_audit.py --check` passes (no UI added).
- **Not verified:** no preset reaches a renderer yet (step 2); `HorizonEditor` and `HorizonGame` not
  fully linked or run; no backend other than the macOS build compiled here; table values are not measured.
- **Decisions taken:** presets are a function over `ProjectGraphics` + `ProjectShadowSettings`, no second
  struct; default tag Custom so "default-constructed = today" holds; Low..Ultra set `graphics.enabled = true`
  (otherwise the viewport and export ignore them), Custom never does; only cost-driving fields are in a preset;
  new levels 0 Low / 1 Medium / 2 High with 2 = today.
- **Decisions suggested, not taken:** whether High should switch GI on (it does now; GI is capability-gated);
  whether the preset table should move out of the .cpp into data later for the benchmark.
- **Pending:** step 2 consumers for texture/material quality and per-backend gaps; step 3 API; step 4 UI; step 5.
- **Traps:** `EditorGraphics::capture` copies values without touching `preset`, so callers that capture
  must call `ProjectSettings::clamp()` (or `detectQualityPreset`) afterwards to refresh the tag. A new
  graphics field needs 4 sites: macro row, `EditorConfig` member, load/save in `EditorApplication.cpp`
  (~1731 / ~15785), `put` in `ExportDialogPanel.cpp`, and the game's read in `GameApplication.cpp` once consumed.
  Worktree configure recipe: shared `_deps` of the main checkout via `FETCHCONTENT_SOURCE_DIR_*`, ccache,
  Ninja from CLion; a full he_tests build took well under 30 min with ccache.
- **Next steps:** 1. Step 2: add `TextureQuality`/`MaterialQuality` reads in `GameApplication.cpp` (~3600) and wire
  them into texture upload and material feature gating, per backend, documenting ignores here.
  2. Step 3: `HE::api` rows `getPreset/setPreset` plus per-field get/set (see memory "four places" rule).
  3. Step 4: quality page in `ProjectSettingsPanel.cpp` (~1349) with a preset combo; call `clamp()` after edits.
