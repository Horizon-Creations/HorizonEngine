#include "doctest.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <HorizonRendering/ClipSpace.h>
#include <HorizonRendering/TemporalAA.h>
#include "../src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#if defined(HE_TESTS_HAVE_SHADERC)   // he_tests links d3d12 + dxgi only then
#include <d3d12.h>
#include <dxgi1_4.h>
#endif
#endif

// ═══ Thema 78, Schritt 6: TAA on D3D11 / D3D12 / Vulkan ═══
// The three backends share the host half (HorizonRendering/TemporalAA.h) and the
// two D3D backends share the HLSL (D3D_Shared/HlslSources.h). The frame itself
// needs a GPU; what can silently go wrong without one is checked here:
//   * the jitter: inside the pixel, 8-periodic, and entering the matrix as a
//     pure clip-space x/y shift of exactly 2*j/size on every clip convention;
//   * the SIGN of the motion. A velocity with the wrong y sign still compiles,
//     still "works" on a still camera, and turns every moving edge into a smear —
//     so the velocity shader and the resolve run on a D3D11 WARP device here and
//     have to agree with each other on which way the history lies.

namespace
{
glm::vec2 ndcOf(const glm::mat4& m, const glm::vec4& p)
{
	const glm::vec4 c = m * p;
	return glm::vec2(c) / c.w;
}
} // namespace

TEST_CASE("taaJitter stays inside the pixel and repeats every 8 TAA frames")
{
	std::set<std::pair<float, float>> seen;
	glm::vec2 sum(0.0f);
	for (uint32_t i = 0; i < 8; ++i)
	{
		const glm::vec2 j = HE::taaJitter(i);
		CHECK(j.x >= -0.5f);
		CHECK(j.x < 0.5f);
		CHECK(j.y >= -0.5f);
		CHECK(j.y < 0.5f);
		seen.insert({ j.x, j.y });
		sum += j;
		// Periodic: frame i and i + 8 sample the same position.
		CHECK(HE::taaJitter(i + 8) == j);
		CHECK(HE::taaJitter(i + 800) == j);
	}
	// Eight distinct positions, none of them the (0,0) centre that index 0 of
	// Halton would give — the sequence starts at 1 on purpose.
	CHECK(seen.size() == 8);
	CHECK(seen.count({ 0.0f, 0.0f }) == 0);
	// Low-discrepancy: the offsets average out near the pixel centre.
	CHECK(std::abs(sum.x / 8.0f) < 0.1f);
	CHECK(std::abs(sum.y / 8.0f) < 0.1f);
	// The first position is Halton(1) = (1/2, 1/3), centred.
	CHECK(HE::taaJitter(0).x == doctest::Approx(0.0f));
	CHECK(HE::taaJitter(0).y == doctest::Approx(1.0f / 3.0f - 0.5f));
}

TEST_CASE("taaJitteredViewProj shifts NDC by exactly 2*j/size and nothing else")
{
	const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
	const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, 2.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
	const int w = 1280, h = 720;
	const glm::vec2 j(0.25f, -0.375f);

	// Every clip convention the three backends rasterise with: GL-style (what
	// D3D11/D3D12 feed their scene), D3D depth-fixed, Vulkan y-flipped. The jitter
	// is applied on the LEFT of the final matrix, so it is the same NDC shift in
	// all of them — no convention can flip or scale it.
	const glm::mat4 conventions[3] = { proj * view, HE::kD3DClipFix * proj * view,
	                                   HE::kVulkanClipFix * proj * view };
	const glm::vec4 points[3] = { glm::vec4(0, 0, 0, 1), glm::vec4(1.5f, -0.5f, 2.0f, 1),
	                              glm::vec4(-3.0f, 1.0f, -4.0f, 1) };
	for (const glm::mat4& vp : conventions)
	{
		const glm::mat4 jit = HE::taaJitteredViewProj(vp, j, w, h);
		for (const glm::vec4& p : points)
		{
			const glm::vec4 c0 = vp * p, c1 = jit * p;
			const glm::vec2 d = ndcOf(jit, p) - ndcOf(vp, p);
			CHECK(d.x == doctest::Approx(2.0f * j.x / w).epsilon(1e-3));
			CHECK(d.y == doctest::Approx(2.0f * j.y / h).epsilon(1e-3));
			// Depth and w untouched: the jittered raster writes the same depth,
			// which is what lets the velocity pass test LESS_EQUAL against it.
			CHECK(c1.z == doctest::Approx(c0.z));
			CHECK(c1.w == doctest::Approx(c0.w));
		}
	}
	// A degenerate size (a minimised viewport) leaves the matrix alone.
	CHECK(HE::taaJitteredViewProj(proj * view, j, 0, h) == proj * view);
	// So does a zero jitter.
	CHECK(HE::taaJitteredViewProj(proj * view, glm::vec2(0.0f), w, h) == proj * view);
}

