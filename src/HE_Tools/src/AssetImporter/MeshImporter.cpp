#include "MeshImporter.h"
#include <algorithm>
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

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

// Appends one primitive's geometry to the merged mesh, transformed by `world`.
// The per-vertex work itself is Importer::appendPrimitive — shared with
// SkeletalMeshImporter, which reads the same streams (plus JOINTS_0/WEIGHTS_0).
void appendPrimitive(StaticMeshAsset& mesh, const cgltf_primitive& prim,
                     const glm::mat4& world, float uniformScale)
{
	Importer::MeshVertexStreams streams{ mesh.vertices, mesh.normals, mesh.uvs };
	Importer::appendPrimitive(prim, world, uniformScale, streams, mesh.indices);
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

} // namespace

std::unique_ptr<StaticMeshAsset> MeshImporter::import(
	const std::filesystem::path&   sourcePath,
	const std::filesystem::path&   contentRoot,
	const std::filesystem::path&   relativeOutputDir,
	const ImportSettings&          settings,
	const Importer::OutputTargets& outputs)
{
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
	for (cgltf_size n = 0; n < data->nodes_count; ++n)
	{
		const cgltf_node& node = data->nodes[n];
		if (!node.mesh)
			continue;
		float m[16];
		cgltf_node_transform_world(&node, m);
		const glm::mat4 world = glm::make_mat4(m);
		for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
			appendPrimitive(*mesh, node.mesh->primitives[p], world, settings.uniformScale);
	}
	// glTFs without a node hierarchy: take the meshes directly
	if (mesh->vertices.empty())
		for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
			for (cgltf_size p = 0; p < data->meshes[mi].primitives_count; ++p)
				appendPrimitive(*mesh, data->meshes[mi].primitives[p],
				                glm::mat4(1.0f), settings.uniformScale);

	if (mesh->vertices.empty())
	{
		logError(sourcePath.string() + ": no triangle geometry found");
		cgltf_free(data);
		return nullptr;
	}

	if (settings.generateNormals && hasMissingNormals(*mesh))
		generateNormals(*mesh);

	// Material + base color texture. The names the two sidecars fall back to stay
	// derived from the SOURCE stem, never from the mesh's (possibly re-imported and
	// renamed) own name — a re-import redirects them through outputs.material /
	// outputs.texture instead, so that ordinary imports keep writing exactly the
	// file names the asset compiler's up-to-date probe already expects.
	if (settings.importMaterials)
		mesh->materialPath = Importer::importBaseColorMaterial(
			data, sourcePath, contentRoot, relativeOutputDir, stem, outputs);
	// No material resolved — importMaterials is off, or the glTF's base-colour image
	// is no longer on disk next to it. saveAsset writes chunk MREF unconditionally
	// from this freshly built asset, so leaving the field empty BLANKS the reference
	// every scene using the mesh resolves its material through; meshSidecarAssets
	// would afterwards return nothing, so not even the next re-import could find the
	// sidecar again and the link would be gone for good. outputs.material is that
	// very reference, read back off the mesh by reimport() before it ran.
	if (mesh->materialPath.empty())
		mesh->materialPath = outputs.material;

	cgltf_free(data);

	if (!Importer::writeAsset(*mesh, contentRoot, sourcePath))
		return nullptr;

	HE_LOG_INFO(Tool, "%s",
		("MeshImporter: " + sourcePath.filename().string() + " -> " + mesh->path
		 + " (" + std::to_string(mesh->vertices.size() / 3) + " verts, "
		 + std::to_string(mesh->indices.size() / 3) + " tris)").c_str());
	return mesh;
}
