# Mesh LODs inside the mesh asset (2026-11-25)

Before this change, Generate LODs wrote each level as its own asset, `<stem>_LOD<n>.hasset`, next to the mesh. Every level showed up in the Content Browser, and a level was only used where someone had wired it by hand (a LODComponent, a foliage layer).

Now the levels live inside the mesh's own `.hasset`. The Static Mesh tab switches between them. In the game, every entity that shows the mesh swaps the level by distance on its own.

## Handoff

**Format.** `HAsset.h` (`CHUNK_MLDI`, `CHUNK_MLOD`) and `StaticMeshAsset::lods`/`lodBlobs` (`Assets.h`).

- **MLDI** is written right after META. It holds the level index: per level the id, `worldError` and the triangle count.
- **MLOD** holds each level as a complete static-mesh `.hasset` with its own UUID.
  - The section materials inside it are left empty.
  - A level draws with the base mesh's slots, matched by index.
- Both chunks are optional tail chunks, so there is no version bump.
- Codecs: `HE::encode/decodeMeshLodIndex` and `HE::encode/decodeMeshLodBlobs`. `HE::encodeAssetBytes` is in `AssetFile.h`.

**Loading** (`ContentManager.cpp`).

- **Shared parse:** `readStaticMeshChunks` is the static-mesh parse, used by the base mesh and by every level.
- **Registration:** `registerMeshLods` registers each level under its own UUID, copying the base's slot materials onto it.
  - It runs on load and at the end of `saveAsset`, so a slot edit reaches every level.
- **Reload after a rewrite:** `refreshMeshLods` re-reads the levels after `AutoLod::generate` has rewritten the file.
- **Lookup and lifetime:**
  - `lodBaseOf` gives the mesh a level belongs to.
  - `unloadAsset(base)` also unloads its levels.
  - `scanDirInto` reads MLDI, so `ensureResident(level)` finds the base file.

**Generator.** `AutoLod.cpp`.

- `generate` rewrites the mesh file with the levels inside it and keeps each level's id.
  - The ids come from MLDI, or else from old sidecars, which are then merged in and deleted.
- Old API names:
  - `removeStaleSidecars` is now `removeStaleLevels` and trims the embedded levels too.
  - `levelsInFile` and `levelCount` are new.
- `Importer::writeAsset` carries a mesh's existing levels over when it writes a mesh that has none. A re-import then keeps the level ids.

**Pak.**

- `HpakWriter.cpp` `splitMeshLods` turns each level into a pak entry of its own, with the base's MSEC materials.
- The base mesh keeps MLDI. The reach filter follows its raw UUIDs, so a level is packed only when its mesh is reached.

**Runtime.**

- `LODSystem::bindAssetLevels`, called from `SceneSystems::tickWorld` before `update`, gives every MeshComponent with asset levels a `LODComponent{fromAsset}`.
  - It is gated on the content epoch and the mesh count, and otherwise runs every 30 ticks.
  - In a pak it requests missing levels asynchronously and binds once they are loaded.
- `update` drops a fromAsset component when the entity's mesh was changed to something else.
- `SceneSerializer` does not save fromAsset components. It saves `levels[0]` whenever the current mesh id is one of the levels.

**Editor.**

- `StaticMeshEditorPanel` has a toolbar group LOD 0..n, a "Detail levels" section with Generate LODs..., and "Merge Level Files" for old sidecars.
- `AutoLodDialog` maps a level id to its mesh.
- Help entries and setting texts are updated.

**Verified.**

- Full `ctest` 387/387.
- `editor_help_audit --check` passes.
- New tests in `test_auto_lod.cpp`:
  - levels embedded, ids stable;
  - a fresh ContentManager resolves a level on its own;
  - slot edits reach the levels;
  - sidecars merged with their ids kept;
  - `removeStaleLevels`;
  - pak split;
  - reach filter;
  - entity binding without anything in the scene.
- New test `test_static_mesh_editor_ui` "LOD switch", with a screenshot looked at.

**Not verified.**

- A real export through the ProjectExporter.
- The live editor with a 3D render on real hardware.
- Windows CI.
- How the cooked statics behave with fromAsset components at runtime. The code watches LODComponent, but no test covers it.
- Skeletal meshes and the engine's `RockALod*` are unchanged.

**Decisions.**

- The levels keep their own UUIDs, so scenes, foliage and the five backends stay unchanged.
- In the editor, `lodBlobs` stays in memory next to the registered levels, so level geometry is held twice in RAM. A pak does not have this, because there the levels are separate entries.
- Merging old sidecars rebuilds the levels from LOD0 with `ratiosFor(n, 0.5, 0.1)`. The foliage flag is lost, the same as on reimport.

**Next steps.**

1. A real export with a mesh that has levels: `HE_HIDDEN_WINDOW`, then mount the pak and check the level entries.
2. Optionally a "Merge Level Files" button in the Content Browser for a whole folder.
3. Optionally per-level distances or a screen tolerance on the asset. Today the default 1 px tolerance is scaled by the entity's size.
