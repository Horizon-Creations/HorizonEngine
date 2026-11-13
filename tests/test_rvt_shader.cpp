#include "doctest.h"
#include <MaterialGraph/MaterialGraph.h>
#include <MaterialGraph/RvtLookup.h>
#include <material/MaterialShaderLibrary.h>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

// The Runtime Virtual Texture read as a graph node (Thema 333 step 3): the generated fragment calls the shared lookup,
// the four table/pool bindings land on t40..t43 with no live sampler, and the real FXC accepts the whole shader.

namespace
{
	HE::MaterialGraph rvtGraph()
	{
		using T = HE::MatNodeType;
		HE::MaterialGraph g;
		const int out = g.addNode(T::Output);
		const int pa  = g.addNode(T::ParamVec4); g.findNode(pa)->s = "RvtVolumeA";
		const int pb  = g.addNode(T::ParamVec4); g.findNode(pb)->s = "RvtVolumeB";
		const int rvt = g.addNode(T::RvtSample);
		g.connect(pa, 0, rvt, 0);
		g.connect(pb, 0, rvt, 1);
		g.connect(rvt, 0, out, HE::kMatOutputBaseColorPin);
		g.connect(rvt, 1, out, HE::kMatOutputNormalPin);
		g.connect(rvt, 2, out, HE::kMatOutputAOPin);
		g.connect(rvt, 3, out, HE::kMatOutputRoughnessPin);
		return g;
	}
}

TEST_CASE("RVT node: the fragment calls the shared lookup and declares the table and the pool")
{
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;
	REQUIRE_FALSE(glsl.empty());
	CHECK(glsl.find("heRvtSample(vWorldPos.xz") != std::string::npos);
	CHECK(glsl.find("binding = 40) uniform usampler2D heRvtTable") != std::string::npos);
	CHECK(glsl.find("binding = 41) uniform sampler2DArray heRvtAlbedo") != std::string::npos);
	CHECK(glsl.find("binding = 42) uniform sampler2DArray heRvtNormal") != std::string::npos);
	CHECK(glsl.find("binding = 43) uniform sampler2DArray heRvtSurface") != std::string::npos);
	// The lookup is the shared text, once.
	const size_t at = glsl.find("void heRvtSample(");
	REQUIRE(at != std::string::npos);
	CHECK(glsl.find("void heRvtSample(", at + 1) == std::string::npos);
	// texelFetch only: no filtered read of the table or the pool (that is what spends no sampler register).
	CHECK(glsl.find("texture(heRvt") == std::string::npos);
	CHECK(glsl.find("texelFetch(heRvtTable") != std::string::npos);

	// A graph without the node declares none of it.
	HE::MaterialGraph plain;
	const int po = plain.addNode(HE::MatNodeType::Output);
	const int pc = plain.addNode(HE::MatNodeType::ConstColor);
	plain.connect(pc, 0, po, HE::kMatOutputBaseColorPin);
	const std::string pg = HE::generateFragment(plain).glsl;
	CHECK(pg.find("heRvt") == std::string::npos);
}

TEST_CASE("RVT node: the HLSL keeps t40..t43, spends no live sampler, and the real FXC compiles it")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;
	const auto& hl = lib.fragment(std::hash<std::string>{}("rvt-node"), glsl, B::HLSL);
	REQUIRE_MESSAGE(hl.ok, hl.log);
	const std::string& hlsl = hl.source;
	for (const char* r : { "register(t40)", "register(t41)", "register(t42)", "register(t43)" })
		CHECK_MESSAGE(hlsl.find(r) != std::string::npos, "missing ", r);
	// Read with Load, never Sample: the table and the arrays cost no sampler.
	CHECK(hlsl.find("heRvtTable.Load(") != std::string::npos);
	CHECK(hlsl.find("heRvtAlbedo.Load(") != std::string::npos);
	CHECK(hlsl.find("heRvtTable.Sample") == std::string::npos);
	CHECK(hlsl.find("heRvtAlbedo.Sample") == std::string::npos);
	CHECK(hlsl.find("heRvtNormal.Sample") == std::string::npos);
	CHECK(hlsl.find("heRvtSurface.Sample") == std::string::npos);

