#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Diagnostics/Logger.h>
#include <Diagnostics/Log.h>   // addSink — the exec-trace tests read the site from a sink
#include <algorithm>   // sort — libc++ pulls it in transitively, MSVC does not
#include <set>
#include <string>
#include <utility>
#include <vector>

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

	// Event(event) exec → EmitEvent(emit).  EmitEvent: execIn 0 / execOut 1.
	void eventEmits(Graph& g, const std::string& event, const std::string& emit)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = 0;
		const int e = g.addNode(ev);
		Node em; em.type = NodeType::EmitEvent; em.s = emit; em.hasArg = false;
		const int m = g.addNode(em);
		REQUIRE(g.connect(e, 0, m, 0));
	}

	// Event(event) exec → BindEvent(Target = GetGameInstance, bindName).
	// BindEvent: execIn 0 / execOut 1 / Target(Ref) dataIn 2; GetGameInstance dataOut 0.
	void eventBindsGameInstance(Graph& g, const std::string& event, const std::string& bindName)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = 0;
		const int e = g.addNode(ev);
		Node gi; gi.type = NodeType::GetGameInstance;
		const int i = g.addNode(gi);
		Node be; be.type = NodeType::BindEvent; be.s = bindName;
		const int b = g.addNode(be);
		REQUIRE(g.connect(e, 0, b, 0)); // exec
		REQUIRE(g.connect(i, 0, b, 2)); // Ref → Target
	}

	// Event(event) exec → CallExternal(Target = GetSelf, fn).
	void eventCallsSelf(Graph& g, const std::string& event, const std::string& fn)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = 0;
		const int e = g.addNode(ev);
		Node gs; gs.type = NodeType::GetSelf;
		const int s = g.addNode(gs);
		Node ce; ce.type = NodeType::CallExternal; ce.s = fn;
		const int c = g.addNode(ce);
		REQUIRE(g.connect(e, 0, c, 0)); // exec
		REQUIRE(g.connect(s, 0, c, 2)); // Ref → Target
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

TEST_CASE("Runtime dispatches an event to bound listeners (reference delegation)")
{
	Runtime rt;
	Graph owner;                                  // owner has no nodes of its own
	Graph listener = boolVarGraph("heard");
	eventSetsBool(listener, "Ping", "heard");     // handler: Event Ping → heard = true

	const InstanceId O = rt.add(owner);
	const InstanceId L = rt.add(listener);
	rt.bindEvent(O, "Ping", L);

	CHECK(rt.getVariable(L, "heard").b == false);
	rt.fireEvent(O, "Ping");                       // owner fires → listener's Ping fires too
	CHECK(rt.getVariable(L, "heard").b == true);

	// emitEvent (dispatcher-only) reaches listeners as well.
	rt.setVariable(L, "heard", Value::ofBool(false));
	rt.emitEvent(O, "Ping");
	CHECK(rt.getVariable(L, "heard").b == true);

	// Removing the listener drops the binding — later fires are harmless no-ops.
	rt.remove(L);
	rt.fireEvent(O, "Ping");
	CHECK(rt.count() == 1);
}

TEST_CASE("EmitEvent node broadcasts through the runtime to listeners")
{
	Runtime rt;
	Graph owner;    eventEmits(owner, "Go", "Signal");        // Go → EmitEvent Signal
	Graph listener = boolVarGraph("heard");
	eventSetsBool(listener, "Signal", "heard");

	const InstanceId O = rt.add(owner);
	const InstanceId L = rt.add(listener);
	rt.bindEvent(O, "Signal", L);

	rt.fireEvent(O, "Go");
	CHECK(rt.getVariable(L, "heard").b == true);
}

TEST_CASE("BindEvent + GetGameInstance let a script subscribe to the GameInstance")
{
	Runtime rt;
	Graph gi;                                       // the GameInstance (fires Boom)
	const InstanceId G = rt.setGameInstance(std::move(gi));
	CHECK(rt.gameInstance() == G);

	Graph listener = boolVarGraph("heard");
	eventSetsBool(listener, "Boom", "heard");        // handler
	eventBindsGameInstance(listener, "Setup", "Boom"); // Setup → bind to GameInstance.Boom
	const InstanceId L = rt.add(listener);

	rt.fireEvent(L, "Setup");   // subscribe
	rt.fireEvent(G, "Boom");    // GameInstance broadcasts → listener reacts
	CHECK(rt.getVariable(L, "heard").b == true);
}

TEST_CASE("retainOnlyReachableFrom keeps GameInstance-held objects, drops the rest")
{
	Runtime rt;
	Graph gi;
	{ Variable v; v.name = "kept"; v.type = PinType::Ref; gi.variables.push_back(v); }
	const InstanceId G = rt.setGameInstance(std::move(gi));

	// Three plain objects (as Create Object would add them).
	const InstanceId A = rt.add(Graph{});
	const InstanceId B = rt.add(Graph{});
	const InstanceId C = rt.add(Graph{});

	rt.setVariable(G, "kept",  Value::ofRef(A)); // GameInstance holds A
	rt.setVariable(A, "child", Value::ofRef(C)); // A holds C (transitive)
	// nobody holds B

	rt.retainOnlyReachableFrom(G);

	CHECK(rt.alive(G));       // root
	CHECK(rt.alive(A));       // held by the GameInstance
	CHECK(rt.alive(C));       // reachable through A
	CHECK_FALSE(rt.alive(B)); // unheld → swept
}

namespace
{
	// Object whose "Destruct" event writes true to a public "died" bool on the
	// instance referenced by its "logger" Ref variable (via SetExternal). Lets a
	// destroyed object leave an observable trace on a surviving one.
	Graph destructWritesLoggerGraph()
	{
		Graph g;
		{ Variable v; v.name = "logger"; v.type = PinType::Ref; g.variables.push_back(v); }
		Node ev; ev.type = NodeType::Event; ev.s = "Destruct"; const int e = g.addNode(ev);
		Node gt; gt.type = NodeType::GetVariable; gt.s = "logger"; gt.propType = PinType::Ref; const int t = g.addNode(gt);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f; const int c = g.addNode(cb);
		Node se; se.type = NodeType::SetExternal; se.s = "died"; se.propType = PinType::Bool; const int s = g.addNode(se);
		REQUIRE(g.connect(e, 0, s, 0)); // exec
		REQUIRE(g.connect(t, 0, s, 2)); // logger → Target
		REQUIRE(g.connect(c, 0, s, 3)); // true → Value
		return g;
	}

	Graph publicBoolGraph(const std::string& name)
	{
		Graph g;
		Variable v; v.name = name; v.type = PinType::Bool; v.access = 0;
		g.variables.push_back(v);
		return g;
	}
}

TEST_CASE("destroy() fires the instance's Destruct before removing it")
{
	Runtime rt;
	const InstanceId L = rt.add(publicBoolGraph("died"));   // surviving logger
	const InstanceId A = rt.add(destructWritesLoggerGraph());
	rt.setVariable(A, "logger", Value::ofRef(L));

	CHECK(rt.getVariable(L, "died").b == false);
	rt.destroy(A);
	CHECK_FALSE(rt.alive(A));                     // gone
	CHECK(rt.getVariable(L, "died").b == true);   // …but its Destruct ran first
}

TEST_CASE("destroy() is re-entrancy-safe when Destruct destroys the same instance")
{
	Runtime rt;
	const InstanceId L = rt.add(publicBoolGraph("died"));

	// A's Destruct writes the logger AND calls Destroy Object on Get Self — the
	// self-destroy must not recurse into Destruct forever.
	Graph gA = destructWritesLoggerGraph();
	{
		Node ev; ev.type = NodeType::Event; ev.s = "Destruct"; const int e = gA.addNode(ev);
		Node gs; gs.type = NodeType::GetSelf; const int s = gA.addNode(gs);
		Node du; du.type = NodeType::DestroyObject; const int d = gA.addNode(du);
		REQUIRE(gA.connect(e, 0, d, 0)); // exec
		REQUIRE(gA.connect(s, 0, d, 2)); // Self ref → DestroyObject.Target
	}
	const InstanceId A = rt.add(std::move(gA));
	rt.setVariable(A, "logger", Value::ofRef(L));
	// Route Destroy Object back through the runtime (as the app binds it).
	Runtime::Services svc;
	svc.destroyObject = [&](uint32_t ref){ rt.destroy(ref); };
	rt.setServices(svc);

	rt.destroy(A);                                // must terminate
	CHECK_FALSE(rt.alive(A));
	CHECK(rt.getVariable(L, "died").b == true);   // its Destruct still ran once
}

TEST_CASE("retainOnlyReachableFrom fires Destruct on the objects it sweeps")
{
	Runtime rt;
	Graph gi;
	{ Variable v; v.name = "logger"; v.type = PinType::Ref; gi.variables.push_back(v); }
	const InstanceId G = rt.setGameInstance(std::move(gi));

	const InstanceId L = rt.add(publicBoolGraph("died")); // kept by the GameInstance
	rt.setVariable(G, "logger", Value::ofRef(L));

	const InstanceId B = rt.add(destructWritesLoggerGraph()); // unheld → swept
	rt.setVariable(B, "logger", Value::ofRef(L));

	rt.retainOnlyReachableFrom(G);
	CHECK(rt.alive(G));
	CHECK(rt.alive(L));                           // held by the GameInstance
	CHECK_FALSE(rt.alive(B));                     // unheld → swept…
	CHECK(rt.getVariable(L, "died").b == true);   // …and its Destruct ran during the sweep
}

TEST_CASE("Get/Set External read + write a public variable but not a private one")
{
	Runtime rt;

	// Instance A exposes a public "hp" (Int=5) and a private "secret" (Bool).
	Graph gA;
	{ Variable v; v.name = "hp";     v.type = PinType::Int;  v.f[0] = 5; v.access = 0; gA.variables.push_back(v); }
	{ Variable v; v.name = "secret"; v.type = PinType::Bool;             v.access = 1; gA.variables.push_back(v); }
	const InstanceId A = rt.add(std::move(gA));

	// Instance B holds a ref to A and reads/writes it through Get/Set External.
	Graph gB;
	{ Variable v; v.name = "target";   v.type = PinType::Ref; gB.variables.push_back(v); }
	{ Variable v; v.name = "readback"; v.type = PinType::Int; gB.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = gB.addNode(ev);
	Node gt; gt.type = NodeType::GetVariable; gt.s = "target"; gt.propType = PinType::Ref; const int t = gB.addNode(gt);
	Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 99; const int c = gB.addNode(ci);
	Node se; se.type = NodeType::SetExternal; se.s = "hp"; se.propType = PinType::Int; const int s = gB.addNode(se);
	Node ge; ge.type = NodeType::GetExternal; ge.s = "hp"; ge.propType = PinType::Int; const int x = gB.addNode(ge);
	Node rv; rv.type = NodeType::SetVariable; rv.s = "readback"; rv.propType = PinType::Int; const int r = gB.addNode(rv);
	// Go → SetExternal(target,"hp",99) → SetVariable(readback, GetExternal(target,"hp")).
	REQUIRE(gB.connect(e, 0, s, 0)); // exec
	REQUIRE(gB.connect(t, 0, s, 2)); // target → SetExternal.Target
	REQUIRE(gB.connect(c, 0, s, 3)); // 99 → SetExternal.Value
	REQUIRE(gB.connect(s, 1, r, 0)); // exec
	REQUIRE(gB.connect(t, 0, x, 0)); // target → GetExternal.Target
	REQUIRE(gB.connect(x, 1, r, 2)); // GetExternal.Value → readback
	// TrySecret → SetExternal(target,"secret",true) — must be blocked (private).
	Node ev2; ev2.type = NodeType::Event; ev2.s = "TrySecret"; const int e2 = gB.addNode(ev2);
	Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f; const int cbId = gB.addNode(cb);
	Node ss; ss.type = NodeType::SetExternal; ss.s = "secret"; ss.propType = PinType::Bool; const int s2 = gB.addNode(ss);
	REQUIRE(gB.connect(e2,   0, s2, 0));
	REQUIRE(gB.connect(t,    0, s2, 2));
	REQUIRE(gB.connect(cbId, 0, s2, 3));

	const InstanceId B = rt.add(std::move(gB));
	rt.setVariable(B, "target", Value::ofRef(A));

	rt.fireEvent(B, "Go");
	CHECK(rt.getVariable(A, "hp").i == 99);        // wrote A's public var
	CHECK(rt.getVariable(B, "readback").i == 99);  // read it back through the ref

	rt.fireEvent(B, "TrySecret");
	CHECK(rt.getVariable(A, "secret").b == false); // private var stayed untouched
}

TEST_CASE("Create/Destroy Object instantiate a class, run Construct, cache the ref")
{
	Runtime rt;

	// The class this test spawns: Construct → built = true.
	Graph classGraph = boolVarGraph("built");
	eventSetsBool(classGraph, "Construct", "built");

	int createCount = 0; InstanceId createdRef = 0, destroyedRef = 0; bool builtAfterConstruct = false;
	Runtime::Services svc;
	svc.createObject = [&](const std::string& /*path*/, const float*, const float*) -> uint32_t {
		++createCount;
		const InstanceId id = rt.add(classGraph);
		rt.fireEvent(id, "Construct");
		builtAfterConstruct = rt.getVariable(id, "built").b;
		createdRef = id;
		return id;
	};
	svc.destroyObject = [&](uint32_t ref){ destroyedRef = ref; rt.remove(ref); };
	rt.setServices(svc);

	// Caller: Go → CreateObject → DestroyObject(<created ref>).
	// CreateObject: execIn 0 / execOut 1 / Location dataIn 2 / Rotation dataIn 3 / Object dataOut 4.
	// DestroyObject: execIn 0 / execOut 1 / Object dataIn 2.
	Graph caller;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = caller.addNode(ev);
	Node co; co.type = NodeType::CreateObject; co.s = "MyClass"; const int c = caller.addNode(co);
	Node de; de.type = NodeType::DestroyObject; const int d = caller.addNode(de);
	REQUIRE(caller.connect(e, 0, c, 0)); // exec
	REQUIRE(caller.connect(c, 1, d, 0)); // exec
	REQUIRE(caller.connect(c, 4, d, 2)); // ref out → ref in

	const InstanceId callerId = rt.add(std::move(caller));
	rt.fireEvent(callerId, "Go");

	CHECK(createCount == 1);              // created once — the ref is cached, not re-run
	CHECK(builtAfterConstruct == true);   // the class's Construct ran
	CHECK(createdRef != 0);
	CHECK(destroyedRef == createdRef);    // the cached ref flowed into Destroy Object
	CHECK_FALSE(rt.alive(createdRef));    // and the instance is gone
}

namespace
{
	// What the host saw for one spawn. Both pointers are recorded as
	// "was it null" plus the values, because null is the whole contract here.
	struct SpawnCapture
	{
		bool      posNull = true, rotNull = true;
		glm::vec3 pos{ -1.0f }, rot{ -1.0f };
		int       calls = 0;
	};

	// Binds a createObject that records what it was handed. Returns a graph with
	// Event "Go" → CreateObject; the caller wires the placement pins (or not).
	// CreateObject pins: execIn 0 / execOut 1 / Location 2 / Rotation 3 / Object 4.
	Graph spawnGraph(int& createNodeOut)
	{
		Graph g;
		Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
		Node co; co.type = NodeType::CreateObject; co.s = "SpawnMe";
		createNodeOut = g.addNode(co);
		REQUIRE(g.connect(e, 0, createNodeOut, 0));
		return g;
	}

	// A Make Vector 3 whose X/Y/Z ride in as pin defaults — a real wired Vec3
	// source without three extra literal nodes.
	int vec3Node(Graph& g, float x, float y, float z)
	{
		Node mv; mv.type = NodeType::MakeVector3;
		mv.pinDefaults[0] = Value::ofFloat(x);
		mv.pinDefaults[1] = Value::ofFloat(y);
		mv.pinDefaults[2] = Value::ofFloat(z);
		return g.addNode(mv);
	}
}

TEST_CASE("Create Object with unwired placement pins spawns where the class authored it")
{
	Runtime rt;
	SpawnCapture cap;
	Runtime::Services svc;
	svc.createObject = [&](const std::string&, const float* p, const float* r) -> uint32_t {
		++cap.calls;
		cap.posNull = (p == nullptr);
		cap.rotNull = (r == nullptr);
		return 7u;
	};
	rt.setServices(svc);

	int create = -1;
	Graph g = spawnGraph(create);

	// The anchor: a graph authored before these pins existed has no links on
	// them, and an inline pin DEFAULT must not count as "the author placed it".
	// The node is asked by WIRE, so this (0,0,0)-ish default is deliberately
	// ignored — otherwise every old spawn would silently move to the origin.
	Node* co = g.findNode(create);
	REQUIRE(co != nullptr);
	co->pinDefaults[0] = Value::ofVec3({ 9.0f, 9.0f, 9.0f });

	rt.fireEvent(rt.add(std::move(g)), "Go");

	CHECK(cap.calls == 1);
	CHECK(cap.posNull);   // nullptr, not (0,0,0) and not the pin default
	CHECK(cap.rotNull);
}

TEST_CASE("Create Object with wired placement pins hands the host the values")
{
	SUBCASE("both pins wired")
	{
		Runtime rt;
		SpawnCapture cap;
		Runtime::Services svc;
		svc.createObject = [&](const std::string&, const float* p, const float* r) -> uint32_t {
			++cap.calls;
			cap.posNull = (p == nullptr);
			cap.rotNull = (r == nullptr);
			if (p) cap.pos = { p[0], p[1], p[2] };
			if (r) cap.rot = { r[0], r[1], r[2] };
			return 7u;
		};
		rt.setServices(svc);

		int create = -1;
		Graph g = spawnGraph(create);
		const int loc = vec3Node(g, 1.0f, 2.0f, 3.0f);
		const int rot = vec3Node(g, 0.0f, 90.0f, 0.0f);
		REQUIRE(g.connect(loc, 3, create, 2)); // MakeVector3 dataOut 3 → Location
		REQUIRE(g.connect(rot, 3, create, 3)); // → Rotation

		rt.fireEvent(rt.add(std::move(g)), "Go");

		CHECK(cap.calls == 1);
		CHECK_FALSE(cap.posNull);
		CHECK_FALSE(cap.rotNull);
		CHECK(cap.pos.x == doctest::Approx(1.0f));
		CHECK(cap.pos.y == doctest::Approx(2.0f));
		CHECK(cap.pos.z == doctest::Approx(3.0f));
		CHECK(cap.rot.y == doctest::Approx(90.0f)); // euler degrees, straight through
	}

	SUBCASE("one pin wired — the other stays authored")
	{
		Runtime rt;
		SpawnCapture cap;
		Runtime::Services svc;
		svc.createObject = [&](const std::string&, const float* p, const float* r) -> uint32_t {
			++cap.calls;
			cap.posNull = (p == nullptr);
			cap.rotNull = (r == nullptr);
			if (p) cap.pos = { p[0], p[1], p[2] };
			return 7u;
		};
		rt.setServices(svc);

		int create = -1;
		Graph g = spawnGraph(create);
		const int loc = vec3Node(g, 5.0f, 0.0f, -5.0f);
		REQUIRE(g.connect(loc, 3, create, 2)); // Location only

		rt.fireEvent(rt.add(std::move(g)), "Go");

		CHECK(cap.calls == 1);
		CHECK_FALSE(cap.posNull);
		CHECK(cap.pos.x == doctest::Approx(5.0f));
		CHECK(cap.pos.z == doctest::Approx(-5.0f));
		CHECK(cap.rotNull);   // Rotation untouched → keep the authored rotation
	}
}

TEST_CASE("A graph saved before the placement pins keeps its Object wire")
{
	// Adding two data inputs moved Create Object's Object output from absolute
	// pin 2 to pin 4, and links are stored as absolute pin numbers with no
	// version stamp. This is the on-disk shape of such a graph: the link
	// ORIGINATES at pin 2, which only an old file can do (pin 2 is now the
	// Location input, and links never originate at an input).
	const std::string oldJson = R"({
		"nodes": [
			{ "id": 1, "type": "Event",          "s": "Go" },
			{ "id": 2, "type": "Create Object",  "s": "MyClass" },
			{ "id": 3, "type": "Destroy Object" }
		],
		"links": [ [1, 0, 2, 0], [2, 1, 3, 0], [2, 2, 3, 2] ],
		"variables": []
	})";

	Graph g;
	REQUIRE(fromJson(oldJson, g));

	// The ref wire was re-based onto the new output pin; the exec wires, which
	// sit below the data pins, are untouched.
	const Link* ref = nullptr;
	for (const Link& l : g.links)
		if (l.srcNode == 2 && l.dstNode == 3 && l.dstPin == 2) ref = &l;
	REQUIRE(ref != nullptr);
	CHECK(ref->srcPin == 4);

	// And it still behaves: the created ref reaches Destroy Object, with the
	// spawn left exactly where the class authored it.
	Runtime rt;
	bool posNull = false, rotNull = false;
	uint32_t destroyed = 0;
	Runtime::Services svc;
	svc.createObject = [&](const std::string&, const float* p, const float* r) -> uint32_t {
		posNull = (p == nullptr); rotNull = (r == nullptr); return 55u;
	};
	svc.destroyObject = [&](uint32_t id){ destroyed = id; };
	rt.setServices(svc);

	rt.fireEvent(rt.add(std::move(g)), "Go");

	CHECK(destroyed == 55u);  // the output still flows down the migrated wire
	CHECK(posNull);
	CHECK(rotNull);
}

