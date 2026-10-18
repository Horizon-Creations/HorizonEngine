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
#include <Backends/Vulkan/VulkanMaterialLayout.h> // the renderer's material set 0
#include <spirv_cross.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <Backends/D3D12/D3D12MaterialRootSignature.h>
#endif
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

// ── Step 4: what the Vulkan / D3D11 / D3D12 renderers build from the stages ──

namespace
{
// Statically used set/binding/kind of one SPIR-V stage (kind = HE::vkmat::DescKind,
// -1 for a kind the material set never has).
struct TessVkUse { uint32_t set, binding; int kind; };
std::vector<TessVkUse> activeTessDescriptors(const std::vector<uint32_t>& spirv)
{
	std::vector<TessVkUse> out;
	spirv_cross::Compiler comp(spirv);
	const spirv_cross::ShaderResources r = comp.get_shader_resources(comp.get_active_interface_variables());
	auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, int kind) {
		for (const spirv_cross::Resource& res : list)
			out.push_back({ comp.get_decoration(res.id, spv::DecorationDescriptorSet),
			                comp.get_decoration(res.id, spv::DecorationBinding), kind });
	};
	using K = HE::vkmat::DescKind;
	add(r.uniform_buffers, static_cast<int>(K::UniformBuffer));
	add(r.sampled_images,  static_cast<int>(K::CombinedImageSampler));
	add(r.storage_buffers, static_cast<int>(K::StorageBuffer));
	add(r.separate_images, -1);
	add(r.separate_samplers, -1);
	return out;
}

// The uses no row of the material table serves for `stage` (same set 0, binding,
// kind, and the stage bit; `strip` masks bits off every row first) — empty when all bind.
std::string uncoveredForStage(const std::vector<TessVkUse>& uses, uint32_t stage, uint32_t strip = 0)
{
	std::string miss;
	for (const TessVkUse& u : uses)
	{
		bool ok = false;
		for (const HE::vkmat::Binding& b : HE::vkmat::kBindings)
			ok = ok || (u.set == 0 && b.binding == u.binding && static_cast<int>(b.kind) == u.kind
			            && ((b.stages & ~strip) & stage));
		if (!ok) miss += " " + std::to_string(u.binding) + ":" + std::to_string(u.kind);
	}
	return miss;
}
} // namespace

TEST_CASE("Vulkan: the tessellation stages bind only set 0 rows that carry their stage bit")
{
	// VulkanRenderer builds m_matSetLayout from HE::vkmat::kBindings; a tessellation
	// stage that statically uses a binding whose row lacks its stage bit makes the
	// four-stage pipeline invalid. CI has no Vulkan device, so the reflection is the
	// witness, with the pre-Thema-207 table (tess bits stripped) as negative control.
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	bool sawTexture = false;
	for (const auto& [name, g] : displacementCases())
	{
		CAPTURE(name);
		const HE::MatShaderGen gen = HE::generateFragment(g);
		const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
		const auto& tc = lib.tessControl(h, gen.displacementBody, B::SpirV);
		const auto& te = lib.tessEval(h, gen.displacementBody, B::SpirV);
		REQUIRE_MESSAGE(tc.ok, tc.log);
		REQUIRE_MESSAGE(te.ok, te.log);
		const std::vector<TessVkUse> uc = activeTessDescriptors(tc.spirv);
		const std::vector<TessVkUse> ue = activeTessDescriptors(te.spirv);
		CHECK_FALSE(uc.empty()); // camPos: HeLighting
		CHECK_FALSE(ue.empty()); // U at least
		CHECK_MESSAGE(uncoveredForStage(uc, HE::vkmat::kStageTessControl).empty(),
		              "control stage uncovered:", uncoveredForStage(uc, HE::vkmat::kStageTessControl));
		CHECK_MESSAGE(uncoveredForStage(ue, HE::vkmat::kStageTessEval).empty(),
		              "evaluation stage uncovered:", uncoveredForStage(ue, HE::vkmat::kStageTessEval));
		CHECK_FALSE(uncoveredForStage(ue, HE::vkmat::kStageTessEval, HE::vkmat::kStageTess).empty());
		for (const TessVkUse& u : ue)
			sawTexture = sawTexture || u.kind == static_cast<int>(HE::vkmat::DescKind::CombinedImageSampler);

		// Vulkan's default domain origin is upper left: the input winding is `cw` there
		// (GL's `ccw`, the test above).
		spirv_cross::Compiler comp(te.spirv);
		CHECK(comp.get_execution_mode_bitset().get(spv::ExecutionModeVertexOrderCw));
		CHECK_FALSE(comp.get_execution_mode_bitset().get(spv::ExecutionModeVertexOrderCcw));
	}
	CHECK(sawTexture); // the texture case really samples in the evaluation stage
}

