#include "Backends/Vulkan/VulkanRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <memory>
#include <Diagnostics/Logger.h>

static constexpr uint32_t k_maxFramesInFlight = 2;

static void vkCheck(VkResult r, const char* msg)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan: ") + msg);
}

namespace
{
    // Push constants: per-object transforms (128 bytes — the guaranteed minimum).
    struct PushConstants { glm::mat4 mvp; glm::mat4 model; };

    // Per-frame UBO, std140 layout (matches the GLSL `Frame` block).
    struct FrameUBOData
    {
        glm::vec4  cameraPos;
        glm::ivec4 lightCount;
        glm::vec4  lightPos[8];
        glm::vec4  lightDir[8];
        glm::vec4  lightColor[8];
        glm::vec4  lightParams[8];
        glm::mat4  lightVP;
        glm::ivec4 shadowEnabled;
    };

    // The camera projection is built by the shared RenderExtractor with GL
    // conventions (Y up, depth -1..1). This remaps clip space for Vulkan
    // (Y down, depth 0..1) so it doesn't depend on how glm was compiled there.
    const glm::mat4 kVulkanClipFix = glm::mat4(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f,-1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, 0.0f,
        0.0f, 0.0f, 0.5f, 1.0f);
}

VulkanRenderer::VulkanRenderer()  = default;
VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::Initialize(HE::Window* window)
{
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: initializing");
    m_sdlWindow = window->GetNativeWindow();
    createInstance();       Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: instance created");
    createSurface();        Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: surface created");
    pickPhysicalDevice();   Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: physical device selected");
    createDevice();         Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: logical device created");
    createSwapchain(window->GetWidth(), window->GetHeight()); Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: swapchain created");
    createRenderPass();     Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: render pass created");
    createFramebuffers();   Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: framebuffers created");
    createCommandBuffers(); Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: command buffers created");
    createSyncObjects();    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: sync objects created");
    createShadowResources();Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: shadow resources created");
    createScenePipeline();  Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: scene pipeline created");
    createCube();           Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: initialized successfully");
    createPostFXPipelines();
	m_shaderManager = VulkanShaderManager();
}

void VulkanRenderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: shutdown — waiting for GPU");
    if (m_device) vkDeviceWaitIdle(m_device);
    destroyPostFXResources();
    destroyPostFXPipelines();
    // Destroy secondary windows first
    for (auto& [sdlWin, wd] : m_extraWindows)
        destroyWindowData(wd);
    m_extraWindows.clear();
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_frameFence[i])  vkDestroyFence    (m_device, m_frameFence[i],  nullptr);
        if (m_renderDone[i])  vkDestroySemaphore(m_device, m_renderDone[i],  nullptr);
        if (m_imageReady[i])  vkDestroySemaphore(m_device, m_imageReady[i],  nullptr);
    }
    // Scene resources
    for (auto& [id, mesh] : m_meshCache)
    {
        if (mesh.vbuf) vkDestroyBuffer(m_device, mesh.vbuf, nullptr);
        if (mesh.vmem) vkFreeMemory   (m_device, mesh.vmem, nullptr);
        if (mesh.ibuf) vkDestroyBuffer(m_device, mesh.ibuf, nullptr);
        if (mesh.imem) vkFreeMemory   (m_device, mesh.imem, nullptr);
    }
    m_meshCache.clear();
    if (m_cube.vbuf) vkDestroyBuffer(m_device, m_cube.vbuf, nullptr);
    if (m_cube.vmem) vkFreeMemory   (m_device, m_cube.vmem, nullptr);
    if (m_cube.ibuf) vkDestroyBuffer(m_device, m_cube.ibuf, nullptr);
    if (m_cube.imem) vkFreeMemory   (m_device, m_cube.imem, nullptr);
    m_cube = {};
    destroyScenePipeline();

    destroyViewportResources();
    destroyShadowResources();
    if (m_cmdPool)     vkDestroyCommandPool(m_device,   m_cmdPool,     nullptr);
    if (m_renderPass)  vkDestroyRenderPass (m_device,   m_renderPass,  nullptr);
    destroySwapchain();
    if (m_device)   vkDestroyDevice    (m_device,              nullptr);
    if (m_surface)  vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance) vkDestroyInstance  (m_instance,            nullptr);
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: all resources released");
}

