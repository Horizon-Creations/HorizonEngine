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

## 6. Editor page (step 4)

**Project settings ▸ World ▸ Quality** (`ProjectSettingsPanel::Page::Quality`, `drawQualityPage` in
`ProjectSettingsPanel.cpp`; rail entry in `EditorSettingsPanel.cpp`, `Page::WorldQuality`). It edits the same members
as the Shadows and Post-Processing / Reflections & GI pages, so there is one model:

* **Preset** combo: Low, Medium, High, Ultra, Custom. It always shows `detectQualityPreset` of the values, so a moved
  slider reads Custom at once. Picking a named preset runs `applyQualityPreset` (graphics block on); picking Custom
  changes nothing.
* **Resolution and edges:** anti-aliasing method, render scale. **Shadows:** resolution, distance, cascades.
  **Screen effects:** ambient occlusion, SSR (+ quality), global illumination (+ shadow quality), GI reflections (+ quality,
  bounces); the sub-rows appear only while their effect is on. **Asset detail:** texture quality, material quality.
* While the project does not set its own graphics, the controls show what the editor draws with (an amber line says so);
  the first preset or edit starts from those values and turns the project's graphics on, so nothing jumps.
* The model follows every change; the file is written once when no control is held (same split as the other pages).
* Screenshots (headless, `test_project_settings_ui`): `docs/engine-quality-settings-2026-11-05/quality-page-*.png`
  (`ultra-top`, `ultra-bottom`, `no-project-graphics`).
* 16 help entries (`Quality/<label>`, `EditorHelp.cpp` ~5088) and a "Project: quality" Settings Reference section;
  docs-site anchors are left empty (step 5 writes the page).

## Handoff

- **Topic / step:** 324 Engine quality settings, step 4 of 5 (global step id 1511). Earlier: step 1 (1508) model,
  step 2 (1509) renderer/game wiring, step 3 (1510) script API.
- **Branch / commit:** `claude/engine-quality-settings` (pushed: see the Hive artifact for the sha)
- **Done:** Project settings ▸ World ▸ Quality page: preset combo + the regulators the presets are made of, help for
  every control, rail entry, headless screenshots.
- **Files touched:** `ProjectSettingsPanel.cpp` (`drawQualityPage` ~795, page table), `ProjectSettingsPanel.h` (`Page::Quality`),
  `EditorSettingsPanel.h/.cpp` (`WorldQuality`: enum, rail, two mappings), `EditorHelp.cpp` (~5088 entries, ~10170 reference),
  `tests/test_project_settings_ui.cpp` (page in the loop + 2 cases at the end).
- **Verified:** he_tests build RC 0; `test_project_settings_ui` 8/8 (page draws over a real project and writes nothing by being
  looked at; an Ultra project reads Ultra; a click on Ambient occlusion with the editor at SMAA / 0.8 render scale turns
  the block on, keeps those values, reads Custom, and survives a reload through a fresh ProjectManager); full `ctest -j4`
  362/362; `editor_help_audit.py --check` passes (1273/1273, was 1257 before the topic); screenshots looked at.
- **Not verified:** the combo popup was not clicked (the preset combo is exercised only through its model);
  no live editor was launched, so the viewport following a preset change on screen is unseen; no hover tooltips looked at;
  Windows/Linux.
- **Decisions taken:** the page lives under World next to Shadows (project data, not a Preferences page); shadows appear
  on both pages and edit the same members; bloom, DoF, motion blur stay on their own pages; picking Custom is a no-op.
- **Decisions suggested, not taken:** a "Reset to project defaults" button; showing the backend that ignores a level next to the
  row (e.g. a greyed "Metal only" tag on Texture quality).
- **Pending:** step 5 only: user documentation (docs site page `rendering#quality` and the anchors in the help entries),
  a visible Low vs High A/B in the live editor (`HE_DUMP_TEXQUALITY`, `scripts/he_shot.py`, a scene with a mipped texture),
  a PIE run that calls the quality rows from a script, and the final topic summary/PR.
- **Traps:** (1) the UI tests' display is 900x620 and the page is taller: scroll with `ImGui::SetScrollY` in the body lambda
  (see `showQualityPage`). (2) A new `ProjectSettingsPanel::Page` needs `pageTitle`, `drawPage`, `requestOpenProjectPage`,
  `projectPageOf`, the rail table and `EditorSettingsPanel::Page` — all six, or a page is reachable but not listed.
  (3) `EditorGraphics::capture` copies the editor's values but not `enabled`/`preset`.
- **Next steps:** 1. Step 5: live-editor witness (open a project in the editor with `HE_HIDDEN_WINDOW=1` per
  `docs/headless-runs.md`, set a preset through the page or `HE_DUMP_TEXQUALITY`, compare frames); never leave a
  HorizonEditor running afterwards. 2. Write the docs page and fill the anchors. 3. Open the PR into `release/0.8.0`.
