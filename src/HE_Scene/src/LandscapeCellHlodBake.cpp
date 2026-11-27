#include "HorizonScene/LandscapeCellHlodBake.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/SceneJsonParse.h"
#include "HorizonScene/SceneSerializer.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureBlockDecode.h>
#include <Diagnostics/Profiler.h>

#include <climits>
#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace HE
{

namespace
{
	// The mesh asset's triangles as plain arrays (the cooked interleaved form or the SoA arrays).
	bool readMesh(const StaticMeshAsset& a, CellMeshLocal& out)
	{
		if (a.cooked && a.vertexCount > 0 && a.interleaved.size() >= static_cast<size_t>(a.vertexCount) * 8)
		{
			for (uint32_t i = 0; i < a.vertexCount; ++i)
			{
				const float* v = &a.interleaved[static_cast<size_t>(i) * 8];
				out.positions.emplace_back(v[0], v[1], v[2]);
				out.normals.emplace_back(v[3], v[4], v[5]);
			}
		}
		else
		{
			const size_t n = a.vertices.size() / 3;
			if (n == 0) return false;
			for (size_t i = 0; i < n; ++i)
			{
				out.positions.emplace_back(a.vertices[i * 3], a.vertices[i * 3 + 1], a.vertices[i * 3 + 2]);
				out.normals.push_back(a.normals.size() >= (i + 1) * 3 ? glm::vec3(a.normals[i * 3], a.normals[i * 3 + 1], a.normals[i * 3 + 2]) : glm::vec3(0, 1, 0));
			}
		}
		out.indices = a.indices;
		return true;
	}

	// The colour a far eye sees an instance in: its material's base colour times the mean of its albedo texture. The entity's material
	// override first, else the mesh's first section. Linear; mid grey when nothing can be read (a graph material has no base colour
	// to speak of, a texture in a format the CPU cannot read keeps the base colour alone).
	glm::vec3 instanceColour(const HE::UUID& materialOverride, const StaticMeshAsset* mesh, ContentManager& content)
	{
		const MaterialAsset* mat = nullptr;
		if (materialOverride != HE::UUID{})
		{
			content.ensureResident(materialOverride);
			mat = content.getMaterial(materialOverride);
		}
		if (!mat && mesh && !mesh->sections.empty()) mat = content.resolveMaterialRef(mesh->sections[0].materialId, mesh->sections[0].materialPath);
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

	// A parsed scene is destroyed on the pool: a cell's tree is a lot of small allocations.
	void releaseOnPool(nlohmann::json&& scene)
	{
		auto holder = std::make_shared<nlohmann::json>(std::move(scene));
		globalPool().post([holder]() mutable { holder.reset(); }, "CellHlodRelease", 1, JobPriority::Low);
	}

	constexpr uint64_t kCellHlodBakeVersion = 1;   // bump when what the steps make from the same cell changes: the caches go stale
}

bool cellStatics(const nlohmann::json& parsedCell, std::vector<CellStatic>& out)
{
	HorizonWorld scratch;
	SceneSerializer serializer;
	if (!serializer.loadAdditiveFromJson(scratch, parsedCell)) return false;
	auto& reg = scratch.registry();
	for (const entt::entity e : reg.view<MeshComponent>())
	{
		if (!isStaticMeshEntity(reg, e)) continue;
		CellStatic s;
		s.mesh = reg.get<MeshComponent>(e).meshAssetId;
		if (const auto* mc = reg.try_get<MaterialComponent>(e)) s.material = mc->materialAssetId;
		s.world = HE::worldMatrixOf(scratch, e);
		out.push_back(s);
	}
	return true;
}

std::vector<CellInstance> resolveCellStatics(const std::vector<CellStatic>& statics, ContentManager& content, std::vector<uint8_t>& palette,
                                             CellHlodBakeStats& stats)
{
	// A mesh asset is read once for all its instances, and a colour once per mesh and material (its texture's mean is the costly part).
	struct Loaded
	{
		std::shared_ptr<const CellMeshLocal> mesh;   // null: it did not load
		std::unordered_map<HE::UUID, uint32_t> colour;   // by the entity's material override
	};
	std::unordered_map<HE::UUID, Loaded> loaded;
	std::vector<CellInstance> out;
	out.reserve(statics.size());
	for (const CellStatic& s : statics)
	{
		const auto [it, fresh] = loaded.try_emplace(s.mesh);
		Loaded& l = it->second;
		if (fresh)
		{
			content.ensureResident(s.mesh);
			const StaticMeshAsset* asset = content.getStaticMesh(s.mesh);
			auto local = std::make_shared<CellMeshLocal>();
			if (asset && asset->indices.size() >= 3 && readMesh(*asset, *local)) l.mesh = std::move(local);
		}
		if (!l.mesh) { ++stats.meshesMissing; continue; }
		auto c = l.colour.find(s.material);
		if (c == l.colour.end())
		{
			content.ensureResident(s.mesh);   // the pointer a load since then may have moved: asked again
			c = l.colour.emplace(s.material, paletteEntry(palette, instanceColour(s.material, content.getStaticMesh(s.mesh), content))).first;
		}
		out.push_back({ l.mesh, s.world, c->second });
	}
	return out;
}

LandscapeHlodMesh mergeCellInstances(std::vector<CellInstance>& instances, const CellHlodBakeOptions& options, CellHlodBakeStats& stats)
{
	struct Ranked
	{
		LandscapeHlodSourceMesh mesh;
		float                   size = 0.0f;   // bounding diagonal in metres, what the budgets rank by
	};
	std::vector<Ranked> ranked;
	ranked.reserve(instances.size());
	for (const CellInstance& in : instances)
	{
		const CellMeshLocal& m = *in.mesh;
		const glm::mat3 normalM = glm::inverseTranspose(glm::mat3(in.world));
		glm::vec3 lo(1e30f), hi(-1e30f);
		Ranked r;
		r.mesh.positions.reserve(m.positions.size());
		r.mesh.normals.reserve(m.positions.size());
		for (size_t i = 0; i < m.positions.size(); ++i)
		{
			const glm::vec3 p = glm::vec3(in.world * glm::vec4(m.positions[i], 1.0f));
			lo = glm::min(lo, p);
			hi = glm::max(hi, p);
			r.mesh.positions.push_back(p);
			const glm::vec3 n = normalM * m.normals[i];
			r.mesh.normals.push_back(glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 1, 0));
		}
		// Every vertex of the instance gets ONE uv: the palette texel of the instance's colour (the mesh's own uvs mean nothing
		// to a proxy, which does not have the instance's textures).
		r.mesh.uvs.assign(m.positions.size(), landscapeHlodPaletteUv(in.paletteEntry));
		r.mesh.indices = m.indices;
		r.size = glm::length(hi - lo);
		ranked.push_back(std::move(r));
	}
	instances.clear();

	// The big instances first; the budgets cut the small ones.
	std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.size > b.size; });
	std::vector<LandscapeHlodSourceMesh> sources;
	size_t triangles = 0;
	for (Ranked& r : ranked)
	{
		const size_t t = r.mesh.indices.size() / 3;
		if (sources.size() >= options.maxInstancesPerCell || triangles + t > options.maxTrianglesPerCell) { ++stats.instancesDropped; continue; }
		triangles += t;
		sources.push_back(std::move(r.mesh));
	}
	LandscapeHlodMesh merged = bakeCellHlod(sources);
	if (!merged.empty())
	{
		stats.instances += sources.size();
		stats.triangles += merged.indices.size() / 3;
		stats.bytes += merged.bytes();
	}
	return merged;
}

