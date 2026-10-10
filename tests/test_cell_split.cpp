#include "doctest.h"
#include "TestFsUtil.h"
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
#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
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
//   Props (folder)            → looked through, then dropped: nothing stays in it;
//                               the merge rebuilds it from the cells' cellFolders
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

// name → stable id, of every named entity but the world root.
std::map<std::string, HE::UUID> idsByName(HorizonWorld& w)
{
	std::map<std::string, HE::UUID> out;
	for (auto [e, n, id] : w.registry().view<NameComponent, EntityIdComponent>().each())
		if (e != w.rootEntity()) out[n.name] = id.id;
	return out;
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
	const std::map<std::string, HE::UUID>  idsBefore = idsByName(world);

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
	const std::map<std::string, glm::vec3> after = placed(world);
	CHECK(after.count("Cell 0,0") == 0);
	CHECK(after == before);
	// …under the identities they had before the split, the dropped Props folder
	// included: whatever in the base refers to one of them (a joint, a script)
	// still finds it.
	CHECK(idsByName(world) == idsBefore);
	// …and in the hierarchy they had: Props is back under the root, holding its
	// four subtrees in their old order, the Flag under the Tower.
	auto& reg = world.registry();
	Entity props = entt::null;
	for (auto [e, n] : reg.view<NameComponent>().each())
	{
		if (n.name == "Props") props = e;
		if (n.name == "Flag")
			CHECK(reg.get<NameComponent>(reg.get<HierarchyComponent>(e).parent).name == "Tower");
	}
	REQUIRE((props != entt::null));
	CHECK((reg.get<HierarchyComponent>(props).parent == world.rootEntity()));
	std::vector<std::string> order;
	for (Entity c : reg.get<HierarchyComponent>(props).children) order.push_back(reg.get<NameComponent>(c).name);
	CHECK(order == std::vector<std::string>{ "RockA", "Lamp", "RockB", "Tower" });

	// Merged again: nothing left to merge.
	CHECK_FALSE(HE::mergeCellsIntoWorld(world, {}, &error));
}

// ─── Cells that keep their ids (Thema 164, step 2a) ───────────────────────────
// A cell used to come in with fresh ids on every load, so anything that referred
// to one of its entities by id — a prefab placement's bindings above all — pointed
// at nothing, and prefab instances therefore had to stay in the base. These tests
// pin the other half: a cell the C++ splitter wrote carries a "streaming" head,
// the streamer then loads it with the ids it was saved with, and a village of
// prefab houses can stream.

namespace
{
using Files = std::map<std::string, std::string>;   // project-relative path → file bytes

// What GameApplication's reader hands a worker: the bytes of a cell, by path.
HE::CellStreamer::Reader readerOver(const Files& files)
{
	return [&files](const std::string& path) -> std::function<bool(std::vector<uint8_t>&)>
	{
		const auto it = files.find(path);
		if (it == files.end()) return {};
		const std::string bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out.assign(bytes.begin(), bytes.end()); return true; };
	};
}