TEST_CASE("The TAA HLSL keeps the bindings both D3D backends set up")
{
	const std::string vel = HE::hlsl::kTaaVelocityHLSL;
	CHECK(vel.find("register(b0)") != std::string::npos);
	CHECK(vel.find("VSVelocity") != std::string::npos);
	CHECK(vel.find("PSVelocity") != std::string::npos);
	// Same vertex input as the scene VS — the backends reuse its input layout.
	CHECK(vel.find("POSITION") != std::string::npos);
	CHECK(vel.find("NORMAL") != std::string::npos);
	CHECK(vel.find("TEXCOORD0") != std::string::npos);

	const std::string res = HE::hlsl::kTaaResolveHLSL;
	for (const char* reg : { "register(t0)", "register(t1)", "register(t2)", "register(s0)", "register(b0)" })
		CHECK_MESSAGE(res.find(reg) != std::string::npos, reg);
	const std::string shp = HE::hlsl::kTaaSharpenHLSL;
	for (const char* reg : { "register(t0)", "register(s0)", "register(b0)" })
		CHECK_MESSAGE(shp.find(reg) != std::string::npos, reg);
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace
{
ComPtr<ID3DBlob> compileHlsl(const char* src, const char* entry, const char* profile)
{
	ComPtr<ID3DBlob> out, err;
	const HRESULT hr = D3DCompile(src, std::strlen(src), entry, nullptr, nullptr, entry, profile,
	                              0, 0, &out, &err);
	INFO("FXC ", entry, " ", profile, ": ",
	     err ? static_cast<const char*>(err->GetBufferPointer()) : "");
	REQUIRE(SUCCEEDED(hr));
	return out;
}

struct Warp11
{
	ComPtr<ID3D11Device>        dev;
	ComPtr<ID3D11DeviceContext> ctx;
	Warp11()
	{
		const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
		const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &want, 1,
		                                     D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		REQUIRE_MESSAGE(SUCCEEDED(hr), "D3D11CreateDevice(WARP) failed: ", hr);
	}

	// A float render target + its CPU mirror. `data` (optional) is the initial
	// content, row-major, `comps` floats per texel.
	struct Tex
	{
		ComPtr<ID3D11Texture2D>          tex, staging;
		ComPtr<ID3D11RenderTargetView>   rtv;
		ComPtr<ID3D11ShaderResourceView> srv;
	};
	Tex makeTex(UINT w, UINT h, DXGI_FORMAT fmt, UINT comps, const float* data = nullptr)
	{
		Tex t;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = fmt; td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{ data, static_cast<UINT>(w * comps * sizeof(float)), 0 };
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, data ? &init : nullptr, &t.tex)));
		REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)));
		REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)));
		td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t.staging)));
		return t;
	}
	std::vector<float> read(const Tex& t, UINT w, UINT h, UINT comps)
	{
		ctx->CopyResource(t.staging.Get(), t.tex.Get());
		D3D11_MAPPED_SUBRESOURCE m{};
		REQUIRE(SUCCEEDED(ctx->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &m)));
		std::vector<float> out(static_cast<size_t>(w) * h * comps);
		for (UINT y = 0; y < h; ++y)
			std::memcpy(out.data() + static_cast<size_t>(y) * w * comps,
			            static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, w * comps * sizeof(float));
		ctx->Unmap(t.staging.Get(), 0);
		return out;
	}
	ComPtr<ID3D11Buffer> makeCB(const void* data, UINT bytes)
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (bytes + 15u) & ~15u; bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		std::vector<uint8_t> padded(bd.ByteWidth, 0);
		std::memcpy(padded.data(), data, bytes);
		D3D11_SUBRESOURCE_DATA init{ padded.data(), 0, 0 };
		ComPtr<ID3D11Buffer> cb;
		REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, &init, &cb)));
		return cb;
	}
	void viewport(UINT w, UINT h)
	{
		D3D11_VIEWPORT vp{}; vp.Width = float(w); vp.Height = float(h); vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);
		ComPtr<ID3D11RasterizerState> rs;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		REQUIRE(SUCCEEDED(dev->CreateRasterizerState(&rd, &rs)));
		ctx->RSSetState(rs.Get());
	}
};
} // namespace

