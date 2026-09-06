#include "doctest.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
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
	CHECK(glsl.find("heLitP(") != std::string::npos);         // lit output (full-scene-lights variant)
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

TEST_CASE("MaterialGraph v2 nodes: noise helpers, view-dir fresnel, named params")
{
	// Noise/FBM pull in the helper functions exactly once; Panner uses the time uniform.
	MaterialGraph g;
	const int out    = g.addNode(MatNodeType::Output);
	const int uv     = g.addNode(MatNodeType::UV);
	const int pan    = g.addNode(MatNodeType::Panner);
	const int fbm    = g.addNode(MatNodeType::Fbm);
	const int noise  = g.addNode(MatNodeType::ValueNoise);
	const int lerp   = g.addNode(MatNodeType::Lerp);
	const int colA   = g.addNode(MatNodeType::ParamColor);
	g.findNode(colA)->s = "GrassTint";
	CHECK(g.connect(uv,   0, pan,   0));
	CHECK(g.connect(pan,  0, fbm,   0));
	CHECK(g.connect(uv,   0, noise, 0));
	CHECK(g.connect(colA, 0, lerp,  0));
	CHECK(g.connect(fbm,  0, lerp,  2));
	CHECK(g.connect(lerp, 0, out,   0));
	const std::string glsl = HE::generateFragmentGlsl(g);
	CHECK(glsl.find("heValueNoise(") != std::string::npos);
	CHECK(glsl.find("heFbm(") != std::string::npos);
	// helper definitions appear exactly once
	size_t first = glsl.find("float heValueNoise(vec2 p)");
	REQUIRE(first != std::string::npos);
	CHECK(glsl.find("float heValueNoise(vec2 p)", first + 1) == std::string::npos);
	CHECK(glsl.find("param: GrassTint") != std::string::npos);
	CHECK(glsl.find("vWorldPos") != std::string::npos); // varying declared in the header

	// Fresnel + ViewDir use the real camera position, not a fixed axis.
	MaterialGraph g2 = MaterialGraph::makeDefault();
	int out2 = 0;
	for (auto& n : g2.nodes) if (n.type == MatNodeType::Output) out2 = n.id;
	const int fres = g2.addNode(MatNodeType::Fresnel);
	CHECK(g2.connect(fres, 0, out2, 1));
	const std::string fresGlsl = HE::generateFragmentGlsl(g2);
	CHECK(fresGlsl.find("heLight.camPos.xyz - vWorldPos") != std::string::npos);

	// Param name survives the JSON round-trip.
	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), r));
	bool foundName = false;
	for (const auto& n : r.nodes)
		if (n.type == MatNodeType::ParamColor && n.s == "GrassTint") foundName = true;
	CHECK(foundName);
	CHECK(HE::generateFragmentGlsl(r) == glsl);
}

TEST_CASE("MaterialGraph v3: RGBA output, split/combine masks, param slots")
{
	// Opacity flows into oColor.a; texture alpha is a separate pin; split/combine round-trip.
	// Since blend modes (v9), the Opacity pin only feeds alpha in TRANSLUCENT mode.
	MaterialGraph g;
	const int out   = g.addNode(MatNodeType::Output);
	g.findNode(out)->p[1] = 2.0f; // Translucent
	const int tex   = g.addNode(MatNodeType::TextureSample);
	const int comb  = g.addNode(MatNodeType::CombineRGBA);
	const int split = g.addNode(MatNodeType::SplitRGBA);
	const int pf    = g.addNode(MatNodeType::ParamFloat);
	g.findNode(pf)->s = "Glow";
	const int pc    = g.addNode(MatNodeType::ParamColor);
	g.findNode(pc)->s = "Tint";
	CHECK(g.connect(tex,   0, comb,  0)); // RGB (coerced vec3→float .x)
	CHECK(g.connect(tex,   1, comb,  3)); // texture ALPHA pin → A
	CHECK(g.connect(comb,  0, split, 0)); // vec4 → split
	CHECK(g.connect(split, 3, out,   4)); // A → Opacity
	CHECK(g.connect(pc,    0, out,   0)); // param color → BaseColor
	CHECK(g.connect(pf,    0, out,   1)); // param float → Metallic

	const HE::MatShaderGen gen = HE::generateFragment(g);
	CHECK(gen.glsl.find(".w") != std::string::npos);                       // alpha access
	CHECK(gen.glsl.find("uniform HeParams") != std::string::npos);         // params UBO emitted
	CHECK(gen.glsl.find("heParams.v[") != std::string::npos);
	REQUIRE(gen.params.size() == 2);
	// Slot order is emission order; both named params present with their values.
	bool hasTint = false, hasGlow = false;
	for (const auto& sl : gen.params)
	{
		if (sl.name == "Tint") { hasTint = true; CHECK(sl.isColor); }
		if (sl.name == "Glow") { hasGlow = true; CHECK_FALSE(sl.isColor); }
	}
	CHECK(hasTint); CHECK(hasGlow);

	// No params → no UBO block.
	CHECK(HE::generateFragmentGlsl(MaterialGraph::makeDefault()).find("HeParams")
	      == std::string::npos);
}

TEST_CASE("MaterialGraph v3: material functions inline (and recursion is guarded)")
{
	// Function: doubles its input. Interface = FnInput → FnOutput.
	MaterialGraph fn;
	const int fin  = fn.addNode(MatNodeType::FnInput);
	fn.findNode(fin)->s = "X";
	const int two  = fn.addNode(MatNodeType::ConstFloat);
	fn.findNode(two)->p[0] = 2.0f;
	const int mul  = fn.addNode(MatNodeType::Multiply);
	const int fout = fn.addNode(MatNodeType::FnOutput);
	fn.findNode(fout)->s = "Doubled";
	CHECK(fn.connect(fin, 0, mul, 0));
	CHECK(fn.connect(two, 0, mul, 1));
	CHECK(fn.connect(mul, 0, fout, 0));

	// Interface extraction drives the call node's pins.
	std::vector<HE::MatPinDesc> ins, outs;
	HE::matFunctionPins(fn, ins, outs);
	REQUIRE(ins.size() == 1);  CHECK(std::string(ins[0].name) == "X");
	REQUIRE(outs.size() == 1); CHECK(std::string(outs[0].name) == "Doubled");

	// Material calls the function with a constant.
	MaterialGraph g;
	const int out  = g.addNode(MatNodeType::Output);
	const int c    = g.addNode(MatNodeType::ConstColor);
	const int call = g.addNode(MatNodeType::FunctionCall);
	g.findNode(call)->s = "Fns/Double.hasset";
	CHECK(g.connect(c,    0, call, 0));
	CHECK(g.connect(call, 0, out,  0));

	HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
	{ return path == "Fns/Double.hasset" ? &fn : nullptr; };
	const std::string glsl = HE::generateFragment(g, loader).glsl;
	CHECK(glsl.find("* ") != std::string::npos);           // the multiply was inlined
	CHECK(glsl.find("2.000000") != std::string::npos);     // with the function's constant
	CHECK(glsl.find("missing function") == std::string::npos);

	// Missing loader → magenta placeholder, still valid GLSL.
	const std::string noLoader = HE::generateFragment(g, {}).glsl;
	CHECK(noLoader.find("missing function") != std::string::npos);

	// Self-recursive function → guarded (magenta), terminates.
	MaterialGraph rec;
	const int rIn   = rec.addNode(MatNodeType::FnInput);
	const int rCall = rec.addNode(MatNodeType::FunctionCall);
	rec.findNode(rCall)->s = "Fns/Rec.hasset";
	const int rOut  = rec.addNode(MatNodeType::FnOutput);
	CHECK(rec.connect(rIn,   0, rCall, 0));
	CHECK(rec.connect(rCall, 0, rOut,  0));
	MaterialGraph g2;
	const int out2  = g2.addNode(MatNodeType::Output);
	const int call2 = g2.addNode(MatNodeType::FunctionCall);
	g2.findNode(call2)->s = "Fns/Rec.hasset";
	CHECK(g2.connect(call2, 0, out2, 0));
	HE::MatFunctionLoader recLoader = [&](const std::string& path) -> const MaterialGraph*
	{ return path == "Fns/Rec.hasset" ? &rec : nullptr; };
	const std::string recGlsl = HE::generateFragment(g2, recLoader).glsl; // must terminate
	CHECK(recGlsl.find("recursive") != std::string::npos);
}

TEST_CASE("MaterialGraph v4: extra inputs + project-texture slots")
{
	// New input nodes emit the expected expressions.
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int cd  = g.addNode(MatNodeType::CameraDistance);
	const int sp  = g.addNode(MatNodeType::ScreenPos);
	const int cp  = g.addNode(MatNodeType::CameraPos);
	const int v4  = g.addNode(MatNodeType::ConstVec4);
	g.findNode(v4)->p[3] = 0.5f;
	CHECK(g.connect(cp, 0, out, 0)); // BaseColor = camera pos
	CHECK(g.connect(cd, 0, out, 1)); // Metallic = distance
	CHECK(g.connect(sp, 0, out, HE::kMatOutputEmissivePin)); // Emissive from screen pos (coerced)
	CHECK(g.connect(v4, 0, out, HE::kMatOutputOpacityPin)); // Opacity from vec4.x
	const std::string glsl = HE::generateFragmentGlsl(g);
	CHECK(glsl.find("length(heLight.camPos.xyz - vWorldPos)") != std::string::npos);
	CHECK(glsl.find("gl_FragCoord.xy") != std::string::npos);
	CHECK(glsl.find("heLight.camPos.xyz") != std::string::npos);

	// Texture Sample nodes: empty path → legacy heTex0; distinct picked paths get
	// their own slots heTexP0/heTexP1; a repeated path shares a slot.
	MaterialGraph t;
	const int tout  = t.addNode(MatNodeType::Output);
	const int def   = t.addNode(MatNodeType::TextureSample);           // no path → heTex0
	const int a     = t.addNode(MatNodeType::TextureSample);
	t.findNode(a)->s = "Tex/grass.hasset";
	const int b     = t.addNode(MatNodeType::TextureSample);
	t.findNode(b)->s = "Tex/rock.hasset";
	const int b2    = t.addNode(MatNodeType::TextureSample);
	t.findNode(b2)->s = "Tex/rock.hasset";                            // same as b → shares slot
	const int comb  = t.addNode(MatNodeType::Add);
	CHECK(t.connect(def, 0, comb, 0));
	CHECK(t.connect(a,   0, comb, 1));
	CHECK(t.connect(comb,0, tout, 0));
	CHECK(t.connect(b,   0, tout, HE::kMatOutputEmissivePin));
	CHECK(t.connect(b2,  0, tout, HE::kMatOutputEmissivePin)); // replaces link but references rock again
	const HE::MatShaderGen gen = HE::generateFragment(t);
	REQUIRE(gen.textures.size() == 2);          // grass, rock (dedup)
	CHECK(gen.textures[0] == "Tex/grass.hasset");
	CHECK(gen.textures[1] == "Tex/rock.hasset");
	CHECK(gen.glsl.find("uniform sampler2D heTex0")   != std::string::npos); // legacy default used
	CHECK(gen.glsl.find("uniform sampler2D heTexP0")  != std::string::npos);
	CHECK(gen.glsl.find("uniform sampler2D heTexP1")  != std::string::npos);
	CHECK(gen.glsl.find("binding = 4)")               != std::string::npos); // first project tex
}

