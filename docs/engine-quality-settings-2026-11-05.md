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

## 4. Open question from the human, out of scope here

The human's note on the topic asks for hardware detection as an extension of the hardware
benchmark, to pick the best settings for a PC. Not built. The hook it needs is exactly
`setPreset` (step 3) plus `detectQualityPreset`; a benchmark only has to choose a
`QualityPreset` and apply it.

## Handoff

- **Topic / step:** 324 Engine quality settings, step 2 of 5 (global step id 1509). Step 1 (id 1508,
  commit 523bcac24): `QualityPreset` model, tag, `textureQuality`/`materialQuality` fields.
- **Branch / commit:** `claude/engine-quality-settings` (pushed: see Hive artifact for the sha)
- **Done:** the game's graphics push is struct-driven (`m_projectSettings.graphics`), so a script can
  change it at run time; `IRenderer::SetQualitySettings` + `TextureMinLodForQuality`; Metal acts on
  texture quality; editor pushes the same.
- **Files touched:** `IRenderer.h` (~640, `QualitySettings`), `MetalRenderer.h/.mm`
  (`SetQualitySettings`, `m_materialSamplerMinLod`), `GameApplication.cpp` (init fill ~716, push block
  ~3480-3640), `EditorApplication.cpp` (~3722, ~9130), `ProjectSettings.h/.cpp`
  (`readGraphicsFromConfig`), `tests/test_project_settings.cpp` (3 new cases).
- **Verified:** `he_tests` Release build RC 0; `test_project_settings` 41/41, `test_editor_graphics`
  5/5, `test_export_profiles` 52/52; `MetalRenderer.mm.o`, `GameApplication.cpp.o`,
  `EditorApplication.cpp.o`, `ExportDialogPanel.cpp.o` compile. Python check: every macro row's
  `put("Key", cfg.Key)` in `ExportDialogPanel.cpp` equals its `EditorConfig` column (38/38), and all 34
  `getCustomConfig*` defaults in the game block equalled the macro defaults before they were replaced.
- **Not verified:** nothing run at runtime. No editor/game link, no screenshot: a Low vs High A/B
  (`HE_DUMP_TEXQUALITY`) would show nothing on single-level textures, so it needs a scene with a
  mipped texture. GL, D3D11/12 and Vulkan not compiled or touched.
- **Decisions taken:** texture quality = sampler min-LOD, not skip-at-upload (switches live, no cache
  invalidation); Metal only, others documented as ignored; `materialQuality` plumbed, no consumer;
  game struct filled from config keys at start so startup is byte-for-byte the old behaviour.
- **Decisions suggested, not taken:** GL texture quality via `GL_TEXTURE_MIN_LOD` over its texture caches;
  a material-graph "quality level" input as the first real `materialQuality` consumer.
- **Pending:** step 3 API, step 4 UI, step 5 verification incl. a visible A/B.
- **Traps:** (1) PIE: nothing snapshots `currentProject().settings`, so a script that applies a preset
  during play dirties the project; step 3 must snapshot/restore `graphics` + `shadows` around PIE
  (find start/stop near `startPlayNetSession`, `EditorApplication.cpp` ~5354) or route runtime changes
  to a PIE-local copy. (2) `Ctx` (EngineApi.h): new members go at the END, both apps build it by
  positional aggregate init. (3) An `api::settings::Host` (player's settings menu) already exists in the
  game, see `GameApplication.cpp` near the gamepad deadzones; check whether quality rows belong there.
  (4) `EditorGraphics::capture` leaves `preset` alone: call `ProjectSettings::clamp()` after it.
  (5) Do not edit a file while `ninja` compiles it (stale object).
- **Next steps:** 1. Step 3: `HE::api::quality` rows (get/setPreset by name, per-field get/set for
  texture, shadow, AA...), applying to the host's `ProjectSettings` through a Ctx callback or pointer
  added at the end of `Ctx`; four-places rule for new rows (see memory). 2. PIE snapshot (trap 1).
  3. Step 4: quality page in `ProjectSettingsPanel.cpp` (~1349).
