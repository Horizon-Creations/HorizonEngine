// Graph-material instancing (Thema 182, Schritt 2 = plan 2e).
//
// A foliage layer with a node-graph material (the Foliage template: Wind Sway on the
// WPO pin) used to draw one call per visible plant on Metal and GL, because the only
// instanced program was the built-in PBR one. MaterialShaderLibrary now emits an
// instanced twin of the material vertex (standardVertexInstanced / customVertexInstanced)
// and the GL and Metal colour passes draw a batch once with it. What these tests pin:
//   * the twin cross-compiles on every backend, with a real Wind Sway body, and reads
//     the instance data where each backend binds it (Metal vertex buffer 5, the
//     attribute locations 4-7 elsewhere);
//   * GL links it: the blocks it shares with the fragment are declared identically;
//   * the plain forms are untouched (the twin is opt-in, own cache slots);
//   * the GL and Metal draw sites gate it the way the plan says (source guards, since a
//     GL context or a Metal device is not there on every CI machine).
#include "doctest.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <MaterialGraph/MaterialGraph.h>
#include <material/MaterialShaderLibrary.h>

namespace fs = std::filesystem;
using namespace HE;

namespace
{
	fs::path findRepoRoot()
	{
		std::error_code ec;
		std::vector<fs::path> seeds;
		fs::path self(__FILE__);
		if (!self.is_absolute()) self = fs::current_path(ec) / self;
		seeds.push_back(self.parent_path());
		seeds.push_back(fs::current_path(ec));
		for (fs::path seed : seeds)
			for (int up = 0; up < 8 && !seed.empty(); ++up, seed = seed.parent_path())
				if (fs::exists(seed / "src" / "HE_Rendering" / "src" / "Backends" / "Metal" / "MetalRenderer.mm", ec))
					return seed;
		return {};
	}

	std::string readFile(const fs::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		std::ostringstream ss;
		ss << f.rdbuf();
		std::string s = ss.str();
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		return s;
	}

	size_t countOf(const std::string& hay, const std::string& needle)
	{
		size_t n = 0;
		for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
		return n;
	}

#if defined(HE_TESTS_HAVE_SHADERC)
	// The Foliage template's vertex: a lit default graph with Wind Sway on the WPO pin.
	MatShaderGen windGraph()
	{
		MaterialGraph g = MaterialGraph::makeDefault();
		int out = 0;
		for (auto& n : g.nodes) if (n.type == MatNodeType::Output) out = n.id;
		const int sway = g.addNode(MatNodeType::WindSway);
		REQUIRE(g.connect(sway, 0, out, HE::kMatOutputWPOPin));
		MatShaderGen gen = HE::generateFragment(g);
		REQUIRE_FALSE(gen.vertexBody.empty());
		return gen;
	}

