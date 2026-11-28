#pragma once
// AMD FSR on Vulkan (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md): the FidelityFX API of the FSR SDK, loaded at run time
// from amd_fidelityfx_loader_vk.dll beside the engine module (cmake/HeFidelityFX.cmake puts it there). The Vulkan twin of
// D3D12Fsr: without the DLL, or in a build without HE_HAVE_FSR, available() is false and the renderer falls back
// (IRenderer::ResolveAAMethod).
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
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
        Image reactive;   // R8 render size, SHADER_READ_ONLY_OPTIMAL; no image = none (generateReactive)
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        glm::vec2 jitter{ 0.0f };
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f;
        float frameMs = 16.6f;
        float sharpness = 0.0f;
        bool  reset = false;
    };
    bool dispatch(const Dispatch& d);

    // FSR's reactive mask from the frame before and after its blended tail (see D3D12Fsr::generateReactive). opaque and
    // color SHADER_READ_ONLY_OPTIMAL, reactive R8 with STORAGE in GENERAL; all render size; outside any render pass.
    bool generateReactive(VkCommandBuffer cmd, const Image& opaque, const Image& color, const Image& reactive,
                          uint32_t renderW, uint32_t renderH);

    void destroy();   // the context (the caller has waited for the GPU); the DLL stays loaded

private:
    void*    m_module = nullptr;    // HMODULE of the loader
    void*    m_fns = nullptr;       // ffxFunctions
    void*    m_context = nullptr;   // ffxContext
    bool     m_loaded = false, m_tried = false;
    uint32_t m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
    std::string m_version;
};
} // namespace HE
