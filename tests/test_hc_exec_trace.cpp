#include "doctest.h"
#include "../src/HE_Editor/HcExecTrace.h"
#include "../src/HE_Editor/LevelScriptPanel.h"
#include "../src/HE_Editor/GameInstancePanel.h"

#include "TestFsUtil.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace HorizonCode;

// ── The editor's keeper of "which node just ran" and "show me that node" ─────
// The interpreter side (Runtime::setExecListener, currentExecSite) is covered
// in test_horizoncode_runtime.cpp; this is the editor module on top of it.

namespace
{
	struct Reset
	{
		Reset()  { wipe(); }
		~Reset() { wipe(); }
		static void wipe()
		{
			HcExecTrace::detach();
			HcExecTrace::clearHits();
			HcExecTrace::cancelReveal();
			HcExecTrace::setBreakpointStore("");   // in memory only — and empty
			HcExecTrace::clearAllBreakpoints();
			HcExecTrace::clearPaused();
			HcExecTrace::takeBreakHit();
		}
	};
}

TEST_CASE("HcExecTrace: runtime keys map to the tabs that show them")
{
	// The two editor-owned graphs go to their reserved tab paths — the level
	// script regardless of which scene's uuid the key carries.
	CHECK(HcExecTrace::tabKeyFor("__game_instance__") == GameInstancePanel::kTabPath);
	CHECK(HcExecTrace::tabKeyFor("level:0123456789abcdef0123456789abcdef") == LevelScriptPanel::kTabPath);
	CHECK(HcExecTrace::tabKeyFor("level:Main") == LevelScriptPanel::kTabPath);
	// A class or widget asset is already the key its panel uses.
	CHECK(HcExecTrace::tabKeyFor("Content/Enemy.hasset") == "Content/Enemy.hasset");
	CHECK(HcExecTrace::tabKeyFor("UI/Menu.hasset") == "UI/Menu.hasset");
	// A tab key passes through unchanged, so callers may hand over either.
	CHECK(HcExecTrace::tabKeyFor(LevelScriptPanel::kTabPath) == LevelScriptPanel::kTabPath);
	CHECK(HcExecTrace::tabKeyFor("").empty());
}

TEST_CASE("HcExecTrace: a hit glows fully at once and fades to nothing over the window")
{
	Reset reset;
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 100.0) == 0.0f);   // never ran

	HcExecTrace::recordHitAt("Content/A.hasset", 7, 42u, 100.0);
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 100.0) == doctest::Approx(1.0f));
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 100.0 + HcExecTrace::kFadeSeconds / 2.0)
	      == doctest::Approx(0.5f));
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 100.0 + HcExecTrace::kFadeSeconds) == 0.0f);
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 1000.0) == 0.0f);
	// A clock that stepped backwards reads as "just now", never as negative.
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 99.0) == doctest::Approx(1.0f));

	// Another node of the same graph, and the same node of another graph, are
	// untouched.
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 8, 100.0) == 0.0f);
	CHECK(HcExecTrace::glowAt("Content/B.hasset", 7, 100.0) == 0.0f);

	// A later hit restarts the fade, and names the instance that ran it.
	HcExecTrace::recordHitAt("Content/A.hasset", 7, 43u, 200.0);
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 200.0) == doctest::Approx(1.0f));
	CHECK(HcExecTrace::lastInstanceOf("Content/A.hasset") == 43u);
	CHECK(HcExecTrace::lastInstanceOf("Content/B.hasset") == 0u);

	HcExecTrace::clearHits();
	CHECK(HcExecTrace::glowAt("Content/A.hasset", 7, 200.0) == 0.0f);
	CHECK(HcExecTrace::lastInstanceOf("Content/A.hasset") == 0u);
}

