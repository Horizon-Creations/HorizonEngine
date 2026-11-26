#pragma once
#include "HorizonScene/LandscapeFoliage.h"
#include "HorizonScene/LandscapeSource.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

class HorizonWorld;
class ContentManager;
class IRenderer;

namespace HE
{

class LandscapeRuntime;

// ── Foliage and water of a tile landscape, drawn by the engine's own paths (Thema 326, step 2 part B) ─────────────────────────────────────
// LandscapeRuntime holds the foliage of the tiles around the camera (one immutable FoliageStore per tile and layer) and the water of a region
// (sheets with ready meshes). The renderers draw foliage from FoliageComponents and water from ordinary mesh entities with the engine water
// material, on every backend already. So the draw contract is met here, once, in the scene, and no backend has anything to add:
//
//   foliage  one entity per (tile, layer): a FoliageComponent with that one layer, whose `store` is the tile's, standing at
//            tile centre - floating origin. RenderExtractor takes it through the same bucket logic as the single landscape.
//   water    one entity per sheet of the region: MeshComponent + MaterialComponent (engine water), standing at
//            region centre + (surface.center.x, level, surface.center.y) - floating origin, the mesh owned by the entity.
//
// The entities carry LandscapeProxyComponent, which isTerrainGenerated answers for: never saved, listed, replicated or picked. sync() makes
// them match the runtime and moves them with the floating origin; call it once a frame after LandscapeRuntime::update and before the
// transforms are propagated. A world that is replaced (a scene swap) is noticed: its entities are gone with it.
//
// NOT here: the tessellated water (plain water is drawn), the waves' influence on buoyancy or the underwater camera (those read a water::Field
// of a TerrainComponent, which a tile landscape does not have; LandscapeRuntime::waterSurfaceAt answers for it), shadows of the sheets.
struct LandscapeProxyStats
{
	size_t foliageEntities = 0;
	size_t waterEntities = 0;
	size_t created = 0;       // entities made, over the lifetime
	size_t destroyed = 0;
	size_t meshesRegistered = 0;
};

class LandscapeWorldProxies
{
public:
	~LandscapeWorldProxies() = default;

	// `renderer` may be null (a headless run): water meshes are then not invalidated in a cache that does not exist.
	void sync(HorizonWorld& world, const LandscapeRuntime& landscape, ContentManager& content, IRenderer* renderer);
	// Destroys every entity (the landscape went, the application closes). A world that is already gone is not touched: pass the same one.
	void clear(HorizonWorld& world, ContentManager& content, IRenderer* renderer);

	const LandscapeProxyStats& stats() const { return m_stats; }

private:
	struct FoliageKey
	{
		LandscapeTileKey tile;
		size_t           layer = 0;
		bool operator==(const FoliageKey& o) const { return tile == o.tile && layer == o.layer; }
	};
	struct FoliageKeyHash
	{
		size_t operator()(const FoliageKey& k) const { return LandscapeTileKeyHash()(k.tile) ^ (k.layer * 2654435761u); }
	};

	void forget();
	void removeEntity(HorizonWorld& world, ContentManager& content, IRenderer* renderer, entt::entity e);

	const HorizonWorld* m_world = nullptr;
	std::unordered_map<FoliageKey, entt::entity, FoliageKeyHash> m_foliage;
	std::vector<entt::entity> m_water;
	uint64_t m_waterRevision = 0;
	uint32_t m_foliageRevision = 0;
	LandscapeProxyStats m_stats;
};

} // namespace HE
