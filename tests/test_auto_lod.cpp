#include "doctest.h"
#include "AutoLod.h"
#include <HorizonScene/AutoLodApply.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A UV sphere of lat x lon quads; the first `split` share of its triangles is
// section 0, the rest section 1 (two materials, as a multi-material import has).
StaticMeshAsset makeSphere(int lat, int lon, bool twoSections)
{
	StaticMeshAsset m;
	m.type = HE::AssetType::StaticMesh;
	m.path = "Meshes/Ball.hasset";
	m.name = "Ball";
	m.materialPath = "Materials/A.hasset";
	for (int y = 0; y <= lat; ++y)
		for (int x = 0; x <= lon; ++x)
		{
			const float v = static_cast<float>(y) / lat, u = static_cast<float>(x) / lon;
			const float th = v * 3.14159265f, ph = u * 6.2831853f;
			const float nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = std::sin(th) * std::sin(ph);
			m.vertices.insert(m.vertices.end(), { nx, ny, nz });
			m.normals.insert(m.normals.end(), { nx, ny, nz });
			m.uvs.insert(m.uvs.end(), { u, v });
		}
	const uint32_t row = static_cast<uint32_t>(lon + 1);
	for (int y = 0; y < lat; ++y)
		for (int x = 0; x < lon; ++x)
		{
			const uint32_t a = static_cast<uint32_t>(y) * row + static_cast<uint32_t>(x), b = a + 1, c = a + row, d = c + 1;
			m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
		}
	MeshSection s0;
	s0.materialPath = "Materials/A.hasset";
	if (!twoSections)
	{
		s0.indexCount = static_cast<uint32_t>(m.indices.size());
		m.sections = { s0 };
		return m;
	}
	const uint32_t half = static_cast<uint32_t>(m.indices.size() / 6) * 3;   // whole triangles
	s0.indexCount = half;
	MeshSection s1;
	s1.indexOffset  = half;
	s1.indexCount   = static_cast<uint32_t>(m.indices.size()) - half;
	s1.materialPath = "Materials/B.hasset";
	m.sections = { s0, s1 };
	return m;
}

// Disconnected leaf cards in a bush: the thin geometry the foliage mode is for.
StaticMeshAsset makeLeaves(int count)
{
	StaticMeshAsset m;
	m.type = HE::AssetType::StaticMesh;
	m.path = "Meshes/Bush.hasset";
	for (int i = 0; i < count; ++i)
	{
		const float a = static_cast<float>(i) * 0.7f, r = 0.2f + 0.05f * static_cast<float>(i % 7);
		const float cx = std::cos(a) * r, cz = std::sin(a) * r, h = 0.1f * static_cast<float>(i % 5);
		const uint32_t base = static_cast<uint32_t>(m.vertices.size() / 3);
		const float s = 0.08f;
		const float p[4][3] = { { cx - s, h, cz }, { cx + s, h, cz }, { cx + s, h + 2 * s, cz }, { cx - s, h + 2 * s, cz } };
		const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		for (int k = 0; k < 4; ++k)
		{
			m.vertices.insert(m.vertices.end(), { p[k][0], p[k][1], p[k][2] });
			m.normals.insert(m.normals.end(), { 0.0f, 0.0f, 1.0f });
			m.uvs.insert(m.uvs.end(), { uv[k][0], uv[k][1] });
		}
		m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
	}
	return m;
}

uint32_t triangles(const StaticMeshAsset& m) { return static_cast<uint32_t>(m.indices.size() / 3); }

} // namespace

TEST_CASE("Auto-LOD: the level ratios of the dialog are a descending run that ends on the floor")
{
	const std::vector<float> r = AutoLod::ratiosFor(3, 0.5f, 0.1f);
	REQUIRE(r.size() == 3u);
	CHECK(r[0] == doctest::Approx(0.5f));
	CHECK(r[1] == doctest::Approx(0.25f));
	CHECK(r[2] == doctest::Approx(0.1f));
	// A floor above the geometric run never makes the chain climb again.
	const std::vector<float> high = AutoLod::ratiosFor(4, 0.5f, 0.4f);
	for (size_t i = 1; i < high.size(); ++i) CHECK(high[i] <= high[i - 1]);
	CHECK(AutoLod::ratiosFor(99, 0.5f, 0.1f).size() == 8u);
}

