// Pull on Construct (docs/state-driven-data-exchange-design.md §2, Thema 127):
// a variable that names a source is copied from it at registration, before the
// instance's first event; when the source cannot answer, the declared default
// stays. These tests drive the interpreted Runtime directly — the compiled side
// runs through the same Runtime::pullOnConstruct and is compared against it in
// the codegen parity suite (fixture "pull_construct").
#include "doctest.h"
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Diagnostics/Log.h>
#include <Types/TypeRegistry.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/WidgetManager.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace HorizonCode;

namespace
{
	constexpr const char* kRunStats = "test/pull/RunStats.hasset";

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
	Variable pulling(Variable v, const char* src, const std::string& var,
	                 const std::string& member = {}, const std::string& cls = {})
	{
		v.pullSource = src; v.pullVar = var; v.pullMember = member; v.pullClass = cls;
		return v;
	}

	// Event(event) → SetVariable(dst) = GetVariable(src). What the instance
	// SAW at that moment ends up in `dst`, which is how these tests look inside
	// PreConstruct/Construct.
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

	// Event(event) → CreateObject(cls). CreateObject: execIn 0 / execOut 1 /
	// Location 2 / Rotation 3 / Object 4.
	void eventCreates(Graph& g, const std::string& event, const std::string& cls)
	{
		Node ev; ev.type = NodeType::Event; ev.s = event;
		const int e = g.addNode(ev);
		Node co; co.type = NodeType::CreateObject; co.s = cls;
		const int c = g.addNode(co);
		REQUIRE(g.connect(e, 0, c, 0));
	}

	Graph giWith(std::vector<Variable> vars)
	{
		Graph g;
		g.variables = std::move(vars);
		return g;
	}

	// Counts the Pull on Construct warnings while alive.
	struct PullWarnings
	{
		static inline std::vector<std::string> seen;
		int handle = 0;
		PullWarnings()
		{
			seen.clear();
			handle = HE::Log::addSink([](const HE::Log::Record& r, void*)
			{
				const std::string m = r.message ? r.message : "";
				if (m.find("Pull on Construct") != std::string::npos) seen.push_back(m);
			}, nullptr);
		}
		~PullWarnings() { HE::Log::removeSink(handle); }
		size_t count() const { return seen.size(); }
	};

	void registerRunStats()
	{
		HE::StructDef def;
		def.name = "RunStats"; def.assetPath = kRunStats;
		HE::StructField score; score.name = "Score"; score.type = PinType::Int;
		score.defaultValue = Value::ofInt(0);
		score.formerNames = { "Points" };   // renamed once: Points → Score
		HE::StructField kills; kills.name = "Kills"; kills.type = PinType::Int;
		kills.defaultValue = Value::ofInt(0);
		def.fields = { score, kills };
		HE::TypeRegistry::instance().registerStruct(def);
	}

	Variable runStatsVar(const std::string& name, int score, int kills)
	{
		Variable v; v.name = name; v.type = PinType::Struct; v.typeName = kRunStats;
		v.structDefaults["Score"] = Value::ofInt(score);
		v.structDefaults["Kills"] = Value::ofInt(kills);
		return v;
	}

	const Runtime::PullOutcome* outcome(const Runtime& rt, InstanceId id, const std::string& name)
	{
		static std::vector<Runtime::PullOutcome> keep;
		keep = rt.pulledVariablesOf(id);
		for (const auto& o : keep)
			if (o.name == name) return &o;
		return nullptr;
	}
}

