#include "doctest.h"
#include <cstddef>
#include <regex>
#include <string>
#include <vector>

#include <HorizonRendering/SkyFrameParams.h>
#include <HorizonRendering/SkyShaderSource.h>

// ═══ Thema 78, Schritt 3: one sky shader for OpenGL, Vulkan, D3D11 and D3D12 ═══
// kSkyFS is the GL sky. Vulkan and both D3D backends compile the same text
// behind kSkyVulkanPrelude, which swaps the GL uniform declarations for a
// uniform block laid out as HE::SkyFrameParams and #defines every GL uniform
// name onto a field of it. Two things can drift without any backend noticing
// at build time — the renderers only find out when they compile the shader at
// startup, and then fall back to their old sky with one log line:
//   * a uniform added to kSkyFS that the prelude does not map (the body then
//     names an undeclared identifier), and
//   * the prelude's block and the C++ struct disagreeing on field order (every
//     field past the first mismatch reads its neighbour's value).
// Both are checked here without a compiler. With one (HE_TESTS_HAVE_SHADERC)
// the assembled source is run through the real cross-compile the renderers
// call, and on Windows through FXC, a D3D11 WARP draw and a D3D12 PSO.

namespace
{
// The GL declaration block of kSkyFS: everything above the //#SKYFUNC# marker.
std::string glDeclarationBlock()
{
	const std::string fs = HE::glsl::kSkyFS;
	const size_t at = fs.find("//#SKYFUNC#");
	REQUIRE(at != std::string::npos);
	return fs.substr(0, at);
}

std::vector<std::string> glUniformNames()
{
	const std::string decl = glDeclarationBlock();
	static const std::regex re(R"(uniform\s+\w+\s+(\w+)\s*;)");
	std::vector<std::string> names;
	for (auto it = std::sregex_iterator(decl.begin(), decl.end(), re); it != std::sregex_iterator(); ++it)
		names.push_back((*it)[1].str());
	return names;
}

// Declared by the prelude either as a real resource or as a #define.
bool preludeMaps(const std::string& name)
{
	const std::string prelude = HE::glsl::kSkyVulkanPrelude;
	const std::regex define("#define\\s+" + name + "\\s");
	const std::regex resource("uniform\\s+\\w+\\s+" + name + "\\s*;");
	return std::regex_search(prelude, define) || std::regex_search(prelude, resource);
}
} // namespace

TEST_CASE("Sky prelude maps every uniform the GL sky shader declares")
{
	const std::vector<std::string> names = glUniformNames();
	// Guards the parser itself: kSkyFS declares 54 uniforms today. A regex that
	// matched nothing would make the loop below vacuously green.
	REQUIRE(names.size() >= 50);
	for (const std::string& n : names)
		CHECK_MESSAGE(preludeMaps(n), "kSkyFS uniform '", n, "' has no #define in kSkyVulkanPrelude "
		                              "(Vulkan/D3D11/D3D12 would fail to compile the sky)");
	// Negative control on preludeMaps: a name the prelude certainly lacks.
	CHECK_FALSE(preludeMaps("uNotASkyUniform"));
}

TEST_CASE("Sky prelude uniform block has HE::SkyFrameParams' field order")
{
	const std::string prelude = HE::glsl::kSkyVulkanPrelude;
	const size_t open  = prelude.find("uniform SkyEnv");
	const size_t close = prelude.find("} heSky;");
	REQUIRE(open != std::string::npos);
	REQUIRE(close != std::string::npos);
	const std::string block = prelude.substr(open, close - open);
	static const std::regex member(R"((mat4|vec4)\s+(\w+)\s*;)");
	std::vector<std::string> fields;
	for (auto it = std::sregex_iterator(block.begin(), block.end(), member); it != std::sregex_iterator(); ++it)
		fields.push_back((*it)[2].str());

	// std140 with a mat4 followed by vec4s is tightly packed: field i sits at
	// 64 + 16 * (i - 1). The C++ side is asked for the same offsets by name.
	using P = HE::SkyFrameParams;
	const std::vector<std::pair<std::string, size_t>> cpp = {
		{ "invViewProj",    offsetof(P, invViewProj)    }, { "sunDir",       offsetof(P, sunDir)       },
		{ "sunColor",       offsetof(P, sunColor)       }, { "params",       offsetof(P, params)       },
		{ "nebulaColor",    offsetof(P, nebulaColor)    }, { "auroraColor",  offsetof(P, auroraColor)  },
		{ "wind",           offsetof(P, wind)           }, { "cameraPos",    offsetof(P, cameraPos)    },
		{ "cloud",          offsetof(P, cloud)          }, { "cloudTint",    offsetof(P, cloudTint)    },
		{ "cirrus",         offsetof(P, cirrus)         }, { "nebulaColor2", offsetof(P, nebulaColor2) },
		{ "nebulaColor3",   offsetof(P, nebulaColor3)   }, { "auroraColorTop", offsetof(P, auroraColorTop) },
		{ "starColor",      offsetof(P, starColor)      }, { "star",         offsetof(P, star)         },
		{ "star2",          offsetof(P, star2)          }, { "neb2",         offsetof(P, neb2)         },
	};
	REQUIRE(fields.size() == cpp.size());
	for (size_t i = 0; i < cpp.size(); ++i)
	{
		const size_t glslOffset = (i == 0) ? 0 : 64 + 16 * (i - 1);
		CHECK_MESSAGE(fields[i] == cpp[i].first, "prelude field ", i, " is '", fields[i], "', SkyFrameParams has '", cpp[i].first, "'");
		CHECK_MESSAGE(glslOffset == cpp[i].second, cpp[i].first, ": std140 offset ", glslOffset, " vs C++ ", cpp[i].second);
	}
	CHECK(sizeof(P) == 64 + 16 * 17);
}

