// Extract on Destruct (docs/state-driven-data-exchange-design.md §3, Thema 127):
// a class names one struct and which variable fills which member; when an
// instance is destroyed, after its own Destruct and before it is unregistered,
// the engine builds that struct and sends it as "OnDestroyed" to everything
// bound to the instance. These tests drive the Runtime directly, with
// interpreted owners and listeners and a hand-written compiled listener/owner;
// the generated side is compared against the interpreter in the codegen parity
// suite (fixture "extract_destruct").
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeCompiled.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Diagnostics/Log.h>
#include <Types/TypeRegistry.h>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	constexpr const char* kReport = "test/extract/EnemyReport.hasset";
	constexpr const char* kCrate  = "test/extract/CrateReport.hasset";

	Variable intVar(const std::string& name, int def = 0)
	{
		Variable v; v.name = name; v.type = PinType::Int; v.f[0] = (float)def;
		return v;
	}
	Variable floatVar(const std::string& name, float def = 0.0f)
	{
		Variable v; v.name = name; v.type = PinType::Float; v.f[0] = def;
		return v;
	}
	Variable structVar(const std::string& name, const char* path)
	{
		Variable v; v.name = name; v.type = PinType::Struct; v.typeName = path;
		return v;
	}

	// EnemyReport { Kills: Int (was "Frags"), Tier: Int, Who: Ref, Bonus: Float = 2.5 }
	// CrateReport { Coins: Int }
	void registerStructs()
	{
		HE::StructDef def;
		def.name = "EnemyReport"; def.assetPath = kReport;
		HE::StructField kills; kills.name = "Kills"; kills.type = PinType::Int;
		kills.defaultValue = Value::ofInt(0); kills.formerNames = { "Frags" };
		HE::StructField tier; tier.name = "Tier"; tier.type = PinType::Int;
		tier.defaultValue = Value::ofInt(0);
		HE::StructField who; who.name = "Who"; who.type = PinType::Ref;
		who.defaultValue = Value::ofRef(0);
		HE::StructField bonus; bonus.name = "Bonus"; bonus.type = PinType::Float;
		bonus.defaultValue = Value::ofFloat(2.5f);
		def.fields = { kills, tier, who, bonus };
		HE::TypeRegistry::instance().registerStruct(def);

		HE::StructDef crate;
		crate.name = "CrateReport"; crate.assetPath = kCrate;
		HE::StructField coins; coins.name = "Coins"; coins.type = PinType::Int;
		coins.defaultValue = Value::ofInt(0);
		crate.fields = { coins };
		HE::TypeRegistry::instance().registerStruct(crate);
	}

	// Event(event) → SetVariable(dst) = GetVariable(src).
	void eventCopies(Graph& g, const std::string& event, const std::string& src,
	                 const std::string& dst, PinType t)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node gv; gv.type = NodeType::GetVariable; gv.s = src; gv.propType = t;
		const int get = g.addNode(gv);
		Node sv; sv.type = NodeType::SetVariable; sv.s = dst; sv.propType = t;
		const int set = g.addNode(sv);
		REQUIRE(g.connect(e, 0, set, 0));
		REQUIRE(g.connect(get, 0, set, 2));
	}

	// The Enemy: Kills, LootTier, Health, FinalKills; its own Destruct writes
	// FinalKills into Kills — the "last value" the extract must carry.
	Graph enemyGraph(int kills, int finalKills, int tier)
	{
		Graph g;
		g.variables = { intVar("Kills", kills), intVar("LootTier", tier),
		                floatVar("Health", 1.0f), intVar("FinalKills", finalKills) };
		eventCopies(g, "Destruct", "FinalKills", "Kills", PinType::Int);
		g.extract.structPath = kReport;
		g.extract.map = { { "Kills", "Kills" }, { "Tier", "LootTier" }, { "Who", kExtractSelf } };
		return g;
	}

	// A listener: OnDestroyed(<struct>) → SetVariable(Got). `typeName` empty =
	// a handler without argument, which copies Marker into Got instead.
	Graph listenerGraph(const char* typeName)
	{
		Graph g;
		Node ev; ev.type = NodeType::Event; ev.s = kOnDestroyed;
		if (typeName)
		{
			g.variables = { structVar("Got", typeName), intVar("Calls", 0) };
			ev.hasArg = true; ev.propType = PinType::Struct; ev.typeName = typeName;
			const int e = g.addNode(ev);
			Node sv; sv.type = NodeType::SetVariable; sv.s = "Got"; sv.propType = PinType::Struct;
			sv.typeName = typeName;
			const int set = g.addNode(sv);
			REQUIRE(g.connect(e, 0, set, 0));
			REQUIRE(g.connect(e, 1, set, 2));
		}
		else
		{
			g.variables = { intVar("Marker", 1), intVar("Got", 0) };
			const int e = g.addNode(ev);
			Node gv; gv.type = NodeType::GetVariable; gv.s = "Marker"; gv.propType = PinType::Int;
			const int get = g.addNode(gv);
			Node sv; sv.type = NodeType::SetVariable; sv.s = "Got"; sv.propType = PinType::Int;
			const int set = g.addNode(sv);
			REQUIRE(g.connect(e, 0, set, 0));
			REQUIRE(g.connect(get, 0, set, 2));
		}
		return g;
	}

	int member(const Value& s, size_t i) { return i < s.items.size() ? s.items[i].i : -999; }

	// Counts the Extract/OnDestroyed warnings while alive.
	struct ExtractWarnings
	{
		static inline std::vector<std::string> seen;
		int handle = 0;
		ExtractWarnings()
		{
			seen.clear();
			handle = HE::Log::addSink([](const HE::Log::Record& r, void*)
			{
				const std::string m = r.message ? r.message : "";
				if (m.find("Extract on Destruct") != std::string::npos ||
				    m.find("OnDestroyed") != std::string::npos)
					seen.push_back(m);
			}, nullptr);
		}
		~ExtractWarnings() { HE::Log::removeSink(handle); }
		size_t count() const { return seen.size(); }
	};

	// A hand-written compiled listener: records every OnDestroyed it receives,
	// and — while the handler runs — whether the dying instance is still
	// readable through the @Self it was handed.
	struct Seen
	{
		int calls = 0;
		Value last;
		bool selfAliveInHandler = false;
		int  selfKillsInHandler = -1;
	};
	class CompiledListener : public CompiledInstance
	{
	public:
		CompiledListener(Seen* seen, Runtime* rt, int argType, const char* typeName)
			: m_seen(seen), m_rt(rt), m_argType(argType), m_typeName(typeName) {}
		const char* classKey() const override { return "test/extract/CompiledListener.hasset"; }
		const std::vector<CompiledEventInfo>& eventInfos() const override
		{
			m_events = { { kOnDestroyed, 0, m_argType, m_typeName } };
			return m_events;
		}
		void fireEvent(const std::string& name, int, const Value& arg) override
		{
			if (name != kOnDestroyed) return;
			++m_seen->calls;
			m_seen->last = arg;
			if (arg.items.size() >= 3)
			{
				const InstanceId who = arg.items[2].ref;
				m_seen->selfAliveInHandler = m_rt->alive(who);
				m_seen->selfKillsInHandler = m_rt->getVariable(who, "Kills").i;
			}
		}
	private:
		Seen*        m_seen;
		Runtime*     m_rt;
		int          m_argType;
		const char*  m_typeName;
		mutable std::vector<CompiledEventInfo> m_events;
	};

	// A hand-written compiled OWNER whose extractOnDestruct is what the codegen
	// would emit for the Enemy above.
	class CompiledEnemy : public CompiledInstance
	{
	public:
		int v_Kills = 4, v_LootTier = 2;
		const char* classKey() const override { return "test/extract/CompiledEnemy.hasset"; }
		bool extractOnDestruct(Value& out) const override
		{
			out = HE::TypeRegistry::instance().makeDefaultValue(kReport);
			out.items[0] = Value::ofInt(v_Kills);
			out.items[1] = Value::ofInt(v_LootTier);
			return true;
		}
	};
}

