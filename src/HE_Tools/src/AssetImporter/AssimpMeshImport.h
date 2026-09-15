#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "ImporterCommon.h"   // Importer::BakedRange

// Assimp lives in the build tree's _deps and is a PRIVATE dependency of
// HorizonImporters — nothing that includes this header (MeshImporter, the
// editor, the tests) sees an Assimp header, so the scene type is forward-declared.
struct aiScene;

// The FBX / OBJ / COLLADA half of MeshImporter: everything glTF does natively
// through cgltf comes in here through Assimp, converted into the same
// StaticMeshAsset streams (SoA positions/normals/UVs + indices + BakedRange
// table) so the rest of the import — normals, sections, sidecars, the write —
// is shared with the glTF path rather than duplicated.
//
// Only compiled when HE_HAVE_ASSIMP is defined (root CMakeLists, HE_ENABLE_ASSIMP).
namespace Importer
{
	// True for the extensions this file reads: .fbx, .obj, .dae (any case).
	// The routing in ImporterCommon (classifySource) and the asset compiler ask
	// this rather than keeping their own copy of the list.
	bool isAssimpSource(const std::filesystem::path& sourcePath);

	// One scene's geometry, baked the way MeshImporter bakes glTF: every mesh
	// instance of every node with the node's world transform applied, vertices
	// appended contiguously, indices rebased onto them.
	struct AssimpBakedGeometry
	{
		std::vector<float>      positions;   // xyz per vertex
		std::vector<float>      normals;     // xyz per vertex, (0,0,0) where the source has none
		std::vector<float>      uvs;         // uv per vertex, (0,0) where the source has none
		std::vector<uint32_t>   indices;     // triangles only
		// One entry per baked aiMesh instance, in bake order, with the mesh's
		// aiScene::mMaterials index — what buildMeshSections groups by.
		std::vector<BakedRange> ranges;
		// Some mesh carried bones. It is imported anyway, as its bind pose, since
		// this is the STATIC path; the caller logs it so a rigged FBX does not
		// silently turn into a statue.
		bool                    skinned        = false;
		// Some vertex had no source normal (its slot is (0,0,0)); the caller
		// generates them, as it does for glTF.
		bool                    missingNormals = false;
	};

	// A loaded Assimp scene. Kept as an object rather than a one-shot function so
	// the material pass (a later step) can read aiScene::mMaterials off the same
	// scene the geometry was baked from — the material INDICES in `ranges` are
	// only meaningful against that table.
	class AssimpScene
	{
	public:
		AssimpScene();
		~AssimpScene();
		AssimpScene(const AssimpScene&)            = delete;
		AssimpScene& operator=(const AssimpScene&) = delete;

		// Reads `sourcePath` with the post-processing the engine's mesh format
		// needs (triangulation, identical-vertex merge, point/line removal). False
		// with Assimp's reason in `error`.
		bool load(const std::filesystem::path& sourcePath, std::string& error);

		// Null until load() succeeded.
		const aiScene* scene() const;

		// Bakes the node tree into `out` (see AssimpBakedGeometry), positions
		// scaled by `uniformScale` — on top of the format's own unit correction:
		// FBX geometry arrives in the file's units and is brought to metres via
		// its UnitScaleFactor (centimetres per unit; the .cpp explains why Assimp
		// does not do that itself). COLLADA's <unit> and <up_axis> are already in
		// Assimp's root transform; OBJ has no units.
		// Meshes no node references are baked untransformed as a fallback, as the
		// glTF path does for files without a node hierarchy.
		// False when nothing triangular came out.
		bool bake(float uniformScale, AssimpBakedGeometry& out) const;

		// The material a mesh binds at mesh level (chunk MREF) — that of the first
		// baked range, which is section 0's, exactly the glTF rule. The first
		// declared material when nothing was baked; BakedRange::kNoMaterial when
		// the scene declares none.
		int primaryMaterialIndex(const AssimpBakedGeometry& baked) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
		bool                  fbx_ = false;   // source was FBX: bake() applies the cm → m factor
	};
}
