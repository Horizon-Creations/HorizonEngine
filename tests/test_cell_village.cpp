// The village of the plan (docs/entity-cell-streaming-plan-2026-10-10.md, section 8,
// acceptance of step 3; Thema 183 step 5): prefab houses, 50 NPCs with a script and a
// SaveStateComponent, waypoints and a manager the author marked Resident, crates, and a
// gate hinged to a post, put together in an editor world, split by the editor's own
// splitter (splitWorldIntoCells), and then streamed by a game-like host through
// HE::CellRuntime: physics, the class runtime and its EntityHost, Lua. The cells are
// loaded and unloaded many times, all at once and on walks where some stay and others
// go, and after every move nothing may be lost or doubled: no NPC its position or its
// Save Game variable, no crate that was destroyed comes back, no prefab placement its
// bindings, the gate keeps its joint (it waits while the gate's own square is away,
// see checkVillage), and no script, body or entity is left behind.
//
// The controls sit in the same village, so they cost no rebuild: an NPC without a
// SaveStateComponent comes back where the file put it, untouched crates come back,
// an NPC a script hung out of its cell is there once, and three guards with a
// character controller stay in the base (the class table keeps them there).
//
// The bench at the end is the measuring ladder of this step: NPC density per cell.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/CellPhysics.h>
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/CellScripts.h>
#include <HorizonScene/CellSplit.h>
#include <HorizonScene/CellState.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneJsonParse.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/JointComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/PrefabInstanceComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/SaveStateComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/StreamingComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace HorizonCode;
using json = nlohmann::json;

namespace
{
constexpr int   kCells    = 4;        // the village is 4 × 4 cells
constexpr float kCellSize = 100.0f;   // m

TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

Entity spawn(HorizonWorld& w, const std::string& name, glm::vec3 pos, Entity parent = entt::null)
{
	const Entity e = w.createEntity(name);
	tf(w, e).position = pos;
	if (parent != entt::null) w.reparentEntity(e, parent);
	return e;
}

Entity staticBody(HorizonWorld& w, Entity e)
{
	RigidBodyComponent rb;
	rb.type = RigidBodyType::Static;
	w.addComponent(e, rb);
	return e;
}

// The villagers' script. Each start counts, and looks the well up BY NAME: an NPC
// that named its waypoint in the file (an entity variable) would be held in the base
// by the splitter's ref hull, so a village's NPCs find their resident landmarks at
// run time. The walk is relative to where the NPC stands, so it goes on from wherever
// the cell's state put it back; `self.t` is not saved and starts over.
constexpr const char* kVillagerLua = R"lua(
local M = {}
function M.onStart(self)
    _starts = (_starts or 0) + 1
    local well = horizon.entity.findByName("Waypoint 0")
    if well ~= nil and well ~= 0 then _foundWell = (_foundWell or 0) + 1 end
    self.t = 0
end
function M.onUpdate(self, dt)
    self.t = self.t + dt
    local dir = 1
    if math.floor(self.t / 4) % 2 == 1 then dir = -1 end
    local x, y, z = horizon.getPosition(self.entityId)
    horizon.setPosition(self.entityId, x + 0.5 * dt * dir, y, z)
end
return M
)lua";

// The mayor's: resident, counts its frames, so a cell going cannot have stopped it.
constexpr const char* kMayorLua = R"lua(
local M = {}
function M.onUpdate(self, dt)
    _mayorTicks = (_mayorTicks or 0) + 1
end
return M
)lua";

