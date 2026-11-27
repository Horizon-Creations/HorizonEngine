#include "LandscapeFarBake.h"

#include "EditorApplication.h"   // AppContext
#include "NotificationStore.h"   // a bake's result goes to the footer bell
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include "ProjectManager.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/LandscapeHlod.h>
#include <HorizonScene/LandscapeEdit.h>
#include <HorizonScene/LandscapeExport.h>      // landscapeFarPaintFor: what the export paints with
#include <HorizonScene/LandscapeFarBake.h>     // the region bake of the tile world
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeAtlas.h>       // which tiles a region drawn from the material has in the atlas
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LandscapeFarBake
{
namespace
{
	namespace fs = std::filesystem;
	using HE::landscape::HlodAssetBlob;

	// One landscape's saved bake: the key it was baked from, and its tiles (ids,
	// paths and types; the bytes stay on disk until they are loaded).
	struct Entry { uint64_t key = 0; std::vector<HlodAssetBlob> assets; };

	std::string                                s_project;     // the project the state below belongs to
	std::unordered_map<std::string, Entry>     s_index;       // landscape (hex id) -> its saved bake
	std::unordered_map<std::string, uint64_t>  s_resident;    // landscape -> key of the tiles now resident
	std::unordered_map<std::string, uint64_t>  s_measured;    // landscape -> key last measured
	std::unordered_set<uint64_t>               s_refused;     // automatic: keys a bake made nothing for
	std::string                                s_summary;
	double                                     s_lastChange = 0.0;
	double                                     s_nextMeasure = 0.0;

	double now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	std::string hexOf(const HE::UUID& id)
	{
		char buf[40];
		std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(id.hi),
		              static_cast<unsigned long long>(id.lo));
		return buf;
	}

	fs::path saveDir(AppContext& ctx)
	{
		fs::path p = ctx.projectManager->currentProject().path;
		if (fs::is_regular_file(p)) p = p.parent_path();
		return p / "Saved" / "FarLandscape";
	}

	// What a landscape's far tiles depend on. Word-wise, so the million heights of a
	// 1025 landscape cost well under a millisecond (it runs once a second).
	struct Hasher
	{
		uint64_t h = 0xcbf29ce484222325ull;
		void word(uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xFF51AFD7ED558CCDull; }
		void bytes(const void* p, size_t n)
		{
			const auto* b = static_cast<const uint8_t*>(p);
			size_t i = 0;
			for (; i + 8 <= n; i += 8) { uint64_t v; std::memcpy(&v, b + i, 8); word(v); }
			uint64_t tail = 0;
			std::memcpy(&tail, b + i, n - i);
			word(tail ^ n);
		}
		void f(float v) { uint32_t u; std::memcpy(&u, &v, 4); word(u); }
	};

	uint64_t keyOf(HorizonWorld& w, entt::entity e, const TerrainComponent& tc)
	{
		Hasher k;
		const HE::UUID id = w.entityId(e);
		k.word(id.hi); k.word(id.lo);
		if (const auto* xf = w.registry().try_get<TransformComponent>(e))
		{
			k.f(xf->position.x); k.f(xf->position.y); k.f(xf->position.z);
			k.f(xf->rotation.x); k.f(xf->rotation.y); k.f(xf->rotation.z);
		}
		if (const auto* mc = w.registry().try_get<MaterialComponent>(e))
		{
			k.word(mc->materialAssetId.hi); k.word(mc->materialAssetId.lo);
		}
		k.f(tc.sizeX); k.f(tc.sizeZ); k.word(tc.resolution); k.f(tc.heightScale);
		k.word(static_cast<uint64_t>(tc.seed)); k.word(tc.weightRes);
		k.bytes(tc.sculptHeights.data(), tc.sculptHeights.size() * sizeof(float));
		k.bytes(tc.layerWeights.data(), tc.layerWeights.size());
		k.bytes(tc.layerWeights2.data(), tc.layerWeights2.size());
		k.bytes(tc.layerWeights3.data(), tc.layerWeights3.size());
		return k.h;
	}

	// Every landscape of the open scene (not the chunks it spawns), with its key.
	std::vector<std::pair<entt::entity, std::string>> landscapes(AppContext& ctx)
	{
		std::vector<std::pair<entt::entity, std::string>> out;
		auto& reg = ctx.world->registry();
		for (auto [e, tc] : reg.view<TerrainComponent>().each())
			if (!reg.all_of<TerrainChunkComponent>(e) && ctx.world->entityId(e) != HE::UUID{})
				out.emplace_back(e, hexOf(ctx.world->entityId(e)));
		return out;
	}

	void readIndex(AppContext& ctx)
	{
		s_index.clear();
		std::ifstream in(saveDir(ctx) / "index.json");
		if (!in) return;
		const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
		if (!j.is_object()) return;
		for (auto it = j.begin(); it != j.end(); ++it)
		{
			Entry e;
			e.key = std::strtoull(it->value("key", "0").c_str(), nullptr, 16);
			for (const auto& a : it->value("assets", nlohmann::json::array()))
			{
				HlodAssetBlob b;
				b.id   = HE::UUID{ std::strtoull(a.value("hi", "0").c_str(), nullptr, 16),
				                   std::strtoull(a.value("lo", "0").c_str(), nullptr, 16) };
				b.type = a.value("type", uint16_t(0));
				b.relPath = a.value("path", std::string());
				if (!b.relPath.empty()) e.assets.push_back(std::move(b));
			}
			s_index[it.key()] = std::move(e);
		}
	}

	void writeIndex(AppContext& ctx)
	{
		nlohmann::json j = nlohmann::json::object();
		char buf[24];
		for (const auto& [hex, e] : s_index)
		{
			nlohmann::json assets = nlohmann::json::array();
			for (const HlodAssetBlob& b : e.assets)
			{
				nlohmann::json a;
				std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(b.id.hi)); a["hi"] = buf;
				std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(b.id.lo)); a["lo"] = buf;
				a["type"] = b.type; a["path"] = b.relPath;
				assets.push_back(std::move(a));
			}
			std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(e.key));
			j[hex] = { { "key", buf }, { "assets", std::move(assets) } };
		}
		std::error_code ec;
		fs::create_directories(saveDir(ctx), ec);
		std::ofstream(saveDir(ctx) / "index.json") << j.dump(1);
	}

	void rebuild(AppContext& ctx, entt::entity e)
	{
		if (auto* tc = ctx.world->registry().try_get<TerrainComponent>(e)) tc->dirty = true;
	}

	// The saved tiles of `hex` into the content manager, if they were baked from `key`.
	bool loadSaved(AppContext& ctx, const std::string& hex, uint64_t key)
	{
		const auto it = s_index.find(hex);
		if (it == s_index.end() || it->second.key != key) return false;
		std::vector<HlodAssetBlob> blobs;
		for (const HlodAssetBlob& a : it->second.assets)
		{
			HlodAssetBlob b = a;
			std::ifstream f(saveDir(ctx) / a.relPath, std::ios::binary);
			if (!f) return false;   // a tile is missing: the bake is incomplete, the live material stays
			b.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			blobs.push_back(std::move(b));
		}
		const size_t n = HE::landscape::loadHlodAssets(*ctx.contentManager, blobs);
		HE_LOG_INFO(Editor, "LandscapeFarBake: %zu saved far tile asset(s) of landscape %s loaded", n, hex.c_str());
		s_resident[hex] = key;
		return true;
	}

	// The resident tiles of `hex` go: its far chunks draw the live material again.
	void putAway(AppContext& ctx, const std::string& hex)
	{
		if (const auto it = s_index.find(hex); it != s_index.end())
			for (const HlodAssetBlob& a : it->second.assets) ctx.contentManager->unloadAsset(a.id);
		s_resident.erase(hex);
	}

	bool usable(AppContext& ctx)
	{
		return ctx.projectLoaded && ctx.world && ctx.contentManager && ctx.renderer && ctx.projectManager;
	}

	// ── The tile world's far image ─────────────────────────────────────────────
	// A region the worker wants drawn from the material: it waits, the main thread draws it when the region's tiles are in the atlas.
	struct GroundRequest
	{
		std::mutex              m;
		std::condition_variable cv;
		bool                    pending = false, answered = false, ok = false;
		int                     rx = 0, rz = 0;
		std::vector<uint8_t>    rgba;
	};
	struct TileJob
	{
		std::thread                                thread;
		std::atomic<bool>                          done{ false };
		std::atomic<bool>                          cancel{ false };
		std::shared_ptr<HE::LandscapeEditSource>   edits;    // kept alive: the runtime may reset meanwhile
		std::shared_ptr<const HE::LandscapePackSource> pack;
		std::shared_ptr<HE::LandscapeFarTable>     table = std::make_shared<HE::LandscapeFarTable>();
		HE::LandscapeFarBakeStats                  stats;
		uint64_t                                   revision = 0;   // the runtime's coarseRevision when it started
		uint64_t                                   painterHash = 0;
		fs::path                                   cacheFile;
		double                                     started = 0.0;
		// From the material (a GroundRequest per region), else from the weights on the worker alone.
		bool                                       material = false, walk = false;
		HE::LandscapeFarLayout                     layout;   // what bakeLandscapeFar plans (default budget and side)
		GroundRequest                              ground;
		std::atomic<int>                           progress{ 0 };
		// Main thread only: the region being waited for.
		int                                        waitRx = -1, waitRz = -1, residentFrames = 0;
		double                                     waitSince = 0.0;
	};
	// Supersampling of the ground image: the far image's texel is the mean of 4 x 4 pixels of it (a texel of 32 m from 8 m pixels).
	constexpr uint32_t kGroundSupersample = 4;
	// 3: the final base colour under a dry, snowless sky (painted layers and height snow; the weather's snow, wetness, puddles live)
	constexpr uint64_t kGroundPainterVersion = 3;
	constexpr double   kGroundWaitSeconds = 30.0;   // a region whose tiles never all arrive (an atlas that refused one) keeps its old image
	LandscapeFarBake::GroundImage               s_groundImage;
	std::function<uint64_t()>                   s_groundKey;
	bool                                        s_anchorOn = false;
	glm::dvec3                                  s_anchor{ 0.0 };
	int                                         s_tileDeferred = 0;   // regions the last bake left as they were (not loaded)
	uint64_t                                    s_tileBakedPainter = 0;
	HE::LandscapeRuntime*                       s_tileRuntime = nullptr;
	std::unique_ptr<TileJob>                    s_tileJob;
	std::shared_ptr<const HE::LandscapeFarTable> s_tileLast;          // the last table: what the next bake reuses region by region
	const HE::LandscapeEditSource*              s_tileOverlay = nullptr;   // the overlay s_tileLast and the revisions belong to
	uint64_t                                    s_tileBaked = ~0ull;  // coarseRevision the far image is up to date with
	uint64_t                                    s_tileSeen = 0;
	double                                      s_tileChangeAt = 0.0;
	std::string                                 s_tileSummary;

	fs::path tileCacheFile(AppContext& ctx)
	{
		fs::path p = ctx.projectManager->currentProject().path;
		if (fs::is_regular_file(p)) p = p.parent_path();
		// The export's own cache (ExportDialogPanel: farCacheDir), so an export after an editor bake paints nothing again.
		return p / "Saved" / "LandscapeFar" / (s_tileRuntime->packFile().stem().generic_string() + ".far");
	}

	void stopTileJob()
	{
		s_anchorOn = false;
		if (!s_tileJob) return;
		s_tileJob->cancel = true;   // a worker waiting for a region of the material sees it within 50 ms
		if (s_tileJob->thread.joinable()) s_tileJob->thread.join();
		s_tileJob.reset();
	}

	uint64_t groundHashOf(uint64_t materialKey)
	{
		if (materialKey == 0) return 0;
		uint64_t h = 1469598103934665603ull;
		for (const uint64_t v : { kGroundPainterVersion, materialKey, uint64_t(kGroundSupersample) })
			for (int b = 0; b < 8; ++b) h = (h ^ ((v >> (8 * b)) & 0xFF)) * 1099511628211ull;
		return h;
	}

	// The worker's side of a region drawn from the material: asks, waits for the main thread (or a cancel).
	bool askGround(TileJob& j, int rx, int rz, std::vector<uint8_t>& rgba)
	{
		GroundRequest& g = j.ground;
		std::unique_lock<std::mutex> lock(g.m);
		g.rx = rx; g.rz = rz;
		g.pending = true; g.answered = false; g.ok = false;
		g.rgba.clear();
		while (!g.answered && !j.cancel.load()) g.cv.wait_for(lock, std::chrono::milliseconds(50));
		g.pending = false;
		if (!g.answered || !g.ok) return false;
		rgba = std::move(g.rgba);
		return true;
	}

	void answerGround(TileJob& j, bool ok, std::vector<uint8_t>&& rgba)
	{
		{
			std::lock_guard<std::mutex> lock(j.ground.m);
			j.ground.ok = ok;
			j.ground.rgba = std::move(rgba);
			j.ground.answered = true;
		}
		j.ground.cv.notify_all();
		j.waitRx = j.waitRz = -1;
		j.residentFrames = 0;
	}

	// The main thread's side, once a frame: the region the worker waits for, drawn from above once its tiles are in the atlas.
	void serveGround(TileJob& j)
	{
		int rx, rz;
		{
			std::lock_guard<std::mutex> lock(j.ground.m);
			if (!j.ground.pending || j.ground.answered) return;
			rx = j.ground.rx; rz = j.ground.rz;
		}
		const HE::LandscapeFarLayout& l = j.layout;
		const HE::LandscapeAtlas* atlas = s_tileRuntime ? s_tileRuntime->atlas() : nullptr;
		const double tileM = s_tileRuntime ? double(s_tileRuntime->quadtree().tileM) : 0.0;
		if (!atlas || tileM <= 0.0 || !s_groundImage) { answerGround(j, false, {}); return; }
		if (rx != j.waitRx || rz != j.waitRz) { j.waitRx = rx; j.waitRz = rz; j.waitSince = now(); j.residentFrames = 0; }

		const int x0 = rx * l.regionTiles, z0 = rz * l.regionTiles;
		const int x1 = std::min(x0 + l.regionTiles, l.tilesX), z1 = std::min(z0 + l.regionTiles, l.tilesZ);
		bool resident = true;
		for (int z = z0; z < z1 && resident; ++z)
			for (int x = x0; x < x1 && resident; ++x)
				if (j.pack->has({ x, z }) && atlas->lookup(x, z) == HE::LandscapeAtlas::kNoSlot) resident = false;
		const glm::dvec2 mn(double(x0) * tileM, double(z0) * tileM), mx = mn + glm::dvec2(double(l.regionTiles) * tileM);
		if (!resident)
		{
			j.residentFrames = 0;
			// Automatic: a region that is not loaded keeps what it had. Manual: the streaming goes there and the region waits for it.
			if (!j.walk || now() - j.waitSince > kGroundWaitSeconds)
			{
				if (j.walk) HE_LOG_WARN(Editor, "LandscapeFarBake: region (%d, %d) did not load in %.0f s, it keeps its old image", rx, rz, kGroundWaitSeconds);
				answerGround(j, false, {});
				return;
			}
			s_anchorOn = true;
			s_anchor = glm::dvec3((mn.x + mx.x) * 0.5, double(s_tileRuntime->config().heightMinM), (mn.y + mx.y) * 0.5);
			return;
		}
		// One frame more: the uploads of tiles that just got a slot go out with the next frame the landscape builds.
		if (++j.residentFrames < 2) return;

		const int side = l.regionSide();
		const uint32_t px = uint32_t(side) * kGroundSupersample;
		std::vector<uint8_t> image;
		if (!s_groundImage(mn, mx, px, image) || image.size() != size_t(px) * px * 4) { answerGround(j, false, {}); return; }
		// The mean of each texel's pixels in linear light; texels past the world's last tile stay 0 (a tile the world has not).
		std::vector<uint8_t> rgba(size_t(side) * size_t(side) * 4, 0);
		const int realX = (x1 - x0) * l.texelsPerTile, realZ = (z1 - z0) * l.texelsPerTile;
		const float inv = 1.0f / float(kGroundSupersample * kGroundSupersample);
		for (int ty = 0; ty < realZ; ++ty)
			for (int tx = 0; tx < realX; ++tx)
			{
				float sum[3] = { 0.0f, 0.0f, 0.0f };
				for (uint32_t sy = 0; sy < kGroundSupersample; ++sy)
					for (uint32_t sx = 0; sx < kGroundSupersample; ++sx)
					{
						const uint8_t* p = &image[((size_t(ty) * kGroundSupersample + sy) * px + size_t(tx) * kGroundSupersample + sx) * 4];
						for (int c = 0; c < 3; ++c) sum[c] += HE::landscapeFarDecodeSrgb(p[c]);
					}
				uint8_t* d = &rgba[(size_t(ty) * size_t(side) + size_t(tx)) * 4];
				for (int c = 0; c < 3; ++c) d[c] = HE::landscapeFarEncodeSrgb(sum[c] * inv);
				d[3] = 255;   // the material's base colour has its own occlusion in it; no separate AO
			}
		answerGround(j, true, std::move(rgba));
	}

	// The finished job's table onto the overlay it was baked from, if that is still the runtime's.
	void finishTileJob()
	{
		TileJob& j = *s_tileJob;
		if (j.thread.joinable()) j.thread.join();
		s_anchorOn = false;   // the streaming goes back to the camera
		const auto edits = s_tileRuntime ? s_tileRuntime->editsShared() : nullptr;
		char line[200];
		if (j.stats.cancelled)
			std::snprintf(line, sizeof(line), "Far image: cancelled.");
		else if (!edits || edits != j.edits || j.table->empty())
			std::snprintf(line, sizeof(line), "Far image: nothing came out (%d of %d region(s) failed).", j.stats.failed, j.stats.regions);
		else
		{
			const uint64_t before = s_tileRuntime->coarseRevision();
			edits->setFarImage(j.table);
			s_tileRuntime->coarseChanged();   // the window assembles and uploads the new image
			s_tileLast = j.table;
			// Up to date only if nothing was edited while it baked; else the next automatic bake catches up.
			s_tileBaked = (before == j.revision) ? s_tileRuntime->coarseRevision() : j.revision;
			s_tileBakedPainter = j.painterHash;
			s_tileDeferred = j.stats.deferred;
			if (j.stats.deferred > 0)
				std::snprintf(line, sizeof(line), "Far image: %d region(s) painted from the %s, %d kept, %d not loaded (stale until a manual bake), in %.1f s.",
				              j.stats.baked, j.material ? "material" : "weights", j.stats.reused, j.stats.deferred, now() - j.started);
			else
				std::snprintf(line, sizeof(line), "Far image: %d region(s) painted from the %s, %d kept, in %.1f s.", j.stats.baked,
				              j.material ? "material" : "weights", j.stats.reused, now() - j.started);
		}
		s_tileSummary = line;
		HE_LOG_INFO(Editor, "LandscapeFarBake: %s", s_tileSummary.c_str());
		s_tileJob.reset();
	}

	// Once per overlay: the last bake from the cache, so a reopened world shows it without painting.
	void adoptTileCache(AppContext& ctx, HE::LandscapeEditSource& edits)
	{
		s_tileOverlay = &edits;
		s_tileLast.reset();
		s_tileBaked = ~0ull;
		if (ctx.editorConfig.LandscapeFarBake == 0) return;
		std::ifstream in(tileCacheFile(ctx), std::ios::binary);
		if (!in) return;
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto table = std::make_shared<HE::LandscapeFarTable>();
		if (!HE::LandscapeFarTable::decode(bytes, *table) || table->empty()) return;
		edits.setFarImage(table);
		s_tileRuntime->coarseChanged();
		s_tileLast = table;
		// Manual: the cached image is what was baked last, shown as it is. Automatic checks it against the tiles once (a bake
		// that finds every region's hash unchanged paints nothing).
		s_tileBaked = ctx.editorConfig.LandscapeFarBake == 2 ? ~0ull : s_tileRuntime->coarseRevision();
		HE_LOG_INFO(Editor, "LandscapeFarBake: the tile world's far image from %s", tileCacheFile(ctx).string().c_str());
	}

	void tickTile(AppContext& ctx, bool userBusy)
	{
		if (!s_tileRuntime || !s_tileRuntime->active()) { s_anchorOn = false; return; }
		if (s_tileJob && s_tileJob->done) finishTileJob();
		HE::LandscapeEditSource* edits = s_tileRuntime->edits();
		if (!edits) return;
		if (edits != s_tileOverlay) { stopTileJob(); adoptTileCache(ctx, *edits); }
		if (s_tileJob && s_tileJob->material)
		{
			serveGround(*s_tileJob);
			char line[160];
			std::snprintf(line, sizeof(line), "Far image: baking from the material, region %d of %d...", s_tileJob->progress.load(),
			              s_tileJob->layout.regionsX() * s_tileJob->layout.regionsZ());
			s_tileSummary = line;
		}

		const double t = now();
		const uint64_t rev = s_tileRuntime->coarseRevision();
		if (rev != s_tileSeen) { s_tileSeen = rev; s_tileChangeAt = t; }
		// A witness: HE_LANDSCAPE_FAR_BAKE_NOW=1 presses Bake Far Image once, as soon as the material can be drawn.
		static bool s_witnessBaked = false;
		if (!s_witnessBaked && !s_tileJob && groundPainterHash() != 0)
			if (const char* e = std::getenv("HE_LANDSCAPE_FAR_BAKE_NOW"); e && *e == '1') { s_witnessBaked = true; bakeTileWorld(ctx, /*walk=*/true); return; }
		if (ctx.editorConfig.LandscapeFarBake != 2 || userBusy || ctx.isPlaying || s_tileJob) return;
		// A new edit, or another material (or the material became readable): the regions loaded now, after a few quiet seconds.
		const bool painterChanged = s_tileBaked != ~0ull && groundPainterHash() != 0 && groundPainterHash() != s_tileBakedPainter;
		if ((rev != s_tileBaked || painterChanged) && t - s_tileChangeAt >= 4.0) bakeTileWorld(ctx, /*walk=*/false);
	}
}

