#include "doctest.h"
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtSplit.h>
#include <MaterialGraph/WeatherMaterialFunctions.h>
#include <Trails/TrailClipmap.h>
#include <algorithm>
#include <string>
#include <vector>

// RVT 2.0 step 4 (Thema 335): the auto landscape material is cut at an RVT Write / RVT Read pair. What a page is painted from
// (layer blend of the groups, bombing, slope blend, painted layers) sits before the Write node; snow, the procedural sand's
// ripples, puddles and trails sit after the Read node. So the snow height or the weather never reaches a page hash, and a
// change to the layers does.

namespace
{
	using T = HE::MatNodeType;

	struct Variant { const char* name; HE::AutoLandscapeGroupId group; bool mixed; };
	const Variant kVariants[] = {
		{ "Mountains", HE::AutoLandscapeGroupId::Mountains, false },
		{ "Forest",    HE::AutoLandscapeGroupId::Forest,    false },
		{ "Grassland", HE::AutoLandscapeGroupId::Grassland, false },
		{ "Desert",    HE::AutoLandscapeGroupId::Desert,    false },
		{ "Mix",       HE::AutoLandscapeGroupId::Mountains, true  },
	};

	// The value a Param node takes in `ev` (an explicit entry) or, without one, the node's own default.
	void set(HE::MatRvtEnv& ev, const char* name, float x, float y = 0, float z = 0, float w = 0) { ev.params[name] = { x, y, z, w }; }

	HE::MatRvtMaterialState stateOf(const HE::AutoLandscapeGraph& g, const HE::MatRvtSplit& s, const HE::MatRvtEnv& ev)
	{
		return HE::matRvtMaterialState(g.graph, s, ev);
	}

	bool anyNode(const HE::MaterialGraph& g, T type)
	{
		for (const HE::MatGraphNode& n : g.nodes) if (n.type == type) return true;
		return false;
	}
	bool anyArrayTap(const HE::MaterialGraph& g)
	{
		for (const HE::MatGraphNode& n : g.nodes)
			if (n.type == T::TextureArraySample || n.type == T::NormalMapArraySample || n.type == T::TextureArrayBombSample
			    || n.type == T::NormalMapArrayBombSample) return true;
		return false;
	}
}

TEST_CASE("RVT auto landscape: every group's Lit graph is a Write/Read pair with four pages and nothing per-frame in them")
{
	for (const Variant& v : kVariants)
	{
		CAPTURE(v.name);
		const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, v.group, v.mixed);
		REQUIRE(g.rvtWrite != 0);
		REQUIRE(g.rvtRead != 0);
		const HE::MatRvtSplit s = HE::matSplitRvt(g.graph);
		CHECK(s.hasRvtNodes);
		for (const std::string& p : s.problems) MESSAGE(p);
		REQUIRE(s.ok());
		// Base colour, normal, roughness, AO. Height and Mask stay unconnected: a Read does not give them back.
		REQUIRE(s.pages.size() == 4);
		for (int pin = 0; pin < 4; ++pin) CHECK(s.pages[size_t(pin)].writePin == pin);

		HE::MatRvtEnv ev;
		const HE::MatRvtMaterialState st = stateOf(g, s, ev);
		for (size_t i = 0; i < s.pages.size(); ++i)
		{
			const HE::MatRvtPageDeps& d = st.pageDeps[i];
			CAPTURE(i);
			CHECK(d.frozen.empty());                       // no Time / Weather / Camera in a page: it is painted once
			CHECK((d.reads & HE::kRvtReadsWeights) != 0);  // the painted layers
			CHECK((d.reads & HE::kRvtReadsHeight) != 0);   // the world position's Y (rock/slope blend)
			for (const std::string& a : d.assets)
			{
				// Textures: the layer arrays, and nothing live. Functions: none (snow, sand and the weather are live).
				CHECK(a.find("Trail") == std::string::npos);
				CHECK(a.find("MF_") == std::string::npos);
			}
			for (int id : s.pages[i].sourceNodes)
			{
				const HE::MatGraphNode* n = g.graph.findNode(id);
				REQUIRE(n);
				CHECK(n->type != T::Weather);
				CHECK(n->type != T::FunctionCall);
				CHECK(n->type != T::CameraDistance);
				CHECK(n->type != T::RvtRead);
			}
		}
		// The live program holds no array tap: the layer arrays are only ever sampled while a page is painted.
		CHECK_FALSE(anyArrayTap(s.live));
		CHECK_FALSE(anyNode(s.live, T::RvtWrite));
		CHECK(anyNode(s.live, T::RvtRead));
		CHECK(anyNode(s.live, T::Weather));
		CHECK(anyNode(s.live, T::FunctionCall));            // MF_ProceduralSnow, MF_WeatherSnow, MF_WeatherPuddles
	}
}