void VulkanRenderer::Render()
{
    // Resize viewport resources if the editor requested a different size.
    if (m_viewportReqW > 0 && m_viewportReqH > 0 &&
        (m_viewportReqW != m_viewportW || m_viewportReqH != m_viewportH))
    {
        vkDeviceWaitIdle(m_device);
        createViewportResources(m_viewportReqW, m_viewportReqH);
    }

    const bool useViewport = m_viewportImage != VK_NULL_HANDLE
                          && m_viewportW > 0 && m_viewportH > 0;

    const uint32_t fi = m_currentFrame;

    vkWaitForFences(m_device, 1, &m_frameFence[fi], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                         m_imageReady[fi], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        vkCheck(acq, "vkAcquireNextImageKHR");

    vkResetFences(m_device, 1, &m_frameFence[fi]);

    VkCommandBuffer cmd = m_cmdBufs[imageIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // Shadow map first, in its own render pass (before the scene/swapchain pass).
    EncodeShadowMap(cmd);

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.18f, 0.18f, 0.20f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };

    if (useViewport)
    {
        const bool useHDR = m_postFxReady && m_hdrFB && m_ldrFB && m_fxaaFB;
        if (useHDR)
        {
            // ── Scene → HDR RT (RGBA16F) ───────────────────────────────────
            VkRenderPassBeginInfo hdrpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            hdrpbi.renderPass  = m_postFxSceneRP;
            hdrpbi.framebuffer = m_hdrFB;
            hdrpbi.renderArea.extent = { m_viewportW, m_viewportH };
            hdrpbi.clearValueCount   = 2;
            hdrpbi.pClearValues      = clears;
            vkCmdBeginRenderPass(cmd, &hdrpbi, VK_SUBPASS_CONTENTS_INLINE);
            DrawScene(cmd, m_viewportW, m_viewportH, /*hdr=*/true);
            vkCmdEndRenderPass(cmd);
            m_hdrLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // Transition HDR → SHADER_READ_ONLY for bloom bright pass.
            runPostFXBarrier(cmd, m_hdrImage,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            m_hdrLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Helper: run one fullscreen blit pass.
            auto blitPass = [&](VkRenderPass rp, VkFramebuffer fb, uint32_t bw, uint32_t bh,
                                VkPipeline pipe, VkDescriptorSet ds, const float params[4]) {
                VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                bi.renderPass=rp; bi.framebuffer=fb; bi.renderArea.extent={bw,bh};
                vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_postFxPipeLayout, 0, 1, &ds, 0, nullptr);
                vkCmdPushConstants(cmd, m_postFxPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, params);
                VkViewport vp{0,0,(float)bw,(float)bh,0,1}; vkCmdSetViewport(cmd,0,1,&vp);
                VkRect2D sc{{0,0},{bw,bh}}; vkCmdSetScissor(cmd,0,1,&sc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRenderPass(cmd);
            };

            const uint32_t bw = std::max(1u, m_viewportW/2), bh = std::max(1u, m_viewportH/2);

            // ── Bloom bright pass ──────────────────────────────────────────
            { const float p[4]={m_bloomThreshold,m_bloomKnee,0,0};
              blitPass(m_postFxBlitF16, m_bloomFB[0], bw, bh, m_bloomBrightPipe, m_postFxDS[0], p); }
            m_bloomLayout[0] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // ── 10 ping-pong blur passes ───────────────────────────────────
            bool horiz = true;
            for (int pass = 0; pass < 10; ++pass)
            {
                const int dst = horiz?1:0, src = horiz?0:1;
                runPostFXBarrier(cmd, m_bloomImage[src],
                    m_bloomLayout[src], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                m_bloomLayout[src] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                const float p[4]={1.0f/float(bw),1.0f/float(bh),horiz?1.0f:0.0f,0};
                blitPass(m_postFxBlitF16, m_bloomFB[dst], bw, bh, m_bloomBlurPipe,
                         m_postFxDS[1+src], p);
                m_bloomLayout[dst] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                horiz = !horiz;
            }
            // After 10 passes: result in bloom[0] (COLOR_ATTACHMENT_OPTIMAL).
            runPostFXBarrier(cmd, m_bloomImage[0],
                m_bloomLayout[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            m_bloomLayout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // ── Tonemap: hdr+bloom[0] → ldrFB ─────────────────────────────
            { const float p[4]={m_exposure, m_bloomStrength, 0, 0};
              blitPass(m_postFxBlitF8, m_ldrFB, m_viewportW, m_viewportH, m_tonemapPipe, m_postFxDS[3], p); }
            m_ldrLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            runPostFXBarrier(cmd, m_ldrImage,
                m_ldrLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            m_ldrLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // Transition viewportImage to COLOR_ATTACHMENT for FXAA output.
            runPostFXBarrier(cmd, m_viewportImage,
                m_viewportLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                m_viewportLayout==VK_IMAGE_LAYOUT_UNDEFINED
                    ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            m_viewportLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            // ── FXAA: ldrSRV → viewportFB ─────────────────────────────────
            { const float p[4]={1.0f/float(m_viewportW),1.0f/float(m_viewportH),0,0};
              blitPass(m_postFxFinalRP, m_fxaaFB, m_viewportW, m_viewportH, m_fxaaPipe, m_postFxDS[4], p); }
            m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // bloomRT[1] may still be in COLOR_ATTACHMENT_OPTIMAL — normalize for next frame.
            if (m_bloomLayout[1] == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                runPostFXBarrier(cmd, m_bloomImage[1],
                    m_bloomLayout[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                m_bloomLayout[1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        else
        {
            // ── Fallback: Scene → viewport RT (no PostFX) ─────────────────
            const bool fromUndefined = (m_viewportLayout == VK_IMAGE_LAYOUT_UNDEFINED);
            VkImageMemoryBarrier toColor{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toColor.oldLayout = m_viewportLayout; toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toColor.image = m_viewportImage; toColor.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toColor.srcAccessMask = fromUndefined ? 0u : (uint32_t)VK_ACCESS_SHADER_READ_BIT;
            toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                fromUndefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);
            m_viewportLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkRenderPassBeginInfo vrpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            vrpbi.renderPass = m_viewportRenderPass; vrpbi.framebuffer = m_viewportFramebuffer;
            vrpbi.renderArea.extent = { m_viewportW, m_viewportH };
            vrpbi.clearValueCount = 2; vrpbi.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &vrpbi, VK_SUBPASS_CONTENTS_INLINE);
            DrawScene(cmd, m_viewportW, m_viewportH);
            vkCmdEndRenderPass(cmd);
            m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    // ── Swapchain render pass: scene (non-viewport mode) + ImGui overlay ────
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = m_renderPass;
    rpbi.framebuffer       = m_framebuffers[imageIndex];
    rpbi.renderArea.extent = m_swapExtent;
    rpbi.clearValueCount   = 2;
    rpbi.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    if (!useViewport)
        DrawScene(cmd, m_swapExtent.width, m_swapExtent.height);
    if (m_overlayCallback) m_overlayCallback(cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &m_imageReady[fi];
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &m_renderDone[fi];
    vkQueueSubmit(m_graphicsQueue, 1, &si, m_frameFence[fi]);

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderDone[fi];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &imageIndex;
    VkResult pres = vkQueuePresentKHR(m_graphicsQueue, &pi);
    // OUT_OF_DATE/SUBOPTIMAL after present: the image was still shown, but the
    // swapchain no longer matches the surface — rebuild for the next frame.
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
        recreateSwapchain();
    else if (pres != VK_SUCCESS)
        vkCheck(pres, "vkQueuePresentKHR");

    m_currentFrame = (m_currentFrame + 1) % k_maxFramesInFlight;
}

IRenderer::Capabilities VulkanRenderer::GetCapabilities() const { return { true, m_postFxReady, false }; }

void*    VulkanRenderer::GetInstance()       const { return static_cast<void*>(m_instance); }
void*    VulkanRenderer::GetPhysicalDevice() const { return static_cast<void*>(m_physDevice); }
void*    VulkanRenderer::GetDevice()         const { return static_cast<void*>(m_device); }
void*    VulkanRenderer::GetQueue()          const { return static_cast<void*>(m_graphicsQueue); }
uint64_t VulkanRenderer::GetRenderPass() const
{
    uint64_t v = 0;
    static_assert(sizeof(m_renderPass) == sizeof(v), "VkRenderPass size mismatch");
    std::memcpy(&v, &m_renderPass, sizeof(v));
    return v;
}
uint32_t VulkanRenderer::GetQueueFamily()    const { return m_graphicsFamily; }
uint32_t VulkanRenderer::GetImageCount()     const { return static_cast<uint32_t>(m_swapImages.size()); }

void VulkanRenderer::SetVSync(bool enabled)
{
    if (m_vsync == enabled) return;
    Logger::Log(Logger::LogLevel::Info, enabled ? "VulkanRenderer: VSync enabled — recreating swapchain" : "VulkanRenderer: VSync disabled — recreating swapchain");
    m_vsync = enabled;
    if (!m_device || !m_swapchain) return;

    recreateSwapchain();   // picks up the new present mode via m_vsync
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: swapchain recreated");
}

void VulkanRenderer::createInstance()
{
    uint32_t extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    VkApplicationInfo appInfo{};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "HorizonEngine";
    appInfo.apiVersion       = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = extCount;
    ci.ppEnabledExtensionNames = sdlExts;
    vkCheck(vkCreateInstance(&ci, nullptr, &m_instance), "vkCreateInstance");
}

void VulkanRenderer::createSurface()
{
    if (!SDL_Vulkan_CreateSurface(m_sdlWindow, m_instance, nullptr, &m_surface))
        throw std::runtime_error("Vulkan: SDL_Vulkan_CreateSurface failed");
}

void VulkanRenderer::pickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("Vulkan: no GPU found");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devs.data());
    m_physDevice = devs[0];
}

void VulkanRenderer::createDevice()
{
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qProps(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &qCount, qProps.data());
    for (uint32_t i = 0; i < qCount; ++i)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physDevice, i, m_surface, &present);
        if ((qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        { m_graphicsFamily = i; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_graphicsFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = devExts;
    vkCheck(vkCreateDevice(m_physDevice, &dci, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
}

void VulkanRenderer::createSwapchain(uint32_t w, uint32_t h)
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, m_surface, &caps);
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, m_surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
    m_swapFormat = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX)
        m_swapExtent = caps.currentExtent;
    else
        m_swapExtent = { std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  w)),
                         std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, h)) };
    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = m_surface;
    sci.minImageCount    = std::max(2u, caps.minImageCount);
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = m_swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = m_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    sci.clipped          = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(m_device, &sci, nullptr, &m_swapchain), "vkCreateSwapchainKHR");
    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, nullptr);
    m_swapImages.resize(imgCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imgCount, m_swapImages.data());
    m_swapViews.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i)
    {
        VkImageViewCreateInfo vci{};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = m_swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = m_swapFormat;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_swapViews[i]), "vkCreateImageView");
    }
    createDepthResources();
}

void VulkanRenderer::createDepthResources()
{
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = m_depthFormat;
    ici.extent        = { m_swapExtent.width, m_swapExtent.height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_depthImage), "vkCreateImage depth");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_depthImage, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_depthMemory), "vkAllocateMemory depth");
    vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0);

    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = m_depthImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = m_depthFormat;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_depthView), "vkCreateImageView depth");
}

void VulkanRenderer::destroyDepthResources()
{
    if (m_depthView)   { vkDestroyImageView(m_device, m_depthView, nullptr);   m_depthView = VK_NULL_HANDLE; }
    if (m_depthImage)  { vkDestroyImage    (m_device, m_depthImage, nullptr);  m_depthImage = VK_NULL_HANDLE; }
    if (m_depthMemory) { vkFreeMemory      (m_device, m_depthMemory, nullptr); m_depthMemory = VK_NULL_HANDLE; }
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_physDevice, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("Vulkan: no suitable memory type");
}

void VulkanRenderer::createRenderPass()
{
    VkAttachmentDescription attachments[2]{};
    VkAttachmentDescription& color = attachments[0];
    color.format         = m_swapFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription& depth = attachments[1];
    depth.format         = m_depthFormat;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = 1;
    sub.pColorAttachments       = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstSubpass    = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments    = attachments;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;
    vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_renderPass), "vkCreateRenderPass");
}

void VulkanRenderer::createFramebuffers()
{
    m_framebuffers.resize(m_swapViews.size());
    for (size_t i = 0; i < m_swapViews.size(); ++i)
    {
        VkImageView attachments[2] = { m_swapViews[i], m_depthView };
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = m_renderPass;
        fci.attachmentCount = 2;
        fci.pAttachments    = attachments;
        fci.width           = m_swapExtent.width;
        fci.height          = m_swapExtent.height;
        fci.layers          = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_framebuffers[i]), "vkCreateFramebuffer");
    }
}

