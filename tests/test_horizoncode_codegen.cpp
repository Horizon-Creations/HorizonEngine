// The HorizonCode → C++ parity harness (plan §10.1): every fixture graph runs
// TWICE — interpreted (Runtime + Graph) and compiled (the class hc_codegen
// emitted at build time) — against identical mock hosts. After every driven
// step the harness asserts the two runs are indistinguishable: identical
// host-call traces (order AND count — the re-evaluation clauses live here),
// identical variable snapshots, identical function results.
#include "doctest.h"
#include "fixtures.h"      // tests/fixtures/hcodegen — the shared graphs
#include "hc_registry.h"   // generated at build time by hc_codegen
#include <HorizonCode/HorizonCodeCompiled.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/EngineApi.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	std::string valueStr(const Value& v)
	{
		char buf[256];
		if (v.isArray)
		{
			std::string s = "[";
			for (size_t i = 0; i < v.items.size(); ++i)
			{
				if (i) s += ", ";
				s += valueStr(v.items[i]);
			}
			return s + "]";
		}
		switch (v.type)
		{
			case PinType::Float:  std::snprintf(buf, sizeof buf, "f:%g", v.f); return buf;
			case PinType::Bool:   return v.b ? "b:true" : "b:false";
			case PinType::Int:    return "i:" + std::to_string(v.i);
			case PinType::String: return "s:\"" + v.s + "\"";
			case PinType::Vec2:   std::snprintf(buf, sizeof buf, "v2:(%g,%g)", v.v2.x, v.v2.y); return buf;
			case PinType::Color:  std::snprintf(buf, sizeof buf, "col:(%g,%g,%g,%g)", v.col.x, v.col.y, v.col.z, v.col.w); return buf;
			case PinType::Ref:    return "ref:" + std::to_string(v.ref);
			case PinType::Transform:
				std::snprintf(buf, sizeof buf, "xf:(%g,%g,%g|%g,%g,%g|%g,%g,%g)",
				              v.tpos.x, v.tpos.y, v.tpos.z, v.trot.x, v.trot.y, v.trot.z,
				              v.tscl.x, v.tscl.y, v.tscl.z);
				return buf;
			default: return "?";
		}
	}

	bool valueEq(const Value& a, const Value& b)
	{
		if (a.type != b.type || a.isArray != b.isArray) return false;
		if (a.isArray)
		{
			if (a.items.size() != b.items.size()) return false;
			for (size_t i = 0; i < a.items.size(); ++i)
				if (!valueEq(a.items[i], b.items[i])) return false;
			return true;
		}
		switch (a.type)
		{
			case PinType::Float:  return a.f == b.f;   // bit-exact parity
			case PinType::Bool:   return a.b == b.b;
			case PinType::Int:    return a.i == b.i;
			case PinType::String: return a.s == b.s;
			case PinType::Vec2:   return a.v2 == b.v2;
			case PinType::Color:  return a.col == b.col;
			case PinType::Ref:    return a.ref == b.ref;
			case PinType::Transform:
				return a.tpos == b.tpos && a.trot == b.trot && a.tscl == b.tscl;
			default: return false;
		}
	}

	// One backend's world: a Runtime + recording mock host. Both backends get an
	// identical twin so their traces must line up call for call.
	const CompiledClassEntry* findCompiled(const std::string& key);

	struct World
	{
		Runtime rt;
		InstanceId id = 0;
		std::vector<std::string> trace;
		std::map<std::pair<int, std::string>, Value> props;
		int widgetCounter = 0;
		bool useCompiled = false;   // this world instantiates classes compiled

		HostBindings host()
		{
			HostBindings h;
			h.getProperty = [this](InstanceId, int elem, const std::string& prop) -> Value
			{
				trace.push_back("getProp " + std::to_string(elem) + " " + prop);
				auto it = props.find({ elem, prop });
				return it != props.end() ? it->second : Value{};
			};
			h.setProperty = [this](InstanceId, int elem, const std::string& prop, const Value& v)
			{
				trace.push_back("setProp " + std::to_string(elem) + " " + prop + " = " + valueStr(v));
				props[{ elem, prop }] = v;
			};
			h.showSelf = [this](InstanceId) { trace.push_back("showSelf"); };
			h.hideSelf = [this](InstanceId) { trace.push_back("hideSelf"); };
			return h;
		}

		void bindServices()
		{
			Runtime::Services s;
			s.createWidget = [this](const std::string& path) -> int
			{
				const int wid = 100 + ++widgetCounter;
				trace.push_back("createWidget " + path + " -> " + std::to_string(wid));
				return wid;
			};
			s.showWidget    = [this](int wid) { trace.push_back("showWidget " + std::to_string(wid)); };
			s.hideWidget    = [this](int wid) { trace.push_back("hideWidget " + std::to_string(wid)); };
			s.destroyWidget = [this](int wid) { trace.push_back("destroyWidget " + std::to_string(wid)); };
			// Create Object instantiates the class in THIS world's backend and
			// fires "Construct" — the same shape as the app's svc.createObject.
			s.createObject  = [this](const std::string& path) -> uint32_t
			{
				InstanceId nid = 0;
				if (useCompiled)
				{
					if (const CompiledClassEntry* e = findCompiled(path))
						nid = rt.addCompiled(CompiledPtr(e->create(), CompiledDeleter{ e->destroy }));
				}
				else
				{
					for (auto& src : hcfix::all())
						if (src.key == path) { nid = rt.add(std::move(src.graph)); break; }
				}
				trace.push_back("createObject " + path + " -> " + std::to_string(nid));
				if (nid) rt.fireEvent(nid, "Construct");
				return nid;
			};
			s.destroyObject = [this](uint32_t ref)
			{
				trace.push_back("destroyObject " + std::to_string(ref));
				rt.destroy(ref);
			};
			s.callApi = [this](const std::string& id_, const std::vector<Value>& args) -> std::vector<Value>
			{
				// The REAL registry against a null-world Ctx (its null tolerance
				// makes every call well-defined and deterministic) — args AND
				// results are recorded, so dispatch counts, coerced argument
				// values and returned values must all match across backends.
				std::string t = "callApi " + id_ + "(";
				for (size_t i = 0; i < args.size(); ++i)
				{
					if (i) t += ", ";
					t += valueStr(args[i]);
				}
				std::vector<Value> r;
				if (const HE::api::ApiFn* fn = HE::api::find(id_))
				{
					HE::api::Ctx ctx;
					r = fn->invoke(ctx, args);
				}
				t += ") -> (";
				for (size_t i = 0; i < r.size(); ++i)
				{
					if (i) t += ", ";
					t += valueStr(r[i]);
				}
				trace.push_back(t + ")");
				return r;
			};
			rt.setServices(std::move(s));
		}
	};

	const CompiledClassEntry* findCompiled(const std::string& key)
	{
		int count = 0;
		const CompiledClassEntry* entries = hcgen::classes(&count);
		for (int i = 0; i < count; ++i)
			if (key == entries[i].key) return &entries[i];
		return nullptr;
	}

	// Both backends of one fixture, driven in lockstep.
	struct ParityPair
	{
		World interp, comp;

		explicit ParityPair(const std::string& key)
		{
			HE::hccg::ClassSource src;
			bool found = false;
			for (auto& s : hcfix::all())
				if (s.key == key) { src = std::move(s); found = true; break; }
			REQUIRE(found);

			comp.useCompiled = true;
			interp.bindServices();
			comp.bindServices();
			interp.id = interp.rt.add(std::move(src.graph), interp.host());

			const CompiledClassEntry* entry = findCompiled(key);
			REQUIRE(entry != nullptr);
			comp.id = comp.rt.addCompiled(
				CompiledPtr(entry->create(), CompiledDeleter{ entry->destroy }), comp.host());
			REQUIRE(comp.id != 0);
		}

		void checkParity()
		{
			// Host-call traces: order and count must match exactly.
			REQUIRE(interp.trace.size() == comp.trace.size());
			for (size_t i = 0; i < interp.trace.size(); ++i)
			{
				INFO("trace entry ", i);
				CHECK(interp.trace[i] == comp.trace[i]);
			}
			// Variable stores.
			const auto si = interp.rt.variablesSnapshot(interp.id);
			const auto sc = comp.rt.variablesSnapshot(comp.id);
			REQUIRE(si.size() == sc.size());
			for (const auto& [name, vi] : si)
			{
				INFO("variable ", name, ": interp=", valueStr(vi),
				     " compiled=", sc.count(name) ? valueStr(sc.at(name)) : "<missing>");
				REQUIRE(sc.count(name) == 1);
				CHECK(valueEq(vi, sc.at(name)));
			}
			// Property end states.
			REQUIRE(interp.props.size() == comp.props.size());
			for (const auto& [k, vi] : interp.props)
			{
				INFO("property ", k.first, ".", k.second);
				REQUIRE(comp.props.count(k) == 1);
				CHECK(valueEq(vi, comp.props.at(k)));
			}
		}

		void fire(const std::string& ev, int elem = 0, const Value& arg = {}, bool compare = true)
		{
			interp.rt.fireEvent(interp.id, ev, elem, arg);
			comp.rt.fireEvent(comp.id, ev, elem, arg);
			if (compare) checkParity();
		}

		// Returns the interpreted result row (after asserting it equals the
		// compiled one, incl. the success flag).
		std::pair<bool, std::vector<Value>> call(const std::string& fn, bool requirePublic,
		                                         const std::vector<Value>& args = {})
		{
			std::vector<Value> ri, rc;
			const bool bi = interp.rt.callFunction(interp.id, fn, requirePublic, args, &ri);
			const bool bc = comp.rt.callFunction(comp.id, fn, requirePublic, args, &rc);
			CHECK(bi == bc);
			REQUIRE(ri.size() == rc.size());
			for (size_t i = 0; i < ri.size(); ++i)
			{
				INFO("result ", i, ": interp=", valueStr(ri[i]), " compiled=", valueStr(rc[i]));
				CHECK(valueEq(ri[i], rc[i]));
			}
			checkParity();
			return { bi, ri };
		}

		Value var(const std::string& name) { return interp.rt.getVariable(interp.id, name); }

		// Snapshot-compare a SECONDARY instance (e.g. one Create Object made) —
		// ids line up because both worlds mint them identically.
		void checkInstance(InstanceId other)
		{
			CHECK(interp.rt.alive(other) == comp.rt.alive(other));
			const auto si = interp.rt.variablesSnapshot(other);
			const auto sc = comp.rt.variablesSnapshot(other);
			REQUIRE(si.size() == sc.size());
			for (const auto& [name, vi] : si)
			{
				INFO("instance ", other, " variable ", name);
				REQUIRE(sc.count(name) == 1);
				CHECK(valueEq(vi, sc.at(name)));
			}
		}
	};
}

