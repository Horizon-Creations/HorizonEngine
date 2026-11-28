#include "Backends/Streamline/StreamlineDlss.h"

#include <Diagnostics/Logger.h>

#ifdef HE_HAVE_STREAMLINE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>
#include <sl_security.h>
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace HE
{
StreamlineDlss& StreamlineDlss::instance()
{
    static StreamlineDlss s;
    return s;
}

#ifdef HE_HAVE_STREAMLINE
namespace
{
// Streamline's entry points, resolved from the loaded interposer (it is never linked, so a missing DLL costs nothing).
struct Fns
{
    PFun_slInit*                   init = nullptr;
    PFun_slShutdown*               shutdown = nullptr;
    PFun_slIsFeatureSupported*     isFeatureSupported = nullptr;
    PFun_slSetTagForFrame*         setTagForFrame = nullptr;
    PFun_slSetConstants*           setConstants = nullptr;
    PFun_slEvaluateFeature*        evaluateFeature = nullptr;
    PFun_slGetNewFrameToken*       getNewFrameToken = nullptr;
    PFun_slSetD3DDevice*           setD3DDevice = nullptr;
    PFun_slUpgradeInterface*       upgradeInterface = nullptr;
    PFun_slGetNativeInterface*     getNativeInterface = nullptr;
    PFun_slGetFeatureFunction*     getFeatureFunction = nullptr;
    PFun_slGetFeatureRequirements* getFeatureRequirements = nullptr;
    PFun_slGetFeatureVersion*      getFeatureVersion = nullptr;
    PFun_slSetVulkanInfo*          setVulkanInfo = nullptr;
    PFN_vkGetDeviceProcAddr        vkGetDeviceProcAddr = nullptr;
    PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;   // feature functions, after slInit
    PFun_slDLSSSetOptions*         dlssSetOptions = nullptr;
};
Fns& fns(void* p) { return *static_cast<Fns*>(p); }

bool debugOn() { static const bool on = [] { const char* e = std::getenv("HE_DLSS_DEBUG"); return e && *e == '1'; }(); return on; }

void onLog(sl::LogType type, const char* msg)
{
    std::string m(msg ? msg : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (type == sl::LogType::eError)      HE_LOG_ERROR(RHI, "Streamline: %s", m.c_str());
    else if (type == sl::LogType::eWarn)  HE_LOG_WARN(RHI, "Streamline: %s", m.c_str());
    else if (debugOn())                   HE_LOG_INFO(RHI, "Streamline: %s", m.c_str());
}

// The DLSS mode for a render scale: DLAA at 1, else the preset ratio nearest to it (NVIDIA's table).
sl::DLSSMode modeFor(float scale)
{
    if (scale >= 0.99f) return sl::DLSSMode::eDLAA;
    struct { float r; sl::DLSSMode m; } t[] = { { 0.667f, sl::DLSSMode::eMaxQuality }, { 0.58f, sl::DLSSMode::eBalanced },
                                                { 0.5f, sl::DLSSMode::eMaxPerformance }, { 0.333f, sl::DLSSMode::eUltraPerformance } };
    sl::DLSSMode best = t[0].m;
    float d = 1e9f;
    for (const auto& x : t) if (std::fabs(scale - x.r) < d) { d = std::fabs(scale - x.r); best = x.m; }
    return best;
}

sl::float4x4 rowsOf(const glm::mat4& m)
{
    // glm is column-major with column vectors; its memory read as rows is the transposed matrix, which is what
    // Streamline's row-major, row-vector float4x4 is (the convention of NVIDIA's samples).
    sl::float4x4 r;
    std::memcpy(&r, &m, sizeof(r));
    return r;
}

const char* resultName(sl::Result r)
{
    switch (r)
    {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "driver out of date";
    case sl::Result::eErrorOSOutOfDate: return "OS out of date";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported adapter (not an NVIDIA RTX GPU)";
    case sl::Result::eErrorAdapterNotSupported: return "adapter not supported";
    case sl::Result::eErrorFeatureMissing: return "feature missing";
    case sl::Result::eErrorFeatureNotSupported: return "feature not supported";
    default: return "error";
    }
}
} // namespace
#endif

bool StreamlineDlss::init(Api api)
{
    if (m_tried) return m_initialized && m_api == api;
    m_tried = true;
    m_api = api;
#ifdef HE_HAVE_STREAMLINE
    if (const char* e = std::getenv("HE_DLSS"); e && *e == '0')
    {
        HE_LOG_INFO(RHI, "%s", "Streamline: HE_DLSS=0, DLSS off");
        return false;
    }
    // Beside the module this code is in (editor: beside the exe; game: beside HorizonEngine.dll), as FSR's loader.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&StreamlineDlss::instance), &self);
    wchar_t mod[MAX_PATH] = {};
    GetModuleFileNameW(self, mod, MAX_PATH);
    std::wstring dir(mod);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring path = dir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        HE_LOG_INFO(RHI, "%s", "Streamline: DLSS unavailable (sl.interposer.dll not beside the engine)");
        return false;
    }
    // NVIDIA's own check (sl_security.h): a DLL that is not NVIDIA-signed is never loaded.
    if (!sl::security::verifyEmbeddedSignature(path.c_str()))
    {
        HE_LOG_WARN(RHI, "%s", "Streamline: sl.interposer.dll is not signed by NVIDIA — DLSS off");
        return false;
    }
    HMODULE h = LoadLibraryW(path.c_str());
    if (!h)
    {
        HE_LOG_WARN(RHI, "Streamline: sl.interposer.dll did not load (%lu) — DLSS off", GetLastError());
        return false;
    }
    auto* f = new Fns{};
    auto get = [&](auto& slot, const char* name) { slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(GetProcAddress(h, name)); };
    get(f->init, "slInit"); get(f->shutdown, "slShutdown"); get(f->isFeatureSupported, "slIsFeatureSupported");
    get(f->setTagForFrame, "slSetTagForFrame"); get(f->setConstants, "slSetConstants"); get(f->evaluateFeature, "slEvaluateFeature");
    get(f->getNewFrameToken, "slGetNewFrameToken"); get(f->setD3DDevice, "slSetD3DDevice"); get(f->upgradeInterface, "slUpgradeInterface");
    get(f->getNativeInterface, "slGetNativeInterface");
    get(f->getFeatureFunction, "slGetFeatureFunction"); get(f->getFeatureRequirements, "slGetFeatureRequirements");
    get(f->getFeatureVersion, "slGetFeatureVersion"); get(f->setVulkanInfo, "slSetVulkanInfo");
    get(f->vkGetDeviceProcAddr, "vkGetDeviceProcAddr");
    if (!f->init || !f->shutdown || !f->isFeatureSupported || !f->setTagForFrame || !f->setConstants || !f->evaluateFeature
        || !f->getNewFrameToken || !f->setD3DDevice || !f->upgradeInterface || !f->getFeatureFunction)
    {
        HE_LOG_WARN(RHI, "%s", "Streamline: sl.interposer.dll lacks the Streamline entry points — DLSS off");
        delete f;
        FreeLibrary(h);
        return false;
    }
    static std::wstring s_dir;   // the plugin path must outlive slInit
    s_dir = dir;
    static const wchar_t* s_paths[1];
    s_paths[0] = s_dir.c_str();
    static const sl::Feature s_features[] = { sl::kFeatureDLSS };
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = debugOn() ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = s_paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = &onLog;
    // Manual hooking: no global redirection of D3D/DXGI/Vulkan (FSR's frame generation swapchain and the debug layer see
    // the native objects). No over-the-air updates: the pinned, signed DLLs beside the engine are what runs.
    // Frame-based tagging: slSetTagForFrame, the tags belong to the frame token they are given with.
    // A DXGI factory proxy of its own instead of a patched v-table on the native factory class (which every factory in the
    // process shares: the adapter query, FSR's frame generation swapchain).
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking
               | sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    pref.featuresToLoad = s_features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "HorizonEngine";
    pref.projectId = "5f0d6c3a-4e2b-4c8e-9a51-7b3d2e8f1c64";
    pref.renderAPI = api == Api::Vulkan ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;
    const sl::Result r = f->init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk)
    {
        HE_LOG_WARN(RHI, "Streamline: slInit failed (%d, %s) — DLSS off", int(r), resultName(r));
        delete f;
        FreeLibrary(h);
        return false;
    }
    m_module = h;
    m_fns = f;
    m_initialized = true;
    HE_LOG_INFO(RHI, "Streamline %d.%d.%d initialised (%s, manual hooking)", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH,
                api == Api::Vulkan ? "Vulkan" : "D3D12");
    return true;
#else
    return false;
#endif
}