TEST_CASE("TAA velocity (HLSL, WARP): a surface moving right and up reports +u and -v")
{
	Warp11 d;
	ComPtr<ID3DBlob> vsB = compileHlsl(HE::hlsl::kTaaVelocityHLSL, "VSVelocity", "vs_5_0");
	ComPtr<ID3DBlob> psB = compileHlsl(HE::hlsl::kTaaVelocityHLSL, "PSVelocity", "ps_5_0");
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	REQUIRE(SUCCEEDED(d.dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs)));
	REQUIRE(SUCCEEDED(d.dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &ps)));

	// The scene's vertex layout (pos3 + normal3 + uv2, 32 B), as both D3D
	// backends bind it for the velocity pass.
	const D3D11_INPUT_ELEMENT_DESC il[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	ComPtr<ID3D11InputLayout> layout;
	REQUIRE(SUCCEEDED(d.dev->CreateInputLayout(il, 3, vsB->GetBufferPointer(), vsB->GetBufferSize(), &layout)));
	// One triangle that covers the whole target at z = 0.5.
	const float verts[3 * 8] = {
		-1.0f, -1.0f, 0.5f,  0, 0, 1,  0, 0,
		-1.0f,  3.0f, 0.5f,  0, 0, 1,  0, 0,
		 3.0f, -1.0f, 0.5f,  0, 0, 1,  0, 0,
	};
	ComPtr<ID3D11Buffer> vb;
	{
		D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(verts); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init{ verts, 0, 0 };
		REQUIRE(SUCCEEDED(d.dev->CreateBuffer(&bd, &init, &vb)));
	}

	// Last frame the surface sat 0.2 NDC further left and 0.1 NDC further down
	// (and at a different w, so the divide is exercised); now it is at identity.
	// The jittered raster matrix is deliberately something else again — the
	// motion must come from the two measuring matrices only.
	HE::TaaVelocityConstants c;
	c.mvpJitter = glm::translate(glm::mat4(1.0f), glm::vec3(0.01f, -0.02f, 0.0f));
	c.mvpNow    = glm::mat4(1.0f);
	// Scaled by 2 as a whole: same NDC, w = 2 — a shader that forgot the divide
	// would report twice the motion.
	c.mvpPrev = 2.0f * glm::translate(glm::mat4(1.0f), glm::vec3(-0.2f, -0.1f, 0.0f));
	ComPtr<ID3D11Buffer> cb = d.makeCB(&c, sizeof(c));

	constexpr UINT W = 4, H = 4;
	Warp11::Tex target = d.makeTex(W, H, DXGI_FORMAT_R32G32_FLOAT, 2);
	const float clear[4] = { -9.0f, -9.0f, 0.0f, 0.0f };
	d.ctx->ClearRenderTargetView(target.rtv.Get(), clear);
	d.ctx->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
	d.viewport(W, H);
	const UINT stride = 32, offset = 0;
	d.ctx->IASetInputLayout(layout.Get());
	d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d.ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
	d.ctx->VSSetShader(vs.Get(), nullptr, 0);
	d.ctx->PSSetShader(ps.Get(), nullptr, 0);
	d.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
	d.ctx->Draw(3, 0);

	const std::vector<float> v = d.read(target, W, H, 2);
	for (UINT i = 0; i < W * H; ++i)
	{
		// ndcNow - ndcPrev = (+0.2, +0.1): right and UP on screen. D3D's v
		// points down, so up is a smaller v: uvNow - uvPrev = (+0.1, -0.05).
		CHECK(v[i * 2 + 0] == doctest::Approx(0.1f).epsilon(1e-4));
		CHECK(v[i * 2 + 1] == doctest::Approx(-0.05f).epsilon(1e-4));
	}
}

