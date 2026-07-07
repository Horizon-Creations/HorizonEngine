#include "doctest.h"
#include <HorizonScene/GameInstanceHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonCode/HorizonCode.h>
#include <string>

using namespace HorizonCode;

namespace
{
	// Event(event) → SetVariable(var, ConstString(value)).
	void eventSetsString(Graph& g, const std::string& event,
	                     const std::string& var, const std::string& value)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node cs; cs.type = NodeType::ConstString; cs.s = value;
		const int c = g.addNode(cs);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::String;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}
	void eventSetsBool(Graph& g, const std::string& event, const std::string& var)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}
	// Event(event) → BindEvent(Target = GetGameInstance, bindName).
	void eventBindsGameInstance(Graph& g, const std::string& event, const std::string& bindName)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node gi; gi.type = NodeType::GetGameInstance;
		const int i = g.addNode(gi);
		Node be; be.type = NodeType::BindEvent; be.s = bindName;
		const int b = g.addNode(be);
		REQUIRE(g.connect(e, 0, b, 0));
		REQUIRE(g.connect(i, 0, b, 2));
	}

	std::string stringVarGraph(const char* name)
	{
		Graph g;
		Variable v; v.name = name; v.type = PinType::String; v.s = "none";
		g.variables.push_back(v);
		eventSetsString(g, "OnInit",     name, "init");
		eventSetsString(g, "OnShutdown", name, "shutdown");
		return toJson(g);
	}
}

TEST_CASE("GameInstance fires lifecycle events and a scene can reference it")
{
	GameInstanceHost host;
	host.setGraph(stringVarGraph("phase"));
	const InstanceId gi = host.id();
	REQUIRE(gi != 0);

	// OnInit runs once, before any world exists.
	host.fireInit();
	CHECK(host.isRunning());
	CHECK(host.runtime().getVariable(gi, "phase").s == "init");

	// A scene joins the app runtime and its level script binds to the
	// GameInstance's focus event via Get Game Instance + Bind Event.
	HorizonWorld w;
	w.setScriptRuntime(&host.runtime());
	Graph level;
	{ Variable v; v.name = "heard"; v.type = PinType::Bool; level.variables.push_back(v); }
	eventBindsGameInstance(level, "OnLevelLoaded", "OnWindowFocusChanged");
	eventSetsBool(level, "OnWindowFocusChanged", "heard");
	w.setLevelScriptJson(toJson(level));
	w.fireLevelLoaded(); // subscribes to the GameInstance

	CHECK(w.levelVariables().at("heard").b == false);
	host.setWindowFocus(false); // GameInstance broadcasts → the level reacts
	CHECK(w.levelVariables().at("heard").b == true);

	// Switching scenes (clear) drops the level but the GameInstance persists.
	w.clear();
	CHECK(host.runtime().alive(gi));
	CHECK(host.isRunning());
	CHECK(host.id() == gi);

	host.fireShutdown();
	CHECK_FALSE(host.isRunning());
	// fireShutdown runs OnShutdown, then resets the GameInstance to a clean state
	// (fresh defaults) so the next play session starts blank.
	CHECK(host.runtime().getVariable(gi, "phase").s == "none");
}

TEST_CASE("Window focus fires only on a real change while running")
{
	GameInstanceHost host;
	Graph g;
	{ Variable v; v.name = "focus"; v.type = PinType::Bool; v.f[0] = 1.0f; g.variables.push_back(v); }
	// OnWindowFocusChanged → focus = <arg>. Event carries a Bool arg.
	Node ev; ev.type = NodeType::Event; ev.s = "OnWindowFocusChanged";
	ev.hasArg = true; ev.propType = PinType::Bool;
	const int e = g.addNode(ev);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "focus"; sv.propType = PinType::Bool;
	const int s = g.addNode(sv);
	REQUIRE(g.connect(e, 0, s, 0));       // exec
	REQUIRE(g.connect(e, 1, s, 2));       // Event arg (dataOut 1) → SetVariable Value
	host.setGraph(toJson(g));

	// Not running yet → focus changes are ignored.
	host.setWindowFocus(false);
	CHECK(host.runtime().getVariable(host.id(), "focus").b == true);

	host.fireInit();
	host.setWindowFocus(false);
	CHECK(host.runtime().getVariable(host.id(), "focus").b == false);
	// No change → no fire (still false even though we could re-write true).
	host.runtime().setVariable(host.id(), "focus", Value::ofBool(true));
	host.setWindowFocus(false); // same as current focus state → no-op
	CHECK(host.runtime().getVariable(host.id(), "focus").b == true);
}
