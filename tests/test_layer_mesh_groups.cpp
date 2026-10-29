// Landscape 3.0, step 4 (Thema 233): the layer meshes are a set per texture group.
//
// Mountains is the set Thema 180 shipped (Grass Tuft, Pebble, Rock A/B/C, Snow Drift) and its
// placement numbers are pinned below as literals, so a change to the group table or to the preset
// code cannot move a rock or a snow drift of an existing scene without this file going red. Forest
// and Grassland list kinds of their own; every kind's mesh ids are unique across the whole table,
// and the rules that place rock and snow read the group's row (rock slope, snow height).
#include "doctest.h"

#include <HorizonScene/LayerMeshPresets.h>

#include <set>
#include <vector>
#include <string>
#include <utility>

namespace
{
	struct LodPin { float distance, density; };
	struct MountainsPin
	{
		HE::LayerMeshKind kind;
		const char* name;
		float density, minScale, maxScale, drawDistance;
		float slopeMin, slopeMax, heightMin;
		bool  shadow;
		float shadowDistance;
		std::vector<LodPin> lods;
	};

	// The numbers of LayerMeshPresets.h as Thema 180 wrote them (rock past a steepness of 0.12 is
	// slopeMin 0.15, snow height 60 m + 6 m of blend is heightMin 66).
	std::vector<MountainsPin> mountainsPins()
	{
		const float noH = -FoliageLayer::kNoHeightLimit;
		return {
			{ HE::LayerMeshKind::Tuft,      "Grass Tuft", 0.30f,  0.7f, 1.5f,  70.0f, 0.0f,  0.12f, noH,  false,  0.0f, { { 15.0f, 0.7f }, { 35.0f, 0.4f } } },
			{ HE::LayerMeshKind::Pebble,    "Pebble",     0.20f,  0.5f, 1.6f,  45.0f, 0.0f,  0.25f, noH,  false,  0.0f, { { 15.0f, 0.6f } } },
			{ HE::LayerMeshKind::RockA,     "Rock A",     0.004f, 0.6f, 2.4f, 160.0f, 0.15f, 1.0f,  noH,  true,  60.0f, { { 30.0f, 1.0f }, { 75.0f, 0.7f } } },
			{ HE::LayerMeshKind::RockB,     "Rock B",     0.004f, 0.6f, 2.4f, 160.0f, 0.15f, 1.0f,  noH,  true,  60.0f, { { 30.0f, 1.0f }, { 75.0f, 0.7f } } },
			{ HE::LayerMeshKind::RockC,     "Rock C",     0.004f, 0.6f, 2.4f, 160.0f, 0.15f, 1.0f,  noH,  true,  60.0f, { { 30.0f, 1.0f }, { 75.0f, 0.7f } } },
			{ HE::LayerMeshKind::SnowDrift, "Snow Drift", 0.01f,  0.8f, 2.0f, 110.0f, 0.0f,  0.35f, 66.0f, false,  0.0f, { { 40.0f, 0.8f } } },
		};
	}

	void checkMountainsPin(const MountainsPin& p, const FoliageLayer& l)
	{
		CAPTURE(p.name);
		CHECK(l.name == p.name);
		CHECK(l.meshAssetId == HE::layerMeshLevel0(p.kind));
		CHECK(l.density == p.density);
		CHECK(l.minScale == p.minScale);
		CHECK(l.maxScale == p.maxScale);
		CHECK(l.drawDistance == p.drawDistance);
		CHECK(l.slopeMin == p.slopeMin);
		CHECK(l.slopeMax == p.slopeMax);
		CHECK(l.heightMin == p.heightMin);
		CHECK(l.heightMax == FoliageLayer::kNoHeightLimit);
		CHECK(l.weightLayer == -1);
		CHECK(l.castsShadow == p.shadow);
		CHECK(l.contributesAO == p.shadow);
		CHECK(l.shadowDistance == p.shadowDistance);
		CHECK(l.bucketSize == 32.0f);
		CHECK(l.dirty);
		REQUIRE(l.lods.size() == p.lods.size());
		for (size_t i = 0; i < p.lods.size(); ++i)
		{
			CHECK(l.lods[i].meshAssetId == HE::layerMeshLevelId(p.kind, static_cast<int>(i) + 1));
			CHECK(l.lods[i].distance == p.lods[i].distance);
			CHECK(l.lods[i].density == p.lods[i].density);
		}
	}
}