TEST_CASE("Widget nodes call the runtime services and cache CreateWidget's id")
{
	Runtime rt;
	int createCount = 0, shownId = -1;
	std::string createdPath;
	Runtime::Services svc;
	svc.createWidget = [&](const std::string& p){ ++createCount; createdPath = p; return 42; };
	svc.showWidget   = [&](int id){ shownId = id; };
	rt.setServices(svc);

	// Event Go → CreateWidget("hud.ui") → ShowSelf(<created id>).
	// CreateWidget: execIn 0 / execOut 1 / Widget dataOut 2.
	// ShowSelf:   execIn 0 / execOut 1 / Widget dataIn 2.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node cw; cw.type = NodeType::CreateWidget; cw.s = "hud.ui"; const int c = g.addNode(cw);
	Node sw; sw.type = NodeType::ShowWidget; const int s = g.addNode(sw);
	REQUIRE(g.connect(e, 0, c, 0)); // Event exec → CreateWidget exec-in
	REQUIRE(g.connect(c, 1, s, 0)); // CreateWidget exec-out → ShowSelf exec-in
	REQUIRE(g.connect(c, 2, s, 2)); // CreateWidget id → ShowSelf Widget

	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Go");

	CHECK(createCount == 1);        // created exactly once — the id is cached, not re-run
	CHECK(createdPath == "hud.ui");
	CHECK(shownId == 42);           // the cached id flowed into Show Widget
}

TEST_CASE("CallExternal via GetSelf runs a public function but not a private one")
{
	Runtime rt;
	Graph g = boolVarGraph("done");
	{ Variable v; v.name = "secret"; v.type = PinType::Bool; g.variables.push_back(v); }
	funcSetsBool(g, "Pub",  /*access=*/0, "done");
	funcSetsBool(g, "Priv", /*access=*/1, "secret");
	eventCallsSelf(g, "GoPub",  "Pub");
	eventCallsSelf(g, "GoPriv", "Priv");

	const InstanceId id = rt.add(std::move(g));

	rt.fireEvent(id, "GoPub");
	CHECK(rt.getVariable(id, "done").b == true);

	// CallExternal always requires public → the private function stays unreached.
	rt.fireEvent(id, "GoPriv");
	CHECK(rt.getVariable(id, "secret").b == false);
}

TEST_CASE("Function with typed input + output passes an argument and returns a value")
{
	Runtime rt;
	Graph g;
	{ Variable v; v.name = "out"; v.type = PinType::Int; g.variables.push_back(v); }

	// Function Double(x:Int) -> (y:Int) whose body just returns its input.
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Passthrough";
	fe.params = { { "x", PinType::Int } }; fe.results = { { "y", PinType::Int } };
	const int feId = g.addNode(fe);
	Node fr; fr.type = NodeType::FunctionReturn; fr.s = "Passthrough"; const int frId = g.addNode(fr);

	// Go → Passthrough(21) → out = returned y.
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 21; const int c = g.addNode(ci);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "Passthrough"; const int fcId = g.addNode(fc);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Int; const int s = g.addNode(sv);

	syncFunctionSignatures(g); // call + return mirror the entry's interface

	// Body: entry.exec → return.exec ; entry.x → return.y
	REQUIRE(g.connect(feId, 0, frId, 0));
	REQUIRE(g.connect(feId, 1, frId, 1));
	// Main: Go → call ; 21 → call.x ; call.exec → SetVariable ; call.y → SetVariable.Value
	REQUIRE(g.connect(e,    0, fcId, 0));
	REQUIRE(g.connect(c,    0, fcId, 2));
	REQUIRE(g.connect(fcId, 1, s,    0));
	REQUIRE(g.connect(fcId, 3, s,    2));

	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Go");
	CHECK(rt.getVariable(id, "out").i == 21); // argument reached the body and came back out
}

TEST_CASE("syncFunctionSignatures mirrors a function's interface onto calls + survives JSON")
{
	Graph g;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Calc";
	fe.params  = { { "a", PinType::Float }, { "b", PinType::Int } };
	fe.results = { { "r", PinType::Bool } };
	g.addNode(fe);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "Calc"; const int fcId = g.addNode(fc);

	syncFunctionSignatures(g);
	const Node* call = g.findNode(fcId);
	REQUIRE(call);
	CHECK(call->params.size()  == 2);
	CHECK(call->results.size() == 1);
	CHECK(call->params[1].type == PinType::Int);

	// Round-trip through JSON keeps the interface (re-synced on load).
	Graph g2;
	REQUIRE(fromJson(toJson(g), g2));
	const Node* call2 = nullptr;
	for (const auto& n : g2.nodes) if (n.type == NodeType::FunctionCall) call2 = &n;
	REQUIRE(call2);
	CHECK(call2->params.size()  == 2);
	CHECK(call2->results.size() == 1);
	CHECK(call2->results[0].type == PinType::Bool);
}

