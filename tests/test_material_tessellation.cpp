// Displacement pin + GPU tessellation (Thema 207, Schritt 2): the graph side
// (pin 9, the "tess" settings, scope "ds"), the MTRL tail, and the shader
// library's control/eval stages. Design:
// docs/material-graph-tessellation-displacement-entwurf-2026-10-15.md
#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/MaterialGraph.h>
#if defined(HE_TESTS_HAVE_SHADERC)
#include <material/MaterialShaderLibrary.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

using HE::MaterialGraph;
using HE::MatNodeType;
namespace fs = std::filesystem;

namespace
{
int outputOf(const MaterialGraph& g)
{
	for (const auto& n : g.nodes)
		if (n.type == MatNodeType::Output) return n.id;
	return 0;
}

// The default material with a constant height on the Displacement pin.
MaterialGraph withConstDisplacement(float h)
{
	MaterialGraph g = MaterialGraph::makeDefault();
	const int c = g.addNode(MatNodeType::ConstFloat);
	g.findNode(c)->p[0] = h;
	REQUIRE(g.connect(c, 0, outputOf(g), HE::kMatOutputDisplacementPin));
	return g;
}

// Everything the domain stage would run (declarations + statements).
std::string dispPart(const HE::MatShaderGen& gen)
{
	HE::MatTessSettings t;
	std::string decls, stmts;
	REQUIRE(HE::matSplitDisplacement(gen.displacementBody, t, decls, stmts));
	return decls + stmts;
}
} // namespace

TEST_CASE("Displacement is Output pin 9, appended after World Position Offset")
{
	const HE::MatNodeDesc& d = HE::matNodeDesc(MatNodeType::Output);
	REQUIRE(d.inputs.size() == 10);
	CHECK(HE::kMatOutputDisplacementPin == 9);
	CHECK(std::string(d.inputs[HE::kMatOutputDisplacementPin].name) == "Displacement");
	CHECK(d.inputs[HE::kMatOutputDisplacementPin].type == HE::MatPinType::Float);
	CHECK(std::string(d.inputs[HE::kMatOutputWPOPin].name) == "World Position Offset");
	// Shown in every blend mode (it does not depend on how the colour is blended).
	for (int mode = 0; mode < 3; ++mode)
	{
		std::vector<HE::MatPinDesc> pins; std::vector<int> reg;
		HE::matOutputPins(mode, pins, reg);
		REQUIRE_FALSE(reg.empty());
		CHECK(reg.back() == HE::kMatOutputDisplacementPin);
	}
}

TEST_CASE("Unconnected Displacement: no body, and the settings change nothing else")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	const HE::MatShaderGen a = HE::generateFragment(g);
	CHECK(a.displacementBody.empty());

	// Settings without a connected pin are inert: same shaders, same everything.
	g.findNode(outputOf(g))->tess.maxFactor = 32.0f;
	const HE::MatShaderGen b = HE::generateFragment(g);
	CHECK(b.displacementBody.empty());
	CHECK(b.glsl == a.glsl);
	CHECK(b.glslGBuffer == a.glslGBuffer);
	CHECK(b.vertexBody == a.vertexBody);
}

TEST_CASE("Connected Displacement: settings line, statements, fragment untouched")
{
	const MaterialGraph plain = MaterialGraph::makeDefault();
	const HE::MatShaderGen ref = HE::generateFragment(plain);
	const HE::MatShaderGen gen = HE::generateFragment(withConstDisplacement(0.05f));
	REQUIRE_FALSE(gen.displacementBody.empty());
	// A constant height reads nothing the fragment declares: the fragment and the
	// G-buffer variant stay byte-identical, so their pipelines are shared.
	CHECK(gen.glsl == ref.glsl);
	CHECK(gen.glslGBuffer == ref.glslGBuffer);
	CHECK(gen.vertexBody.empty());

	CHECK(gen.displacementBody.rfind("// heTess 8.000000 5.000000 40.000000 0.100000\n", 0) == 0);
	HE::MatTessSettings t;
	std::string decls, stmts;
	REQUIRE(HE::matSplitDisplacement(gen.displacementBody, t, decls, stmts));
	CHECK(t == HE::MatTessSettings{});
	CHECK(stmts.find("float heDisp = ") != std::string::npos);
	CHECK(stmts.find("0.050000") != std::string::npos);
	// The statements left the fragment.
	CHECK(gen.glsl.find("0.050000") == std::string::npos);

	// Text that is not a displacement body is refused, not half-parsed.
	CHECK_FALSE(HE::matSplitDisplacement("", t, decls, stmts));
	CHECK_FALSE(HE::matSplitDisplacement("vec3 heWpo = vec3(0.0);\n", t, decls, stmts));
	CHECK_FALSE(HE::matSplitDisplacement("// heTess 8 5 40\n// heDispBody\nfloat heDisp = 0.0;\n", t, decls, stmts));
}