TEST_CASE("BuildSkyFragmentGLSL450 swaps the GL header for the prelude and keeps the body")
{
	const std::string src = HE::glsl::BuildSkyFragmentGLSL450();
	REQUIRE_FALSE(src.empty());
	CHECK(src.rfind("#version 450", 0) == 0);                    // the prelude opens the file
	CHECK(src.find("#version 410") == std::string::npos);        // the GL header is gone
	CHECK(src.find("uniform float     uTime;") == std::string::npos);
	CHECK(src.find(HE::glsl::kSkyFuncGLSL) != std::string::npos); // analytic sky spliced in
	// ...and the body is kSkyFS verbatim from the marker on, main() included.
	const std::string fs = HE::glsl::kSkyFS;
	const std::string body = fs.substr(fs.find("//#SKYFUNC#") + std::string("//#SKYFUNC#").size());
	CHECK(src.size() > body.size());
	CHECK(src.compare(src.size() - body.size(), body.size(), body) == 0);
}

TEST_CASE("Overcast sky: every cloud threshold goes through cloudLo, and the celestial layer is veiled")
{
	// "100 % clouds" has to be a closed deck with nothing of the night sky behind it. That
	// rests on three things in the shader text, any of which can quietly drift apart: ONE
	// coverage->threshold function (so the march, its light march, the shadow map and the
	// god-ray gate agree about the holes), the celestial layer scaled by the overcast
	// amount, and the deck carried on to the horizon.
	const std::string fs = HE::glsl::kSkyFS;
	CHECK(fs.find("float cloudLo(float coverage)") != std::string::npos);
	CHECK(fs.find("float overcastAmount(float coverage)") != std::string::npos);
	CHECK(fs.find("vec3 overcastBand(") != std::string::npos);
	// The old inline mapping must be gone everywhere but cloudLo's own body.
	size_t inlineMaps = 0;
	for (size_t at = fs.find("mix(0.70, 0.22"); at != std::string::npos; at = fs.find("mix(0.70, 0.22", at + 1))
		++inlineMaps;
	CHECK(inlineMaps == 1);
	CHECK(fs.find("float celestialVeil = 1.0 - overcastAmount(uCloudCoverage);") != std::string::npos);
	CHECK(fs.find("celestial * celestialVeil") != std::string::npos);
	CHECK(fs.find("moonCorona(dir, uSunDir, true, uMoonPhase) * celestialVeil") != std::string::npos);
	// Full coverage fills whatever thin spot the noise left (three marches: dome, 3D, 3D real).
	size_t fills = 0;
	const std::string fill = "smoothstep(0.95, 1.0, coverage)";
	for (size_t at = fs.find(fill); at != std::string::npos; at = fs.find(fill, at + 1)) ++fills;
	CHECK(fills == 3);
}

#if defined(HE_TESTS_HAVE_SHADERC)
#include "ShaderCompiler.h"

namespace
{
// Exactly the call D3D11Renderer/D3D12Renderer::createSkyPipeline make.
he::shaderc::Result compileSkyHlsl(const std::string& glsl)
{
	using he::shaderc::Stage;
	using namespace HE::glsl;
	return he::shaderc::compileHlslPinned(glsl, Stage::Fragment, {
		{ Stage::Fragment, 0, kSkySlotEnv.binding,   kSkySlotEnv.hlslReg   },
		{ Stage::Fragment, 0, kSkySlotMoon.binding,  kSkySlotMoon.hlslReg  },
		{ Stage::Fragment, 0, kSkySlotNoise.binding, kSkySlotNoise.hlslReg },
	});
}
} // namespace

TEST_CASE("The GL sky cross-compiles to SPIR-V (Vulkan) and pinned HLSL SM 5.0 (D3D11/D3D12)")
{
	const std::string glsl = HE::glsl::BuildSkyFragmentGLSL450();

	// Negative control: the same source with one undeclared name must fail, or
	// an always-ok compiler (the stub, say) could turn this case green.
	{
		const he::shaderc::Result bad = he::shaderc::compile(
			glsl + "\nvoid heSkyNeg() { heNotDeclared = 1.0; }\n",
			he::shaderc::Stage::Fragment, he::shaderc::Target::SpirvBinary);
		REQUIRE_FALSE(bad.ok);
	}

	const he::shaderc::Result spv = he::shaderc::compile(glsl, he::shaderc::Stage::Fragment,
	                                                     he::shaderc::Target::SpirvBinary);
	REQUIRE_MESSAGE(spv.ok, spv.log);
	CHECK(spv.spirv.size() > 1000);

	const he::shaderc::Result hlsl = compileSkyHlsl(glsl);
	REQUIRE_MESSAGE(hlsl.ok, hlsl.log);
	const std::string& h = hlsl.source;
	// The registers D3D11/D3D12 bind: constants b0, moon t0/s0, noise t1/s1.
	CHECK(h.find("cbuffer SkyEnv : register(b0)") != std::string::npos);
	CHECK(h.find("Texture2D<float4> uMoonTex : register(t0)") != std::string::npos);
	CHECK(h.find("uMoonTex_sampler : register(s0)") != std::string::npos);
	CHECK(h.find("Texture3D<float4> uNoise : register(t1)") != std::string::npos);
	CHECK(h.find("uNoise_sampler : register(s1)") != std::string::npos);
	// Same byte layout as SkyFrameParams: the last vec4 in constant register 20.
	CHECK(h.find("heSky_neb2 : packoffset(c20)") != std::string::npos);
	// The noise volume is fetched with an explicit LOD (see kSkyVulkanPrelude).
	CHECK(h.find("SampleLevel(") != std::string::npos);
}

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <HorizonRendering/SkyNoise3D.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
using Microsoft::WRL::ComPtr;