TEST_CASE("CallExternal passes an argument to a public function and reads its return")
{
	Runtime rt;
	// A exposes a public Echo(x:Int) -> (y:Int) that returns its input.
	Graph gA;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Echo"; fe.access = 0;
	fe.params = { { "x", PinType::Int } }; fe.results = { { "y", PinType::Int } };
	const int feA = gA.addNode(fe);
	Node fr; fr.type = NodeType::FunctionReturn; fr.s = "Echo"; const int frA = gA.addNode(fr);
	syncFunctionSignatures(gA);
	REQUIRE(gA.connect(feA, 0, frA, 0)); // exec
	REQUIRE(gA.connect(feA, 1, frA, 1)); // x -> y
	const InstanceId A = rt.add(std::move(gA));

	// B: out = Echo(42) called on its "target" ref (→ A).
	Graph gB;
	{ Variable v; v.name = "target"; v.type = PinType::Ref; gB.variables.push_back(v); }
	{ Variable v; v.name = "out";    v.type = PinType::Int; gB.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = gB.addNode(ev);
	Node gt; gt.type = NodeType::GetVariable; gt.s = "target"; gt.propType = PinType::Ref; const int t = gB.addNode(gt);
	Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 42; const int c = gB.addNode(ci);
	Node ce; ce.type = NodeType::CallExternal; ce.s = "Echo";
	ce.params = { { "x", PinType::Int } }; ce.results = { { "y", PinType::Int } }; // typed signature
	const int ceId = gB.addNode(ce);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Int; const int s = gB.addNode(sv);
	// Pins: CallExternal execIn 0 / execOut 1 / Target 2 / x 3 / y(out) 4.
	REQUIRE(gB.connect(e,    0, ceId, 0)); // exec
	REQUIRE(gB.connect(t,    0, ceId, 2)); // target -> Target
	REQUIRE(gB.connect(c,    0, ceId, 3)); // 42 -> x
	REQUIRE(gB.connect(ceId, 1, s,    0)); // exec -> SetVariable
	REQUIRE(gB.connect(ceId, 4, s,    2)); // y -> out
	const InstanceId B = rt.add(std::move(gB));
	rt.setVariable(B, "target", Value::ofRef(A));

	rt.fireEvent(B, "Go");
	CHECK(rt.getVariable(B, "out").i == 42); // argument crossed the instance boundary and came back
}

TEST_CASE("assignSubgraphs partitions a flat graph into per-function sub-graphs")
{
	Graph g;
	{ Variable v; v.name = "x"; v.type = PinType::Bool; g.variables.push_back(v); }
	// Event graph: Go → SetVariable(x, true).
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f; const int c = g.addNode(cb);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "x"; sv.propType = PinType::Bool; const int s = g.addNode(sv);
	REQUIRE(g.connect(e, 0, s, 0));
	REQUIRE(g.connect(c, 0, s, 2));
	// Function Fn(): entry → return, with a Const feeding the return value.
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Fn"; fe.results = { { "r", PinType::Bool } };
	const int fnId = g.addNode(fe);
	Node fr; fr.type = NodeType::FunctionReturn; fr.s = "Fn"; const int frId = g.addNode(fr);
	syncFunctionSignatures(g);
	Node ci; ci.type = NodeType::ConstBool; ci.f[0] = 1.0f; const int ci2 = g.addNode(ci);
	REQUIRE(g.connect(fnId, 0, frId, 0)); // exec
	REQUIRE(g.connect(ci2,  0, frId, 1)); // const → return.r

	assignSubgraphs(g);

	CHECK(g.findNode(e)->subgraph   == 0);      // event chain stays in the event graph
	CHECK(g.findNode(s)->subgraph   == 0);
	CHECK(g.findNode(c)->subgraph   == 0);
	CHECK(g.findNode(fnId)->subgraph == fnId);  // function body → its own sub-graph
	CHECK(g.findNode(frId)->subgraph == fnId);
	CHECK(g.findNode(ci2)->subgraph  == fnId);

	// Idempotent: a second pass (already partitioned) changes nothing.
	assignSubgraphs(g);
	CHECK(g.findNode(frId)->subgraph == fnId);
}

namespace {
	std::vector<std::string> g_hcErrors;
	void hcErrorSink(HE::LogLevel level, const char* msg, void*)
	{ if (level == HE::LogLevel::Error) g_hcErrors.emplace_back(msg ? msg : ""); }
}

TEST_CASE("HorizonCode logs a null-reference error for a Call on a null target")
{
	// Calling a function through an unwired (null) Ref is the classic "Accessed
	// None" runtime error — it must be logged (Error) so it surfaces in the game
	// log and the editor's post-PIE report, not silently swallowed.
	g_hcErrors.clear();
	Logger::setSink(&hcErrorSink, nullptr);

	Runtime rt;
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.elem = 0;
	const int e = g.addNode(ev);
	Node ce; ce.type = NodeType::CallExternal; ce.s = "DoThing";
	const int c = g.addNode(ce);
	REQUIRE(g.connect(e, 0, c, 0));   // exec only; Target (Ref, dataIn 2) left unwired → null
	const InstanceId inst = rt.add(g);
	rt.fireEvent(inst, "Go", 0);

	Logger::setSink(nullptr, nullptr);

	bool found = false;
	for (const auto& m : g_hcErrors)
		if (m.find("null reference") != std::string::npos && m.find("DoThing") != std::string::npos)
			{ found = true; break; }
	CHECK(found);
}

// ── Function-local variables (Variable::scope != 0) ──────────────────────────

TEST_CASE("Function-locals seed from their default per call and stay out of the instance store")
{
	Graph g;
	Variable out; out.name = "out"; out.type = PinType::Float;
	g.variables.push_back(out);

	// Function F: Set out = Get l  (l = local of F, default 5).
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F"; fe.access = 0;
	const int f = g.addNode(fe);
	Variable l; l.name = "l"; l.type = PinType::Float; l.f[0] = 5.0f; l.scope = f;
	g.variables.push_back(l);
	Node gv; gv.type = NodeType::GetVariable; gv.s = "l"; gv.propType = PinType::Float;
	const int r = g.addNode(gv);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Float;
	const int s = g.addNode(sv);
	REQUIRE(g.connect(f, 0, s, 0));   // exec
	REQUIRE(g.connect(r, 0, s, 2));   // Get l → Value

	Runtime rt;
	const InstanceId id = rt.add(g);
	// The local is NOT part of the instance store (only scope-0 vars are seeded).
	CHECK(rt.variablesOf(id).count("l") == 0);
	CHECK(rt.callFunction(id, "F", true));
	CHECK(rt.getVariable(id, "out").f == doctest::Approx(5.0f));
	CHECK(rt.variablesOf(id).count("l") == 0); // a Set inside F must not create it either
}

TEST_CASE("Function-locals reset on every invocation (no persistence across calls)")
{
	Graph g;
	Variable seen; seen.name = "seen"; seen.type = PinType::Float;
	g.variables.push_back(seen);

	// F: Set c = (Get c) + 1  →  Set seen = Get c.   c = local, default 0.
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F"; fe.access = 0;
	const int f = g.addNode(fe);
	Variable c; c.name = "c"; c.type = PinType::Float; c.scope = f;
	g.variables.push_back(c);

	Node g1; g1.type = NodeType::GetVariable; g1.s = "c"; g1.propType = PinType::Float;
	const int gc1 = g.addNode(g1);
	Node one; one.type = NodeType::ConstFloat; one.f[0] = 1.0f;
	const int k1 = g.addNode(one);
	Node add; add.type = NodeType::Add;
	const int a = g.addNode(add);
	Node s1; s1.type = NodeType::SetVariable; s1.s = "c"; s1.propType = PinType::Float;
	const int sc = g.addNode(s1);
	Node g2; g2.type = NodeType::GetVariable; g2.s = "c"; g2.propType = PinType::Float;
	const int gc2 = g.addNode(g2);
	Node s2; s2.type = NodeType::SetVariable; s2.s = "seen"; s2.propType = PinType::Float;
	const int ss = g.addNode(s2);

	REQUIRE(g.connect(f, 0, sc, 0));     // exec: F → Set c
	REQUIRE(g.connect(gc1, 0, a, 0));    // Get c → Add.A
	REQUIRE(g.connect(k1, 0, a, 1));     // 1 → Add.B
	REQUIRE(g.connect(a, 2, sc, 2));     // Add → Set c.Value
	REQUIRE(g.connect(sc, 1, ss, 0));    // exec: Set c → Set seen
	REQUIRE(g.connect(gc2, 0, ss, 2));   // Get c → Set seen.Value

	Runtime rt;
	const InstanceId id = rt.add(g);
	CHECK(rt.callFunction(id, "F", true));
	CHECK(rt.getVariable(id, "seen").f == doctest::Approx(1.0f));
	CHECK(rt.callFunction(id, "F", true));
	// A persistent (instance) c would read 2 here; a per-call local reads 1.
	CHECK(rt.getVariable(id, "seen").f == doctest::Approx(1.0f));
	CHECK(rt.variablesOf(id).count("c") == 0);
}

TEST_CASE("Recursion keeps a fresh local per call frame")
{
	// R(n): Set mine = n; if (n > 0.5) { R(n - 1); Set final = Get mine; }
	//                     else          Set final = Get mine;
	// With per-frame locals, the OUTERMOST write wins → final == the original n.
	// With a shared store the inner frames would clobber `mine` → final == 0.
	Graph g;
	Variable fin; fin.name = "final"; fin.type = PinType::Float;
	g.variables.push_back(fin);

	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "R"; fe.access = 0;
	fe.params.push_back({ "n", PinType::Float });
	const int f = g.addNode(fe);
	Variable mine; mine.name = "mine"; mine.type = PinType::Float; mine.scope = f;
	g.variables.push_back(mine);

	Node sm; sm.type = NodeType::SetVariable; sm.s = "mine"; sm.propType = PinType::Float;
	const int smId = g.addNode(sm);
	Node gt; gt.type = NodeType::Greater;
	const int gtId = g.addNode(gt);
	Node half; half.type = NodeType::ConstFloat; half.f[0] = 0.5f;
	const int halfId = g.addNode(half);
	Node br; br.type = NodeType::Branch;
	const int brId = g.addNode(br);
	Node sub; sub.type = NodeType::Subtract;
	const int subId = g.addNode(sub);
	Node one; one.type = NodeType::ConstFloat; one.f[0] = 1.0f;
	const int oneId = g.addNode(one);
	Node call; call.type = NodeType::FunctionCall; call.s = "R";
	const int callId = g.addNode(call);
	Node gm1; gm1.type = NodeType::GetVariable; gm1.s = "mine"; gm1.propType = PinType::Float;
	const int gm1Id = g.addNode(gm1);
	Node sf1; sf1.type = NodeType::SetVariable; sf1.s = "final"; sf1.propType = PinType::Float;
	const int sf1Id = g.addNode(sf1);
	Node gm2; gm2.type = NodeType::GetVariable; gm2.s = "mine"; gm2.propType = PinType::Float;
	const int gm2Id = g.addNode(gm2);
	Node sf2; sf2.type = NodeType::SetVariable; sf2.s = "final"; sf2.propType = PinType::Float;
	const int sf2Id = g.addNode(sf2);

	syncFunctionSignatures(g);           // FunctionCall mirrors R's params

	REQUIRE(g.connect(f, 0, smId, 0));       // exec: R → Set mine
	REQUIRE(g.connect(f, 1, smId, 2));       // n → Set mine.Value
	REQUIRE(g.connect(smId, 1, brId, 0));    // exec: Set mine → Branch
	REQUIRE(g.connect(f, 1, gtId, 0));       // n → Greater.A
	REQUIRE(g.connect(halfId, 0, gtId, 1));  // 0.5 → Greater.B
	REQUIRE(g.connect(gtId, 2, brId, 3));    // cond
	REQUIRE(g.connect(brId, 1, callId, 0));  // True → Call R
	REQUIRE(g.connect(f, 1, subId, 0));      // n → Sub.A
	REQUIRE(g.connect(oneId, 0, subId, 1));  // 1 → Sub.B
	REQUIRE(g.connect(subId, 2, callId, 2)); // n-1 → Call arg
	REQUIRE(g.connect(callId, 1, sf1Id, 0)); // exec after the call
	REQUIRE(g.connect(gm1Id, 0, sf1Id, 2));  // Get mine → Set final
	REQUIRE(g.connect(brId, 2, sf2Id, 0));   // False → Set final
	REQUIRE(g.connect(gm2Id, 0, sf2Id, 2));  // Get mine → Set final

	Runtime rt;
	const InstanceId id = rt.add(g);
	CHECK(rt.callFunction(id, "R", true, { Value::ofFloat(2.0f) }));
	CHECK(rt.getVariable(id, "final").f == doctest::Approx(2.0f));
}

TEST_CASE("A local read outside its function yields the type's default")
{
	Graph g;
	Variable out; out.name = "out"; out.type = PinType::Float;
	g.variables.push_back(out);
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F"; fe.access = 0;
	const int f = g.addNode(fe);
	Variable l; l.name = "l"; l.type = PinType::Float; l.f[0] = 7.0f; l.scope = f;
	g.variables.push_back(l);

	// Event graph reads the local — no frame of F is active → 0, not 7.
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.elem = 0;
	const int e = g.addNode(ev);
	Node gv; gv.type = NodeType::GetVariable; gv.s = "l"; gv.propType = PinType::Float;
	const int r = g.addNode(gv);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Float;
	const int s = g.addNode(sv);
	REQUIRE(g.connect(e, 0, s, 0));
	REQUIRE(g.connect(r, 0, s, 2));

	Runtime rt;
	const InstanceId id = rt.add(g);
	rt.fireEvent(id, "Go", 0);
	CHECK(rt.getVariable(id, "out").f == doctest::Approx(0.0f));
}

TEST_CASE("Locals are never visible through Get (Ref) — even when marked public")
{
	Graph g;
	Variable out; out.name = "out"; out.type = PinType::Float;
	g.variables.push_back(out);
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F"; fe.access = 0;
	const int f = g.addNode(fe);
	Variable l; l.name = "l"; l.type = PinType::Float; l.f[0] = 7.0f;
	l.scope = f; l.access = 0;   // "public" — must still be invisible externally
	g.variables.push_back(l);

	// Event Go: Set out = GetExternal(Target = Self, "l") — warns, yields 0.
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.elem = 0;
	const int e = g.addNode(ev);
	Node self; self.type = NodeType::GetSelf;
	const int selfId = g.addNode(self);
	Node ge; ge.type = NodeType::GetExternal; ge.s = "l"; ge.propType = PinType::Float;
	const int geId = g.addNode(ge);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Float;
	const int s = g.addNode(sv);
	REQUIRE(g.connect(e, 0, s, 0));         // exec
	REQUIRE(g.connect(selfId, 0, geId, 0)); // Self → Target
	REQUIRE(g.connect(geId, 1, s, 2));      // external value → Set out

	Runtime rt;
	const InstanceId id = rt.add(g);
	rt.fireEvent(id, "Go", 0);
	CHECK(rt.getVariable(id, "out").f == doctest::Approx(0.0f));
}

TEST_CASE("Variable scope survives the JSON round-trip and dies with its function")
{
	Graph g;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F";
	const int f = g.addNode(fe);
	Variable inst; inst.name = "keep"; inst.type = PinType::Int;
	g.variables.push_back(inst);
	Variable l; l.name = "l"; l.type = PinType::Float; l.f[0] = 3.0f; l.scope = f;
	g.variables.push_back(l);

	Graph back;
	REQUIRE(fromJson(toJson(g), back));
	const Variable* lv = back.findVariable("l");
	REQUIRE(lv != nullptr);
	CHECK(lv->scope == f);
	CHECK(back.findVariable("keep")->scope == 0);

	// Deleting the function removes its locals (but not instance variables).
	back.removeNode(f);
	CHECK(back.findVariable("l") == nullptr);
	CHECK(back.findVariable("keep") != nullptr);
}

// ═══ Graph container semantics ═══════════════════════════════════════════════

TEST_CASE("connect: a DATA input holds one link, an EXEC output holds one link")
{
	// The one place HorizonCode's link semantics differ from Material/
	// ParticleGraph: it also has exec pins, and an exec OUTPUT is a single "what
	// runs next" pointer, so the SOURCE side is what gets replaced there. Data
	// pins follow the shared rule — replace on the INPUT, fan out on the output.
	Graph g;
	Node seq; seq.type = NodeType::Sequence;
	const int s = g.addNode(seq);
	Node p1; p1.type = NodeType::Print;
	const int a = g.addNode(p1);
	Node p2; p2.type = NodeType::Print;
	const int b = g.addNode(p2);

	// Sequence pins: execIn 0, Then 0 = 1, Then 1 = 2.
	CHECK(g.connect(s, 1, a, 0));
	REQUIRE(g.links.size() == 1);
	CHECK(g.connect(s, 1, b, 0));      // same exec OUT → replaces
	REQUIRE(g.links.size() == 1);
	CHECK(g.links[0].dstNode == b);
	CHECK(g.connect(s, 2, a, 0));      // a different exec out → additional link
	CHECK(g.links.size() == 2);

	// Data: two strings racing for the same Print input.
	Node c1; c1.type = NodeType::ConstString; c1.s = "one";
	const int k1 = g.addNode(c1);
	Node c2; c2.type = NodeType::ConstString; c2.s = "two";
	const int k2 = g.addNode(c2);
	CHECK(g.connect(k1, 0, a, 2));     // Print dataIn 0 is unified pin 2
	CHECK(g.connect(k2, 0, a, 2));     // replaces, never appends
	int toPrintA = 0;
	for (const Link& l : g.links) if (l.dstNode == a && l.dstPin == 2) ++toPrintA;
	CHECK(toPrintA == 1);

	// One data OUTPUT may feed many inputs.
	CHECK(g.connect(k2, 0, b, 2));
	int fromK2 = 0;
	for (const Link& l : g.links) if (l.srcNode == k2) ++fromK2;
	CHECK(fromK2 == 2);
}

TEST_CASE("removeNode drops every link touching the node, in either direction")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Tick";
	const int e = g.addNode(ev);
	Node pr; pr.type = NodeType::Print;
	const int p = g.addNode(pr);
	Node cs; cs.type = NodeType::ConstString; cs.s = "x";
	const int c = g.addNode(cs);
	CHECK(g.connect(e, 0, p, 0));   // exec in
	CHECK(g.connect(c, 0, p, 2));   // data in
	REQUIRE(g.links.size() == 2);

	g.removeNode(p);
	CHECK(g.findNode(p) == nullptr);
	CHECK(g.links.empty());         // both the incoming exec and the incoming data
}

TEST_CASE("Graph never reuses a node id after removeNode")
{
	Graph g;
	Node n1; n1.type = NodeType::ConstFloat;
	const int a = g.addNode(n1);
	Node n2; n2.type = NodeType::ConstFloat;
	const int b = g.addNode(n2);
	g.removeNode(a);
	g.removeNode(b);
	Node n3; n3.type = NodeType::ConstFloat;
	const int c = g.addNode(n3);
	CHECK(c != a);
	CHECK(c != b);
	CHECK(c > b);
}

TEST_CASE("every node type has a display name, a tooltip and a real category")
{
	// nodeDisplayName falls back to "?" and nodeTooltip to "" — both silent. A
	// forgotten case therefore ships a node that shows as "?" in the menu and,
	// worse, SERIALISES as "?": the display name is the on-disk type key, so two
	// nameless nodes would collide and load as each other. Nothing else in the
	// suite looks at these.
	std::set<std::string> names;
	for (NodeType t : nodeRegistry())
	{
		const char* name = nodeDisplayName(t);
		REQUIRE(name != nullptr);
		INFO("node type index ", (int)t, " name '", name, "'");
		CHECK(std::string(name) != "?");
		CHECK(std::string(name) != "");
		// Unique, because the name is what fromJson matches on.
		CHECK(names.insert(name).second);

		CHECK(std::string(nodeTooltip(t)) != "");
		// "Misc" is nodeCategory's fallback; the add menu lists by category, so a
		// node that lands there is invisible in every host.
		CHECK(std::string(nodeCategory(t)) != "Misc");

		// And the name actually survives a save/load: nodeFromJson matches the
		// stored string back to a type, so a node whose name does not resolve
		// would be dropped — silently, along with all its links.
		Node n; n.type = t; n.id = 1;
		Node back;
		REQUIRE(nodeFromJson(nodeToJson(n), back));
		CHECK(back.type == t);
	}
}