TEST_CASE("Extract on Destruct: a listener receives the struct, with the value Destruct set last")
{
	registerStructs();
	Runtime rt;
	const InstanceId enemy = rt.add(enemyGraph(/*kills*/3, /*finalKills*/9, /*tier*/2), {},
	                                { "test/Enemy.hasset", "Object" });
	const InstanceId spawner = rt.add(listenerGraph(kReport));
	rt.bindEvent(enemy, kOnDestroyed, spawner);

	// What it would send BEFORE its own Destruct ran: the old Kills. The real
	// destroy must not send this (negative control for "after Destruct").
	const Value early = rt.buildExtract(enemy);
	REQUIRE(early.items.size() == 4);
	CHECK(member(early, 0) == 3);

	rt.destroy(enemy);
	CHECK_FALSE(rt.alive(enemy));
	const Value got = rt.getVariable(spawner, "Got");
	REQUIRE(got.type == PinType::Struct);
	REQUIRE(got.items.size() == 4);
	CHECK(got.typeName == kReport);
	CHECK(member(got, 0) == 9);                      // Kills: FinalKills, set in Destruct
	CHECK(member(got, 1) == 2);                      // Tier ← LootTier
	CHECK(got.items[2].ref == enemy);                // Who ← @Self
	CHECK(got.items[3].f == doctest::Approx(2.5f));  // Bonus: unmapped → struct default
}