#if defined(_WIN32)
namespace
{
using Microsoft::WRL::ComPtr;

ComPtr<ID3DBlob> tessFxc(const std::string& src, const char* entry, const char* profile, std::string& err)
{
	ComPtr<ID3DBlob> code, msg;
	err.clear();
	if (FAILED(D3DCompile(src.c_str(), src.size(), "tess", nullptr, nullptr, entry, profile, 0, 0, &code, &msg)))
	{
		err = msg ? std::string(static_cast<const char*>(msg->GetBufferPointer()), msg->GetBufferSize()) : "?";
		code.Reset();
	}
	return code;
}

const D3D12_INPUT_ELEMENT_DESC kTessIl12[] = {
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
const D3D11_INPUT_ELEMENT_DESC kTessIl11[] = {
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
} // namespace

TEST_CASE("D3D12 on WARP: hull + domain shells pass FXC and build the four-stage material PSO")
{
	// What D3D12Renderer::GetOrBuildMaterialPSO builds for a Displacement material:
	// VS (standard) → HS → DS → PS (the material fragment), PATCH topology, against
	// the material root signature. PSO creation holds every stage's registers against
	// the signature AND the stages' signatures against each other, so a mismatch in
	// the hand-written control point (HeTessCP) shows here, not first on a GPU.
	using LB = HE::MaterialShaderLibrary::Backend;
	namespace m12 = HE::d3d12mat;
	HE::MaterialShaderLibrary lib;
	std::string err;
	REQUIRE(tessFxc("float4 main() : SV_Position { return nonsense; }", "main", "ds_5_0", err).Get() == nullptr);

	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> warp;
	ComPtr<ID3D12Device> dev;
	REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
	REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
	REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));
	auto rootSig = [&](const m12::MaterialRootSignature& d) {
		ComPtr<ID3DBlob> sig, e;
		ComPtr<ID3D12RootSignature> rs;
		if (SUCCEEDED(D3D12SerializeRootSignature(&d.desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &e)))
			dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs));
		return rs;
	};
	m12::MaterialRootSignature full, pixelOnly;
	m12::DescribeMaterialRootSignature(full);
	m12::DescribeMaterialRootSignature(pixelOnly, m12::kRangeCount, m12::kSamplerCount, m12::kParamCount,
	                                   D3D12_SHADER_VISIBILITY_PIXEL);
	ComPtr<ID3D12RootSignature> fullRs = rootSig(full), pixelRs = rootSig(pixelOnly);
	REQUIRE(fullRs.Get() != nullptr);
	REQUIRE(pixelRs.Get() != nullptr);

	ComPtr<ID3DBlob> vs = tessFxc(lib.standardVertex(LB::HLSL).source, "main", "vs_5_0", err);
	REQUIRE_MESSAGE(vs.Get() != nullptr, "standard vertex: ", err);

	bool texturePixelOnlyRefused = false;
	for (const auto& [name, g] : displacementCases())
	{
		CAPTURE(name);
		const HE::MatShaderGen gen = HE::generateFragment(g);
		const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
		const auto& hc = lib.tessControl(h, gen.displacementBody, LB::HLSL);
		const auto& dc = lib.tessEval(h, gen.displacementBody, LB::HLSL);
		REQUIRE_MESSAGE(hc.ok, hc.log);
		REQUIRE_MESSAGE(dc.ok, dc.log);
		ComPtr<ID3DBlob> hs = tessFxc(hc.source, HE::MaterialShaderLibrary::kHlslHullEntry, "hs_5_0", err);
		REQUIRE_MESSAGE(hs.Get() != nullptr, "hs_5_0: ", err);
		ComPtr<ID3DBlob> ds = tessFxc(dc.source, HE::MaterialShaderLibrary::kHlslDomainEntry, "ds_5_0", err);
		REQUIRE_MESSAGE(ds.Get() != nullptr, "ds_5_0: ", err);
		const auto& fc = lib.fragment(std::hash<std::string>{}(gen.glsl), gen.glsl, LB::HLSL);
		REQUIRE_MESSAGE(fc.ok, fc.log);
		ComPtr<ID3DBlob> ps = tessFxc(fc.source, "main", "ps_5_0", err);
		REQUIRE_MESSAGE(ps.Get() != nullptr, "ps_5_0: ", err);

		auto pso = [&](ID3D12RootSignature* rs, bool withTess, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo) -> HRESULT {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = rs;
			pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			if (withTess)
			{
				pd.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
				pd.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
			}
			pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pd.InputLayout           = { kTessIl12, 3 };
			pd.PrimitiveTopologyType = topo;
			pd.NumRenderTargets      = 1;
			pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
			pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
			pd.SampleDesc.Count      = 1;
			pd.SampleMask            = UINT_MAX;
			pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.DepthStencilState.DepthEnable    = TRUE;
			pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
			pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			ComPtr<ID3D12PipelineState> p;
			return dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p));
		};
		CHECK_MESSAGE(SUCCEEDED(pso(fullRs.Get(), true, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)),
		              "four-stage material PSO refused on WARP");
		CHECK(SUCCEEDED(pso(fullRs.Get(), false, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE))); // the fallback
		// Negative control: the device does judge the stages (HS/DS need PATCH).
		CHECK(FAILED(pso(fullRs.Get(), true, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)));
		// The pre-Thema-207 signature hid the SRV table from the domain shader.
		if (gen.displacementBody.find("textureLod") != std::string::npos)
			texturePixelOnlyRefused = texturePixelOnlyRefused
			                       || FAILED(pso(pixelRs.Get(), true, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH));
	}
	CHECK(texturePixelOnlyRefused);

	// D3D11 creates the same bytecode as hull/domain shader objects (WARP).
	ComPtr<ID3D11Device> d11;
	REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
	                                    D3D11_SDK_VERSION, &d11, nullptr, nullptr)));
	const HE::MatShaderGen gen = HE::generateFragment(displacementCases()[1].second); // texture
	const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
	ComPtr<ID3DBlob> hs = tessFxc(lib.tessControl(h, gen.displacementBody, LB::HLSL).source,
	                              HE::MaterialShaderLibrary::kHlslHullEntry, "hs_5_0", err);
	ComPtr<ID3DBlob> ds = tessFxc(lib.tessEval(h, gen.displacementBody, LB::HLSL).source,
	                              HE::MaterialShaderLibrary::kHlslDomainEntry, "ds_5_0", err);
	REQUIRE(hs.Get() != nullptr);
	REQUIRE(ds.Get() != nullptr);
	ComPtr<ID3D11HullShader> h11;
	ComPtr<ID3D11DomainShader> d11s;
	CHECK(SUCCEEDED(d11->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr, &h11)));
	CHECK(SUCCEEDED(d11->CreateDomainShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, &d11s)));
}

