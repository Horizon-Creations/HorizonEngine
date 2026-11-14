#include "HorizonScene/LandscapeRuntime.h"

#include "HorizonScene/LandscapeGpuLayout.h"
#include "HorizonScene/LandscapePatchMesh.h"

#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/LandscapeWorldComponent.h"
#include "HorizonScene/Components/NavAgentComponent.h"
#include "HorizonScene/Components/NavMeshComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/TransformHierarchy.h"

#include <Diagnostics/Log.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

#include <algorithm>
#include <cmath>

namespace HE
{
static_assert(PhysicsWorld::kGroundOwner == 0xFFFFFFFEu, "LandscapeRuntime's default ground owner is PhysicsWorld::kGroundOwner");
namespace
{
// A key over what a layer's scatter depends on (not the runtime state, not the mask, which a tile landscape ignores). FoliageSystem does not
// bump FoliageComponent::revision for an entity without a TerrainComponent, so the runtime keeps its own.
uint64_t foliageKey(const std::vector<FoliageLayer>& layers, bool visible)
{
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](const void* d, size_t n) {
		const uint8_t* p = static_cast<const uint8_t*>(d);
		for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
	};
	mix(&visible, sizeof(visible));
	const size_t n = layers.size();
	mix(&n, sizeof(n));
	for (const FoliageLayer& l : layers)
	{
		mix(&l.visible, sizeof(l.visible));
		mix(&l.meshAssetId, sizeof(l.meshAssetId));
		mix(&l.materialAssetId, sizeof(l.materialAssetId));
		mix(&l.density, sizeof(l.density));
		mix(&l.seed, sizeof(l.seed));
		mix(&l.minScale, sizeof(l.minScale));
		mix(&l.maxScale, sizeof(l.maxScale));
		mix(&l.drawDistance, sizeof(l.drawDistance));
		mix(&l.bucketSize, sizeof(l.bucketSize));
		mix(&l.slopeMin, sizeof(l.slopeMin));
		mix(&l.slopeMax, sizeof(l.slopeMax));
		mix(&l.heightMin, sizeof(l.heightMin));
		mix(&l.heightMax, sizeof(l.heightMax));
		mix(&l.weightLayer, sizeof(l.weightLayer));
		const size_t lods = l.lods.size();
		mix(&lods, sizeof(lods));
		for (const FoliageLod& lod : l.lods) { mix(&lod.meshAssetId, sizeof(lod.meshAssetId)); mix(&lod.materialAssetId, sizeof(lod.materialAssetId)); mix(&lod.distance, sizeof(lod.distance)); mix(&lod.density, sizeof(lod.density)); }
	}
	return h | 1;   // never 0
}

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
	m_renderStatic.reset();
	return true;
}

void LandscapeRuntime::reset()
{
	if (m_physics) m_ground.clear(*m_physics);
	m_foliage.clear();
	m_water.clear();
	m_gi.clear();
	if (m_nav) m_nav->clear();
	m_foliageKey = 0;
	m_store.reset();
	m_atlas.reset();
	m_source.reset();
	m_pack.clear();
	m_pendingUploads.clear();
	m_pendingReleases.clear();
	m_pendingTable.clear();
	m_haveCamera = m_hasLastCamera = false;
	m_renderStatic.reset();
}