TEST_CASE("Pull on Construct: a Game Instance variable is there before Construct runs")
{
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 7) }));

	Graph hud;
	hud.variables = { pulling(intVar("Score", 0), kPullFromGameInstance, "Score"),
	                  intVar("SeenInPre", -1), intVar("SeenInConstruct", -1) };
	eventCopies(hud, "PreConstruct", "Score", "SeenInPre", PinType::Int);
	eventCopies(hud, "Construct", "Score", "SeenInConstruct", PinType::Int);
	const InstanceId id = rt.add(hud, {}, { "test/Hud.hasset", "Widget" });

	// Already pulled at registration — before any host fires anything.
	CHECK(rt.getVariable(id, "Score").i == 7);
	rt.firePreConstruct(id);
	rt.fireConstruct(id);
	CHECK(rt.getVariable(id, "SeenInPre").i == 7);
	CHECK(rt.getVariable(id, "SeenInConstruct").i == 7);
	const auto* o = outcome(rt, id, "Score");
	REQUIRE(o != nullptr);
	CHECK(o->pulled);
	CHECK(o->reason.empty());

	// Negative control: the same class without the pull keeps its default.
	Graph plain;
	plain.variables = { intVar("Score", 0), intVar("SeenInConstruct", -1) };
	eventCopies(plain, "Construct", "Score", "SeenInConstruct", PinType::Int);
	const InstanceId pid = rt.add(plain);
	rt.fireConstruct(pid);
	CHECK(rt.getVariable(pid, "SeenInConstruct").i == 0);
	CHECK(rt.pulledVariablesOf(pid).empty());
}

TEST_CASE("Pull on Construct: once — a later change at the source does not follow")
{
	Runtime rt;
	const InstanceId gi = rt.setGameInstance(giWith({ intVar("Score", 3) }));
	Graph g; g.variables = { pulling(intVar("Score"), kPullFromGameInstance, "Score") };
	const InstanceId id = rt.add(g);
	rt.setVariable(gi, "Score", Value::ofInt(99));
	CHECK(rt.getVariable(id, "Score").i == 3);
	// …but whatever is created afterwards pulls the new value.
	const InstanceId later = rt.add(g);
	CHECK(rt.getVariable(later, "Score").i == 99);
}

TEST_CASE("Pull on Construct: a struct member, also under its former name")
{
	registerRunStats();
	Runtime rt;
	rt.setGameInstance(giWith({ runStatsVar("LastRun", 12, 4) }));

	Graph g;
	g.variables = { pulling(intVar("Score", -1), kPullFromGameInstance, "LastRun", "Score"),
	                pulling(intVar("Kills", -1), kPullFromGameInstance, "LastRun", "Kills"),
	                // The member was renamed Points → Score in the struct; a graph
	                // saved before that still names it by the old name.
	                pulling(intVar("Old", -1), kPullFromGameInstance, "LastRun", "Points"),
	                // A whole struct pulls too, as a copy.
	                pulling(runStatsVar("Whole", 0, 0), kPullFromGameInstance, "LastRun") };
	const InstanceId id = rt.add(g);
	CHECK(rt.getVariable(id, "Score").i == 12);
	CHECK(rt.getVariable(id, "Kills").i == 4);
	CHECK(rt.getVariable(id, "Old").i == 12);
	const Value whole = rt.getVariable(id, "Whole");
	REQUIRE(whole.items.size() == 2);
	CHECK(whole.items[0].i == 12);
	CHECK(whole.items[1].i == 4);

	// A member nobody declared, and a member asked of a non-struct.
	Graph bad;
	bad.variables = { pulling(intVar("A", 5), kPullFromGameInstance, "LastRun", "Nope"),
	                  pulling(intVar("B", 6), kPullFromGameInstance, "Missing", "Score") };
	Graph scalarGi = giWith({ intVar("Plain", 1) });
	const InstanceId bid = rt.add(bad);
	CHECK(rt.getVariable(bid, "A").i == 5);
	CHECK(outcome(rt, bid, "A")->why == PullFailure::NoSuchMember);
	CHECK(outcome(rt, bid, "B")->why == PullFailure::NoPublicVariable);

	Runtime rt2;
	rt2.setGameInstance(scalarGi);
	Graph notStruct;
	notStruct.variables = { pulling(intVar("C", 8), kPullFromGameInstance, "Plain", "Score") };
	const InstanceId cid = rt2.add(notStruct);
	CHECK(rt2.getVariable(cid, "C").i == 8);
	CHECK(outcome(rt2, cid, "C")->why == PullFailure::NotAStruct);
}

