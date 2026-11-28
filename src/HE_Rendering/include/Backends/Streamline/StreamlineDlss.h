#pragma once
// NVIDIA DLSS Super Resolution / DLAA through Streamline (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md, step 15).
// Streamline 2.14 is loaded at run time: sl.interposer.dll beside the engine module (cmake/HeStreamline.cmake puts it and the
// sl.common / sl.dlss / nvngx_dlss DLLs there), its NVIDIA signature checked before LoadLibrary, initialised with MANUAL
// hooking (no global API redirection; the renderer routes the few hooked calls through Streamline's proxies itself),
// DLSS as the only feature, no over-the-air updates. Streamline is process-wide, and so is this: one renderer uses it.
// Without the DLLs, on a GPU without DLSS or in a build without HE_HAVE_STREAMLINE, available() is false and the renderer
// falls back (IRenderer::ResolveAAMethod: DLSS -> FSR -> TAA).
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace HE
{
class StreamlineDlss
{
public:
    enum class Api { D3D12, Vulkan };
    static StreamlineDlss& instance();

    // Loads and initialises Streamline once (logs why not, once). D3D12: after the device, before its command queue and the
    // swapchain are made. Vulkan: before the instance is made. HE_DLSS=0 keeps it off.
    bool init(Api api);
    bool initialized() const { return m_initialized; }
    // slShutdown: before the swapchain, device and instance go.
    void shutdown();

    // D3D12: the device and its adapter (LUID); decides available().
    bool setD3DDevice(void* nativeDevice, const void* luid, uint32_t luidBytes);
    // slUpgradeInterface: `*iface` becomes Streamline's proxy (D3D12 device / DXGI factory). False: left as it was.
    bool upgradeInterface(void** iface);
    // slGetNativeInterface: the native object behind a Streamline proxy, or nullptr if `proxy` is none. The caller takes a
    // reference of its own (whether Streamline added one is not documented: at worst one reference stays until exit).
    void* nativeInterface(void* proxy);

    // Vulkan: what DLSS asks of the instance and the device, and the device once made (decides available()).
    struct VkRequirements
    {
        const char* const* instanceExtensions = nullptr; uint32_t numInstanceExtensions = 0;
        const char* const* deviceExtensions = nullptr;   uint32_t numDeviceExtensions = 0;
        const char* const* features12 = nullptr;         uint32_t numFeatures12 = 0;   // VkPhysicalDeviceVulkan12Features names
        const char* const* features13 = nullptr;         uint32_t numFeatures13 = 0;
        uint32_t graphicsQueues = 0, computeQueues = 0;  // extra queues Streamline wants beside the host's
    };
    bool vkRequirements(VkRequirements& out);
    bool setVulkanInfo(void* instance, void* physicalDevice, void* device, uint32_t graphicsFamily, uint32_t graphicsIndex,
                       uint32_t computeFamily, uint32_t computeIndex);
    // Streamline's vkGetDeviceProcAddr: the proxies of the hooked Vulkan calls (sl_hooks.h); nullptr when not initialised.
    void* vkDeviceProc(void* device, const char* name);

    bool available() const { return m_available; }
    const std::string& version() const { return m_version; }

    // The render size DLSS wants for a render scale (Quality 0.67 ... Ultra Performance 0.33, 1 = DLAA) at an output size,
    // and the range it accepts (dynamic resolution): the scene is rendered at the engine's own scale, clamped into it.
    struct RenderRange { uint32_t optimalW = 0, optimalH = 0, minW = 0, minH = 0, maxW = 0, maxH = 0; };
    bool renderRange(float scale, uint32_t displayW, uint32_t displayH, RenderRange& out);

    // One image as Streamline sees it. D3D12: native = ID3D12Resource*, state = the D3D12_RESOURCE_STATES it is in when DLSS
    // runs. Vulkan: native = VkImage, view = VkImageView, state = VkImageLayout, plus what Vulkan cannot be asked about it.
    struct Image
    {
        void*    native = nullptr;
        void*    view = nullptr;
        uint32_t state = 0;
        uint32_t width = 0, height = 0;
        uint32_t format = 0;   // VkFormat (Vulkan only)
        uint32_t usage = 0;    // VkImageUsageFlags (Vulkan only)
    };
    struct Eval
    {
        void*     cmd = nullptr;            // ID3D12GraphicsCommandList* / VkCommandBuffer, outside any render pass
        uint32_t  frameIndex = 0;           // the host's frame counter (one evaluate per frame)
        Image     color, depth, motion, output;   // HDR in (render size), depth, velocity (render size), HDR out (display)
        uint32_t  renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        float     scale = 1.0f;             // the engine's render scale (picks the DLSS mode)
        glm::vec2 jitter{ 0.0f };           // pixel offset, as DLSS wants it (the renderer owns the sign)
        glm::vec2 mvecScale{ 1.0f };        // velocity -> DLSS's normalised [-1,1] screen units
        glm::mat4 viewToClip{ 1.0f };       // unjittered projection (column-major glm; converted to Streamline's rows)
        glm::mat4 clipToPrevClip{ 1.0f };
        glm::vec3 camPos{ 0.0f }, camUp{ 0.0f, 1.0f, 0.0f }, camRight{ 1.0f, 0.0f, 0.0f }, camFwd{ 0.0f, 0.0f, -1.0f };
        float     nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f, aspect = 1.0f;
        bool      reset = false;
    };
    // Constants, options, tags and the DLSS dispatch on `cmd`. The caller restores its own bindings afterwards.
    bool evaluate(const Eval& e);

private:
    StreamlineDlss() = default;
    void*       m_module = nullptr;
    void*       m_fns = nullptr;   // the resolved function table (Streamline.cpp)
    Api         m_api = Api::D3D12;
    bool        m_tried = false, m_initialized = false, m_available = false;
    uint32_t    m_mode = 0, m_optW = 0, m_optH = 0;   // the options last given (sl::DLSSMode, output size)
    std::string m_version;
};
} // namespace HE