TEST_CASE("TAA resolve (HLSL, WARP): history is fetched from where the velocity says it was")
{
	Warp11 d;
	ComPtr<ID3DBlob> vsB = compileHlsl(HE::hlsl::kFSTriangleVS, "main", "vs_5_0");
	ComPtr<ID3DBlob> psB = compileHlsl(HE::hlsl::kTaaResolveHLSL, "main", "ps_5_0");
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	REQUIRE(SUCCEEDED(d.dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs)));
	REQUIRE(SUCCEEDED(d.dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &ps)));

	// A 2D gradient that moved by (+1, +2) pixels (right, and DOWN in texture
	// rows) since last frame: history(p) = current(p + (1, 2)). The velocity
	// pass writes that motion in uv units as uvNow - uvPrev = (+1/W, +2/H).
	// Reprojecting correctly (uv - vel) lands on a history texel that holds
	// exactly this frame's colour, so the resolve reproduces the current image
	// whatever the blend weight; reprojecting the wrong way lands two texels off
	// and the neighbourhood clamp can only pull it one step back.
	constexpr UINT W = 16, H = 16;
	auto cur = [&](int x, int y) { return glm::vec2((x + 0.5f) / W, (y + 0.5f) / H); };
	std::vector<float> current(W * H * 4), history(W * H * 4), velocity(W * H * 2);
	for (int y = 0; y < int(H); ++y)
		for (int x = 0; x < int(W); ++x)
		{
			const size_t i = static_cast<size_t>(y) * W + x;
			const glm::vec2 c = cur(x, y), hsh = cur(x + 1, y + 2);
			current[i * 4 + 0] = c.x;   current[i * 4 + 1] = c.y;   current[i * 4 + 2] = 0.25f; current[i * 4 + 3] = 1;
			history[i * 4 + 0] = hsh.x; history[i * 4 + 1] = hsh.y; history[i * 4 + 2] = 0.25f; history[i * 4 + 3] = 1;
			velocity[i * 2 + 0] = 1.0f / W;
			velocity[i * 2 + 1] = 2.0f / H;
		}
	Warp11::Tex curT = d.makeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, current.data());
	Warp11::Tex hisT = d.makeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, history.data());
	Warp11::Tex velT = d.makeTex(W, H, DXGI_FORMAT_R32G32_FLOAT, 2, velocity.data());
	Warp11::Tex outT = d.makeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 4);

	ComPtr<ID3D11SamplerState> linearClamp;
	{
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		REQUIRE(SUCCEEDED(d.dev->CreateSamplerState(&sd, &linearClamp)));
	}

	auto resolve = [&](float blend) {
		const float params[4] = { 1.0f / W, 1.0f / H, blend, 0.0f };
		ComPtr<ID3D11Buffer> cb = d.makeCB(params, sizeof(params));
		d.ctx->OMSetRenderTargets(1, outT.rtv.GetAddressOf(), nullptr);
		d.viewport(W, H);
		d.ctx->IASetInputLayout(nullptr);
		d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d.ctx->VSSetShader(vs.Get(), nullptr, 0);
		d.ctx->PSSetShader(ps.Get(), nullptr, 0);
		d.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
		ID3D11ShaderResourceView* srvs[3] = { curT.srv.Get(), hisT.srv.Get(), velT.srv.Get() };
		d.ctx->PSSetShaderResources(0, 3, srvs);
		d.ctx->PSSetSamplers(0, 1, linearClamp.GetAddressOf());
		d.ctx->Draw(3, 0);
		ID3D11ShaderResourceView* nulls[3] = {};
		d.ctx->PSSetShaderResources(0, 3, nulls);
		ID3D11RenderTargetView* n = nullptr;
		d.ctx->OMSetRenderTargets(1, &n, nullptr);
		return d.read(outT, W, H, 4);
	};

	// Interior pixels only: at the border the reprojection leaves the texture
	// (current only, by design) or the clamp-to-edge fetch repeats a row.
	auto checkInterior = [&](const std::vector<float>& out, const char* what) {
		int bad = 0;
		for (int y = 3; y < int(H) - 3; ++y)
			for (int x = 3; x < int(W) - 3; ++x)
			{
				const size_t i = static_cast<size_t>(y) * W + x;
				const glm::vec2 want = cur(x, y);
				if (std::abs(out[i * 4 + 0] - want.x) > 1e-3f || std::abs(out[i * 4 + 1] - want.y) > 1e-3f)
				{
					if (bad++ < 4)
						MESSAGE(what, ": pixel (", x, ",", y, ") = (", out[i * 4 + 0], ", ", out[i * 4 + 1],
						        ") want (", want.x, ", ", want.y, ")");
				}
			}
		CHECK_MESSAGE(bad == 0, what);
	};
	checkInterior(resolve(0.0f), "blend 0 (no history) passes the current frame through");
	checkInterior(resolve(HE::kTaaHistoryBlend), "blend 0.9 reprojects onto this frame's colour");

	// Negative control: the same history read along the OPPOSITE motion must
	// NOT reproduce the current image — otherwise the case above proves nothing.
	for (size_t i = 0; i < W * H; ++i) { velocity[i * 2 + 0] = -1.0f / W; velocity[i * 2 + 1] = -2.0f / H; }
	d.ctx->UpdateSubresource(velT.tex.Get(), 0, nullptr, velocity.data(), W * 2 * sizeof(float), 0);
	const std::vector<float> wrong = resolve(HE::kTaaHistoryBlend);
	const size_t mid = static_cast<size_t>(H / 2) * W + W / 2;
	CHECK(std::abs(wrong[mid * 4 + 1] - cur(W / 2, H / 2).y) > 0.5f / H);
}