TEST_CASE("RVT auto landscape: snow, sand, puddles and trails move no page hash; the layers do")
{
	for (const Variant& v : kVariants)
	{
		CAPTURE(v.name);
		const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, v.group, v.mixed);
		const HE::MatRvtSplit s = HE::matSplitRvt(g.graph);
		REQUIRE(s.ok());
		HE::MatRvtEnv base;
		const HE::MatRvtMaterialState b = stateOf(g, s, base);

		const char* const live[] = { HE::kAutoLandscapeParamSnowHeight, HE::kAutoLandscapeParamSnowBlend,
		                             HE::kAutoLandscapeParamSnowMaxSlope, HE::kAutoLandscapeParamPuddleAmount,
		                             HE::kAutoLandscapeParamPuddleMaxSlope, HE::kAutoLandscapeParamSnowColor,
		                             HE::kAutoLandscapeParamSnowLook, HE::kAutoLandscapeParamSandLook };
		for (const char* name : live)
		{
			CAPTURE(name);
			HE::MatRvtEnv ev = base;
			set(ev, name, 0.123f, 0.456f, 0.789f, 0.321f);
			const HE::MatRvtMaterialState m = stateOf(g, s, ev);
			CHECK(m.state == b.state);                  // an empty dirty set: no page is stale
			CHECK(m.pageHashes == b.pageHashes);
		}

		const char* const baked[] = { HE::kAutoLandscapeParamGroundTile, HE::kAutoLandscapeParamRockTile,
		                              HE::kAutoLandscapeParamBombCell, HE::kAutoLandscapeParamRockSlope,
		                              HE::kAutoLandscapeParamRockBlend, HE::kAutoLandscapeParamHeightBlend,
		                              HE::kAutoLandscapeParamDirtAmount, HE::kAutoLandscapeParamDirtSize };
		for (const char* name : baked)
		{
			CAPTURE(name);
			HE::MatRvtEnv ev = base;
			set(ev, name, 0.77f);
			const HE::MatRvtMaterialState m = stateOf(g, s, ev);
			CHECK(m.state != b.state);                  // a page reads it: it repaints
			size_t changed = 0;
			for (size_t i = 0; i < m.pageHashes.size(); ++i) changed += m.pageHashes[i] != b.pageHashes[i] ? 1 : 0;
			CHECK(changed >= 1);
			CHECK(changed <= 4);
		}

		// A sculpt stroke and a paint stroke both reach these pages (height for the slope, the weights for the layers).
		CHECK(HE::matRvtEditReaches(b.reads, HE::kRvtEditSculpt));
		CHECK(HE::matRvtEditReaches(b.reads, HE::kRvtEditPaint));
	}
}

TEST_CASE("RVT auto landscape: the Bombing switch is a page input, not a live one")
{
	const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph();
	const HE::MatRvtSplit s = HE::matSplitRvt(g.graph);
	REQUIRE(s.ok());
	HE::MatRvtEnv on, off;
	on.switches[HE::kAutoLandscapeSwitchBombing] = true;
	off.switches[HE::kAutoLandscapeSwitchBombing] = false;
	CHECK(stateOf(g, s, on).state != stateOf(g, s, off).state);
}

TEST_CASE("RVT auto landscape: with RVT off the pair is inlined into the whole material, with it on the live shader reads the pool")
{
	const HE::MatFunctionLoader fn = HE::weatherFunctionLoader();
	for (const Variant& v : kVariants)
	{
		CAPTURE(v.name);
		const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, v.group, v.mixed);
		const HE::MaterialGraph inl = HE::matInlineRvt(g.graph);
		CHECK_FALSE(anyNode(inl, T::RvtRead));
		CHECK_FALSE(anyNode(inl, T::RvtWrite));
		CHECK(anyArrayTap(inl));                        // RVT off: the arrays are sampled per pixel again

		const bool was = HE::matRvtEnabled();
		HE::matSetRvtEnabled(false);
		const HE::MatShaderGen off = HE::generateFragment(g.graph, fn);
		HE::matSetRvtEnabled(true);
		const HE::MatShaderGen on = HE::generateFragment(g.graph, fn);
		HE::matSetRvtEnabled(was);
		REQUIRE_FALSE(off.glsl.empty());
		REQUIRE_FALSE(on.glsl.empty());
		CHECK(off.glsl.find("heRvtSample") == std::string::npos);
		CHECK(on.glsl.find("heRvtSample") != std::string::npos);
		CHECK(on.glsl.size() < off.glsl.size());        // the live program is the smaller one
		CHECK(off.textureArrayMask != 0);

		// The material still names everything it is made of (the instance panel, the GI and the asset scan read these):
		// every parameter of the whole material, plus the two volume vectors the engine fills for the Read node.
		for (const HE::MatParamSlot& p : off.params)
		{
			bool found = false;
			for (const HE::MatParamSlot& q : on.params) found = found || q.name == p.name;
			CHECK_MESSAGE(found, "parameter missing with RVT on: ", p.name);
		}
		CHECK(on.params.size() == off.params.size() + 2);
		auto has = [&](const std::string& n) { for (const HE::MatParamSlot& q : on.params) if (q.name == n) return true; return false; };
		CHECK(has(HE::matRvtVolumeParamName(HE::kAutoLandscapeRvtName, 'A')));
		CHECK(has(HE::matRvtVolumeParamName(HE::kAutoLandscapeRvtName, 'B')));
		std::vector<std::string> a = on.textures, b = off.textures;
		std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
		CHECK(a == b);
		CHECK(on.textureArrayMask != 0);                // declared, never sampled by the live text

		// The consumer the page driver draws is the live graph alone: no array, only the trail clipmap, and the shader is
		// the live text.
		HE::matSetRvtEnabled(true);
		const HE::MatRvtSplit sp = HE::matSplitRvt(g.graph);
		const HE::MatShaderGen live = HE::generateFragment(sp.live, fn);
		HE::matSetRvtEnabled(was);
		CHECK(live.textureArrayMask == 0);
		REQUIRE(live.textures.size() == 1);
		CHECK(live.textures[0] == HE::kTrailTexturePath);
		CHECK(live.glsl.find("heRvtSample") != std::string::npos);
		CHECK(live.glsl.size() <= on.glsl.size() + 64);

		// What the live part saves per pixel against the whole material, counted in the generated text: texture reads
		// (every array tap is a textureGrad on a bombed hex) and GLSL size.
		auto count = [](const std::string& s, const char* needle) {
			size_t n = 0;
			for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
			return n;
		};
		const size_t tapsWhole = count(off.glsl, "textureGrad(") + count(off.glsl, "texture(") + count(off.glsl, "textureLod(");
		const size_t tapsLive  = count(live.glsl, "textureGrad(") + count(live.glsl, "texture(") + count(live.glsl, "textureLod(");
		MESSAGE(v.name, ": texture reads whole material ", tapsWhole, ", live part ", tapsLive, "; GLSL bytes ", off.glsl.size(), " vs ", live.glsl.size());
		CHECK(tapsLive < tapsWhole);
	}
}

