#include "doctest.h"
#include "OutlinerFilter.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
#include <string>
#include <vector>

// The rule behind the Outliner's header row — type "torch", pick "Light" —
// asked the way the user asks it: is the thing I typed found regardless of
// case, do I still see WHERE it is, does everything else get out of the way,
// and does clearing the box give me the whole tree back. The panel draws
// whatever this says; a window is not needed to know whether it is right.

using OutlinerFilter::Row;
using OutlinerFilter::Show;
using OutlinerFilter::Shown;

namespace
{
	// A tree written the way the Outliner caches it: depth-first, children
	// after their parent, one deeper.
	//
	//   0 World
	//   1 ├─ Props
	//   2 │  ├─ Torch_01
	//   2 │  └─ Crate
	//   1 ├─ Lights
	//   2 │  └─ Torch_02
	//   3 │     └─ Flame
	//   1 └─ Camera
	const std::vector<std::string> kNames = {
		"World", "Props", "Torch_01", "Crate", "Lights", "Torch_02", "Flame", "Camera" };
	const std::vector<int> kDepths = { 0, 1, 2, 2, 1, 2, 3, 1 };

	std::vector<Row> rowsMatching(const std::string& needle)
	{
		std::vector<Row> rows;
		for (size_t i = 0; i < kNames.size(); ++i)
			// The root is the tree's own top, never a hit — the panel applies
			// the same rule before it asks.
			rows.push_back({ kDepths[i], i != 0 && OutlinerFilter::nameMatches(kNames[i], needle) });
		return rows;
	}

	std::vector<Show> showsOf(const std::vector<Shown>& s)
	{
		std::vector<Show> out;
		for (const Shown& r : s) out.push_back(r.show);
		return out;
	}
}

TEST_CASE("OutlinerFilter::nameMatches is a case-insensitive substring")
{
	CHECK(OutlinerFilter::nameMatches("Torch_01", "torch"));
	CHECK(OutlinerFilter::nameMatches("Torch_01", "ORCH_0"));
	CHECK(OutlinerFilter::nameMatches("Torch_01", "Torch_01"));
	CHECK_FALSE(OutlinerFilter::nameMatches("Torch_01", "torches"));
	CHECK_FALSE(OutlinerFilter::nameMatches("Torch_01", "Torch_02"));
	// A prefix is not required — "search" means anywhere in the name.
	CHECK(OutlinerFilter::nameMatches("Old Torch", "torch"));
	// An empty needle is no filter at all.
	CHECK(OutlinerFilter::nameMatches("anything", ""));
	CHECK(OutlinerFilter::nameMatches("", ""));
	CHECK_FALSE(OutlinerFilter::nameMatches("", "a"));
}

TEST_CASE("OutlinerFilter::apply keeps a hit's path, dimmed, and drops the rest")
{
	const auto shown = OutlinerFilter::apply(rowsMatching("torch"));
	REQUIRE(shown.size() == kNames.size());
	CHECK(showsOf(shown) == std::vector<Show>{
		Show::Context,  // World: the way to both hits
		Show::Context,  // Props
		Show::Hit,      // Torch_01
		Show::Hidden,   // Crate: a sibling of a hit is not a hit
		Show::Context,  // Lights
		Show::Hit,      // Torch_02
		Show::Hidden,   // Flame: under a hit, but not one itself
		Show::Hidden,   // Camera
	});

	SUBCASE("a row is opened only when something shown hangs under it")
	{
		CHECK(shown[0].childShown);        // World → Props, Lights
		CHECK(shown[1].childShown);        // Props → Torch_01
		CHECK_FALSE(shown[2].childShown);  // Torch_01: a leaf
		CHECK(shown[4].childShown);        // Lights → Torch_02
		// Torch_02 has a child, Flame, but Flame is hidden — so the hit is
		// drawn as a leaf rather than as an arrow that opens onto nothing.
		CHECK_FALSE(shown[5].childShown);
	}
}

TEST_CASE("OutlinerFilter::apply: a hit stays a hit when a deeper hit runs through it")
{
	// "l" is in Lights AND in Flame: the ancestor is a hit in its own right and
	// must not be demoted to a dimmed path row by the hit under it.
	const auto shown = OutlinerFilter::apply(rowsMatching("l"));
	CHECK(shown[4].show == Show::Hit);      // Lights
	CHECK(shown[6].show == Show::Hit);      // Flame
	CHECK(shown[5].show == Show::Context);  // Torch_02, between the two
	CHECK(shown[0].show == Show::Context);  // World
	CHECK(shown[4].childShown);
	CHECK(shown[5].childShown);
	CHECK_FALSE(shown[6].childShown);
}

