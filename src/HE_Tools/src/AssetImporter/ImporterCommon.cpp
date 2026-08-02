#include "ImporterCommon.h"
#include <cstdint>
#include <cstring>
#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
#include "TextureImporter.h"

// Declarations only — CGLTF_IMPLEMENTATION is defined exactly once, in
// MeshImporter.cpp, for the whole HorizonImporters link unit.
#include "cgltf.h"

#include <glm/glm.hpp>

namespace Importer
{

std::string toAssetPath(const std::filesystem::path& relativePath)
{
	std::string s = relativePath.generic_string();
	// Strip a leading "./" that std::filesystem::relative sometimes produces
	if (s.rfind("./", 0) == 0)
		s.erase(0, 2);
	return s;
}

bool gltfHasSkin(const std::filesystem::path& sourcePath)
{
	cgltf_options options{};
	cgltf_data*   data = nullptr;
	// cgltf_parse_file reads the JSON only — skins_count is filled in without
	// touching the (potentially large) .bin buffers or embedded images.
	if (cgltf_parse_file(&options, sourcePath.string().c_str(), &data) != cgltf_result_success)
		return false;
	const bool skinned = data->skins_count > 0;
	cgltf_free(data);
	return skinned;
}

static HE::UUID existingUUID(const std::filesystem::path& file)
{
	HAsset::Reader reader;
	if (!reader.open(file.string()))
		return HE::UUID{};

	const auto* meta = reader.findChunk(HAsset::CHUNK_META);
	if (!meta || reader.header().version < 2)
		return HE::UUID{};

	HE::UUID id;
	size_t   off = sizeof(uint16_t); // skip asset type
	if (!HAsset::Reader::readPOD(meta->data, off, id.hi)) return HE::UUID{};
	if (!HAsset::Reader::readPOD(meta->data, off, id.lo)) return HE::UUID{};
	return id;
}

bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot)
{
	const std::filesystem::path target = contentRoot / asset.path;

	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);

	// Re-import: keep the identity the asset already has on disk.
	if (asset.id == HE::UUID{})
		asset.id = existingUUID(target);

	ContentManager cm(contentRoot.string());
	if (!cm.saveAsset(asset))
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: failed to write " + target.string()).c_str());
		return false;
	}
	return true;
}

// ─── Shared glTF geometry path ───────────────────────────────────────────────

GltfPrimitiveAttributes appendPrimitive(const cgltf_primitive& prim,
                                        const glm::mat4&       world,
                                        float                  uniformScale,
                                        MeshVertexStreams      out,
                                        std::vector<uint32_t>& indices)
{
	if (prim.type != cgltf_primitive_type_triangles)
		return {};

	GltfPrimitiveAttributes attrs;
	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const cgltf_attribute& attr = prim.attributes[i];
		switch (attr.type)
		{
		case cgltf_attribute_type_position: attrs.position = attr.data; break;
		case cgltf_attribute_type_normal:   attrs.normal   = attr.data; break;
		case cgltf_attribute_type_texcoord: if (attr.index == 0) attrs.uv      = attr.data; break;
		case cgltf_attribute_type_joints:   if (attr.index == 0) attrs.joints  = attr.data; break;
		case cgltf_attribute_type_weights:  if (attr.index == 0) attrs.weights = attr.data; break;
		default: break;
		}
	}
	if (!attrs.position)
		return {};

	const uint32_t  baseVertex = static_cast<uint32_t>(out.positions.size() / 3);
	const glm::mat3 normalMat  = glm::transpose(glm::inverse(glm::mat3(world)));

	for (cgltf_size v = 0; v < attrs.position->count; ++v)
	{
		float p[3] = {};
		cgltf_accessor_read_float(attrs.position, v, p, 3);
		glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f)) * uniformScale;
		out.positions.insert(out.positions.end(), { wp.x, wp.y, wp.z });

		if (attrs.normal && v < attrs.normal->count)
		{
			float n[3] = {};
			cgltf_accessor_read_float(attrs.normal, v, n, 3);
			glm::vec3 wn = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
			out.normals.insert(out.normals.end(), { wn.x, wn.y, wn.z });
		}
		else
			out.normals.insert(out.normals.end(), { 0.0f, 0.0f, 0.0f });

		if (attrs.uv && v < attrs.uv->count)
		{
			float uv[2] = {};
			cgltf_accessor_read_float(attrs.uv, v, uv, 2);
			// glTF puts the UV origin at the TOP-left; the engine is GL-style
			// BOTTOM-left (TextureImporter flips images on import, the Metal
			// backend flips V at sample time to match). Taking glTF's V verbatim
			// rendered every imported mesh's texture vertically mirrored.
			out.uvs.insert(out.uvs.end(), { uv[0], 1.0f - uv[1] });
		}
		else
			out.uvs.insert(out.uvs.end(), { 0.0f, 0.0f });
	}

	if (prim.indices)
	{
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			indices.push_back(baseVertex
				+ static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
	}
	else
	{
		for (cgltf_size i = 0; i < attrs.position->count; ++i)
			indices.push_back(baseVertex + static_cast<uint32_t>(i));
	}

	return attrs;
}

