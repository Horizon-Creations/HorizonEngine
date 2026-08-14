// WP0 gate of the HorizonCode → C++ codegen plan (docs/horizoncode-cpp-codegen-
// implementation-plan.md §4): the compiled-instance object model, proven with a
// HAND-WRITTEN CompiledInstance before any generator exists. The mock mirrors
// the shape the generator will emit — typed members, static var/event tables,
// name-switch reflection — and must be indistinguishable from an interpreted
// instance across every Runtime seam: fire/call, external access, bind/emit in
// both directions, GC, overflow store, event bindings.
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeCompiled.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	// Observations that must survive the instance's destruction.
	struct Probe
	{
		int constructs = 0, destructs = 0, clicks = 0;
	};

	// What hcgen would emit for a class with:
	//   public Float Score = 5, private Float Secret = 1,
	//   public Ref Child, public Ref[] Kids
	//   events Construct(0), OnClicked(elem 2), Ping(0), Destruct(0)
	//   functions AddScore(Amount)->Total (public), Hidden() (private)
	class MockCompiled : public CompiledInstance
	{
	public:
		explicit MockCompiled(Probe* probe = nullptr) : m_probe(probe) {}

		const char* classKey() const override { return "Content/Logic/Mock.hasset"; }

		const std::vector<CompiledVarInfo>& varInfos() const override
		{
			static const std::vector<CompiledVarInfo> kVars = {
				{ "Score",  PinType::Float, false, 0 },
				{ "Secret", PinType::Float, false, 1 },
				{ "Child",  PinType::Ref,   false, 0 },
				{ "Kids",   PinType::Ref,   true,  0 },
			};
			return kVars;
		}
		const std::vector<CompiledEventInfo>& eventInfos() const override
		{
			static const std::vector<CompiledEventInfo> kEvents = {
				{ "Construct", 0 }, { "OnClicked", 2 }, { "Ping", 0 }, { "Destruct", 0 },
			};
			return kEvents;
		}

		void fireEvent(const std::string& name, int elem, const Value& arg) override
		{
			if (name == "Construct" && m_probe) ++m_probe->constructs;
			if (name == "Destruct"  && m_probe) ++m_probe->destructs;
			if (name == "OnClicked" && (2 == 0 || 2 == elem) && m_probe) ++m_probe->clicks;
			if (name == "Ping")
			{
				// Behaves like: Score += arg; Emit Event "Pong" (arg = Score).
				v_Score += arg.f;
				if (m_ctx.emitEvent) m_ctx.emitEvent("Pong", Value::ofFloat(v_Score));
			}
		}

		bool callFunction(const std::string& name, bool requirePublic,
		                  const std::vector<Value>& args, std::vector<Value>* results) override
		{
			if (name == "AddScore")
			{
				v_Score += args.size() > 0 ? args[0].f : 0.0f;
				if (results) *results = { Value::ofFloat(v_Score) };
				return true;
			}
			if (name == "Hidden")
			{
				if (requirePublic) return false; // private
				v_Secret = 42.0f;
				return true;
			}
			return false;
		}

		Value getVariable(const std::string& name) const override
		{
			if (name == "Score")  return Value::ofFloat(v_Score);
			if (name == "Secret") return Value::ofFloat(v_Secret);
			if (name == "Child")  return Value::ofRef(v_Child);
			if (name == "Kids")
			{
				Value v; v.type = PinType::Ref; v.isArray = true;
				for (const uint32_t k : v_Kids) v.items.push_back(Value::ofRef(k));
				return v;
			}
			return {};
		}
		bool setVariable(const std::string& name, const Value& v) override
		{
			if (name == "Score")  { v_Score  = v.f;   return true; }
			if (name == "Secret") { v_Secret = v.f;   return true; }
			if (name == "Child")  { v_Child  = v.ref; return true; }
			if (name == "Kids")
			{
				v_Kids.clear();
				for (const auto& item : v.items) v_Kids.push_back(item.ref);
				return true;
			}
			return false;
		}
		void reseedVariables() override
		{
			v_Score = 5.0f; v_Secret = 1.0f; v_Child = 0; v_Kids.clear();
		}
		void collectRefs(std::vector<uint32_t>& out) const override
		{
			if (v_Child) out.push_back(v_Child);
			for (const uint32_t k : v_Kids) if (k) out.push_back(k);
		}

		// Test taps into the bound Context (what generated code calls internally).
		uint32_t ctxCreateObject(const std::string& path)
		{ return m_ctx.createObject ? m_ctx.createObject(path) : 0u; }
		Value ctxGetExternal(uint32_t target, const std::string& var)
		{ return m_ctx.getExternal ? m_ctx.getExternal(target, var) : Value{}; }
		void ctxSetExternal(uint32_t target, const std::string& var, const Value& v)
		{ if (m_ctx.setExternal) m_ctx.setExternal(target, var, v); }
		std::vector<Value> ctxCallExternal(uint32_t target, const std::string& fn,
		                                   const std::vector<Value>& args)
		{ return m_ctx.callExternal ? m_ctx.callExternal(target, fn, args) : std::vector<Value>{}; }
		Value ctxGameInstance()
		{ return m_ctx.getGameInstance ? m_ctx.getGameInstance() : Value{}; }

	private:
		Probe*   m_probe = nullptr;
		float    v_Score  = 5.0f;
		float    v_Secret = 1.0f;
		uint32_t v_Child  = 0;
		std::vector<uint32_t> v_Kids;
	};

	// Interpreted helper: graph with a bool var set true by an Event node.
	Graph interpretedPongCatcher()
	{
		Graph g;
		Variable v; v.name = "gotPong"; v.type = PinType::Bool;
		g.variables.push_back(v);
		Node ev; ev.type = NodeType::Event; ev.s = "Pong"; ev.elem = 0;
		const int e = g.addNode(ev);
		Node cb; cb.type = NodeType::ConstBool; cb.f[0] = 1.0f;
		const int c = g.addNode(cb);
		Node sv; sv.type = NodeType::SetVariable; sv.s = "gotPong"; sv.propType = PinType::Bool;
		const int s = g.addNode(sv);
		REQUIRE(g.connect(e, 0, s, 0));
		REQUIRE(g.connect(c, 0, s, 2));
		return g;
	}
}

