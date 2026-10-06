#include "doctest.h"
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/CellStreamer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ── Streaming cells, split and merged in the editor (Thema 153, Schritt 6) ────
// HE::splitSceneIntoCells is scripts/split_scene_cells.py in C++, so a scene
// can be split from the editor; HE::mergeCellsIntoWorld is the way back, so a
// split scene stays editable as one piece. The rules asserted here are the
// script's: whole top-level subtrees of placed things move, folders are looked
// through, everything that has to be there all the time stays in the base.

using json = nlohmann::json;

namespace
{
Entity spawn(HorizonWorld& w, const std::string& name, glm::vec3 pos, Entity parent = entt::null)
{
	const Entity e = w.createEntity(name);
	TransformComponent t;
	t.position = pos;
	w.addComponent(e, t);
	if (parent != entt::null) w.reparentEntity(e, parent);
	return e;
}

Entity mesh(HorizonWorld& w, const std::string& name, glm::vec3 pos, Entity parent = entt::null)
{
	const Entity e = spawn(w, name, pos, parent);
	w.registry().emplace<MeshComponent>(e);
	return e;
}

// The test scene, 100 m cells:
//   Props (folder)            → looked through, then dropped: nothing stays in it
//     RockA   mesh  50, 50    → cell 0,0
//     Lamp    point 60, 40    → cell 0,0
//     RockB   mesh 250,-30    → cell 2,-1
//     Tower   mesh 120, 10    → cell 1,0, with its child
//       Flag  mesh (local)
//   Statue    mesh  30, 30    → stays: its child is a camera
//     Eye     camera
//   Sun       directional     → stays
//   Crate     dynamic body    → stays
//   Camera    camera          → stays
void buildScene(HorizonWorld& w)
{
	const Entity props = w.createEntity("Props");
	mesh(w, "RockA", { 50.0f, 0.0f, 50.0f }, props);
	const Entity lamp = spawn(w, "Lamp", { 60.0f, 2.0f, 40.0f }, props);
	LightComponent point;
	point.type = HE::LightType::Point;
	w.registry().emplace<LightComponent>(lamp, point);
	mesh(w, "RockB", { 250.0f, 0.0f, -30.0f }, props);
	const Entity tower = mesh(w, "Tower", { 120.0f, 0.0f, 10.0f }, props);
	mesh(w, "Flag", { 0.0f, 12.0f, 0.0f }, tower);

	const Entity statue = mesh(w, "Statue", { 30.0f, 0.0f, 30.0f });
	const Entity eye = spawn(w, "Eye", { 0.0f, 2.0f, 0.0f }, statue);
	w.registry().emplace<CameraComponent>(eye);

	const Entity sun = spawn(w, "Sun", { 0.0f, 100.0f, 0.0f });
	LightComponent dir;
	dir.type = HE::LightType::Directional;
	w.registry().emplace<LightComponent>(sun, dir);

	const Entity crate = mesh(w, "Crate", { 20.0f, 0.0f, 20.0f });
	RigidBodyComponent body;
	body.type = HE::RigidBodyType::Dynamic;
	w.registry().emplace<RigidBodyComponent>(crate, body);

	const Entity cam = spawn(w, "Camera", { 0.0f, 5.0f, -10.0f });
	w.registry().emplace<CameraComponent>(cam);
	HE::propagateTransforms(w);
}

json sceneJson(const HorizonWorld& w)
{
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(w, bytes));
	return HE::parseSceneCbor(bytes);
}

std::set<std::string> namesIn(const json& scene)
{
	std::set<std::string> names;
	for (const json& e : scene["entities"]) names.insert(e.value("name", std::string()));
	return names;
}

// name → world position, of every named entity but the world root.
std::map<std::string, glm::vec3> placed(HorizonWorld& w)
{
	std::map<std::string, glm::vec3> out;
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (e != w.rootEntity() && w.registry().all_of<TransformComponent>(e))
			out[n.name] = HE::worldPositionOf(w, e);
	return out;
}
} // namespace

