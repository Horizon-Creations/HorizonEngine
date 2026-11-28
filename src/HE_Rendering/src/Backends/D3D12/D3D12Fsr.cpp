#include "Backends/D3D12/D3D12Fsr.h"

#include <Diagnostics/Logger.h>

#ifdef HE_HAVE_FSR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
#ifdef HE_HAVE_FSR_FG
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#endif
#include <dxgi1_6.h>
#endif

#include <cstdlib>
#include <cstring>

namespace HE
{
#ifdef HE_HAVE_FSR
namespace
{
ffxFunctions& fns(void* p) { return *static_cast<ffxFunctions*>(p); }

// The runtime's own complaints (wrong state, format, jitter...), only with HE_FSR_DEBUG=1: it also turns the runtime's
// API checks on (FFX_UPSCALE_ENABLE_DEBUG_CHECKING).
bool debugOn() { static const bool on = [] { const char* e = std::getenv("HE_FSR_DEBUG"); return e && *e == '1'; }(); return on; }
void onMessage(uint32_t type, const wchar_t* message)
{
    char buf[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) HE_LOG_ERROR(RHI, "FSR: %s", buf);
    else                                    HE_LOG_WARN(RHI, "FSR: %s", buf);
}
} // namespace
#endif

D3D12Fsr::~D3D12Fsr()
{
    destroy();
#ifdef HE_HAVE_FSR
    delete static_cast<ffxFunctions*>(m_fns);
    if (m_module) FreeLibrary(static_cast<HMODULE>(m_module));
#endif
}

bool D3D12Fsr::load()
{
    if (m_tried) return m_loaded;
    m_tried = true;
#ifdef HE_HAVE_FSR
    // By absolute path beside the module this code is in (the editor: beside the exe; the game: beside
    // HorizonEngine.dll in its HorizonEngine/ folder), not through the DLL search order.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fns), &self);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(self, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_loader_dx12.dll";
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod)
    {
        HE_LOG_INFO(RHI, "%s", "D3D12Renderer: FSR unavailable (amd_fidelityfx_loader_dx12.dll not beside the executable)");
        return false;
    }
    auto* f = new ffxFunctions{};
    ffxLoadFunctions(f, mod);
    if (!f->CreateContext || !f->DestroyContext || !f->Configure || !f->Query || !f->Dispatch)
    {
        HE_LOG_WARN(RHI, "%s", "D3D12Renderer: FSR unavailable (the loader DLL lacks the FidelityFX API entry points)");
        delete f;
        FreeLibrary(mod);
        return false;
    }
    m_module = mod;
    m_fns = f;
    m_loaded = true;
    return true;
#else
    return false;
#endif
}

bool D3D12Fsr::ensureContext(ID3D12Device* device, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH)
{
#ifdef HE_HAVE_FSR
    if (!m_loaded || !device || !renderW || !renderH || !displayW || !displayH) return false;
    if (m_context && m_rw == renderW && m_rh == renderH && m_dw == displayW && m_dh == displayH) return true;
    destroy();
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &backend.header;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &ver.header;
    // Linear HDR in, no exposure of our own (the tonemap's is a fixed 1): FSR measures it. Depth 0..1, not inverted.
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
               | (debugOn() ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u);
    desc.maxRenderSize  = { renderW, renderH };
    desc.maxUpscaleSize = { displayW, displayH };
    desc.fpMessage = debugOn() ? &onMessage : nullptr;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx)
    {
        HE_LOG_WARN(RHI, "D3D12Renderer: FSR context creation failed (%u) at %ux%u -> %ux%u", rc, renderW, renderH, displayW, displayH);
        return false;
    }
    m_context = ctx;
    m_rw = renderW; m_rh = renderH; m_dw = displayW; m_dh = displayH;
    if (m_version.empty())
    {
        // Which upscaler the loader picked (3.1.x on any GPU, 4.x on RDNA 4): its first, default entry.
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = device;
        q.outputCount = &count;
        fns(m_fns).Query(nullptr, &q.header);
        if (count > 0)
        {
            const char* names[16] = {};
            uint64_t ids[16] = {};
            count = count > 16 ? 16 : count;
            q.versionNames = names; q.versionIds = ids;
            fns(m_fns).Query(nullptr, &q.header);
            m_version = names[0] ? names[0] : "?";
        }
        HE_LOG_INFO(RHI, "D3D12Renderer: FSR %s active (%ux%u -> %ux%u)", m_version.c_str(), renderW, renderH, displayW, displayH);
    }
    return true;
