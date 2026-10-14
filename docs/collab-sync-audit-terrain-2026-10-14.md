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

*Filled in at the end of the step (section 4 and Handoff).*