// A trader's class: a Save Game variable `trips`, one more on every Tick.
Graph traderGraph()
{
	Graph g;
	{ Variable v; v.name = "trips"; v.type = PinType::Int; v.saveGame = true; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Tick";
	const int e = g.addNode(std::move(ev));
	Node get; get.type = NodeType::GetVariable; get.s = "trips"; get.propType = PinType::Int;
	const int gv = g.addNode(std::move(get));
	Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
	const int k = g.addNode(std::move(one));
	Node add; add.type = NodeType::Add;
	const int a = g.addNode(std::move(add));
	Node set; set.type = NodeType::SetVariable; set.s = "trips"; set.propType = PinType::Int;
	const int s = g.addNode(std::move(set));
	REQUIRE(g.connect(gv, 0, a, 0));
	REQUIRE(g.connect(k,  0, a, 1));
	REQUIRE(g.connect(e,  0, s, 0));
	REQUIRE(g.connect(a,  2, s, 2));
	return g;
}

HE::UUID writeLua(ContentManager& cm, const char* name, const char* source)
{
	ScriptAsset a;
	a.type       = HE::AssetType::Script;
	a.name       = name;
	a.path       = std::string(name) + ".hasset";
	a.sourceCode = source;
	a.language   = HE::ScriptLanguage::Lua;
	REQUIRE(cm.saveAsset(a));
	return cm.loadAsset(a.path);
}

HE::UUID writeClass(ContentManager& cm, const char* name, const Graph& g)
{
	HorizonCodeClassAsset a;
	a.type      = HE::AssetType::HorizonCodeClass;
	a.name      = name;
	a.path      = std::string(name) + ".hasset";
	a.baseClass = "Entity";
	a.graphJson = toJson(g);
	REQUIRE(cm.saveAsset(a));
	return cm.loadAsset(a.path);
}

void addScript(HorizonWorld& w, Entity e, const HE::UUID& asset, const char* module)
{
	ScriptComponent sc;
	sc.scriptAssetId = asset;
	sc.moduleName    = module;
	w.addComponent(e, sc);
}

json sceneJson(const HorizonWorld& w)
{
	SceneSerializer ser;
	std::vector<uint8_t> bytes;
	REQUIRE(ser.saveToMemory(w, bytes));
	return HE::parseSceneCbor(bytes);
}

struct Cell
{
	int x = 0, z = 0;
	bool operator<(const Cell& o) const { return x != o.x ? x < o.x : z < o.z; }
};

Cell cellOf(glm::vec3 p)
{
	return { static_cast<int>(std::floor(p.x / kCellSize)), static_cast<int>(std::floor(p.z / kCellSize)) };
}

// Where the k-th thing of a kind stands in cell (cx, cz): a grid of 6.5 m inside the
// cell, kept 10 m off its edges, one row per kind (`row0`) so kinds do not overlap
// at low densities.
glm::vec3 slot(int cx, int cz, int k, int row0)
{
	const int col = k % 12, row = row0 + k / 12;
	return { cx * kCellSize + 10.0f + col * 6.5f, 0.0f, cz * kCellSize + 10.0f + (row % 12) * 6.5f };
}

// The village in the editor, then split. Names are unique; what the test knows of
// each moving thing (id, cell, where the file put it) is taken before the split.
struct Village
{
	struct Thing
	{
		std::string name;
		HE::UUID    id;
		Cell        cell;
		glm::vec3   at{ 0.0f };
	};
	std::filesystem::path root;
	ContentManager        cm;
	HorizonWorld          editor;
	std::map<std::string, std::string> files;   // project-relative path → cell file
	HE::CellSplitResult   split;
	std::vector<Thing>    villagers, traders, houses, crates;
	Thing                 drifter;
	HE::UUID              gate, post;

	Village(const char* name, int villagerCount, int traderCount, int houseCount, int crateCount)
		: root(std::filesystem::temp_directory_path() / name)
		, cm((prepare(root) / "Content").string())
	{
		const HE::UUID villagerLua = writeLua(cm, "Villager", kVillagerLua);
		const HE::UUID mayorLua    = writeLua(cm, "Mayor", kMayorLua);
		const HE::UUID traderClass = writeClass(cm, "Trader", traderGraph());
		const int nCells = kCells * kCells;
		auto cellXZ = [](int i) { return Cell{ i % kCells, (i / kCells) % kCells }; };
		auto thing = [&](Entity e, Cell c)
		{
			return Thing{ editor.registry().get<NameComponent>(e).name, editor.entityId(e), c, tf(editor, e).position };
		};

		// Houses: a prefab placement each, root, a wall with a static body, a door that
		// was nudged by hand (an override).
		for (int i = 0; i < houseCount; ++i)
		{
			const Cell c = cellXZ(i);
			const std::string n = "House " + std::to_string(i);
			const Entity h = spawn(editor, n, slot(c.x, c.z, i / nCells, 9));
			editor.registry().emplace<MeshComponent>(h);
			const Entity wall = staticBody(editor, spawn(editor, n + " Wall", { 2.0f, 0.0f, 0.0f }, h));
			editor.registry().emplace<MeshComponent>(wall);
			const Entity door = spawn(editor, n + " Door", { 0.0f, 0.0f, 3.0f }, h);
			editor.registry().emplace<MeshComponent>(door);
			PrefabInstanceComponent inst;
			inst.asset = HE::UUID::generate();
			for (Entity e : { h, wall, door })
				inst.bindings.push_back({ HE::UUID::generate(), editor.entityId(e) });
			inst.setOverride(inst.bindings[2].templateEntity, "transform", "position");
			editor.registry().emplace<PrefabInstanceComponent>(h, inst);
			houses.push_back(thing(h, c));
		}
		// The 50 NPCs (in the test): Lua villagers and class traders, each with a
		// SaveStateComponent, spread round-robin over the cells.
		for (int i = 0; i < villagerCount; ++i)
		{
			const Cell c = cellXZ(i);
			const Entity v = spawn(editor, "Villager " + std::to_string(i), slot(c.x, c.z, i / nCells, 0));
			addScript(editor, v, villagerLua, "villager");
			editor.addComponent(v, SaveStateComponent{});
			villagers.push_back(thing(v, c));
		}
		const int villagerRows = (villagerCount / nCells + 11) / 12 + 1;
		for (int i = 0; i < traderCount; ++i)
		{
			const Cell c = cellXZ(i);
			const Entity t = spawn(editor, "Trader " + std::to_string(i), slot(c.x, c.z, i / nCells, villagerRows));
			addScript(editor, t, traderClass, "Trader");
			editor.addComponent(t, SaveStateComponent{});
			traders.push_back(thing(t, c));
		}
		// Crates: a static body each, nothing to run.
		for (int i = 0; i < crateCount; ++i)
		{
			const Cell c = cellXZ(i);
			const Entity k = staticBody(editor, spawn(editor, "Crate " + std::to_string(i), slot(c.x, c.z, i / nCells, 10)));
			crates.push_back(thing(k, c));
		}
		// The control without a SaveStateComponent: a villager's script, nothing kept.
		{
			const Entity d = spawn(editor, "Drifter", slot(1, 1, 0, 11));
			addScript(editor, d, villagerLua, "villager");
			drifter = thing(d, Cell{ 1, 1 });
		}
		// Resident by the author's switch: the waypoints and the mayor.
		for (int k = 0; k < 8; ++k)
		{
			const Entity w = spawn(editor, "Waypoint " + std::to_string(k), { 50.0f + 40.0f * k, 0.0f, 200.0f });
			editor.registry().emplace<StreamingComponent>(w);   // Resident is the default
		}
		{
			const Entity m = spawn(editor, "Mayor", { 200.0f, 0.0f, 205.0f });
			addScript(editor, m, mayorLua, "mayor");
			editor.registry().emplace<StreamingComponent>(m);
		}
		// Resident by the class table: guards with a character controller and a script.
		for (int k = 0; k < 3; ++k)
		{
			const Entity g = spawn(editor, "Guard " + std::to_string(k), { 110.0f + 10.0f * k, 1.0f, 310.0f });
			editor.registry().emplace<CharacterControllerComponent>(g);
			addScript(editor, g, villagerLua, "villager");
		}
		// The gate: a dynamic body (Resident) on a fixed joint to a static post, which
		// alone would go into cell 2,0; the joint's reference keeps it in the base.
		{
			const Entity p = staticBody(editor, spawn(editor, "Gate Post", { 205.0f, 10.0f, 5.0f }));
			const Entity g = spawn(editor, "Gate", { 207.0f, 10.0f, 5.0f });
			RigidBodyComponent rb;
			rb.type = RigidBodyType::Dynamic;
			rb.mass = 5.0f;
			editor.addComponent(g, rb);
			JointComponent j;
			j.type   = JointType::Fixed;
			j.target = editor.entityId(p);
			editor.registry().emplace_or_replace<JointComponent>(g, j);
			gate = editor.entityId(g);
			post = editor.entityId(p);
		}
		{
			const Entity cam = spawn(editor, "Camera", { 5000.0f, 1.7f, 5000.0f });
			editor.registry().emplace<CameraComponent>(cam);
		}

		HE::CellSplitOptions o;
		o.cellSize     = kCellSize;
		o.loadRadius   = 80.0f;
		o.unloadRadius = 110.0f;
		o.dir          = "Content/Village.cells";
		split = HE::splitWorldIntoCells(editor, o,
			[this](const std::string& path, const std::string& text) { files[path] = text; return true; });
		REQUIRE(split.error.empty());
	}
	static std::filesystem::path prepare(const std::filesystem::path& root)
	{
		he_test::removeAllQuiet(root);
		std::filesystem::create_directories(root / "Content");
		return root;
	}
	~Village() { he_test::removeAllQuiet(root); }
};

// A host like the game's (GameApplication's ICellHost): cells from the split's files,
// the bodies into a physics world, the scripts through the shared helper, and the
// state's Ctx with the class runtime. It times its two hooks for the bench.
struct Host final : HE::ICellHost
{
	const std::map<std::string, std::string>* files = nullptr;
	PhysicsWorld*       physics = nullptr;
	ContentManager*     cm = nullptr;
	HE::CellScriptHosts scripts;
	double loadedMsMax = 0.0, unloadingMsMax = 0.0, loadedMsSum = 0.0, unloadingMsSum = 0.0;

	std::function<bool(std::vector<uint8_t>&)> openCellFile(const std::string& path) override
	{
		const auto it = files->find(path);
		if (it == files->end()) return {};
		const std::string bytes = it->second;
		return [bytes](std::vector<uint8_t>& out) { out.assign(bytes.begin(), bytes.end()); return true; };
	}
	PhysicsWorld* cellPhysics() override { return physics; }
	void streamCellAssets(HorizonWorld&, const HE::CancelToken&, const std::vector<uint32_t>&) override {}
	void cellLoaded(HorizonWorld& world, entt::entity, const std::vector<entt::entity>& created) override
	{
		const auto t0 = std::chrono::steady_clock::now();
		HE::startCellScripts(scripts, world, created);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		loadedMsMax = std::max(loadedMsMax, ms);
		loadedMsSum += ms;
	}
	void cellUnloading(HorizonWorld& world, entt::entity root) override
	{
		const auto t0 = std::chrono::steady_clock::now();
		HE::stopCellScripts(scripts, world, root);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		unloadingMsMax = std::max(unloadingMsMax, ms);
		unloadingMsSum += ms;
	}
	void cellApiCtx(HE::api::Ctx& ctx) override
	{
		ctx.content  = cm;
		ctx.runtime  = scripts.runtime;
		ctx.entities = scripts.entities;
	}
};

// The game: the base as the game loads it (through memory, as from a pak), physics
// over it with the cells' hold, the class runtime, Lua, and the cell runtime. The base
// scripts start the way the game starts them, before any cell is there.
struct Game
{
	Village&                   village;
	HorizonWorld               world;
	PhysicsWorld               phys;
	Runtime                    rt;
	EntityHost                 classes;
	ScriptContext              text{ world };
	ScriptContext::InstanceMap textInstances;
	Host                       host;
	HE::CellRuntime            cells{ host };
	Entity                     cam = entt::null;
	int                        pinHandle = 0;
	std::vector<double>        updateMs;   // every update() of pump() and frame()

	explicit Game(Village& v) : village(v)
	{
		REQUIRE(SceneSerializer().loadFromMemory(world, json::to_cbor(sceneJson(v.editor))));
		REQUIRE_FALSE(world.cellManifestJson().empty());
		for (auto [e, n] : world.registry().view<NameComponent>().each())
			if (n.name == "Camera") cam = e;
		REQUIRE((cam != entt::null));
		phys.initialize(world);
		phys.setRegionHold([this](const glm::vec3& p) { return HE::cellHolds(cells.streamer(), world, p); });
		classes.setPhysicsWorld(&phys);
		classes.begin(rt, world, v.cm);
		text.startWorldScripts(v.cm, textInstances);
		host.files                 = &v.files;
		host.physics               = &phys;
		host.cm                    = &v.cm;
		host.scripts.entities      = &classes;
		host.scripts.text          = &text;
		host.scripts.textInstances = &textInstances;
		host.scripts.runtime       = &rt;
		host.scripts.content       = &v.cm;
	}
	~Game()
	{
		cells.clear(world);
		classes.end();
	}

	void timedUpdate(float dt)
	{
		const auto t0 = std::chrono::steady_clock::now();
		cells.update(world, dt);
		updateMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
	// Streams from where the camera and pins are until nothing is in flight, then one
	// physics step (it reaps the bodies of what went). No script runs in here.
	void pump()
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		int quiet = 0;
		while (std::chrono::steady_clock::now() < until && quiet < 3)
		{
			timedUpdate(0.0f);
			const auto& st = cells.streamer().stats();
			quiet = (st.inFlight == 0 && st.ready == 0 && st.building == 0) ? quiet + 1 : 0;
			if (quiet == 0) std::this_thread::yield();
		}
		REQUIRE(quiet >= 3);
		phys.step(world, 1.0f / 60.0f);
	}
	void cameraTo(float x, float z)
	{
		tf(world, cam).position = { x, 1.7f, z };
	}
	void allIn()
	{
		cameraTo(200.0f, 200.0f);
		if (pinHandle == 0) pinHandle = cells.pin({ 200.0, 0.0, 200.0 }, 400.0f);
		pump();
	}
	void allOut()
	{
		if (pinHandle != 0) CHECK(cells.unpin(pinHandle));
		pinHandle = 0;
		cameraTo(5000.0f, 5000.0f);
		pump();
	}
	// `n` game frames of 1/60 s: scripts, classes, physics, then the cells (the camera
	// stands, so they do not change).
	void frames(int n)
	{
		const float dt = 1.0f / 60.0f;
		for (int i = 0; i < n; ++i)
		{
			for (const auto& [entity, inst] : textInstances) text.callOnUpdate(inst, dt);
			classes.tick(dt);
			phys.step(world, dt);
			timedUpdate(dt);
		}
	}
	bool loaded(Cell c) const { return cells.streamer().isLoaded(c.x, c.z); }
	Entity find(const HE::UUID& id) const { return world.findByEntityId(id); }
	double luaGlobal(const char* name) { return text.engine().getGlobalNumber(name); }
};

// What the village checks after every move: the record of every NPC as it was last
// seen (no script runs between that and the move), and who was there.
struct Ledger
{
	std::unordered_map<HE::UUID, glm::vec3> pos;
	std::unordered_map<HE::UUID, int>       trips;
	std::unordered_set<HE::UUID>  present;
};

// Everything that must hold after the cells moved: per NPC, here exactly when its
// cell is (a hung-out one always), back where it was last seen (or where the file put
// it, for the control without SaveState and for a first load), its trips intact; every
// name at most once; crates; houses with bindings; instance and body counts.
void checkVillage(Game& g, Ledger& ledger, const std::set<std::string>& destroyed,
                  const HE::UUID& hungOut, size_t baseLua, size_t baseBodies, const char* when)
{
	INFO(std::string(when));   // a const char* would print as a bool
	Village& v = g.village;
	auto& reg = g.world.registry();

	std::map<std::string, int> names;
	for (auto [e, n] : reg.view<NameComponent>().each()) ++names[n.name];
	for (const auto& [n, count] : names)
	{
		CAPTURE(n);
		CHECK(count == 1);
	}

	size_t luaCells = 0;
	std::unordered_set<HE::UUID> nowPresent;
	auto npc = [&](const Village::Thing& t, bool keepsState, bool isTrader)
	{
		CAPTURE(t.name);
		const Entity e = g.find(t.id);
		const bool expected = t.id == hungOut || g.loaded(t.cell);
		REQUIRE((e != entt::null) == expected);
		if (e == entt::null) return;
		nowPresent.insert(t.id);
		if (!isTrader && t.id != hungOut) ++luaCells;
		const bool cameBack = ledger.present.count(t.id) == 0;
		const bool seen     = ledger.pos.count(t.id) != 0;
		const glm::vec3 want = (seen && (keepsState || !cameBack)) ? ledger.pos[t.id] : t.at;
		const glm::vec3 now  = HE::worldPositionOf(g.world, e);
		CHECK(now.x == doctest::Approx(want.x).epsilon(1e-4));
		CHECK(now.y == doctest::Approx(want.y).epsilon(1e-4));
		CHECK(now.z == doctest::Approx(want.z).epsilon(1e-4));
		if (isTrader)
		{
			const InstanceId inst = g.classes.instanceOf(e);
			REQUIRE(inst != 0);
			const int wantTrips = ledger.trips.count(t.id) ? ledger.trips[t.id] : 0;
			CHECK(g.rt.getVariable(inst, "trips").i == wantTrips);
		}
	};
	for (const auto& t : v.villagers) npc(t, true, false);
	for (const auto& t : v.traders)   npc(t, true, true);
	npc(v.drifter, false, false);
	ledger.present = std::move(nowPresent);

	// Crates: a destroyed one never again, every other one with its cell.
	size_t crates = 0;
	for (const auto& t : v.crates)
	{
		CAPTURE(t.name);
		const bool here = g.find(t.id) != entt::null;
		CHECK(here == (destroyed.count(t.name) == 0 && g.loaded(t.cell)));
		crates += here ? 1 : 0;
	}
	// Houses: the same entities, and every binding names an entity under the root.
	size_t houses = 0;
	for (const auto& t : v.houses)
	{
		CAPTURE(t.name);
		const Entity h = g.find(t.id);
		CHECK((h != entt::null) == g.loaded(t.cell));
		if (h == entt::null) continue;
		++houses;
		const auto* inst = reg.try_get<PrefabInstanceComponent>(h);
		REQUIRE(inst != nullptr);
		REQUIRE(inst->bindings.size() == 3);
		for (const auto& b : inst->bindings)
		{
			const Entity e = g.find(b.instanceEntity);
			REQUIRE((e != entt::null));
			CHECK(g.world.isAncestorOf(h, e));
		}
	}

	// Nothing runs that is gone, nothing that is there runs twice: the Lua instances are
	// the base's, the hung-out villager's and the cells' villagers'; the classes are the
	// traders that are there.
	const size_t hung = hungOut == HE::UUID{} ? 0u : 1u;
	CHECK(g.textInstances.size() == baseLua + hung + luaCells);
	size_t traders = 0;
	for (const auto& t : v.traders) traders += g.find(t.id) != entt::null ? 1 : 0;
	CHECK(g.classes.count() == traders);
	// Bodies: the base's, a wall per house that is there, the crates that are there.
	CHECK(g.phys.bodyCount() == baseBodies + houses + crates);
	// The gate: its post never left. The gate stands in cell 2,0, and while that cell
	// is away the region hold has the gate out of the broad phase and its joint waits
	// (PhysicsWorld::applyRegionHold); with the cell back it is built again. Either
	// way the gate hangs where it hung.
	CHECK((g.find(v.post) != entt::null));
	const Entity gate = g.find(v.gate);
	CHECK(g.phys.hasJoint(static_cast<uint32_t>(gate)) == g.loaded(Cell{ 2, 0 }));
	CHECK(HE::worldPositionOf(g.world, gate).y == doctest::Approx(10.0f).epsilon(0.02));
	CHECK(g.cells.streamer().stats().idCollisions == 0);
	CHECK(g.cells.streamer().stats().failed == 0);
}

// After `n` frames: what every NPC that is there looks like now.
void record(Game& g, Ledger& ledger)
{
	for (const auto* list : { &g.village.villagers, &g.village.traders })
		for (const auto& t : *list)
			if (const Entity e = g.find(t.id); e != entt::null)
			{
				ledger.pos[t.id] = HE::worldPositionOf(g.world, e);
				if (const InstanceId inst = g.classes.instanceOf(e))
					ledger.trips[t.id] = g.rt.getVariable(inst, "trips").i;
			}
	if (const Entity d = g.find(g.village.drifter.id); d != entt::null)
		ledger.pos[g.village.drifter.id] = HE::worldPositionOf(g.world, d);
}

size_t loadedCells(Game& g)
{
	size_t n = 0;
	for (int x = 0; x < kCells; ++x)
		for (int z = 0; z < kCells; ++z) n += g.loaded({ x, z }) ? 1 : 0;
	return n;
}
} // namespace

