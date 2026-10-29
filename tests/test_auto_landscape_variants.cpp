// Auto landscape material 2.0, step 3 (Thema 180): several texture variants per layer.
//
//   * the Groups pin of the array nodes: 0 / unwired keeps the shader text byte for byte;
//     wired, every hex of a bombed read draws its own variant, without one more texture read;
//   * the variant a hex draws (the low 8 bits of its hash, scaled to 0..V-1 in integers) is
//     evenly distributed, per layer seed, independent between layers;
//   * layer g's variant v is slice g * V + v, V = depth / 4, WetGround last; a shorter array
//     can never be read outside itself;
//   * the arrays assemble in that order (HE::buildTextureArray + autoLandscapeSliceSource).
#include "doctest.h"

#include <ContentManager/Assets.h>
#include <ContentManager/TextureArrayBuild.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/WeatherMaterialFunctions.h>
#include <material/MaterialShaderLibrary.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using HE::MaterialGraph;
using HE::MatNodeType;

namespace
{
size_t countOf(const std::string& s, const std::string& what)
{
	size_t n = 0;
	for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
	return n;
}

uint64_t fnv1a(const std::string& s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
	return h;
}

// A layer read the way the auto material reads it, with the Groups pin left alone: an array
// (bombed or plain) Albedo and Normal on one UV and slice. The text of this graph before the
// Groups pin existed is what the byte-identity check below pins.
MaterialGraph makeArrayGraph(bool plain)
{
	MaterialGraph g;
	const int out = g.addNode(MatNodeType::Output);
	const int uv = g.addNode(MatNodeType::UV);
	g.findNode(uv)->p[0] = g.findNode(uv)->p[1] = 8.0f;
	const int slice = g.addNode(MatNodeType::ConstFloat);
	g.findNode(slice)->p[0] = 2.0f;
	const int alb = g.addNode(plain ? MatNodeType::TextureArraySample : MatNodeType::TextureArrayBombSample);
	g.findNode(alb)->s = "Textures/T_Albedo_Array.hasset";
	const int nrm = g.addNode(plain ? MatNodeType::NormalMapArraySample : MatNodeType::NormalMapArrayBombSample);
	g.findNode(nrm)->s = "Textures/T_Normal_Array.hasset";
	for (int n : { alb, nrm })
	{
		CHECK(g.connect(uv, 0, n, 0));
		CHECK(g.connect(slice, 0, n, 1));
	}
	CHECK(g.connect(alb, 0, out, HE::kMatOutputBaseColorPin));
	CHECK(g.connect(nrm, 0, out, HE::kMatOutputNormalPin));
	return g;
}

// The same graph with a constant Groups value on the Groups pin of both nodes.
MaterialGraph makeGroupedGraph(bool plain, float groups)
{
	MaterialGraph g = makeArrayGraph(plain);
	const int k = g.addNode(MatNodeType::ConstFloat);
	g.findNode(k)->p[0] = groups;
	const int pin = plain ? 2 : 3;
	for (const auto& n : g.nodes)
		if (n.type == MatNodeType::TextureArraySample || n.type == MatNodeType::NormalMapArraySample
		    || n.type == MatNodeType::TextureArrayBombSample || n.type == MatNodeType::NormalMapArrayBombSample)
			CHECK(g.connect(k, 0, n.id, pin));
	return g;
}

// heBombHash on the CPU, line for line (MaterialGraph.cpp, the pcg3d the bombing nodes share):
// uvec3 v = uvec3(uvec2(cell), seed) * 1664525u + 1013904223u; ... Only .x is needed here.
std::array<uint32_t, 3> bombHash(int32_t cx, int32_t cy, uint32_t seed)
{
	std::array<uint32_t, 3> v = { static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), seed };
	for (uint32_t& c : v) c = c * 1664525u + 1013904223u;
	v[0] += v[1] * v[2]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1];
	for (uint32_t& c : v) c ^= c >> 16u;
	v[0] += v[1] * v[2]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1];
	return v;
}

int variantOfCell(int32_t cx, int32_t cy, uint32_t seed, int variants)
{
	return HE::autoLandscapeVariantOf(bombHash(cx, cy, seed)[0], variants);
}
} // namespace

