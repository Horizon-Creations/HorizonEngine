#include "doctest.h"
#include <cmath>
#include <map>
#include <string>

#include <Hpak/MeshTextureBake.h>

// The export-time mesh texture bake: what it merges, what it keeps, and that the lossless mode
// really copies texels. Geometry is a few quads, textures are small deterministic patterns.

using namespace HE;
using namespace HE::MeshBake;

namespace
{

// A w x h image whose texel (x, y) is (r0 + 20x, g0 + 20y, b0), alpha 255.
Image pattern(uint32_t w, uint32_t h, uint8_t r0, uint8_t g0, uint8_t b0, bool srgb = false)
{
	Image im; im.width = w; im.height = h; im.srgb = srgb;
	im.rgba.resize(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x)
		{
			uint8_t* p = &im.rgba[(size_t(y) * w + x) * 4];
			p[0] = uint8_t(r0 + 20 * x); p[1] = uint8_t(g0 + 20 * y); p[2] = b0; p[3] = 255;
		}
	return im;
}

const uint8_t* px(const Image& im, uint32_t x, uint32_t y) { return &im.rgba[(size_t(y) * im.width + x) * 4]; }

struct Scene
{
	std::map<std::string, Image> images;
	ImageLookup lookup() const {
		return [this](const std::string& p) -> const Image* {
			auto it = images.find(p);
			return it == images.end() ? nullptr : &it->second;
		};
	}
};

// Appends a quad (4 vertices, 2 triangles) with UVs spanning [u0,u1]x[v0,v1]; returns its section.
SectionInput addQuad(MeshInput& m, float x, float u0, float v0, float u1, float v1,
                     const SimpleTextureMaterial& mat)
{
	const uint32_t base = uint32_t(m.vertices.size() / 3);
	const float xs[4] = { x, x + 1, x + 1, x }, ys[4] = { 0, 0, 1, 1 };
	const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
	for (int i = 0; i < 4; ++i)
	{
		m.vertices.insert(m.vertices.end(), { xs[i], ys[i], 0.0f });
		m.normals.insert(m.normals.end(), { 0.0f, 0.0f, 1.0f });
		m.uvs.insert(m.uvs.end(), { us[i], vs[i] });
	}
	SectionInput s;
	s.indexOffset = uint32_t(m.indices.size());
	for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) m.indices.push_back(base + i);
	s.indexCount = 6;
	s.simple = true;
	s.material = mat;
	m.sections.push_back(s);
	return s;
}

SimpleTextureMaterial baseOnly(const char* tex)
{
	SimpleTextureMaterial m;
	m.baseColor.tex.path = tex;
	return m;
}

// The atlas position (in texels) a section's old UV (u, v) was moved to: found through the
// first vertex of that section, which sits at the section's (u0, v0) corner.
void cornerOf(const Result& r, const Image& atlas, uint32_t section, int& x, int& y)
{
	const uint32_t firstIndex = section * 6;
	const uint32_t v = r.indices[firstIndex];
	x = int(std::lround(r.uvs[v * 2] * float(atlas.width)));
	y = int(std::lround(r.uvs[v * 2 + 1] * float(atlas.height)));
}

} // namespace

TEST_CASE("Mesh bake merge: two quads become one section and every texel is copied unchanged")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	MeshInput m;
	addQuad(m, 0, 0, 0, 1, 1, baseOnly("A"));
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));

	Options o; o.mode = Mode::Merge;
	const Result r = bake(m, sc.lookup(), o);
	REQUIRE_MESSAGE(r.ok, r.why);
	CHECK(r.lossless);
	CHECK(r.mergedSections == 2);
	CHECK(r.mergedIndexCount == 12);
	CHECK(r.keptSections.empty());
	CHECK(r.indices.size() == 12);
	REQUIRE(r.base.atlas);
	CHECK_FALSE(r.orm.atlas);
	CHECK_FALSE(r.normal.atlas);
	CHECK(r.material.baseColor.tex.path == kBakedBase);
	CHECK(r.material.baseColor.factor[0] == doctest::Approx(1.0f));

	// Every source texel of both quads is in the atlas, byte for byte, at the mapped place.
	const Image& atlas = r.base.image;
	const char* names[2] = { "A", "B" };
	for (uint32_t s = 0; s < 2; ++s)
	{
		int cx, cy;
		cornerOf(r, atlas, s, cx, cy);
		const Image& src = sc.images[names[s]];
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x)
				for (int c = 0; c < 3; ++c)
					CHECK(px(atlas, uint32_t(cx) + x, uint32_t(cy) + y)[c] == px(src, x, y)[c]);
	}
}

