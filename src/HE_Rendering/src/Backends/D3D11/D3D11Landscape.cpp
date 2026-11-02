#include "Backends/D3D11/D3D11Landscape.h"

#include "Backends/D3D_Shared/LandscapeD3D.h"

#include <HorizonRendering/LandscapeShaderSource.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace HE;

struct D3D11Landscape::Impl
{
	ID3D11Device* device = nullptr;   // not owned: the renderer's, which outlives this
	ComPtr<ID3D11VertexShader>      vs;
	ComPtr<ID3D11PixelShader>       ps;
	ComPtr<ID3D11DepthStencilState> depth;
	ComPtr<ID3D11RasterizerState>   raster;
	ComPtr<ID3D11BlendState>        blend;
	ComPtr<ID3D11SamplerState>      sampler;   // the albedo array's: linear, repeat, mips
	ComPtr<ID3D11Buffer>            globalsCB, patchCB, indices;

	// the nine images, in the register order of kLandscapeHLSL (t32 ..): tile heights, slot table, coarse heights, 3 x tile weights, 3 x coarse weights
	ComPtr<ID3D11Texture2D>          tex[d3dls::kSrvCount];
	ComPtr<ID3D11ShaderResourceView> srv[d3dls::kSrvCount];
	std::vector<uint16_t> table;      // CPU mirror of the slot table: a change is one UpdateSubresource of the whole (small) image
	std::shared_ptr<const LandscapeRenderStatic> world;   // what the images were made for
	Stats stats;
	bool ok = false;

	void destroyImages()
	{
		for (unsigned i = 0; i < d3dls::kSrvCount; ++i) { srv[i].Reset(); tex[i].Reset(); }
		indices.Reset();
		table.clear();
		world.reset();
	}

	bool makeImage(unsigned i, DXGI_FORMAT fmt, int w, int h, int layers, const void* data, UINT rowPitch)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = static_cast<UINT>(std::max(w, 1));
		d.Height = static_cast<UINT>(std::max(h, 1));
		d.MipLevels = 1;
		d.ArraySize = static_cast<UINT>(std::max(layers, 1));
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{};
		init.pSysMem = data;
		init.SysMemPitch = rowPitch;
		if (FAILED(device->CreateTexture2D(&d, data ? &init : nullptr, tex[i].GetAddressOf()))) return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC v{};
		v.Format = fmt;
		if (layers > 0)
		{
			v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			v.Texture2DArray.MipLevels = 1;
			v.Texture2DArray.ArraySize = d.ArraySize;
		}
		else
		{
			v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			v.Texture2D.MipLevels = 1;
		}
		return SUCCEEDED(device->CreateShaderResourceView(tex[i].Get(), &v, srv[i].GetAddressOf()));
	}
};

namespace
{
bool compileStage(const char* entry, const char* profile, ComPtr<ID3DBlob>& out, std::string& error)
{
	ComPtr<ID3DBlob> errors;
	const char* src = HE::glsl::kLandscapeHLSL;
	const HRESULT hr = D3DCompile(src, std::strlen(src), "landscape.hlsl", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
	                              out.GetAddressOf(), errors.GetAddressOf());
	if (FAILED(hr))
	{
		error = std::string("landscape ") + profile + ": " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
		return false;
	}
	return true;
}

// Puts the context back as it was found, whatever draw() returns through.
struct StateGuard
{
	ID3D11DeviceContext* ctx;
	ComPtr<ID3D11VertexShader>      vs;
	ComPtr<ID3D11PixelShader>       ps;
	ComPtr<ID3D11InputLayout>       layout;
	D3D11_PRIMITIVE_TOPOLOGY        topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	ComPtr<ID3D11Buffer>            ib;
	DXGI_FORMAT                     ibFormat = DXGI_FORMAT_UNKNOWN;
	UINT                            ibOffset = 0;
	ComPtr<ID3D11RasterizerState>   raster;
	ComPtr<ID3D11DepthStencilState> depth;
	UINT                            stencilRef = 0;
	ComPtr<ID3D11BlendState>        blend;
	float                           blendFactor[4] = {};
	UINT                            sampleMask = 0xFFFFFFFF;
	ComPtr<ID3D11Buffer>            vsCB[2], psCB;
	ComPtr<ID3D11ShaderResourceView> vsSrv[d3dls::kSrvCount], psSrv[d3dls::kSrvCount];
	ComPtr<ID3D11SamplerState>       psSampler;