LandscapeHlodTable bakeCellProxies(const CellManifest& manifest, const CellStreamer::Reader& reader, ContentManager& content,
                                   const CellHlodBakeOptions& options, CellHlodBakeStats* statsOut)
{
	CellHlodBakeStats st;
	LandscapeHlodTable table;
	st.cells = manifest.cells.size();
	for (const CellManifest::Cell& c : manifest.cells)
	{
		const auto read = reader ? reader(manifest.cellPath(c.x, c.z)) : std::function<bool(std::vector<uint8_t>&)>{};
		std::vector<uint8_t> bytes;
		std::vector<CellStatic> statics;
		if (!read || !read(bytes)) { ++st.failed; continue; }
		const nlohmann::json parsed = parseSceneBytes(bytes);
		if (parsed.is_discarded() || !cellStatics(parsed, statics)) { ++st.failed; continue; }
		std::vector<CellInstance> instances = resolveCellStatics(statics, content, table.palette, st);
		LandscapeHlodMesh merged = mergeCellInstances(instances, options, st);
		if (merged.empty()) { ++st.empty; continue; }
		++st.baked;
		table.set(c.x, c.z, std::move(merged));
	}
	if (statsOut) *statsOut = st;
	return table;
}

CellStreamer::Reader cellFileReader(const std::filesystem::path& projectRoot)
{
	return [projectRoot](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)> {
		const std::filesystem::path file = projectRoot / path;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec)) return {};
		return [file](std::vector<uint8_t>& out) {
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	};
}