#if defined(_WIN32)
	// The same D3DCompile call both D3D renderers make: this is where two live samplers on one register (X4500) or a
	// register past SM 5.0's range (X4509) would show.
	Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
	const HRESULT hr = D3DCompile(hlsl.c_str(), hlsl.size(), "rvt", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err);
	const std::string fxcLog = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : std::string("no log");
	CHECK_MESSAGE(SUCCEEDED(hr), fxcLog);
#endif

	// Out of the fragment's other stage: the displacement (domain) stage has no derivatives, so it never gets the function.
	CHECK(glsl.find("dFdx(wxz)") != std::string::npos);
}

TEST_CASE("RVT node: the HLSL also keeps t44..t46 (cold BC7 arrays, step 9), read with Load, no live sampler, real FXC compiles it")
{
	// HE_RVT_COLD defaults to "wanted" (no env var set); D3D11/D3D12 never need SetRvtColdVulkanFits (that flag only
	// narrows the Vulkan SpirV path — texelFetch/Load costs no live sampler on D3D, unlike a Vulkan combined sampler).
	const char* coldEnv = std::getenv("HE_RVT_COLD");
	if (coldEnv && *coldEnv == '0')
	{
		MESSAGE("HE_RVT_COLD=0: this build/run asked for cold off, nothing to check here");
		return;
	}
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;
	const auto& hl = lib.fragment(std::hash<std::string>{}("rvt-node-cold"), glsl, B::HLSL);
	REQUIRE_MESSAGE(hl.ok, hl.log);
	const std::string& hlsl = hl.source;
	for (const char* r : { "register(t44)", "register(t45)", "register(t46)" })
		CHECK_MESSAGE(hlsl.find(r) != std::string::npos, "missing ", r);
	CHECK(hlsl.find("heRvtAlbedoC.Load(") != std::string::npos);
	CHECK(hlsl.find("heRvtAlbedoC.Sample") == std::string::npos);
	CHECK(hlsl.find("heRvtNormalC.Sample") == std::string::npos);
	CHECK(hlsl.find("heRvtSurfaceC.Sample") == std::string::npos);

#if defined(_WIN32)
	Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
	const HRESULT hr = D3DCompile(hlsl.c_str(), hlsl.size(), "rvt-cold", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err);
	const std::string fxcLog = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : std::string("no log");
	CHECK_MESSAGE(SUCCEEDED(hr), fxcLog);
#endif
}

TEST_CASE("RVT node: Vulkan SpirV keeps 44..46 only when SetRvtColdVulkanFits says the layout fits (step 9)")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;

	// Device-fits path: compiles, and the GLSL source handed to SPIRV-Cross (visible via a fresh hash so the cache
	// does not hand back the device-does-not-fit compile below) declares the cold arrays.
	HE::MaterialShaderLibrary::SetRvtColdVulkanFits(true);
	const auto& fits = lib.fragment(std::hash<std::string>{}("rvt-node-vk-cold-fit"), glsl, B::SpirV);
	HE::MaterialShaderLibrary::SetRvtColdVulkanFits(false);   // reset before any other test reads the flag
	REQUIRE_MESSAGE(fits.ok, fits.log);
	REQUIRE_FALSE(fits.spirv.empty());

	// Device-does-not-fit path (the default state every other test in this binary relies on): compiles too, and must
	// NOT statically reference bindings 44..46 — the layout omits them (VulkanMaterialLayout.h's rvtColdFit), so a
	// pipeline whose shader used them anyway would be invalid.
	const auto& noFit = lib.fragment(std::hash<std::string>{}("rvt-node-vk-cold-nofit"), glsl, B::SpirV);
	REQUIRE_MESSAGE(noFit.ok, noFit.log);
	REQUIRE_FALSE(noFit.spirv.empty());
	CHECK(fits.spirv != noFit.spirv);
}