TEST_CASE("HcExecTrace: hits are filed under the TAB key, whatever the runtime called the graph")
{
	Reset reset;
	HcExecTrace::recordHitAt("level:abc", 3, 1u, 10.0);
	HcExecTrace::recordHitAt("__game_instance__", 4, 2u, 10.0);
	CHECK(HcExecTrace::glowAt(LevelScriptPanel::kTabPath, 3, 10.0) == doctest::Approx(1.0f));
	CHECK(HcExecTrace::glowAt(GameInstancePanel::kTabPath, 4, 10.0) == doctest::Approx(1.0f));
	// The runtime key itself is not a tab, so nothing is filed under it.
	CHECK(HcExecTrace::glowAt("level:abc", 3, 10.0) == 0.0f);
	// A keyless instance (a bare test graph) and node 0 are not recorded.
	HcExecTrace::recordHitAt("", 3, 1u, 10.0);
	HcExecTrace::recordHitAt("Content/X.hasset", 0, 1u, 10.0);
	CHECK(HcExecTrace::glowAt("", 3, 10.0) == 0.0f);
	CHECK(HcExecTrace::glowAt("Content/X.hasset", 0, 10.0) == 0.0f);
}

TEST_CASE("HcExecTrace: a reveal is two one-shots — the shell's tab, then the panel's node")
{
	Reset reset;
	std::string tab;
	int node = 0;
	CHECK_FALSE(HcExecTrace::takeRevealTab(tab));
	CHECK_FALSE(HcExecTrace::takeRevealNode("Content/A.hasset", node));

	HcExecTrace::requestReveal("level:abc", 12);
	// The shell sees the tab once …
	REQUIRE(HcExecTrace::takeRevealTab(tab));
	CHECK(tab == LevelScriptPanel::kTabPath);
	CHECK_FALSE(HcExecTrace::takeRevealTab(tab));
	// … the node stays pending for the right panel, however many frames later,
	// and a panel for another tab asking in between gets nothing.
	CHECK(HcExecTrace::revealPendingFor(LevelScriptPanel::kTabPath));
	CHECK_FALSE(HcExecTrace::takeRevealNode("Content/A.hasset", node));
	CHECK(node == 0);
	REQUIRE(HcExecTrace::takeRevealNode(LevelScriptPanel::kTabPath, node));
	CHECK(node == 12);
	// Consumed entirely.
	CHECK_FALSE(HcExecTrace::takeRevealNode(LevelScriptPanel::kTabPath, node));
	CHECK_FALSE(HcExecTrace::revealPendingFor(LevelScriptPanel::kTabPath));
	CHECK_FALSE(HcExecTrace::takeRevealTab(tab));
}

TEST_CASE("HcExecTrace: a newer reveal replaces a pending one; cancel drops it; junk is ignored")
{
	Reset reset;
	std::string tab;
	int node = 0;
	HcExecTrace::requestReveal("Content/A.hasset", 1);
	HcExecTrace::requestReveal("Content/B.hasset", 2);
	REQUIRE(HcExecTrace::takeRevealTab(tab));
	CHECK(tab == "Content/B.hasset");
	CHECK_FALSE(HcExecTrace::takeRevealNode("Content/A.hasset", node));
	REQUIRE(HcExecTrace::takeRevealNode("Content/B.hasset", node));
	CHECK(node == 2);

	HcExecTrace::requestReveal("Content/C.hasset", 3);
	HcExecTrace::cancelReveal();
	CHECK_FALSE(HcExecTrace::takeRevealTab(tab));
	CHECK_FALSE(HcExecTrace::takeRevealNode("Content/C.hasset", node));

	HcExecTrace::requestReveal("", 3);
	HcExecTrace::requestReveal("Content/C.hasset", 0);
	CHECK_FALSE(HcExecTrace::takeRevealTab(tab));
}