// The renderers' FXC flags for the cross-compiled sky (Release build).
constexpr UINT kSkyFxcFlags = D3DCOMPILE_PREFER_FLOW_CONTROL;

bool fxcCompile(const std::string& src, const char* entry, const char* profile,
                ComPtr<ID3DBlob>& out, std::string& err)
{
	ComPtr<ID3DBlob> cerr;
	const HRESULT hr = D3DCompile(src.c_str(), src.size(), entry, nullptr, nullptr,
	                              entry, profile, kSkyFxcFlags, 0, &out, &cerr);
	if (cerr) err.assign(static_cast<const char*>(cerr->GetBufferPointer()), cerr->GetBufferSize());
	return SUCCEEDED(hr) && out && out->GetBufferSize() > 0;
}

struct SkyBytecode { ComPtr<ID3DBlob> vs, ps; };

// Cross-compile + FXC, once for all cases below (FXC on this shader is slow).
const SkyBytecode& skyBytecode()
{
	static SkyBytecode bc = []
	{
		SkyBytecode b;
		const he::shaderc::Result hlsl = compileSkyHlsl(HE::glsl::BuildSkyFragmentGLSL450());
		REQUIRE_MESSAGE(hlsl.ok, hlsl.log);
		std::string err;
		const auto t0 = std::chrono::steady_clock::now();
		const bool psOk = fxcCompile(hlsl.source, "main", "ps_5_0", b.ps, err);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		MESSAGE("FXC ps_5_0 on the cross-compiled sky: ", ms, " ms, ",
		        psOk ? std::to_string(b.ps->GetBufferSize()) + " bytes" : std::string("FAILED"));
		REQUIRE_MESSAGE(psOk, err);
		err.clear();
		REQUIRE_MESSAGE(fxcCompile(HE::glsl::kSkyVSCrossHLSL, "VSSkyCross", "vs_5_0", b.vs, err), err);
		return b;
	}();
	return bc;
}

HE::SkyFrameParams skyLookingUp(const glm::vec3& sunDir)
{
	// Clear sky with the night layers off: the zenith colour is the atmosphere
	// alone. The target is one pixel looking straight up, so with the default
	// star field it lands on whatever star sits at the zenith — at timeOfDay 0.5
	// that is a bright one, and GL's own kSkyFS draws the night zenith at ~0.9
	// there too (checked against a GL 4.1 render of the same inputs).
	IRenderer::EnvironmentSettings env;
	env.cloudCoverage     = 0.0f;
	env.starBrightness    = 0.0f; // stars + Milky Way (starField)
	env.milkyWayIntensity = 0.0f;
	env.nebulaIntensity   = 0.0f;
	env.auroraIntensity   = 0.0f;
	env.shootingStars     = 0.0f;
	const glm::vec3 eye(0.0f, 1.0f, 0.0f);
	const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
	HE::SkyFrameInputs in;
	in.invViewProj = glm::inverse(proj * view);
	in.sunDir      = glm::normalize(sunDir);
	in.cameraPos   = eye;
	in.time        = 10.0f;
	return HE::BuildSkyFrameParams(env, in);
}

// Deep night looking into the galactic band, every night layer off except the
// nebula — so "image with nebula minus image without" is the nebula alone:
// nothing after nebula() in kSkyFS reshapes it (cirrus, contrails, rain and
// clouds are all at 0, the rest of main() adds terms that do not depend on it).
//   * timeOfDay 0: celestialDir turns by 0, so cdir == dir and the band sits
//     where the GL source puts it (galN), not rotated away.
//   * look = the zenith projected onto the galactic plane (y ~0.85), far
//     above nebula()'s 0.16 horizon fade; 60 degree FOV around it.
//   * sun y ~-0.81: below the -0.22 the deep-night gate needs to be fully on.
HE::SkyFrameParams skyNebulaNight(float intensity, int quality, const glm::vec3& colour1,
                                  const glm::vec3& colour2 = IRenderer::EnvironmentSettings{}.nebulaColor2,
                                  const glm::vec3& colour3 = IRenderer::EnvironmentSettings{}.nebulaColor3)
{
	IRenderer::EnvironmentSettings env;
	env.timeOfDay         = 0.0f;
	env.cloudCoverage     = 0.0f;
	env.cirrusAmount      = 0.0f;
	env.contrailAmount    = 0.0f;
	env.starBrightness    = 0.0f;
	env.milkyWayIntensity = 0.0f;
	env.auroraIntensity   = 0.0f;
	env.shootingStars     = 0.0f;
	env.nebulaIntensity   = intensity;
	env.nebulaCoverage    = 0.8f;
	env.nebulaQuality     = quality;
	env.nebulaColor       = colour1;
	env.nebulaColor2      = colour2;
	env.nebulaColor3      = colour3;
	const glm::vec3 galN = glm::normalize(glm::vec3(0.46f, 0.52f, -0.72f)); // as in nebula()
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::vec3 look = glm::normalize(up - galN * glm::dot(up, galN));
	const glm::vec3 eye(0.0f, 1.0f, 0.0f);
	const glm::mat4 view = glm::lookAt(eye, eye + look, up);
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
	HE::SkyFrameInputs in;
	in.invViewProj = glm::inverse(proj * view);
	in.sunDir      = glm::normalize(glm::vec3(0.3f, -0.5f, 0.2f));
	in.cameraPos   = eye;
	in.time        = 10.0f;
	return HE::BuildSkyFrameParams(env, in);
}

