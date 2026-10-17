// REN-101 / REN-1001 (docs/audit/modul_rendering.md): every graph-material draw on all
// five backends asks MaterialShaderLibrary::resolveShaders for its (hash, source) pair,
// and the hash is the key of every shader / PSO / pipeline cache behind it. These
// cases pin the key bit for bit against the formula the backends have always keyed
// on, prove the key follows every way a material's shader text changes, and time
// 1000 resolves (one frame of 1000 graph-material draws) for the record.
#include "doctest.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/WeatherMaterialFunctions.h>
#include <material/MaterialShaderLibrary.h>


namespace
{
// The key as the backends have computed it since the WPO fold landed: hash of the
// fragment, the vertex body folded in only when there is one. Spelled out here so
// a change in MaterialShaderLibrary cannot move the key and this test with it.
uint64_t referenceKey(const std::string& frag, const std::string& vert)
{
	uint64_t h = std::hash<std::string>{}(frag);
	if (!vert.empty()) h ^= std::hash<std::string>{}(vert) * 0x9E3779B97F4A7C15ULL;
	return h;
}

// The auto-landscape material: the biggest graph the engine ships, so the most text
// a resolve has ever had to copy and hash.
MaterialAsset landscapeMaterial()
{
	const HE::MatShaderGen gen = HE::generateFragment(HE::buildAutoLandscapeGraph().graph,
	                                                  HE::weatherFunctionLoader());
	MaterialAsset m;
	m.type = HE::AssetType::Material;
	m.name = "Landscape";
	m.path = "mats/landscape.hasset";
	m.customShaderFragGlsl = gen.glsl;
	m.customShaderGBufGlsl = gen.glslGBuffer;
	m.customShaderVertGlsl = gen.vertexBody;
	return m;
}

// A small graph with a World Position Offset, so the vertex fold is exercised.
MaterialAsset wpoMaterial()
{
	HE::MaterialGraph g;
	const int out = g.addNode(HE::MatNodeType::Output);
	const int t   = g.addNode(HE::MatNodeType::Time);
	const int s   = g.addNode(HE::MatNodeType::Sine);
	REQUIRE(g.connect(t, 0, s, 0));
	REQUIRE(g.connect(s, 0, out, HE::kMatOutputBaseColorPin));
	REQUIRE(g.connect(s, 0, out, HE::kMatOutputWPOPin));
	const HE::MatShaderGen gen = HE::generateFragment(g);
	MaterialAsset m;
	m.type = HE::AssetType::Material;
	m.name = "Wobble";
	m.path = "mats/wobble.hasset";
	m.customShaderFragGlsl = gen.glsl;
	m.customShaderGBufGlsl = gen.glslGBuffer;
	m.customShaderVertGlsl = gen.vertexBody;
	return m;
}

MaterialAsset builtinMaterial()
{
	MaterialAsset m;
	m.type = HE::AssetType::Material;
	m.name = "Plain";
	m.path = "mats/plain.hasset";
	return m;
}
} // namespace

