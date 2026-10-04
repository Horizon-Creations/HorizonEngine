// Pull on Construct, the editor's decisions (HcPull): which source variables a
// target can take, what the status line says, and what "Add to Target" writes
// into the source. ImGui-free by construction, so asserted here instead of
// clicked through — the drawing (HcPullUi) only shows these answers.
#include "doctest.h"

#include "HcPull.h"
#include <HorizonCode/HorizonCode.h>
#include <Types/TypeRegistry.h>
#include <algorithm>

using namespace HorizonCode;

namespace
{
	constexpr const char* kStats = "test/pulled/Stats.hasset";

	void registerStats()
	{
		HE::StructDef def;
		def.name = "Stats"; def.assetPath = kStats;
		HE::StructField score; score.name = "Score"; score.type = PinType::Int;
		score.defaultValue = Value::ofInt(0);
		score.formerNames = { "Points" };
		HE::StructField name; name.name = "Name"; name.type = PinType::String;
		def.fields = { score, name };
		HE::TypeRegistry::instance().registerStruct(def);
	}

	Variable intTarget(const char* src, const char* var, const char* member = "")
	{
		Variable v; v.name = "Score"; v.type = PinType::Int; v.f[0] = 5.0f;
		v.pullSource = src; v.pullVar = var; v.pullMember = member;
		return v;
	}

	Graph giGraph()
	{
		Graph g;
		Variable score; score.name = "Score"; score.type = PinType::Int;
		Variable ratio; ratio.name = "Ratio"; ratio.type = PinType::Float;
		Variable text;  text.name = "Title"; text.type = PinType::String;
		Variable hidden; hidden.name = "Hidden"; hidden.type = PinType::Int; hidden.access = 1;
		Variable run; run.name = "Run"; run.type = PinType::Struct; run.typeName = kStats;
		g.variables = { score, ratio, text, hidden, run };
		return g;
	}
}

TEST_CASE("Pull editor: only public instance variables are offered, each with its fit")
{
	registerStats();
	const Graph gi = giGraph();
	const auto vars = HcPull::publicVariables(gi);
	REQUIRE(vars.size() == 4);   // Hidden is private
	const Variable t = intTarget(kPullFromGameInstance, "Score");
	for (const auto& sv : vars)
	{
		const std::string why = HcPull::fitOf(t, sv);
		if (sv.name == "Score" || sv.name == "Ratio") CHECK(why.empty());   // Float converts
		if (sv.name == "Title") CHECK(why == "String, needs Int");
		if (sv.name == "Run")   CHECK(why == "Struct Stats: pick a member");
	}
	const auto members = HcPull::fittingMembers(t, vars.back());
	REQUIRE(members.size() == 1);
	CHECK(members[0] == "Score");
}

TEST_CASE("Pull editor: the status line says what the runtime would warn")
{
	registerStats();
	const auto vars = HcPull::publicVariables(giGraph());

	HcPull::Status ok = HcPull::check(intTarget(kPullFromGameInstance, "Run", "Score"), &vars);
	CHECK(ok.ok);
	CHECK(ok.text == "Game Instance > Run > Score (Int)");
	CHECK(ok.liveMember.empty());

	// Saved under the member's former name: still resolves, and the editor is
	// told the live name to write back.
	HcPull::Status old = HcPull::check(intTarget(kPullFromGameInstance, "Run", "Points"), &vars);
	CHECK(old.ok);
	CHECK(old.liveMember == "Score");

	HcPull::Status hidden = HcPull::check(intTarget(kPullFromGameInstance, "Hidden"), &vars);
	CHECK_FALSE(hidden.ok);
	CHECK(hidden.why == PullFailure::NoPublicVariable);
	CHECK(hidden.text == "Game Instance has no public variable 'Hidden'");

	HcPull::Status clash = HcPull::check(intTarget(kPullFromGameInstance, "Title"), &vars);
	CHECK(clash.why == PullFailure::TypeMismatch);
	CHECK(clash.text.find("String, needs Int") != std::string::npos);

	HcPull::Status member = HcPull::check(intTarget(kPullFromGameInstance, "Run", "Nope"), &vars);
	CHECK(member.why == PullFailure::NoSuchMember);

	// A creator with no expected class: nothing to check against, not an error.
	HcPull::Status creator = HcPull::check(intTarget(kPullFromCreator, "Gift"), nullptr);
	CHECK(creator.ok);
	CHECK(creator.text.find("checked when it is created") != std::string::npos);

	CHECK(HcPull::describe(intTarget(kPullFromGameInstance, "Run", "Score"))
	      == "Pulled from Game Instance > Run > Score");
	Variable fromSpawner = intTarget(kPullFromCreator, "Gift");
	fromSpawner.pullClass = "Classes/Spawner.hasset";
	CHECK(HcPull::describe(fromSpawner) == "Pulled from Creator (Spawner) > Gift");
}