TEST_CASE("TAA sharpen (HLSL, WARP): amount 0 is an exact copy, a flat image stays flat")
{
	Warp11 d;
	ComPtr<ID3DBlob> vsB = compileHlsl(HE::hlsl::kFSTriangleVS, "main", "vs_5_0");
	ComPtr<ID3DBlob> psB = compileHlsl(HE::hlsl::kTaaSharpenHLSL, "main", "ps_5_0");
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader>  ps;
	REQUIRE(SUCCEEDED(d.dev->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &vs)));
	REQUIRE(SUCCEEDED(d.dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &ps)));
	ComPtr<ID3D11SamplerState> linearClamp;
	{
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		REQUIRE(SUCCEEDED(d.dev->CreateSamplerState(&sd, &linearClamp)));
	}
	constexpr UINT W = 8, H = 8;
	std::vector<float> img(W * H * 4), flat(W * H * 4, 0.4f);
	for (size_t i = 0; i < W * H; ++i)
	{
		img[i * 4 + 0] = (i % 3) * 0.3f; img[i * 4 + 1] = (i % 5) * 0.2f; img[i * 4 + 2] = 0.5f; img[i * 4 + 3] = 1;
	}
	auto run = [&](const std::vector<float>& src, float amount) {
		Warp11::Tex inT  = d.makeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, src.data());
		Warp11::Tex outT = d.makeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 4);
		const float params[4] = { 1.0f / W, 1.0f / H, amount, 0.0f };
		ComPtr<ID3D11Buffer> cb = d.makeCB(params, sizeof(params));
		d.ctx->OMSetRenderTargets(1, outT.rtv.GetAddressOf(), nullptr);
		d.viewport(W, H);
		d.ctx->IASetInputLayout(nullptr);
		d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d.ctx->VSSetShader(vs.Get(), nullptr, 0);
		d.ctx->PSSetShader(ps.Get(), nullptr, 0);
		d.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
		d.ctx->PSSetShaderResources(0, 1, inT.srv.GetAddressOf());
		d.ctx->PSSetSamplers(0, 1, linearClamp.GetAddressOf());
		d.ctx->Draw(3, 0);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		d.ctx->PSSetShaderResources(0, 1, &nullSrv);
		return d.read(outT, W, H, 4);
	};
	const std::vector<float> copy = run(img, 0.0f);
	for (size_t i = 0; i < W * H; ++i)
		for (int c = 0; c < 3; ++c)
			CHECK(copy[i * 4 + c] == doctest::Approx(img[i * 4 + c]).epsilon(1e-5));
	const std::vector<float> sharpFlat = run(flat, 1.0f);
	for (size_t i = 0; i < W * H; ++i)
		CHECK(sharpFlat[i * 4 + 0] == doctest::Approx(0.4f).epsilon(1e-5));
}