TEST_CASE("MaterialGraph v5: logic nodes, If, and new parameter types")
{
	// If(Cond, orange, blue) driven by Greater(param 'K' > 0.5) → BaseColor.
	MaterialGraph g;
	const int out   = g.addNode(MatNodeType::Output);
	const int k     = g.addNode(MatNodeType::ParamFloat);
	g.findNode(k)->s = "K"; g.findNode(k)->p[0] = 0.8f;
	const int half  = g.addNode(MatNodeType::ConstFloat);
	g.findNode(half)->p[0] = 0.5f;
	const int gt    = g.addNode(MatNodeType::Greater);
	const int orange= g.addNode(MatNodeType::ConstColor);
	g.findNode(orange)->p[0] = 0.9f;
	const int blue  = g.addNode(MatNodeType::ConstColor);
	g.findNode(blue)->p[2] = 0.9f;
	const int iff   = g.addNode(MatNodeType::If);
	CHECK(g.connect(k,     0, gt,  0));
	CHECK(g.connect(half,  0, gt,  1));
	CHECK(g.connect(gt,    0, iff, 0));  // Cond
	CHECK(g.connect(orange,0, iff, 1));  // True
	CHECK(g.connect(blue,  0, iff, 2));  // False
	CHECK(g.connect(iff,   0, out, 0));  // BaseColor
	const std::string glsl = HE::generateFragmentGlsl(g);
	CHECK(glsl.find(" > ") != std::string::npos);          // Greater
	CHECK(glsl.find("mix(") != std::string::npos);         // If via mix+step
	CHECK(glsl.find("step(0.5,") != std::string::npos);    // If condition threshold

	// And/Or/Not compile to boolean float expressions.
	MaterialGraph b;
	const int bout = b.addNode(MatNodeType::Output);
	const int p1   = b.addNode(MatNodeType::ConstBool);
	const int p2   = b.addNode(MatNodeType::ConstBool); b.findNode(p2)->p[0] = 0.0f;
	const int andN = b.addNode(MatNodeType::And);
	const int notN = b.addNode(MatNodeType::Not);
	CHECK(b.connect(p1, 0, andN, 0));
	CHECK(b.connect(p2, 0, andN, 1));
	CHECK(b.connect(andN, 0, notN, 0));
	CHECK(b.connect(notN, 0, bout, 1)); // Metallic
	const std::string bglsl = HE::generateFragmentGlsl(b);
	CHECK(bglsl.find("&&") != std::string::npos);          // And
	CHECK(bglsl.find("<= 0.5") != std::string::npos);      // Not

	// New parameter types get HeParams slots with the right component layout.
	MaterialGraph p;
	const int pout = p.addNode(MatNodeType::Output);
	const int pv2  = p.addNode(MatNodeType::ParamVec2);
	p.findNode(pv2)->s = "Tiling"; p.findNode(pv2)->p[0] = 4.0f; p.findNode(pv2)->p[1] = 2.0f;
	const int pv4  = p.addNode(MatNodeType::ParamVec4);
	p.findNode(pv4)->s = "Tint"; p.findNode(pv4)->p[0] = 0.3f; p.findNode(pv4)->p[3] = 1.0f;
	const int pb   = p.addNode(MatNodeType::ParamBool);
	p.findNode(pb)->s = "Toggle"; p.findNode(pb)->p[0] = 1.0f;
	CHECK(p.connect(pv4, 0, pout, 0));  // BaseColor from vec4 (coerced)
	CHECK(p.connect(pb,  0, pout, 1));  // Metallic from bool
	CHECK(p.connect(pv2, 0, pout, HE::kMatOutputEmissivePin));  // Emissive from vec2 (coerced)
	const HE::MatShaderGen gen = HE::generateFragment(p);
	REQUIRE(gen.params.size() == 3);
	// Slots in first-emit order; check names + component values survive.
	bool sawTiling = false, sawTint = false, sawToggle = false;
	for (const auto& sl : gen.params)
	{
		if (sl.name == "Tiling") { sawTiling = true; CHECK(sl.kind == HE::MatParamKind::Vec2);
			CHECK(sl.value[0] == doctest::Approx(4.0f)); CHECK(sl.value[1] == doctest::Approx(2.0f)); }
		if (sl.name == "Tint")   { sawTint = true; CHECK(sl.kind == HE::MatParamKind::Vec4);
			CHECK(sl.value[0] == doctest::Approx(0.3f)); CHECK(sl.value[3] == doctest::Approx(1.0f)); }
		if (sl.name == "Toggle") { sawToggle = true; CHECK(sl.kind == HE::MatParamKind::Bool);
			CHECK(sl.value[0] == doctest::Approx(1.0f)); }
	}
	CHECK(sawTiling); CHECK(sawTint); CHECK(sawToggle);
	CHECK(gen.glsl.find("step(0.5, heParams") != std::string::npos); // ParamBool threshold
}

TEST_CASE("Every node type has a registry entry and its emit matches its pins")
{
	// The registry (pins) drives both the editor UI and codegen; a type missing from
	// it, or an emit case reading a pin the registry doesn't declare, is a bug.
	for (int t = 0; t <= (int)MatNodeType::NormalMapSample; ++t)
	{
		const auto type = static_cast<MatNodeType>(t);
		const HE::MatNodeDesc& d = HE::matNodeDesc(type);
		CHECK_MESSAGE(d.type == type, "registry entry missing/mismatched for node ", t);
		// matNodeDescByName must round-trip the display name back to the same type.
		const HE::MatNodeDesc* byName = HE::matNodeDescByName(d.name);
		REQUIRE(byName != nullptr);
		CHECK(byName->type == type);
	}
}

TEST_CASE("Unconnected noise UV falls back to the mesh UV (vUV), not vec2(0)")
{
	// Regression: a Noise/FBM/Checker node with nothing wired to its UV pin used to
	// default to vec2(0), producing a CONSTANT value that just darkened everything.
	// It must now sample vUV so the pattern actually varies across the surface.
	struct Case { MatNodeType type; const char* expect; };
	for (Case cs : { Case{ MatNodeType::ValueNoise, "heValueNoise(vUV" },
	                 Case{ MatNodeType::Fbm,        "heFbm(vUV" },
	                 Case{ MatNodeType::Checker,    "floor(vUV" } })
	{
		MaterialGraph g;
		const int out = g.addNode(MatNodeType::Output);
		const int n   = g.addNode(cs.type);
		CHECK(g.connect(n, 0, out, 0));
		const std::string glsl = HE::generateFragment(g).glsl;
		// The noise samples the mesh UV directly — no vec2(0) constant fed into it.
		CHECK_MESSAGE(glsl.find(cs.expect) != std::string::npos,
		              "expected '", cs.expect, "' in:\n", glsl);
	}
}

TEST_CASE("Noise Texture node emits 3D world-space fbm as a vec3 (mesh-independent, drop-in)")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int tex = g.addNode(MatNodeType::NoiseTexture); // default Scale = 6
	const int col = g.addNode(MatNodeType::ConstColor);
	const int mul = g.addNode(MatNodeType::Multiply);
	CHECK(g.connect(col, 0, mul, 0));
	CHECK(g.connect(tex, 0, mul, 1));                          // colour * noise → mottling
	CHECK(g.connect(mul, 0, out, 0));
	const std::string glsl = HE::generateFragment(g).glsl;
	// World-space 3D noise so it varies on ANY mesh (a UV-less cube has vUV=0 everywhere,
	// which would collapse UV noise to a single value → flat/black).
	CHECK(glsl.find("heFbm3(vWorldPos") != std::string::npos);
	CHECK(glsl.find("float heValueNoise3(vec3") != std::string::npos); // 3D helper injected
	CHECK(glsl.find("vec3(") != std::string::npos);           // grayscale RGB for a clean multiply
	CHECK(glsl.find("6.0") != std::string::npos);             // default Scale baked in
}

TEST_CASE("Reroute passes its input through unchanged (colour survives the dot)")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int col = g.addNode(MatNodeType::ConstColor);
	g.findNode(col)->p[0] = 0.9f; g.findNode(col)->p[1] = 0.1f; g.findNode(col)->p[2] = 0.2f;
	const int rr  = g.addNode(MatNodeType::Reroute);
	CHECK(g.connect(col, 0, rr, 0));
	CHECK(g.connect(rr, 0, out, 0));
	const std::string glsl = HE::generateFragment(g).glsl;
	// The colour literal must reach the output THROUGH the reroute (vec4 round-trip).
	CHECK(glsl.find("0.900000") != std::string::npos);
	CHECK(glsl.find("0.100000") != std::string::npos);
}

TEST_CASE("Material function with several NAMED inputs/outputs: pins + inlining by index")
{
	// f(Base, Amount) → Doubled = Base*Amount, Inverted = 1-Base
	MaterialGraph fn;
	const int inA = fn.addNode(MatNodeType::FnInput);
	fn.findNode(inA)->s = "Base";   fn.findNode(inA)->p[0] = 2.0f; // vec3
	const int inB = fn.addNode(MatNodeType::FnInput);
	fn.findNode(inB)->s = "Amount"; fn.findNode(inB)->p[0] = 0.0f; // float
	const int mul = fn.addNode(MatNodeType::Multiply);
	const int inv = fn.addNode(MatNodeType::OneMinus);
	const int o1  = fn.addNode(MatNodeType::FnOutput);
	fn.findNode(o1)->s = "Doubled";  fn.findNode(o1)->p[0] = 2.0f;
	const int o2  = fn.addNode(MatNodeType::FnOutput);
	fn.findNode(o2)->s = "Inverted"; fn.findNode(o2)->p[0] = 2.0f;
	CHECK(fn.connect(inA, 0, mul, 0));
	CHECK(fn.connect(inB, 0, mul, 1));
	CHECK(fn.connect(mul, 0, o1, 0));
	CHECK(fn.connect(inA, 0, inv, 0));
	CHECK(fn.connect(inv, 0, o2, 0));

	// The call node's pins carry the GIVEN names, in id order.
	std::vector<HE::MatPinDesc> ins, outs;
	HE::matFunctionPins(fn, ins, outs);
	REQUIRE(ins.size() == 2);
	REQUIRE(outs.size() == 2);
	CHECK(std::string(ins[0].name)  == "Base");
	CHECK(std::string(ins[1].name)  == "Amount");
	CHECK(std::string(outs[0].name) == "Doubled");
	CHECK(std::string(outs[1].name) == "Inverted");

	// Material: two distinct constants into the call; BOTH outputs consumed.
	MaterialGraph g;
	const int out  = g.addNode(MatNodeType::Output);
	const int col  = g.addNode(MatNodeType::ConstColor);
	g.findNode(col)->p[0] = 0.7f; g.findNode(col)->p[1] = 0.3f; g.findNode(col)->p[2] = 0.1f;
	const int amt  = g.addNode(MatNodeType::ConstFloat);
	g.findNode(amt)->p[0] = 0.25f;
	const int call = g.addNode(MatNodeType::FunctionCall);
	g.findNode(call)->s = "fns/multi.hasset";
	CHECK(g.connect(col,  0, call, 0)); // Base
	CHECK(g.connect(amt,  0, call, 1)); // Amount
	CHECK(g.connect(call, 0, out, 0));  // Doubled  → BaseColor
	CHECK(g.connect(call, 1, out, 1));  // Inverted → Metallic

	HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
	{ return path == "fns/multi.hasset" ? &fn : nullptr; };
	const std::string glsl = HE::generateFragment(g, loader).glsl;
	// Both input values land in the inlined body, and the inputs resolve BY INDEX:
	// Base gets the colour, Amount the scalar.
	CHECK(glsl.find("0.700000") != std::string::npos);
	CHECK(glsl.find("0.250000") != std::string::npos);
	// Both outputs produce distinct expressions (the OneMinus branch is inlined too).
	CHECK(glsl.find("vec3(1.0) - ") != std::string::npos);
}