TEST_CASE("Pull editor: Add to Target writes what is missing, and nothing else")
{
	registerStats();
	SUBCASE("a plain variable is added with the target's shape and default")
	{
		Graph gi = giGraph();
		Variable t = intTarget(kPullFromGameInstance, "Lives");
		t.replicated = true;   // the target's own settings stay with the target
		std::string what;
		CHECK(HcPull::addToTarget(gi, t, {}, &what) == HcPull::Add::AddedVariable);
		const Variable* added = gi.findVariable("Lives");
		REQUIRE(added != nullptr);
		CHECK(added->type == PinType::Int);
		CHECK(added->f[0] == 5.0f);
		CHECK(added->access == 0);
		CHECK_FALSE(added->replicated);
		CHECK(added->pullSource.empty());
		CHECK(what.find("Lives") != std::string::npos);
		// Now it resolves, and a second click does nothing.
		const auto vars = HcPull::publicVariables(gi);
		CHECK(HcPull::check(t, &vars).ok);
		CHECK(HcPull::addToTarget(gi, t) == HcPull::Add::NothingToDo);
	}
	SUBCASE("a private one of the right type is made public, a clash is refused")
	{
		Graph gi = giGraph();
		CHECK(HcPull::addToTarget(gi, intTarget(kPullFromGameInstance, "Hidden"))
		      == HcPull::Add::MadePublic);
		CHECK(gi.findVariable("Hidden")->access == 0);
		const size_t before = gi.variables.size();
		CHECK(HcPull::addToTarget(gi, intTarget(kPullFromGameInstance, "Title"))
		      == HcPull::Add::Refused);
		CHECK(gi.variables.size() == before);
		CHECK(gi.findVariable("Title")->type == PinType::String);
	}
	SUBCASE("a member pull: missing variable asks for its struct, then adds it")
	{
		Graph gi = giGraph();
		const Variable t = intTarget(kPullFromGameInstance, "Best", "Score");
		CHECK(HcPull::addToTarget(gi, t) == HcPull::Add::NeedsStructType);
		const auto structs = HcPull::structsWithMember(t, "Score");
		REQUIRE(std::find(structs.begin(), structs.end(), kStats) != structs.end());
		CHECK(HcPull::addToTarget(gi, t, kStats) == HcPull::Add::AddedStructVar);
		const Variable* best = gi.findVariable("Best");
		REQUIRE(best != nullptr);
		CHECK(best->type == PinType::Struct);
		CHECK(best->typeName == kStats);
		CHECK(best->structDefaults.at("Score").i == 5);   // starts where the target would
		const auto vars = HcPull::publicVariables(gi);
		CHECK(HcPull::check(t, &vars).ok);
	}
	SUBCASE("a member pull: the struct exists, the field does not")
	{
		Graph gi = giGraph();
		const Variable t = intTarget(kPullFromGameInstance, "Run", "Deaths");
		CHECK(HcPull::addToTarget(gi, t) == HcPull::Add::NeedsMemberInStruct);
		HE::StructDef def;
		REQUIRE(HE::TypeRegistry::instance().getStruct(kStats, def));
		CHECK(HcPull::addMember(def, t, "Deaths"));
		REQUIRE(def.findField("Deaths") != nullptr);
		CHECK(def.findField("Deaths")->type == PinType::Int);
		CHECK_FALSE(HcPull::addMember(def, t, "Deaths"));    // already there
		CHECK_FALSE(HcPull::addMember(def, t, "Points"));    // a former name counts
	}
}