TEST_CASE("Pull on Construct: every fallback keeps the default, with ONE warning per class")
{
	PullWarnings warnings;
	{
		// No Game Instance at all.
		Runtime rt;
		Graph g; g.variables = { pulling(intVar("Score", 41), kPullFromGameInstance, "Score") };
		const ClassIdentity cls{ "test/NoGi.hasset", "Object" };
		std::vector<InstanceId> ids;
		for (int i = 0; i < 10; ++i) ids.push_back(rt.add(g, {}, cls));
		for (InstanceId id : ids) CHECK(rt.getVariable(id, "Score").i == 41);
		CHECK(outcome(rt, ids.back(), "Score")->why == PullFailure::NoGameInstance);
		CHECK(warnings.count() == 1);   // ten instances, one warning
		REQUIRE(!warnings.seen.empty());
		CHECK(warnings.seen[0].find("test/NoGi.hasset.Score") != std::string::npos);
		CHECK(warnings.seen[0].find("default used") != std::string::npos);
	}
	warnings.seen.clear();
	{
		Runtime rt;
		Variable priv = intVar("Secret", 9); priv.access = 1;
		Variable text; text.name = "Name"; text.type = PinType::String; text.s = "x";
		rt.setGameInstance(giWith({ priv, text }));
		Graph g;
		g.variables = { pulling(intVar("A", 1), kPullFromGameInstance, "Secret"),
		                pulling(intVar("B", 2), kPullFromGameInstance, "Gone"),
		                pulling(intVar("C", 3), kPullFromGameInstance, "Name") };
		const InstanceId id = rt.add(g, {}, { "test/Fallbacks.hasset", "Object" });
		CHECK(rt.getVariable(id, "A").i == 1);   // private on the source
		CHECK(rt.getVariable(id, "B").i == 2);   // not there at all
		CHECK(rt.getVariable(id, "C").i == 3);   // String into Int
		CHECK(outcome(rt, id, "A")->why == PullFailure::NoPublicVariable);
		CHECK(outcome(rt, id, "B")->why == PullFailure::NoPublicVariable);
		CHECK(outcome(rt, id, "C")->why == PullFailure::TypeMismatch);
		CHECK(outcome(rt, id, "C")->reason.find("String, needs Int") != std::string::npos);
		CHECK(warnings.count() == 3);
		rt.add(g, {}, { "test/Fallbacks.hasset", "Object" });
		CHECK(warnings.count() == 3);   // the second instance adds nothing
	}
}

TEST_CASE("Pull on Construct: Int and Float convert, a container never meets a scalar")
{
	Runtime rt;
	Variable arr; arr.name = "List"; arr.type = PinType::Int; arr.isArray = true;
	arr.container = ContainerKind::Array;
	arr.defaultItems = { Value::ofInt(1), Value::ofInt(2) };
	rt.setGameInstance(giWith({ floatVar("Ratio", 2.75f), intVar("Count", 5), arr }));

	Graph g;
	Variable listOut = arr; listOut.name = "ListOut"; listOut.defaultItems.clear();
	listOut = pulling(listOut, kPullFromGameInstance, "List");
	Variable floatList = arr; floatList.name = "FloatList"; floatList.type = PinType::Float;
	floatList.defaultItems.clear();
	floatList = pulling(floatList, kPullFromGameInstance, "List");
	g.variables = { pulling(intVar("RatioInt", -1), kPullFromGameInstance, "Ratio"),
	                pulling(floatVar("CountF", -1.0f), kPullFromGameInstance, "Count"),
	                pulling(intVar("Scalar", -1), kPullFromGameInstance, "List"),
	                listOut, floatList };
	const InstanceId id = rt.add(g);
	CHECK(rt.getVariable(id, "RatioInt").type == PinType::Int);
	CHECK(rt.getVariable(id, "RatioInt").i == 2);
	CHECK(rt.getVariable(id, "CountF").type == PinType::Float);
	CHECK(rt.getVariable(id, "CountF").f == 5.0f);
	CHECK(rt.getVariable(id, "Scalar").i == -1);
	CHECK(outcome(rt, id, "Scalar")->why == PullFailure::TypeMismatch);
	CHECK(rt.getVariable(id, "ListOut").items.size() == 2);
	// Numbers convert as scalars only: an Int[] does not land in a Float[].
	CHECK(rt.getVariable(id, "FloatList").items.empty());
	CHECK(outcome(rt, id, "FloatList")->why == PullFailure::TypeMismatch);
}