void StreamlineDlss::shutdown()
{
#ifdef HE_HAVE_STREAMLINE
    if (m_initialized)
    {
        fns(m_fns).shutdown();
        delete static_cast<Fns*>(m_fns);
        m_fns = nullptr;
        // The interposer stays loaded: the renderer releases the swapchain and queue proxies it made AFTER slShutdown (as
        // Streamline asks), and their code lives in it. The process ends with it.
    }
#endif
    m_initialized = m_available = false;
}

#ifdef HE_HAVE_STREAMLINE
namespace
{
void logSupport(sl::Result r, std::string& version, void* fnsPtr, bool& available)
{
    available = r == sl::Result::eOk;
    if (!available)
    {
        HE_LOG_INFO(RHI, "Streamline: DLSS unavailable on this GPU (%s)", resultName(r));
        return;
    }
    // The feature's own functions exist once the device is known (slSetD3DDevice / slSetVulkanInfo).
    Fns& f = fns(fnsPtr);
    void* fn = nullptr;
    if (f.getFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) == sl::Result::eOk)
        f.dlssGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
    fn = nullptr;
    if (f.getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn) == sl::Result::eOk)
        f.dlssSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
    if (!f.dlssSetOptions)
    {
        available = false;
        HE_LOG_WARN(RHI, "%s", "Streamline: DLSS loaded without its option functions — DLSS off");
        return;
    }
    sl::FeatureVersion v{};
    if (fns(fnsPtr).getFeatureVersion && fns(fnsPtr).getFeatureVersion(sl::kFeatureDLSS, v) == sl::Result::eOk)
        version = std::to_string(v.versionNGX.major) + "." + std::to_string(v.versionNGX.minor) + "." + std::to_string(v.versionNGX.build);
    else
        version = "?";
    HE_LOG_INFO(RHI, "Streamline: DLSS %s available", version.c_str());
}
} // namespace
#endif