TEST_CASE("D3D11 on WARP: a constant height moves the tessellated triangle along its normal")
{
	// The draw D3D11Renderer issues for a Displacement material, minus the lighting:
	// standard VS → hull → domain on 3-control-point patches, U at b1 for the VS and
	// the DS. One triangle in the NDC square (identity mvp/model) whose normals all
	// point along +x: the domain stage must move it right by the height in NDC, and
	// clamp the height to maxDisplacement (0.1, the default). Coverage is read from
	// the depth buffer (no pixel shader: the triangle's x extent is the witness).
	using LB = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	std::string err;
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	D3D_FEATURE_LEVEL fl{};
	REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
	                                    D3D11_SDK_VERSION, &dev, &fl, &ctx)));
	REQUIRE(fl >= D3D_FEATURE_LEVEL_11_0);

	ComPtr<ID3DBlob> vsb = tessFxc(lib.standardVertex(LB::HLSL).source, "main", "vs_5_0", err);
	REQUIRE_MESSAGE(vsb.Get() != nullptr, err);
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11InputLayout> il;
	REQUIRE(SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)));
	REQUIRE(SUCCEEDED(dev->CreateInputLayout(kTessIl11, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il)));

	// pos, normal (+x), uv — the material vertex layout (32 B).
	const float verts[] = {
		-0.5f, -0.5f, 0.5f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f,
		 0.0f,  0.5f, 0.5f,  1.0f, 0.0f, 0.0f,  0.5f, 1.0f,
		 0.5f, -0.5f, 0.5f,  1.0f, 0.0f, 0.0f,  1.0f, 0.0f,
	};
	ComPtr<ID3D11Buffer> vb, ucb, zeroCb;
	{
		D3D11_BUFFER_DESC bd{ sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA sd{ verts, 0, 0 };
		REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, &sd, &vb)));
		float u[44] = {}; // std140 U: mvp, model, color, flags, pbr (176 B)
		for (int i = 0; i < 4; ++i) { u[i * 5] = 1.0f; u[16 + i * 5] = 1.0f; }
		D3D11_BUFFER_DESC cd{ sizeof(u), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA cs{ u, 0, 0 };
		REQUIRE(SUCCEEDED(dev->CreateBuffer(&cd, &cs, &ucb)));
		std::vector<float> zeros(1024, 0.0f); // HeLighting / HeParams: all zero
		D3D11_BUFFER_DESC zd{ static_cast<UINT>(zeros.size() * sizeof(float)), D3D11_USAGE_DEFAULT,
		                      D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA zs{ zeros.data(), 0, 0 };
		REQUIRE(SUCCEEDED(dev->CreateBuffer(&zd, &zs, &zeroCb)));
	}
	constexpr UINT kSize = 64;
	ComPtr<ID3D11Texture2D> depth, staging;
	ComPtr<ID3D11DepthStencilView> dsv;
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = kSize; td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_D32_FLOAT; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depth)));
		REQUIRE(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv)));
		td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging)));
	}
	ComPtr<ID3D11RasterizerState> rast;
	{
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		REQUIRE(SUCCEEDED(dev->CreateRasterizerState(&rd, &rast)));
	}

	// Mean x (pixels) of the covered depth texels after one draw; < 0 = nothing drawn.
	auto centroidX = [&](ID3D11HullShader* hs, ID3D11DomainShader* ds) -> double {
		ctx->ClearState();
		ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		ctx->OMSetRenderTargets(0, nullptr, dsv.Get());
		D3D11_VIEWPORT vp{ 0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f };
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(rast.Get());
		const UINT stride = 32, offset = 0;
		ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
		ctx->IASetInputLayout(il.Get());
		ctx->IASetPrimitiveTopology(hs ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
		                               : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs.Get(), nullptr, 0);
		ctx->VSSetConstantBuffers(1, 1, ucb.GetAddressOf());
		ctx->HSSetShader(hs, nullptr, 0);
		ctx->DSSetShader(ds, nullptr, 0);
		ctx->HSSetConstantBuffers(0, 1, zeroCb.GetAddressOf());
		ctx->DSSetConstantBuffers(0, 1, zeroCb.GetAddressOf());
		ctx->DSSetConstantBuffers(1, 1, ucb.GetAddressOf());
		ctx->DSSetConstantBuffers(3, 1, zeroCb.GetAddressOf());
		ctx->PSSetShader(nullptr, nullptr, 0);
		ctx->Draw(3, 0);
		ctx->CopyResource(staging.Get(), depth.Get());
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return -2.0;
		double sum = 0.0; size_t n = 0;
		for (UINT y = 0; y < kSize; ++y)
		{
			const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
			for (UINT x = 0; x < kSize; ++x)
				if (row[x] < 0.75f) { sum += x + 0.5; ++n; }
		}
		ctx->Unmap(staging.Get(), 0);
		return n ? sum / double(n) : -1.0;
	};

	const double base = centroidX(nullptr, nullptr);
	REQUIRE(base > 0.0);
	CHECK(base == doctest::Approx(kSize / 2.0).epsilon(0.02));
	// Pixels per NDC unit along x: the viewport spans 2 units.
	const double pxPerNdc = kSize / 2.0;
	for (const float height : { 0.0f, 0.08f, 0.3f })
	{
		CAPTURE(height);
		const HE::MatShaderGen gen = HE::generateFragment(withConstDisplacement(height));
		const uint64_t h = std::hash<std::string>{}(gen.displacementBody);
		ComPtr<ID3DBlob> hsb = tessFxc(lib.tessControl(h, gen.displacementBody, LB::HLSL).source,
		                               HE::MaterialShaderLibrary::kHlslHullEntry, "hs_5_0", err);
		REQUIRE_MESSAGE(hsb.Get() != nullptr, err);
		ComPtr<ID3DBlob> dsb = tessFxc(lib.tessEval(h, gen.displacementBody, LB::HLSL).source,
		                               HE::MaterialShaderLibrary::kHlslDomainEntry, "ds_5_0", err);
		REQUIRE_MESSAGE(dsb.Get() != nullptr, err);
		ComPtr<ID3D11HullShader> hs;
		ComPtr<ID3D11DomainShader> ds;
		REQUIRE(SUCCEEDED(dev->CreateHullShader(hsb->GetBufferPointer(), hsb->GetBufferSize(), nullptr, &hs)));
		REQUIRE(SUCCEEDED(dev->CreateDomainShader(dsb->GetBufferPointer(), dsb->GetBufferSize(), nullptr, &ds)));
		const double moved = centroidX(hs.Get(), ds.Get());
		REQUIRE(moved > 0.0);
		const double expected = std::min(height, HE::MatTessSettings{}.maxDisplacement) * pxPerNdc;
		CHECK_MESSAGE(std::abs((moved - base) - expected) < 0.35,
		              "moved ", moved - base, " px, expected ", expected, " px");
	}
}
#endif // _WIN32
#endif // HE_TESTS_HAVE_SHADERC