TEST_CASE("Pull on Construct: a pulled container is a copy, not a view")
{
	Runtime rt;
	Variable arr; arr.name = "List"; arr.type = PinType::Int; arr.isArray = true;
	arr.container = ContainerKind::Array;
	arr.defaultItems = { Value::ofInt(1), Value::ofInt(2) };
	const InstanceId gi = rt.setGameInstance(giWith({ arr }));
	Variable mine = arr; mine.defaultItems.clear();
	Graph g; g.variables = { pulling(mine, kPullFromGameInstance, "List") };
	const InstanceId id = rt.add(g);
	Value changed = Value::ofArray(PinType::Int);
	changed.items = { Value::ofInt(9) };
	rt.setVariable(gi, "List", changed);
	const Value got = rt.getVariable(id, "List");
	REQUIRE(got.items.size() == 2);
	CHECK(got.items[0].i == 1);
	CHECK(got.items[1].i == 2);
}

TEST_CASE("Pull on Construct: the Game Instance itself never pulls, and never warns about it")
{
	PullWarnings warnings;
	Runtime rt;
	// A hand-edited GI graph that pulls from itself. Neither the first GI nor
	// its replacement may pull or warn: during its own registration there is
	// no Game Instance yet, and it would be its own source anyway.
	Graph gi = giWith({ intVar("Score", 4),
	                    pulling(intVar("Copy", 1), kPullFromGameInstance, "Score") });
	const InstanceId first = rt.setGameInstance(gi);
	CHECK(rt.getVariable(first, "Copy").i == 1);
	CHECK(rt.pulledVariablesOf(first).empty());
	const InstanceId second = rt.setGameInstance(gi);
	CHECK(second != first);
	CHECK(rt.getVariable(second, "Copy").i == 1);
	CHECK(rt.pulledVariablesOf(second).empty());
	CHECK(warnings.count() == 0);
}

TEST_CASE("Pull on Construct: the level script and plain objects pull like everything else")
{
	// The hook sits in the Runtime's registration, so a level script (keyed by
	// the host, registered by HorizonWorld) gets it without any host code.
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Difficulty", 3) }));
	Graph level; level.variables = { pulling(intVar("Difficulty"), kPullFromGameInstance, "Difficulty"),
	                                 intVar("Seen", -1) };
	eventCopies(level, "OnLevelLoaded", "Difficulty", "Seen", PinType::Int);
	const InstanceId lv = rt.add(level, {}, { "level:1234", "Level" });
	rt.fireOnLevelLoaded(lv);
	CHECK(rt.getVariable(lv, "Seen").i == 3);
	CHECK(rt.creatorOf(lv) == 0);
}