bool StreamlineDlss::setD3DDevice(void* nativeDevice, const void* luid, uint32_t luidBytes)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !nativeDevice) return false;
    const sl::Result r = fns(m_fns).setD3DDevice(nativeDevice);
    if (r != sl::Result::eOk)
    {
        HE_LOG_WARN(RHI, "Streamline: slSetD3DDevice failed (%d) — DLSS off", int(r));
        m_available = false;
        return false;
    }
    sl::AdapterInfo ai{};
    ai.deviceLUID = static_cast<uint8_t*>(const_cast<void*>(luid));
    ai.deviceLUIDSizeInBytes = luidBytes;
    logSupport(fns(m_fns).isFeatureSupported(sl::kFeatureDLSS, ai), m_version, m_fns, m_available);
    return m_available;
#else
    (void)nativeDevice; (void)luid; (void)luidBytes;
    return false;
#endif
}

bool StreamlineDlss::upgradeInterface(void** iface)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !iface || !*iface) return false;
    void* before = *iface;
    if (fns(m_fns).upgradeInterface(iface) == sl::Result::eOk && *iface) return true;
    *iface = before;
    return false;
#else
    (void)iface;
    return false;
#endif
}

void* StreamlineDlss::nativeInterface(void* proxy)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !proxy || !fns(m_fns).getNativeInterface) return nullptr;
    void* native = nullptr;
    if (fns(m_fns).getNativeInterface(proxy, &native) != sl::Result::eOk || !native || native == proxy) return nullptr;
    return native;