TEST_CASE("Static Switch: untaken branch is CULLED; override map flips the permutation")
{
	// switch(True = red const, False = blue const) → BaseColor
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int sw  = g.addNode(MatNodeType::StaticSwitch); // default ON
	g.findNode(sw)->s = "UseRed";
	const int red = g.addNode(MatNodeType::ConstColor);
	g.findNode(red)->p[0] = 0.91f; g.findNode(red)->p[1] = 0.0f; g.findNode(red)->p[2] = 0.0f;
	const int blu = g.addNode(MatNodeType::ConstColor);
	g.findNode(blu)->p[0] = 0.0f; g.findNode(blu)->p[1] = 0.0f; g.findNode(blu)->p[2] = 0.87f;
	CHECK(g.connect(red, 0, sw, 0));
	CHECK(g.connect(blu, 0, sw, 1));
	CHECK(g.connect(sw,  0, out, 0));

	// Default (on): ONLY the red branch is in the shader — blue is culled entirely.
	const HE::MatShaderGen onGen = HE::generateFragment(g);
	CHECK(onGen.glsl.find("0.910000") != std::string::npos);
	CHECK(onGen.glsl.find("0.870000") == std::string::npos);
	REQUIRE(onGen.switches.size() == 1);
	CHECK(onGen.switches[0].first  == "UseRed");
	CHECK(onGen.switches[0].second == true);

	// Override map → the OTHER permutation, with a different source (its own hash).
	std::map<std::string, bool> ov{ { "UseRed", false } };
	const HE::MatShaderGen offGen = HE::generateFragment(g, {}, &ov);
	CHECK(offGen.glsl.find("0.870000") != std::string::npos);
	CHECK(offGen.glsl.find("0.910000") == std::string::npos);
	CHECK(offGen.switches[0].second == false);
	CHECK(onGen.glsl != offGen.glsl);
}

TEST_CASE("Param metadata (range/group/tooltip) flows into slots and survives JSON")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int pf  = g.addNode(MatNodeType::ParamFloat);
	HE::MatGraphNode* n = g.findNode(pf);
	n->s = "Roughness"; n->p[0] = 0.4f;
	n->p[1] = 0.0f; n->p[2] = 1.0f;         // slider range
	n->group = "Surface"; n->tooltip = "0 = mirror, 1 = chalk";
	CHECK(g.connect(pf, 0, out, HE::kMatOutputRoughnessPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE(gen.params.size() == 1);
	CHECK(gen.params[0].minV == doctest::Approx(0.0f));
	CHECK(gen.params[0].maxV == doctest::Approx(1.0f));
	CHECK(gen.params[0].group   == "Surface");
	CHECK(gen.params[0].tooltip == "0 = mirror, 1 = chalk");

	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), r));
	bool found = false;
	for (const auto& rn : r.nodes)
		if (rn.type == MatNodeType::ParamFloat)
		{ found = rn.group == "Surface" && rn.tooltip == "0 = mirror, 1 = chalk"; break; }
	CHECK(found);
}

TEST_CASE("Blend modes: opaque forces alpha 1, masked discards, translucent feeds alpha")
{
	auto makeG = [](float mode) {
		MaterialGraph g;
		const int out = g.addNode(MatNodeType::Output);
		g.findNode(out)->p[1] = mode;
		g.findNode(out)->p[2] = 0.33f; // mask cutoff
		const int a = g.addNode(MatNodeType::ConstFloat);
		g.findNode(a)->p[0] = 0.42f;
		g.connect(a, 0, out, HE::kMatOutputOpacityPin);
		return g;
	};
	const std::string opq = HE::generateFragment(makeG(0.0f)).glsl;
	CHECK(opq.find("discard") == std::string::npos);
	CHECK(opq.find(", 1.0);") != std::string::npos);      // alpha forced solid
	CHECK(opq.find("0.420000") == std::string::npos);     // pin 4 ignored → subtree culled

	const HE::MatShaderGen msk = HE::generateFragment(makeG(1.0f));
	CHECK(msk.glsl.find("discard") != std::string::npos);
	CHECK(msk.glsl.find("< 0.330000") != std::string::npos); // authored cutoff
	CHECK(msk.blendMode == 1);

	const HE::MatShaderGen trn = HE::generateFragment(makeG(2.0f));
	CHECK(trn.glsl.find("discard") == std::string::npos);
	CHECK(trn.glsl.find("0.420000") != std::string::npos);   // opacity reaches alpha
	CHECK(trn.blendMode == 2);
}

TEST_CASE("Normal pin replaces vNormal in heLit; Normal Map emits the perturb helper")
{
	// Unconnected → the interpolated vertex normal.
	MaterialGraph g0 = MaterialGraph::makeDefault();
	CHECK(HE::generateFragmentGlsl(g0).find("vec3 heN = normalize(vNormal);")
	      != std::string::npos);

	// Normal Map → hePerturbNormal + screen-space TBN helper, fed into heLit via heN.
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int nm  = g.addNode(MatNodeType::NormalMapSample);
	CHECK(g.connect(nm, 0, out, HE::kMatOutputNormalPin)); // Normal pin
	const std::string glsl = HE::generateFragment(g).glsl;
	CHECK(glsl.find("vec3 hePerturbNormal(") != std::string::npos);
	CHECK(glsl.find("dFdx(") != std::string::npos);
	CHECK(glsl.find("heLitP(") != std::string::npos);
	CHECK(glsl.find("heN") != std::string::npos);
	CHECK(glsl.find("normalize(vNormal);") == std::string::npos); // replaced by the map
}

TEST_CASE("WPO pin generates a vertex body; its statements leave the fragment")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	int out = 0;
	for (auto& n : g.nodes) if (n.type == MatNodeType::Output) out = n.id;
	// Wind-ish offset: sin(time) into Combine3 → WPO. The 0.777 constant is a tracer.
	const int t   = g.addNode(MatNodeType::Time);
	const int sn  = g.addNode(MatNodeType::Sine);
	const int amp = g.addNode(MatNodeType::ConstFloat);
	g.findNode(amp)->p[0] = 0.777f;
	const int mul = g.addNode(MatNodeType::Multiply);
	const int cmb = g.addNode(MatNodeType::Combine3);
	CHECK(g.connect(t,   0, sn,  0));
	CHECK(g.connect(sn,  0, mul, 0));
	CHECK(g.connect(amp, 0, mul, 1));
	CHECK(g.connect(mul, 0, cmb, 0)); // offset in X
	CHECK(g.connect(cmb, 0, out, HE::kMatOutputWPOPin)); // WPO pin

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE(!gen.vertexBody.empty());
	CHECK(gen.vertexBody.find("vec3 heWpo") != std::string::npos);
	CHECK(gen.vertexBody.find("0.777000") != std::string::npos); // tracer in the VS body…
	CHECK(gen.glsl.find("0.777000") == std::string::npos);       // …and NOT in the fragment
}

TEST_CASE("Comment boxes round-trip through graph JSON (and old JSON still loads)")
{
	MaterialGraph g = MaterialGraph::makeDefault();
	HE::MatGraphComment cb;
	cb.id = g.nextId++;
	cb.text = "shading section";
	cb.x = -40.0f; cb.y = 12.5f; cb.w = 300.0f; cb.h = 210.0f;
	g.comments.push_back(cb);

	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), r));
	REQUIRE(r.comments.size() == 1);
	CHECK(r.comments[0].text == "shading section");
	CHECK(r.comments[0].x == doctest::Approx(-40.0f));
	CHECK(r.comments[0].w == doctest::Approx(300.0f));
	CHECK(r.nextId > r.comments[0].id); // id space shared with nodes

	// A pre-comment JSON (no "comments" key) must still parse to an empty list.
	MaterialGraph old;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(MaterialGraph::makeDefault()), old));
	CHECK(old.comments.empty());
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("Every standard node cross-compiles with all inputs wired")
{
	// Wire each node's inputs from a source and its output into the material Output,
	// then cross-compile. A pin/emit mismatch produces malformed GLSL that fails here.
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	for (const HE::MatNodeDesc& d : HE::matNodeRegistry())
	{
		// Output is the sink; the function-interface + call nodes need a function graph.
		if (d.type == MatNodeType::Output || d.type == MatNodeType::FnInput ||
		    d.type == MatNodeType::FnOutput || d.type == MatNodeType::FunctionCall)
			continue;

		MaterialGraph g;
		const int out = g.addNode(MatNodeType::Output);
		const int n   = g.addNode(d.type);
		// Feed every input pin from a fresh source node (ConstColor → coerces to any type).
		for (size_t i = 0; i < d.inputs.size(); ++i)
		{
			const int src = g.addNode(MatNodeType::ConstColor);
			CHECK(g.connect(src, 0, n, (int)i));
		}
		// Route the node's first output into BaseColor (or Metallic for a sink-less node).
		if (!d.outputs.empty())
			CHECK(g.connect(n, 0, out, 0));

		const std::string glsl = HE::generateFragment(g).glsl;
		const uint64_t hash = std::hash<std::string>{}(glsl) ^ (uint64_t)d.type;
		const auto& msl = lib.fragment(hash, glsl, B::Metal);
		CHECK_MESSAGE(msl.ok, "MSL compile failed for node '", d.name, "': ", msl.log);
		const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
		CHECK_MESSAGE(gl.ok, "GLSL compile failed for node '", d.name, "': ", gl.log);
	}
}

TEST_CASE("Decal shaders cross-compile for every backend")
{
	// docs/decals-cross-backend-plan.md §7 gate 2. The vertex stage is backend-
	// agnostic; the SAMPLED fragment is what the GL/Vulkan/D3D ports use, so it
	// has to survive all four emitters. The framebuffer-fetch fragment is Metal-
	// only by construction (subpassInput → [[color(3)]]).
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	for (B b : { B::Metal, B::GLSL410, B::HLSL, B::SpirV })
	{
		const auto& v = lib.decalVertex(b);
		CHECK_MESSAGE(v.ok, "decal vertex failed for backend ", (int)b, ": ", v.log);
		const auto& f = lib.decalFragmentSampled(b);
		CHECK_MESSAGE(f.ok, "sampled decal fragment failed for backend ", (int)b, ": ", f.log);
		// The forward variant is what Vulkan/D3D11/D3D12 draw with; it carries
		// ddx/ddy and faceforward, which not every emitter spells the same way.
		const auto& fw = lib.decalFragmentForward(b);
		CHECK_MESSAGE(fw.ok, "forward decal fragment failed for backend ", (int)b, ": ", fw.log);
	}
	const auto& fetch = lib.decalFragment(B::Metal);
	CHECK_MESSAGE(fetch.ok, fetch.log);

	// The cache key must separate the fragment variants (it used to be
	// backend*2 + stage, which collided the moment a second fragment appeared).
	CHECK(&lib.decalFragment(B::Metal) != &lib.decalFragmentSampled(B::Metal));
	CHECK(lib.decalFragment(B::Metal).source != lib.decalFragmentSampled(B::Metal).source);
	CHECK(lib.decalFragmentForward(B::Metal).source != lib.decalFragmentSampled(B::Metal).source);

	// The forward variant must contain NO discard: its normal comes from
	// derivatives, which are undefined in non-uniform control flow. Coverage is
	// folded into the alpha instead. Checked on the GLSL emitter, which keeps
	// the keyword verbatim.
	const std::string fwGlsl = lib.decalFragmentForward(B::GLSL410).source;
	CHECK(fwGlsl.find("discard") == std::string::npos);
	CHECK(lib.decalFragmentSampled(B::GLSL410).source.find("discard") != std::string::npos);

	// Both stages declare the SAME HeDecal block — a member mismatch is a LINK
	// error on GL, which no test without a context can catch. Compare the block
	// text the emitter produced for each stage.
	auto block = [](const std::string& src) -> std::string {
		const size_t b = src.find("HeDecal");
		if (b == std::string::npos) return {};
		const size_t e = src.find('}', b);
		if (e == std::string::npos) return {};
		return src.substr(b, e - b);
	};
	const std::string vBlock = block(lib.decalVertex(B::GLSL410).source);
	CHECK_FALSE(vBlock.empty());
	CHECK(vBlock == block(lib.decalFragmentSampled(B::GLSL410).source));
	CHECK(vBlock == block(fwGlsl));
}

