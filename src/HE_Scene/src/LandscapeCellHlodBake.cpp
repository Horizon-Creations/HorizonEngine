#include "HorizonScene/LandscapeCellHlodBake.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include <ContentManager/ContentManager.h>

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
			inst.mesh.uvs = std::move(uv);
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
	const auto fail = [&](const char* why) { if (error) *error = why; return false; };
	std::string err;
	auto pack = LandscapePackSource::open(packPath, &err);
	if (!pack) return fail("not a landscape pack");
	const bool hadHlod = pack->hlod() != nullptr;
	pack.reset();   // the file is renamed over below: no open handle on it (Windows)

	std::error_code ec;
	uint64_t size = std::filesystem::file_size(packPath, ec);
	if (ec) return fail("pack size unreadable");

	// An existing HLOD section ends the file with its trailer: cut the file back to where that table began.
	if (hadHlod && size >= kLandscapePackHlodTrailerBytes)
	{
		uint8_t tr[kLandscapePackHlodTrailerBytes];
		std::ifstream in(packPath, std::ios::binary);
		in.seekg(static_cast<std::streamoff>(size - sizeof(tr)));
		in.read(reinterpret_cast<char*>(tr), sizeof(tr));
		if (!in) return fail("pack trailer unreadable");
		uint64_t off = 0;
		for (int i = 7; i >= 0; --i) off = (off << 8) | tr[i];
		if (off > size) return fail("pack trailer is damaged");
		size = off;
	}
	// Written to a sibling and renamed over the pack: a crash leaves either the old or the new file, never half of one.
	const std::string tmp = packPath + ".hlod.tmp";
	{
		std::ifstream in(packPath, std::ios::binary);
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!in || !out) return fail("cannot write next to the pack");
		constexpr size_t kChunk = 1u << 20;
		std::vector<char> buf(kChunk);
		uint64_t left = size;
		while (left > 0)
		{
			const size_t n = static_cast<size_t>(std::min<uint64_t>(left, kChunk));
			in.read(buf.data(), static_cast<std::streamsize>(n));
			if (static_cast<size_t>(in.gcount()) != n) return fail("pack read failed");
			out.write(buf.data(), static_cast<std::streamsize>(n));
			left -= n;
		}
		if (table.cellCount() > 0)
		{
			const std::vector<uint8_t> hb = table.encode();
			uint8_t tr[kLandscapePackHlodTrailerBytes];
			const uint64_t sz = hb.size();
			for (int i = 0; i < 8; ++i) { tr[i] = static_cast<uint8_t>(size >> (8 * i)); tr[8 + i] = static_cast<uint8_t>(sz >> (8 * i)); }
			for (int i = 0; i < 4; ++i) tr[16 + i] = static_cast<uint8_t>(kLandscapePackHlodMagic >> (8 * i));
			out.write(reinterpret_cast<const char*>(hb.data()), static_cast<std::streamsize>(hb.size()));
			out.write(reinterpret_cast<const char*>(tr), sizeof(tr));
		}
		out.flush();
		if (!out) { std::filesystem::remove(tmp, ec); return fail("pack write failed"); }
	}
	std::filesystem::rename(tmp, packPath, ec);
	if (ec) { std::filesystem::remove(tmp, ec); return fail("cannot replace the pack"); }
	return true;
}

} // namespace HE