TEST_CASE("Layer mesh groups: Mountains' presets are exactly what Thema 180 shipped")
{
	for (const MountainsPin& p : mountainsPins())
	{
		FoliageLayer l;
		HE::applyLayerMeshPreset(l, p.kind);
		checkMountainsPin(p, l);
	}
}

namespace
{
	std::vector<HE::LayerMeshKind> kindsOf(HE::AutoLandscapeGroupId g)
	{
		const HE::LayerMeshSet set = HE::layerMeshKindsOfGroup(g);
		return std::vector<HE::LayerMeshKind>(set.kinds, set.kinds + set.count);
	}
	constexpr auto kMountains = HE::AutoLandscapeGroupId::Mountains;
	constexpr auto kForest    = HE::AutoLandscapeGroupId::Forest;
	constexpr auto kGrassland = HE::AutoLandscapeGroupId::Grassland;
}

TEST_CASE("Layer mesh groups: Mountains lists exactly the six kinds of Thema 180, in order")
{
	using K = HE::LayerMeshKind;
	CHECK(kindsOf(kMountains) == std::vector<K>{ K::Tuft, K::Pebble, K::RockA, K::RockB, K::RockC, K::SnowDrift });
	// The enum keeps the six at the front: a kind's number never moves.
	CHECK(static_cast<int>(K::Tuft) == 0);
	CHECK(static_cast<int>(K::SnowDrift) == 5);
	// The placement numbers the Mountains row feeds (they were literals before the table held them).
	CHECK(HE::autoLandscapeGroup(kMountains).rockSlope == 0.12f);
	CHECK(HE::autoLandscapeGroup(kMountains).snowHeight == 60.0f);
}

TEST_CASE("Layer mesh groups: Forest and Grassland offer kinds of their own")
{
	using K = HE::LayerMeshKind;
	CHECK(kindsOf(kForest)    == std::vector<K>{ K::Fern, K::ForestStump, K::FallenLog, K::MossRock });
	CHECK(kindsOf(kGrassland) == std::vector<K>{ K::TallGrass, K::FlowerTuft, K::Pebble, K::RockA, K::ScreeChip });

	// Forest shares nothing with Mountains; Grassland borrows only the pebble and one small rock.
	for (K k : kindsOf(kForest))
		for (K m : kindsOf(kMountains)) CHECK(k != m);
	int shared = 0;
	for (K k : kindsOf(kGrassland))
		for (K m : kindsOf(kMountains)) shared += k == m ? 1 : 0;
	CHECK(shared == 2);

	// No kind is left out of every group (an unreachable kind could not be picked), and none repeats
	// within a group.
	std::set<int> offered;
	for (const HE::AutoLandscapeGroup& g : HE::kAutoLandscapeGroups)
	{
		std::set<int> inGroup;
		for (K k : kindsOf(g.id)) { offered.insert(static_cast<int>(k)); CHECK(inGroup.insert(static_cast<int>(k)).second); }
	}
	CHECK(static_cast<int>(offered.size()) == HE::kLayerMeshKindCount);
}

