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
			case PinType::Enum:   return "e:" + std::to_string(v.i) + "@" + v.typeName;
			case PinType::Struct:
			{
				std::string s = "st{" + v.typeName;
				for (const Value& it : v.items) s += ", " + valueStr(it);
				return s + "}";
			}
			default: return "?";
		}
	}

	// `inArray`: element Values are compared WITHOUT their typeName. The
	// interpreter fills that field inconsistently for array payloads (an enum
	// array seeded from a struct definition tags its items, one built by
	// ArrayAdd does not), so it carries no meaning there — the declared element
	// type lives on the pin. For scalars the typeName IS meaningful and is
	// compared: it is how a consumer knows WHICH enum/struct it received.
	bool valueEq(const Value& a, const Value& b, bool inArray = false)
	{
		if (a.type != b.type || a.isArray != b.isArray) return false;
		if (!a.isArray && !inArray && a.type == PinType::Struct && a.typeName != b.typeName)
			return false;
		// Enums: compared only when BOTH sides name a definition. An enum Value
		// that reaches the interpreter through a dynamic boundary (an Int event
		// arg landing on an enum pin) keeps the EMPTY typeName `coerce` gives
		// it, while generated code stamps the pin's DECLARED definition — it is
		// the one place the compiled backend knows more than the interpreter,
		// and the extra information is the correct one (a consumer needs to
		// know which enum it holds). Values, which is what anything acts on,
		// are still compared exactly.
		if (!a.isArray && !inArray && a.type == PinType::Enum &&
		    !a.typeName.empty() && !b.typeName.empty() && a.typeName != b.typeName)
			return false;
		if (a.isArray)
		{
			if (a.items.size() != b.items.size()) return false;
			for (size_t i = 0; i < a.items.size(); ++i)
				if (!valueEq(a.items[i], b.items[i], /*inArray=*/true)) return false;
			return true;
		}
		if (a.type == PinType::Struct)
		{
			// Fields are matched positionally, which IS the layout contract:
			// a struct Value's items are in definition order (§3.4).
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
			case PinType::Enum:   return a.i == b.i;   // int-backed on both sides
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
				// The class identity travels with the instance in BOTH worlds,
				// exactly as the apps' svc.createObject does it. Without it the
				// interpreted side would have no class to compare a Cast
				// against while the compiled side reads its own classKey() —
				// and the two would disagree for reasons the fixture never
				// intended to test.
				ClassIdentity cls;
				for (const auto& src : hcfix::all())
					if (src.key == path) { cls = { src.key, src.baseClass }; break; }
				if (useCompiled)
				{
					if (const CompiledClassEntry* e = findCompiled(path))
						nid = rt.addCompiled(CompiledPtr(e->create(), CompiledDeleter{ e->destroy }),
						                     {}, cls);
				}
				else
				{
					for (auto& src : hcfix::all())
						if (src.key == path) { nid = rt.add(std::move(src.graph), {}, cls); break; }
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
			s.callApi = [this](InstanceId, const std::string& id_,
			                   const std::vector<Value>& args) -> std::vector<Value>
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
		CompiledInstance* compInst = nullptr;   // the compiled side, by pointer

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
			// Both worlds get the GameInstance, each in its own backend: it is
			// the one reference generated code resolves without a lookup, so it
			// has to be present for that path to be exercised at all.
			{
				for (auto& s : hcfix::all())
					if (s.key == "__game_instance__")
					{ interp.rt.setGameInstance(std::move(s.graph)); break; }
				if (const CompiledClassEntry* gi = findCompiled("__game_instance__"))
					comp.rt.setGameInstanceCompiled(
						CompiledPtr(gi->create(), CompiledDeleter{ gi->destroy }));
			}
			const ClassIdentity cls{ src.key, src.baseClass, src.chain };
			// The interpreted side gets the CHAIN as levels, root first — the
			// fixture's `graph` is one class's own level now, exactly like the
			// compiled side, where the ancestry is C++ inheritance. Building it
			// here is what makes the two comparable at all for a derived class.
			{
				std::vector<Graph> levels;
				for (auto a = src.chain.rbegin(); a != src.chain.rend(); ++a)
				{
					bool have = false;
					for (auto& s : hcfix::all())
						if (s.key == *a) { levels.push_back(std::move(s.graph)); have = true; break; }
					REQUIRE(have);   // a fixture chain must be complete
				}
				levels.push_back(std::move(src.graph));
				interp.id = interp.rt.addLevels(std::move(levels), interp.host(), cls);
			}

			const CompiledClassEntry* entry = findCompiled(key);
			REQUIRE(entry != nullptr);
			CompiledPtr owned(entry->create(), CompiledDeleter{ entry->destroy });
			compInst = owned.get();   // kept so a test can call its hooks directly
			comp.id = comp.rt.addCompiled(std::move(owned), comp.host(), cls);
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

		// Advance latent flow (Delay) on both backends in lockstep.
		void update(float dt, bool compare = true)
		{
			interp.rt.update(dt);
			comp.rt.update(dt);
			if (compare) checkParity();
		}

		void reseed()
		{
			interp.rt.reseedVariables(interp.id);
			comp.rt.reseedVariables(comp.id);
			checkParity();
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
	// A Return inside a Sequence arm ends only ITS chain: arm 1 still runs (the
	// side effect happens) and its own Return overwrites the result.
	auto [okE, rE] = p.call("Early", true, { Value::ofFloat(0) });
	CHECK(okE);
	REQUIRE(rE.size() == 1);
	CHECK(rE[0].f == 2.0f);               // arm 1's Return wins, not arm 0's
	CHECK(p.var("sec").f == 7.0f);        // arm 1 ran at all

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
	// save v2: with no active save the set fails and the get returns its
	// default (-1) — identically on both backends. The VALUE round-trip lives
	// in test_engine_api's save-v2 cases.
	CHECK(p.var("saved").f == -1.0f);
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
	// The harness registers a GameInstance in both worlds (see ParityPair), and
	// both mint ids identically — checkParity has already compared the two.
	CHECK(p.var("giRef").ref != 0);

	p.fire("Kill");
	CHECK_FALSE(p.interp.rt.alive(obj));
	CHECK_FALSE(p.comp.rt.alive(obj));
	p.checkInstance(obj);   // both dead → both empty
}

TEST_CASE("codegen parity: casts (the DIRECT lowerings agree with the interpreter)")
{
	// The fixtures are generated with OnFailure::Stop, so the compiled side here
	// is not using the hc::castRef seam at all: an exact HorizonCode class
	// lowers to hc::as<T> (a pointer comparison) and an engine base class to
	// hc::castBase (resolveCompiled + baseClassKey). That fast path is only
	// sound if it answers exactly what Runtime::instanceIsA answers, and this is
	// the case that proves it — without it the optimization would be untested.
	ParityPair p("fix/casts");
	p.fire("Spawn");
	const uint32_t obj = p.var("obj").ref;
	CHECK(obj != 0);
	p.checkInstance(obj);

	p.fire("Go");
	// Every marker is wired onto the branch the cast is EXPECTED to take, so a
	// wrong answer stops the chain and leaves the rest at zero.
	CHECK(p.var("exact").f == 1.0f);   // exact class          → hc::as<T>
	CHECK(p.var("base").f  == 1.0f);   // its own engine base  → hc::castBase
	CHECK(p.var("up").f    == 1.0f);   // Entity, up the chain → hc::castBase
	CHECK(p.var("side").f  == 1.0f);   // PlayerController     → Failure
	CHECK(p.var("other").f == 1.0f);   // a different class    → Failure
	CHECK(p.var("null").f  == 1.0f);   // Ref 0                → Failure
	// The Success branch hands the SAME reference through, not a fresh one.
	CHECK(p.var("gotRef").ref == obj);
}

TEST_CASE("codegen parity: a derived class is a derived class in C++ too")
{
	// The interpreted side runs two graph LEVELS; the compiled side is one
	// object of a class that derives from another. Every clause of "what
	// inheritance means" has to come out the same on both.
	ParityPair p("fix/inherit_derived");

	// 1. An override REPLACES: Construct is declared in both, and only the
	//    derived one may run (baseOnly would be 1 if the base's had).
	p.compInst->onConstruct();
	p.interp.rt.fireConstruct(p.interp.id);
	p.checkParity();
	CHECK(p.var("mine").f == 5.0f);
	CHECK(p.var("baseOnly").f == 0.0f);

	// 2. An inherited variable is ONE variable: the derived handler writes the
	//    base's `hits`, and the base's default for `hp` is visible here.
	CHECK(p.var("hp").f == 100.0f);
	p.fire("Ping");
	CHECK(p.var("hits").f == 99.0f);

	// 3. What the derived class does NOT handle still reaches the base.
	p.fire("Pong");
	CHECK(p.var("hits").f == 109.0f);

	// 4. A call to an inherited PUBLIC function runs it, results and all; a call
	//    to a private one reaches nothing (both are in the "Go" chain).
	p.fire("Go");
	CHECK(p.var("hp").f   == 70.0f);
	CHECK(p.var("left").f == 70.0f);
	CHECK(p.var("secret").f == 0.0f);   // private to the base — not callable

	// 5. A Cast to the BASE class succeeds. One exact class tag per class cannot
	//    answer that, so this is the case the chain-aware lowering exists for.
	p.fire("Check");
	CHECK(p.var("isBase").f == 1.0f);

	// 6. The name-keyed seam reaches the whole chain — this is what the Runtime
	//    (and the garbage collector, through the same table) actually uses.
	CHECK(p.comp.rt.getVariable(p.comp.id, "hp").f == 70.0f);
	p.comp.rt.setVariable(p.comp.id, "hp", Value::ofFloat(5.0f));
	p.interp.rt.setVariable(p.interp.id, "hp", Value::ofFloat(5.0f));
	p.checkParity();

	// 7. Reseeding resets the base's variables as well as the derived one's —
	//    the base's own reseed is the only thing that can reach them.
	p.reseed();
	CHECK(p.var("hp").f == 100.0f);
	CHECK(p.var("mine").f == 0.0f);
}

TEST_CASE("codegen parity: a base class with no variables at all")
{
	// slots() is a generated static, not a virtual — a child that concatenated
	// its base's table when the base emitted none would name a function that
	// does not exist, and the break would surface as a compiler error during a
	// packaged export. That this fixture is COMPILED INTO this binary is half
	// the test; the rest checks the inherited call still lands.
	ParityPair p("fix/inherit_novars");
	p.fire("Go");
	CHECK(p.var("got").f == 7.0f);
	p.reseed();
	CHECK(p.var("got").f == 0.0f);
}

TEST_CASE("codegen: inheritance is emitted as inheritance in both failure modes")
{
	// The class frame is the same either way — only the Cast lowering differs by
	// mode (see the next case), and that difference must not quietly change what
	// a derived class IS.
	std::vector<HE::hccg::ClassSource> pair;
	for (auto& s : hcfix::all())
		if (s.key == "fix/inherit_base" || s.key == "fix/inherit_derived")
			pair.push_back(std::move(s));
	REQUIRE(pair.size() == 2);

	for (const HE::hccg::OnFailure mode : { HE::hccg::OnFailure::Interpret,
	                                        HE::hccg::OnFailure::Stop })
	{
		HE::hccg::Options opt;
		opt.onFailure = mode;
		std::vector<HE::hccg::ClassSource> copy = pair;
		HE::hccg::Result r = HE::hccg::generate(std::move(copy), opt);
		REQUIRE(r.ok);
		REQUIRE(r.fallbacks.empty());
		std::string all;
		for (const auto& f : r.files) all += f.contents;

		CHECK(all.find("class C_inherit_derived final : public C_inherit_base") != std::string::npos);
		// The base cannot be final — something derives from it.
		CHECK(all.find("class C_inherit_base : public HorizonCode::CompiledInstance") != std::string::npos);
		// One table for the chain, and the unhandled cases passed upward.
		CHECK(all.find("hc::concatSlots(C_inherit_base::slots()") != std::string::npos);
		CHECK(all.find("C_inherit_base::fireEvent(name, elem, arg)") != std::string::npos);
		CHECK(all.find("C_inherit_base::reseedVariables()") != std::string::npos);
		// The inherited members are reachable from the derived bodies: a direct
		// member write, and a direct qualified call.
		CHECK(all.find("v_hits = 99.0f") != std::string::npos);
		CHECK(all.find("C_inherit_base::Damage(") != std::string::npos);
		CHECK(all.find("\nprotected:") != std::string::npos);
	}
}

TEST_CASE("codegen: a class whose base did not compile ships interpreted with it")
{
	// `C_Goblin : public C_Enemy` cannot exist without C_Enemy. Handing in the
	// derived class alone must not produce a class with a missing base — it has
	// to fall back, which is the answer that keeps ONE object per instance.
	HE::hccg::ClassSource derived;
	for (auto& s : hcfix::all())
		if (s.key == "fix/inherit_derived") { derived = std::move(s); break; }
	REQUIRE(!derived.key.empty());

	HE::hccg::Options opt;   // Interpret: a fallback is an answer, not a failure
	HE::hccg::Result r = HE::hccg::generate({ std::move(derived) }, opt);
	CHECK(r.ok);
	REQUIRE(r.fallbacks.size() == 1);
	CHECK(r.fallbacks[0].key == "fix/inherit_derived");
	CHECK(r.fallbacks[0].reason.find("fix/inherit_base") != std::string::npos);
}

TEST_CASE("codegen: Cast lowers through the seam when other classes may be interpreted")
{
	// The other half of the story: with OnFailure::Interpret an interpreted
	// instance can turn up in the same run, and hc::as would answer null for it
	// — so the emission has to go through hc::castRef, which lands on the very
	// Runtime::instanceIsA the interpreter asks. Checked on the emitted text
	// because the compiled fixtures above can only ever exercise one mode.
	HE::hccg::ClassSource src;
	for (auto& s : hcfix::all())
		if (s.key == "fix/casts") { src = std::move(s); break; }
	REQUIRE(!src.key.empty());

	auto emitFor = [&](HE::hccg::OnFailure mode)
	{
		HE::hccg::Options opt;
		opt.onFailure = mode;
		HE::hccg::ClassSource copy = src;
		HE::hccg::Result r = HE::hccg::generate({ std::move(copy) }, opt);
		REQUIRE(r.ok);
		std::string all;
		for (const auto& f : r.files) all += f.contents;
		return all;
	};

	const std::string interp = emitFor(HE::hccg::OnFailure::Interpret);
	CHECK(interp.find("hc::castRef(") != std::string::npos);
	CHECK(interp.find("hc::castBase(") == std::string::npos);
	CHECK(interp.find("hc::as<") == std::string::npos);

	// Under Stop the engine-base casts take castBase. The exact-class cast falls
	// back to the seam HERE only because this one-class run did not compile
	// fix/cast_target — that fallback is deliberate (a target with no generated
	// C++ type has nothing to name), and the fixture parity case above covers
	// the hc::as path with the whole set compiled.
	const std::string stop = emitFor(HE::hccg::OnFailure::Stop);
	CHECK(stop.find("hc::castBase(") != std::string::npos);
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
			HE::hccg::ClassSource src;
			for (auto& s : hcfix::all())
				if (s.key == key) { src = std::move(s); break; }
			REQUIRE(!src.key.empty());
			// Same identity either way — which backend serves an instance must
			// never change what class it IS.
			const ClassIdentity cls{ src.key, src.baseClass };
			if (compiled)
			{
				const CompiledClassEntry* e = findCompiled(key);
				REQUIRE(e != nullptr);
				return rt.addCompiled(CompiledPtr(e->create(), CompiledDeleter{ e->destroy }), {}, cls);
			}
			return rt.add(std::move(src.graph), {}, cls);
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

	// The dispatch BUDGET: a bind cycle of RELAYING listeners branches the
	// dispatch tree exponentially (~2^32 fires with the depth guard alone) —
	// the per-cascade budget must cut it off quickly and identically for every
	// population. Before the budget existed this case effectively hung.
	auto relayCycleGot = [](bool l1Compiled, bool l2Compiled) -> std::pair<float, float>
	{
		Mixed m;
		m.l1 = m.make("fix/dispatch_listener", l1Compiled);
		m.l2 = m.make("fix/dispatch_listener", l2Compiled);
		m.rt.fireEvent(m.l1, "Setup", 0, Value::ofRef(m.l2));
		m.rt.fireEvent(m.l2, "Setup", 0, Value::ofRef(m.l1));
		m.rt.fireEvent(m.l1, "Sig", 0, Value::ofFloat(1.0f));
		// A second cascade gets a FRESH budget (reset at depth 0).
		m.rt.fireEvent(m.l1, "Sig", 0, Value::ofFloat(0.0f));
		return { m.got(m.l1), m.got(m.l2) };
	};
	const auto rr = relayCycleGot(false, false);
	CHECK(rr.first > 0.0f);
	const auto rc = relayCycleGot(true, true);
	const auto rm = relayCycleGot(true, false);
	CHECK(rc.first == rr.first);
	CHECK(rc.second == rr.second);
	CHECK(rm.first == rr.first);
	CHECK(rm.second == rr.second);
}

TEST_CASE("codegen parity: latent_flow (Delay, Do Once, Flip Flop, Is Valid)")
{
	ParityPair p("fix/latent_flow");

	// Do Once: only the first fire passes (per instance).
	p.fire("Once");
	p.fire("Once");
	p.fire("Once");
	CHECK(p.var("once").f == 1.0f);
	// reseedVariables also resets node state — Do Once fires again.
	p.reseed();
	p.fire("Once");
	CHECK(p.var("once").f == 1.0f);   // vars were reseeded too: 0 + 1

	// Flip Flop alternates starting with A; IsA reports the side just taken.
	p.fire("Flip");
	CHECK(p.var("isA").b == true);
	p.fire("Flip");
	p.fire("Flip");
	CHECK(p.var("flip").s == "ABA");

	// Is Valid: own ref lives, a made-up one doesn't.
	p.fire("Check", 0, Value::ofRef(p.interp.id));
	CHECK(p.var("valid").b == true);
	p.fire("Check", 0, Value::ofRef(999999));
	CHECK(p.var("valid").b == false);

	// Delay: fire runs the chain up TO the Delay; the continuation runs when
	// Runtime::update crosses the duration. Retriggering while pending is
	// ignored (one continuation, not two).
	p.fire("Wait");
	CHECK(p.var("n").f == 1.0f);      // pre-Delay half ran
	p.update(0.5f);
	CHECK(p.var("n").f == 1.0f);      // not yet
	p.fire("Wait");                    // re-trigger: pre-half again, schedule IGNORED
	CHECK(p.var("n").f == 2.0f);
	p.update(0.6f);
	CHECK(p.var("n").f == 12.0f);     // ONE continuation (+10), not two
	p.update(5.0f);
	CHECK(p.var("n").f == 12.0f);     // nothing pending anymore

	// A destroyed instance never resumes (no ghost continuation).
	p.fire("Wait", 0, {}, /*compare=*/true);
	p.interp.rt.destroy(p.interp.id);
	p.comp.rt.destroy(p.comp.id);
	p.interp.rt.update(2.0f);
	p.comp.rt.update(2.0f);
	CHECK_FALSE(p.interp.rt.alive(p.interp.id));
	CHECK_FALSE(p.comp.rt.alive(p.comp.id));
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

// ── user-defined types ───────────────────────────────────────────────────────
// The definitions live in fixtures.h (registerTypes), so the interpreter and
// hc_codegen resolve the SAME entries/fields — the whole point of the parity
// pair being one graph and two backends.

TEST_CASE("codegen parity: enums")
{
	ParityPair p("fix/enums");

	// Sparse entry values: the default is the entry NAME "Angry" = 5, and the
	// name comes back through EnumToString / the underlying int through ToInt.
	p.fire("Init");
	CHECK(p.var("m").type == PinType::Enum);
	CHECK(p.var("m").i == 5);
	CHECK(p.var("ms").s == "Angry");
	CHECK(p.var("mi").i == 5);
	CHECK(p.var("back").i == 1);          // IntToEnum(1) → Happy

	// Switch routes on the VALUE via the entry name (renumber-safe), and an
	// unclaimed value takes Default.
	Value happy; happy.type = PinType::Enum; happy.typeName = hcfix::kMoodEnum; happy.i = 1;
	p.fire("Pick", 0, happy);
	CHECK(p.var("route").s == "happy");
	Value none; none.type = PinType::Enum; none.typeName = hcfix::kMoodEnum; none.i = 99;
	p.fire("Pick", 0, none);
	CHECK(p.var("route").s == "other");
	// A dynamic Value boundary: an Int coerces into an enum pin, a Bool does not.
	p.fire("Pick", 0, Value::ofInt(0));
	CHECK(p.var("route").s == "calm");
	p.fire("Pick", 0, Value::ofBool(true));
	CHECK(p.var("route").s == "calm");    // Bool → 0 → Calm (coerce, §3.3)

	p.fire("Calm");
	CHECK(p.var("m").i == 0);

	// Enum arrays: elements carry the entry values, reads come back as enums.
	p.fire("Push");
	CHECK(p.var("marr").items.size() == 1);
	CHECK(p.var("first").i == 5);
	p.fire("Push");
	CHECK(p.var("marr").items.size() == 2);

	// Two entries on value 1: findValue answers "A", so "B" is unreachable —
	// and the generated switch must not carry two `case 1:` labels at all.
	p.fire("Dup");
	CHECK(p.var("ds").s == "A");
	CHECK(p.var("route").s == "A");

	p.reseed();
	CHECK(p.var("m").i == 5);
}

TEST_CASE("codegen parity: structs")
{
	ParityPair p("fix/structs");

	// Seeding: the definition's field defaults, with this graph's override on
	// top (hp 100 → 42), nested struct and array fields included.
	const Value s0 = p.var("s");
	CHECK(s0.type == PinType::Struct);
	CHECK(s0.typeName == hcfix::kStatsType);
	REQUIRE(s0.items.size() == 6);
	CHECK(s0.items[0].f == 42.0f);        // per-graph override
	CHECK(s0.items[1].i == 3);            // definition default
	CHECK(s0.items[2].i == 1);            // enum default by entry NAME ("Happy")
	CHECK(s0.items[4].items.size() == 2); // authored array slots

	// Reads: one field, a nested field, a whole break, the enum field by name.
	p.fire("Read");
	CHECK(p.var("hp").f == 42.0f);
	CHECK(p.var("tag").s == "in");
	CHECK(p.var("lvl").i == 3);
	CHECK(p.var("moodName").s == "Happy");
	CHECK(p.var("hit0").f == 2.0f);

	// Set Struct Field is a pure copy-with-one-field-replaced.
	p.fire("Bump");
	CHECK(p.var("s").items[0].f == 43.0f);
	CHECK(p.var("s").items[1].i == 3);    // everything else survives the copy
	p.fire("Read");
	CHECK(p.var("hp").f == 43.0f);

	// An unwired Struct input re-seeds from the definition, it is not zeros.
	p.fire("Fresh");
	{
		const Value sf = p.var("s");
		REQUIRE(sf.items.size() == 6);
		CHECK(sf.items[0].f == 5.0f);     // the written field
		CHECK(sf.items[1].i == 3);        // definition default, NOT 0
		CHECK(sf.items[4].items.size() == 2);
	}

	// Make Struct: unwired pins keep the DEFINITION's defaults, not zeros.
	p.fire("Make");
	CHECK(p.var("s").items[0].f == 7.0f);
	CHECK(p.var("s").items[1].i == 3);
	CHECK(p.var("s").items[2].i == 1);

	// Struct arrays: Add copies; Contains/IndexOf can never match, because
	// valueEquals has no Struct case (§3.4) — both backends agree on that.
	p.fire("Push");
	CHECK(p.var("arr").items.size() == 1);
	CHECK(p.var("found").b == false);
	CHECK(p.var("idx").i == -1);

	// A struct across the callFunction Value seam, in and out.
	Value in = HE::TypeRegistry::instance().makeDefaultValue(hcfix::kStatsType);
	auto [ok, res] = p.call("Scale", true, { in, Value::ofFloat(2.0f) });
	CHECK(ok);
	REQUIRE(res.size() == 1);
	CHECK(res[0].type == PinType::Struct);
	CHECK(res[0].typeName == hcfix::kStatsType);
	REQUIRE(res[0].items.size() == 6);
	CHECK(res[0].items[0].f == 200.0f);   // definition default 100 × 2
	CHECK(res[0].items[1].i == 3);        // untouched fields survive

	// NOTE: passing a NON-struct Value where a struct is expected is a
	// sharpened non-goal (§3.6). The interpreter's struct Value remembers that
	// it failed to coerce (empty typeName) and Set Struct Field then re-seeds it
	// from the definition, while a generated S_Stats is statically the right
	// type and stays at its zeros. Both are self-consistent; reproducing the
	// interpreter's would cost a hidden "am I real" flag on every struct.

	p.reseed();
	CHECK(p.var("s").items[0].f == 42.0f);
}

TEST_CASE("codegen parity: gi_caller (the GameInstance resolves without a lookup)")
{
	ParityPair p("fix/gi_caller");
	p.fire("Bump");
	CHECK(p.var("seen").f == 3.0f);     // public variable, written then read back
	CHECK(p.var("total").f == 13.0f);   // public function on the GameInstance
	CHECK(p.var("sneak").f == 0.0f);    // private one still refused, via the seam
	p.fire("Bump");
	CHECK(p.var("total").f == 13.0f);   // Set 3 again, then +10
}

TEST_CASE("codegen: the engine-event hooks do exactly what the named path does")
{
	// Step A of the native event route: a compiled class overrides the
	// CompiledInstance hook for every engine event its graph handles. Nothing
	// delivers through them yet (that is the engine's side), so this drives the
	// interpreted world BY NAME and the compiled one THROUGH THE HOOK and
	// demands the two stay indistinguishable — the precondition for switching
	// the engine's call sites over.
	ParityPair p("fix/engine_events");
	REQUIRE(p.compInst != nullptr);

	// Element 0 handler answers for every element; the element-2 one only for 2.
	p.interp.rt.fireEvent(p.interp.id, "OnClicked", 1, {});
	p.compInst->onClicked(1);
	p.checkParity();
	CHECK(p.var("trace").s == "a");

	p.interp.rt.fireEvent(p.interp.id, "OnClicked", 2, {});
	p.compInst->onClicked(2);
	p.checkParity();
	CHECK(p.var("trace").s == "aab");   // elem 0 handler runs too, then elem 2

	// A typed argument arrives without ever being boxed by the caller.
	p.interp.rt.fireEvent(p.interp.id, "OnTextChanged", 0, Value::ofString("hi"));
	p.compInst->onTextChanged(0, "hi");
	p.checkParity();
	CHECK(p.var("text").s == "hi");

	p.interp.rt.fireEvent(p.interp.id, "Tick", 0, Value::ofFloat(0.25f));
	p.compInst->onTick(0.25f);
	p.checkParity();
	CHECK(p.var("sum").f == 0.25f);

	p.interp.rt.fireEvent(p.interp.id, "Construct", 0, {});
	p.compInst->onConstruct();
	p.checkParity();
	CHECK(p.var("built").f == 1.0f);

	// An event the graph does not handle is reachable and silent, not a crash.
	p.compInst->onShutdown();
	p.compInst->onValueChanged(3, 1.0f);
	p.checkParity();
}
