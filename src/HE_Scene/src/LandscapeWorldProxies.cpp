#include "HorizonScene/LandscapeWorldProxies.h"
#include "HorizonScene/LandscapeRuntime.h"
#include "HorizonScene/LandscapeWater.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/WaterSurface.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/LandscapeProxyComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <unordered_set>

namespace HE
{

namespace
{
	// The entity stands where the absolute position is, seen from the world's floating origin. Float is enough: the origin follows the camera, so
	// the difference is a few kilometres at most.
	glm::vec3 relativeTo(const glm::dvec3& absolute, const glm::dvec3& origin)
	{
		return glm::vec3(absolute - origin);
	}

	void place(entt::registry& reg, entt::entity e, const glm::dvec3& absolute, const glm::dvec3& origin)
	{
		const glm::vec3 p = relativeTo(absolute, origin);
		if (auto* tf = reg.try_get<TransformComponent>(e))
		{
			if (tf->position != p) { tf->position = p; tf->dirty = true; }
			return;
		}
		TransformComponent tf;
		tf.position = p;
		tf.dirty = true;
		reg.emplace<TransformComponent>(e, tf);
	}

	bool alive(const entt::registry& reg, entt::entity e)
	{
		return e != entt::null && reg.valid(e) && reg.all_of<LandscapeProxyComponent>(e);
	}