TEST_CASE("Auto-LOD: a level starts where its error shrinks to the pixel tolerance, and the starts only grow")
{
	HE::LodScreenMetrics m;
	m.referenceHeightPx = 1000.0f;
	m.fovYDegrees       = 90.0f;   // tan(45 deg) = 1
	m.tolerancePx       = 2.0f;
	// 0.1 m of error: 0.1 * 1000 / (2 * 1 * 2) = 25 m.
	CHECK(HE::lodDistanceForError(0.1f, m) == doctest::Approx(25.0f));
	// Twice the tolerance = half the distance; twice the error = twice.
	HE::LodScreenMetrics loose = m; loose.tolerancePx = 4.0f;
	CHECK(HE::lodDistanceForError(0.1f, loose) == doctest::Approx(12.5f));
	CHECK(HE::lodDistanceForError(0.2f, m) == doctest::Approx(50.0f));

	const std::vector<float> d = HE::lodDistancesFromErrors({ 0.1f, 0.05f, 0.0f, 0.4f }, m, 1.0f);
	REQUIRE(d.size() == 4u);
	CHECK(d[0] == doctest::Approx(25.0f));
	for (size_t i = 1; i < d.size(); ++i) CHECK(d[i] > d[i - 1]);   // a better level never starts nearer
}

TEST_CASE("Auto-LOD: every level keeps the material sections as contiguous runs and has fewer triangles")
{
	const StaticMeshAsset src = makeSphere(24, 32, /*twoSections=*/true);
	AutoLod::Settings settings;
	settings.ratios = { 0.5f, 0.25f };
	std::vector<StaticMeshAsset> levels;
	const AutoLod::Chain chain = AutoLod::build(src, settings, &levels);
	REQUIRE(chain.error.empty());
	REQUIRE(chain.levels.size() >= 2u);
	CHECK(chain.levels.size() == levels.size() + 1);
	CHECK(chain.levels[0].triangles == triangles(src));
	CHECK(chain.levels[0].worldError == 0.0f);
	for (size_t i = 0; i < levels.size(); ++i)
	{
		const StaticMeshAsset& lv = levels[i];
		CHECK(triangles(lv) < chain.levels[i].triangles);
		CHECK(chain.levels[i + 1].worldError >= chain.levels[i].worldError);
		REQUIRE(lv.sections.size() == 2u);
		CHECK(lv.sections[0].materialPath == "Materials/A.hasset");
		CHECK(lv.sections[1].materialPath == "Materials/B.hasset");
		// Contiguous and covering the buffer exactly (the MSEC contract).
		CHECK(lv.sections[0].indexOffset == 0u);
		CHECK(lv.sections[1].indexOffset == lv.sections[0].indexCount);
		CHECK(lv.sections[0].indexCount + lv.sections[1].indexCount == lv.indices.size());
		CHECK(lv.vertices.size() / 3 == lv.normals.size() / 3);
		CHECK(lv.vertices.size() / 3 == lv.uvs.size() / 2);
		for (uint32_t idx : lv.indices) CHECK(idx < lv.vertices.size() / 3);
	}
}

TEST_CASE("Auto-LOD: a mesh without a section table stays without one")
{
	StaticMeshAsset src = makeSphere(20, 28, false);
	src.sections.clear();
	AutoLod::Settings settings;
	settings.ratios = { 0.5f };
	std::vector<StaticMeshAsset> levels;
	const AutoLod::Chain chain = AutoLod::build(src, settings, &levels);
	REQUIRE(chain.error.empty());
	REQUIRE(levels.size() == 1u);
	CHECK(levels[0].sections.empty());
}

TEST_CASE("Auto-LOD: foliage mode lets leaf cards vanish, and a tiny mesh yields an error instead of a copy")
{
	const StaticMeshAsset bush = makeLeaves(60);
	AutoLod::Settings settings;
	settings.foliage = true;
	settings.ratios  = { 0.5f, 0.25f };
	std::vector<StaticMeshAsset> levels;
	const AutoLod::Chain chain = AutoLod::build(bush, settings, &levels);
	REQUIRE(chain.error.empty());
	REQUIRE(!levels.empty());
	CHECK(triangles(levels.front()) <= triangles(bush) * 6 / 10);

	// Two triangles cannot be reduced: no copy of LOD0 is passed off as a level.
	StaticMeshAsset quad = makeLeaves(1);
	const AutoLod::Chain none = AutoLod::build(quad, settings);
	CHECK_FALSE(none.error.empty());
	CHECK(none.levels.size() == 1u);

	// A packed (cooked) mesh has no readable triangles.
	StaticMeshAsset cooked = bush;
	cooked.cooked = true;
	CHECK_FALSE(AutoLod::build(cooked, settings).error.empty());
}

