#pragma once
#include "HorizonScene/LandscapeStore.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

class PhysicsWorld;

namespace HE
{

// ── Ground for the world-scale landscape (Thema 326, step 3) ─────────────────────
// The single landscape has ONE Jolt height field for its whole area. A 100 km world cannot: its height field would be 50 000
// samples to a side. Instead every loaded tile near an anchor (the camera, a player, a vehicle) gets a static height field of
// its own, in the Terrain channel, and loses it when the anchor is far. A tile is 257 x 257 samples at 2 m (level 0); the
// neighbours share their border samples, so two fields meet without a gap.
//
// Anchors keep ground under them within `radius` (1.5 km), and ground stays until every anchor is beyond radius * keepFactor,
// the same hysteresis the store uses. Ground is built from the store's resident tiles only, nearest first and at most
// `maxBuildsPerUpdate` per update (a shape build is real work on the stepping thread; how much has not been measured).
//
// Order in a frame: LandscapeStore::update, then update() here, then PhysicsWorld::step. A body that steps over a tile whose
// ground is not there yet falls through it: holdsAt() is the test for PhysicsWorld::setRegionHold that keeps such bodies out of
// the simulation until it is (the same job CellStreamer does for cells).
//
// Space. Tiles are in the absolute world; Jolt works origin-relative. A field is created at (absolute corner - origin); after that
// shiftOrigin moves it with every other body, so the caller only has to pass the CURRENT world origin each update.
struct LandscapePhysicsConfig
{
	float    radius = 1500.0f;
	float    keepFactor = 1.25f;
	int      level = 0;                    // tile mip the ground is built from (0 = the tile's finest raster)
	size_t   maxBuildsPerUpdate = 2;
	uint32_t reportAs = 0xFFFFFFFEu;       // the entity a hit on the ground reports (the landscape entity); the default is PhysicsWorld::kGroundOwner,
	                                       // NOT kNoEntity, which every default raycast ignores
};

struct LandscapePhysicsStats
{
	size_t   fields = 0;
	uint64_t built = 0;
	uint64_t removed = 0;
	uint64_t deferred = 0;    // wanted but not built this update (budget, or the tile is not resident yet)
	uint64_t failed = 0;      // Jolt refused the shape or the body
};

class LandscapePhysics
{
public:
	explicit LandscapePhysics(LandscapePhysicsConfig config = {}) : m_config(config) {}

	// `anchors`: absolute positions that need ground. `heightMinM` / `heightScaleM`: metres of a stored height 0 and of 65535 above it.
	void update(PhysicsWorld& physics, const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, const glm::dvec3& origin,
	            float heightMinM, float heightScaleM);

	// Takes all ground out of `physics` (the landscape ended, the scene changed).
	void clear(PhysicsWorld& physics);

	bool hasGround(LandscapeTileKey key) const { return m_fields.count(pack(key)) != 0; }
	size_t count() const { return m_fields.size(); }

	// PhysicsWorld::setRegionHold's test: true for a position (origin-relative, as Jolt has it) over the world whose tile has no ground
	// yet. A position outside the world is never held (nothing to wait for). `tileM` and the tile counts come from the store's source.
	bool holdsAt(const glm::vec3& worldPosition, const glm::dvec3& origin, const LandscapeStore& store) const;

	const LandscapePhysicsStats& stats() const { return m_stats; }
	const LandscapePhysicsConfig& config() const { return m_config; }
	void setConfig(const LandscapePhysicsConfig& config) { m_config = config; }

	static uint64_t pack(LandscapeTileKey k) { return (static_cast<uint64_t>(static_cast<uint32_t>(k.z)) << 32) | static_cast<uint32_t>(k.x); }

private:
	LandscapePhysicsConfig m_config;
	std::unordered_map<uint64_t, LandscapeTileKey> m_fields;   // by packed key
	LandscapePhysicsStats m_stats;
};

} // namespace HE
