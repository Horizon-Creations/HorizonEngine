#pragma once
#include "HorizonScene/LandscapeCellHlod.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <unordered_map>

class HorizonWorld;
class ContentManager;
class IRenderer;

namespace HE
{

class CellStreamer;

// ── The drawing side of the cell HLOD (Thema 326, step 6) ────────────────────────────────────────────────────────────────────────────────
// LandscapeHlodTable holds one baked proxy mesh per entity cell, hlodCellsToDraw says which cells have no real content right now. This makes
// the proxies visible: one entity per cell that is to draw (MeshComponent with the cell's merged mesh, the engine's default material), made
// when the cell is selected and destroyed, mesh given back, when it is not. Like LandscapeWorldProxies' entities they carry
// LandscapeProxyComponent (Kind::Hlod): never saved, listed, replicated or picked.
//
// Two bounds keep this cheap on a 100 km world with a table of tens of thousands of cells:
//   * `drawRadius`: only cells whose square is within this many metres of the camera get an entity (and so a mesh in the content manager and
//     on the GPU). Beyond it the terrain's coarse quadtree level is all there is, which is what this plan wants of 8+ km anyway.
//   * the pass over the table runs at most every `everyFrames` frames (a cell's proxy appearing a few frames late costs nothing: it replaces
//     content that is not there, and the cell's own loading takes far longer than that).
//
// The mesh positions are absolute (LandscapeCellHlod.h): every entity therefore stands at -world.origin(), like a cell root, and is re-placed
// every sync so the floating origin can move. Call sync() once a frame after CellStreamer::update and before the transforms are propagated.
struct LandscapeHlodProxyConfig
{
	double drawRadius = 6000.0;   // m from the camera to a cell's square
	int    everyFrames = 4;       // table pass period; 1 = every frame
};

struct LandscapeHlodProxyStats
{
	size_t entities = 0;
	size_t created = 0;       // over the lifetime
	size_t destroyed = 0;
	size_t passes = 0;        // table passes made
};

class LandscapeHlodProxies
{
public:
	void setConfig(const LandscapeHlodProxyConfig& c) { m_config = c; }
	const LandscapeHlodProxyConfig& config() const { return m_config; }

	// `cameraAbsolute`: the viewpoint in absolute metres. `renderer` may be null (headless).
	void sync(HorizonWorld& world, const LandscapeHlodTable& table, const CellStreamer& streamer, const glm::dvec3& cameraAbsolute,
	          ContentManager& content, IRenderer* renderer);
	// Destroys every entity (the application closes, the table changes). A world that is already gone is not touched: pass the same one.
	void clear(HorizonWorld& world, ContentManager& content, IRenderer* renderer);

	const LandscapeHlodProxyStats& stats() const { return m_stats; }

private:
	void removeEntity(HorizonWorld& world, ContentManager& content, IRenderer* renderer, entt::entity e);

	LandscapeHlodProxyConfig m_config;
	const HorizonWorld* m_world = nullptr;
	std::unordered_map<LandscapeHlodCellKey, entt::entity, LandscapeHlodCellKeyHash> m_cells;
	unsigned m_frame = 0;
	LandscapeHlodProxyStats m_stats;
};

} // namespace HE