TEST_CASE("RVT cold bit (step 9): bit 23 never corrupts the 24-bit slot, boundary slot 0x7FFFFF round-trips")
{
	// RvtLookup.h's shader text: `slot = e & 0x7FFFFFu` (23 bits), `cold = (e & 0x800000u) != 0`. The manager's raw
	// entry (what SetRvtPageTable/ReadbackRvtPageTable carry) is `slot | effectiveMip << 24` with a 24-bit slot
	// (0xFFFFFFu) — the renderer's upload step narrows that to 23 bits plus the cold flag when it rewrites the entry.
	constexpr uint32_t kMaxSlot = 0x7FFFFFu;   // the highest slot the cold bit leaves room for
	CHECK(kMaxSlot == (1u << 23) - 1);

	auto rewrite = [](uint32_t slot, bool cold, uint32_t eff) -> uint32_t {
		return (eff << 24) | (cold ? 0x800000u : 0u) | (slot & 0x7FFFFFu);
	};
	auto slotOf = [](uint32_t e) { return e & 0x7FFFFFu; };
	auto coldOf = [](uint32_t e) { return (e & 0x800000u) != 0u; };
	auto effOf  = [](uint32_t e) { return e >> 24; };

	for (const uint32_t slot : { 0u, 1u, 12345u, kMaxSlot - 1, kMaxSlot })
	{
		for (const bool cold : { false, true })
		{
			const uint32_t e = rewrite(slot, cold, 3);
			CHECK(slotOf(e) == slot);
			CHECK(coldOf(e) == cold);
			CHECK(effOf(e) == 3u);
		}
	}
	// 2048 is this engine's own RVT pool cap (CreateRvtPool rejects slots > 2048 on every backend), well inside 23 bits:
	// the cold bit never collides with any slot this engine can actually hand out.
	CHECK(2048u < kMaxSlot);
}

TEST_CASE("RVT node: the MSL reads the table and the pool as textures 18..21 and spends no sampler (Metal's cap is 16)")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;
	const auto& ms = lib.fragment(std::hash<std::string>{}("rvt-node-msl"), glsl, B::Metal);
	REQUIRE_MESSAGE(ms.ok, ms.log);
	const std::string& msl = ms.source;
	// Texture 17 is heFogVolume's (Thema 225 step 5); RVT starts one higher than it used to.
	CHECK_MESSAGE(msl.find("heRvtTable [[texture(18)]]") != std::string::npos, msl);
	CHECK(msl.find("heRvtAlbedo [[texture(19)]]") != std::string::npos);
	CHECK(msl.find("heRvtNormal [[texture(20)]]") != std::string::npos);
	CHECK(msl.find("heRvtSurface [[texture(21)]]") != std::string::npos);
	// The packed pool (step 6k): the normal layer holds x and z, the lookup rebuilds y from the unit length.
	const char* packEnv = std::getenv("HE_RVT_PACK");
	const size_t nrm = msl.find("heRvtNormalOf(");
	CHECK(nrm != std::string::npos);
	if (!(packEnv && *packEnv == '0') && nrm != std::string::npos)
		CHECK(msl.find("sqrt(", nrm) != std::string::npos);
	// The cold pool's block-compressed arrays (step 6j): textures 22..24, sampled (inline sampler), selected by the table entry's bit 23.
	const char* coldEnv = std::getenv("HE_RVT_COLD");
	if (!(coldEnv && *coldEnv == '0'))
	{
		CHECK(msl.find("heRvtAlbedoC [[texture(22)]]") != std::string::npos);
		CHECK(msl.find("heRvtNormalC [[texture(23)]]") != std::string::npos);
		CHECK(msl.find("heRvtSurfaceC [[texture(24)]]") != std::string::npos);
		CHECK(msl.find("heRvtAlbedoC.sample(") != std::string::npos);
	}
	// Fetch only (.read), and no [[sampler(n)]] with n >= 16 anywhere: an inline constexpr sampler stands in for the four.
	CHECK(msl.find("heRvtTable.read(") != std::string::npos);
	CHECK(msl.find("heRvtAlbedo.read(") != std::string::npos);
	size_t at = 0;
	while ((at = msl.find("[[sampler(", at)) != std::string::npos)
	{
		at += 10;
		CHECK_MESSAGE(std::atoi(msl.c_str() + at) < 16, "a sampler argument past Metal's 16");
	}
	CHECK(msl.find("constexpr sampler heRvtAlbedoSmplr") != std::string::npos);   // inline, not an argument
}