TEST_CASE("codegen parity: flow_branch_sequence")
{
	ParityPair p("fix/flow_branch_sequence");
	p.fire("Run");
	CHECK(p.var("trace").s == "ATB");   // Then0 (A, branch True) fully before Then1 (B)
	p.fire("SetFlag", 0, Value::ofBool(false));
	p.fire("Run");
	CHECK(p.var("trace").s == "ATBAFB");
}

TEST_CASE("codegen parity: coerce_matrix")
{
	ParityPair p("fix/coerce_matrix");
	p.fire("Defaults");
	CHECK(p.var("fOut").f == 1.0f);   // Bool(true) default on a Float pin
	CHECK(p.var("iOut").i == 3);      // Float(3.9) truncates on an Int pin
	CHECK(p.var("bOut").b == true);   // Int(2) on a Bool pin
	CHECK(p.var("sOut").s == "");     // Float on a String pin → zero value
	p.fire("ArgF", 0, Value::ofInt(5));
	CHECK(p.var("fOut").f == 5.0f);
	p.fire("ArgF", 0, Value::ofString("x"));
	CHECK(p.var("fOut").f == 0.0f);   // inconvertible arg → zero
	p.fire("ArgB", 0, Value::ofFloat(2.0f));
	CHECK(p.var("bOut").b == true);
	p.fire("ArgB", 0, Value::ofFloat(0.0f));
	CHECK(p.var("bOut").b == false);
}