TEST_CASE("Pull on Construct: from the Creator, nested creation, and the expected class")
{
	Runtime rt;
	std::map<std::string, Graph> classes;
	std::map<std::string, ClassIdentity> idents;

	Runtime::Services svc;
	svc.createObject = [&](const std::string& path, const float*, const float*) -> uint32_t
	{
		auto it = classes.find(path);
		if (it == classes.end()) return 0u;
		const InstanceId id = rt.add(it->second, {}, idents[path]);
		rt.fireConstruct(id);
		return id;
	};
	rt.setServices(svc);

	// Spawner (public Gift = 11) creates Child on "Go". Child pulls Gift from
	// its creator, and in its own Construct creates Grandchild, which pulls
	// Gift from ITS creator — the Child, not the Spawner. Both read 11, but
	// creatorOf tells the two paths apart.
	Graph grandchild;
	grandchild.variables = { pulling(intVar("Gift", -1), kPullFromCreator, "Gift") };
	classes["test/Grandchild.hasset"] = grandchild;
	idents["test/Grandchild.hasset"] = { "test/Grandchild.hasset", "Object" };

	Graph child;
	child.variables = { pulling(intVar("Gift", -1), kPullFromCreator, "Gift"),
	                    intVar("SeenInConstruct", -2) };
	eventCopies(child, "Construct", "Gift", "SeenInConstruct", PinType::Int);
	eventCreates(child, "Construct", "test/Grandchild.hasset");
	classes["test/Child.hasset"] = child;
	idents["test/Child.hasset"] = { "test/Child.hasset", "Object" };

	Graph spawner;
	spawner.variables = { intVar("Gift", 11) };
	eventCreates(spawner, "Go", "test/Child.hasset");
	const InstanceId sp = rt.add(spawner, {}, { "test/Spawner.hasset", "Object" });
	rt.fireEvent(sp, "Go");

	InstanceId childId = 0, grandId = 0;
	for (InstanceId id = 1; id < 64; ++id)
	{
		if (!rt.alive(id)) continue;
		if (rt.classKeyOf(id) == "test/Child.hasset")      childId = id;
		if (rt.classKeyOf(id) == "test/Grandchild.hasset") grandId = id;
	}
	REQUIRE(childId != 0);
	REQUIRE(grandId != 0);
	CHECK(rt.creatorOf(childId) == sp);
	CHECK(rt.creatorOf(grandId) == childId);   // nested: B created C, not A
	CHECK(rt.getVariable(childId, "Gift").i == 11);
	CHECK(rt.getVariable(childId, "SeenInConstruct").i == 11);
	CHECK(rt.getVariable(grandId, "Gift").i == 11);
	CHECK(rt.creatorOf(sp) == 0);   // registered from outside HorizonCode

	// Placed (registered by a host, not by a graph): no creator, default.
	const InstanceId placed = rt.add(child, {}, idents["test/Child.hasset"]);
	CHECK(rt.getVariable(placed, "Gift").i == -1);
	CHECK(outcome(rt, placed, "Gift")->why == PullFailure::NoCreator);

	// The expected class: a creator that is not one falls back, a derived
	// one counts.
	Graph typed;
	typed.variables = { pulling(intVar("Gift", -1), kPullFromCreator, "Gift", {},
	                            "test/Spawner.hasset") };
	classes["test/Typed.hasset"] = typed;
	idents["test/Typed.hasset"] = { "test/Typed.hasset", "Object" };
	Graph other; other.variables = { intVar("Gift", 22) };
	eventCreates(other, "Go", "test/Typed.hasset");
	const InstanceId wrong = rt.add(other, {}, { "test/Other.hasset", "Object" });
	rt.fireEvent(wrong, "Go");
	Graph derived; derived.variables = { intVar("Gift", 33) };
	eventCreates(derived, "Go", "test/Typed.hasset");
	ClassIdentity derivedCls{ "test/SpawnerChild.hasset", "Object", { "test/Spawner.hasset" } };
	const InstanceId right = rt.add(derived, {}, derivedCls);
	rt.fireEvent(right, "Go");

	InstanceId fromWrong = 0, fromRight = 0;
	for (InstanceId id = 1; id < 128; ++id)
		if (rt.alive(id) && rt.classKeyOf(id) == "test/Typed.hasset")
			(rt.creatorOf(id) == wrong ? fromWrong : fromRight) = id;
	REQUIRE(fromWrong != 0);
	REQUIRE(fromRight != 0);
	CHECK(rt.getVariable(fromWrong, "Gift").i == -1);
	CHECK(outcome(rt, fromWrong, "Gift")->why == PullFailure::CreatorWrongClass);
	CHECK(rt.getVariable(fromRight, "Gift").i == 33);
}

TEST_CASE("Pull on Construct: a creator destroyed before its creation registers is reported")
{
	// Not reachable through a graph (Create Object is synchronous), so the
	// outcome is checked through the API surface the editor uses instead.
	CHECK(pullFailureText(PullFailure::CreatorGone, kPullFromCreator, "Gift", {})
	      == "the creator was already destroyed");
	CHECK(pullFailureText(PullFailure::NoPublicVariable, kPullFromGameInstance, "Score", {})
	      == "Game Instance has no public variable 'Score'");
	CHECK(pullFailureText(PullFailure::CreatorWrongClass, kPullFromCreator, "Gift", {},
	                      "test/Spawner.hasset")
	      == "the creator is not a test/Spawner.hasset");
}

