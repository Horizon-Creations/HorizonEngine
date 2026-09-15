#include "MeshImporter.h"
#include <algorithm>
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"
#ifdef HE_HAVE_ASSIMP
#include "AssimpMeshImport.h"   // FBX / OBJ / COLLADA (no Assimp headers leak through it)
#endif

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace
{

void logError(const std::string& msg)
{
	HE_LOG_ERROR(Tool, "%s", ("MeshImporter: " + msg).c_str());
}

// Appends one primitive's geometry to the merged mesh, transformed by `world`,
// and records where its indices landed (`baked`) so the primitives can be
// grouped into material sections once everything is in the buffer.
// The per-vertex work itself is Importer::appendPrimitive — shared with
// SkeletalMeshImporter, which reads the same streams (plus JOINTS_0/WEIGHTS_0).
//
// The UV set is chosen PER PRIMITIVE from its own material, because that is the
// set the material's textures are addressed by — an Unreal bake puts them on
// TEXCOORD_1. A mesh carries one UV stream, but each primitive owns a contiguous
// range of it, so this is exact rather than a compromise.
void appendPrimitive(StaticMeshAsset& mesh, const cgltf_primitive& prim,
                     const glm::mat4& world, float uniformScale,
                     const std::filesystem::path& sourcePath,
                     std::vector<Importer::BakedPrimitive>& baked)
{
	Importer::MeshVertexStreams streams{ mesh.vertices, mesh.normals, mesh.uvs };
	const int wanted = Importer::gltfMaterialUvSet(prim.material);
	const auto indexStart = static_cast<uint32_t>(mesh.indices.size());
	const auto attrs = Importer::appendPrimitive(prim, world, uniformScale, streams,
	                                             mesh.indices, wanted);
	if (!attrs.position)
		return;   // skipped (non-triangles / no POSITION): nothing was appended
	baked.push_back({ indexStart,
	                  static_cast<uint32_t>(mesh.indices.size()) - indexStart,
	                  prim.material });
	if (attrs.uvSet != wanted)
		HE_LOG_WARN(Tool, "%s",
			("MeshImporter: " + sourcePath.filename().string() + ": material '"
			 + (prim.material && prim.material->name ? prim.material->name : "?")
			 + "' samples TEXCOORD_" + std::to_string(wanted)
			 + " but the mesh has no such UV set — fell back to TEXCOORD_"
			 + std::to_string(attrs.uvSet) + ", its textures will be misplaced").c_str());
}

// True if any appended normal is still the (0,0,0) placeholder.
bool hasMissingNormals(const StaticMeshAsset& mesh)
{
	for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3)
		if (mesh.normals[i] == 0.0f && mesh.normals[i+1] == 0.0f && mesh.normals[i+2] == 0.0f)
			return true;
	return false;
}

// Area-weighted per-vertex normals from triangle faces.
void generateNormals(StaticMeshAsset& mesh)
{
	std::fill(mesh.normals.begin(), mesh.normals.end(), 0.0f);
	for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
	{
		const uint32_t ia = mesh.indices[i], ib = mesh.indices[i+1], ic = mesh.indices[i+2];
		const glm::vec3 a = glm::make_vec3(&mesh.vertices[ia*3]);
		const glm::vec3 b = glm::make_vec3(&mesh.vertices[ib*3]);
		const glm::vec3 c = glm::make_vec3(&mesh.vertices[ic*3]);
		const glm::vec3 n = glm::cross(b - a, c - a); // length ∝ face area
		for (uint32_t idx : { ia, ib, ic })
		{
			mesh.normals[idx*3+0] += n.x;
			mesh.normals[idx*3+1] += n.y;
			mesh.normals[idx*3+2] += n.z;
		}
	}
	for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3)
	{
		glm::vec3 n = glm::make_vec3(&mesh.normals[i]);
		if (glm::dot(n, n) > 1e-12f)
		{
			n = glm::normalize(n);
			mesh.normals[i] = n.x; mesh.normals[i+1] = n.y; mesh.normals[i+2] = n.z;
		}
	}
}

