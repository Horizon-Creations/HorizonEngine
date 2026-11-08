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

TEST_CASE("RVT node: the MSL reads the table and the pool as textures 17..20 and spends no sampler (Metal's cap is 16)")
{
	using B = HE::MaterialShaderLibrary::Backend;
	HE::MaterialShaderLibrary lib;
	const std::string glsl = HE::generateFragment(rvtGraph()).glsl;
	const auto& ms = lib.fragment(std::hash<std::string>{}("rvt-node-msl"), glsl, B::Metal);
	REQUIRE_MESSAGE(ms.ok, ms.log);
	const std::string& msl = ms.source;
	CHECK_MESSAGE(msl.find("heRvtTable [[texture(17)]]") != std::string::npos, msl);
	CHECK(msl.find("heRvtAlbedo [[texture(18)]]") != std::string::npos);
	CHECK(msl.find("heRvtNormal [[texture(19)]]") != std::string::npos);
	CHECK(msl.find("heRvtSurface [[texture(20)]]") != std::string::npos);
	// The packed pool (step 6k): the normal layer holds x and z, the lookup rebuilds y from the unit length.
	const char* packEnv = std::getenv("HE_RVT_PACK");
	const size_t nrm = msl.find("heRvtNormalOf(");
	CHECK(nrm != std::string::npos);
	if (!(packEnv && *packEnv == '0') && nrm != std::string::npos)
		CHECK(msl.find("sqrt(", nrm) != std::string::npos);
	// The cold pool's block-compressed arrays (step 6j): textures 21..23, sampled (inline sampler), selected by the table entry's bit 23.
	const char* coldEnv = std::getenv("HE_RVT_COLD");
	if (!(coldEnv && *coldEnv == '0'))
	{
		CHECK(msl.find("heRvtAlbedoC [[texture(21)]]") != std::string::npos);
		CHECK(msl.find("heRvtNormalC [[texture(22)]]") != std::string::npos);
		CHECK(msl.find("heRvtSurfaceC [[texture(23)]]") != std::string::npos);
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