void VulkanRenderer::createCommandBuffers()
{
    // Idempotent: keep the pool across swapchain recreations (recreating it each
    // time without destroying the old one leaks a pool per resize). Only the
    // command buffers are freed and reallocated for the new image count.
    if (!m_cmdPool)
    {
        VkCommandPoolCreateInfo cpci{};
        cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = m_graphicsFamily;
        cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCheck(vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmdPool), "vkCreateCommandPool");
    }
    if (!m_cmdBufs.empty())
    {
        vkFreeCommandBuffers(m_device, m_cmdPool,
                             static_cast<uint32_t>(m_cmdBufs.size()), m_cmdBufs.data());
        m_cmdBufs.clear();
    }
    m_cmdBufs.resize(m_framebuffers.size());
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = static_cast<uint32_t>(m_cmdBufs.size());
    vkCheck(vkAllocateCommandBuffers(m_device, &cbai, m_cmdBufs.data()), "vkAllocateCommandBuffers");
}

void VulkanRenderer::createSyncObjects()
{
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        vkCheck(vkCreateSemaphore(m_device, &sci, nullptr, &m_imageReady[i]), "imageReady");
        vkCheck(vkCreateSemaphore(m_device, &sci, nullptr, &m_renderDone[i]), "renderDone");
        vkCheck(vkCreateFence    (m_device, &fci, nullptr, &m_frameFence[i]), "frameFence");
    }
}

void VulkanRenderer::destroySwapchain()
{
    destroyDepthResources();
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    for (auto iv : m_swapViews)    vkDestroyImageView  (m_device, iv, nullptr);
    m_framebuffers.clear();
    m_swapViews.clear();
    m_swapImages.clear();
    if (m_swapchain) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
}

void VulkanRenderer::recreateSwapchain()
{
    if (!m_device || !m_sdlWindow) return;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_sdlWindow, &w, &h);
    // Window minimised — keep the current (out-of-date) swapchain and retry on a
    // later frame once it has a non-zero size again. Recreating at 0×0 is invalid.
    if (w <= 0 || h <= 0) return;

    vkDeviceWaitIdle(m_device);
    destroySwapchain();                                  // also frees depth + framebuffers
    createSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    createFramebuffers();
    createCommandBuffers();                              // pool kept, buffers reallocated
}

// ─── Multi-window helpers ────────────────────────────────────────────────────

void VulkanRenderer::createWindowData(SDL_Window* sdlWin, uint32_t w, uint32_t h, WindowData& wd)
{
    // Surface
    if (!SDL_Vulkan_CreateSurface(sdlWin, m_instance, nullptr, &wd.surface))
        throw std::runtime_error("Vulkan: SDL_Vulkan_CreateSurface failed for secondary window");

    // Swapchain
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDevice, wd.surface, &caps);
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, wd.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDevice, wd.surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
    wd.swapFormat = chosen.format;
    if (caps.currentExtent.width != UINT32_MAX)
        wd.swapExtent = caps.currentExtent;
    else
        wd.swapExtent = { std::max(caps.minImageExtent.width,  std::min(caps.maxImageExtent.width,  w)),
                          std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, h)) };
    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = wd.surface;
    sci.minImageCount    = std::max(2u, caps.minImageCount);
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = wd.swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = m_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    sci.clipped          = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(m_device, &sci, nullptr, &wd.swapchain), "secondary vkCreateSwapchainKHR");
    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(m_device, wd.swapchain, &imgCount, nullptr);
    wd.swapImages.resize(imgCount);
    vkGetSwapchainImagesKHR(m_device, wd.swapchain, &imgCount, wd.swapImages.data());
    wd.swapViews.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i)
    {
        VkImageViewCreateInfo vci{};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = wd.swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = wd.swapFormat;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &wd.swapViews[i]), "secondary vkCreateImageView");
    }

    // RenderPass (same layout as primary)
    VkAttachmentDescription color{};
    color.format         = wd.swapFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstSubpass    = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &color;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;
    vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &wd.renderPass), "secondary vkCreateRenderPass");

    // Framebuffers
    wd.framebuffers.resize(wd.swapViews.size());
    for (size_t i = 0; i < wd.swapViews.size(); ++i)
    {
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = wd.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &wd.swapViews[i];
        fci.width           = wd.swapExtent.width;
        fci.height          = wd.swapExtent.height;
        fci.layers          = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &wd.framebuffers[i]), "secondary vkCreateFramebuffer");
    }

    // Command pool + buffers
    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = m_graphicsFamily;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(m_device, &cpci, nullptr, &wd.cmdPool), "secondary vkCreateCommandPool");
    wd.cmdBufs.resize(wd.framebuffers.size());
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = wd.cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = static_cast<uint32_t>(wd.cmdBufs.size());
    vkCheck(vkAllocateCommandBuffers(m_device, &cbai, wd.cmdBufs.data()), "secondary vkAllocateCommandBuffers");

    // Sync objects
    VkSemaphoreCreateInfo semCI{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fenCI{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        vkCheck(vkCreateSemaphore(m_device, &semCI, nullptr, &wd.imageReady[i]), "secondary imageReady");
        vkCheck(vkCreateSemaphore(m_device, &semCI, nullptr, &wd.renderDone[i]), "secondary renderDone");
        vkCheck(vkCreateFence    (m_device, &fenCI, nullptr, &wd.frameFence[i]), "secondary frameFence");
    }
}

void VulkanRenderer::destroyWindowData(WindowData& wd)
{
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (wd.frameFence[i])  vkDestroyFence    (m_device, wd.frameFence[i],  nullptr);
        if (wd.renderDone[i])  vkDestroySemaphore(m_device, wd.renderDone[i],  nullptr);
        if (wd.imageReady[i])  vkDestroySemaphore(m_device, wd.imageReady[i],  nullptr);
    }
    if (wd.cmdPool) vkDestroyCommandPool(m_device, wd.cmdPool, nullptr);
    if (wd.renderPass) vkDestroyRenderPass(m_device, wd.renderPass, nullptr);
    for (auto fb : wd.framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    for (auto iv : wd.swapViews)    vkDestroyImageView  (m_device, iv, nullptr);
    if (wd.swapchain) vkDestroySwapchainKHR(m_device, wd.swapchain, nullptr);
    if (wd.surface)   vkDestroySurfaceKHR  (m_instance, wd.surface, nullptr);
    wd = {};
}

