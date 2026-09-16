#include "doctest.h"
#include "../src/HE_Editor/HcExecTrace.h"
#include "../src/HE_Editor/LevelScriptPanel.h"
#include "../src/HE_Editor/GameInstancePanel.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>

#include <string>

using namespace HorizonCode;

// ── The editor's keeper of "which node just ran" and "show me that node" ─────
// The interpreter side (Runtime::setExecListener, currentExecSite) is covered
// in test_horizoncode_runtime.cpp; this is the editor module on top of it.

namespace
{
	struct Reset
	{
		Reset()  { HcExecTrace::clearHits(); HcExecTrace::cancelReveal(); }
		~Reset() { HcExecTrace::clearHits(); HcExecTrace::cancelReveal(); }
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