TEST_CASE("Pull on Construct: the leaf-most declaration decides")
{
	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 5), intVar("Other", 8) }));
	Graph base;
	base.variables = { pulling(intVar("Score", 1), kPullFromGameInstance, "Score"),
	                   pulling(intVar("Keep", 2), kPullFromGameInstance, "Score") };
	Graph derived;
	derived.variables = { intVar("Score", 1),   // re-declared WITHOUT a pull: off
	                      pulling(intVar("Keep", 2), kPullFromGameInstance, "Other") };
	std::vector<Graph> levels = { base, derived };
	const InstanceId id = rt.addLevels(levels, {}, { "test/Derived.hasset", "Object",
	                                                  { "test/Base.hasset" } });
	CHECK(rt.getVariable(id, "Score").i == 1);
	CHECK(rt.getVariable(id, "Keep").i == 8);
	CHECK(rt.pulledVariablesOf(id).size() == 1);

	// The base alone still pulls.
	const InstanceId b = rt.add(base);
	CHECK(rt.getVariable(b, "Score").i == 5);
}

TEST_CASE("Pull on Construct: JSON round trip, and what the loader refuses")
{
	Graph g;
	g.variables = { pulling(intVar("Score"), kPullFromGameInstance, "LastRun", "Score"),
	                pulling(intVar("Gift"), kPullFromCreator, "Gift", {}, "test/Spawner.hasset"),
	                intVar("Plain") };
	Graph back;
	REQUIRE(fromJson(toJson(g), back));
	const Variable* s = back.findVariable("Score");
	REQUIRE(s != nullptr);
	CHECK(s->pullSource == kPullFromGameInstance);
	CHECK(s->pullVar == "LastRun");
	CHECK(s->pullMember == "Score");
	CHECK(s->pullClass.empty());
	const Variable* gift = back.findVariable("Gift");
	REQUIRE(gift != nullptr);
	CHECK(gift->pullSource == kPullFromCreator);
	CHECK(gift->pullClass == "test/Spawner.hasset");
	const Variable* plain = back.findVariable("Plain");
	REQUIRE(plain != nullptr);
	CHECK(plain->pullSource.empty());
	// A variable that pulls nothing writes nothing.
	CHECK(variableToJson(*plain).find("pull") == std::string::npos);

	// Refused on load: a function-local, an unknown source, a pull without a
	// variable, and a class on a source that is not the creator.
	auto load = [](const std::string& json)
	{
		Variable v;
		REQUIRE(variableFromJson(json, v));
		return v;
	};
	CHECK(load(R"({"name":"L","type":3,"scope":7,"pull":{"src":"GameInstance","var":"S"}})")
	          .pullSource.empty());
	CHECK(load(R"({"name":"U","type":3,"pull":{"src":"DataTable","var":"S"}})")
	          .pullSource.empty());
	CHECK(load(R"({"name":"E","type":3,"pull":{"src":"GameInstance","var":""}})")
	          .pullSource.empty());
	const Variable c = load(R"({"name":"C","type":3,"pull":{"src":"GameInstance","var":"S","class":"x"}})");
	CHECK(c.pullSource == kPullFromGameInstance);
	CHECK(c.pullClass.empty());
}

TEST_CASE("Pull on Construct: the compatibility rule the editor shares")
{
	std::string why;
	CHECK(pullShapesCompatible(PinType::Float, ContainerKind::None, PinType::String, "",
	                           PinType::Int, ContainerKind::None, PinType::String, ""));
	CHECK_FALSE(pullShapesCompatible(PinType::Float, ContainerKind::Array, PinType::String, "",
	                                 PinType::Int, ContainerKind::Array, PinType::String, "", &why));
	CHECK(why == "Float[], needs Int[]");
	CHECK_FALSE(pullShapesCompatible(PinType::Struct, ContainerKind::None, PinType::String,
	                                 "a/RunStats.hasset", PinType::Struct, ContainerKind::None,
	                                 PinType::String, "a/Other.hasset", &why));
	CHECK(why == "Struct RunStats, needs Struct Other");
	CHECK(pullShapesCompatible(PinType::Ref, ContainerKind::None, PinType::String, "",
	                           PinType::Ref, ContainerKind::None, PinType::String, ""));
	CHECK_FALSE(pullShapesCompatible(PinType::Int, ContainerKind::Map, PinType::String, "",
	                                 PinType::Int, ContainerKind::Map, PinType::Int, ""));
	CHECK(isKnownPullSource("GameInstance"));
	CHECK(isKnownPullSource("Creator"));
	CHECK_FALSE(isKnownPullSource(""));
	CHECK_FALSE(isKnownPullSource("SaveGame"));
}