TEST_CASE("codegen parity: math_ops")
{
	ParityPair p("fix/math_ops");
	p.fire("Calc");
	CHECK(p.var("add").f == 3.0f);
	CHECK(p.var("sub").f == -3.5f);
	CHECK(p.var("mul").f == 4.5f);
	CHECK(p.var("d0").f == 0.0f);     // divide by zero → 0
	CHECK(p.var("d1").f == 2.5f);
	CHECK(p.var("gt").b == true);
	CHECK(p.var("lt").b == false);
	CHECK(p.var("eq").b == true);     // |0.3000001 - 0.3| < 1e-6
	CHECK(p.var("lg").b == true);     // And(Not(false), Or(false, true))
	CHECK(p.var("str").s == "3.5x");  // %g + Concat
}

TEST_CASE("codegen parity: variables")
{
	ParityPair p("fix/variables");
	p.fire("Mut");
	p.fire("Mut");
	CHECK(p.var("f").f == 3.5f);          // 1.5 + 1 + 1
	// Pass-through re-evaluates the Value INPUT at read time — after the 2nd
	// Mut set f to 3.5, the re-read computes f + 1 again.
	CHECK(p.var("copied").f == 4.5f);
	CHECK(p.var("ghostRead").i == 42);    // undeclared set-then-get
	CHECK(p.var("arrF").items.size() == 5);
}

