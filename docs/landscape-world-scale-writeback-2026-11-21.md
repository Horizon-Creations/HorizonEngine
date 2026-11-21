# Tile landscape: sculpt and paint that write back (Thema 326, 2026-11-21)

Delivers the design `docs/landscape-world-scale-sculpt-writeback-design-2026-11-06.md` (items 2 to 7; item 1 was step 14). That
doc's "not delivered" status is superseded by this one.

## Built

| Part | Where | What |
|---|---|---|
| Overlay | `LandscapeEdit.h/.cpp` `LandscapeEditSource` | In front of any tile source: edited tiles answer from memory, the rest from the base; `coarse()` is a patched copy. Saved as `<pack>.edits` beside the pack (`save`/`loadFile`), stamped with `LandscapePackSource::identity()` so a file left from another build of the same path gives nothing |
| Inverse | `landscapeWriteRegionTerrain` | Region terrain back into whole tiles: 16 bit quantised and clamped, a border sample written to every tile that holds it, weights re-interleaved, water rasters carried over; only tiles whose level 0 changed come out |
| Session | `LandscapeEditSession` | `focus(x, z, radius)` picks 1, 2 or 4 tiles around a brush with a sample to spare (never 3); the tool edits `terrain()` with `TerrainSculpt` / `TerrainPaint`; `commit()` per dab; `beginStroke/endStroke` = one undo step (32 kept); `undo/redo` |
| No blink | `LandscapeStore::replace`, `LandscapeAtlas::sync` | A resident tile takes new content in place; the atlas uploads it again into the slot it has (no table change) |
| Stroke end | `LandscapeRuntime::coarseChanged`, `tileChanged` | Coarse copy patched once, tiles dropped once so ground bodies, foliage, navigation and water are built again from the new tile |
| Far levels | `LandscapeRenderStatic::coarseRevision`, `OpenGLLandscape.cpp`, `MetalLandscape.mm` | Coarse images uploaded again, the atlas left alone (never a new generation: that empties every backend's atlas) |
| Runtime | `LandscapeRuntime::setEditable / edits() / packFile()` | A pack with an edits file, or an editable runtime, runs on the overlay |
| Editor | `LandscapeWorldTools.cpp` (`drawEditBlock`, `brushInViewport`), `ViewportPanel.cpp`, `EditorApplication.cpp` | "Edit Tiles" in the Tile Landscape section, Landscape mode only: ring cursor, Raise / Lower / Smooth / Flatten / Paint Layer, Undo Stroke, Redo Stroke, Save Tile Edits. Saving the scene saves the edits; unsaved edits make the scene dirty (`sceneHasUnsavedChanges`) |
| Export | `LandscapeExport.cpp`, `landscapePackBakeEdits` | Edits are baked into the shipped copy of the pack (streaming rewrite, other sections carried over); the far image is baked from the edited world |

## Verified (Mac, Apple M5, Release)

* `tests/test_landscape_edit.cpp`, 9 cases: exact round trip of an untouched region; one, two and four tiles for a dab inside, on a border,
  on a corner, with equal shared samples; the real brushes (`TerrainSculpt::apply`, `TerrainPaint::paint`) reaching tiles; overlay never
  reading the base for an edited key; coarse copy patched and restored; edits file round trip, cut short, junk, another world's stamp;
  stroke in place (same slot, no table update), coarse revision at stroke end, undo/redo; bake into a pack copy.
* Whole `he_tests` suite on the write-back commit: 6458 / 6458. Landscape files after the fixes: 186 / 186.
* `landscape_gpu_check` scene 5 (exit 0 overall): a dab near the camera moves 4509 px with the stroke still open and every tile stays
  resident; a stroke on far ground (12 km) moves 57 px after it ends on OpenGL and Metal, 0 on Vulkan (as built).

## Not verified / not done

* **The editor brush was never driven**: no click in a running editor, no UI test. It compiles, the help audit is clean, and everything
  below it is tested. First thing to do by hand.
* **Vulkan, D3D11, D3D12** do not look at `coarseRevision`: after a stroke the far levels stay as they were until the landscape is
  opened again. Near tiles (the atlas) are fine everywhere, that path has no backend code.
* The end of a stroke drops the changed tiles once (one or two frames on the far levels there). In-place refresh of physics, foliage,
  navigation and water would remove that.
* Water on edited ground is not rebuilt specially (the region's sheets follow the tile reload); editing water itself is not offered.
* Baking edits into the **project's** pack from the editor (only the export's copy is baked). Foliage, water and mountain tools on tiles.
* Height quantisation stays option (a) of the design: 4.6 cm steps at 3000 m, shown in the section.

## Handoff

- **Topic / step:** 326, write-back (design items 2-7). No board step; owner asked for it directly.
- **Branch / commits:** `claude/landscape-weltmassstab-100km`, `eea2f8106` plus the fix commit after it.
- **Decisions taken:** dab = in-place replace, stroke end = drop and reload; undo holds tiles, not blobs; edits file is rewritten whole;
  the brush is a small tool of its own in `LandscapeWorldTools`, not a target abstraction inside `TerrainTools.cpp` (2871 lines built
  around an entity's terrain); world identity = hash of the pack's first 32 header bytes and its tile index.
- **Traps:** never bump the generation for an edit; `LandscapeEditSession::read` caches the region's tiles, so anything that changes the
  overlay behind the session must go through it; `far`/`near`/`small` are Windows macros; the session's region terrain must never enter
  a registry.
- **Next steps:** 1. Drive the brush in the editor once (Landscape mode, a Tile Landscape selected, Edit Tiles on). 2. `coarseRevision`
  on Vulkan (step 27) and D3D (step 28). 3. Fan-out without the drop at stroke end. 4. "Bake edits into the pack" as an editor action.