#else
    (void)proxy;
    return nullptr;
#endif
}

bool StreamlineDlss::vkRequirements(VkRequirements& out)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !fns(m_fns).getFeatureRequirements) return false;
    static sl::FeatureRequirements req{};   // its string lists are Streamline's, valid while it is loaded
    if (fns(m_fns).getFeatureRequirements(sl::kFeatureDLSS, req) != sl::Result::eOk) return false;
    out.instanceExtensions = req.vkInstanceExtensions; out.numInstanceExtensions = req.vkNumInstanceExtensions;
    out.deviceExtensions = req.vkDeviceExtensions;     out.numDeviceExtensions = req.vkNumDeviceExtensions;
    out.features12 = req.vkFeatures12;                 out.numFeatures12 = req.vkNumFeatures12;
    out.features13 = req.vkFeatures13;                 out.numFeatures13 = req.vkNumFeatures13;
    out.graphicsQueues = req.vkNumGraphicsQueuesRequired;
    out.computeQueues = req.vkNumComputeQueuesRequired;
    return true;
#else
    (void)out;
    return false;
#endif
}

bool StreamlineDlss::setVulkanInfo(void* instance, void* physicalDevice, void* device, uint32_t graphicsFamily,
                                   uint32_t graphicsIndex, uint32_t computeFamily, uint32_t computeIndex)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !fns(m_fns).setVulkanInfo) return false;
    sl::VulkanInfo vi{};
    vi.instance = static_cast<VkInstance>(instance);
    vi.physicalDevice = static_cast<VkPhysicalDevice>(physicalDevice);
    vi.device = static_cast<VkDevice>(device);
    vi.graphicsQueueFamily = graphicsFamily; vi.graphicsQueueIndex = graphicsIndex;
    vi.computeQueueFamily = computeFamily;   vi.computeQueueIndex = computeIndex;
    const sl::Result r = fns(m_fns).setVulkanInfo(vi);
    if (r != sl::Result::eOk)
    {
        HE_LOG_WARN(RHI, "Streamline: slSetVulkanInfo failed (%d) — DLSS off", int(r));
        m_available = false;
        return false;
    }
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = physicalDevice;
    logSupport(fns(m_fns).isFeatureSupported(sl::kFeatureDLSS, ai), m_version, m_fns, m_available);
    return m_available;
#else
    (void)instance; (void)physicalDevice; (void)device; (void)graphicsFamily; (void)graphicsIndex; (void)computeFamily; (void)computeIndex;
    return false;
#endif
}

void* StreamlineDlss::vkDeviceProc(void* device, const char* name)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_initialized || !fns(m_fns).vkGetDeviceProcAddr) return nullptr;
    return reinterpret_cast<void*>(fns(m_fns).vkGetDeviceProcAddr(static_cast<VkDevice>(device), name));
#else
    (void)device; (void)name;
    return nullptr;
#endif
}

bool StreamlineDlss::renderRange(float scale, uint32_t displayW, uint32_t displayH, RenderRange& out)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_available || !fns(m_fns).dlssGetOptimalSettings) return false;
    sl::DLSSOptions o{};
    o.mode = modeFor(scale);
    o.outputWidth = displayW; o.outputHeight = displayH;
    sl::DLSSOptimalSettings s{};
    if (fns(m_fns).dlssGetOptimalSettings(o, s) != sl::Result::eOk) return false;
    out = { s.optimalRenderWidth, s.optimalRenderHeight, s.renderWidthMin, s.renderHeightMin, s.renderWidthMax, s.renderHeightMax };
    return true;
#else
    (void)scale; (void)displayW; (void)displayH; (void)out;
    return false;
#endif
}