TEST_CASE("codegen parity: functions_basic")
{
	ParityPair p("fix/functions_basic");
	auto [ok1, r1] = p.call("Sum", true, { Value::ofFloat(5), Value::ofFloat(6) });
	CHECK(ok1);
	REQUIRE(r1.size() == 1);
	CHECK(r1[0].f == 11.0f);
	auto [ok2, r2] = p.call("Sum", true, { Value::ofFloat(5) });   // missing arg → typed default
	CHECK(ok2);
	CHECK(r2[0].f == 5.0f);
	auto [ok3, r3] = p.call("Pick", true, { Value::ofFloat(1) });
	CHECK(ok3);
	CHECK(r3[0].s == "pos");
	auto [ok4, r4] = p.call("Pick", true, { Value::ofFloat(-1) });
	CHECK(ok4);
	CHECK(r4[0].s == "neg");
	auto [okPriv, _] = p.call("Secret", true);    // private + requirePublic → refused
	CHECK_FALSE(okPriv);
	auto [okPriv2, __] = p.call("Secret", false);
	CHECK(okPriv2);
	CHECK(p.var("sec").f == 1.0f);
	auto [okMiss, ___] = p.call("Nope", true);
	CHECK_FALSE(okMiss);
	p.fire("UseSum");
	CHECK(p.var("out").f == 7.0f);
}

TEST_CASE("codegen parity: functions_recursive (per-run exec cache across frames)")
{
	ParityPair p("fix/functions_recursive");
	auto [ok, r] = p.call("R", true, { Value::ofFloat(2) });
	CHECK(ok);
	REQUIRE(r.size() == 1);
	// The S-call cache is per RUN: after the innermost frame it holds S(0)=1,
	// so every accumulation reads 1 → sum walks 1, 2, 3. A per-invocation cache
	// would read 21/11 in the outer frames — the RunState decision (§5.4).
	CHECK(r[0].f == 3.0f);
	CHECK(p.var("sum").f == 3.0f);
}

