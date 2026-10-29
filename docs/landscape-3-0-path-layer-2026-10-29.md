# Landscape 3.0 Folge — Step 3: Path-Layer je Gruppe

## What this step adds

A new paint-only layer, "Path", in every group's auto-landscape material. It is never
placed automatically; it only appears where a human paints it, reading its own
standalone Albedo texture (tiled at the ground tiling, like Grass/Dirt) and Dirt's
existing normal map (not a texture of its own — see "Decisions taken").

## The channel layout decision (binding for Thema 330 and step 4)

The original plan ("kAutoLandscapeRoleCount 4->6, Path+Water, depth 6V+1") turned out to
be unsafe and was **not** implemented that way. Two hard constraints found during this
step:

1. **Weight channels 6 and 7 are never free.** `TerrainComponent::mixGroup`'s two group
   shares live there (`TerrainComponent.h:100-109`), and the shader
   (`emitLayerBlendNode`, `MaterialGraph.cpp`) reads channels `0..names.size()-1`
   contiguously and *normalises* the paint blend by their weight sum. A material whose
   own paint-layer list reaches past channel 5 would, without care, read the mix-group
   share bytes as paint weight — silently wrong rendering on any terrain that uses mix
   groups, and a reinterpretation of old scene data, not just new.
2. **A material graph is capped at `kMatMaxGraphTextures` (4) distinct textures.**
   Mountains/Forest/Grassland/Desert already use all 3 (their Albedo/Normal/Mask
   arrays). A Path role with its own Albedo *and* Normal would need a 5th slot that
   does not exist; the excess silently falls back to the default mesh texture
   (`heTex0`) with no error. That is why Path has **no normal texture of its own** —
   it reads Dirt's array slot (`AutoLandscapeMaterial.cpp`, the `path` block right
   after `dirt`'s raw read).

**The resolution, now in `kAutoLandscapePaintLayerNames`:**
`"Auto\nGrass\nDirt\nRock\nSnow\nPuddles\n\n\nPath"` — channels 0-5 keep their old
meaning untouched (old scene bytes are not reinterpreted), channels 6/7 are a
deliberate **empty-name gap** that the shader now skips (see below), and **Path is
channel 8**. **Channel 9 is reserved for Water** (step 4) — append `"\nWater"` with no
further gap, nothing else to change for the gap mechanism itself.

If step 4 or Thema 330 need a *texture-backed, variant/bombing-capable* role instead of
a single-channel paint flag, that still requires the `kAutoLandscapeRoleCount` bump the
original plan called for — and, separately from the channel question, it will need to
free a texture slot (either raise `kMatMaxGraphTextures`, a global, every-backend change,
or drop one of the three shared arrays' use for that new role). Neither of those
decisions was made here; both are now unblocked by seeing exactly where they bite.

## The empty-name gap mechanism (reusable beyond Path)

- `matLandscapeLayerNames` (`MaterialGraph.cpp`) now **keeps** an empty segment between
  two `\n`s instead of dropping it — list position is weight channel, with no exception.
  The one special case: an input that is *entirely* blank (`""` or whitespace) still
  reads as `{"Layer 1"}`, not a zero-length gap list — that is a fresh, unnamed node, not
  a reserved channel.
- `emitLayerBlendNode` skips an empty-named channel in both the weighted sum and the
  weight sum, so it contributes nothing and does not dilute the normalisation — verified
  byte-identical codegen for every *existing* (gap-free) name list.
- The Paint tab (`TerrainTools.cpp`) skips drawing a row for an empty name, but keeps
  the loop index as the channel, so the next real layer still paints its own channel.
- `AutoLandscapeMaterial.cpp`'s `wA`/`wB` weight-extraction nodes and the three final
  `layerBlend` mixes were extended from 6 to 9 entries each; the two gap slots take a
  harmless filler (`none`/`s2.*`) since the codegen skip makes their wiring unobservable.

## Decisions taken

- Path reads Dirt's array slice for its normal (no normal texture of its own) — reason:
  `kMatMaxGraphTextures` budget, see above.
- Path's mask (AO/roughness/height) is a flat constant, not a texture — kept the scope
  to one new texture per group.
- Path's Albedo is a new standalone (non-array, non-bombed) texture per group, generated
  procedurally by `landscape_tex_gen` (`writePathTextures`, ids `0x4D0..0x4D3` — four,
  one per group, clear of every existing id block up to `0x4CB`). **Not committed**:
  like the three per-group arrays, texture *content* is not checked into this repo
  (generated on demand); only the four `M_AutoLandscape*.hasset` material files are.
- Role label "Path" is the same across all four groups (matches the existing
  precedent of "Snow" being the same label everywhere).
- No new exposed parameters for Path (it needed none); `kAutoLandscapeParamCount`
  (13) is unchanged by this step.

## Decisions suggested, not taken

- Giving Path real, group-distinct photographs (grass_path_3 / forest_ground_04 /
  sandy_gravel_02 / rocky_trail, per the original texture suggestions) is step 6's job
  ("Echte Texturen packen"); `writePathTextures` only writes a procedural placeholder.
- Whether Path should ever get a normal map of its own depends on whether the
  `kMatMaxGraphTextures` budget is raised globally — not decided here, flagged to the
  queen (Thema 329 topic, request #161) before implementing, who approved the
  no-raise / own-texture-pair-minus-normal compromise.

## Verified (Windows, NN-WS03)

- `test_landscape_groups`, `test_material_graph`, `test_auto_landscape_variants`,
  `test_terrain_tools_ui`, `test_terrain_mix_groups`, `test_terrain`: all green.
- Full `he_tests` run: 3655/3661 cases, 841621/841634 assertions passed; the 6 failures
  (gamepad hot-plug/hardware, a physics-threading SEH crash, an unrelated codegen-export
  case, an inspector-UI replication case) are pre-existing and untouched by this step's
  files.
- The four `M_AutoLandscape*.hasset` files were regenerated with
  `landscape_tex_gen EditorDeps/EngineContent/Materials --material [--group G]` and the
  "shipped == builder" tripwire passes for all four.
- Not verified: Linux/macOS (no toolchain here); the Paint tab's new row-skip and the
  "click still paints channel 8" path were checked by the existing UI test suite, not by
  hand in a live editor.

## Next steps

1. Step 4 (Water/Kiesel layer): append `"\nWater"` to every group's `paintLayerNames`
   (channel 9, no new gap needed) and wire it the same way Path is wired, or make the
   bigger role-count/texture-budget decision first if it needs its own array role.
2. Step 6 ("Echte Texturen packen"): replace `writePathTextures`'s procedural
   placeholder with real per-group photographs at the same four asset paths.
3. Thema 330 (Seasons): read this doc before touching `kAutoLandscapePaintLayerNames`
   or `kAutoLandscapeRoleCount` — the channel 6/7 gap and channel 8/9 assignment here
   are load-bearing for old-scene and mix-group compatibility.