TEST_CASE("resolveShaders keys a material exactly as the backends always have")
{
	ContentManager cm;
	const MaterialAsset land = landscapeMaterial();
	const MaterialAsset wpo  = wpoMaterial();
	REQUIRE(!land.customShaderFragGlsl.empty());
	REQUIRE(!land.customShaderGBufGlsl.empty());
	REQUIRE(!wpo.customShaderVertGlsl.empty());
	const HE::UUID landId  = cm.registerMaterial(land);
	const HE::UUID wpoId   = cm.registerMaterial(wpo);
	const HE::UUID plainId = cm.registerMaterial(builtinMaterial());

	HE::MaterialShaderLibrary lib;
	for (int pass = 0; pass < 2; ++pass) // first resolve and the repeat a frame later
	{
		CAPTURE(pass);
		for (const auto& [id, src] : { std::pair{ landId, &land }, std::pair{ wpoId, &wpo } })
		{
			uint64_t h = 0; std::string frag, vert;
			REQUIRE(lib.resolveShaders(cm, id, h, frag, vert));
			CHECK(h == referenceKey(src->customShaderFragGlsl, src->customShaderVertGlsl));
			CHECK(frag == src->customShaderFragGlsl);
			CHECK(vert == src->customShaderVertGlsl);

			uint64_t f = 0; std::string fragOnly;
			REQUIRE(lib.resolveFragment(cm, id, f, fragOnly));
			CHECK(f == std::hash<std::string>{}(src->customShaderFragGlsl));
			CHECK(fragOnly == src->customShaderFragGlsl);

			uint64_t g = 0; std::string gbFrag, gbVert;
			REQUIRE(lib.resolveGBufferShaders(cm, id, g, gbFrag, gbVert));
			CHECK(g == referenceKey(src->customShaderGBufGlsl, src->customShaderVertGlsl));
			CHECK(gbFrag == src->customShaderGBufGlsl);
			CHECK(gbVert == src->customShaderVertGlsl);
			CHECK(g != h); // forward and G-buffer pipelines never share a key

			// The draw-loop form: the same keys, and the asset's own strings, no copy.
			const MaterialAsset* asset = cm.getMaterial(id);
			uint64_t hp = 0; const std::string* fp = nullptr; const std::string* vp = nullptr;
			REQUIRE(lib.resolveShaders(cm, id, hp, fp, vp));
			CHECK(hp == h);
			CHECK(fp == &asset->customShaderFragGlsl);
			CHECK(vp == &asset->customShaderVertGlsl);
			uint64_t gp = 0;
			REQUIRE(lib.resolveGBufferShaders(cm, id, gp, fp, vp));
			CHECK(gp == g);
			CHECK(fp == &asset->customShaderGBufGlsl);
			uint64_t fpHash = 0;
			REQUIRE(lib.resolveFragment(cm, id, fpHash, fp));
			CHECK(fpHash == f);
		}
		uint64_t h = 0; std::string frag, vert;
		CHECK_FALSE(lib.resolveShaders(cm, plainId, h, frag, vert));
		CHECK_FALSE(lib.resolveGBufferShaders(cm, plainId, h, frag, vert));
		CHECK_FALSE(lib.resolveShaders(cm, HE::UUID{}, h, frag, vert));
		CHECK_FALSE(lib.resolveShaders(cm, HE::UUID::generate(), h, frag, vert));
		const std::string* fp = nullptr; const std::string* vp = nullptr;
		CHECK_FALSE(lib.resolveShaders(cm, plainId, h, fp, vp));
		CHECK_FALSE(lib.resolveGBufferShaders(cm, plainId, h, fp, vp));
	}
}