TEST_CASE("Layer mesh groups: every mesh id of every kind and level is unique, and the new kinds sit in the next block")
{
	std::set<std::pair<uint64_t, uint64_t>> seen;
	int levels = 0;
	for (int k = 0; k < HE::kLayerMeshKindCount; ++k)
	{
		const auto kind = static_cast<HE::LayerMeshKind>(k);
		CAPTURE(HE::layerMeshKindName(kind));
		for (int lv = 0; lv < HE::layerMeshLevelCount(kind); ++lv)
		{
			const HE::UUID id = HE::layerMeshLevelId(kind, lv);
			CHECK(id.lo == 1);
			CHECK(id.hi >= 0x109);                                     // after CrossCard (0x108), in mesh_gen's block
			CHECK(seen.insert({ id.hi, id.lo }).second);
			// Nothing else in the 0x1xx block of the table: the kinds fill 0x109 .. 0x12A without a gap.
			CHECK(id.hi <= 0x12A);
			++levels;
		}
	}
	CHECK(levels == 0x12A - 0x109 + 1);
	CHECK(static_cast<int>(seen.size()) == levels);
	// The new kinds start where Snow Drift's two levels end.
	CHECK(HE::layerMeshLevel0(HE::LayerMeshKind::Fern).hi == 0x119);
	CHECK(HE::layerMeshLevel0(HE::LayerMeshKind::ScreeChip).hi == 0x129);
}

TEST_CASE("Layer mesh groups: the group's table row sets where rock and snow kinds stand")
{
	using K = HE::LayerMeshKind;
	const auto make = [](K k, HE::AutoLandscapeGroupId g) { FoliageLayer l; HE::applyLayerMeshPreset(l, k, g); return l; };

	// Forest's rock starts at 0.10: its Mossy Rock stands at 0.13, ferns and stumps stay below 0.10.
	CHECK(make(K::MossRock, kForest).slopeMin == 0.13f);
	CHECK(make(K::Fern, kForest).slopeMax == 0.10f);
	CHECK(make(K::ForestStump, kForest).slopeMax == 0.10f);
	CHECK(make(K::FallenLog, kForest).slopeMax == 0.10f);
	// Grassland's at 0.20: Stones texture from 0.20, so the borrowed rock and the chips from 0.23.
	CHECK(make(K::RockA, kGrassland).slopeMin == 0.23f);
	CHECK(make(K::ScreeChip, kGrassland).slopeMin == 0.23f);
	CHECK(make(K::TallGrass, kGrassland).slopeMax == 0.20f);
	CHECK(make(K::FlowerTuft, kGrassland).slopeMax == 0.20f);
	// A Forest or Grassland snow line is far above any hill: a drift asked for there stays up with it.
	CHECK(make(K::SnowDrift, kForest).heightMin == 2006.0f);
	CHECK(make(K::SnowDrift, kMountains).heightMin == 66.0f);

	for (const HE::AutoLandscapeGroup& g : HE::kAutoLandscapeGroups)
		for (HE::LayerMeshKind k : kindsOf(g.id))
		{
			CAPTURE(g.name);
			CAPTURE(HE::layerMeshKindName(k));
			const FoliageLayer l = make(k, g.id);
			CHECK(l.slopeMin >= 0.0f);
			CHECK(l.slopeMin <= l.slopeMax);
			CHECK(l.density > 0.0f);
			CHECK(l.minScale <= l.maxScale);
			REQUIRE(l.lods.size() + 1u == static_cast<size_t>(HE::layerMeshLevelCount(k)));
			CHECK(l.lods.back().distance < l.drawDistance);
		}
}

TEST_CASE("Layer mesh groups: a terrain's group is found from its material, Mountains when it is none")
{
	CHECK(HE::layerMeshGroupOfMaterial(HE::kAutoLandscapeMaterialId) == kMountains);
	CHECK(HE::layerMeshGroupOfMaterial(HE::autoLandscapeGroup(kForest).materialId) == kForest);
	CHECK(HE::layerMeshGroupOfMaterial(HE::autoLandscapeGroup(kGrassland).materialId) == kGrassland);
	CHECK(HE::layerMeshGroupOfMaterial(HE::UUID{}) == kMountains);                  // no material
	CHECK(HE::layerMeshGroupOfMaterial(HE::UUID{ 0x1234, 0x5678 }) == kMountains);  // a custom one

	CHECK(HE::layerMeshKindOfStem("fallenlog") == HE::LayerMeshKind::FallenLog);
	CHECK(HE::layerMeshKindOfStem("SNOWDRIFT") == HE::LayerMeshKind::SnowDrift);
	CHECK(HE::layerMeshKindOfStem("nope") == HE::LayerMeshKind::Count);
}
