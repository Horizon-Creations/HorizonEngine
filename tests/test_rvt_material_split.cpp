#include "doctest.h"
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtSplit.h>
#include <algorithm>
#include <set>
#include <string>

// RVT 2.0 step 1 (Thema 335): the RVT Write / RVT Read nodes and the graph cut. A graph with a Write and a Read splits into
// one unlit page graph per connected Write input and a live graph; nothing after the Read node may reach a page or its hash.

namespace
{
	using T = HE::MatNodeType;

	struct Fixture
	{
		HE::MaterialGraph g;
		int out = 0, write = 0, read = 0;
		int preTint = 0, preRough = 0, shared = 0, postSnow = 0, mulBase = 0, mulPost = 0;
	};

	// PreTint*PreTint2 -> Write.BaseColor, PreRough -> Write.Roughness, Shared -> Write.AO AND Output.Metallic;
	// Read.BaseColor * PostSnow -> Output.BaseColor, Read.Roughness -> Output.Roughness, Read.Normal -> Output.Normal.
	Fixture build()
	{
		Fixture f;
		HE::MaterialGraph& g = f.g;
		f.out = g.addNode(T::Output);
		f.preTint  = g.addNode(T::ParamColor); g.findNode(f.preTint)->s = "PreTint";
		const int c = g.addNode(T::ConstColor);
		f.mulBase  = g.addNode(T::Multiply);
		f.preRough = g.addNode(T::ParamFloat); g.findNode(f.preRough)->s = "PreRough";
		f.shared   = g.addNode(T::ParamFloat); g.findNode(f.shared)->s = "SharedAO";
		f.write    = g.addNode(T::RvtWrite);   g.findNode(f.write)->s = "Ground";
		f.read     = g.addNode(T::RvtRead);    g.findNode(f.read)->s = "Ground";
		f.postSnow = g.addNode(T::ParamColor); g.findNode(f.postSnow)->s = "PostSnow";
		f.mulPost  = g.addNode(T::Multiply);
		g.connect(f.preTint, 0, f.mulBase, 0);
		g.connect(c, 0, f.mulBase, 1);
		g.connect(f.mulBase, 0, f.write, HE::kRvtWriteBaseColorPin);
		g.connect(f.preRough, 0, f.write, HE::kRvtWriteRoughnessPin);
		g.connect(f.shared, 0, f.write, HE::kRvtWriteAOPin);
		g.connect(f.shared, 0, f.out, HE::kMatOutputMetallicPin);
		g.connect(f.read, HE::kRvtReadBaseColorPin, f.mulPost, 0);
		g.connect(f.postSnow, 0, f.mulPost, 1);
		g.connect(f.mulPost, 0, f.out, HE::kMatOutputBaseColorPin);
		g.connect(f.read, HE::kRvtReadRoughnessPin, f.out, HE::kMatOutputRoughnessPin);
		g.connect(f.read, HE::kRvtReadNormalPin, f.out, HE::kMatOutputNormalPin);
		return f;
	}

	bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
	const HE::MatRvtPage* pageFor(const HE::MatRvtSplit& s, int pin)
	{
		for (const HE::MatRvtPage& p : s.pages) if (p.writePin == pin) return &p;
		return nullptr;
	}
}

