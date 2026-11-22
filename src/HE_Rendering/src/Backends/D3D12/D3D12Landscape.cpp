#include "Backends/D3D12/D3D12Landscape.h"

#include "Backends/D3D_Shared/LandscapeD3D.h"
#include "Backends/Vulkan/VulkanLandscapeLayout.h"   // HE::vkls: the GLSL blocks a material's landscape variant reads (no Vulkan header in it)

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
constexpr unsigned kGlobalsSlots = 128;     // LsGlobals copies in the constant buffer: a draw takes the next one, so no two live draws share it (a frame has up to 21 with the shadow layers, and three frames may be in flight)
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
		D3D12_SHADER_RESOURCE_VIEW_DESC    view{};    // as makeImage wrote it into the class's own tables (writeMaterialViews writes it again)
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
	// The baked far image (LandscapeRenderStatic::farMips): its own resource, since its mip count varies with the world (every image
	// above has one level). Made once per generation, uploaded directly through the ring below (farPending marks a copy still owed to it).
	ComPtr<ID3D12Resource>           farTex;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> farFp;
	std::vector<UINT>                farRows;
	std::vector<UINT64>              farRowBytes;
	UINT64                           farTotal = 0;
	bool                             farPending = false;   // this generation made farTex but has not copied its bytes into it yet
	bool                             farPresent = false;   // farTex holds a real image (set once the copy above is recorded)
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
		farTex.Reset();
		farFp.clear(); farRows.clear(); farRowBytes.clear(); farTotal = 0; farPending = false; farPresent = false;
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
		im.view = sv;
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot(unsigned set, unsigned i) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(set * d3dls::kSrvCount + i) * srvInc;
		return h;
	}

	// A borrowed texture-group array (albedo, normal or mask) at `slot` of a table (null = a null view: nothing reads it where the shader's
	// tex.y / texMaps says "flat" / "off").
	void writeBorrowed(unsigned set, unsigned slot, ID3D12Resource* res)
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
		device->CreateShaderResourceView(res, &sv, cpuSlot(set, slot));
	}

	// The baked far image's slot (t44, kFarIndex): a plain Texture2D with its real mip chain, not an array like writeBorrowed's. Null
	// when the world has none, same as the borrowed arrays (the shader does not read it unless texMaps.z says so).
	void writeFar(unsigned set, ID3D12Resource* res)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sv.Texture2D.MipLevels = 1;
		if (res)
		{
			const D3D12_RESOURCE_DESC d = res->GetDesc();
			sv.Format = d.Format;
			sv.Texture2D.MipLevels = d.MipLevels;
		}
		device->CreateShaderResourceView(res, &sv, cpuSlot(set, d3dls::kFarIndex));
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

	// The vertex stage alone into a depth-only target (a shadow layer): no pixel shader, no render target, and the shadow pass's
	// depth bias (the renderer's depthPSO: 4 constant, 2 slope scaled). Tried once.
	ComPtr<ID3D12PipelineState> depthPso;
	bool depthPsoTried = false;
	ID3D12PipelineState* depthPsoGet()
	{
		if (depthPsoTried) return depthPso.Get();
		depthPsoTried = true;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = rootSig.Get();
		pd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.RasterizerState.DepthBias = 4;
		pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
		pd.DepthStencilState.DepthEnable = TRUE;
		pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 0;
		pd.SampleDesc.Count = 1;
		if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&depthPso)))) depthPso.Reset();
		return depthPso.Get();
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

	// root signature: 0 = b12 (the frame's constants, a root CBV), 1 = b13 (the patch, twelve root constants), 2 = t32-t44 (the images,
	// t44 the baked far image), static samplers s13 (the albedo/normal/mask arrays: linear, repeat, mips) and s14 (the far image: linear,
	// clamp-to-edge, mips — its uv does not repeat)
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
	D3D12_STATIC_SAMPLER_DESC farSmp{};   // the baked far image's own: linear + mipped, clamped (its uv does not repeat)
	farSmp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	farSmp.AddressU = farSmp.AddressV = farSmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	farSmp.MaxLOD = D3D12_FLOAT32_MAX;
	farSmp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	farSmp.ShaderRegister = d3dls::kFarSamplerReg;
	farSmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC smps[2] = { smp, farSmp };
	D3D12_ROOT_SIGNATURE_DESC rsd{};
	rsd.NumParameters = 3;
	rsd.pParameters = params;
	rsd.NumStaticSamplers = 2;
	rsd.pStaticSamplers = smps;
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
	for (unsigned s = 0; s < kSets; ++s)   // every descriptor of a table is valid from the start (resource binding tier 1)
	{
		m.writeBorrowed(s, d3dls::kImageCount, nullptr);
		m.writeBorrowed(s, d3dls::kImageCount + 1, nullptr);
		m.writeBorrowed(s, d3dls::kImageCount + 2, nullptr);
		m.writeFar(s, nullptr);
	}

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
	m.depthPso.Reset();
	m.depthPsoTried = false;
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
	// Or of a render window that moved, or far ground that was edited (same generation, another coarseRevision): the coarse
	// images and the far image again, at their new sizes; the tile arrays, the slot table and the index buffer stay as they
	// are, so the near ground is never thrown away (Windows handoff, item 2a).
	const bool fresh = !m.world || m.world->generation != w.generation;
	if (m.world != f.world && (fresh || m.world->coarseRevision != w.coarseRevision))
	{
		m.idle();   // the old images may still be read by frames in flight
		bool good = true;
		if (fresh)
		{
			m.destroyImages();
			good &= m.makeImage(0, DXGI_FORMAT_R16_UINT, w.heightW, w.heightH, w.slotCapacity);
			for (unsigned p = 0; p < 3; ++p)   // pages the tiles do not have stay zero (a landscape of fewer than 9 channels); the shader reads all three
				good &= m.makeImage(3 + p, DXGI_FORMAT_R8G8B8A8_UNORM, w.weightW, w.weightH, w.slotCapacity);

			m.table.assign(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
			good &= m.makeImage(1, DXGI_FORMAT_R16_UINT, w.tableWindow, w.tableWindow, 0);
		}
		else
		{
			m.farTex.Reset();
			m.farFp.clear(); m.farRows.clear(); m.farRowBytes.clear(); m.farTotal = 0; m.farPending = false; m.farPresent = false;
		}

		const bool haveCoarse = w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH) && w.coarseW > 0 && w.coarseH > 0;
		if (!haveCoarse) ++m.stats.rejected;
		good &= m.makeImage(2, DXGI_FORMAT_R16_UINT, haveCoarse ? w.coarseW : 1, haveCoarse ? w.coarseH : 1, 0);

		const int cw = std::max(w.coarseWeightW, 1), ch = std::max(w.coarseWeightH, 1);
		for (unsigned p = 0; p < 3; ++p)
			good &= m.makeImage(6 + p, DXGI_FORMAT_R8G8B8A8_UNORM, cw, ch, 0);

		// the index buffer: the grid's triangles, read by the input assembler straight from an upload-heap buffer (written once)
		uint8_t* iptr = nullptr;
		if (fresh)
		{
			m.indices = makeUploadBuffer(m.device, w.patchIndices.size() * sizeof(uint16_t), &iptr);
			good &= m.indices != nullptr;
		}
		if (!good)
		{
			m.destroyImages();
			++m.stats.rejected;
			return;
		}
		if (fresh)
		{
			std::memcpy(iptr, w.patchIndices.data(), w.patchIndices.size() * sizeof(uint16_t));
			m.ibv.BufferLocation = m.indices->GetGPUVirtualAddress();
			m.ibv.SizeInBytes = static_cast<UINT>(w.patchIndices.size() * sizeof(uint16_t));
			m.ibv.Format = DXGI_FORMAT_R16_UINT;

			jobs.push_back({ 1, 0, m.table.data() });
		}
		if (haveCoarse) jobs.push_back({ 2, 0, w.coarseHeights.data() });
		else { scratch.emplace_back(2, 0); jobs.push_back({ 2, 0, scratch.back().data() }); }
		for (unsigned p = 0; p < 3; ++p)
		{
			const bool have = p < w.coarseWeights.size() && w.coarseWeights[p].size() == static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4;
			if (have) jobs.push_back({ 6 + p, 0, w.coarseWeights[p].data() });
			else { scratch.emplace_back(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4, 0); jobs.push_back({ 6 + p, 0, scratch.back().data() }); }
		}

		// The baked far image: its own resource (not `good`-gated — a bad one leaves the world without it, like GL/Vulkan/D3D11, instead
		// of failing the whole landscape). Copied below, through the same ring as the jobs above, once farPending is seen.
		if (w.farW > 0 && w.farH > 0 && !w.farMips.empty())
		{
			const UINT mips = static_cast<UINT>(w.farMips.size());
			D3D12_HEAP_PROPERTIES fhp{};
			fhp.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC frd{};
			frd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			frd.Width = static_cast<UINT64>(w.farW);
			frd.Height = static_cast<UINT>(w.farH);
			frd.DepthOrArraySize = 1;
			frd.MipLevels = static_cast<UINT16>(mips);
			frd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			frd.SampleDesc.Count = 1;
			frd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			if (SUCCEEDED(m.device->CreateCommittedResource(&fhp, D3D12_HEAP_FLAG_NONE, &frd, kSrvState, nullptr, IID_PPV_ARGS(&m.farTex))))
			{
				m.farFp.resize(mips);
				m.farRows.resize(mips);
				m.farRowBytes.resize(mips);
				m.device->GetCopyableFootprints(&frd, 0, mips, 0, m.farFp.data(), m.farRows.data(), m.farRowBytes.data(), &m.farTotal);
				m.farPending = true;
				m.farPresent = true;
			}
			else { m.farTex.Reset(); ++m.stats.rejected; }
		}
		for (unsigned s = 0; s < kSets; ++s) m.writeFar(s, m.farTex.Get());

		m.world = f.world;
		if (fresh) ++m.stats.generations;
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
	// The far image's own copy, below: not one of `jobs` (its footprints are per-mip, not per-array-slot), but the same ring.
	if (m.farPending) total += alignUp(m.farTotal, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
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

	D3D12_RESOURCE_BARRIER bars[d3dls::kImageCount + 1];
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
	if (m.farPending)
	{
		D3D12_RESOURCE_BARRIER& b = bars[nb++];
		b = D3D12_RESOURCE_BARRIER{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = m.farTex.Get();
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

	if (m.farPending)
	{
		cursor = alignUp(cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		const UINT64 farBase = cursor;
		bool sizeOk = true;
		for (UINT l = 0; l < static_cast<UINT>(m.farFp.size()); ++l)
		{
			sizeOk = w.farMips[l].size() == static_cast<size_t>(m.farRowBytes[l]) * m.farRows[l];
			if (!sizeOk) break;
			uint8_t* dst = ring.mapped + farBase + m.farFp[l].Offset;
			const uint8_t* src = w.farMips[l].data();
			for (UINT y = 0; y < m.farRows[l]; ++y)
				std::memcpy(dst + static_cast<size_t>(y) * m.farFp[l].Footprint.RowPitch, src + static_cast<size_t>(y) * m.farRowBytes[l], static_cast<size_t>(m.farRowBytes[l]));
			D3D12_TEXTURE_COPY_LOCATION to{};
			to.pResource = m.farTex.Get();
			to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			to.SubresourceIndex = l;
			D3D12_TEXTURE_COPY_LOCATION from{};
			from.pResource = ring.res.Get();
			from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			from.PlacedFootprint = m.farFp[l];
			from.PlacedFootprint.Offset += farBase;
			cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		}
		if (!sizeOk)   // a mip of the wrong size: leave the world without a far image, like OpenGLLandscape::makeFar / the others
		{
			++m.stats.rejected;
			m.farPresent = false;
			for (unsigned s = 0; s < kSets; ++s) m.writeFar(s, nullptr);
		}
		m.farPending = false;
	}

	for (UINT i = 0; i < nb; ++i) std::swap(bars[i].Transition.StateBefore, bars[i].Transition.StateAfter);
	cl->ResourceBarrier(nb, bars);
}

void D3D12Landscape::drawDepth(ID3D12GraphicsCommandList* cl, const HE::LandscapeRenderFrame& f, const glm::mat4& clip)
{
	Impl& m = *m_impl;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation) return;
	ID3D12PipelineState* pso = m.depthPsoGet();
	if (!pso) return;
	const LandscapeRenderStatic& w = *m.world;

	d3dls::Globals g;
	d3dls::fillGlobals(g, f, clip, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), glm::vec3(0.0f), false, false, false, false);
	const unsigned slot = m.globalsNext++ % kGlobalsSlots;
	std::memcpy(m.globalsMapped + static_cast<size_t>(slot) * m.globalsStride, &g, sizeof(g));
	// The table the last colour draw used, as it is: the vertex stage reads the landscape's own images, which sit at the same
	// place of every table; the borrowed arrays behind them are not read here and must not be rewritten under a frame in flight.
	const unsigned set = (m.setNext + kSets - 1) % kSets;

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
		if ((p.quadrants & 15) == 15) cl->DrawIndexedInstanced(total, 1, 0, 0, 0);
		else
			for (int q = 0; q < 4; ++q)
				if ((p.quadrants & (1 << q)) && w.quadrantCount[q] != 0) cl->DrawIndexedInstanced(w.quadrantCount[q], 1, w.quadrantFirst[q], 0, 0);
	}
}

bool D3D12Landscape::writeMaterialViews(size_t cpuFirst, unsigned increment) const
{
	const Impl& m = *m_impl;
	if (!m.ok || !m.world) return false;
	for (unsigned i = 0; i < d3dls::kImageCount; ++i)
	{
		if (!m.images[i].res) return false;
		D3D12_CPU_DESCRIPTOR_HANDLE h{};
		h.ptr = cpuFirst + static_cast<SIZE_T>(i) * increment;
		m.device->CreateShaderResourceView(m.images[i].res.Get(), &m.images[i].view, h);
	}
	return true;
}

bool D3D12Landscape::drawPatches(ID3D12GraphicsCommandList* cl, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, unsigned rootGlobals,
                                 unsigned rootPatch)
{
	Impl& m = *m_impl;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation) return false;
	const LandscapeRenderStatic& w = *m.world;

	// A slot of the same ring the fixed blend's globals use: the GLSL block is no larger than a slot.
	static_assert(sizeof(vkls::Globals) <= 1024 && sizeof(d3dls::Globals) <= 1024, "LsGlobals of either layout has to fit a 1024-byte ring slot");
	if (m.globalsStride < sizeof(vkls::Globals)) return false;
	vkls::Globals g;
	vkls::fillGlobals(g, f, viewProj, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), glm::vec3(0.0f), false, false, false, false);   // the material lights, not these
	const unsigned slot = m.globalsNext++ % kGlobalsSlots;
	std::memcpy(m.globalsMapped + static_cast<size_t>(slot) * m.globalsStride, &g, sizeof(g));
	cl->SetGraphicsRootConstantBufferView(rootGlobals, m.globals->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * m.globalsStride);
	cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cl->IASetIndexBuffer(&m.ibv);

	m.stats.patches = m.stats.drawCalls = 0;
	const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
	for (const LandscapeRenderPatch& p : f.patches)
	{
		vkls::PatchPush pd;
		vkls::fillPatch(pd, p);
		cl->SetGraphicsRoot32BitConstants(rootPatch, sizeof(pd) / 4, &pd, 0);
		if ((p.quadrants & 15) == 15)
		{
			cl->DrawIndexedInstanced(total, 1, 0, 0, 0);
			++m.stats.drawCalls;
		}
		else
			for (int q = 0; q < 4; ++q)
			{
				if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
				cl->DrawIndexedInstanced(w.quadrantCount[q], 1, w.quadrantFirst[q], 0, 0);
				++m.stats.drawCalls;
			}
		++m.stats.patches;
	}
	return true;
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
	d3dls::fillGlobals(g, f, viewProj, shading.sunDir, shading.sunColor, shading.ambient, shading.albedoArray != nullptr,
	                   shading.normalArray != nullptr, shading.maskArray != nullptr, m.farPresent);
	const unsigned slot = m.globalsNext++ % kGlobalsSlots;
	std::memcpy(m.globalsMapped + static_cast<size_t>(slot) * m.globalsStride, &g, sizeof(g));
	const unsigned set = m.setNext++ % kSets;
	// a table no frame in flight is reading: the ring is deeper than the draws of the renderer's frames
	m.writeBorrowed(set, d3dls::kImageCount, shading.albedoArray);
	m.writeBorrowed(set, d3dls::kImageCount + 1, shading.normalArray);
	m.writeBorrowed(set, d3dls::kImageCount + 2, shading.maskArray);

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
