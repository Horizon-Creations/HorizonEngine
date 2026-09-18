// A breakpoint along the path the EDITOR takes, not the interpreter alone.
// test_hc_exec_trace.cpp proves that HcExecTrace attached to a Runtime stops
// a run at a breakpoint; that run is registered with Runtime::add and a key
// the test made up. In the editor the class is an ASSET: the class tab keys
// its breakpoints on the tab's content-relative path (HorizonCodeClassPanel:
// toContentRelativePath of the tab path), and play mode binds the class
// through EntityHost, which registers the instance under the ASSET's path
// (EntityHost::bind: ClassIdentity{ a->path }) and fires Construct +
// BeginPlay itself. If the two spellings ever drift, or the host bypasses
// the interpreter, the person sets a red disc and play runs straight
// through it — which is exactly what was reported. This test holds the two
// ends together: the key the panel would use, the host the play start uses.
#include "doctest.h"
#include "TestFsUtil.h"
#include "../src/HE_Editor/HcExecTrace.h"
#include "../src/HE_Editor/LevelScriptPanel.h"

#include <HorizonScene/EntityHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace HorizonCode;

namespace
{
	struct TempDir
	{
		fs::path path;
		explicit TempDir(const char* name)
		{
			path = fs::temp_directory_path() / name;
			he_test::removeAllQuiet(path);
			fs::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	// The module is process-wide state; every case starts and ends clean.
	struct Reset
	{
		Reset()  { wipe(); }
		~Reset() { wipe(); }
		static void wipe()
		{
			HcExecTrace::detach();
			HcExecTrace::clearHits();
			HcExecTrace::cancelReveal();
			HcExecTrace::setBreakpointStore("");
			HcExecTrace::clearAllBreakpoints();
			HcExecTrace::clearPaused();
			HcExecTrace::takeBreakHit();
		}
	};

	// BeginPlay → Print "a" → Print "b" → Set ran = 1. The breakpoint goes on
	// the second Print; `ran` says whether the chain got past it.
	struct ChainGraph
	{
		Graph graph;
		int   entry = 0, printA = 0, printB = 0, set = 0;
	};
	ChainGraph chainGraph()
	{
		ChainGraph c;
		Graph& g = c.graph;
		Variable v; v.name = "ran"; v.type = PinType::Int;
		g.variables.push_back(v);
		Node ev; ev.type = NodeType::Event; ev.s = "BeginPlay";
		c.entry = g.addNode(std::move(ev));
		auto print = [&g](const char* text)
		{
			Node cs; cs.type = NodeType::ConstString; cs.s = text;
			const int k = g.addNode(std::move(cs));
			Node pr; pr.type = NodeType::Print;
			const int p = g.addNode(std::move(pr));
			REQUIRE(g.connect(k, 0, p, 2));   // Print: execIn 0 / execOut 1 / Text 2
			return p;
		};
		c.printA = print("a");
		c.printB = print("b");
		Node one; one.type = NodeType::ConstInt; one.f[0] = 1.0f;
		const int k = g.addNode(std::move(one));
		Node st; st.type = NodeType::SetVariable; st.s = "ran"; st.propType = PinType::Int;
		c.set = g.addNode(std::move(st));
		REQUIRE(g.connect(c.entry,  0, c.printA, 0));
		REQUIRE(g.connect(c.printA, 1, c.printB, 0));
		REQUIRE(g.connect(c.printB, 1, c.set,    0));
		REQUIRE(g.connect(k,        0, c.set,    2));
		return c;
	}

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

	// The key the class tab files a breakpoint under: the panel is handed the
	// tab's path (an absolute file path) and turns it into a content-relative
	// one (HorizonCodeClassPanel::render → toContentRelativePath).
	std::string tabKeyOf(ContentManager& cm, const std::string& classPath)
	{
		return cm.toContentRelativePath(cm.resolveAbsolutePath(classPath));
	}
}

TEST_CASE("Play path: a breakpoint set on the class tab stops the class EntityHost binds at play start")
{
	Reset reset;
	TempDir dir("he_test_hc_breakpoint_play_path");
	ContentManager cm(dir.path.string());
	const ChainGraph c = chainGraph();
	const std::string cls = writeClass(cm, "Door", c.graph);

	HorizonWorld world;
	const Entity e = world.createEntity("Door1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc; sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
	}

	// The two spellings of the class must be one key, or nothing below can
	// work: this is the check that names the bug if they ever drift.
	const std::string tabKey = tabKeyOf(cm, cls);
	REQUIRE_FALSE(tabKey.empty());
	{
		const HorizonCodeClassAsset* a = cm.getHorizonCodeClass(cm.loadAsset(cls));
		REQUIRE(a);
		CHECK(a->path == tabKey);
	}

	// The editor's wiring: one runtime, attached once, the store of the open
	// project (in memory here), the breakpoint set the way the context menu
	// sets it — on the TAB key.
	Runtime rt;
	HcExecTrace::attach(rt);
	HcExecTrace::setBreakpoint(tabKey, c.printB, true);
	REQUIRE(HcExecTrace::hasBreakpoint(tabKey, c.printB));

	// Play start (EditorApplication::setPlayMode → EntityHost::begin): the
	// host loads the asset, registers it under the asset's path and fires
	// Construct + BeginPlay from inside begin().
	EntityHost host;
	host.begin(rt, world, cm);
	const InstanceId inst = host.instanceOf(e);
	REQUIRE(inst != 0);

	// The run is stopped BEFORE Print "b": the entry and Print "a" lit up,
	// "b" did not, the chain never reached the Set, and the shell got its
	// one-shot — the frame after this would freeze the world tick.
	REQUIRE(rt.isSuspended());
	const SuspendedRun* run = rt.suspendedRun(0);
	REQUIRE(run);
	CHECK(run->nodeId   == c.printB);
	CHECK(run->classKey == tabKey);
	CHECK(run->instance == inst);
	CHECK(rt.getVariable(inst, "ran").i == 0);
	CHECK(HcExecTrace::glowOf(tabKey, c.entry)  > 0.5f);
	CHECK(HcExecTrace::glowOf(tabKey, c.printA) > 0.5f);
	CHECK(HcExecTrace::glowOf(tabKey, c.printB) == 0.0f);
	CHECK(HcExecTrace::isPaused());
	CHECK(HcExecTrace::pausedNodeOf(tabKey) == c.printB);
	CHECK(HcExecTrace::pausedInstance() == inst);
	CHECK(HcExecTrace::takeBreakHit());
	// …and the reveal that opens the class tab names THIS tab.
	std::string revealTab;
	REQUIRE(HcExecTrace::takeRevealTab(revealTab));
	CHECK(revealTab == tabKey);

	// Continue (the parked HcResume::Continue of the next frame): the chain
	// finishes, the Set ran, nothing is stopped any more.
	rt.debugContinue();
	CHECK_FALSE(rt.isSuspended());
	CHECK(rt.getVariable(inst, "ran").i == 1);
	HcExecTrace::refreshPaused();
	CHECK_FALSE(HcExecTrace::isPaused());

	host.end();
}

TEST_CASE("Play path: without a breakpoint the same class runs straight through (negative control)")
{
	Reset reset;
	TempDir dir("he_test_hc_breakpoint_play_path_neg");
	ContentManager cm(dir.path.string());
	const ChainGraph c = chainGraph();
	const std::string cls = writeClass(cm, "Door", c.graph);

	HorizonWorld world;
	const Entity e = world.createEntity("Door1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc; sc.scriptAssetId = cm.loadAsset(cls);
		world.addComponent(e, sc);
	}

	Runtime rt;
	HcExecTrace::attach(rt);
	// A breakpoint on ANOTHER class must not stop this one either.
	HcExecTrace::setBreakpoint("Other.hasset", c.printB, true);

	EntityHost host;
	host.begin(rt, world, cm);
	const InstanceId inst = host.instanceOf(e);
	REQUIRE(inst != 0);

	CHECK_FALSE(rt.isSuspended());
	CHECK(rt.getVariable(inst, "ran").i == 1);
	CHECK_FALSE(HcExecTrace::isPaused());
	CHECK_FALSE(HcExecTrace::takeBreakHit());
	const std::string tabKey = tabKeyOf(cm, cls);
	CHECK(HcExecTrace::glowOf(tabKey, c.printB) > 0.5f);   // it ran, and lit up

	host.end();
}

TEST_CASE("Play path: a breakpoint set BEFORE play, on a class whose asset is loaded only at play start")
{
	// The order the person has: the tab is open (asset loaded once for the
	// tab), the breakpoint set, then Play — where the host loads the class
	// AGAIN through its own ContentManager path. The breakpoint must survive
	// that second load: it lives in HcExecTrace, not in the asset.
	Reset reset;
	TempDir dir("he_test_hc_breakpoint_play_path_reload");
	ContentManager cm(dir.path.string());
	const ChainGraph c = chainGraph();
	const std::string cls = writeClass(cm, "Door", c.graph);
	const std::string tabKey = tabKeyOf(cm, cls);

	Runtime rt;
	HcExecTrace::attach(rt);
	HcExecTrace::setBreakpoint(tabKey, c.printA, true);

	// A second ContentManager on the same root, the way a fresh project open
	// would see it: the asset is read from disk, its path is what the file
	// index says it is.
	ContentManager cm2(dir.path.string());
	HorizonWorld world;
	const Entity e = world.createEntity("Door1");
	world.addComponent(e, TransformComponent{});
	{
		ScriptComponent sc; sc.scriptAssetId = cm2.loadAsset(cls);
		world.addComponent(e, sc);
	}
	EntityHost host;
	host.begin(rt, world, cm2);
	REQUIRE(host.instanceOf(e) != 0);
	REQUIRE(rt.isSuspended());
	CHECK(rt.suspendedRun(0)->nodeId == c.printA);
	CHECK(HcExecTrace::pausedNodeOf(tabKey) == c.printA);
	rt.debugContinue();
	host.end();
}

// ── The Level Script ─────────────────────────────────────────────────────────
// The other graph a person sets a breakpoint in — the first one, usually. The
// Level Script tab keys its breakpoints on LevelScriptPanel::kTabPath, and the
// runtime spells the level as "level:<uuid>" (HorizonWorld::setLevelScriptKey,
// mapped to the tab by HcExecTrace::tabKeyFor). Play start runs it through
// HorizonWorld::fireLevelLoaded on the app-wide runtime the editor injected
// (EditorApplication: setScriptRuntime + attach on the same runtime).
namespace
{
	// OnLevelLoaded → Print "a" → Print "b" → Set ran = 1, like chainGraph.
	ChainGraph levelChainGraph()
	{
		ChainGraph c = chainGraph();
		c.graph.findNode(c.entry)->s = "OnLevelLoaded";
		return c;
	}
}

TEST_CASE("Play path: a breakpoint set on the Level Script tab stops the level script fireLevelLoaded runs")
{
	Reset reset;
	const ChainGraph c = levelChainGraph();
	Runtime rt;
	HcExecTrace::attach(rt);
	HorizonWorld world;
	world.setScriptRuntime(&rt);
	world.setLevelScriptJson(toJson(c.graph));

	SUBCASE("keyed by the scene, the way the packaged game and the editor's play start key it")
	{
		world.setLevelScriptKey("level:0123456789abcdef0123456789abcdef");
	}
	SUBCASE("never keyed — a host that only ever interprets, and every editor before this fix")
	{
		// The editor did not set a key at all (setLevelScriptKey was the
		// packaged game's business, for the compiled lookup), so the level
		// script ran under an EMPTY class key: no hit was recorded, no
		// breakpoint ever matched, no console line could find its node. The
		// world must fall back to a level key of its own.
	}

	HcExecTrace::setBreakpoint(LevelScriptPanel::kTabPath, c.printB, true);
	world.fireLevelLoaded();

	REQUIRE(rt.isSuspended());
	const SuspendedRun* run = rt.suspendedRun(0);
	REQUIRE(run);
	CHECK(run->nodeId == c.printB);
	CHECK(run->classKey.rfind("level:", 0) == 0);
	CHECK(HcExecTrace::tabKeyFor(run->classKey) == LevelScriptPanel::kTabPath);
	CHECK(HcExecTrace::glowOf(LevelScriptPanel::kTabPath, c.entry)  > 0.5f);
	CHECK(HcExecTrace::glowOf(LevelScriptPanel::kTabPath, c.printA) > 0.5f);
	CHECK(HcExecTrace::glowOf(LevelScriptPanel::kTabPath, c.printB) == 0.0f);
	CHECK(HcExecTrace::isPaused());
	CHECK(HcExecTrace::pausedNodeOf(LevelScriptPanel::kTabPath) == c.printB);
	CHECK(HcExecTrace::takeBreakHit());
	std::string revealTab;
	REQUIRE(HcExecTrace::takeRevealTab(revealTab));
	CHECK(revealTab == LevelScriptPanel::kTabPath);

	rt.debugContinue();
	CHECK_FALSE(rt.isSuspended());
	HcExecTrace::refreshPaused();
	CHECK_FALSE(HcExecTrace::isPaused());
	world.fireLevelUnloaded();
}

TEST_CASE("Play path: without a breakpoint the level script runs straight through and lights up (negative control)")
{
	Reset reset;
	const ChainGraph c = levelChainGraph();
	Runtime rt;
	HcExecTrace::attach(rt);
	HorizonWorld world;
	world.setScriptRuntime(&rt);
	world.setLevelScriptJson(toJson(c.graph));
	HcExecTrace::setBreakpoint("Other.hasset", c.printB, true);   // someone else's

	world.fireLevelLoaded();
	CHECK_FALSE(rt.isSuspended());
	CHECK_FALSE(HcExecTrace::isPaused());
	CHECK_FALSE(HcExecTrace::takeBreakHit());
	// The highlighting of the level script was dead for the same reason the
	// breakpoint was: this is the line that says it lives.
	CHECK(HcExecTrace::glowOf(LevelScriptPanel::kTabPath, c.printB) > 0.5f);
	CHECK(HcExecTrace::lastInstanceOf(LevelScriptPanel::kTabPath) != 0);
	world.fireLevelUnloaded();
}