TEST_CASE("RVT nodes: Write takes base color, normal, roughness, AO and three optional pins; Read gives five")
{
	const HE::MatNodeDesc& w = HE::matNodeDesc(T::RvtWrite);
	REQUIRE(w.inputs.size() == HE::kRvtWritePinCount);
	CHECK(w.outputs.empty());
	CHECK(std::string(w.inputs[HE::kRvtWriteBaseColorPin].name) == "Base Color");
	CHECK(std::string(w.inputs[HE::kRvtWriteNormalPin].name) == "Normal");
	CHECK(std::string(w.inputs[HE::kRvtWriteRoughnessPin].name) == "Roughness");
	CHECK(std::string(w.inputs[HE::kRvtWriteAOPin].name) == "AO");
	CHECK(std::string(w.inputs[HE::kRvtWriteSpecularPin].name) == "Specular");
	CHECK(std::string(w.inputs[HE::kRvtWriteHeightPin].name) == "Height");
	CHECK(std::string(w.inputs[HE::kRvtWriteMaskPin].name) == "Mask");
	const HE::MatNodeDesc& r = HE::matNodeDesc(T::RvtRead);
	CHECK(r.inputs.empty());
	REQUIRE(r.outputs.size() == 5);
	CHECK(std::string(r.outputs[HE::kRvtReadBaseColorPin].name) == "Base Color");
	CHECK(std::string(r.outputs[HE::kRvtReadNormalPin].name) == "Normal");
	CHECK(std::string(r.outputs[HE::kRvtReadRoughnessPin].name) == "Roughness");
	CHECK(std::string(r.outputs[HE::kRvtReadAOPin].name) == "AO");
	CHECK(std::string(r.outputs[HE::kRvtReadValidPin].name) == "Valid");
	CHECK(HE::matNodeDescByName("RVT Write")->type == T::RvtWrite);
	CHECK(HE::matNodeDescByName("RVT Read")->type == T::RvtRead);
	// Appended after RvtSample: the older enum values keep their numbers.
	CHECK((int)T::RvtWrite == (int)T::RvtSample + 1);
	CHECK((int)T::RvtRead == (int)T::RvtSample + 2);
}

TEST_CASE("RVT nodes survive the graph JSON (types by name, the RVT name and density kept)")
{
	Fixture f = build();
	f.g.findNode(f.write)->p[0] = 8.0f;
	HE::MaterialGraph back;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(f.g), back));
	const HE::MatGraphNode* w = back.findNode(f.write);
	const HE::MatGraphNode* r = back.findNode(f.read);
	REQUIRE(w); REQUIRE(r);
	CHECK(w->type == T::RvtWrite); CHECK(w->s == "Ground"); CHECK(w->p[0] == 8.0f);
	CHECK(r->type == T::RvtRead);  CHECK(r->s == "Ground");
	CHECK(back.links.size() == f.g.links.size());
}

TEST_CASE("RVT split: one page graph per connected Write input, the live graph keeps only what comes after Read")
{
	Fixture f = build();
	const HE::MatRvtSplit s = HE::matSplitRvt(f.g);
	CHECK(s.hasRvtNodes);
	CHECK(s.ok());
	REQUIRE(s.pages.size() == 3);   // Base Color, Roughness, AO: the three connected pins
	REQUIRE(pageFor(s, HE::kRvtWriteBaseColorPin));
	REQUIRE(pageFor(s, HE::kRvtWriteRoughnessPin));
	REQUIRE(pageFor(s, HE::kRvtWriteAOPin));
	CHECK(pageFor(s, HE::kRvtWriteNormalPin) == nullptr);   // unconnected: no page

	const HE::MatRvtPage& base = *pageFor(s, HE::kRvtWriteBaseColorPin);
	CHECK(base.rvtName == "Ground");
	CHECK(base.pinName == "Base Color");
	// Exactly the nodes the pin depends on: PreTint, the constant, the Multiply.
	CHECK(base.sourceNodes.size() == 3);
	const std::string baseGlsl = HE::generateFragmentGlsl(base.graph);
	CHECK(has(baseGlsl, "param: PreTint"));
	CHECK_FALSE(has(baseGlsl, "PostSnow"));
	CHECK_FALSE(has(baseGlsl, "PreRough"));
	CHECK_FALSE(has(baseGlsl, "heRvtSample"));
	CHECK_FALSE(has(baseGlsl, "no Output node"));

	const std::string roughGlsl = HE::generateFragmentGlsl(pageFor(s, HE::kRvtWriteRoughnessPin)->graph);
	CHECK(has(roughGlsl, "param: PreRough"));
	CHECK_FALSE(has(roughGlsl, "param: PreTint"));
	const std::string aoGlsl = HE::generateFragmentGlsl(pageFor(s, HE::kRvtWriteAOPin)->graph);
	CHECK(has(aoGlsl, "param: SharedAO"));

	// Each page graph holds one Output, unlit.
	for (const HE::MatRvtPage& p : s.pages)
	{
		int outputs = 0;
		for (const HE::MatGraphNode& n : p.graph.nodes)
		{
			if (n.type == T::Output) { ++outputs; CHECK(n.p[0] == 0.0f); }
			CHECK(n.type != T::RvtRead);
			CHECK(n.type != T::RvtWrite);
		}
		CHECK(outputs == 1);
		CHECK(p.hash != 0);
	}

	// The live graph: no Write, none of the nodes only a page needed; the SHARED node is in both.
	CHECK(s.live.findNode(f.write) == nullptr);
	CHECK(s.live.findNode(f.preTint) == nullptr);
	CHECK(s.live.findNode(f.preRough) == nullptr);
	CHECK(s.live.findNode(f.mulBase) == nullptr);
	CHECK(s.live.findNode(f.read) != nullptr);
	CHECK(s.live.findNode(f.postSnow) != nullptr);
	CHECK(s.live.findNode(f.shared) != nullptr);
	const std::string live = HE::generateFragmentGlsl(s.live);
	CHECK(has(live, "heRvtSample(vWorldPos.xz"));
	CHECK(has(live, "param: PostSnow"));
	CHECK(has(live, "param: SharedAO"));
	CHECK_FALSE(has(live, "param: PreTint"));
	CHECK_FALSE(has(live, "param: PreRough"));
	// The two volume vectors are parameters named after the RVT, for the engine to fill.
	CHECK(has(live, "// param: ")); // sanity
	const HE::MatShaderGen gen = HE::generateFragment(s.live);
	bool a = false, b = false;
	for (const HE::MatParamSlot& p : gen.params)
	{
		a = a || p.name == HE::matRvtVolumeParamName("Ground", 'A');
		b = b || p.name == HE::matRvtVolumeParamName("Ground", 'B');
	}
	CHECK(a); CHECK(b);
	// The source graph is untouched.
	CHECK(f.g.findNode(f.write) != nullptr);
}

