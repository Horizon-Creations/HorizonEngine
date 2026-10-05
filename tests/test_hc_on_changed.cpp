// Notify on Change (docs/bind-to-variable-binding-plan.md §4, Thema 137): a
// variable marked notifyChange reports its own change at the frame-end compare
// (Runtime::exchangeState) — OnChanged_<Var>(Old) on the owner, "<Var>Changed"
// with the new value to everyone bound to it per Bind Event — and scripts can
// subscribe to any public variable through Runtime::watch. Interpreted Runtime
// driven directly; the compiled side reads the same flag from varInfos().
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Diagnostics/Log.h>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	Variable intVar(const std::string& name, int def = 0)
	{
		Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
		return v;
	}
	Variable notifying(Variable v) { v.notifyChange = true; return v; }

	// The owner: Score (Notify on Change) and a private
	// OnChanged_Score(Old:Int) that writes Old into OldSeen.
	Graph ownerGraph(int score = 5, bool withHandler = true)
	{
		Graph g;
		g.variables = { notifying(intVar("Score", score)), intVar("OldSeen", -1) };
		if (withHandler)
		{
			Node fe; fe.type = NodeType::FunctionEntry; fe.s = "OnChanged_Score"; fe.access = 1;
			fe.params = { { "Old", PinType::Int } };
			const int f = g.addNode(fe);
			Node sv; sv.type = NodeType::SetVariable; sv.s = "OldSeen"; sv.propType = PinType::Int;
			const int set = g.addNode(sv);
			REQUIRE(g.connect(f, 0, set, 0));
			REQUIRE(g.connect(f, 1, set, 2));
		}
		return g;
	}

	// A listener handling `event`: with an argument of `argType` it stores it
	// in Got; without one it copies Marker (7) into Got.
	Graph listenerGraph(const std::string& event, bool withArg, PinType argType = PinType::Int,
	                    const std::string& typeName = {})
	{
		Graph g;
		g.variables = { intVar("Got", 0), intVar("Marker", 7) };
		Node ev; ev.type = NodeType::Event; ev.s = event;
		if (withArg) { ev.hasArg = true; ev.propType = argType; ev.typeName = typeName; }
		const int e = g.addNode(ev);
		Node sv; sv.type = NodeType::SetVariable; sv.s = "Got"; sv.propType = PinType::Int;
		const int set = g.addNode(sv);
		REQUIRE(g.connect(e, 0, set, 0));
		// A non-numeric argument cannot feed an Int Set: such a listener
		// copies Marker instead, which still shows whether it fired.
		if (withArg && (argType == PinType::Int || argType == PinType::Float))
			REQUIRE(g.connect(e, 1, set, 2));
		else
		{
			Node gv; gv.type = NodeType::GetVariable; gv.s = "Marker"; gv.propType = PinType::Int;
			const int get = g.addNode(gv);
			REQUIRE(g.connect(get, 0, set, 2));
		}
		return g;
	}

	struct NotifyWarnings
	{
		static inline std::vector<std::string> seen;
		int handle = 0;
		NotifyWarnings()
		{
			seen.clear();
			handle = HE::Log::addSink([](const HE::Log::Record& r, void*)
			{
				const std::string m = r.message ? r.message : "";
				if (m.find("Notify on Change") != std::string::npos) seen.push_back(m);
			}, nullptr);
		}
		~NotifyWarnings() { HE::Log::removeSink(handle); }
		size_t count() const { return seen.size(); }
	};

	int got(const Runtime& rt, InstanceId id, const char* var) { return rt.getVariable(id, var).i; }
}

TEST_CASE("Notify on Change: OnChanged_<Var>(Old) once per frame, never for the starting state")
{
	Runtime rt;
	const InstanceId o = rt.add(ownerGraph(5));
	// Construct-time writes are starting state: the first compare only baselines.
	rt.setVariable(o, "Score", Value::ofInt(6));
	CHECK(rt.exchangeState() == 0);
	CHECK(got(rt, o, "OldSeen") == -1);

	rt.setVariable(o, "Score", Value::ofInt(8));
	CHECK(got(rt, o, "OldSeen") == -1);            // not before the frame's end
	CHECK(rt.exchangeState() > 0);
	CHECK(got(rt, o, "OldSeen") == 6);

	// Twice in one frame: one report, Old = the value at the last compare.
	rt.setVariable(o, "OldSeen", Value::ofInt(-1));
	rt.exchangeState();                            // OldSeen itself is not watched
	rt.setVariable(o, "Score", Value::ofInt(9));
	rt.setVariable(o, "Score", Value::ofInt(10));
	rt.exchangeState();
	CHECK(got(rt, o, "OldSeen") == 8);

	// Written and put back in the same frame: nothing.
	rt.setVariable(o, "OldSeen", Value::ofInt(-1));
	rt.setVariable(o, "Score", Value::ofInt(99));
	rt.setVariable(o, "Score", Value::ofInt(10));
	CHECK(rt.exchangeState() == 0);
	CHECK(got(rt, o, "OldSeen") == -1);
}