TEST_CASE("signatureCountsOf / dataPinDescOf agree with signatureOf for every node type")
{
	// The allocation-free accessors must answer exactly what building the full
	// NodeSig would — they are the same switch, and the hot paths use them.
	for (NodeType t : nodeRegistry())
	{
		Node n; n.type = t;
		n.hasArg = true;                 // exercise the variable-pin branches
		n.propType = PinType::Int;
		n.params  = { { "a", PinType::Float, false }, { "b", PinType::String, true } };
		n.results = { { "r", PinType::Bool, false } };

		const NodeSig sig = signatureOf(n);
		const NodeSigCounts cnt = signatureCountsOf(n);
		CHECK(cnt.execIns  == (int)sig.execIns.size());
		CHECK(cnt.execOuts == (int)sig.execOuts.size());
		CHECK(cnt.dataIns  == (int)sig.dataIns.size());
		CHECK(cnt.dataOuts == (int)sig.dataOuts.size());

		for (int i = 0; i < (int)sig.dataIns.size(); ++i)
		{
			PinDesc d;
			REQUIRE(dataPinDescOf(n, /*input=*/true, i, d));
			CHECK(d.type == sig.dataIns[i].type);
			CHECK(d.isArray == sig.dataIns[i].isArray);
		}
		for (int i = 0; i < (int)sig.dataOuts.size(); ++i)
		{
			PinDesc d;
			REQUIRE(dataPinDescOf(n, /*input=*/false, i, d));
			CHECK(d.type == sig.dataOuts[i].type);
			CHECK(d.isArray == sig.dataOuts[i].isArray);
		}
		PinDesc oob;
		CHECK_FALSE(dataPinDescOf(n, true,  (int)sig.dataIns.size(),  oob));
		CHECK_FALSE(dataPinDescOf(n, false, (int)sig.dataOuts.size(), oob));
		CHECK_FALSE(dataPinDescOf(n, true,  -1, oob));
	}
}

// ═══ On-disk format ══════════════════════════════════════════════════════════

TEST_CASE("fromJson loads a hand-written OLD-format document")
{
	// The shape toJson writes: pretty dump, node type by DISPLAY NAME, links as
	// ARRAYS [srcNode, srcPin, dstNode, dstPin]. Kept as a literal so refactoring
	// the writer can never quietly redefine what still loads.
	const std::string old =
		R"J({"nextId":4,)J"
		R"J("nodes":[{"id":1,"type":"Event","pos":[10.0,20.0],"s":"OnClicked","hasArg":false},)J"
		R"J({"id":2,"type":"Print","pos":[200.0,20.0]},)J"
		R"J({"id":3,"type":"String","pos":[40.0,120.0],"s":"hello"}],)J"
		R"J("links":[[1,0,2,0],[3,0,2,2]],)J"
		R"J("variables":[{"name":"Count","type":3,"f":[7.0,0.0,0.0,0.0]}]})J";

	Graph g;
	REQUIRE(fromJson(old, g));
	REQUIRE(g.nodes.size() == 3);
	CHECK(g.nodes[0].type == NodeType::Event);
	CHECK(g.nodes[0].s == "OnClicked");
	CHECK(g.nodes[2].type == NodeType::ConstString);
	CHECK(g.nodes[2].s == "hello");
	REQUIRE(g.links.size() == 2);
	CHECK(g.links[0].srcNode == 1);
	CHECK(g.links[1].dstPin  == 2);
	REQUIRE(g.variables.size() == 1);
	CHECK(g.variables[0].name == "Count");
	CHECK(g.nextId == 4);
}

TEST_CASE("fromJson still accepts the legacy \"Array Add\" node name")
{
	// Nodes serialize by DISPLAY NAME, so the "Array Add" → "Array Append" rename
	// would have dropped the node (and its links) out of every saved graph. The
	// alias table is what keeps those graphs loading — this is its regression test.
	const std::string legacy =
		R"J({"nextId":2,"nodes":[{"id":1,"type":"Array Add","pos":[0.0,0.0],"propType":2}],)J"
		R"J("links":[],"variables":[]})J";
	Graph g;
	REQUIRE(fromJson(legacy, g));
	REQUIRE(g.nodes.size() == 1);
	CHECK(g.nodes[0].type == NodeType::ArrayAdd);

	// The current name loads too, and round-trips.
	Graph r;
	REQUIRE(fromJson(toJson(g), r));
	REQUIRE(r.nodes.size() == 1);
	CHECK(r.nodes[0].type == NodeType::ArrayAdd);
}

TEST_CASE("fromJson drops links whose endpoints did not survive, and repairs nextId")
{
	const std::string json =
		R"J({"nextId":1,"nodes":[{"id":9,"type":"Print","pos":[0.0,0.0]}],)J"
		R"J("links":[[9,0,404,0],[404,0,9,0],[9]],"variables":[]})J";
	Graph g;
	REQUIRE(fromJson(json, g));
	CHECK(g.links.empty());   // node 404 was never loaded; the 1-element link is skipped
	CHECK(g.nextId == 10);    // a saved id >= nextId must push it past
	Node n; n.type = NodeType::ConstFloat;
	CHECK(g.addNode(n) == 10);
}

// ── Declared events ─────────────────────────────────────────────────────────

TEST_CASE("a graph's custom events survive a JSON round trip")
{
    Graph g;
    Node ev; ev.type = NodeType::Event; ev.s = "Signal"; ev.hasArg = true;
    ev.propType = PinType::Int;
    g.addNode(std::move(ev));
    Node em; em.type = NodeType::EmitEvent; em.s = "Other";
    g.addNode(std::move(em));
    // Engine events are a fixed list, not this class's interface.
    Node ctor; ctor.type = NodeType::Event; ctor.s = "Construct";
    g.addNode(std::move(ctor));

    Graph loaded;
    REQUIRE(fromJson(toJson(g), loaded));
    REQUIRE(loaded.events.size() == 2);          // Signal + Other, not Construct
    const EventDecl* sig = loaded.findEvent("Signal");
    REQUIRE(sig != nullptr);
    CHECK(sig->hasArg);
    CHECK(sig->argType == PinType::Int);
    CHECK(loaded.findEvent("Other") != nullptr);
    CHECK(loaded.findEvent("Construct") == nullptr);

    // A second trip changes nothing, and the declarations now come from the
    // payload rather than being harvested again.
    Graph twice;
    REQUIRE(fromJson(toJson(loaded), twice));
    CHECK(twice.events.size() == 2);
}

TEST_CASE("event ids are stable, shared, and reversible")
{
    const EventId a = eventId("Signal");
    const EventId b = eventId("Signal");
    CHECK(a == b);
    CHECK(a != 0);
    CHECK(eventId("Other") != a);
    CHECK(eventName(a) == "Signal");
    CHECK(eventId("") == 0);          // "no event" has an id of its own
    CHECK(eventName(0).empty());
    CHECK(eventName(999999).empty()); // an id nobody handed out reads as none
}

TEST_CASE("engine events are a closed list, separate from a class's own")
{
    CHECK(findEngineEvent("OnClicked") != nullptr);
    CHECK(findEngineEvent("Construct") != nullptr);
    CHECK(findEngineEvent("Signal") == nullptr);
    const EngineEventDesc* tick = findEngineEvent("Tick");
    REQUIRE(tick != nullptr);
    CHECK(tick->arg == PinType::Float);
    CHECK_FALSE(tick->elem);          // the call site carries no element
    const EngineEventDesc* click = findEngineEvent("OnClicked");
    REQUIRE(click != nullptr);
    CHECK(click->elem);
    CHECK(std::string(click->hook) == "onClicked");
}

// ── The engine class taxonomy ───────────────────────────────────────────────

TEST_CASE("the engine class chain runs Object → Entity → the two player classes")
{
    CHECK(findEngineClass("Entity") != nullptr);
    CHECK(findEngineClass("PlayerCharacter") != nullptr);
    CHECK(findEngineClass("Goblin") == nullptr);
    // "" is how every asset predating the taxonomy spells Object on disk.
    REQUIRE(findEngineClass("") != nullptr);
    CHECK(std::string(findEngineClass("")->name) == "Object");

    CHECK(engineClassIsA("PlayerCharacter", "Entity"));
    CHECK(engineClassIsA("PlayerCharacter", "Object"));
    CHECK(engineClassIsA("PlayerCharacter", ""));          // "" == Object
    CHECK(engineClassIsA("Entity", "Object"));
    CHECK(engineClassIsA("Entity", "Entity"));
    CHECK(engineClassIsA("", "Object"));
    // Downward and sideways are not "is a".
    CHECK_FALSE(engineClassIsA("Entity", "PlayerCharacter"));
    CHECK_FALSE(engineClassIsA("PlayerCharacter", "PlayerController"));
    CHECK_FALSE(engineClassIsA("Object", "Entity"));
    // A name the table does not know matches itself and nothing else — a
    // garbage baseClass must not answer "yes" to every cast.
    CHECK(engineClassIsA("Goblin", "Goblin"));
    CHECK_FALSE(engineClassIsA("Goblin", "Entity"));
    CHECK_FALSE(engineClassIsA("Goblin", "Object"));
}

TEST_CASE("a class's event catalog is its whole chain, base first")
{
    const auto object = engineClassEvents("");
    REQUIRE(object.size() == 2);
    CHECK(std::string(object[0]) == "Construct");
    CHECK(std::string(object[1]) == "Destruct");

    // Entity is where the game lifecycle and the physics contacts start, and a
    // player class inherits both without restating either.
    const auto entity = engineClassEvents("Entity");
    const auto player = engineClassEvents("PlayerCharacter");
    CHECK(player.size() == entity.size());
    auto has = [](const std::vector<const char*>& v, const char* n)
    {
        for (const char* e : v) if (std::string(e) == n) return true;
        return false;
    };
    CHECK(has(entity, "Construct"));       // from Object
    CHECK(has(entity, "BeginPlay"));
    CHECK(has(entity, "Tick"));
    CHECK(has(entity, "OnBeginOverlap"));
    CHECK(has(entity, "OnEndOverlap"));
    CHECK(has(entity, "OnHit"));
    CHECK(has(entity, "OnHitEnd"));
    // The animation notifies land on Entity for the same reason the contacts do:
    // they are addressed to a thing in the world, and the clip that fires them
    // is played on one.
    CHECK(has(entity, "OnAnimationNotify"));
    CHECK(has(entity, "OnAnimationNotifyBegin"));
    CHECK(has(entity, "OnAnimationNotifyEnd"));
    // The anti-cheat report that names this entity — addressed to a thing in
    // the world, like the contacts, so it lands on Entity and not on Object.
    CHECK(has(entity, "OnCheatDetected"));
    CHECK_FALSE(has(object, "OnCheatDetected"));
    CHECK(has(player, "BeginPlay"));
    CHECK(has(player, "OnBeginOverlap"));
    // A plain Object has no world presence, so neither the game lifecycle nor
    // any contact can reach it.
    CHECK_FALSE(has(object, "Tick"));
    CHECK_FALSE(has(object, "OnHit"));

    // Every event a class offers must be one the engine actually fires.
    for (const char* e : player) CHECK(findEngineEvent(e) != nullptr);
}

TEST_CASE("an instance knows its class, so a reference can be tested against one")
{
    Runtime rt;
    const InstanceId goblin = rt.add(Graph{}, {}, { "Content/Goblin.hasset", "Entity" });
    const InstanceId hero   = rt.add(Graph{}, {}, { "Content/Hero.hasset",   "PlayerCharacter" });
    const InstanceId plain  = rt.add(Graph{});   // registered without an identity

    CHECK(rt.classKeyOf(goblin) == "Content/Goblin.hasset");
    CHECK(rt.baseClassOf(hero) == "PlayerCharacter");

    // Exact class.
    CHECK(rt.instanceIsA(goblin, "Content/Goblin.hasset"));
    CHECK_FALSE(rt.instanceIsA(goblin, "Content/Hero.hasset"));
    // Up the engine chain.
    CHECK(rt.instanceIsA(goblin, "Entity"));
    CHECK(rt.instanceIsA(goblin, "Object"));
    CHECK(rt.instanceIsA(hero, "PlayerCharacter"));
    CHECK(rt.instanceIsA(hero, "Entity"));
    CHECK_FALSE(rt.instanceIsA(hero, "PlayerController"));
    CHECK_FALSE(rt.instanceIsA(goblin, "PlayerCharacter"));
    // HorizonCode classes do not derive from one another: an asset path only
    // ever matches itself, never another class's path.
    CHECK_FALSE(rt.instanceIsA(hero, "Content/Goblin.hasset"));

    // An instance nobody named is still an Object and nothing more.
    CHECK(rt.instanceIsA(plain, "Object"));
    CHECK_FALSE(rt.instanceIsA(plain, "Entity"));

    // A dead or never-existing reference is not an instance of anything — this
    // is what makes Cast on a destroyed object take its Failure branch.
    rt.destroy(goblin);
    CHECK_FALSE(rt.instanceIsA(goblin, "Entity"));
    CHECK_FALSE(rt.instanceIsA(0, "Object"));
    CHECK_FALSE(rt.instanceIsA(hero, ""));   // no class named "" to cast to
}

TEST_CASE("the GameInstance is keyed by its host, not by a file")
{
    Runtime rt;
    const InstanceId gi = rt.setGameInstance(Graph{});
    CHECK(rt.classKeyOf(gi) == "__game_instance__");
    CHECK(rt.instanceIsA(gi, "__game_instance__"));
    CHECK(rt.instanceIsA(gi, "Object"));
    CHECK_FALSE(rt.instanceIsA(gi, "Entity"));
}

TEST_CASE("the scene entity an instance owns rides along with it")
{
    Runtime rt;
    const InstanceId inst = rt.add(Graph{}, {}, { "Content/Door.hasset", "Entity" });
    CHECK(rt.ownedEntity(inst) == 0u);        // nothing bound yet
    rt.setOwnedEntity(inst, 42u);
    CHECK(rt.ownedEntity(inst) == 42u);
    rt.remove(inst);
    CHECK(rt.ownedEntity(inst) == 0u);        // a gone instance owns nothing
}

// ── The Cast node ───────────────────────────────────────────────────────────

namespace
{
    // Event("Go") → Cast(target) with the Object input fed by a Ref literal
    // that the caller supplies as an event arg. Success sets "hit", Failure
    // sets "miss", and the cast's output reference is stored into "got".
    //
    // Ref values have no literal node, so the reference travels as the event's
    // argument: Event.hasArg with propType Ref.
    Graph castGraph(const std::string& target)
    {
        Graph g;
        Variable hit;  hit.name  = "hit";  hit.type = PinType::Bool;  g.variables.push_back(hit);
        Variable miss; miss.name = "miss"; miss.type = PinType::Bool;  g.variables.push_back(miss);
        Variable got;  got.name  = "got";  got.type = PinType::Ref;    g.variables.push_back(got);

        Node ev; ev.type = NodeType::Event; ev.s = "Go";
        ev.hasArg = true; ev.propType = PinType::Ref;
        const int e = g.addNode(std::move(ev));

        Node ca; ca.type = NodeType::Cast; ca.s = target;
        const int c = g.addNode(std::move(ca));

        Node sh; sh.type = NodeType::SetVariable; sh.s = "hit";  sh.propType = PinType::Bool;
        const int h = g.addNode(std::move(sh));
        Node sm; sm.type = NodeType::SetVariable; sm.s = "miss"; sm.propType = PinType::Bool;
        const int m = g.addNode(std::move(sm));
        Node sg; sg.type = NodeType::SetVariable; sg.s = "got";  sg.propType = PinType::Ref;
        const int gi = g.addNode(std::move(sg));
        Node tb; tb.type = NodeType::ConstBool; tb.f[0] = 1.0f;
        const int t = g.addNode(std::move(tb));

        // Cast pins: execIn 0, Success 1, Failure 2, Object 3, "As …" 4.
        // Event exec (0) → Cast exec-in; Event arg (1, a Ref) → Cast Object.
        REQUIRE(g.connect(e, 0, c, 0));
        REQUIRE(g.connect(e, 1, c, 3));
        // Success → Set hit → Set got (from the cast's output).
        REQUIRE(g.connect(c, 1, h, 0));
        REQUIRE(g.connect(t, 0, h, 2));
        REQUIRE(g.connect(h, 1, gi, 0));
        REQUIRE(g.connect(c, 4, gi, 2));
        // Failure → Set miss.
        REQUIRE(g.connect(c, 2, m, 0));
        REQUIRE(g.connect(t, 0, m, 2));
        return g;
    }
}