TEST_CASE("HcExecTrace: attached to a runtime, a fired event lights its chain under the class key")
{
	Reset reset;
	// Event(Ping) → Print("x"). Print: execIn 0 / Text dataIn 2; ConstString dataOut 0.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Ping";
	const int e = g.addNode(ev);
	Node cs; cs.type = NodeType::ConstString; cs.s = "x";
	const int c = g.addNode(cs);
	Node pr; pr.type = NodeType::Print;
	const int p = g.addNode(pr);
	REQUIRE(g.connect(e, 0, p, 0));
	REQUIRE(g.connect(c, 0, p, 2));

	Runtime rt;
	HcExecTrace::attach(rt);
	ClassIdentity cls; cls.key = "Content/Lit.hasset";
	const InstanceId id = rt.add(g, {}, cls);
	rt.fireEvent(id, "Ping");

	// Read straight after the fire: well inside the window.
	CHECK(HcExecTrace::glowOf("Content/Lit.hasset", e) > 0.5f);
	CHECK(HcExecTrace::glowOf("Content/Lit.hasset", p) > 0.5f);
	// The pure ConstString is read, not executed — no glow.
	CHECK(HcExecTrace::glowOf("Content/Lit.hasset", c) == 0.0f);
	CHECK(HcExecTrace::lastInstanceOf("Content/Lit.hasset") == id);
}

// ── Breakpoints and the stop ─────────────────────────────────────────────────

TEST_CASE("HcExecTrace: breakpoints are filed under the tab key and answer the runtime's question")
{
	Reset reset;
	CHECK(HcExecTrace::breakpointCount() == 0);
	CHECK_FALSE(HcExecTrace::hasBreakpoint("Content/A.hasset", 3));

	HcExecTrace::setBreakpoint("Content/A.hasset", 3, true);
	HcExecTrace::toggleBreakpoint("Content/A.hasset", 5);
	// The level script's breakpoints are set on its TAB and asked for under
	// the runtime's "level:<uuid>" spelling — whichever scene is playing.
	HcExecTrace::setBreakpoint(LevelScriptPanel::kTabPath, 9, true);
	CHECK(HcExecTrace::breakpointCount() == 3);
	CHECK(HcExecTrace::hasBreakpoint("Content/A.hasset", 3));
	CHECK(HcExecTrace::hasBreakpoint("Content/A.hasset", 5));
	CHECK_FALSE(HcExecTrace::hasBreakpoint("Content/A.hasset", 4));
	CHECK_FALSE(HcExecTrace::hasBreakpoint("Content/B.hasset", 3));
	CHECK(HcExecTrace::breakpointsOf("Content/A.hasset") == std::vector<int>{ 3, 5 });
	CHECK(HcExecTrace::shouldBreakAt("Content/A.hasset", 3));
	CHECK(HcExecTrace::shouldBreakAt("level:0123456789abcdef0123456789abcdef", 9));
	CHECK_FALSE(HcExecTrace::shouldBreakAt("level:0123456789abcdef0123456789abcdef", 3));

	// Toggle off, clear one graph, clear all. Junk is ignored.
	HcExecTrace::toggleBreakpoint("Content/A.hasset", 5);
	CHECK_FALSE(HcExecTrace::hasBreakpoint("Content/A.hasset", 5));
	HcExecTrace::clearBreakpoints("Content/A.hasset");
	CHECK(HcExecTrace::breakpointsOf("Content/A.hasset").empty());
	CHECK(HcExecTrace::breakpointCount() == 1);
	HcExecTrace::setBreakpoint("", 3, true);
	HcExecTrace::setBreakpoint("Content/A.hasset", 0, true);
	CHECK(HcExecTrace::breakpointCount() == 1);
	HcExecTrace::clearAllBreakpoints();
	CHECK(HcExecTrace::breakpointCount() == 0);
}

