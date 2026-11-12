# Tile landscape: sculpt/paint that writes back (design, Thema 326)

Status: **design only, nothing built.** Written 06.11.2026 after reading the code listed under "What I read". Decisions are marked *taken* (by me, reversible) or *open* (the owner decides).

## The problem

A pack (`LandscapeSource.h`, "Pack file") is write-once: a fixed index of `u64 offset, u32 size` per tile, the tile blobs back to back, then the coarse world, the water table and the HLOD tail. Editing a tile changes its blob size, so it cannot be replaced in place. Four more facts make "just save the tile" wrong:

1. **Tiles share their border samples** with the neighbour (level 0 has `hCells + 1` samples a side, `LandscapeTile.h`). A dab on a border sample must change up to four tiles, or a seam opens.
2. **The coarse world is derived** from the tiles' mips (`landscapeCoarseSampleOf`), always resident, and uploaded once per `coarseGeneration`. An edited tile makes the far levels stale.
3. **Heights are 16 bit** (4.6 cm per step at the default 3000 m scale). The existing brushes work in float metres on a `TerrainComponent`; a small `Smooth` or `Roughen` dab that moves a sample by less than half a step is lost when it is written back. This needs a decision (below), not a silent loss.
4. **Everything downstream is keyed by tile**: the store's decoded tile, the atlas slot (GPU), `LandscapePhysics` ground, foliage scatter, the nav tile, water sheets. None of them can be told "tile (x, z) changed" today (`LandscapeStore` has `clear()` and eviction, no `invalidate(key)`).

## What exists and can be reused (verified by reading)

* `landscapeRegionTerrain` (`LandscapeTileTerrain.h`) assembles an n x n block of resident tiles into one `TerrainComponent` (absolute metres, weights de-interleaved into three pages). **The inverse does not exist.**
* `TerrainSculpt::apply` / `TerrainPaint` / `TerrainGenerate` are pure functions on a `TerrainComponent&` and report a dirty rect (`markRegionDirty`). They run on a stack terrain, which is the only place the region terrain may live (its header: never in a registry, three systems would treat it as a landscape).
* `landscapeCoarseSampleOf(tile, layout)` gives one tile's share of the coarse world; `LandscapePackWriter::add(key, blob, coarse)` takes it so a rewrite does not decode 38 000 tiles again.
* `landscapePackSetHlod` already does "write a sibling, rename over".

## Design

### 1. An edit layer in front of the source (*taken*)

`LandscapeEditSource : LandscapeTileSource` wraps the pack source. `load(key)` answers from an in-memory map of edited tiles (`std::map<key, std::shared_ptr<const LandscapeTile>>`, level 0 edited, mips by `buildMips()`) and otherwise from the base. `coarse()` returns an overlay copy of the coarse world (about 20 MB for 100 km) that is patched per edited tile with `landscapeCoarseSampleOf`. `water()` / `hlod()` forward. The pack file is **not touched while editing**.

Why an overlay and not in-place append: the index is fixed-size per tile so an append-only pack cannot gain a second version of a tile without a new index, and an editor crash must never leave the author's world half-written.

### 2. The inverse of the region terrain (*taken*)

`landscapeWriteRegionTerrain(terrain, origin, n, dirtyRect, out tiles)`: from the (edited) region terrain back into level-0 tiles, **only the tiles the dirty rect touches**, with:

* heights quantised back to 16 bit; the region's border samples shared between tiles are written to **every** tile that holds them;
* weights re-interleaved from the three pages;
* water coverage/owner untouched (see "Not covered").

**Region size is 1, 2 or 4 tiles a side.** `TerrainSculpt::ensureHeights` snaps `resolution` to 2^n + 1; 4 x 256 + 1 = 1025 = 2^10 + 1 and 2 x 256 + 1 = 513 fit, 3 x 256 + 1 = 769 does not and would be resampled behind the caller's back. `landscapeRegionTerrain` allows n <= 4 only; the tools must also refuse n = 3.

**Outer border of the region:** its edge samples are shared with tiles outside it. Policy (*taken*): the editing region follows the brush, and a dab whose influence reaches within one sample of the region's outer edge first moves the region (commit, re-assemble around the brush). Then no dab ever writes a shared sample whose other owner is outside the region. Simpler than loading and patching neighbours, and the commit is cheap (touched tiles only).

### 3. Quantisation (*open*)