TEST_CASE("Compiled instance registers, fires events and takes function calls like an interpreted one")
{
	Runtime rt;
	Probe probe;
	const InstanceId id = rt.addCompiled(makeCompiled<MockCompiled>(&probe));
	REQUIRE(id != 0);
	CHECK(rt.alive(id));

	rt.fireEvent(id, "Construct");
	CHECK(probe.constructs == 1);

	// Element filtering is the instance's own business (fireEvent forwards elem).
	rt.fireEvent(id, "OnClicked", 7);
	CHECK(probe.clicks == 0);
	rt.fireEvent(id, "OnClicked", 2);
	CHECK(probe.clicks == 1);

	std::vector<Value> results;
	CHECK(rt.callFunction(id, "AddScore", true, { Value::ofFloat(2.5f) }, &results));
	REQUIRE(results.size() == 1);
	CHECK(results[0].f == doctest::Approx(7.5f));

	// Private functions honor requirePublic across the class boundary.
	CHECK_FALSE(rt.callFunction(id, "Hidden", true));
	CHECK(rt.callFunction(id, "Hidden", false));

	// addCompiled(null) must not mint an id.
	CHECK(rt.addCompiled(CompiledPtr{}) == 0);
}

TEST_CASE("Compiled variables: reflection, overflow store for undeclared names, snapshot, reseed")
{
	Runtime rt;
	const InstanceId id = rt.addCompiled(makeCompiled<MockCompiled>());

	// Declared members answer through reflection (generated ctor defaults).
	CHECK(rt.getVariable(id, "Score").f == doctest::Approx(5.0f));
	rt.setVariable(id, "Score", Value::ofFloat(9.0f));
	CHECK(rt.getVariable(id, "Score").f == doctest::Approx(9.0f));

	// Set on an UNDECLARED name creates an overflow entry (§3.4 semantics).
	rt.setVariable(id, "improvised", Value::ofInt(11));
	CHECK(rt.getVariable(id, "improvised").i == 11);
	CHECK(rt.variablesOf(id).count("improvised") == 1);   // overflow store only
	CHECK(rt.variablesOf(id).count("Score") == 0);        // members are not in it

	// The snapshot merges both worlds.
	const auto snap = rt.variablesSnapshot(id);
	REQUIRE(snap.count("Score") == 1);
	REQUIRE(snap.count("Secret") == 1);
	REQUIRE(snap.count("improvised") == 1);
	CHECK(snap.at("Score").f == doctest::Approx(9.0f));
	CHECK(snap.at("improvised").i == 11);

	// Reseed: members back to declared defaults, overflow cleared.
	rt.reseedVariables(id);
	CHECK(rt.getVariable(id, "Score").f == doctest::Approx(5.0f));
	CHECK(rt.getVariable(id, "improvised").i == 0);
	CHECK(rt.variablesOf(id).empty());
}

