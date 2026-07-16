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
	CHECK(g.connect(sp, 0, out, 3)); // Emissive from screen pos (coerced)
	CHECK(g.connect(v4, 0, out, 4)); // Opacity from vec4.x
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
	CHECK(t.connect(b,   0, tout, 3));
	CHECK(t.connect(b2,  0, tout, 3)); // replaces link but references rock again
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
	CHECK(p.connect(pv2, 0, pout, 3));  // Emissive from vec2 (coerced)
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
	CHECK(g.connect(pf, 0, out, 2));

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
		g.connect(a, 0, out, 4); // pin 4 = Opacity/OpacityMask
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
	CHECK(g.connect(nm, 0, out, 5)); // Normal pin
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
	CHECK(g.connect(cmb, 0, out, 6)); // WPO pin

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
	g.connect(eq,  0, out, 4);         // Opacity from Equal (unconnected inputs → defaults)
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
