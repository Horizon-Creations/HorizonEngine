// Bind To (docs/bind-to-variable-binding-plan.md §3, Thema 137): a variable
// whose pull is marked bindTo stays bound to its source; Runtime::exchangeState,
// which the hosts call at every frame's end, writes the source's value into it
// whenever the source moved. These tests drive the interpreted Runtime
// directly — the compiled side runs through the same exchangeState and is
// compared against it in the codegen parity suite (fixture "bind_to").
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Diagnostics/Log.h>
#include <Types/TypeRegistry.h>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	constexpr const char* kBindStats = "test/bind/BindStats.hasset";

	Variable intVar(const std::string& name, int def = 0)
	{
		Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
		return v;
	}
	Variable refVar(const std::string& name)
	{
		Variable v; v.name = name; v.type = PinType::Ref;
		return v;
	}
	Variable pulling(Variable v, const char* src, const std::string& var,
	                 const std::string& member = {}, const std::string& cls = {})
	{
		v.pullSource = src; v.pullVar = var; v.pullMember = member; v.pullClass = cls;
		return v;
	}
	Variable bound(Variable v, const char* src, const std::string& var,
	               const std::string& member = {}, const std::string& ref = {})
	{
		v = pulling(std::move(v), src, var, member);
		v.bindTo = true;
		v.pullRef = ref;
		return v;
	}
	Graph giWith(std::vector<Variable> vars)
	{
		Graph g;
		g.variables = std::move(vars);
		return g;
	}

	// Counts the Bind To warnings while alive.
	struct BindWarnings
	{
		static inline std::vector<std::string> seen;
		int handle = 0;
		BindWarnings()
		{
			seen.clear();
			handle = HE::Log::addSink([](const HE::Log::Record& r, void*)
			{
				const std::string m = r.message ? r.message : "";
				if (m.find("Bind To") != std::string::npos) seen.push_back(m);
			}, nullptr);
		}
		~BindWarnings() { HE::Log::removeSink(handle); }
		size_t count() const { return seen.size(); }
	};

	const Runtime::BindOutcome* binding(const Runtime& rt, InstanceId id, const std::string& name)
	{
		static std::vector<Runtime::BindOutcome> keep;
		keep = rt.boundVariablesOf(id);
		for (const auto& b : keep)
			if (b.name == name) return &b;
		return nullptr;
	}

	void registerBindStats()
	{
		HE::StructDef def;
		def.name = "BindStats"; def.assetPath = kBindStats;
		HE::StructField score; score.name = "Score"; score.type = PinType::Int;
		score.defaultValue = Value::ofInt(0);
		score.formerNames = { "Points" };   // renamed once: Points → Score
		def.fields = { score };
		HE::TypeRegistry::instance().registerStruct(def);
	}
}

TEST_CASE("Bind To: follows the Game Instance for the whole life, Pull on Construct does not")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(giWith({ intVar("Score", 7) }));
	Graph hud;  hud.variables  = { bound(intVar("Score"), kPullFromGameInstance, "Score") };
	Graph snap; snap.variables = { pulling(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId h = rt.add(hud);
	const InstanceId s = rt.add(snap);
	// Construct-time pull for both: the value is there before any event.
	CHECK(rt.getVariable(h, "Score").i == 7);
	CHECK(rt.getVariable(s, "Score").i == 7);
	// Nothing moved: the first compare writes nothing (the pull IS the start).
	CHECK(rt.exchangeState() == 0);

	rt.setVariable(gi, "Score", Value::ofInt(9));
	CHECK(rt.getVariable(h, "Score").i == 7);      // not before the frame's end
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(h, "Score").i == 9);
	// Negative control: the plain pull stays a snapshot.
	CHECK(rt.getVariable(s, "Score").i == 7);
	CHECK(rt.boundVariablesOf(s).empty());
	const auto* b = binding(rt, h, "Score");
	REQUIRE(b != nullptr);
	CHECK(b->boundTo == gi);
	CHECK(b->why == PullFailure::None);

	// A quiet frame writes nothing — the host's "redraw?" answer.
	CHECK(rt.exchangeState() == 0);
}

