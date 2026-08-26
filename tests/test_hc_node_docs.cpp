#include "doctest.h"

#include "HcNodeDocs.h"
#include "DocsLibrary.h"
#include "HcNodeReference.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/EngineApi.h>

#include <algorithm>
#include <vector>
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

TEST_CASE("node reference: the manual has an entry per callable thing")
{
	// The page the hover tooltip's F1 lands on. Generated from the same
	// registries the palette is built from, so "is it complete" is answerable
	// rather than a matter of somebody having remembered.
	HE::Ed::Docs::Library lib;
#ifdef HE_DOCS_BUNDLE_PATH
	REQUIRE(lib.load(HE_DOCS_BUNDLE_PATH));
#endif
	HE::Ed::NodeReference::install(lib);

	const HE::Ed::Docs::Page* page = lib.page(HE::Ed::NodeReference::kPageId);
	REQUIRE(page != nullptr);
	CHECK(page->sections.size() > 300);

	std::set<std::string> ids;
	for (const HE::Ed::Docs::Section& s : page->sections) ids.insert(s.id);

	for (const HE::api::ApiFn& fn : HE::api::registry())
		CHECK_MESSAGE(ids.count(fn.id) == 1,
		              "engine call missing from the node reference: ", std::string(fn.id));
	for (HorizonCode::NodeType t : HorizonCode::nodeRegistry())
	{
		std::string id = HorizonCode::nodeDisplayName(t);
		id.erase(std::remove(id.begin(), id.end(), ' '), id.end());
		if (id.empty()) continue;
		CHECK_MESSAGE(ids.count("node." + id) == 1,
		              "built-in node missing from the node reference: ", id);
	}

	// Installing twice must replace, not duplicate — the reader can reach
	// ensureLoaded with a library another reader already filled.
	const std::size_t before = lib.pages().size();
	HE::Ed::NodeReference::install(lib);
	CHECK(lib.pages().size() == before);
	CHECK(lib.page(HE::Ed::NodeReference::kPageId)->sections.size() == page->sections.size());

	// And it has to be findable: the whole point of it being a page rather than
	// a special screen is that search reaches it.
	const std::vector<HE::Ed::Docs::Hit> hits = lib.search("addImpulse");
	REQUIRE(!hits.empty());
	CHECK(lib.pages()[hits[0].page].id == HE::Ed::NodeReference::kPageId);
	CHECK(lib.pages()[hits[0].page].sections[hits[0].section].id == "physics.addImpulse");
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

	// The other direction: a field sentence whose row was removed from the
	// engine's X-list would sit here forever, describing something that no
	// longer exists and that nothing will ever show.
	std::set<std::string> ids;
	for (const HE::api::ApiFn& fn : HE::api::registry()) ids.insert(fn.id ? fn.id : "");
	for (int i = 0; i < NodeDocs::envFieldCount(); ++i)
	{
		const std::string name(NodeDocs::envFieldName(i));
		CHECK_MESSAGE(ids.count("env.get" + name) == 1,
		              "described sky field is not in the engine's list: ", name);
	}
}