// The real host: WidgetManager registers the embed first and the page after,
// then fires PreConstruct on both before any Construct (Thema 119). The pull
// happens inside each registration, so both PreConstructs already see it —
// and a hot reload's restored running value lands on top of the pulled one.
TEST_CASE("Pull on Construct: a widget and its embed have it in PreConstruct, and a reload keeps the running value")
{
	struct TempDir
	{
		std::filesystem::path path;
		TempDir()
		{
			path = std::filesystem::temp_directory_path() / "he_test_pull_on_construct";
			std::filesystem::remove_all(path);
			std::filesystem::create_directories(path);
		}
		~TempDir() { std::filesystem::remove_all(path); }
	} dir;
	ContentManager cm(dir.path.string());

	auto pullingGraph = [] {
		Graph g;
		g.variables = { pulling(intVar("Score", 0), kPullFromGameInstance, "Score"),
		                intVar("SeenInPre", -1) };
		eventCopies(g, "PreConstruct", "Score", "SeenInPre", PinType::Int);
		return g;
	};
	auto registerWidget = [&cm](const HE::UIWidgetTree& tree, const Graph& g, const char* path)
	{
		UIWidgetAsset a;
		a.treeJson = HE::uiWidgetTreeToJson(tree);
		a.graphJson = toJson(g);
		a.path = path;
		cm.registerWidget(std::move(a));
	};

	HE::UIWidgetTree card;
	card.canvasWidth = 200.0f; card.canvasHeight = 100.0f;
	card.add(HE::UIWidgetType::Panel);
	registerWidget(card, pullingGraph(), "mem://pull_card.hasset");

	HE::UIWidgetTree page;
	page.canvasWidth = 400.0f; page.canvasHeight = 300.0f;
	{
		const int slot = page.add(HE::UIWidgetType::WidgetRef);
		auto* r = dynamic_cast<HE::UIWidgetRef*>(page.find(slot));
		REQUIRE(r != nullptr);
		r->widgetPath = "mem://pull_card.hasset";
		r->name = "Card";
	}
	registerWidget(page, pullingGraph(), "mem://pull_page.hasset");

	Runtime rt;
	rt.setGameInstance(giWith({ intVar("Score", 7) }));
	WidgetManager wm;
	wm.setRuntime(&rt);

	const int id = wm.createWidget(cm, "mem://pull_page.hasset");
	REQUIRE(id != 0);
	const InstanceId pageId = (InstanceId)id;
	const InstanceId cardId = wm.childInstance(id, "Card");
	REQUIRE(cardId != 0);
	CHECK(rt.getVariable(pageId, "Score").i == 7);
	CHECK(rt.getVariable(pageId, "SeenInPre").i == 7);
	CHECK(rt.getVariable(cardId, "Score").i == 7);
	CHECK(rt.getVariable(cardId, "SeenInPre").i == 7);
	// A widget created from a graph's Create Widget would name its creator; one
	// the host creates directly has none.
	CHECK(rt.creatorOf(pageId) == 0);

	// Hot reload: the running value is 42, the source still says 7. After the
	// rebuild the restored 42 wins over the freshly pulled 7 (design §2.7).
	rt.setVariable(pageId, "Score", Value::ofInt(42));
	const WidgetManager::StateSnapshot snap = wm.captureState();
	wm.clear();
	const int again = wm.createWidget(cm, "mem://pull_page.hasset");
	REQUIRE(again != 0);
	CHECK(rt.getVariable((InstanceId)again, "Score").i == 7);   // pulled anew
	CHECK(wm.restoreState(snap) > 0);
	CHECK(rt.getVariable((InstanceId)again, "Score").i == 42);  // the running value wins
}