TEST_CASE("RVT split: the Read node's pins come out in the Write order (base, normal, roughness, AO, valid)")
{
	Fixture f = build();
	const HE::MatRvtSplit s = HE::matSplitRvt(f.g);
	const std::string live = HE::generateFragmentGlsl(s.live);
	// Read.Roughness feeds Output.Roughness: the lit call must get the lookup's roughness (_r) there, not its AO (_ao).
	// (The variable prefix is the emit counter, not the node id, so match the suffixes.)
	const size_t lit = live.find("heLitP(");
	REQUIRE(lit != std::string::npos);
	const std::string call = live.substr(lit, live.find(')', lit) - lit);
	CHECK(has(call, "_r, vWorldPos"));
	CHECK_FALSE(has(call, "_ao, vWorldPos"));
	CHECK(has(live, "_ok;"));   // Valid is the lookup's fifth output
	(void)f;
}

TEST_CASE("RVT split: a Write with no Read still builds its pages, and a graph with a Read and no Write needs none")
{
	{
		Fixture f = build();
		// Cut every Read link: the live side no longer reads the RVT at all.
		for (size_t i = f.g.links.size(); i-- > 0;)
			if (f.g.links[i].srcNode == f.read) f.g.links.erase(f.g.links.begin() + i);
		f.g.removeNode(f.read);
		const HE::MatRvtSplit s = HE::matSplitRvt(f.g);
		CHECK(s.hasRvtNodes);
		CHECK(s.pages.size() == 3);
		CHECK(s.ok());
		CHECK_FALSE(has(HE::generateFragmentGlsl(s.live), "heRvtSample"));
	}
	{
		HE::MaterialGraph g;
		const int out = g.addNode(T::Output);
		const int rd  = g.addNode(T::RvtRead);
		g.connect(rd, HE::kRvtReadBaseColorPin, out, HE::kMatOutputBaseColorPin);
		const HE::MatRvtSplit s = HE::matSplitRvt(g);
		CHECK(s.hasRvtNodes);
		CHECK(s.pages.empty());
		CHECK(s.ok());                        // the Write may live in another material
		CHECK(s.live.nodes.size() == g.nodes.size());
		CHECK(has(HE::generateFragmentGlsl(s.live), "heRvtSample"));
	}
}