TEST_CASE("Tessellation settings: sanitised into the text, a change is new text")
{
	MaterialGraph g = withConstDisplacement(0.05f);
	HE::MatTessSettings& s = g.findNode(outputOf(g))->tess;
	s.maxFactor = 1000.0f; s.fadeNear = 30.0f; s.fadeFar = 10.0f; s.maxDisplacement = -1.0f;
	const HE::MatShaderGen gen = HE::generateFragment(g);
	HE::MatTessSettings t;
	std::string decls, stmts;
	REQUIRE(HE::matSplitDisplacement(gen.displacementBody, t, decls, stmts));
	CHECK(t.maxFactor == doctest::Approx(HE::kMatTessMaxFactor));
	CHECK(t.fadeNear == doctest::Approx(30.0f));
	CHECK(t.fadeFar > t.fadeNear);
	CHECK(t.maxDisplacement == doctest::Approx(0.0f));

	s = HE::MatTessSettings{};
	const std::string before = HE::generateFragment(g).displacementBody;
	s.maxDisplacement = 0.25f;
	CHECK(HE::generateFragment(g).displacementBody != before);
}

TEST_CASE("Tessellation settings travel in the Output node's JSON, defaults write nothing")
{
	MaterialGraph g = withConstDisplacement(0.05f);
	const std::string plainJson = HE::materialGraphToJson(g);
	CHECK(plainJson.find("\"tess\"") == std::string::npos); // old graphs keep their exact JSON

	HE::MatGraphNode& out = *g.findNode(outputOf(g));
	out.tess.maxFactor = 16.0f; out.tess.fadeNear = 2.0f; out.tess.fadeFar = 20.0f;
	out.tess.maxDisplacement = 0.3f;
	const std::string json = HE::materialGraphToJson(g);
	CHECK(json.find("\"tess\"") != std::string::npos);
	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(json, r));
	CHECK(r.findNode(outputOf(r))->tess == out.tess);
	CHECK(HE::generateFragment(r).displacementBody == HE::generateFragment(g).displacementBody);

	// The item form collaboration sends is the same.
	HE::MatGraphNode n;
	REQUIRE(HE::matNodeFromJson(HE::matNodeToJson(out), n));
	CHECK(n.tess == out.tess);

	// A malformed object keeps the defaults field by field.
	HE::MatGraphNode m;
	REQUIRE(HE::matNodeFromJson(R"({"id":1,"type":"Output","tess":{"max":"x","near":3}})", m));
	CHECK(m.tess.maxFactor == doctest::Approx(HE::MatTessSettings{}.maxFactor));
	CHECK(m.tess.fadeNear == doctest::Approx(3.0f));
}

TEST_CASE("Scope ds: no derivatives, explicit mip, shared texture slots")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	const int out = outputOf(g);
	// One height texture, read by the fragment (base colour) AND the displacement.
	const int tex = g.addNode(MatNodeType::TextureSample);
	g.findNode(tex)->s = "Textures/Height.hasset";
	REQUIRE(g.connect(tex, 0, out, HE::kMatOutputBaseColorPin));
	const int split = g.addNode(MatNodeType::SplitRGBA);
	REQUIRE(g.connect(tex, 0, split, 0));
	// + a normal map and a screen position, which have no meaning there.
	const int nm = g.addNode(MatNodeType::NormalMapSample);
	REQUIRE(g.connect(nm, 0, out, HE::kMatOutputNormalPin));
	const int add = g.addNode(MatNodeType::Add);
	REQUIRE(g.connect(split, 0, add, 0));
	const int dot = g.addNode(MatNodeType::DotProduct);
	REQUIRE(g.connect(nm, 0, dot, 0));
	REQUIRE(g.connect(nm, 0, dot, 1));
	REQUIRE(g.connect(dot, 0, add, 1));
	const int sp = g.addNode(MatNodeType::ScreenPos);
	const int sps = g.addNode(MatNodeType::SplitRGBA);
	REQUIRE(g.connect(sp, 0, sps, 0));
	const int add2 = g.addNode(MatNodeType::Add);
	REQUIRE(g.connect(add, 0, add2, 0));
	REQUIRE(g.connect(sps, 0, add2, 1));
	REQUIRE(g.connect(add2, 0, out, HE::kMatOutputDisplacementPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.displacementBody.empty());
	const std::string ds = dispPart(gen);
	CAPTURE(ds);
	CHECK(gen.textures.size() == 1);                    // one slot for both stages
	CHECK(ds.find("textureLod(heTexP0") != std::string::npos);
	CHECK(ds.find("uniform sampler2D heTexP0") != std::string::npos);
	CHECK(ds.find("texture(heTexP0") == std::string::npos);
	CHECK(ds.find("dFdx") == std::string::npos);
	CHECK(ds.find("dFdy") == std::string::npos);
	CHECK(ds.find("gl_FragCoord") == std::string::npos);
	CHECK(ds.find("hePerturbNormal") == std::string::npos);
	// The fragment keeps its own forms.
	CHECK(gen.glsl.find("texture(heTexP0") != std::string::npos);
	CHECK(gen.glsl.find("hePerturbNormal(") != std::string::npos);
}

