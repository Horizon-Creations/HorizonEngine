#include "doctest.h"
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtSplit.h>
#include <string>

// RVT 2.0 step 3 (Thema 335): the engine derives from the page subgraphs what makes a page stale. A parameter behind the Read
// node paints no page; a parameter before the Write node repaints only the pins that read it; a sculpt stroke reaches only
// pages that read the height, a paint stroke only pages that read the weights.

namespace
{
	using T = HE::MatNodeType;

	// BaseColor page: Lerp(ColorA, ColorB, Blend) * ParamTint ; Roughness page: ParamRough ; AO page: LayerBlend weights.
	// Live: Read.BaseColor * SnowTint -> Output.BaseColor, Read.Roughness -> Output.Roughness.
	struct Fx
	{
		HE::MaterialGraph g;
		int write = 0, read = 0, tint = 0, rough = 0, blend = 0, snow = 0, layers = 0, pos = 0, sw = 0, swA = 0, swB = 0, tex = 0;
	};

	Fx build()
	{
		Fx f;
		HE::MaterialGraph& g = f.g;
		const int out = g.addNode(T::Output);
		f.tint  = g.addNode(T::ParamColor); g.findNode(f.tint)->s = "Tint";
		f.rough = g.addNode(T::ParamFloat); g.findNode(f.rough)->s = "Rough";
		f.snow  = g.addNode(T::ParamColor); g.findNode(f.snow)->s = "SnowTint";
		const int mulBase = g.addNode(T::Multiply);
		const int mulLive = g.addNode(T::Multiply);
		f.write = g.addNode(T::RvtWrite); g.findNode(f.write)->s = "Ground";
		f.read  = g.addNode(T::RvtRead);  g.findNode(f.read)->s  = "Ground";
		const int c = g.addNode(T::ConstColor);
		g.connect(f.tint, 0, mulBase, 0);
		g.connect(c, 0, mulBase, 1);
		g.connect(mulBase, 0, f.write, HE::kRvtWriteBaseColorPin);
		g.connect(f.rough, 0, f.write, HE::kRvtWriteRoughnessPin);
		g.connect(f.read, HE::kRvtReadBaseColorPin, mulLive, 0);
		g.connect(f.snow, 0, mulLive, 1);
		g.connect(mulLive, 0, out, HE::kMatOutputBaseColorPin);
		g.connect(f.read, HE::kRvtReadRoughnessPin, out, HE::kMatOutputRoughnessPin);
		return f;
	}

	HE::MatRvtEnv env(float tint = 0.5f, float rough = 0.6f, float snow = 0.9f)
	{
		HE::MatRvtEnv e;
		e.params["Tint"]     = { tint, tint, tint, 1.0f };
		e.params["Rough"]    = { rough, 0.0f, 0.0f, 0.0f };
		e.params["SnowTint"] = { snow, snow, snow, 1.0f };
		return e;
	}

	const HE::MatRvtPage* page(const HE::MatRvtSplit& s, int pin)
	{
		for (const HE::MatRvtPage& p : s.pages) if (p.writePin == pin) return &p;
		return nullptr;
	}
}

TEST_CASE("RVT invalidation: a parameter AFTER the Read node changes no page hash, a parameter BEFORE the Write node does")
{
	const Fx f = build();
	const HE::MatRvtSplit split = HE::matSplitRvt(f.g);
	REQUIRE(split.ok());
	REQUIRE(split.pages.size() == 2);
	const HE::MatRvtMaterialState base = HE::matRvtMaterialState(f.g, split, env());

	// Behind Read: the snow tint, however far it moves.
	const HE::MatRvtMaterialState snowy = HE::matRvtMaterialState(f.g, split, env(0.5f, 0.6f, 0.1f));
	CHECK(snowy.state == base.state);
	CHECK(snowy.pageHashes == base.pageHashes);

	// Before Write: the tint belongs to the Base Color page only, the roughness to the Roughness page only.
	const HE::MatRvtMaterialState tinted = HE::matRvtMaterialState(f.g, split, env(0.8f));
	CHECK(tinted.state != base.state);
	const HE::MatRvtMaterialState rougher = HE::matRvtMaterialState(f.g, split, env(0.5f, 0.2f));
	CHECK(rougher.state != base.state);
	for (size_t i = 0; i < split.pages.size(); ++i)
	{
		const bool isBase = split.pages[i].writePin == HE::kRvtWriteBaseColorPin;
		CHECK((tinted.pageHashes[i] != base.pageHashes[i]) == isBase);
		CHECK((rougher.pageHashes[i] != base.pageHashes[i]) == !isBase);
	}
}

