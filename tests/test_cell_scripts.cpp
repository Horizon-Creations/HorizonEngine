// The scripts of a streamed cell (Thema 183 step 3): a cell's HorizonCode classes
// and Lua scripts start when the cell is built and stop before it goes, through the
// one helper both hosts call (HE::startCellScripts / stopCellScripts in
// ICellHost::cellLoaded / cellUnloading); a class's Save Game variables survive the
// unload (CellState, step 2, reads them while the scripts still run); what a script
// hung out of the cell keeps its script.
#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/CellRuntime.h>
#include <HorizonScene/CellScripts.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/SaveStateComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace HorizonCode;

namespace
{
const Entity kNone = entt::null;   // a handle doctest can compare with

TransformComponent& tf(HorizonWorld& w, Entity e)
{
	return w.registry().get_or_emplace<TransformComponent>(e);
}

// A class that counts its lifecycle events, each in an Int variable of its own
// (test_entity_host.cpp's lifecycle graph).
Graph lifecycleGraph()
{
	Graph g;
	for (const char* n : { "constructs", "beginPlays", "destructs" })
	{
		Variable v; v.name = n; v.type = PinType::Int;
		g.variables.push_back(v);
	}
	auto count = [&g](const char* event, const char* var)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(std::move(ev));
		Node get; get.type = NodeType::GetVariable; get.s = var; get.propType = PinType::Int;
		const int gv = g.addNode(std::move(get));
		Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
		const int k = g.addNode(std::move(one));
		Node add; add.type = NodeType::Add;
		const int a = g.addNode(std::move(add));
		Node set; set.type = NodeType::SetVariable; set.s = var; set.propType = PinType::Int;
		const int s = g.addNode(std::move(set));
		REQUIRE(g.connect(gv, 0, a, 0));
		REQUIRE(g.connect(k,  0, a, 1));
		REQUIRE(g.connect(e,  0, s, 0));
		REQUIRE(g.connect(a,  2, s, 2));
	};
	count("Construct", "constructs");
	count("BeginPlay", "beginPlays");
	count("Destruct",  "destructs");
	return g;
}

// A class with a Save Game variable, `gold`, whose BeginPlay copies what `gold` is
// at that moment into `goldAtBeginPlay` (not saved): which of the two comes first,
// BeginPlay or the restored variable, is what the test pins.
Graph guardGraph()
{
	Graph g;
	{ Variable v; v.name = "gold"; v.type = PinType::Int; v.saveGame = true; g.variables.push_back(v); }
	{ Variable v; v.name = "goldAtBeginPlay"; v.type = PinType::Int; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "BeginPlay";
	const int e = g.addNode(std::move(ev));
	Node get; get.type = NodeType::GetVariable; get.s = "gold"; get.propType = PinType::Int;
	const int gv = g.addNode(std::move(get));
	Node set; set.type = NodeType::SetVariable; set.s = "goldAtBeginPlay"; set.propType = PinType::Int;
	const int s = g.addNode(std::move(set));
	REQUIRE(g.connect(e,  0, s, 0));
	REQUIRE(g.connect(gv, 0, s, 2));
	return g;
}

// onStart counts in a Lua global, so a second start shows.
constexpr const char* kTorchLua = R"lua(
local M = {}
function M.onStart(self)
    _torchStarts = (_torchStarts or 0) + 1
end
return M
)lua";

std::string writeClass(ContentManager& cm, const char* name, const Graph& g)
{
	HorizonCodeClassAsset a;
	a.type      = HE::AssetType::HorizonCodeClass;
	a.name      = name;
	a.path      = std::string(name) + ".hasset";
	a.baseClass = "Entity";
	a.graphJson = toJson(g);
	REQUIRE(cm.saveAsset(a));
	return a.path;
}

std::string writeLua(ContentManager& cm, const char* name, const char* source)
{
	ScriptAsset a;
	a.type       = HE::AssetType::Script;
	a.name       = name;
	a.path       = std::string(name) + ".hasset";
	a.sourceCode = source;
	a.language   = HE::ScriptLanguage::Lua;
	REQUIRE(cm.saveAsset(a));
	return a.path;
}