TEST_CASE("Scope ds: texture bombing reads mip 0 through zero gradients")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	const int bomb = g.addNode(MatNodeType::TextureBombSample);
	g.findNode(bomb)->s = "Textures/Height.hasset";
	const int split = g.addNode(MatNodeType::SplitRGBA);
	REQUIRE(g.connect(bomb, 0, split, 0));
	REQUIRE(g.connect(split, 0, outputOf(g), HE::kMatOutputDisplacementPin));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	const std::string ds = dispPart(gen);
	CAPTURE(ds);
	CHECK(ds.find("heBombGrid(") != std::string::npos);
	CHECK(ds.find("void heBombGrid(") != std::string::npos); // helper declared in the stage
	CHECK(ds.find("textureGrad(") != std::string::npos);
	CHECK(ds.find("dFdx") == std::string::npos);
}

TEST_CASE("A UI material ignores the Displacement pin")
{
	MaterialGraph g = withConstDisplacement(0.05f);
	g.findNode(outputOf(g))->p[3] = static_cast<float>(HE::MatDomain::UserInterface);
	CHECK(HE::generateFragment(g).displacementBody.empty());
}

// The Material Editor animates its preview only for text that reads Time. A
// height driven by Time lives in the displacement body alone, so the editor
// asks that body too (MaterialEditorPanel, usesTime) — this is what it relies on.
TEST_CASE("A Time-driven height is found in the displacement body, not the fragment")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	const int time = g.addNode(MatNodeType::Time);
	const int sine = g.addNode(MatNodeType::Sine);
	REQUIRE(g.connect(time, 0, sine, 0));
	REQUIRE(g.connect(sine, 0, outputOf(g), HE::kMatOutputDisplacementPin));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.displacementBody.empty());
	CHECK(HE::matGlslUsesTime(gen.displacementBody));
	CHECK_FALSE(HE::matGlslUsesTime(gen.glsl));
}

TEST_CASE("MTRL tail: the displacement body survives save and load")
{
	const fs::path root = fs::temp_directory_path() / "he_test_tessellation" / "Content";
	he_test::removeAllQuiet(root.parent_path());
	fs::create_directories(root);
	const HE::MatShaderGen gen = HE::generateFragment(withConstDisplacement(0.05f));
	REQUIRE_FALSE(gen.displacementBody.empty());
	{
		ContentManager cm(root.string());
		// No graph, like a packaged material: what loads is what was written.
		MaterialAsset m;
		m.type = HE::AssetType::Material;
		m.name = "Packed"; m.path = "Packed.hasset";
		m.customShaderFragGlsl = gen.glsl;
		m.customShaderDispGlsl = gen.displacementBody;
		REQUIRE(cm.saveAsset(m));
		// With a graph: regenerated at load, from the graph.
		MaterialAsset w;
		w.type = HE::AssetType::Material;
		w.name = "Graph"; w.path = "Graph.hasset";
		w.nodeGraphJson = HE::materialGraphToJson(withConstDisplacement(0.07f));
		REQUIRE(cm.saveAsset(w));
		// Without displacement the field stays empty.
		MaterialAsset e;
		e.type = HE::AssetType::Material;
		e.name = "Plain"; e.path = "Plain.hasset";
		e.customShaderFragGlsl = gen.glsl;
		REQUIRE(cm.saveAsset(e));
	}
	ContentManager fresh(root.string());
	const MaterialAsset* packed = fresh.getMaterial(fresh.loadAsset("Packed.hasset"));
	REQUIRE(packed);
	CHECK(packed->customShaderDispGlsl == gen.displacementBody);
	const MaterialAsset* graph = fresh.getMaterial(fresh.loadAsset("Graph.hasset"));
	REQUIRE(graph);
	CHECK(graph->customShaderDispGlsl.find("0.070000") != std::string::npos);
	const MaterialAsset* plain = fresh.getMaterial(fresh.loadAsset("Plain.hasset"));
	REQUIRE(plain);
	CHECK(plain->customShaderDispGlsl.empty());
	he_test::removeAllQuiet(root.parent_path());
}

