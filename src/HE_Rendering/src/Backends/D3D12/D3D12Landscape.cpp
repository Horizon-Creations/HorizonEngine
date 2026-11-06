#include "Backends/D3D12/D3D12Landscape.h"

#include "Backends/D3D_Shared/LandscapeD3D.h"

#include <HorizonRendering/LandscapeShaderSource.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace HE;

namespace
{
constexpr D3D12_RESOURCE_STATES kSrvState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr unsigned kRing = 4;               // upload buffers in the ring: deeper than the renderer's three frames in flight
constexpr unsigned kGlobalsSlots = 32;      // LsGlobals copies in the constant buffer: a draw takes the next one, so no two live draws share it
constexpr unsigned kSets = 8;               // descriptor tables in the heap: a draw takes the next one (its albedo descriptor is rewritten), so no two live draws share it
constexpr UINT64   kMinRingBytes = 4ull * 1024 * 1024;

UINT64 alignUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

ComPtr<ID3D12Resource> makeUploadBuffer(ID3D12Device* device, UINT64 bytes, uint8_t** mapped)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC rd{};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = bytes;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> res;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)))) return nullptr;
	void* ptr = nullptr;
	const D3D12_RANGE noRead{ 0, 0 };
	if (FAILED(res->Map(0, &noRead, &ptr))) return nullptr;
	if (mapped) *mapped = static_cast<uint8_t*>(ptr);
	return res;
}

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
} // namespace

struct D3D12Landscape::Impl
{
	struct Image
	{
		ComPtr<ID3D12Resource>             res;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};      // one slice, offset 0: the pitch and the size of what a copy needs
		UINT                               rows = 0;
		UINT64                             rowBytes = 0;
		UINT64                             total = 0;
	};
	struct Ring
	{
		ComPtr<ID3D12Resource> res;
		uint8_t*               mapped = nullptr;
		UINT64                 capacity = 0;
	};
	struct Job
	{
		unsigned    image;
		UINT        subresource;
		const void* src;
	};

	ID3D12Device*                    device = nullptr;   // not owned: the renderer's, which outlives this
	std::function<void()>            waitIdle;
	ComPtr<ID3DBlob>                 vsBlob, psBlob;
	ComPtr<ID3D12RootSignature>      rootSig;
	std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>> psos;   // one per colour format the caller draws into
	ComPtr<ID3D12DescriptorHeap>     srvHeap;
	UINT                             srvInc = 0;

	Image                            images[d3dls::kImageCount];   // tile heights, slot table, coarse heights, 3 x tile weights, 3 x coarse weights
	ComPtr<ID3D12Resource>           indices;
	D3D12_INDEX_BUFFER_VIEW          ibv{};
	ComPtr<ID3D12Resource>           globals;
	uint8_t*                         globalsMapped = nullptr;
	UINT64                           globalsStride = alignUp(sizeof(d3dls::Globals), 256);
	unsigned                         globalsNext = 0;
	unsigned                         setNext = 0;
	Ring                             ring[kRing];
	unsigned                         ringNext = 0;
	std::vector<uint16_t>            table;      // CPU mirror of the slot table: a change is one copy of the whole (small) image
	std::shared_ptr<const LandscapeRenderStatic> world;   // what the images were made for
	Stats                            stats;
	bool                             ok = false;

	void idle() { if (waitIdle) waitIdle(); }

	void destroyImages()
	{
		for (Image& im : images) im = Image{};
		indices.Reset();
		ibv = D3D12_INDEX_BUFFER_VIEW{};
		table.clear();
		world.reset();
	}

	bool makeImage(unsigned i, DXGI_FORMAT fmt, int w, int h, int layers)
	{
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = static_cast<UINT64>(std::max(w, 1));
		rd.Height = static_cast<UINT>(std::max(h, 1));
		rd.DepthOrArraySize = static_cast<UINT16>(std::max(layers, 1));
		rd.MipLevels = 1;
		rd.Format = fmt;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		Image& im = images[i];
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kSrvState, nullptr, IID_PPV_ARGS(&im.res)))) return false;
		device->GetCopyableFootprints(&rd, 0, 1, 0, &im.fp, &im.rows, &im.rowBytes, &im.total);

		D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = fmt;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		if (layers > 0)
		{
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			sv.Texture2DArray.MipLevels = 1;
			sv.Texture2DArray.ArraySize = static_cast<UINT>(layers);
		}
		else
		{
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sv.Texture2D.MipLevels = 1;
		}
		for (unsigned s = 0; s < kSets; ++s)   // the image sits at the same place of every table of the ring
			device->CreateShaderResourceView(im.res.Get(), &sv, cpuSlot(s, i));
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(unsigned set, unsigned i) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(set * d3dls::kSrvCount + i) * srvInc;
		return h;
	}

	// The borrowed albedo array at the tail of a table (null = a null view: nothing reads it where the shader's tex.y says "flat").
	void writeAlbedo(unsigned set, ID3D12Resource* res)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sv.Texture2DArray.MipLevels = 1;
		sv.Texture2DArray.ArraySize = 1;
		if (res)
		{
			const D3D12_RESOURCE_DESC d = res->GetDesc();
			sv.Format = d.Format;
			sv.Texture2DArray.MipLevels = d.MipLevels;
			sv.Texture2DArray.ArraySize = d.DepthOrArraySize;
		}
		device->CreateShaderResourceView(res, &sv, cpuSlot(set, d3dls::kImageCount));
	}

	ID3D12PipelineState* psoFor(DXGI_FORMAT fmt)
	{
		for (auto& [f, p] : psos)
			if (f == fmt) return p.Get();
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = rootSig.Get();
		pd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
		pd.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.DepthStencilState.DepthEnable = TRUE;
		pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = fmt;
		pd.SampleDesc.Count = 1;
		ComPtr<ID3D12PipelineState> pso;
		if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
		psos.emplace_back(fmt, pso);
		return psos.back().second.Get();
	}
};