TEST_CASE("Cast takes Success for its own class and hands the reference through")
{
    Runtime rt;
    const InstanceId target = rt.add(Graph{}, {}, { "Content/Goblin.hasset", "Entity" });
    const InstanceId caster = rt.add(castGraph("Content/Goblin.hasset"));

    rt.fireEvent(caster, "Go", 0, Value::ofRef(target));
    CHECK(rt.getVariable(caster, "hit").b);
    CHECK_FALSE(rt.getVariable(caster, "miss").b);
    CHECK(rt.getVariable(caster, "got").ref == target);
}

TEST_CASE("Cast to a base class succeeds; the other way round and sideways fail")
{
    Runtime rt;
    const InstanceId hero = rt.add(Graph{}, {}, { "Content/Hero.hasset", "PlayerCharacter" });

    // Up the chain: a PlayerCharacter IS an Entity.
    {
        const InstanceId c = rt.add(castGraph("Entity"));
        rt.fireEvent(c, "Go", 0, Value::ofRef(hero));
        CHECK(rt.getVariable(c, "hit").b);
        CHECK(rt.getVariable(c, "got").ref == hero);
    }
    // Sideways: it is not a PlayerController.
    {
        const InstanceId c = rt.add(castGraph("PlayerController"));
        rt.fireEvent(c, "Go", 0, Value::ofRef(hero));
        CHECK(rt.getVariable(c, "miss").b);
        CHECK(rt.getVariable(c, "got").ref == 0u);
    }
    // Down the chain: an Entity-only instance is not a PlayerCharacter.
    {
        const InstanceId plain = rt.add(Graph{}, {}, { "Content/Door.hasset", "Entity" });
        const InstanceId c = rt.add(castGraph("PlayerCharacter"));
        rt.fireEvent(c, "Go", 0, Value::ofRef(plain));
        CHECK(rt.getVariable(c, "miss").b);
    }
}

TEST_CASE("Cast on a null or destroyed reference takes Failure")
{
    Runtime rt;
    const InstanceId victim = rt.add(Graph{}, {}, { "Content/Goblin.hasset", "Entity" });

    {   // Ref 0 — nothing was ever there.
        const InstanceId c = rt.add(castGraph("Entity"));
        rt.fireEvent(c, "Go", 0, Value::ofRef(0));
        CHECK(rt.getVariable(c, "miss").b);
        CHECK_FALSE(rt.getVariable(c, "hit").b);
    }
    {   // A reference to something that has since been destroyed. This is why
        // Cast doubles as an Is Valid: the guard is built in.
        rt.destroy(victim);
        const InstanceId c = rt.add(castGraph("Entity"));
        rt.fireEvent(c, "Go", 0, Value::ofRef(victim));
        CHECK(rt.getVariable(c, "miss").b);
        CHECK(rt.getVariable(c, "got").ref == 0u);
    }
}

TEST_CASE("a Cast with no target class always takes Failure")
{
    // Consistent rather than broken — and the codegen warns about it instead of
    // failing the build, so both backends agree on this graph too.
    Runtime rt;
    const InstanceId target = rt.add(Graph{}, {}, { "Content/Goblin.hasset", "Entity" });
    const InstanceId c      = rt.add(castGraph(""));
    rt.fireEvent(c, "Go", 0, Value::ofRef(target));
    CHECK(rt.getVariable(c, "miss").b);
    CHECK_FALSE(rt.getVariable(c, "hit").b);
}

TEST_CASE("the Cast node's serialized name does not depend on its target")
{
    // nodeDisplayName IS the key on disk (see the boxed warning in
    // HorizonCode.cpp): if it ever grew the class name, every saved graph
    // holding a Cast would stop loading the moment the target changed.
    CHECK(std::string(nodeDisplayName(NodeType::Cast)) == "Cast");
    CHECK(std::string(nodeCategory(NodeType::Cast)) == "Reference");

    Graph g = castGraph("Content/Goblin.hasset");
    Graph back;
    REQUIRE(fromJson(toJson(g), back));
    const Node* c = nullptr;
    for (const Node& n : back.nodes) if (n.type == NodeType::Cast) c = &n;
    REQUIRE(c != nullptr);
    CHECK(c->s == "Content/Goblin.hasset");
    // Links survive, so the round trip did not shift a pin.
    CHECK(back.links.size() == g.links.size());
}

// ── Converting wires ────────────────────────────────────────────────────────

TEST_CASE("elementary types convert on the wire")
{
    // What connect() accepts is exactly what coerce() performs — no more.
    CHECK(canConvertPinType(PinType::Float, PinType::Int));
    CHECK(canConvertPinType(PinType::Int,   PinType::Bool));
    CHECK(canConvertPinType(PinType::Bool,  PinType::Float));
    CHECK(canConvertPinType(PinType::Enum,  PinType::Int));
    CHECK(canConvertPinType(PinType::Int,   PinType::Enum));
    CHECK(canConvertPinType(PinType::Vec2,  PinType::Vec2));
    // String's coerce yields the zero value — that is a loss, not a conversion.
    CHECK_FALSE(canConvertPinType(PinType::Float,  PinType::String));
    CHECK_FALSE(canConvertPinType(PinType::String, PinType::Float));
    CHECK_FALSE(canConvertPinType(PinType::Color,  PinType::Vec2));
    CHECK_FALSE(canConvertPinType(PinType::Struct, PinType::Float));

    Graph g;
    Variable out; out.name = "out"; out.type = PinType::Int;
    g.variables.push_back(out);
    Node ev; ev.type = NodeType::Event; ev.s = "Go";
    const int e = g.addNode(std::move(ev));
    Node cf; cf.type = NodeType::ConstFloat; cf.f[0] = 3.9f;
    const int c = g.addNode(std::move(cf));
    Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Int;
    const int s = g.addNode(std::move(sv));

    // Float → Int used to be refused outright; now it connects and truncates.
    REQUIRE(g.connect(c, 0, s, 2));
    REQUIRE(g.connect(e, 0, s, 0));
    // An array still never converts, and a String still refuses.
    Node cs; cs.type = NodeType::ConstString; cs.s = "x";
    const int str = g.addNode(std::move(cs));
    CHECK_FALSE(g.connect(str, 0, s, 2));

    Runtime rt;
    const InstanceId id = rt.add(std::move(g));
    rt.fireEvent(id, "Go", 0, {});
    const Value v = rt.getVariable(id, "out");
    CHECK(v.type == PinType::Int);
    CHECK(v.i == 3);                      // (int)3.9, the interpreter's own rule
}

// ── Conversion nodes on a refused wire ───────────────────────────────────────
namespace
{
	const Node* nodeOfType(const Graph& g, NodeType t)
	{
		for (const Node& n : g.nodes) if (n.type == t) return &n;
		return nullptr;
	}

	// How many links run from one node to another (pin-agnostic).
	int linkCount(const Graph& g, int from, int to)
	{
		int c = 0;
		for (const Link& l : g.links) if (l.srcNode == from && l.dstNode == to) ++c;
		return c;
	}
}

TEST_CASE("conversionNodeFor names a node only where the wire has no other way")
{
	using CK = ContainerKind;
	NodeType n{};

	CHECK(conversionNodeFor(PinType::Float, CK::None, PinType::String, CK::None, n));
	CHECK(n == NodeType::ToString);
	CHECK(conversionNodeFor(PinType::Int,   CK::None, PinType::String, CK::None, n));
	CHECK(n == NodeType::ToString);
	CHECK(conversionNodeFor(PinType::Bool,  CK::None, PinType::String, CK::None, n));
	CHECK(n == NodeType::ToString);
	// Enum beats To String: its int backing would slide into that node's Float
	// pin and print the NUMBER where the entry name was meant.
	CHECK(conversionNodeFor(PinType::Enum,  CK::None, PinType::String, CK::None, n));
	CHECK(n == NodeType::EnumToString);
	CHECK(conversionNodeFor(PinType::Float, CK::Set,  PinType::Float,  CK::Array, n));
	CHECK(n == NodeType::SetToArray);

	// Whatever canConvertPinType already carries needs no node. Enum to Int and
	// Int to Enum EXIST as nodes and must still never be inserted for a wire.
	CHECK_FALSE(conversionNodeFor(PinType::Enum,   CK::None, PinType::Int,    CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Int,    CK::None, PinType::Enum,   CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Float,  CK::None, PinType::Int,    CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Vec3,   CK::None, PinType::Color,  CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::String, CK::None, PinType::String, CK::None, n));
	// Exec is control flow, not a value.
	CHECK_FALSE(conversionNodeFor(PinType::Exec,  CK::None, PinType::Exec,   CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Exec,  CK::None, PinType::String, CK::None, n));
	// Pairs no single node bridges.
	CHECK_FALSE(conversionNodeFor(PinType::Ref,       CK::None, PinType::Float,  CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::String,    CK::None, PinType::Float,  CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Vec3,      CK::None, PinType::String, CK::None, n));
	CHECK_FALSE(conversionNodeFor(PinType::Transform, CK::None, PinType::String, CK::None, n));
	// Array → Set is answered too, since Set From Array exists: the drag that
	// used to be a dead end now costs one node.
	CHECK(conversionNodeFor(PinType::Float, CK::Array, PinType::Float, CK::Set, n));
	CHECK(n == NodeType::SetFromArray);
	// Container mismatches neither conversion solves.
	CHECK_FALSE(conversionNodeFor(PinType::Float, CK::Array, PinType::Float,  CK::Map,   n));
	CHECK_FALSE(conversionNodeFor(PinType::Float, CK::Set,   PinType::Float,  CK::None,  n));
	CHECK_FALSE(conversionNodeFor(PinType::Float, CK::None,  PinType::String, CK::Array, n));
	// Both container conversions change the CONTAINER; neither also converts the
	// elements, so a differing element type is a refusal, not a two-step job.
	CHECK_FALSE(conversionNodeFor(PinType::Float, CK::Set,   PinType::String, CK::Array, n));
	CHECK_FALSE(conversionNodeFor(PinType::Float, CK::Array, PinType::String, CK::Set,   n));
}

TEST_CASE("a refused Float → String wire builds a To String node between the two")
{
	Graph g;
	Variable out; out.name = "out"; out.type = PinType::String;
	g.variables.push_back(out);
	Node ev; ev.type = NodeType::Event; ev.s = "Go";
	const int e = g.addNode(std::move(ev));
	Node cf; cf.type = NodeType::ConstFloat; cf.f[0] = 3.9f; cf.x = 100.0f; cf.y = 40.0f;
	const int c = g.addNode(std::move(cf));
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::String;
	sv.x = 300.0f; sv.y = 80.0f;
	const int s = g.addNode(std::move(sv));
	REQUIRE(g.connect(e, 0, s, 0));

	// A Float at a String pin is refused outright — that used to be the end of it.
	REQUIRE_FALSE(g.connect(c, 0, s, 2));
	REQUIRE(connectWithConversion(g, c, 0, s, 2));
	REQUIRE(g.nodes.size() == 4);

	const Node* conv = nodeOfType(g, NodeType::ToString);
	REQUIRE(conv != nullptr);
	CHECK(conv->x == doctest::Approx(200.0f));    // half-way down the wire
	CHECK(conv->y == doctest::Approx(60.0f));
	// Through the node, never straight across.
	CHECK(linkCount(g, c, conv->id) == 1);
	CHECK(linkCount(g, conv->id, s) == 1);
	CHECK(linkCount(g, c, s) == 0);

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Go", 0, {});
	const Value v = rt.getVariable(id, "out");
	CHECK(v.type == PinType::String);
	CHECK(v.s == "3.9");                          // "%g", the To String node's rule
}

TEST_CASE("an Enum at a String pin gets an Enum to String that knows the definition")
{
	SUBCASE("straight off a typed pin")
	{
		Graph g;
		Variable label; label.name = "label"; label.type = PinType::String;
		g.variables.push_back(label);
		Node ce; ce.type = NodeType::ConstEnum; ce.typeName = "Types/Mood.henum"; ce.f[0] = 1.0f;
		const int c = g.addNode(std::move(ce));
		Node sv; sv.type = NodeType::SetVariable; sv.s = "label"; sv.propType = PinType::String;
		const int s = g.addNode(std::move(sv));

		REQUIRE_FALSE(g.connect(c, 0, s, 2));
		REQUIRE(connectWithConversion(g, c, 0, s, 2));
		const Node* conv = nodeOfType(g, NodeType::EnumToString);
		REQUIRE(conv != nullptr);
		CHECK(conv->typeName == "Types/Mood.henum");
	}
	SUBCASE("off a pin that names no definition of its own")
	{
		// The editor leaves a Get Variable node's typeName empty; the declaration
		// is what knows the enum. So the spawned node cannot copy it off the pin
		// and has to learn it the way every other bare node does — along the wire.
		Graph g;
		Variable mood; mood.name = "mood"; mood.type = PinType::Enum;
		mood.typeName = "Types/Mood.henum";
		g.variables.push_back(mood);
		Variable label; label.name = "label"; label.type = PinType::String;
		g.variables.push_back(label);
		Node gv; gv.type = NodeType::GetVariable; gv.s = "mood"; gv.propType = PinType::Enum;
		const int r = g.addNode(std::move(gv));
		Node sv; sv.type = NodeType::SetVariable; sv.s = "label"; sv.propType = PinType::String;
		const int s = g.addNode(std::move(sv));

		REQUIRE_FALSE(g.connect(r, 0, s, 2));
		REQUIRE(connectWithConversion(g, r, 0, s, 2));
		const Node* conv = nodeOfType(g, NodeType::EnumToString);
		REQUIRE(conv != nullptr);
		CHECK(conv->typeName == "Types/Mood.henum");
	}
}

TEST_CASE("a pair with no conversion node stays unconnected, and spawns nothing")
{
	Graph g;
	Variable out; out.name = "out"; out.type = PinType::Float;
	g.variables.push_back(out);
	Node gs; gs.type = NodeType::GetSelf;
	const int self = g.addNode(std::move(gs));
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Float;
	const int s = g.addNode(std::move(sv));

	CHECK_FALSE(g.connect(self, 0, s, 2));            // Object into a Float pin
	CHECK_FALSE(connectWithConversion(g, self, 0, s, 2));
	CHECK(g.nodes.size() == 2);
	CHECK(g.links.empty());

	// Exec pins are none of a conversion's business, at either end — not even a
	// perfectly legal exec wire, which is the plain connect's to make.
	Node ev; ev.type = NodeType::Event; ev.s = "Go";
	const int e = g.addNode(std::move(ev));
	CHECK_FALSE(connectWithConversion(g, e, 0, s, 0));
	CHECK_FALSE(connectWithConversion(g, self, 0, s, 0));
	// Nor does a node reach its own pins (Set Variable passes its value through).
	CHECK_FALSE(connectWithConversion(g, s, 3, s, 2));
	CHECK(g.nodes.size() == 3);
	CHECK(g.links.empty());
}

TEST_CASE("a conversion whose second half is refused leaves nothing behind")
{
	// Set<Alpha> → Array<Beta>: the lookup answers Set To Array (the kinds are
	// what it converts), but the definitions differ, so the node's own output is
	// refused at the destination. The half-built node must not survive that.
	Graph g;
	Node sm; sm.type = NodeType::SetMake; sm.propType = PinType::Enum;
	sm.typeName = "Types/Alpha.henum"; sm.container = ContainerKind::Set;
	const int mk = g.addNode(std::move(sm));
	Node al; al.type = NodeType::ArrayLength; al.propType = PinType::Enum;
	al.typeName = "Types/Beta.henum";
	const int len = g.addNode(std::move(al));

	REQUIRE_FALSE(g.connect(mk, 0, len, 0));
	CHECK_FALSE(connectWithConversion(g, mk, 0, len, 0));
	CHECK(g.nodes.size() == 2);
	CHECK(g.links.empty());

	// The same wire between two nodes of the SAME definition does get its node.
	Node al2; al2.type = NodeType::ArrayLength; al2.propType = PinType::Enum;
	al2.typeName = "Types/Alpha.henum";
	const int len2 = g.addNode(std::move(al2));
	REQUIRE(connectWithConversion(g, mk, 0, len2, 0));
	const Node* conv = nodeOfType(g, NodeType::SetToArray);
	REQUIRE(conv != nullptr);
	CHECK(conv->propType == PinType::Enum);       // both its pins are built from this
	CHECK(conv->typeName == "Types/Alpha.henum");
	CHECK(linkCount(g, mk, conv->id) == 1);
	CHECK(linkCount(g, conv->id, len2) == 1);
}

