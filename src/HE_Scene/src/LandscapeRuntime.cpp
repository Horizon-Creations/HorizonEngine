#include "HorizonScene/LandscapeRuntime.h"

#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/LandscapeWorldComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{
bool fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}
} // namespace

bool LandscapeRuntime::begin(std::shared_ptr<const LandscapeTileSource> source, const LandscapeRuntimeConfig& config, std::string* error)
{
	reset();
	if (!beginWith(std::move(source), config, error))
	{
		reset();
		return false;
	}
	return true;
}

bool LandscapeRuntime::beginFromPack(const std::string& path, const LandscapeRuntimeConfig& config, std::string* error)
{
	std::string err;
	auto pack = LandscapePackSource::open(path, &err);
	if (!pack) return (reset(), fail(error, "landscape pack '" + path + "': " + err));
	const bool ok = begin(std::move(pack), config, error);
	if (ok) m_pack = path;
	return ok;
}

bool LandscapeRuntime::beginWith(std::shared_ptr<const LandscapeTileSource> source, const LandscapeRuntimeConfig& configIn, std::string* error)
{
	if (!source) return fail(error, "no tile source");
	// What the shaders have room for (LandscapeShaderSource.h): eight tile mips, twelve weight channels in three pages.
	const LandscapeGeometry& geo = source->geometry();
	if (geo.heightLevels() > 8) return fail(error, "the tile has " + std::to_string(geo.heightLevels()) + " height mips: the shader holds 8");
	if (geo.channels > 12) return fail(error, "the tile has " + std::to_string(geo.channels) + " weight channels: the shader holds 12 (three pages)");
	const LandscapeCoarseWorld* coarse = source->coarse();
	if (!coarse || !coarse->valid()) return fail(error, "the tile source has no coarse world (a version 1 pack): the far levels have no data");
	if (coarse->heights.size() > 16) return fail(error, "the coarse world has " + std::to_string(coarse->heights.size()) + " grids: the shader holds 16");
	const LandscapeGeometry& g = source->geometry();

	// The pack says what its heights mean; the config's numbers are for a source that does not.
	LandscapeRuntimeConfig config = configIn;
	{
		float mn = 0.0f, sc = 0.0f;
		if (source->heightRange(mn, sc)) { config.heightMinM = mn; config.heightScaleM = sc; }
	}
	LandscapeQuadtreeConfig q = LandscapeQuadtreeConfig::fromGeometry(g, source->tilesX(), source->tilesZ());
	// The tile levels are exactly those below the coarse world's raster: one number, taken from the pack.
	q.tileLevels = coarse->heightLevel - 1;
	if (q.tileLevels < 0) return fail(error, "the coarse world starts at tile level 0: no tile level would be drawn");
	q.rangeFactor = config.rangeFactor;
	q.morphStart = config.morphStart;
	if (q.rangeFactor * q.morphStart < 1.4143f) return fail(error, "rangeFactor * morphStart is below sqrt(2): the morph would leave cracks between levels");
	q.minY = config.heightMinM;
	q.maxY = config.heightMinM + config.heightScaleM;
	if (std::abs(q.cellSize(q.tileLevels + 1) - coarse->cellM(0)) > 1e-3f * coarse->cellM(0))
		return fail(error, "the coarse world's raster does not equal the first coarse quadtree level's cell size");
	if (!q.fitsStore(config.store.loadRadius))
		return fail(error, "loadRadius " + std::to_string(config.store.loadRadius) + " m is below the " + std::to_string(q.minLoadRadius()) +
		                       " m the fine levels need");
	const int needWindow = 2 * static_cast<int>(std::ceil(config.store.loadRadius * config.store.keepFactor / g.tileM)) + 2;
	if (config.atlasWindow < needWindow)
		return fail(error, "atlasWindow " + std::to_string(config.atlasWindow) + " is below the " + std::to_string(needWindow) + " tiles the loaded disc spans");

	m_source = std::move(source);
	m_config = config;
	m_quadtree = q;
	m_store = std::make_unique<LandscapeStore>(m_source, config.store);
	m_atlas = std::make_unique<LandscapeAtlas>(m_store->stats().maxTiles, config.atlasWindow);
	++m_generation;
	return true;
}

void LandscapeRuntime::reset()
{
	m_store.reset();
	m_atlas.reset();
	m_source.reset();
	m_pack.clear();
	m_pendingUploads.clear();
	m_pendingReleases.clear();
	m_pendingTable.clear();
	m_haveCamera = m_hasLastCamera = false;
}