TEST_CASE("resolveShaders follows every edit of a registered material's shader text")
{
	ContentManager cm;
	const HE::UUID id = cm.registerMaterial(wpoMaterial());
	HE::MaterialShaderLibrary lib;
	auto key = [&](uint64_t& h) {
		const std::string* f = nullptr; const std::string* v = nullptr;
		return lib.resolveShaders(cm, id, h, f, v);
	};
	auto gkey = [&](uint64_t& h) {
		const std::string* f = nullptr; const std::string* v = nullptr;
		return lib.resolveGBufferShaders(cm, id, h, f, v);
	};

	uint64_t h0 = 0, g0 = 0;
	REQUIRE(key(h0));
	REQUIRE(gkey(g0));

	// A same-length edit in place: what a graph edit that only changes a constant
	// (0.5 → 0.7) produces, and the one change a string's address and size do not see.
	{
		MaterialAsset* m = cm.getMaterialMutable(id);
		REQUIRE(m != nullptr);
		std::string& s = m->customShaderFragGlsl;
		const char* before = s.data();
		const size_t at = s.find_last_not_of(" \n\t}");
		REQUIRE(at != std::string::npos);
		s[at] = s[at] == 'x' ? 'y' : 'x';
		REQUIRE(s.data() == before); // really in place
	}
	uint64_t h1 = 0;
	REQUIRE(key(h1));
	CHECK(h1 != h0);
	CHECK(h1 == referenceKey(cm.getMaterial(id)->customShaderFragGlsl,
	                         cm.getMaterial(id)->customShaderVertGlsl));

	// The vertex body alone changes (WPO graph edit) → the forward AND the G-buffer key move.
	{
		MaterialAsset* m = cm.getMaterialMutable(id);
		m->customShaderVertGlsl += "\n// edited";
	}
	uint64_t h2 = 0, g2 = 0;
	REQUIRE(key(h2));
	REQUIRE(gkey(g2));
	CHECK(h2 != h1);
	CHECK(g2 != g0);
	CHECK(g2 == referenceKey(cm.getMaterial(id)->customShaderGBufGlsl,
	                         cm.getMaterial(id)->customShaderVertGlsl));

	// WPO removed → the fold goes away, the key is the fragment's own hash again.
	cm.getMaterialMutable(id)->customShaderVertGlsl.clear();
	uint64_t h3 = 0;
	REQUIRE(key(h3));
	CHECK(h3 == std::hash<std::string>{}(cm.getMaterial(id)->customShaderFragGlsl));

	// The custom shader cleared in place (the editor's "back to built-in PBR"):
	// the material must stop resolving at once, not keep answering from a memo.
	cm.getMaterialMutable(id)->customShaderFragGlsl.clear();
	CHECK_FALSE(key(h3));
	CHECK_FALSE(gkey(h3));

	// And a whole new material object in the same slot (hot reload: replaceMaterial
	// move-assigns the reloaded payload over the old one, same address).
	const MaterialAsset fresh = landscapeMaterial();
	const MaterialAsset* slot = cm.getMaterial(id);
	REQUIRE(cm.replaceMaterial(id, fresh));
	REQUIRE(cm.getMaterial(id) == slot);
	uint64_t h4 = 0;
	REQUIRE(key(h4));
	CHECK(h4 == referenceKey(fresh.customShaderFragGlsl, fresh.customShaderVertGlsl));
}

// The measurement the audit asked for: wall time of 1000 resolves, i.e. one frame
// with 1000 graph-material draws, on the auto-landscape material. Reported, never
// asserted — a shared machine makes any threshold flaky.
TEST_CASE("resolveShaders cost for 1000 draws (measurement, REN-1001)")
{
	ContentManager cm;
	const HE::UUID id = cm.registerMaterial(landscapeMaterial());
	HE::MaterialShaderLibrary lib;
	constexpr int kDraws = 1000, kFrames = 31;
	const size_t bytes = cm.getMaterial(id)->customShaderFragGlsl.size()
	                   + cm.getMaterial(id)->customShaderVertGlsl.size();

	auto frameUs = [&](const std::function<uint64_t()>& draw) {
		std::vector<double> us;
		uint64_t sink = 0;
		for (int f = 0; f < kFrames; ++f)
		{
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < kDraws; ++i) sink += draw();
			const auto t1 = std::chrono::steady_clock::now();
			us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
		}
		std::sort(us.begin(), us.end());
		CHECK(sink != 0);
		return std::pair{ us.front(), us[us.size() / 2] };
	};

	// What every draw paid before REN-1001: copy both strings, hash both.
	const auto legacy = frameUs([&] {
		const MaterialAsset* m = cm.getMaterial(id);
		std::string frag = m->customShaderFragGlsl, vert = m->customShaderVertGlsl;
		return referenceKey(frag, vert);
	});
	const auto copying = frameUs([&] {
		uint64_t h = 0; std::string f, v;
		lib.resolveShaders(cm, id, h, f, v);
		return h;
	});

	const auto pointer = frameUs([&] {
		uint64_t h = 0; const std::string* f = nullptr; const std::string* v = nullptr;
		lib.resolveShaders(cm, id, h, f, v);
		return h;
	});

	char line[320];
	std::snprintf(line, sizeof line,
	              "REN-1001: 1000 resolves of a %zu-byte material, min/median us per frame: "
	              "legacy copy+hash %.1f/%.1f, resolveShaders(string&) %.1f/%.1f, "
	              "resolveShaders(const string*&) %.1f/%.1f",
	              bytes, legacy.first, legacy.second, copying.first, copying.second,
	              pointer.first, pointer.second);
	MESSAGE(line);
	std::printf("%s\n", line);
}