void VulkanRenderer::renderWindowData(WindowData& wd)
{
    const uint32_t fi = wd.currentFrame;
    vkWaitForFences(m_device, 1, &wd.frameFence[fi], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(m_device, wd.swapchain, UINT64_MAX,
                                         wd.imageReady[fi], VK_NULL_HANDLE, &imageIndex);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        vkCheck(acq, "secondary vkAcquireNextImageKHR");

    vkResetFences(m_device, 1, &wd.frameFence[fi]);

    VkCommandBuffer cmd = wd.cmdBufs[imageIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clear{};
    clear.color = { { 0.18f, 0.18f, 0.20f, 1.0f } };
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = wd.renderPass;
    rpbi.framebuffer       = wd.framebuffers[imageIndex];
    rpbi.renderArea.extent = wd.swapExtent;
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    // Overlay injection point for secondary windows (same context pointer convention)
    if (m_overlayCallback) m_overlayCallback(static_cast<void*>(&cmd));
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &wd.imageReady[fi];
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &wd.renderDone[fi];
    vkQueueSubmit(m_graphicsQueue, 1, &si, wd.frameFence[fi]);

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &wd.renderDone[fi];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &wd.swapchain;
    pi.pImageIndices      = &imageIndex;
    vkQueuePresentKHR(m_graphicsQueue, &pi);

    wd.currentFrame = (wd.currentFrame + 1) % k_maxFramesInFlight;
}

// ─── IRenderer multi-window virtuals ─────────────────────────────────────────

void VulkanRenderer::AttachWindow(HE::Window* window)
{
    SDL_Window* sdlWin = window->GetNativeWindow();
    if (m_extraWindows.count(sdlWin)) return; // already attached
    WindowData& wd = m_extraWindows[sdlWin];
    createWindowData(sdlWin, window->GetWidth(), window->GetHeight(), wd);
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: secondary window attached");
}

void VulkanRenderer::DetachWindow(HE::Window* window)
{
    auto it = m_extraWindows.find(window->GetNativeWindow());
    if (it == m_extraWindows.end()) return;
    destroyWindowData(it->second);
    m_extraWindows.erase(it);
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: secondary window detached");
}

void VulkanRenderer::RenderWindow(HE::Window* window)
{
    auto it = m_extraWindows.find(window->GetNativeWindow());
    if (it == m_extraWindows.end()) return;
    renderWindowData(it->second);
}

// ─── Scene draw path ─────────────────────────────────────────────────────────

VkShaderModule VulkanRenderer::loadShaderModule(const char* spvFileName)
{
    // SPIR-V is loaded from <exe dir>/Shaders/<name>.spv. The .spv files are
    // produced from src/HE_Rendering/shaders/*.{vert,frag} by glslc at build
    // time (see CMakeLists) — they cannot be generated on a machine without a
    // Vulkan/glslc toolchain, which is also why this backend isn't built there.
    const char* base = SDL_GetBasePath();
    std::string path = std::string(base ? base : "") + "Shaders/" + spvFileName;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        Logger::Log(Logger::LogLevel::Warning, ("VulkanRenderer: shader not found — " + path).c_str());
        return VK_NULL_HANDLE;
    }
    const size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> code(size);
    f.seekg(0); f.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

void VulkanRenderer::createScenePipeline()
{
    // Descriptor set: binding 0 = per-frame UBO, binding 1 = shadow map, binding 2 = per-draw material UBO.
    VkDescriptorSetLayoutBinding binds[3]{};
    binds[0].binding         = 0;
    binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding         = 1;
    binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].binding         = 2;
    binds[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[2].descriptorCount = 1;
    binds[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 3;
    slci.pBindings    = binds;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_sceneSetLayout), "descriptor set layout");

    // Create the per-draw material UBO (32 bytes, host-coherent, written per-draw via vkCmdUpdateBuffer).
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = 32;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &m_matUBO), "mat ubo");
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, m_matUBO, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_matMem), "mat ubo mem");
        vkBindBufferMemory(m_device, m_matUBO, m_matMem, 0);
    }

    // Pipeline layout: the set above + per-object push constants (MVP + model).
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_sceneSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_scenePipelineLayout), "pipeline layout");

    // Per-frame UBO buffers + descriptor sets (one per frame in flight).
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 2 },  // binding0 + binding2
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = ps;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_descPool), "descriptor pool");

    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = sizeof(FrameUBOData);
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &m_frameUBO[i].buf), "ubo buffer");
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, m_frameUBO[i].buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_frameUBO[i].mem), "ubo memory");
        vkBindBufferMemory(m_device, m_frameUBO[i].buf, m_frameUBO[i].mem, 0);
        vkMapMemory(m_device, m_frameUBO[i].mem, 0, sizeof(FrameUBOData), 0, &m_frameUBO[i].mapped);

        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_sceneSetLayout;
        vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &m_frameUBO[i].set), "descriptor set");

        VkDescriptorBufferInfo dbi{ m_frameUBO[i].buf, 0, sizeof(FrameUBOData) };
        VkDescriptorImageInfo  dii{ m_shadowSampler, m_shadowView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo matDbi{ m_matUBO, 0, 32 };
        VkWriteDescriptorSet w[3]{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = m_frameUBO[i].set;
        w[0].dstBinding      = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo     = &dbi;
        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = m_frameUBO[i].set;
        w[1].dstBinding      = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo      = &dii;
        w[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet          = m_frameUBO[i].set;
        w[2].dstBinding      = 2;
        w[2].descriptorCount = 1;
        w[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[2].pBufferInfo     = &matDbi;
        vkUpdateDescriptorSets(m_device, m_shadowView ? 3 : 2, w, 0, nullptr);
    }

    VkShaderModule vs = loadShaderModule("scene.vert.spv");
    VkShaderModule fs = loadShaderModule("scene.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    {
        Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: scene shaders missing — scene will not draw");
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        return; // m_scenePipeline stays null; clear-only still works
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription bind{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE; // meshes have no guaranteed winding
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = m_scenePipelineLayout;
    pci.renderPass          = m_renderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_scenePipeline) != VK_SUCCESS)
        Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: graphics pipeline creation failed");

    // Alpha-blend pipeline for sorted transparency (depth test, no depth write).
    {
        VkPipelineColorBlendAttachmentState tcba{};
        tcba.blendEnable         = VK_TRUE;
        tcba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        tcba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tcba.colorBlendOp        = VK_BLEND_OP_ADD;
        tcba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        tcba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        tcba.alphaBlendOp        = VK_BLEND_OP_ADD;
        tcba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo tcb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        tcb.attachmentCount = 1;
        tcb.pAttachments    = &tcba;

        VkPipelineDepthStencilStateCreateInfo tds = ds;
        tds.depthWriteEnable = VK_FALSE;

        VkGraphicsPipelineCreateInfo tpci = pci;
        tpci.pColorBlendState    = &tcb;
        tpci.pDepthStencilState  = &tds;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &tpci, nullptr, &m_sceneTransparentPipeline) != VK_SUCCESS)
            Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: transparent pipeline creation failed");
    }

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);

    // ── Shadow pipeline: depth-only, into the shadow render pass ────────────
    if (m_shadowPass)
    {
        VkShaderModule svs = loadShaderModule("scene_shadow.vert.spv");
        if (svs != VK_NULL_HANDLE)
        {
            VkPipelineShaderStageCreateInfo sstage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            sstage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
            sstage.module = svs;
            sstage.pName  = "main";

            VkGraphicsPipelineCreateInfo spci = pci; // reuse vertex input / depth / dynamic state
            spci.stageCount      = 1;            // vertex only — depth-only pass
            spci.pStages         = &sstage;
            spci.pColorBlendState = nullptr;     // no color attachment
            spci.renderPass      = m_shadowPass;
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &spci, nullptr, &m_shadowPipeline) != VK_SUCCESS)
                Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: shadow pipeline creation failed");
            vkDestroyShaderModule(m_device, svs, nullptr);
        }
    }
}

void VulkanRenderer::destroyScenePipeline()
{
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_frameUBO[i].buf) vkDestroyBuffer(m_device, m_frameUBO[i].buf, nullptr);
        if (m_frameUBO[i].mem) vkFreeMemory   (m_device, m_frameUBO[i].mem, nullptr);
        m_frameUBO[i] = {};
    }
    if (m_matUBO)              { vkDestroyBuffer(m_device, m_matUBO, nullptr); m_matUBO = VK_NULL_HANDLE; }
    if (m_matMem)              { vkFreeMemory   (m_device, m_matMem, nullptr); m_matMem = VK_NULL_HANDLE; }
    if (m_descPool)            { vkDestroyDescriptorPool(m_device, m_descPool, nullptr);            m_descPool = VK_NULL_HANDLE; }
    if (m_shadowPipeline)                { vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);                m_shadowPipeline = VK_NULL_HANDLE; }
    if (m_sceneTransparentPipelineHDR)   { vkDestroyPipeline(m_device, m_sceneTransparentPipelineHDR, nullptr);   m_sceneTransparentPipelineHDR = VK_NULL_HANDLE; }
    if (m_scenePipelineHDR)              { vkDestroyPipeline(m_device, m_scenePipelineHDR, nullptr);              m_scenePipelineHDR = VK_NULL_HANDLE; }
    if (m_sceneTransparentPipeline)      { vkDestroyPipeline(m_device, m_sceneTransparentPipeline, nullptr);      m_sceneTransparentPipeline = VK_NULL_HANDLE; }
    if (m_scenePipeline)                 { vkDestroyPipeline(m_device, m_scenePipeline, nullptr);                 m_scenePipeline = VK_NULL_HANDLE; }
    if (m_scenePipelineLayout) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
    if (m_sceneSetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_sceneSetLayout, nullptr); m_sceneSetLayout = VK_NULL_HANDLE; }
}

void VulkanRenderer::createShadowResources()
{
    // Depth image, sampled by the scene pass.
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = m_depthFormat;
    ici.extent      = { m_shadowSize, m_shadowSize, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_shadowImage), "shadow image");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_shadowImage, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_shadowMemory), "shadow memory");
    vkBindImageMemory(m_device, m_shadowImage, m_shadowMemory, 0);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image    = m_shadowImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = m_depthFormat;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_shadowView), "shadow view");

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside the map = lit
    vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_shadowSampler), "shadow sampler");

    // Depth-only render pass; final layout is shader-readable for sampling.
    VkAttachmentDescription depth{};
    depth.format         = m_depthFormat;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &depth;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies   = deps;
    vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_shadowPass), "shadow render pass");

    VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass      = m_shadowPass;
    fci.attachmentCount = 1;
    fci.pAttachments    = &m_shadowView;
    fci.width           = m_shadowSize;
    fci.height          = m_shadowSize;
    fci.layers          = 1;
    vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_shadowFB), "shadow framebuffer");
}