#else
    (void)device; (void)renderW; (void)renderH; (void)displayW; (void)displayH;
    return false;
#endif
}

glm::vec2 D3D12Fsr::jitter(uint32_t index, uint32_t renderW, uint32_t displayW)
{
#ifdef HE_HAVE_FSR
    if (!m_context) return glm::vec2(0.0f);
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount pc{};
    pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pc.renderWidth = renderW; pc.displayWidth = displayW; pc.pOutPhaseCount = &phases;
    if (fns(m_fns).Query(reinterpret_cast<ffxContext*>(&m_context), &pc.header) != FFX_API_RETURN_OK || phases <= 0) return glm::vec2(0.0f);
    glm::vec2 j(0.0f);
    ffxQueryDescUpscaleGetJitterOffset jo{};
    jo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jo.index = static_cast<int32_t>(index % static_cast<uint32_t>(phases)); jo.phaseCount = phases;
    jo.pOutX = &j.x; jo.pOutY = &j.y;
    fns(m_fns).Query(reinterpret_cast<ffxContext*>(&m_context), &jo.header);
    return j;
#else
    (void)index; (void)renderW; (void)displayW;
    return glm::vec2(0.0f);
#endif
}

bool D3D12Fsr::dispatch(const Dispatch& d)
{
#ifdef HE_HAVE_FSR
    if (!m_context || !d.cl || !d.color || !d.depth || !d.velocity || !d.output) return false;
    const uint32_t readState = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale u{};
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = d.cl;
    u.color         = ffxApiGetResourceDX12(d.color, readState);
    u.depth         = ffxApiGetResourceDX12(d.depth, readState);
    u.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;   // the D32 depth is R32_TYPELESS: read as float
    u.motionVectors = ffxApiGetResourceDX12(d.velocity, readState);
    u.output        = ffxApiGetResourceDX12(d.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    u.jitterOffset  = { d.jitter.x, d.jitter.y };
    // The velocity is uvNow - uvPrev in uv (y down): FSR wants the way back, in pixels.
    // Render pixels, not display ones: the velocity was written at the render size.
    u.motionVectorScale = { -static_cast<float>(d.renderW), -static_cast<float>(d.renderH) };
    u.renderSize  = { d.renderW, d.renderH };
    u.upscaleSize = { d.displayW, d.displayH };
    u.enableSharpening = d.sharpness > 0.0f;
    u.sharpness        = d.sharpness;
    u.frameTimeDelta   = d.frameMs;
    u.preExposure      = 1.0f;
    u.reset            = d.reset;
    u.cameraNear       = d.nearZ;
    u.cameraFar        = d.farZ;
    u.cameraFovAngleVertical  = d.fovY;
    u.viewSpaceToMetersFactor = 1.0f;
    if (d.reactive) u.reactive = ffxApiGetResourceDX12(d.reactive, readState);
    // HE_FSR_DEBUGVIEW=1: AMD's debug view in the output (reactive mask, motion vectors, ... in a grid).
    static const bool s_view = [] { const char* e = std::getenv("HE_FSR_DEBUGVIEW"); return e && *e == '1'; }();
    if (s_view) u.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    return fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_context), &u.header) == FFX_API_RETURN_OK;
#else
    (void)d;
    return false;
#endif
}

bool D3D12Fsr::generateReactive(ID3D12GraphicsCommandList* cl, ID3D12Resource* opaque, ID3D12Resource* color,
                                ID3D12Resource* reactive, uint32_t renderW, uint32_t renderH)
{
#ifdef HE_HAVE_FSR
    if (!m_context || !cl || !opaque || !color || !reactive) return false;
    ffxDispatchDescUpscaleGenerateReactiveMask g{};
    g.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
    g.commandList = cl;
    g.colorOpaqueOnly = ffxApiGetResourceDX12(opaque, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    g.colorPreUpscale = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    g.outReactive     = ffxApiGetResourceDX12(reactive, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    g.renderSize = { renderW, renderH };
    // AMD sample's values (fsrapirendermodule.cpp, "hard-coded in the original FSR2 sample").
    g.scale = 1.0f;
    g.cutoffThreshold = 0.2f;
    g.binaryValue = 0.9f;
    g.flags = FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_TONEMAP | FFX_UPSCALE_AUTOREACTIVEFLAGS_APPLY_THRESHOLD
            | FFX_UPSCALE_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;
    return fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_context), &g.header) == FFX_API_RETURN_OK;
#else
    (void)cl; (void)opaque; (void)color; (void)reactive; (void)renderW; (void)renderH;
    return false;
#endif
}