// ── Input Action nodes ───────────────────────────────────────────────────────
namespace
{
	// InputAction(action) — Pressed → Set pressedVar, Released → Set releasedVar.
	// Pins: execOut 0 = Pressed, execOut 1 = Released.
	int addInputAction(Graph& g, const std::string& action,
	                   const std::string& pressedVar, const std::string& releasedVar)
	{
		Node ia; ia.type = NodeType::InputAction; ia.s = action;
		const int a = g.addNode(std::move(ia));
		auto arm = [&](int outPin, const std::string& var)
		{
			Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
			const int c = g.addNode(std::move(cb));
			Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
			const int s = g.addNode(std::move(sv));
			REQUIRE(g.connect(a, outPin, s, 0));
			REQUIRE(g.connect(c, 0, s, 2));
		};
		arm(0, pressedVar);
		arm(1, releasedVar);
		return a;
	}
}

TEST_CASE("an Input Action node routes Pressed and Released to its own exec-outs")
{
	Graph g;
	for (const char* n : { "down", "up" })
	{ Variable v; v.name = n; v.type = PinType::Bool; g.variables.push_back(v); }
	addInputAction(g, "Jump", "down", "up");

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));

	// The host fires the ordinary named events — the node is matched against
	// them, which is what keeps Lua/Python and older graphs working unchanged.
	rt.fireEvent(id, "Input.Jump.Pressed", 0, {});
	CHECK(rt.getVariable(id, "down").b);
	CHECK_FALSE(rt.getVariable(id, "up").b);

	rt.fireEvent(id, "Input.Jump.Released", 0, {});
	CHECK(rt.getVariable(id, "up").b);

	// A different action, and the axis name of this one, reach neither chain.
	rt.setVariable(id, "down", Value::ofBool(false));
	rt.fireEvent(id, "Input.Fire.Pressed", 0, {});
	rt.fireEvent(id, "Input.Jump.Axis", 0, Value::ofFloat(1.0f));
	CHECK_FALSE(rt.getVariable(id, "down").b);
}

TEST_CASE("an axis Input Action has one chain and carries its value")
{
	Graph g;
	Variable v; v.name = "amount"; v.type = PinType::Float;
	g.variables.push_back(v);

	Node ia; ia.type = NodeType::InputAction; ia.s = "Move"; ia.hasArg = true;  // axis
	const int a = g.addNode(std::move(ia));
	Node sv; sv.type = NodeType::SetVariable; sv.s = "amount"; sv.propType = PinType::Float;
	const int s = g.addNode(std::move(sv));
	REQUIRE(g.connect(a, 0, s, 0));
	// data-out 0 is the axis Value (execOut 0 comes first in the unified layout).
	REQUIRE(g.connect(a, 1, s, 2));

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Input.Move.Axis", 0, Value::ofFloat(0.75f));
	CHECK(rt.getVariable(id, "amount").f == doctest::Approx(0.75f));

	// An axis action has no press or release — those names reach nothing.
	rt.fireEvent(id, "Input.Move.Pressed", 0, Value::ofFloat(9.0f));
	CHECK(rt.getVariable(id, "amount").f == doctest::Approx(0.75f));
}

TEST_CASE("an Input Action node is reported as handling its events")
{
	// eventBindingsOf is what says an instance cares about an event at all — a
	// handler missing from it exists and is never fired.
	Graph g;
	Variable v; v.name = "x"; v.type = PinType::Bool; g.variables.push_back(v);
	addInputAction(g, "Jump", "x", "x");

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));
	std::vector<std::string> names;
	for (const auto& b : rt.eventBindingsOf(id)) names.push_back(b.name);
	std::sort(names.begin(), names.end());
	CHECK(names == std::vector<std::string>{ "Input.Jump.Pressed", "Input.Jump.Released" });
}

TEST_CASE("a Real Time Delay runs on the unscaled clock, an ordinary one does not")
{
	// The pause case in miniature: a frame where game time stands still but
	// real time keeps moving. One Delay has to finish, the other must not.
	Graph g;
	for (const char* n : { "real", "game" })
	{ Variable v; v.name = n; v.type = PinType::Bool; g.variables.push_back(v); }

	// Two independent chains: Event → Delay(1s) → SetVariable(<var>, true).
	auto delayChain = [&g](const char* event, const char* var, bool realTime)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node d; d.type = NodeType::Delay;
		d.pinDefaults[0] = Value::ofFloat(1.0f);
		if (realTime) d.pinDefaults[1] = Value::ofBool(true);
		const int dl = g.addNode(d);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, dl, 0));    // Event exec-out → Delay exec-in
		REQUIRE(g.connect(dl, 1, s, 0));    // Delay "Completed" → Set exec-in
		REQUIRE(g.connect(c, 0, s, 2));     // ConstBool → Set Value
	};
	delayChain("Real", "real", /*realTime=*/true);
	delayChain("Game", "game", /*realTime=*/false);

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Real");
	rt.fireEvent(id, "Game");

	// Paused: game dt 0, real dt 1.2s.
	rt.update(0.0f, 1.2f);
	CHECK(rt.getVariable(id, "real").b);
	CHECK_FALSE(rt.getVariable(id, "game").b);   // still waiting out the pause

	// Resumed: the ordinary one finishes on game time.
	rt.update(1.2f, 1.2f);
	CHECK(rt.getVariable(id, "game").b);
}

TEST_CASE("one dt means one clock: a caller with no time scale keeps the old behaviour")
{
	// Every tool and test that predates the Real Time pin calls update(dt) with
	// a single number. Both kinds of Delay must still expire on it.
	Graph g;
	for (const char* n : { "real", "game" })
	{ Variable v; v.name = n; v.type = PinType::Bool; g.variables.push_back(v); }

	auto delayChain = [&g](const char* event, const char* var, bool realTime)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node d; d.type = NodeType::Delay;
		d.pinDefaults[0] = Value::ofFloat(1.0f);
		if (realTime) d.pinDefaults[1] = Value::ofBool(true);
		const int dl = g.addNode(d);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = var; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, dl, 0));
		REQUIRE(g.connect(dl, 1, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
	};
	delayChain("Real", "real", /*realTime=*/true);
	delayChain("Game", "game", /*realTime=*/false);

	Runtime rt;
	const InstanceId id = rt.add(std::move(g));
	rt.fireEvent(id, "Real");
	rt.fireEvent(id, "Game");
	rt.update(1.5f);
	CHECK(rt.getVariable(id, "real").b);
	CHECK(rt.getVariable(id, "game").b);
}

// ─── Migration hint: Create Widget makes a HIDDEN widget ─────────────────────
// Create Widget used to put its widget on screen by itself. It does not any
// more, and there is deliberately no automatic migration — so the analysis
// behind the warning has to be right about BOTH halves: it must find the graph
// that draws nothing, and it must stay quiet on every graph that works.

TEST_CASE("widgetCreatorsWithoutShow finds the Create Widget nobody shows")
{
	// Pins: CreateWidget execIn 0 / execOut 1 / Widget dataOut 2.
	//       ShowWidget  execIn 0 / execOut 1 / Widget dataIn  2.
	//       SetVariable execIn 0 / execOut 1 / Value dataIn 2 / Value dataOut 3.
	//       GetVariable Value dataOut 0.
	SUBCASE("wired straight into Show Widget: silent")
	{
		Graph g;
		Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
		const int c = g.addNode(cw);
		Node sw; sw.type = NodeType::ShowWidget;
		const int s = g.addNode(sw);
		REQUIRE(g.connect(c, 1, s, 0));
		REQUIRE(g.connect(c, 2, s, 2));
		CHECK(widgetCreatorsWithoutShow(g).empty());
	}
	SUBCASE("Widget output goes nowhere: reported")
	{
		Graph g;
		Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
		const int c = g.addNode(cw);
		// BEFORE THE CHANGE: this graph put its widget on screen. Now it draws
		// nothing, which is exactly what the hint has to say out loud.
		const auto found = widgetCreatorsWithoutShow(g);
		REQUIRE(found.size() == 1);
		CHECK(found[0] == c);
	}
	SUBCASE("parked in a variable and shown from another event: silent")
	{
		Graph g;
		Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
		const int c = g.addNode(cw);
		Node sv; sv.type = NodeType::SetVariable; sv.s = "panel"; sv.propType = PinType::Ref;
		const int v = g.addNode(sv);
		Node gv; gv.type = NodeType::GetVariable; gv.s = "panel"; gv.propType = PinType::Ref;
		const int r = g.addNode(gv);
		Node sw; sw.type = NodeType::ShowWidget;
		const int s = g.addNode(sw);
		REQUIRE(g.connect(c, 2, v, 2));   // Create → Set Variable
		REQUIRE(g.connect(r, 0, s, 2));   // Get Variable → Show Widget
		CHECK(widgetCreatorsWithoutShow(g).empty());
	}
	SUBCASE("only hidden and destroyed: reported")
	{
		Graph g;
		Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
		const int c = g.addNode(cw);
		Node hw; hw.type = NodeType::HideWidget;    const int h = g.addNode(hw);
		Node dw; dw.type = NodeType::DestroyWidget; const int d = g.addNode(dw);
		REQUIRE(g.connect(c, 2, h, 2));
		REQUIRE(g.connect(c, 2, d, 2));
		const auto found = widgetCreatorsWithoutShow(g);
		REQUIRE(found.size() == 1);
		CHECK(found[0] == c);
	}
	SUBCASE("handed to something this cannot read: silent")
	{
		// A widget passed to a function (or to Show Modal Widget, or to a node
		// added after this was written) has left the part of the graph the
		// analysis can follow. A hint that fires on a working graph is a hint
		// people learn to skip, so silence is the answer.
		Graph g;
		Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
		const int c = g.addNode(cw);
		Node fc; fc.type = NodeType::FunctionCall; fc.s = "Present";
		FuncParam p; p.name = "Widget"; p.type = PinType::Ref;
		fc.params.push_back(p);
		const int f = g.addNode(fc);
		// FunctionCall: execIn 0 / execOut 1 / first param dataIn 2.
		REQUIRE(g.connect(c, 2, f, 2));
		CHECK(widgetCreatorsWithoutShow(g).empty());
	}
}

namespace {
	std::vector<std::string> g_hcWarnings;
	void hcWarnSink(HE::LogLevel level, const char* msg, void*)
	{ if (level == HE::LogLevel::Warning) g_hcWarnings.emplace_back(msg ? msg : ""); }

	int unshownWidgetWarnings()
	{
		int n = 0;
		for (const auto& m : g_hcWarnings)
			if (m.find("creates a HIDDEN widget") != std::string::npos) ++n;
		return n;
	}
}

TEST_CASE("Registering a graph warns once per class about an unshown Create Widget")
{
	// The decision was a hint, not a repair: the runtime says what is wrong when
	// the graph arrives, and leaves the graph alone. Said once per class — a
	// class spawned a hundred times has one thing wrong with it.
	g_hcWarnings.clear();
	Logger::setSink(&hcWarnSink, nullptr);

	Graph g;
	Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
	g.addNode(cw);

	Runtime rt;
	ClassIdentity cls; cls.key = "Content/Menu.hasset";
	rt.add(g, {}, cls);
	rt.add(g, {}, cls);          // a second instance of the SAME class
	rt.add(g, {}, cls);

	Logger::setSink(nullptr, nullptr);
	CHECK(unshownWidgetWarnings() == 1);
	bool named = false;
	for (const auto& m : g_hcWarnings)
		if (m.find("Content/Menu.hasset") != std::string::npos
		    && m.find("UI/Root.hasset") != std::string::npos) named = true;
	// The message has to say WHICH graph and WHICH widget, or it is a riddle in
	// a project with forty of them.
	CHECK(named);
}

TEST_CASE("A graph that shows its widget registers without a word")
{
	g_hcWarnings.clear();
	Logger::setSink(&hcWarnSink, nullptr);

	Graph g;
	Node cw; cw.type = NodeType::CreateWidget; cw.s = "UI/Root.hasset";
	const int c = g.addNode(cw);
	Node sw; sw.type = NodeType::ShowWidget;
	const int s = g.addNode(sw);
	REQUIRE(g.connect(c, 1, s, 0));
	REQUIRE(g.connect(c, 2, s, 2));

	Runtime rt;
	ClassIdentity cls; cls.key = "Content/Good.hasset";
	rt.add(g, {}, cls);

	Logger::setSink(nullptr, nullptr);
	CHECK(unshownWidgetWarnings() == 0);
}

// ═══ Execution trace (node highlighting / console → node) ═══════════════════

namespace
{
	struct TraceHit { InstanceId inst; std::string key; size_t level; int node; };

	// Event(event) exec → Print(ConstString). Returns {event id, print id}.
	// Print: execIn 0 / execOut 1 / Text dataIn 2; ConstString dataOut 0.
	std::pair<int, int> eventPrints(Graph& g, const std::string& event, const std::string& text)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event; ev.elem = 0;
		const int e = g.addNode(ev);
		Node cs; cs.type = NodeType::ConstString; cs.s = text;
		const int c = g.addNode(cs);
		Node pr; pr.type = NodeType::Print;
		const int p = g.addNode(pr);
		REQUIRE(g.connect(e, 0, p, 0));
		REQUIRE(g.connect(c, 0, p, 2));
		return { e, p };
	}
}

TEST_CASE("exec trace: the listener hears the entry node and every exec node, with the class key")
{
	Graph g;
	const auto [e, p] = eventPrints(g, "Ping", "hello");

	Runtime rt;
	std::vector<TraceHit> hits;
	rt.setExecListener([&](InstanceId inst, const std::string& key, size_t level, int node)
	{ hits.push_back({ inst, key, level, node }); });
	ClassIdentity cls; cls.key = "Content/Traced.hasset";
	const InstanceId id = rt.add(g, {}, cls);

	// Nothing runs at registration.
	CHECK(hits.empty());
	CHECK(currentExecSite().nodeId == 0);

	rt.fireEvent(id, "Ping");
	REQUIRE(hits.size() == 2);
	CHECK(hits[0].node == e);          // the Event node itself, first
	CHECK(hits[1].node == p);          // then the Print it drives
	for (const TraceHit& h : hits)
	{
		CHECK(h.inst  == id);
		CHECK(h.key   == "Content/Traced.hasset");
		CHECK(h.level == 0);
	}
	// Between runs the thread is inside no node.
	CHECK(currentExecSite().nodeId == 0);

	// An event nobody handles leaves no trace — and neither does a pure read.
	hits.clear();
	rt.fireEvent(id, "Nothing");
	CHECK(hits.empty());
}

TEST_CASE("exec trace: a log line written by a node arrives at the sink while that node is current")
{
	Graph g;
	const auto [e, p] = eventPrints(g, "Ping", "from the print node");

	// The console's mechanism, without the console: a sink reads the site.
	// The site's classKey points into the running Context and is only good
	// for the duration of the node (HorizonCode.h) — so the sink copies it,
	// which is exactly what the console's sink has to do too.
	struct Seen { ExecSite site; std::string key; std::string msg; };
	static std::vector<Seen> s_seen;
	s_seen.clear();
	const int sink = HE::Log::addSink([](const HE::Log::Record& r, void*)
	{
		if (r.category != HE::Log::Cat::HorizonCode) return;
		s_seen.push_back({ currentExecSite(), currentExecSite().classKey,
		                   r.message ? r.message : "" });
	}, nullptr);

	Runtime rt;
	ClassIdentity cls; cls.key = "Content/Traced.hasset";
	const InstanceId id = rt.add(g, {}, cls);
	rt.fireEvent(id, "Ping");
	HE::Log::removeSink(sink);

	REQUIRE(s_seen.size() >= 1);
	const Seen* mine = nullptr;
	for (const Seen& s : s_seen) if (s.msg.find("from the print node") != std::string::npos) mine = &s;
	REQUIRE(mine != nullptr);
	CHECK(mine->site.nodeId   == p);
	CHECK(mine->site.instance == id);
	CHECK(mine->key == "Content/Traced.hasset");
	(void)e;
}

