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
| `textureQuality` | **acts**: `lodMinClamp` 2/1/0 (Low/Med/High) on two samplers, live, no re-upload: `m_materialSampler` (landscape layers, graph-material texture slots) and `m_worldSampler` (base colour of mesh, skinned mesh, decal and blended draws) | ignored (per-texture `glTexParameter`, no sampler objects) | ignored |
| `materialQuality` | stored (`GetQualitySettings`), no feature gated on it | same | same |

`textureQuality` only changes textures that carry a mip chain; a single-level texture (what the
packer cooks, and most loose imports) has no coarser level and looks the same. The landscape
arrays of Catania are 2048 x 2048 with 12 mip levels and 13 layers, so there it does act.
The shared `m_linearSampler` is untouched on purpose: the shadow map, sky environment, AO, UI and
post passes bind it (scene-pass slots 1 to 3), and a mip clamp there would soften lighting.
The scene shaders sample through their slot-0 sampler for the base colour only, which is why a
clone of it (`m_worldSampler`) is safe for slot 0.

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
1. **Texture quality on Catania: first reading was wrong, corrected in section 8.** The first pair (camera 35 m and 3 m up)
   showed no change (mean 0.04 / 0.05) and I blamed the sampler binds. Both the conclusion and the cause I gave (lines
   ~14452 and ~15548 are the UI pass and the landscape weightmap, not the landscape textures) were wrong: the effect only
   appears where the ground is sampled below the clamped mip level, i.e. within about a metre or two of the camera.
2. **The first dump of a session is not comparable:** the very first run had no terrain drawn (5 draws instead of 89, mean diff
   14.9 against the later runs). Throw it away or warm up once; from the second run on, runs are identical.