TEST_CASE("Decal HLSL registers stay inside D3D11's bindable range")
{
	// docs/decals-cross-backend-plan.md §6b. SPIRV-Cross turns layout(binding = N)
	// into register(bN/tN/sN) one-for-one, and the canonical decal bindings are
	// 19/22/23 — past D3D11's hard limits of 14 constant-buffer and 16 sampler
	// slots per stage. A shader emitted that way cannot be bound at all, and no
	// D3D11 header exists on the build machine to catch it, so the pins are
	// checked here on the emitted text. This is the ONLY automatic net under the
	// D3D11 decal pass; the numbers are the contract D3D12 inherits.
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;

	const std::string vs = lib.decalVertex(B::HLSL).source;
	CHECK(vs.find("register(b13)") != std::string::npos);
	CHECK(vs.find("register(b23)") == std::string::npos);
	CHECK(vs.find("SV_VertexID")   != std::string::npos); // bufferless cube, no VB

	for (const std::string& ps : { lib.decalFragmentSampled(B::HLSL).source,
	                               lib.decalFragmentForward(B::HLSL).source })
	{
		CHECK(ps.find("register(b13)") != std::string::npos); // HeDecal
		CHECK(ps.find("register(t14)") != std::string::npos); // heDecalTex
		CHECK(ps.find("register(s14)") != std::string::npos);
		CHECK(ps.find("register(t15)") != std::string::npos); // heGBDepth
		CHECK(ps.find("register(s15)") != std::string::npos);
		// The canonical numbers must be gone, not merely joined by the pinned ones.
		CHECK(ps.find("register(b23)") == std::string::npos);
		CHECK(ps.find("register(s19)") == std::string::npos);
		CHECK(ps.find("register(s22)") == std::string::npos);
	}

	// Pinning must not change what the shader computes: the GLSL emitter is the
	// unpinned reference, and both still carry the same box clip and the same
	// coverage rule.
	CHECK(lib.decalFragmentForward(B::HLSL).source.find("discard") == std::string::npos);
	CHECK(lib.decalFragmentSampled(B::HLSL).source.find("discard") != std::string::npos);
}

TEST_CASE("v5 graph (logic + If + new params) cross-compiles for Metal and GL")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int pv2 = g.addNode(MatNodeType::ParamVec2); g.findNode(pv2)->s = "Tiling";
	const int pv4 = g.addNode(MatNodeType::ParamVec4); g.findNode(pv4)->s = "Tint";
	const int pb  = g.addNode(MatNodeType::ParamBool);  g.findNode(pb)->s = "Toggle";
	const int cb  = g.addNode(MatNodeType::ConstBool);
	const int orr = g.addNode(MatNodeType::Or);
	const int ge  = g.addNode(MatNodeType::GreaterEqual);
	const int eq  = g.addNode(MatNodeType::Equal);
	const int iff = g.addNode(MatNodeType::If);
	const int blue= g.addNode(MatNodeType::ConstColor); g.findNode(blue)->p[2] = 1.0f;
	g.connect(pv2, 0, ge, 0);          // vec2 coerced to float for compare
	g.connect(cb,  0, ge, 1);
	g.connect(pb,  0, orr, 0);
	g.connect(ge,  0, orr, 1);
	g.connect(orr, 0, iff, 0);         // Cond
	g.connect(pv4, 0, iff, 1);         // True (vec4 → vec3)
	g.connect(blue,0, iff, 2);         // False
	g.connect(iff, 0, out, 0);         // BaseColor
	g.connect(eq,  0, out, HE::kMatOutputOpacityPin); // Opacity from Equal (unconnected inputs → defaults)
	const std::string glsl = HE::generateFragment(g).glsl;
	const uint64_t hash = std::hash<std::string>{}(glsl);
	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, msl.log);
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, gl.log);
}

TEST_CASE("v4 graph (project textures + new inputs) cross-compiles for Metal and GL")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int ts  = g.addNode(MatNodeType::TextureSample);
	g.findNode(ts)->s = "Tex/a.hasset";
	const int uv  = g.addNode(MatNodeType::UV);
	const int cd  = g.addNode(MatNodeType::CameraDistance);
	CHECK(g.connect(uv, 0, ts,  0));
	CHECK(g.connect(ts, 0, out, 0));
	CHECK(g.connect(cd, 0, out, 1));
	const std::string glsl = HE::generateFragment(g).glsl;
	const uint64_t hash = std::hash<std::string>{}(glsl);
	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, msl.log);
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, gl.log);
}

TEST_CASE("v3 graph (params + function call + alpha) cross-compiles for Metal and GL")
{
	MaterialGraph fn;
	const int fin  = fn.addNode(MatNodeType::FnInput);
	const int fout = fn.addNode(MatNodeType::FnOutput);
	CHECK(fn.connect(fin, 0, fout, 0));

	MaterialGraph g;
	const int out  = g.addNode(MatNodeType::Output);
	const int pc   = g.addNode(MatNodeType::ParamColor);
	g.findNode(pc)->s = "Base";
	const int call = g.addNode(MatNodeType::FunctionCall);
	g.findNode(call)->s = "F.hasset";
	const int half = g.addNode(MatNodeType::ConstFloat);
	g.findNode(half)->p[0] = 0.5f;
	CHECK(g.connect(pc,   0, call, 0));
	CHECK(g.connect(call, 0, out,  0));
	CHECK(g.connect(half, 0, out,  4)); // opacity

	HE::MatFunctionLoader loader = [&](const std::string&) -> const MaterialGraph* { return &fn; };
	const std::string glsl = HE::generateFragment(g, loader).glsl;
	const uint64_t hash = std::hash<std::string>{}(glsl);
	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, msl.log);
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, gl.log);
	CHECK(gl.source.find("readonly buffer") == std::string::npos);
}

TEST_CASE("v2 graph GLSL (noise + fresnel + panner) cross-compiles for Metal and GL")
{
	MaterialGraph g;
	const int out   = g.addNode(MatNodeType::Output);
	const int uv    = g.addNode(MatNodeType::UV);
	const int pan   = g.addNode(MatNodeType::Panner);
	const int fbm   = g.addNode(MatNodeType::Fbm);
	const int fres  = g.addNode(MatNodeType::Fresnel);
	const int comb  = g.addNode(MatNodeType::Combine3);
	CHECK(g.connect(uv,   0, pan,  0));
	CHECK(g.connect(pan,  0, fbm,  0));
	CHECK(g.connect(fbm,  0, comb, 0));
	CHECK(g.connect(fres, 0, comb, 1));
	CHECK(g.connect(comb, 0, out,  0));
	const std::string glsl = HE::generateFragmentGlsl(g);
	const uint64_t hash = std::hash<std::string>{}(glsl);

	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, msl.log);
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, gl.log);
	CHECK(gl.source.find("readonly buffer") == std::string::npos);
	// The extended standard vertices (vWorldPos) still compile.
	CHECK(lib.standardVertex(B::Metal).ok);
	CHECK(lib.standardVertex(B::GLSL410).ok);
}

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

// ── Precompiled-shader (PSHD) byte layout ───────────────────────────────────
// encode/decode is the single source of truth shared by the exporter and the
// runtime; a roundtrip must preserve backend + both (possibly binary) sources.
#include <ContentManager/Assets.h>