TEST_CASE("Notify on Change: without the flag nothing reports, without the function nothing breaks")
{
	Runtime rt;
	Graph plain = ownerGraph(5);
	plain.variables[0].notifyChange = false;
	const InstanceId p = rt.add(plain);
	const InstanceId bare = rt.add(ownerGraph(5, /*withHandler=*/false));
	const InstanceId l = rt.add(listenerGraph("ScoreChanged", true));
	rt.bindEvent(bare, "ScoreChanged", l);
	rt.exchangeState();
	rt.setVariable(p, "Score", Value::ofInt(1));
	rt.setVariable(bare, "Score", Value::ofInt(2));
	rt.exchangeState();
	CHECK(got(rt, p, "OldSeen") == -1);            // negative control: no flag, no call
	CHECK(got(rt, l, "Got") == 2);                 // no OnChanged_ function: listeners still hear it
}

TEST_CASE("Notify on Change: listeners get <Var>Changed with the new value, typed per listener")
{
	NotifyWarnings warn;
	Runtime rt;
	const InstanceId o = rt.add(ownerGraph(5));
	const InstanceId typed = rt.add(listenerGraph("ScoreChanged", true, PinType::Int));
	const InstanceId asFloat = rt.add(listenerGraph("ScoreChanged", true, PinType::Float));
	const InstanceId noArg = rt.add(listenerGraph("ScoreChanged", false));
	const InstanceId wrong = rt.add(listenerGraph("ScoreChanged", true, PinType::Struct, "test/none.hasset"));
	for (InstanceId l : { typed, asFloat, noArg, wrong }) rt.bindEvent(o, "ScoreChanged", l);
	rt.exchangeState();
	CHECK(got(rt, typed, "Got") == 0);             // the baseline fires nothing

	rt.setVariable(o, "Score", Value::ofInt(12));
	rt.exchangeState();
	CHECK(got(rt, typed, "Got") == 12);
	CHECK(got(rt, asFloat, "Got") == 12);          // numeric, as the pull converts
	CHECK(got(rt, noArg, "Got") == 7);             // fired without the value
	CHECK(got(rt, wrong, "Got") == 0);             // skipped ...
	CHECK(warn.count() == 1);                      // ... with one warning
	rt.setVariable(o, "Score", Value::ofInt(13));
	rt.exchangeState();
	CHECK(warn.count() == 1);                      // once per listener class, not per frame
}

TEST_CASE("Notify on Change: <Var>Changed is not passed on to the listener's listeners")
{
	Runtime rt;
	const InstanceId a = rt.add(ownerGraph(5));
	const InstanceId l = rt.add(listenerGraph("ScoreChanged", true));
	const InstanceId m = rt.add(listenerGraph("ScoreChanged", true));
	rt.bindEvent(a, "ScoreChanged", l);
	rt.bindEvent(l, "ScoreChanged", m);
	rt.exchangeState();
	rt.setVariable(a, "Score", Value::ofInt(3));
	rt.exchangeState();
	CHECK(got(rt, l, "Got") == 3);
	CHECK(got(rt, m, "Got") == 0);
}

TEST_CASE("Notify on Change: a Bind To push reports, the first one through a reference too")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance([] { Graph g; g.variables = { intVar("Score", 7) }; return g; }());
	Graph viaGi = ownerGraph(0);
	viaGi.variables[0].pullSource = kPullFromGameInstance;
	viaGi.variables[0].pullVar = "Score";
	viaGi.variables[0].bindTo = true;
	const InstanceId h = rt.add(viaGi);
	CHECK(got(rt, h, "Score") == 7);
	rt.exchangeState();
	CHECK(got(rt, h, "OldSeen") == -1);            // the pulled start value is starting state
	rt.setVariable(gi, "Score", Value::ofInt(9));
	rt.exchangeState();
	CHECK(got(rt, h, "Score") == 9);
	CHECK(got(rt, h, "OldSeen") == 7);

	// Through a reference: null at first, the first push (0 → 4) reports.
	Graph src; src.variables = { intVar("Score", 4) };
	const InstanceId s = rt.add(src);
	Graph viaRef = ownerGraph(0);
	{ Variable r; r.name = "Source"; r.type = PinType::Ref; viaRef.variables.push_back(r); }
	viaRef.variables[0].pullSource = kPullFromRef;
	viaRef.variables[0].pullVar = "Score";
	viaRef.variables[0].bindTo = true;
	viaRef.variables[0].pullRef = "Source";
	const InstanceId c = rt.add(viaRef);
	rt.setVariable(c, "Source", Value::ofRef(s));
	rt.exchangeState();
	CHECK(got(rt, c, "Score") == 4);
	CHECK(got(rt, c, "OldSeen") == 0);
}

