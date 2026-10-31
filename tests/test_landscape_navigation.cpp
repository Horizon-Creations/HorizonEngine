// Navigation on the world-scale landscape (Thema 326, step 3): a Detour tile per landscape tile, built from the tile's heights on the job pool,
// added to one mesh when finished and taken out when the anchors leave. Real Recast builds and real NavigationSystem path searches; no timing.
#include "doctest.h"
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapeNavigation.h>
#include <HorizonScene/LandscapeRuntime.h>
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/NavigationSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <random>
#include <thread>

using namespace HE;

namespace
{
constexpr float kMin = 0.0f, kScale = 3000.0f;
using HeightFn = std::function<double(double, double)>;

// Heights from a function of the absolute position (metres), at the tile's own raster; every level is the strided subsample of level 0.
class FnSource final : public LandscapeTileSource
{
public:
	FnSource(int tiles, HeightFn f) : m_tiles(tiles), m_f(std::move(f))
	{
		ProceduralLandscapeParams p;
		p.tilesX = p.tilesZ = tiles;
		m_base = ProceduralLandscapeSource(p).generate({ 0, 0 });
	}
	const LandscapeGeometry& geometry() const override { return m_base.geometry; }
	int  tilesX() const override { return m_tiles; }
	int  tilesZ() const override { return m_tiles; }
	bool heightRange(float& minM, float& scaleM) const override { minM = kMin; scaleM = kScale; return true; }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override
	{
		if (key.x < 0 || key.z < 0 || key.x >= m_tiles || key.z >= m_tiles) return false;
		out = m_base;
		const int cells = out.geometry.hCells;
		const double spacing = out.geometry.tileM / cells;
		for (size_t level = 0; level < out.heights.size(); ++level)
		{
			const int dim = out.geometry.heightDim(static_cast<int>(level));
			for (int j = 0; j < dim; ++j)
				for (int i = 0; i < dim; ++i)
				{
					const double x = (static_cast<double>(key.x) * cells + (static_cast<double>(i) * (1 << level))) * spacing;
					const double z = (static_cast<double>(key.z) * cells + (static_cast<double>(j) * (1 << level))) * spacing;
					const double v = std::clamp((m_f(x, z) - kMin) / kScale * 65535.0, 0.0, 65535.0);
					out.heights[level][static_cast<size_t>(j) * dim + i] = static_cast<uint16_t>(std::lround(v));
				}
		}
		if (maxLevels >= 0)
		{
			out.heights.resize(std::min<size_t>(out.heights.size(), static_cast<size_t>(maxLevels)));
			out.weights.resize(std::min<size_t>(out.weights.size(), static_cast<size_t>(maxLevels)));
		}
		return true;
	}

private:
	int          m_tiles;
	HeightFn     m_f;
	LandscapeTile m_base;
};

LandscapeNavConfig testNav(float radius = 600.0f)
{
	LandscapeNavConfig c;
	c.nav.cellSize = 1.0f;       // the tests need the topology, not the 0.5 m grid
	c.nav.cellHeight = 0.25f;     // the landscape default
	c.nav.walkableRadius = 1.0f;
	c.nav.walkableClimb = 1.5f;  // with 1 m cells: the ledge filter wants twice a cell's rise within the climb (see the slope test)
	c.radius = radius;
	c.maxInFlight = 4;
	c.maxStartsPerUpdate = 4;
	return c;
}

bool settleStore(LandscapeStore& store, const glm::dvec3& at, int ms = 30000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		store.update({ { at } }, 5.0);
		if (store.settled()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// Updates until every tile in `want` is built (done), keeping the store as it is.
bool settleNav(LandscapeNavigation& nav, LandscapeStore& store, const std::vector<glm::dvec3>& anchors, const std::vector<LandscapeTileKey>& want, int ms = 60000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		nav.update(store, anchors, kMin, kScale);
		bool all = true;
		for (const LandscapeTileKey& k : want) all = all && nav.tileDone(k);
		if (all && nav.stats().pending == 0) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// A scene with a nav mesh entity and one agent; the landscape's mesh is put into the component.
struct NavScene
{
	HorizonWorld world;
	Entity agent{};
	Entity navEntity{};
	NavScene(const LandscapeNavigation& nav)
	{
		navEntity = world.createEntity("NavMesh");
		auto& nmc = world.registry().emplace<NavMeshComponent>(navEntity);
		nav.attach(nmc);
		agent = world.createEntity("Agent");
		world.registry().get_or_emplace<TransformComponent>(agent);
		world.registry().emplace<NavAgentComponent>(agent);
	}
	void place(const glm::vec3& p) { world.registry().get<TransformComponent>(agent).position = p; }
	bool moveTo(const glm::vec3& p) { return NavigationSystem::moveTo(world, agent, p); }
	float remaining() { return NavigationSystem::remainingDistance(world, agent); }
};

std::vector<LandscapeTileKey> allTiles(int n)
{
	std::vector<LandscapeTileKey> v;
	for (int z = 0; z < n; ++z)
		for (int x = 0; x < n; ++x) v.push_back({ x, z });
	return v;
}
} // namespace

TEST_CASE("landscape navigation: flat ground across tile borders is one walkable surface, a path crosses them")
{
	auto src = std::make_shared<FnSource>(3, [](double, double) { return 200.0; });
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 centre(768.0, 200.0, 768.0);
	REQUIRE(settleStore(store, centre));

	LandscapeNavigation nav(testNav());
	REQUIRE(settleNav(nav, store, { centre }, allTiles(3)));
	CHECK(nav.stats().tiles == 9);
	CHECK(nav.stats().polygons > 0);
	CHECK(nav.stats().failed == 0);
	CHECK(nav.navMesh() != nullptr);
	CHECK(nav.navQuery() != nullptr);

	NavScene scene(nav);
	scene.place({ 100.0f, 200.0f, 768.0f });
	REQUIRE(scene.moveTo({ 1400.0f, 200.0f, 768.0f }));        // over two tile borders (x = 512, 1024)
	const float d = scene.remaining();
	CHECK(d >= 1299.0f);                                          // never shorter than the straight line
	CHECK(d <= 1330.0f);                                          // and, on flat ground, about that

	// above the surface by more than the search box, or outside the world: not on the mesh
	scene.place({ 100.0f, 260.0f, 768.0f });
	CHECK_FALSE(scene.moveTo({ 1400.0f, 200.0f, 768.0f }));
	scene.place({ 100.0f, 200.0f, 768.0f });
	CHECK_FALSE(scene.moveTo({ 1400.0f, 200.0f, 5000.0f }));
}

TEST_CASE("landscape navigation: a slope past maxSlope is a wall, a gentler one is a way through")
{
	// A band at x = 700 .. 740 (68 degrees) climbs 100 m: the two sides are not connected.
	// A ramp over x = 700 .. 950 (22 degrees) climbs the same 100 m: they are. (Recast's filter drops a span whose neighbours on both sides
	// differ from it by more than the climb together: with 1 m cells a ramp needs climb >= 2 x its rise per metre.)
	struct Case { const char* name; HeightFn f; bool connected; };
	const Case cases[] = {
		{ "cliff", [](double x, double) { return x < 700.0 ? 100.0 : (x > 740.0 ? 200.0 : 100.0 + (x - 700.0) * 2.5); }, false },
		{ "ramp", [](double x, double) { return x < 700.0 ? 100.0 : (x > 950.0 ? 200.0 : 100.0 + (x - 700.0) * 0.4); }, true },
	};
	for (const Case& c : cases)
	{
		CAPTURE(c.name);
		auto src = std::make_shared<FnSource>(3, c.f);
		LandscapeStoreConfig cfg;
		cfg.loadRadius = 6000.0f;
		LandscapeStore store(src, cfg);
		const glm::dvec3 centre(768.0, 150.0, 768.0);
		REQUIRE(settleStore(store, centre));
		LandscapeNavigation nav(testNav());
		REQUIRE(settleNav(nav, store, { centre }, allTiles(3)));

		NavScene scene(nav);
		scene.place({ 300.0f, 100.0f, 768.0f });
		REQUIRE(scene.moveTo({ 1300.0f, 200.0f, 768.0f }));      // both ends are on the mesh in both cases
		const float d = scene.remaining();
		// connected: the way, about 1000 m horizontally and 100 m up. Not connected: Detour's partial path ends at the foot of the wall.
		if (c.connected) { CHECK(d >= 1000.0f); CHECK(d <= 1060.0f); }
		else CHECK(d < 500.0f);
	}
}

TEST_CASE("landscape navigation: at the production cell size a 40 degree ramp is a way through and a 50 degree one is a wall")
{
	// The landscape defaults (0.5 m cells, 0.25 m height cells, climb 1.0 m): maxSlope 45 degrees is what limits, not the ledge filter.
	struct Case { const char* name; double slope; bool connected; };
	const Case cases[] = { { "40 degrees", std::tan(40.0 * 3.14159265358979 / 180.0), true }, { "50 degrees", std::tan(50.0 * 3.14159265358979 / 180.0), false } };
	for (const Case& c : cases)
	{
		CAPTURE(c.name);
		const double s = c.slope;
		auto src = std::make_shared<FnSource>(2, [s](double x, double) { return x < 300.0 ? 100.0 : std::min(200.0, 100.0 + (x - 300.0) * s); });
		LandscapeStoreConfig cfg;
		cfg.loadRadius = 6000.0f;
		LandscapeStore store(src, cfg);
		const glm::dvec3 centre(512.0, 150.0, 100.0);
		REQUIRE(settleStore(store, centre));
		LandscapeNavConfig nc;                // all defaults: cellSize 0.5 m
		nc.radius = 300.0f;                   // the tile row z = 0 only
		nc.maxInFlight = 4;
		nc.maxStartsPerUpdate = 4;
		LandscapeNavigation nav(nc);
		REQUIRE(settleNav(nav, store, { centre }, { { 0, 0 }, { 1, 0 } }));
		CHECK(nav.stats().failed == 0);

		NavScene scene(nav);
		scene.place({ 100.0f, 100.0f, 100.0f });
		REQUIRE(scene.moveTo({ 800.0f, 200.0f, 100.0f }));
		const float d = scene.remaining();
		if (c.connected) { CHECK(d >= 700.0f); CHECK(d <= 780.0f); }
		else CHECK(d < 400.0f);
	}
}

TEST_CASE("landscape navigation: tiles leave the mesh when the anchors do, and wait for neighbours that are not in memory")
{
	auto src = std::make_shared<FnSource>(6, [](double, double) { return 200.0; });
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);

	// the store has loaded nothing: nothing can be built, and nothing crashes
	LandscapeNavigation nav(testNav(300.0f));
	const glm::dvec3 nearOrigin(256.0, 200.0, 256.0);
	for (int i = 0; i < 5; ++i) nav.update(store, { nearOrigin }, kMin, kScale);
	CHECK(nav.stats().started == 0);
	CHECK(nav.stats().deferred > 0);

	REQUIRE(settleStore(store, nearOrigin));
	REQUIRE(settleNav(nav, store, { nearOrigin }, { { 0, 0 } }));
	CHECK(nav.tileBuilt({ 0, 0 }));
	{
		NavScene scene(nav);
		scene.place({ 100.0f, 200.0f, 100.0f });
		CHECK(scene.moveTo({ 200.0f, 200.0f, 200.0f }));
	}

	// the anchor goes to the far corner: the first tiles are dropped, the new ones built
	const glm::dvec3 farCorner(5.0 * 512.0 + 256.0, 200.0, 5.0 * 512.0 + 256.0);
	REQUIRE(settleStore(store, farCorner));
	REQUIRE(settleNav(nav, store, { farCorner }, { { 5, 5 } }));
	CHECK(nav.stats().dropped > 0);
	CHECK_FALSE(nav.tileBuilt({ 0, 0 }));
	CHECK(nav.tileBuilt({ 5, 5 }));
	NavScene scene(nav);
	scene.place({ 100.0f, 200.0f, 100.0f });
	CHECK_FALSE(scene.moveTo({ 200.0f, 200.0f, 200.0f }));         // no mesh there any more
	scene.place({ 5.0f * 512.0f + 100.0f, 200.0f, 5.0f * 512.0f + 100.0f });
	CHECK(scene.moveTo({ 5.0f * 512.0f + 200.0f, 200.0f, 5.0f * 512.0f + 200.0f }));
}

TEST_CASE("landscape navigation: sea and cliffs leave a tile empty, which is done and costs nothing")
{
	// Everything steeper than 45 degrees everywhere: a sawtooth of 4 m across 2 m.
	auto src = std::make_shared<FnSource>(2, [](double x, double) { return std::fmod(std::floor(x / 2.0), 2.0) == 0.0 ? 100.0 : 104.0; });
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 centre(512.0, 100.0, 512.0);
	REQUIRE(settleStore(store, centre));
	LandscapeNavigation nav(testNav());
	REQUIRE(settleNav(nav, store, { centre }, allTiles(2)));
	CHECK(nav.stats().tiles == 0);
	CHECK(nav.stats().empty == 4);
	CHECK(nav.stats().polygons == 0);
	CHECK(nav.stats().failed == 0);
	CHECK(nav.tileDone({ 0, 0 }));
	CHECK_FALSE(nav.tileBuilt({ 0, 0 }));
}

TEST_CASE("landscape navigation: destroying it with builds running waits for them and lets go of the tiles")
{
	auto src = std::make_shared<FnSource>(3, [](double, double) { return 200.0; });
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 6000.0f;
	LandscapeStore store(src, cfg);
	const glm::dvec3 centre(768.0, 200.0, 768.0);
	REQUIRE(settleStore(store, centre));
	{
		LandscapeNavigation nav(testNav());
		nav.update(store, { centre }, kMin, kScale);
		CHECK(nav.stats().started > 0);
	}   // the builds were started a moment ago and are waited for here
	const auto held = store.tile({ 1, 1 });
	REQUIRE(held);
	for (int i = 0; i < 100 && held.use_count() > 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(held.use_count() == 2);   // the store's and ours
}

TEST_CASE("landscape runtime: the scene's nav agents make the runtime build the mesh around them and the agent can walk it")
{
	const auto src = std::make_shared<FnSource>(4, [](double, double) { return 200.0; });
	const auto path = std::filesystem::temp_directory_path() / ("he_landscape_nav_" + std::to_string(std::random_device{}()) + ".bin");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), src->geometry(), 4, 4, 4, kMin, kScale));
		for (int z = 0; z < 4; ++z)
			for (int x = 0; x < 4; ++x)
			{
				LandscapeTile t;
				REQUIRE(src->load({ x, z }, -1, t));
				REQUIRE(w.add({ x, z }, encodeLandscapeTile(t)));
			}
		REQUIRE(w.finish());
	}

	HorizonWorld world;
	const Entity landscape = world.createEntity("Landscape");
	auto& comp = world.registry().emplace<LandscapeWorldComponent>(landscape);
	comp.pack = path.string();
	comp.heightMinM = kMin;
	comp.heightScaleM = kScale;
	const Entity cam = world.createEntity("Camera");
	world.registry().get_or_emplace<TransformComponent>(cam).position = { 1000.0f, 400.0f, 1000.0f };
	world.registry().emplace<CameraComponent>(cam);
	const Entity navEntity = world.createEntity("NavMesh");
	world.registry().emplace<NavMeshComponent>(navEntity);
	const Entity agent = world.createEntity("Agent");
	world.registry().get_or_emplace<TransformComponent>(agent).position = { 800.0f, 200.0f, 1000.0f };
	world.registry().emplace<NavAgentComponent>(agent);

	LandscapeRuntime rt;
	LandscapeNavConfig nc = testNav();
	rt.setNavigation(&nc);
	REQUIRE(rt.navigation() != nullptr);
	rt.update(world, 0.016f);
	REQUIRE(rt.active());
	const auto t0 = std::chrono::steady_clock::now();
	const auto here = [&](int x, int z) { return rt.navigation()->tileBuilt({ x, z }); };
	while (!(here(1, 1) && here(2, 1)) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
	{
		rt.update(world, 0.016f);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(here(1, 1));
	REQUIRE(here(2, 1));
	CHECK(world.registry().get<NavMeshComponent>(navEntity).navMesh == rt.navigation()->navMesh());

	CHECK(NavigationSystem::moveTo(world, agent, { 1400.0f, 200.0f, 1000.0f }));
	const float d = NavigationSystem::remainingDistance(world, agent);
	CHECK(d >= 599.0f);
	CHECK(d <= 620.0f);

	// without a config the runtime builds nothing
	rt.setNavigation(nullptr);
	CHECK(rt.navigation() == nullptr);
	rt.update(world, 0.016f);

	rt.reset();
	std::filesystem::remove(path);
}