TEST_CASE("Mesh bake merge: a sub-rectangle of a texture copies only that part")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	MeshInput m;
	addQuad(m, 0, 0.25f, 0.5f, 0.75f, 1.0f, baseOnly("A"));   // texels 2..6 x 4..8
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
	Options o; o.mode = Mode::Merge;
	const Result r = bake(m, sc.lookup(), o);
	REQUIRE_MESSAGE(r.ok, r.why);
	int cx, cy;
	cornerOf(r, r.base.image, 0, cx, cy);
	for (uint32_t y = 0; y < 4; ++y)
		for (uint32_t x = 0; x < 4; ++x)
			CHECK(px(r.base.image, uint32_t(cx) + x, uint32_t(cy) + y)[0] == px(sc.images["A"], 2 + x, 4 + y)[0]);
}

TEST_CASE("Mesh bake merge: refusals say why")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	Options o; o.mode = Mode::Merge;
	{   // tiling UVs
		MeshInput m;
		addQuad(m, 0, 0, 0, 4, 4, baseOnly("A"));
		addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
		const Result r = bake(m, sc.lookup(), o);
		CHECK_FALSE(r.ok);
		CHECK(r.why == "a section's UVs tile");
	}
	{   // a texture UV transform
		MeshInput m;
		SimpleTextureMaterial a = baseOnly("A");
		a.baseColor.tex.uvScale[0] = 2.0f;
		addQuad(m, 0, 0, 0, 1, 1, a);
		addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
		CHECK(bake(m, sc.lookup(), o).why == "a texture uses tiling or an offset");
	}
	{   // textured sections with different tints
		MeshInput m;
		SimpleTextureMaterial a = baseOnly("A");
		a.baseColor.factor[0] = 0.5f;
		addQuad(m, 0, 0, 0, 1, 1, a);
		addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
		CHECK(bake(m, sc.lookup(), o).why == "textured sections have different color factors");
	}
	{   // a section whose own textures differ in size
		MeshInput m;
		sc.images["N"] = pattern(4, 4, 128, 128, 255);
		SimpleTextureMaterial a = baseOnly("A");
		a.normal.path = "N";
		addQuad(m, 0, 0, 0, 1, 1, a);
		addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
		CHECK(bake(m, sc.lookup(), o).why == "a section's textures differ in size");
		sc.images.erase("N");
	}
	{   // a missing texture
		MeshInput m;
		addQuad(m, 0, 0, 0, 1, 1, baseOnly("A"));
		addQuad(m, 2, 0, 0, 1, 1, baseOnly("Gone"));
		CHECK(bake(m, sc.lookup(), o).why == "texture 'Gone' is not available");
	}
}

TEST_CASE("Mesh bake: fewer than two simple sections is nothing to merge")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	MeshInput m;
	addQuad(m, 0, 0, 0, 1, 1, baseOnly("A"));
	SectionInput& only = m.sections[0];
	CHECK(bake(m, sc.lookup(), Options{}).why == "fewer than two simple sections");
	only.simple = false;
	CHECK_FALSE(bake(m, sc.lookup(), Options{}).ok);
	MeshInput empty;
	CHECK(bake(empty, sc.lookup(), Options{}).why == "the mesh has no geometry");
}