TEST_CASE("codegen parity: foreach_arrays")
{
	ParityPair p("fix/foreach_arrays");
	p.fire("Loop");
	CHECK(p.var("total").f == 18.0f);      // 5+6+7
	CHECK(p.var("count").i == 3);
	CHECK(p.var("afterElem").f == 7.0f);   // last iteration persists after Done
	CHECK(p.var("afterIdx").i == 2);
	p.fire("Ops");
	REQUIRE(p.var("built").items.size() == 2);
	CHECK(p.var("built").items[1].f == 2.0f);
	REQUIRE(p.var("mod").items.size() == 3);   // {9, 7, 42} after set/insert-clamp/remove
	CHECK(p.var("mod").items[0].f == 9.0f);
	CHECK(p.var("mod").items[2].f == 42.0f);
	CHECK(p.var("has6").b == true);
	CHECK(p.var("idx7").i == 2);
	CHECK(p.var("oob").f == 0.0f);             // out-of-range Get → element default
	p.fire("Nested");
	CHECK(p.var("nestedSum").f == 324.0f);     // (5+6+7)^2
}

TEST_CASE("codegen parity: events_multi (order, elem filter, shared per-fire cache)")
{
	ParityPair p("fix/events_multi");
	p.fire("Ping", 0);                      // only handler A (elem 0 = any)
	CHECK(p.var("trace").s == "a");
	CHECK(p.var("wRef").ref == 0);          // handler B never ran
	p.fire("Ping", 2);                      // A then B, sharing one run's cache
	CHECK(p.var("trace").s == "aab");
	CHECK(p.var("wRef").ref == 102);        // B read A's fresh CreateWidget ref
	p.fire("Tick", 0, Value::ofFloat(1.5f));
	p.fire("Tick", 0, Value::ofFloat(2.0f));
	CHECK(p.var("tickSum").f == 3.5f);
}

TEST_CASE("codegen parity: widget_props")
{
	ParityPair p("fix/widget_props");
	p.fire("UI");
	CHECK(p.var("got").f == 0.5f);          // set → get round-trips the host map
	CHECK(p.var("copied").s == "hi");       // Set-property pass-through
	CHECK(p.interp.props.at({ 3, "opacity" }).f == 0.5f);
	CHECK(p.interp.props.at({ 4, "text" }).s == "hi");
	// showSelf/hideSelf both recorded (order asserted by the trace comparison).
	CHECK(std::count(p.interp.trace.begin(), p.interp.trace.end(), "showSelf") == 1);
	CHECK(std::count(p.interp.trace.begin(), p.interp.trace.end(), "hideSelf") == 1);
}

TEST_CASE("codegen parity: limits_smoke (both sides abort cleanly, §3.6)")
{
	ParityPair p("fix/limits_smoke");
	// 20^3 body iterations ≫ 4096 steps: the abort points differ by design
	// (sharpened, not exact) — the assertion is only clean termination.
	p.fire("Burn", 0, {}, /*compare=*/false);
	CHECK(p.interp.rt.alive(p.interp.id));
	CHECK(p.comp.rt.alive(p.comp.id));
}

TEST_CASE("codegen parity: engine_pure_multiout (dispatch per data-out read)")
{
	ParityPair p("fix/engine_pure_multiout");
	p.fire("Query");
	// A pure EngineCall re-dispatches at EVERY read: 4 raycast reads (incl. the
	// same output twice) + 1 math.sin — the trace comparison in fire() already
	// asserted order/count/args/results across backends; pin the counts here.
	const auto isRaycast = [](const std::string& t) { return t.rfind("callApi physics.raycast", 0) == 0; };
	CHECK(std::count_if(p.interp.trace.begin(), p.interp.trace.end(), isRaycast) == 4);
	CHECK(p.var("hit").b == false);   // null world → deterministic miss
	CHECK(p.var("hit2").b == false);
	CHECK(p.var("sinv").f == std::sin(0.5f));
}

TEST_CASE("codegen parity: engine_exec_cached (one dispatch, cached reads, save round-trip)")
{
	ParityPair p("fix/engine_exec_cached");
	p.fire("Roll");
	// Exec EngineCalls dispatch exactly once; both value reads hit the cache.
	const auto isValue = [](const std::string& t) { return t.rfind("callApi random.value", 0) == 0; };
	CHECK(std::count_if(p.interp.trace.begin(), p.interp.trace.end(), isValue) == 1);
	CHECK(p.var("v1").f == p.var("v1b").f);
	// The seed arg coerced Float(42.7) → Int(42) — visible in the trace.
	REQUIRE(!p.interp.trace.empty());
	CHECK(p.interp.trace[0] == "callApi random.seed(i:42) -> ()");
	CHECK(p.var("saved").f == 3.5f);
}

