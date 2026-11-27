// A scene moved off the single landscape (topic 355, step 9; HE::planSceneLandscapeMigration / applySceneLandscapeMigration):
// the plan's refusals, the shift that puts the terrain's south-west corner at (0, 0), and the scene after it (component swapped on
// the same entity, its chunks gone, every top-level entity moved, children left as they were relative to their parents).
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapePackBuild.h>
#include <HorizonScene/LandscapeSource.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace HE;

namespace
{
TerrainComponent smallTerrain()
{
	TerrainComponent tc;
	tc.sizeX = tc.sizeZ = 1024.0f;
	tc.resolution = 33;
	tc.sculptHeights.assign(33u * 33u, 0.0f);
	for (uint32_t i = 0; i < tc.sculptHeights.size(); ++i) tc.sculptHeights[i] = float(i % 33) * 2.0f;   // a slope along x
	return tc;
}

entt::entity place(HorizonWorld& world, const char* name, const glm::vec3& pos)
{
	const entt::entity e = world.createEntity(name);
	world.registry().get_or_emplace<TransformComponent>(e).position = pos;
	return e;
}
} // namespace

TEST_CASE("scene migration: the plan refuses what a tile world cannot take")
{
	{
		HorizonWorld w;
		place(w, "Nothing", glm::vec3(0.0f));
		CHECK_FALSE(planSceneLandscapeMigration(w).ok());   // no terrain
	}
	{
		HorizonWorld w;
		w.registry().emplace<TerrainComponent>(place(w, "A", glm::vec3(0.0f)), smallTerrain());
		w.registry().emplace<TerrainComponent>(place(w, "B", glm::vec3(5000.0f, 0.0f, 0.0f)), smallTerrain());
		const auto plan = planSceneLandscapeMigration(w);
		CHECK_FALSE(plan.ok());
		CHECK(plan.error.find("more than one") != std::string::npos);
	}
	{
		HorizonWorld w;
		const entt::entity t = place(w, "Turned", glm::vec3(0.0f));
		w.registry().emplace<TerrainComponent>(t, smallTerrain());
		w.registry().get<TransformComponent>(t).rotation.y = 30.0f;
		CHECK_FALSE(planSceneLandscapeMigration(w).ok());
	}
	{
		HorizonWorld w;
		w.registry().emplace<TerrainComponent>(place(w, "Old", glm::vec3(0.0f)), smallTerrain());
		w.registry().emplace<LandscapeWorldComponent>(place(w, "New", glm::vec3(0.0f)));
		CHECK_FALSE(planSceneLandscapeMigration(w).ok());   // a tile landscape already
	}
}

TEST_CASE("scene migration: the scene moves with the ground and the terrain's entity becomes the tile landscape")
{
	const fs::path dir = fs::temp_directory_path() /
	                     ("he_scene_migration_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	he_test::removeAllQuiet(dir);
	fs::create_directories(dir);

	HorizonWorld w;
	auto& reg = w.registry();
	const entt::entity terrain = place(w, "Landscape", glm::vec3(100.0f, 5.0f, 200.0f));
	reg.emplace<TerrainComponent>(terrain, smallTerrain());
	reg.emplace_or_replace<MaterialComponent>(terrain).materialAssetId = kAutoLandscapeGroups[2].materialId;   // Grassland
	const entt::entity house = place(w, "House", glm::vec3(10.0f, 0.0f, -20.0f));
	const entt::entity door = place(w, "Door", glm::vec3(1.0f, 2.0f, 3.0f));
	w.reparentEntity(door, house);
	reg.get<TransformComponent>(door).position = glm::vec3(1.0f, 2.0f, 3.0f);   // local to the house
	// A chunk the terrain made (the TerrainSystem's children of the terrain entity).
	const entt::entity chunk = w.createEntity("TerrainChunk");
	w.reparentEntity(chunk, terrain);
	TerrainChunkComponent cc;
	cc.terrain = terrain;
	reg.emplace<TerrainChunkComponent>(chunk, cc);

	const LandscapeSceneMigration plan = planSceneLandscapeMigration(w);
	REQUIRE_MESSAGE(plan.ok(), plan.error);
	CHECK(plan.terrain == terrain);
	// The south-west corner (100 - 512, 200 - 512) goes to (0, 0); the height stays.
	CHECK(plan.shift.x == doctest::Approx(412.0));
	CHECK(plan.shift.y == doctest::Approx(0.0));
	CHECK(plan.shift.z == doctest::Approx(312.0));
	CHECK(plan.params.yOffsetM == doctest::Approx(5.0f));
	CHECK(plan.textureGroup == 2);

	LandscapeMigrateParams params = plan.params;
	params.geometry.tileM = 512.0f;
	params.geometry.hCells = 32;
	params.geometry.wTexels = 16;
	params.geometry.channels = 12;
	const std::string packPath = (dir / "world.hlp").string();
	const LandscapeBuildResult pack = migrateTerrainToPack(*plan.terrainCopy, packPath, params);
	REQUIRE_MESSAGE(pack.ok, pack.error);

	// A failed pack changes nothing.
	LandscapeBuildResult failed;
	CHECK(applySceneLandscapeMigration(w, plan, failed, "x.hlp") == 0);
	CHECK(reg.all_of<TerrainComponent>(terrain));

	const size_t moved = applySceneLandscapeMigration(w, plan, pack, "Content/Landscape/world.hlp");
	CHECK(moved >= 2);   // the landscape's entity and the house (and whatever top-level entities the world makes itself)
	CHECK_FALSE(reg.all_of<TerrainComponent>(terrain));
	REQUIRE(reg.all_of<LandscapeWorldComponent>(terrain));
	const auto& lw = reg.get<LandscapeWorldComponent>(terrain);
	CHECK(lw.pack == "Content/Landscape/world.hlp");
	CHECK(lw.heightMinM == doctest::Approx(pack.heightMinM));
	CHECK(lw.heightScaleM == doctest::Approx(pack.heightScaleM));
	CHECK(lw.textureGroup == 2);
	CHECK(reg.all_of<MaterialComponent>(terrain));   // the ground keeps its material
	CHECK_FALSE(reg.valid(chunk));
	CHECK(reg.get<TransformComponent>(house).position.x == doctest::Approx(422.0f));
	CHECK(reg.get<TransformComponent>(house).position.z == doctest::Approx(292.0f));
	CHECK(reg.get<TransformComponent>(door).position.x == doctest::Approx(1.0f));   // a child keeps its place on its parent
	CHECK(reg.get<TransformComponent>(door).position.z == doctest::Approx(3.0f));
	CHECK(reg.get<TransformComponent>(terrain).position.x == doctest::Approx(512.0f));

	// The pack covers the ground where the terrain was: 2 x 2 tiles of 512 m from (0, 0).
	std::string err;
	const auto src = LandscapePackSource::open(packPath, &err);
	REQUIRE_MESSAGE(src, err);
	CHECK(src->tilesX() == 2);
	CHECK(src->tilesZ() == 2);

	CHECK_FALSE(planSceneLandscapeMigration(w).ok());   // done: there is no terrain left to move
	he_test::removeAllQuiet(dir);
}