// The last stretch every format shares: pin section 0 to the mesh-level
// material, write the asset, report. Slot 0 is spelled out with the mesh-level
// material so the two never disagree — even when that one came from a
// re-import redirect rather than the source file.
std::unique_ptr<StaticMeshAsset> finishImport(std::unique_ptr<StaticMeshAsset> mesh,
                                              const std::filesystem::path&     sourcePath,
                                              const std::filesystem::path&     contentRoot)
{
	if (!mesh->sections.empty() && mesh->sections[0].materialPath.empty())
		mesh->sections[0].materialPath = mesh->materialPath;

	if (!Importer::writeAsset(*mesh, contentRoot, sourcePath))
		return nullptr;

	HE_LOG_INFO(Tool, "%s",
		("MeshImporter: " + sourcePath.filename().string() + " -> " + mesh->path
		 + " (" + std::to_string(mesh->vertices.size() / 3) + " verts, "
		 + std::to_string(mesh->indices.size() / 3) + " tris, "
		 + std::to_string(mesh->sections.size()) + " section"
		 + (mesh->sections.size() == 1 ? "" : "s") + ")").c_str());
	return mesh;
}

#ifdef HE_HAVE_ASSIMP
// FBX / OBJ / COLLADA through Assimp. The geometry arrives in the same streams
// the glTF bake fills, so normals, sections and the write are the shared code
// above; only the reading differs. Materials are NOT imported on this path yet —
// every section keeps an empty path ("the mesh's own material"), and the mesh
// binds the re-import redirect if there is one, so the slots exist for the
// user to fill in the inspector and a later material pass to bind.
std::unique_ptr<StaticMeshAsset> importViaAssimp(
	const std::filesystem::path&        sourcePath,
	const std::filesystem::path&        contentRoot,
	const std::filesystem::path&        relativeOutputDir,
	const MeshImporter::ImportSettings& settings,
	const Importer::OutputTargets&      outputs)
{
	Importer::AssimpScene scene;
	std::string           error;
	if (!scene.load(sourcePath, error))
	{
		logError(sourcePath.string() + ": " + error);
		return nullptr;
	}

	Importer::AssimpBakedGeometry baked;
	if (!scene.bake(settings.uniformScale, baked))
	{
		logError(sourcePath.string() + ": no triangle geometry found");
		return nullptr;
	}
	if (baked.skinned)
		HE_LOG_WARN(Tool, "%s",
			("MeshImporter: " + sourcePath.filename().string()
			 + " carries a skeleton — imported as a StaticMesh in its bind pose "
			   "(skinned import is glTF-only)").c_str());

	const std::string stem = sourcePath.stem().string();
	const auto        out  = Importer::resolveOutput(outputs.asset, relativeOutputDir, stem);

	auto mesh = std::make_unique<StaticMeshAsset>();
	mesh->type     = HE::AssetType::StaticMesh;
	mesh->name     = out.name;
	mesh->path     = out.path;
	mesh->vertices = std::move(baked.positions);
	mesh->normals  = std::move(baked.normals);
	mesh->uvs      = std::move(baked.uvs);
	mesh->indices  = std::move(baked.indices);

	if (settings.generateNormals && baked.missingNormals)
		generateNormals(*mesh);

	// See the glTF path for why an empty MREF must be avoided.
	mesh->materialPath = outputs.material;
	mesh->sections = Importer::buildMeshSections(
		baked.ranges, scene.primaryMaterialIndex(baked), /*materialPaths=*/{}, mesh->indices);

	return finishImport(std::move(mesh), sourcePath, contentRoot);
}
#endif

} // namespace