TEST_CASE("RVT split: nothing after Read enters a page or its hash")
{
	Fixture f = build();
	const HE::MatRvtSplit base = HE::matSplitRvt(f.g);
	REQUIRE(base.pages.size() == 3);

	const std::set<int> after = HE::matRvtDownstreamOfRead(f.g);
	CHECK(after.count(f.read) == 1);
	CHECK(after.count(f.mulPost) == 1);
	CHECK(after.count(f.out) == 1);
	CHECK(after.count(f.postSnow) == 0);   // feeds the live side, but is not downstream of the Read
	CHECK(after.count(f.shared) == 0);
	for (const HE::MatRvtPage& p : base.pages)
		for (int id : p.sourceNodes) CHECK(after.count(id) == 0);

	// Editing the live side (a parameter after Read, the Output, a new live node, the layout) repaints no page.
	Fixture g = build();
	g.g.findNode(g.postSnow)->p[0] = 0.123f;
	g.g.findNode(g.postSnow)->s = "PostSnow";
	g.g.findNode(g.out)->p[1] = 1.0f;
	g.g.findNode(g.mulPost)->x = 900.0f;
	g.g.findNode(g.preTint)->x = 123.0f;                      // layout of a page node: cosmetic
	g.g.findNode(g.preTint)->tooltip = "now with a tooltip";  // metadata: cosmetic
	const int extra = g.g.addNode(T::ConstFloat);
	g.g.connect(extra, 0, g.out, HE::kMatOutputOpacityPin);
	const HE::MatRvtSplit edited = HE::matSplitRvt(g.g);
	REQUIRE(edited.pages.size() == 3);
	for (size_t i = 0; i < 3; ++i) CHECK(edited.pages[i].hash == base.pages[i].hash);

	// Editing something BEFORE the Write repaints exactly the pages that depend on it.
	Fixture h = build();
	h.g.findNode(h.preRough)->p[0] = 0.77f;
	const HE::MatRvtSplit rough = HE::matSplitRvt(h.g);
	CHECK(pageFor(rough, HE::kRvtWriteRoughnessPin)->hash != pageFor(base, HE::kRvtWriteRoughnessPin)->hash);
	CHECK(pageFor(rough, HE::kRvtWriteBaseColorPin)->hash == pageFor(base, HE::kRvtWriteBaseColorPin)->hash);
	CHECK(pageFor(rough, HE::kRvtWriteAOPin)->hash == pageFor(base, HE::kRvtWriteAOPin)->hash);

	// The shared node feeds a page AND the live side: its change repaints the AO page only.
	Fixture k = build();
	k.g.findNode(k.shared)->p[0] = 0.31f;
	const HE::MatRvtSplit sh = HE::matSplitRvt(k.g);
	CHECK(pageFor(sh, HE::kRvtWriteAOPin)->hash != pageFor(base, HE::kRvtWriteAOPin)->hash);
	CHECK(pageFor(sh, HE::kRvtWriteBaseColorPin)->hash == pageFor(base, HE::kRvtWriteBaseColorPin)->hash);

	// The Write node's own settings (density) repaint every page of that RVT.
	Fixture m = build();
	m.g.findNode(m.write)->p[0] = 8.0f;
	const HE::MatRvtSplit dense = HE::matSplitRvt(m.g);
	for (size_t i = 0; i < 3; ++i) CHECK(dense.pages[i].hash != base.pages[i].hash);

	// The order of the link vector is an editing accident and must not matter.
	Fixture o = build();
	std::reverse(o.g.links.begin(), o.g.links.end());
	const HE::MatRvtSplit rev = HE::matSplitRvt(o.g);
	REQUIRE(rev.pages.size() == 3);
	for (size_t i = 0; i < 3; ++i) CHECK(rev.pages[i].hash == base.pages[i].hash);
}

TEST_CASE("RVT split, negative: a Write input that depends on a Read gets no page and is reported")
{
	Fixture f = build();
	// Read.Base Color * PostSnow already exists as mulPost; wire it back into the Write's Normal pin.
	f.g.connect(f.mulPost, 0, f.write, HE::kRvtWriteNormalPin);
	const HE::MatRvtSplit s = HE::matSplitRvt(f.g);
	CHECK_FALSE(s.ok());
	REQUIRE(s.problems.size() == 1);
	CHECK(s.problems[0].find("Normal") != std::string::npos);
	CHECK(s.problems[0].find("RVT Read") != std::string::npos);
	CHECK(pageFor(s, HE::kRvtWriteNormalPin) == nullptr);
	CHECK(s.pages.size() == 3);            // the three clean pins still get theirs
	// The live graph keeps the Read chain.
	CHECK(s.live.findNode(f.mulPost) != nullptr);
	CHECK(s.live.findNode(f.read) != nullptr);
}

