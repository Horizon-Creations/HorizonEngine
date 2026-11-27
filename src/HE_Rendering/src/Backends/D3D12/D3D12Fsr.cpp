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
    // By absolute path beside the executable (where the deploy puts it), not through the DLL search order.
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
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
    u.motionVectorScale = { -static_cast<float>(d.renderW), -static_cast<float>(d.renderH) };    u.renderSize  = { d.renderW, d.renderH };
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
    return fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_context), &u.header) == FFX_API_RETURN_OK;
#else
    (void)d;
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
} // namespace HE
