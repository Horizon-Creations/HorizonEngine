# Collab sync audit, part 2: landscape (2026-10-14)

Topic 198, step 3. What a live collab session replicates when somebody **models a
landscape** (sculpt, Mountain, heightmap import, layer paint, foliage paint, water,
the Landscape panel's settings, the MCP `terrain_*` tools), what it did not, and what
this step changed. Inventory verified by reading the code on the branch @ fa7203d9.

## 1. Where the landscape lives

Everything a landscape tool writes is **inline in two components of one entity**:

| Data | Field | Size (worst case) | Written by |
|---|---|---|---|
| Heights | `TerrainComponent::sculptHeights` (res², float) | 1025² × 4 = 4.2 MB | sculpt brush, Flatten/Ramp/Smooth…, Mountain, heightmap import, water dig, lake tool, MCP `terrain_sculpt/mountain/heightmap/lake` |
| Layer weights | `layerWeights`, `layerWeights2` (weightRes² × 4 B) | 2 × 1 MB at 512 | paint brush, Fill Layer, MCP `terrain_paint` |
| Foliage mask | `FoliageComponent::densityMask` (maskRes² B) | 16 KB at 128 | foliage brush |
| Water | `TerrainComponent::water` (cells RLE + bodies) | small (RLE) | water brush, lake tool, panel water settings |
| Settings | the rest of both components (size, resolution, noise, tessellation, foliage mesh/density…) | small | Landscape panel, Details panel |

The tools edit whichever entity carries the first `TerrainComponent`
(`TerrainTools.cpp:351-356`); Landscape mode never selects it (only creating one
does, `:1263`).

## 2. What reached peers before this step

| Operation | Status | Why / where |
|---|---|---|
| Sculpt / paint / foliage / water stroke, Mountain, heightmap import, Fill Layer, panel settings | **missing** (unless the terrain happened to be the locked primary selection) | the only per-frame path is the primary selection's component blob (`EditorApplication.cpp:4666-4747`). Landscape mode does not select the terrain, so nothing went out. |
| Same, with the terrain selected and locked | **synced, badly** | the whole entity is serialized every frame (base64 of heights + weightmaps, up to ~8 MB of CBOR, then FNV-hashed), and sent every frame it changes. The receiver `replaceEntityComponents` builds a fresh `TerrainComponent`: `dirty = true`, `builtRes = 0`, so **every chunk is rebuilt and re-uploaded per received frame**. |
| MCP `terrain_*` tools | **synced, badly** | `EditorCommands` `SetComponents` with the lock (`McpToolsTerrain.cpp:200-226`): whole entity incl. bulk; same full rebuild on the receiver. |
| Session undo of a stroke | **missing** | strokes take `m_undo` snapshots (`TerrainTools.cpp:716, 746, 778, 830, 854`), which a session withholds (step 2); `CollabUndo` has no landscape entry. |
| Late joiner | **synced** | host snapshot carries both components whole. |

Two traps behind "synced, badly": (a) a peer that holds the terrain selected re-sends
its whole blob whenever *anything* changes it, so a whole-entity blob can arrive after
newer region edits and revert them; (b) the cost is per frame, not per change.

## 3. What this step does

**Landscape grids travel as region patches, detected by a diff, ordered by the host.**

| Piece | Where | What |
|---|---|---|
| Wire | `CollabSession.h` "Landscape patches", `CollabSession.cpp` `sendTerrainPatch` / `handleTerrainUpdate` / `handleTerrainRelay`, ids +39/+40 | `TerrainPatch {netId, seq, field, res, x0 z0 w h, bytes}`. **No lock.** The host applies, then relays to everyone **including the sender** (its ack). Protocol **v14** (equality-checked at join). |
| Change detection | `TerrainSync.cpp` `collect` / `collectGrid` | per landscape a mirror of what the peers have; every ≥33 ms the bounding rect of what differs goes out raw (heights float, weights RGBA8 ×2 pages, foliage mask). Tool-agnostic: brush, Mountain, heightmap import, Fill Layer, MCP `terrain_*`, water dig, undo all replicate. Bands ≤1 MB; pacing `max(33 ms, bytes/4096 per ms)` (~4 MB/s per grid). |
| Size changes | `kWholeGrid` flag | a grid that appears/vanishes/resizes is sent whole, first band flagged; only that band may reallocate on the receiver. A plain patch for a grid of another size is dropped (a stroke from before a resize cannot wipe the new grid). `res 0` = cleared. |
| Settings | `Field::Settings`, `settingsBlob` | both components **without grids** (water cells RLE + bodies included), hash-checked every 200 ms. The receiver keeps its grids and its built chunks; `dirty` only if the shape changed (`shapeDiffers`); foliage keeps its scatter unless a scatter input changed. |
| Concurrency | `applyRemote` / `acknowledge` | a client keeps its sent-but-unechoed patches and re-applies them over every peer patch; since the host relays in order over one stream, every machine ends in the host's order. Unsent local edits (≤33 ms) under a peer's patch lose to it. |
| Component blob | `serializeWithoutGrids`, `replaceKeepingGrids`; `EditorApplication.cpp` per-frame selection publish, `h.publishComponents`, CollabUndo components handler, `onRemoteComponents` | grids set aside (swap, no copy) while serializing; the receiver keeps its grids/runtime. Fixes the 8 MB/frame blob and the "stale whole blob reverts newer patches" race. `rememberComponents` stops a holder from echoing a peer's settings. |
| Editor wiring | `EditorApplication.cpp` after `updateAssetCollabSync`; `onRemoteTerrainPatch`; `onWorldReplaced` | collect every frame in a session; reset on entering/leaving a session and on snapshot. |

Tests: `tests/test_terrain_sync.cpp` (8 cases: rect size, first stroke whole, paint + page 2 + foliage mask, three-way concurrent sculpt converges, settings keep grids/chunks, malformed/mismatched patches change nothing, pacing loses nothing, grid-free blob both ways) and two wire cases at the end of `tests/test_net_collab.cpp` (echo to sender, no lock needed, empty clear patch, oversize/no-id refused).

## 4. Not done here (decisions and limits)

* **Session undo of a stroke:** still none. Strokes push to `m_undo`, which a session withholds; `CollabUndo` has no landscape entry, so Ctrl+Z in a session pops whatever entry is on top (an unrelated transform). Proposal for step 6: TerrainSync knows before (mirror) and after (live) of every rect it sends; keep an "open stroke" per landscape (union rect, mirror crop at its first change), close it after ~400 ms without a local change and record `before/after` crops as one `CollabUndo` entry whose apply writes the crop locally (the diff then publishes it).
* **Removing the FoliageComponent / TerrainComponent** of a landscape replicates only through the selection blob (the settings patch updates, it never removes).
* **Viewport picking AABB** of a remotely sculpted landscape is not invalidated (the local brush does it through the viewport's cache); the same gap exists for every remote component edit.
* **Receiver re-derivations** (TerrainSystem's 2ⁿ+1 resample, a lazy `ensureHeights`) can look like a local edit to the receiver's own diff and be sent once more. It converges (same data, last in host order), it only costs one grid of bandwidth.
* Not run in a live two-editor session.