TEST_CASE("External access on a compiled target enforces the public-access check via varInfos")
{
	Runtime rt;
	const InstanceId target = rt.addCompiled(makeCompiled<MockCompiled>());
	// The caller: keep a raw pointer to reach its bound Context via the test taps.
	auto owned = makeCompiled<MockCompiled>();
	auto* c = static_cast<MockCompiled*>(owned.get());
	rt.addCompiled(std::move(owned));

	// Public member: readable + writable.
	CHECK(c->ctxGetExternal(target, "Score").f == doctest::Approx(5.0f));
	c->ctxSetExternal(target, "Score", Value::ofFloat(3.0f));
	CHECK(rt.getVariable(target, "Score").f == doctest::Approx(3.0f));

	// Private member: warn + no-op (read yields {}, write is dropped).
	CHECK(c->ctxGetExternal(target, "Secret").f == doctest::Approx(0.0f));
	c->ctxSetExternal(target, "Secret", Value::ofFloat(99.0f));
	CHECK(rt.getVariable(target, "Secret").f == doctest::Approx(1.0f));

	// Unknown member: same warn path, overflow store is NOT reachable externally.
	rt.setVariable(target, "improvised", Value::ofInt(5));
	CHECK(c->ctxGetExternal(target, "improvised").i == 0);

	// Cross-boundary function call: public works, private is refused.
	const auto r = c->ctxCallExternal(target, "AddScore", { Value::ofFloat(1.0f) });
	REQUIRE(r.size() == 1);
	CHECK(r[0].f == doctest::Approx(4.0f));
	CHECK(c->ctxCallExternal(target, "Hidden", {}).empty());
	CHECK(rt.getVariable(target, "Secret").f == doctest::Approx(1.0f));
}

TEST_CASE("Bind/Emit works in both directions between compiled and interpreted instances")
{
	Runtime rt;

	// Direction 1: interpreted listener on a compiled owner. The mock's "Ping"
	// handler emits "Pong" through its Context — the listener's Event node fires.
	auto owned = makeCompiled<MockCompiled>();
	const InstanceId owner = rt.addCompiled(std::move(owned));
	const InstanceId listener = rt.add(interpretedPongCatcher());
	rt.bindEvent(owner, "Pong", listener);

	rt.fireEvent(owner, "Ping", 0, Value::ofFloat(1.0f));
	CHECK(rt.getVariable(listener, "gotPong").b == true);

	// Direction 2: compiled listener on an interpreted owner. Firing any event
	// on the owner reaches bound listeners after its own handlers.
	Probe probe;
	const InstanceId cListener = rt.addCompiled(makeCompiled<MockCompiled>(&probe));
	Graph owner2Graph; // no nodes needed — dispatch happens Runtime-side
	const InstanceId owner2 = rt.add(std::move(owner2Graph));
	rt.bindEvent(owner2, "Construct", cListener);
	rt.fireEvent(owner2, "Construct");
	CHECK(probe.constructs == 1);
}

