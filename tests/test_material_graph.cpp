#include "doctest.h"

#include <MaterialGraph/MaterialGraph.h>

#if defined(HE_TESTS_HAVE_SHADERC)
#include <material/MaterialShaderLibrary.h>
#endif

using HE::MaterialGraph;
using HE::MatNodeType;

// Build the demo graph used across tests: lerp(orange, blue, fresnel) → lit output,
// with a time-driven sine on metallic — exercises inputs, math, shading and coercion.
static MaterialGraph makeDemoGraph()
{
	MaterialGraph g;
	const int out  = g.addNode(MatNodeType::Output);
	const int a    = g.addNode(MatNodeType::ConstColor);
	g.findNode(a)->p[0] = 0.95f; g.findNode(a)->p[1] = 0.42f; g.findNode(a)->p[2] = 0.18f;
	const int b    = g.addNode(MatNodeType::ConstColor);
	g.findNode(b)->p[0] = 0.10f; g.findNode(b)->p[1] = 0.35f; g.findNode(b)->p[2] = 0.85f;
	const int fres = g.addNode(MatNodeType::Fresnel);
	const int lerp = g.addNode(MatNodeType::Lerp);
	const int time = g.addNode(MatNodeType::Time);
	const int sine = g.addNode(MatNodeType::Sine);
	CHECK(g.connect(a,    0, lerp, 0));
	CHECK(g.connect(b,    0, lerp, 1));
	CHECK(g.connect(fres, 0, lerp, 2));
	CHECK(g.connect(lerp, 0, out,  0)); // BaseColor
	CHECK(g.connect(time, 0, sine, 0));
	CHECK(g.connect(sine, 0, out,  1)); // Metallic (float)
	return g;
}

TEST_CASE("MaterialGraph codegen emits the expected constructs")
{
	const MaterialGraph g = makeDemoGraph();
	const std::string glsl = HE::generateFragmentGlsl(g);

	CHECK(glsl.find("#version 450") == 0);
	CHECK(glsl.find("heLit(") != std::string::npos);          // lit output
	CHECK(glsl.find("mix(") != std::string::npos);            // Lerp
	CHECK(glsl.find("sin(") != std::string::npos);            // Sine
	CHECK(glsl.find("heLight.sunDir.w") != std::string::npos);// Time
	CHECK(glsl.find("vec2 vUV") != std::string::npos);        // UV varying declared
	CHECK(glsl.find("sampler2D") == std::string::npos);       // no texture node → no sampler

	// Unlit output drops heLit.
	MaterialGraph g2 = makeDemoGraph();
	for (auto& n : g2.nodes) if (n.type == MatNodeType::Output) n.p[0] = 0.0f;
	const std::string unlit = HE::generateFragmentGlsl(g2);
	CHECK(unlit.find("heLit(") == std::string::npos);

	// A texture node pulls in the sampler declaration.
	MaterialGraph g3 = MaterialGraph::makeDefault();
	const int texN = g3.addNode(MatNodeType::TextureSample);
	int outId = 0;
	for (auto& n : g3.nodes) if (n.type == MatNodeType::Output) outId = n.id;
	CHECK(g3.connect(texN, 0, outId, 0));
	const std::string texGlsl = HE::generateFragmentGlsl(g3);
	CHECK(texGlsl.find("uniform sampler2D heTex0") != std::string::npos);
	CHECK(texGlsl.find("texture(heTex0") != std::string::npos);
}

TEST_CASE("MaterialGraph guards: cycles, missing output, output not deletable")
{
	// Cycle: A.add → B.add → A.add again must not recurse forever.
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int n1  = g.addNode(MatNodeType::Add);
	const int n2  = g.addNode(MatNodeType::Add);
	CHECK(g.connect(n1, 0, n2, 0));
	CHECK(g.connect(n2, 0, n1, 0)); // cycle
	CHECK(g.connect(n2, 0, out, 0));
	const std::string glsl = HE::generateFragmentGlsl(g); // must terminate
	CHECK(glsl.find("void main()") != std::string::npos);

	// No output node → valid magenta error shader.
	MaterialGraph empty;
	const std::string err = HE::generateFragmentGlsl(empty);
	CHECK(err.find("vec4(1.0, 0.0, 1.0, 1.0)") != std::string::npos);

	// The output node cannot be removed; other nodes can (links go with them).
	MaterialGraph g4 = makeDemoGraph();
	const size_t nodesBefore = g4.nodes.size();
	int outId = 0, lerpId = 0;
	for (auto& n : g4.nodes)
	{
		if (n.type == MatNodeType::Output) outId  = n.id;
		if (n.type == MatNodeType::Lerp)   lerpId = n.id;
	}
	g4.removeNode(outId);
	CHECK(g4.nodes.size() == nodesBefore);
	g4.removeNode(lerpId);
	CHECK(g4.nodes.size() == nodesBefore - 1);
	for (const auto& l : g4.links)
	{
		CHECK(l.srcNode != lerpId);
		CHECK(l.dstNode != lerpId);
	}
}

TEST_CASE("MaterialGraph JSON round-trip preserves nodes, params and links")
{
	const MaterialGraph g = makeDemoGraph();
	const std::string json = HE::materialGraphToJson(g);

	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(json, r));
	CHECK(r.nodes.size() == g.nodes.size());
	CHECK(r.links.size() == g.links.size());
	CHECK(r.nextId == g.nextId);
	// Codegen over the round-tripped graph is byte-identical — the real invariant.
	CHECK(HE::generateFragmentGlsl(r) == HE::generateFragmentGlsl(g));

	// Garbage / empty input is rejected without touching the output graph.
	MaterialGraph untouched = MaterialGraph::makeDefault();
	const size_t n = untouched.nodes.size();
	CHECK_FALSE(HE::materialGraphFromJson("not json {", untouched));
	CHECK_FALSE(HE::materialGraphFromJson("", untouched));
	CHECK(untouched.nodes.size() == n);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("Generated graph GLSL cross-compiles through the real material pipeline")
{
	const MaterialGraph g = makeDemoGraph();
	const std::string glsl = HE::generateFragmentGlsl(g);
	const uint64_t hash = std::hash<std::string>{}(glsl);

	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	// Fragment (with the lighting preamble injected) for Metal + desktop GL.
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, msl.log);
	CHECK(msl.source.find("fragment ") != std::string::npos); // MSL entry point emitted
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, gl.log);
	CHECK(gl.source.find("#version 410") == 0);
	CHECK(gl.source.find("readonly buffer") == std::string::npos); // GL-4.1 safe (no SSBO)
	// The standard vertices still compile after the vUV extension.
	CHECK(lib.standardVertex(B::Metal).ok);
	CHECK(lib.standardVertex(B::GLSL410).ok);
}
#endif