TEST_CASE("Bind To: a local write stays until the source moves again")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(giWith({ intVar("Score", 7) }));
	Graph g; g.variables = { bound(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.add(g);
	rt.setVariable(id, "Score", Value::ofInt(3));
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.getVariable(id, "Score").i == 3);
	rt.setVariable(gi, "Score", Value::ofInt(8));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "Score").i == 8);
}

TEST_CASE("Bind To: a spawn value set after the pull stays until the source moves")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(giWith({ intVar("Score", 7) }));
	Graph g; g.variables = { bound(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.add(g);
	// Expose on Spawn / a hot-reload restore: both land after registration.
	REQUIRE(rt.setPublicVariable(id, "Score", Value::ofInt(50)));
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.getVariable(id, "Score").i == 50);
	rt.setVariable(gi, "Score", Value::ofInt(51));
	rt.exchangeState();
	CHECK(rt.getVariable(id, "Score").i == 51);
}

TEST_CASE("Bind To: a reference source — waits quietly, follows, re-targets, rests when its target dies")
{
	BindWarnings warnings;
	Runtime rt;
	Graph src; src.variables = { intVar("Hp", 0) };
	const InstanceId a = rt.add(src, {}, { "test/Src.hasset", "Object" });
	const InstanceId b = rt.add(src, {}, { "test/Src.hasset", "Object" });
	rt.setVariable(a, "Hp", Value::ofInt(5));
	rt.setVariable(b, "Hp", Value::ofInt(5));

	Graph bar;
	bar.variables = { refVar("Target"),
	                  bound(intVar("Hp", -1), kPullFromRef, "Hp", {}, "Target") };
	const InstanceId id = rt.add(bar, {}, { "test/Bar.hasset", "Widget" });
	// No pull at registration, and no warning about the null reference.
	CHECK(rt.getVariable(id, "Hp").i == -1);
	CHECK(rt.pulledVariablesOf(id).empty());
	CHECK(rt.exchangeState() == 0);
	CHECK(warnings.count() == 0);
	REQUIRE(binding(rt, id, "Hp") != nullptr);
	CHECK(binding(rt, id, "Hp")->boundTo == 0);
	CHECK(binding(rt, id, "Hp")->why == PullFailure::None);

	// Assigned → pushed at the same frame's end.
	rt.setVariable(id, "Target", Value::ofRef(a));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "Hp").i == 5);
	CHECK(binding(rt, id, "Hp")->boundTo == a);

	// Re-targeted onto an instance with the SAME value: the identity moved,
	// so it pushes anyway (a local write in between proves it wrote).
	rt.setVariable(id, "Hp", Value::ofInt(0));
	rt.setVariable(id, "Target", Value::ofRef(b));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "Hp").i == 5);
	CHECK(binding(rt, id, "Hp")->boundTo == b);

	// The target dies: the binding rests, the value stays, one warning.
	rt.setVariable(b, "Hp", Value::ofInt(6));
	rt.exchangeState();
	CHECK(rt.getVariable(id, "Hp").i == 6);
	rt.destroy(b);
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.getVariable(id, "Hp").i == 6);
	CHECK(binding(rt, id, "Hp")->boundTo == 0);
	CHECK(binding(rt, id, "Hp")->why == PullFailure::RefTargetGone);
	CHECK(warnings.count() == 1);
	REQUIRE(!warnings.seen.empty());
	CHECK(warnings.seen[0].find("test/Bar.hasset.Hp") != std::string::npos);
	CHECK(warnings.seen[0].find("Reference 'Target'") != std::string::npos);

	// Pointed somewhere alive again: back immediately.
	rt.setVariable(id, "Target", Value::ofRef(a));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "Hp").i == 5);
}