void setTileRuntime(HE::LandscapeRuntime* runtime)
{
	if (runtime != s_tileRuntime) { stopTileJob(); s_tileOverlay = nullptr; s_tileLast.reset(); }
	s_tileRuntime = runtime;
}

TileState tileState()
{
	if (!s_tileRuntime || !s_tileRuntime->active() || !s_tileRuntime->edits()) return TileState::NoWorld;
	if (s_tileJob) return TileState::Baking;
	if (s_tileBaked == s_tileRuntime->coarseRevision() && s_tileDeferred == 0) return TileState::UpToDate;
	return s_tileLast ? TileState::Stale : TileState::Idle;
}

const std::string& tileSummary() { return s_tileSummary; }

void setGroundImage(GroundImage image, std::function<uint64_t()> materialKey)
{
	s_groundImage = std::move(image);
	s_groundKey = std::move(materialKey);
}

uint64_t groundPainterHash()
{
	return s_groundImage && s_groundKey ? groundHashOf(s_groundKey()) : 0;
}

glm::dvec3 tileAnchor(const glm::dvec3& eye)
{
	return s_anchorOn && s_tileJob ? s_anchor : eye;
}

bool bakeTileWorld(AppContext& ctx, bool walk)
{
	if (!usable(ctx) || s_tileJob || !s_tileRuntime || !s_tileRuntime->active()) return false;
	const auto edits = s_tileRuntime->editsShared();
	if (!edits) return false;
	const auto pack = std::dynamic_pointer_cast<const HE::LandscapePackSource>(edits->baseShared());
	if (!pack) { s_tileSummary = "Far image: the world is not a pack."; return false; }

	// Main thread: what to paint with (the content manager), the hash of the unedited tiles, the options.
	auto job = std::make_unique<TileJob>();
	job->edits = edits;
	job->pack = pack;
	job->revision = s_tileRuntime->coarseRevision();
	job->cacheFile = tileCacheFile(ctx);
	job->started = now();
	job->walk = walk;
	HE::LandscapeFarBakeOptions bo;
	TileJob* jp = job.get();
	const uint64_t groundHash = groundPainterHash();
	job->layout = HE::planLandscapeFar(edits->geometry(), edits->tilesX(), edits->tilesZ(), bo.budgetBytes, bo.maxSide, bo.regionTiles);
	if (groundHash != 0)
	{
		// The landscape material, drawn region by region on the main thread (serveGround).
		job->material = true;
		bo.painter = [jp](const HE::LandscapeTileSource&, const HE::LandscapeFarLayout&, int rx, int rz, std::vector<uint8_t>& rgba) {
			return askGround(*jp, rx, rz, rgba);
		};
		bo.painterHash = groundHash;
	}
	else
	{
		const HE::LandscapeFarPaint paint = HE::landscapeFarPaintFor(s_tileRuntime->config().textureGroup, *ctx.contentManager);
		bo.painter = HE::landscapeFarWeightPainter(paint);
		bo.painterHash = HE::landscapeFarPaintHash(paint);
	}
	job->painterHash = bo.painterHash;
	bo.progress = [jp](int done, int) { jp->progress = done; return !jp->cancel.load(); };
	std::shared_ptr<const HE::LandscapeFarTable> previous = s_tileLast ? s_tileLast
		: pack->farImage() ? std::make_shared<HE::LandscapeFarTable>(*pack->farImage()) : nullptr;
	const HE::LandscapeFarTileHash packHash = HE::landscapePackTileHash(*pack);

	job->thread = std::thread([jp, bo, previous, packHash, pack]
	{
		const HE::LandscapeEditSource& world = *jp->edits;
		const HE::LandscapeFarTileHash hash = [&world, &packHash](HE::LandscapeTileKey k) { return world.contentHash(k, packHash); };
		HE::bakeLandscapeFar(world, hash, bo, previous.get(), *jp->table, jp->stats);
		if (!jp->stats.cancelled && !jp->table->empty())
		{
			std::error_code ec;
			fs::create_directories(jp->cacheFile.parent_path(), ec);
			const std::vector<uint8_t> bytes = jp->table->encode();
			std::ofstream(jp->cacheFile, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
			                                                     static_cast<std::streamsize>(bytes.size()));
		}
		jp->done = true;
	});
	s_tileJob = std::move(job);
	s_tileSummary = "Far image: baking...";
	return true;
}