#if defined(HE_TESTS_HAVE_SHADERC)
namespace
{
// Graphs whose displacement body exercises the "ds" rules and the helpers the
// stage has to declare itself.
std::vector<std::pair<std::string, MaterialGraph>> displacementCases()
{
	std::vector<std::pair<std::string, MaterialGraph>> cases;
	cases.emplace_back("constant", withConstDisplacement(0.05f));
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		const int tex = g.addNode(MatNodeType::TextureSample);
		g.findNode(tex)->s = "Textures/Height.hasset";
		const int split = g.addNode(MatNodeType::SplitRGBA);
		REQUIRE(g.connect(tex, 0, split, 0));
		const int nm = g.addNode(MatNodeType::NormalMapSample);
		REQUIRE(g.connect(nm, 0, outputOf(g), HE::kMatOutputNormalPin));
		REQUIRE(g.connect(split, 0, outputOf(g), HE::kMatOutputDisplacementPin));
		cases.emplace_back("texture + fragment normal map", g);
	}
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		const int bomb = g.addNode(MatNodeType::TextureBombSample);
		g.findNode(bomb)->s = "Textures/Height.hasset";
		const int split = g.addNode(MatNodeType::SplitRGBA);
		REQUIRE(g.connect(bomb, 0, split, 0));
		REQUIRE(g.connect(split, 0, outputOf(g), HE::kMatOutputDisplacementPin));
		cases.emplace_back("texture bombing", g);
	}
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		const int noise = g.addNode(MatNodeType::ValueNoise);
		const int time = g.addNode(MatNodeType::Time);
		const int prm = g.addNode(MatNodeType::ParamFloat);
		g.findNode(prm)->s = "Height";
		g.findNode(prm)->p[0] = 0.1f;
		const int mul = g.addNode(MatNodeType::Multiply);
		REQUIRE(g.connect(noise, 0, mul, 0));
		REQUIRE(g.connect(prm, 0, mul, 1));
		const int add = g.addNode(MatNodeType::Add);
		REQUIRE(g.connect(mul, 0, add, 0));
		REQUIRE(g.connect(time, 0, add, 1));
		REQUIRE(g.connect(add, 0, outputOf(g), HE::kMatOutputDisplacementPin));
		cases.emplace_back("noise * param + time", g);
	}
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		const int rip = g.addNode(MatNodeType::WaterRipple);
		REQUIRE(g.connect(rip, 0, outputOf(g), HE::kMatOutputDisplacementPin));
		cases.emplace_back("water ripple height", g);
	}
	return cases;
}
} // namespace

