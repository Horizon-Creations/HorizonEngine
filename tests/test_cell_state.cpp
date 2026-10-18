// HE::CellState through HE::CellRuntime (Thema 164 step 3b, Thema 183 step 2): what
// changed in a cell while it was there is still so after it was unloaded and loaded
// again — a destroyed box stays destroyed (tombstone), a moved or hidden box with a
// SaveStateComponent comes back where and how it was left, and its body with it.
// Only for a cell with stable ids (version 2); within one session.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EntityVisibility.h>
#include <HorizonScene/FloatingOrigin.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/EntityIdComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/SaveStateComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Types/TypeRegistry.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
const Entity kNone = entt::null;   // a handle doctest can compare with

TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

// One 100 m cell at 0,0 with eight static boxes in a row along x (box i at x = 40 + 2i,
// z = 50), each with a cube mesh and a static body. Boxes 0, 1 and 5 carry a
// SaveStateComponent. `head`: the version-2 head that makes the ids stable.
constexpr int kBoxes = 8;
bool stateful(int i) { return i == 0 || i == 1 || i == 5; }

void writeCell(const std::filesystem::path& file, bool head)
{
	HorizonWorld cell;
	for (int i = 0; i < kBoxes; ++i)
	{
		const Entity b = cell.createEntity("Box " + std::to_string(i));
		tf(cell, b).position = { 40.0f + static_cast<float>(i) * 2.0f, 0.5f, 50.0f };
		MeshComponent mc;
		mc.meshAssetId = HE::kDefaultCubeMeshId;
		cell.addComponent(b, mc);
		RigidBodyComponent rb;
		rb.type = RigidBodyType::Static;
		cell.addComponent(b, rb);
		if (stateful(i)) cell.addComponent(b, SaveStateComponent{});
	}
	std::filesystem::create_directories(file.parent_path());
	SceneSerializer ser;
	REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
	if (!head) return;
	nlohmann::json j;
	{
		std::ifstream in(file);
		j = nlohmann::json::parse(in);
	}
	j["streaming"] = { { "version", 2 } };
	std::ofstream out(file, std::ios::trunc);
	out << j.dump();
}

constexpr const char* kManifest =
	R"({"version": 2, "cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 2,
	    "dir": "W.cells", "list": [[0,0,8,8]]})";

std::filesystem::path writeWorld(const char* name, bool head = true)
{
	const auto root = std::filesystem::temp_directory_path() / name;
	he_test::removeAllQuiet(root);
	writeCell(root / "W.cells" / "cell_0_0.hescene", head);
	return root;
}

struct Host final : HE::ICellHost
{
	std::filesystem::path   root;
	PhysicsWorld*           physics = nullptr;
	std::vector<std::string> loadedNames;   // the boxes the last cellLoaded was handed, by name
	int                      loadedCalls = 0;

	std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) override
	{
		const auto file = root / path;
		if (!std::filesystem::exists(file)) return {};
		return [file](std::vector<uint8_t>& out)
		{
			std::ifstream in(file, std::ios::binary);
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return !out.empty();
		};
	}
	PhysicsWorld* cellPhysics() override { return physics; }
	void streamCellAssets(HorizonWorld&, const HE::CancelToken&, const std::vector<uint32_t>&) override {}
	void cellLoaded(HorizonWorld& world, entt::entity, const std::vector<entt::entity>& created) override
	{
		++loadedCalls;
		loadedNames.clear();
		for (entt::entity e : created)
		{
			REQUIRE(world.registry().valid(e));
			if (const auto* n = world.registry().try_get<NameComponent>(e); n && n->name.rfind("Box ", 0) == 0)
				loadedNames.push_back(n->name);
		}
	}
};

Entity makeCamera(HorizonWorld& world, float x)
{
	const Entity cam = world.createEntity("Camera");
	tf(world, cam).position = { x, 1.7f, 50.0f };
	world.registry().emplace<CameraComponent>(cam);
	return cam;
}

