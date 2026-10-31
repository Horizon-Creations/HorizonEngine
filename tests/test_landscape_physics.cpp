// HE::LandscapePhysics and PhysicsWorld::addGroundField (Thema 326, step 3, the part that needs no GPU): every loaded tile
// near an anchor has a Jolt height field of its own, they meet without a gap, they go when the anchor leaves, they move with a
// floating origin, and nothing falls through a tile that is not there yet. Real Jolt, no timing.
#include "doctest.h"
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/FloatingOrigin.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapePhysics.h>
#include <HorizonScene/PhysicsWorld.h>
#include <Physics/CollisionLayers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace HE;

namespace
{
constexpr float kMin = 0.0f, kScale = 1000.0f;

LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.hCells = 32;      // 33 x 33 samples, 16 m apart
	g.wTexels = 16;
	g.channels = 4;
	return g;
}

std::shared_ptr<ProceduralLandscapeSource> world(int tiles, bool small = true)
{
	ProceduralLandscapeParams p;
	if (small) p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = tiles;
	return std::make_shared<ProceduralLandscapeSource>(p);
}

bool settle(LandscapeStore& store, const glm::dvec3& cam, int ms = 20000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		store.update({ { cam } }, 5.0);
		if (store.settled()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// The ground height under an absolute (x, z) as the tile says it, at lattice points of the tile's finest raster.
float tileHeight(const ProceduralLandscapeSource& src, double x, double z)
{
	const LandscapeGeometry& g = src.geometry();
	const int tx = std::min(static_cast<int>(x / g.tileM), src.tilesX() - 1), tz = std::min(static_cast<int>(z / g.tileM), src.tilesZ() - 1);
	const LandscapeTile t = src.generate({ tx, tz });
	const double cell = g.tileM / g.hCells;
	const int ix = static_cast<int>(std::lround((x - tx * g.tileM) / cell)), iz = static_cast<int>(std::lround((z - tz * g.tileM) / cell));
	return kMin + t.heights[0][static_cast<size_t>(iz) * g.heightDim(0) + ix] * (kScale / 65535.0f);
}

// A ray straight down onto the ground at an ORIGIN-RELATIVE (x, z).
PhysicsWorld::RaycastHit down(const PhysicsWorld& phys, float x, float z)
{
	return phys.raycast({ x, 5000.0f, z }, { 0.0f, -1.0f, 0.0f }, 10000.0f);
}
} // namespace

TEST_CASE("physics ground field: a flat field answers a ray, a key is used once, bad input is refused")
{
	PhysicsWorld phys;
	HorizonWorld w;
	phys.initialize(w);
	std::vector<float> flat(33 * 33, 12.5f);
	CHECK(phys.groundFieldCount() == 0);
	REQUIRE(phys.addGroundField(7, flat.data(), 33, { 100.0f, 0.0f, 200.0f }, 2.0f, 2.0f, 42));
	CHECK(phys.hasGroundField(7));
	CHECK(phys.groundFieldCount() == 1);
	CHECK(phys.bodyCount() == 1);                                           // it counts against Jolt's body table
	const auto hit = down(phys, 120.0f, 220.0f);
	REQUIRE(hit.hit);
	CHECK(hit.point.y == doctest::Approx(12.5f).epsilon(1e-3));
	CHECK(hit.entityId == 42u);                                              // a hit reports the landscape entity
	CHECK(hit.layer == HE::CollisionLayerConfig::kTerrain);
	// ground nobody owns is found by an ordinary ray, and reports no real entity (kNoEntity would hide it from every default query)
	REQUIRE(phys.addGroundField(9, flat.data(), 33, { 1000.0f, 0.0f, 1000.0f }, 2.0f, 2.0f));
	const auto unowned = down(phys, 1010.0f, 1010.0f);
	REQUIRE(unowned.hit);
	CHECK(unowned.entityId == PhysicsWorld::kGroundOwner);
	CHECK(unowned.entityId != PhysicsWorld::kNoEntity);
	REQUIRE(phys.removeGroundField(9));
	CHECK_FALSE(down(phys, 99.0f, 220.0f).hit);                              // left of the field
	CHECK_FALSE(down(phys, 120.0f, 267.0f).hit);                             // 100 + 66 and 200 + 66 are the far edges
	CHECK_FALSE(phys.addGroundField(7, flat.data(), 33, { 0, 0, 0 }, 2.0f, 2.0f));      // key in use
	CHECK_FALSE(phys.addGroundField(8, nullptr, 33, { 0, 0, 0 }, 2.0f, 2.0f));
	CHECK_FALSE(phys.addGroundField(8, flat.data(), 2, { 0, 0, 0 }, 2.0f, 2.0f));        // fewer than 3 samples
	CHECK_FALSE(phys.addGroundField(8, flat.data(), 33, { 0, 0, 0 }, 0.0f, 2.0f));       // no spacing
	CHECK(phys.groundFieldCount() == 1);
	REQUIRE(phys.removeGroundField(7));
	CHECK_FALSE(phys.removeGroundField(7));
	CHECK_FALSE(down(phys, 120.0f, 220.0f).hit);
	CHECK(phys.groundFieldCount() == 0);
	REQUIRE(phys.addGroundField(7, flat.data(), 33, { 100.0f, 0.0f, 200.0f }, 2.0f, 2.0f));   // and may be used again
	phys.clear();
	CHECK(phys.groundFieldCount() == 0);                                     // clear() takes the ground too
}

TEST_CASE("landscape physics: the ground under a ray is the tile's own height, and neighbouring tiles meet without a gap")
{
	auto src = world(6);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2500.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(1536.0, 600.0, 1536.0);          // on the corner of four tiles
	REQUIRE(settle(store, cam));

	PhysicsWorld phys;
	HorizonWorld w;
	phys.initialize(w);
	LandscapePhysicsConfig pc;
	pc.radius = 1200.0f;
	pc.maxBuildsPerUpdate = 100;
	LandscapePhysics ground(pc);
	ground.update(phys, store, { cam }, glm::dvec3(0.0), kMin, kScale);
	CHECK(ground.count() == phys.groundFieldCount());
	REQUIRE(ground.count() >= 9);
	CHECK(ground.stats().failed == 0);

	// at lattice points the field is the tile (Jolt packs heights; allow the packing's rounding)
	double worst = 0.0;
	int hits = 0;
	for (double x = 1104.0; x <= 1984.0; x += 16.0)               // multiples of the 16 m raster
		for (double z = 1104.0; z <= 1984.0; z += 16.0)
		{
			const auto h = down(phys, static_cast<float>(x), static_cast<float>(z));
			REQUIRE(h.hit);
			worst = std::max(worst, std::abs(static_cast<double>(h.point.y) - tileHeight(*src, x, z)));
			++hits;
		}
	MESSAGE("largest difference between the Jolt ground and the tile's height at " << hits << " lattice points: " << worst << " m");
	CHECK(worst < 0.25);                                  // Jolt packs each 2 x 2 block of samples into 8 bits

	// a line across three tiles: every sample hits, and across a tile border the height is continuous
	for (double z : { 1100.0, 1536.0, 1537.0, 1900.0 })
	{
		float last = 0.0f;
		bool have = false;
		for (double x = 600.0; x <= 2500.0; x += 1.0)
		{
			const auto h = down(phys, static_cast<float>(x), static_cast<float>(z));
			REQUIRE_MESSAGE(h.hit, "gap at x=" << x << " z=" << z);
			if (have) CHECK(std::abs(h.point.y - last) < 8.0f);      // no wall of the height of the terrain, steepest cell here is far below
			last = h.point.y;
			have = true;
		}
	}
	// exactly on a border: from either side the same height
	// The two fields hold the same border samples, but Jolt packs each field on its own, so the two surfaces can differ by the packing's rounding.
	float seam = 0.0f;
	for (double z = 1104.0; z <= 1984.0; z += 8.0)
	{
		const float a = down(phys, 1536.0f - 0.001f, static_cast<float>(z)).point.y, b = down(phys, 1536.0f + 0.001f, static_cast<float>(z)).point.y;
		seam = std::max(seam, std::abs(a - b));
	}
	MESSAGE("largest step across a tile border: " << seam << " m");
	CHECK(seam < 0.25f);
}

TEST_CASE("landscape physics: ground goes when every anchor is far, comes where an anchor is, and a second anchor keeps its own")
{
	auto src = world(30);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 3000.0f;
	LandscapeStore store(src, cfg);
	PhysicsWorld phys;
	HorizonWorld w;
	phys.initialize(w);
	LandscapePhysicsConfig pc;
	pc.radius = 1000.0f;
	pc.maxBuildsPerUpdate = 1000;
	LandscapePhysics ground(pc);

	const glm::dvec3 a(2000.0, 500.0, 2000.0), b(11000.0, 500.0, 11000.0);
	REQUIRE(settle(store, a));
	ground.update(phys, store, { a }, glm::dvec3(0.0), kMin, kScale);
	const size_t atA = ground.count();
	REQUIRE(atA > 4);
	CHECK(down(phys, 2000.0f, 2000.0f).hit);
	CHECK_FALSE(down(phys, 11000.0f, 11000.0f).hit);

	// walk to b: the ground at a goes once it is beyond radius * keepFactor, the ground at b comes as its tiles load
	REQUIRE(settle(store, b));
	ground.update(phys, store, { b }, glm::dvec3(0.0), kMin, kScale);
	CHECK(down(phys, 11000.0f, 11000.0f).hit);
	CHECK_FALSE(down(phys, 2000.0f, 2000.0f).hit);
	CHECK(ground.count() == phys.groundFieldCount());
	CHECK(ground.count() <= atA + 4);                  // the same few tiles, not the way between
	CHECK(ground.stats().removed >= atA);

	// two anchors: both neighbourhoods keep ground
	REQUIRE(settle(store, a));
	ground.update(phys, store, { a, b }, glm::dvec3(0.0), kMin, kScale);
	CHECK(down(phys, 2000.0f, 2000.0f).hit);
	CHECK(down(phys, 11000.0f, 11000.0f).hit);        // b's ground is kept while b is an anchor, even though the store moved to a? (b is beyond the store's radius: it stays because it was built)
	// no anchor at all: all ground goes
	ground.update(phys, store, {}, glm::dvec3(0.0), kMin, kScale);
	CHECK(ground.count() == 0);
	CHECK(phys.groundFieldCount() == 0);
}

TEST_CASE("landscape physics: a floating origin shift moves the ground with every body, and ground built afterwards joins it")
{
	auto src = world(8);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 3000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(1500.0, 600.0, 1500.0);
	REQUIRE(settle(store, cam));

	PhysicsWorld phys;
	HorizonWorld w;
	phys.initialize(w);
	LandscapePhysicsConfig pc;
	pc.radius = 800.0f;
	pc.maxBuildsPerUpdate = 1;                         // one tile an update: the rest comes after the shift
	LandscapePhysics ground(pc);
	glm::dvec3 origin(0.0);
	ground.update(phys, store, { cam }, origin, kMin, kScale);
	REQUIRE(ground.count() == 1);
	const float before = down(phys, 1500.0f, 1500.0f).point.y;

	// the world's origin moves by (1024, 0, 512): everything is now that much further back in Jolt's space
	const glm::vec3 shift(1024.0f, 0.0f, 512.0f);
	phys.shiftOrigin(shift);
	origin += glm::dvec3(shift);
	const auto moved = down(phys, 1500.0f - shift.x, 1500.0f - shift.z);
	REQUIRE(moved.hit);
	CHECK(moved.point.y == doctest::Approx(before).epsilon(1e-4));

	// the rest of the ground is built with the new origin and meets the shifted one: no gap along a line through all of it
	for (int i = 0; i < 40; ++i) ground.update(phys, store, { cam }, origin, kMin, kScale);
	REQUIRE(ground.count() >= 4);
	for (double x = 800.0; x <= 2200.0; x += 1.0)
	{
		const auto h = down(phys, static_cast<float>(x - origin.x), static_cast<float>(1500.0 - origin.z));
		REQUIRE_MESSAGE(h.hit, "gap at absolute x=" << x);
	}
	for (double x : { 1024.0, 1536.0, 2048.0 })       // tile borders
	{
		const float a = down(phys, static_cast<float>(x - origin.x) - 0.001f, static_cast<float>(1500.0 - origin.z)).point.y;
		const float b = down(phys, static_cast<float>(x - origin.x) + 0.001f, static_cast<float>(1500.0 - origin.z)).point.y;
		CHECK(std::abs(a - b) < 0.25f);
	}
}

TEST_CASE("landscape physics: a body over a tile without ground is held, and falls onto it once it is built")
{
	auto src = world(6);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 3000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(1500.0, 600.0, 1500.0);
	REQUIRE(settle(store, cam));

	HorizonWorld w;
	const Entity crate = w.createEntity("Crate");
	const glm::vec3 start(1500.0f, tileHeight(*src, 1504.0, 1504.0) + 4.0f, 1500.0f);
	w.registry().get_or_emplace<TransformComponent>(crate).position = start;
	RigidBodyComponent rb;
	rb.type = RigidBodyType::Dynamic;
	rb.mass = 1.0f;
	w.addComponent(crate, rb);

	PhysicsWorld phys;
	phys.initialize(w);
	LandscapePhysicsConfig pc;
	pc.radius = 1200.0f;
	pc.maxBuildsPerUpdate = 100;
	LandscapePhysics ground(pc);
	phys.setRegionHold([&](const glm::vec3& p) { return ground.holdsAt(p, glm::dvec3(0.0), store); });

	// no ground yet: the crate must not move, however long the world steps
	for (int i = 0; i < 120; ++i) phys.step(w, 1.0f / 60.0f);
	CHECK(w.registry().get<TransformComponent>(crate).position.y == doctest::Approx(start.y).epsilon(1e-4));
	CHECK(phys.heldCount() >= 1);

	// the ground comes: the crate is let go, falls and stays on it
	ground.update(phys, store, { cam }, glm::dvec3(0.0), kMin, kScale);
	REQUIRE(ground.count() > 0);
	for (int i = 0; i < 240; ++i) phys.step(w, 1.0f / 60.0f);
	const float y = w.registry().get<TransformComponent>(crate).position.y;
	const float groundY = down(phys, 1500.0f, 1500.0f).point.y;
	CHECK(y < start.y);                                // it fell ...
	CHECK(y > groundY - 0.5f);                         // ... and did not go through
	CHECK(y < groundY + 3.0f);                         // ... and lies on it (the slope may carry it a little)

	// outside the world nothing is ever held
	CHECK_FALSE(ground.holdsAt({ -50.0f, 10.0f, 10.0f }, glm::dvec3(0.0), store));
	CHECK_FALSE(ground.holdsAt({ 100000.0f, 10.0f, 10.0f }, glm::dvec3(0.0), store));
}

TEST_CASE("landscape physics: the real tile size is accepted by Jolt, 257 x 257 samples")
{
	auto src = world(2, false);                        // 512 m tiles, 2 m raster
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 800.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 cam(700.0, 300.0, 300.0);
	REQUIRE(settle(store, cam));
	PhysicsWorld phys;
	HorizonWorld w;
	phys.initialize(w);
	LandscapePhysicsConfig pc;
	pc.radius = 300.0f;
	pc.maxBuildsPerUpdate = 10;
	LandscapePhysics ground(pc);
	ground.update(phys, store, { cam }, glm::dvec3(0.0), kMin, kScale);
	CHECK(ground.stats().failed == 0);
	REQUIRE(ground.count() >= 2);
	for (double x = 400.0; x <= 900.0; x += 50.0)
		REQUIRE(down(phys, static_cast<float>(x), 300.0f).hit);
	// a coarser mip is a legal source too: 129 samples, 4 m apart
	LandscapePhysicsConfig coarse = pc;
	coarse.level = 1;
	LandscapePhysics ground2(coarse);
	PhysicsWorld phys2;
	phys2.initialize(w);
	ground2.update(phys2, store, { cam }, glm::dvec3(0.0), kMin, kScale);
	CHECK(ground2.stats().failed == 0);
	CHECK(down(phys2, 700.0f, 300.0f).hit);
}