// The project: a content folder with the two classes and the Lua script, and one
// 100 m cell at 0,0 (version 2: stable ids) holding
//   Guard     the guard class and a SaveStateComponent
//   Wanderer  the lifecycle class, no SaveStateComponent
//   Torch     the Lua script
//   Rock      nothing to run
struct Project
{
	std::filesystem::path root;
	ContentManager        cm;

	explicit Project(const char* name)
		: root(std::filesystem::temp_directory_path() / name)
		, cm((prepare(root) / "Content").string())
	{
		const HE::UUID guard    = cm.loadAsset(writeClass(cm, "Guard", guardGraph()));
		const HE::UUID wanderer = cm.loadAsset(writeClass(cm, "Wanderer", lifecycleGraph()));
		const HE::UUID torch    = cm.loadAsset(writeLua(cm, "Torch", kTorchLua));

		HorizonWorld cell;
		auto place = [&](const char* n, float x)
		{
			const Entity e = cell.createEntity(n);
			tf(cell, e).position = { x, 0.0f, 50.0f };
			return e;
		};
		auto script = [&](Entity e, const HE::UUID& asset, const char* module)
		{
			ScriptComponent sc;
			sc.scriptAssetId = asset;
			sc.moduleName    = module;
			cell.addComponent(e, sc);
		};
		const Entity g = place("Guard", 40.0f);
		script(g, guard, "Guard");
		cell.addComponent(g, SaveStateComponent{});
		script(place("Wanderer", 45.0f), wanderer, "Wanderer");
		script(place("Torch", 50.0f), torch, "torch");
		place("Rock", 55.0f);

		const auto file = root / "W.cells" / "cell_0_0.hescene";
		std::filesystem::create_directories(file.parent_path());
		SceneSerializer ser;
		REQUIRE(ser.save(cell, file, SerializeFormat::JSON));
		nlohmann::json j;
		{
			std::ifstream in(file);
			j = nlohmann::json::parse(in);
		}
		j["streaming"] = { { "version", 2 } };
		std::ofstream out(file, std::ios::trunc);
		out << j.dump();
	}
	static std::filesystem::path prepare(const std::filesystem::path& root)
	{
		he_test::removeAllQuiet(root);
		std::filesystem::create_directories(root / "Content");
		return root;
	}
};

constexpr const char* kManifest =
	R"({"version": 2, "cellSize": 100, "loadRadius": 60, "unloadRadius": 120, "lookaheadSec": 2,
	    "dir": "W.cells", "list": [[0,0,4,0]]})";

// A host like the game's: the cell's scripts go through the shared helper, and the
// state's Ctx carries the script runtime and the entity host.
struct Host final : HE::ICellHost
{
	std::filesystem::path root;
	ContentManager*       cm = nullptr;
	HE::CellScriptHosts   scripts;
	// At cellUnloading, before the helper ran: how many class instances were bound,
	// and whether every entity of the cell was still there.
	size_t boundAtUnload = 0;
	bool   cellWholeAtUnload = false;

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
	PhysicsWorld* cellPhysics() override { return nullptr; }
	void streamCellAssets(HorizonWorld&, const HE::CancelToken&, const std::vector<uint32_t>&) override {}
	void cellLoaded(HorizonWorld& world, entt::entity, const std::vector<entt::entity>& created) override
	{
		HE::startCellScripts(scripts, world, created);
	}
	void cellUnloading(HorizonWorld& world, entt::entity root) override
	{
		boundAtUnload = scripts.entities ? scripts.entities->count() : 0;
		int there = 0;
		const auto* h = world.registry().try_get<HierarchyComponent>(root);
		if (h)
			for (entt::entity c : h->children)
				if (world.registry().valid(c)) ++there;
		cellWholeAtUnload = there == 4;   // Guard, Wanderer, Torch, Rock
		HE::stopCellScripts(scripts, world, root);
	}
	void cellApiCtx(HE::api::Ctx& ctx) override
	{
		ctx.content  = cm;
		ctx.runtime  = scripts.runtime;
		ctx.entities = scripts.entities;
	}
};

