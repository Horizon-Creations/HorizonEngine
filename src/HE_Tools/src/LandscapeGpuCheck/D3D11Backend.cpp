#include "GpuCheck.h"

#include <Backends/D3D11/D3D11Landscape.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gpucheck
{
namespace
{
// D3D11 offscreen: RGBA8 colour + a 32-bit float depth buffer, read back through staging copies. The scene pass of D3D11Renderer rasterises the plain
// GL view-projection (kD3DClipFix is NOT applied), so what lands in the depth buffer is the GL ndc z of the half of the frustum that is not clipped
// (depthIsNdc, like Metal); the landscape takes the same matrix.
class D3D11Backend final : public Backend
{
public:
	~D3D11Backend() override
	{
		m_landscape.shutdown();
	}
	const char* name() const override { return "D3D11"; }
	bool depthIsNdc() const override { return true; }

	bool init(std::string& error) override
	{
		const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL got{};
		const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, m_device.GetAddressOf(), &got,
		                                     m_ctx.GetAddressOf());
		if (FAILED(hr)) { error = "D3D11CreateDevice failed"; return false; }
		return m_landscape.init(m_device.Get(), &error);
	}

	bool render(const RenderIn& in, RenderOut& out) override
	{
		if ((in.width != m_w || in.height != m_h) && !makeTarget(in.width, in.height)) { out.error = "target"; return false; }
		if (!m_rtv) { out.error = "target"; return false; }
		D3D11_VIEWPORT vp{};
		vp.Width = static_cast<float>(m_w);
		vp.Height = static_cast<float>(m_h);
		vp.MaxDepth = 1.0f;
		ID3D11RenderTargetView* rtv = m_rtv.Get();
		m_ctx->OMSetRenderTargets(1, &rtv, m_dsv.Get());
		m_ctx->RSSetViewports(1, &vp);
		const float clear[4] = { in.clear.r, in.clear.g, in.clear.b, in.clear.a };
		m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);
		m_ctx->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

		HE::LandscapeRenderFrame frame;
		in.provider(glm::dmat4(in.viewProj), false, frame);
		m_landscape.upload(m_ctx.Get(), frame);
		D3D11Landscape::Shading sh;
		sh.sunDir = in.sunDir;
		sh.sunColor = in.sunColor;
		sh.ambient = in.ambient;
		// The world-UV ground's albedo/normal/mask arrays, RGBA8, no mips, like the other backends'.
		ComPtr<ID3D11Texture2D> albedoTex, normalTex, maskTex;
		ComPtr<ID3D11ShaderResourceView> albedoSrv, normalSrv, maskSrv;
		const auto makeArray = [&](const TexArray* t, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv) {
			if (!t || t->size <= 0 || t->layers <= 0) return;
			D3D11_TEXTURE2D_DESC d{};
			d.Width = d.Height = static_cast<UINT>(t->size);
			d.MipLevels = 1;
			d.ArraySize = static_cast<UINT>(t->layers);
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			d.Usage = D3D11_USAGE_IMMUTABLE;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			std::vector<D3D11_SUBRESOURCE_DATA> init(static_cast<size_t>(t->layers));
			const size_t slice = static_cast<size_t>(t->size) * static_cast<size_t>(t->size) * 4;
			for (size_t i = 0; i < init.size(); ++i)
			{
				init[i].pSysMem = t->rgba.data() + i * slice;
				init[i].SysMemPitch = static_cast<UINT>(t->size) * 4;
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC v{};
			v.Format = d.Format;
			v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			v.Texture2DArray.MipLevels = 1;
			v.Texture2DArray.ArraySize = d.ArraySize;
			if (SUCCEEDED(m_device->CreateTexture2D(&d, init.data(), tex.GetAddressOf()))) m_device->CreateShaderResourceView(tex.Get(), &v, srv.GetAddressOf());
		};
		makeArray(in.albedo, albedoTex, albedoSrv);
		makeArray(in.normal, normalTex, normalSrv);
		makeArray(in.mask, maskTex, maskSrv);
		sh.albedoArray = albedoSrv.Get();
		sh.normalArray = normalSrv.Get();
		sh.maskArray = maskSrv.Get();
		m_landscape.draw(m_ctx.Get(), frame, in.viewProj, sh);

		m_ctx->CopyResource(m_colorStaging.Get(), m_color.Get());
		m_ctx->CopyResource(m_depthStaging.Get(), m_depth.Get());
		out.width = m_w;
		out.height = m_h;
		out.rgba.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4, 0);
		out.depth.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h), 1.0f);
		D3D11_MAPPED_SUBRESOURCE mc{}, md{};
		if (FAILED(m_ctx->Map(m_colorStaging.Get(), 0, D3D11_MAP_READ, 0, &mc))) { out.error = "map colour"; return false; }
		for (int y = 0; y < m_h; ++y)
			std::memcpy(&out.rgba[static_cast<size_t>(y) * static_cast<size_t>(m_w) * 4], static_cast<const uint8_t*>(mc.pData) + static_cast<size_t>(y) * mc.RowPitch,
			            static_cast<size_t>(m_w) * 4);
		m_ctx->Unmap(m_colorStaging.Get(), 0);
		if (FAILED(m_ctx->Map(m_depthStaging.Get(), 0, D3D11_MAP_READ, 0, &md))) { out.error = "map depth"; return false; }
		for (int y = 0; y < m_h; ++y)
			std::memcpy(&out.depth[static_cast<size_t>(y) * static_cast<size_t>(m_w)], static_cast<const uint8_t*>(md.pData) + static_cast<size_t>(y) * md.RowPitch,
			            static_cast<size_t>(m_w) * 4);
		m_ctx->Unmap(m_depthStaging.Get(), 0);

		const auto& st = m_landscape.stats();
		out.uploads = st.uploads;
		out.patches = st.patches;
		out.drawCalls = st.drawCalls;
		out.rejected = st.rejected;
		return true;
	}