TEST_CASE("Groups unwired: the array nodes emit exactly the text they always did")
{
	// FNV-1a of glsl + "\n//GB\n" + glslGBuffer of makeArrayGraph() through generateFragment, taken
	// from a build of e6d4920d, the commit before the Groups pin existed. Any change here changes
	// every existing graph's text and with it their pipeline caches.
	for (const bool plain : { false, true })
	{
		CAPTURE(plain);
		const HE::MatShaderGen gen = HE::generateFragment(makeArrayGraph(plain));
		const uint64_t h = fnv1a(gen.glsl + "\n//GB\n" + gen.glslGBuffer);
		CHECK(h == (plain ? 0x38da6e5dc31502aaULL : 0xcebe9db1000ebfb8ULL));
		CHECK(gen.glsl.find("heBombGridV") == std::string::npos);
		CHECK(gen.glsl.find("heBombTapV") == std::string::npos);
	}
	// A save/load round trip of the old graph keeps the text.
	const MaterialGraph g = makeArrayGraph(false);
	MaterialGraph back;
	REQUIRE(HE::materialGraphFromJson(HE::materialGraphToJson(g), back));
	CHECK(HE::generateFragment(back).glsl == HE::generateFragment(g).glsl);
}

TEST_CASE("Groups wired: a variant per hex, no extra texture read")
{
	const HE::MatShaderGen plainBomb = HE::generateFragment(makeArrayGraph(false));
	const HE::MatShaderGen grouped   = HE::generateFragment(makeGroupedGraph(false, 4.0f));

	// Six taps (2 nodes x 3 hexes) per pixel either way: the variant is picked from numbers the hash
	// already holds. The grouped text holds them twice, once per side of `if (<v>_V == 1)`: V = 1
	// reads the layer with the plain taps, V > 1 reads the slices the hash picked (Thema 237).
	CHECK(countOf(plainBomb.glsl, "textureGrad(heTexP") == 6u);
	CHECK(countOf(grouped.glsl, "textureGrad(heTexP") == 12u);
	CHECK(countOf(grouped.glsl, "_V == 1) {") == 2u);
	CHECK(countOf(grouped.glsl, "_s0))") == 2u); // the slice is read on the V > 1 side only
	// One variant grid shared by the Albedo and the Normal read of the layer, no plain grid.
	CHECK(countOf(grouped.glsl, "void heBombGridV(") == 1u);
	CHECK(countOf(grouped.glsl, "heBombGridV(n") == 1u);
	CHECK(grouped.glsl.find("void heBombGrid(") == std::string::npos);
	CHECK(grouped.glsl.find("heBombGrid(n") == std::string::npos);
	// The hash, the integer arithmetic, the clamp into the array.
	CHECK(grouped.glsl.find("uvec3 heBombHash(ivec2 cell, uint seed)") != std::string::npos);
	CHECK(grouped.glsl.find("sel = h.x & 255u;") != std::string::npos);
	CHECK(grouped.glsl.find("& 255u) * uint(") != std::string::npos);
	CHECK(grouped.glsl.find(">> 8u), 0, textureSize(heTexP") != std::string::npos);
	CHECK(grouped.glsl.find("> 0 ? max(textureSize(heTexP0, 0).z / int(floor(n") != std::string::npos);
	CHECK(grouped.glsl.find("fract(sin") == std::string::npos);
	// The rounding rule of the group stays the one the plain nodes use.
	CHECK(grouped.glsl.find("= clamp(floor(") != std::string::npos);
	// The G-buffer variant carries the same body.
	CHECK(countOf(grouped.glslGBuffer, "heBombGridV(n") == 1u);

	// Unbombed (the plain nodes, what the "Texture Bombing" switch off leaves): variant 0 of the
	// group, slice = group * V, still one read per node.
	const HE::MatShaderGen plain = HE::generateFragment(makeGroupedGraph(true, 4.0f));
	CHECK(countOf(plain.glsl, "texture(heTexP") == 2u);
	CHECK(plain.glsl.find("heBombGridV") == std::string::npos);
	CHECK(plain.glsl.find("* float(n") != std::string::npos);
	CHECK(plain.glsl.find("> 0 ? max(textureSize(heTexP0, 0).z / int(floor(n") != std::string::npos);
}