bool StreamlineDlss::evaluate(const Eval& e)
{
#ifdef HE_HAVE_STREAMLINE
    if (!m_available || !e.cmd || !e.color.native || !e.depth.native || !e.motion.native || !e.output.native) return false;
    Fns& f = fns(m_fns);
    sl::FrameToken* token = nullptr;
    if (f.getNewFrameToken(token, &e.frameIndex) != sl::Result::eOk || !token) return false;
    const sl::ViewportHandle vp(0u);

    sl::Constants c{};
    c.cameraViewToClip = rowsOf(e.viewToClip);
    c.clipToCameraView = rowsOf(glm::inverse(e.viewToClip));
    c.clipToLensClip = rowsOf(glm::mat4(1.0f));
    c.clipToPrevClip = rowsOf(e.clipToPrevClip);
    c.prevClipToClip = rowsOf(glm::inverse(e.clipToPrevClip));
    c.jitterOffset = { e.jitter.x, e.jitter.y };
    c.mvecScale = { e.mvecScale.x, e.mvecScale.y };
    c.cameraPinholeOffset = { 0.0f, 0.0f };
    c.cameraPos = { e.camPos.x, e.camPos.y, e.camPos.z };
    c.cameraUp = { e.camUp.x, e.camUp.y, e.camUp.z };
    c.cameraRight = { e.camRight.x, e.camRight.y, e.camRight.z };
    c.cameraFwd = { e.camFwd.x, e.camFwd.y, e.camFwd.z };
    c.cameraNear = e.nearZ; c.cameraFar = e.farZ; c.cameraFOV = e.fovY; c.cameraAspectRatio = e.aspect;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = e.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    if (f.setConstants(c, *token, vp) != sl::Result::eOk) return false;

    // Options on a change only: the mode follows the engine's scale, the output the display size.
    const sl::DLSSMode mode = modeFor(e.scale);
    if (uint32_t(mode) != m_mode || e.displayW != m_optW || e.displayH != m_optH)
    {
        sl::DLSSOptions o{};
        o.mode = mode;
        o.outputWidth = e.displayW; o.outputHeight = e.displayH;
        o.colorBuffersHDR = sl::Boolean::eTrue;          // linear HDR, before the tonemap
        o.useAutoExposure = sl::Boolean::eTrue;          // no exposure texture (the engine's exposure is a fixed 1)
        if (!f.dlssSetOptions || f.dlssSetOptions(vp, o) != sl::Result::eOk) return false;
        m_mode = uint32_t(mode); m_optW = e.displayW; m_optH = e.displayH;
        static const char* names[] = { "off", "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA" };
        HE_LOG_INFO(RHI, "Streamline: DLSS %s %s (%ux%u -> %ux%u)", m_version.c_str(), names[uint32_t(mode) < 7 ? uint32_t(mode) : 0],
                    e.renderW, e.renderH, e.displayW, e.displayH);
    }

    auto res = [&](const Image& i) {
        sl::Resource r(sl::ResourceType::eTex2d, i.native, nullptr, i.view, i.state);
        r.width = i.width; r.height = i.height; r.nativeFormat = i.format; r.mipLevels = 1; r.arrayLayers = 1; r.usage = i.usage;
        return r;
    };
    sl::Resource colorIn = res(e.color), colorOut = res(e.output), depth = res(e.depth), mvec = res(e.motion);
    const sl::Extent renderExt{ 0, 0, e.renderW, e.renderH }, displayExt{ 0, 0, e.displayW, e.displayH };
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorIn,  sl::kBufferTypeScalingInputColor,  sl::ResourceLifecycle::eValidUntilPresent, &renderExt),
        sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &displayExt),
        sl::ResourceTag(&depth,    sl::kBufferTypeDepth,              sl::ResourceLifecycle::eValidUntilPresent, &renderExt),
        sl::ResourceTag(&mvec,     sl::kBufferTypeMotionVectors,      sl::ResourceLifecycle::eValidUntilPresent, &renderExt),
    };
    if (f.setTagForFrame(*token, vp, tags, 4, e.cmd) != sl::Result::eOk) return false;
    const sl::BaseStructure* inputs[] = { &vp };
    const sl::Result r = f.evaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, e.cmd);
    if (r != sl::Result::eOk)
    {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; HE_LOG_WARN(RHI, "Streamline: DLSS evaluate failed (%d)", int(r)); }
        return false;
    }
    return true;
#else
    (void)e;
    return false;
#endif
}
} // namespace HE