TEST_CASE("exec trace: a nested call restores the caller's site and names the callee's class")
{
	// Caller: Event(Go) → CallExternal(Target = GetSelf... no: a SECOND instance)
	// is cumbersome to wire; the Runtime's callFunction from inside a node is
	// what matters, so use Call Function (Ref) on a Ref variable.
	Graph callee;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Work"; fe.access = 0;
	const int f = callee.addNode(fe);
	Node cs; cs.type = NodeType::ConstString; cs.s = "inner";
	const int c = callee.addNode(cs);
	Node pr; pr.type = NodeType::Print;
	const int p = callee.addNode(pr);
	REQUIRE(callee.connect(f, 0, p, 0));
	REQUIRE(callee.connect(c, 0, p, 2));

	Graph caller;
	Variable rv; rv.name = "other"; rv.type = PinType::Ref;
	caller.variables.push_back(rv);
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.elem = 0;
	const int e = caller.addNode(ev);
	Node gv; gv.type = NodeType::GetVariable; gv.s = "other"; gv.propType = PinType::Ref;
	const int gvId = caller.addNode(gv);
	Node ce; ce.type = NodeType::CallExternal; ce.s = "Work";
	const int ceId = caller.addNode(ce);
	Node cs2; cs2.type = NodeType::ConstString; cs2.s = "after";
	const int c2 = caller.addNode(cs2);
	Node pr2; pr2.type = NodeType::Print;
	const int p2 = caller.addNode(pr2);
	REQUIRE(caller.connect(e, 0, ceId, 0));      // exec
	REQUIRE(caller.connect(gvId, 0, ceId, 2));   // Ref → Target
	REQUIRE(caller.connect(ceId, 1, p2, 0));     // exec on
	REQUIRE(caller.connect(c2, 0, p2, 2));

	Runtime rt;
	std::vector<TraceHit> hits;
	rt.setExecListener([&](InstanceId inst, const std::string& key, size_t level, int node)
	{ hits.push_back({ inst, key, level, node }); });
	ClassIdentity ccls; ccls.key = "Content/Callee.hasset";
	const InstanceId calleeId = rt.add(callee, {}, ccls);
	ClassIdentity rcls; rcls.key = "Content/Caller.hasset";
	const InstanceId callerId = rt.add(caller, {}, rcls);
	rt.setVariable(callerId, "other", Value::ofRef(calleeId));

	// The site the sink sees for each Print, in order.
	// The site's classKey points into the running Context and is only good
	// for the duration of the node (HorizonCode.h) — so the sink copies it,
	// which is exactly what the console's sink has to do too.
	struct Seen { ExecSite site; std::string key; std::string msg; };
	static std::vector<Seen> s_seen;
	s_seen.clear();
	const int sink = HE::Log::addSink([](const HE::Log::Record& r, void*)
	{
		if (r.category != HE::Log::Cat::HorizonCode) return;
		s_seen.push_back({ currentExecSite(), currentExecSite().classKey,
		                   r.message ? r.message : "" });
	}, nullptr);
	rt.fireEvent(callerId, "Go");
	HE::Log::removeSink(sink);

	// Listener order: Event, CallExternal, (callee) FunctionEntry, Print, (caller) Print.
	REQUIRE(hits.size() == 5);
	CHECK(hits[0].node == e);    CHECK(hits[0].inst == callerId); CHECK(hits[0].key == "Content/Caller.hasset");
	CHECK(hits[1].node == ceId); CHECK(hits[1].inst == callerId);
	CHECK(hits[2].node == f);    CHECK(hits[2].inst == calleeId); CHECK(hits[2].key == "Content/Callee.hasset");
	CHECK(hits[3].node == p);    CHECK(hits[3].inst == calleeId);
	CHECK(hits[4].node == p2);   CHECK(hits[4].inst == callerId);

	// The inner Print logged under the callee's site, the outer under the
	// caller's — i.e. the site was restored when the nested Runner returned.
	REQUIRE(s_seen.size() == 2);
	CHECK(s_seen[0].msg.find("inner") != std::string::npos);
	CHECK(s_seen[0].site.instance == calleeId);
	CHECK(s_seen[0].site.nodeId   == p);
	CHECK(s_seen[1].msg.find("after") != std::string::npos);
	CHECK(s_seen[1].site.instance == callerId);
	CHECK(s_seen[1].site.nodeId   == p2);
	CHECK(currentExecSite().nodeId == 0);
}

TEST_CASE("exec trace: a handler on a base level is reported under the base's key")
{
	Graph base;
	const auto [be, bp] = eventPrints(base, "Ping", "base");
	Graph derived;
	const auto [de, dp] = eventPrints(derived, "Other", "derived");

	Runtime rt;
	std::vector<TraceHit> hits;
	rt.setExecListener([&](InstanceId inst, const std::string& key, size_t level, int node)
	{ hits.push_back({ inst, key, level, node }); });
	ClassIdentity cls; cls.key = "Content/Child.hasset"; cls.chain = { "Content/Base.hasset" };
	const InstanceId id = rt.addLevels({ base, derived }, {}, cls);

	CHECK(rt.classKeyAtLevel(id, 0) == "Content/Base.hasset");
	CHECK(rt.classKeyAtLevel(id, 1) == "Content/Child.hasset");
	CHECK(rt.classKeyAtLevel(id, 2).empty());
	CHECK(rt.classKeyAtLevel(999, 0).empty());

	rt.fireEvent(id, "Ping");
	REQUIRE(hits.size() == 2);
	CHECK(hits[0].node == be); CHECK(hits[0].key == "Content/Base.hasset");  CHECK(hits[0].level == 0);
	CHECK(hits[1].node == bp); CHECK(hits[1].key == "Content/Base.hasset");

	hits.clear();
	rt.fireEvent(id, "Other");
	REQUIRE(hits.size() == 2);
	CHECK(hits[0].node == de); CHECK(hits[0].key == "Content/Child.hasset"); CHECK(hits[0].level == 1);
	CHECK(hits[1].node == dp);
}

// ═══ Breakpoints (stop / continue / step) ════════════════════════════════════
// The exec listener from the block above is the oracle throughout: a node the
// run stopped AT must not have been reported before Continue, and must be
// reported — in order — after it.

namespace
{
	// A Print node fed by a constant; returns the Print's id.
	// Print: execIn 0 / execOut 1 / Text dataIn 2; ConstString dataOut 0.
	int printNode(Graph& g, const std::string& text)
	{
		Node cs; cs.type = NodeType::ConstString; cs.s = text;
		const int c = g.addNode(cs);
		Node pr; pr.type = NodeType::Print;
		const int p = g.addNode(pr);
		REQUIRE(g.connect(c, 0, p, 2));
		return p;
	}
	// Chain `from`'s exec-out `pin` into `to`'s exec-in.
	void chain(Graph& g, int from, int pin, int to) { REQUIRE(g.connect(from, pin, to, 0)); }

	std::vector<int> nodesOf(const std::vector<TraceHit>& hits)
	{
		std::vector<int> out;
		for (const TraceHit& h : hits) out.push_back(h.node);
		return out;
	}

	// A runtime with the listener recording into `hits` and the predicate
	// stopping at every node in `bps`.
	struct DebugRig
	{
		Runtime               rt;
		std::vector<TraceHit> hits;
		std::set<int>         bps;
		DebugRig()
		{
			rt.setExecListener([this](InstanceId inst, const std::string& key, size_t level, int node)
			{ hits.push_back({ inst, key, level, node }); });
			rt.setBreakPredicate([this](InstanceId, const std::string&, size_t, int node)
			{ return bps.count(node) != 0; });
		}
	};
}

TEST_CASE("breakpoint: a run stops BEFORE the node, and Continue runs it and the rest")
{
	// Go → P1 → P2 → P3, breakpoint on P2.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p1 = printNode(g, "one"), p2 = printNode(g, "two"), p3 = printNode(g, "three");
	chain(g, e, 0, p1); chain(g, p1, 1, p2); chain(g, p2, 1, p3);

	DebugRig d;
	d.bps = { p2 };
	ClassIdentity cls; cls.key = "Content/Dbg.hasset";
	const InstanceId id = d.rt.add(g, {}, cls);

	std::vector<Runtime::SuspendedSite> told;
	d.rt.setSuspendListener([&](const Runtime::SuspendedSite& s) { told.push_back(s); });

	CHECK_FALSE(d.rt.isSuspended());
	d.rt.fireEvent(id, "Go");
	// P2 has not run: not reported, and nothing after it either.
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1 });
	REQUIRE(d.rt.isSuspended());
	REQUIRE(d.rt.suspendedSites().size() == 1);
	CHECK(d.rt.suspendedSites()[0].nodeId   == p2);
	CHECK(d.rt.suspendedSites()[0].instance == id);
	CHECK(d.rt.suspendedSites()[0].classKey == "Content/Dbg.hasset");
	REQUIRE(told.size() == 1);
	CHECK(told[0].nodeId == p2);
	REQUIRE(d.rt.suspendedRun(0) != nullptr);
	CHECK(d.rt.suspendedRun(0)->nodeId == p2);
	CHECK(d.rt.suspendedRun(1) == nullptr);
	// Between the stop and the continue the thread is inside no node.
	CHECK(currentExecSite().nodeId == 0);

	d.rt.debugContinue();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1, p2, p3 });
	CHECK_FALSE(d.rt.isSuspended());
	CHECK(d.rt.suspendedRun(0) == nullptr);

	// The breakpoint is still there: the next fire stops again.
	d.hits.clear();
	d.rt.fireEvent(id, "Go");
	CHECK(d.rt.isSuspended());
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1 });
	// Abort drops it without running the rest.
	d.rt.debugAbort();
	CHECK_FALSE(d.rt.isSuspended());
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1 });
	// No breakpoints → the run goes straight through, as before.
	d.bps.clear();
	d.hits.clear();
	d.rt.fireEvent(id, "Go");
	CHECK_FALSE(d.rt.isSuspended());
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1, p2, p3 });
}

TEST_CASE("breakpoint: on the Event node itself, the chain has not started; Continue starts it")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p1 = printNode(g, "one"), p2 = printNode(g, "two");
	chain(g, e, 0, p1); chain(g, p1, 1, p2);

	DebugRig d;
	d.bps = { e };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	CHECK(d.hits.empty());
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedSites()[0].nodeId == e);
	d.rt.debugContinue();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1, p2 });
	CHECK_FALSE(d.rt.isSuspended());
}

TEST_CASE("breakpoint: Step runs exactly one node and stops at the next; the last step ends the run")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p1 = printNode(g, "one"), p2 = printNode(g, "two"), p3 = printNode(g, "three");
	chain(g, e, 0, p1); chain(g, p1, 1, p2); chain(g, p2, 1, p3);

	DebugRig d;
	d.bps = { p1 };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedSites()[0].nodeId == p1);

	d.rt.debugStep();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1 });
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedSites()[0].nodeId == p2);

	d.rt.debugStep();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1, p2 });
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedSites()[0].nodeId == p3);

	// The chain ends after P3: nothing to stop at, the run is over.
	d.rt.debugStep();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1, p2, p3 });
	CHECK_FALSE(d.rt.isSuspended());
	// Step with nothing stopped is a no-op.
	d.rt.debugStep();
	CHECK_FALSE(d.rt.isSuspended());
}

TEST_CASE("breakpoint: Break Next stops at the next node anywhere, once")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p1 = printNode(g, "one");
	chain(g, e, 0, p1);

	DebugRig d;   // no breakpoints at all
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	CHECK_FALSE(d.rt.isSuspended());

	d.rt.debugBreakNext();
	CHECK(d.rt.debugBreakNextArmed());
	d.hits.clear();
	d.rt.fireEvent(id, "Go");
	// The first node a run reaches is its entry.
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedSites()[0].nodeId == e);
	CHECK_FALSE(d.rt.debugBreakNextArmed());
	d.rt.debugContinue();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, p1 });
	// Disarmed: the next run goes through.
	d.rt.fireEvent(id, "Go");
	CHECK_FALSE(d.rt.isSuspended());
	// Taken back by hand before anything ran: the same.
	d.rt.debugBreakNext();
	d.rt.debugBreakNext(false);
	CHECK_FALSE(d.rt.debugBreakNextArmed());
	d.rt.fireEvent(id, "Go");
	CHECK_FALSE(d.rt.isSuspended());
}

TEST_CASE("breakpoint: a Sequence's second output still fires after a stop in its first")
{
	// Go → Seq; Then 0 → A → B ; Then 1 → C. Breakpoint on B.
	// Sequence: execIn 0 / Then 0 = 1 / Then 1 = 2.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node sq; sq.type = NodeType::Sequence; const int seq = g.addNode(sq);
	const int a = printNode(g, "A"), b = printNode(g, "B"), c = printNode(g, "C");
	chain(g, e, 0, seq); chain(g, seq, 1, a); chain(g, a, 1, b); chain(g, seq, 2, c);

	DebugRig d;
	d.bps = { b };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, seq, a });
	REQUIRE(d.rt.isSuspended());
	REQUIRE(d.rt.suspendedRun(0)->outer.size() == 1);
	CHECK(d.rt.suspendedRun(0)->outer[0].kind == SuspendedRun::Frame::Kind::Sequence);
	CHECK(d.rt.suspendedRun(0)->outer[0].next == 1);

	d.rt.debugContinue();
	// B ran, then the Sequence's Then 1 — which a Delay would have lost.
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, seq, a, b, c });
	CHECK_FALSE(d.rt.isSuspended());
}

TEST_CASE("breakpoint: a For Each resumes with the remaining iterations, stopping in each, then Done")
{
	// Go → ForEach(list) ; Body → Set seen = Append(seen, Element) ; Done → D.
	// ForEach: execIn 0, Body 1, Done 2, Array-in 3, Element-out 4, Index-out 5.
	Graph g;
	{ Variable v; v.name = "list"; v.type = PinType::String; v.isArray = true;
	  v.defaultItems = { Value::ofString("a"), Value::ofString("b"), Value::ofString("c") };
	  g.variables.push_back(v); }
	{ Variable v; v.name = "seen"; v.type = PinType::String; v.isArray = true; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node gl; gl.type = NodeType::GetVariable; gl.s = "list"; gl.propType = PinType::String; gl.isArray = true;
	const int getList = g.addNode(gl);
	Node fe; fe.type = NodeType::ForEach; fe.propType = PinType::String; fe.isArray = true;
	const int loop = g.addNode(fe);
	Node gs; gs.type = NodeType::GetVariable; gs.s = "seen"; gs.propType = PinType::String; gs.isArray = true;
	const int getSeen = g.addNode(gs);
	Node ad; ad.type = NodeType::ArrayAdd; ad.propType = PinType::String; const int add = g.addNode(ad);
	Node ss; ss.type = NodeType::SetVariable; ss.s = "seen"; ss.propType = PinType::String; ss.isArray = true;
	const int setSeen = g.addNode(ss);
	const int done = printNode(g, "done");
	REQUIRE(g.connect(getList, 0, loop, 3));
	REQUIRE(g.connect(getSeen, 0, add, 0));
	REQUIRE(g.connect(loop, 4, add, 1));
	REQUIRE(g.connect(add, 2, setSeen, 2));
	chain(g, e, 0, loop); chain(g, loop, 1, setSeen); chain(g, loop, 2, done);

	auto seen = [&](Runtime& rt, InstanceId id)
	{
		std::vector<std::string> out;
		for (const Value& v : rt.getVariable(id, "seen").items) out.push_back(v.s);
		return out;
	};

	DebugRig d;
	d.bps = { setSeen };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	REQUIRE(d.rt.isSuspended());
	CHECK(seen(d.rt, id).empty());
	REQUIRE(d.rt.suspendedRun(0)->outer.size() == 1);
	CHECK(d.rt.suspendedRun(0)->outer[0].kind == SuspendedRun::Frame::Kind::Loop);
	CHECK(d.rt.suspendedRun(0)->outer[0].next == 1);
	CHECK(d.rt.suspendedRun(0)->outer[0].collection.items.size() == 3);

	// Each Continue writes one element and stops on the next iteration's Set.
	d.rt.debugContinue();
	CHECK(seen(d.rt, id) == std::vector<std::string>{ "a" });
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedRun(0)->outer[0].next == 2);
	d.rt.debugContinue();
	CHECK(seen(d.rt, id) == std::vector<std::string>{ "a", "b" });
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.suspendedRun(0)->outer[0].next == 3);
	d.rt.debugContinue();
	CHECK(seen(d.rt, id) == std::vector<std::string>{ "a", "b", "c" });
	CHECK_FALSE(d.rt.isSuspended());
	// Done fired last.
	REQUIRE(!d.hits.empty());
	CHECK(d.hits.back().node == done);
}