Entity named(HorizonWorld& world, const std::string& name)
{
	Entity found = entt::null;
	int n = 0;
	for (auto [e, nc] : world.registry().view<NameComponent>().each())
		if (nc.name == name) { found = e; ++n; }
	return n == 1 ? found : entt::null;
}

template <class Done>
void pump(HE::CellRuntime& rt, HorizonWorld& world, Done done)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < until)
	{
		rt.update(world, 0.0f);
		const auto& st = rt.streamer().stats();
		if (done() && st.inFlight == 0 && st.ready == 0 && st.building == 0) return;
		std::this_thread::yield();
	}
}

// Everything a play session has: the world with a camera, the class runtime and
// its entity host, the Lua context and its instance map, and the cell runtime.
struct Session
{
	Project&                   project;
	HorizonWorld               world;
	Entity                     cam = entt::null;
	Runtime                    rt;
	EntityHost                 classes;
	ScriptContext              text{ world };
	ScriptContext::InstanceMap textInstances;
	Host                       host;
	HE::CellRuntime            cells{ host };

	explicit Session(Project& p) : project(p)
	{
		cam = world.createEntity("Camera");
		tf(world, cam).position = { 1000.0f, 1.7f, 50.0f };
		world.registry().emplace<CameraComponent>(cam);
		classes.begin(rt, world, p.cm);
		host.root                  = p.root;
		host.cm                    = &p.cm;
		host.scripts.entities      = &classes;
		host.scripts.text          = &text;
		host.scripts.textInstances = &textInstances;
		host.scripts.runtime       = &rt;
		host.scripts.content       = &p.cm;
		world.setCellManifestJson(kManifest);
	}
	~Session() { classes.end(); }

	void load()
	{
		tf(world, cam).position.x = 50.0f;
		pump(cells, world, [&] { return cells.streamer().isLoaded(0, 0); });
		REQUIRE(cells.streamer().isLoaded(0, 0));
	}
	void unload()
	{
		tf(world, cam).position.x = 1000.0f;
		pump(cells, world, [&] { return !cells.streamer().isLoaded(0, 0); });
		REQUIRE_FALSE(cells.streamer().isLoaded(0, 0));
	}
	InstanceId instanceOn(const char* name) { return classes.instanceOf(named(world, name)); }
	double     torchStarts() { return text.engine().getGlobalNumber("_torchStarts"); }
};
} // namespace

TEST_CASE("Cell scripts: a cell's classes and Lua scripts start when it is built and stop before it goes")
{
	Project p("he_cell_scripts_lifecycle");
	Session s(p);
	CHECK(s.classes.count() == 0);   // nothing in the world runs anything before the cell

	s.load();
	CHECK(s.classes.count() == 2);   // Guard and Wanderer; the Rock runs nothing
	const InstanceId first = s.instanceOn("Wanderer");
	REQUIRE(first != 0);
	CHECK(s.rt.getVariable(first, "constructs").i == 1);
	CHECK(s.rt.getVariable(first, "beginPlays").i == 1);
	const Entity torch = named(s.world, "Torch");
	REQUIRE(torch != kNone);
	CHECK(s.textInstances.count(static_cast<uint32_t>(torch)) == 1);
	CHECK(s.torchStarts() == doctest::Approx(1.0));

	s.unload();
	// The host heard the unload with both classes still bound and the cell whole…
	CHECK(s.host.boundAtUnload == 2);
	CHECK(s.host.cellWholeAtUnload);
	// …and after it nothing of the cell runs. Nobody ticked the entity host, so its
	// reaper never ran: the instance went with the cell, not with a later sweep.
	CHECK(s.classes.count() == 0);
	CHECK_FALSE(s.rt.alive(first));
	CHECK(s.textInstances.empty());

	// Loaded again: new instances, each started once.
	s.load();
	CHECK(s.classes.count() == 2);
	const InstanceId second = s.instanceOn("Wanderer");
	REQUIRE(second != 0);
	CHECK(second != first);
	CHECK(s.rt.getVariable(second, "constructs").i == 1);
	CHECK(s.rt.getVariable(second, "beginPlays").i == 1);
	CHECK(s.textInstances.size() == 1);
	CHECK(s.torchStarts() == doctest::Approx(2.0));
}