TEST_CASE("codegen parity: refs_objects (create/destroy, external access, warn paths)")
{
	ParityPair p("fix/refs_objects");
	p.fire("Spawn");
	const uint32_t obj = p.var("obj").ref;
	CHECK(obj != 0);
	p.checkInstance(obj);
	CHECK(p.interp.rt.getVariable(obj, "constructed").f == 1.0f);   // Construct fired

	p.fire("Poke");
	p.checkInstance(obj);
	CHECK(p.var("seen").f == 50.0f);    // Set (Ref) → Get (Ref) round trip
	CHECK(p.var("left").f == 30.0f);    // Damage(20) on hp 50
	CHECK(p.var("sneak").f == 0.0f);    // private member → warn + zero
	CHECK(p.interp.rt.getVariable(obj, "hp").f == 30.0f);   // private Heal refused

	p.fire("NullPoke");
	CHECK(p.var("nseen").f == 0.0f);    // null target → error log + zero

	p.fire("Who");
	CHECK(p.var("meRef").ref == p.interp.id);   // GetSelf
	CHECK(p.var("giRef").ref == 0);             // no GameInstance registered

	p.fire("Kill");
	CHECK_FALSE(p.interp.rt.alive(obj));
	CHECK_FALSE(p.comp.rt.alive(obj));
	p.checkInstance(obj);   // both dead → both empty
}