TEST_CASE("RVT invalidation: a changed Write pin moves the hash of its pool layer only (colour, normal, AO + roughness)")
{
	// One parameter per pin, so each pin has an input of its own to change.
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int cTint = g.addNode(T::ParamColor);  g.findNode(cTint)->s = "Tint";
	const int cNorm = g.addNode(T::ParamColor);  g.findNode(cNorm)->s = "NormalVec";
	const int fRough = g.addNode(T::ParamFloat); g.findNode(fRough)->s = "Rough";
	const int fAo = g.addNode(T::ParamFloat);    g.findNode(fAo)->s = "Ao";
	const int cSnow = g.addNode(T::ParamColor);  g.findNode(cSnow)->s = "SnowTint";
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "Ground";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "Ground";
	const int mulLive = g.addNode(T::Multiply);
	g.connect(cTint, 0, wr, HE::kRvtWriteBaseColorPin);
	g.connect(cNorm, 0, wr, HE::kRvtWriteNormalPin);
	g.connect(fRough, 0, wr, HE::kRvtWriteRoughnessPin);
	g.connect(fAo, 0, wr, HE::kRvtWriteAOPin);
	g.connect(rd, HE::kRvtReadBaseColorPin, mulLive, 0);
	g.connect(cSnow, 0, mulLive, 1);
	g.connect(mulLive, 0, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.ok());
	REQUIRE(split.pages.size() == 4);

	struct Vals { float tint = 0.5f, norm = 0.3f, rough = 0.6f, ao = 0.8f, snow = 0.9f; };
	auto layerState = [&](const Vals& v) {
		HE::MatRvtEnv e;
		e.params["Tint"]      = { v.tint, v.tint, v.tint, 1.0f };
		e.params["NormalVec"] = { v.norm, v.norm, 1.0f, 1.0f };
		e.params["Rough"]     = { v.rough, 0.0f, 0.0f, 0.0f };
		e.params["Ao"]        = { v.ao, 0.0f, 0.0f, 0.0f };
		e.params["SnowTint"]  = { v.snow, v.snow, v.snow, 1.0f };
		return HE::matRvtLayerState(split, HE::matRvtMaterialState(g, split, e), "Ground");
	};
	const HE::MatRvtLayerState base = layerState({});
	for (int k = 0; k < 3; ++k) CHECK(base.hash[k] != 0);
	CHECK(base.hash[0] != base.hash[1]);
	CHECK(base.hash[1] != base.hash[2]);

	// The same values again: nothing moved. The live half's snow tint: no layer.
	{ const HE::MatRvtLayerState s = layerState({}); for (int k = 0; k < 3; ++k) CHECK(s.hash[k] == base.hash[k]); }
	{ Vals v; v.snow = 0.1f; const HE::MatRvtLayerState s = layerState(v); for (int k = 0; k < 3; ++k) CHECK(s.hash[k] == base.hash[k]); }

	// Each pin moves its own layer and no other one; AO and roughness share layer 2.
	struct Case { const char* what; Vals v; bool moved[3]; };
	Vals tint; tint.tint = 0.1f;
	Vals norm; norm.norm = 0.9f;
	Vals rough; rough.rough = 0.1f;
	Vals ao; ao.ao = 0.2f;
	const Case cases[] = {
		{ "Base Color", tint,  { true,  false, false } },
		{ "Normal",     norm,  { false, true,  false } },
		{ "Roughness",  rough, { false, false, true  } },
		{ "AO",         ao,    { false, false, true  } },
	};
	for (const Case& c : cases)
	{
		CAPTURE(c.what);
		const HE::MatRvtLayerState s = layerState(c.v);
		for (int k = 0; k < 3; ++k) { CAPTURE(k); CHECK((s.hash[k] != base.hash[k]) == c.moved[k]); }
	}
}

TEST_CASE("RVT invalidation: a layer's reads are those of the pages it is cut from; a layer without a page reads nothing")
{
	// Colour reads the height, roughness the weights, AO nothing; the normal pin is not connected.
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int wp = g.addNode(T::WorldPos);
	const int lb = g.addNode(T::LandscapeLayerBlend);
	const int flat = g.addNode(T::ConstColor);
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "G";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "G";
	g.connect(wp, 0, wr, HE::kRvtWriteBaseColorPin);
	g.connect(lb, 0, wr, HE::kRvtWriteRoughnessPin);
	g.connect(flat, 0, wr, HE::kRvtWriteAOPin);
	g.connect(rd, HE::kRvtReadBaseColorPin, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.ok());
	const HE::MatRvtLayerState s = HE::matRvtLayerState(split, HE::matRvtMaterialState(g, split, HE::MatRvtEnv{}), "G");
	CHECK(HE::matRvtEditReaches(s.reads[0], HE::kRvtEditSculpt));        // colour: height
	CHECK_FALSE(HE::matRvtEditReaches(s.reads[0], HE::kRvtEditPaint));
	CHECK(s.reads[1] == 0);                                              // normal: no page at all
	CHECK(HE::matRvtEditReaches(s.reads[2], HE::kRvtEditPaint));         // AO + roughness: the weights (roughness), AO adds nothing
	CHECK_FALSE(HE::matRvtEditReaches(s.reads[2], HE::kRvtEditSculpt));
	// Another RVT's name: no layer has a page.
	const HE::MatRvtLayerState none = HE::matRvtLayerState(split, HE::matRvtMaterialState(g, split, HE::MatRvtEnv{}), "Other");
	for (int k = 0; k < 3; ++k) CHECK(none.reads[k] == 0);
}