TEST_CASE("Mesh bake resample: tiling is drawn out, so the atlas is larger than the source")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	MeshInput m;
	addQuad(m, 0, 0, 0, 2, 2, baseOnly("A"));    // repeats twice each way
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
	Options o; o.mode = Mode::Resample;
	const Result r = bake(m, sc.lookup(), o);
	REQUIRE_MESSAGE(r.ok, r.why);
	CHECK_FALSE(r.lossless);
	int cx, cy;
	cornerOf(r, r.base.image, 0, cx, cy);
	// The 16 x 16 rect is the 8 x 8 texture twice over, texel for texel.
	for (uint32_t y = 0; y < 16; ++y)
		for (uint32_t x = 0; x < 16; ++x)
			for (int c = 0; c < 2; ++c)
				CHECK(px(r.base.image, uint32_t(cx) + x, uint32_t(cy) + y)[c] == px(sc.images["A"], x % 8, y % 8)[c]);
	// The vertex at UV (2, 2) lands on the far corner of that rect.
	const uint32_t far = r.indices[2];
	CHECK(r.uvs[far * 2] * float(r.base.image.width) == doctest::Approx(float(cx + 16)));
}

TEST_CASE("Mesh bake resample: a tint is folded into the texels, and a role one section lacks is filled flat")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	sc.images["N"] = pattern(8, 8, 200, 100, 220);
	MeshInput m;
	SimpleTextureMaterial a = baseOnly("A");
	a.baseColor.factor[0] = 0.5f; a.baseColor.factor[1] = 1.0f; a.baseColor.factor[2] = 1.0f;
	a.normal.path = "N";
	a.normalStrength = 1.0f;
	addQuad(m, 0, 0, 0, 1, 1, a);
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
	const Result r = bake(m, sc.lookup(), Options{});
	REQUIRE_MESSAGE(r.ok, r.why);
	int cx, cy;
	cornerOf(r, r.base.image, 0, cx, cy);
	CHECK(px(r.base.image, uint32_t(cx) + 3, uint32_t(cy) + 2)[0] == doctest::Approx(0.5f * px(sc.images["A"], 3, 2)[0]).epsilon(0.01));
	CHECK(px(r.base.image, uint32_t(cx) + 3, uint32_t(cy) + 2)[1] == px(sc.images["A"], 3, 2)[1]);
	CHECK(r.material.baseColor.factor[0] == doctest::Approx(1.0f));   // folded, not carried
	// The normal atlas exists; the section without a normal map reads a flat normal.
	REQUIRE(r.normal.atlas);
	CHECK(r.material.normal.path == kBakedNormal);
	int bx, by;
	cornerOf(r, r.normal.image, 1, bx, by);
	CHECK(px(r.normal.image, uint32_t(bx) + 4, uint32_t(by) + 4)[0] == 128);
	CHECK(px(r.normal.image, uint32_t(bx) + 4, uint32_t(by) + 4)[1] == 128);
	CHECK(px(r.normal.image, uint32_t(bx) + 4, uint32_t(by) + 4)[2] == 255);
}