TEST_CASE("OutlinerFilter::apply: nothing matches means nothing is shown")
{
	const auto shown = OutlinerFilter::apply(rowsMatching("zebra"));
	for (const Shown& r : shown)
	{
		CHECK(r.show == Show::Hidden);
		CHECK_FALSE(r.childShown);
	}
}

TEST_CASE("OutlinerFilter::apply: an empty search shows every row as a hit")
{
	auto rows = rowsMatching("");
	rows[0].matches = true;   // with no filter the panel does not run this at all,
	                          // but the rule should still be the identity
	const auto shown = OutlinerFilter::apply(rows);
	for (size_t i = 0; i < shown.size(); ++i)
	{
		CHECK(shown[i].show == Show::Hit);
		const bool hasChild = i + 1 < kDepths.size() && kDepths[i + 1] > kDepths[i];
		CHECK(shown[i].childShown == hasChild);
	}
}

TEST_CASE("OutlinerFilter::apply is closed under \"parent of\"")
{
	// The property the panel's depth-walk relies on: no shown row ever hangs
	// under a hidden one. Checked for every single-row hit in the tree.
	for (size_t hit = 1; hit < kNames.size(); ++hit)
	{
		std::vector<Row> rows;
		for (size_t i = 0; i < kNames.size(); ++i) rows.push_back({ kDepths[i], i == hit });
		const auto shown = OutlinerFilter::apply(rows);
		std::vector<size_t> path;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const auto d = static_cast<size_t>(rows[i].depth);
			if (path.size() > d) path.resize(d);
			if (shown[i].show != Show::Hidden)
				for (const size_t a : path) CHECK(shown[a].show != Show::Hidden);
			path.push_back(i);
		}
	}
}

TEST_CASE("OutlinerFilter kinds: an entity's type is the component that makes it one")
{
	HorizonWorld world;
	const Entity plain  = world.createEntity("Empty");
	world.addComponent(plain, TransformComponent{});
	const Entity light  = world.createEntity("Lamp");
	world.addComponent(light, TransformComponent{});
	world.addComponent(light, LightComponent{});
	const Entity camera = world.createEntity("Cam");
	world.addComponent(camera, CameraComponent{});
	const Entity rig    = world.createEntity("Rig");
	world.addComponent(rig, CameraRigComponent{});
	const Entity mesh   = world.createEntity("Rock");
	world.addComponent(mesh, MeshComponent{});
	world.addComponent(mesh, PrefabInstanceComponent{});
	const auto& reg = world.registry();

	const auto kindNamed = [](const char* label) -> const OutlinerFilter::Kind*
	{
		for (int i = 0; i < OutlinerFilter::kindCount(); ++i)
			if (std::string(OutlinerFilter::kindAt(i).label) == label)
				return &OutlinerFilter::kindAt(i);
		return nullptr;
	};

	SUBCASE("\"All types\" is entry zero and takes everything")
	{
		REQUIRE(OutlinerFilter::kindCount() > 1);
		CHECK(std::string(OutlinerFilter::kindAt(OutlinerFilter::kAllKinds).label) == "All types");
		for (Entity e : { plain, light, camera, rig, mesh })
			CHECK(OutlinerFilter::kindAt(OutlinerFilter::kAllKinds).test(reg, e));
	}
	SUBCASE("Light")
	{
		const auto* k = kindNamed("Light");
		REQUIRE(k != nullptr);
		CHECK(k->test(reg, light));
		CHECK_FALSE(k->test(reg, plain));
		CHECK_FALSE(k->test(reg, mesh));
	}
	SUBCASE("Camera covers the plain camera and the rig alike")
	{
		const auto* k = kindNamed("Camera");
		REQUIRE(k != nullptr);
		CHECK(k->test(reg, camera));
		CHECK(k->test(reg, rig));
		CHECK_FALSE(k->test(reg, light));
	}
	SUBCASE("an entity is every kind it carries")
	{
		CHECK(kindNamed("Mesh")->test(reg, mesh));
		CHECK(kindNamed("Prefab Instance")->test(reg, mesh));
		CHECK_FALSE(kindNamed("Mesh")->test(reg, plain));
	}
	SUBCASE("an index off the table falls back to \"All types\"")
	{
		CHECK(&OutlinerFilter::kindAt(-1) == &OutlinerFilter::kindAt(0));
		CHECK(&OutlinerFilter::kindAt(OutlinerFilter::kindCount()) == &OutlinerFilter::kindAt(0));
	}
	SUBCASE("every label is unique — a dropdown with two \"Light\" rows helps nobody")
	{
		for (int i = 0; i < OutlinerFilter::kindCount(); ++i)
			for (int j = i + 1; j < OutlinerFilter::kindCount(); ++j)
				CHECK(std::string(OutlinerFilter::kindAt(i).label) !=
				      OutlinerFilter::kindAt(j).label);
	}
}