#if defined(HE_TESTS_HAVE_SHADERC)
// D3D12Renderer builds three things for TAA that no draw on this runner ever
// exercises: the velocity root signature (root CBV b0), the resolve root
// signature (b0 constants + ONE t0..t2 table — the postFx one has single-SRV
// tables) and the sharpen on the postFx root signature. CreateGraphicsPipeline-
// State is the only judge of "does the root signature cover every register the
// bytecode binds"; the descs below are D3D12Renderer::createTaaPipelines'.
TEST_CASE("D3D12: the TAA velocity, resolve and sharpen PSOs build against their root signatures (WARP)")
{
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter>  adapter;
	ComPtr<ID3D12Device>  dev;
	REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
	REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
	REQUIRE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))));

	auto rootSig = [&](const D3D12_ROOT_SIGNATURE_DESC& rsd) {
		ComPtr<ID3DBlob> sig, err;
		REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
		ComPtr<ID3D12RootSignature> rs;
		REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
		                                           IID_PPV_ARGS(&rs))));
		return rs;
	};
	D3D12_STATIC_SAMPLER_DESC samp{};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.MaxLOD = D3D12_FLOAT32_MAX;
	samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	ComPtr<ID3D12RootSignature> velRS, resolveRS, postFxRS;
	{
		D3D12_ROOT_PARAMETER rp{};
		rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rp.Descriptor.ShaderRegister = 0;
		rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		D3D12_ROOT_SIGNATURE_DESC rsd{};
		rsd.NumParameters = 1; rsd.pParameters = &rp;
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		velRS = rootSig(rsd);
	}
	{
		D3D12_DESCRIPTOR_RANGE r{};
		r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 3; r.BaseShaderRegister = 0;
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, 4 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 1, &r }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rsd{};
		rsd.NumParameters = 2; rsd.pParameters = params;
		rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
		resolveRS = rootSig(rsd);
	}
	{
		// D3D12Renderer::createPostFXPipelines' root signature, verbatim.
		D3D12_DESCRIPTOR_RANGE r0{}; r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
		D3D12_DESCRIPTOR_RANGE r1{}; r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, 4 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 1, &r0 }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable = { 1, &r1 }; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC ps = samp; ps.MaxLOD = 0.0f;
		D3D12_ROOT_SIGNATURE_DESC rsd{};
		rsd.NumParameters = 3; rsd.pParameters = params;
		rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ps;
		postFxRS = rootSig(rsd);
	}

	ComPtr<ID3DBlob> fsVS  = compileHlsl(HE::hlsl::kFSTriangleVS,    "main",       "vs_5_0");
	ComPtr<ID3DBlob> velVS = compileHlsl(HE::hlsl::kTaaVelocityHLSL, "VSVelocity", "vs_5_0");
	ComPtr<ID3DBlob> velPS = compileHlsl(HE::hlsl::kTaaVelocityHLSL, "PSVelocity", "ps_5_0");
	ComPtr<ID3DBlob> resPS = compileHlsl(HE::hlsl::kTaaResolveHLSL,  "main",       "ps_5_0");
	ComPtr<ID3DBlob> shpPS = compileHlsl(HE::hlsl::kTaaSharpenHLSL,  "main",       "ps_5_0");

	{
		const D3D12_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature        = velRS.Get();
		pd.VS                    = { velVS->GetBufferPointer(), velVS->GetBufferSize() };
		pd.PS                    = { velPS->GetBufferPointer(), velPS->GetBufferSize() };
		pd.InputLayout           = { layout, 3 };
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets      = 1;
		pd.RTVFormats[0]         = DXGI_FORMAT_R16G16_FLOAT;
		pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
		pd.SampleDesc.Count      = 1;
		pd.SampleMask            = UINT_MAX;
		pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.DepthStencilState.DepthEnable    = TRUE;
		pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		ComPtr<ID3D12PipelineState> pso;
		const HRESULT hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
		CHECK_MESSAGE(SUCCEEDED(hr), "velocity PSO: ", hr);
	}

	auto fullscreen = [&](ID3D12RootSignature* rs, ID3DBlob* ps) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = rs;
		pd.VS = { fsVS->GetBufferPointer(), fsVS->GetBufferSize() };
		pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.DepthStencilState.DepthEnable = FALSE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		ComPtr<ID3D12PipelineState> pso;
		return dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
	};
	const HRESULT resHr = fullscreen(resolveRS.Get(), resPS.Get());
	CHECK_MESSAGE(SUCCEEDED(resHr), "resolve PSO: ", resHr);
	const HRESULT shpHr = fullscreen(postFxRS.Get(), shpPS.Get());
	CHECK_MESSAGE(SUCCEEDED(shpHr), "sharpen PSO: ", shpHr);
	// Negative control: the resolve against the postFx root signature — whose
	// tables stop at t1 — must be REJECTED. That is why it has its own root
	// signature, and it proves the positive checks above can fail at all.
	const HRESULT wrongHr = fullscreen(postFxRS.Get(), resPS.Get());
	CHECK_MESSAGE(FAILED(wrongHr), "resolve PSO without t2 in the root signature was accepted");
}
#endif // HE_TESTS_HAVE_SHADERC
#endif