void reset()
{
	s_project.clear();
	s_index.clear();
	s_resident.clear();
	s_measured.clear();
	s_refused.clear();
	s_summary.clear();
	stopTileJob();   // waits for a running tile bake (it is cancelled first)
	s_tileOverlay = nullptr;
	s_tileLast.reset();
	s_tileSummary.clear();
	s_tileDeferred = 0;
	s_tileBakedPainter = 0;
}

const std::string& lastSummary() { return s_summary; }

State state(AppContext& ctx)
{
	if (!usable(ctx)) return State::NoLandscape;
	const auto lands = landscapes(ctx);
	if (lands.empty()) return State::NoLandscape;
	bool anyBaked = false, anyStale = false;
	for (const auto& [e, hex] : lands)
	{
		const auto it = s_index.find(hex);
		if (it == s_index.end()) { anyStale = true; continue; }
		anyBaked = true;
		const auto m = s_measured.find(hex);
		if (m == s_measured.end() || m->second != it->second.key) anyStale = true;
	}
	return !anyBaked ? State::NotBaked : anyStale ? State::Stale : State::UpToDate;
}

bool bakeNow(AppContext& ctx, std::string* summary)
{
	if (!usable(ctx)) return false;
	const auto lands = landscapes(ctx);
	if (lands.empty()) { s_summary = "The scene has no landscape."; if (summary) *summary = s_summary; return false; }

	HE::landscape::HlodExportOptions o;
	std::vector<HlodAssetBlob> blobs;
	std::vector<HE::UUID> seen;
	std::vector<std::string> notes;
	// The live world is the source and also what the renderer goes back to.
	const HE::landscape::HlodExportStats st = HE::landscape::bakeWorldLandscapes(
		*ctx.world, ctx.world, *ctx.contentManager, *ctx.renderer, o, blobs, seen,
		[&](const std::string& s) { notes.push_back(s); HE_LOG_INFO(Editor, "LandscapeFarBake: %s", s.c_str()); });

	// Per landscape: its tiles replace whatever it had, on disk and in the index.
	std::error_code ec;
	const fs::path dir = saveDir(ctx);
	std::unordered_map<std::string, std::vector<HlodAssetBlob>> byLand;
	for (HlodAssetBlob& b : blobs)
	{
		const std::string file = fs::path(b.relPath).filename().string();
		const std::string hex = file.substr(0, 32);
		byLand[hex].push_back(std::move(b));
	}
	for (const auto& [e, hex] : lands)
	{
		const auto got = byLand.find(hex);
		if (got == byLand.end()) continue;   // refused (not an Auto Landscape, rotated, ...): it keeps the live material
		putAway(ctx, hex);
		if (const auto old = s_index.find(hex); old != s_index.end())
			for (const HlodAssetBlob& a : old->second.assets) fs::remove(dir / a.relPath, ec);
		Entry entry;
		entry.key = keyOf(*ctx.world, e, ctx.world->registry().get<TerrainComponent>(e));
		for (const HlodAssetBlob& b : got->second)
		{
			fs::create_directories((dir / b.relPath).parent_path(), ec);
			std::ofstream(dir / b.relPath, std::ios::binary)
				.write(reinterpret_cast<const char*>(b.bytes.data()), static_cast<std::streamsize>(b.bytes.size()));
			entry.assets.push_back({ b.id, b.relPath, b.type, {} });
		}
		HE::landscape::loadHlodAssets(*ctx.contentManager, got->second);
		s_resident[hex] = entry.key;
		s_measured[hex] = entry.key;
		s_index[hex] = std::move(entry);
		rebuild(ctx, e);
	}
	writeIndex(ctx);

	char line[200];
	if (st.chunks == 0)
		std::snprintf(line, sizeof(line), "Nothing was baked%s%s", notes.empty() ? "." : ": ",
		              notes.empty() ? "" : notes.back().c_str());
	else
		std::snprintf(line, sizeof(line), "%u chunk(s) of %u landscape(s) baked in %.1f s%s.",
		              st.chunks, st.terrains, st.ms / 1000.0,
		              st.skipped ? ", some landscapes were left out (see the log)" : "");
	s_summary = line;
	if (summary) *summary = s_summary;
	HE_LOG_INFO(Editor, "LandscapeFarBake: %s", s_summary.c_str());
	return st.chunks > 0;
}

