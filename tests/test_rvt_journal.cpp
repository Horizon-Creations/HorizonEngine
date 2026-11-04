#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TerrainPaint.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>

// What the Runtime Virtual Texture driver learns about edits of the ground (Thema 333 step 4): a stroke leaves a rect,
// a rebuild or an unannounced weight upload leaves "all", and the journal is the driver's to clear, not TerrainSystem's.

namespace
{
	struct Land
	{
		HorizonWorld world;
		ContentManager cm;
		entt::entity e = entt::null;
		Land()
		{
			e = world.createEntity("Land");
			world.registry().emplace<TransformComponent>(e, TransformComponent{});
			TerrainComponent tc;
			tc.sizeX = tc.sizeZ = 64.0f;
			tc.resolution = 65;
			tc.heightScale = 0.0f;
			tc.sculptHeights.assign(65u * 65u, 5.0f);
			world.registry().emplace<TerrainComponent>(e, std::move(tc));
			tick();
			tc_().rvtJournal.clear();
		}
		TerrainComponent& tc_() { return world.registry().get<TerrainComponent>(e); }
		void tick() { TerrainSystem::updateTerrains(world, cm, nullptr); }
	};
}

TEST_CASE("RVT journal: the first build and a full rebuild note everything")
{
	HorizonWorld world;
	ContentManager cm;
	const entt::entity e = world.createEntity("Land");
	world.registry().emplace<TransformComponent>(e, TransformComponent{});
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 64.0f; tc.resolution = 65; tc.heightScale = 0.0f;
	world.registry().emplace<TerrainComponent>(e, std::move(tc));
	CHECK_FALSE(world.registry().get<TerrainComponent>(e).rvtJournal.any);   // nothing noted until something is rebuilt
	TerrainSystem::updateTerrains(world, cm, nullptr);
	const auto& j = world.registry().get<TerrainComponent>(e).rvtJournal;
	CHECK(j.any);
	CHECK(j.all);

	Land l;
	CHECK_FALSE(l.tc_().rvtJournal.any);
	l.tick();
	CHECK_FALSE(l.tc_().rvtJournal.any);          // a tick with nothing to do notes nothing
	l.tc_().dirty = true;                          // an inspector change, a load: a full rebuild
	l.tick();
	CHECK(l.tc_().rvtJournal.all);
}

TEST_CASE("RVT journal: a sculpt stroke leaves exactly its rect, and TerrainSystem leaves it for the driver")
{
	Land l;
	TerrainSculpt::markRegionDirty(l.tc_(), -6.0f, 4.0f, -2.0f, 9.0f);
	l.tick();
	const auto& j = l.tc_().rvtJournal;
	REQUIRE(j.any);
	CHECK_FALSE(j.all);
	CHECK(j.minX == doctest::Approx(-6.0f)); CHECK(j.maxX == doctest::Approx(-2.0f));
	CHECK(j.minZ == doctest::Approx(4.0f));  CHECK(j.maxZ == doctest::Approx(9.0f));
	CHECK_FALSE(l.tc_().regionDirty);              // TerrainSystem cleared ITS flags ...
	CHECK(l.tc_().rvtJournal.any);                 // ... and left the journal

	// A second stroke before the driver drains: the rects merge.
	TerrainSculpt::markRegionDirty(l.tc_(), 10.0f, -3.0f, 12.0f, 0.0f);
	l.tick();
	CHECK(l.tc_().rvtJournal.minX == doctest::Approx(-6.0f));
	CHECK(l.tc_().rvtJournal.maxX == doctest::Approx(12.0f));
	CHECK(l.tc_().rvtJournal.minZ == doctest::Approx(-3.0f));
	CHECK(l.tc_().rvtJournal.maxZ == doctest::Approx(9.0f));
	CHECK_FALSE(l.tc_().rvtJournal.all);
}

TEST_CASE("RVT journal: a paint stroke leaves its texel rect, a fill or an unannounced weight upload leaves all")
{
	Land l;
	TerrainPaint::ensureWeightmap(l.tc_());
	l.tick();
	l.tc_().rvtJournal.clear();

	REQUIRE(TerrainPaint::paint(l.tc_(), 10.0f, -8.0f, 1, 3.0f, 2.0f, 1.0f));   // reach 5 m, local (10, -8)
	l.tick();
	const auto& j = l.tc_().rvtJournal;
	REQUIRE(j.any);
	CHECK_FALSE(j.all);
	// The texel rect around the brush: at least its reach, at most a texel or two more (weightRes 256 over 64 m = 0.25 m)
	CHECK(j.minX <= 5.0f);   CHECK(j.minX >= 4.0f);
	CHECK(j.maxX >= 15.0f);  CHECK(j.maxX <= 16.0f);
	CHECK(j.minZ <= -13.0f); CHECK(j.minZ >= -14.0f);
	CHECK(j.maxZ >= -3.0f);  CHECK(j.maxZ <= -2.0f);

	l.tc_().rvtJournal.clear();
	REQUIRE(TerrainPaint::fillLayer(l.tc_(), 2));      // sets weightsDirty with no rect
	l.tick();
	CHECK(l.tc_().rvtJournal.all);

	l.tc_().rvtJournal.clear();
	l.tc_().layerWeights.clear();                      // painted -> unpainted (an undo): the shader falls back to layer 0
	l.tc_().weightsDirty = true;
	l.tick();
	CHECK(l.tc_().rvtJournal.all);
}