// The renderers bake 256^3 in Release and 64^3 in Debug; the Debug size keeps
// the bake and the upload small here (64 * 4 bytes is also D3D12's 256-byte
// row pitch, so the D3D12 copy needs no padding).
constexpr int kTestNoiseN = 64;

using Image = std::vector<glm::vec4>;

// The clear value is -1: a channel still below 0 was never shaded.
std::string unshadedReason(const Image& img)
{
	for (size_t i = 0; i < img.size(); ++i)
		for (int c = 0; c < 3; ++c)
			if (!std::isfinite(img[i][c]) || img[i][c] < 0.0f)
				return "pixel " + std::to_string(i) + " channel " + std::to_string(c) + " = " + std::to_string(img[i][c]);
	return {};
}

struct ImageDelta
{
	glm::dvec3 sum{ 0.0 };  // per-channel sum of (a - b)
	double meanAbs = 0.0;   // mean |a - b| over pixels and RGB
	float  maxAbs  = 0.0f;
	double changed = 0.0;   // fraction of pixels with an RGB delta above 1e-3
};

ImageDelta imageDelta(const Image& a, const Image& b)
{
	REQUIRE(a.size() == b.size());
	REQUIRE_FALSE(a.empty());
	ImageDelta d;
	size_t changed = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		bool moved = false;
		for (int c = 0; c < 3; ++c)
		{
			const float v = a[i][c] - b[i][c];
			d.sum[c]  += v;
			d.meanAbs += std::fabs(v);
			d.maxAbs   = std::max(d.maxAbs, std::fabs(v));
			moved      = moved || std::fabs(v) > 1e-3f;
		}
		changed += moved ? 1 : 0;
	}
	d.meanAbs /= 3.0 * static_cast<double>(a.size());
	d.changed  = static_cast<double>(changed) / static_cast<double>(a.size());
	return d;
}

std::string drainD3D11(ID3D11InfoQueue* iq)
{
	if (!iq) return "(no D3D11 debug layer)";
	std::string out;
	for (UINT64 i = 0, n = iq->GetNumStoredMessages(); i < n; ++i)
	{
		SIZE_T len = 0;
		iq->GetMessage(i, nullptr, &len);
		std::vector<char> buf(len);
		auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
		if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription)
			out += std::string(msg->pDescription, msg->DescriptionByteLength) + "\n";
	}
	iq->ClearStoredMessages();
	return out;
}

// D3D11Renderer's sky draw on WARP: the cross-compiled VS/PS, constants b0,
// moon t0 (1x1 white) + clamp s0, noise volume t1 + wrap s1, one fullscreen
// triangle into an RGBA32F target of n x n pixels, read back.
struct D3D11SkyRig
{
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11InfoQueue> iq;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	ComPtr<ID3D11ShaderResourceView> moonSrv, noiseSrv;
	ComPtr<ID3D11SamplerState> clampS, wrapS;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11RasterizerState> rs;

	void init(const SkyBytecode& bc)
	{
		const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
		                               &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		if (FAILED(hr))
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			                       &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		REQUIRE_MESSAGE(SUCCEEDED(hr), "D3D11CreateDevice(WARP) failed: ", hr);
		dev.As(&iq); // null without the SDK layers

		REQUIRE(SUCCEEDED(dev->CreateVertexShader(bc.vs->GetBufferPointer(), bc.vs->GetBufferSize(), nullptr, &vs)));
		REQUIRE(SUCCEEDED(dev->CreatePixelShader(bc.ps->GetBufferPointer(), bc.ps->GetBufferSize(), nullptr, &ps)));
		{
			const uint32_t white = 0xFFFFFFFFu;
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ &white, 4, 0 };
			ComPtr<ID3D11Texture2D> tex;
			REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex)));
			REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &moonSrv)));
		}
		{
			constexpr int kN = kTestNoiseN;
			const std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kN);
			D3D11_TEXTURE3D_DESC nd{};
			nd.Width = nd.Height = nd.Depth = kN; nd.MipLevels = 1;
			nd.Format = DXGI_FORMAT_R16G16_UNORM; nd.Usage = D3D11_USAGE_IMMUTABLE;
			nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ noise.data(), kN * 4u, kN * kN * 4u };
			ComPtr<ID3D11Texture3D> tex;
			REQUIRE(SUCCEEDED(dev->CreateTexture3D(&nd, &init, &tex)));
			REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, &noiseSrv)));
		}
		auto sampler = [&](D3D11_TEXTURE_ADDRESS_MODE mode) {
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = mode;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			ComPtr<ID3D11SamplerState> s;
			dev->CreateSamplerState(&sd, &s);
			return s;
		};
		clampS = sampler(D3D11_TEXTURE_ADDRESS_CLAMP);
		wrapS  = sampler(D3D11_TEXTURE_ADDRESS_WRAP);
		{
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = sizeof(HE::SkyFrameParams);
			bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb)));
		}
		{
			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
			REQUIRE(SUCCEEDED(dev->CreateRasterizerState(&rd, &rs)));
		}
	}

	std::string drain() { return drainD3D11(iq.Get()); }

	Image draw(const HE::SkyFrameParams& p, UINT n)
	{
		ComPtr<ID3D11Texture2D> target, staging;
		ComPtr<ID3D11RenderTargetView> rtv;
		{
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = n; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &target)));
			REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
			td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging)));
		}
		D3D11_MAPPED_SUBRESOURCE m{};
		REQUIRE(SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)));
		std::memcpy(m.pData, &p, sizeof(p));
		ctx->Unmap(cb.Get(), 0);
		const float clear[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
		ctx->ClearRenderTargetView(rtv.Get(), clear);
		ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
		ctx->OMSetRenderTargets(1, rtvs, nullptr);
		D3D11_VIEWPORT vp{}; vp.Width = static_cast<float>(n); vp.Height = static_cast<float>(n); vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(rs.Get());
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs.Get(), nullptr, 0);
		ctx->PSSetShader(ps.Get(), nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
		ID3D11ShaderResourceView* srvs[] = { moonSrv.Get(), noiseSrv.Get() };
		ctx->PSSetShaderResources(0, 2, srvs);
		ID3D11SamplerState* samps[] = { clampS.Get(), wrapS.Get() };
		ctx->PSSetSamplers(0, 2, samps);
		ctx->Draw(3, 0);
		ID3D11RenderTargetView* none[] = { nullptr };
		ctx->OMSetRenderTargets(1, none, nullptr);
		ctx->CopyResource(staging.Get(), target.Get());
		Image img(static_cast<size_t>(n) * n);
		REQUIRE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)));
		for (UINT y = 0; y < n; ++y)
			std::memcpy(&img[static_cast<size_t>(y) * n], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
			            n * sizeof(glm::vec4));
		ctx->Unmap(staging.Get(), 0);
		return img;
	}
};