private:
	bool makeTarget(int w, int h)
	{
		m_rtv.Reset(); m_dsv.Reset(); m_color.Reset(); m_depth.Reset(); m_colorStaging.Reset(); m_depthStaging.Reset();
		m_w = w;
		m_h = h;
		D3D11_TEXTURE2D_DESC d{};
		d.Width = static_cast<UINT>(w);
		d.Height = static_cast<UINT>(h);
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.SampleDesc.Count = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.BindFlags = D3D11_BIND_RENDER_TARGET;
		bool ok = SUCCEEDED(m_device->CreateTexture2D(&d, nullptr, m_color.GetAddressOf())) && SUCCEEDED(m_device->CreateRenderTargetView(m_color.Get(), nullptr, m_rtv.GetAddressOf()));
		D3D11_TEXTURE2D_DESC s = d;
		s.Usage = D3D11_USAGE_STAGING;
		s.BindFlags = 0;
		s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ok &= SUCCEEDED(m_device->CreateTexture2D(&s, nullptr, m_colorStaging.GetAddressOf()));
		d.Format = DXGI_FORMAT_R32_TYPELESS;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
		dv.Format = DXGI_FORMAT_D32_FLOAT;
		dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		ok &= SUCCEEDED(m_device->CreateTexture2D(&d, nullptr, m_depth.GetAddressOf())) && SUCCEEDED(m_device->CreateDepthStencilView(m_depth.Get(), &dv, m_dsv.GetAddressOf()));
		s.Format = DXGI_FORMAT_R32_TYPELESS;
		ok &= SUCCEEDED(m_device->CreateTexture2D(&s, nullptr, m_depthStaging.GetAddressOf()));
		return ok;
	}

	ComPtr<ID3D11Device>           m_device;
	ComPtr<ID3D11DeviceContext>    m_ctx;
	ComPtr<ID3D11Texture2D>        m_color, m_depth, m_colorStaging, m_depthStaging;
	ComPtr<ID3D11RenderTargetView> m_rtv;
	ComPtr<ID3D11DepthStencilView> m_dsv;
	int m_w = 0, m_h = 0;
	D3D11Landscape m_landscape;
};
} // namespace

std::unique_ptr<Backend> makeD3D11Backend() { return std::make_unique<D3D11Backend>(); }
} // namespace gpucheck
