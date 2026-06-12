#include "Backends/Vulkan/VulkanRenderer.h"
#include <Window/Window.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <Diagnostics/Logger.h>

static constexpr uint32_t k_maxFramesInFlight = 2;

static void vkCheck(VkResult r, const char* msg)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan: ") + msg);
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
    createSyncObjects();    Logger::Log(Logger::LogLevel::Info, "VulkanRenderer: initialized successfully");
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

    VkClearValue clear{};
    clear.color = { { 0.18f, 0.18f, 0.20f, 1.0f } };
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass        = m_renderPass;
    rpbi.framebuffer       = m_framebuffers[imageIndex];
    rpbi.renderArea.extent = m_swapExtent;
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
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
}

void VulkanRenderer::createRenderPass()
{
    VkAttachmentDescription color{};
    color.format         = m_swapFormat;
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
    vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_renderPass), "vkCreateRenderPass");
}

void VulkanRenderer::createFramebuffers()
{
    m_framebuffers.resize(m_swapViews.size());
    for (size_t i = 0; i < m_swapViews.size(); ++i)
    {
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = m_renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &m_swapViews[i];
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