void appendSkinning(const GltfPrimitiveAttributes& attrs,
                    std::vector<uint32_t>&         boneIDs,
                    std::vector<float>&            boneWeights)
{
	if (!attrs.position)
		return;

	for (cgltf_size v = 0; v < attrs.position->count; ++v)
	{
		// Skinning: 4 joints + 4 weights per vertex
		uint32_t jids[4] = {};
		float    wts[4]  = { 1.0f, 0.0f, 0.0f, 0.0f };
		if (attrs.joints && v < attrs.joints->count)
		{
			float jf[4] = {};
			cgltf_accessor_read_float(attrs.joints, v, jf, 4);
			for (int k = 0; k < 4; ++k)
				jids[k] = static_cast<uint32_t>(jf[k]);
		}
		if (attrs.weights && v < attrs.weights->count)
			cgltf_accessor_read_float(attrs.weights, v, wts, 4);

		boneIDs.insert(boneIDs.end(), { jids[0], jids[1], jids[2], jids[3] });
		boneWeights.insert(boneWeights.end(), { wts[0], wts[1], wts[2], wts[3] });
	}
}

namespace
{
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

std::string importBaseColorMaterial(const cgltf_data*            data,
                                    const std::filesystem::path& sourcePath,
                                    const std::filesystem::path& contentRoot,
                                    const std::filesystem::path& relativeOutputDir,
                                    const std::string&           meshStem)
{
	const std::string texPath = importBaseColorTexture(
		data, sourcePath, contentRoot, relativeOutputDir, meshStem);
	if (texPath.empty())
		return {};

	MaterialAsset mat;
	mat.type       = HE::AssetType::Material;
	mat.name       = meshStem + "_mat";
	mat.path       = Importer::toAssetPath(relativeOutputDir / (mat.name + ".hasset"));
	mat.shaderPath = "builtin/unlit";
	mat.texturePaths.push_back(texPath);
	if (!Importer::writeAsset(mat, contentRoot))
		return {};
	return mat.path;
}

// ─── Re-import bookkeeping ───────────────────────────────────────────────────

std::vector<std::string> meshSidecarAssets(const std::filesystem::path& meshAsset,
                                           const std::filesystem::path& contentRoot)
{
	std::vector<std::string> out;

	HAsset::Reader reader;
	if (!reader.open(meshAsset.string()))
		return out;

	// Only the two mesh asset types carry MREF, so every other asset type leaves
	// here with an empty list.
	const auto* mref = reader.findChunk(HAsset::CHUNK_MREF);
	if (!mref)
		return out;

	std::string materialPath;
	size_t      off = 0;
	if (!HAsset::Reader::readString(mref->data, off, materialPath) || materialPath.empty())
		return out;
	out.push_back(materialPath);

	// …and the textures that material references, written by the same import.
	HAsset::Reader matReader;
	if (!matReader.open((contentRoot / materialPath).string()))
		return out;  // gone — the caller already sees the missing material
	const auto* mtrl = matReader.findChunk(HAsset::CHUNK_MTRL);
	if (!mtrl)
		return out;

	std::string              shaderPath;    // read past it: never a file on disk
	std::vector<std::string> texturePaths;
	size_t                   moff = 0;
	if (!HAsset::Reader::readString(mtrl->data, moff, shaderPath)) return out;
	if (!HAsset::Reader::readVec(mtrl->data, moff, texturePaths))  return out;
	for (const std::string& tex : texturePaths)
		if (!tex.empty())
			out.push_back(tex);

	return out;
}

bool importOutputsUpToDate(const std::filesystem::path&    source,
                           const std::filesystem::path&    contentRoot,
                           const std::filesystem::path&    primaryOutput,
                           const std::vector<std::string>& extraOutputs)
{
	std::error_code ec;
	const auto srcTime = std::filesystem::last_write_time(source, ec);
	if (ec) return false;

	const auto current = [&srcTime](const std::filesystem::path& output)
	{
		std::error_code e;
		const auto outTime = std::filesystem::last_write_time(output, e);
		return !e && outTime >= srcTime;
	};

	if (!current(primaryOutput))
		return false;
	for (const std::string& rel : meshSidecarAssets(primaryOutput, contentRoot))
		if (!current(contentRoot / rel))
			return false;
	for (const std::string& rel : extraOutputs)
		if (!current(contentRoot / rel))
			return false;
	return true;
}

} // namespace Importer