void D3D12Fsr::destroy()
{
#ifdef HE_HAVE_FSR
    if (m_context && m_fns) fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_context), nullptr);
#endif
    m_context = nullptr;
    m_rw = m_rh = m_dw = m_dh = 0;
}

// ── Frame generation ─────────────────────────────────────────────────────────
bool D3D12Fsr::frameGenAvailable(ID3D12Device* device)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    if (m_fgAvail >= 0) return m_fgAvail == 1;
    if (!m_loaded || !device) return false;
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = device;
    q.outputCount = &count;
    fns(m_fns).Query(nullptr, &q.header);
    m_fgAvail = count > 0 ? 1 : 0;
    if (!m_fgAvail)
        HE_LOG_INFO(RHI, "%s", "D3D12Renderer: FSR frame generation unavailable (no provider: amd_fidelityfx_framegeneration_dx12.dll)");
    return m_fgAvail == 1;
#else
    (void)device;
    return false;
#endif
}

bool D3D12Fsr::replaceSwapchain(Microsoft::WRL::ComPtr<IDXGISwapChain3>& sc, ID3D12CommandQueue* queue)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    using Microsoft::WRL::ComPtr;
    if (m_scContext) return true;
    if (!m_loaded || !sc || !queue) return false;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d1{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    // A native factory of its own, not sc's parent: with NVIDIA's Streamline loaded, sc and its parent are Streamline's
    // proxies, and FSR's swapchain must be made natively (Streamline's proxy would otherwise reach native DXGI calls).
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(sc->GetHwnd(&hwnd)) || FAILED(sc->GetDesc1(&d1)) || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return false;
    const bool fullscreen = SUCCEEDED(sc->GetFullscreenDesc(&fs)) && !fs.Windowed;
    sc.Reset();   // one swapchain per window: the old one has to be gone before the proxy is made for it

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    IDXGISwapChain4* proxy = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    desc.header.pNext = &ver.header;
    desc.swapchain = &proxy;
    desc.hwnd = hwnd;
    desc.desc = &d1;
    desc.fullscreenDesc = fullscreen ? &fs : nullptr;
    desc.dxgiFactory = factory.Get();
    desc.gameQueue = queue;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    if (rc == FFX_API_RETURN_OK && ctx && proxy)
    {
        m_scContext = ctx;
        sc.Attach(proxy);   // the reference the context handed out is ours now
        HE_LOG_INFO(RHI, "D3D12Renderer: FSR frame interpolation swapchain in place (%ux%u)", d1.Width, d1.Height);
        return true;
    }
    if (ctx) fns(m_fns).DestroyContext(&ctx, nullptr);
    if (proxy) proxy->Release();
    // Back to a plain swapchain for the same window: the game must keep presenting.
    ComPtr<IDXGISwapChain1> s1;
    if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &d1, fullscreen ? &fs : nullptr, nullptr, &s1)))
        s1.As(&sc);
    HE_LOG_WARN(RHI, "D3D12Renderer: FSR frame interpolation swapchain failed (%u) — frame generation off", rc);
    return false;
#else
    (void)sc; (void)queue;
    return false;
#endif
}

bool D3D12Fsr::ensureFrameGen(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBuffer)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    if (!m_scContext || !device || !displayW || !displayH) return false;
    if (m_fgContext && m_fgW == displayW && m_fgH == displayH) return true;
    if (m_fgContext) { fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_fgContext), nullptr); m_fgContext = nullptr; }
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxCreateContextDescFrameGenerationHudless hud{};
    hud.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hud.header.pNext = &backend.header;
    hud.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(backBuffer);
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &hud.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;   // PrepareV2 works only with this chained
    ffxCreateContextDescFrameGeneration desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = &ver.header;
    // The back buffer it interpolates is the tonemapped RGBA8 image: no HDR flag. Depth 0..1, not inverted, finite.
    desc.flags = debugOn() ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
    desc.displaySize   = { displayW, displayH };
    desc.maxRenderSize = { displayW, displayH };
    desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(backBuffer);
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx)
    {
        HE_LOG_WARN(RHI, "D3D12Renderer: FSR frame generation context failed (%u) at %ux%u", rc, displayW, displayH);
        return false;
    }
    m_fgContext = ctx;
    m_fgW = displayW; m_fgH = displayH;
    HE_LOG_INFO(RHI, "D3D12Renderer: FSR frame generation ready (%ux%u)", displayW, displayH);
    return true;