TEST_CASE("Tessellation stages cross-compile on every tessellating target")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	for (const auto& [name, g] : displacementCases())
	{
		CAPTURE(name);
		const HE::MatShaderGen gen = HE::generateFragment(g);
		REQUIRE_FALSE(gen.displacementBody.empty());
		const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
		for (B b : { B::GLSL410, B::GLSL430, B::SpirV })
		{
			CAPTURE((int)b);
			const auto& tc = lib.tessControl(h, gen.displacementBody, b);
			const auto& te = lib.tessEval(h, gen.displacementBody, b);
			CHECK_MESSAGE(tc.ok, tc.log);
			CHECK_MESSAGE(te.ok, te.log);
			if (b == B::SpirV) { CHECK_FALSE(te.spirv.empty()); }
			else               { CHECK(te.source.find("gl_TessCoord") != std::string::npos); }
		}
		// D3D: a hull shell, and the kernel + domain shell in one unit.
		const auto& hs = lib.tessControl(h, gen.displacementBody, B::HLSL);
		const auto& ds = lib.tessEval(h, gen.displacementBody, B::HLSL);
		REQUIRE_MESSAGE(hs.ok, hs.log);
		REQUIRE_MESSAGE(ds.ok, ds.log);
		CHECK(hs.source.find(std::string(HE::MaterialShaderLibrary::kHlslHullEntry) + "(") != std::string::npos);
		CHECK(hs.source.find("partitioning(\"fractional_odd\")") != std::string::npos);
		CHECK(ds.source.find("void heDispKernel_inner()") != std::string::npos);
		CHECK(ds.source.find(std::string(HE::MaterialShaderLibrary::kHlslDomainEntry) + "(") != std::string::npos);
		// The shell writes the kernel's statics by these names.
		for (const char* g2 : { "static float3 heInNormal", "static float4 heInClip", "static float3 vWorldPos",
		                        "static float4 gl_Position" })
			CHECK_MESSAGE(ds.source.find(g2) != std::string::npos, g2);
	}
	// No tessellation on Metal / GLES, and no body → not ok (the fallback).
	const HE::MatShaderGen gen = HE::generateFragment(withConstDisplacement(0.05f));
	const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
	CHECK_FALSE(lib.tessEval(h, gen.displacementBody, B::Metal).ok);
	CHECK_FALSE(lib.tessControl(h, gen.displacementBody, B::GLSLES300).ok);
	CHECK_FALSE(lib.tessEval(1, "float heDisp = 0.0;\n", B::GLSL410).ok);
}

TEST_CASE("GL tessellation stages declare the fragment's HeLighting block")
{
	// One GL program, so one HeLighting declaration in every stage (see the WPO
	// test "GL links a WPO material"). The control stage reads camPos from it.
	using B = HE::MaterialShaderLibrary::Backend;
	const HE::MatShaderGen gen = HE::generateFragment(displacementCases()[3].second); // reads Time
	HE::MaterialShaderLibrary lib;
	auto block = [](const std::string& src) -> std::string {
		const size_t b = src.find("uniform HeLighting");
		if (b == std::string::npos) return {};
		const size_t e = src.find("heLight;", b);
		return e == std::string::npos ? std::string() : src.substr(b, e - b);
	};
	const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
	const auto& fs = lib.fragment(std::hash<std::string>{}(gen.glsl), gen.glsl, B::GLSL410);
	const auto& tc = lib.tessControl(h, gen.displacementBody, B::GLSL410);
	const auto& te = lib.tessEval(h, gen.displacementBody, B::GLSL410);
	REQUIRE_MESSAGE(fs.ok, fs.log);
	REQUIRE_MESSAGE(tc.ok, tc.log);
	REQUIRE_MESSAGE(te.ok, te.log);
	const std::string fb = block(fs.source);
	REQUIRE_FALSE(fb.empty());
	CHECK(block(tc.source) == fb);
	CHECK(block(te.source) == fb);
	// ccw = the input winding in GL's domain (measured on macOS GL 4.1 with culling).
	CHECK(te.source.find("ccw") != std::string::npos);

	// HE_TESS_DUMP_DIR: write a whole GLSL 4.10 program (vertex, control, eval,
	// fragment) for a driver-side link/draw check outside the editor
	// (scripts/tess_gl_harness.c).
	if (const char* dir = std::getenv("HE_TESS_DUMP_DIR"))
	{
		const auto& vs = lib.standardVertex(B::GLSL410);
		REQUIRE_MESSAGE(vs.ok, vs.log);
		const std::pair<const char*, const std::string*> files[] = {
			{ "material.vert", &vs.source }, { "material.tesc", &tc.source },
			{ "material.tese", &te.source }, { "material.frag", &fs.source } };
		for (const auto& [file, src] : files)
			std::ofstream(fs::path(dir) / file, std::ios::binary) << *src;
		// The D3D units too, for an offline HLSL check (glslangValidator -D, entry
		// HSMain -S tesc / DSMain -S tese): there is no FXC on a Mac.
		const auto& hs = lib.tessControl(h, gen.displacementBody, B::HLSL);
		const auto& ds = lib.tessEval(h, gen.displacementBody, B::HLSL);
		REQUIRE_MESSAGE(hs.ok, hs.log);
		REQUIRE_MESSAGE(ds.ok, ds.log);
		std::ofstream(fs::path(dir) / "material.hs.hlsl", std::ios::binary) << hs.source;
		std::ofstream(fs::path(dir) / "material.ds.hlsl", std::ios::binary) << ds.source;
	}
}
#endif // HE_TESTS_HAVE_SHADERC