TEST_CASE("Bind To: the owner's removal takes its bindings along")
{
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 1) }));
	Graph g; g.variables = { bound(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.add(g);
	CHECK(rt.boundVariablesOf(id).size() == 1);
	rt.destroy(id);
	CHECK(rt.boundVariablesOf(id).empty());
	CHECK(rt.exchangeState() == 0);
	rt.add(g);
	rt.clear();
	CHECK(rt.exchangeState() == 0);
}

TEST_CASE("Bind To: from the Creator, which may die first")
{
	Runtime rt;
	Graph spawner; spawner.variables = { intVar("Gift", 11) };
	const InstanceId sp = rt.add(spawner, {}, { "test/Spawner.hasset", "Object" });

	// A creator is whoever was creating at registration; the runtime reads it
	// off its creator stack, which only a graph's Create Object fills. Give the
	// child its creator the same way.
	Graph child; child.variables = { bound(intVar("Gift", -1), kPullFromCreator, "Gift") };
	InstanceId childId = 0;
	Runtime::Services svc;
	svc.createObject = [&](const std::string&, const float*, const float*) -> uint32_t
	{
		childId = rt.add(child, {}, { "test/Child.hasset", "Object" });
		return childId;
	};
	rt.setServices(svc);
	Node ev; ev.type = NodeType::Event; ev.s = "Go";
	Node co; co.type = NodeType::CreateObject; co.s = "test/Child.hasset";
	Graph go = spawner;
	const int e = go.addNode(ev);
	const int c = go.addNode(co);
	REQUIRE(go.connect(e, 0, c, 0));
	const InstanceId sp2 = rt.add(go, {}, { "test/Spawner.hasset", "Object" });
	rt.fireEvent(sp2, "Go");
	REQUIRE(childId != 0);
	CHECK(rt.creatorOf(childId) == sp2);
	CHECK(rt.getVariable(childId, "Gift").i == 11);

	rt.setVariable(sp2, "Gift", Value::ofInt(12));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(childId, "Gift").i == 12);

	rt.destroy(sp2);
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.getVariable(childId, "Gift").i == 12);
	CHECK(binding(rt, childId, "Gift")->why == PullFailure::CreatorGone);
	(void)sp;
}

TEST_CASE("Bind To: a struct member, also under its former name, and the whole struct")
{
	registerBindStats();
	Runtime rt;
	Variable last; last.name = "LastRun"; last.type = PinType::Struct; last.typeName = kBindStats;
	last.structDefaults["Score"] = Value::ofInt(4);
	const InstanceId gi = rt.setGameInstance(giWith({ last }));

	Variable whole; whole.name = "Whole"; whole.type = PinType::Struct; whole.typeName = kBindStats;
	Graph g;
	g.variables = { bound(intVar("Score", -1), kPullFromGameInstance, "LastRun", "Score"),
	                bound(intVar("Old", -1), kPullFromGameInstance, "LastRun", "Points"),
	                bound(whole, kPullFromGameInstance, "LastRun") };
	const InstanceId id = rt.add(g);
	CHECK(rt.getVariable(id, "Score").i == 4);
	CHECK(rt.getVariable(id, "Old").i == 4);

	Value run = rt.getVariable(gi, "LastRun");
	REQUIRE(run.items.size() == 1);
	run.items[0] = Value::ofInt(20);
	rt.setVariable(gi, "LastRun", run);
	CHECK(rt.exchangeState() == 3);
	CHECK(rt.getVariable(id, "Score").i == 20);
	CHECK(rt.getVariable(id, "Old").i == 20);
	REQUIRE(rt.getVariable(id, "Whole").items.size() == 1);
	CHECK(rt.getVariable(id, "Whole").items[0].i == 20);
}