Options: (a) accept 4.6 cm steps and tell the author (the brush's `amount` below half a step does nothing; the panel shows the step); (b) write a per-pack `heightScale` that fits the real range of the world (a pack from a 400 m island at 3000 m scale wastes 87 % of the range): `heightMinM/heightScaleM` are in the header already but changing them re-quantises every tile. Recommend (a) now, and a "fit height range" batch job later, off the edit path.

### 4. Telling the runtime a tile changed (*taken*, the larger half of the work)

`LandscapeStore::invalidate(key)` (drop the resident entry, bump a per-tile `revision`, start the load again; a tile mid-load is re-ticketed, not trusted) and a `LandscapeRuntime::tileChanged(keys)` that fans out:

| Consumer | What must happen |
|---|---|
| Atlas (GPU) | re-upload the slot of the tile: `invalidate()` already makes it non-resident, so the atlas's own `sync()` releases and later re-takes it, no runtime code needed (step 14, verified). Coarse world: **not** `LandscapeRuntime::m_generation` — every backend's `*Landscape.cpp/.mm` wipes `tileHeights`/`weights`/the slot table to empty on a generation change and only `LandscapeAtlas::sync()` repopulates already-resident tiles' slots, so a generation bump loses every tile the edit did not touch. Step 15 needs its own coarse-only revision and a matching refresh path in each backend that leaves the atlas textures alone |
| `LandscapePhysics` | rebuild the tile's ground body |
| Foliage | the tile's scatter is stale (heights and weights changed): drop and re-scatter that tile |
| Navigation | rebuild that nav tile |
| Water | heights moved under a sheet: rebuild the region's sheets, or refuse edits on tiles with water in the first version |
| HLOD proxies | unaffected (they are cell meshes), except props placed on the old ground; out of scope |

A dab is applied to the **stack region terrain** and shown through it at once; the tiles are committed (quantise, store, invalidate) on stroke end. Per-dab invalidation would stall the atlas.

### 5. Undo (*taken*)

A stroke records the encoded blobs (`encodeLandscapeTile`) of the tiles it touched, before and after. Undo swaps them into the edit layer and invalidates. A 2 km region is at most 16 tiles of about 1 MB.

### 6. Persistence (*open*, recommendation)

* **Sidecar `<pack>.edits`** (recommended): an append-only log of `{key, blob}` records with an index trailer, last record per key wins, read by `LandscapeEditSource` at open. Editing and autosave never rewrite the pack; an editor crash loses at most the last record.
* **Bake edits into the pack** (explicit action, and **at export**): a streaming rewrite through `LandscapePackWriter` (unchanged tiles copied with their stored coarse samples, edited ones from the overlay, water and HLOD sections carried over), to a sibling, then rename. The export already copies the pack and has an `afterCopy` hook (`ExportSettings::dataFiles`), so a shipped game never needs the sidecar: the copy is baked there.
* Rejected: rewriting the pack on every save (a 4 GB write for a dab).

### 7. The editor side (*open*)

The Landscape mode (`TerrainTools.cpp`) is built around a `TerrainComponent` on an entity. The tile landscape has none, and must not (rule above). Two ways: (a) the tools get a "target" abstraction (an entity's terrain, or the tile region's stack terrain) so the brush code stays one path; (b) a second, smaller tile tool set. Recommend (a); it is the largest UI change and the part I cannot estimate without reading `TerrainTools.cpp` (not read for this design).

## Steps, in order

1. `LandscapeStore::invalidate` + `tileChanged` fan-out for one consumer at a time (atlas first, it is visible), tests with `ProceduralLandscapeSource` and a counting source.
2. `landscapeWriteRegionTerrain` (inverse) with a round-trip test: region terrain -> tiles -> region terrain is bit-equal for an untouched region and shared borders agree after a dab.
3. `LandscapeEditSource` (overlay, coarse patch) + test that base tiles are never read for an edited key and the coarse sample matches `landscapeCoarseSampleOf`.
4. Sidecar format + reader/writer + crash test (truncated record ignored).
5. Bake into pack (streaming rewrite) + hook into the export copy.
6. Tool target abstraction in `TerrainTools` (region follows the brush), undo, help entries.
7. Water/foliage/nav fan-out completed; then the visual check on a screen.

## Not covered

Water edited together with the ground (`water::Field` of a migrated terrain is not cut into tiles, the migrated pack has no water table); painting texture-group changes; multi-user editing of one pack (the collaboration layer streams scenes, not packs).

## What I read

`LandscapeSource.h` (pack layout, `LandscapeTileSource`), `LandscapeTile.h` (shared borders, mips), `LandscapeTileTerrain.h` (region assembly, the "never in a registry" rule), `LandscapeStore.h` (no invalidate), `TerrainSculpt.h` (pure, `ensureHeights` snapping), `TerrainTools.h` (header only), `docs/landscape-world-scale-editor-2026-11-03.md` (earlier notes on this item). Not read: `TerrainTools.cpp`, the atlas and physics upload code, `LandscapePackBuild.cpp`.

## Handoff

- **Topic / step:** 326, "Sculpt/paint write-back on tiles" (step 1 of the editor doc's open list).
- **Branch:** `claude/landscape-weltmassstab-100km`; this doc only, no code.
- **Verified:** by reading only (list above). One fact checked against code: n = 3 regions do not fit `ensureHeights`' 2^n + 1 snap.
- **Decisions taken:** overlay source, inverse of the region terrain with region following the brush, commit on stroke end, undo by blobs. **Open for the owner:** height quantisation (a vs b), sidecar vs other persistence, tool target abstraction vs separate tile tools, whether water tiles are editable in the first version.
- **Next steps:** the list above, starting with `LandscapeStore::invalidate`; read `TerrainTools.cpp` (sculpt stroke handling) and the atlas upload before step 6.
