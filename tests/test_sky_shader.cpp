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
#include <chrono>
#include <climits>
#include <cmath>
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
} // namespace

TEST_CASE("D3D11: the cross-compiled sky draws a blue zenith by day and a dark one by night (WARP)")
{
	const SkyBytecode& bc = skyBytecode();

	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	ComPtr<ID3D11InfoQueue> iq;
	{
		const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
		                               &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		if (FAILED(hr))
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			                       &want, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		REQUIRE_MESSAGE(SUCCEEDED(hr), "D3D11CreateDevice(WARP) failed: ", hr);
		dev.As(&iq); // null without the SDK layers
	}
	auto drain = [&]() -> std::string {
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
	};

	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	REQUIRE(SUCCEEDED(dev->CreateVertexShader(bc.vs->GetBufferPointer(), bc.vs->GetBufferSize(), nullptr, &vs)));
	REQUIRE(SUCCEEDED(dev->CreatePixelShader(bc.ps->GetBufferPointer(), bc.ps->GetBufferSize(), nullptr, &ps)));

	// The renderer's resources: moon t0 (1x1 white) + clamp s0, noise volume t1 + wrap s1.
	ComPtr<ID3D11ShaderResourceView> moonSrv, noiseSrv;
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
		constexpr int kN = 16;
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
	ComPtr<ID3D11SamplerState> clampS = sampler(D3D11_TEXTURE_ADDRESS_CLAMP);
	ComPtr<ID3D11SamplerState> wrapS  = sampler(D3D11_TEXTURE_ADDRESS_WRAP);

	ComPtr<ID3D11Buffer> cb;
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(HE::SkyFrameParams);
		bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb)));
	}

	ComPtr<ID3D11Texture2D> target, staging;
	ComPtr<ID3D11RenderTargetView> rtv;
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &target)));
		REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(target.Get(), nullptr, &rtv)));
		td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging)));
	}
	ComPtr<ID3D11RasterizerState> rs;
	{
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		REQUIRE(SUCCEEDED(dev->CreateRasterizerState(&rd, &rs)));
	}

	auto drawPixel = [&](const HE::SkyFrameParams& p) -> glm::vec4 {
		D3D11_MAPPED_SUBRESOURCE m{};
		REQUIRE(SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)));
		std::memcpy(m.pData, &p, sizeof(p));
		ctx->Unmap(cb.Get(), 0);
		const float clear[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
		ctx->ClearRenderTargetView(rtv.Get(), clear);
		ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
		ctx->OMSetRenderTargets(1, rtvs, nullptr);
		D3D11_VIEWPORT vp{}; vp.Width = 1.0f; vp.Height = 1.0f; vp.MaxDepth = 1.0f;
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
		ctx->CopyResource(staging.Get(), target.Get());
		glm::vec4 px(0.0f);
		REQUIRE(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)));
		std::memcpy(&px, m.pData, sizeof(px));
		ctx->Unmap(staging.Get(), 0);
		return px;
	};

	const glm::vec4 day   = drawPixel(skyLookingUp(glm::vec3(0.3f,  0.8f, 0.2f)));
	const std::string dayLog = drain();
	const glm::vec4 night = drawPixel(skyLookingUp(glm::vec3(0.3f, -0.8f, 0.2f)));
	const std::string nightLog = drain();
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

TEST_CASE("D3D12: the cross-compiled sky builds a PSO against the renderer's sky root signature (WARP)")
{
	const SkyBytecode& bc = skyBytecode();

	{
		ComPtr<ID3D12Debug> dbg;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
	}
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter>  adapter;
	ComPtr<ID3D12Device>  dev;
	REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
	REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
	REQUIRE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));
	ComPtr<ID3D12InfoQueue> iq;
	dev.As(&iq);
	auto drain = [&]() -> std::string {
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
	};

	// D3D12Renderer::createSkyPipeline's root signature, as built there:
	// [0] root CBV b0, [1] SRV table t0..t1, static samplers s0 (clamp) + s1 (wrap).
	ComPtr<ID3D12RootSignature> rootSig;
	{
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
	}

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