TEST_CASE("Extract on Destruct: @Self is readable inside the handler and dead afterwards")
{
	registerStructs();
	Runtime rt;
	Seen seen;
	const InstanceId enemy = rt.add(enemyGraph(1, 5, 0));
	const InstanceId l = rt.addCompiled(makeCompiled<CompiledListener>(
		&seen, &rt, (int)PinType::Struct, kReport));
	rt.bindEvent(enemy, kOnDestroyed, l);

	rt.destroy(enemy);
	CHECK(seen.calls == 1);
	CHECK(seen.selfAliveInHandler);          // still registered while OnDestroyed runs
	CHECK(seen.selfKillsInHandler == 5);     // and its variables readable through it
	REQUIRE(seen.last.items.size() == 4);
	CHECK(seen.last.items[2].ref == enemy);
	CHECK_FALSE(rt.alive(enemy));            // gone right after
}

TEST_CASE("Extract on Destruct: a deleted variable leaves the member at its default, warned once")
{
	registerStructs();
	ExtractWarnings warn;
	Runtime rt;
	std::vector<InstanceId> spawners;
	for (int k = 0; k < 10; ++k)
	{
		Graph g = enemyGraph(1, 7, 3);
		g.extract.map.push_back({ "Bonus", "GoneVariable" });   // not declared
		const InstanceId e = rt.add(g, {}, { "test/Broken.hasset", "Object" });
		const InstanceId s = rt.add(listenerGraph(kReport));
		rt.bindEvent(e, kOnDestroyed, s);
		rt.destroy(e);
		spawners.push_back(s);
	}
	const Value got = rt.getVariable(spawners.back(), "Got");
	REQUIRE(got.items.size() == 4);
	CHECK(member(got, 0) == 7);
	CHECK(got.items[3].f == doctest::Approx(2.5f));
	CHECK(warn.count() == 1);   // ten instances of one class, one warning
}

