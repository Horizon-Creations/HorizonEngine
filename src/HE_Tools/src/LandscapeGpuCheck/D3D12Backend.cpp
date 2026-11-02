#include "GpuCheck.h"

#include <Backends/D3D12/D3D12Landscape.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gpucheck
{
namespace
{
// D3D12 offscreen: RGBA8 colour + a 32-bit float depth buffer in committed resources, one direct queue, every render() submitted and waited for, the results
// read back through buffers. Like D3D11, the scene pass of D3D12Renderer rasterises the plain GL view-projection (kD3DClipFix is NOT applied), so the
// depth buffer holds the GL ndc z of the half of the frustum that is not clipped (depthIsNdc); the landscape takes the same matrix.
// The debug layer is on when the machine has it: an error it reports here is the same error the renderer would have had.
class D3D12Backend final : public Backend
{
public:
	~D3D12Backend() override
	{
		if (m_queue && m_fence) waitIdle();
		m_landscape.shutdown();
		if (m_event) CloseHandle(m_event);
	}
	const char* name() const override { return "D3D12"; }
	bool depthIsNdc() const override { return true; }

	bool init(std::string& error) override
	{
		ComPtr<ID3D12Debug> dbg;
		const bool layer = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)));
		if (layer) dbg->EnableDebugLayer();
		std::printf("  D3D12 debug layer: %s\n", layer ? "on" : "not installed");
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) { error = "D3D12CreateDevice failed"; return false; }
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))
		    || FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc)))
		    || FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), nullptr, IID_PPV_ARGS(&m_list)))
		    || FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
		{
			error = "D3D12 queue, list or fence could not be created";
			return false;
		}
		m_list->Close();
		m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.NumDescriptors = 1;
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		bool ok = SUCCEEDED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		ok &= SUCCEEDED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_dsvHeap)));
		if (!ok) { error = "D3D12 descriptor heaps"; return false; }
		return m_landscape.init(m_device.Get(), [this]() { waitIdle(); }, &error);
	}

	bool render(const RenderIn& in, RenderOut& out) override
	{
		if ((in.width != m_w || in.height != m_h) && !makeTarget(in.width, in.height)) { out.error = "target"; return false; }
		if (!m_color) { out.error = "target"; return false; }

		// the landscape's own waits (a landscape began, the upload ring grew) need an idle queue and no open list: the list is opened after them
		m_alloc->Reset();
		m_list->Reset(m_alloc.Get(), nullptr);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(m_w), static_cast<float>(m_h), 0.0f, 1.0f };
		const D3D12_RECT sc{ 0, 0, m_w, m_h };
		m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		m_list->RSSetViewports(1, &vp);
		m_list->RSSetScissorRects(1, &sc);
		const float clear[4] = { in.clear.r, in.clear.g, in.clear.b, in.clear.a };
		m_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
		m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		HE::LandscapeRenderFrame frame;
		in.provider(glm::dmat4(in.viewProj), false, frame);
		m_landscape.upload(m_list.Get(), frame);
		D3D12Landscape::Shading sh;
		sh.sunDir = in.sunDir;
		sh.sunColor = in.sunColor;
		sh.ambient = in.ambient;
		m_landscape.draw(m_list.Get(), frame, in.viewProj, sh, DXGI_FORMAT_R8G8B8A8_UNORM);

		D3D12_RESOURCE_BARRIER b[2]{};
		b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b[0].Transition.pResource = m_color.Get();
		b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b[1].Transition.pResource = m_depth.Get();
		b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		m_list->ResourceBarrier(2, b);
		copyOut(m_color.Get(), m_colorFp, m_colorBuf.Get());
		copyOut(m_depth.Get(), m_depthFp, m_depthBuf.Get());
		std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
		std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
		m_list->ResourceBarrier(2, b);
		m_list->Close();
		ID3D12CommandList* lists[] = { m_list.Get() };
		m_queue->ExecuteCommandLists(1, lists);
		waitIdle();
		if (const int errors = drainDebugLayer(); errors > 0) { out.error = "the D3D12 debug layer reported " + std::to_string(errors) + " error(s)"; return false; }

		out.width = m_w;
		out.height = m_h;
		out.rgba.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4, 0);
		out.depth.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h), 1.0f);
		uint8_t* pc = nullptr;
		uint8_t* pd = nullptr;
		if (FAILED(m_colorBuf->Map(0, nullptr, reinterpret_cast<void**>(&pc))) || FAILED(m_depthBuf->Map(0, nullptr, reinterpret_cast<void**>(&pd)))) { out.error = "map"; return false; }
		for (int y = 0; y < m_h; ++y)
		{
			std::memcpy(&out.rgba[static_cast<size_t>(y) * static_cast<size_t>(m_w) * 4], pc + static_cast<size_t>(y) * m_colorFp.Footprint.RowPitch, static_cast<size_t>(m_w) * 4);
			std::memcpy(&out.depth[static_cast<size_t>(y) * static_cast<size_t>(m_w)], pd + static_cast<size_t>(y) * m_depthFp.Footprint.RowPitch, static_cast<size_t>(m_w) * 4);
		}
		const D3D12_RANGE none{ 0, 0 };
		m_colorBuf->Unmap(0, &none);
		m_depthBuf->Unmap(0, &none);

		const auto& st = m_landscape.stats();
		out.uploads = st.uploads;
		out.patches = st.patches;
		out.drawCalls = st.drawCalls;
		out.rejected = st.rejected;
		return true;
	}