// Every entity of that name (a box built twice shows up twice).
std::vector<Entity> named(HorizonWorld& world, const std::string& name)
{
	std::vector<Entity> out;
	for (auto [e, n] : world.registry().view<NameComponent>().each())
		if (n.name == name) out.push_back(e);
	return out;
}
Entity box(HorizonWorld& world, int i)
{
	const std::vector<Entity> all = named(world, "Box " + std::to_string(i));
	return all.size() == 1 ? all.front() : entt::null;
}

template <class Done>
void pump(HE::CellRuntime& rt, HorizonWorld& world, Done done, double budgetMs = HE::CellRuntime::kDefaultBudgetMs)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		rt.update(world, 0.0f, budgetMs);
		const auto& st = rt.streamer().stats();
		if (done() && st.inFlight == 0 && st.ready == 0 && st.building == 0) return;
		std::this_thread::yield();
	}
}
void loadCell(HE::CellRuntime& rt, HorizonWorld& world, Entity cam)
{
	tf(world, cam).position.x = 50.0f;
	pump(rt, world, [&] { return rt.streamer().isLoaded(0, 0); });
	REQUIRE(rt.streamer().isLoaded(0, 0));
}
void unloadCell(HE::CellRuntime& rt, HorizonWorld& world, Entity cam)
{
	tf(world, cam).position.x = 1000.0f;
	pump(rt, world, [&] { return !rt.streamer().isLoaded(0, 0); });
	REQUIRE_FALSE(rt.streamer().isLoaded(0, 0));
}

// The entity a ray straight down at (x, z) meets first, or entt::null.
Entity hitAt(PhysicsWorld& phys, float x, float z)
{
	const PhysicsWorld::RaycastHit h = phys.raycast({ x, 10.0f, z }, { 0.0f, -1.0f, 0.0f }, 20.0f);
	return h.hit ? static_cast<Entity>(h.entityId) : entt::null;
}
} // namespace

