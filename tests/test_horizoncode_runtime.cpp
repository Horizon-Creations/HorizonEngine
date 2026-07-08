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
	svc.createObject = [&](const std::string& /*path*/) -> uint32_t {
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
	// CreateObject: execIn 0 / execOut 1 / Object dataOut 2.
	// DestroyObject: execIn 0 / execOut 1 / Object dataIn 2.
	Graph caller;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = caller.addNode(ev);
	Node co; co.type = NodeType::CreateObject; co.s = "MyClass"; const int c = caller.addNode(co);
	Node de; de.type = NodeType::DestroyObject; const int d = caller.addNode(de);
	REQUIRE(caller.connect(e, 0, c, 0)); // exec
	REQUIRE(caller.connect(c, 1, d, 0)); // exec
	REQUIRE(caller.connect(c, 2, d, 2)); // ref out → ref in

	const InstanceId callerId = rt.add(std::move(caller));
	rt.fireEvent(callerId, "Go");

	CHECK(createCount == 1);              // created once — the ref is cached, not re-run
	CHECK(builtAfterConstruct == true);   // the class's Construct ran
	CHECK(createdRef != 0);
	CHECK(destroyedRef == createdRef);    // the cached ref flowed into Destroy Object
	CHECK_FALSE(rt.alive(createdRef));    // and the instance is gone
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

	// Event Go → CreateWidget("hud.ui") → ShowWidget(<created id>).
	// CreateWidget: execIn 0 / execOut 1 / Widget dataOut 2.
	// ShowWidget:   execIn 0 / execOut 1 / Widget dataIn 2.
	Graph g;
	Node ev; ev.type = NodeType::Event; ev.s = "Go"; const int e = g.addNode(ev);
	Node cw; cw.type = NodeType::CreateWidget; cw.s = "hud.ui"; const int c = g.addNode(cw);
	Node sw; sw.type = NodeType::ShowWidgetId; const int s = g.addNode(sw);
	REQUIRE(g.connect(e, 0, c, 0)); // Event exec → CreateWidget exec-in
	REQUIRE(g.connect(c, 1, s, 0)); // CreateWidget exec-out → ShowWidget exec-in
	REQUIRE(g.connect(c, 2, s, 2)); // CreateWidget id → ShowWidget Widget

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