	explicit StateGuard(ID3D11DeviceContext* c) : ctx(c)
	{
		ID3D11SamplerState* smp = nullptr;
		ctx->PSGetSamplers(d3dls::kSamplerReg, 1, &smp);
		psSampler.Attach(smp);
		ctx->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
		ctx->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
		ctx->IAGetInputLayout(layout.GetAddressOf());
		ctx->IAGetPrimitiveTopology(&topology);
		ctx->IAGetIndexBuffer(ib.GetAddressOf(), &ibFormat, &ibOffset);
		ctx->RSGetState(raster.GetAddressOf());
		ctx->OMGetDepthStencilState(depth.GetAddressOf(), &stencilRef);
		ctx->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
		ID3D11Buffer* b[2] = {};
		ctx->VSGetConstantBuffers(d3dls::kGlobalsReg, 2, b);
		vsCB[0].Attach(b[0]); vsCB[1].Attach(b[1]);
		ID3D11Buffer* pb = nullptr;
		ctx->PSGetConstantBuffers(d3dls::kGlobalsReg, 1, &pb);
		psCB.Attach(pb);
		ID3D11ShaderResourceView* v[d3dls::kSrvCount] = {};
		ctx->VSGetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, v);
		for (unsigned i = 0; i < d3dls::kSrvCount; ++i) vsSrv[i].Attach(v[i]);
		ID3D11ShaderResourceView* p[d3dls::kSrvCount] = {};
		ctx->PSGetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, p);
		for (unsigned i = 0; i < d3dls::kSrvCount; ++i) psSrv[i].Attach(p[i]);
	}
	~StateGuard()
	{
		ctx->VSSetShader(vs.Get(), nullptr, 0);
		ctx->PSSetShader(ps.Get(), nullptr, 0);
		ctx->IASetInputLayout(layout.Get());
		ctx->IASetPrimitiveTopology(topology);
		ctx->IASetIndexBuffer(ib.Get(), ibFormat, ibOffset);
		ctx->RSSetState(raster.Get());
		ctx->OMSetDepthStencilState(depth.Get(), stencilRef);
		ctx->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
		ID3D11Buffer* b[2] = { vsCB[0].Get(), vsCB[1].Get() };
		ctx->VSSetConstantBuffers(d3dls::kGlobalsReg, 2, b);
		ID3D11Buffer* pb = psCB.Get();
		ctx->PSSetConstantBuffers(d3dls::kGlobalsReg, 1, &pb);
		ID3D11ShaderResourceView* v[d3dls::kSrvCount];
		ID3D11ShaderResourceView* p[d3dls::kSrvCount];
		for (unsigned i = 0; i < d3dls::kSrvCount; ++i) { v[i] = vsSrv[i].Get(); p[i] = psSrv[i].Get(); }
		ctx->VSSetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, v);
		ctx->PSSetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, p);
		ID3D11SamplerState* smp = psSampler.Get();
		ctx->PSSetSamplers(d3dls::kSamplerReg, 1, &smp);
	}
};
} // namespace

D3D11Landscape::D3D11Landscape() : m_impl(std::make_unique<Impl>()) {}
D3D11Landscape::~D3D11Landscape() = default;
bool D3D11Landscape::ready() const { return m_impl->ok; }
const D3D11Landscape::Stats& D3D11Landscape::stats() const { return m_impl->stats; }

bool D3D11Landscape::init(ID3D11Device* device, std::string* error)
{
	Impl& m = *m_impl;
	if (m.ok) return true;
	std::string err;
	if (!device) { if (error) *error = "landscape: no device"; return false; }
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
	{
		if (error) *error = "landscape: needs feature level 11_0 (shader model 5)";
		return false;
	}
	m.device = device;

	ComPtr<ID3DBlob> vsb, psb;
	if (!compileStage("VSMain", "vs_5_0", vsb, err) || !compileStage("PSMain", "ps_5_0", psb, err))
	{
		if (error) *error = err;
		return false;
	}
	if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, m.vs.GetAddressOf()))
	    || FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, m.ps.GetAddressOf())))
	{
		if (error) *error = "landscape: CreateVertexShader / CreatePixelShader failed";
		return false;
	}

	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = TRUE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc = D3D11_COMPARISON_LESS;
	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	D3D11_BLEND_DESC bs{};
	bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(device->CreateSamplerState(&sd, m.sampler.GetAddressOf())))
	{
		if (error) *error = "landscape: sampler state";
		return false;
	}
	D3D11_BUFFER_DESC cb{};
	cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	cb.ByteWidth = sizeof(d3dls::Globals);
	if (FAILED(device->CreateDepthStencilState(&ds, m.depth.GetAddressOf())) || FAILED(device->CreateRasterizerState(&rs, m.raster.GetAddressOf()))
	    || FAILED(device->CreateBlendState(&bs, m.blend.GetAddressOf())) || FAILED(device->CreateBuffer(&cb, nullptr, m.globalsCB.GetAddressOf())))
	{
		if (error) *error = "landscape: states or constant buffer could not be created";
		return false;
	}
	cb.ByteWidth = sizeof(d3dls::PatchData);
	if (FAILED(device->CreateBuffer(&cb, nullptr, m.patchCB.GetAddressOf())))
	{
		if (error) *error = "landscape: the patch constant buffer could not be created";
		return false;
	}
	m.ok = true;
	return true;
}