TEST_CASE("RVT invalidation: an edit of a node after the Read node, or of the graph's live half, leaves the state alone")
{
	Fx f = build();
	const HE::MatRvtMaterialState before = HE::matRvtMaterialState(f.g, HE::matSplitRvt(f.g), env());
	// A new live-only node and a rewire after the Read.
	const int extra = f.g.addNode(T::ConstFloat);
	f.g.findNode(extra)->p[0] = 0.3f;
	f.g.findNode(f.snow)->p[0] = 0.2f;   // the snow tint's own default, also after the Read
	const HE::MatRvtMaterialState after = HE::matRvtMaterialState(f.g, HE::matSplitRvt(f.g), env());
	CHECK(after.state == before.state);
}

TEST_CASE("RVT invalidation: entity overrides count like material values, a parameter nobody reads does not")
{
	const Fx f = build();
	const HE::MatRvtSplit split = HE::matSplitRvt(f.g);
	HE::MatRvtEnv a = env();
	HE::MatRvtEnv b = env();
	b.params["Unused"] = { 1, 2, 3, 4 };
	CHECK(HE::matRvtMaterialState(f.g, split, a).state == HE::matRvtMaterialState(f.g, split, b).state);
	b.params["Tint"] = { 0.9f, 0.5f, 0.5f, 1.0f };   // the entity's override of a page parameter
	CHECK(HE::matRvtMaterialState(f.g, split, a).state != HE::matRvtMaterialState(f.g, split, b).state);
}

TEST_CASE("RVT invalidation: only the taken side of a Static Switch is a dependency")
{
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int sw = g.addNode(T::StaticSwitch); g.findNode(sw)->s = "Detail"; g.findNode(sw)->p[0] = 1.0f;
	const int pa = g.addNode(T::ParamColor); g.findNode(pa)->s = "OnColor";
	const int pb = g.addNode(T::ParamColor); g.findNode(pb)->s = "OffColor";
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "G";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "G";
	g.connect(pa, 0, sw, 0);
	g.connect(pb, 0, sw, 1);
	g.connect(sw, 0, wr, HE::kRvtWriteBaseColorPin);
	g.connect(rd, HE::kRvtReadBaseColorPin, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.pages.size() == 1);

	HE::MatRvtEnv e;
	e.params["OnColor"] = { 1, 0, 0, 1 }; e.params["OffColor"] = { 0, 1, 0, 1 };
	const uint64_t h0 = HE::matRvtPageStateHash(g, split.pages[0], e);
	const HE::MatRvtPageDeps d = HE::matRvtPageDeps(g, split.pages[0], e);
	CHECK(d.params == std::vector<std::string>{ "OnColor" });
	CHECK(d.switches == std::vector<std::string>{ "Detail" });

	HE::MatRvtEnv offChanged = e;
	offChanged.params["OffColor"] = { 0, 0, 1, 1 };       // the culled side
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], offChanged) == h0);
	HE::MatRvtEnv onChanged = e;
	onChanged.params["OnColor"] = { 0, 0, 1, 1 };
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], onChanged) != h0);
	HE::MatRvtEnv flipped = e;
	flipped.switches["Detail"] = false;                    // the permutation flips: the other side is live now
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], flipped) != h0);
	CHECK(HE::matRvtPageDeps(g, split.pages[0], flipped).params == std::vector<std::string>{ "OffColor" });
}