std::unique_ptr<StaticMeshAsset> MeshImporter::import(
	const std::filesystem::path&   sourcePath,
	const std::filesystem::path&   contentRoot,
	const std::filesystem::path&   relativeOutputDir,
	const ImportSettings&          settings,
	const Importer::OutputTargets& outputs)
{
#ifdef HE_HAVE_ASSIMP
	if (Importer::isAssimpSource(sourcePath))
		return importViaAssimp(sourcePath, contentRoot, relativeOutputDir, settings, outputs);
#endif

	cgltf_options options{};
	cgltf_data*   data = nullptr;

	cgltf_result res = cgltf_parse_file(&options, sourcePath.string().c_str(), &data);
	if (res != cgltf_result_success)
	{
		logError(sourcePath.string() + ": parse failed (cgltf_result "
		         + std::to_string(static_cast<int>(res)) + ")");
		return nullptr;
	}
	res = cgltf_load_buffers(&options, data, sourcePath.string().c_str());
	if (res != cgltf_result_success)
	{
		logError(sourcePath.string() + ": buffer load failed");
		cgltf_free(data);
		return nullptr;
	}

	const std::string stem = sourcePath.stem().string();
	const auto        out  = Importer::resolveOutput(outputs.asset, relativeOutputDir, stem);

	auto mesh = std::make_unique<StaticMeshAsset>();
	mesh->type = HE::AssetType::StaticMesh;
	mesh->name = out.name;
	mesh->path = out.path;

	// Bake every mesh-bearing node with its world transform
	std::vector<Importer::BakedPrimitive> baked;
	for (cgltf_size n = 0; n < data->nodes_count; ++n)
	{
		const cgltf_node& node = data->nodes[n];
		if (!node.mesh)
			continue;
		float m[16];
		cgltf_node_transform_world(&node, m);
		const glm::mat4 world = glm::make_mat4(m);
		for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
			appendPrimitive(*mesh, node.mesh->primitives[p], world, settings.uniformScale, sourcePath, baked);
	}
	// glTFs without a node hierarchy: take the meshes directly
	if (mesh->vertices.empty())
		for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
			for (cgltf_size p = 0; p < data->meshes[mi].primitives_count; ++p)
				appendPrimitive(*mesh, data->meshes[mi].primitives[p],
				                glm::mat4(1.0f), settings.uniformScale, sourcePath, baked);

	if (mesh->vertices.empty())
	{
		logError(sourcePath.string() + ": no triangle geometry found");
		cgltf_free(data);
		return nullptr;
	}

	if (settings.generateNormals && hasMissingNormals(*mesh))
		generateNormals(*mesh);

	// Materials + textures. EVERY glTF material becomes its own asset; each is bound
	// to the section holding the primitives authored with it, and the mesh-level
	// reference (MREF) is section 0's. The stem passed here is only a fallback for
	// UNNAMED materials and embedded images — and it stays derived from the SOURCE,
	// never from the mesh's (possibly re-imported and renamed) own name, so ordinary
	// imports keep writing exactly the file names the asset compiler's up-to-date
	// probe already expects.
	Importer::GltfMaterialImport materials;
	if (settings.importMaterials)
		materials = Importer::importGltfMaterials(
			data, sourcePath, contentRoot, relativeOutputDir, stem, outputs);
	mesh->materialPath = materials.primary;
	// No material resolved — importMaterials is off, or the glTF declares none.
	// saveAsset writes chunk MREF unconditionally
	// from this freshly built asset, so leaving the field empty BLANKS the reference
	// every scene using the mesh resolves its material through; meshSidecarAssets
	// would afterwards return nothing, so not even the next re-import could find the
	// sidecar again and the link would be gone for good. outputs.material is that
	// very reference, read back off the mesh by reimport() before it ran.
	if (mesh->materialPath.empty())
		mesh->materialPath = outputs.material;

	// Sections: one per material, the index buffer regrouped to match.
	mesh->sections = Importer::buildMeshSections(
		data, baked, Importer::gltfPrimaryMaterial(data), materials.paths, mesh->indices);

	cgltf_free(data);

	return finishImport(std::move(mesh), sourcePath, contentRoot);
}