void D3D11Landscape::shutdown()
{
	Impl& m = *m_impl;
	m.destroyImages();
	m.vs.Reset(); m.ps.Reset(); m.depth.Reset(); m.raster.Reset(); m.blend.Reset(); m.sampler.Reset(); m.globalsCB.Reset(); m.patchCB.Reset();
	m.device = nullptr;
	m.ok = false;
}

void D3D11Landscape::upload(ID3D11DeviceContext* ctx, HE::LandscapeRenderFrame& f)
{
	Impl& m = *m_impl;
	if (!m.ok || !f.active || !f.world) { f.uploads.clear(); f.tableUpdates.clear(); return; }
	const LandscapeRenderStatic& w = *f.world;
	if (!d3dls::worldUsable(w)) { ++m.stats.rejected; f.uploads.clear(); f.tableUpdates.clear(); return; }

	// ── the images of a landscape that began (or the first frame) ─────────────
	if (m.world != f.world && (!m.world || m.world->generation != w.generation))
	{
		m.destroyImages();
		bool good = true;
		good &= m.makeImage(0, DXGI_FORMAT_R16_UINT, w.heightW, w.heightH, w.slotCapacity, nullptr, 0);
		for (unsigned p = 0; p < 3; ++p)   // pages the tiles do not have stay zero (a landscape of fewer than 9 channels); the shader reads all three
			good &= m.makeImage(3 + p, DXGI_FORMAT_R8G8B8A8_UNORM, w.weightW, w.weightH, w.slotCapacity, nullptr, 0);

		m.table.assign(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
		good &= m.makeImage(1, DXGI_FORMAT_R16_UINT, w.tableWindow, w.tableWindow, 0, m.table.data(), static_cast<UINT>(w.tableWindow) * 2);

		const bool haveCoarse = w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH) && w.coarseW > 0 && w.coarseH > 0;
		if (!haveCoarse) ++m.stats.rejected;
		const std::vector<uint16_t> noHeights(haveCoarse ? 0 : 1, 0);
		good &= m.makeImage(2, DXGI_FORMAT_R16_UINT, haveCoarse ? w.coarseW : 1, haveCoarse ? w.coarseH : 1, 0,
		                    haveCoarse ? w.coarseHeights.data() : noHeights.data(), static_cast<UINT>(haveCoarse ? w.coarseW : 1) * 2);

		const int cw = std::max(w.coarseWeightW, 1), ch = std::max(w.coarseWeightH, 1);
		for (unsigned p = 0; p < 3; ++p)
		{
			const bool have = p < w.coarseWeights.size() && w.coarseWeights[p].size() == static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4;
			const std::vector<uint8_t> none(have ? 0 : static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4, 0);   // a page the world does not have is zero
			good &= m.makeImage(6 + p, DXGI_FORMAT_R8G8B8A8_UNORM, cw, ch, 0, have ? w.coarseWeights[p].data() : none.data(), static_cast<UINT>(cw) * 4);
		}

		D3D11_BUFFER_DESC ib{};
		ib.ByteWidth = static_cast<UINT>(w.patchIndices.size() * sizeof(uint16_t));
		ib.Usage = D3D11_USAGE_IMMUTABLE;
		ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA id{};
		id.pSysMem = w.patchIndices.data();
		good &= SUCCEEDED(m.device->CreateBuffer(&ib, &id, m.indices.GetAddressOf()));
		if (!good)
		{
			m.destroyImages();
			++m.stats.rejected;
			f.uploads.clear();
			f.tableUpdates.clear();
			return;
		}
		m.world = f.world;
		++m.stats.generations;
	}
	if (!m.world) { f.uploads.clear(); f.tableUpdates.clear(); return; }

	// ── the frame's uploads, then its table changes (a slot's image is in before the table says so) ──
	const size_t hBytes = static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH);
	const size_t wBytes = static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4;
	for (const LandscapeRenderUpload& u : f.uploads)
	{
		if (u.slot >= w.slotCapacity || u.heights.size() != hBytes) { ++m.stats.rejected; continue; }
		ctx->UpdateSubresource(m.tex[0].Get(), D3D11CalcSubresource(0, u.slot, 1), nullptr, u.heights.data(), static_cast<UINT>(w.heightW) * 2, 0);
		for (size_t p = 0; p < u.pages.size() && p < 3; ++p)
		{
			if (u.pages[p].size() != wBytes) { ++m.stats.rejected; continue; }
			ctx->UpdateSubresource(m.tex[3 + p].Get(), D3D11CalcSubresource(0, u.slot, 1), nullptr, u.pages[p].data(), static_cast<UINT>(w.weightW) * 4, 0);
		}
		++m.stats.uploads;
	}
	bool tableDirty = false;
	for (const LandscapeRenderTableUpdate& t : f.tableUpdates)
	{
		if (t.index >= m.table.size()) { ++m.stats.rejected; continue; }
		m.table[t.index] = t.slot;
		tableDirty = true;
		++m.stats.tableUpdates;
	}
	if (tableDirty)
		ctx->UpdateSubresource(m.tex[1].Get(), 0, nullptr, m.table.data(), static_cast<UINT>(w.tableWindow) * 2, 0);
	f.uploads.clear();
	f.tableUpdates.clear();
}