void VulkanRenderer::destroyShadowResources()
{
    if (m_shadowFB)      { vkDestroyFramebuffer(m_device, m_shadowFB, nullptr);   m_shadowFB = VK_NULL_HANDLE; }
    if (m_shadowPass)    { vkDestroyRenderPass (m_device, m_shadowPass, nullptr); m_shadowPass = VK_NULL_HANDLE; }
    if (m_shadowSampler) { vkDestroySampler    (m_device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
    if (m_shadowView)    { vkDestroyImageView  (m_device, m_shadowView, nullptr); m_shadowView = VK_NULL_HANDLE; }
    if (m_shadowImage)   { vkDestroyImage      (m_device, m_shadowImage, nullptr); m_shadowImage = VK_NULL_HANDLE; }
    if (m_shadowMemory)  { vkFreeMemory        (m_device, m_shadowMemory, nullptr); m_shadowMemory = VK_NULL_HANDLE; }
}

// ─── PostFX pipeline ──────────────────────────────────────────────────────────

void VulkanRenderer::runPostFXBarrier(VkCommandBuffer cmd, VkImage img,
    VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    if (from == to) return;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (from == VK_IMAGE_LAYOUT_UNDEFINED)
        b.srcAccessMask = 0;
    else if (from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    else if (from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    if (to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    else if (to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRenderer::createPostFXPipelines()
{
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_postFxSampler), "postfx sampler");

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[0].pImmutableSamplers = &m_postFxSampler;
    binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].pImmutableSamplers = &m_postFxSampler;
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 2; slci.pBindings = binds;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_postFxDSLayout), "postfx dsl");

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 5; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_postFxDSPool), "postfx pool");

    VkDescriptorSetLayout layouts[5]; for (auto& l : layouts) l = m_postFxDSLayout;
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = m_postFxDSPool; dsai.descriptorSetCount = 5; dsai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(m_device, &dsai, m_postFxDS), "postfx ds alloc");

    // Dummy 1×1 RGBA8 image for unused binding slots.
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {1,1,1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_dummyImage), "dummy image");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, m_dummyImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_dummyMemory), "dummy memory");
        vkBindImageMemory(m_device, m_dummyImage, m_dummyMemory, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_dummyImage; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_dummyView), "dummy view");
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_dummyImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = 16;
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &m_postFxDSLayout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_postFxPipeLayout), "postfx pipe layout");

    // ── Render passes ─────────────────────────────────────────────────────────
    auto makeBlitRP = [&](VkFormat fmt, VkImageLayout finalLayout, VkRenderPass& rp) {
        VkAttachmentDescription att{};
        att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = finalLayout;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &rp), "postfx blit render pass");
    };

    // Scene pass for HDR: RGBA16F color + depth.
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format = VK_FORMAT_R16G16B16A16_SFLOAT; atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[1].format = m_depthFormat; atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2; rpci.pAttachments = atts;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_postFxSceneRP), "postfx scene render pass");
    }

    makeBlitRP(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, m_postFxBlitF16);
    makeBlitRP(VK_FORMAT_R8G8B8A8_UNORM,      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, m_postFxBlitF8);
    makeBlitRP(VK_FORMAT_R8G8B8A8_UNORM,      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_postFxFinalRP);

    // ── Pipelines ─────────────────────────────────────────────────────────────
    VkShaderModule vsM  = loadShaderModule("postfx.vert.spv");
    VkShaderModule tmFS = loadShaderModule("postfx_tonemap.frag.spv");
    VkShaderModule fxFS = loadShaderModule("postfx_fxaa.frag.spv");
    VkShaderModule brFS = loadShaderModule("postfx_bloom_bright.frag.spv");
    VkShaderModule blFS = loadShaderModule("postfx_bloom_blur.frag.spv");

    if (!vsM || !tmFS || !fxFS || !brFS || !blFS)
    {
        Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: PostFX shaders missing — no HDR/bloom/FXAA");
        for (auto m : {vsM,tmFS,fxFS,brFS,blFS}) if (m) vkDestroyShaderModule(m_device,m,nullptr);
        return;
    }

    auto makePipe = [&](VkShaderModule fs, VkRenderPass rp, VkPipeline& out) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsM; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2; pci.pStages = stages;
        pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp; pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
        pci.pDynamicState = &dyn; pci.layout = m_postFxPipeLayout;
        pci.renderPass = rp; pci.subpass = 0;
        vkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out), "postfx pipeline");
    };

    makePipe(brFS, m_postFxBlitF16, m_bloomBrightPipe);
    makePipe(blFS, m_postFxBlitF16, m_bloomBlurPipe);
    makePipe(tmFS, m_postFxBlitF8,  m_tonemapPipe);
    makePipe(fxFS, m_postFxFinalRP, m_fxaaPipe);

    for (auto m : {vsM,tmFS,fxFS,brFS,blFS}) vkDestroyShaderModule(m_device,m,nullptr);

    m_postFxReady = true;
    // ── HDR-compatible scene pipelines ────────────────────────────────────────
    // m_renderPass (swapchain format) and m_postFxSceneRP (RGBA16F) are
    // incompatible render passes in Vulkan. We need matching pipelines for the
    // HDR path. Rebuild with identical state; only renderPass differs.
    {
        VkShaderModule vs2 = loadShaderModule("scene.vert.spv");
        VkShaderModule fs2 = loadShaderModule("scene.frag.spv");
        if (vs2 && fs2)
        {
            VkPipelineShaderStageCreateInfo st[2]{};
            st[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;   st[0].module=vs2; st[0].pName="main";
            st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=fs2; st[1].pName="main";
            VkVertexInputBindingDescription vib{ 0, 8u*sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
            VkVertexInputAttributeDescription vias[3]={ {0,0,VK_FORMAT_R32G32B32_SFLOAT,0}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,12}, {2,0,VK_FORMAT_R32G32_SFLOAT,24} };
            VkPipelineVertexInputStateCreateInfo vi2{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi2.vertexBindingDescriptionCount=1; vi2.pVertexBindingDescriptions=&vib;
            vi2.vertexAttributeDescriptionCount=3; vi2.pVertexAttributeDescriptions=vias;
            VkPipelineInputAssemblyStateCreateInfo ia2{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia2.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp2{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp2.viewportCount=1; vp2.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo rs2{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs2.polygonMode=VK_POLYGON_MODE_FILL; rs2.cullMode=VK_CULL_MODE_NONE; rs2.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs2.lineWidth=1.0f;
            VkPipelineMultisampleStateCreateInfo ms2{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms2.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds2{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds2.depthTestEnable=VK_TRUE; ds2.depthWriteEnable=VK_TRUE; ds2.depthCompareOp=VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState cba2{};
            cba2.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb2{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb2.attachmentCount=1; cb2.pAttachments=&cba2;
            VkDynamicState dynSt[]={ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dyn2{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dyn2.dynamicStateCount=2; dyn2.pDynamicStates=dynSt;
            VkGraphicsPipelineCreateInfo pci2{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pci2.stageCount=2; pci2.pStages=st; pci2.pVertexInputState=&vi2; pci2.pInputAssemblyState=&ia2;
            pci2.pViewportState=&vp2; pci2.pRasterizationState=&rs2; pci2.pMultisampleState=&ms2;
            pci2.pDepthStencilState=&ds2; pci2.pColorBlendState=&cb2; pci2.pDynamicState=&dyn2;
            pci2.layout=m_scenePipelineLayout; pci2.renderPass=m_postFxSceneRP;
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci2, nullptr, &m_scenePipelineHDR) != VK_SUCCESS)
                Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: HDR scene pipeline failed");
            // Alpha-blend transparent variant
            {
                VkPipelineColorBlendAttachmentState tcba2=cba2;
                tcba2.blendEnable=VK_TRUE;
                tcba2.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; tcba2.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                tcba2.colorBlendOp=VK_BLEND_OP_ADD;
                tcba2.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; tcba2.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO; tcba2.alphaBlendOp=VK_BLEND_OP_ADD;
                VkPipelineColorBlendStateCreateInfo tcb2=cb2; tcb2.pAttachments=&tcba2;
                VkPipelineDepthStencilStateCreateInfo tds2=ds2; tds2.depthWriteEnable=VK_FALSE;
                VkGraphicsPipelineCreateInfo tpci2=pci2; tpci2.pColorBlendState=&tcb2; tpci2.pDepthStencilState=&tds2;
                if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &tpci2, nullptr, &m_sceneTransparentPipelineHDR) != VK_SUCCESS)
                    Logger::Log(Logger::LogLevel::Error, "VulkanRenderer: HDR transparent pipeline failed");
            }
            vkDestroyShaderModule(m_device, vs2, nullptr);
            vkDestroyShaderModule(m_device, fs2, nullptr);
        }
    }

    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: PostFX pipelines created");
}

void VulkanRenderer::destroyPostFXPipelines()
{
    m_postFxReady = false;
    for (auto p : {m_bloomBrightPipe,m_bloomBlurPipe,m_tonemapPipe,m_fxaaPipe})
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    m_bloomBrightPipe=m_bloomBlurPipe=m_tonemapPipe=m_fxaaPipe=VK_NULL_HANDLE;
    if (m_postFxPipeLayout) { vkDestroyPipelineLayout(m_device, m_postFxPipeLayout, nullptr); m_postFxPipeLayout=VK_NULL_HANDLE; }
    if (m_postFxDSPool)     { vkDestroyDescriptorPool(m_device, m_postFxDSPool, nullptr); m_postFxDSPool=VK_NULL_HANDLE; }
    if (m_postFxDSLayout)   { vkDestroyDescriptorSetLayout(m_device, m_postFxDSLayout, nullptr); m_postFxDSLayout=VK_NULL_HANDLE; }
    if (m_postFxSampler)    { vkDestroySampler(m_device, m_postFxSampler, nullptr); m_postFxSampler=VK_NULL_HANDLE; }
    if (m_dummyView)        { vkDestroyImageView(m_device, m_dummyView, nullptr); m_dummyView=VK_NULL_HANDLE; }
    if (m_dummyImage)       { vkDestroyImage(m_device, m_dummyImage, nullptr); m_dummyImage=VK_NULL_HANDLE; }
    if (m_dummyMemory)      { vkFreeMemory(m_device, m_dummyMemory, nullptr); m_dummyMemory=VK_NULL_HANDLE; }
    for (auto rp : {m_postFxSceneRP,m_postFxBlitF16,m_postFxBlitF8,m_postFxFinalRP})
        if (rp) vkDestroyRenderPass(m_device, rp, nullptr);
    m_postFxSceneRP=m_postFxBlitF16=m_postFxBlitF8=m_postFxFinalRP=VK_NULL_HANDLE;
}

void VulkanRenderer::createPostFXResources(uint32_t w, uint32_t h)
{
    destroyPostFXResources();
    if (!m_postFxReady || !m_postFxSampler) return;

    auto makeImg = [&](VkFormat fmt, uint32_t iw, uint32_t ih, VkImageUsageFlags usage,
                       VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = {iw,ih,1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.usage = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &img), "postfx image");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, img, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &mem), "postfx image mem");
        vkBindImageMemory(m_device, img, mem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &view), "postfx image view");
    };
    auto makeFB = [&](VkRenderPass rp, VkImageView v, uint32_t fw, uint32_t fh, VkFramebuffer& fb) {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &v;
        fci.width = fw; fci.height = fh; fci.layers = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &fb), "postfx framebuffer");
    };

    const VkImageUsageFlags RTf = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const uint32_t bw = std::max(1u, w/2), bh = std::max(1u, h/2);
    makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, w,  h,  RTf, m_hdrImage,     m_hdrMemory,     m_hdrView);
    makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, bw, bh, RTf, m_bloomImage[0],m_bloomMemory[0],m_bloomView[0]);
    makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, bw, bh, RTf, m_bloomImage[1],m_bloomMemory[1],m_bloomView[1]);
    makeImg(VK_FORMAT_R8G8B8A8_UNORM,      w,  h,  RTf, m_ldrImage,     m_ldrMemory,     m_ldrView);
    m_hdrLayout = m_bloomLayout[0] = m_bloomLayout[1] = m_ldrLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // HDR scene framebuffer needs hdrView + viewportDepthView.
    {
        VkImageView atts[] = { m_hdrView, m_viewportDepthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_postFxSceneRP; fci.attachmentCount = 2; fci.pAttachments = atts;
        fci.width = w; fci.height = h; fci.layers = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_hdrFB), "hdr scene fb");
    }
    makeFB(m_postFxBlitF16, m_bloomView[0], bw, bh, m_bloomFB[0]);
    makeFB(m_postFxBlitF16, m_bloomView[1], bw, bh, m_bloomFB[1]);
    makeFB(m_postFxBlitF8,  m_ldrView,      w,  h,  m_ldrFB);
    makeFB(m_postFxFinalRP, m_viewportView, w,  h,  m_fxaaFB);

    // Write descriptor sets: [0]=bloomBright, [1]=blurH(bloom[0]), [2]=blurV(bloom[1]),
    //                         [3]=tonemap, [4]=fxaa
    struct DSEntry { VkDescriptorSet set; VkImageView t0; VkImageView t1; };
    DSEntry dss[5] = {
        { m_postFxDS[0], m_hdrView,      m_dummyView    },
        { m_postFxDS[1], m_bloomView[0], m_dummyView    },
        { m_postFxDS[2], m_bloomView[1], m_dummyView    },
        { m_postFxDS[3], m_hdrView,      m_bloomView[0] },
        { m_postFxDS[4], m_ldrView,      m_dummyView    },
    };
    for (auto& d : dss)
    {
        VkDescriptorImageInfo ii0{ VK_NULL_HANDLE, d.t0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ii1{ VK_NULL_HANDLE, d.t1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = d.set;
        w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii0;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = d.set;
        w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &ii1;
        vkUpdateDescriptorSets(m_device, 2, w, 0, nullptr);
    }
}

void VulkanRenderer::destroyPostFXResources()
{
    vkDeviceWaitIdle(m_device);
    if (m_fxaaFB)     { vkDestroyFramebuffer(m_device, m_fxaaFB,     nullptr); m_fxaaFB=VK_NULL_HANDLE; }
    if (m_ldrFB)      { vkDestroyFramebuffer(m_device, m_ldrFB,      nullptr); m_ldrFB=VK_NULL_HANDLE; }
    if (m_bloomFB[1]) { vkDestroyFramebuffer(m_device, m_bloomFB[1], nullptr); m_bloomFB[1]=VK_NULL_HANDLE; }
    if (m_bloomFB[0]) { vkDestroyFramebuffer(m_device, m_bloomFB[0], nullptr); m_bloomFB[0]=VK_NULL_HANDLE; }
    if (m_hdrFB)      { vkDestroyFramebuffer(m_device, m_hdrFB,      nullptr); m_hdrFB=VK_NULL_HANDLE; }
    if (m_ldrView)    { vkDestroyImageView(m_device, m_ldrView,    nullptr); m_ldrView=VK_NULL_HANDLE; }
    if (m_ldrImage)   { vkDestroyImage    (m_device, m_ldrImage,   nullptr); m_ldrImage=VK_NULL_HANDLE; }
    if (m_ldrMemory)  { vkFreeMemory      (m_device, m_ldrMemory,  nullptr); m_ldrMemory=VK_NULL_HANDLE; }
    for (int i = 1; i >= 0; --i) {
        if (m_bloomView[i])   { vkDestroyImageView(m_device, m_bloomView[i],   nullptr); m_bloomView[i]=VK_NULL_HANDLE; }
        if (m_bloomImage[i])  { vkDestroyImage    (m_device, m_bloomImage[i],  nullptr); m_bloomImage[i]=VK_NULL_HANDLE; }
        if (m_bloomMemory[i]) { vkFreeMemory      (m_device, m_bloomMemory[i], nullptr); m_bloomMemory[i]=VK_NULL_HANDLE; }
    }
    if (m_hdrView)    { vkDestroyImageView(m_device, m_hdrView,    nullptr); m_hdrView=VK_NULL_HANDLE; }
    if (m_hdrImage)   { vkDestroyImage    (m_device, m_hdrImage,   nullptr); m_hdrImage=VK_NULL_HANDLE; }
    if (m_hdrMemory)  { vkFreeMemory      (m_device, m_hdrMemory,  nullptr); m_hdrMemory=VK_NULL_HANDLE; }
    m_hdrLayout = m_bloomLayout[0] = m_bloomLayout[1] = m_ldrLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ─── Viewport offscreen render target ─────────────────────────────────────────

void VulkanRenderer::destroyViewportResources()
{
    destroyPostFXResources();
    if (m_viewportFramebuffer) { vkDestroyFramebuffer(m_device, m_viewportFramebuffer, nullptr); m_viewportFramebuffer = VK_NULL_HANDLE; }
    if (m_viewportRenderPass)  { vkDestroyRenderPass (m_device, m_viewportRenderPass, nullptr);  m_viewportRenderPass = VK_NULL_HANDLE; }
    if (m_viewportSampler)     { vkDestroySampler    (m_device, m_viewportSampler, nullptr);     m_viewportSampler = VK_NULL_HANDLE; }
    if (m_viewportDepthView)   { vkDestroyImageView  (m_device, m_viewportDepthView, nullptr);   m_viewportDepthView = VK_NULL_HANDLE; }
    if (m_viewportDepthImage)  { vkDestroyImage      (m_device, m_viewportDepthImage, nullptr);  m_viewportDepthImage = VK_NULL_HANDLE; }
    if (m_viewportDepthMemory) { vkFreeMemory        (m_device, m_viewportDepthMemory, nullptr); m_viewportDepthMemory = VK_NULL_HANDLE; }
    if (m_viewportView)        { vkDestroyImageView  (m_device, m_viewportView, nullptr);        m_viewportView = VK_NULL_HANDLE; }
    if (m_viewportImage)       { vkDestroyImage      (m_device, m_viewportImage, nullptr);       m_viewportImage = VK_NULL_HANDLE; }
    if (m_viewportMemory)      { vkFreeMemory        (m_device, m_viewportMemory, nullptr);      m_viewportMemory = VK_NULL_HANDLE; }
    m_viewportW = m_viewportH = 0;
    m_viewportLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::createViewportResources(uint32_t w, uint32_t h)
{
    destroyViewportResources();

    // ── Color image (RGBA8, sampled by ImGui, written by scene render pass) ─
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent      = { w, h, 1 };
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_viewportImage), "viewport image");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_viewportImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_viewportMemory), "viewport memory");
        vkBindImageMemory(m_device, m_viewportImage, m_viewportMemory, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = m_viewportImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_viewportView), "viewport view");
    }

    // ── Depth image ──────────────────────────────────────────────────────────
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = m_depthFormat;
        ici.extent      = { w, h, 1 };
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_viewportDepthImage), "viewport depth image");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_viewportDepthImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_viewportDepthMemory), "viewport depth memory");
        vkBindImageMemory(m_device, m_viewportDepthImage, m_viewportDepthMemory, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = m_viewportDepthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = m_depthFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_viewportDepthView), "viewport depth view");
    }

    // ── Render pass: color → SHADER_READ_ONLY, depth → don't care ───────────
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = VK_FORMAT_R8G8B8A8_UNORM;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription depthAtt{};
        depthAtt.format         = m_depthFormat;
        depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 1;
        sub.pColorAttachments       = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkAttachmentDescription atts[2] = { colorAtt, depthAtt };
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2;
        rpci.pAttachments    = atts;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_viewportRenderPass), "viewport render pass");
    }

    // ── Framebuffer ──────────────────────────────────────────────────────────
    {
        VkImageView atts[2] = { m_viewportView, m_viewportDepthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_viewportRenderPass;
        fci.attachmentCount = 2;
        fci.pAttachments    = atts;
        fci.width           = w;
        fci.height          = h;
        fci.layers          = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_viewportFramebuffer), "viewport framebuffer");
    }

    // ── Sampler (for ImGui to sample the viewport texture) ───────────────────
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_viewportSampler), "viewport sampler");
    }

    m_viewportW          = w;
    m_viewportH          = h;
    m_viewportLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    m_viewportResChanged = true;

    createPostFXResources(w, h);
}