TEST_CASE("Extract on Destruct: type mismatch and @Self on a non-Ref keep the default")
{
	registerStructs();
	ExtractWarnings warn;
	Runtime rt;
	Graph g = enemyGraph(1, 7, 3);
	g.extract.map = { { "Kills", "Health" },    // Float into Int: converts (scalar numbers)
	                  { "Tier", kExtractSelf } }; // Self into an Int: refused
	const InstanceId e = rt.add(g, {}, { "test/Mismatch.hasset", "Object" });
	const Value v = rt.buildExtract(e);
	REQUIRE(v.items.size() == 4);
	CHECK(member(v, 0) == 1);      // Health 1.0 → 1
	CHECK(member(v, 1) == 0);      // Tier stays at the default
	CHECK(warn.count() == 1);
}

TEST_CASE("OnDestroyed: without Extract it still arrives, with no data")
{
	registerStructs();
	ExtractWarnings warn;
	Runtime rt;
	Graph plain;
	plain.variables = { intVar("Hp", 3) };
	const InstanceId o = rt.add(plain, {}, { "test/Plain.hasset", "Object" });
	const InstanceId noArg = rt.add(listenerGraph(nullptr));
	const InstanceId typed = rt.add(listenerGraph(kReport), {}, { "test/Typed.hasset", "Object" });
	rt.bindEvent(o, kOnDestroyed, noArg);
	rt.bindEvent(o, kOnDestroyed, typed);
	CHECK(rt.buildExtract(o).type != PinType::Struct);

	rt.destroy(o);
	CHECK(rt.getVariable(noArg, "Got").i == 1);   // "it died" arrives
	// The typed handler expects data nobody sent: skipped, not fed a default.
	const Value got = rt.getVariable(typed, "Got");
	CHECK((got.items.empty() || got.items[0].i == 0));
	CHECK(warn.count() == 1);
}

TEST_CASE("OnDestroyed: a listener of another struct type is skipped, a listener without argument is called")
{
	registerStructs();
	ExtractWarnings warn;
	Runtime rt;
	Seen crateSeen, plainSeen;
	const InstanceId enemy = rt.add(enemyGraph(1, 4, 1));
	const InstanceId crateL = rt.addCompiled(makeCompiled<CompiledListener>(
		&crateSeen, &rt, (int)PinType::Struct, kCrate));
	const InstanceId plainL = rt.addCompiled(makeCompiled<CompiledListener>(
		&plainSeen, &rt, -1, ""));
	rt.bindEvent(enemy, kOnDestroyed, crateL);
	rt.bindEvent(enemy, kOnDestroyed, plainL);

	rt.destroy(enemy);
	CHECK(crateSeen.calls == 0);          // CrateReport handler, EnemyReport sent
	CHECK(plainSeen.calls == 1);          // no argument: the payload is dropped
	CHECK(plainSeen.last.type != PinType::Struct);
	CHECK(warn.count() == 1);
}

TEST_CASE("OnDestroyed: the dying instance itself never receives it")
{
	registerStructs();
	Runtime rt;
	Graph self = listenerGraph(nullptr);
	self.extract.structPath = kReport;
	const InstanceId o = rt.add(self);
	const InstanceId watcher = rt.add(listenerGraph(nullptr));
	rt.bindEvent(o, kOnDestroyed, o);         // bound to itself
	rt.bindEvent(o, kOnDestroyed, watcher);
	rt.destroy(o);
	CHECK(rt.getVariable(watcher, "Got").i == 1);
	// o is gone; had it run its own OnDestroyed, nothing could see it — so the
	// check is that the runtime did not crash and the watcher still got one.
}

TEST_CASE("OnDestroyed: a listener's own listeners do not hear it")
{
	registerStructs();
	Runtime rt;
	const InstanceId enemy = rt.add(enemyGraph(1, 2, 0));
	const InstanceId l = rt.add(listenerGraph(nullptr));
	const InstanceId l2 = rt.add(listenerGraph(nullptr));
	rt.bindEvent(enemy, kOnDestroyed, l);
	rt.bindEvent(l, kOnDestroyed, l2);    // "tell me when L dies"
	rt.destroy(enemy);
	CHECK(rt.getVariable(l, "Got").i == 1);
	CHECK(rt.getVariable(l2, "Got").i == 0);   // L did not die
}