TEST_CASE("Auto-LOD: generate writes <name>_LOD<n> sidecars that keep their UUID when written again")
{
	const fs::path root = fs::temp_directory_path() / "he_test_auto_lod_generate";
	std::error_code ec;
	fs::remove_all(root, ec);
	fs::create_directories(root, ec);

	StaticMeshAsset src = makeSphere(24, 32, false);
	AutoLod::Settings settings;
	settings.ratios = { 0.5f, 0.2f };

	CHECK(AutoLod::lodPath("Meshes/Ball.hasset", 2) == "Meshes/Ball_LOD2.hasset");
	CHECK(AutoLod::isLodSidecarPath("Meshes/Ball_LOD2.hasset"));
	CHECK_FALSE(AutoLod::isLodSidecarPath("Meshes/Ball.hasset"));
	CHECK_FALSE(AutoLod::isLodSidecarPath("Meshes/Ball_LODge.hasset"));
	CHECK(AutoLod::findLodSidecars(root, src.path).empty());

	const AutoLod::Chain first = AutoLod::generate(src, root, settings);
	REQUIRE(first.error.empty());
	REQUIRE(first.levels.size() >= 2u);
	for (size_t i = 1; i < first.levels.size(); ++i)
	{
		CHECK(first.levels[i].path == AutoLod::lodPath(src.path, static_cast<int>(i)));
		CHECK_FALSE(first.levels[i].id == HE::UUID{});
		CHECK(fs::exists(root / first.levels[i].path));
	}
	CHECK(AutoLod::findLodSidecars(root, src.path).size() == first.levels.size() - 1);

	const AutoLod::Chain second = AutoLod::generate(src, root, settings);
	REQUIRE(second.levels.size() == first.levels.size());
	for (size_t i = 1; i < first.levels.size(); ++i)
		CHECK(second.levels[i].id == first.levels[i].id);   // every scene reference survives

	fs::remove_all(root, ec);
}

TEST_CASE("Auto-LOD: a chain fills a foliage layer with sorted levels, thinner far out, inside the draw distance")
{
	std::vector<HE::AutoLodApply::Level> chain;
	for (int i = 1; i <= 4; ++i)
	{
		HE::AutoLodApply::Level l;
		l.mesh       = HE::UUID{ 0x700u + static_cast<uint64_t>(i), 1u };
		l.worldError = 0.05f * static_cast<float>(i * i);
		chain.push_back(l);
	}
	FoliageLayer layer;
	layer.meshAssetId  = HE::kDefaultCubeMeshId;
	layer.drawDistance = 5000.0f;
	layer.lodHysteresis = 99.0f;
	HE::AutoLodApply::FoliageOptions opt;
	REQUIRE(HE::AutoLodApply::fillFoliageLayer(layer, chain, opt) == 4u);
	REQUIRE(layer.lods.size() == 4u);
	for (size_t i = 0; i < layer.lods.size(); ++i)
	{
		CHECK(layer.lods[i].meshAssetId == chain[i].mesh);
		CHECK(layer.lods[i].materialAssetId == HE::UUID{});   // falls back to the layer's own
		if (i) CHECK(layer.lods[i].distance > layer.lods[i - 1].distance);
		if (i) CHECK(layer.lods[i].density <= layer.lods[i - 1].density);
	}
	CHECK(layer.lods[0].density == doctest::Approx(1.0f));
	CHECK(layer.lods.back().density < 1.0f);
	CHECK(layer.lods.back().density >= 0.3f);
	CHECK(layer.lodHysteresis >= 1.0f);
	CHECK(layer.lodHysteresis <= 10.0f);
	CHECK(layer.dirty);

	// The cross card is opt-in and comes last; the layer's cut-off is not moved.
	opt.crossCard = true;
	HE::AutoLodApply::fillFoliageLayer(layer, chain, opt);
	REQUIRE(layer.lods.size() == 5u);
	CHECK(layer.lods.back().meshAssetId == HE::kEngineCrossCardMeshId);
	CHECK(layer.lods.back().distance > layer.lods[3].distance);
	CHECK(layer.drawDistance == 5000.0f);

	// A level that would start past the cut-off is left out, not drawn nowhere.
	layer.drawDistance = layer.lods[1].distance + 0.5f;
	opt.crossCard = false;
	HE::AutoLodApply::fillFoliageLayer(layer, chain, opt);
	CHECK(layer.lods.size() == 2u);
	CHECK(layer.drawDistance < 5000.0f);
}