TEST_CASE("Notify on Change: the Game Instance reports too")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(ownerGraph(1));
	rt.exchangeState();
	rt.setVariable(gi, "Score", Value::ofInt(2));
	rt.exchangeState();
	CHECK(got(rt, gi, "OldSeen") == 1);
}

TEST_CASE("Notify on Change: a handler that writes another watched variable settles in the same call")
{
	Runtime rt;
	// OnChanged_Score writes OldSeen; OldSeen is watched too, and its own
	// listener must hear it within the same exchangeState.
	Graph g = ownerGraph(5);
	g.variables[1].notifyChange = true;
	const InstanceId o = rt.add(g);
	const InstanceId l = rt.add(listenerGraph("OldSeenChanged", true));
	rt.bindEvent(o, "OldSeenChanged", l);
	rt.exchangeState();
	rt.setVariable(o, "Score", Value::ofInt(6));
	rt.exchangeState();
	CHECK(got(rt, o, "OldSeen") == 5);
	CHECK(got(rt, l, "Got") == 5);
}

TEST_CASE("Notify on Change: script subscriptions report through the host hook only")
{
	Runtime rt;
	struct Call { InstanceId owner; std::string var; int old, now; std::vector<uint64_t> tokens; };
	std::vector<Call> calls;
	rt.onVariableChanged = [&](InstanceId owner, const std::string& var, const Value& old,
	                           const Value& now, const std::vector<uint64_t>& tokens)
	{ calls.push_back({ owner, var, old.i, now.i, tokens }); };

	Graph g = ownerGraph(5);
	g.variables[0].notifyChange = false;            // not declared: a pure subscription
	{ Variable hidden = intVar("Hidden", 0); hidden.access = 1; g.variables.push_back(hidden); }
	const InstanceId o = rt.add(g);
	CHECK(rt.watch(o, "Score", 11));
	CHECK(rt.watch(o, "Score", 12));
	CHECK_FALSE(rt.watch(o, "Hidden", 11));         // private: not a door from outside
	CHECK_FALSE(rt.watch(o, "Nope", 11));
	CHECK_FALSE(rt.watch(9999, "Score", 11));
	rt.exchangeState();
	CHECK(calls.empty());

	rt.setVariable(o, "Score", Value::ofInt(6));
	rt.exchangeState();
	REQUIRE(calls.size() == 1);
	CHECK(calls[0].owner == o);
	CHECK(calls[0].var == "Score");
	CHECK(calls[0].old == 5);
	CHECK(calls[0].now == 6);
	CHECK(calls[0].tokens == std::vector<uint64_t>{ 11, 12 });
	CHECK(got(rt, o, "OldSeen") == -1);            // a subscription calls no OnChanged_

	rt.unwatch(11);
	rt.setVariable(o, "Score", Value::ofInt(7));
	rt.exchangeState();
	REQUIRE(calls.size() == 2);
	CHECK(calls[1].tokens == std::vector<uint64_t>{ 12 });

	rt.unwatch(o, "Score", 12);
	rt.setVariable(o, "Score", Value::ofInt(8));
	rt.exchangeState();
	CHECK(calls.size() == 2);

	// The owner's death drops its subscriptions: nothing reports a ghost.
	CHECK(rt.watch(o, "Score", 13));
	rt.remove(o);
	CHECK(rt.exchangeState() == 0);
	CHECK(calls.size() == 2);
}

TEST_CASE("Notify on Change: JSON keeps the flag, a function-local drops it")
{
	Graph g;
	g.variables = { notifying(intVar("Score")), intVar("Plain") };
	Variable local = notifying(intVar("Tmp")); local.scope = 3;
	g.variables.push_back(local);
	const std::string json = toJson(g);
	CHECK(json.find("notifyChange") != std::string::npos);
	Graph back;
	REQUIRE(fromJson(json, back));
	REQUIRE(back.variables.size() == 3);
	CHECK(back.variables[0].notifyChange);
	CHECK_FALSE(back.variables[1].notifyChange);
	CHECK_FALSE(back.variables[2].notifyChange);
}