TEST_CASE("CellState: a destroyed box stays destroyed, a moved and hidden one comes back as it was left")
{
	const auto root = writeWorld("he_cell_state_basic");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	PhysicsWorld phys;
	phys.initialize(world);
	Host host;
	host.root    = root;
	host.physics = &phys;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	rt.streamer().setSliceEntities(3);   // several slices: the tombstones are left out of each
	loadCell(rt, world, cam);
	REQUIRE(host.loadedNames.size() == kBoxes);
	CHECK(phys.bodyCount() == static_cast<uint32_t>(kBoxes));

	// While the cell is there: box 1 (stateful) and box 3 (not) are destroyed, box 0
	// (stateful) moved 20 m along z and hidden, box 2 (not stateful) moved the same.
	world.destroyEntity(box(world, 1));
	world.destroyEntity(box(world, 3));
	tf(world, box(world, 0)).position.z = 70.0f;
	HE::setEntityVisible(world.registry(), box(world, 0), false);
	tf(world, box(world, 2)).position.z = 70.0f;
	phys.step(world, 1.0f / 60.0f);   // reaps the destroyed boxes' bodies, as a frame does
	REQUIRE(phys.bodyCount() == static_cast<uint32_t>(kBoxes - 2));

	unloadCell(rt, world, cam);
	CHECK(rt.state().tombstones() == 2);
	CHECK(rt.state().states() == 2);   // boxes 0 and 5; box 1 is a tombstone now
	CHECK(phys.bodyCount() == 0u);

	loadCell(rt, world, cam);
	// The two destroyed boxes are not built again, whichever slice they were in, and the
	// host is not handed them.
	CHECK(named(world, "Box 1").empty());
	CHECK(named(world, "Box 3").empty());
	CHECK(host.loadedNames.size() == kBoxes - 2);
	CHECK(std::find(host.loadedNames.begin(), host.loadedNames.end(), "Box 3") == host.loadedNames.end());
	CHECK(phys.bodyCount() == static_cast<uint32_t>(kBoxes - 2));
	CHECK(rt.streamer().stats().skipped == 2);   // left out of the slices, not built and destroyed
	CHECK(rt.streamer().stats().idCollisions == 0);

	// Box 0 stands where it was left, hidden, and so does its body: the ray at the new
	// place meets it, the one at the authored place meets nothing.
	const Entity b0 = box(world, 0);
	REQUIRE(b0 != kNone);
	CHECK(HE::worldPositionOf(world, b0).z == doctest::Approx(70.0f));
	CHECK(HE::entityVisibility(world.registry(), b0) == HE::Visibility::Hidden);
	CHECK(hitAt(phys, 40.0f, 70.0f) == b0);
	CHECK(hitAt(phys, 40.0f, 50.0f) == kNone);
	// Box 2 has no SaveStateComponent: built from the file, where it was authored (the
	// control for the box above).
	const Entity b2 = box(world, 2);
	REQUIRE(b2 != kNone);
	CHECK(HE::worldPositionOf(world, b2).z == doctest::Approx(50.0f));
	CHECK(hitAt(phys, 44.0f, 50.0f) == b2);

	// And again: what is kept is kept over more than one round.
	unloadCell(rt, world, cam);
	loadCell(rt, world, cam);
	CHECK(named(world, "Box 1").empty());
	REQUIRE(box(world, 0) != kNone);
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(70.0f));

	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellState: a box hung out of the cell is not built a second time")
{
	const auto root = writeWorld("he_cell_state_reparent");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	loadCell(rt, world, cam);

	// A script takes box 4 out of the cell, under the world root: it survives the
	// unload, and it is not built again with the cell — there is one box 4, not two.
	REQUIRE(world.reparentEntity(box(world, 4), world.rootEntity()));
	unloadCell(rt, world, cam);
	CHECK(named(world, "Box 4").size() == 1);
	loadCell(rt, world, cam);
	CHECK(named(world, "Box 4").size() == 1);
	CHECK(rt.streamer().stats().idCollisions == 0);
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellState: a cell without stable ids keeps nothing")
{
	const auto root = writeWorld("he_cell_state_v1", /*head=*/false);
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	loadCell(rt, world, cam);
	const HE::UUID first = world.registry().get<EntityIdComponent>(box(world, 0)).id;
	world.destroyEntity(box(world, 1));
	tf(world, box(world, 0)).position.z = 70.0f;
	unloadCell(rt, world, cam);
	CHECK(rt.state().tombstones() == 0);
	CHECK(rt.state().states() == 0);
	loadCell(rt, world, cam);
	CHECK(named(world, "Box 1").size() == 1);   // back, as before CellState
	REQUIRE(box(world, 0) != kNone);
	CHECK(world.registry().get<EntityIdComponent>(box(world, 0)).id != first);   // fresh ids
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(50.0f));
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellState: a cell abandoned half built keeps what an earlier unload left for the rest")
{
	const auto root = writeWorld("he_cell_state_partial");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	// Slices of two: the root alone, then boxes 0-1, 2-3, 4-5, 6-7.
	rt.streamer().setSliceEntities(2);
	loadCell(rt, world, cam);
	tf(world, box(world, 5)).position.z = 70.0f;   // stateful, in the fourth slice
	world.destroyEntity(box(world, 6));            // in the last slice
	unloadCell(rt, world, cam);
	REQUIRE(rt.state().tombstones() == 1);

	// Back, but with a budget of nothing each update builds one slice: two of them (the
	// root and boxes 0-1), then the camera leaves and the cell is abandoned.
	tf(world, cam).position.x = 50.0f;
	const size_t slicesBefore = rt.streamer().stats().slicesDone;
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (rt.streamer().stats().slicesDone < slicesBefore + 2 && std::chrono::steady_clock::now() < until)
	{
		rt.update(world, 0.0f, 0.0);
		std::this_thread::yield();
	}
	REQUIRE(rt.streamer().stats().slicesDone == slicesBefore + 2);
	REQUIRE(rt.streamer().isBuilding(0, 0));
	CHECK(named(world, "Box 5").empty());          // not built yet
	tf(world, cam).position.x = 1000.0f;
	rt.update(world, 0.0f);
	REQUIRE(rt.streamer().stats().abandoned == 1);
	// What the half cell did not build was not "gone": no new tombstone, box 5's state kept.
	CHECK(rt.state().tombstones() == 1);

	loadCell(rt, world, cam);
	REQUIRE(box(world, 5) != kNone);
	CHECK(HE::worldPositionOf(world, box(world, 5)).z == doctest::Approx(70.0f));
	CHECK(named(world, "Box 6").empty());
	CHECK(named(world, "Box 0").size() == 1);
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellState: a moved box comes back to the same absolute place after the origin moved")
{
	const auto root = writeWorld("he_cell_state_origin");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	loadCell(rt, world, cam);
	tf(world, box(world, 0)).position.z = 70.0f;
	unloadCell(rt, world, cam);

	// The world moves back by 1 km in x while the cell is out (the camera along with
	// it): the cell's root lands at -origin, and the box under it where it was left.
	HE::shiftWorldOrigin(world, nullptr, { 1000.0f, 0.0f, 0.0f });
	REQUIRE(world.origin().x == doctest::Approx(1000.0));
	tf(world, cam).position.x = 50.0f - 1000.0f;
	pump(rt, world, [&] { return rt.streamer().isLoaded(0, 0); });
	REQUIRE(rt.streamer().isLoaded(0, 0));
	const Entity b0 = box(world, 0);
	REQUIRE(b0 != kNone);
	const glm::dvec3 absolute = glm::dvec3(HE::worldPositionOf(world, b0)) + world.origin();
	CHECK(absolute.x == doctest::Approx(40.0));
	CHECK(absolute.z == doctest::Approx(70.0));
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("CellState: clear() and a new session forget; the state is not written out on clear")
{
	const auto root = writeWorld("he_cell_state_clear");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);
	loadCell(rt, world, cam);
	tf(world, box(world, 0)).position.z = 70.0f;
	world.destroyEntity(box(world, 1));
	unloadCell(rt, world, cam);
	REQUIRE(rt.state().tombstones() == 1);
	REQUIRE(rt.state().states() > 0);

	// The editor's Stop: the cells go with the session, and so does what was kept.
	loadCell(rt, world, cam);
	tf(world, box(world, 5)).position.z = 70.0f;
	rt.clear(world);
	CHECK(rt.state().tombstones() == 0);
	CHECK(rt.state().states() == 0);

	// The next session starts from the files.
	loadCell(rt, world, cam);
	CHECK(named(world, "Box 1").size() == 1);
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(50.0f));
	CHECK(HE::worldPositionOf(world, box(world, 5)).z == doctest::Approx(50.0f));
	rt.clear(world);
	he_test::removeAllQuiet(root);
}

TEST_CASE("entity::stateOf / applyState: the save's state without the save")
{
	// No play mode, no active save: the form entity.saveState writes, for an engine
	// store. Quiet for an entity without the component, as a cell is full of them.
	HorizonWorld world;
	const Entity e = world.createEntity("Crate");
	tf(world, e).position = { 1.0f, 2.0f, 3.0f };
	world.registry().emplace<MeshComponent>(e);
	HE::api::Ctx c;
	c.world = &world;
	CHECK(HE::api::entity::stateOf(c, static_cast<uint32_t>(e)).empty());
	CHECK_FALSE(HE::api::entity::applyState(c, static_cast<uint32_t>(e), "{}"));

	world.registry().emplace<SaveStateComponent>(e);
	const std::string text = HE::api::entity::stateOf(c, static_cast<uint32_t>(e));
	REQUIRE_FALSE(text.empty());
	tf(world, e).position = { 9.0f, 9.0f, 9.0f };
	HE::setEntityVisible(world.registry(), e, false);
	// The script half alone leaves the world half as it is...
	CHECK(HE::api::entity::applyState(c, static_cast<uint32_t>(e), text, HE::api::entity::kStateScript));
	CHECK(tf(world, e).position.x == doctest::Approx(9.0f));
	// ...the world half puts transform and visibility back.
	CHECK(HE::api::entity::applyState(c, static_cast<uint32_t>(e), text, HE::api::entity::kStateWorld));
	CHECK(tf(world, e).position.x == doctest::Approx(1.0f));
	CHECK(tf(world, e).position.z == doctest::Approx(3.0f));
	CHECK(HE::entityVisibility(world.registry(), e) != HE::Visibility::Hidden);
	// A component turned off is the same as none.
	world.registry().get<SaveStateComponent>(e).enabled = false;
	CHECK(HE::api::entity::stateOf(c, static_cast<uint32_t>(e)).empty());
}

TEST_CASE("CellState: an entity the session has not seen go takes its state from the active save")
{
	// A door saved by entity.saveState in an earlier session (or before the scene was
	// split) is in a cell now, not loaded when the game loads its save — no script
	// can reach it to apply the state. The cell applies it when it arrives; once the
	// session kept a state of its own, that one wins.
	namespace save = HE::api::save;
	const auto sandbox = std::filesystem::temp_directory_path() / "he_cell_state_save_sandbox";
	he_test::removeAllQuiet(sandbox);
	HE::api::fs::setSandboxRoot(sandbox.string());
	save::close();
	ContentManager cm;
	{
		SaveGameTemplateAsset a;
		a.name = "CellStateTemplate";
		a.path = "mem://cell_state_template";
		HE::StructDef tpl;
		tpl.name = "CellStateSave";
		a.json = HE::TypeRegistry::structToJson(tpl);
		cm.registerSaveGameTemplate(std::move(a));
	}
	save::setPlayMode(true);
	REQUIRE(save::create("cells", &cm, "mem://cell_state_template"));

	const auto root = writeWorld("he_cell_state_save");
	HorizonWorld world;
	const Entity cam = makeCamera(world, 50.0f);
	Host host;
	host.root = root;
	world.setCellManifestJson(kManifest);
	HE::CellRuntime rt(host);

	// The save's entry for box 0 (stateful) and box 2 (not): written as saveState
	// writes it, by the box's id from the file.
	loadCell(rt, world, cam);
	const HE::UUID id0 = world.registry().get<EntityIdComponent>(box(world, 0)).id;
	const HE::UUID id2 = world.registry().get<EntityIdComponent>(box(world, 2)).id;
	rt.clear(world);   // a new session: nothing kept
	const auto key = [](const HE::UUID& id) { return std::to_string(id.hi) + ":" + std::to_string(id.lo); };
	const std::string moved = R"({"transform":{"pos":[40.0,0.5,80.0],"rot":[0.0,0.0,0.0],"scl":[1.0,1.0,1.0]}})";
	REQUIRE(save::setEntityState(key(id0), moved));
	REQUIRE(save::setEntityState(key(id2), moved));

	loadCell(rt, world, cam);
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(80.0f));
	CHECK(HE::worldPositionOf(world, box(world, 2)).z == doctest::Approx(50.0f));   // no SaveStateComponent

	// Moved again in this session, unloaded, loaded: the session's state wins.
	tf(world, box(world, 0)).position.z = 60.0f;
	unloadCell(rt, world, cam);
	loadCell(rt, world, cam);
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(60.0f));

	// Out of play mode the save is not asked.
	rt.clear(world);
	save::setPlayMode(false);
	loadCell(rt, world, cam);
	CHECK(HE::worldPositionOf(world, box(world, 0)).z == doctest::Approx(50.0f));

	rt.clear(world);
	save::close();
	he_test::removeAllQuiet(root);
	he_test::removeAllQuiet(sandbox);
}