TEST_CASE("PSHD encode/decode roundtrip preserves variants")
{
	std::vector<MaterialShaderVariant> in;
	{
		MaterialShaderVariant m;
		m.backend  = static_cast<uint8_t>(HE::RendererBackend::Metal);
		m.vertex   = "vertex float4 v_main() { return 0; }";
		m.fragment = "fragment float4 f_main() { return 1; }";
		in.push_back(m);
	}
	{
		// SpirV path: fragment carries raw bytes, including embedded NULs.
		MaterialShaderVariant v;
		v.backend  = static_cast<uint8_t>(HE::RendererBackend::Vulkan);
		v.vertex   = std::string("\x03\x02\x23\x07\x00\x00\x01\x00", 8);
		v.fragment = std::string("\x00\xDE\xAD\x00\xBE\xEF\x00", 7);
		in.push_back(v);
	}

	const std::vector<uint8_t> bytes = HE::encodeMaterialShaderVariants(in);
	CHECK(!bytes.empty());

	const std::vector<MaterialShaderVariant> out = HE::decodeMaterialShaderVariants(bytes);
	REQUIRE(out.size() == in.size());
	for (size_t i = 0; i < in.size(); ++i)
	{
		CHECK(out[i].backend  == in[i].backend);
		CHECK(out[i].vertex   == in[i].vertex);   // std::string ==, NUL-safe
		CHECK(out[i].fragment == in[i].fragment);
	}

	// Empty input → empty blob → empty decode (exporter treats this as "no chunk").
	CHECK(HE::encodeMaterialShaderVariants({}).empty());
	CHECK(HE::decodeMaterialShaderVariants({}).empty());
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("UI quad vertex cross-compiles for Metal and GL")
{
	// The screen-space vertex that pairs material fragments with in-game UI
	// quads (MaterialShaderLibrary::uiVertex). A varying/binding mismatch or
	// gl_VertexIndex misuse would fail cross-compilation here.
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;

	const auto& msl = lib.uiVertex(B::Metal);
	CHECK_MESSAGE(msl.ok, "uiVertex MSL compile failed: ", msl.log);
	CHECK(!msl.source.empty());

	const auto& gl = lib.uiVertex(B::GLSL410);
	CHECK_MESSAGE(gl.ok, "uiVertex GLSL410 compile failed: ", gl.log);
	CHECK(!gl.source.empty());

	// A material fragment still cross-compiles alongside it (shared varyings).
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int c   = g.addNode(MatNodeType::ConstColor);
	CHECK(g.connect(c, 0, out, 0));
	const std::string glsl = HE::generateFragment(g).glsl;
	const auto& frag = lib.fragment(std::hash<std::string>{}(glsl), glsl, B::Metal);
	CHECK_MESSAGE(frag.ok, "companion fragment failed: ", frag.log);
}
#endif

// ── UV tiling / offset ────────────────────────────────────────────────────────
// The UV node was a bare `vUV` passthrough: a texture could only ever stretch
// once across a mesh, never tile.
TEST_CASE("UV node emits tiling and offset")
{
    HE::MaterialGraph g;
    const int out = g.addNode(HE::MatNodeType::Output);
    const int uv  = g.addNode(HE::MatNodeType::UV);

    // A fresh UV node is the identity, and emits the bare varying (so existing
    // generated shaders stay byte-identical).
    CHECK(g.findNode(uv)->p[0] == doctest::Approx(1.0f));
    CHECK(g.findNode(uv)->p[1] == doctest::Approx(1.0f));
    const int tex = g.addNode(HE::MatNodeType::TextureSample);
    g.connect(uv, 0, tex, 0);
    g.connect(tex, 0, out, 0);
    {
        const std::string src = HE::generateFragmentGlsl(g);
        CHECK(src.find("= vUV;") != std::string::npos);
        CHECK(src.find("vUV * vec2(") == std::string::npos);
    }

    // Tiling/offset are baked into the generated expression.
    g.findNode(uv)->p[0] = 4.0f; g.findNode(uv)->p[1] = 8.0f;
    g.findNode(uv)->p[2] = 0.5f; g.findNode(uv)->p[3] = 0.25f;
    {
        const std::string src = HE::generateFragmentGlsl(g);
        CHECK(src.find("vUV * vec2(4.000000, 8.000000)") != std::string::npos);
        CHECK(src.find("vec2(0.500000, 0.250000)") != std::string::npos);
    }
}

TEST_CASE("legacy UV nodes load as the identity, not as zero tiling")
{
    // Graphs authored before the UV node had params stored p = {0,0,0,0}.
    // Reading that literally would multiply every UV by zero.
    const std::string legacy =
        R"({"nextId":3,"nodes":[{"id":1,"type":"UV","p":[0,0,0,0],"x":0,"y":0}],"links":[]})";
    HE::MaterialGraph g;
    REQUIRE(HE::materialGraphFromJson(legacy, g));
    REQUIRE(g.nodes.size() == 1);
    CHECK(g.nodes[0].p[0] == doctest::Approx(1.0f));
    CHECK(g.nodes[0].p[1] == doctest::Approx(1.0f));
    CHECK(g.nodes[0].p[2] == doctest::Approx(0.0f));
    CHECK(g.nodes[0].p[3] == doctest::Approx(0.0f));
}

// ── Output-pin layout v1 → v2 ─────────────────────────────────────────────────
// v2 inserted Specular at pin 2 and Ambient Occlusion at pin 7. Links are stored
// by pin INDEX, so a v1 graph read as-is would rewire Roughness→Specular,
// Emissive→Roughness, Opacity→Emissive … — every saved material would change.
TEST_CASE("v1 material graphs remap their Output links to the v2 pin layout")
{
	// v1 Output pins: 0 BaseColor, 1 Metallic, 2 Roughness, 3 Emissive,
	//                 4 Opacity, 5 Normal, 6 WPO.
	// R"JSON(...)JSON": the payload contains "Normal (WS)" — a plain R"(...)"
	// would end at that closing paren.
	const std::string v1 = R"JSON({
      "version": 1, "nextId": 9,
      "nodes": [
        { "id": 1, "type": "Output", "p": [1,2,0.5,0], "x": 0, "y": 0 },
        { "id": 2, "type": "Float",  "p": [0.25,0,0,0], "x": -200, "y": 0 },
        { "id": 3, "type": "Float",  "p": [0.75,0,0,0], "x": -200, "y": 60 },
        { "id": 4, "type": "Color",  "p": [1,0,0,0],    "x": -200, "y": 120 },
        { "id": 5, "type": "Float",  "p": [0.4,0,0,0],  "x": -200, "y": 180 },
        { "id": 6, "type": "Normal (WS)", "p": [0,0,0,0], "x": -200, "y": 240 },
        { "id": 7, "type": "World Position", "p": [0,0,0,0], "x": -200, "y": 300 }
      ],
      "links": [
        { "sn": 2, "sp": 0, "dn": 1, "dp": 2 },
        { "sn": 3, "sp": 0, "dn": 1, "dp": 1 },
        { "sn": 4, "sp": 0, "dn": 1, "dp": 3 },
        { "sn": 5, "sp": 0, "dn": 1, "dp": 4 },
        { "sn": 6, "sp": 0, "dn": 1, "dp": 5 },
        { "sn": 7, "sp": 0, "dn": 1, "dp": 6 }
      ]})JSON";

	HE::MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(v1, g));
	auto dstPinOf = [&](int srcNode) {
		for (const auto& l : g.links) if (l.srcNode == srcNode) return l.dstPin;
		return -1;
	};
	CHECK(dstPinOf(2) == HE::kMatOutputRoughnessPin); // was 2
	CHECK(dstPinOf(3) == HE::kMatOutputMetallicPin);  // was 1 (unchanged)
	CHECK(dstPinOf(4) == HE::kMatOutputEmissivePin);  // was 3
	CHECK(dstPinOf(5) == HE::kMatOutputOpacityPin);   // was 4
	CHECK(dstPinOf(6) == HE::kMatOutputNormalPin);    // was 5
	CHECK(dstPinOf(7) == HE::kMatOutputWPOPin);       // was 6
	// Nothing landed on the two NEW pins.
	for (const auto& l : g.links)
	{
		CHECK(l.dstPin != HE::kMatOutputSpecularPin);
		CHECK(l.dstPin != HE::kMatOutputAOPin);
	}

	// Re-saving stamps v2, and a v2 round-trip must be a no-op.
	const std::string v2 = HE::materialGraphToJson(g);
	CHECK(v2.find("\"version\":2") != std::string::npos);
	HE::MaterialGraph g2;
	REQUIRE(HE::materialGraphFromJson(v2, g2));
	REQUIRE(g2.links.size() == g.links.size());
	for (size_t i = 0; i < g.links.size(); ++i)
	{
		CHECK(g2.links[i].dstPin  == g.links[i].dstPin);
		CHECK(g2.links[i].srcNode == g.links[i].srcNode);
	}
}


// Argument list of the first `heLitP(` call, with nested parentheses respected
// (BaseColor is often a `vec3(...)` expression, so "up to the next )" is wrong).
static std::string heLitPArgs(const std::string& glsl)
{
	const size_t call = glsl.find("heLitP(");
	if (call == std::string::npos) return {};
	size_t i = call + 7;
	int depth = 1;
	for (; i < glsl.size() && depth > 0; ++i)
	{
		if (glsl[i] == '(') ++depth;
		else if (glsl[i] == ')') --depth;
	}
	return glsl.substr(call + 7, (i - 1) - (call + 7));
}

// Commas at the TOP level of an argument list (nested calls have their own).
static int topLevelCommas(const std::string& args)
{
	int depth = 0, n = 0;
	for (char c : args)
	{
		if (c == '(') ++depth;
		else if (c == ')') --depth;
		else if (c == ',' && depth == 0) ++n;
	}
	return n;
}

TEST_CASE("Specular and Ambient Occlusion reach the lit shading call")
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output);
	const int spec = g.addNode(HE::MatNodeType::ConstFloat);
	g.findNode(spec)->p[0] = 0.9f;
	const int ao = g.addNode(HE::MatNodeType::ConstFloat);
	g.findNode(ao)->p[0] = 0.3f;
	CHECK(g.connect(spec, 0, out, HE::kMatOutputSpecularPin));
	CHECK(g.connect(ao,   0, out, HE::kMatOutputAOPin));

	const std::string glsl = HE::generateFragmentGlsl(g);
	// Constants become locals, so assert on the CALL SHAPE: heLitP now takes 7
	// arguments and the last two carry the Specular / AO expressions.
	const std::string args = heLitPArgs(glsl);
	REQUIRE_FALSE(args.empty());
	CHECK(topLevelCommas(args) == 6); // 7 arguments
	// Both constants were emitted, and the values reached the shader.
	CHECK(glsl.find("= 0.900000;") != std::string::npos);
	CHECK(glsl.find("= 0.300000;") != std::string::npos);

	// Unconnected → the defaults that reproduce the previous behaviour exactly.
	HE::MaterialGraph plain;
	plain.addNode(HE::MatNodeType::Output);
	const std::string base = HE::generateFragmentGlsl(plain);
	const std::string a2 = heLitPArgs(base);
	REQUIRE_FALSE(a2.empty());
	CHECK(topLevelCommas(a2) == 6);
	CHECK(a2.find("0.500000") != std::string::npos); // Specular default
	CHECK(a2.find("1.000000") != std::string::npos); // AO default
}

// ── Landscape layer blend ─────────────────────────────────────────────────────
TEST_CASE("matLandscapeLayerNames splits, trims and caps the layer list")
{
	CHECK(HE::matLandscapeLayerNames("Grass\nRock") == std::vector<std::string>{ "Grass", "Rock" });
	CHECK(HE::matLandscapeLayerNames("  Grass \n\n\t Rock  ")
	      == std::vector<std::string>{ "Grass", "Rock" });
	// Never empty — a node with no names still has one pin.
	CHECK(HE::matLandscapeLayerNames("").size() == 1);
	// One RGBA weightmap = four channels, so the list is capped.
	CHECK(HE::matLandscapeLayerNames("a\nb\nc\nd\ne\nf").size()
	      == static_cast<size_t>(HE::kMatMaxLandscapeLayers));
}

TEST_CASE("Landscape Layer Blend emits a normalised weightmap blend")
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output);
	const int lb  = g.addNode(HE::MatNodeType::LandscapeLayerBlend);
	g.findNode(lb)->s = "Grass\nRock\nSand";
	const int c0 = g.addNode(HE::MatNodeType::ConstColor);
	g.findNode(c0)->p[0] = 0.1f; g.findNode(c0)->p[1] = 0.6f; g.findNode(c0)->p[2] = 0.2f;
	CHECK(g.connect(c0, 0, lb, 0));
	CHECK(g.connect(lb, 0, out, HE::kMatOutputBaseColorPin));
	// A pin beyond the declared layers must be rejected (dynamic pin count).
	CHECK_FALSE(g.connect(c0, 0, lb, 3));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	// The sampler is declared at the reserved binding and sampled at the RAW UV
	// (the weightmap spans the whole terrain; detail tiling is per layer).
	CHECK(gen.glsl.find("binding = 14) uniform sampler2D heLandscapeWeights") != std::string::npos);
	CHECK(gen.glsl.find("texture(heLandscapeWeights, vUV)") != std::string::npos);
	// Normalised by the weight sum, so a partly painted texel doesn't darken…
	CHECK(gen.glsl.find("1e-4") != std::string::npos);
	// …and a texel with NO weight at all falls back to layer 0 instead of
	// dividing zero by the floor and coming out black (which is what a region
	// painted with a since-removed layer used to do).
	const size_t tern = gen.glsl.find("> 1e-4 ? (");
	REQUIRE(tern != std::string::npos);
	const size_t colon = gen.glsl.find(" : ", tern);
	REQUIRE(colon != std::string::npos);
	const std::string elseExpr =
		gen.glsl.substr(colon + 3, gen.glsl.find(';', colon) - colon - 3);
	// …and that fallback is exactly what layer 0 contributes to the blend.
	CHECK(gen.glsl.find("(" + elseExpr + " * ") != std::string::npos);
	// Exactly the three declared layers are advertised to the landscape tool.
	REQUIRE(gen.layerNames.size() == 3);
	CHECK(gen.layerNames[0] == "Grass");
	CHECK(gen.layerNames[1] == "Rock");
	CHECK(gen.layerNames[2] == "Sand");

	// An ordinary material declares no layers and no weightmap sampler.
	HE::MaterialGraph plain;
	plain.addNode(HE::MatNodeType::Output);
	const HE::MatShaderGen pg = HE::generateFragment(plain);
	CHECK(pg.layerNames.empty());
	CHECK(pg.glsl.find("heLandscapeWeights") == std::string::npos);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("A wired Landscape Layer Blend cross-compiles for Metal and GL")
{
	// The registry sweep above builds this node with NO layer wired; the
	// zero-weight fallback only takes its real shape (layer 0's expression in the
	// ternary's else branch) once a layer is connected. Both backends compile the
	// same canonical GLSL through glslang, so this is what proves the OpenGL
	// variant of the blend as well.
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output);
	const int lb  = g.addNode(HE::MatNodeType::LandscapeLayerBlend);
	g.findNode(lb)->s = "Grass\nRock\nSand";
	for (int i = 0; i < 3; ++i)
	{
		const int c = g.addNode(HE::MatNodeType::ConstColor);
		g.findNode(c)->p[i] = 1.0f;
		CHECK(g.connect(c, 0, lb, i));
	}
	CHECK(g.connect(lb, 0, out, HE::kMatOutputBaseColorPin));

	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(g).glsl;
	const uint64_t    hash = std::hash<std::string>{}(glsl);
	const auto& msl = lib.fragment(hash, glsl, B::Metal);
	CHECK_MESSAGE(msl.ok, "MSL compile failed: ", msl.log);
	const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
	CHECK_MESSAGE(gl.ok, "GLSL compile failed: ", gl.log);
}
#endif

