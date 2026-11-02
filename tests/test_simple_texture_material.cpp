#include "doctest.h"
#include <string>

#include <ContentManager/Assets.h>
#include <Hpak/SimpleTextureMaterial.h>
#include <MaterialGraph/MaterialGraph.h>

// The export bakes only materials that show one texture and nothing else. These tests pin
// what "nothing else" means, on both shapes (the uber shader and a graph), and that every
// way out of it is refused with a reason the export log can print.

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

// Output <- TextureSample (alone). Returns the sample's id; `out` receives the Output's.
struct GraphParts { MaterialGraph g; int out = 0, sample = 0; };

GraphParts sampleGraph(const char* path = "")
{
	GraphParts p;
	p.out    = p.g.addNode(MatNodeType::Output);
	p.sample = p.g.addNode(MatNodeType::TextureSample);
	p.g.findNode(p.sample)->s = path;
	p.g.connect(p.sample, 0, p.out, kMatOutputBaseColorPin);
	return p;
}

MaterialAsset graphMaterial(const MaterialGraph& g, const char* meshTex = "Tex/Bark.hasset")
{
	MaterialAsset m;
	m.texturePaths = { meshTex };
	m.nodeGraphJson = materialGraphToJson(g);
	return m;
}

std::string whyNot(const MaterialAsset& m)
{
	HE::SimpleTextureMaterial r;
	std::string why;
	REQUIRE_FALSE(HE::classifySimpleTextureMaterial(m, r, &why));
	return why;
}

} // namespace