void tick(AppContext& ctx, bool userBusy)
{
	if (!usable(ctx)) return;
	const std::string& project = ctx.projectManager->currentProject().path;
	if (project != s_project)
	{
		reset();
		s_project = project;
		readIndex(ctx);
		s_nextMeasure = 0.0;
	}
	tickTile(ctx, userBusy);   // every frame: a finished bake is applied the frame it ends
	const double t = now();
	if (t < s_nextMeasure) return;
	s_nextMeasure = t + 1.0;

	bool changed = false;
	for (const auto& [e, hex] : landscapes(ctx))
	{
		const uint64_t key = keyOf(*ctx.world, e, ctx.world->registry().get<TerrainComponent>(e));
		const auto m = s_measured.find(hex);
		if (m != s_measured.end() && m->second == key) continue;
		s_measured[hex] = key;
		changed = true;
		const auto res = s_resident.find(hex);
		if (res != s_resident.end() && res->second != key)
		{
			putAway(ctx, hex);   // stale: the far chunks go back to the live material
			rebuild(ctx, e);
		}
		else if (res == s_resident.end() && loadSaved(ctx, hex, key))
			rebuild(ctx, e);     // a scene opened with an up-to-date bake on disk
	}
	if (changed) s_lastChange = t;

	// Automatic: once the landscapes have been still for a few seconds.
	if (ctx.editorConfig.LandscapeFarBake != 2 || userBusy || ctx.isPlaying) return;
	if (t - s_lastChange < 4.0 || state(ctx) == State::UpToDate) return;
	uint64_t combined = 0;
	for (const auto& [hex, key] : s_measured) combined ^= key;
	if (s_refused.count(combined)) return;   // this state already made nothing; do not retry every second
	if (!bakeNow(ctx)) s_refused.insert(combined);
	else HE::Ed::notify(HE::Ed::NoteLevel::Info, "Far landscape baked", s_summary);
}
}