// ═══ Container semantics shared with the other graph systems ═════════════════

TEST_CASE("MaterialGraph::connect REPLACES an existing link on the same input pin")
{
	// An input pin holds at most one link — the rule every graph system in the
	// engine follows (see GraphCommon/GraphModel.h). Wiring an occupied input
	// must not leave two wires racing for the same pin.
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int a   = g.addNode(MatNodeType::ConstColor);
	const int b   = g.addNode(MatNodeType::ConstColor);

	CHECK(g.connect(a, 0, out, HE::kMatOutputBaseColorPin));
	CHECK(g.links.size() == 1);
	CHECK(g.connect(b, 0, out, HE::kMatOutputBaseColorPin));
	REQUIRE(g.links.size() == 1);
	CHECK(g.links[0].srcNode == b);

	// The OUTPUT side is free to fan out to many inputs.
	CHECK(g.connect(b, 0, out, HE::kMatOutputEmissivePin));
	CHECK(g.links.size() == 2);
}

TEST_CASE("MaterialGraph::removeNode drops every link touching the node")
{
	MaterialGraph g;
	const int out  = g.addNode(MatNodeType::Output);
	const int col  = g.addNode(MatNodeType::ConstColor);
	const int lerp = g.addNode(MatNodeType::Lerp);
	CHECK(g.connect(col,  0, lerp, 0));
	CHECK(g.connect(lerp, 0, out,  HE::kMatOutputBaseColorPin));
	REQUIRE(g.links.size() == 2);

	g.removeNode(lerp);                 // links on BOTH sides go with it
	CHECK(g.findNode(lerp) == nullptr);
	CHECK(g.links.empty());
}

TEST_CASE("MaterialGraph never reuses a node id after removeNode")
{
	MaterialGraph g;
	const int a = g.addNode(MatNodeType::ConstFloat);
	const int b = g.addNode(MatNodeType::ConstFloat);
	g.removeNode(a);
	g.removeNode(b);
	const int c = g.addNode(MatNodeType::ConstFloat);
	CHECK(c != a);
	CHECK(c != b);
	CHECK(c > b);
}

// ═══ Parameter budget ════════════════════════════════════════════════════════