CellHlodStamper cellFileStamper(const std::filesystem::path& projectRoot)
{
	return [projectRoot](const std::string& path, CellHlodStamp& out) {
		const std::filesystem::path file = projectRoot / path;
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(file, ec);
		if (ec) return false;
		const auto modified = std::filesystem::last_write_time(file, ec);
		if (ec) return false;
		out.size = size;
		out.modified = static_cast<int64_t>(modified.time_since_epoch().count());
		return true;
	};
}

uint64_t cellHlodOptionsKey(const CellHlodBakeOptions& options)
{
	uint64_t h = 1469598103934665603ull;
	for (const uint64_t v : { kCellHlodBakeVersion, uint64_t(options.maxTrianglesPerCell), uint64_t(options.maxInstancesPerCell) })
		for (int b = 0; b < 8; ++b) h = (h ^ ((v >> (8 * b)) & 0xFF)) * 1099511628211ull;
	return h;
}

// ── CellHlodBakeJob ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// A cell's way through: 0 the file is being read and parsed (pool) → 1 parsed / 2 unreadable (main: statics and content) → 3 merging (pool) →
// 4 done, to be entered into the cache (main). The stage is the hand-over: whoever stores it last wrote the slot's data before.
struct CellHlodBakeJob::Slot
{
	LandscapeHlodCellKey key;
	CellHlodStamp stamp;
	std::atomic<int> stage{ 0 };
	nlohmann::json parsed;
	std::vector<CellInstance> instances;
	LandscapeHlodMesh mesh;
	CellHlodBakeStats stats;
	bool failed = false;
};

struct CellHlodBakeJob::Scan
{
	std::atomic<bool> done{ false };
	std::vector<std::pair<LandscapeHlodCellKey, CellHlodStamp>> dirty;
	std::vector<LandscapeHlodCellKey> gone;   // in the cache, not in the manifest any more
	size_t reused = 0;
};

namespace
{
	constexpr size_t kParseAhead = 8;   // cells read and parsed ahead of the main thread's content work: bounds what is held at once
}

void CellHlodBakeJob::start(const CellManifest& manifest, CellStreamer::Reader reader, CellHlodStamper stamper, const CellHlodBakeOptions& options,
                            CellHlodCache& cache)
{
	cancel();
	m_manifest = manifest;
	m_reader = std::move(reader);
	m_options = options;
	m_cache = &cache;
	m_cancel = CancelToken::create();
	m_slots.clear();
	m_nextParse = m_nextResolve = m_applied = 0;
	m_stats = {};
	m_stats.cells = manifest.cells.size();
	m_phase = Phase::Scan;

	// The pass over the manifest: a stat a cell, on the pool. Reads the cache's stamps, which the caller leaves alone until it is through.
	auto scan = m_scan = std::make_shared<Scan>();
	const bool allDirty = cache.optionsKey != cellHlodOptionsKey(options);
	const auto* stamps = &cache.stamps;
	JobDesc desc;
	desc.name = "CellHlodScan";
	desc.priority = JobPriority::Low;
	desc.cancel = m_cancel;
	desc.kind = JobKind::Io;
	m_scanJob = globalPool().schedule([scan, manifest = m_manifest, stamper = std::move(stamper), stamps, allDirty]
	{
		HE_PROFILE_SCOPE_N("CellHlodScan");
		std::unordered_set<LandscapeHlodCellKey, LandscapeHlodCellKeyHash> inManifest;
		for (const CellManifest::Cell& c : manifest.cells)
		{
			const LandscapeHlodCellKey k{ c.x, c.z };
			inManifest.insert(k);
			CellHlodStamp now;
			if (!stamper || !stamper(manifest.cellPath(c.x, c.z), now)) now = {};
			const auto old = stamps->find(k);
			if (!allDirty && old != stamps->end() && old->second == now) ++scan->reused;
			else scan->dirty.emplace_back(k, now);
		}
		for (const auto& kv : *stamps)
			if (!inManifest.count(kv.first)) scan->gone.push_back(kv.first);
		scan->done.store(true, std::memory_order_release);
	}, std::move(desc));
}

void CellHlodBakeJob::cancel()
{
	if (m_phase == Phase::Idle) return;
	m_cancel.cancel();
	if (m_scanJob.valid()) m_scanJob.wait();   // reads the cache's stamps: the caller may change them from here on
	m_scanJob = {};
	m_scan.reset();
	m_slots.clear();
	m_phase = Phase::Idle;
}