TEST_CASE("Simple texture material: the uber shader with one texture qualifies")
{
	HE::SimpleTextureMaterial r;
	REQUIRE(HE::classifySimpleTextureMaterial(uberMaterial(), r));
	CHECK(r.albedoPath == "Tex/Bark.hasset");
	CHECK(r.tint[0] == doctest::Approx(0.5f));
	CHECK(r.tint[1] == doctest::Approx(0.25f));
	CHECK(r.metallic  == doctest::Approx(0.1f));
	CHECK(r.roughness == doctest::Approx(0.7f));
	CHECK_FALSE(r.fromGraph);
	CHECK(r.identityUv());
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

TEST_CASE("Simple texture material: a graph with a lone Texture Sample qualifies")
{
	// An empty sample path means the material's own texture slot 0.
	GraphParts p = sampleGraph();
	HE::SimpleTextureMaterial r;
	REQUIRE(HE::classifySimpleTextureMaterial(graphMaterial(p.g), r));
	CHECK(r.albedoPath == "Tex/Bark.hasset");
	CHECK(r.fromGraph);
	CHECK(r.tint[0] == doctest::Approx(1.0f));
	CHECK(r.identityUv());
	// A graph's unconnected scalar pins are the Output node's own defaults.
	CHECK(r.metallic  == doctest::Approx(0.0f));
	CHECK(r.roughness == doctest::Approx(0.5f));

	// A named texture wins over slot 0.
	GraphParts named = sampleGraph("Tex/Stone.hasset");
	REQUIRE(HE::classifySimpleTextureMaterial(graphMaterial(named.g), r));
	CHECK(r.albedoPath == "Tex/Stone.hasset");
}

TEST_CASE("Simple texture material: tint and tiling are read from the graph")
{
	GraphParts p = sampleGraph();
	const int mul  = p.g.addNode(MatNodeType::Multiply);
	const int col  = p.g.addNode(MatNodeType::ConstColor);
	const int uv   = p.g.addNode(MatNodeType::UV);
	MatGraphNode* c = p.g.findNode(col);
	c->p[0] = 0.2f; c->p[1] = 0.4f; c->p[2] = 0.6f;
	MatGraphNode* u = p.g.findNode(uv);
	u->p[0] = 4.0f; u->p[1] = 2.0f; u->p[2] = 0.5f; u->p[3] = 0.25f;
	p.g.disconnectInput(p.out, kMatOutputBaseColorPin);
	p.g.connect(p.sample, 0, mul, 0);
	p.g.connect(col, 0, mul, 1);
	p.g.connect(mul, 0, p.out, kMatOutputBaseColorPin);
	p.g.connect(uv, 0, p.sample, 0);

	HE::SimpleTextureMaterial r;
	REQUIRE(HE::classifySimpleTextureMaterial(graphMaterial(p.g), r));
	CHECK(r.tint[0] == doctest::Approx(0.2f));
	CHECK(r.tint[1] == doctest::Approx(0.4f));
	CHECK(r.tint[2] == doctest::Approx(0.6f));
	CHECK(r.uvScale[0]  == doctest::Approx(4.0f));
	CHECK(r.uvScale[1]  == doctest::Approx(2.0f));
	CHECK(r.uvOffset[0] == doctest::Approx(0.5f));
	CHECK(r.uvOffset[1] == doctest::Approx(0.25f));
	CHECK_FALSE(r.identityUv());
}

TEST_CASE("Simple texture material: a graph is refused for anything beyond one texture")
{
	{   // a second Output pin is driven
		GraphParts p = sampleGraph();
		const int k = p.g.addNode(MatNodeType::ConstFloat);
		p.g.connect(k, 0, p.out, kMatOutputRoughnessPin);
		CHECK(whyNot(graphMaterial(p.g)) == "its graph drives Roughness");
	}
	{   // Base Color is not a texture
		GraphParts p = sampleGraph();
		p.g.disconnectInput(p.out, kMatOutputBaseColorPin);
		const int col = p.g.addNode(MatNodeType::ConstColor);
		p.g.connect(col, 0, p.out, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(p.g)) == "Base Color is not a plain texture sample");
	}
	{   // Base Color unconnected
		GraphParts p = sampleGraph();
		p.g.disconnectInput(p.out, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(p.g)) == "Base Color is not connected");
	}
	{   // the alpha pin is not the colour
		GraphParts p = sampleGraph();
		p.g.disconnectInput(p.out, kMatOutputBaseColorPin);
		p.g.connect(p.sample, 1, p.out, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(p.g)) == "Base Color is not a plain texture sample");
	}
	{   // texture times something that is not a constant
		GraphParts p = sampleGraph();
		const int mul = p.g.addNode(MatNodeType::Multiply);
		const int t   = p.g.addNode(MatNodeType::Time);
		p.g.disconnectInput(p.out, kMatOutputBaseColorPin);
		p.g.connect(p.sample, 0, mul, 0);
		p.g.connect(t, 0, mul, 1);
		p.g.connect(mul, 0, p.out, kMatOutputBaseColorPin);
		CHECK(whyNot(graphMaterial(p.g)) == "Base Color is not a texture times a constant");
	}
	{   // the sample's UV is computed
		GraphParts p = sampleGraph();
		const int pan = p.g.addNode(MatNodeType::Panner);
		p.g.connect(pan, 0, p.sample, 0);
		CHECK(whyNot(graphMaterial(p.g)) == "its texture UV is computed");
	}
	{   // masked blend mode
		GraphParts p = sampleGraph();
		p.g.findNode(p.out)->p[1] = static_cast<float>(MatBlendMode::Masked);
		CHECK(whyNot(graphMaterial(p.g)) == "its blend mode is not opaque");
	}
	{   // no texture to sample at all
		GraphParts p = sampleGraph();
		MaterialAsset m = graphMaterial(p.g);
		m.texturePaths.clear();
		CHECK(whyNot(m) == "its texture sample names no texture");
	}
	{   // an unreadable graph
		MaterialAsset m = uberMaterial();
		m.nodeGraphJson = "{ not json";
		CHECK(whyNot(m) == "its node graph cannot be read");
	}
}

TEST_CASE("Simple texture material: what two materials may share")
{
	HE::SimpleTextureMaterial a, b;
	REQUIRE(HE::classifySimpleTextureMaterial(uberMaterial("Tex/A.hasset"), a));
	MaterialAsset other = uberMaterial("Tex/B.hasset");
	REQUIRE(HE::classifySimpleTextureMaterial(other, b));
	CHECK(a.sameShading(b));          // different textures are the bake's business
	CHECK(a.sameTint(b));

	other.roughness = 0.2f;
	REQUIRE(HE::classifySimpleTextureMaterial(other, b));
	CHECK_FALSE(a.sameShading(b));

	other = uberMaterial("Tex/B.hasset");
	other.doubleSided = true;
	REQUIRE(HE::classifySimpleTextureMaterial(other, b));
	CHECK_FALSE(a.sameShading(b));

	other = uberMaterial("Tex/B.hasset");
	other.baseColor[0] = 1.0f;
	REQUIRE(HE::classifySimpleTextureMaterial(other, b));
	CHECK(a.sameShading(b));
	CHECK_FALSE(a.sameTint(b));       // only a resampling bake can fold a different tint in
}