TEST_CASE("More than kMatMaxParams parameters never index past the HeParams array")
{
	// REGRESSION: the layout used to be clamped only AFTER the shader text was
	// built, so an over-budget graph emitted heParams.v[16] (and up) against a
	// `vec4 v[16]` declaration — an out-of-bounds read in the generated shader.
	// The budget is now enforced before emission: surplus params bake their
	// authored default as a literal instead.
	// 20 distinct named float params, chained through Adds so every one of them
	// is actually reached (and therefore slotted) during emission.
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	int acc = -1;
	for (int i = 0; i < 20; ++i)
	{
		const int p = g.addNode(MatNodeType::ParamFloat);
		g.findNode(p)->s    = "P" + std::to_string(i);
		g.findNode(p)->p[0] = 0.25f + (float)i; // authored default, baked when over budget
		if (acc < 0) { acc = p; continue; }
		const int a = g.addNode(MatNodeType::Add);
		CHECK(g.connect(acc, 0, a, 0));
		CHECK(g.connect(p,   0, a, 1));
		acc = a;
	}
	CHECK(g.connect(acc, 0, out, HE::kMatOutputBaseColorPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	CHECK((int)gen.params.size() == HE::kMatMaxParams);

	// No slot index at or beyond the array length appears in the emitted GLSL.
	for (int i = HE::kMatMaxParams; i < 32; ++i)
	{
		const std::string oob = "heParams.v[" + std::to_string(i) + "]";
		CHECK(gen.glsl.find(oob) == std::string::npos);
	}
	// The last in-budget slot IS used, so the test is actually exercising the cap.
	CHECK(gen.glsl.find("heParams.v[" + std::to_string(HE::kMatMaxParams - 1) + "]")
	      != std::string::npos);
	// And the declared array is exactly kMatMaxParams long.
	CHECK(gen.glsl.find("vec4 v[" + std::to_string(HE::kMatMaxParams) + "]")
	      != std::string::npos);
	// Surplus params are baked: their node comment survives without a UBO read.
	CHECK(gen.glsl.find("// param: P19") != std::string::npos);
}

TEST_CASE("Exactly kMatMaxParams parameters still all get real UBO slots")
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	int acc = -1;
	for (int i = 0; i < HE::kMatMaxParams; ++i)
	{
		const int p = g.addNode(MatNodeType::ParamFloat);
		g.findNode(p)->s = "Q" + std::to_string(i);
		if (acc < 0) { acc = p; continue; }
		const int a = g.addNode(MatNodeType::Add);
		CHECK(g.connect(acc, 0, a, 0));
		CHECK(g.connect(p,   0, a, 1));
		acc = a;
	}
	CHECK(g.connect(acc, 0, out, HE::kMatOutputBaseColorPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	CHECK((int)gen.params.size() == HE::kMatMaxParams);
	for (int i = 0; i < HE::kMatMaxParams; ++i)
		CHECK(gen.glsl.find("heParams.v[" + std::to_string(i) + "]") != std::string::npos);
}

// ═══ On-disk format ══════════════════════════════════════════════════════════

TEST_CASE("materialGraphFromJson loads a hand-written OLD-format document")
{
	// The shape the shipped editor writes: compact dump, node type by NAME, links
	// as OBJECTS {sn,sp,dn,dp}, optional s/g/tt keys, no "comments" array at all.
	// Kept as a literal so refactoring the writer can never quietly redefine what
	// still loads.
	const std::string old =
		R"J({"version":2,"nextId":4,)J"
		R"J("nodes":[{"id":1,"type":"Output","p":[1.0,0.0,0.5,0.0],"x":380.0,"y":120.0},)J"
		R"J({"id":2,"type":"Color","p":[0.8,0.4,0.2,0.0],"x":80.0,"y":120.0},)J"
		R"J({"id":3,"type":"Param (Float)","p":[0.5,0.0,1.0,0.0],"x":80.0,"y":260.0,)J"
		R"J("s":"Gloss","g":"Surface","tt":"how shiny"}],)J"
		R"J("links":[{"sn":2,"sp":0,"dn":1,"dp":0},{"sn":3,"sp":0,"dn":1,"dp":3}]})J";

	MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(old, g));
	REQUIRE(g.nodes.size() == 3);
	CHECK(g.nodes[0].type == MatNodeType::Output);
	CHECK(g.nodes[2].s       == "Gloss");
	CHECK(g.nodes[2].group   == "Surface");
	CHECK(g.nodes[2].tooltip == "how shiny");
	REQUIRE(g.links.size() == 2);
	CHECK(g.links[0].dstPin == HE::kMatOutputBaseColorPin);
	CHECK(g.links[1].dstPin == HE::kMatOutputRoughnessPin);
	CHECK(g.comments.empty());
	CHECK(g.nextId == 4);

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE(gen.params.size() == 1);
	CHECK(gen.params[0].name == "Gloss");
}

TEST_CASE("materialGraphFromJson repairs nextId when a saved id is >= it")
{
	const std::string json =
		R"({"version":2,"nextId":1,"nodes":[{"id":9,"type":"Output","p":[1.0,0.0,0.5,0.0]}],"links":[]})";
	MaterialGraph g;
	REQUIRE(HE::materialGraphFromJson(json, g));
	CHECK(g.nextId == 10);
	CHECK(g.addNode(MatNodeType::ConstFloat) == 10);
}

// ─── matGraphApproxSurface: StaticSwitch permutations ────────────────────────
// The GI-hit approx fold must take the SAME branch codegen takes: the override
// map beats the node default, and only the taken input contributes. A switch
// instance whose permutation swaps the BaseColor path must reflect its own
// colour, not the parent default's.
TEST_CASE("matGraphApproxSurface folds the taken StaticSwitch branch")
{
	HE::MaterialGraph g = HE::MaterialGraph::makeDefault(); // Output + ConstColor→BaseColor
	int outId = 0, redId = 0;
	for (auto& n : g.nodes)
	{
		if (n.type == HE::MatNodeType::Output)     outId = n.id;
		if (n.type == HE::MatNodeType::ConstColor) redId = n.id;
	}
	// Default ConstColor becomes the FALSE branch (red).
	HE::MatGraphNode* red = g.findNode(redId);
	REQUIRE(red != nullptr);
	red->p[0] = 1.0f; red->p[1] = 0.0f; red->p[2] = 0.0f;
	g.disconnectInput(outId, HE::kMatOutputBaseColorPin);

	const int swId = g.addNode(HE::MatNodeType::StaticSwitch);
	HE::MatGraphNode* sw = g.findNode(swId);
	REQUIRE(sw != nullptr);
	sw->s    = "ColorSwitch";
	sw->p[0] = 0.0f; // default OFF → False branch
	const int blueId = g.addNode(HE::MatNodeType::ConstColor);
	HE::MatGraphNode* blue = g.findNode(blueId);
	REQUIRE(blue != nullptr);
	blue->p[0] = 0.0f; blue->p[1] = 0.0f; blue->p[2] = 1.0f;

	REQUIRE(g.connect(blueId, 0, swId, 0)); // True  → blue
	REQUIRE(g.connect(redId,  0, swId, 1)); // False → red
	REQUIRE(g.connect(swId, 0, outId, HE::kMatOutputBaseColorPin));

	// Node default (OFF) → red.
	const HE::MatApproxSurface def = HE::matGraphApproxSurface(g);
	CHECK(def.baseColor[0] == doctest::Approx(1.0f));
	CHECK(def.baseColor[2] == doctest::Approx(0.0f));

	// Instance permutation ON → blue.
	std::map<std::string, bool> ov{ { "ColorSwitch", true } };
	const HE::MatApproxSurface on = HE::matGraphApproxSurface(g, &ov);
	CHECK(on.baseColor[0] == doctest::Approx(0.0f));
	CHECK(on.baseColor[2] == doctest::Approx(1.0f));

	// Explicit OFF override behaves like the default.
	ov["ColorSwitch"] = false;
	const HE::MatApproxSurface off = HE::matGraphApproxSurface(g, &ov);
	CHECK(off.baseColor[0] == doctest::Approx(1.0f));
}

// ─── matGraphApproxSurface: procedural chains + landscape layers ──────────────
// The fold is what colours a GI ray hit (there is no material evaluation at a
// BVH/TLAS hit — see HE::giInstanceSurface). Anything it cannot fold reflects
// plain white, so a generator in the chain used to whiten the WHOLE pin.

TEST_CASE("matGraphApproxSurface folds the procedural generators to their mean")
{
	auto pinnedTo = [](HE::MatNodeType gen)
	{
		MaterialGraph g;
		const int outId = g.addNode(MatNodeType::Output);
		const int genId = g.addNode(gen);
		REQUIRE(g.connect(genId, 0, outId, HE::kMatOutputBaseColorPin));
		return HE::matGraphApproxSurface(g).baseColor[0];
	};
	// heValueNoise: uniform hash → 0.5. heFbm/heFbm3: four octaves with
	// amplitudes 0.5+0.25+0.125+0.0625 = 0.9375, so 0.9375 × 0.5. Checker: 0/1.
	CHECK(pinnedTo(MatNodeType::ValueNoise)   == doctest::Approx(0.5f));
	CHECK(pinnedTo(MatNodeType::Fbm)          == doctest::Approx(0.46875f));
	CHECK(pinnedTo(MatNodeType::NoiseTexture) == doctest::Approx(0.46875f));
	CHECK(pinnedTo(MatNodeType::Checker)      == doctest::Approx(0.5f));

	// The real point: `Colour × Noise` keeps the COLOUR. Before, one unfoldable
	// generator made the whole pin fall back to white.
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	const int col   = g.addNode(MatNodeType::ConstColor);
	g.findNode(col)->p[0] = 0.2f; g.findNode(col)->p[1] = 0.8f; g.findNode(col)->p[2] = 0.4f;
	const int fbm = g.addNode(MatNodeType::Fbm);
	const int mul = g.addNode(MatNodeType::Multiply);
	REQUIRE(g.connect(col, 0, mul, 0));
	REQUIRE(g.connect(fbm, 0, mul, 1));
	REQUIRE(g.connect(mul, 0, outId, HE::kMatOutputBaseColorPin));
	const HE::MatApproxSurface ap = HE::matGraphApproxSurface(g);
	CHECK(ap.baseColor[0] == doctest::Approx(0.2f * 0.46875f));
	CHECK(ap.baseColor[1] == doctest::Approx(0.8f * 0.46875f));
	CHECK(ap.baseColor[2] == doctest::Approx(0.4f * 0.46875f));
}

TEST_CASE("matGraphApproxSurface splits a Landscape Layer Blend into its layers")
{
	// The shape of a real landscape material: layer 1 = green × fbm mottling,
	// layer 2 = flat red, blended by the terrain's painted weightmap.
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	const int blend = g.addNode(MatNodeType::LandscapeLayerBlend);
	g.findNode(blend)->s = "Layer 1\nLayer 2";
	const int green = g.addNode(MatNodeType::ConstColor);
	g.findNode(green)->p[0] = 0.0f; g.findNode(green)->p[1] = 1.0f; g.findNode(green)->p[2] = 0.0f;
	const int fbm = g.addNode(MatNodeType::Fbm);
	const int mul = g.addNode(MatNodeType::Multiply);
	const int red = g.addNode(MatNodeType::ConstColor);
	g.findNode(red)->p[0] = 1.0f; g.findNode(red)->p[1] = 0.0f; g.findNode(red)->p[2] = 0.0f;
	REQUIRE(g.connect(green, 0, mul,   0));
	REQUIRE(g.connect(fbm,   0, mul,   1));
	REQUIRE(g.connect(mul,   0, blend, 0)); // Layer 1
	REQUIRE(g.connect(red,   0, blend, 1)); // Layer 2
	REQUIRE(g.connect(blend, 0, outId, HE::kMatOutputBaseColorPin));

	const HE::MatApproxSurface ap = HE::matGraphApproxSurface(g);
	REQUIRE(ap.layerCount == 2);
	// Per-layer folds survive separately, so a consumer holding the terrain's
	// paint can reproduce THAT terrain's colour.
	CHECK(ap.layerColor[0][1] == doctest::Approx(0.46875f)); // green × fbm mean
	CHECK(ap.layerColor[0][0] == doctest::Approx(0.0f));
	CHECK(ap.layerColor[1][0] == doctest::Approx(1.0f));     // flat red
	CHECK(ap.layerColor[1][1] == doctest::Approx(0.0f));
	// baseColor is the paint-agnostic fallback: the average over the layers —
	// and crucially NOT white, which is what it used to be.
	CHECK(ap.baseColor[0] == doctest::Approx(0.5f));
	CHECK(ap.baseColor[1] == doctest::Approx(0.46875f * 0.5f));
	CHECK(ap.baseColor[2] == doctest::Approx(0.0f));
}

TEST_CASE("Landscape layer split ignores unconnected layers, and needs one that folds")
{
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	const int blend = g.addNode(MatNodeType::LandscapeLayerBlend);
	g.findNode(blend)->s = "A\nB\nC";
	const int blue = g.addNode(MatNodeType::ConstColor);
	g.findNode(blue)->p[0] = 0.0f; g.findNode(blue)->p[1] = 0.0f; g.findNode(blue)->p[2] = 1.0f;
	REQUIRE(g.connect(blue,  0, blend, 1)); // only layer B authored
	REQUIRE(g.connect(blend, 0, outId, HE::kMatOutputBaseColorPin));

	const HE::MatApproxSurface ap = HE::matGraphApproxSurface(g);
	REQUIRE(ap.layerCount == 3);
	CHECK(ap.layerColor[1][2] == doctest::Approx(1.0f));
	// The average skips the unauthored layers instead of dragging in black…
	CHECK(ap.baseColor[2] == doctest::Approx(1.0f));
	// …and the unauthored ones stand in with that average, never white.
	CHECK(ap.layerColor[0][2] == doctest::Approx(1.0f));
	CHECK(ap.layerColor[2][2] == doctest::Approx(1.0f));

	// A layer the fold genuinely cannot evaluate (a texture) leaves no split at
	// all — the caller keeps its own default rather than inventing a colour.
	MaterialGraph t;
	const int tOut   = t.addNode(MatNodeType::Output);
	const int tBlend = t.addNode(MatNodeType::LandscapeLayerBlend);
	t.findNode(tBlend)->s = "Only";
	const int tex = t.addNode(MatNodeType::TextureSample);
	REQUIRE(t.connect(tex,    0, tBlend, 0));
	REQUIRE(t.connect(tBlend, 0, tOut,   HE::kMatOutputBaseColorPin));
	const HE::MatApproxSurface tap = HE::matGraphApproxSurface(t);
	CHECK(tap.layerCount == 0);
	CHECK(tap.baseColor[0] == doctest::Approx(1.0f)); // unchanged default
}

// ═══ Material domain ═════════════════════════════════════════════════════════
// A UI material is drawn in screen space after the scene: there is no light to
// shade it with, no world position to fog it against and no G-buffer to write.
// The domain is what says so — before it, a lit material on a widget was shaded
// by a stand-in sun and fogged as if the pixel it sits on were that many world
// units from the camera.

TEST_CASE("Material domain: the UI domain generates an unlit, G-buffer-less shader")
{
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	const int col   = g.addNode(MatNodeType::ConstColor);
	g.findNode(col)->p[0] = 1.0f;
	REQUIRE(g.connect(col, 0, outId, HE::kMatOutputBaseColorPin));

	// Surface (the default) is unchanged: lit, fogged, with a G-buffer variant.
	g.findNode(outId)->p[0] = 1.0f;   // Lit
	HE::MatShaderGen surf = HE::generateFragment(g);
	CHECK(surf.domain == static_cast<uint8_t>(HE::MatDomain::Surface));
	CHECK(surf.glsl.find("heLitP") != std::string::npos);
	CHECK(surf.glsl.find("heApplyFog") != std::string::npos);
	CHECK_FALSE(surf.glslGBuffer.empty());

	// The same graph in the UI domain: no lighting call, no fog, no G-buffer —
	// and the Lit toggle cannot bring them back.
	g.findNode(outId)->p[3] = static_cast<float>(HE::MatDomain::UserInterface);
	HE::MatShaderGen ui = HE::generateFragment(g);
	CHECK(ui.domain == static_cast<uint8_t>(HE::MatDomain::UserInterface));
	CHECK(ui.glsl.find("heLitP") == std::string::npos);
	CHECK(ui.glsl.find("heApplyFog") == std::string::npos);
	CHECK(ui.glslGBuffer.empty());
	CHECK_FALSE(ui.glsl.empty());
	// It still computes the graph: the colour is the pixel.
	CHECK(ui.glsl.find("oColor") != std::string::npos);

	// Nonsense in p[3] is the Surface domain, not an enum that does not exist.
	g.findNode(outId)->p[3] = 42.0f;
	CHECK(HE::generateFragment(g).domain == static_cast<uint8_t>(HE::MatDomain::Surface));

	CHECK(std::string(HE::matDomainName(HE::MatDomain::UserInterface)) == "User Interface");
	CHECK(std::string(HE::matDomainName(HE::MatDomain::Surface)) == "Surface");
}

TEST_CASE("Material domain: it survives the graph's JSON, old graphs stay Surface")
{
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	g.findNode(outId)->p[3] = static_cast<float>(HE::MatDomain::UserInterface);

	MaterialGraph r;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), r));
	REQUIRE(r.findNode(outId) != nullptr);
	CHECK(r.findNode(outId)->p[3] == doctest::Approx(1.0f));
	CHECK(HE::generateFragment(r).domain == static_cast<uint8_t>(HE::MatDomain::UserInterface));

	// A document written before domains existed has no p[3] to read: it loads as
	// Surface, which is what it was.
	MaterialGraph old;
	REQUIRE(HE::materialGraphFromJson(
		R"J({"nextId":2,"nodes":[{"id":1,"type":"Output","p":[1.0,0.0,0.5],"x":0.0,"y":0.0}],"links":[]})J",
		old));
	REQUIRE(old.findNode(1) != nullptr);
	CHECK(HE::generateFragment(old).domain == static_cast<uint8_t>(HE::MatDomain::Surface));
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("A UI-domain material cross-compiles for Metal and GL")
{
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	g.findNode(outId)->p[3] = static_cast<float>(HE::MatDomain::UserInterface);
	const int uv   = g.addNode(MatNodeType::UV);
	const int fbm  = g.addNode(MatNodeType::Fbm);
	REQUIRE(g.connect(uv,  0, fbm,   0));
	REQUIRE(g.connect(fbm, 0, outId, HE::kMatOutputBaseColorPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.glsl.empty());

	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& mtl = lib.fragment(1234u, gen.glsl, B::Metal);
	INFO(mtl.log);
	CHECK(mtl.ok);
	const auto& gl = lib.fragment(1234u, gen.glsl, B::GLSL410);
	INFO(gl.log);
	CHECK(gl.ok);
	// The UI vertex stub it pairs with on both backends.
	CHECK(lib.uiVertex(B::Metal).ok);
	CHECK(lib.uiVertex(B::GLSL410).ok);
}
#endif

// ═══ Backdrop: what is behind the element (D5 Schicht 1) ══════════════════════
TEST_CASE("Backdrop samples the snapshot in the UI domain and nothing outside it")
{
	auto build = [](HE::MatDomain domain)
	{
		MaterialGraph g;
		const int outId = g.addNode(MatNodeType::Output);
		g.findNode(outId)->p[3] = static_cast<float>(domain);
		const int bd = g.addNode(MatNodeType::Backdrop);
		g.findNode(bd)->p[0] = 12.0f;
		REQUIRE(g.connect(bd, 0, outId, HE::kMatOutputBaseColorPin));
		return HE::generateFragment(g).glsl;
	};

	const std::string ui = build(HE::MatDomain::UserInterface);
	CHECK(ui.find("uniform sampler2D heBackdrop") != std::string::npos);
	CHECK(ui.find("heBackdropBlur(") != std::string::npos);
	// It reads its place from the ELEMENT, never from gl_FragCoord: that one's
	// origin differs between GLSL and MSL, and a mirrored backdrop is the bug
	// nobody sees in a screenshot of a symmetric window.
	CHECK(ui.find("heUI.rect.zw + vUV") != std::string::npos);
	// The block grew a fourth row for it — and the non-UI CONSTANT has to grow
	// with it, or every Surface graph with an Element node stops compiling.
	CHECK(ui.find("vec4 screen;") != std::string::npos);

	const std::string surf = build(HE::MatDomain::Surface);
	CHECK(surf.find("heBackdrop") == std::string::npos);

	// The constant stand-in outside the UI domain has to carry the new row too,
	// or every Surface graph with an Element node stops compiling. Backdrop
	// alone does not ask for the block there (it answers black), so this needs
	// a node that does.
	MaterialGraph s;
	const int sOut = s.addNode(MatNodeType::Output);
	const int size = s.addNode(MatNodeType::ElementSize);
	REQUIRE(s.connect(size, 0, sOut, HE::kMatOutputBaseColorPin));
	CHECK(HE::generateFragment(s).glsl.find(
		"struct HeUIBlock { vec4 rect; vec4 radius; vec4 state; vec4 screen; };")
	      != std::string::npos);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("A Backdrop material cross-compiles for Metal and GL")
{
	MaterialGraph g;
	const int outId = g.addNode(MatNodeType::Output);
	g.findNode(outId)->p[3] = static_cast<float>(HE::MatDomain::UserInterface);
	const int bd  = g.addNode(MatNodeType::Backdrop);
	const int st  = g.addNode(MatNodeType::ElementState);
	const int sdf = g.addNode(MatNodeType::RoundedRectSDF);
	const int mul = g.addNode(MatNodeType::Multiply);
	// Frosted glass that brightens on hover: the two halves of Schicht 1 wired
	// into one graph, which is the combination the shipped Glass function is.
	REQUIRE(g.connect(bd,  0, mul,   0));
	REQUIRE(g.connect(st,  0, mul,   1));
	REQUIRE(g.connect(mul, 0, outId, HE::kMatOutputBaseColorPin));
	REQUIRE(g.connect(sdf, 1, outId, HE::kMatOutputOpacityPin));

	const HE::MatShaderGen gen = HE::generateFragment(g);
	REQUIRE_FALSE(gen.glsl.empty());
	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	const auto& mtl = lib.fragment(0xBD01u, gen.glsl, B::Metal);
	INFO(mtl.log);
	CHECK(mtl.ok);
	const auto& gl = lib.fragment(0xBD01u, gen.glsl, B::GLSL410);
	INFO(gl.log);
	CHECK(gl.ok);
}
#endif

// ═══ The shipped UI effect library (D5 Schicht 1, docs/he-apps-plan.md) ══════
// EditorDeps/EngineContent/MaterialFunctions is generated by matfn_gen and
// COMMITTED, so nothing rebuilds it on the way to this test — which is exactly
// why it needs one. Every project inherits these nine, and a function whose
// input has no default, or which no longer compiles for a backend, is silent
// until someone drops it into a graph.

#ifdef HE_EDITOR_DEPS_DIR
namespace
{
// Load the whole library once: name → its graph. Deliberately not a per-test
// helper that reloads — ContentManager hands out pointers into a dense vector,
// and the NEXT loadAsset invalidates every one of them (asset-pointer lifetime).
// So: load everything, then read everything, then let the manager go.
std::map<std::string, MaterialGraph> loadEngineEffects(std::vector<std::string>& namesOut)
{
	const std::filesystem::path dir =
		std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent" / "MaterialFunctions";
	REQUIRE(std::filesystem::is_directory(dir));

	namesOut.clear();
	for (const auto& entry : std::filesystem::directory_iterator(dir))
		if (entry.path().extension() == ".hasset")
			namesOut.push_back(entry.path().filename().string());
	std::sort(namesOut.begin(), namesOut.end());

	ContentManager cm(dir.string());
	std::vector<HE::UUID> ids;
	for (const std::string& file : namesOut)
	{
		const HE::UUID id = cm.loadAsset(file);
		REQUIRE(id != HE::UUID{});
		ids.push_back(id);
	}

	std::map<std::string, MaterialGraph> out;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		const MaterialFunctionAsset* fn = cm.getMaterialFunction(ids[i]);
		REQUIRE(fn);
		MaterialGraph g;
		REQUIRE(HE::materialGraphFromJson(fn->nodeGraphJson, g));
		out.emplace(namesOut[i], std::move(g));
	}
	return out;
}

// A material that does nothing but call one of them, with no pin wired — the
// state an effect is in the second after it is dropped onto a widget.
MaterialGraph callerFor(const std::string& fnPath, HE::MatDomain domain)
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	g.findNode(out)->p[3] = static_cast<float>(domain);
	const int call = g.addNode(MatNodeType::FunctionCall);
	g.findNode(call)->s = fnPath;
	CHECK(g.connect(call, 0, out, HE::kMatOutputBaseColorPin));
	return g;
}
} // namespace

