#include "doctest.h"
#include <string>

#include <ContentManager/Assets.h>
#include <Hpak/SimpleTextureMaterial.h>
#include <MaterialGraph/MaterialGraph.h>

// The export bakes only materials that are a plain PBR texture set. These tests pin what
// "plain" means, on both shapes (the uber shader and a graph shaped like the glTF importer's
// output), and that every way out of it is refused with a reason the export log can print.

using namespace HE; // the node graph lives in HE, the assets in the global namespace

namespace
{

MaterialAsset uberMaterial(const char* tex = "Tex/Bark.hasset")
{
	MaterialAsset m;
	m.texturePaths = { tex };
	m.baseColor[0] = 0.5f; m.baseColor[1] = 0.25f; m.baseColor[2] = 1.0f;
	m.metallic  = 0.1f;
	m.roughness = 0.7f;
	return m;
}

// A graph under construction with the importer's building blocks.
struct Graph
{
	MaterialGraph g;
	int out = 0;
	Graph() { out = g.addNode(MatNodeType::Output); }

	int uv(float sx = 1, float sy = 1, float ox = 0, float oy = 0)
	{
		const int n = g.addNode(MatNodeType::UV);
		MatGraphNode* node = g.findNode(n);
		node->p[0] = sx; node->p[1] = sy; node->p[2] = ox; node->p[3] = oy;
		return n;
	}
	// Texture Sample through a UV node (an empty path = the material's slot 0).
	int sample(const char* path = "", float sx = 1, float sy = 1, float ox = 0, float oy = 0)
	{
		const int n = g.addNode(MatNodeType::TextureSample);
		g.findNode(n)->s = path;
		g.connect(uv(sx, sy, ox, oy), 0, n, 0);
		return n;
	}
	int split(int sampleNode)
	{
		const int n = g.addNode(MatNodeType::SplitRGBA);
		g.connect(sampleNode, 0, n, 0);
		return n;
	}
	int constFloat(float v)
	{
		const int n = g.addNode(MatNodeType::ConstFloat);
		g.findNode(n)->p[0] = v;
		return n;
	}
	int constColor(float r, float gr, float b)
	{
		const int n = g.addNode(MatNodeType::ConstColor);
		MatGraphNode* node = g.findNode(n);
		node->p[0] = r; node->p[1] = gr; node->p[2] = b;
		return n;
	}
	int multiply(int a, int aPin, int b)
	{
		const int n = g.addNode(MatNodeType::Multiply);
		g.connect(a, aPin, n, 0);
		g.connect(b, 0, n, 1);
		return n;
	}
	void toOutput(int src, int srcPin, int pin) { g.connect(src, srcPin, out, pin); }
};

MaterialAsset graphMaterial(const Graph& gr, const char* meshTex = "Tex/Bark.hasset")
{
	MaterialAsset m;
	m.texturePaths = { meshTex };
	m.nodeGraphJson = materialGraphToJson(gr.g);
	return m;
}

std::string whyNot(const MaterialAsset& m)
{
	SimpleTextureMaterial r;
	std::string why;
	REQUIRE_FALSE(classifySimpleTextureMaterial(m, r, &why));
	return why;
}

} // namespace

TEST_CASE("Simple texture material: the uber shader with one texture qualifies")
{
	SimpleTextureMaterial r;
	REQUIRE(classifySimpleTextureMaterial(uberMaterial(), r));
	CHECK(r.baseColor.tex.path == "Tex/Bark.hasset");
	CHECK(r.baseColor.factor[0] == doctest::Approx(0.5f));
	CHECK(r.baseColor.factor[1] == doctest::Approx(0.25f));
	CHECK(r.metallic.factor  == doctest::Approx(0.1f));
	CHECK(r.roughness.factor == doctest::Approx(0.7f));
	CHECK(r.metallic.tex.empty());
	CHECK(r.normal.empty());
	CHECK_FALSE(r.fromGraph);
	CHECK(r.baseColor.tex.identityUv());
	CHECK(r.texturePaths() == std::vector<std::string>{ "Tex/Bark.hasset" });
}

TEST_CASE("Simple texture material: the uber shader is refused for what it cannot bake")
{
	MaterialAsset m = uberMaterial();
	m.texturePaths.clear();
	CHECK(whyNot(m) == "it has no texture");

	m = uberMaterial();
	m.opacity = 0.5f;
	CHECK(whyNot(m) == "it is translucent");

	m = uberMaterial();
	m.blendMode = 1;
	CHECK(whyNot(m) == "its blend mode is not opaque");

	m = uberMaterial();
	m.customShaderFragGlsl = "void main() {}";
	CHECK(whyNot(m) == "it runs hand-written GLSL");

	m = uberMaterial();
	m.parentMaterialPath = "Mats/Master.hasset";
	CHECK(whyNot(m) == "it is a material instance");

	m = uberMaterial();
	m.customShaderVertGlsl = "vec3 heWpo = vec3(0.0);";
	CHECK(whyNot(m) == "it moves vertices");

	m = uberMaterial();
	m.customShaderDispGlsl = "float heDisp = 0.0;";
	CHECK(whyNot(m) == "it displaces the surface");

	m = uberMaterial();
	m.domain = 1;
	CHECK(whyNot(m) == "it is not a Surface material");
}

