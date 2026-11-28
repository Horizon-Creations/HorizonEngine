#include "Backends/Vulkan/VulkanFsr.h"

#include <Diagnostics/Logger.h>

#ifdef HE_HAVE_FSR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>
#ifdef HE_HAVE_FSR_FG_VK
#include <ffx_framegeneration.h>
#include <vk/ffx_api_framegeneration_vk.h>
#endif
#endif

#include <cstdlib>
#include <cstring>

namespace HE
{
#ifdef HE_HAVE_FSR
namespace
{
ffxFunctions& fns(void* p) { return *static_cast<ffxFunctions*>(p); }

// The runtime's own complaints, only with HE_FSR_DEBUG=1 (it also turns the runtime's API checks on).
bool debugOn() { static const bool on = [] { const char* e = std::getenv("HE_FSR_DEBUG"); return e && *e == '1'; }(); return on; }
void onMessage(uint32_t type, const wchar_t* message)
{
    char buf[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) HE_LOG_ERROR(RHI, "FSR: %s", buf);
    else                                    HE_LOG_WARN(RHI, "FSR: %s", buf);
}

// FFX describes a Vulkan image from its create info; only the fields it reads are filled.
FfxApiResource resourceOf(const VulkanFsr::Image& img, uint32_t state)
{
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = img.format;
    ci.extent = { img.width, img.height, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (img.storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    return ffxApiGetResourceVK(reinterpret_cast<void*>(img.image), ffxApiGetImageResourceDescriptionVK(img.image, ci, 0), state);
}
} // namespace
#endif

VulkanFsr::~VulkanFsr()
{
    destroy();
#ifdef HE_HAVE_FSR
    delete static_cast<ffxFunctions*>(m_fns);
    if (m_module) FreeLibrary(static_cast<HMODULE>(m_module));
#endif
}

bool VulkanFsr::load()
{
    if (m_tried) return m_loaded;
    m_tried = true;
#ifdef HE_HAVE_FSR
    // By absolute path beside the module this code is in (editor: beside the exe; game: beside HorizonEngine.dll).
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fns), &self);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(self, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_loader_vk.dll";
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod)
    {
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: FSR unavailable (amd_fidelityfx_loader_vk.dll not beside the engine)");
        return false;
    }
    auto* f = new ffxFunctions{};
    ffxLoadFunctions(f, mod);
    if (!f->CreateContext || !f->DestroyContext || !f->Configure || !f->Query || !f->Dispatch)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: FSR unavailable (the loader DLL lacks the FidelityFX API entry points)");
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

bool VulkanFsr::ensureContext(VkPhysicalDevice phys, VkDevice device, uint32_t renderW, uint32_t renderH,
                              uint32_t displayW, uint32_t displayH)
{
#ifdef HE_HAVE_FSR
    if (!m_loaded || !device || !phys || !renderW || !renderH || !displayW || !displayH) return false;
    if (m_context && m_rw == renderW && m_rh == renderH && m_dw == displayW && m_dh == displayH) return true;
    destroy();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = device;
    backend.deviceContext.vkPhysicalDevice = phys;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.header.pNext = &backend.header;
    ver.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &ver.header;
    // Linear HDR in, FSR measures the exposure. Depth 0..1 (kVulkanClipFix), not inverted.
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
               | (debugOn() ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u);
    desc.maxRenderSize  = { renderW, renderH };
    desc.maxUpscaleSize = { displayW, displayH };
    desc.fpMessage = debugOn() ? &onMessage : nullptr;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx)
    {
        HE_LOG_WARN(RHI, "VulkanRenderer: FSR context creation failed (%u) at %ux%u -> %ux%u", rc, renderW, renderH, displayW, displayH);
        return false;
    }
    m_context = ctx;
    m_rw = renderW; m_rh = renderH; m_dw = displayW; m_dh = displayH;
    if (m_version.empty())
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = &backend.deviceContext;   // for VK a VkDeviceContext* (ffx_api.h), not a handle
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
        HE_LOG_INFO(RHI, "VulkanRenderer: FSR %s active (%ux%u -> %ux%u)", m_version.c_str(), renderW, renderH, displayW, displayH);
    }
    return true;
#else
    (void)phys; (void)device; (void)renderW; (void)renderH; (void)displayW; (void)displayH;
    return false;
#endif
}

