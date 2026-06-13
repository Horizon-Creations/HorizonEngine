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
	m_shaderManager = VulkanShaderManager();
}

void VulkanRenderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: shutdown — waiting for GPU");
    if (m_device) vkDeviceWaitIdle(m_device);
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
    const uint32_t fi = m_currentFrame;

    vkWaitForFences(m_device, 1, &m_frameFence[fi], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                         m_imageReady[fi], VK_NULL_HANDLE, &imageIndex);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
        vkCheck(acq, "vkAcquireNextImageKHR");

    vkResetFences(m_device, 1, &m_frameFence[fi]);

    VkCommandBuffer cmd = m_cmdBufs[imageIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // Shadow map first, in its own render pass (before the swapchain pass).
    EncodeShadowMap(cmd);

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.18f, 0.18f, 0.20f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = m_renderPass;
    rpbi.framebuffer       = m_framebuffers[imageIndex];
    rpbi.renderArea.extent = m_swapExtent;
    rpbi.clearValueCount   = 2;
    rpbi.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
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
    vkQueuePresentKHR(m_graphicsQueue, &pi);

    m_currentFrame = (m_currentFrame + 1) % k_maxFramesInFlight;
}

IRenderer::Capabilities VulkanRenderer::GetCapabilities() const { return { true, true, true }; }

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

    vkDeviceWaitIdle(m_device);
    destroySwapchain();
    createSwapchain(m_swapExtent.width, m_swapExtent.height);
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(m_device, fb, nullptr);
    m_framebuffers.clear();
    createFramebuffers();
    vkResetCommandPool(m_device, m_cmdPool, 0);
    m_cmdBufs.clear();
    createCommandBuffers();
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
    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = m_graphicsFamily;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmdPool), "vkCreateCommandPool");
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
    // Descriptor set: binding 0 = per-frame UBO, binding 1 = shadow map sampler.
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding         = 0;
    binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding         = 1;
    binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 2;
    slci.pBindings    = binds;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_sceneSetLayout), "descriptor set layout");

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
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight },
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
        VkWriteDescriptorSet w[2]{};
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
        vkUpdateDescriptorSets(m_device, m_shadowView ? 2 : 1, w, 0, nullptr);
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
    if (m_descPool)            { vkDestroyDescriptorPool(m_device, m_descPool, nullptr);            m_descPool = VK_NULL_HANDLE; }
    if (m_shadowPipeline)      { vkDestroyPipeline      (m_device, m_shadowPipeline, nullptr);      m_shadowPipeline = VK_NULL_HANDLE; }
    if (m_scenePipeline)       { vkDestroyPipeline      (m_device, m_scenePipeline, nullptr);       m_scenePipeline = VK_NULL_HANDLE; }
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

void VulkanRenderer::EncodeShadowMap(VkCommandBuffer cmd)
{
    if (!m_world || m_shadowPipeline == VK_NULL_HANDLE) return;

    m_extractor.extract(*m_world, m_renderWorld, 1.0f, &m_editorCamera);
    if (!m_renderWorld.shadow.enabled || m_renderWorld.objects.empty()) return;
    for (RenderObject& obj : m_renderWorld.objects)
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);
    m_culler.cull(m_renderWorld, m_visible);
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

void VulkanRenderer::DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (!m_world || m_scenePipeline == VK_NULL_HANDLE || width == 0 || height == 0) return;

    m_extractor.extract(*m_world, m_renderWorld,
                        static_cast<float>(width) / static_cast<float>(height),
                        &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;

    for (RenderObject& obj : m_renderWorld.objects)
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);

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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
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
        for (const DrawCall& dc : cmds.drawCalls())
        {
            const GpuMesh* mesh = resolveMesh(dc.meshAssetId);
            const GpuMesh& m    = mesh ? *mesh : m_cube;
            if (!m.indexCount) continue;

            PushConstants pc{ viewProj * dc.transform, dc.transform };
            vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(pc), &pc);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
        }
    });
}