// The night-sky images the D3D12 case compares itself against: drawn once.
constexpr UINT kNebulaImageN = 32;
const glm::vec3 kNebulaBlue(0.0f, 0.0f, 1.0f);

struct NebulaD3D11Images { Image off, q1; bool ok = false; };
NebulaD3D11Images& nebulaD3D11Images() { static NebulaD3D11Images imgs; return imgs; }
} // namespace

TEST_CASE("D3D11: the cross-compiled sky draws a blue zenith by day and a dark one by night (WARP)")
{
	const SkyBytecode& bc = skyBytecode();
	D3D11SkyRig rig;
	rig.init(bc);

	const glm::vec4 day   = rig.draw(skyLookingUp(glm::vec3(0.3f,  0.8f, 0.2f)), 1)[0];
	const std::string dayLog = rig.drain();
	const glm::vec4 night = rig.draw(skyLookingUp(glm::vec3(0.3f, -0.8f, 0.2f)), 1)[0];
	const std::string nightLog = rig.drain();
	MESSAGE("zenith by day (", day.r, ", ", day.g, ", ", day.b, "), by night (",
	        night.r, ", ", night.g, ", ", night.b, ")");

	// The clear value is -1: a pixel still at -1 was never shaded.
	for (int c = 0; c < 3; ++c)
	{
		REQUIRE_MESSAGE(std::isfinite(day[c]),   "day channel ", c, " not finite; ", dayLog);
		REQUIRE_MESSAGE(std::isfinite(night[c]), "night channel ", c, " not finite; ", nightLog);
		CHECK_MESSAGE(day[c]   >= 0.0f, "day pixel not shaded; ", dayLog);
		CHECK_MESSAGE(night[c] >= 0.0f, "night pixel not shaded; ", nightLog);
	}
	// Rayleigh scattering: the clear daytime zenith is blue, and it is the sun
	// that lights it — the same zenith with the sun below the horizon is darker.
	CHECK_MESSAGE(day.b > day.r, dayLog);
	CHECK(day.b > 0.05f);
	CHECK(day.r + day.g + day.b > 2.0f * (night.r + night.g + night.b));
	// ...and dark in absolute terms: GL draws ~0.09 here, so a night layer
	// leaking into the "atmosphere alone" setup shows up on its own.
	CHECK(night.r + night.g + night.b < 0.2f);
}