void LandscapeRuntime::update(HorizonWorld& world, float dt, double budgetMs)
{
	// The scene says whether there is a tile landscape: the first enabled LandscapeWorldComponent.
	const LandscapeWorldComponent* comp = nullptr;
	for (auto e : world.registry().view<LandscapeWorldComponent>())
	{
		const auto& c = world.registry().get<LandscapeWorldComponent>(e);
		if (c.enabled && !c.pack.empty()) { comp = &c; break; }
	}
	if (!comp) { if (active()) reset(); m_failedPack.clear(); return; }
	if (!active() && comp->pack == m_failedPack) return;       // it failed before; a changed path is a new try
	if (!active() || m_pack != comp->pack)
	{
		LandscapeRuntimeConfig cfg;
		cfg.store.loadRadius = comp->loadRadius;
		cfg.store.byteBudget = static_cast<size_t>(comp->budgetMB) * 1024 * 1024;
		cfg.heightMinM = comp->heightMinM;
		cfg.heightScaleM = comp->heightScaleM;
		std::string err;
		++m_beginAttempts;
		if (!beginFromPack(comp->pack, cfg, &err))
		{
			HE_LOG_WARN(World, "Landscape: %s", err.c_str());
			m_failedPack = comp->pack;
			return;
		}
		m_failedPack.clear();
	}
	if (!active()) return;

	for (auto e : world.registry().view<TransformComponent, CameraComponent>())
	{
		const glm::dvec3 camera = glm::dvec3(worldPositionOf(world, e)) + world.origin();
		// A jump (teleport, respawn) is not a speed.
		glm::vec3 v(0.0f);
		if (m_hasLastCamera && dt > 0.0f)
		{
			v = glm::vec3((camera - m_lastCamera) / static_cast<double>(dt));
			if (glm::length(v) > 300.0f) v = glm::vec3(0.0f);
		}
		updateAt(camera, v, budgetMs);
		m_lastCamera = camera;
		m_hasLastCamera = true;
		return;
	}
}

void LandscapeRuntime::updateAt(const glm::dvec3& cameraAbsolute, const glm::vec3& velocity, double budgetMs)
{
	if (!active()) return;
	m_camera = cameraAbsolute;
	m_haveCamera = true;
	LandscapeAnchor a;
	a.position = cameraAbsolute;
	a.velocity = velocity;
	m_store->update({ a }, budgetMs);

	std::vector<LandscapeAtlas::Upload> up;
	std::vector<LandscapeAtlas::Release> rel;
	std::vector<LandscapeAtlas::TableUpdate> tab;
	m_atlas->sync(*m_store, up, rel, tab, m_config.maxUploadsPerFrame);
	for (auto& u : up) m_pendingUploads.push_back(std::move(u));
	for (auto& r : rel) m_pendingReleases.push_back(r);
	for (auto& t : tab) m_pendingTable.push_back(t);
}

void LandscapeRuntime::buildFrame(const glm::dmat4& viewProjRelative, const glm::dvec3& origin, bool zeroToOne, LandscapeFrame& out)
{
	out = LandscapeFrame{};
	if (!active() || !m_haveCamera) return;
	const LandscapeCoarseWorld* coarse = m_source->coarse();

	// The frustum in absolute space: n . (p - o) + d = n . p + (d - n . o)
	LandscapeFrustum fr = LandscapeFrustum::fromViewProjection(viewProjRelative, zeroToOne);
	for (glm::dvec4& p : fr.planes) p.w -= p.x * origin.x + p.y * origin.y + p.z * origin.z;

	const LandscapeAtlas* atlas = m_atlas.get();
	const auto patches = selectLandscapePatches(m_quadtree, m_camera, &fr, [atlas](int tx, int tz) { return atlas->contains({ tx, tz }); });

	out.active = true;
	out.patches.reserve(patches.size());
	for (const LandscapePatch& p : patches)
	{
		LandscapeFrame::Patch d;
		d.level = p.level;
		d.quadrants = p.quadrants;
		d.coarse = p.level > m_quadtree.tileLevels;
		d.gridIndex = d.coarse ? std::min(p.level - (m_quadtree.tileLevels + 1), static_cast<int>(coarse->heights.size()) - 1) : 0;
		d.x0 = static_cast<float>(p.x0 - origin.x);
		d.z0 = static_cast<float>(p.z0 - origin.z);
		d.size = p.size;
		const float lo = m_quadtree.rangeOf(p.level - 1), hi = m_quadtree.rangeOf(p.level);
		d.morphStart = lo + (hi - lo) * m_quadtree.morphStart;
		d.morphEnd = hi;
		out.patches.push_back(d);
	}
	out.uploads = std::move(m_pendingUploads);
	out.releases = std::move(m_pendingReleases);
	out.tableUpdates = std::move(m_pendingTable);
	m_pendingUploads.clear();
	m_pendingReleases.clear();
	m_pendingTable.clear();
	out.tableWindow = m_atlas->window();
	out.slotCapacity = static_cast<uint16_t>(m_atlas->capacity());
	out.coarse = coarse;
	out.coarseGeneration = m_generation;
	out.origin = origin;
	out.cameraAbsolute = m_camera;
	out.slabDistance = landscapeMorphDistance(m_quadtree, m_camera, m_camera.x, m_camera.z);   // dx = dz = 0: the vertical term alone
	out.worldMaxX = static_cast<float>(m_quadtree.tilesX * static_cast<double>(m_quadtree.tileM));
	out.worldMaxZ = static_cast<float>(m_quadtree.tilesZ * static_cast<double>(m_quadtree.tileM));
	out.tileM = m_quadtree.tileM;
	out.tilesX = m_quadtree.tilesX;
	out.tilesZ = m_quadtree.tilesZ;
	out.heightMinM = m_config.heightMinM;
	out.heightScaleM = m_config.heightScaleM;
	out.tileLevels = m_quadtree.tileLevels;
}

} // namespace HE