#else
    (void)device; (void)displayW; (void)displayH; (void)backBuffer;
    return false;
#endif
}

bool D3D12Fsr::frameGen(const FrameGen& f)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    if (!m_fgContext || !f.swapchain) return false;
    // HE_FSR_FG_DEBUG=1: AMD's tear lines on the generated frames' edges (a visible witness that frames are generated).
    static const bool s_lines = [] { const char* e = std::getenv("HE_FSR_FG_DEBUG"); return e && *e == '1'; }();
    const uint32_t flags = s_lines ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES) : 0u;
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = f.swapchain;
    // AMD's recommended default: the proxy dispatches the generation itself, inside Present.
    cfg.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* user) -> ffxReturnCode_t
    {
        auto* self = static_cast<D3D12Fsr*>(user);
        const ffxReturnCode_t rc = fns(self->m_fns).Dispatch(reinterpret_cast<ffxContext*>(&self->m_fgContext), &params->header);
        if (rc == FFX_API_RETURN_OK) self->m_fgGenerated.fetch_add(params->numGeneratedFrames, std::memory_order_relaxed);
        return rc;
    };
    cfg.frameGenerationCallbackUserContext = this;
    cfg.frameGenerationEnabled = f.enabled;
    cfg.allowAsyncWorkloads = false;   // one hudless texture, reused next frame: the game queue does the work
    if (f.enabled && f.hudless) cfg.HUDLessColor = ffxApiGetResourceDX12(f.hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    cfg.flags = flags;
    cfg.generationRect = { 0, 0, int32_t(f.displayW), int32_t(f.displayH) };
    cfg.frameID = f.frameID;
    if (fns(m_fns).Configure(reinterpret_cast<ffxContext*>(&m_fgContext), &cfg.header) != FFX_API_RETURN_OK) return false;
    if (!f.enabled) return true;
    if (!f.cl || !f.depth || !f.velocity) return false;

    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = f.frameID;
    p.flags = flags;
    p.commandList = f.cl;
    p.renderSize = { f.renderW, f.renderH };
    p.jitterOffset = { f.jitter.x, f.jitter.y };   // what the upscaler is told
    p.motionVectorScale = { -static_cast<float>(f.renderW), -static_cast<float>(f.renderH) };   // as the upscaler's
    p.frameTimeDelta = f.frameMs;
    p.reset = f.reset;
    p.cameraNear = f.nearZ;
    p.cameraFar = f.farZ;
    p.cameraFovAngleVertical = f.fovY;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(f.depth, kRead);
    p.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;   // R32_TYPELESS, read as float (as the upscaler's)
    p.motionVectors = ffxApiGetResourceDX12(f.velocity, kRead);
    const glm::vec3* v[4] = { &f.camPos, &f.camUp, &f.camRight, &f.camForward };
    float* dst[4] = { p.cameraPosition, p.cameraUp, p.cameraRight, p.cameraForward };
    for (int i = 0; i < 4; ++i) { dst[i][0] = v[i]->x; dst[i][1] = v[i]->y; dst[i][2] = v[i]->z; }
    m_fgLastFrameID = f.frameID;
    return fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_fgContext), &p.header) == FFX_API_RETURN_OK;
#else
    (void)f;
    return false;
#endif
}

void D3D12Fsr::destroyFrameGen(IDXGISwapChain* swapchain)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    if (!m_fgContext) return;
    // FG off first: that configure waits for the proxy's queued presents (AMD's teardown order).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = swapchain;
    cfg.frameGenerationEnabled = false;
    cfg.frameID = m_fgLastFrameID + 1;
    fns(m_fns).Configure(reinterpret_cast<ffxContext*>(&m_fgContext), &cfg.header);
    fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_fgContext), nullptr);
#else
    (void)swapchain;
#endif
    m_fgContext = nullptr;
    m_fgW = m_fgH = 0;
}

void D3D12Fsr::destroySwapchainContext()
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG)
    if (m_scContext && m_fns) fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_scContext), nullptr);
#endif
    m_scContext = nullptr;
}
} // namespace HE
