#include "doctest.h"
#include "HcFallbackReport.h"

#include <HorizonCode/HorizonCode.h>
#include <HorizonScene/HcCodegen.h>
#include <string>
#include <vector>

// A HorizonCode class the generator cannot translate does not fail the export:
// it ships interpreted (OnFailure::Interpret, what the export dialog offers by
// default). That is the right behaviour and nothing here changes it — but until
// the warning list existed, the only trace of it was one line in a build log
// that a compiler then buried, and hc_report.txt beside the generated sources.
// A build that compiled fifteen of sixteen classes looked, on screen, exactly
// like one that compiled all sixteen.
//
// These cases drive the real generator with graphs it genuinely cannot compile
// and assert that what comes out the other end names the class and says why.

using namespace HorizonCode;

namespace
{
	// Two Print nodes wired exec-out → exec-in in both directions: a loop with
	// no Delay in it, which the generator refuses (a Delay is what breaks the
	// chain — loops through one are the legitimate timer pattern).
	Graph execCycleGraph()
	{
		Graph g;
		Node ev; ev.type = NodeType::Event; ev.s = "Go";
		const int e = g.addNode(ev);
		Node p; p.type = NodeType::Print;
		const int a = g.addNode(p);
		const int b = g.addNode(p);
		// Pins are [execIns][execOuts][dataIns][dataOuts]: Print has one of each
		// exec, so 0 is its exec-in and 1 its exec-out.
		g.links.push_back({ e, 0, a, 0 });
		g.links.push_back({ a, 1, b, 0 });
		g.links.push_back({ b, 1, a, 0 });   // back to the top, no Delay between
		return g;
	}

	// A graph that compiles cleanly, so a case can tell "listed because it fell
	// back" apart from "listed because everything is listed".
	Graph trivialGraph()
	{
		Graph g;
		Node ev; ev.type = NodeType::Event; ev.s = "Go";
		g.addNode(ev);
		return g;
	}

	bool mentions(const std::string& haystack, const char* needle)
	{
		return haystack.find(needle) != std::string::npos;
	}
}

TEST_CASE("HcFallbackReport: a class that ships interpreted reaches the list with its reason")
{
	std::vector<HE::hccg::ClassSource> sources;
	sources.push_back({ "Content/Scripts/Patrol.hasset", "Patrol.hasset", execCycleGraph() });
	sources.push_back({ "Content/Scripts/Door.hasset",   "Door.hasset",   trivialGraph()   });

	HE::hccg::Options opt;                       // the default: Interpret on failure
	const HE::hccg::Result r = HE::hccg::generate(sources, opt);
	// The export still succeeds — that IS the fallback contract, and the whole
	// reason the verdict needed somewhere visible to live.
	CHECK(r.ok);
	REQUIRE(r.fallbacks.size() == 1);

	const auto notices = HcFallbackReport::collect(sources, r);
	REQUIRE(notices.size() == 1);
	CHECK(notices[0].key == "Content/Scripts/Patrol.hasset");
	// The label, not the registry key: the author named the asset, and a list
	// that answers in paths is a list they have to translate back.
	CHECK(notices[0].label == "Patrol.hasset");
	CHECK(mentions(notices[0].reason, "cycle"));
	CHECK(notices[0].node != 0);                 // enough to highlight the node

	// The one line the dialog draws carries both halves.
	const std::string line = HcFallbackReport::describe(notices[0]);
	CHECK(mentions(line, "Patrol.hasset"));
	CHECK(mentions(line, "cycle"));
	// The cycle reason already ends in "at node N" — saying it twice reads as
	// two different nodes.
	CHECK(line.find("node " + std::to_string(notices[0].node))
	      == line.rfind("node " + std::to_string(notices[0].node)));
}

TEST_CASE("HcFallbackReport: only the classes that fell back are listed")
{
	std::vector<HE::hccg::ClassSource> sources;
	sources.push_back({ "a", "a", trivialGraph() });
	sources.push_back({ "b", "b", trivialGraph() });

	HE::hccg::Options opt;
	const HE::hccg::Result r = HE::hccg::generate(sources, opt);
	REQUIRE(r.ok);
	REQUIRE(r.fallbacks.empty());
	CHECK(HcFallbackReport::collect(sources, r).empty());
}

TEST_CASE("HcFallbackReport: a class with no label is still named")
{
	std::vector<HE::hccg::ClassSource> sources;
	sources.push_back({ "level:Main", "", execCycleGraph() });

	HE::hccg::Options opt;
	const HE::hccg::Result r = HE::hccg::generate(sources, opt);
	REQUIRE(r.fallbacks.size() == 1);

	const auto notices = HcFallbackReport::collect(sources, r);
	REQUIRE(notices.size() == 1);
	CHECK(notices[0].label == "level:Main");
	CHECK(mentions(HcFallbackReport::describe(notices[0]), "level:Main"));
}

TEST_CASE("HcFallbackReport: the headline does not promise more compilate than shipped")
{
	// The ordinary case: some compiled, some did not.
	const std::string partial = HcFallbackReport::headline(16, 1, /*buildFailed=*/false);
	CHECK(mentions(partial, "1 of 16"));
	CHECK(mentions(partial, "interpreted"));

	// No library came out of the toolchain, so NOTHING ships compiled — "1 of
	// 16" would still be the wrong number to put on the screen.
	const std::string failed = HcFallbackReport::headline(16, 1, /*buildFailed=*/true);
	CHECK_FALSE(mentions(failed, "1 of 16"));
	CHECK(mentions(failed, "all 16"));
}