TEST_CASE("Village: prefab houses and 50 scripted NPCs keep everything over many cell unloads and loads")
{
	Village v("he_cell_village", 25, 25, 24, 20);

	// ── The split: what went into cells, what stayed, and why ──
	// Moved: 24 houses × 3, 50 NPCs, the drifter, 20 crates. Kept: the waypoints and the
	// mayor (the author's switch), the guards (character controller), the gate (dynamic
	// body) and its post (the ref hull: the gate's joint names it), the camera.
	CHECK(v.split.moved == 24u * 3u + 50u + 1u + 20u);
	CHECK(v.split.keptForRefs == 1u);
	CHECK(v.split.cells.size() == static_cast<size_t>(kCells * kCells));
	{
		std::set<std::string> base;
		for (auto [e, n] : v.editor.registry().view<NameComponent>().each()) base.insert(n.name);
		for (const char* n : { "Mayor", "Gate", "Gate Post", "Guard 0", "Guard 1", "Guard 2", "Waypoint 0", "Waypoint 7", "Camera" })
		{
			CAPTURE(n);
			CHECK(base.count(n) == 1);
		}
		for (const char* n : { "Villager 0", "Trader 24", "House 0", "Crate 19", "Drifter" })
		{
			CAPTURE(n);
			CHECK(base.count(n) == 0);
		}
		size_t inCells = 0;
		for (const auto& c : v.split.cells)
		{
			CAPTURE(c.x);
			CAPTURE(c.z);
			CHECK(c.scene.contains("streaming"));   // version 2: the ids are kept
			inCells += c.entities;
		}
		CHECK(inCells == v.split.moved);
	}

	Game g(v);
	const size_t baseLua = g.textInstances.size();
	CHECK(baseLua == 4u);   // the mayor and the three guards run from the start
	g.phys.step(g.world, 1.0f / 60.0f);
	const size_t baseBodies = g.phys.bodyCount();
	// Before the runtime's first update nothing streams, so nothing is held yet and the
	// joint stands; the first all-out round below is where it waits.
	CHECK(g.phys.hasJoint(static_cast<uint32_t>(g.find(v.gate))));

	Ledger ledger;
	std::set<std::string> destroyed;
	HE::UUID hungOut;   // none yet
	size_t round = 0;
	auto check = [&](const std::string& when)
	{
		checkVillage(g, ledger, destroyed, hungOut, baseLua, baseBodies, when.c_str());
	};

	// ── Round 0: everything in; the first frames; the player breaks five crates; a
	//    script carries Villager 0 off into the base ──
	g.allIn();
	REQUIRE(loadedCells(g) == static_cast<size_t>(kCells * kCells));
	check("first load");
	// The guards, the villagers and the drifter start once each (the mayor's script has
	// no onStart).
	CHECK(g.luaGlobal("_starts") == doctest::Approx(3 + 25 + 1));
	CHECK(g.luaGlobal("_foundWell") == doctest::Approx(3 + 25 + 1));   // the resident well, by name
	g.frames(30);
	for (int i = 0; i < 5; ++i)
	{
		const Entity k = g.find(v.crates[i].id);
		REQUIRE((k != entt::null));
		g.world.destroyEntity(k);
		destroyed.insert(v.crates[i].name);
	}
	{
		const Entity v0 = g.find(v.villagers[0].id);
		REQUIRE(g.world.reparentEntity(v0, g.world.rootEntity()));
		hungOut = v.villagers[0].id;
	}
	const auto hungInstance = g.textInstances.at(static_cast<uint32_t>(g.find(hungOut)));
	g.phys.step(g.world, 1.0f / 60.0f);
	record(g, ledger);
	const double mayorTicks0 = g.luaGlobal("_mayorTicks");

	// ── Round 1: everything out ──
	g.allOut();
	REQUIRE(loadedCells(g) == 0u);
	check("all out, round 1");
	CHECK(g.cells.state().tombstones() == 5u + 1u);   // the crates, and the villager that left
	CHECK(g.cells.state().states() == 24u + 25u);     // every NPC with SaveState still in its cell
	CHECK(g.classes.count() == 0u);

	// ── Round 2: everything in again ──
	g.allIn();
	check("all in, round 2");
	CHECK(g.textInstances.at(static_cast<uint32_t>(g.find(hungOut))) == hungInstance);
	g.frames(30);
	record(g, ledger);

	// ── Rounds 3–6: walks across the village; cells come and go a few at a time, and
	//    between two steps the scripts run ──
	const auto walk = [&](bool alongX, float fixed, float from, float to)
	{
		const float step = from < to ? 50.0f : -50.0f;
		for (float p = from; from < to ? p <= to : p >= to; p += step)
		{
			if (alongX) g.cameraTo(p, fixed); else g.cameraTo(fixed, p);
			g.pump();
			check("walk " + std::to_string(round) + " at " + std::to_string(static_cast<int>(p)));
			g.frames(10);
			g.phys.step(g.world, 1.0f / 60.0f);
			record(g, ledger);
		}
		++round;
	};
	g.allOut();
	check("all out before the walks");
	round = 3;
	walk(true,  150.0f, -150.0f, 550.0f);   // west to east through the middle rows
	walk(false, 250.0f,  550.0f, -150.0f);  // north to south
	walk(true,   50.0f,  550.0f, -150.0f);  // back along the southern row
	walk(false, 350.0f, -150.0f,  550.0f);

	// ── Rounds 7–9: out, in, out, in ──
	for (int i = 0; i < 2; ++i)
	{
		g.allOut();
		check("all out, round " + std::to_string(7 + 2 * i));
		g.allIn();
		check("all in, round " + std::to_string(8 + 2 * i));
		g.frames(5);
		record(g, ledger);
	}

	// The mayor ran through all of it, the hung-out villager kept its one instance, and
	// the cells were really built and dropped many times.
	CHECK(g.luaGlobal("_mayorTicks") > mayorTicks0);
	CHECK(g.textInstances.at(static_cast<uint32_t>(g.find(hungOut))) == hungInstance);
	const auto& st = g.cells.streamer().stats();
	CHECK(st.unloads >= 4u * 16u);
	CHECK(st.loadsDone >= 5u * 16u);
	CHECK(st.skipped >= 6u);
	MESSAGE("village: " << st.loadsDone << " cell loads, " << st.unloads << " unloads, "
	        << g.updateMs.size() << " updates, worst " << *std::max_element(g.updateMs.begin(), g.updateMs.end()) << " ms");
	// The NPCs did walk: none is where the file put it, except the drifter after a reload.
	size_t moved = 0;
	for (const auto& t : v.villagers)
		if (const Entity e = g.find(t.id); e != entt::null && HE::worldPositionOf(g.world, e).x != t.at.x) ++moved;
	CHECK(moved == v.villagers.size());
}