TEST_CASE("HcExecTrace: a stop marks the node, reveals it, and flags the shell once")
{
	Reset reset;
	CHECK_FALSE(HcExecTrace::isPaused());
	CHECK_FALSE(HcExecTrace::takeBreakHit());

	HcExecTrace::recordStop("level:abc", 7, 42);
	CHECK(HcExecTrace::isPaused());
	CHECK(HcExecTrace::pausedNodeOf(LevelScriptPanel::kTabPath) == 7);
	CHECK(HcExecTrace::pausedNodeOf("Content/A.hasset") == 0);
	CHECK(HcExecTrace::pausedInstance() == 42);
	CHECK(HcExecTrace::takeBreakHit());
	CHECK_FALSE(HcExecTrace::takeBreakHit());      // one-shot
	// Revealed like a console click: the shell's tab half, then the panel's node.
	std::string tab;
	REQUIRE(HcExecTrace::takeRevealTab(tab));
	CHECK(tab == LevelScriptPanel::kTabPath);
	int node = 0;
	REQUIRE(HcExecTrace::takeRevealNode(LevelScriptPanel::kTabPath, node));
	CHECK(node == 7);

	// A second stop while one is shown queues (the runtime keeps the order);
	// the marker stays on the first, the shell is flagged again.
	HcExecTrace::recordStop("Content/A.hasset", 3, 43);
	CHECK(HcExecTrace::pausedNodeOf(LevelScriptPanel::kTabPath) == 7);
	CHECK(HcExecTrace::pausedNodeOf("Content/A.hasset") == 0);
	CHECK(HcExecTrace::takeBreakHit());

	HcExecTrace::clearPaused();
	CHECK_FALSE(HcExecTrace::isPaused());
	CHECK(HcExecTrace::pausedNodeOf(LevelScriptPanel::kTabPath) == 0);
}

TEST_CASE("HcExecTrace: attached to a runtime, a breakpoint stops the run and refresh follows it")
{
	Reset reset;
	// Ping → P1 → P2. Print: execIn 0 / execOut 1 / Text dataIn 2.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Ping";
	const int e = g.addNode(ev);
	int prints[2];
	for (int& p : prints)
	{
		Node cs; cs.type = NodeType::ConstString; cs.s = "x";
		const int c = g.addNode(cs);
		Node pr; pr.type = NodeType::Print;
		p = g.addNode(pr);
		REQUIRE(g.connect(c, 0, p, 2));
	}
	REQUIRE(g.connect(e, 0, prints[0], 0));
	REQUIRE(g.connect(prints[0], 1, prints[1], 0));

	Runtime rt;
	HcExecTrace::attach(rt);
	ClassIdentity cls; cls.key = "Content/Stop.hasset";
	const InstanceId id = rt.add(g, {}, cls);

	HcExecTrace::setBreakpoint("Content/Stop.hasset", prints[0], true);
	rt.fireEvent(id, "Ping");
	// Stopped before P1: the entry lit up, P1 did not, and the marker is on P1.
	REQUIRE(rt.isSuspended());
	CHECK(HcExecTrace::glowOf("Content/Stop.hasset", e) > 0.5f);
	CHECK(HcExecTrace::glowOf("Content/Stop.hasset", prints[0]) == 0.0f);
	CHECK(HcExecTrace::pausedNodeOf("Content/Stop.hasset") == prints[0]);
	CHECK(HcExecTrace::pausedInstance() == id);
	CHECK(HcExecTrace::takeBreakHit());

	// Step: the run moves to P2; the marker follows on refresh (the shell's
	// once-per-frame call), and the new site is revealed.
	HcExecTrace::cancelReveal();
	rt.debugStep();
	CHECK(HcExecTrace::pausedNodeOf("Content/Stop.hasset") == prints[0]);   // not yet refreshed
	HcExecTrace::refreshPaused();
	CHECK(HcExecTrace::pausedNodeOf("Content/Stop.hasset") == prints[1]);
	std::string tab;
	REQUIRE(HcExecTrace::takeRevealTab(tab));
	CHECK(tab == "Content/Stop.hasset");
	CHECK(HcExecTrace::takeBreakHit());            // the step's stop told the shell too

	// Continue: the run ends, and refresh clears the marker.
	rt.debugContinue();
	CHECK_FALSE(rt.isSuspended());
	CHECK(HcExecTrace::glowOf("Content/Stop.hasset", prints[1]) > 0.5f);
	HcExecTrace::refreshPaused();
	CHECK_FALSE(HcExecTrace::isPaused());
	CHECK(HcExecTrace::pausedNodeOf("Content/Stop.hasset") == 0);

	// Without the breakpoint the next fire runs straight through.
	HcExecTrace::clearAllBreakpoints();
	rt.fireEvent(id, "Ping");
	CHECK_FALSE(rt.isSuspended());
	CHECK_FALSE(HcExecTrace::takeBreakHit());
}