TEST_CASE("Simple texture material: a lone Texture Sample with its UV node qualifies")
{
	// An empty sample path means the material's own texture slot 0.
	Graph gr;
	gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
	SimpleTextureMaterial r;
	REQUIRE(classifySimpleTextureMaterial(graphMaterial(gr), r));
	CHECK(r.baseColor.tex.path == "Tex/Bark.hasset");
	CHECK(r.fromGraph);
	CHECK(r.baseColor.factor[0] == doctest::Approx(1.0f));
	CHECK(r.baseColor.tex.identityUv());
	// Pins the graph leaves open keep the Output node's own defaults.
	CHECK(r.metallic.factor  == doctest::Approx(0.0f));
	CHECK(r.roughness.factor == doctest::Approx(0.5f));
	CHECK(r.ao.factor        == doctest::Approx(1.0f));
	CHECK(r.specular         == doctest::Approx(0.5f));

	// A named texture wins over slot 0.
	Graph named;
	named.toOutput(named.sample("Tex/Stone.hasset"), 0, kMatOutputBaseColorPin);
	REQUIRE(classifySimpleTextureMaterial(graphMaterial(named), r));
	CHECK(r.baseColor.tex.path == "Tex/Stone.hasset");
}

TEST_CASE("Simple texture material: an importer-shaped PBR graph qualifies with every role")
{
	Graph gr;
	// Base color: sample x factor, tiled 4x2 with an offset.
	const int base = gr.sample("Tex/Albedo.hasset", 4, 2, 0.5f, 0.25f);
	gr.toOutput(gr.multiply(base, 0, gr.constColor(0.2f, 0.4f, 0.6f)), 0, kMatOutputBaseColorPin);
	// Packed ORM map: G roughness, B metallic x 0.8, R occlusion at strength 0.5.
	const int orm = gr.sample("Tex/ORM.hasset", 4, 2, 0.5f, 0.25f);
	const int split = gr.split(orm);
	gr.toOutput(gr.multiply(split, 2, gr.constFloat(0.8f)), 0, kMatOutputMetallicPin);
	gr.toOutput(split, 1, kMatOutputRoughnessPin);
	{
		const int lerp = gr.g.addNode(MatNodeType::Lerp);
		gr.g.connect(gr.constColor(1, 1, 1), 0, lerp, 0);
		gr.g.connect(split, 0, lerp, 1);
		gr.g.connect(gr.constFloat(0.5f), 0, lerp, 2);
		gr.toOutput(lerp, 0, kMatOutputAOPin);
	}
	gr.toOutput(gr.constFloat(0.3f), 0, kMatOutputSpecularPin);
	// Normal map, strength 2; its UV is left open (falls back to the mesh UV).
	{
		const int n = gr.g.addNode(MatNodeType::NormalMapSample);
		gr.g.findNode(n)->s = "Tex/Normal.hasset";
		gr.g.findNode(n)->p[0] = 2.0f;
		gr.toOutput(n, 0, kMatOutputNormalPin);
	}
	gr.toOutput(gr.constColor(0.1f, 0.2f, 0.3f), 0, kMatOutputEmissivePin);

	SimpleTextureMaterial r;
	std::string why;
	REQUIRE_MESSAGE(classifySimpleTextureMaterial(graphMaterial(gr), r, &why), why);
	CHECK(r.baseColor.tex.path == "Tex/Albedo.hasset");
	CHECK(r.baseColor.factor[0] == doctest::Approx(0.2f));
	CHECK(r.baseColor.factor[2] == doctest::Approx(0.6f));
	CHECK(r.baseColor.tex.uvScale[0]  == doctest::Approx(4.0f));
	CHECK(r.baseColor.tex.uvScale[1]  == doctest::Approx(2.0f));
	CHECK(r.baseColor.tex.uvOffset[0] == doctest::Approx(0.5f));
	CHECK(r.baseColor.tex.uvOffset[1] == doctest::Approx(0.25f));
	CHECK_FALSE(r.baseColor.tex.identityUv());
	CHECK(r.metallic.tex.path == "Tex/ORM.hasset");
	CHECK(r.metallic.channel == 2);
	CHECK(r.metallic.factor  == doctest::Approx(0.8f));
	CHECK(r.roughness.channel == 1);
	CHECK(r.roughness.factor  == doctest::Approx(1.0f));
	CHECK(r.ao.channel == 0);
	CHECK(r.ao.factor  == doctest::Approx(0.5f));
	CHECK(r.specular == doctest::Approx(0.3f));
	CHECK(r.normal.path == "Tex/Normal.hasset");
	CHECK(r.normal.identityUv());
	CHECK(r.normalStrength == doctest::Approx(2.0f));
	CHECK(r.emissive.tex.empty());
	CHECK(r.emissive.factor[1] == doctest::Approx(0.2f));
	CHECK(r.metallic.tex.sameSampling(r.roughness.tex));
	CHECK_FALSE(r.metallic.tex.sameSampling(r.baseColor.tex));
	CHECK(r.texturePaths().size() == 3);   // albedo, ORM (once), normal
}

