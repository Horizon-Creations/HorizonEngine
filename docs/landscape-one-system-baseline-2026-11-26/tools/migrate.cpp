// Scratch: runs the same call the editor's "Migrate Terrain to Tiles" button runs (LandscapeWorldTools::startMigrate ->
// HE::migrateTerrainToPack, yOffset = terrain entity's world y, default geometry), then does what a user does next:
// adds an entity with a LandscapeWorldComponent that points at the pack, and drops the old TerrainComponent.
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/LandscapePackBuild.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <cstdio>
#include <filesystem>
using namespace HE;
int main(int argc, char** argv)
{
	if (argc < 4) { std::fprintf(stderr, "usage: migrate in.hescene out.hescene pack.hlp\n"); return 2; }
	HorizonWorld world;
	SceneSerializer ser;
	if (!ser.load(world, argv[1], SerializeFormat::JSON)) { std::fprintf(stderr, "load failed\n"); return 1; }
	auto& reg = world.registry();
	Entity terrain = entt::null;
	for (auto e : reg.view<TerrainComponent>()) { terrain = e; break; }
	if (terrain == entt::null) { std::fprintf(stderr, "no terrain\n"); return 1; }
	TerrainComponent copy = reg.get<TerrainComponent>(terrain);
	LandscapeMigrateParams mp;
	mp.yOffsetM = worldPositionOf(world, terrain).y;
	LandscapeBuildOptions opt;
	const auto r = migrateTerrainToPack(copy, argv[3], mp, opt);
	std::printf("ok=%d tiles=%dx%d written=%zu minM=%f scaleM=%f err=%s | %s\n", (int)r.ok, r.tilesX, r.tilesZ, (size_t)r.tilesWritten, r.heightMinM, r.heightScaleM, r.error.c_str(), r.summary().c_str());
	if (!r.ok) return 1;
	Entity lw = world.createEntity("Tile Landscape");
	LandscapeWorldComponent c;
	c.pack = std::filesystem::absolute(argv[3]).string();
	c.heightMinM = r.heightMinM;
	c.heightScaleM = r.heightScaleM;
	reg.emplace<LandscapeWorldComponent>(lw, c);
	reg.remove<TerrainComponent>(terrain);
	if (!ser.save(world, argv[2], SerializeFormat::JSON)) { std::fprintf(stderr, "save failed\n"); return 1; }
	return 0;
}