TEST_CASE("Lifecycle events run in the own class only, not at listeners")
{
	Runtime rt;
	// The listener has its own Construct/Destruct/BeginPlay handlers that
	// would mark it; bound to the owner's events of those names, they used to
	// run when the OWNER went through its lifecycle.
	Graph lg;
	lg.variables = { intVar("One", 1), intVar("Constructed", 0), intVar("Destructed", 0),
	                 intVar("Began", 0) };
	eventCopies(lg, "Construct", "One", "Constructed", PinType::Int);
	eventCopies(lg, "Destruct", "One", "Destructed", PinType::Int);
	eventCopies(lg, "BeginPlay", "One", "Began", PinType::Int);
	const InstanceId l = rt.add(lg);
	const InstanceId o = rt.add(Graph{});
	for (const char* ev : { "PreConstruct", "Construct", "BeginPlay", "Destruct", "OnInit" })
		rt.bindEvent(o, ev, l);

	rt.firePreConstruct(o);
	rt.fireConstruct(o);
	rt.fireBeginPlay(o);
	rt.fireOnInit(o);
	rt.destroy(o);
	CHECK(rt.getVariable(l, "Constructed").i == 0);
	CHECK(rt.getVariable(l, "Destructed").i == 0);
	CHECK(rt.getVariable(l, "Began").i == 0);

	// Positive control: the same handlers DO run for the listener's own lifecycle.
	rt.fireConstruct(l);
	CHECK(rt.getVariable(l, "Constructed").i == 1);
}

TEST_CASE("Extract on Destruct: re-entrant destroys extract once")
{
	registerStructs();
	Runtime rt;
	// An owner whose Destruct destroys itself again (Destroy Object on Self,
	// through the host), and a listener that destroys the source once more from
	// inside OnDestroyed: one Extract, one OnDestroyed.
	struct SelfDestroyer : CompiledEnemy
	{
		Runtime* rt = nullptr; InstanceId self = 0;
		void onDestruct() override { rt->destroy(self); }
	};
	struct Redestroyer : CompiledListener
	{
		using CompiledListener::CompiledListener;
		Runtime* rt2 = nullptr; InstanceId victim = 0;
		void fireEvent(const std::string& name, int elem, const Value& arg) override
		{
			CompiledListener::fireEvent(name, elem, arg);
			rt2->destroy(victim);
		}
	};
	Seen seen;
	CompiledPtr ownedEnemy = makeCompiled<SelfDestroyer>();
	auto* raw = static_cast<SelfDestroyer*>(ownedEnemy.get());
	const InstanceId enemy = rt.addCompiled(std::move(ownedEnemy));
	raw->rt = &rt; raw->self = enemy;
	CompiledPtr ownedL = makeCompiled<Redestroyer>(&seen, &rt, (int)PinType::Struct, kReport);
	auto* rawL = static_cast<Redestroyer*>(ownedL.get());
	rawL->rt2 = &rt; rawL->victim = enemy;
	const InstanceId l = rt.addCompiled(std::move(ownedL));
	rt.bindEvent(enemy, kOnDestroyed, l);

	rt.destroy(enemy);
	rt.destroy(enemy);   // already gone: no-op
	CHECK(seen.calls == 1);
	CHECK_FALSE(rt.alive(enemy));
}

TEST_CASE("Extract on Destruct: remove and clear send nothing")
{
	registerStructs();
	Runtime rt;
	Seen seen;
	const InstanceId a = rt.add(enemyGraph(1, 1, 1));
	const InstanceId b = rt.add(enemyGraph(1, 1, 1));
	const InstanceId l = rt.addCompiled(makeCompiled<CompiledListener>(
		&seen, &rt, (int)PinType::Struct, kReport));
	rt.bindEvent(a, kOnDestroyed, l);
	rt.bindEvent(b, kOnDestroyed, l);
	rt.remove(a);
	CHECK(seen.calls == 0);
	rt.clear();
	CHECK(seen.calls == 0);
	(void)b;
}