glm::vec2 VulkanFsr::jitter(uint32_t index, uint32_t renderW, uint32_t displayW)
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

bool VulkanFsr::dispatch(const Dispatch& d)
{
#ifdef HE_HAVE_FSR
    if (!m_context || !d.cmd || !d.color.image || !d.depth.image || !d.velocity.image || !d.output.image) return false;
    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale u{};
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList   = d.cmd;
    u.color         = resourceOf(d.color, kRead);
    u.depth         = resourceOf(d.depth, kRead);
    u.motionVectors = resourceOf(d.velocity, kRead);
    u.output        = resourceOf(d.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    if (d.reactive.image) u.reactive = resourceOf(d.reactive, kRead);
    u.jitterOffset  = { d.jitter.x, d.jitter.y };
    // The velocity is uvNow - uvPrev in uv (y down), at the render size: FSR wants the way back, in render pixels.
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
    static const bool s_view = [] { const char* e = std::getenv("HE_FSR_DEBUGVIEW"); return e && *e == '1'; }();
    if (s_view) u.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    return fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_context), &u.header) == FFX_API_RETURN_OK;
#else
    (void)d;
    return false;
#endif
}

void VulkanFsr::destroy()
{
#ifdef HE_HAVE_FSR
    if (m_context && m_fns) fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_context), nullptr);
#endif
    m_context = nullptr;
    m_rw = m_rh = m_dw = m_dh = 0;
}

// ── Frame generation ─────────────────────────────────────────────────────────
bool VulkanFsr::frameGenAvailable(VkPhysicalDevice phys, VkDevice device) const
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (m_fgAvail >= 0) return m_fgAvail == 1;
    if (!m_loaded || !device || !phys) return false;
    VkDeviceContext dc{ device, phys, vkGetDeviceProcAddr };
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = &dc;   // for VK a VkDeviceContext* (ffx_api.h), not a handle
    q.outputCount = &count;
    fns(m_fns).Query(nullptr, &q.header);
    m_fgAvail = count > 0 ? 1 : 0;
    if (!m_fgAvail)
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: FSR frame generation unavailable (no provider: amd_fidelityfx_framegeneration_vk.dll)");
    return m_fgAvail == 1;
#else
    (void)phys; (void)device;
    return false;
#endif
}

bool VulkanFsr::replaceSwapchain(VkPhysicalDevice phys, VkDevice device, VkSwapchainKHR& sc, const VkSwapchainCreateInfoKHR& ci,
                                 const Queues& q)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (m_scContext || sc || !m_loaded || !device || !q.game.queue || !q.asyncCompute.queue || !q.present.queue
        || !q.imageAcquire.queue) return false;
    ffxCreateContextDescFrameGenerationSwapChainVersionVK ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainVK desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    desc.header.pNext = &ver.header;
    desc.physicalDevice = phys;
    desc.device = device;
    desc.swapchain = &sc;
    desc.createInfo = ci;
    desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    auto info = [](const Queue& x) { VkQueueInfoFFXAPI i{}; i.queue = x.queue; i.familyIndex = x.family; return i; };
    desc.gameQueue = info(q.game);
    desc.asyncComputeQueue = info(q.asyncCompute);
    desc.presentQueue = info(q.present);
    desc.imageAcquireQueue = info(q.imageAcquire);
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    ffxQueryDescSwapchainReplacementFunctionsVK r{};
    r.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (rc == FFX_API_RETURN_OK && ctx && sc && fns(m_fns).Query(&ctx, &r.header) == FFX_API_RETURN_OK
        && r.pOutAcquireNextImageKHR && r.pOutQueuePresentKHR && r.pOutGetSwapchainImagesKHR && r.pOutDestroySwapchainFFXAPI)
    {
        m_scContext = ctx;
        m_pfnAcquire = r.pOutAcquireNextImageKHR;
        m_pfnPresent = r.pOutQueuePresentKHR;
        m_pfnGetImages = r.pOutGetSwapchainImagesKHR;
        m_pfnDestroy = reinterpret_cast<void*>(r.pOutDestroySwapchainFFXAPI);
        m_pfnPresentCount = reinterpret_cast<void*>(r.pOutGetLastPresentCountFFXAPI);
        HE_LOG_INFO(RHI, "VulkanRenderer: FSR frame generation swapchain in place (%ux%u)", ci.imageExtent.width, ci.imageExtent.height);
        return true;
    }
    if (ctx) fns(m_fns).DestroyContext(&ctx, nullptr);
    if (sc && r.pOutDestroySwapchainFFXAPI) r.pOutDestroySwapchainFFXAPI(device, sc, nullptr, nullptr);
    sc = VK_NULL_HANDLE;
    HE_LOG_WARN(RHI, "VulkanRenderer: FSR frame generation swapchain failed (%u) — frame generation off", rc);
    return false;