3. **Ultra, far lake "banding": investigated, not a defect, see section 8.**
4. Not measured: frame time per preset (no perf capture on this machine's load).

Dump override added for this: `HE_DUMP_QUALITYPRESET` in `dumpFrameHeadless` (`EditorApplication.cpp`).

## 8. Follow-up: texture quality on every world sampler, and the Ultra lake "banding"

**Texture quality.** What was found, in order:
* A temporary upload log showed Catania's landscape textures are 2048 x 2048 arrays with 12 mip levels and 13 layers
  (Albedo, Normal, Mask), so a mip clamp can act on them. The scene's base-colour sampler was the shared `m_linearSampler`.
* Section 7's "does nothing" was a measurement artefact: the clamp (mip level 2 for Low) only matters where a pixel covers
  less ground than a mip-2 texel. With 4 m tiles of 2048 texels that is within roughly one to two metres of the camera.
  At a camera 1.7 m above the ground, High vs Low differs clearly (whole frame mean abs 0.85, lower half 1.66, 1.97 % of
  pixels by more than 8; the pebbles go soft, `catania-texture-quality-player-height.png`). High vs Medium differs by 0.02
  there: Medium (level 1) only bites closer than that. At 3 m and 35 m the same pair measured 0.05, so the first test was
  simply too far away.
* The old build (clamp on `m_materialSampler` only) already acted on the landscape: the 3 m pair gives 0.052 old vs 0.066 new.
* Built now (`MetalRenderer.mm`, `ApplyTextureQuality`): a second sampler `m_worldSampler`, a clone of the shared clamp+linear
  one, bound at slot 0 of the four world passes (`EncodeScene`, `EncodeSkinnedObjects`, `EncodeGBuffer`, the blended pass) and
  the decals; texture quality narrows it together with `m_materialSampler`. The shared `m_linearSampler` is NOT clamped: scene
  slots 1 to 3 (shadow map, sky environment, AO) and every UI, post and sky pass bind it, and a mip clamp there would soften
  lighting and the IBL. `ApplyTextureQuality` also runs when the samplers are created, so a quality pushed before the first
  frame (the dump path, a game's first frame) is not lost.
* Effect of the change on Catania: High, Ultra and Low frames are unchanged by it (mean abs 0.08 / 0.01 / 0.00 against the
  frames from before, no pixel > 8), and no sampler is rebuilt at High. Mesh base-colour textures with a mip chain are the
  case the new sampler is for; Catania has none, so that path is verified only for "nothing else changes", not for a visible
  effect.

**Ultra far-lake "banding": not a defect.** The GI-reflection pass produces it, and it is the cloud layer mirrored in the
rippled water:
* gone with GI reflections off (also with SSR off too); unchanged with SSR off, TAA off or GI off alone
  (`catania-ultra-lake-banding-toggles.png`);
* identical for every GI-reflection quality tier (raw / blur / glossy), one bounce vs two, blur on, and render scale 2;
* gone at cloud coverage 0 and at coverage 1 (an overcast, uniform reflection), present at the scene's 0.31
  (`catania-ultra-lake-banding-clouds.png`).
So the shapes are patches of cloud, stretched flat by the grazing angle. The other lakes and the lower presets look "cleaner"
only because the non-ray-traced reflection path does not contain the cloud layer. Nothing to fix; it could be called a
feature of the GI-reflection pass.

## 9. Docs page

The manual section **Quality Settings** (`rendering.html#quality`, between Weather and Performance Features): the presets table,
what a preset leaves alone, texture quality (what it reaches, mip chain and distance caveats, Metal only), material quality,
backends, and the script rows with a Lua and a Python example. Screenshot of how the editor's reader shows it:
`manual-quality-settings-section.png`.

Where it lives and how it got into the editor (this took a decision):
* Source: `Website/HorizonEngineDocs/rendering.html` plus a sidebar link, and `docs-index.json` regenerated with
  `build_docs_index.py` (diff checked: exactly one added section). Committed in the Website repo on `main` (`cf6a8c5`),
  **local only: not pushed, not deployed** (publishing the site is a separate step that wants the human's word).
* The editor's offline manual (`EditorDeps/Docs/he-docs.json`) was NOT regenerated: no Website checkout reproduces the committed
  bundle (it already holds water, sequencer and settings sections that exist on other Website branches only, so a
  regeneration from `main` deleted 10 sections and changed 20). Instead exactly one converted section, `quality`, was spliced into
  the `rendering.html` page of the committed bundle (843 added lines, 0 removed, same JSON formatting). `build_docs_bundle.py --check`
  would have reported the bundle stale before this change as well; the next regeneration from the branch that carries every docs
  change will produce the same section.
* The 16 help entries of the Quality page point at `rendering#quality` now (they were empty).
* Tests: `test_docs_library` asks for the section itself (not the stale-anchor fallback), its two tables and listings, and a
  search hit for "quality preset"; `test_ui_shot` has a scene for it.

## Handoff

- **Topic / step:** 324 Engine quality settings, after step 5 (ids 1508 to 1512 done): follow-ups asked for after the Catania
  comparison (texture quality on every world sampler, banding) and now the docs page. No PR (the human asked for none).
- **Branch / commit:** `claude/engine-quality-settings` (pushed: see the Hive artifact). No pull request opened. Website repo:
  `main` `cf6a8c5`, local only.
- **Done:** texture quality reaches mesh base colours as well as the landscape/graph-material textures on Metal; the banding is
  explained; section 7's wrong finding is corrected in sections 7 and 8; the manual has a Quality Settings section (section 9).
- **Files touched:** `MetalRenderer.h` (`m_worldSampler`, `m_textureMinLod`, `ApplyTextureQuality`), `MetalRenderer.mm`
  (`ApplyTextureQuality` ~12500, creation in `CreateScenePipeline` ~7950, five slot-0 binds), `EditorHelp.cpp` (16 anchors),
  `EditorDeps/Docs/he-docs.json` (one spliced section), `tests/test_docs_library.cpp`, `tests/test_ui_shot.cpp`, this doc, `*.png`;
  Website: `HorizonEngineDocs/rendering.html`, `docs-index.json`.
- **Verified:** editor builds and runs; texture-quality 2/1/0 pairs at 3 m and 1.7 m; the log line "texture quality N — material and
  world samplers start at mip level M" shows the rebuild; High/Ultra/Low unchanged by the change; full `ctest -j4` 362/362 (twice, before
  and after the docs work); `editor_help_audit.py --check` passes; the manual section renders in the reader (screenshot looked at);
  no editor process left; the real Catania untouched.
- **Not verified:** the world sampler's visible effect (no mesh in Catania has a base-colour texture with mips); the live (per-frame)
  push path with a script during play; frame time per preset; D3D11/D3D12/Vulkan/OpenGL (texture quality is ignored there: no sampler
  objects on GL, per-texture `glTexParameter`); the website page in a browser (only the editor reader was looked at); the bundle
  against a full regeneration (see section 9).
- **Decisions taken:** clamp a CLONE of the shared sampler for slot 0 instead of the shared sampler itself (slots 1 to 3 and the
  UI/post/sky passes use it); particles, asset previews and the landscape weightmap keep full detail; splice one section into the
  committed manual bundle rather than regenerate it from an incomplete Website checkout; Website commit local only.
- **Decisions suggested, not taken:** GL texture quality via `GL_TEXTURE_MIN_LOD` over its texture caches; a texture-quality line in the
  Quality page that says "Metal only, textures with mip chains"; pushing and deploying the Website docs.
- **Pending:** push + deploy of the Website docs (human's word); perf per preset (`he_perf_capture.py`); the PR into `release/0.8.0`
  when the human says so.
- **Traps:** (1) `ninja HorizonEditor` can fail "dev MISMATCH": `HorizonVersion.h` bakes the git commit at configure time and the
  dev tree and `out/build/release/sdk-release` configured at different commits; run `cmake out/build/release` and
  `cmake out/build/release/sdk-release` once, then build. (2) The first dump of a session has no terrain: always discard it.
  (3) Dumps need `HE_DUMP_SKYTEST=1` for the camera variables and `HE_DUMP_TERRAINTICK=1` for terrain chunks; the project is
  passed as the `.heproj` argument. (4) Run on a copy; opening a project in the editor can write to it. (5) A texture-quality test
  needs the camera within about 2 m of the ground; farther away every mip is above the clamp and nothing changes.
- **Next steps:** 1. A textured mesh with a mipped base-colour texture as the witness for `m_worldSampler`. 2. Website push + deploy and
  a regeneration of the manual bundle from the branch that has every docs change (`build_docs_bundle.py`; the spliced section must
  come out identical). 3. PR only on the human's word.