TEST_CASE("Auto-LOD: a mesh entity's LODComponent keeps LOD0 first and the last level open-ended")
{
	std::vector<HE::AutoLodApply::Level> chain;
	for (int i = 1; i <= 3; ++i)
		chain.push_back({ HE::UUID{ 0x800u + static_cast<uint64_t>(i), 1u }, 0.1f * static_cast<float>(i) });
	LODComponent lod;
	const HE::UUID lod0{ 0x800u, 1u };
	HE::AutoLodApply::fillLodComponent(lod, lod0, chain, HE::LodScreenMetrics{}, 2.0f);
	REQUIRE(lod.levels.size() == 4u);
	CHECK(lod.levels[0].meshId == lod0);   // navigation, physics and the extractor read this one
	for (size_t i = 1; i < lod.levels.size(); ++i)
		CHECK(lod.levels[i].maxDistance > lod.levels[i - 1].maxDistance);
	CHECK(lod.levels.back().maxDistance == std::numeric_limits<float>::max());
	// An entity twice the size shows the same error twice as far out.
	LODComponent unit;
	HE::AutoLodApply::fillLodComponent(unit, lod0, chain, HE::LodScreenMetrics{}, 1.0f);
	CHECK(lod.levels[0].maxDistance == doctest::Approx(unit.levels[0].maxDistance * 2.0f));
}

TEST_CASE("Auto-LOD: a scene written before chains existed loads the same, and a chain survives save and load")
{
	auto load = [](const json& comps)
	{
		auto world = std::make_unique<HorizonWorld>();
		const Entity e = world->createEntity("Land");
		REQUIRE(SceneSerializer().applyEntityComponents(*world, e, json::to_cbor(comps)));
		return std::make_pair(std::move(world), e);
	};

	// The old form: a foliage layer with no lods keys, a lod component with its levels.
	const json old = {
		{ "foliage", { { "mesh", json() }, { "density", 0.2 }, { "drawDistance", 90.0 } } },
		{ "lod", { { "levels", json::array({ json{ { "meshId", json() }, { "maxDistance", 30.0 } } }) } } },
	};
	auto [oldWorld, oldEnt] = load(old);
	const FoliageLayer& ol = oldWorld->registry().get<FoliageComponent>(oldEnt).layers[0];
	CHECK(ol.lods.empty());
	CHECK(ol.lodHysteresis == FoliageLayer{}.lodHysteresis);
	CHECK(ol.drawDistance == doctest::Approx(90.0f));
	CHECK(oldWorld->registry().get<LODComponent>(oldEnt).levels.size() == 1u);

	// A filled layer and component write and read back to the same levels.
	HorizonWorld world;
	const Entity e = world.createEntity("Land");
	std::vector<HE::AutoLodApply::Level> chain = {
		{ HE::UUID{ 0x901u, 1u }, 0.1f }, { HE::UUID{ 0x902u, 1u }, 0.3f } };
	FoliageComponent fol;
	fol.layers[0].meshAssetId = HE::kDefaultCubeMeshId;
	fol.layers[0].drawDistance = 4000.0f;
	HE::AutoLodApply::fillFoliageLayer(fol.layers[0], chain, {});
	world.registry().emplace<FoliageComponent>(e, fol);
	LODComponent lod;
	HE::AutoLodApply::fillLodComponent(lod, HE::kDefaultCubeMeshId, chain, HE::LodScreenMetrics{}, 1.0f);
	world.registry().emplace<LODComponent>(e, lod);

	const json comps = json::from_cbor(SceneSerializer().serializeEntityComponents(world, e));
	auto [back, backEnt] = load(comps);
	const FoliageLayer& bl = back->registry().get<FoliageComponent>(backEnt).layers[0];
	REQUIRE(bl.lods.size() == fol.layers[0].lods.size());
	for (size_t i = 0; i < bl.lods.size(); ++i)
	{
		CHECK(bl.lods[i].meshAssetId == fol.layers[0].lods[i].meshAssetId);
		CHECK(bl.lods[i].distance == doctest::Approx(fol.layers[0].lods[i].distance));
		CHECK(bl.lods[i].density == doctest::Approx(fol.layers[0].lods[i].density));
	}
	CHECK(bl.lodHysteresis == doctest::Approx(fol.layers[0].lodHysteresis));
	const LODComponent& bc = back->registry().get<LODComponent>(backEnt);
	REQUIRE(bc.levels.size() == lod.levels.size());
	CHECK(bc.levels[1].meshId == lod.levels[1].meshId);
}