TEST_CASE("splitSceneIntoCells: placed things go to their square, the rest stays in the base")
{
	HorizonWorld world;
	buildScene(world);
	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	o.dir      = "Content/W.cells";
	const HE::CellSplitResult r = HE::splitSceneIntoCells(sceneJson(world), o);
	REQUIRE(r.error.empty());

	REQUIRE(r.cells.size() == 3);
	CHECK(r.cells[0].x == 0);  CHECK(r.cells[0].z == 0);  CHECK(r.cells[0].entities == 2);
	CHECK(r.cells[1].x == 1);  CHECK(r.cells[1].z == 0);  CHECK(r.cells[1].entities == 2);
	CHECK(r.cells[2].x == 2);  CHECK(r.cells[2].z == -1); CHECK(r.cells[2].entities == 1);
	CHECK(r.moved == 5);
	CHECK(namesIn(r.cells[0].scene) == std::set<std::string>{ "Cell 0,0", "RockA", "Lamp" });
	CHECK(namesIn(r.cells[1].scene) == std::set<std::string>{ "Cell 1,0", "Tower", "Flag" });

	const std::set<std::string> base = namesIn(r.base);
	for (const char* stays : { "Statue", "Eye", "Sun", "Crate", "Camera" }) CHECK(base.count(stays) == 1);
	for (const char* goes : { "RockA", "Lamp", "RockB", "Tower", "Flag", "Props" }) CHECK(base.count(goes) == 0);

	// The manifest, with the script's default radii.
	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(r.base["cells"].dump(), m));
	CHECK(m.cellSize == 100.0f);
	CHECK(m.loadRadius == 150.0f);
	CHECK(m.unloadRadius == doctest::Approx(187.5f));
	CHECK(m.dir == "Content/W.cells");
	REQUIRE(m.cells.size() == 3);
	CHECK(m.cells[2].entities == 1u);

	// A split scene is not split twice, and a scene without one root is refused.
	CHECK_FALSE(HE::splitSceneIntoCells(r.base, o).error.empty());
	CHECK_FALSE(HE::splitSceneIntoCells(json{ { "entities", json::array() } }, o).error.empty());
	HE::CellSplitOptions bad = o;
	bad.cellSize = 0.0f;
	CHECK_FALSE(HE::splitSceneIntoCells(sceneJson(world), bad).error.empty());
}

TEST_CASE("Cells split in the editor: the world becomes the base, merge puts every entity back")
{
	HorizonWorld world;
	buildScene(world);
	const std::map<std::string, glm::vec3> before = placed(world);

	std::map<std::string, std::string> files;   // project-relative path → text
	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	o.dir      = "Content/W.cells";
	const HE::CellSplitResult r = HE::splitWorldIntoCells(world, o,
		[&](const std::string& path, const std::string& text) { files[path] = text; return true; });
	REQUIRE(r.error.empty());
	CHECK(files.size() == 3);
	CHECK(files.count("Content/W.cells/cell_2_-1.hescene") == 1);
	CHECK_FALSE(world.cellManifestJson().empty());
	const std::map<std::string, glm::vec3> base = placed(world);
	CHECK(base.count("RockA") == 0);
	CHECK(base.count("Statue") == 1);

	// The game streams what the editor split: cell 0,0 comes in around 50, 50.
	{
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(world.cellManifestJson(), m));
		HE::CellStreamer s;
		s.begin(m, [&](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
		{
			const auto it = files.find(path);
			if (it == files.end()) return {};
			const std::string text = it->second;
			return [text](std::vector<uint8_t>& out) { out.assign(text.begin(), text.end()); return true; };
		}, {});
		HorizonWorld game;
		REQUIRE(SceneSerializer().loadFromMemory(game, json::to_cbor(sceneJson(world))));
		for (int i = 0; i < 2000 && !s.isLoaded(0, 0); ++i)
		{
			s.update(game, { 50.0, 1.7, 50.0 }, glm::vec3(0.0f), 4.0);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(s.isLoaded(0, 0));
		const std::map<std::string, glm::vec3> streamed = placed(game);
		REQUIRE(streamed.count("RockA") == 1);
		CHECK(streamed.at("RockA") == before.at("RockA"));
		s.clear(game);
	}

	// A cell that cannot be read stops the merge before anything changes.
	std::string error;
	CHECK_FALSE(HE::mergeCellsIntoWorld(world,
		[&](const std::string& path, std::vector<uint8_t>& out)
		{
			if (path.find("cell_2_-1") != std::string::npos) return false;
			const std::string& t = files.at(path);
			out.assign(t.begin(), t.end());
			return true;
		}, &error));
	CHECK(error.find("cell_2_-1") != std::string::npos);
	CHECK(placed(world) == base);
	CHECK_FALSE(world.cellManifestJson().empty());

	// The merge: every entity back where it stood, no cell roots, no manifest.
	size_t merged = 0;
	REQUIRE(HE::mergeCellsIntoWorld(world,
		[&](const std::string& path, std::vector<uint8_t>& out)
		{
			const std::string& t = files.at(path);
			out.assign(t.begin(), t.end());
			return true;
		}, &error, &merged));
	CHECK(merged == 5);
	CHECK(world.cellManifestJson().empty());
	HE::propagateTransforms(world);
	std::map<std::string, glm::vec3> after = placed(world);
	CHECK(after.count("Cell 0,0") == 0);
	after.erase("Props");   // the folder the split dropped, nothing of its own
	std::map<std::string, glm::vec3> expect = before;
	expect.erase("Props");
	CHECK(after == expect);
	// The moved subtrees hang off the world root again, with their children.
	for (auto [e, n] : world.registry().view<NameComponent>().each())
		if (n.name == "Flag")
		{
			const Entity parent = world.registry().get<HierarchyComponent>(e).parent;
			CHECK(world.registry().get<NameComponent>(parent).name == "Tower");
		}

	// Merged again: nothing left to merge.
	CHECK_FALSE(HE::mergeCellsIntoWorld(world, {}, &error));
}