#else
    (void)phys; (void)device; (void)sc; (void)ci; (void)q;
    return false;
#endif
}

VkResult VulkanFsr::acquireNextImage(VkDevice device, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence,
                                     uint32_t* index)
{
    return (m_scContext ? m_pfnAcquire : vkAcquireNextImageKHR)(device, sc, timeout, sem, fence, index);
}

VkResult VulkanFsr::queuePresent(VkQueue queue, const VkPresentInfoKHR* info)
{
    return (m_scContext ? m_pfnPresent : vkQueuePresentKHR)(queue, info);
}

VkResult VulkanFsr::getSwapchainImages(VkDevice device, VkSwapchainKHR sc, uint32_t* count, VkImage* images)
{
    return (m_scContext ? m_pfnGetImages : vkGetSwapchainImagesKHR)(device, sc, count, images);
}

uint64_t VulkanFsr::presentCount(VkSwapchainKHR sc) const
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (m_scContext && m_pfnPresentCount) return reinterpret_cast<PFN_getLastPresentCountFFXAPI>(m_pfnPresentCount)(sc);
#endif
    (void)sc;
    return 0;
}

void VulkanFsr::destroySwapchain(VkDevice device, VkSwapchainKHR& sc)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (m_scContext)
    {
        // AMD's teardown order: FG off and its context, the proxy's queued presents, the swapchain context, the swapchain.
        destroyFrameGen(sc);
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w{};
        w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        const ffxReturnCode_t rw = fns(m_fns).Dispatch(reinterpret_cast<ffxContext*>(&m_scContext), &w.header);
        const ffxReturnCode_t rd = fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_scContext), nullptr);
        if (rw != FFX_API_RETURN_OK || rd != FFX_API_RETURN_OK)
            HE_LOG_WARN(RHI, "VulkanRenderer: FSR frame generation swapchain teardown: wait %u, destroy %u", rw, rd);
        m_scContext = nullptr;
        if (sc) reinterpret_cast<PFN_vkDestroySwapchainFFXAPI>(m_pfnDestroy)(device, sc, nullptr, nullptr);
        sc = VK_NULL_HANDLE;
        m_pfnAcquire = nullptr; m_pfnPresent = nullptr; m_pfnGetImages = nullptr;
        m_pfnDestroy = m_pfnPresentCount = nullptr;
        return;
    }
#endif
    if (sc) vkDestroySwapchainKHR(device, sc, nullptr);
    sc = VK_NULL_HANDLE;
}

bool VulkanFsr::ensureFrameGen(VkPhysicalDevice phys, VkDevice device, uint32_t displayW, uint32_t displayH, VkFormat backBuffer,
                               VkFormat hudless)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (!m_scContext || !device || !phys || !displayW || !displayH) return false;
    if (m_fgContext && m_fgW == displayW && m_fgH == displayH) return true;
    if (m_fgContext) { fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_fgContext), nullptr); m_fgContext = nullptr; }
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = { device, phys, vkGetDeviceProcAddr };
    ffxCreateContextDescFrameGenerationHudless hud{};
    hud.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hud.header.pNext = &backend.header;
    hud.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(hudless);   // RGBA8 (the viewport image), the swapchain BGRA8
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = &hud.header;
    ver.version = FFX_FRAMEGENERATION_VERSION;   // PrepareV2 works only with this chained
    ffxCreateContextDescFrameGeneration desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = &ver.header;
    // The back buffer it interpolates is display-encoded 8-bit: no HDR flag. Depth 0..1 (kVulkanClipFix), not inverted.
    desc.flags = debugOn() ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
    desc.displaySize   = { displayW, displayH };
    desc.maxRenderSize = { displayW, displayH };
    desc.backBufferFormat = ffxApiGetSurfaceFormatVK(backBuffer);
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = fns(m_fns).CreateContext(&ctx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx)
    {
        HE_LOG_WARN(RHI, "VulkanRenderer: FSR frame generation context failed (%u) at %ux%u", rc, displayW, displayH);
        return false;
    }
    m_fgContext = ctx;
    m_fgW = displayW; m_fgH = displayH;
    HE_LOG_INFO(RHI, "VulkanRenderer: FSR frame generation ready (%ux%u)", displayW, displayH);
    return true;