TEST_CASE("Mesh bake resample: occlusion, roughness and metallic share one ORM atlas, constants included")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	sc.images["ORM"] = pattern(8, 8, 60, 70, 80);
	MeshInput m;
	SimpleTextureMaterial a = baseOnly("A");
	a.ao.tex.path = a.roughness.tex.path = a.metallic.tex.path = "ORM";
	a.ao.channel = 0; a.roughness.channel = 1; a.metallic.channel = 2;
	a.ao.factor = 1.0f; a.metallic.factor = 1.0f;    // what the classifier gives a textured role
	a.roughness.factor = 0.5f;                       // folded
	addQuad(m, 0, 0, 0, 1, 1, a);
	SimpleTextureMaterial b = baseOnly("B");
	b.roughness.factor = 0.3f; b.metallic.factor = 0.1f; b.ao.factor = 1.0f;     // constants
	addQuad(m, 2, 0, 0, 1, 1, b);
	const Result r = bake(m, sc.lookup(), Options{});
	REQUIRE_MESSAGE(r.ok, r.why);
	REQUIRE(r.orm.atlas);
	CHECK(r.material.ao.tex.path == kBakedOrm);
	CHECK(r.material.ao.channel == 0);
	CHECK(r.material.roughness.channel == 1);
	CHECK(r.material.metallic.channel == 2);
	int ax, ay;
	cornerOf(r, r.orm.image, 0, ax, ay);
	const uint8_t* t = px(r.orm.image, uint32_t(ax) + 2, uint32_t(ay) + 3);
	CHECK(t[0] == px(sc.images["ORM"], 2, 3)[0]);                                   // R = occlusion
	CHECK(t[1] == doctest::Approx(0.5f * px(sc.images["ORM"], 2, 3)[1]).epsilon(0.03)); // G = roughness x 0.5
	CHECK(t[2] == px(sc.images["ORM"], 2, 3)[2]);                                   // B = metallic
	int bx, by;
	cornerOf(r, r.orm.image, 1, bx, by);
	const uint8_t* f = px(r.orm.image, uint32_t(bx) + 4, uint32_t(by) + 4);
	CHECK(f[0] == 255);
	CHECK(f[1] == doctest::Approx(0.3f * 255).epsilon(0.01));
	CHECK(f[2] == doctest::Approx(0.1f * 255).epsilon(0.02));
}

TEST_CASE("Mesh bake: sections that differ in shading, or are not simple, stay as they are")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	sc.images["C"] = pattern(8, 8, 1, 2, 3);
	MeshInput m;
	addQuad(m, 0, 0, 0, 1, 1, baseOnly("A"));
	SimpleTextureMaterial twoSided = baseOnly("C");
	twoSided.doubleSided = true;
	addQuad(m, 2, 0, 0, 1, 1, twoSided);               // different side-ness: kept
	addQuad(m, 4, 0, 0, 1, 1, baseOnly("B"));
	SectionInput glass;                                // a section the classifier refused
	glass.simple = false;
	glass.indexOffset = uint32_t(m.indices.size());
	for (uint32_t i : { 0u, 1u, 2u }) m.indices.push_back(i);
	glass.indexCount = 3;
	m.sections.push_back(glass);

	const Result r = bake(m, sc.lookup(), Options{});
	REQUIRE_MESSAGE(r.ok, r.why);
	CHECK(r.mergedSections == 2);
	CHECK(r.mergedIndexCount == 12);
	REQUIRE(r.keptSections.size() == 2);
	CHECK(r.keptSections[0].inputSection == 1);
	CHECK(r.keptSections[0].indexOffset == 12);
	CHECK(r.keptSections[0].indexCount == 6);
	CHECK(r.keptSections[1].inputSection == 3);
	CHECK(r.keptSections[1].indexOffset == 18);
	CHECK(r.keptSections[1].indexCount == 3);
	CHECK(r.indices.size() == 21);
	// A kept section keeps its UVs as they were (it still uses its own material).
	const uint32_t keptVertex = r.indices[12 + 2];
	CHECK(r.uvs[keptVertex * 2] == doctest::Approx(1.0f));
	CHECK(r.uvs[keptVertex * 2 + 1] == doctest::Approx(1.0f));
}

