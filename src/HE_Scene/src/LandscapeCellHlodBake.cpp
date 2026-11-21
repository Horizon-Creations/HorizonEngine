#include "HorizonScene/LandscapeCellHlodBake.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureBlockDecode.h>

#include <climits>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace HE
{

namespace
{
	struct Instance
	{
		LandscapeHlodSourceMesh mesh;
		float                   size = 0.0f;   // bounding diagonal in metres, what the budgets rank by
	};

	// The mesh asset's triangles as plain arrays (the cooked interleaved form or the SoA arrays).
	bool readMesh(const StaticMeshAsset& a, std::vector<glm::vec3>& pos, std::vector<glm::vec3>& nor, std::vector<glm::vec2>& uv)
	{
		if (a.cooked && a.vertexCount > 0 && a.interleaved.size() >= static_cast<size_t>(a.vertexCount) * 8)
		{
			for (uint32_t i = 0; i < a.vertexCount; ++i)
			{
				const float* v = &a.interleaved[static_cast<size_t>(i) * 8];
				pos.emplace_back(v[0], v[1], v[2]);
				nor.emplace_back(v[3], v[4], v[5]);
				uv.emplace_back(v[6], v[7]);
			}
			return true;
		}
		const size_t n = a.vertices.size() / 3;
		if (n == 0) return false;
		for (size_t i = 0; i < n; ++i)
		{
			pos.emplace_back(a.vertices[i * 3], a.vertices[i * 3 + 1], a.vertices[i * 3 + 2]);
			nor.push_back(a.normals.size() >= (i + 1) * 3 ? glm::vec3(a.normals[i * 3], a.normals[i * 3 + 1], a.normals[i * 3 + 2]) : glm::vec3(0, 1, 0));
			uv.push_back(a.uvs.size() >= (i + 1) * 2 ? glm::vec2(a.uvs[i * 2], a.uvs[i * 2 + 1]) : glm::vec2(0.0f));
		}
		return true;
	}

	// The colour a far eye sees an instance in: its material's base colour times the mean of its albedo texture. The entity's material
	// override first, else the mesh's first section. Linear; mid grey when nothing can be read (a graph material has no base colour
	// to speak of, a texture in a format the CPU cannot read keeps the base colour alone).
	glm::vec3 instanceColour(entt::registry& reg, entt::entity e, const StaticMeshAsset& mesh, ContentManager& content)
	{
		const MaterialAsset* mat = nullptr;
		if (const auto* mc = reg.try_get<MaterialComponent>(e); mc && mc->materialAssetId != HE::UUID{})
		{
			content.ensureResident(mc->materialAssetId);
			mat = content.getMaterial(mc->materialAssetId);
		}
		if (!mat && !mesh.sections.empty()) mat = content.resolveMaterialRef(mesh.sections[0].materialId, mesh.sections[0].materialPath);
		if (!mat) return glm::vec3(0.5f);
		glm::vec3 c(mat->baseColor[0], mat->baseColor[1], mat->baseColor[2]);
		const HE::UUID texId = mat->textureIds.empty() ? HE::UUID{} : mat->textureIds[0];
		const std::string texPath = mat->texturePaths.empty() ? std::string() : mat->texturePaths[0];
		if (const TextureAsset* t = content.resolveTextureRef(texId, texPath))
		{
			float rgb[3];
			if (t->layers <= 1 && textureArraySliceMean(*t, 0, rgb)) c *= glm::vec3(rgb[0], rgb[1], rgb[2]);
		}
		return glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
	}

	uint8_t toSrgb8(float linear)
	{
		const float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
		return static_cast<uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
	}

	// The palette entry of a colour, added if the palette does not have it. Colours are held to 5 bits a channel, so near twins
	// share an entry; a full palette gives the nearest entry it has.
	uint32_t paletteEntry(std::vector<uint8_t>& palette, const glm::vec3& linear)
	{
		const uint8_t rgb[3] = { static_cast<uint8_t>(toSrgb8(linear.r) & 0xF8u), static_cast<uint8_t>(toSrgb8(linear.g) & 0xF8u), static_cast<uint8_t>(toSrgb8(linear.b) & 0xF8u) };
		uint32_t best = 0;
		int bestDist = INT_MAX;
		for (uint32_t i = 0; i * 3 + 2 < palette.size(); ++i)
		{
			const int dr = palette[i * 3] - rgb[0], dg = palette[i * 3 + 1] - rgb[1], db = palette[i * 3 + 2] - rgb[2];
			const int d = dr * dr + dg * dg + db * db;
			if (d == 0) return i;
			if (d < bestDist) { bestDist = d; best = i; }
		}
		if (palette.size() / 3 >= kLandscapeHlodPaletteEntries) return best;
		palette.insert(palette.end(), rgb, rgb + 3);
		return static_cast<uint32_t>(palette.size() / 3 - 1);
	}

	bool isStaticMeshEntity(entt::registry& reg, entt::entity e)
	{
		const auto* mc = reg.try_get<MeshComponent>(e);
		if (!mc || !mc->visible || mc->meshAssetId == HE::UUID{}) return false;
		if (reg.all_of<SkeletalMeshComponent>(e) || reg.all_of<AnimatorComponent>(e)) return false;
		if (const auto* rb = reg.try_get<RigidBodyComponent>(e); rb && rb->type != RigidBodyType::Static) return false;
		return true;
	}
}

LandscapeHlodTable bakeCellProxies(const CellManifest& manifest, const CellStreamer::Reader& reader, ContentManager& content,
                                   const CellHlodBakeOptions& options, CellHlodBakeStats* statsOut)
{
	CellHlodBakeStats st;
	LandscapeHlodTable table;
	st.cells = manifest.cells.size();
	SceneSerializer serializer;

	for (const CellManifest::Cell& c : manifest.cells)
	{
		const auto read = reader ? reader(manifest.cellPath(c.x, c.z)) : std::function<bool(std::vector<uint8_t>&)>{};
		std::vector<uint8_t> bytes;
		HorizonWorld scratch;
		if (!read || !read(bytes) || !serializer.loadFromMemory(scratch, bytes)) { ++st.failed; continue; }

		std::vector<Instance> instances;
		auto& reg = scratch.registry();
		for (const entt::entity e : reg.view<MeshComponent>())
		{
			if (!isStaticMeshEntity(reg, e)) continue;
			const HE::UUID id = reg.get<MeshComponent>(e).meshAssetId;
			content.ensureResident(id);
			const StaticMeshAsset* asset = content.getStaticMesh(id);
			std::vector<glm::vec3> pos, nor;
			std::vector<glm::vec2> uv;
			if (!asset || !readMesh(*asset, pos, nor, uv) || asset->indices.size() < 3) { ++st.meshesMissing; continue; }

			const glm::mat4 world = HE::worldMatrixOf(scratch, e);
			const glm::mat3 normalM = glm::inverseTranspose(glm::mat3(world));
			glm::vec3 lo(1e30f), hi(-1e30f);
			Instance inst;
			inst.mesh.positions.reserve(pos.size());
			inst.mesh.normals.reserve(pos.size());
			for (size_t i = 0; i < pos.size(); ++i)
			{
				const glm::vec3 p = glm::vec3(world * glm::vec4(pos[i], 1.0f));
				lo = glm::min(lo, p);
				hi = glm::max(hi, p);
				inst.mesh.positions.push_back(p);
				const glm::vec3 n = normalM * nor[i];
				inst.mesh.normals.push_back(glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 1, 0));
			}
			// Every vertex of the instance gets ONE uv: the palette texel of the instance's colour (the mesh's own uvs mean nothing
			// to a proxy, which does not have the instance's textures).
			inst.mesh.uvs.assign(pos.size(), landscapeHlodPaletteUv(paletteEntry(table.palette, instanceColour(reg, e, *asset, content))));
			inst.mesh.indices = asset->indices;
			inst.size = glm::length(hi - lo);
			instances.push_back(std::move(inst));
		}
		if (instances.empty()) { ++st.empty; continue; }

		// The big instances first; the budgets cut the small ones.
		std::stable_sort(instances.begin(), instances.end(), [](const Instance& a, const Instance& b) { return a.size > b.size; });
		std::vector<LandscapeHlodSourceMesh> sources;
		size_t triangles = 0;
		for (Instance& inst : instances)
		{
			const size_t t = inst.mesh.indices.size() / 3;
			if (sources.size() >= options.maxInstancesPerCell || triangles + t > options.maxTrianglesPerCell) { ++st.instancesDropped; continue; }
			triangles += t;
			sources.push_back(std::move(inst.mesh));
		}
		LandscapeHlodMesh merged = bakeCellHlod(sources);
		if (merged.empty()) { ++st.empty; continue; }
		st.instances += sources.size();
		st.triangles += merged.indices.size() / 3;
		st.bytes += merged.bytes();
		++st.baked;
		table.set(c.x, c.z, std::move(merged));
	}
	if (statsOut) *statsOut = st;
	return table;
}

bool landscapePackSetHlod(const std::string& packPath, const LandscapeHlodTable& table, std::string* error)
{
	return landscapePackSetTails(packPath, &table, nullptr, error);   // the far image, if the pack has one, stays
}

} // namespace HE
