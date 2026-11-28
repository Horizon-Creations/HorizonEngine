#pragma once
// AMD FSR on Vulkan (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md): the FidelityFX API of the FSR SDK, loaded at run time
// from amd_fidelityfx_loader_vk.dll beside the engine module (cmake/HeFidelityFX.cmake puts it there). The Vulkan twin of
// D3D12Fsr: without the DLL, or in a build without HE_HAVE_FSR, available() is false and the renderer falls back
// (IRenderer::ResolveAAMethod).
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <atomic>
#include <cstdint>
#include <string>

namespace HE
{
class VulkanFsr
{
public:
    VulkanFsr() = default;
    ~VulkanFsr();
    VulkanFsr(const VulkanFsr&) = delete;
    VulkanFsr& operator=(const VulkanFsr&) = delete;

    // Opens the loader DLL once; true when FSR can be used on this machine. Logs why not, once.
    bool load();
    bool available() const { return m_loaded; }

    // The context for these sizes (made again when they change; the caller has waited for the GPU then). False: no FSR.
    bool ensureContext(VkPhysicalDevice phys, VkDevice device, uint32_t renderW, uint32_t renderH,
                       uint32_t displayW, uint32_t displayH);
    bool hasContext() const { return m_context != nullptr; }
    bool sizedFor(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH) const
    {
        return m_context && m_rw == renderW && m_rh == renderH && m_dw == displayW && m_dh == displayH;
    }
    const std::string& version() const { return m_version; }

    // FSR's jitter for frame `index` in render pixels, exactly as the dispatch must be told it (FSR's sign convention).
    glm::vec2 jitter(uint32_t index, uint32_t renderW, uint32_t displayW);

    // One image as FFX sees it: the handle and what it was created with (FFX describes it from that).
    struct Image
    {
        VkImage    image = VK_NULL_HANDLE;
        VkFormat   format = VK_FORMAT_UNDEFINED;
        uint32_t   width = 0, height = 0;
        bool       storage = false;   // created with VK_IMAGE_USAGE_STORAGE_BIT (an output)
    };
    struct Dispatch
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // outside any render pass
        Image color;      // HDR, render size, SHADER_READ_ONLY_OPTIMAL
        Image depth;      // D32, render size, SHADER_READ_ONLY_OPTIMAL (depth aspect)
        Image velocity;   // RG16F uvNow - uvPrev, render size, SHADER_READ_ONLY_OPTIMAL
        Image output;     // RGBA16F with STORAGE, display size, GENERAL
        Image reactive;   // R8 render size, SHADER_READ_ONLY_OPTIMAL; no image = none (fsr_reactive.comp)
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        glm::vec2 jitter{ 0.0f };
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f;
        float frameMs = 16.6f;
        float sharpness = 0.0f;
        bool  reset = false;
    };
    bool dispatch(const Dispatch& d);

    void destroy();   // the context (the caller has waited for the GPU); the DLL stays loaded

    // ── Frame generation (FSR 3.1 FG, HE_HAVE_FSR_FG_VK): one generated frame per rendered one, the game's swapchain ──
    // D3D12Fsr's pattern: the window's swapchain is made by FSR's frame generation swapchain context instead
    // (replaceSwapchain, in place of vkCreateSwapchainKHR), and every acquire / present / get-images / destroy of it goes
    // through FFX's replacement functions — the wrappers below, which are the plain Vulkan calls otherwise. The proxy
    // submits on queues of its own from its threads: present, image acquire and async compute must not be the game's.
    struct Queue  { VkQueue queue = VK_NULL_HANDLE; uint32_t family = 0; };
    struct Queues { Queue game, asyncCompute, present, imageAcquire; };
    bool frameGenAvailable(VkPhysicalDevice phys, VkDevice device) const;   // the FG provider answers on this device (cached)
    // `sc` = VK_NULL_HANDLE (the surface's old swapchain already gone). On success sc is the proxy; on failure it stays
    // VK_NULL_HANDLE and the caller makes a plain one.
    bool replaceSwapchain(VkPhysicalDevice phys, VkDevice device, VkSwapchainKHR& sc, const VkSwapchainCreateInfoKHR& ci,
                          const Queues& q);
    bool swapchainReplaced() const { return m_scContext != nullptr; }
    VkResult acquireNextImage(VkDevice device, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* index);
    VkResult queuePresent(VkQueue queue, const VkPresentInfoKHR* info);
    VkResult getSwapchainImages(VkDevice device, VkSwapchainKHR sc, uint32_t* count, VkImage* images);
    // The swapchain gone; a proxy with all of it (FG off and its context, the queued presents, the swapchain context).
    void destroySwapchain(VkDevice device, VkSwapchainKHR& sc);
    uint64_t presentCount(VkSwapchainKHR sc) const;   // presents since the proxy was made (0 without one)

    // The FG context at the back buffer's size (made again when it changes; the caller has waited for the GPU).
    bool ensureFrameGen(VkPhysicalDevice phys, VkDevice device, uint32_t displayW, uint32_t displayH, VkFormat backBuffer,
                        VkFormat hudless);
    bool hasFrameGen() const { return m_fgContext != nullptr; }
    struct FrameGen
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;   // outside any render pass
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;   // the proxy
        bool enabled = false;                 // false: configure only (FG off)
        uint64_t frameID = 0;                 // +1 per presented frame, the same in configure and prepare
        Image hudless;                        // the frame before its UI, display size, SHADER_READ_ONLY_OPTIMAL
        Image depth;                          // as Dispatch: render size, SHADER_READ_ONLY_OPTIMAL
        Image velocity;
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        glm::vec2 jitter{ 0.0f };
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f, frameMs = 16.6f;
        bool reset = false;
        glm::vec3 camPos{ 0.0f }, camUp{ 0.0f, 1.0f, 0.0f }, camRight{ 1.0f, 0.0f, 0.0f }, camForward{ 0.0f, 0.0f, -1.0f };
    };
    bool frameGen(const FrameGen& f);   // configure, then (enabled) the prepare dispatch on f.cmd
    void destroyFrameGen(VkSwapchainKHR sc);   // FG off and its context gone; the proxy stays
    // Frames FSR generated so far (counted in its dispatch callback, inside the proxy's present).
    uint64_t generatedFrames() const { return m_fgGenerated.load(std::memory_order_relaxed); }

private:
    void*    m_module = nullptr;    // HMODULE of the loader
    void*    m_fns = nullptr;       // ffxFunctions
    void*    m_context = nullptr;   // ffxContext
    bool     m_loaded = false, m_tried = false;
    uint32_t m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
    std::string m_version;
    void*    m_scContext = nullptr;   // the frame generation swapchain's ffxContext
    void*    m_fgContext = nullptr;   // the frame generation ffxContext
    uint32_t m_fgW = 0, m_fgH = 0;
    mutable int m_fgAvail = -1;       // frameGenAvailable's answer, -1 = not asked yet
    uint64_t m_fgLastFrameID = 0;
    std::atomic<uint64_t> m_fgGenerated{ 0 };
    // FFX's replacement functions for the proxy (ffxQueryDescSwapchainReplacementFunctionsVK).
    PFN_vkAcquireNextImageKHR   m_pfnAcquire = nullptr;
    PFN_vkQueuePresentKHR       m_pfnPresent = nullptr;
    PFN_vkGetSwapchainImagesKHR m_pfnGetImages = nullptr;
    void*                       m_pfnDestroy = nullptr;        // PFN_vkDestroySwapchainFFXAPI
    void*                       m_pfnPresentCount = nullptr;   // PFN_getLastPresentCountFFXAPI
};
} // namespace HE