TEST_CASE("Engine UI effects: every shipped function has an interface a caller can use")
{
	std::vector<std::string> names;
	const std::map<std::string, MaterialGraph> fns = loadEngineEffects(names);
	// The plan's nine. A tenth is welcome and has to be looked at: this number
	// is the prompt to check it against the list, not a cap.
	CHECK(names.size() == 9);

	for (const auto& [name, fn] : fns)
	{
		CAPTURE(name);
		std::vector<HE::MatPinDesc> ins, outs;
		HE::matFunctionPins(fn, ins, outs);
		// No outputs = a call node that can only emit magenta.
		CHECK_FALSE(outs.empty());
		CHECK_FALSE(ins.empty());

		for (const HE::MatGraphNode& n : fn.nodes)
		{
			CAPTURE(n.s);
			// An input without an authored default falls back to the blanket
			// 0.5, i.e. "blur by half a pixel" — the generator warns, this
			// fails. That contract is the whole reason the library is usable
			// before anything is wired.
			if (n.type == MatNodeType::FnInput)
				CHECK(n.p[2] >= 0.5f);
			// The library is FLAT (matfn_gen's rule 2): a nested call would
			// have to name its target by content-relative path, which is a
			// second resolution path nothing here tests.
			CHECK(n.type != MatNodeType::FunctionCall);
		}
		// Every interface pin is named — the name IS the documentation on the
		// call node, and an empty one shows up as a blank pin.
		for (const HE::MatPinDesc& p : ins)  CHECK(std::string(p.name).size() > 0);
		for (const HE::MatPinDesc& p : outs) CHECK(std::string(p.name).size() > 0);
	}
}

TEST_CASE("Engine UI effects: an unwired call emits real code, not the magenta placeholder")
{
	std::vector<std::string> names;
	const std::map<std::string, MaterialGraph> fns = loadEngineEffects(names);
	HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
	{
		const auto it = fns.find(path);
		return it == fns.end() ? nullptr : &it->second;
	};

	for (const std::string& name : names)
	{
		CAPTURE(name);
		const MaterialGraph g = callerFor(name, HE::MatDomain::UserInterface);
		const HE::MatShaderGen gen = HE::generateFragment(g, loader);
		REQUIRE_FALSE(gen.glsl.empty());
		CHECK(gen.glsl.find("// missing function") == std::string::npos);
		CHECK(gen.glsl.find("// recursive") == std::string::npos);
		CHECK(gen.glsl.find("function has no output") == std::string::npos);
		CHECK(gen.domain == static_cast<uint8_t>(HE::MatDomain::UserInterface));
		// A shipped effect exposes its knobs as function INPUTS, never as Param
		// nodes: those would land in the calling material's parameter panel under
		// a fixed name, collide when the effect is used twice, and eat the
		// 16-slot budget for a value the caller cannot even see is there.
		CHECK(gen.params.empty());
	}

	// The defaults are actually in the code: Frosted Glass blurs by its
	// authored 14 px with nothing wired, not by the blanket 0.5.
	const MaterialGraph g = callerFor("FrostedGlass.hasset", HE::MatDomain::UserInterface);
	const std::string glsl = HE::generateFragment(g, loader).glsl;
	CHECK(glsl.find("14.000000") != std::string::npos);
	CHECK(glsl.find("heBackdropBlur") != std::string::npos);
}

TEST_CASE("A Function Input's default reaches an unwired caller pin, and only when authored")
{
	// The mechanism the library rests on, on a graph small enough to read.
	MaterialGraph fn;
	const int in   = fn.addNode(MatNodeType::FnInput);
	fn.findNode(in)->s = "Amount";
	fn.findNode(in)->p[0] = 0.0f;   // Float
	const int fout = fn.addNode(MatNodeType::FnOutput);
	fn.findNode(fout)->s = "Out";
	fn.findNode(fout)->p[0] = 0.0f;
	CHECK(fn.connect(in, 0, fout, 0));

	HE::MatFunctionLoader loader = [&](const std::string&) -> const MaterialGraph* { return &fn; };
	const MaterialGraph caller = callerFor("Fn.hasset", HE::MatDomain::Surface);

	// No default authored (p[2] == 0): the old blanket 0.5 stands, so graphs
	// written before defaults existed keep the shader they always had.
	std::vector<HE::MatPinDesc> ins, outs;
	HE::matFunctionPins(fn, ins, outs);
	REQUIRE(ins.size() == 1);
	CHECK(ins[0].def == HE::kMatFnInputLegacyDefault);
	CHECK(HE::generateFragment(caller, loader).glsl.find("0.500000") != std::string::npos);

	// Authored: the number the function asked for.
	fn.findNode(in)->p[1] = 12.0f;
	fn.findNode(in)->p[2] = 1.0f;
	HE::matFunctionPins(fn, ins, outs);
	CHECK(ins[0].def == 12.0f);
	CHECK(HE::generateFragment(caller, loader).glsl.find("12.000000") != std::string::npos);

	// A WIRED pin still wins over the default — the default is a fallback, not
	// an override.
	MaterialGraph wired = caller;
	const int k = wired.addNode(MatNodeType::ConstFloat);
	wired.findNode(k)->p[0] = 3.0f;
	int callId = 0;
	for (const HE::MatGraphNode& n : wired.nodes)
		if (n.type == MatNodeType::FunctionCall) callId = n.id;
	CHECK(wired.connect(k, 0, callId, 0));
	const std::string glsl = HE::generateFragment(wired, loader).glsl;
	CHECK(glsl.find("3.000000") != std::string::npos);
	CHECK(glsl.find("12.000000") == std::string::npos);
}

#if defined(HE_TESTS_HAVE_SHADERC)
TEST_CASE("Engine UI effects: every shipped function cross-compiles for Metal and GL")
{
	std::vector<std::string> names;
	const std::map<std::string, MaterialGraph> fns = loadEngineEffects(names);
	HE::MatFunctionLoader loader = [&](const std::string& path) -> const MaterialGraph*
	{
		const auto it = fns.find(path);
		return it == fns.end() ? nullptr : &it->second;
	};

	HE::MaterialShaderLibrary lib;
	using B = HE::MaterialShaderLibrary::Backend;
	// BOTH domains: Backdrop is the one node whose emitted text depends on the
	// domain (a sampler inside the UI pass, black outside it), so a library that
	// only compiled as UI would break the day someone dropped Frosted Glass on a
	// mesh — which the node explicitly permits.
	for (const HE::MatDomain domain : { HE::MatDomain::UserInterface, HE::MatDomain::Surface })
	{
		for (const std::string& name : names)
		{
			CAPTURE(name);
			CAPTURE(static_cast<int>(domain));
			const MaterialGraph g = callerFor(name, domain);
			const std::string glsl = HE::generateFragment(g, loader).glsl;
			REQUIRE_FALSE(glsl.empty());
			const uint64_t hash = std::hash<std::string>{}(glsl);
			const auto& mtl = lib.fragment(hash, glsl, B::Metal);
			CHECK_MESSAGE(mtl.ok, "MSL compile failed: ", mtl.log);
			const auto& gl = lib.fragment(hash, glsl, B::GLSL410);
			CHECK_MESSAGE(gl.ok, "GLSL compile failed: ", gl.log);
		}
	}
}
#endif
#endif // HE_EDITOR_DEPS_DIR