#else
    (void)phys; (void)device; (void)displayW; (void)displayH; (void)backBuffer; (void)hudless;
    return false;
#endif
}

bool VulkanFsr::frameGen(const FrameGen& f)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (!m_fgContext || !f.swapchain) return false;
    // HE_FSR_FG_DEBUG=1: AMD's tear lines on the generated frames' edges (a visible witness that frames are generated).
    static const bool s_lines = [] { const char* e = std::getenv("HE_FSR_FG_DEBUG"); return e && *e == '1'; }();
    const uint32_t flags = s_lines ? uint32_t(FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES) : 0u;
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = reinterpret_cast<void*>(f.swapchain);
    // AMD's recommended default: the proxy dispatches the generation itself, inside its present.
    cfg.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* user) -> ffxReturnCode_t
    {
        auto* self = static_cast<VulkanFsr*>(user);
        const ffxReturnCode_t rc = fns(self->m_fns).Dispatch(reinterpret_cast<ffxContext*>(&self->m_fgContext), &params->header);
        if (rc == FFX_API_RETURN_OK) self->m_fgGenerated.fetch_add(params->numGeneratedFrames, std::memory_order_relaxed);
        return rc;
    };
    cfg.frameGenerationCallbackUserContext = this;
    cfg.frameGenerationEnabled = f.enabled;
    cfg.allowAsyncWorkloads = false;   // one hudless image, reused next frame: the game queue does the work
    if (f.enabled && f.hudless.image) cfg.HUDLessColor = resourceOf(f.hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    cfg.flags = flags;
    cfg.generationRect = { 0, 0, int32_t(f.displayW), int32_t(f.displayH) };
    cfg.frameID = f.frameID;
    if (fns(m_fns).Configure(reinterpret_cast<ffxContext*>(&m_fgContext), &cfg.header) != FFX_API_RETURN_OK) return false;
    if (!f.enabled) return true;
    if (!f.cmd || !f.depth.image || !f.velocity.image) return false;

    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = f.frameID;
    p.flags = flags;
    p.commandList = f.cmd;
    p.renderSize = { f.renderW, f.renderH };
    p.jitterOffset = { f.jitter.x, f.jitter.y };   // what the upscaler is told
    p.motionVectorScale = { -static_cast<float>(f.renderW), -static_cast<float>(f.renderH) };   // as the upscaler's
    p.frameTimeDelta = f.frameMs;
    p.reset = f.reset;
    p.cameraNear = f.nearZ;
    p.cameraFar = f.farZ;
    p.cameraFovAngleVertical = f.fovY;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = resourceOf(f.depth, kRead);
    p.motionVectors = resourceOf(f.velocity, kRead);
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

void VulkanFsr::destroyFrameGen(VkSwapchainKHR sc)
{
#if defined(HE_HAVE_FSR) && defined(HE_HAVE_FSR_FG_VK)
    if (!m_fgContext) return;
    // FG off first: that configure waits for the proxy's queued presents (AMD's teardown order).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = reinterpret_cast<void*>(sc);
    cfg.frameGenerationEnabled = false;
    cfg.frameID = m_fgLastFrameID + 1;
    const ffxReturnCode_t rc = fns(m_fns).Configure(reinterpret_cast<ffxContext*>(&m_fgContext), &cfg.header);
    const ffxReturnCode_t rd = fns(m_fns).DestroyContext(reinterpret_cast<ffxContext*>(&m_fgContext), nullptr);
    if (rc != FFX_API_RETURN_OK || rd != FFX_API_RETURN_OK)
        HE_LOG_WARN(RHI, "VulkanRenderer: FSR frame generation teardown: configure %u, destroy %u", rc, rd);
#else
    (void)sc;
#endif
    m_fgContext = nullptr;
    m_fgW = m_fgH = 0;
}
} // namespace HE