TEST_CASE("Cell scripts: a class's Save Game variable survives the unload, and is put back after BeginPlay")
{
	Project p("he_cell_scripts_savegame");
	Session s(p);
	s.load();
	const InstanceId guard = s.instanceOn("Guard");
	REQUIRE(guard != 0);
	CHECK(s.rt.getVariable(guard, "goldAtBeginPlay").i == 0);
	s.rt.setVariable(guard, "gold", Value::ofInt(42));
	s.rt.setVariable(s.instanceOn("Wanderer"), "beginPlays", Value::ofInt(7));   // not saved

	s.unload();
	// CellState wrote the variable out while the class still ran (it goes before the
	// host in the unload).
	CHECK(s.cells.state().states() == 1);

	s.load();
	const InstanceId again = s.instanceOn("Guard");
	REQUIRE(again != 0);
	CHECK(s.rt.getVariable(again, "gold").i == 42);
	// The order, as decided (plan section 18): BeginPlay ran before the variable was
	// put back, so it saw the default. A class that needs the restored value in
	// BeginPlay reads it on its first Tick, or calls Apply Saved State itself.
	CHECK(s.rt.getVariable(again, "goldAtBeginPlay").i == 0);
	// Without a SaveStateComponent nothing is kept: a fresh start.
	CHECK(s.rt.getVariable(s.instanceOn("Wanderer"), "beginPlays").i == 1);
}

TEST_CASE("Cell scripts: what a script hung out of the cell keeps its script and is not started twice")
{
	Project p("he_cell_scripts_hungout");
	Session s(p);
	s.load();
	const Entity wanderer = named(s.world, "Wanderer");
	const InstanceId inst = s.classes.instanceOf(wanderer);
	REQUIRE(inst != 0);
	REQUIRE(s.world.reparentEntity(wanderer, s.world.rootEntity()));

	s.unload();
	CHECK(s.world.registry().valid(wanderer));
	CHECK(s.classes.count() == 1);
	CHECK(s.classes.instanceOf(wanderer) == inst);
	CHECK(s.rt.alive(inst));

	// Loaded again: the Wanderer is a tombstone of the cell now (it left it), so the
	// cell does not bring a second one, and the one in the base keeps its instance.
	s.load();
	CHECK(named(s.world, "Wanderer") == wanderer);
	CHECK(s.classes.instanceOf(wanderer) == inst);
	CHECK(s.rt.getVariable(inst, "beginPlays").i == 1);
	CHECK(s.classes.count() == 2);   // the Guard again, and the Wanderer from before
}

TEST_CASE("Cell scripts: starting what already runs is a no-op, and the helper survives hosts that are down")
{
	Project p("he_cell_scripts_helper");
	Session s(p);
	s.load();
	std::vector<Entity> all;
	for (auto [e, nc] : s.world.registry().view<NameComponent>().each()) all.push_back(e);
	const InstanceId guard = s.instanceOn("Guard");
	const Entity     torch = named(s.world, "Torch");
	const auto       torchInst = s.textInstances.at(static_cast<uint32_t>(torch));

	// Everything is running already: nothing starts, nothing is replaced.
	CHECK(HE::startCellScripts(s.host.scripts, s.world, all) == 0);
	CHECK(s.classes.count() == 2);
	CHECK(s.instanceOn("Guard") == guard);
	CHECK(s.rt.getVariable(guard, "goldAtBeginPlay").i == 0);
	CHECK(s.textInstances.at(static_cast<uint32_t>(torch)) == torchInst);
	CHECK(s.torchStarts() == doctest::Approx(1.0));

	// The editor's Stop: the entity host ends first, the cells go after (clear), and
	// only the Lua instances are left for the helper to stop.
	s.classes.end();
	s.cells.clear(s.world);
	CHECK_FALSE(s.cells.streamer().isLoaded(0, 0));
	CHECK(s.textInstances.empty());
	CHECK(named(s.world, "Torch") == kNone);
}