TEST_CASE("Simple texture material: a graph is refused for anything beyond a plain PBR set")
{
	{   // Opacity is driven
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.toOutput(gr.constFloat(0.5f), 0, kMatOutputOpacityPin);
		CHECK(whyNot(graphMaterial(gr)) == "its graph drives Opacity");
	}
	{   // World Position Offset is driven
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.toOutput(gr.constColor(0, 0, 0), 0, kMatOutputWPOPin);
		CHECK(whyNot(graphMaterial(gr)) == "its graph drives World Position Offset");
	}
	{   // the sample's alpha is not the colour
		Graph gr;
		gr.toOutput(gr.sample(), 1, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(gr)) == "Base Color is not a texture or a constant");
	}
	{   // texture times something that is not a constant
		Graph gr;
		const int t = gr.g.addNode(MatNodeType::Time);
		gr.toOutput(gr.multiply(gr.sample(), 0, t), 0, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(gr)) == "Base Color is not a texture or a constant");
	}
	{   // a Texture Sample with an open UV pin reads ONE texel (vec2(0)), not the mesh UV
		Graph gr;
		const int s = gr.g.addNode(MatNodeType::TextureSample);
		gr.toOutput(s, 0, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(gr)) == "a texture sample has no UV connected");
	}
	{   // a computed UV
		Graph gr;
		const int s = gr.g.addNode(MatNodeType::TextureSample);
		gr.g.connect(gr.g.addNode(MatNodeType::Panner), 0, s, 0);
		gr.toOutput(s, 0, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(gr)) == "a texture UV is computed");
	}
	{   // roughness from something that is not a texture channel
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.toOutput(gr.g.addNode(MatNodeType::Time), 0, kMatOutputRoughnessPin);
		CHECK(whyNot(graphMaterial(gr)) == "Roughness is not a texture channel or a constant");
	}
	{   // a split that is not fed by a texture sample
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		const int split = gr.g.addNode(MatNodeType::SplitRGBA);
		gr.toOutput(split, 1, kMatOutputRoughnessPin);
		CHECK(whyNot(graphMaterial(gr)) == "Roughness is not a texture channel or a constant");
	}
	{   // specular from a texture
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.toOutput(gr.split(gr.sample()), 0, kMatOutputSpecularPin);
		CHECK(whyNot(graphMaterial(gr)) == "Specular is not a constant");
	}
	{   // normal that is not a normal map node
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.toOutput(gr.g.addNode(MatNodeType::NormalWS), 0, kMatOutputNormalPin);
		CHECK(whyNot(graphMaterial(gr)) == "Normal is not a normal map");
	}
	{   // masked blend mode
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		gr.g.findNode(gr.out)->p[1] = static_cast<float>(MatBlendMode::Masked);
		CHECK(whyNot(graphMaterial(gr)) == "its blend mode is not opaque");
	}
	{   // nothing to sample at all: a flat colour is not a texture bake
		Graph gr;
		gr.toOutput(gr.constColor(1, 0, 0), 0, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(gr)) == "it uses no texture");
	}
	{   // an empty path with no slot 0 to fall back on
		Graph gr;
		gr.toOutput(gr.sample(), 0, kMatOutputBaseColorPin);
		MaterialAsset m = graphMaterial(gr);
		m.texturePaths.clear();
		CHECK(whyNot(m) == "a texture sample names no texture");
	}
	{   // an unreadable graph
		MaterialAsset m = uberMaterial();
		m.nodeGraphJson = "{ not json";
		CHECK(whyNot(m) == "its node graph cannot be read");
	}
}

TEST_CASE("Simple texture material: what two materials may share")
{
	SimpleTextureMaterial a, b;
	REQUIRE(classifySimpleTextureMaterial(uberMaterial("Tex/A.hasset"), a));
	MaterialAsset other = uberMaterial("Tex/B.hasset");
	REQUIRE(classifySimpleTextureMaterial(other, b));
	CHECK(a.sameShading(b));          // different textures are the bake's business
	CHECK(a.sameFactors(b));

	other = uberMaterial("Tex/B.hasset");
	other.doubleSided = true;
	REQUIRE(classifySimpleTextureMaterial(other, b));
	CHECK_FALSE(a.sameShading(b));    // side-ness cannot be folded into texels

	other = uberMaterial("Tex/B.hasset");
	other.roughness = 0.2f;
	REQUIRE(classifySimpleTextureMaterial(other, b));
	CHECK(a.sameShading(b));
	CHECK_FALSE(a.sameFactors(b));    // only a resampling bake can fold a different factor in

	other = uberMaterial("Tex/B.hasset");
	other.baseColor[0] = 1.0f;
	REQUIRE(classifySimpleTextureMaterial(other, b));
	CHECK_FALSE(a.sameFactors(b));
}