TEST_CASE("breakpoint: a stop inside a called function keeps its frame, and Continue delivers the results")
{
	// Go → Passthrough(21) → out = y.  Inside: Entry → Print "inner" → Return.
	Graph g;
	{ Variable v; v.name = "out"; v.type = PinType::Int; g.variables.push_back(v); }
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Passthrough";
	fe.params = { { "x", PinType::Int } }; fe.results = { { "y", PinType::Int } };
	const int feId = g.addNode(fe);
	const int inner = printNode(g, "inner");
	Node fr; fr.type = NodeType::FunctionReturn; fr.s = "Passthrough"; const int frId = g.addNode(fr);
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node ci; ci.type = NodeType::ConstInt; ci.f[0] = 21; const int c = g.addNode(ci);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "Passthrough"; const int fcId = g.addNode(fc);
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Int; const int s = g.addNode(sv);
	const int after = printNode(g, "after");
	syncFunctionSignatures(g);
	chain(g, feId, 0, inner); chain(g, inner, 1, frId);
	REQUIRE(g.connect(feId, 1, frId, 1));      // entry.x → return.y
	chain(g, e, 0, fcId);
	REQUIRE(g.connect(c, 0, fcId, 2));
	chain(g, fcId, 1, s); chain(g, s, 1, after);
	REQUIRE(g.connect(fcId, 3, s, 2));

	DebugRig d;
	d.bps = { inner };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go");
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, fcId, feId });
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.getVariable(id, "out").i == 0);        // the caller's chain has NOT gone on
	// The function's frame travels with the stop — its argument is readable.
	const SuspendedRun* run = d.rt.suspendedRun(0);
	REQUIRE(run != nullptr);
	REQUIRE(run->callStack.size() == 1);
	CHECK(run->callStack[0].fnEntryId == feId);
	REQUIRE(run->callStack[0].args.size() == 1);
	CHECK(run->callStack[0].args[0].i == 21);
	REQUIRE(run->outer.size() == 1);
	CHECK(run->outer[0].kind == SuspendedRun::Frame::Kind::Call);
	CHECK(run->outer[0].nodeId == fcId);

	d.rt.debugContinue();
	CHECK(nodesOf(d.hits) == std::vector<int>{ e, fcId, feId, inner, frId, s, after });
	CHECK(d.rt.getVariable(id, "out").i == 21);       // results came back through the call
	CHECK_FALSE(d.rt.isSuspended());
}

TEST_CASE("breakpoint: the event argument survives the stop")
{
	// Go(Int) → P → out = arg. Breakpoint on the Set.
	Graph g;
	{ Variable v; v.name = "out"; v.type = PinType::Int; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; ev.hasArg = true; ev.propType = PinType::Int;
	const int e = g.addNode(ev);
	const int p = printNode(g, "p");
	Node sv; sv.type = NodeType::SetVariable; sv.s = "out"; sv.propType = PinType::Int; const int s = g.addNode(sv);
	chain(g, e, 0, p); chain(g, p, 1, s);
	REQUIRE(g.connect(e, 1, s, 2));   // Event's Value → Set's Value

	DebugRig d;
	d.bps = { s };
	const InstanceId id = d.rt.add(g);
	d.rt.fireEvent(id, "Go", 0, Value::ofInt(7));
	REQUIRE(d.rt.isSuspended());
	CHECK(d.rt.getVariable(id, "out").i == 0);
	d.rt.debugContinue();
	CHECK(d.rt.getVariable(id, "out").i == 7);
}

TEST_CASE("breakpoint: a stopped run dies with its instance, and clear() drops them all")
{
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	const int p = printNode(g, "p");
	chain(g, e, 0, p);

	DebugRig d;
	d.bps = { p };
	const InstanceId a = d.rt.add(g);
	const InstanceId b = d.rt.add(g);
	d.rt.fireEvent(a, "Go");
	d.rt.fireEvent(b, "Go");
	REQUIRE(d.rt.suspendedSites().size() == 2);
	d.rt.remove(a);
	REQUIRE(d.rt.suspendedSites().size() == 1);
	CHECK(d.rt.suspendedSites()[0].instance == b);
	d.hits.clear();
	d.rt.debugContinue();
	CHECK(nodesOf(d.hits) == std::vector<int>{ p });
	CHECK(d.hits[0].inst == b);
	CHECK_FALSE(d.rt.isSuspended());

	d.rt.fireEvent(b, "Go");
	REQUIRE(d.rt.isSuspended());
	d.rt.clear();
	CHECK_FALSE(d.rt.isSuspended());
}

// ── Runtime errors are said, not swallowed ───────────────────────────────────
// Every path below used to return a default value or skip the node without a
// word, so the graph looked like one that does nothing. Now each of them
// reaches the log — as an Error for a call that could not happen and a run
// that was cut short, as a Warning (like Array Get's) for a write that went
// nowhere — with the node current, so the console row leads back to it.
namespace
{
	struct HcLogCapture
	{
		struct Seen { HE::LogLevel level; std::string msg; int node; };
		static std::vector<Seen> s_seen;
		int sink = 0;
		HcLogCapture()
		{
			s_seen.clear();
			sink = HE::Log::addSink([](const HE::Log::Record& r, void*)
			{
				if (r.category != HE::Log::Cat::HorizonCode || r.level < HE::LogLevel::Warning) return;
				s_seen.push_back({ r.level, r.message ? r.message : "", currentExecSite().nodeId });
			}, nullptr);
		}
		~HcLogCapture() { HE::Log::removeSink(sink); }
		int count(const char* needle, HE::LogLevel atLeast = HE::LogLevel::Warning) const
		{
			int n = 0;
			for (const Seen& s : s_seen)
				if (s.level >= atLeast && s.msg.find(needle) != std::string::npos) ++n;
			return n;
		}
		const Seen* first(const char* needle) const
		{
			for (const Seen& s : s_seen) if (s.msg.find(needle) != std::string::npos) return &s;
			return nullptr;
		}
	};
	std::vector<HcLogCapture::Seen> HcLogCapture::s_seen;
}

TEST_CASE("runtime errors: a Call Function naming a function no level has is an error, with the node current")
{
	// Go → Call Function "Gone" → Print after. "Gone" exists nowhere — the
	// function was renamed or deleted after the node was placed.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "Gone"; const int fcId = g.addNode(fc);
	const int after = printNode(g, "after");
	chain(g, e, 0, fcId); chain(g, fcId, 1, after);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	REQUIRE(seen.count("Call Function 'Gone'", HE::LogLevel::Error) == 1);
	// The row leads back to the call node, not to the event or the print.
	CHECK(seen.first("Call Function 'Gone'")->node == fcId);
	// Said once per call, not once per session: fire again, it is said again.
	rt.fireEvent(id, "Go");
	CHECK(seen.count("Call Function 'Gone'", HE::LogLevel::Error) == 2);
	// The chain went on past the skipped call: the run is not aborted for it.
	CHECK(rt.alive(id));
	(void)after;
}

TEST_CASE("runtime errors: a Call Function that resolves is not an error (negative control)")
{
	Graph g;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "Here"; fe.access = 0; const int feId = g.addNode(fe);
	const int inner = printNode(g, "inner");
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "Here"; const int fcId = g.addNode(fc);
	syncFunctionSignatures(g);
	chain(g, feId, 0, inner);
	chain(g, e, 0, fcId);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	CHECK(seen.count("Call Function", HE::LogLevel::Error) == 0);
	CHECK(seen.count("", HE::LogLevel::Error) == 0);
}

TEST_CASE("runtime errors: Array Set and Array Remove out of range are said, like Array Get's miss")
{
	// Go → Set list = ArraySet(list, 5, "x") → Set list = ArrayRemove(list, 7).
	// `list` has one element, so both indices are out of range.
	Graph g;
	{ Variable v; v.name = "list"; v.type = PinType::String; v.isArray = true;
	  v.defaultItems = { Value::ofString("a") };
	  g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node gl; gl.type = NodeType::GetVariable; gl.s = "list"; gl.propType = PinType::String; gl.isArray = true;
	const int getList = g.addNode(gl);
	Node i5; i5.type = NodeType::ConstInt; i5.f[0] = 5; const int c5 = g.addNode(i5);
	Node sx; sx.type = NodeType::ConstString; sx.s = "x"; const int cx = g.addNode(sx);
	// ArraySet: dataIns Array 0, Index 1, Value 2; dataOut Array 3.
	Node as; as.type = NodeType::ArraySet; as.propType = PinType::String; const int setAt = g.addNode(as);
	Node s1; s1.type = NodeType::SetVariable; s1.s = "list"; s1.propType = PinType::String; s1.isArray = true;
	const int set1 = g.addNode(s1);
	Node gl2 = gl; const int getList2 = g.addNode(gl2);
	Node i7; i7.type = NodeType::ConstInt; i7.f[0] = 7; const int c7 = g.addNode(i7);
	// ArrayRemove: dataIns Array 0, Index 1; dataOut Array 2.
	Node ar; ar.type = NodeType::ArrayRemove; ar.propType = PinType::String; const int removeAt = g.addNode(ar);
	Node s2 = s1; const int set2 = g.addNode(s2);
	REQUIRE(g.connect(getList, 0, setAt, 0));
	REQUIRE(g.connect(c5, 0, setAt, 1));
	REQUIRE(g.connect(cx, 0, setAt, 2));
	REQUIRE(g.connect(setAt, 3, set1, 2));
	REQUIRE(g.connect(getList2, 0, removeAt, 0));
	REQUIRE(g.connect(c7, 0, removeAt, 1));
	REQUIRE(g.connect(removeAt, 2, set2, 2));
	chain(g, e, 0, set1); chain(g, set1, 1, set2);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	REQUIRE(seen.count("Array Set index 5 out of range (size 1)") == 1);
	REQUIRE(seen.count("Array Remove index 7 out of range (size 1)") == 1);
	// Each names ITS node — the pure node, not the Set Variable that pulled on it.
	CHECK(seen.first("Array Set index")->node    == setAt);
	CHECK(seen.first("Array Remove index")->node == removeAt);
	// The semantics did not change: the array is left as it was.
	REQUIRE(rt.getVariable(id, "list").items.size() == 1);
	CHECK(rt.getVariable(id, "list").items[0].s == "a");
}

TEST_CASE("runtime errors: Divide by zero is said as an error, with the Divide node current, and still yields 0")
{
	// Go → Set q = Divide(5, 0). The divisor is 0: the result used to be a
	// silent 0, indistinguishable from a graph that computes 0 on purpose.
	Graph g;
	{ Variable v; v.name = "q"; v.type = PinType::Float; v.f[0] = 7.0f; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node a; a.type = NodeType::ConstFloat; a.f[0] = 5.0f; const int ca = g.addNode(a);
	Node b; b.type = NodeType::ConstFloat; b.f[0] = 0.0f; const int cb = g.addNode(b);
	Node dv; dv.type = NodeType::Divide; const int div = g.addNode(dv);
	Node sq; sq.type = NodeType::SetVariable; sq.s = "q"; sq.propType = PinType::Float;
	const int setQ = g.addNode(sq);
	REQUIRE(g.connect(ca, 0, div, 0));
	REQUIRE(g.connect(cb, 0, div, 1));
	REQUIRE(g.connect(div, 2, setQ, 2));   // Divide: dataIns A 0, B 1; dataOut 2
	chain(g, e, 0, setQ);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	REQUIRE(seen.count("Divide by zero", HE::LogLevel::Error) == 1);
	// The row leads back to the Divide node itself — the pure node, not the
	// Set Variable that pulled on it (that is what "Go to Node" opens).
	CHECK(seen.first("Divide by zero")->node == div);
	// The value did not change: 0, not NaN or inf, and the chain went on.
	CHECK(rt.getVariable(id, "q").f == 0.0f);
	CHECK(rt.alive(id));
	// Said per evaluation, not once per session. (A distinct line in between,
	// because the log folds identical consecutive records — Log.cpp — and the
	// sink never sees the fold.)
	HE_LOG_INFO(HorizonCode, "between the two runs");
	rt.fireEvent(id, "Go");
	CHECK(seen.count("Divide by zero", HE::LogLevel::Error) == 2);
}

TEST_CASE("runtime errors: a Divide with a non-zero divisor is not an error (negative control)")
{
	Graph g;
	{ Variable v; v.name = "q"; v.type = PinType::Float; g.variables.push_back(v); }
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node a; a.type = NodeType::ConstFloat; a.f[0] = 5.0f; const int ca = g.addNode(a);
	Node b; b.type = NodeType::ConstFloat; b.f[0] = 2.0f; const int cb = g.addNode(b);
	Node dv; dv.type = NodeType::Divide; const int div = g.addNode(dv);
	Node sq; sq.type = NodeType::SetVariable; sq.s = "q"; sq.propType = PinType::Float;
	const int setQ = g.addNode(sq);
	REQUIRE(g.connect(ca, 0, div, 0));
	REQUIRE(g.connect(cb, 0, div, 1));
	REQUIRE(g.connect(div, 2, setQ, 2));   // Divide: dataIns A 0, B 1; dataOut 2
	chain(g, e, 0, setQ);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	CHECK(rt.getVariable(id, "q").f == 2.5f);
	CHECK(seen.count("", HE::LogLevel::Warning) == 0);
}

TEST_CASE("runtime errors: a run cut short by the nesting limit says so once, as an error")
{
	// F calls F: local recursion with no exit. Every level nests one deeper
	// until the Runner's depth limit cuts the run — which used to be silent.
	Graph g;
	Node fe; fe.type = NodeType::FunctionEntry; fe.s = "F"; fe.access = 0; const int feId = g.addNode(fe);
	Node fc; fc.type = NodeType::FunctionCall; fc.s = "F"; const int fcId = g.addNode(fc);
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node fc0; fc0.type = NodeType::FunctionCall; fc0.s = "F"; const int fc0Id = g.addNode(fc0);
	syncFunctionSignatures(g);
	chain(g, feId, 0, fcId);
	chain(g, e, 0, fc0Id);

	Runtime rt;
	const InstanceId id = rt.add(g);
	HcLogCapture seen;
	rt.fireEvent(id, "Go");
	CHECK(rt.alive(id));
	// Once — not once per level on the way back out.
	CHECK(seen.count("limit hit", HE::LogLevel::Error) == 1);
	CHECK(seen.count("the run was aborted here", HE::LogLevel::Error) == 1);
	// A second run says it again: the once is per run. (A distinct line in
	// between, because the log itself folds identical consecutive records into
	// a "repeated N more times" note — Log.cpp — which the sink never sees.)
	HE_LOG_INFO(HorizonCode, "between the two runs");
	rt.fireEvent(id, "Go");
	CHECK(seen.count("limit hit", HE::LogLevel::Error) == 2);
}