TEST_CASE("RVT split: a second Write of the same RVT name is ignored and reported")
{
	Fixture f = build();
	const int second = f.g.addNode(T::RvtWrite);
	f.g.findNode(second)->s = "Ground";
	f.g.connect(f.preRough, 0, second, HE::kRvtWriteHeightPin);
	const HE::MatRvtSplit s = HE::matSplitRvt(f.g);
	CHECK_FALSE(s.ok());
	CHECK(s.pages.size() == 3);            // the lower id wins; the second Write cut no page
	CHECK(s.live.findNode(second) == nullptr);
}

TEST_CASE("RVT split: a graph without RVT nodes is returned as it is and compiles with no RVT code")
{
	HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
	const HE::MatRvtSplit s = HE::matSplitRvt(g);
	CHECK_FALSE(s.hasRvtNodes);
	CHECK(s.pages.empty());
	CHECK(s.ok());
	CHECK(s.live.nodes.size() == g.nodes.size());
	CHECK(s.live.links.size() == g.links.size());
	CHECK(HE::generateFragmentGlsl(s.live) == HE::generateFragmentGlsl(g));
	CHECK_FALSE(has(HE::generateFragmentGlsl(g), "heRvt"));
}

// ── RVT 2.0 step 2 (Thema 335): RVT off = the Write node is skipped, the Read node is replaced by what feeds the Write ──
namespace
{
	// Switches the global RVT answer for one scope, and puts it back.
	struct RvtSwitch
	{
		bool before = HE::matRvtEnabled();
		explicit RvtSwitch(bool on) { HE::matSetRvtEnabled(on); }
		~RvtSwitch() { HE::matSetRvtEnabled(before); }
	};

	// The fixture's graph as a material author would have drawn it without the pair: the Write's sources go straight to the
	// consumers of the Read's matching outputs. Same node ids, so the generated variable names are the same.
	HE::MaterialGraph withoutRvt(const Fixture& f)
	{
		HE::MaterialGraph g = f.g;
		g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const HE::MatGraphLink& l) {
			return l.dstNode == f.write || l.srcNode == f.read; }), g.links.end());
		g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const HE::MatGraphNode& n) {
			return n.id == f.write || n.id == f.read; }), g.nodes.end());
		g.connect(f.mulBase, 0, f.mulPost, 0);                                   // Write.BaseColor -> Read.BaseColor
		g.connect(f.preRough, 0, f.out, HE::kMatOutputRoughnessPin);              // Write.Roughness -> Read.Roughness
		return g;                                                                // Read.Normal: the Write's Normal was unconnected
	}
}

TEST_CASE("RVT off: matInlineRvt gives the graph a material without the pair would have, and generateFragment makes the same text")
{
	const Fixture f = build();
	const HE::MaterialGraph plain = withoutRvt(f);

	const HE::MaterialGraph inl = HE::matInlineRvt(f.g);
	CHECK(inl.findNode(f.write) == nullptr);
	CHECK(inl.findNode(f.read) == nullptr);
	CHECK(inl.nodes.size() == plain.nodes.size());
	CHECK(inl.links.size() == plain.links.size());

	std::string onGlsl;
	{ RvtSwitch on(true);  onGlsl = HE::generateFragmentGlsl(f.g); }
	CHECK(has(onGlsl, "heRvtSample"));                                           // with RVT on the Read node is the lookup
	std::string offGlsl;
	{ RvtSwitch off(false); offGlsl = HE::generateFragmentGlsl(f.g); }
	CHECK_FALSE(has(offGlsl, "heRvt"));                                          // off: no lookup, no pool, no volume parameters
	std::string plainGlsl;
	{ RvtSwitch off(false); plainGlsl = HE::generateFragmentGlsl(plain); }
	CHECK(offGlsl == plainGlsl);                                                 // the same shader as a graph that never had the pair
	CHECK(offGlsl != onGlsl);
}