TEST_CASE("HcExecTrace: the store round-trips breakpoints per project and replaces, never merges")
{
	Reset reset;
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "he_test_hc_breakpoints";
	he_test::removeAllQuiet(root);
	fs::create_directories(root);
	struct Cleanup { fs::path r; ~Cleanup() { HcExecTrace::setBreakpointStore(""); he_test::removeAllQuiet(r); } } cleanup{ root };

	// The layout: a .heproj path and its directory name the same file.
	const std::string proj = (root / "Game.heproj").string();
	{ std::ofstream(proj) << "{}"; }
	const std::string store = HcExecTrace::breakpointStoreForProject(proj);
	CHECK(store == (root / "Saved" / "Breakpoints.json").string());
	CHECK(HcExecTrace::breakpointStoreForProject(root.string()) == store);
	CHECK(HcExecTrace::breakpointStoreForProject("").empty());

	// A fresh project: no file, nothing read, nothing there — and setting the
	// store dropped whatever the previous project had in memory.
	HcExecTrace::setBreakpoint("Content/Old.hasset", 7, true);
	CHECK_FALSE(HcExecTrace::setBreakpointStore(store));
	CHECK(HcExecTrace::breakpointStore() == store);
	CHECK(HcExecTrace::breakpointCount() == 0);
	CHECK_FALSE(fs::exists(store));

	// Every change writes through — the directory is made on the way.
	HcExecTrace::setBreakpoint("Content/A.hasset", 3, true);
	CHECK(fs::exists(store));
	HcExecTrace::setBreakpoint("Content/A.hasset", 5, true);
	HcExecTrace::setBreakpoint("level:0123456789abcdef0123456789abcdef", 9, true);   // filed under the tab
	const std::string written = HcExecTrace::saveBreakpointsJson();
	CHECK(written.find("\"Content/A.hasset\"") != std::string::npos);
	CHECK(written.find(LevelScriptPanel::kTabPath) != std::string::npos);
	{
		std::ifstream in(store, std::ios::binary);
		const std::string onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(onDisk == written);
	}

	// Another editor session: memory wiped, the store read back.
	HcExecTrace::setBreakpointStore("");
	CHECK(HcExecTrace::breakpointCount() == 0);
	CHECK(HcExecTrace::setBreakpointStore(store));
	CHECK(HcExecTrace::breakpointCount() == 3);
	CHECK(HcExecTrace::breakpointsOf("Content/A.hasset") == std::vector<int>{ 3, 5 });
	CHECK(HcExecTrace::shouldBreakAt("level:fedcba9876543210fedcba9876543210", 9));

	// Removing the last one of a graph and clearing all both reach the file.
	HcExecTrace::setBreakpoint("Content/A.hasset", 3, false);
	HcExecTrace::setBreakpoint("Content/A.hasset", 5, false);
	CHECK(HcExecTrace::saveBreakpointsJson().find("Content/A.hasset") == std::string::npos);
	HcExecTrace::clearAllBreakpoints();
	CHECK(HcExecTrace::setBreakpointStore(store));
	CHECK(HcExecTrace::breakpointCount() == 0);

	// A torn file: nothing resurrected, nothing kept from before either.
	HcExecTrace::setBreakpoint("Content/B.hasset", 2, true);
	CHECK_FALSE(HcExecTrace::loadBreakpointsJson("{ \"breakpoints\": { \"Content/B.hasset\": [2"));
	CHECK(HcExecTrace::breakpointCount() == 0);
	// Junk inside a good document is skipped, not fatal: ids that are not
	// integers, a 0, an empty key, a value that is not an array.
	CHECK(HcExecTrace::loadBreakpointsJson(
		"{ \"breakpoints\": { \"Content/C.hasset\": [4, \"x\", 0, 6], \"\": [1], \"Content/D.hasset\": 5 } }"));
	CHECK(HcExecTrace::breakpointsOf("Content/C.hasset") == std::vector<int>{ 4, 6 });
	CHECK(HcExecTrace::breakpointCount() == 2);
}
