# One landscape system: the tile world replaces the single landscape (plan, 2026-11-21)

Owner's direction (21.11.): keep the streamed tile landscape as the ONLY landscape. Who does not want streaming switches it off, which
means "everything is loaded and kept", with the coarse world and the far image still there. Then the old system and its separate
editor mode go, and the editor never has to ask which kind of landscape a world has.

Nothing here is built. Counts are from `grep` on the branch `claude/landscape-weltmassstab-100km` at `37b80dc8a`.

## What "the old system" is, and what stays

`TerrainComponent` is named in 62 files of HE_Scene, 17 of HE_Editor, 9 of HE_Rendering/HE_Core and 43 test files. Two different things
hide behind that name:

* **A landscape entity in a scene** and everything that treats it as one: `TerrainSystem.cpp` (1445 lines: chunk meshes and their LODs),
  the one Jolt height field (`PhysicsWorld.cpp`), the GI landscape table (`GiLandscape.h`, `RenderExtractor.cpp`), the RVT driver
  (`RvtLandscape.cpp`), the chunk far material (`LandscapeHlod*.cpp`), the inline heights and weights in the scene file
  (`SceneSerializer.cpp`), collaboration (`TerrainSync.*`), and the Landscape mode (`TerrainTools.cpp`, 2871 lines). **This goes.**
* **A working format on the stack**: the brushes (`TerrainSculpt`, `TerrainPaint`), water (`Water*.cpp`), foliage scatter and the
  generators take a `TerrainComponent&`, and the tile world already hands them one made from tiles (`LandscapeTileTerrain.h`). **This
  stays**; it only stops being a component (a rename at the very end, if at all).

## What the tile world cannot do yet (the blockers, largest first)

| # | Gap | Today |
|---|---|---|
| 1 | The landscape **material** | `kLandscapeFS` is a fixed layer blend. The Auto Landscape graph, RVT, snow / sand / weather / trails only run on the old system |
| 2 | **Tools** | Tiles have Sculpt and Paint Layer. Foliage painting, the water brush and lakes, Mountain, Ramp, heightmap import onto an existing world: old system only |
| 3 | **GI** | Sun shadows on GL / Vulkan / D3D11 only; reflections an OpenGL prototype |
| 4 | **Migration** | `Migrate Terrain to Tiles` exists but drops the terrain's water, and nothing converts a scene when it is loaded |
| 5 | **"Streaming off"** | No switch. And a limit: every resident tile is a layer of the atlas's GPU arrays, so "keep everything" fits worlds up to roughly 20 km a side (estimated from the 2048-layer limit, not measured) |
| 6 | Script API, MCP tools, tutorial, outliner, viewport picking, spline tool | all address a `TerrainComponent` entity |

## Steps, in order

Each ends with its own doc and handoff. "Catania" = the 2 km reference scene of the earlier audits: every step that draws compares
pictures and frame time against the old system on it, because a small landscape must not get slower.

1. **Baseline** (no code): Catania on the old system, per backend that is at hand: frame time, landscape GPU pass, memory, five fixed
   views as pictures. The number every later step is held against.
2. **"Streaming off"**: `LandscapeWorldComponent::keepAll`. Load radius = the world, no byte cap, atlas sized from the tile count; a
   world too large for the atlas is refused with a sentence that says so. Small; no backend code.
3. **The material in the tile world** (gap 1; Thema 335 step 10; needs PR 233). The tile shader draws the landscape material near the
   camera and the far image far away; `landscapeFarWeightPainter` is replaced by a painter that photographs the material. The largest
   step and the one everything else waits for. Metal first, then the others.
4. **GI parity** (gap 3): shadows on Metal and D3D12, reflections on all five.
5. **One Landscape mode** (gap 2): the brush code gets a target (the region terrain of `LandscapeEditSession`) instead of an entity's
   component; foliage masks and water rasters are written back to tiles like heights and weights; strokes become steps of the
   editor's own undo. `LandscapeWorldTools`' separate brush and its "Edit Tiles" switch go.
6. **Migration on load** (gap 4): a scene with a `TerrainComponent` becomes a pack plus a `LandscapeWorldComponent` when it is opened,
   water, foliage masks and material overrides included. Lossless rule: a test that loads the old form stays forever.
7. **Everything that names the entity** (gap 6): script API, MCP tools, tutorial, outliner, picking, spline tool.
8. **Remove**: `TerrainSystem`'s chunk meshes, the single height field body, the GI landscape table's terrain entries, the RVT
   terrain driver, the chunk far material, `TerrainSync`, the old Landscape mode. The 43 test files are rewritten or deleted with
   what they test. Catania against the baseline of step 1 is the acceptance.

Steps 2, 4 and 6 do not wait for each other. Step 3 blocks 5 (the tools paint what the material shows) and 8.

## Risks

* **Small landscapes get slower.** The quadtree and the atlas are built for 100 km; a 2 km landscape is 16 tiles. Step 1 exists so this
  is seen at step 3, not at step 8.
* **Step 3 is verifiable on Metal only on the Mac.** D3D and Vulkan parity is Windows work, like the far image today.
* **Scenes in the wild**: step 6 runs on every existing project's scenes. A failed migration must leave the scene as it was.
* **Collaboration** streams scenes, not packs: a pack edited by two people has no answer yet (`TerrainSync` does that for terrains).

## Decisions the owner has to take first

1. PR 233 (RVT 2.0): merge it into `release/0.8.0`, or let the landscape branch take the RVT branch in? Step 3 needs its code.
2. Is this one topic for 0.8.0, or does the removal (step 8) move to 0.9.0? Steps 2 to 7 are useful without it.
3. Tile size for small worlds: stay at 512 m (a 300 m island is one tile, mostly empty), or allow smaller tiles?

## Handoff

- **Topic / step:** none yet. Proposed as its own Hive topic that builds on 326 and 335; not opened, so that no bee starts before
  the three decisions above are taken.
- **Done:** this plan. **Verified:** the file counts and line counts (grep, wc). **Not verified:** the 20 km limit of "keep everything"
  (an estimate), every effort implied by the order.
- **Next step:** the owner's three decisions, then steps 1 and 2.