TEST_CASE("The auto landscape graph reads variants: 27 hex reads, one variant grid per layer")
{
	const MaterialGraph g = HE::buildAutoLandscapeGraph().graph;
	const HE::MatShaderGen lit = HE::generateFragment(g, HE::weatherFunctionLoader());
	REQUIRE(lit.textures.size() == 4);   // the three arrays + Path's own Albedo (Thema 329, step 3)
	// 3 layers (Grass, Dirt, Rock) x (Albedo + Normal + Mask) x 3 hexes = 27 reads per pixel (36 while Snow was
	// a bombed texture layer too; since Thema 329 step 2 it is MF_ProceduralSnow and reads nothing); the text holds
	// them once for V = 1 and once for V > 1 (see the grouped test above).
	CHECK(countOf(lit.glsl, "textureGrad(heTexP") == 54u);
	CHECK(countOf(lit.glsl, "_V == 1) {") == 9u);
	CHECK(countOf(lit.glsl, "_s0))") == 9u);
	CHECK(countOf(lit.glsl, "heBombGridV(") == 4u);    // the definition and one call per layer (three seeds)
	CHECK(lit.glsl.find("heBombGrid(n") == std::string::npos);
	for (const char* seed : { ", 11u, n", ", 23u, n", ", 37u, n" })
		CHECK_MESSAGE(lit.glsl.find(seed) != std::string::npos, seed);
	CHECK(lit.glsl.find(", 53u, n") == std::string::npos);   // the snow's old seed: no bombed snow read left
	// No new parameter, no new texture slot.
	CHECK(lit.params.size() == static_cast<size_t>(HE::kAutoLandscapeParamCount));

	// The negative control: bombing off reads variant 0 of every layer with plain reads.
	const std::map<std::string, bool> noBomb{ { HE::kAutoLandscapeSwitchBombing, false } };
	const HE::MatShaderGen off = HE::generateFragment(g, HE::weatherFunctionLoader(), &noBomb);
	CHECK(off.glsl.find("heBombGridV") == std::string::npos);
	CHECK(off.glsl.find("textureGrad(heTexP") == std::string::npos);
	CHECK(off.params.size() == static_cast<size_t>(HE::kAutoLandscapeParamCount) - 1);
}

TEST_CASE("The variant helpers cross-compile for all five backends")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	for (const MaterialGraph& g : { makeGroupedGraph(false, 4.0f), makeGroupedGraph(true, 4.0f),
	                                HE::buildAutoLandscapeGraph().graph })
	{
		const HE::MatShaderGen gen = HE::generateFragment(g, HE::weatherFunctionLoader());
		const uint64_t hash = std::hash<std::string>{}(gen.glsl);
		for (B b : { B::Metal, B::GLSL410, B::GLSLES300, B::GLSL430, B::HLSL, B::SpirV })
		{
			const auto& r = lib.fragment(hash, gen.glsl, b);
			CHECK_MESSAGE(r.ok, "backend ", static_cast<int>(b), ": ", r.log);
		}
		const std::string hlsl = lib.fragment(hash, gen.glsl, B::HLSL).source;
		const std::string msl  = lib.fragment(hash, gen.glsl, B::Metal).source;
		// The integer arithmetic survives translation as integers (a float path would round
		// differently per API): a mask with 255 and a shift by 8 on unsigned ints.
		if (gen.glsl.find("heBombGridV") != std::string::npos)
		{
			CHECK_MESSAGE(hlsl.find("& 255u") != std::string::npos, hlsl);
			CHECK_MESSAGE(msl.find("& 255u") != std::string::npos, msl);
			CHECK_MESSAGE(hlsl.find(">> 8u") != std::string::npos, hlsl);
		}
		// Arrays still cost no slot beyond the three the material had.
		REQUIRE(gen.textures.size() <= 4);
	}
}

// Opt-in witness (Thema 237): HE_DUMP_AUTOLAND_SHADERS=<dir> writes the auto landscape fragment text
// (lit + G-buffer GLSL, and the lit one cross-compiled to MSL / HLSL) so two builds can be diffed.
TEST_CASE("Dump the auto landscape shaders" * doctest::skip(std::getenv("HE_DUMP_AUTOLAND_SHADERS") == nullptr))
{
	const std::string dir = std::getenv("HE_DUMP_AUTOLAND_SHADERS");
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const HE::MatShaderGen gen = HE::generateFragment(HE::buildAutoLandscapeGraph().graph, HE::weatherFunctionLoader());
	const uint64_t hash = std::hash<std::string>{}(gen.glsl);
	std::ofstream(dir + "/lit.glsl") << gen.glsl;
	std::ofstream(dir + "/gbuffer.glsl") << gen.glslGBuffer;
	std::ofstream(dir + "/lit.msl") << lib.fragment(hash, gen.glsl, B::Metal).source;
	std::ofstream(dir + "/lit.hlsl") << lib.fragment(hash, gen.glsl, B::HLSL).source;
}

