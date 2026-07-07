#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <string>

using namespace HorizonCode;

namespace
{
	// Event(name) exec → SetVariable(var, ConstBool true).
	// Pins: Event execOut 0; SetVariable execIn 0 / execOut 1 / Value dataIn 2;
	//       ConstBool dataOut 0.
	void eventSetsBool(Graph& g, const std::string& event, const std::string& var)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = 0;
		const int e = g.addNode(ev);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}

	// FunctionEntry(fn, access) exec → SetVariable(var, ConstBool true).
	void funcSetsBool(Graph& g, const std::string& fn, int access, const std::string& var)
	{
		Node fe; fe.type = NodeType::FunctionEntry; fe.s = fn; fe.access = access;
		const int f = g.addNode(fe);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(f, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	}

	Graph boolVarGraph(const std::string& name)
	{
		Graph g;
		Variable v; v.name = name; v.type = PinType::Bool; v.f[0] = 0.0f;
		g.variables.push_back(v);
		return g;
	}
}

TEST_CASE("Runtime seeds variable defaults and keeps instances isolated")
{
	Runtime rt;
	Graph g;
	Variable v; v.name = "x"; v.type = PinType::Int; v.f[0] = 3.0f; // default 3
	g.variables.push_back(v);

	const InstanceId a = rt.add(g);
	const InstanceId b = rt.add(g);
	REQUIRE(a != b);
	CHECK(rt.count() == 2);

	// Both seeded from the default.
	CHECK(rt.getVariable(a, "x").i == 3);
	CHECK(rt.getVariable(b, "x").i == 3);

	// Writing one instance's private store leaves the other untouched.
	rt.setVariable(a, "x", Value::ofInt(10));
	CHECK(rt.getVariable(a, "x").i == 10);
	CHECK(rt.getVariable(b, "x").i == 3);
}

TEST_CASE("Runtime fires an event on the right instance's graph")
{
	Runtime rt;
	Graph g = boolVarGraph("hit");
	eventSetsBool(g, "Ping", "hit");

	const InstanceId a = rt.add(g);
	const InstanceId b = rt.add(g);

	CHECK(rt.getVariable(a, "hit").b == false);
	rt.fireEvent(a, "Ping");
	CHECK(rt.getVariable(a, "hit").b == true);
	CHECK(rt.getVariable(b, "hit").b == false); // untouched

	// An event name with no matching node is a no-op.
	rt.fireEvent(b, "Nope");
	CHECK(rt.getVariable(b, "hit").b == false);
}

TEST_CASE("Runtime enforces the public/private access modifier on calls")
{
	Runtime rt;
	Graph g = boolVarGraph("pub");
	{ Variable v; v.name = "priv"; v.type = PinType::Bool; g.variables.push_back(v); }
	funcSetsBool(g, "Public",  /*access=*/0, "pub");
	funcSetsBool(g, "Private", /*access=*/1, "priv");

	const InstanceId id = rt.add(g);

	// Public function runs through the cross-class (requirePublic) path.
	CHECK(rt.callFunction(id, "Public", /*requirePublic=*/true));
	CHECK(rt.getVariable(id, "pub").b == true);

	// Private function is refused when a public entry is required...
	CHECK_FALSE(rt.callFunction(id, "Private", /*requirePublic=*/true));
	CHECK(rt.getVariable(id, "priv").b == false);
	// ...but reachable internally (requirePublic = false).
	CHECK(rt.callFunction(id, "Private", /*requirePublic=*/false));
	CHECK(rt.getVariable(id, "priv").b == true);

	// Missing function / instance → false.
	CHECK_FALSE(rt.callFunction(id, "Ghost", true));
	CHECK_FALSE(rt.callFunction(9999, "Public", true));
}

TEST_CASE("Runtime routes property side effects to the instance's host bindings")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.elem = 0;
	const int e = g.addNode(ev);
	Node cs; cs.type = NodeType::ConstString; cs.s = "hi";
	const int c = g.addNode(cs);
	Node sp; sp.type = NodeType::SetProperty; sp.elem = 7; sp.s = "text"; sp.propType = PinType::String;
	const int s = g.addNode(sp);
	REQUIRE(g.connect(e, 0, s, 0));
	REQUIRE(g.connect(c, 0, s, 2));

	InstanceId gotId = 0; int gotElem = -1; std::string gotProp, gotVal;
	HostBindings hb;
	hb.setProperty = [&](InstanceId id, int elem, const std::string& prop, const Value& v)
	{ gotId = id; gotElem = elem; gotProp = prop; gotVal = v.s; };

	Runtime rt;
	const InstanceId id = rt.add(std::move(g), hb);
	rt.fireEvent(id, "Go");

	CHECK(gotId == id);
	CHECK(gotElem == 7);
	CHECK(gotProp == "text");
	CHECK(gotVal == "hi");
}

TEST_CASE("Runtime remove drops an instance; unknown ids are safe")
{
	Runtime rt;
	Graph g = boolVarGraph("hit");
	const InstanceId id = rt.add(g);
	CHECK(rt.alive(id));

	rt.remove(id);
	CHECK_FALSE(rt.alive(id));
	CHECK(rt.count() == 0);
	CHECK(rt.variablesOf(id).empty());
	CHECK(rt.graphOf(id).nodes.empty());
	CHECK(rt.getVariable(id, "hit").b == false); // default-constructed, no throw

	rt.remove(9999);            // no-op, no crash
	rt.fireEvent(9999, "Ping"); // no-op
	rt.clear();
	CHECK(rt.count() == 0);
}