private:
	// Prints what the debug layer says (errors and warnings) and returns the number of errors; 0 and silent when the layer is not installed.
	int drainDebugLayer()
	{
		if (!m_info) m_device.As(&m_info);
		if (!m_info) return 0;
		int errors = 0;
		const UINT64 n = m_info->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T len = 0;
			if (FAILED(m_info->GetMessage(i, nullptr, &len)) || len == 0) continue;
			std::vector<char> buf(len);
			auto* dm = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
			if (FAILED(m_info->GetMessage(i, dm, &len)) || !dm->pDescription) continue;
			const bool err = dm->Severity == D3D12_MESSAGE_SEVERITY_ERROR || dm->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
			errors += err ? 1 : 0;
			if (err || dm->Severity == D3D12_MESSAGE_SEVERITY_WARNING) std::printf("  D3D12 debug layer %s: %s\n", err ? "ERROR" : "warning", dm->pDescription);
		}
		m_info->ClearStoredMessages();
		return errors;
	}

	void waitIdle()
	{
		const UINT64 v = ++m_fenceValue;
		m_queue->Signal(m_fence.Get(), v);
		if (m_fence->GetCompletedValue() < v)
		{
			m_fence->SetEventOnCompletion(v, m_event);
			WaitForSingleObject(m_event, INFINITE);
		}
	}

	ComPtr<ID3D12Resource> makeReadback(UINT64 bytes)
	{
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = bytes;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> r;
		m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
		return r;
	}

	void copyOut(ID3D12Resource* tex, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, ID3D12Resource* buf)
	{
		D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
		src.pResource = tex;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.pResource = buf;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = fp;
		m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	bool makeTarget(int w, int h)
	{
		waitIdle();
		m_color.Reset(); m_depth.Reset(); m_colorBuf.Reset(); m_depthBuf.Reset();
		m_w = w;
		m_h = h;
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = static_cast<UINT64>(w);
		rd.Height = static_cast<UINT>(h);
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE cv{};
		cv.Format = rd.Format;
		cv.Color[0] = 0.5f; cv.Color[1] = 0.7f; cv.Color[2] = 1.0f; cv.Color[3] = 1.0f;   // RenderIn::clear's default: a clear to anything else warns
		bool ok = SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&m_color)));
		UINT rows = 0;
		UINT64 rowBytes = 0, total = 0;
		m_device->GetCopyableFootprints(&rd, 0, 1, 0, &m_colorFp, &rows, &rowBytes, &total);
		m_colorBuf = makeReadback(total);
		m_device->CreateRenderTargetView(m_color.Get(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		rd.Format = DXGI_FORMAT_D32_FLOAT;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		cv.Format = rd.Format;
		cv.DepthStencil.Depth = 1.0f;
		ok &= SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_depth)));
		m_device->GetCopyableFootprints(&rd, 0, 1, 0, &m_depthFp, &rows, &rowBytes, &total);
		m_depthBuf = makeReadback(total);
		m_device->CreateDepthStencilView(m_depth.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
		return ok && m_colorBuf && m_depthBuf;
	}

	ComPtr<ID3D12Device>              m_device;
	ComPtr<ID3D12InfoQueue>           m_info;
	ComPtr<ID3D12CommandQueue>        m_queue;
	ComPtr<ID3D12CommandAllocator>    m_alloc;
	ComPtr<ID3D12GraphicsCommandList> m_list;
	ComPtr<ID3D12Fence>               m_fence;
	HANDLE                            m_event = nullptr;
	UINT64                            m_fenceValue = 0;
	ComPtr<ID3D12DescriptorHeap>      m_rtvHeap, m_dsvHeap;
	ComPtr<ID3D12Resource>            m_color, m_depth, m_colorBuf, m_depthBuf;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_colorFp{}, m_depthFp{};
	int m_w = 0, m_h = 0;
	D3D12Landscape m_landscape;
};
} // namespace

std::unique_ptr<Backend> makeD3D12Backend() { return std::make_unique<D3D12Backend>(); }
} // namespace gpucheck