TEST_CASE("Bind To: a chain settles in one compare, however it was registered")
{
	// C0 ← C1 ← C2 ← C3 (each bound to the next through a reference), with
	// the consumers registered BEFORE their sources: every round moves the
	// value one link, and the compare keeps going until nothing moves.
	BindWarnings warnings;
	Runtime rt;
	Graph link;
	link.variables = { refVar("Next"), bound(intVar("V", 0), kPullFromRef, "V", {}, "Next") };
	std::vector<InstanceId> c;
	for (int i = 0; i < 4; ++i) c.push_back(rt.add(link, {}, { "test/Link.hasset", "Object" }));
	for (int i = 0; i < 3; ++i) rt.setVariable(c[i], "Next", Value::ofRef(c[i + 1]));
	CHECK(rt.exchangeState() == 3);    // first pushes: 0 everywhere, but written
	rt.setVariable(c[3], "V", Value::ofInt(9));
	CHECK(rt.exchangeState() == 3);
	for (int i = 0; i < 4; ++i) CHECK(rt.getVariable(c[i], "V").i == 9);
	CHECK(warnings.count() == 0);
}

TEST_CASE("Bind To: a chain longer than the round cap finishes next frame, with one warning")
{
	BindWarnings warnings;
	Runtime rt;
	Graph link;
	link.variables = { refVar("Next"), bound(intVar("V", 0), kPullFromRef, "V", {}, "Next") };
	std::vector<InstanceId> c;
	for (int i = 0; i < 12; ++i) c.push_back(rt.add(link, {}, { "test/Long.hasset", "Object" }));
	for (int i = 0; i < 11; ++i) rt.setVariable(c[i], "Next", Value::ofRef(c[i + 1]));
	rt.exchangeState();
	rt.exchangeState();
	rt.setVariable(c[11], "V", Value::ofInt(5));
	rt.exchangeState();
	CHECK(rt.getVariable(c[0], "V").i == 0);   // eight links moved, three to go
	CHECK(rt.getVariable(c[3], "V").i == 5);
	rt.exchangeState();
	for (InstanceId id : c) CHECK(rt.getVariable(id, "V").i == 5);
	CHECK(warnings.count() == 1);
	rt.setVariable(c[11], "V", Value::ofInt(6));
	rt.exchangeState();
	CHECK(warnings.count() == 1);              // once per session
}

TEST_CASE("Bind To: a cycle of two comes to rest")
{
	BindWarnings warnings;
	Runtime rt;
	Graph peer;
	peer.variables = { refVar("Peer"), bound(intVar("X", 0), kPullFromRef, "X", {}, "Peer") };
	const InstanceId a = rt.add(peer);
	const InstanceId b = rt.add(peer);
	rt.setVariable(a, "X", Value::ofInt(1));
	rt.setVariable(b, "X", Value::ofInt(2));
	rt.setVariable(a, "Peer", Value::ofRef(b));
	rt.setVariable(b, "Peer", Value::ofRef(a));
	rt.exchangeState();
	CHECK(rt.getVariable(a, "X").i == rt.getVariable(b, "X").i);
	CHECK(rt.exchangeState() == 0);
	rt.setVariable(a, "X", Value::ofInt(7));
	rt.exchangeState();
	CHECK(rt.getVariable(b, "X").i == 7);
	CHECK(rt.getVariable(a, "X").i == 7);
	CHECK(rt.exchangeState() == 0);
	CHECK(warnings.count() == 0);
}