void D3D11Landscape::draw(ID3D11DeviceContext* ctx, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, const Shading& shading)
{
	Impl& m = *m_impl;
	m.stats.patches = m.stats.drawCalls = 0;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation) return;
	const LandscapeRenderStatic& w = *m.world;
	StateGuard guard(ctx);

	d3dls::Globals g;
	d3dls::fillGlobals(g, f, viewProj, shading.sunDir, shading.sunColor, shading.ambient, shading.albedoArray != nullptr);
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(ctx->Map(m.globalsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
	std::memcpy(mapped.pData, &g, sizeof(g));
	ctx->Unmap(m.globalsCB.Get(), 0);

	ID3D11ShaderResourceView* views[d3dls::kSrvCount];
	for (unsigned i = 0; i < d3dls::kImageCount; ++i) views[i] = m.srv[i].Get();
	views[d3dls::kImageCount] = shading.albedoArray;   // t41: the world-UV albedo array, or nothing (the shader reads it only where tex.y says so)
	ID3D11SamplerState* smp = m.sampler.Get();
	ctx->PSSetSamplers(d3dls::kSamplerReg, 1, &smp);
	ctx->VSSetShader(m.vs.Get(), nullptr, 0);
	ctx->PSSetShader(m.ps.Get(), nullptr, 0);
	ctx->IASetInputLayout(nullptr);   // the grid comes from SV_VertexID
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->IASetIndexBuffer(m.indices.Get(), DXGI_FORMAT_R16_UINT, 0);
	ctx->RSSetState(m.raster.Get());
	ctx->OMSetDepthStencilState(m.depth.Get(), 0);
	ctx->OMSetBlendState(m.blend.Get(), nullptr, 0xFFFFFFFF);
	ID3D11Buffer* cbs[2] = { m.globalsCB.Get(), m.patchCB.Get() };
	ctx->VSSetConstantBuffers(d3dls::kGlobalsReg, 2, cbs);
	ctx->PSSetConstantBuffers(d3dls::kGlobalsReg, 1, cbs);
	ctx->VSSetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, views);
	ctx->PSSetShaderResources(d3dls::kFirstSrv, d3dls::kSrvCount, views);

	const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
	for (const LandscapeRenderPatch& p : f.patches)
	{
		d3dls::PatchData pd;
		d3dls::fillPatch(pd, p);
		if (FAILED(ctx->Map(m.patchCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) break;
		std::memcpy(mapped.pData, &pd, sizeof(pd));
		ctx->Unmap(m.patchCB.Get(), 0);
		if ((p.quadrants & 15) == 15)
		{
			ctx->DrawIndexed(total, 0, 0);
			++m.stats.drawCalls;
		}
		else
		{
			for (int q = 0; q < 4; ++q)
			{
				if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
				ctx->DrawIndexed(w.quadrantCount[q], w.quadrantFirst[q], 0);
				++m.stats.drawCalls;
			}
		}
		++m.stats.patches;
	}
}