void  VulkanRenderer::SetViewportSize(uint32_t w, uint32_t h)
{
    m_viewportReqW = w;
    m_viewportReqH = h;
}

bool VulkanRenderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH)
{
    if (!m_viewportImage || m_viewportW == 0 || m_viewportH == 0) return false;

    const uint32_t w = m_viewportW;
    const uint32_t h = m_viewportH;
    const VkDeviceSize rowBytes   = static_cast<VkDeviceSize>(w) * 4;
    const VkDeviceSize totalBytes = rowBytes * h;

    // Create a staging buffer
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = totalBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, stagingBuf, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_device, &mai, nullptr, &stagingMem) != VK_SUCCESS)
    { vkDestroyBuffer(m_device, stagingBuf, nullptr); return false; }
    vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);

    // One-shot command buffer to copy image → staging
    vkDeviceWaitIdle(m_device);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer tmp = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &tmp);
    VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(tmp, &cbbi);

    // Transition image to TRANSFER_SRC
    VkImageMemoryBarrier toSrc{};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout           = m_viewportLayout;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = m_viewportImage;
    toSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toSrc.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { w, h, 1 };
    vkCmdCopyImageToBuffer(tmp, m_viewportImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition back
    VkImageMemoryBarrier toSR = toSrc;
    toSR.oldLayout      = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSR.newLayout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSR.srcAccessMask  = VK_ACCESS_TRANSFER_READ_BIT;
    toSR.dstAccessMask  = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSR);
    m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkEndCommandBuffer(tmp);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &tmp;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);

    // Map and copy pixels
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMem, 0, totalBytes, 0, &mapped);
    outW = w; outH = h;
    rgba.resize(static_cast<size_t>(totalBytes));
    std::memcpy(rgba.data(), mapped, static_cast<size_t>(totalBytes));
    vkUnmapMemory(m_device, stagingMem);

    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory   (m_device, stagingMem, nullptr);
    return true;
}