TEST_CASE("Bind To: what cannot bind rests, with ONE warning per class and cause")
{
	BindWarnings warnings;
	Runtime rt;
	Variable priv = intVar("Secret", 9); priv.access = 1;
	Variable text; text.name = "Name"; text.type = PinType::String; text.s = "x";
	rt.setGameInstance(giWith({ priv, text }));
	Graph g;
	g.variables = { bound(intVar("A", 1), kPullFromGameInstance, "Secret"),
	                bound(intVar("B", 3), kPullFromGameInstance, "Name"),
	                intVar("NotARef"),
	                bound(intVar("C", 4), kPullFromRef, "V", {}, "NotARef"),
	                bound(intVar("D", 5), kPullFromRef, "V", {}, "Nowhere") };
	std::vector<InstanceId> ids;
	for (int i = 0; i < 10; ++i) ids.push_back(rt.add(g, {}, { "test/Rests.hasset", "Object" }));
	CHECK(rt.exchangeState() == 0);
	CHECK(rt.exchangeState() == 0);
	for (InstanceId id : ids)
	{
		CHECK(rt.getVariable(id, "A").i == 1);
		CHECK(rt.getVariable(id, "B").i == 3);
		CHECK(rt.getVariable(id, "C").i == 4);
		CHECK(rt.getVariable(id, "D").i == 5);
	}
	CHECK(binding(rt, ids[0], "A")->why == PullFailure::NoPublicVariable);
	CHECK(binding(rt, ids[0], "B")->why == PullFailure::TypeMismatch);
	CHECK(binding(rt, ids[0], "C")->why == PullFailure::NoRefVariable);
	CHECK(binding(rt, ids[0], "D")->why == PullFailure::NoRefVariable);
	// A and B already warned as Pull on Construct failures and say nothing
	// twice; C and D are Bind-only and warn once each, for ten instances.
	CHECK(warnings.count() == 2);
}

TEST_CASE("Bind To: a container is compared deep and copied deep")
{
	Runtime rt;
	Variable list; list.name = "List"; list.type = PinType::Int; list.isArray = true;
	list.defaultItems = { Value::ofInt(1), Value::ofInt(2) };
	const InstanceId gi = rt.setGameInstance(giWith({ list }));
	Variable mine = list; mine.defaultItems.clear();
	Graph g; g.variables = { bound(mine, kPullFromGameInstance, "List") };
	const InstanceId id = rt.add(g);
	REQUIRE(rt.getVariable(id, "List").items.size() == 2);

	// One element changes → noticed.
	Value v = rt.getVariable(gi, "List");
	v.items[1] = Value::ofInt(20);
	rt.setVariable(gi, "List", v);
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "List").items[1].i == 20);
	// The copy is deep: changing the source afterwards leaves the bound one
	// alone until the next compare.
	v.items[0] = Value::ofInt(10);
	rt.setVariable(gi, "List", v);
	CHECK(rt.getVariable(id, "List").items[0].i == 1);
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(id, "List").items[0].i == 10);
}

