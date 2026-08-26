#include "doctest.h"

#include "HcNodeDocs.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <set>
#include <string>

// ── "What does this node do?" — for every node ───────────────────────────────
// The node reference is generated from the engine's own registries, so it can
// never list something that does not exist. The other direction is the one that
// needs a test: a call ADDED to the registry appears in the palette, in graphs
// and on the reference page with nothing to say about it, and nothing about
// that is an error at build time. This is that error.

using namespace HE::Ed;

TEST_CASE("node docs: every engine call in the registry has a description")
{
	const std::vector<HE::api::ApiFn>& registry = HE::api::registry();
	REQUIRE(registry.size() > 150);

	int checked = 0;
	for (const HE::api::ApiFn& fn : registry)
	{
		const std::string id(fn.id ? fn.id : "");
		CHECK_MESSAGE(NodeDocs::hasEngineCall(id),
		              "engine call with no description — add one to HcNodeDocs.cpp: ", id);
		const std::string doc = NodeDocs::engineCall(id);
		// Long enough to be a sentence rather than a restatement of the id. The
		// bar is low on purpose: "The text in upper case." IS the whole answer
		// for string.toUpper, and padding it out would make the reference worse,
		// not better. What this catches is a stub.
		CHECK_MESSAGE(doc.size() >= 20, "description too short to say anything: ", id);
		CHECK_MESSAGE(doc.find("  ") == std::string::npos,
		              "double space (a wrapped literal lost one): ", id);
		++checked;
	}
	INFO("engine calls documented: " << checked);
}

TEST_CASE("node docs: the table has no entry for a call that no longer exists")
{
	std::set<std::string> ids;
	for (const HE::api::ApiFn& fn : HE::api::registry()) ids.insert(fn.id ? fn.id : "");

	std::set<std::string> seen;
	for (int i = 0; i < NodeDocs::explicitCount(); ++i)
	{
		const std::string id(NodeDocs::explicitId(i));
		CHECK_MESSAGE(ids.count(id) == 1,
		              "described call is not in the registry (renamed? removed?): ", id);
		CHECK_MESSAGE(seen.insert(id).second, "described twice: ", id);
	}
}

TEST_CASE("node docs: every built-in node type explains itself too")
{
	// The built-ins live in HorizonCode::nodeTooltip and always did — this is
	// the guard that a node type added to the enum gets a line there, which is
	// the same failure in the other half of the palette.
	int checked = 0;
	for (HorizonCode::NodeType t : HorizonCode::nodeRegistry())
	{
		const char* tip = HorizonCode::nodeTooltip(t);
		const std::string s = tip ? tip : "";
		// Only "is there one at all": the arithmetic nodes answer in four words
		// ("Adds two values.") and are none the worse for it.
		CHECK_MESSAGE(!s.empty(), "built-in node with no tooltip: type ",
		              std::to_string(static_cast<int>(t)));
		++checked;
	}
	INFO("built-in node types documented: " << checked);
	CHECK(checked > 30);
}

TEST_CASE("node docs: the generated sky rows are covered by the field list")
{
	// Every env.get…/env.set… pair is built from one sentence keyed by the field
	// name. A sky property added to the X-list in EngineApi.h therefore shows up
	// here — undocumented — rather than in the manual.
	int env = 0;
	for (const HE::api::ApiFn& fn : HE::api::registry())
	{
		const std::string id(fn.id ? fn.id : "");
		if (id.rfind("env.", 0) != 0) continue;
		++env;
		const std::string doc = NodeDocs::engineCall(id);
		REQUIRE_MESSAGE(!doc.empty(), "sky property with no description: ", id);
		// Composed, not hand-written: the get/set wording has to be there.
		const bool isSet = id.rfind("env.set", 0) == 0;
		CHECK(doc.rfind(isSet ? "Sets " : "Reads ", 0) == 0);
		if (isSet)
			CHECK_MESSAGE(doc.find("Weather component") != std::string::npos,
			              "a sky WRITE must warn about the Weather override: ", id);
	}
	INFO("sky properties documented: " << env);
	CHECK(env >= 80);
}