	// The text of a uniform block from its name to the instance name, GLSL ES's
	// `highp ` dropped (the vertex stage defaults to it and leaves the word out).
	std::string block(const std::string& src, const std::string& name, const std::string& inst)
	{
		const size_t b = src.find("uniform " + name);
		if (b == std::string::npos) return {};
		const size_t e = src.find(inst + ";", b);
		if (e == std::string::npos) return {};
		std::string s = src.substr(b, e - b);
		for (size_t p; (p = s.find("highp ")) != std::string::npos; ) s.erase(p, 6);
		return s;
	}
#endif
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("The instanced material vertex cross-compiles on every backend, Wind Sway included")
{
	const MatShaderGen gen = windGraph();
	using B = MaterialShaderLibrary::Backend;
	MaterialShaderLibrary lib;
	const uint64_t vh = std::hash<std::string>{}(gen.vertexBody);
	for (B b : { B::Metal, B::GLSL410, B::GLSL430, B::GLSLES300, B::HLSL, B::SpirV })
	{
		INFO("backend ", (int)b);
		const auto& sv = lib.standardVertexInstanced(b);
		const auto& cv = lib.customVertexInstanced(vh, gen.vertexBody, b);
		CHECK_MESSAGE(sv.ok, sv.log);
		CHECK_MESSAGE(cv.ok, cv.log);
		if (b == B::SpirV) { CHECK_FALSE(cv.spirv.empty()); continue; }
		REQUIRE_FALSE(cv.source.empty());
		if (b == B::Metal)
		{
			// {mvp, model} per instance at the slot every instanced Metal draw binds,
			// the wind channels (HeLighting) still at buffer 2, HeParams at 3.
			CHECK(sv.source.find("[[buffer(5)]]") != std::string::npos);
			CHECK(cv.source.find("[[buffer(5)]]") != std::string::npos);
			CHECK(cv.source.find("[[buffer(2)]]") != std::string::npos);
			CHECK(cv.source.find("[[instance_id]]") != std::string::npos);
		}
		else if (b == B::HLSL)
			CHECK(cv.source.find("TEXCOORD4") != std::string::npos); // location 4 → semantic
		else
		{
			// The mesh VAO's divisor-1 binding: four vec4 at locations 4..7.
			CHECK(cv.source.find("layout(location = 4) in") != std::string::npos);
			CHECK(cv.source.find("layout(location = 7) in") != std::string::npos);
			CHECK(sv.source.find("layout(location = 4) in") != std::string::npos);
		}
	}
}

TEST_CASE("The instanced twin is a separate cache slot; the plain vertex is unchanged by it")
{
	const MatShaderGen gen = windGraph();
	using B = MaterialShaderLibrary::Backend;
	MaterialShaderLibrary lib;
	const uint64_t vh = std::hash<std::string>{}(gen.vertexBody);
	for (B b : { B::Metal, B::GLSL410 })
	{
		INFO("backend ", (int)b);
		const std::string plainStd = lib.standardVertex(b).source;
		const std::string plainCus = lib.customVertex(vh, gen.vertexBody, b).source;
		const std::string instStd  = lib.standardVertexInstanced(b).source;
		const std::string instCus  = lib.customVertexInstanced(vh, gen.vertexBody, b).source;
		CHECK(instStd != plainStd);
		CHECK(instCus != plainCus);
		// Asking for the twin first must not change what the plain call returns.
		MaterialShaderLibrary fresh;
		(void)fresh.customVertexInstanced(vh, gen.vertexBody, b);
		(void)fresh.standardVertexInstanced(b);
		CHECK(fresh.customVertex(vh, gen.vertexBody, b).source == plainCus);
		CHECK(fresh.standardVertex(b).source == plainStd);
		// The plain forms carry no instance input at all.
		CHECK(plainStd.find("aInstCol") == std::string::npos);
		CHECK(plainCus.find("aInstCol") == std::string::npos);
		CHECK(plainCus.find("[[buffer(5)]]") == std::string::npos);
	}
}

TEST_CASE("GL links the instanced WPO twin: the shared blocks match the fragment's")
{
	// GL links both stages into one program; a block of the same name must be declared
	// identically in each (the plain WPO vertex once failed on exactly that, see
	// test_material_graph "GL links a WPO material"). The twin keeps U and HeLighting.
	const MatShaderGen gen = windGraph();
	using B = MaterialShaderLibrary::Backend;
	MaterialShaderLibrary lib;
	for (B b : { B::GLSL410, B::GLSLES300 })
	{
		INFO("backend ", (int)b);
		const auto& fs = lib.fragment(std::hash<std::string>{}(gen.glsl), gen.glsl, b);
		const auto& vs = lib.customVertexInstanced(std::hash<std::string>{}(gen.vertexBody), gen.vertexBody, b);
		REQUIRE_MESSAGE(fs.ok, fs.log);
		REQUIRE_MESSAGE(vs.ok, vs.log);
		const std::string fl = block(fs.source, "HeLighting", "heLight");
		const std::string vl = block(vs.source, "HeLighting", "heLight");
		REQUIRE_FALSE(vl.empty());
		CHECK_MESSAGE(vl == fl, "vertex:\n", vl, "\nfragment:\n", fl);
		const std::string fu = block(fs.source, "U", "u");
		const std::string vu = block(vs.source, "U", "u");
		REQUIRE_FALSE(vu.empty());
		if (!fu.empty()) CHECK_MESSAGE(vu == fu, "vertex:\n", vu, "\nfragment:\n", fu);
	}
}
#endif // HE_TESTS_HAVE_SHADERC

TEST_CASE("GL and Metal draw a graph-material batch once, with the material's instanced twin")
{
	// The draw sites, read as source: a GL context or a Metal device is not there on every
	// CI machine. Each colour pass must build the twin only for a batch, only when the
	// switch HE_GRAPH_INSTANCING allows it, and keep the per-instance loop as the fallback
	// (baked pak variant, failed build, switch off).
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("backend sources not found - graph instancing guard skipped");
		return;
	}
	const fs::path dir = root / "src" / "HE_Rendering" / "src" / "Backends";

	const std::string gl = readFile(dir / "OpenGL" / "OpenGLRenderer.cpp");
	REQUIRE(!gl.empty());
	// G-buffer and forward pass: one instanced build each, both behind the switch.
	CHECK(countOf(gl, "/*instanced=*/true)") == 2);
	CHECK(countOf(gl, "RenderSorter::graphInstancingEnabled()") == 2);
	CHECK(countOf(gl, "DrawGraphBatchInstanced(dc.instanceTransforms") == 2);
	// No baked instanced vertex: a precompiled material must get 0 back, never its
	// plain vertex under an instanced draw (that would stack every plant on u.model).
	CHECK(gl.find("if (instanced && precompiled) return 0;") != std::string::npos);
	// The loops stay as the fallback.
	CHECK(countOf(gl, "drawGraphInstance(t)") >= 2);

	const std::string mtl = readFile(dir / "Metal" / "MetalRenderer.mm");
	REQUIRE(!mtl.empty());
	CHECK(countOf(mtl, "/*instanced=*/true)") == 2);
	CHECK(countOf(mtl, "RenderSorter::graphInstancingEnabled()") == 2);
	CHECK(mtl.find("if (instanced && precompiled) return nullptr;") != std::string::npos);
	// The forward PSO may come from the pak (no twin); the G-buffer PSO never does.
	CHECK(mtl.find("instCount <= k_maxInstances && !cShBaked") != std::string::npos);
	// Both loops hand the batch to drawInstance, which binds buffer 5 and draws instanceCount.
	CHECK(countOf(mtl, "drawInstance(dc.instanceTransforms.front(), cMaterialPipeline") == 2);
	CHECK(countOf(mtl, "instanceCount:instCount];") == 2);
}