TEST_CASE("Bind To: a replaced Game Instance is another source")
{
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 7) }));
	Graph g; g.variables = { bound(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.add(g);
	// Same value, new instance (play started over): identity moved, so it writes.
	const InstanceId gi2 = rt.setGameInstance(giWith({ intVar("Score", 7) }));
	CHECK(rt.exchangeState() == 1);
	CHECK(binding(rt, id, "Score")->boundTo == gi2);
}

TEST_CASE("Bind To: the leaf-most declaration decides")
{
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 5) }));
	Graph base;    base.variables    = { bound(intVar("Score", 1), kPullFromGameInstance, "Score") };
	Graph derived; derived.variables = { pulling(intVar("Score", 1), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.addLevels({ base, derived }, {}, { "test/D.hasset", "Object",
	                                                            { "test/B.hasset" } });
	CHECK(rt.getVariable(id, "Score").i == 5);   // still pulls
	CHECK(rt.boundVariablesOf(id).empty());      // but the derived class unbound it
	// A derived class may bind through an INHERITED reference.
	Graph refBase;   refBase.variables = { refVar("Target") };
	Graph refChild;  refChild.variables = { bound(intVar("V", -1), kPullFromRef, "Score", {}, "Target") };
	const InstanceId rc = rt.addLevels({ refBase, refChild }, {}, { "test/RC.hasset", "Object",
	                                                               { "test/RB.hasset" } });
	rt.setVariable(rc, "Target", Value::ofRef(rt.gameInstance()));
	CHECK(rt.exchangeState() == 1);
	CHECK(rt.getVariable(rc, "V").i == 5);
}

TEST_CASE("Bind To: JSON round trip, and what the loader refuses")
{
	Graph g;
	g.variables = { bound(intVar("Score"), kPullFromGameInstance, "Score"),
	                refVar("Target"),
	                bound(intVar("Hp"), kPullFromRef, "Hp", {}, "Target"),
	                pulling(intVar("Snap"), kPullFromGameInstance, "Score") };
	Graph back;
	REQUIRE(fromJson(toJson(g), back));
	const Variable* s = back.findVariable("Score");
	REQUIRE(s != nullptr);
	CHECK(s->bindTo);
	CHECK(s->pullRef.empty());
	const Variable* hp = back.findVariable("Hp");
	REQUIRE(hp != nullptr);
	CHECK(hp->bindTo);
	CHECK(hp->pullSource == kPullFromRef);
	CHECK(hp->pullRef == "Target");
	const Variable* snap = back.findVariable("Snap");
	REQUIRE(snap != nullptr);
	CHECK_FALSE(snap->bindTo);
	// A plain pull saves exactly as before Bind To existed.
	const std::string snapJson = variableToJson(*snap);
	CHECK(snapJson.find("bind") == std::string::npos);
	CHECK(snapJson.find("ref") == std::string::npos);

	auto load = [](const std::string& json)
	{
		Variable v;
		REQUIRE(variableFromJson(json, v));
		return v;
	};
	// Replicated: the binding goes, the pull stays.
	const Variable rep = load(R"({"name":"R","type":3,"rep":true,"pull":{"src":"GameInstance","var":"S","bind":true}})");
	CHECK(rep.replicated);
	CHECK(rep.pullSource == kPullFromGameInstance);
	CHECK_FALSE(rep.bindTo);
	// A reference source without Bind To, without a reference, or on a
	// Replicated variable: dropped whole.
	CHECK(load(R"({"name":"N","type":3,"pull":{"src":"Ref","var":"S","ref":"T"}})").pullSource.empty());
	CHECK(load(R"({"name":"E","type":3,"pull":{"src":"Ref","var":"S","bind":true}})").pullSource.empty());
	const Variable repRef = load(R"({"name":"Q","type":3,"rep":true,"pull":{"src":"Ref","var":"S","bind":true,"ref":"T"}})");
	CHECK(repRef.pullSource.empty());
	CHECK_FALSE(repRef.bindTo);
	// A function-local binds nothing.
	const Variable local = load(R"({"name":"L","type":3,"scope":7,"pull":{"src":"GameInstance","var":"S","bind":true}})");
	CHECK(local.pullSource.empty());
	CHECK_FALSE(local.bindTo);
	// `ref` on a source that is not Ref is not kept.
	const Variable stray = load(R"({"name":"G","type":3,"pull":{"src":"GameInstance","var":"S","bind":true,"ref":"T"}})");
	CHECK(stray.bindTo);
	CHECK(stray.pullRef.empty());
}

TEST_CASE("Bind To: the shared sentences and the moved compare")
{
	CHECK(isKnownPullSource(kPullFromRef));
	CHECK(pullSourceLabel(kPullFromRef, "Target") == "Reference 'Target'");
	CHECK(pullFailureText(PullFailure::RefTargetGone, kPullFromRef, "Hp", {}, {}, "Target")
	      == "Reference 'Target' holds an object that was destroyed");
	CHECK(pullFailureText(PullFailure::NoRefVariable, kPullFromRef, "Hp", {}, {}, "Target")
	      .find("no object reference variable 'Target'") != std::string::npos);
	// valuesEqual lives in HE_Core now and is the whole-value rule.
	Value a = Value::ofArray(PinType::Int); a.items = { Value::ofInt(1) };
	Value b = a;
	CHECK(valuesEqual(a, b));
	b.items[0] = Value::ofInt(2);
	CHECK_FALSE(valuesEqual(a, b));
	CHECK_FALSE(valuesEqual(Value::ofInt(1), a));
	CHECK(valueTypesMatch(a, b));
}