// Schritt 2 of "Nebula-Paritaet": the nebula v3 in kSkyFS reaches D3D11/D3D12
// only through the cross-compile above, and until now no test ever drew it
// (skyLookingUp switches it off). The checks are differential, so they need no
// GL reference numbers: the nebula only adds light, each quality tier takes
// its own code path, and colour 1 is the interior gas colour.
TEST_CASE("D3D11: the cross-compiled sky draws the nebula at quality 0/1/2 and colour 1 tints it (WARP)")
{
	const SkyBytecode& bc = skyBytecode();
	D3D11SkyRig rig;
	rig.init(bc);
	const UINT n = kNebulaImageN;

	const Image off = rig.draw(skyNebulaNight(0.0f, 1, kNebulaBlue), n);
	const std::string offLog = rig.drain();
	REQUIRE_MESSAGE(unshadedReason(off).empty(), "nebula off: ", unshadedReason(off), "; ", offLog);

	Image on[3];
	for (int q = 0; q < 3; ++q)
	{
		on[q] = rig.draw(skyNebulaNight(1.0f, q, kNebulaBlue), n);
		const std::string log = rig.drain();
		REQUIRE_MESSAGE(unshadedReason(on[q]).empty(), "quality ", q, ": ", unshadedReason(on[q]), "; ", log);
		const ImageDelta d = imageDelta(on[q], off);
		MESSAGE("quality ", q, ": nebula adds RGB (", d.sum.r, ", ", d.sum.g, ", ", d.sum.b, ") over ", n * n,
		        " px, mean |delta| ", d.meanAbs, ", max ", d.maxAbs, ", ", d.changed * 100.0, "% of pixels changed");
		// Visible, and as light: nebula() returns max(C, 0) and is added to col.
		CHECK_MESSAGE(d.sum.r + d.sum.g + d.sum.b > 1e-3 * n * n, "quality ", q, ": nebula adds no light; ", log);
		CHECK_MESSAGE(d.changed > 0.05, "quality ", q, ": nebula touches almost no pixel; ", log);
		CHECK_MESSAGE(d.sum.r >= -1e-3 * n * n, "quality ", q, ": nebula darkens the sky");
		CHECK_MESSAGE(d.sum.b >= -1e-3 * n * n, "quality ", q, ": nebula darkens the sky");
	}
	// uNebulaHiFi (nebulaColor2.w) reaches the shader: High adds the second
	// warp, the crinkle and the beads, Max the back web and the neck fray.
	const ImageDelta q01 = imageDelta(on[1], on[0]);
	const ImageDelta q12 = imageDelta(on[2], on[1]);
	MESSAGE("quality 1 vs 0: mean |delta| ", q01.meanAbs, "; quality 2 vs 1: mean |delta| ", q12.meanAbs);
	CHECK(q01.meanAbs > 1e-5);
	CHECK(q12.meanAbs > 1e-5);

	// Colour 1 is the interior colour: the same nebula with colour 1 pure blue
	// vs pure red. Colours 2/3 stay equal (and dark, so colour 1 carries most
	// of the light). Per pixel the nebula's colour is K + colour1 * a with
	// K, a >= 0 shared by both images, and the final rolloff keeps the hue, so
	// blue's share of the nebula's light must be larger in the blue image and
	// red's in the red image — whatever the noise puts in the frame.
	const glm::vec3 dim(0.05f);
	const Image blue = rig.draw(skyNebulaNight(1.0f, 1, glm::vec3(0.0f, 0.0f, 1.0f), dim, dim), n);
	const Image red  = rig.draw(skyNebulaNight(1.0f, 1, glm::vec3(1.0f, 0.0f, 0.0f), dim, dim), n);
	const std::string colourLog = rig.drain();
	REQUIRE(unshadedReason(blue).empty());
	REQUIRE(unshadedReason(red).empty());
	const ImageDelta nb = imageDelta(blue, off);
	const ImageDelta nr = imageDelta(red, off);
	const double totB = nb.sum.r + nb.sum.g + nb.sum.b;
	const double totR = nr.sum.r + nr.sum.g + nr.sum.b;
	MESSAGE("colour 1 blue: nebula RGB (", nb.sum.r, ", ", nb.sum.g, ", ", nb.sum.b, "); colour 1 red: (",
	        nr.sum.r, ", ", nr.sum.g, ", ", nr.sum.b, ")");
	REQUIRE_MESSAGE(totB > 1e-3 * n * n, colourLog);
	REQUIRE_MESSAGE(totR > 1e-3 * n * n, colourLog);
	CHECK(nb.sum.b / totB > nr.sum.b / totR);
	CHECK(nr.sum.r / totR > nb.sum.r / totB);
	// ...and with colour 1 blue and colours 2/3 nearly black, blue leads.
	CHECK(nb.sum.b > nb.sum.r);

	nebulaD3D11Images() = { off, on[1], true };
}

namespace
{
std::string drainD3D12(ID3D12InfoQueue* iq)
{
	if (!iq) return "(no D3D12 debug layer)";
	std::string out;
	for (UINT64 i = 0, n = iq->GetNumStoredMessages(); i < n; ++i)
	{
		SIZE_T len = 0;
		iq->GetMessage(i, nullptr, &len);
		std::vector<char> buf(len);
		auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
		if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription)
			out += std::string(msg->pDescription, msg->DescriptionByteLength) + "\n";
	}
	iq->ClearStoredMessages();
	return out;
}

struct D3D12Warp
{
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter>  adapter;
	ComPtr<ID3D12Device>  dev;
	ComPtr<ID3D12InfoQueue> iq;

	void init()
	{
		{
			ComPtr<ID3D12Debug> dbg;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
		}
		REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
		REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
		REQUIRE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));
		dev.As(&iq);
	}
	std::string drain() { return drainD3D12(iq.Get()); }
};

// D3D12Renderer::createSkyPipeline's root signature, as built there:
// [0] root CBV b0, [1] SRV table t0..t1, static samplers s0 (clamp) + s1 (wrap).
ComPtr<ID3D12RootSignature> skyRootSignature(ID3D12Device* dev)
{
	ComPtr<ID3D12RootSignature> rootSig;
	D3D12_DESCRIPTOR_RANGE r{};
	r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 2; r.BaseShaderRegister = 0;
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor    = { 0, 0 };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = { 1, &r };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC samps[2]{};
	for (UINT i = 0; i < 2; ++i)
	{
		samps[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		const auto mode = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samps[i].AddressU = samps[i].AddressV = samps[i].AddressW = mode;
		samps[i].ShaderRegister = i; samps[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	D3D12_ROOT_SIGNATURE_DESC rsd{};
	rsd.NumParameters = 2; rsd.pParameters = params;
	rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samps;
	ComPtr<ID3DBlob> sig, err;
	REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
	REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
	                                           IID_PPV_ARGS(&rootSig))));
	return rootSig;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
	bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buf;
	REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&buf))));
	return buf;
}

void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter  = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cl->ResourceBarrier(1, &b);
}