TEST_CASE("GC follows Ref members and Ref arrays of compiled instances (collectRefs)")
{
	Runtime rt;

	auto rootOwned = makeCompiled<MockCompiled>();
	auto* root = static_cast<MockCompiled*>(rootOwned.get());
	const InstanceId rootId = rt.addCompiled(std::move(rootOwned));

	const InstanceId child   = rt.addCompiled(makeCompiled<MockCompiled>());
	const InstanceId kid     = rt.add(Graph{});                 // interpreted, held via Ref array
	const InstanceId orphan1 = rt.addCompiled(makeCompiled<MockCompiled>());
	const InstanceId orphan2 = rt.add(Graph{});

	root->setVariable("Child", Value::ofRef(child));
	Value kids; kids.type = PinType::Ref; kids.isArray = true;
	kids.items.push_back(Value::ofRef(kid));
	root->setVariable("Kids", kids);
	// Overflow-held Refs count too (undeclared name on a compiled instance).
	const InstanceId viaOverflow = rt.add(Graph{});
	rt.setVariable(rootId, "stash", Value::ofRef(viaOverflow));

	rt.retainOnlyReachableFrom(rootId);

	CHECK(rt.alive(rootId));
	CHECK(rt.alive(child));
	CHECK(rt.alive(kid));
	CHECK(rt.alive(viaOverflow));
	CHECK_FALSE(rt.alive(orphan1));
	CHECK_FALSE(rt.alive(orphan2));
}

TEST_CASE("eventBindingsOf serves both backends (widget interactivity source)")
{
	Runtime rt;
	const InstanceId compiled = rt.addCompiled(makeCompiled<MockCompiled>());
	const InstanceId interp   = rt.add(interpretedPongCatcher());

	const auto cb = rt.eventBindingsOf(compiled);
	REQUIRE(cb.size() == 4);
	const bool hasClick = std::any_of(cb.begin(), cb.end(), [](const auto& b)
	{ return b.name == "OnClicked" && b.elem == 2; });
	CHECK(hasClick);

	const auto ib = rt.eventBindingsOf(interp);
	REQUIRE(ib.size() == 1);
	CHECK(ib[0].name == "Pong");
	CHECK(ib[0].elem == 0);

	CHECK(rt.eventBindingsOf(9999).empty());
}

TEST_CASE("a class that implements only fireEvent still gets the engine events")
{
	// The hooks are an optimization, not a condition: CompiledInstance's default
	// forwards each of them to fireEvent under the canonical name, so a
	// hand-written class — or a generated one whose graph does not handle that
	// event — behaves exactly as it did before the hooks existed.
	struct Probe final : CompiledInstance
	{
		std::vector<std::string> seen;
		const char* classKey() const override { return "probe"; }
		void fireEvent(const std::string& name, int elem, const Value& arg) override
		{
			seen.push_back(name + "/" + std::to_string(elem) + "/" + valueTag(arg));
		}
		static std::string valueTag(const Value& v)
		{
			switch (v.type)
			{
				case PinType::Float:  return std::to_string((int)(v.f * 100));
				case PinType::Bool:   return v.b ? "true" : "false";
				case PinType::String: return v.s;
				case PinType::Int:    return std::to_string(v.i);
				default:              return "?";
			}
		}
	};
	Probe p;
	p.onConstruct();
	p.onClicked(4);
	p.onTick(0.5f);
	p.onTextChanged(2, "hi");
	p.onCheckChanged(7, true);
	p.onLevelUnloaded();
	REQUIRE(p.seen.size() == 6);
	CHECK(p.seen[0] == "Construct/0/0");
	CHECK(p.seen[1] == "OnClicked/4/0");
	CHECK(p.seen[2] == "Tick/0/50");
	CHECK(p.seen[3] == "OnTextChanged/2/hi");
	CHECK(p.seen[4] == "OnCheckChanged/7/true");
	CHECK(p.seen[5] == "OnLevelUnloaded/0/0");
}

TEST_CASE("destroy fires Destruct on a compiled instance before removing it")
{
	Runtime rt;
	Probe probe;
	const InstanceId id = rt.addCompiled(makeCompiled<MockCompiled>(&probe));
	rt.destroy(id);
	CHECK(probe.destructs == 1);
	CHECK_FALSE(rt.alive(id));
}