void* VulkanRenderer::GetViewportVkImageView() const { return static_cast<void*>(m_viewportView); }
void* VulkanRenderer::GetViewportVkSampler()   const { return static_cast<void*>(m_viewportSampler); }
bool  VulkanRenderer::HasViewportResourceChanged() const { return m_viewportResChanged; }
void  VulkanRenderer::ClearViewportResourceChanged()     { m_viewportResChanged = false; }

void VulkanRenderer::EncodeShadowMap(VkCommandBuffer cmd)
{
    if (!m_world || m_shadowPipeline == VK_NULL_HANDLE) return;

    m_extractor.extract(*m_world, m_renderWorld, 1.0f, &m_editorCamera);
    if (!m_renderWorld.shadow.enabled || m_renderWorld.objects.empty()) return;
    for (RenderObject& obj : m_renderWorld.objects)
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);
    // Cull casters against the LIGHT frustum, not the camera — an off-screen
    // object still casts a shadow into the visible scene while it lies within the
    // shadow map's coverage. (Camera-culling the casters made shadows pop out as
    // their caster left the screen.)
    m_culler.cull(m_renderWorld, m_renderWorld.shadow.viewProj, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
    if (m_sortedIndices.empty()) return;

    const glm::mat4 lightClip = kVulkanClipFix * m_renderWorld.shadow.viewProj;

    VkClearValue clear{};
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass        = m_shadowPass;
    rpbi.framebuffer       = m_shadowFB;
    rpbi.renderArea.extent = { m_shadowSize, m_shadowSize };
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
    VkViewport vp{ 0.0f, 0.0f, (float)m_shadowSize, (float)m_shadowSize, 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { m_shadowSize, m_shadowSize } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    for (uint32_t idx : m_sortedIndices)
    {
        const RenderObject& obj = m_renderWorld.objects[idx];
        const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
        const GpuMesh& m    = mesh ? *mesh : m_cube;
        if (!m.indexCount) continue;
        PushConstants pc{ lightClip * obj.transform, obj.transform };
        vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
        vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

bool VulkanRenderer::createMeshBuffers(GpuMesh& mesh, const std::vector<float>& interleaved,
                                       const std::vector<uint32_t>& indices)
{
    auto makeBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage, const void* data,
                       VkBuffer& buf, VkDeviceMemory& mem) -> bool
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buf, mem, 0);
        void* p = nullptr;
        vkMapMemory(m_device, mem, 0, size, 0, &p);
        std::memcpy(p, data, static_cast<size_t>(size));
        vkUnmapMemory(m_device, mem);
        return true;
    };
    const VkDeviceSize vsize = interleaved.size() * sizeof(float);
    const VkDeviceSize isize = indices.size() * sizeof(uint32_t);
    if (!makeBuf(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, interleaved.data(), mesh.vbuf, mesh.vmem)) return false;
    if (!makeBuf(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  indices.data(),     mesh.ibuf, mesh.imem)) return false;
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

void VulkanRenderer::createCube()
{
    static const float v[] = {
         0.5f,-0.5f,-0.5f, 1,0,0, 0,0,   0.5f, 0.5f,-0.5f, 1,0,0, 0,0,   0.5f, 0.5f, 0.5f, 1,0,0, 0,0,   0.5f,-0.5f, 0.5f, 1,0,0, 0,0,
        -0.5f,-0.5f, 0.5f,-1,0,0, 0,0,  -0.5f, 0.5f, 0.5f,-1,0,0, 0,0,  -0.5f, 0.5f,-0.5f,-1,0,0, 0,0,  -0.5f,-0.5f,-0.5f,-1,0,0, 0,0,
        -0.5f, 0.5f,-0.5f, 0,1,0, 0,0,  -0.5f, 0.5f, 0.5f, 0,1,0, 0,0,   0.5f, 0.5f, 0.5f, 0,1,0, 0,0,   0.5f, 0.5f,-0.5f, 0,1,0, 0,0,
        -0.5f,-0.5f, 0.5f, 0,-1,0,0,0,  -0.5f,-0.5f,-0.5f, 0,-1,0,0,0,   0.5f,-0.5f,-0.5f, 0,-1,0,0,0,   0.5f,-0.5f, 0.5f, 0,-1,0,0,0,
        -0.5f,-0.5f, 0.5f, 0,0,1, 0,0,   0.5f,-0.5f, 0.5f, 0,0,1, 0,0,   0.5f, 0.5f, 0.5f, 0,0,1, 0,0,  -0.5f, 0.5f, 0.5f, 0,0,1, 0,0,
         0.5f,-0.5f,-0.5f, 0,0,-1,0,0,  -0.5f,-0.5f,-0.5f, 0,0,-1,0,0,  -0.5f, 0.5f,-0.5f, 0,0,-1,0,0,   0.5f, 0.5f,-0.5f, 0,0,-1,0,0,
    };
    static const uint32_t idx[] = {
         0, 2, 1,  0, 3, 2,    4, 6, 5,  4, 7, 6,
         8,10, 9,  8,11,10,   12,14,13, 12,15,14,
        16,18,17, 16,19,18,   20,22,21, 20,23,22,
    };
    std::vector<float>    verts(v, v + sizeof(v) / sizeof(float));
    std::vector<uint32_t> indices(idx, idx + sizeof(idx) / sizeof(uint32_t));
    createMeshBuffers(m_cube, verts, indices);
    m_cube.localBounds.expand({ -0.5f, -0.5f, -0.5f });
    m_cube.localBounds.expand({  0.5f,  0.5f,  0.5f });
}

const VulkanRenderer::GpuMesh* VulkanRenderer::resolveMesh(const HE::UUID& assetId)
{
    if (assetId == HE::UUID{} || !m_contentManager) return nullptr;
    if (auto it = m_meshCache.find(assetId); it != m_meshCache.end()) return &it->second;

    const StaticMeshAsset* asset = m_contentManager->getStaticMesh(assetId);
    if (!asset || asset->vertices.empty() || asset->indices.empty()) return nullptr;

    const size_t vertexCount = asset->vertices.size() / 3;
    std::vector<float> interleaved;
    interleaved.reserve(vertexCount * 8);
    for (size_t i = 0; i < vertexCount; ++i)
    {
        interleaved.insert(interleaved.end(),
            { asset->vertices[i*3+0], asset->vertices[i*3+1], asset->vertices[i*3+2] });
        if (i * 3 + 2 < asset->normals.size())
            interleaved.insert(interleaved.end(),
                { asset->normals[i*3+0], asset->normals[i*3+1], asset->normals[i*3+2] });
        else
            interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
        if (i * 2 + 1 < asset->uvs.size())
            interleaved.insert(interleaved.end(), { asset->uvs[i*2+0], asset->uvs[i*2+1] });
        else
            interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
    }
    GpuMesh mesh;
    mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
    if (!createMeshBuffers(mesh, interleaved, asset->indices)) return nullptr;
    return &m_meshCache.emplace(assetId, mesh).first->second;
}

void VulkanRenderer::DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool hdr)
{
    if (!m_world || m_scenePipeline == VK_NULL_HANDLE || width == 0 || height == 0) return;

    m_extractor.extract(*m_world, m_renderWorld,
                        static_cast<float>(width) / static_cast<float>(height),
                        &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;

    for (RenderObject& obj : m_renderWorld.objects)
    {
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);
        if (m_contentManager)
        {
            const HE::UUID matId = obj.materialAssetId;
            if (const MaterialAsset* mat = (matId == HE::UUID{}) ? nullptr
                                           : m_contentManager->getMaterial(matId))
            {
                obj.baseColor = { mat->baseColor[0], mat->baseColor[1], mat->baseColor[2] };
                obj.metallic  = mat->metallic;
                obj.roughness = mat->roughness;
                obj.opacity   = mat->opacity;
            }
        }
    }

    m_culler.cull(m_renderWorld, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
    if (m_sortedIndices.empty()) return;

    if (m_renderGraph.empty())
        m_renderGraph.addPass(std::make_unique<GeometryPass>());

    // GL-convention projection from the extractor → Vulkan clip space.
    const glm::mat4 viewProj =
        kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;

    // Per-frame UBO for this in-flight slot.
    {
        FrameUBOData f{};
        f.cameraPos     = glm::vec4(m_renderWorld.camera.position, 1.0f);
        const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), 8);
        f.lightCount    = glm::ivec4(count, 0, 0, 0);
        for (int i = 0; i < count; ++i)
        {
            const LightData& l = m_renderWorld.lights[i];
            f.lightPos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
            f.lightDir[i]    = glm::vec4(l.direction, l.spotAngleCos);
            f.lightColor[i]  = glm::vec4(l.color,     l.intensity);
            f.lightParams[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
        }
        f.lightVP       = kVulkanClipFix * m_renderWorld.shadow.viewProj;
        f.shadowEnabled = glm::ivec4(m_renderWorld.shadow.enabled ? 1 : 0, 0, 0, 0);
        if (m_frameUBO[m_currentFrame].mapped)
            std::memcpy(m_frameUBO[m_currentFrame].mapped, &f, sizeof(f));
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { width, height } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                            0, 1, &m_frameUBO[m_currentFrame].set, 0, nullptr);

    // Per-pass sink: today the only pass renders to the backbuffer (the active
    // render pass). Offscreen targets (id != backbuffer) arrive with shadows/HDR.
    m_renderGraph.execute(m_renderWorld, m_sortedIndices,
        [&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
    {
        if (io.output.id != kBackbufferTarget) return;

        const glm::vec3 camPos = m_renderWorld.camera.position;
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        for (const DrawCall& dc : cmds.drawCalls())
            (dc.opacity < 0.999f ? transparentDCs : opaqueDCs).push_back(&dc);
        std::sort(transparentDCs.begin(), transparentDCs.end(),
            [&](const DrawCall* a, const DrawCall* b) {
                return glm::length(glm::vec3(a->transform[3]) - camPos) >
                       glm::length(glm::vec3(b->transform[3]) - camPos);
            });

        auto drawDCVk = [&](const DrawCall& dc) {
            const GpuMesh* mesh = resolveMesh(dc.meshAssetId);
            const GpuMesh& m    = mesh ? *mesh : m_cube;
            if (!m.indexCount) return;

            if (m_matUBO)
            {
                struct MatData { float r,g,b,met; float rough,opacity; float pad[2]; } md{
                    dc.baseColor.r, dc.baseColor.g, dc.baseColor.b, dc.metallic,
                    dc.roughness, dc.opacity
                };
                vkCmdUpdateBuffer(cmd, m_matUBO, 0, sizeof(md), &md);
                VkBufferMemoryBarrier bar{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
                bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bar.buffer = m_matUBO; bar.offset = 0; bar.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 1, &bar, 0, nullptr);
            }

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            auto drawOne = [&](const glm::mat4& model) {
                PushConstants pc2{ viewProj * model, model };
                vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(pc2), &pc2);
                vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
            };
            if (!dc.instanceTransforms.empty())
                for (const glm::mat4& t : dc.instanceTransforms) drawOne(t);
            else
                drawOne(dc.transform);
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
        for (const DrawCall* dc : opaqueDCs) drawDCVk(*dc);

        const VkPipeline transPipe = hdr && m_sceneTransparentPipelineHDR
            ? m_sceneTransparentPipelineHDR : m_sceneTransparentPipeline;
        if (!transparentDCs.empty() && transPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transPipe);
            for (const DrawCall* dc : transparentDCs) drawDCVk(*dc);
        }
    });
}

