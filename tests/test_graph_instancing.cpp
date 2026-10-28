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
//     GL context or a Metal device is not there on every CI machine);
//   * since step 4 (plan 2f) the D3D11, D3D12 and Vulkan draw sites too, with the twin's
//     own cache key salted before the first cache lookup.
#include "doctest.h"
#include <algorithm>
#include <cstdlib>
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
TEST_CASE("Graph instancing: dump the instanced Wind Sway twins for the offline compilers (HE_DUMP_WATER_SHADERS)")
{
	// The in-process checks above stop at SPIRV-Cross. The Metal side of 2e is a text patch
	// on Windows, and the macOS CI job is the only place a real Metal compiler sees it:
	// scripts/water_shader_offline_check.py runs this case next to the water dump and hands
	// the files to glslangValidator -l (GL pairs linked), spirv-val, fxc and xcrun metal.
	const char* dir = std::getenv("HE_DUMP_WATER_SHADERS");
	if (!dir || !*dir)
	{
		MESSAGE("HE_DUMP_WATER_SHADERS not set - nothing dumped (scripts/water_shader_offline_check.py sets it)");
		return;
	}
	const MatShaderGen gen = windGraph();
	const fs::path out(dir);
	fs::create_directories(out);
	auto put = [&](const char* name, const std::string& text) {
		std::ofstream f(out / name, std::ios::binary);
		f.write(text.data(), static_cast<std::streamsize>(text.size()));
		REQUIRE_MESSAGE(f.good(), "could not write ", (out / name).string());
	};
	auto putSpv = [&](const char* name, const std::vector<uint32_t>& words) {
		std::ofstream f(out / name, std::ios::binary);
		f.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
		REQUIRE_MESSAGE(f.good(), "could not write ", (out / name).string());
	};
	auto need = [](const MaterialShaderLibrary::Compiled& c, const char* what) -> const MaterialShaderLibrary::Compiled& {
		REQUIRE_MESSAGE(c.ok, what, ": ", c.log);
		return c;
	};
	using B = MaterialShaderLibrary::Backend;
	MaterialShaderLibrary lib;
	const uint64_t vh = std::hash<std::string>{}(gen.vertexBody);
	const uint64_t hf = std::hash<std::string>{}(gen.glsl), hg = std::hash<std::string>{}(gen.glslGBuffer);
	// GL: the twin with the fragments it is linked to at runtime.
	put("wind_inst_gl410.vert",      need(lib.customVertexInstanced(vh, gen.vertexBody, B::GLSL410), "GL410 wind twin").source);
	put("inst_gl410.vert",           need(lib.standardVertexInstanced(B::GLSL410), "GL410 plain twin").source);
	put("wind_gl410.frag",           need(lib.fragment(hf, gen.glsl, B::GLSL410), "GL410 wind fragment").source);
	put("wind_gl410_gbuf.frag",      need(lib.fragment(hg, gen.glslGBuffer, B::GLSL410), "GL410 wind G-buffer").source);
	put("wind_inst_es300.vert",      need(lib.customVertexInstanced(vh, gen.vertexBody, B::GLSLES300), "ES300 wind twin").source);
	put("wind_es300.frag",           need(lib.fragment(hf, gen.glsl, B::GLSLES300), "ES300 wind fragment").source);
	put("wind_inst_gl430.vert",      need(lib.customVertexInstanced(vh, gen.vertexBody, B::GLSL430), "GL430 wind twin").source);
	put("wind_gl430_clustered.frag", need(lib.fragmentClustered(hf, gen.glsl, B::GLSL430), "GL430 wind clustered").source);
	// D3D / Vulkan: compiled, not wired yet (Windows track).
	put("wind_inst_hlsl_vert.hlsl",  need(lib.customVertexInstanced(vh, gen.vertexBody, B::HLSL), "HLSL wind twin").source);
	putSpv("wind_inst_vk_vert.spv",  need(lib.customVertexInstanced(vh, gen.vertexBody, B::SpirV), "SPIR-V wind twin").spirv);
	// Metal: both twins as MetalRenderer builds them (pinned, instance array at buffer 5).
	put("wind_inst_metal_vert.metal", need(lib.customVertexInstanced(vh, gen.vertexBody, B::Metal), "MSL wind twin").source);
	put("inst_metal_vert.metal",      need(lib.standardVertexInstanced(B::Metal), "MSL plain twin").source);
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
	CHECK(countOf(gl, "DrawGraphBatchInstanced(dc,") == 2);
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
	CHECK(countOf(mtl, "drawInstance(dc.firstInstance(), cMaterialPipeline") == 2);
	CHECK(countOf(mtl, "instanceCount:instCount];") == 2);
}