TEST_CASE("Extract on Destruct: a derived class inherits the spec, or replaces it whole")
{
	registerStructs();
	Runtime rt;
	// Base extracts EnemyReport; the derived level declares nothing → base's.
	{
		Graph base = enemyGraph(1, 6, 4);
		Graph derived;
		const InstanceId id = rt.addLevels({ base, derived });
		const Value v = rt.buildExtract(id);
		REQUIRE(v.items.size() == 4);
		CHECK(v.typeName == kReport);
		CHECK(member(v, 1) == 4);
	}
	// The derived level names CrateReport → only that, nothing of the base's.
	{
		Graph base = enemyGraph(1, 6, 4);
		Graph derived;
		derived.variables = { intVar("Gold", 11) };
		derived.extract.structPath = kCrate;
		derived.extract.map = { { "Coins", "Gold" } };
		const InstanceId id = rt.addLevels({ base, derived });
		const Value v = rt.buildExtract(id);
		REQUIRE(v.items.size() == 1);
		CHECK(v.typeName == kCrate);
		CHECK(member(v, 0) == 11);
	}
	// Inherited AND private variables are extractable by the class itself.
	{
		Graph base;
		Variable secret = intVar("Secret", 42); secret.access = 1;
		base.variables = { secret };
		Graph derived;
		derived.extract.structPath = kCrate;
		derived.extract.map = { { "Coins", "Secret" } };
		const InstanceId id = rt.addLevels({ base, derived });
		CHECK(member(rt.buildExtract(id), 0) == 42);
	}
}

TEST_CASE("Extract on Destruct: a compiled owner extracts through its generated hook")
{
	registerStructs();
	Runtime rt;
	const InstanceId enemy = rt.addCompiled(makeCompiled<CompiledEnemy>());
	const InstanceId l = rt.add(listenerGraph(kReport));
	rt.bindEvent(enemy, kOnDestroyed, l);
	rt.destroy(enemy);
	const Value got = rt.getVariable(l, "Got");
	REQUIRE(got.items.size() == 4);
	CHECK(member(got, 0) == 4);
	CHECK(member(got, 1) == 2);
}

TEST_CASE("Extract on Destruct: JSON round trip, formerNames, dropped entries, EmitEvent warning")
{
	registerStructs();
	Graph g = enemyGraph(1, 2, 3);
	g.extract.map.push_back({ "Bonus", "Health" });
	Graph back;
	REQUIRE(fromJson(toJson(g), back));
	CHECK(back.extract.structPath == kReport);
	REQUIRE(back.extract.map.size() == 4);
	CHECK(back.extract.map[1].member == "Tier");
	CHECK(back.extract.map[1].var == "LootTier");
	CHECK(back.extract.map[2].var == kExtractSelf);

	// Nothing set → no key at all (old graphs keep their bytes).
	Graph none;
	CHECK(toJson(none).find("\"extract\"") == std::string::npos);

	// A member saved under a former name comes back under the live one; an
	// entry without a variable, and a second row for the same member, drop.
	const std::string json = R"({"nodes":[],"links":[],"variables":[],
		"extract":{"struct":"test/extract/EnemyReport.hasset",
		           "map":[{"member":"Frags","var":"K"},{"member":"Tier","var":""},
		                  {"member":"Kills","var":"Other"}]}})";
	Graph old;
	REQUIRE(fromJson(json, old));
	REQUIRE(old.extract.map.size() == 1);
	CHECK(old.extract.map[0].member == "Kills");
	CHECK(old.extract.map[0].var == "K");

	// A graph that emits its own "OnDestroyed" hears about the engine's.
	ExtractWarnings warn;
	Graph emits;
	Node em; em.type = NodeType::EmitEvent; em.s = kOnDestroyed;
	emits.addNode(em);
	Graph reread;
	REQUIRE(fromJson(toJson(emits), reread));
	CHECK(warn.count() == 1);
}