TEST_CASE("The variant a hex draws is evenly distributed, per layer seed")
{
	// 10^5 hex cells per seed; chi-square against the uniform distribution. The critical value
	// for df = V - 1 at p = 0.001 is 10.83 (V = 2), 13.82 (3), 16.27 (4), 18.47 (5); a deterministic
	// hash, so this either always passes or always fails.
	const int cells = 316;                       // 316^2 = 99856
	const double crit[] = { 0.0, 0.0, 10.83, 13.82, 16.27 };
	for (const uint32_t seed : { 11u, 23u, 37u, 53u })
		for (int variants = 2; variants <= 4; ++variants)
		{
			std::vector<int> bin(static_cast<size_t>(variants), 0);
			for (int y = -cells / 2; y < cells / 2; ++y)
				for (int x = -cells / 2; x < cells / 2; ++x)
					++bin[static_cast<size_t>(variantOfCell(x, y, seed, variants))];
			const double expect = static_cast<double>(cells) * cells / variants;
			double chi = 0.0;
			for (int c : bin) chi += (c - expect) * (c - expect) / expect;
			CHECK_MESSAGE(chi < crit[variants], "seed ", seed, " V ", variants, " chi2 ", chi);
		}
	// V = 1 (the placeholder arrays): always variant 0, whatever the hash says.
	for (int i = 0; i < 1000; ++i)
		CHECK(variantOfCell(i, -i * 3, 11u, 1) == 0);
}

TEST_CASE("Layers choose their variants independently, and neighbouring hexes do not repeat")
{
	// The four seeds are the auto material's. Albedo, Normal and Mask of one layer share the
	// seed (the same grid), so they draw the same variant by construction; two LAYERS must not,
	// or a rock variant would always sit on the same hexes as a grass variant.
	const int variants = 3, cells = 200;
	int agree = 0, total = 0, sameAsRight = 0;
	for (int y = 0; y < cells; ++y)
		for (int x = 0; x < cells; ++x)
		{
			const int a = variantOfCell(x, y, 11u, variants);
			const int b = variantOfCell(x, y, 23u, variants);
			agree += a == b;
			sameAsRight += a == variantOfCell(x + 1, y, 11u, variants);
			++total;
		}
	// Independent uniform choices of 3: they agree on a third of the cells (+- 3 %), as do
	// neighbours within one layer.
	CHECK(std::abs(static_cast<double>(agree) / total - 1.0 / 3.0) < 0.03);
	CHECK(std::abs(static_cast<double>(sameAsRight) / total - 1.0 / 3.0) < 0.03);
}

TEST_CASE("Layer g, variant v is slice g * V + v; a shorter array never reads outside itself")
{
	using HE::autoLandscapeSliceOf;
	using HE::autoLandscapeVariantCount;
	// V from the depth, the way the shader divides it.
	CHECK(autoLandscapeVariantCount(5) == 1);    // the placeholder arrays
	CHECK(autoLandscapeVariantCount(9) == 2);
	CHECK(autoLandscapeVariantCount(13) == 3);   // the target set
	CHECK(autoLandscapeVariantCount(17) == 4);
	CHECK(autoLandscapeVariantCount(3) == 1);    // a damaged, too short array still yields V >= 1
	for (int v : { 1, 2, 3, 4 }) CHECK(HE::autoLandscapeArrayDepth(v) == 4 * v + 1);

	for (const int variants : { 1, 2, 3, 4 })
	{
		const int depth = HE::autoLandscapeArrayDepth(variants);
		std::vector<int> seen(static_cast<size_t>(depth), 0);
		for (int g = 0; g < 4; ++g)
			for (int v = 0; v < variants; ++v)
			{
				const int s = autoLandscapeSliceOf(g, v, depth);
				CHECK(s == g * variants + v);
				REQUIRE(s >= 0);
				REQUIRE(s < depth);
				++seen[static_cast<size_t>(s)];
			}
		// Every slice of the four painted layers exactly once; the last one is WetGround, unread.
		for (int s = 0; s < 4 * variants; ++s) CHECK(seen[static_cast<size_t>(s)] == 1);
		CHECK(seen[static_cast<size_t>(4 * variants)] == 0);
		// ... and the inverse used by the array packer agrees.
		for (int s = 0; s < depth; ++s)
		{
			int layer = -1, variant = -1;
			HE::autoLandscapeSliceSource(s, variants, layer, variant);
			if (s < 4 * variants) { CHECK(layer == s / variants); CHECK(variant == s % variants); }
			else                  { CHECK(layer == 4); CHECK(variant == 0); }
		}
	}
	// V = 1 is today's layout: slice = layer.
	for (int g = 0; g < 5; ++g) CHECK(autoLandscapeSliceOf(g, 0, 5) == g);

	// Whatever hash value and group: the slice the shader computes is inside the array, for
	// every depth, also for a group number past the last (the clamp), and for arrays that do not
	// match the V the material was authored for.
	for (int depth : { 1, 2, 3, 5, 9, 13, 17 })
	{
		const int variants = autoLandscapeVariantCount(depth);
		for (int g = 0; g < 6; ++g)
			for (uint32_t h = 0; h < 256; h += 5)
			{
				const int s = autoLandscapeSliceOf(g, HE::autoLandscapeVariantOf(h, variants), depth);
				CHECK(s >= 0);
				CHECK(s < depth);
			}
	}
	// The variant number itself: 0 .. V-1 for every low byte.
	for (int variants : { 1, 2, 3, 4, 5 })
		for (uint32_t h = 0; h < 256; ++h)
		{
			const int v = HE::autoLandscapeVariantOf(h * 0x01010100u + h, variants);
			CHECK(v >= 0);
			CHECK(v < variants);
		}
}