// D3D12Renderer's sky draw on WARP, the D3D12 twin of D3D11SkyRig::draw: the
// same bytecode against skyRootSignature, moon t0 (1x1 white) + noise volume
// t1 in a shader-visible SRV table, constants as a root CBV, one fullscreen
// triangle into an RGBA32F target of n x n pixels, copied to a readback buffer.
// Everything is created per call; the case draws twice.
Image d3d12DrawSky(D3D12Warp& w, ID3D12RootSignature* rootSig, const SkyBytecode& bc,
                   const HE::SkyFrameParams& p, UINT n, std::string& log)
{
	ID3D12Device* dev = w.dev.Get();

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = rootSig;
	pd.VS = { bc.vs->GetBufferPointer(), bc.vs->GetBufferSize() };
	pd.PS = { bc.ps->GetBufferPointer(), bc.ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.RasterizerState.DepthClipEnable = TRUE;
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
	pd.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	{
		const HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
		REQUIRE_MESSAGE(SUCCEEDED(hr), "CreateGraphicsPipelineState (RGBA32F): ", hr, "; ", w.drain());
	}

	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> cl;
	ComPtr<ID3D12Fence> fence;
	{
		D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
		REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
		REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(),
		                                         IID_PPV_ARGS(&cl))));
		REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
	}

	// Textures in COPY_DEST, filled from upload buffers (row pitch 256 for both).
	D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	ComPtr<ID3D12Resource> moonTex, noiseTex;
	{
		D3D12_RESOURCE_DESC td{};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
		REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
		                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&moonTex))));
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		td.Width = td.Height = kTestNoiseN; td.DepthOrArraySize = static_cast<UINT16>(kTestNoiseN);
		td.Format = DXGI_FORMAT_R16G16_UNORM;
		REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
		                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&noiseTex))));
	}
	static_assert(kTestNoiseN * 4 % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0, "noise rows must need no padding");
	constexpr UINT kNoiseRow = kTestNoiseN * 4;
	ComPtr<ID3D12Resource> moonUp  = makeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
	                                            D3D12_RESOURCE_STATE_GENERIC_READ);
	ComPtr<ID3D12Resource> noiseUp = makeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD,
	                                            static_cast<UINT64>(kNoiseRow) * kTestNoiseN * kTestNoiseN,
	                                            D3D12_RESOURCE_STATE_GENERIC_READ);
	constexpr UINT64 kCbSize = (sizeof(HE::SkyFrameParams) + 255) & ~UINT64(255);
	ComPtr<ID3D12Resource> cbUp = makeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, kCbSize, D3D12_RESOURCE_STATE_GENERIC_READ);
	{
		void* mapped = nullptr; const D3D12_RANGE noRead{ 0, 0 };
		REQUIRE(SUCCEEDED(moonUp->Map(0, &noRead, &mapped)));
		const uint32_t white = 0xFFFFFFFFu;
		std::memcpy(mapped, &white, sizeof(white));
		moonUp->Unmap(0, nullptr);

		const std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kTestNoiseN); // tightly packed RG16
		REQUIRE(noise.size() * sizeof(uint16_t) == static_cast<size_t>(kNoiseRow) * kTestNoiseN * kTestNoiseN);
		REQUIRE(SUCCEEDED(noiseUp->Map(0, &noRead, &mapped)));
		std::memcpy(mapped, noise.data(), noise.size() * sizeof(uint16_t));
		noiseUp->Unmap(0, nullptr);

		REQUIRE(SUCCEEDED(cbUp->Map(0, &noRead, &mapped)));
		std::memcpy(mapped, &p, sizeof(p));
		cbUp->Unmap(0, nullptr);
	}

	// SRV table t0..t1 (the root signature's one range) + the render target.
	ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap))));
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))));
	}
	D3D12_CPU_DESCRIPTOR_HANDLE srv = srvHeap->GetCPUDescriptorHandleForHeapStart();
	dev->CreateShaderResourceView(moonTex.Get(), nullptr, srv);
	srv.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	dev->CreateShaderResourceView(noiseTex.Get(), nullptr, srv);

	const float clear[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
	ComPtr<ID3D12Resource> target;
	{
		D3D12_RESOURCE_DESC td{};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = n; td.Height = n; td.DepthOrArraySize = 1; td.MipLevels = 1;
		td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE cv{}; cv.Format = td.Format;
		std::memcpy(cv.Color, clear, sizeof(clear));
		REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
		                  D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&target))));
	}
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	dev->CreateRenderTargetView(target.Get(), nullptr, rtv);

	const UINT readRow = (n * 16u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	ComPtr<ID3D12Resource> readback = makeBuffer(dev, D3D12_HEAP_TYPE_READBACK, static_cast<UINT64>(readRow) * n,
	                                             D3D12_RESOURCE_STATE_COPY_DEST);

	// Record: uploads, draw, copy out.
	{
		D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = moonUp.Get();
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
		dst.pResource = moonTex.Get();
		cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		src.pResource = noiseUp.Get();
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16_UNORM, static_cast<UINT>(kTestNoiseN),
		                                  static_cast<UINT>(kTestNoiseN), static_cast<UINT>(kTestNoiseN), kNoiseRow };
		dst.pResource = noiseTex.Get();
		cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		transition(cl.Get(), moonTex.Get(),  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		transition(cl.Get(), noiseTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(n), static_cast<float>(n), 0.0f, 1.0f };
	const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(n), static_cast<LONG>(n) };
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &scissor);
	cl->SetGraphicsRootSignature(rootSig);
	ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
	cl->SetDescriptorHeaps(1, heaps);
	cl->SetGraphicsRootConstantBufferView(0, cbUp->GetGPUVirtualAddress());
	cl->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->DrawInstanced(3, 1, 0, 0);
	transition(cl.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	{
		D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = target.Get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.pResource = readback.Get();
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, n, n, 1, readRow };
		cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	REQUIRE(SUCCEEDED(cl->Close()));
	ID3D12CommandList* lists[] = { cl.Get() };
	queue->ExecuteCommandLists(1, lists);
	REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), 1)));
	if (fence->GetCompletedValue() < 1)
	{
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		REQUIRE(ev != nullptr);
		REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
		WaitForSingleObject(ev, INFINITE);
		CloseHandle(ev);
	}
	log = w.drain();
	REQUIRE_MESSAGE(SUCCEEDED(dev->GetDeviceRemovedReason()), "device removed: ", dev->GetDeviceRemovedReason(), "; ", log);

	Image img(static_cast<size_t>(n) * n);
	void* mapped = nullptr;
	const D3D12_RANGE readAll{ 0, static_cast<SIZE_T>(readRow) * n };
	REQUIRE(SUCCEEDED(readback->Map(0, &readAll, &mapped)));
	for (UINT y = 0; y < n; ++y)
		std::memcpy(&img[static_cast<size_t>(y) * n], static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * readRow,
		            n * sizeof(glm::vec4));
	const D3D12_RANGE noWrite{ 0, 0 };
	readback->Unmap(0, &noWrite);
	return img;
}
} // namespace

