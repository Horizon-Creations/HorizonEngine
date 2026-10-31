#pragma once
#include "HorizonScene/LandscapeAtlas.h"
#include "HorizonScene/LandscapeQuadtree.h"
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeStore.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <string>
#include <vector>

class HorizonWorld;

namespace HE
{

// ── The world-scale landscape of a running scene (Thema 326, step 2, part A) ──────
// Owns the pieces and runs them in order, once a frame: the camera drives the LandscapeStore (tiles in memory), the
// LandscapeAtlas follows the store (which tile in which GPU layer), and buildFrame() turns the camera's frustum into the
// patch list plus the uploads a renderer has to do first. The application owns one, like CellRuntime, and calls
// update() with the world and buildFrame() with the camera's matrices it has just rendered; the render extractor is not
// involved (so the landscape cannot touch the GI landscape table of TerrainComponents).
//
// Space: the store and the quadtree work in ABSOLUTE world coordinates (double). The frame hands the renderer origin-relative
// float positions: absolute minus the world's floating origin, which stays within a few tens of kilometres of the camera.
struct LandscapeRuntimeConfig
{
	LandscapeStoreConfig store;               // loadRadius, byte budget, ...
	int      atlasWindow = 64;                // tiles per side of the slot table, a power of two
	size_t   maxUploadsPerFrame = 8;
	float    heightMinM = 0.0f;               // metres of a stored height 0 ...
	float    heightScaleM = 3000.0f;          // ... and of 65535 above it
	// The quadtree's reach (LandscapeQuadtreeConfig): level L is drawn out to rangeFactor * its patch size. A smaller factor needs a
	// smaller loadRadius (fitsStore) and shows the coarse levels sooner; rangeFactor * morphStart has to stay >= sqrt(2) for the seams.
	float    rangeFactor = 6.0f;
	float    morphStart = 0.6f;
};

// What a renderer needs for one frame. Positions are origin-relative; heights are metres.
struct LandscapeFrame
{
	struct Patch
	{
		int      level = 0;
		uint8_t  quadrants = 15;      // which quarters to draw (LandscapePatchMesh::quadrantFirst/Count)
		bool     coarse = false;      // true: heights from the coarse world grid `gridIndex`, false: from the tile atlas
		int      gridIndex = 0;       // coarse only: LandscapeCoarseWorld::heights[gridIndex]
		float    x0 = 0.0f, z0 = 0.0f;  // corner of the node, origin-relative
		float    size = 0.0f;         // node edge in metres
		float    morphStart = 0.0f;   // distances (landscapeMorphDistance) between which the odd vertices morph, see LandscapeQuadtree.h
		float    morphEnd = 0.0f;
	};
	bool                              active = false;
	std::vector<Patch>                patches;
	std::vector<LandscapeAtlas::Upload>      uploads;       // tiles to put into the atlas before drawing
	std::vector<LandscapeAtlas::Release>     releases;
	std::vector<LandscapeAtlas::TableUpdate> tableUpdates;  // changed entries of the slot table (in order)
	int                               tableWindow = 0;
	uint16_t                          slotCapacity = 0;
	const LandscapeCoarseWorld*       coarse = nullptr;      // valid as long as the runtime is; upload once (coarseGeneration)
	uint64_t                          coarseGeneration = 0;  // changes when the runtime began again
	glm::dvec3                        origin{ 0.0 };         // absolute = relative + origin
	glm::dvec3                        cameraAbsolute{ 0.0 };
	float                             slabDistance = 0.0f;   // camera to the height slab [minY, maxY]: the vertical term of every morph distance
	float                             worldMaxX = 0.0f, worldMaxZ = 0.0f;   // absolute extent: fragments beyond are discarded
	float                             tileM = 0.0f;
	int                               tilesX = 0, tilesZ = 0;
	float                             heightMinM = 0.0f, heightScaleM = 0.0f;
	int                               tileLevels = 0;
};

class LandscapeRuntime
{
public:
	// Starts on a source (a pack, or a procedural one in tests). Derives the quadtree from the source's geometry and the
	// coarse world's first level, and checks the rules that tie the parts together: the coarse world's raster is the first
	// coarse quadtree level, the store loads far enough for the tile levels (fitsStore) and the slot table is wider than the
	// loaded disc. A broken rule is an error and the runtime stays inactive.
	bool begin(std::shared_ptr<const LandscapeTileSource> source, const LandscapeRuntimeConfig& config, std::string* error = nullptr);
	bool beginFromPack(const std::string& path, const LandscapeRuntimeConfig& config, std::string* error = nullptr);
	void reset();
	bool active() const { return m_store != nullptr; }

	// The scene's tile landscape (the first entity with a LandscapeWorldComponent) drives the runtime: it begins when the
	// component appears or changes its pack, resets when it goes. Then the camera moves the store. Main thread.
	void update(HorizonWorld& world, float dt, double budgetMs = 1.0);
	// The same without a world: the camera's absolute position and velocity.
	void updateAt(const glm::dvec3& cameraAbsolute, const glm::vec3& velocity, double budgetMs = 1.0);

	// The frame for the camera that was just rendered with `viewProjRelative` (origin-relative world to clip). `zeroToOne`: the
	// backend's clip depth range. Moves the uploads and table changes collected since the last frame into `out`.
	void buildFrame(const glm::dmat4& viewProjRelative, const glm::dvec3& origin, bool zeroToOne, LandscapeFrame& out);

	const LandscapeStore*  store() const { return m_store.get(); }
	const LandscapeAtlas*  atlas() const { return m_atlas.get(); }
	const LandscapeQuadtreeConfig& quadtree() const { return m_quadtree; }
	const LandscapeRuntimeConfig&  config() const { return m_config; }
	// How often update() tried to start from a pack (a failing pack counts once, not once a frame).
	int beginAttempts() const { return m_beginAttempts; }

private:
	bool beginWith(std::shared_ptr<const LandscapeTileSource> source, const LandscapeRuntimeConfig& config, std::string* error);

	std::shared_ptr<const LandscapeTileSource> m_source;
	LandscapeRuntimeConfig         m_config;
	LandscapeQuadtreeConfig        m_quadtree;
	std::unique_ptr<LandscapeStore> m_store;
	std::unique_ptr<LandscapeAtlas> m_atlas;
	glm::dvec3 m_camera{ 0.0 };
	glm::dvec3 m_lastCamera{ 0.0 };
	bool       m_haveCamera = false;
	bool       m_hasLastCamera = false;
	uint64_t   m_generation = 0;
	std::string m_pack;
	std::string m_failedPack;   // a pack that could not start: not tried again until the component names another
	int         m_beginAttempts = 0;
	std::vector<LandscapeAtlas::Upload>      m_pendingUploads;
	std::vector<LandscapeAtlas::Release>     m_pendingReleases;
	std::vector<LandscapeAtlas::TableUpdate> m_pendingTable;
};

} // namespace HE
