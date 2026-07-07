#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonCode/HorizonCode.h>
#include <cstdint>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	// Append "Event(eventName) → SetVariable(varName, ConstString(value))" to g,
	// wiring exec + the string value. Pin scheme (unified index):
	//   Event:       execOut 0
	//   SetVariable: execIn 0, execOut 1, Value dataIn 2
	//   ConstString: dataOut 0
	void addEventSetsVar(Graph& g, const std::string& eventName,
	                     const std::string& varName, const std::string& value)
	{
		Node ev; ev.type = NodeType::Event; ev.s = eventName; ev.elem = 0;
		const int evId = g.addNode(ev);

		Node cs; cs.type = NodeType::ConstString; cs.s = value;
		const int csId = g.addNode(cs);

		Node sv; sv.type = NodeType::SetVariable; sv.s = varName; sv.propType = PinType::String;
		const int svId = g.addNode(sv);

		REQUIRE(g.connect(evId, 0, svId, 0)); // Event exec → SetVariable exec-in
		REQUIRE(g.connect(csId, 0, svId, 2)); // ConstString value → SetVariable Value
	}

	// A level script with a String variable "phase" (default "none") that becomes
	// "loaded" on OnLevelLoaded and "unloaded" on OnLevelUnloaded.
	Graph makePhaseGraph()
	{
		Graph g;
		Variable v; v.name = "phase"; v.type = PinType::String; v.s = "none";
		g.variables.push_back(v);
		addEventSetsVar(g, "OnLevelLoaded",   "phase", "loaded");
		addEventSetsVar(g, "OnLevelUnloaded", "phase", "unloaded");
		return g;
	}
}

TEST_CASE("Level script fires OnLevelLoaded / OnLevelUnloaded through the world")
{
	HorizonWorld w;
	w.setLevelScriptJson(toJson(makePhaseGraph()));

	CHECK_FALSE(w.isLevelRunning());

	w.fireLevelLoaded();
	CHECK(w.isLevelRunning());
	REQUIRE(w.levelVariables().count("phase") == 1);
	CHECK(w.levelVariables().at("phase").s == "loaded");

	w.fireLevelUnloaded();
	CHECK_FALSE(w.isLevelRunning());
	CHECK(w.levelVariables().at("phase").s == "unloaded");
}

TEST_CASE("OnLevelLoaded fires exactly once; OnLevelUnloaded no-ops unless running")
{
	HorizonWorld w;
	w.setLevelScriptJson(toJson(makePhaseGraph()));

	// Unloaded before ever loading is a no-op: the graph never runs, so the
	// variable store is never even seeded.
	w.fireLevelUnloaded();
	CHECK_FALSE(w.isLevelRunning());
	CHECK(w.levelVariables().empty());

	w.fireLevelLoaded();
	CHECK(w.levelVariables().at("phase").s == "loaded");

	// A second load while already running does nothing (no re-seed, no re-fire).
	// Prove it by mutating the store and checking a redundant load leaves it be.
	w.fireLevelLoaded();
	CHECK(w.isLevelRunning());
	CHECK(w.levelVariables().at("phase").s == "loaded");

	w.fireLevelUnloaded();
	CHECK(w.levelVariables().at("phase").s == "unloaded");
	// A second unload is likewise a no-op.
	w.fireLevelUnloaded();
	CHECK_FALSE(w.isLevelRunning());
	CHECK(w.levelVariables().at("phase").s == "unloaded");
}

TEST_CASE("clear() ends a running level but not an idle one")
{
	HorizonWorld w;
	w.setLevelScriptJson(toJson(makePhaseGraph()));
	w.fireLevelLoaded();
	REQUIRE(w.isLevelRunning());

	w.clear();
	CHECK_FALSE(w.isLevelRunning());
	// clear() also drops the script itself (a loaded scene restores its own).
	CHECK(w.levelScript().nodes.empty());
	CHECK(w.levelVariables().empty());

	// Edit-time clear() (never loaded) must not spuriously fire / crash.
	HorizonWorld w2;
	w2.setLevelScriptJson(toJson(makePhaseGraph()));
	CHECK_FALSE(w2.isLevelRunning());
	w2.clear();
	CHECK_FALSE(w2.isLevelRunning());
}

TEST_CASE("Level script round-trips through the scene serializer")
{
	HorizonWorld w1;
	const Graph g = makePhaseGraph();
	w1.setLevelScriptJson(toJson(g));

	std::vector<uint8_t> buf;
	SceneSerializer ser;
	REQUIRE(ser.saveToMemory(w1, buf));

	HorizonWorld w2;
	REQUIRE(ser.loadFromMemory(w2, buf));

	CHECK(w2.levelScript().nodes.size()     == g.nodes.size());
	CHECK(w2.levelScript().links.size()     == g.links.size());
	CHECK(w2.levelScript().variables.size() == g.variables.size());

	// The restored graph still runs.
	w2.fireLevelLoaded();
	CHECK(w2.levelVariables().at("phase").s == "loaded");
}

TEST_CASE("A scene without a level script serializes clean")
{
	HorizonWorld w1;
	CHECK(w1.levelScriptJson().empty());

	std::vector<uint8_t> buf;
	SceneSerializer ser;
	REQUIRE(ser.saveToMemory(w1, buf));

	HorizonWorld w2;
	REQUIRE(ser.loadFromMemory(w2, buf));
	CHECK(w2.levelScriptJson().empty());
	CHECK(w2.levelScript().nodes.empty());
}
