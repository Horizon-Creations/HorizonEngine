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

## 7. Comparison on the Catania project (step 5, live editor)

Setup: the real `HorizonEditor` built from this branch (Release, Metal), one headless frame per preset
(`HE_DUMP_QUALITYPRESET=<Low|Medium|High|Ultra>`, the same play-local road a script's preset takes), 1280x720, noon
(`HE_DUMP_TOD=0.5`; the scene is set to midnight), one fixed camera over the lakes (0, 35, 40), `HE_SKY_TIME=30`,
`HE_DUMP_TERRAINTICK=1` (builds the terrain chunks), 8 settle frames. Run on a COPY of Catania (without Export/Exports,
15 MB, the user's uncommitted state) with a private `HE_CONFIG_DIR`; the real project's git status and files were checked
identical before and after. Images: `docs/engine-quality-settings-2026-11-05/catania-*.png`.

| Pair | mean abs pixel diff (0-255) | pixels differing by > 8 |
|---|---|---|
| noise floor: High vs High again | 0.03 | 0.0 % |
| noise floor: default vs default again | 0.00 | 0.0 % |
| default (project's own values) vs Low | 1.84 | 3.3 % |
| default vs Medium | 0.28 | 0.6 % |
| default vs High | 2.85 | 10.1 % |
| default vs Ultra | 3.71 | 12.4 % |
| Low vs Medium / Medium vs High / High vs Ultra | 1.89 / 2.71 / 2.22 | 3.6 / 9.6 / 4.6 % |
| Low vs Ultra | 3.56 | 12.1 % |

What the frames show: Low is crisper and darker (render scale 0.75 and no ambient occlusion or global illumination give hard
contrast on the mountains); Medium is almost the project's current look (0.28); High and Ultra light the mountains softly
(global illumination bounce, ambient occlusion), Ultra adds temporal smoothing (TAA) and reflections on the lakes. Every
preset step is visible and the repeat runs are bit-stable, so the differences are the presets and not noise.

**Findings:**
1. **Texture quality does nothing on Catania** (High with texture quality 0 vs 2: mean 0.04 and 0.05, no pixel > 8, also with
   the camera 3 m above the ground). Cause found in code, not guessed: step 2 clamps `m_materialSampler` only (the
   graph-material `heTexP` slots, six `setFragmentSamplerState:...m_materialSampler` sites in `MetalRenderer.mm`), while the
   landscape layers and base textures bind `m_linearSampler` (e.g. ~14452 `atIndex:5`, ~15548 `atIndex:13`), which the UI,
   post and sky passes share and which therefore cannot be clamped globally. A dedicated world-texture sampler for those binds
   is the fix; whether the landscape textures carry mip chains at all was not checked.
2. **The first dump of a session is not comparable:** the very first run had no terrain drawn (5 draws instead of 89, mean diff
   14.9 against the later runs). Throw it away or warm up once; from the second run on, runs are identical.
3. **Ultra, far lake:** the far lake shows horizontal banding in the GI-reflection image (visible in
   `catania-low-vs-ultra-crop.png`). Not investigated, not part of this topic; mentioned so it is not mistaken for the preset.
4. Not measured: frame time per preset (no perf capture on this machine's load).

Dump override added for this: `HE_DUMP_QUALITYPRESET` in `dumpFrameHeadless` (`EditorApplication.cpp`).

## Handoff

- **Topic / step:** 324 Engine quality settings, step 5 of 5 (global step id 1512): live comparison on Catania; no PR (the human
  asked for none). Earlier: 1508 model, 1509 renderer/game, 1510 script API, 1511 editor page.
- **Branch / commit:** `claude/engine-quality-settings` (pushed: see the Hive artifact). No pull request opened.
- **Done:** Low/Medium/High/Ultra rendered on the user's Catania project in the real editor, numbers and images in section 7.
- **Files touched:** `EditorApplication.cpp` (~9060 `HE_DUMP_QUALITYPRESET`), this doc, `docs/engine-quality-settings-2026-11-05/catania-*.png`.
- **Verified:** editor builds and runs (Release, Metal) on the branch; five presets + two baselines + two High repeats + two
  texture-quality pairs rendered; noise floor 0.00-0.03; real Catania untouched (git status and mtimes checked).
- **Not verified:** frame time per preset; D3D11/D3D12/Vulkan/OpenGL; the documentation page (`rendering#quality`) is not
  written, so the help entries still have empty anchors; a script calling the quality rows during play in the live editor;
  the preset combo clicked in the live editor.
- **Decisions taken:** compare on a copy with a private config dir; noon and a fixed camera instead of the scene's own midnight
  and default view; the first run discarded as a cold-start artifact.
- **Decisions suggested, not taken:** give world textures (landscape layers, base textures) their own sampler so texture quality
  reaches them (finding 1); investigate the Ultra far-lake banding; write the docs page.
- **Pending:** the texture-quality fix above, the docs page, a perf measurement per preset (`he_perf_capture.py`), and when the human
  wants it, the PR into `release/0.8.0`.
- **Traps:** (1) `ninja HorizonEditor` can fail "dev MISMATCH": `HorizonVersion.h` bakes the git commit at configure time and the
  dev tree and `out/build/release/sdk-release` configured at different commits; run `cmake out/build/release` and
  `cmake out/build/release/sdk-release` once, then build. (2) The first dump of a session has no terrain: always discard it.
  (3) Dumps need `HE_DUMP_SKYTEST=1` for the camera variables and `HE_DUMP_TERRAINTICK=1` for terrain chunks; the project is
  passed as the `.heproj` argument. (4) Run on a copy; opening a project in the editor can write to it.
- **Next steps:** 1. Decide with the human: fix texture quality for the landscape (new sampler for the world-texture binds in
  `MetalRenderer.mm`, then re-run the ground-level pair with texture quality 0 and 2). 2. Docs page + anchors. 3. PR only on the human's word.