// Pumps the streamer from one standpoint until `done`, or a few seconds have passed.
template <class Done>
void streamAt(HE::CellStreamer& s, HorizonWorld& world, const glm::dvec3& at, Done done)
{
	for (int i = 0; i < 3000 && !done(); ++i)
	{
		s.update(world, at, glm::vec3(0.0f), 4.0);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

struct House { Entity root, wall, door; };

// A placed prefab: the root carries the PrefabInstanceComponent, whose bindings pair
// a record of the (made-up) prefab asset with the entity that stands for it, root
// included, and the door has been nudged by hand, which is an override.
House placeHouse(HorizonWorld& w, const std::string& name, glm::vec3 pos)
{
	House h;
	h.root = mesh(w, name, pos);
	h.wall = mesh(w, name + " Wall", { 2.0f, 0.0f, 0.0f }, h.root);
	h.door = mesh(w, name + " Door", { 0.0f, 0.0f, 3.0f }, h.root);
	PrefabInstanceComponent inst;
	inst.asset = HE::UUID::generate();
	for (Entity e : { h.root, h.wall, h.door })
		inst.bindings.push_back({ HE::UUID::generate(), w.entityId(e) });
	inst.setOverride(inst.bindings[2].templateEntity, "transform", "position");
	w.registry().emplace<PrefabInstanceComponent>(h.root, inst);
	return h;
}

Entity named(HorizonWorld& w, const std::string& name)
{
	for (auto [e, n] : w.registry().view<NameComponent>().each())
		if (n.name == name) return e;
	return entt::null;
}

Entity childNamed(HorizonWorld& w, Entity parent, const std::string& name)
{
	auto& reg = w.registry();
	if (const auto* h = reg.try_get<HierarchyComponent>(parent))
		for (Entity c : h->children)
			if (reg.get<NameComponent>(c).name == name) return c;
	return entt::null;
}

// {uuid, components} of the record called `name` in a scene's JSON, or null.
json recordOf(const json& scene, const std::string& name)
{
	for (const json& e : scene["entities"])
		if (e.value("name", std::string()) == name)
			return json{ { "uuid", e["uuid"] }, { "components", e.value("components", json::object()) } };
	return nullptr;
}

// The scene split in the editor: the cell files by path, the world left as the base.
Files splitInto(HorizonWorld& world, HE::CellSplitResult* result = nullptr)
{
	Files files;
	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	o.dir      = "Content/V.cells";
	HE::CellSplitResult r = HE::splitWorldIntoCells(world, o,
		[&](const std::string& path, const std::string& text) { files[path] = text; return true; });
	REQUIRE(r.error.empty());
	if (result) *result = std::move(r);
	return files;
}

// A game world with the base of `editorWorld` loaded and a streamer over `files`.
struct Game
{
	HorizonWorld     world;
	HE::CellStreamer streamer;
	Game(HorizonWorld& editorWorld, const Files& files)
	{
		REQUIRE(SceneSerializer().loadFromMemory(world, json::to_cbor(sceneJson(editorWorld))));
		HE::CellManifest m;
		REQUIRE(HE::CellManifest::parse(world.cellManifestJson(), m));
		streamer.begin(m, readerOver(files), {});
	}
	~Game() { streamer.clear(world); }
};
} // namespace

TEST_CASE("A prefab house in a cell: unloaded and loaded again it is the same entities, bindings intact")
{
	HorizonWorld world;
	const House house = placeHouse(world, "House", { 50.0f, 0.0f, 50.0f });   // cell 0,0
	placeHouse(world, "Barn", { 450.0f, 0.0f, 50.0f });                        // cell 4,0
	const Entity cam = spawn(world, "Camera", { 0.0f, 5.0f, -10.0f });
	world.registry().emplace<CameraComponent>(cam);

	const HE::UUID idRoot = world.entityId(house.root), idWall = world.entityId(house.wall),
	               idDoor = world.entityId(house.door);
	const PrefabInstanceComponent before = world.registry().get<PrefabInstanceComponent>(house.root);
	const json sceneBefore = sceneJson(world);

	HE::CellSplitResult r;
	const Files files = splitInto(world, &r);
	// A placement is a cell thing now: both houses left the base.
	REQUIRE(r.cells.size() == 2);
	CHECK(r.cells[0].entities == 3);
	CHECK(r.cells[1].entities == 3);
	CHECK(placed(world).count("House") == 0);
	CHECK(placed(world).count("Barn") == 0);
	CHECK(placed(world).count("Camera") == 1);

	Game game(world, files);
	HorizonWorld&     g = game.world;
	HE::CellStreamer& s = game.streamer;
	const glm::dvec3 atHouse(50.0, 1.7, 50.0), atBarn(450.0, 1.7, 50.0);

	// The house as the game holds it: the ids it was saved with, the placement's
	// data as it was, and every binding resolving to the entity it names.
	const auto expectHouse = [&](const char* when)
	{
		INFO(when);
		auto& reg = g.registry();
		const Entity root = g.findByEntityId(idRoot);
		REQUIRE((root != entt::null));
		CHECK(reg.get<NameComponent>(root).name == "House");
		CHECK((g.findByEntityId(idWall) == childNamed(g, root, "House Wall")));
		CHECK((g.findByEntityId(idDoor) == childNamed(g, root, "House Door")));
		const auto* inst = reg.try_get<PrefabInstanceComponent>(root);
		REQUIRE(inst != nullptr);
		CHECK(inst->asset == before.asset);
		CHECK(inst->bindings == before.bindings);
		CHECK(inst->overrides == before.overrides);
		for (const auto& b : inst->bindings)
		{
			const Entity e = g.findByEntityId(b.instanceEntity);
			REQUIRE((e != entt::null));
			CHECK(g.entityId(e) == b.instanceEntity);
			CHECK(g.isAncestorOf(root, e));
		}
		// Not just the ids: the records are what they were before the split.
		const json now = sceneJson(g);
		for (const char* n : { "House", "House Wall", "House Door" })
			CHECK(recordOf(now, n) == recordOf(sceneBefore, n));
	};

	streamAt(s, g, atHouse, [&] { return s.isLoaded(0, 0); });
	REQUIRE(s.isLoaded(0, 0));
	expectHouse("first load");

	// Away: the house is gone, and its ids with it.
	streamAt(s, g, atBarn, [&] { return s.isLoaded(4, 0) && !s.isLoaded(0, 0); });
	REQUIRE_FALSE(s.isLoaded(0, 0));
	CHECK((g.findByEntityId(idRoot) == entt::null));
	CHECK((g.findByEntityId(idWall) == entt::null));
	CHECK((g.findByEntityId(idDoor) == entt::null));
	CHECK((named(g, "House Door") == entt::null));

	// And back: the same entities again.
	streamAt(s, g, atHouse, [&] { return s.isLoaded(0, 0); });
	REQUIRE(s.isLoaded(0, 0));
	expectHouse("after an unload");
	CHECK(s.stats().idCollisions == 0);
	CHECK(s.stats().unloads >= 1);

	// The way back from the split gives the editor the same identities too.
	std::string error;
	REQUIRE(HE::mergeCellsIntoWorld(world,
		[&](const std::string& path, std::vector<uint8_t>& out)
		{
			const std::string& t = files.at(path);
			out.assign(t.begin(), t.end());
			return true;
		}, &error));
	CHECK((world.findByEntityId(idRoot) == named(world, "House")));
	CHECK((world.findByEntityId(idDoor) == named(world, "House Door")));
	const Entity merged = named(world, "House");
	REQUIRE((merged != entt::null));
	const auto* mergedInstance = world.registry().try_get<PrefabInstanceComponent>(merged);
	REQUIRE(mergedInstance != nullptr);
	CHECK(mergedInstance->bindings == before.bindings);
	CHECK(mergedInstance->overrides == before.overrides);
}

TEST_CASE("Cells without the streaming head load as they always did, with fresh ids")
{
	// The negative control for the test above, and the promise to old projects: a
	// cell written by scripts/split_scene_cells.py, or by the editor before the head
	// existed, has no "streaming" object, and its ids are still minted on load.
	HorizonWorld world;
	const House house = placeHouse(world, "House", { 50.0f, 0.0f, 50.0f });
	const HE::UUID idRoot = world.entityId(house.root);
	Files files = splitInto(world);
	for (auto& [path, text] : files)
	{
		json cell = json::parse(text);
		REQUIRE(cell.contains("streaming"));
		cell.erase("streaming");
		text = cell.dump();
	}

	Game game(world, files);
	HorizonWorld& g = game.world;
	streamAt(game.streamer, g, { 50.0, 1.7, 50.0 }, [&] { return game.streamer.isLoaded(0, 0); });
	REQUIRE(game.streamer.isLoaded(0, 0));
	const Entity root = named(g, "House");
	REQUIRE((root != entt::null));
	CHECK(g.entityId(root) != idRoot);
	CHECK((g.findByEntityId(idRoot) == entt::null));
	// …which is exactly why a placement could not stream before: nothing it names exists.
	const auto* inst = g.registry().try_get<PrefabInstanceComponent>(root);
	REQUIRE(inst != nullptr);
	for (const auto& b : inst->bindings)
		CHECK((g.findByEntityId(b.instanceEntity) == entt::null));
	CHECK(game.streamer.stats().idCollisions == 0);
}

TEST_CASE("A cell whose ids the world already holds takes none of them and says how many")
{
	// A copied cell file, or a project holding two copies of one scene: loading
	// the cell with its stored ids would give two entities one identity.
	HorizonWorld world;
	const House house = placeHouse(world, "House", { 50.0f, 0.0f, 50.0f });
	const HE::UUID idRoot = world.entityId(house.root), idWall = world.entityId(house.wall),
	               idDoor = world.entityId(house.door);
	const Files files = splitInto(world);

	Game game(world, files);
	HorizonWorld& g = game.world;
	const Entity squatter = g.createEntity("Squatter");
	g.setEntityId(squatter, idDoor);

	streamAt(game.streamer, g, { 50.0, 1.7, 50.0 }, [&] { return game.streamer.isLoaded(0, 0); });
	REQUIRE(game.streamer.isLoaded(0, 0));
	CHECK(game.streamer.stats().idCollisions == 1);
	// Nobody was robbed: the door's id is still the squatter's ...
	CHECK((g.findByEntityId(idDoor) == squatter));
	// ... the door of the house got another, and the rest kept the ids it was saved with.
	const Entity root = g.findByEntityId(idRoot);
	REQUIRE((root != entt::null));
	const Entity door = childNamed(g, root, "House Door");
	REQUIRE((door != entt::null));
	CHECK((door != squatter));
	CHECK(g.entityId(door) != idDoor);
	CHECK((g.findByEntityId(idWall) == childNamed(g, root, "House Wall")));
}

TEST_CASE("The cell head survives the export's load and save, so a shipped cell keeps its ids")
{
	// ExportDialogPanel packs every .hescene of a project by loading it into a world
	// and saving that to memory. A top-level key the world does not carry is gone
	// after that — the manifest rides in the world for this reason, and so does the
	// head, or a shipped game would stream every cell with fresh ids again.
	HorizonWorld world;
	const House house = placeHouse(world, "House", { 50.0f, 0.0f, 50.0f });
	const HE::UUID idRoot = world.entityId(house.root);
	const PrefabInstanceComponent before = world.registry().get<PrefabInstanceComponent>(house.root);
	const Files files = splitInto(world);
	REQUIRE(files.size() == 1);

	const auto dir = std::filesystem::temp_directory_path() / "he_cell_head_export";
	he_test::removeAllQuiet(dir);
	std::filesystem::create_directories(dir);
	Files packed;
	for (const auto& [path, text] : files)
	{
		const auto file = dir / "cell.hescene";
		{ std::ofstream out(file, std::ios::binary); out << text; }
		HorizonWorld loaded;
		SceneSerializer ser;
		REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
		CHECK_FALSE(loaded.cellHeadJson().empty());
		std::vector<uint8_t> bytes;
		REQUIRE(ser.saveToMemory(loaded, bytes));
		const json cbor = HE::parseSceneCbor(bytes);
		REQUIRE(cbor.contains("streaming"));
		CHECK(cbor["streaming"].value("version", 0) == HE::kCellFormatVersion);
		packed[path] = std::string(bytes.begin(), bytes.end());
		loaded.clear();
		CHECK(loaded.cellHeadJson().empty());
	}
	he_test::removeAllQuiet(dir);

	// Streamed from the packed bytes, as the shipped game reads them.
	Game game(world, packed);
	HorizonWorld& g = game.world;
	streamAt(game.streamer, g, { 50.0, 1.7, 50.0 }, [&] { return game.streamer.isLoaded(0, 0); });
	REQUIRE(game.streamer.isLoaded(0, 0));
	const Entity root = g.findByEntityId(idRoot);
	REQUIRE((root != entt::null));
	CHECK(g.registry().get<PrefabInstanceComponent>(root).bindings == before.bindings);
	// The base never takes a cell's head: it is not a cell.
	CHECK(g.cellHeadJson().empty());
}

TEST_CASE("splitSceneIntoCells: the manifest says version 2 and how many bodies each cell holds, the head says it too")
{
	// bodies is the number of entities that can own a physics body — a rigid body or a
	// collider — so a loader can see what a cell will cost before it builds it.
	const auto record = [](std::uint64_t n, const char* name, json comps)
	{
		comps["transform"] = { { "position", { 50.0, 0.0, 50.0 } } };
		return json{ { "uuid", json::array({ 0x4000 + n, n }) }, { "name", name },
		             { "parent", json::array({ 0x4000, 1 }) }, { "components", std::move(comps) } };
	};
	json ents = json::array();
	ents.push_back({ { "uuid", json::array({ 0x4000, 1 }) }, { "name", "World" }, { "parent", nullptr },
	                 { "children", json::array({ json::array({ 0x4002, 2 }), json::array({ 0x4003, 3 }),
	                                              json::array({ 0x4004, 4 }) }) } });
	ents.push_back(record(2, "Wall",    { { "mesh", json::object() }, { "rigidbody", { { "type", 0 } } },
	                                      { "collider", json::object() } }));
	ents.push_back(record(3, "Trigger", { { "collider", json::object() } }));
	ents.push_back(record(4, "Statue",  { { "mesh", json::object() } }));
	HE::CellSplitOptions o;
	o.cellSize = 100.0f;
	o.dir      = "Content/W.cells";
	const HE::CellSplitResult r = HE::splitSceneIntoCells(json{ { "version", "1.1" }, { "entities", ents } }, o);
	REQUIRE(r.error.empty());
	REQUIRE(r.cells.size() == 1);
	CHECK(r.cells[0].entities == 3);
	CHECK(r.cells[0].bodies == 2);

	const json& head = r.cells[0].scene["streaming"];
	CHECK(head["version"] == HE::kCellFormatVersion);
	CHECK(head["cell"] == json::array({ 0, 0 }));
	CHECK(head["bodies"] == 2);
	CHECK(head["classes"]["static"] == 3);
	CHECK(head["classes"]["stateful"] == 0);

	HE::CellManifest m;
	REQUIRE(HE::CellManifest::parse(r.base["cells"].dump(), m));
	CHECK(m.version == HE::kCellFormatVersion);
	REQUIRE(m.cells.size() == 1);
	CHECK(m.cells[0].entities == 3u);
	CHECK(m.cells[0].bodies == 2u);
}

TEST_CASE("splitSceneIntoCells: which components may move into a cell, and which keep their subtree in the base")
{
	// The class table is the one place that decides. Each key below is a scene
	// component key; the ones that move are placed things whose state is in the
	// file and that rebuild from it, everything else stays resident — also a key
	// nobody has decided about yet.
	const auto lone = [](const std::string& key, json block)
	{
		json comps = { { "transform", { { "position", { 50.0, 0.0, 50.0 } } } } };
		comps[key] = std::move(block);
		json ents = json::array();
		ents.push_back({ { "uuid", json::array({ 0x5000, 1 }) }, { "name", "World" }, { "parent", nullptr },
		                 { "children", json::array({ json::array({ 0x5000, 2 }) }) } });
		ents.push_back({ { "uuid", json::array({ 0x5000, 2 }) }, { "name", "Thing" },
		                 { "parent", json::array({ 0x5000, 1 }) }, { "components", std::move(comps) } });
		HE::CellSplitOptions o;
		o.cellSize = 100.0f;
		o.dir      = "Content/W.cells";
		return HE::splitSceneIntoCells(json{ { "version", "1.1" }, { "entities", ents } }, o);
	};
	const auto moves = [&](const std::string& key, json block = json::object())
	{
		const HE::CellSplitResult r = lone(key, std::move(block));
		return r.error.empty() && r.moved == 1;
	};

	for (const char* key : { "mesh", "material", "lod", "collider", "decal", "inactive",
	                         "prefab", "particlesystem", "skeletalmesh", "animator", "animatorblend",
	                         "propertyanimator" })
		CHECK_MESSAGE(moves(key), "should move: ", key);
	CHECK(moves("light", { { "type", 1 } }));       // point
	CHECK(moves("light", { { "type", 2 } }));       // spot
	CHECK(moves("rigidbody", { { "type", 0 } }));   // static

	for (const char* key : { "audiosource", "audiolistener", "animationlayers", "rootmotion",
	                         "animstatemachine", "ik", "sequenceplayer", "script", "saveState",
	                         "navagent", "navmesh", "characterController", "movement", "camera",
	                         "cameraRig", "joint", "rope", "trail", "network", "environment", "weather",
	                         "terrain", "foliage", "uicanvas", "uielement", "uitext", "uiimage", "uibutton",
	                         "transform2d", "aComponentNobodyHasDecidedAbout" })
		CHECK_MESSAGE(!moves(key), "should stay in the base: ", key);
	CHECK_FALSE(moves("light", { { "type", 0 } }));       // directional
	CHECK_FALSE(moves("rigidbody", { { "type", 1 } }));   // dynamic
	CHECK_FALSE(moves("rigidbody", { { "type", 2 } }));   // kinematic
}

TEST_CASE("splitSceneIntoCells: a placement whose bindings leave its subtree moves only together with what it binds")
{
	// A binding names an entity of the placement by id. One that names an entity
	// outside the subtree (a child dragged out of the house) would dangle as soon as
	// the two sit in different cells, or one in the base. Since step 3a the two are one
	// cluster (the ref hull): they go into one cell together when both can move, and both
	// stay in the base when the bound one cannot. (Step 2a kept the placement in the base
	// in both cases.)
	const auto id = [](std::uint64_t n) { return json::array({ 0x6000 + n, n }); };
	const auto placement = [&](std::uint64_t root, std::uint64_t child, std::uint64_t bound, const char* name)
	{
		json prefab = json::object();
		prefab["asset"] = id(90);
		prefab["bindings"] = json::array();
		prefab["bindings"].push_back({ { "template", id(91) }, { "entity", id(root) } });
		prefab["bindings"].push_back({ { "template", id(92) }, { "entity", id(bound) } });
		json rootComps = json::object();
		rootComps["transform"] = { { "position", { 50.0, 0.0, 50.0 } } };
		rootComps["prefab"]    = std::move(prefab);
		json rootRecord = json::object();
		rootRecord["uuid"]       = id(root);
		rootRecord["name"]       = name;
		rootRecord["parent"]     = id(1);
		rootRecord["children"]   = json::array({ id(child) });
		rootRecord["components"] = std::move(rootComps);
		json childRecord = json::object();
		childRecord["uuid"]       = id(child);
		childRecord["name"]       = std::string(name) + " Door";
		childRecord["parent"]     = id(root);
		childRecord["components"] = { { "mesh", json::object() } };
		return std::vector<json>{ rootRecord, childRecord };
	};

	// `lampComponents` is what the Lamp beside the placement carries: a mesh (it can
	// move) or a script (it cannot).
	const auto split = [&](json lampComponents)
	{
		json ents = json::array();
		ents.push_back({ { "uuid", id(1) }, { "name", "World" }, { "parent", nullptr },
		                 { "children", json::array({ id(10), id(20), id(30) }) } });
		for (const json& e : placement(10, 11, 11, "Kept")) ents.push_back(e);      // binds its own child
		for (const json& e : placement(20, 21, 30, "Split")) ents.push_back(e);     // binds the Lamp beside it
		lampComponents["transform"] = { { "position", { 60.0, 0.0, 60.0 } } };
		ents.push_back({ { "uuid", id(30) }, { "name", "Lamp" }, { "parent", id(1) },
		                 { "components", std::move(lampComponents) } });
		HE::CellSplitOptions o;
		o.cellSize = 100.0f;
		o.dir      = "Content/W.cells";
		return HE::splitSceneIntoCells(json{ { "version", "1.1" }, { "entities", ents } }, o);
	};

	SUBCASE("the Lamp can move: placement and Lamp are one cluster and go into the cell together")
	{
		const HE::CellSplitResult r = split({ { "mesh", json::object() } });
		REQUIRE(r.error.empty());
		REQUIRE(r.cells.size() == 1);
		const std::set<std::string> inCell = namesIn(r.cells[0].scene), inBase = namesIn(r.base);
		CHECK(inCell.count("Kept") == 1);
		CHECK(inCell.count("Kept Door") == 1);
		CHECK(inCell.count("Lamp") == 1);
		CHECK(inCell.count("Split") == 1);
		CHECK(inCell.count("Split Door") == 1);
		CHECK(inBase.count("Split") == 0);
		CHECK(r.clusters == 1u);
		CHECK(r.keptForRefs == 0u);
		// The head names the cluster by the ids of its subtrees, and the cell lists them side
		// by side in the root's children, in the order the traversal met them.
		const json& head = r.cells[0].scene["streaming"];
		REQUIRE(head.contains("clusters"));
		REQUIRE(head["clusters"].size() == 1u);
		CHECK(head["clusters"][0] == json::array({ id(20), id(30) }));
		const json& rootKids = r.cells[0].scene["entities"][0]["children"];
		CHECK(rootKids == json::array({ id(10), id(20), id(30) }));
	}

	SUBCASE("the Lamp cannot move: placement and Lamp both stay in the base")
	{
		const HE::CellSplitResult r = split({ { "script", { { "asset", id(77) } } } });
		REQUIRE(r.error.empty());
		REQUIRE(r.cells.size() == 1);
		const std::set<std::string> inCell = namesIn(r.cells[0].scene), inBase = namesIn(r.base);
		CHECK(inCell.count("Kept") == 1);
		CHECK(inCell.count("Kept Door") == 1);
		CHECK(inBase.count("Lamp") == 1);
		CHECK(inBase.count("Split") == 1);
		CHECK(inBase.count("Split Door") == 1);
		CHECK(inCell.count("Split") == 0);
		CHECK(r.keptForRefs == 1u);   // the placement; the Lamp was not movable to begin with
		CHECK(r.clusters == 0u);
		CHECK_FALSE(r.cells[0].scene["streaming"].contains("clusters"));
	}
}

TEST_CASE("splitSceneIntoCells: what the base refers to inside a cell keeps it in the base, in either direction")
{
	// The ref hull (Thema 164, step 3a). A door of the base hinged to a frame that stands in a
	// cell: the frame would be gone with the cell and the joint with it. Whatever names another
	// entity by id keeps both together; one side that cannot move keeps both in the base. The
	// reference is found in the component block, wherever it is, not in a list of fields: the
	// "joint" here, and a made-up field of a mesh block nobody told the splitter about.
	const auto id = [](std::uint64_t n) { return json::array({ 0x7000 + n, n }); };
	const auto record = [&](std::uint64_t n, const char* name, json components, double x = 50.0)
	{
		components["transform"] = { { "position", { x, 0.0, 50.0 } } };
		return json{ { "uuid", id(n) }, { "name", name }, { "parent", id(1) }, { "components", std::move(components) } };
	};
	const auto world = [&](std::vector<json> things)
	{
		json kids = json::array();
		for (const json& t : things) kids.push_back(t["uuid"]);
		json ents = json::array();
		ents.push_back({ { "uuid", id(1) }, { "name", "World" }, { "parent", nullptr }, { "children", kids } });
		for (json& t : things) ents.push_back(std::move(t));
		HE::CellSplitOptions o;
		o.cellSize = 100.0f;
		o.dir      = "Content/R.cells";
		return HE::splitSceneIntoCells(json{ { "version", "1.1" }, { "entities", ents } }, o);
	};
	const json solid = { { "mesh", json::object() } };

	SUBCASE("no reference: both move, as they always did")
	{
		const auto r = world({ record(10, "Door", { { "mesh", json::object() } }), record(11, "Frame", solid, 60.0) });
		REQUIRE(r.error.empty());
		CHECK(namesIn(r.base).count("Door") == 0);
		CHECK(namesIn(r.base).count("Frame") == 0);
		CHECK(r.keptForRefs == 0u);
	}
	SUBCASE("a joint of the base aimed at a frame in a cell keeps the frame in the base")
	{
		// The door carries a joint, which the table keeps in the base; it names the frame.
		json door = solid;
		door["joint"] = { { "type", 0 }, { "target", id(11) } };
		const auto r = world({ record(10, "Door", door), record(11, "Frame", solid, 60.0), record(12, "Rock", solid, 70.0) });
		REQUIRE(r.error.empty());
		const auto base = namesIn(r.base);
		CHECK(base.count("Door") == 1);
		CHECK(base.count("Frame") == 1);                // would have moved by its components
		CHECK(base.count("Rock") == 0);                 // nothing refers to it: it moves
		CHECK(r.keptForRefs == 1u);
		// The reference holds in the file the base writes: the base still has both ends.
		bool joint = false;
		for (const json& e : r.base["entities"])
			if (e.value("name", std::string()) == "Door") joint = e["components"].contains("joint");
		CHECK(joint);
	}
	SUBCASE("a movable thing that names something in the base stays with it")
	{
		// A lamp whose mesh block carries the id of a pole in the base (a script keeps the pole
		// there). The field is made up, and it is the only thing that tells the lamp from a
		// free one: that is the point of finding references by their value.
		json lamp = { { "mesh", { { "partner", id(11) } } } };
		json pole = { { "script", { { "asset", id(77) } } } };
		const auto r = world({ record(10, "Lamp", lamp), record(11, "Pole", pole, 60.0) });
		REQUIRE(r.error.empty());
		const auto base = namesIn(r.base);
		CHECK(base.count("Lamp") == 1);
		CHECK(base.count("Pole") == 1);
		CHECK(r.keptForRefs == 1u);
	}
	SUBCASE("two things that name each other move together, into the cell of the first")
	{
		json a = { { "mesh", { { "partner", id(11) } } } };
		json b = { { "mesh", { { "partner", id(10) } } } };
		// A is in cell (0,0), B in cell (3,0) by its own position; the cluster goes where A is.
		const auto r = world({ record(10, "A", a, 50.0), record(12, "Elsewhere", solid, 350.0), record(11, "B", b, 350.0) });
		REQUIRE(r.error.empty());
		REQUIRE(r.cells.size() == 2);
		const auto first = namesIn(r.cells[0].scene), second = namesIn(r.cells[1].scene);
		REQUIRE(r.cells[0].x == 0);
		REQUIRE(r.cells[1].x == 3);
		CHECK(first.count("A") == 1);
		CHECK(first.count("B") == 1);                 // moved to A's cell, off its own square
		CHECK(second.count("Elsewhere") == 1);
		CHECK(second.count("B") == 0);
		CHECK(r.clusters == 1u);
		CHECK(r.keptForRefs == 0u);
	}
	SUBCASE("an id that is not an entity of the scene is no reference")
	{
		// An asset id, or a pair of small numbers, in a field. Nothing to keep together.
		json thing = { { "mesh", { { "asset", id(9999) }, { "range", json::array({ 3, 4 }) } } } };
		const auto r = world({ record(10, "Thing", thing) });
		REQUIRE(r.error.empty());
		CHECK(namesIn(r.base).count("Thing") == 0);
		CHECK(r.keptForRefs == 0u);
	}
}

namespace
{
// One decorative component through a cell: the entity has a mesh and the
// component, goes into a cell with the split and comes back out of the streamer
// with the component and every value in it as saved.
template <class C>
void comesBackFromACell(const char* what, const C& component)
{
	INFO(what);
	HorizonWorld world;
	const Entity thing = mesh(world, "Thing", { 50.0f, 0.0f, 50.0f });
	world.registry().emplace<C>(thing, component);
	const json before = recordOf(sceneJson(world), "Thing");
	REQUIRE(!before.is_null());
	REQUIRE(before["components"].size() >= 3);   // transform, mesh and the component

	HE::CellSplitResult r;
	const Files files = splitInto(world, &r);
	REQUIRE(r.cells.size() == 1);
	CHECK(r.cells[0].entities == 1);
	CHECK(placed(world).count("Thing") == 0);

	Game game(world, files);
	streamAt(game.streamer, game.world, { 50.0, 1.7, 50.0 }, [&] { return game.streamer.isLoaded(0, 0); });
	REQUIRE(game.streamer.isLoaded(0, 0));
	const Entity back = named(game.world, "Thing");
	REQUIRE((back != entt::null));
	CHECK(game.world.registry().all_of<C>(back));
	CHECK(recordOf(sceneJson(game.world), "Thing") == before);
}
} // namespace

TEST_CASE("The decorative components that may stream come back from a cell as they went in")
{
	ParticleSystemComponent particles;
	particles.particleAssetId = HE::UUID::generate();
	comesBackFromACell("particlesystem", particles);

	SkeletalMeshComponent skeleton;
	skeleton.meshAssetId = HE::UUID::generate();
	comesBackFromACell("skeletalmesh", skeleton);

	AnimatorComponent animator;
	animator.clipAssetId   = HE::UUID::generate();
	animator.playbackSpeed = 1.5f;
	animator.looping       = false;
	comesBackFromACell("animator", animator);

	AnimatorBlendComponent blend;
	blend.clipAId = HE::UUID::generate();
	blend.clipBId = HE::UUID::generate();
	comesBackFromACell("animatorblend", blend);

	PropertyAnimatorComponent props;
	props.clipId        = HE::UUID::generate();
	props.playbackSpeed = 0.5f;
	comesBackFromACell("propertyanimator", props);
}