	void ensureWaterMaterial(ContentManager& cm)
	{
		if (cm.ensureResident(kEngineWaterMaterialId)) return;
		cm.loadAsset(WaterSurface::kMaterialPath);
	}
}

void LandscapeWorldProxies::forget()
{
	m_foliage.clear();
	m_water.clear();
	m_waterRevision = 0;
	m_foliageRevision = 0;
	m_stats.foliageEntities = 0;
	m_stats.waterEntities = 0;
}

void LandscapeWorldProxies::removeEntity(HorizonWorld& world, ContentManager& content, IRenderer* renderer, entt::entity e)
{
	auto& reg = world.registry();
	HE::UUID mesh{};
	if (const auto* p = reg.try_get<LandscapeProxyComponent>(e)) mesh = p->meshId;
	world.destroyEntity(e);
	++m_stats.destroyed;
	if (mesh != HE::UUID{})
	{
		if (renderer) renderer->InvalidateMesh(mesh);
		content.unloadAsset(mesh);
	}
}

void LandscapeWorldProxies::clear(HorizonWorld& world, ContentManager& content, IRenderer* renderer)
{
	if (m_world == &world)
	{
		auto& reg = world.registry();
		for (auto& kv : m_foliage) if (alive(reg, kv.second)) removeEntity(world, content, renderer, kv.second);
		for (entt::entity e : m_water) if (alive(reg, e)) removeEntity(world, content, renderer, e);
	}
	forget();
	m_world = nullptr;
}

void LandscapeWorldProxies::sync(HorizonWorld& world, const LandscapeRuntime& landscape, ContentManager& content, IRenderer* renderer)
{
	auto& reg = world.registry();
	if (m_world != &world)
	{
		// Another world than the one the entities were made in: they are gone with it (or belong to it, not to us).
		forget();
		m_world = &world;
	}
	const glm::dvec3 origin = world.origin();

	// ── Foliage ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
	std::vector<LandscapeFoliageDraw> draws;
	if (landscape.active()) landscape.foliageDraws(draws);
	const std::vector<FoliageLayer>& layers = landscape.foliage().layers();
	const uint32_t revision = landscape.foliage().revision();

	if (revision != m_foliageRevision)
	{
		// The layers changed (settings, a new list): the runtime threw its scatters away; so do we, and make them again from what it has now.
		for (auto& kv : m_foliage) if (alive(reg, kv.second)) removeEntity(world, content, renderer, kv.second);
		m_foliage.clear();
		m_foliageRevision = revision;
	}

	std::unordered_set<FoliageKey, FoliageKeyHash> wanted;
	wanted.reserve(draws.size());
	for (const LandscapeFoliageDraw& d : draws)
	{
		if (!d.store || d.layer >= layers.size()) continue;
		const FoliageKey key{ d.tile, d.layer };
		wanted.insert(key);

		entt::entity e = entt::null;
		if (auto it = m_foliage.find(key); it != m_foliage.end() && alive(reg, it->second)) e = it->second;

		if (e == entt::null)
		{
			e = world.createEntity("LandscapeFoliage");
			LandscapeProxyComponent proxy;
			proxy.kind = LandscapeProxyComponent::Kind::Foliage;
			proxy.centre = d.centre;
			reg.emplace<LandscapeProxyComponent>(e, proxy);

			FoliageComponent fc;
			fc.dirty = false;                       // FoliageSystem must not scatter it again (it has no terrain, and the store is ready)
			fc.layers.assign(1, layers[d.layer]);
			FoliageLayer& l = fc.layers[0];
			l.cachedInstances.clear();
			l.cachedInstances.shrink_to_fit();
			l.densityMask.clear();                  // a mask over a landscape's 0..1 means nothing per tile (LandscapeFoliage.h)
			l.dirty = false;
			l.store = d.store;
			reg.emplace<FoliageComponent>(e, std::move(fc));
			m_foliage[key] = e;
			++m_stats.created;
		}
		else
		{
			FoliageComponent& fc = reg.get<FoliageComponent>(e);
			if (!fc.layers.empty() && fc.layers[0].store != d.store)
			{
				fc.layers[0].store = d.store;
				++fc.revision;
			}
			reg.get<LandscapeProxyComponent>(e).centre = d.centre;
		}
		place(reg, e, d.centre, origin);
	}
	for (auto it = m_foliage.begin(); it != m_foliage.end();)
	{
		if (!wanted.count(it->first))
		{
			if (alive(reg, it->second)) removeEntity(world, content, renderer, it->second);
			it = m_foliage.erase(it);
		}
		else ++it;
	}
	m_stats.foliageEntities = m_foliage.size();

	// ── Water ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
	const std::shared_ptr<const LandscapeWaterRegion> region = landscape.active() ? landscape.waterRegion() : nullptr;
	const uint64_t waterRevision = region ? region->revision : 0;
	bool waterLost = false;   // an entity gone behind our back (a scene load into the same world): make the sheets again
	for (entt::entity e : m_water) waterLost = waterLost || !alive(reg, e);
	if (waterRevision != m_waterRevision || waterLost)
	{
		// A new region replaces the old as a whole (LandscapeWater.h): the sheets are new meshes.
		for (entt::entity e : m_water) if (alive(reg, e)) removeEntity(world, content, renderer, e);
		m_water.clear();
		m_waterRevision = waterRevision;
		if (region)
		{
			ensureWaterMaterial(content);
			for (const LandscapeWaterSheet& sheet : region->sheets)
			{
				if (sheet.surface.empty()) continue;
				StaticMeshAsset mesh = sheet.surface.mesh;   // the region is shared and immutable: the entity owns its copy
				const HE::UUID meshId = content.registerStaticMesh(std::move(mesh));
				++m_stats.meshesRegistered;

				const entt::entity e = world.createEntity("LandscapeWater");
				LandscapeProxyComponent proxy;
				proxy.kind = LandscapeProxyComponent::Kind::Water;
				proxy.centre = region->centre + glm::dvec3(sheet.surface.center.x, sheet.level, sheet.surface.center.y);
				proxy.meshId = meshId;
				proxy.revision = waterRevision;
				reg.emplace<LandscapeProxyComponent>(e, proxy);

				MaterialComponent mat;
				mat.materialAssetId = kEngineWaterMaterialId;
				reg.emplace_or_replace<MaterialComponent>(e, mat);

				MeshComponent mc;
				mc.meshAssetId = meshId;
				mc.castsShadow = false;     // a translucent sheet; its shadow would be a hard grey slab (as WaterSurface)
				mc.dirty = true;
				reg.emplace_or_replace<MeshComponent>(e, mc);

				m_water.push_back(e);
				++m_stats.created;
			}
		}
	}
	for (entt::entity e : m_water)
		if (alive(reg, e)) place(reg, e, reg.get<LandscapeProxyComponent>(e).centre, origin);
	m_stats.waterEntities = m_water.size();
}

} // namespace HE