void CellHlodBakeJob::pump(ContentManager& content, double budgetMs)
{
	if (m_phase == Phase::Scan)
	{
		if (!m_scan->done.load(std::memory_order_acquire)) return;
		m_stats.reused = m_scan->reused;
		for (const LandscapeHlodCellKey& k : m_scan->gone)
		{
			m_cache->table.set(k.x, k.z, {});
			m_cache->stamps.erase(k);
		}
		for (auto& [k, stamp] : m_scan->dirty)
		{
			auto slot = std::make_shared<Slot>();
			slot->key = k;
			slot->stamp = stamp;
			m_slots.push_back(std::move(slot));
		}
		m_scan.reset();
		m_scanJob = {};
		m_phase = Phase::Cells;
	}
	if (m_phase != Phase::Cells) return;
	HE_PROFILE_SCOPE_N("CellHlodPump");

	// Reading and parsing ahead, a few cells at most.
	while (m_nextParse < m_slots.size() && m_nextParse - m_nextResolve < kParseAhead)
	{
		const std::shared_ptr<Slot>& slot = m_slots[m_nextParse++];
		JobDesc desc;
		desc.name = "CellHlodParse";
		desc.priority = JobPriority::Low;
		desc.cancel = m_cancel;
		auto read = m_reader ? m_reader(m_manifest.cellPath(slot->key.x, slot->key.z)) : std::function<bool(std::vector<uint8_t>&)>{};
		globalPool().schedule([slot, read = std::move(read), token = m_cancel]
		{
			HE_PROFILE_SCOPE_N("CellHlodParse");
			std::vector<uint8_t> bytes;
			if (token.cancelled() || !read || !read(bytes) || bytes.empty()) { slot->stage.store(2, std::memory_order_release); return; }
			slot->parsed = parseSceneBytes(bytes);
			const bool ok = !slot->parsed.is_discarded() && slot->parsed.is_object();
			slot->stage.store(ok ? 1 : 2, std::memory_order_release);
		}, std::move(desc));
	}

	// The content work, in order, while the budget lasts (a cell that is ready is always taken).
	const auto t0 = std::chrono::steady_clock::now();
	bool first = true;
	while (m_nextResolve < m_slots.size())
	{
		Slot& s = *m_slots[m_nextResolve];
		const int stage = s.stage.load(std::memory_order_acquire);
		if (stage != 1 && stage != 2) break;
		if (!first && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= budgetMs) break;
		first = false;
		++m_nextResolve;
		std::vector<CellStatic> statics;
		if (stage == 2 || !cellStatics(s.parsed, statics)) { s.failed = true; s.stage.store(4, std::memory_order_release); continue; }
		releaseOnPool(std::move(s.parsed));
		s.instances = resolveCellStatics(statics, content, m_cache->table.palette, s.stats);
		if (s.instances.empty()) { s.stage.store(4, std::memory_order_release); continue; }
		s.stage.store(3, std::memory_order_relaxed);
		JobDesc desc;
		desc.name = "CellHlodMerge";
		desc.priority = JobPriority::Low;
		desc.cancel = m_cancel;
		globalPool().schedule([slot = m_slots[m_nextResolve - 1], options = m_options]
		{
			HE_PROFILE_SCOPE_N("CellHlodMerge");
			slot->mesh = mergeCellInstances(slot->instances, options, slot->stats);
			slot->stage.store(4, std::memory_order_release);
		}, std::move(desc));
	}

	// What is through goes into the cache, in order.
	while (m_applied < m_slots.size() && m_slots[m_applied]->stage.load(std::memory_order_acquire) == 4)
	{
		Slot& s = *m_slots[m_applied];
		if (s.failed) ++m_stats.failed;
		else if (s.mesh.empty()) ++m_stats.empty;
		else ++m_stats.baked;
		m_stats.instances += s.stats.instances;
		m_stats.instancesDropped += s.stats.instancesDropped;
		m_stats.meshesMissing += s.stats.meshesMissing;
		m_stats.triangles += s.stats.triangles;
		m_stats.bytes += s.stats.bytes;
		m_cache->table.set(s.key.x, s.key.z, std::move(s.mesh));
		m_cache->stamps[s.key] = s.stamp;
		m_slots[m_applied].reset();
		++m_applied;
	}
	if (m_applied == m_slots.size())
	{
		m_cache->optionsKey = cellHlodOptionsKey(m_options);
		m_slots.clear();
		m_applied = 0;   // cellsLeft() reads 0 either way
		m_phase = Phase::Done;
	}
}

bool landscapePackSetHlod(const std::string& packPath, const LandscapeHlodTable& table, std::string* error)
{
	return landscapePackSetTails(packPath, &table, nullptr, error);   // the far image, if the pack has one, stays
}

} // namespace HE