// The ladder of this step: what a cell's scripts and state cost the frame they go in
// or out in, by NPC density per cell. Script start (cellLoaded) and the state's capture
// (CellState, at the unload) run once per cell, in one update(), outside the 4 ms slice
// budget, so a dense cell shows up as one long frame. Run with
//   he_tests --no-skip --test-case="Village bench*"
TEST_CASE("Village bench: a cell's scripts and state by NPC density" * doctest::skip())
{
	for (const int perCell : { 3, 30, 125 })
	{
		const int npcs = perCell * kCells * kCells;
		Village v("he_cell_village_bench", npcs / 2, npcs - npcs / 2, 24, 20);
		Game g(v);
		auto stats = [](std::vector<double> ms)
		{
			std::sort(ms.begin(), ms.end());
			struct { double p50, p99, max, sum; } s{};
			if (ms.empty()) return s;
			s.p50 = ms[ms.size() / 2];
			s.p99 = ms[std::min(ms.size() - 1, ms.size() * 99 / 100)];
			s.max = ms.back();
			for (double m : ms) s.sum += m;
			return s;
		};
		for (int rep = 0; rep < 3; ++rep)
		{
			g.host.loadedMsMax = g.host.unloadingMsMax = g.host.loadedMsSum = g.host.unloadingMsSum = 0.0;
			g.updateMs.clear();
			g.allIn();
			const auto in = stats(g.updateMs);
			const size_t inUpdates = g.updateMs.size();
			const double inHostMax = g.host.loadedMsMax, inHostSum = g.host.loadedMsSum;
			g.frames(30);
			g.updateMs.clear();
			g.allOut();
			const auto out = stats(g.updateMs);
			std::printf("VILLAGE_BENCH perCell=%d npcs=%d rep=%d | in: updates=%zu max=%.2f sum=%.1f ms, cellLoaded max=%.2f sum=%.1f"
			            " | out: updates=%zu max=%.2f sum=%.1f ms, cellUnloading max=%.2f sum=%.1f, states=%zu\n",
			            perCell, npcs, rep, inUpdates, in.max, in.sum,
			            inHostMax, inHostSum, g.updateMs.size(), out.max, out.sum, g.host.unloadingMsMax,
			            g.host.unloadingMsSum, g.cells.state().states());
		}
		// A walk at 20 m/s through the middle, a game frame each (scripts, classes,
		// physics, cells with the lookahead): the frame distribution of update().
		g.cameraTo(-150.0f, 150.0f);
		g.pump();
		g.updateMs.clear();
		const float dt = 1.0f / 60.0f;
		for (int f = 0; f < 2100; ++f)   // 700 m
		{
			g.cameraTo(-150.0f + 20.0f * dt * f, 150.0f);
			g.frames(1);
		}
		const auto walk = stats(g.updateMs);
		std::printf("VILLAGE_BENCH perCell=%d npcs=%d walk 20 m/s: frames=%zu p50=%.3f p99=%.3f max=%.2f ms, loads=%zu unloads=%zu\n",
		            perCell, npcs, g.updateMs.size(), walk.p50, walk.p99, walk.max,
		            g.cells.streamer().stats().loadsDone, g.cells.streamer().stats().unloads);
		std::fflush(stdout);
	}
}