TEST_CASE("The arrays assemble in that order: variants of a layer next to each other, WetGround last")
{
	// 4x4 RGBA8 slices whose red channel encodes (layer, variant), through the very function the
	// packer uses; then read each slice back out of the array's payload.
	const int w = 4, h = 4;
	for (const int variants : { 1, 3 })
	{
		const int depth = HE::autoLandscapeArrayDepth(variants);
		std::vector<TextureAsset> src(static_cast<size_t>(depth));
		std::vector<const TextureAsset*> ptrs;
		for (int s = 0; s < depth; ++s)
		{
			int layer = 0, variant = 0;
			HE::autoLandscapeSliceSource(s, variants, layer, variant);
			TextureAsset& t = src[static_cast<size_t>(s)];
			t.type = HE::AssetType::Texture;
			t.width = w; t.height = h; t.channels = 4; t.mipLevels = 1;
			t.format = TextureFormat::RGBA8;
			t.srgb = true;
			t.data.assign(static_cast<size_t>(w) * h * 4, 0);
			for (size_t i = 0; i < t.data.size(); i += 4)
			{
				t.data[i]     = static_cast<uint8_t>(10 * layer + variant);
				t.data[i + 3] = 255;
			}
			ptrs.push_back(&t);
		}
		TextureAsset arr;
		std::string err;
		REQUIRE_MESSAGE(HE::buildTextureArray(ptrs, arr, /*bakeMips=*/true, &err), err);
		CHECK(arr.layers == static_cast<uint32_t>(depth));
		CHECK(HE::textureArrayPayloadValid(arr));
		for (int g = 0; g < 4; ++g)
			for (int v = 0; v < variants; ++v)
			{
				const int s = HE::autoLandscapeSliceOf(g, v, depth);
				const size_t at = HE::textureArrayOffset(arr.width, arr.height, arr.mipLevels, static_cast<uint32_t>(s), 0);
				CHECK_MESSAGE(arr.data[at] == 10 * g + v, "layer ", g, " variant ", v, " slice ", s);
			}
		const size_t wet = HE::textureArrayOffset(arr.width, arr.height, arr.mipLevels, static_cast<uint32_t>(depth - 1), 0);
		CHECK(arr.data[wet] == 40);   // layer 4, variant 0
	}
	// Slices of different sizes do not make an array (the packer resizes before it gets here).
	std::vector<TextureAsset> bad(2);
	std::vector<const TextureAsset*> ptrs;
	for (size_t i = 0; i < bad.size(); ++i)
	{
		const uint32_t s = i ? 8 : 4;
		bad[i].width = bad[i].height = s; bad[i].channels = 4; bad[i].mipLevels = 1;
		bad[i].format = TextureFormat::RGBA8;
		bad[i].data.assign(static_cast<size_t>(s) * s * 4, 255);
		ptrs.push_back(&bad[i]);
	}
	TextureAsset out;
	CHECK_FALSE(HE::buildTextureArray(ptrs, out, true));
}