TEST_CASE("codegen parity: dispatchers (mixed compiled↔interpreted in ONE Runtime)")
{
	// Three runtimes with the same population — all-interpreted (the reference),
	// interpreted owner + compiled listeners, compiled owner + interpreted
	// listeners — driven identically must end in identical state (§3.5: the
	// dispatch machinery is Runtime-side and backend-agnostic).
	struct Mixed
	{
		Runtime rt;
		InstanceId owner = 0, l1 = 0, l2 = 0;

		InstanceId make(const std::string& key, bool compiled)
		{
			if (compiled)
			{
				const CompiledClassEntry* e = findCompiled(key);
				REQUIRE(e != nullptr);
				return rt.addCompiled(CompiledPtr(e->create(), CompiledDeleter{ e->destroy }));
			}
			HE::hccg::ClassSource src;
			for (auto& s : hcfix::all())
				if (s.key == key) { src = std::move(s); break; }
			REQUIRE(!src.key.empty());
			return rt.add(std::move(src.graph));
		}

		void setup(bool ownerCompiled, bool listenersCompiled)
		{
			owner = make("fix/dispatch_owner", ownerCompiled);
			l1 = make("fix/dispatch_listener", listenersCompiled);
			l2 = make("fix/dispatch_listener", listenersCompiled);
			rt.fireEvent(l1, "Setup", 0, Value::ofRef(owner));   // l1 listens to owner
			rt.fireEvent(l2, "Setup", 0, Value::ofRef(l1));      // l2 listens to l1 (relay chain)
		}
		void drive()
		{
			rt.fireEvent(owner, "Go");           // emit → l1 (+relay → l2)
			rt.fireEvent(owner, "Sig", 0, Value::ofFloat(2.0f)); // own handler + listeners
		}
		float got(InstanceId id) { return rt.getVariable(id, "got").f; }
	};

	Mixed ref, mixA, mixB;
	ref.setup(false, false);
	mixA.setup(false, true);
	mixB.setup(true, false);
	ref.drive();
	mixA.drive();
	mixB.drive();

	// The relay chain: Go emits Sig(7) → l1 got 7; l2 (bound to l1) receives it
	// TWICE — once from l1's relay-emit, once from fireEvent(l1)'s trailing
	// listener dispatch (§3.5: a fired event also reaches the fired instance's
	// listeners) → l2 got 14. Direct Sig(2): ownGot 2, l1 → 9, l2 +2+2 → 18.
	CHECK(ref.got(ref.l1) == 9.0f);
	CHECK(ref.got(ref.l2) == 18.0f);
	CHECK(ref.rt.getVariable(ref.owner, "ownGot").f == 2.0f);
	for (Mixed* m : { &mixA, &mixB })
	{
		CHECK(m->got(m->l1) == ref.got(ref.l1));
		CHECK(m->got(m->l2) == ref.got(ref.l2));
		CHECK(m->rt.getVariable(m->owner, "ownGot").f == 2.0f);
	}

	// Depth-guard smoke: a bind CYCLE between two NON-relaying sinks terminates
	// via the dispatch-depth guard (fireEvent's trailing listener dispatch hops
	// l1→l2→l1→… and is cut at 32), identically for every population. NB: the
	// cycle deliberately uses sinks — a RELAYING listener in a cycle branches
	// the dispatch tree (EmitEvent + trailing dispatch) into ~2^32 fires; the
	// guard bounds depth, not total work.
	auto cycleGot = [](bool compiled) -> std::pair<float, float>
	{
		Mixed m;
		m.l1 = m.make("fix/dispatch_sink", compiled);
		m.l2 = m.make("fix/dispatch_sink", !compiled);   // genuinely mixed cycle
		m.rt.fireEvent(m.l1, "Setup", 0, Value::ofRef(m.l2));
		m.rt.fireEvent(m.l2, "Setup", 0, Value::ofRef(m.l1));
		m.rt.fireEvent(m.l1, "Sig", 0, Value::ofFloat(1.0f));
		return { m.got(m.l1), m.got(m.l2) };
	};
	Mixed refCycle;
	refCycle.l1 = refCycle.make("fix/dispatch_sink", false);
	refCycle.l2 = refCycle.make("fix/dispatch_sink", false);
	refCycle.rt.fireEvent(refCycle.l1, "Setup", 0, Value::ofRef(refCycle.l2));
	refCycle.rt.fireEvent(refCycle.l2, "Setup", 0, Value::ofRef(refCycle.l1));
	refCycle.rt.fireEvent(refCycle.l1, "Sig", 0, Value::ofFloat(1.0f));
	CHECK(refCycle.got(refCycle.l1) > 0.0f);   // it ran…
	CHECK(refCycle.got(refCycle.l1) < 40.0f);  // …and the guard cut it
	const auto a = cycleGot(true), b = cycleGot(false);
	CHECK(a.first == refCycle.got(refCycle.l1));
	CHECK(a.second == refCycle.got(refCycle.l2));
	CHECK(b.first == refCycle.got(refCycle.l1));
	CHECK(b.second == refCycle.got(refCycle.l2));
}

TEST_CASE("codegen parity: functions_locals (§13.4)")
{
	ParityPair p("fix/functions_locals");
	auto [ok1, r1] = p.call("Work", true, { Value::ofFloat(5) });
	CHECK(ok1);
	CHECK(r1[0].f == 15.0f);              // local acc = 10 + 5
	CHECK(p.var("out").f == 15.0f);
	CHECK(p.var("outLen").i == 2);        // local array {1} + n
	auto [ok2, r2] = p.call("Work", true, { Value::ofFloat(5) });
	CHECK(ok2);
	CHECK(r2[0].f == 15.0f);              // locals reset per invocation — no drift
	CHECK(p.var("outLen").i == 2);

	// Recursion: local `mine` is per FRAME, the S2-call cache per RUN.
	auto [ok3, r3] = p.call("R2", true, { Value::ofFloat(2) });
	CHECK(ok3);
	CHECK(r3[0].f == 2.0f);               // mine(2) + overwritten cache S2(0)=0

	// Locals never surface in the instance store / snapshot / external reads.
	CHECK(p.interp.rt.variablesSnapshot(p.interp.id).count("acc") == 0);
	CHECK(p.comp.rt.variablesSnapshot(p.comp.id).count("acc") == 0);
	CHECK(p.var("acc").f == 0.0f);
}