TEST_CASE("Mesh bake: a vertex two sections share is copied, so each section can carry its own UV")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	MeshInput m;
	// Three vertices, two triangles that share vertex 1, one per section.
	m.vertices = { 0,0,0,  1,0,0,  2,0,0,  1,1,0 };
	m.normals  = { 0,0,1,  0,0,1,  0,0,1,  0,0,1 };
	m.uvs      = { 0,0,  1,0,  1,1,  0,1 };
	m.indices  = { 0, 1, 3,   1, 2, 3 };
	SectionInput s0, s1;
	s0.simple = s1.simple = true;
	s0.indexOffset = 0; s0.indexCount = 3; s0.material = baseOnly("A");
	s1.indexOffset = 3; s1.indexCount = 3; s1.material = baseOnly("B");
	m.sections = { s0, s1 };
	const Result r = bake(m, sc.lookup(), Options{});
	REQUIRE_MESSAGE(r.ok, r.why);
	CHECK(r.vertices.size() / 3 == 6);                 // 3 + 3, none shared any more
	CHECK(r.indices[1] != r.indices[3]);               // old vertex 1, once per section
	CHECK((r.uvs[r.indices[1] * 2] != r.uvs[r.indices[3] * 2]
	       || r.uvs[r.indices[1] * 2 + 1] != r.uvs[r.indices[3] * 2 + 1]));
}

TEST_CASE("Mesh bake: an sRGB colour atlas is filtered and folded in linear light")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50, /*srgb=*/true);
	sc.images["B"] = pattern(8, 8, 100, 40, 90, /*srgb=*/true);
	MeshInput m;
	addQuad(m, 0, 0, 0, 1, 1, baseOnly("A"));
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
	Options o; o.mode = Mode::Merge;
	const Result lossless = bake(m, sc.lookup(), o);
	REQUIRE_MESSAGE(lossless.ok, lossless.why);
	CHECK(lossless.base.image.srgb);
	int cx, cy;
	cornerOf(lossless, lossless.base.image, 0, cx, cy);
	// The decode/encode round trip must not move a single byte.
	for (uint32_t y = 0; y < 8; ++y)
		for (uint32_t x = 0; x < 8; ++x)
			for (int c = 0; c < 3; ++c)
				CHECK(px(lossless.base.image, uint32_t(cx) + x, uint32_t(cy) + y)[c] == px(sc.images["A"], x, y)[c]);

	// A tint of 0.5 halves the LINEAR value, not the stored one.
	MeshInput t;
	SimpleTextureMaterial tinted = baseOnly("A");
	tinted.baseColor.factor[0] = tinted.baseColor.factor[1] = tinted.baseColor.factor[2] = 0.5f;
	addQuad(t, 0, 0, 0, 1, 1, tinted);
	addQuad(t, 2, 0, 0, 1, 1, baseOnly("B"));
	const Result folded = bake(t, sc.lookup(), Options{});
	REQUIRE_MESSAGE(folded.ok, folded.why);
	cornerOf(folded, folded.base.image, 0, cx, cy);
	const float stored = float(px(sc.images["A"], 5, 5)[0]) / 255.0f;
	const float lin = stored <= 0.04045f ? stored / 12.92f : std::pow((stored + 0.055f) / 1.055f, 2.4f);
	const float half = 0.5f * lin;
	const float expect = (half <= 0.0031308f ? half * 12.92f : 1.055f * std::pow(half, 1.0f / 2.4f) - 0.055f) * 255.0f;
	CHECK(float(px(folded.base.image, uint32_t(cx) + 5, uint32_t(cy) + 5)[0]) == doctest::Approx(expect).epsilon(0.02));
}

TEST_CASE("Mesh bake: a mesh that needs more than a quarter of detail loss is refused, not blurred")
{
	Scene sc;
	sc.images["A"] = pattern(8, 8, 10, 10, 50);
	sc.images["B"] = pattern(8, 8, 100, 40, 90);
	MeshInput m;
	addQuad(m, 0, 0, 0, 8000, 8000, baseOnly("A"));    // 64000 x 64000 texels at full detail
	addQuad(m, 2, 0, 0, 1, 1, baseOnly("B"));
	const Result r = bake(m, sc.lookup(), Options{});
	CHECK_FALSE(r.ok);
	CHECK(r.why == "the atlas would need less than a quarter of the texture detail");
}