TEST_CASE("RVT off: the Read's Valid output is 1, an unconnected Write input leaves the consumer on its own default, a chain settles")
{
	Fixture f = build();
	f.g.connect(f.read, HE::kRvtReadValidPin, f.out, HE::kMatOutputAOPin);
	const HE::MaterialGraph inl = HE::matInlineRvt(f.g);
	int constNode = 0;
	for (const HE::MatGraphLink& l : inl.links)
		if (l.dstNode == f.out && l.dstPin == HE::kMatOutputAOPin) constNode = l.srcNode;
	REQUIRE(constNode != 0);
	const HE::MatGraphNode* c = inl.findNode(constNode);
	REQUIRE(c != nullptr);
	CHECK(c->type == T::ConstFloat);
	CHECK(c->p[0] == doctest::Approx(1.0f));
	// The Write's Normal input was never connected: the Output's Normal pin has no link after the inline.
	for (const HE::MatGraphLink& l : inl.links) CHECK_FALSE((l.dstNode == f.out && l.dstPin == HE::kMatOutputNormalPin));

	// A Read of an RVT nobody writes: Valid is 0 and the other outputs are unconnected.
	HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
	const int out = [&] { for (const HE::MatGraphNode& n : g.nodes) if (n.type == T::Output) return n.id; return 0; }();
	const int rd = g.addNode(T::RvtRead); g.findNode(rd)->s = "Nowhere";
	g.connect(rd, HE::kRvtReadValidPin, out, HE::kMatOutputAOPin);
	const HE::MaterialGraph lone = HE::matInlineRvt(g);
	CHECK(lone.findNode(rd) == nullptr);
	for (const HE::MatGraphLink& l : lone.links)
		if (l.dstNode == out && l.dstPin == HE::kMatOutputAOPin) CHECK(lone.findNode(l.srcNode)->p[0] == doctest::Approx(0.0f));

	// Read A feeds Write B, Read B feeds the Output: the chain resolves to A's source.
	HE::MaterialGraph ch = HE::MaterialGraph::makeDefault();
	int o2 = 0; for (const HE::MatGraphNode& n : ch.nodes) if (n.type == T::Output) o2 = n.id;
	const int src = ch.addNode(T::ParamColor); ch.findNode(src)->s = "Src";
	const int wa = ch.addNode(T::RvtWrite); ch.findNode(wa)->s = "A";
	const int ra = ch.addNode(T::RvtRead);  ch.findNode(ra)->s = "A";
	const int wb = ch.addNode(T::RvtWrite); ch.findNode(wb)->s = "B";
	const int rb = ch.addNode(T::RvtRead);  ch.findNode(rb)->s = "B";
	ch.connect(src, 0, wa, HE::kRvtWriteBaseColorPin);
	ch.connect(ra, HE::kRvtReadBaseColorPin, wb, HE::kRvtWriteBaseColorPin);
	ch.connect(rb, HE::kRvtReadBaseColorPin, o2, HE::kMatOutputBaseColorPin);
	const HE::MaterialGraph settled = HE::matInlineRvt(ch);
	bool direct = false;
	for (const HE::MatGraphLink& l : settled.links)
		if (l.dstNode == o2 && l.dstPin == HE::kMatOutputBaseColorPin) direct = l.srcNode == src;
	CHECK(direct);
}

TEST_CASE("RVT off: a graph without RVT nodes is untouched, and the switch reads fresh on every call")
{
	const HE::MaterialGraph g = HE::MaterialGraph::makeDefault();
	const HE::MaterialGraph same = HE::matInlineRvt(g);
	CHECK(same.nodes.size() == g.nodes.size());
	CHECK(same.links.size() == g.links.size());
	{ RvtSwitch off(false); CHECK_FALSE(HE::matRvtEnabled()); CHECK(HE::generateFragmentGlsl(g) == [&] { RvtSwitch on(true); return HE::generateFragmentGlsl(g); }()); }
	{ RvtSwitch on(true);   CHECK(HE::matRvtEnabled()); }
}