TEST_CASE("D3D12: the cross-compiled sky draws the nebula like D3D11 does (WARP)")
{
	const SkyBytecode& bc = skyBytecode();
	D3D12Warp w;
	w.init();
	const ComPtr<ID3D12RootSignature> rootSig = skyRootSignature(w.dev.Get());
	const UINT n = kNebulaImageN;

	std::string offLog, onLog;
	const Image off = d3d12DrawSky(w, rootSig.Get(), bc, skyNebulaNight(0.0f, 1, kNebulaBlue), n, offLog);
	const Image on  = d3d12DrawSky(w, rootSig.Get(), bc, skyNebulaNight(1.0f, 1, kNebulaBlue), n, onLog);
	REQUIRE_MESSAGE(unshadedReason(off).empty(), "nebula off: ", unshadedReason(off), "; ", offLog);
	REQUIRE_MESSAGE(unshadedReason(on).empty(),  "nebula on: ",  unshadedReason(on),  "; ", onLog);

	const ImageDelta neb = imageDelta(on, off);
	MESSAGE("D3D12 quality 1: nebula adds RGB (", neb.sum.r, ", ", neb.sum.g, ", ", neb.sum.b, "), ",
	        neb.changed * 100.0, "% of pixels changed");
	CHECK_MESSAGE(neb.sum.r + neb.sum.g + neb.sum.b > 1e-3 * n * n, "D3D12 draws no nebula; ", onLog);
	CHECK(neb.changed > 0.05);

	// Same FXC bytecode, same inputs, same WARP rasteriser: D3D12 must agree
	// with the D3D11 image. Only meaningful when the D3D11 case ran first
	// (doctest runs a file's cases in order; a filtered run skips this part).
	const NebulaD3D11Images& d11 = nebulaD3D11Images();
	if (d11.ok)
	{
		const ImageDelta dOff = imageDelta(off, d11.off);
		const ImageDelta dOn  = imageDelta(on,  d11.q1);
		MESSAGE("D3D12 vs D3D11: nebula off mean |delta| ", dOff.meanAbs, " (max ", dOff.maxAbs,
		        "), on mean |delta| ", dOn.meanAbs, " (max ", dOn.maxAbs, ")");
		CHECK(dOff.meanAbs < 1e-3);
		CHECK(dOn.meanAbs < 1e-3);
	}
	else
		MESSAGE("D3D11 nebula images missing (case filtered out or failed) - D3D12/D3D11 comparison skipped");
}

TEST_CASE("D3D12: the cross-compiled sky builds a PSO against the renderer's sky root signature (WARP)")
{
	const SkyBytecode& bc = skyBytecode();
	D3D12Warp w;
	w.init();
	ID3D12Device* dev = w.dev.Get();
	auto drain = [&]() { return w.drain(); };
	const ComPtr<ID3D12RootSignature> rootSig = skyRootSignature(dev);

	// The HDR PSO exactly as makeSkyPso describes it.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = rootSig.Get();
	pd.VS = { bc.vs->GetBufferPointer(), bc.vs->GetBufferSize() };
	pd.PS = { bc.ps->GetBufferPointer(), bc.ps->GetBufferSize() };
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	const HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
	CHECK_MESSAGE(SUCCEEDED(hr), "CreateGraphicsPipelineState: ", hr, "; ", drain());

	// Negative control: the old kSkyVSHLSL order (SV_POSITION before TEXCOORD0)
	// is what kSkyVSCrossHLSL exists to avoid. Logged, not asserted — whether
	// the runtime rejects it is its call; the case above is the contract.
	{
		const char* oldOrderVS =
			"struct O { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };\n"
			"O VSOld(uint vid : SV_VertexID) { O o; float x = (float)((vid & 1u) << 2u) - 1.0f;"
			" float y = (float)((vid & 2u) << 1u) - 1.0f; o.pos = float4(x, y, 1, 1); o.ndc = float2(x, y); return o; }\n";
		ComPtr<ID3DBlob> oldVs;
		std::string err;
		if (fxcCompile(oldOrderVS, "VSOld", "vs_5_0", oldVs, err))
		{
			pd.VS = { oldVs->GetBufferPointer(), oldVs->GetBufferSize() };
			ComPtr<ID3D12PipelineState> oldPso;
			const HRESULT hrOld = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&oldPso));
			MESSAGE("negative control, not a failure: PSO with the old SV_POSITION-first VS ",
			        SUCCEEDED(hrOld) ? "accepted" : "rejected (expected)", " (", hrOld, ") ", drain());
		}
	}
}
#endif // _WIN32
#endif // HE_TESTS_HAVE_SHADERC
