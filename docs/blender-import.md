# Blender (.blend) import through Assimp (2026-10-09)

`.blend` is a mesh source like .fbx/.obj/.dae: Import Asset, drag & drop, the Content
Browser's Import item and the asset compiler all route it to `MeshImporter` →
`AssimpMeshImport` (`isAssimpSource`). Only Assimp's *Blender* importer is added to the
static build (`ASSIMP_BUILD_BLEND_IMPORTER`, root `CMakeLists.txt`).

## What Assimp's reader can and cannot do
* **Versions: Blender 2.4 up to 3.4.** 3.5 moved mesh vertices out of `Mesh.mvert` into
  generic attributes; Assimp requires `mvert` and fails with a "field not found" text.
  Files from 3.5 / 4.x / 5.x therefore do not import through this path.
  `Importer::blendReadHint` reads the file header and appends the Blender version and the
  way out ("File > Export > glTF 2.0 (.glb), then import that file") to the error.
* **Compression:** gzip (Blender 2.x/3.x "Compress") is inflated; **zstd** (3.0+) is not -
  the hint says to save with Compress off.
* **Content:** mesh objects with their materials/UVs/normals, node transforms, plus the
  Mirror and Subdivision modifiers. Other modifiers, curves, text, metaballs and
  geometry nodes are not geometry to it; armatures are not read (static mesh only).
* **Axes:** Blender is Z up / -Y forward and Assimp leaves it so; `AssimpScene::bake`
  rotates (x, y, z) -> (x, z, -y), the rotation Blender's own glTF exporter applies.
* **Units:** Blender's `unit_settings.scale_length` is not read; 1 unit = 1 m.

## Tests
`tests/test_assimpimport.cpp`: routing, `blendReadHint` (versions, new header, zstd,
gzip, not-blender), the refused-file error, and - against Assimp's sample models, found
through `HE_ASSIMP_TEST_MODELS` (set from the fetched Assimp source tree, nothing of
ours is committed) - the Z-up plane, plain + gzip cubes, and a `MeshImporter` import.
