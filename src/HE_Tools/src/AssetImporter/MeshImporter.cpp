#include "MeshImporter.h"
#include "TextureImporter.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>

namespace
{

void logError(const std::string& msg)
{
	Logger::Log(Logger::LogLevel::Error, ("MeshImporter: " + msg).c_str());
}

// Appends one primitive's geometry to the merged mesh, transformed by `world`.
void appendPrimitive(StaticMeshAsset& mesh, const cgltf_primitive& prim,
                     const glm::mat4& world, float uniformScale)
{
	if (prim.type != cgltf_primitive_type_triangles)
		return;

	const cgltf_accessor* posAcc  = nullptr;
	const cgltf_accessor* normAcc = nullptr;
	const cgltf_accessor* uvAcc   = nullptr;
	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const cgltf_attribute& attr = prim.attributes[i];
		if      (attr.type == cgltf_attribute_type_position)                      posAcc  = attr.data;
		else if (attr.type == cgltf_attribute_type_normal)                        normAcc = attr.data;
		else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)   uvAcc   = attr.data;
	}
	if (!posAcc)
		return;

	const uint32_t  baseVertex = static_cast<uint32_t>(mesh.vertices.size() / 3);
	const glm::mat3 normalMat  = glm::transpose(glm::inverse(glm::mat3(world)));

	for (cgltf_size v = 0; v < posAcc->count; ++v)
	{
		float p[3] = {};
		cgltf_accessor_read_float(posAcc, v, p, 3);
		glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f)) * uniformScale;
		mesh.vertices.insert(mesh.vertices.end(), { wp.x, wp.y, wp.z });

		if (normAcc && v < normAcc->count)
		{
			float n[3] = {};
			cgltf_accessor_read_float(normAcc, v, n, 3);
			glm::vec3 wn = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
			mesh.normals.insert(mesh.normals.end(), { wn.x, wn.y, wn.z });
		}
		else
			mesh.normals.insert(mesh.normals.end(), { 0.0f, 0.0f, 0.0f });

		if (uvAcc && v < uvAcc->count)
		{
			float uv[2] = {};
			cgltf_accessor_read_float(uvAcc, v, uv, 2);
			mesh.uvs.insert(mesh.uvs.end(), { uv[0], uv[1] });
		}
		else
			mesh.uvs.insert(mesh.uvs.end(), { 0.0f, 0.0f });
	}

	if (prim.indices)
	{
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			mesh.indices.push_back(baseVertex
				+ static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
	}
	else
	{
		for (cgltf_size i = 0; i < posAcc->count; ++i)
			mesh.indices.push_back(baseVertex + static_cast<uint32_t>(i));
	}
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

// Finds the first base-color texture in the glTF and imports it. Returns the
// asset path of the written texture, or empty.
std::string importBaseColorTexture(const cgltf_data* data,
                                   const std::filesystem::path& sourcePath,
                                   const std::filesystem::path& contentRoot,
                                   const std::filesystem::path& relativeOutputDir,
                                   const std::string& meshStem)
{
	const cgltf_texture* texture = nullptr;
	for (cgltf_size m = 0; m < data->materials_count && !texture; ++m)
	{
		const cgltf_material& mat = data->materials[m];
		if (mat.has_pbr_metallic_roughness && mat.pbr_metallic_roughness.base_color_texture.texture)
			texture = mat.pbr_metallic_roughness.base_color_texture.texture;
	}
	if (!texture || !texture->image)
		return {};

	const cgltf_image* img = texture->image;
	const std::string texName = meshStem + "_basecolor";

	if (img->uri && std::strncmp(img->uri, "data:", 5) != 0)
	{
		// External file referenced relative to the glTF
		char decoded[1024];
		std::strncpy(decoded, img->uri, sizeof(decoded) - 1);
		decoded[sizeof(decoded) - 1] = '\0';
		cgltf_decode_uri(decoded);
		const std::filesystem::path texFile = sourcePath.parent_path() / decoded;
		auto tex = TextureImporter::import(texFile, contentRoot, relativeOutputDir);
		return tex ? tex->path : std::string{};
	}

	if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data)
	{
		// Embedded in the binary buffer (.glb or data URI)
		const auto* bytes = static_cast<const uint8_t*>(img->buffer_view->buffer->data)
		                  + img->buffer_view->offset;
		auto tex = TextureImporter::decodeFromMemory(bytes, img->buffer_view->size);
		if (!tex)
			return {};
		tex->type = HE::AssetType::Texture;
		tex->name = texName;
		tex->path = Importer::toAssetPath(relativeOutputDir / (texName + ".hasset"));
		if (!Importer::writeAsset(*tex, contentRoot))
			return {};
		return tex->path;
	}
	return {};
}

} // namespace

std::unique_ptr<StaticMeshAsset> MeshImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir,
	const ImportSettings&        settings)
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

	auto mesh = std::make_unique<StaticMeshAsset>();
	mesh->type = HE::AssetType::StaticMesh;
	mesh->name = sourcePath.stem().string();
	mesh->path = Importer::toAssetPath(relativeOutputDir / (mesh->name + ".hasset"));

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

	// Material + base color texture
	if (settings.importMaterials)
	{
		const std::string texPath = importBaseColorTexture(
			data, sourcePath, contentRoot, relativeOutputDir, mesh->name);
		if (!texPath.empty())
		{
			MaterialAsset mat;
			mat.type       = HE::AssetType::Material;
			mat.name       = mesh->name + "_mat";
			mat.path       = Importer::toAssetPath(relativeOutputDir / (mat.name + ".hasset"));
			mat.shaderPath = "builtin/unlit";
			mat.texturePaths.push_back(texPath);
			if (Importer::writeAsset(mat, contentRoot))
				mesh->materialPath = mat.path;
		}
	}

	cgltf_free(data);

	if (!Importer::writeAsset(*mesh, contentRoot))
		return nullptr;

	Logger::Log(Logger::LogLevel::Info,
		("MeshImporter: " + sourcePath.filename().string() + " -> " + mesh->path
		 + " (" + std::to_string(mesh->vertices.size() / 3) + " verts, "
		 + std::to_string(mesh->indices.size() / 3) + " tris)").c_str());
	return mesh;
}