TEST_CASE("Services bound AFTER addCompiled still reach the compiled instance")
{
	// The packaged-game boot order: the compiled GameInstance registers (and
	// gets its Context) BEFORE setServices runs. The Context must forward to
	// the runtime's CURRENT services at call time — a copy taken at bind time
	// left compiled instances with dead services forever ("Create Object
	// failed" on a perfectly shipped class).
	Runtime rt;
	auto owned = makeCompiled<MockCompiled>();
	auto* m = static_cast<MockCompiled*>(owned.get());
	rt.addCompiled(std::move(owned));            // context bound here…

	CHECK(m->ctxCreateObject("Content/X.hasset") == 0u);   // …no services yet

	Runtime::Services svc;                       // …services arrive afterwards
	svc.createObject = [](const std::string&) -> uint32_t { return 77u; };
	rt.setServices(std::move(svc));
	CHECK(m->ctxCreateObject("Content/X.hasset") == 77u);
}

TEST_CASE("setGameInstanceCompiled installs the handle; a null instance keeps the old one")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstanceCompiled(makeCompiled<MockCompiled>());
	REQUIRE(gi != 0);
	CHECK(rt.gameInstance() == gi);

	// Any instance resolves Get Game Instance to it through its Context.
	auto owned = makeCompiled<MockCompiled>();
	auto* m = static_cast<MockCompiled*>(owned.get());
	rt.addCompiled(std::move(owned));
	CHECK(m->ctxGameInstance().ref == gi);

	// Null must not clobber a working GameInstance.
	CHECK(rt.setGameInstanceCompiled(CompiledPtr{}) == gi);
	CHECK(rt.gameInstance() == gi);
	CHECK(rt.alive(gi));
}

// ── class identity across both backends ─────────────────────────────────────

TEST_CASE("a compiled instance names its own class when the caller does not")
{
	Runtime rt;
	const InstanceId inst = rt.addCompiled(makeCompiled<MockCompiled>());
	REQUIRE(inst != 0);
	// Filled in from classKey()/baseClassKey(), so a caller holding nothing but
	// the compiled object does not have to restate what the object knows.
	CHECK(rt.classKeyOf(inst) == "Content/Logic/Mock.hasset");
	CHECK(rt.instanceIsA(inst, "Content/Logic/Mock.hasset"));

	// MockCompiled overrides no baseClassKey, and the DEFAULT is "" = Object —
	// the same thing a class without a taxonomy row has always been. A
	// hand-written CompiledInstance therefore keeps working unchanged.
	CHECK(rt.baseClassOf(inst).empty());
	CHECK(rt.instanceIsA(inst, "Object"));
	CHECK_FALSE(rt.instanceIsA(inst, "Entity"));
}

TEST_CASE("an explicitly passed identity wins over the compiled instance's own")
{
	// The level script and the GameInstance are keyed by their HOST, not by the
	// asset path the codegen happened to use — and PlayerHost trusts the asset's
	// baseClass over a generated library that may predate an edit to it.
	Runtime rt;
	const InstanceId inst = rt.addCompiled(makeCompiled<MockCompiled>(), {},
	                                       { "Content/Logic/Hero.hasset", "PlayerCharacter" });
	CHECK(rt.classKeyOf(inst) == "Content/Logic/Hero.hasset");
	CHECK(rt.instanceIsA(inst, "PlayerCharacter"));
	CHECK(rt.instanceIsA(inst, "Entity"));
	CHECK_FALSE(rt.instanceIsA(inst, "Content/Logic/Mock.hasset"));
}

TEST_CASE("interpreted and compiled instances of one class answer isA identically")
{
	// The whole point of routing Cast through the Runtime rather than classTag:
	// which backend served an instance must never change what it IS.
	Runtime rt;
	const ClassIdentity cls{ "Content/Logic/Hero.hasset", "PlayerCharacter" };
	const InstanceId compiled    = rt.addCompiled(makeCompiled<MockCompiled>(), {}, cls);
	const InstanceId interpreted = rt.add(Graph{}, {}, cls);

	for (const InstanceId id : { compiled, interpreted })
	{
		CHECK(rt.instanceIsA(id, "Content/Logic/Hero.hasset"));
		CHECK(rt.instanceIsA(id, "PlayerCharacter"));
		CHECK(rt.instanceIsA(id, "Entity"));
		CHECK(rt.instanceIsA(id, "Object"));
		CHECK_FALSE(rt.instanceIsA(id, "PlayerController"));
	}
}