D3D12Landscape::D3D12Landscape() : m_impl(std::make_unique<Impl>()) {}
D3D12Landscape::~D3D12Landscape() = default;
bool D3D12Landscape::ready() const { return m_impl->ok; }
const D3D12Landscape::Stats& D3D12Landscape::stats() const { return m_impl->stats; }

bool D3D12Landscape::init(ID3D12Device* device, std::function<void()> waitIdle, std::string* error)
{
	Impl& m = *m_impl;
	if (m.ok) return true;
	std::string err;
	if (!device) { if (error) *error = "landscape: no device"; return false; }
	m.device = device;
	m.waitIdle = std::move(waitIdle);

	if (!compileStage("VSMain", "vs_5_0", m.vsBlob, err) || !compileStage("PSMain", "ps_5_0", m.psBlob, err))
	{
		if (error) *error = err;
		return false;
	}

	// root signature: 0 = b12 (the frame's constants, a root CBV), 1 = b13 (the patch, twelve root constants), 2 = t32-t41 (the images),
	// static sampler s13 (the albedo array: linear, repeat, mips)
	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = d3dls::kSrvCount;
	range.BaseShaderRegister = d3dls::kFirstSrv;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER params[3]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = d3dls::kGlobalsReg;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[1].Constants.ShaderRegister = d3dls::kPatchReg;
	params[1].Constants.Num32BitValues = sizeof(d3dls::PatchData) / 4;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[2].DescriptorTable.NumDescriptorRanges = 1;
	params[2].DescriptorTable.pDescriptorRanges = &range;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_STATIC_SAMPLER_DESC smp{};
	smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	smp.MaxLOD = D3D12_FLOAT32_MAX;
	smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	smp.ShaderRegister = d3dls::kSamplerReg;
	smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsd{};
	rsd.NumParameters = 3;
	rsd.pParameters = params;
	rsd.NumStaticSamplers = 1;
	rsd.pStaticSamplers = &smp;
	ComPtr<ID3DBlob> rsBlob, rsErr;
	if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErr.GetAddressOf()))
	    || FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m.rootSig))))
	{
		if (error) *error = std::string("landscape: root signature: ") + (rsErr ? static_cast<const char*>(rsErr->GetBufferPointer()) : "failed");
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = d3dls::kSrvCount * kSets;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.srvHeap))))
	{
		if (error) *error = "landscape: descriptor heap";
		return false;
	}
	m.srvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	for (unsigned s = 0; s < kSets; ++s) m.writeAlbedo(s, nullptr);   // every descriptor of a table is valid from the start (resource binding tier 1)

	m.globals = makeUploadBuffer(device, m.globalsStride * kGlobalsSlots, &m.globalsMapped);
	if (!m.globals)
	{
		if (error) *error = "landscape: constant buffer";
		return false;
	}
	m.ok = true;
	return true;
}

void D3D12Landscape::shutdown()
{
	Impl& m = *m_impl;
	if (m.ok) m.idle();   // nothing the GPU still reads may go
	m.destroyImages();
	for (Impl::Ring& r : m.ring) r = Impl::Ring{};
	m.psos.clear();
	m.globals.Reset();
	m.globalsMapped = nullptr;
	m.srvHeap.Reset();
	m.rootSig.Reset();
	m.vsBlob.Reset();
	m.psBlob.Reset();
	m.device = nullptr;
	m.ok = false;
}