TEST_CASE("RVT auto landscape: the debug views and the displacement variant keep the whole graph in one program")
{
	const HE::AutoLandscapeView views[] = { HE::AutoLandscapeView::MasksRockSnowWater, HE::AutoLandscapeView::MasksDirtWet,
	                                        HE::AutoLandscapeView::Normal, HE::AutoLandscapeView::Surface,
	                                        HE::AutoLandscapeView::BaseColor };
	for (const Variant& v : kVariants)
	{
		CAPTURE(v.name);
		for (HE::AutoLandscapeView view : views)
		{
			const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph(view, v.group, v.mixed);
			CHECK(g.rvtWrite == 0);
			CHECK(g.rvtRead == 0);
			CHECK_FALSE(HE::matSplitRvt(g.graph).hasRvtNodes);
			CHECK_FALSE(HE::generateFragment(g.graph, HE::weatherFunctionLoader()).glsl.empty());
		}
		const HE::AutoLandscapeGraph d = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, v.group, v.mixed, true);
		CHECK(d.rvtWrite == 0);
		CHECK_FALSE(HE::matSplitRvt(d.graph).hasRvtNodes);
	}
}

TEST_CASE("RVT auto landscape: the page painter's three layer views are cut from the Write pages and compile")
{
	for (const Variant& v : kVariants)
	{
		CAPTURE(v.name);
		const HE::AutoLandscapeGraph g = HE::buildAutoLandscapeGraph(HE::AutoLandscapeView::Lit, v.group, v.mixed);
		const HE::MatRvtSplit s = HE::matSplitRvt(g.graph);
		const HE::MatRvtLayerViews lv = HE::matRvtLayerViews(s, HE::kAutoLandscapeRvtName);
		REQUIRE(lv.ok);
		CHECK(lv.pins[0] == (1u << HE::kRvtWriteBaseColorPin));
		CHECK(lv.pins[1] == (1u << HE::kRvtWriteNormalPin));
		CHECK(lv.pins[2] == ((1u << HE::kRvtWriteRoughnessPin) | (1u << HE::kRvtWriteAOPin)));
		for (int k = 0; k < 3; ++k)
		{
			CAPTURE(k);
			const HE::MaterialGraph& vg = lv.view[k];
			CHECK_FALSE(anyNode(vg, T::RvtRead));
			CHECK_FALSE(anyNode(vg, T::RvtWrite));
			CHECK_FALSE(anyNode(vg, T::Weather));            // a page never reads the weather
			CHECK_FALSE(anyNode(vg, T::FunctionCall));       // snow, sand and puddles are live
			CHECK(anyArrayTap(vg));                          // the layer arrays are read here, and only here
			const HE::MatShaderGen gen = HE::generateFragment(vg, HE::weatherFunctionLoader());
			CHECK_FALSE(gen.glsl.empty());
			CHECK(gen.glsl.find("vec3(1.0, 0.0, 1.0)") == std::string::npos);   // no missing-output magenta
		}
		// A page change moves exactly the layers that read it: a colour-only parameter is not in the AO/roughness view's pages.
		HE::MatRvtEnv a, b;
		set(b, HE::kAutoLandscapeParamRockSlope, 0.9f);
		const HE::MatRvtMaterialState sa = stateOf(g, s, a), sb = stateOf(g, s, b);
		size_t differing = 0;
		for (size_t i = 0; i < sa.pageHashes.size(); ++i) differing += sa.pageHashes[i] != sb.pageHashes[i];
		CHECK(differing >= 1);
	}
}
