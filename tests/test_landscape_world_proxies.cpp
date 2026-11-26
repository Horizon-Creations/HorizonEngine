// Foliage and water of a tile landscape as entities the renderers draw already (Thema 326, step 2 part B): LandscapeWorldProxies makes one
// hidden entity per (tile, foliage layer) and per water sheet from what LandscapeRuntime holds, moves them with the floating origin and takes
// them away again. Real tiles, real scatter and a real water region; no GPU, no timing.
#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LandscapeProxyComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeWater.h>
#include <HorizonScene/LandscapeWorldProxies.h>

#include <chrono>
#include <thread>

using namespace HE;

namespace
{
std::shared_ptr<ProceduralLandscapeSource> seaWorld()
{
	ProceduralLandscapeParams p;
	p.tilesX = p.tilesZ = 8;      // the real geometry: 512 m tiles, a 4 km world
	p.seaLevelM = 1500.0f;        // the fBm heights sit around 1500 m: about half the cells are under it
	return std::make_shared<ProceduralLandscapeSource>(p);
}

FoliageLayer grass()
{
	FoliageLayer l;
	l.meshAssetId = HE::UUID{ 1, 0 };
	l.density = 0.02f;
	l.drawDistance = 300.0f;
	l.seed = 5;
	return l;
}

// Updates until the runtime holds foliage and a finished water region, or `ms` ran out.
bool settle(LandscapeRuntime& rt, const glm::dvec3& cam, int ms = 60000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		rt.updateAt(cam, glm::vec3(0.0f), 5.0);
		std::vector<LandscapeFoliageDraw> draws;
		rt.foliageDraws(draws);
		const auto region = rt.waterRegion();
		if (!draws.empty() && region && !region->sheets.empty() && !rt.water().stats().building) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
} // namespace

TEST_CASE("landscape world proxies: foliage and water become hidden entities at centre minus origin, follow the origin, and go with the landscape")
{
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	std::string error;
	REQUIRE(rt.begin(seaWorld(), cfg, &error));
	rt.setFoliageLayers({ grass() });
	const glm::dvec3 cam(2048.0, 1700.0, 2048.0);
	REQUIRE(settle(rt, cam));

	HorizonWorld world;
	ContentManager cm{ "." };
	LandscapeWorldProxies proxies;
	auto& reg = world.registry();

	std::vector<LandscapeFoliageDraw> draws;
	rt.foliageDraws(draws);
	const auto region = rt.waterRegion();
	REQUIRE(region);
	size_t sheets = 0;
	for (const auto& s : region->sheets) sheets += s.surface.empty() ? 0 : 1;
	REQUIRE(sheets > 0);

	world.setOrigin(glm::dvec3(1000.0, 0.0, 1000.0));
	proxies.sync(world, rt, cm, nullptr);
	CHECK(proxies.stats().foliageEntities == draws.size());
	CHECK(proxies.stats().waterEntities == sheets);

	size_t foliage = 0, water = 0;
	for (auto [e, proxy] : reg.view<LandscapeProxyComponent>().each())
	{
		CHECK(isTerrainGenerated(reg, e));                 // never saved, listed or picked
		const auto& tf = reg.get<TransformComponent>(e);
		const glm::vec3 want = glm::vec3(proxy.centre - world.origin());
		CHECK(tf.position.x == doctest::Approx(want.x));
		CHECK(tf.position.y == doctest::Approx(want.y));
		CHECK(tf.position.z == doctest::Approx(want.z));
		if (proxy.kind == LandscapeProxyComponent::Kind::Foliage)
		{
			++foliage;
			const auto& fc = reg.get<FoliageComponent>(e);
			REQUIRE(fc.layers.size() == 1);
			CHECK(fc.layers[0].store);
			CHECK(fc.layers[0].cachedInstances.empty());
			CHECK(fc.layers[0].meshAssetId == grass().meshAssetId);
		}
		else
		{
			++water;
			REQUIRE(reg.all_of<MeshComponent>(e));
			CHECK(reg.get<MeshComponent>(e).meshAssetId == proxy.meshId);
			CHECK_FALSE(reg.get<MeshComponent>(e).castsShadow);
			CHECK(cm.getStaticMesh(proxy.meshId) != nullptr);
			REQUIRE(reg.all_of<MaterialComponent>(e));
		}
	}
	CHECK(foliage == draws.size());
	CHECK(water == sheets);

	// The same state again makes nothing new; each foliage store is the runtime's own object (the renderer's level memory keys on it).
	const size_t created = proxies.stats().created;
	proxies.sync(world, rt, cm, nullptr);
	CHECK(proxies.stats().created == created);
	for (const LandscapeFoliageDraw& d : draws)
		for (auto [e, proxy, fc] : reg.view<LandscapeProxyComponent, FoliageComponent>().each())
			if (proxy.centre == d.centre) CHECK(fc.layers[0].store == d.store);

	// The floating origin moves: the entities slide by the difference, nothing is rebuilt.
	world.setOrigin(glm::dvec3(1500.0, 0.0, 700.0));
	proxies.sync(world, rt, cm, nullptr);
	CHECK(proxies.stats().created == created);
	for (auto [e, proxy] : reg.view<LandscapeProxyComponent>().each())
	{
		const glm::vec3 want = glm::vec3(proxy.centre - world.origin());
		CHECK(reg.get<TransformComponent>(e).position.x == doctest::Approx(want.x));
		CHECK(reg.get<TransformComponent>(e).position.z == doctest::Approx(want.z));
	}

	// A water mesh belongs to its entity: it is given back when the landscape goes.
	std::vector<HE::UUID> meshes;
	for (auto [e, proxy] : reg.view<LandscapeProxyComponent>().each())
		if (proxy.kind == LandscapeProxyComponent::Kind::Water) meshes.push_back(proxy.meshId);
	REQUIRE_FALSE(meshes.empty());
	rt.reset();
	proxies.sync(world, rt, cm, nullptr);
	CHECK(proxies.stats().foliageEntities == 0);
	CHECK(proxies.stats().waterEntities == 0);
	CHECK(reg.view<LandscapeProxyComponent>().size() == 0);
	for (const HE::UUID& id : meshes) CHECK(cm.getStaticMesh(id) == nullptr);
}

TEST_CASE("landscape world proxies: a different world than before is not touched, and clear() leaves nothing behind")
{
	LandscapeRuntime rt;
	LandscapeRuntimeConfig cfg;
	REQUIRE(rt.begin(seaWorld(), cfg));
	rt.setFoliageLayers({ grass() });
	REQUIRE(settle(rt, glm::dvec3(2048.0, 1700.0, 2048.0)));

	ContentManager cm{ "." };
	LandscapeWorldProxies proxies;
	{
		HorizonWorld a;
		proxies.sync(a, rt, cm, nullptr);
		CHECK(a.registry().view<LandscapeProxyComponent>().size() > 0);
	}
	HorizonWorld b;
	proxies.sync(b, rt, cm, nullptr);   // the first world is gone; the bookkeeping starts over in the new one
	const size_t n = b.registry().view<LandscapeProxyComponent>().size();
	CHECK(n > 0);
	CHECK(n == proxies.stats().foliageEntities + proxies.stats().waterEntities);
	proxies.clear(b, cm, nullptr);
	CHECK(b.registry().view<LandscapeProxyComponent>().size() == 0);
}