TEST_CASE("D3D11, D3D12 and Vulkan draw a graph-material batch once, with the material's instanced twin")
{
	// Thema 182 step 4 (plan 2f): the twin wired on the Windows backends. Read as source like
	// the GL/Metal guard above (no D3D device on the macOS/Linux runners); the run on real
	// hardware is scripts/perf/foliage_graph_ab.py --rhi D3D11|D3D12|Vulkan.
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("backend sources not found - graph instancing guard skipped");
		return;
	}
	const fs::path dir = root / "src" / "HE_Rendering" / "src" / "Backends";
	struct Site { const char* name; fs::path file; const char* salt; const char* firstCacheUse;
	              const char* lastInstElement; const char* batchDraw; };
	const Site sites[] = {
		{ "D3D11", dir / "D3D11" / "D3D11Renderer.cpp",
		  "^ (instanced ? 0x1D57A9CEDB0B1E55ULL : 0ULL);", "m_materialShaders.find(key)",
		  "{ \"TEXCOORD\", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 }",
		  "DrawIndexedInstanced(range.count, instCount, range.start, 0, 0);" },
		{ "D3D12", dir / "D3D12" / "D3D12Renderer.cpp",
		  "if (instanced) hash ^= 0x1D57A9CEDB0B1E55ULL;", "m_matBytecode.find(hash)",
		  "{ \"TEXCOORD\", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, kInst, 1 }",
		  "DrawIndexedInstanced(range.count, instCount, range.start, 0, 0);" },
		{ "Vulkan", dir / "Vulkan" / "VulkanRenderer.cpp",
		  "if (instanced) hash ^= 0x1D57A9CEDB0B1E55ULL;", "m_materialPipelines.find(key)",
		  "{ 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 112 }",
		  "vkCmdDrawIndexed(cmd, range.count, instCount, range.start, 0, 0);" },
	};
	for (const Site& s : sites)
	{
		INFO(s.name);
		const std::string src = readFile(s.file);
		REQUIRE(!src.empty());
		// Forward and G-buffer pass: one twin build each, behind the switch.
		CHECK(countOf(src, "/*instanced=*/true);") == 2);
		CHECK(countOf(src, "RenderSorter::graphInstancingEnabled()") == 1);
		// The pak bakes no instanced vertex: a precompiled material gets no twin.
		CHECK(src.find("if (instanced && precompiled) return") != std::string::npos);
		// The twin has its own cache key, salted BEFORE the first cache lookup: the plain
		// vertex behind the instanced input layout builds silently and stacks every plant
		// on one spot (the bytecode/shader caches are keyed by the shader hash alone).
		const size_t salt = src.find(s.salt), use = src.find(s.firstCacheUse);
		CHECK(salt != std::string::npos);
		CHECK(use != std::string::npos);
		CHECK(salt < use);
		// The model at locations 4..7, stepped per instance, and ONE draw for the batch.
		CHECK(src.find(s.lastInstElement) != std::string::npos);
		CHECK(countOf(src, s.batchDraw) == 1);
		// The per-instance material loop stays as the fallback.
		// (read in place or copied: forEachInstance serves both forms of the batch)
		CHECK((src.find("drawMatInstance(t)") != std::string::npos
		       || src.find("forEachInstance(drawMatInstance)") != std::string::npos));
	}
}

TEST_CASE("D3D12 and Vulkan take every instanced run's slots from a growable arena")
{
	// Thema 182 step 4 (plan 2f, the instance budget). Every instanced run of a frame takes
	// slots of its own (each cascade, SSAO, GI, the scene), and a frame past the fixed
	// 65536-slot ring used to fall back to one draw per object. On D3D12 that loop shares the
	// k_maxDraws constant-buffer ring, which the shadow pass used up at 100k foliage plants:
	// the scene pass then drew nothing, terrain included (RTX 4070). The arena opens a further
	// chunk instead. Pinned as source: no draw site may test the fixed capacity any more.
	const fs::path root = findRepoRoot();
	if (root.empty())
	{
		MESSAGE("backend sources not found - instance arena guard skipped");
		return;
	}
	const fs::path dir = root / "src" / "HE_Rendering" / "src" / "Backends";
	const std::string d3d12 = readFile(dir / "D3D12" / "D3D12Renderer.cpp");
	const std::string vk    = readFile(dir / "Vulkan" / "VulkanRenderer.cpp");
	REQUIRE(!d3d12.empty());
	REQUIRE(!vk.empty());
	for (const std::string* src : { &d3d12, &vk })
	{
		INFO((src == &d3d12 ? "D3D12" : "Vulkan"));
		// drawDepthInstanced + the scene's built-in batch + the graph twin.
		CHECK(countOf(*src, "allocInstances(") >= 4); // the definition and three callers
		CHECK(src->find("<= k_maxInstances") == std::string::npos);
		CHECK(src->find("> k_maxInstances") == std::string::npos);
		// Step 8: the sizing is HE::InstanceArena's (test_instance_arena.cpp), no
		// backend-local cap that counts chunk capacities.
		CHECK(src->find("k_instArenaMax") == std::string::npos);
		CHECK(src->find(".chunkFor(count)") != std::string::npos);
		CHECK(countOf(*src, ".refuse(count)") == 2); // over budget, chunk creation failed
		CHECK(src->find(".nextMainCapacity()") != std::string::npos);
	}
	const std::string vkH = readFile(root / "src" / "HE_Rendering" / "include" / "Backends" / "Vulkan" / "VulkanRenderer.h");
	CHECK(d3d12.find("InstanceArena::budgetFromEnv(k_instStride)") != std::string::npos);
	CHECK(vkH.find("InstanceArena::budgetFromEnv(k_instStride)") != std::string::npos);
	// The slot's ring is reset once per frame through the arena, never by hand.
	CHECK(d3d12.find("p.beginInstanceFrame(p.frameIndex);") != std::string::npos);
	CHECK(countOf(vk, "beginInstanceFrame();") == 2); // Render() and DrawViewportFrame()
	CHECK(vk.find("m_instCursor = 0;") == std::string::npos);
	// Where the per-object ring still runs out, it says so.
	CHECK(countOf(d3d12, "p.noteDrawRingFull()") >= 3);
}