void LandscapeRuntime::update(HorizonWorld& world, float dt, double budgetMs, const glm::dvec3* cameraOverride)
{
	// The scene says whether there is a tile landscape: the first enabled LandscapeWorldComponent.
	const LandscapeWorldComponent* comp = nullptr;
	for (auto e : world.registry().view<LandscapeWorldComponent>())
	{
		const auto& c = world.registry().get<LandscapeWorldComponent>(e);
		if (c.enabled && !c.pack.empty()) { comp = &c; m_landscapeEntity = static_cast<uint32_t>(e); break; }
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
		cfg.textureGroup = comp->textureGroup;
		cfg.textureMeters = comp->textureMeters;
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

	const auto run = [&](const glm::dvec3& camera)
	{
		// A jump (teleport, respawn) is not a speed.
		glm::vec3 v(0.0f);
		if (m_hasLastCamera && dt > 0.0f)
		{
			v = glm::vec3((camera - m_lastCamera) / static_cast<double>(dt));
			if (glm::length(v) > 300.0f) v = glm::vec3(0.0f);
		}
		// the landscape entity's foliage layers, scattered per tile; no component (or removed) means none
		const FoliageComponent* fol = world.registry().try_get<FoliageComponent>(static_cast<entt::entity>(m_landscapeEntity));
		if (fol) { const uint64_t key = foliageKey(fol->layers, fol->visible); if (key != m_foliageKey) { setFoliageLayers(fol->visible ? fol->layers : std::vector<FoliageLayer>{}); m_foliageKey = key; } }
		else if (m_foliageKey != 0) { setFoliageLayers({}); m_foliageKey = 0; }
		if (m_nav)
		{
			// who walks: the agents, and where the moving ones are going
			m_navAnchors.clear();
			for (auto [e, agent, tc] : world.registry().view<NavAgentComponent, TransformComponent>().each())
			{
				m_navAnchors.push_back(glm::dvec3(worldPositionOf(world, e)) + world.origin());
				if (agent.moving) m_navAnchors.push_back(glm::dvec3(agent.targetPos) + world.origin());
			}
		}
		updateAt(camera, v, budgetMs, world.origin());
		if (m_nav)
			for (auto [e, nmc] : world.registry().view<NavMeshComponent>().each())
			{
				if (nmc.navMesh != m_nav->navMesh()) m_nav->attach(nmc);
				break;
			}
		m_lastCamera = camera;
		m_hasLastCamera = true;
	};
	if (cameraOverride) { run(*cameraOverride); return; }
	for (auto e : world.registry().view<TransformComponent, CameraComponent>())
	{
		run(glm::dvec3(worldPositionOf(world, e)) + world.origin());
		return;
	}
}

void LandscapeRuntime::updateAt(const glm::dvec3& cameraAbsolute, const glm::vec3& velocity, double budgetMs, const glm::dvec3& origin)
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

	m_foliage.update(*m_store, { cameraAbsolute }, m_config.heightMinM, m_config.heightScaleM);
	m_water.update(*m_store, cameraAbsolute, { m_config.heightMinM, m_config.heightScaleM }, m_source->water());
	m_gi.update(*m_store, cameraAbsolute, { m_config.heightMinM, m_config.heightScaleM });
	if (m_nav) m_nav->update(*m_store, m_navAnchors, m_config.heightMinM, m_config.heightScaleM);

	if (m_physics)
	{
		std::vector<glm::dvec3> anchors = m_physicsAnchors;
		anchors.push_back(cameraAbsolute);
		LandscapePhysicsConfig pc = m_ground.config();
		pc.reportAs = m_landscapeEntity;
		m_ground.setConfig(pc);
		m_ground.update(*m_physics, *m_store, anchors, origin, m_config.heightMinM, m_config.heightScaleM);
	}
}

void LandscapeRuntime::tileChanged(const std::vector<LandscapeTileKey>& keys)
{
	if (!active()) return;
	m_store->invalidate(keys);
}

void LandscapeRuntime::setNavigation(const LandscapeNavConfig* config)
{
	if (!config) { m_nav.reset(); m_navAnchors.clear(); return; }
	m_nav = std::make_unique<LandscapeNavigation>(*config);
}

void LandscapeRuntime::setFoliageLayers(const std::vector<FoliageLayer>& layers)
{
	m_foliage.setLayers(layers);
}

void LandscapeRuntime::setPhysics(PhysicsWorld* physics, const LandscapePhysicsConfig& config)
{
	if (m_physics && m_physics != physics) m_ground.clear(*m_physics);
	m_physics = physics;
	m_ground = LandscapePhysics(config);
}