TEST_CASE("RVT invalidation: a texture's revision is part of the page, a texture behind the Read node is not")
{
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int tex = g.addNode(T::TextureSample); g.findNode(tex)->s = "Textures/rock.png";
	const int live = g.addNode(T::TextureSample); g.findNode(live)->s = "Textures/snow.png";
	const int mul = g.addNode(T::Multiply);
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "G";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "G";
	g.connect(tex, 0, wr, HE::kRvtWriteBaseColorPin);
	g.connect(rd, HE::kRvtReadBaseColorPin, mul, 0);
	g.connect(live, 0, mul, 1);
	g.connect(mul, 0, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.pages.size() == 1);
	HE::MatRvtEnv e;
	e.assetRevision["Textures/rock.png"] = 7; e.assetRevision["Textures/snow.png"] = 3;
	const uint64_t h0 = HE::matRvtPageStateHash(g, split.pages[0], e);
	CHECK(HE::matRvtPageDeps(g, split.pages[0], e).assets == std::vector<std::string>{ "Textures/rock.png" });
	e.assetRevision["Textures/snow.png"] = 4;
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], e) == h0);
	e.assetRevision["Textures/rock.png"] = 8;
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], e) != h0);
}

TEST_CASE("RVT invalidation: which kind of terrain edit reaches which page")
{
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int wp = g.addNode(T::WorldPos);
	const int nrm = g.addNode(T::NormalWS);
	const int lb = g.addNode(T::LandscapeLayerBlend);
	const int flat = g.addNode(T::ConstColor);
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "G";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "G";
	g.connect(wp, 0, wr, HE::kRvtWriteBaseColorPin);       // height
	g.connect(lb, 0, wr, HE::kRvtWriteRoughnessPin);       // weights
	g.connect(flat, 0, wr, HE::kRvtWriteAOPin);            // nothing of the ground
	g.connect(nrm, 0, wr, HE::kRvtWriteNormalPin);         // normal
	g.connect(rd, HE::kRvtReadBaseColorPin, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.pages.size() == 4);
	const HE::MatRvtEnv e;
	auto reads = [&](int pin) { return HE::matRvtPageDeps(g, *page(split, pin), e).reads; };
	CHECK((reads(HE::kRvtWriteBaseColorPin) & HE::kRvtReadsHeight) != 0);
	CHECK(HE::matRvtEditReaches(reads(HE::kRvtWriteBaseColorPin), HE::kRvtEditSculpt));
	CHECK_FALSE(HE::matRvtEditReaches(reads(HE::kRvtWriteBaseColorPin), HE::kRvtEditPaint));
	CHECK(HE::matRvtEditReaches(reads(HE::kRvtWriteRoughnessPin), HE::kRvtEditPaint));
	CHECK_FALSE(HE::matRvtEditReaches(reads(HE::kRvtWriteRoughnessPin), HE::kRvtEditSculpt));
	CHECK(reads(HE::kRvtWriteAOPin) == 0);
	CHECK_FALSE(HE::matRvtEditReaches(reads(HE::kRvtWriteAOPin), HE::kRvtEditSculpt | HE::kRvtEditPaint));
	CHECK(HE::matRvtEditReaches(reads(HE::kRvtWriteNormalPin), HE::kRvtEditSculpt));
	const HE::MatRvtMaterialState ms = HE::matRvtMaterialState(g, split, e);
	CHECK(ms.reads == (HE::kRvtReadsHeight | HE::kRvtReadsNormal | HE::kRvtReadsWeights));
}

TEST_CASE("RVT invalidation: a Function Call reads everything unless the caller says otherwise; per-frame inputs are listed as frozen")
{
	HE::MaterialGraph g;
	const int out = g.addNode(T::Output);
	const int fn = g.addNode(T::FunctionCall); g.findNode(fn)->s = "Functions/Rock";
	const int time = g.addNode(T::Time);
	const int add = g.addNode(T::Add);
	const int wr = g.addNode(T::RvtWrite); g.findNode(wr)->s = "G";
	const int rd = g.addNode(T::RvtRead);  g.findNode(rd)->s = "G";
	g.connect(fn, 0, add, 0);
	g.connect(time, 0, add, 1);
	g.connect(add, 0, wr, HE::kRvtWriteBaseColorPin);
	g.connect(rd, HE::kRvtReadBaseColorPin, out, HE::kMatOutputBaseColorPin);
	const HE::MatRvtSplit split = HE::matSplitRvt(g);
	REQUIRE(split.pages.size() == 1);
	HE::MatRvtEnv e;
	HE::MatRvtPageDeps d = HE::matRvtPageDeps(g, split.pages[0], e);
	CHECK(d.reads == HE::kRvtReadsGround);
	CHECK(d.assets == std::vector<std::string>{ "Functions/Rock" });
	REQUIRE(d.frozen.size() == 1);
	// Time never enters the hash: the clock does not repaint a page.
	const uint64_t h0 = HE::matRvtPageStateHash(g, split.pages[0], e);
	e.functionReads["Functions/Rock"] = HE::kRvtReadsWeights;
	CHECK(HE::matRvtPageDeps(g, split.pages[0], e).reads == HE::kRvtReadsWeights);
	CHECK(HE::matRvtPageStateHash(g, split.pages[0], e) == h0);
}