void D3D12Landscape::upload(ID3D12GraphicsCommandList* cl, HE::LandscapeRenderFrame& f)
{
	Impl& m = *m_impl;
	// The frame's uploads are executed once, whatever happens: the copy jobs below point INTO them, so they go when this function does.
	struct Dropper { HE::LandscapeRenderFrame& f; ~Dropper() { f.uploads.clear(); f.tableUpdates.clear(); } } dropper{ f };
	if (!m.ok || !f.active || !f.world) return;
	const LandscapeRenderStatic& w = *f.world;
	if (!d3dls::worldUsable(w)) { ++m.stats.rejected; return; }

	std::vector<Impl::Job> jobs;
	std::vector<std::vector<uint8_t>> scratch;   // zero images for pages a world does not have, alive until the copies are recorded

	// ── the images of a landscape that began (or the first frame) ─────────────
	if (m.world != f.world && (!m.world || m.world->generation != w.generation))
	{
		m.idle();   // the old images may still be read by frames in flight
		m.destroyImages();
		bool good = true;
		good &= m.makeImage(0, DXGI_FORMAT_R16_UINT, w.heightW, w.heightH, w.slotCapacity);
		for (unsigned p = 0; p < 3; ++p)   // pages the tiles do not have stay zero (a landscape of fewer than 9 channels); the shader reads all three
			good &= m.makeImage(3 + p, DXGI_FORMAT_R8G8B8A8_UNORM, w.weightW, w.weightH, w.slotCapacity);

		m.table.assign(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
		good &= m.makeImage(1, DXGI_FORMAT_R16_UINT, w.tableWindow, w.tableWindow, 0);

		const bool haveCoarse = w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH) && w.coarseW > 0 && w.coarseH > 0;
		if (!haveCoarse) ++m.stats.rejected;
		good &= m.makeImage(2, DXGI_FORMAT_R16_UINT, haveCoarse ? w.coarseW : 1, haveCoarse ? w.coarseH : 1, 0);

		const int cw = std::max(w.coarseWeightW, 1), ch = std::max(w.coarseWeightH, 1);
		for (unsigned p = 0; p < 3; ++p)
			good &= m.makeImage(6 + p, DXGI_FORMAT_R8G8B8A8_UNORM, cw, ch, 0);

		// the index buffer: the grid's triangles, read by the input assembler straight from an upload-heap buffer (written once)
		uint8_t* iptr = nullptr;
		m.indices = makeUploadBuffer(m.device, w.patchIndices.size() * sizeof(uint16_t), &iptr);
		good &= m.indices != nullptr;
		if (!good)
		{
			m.destroyImages();
			++m.stats.rejected;
			return;
		}
		std::memcpy(iptr, w.patchIndices.data(), w.patchIndices.size() * sizeof(uint16_t));
		m.ibv.BufferLocation = m.indices->GetGPUVirtualAddress();
		m.ibv.SizeInBytes = static_cast<UINT>(w.patchIndices.size() * sizeof(uint16_t));
		m.ibv.Format = DXGI_FORMAT_R16_UINT;

		jobs.push_back({ 1, 0, m.table.data() });
		if (haveCoarse) jobs.push_back({ 2, 0, w.coarseHeights.data() });
		else { scratch.emplace_back(2, 0); jobs.push_back({ 2, 0, scratch.back().data() }); }
		for (unsigned p = 0; p < 3; ++p)
		{
			const bool have = p < w.coarseWeights.size() && w.coarseWeights[p].size() == static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4;
			if (have) jobs.push_back({ 6 + p, 0, w.coarseWeights[p].data() });
			else { scratch.emplace_back(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4, 0); jobs.push_back({ 6 + p, 0, scratch.back().data() }); }
		}
		m.world = f.world;
		++m.stats.generations;
	}
	if (!m.world) return;

	// ── the frame's uploads, then its table changes (a slot's image is in before the table says so) ──
	const size_t hBytes = static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH);
	const size_t wBytes = static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4;
	for (const LandscapeRenderUpload& u : f.uploads)
	{
		if (u.slot >= w.slotCapacity || u.heights.size() != hBytes) { ++m.stats.rejected; continue; }
		jobs.push_back({ 0, u.slot, u.heights.data() });
		for (size_t p = 0; p < u.pages.size() && p < 3; ++p)
		{
			if (u.pages[p].size() != wBytes) { ++m.stats.rejected; continue; }
			jobs.push_back({ static_cast<unsigned>(3 + p), u.slot, u.pages[p].data() });
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
	if (tableDirty && std::none_of(jobs.begin(), jobs.end(), [](const Impl::Job& j) { return j.image == 1; })) jobs.push_back({ 1, 0, m.table.data() });
	if (jobs.empty()) return;

	// ── one upload buffer of the ring, big enough for every copy of this call ──
	UINT64 total = 0;
	bool touched[d3dls::kImageCount] = {};
	for (const Impl::Job& j : jobs)
	{
		total += alignUp(m.images[j.image].total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		touched[j.image] = true;
	}
	Impl::Ring& ring = m.ring[m.ringNext++ % kRing];
	if (ring.capacity < total)
	{
		if (ring.res) m.idle();   // a frame in flight may still read this buffer
		ring = Impl::Ring{};
		const UINT64 cap = std::max<UINT64>({ total, kMinRingBytes });
		ring.res = makeUploadBuffer(m.device, cap, &ring.mapped);
		if (!ring.res) { ++m.stats.rejected; return; }
		ring.capacity = cap;
	}

	D3D12_RESOURCE_BARRIER bars[d3dls::kImageCount];
	UINT nb = 0;
	for (unsigned i = 0; i < d3dls::kImageCount; ++i)
	{
		if (!touched[i]) continue;
		D3D12_RESOURCE_BARRIER& b = bars[nb++];
		b = D3D12_RESOURCE_BARRIER{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = m.images[i].res.Get();
		b.Transition.StateBefore = kSrvState;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	}
	cl->ResourceBarrier(nb, bars);

	UINT64 cursor = 0;
	for (const Impl::Job& j : jobs)
	{
		const Impl::Image& im = m.images[j.image];
		cursor = alignUp(cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		const UINT pitch = im.fp.Footprint.RowPitch;
		uint8_t* dst = ring.mapped + cursor;
		const uint8_t* src = static_cast<const uint8_t*>(j.src);
		for (UINT y = 0; y < im.rows; ++y)
			std::memcpy(dst + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * im.rowBytes, static_cast<size_t>(im.rowBytes));
		D3D12_TEXTURE_COPY_LOCATION to{};
		to.pResource = im.res.Get();
		to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		to.SubresourceIndex = j.subresource;
		D3D12_TEXTURE_COPY_LOCATION from{};
		from.pResource = ring.res.Get();
		from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		from.PlacedFootprint = im.fp;
		from.PlacedFootprint.Offset = cursor;
		cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		cursor += im.total;
	}

	for (UINT i = 0; i < nb; ++i) std::swap(bars[i].Transition.StateBefore, bars[i].Transition.StateAfter);
	cl->ResourceBarrier(nb, bars);
}

void D3D12Landscape::draw(ID3D12GraphicsCommandList* cl, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, const Shading& shading,
                          unsigned colorFormat)
{
	Impl& m = *m_impl;
	m.stats.patches = m.stats.drawCalls = 0;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation) return;
	ID3D12PipelineState* pso = m.psoFor(static_cast<DXGI_FORMAT>(colorFormat));
	if (!pso) return;
	const LandscapeRenderStatic& w = *m.world;

	d3dls::Globals g;
	d3dls::fillGlobals(g, f, viewProj, shading.sunDir, shading.sunColor, shading.ambient, shading.albedoArray != nullptr);
	const unsigned slot = m.globalsNext++ % kGlobalsSlots;
	std::memcpy(m.globalsMapped + static_cast<size_t>(slot) * m.globalsStride, &g, sizeof(g));
	const unsigned set = m.setNext++ % kSets;
	m.writeAlbedo(set, shading.albedoArray);   // a table no frame in flight is reading: the ring is deeper than the draws of the renderer's frames

	ID3D12DescriptorHeap* heaps[] = { m.srvHeap.Get() };
	cl->SetDescriptorHeaps(1, heaps);
	cl->SetGraphicsRootSignature(m.rootSig.Get());
	cl->SetPipelineState(pso);
	cl->SetGraphicsRootConstantBufferView(0, m.globals->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * m.globalsStride);
	D3D12_GPU_DESCRIPTOR_HANDLE table = m.srvHeap->GetGPUDescriptorHandleForHeapStart();
	table.ptr += static_cast<UINT64>(set) * d3dls::kSrvCount * m.srvInc;
	cl->SetGraphicsRootDescriptorTable(2, table);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->IASetIndexBuffer(&m.ibv);

	const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
	for (const LandscapeRenderPatch& p : f.patches)
	{
		d3dls::PatchData pd;
		d3dls::fillPatch(pd, p);
		cl->SetGraphicsRoot32BitConstants(1, sizeof(pd) / 4, &pd, 0);
		if ((p.quadrants & 15) == 15)
		{
			cl->DrawIndexedInstanced(total, 1, 0, 0, 0);
			++m.stats.drawCalls;
		}
		else
		{
			for (int q = 0; q < 4; ++q)
			{
				if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
				cl->DrawIndexedInstanced(w.quadrantCount[q], 1, w.quadrantFirst[q], 0, 0);
				++m.stats.drawCalls;
			}
		}
		++m.stats.patches;
	}
}