bool LandscapeRuntime::groundHolds(const glm::vec3& worldPosition, const glm::dvec3& origin) const
{
	return m_physics && m_store && m_ground.holdsAt(worldPosition, origin, *m_store);
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
		d.ix = p.ix;
		d.iz = p.iz;
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

std::array<glm::vec4, 12> LandscapeRuntimeConfig::defaultLayerColors()
{
	// grass, rock, sand, snow, dirt, forest floor, then six neighbours that are told apart on screen
	return { glm::vec4(0.22f, 0.40f, 0.12f, 1), glm::vec4(0.42f, 0.40f, 0.38f, 1), glm::vec4(0.76f, 0.68f, 0.45f, 1), glm::vec4(0.92f, 0.94f, 0.96f, 1),
	         glm::vec4(0.35f, 0.26f, 0.17f, 1), glm::vec4(0.12f, 0.26f, 0.10f, 1), glm::vec4(0.55f, 0.50f, 0.30f, 1), glm::vec4(0.30f, 0.30f, 0.34f, 1),
	         glm::vec4(0.60f, 0.35f, 0.25f, 1), glm::vec4(0.25f, 0.45f, 0.45f, 1), glm::vec4(0.70f, 0.70f, 0.20f, 1), glm::vec4(0.50f, 0.20f, 0.50f, 1) };
}

void LandscapeRuntime::buildRenderFrame(const glm::dmat4& viewProjRelative, const glm::dvec3& origin, bool zeroToOne, LandscapeRenderFrame& out)
{
	out = LandscapeRenderFrame{};
	LandscapeFrame f;
	buildFrame(viewProjRelative, origin, zeroToOne, f);
	if (!f.active) return;
	const LandscapeCoarseWorld* coarse = f.coarse;
	if (!coarse || !coarse->valid()) return;

	if (!m_renderStatic || m_renderStatic->generation != f.coarseGeneration)
	{
		auto w = std::make_shared<LandscapeRenderStatic>();
		const LandscapeGeometry& g = m_source->geometry();
		const LandscapeTileImageLayout tl = LandscapeTileImageLayout::forGeometry(g);
		const LandscapeCoarseImageLayout cl = LandscapeCoarseImageLayout::forCoarse(*coarse);
		w->generation = f.coarseGeneration;
		w->tileM = f.tileM;
		w->heightMinM = f.heightMinM;
		w->heightScaleM = f.heightScaleM;
		w->worldMaxX = f.worldMaxX;
		w->worldMaxZ = f.worldMaxZ;
		w->tilesX = f.tilesX;
		w->tilesZ = f.tilesZ;
		w->hCells = g.hCells;
		w->wTexels = g.wTexels;
		w->tileLevels = f.tileLevels;
		w->tableWindow = f.tableWindow;
		w->slotCapacity = f.slotCapacity;
		w->heightW = tl.heightW;
		w->heightH = tl.heightH;
		w->weightW = tl.weightW;
		w->weightH = tl.weightH;
		w->weightPages = tl.weightPages;
		w->heightLevels = static_cast<int>(std::min<size_t>(tl.heightRect.size(), 8));
		for (int i = 0; i < w->heightLevels; ++i)
		{
			w->heightRect[i][0] = tl.heightRect[static_cast<size_t>(i)].x;
			w->heightRect[i][1] = tl.heightRect[static_cast<size_t>(i)].y;
			w->heightRect[i][2] = tl.heightRect[static_cast<size_t>(i)].w;
			w->heightRect[i][3] = tl.heightRect[static_cast<size_t>(i)].h;
		}
		w->coarseW = cl.width;
		w->coarseH = cl.height;
		w->coarseGridCount = static_cast<int>(std::min<size_t>(cl.grids.size(), 16));
		for (int i = 0; i < w->coarseGridCount; ++i)
		{
			w->coarseGrid[i][0] = cl.grids[static_cast<size_t>(i)].w;
			w->coarseGrid[i][1] = cl.grids[static_cast<size_t>(i)].h;
			w->coarseGrid[i][2] = cl.grids[static_cast<size_t>(i)].rowOffset;
		}
		w->coarseWeightW = cl.weightW;
		w->coarseWeightH = cl.weightH;
		landscapePackCoarse(*coarse, cl, w->coarseHeights, w->coarseWeights);
		for (int i = 0; i < 12; ++i)
			for (int c = 0; c < 4; ++c) w->layerColor[i][c] = m_config.layerColors[static_cast<size_t>(i)][c];
		if (m_config.textureGroup >= 0 && m_config.textureGroup < kAutoLandscapeGroupCount)
		{
			const AutoLandscapeGroup& grp = autoLandscapeGroup(static_cast<AutoLandscapeGroupId>(m_config.textureGroup));
			w->albedoArrayPath = grp.arrayPaths[0];
			w->normalArrayPath = grp.arrayPaths[1];
			w->maskArrayPath = grp.arrayPaths[2];
			w->textureMeters = m_config.textureMeters > 0.0f ? m_config.textureMeters : 8.0f;
		}
		const LandscapePatchMesh mesh = LandscapePatchMesh::build(64);
		w->patchCells = mesh.cells;
		w->patchIndices = mesh.indices;
		for (int q = 0; q < 4; ++q)
		{
			w->quadrantFirst[q] = mesh.quadrantFirst[q];
			w->quadrantCount[q] = mesh.quadrantCount[q];
		}
		m_renderStatic = std::move(w);
	}

	out.active = true;
	out.world = m_renderStatic;
	out.origin = f.origin;
	out.cameraRel = glm::vec3(f.cameraAbsolute - f.origin);
	out.slabDistance = f.slabDistance;
	out.patches.reserve(f.patches.size());
	for (const LandscapeFrame::Patch& p : f.patches)
	{
		LandscapeRenderPatch d;
		d.level = p.level;
		d.quadrants = p.quadrants;
		d.coarse = p.coarse;
		d.gridIndex = p.gridIndex;
		d.cornerX = p.ix * 64;
		d.cornerZ = p.iz * 64;
		d.x0 = p.x0;
		d.z0 = p.z0;
		d.size = p.size;
		d.morphStart = p.morphStart;
		d.morphEnd = p.morphEnd;
		out.patches.push_back(d);
	}
	const LandscapeTileImageLayout tl = LandscapeTileImageLayout::forGeometry(m_source->geometry());
	out.uploads.reserve(f.uploads.size());
	for (LandscapeAtlas::Upload& u : f.uploads)
	{
		LandscapeRenderUpload r;
		r.slot = u.slot;
		if (u.tile) landscapePackTile(*u.tile, tl, r.heights, r.pages);
		u.tile.reset();   // the store's tile is not held past the copy
		out.uploads.push_back(std::move(r));
	}
	out.tableUpdates.reserve(f.tableUpdates.size());
	for (const LandscapeAtlas::TableUpdate& t : f.tableUpdates) out.tableUpdates.push_back({ t.index, t.slot });
}

LandscapeProvider LandscapeRuntime::renderProvider(std::function<glm::dvec3()> origin)
{
	return [this, origin = std::move(origin)](const glm::dmat4& vp, bool zeroToOne, LandscapeRenderFrame& out) {
		buildRenderFrame(vp, origin ? origin() : glm::dvec3(0.0), zeroToOne, out);
	};
}

LandscapeProvider LandscapeRuntime::renderProvider(const HorizonWorld& world)
{
	return renderProvider([&world]() { return world.origin(); });
}

} // namespace HE
