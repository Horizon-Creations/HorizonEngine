#include "Backends/Vulkan/VulkanRenderer.h"
#include <cstdint>
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <Renderer/UIFont.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <functional>
#include <Diagnostics/Logger.h>
#include <HorizonRendering/ClipSpace.h>
#include <HorizonRendering/LightPacking.h>
#include <HorizonRendering/RenderConstants.h>
#include <HorizonRendering/SkyFrameParams.h>
#include <HorizonRendering/SkyNoise3D.h>
#include <HorizonRendering/SsaoKernel.h>
#include <HorizonRendering/PreviewFraming.h> // shared thumbnail/preview camera + framing constants
#include <material/PreviewMesh.h>            // procedural sphere/cube/plane for material tiles
#if defined(HE_HAVE_SHADERC)
#include "ShaderCompiler.h"                  // he::shaderc — runtime GLSL → SPIR-V (particle tiles)
#endif

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
        // sky/fog additions — must match scene.frag Frame block exactly
        glm::vec4  sunDir;    // xyz = sun direction, w = 0
        glm::vec4  fog;       // x = fogDensity, y = fogHeightFalloff, zw = 0
        glm::vec4  viewport;  // x=W, y=H, z=ssaoEnabled(1.0), w=0
        glm::vec4  giGridOrigin; // xyz = probe grid origin, w = spacing
        glm::vec4  giGridCounts; // xyz = probe counts, w = probesPerRow
        glm::vec4  giParams;     // x = indirectIntensity, y = giEnabled(1.0), zw = 0
    };

    // Sky pass UBO (set=0 binding=0 in sky.frag) — must match std140 exactly.
    struct SkyUBOData
    {
        glm::mat4  invViewProj;                          // offset   0, 64 bytes
        glm::vec3  sunDir;       float timeOfDay;        // offset  64, 16 bytes
        glm::vec3  sunColor;     float cloudCoverage;    // offset  80, 16 bytes
        glm::vec3  wind;         float time;             // offset  96, 16 bytes
        glm::vec3  auroraColor;  float aurora;           // offset 112, 16 bytes
        float      milkyWay;     float flash;  int hasMoonTex;  float nebula; // offset 128, 16 bytes
        glm::vec3  nebulaColor;  float _pad2;            // offset 144, 16 bytes
    }; // 160 bytes

    // Debug line pass constant buffer (set=0 binding=0 in debug_line.vert).
    struct DebugUBOData
    {
        glm::mat4 uVP;
    };
}

VulkanRenderer::VulkanRenderer()  = default;
VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::Initialize(HE::Window* window)
{
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: initializing");
    m_sdlWindow = window->GetNativeWindow();
    createInstance();       HE_LOG_INFO(RHI, "%s", "VulkanRenderer: instance created");
    createSurface();        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: surface created");
    pickPhysicalDevice();   HE_LOG_INFO(RHI, "%s", "VulkanRenderer: physical device selected");
    createDevice();         HE_LOG_INFO(RHI, "%s", "VulkanRenderer: logical device created");
    gpuTimerInit();
    createSwapchain(window->GetWidth(), window->GetHeight()); HE_LOG_INFO(RHI, "%s", "VulkanRenderer: swapchain created");
    createRenderPass();     HE_LOG_INFO(RHI, "%s", "VulkanRenderer: render pass created");
    createFramebuffers();   HE_LOG_INFO(RHI, "%s", "VulkanRenderer: framebuffers created");
    createCommandBuffers(); HE_LOG_INFO(RHI, "%s", "VulkanRenderer: command buffers created");
    createSyncObjects();    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: sync objects created");
    createShadowResources();HE_LOG_INFO(RHI, "%s", "VulkanRenderer: shadow resources created");
    createScenePipeline();  HE_LOG_INFO(RHI, "%s", "VulkanRenderer: scene pipeline created");
    createCube();           HE_LOG_INFO(RHI, "%s", "VulkanRenderer: initialized successfully");
    createPostFXPipelines();
    createSkyPipeline();         HE_LOG_INFO(RHI, "%s", "VulkanRenderer: sky pipeline created");
    createDebugLinePipeline();   HE_LOG_INFO(RHI, "%s", "VulkanRenderer: debug line pipeline created");
    createSSAOPipeline();        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: SSAO pipeline created");
    createSkinnedPipeline();     HE_LOG_INFO(RHI, "%s", "VulkanRenderer: skinned pipeline created");
    createUIPipeline();          HE_LOG_INFO(RHI, "%s", "VulkanRenderer: UI pipeline created");
	m_shaderManager = VulkanShaderManager();
}

void VulkanRenderer::Shutdown()
{
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: shutdown — waiting for GPU");
    if (m_device) vkDeviceWaitIdle(m_device);
    // ImGui editor textures (logo + content-browser icons): destroy the GPU
    // resources we retained for them. The ImGui descriptor sets are freed by
    // ImGui's own shutdown, which the editor runs before tearing down the renderer.
    for (auto& t : m_imguiTextures)
    {
        if (t.sampler) vkDestroySampler  (m_device, t.sampler, nullptr);
        if (t.view)    vkDestroyImageView(m_device, t.view,    nullptr);
        if (t.image)   vkDestroyImage    (m_device, t.image,   nullptr);
        if (t.memory)  vkFreeMemory      (m_device, t.memory,  nullptr);
    }
    m_imguiTextures.clear();
    destroySkyPipeline();
    destroyDebugLinePipeline();
    destroyPostFXResources();
    destroyPostFXPipelines();
    // SSAO viewport-size targets (already freed by destroyViewportResources below,
    // but call explicitly in case viewport was never created).
    destroySSAOTargets();
    destroyGiAccel();
    destroyGiTargets();
    destroyGiProbeAtlas();
    for (GiBuffer* b : { m_giShadowUBO, m_giProbeUBO, m_giTemporalUBO })
        for (uint32_t i = 0; i < 3; ++i)
        {
            if (b[i].mapped) { vkUnmapMemory(m_device, b[i].mem); b[i].mapped = nullptr; }
            if (b[i].buf)    { vkDestroyBuffer(m_device, b[i].buf, nullptr); b[i].buf = VK_NULL_HANDLE; }
            if (b[i].mem)    { vkFreeMemory(m_device, b[i].mem, nullptr);    b[i].mem = VK_NULL_HANDLE; }
            b[i].size = 0;
        }
    if (m_giGBufPipe)     { vkDestroyPipeline(m_device, m_giGBufPipe, nullptr);     m_giGBufPipe = VK_NULL_HANDLE; }
    if (m_giTemporalPipe) { vkDestroyPipeline(m_device, m_giTemporalPipe, nullptr); m_giTemporalPipe = VK_NULL_HANDLE; }
    if (m_giBlurPipe)     { vkDestroyPipeline(m_device, m_giBlurPipe, nullptr);     m_giBlurPipe = VK_NULL_HANDLE; }
    if (m_giShadowPipe)   { vkDestroyPipeline(m_device, m_giShadowPipe, nullptr);   m_giShadowPipe = VK_NULL_HANDLE; }
    if (m_giProbePipe)    { vkDestroyPipeline(m_device, m_giProbePipe, nullptr);    m_giProbePipe = VK_NULL_HANDLE; }
    if (m_giGBufRP)       { vkDestroyRenderPass(m_device, m_giGBufRP, nullptr);     m_giGBufRP = VK_NULL_HANDLE; }
    if (m_giTemporalRP)   { vkDestroyRenderPass(m_device, m_giTemporalRP, nullptr); m_giTemporalRP = VK_NULL_HANDLE; }
    if (m_giBlurRP)       { vkDestroyRenderPass(m_device, m_giBlurRP, nullptr);     m_giBlurRP = VK_NULL_HANDLE; }
    if (m_giShadowPL)     { vkDestroyPipelineLayout(m_device, m_giShadowPL, nullptr); m_giShadowPL = VK_NULL_HANDLE; }
    if (m_giProbePL)      { vkDestroyPipelineLayout(m_device, m_giProbePL, nullptr);  m_giProbePL = VK_NULL_HANDLE; }
    if (m_giFsPL)         { vkDestroyPipelineLayout(m_device, m_giFsPL, nullptr);     m_giFsPL = VK_NULL_HANDLE; }
    if (m_giGBufPL)       { vkDestroyPipelineLayout(m_device, m_giGBufPL, nullptr);   m_giGBufPL = VK_NULL_HANDLE; }
    if (m_giShadowDSL)    { vkDestroyDescriptorSetLayout(m_device, m_giShadowDSL, nullptr); m_giShadowDSL = VK_NULL_HANDLE; }
    if (m_giProbeDSL)     { vkDestroyDescriptorSetLayout(m_device, m_giProbeDSL, nullptr);  m_giProbeDSL = VK_NULL_HANDLE; }
    if (m_giFsDSL)        { vkDestroyDescriptorSetLayout(m_device, m_giFsDSL, nullptr);     m_giFsDSL = VK_NULL_HANDLE; }
    if (m_giDescPool)     { vkDestroyDescriptorPool(m_device, m_giDescPool, nullptr);       m_giDescPool = VK_NULL_HANDLE; }
    // HW ray-query kernel objects (the AS/BLAS/TLAS themselves are freed by
    // destroyGiAccel above).
    if (m_giShadowHwPipe) { vkDestroyPipeline(m_device, m_giShadowHwPipe, nullptr);         m_giShadowHwPipe = VK_NULL_HANDLE; }
    if (m_giProbeHwPipe)  { vkDestroyPipeline(m_device, m_giProbeHwPipe, nullptr);          m_giProbeHwPipe  = VK_NULL_HANDLE; }
    if (m_giShadowHwPL)   { vkDestroyPipelineLayout(m_device, m_giShadowHwPL, nullptr);     m_giShadowHwPL   = VK_NULL_HANDLE; }
    if (m_giProbeHwPL)    { vkDestroyPipelineLayout(m_device, m_giProbeHwPL, nullptr);      m_giProbeHwPL    = VK_NULL_HANDLE; }
    if (m_giHwDescPool)   { vkDestroyDescriptorPool(m_device, m_giHwDescPool, nullptr);     m_giHwDescPool   = VK_NULL_HANDLE; }
    if (m_giShadowHwDSL)  { vkDestroyDescriptorSetLayout(m_device, m_giShadowHwDSL, nullptr); m_giShadowHwDSL = VK_NULL_HANDLE; }
    if (m_giProbeHwDSL)   { vkDestroyDescriptorSetLayout(m_device, m_giProbeHwDSL, nullptr);  m_giProbeHwDSL  = VK_NULL_HANDLE; }
    m_giHwPipesReady = false;
    if (m_giPointSampler) { vkDestroySampler(m_device, m_giPointSampler, nullptr);          m_giPointSampler = VK_NULL_HANDLE; }
    m_giPipelinesTried = false;
    m_giReady          = false;
    // SSAO pipeline-level resources (static, not viewport-size-dependent).
    if (m_ssaoPosGfxPipeline)   { vkDestroyPipeline      (m_device, m_ssaoPosGfxPipeline,   nullptr); m_ssaoPosGfxPipeline   = VK_NULL_HANDLE; }
    if (m_ssaoGfxPipeline)      { vkDestroyPipeline      (m_device, m_ssaoGfxPipeline,       nullptr); m_ssaoGfxPipeline      = VK_NULL_HANDLE; }
    if (m_ssaoBlurGfxPipeline)  { vkDestroyPipeline      (m_device, m_ssaoBlurGfxPipeline,   nullptr); m_ssaoBlurGfxPipeline  = VK_NULL_HANDLE; }
    if (m_ssaoPipeLayout)       { vkDestroyPipelineLayout(m_device, m_ssaoPipeLayout,         nullptr); m_ssaoPipeLayout       = VK_NULL_HANDLE; }
    if (m_ssaoBlurPipeLayout)   { vkDestroyPipelineLayout(m_device, m_ssaoBlurPipeLayout,     nullptr); m_ssaoBlurPipeLayout   = VK_NULL_HANDLE; }
    if (m_ssaoDescPool)         { vkDestroyDescriptorPool(m_device, m_ssaoDescPool,           nullptr); m_ssaoDescPool         = VK_NULL_HANDLE; }
    if (m_ssaoDescLayout)       { vkDestroyDescriptorSetLayout(m_device, m_ssaoDescLayout,    nullptr); m_ssaoDescLayout       = VK_NULL_HANDLE; }
    if (m_ssaoBlurDescLayout)   { vkDestroyDescriptorSetLayout(m_device, m_ssaoBlurDescLayout,nullptr); m_ssaoBlurDescLayout   = VK_NULL_HANDLE; }
    if (m_ssaoPosRenderPass)    { vkDestroyRenderPass    (m_device, m_ssaoPosRenderPass,      nullptr); m_ssaoPosRenderPass    = VK_NULL_HANDLE; }
    if (m_ssaoRenderPass)       { vkDestroyRenderPass    (m_device, m_ssaoRenderPass,         nullptr); m_ssaoRenderPass       = VK_NULL_HANDLE; }
    if (m_ssaoBlurRenderPass)   { vkDestroyRenderPass    (m_device, m_ssaoBlurRenderPass,     nullptr); m_ssaoBlurRenderPass   = VK_NULL_HANDLE; }
    if (m_ssaoUBOPtr)           { vkUnmapMemory(m_device, m_ssaoUBOMem);                                m_ssaoUBOPtr           = nullptr; }
    if (m_ssaoUBO)              { vkDestroyBuffer        (m_device, m_ssaoUBO,               nullptr); m_ssaoUBO              = VK_NULL_HANDLE; }
    if (m_ssaoUBOMem)           { vkFreeMemory           (m_device, m_ssaoUBOMem,            nullptr); m_ssaoUBOMem           = VK_NULL_HANDLE; }
    if (m_ssaoNoiseSampler)     { vkDestroySampler       (m_device, m_ssaoNoiseSampler,      nullptr); m_ssaoNoiseSampler     = VK_NULL_HANDLE; }
    if (m_ssaoSampler)          { vkDestroySampler       (m_device, m_ssaoSampler,           nullptr); m_ssaoSampler          = VK_NULL_HANDLE; }
    if (m_ssaoNoiseView)        { vkDestroyImageView     (m_device, m_ssaoNoiseView,         nullptr); m_ssaoNoiseView        = VK_NULL_HANDLE; }
    if (m_ssaoNoiseTex)         { vkDestroyImage         (m_device, m_ssaoNoiseTex,          nullptr); m_ssaoNoiseTex         = VK_NULL_HANDLE; }
    if (m_ssaoNoiseMem)         { vkFreeMemory           (m_device, m_ssaoNoiseMem,          nullptr); m_ssaoNoiseMem         = VK_NULL_HANDLE; }
    if (m_ssaoWhiteView)        { vkDestroyImageView     (m_device, m_ssaoWhiteView,         nullptr); m_ssaoWhiteView        = VK_NULL_HANDLE; }
    if (m_ssaoWhiteTex)         { vkDestroyImage         (m_device, m_ssaoWhiteTex,          nullptr); m_ssaoWhiteTex         = VK_NULL_HANDLE; }
    if (m_ssaoWhiteMem)         { vkFreeMemory           (m_device, m_ssaoWhiteMem,          nullptr); m_ssaoWhiteMem         = VK_NULL_HANDLE; }
    // Skeletal mesh skinning resources
    destroySkeletalMeshCache();
    if (m_skinnedPipelineHDR)  { vkDestroyPipeline      (m_device, m_skinnedPipelineHDR, nullptr);  m_skinnedPipelineHDR  = VK_NULL_HANDLE; }
    if (m_skinnedPipeline)     { vkDestroyPipeline      (m_device, m_skinnedPipeline,    nullptr);  m_skinnedPipeline     = VK_NULL_HANDLE; }
    if (m_skinnedPipeLayout)   { vkDestroyPipelineLayout(m_device, m_skinnedPipeLayout,  nullptr);  m_skinnedPipeLayout   = VK_NULL_HANDLE; }
    if (m_skinnedDescPool)     { vkDestroyDescriptorPool(m_device, m_skinnedDescPool,    nullptr);  m_skinnedDescPool     = VK_NULL_HANDLE; }
    if (m_skinnedBonesDSL)     { vkDestroyDescriptorSetLayout(m_device, m_skinnedBonesDSL, nullptr); m_skinnedBonesDSL    = VK_NULL_HANDLE; }
    // Interactive asset previews BEFORE the thumbnails: their framebuffers are
    // built over m_thumbRenderPass, which destroyThumbnailResources() destroys,
    // and a framebuffer must not outlive its render pass.
    destroyPreviewResources();
    // Content-Browser thumbnails. BEFORE the UI block below: m_thumbUIFB is a
    // framebuffer over m_uiViewportRP, and a framebuffer must not outlive the
    // render pass it was created with.
    destroyThumbnailResources();
    // UI canvas pipeline + font atlases
    destroyUIFontAtlases();
    if (m_uiViewportFB)       { vkDestroyFramebuffer    (m_device, m_uiViewportFB,      nullptr); m_uiViewportFB      = VK_NULL_HANDLE; }
    if (m_uiViewportPipeline) { vkDestroyPipeline       (m_device, m_uiViewportPipeline,nullptr); m_uiViewportPipeline = VK_NULL_HANDLE; }
    if (m_uiPipeline)         { vkDestroyPipeline       (m_device, m_uiPipeline,        nullptr); m_uiPipeline        = VK_NULL_HANDLE; }
    if (m_uiPipeLayout)       { vkDestroyPipelineLayout (m_device, m_uiPipeLayout,      nullptr); m_uiPipeLayout      = VK_NULL_HANDLE; }
    if (m_uiViewportRP)       { vkDestroyRenderPass     (m_device, m_uiViewportRP,      nullptr); m_uiViewportRP      = VK_NULL_HANDLE; }
    if (m_uiAtlasDescPool)    { vkDestroyDescriptorPool (m_device, m_uiAtlasDescPool,   nullptr); m_uiAtlasDescPool   = VK_NULL_HANDLE; }
    if (m_uiAtlasDSLayout)    { vkDestroyDescriptorSetLayout(m_device, m_uiAtlasDSLayout, nullptr); m_uiAtlasDSLayout = VK_NULL_HANDLE; }
    if (m_uiFontSampler)      { vkDestroySampler        (m_device, m_uiFontSampler,     nullptr); m_uiFontSampler     = VK_NULL_HANDLE; }
    if (m_tsQueryPool)        { vkDestroyQueryPool      (m_device, m_tsQueryPool,       nullptr); m_tsQueryPool       = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_boneUBOPtr[i])  { vkUnmapMemory(m_device, m_boneUBOMem[i]); m_boneUBOPtr[i] = nullptr; }
        if (m_boneUBO[i])     { vkDestroyBuffer(m_device, m_boneUBO[i],   nullptr); m_boneUBO[i]    = VK_NULL_HANDLE; }
        if (m_boneUBOMem[i])  { vkFreeMemory  (m_device, m_boneUBOMem[i], nullptr); m_boneUBOMem[i] = VK_NULL_HANDLE; }
    }
    // Destroy secondary windows first
    for (auto& [sdlWin, wd] : m_extraWindows)
        destroyWindowData(wd);
    m_extraWindows.clear();
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_frameFence[i])  vkDestroyFence    (m_device, m_frameFence[i],  nullptr);
        if (m_imageReady[i])  vkDestroySemaphore(m_device, m_imageReady[i],  nullptr);
    }
    for (VkSemaphore s : m_renderDone) if (s) vkDestroySemaphore(m_device, s, nullptr);
    m_renderDone.clear();
    m_imagesInFlight.clear();   // fences are owned by m_frameFence — don't destroy here
    // Scene resources
    for (auto& [id, mesh] : m_meshCache)
    {
        if (mesh.vbuf) vkDestroyBuffer(m_device, mesh.vbuf, nullptr);
        if (mesh.vmem) vkFreeMemory   (m_device, mesh.vmem, nullptr);
        if (mesh.ibuf) vkDestroyBuffer(m_device, mesh.ibuf, nullptr);
        if (mesh.imem) vkFreeMemory   (m_device, mesh.imem, nullptr);
        // Base-color texture (descriptor set is freed with m_albedoPool in destroyScenePipeline).
        if (mesh.albedoView)  vkDestroyImageView(m_device, mesh.albedoView,  nullptr);
        if (mesh.albedoImage) vkDestroyImage    (m_device, mesh.albedoImage, nullptr);
        if (mesh.albedoMem)   vkFreeMemory      (m_device, mesh.albedoMem,   nullptr);
    }
    m_meshCache.clear();
    if (m_cube.vbuf) vkDestroyBuffer(m_device, m_cube.vbuf, nullptr);
    if (m_cube.vmem) vkFreeMemory   (m_device, m_cube.vmem, nullptr);
    if (m_cube.ibuf) vkDestroyBuffer(m_device, m_cube.ibuf, nullptr);
    if (m_cube.imem) vkFreeMemory   (m_device, m_cube.imem, nullptr);
    m_cube = {};
    // Override-material textures (before destroyScenePipeline destroys m_albedoPool, since
    // destroyMaterialTex frees descriptor sets from it). Device is already idle here.
    for (auto& [id, mt] : m_materialTexCache) destroyMaterialTex(mt);
    m_materialTexCache.clear();
    m_pendingMatInval.clear();
    m_pendingMeshInval.clear();
    destroyScenePipeline();

    destroyViewportResources();
    for (auto& r : m_retiredViewports)   // flush any pending retired viewport images
    {
        if (r.view) vkDestroyImageView(m_device, r.view, nullptr);
        if (r.img)  vkDestroyImage    (m_device, r.img,  nullptr);
        if (r.mem)  vkFreeMemory       (m_device, r.mem,  nullptr);
    }
    m_retiredViewports.clear();
    destroyShadowResources();
    if (m_cmdPool)     vkDestroyCommandPool(m_device,   m_cmdPool,     nullptr);
    if (m_renderPass)  vkDestroyRenderPass (m_device,   m_renderPass,  nullptr);
    destroySwapchain();
    if (m_device)   vkDestroyDevice    (m_device,              nullptr);
    if (m_surface)  vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger)
    {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance) vkDestroyInstance  (m_instance,            nullptr);
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: all resources released");
}

void VulkanRenderer::Render()
{
    m_wallTime = static_cast<float>(SDL_GetTicks()) * 0.001f;
    m_ssaoRanThisFrame = false;  // cleared each frame; set true only inside runSSAO()
    m_giRanThisFrame   = false;  // cleared each frame; set true only inside runGi()
    m_statDraws = m_statTris = m_statVisible = m_statTotal = 0;  // rebuilt by DrawScene

    // Drop caches for materials/meshes edited since last frame, before any recording — the
    // frame's DrawScene then re-resolves them fresh from the ContentManager.
    processPendingInvalidations();

    // Free retired viewport color images once enough frames have passed (GPU done).
    for (auto it = m_retiredViewports.begin(); it != m_retiredViewports.end(); )
    {
        if (--it->frames <= 0)
        {
            if (it->view) vkDestroyImageView(m_device, it->view, nullptr);
            if (it->img)  vkDestroyImage    (m_device, it->img,  nullptr);
            if (it->mem)  vkFreeMemory       (m_device, it->mem,  nullptr);
            it = m_retiredViewports.erase(it);
        }
        else ++it;
    }

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

    // This swapchain image's command buffer / present semaphore may still be in use by a
    // PREVIOUS in-flight frame (the per-frame fence we waited above does not cover it).
    // Wait on the fence that last used this image before reusing its resources.
    if (m_imagesInFlight[imageIndex] != VK_NULL_HANDLE)
        vkWaitForFences(m_device, 1, &m_imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
    m_imagesInFlight[imageIndex] = m_frameFence[fi];

    vkResetFences(m_device, 1, &m_frameFence[fi]);

    VkCommandBuffer cmd = m_cmdBufs[imageIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &bi);

    // Frame-begin timestamp (also reaps the slot's previous results — no stall).
    gpuTimerBegin(cmd);

    // Shadow map first, in its own render pass (before the scene/swapchain pass).
    EncodeShadowMap(cmd);

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };

    if (useViewport)
    {
        const bool useHDR = m_postFxReady && m_hdrFB && m_ldrFB && m_fxaaFB;
        if (useHDR)
        {
            // ── Ray-traced GI (software compute): G-buffer + shadow rays +
            // temporal + blur + probe update. Extracts the scene itself.
            // Replaces the CSM lookup AND SSAO in scene.frag when it runs.
            runGi(cmd, m_viewportW, m_viewportH);

            // ── SSAO position prepass + occlusion compute + blur ───────────
            // runSSAO() extracts the scene itself (like EncodeShadowMap) so it
            // doesn't depend on DrawScene having run first. Skipped when GI
            // shades this frame (probe indirect replaces AO).
            if (!m_giRanThisFrame)
                runSSAO(cmd, m_viewportW, m_viewportH);

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

            if (m_bloomEnabled)
            {
                // ── Bloom bright pass ──────────────────────────────────────
                { const float p[4]={m_bloomThreshold,m_bloomKnee,0,0};
                  blitPass(m_postFxBlitF16, m_bloomFB[0], bw, bh, m_bloomBrightPipe, m_postFxDS[0], p); }
                m_bloomLayout[0] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                // ── 10 ping-pong blur passes ───────────────────────────────
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
            }
            else if (m_bloomLayout[0] != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                // Bloom disabled: bright/blur are skipped, but the tonemap set
                // still samples bloom[0]. Fill it once with black (bright pass,
                // unreachable threshold → contrib 0) so uninitialized F16 memory
                // (possibly NaN — which survives the strength-0 multiply) never
                // reaches the tonemapper, then park it in SHADER_READ_ONLY.
                { const float p[4]={3.4e38f, m_bloomKnee, 0, 0};
                  blitPass(m_postFxBlitF16, m_bloomFB[0], bw, bh, m_bloomBrightPipe, m_postFxDS[0], p); }
                runPostFXBarrier(cmd, m_bloomImage[0],
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                m_bloomLayout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            // ── Tonemap: hdr+bloom[0] → ldrFB ─────────────────────────────
            { const float p[4]={m_exposure, m_bloomEnabled ? m_bloomStrength : 0.0f, 0, 0};
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

            // ── AA resolve: ldrSRV → viewportFB ───────────────────────────
            // Always runs — it is what writes m_viewportImage. The AA method
            // only decides which pipeline does it (FXAA vs. passthrough).
            { const float p[4]={1.0f/float(m_viewportW),1.0f/float(m_viewportH),0,0};
              const VkPipeline aaPipe =
                  (m_aaMethod == HE::AAMethod::Off  && m_aaBlitPipe) ? m_aaBlitPipe :
                  (m_aaMethod == HE::AAMethod::SMAA && m_smaaPipe)   ? m_smaaPipe   :
                                                                       m_fxaaPipe;
              blitPass(m_postFxFinalRP, m_fxaaFB, m_viewportW, m_viewportH, aaPipe, m_postFxDS[4], p); }
            m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // ── UI canvas: composite onto viewport image (editor path) ──────
            if (m_uiViewportPipeline && !m_renderWorld.uiObjects.empty() && m_uiViewportFB)
            {
                // viewportImage is SHADER_READ_ONLY after FXAA; transition to COLOR_ATTACHMENT.
                runPostFXBarrier(cmd, m_viewportImage,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                m_viewportLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                VkRenderPassBeginInfo uirpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                uirpbi.renderPass        = m_uiViewportRP;
                uirpbi.framebuffer       = m_uiViewportFB;
                uirpbi.renderArea.extent = { m_viewportW, m_viewportH };
                vkCmdBeginRenderPass(cmd, &uirpbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiViewportPipeline);
                VkViewport uivp = { 0, 0, float(m_viewportW), float(m_viewportH), 0, 1 };
                VkRect2D   uisc = { {0,0}, {m_viewportW, m_viewportH} };
                vkCmdSetViewport(cmd, 0, 1, &uivp);
                vkCmdSetScissor(cmd,  0, 1, &uisc);
                runUIPass(cmd, int(m_viewportW), int(m_viewportH));
                vkCmdEndRenderPass(cmd);
                m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

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

            // ── UI canvas: composite onto viewport image (no-PostFX editor path) ─
            if (m_uiViewportPipeline && !m_renderWorld.uiObjects.empty() && m_uiViewportFB)
            {
                runPostFXBarrier(cmd, m_viewportImage,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                m_viewportLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                VkRenderPassBeginInfo uirpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                uirpbi.renderPass        = m_uiViewportRP;
                uirpbi.framebuffer       = m_uiViewportFB;
                uirpbi.renderArea.extent = { m_viewportW, m_viewportH };
                vkCmdBeginRenderPass(cmd, &uirpbi, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiViewportPipeline);
                VkViewport uivp = { 0, 0, float(m_viewportW), float(m_viewportH), 0, 1 };
                VkRect2D   uisc = { {0,0}, {m_viewportW, m_viewportH} };
                vkCmdSetViewport(cmd, 0, 1, &uivp);
                vkCmdSetScissor(cmd,  0, 1, &uisc);
                runUIPass(cmd, int(m_viewportW), int(m_viewportH));
                vkCmdEndRenderPass(cmd);
                m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
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
    {
        DrawScene(cmd, m_swapExtent.width, m_swapExtent.height);
        // UI canvas — inline in the swapchain pass (game / non-editor path).
        if (m_uiPipeline && !m_renderWorld.uiObjects.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeline);
            VkViewport uivp = { 0, 0, float(m_swapExtent.width), float(m_swapExtent.height), 0, 1 };
            VkRect2D   uisc = { {0,0}, {m_swapExtent.width, m_swapExtent.height} };
            vkCmdSetViewport(cmd, 0, 1, &uivp);
            vkCmdSetScissor(cmd,  0, 1, &uisc);
            runUIPass(cmd, int(m_swapExtent.width), int(m_swapExtent.height));
        }
    }
    if (m_overlayCallback) m_overlayCallback(cmd);
    vkCmdEndRenderPass(cmd);
    gpuTimerEnd(cmd);   // frame-end timestamp (after all GPU work this frame)
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
    si.pSignalSemaphores    = &m_renderDone[imageIndex];   // per-image present semaphore
    vkQueueSubmit(m_graphicsQueue, 1, &si, m_frameFence[fi]);

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderDone[imageIndex];     // per-image present semaphore
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

IRenderer::Capabilities VulkanRenderer::GetCapabilities() const
{
    Capabilities c;
    c.supportsShadows        = true;
    c.supportsPostProcessing = m_postFxReady;
    c.supportsHDR            = false;
    c.supportsGpuParticles   = false;
    // Software compute ray tracing (gi_*.comp) — compute is core Vulkan, so no
    // extension gate. If pipeline creation fails at runtime, runGi() leaves
    // m_giRanThisFrame false and rendering falls back to CSM+SSAO.
    c.supportsGlobalIllumination = true;
    return c;
}

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
    HE_LOG_INFO(RHI, "%s", enabled ? "VulkanRenderer: VSync enabled — recreating swapchain" : "VulkanRenderer: VSync disabled — recreating swapchain");
    m_vsync = enabled;
    if (!m_device || !m_swapchain) return;

    recreateSwapchain();   // picks up the new present mode via m_vsync
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: swapchain recreated");
}

// Validation messages → engine Logger so Vulkan API misuse is visible without a
// debugger/external tooling (the Vulkan analog of the D3D12 InfoQueue drain).
static VKAPI_ATTR VkBool32 VKAPI_CALL vkDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/)
{
    if (!data || !data->pMessage) return VK_FALSE;
    const auto lvl = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                         ? Logger::LogLevel::Error
                     : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                         ? Logger::LogLevel::Warning
                         : Logger::LogLevel::Info;
    // Only surface warnings/errors — info/verbose would flood the log.
    if (lvl != Logger::LogLevel::Info)
        Logger::LogTo(HE::Log::Cat::RHI, lvl, (std::string("Vulkan validation: ") + data->pMessage).c_str());
    return VK_FALSE;
}

void VulkanRenderer::createInstance()
{
    uint32_t extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);

    std::vector<const char*> exts(sdlExts, sdlExts + extCount);
    std::vector<const char*> layers;

    // GPU debug instrumentation (validation layer). TEMPORARILY forced ON while the Vulkan
    // backend is still broken (black screen) so the user gets a validation log with zero
    // setup. Re-gate to `_DEBUG || std::getenv("HE_GPU_DEBUG")` once Vulkan renders.
    const bool gpuDebug = true;
    (void)0;
    bool haveDebugUtils = false;
    if (gpuDebug)
    {
        // Enable the KHRONOS validation layer + debug-utils extension ONLY when actually
        // present (Vulkan SDK installed), so a machine without them still initialises.
        bool haveValidation = false;
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        if (n) vkEnumerateInstanceLayerProperties(&n, props.data());
        for (const auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) { haveValidation = true; break; }

        uint32_t en = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> eprops(en);
        if (en) vkEnumerateInstanceExtensionProperties(nullptr, &en, eprops.data());
        for (const auto& e : eprops)
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) { haveDebugUtils = true; break; }
    }
    if (haveValidation) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (haveDebugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    HE_LOG_INFO(RHI, "%s",
        haveValidation ? "VulkanRenderer: validation layer ENABLED (HE_GPU_DEBUG)"
                       : "VulkanRenderer: validation layer requested but NOT available");
    }

    // Vulkan 1.2 is required for the HW ray-query GI path (SPIR-V 1.4+,
    // buffer-device-address core). A 1.0 loader has no
    // vkEnumerateInstanceVersion — fetch it dynamically; if the loader can't
    // do 1.2 we fall back to 1.0 (everything else in this backend is 1.0-
    // compatible) and the HW-RT gate in createDevice() stays off.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (auto enumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        enumVer(&loaderVersion);
    m_instanceApiVersion = loaderVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2
                                                               : VK_API_VERSION_1_0;

    VkApplicationInfo appInfo{};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "HorizonEngine";
    appInfo.apiVersion       = m_instanceApiVersion;
    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &appInfo;
    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    vkCheck(vkCreateInstance(&ci, nullptr, &m_instance), "vkCreateInstance");

    if (gpuDebug && haveDebugUtils)
    {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT dci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = vkDebugCallback;
            create(m_instance, &dci, nullptr, &m_debugMessenger);
        }
    }
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

    // ── Hardware ray tracing probe (VK_KHR_ray_query in compute) ────────────
    // Needs Vulkan 1.2 (instance AND device), the three KHR extensions and the
    // accelerationStructure + rayQuery + bufferDeviceAddress features. Any
    // miss → SW GI only (no behaviour change); HE_GI_FORCE_SW=1 skips the
    // probe entirely, mirroring the Metal backend's override.
    std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    bool wantHwRt = false;
    if (const char* force = std::getenv("HE_GI_FORCE_SW"); force && *force && *force != '0')
    {
        HE_LOG_INFO(RHI, "%s",
                    "VulkanRenderer: HE_GI_FORCE_SW set — software GI path forced");
    }
    else if (m_instanceApiVersion >= VK_API_VERSION_1_2)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physDevice, &props);
        bool hasAccel = false, hasRayQuery = false, hasDefHostOps = false;
        {
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(m_physDevice, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> eprops(n);
            if (n) vkEnumerateDeviceExtensionProperties(m_physDevice, nullptr, &n, eprops.data());
            for (const auto& e : eprops)
            {
                if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) hasAccel = true;
                else if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) hasRayQuery = true;
                else if (std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) hasDefHostOps = true;
            }
        }
        if (props.apiVersion >= VK_API_VERSION_1_2 && hasAccel && hasRayQuery && hasDefHostOps)
        {
            VkPhysicalDeviceFeatures2 feat2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            feat2.pNext  = &asFeat;
            asFeat.pNext = &rqFeat;
            rqFeat.pNext = &bdaFeat;
            vkGetPhysicalDeviceFeatures2(m_physDevice, &feat2);
            if (asFeat.accelerationStructure && rqFeat.rayQuery && bdaFeat.bufferDeviceAddress)
                wantHwRt = true;
        }
        if (wantHwRt)
        {
            // Scratch-offset alignment for AS builds (needed to place the
            // scratch device address correctly).
            VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
            VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props2.pNext = &asProps;
            vkGetPhysicalDeviceProperties2(m_physDevice, &props2);
            m_giAsScratchAlign = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 1);

            devExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            devExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            devExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }
    }

    // Fresh feature structs for device creation: enable ONLY what we use.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rqEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceBufferDeviceAddressFeatures bdaEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    asEnable.accelerationStructure = VK_TRUE;
    rqEnable.rayQuery              = VK_TRUE;
    bdaEnable.bufferDeviceAddress  = VK_TRUE;
    asEnable.pNext = &rqEnable;
    rqEnable.pNext = &bdaEnable;

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = wantHwRt ? &asEnable : nullptr;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    vkCheck(vkCreateDevice(m_physDevice, &dci, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);

    if (wantHwRt)
    {
        // KHR entry points — device-level, so vkGetDeviceProcAddr.
        m_pfnCreateAS = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            vkGetDeviceProcAddr(m_device, "vkCreateAccelerationStructureKHR"));
        m_pfnDestroyAS = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(m_device, "vkDestroyAccelerationStructureKHR"));
        m_pfnGetASBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureBuildSizesKHR"));
        m_pfnCmdBuildAS = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(m_device, "vkCmdBuildAccelerationStructuresKHR"));
        m_pfnGetASAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureDeviceAddressKHR"));
        m_pfnGetBufferAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddressKHR"));
        // Core-1.2 alias fallback (some drivers only export the core name).
        if (!m_pfnGetBufferAddress)
            m_pfnGetBufferAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddress"));
        m_giHwRt = m_pfnCreateAS && m_pfnDestroyAS && m_pfnGetASBuildSizes &&
                   m_pfnCmdBuildAS && m_pfnGetASAddress && m_pfnGetBufferAddress;
        HE_LOG_INFO(RHI, "%s",
                    m_giHwRt ? "VulkanRenderer: GI hardware ray tracing available (VK_KHR_ray_query)"
                             : "VulkanRenderer: HW-RT entry points missing — software GI path");
    }
    else
    {
        HE_LOG_INFO(RHI, "%s",
                    "VulkanRenderer: GI uses software ray tracing (no VK_KHR_ray_query)");
    }
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
    // Per frame-in-flight: acquire semaphore + submit fence.
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        vkCheck(vkCreateSemaphore(m_device, &sci, nullptr, &m_imageReady[i]), "imageReady");
        vkCheck(vkCreateFence    (m_device, &fci, nullptr, &m_frameFence[i]), "frameFence");
    }
    createImageSyncObjects();
}

// Present semaphores are PER SWAPCHAIN IMAGE (not per frame-in-flight): the present op
// holds the semaphore until the image is actually scanned out, which the per-frame fence
// does NOT track — reusing a per-frame present semaphore trips "semaphore still in use by
// swapchain". m_imagesInFlight[img] tracks which frame's fence last used each image so we
// can wait before reusing that image's command buffer. Recreated with the swapchain since
// the image count can change.
void VulkanRenderer::createImageSyncObjects()
{
    for (VkSemaphore s : m_renderDone) if (s) vkDestroySemaphore(m_device, s, nullptr);
    m_renderDone.clear();
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    m_renderDone.resize(m_framebuffers.size());
    for (auto& s : m_renderDone)
        vkCheck(vkCreateSemaphore(m_device, &sci, nullptr, &s), "renderDone(perImage)");
    m_imagesInFlight.assign(m_framebuffers.size(), VK_NULL_HANDLE);
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
    createImageSyncObjects();                            // image count may have changed
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
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
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
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: secondary window attached");
}

void VulkanRenderer::DetachWindow(HE::Window* window)
{
    auto it = m_extraWindows.find(window->GetNativeWindow());
    if (it == m_extraWindows.end()) return;
    destroyWindowData(it->second);
    m_extraWindows.erase(it);
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: secondary window detached");
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
        HE_LOG_WARN(RHI, "%s", ("VulkanRenderer: shader not found — " + path).c_str());
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
    // Descriptor set: binding 0 = per-frame UBO, binding 1 = shadow map,
    //                 binding 2 = per-draw material UBO, binding 3 = SSAO AO texture.
    VkDescriptorSetLayoutBinding binds[8]{};
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
    binds[3].binding         = 3;  // uAO: SSAO occlusion (1x1 white fallback when disabled)
    binds[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[3].descriptorCount = 1;
    binds[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Bindings 4-7: ray-traced GI mask + DDGI probe atlases + per-pixel
    // local-light mask (white fallbacks when GI is off — giParams.y == 0
    // gates all sampling in scene.frag).
    for (uint32_t gb = 4; gb <= 7; ++gb)
    {
        binds[gb].binding         = gb;
        binds[gb].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[gb].descriptorCount = 1;
        binds[gb].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 8;
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

    // Base-color descriptor set layout (set = 2: one combined image sampler, fragment stage)
    // + an empty layout to fill the scene pipeline's unused set-1 slot (skinned uses set 1
    // for its bones UBO, so uAlbedo lives at set 2 in the shared scene.frag).
    {
        VkDescriptorSetLayoutBinding ab{};
        ab.binding         = 0;
        ab.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ab.descriptorCount = 1;
        ab.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo aslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        aslci.bindingCount = 1;
        aslci.pBindings    = &ab;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &aslci, nullptr, &m_albedoSetLayout), "albedo set layout");

        VkDescriptorSetLayoutCreateInfo eslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        eslci.bindingCount = 0;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &eslci, nullptr, &m_emptySetLayout), "empty set layout");
    }

    // Pipeline layout: scene set (0) + empty (1) + albedo (2) + per-object push constants.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(PushConstants);
    VkDescriptorSetLayout sceneSets[3] = { m_sceneSetLayout, m_emptySetLayout, m_albedoSetLayout };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 3;
    plci.pSetLayouts            = sceneSets;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_scenePipelineLayout), "pipeline layout");

    // Per-frame UBO buffers + descriptor sets (one per frame in flight).
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 2 },  // binding0 + binding2
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight * 6 },  // binding1(shadow) + binding3(AO) + bindings4-7(GI)
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
        // Binding 3 (AO): filled in by createSSAOPipeline() with the white fallback
        // view after it creates the 1x1 white texture; left unwritten here.
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

    // ── Base-color texture sampler, per-mesh descriptor pool, and 1x1 white default ──
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_albedoSampler), "albedo sampler");

        // One combined-image-sampler set per unique textured mesh + override material
        // (+1 for the white default). FREE bit so hot-reload can recycle invalidated sets.
        VkDescriptorPoolSize aps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxMeshTextures + 1 };
        VkDescriptorPoolCreateInfo adp{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        adp.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        adp.maxSets       = k_maxMeshTextures + 1;
        adp.poolSizeCount = 1;
        adp.pPoolSizes    = &aps;
        vkCheck(vkCreateDescriptorPool(m_device, &adp, nullptr, &m_albedoPool), "albedo pool");

        // 1x1 opaque-white default so untextured meshes still bind a valid set-2 descriptor.
        const uint8_t white[4] = { 255, 255, 255, 255 };
        if (uploadRGBA8Image(white, 1, 1, m_whiteAlbedoImage, m_whiteAlbedoMem, m_whiteAlbedoView))
        {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool     = m_albedoPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts        = &m_albedoSetLayout;
            if (vkAllocateDescriptorSets(m_device, &dsai, &m_whiteAlbedoSet) == VK_SUCCESS)
            {
                VkDescriptorImageInfo dii{ m_albedoSampler, m_whiteAlbedoView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet ww{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                ww.dstSet          = m_whiteAlbedoSet;
                ww.dstBinding      = 0;
                ww.descriptorCount = 1;
                ww.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ww.pImageInfo      = &dii;
                vkUpdateDescriptorSets(m_device, 1, &ww, 0, nullptr);
            }
        }
        if (!m_whiteAlbedoSet)
            HE_LOG_ERROR(RHI, "%s",
                "VulkanRenderer: white base-color default failed — untextured meshes will be skipped");
    }

    // A3: per-frame instance-transform vertex buffer (host-visible + mapped), used by
    // the instanced scene pipeline at binding 1.
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ibci.size  = static_cast<VkDeviceSize>(k_maxInstances) * k_instStride;
        ibci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vkCheck(vkCreateBuffer(m_device, &ibci, nullptr, &m_instanceBuf[i].buf), "instance buffer");
        VkMemoryRequirements ireq{}; vkGetBufferMemoryRequirements(m_device, m_instanceBuf[i].buf, &ireq);
        VkMemoryAllocateInfo imai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imai.allocationSize  = ireq.size;
        imai.memoryTypeIndex = findMemoryType(ireq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &imai, nullptr, &m_instanceBuf[i].mem), "instance memory");
        vkBindBufferMemory(m_device, m_instanceBuf[i].buf, m_instanceBuf[i].mem, 0);
        vkMapMemory(m_device, m_instanceBuf[i].mem, 0, ibci.size, 0, &m_instanceBuf[i].mapped);
    }

    VkShaderModule vs = loadShaderModule("scene.vert.spv");
    VkShaderModule fs = loadShaderModule("scene.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: scene shaders missing — scene will not draw");
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
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: graphics pipeline creation failed");

    // A3: instanced variant — same fragment shader + states, VS reads per-instance
    // mvp+model from binding 1 (VK_VERTEX_INPUT_RATE_INSTANCE). Created from `pci`
    // BEFORE the transparent block below mutates it (opaque config).
    if (VkShaderModule ivs = loadShaderModule("scene_instanced.vert.spv"))
    {
        VkPipelineShaderStageCreateInfo istages[2] = { stages[0], stages[1] };
        istages[0].module = ivs;
        VkVertexInputBindingDescription ibinds[2] = {
            { 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, k_instStride,       VK_VERTEX_INPUT_RATE_INSTANCE },
        };
        VkVertexInputAttributeDescription iattrs[11] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0 },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,   12 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,      24 },
            { 3,  1, VK_FORMAT_R32G32B32A32_SFLOAT,   0 },
            { 4,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  16 },
            { 5,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  32 },
            { 6,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  48 },
            { 7,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  64 },
            { 8,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  80 },
            { 9,  1, VK_FORMAT_R32G32B32A32_SFLOAT,  96 },
            { 10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 112 },
        };
        VkPipelineVertexInputStateCreateInfo ivi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        ivi.vertexBindingDescriptionCount   = 2;
        ivi.pVertexBindingDescriptions      = ibinds;
        ivi.vertexAttributeDescriptionCount = 11;
        ivi.pVertexAttributeDescriptions    = iattrs;
        VkGraphicsPipelineCreateInfo ipci = pci;
        ipci.pStages           = istages;
        ipci.pVertexInputState = &ivi;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ipci, nullptr, &m_sceneInstancedPipeline) != VK_SUCCESS)
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instanced scene pipeline creation failed");
        vkDestroyShaderModule(m_device, ivs, nullptr);
    }

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
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: transparent pipeline creation failed");
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
                HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: shadow pipeline creation failed");
            vkDestroyShaderModule(m_device, svs, nullptr);
        }
    }

    // A4: build the material-graph descriptor-set layout, pipeline layout, per-frame
    // descriptor pools and UBO ring buffers once (the white default + albedo sampler this
    // path binds already exist from the block above). No-op when HE_HAVE_SHADERC is off.
    createMaterialResources();
}

void VulkanRenderer::destroyScenePipeline()
{
    destroyMaterialResources();

    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_frameUBO[i].buf) vkDestroyBuffer(m_device, m_frameUBO[i].buf, nullptr);
        if (m_frameUBO[i].mem) vkFreeMemory   (m_device, m_frameUBO[i].mem, nullptr);
        m_frameUBO[i] = {};
        if (m_instanceBuf[i].mem)    vkUnmapMemory(m_device, m_instanceBuf[i].mem);
        if (m_instanceBuf[i].buf)    vkDestroyBuffer(m_device, m_instanceBuf[i].buf, nullptr);
        if (m_instanceBuf[i].mem)    vkFreeMemory   (m_device, m_instanceBuf[i].mem, nullptr);
        m_instanceBuf[i] = {};
    }
    if (m_matUBO)              { vkDestroyBuffer(m_device, m_matUBO, nullptr); m_matUBO = VK_NULL_HANDLE; }
    if (m_matMem)              { vkFreeMemory   (m_device, m_matMem, nullptr); m_matMem = VK_NULL_HANDLE; }
    if (m_descPool)            { vkDestroyDescriptorPool(m_device, m_descPool, nullptr);            m_descPool = VK_NULL_HANDLE; }
    // Base-color texture resources (per-mesh images are freed with their caches; the pool
    // destroy frees every albedo descriptor set incl. m_whiteAlbedoSet).
    if (m_albedoPool)          { vkDestroyDescriptorPool(m_device, m_albedoPool, nullptr);          m_albedoPool = VK_NULL_HANDLE; }
    if (m_whiteAlbedoView)     { vkDestroyImageView(m_device, m_whiteAlbedoView, nullptr);          m_whiteAlbedoView = VK_NULL_HANDLE; }
    if (m_whiteAlbedoImage)    { vkDestroyImage(m_device, m_whiteAlbedoImage, nullptr);             m_whiteAlbedoImage = VK_NULL_HANDLE; }
    if (m_whiteAlbedoMem)      { vkFreeMemory(m_device, m_whiteAlbedoMem, nullptr);                 m_whiteAlbedoMem = VK_NULL_HANDLE; }
    if (m_albedoSampler)       { vkDestroySampler(m_device, m_albedoSampler, nullptr);              m_albedoSampler = VK_NULL_HANDLE; }
    if (m_albedoSetLayout)     { vkDestroyDescriptorSetLayout(m_device, m_albedoSetLayout, nullptr); m_albedoSetLayout = VK_NULL_HANDLE; }
    if (m_emptySetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_emptySetLayout, nullptr);  m_emptySetLayout = VK_NULL_HANDLE; }
    if (m_shadowPipeline)                { vkDestroyPipeline(m_device, m_shadowPipeline, nullptr);                m_shadowPipeline = VK_NULL_HANDLE; }
    if (m_sceneTransparentPipelineHDR)   { vkDestroyPipeline(m_device, m_sceneTransparentPipelineHDR, nullptr);   m_sceneTransparentPipelineHDR = VK_NULL_HANDLE; }
    if (m_sceneInstancedPipelineHDR)     { vkDestroyPipeline(m_device, m_sceneInstancedPipelineHDR, nullptr);     m_sceneInstancedPipelineHDR = VK_NULL_HANDLE; }
    if (m_sceneInstancedPipeline)        { vkDestroyPipeline(m_device, m_sceneInstancedPipeline, nullptr);        m_sceneInstancedPipeline = VK_NULL_HANDLE; }
    if (m_scenePipelineHDR)              { vkDestroyPipeline(m_device, m_scenePipelineHDR, nullptr);              m_scenePipelineHDR = VK_NULL_HANDLE; }
    if (m_sceneTransparentPipeline)      { vkDestroyPipeline(m_device, m_sceneTransparentPipeline, nullptr);      m_sceneTransparentPipeline = VK_NULL_HANDLE; }
    if (m_scenePipeline)                 { vkDestroyPipeline(m_device, m_scenePipeline, nullptr);                 m_scenePipeline = VK_NULL_HANDLE; }
    if (m_scenePipelineLayout) { vkDestroyPipelineLayout(m_device, m_scenePipelineLayout, nullptr); m_scenePipelineLayout = VK_NULL_HANDLE; }
    if (m_sceneSetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_sceneSetLayout, nullptr); m_sceneSetLayout = VK_NULL_HANDLE; }
}

// ─────────────────────────────────────────────────────────────────────────────
// A4: node-graph material pipelines
//
// Graph materials render through per-material VkPipelines the engine builds at draw
// time from MaterialShaderLibrary SPIR-V (VS + FS). They all share ONE descriptor-set
// layout / pipeline layout and, per frame in flight, a descriptor pool (reset whole
// each frame) plus host-visible UBO rings for the per-object `U` block and per-draw
// `HeParams`, and a single `HeLighting` buffer filled once per frame. Mirrors the GL
// reference (OpenGLRenderer::GetOrBuildMaterialProgram + its draw integration).
// ─────────────────────────────────────────────────────────────────────────────
void VulkanRenderer::createMaterialResources()
{
#if defined(HE_HAVE_SHADERC)
    // The material fragment samples the white default at heTex0/heTexP0..3 for this
    // first increment; without it (or the shared albedo sampler) there is nothing valid
    // to bind, so leave the path disabled rather than sample an unbound descriptor.
    if (!m_whiteAlbedoView || !m_albedoSampler)
    {
        HE_LOG_WARN(RHI, "%s",
            "VulkanRenderer: A4 material path disabled — white base-color default unavailable");
        return;
    }

    // 1-layer 2D-ARRAY view over the white image for the heCsm default (binding 12).
    // viewType 2D_ARRAY over a plain VK_IMAGE_TYPE_2D image with layerCount 1 is legal.
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_whiteAlbedoImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &m_whiteArrayView) != VK_SUCCESS)
        {
            HE_LOG_WARN(RHI, "%s",
                "VulkanRenderer: A4 material path disabled — white array view failed");
            return;
        }
    }

    // ── Descriptor set 0 layout: canonical bindings 0-7 (matches the generated SPIR-V)
    //    + 8/9 for the WPO custom vertex, which reads HeLighting/HeParams in the VERTEX
    //    stage at those slots (MaterialShaderLibrary.cpp kWpoUniforms). Extra bindings are
    //    harmless for the standard (non-WPO) vertex, which references none of them. ──
    VkDescriptorSetLayoutBinding b[13]{};
    auto setB = [&](int i, uint32_t binding, VkDescriptorType type, VkShaderStageFlags stage) {
        b[i].binding = binding; b[i].descriptorType = type; b[i].descriptorCount = 1; b[i].stageFlags = stage;
    };
    setB(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT); // HeLighting
    setB(1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT);   // U (per object)
    setB(2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heTex0
    setB(3, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT); // HeParams
    setB(4, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heTexP0
    setB(5, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heTexP1
    setB(6, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heTexP2
    setB(7, 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heTexP3
    setB(8, 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT);   // HeLighting (WPO VS)
    setB(9, 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_VERTEX_BIT);   // HeParams   (WPO VS)
    setB(10, 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heGIShadow (GI sun mask)
    setB(11, 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heGILocal (GI local mask)
    setB(12, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT); // heCsm (CSM fallback, 2D array)
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 13;
    slci.pBindings    = b;
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_matSetLayout) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: A4 material descriptor-set layout failed");
        return;
    }

    // Pipeline layout: one set, no push constants (per-object data goes through the U UBO).
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &m_matSetLayout;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_matPipelineLayout) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: A4 material pipeline layout failed");
        return;
    }

    // Per-frame descriptor pool (reset whole each frame — no FREE bit) + UBO buffers.
    // Each allocated set consumes 5 UBO descriptors (b0/b1/b3/b8/b9) and 5 samplers
    // (b2/b4..7), so the pool must cover k_matMaxDraws sets worth of each.
    auto makeBuf = [&](VkDeviceSize size, MatFrameBuf& mb) -> bool {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &mb.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, mb.buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mb.mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, mb.buf, mb.mem, 0);
        return vkMapMemory(m_device, mb.mem, 0, size, 0, &mb.mapped) == VK_SUCCESS;
    };

    const VkDeviceSize ringSize = static_cast<VkDeviceSize>(k_matMaxDraws) * k_matSlotStride;
    for (uint32_t f = 0; f < k_maxFramesInFlight; ++f)
    {
        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         5u * k_matMaxDraws }, // b0,b1,b3,b8,b9
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8u * k_matMaxDraws }, // b2,b4-b7,b10-b12
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_matMaxDraws;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_matPool[f]) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: A4 material descriptor pool failed");
            return;
        }
        // NOTE: sized to the FULL Lighting struct — this was 64 (the v1 sun-only
        // block) while the fill site memcpy'd sizeof(Lighting), overflowing the
        // mapped allocation ever since the v2 8-light window landed.
        if (!makeBuf(sizeof(HE::MaterialShaderLibrary::Lighting), m_matLightBuf[f])
            || !makeBuf(ringSize, m_matObjBuf[f]) || !makeBuf(ringSize, m_matParBuf[f]))
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: A4 material UBO ring allocation failed");
            return;
        }
    }

    m_matReady = true;
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: A4 material resources created");
#endif
}

void VulkanRenderer::destroyMaterialResources()
{
#if defined(HE_HAVE_SHADERC)
    m_matReady = false;
    for (auto& kv : m_materialPipelines)
        if (kv.second) vkDestroyPipeline(m_device, kv.second, nullptr);
    m_materialPipelines.clear();
    for (uint32_t f = 0; f < k_maxFramesInFlight; ++f)
    {
        for (MatFrameBuf* mb : { &m_matLightBuf[f], &m_matObjBuf[f], &m_matParBuf[f] })
        {
            if (mb->mem) vkUnmapMemory(m_device, mb->mem);
            if (mb->buf) vkDestroyBuffer(m_device, mb->buf, nullptr);
            if (mb->mem) vkFreeMemory   (m_device, mb->mem, nullptr);
            *mb = {};
        }
        if (m_matPool[f]) { vkDestroyDescriptorPool(m_device, m_matPool[f], nullptr); m_matPool[f] = VK_NULL_HANDLE; }
        m_matDrawCursor[f] = 0;
    }
    if (m_matPipelineLayout) { vkDestroyPipelineLayout(m_device, m_matPipelineLayout, nullptr); m_matPipelineLayout = VK_NULL_HANDLE; }
    if (m_matSetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_matSetLayout, nullptr); m_matSetLayout = VK_NULL_HANDLE; }
    if (m_whiteArrayView)    { vkDestroyImageView(m_device, m_whiteArrayView, nullptr);         m_whiteArrayView = VK_NULL_HANDLE; }
#endif
}

VkPipeline VulkanRenderer::GetOrBuildMaterialPipeline(uint64_t hash, const std::string& frag,
                                                      const std::string& vertBody, bool hdr,
                                                      bool transparent, VkRenderPass rpOverride)
{
#if defined(HE_HAVE_SHADERC)
    // Cache key mixes the shader hash with the render-target + blend variant so LDR (swapchain)
    // / HDR (RGBA16F offscreen) / opaque / transparent pipelines never collide.
    // The rpOverride bit is a FIXED salt, not the VkRenderPass value: handles are
    // not stable across recreation, and the salt only has to separate this variant
    // from the LDR one — which WarmupMaterials caches under the bare hash and
    // builds for m_renderPass (a different colour format), so a collision would
    // hand the preview pass an incompatible pipeline.
    const uint64_t key = hash ^ (hdr ? 0x9E3779B97F4A7C15ULL : 0ULL)
                              ^ (transparent ? 0xD1B54A32D192ED03ULL : 0ULL)
                              ^ (rpOverride != VK_NULL_HANDLE ? 0xA24BAED4963EE407ULL : 0ULL);
    if (auto it = m_materialPipelines.find(key); it != m_materialPipelines.end()) return it->second;

    VkRenderPass rp = rpOverride != VK_NULL_HANDLE ? rpOverride
                                                   : (hdr ? m_postFxSceneRP : m_renderPass);
    if (rp == VK_NULL_HANDLE)
        return VK_NULL_HANDLE; // target pass not ready yet — retry next frame (not cached)

    using Backend = HE::MaterialShaderLibrary::Backend;
    // Standard vertex (no WPO) or the graph's custom vertex body, cross-compiled to SPIR-V.
    const HE::MaterialShaderLibrary::Compiled& vc = vertBody.empty()
        ? m_matShaderLib.standardVertex(Backend::SpirV)
        : m_matShaderLib.customVertex(std::hash<std::string>{}(vertBody), vertBody, Backend::SpirV);
    const HE::MaterialShaderLibrary::Compiled& fc = m_matShaderLib.fragment(hash, frag, Backend::SpirV);
    if (!vc.ok || !fc.ok || vc.spirv.empty() || fc.spirv.empty())
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: A4 material shader cross-compile failed");
        m_materialPipelines.emplace(key, VK_NULL_HANDLE); // cache the miss — don't retry every draw
        return VK_NULL_HANDLE;
    }

    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule vs = makeModule(vc.spirv);
    VkShaderModule fs = makeModule(fc.spirv);
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: A4 material shader module creation failed");
        m_materialPipelines.emplace(key, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    // Vertex input identical to the scene pipeline (binding 0 = interleaved pos/normal/uv,
    // 32 B; the standard/custom vertex reads attribute locations 0/1/2).
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
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    // Transparent graph materials: depth-test but no depth-write + alpha blend, matching the
    // built-in transparent pipeline (m_sceneTransparentPipeline).
    ds.depthWriteEnable = transparent ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (transparent)
    {
        cba.blendEnable         = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    }
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
    pci.layout              = m_matPipelineLayout;
    pci.renderPass          = rp;
    pci.subpass             = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: A4 material pipeline creation failed");
        pipe = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
    m_materialPipelines.emplace(key, pipe); // cache success OR failure (null → no per-draw retry)
    return pipe;
#else
    (void)hash; (void)frag; (void)vertBody; (void)hdr;
    return VK_NULL_HANDLE;
#endif
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
    VkShaderModule smFS = loadShaderModule("postfx_smaa.frag.spv");
    VkShaderModule btFS = loadShaderModule("postfx_aa_blit.frag.spv");
    VkShaderModule brFS = loadShaderModule("postfx_bloom_bright.frag.spv");
    VkShaderModule blFS = loadShaderModule("postfx_bloom_blur.frag.spv");

    if (!vsM || !tmFS || !fxFS || !smFS || !btFS || !brFS || !blFS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: PostFX shaders missing — no HDR/bloom/FXAA");
        for (auto m : {vsM,tmFS,fxFS,smFS,btFS,brFS,blFS}) if (m) vkDestroyShaderModule(m_device,m,nullptr);
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
    makePipe(smFS, m_postFxFinalRP, m_smaaPipe);     // AA = SMAA
    makePipe(btFS, m_postFxFinalRP, m_aaBlitPipe);   // AA = Off

    for (auto m : {vsM,tmFS,fxFS,smFS,btFS,brFS,blFS}) vkDestroyShaderModule(m_device,m,nullptr);

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
                HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: HDR scene pipeline failed");
            // A3: instanced HDR variant (per-instance mvp+model at binding 1).
            if (VkShaderModule ivs2 = loadShaderModule("scene_instanced.vert.spv"))
            {
                VkPipelineShaderStageCreateInfo ist[2] = { st[0], st[1] };
                ist[0].module = ivs2;
                VkVertexInputBindingDescription ibnd[2] = {
                    { 0, 8u*sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
                    { 1, k_instStride,     VK_VERTEX_INPUT_RATE_INSTANCE },
                };
                VkVertexInputAttributeDescription iat[11] = {
                    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
                    { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 },
                    { 3,1,VK_FORMAT_R32G32B32A32_SFLOAT,0 }, { 4,1,VK_FORMAT_R32G32B32A32_SFLOAT,16 },
                    { 5,1,VK_FORMAT_R32G32B32A32_SFLOAT,32 }, { 6,1,VK_FORMAT_R32G32B32A32_SFLOAT,48 },
                    { 7,1,VK_FORMAT_R32G32B32A32_SFLOAT,64 }, { 8,1,VK_FORMAT_R32G32B32A32_SFLOAT,80 },
                    { 9,1,VK_FORMAT_R32G32B32A32_SFLOAT,96 }, { 10,1,VK_FORMAT_R32G32B32A32_SFLOAT,112 },
                };
                VkPipelineVertexInputStateCreateInfo ivi2{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
                ivi2.vertexBindingDescriptionCount=2; ivi2.pVertexBindingDescriptions=ibnd;
                ivi2.vertexAttributeDescriptionCount=11; ivi2.pVertexAttributeDescriptions=iat;
                VkGraphicsPipelineCreateInfo ipci2=pci2; ipci2.pStages=ist; ipci2.pVertexInputState=&ivi2;
                if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ipci2, nullptr, &m_sceneInstancedPipelineHDR) != VK_SUCCESS)
                    HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instanced HDR scene pipeline failed");
                vkDestroyShaderModule(m_device, ivs2, nullptr);
            }
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
                    HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: HDR transparent pipeline failed");
            }
            vkDestroyShaderModule(m_device, vs2, nullptr);
            vkDestroyShaderModule(m_device, fs2, nullptr);
        }
    }

    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: PostFX pipelines created");
}

void VulkanRenderer::destroyPostFXPipelines()
{
    m_postFxReady = false;
    for (auto p : {m_bloomBrightPipe,m_bloomBlurPipe,m_tonemapPipe,m_fxaaPipe,m_smaaPipe,m_aaBlitPipe})
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
    // UI overlay framebuffer — uses the same viewport image with LOAD_OP_LOAD render pass.
    if (m_uiViewportRP && m_viewportView)
        makeFB(m_uiViewportRP, m_viewportView, w, h, m_uiViewportFB);

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
    if (m_uiViewportFB) { vkDestroyFramebuffer(m_device, m_uiViewportFB, nullptr); m_uiViewportFB = VK_NULL_HANDLE; }
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
    destroySSAOTargets();
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
    // Retire the OLD color image/view/memory: ImGui's viewport descriptor still points at
    // them for the CURRENT frame (the editor only updates it next frame on
    // HasViewportResourceChanged). Destroying them now → ImGui samples a dead image → null
    // view → GPU fault → ~2s TDR → black. Freed a few frames later by the Render() sweep.
    if (m_viewportImage)
    {
        m_retiredViewports.push_back({ m_viewportImage, m_viewportView, m_viewportMemory,
                                       static_cast<int>(k_maxFramesInFlight) + 2 });
        m_viewportImage  = VK_NULL_HANDLE;
        m_viewportView   = VK_NULL_HANDLE;
        m_viewportMemory = VK_NULL_HANDLE;
    }
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
    createSSAOTargets(w, h);
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

    m_extractor.setContentManager(m_contentManager);
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

    const glm::mat4 lightClip = HE::kVulkanClipFix * m_renderWorld.shadow.viewProj;

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

bool VulkanRenderer::uploadRGBA8Image(const uint8_t* rgba, uint32_t width, uint32_t height,
                                      VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    if (!rgba || width == 0 || height == 0) return false;
    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * 4u;

    // Staging buffer (tightly packed — vkCmdCopyBufferToImage handles the row layout).
    VkBuffer       stageBuf = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = dataSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &stageBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, stageBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &stageMem) != VK_SUCCESS)
        { vkDestroyBuffer(m_device, stageBuf, nullptr); return false; }
        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
        void* ptr = nullptr;
        vkMapMemory(m_device, stageMem, 0, dataSize, 0, &ptr);
        std::memcpy(ptr, rgba, static_cast<size_t>(dataSize));
        vkUnmapMemory(m_device, stageMem);
    }

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &image) != VK_SUCCESS)
    { image = VK_NULL_HANDLE; vkDestroyBuffer(m_device, stageBuf, nullptr); vkFreeMemory(m_device, stageMem, nullptr); return false; }
    VkMemoryRequirements imr{}; vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imal, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkDestroyBuffer(m_device, stageBuf, nullptr); vkFreeMemory(m_device, stageMem, nullptr);
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    // One-shot command buffer: UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY.
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer oneCB = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
    VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(oneCB, &oneBI);
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask    = 0;
        bar.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { width, height, 1 };
    vkCmdCopyBufferToImage(oneCB, stageBuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    vkEndCommandBuffer(oneCB);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &oneCB;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);
    vkDestroyBuffer(m_device, stageBuf, nullptr);
    vkFreeMemory(m_device, stageMem, nullptr);

    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = image;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_device, &ivci, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);  memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Upload a cooked TextureAsset (RGBA8 or block-compressed BC7/BC3) with its full
// pre-baked mip chain to a device-local sampled image + view. All levels are staged
// into one buffer and copied per-mip; block formats use no runtime mip generation.
// Returns false when the device can't sample the shipped format (→ untextured draw).
bool VulkanRenderer::uploadTextureImage(const TextureAsset* tex,
                                        VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; view = VK_NULL_HANDLE;
    if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
        return false;
    const uint32_t width  = static_cast<uint32_t>(tex->width);
    const uint32_t height = static_cast<uint32_t>(tex->height);
    const uint32_t mips   = tex->mipLevels > 0 ? tex->mipLevels : 1;

    // Resolve the shipped format to a VkFormat + block flag. ASTC is Metal-only and
    // never packed for Vulkan; treat it (and anything unknown) as unsupported.
    VkFormat vkFmt; bool isBlock;
    switch (tex->format)
    {
    case TextureFormat::RGBA8: vkFmt = VK_FORMAT_R8G8B8A8_UNORM; isBlock = false; break;
    case TextureFormat::BC7:   vkFmt = VK_FORMAT_BC7_UNORM_BLOCK; isBlock = true;  break;
    case TextureFormat::BC3:   vkFmt = VK_FORMAT_BC3_UNORM_BLOCK; isBlock = true;  break;
    default: return false;
    }
    // Device must actually sample this format (BC support is optional in Vulkan).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_physDevice, vkFmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;

    // Per-level byte size + running offsets (level 0 first). Block: 16 B/4x4 block.
    auto levelBytes = [isBlock](uint32_t w, uint32_t h) -> VkDeviceSize {
        return isBlock ? static_cast<VkDeviceSize>((w + 3) / 4) * ((h + 3) / 4) * 16
                       : static_cast<VkDeviceSize>(w) * h * 4;
    };
    std::vector<VkDeviceSize> offsets(mips);
    VkDeviceSize total = 0;
    { uint32_t lw = width, lh = height;
      for (uint32_t l = 0; l < mips; ++l)
      { offsets[l] = total; total += levelBytes(lw, lh);
        lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1; } }
    if (tex->data.size() < total) return false; // truncated payload

    // Staging buffer holding the whole (already tightly packed) mip chain.
    VkBuffer       stageBuf = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = total;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &stageBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, stageBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &stageMem) != VK_SUCCESS)
        { vkDestroyBuffer(m_device, stageBuf, nullptr); return false; }
        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
        void* ptr = nullptr;
        vkMapMemory(m_device, stageMem, 0, total, 0, &ptr);
        std::memcpy(ptr, tex->data.data(), static_cast<size_t>(total));
        vkUnmapMemory(m_device, stageMem);
    }

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkFmt;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = mips;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &image) != VK_SUCCESS)
    { image = VK_NULL_HANDLE; vkDestroyBuffer(m_device, stageBuf, nullptr); vkFreeMemory(m_device, stageMem, nullptr); return false; }
    VkMemoryRequirements imr{}; vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imal, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkDestroyBuffer(m_device, stageBuf, nullptr); vkFreeMemory(m_device, stageMem, nullptr);
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    // One-shot command buffer: UNDEFINED → TRANSFER_DST → copy every mip → SHADER_READ.
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer oneCB = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
    VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(oneCB, &oneBI);
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
        bar.srcAccessMask    = 0;
        bar.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    { uint32_t lw = width, lh = height;
      for (uint32_t l = 0; l < mips; ++l)
      {
        VkBufferImageCopy region{};
        region.bufferOffset     = offsets[l]; // multiple of 16 → satisfies block/4-byte alignment
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
        region.imageExtent      = { lw, lh, 1 };
        vkCmdCopyBufferToImage(oneCB, stageBuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1;
      } }
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
        bar.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    vkEndCommandBuffer(oneCB);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &oneCB;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);
    vkDestroyBuffer(m_device, stageBuf, nullptr);
    vkFreeMemory(m_device, stageMem, nullptr);

    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = image;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = vkFmt;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    if (vkCreateImageView(m_device, &ivci, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);  memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool VulkanRenderer::resolveAndUploadAlbedo(const HE::UUID& materialId, const std::string& materialPath,
                                            VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                                            VkDescriptorSet& set)
{
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; view = VK_NULL_HANDLE; set = VK_NULL_HANDLE;
    if (!m_contentManager || !m_albedoPool || !m_albedoSetLayout) return false;
    const MaterialAsset* mat = m_contentManager->resolveMaterialRef(materialId, materialPath);
    if (!mat) return false;
    const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
    const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
    const TextureAsset* tex = m_contentManager->resolveTextureRef(texId0, texPath0);

    // RGBA8 + cooked BC7/BC3 (full pre-baked mip chain); uploadTextureImage returns
    // false when this device can't sample the shipped format → untextured fallback.
    if (!uploadTextureImage(tex, image, memory, view))
        return false;

    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = m_albedoPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_albedoSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS)
    {
        // Pool exhausted (> k_maxMeshTextures unique textured meshes) — drop to flat.
        vkDestroyImageView(m_device, view, nullptr);  view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr);     image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);      memory = VK_NULL_HANDLE;
        set = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo dii{ m_albedoSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet ww{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    ww.dstSet          = set;
    ww.dstBinding      = 0;
    ww.descriptorCount = 1;
    ww.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ww.pImageInfo      = &dii;
    vkUpdateDescriptorSets(m_device, 1, &ww, 0, nullptr);
    return true;
}

void VulkanRenderer::destroyMaterialTex(MaterialTexVk& mt)
{
    if (mt.set)   vkFreeDescriptorSets(m_device, m_albedoPool, 1, &mt.set); // pool has the FREE bit
    if (mt.view)  vkDestroyImageView(m_device, mt.view, nullptr);
    if (mt.image) vkDestroyImage(m_device, mt.image, nullptr);
    if (mt.mem)   vkFreeMemory(m_device, mt.mem, nullptr);
    mt = MaterialTexVk{};
}

bool VulkanRenderer::resolveMaterialOverride(const HE::UUID& materialId, const MaterialTexVk*& out)
{
    out = nullptr;
    if (materialId == HE::UUID{} || !m_contentManager || !m_albedoPool) return false;
    if (auto it = m_materialTexCache.find(materialId); it != m_materialTexCache.end())
    { out = &it->second; return true; }
    // Only cache once the material asset is loaded (mirrors GL: getMaterial null → retry).
    if (!m_contentManager->getMaterial(materialId)) return false;
    // Loaded → resolve its texture (set stays null when the material has no texture → flat).
    MaterialTexVk mt;
    resolveAndUploadAlbedo(materialId, std::string{}, mt.image, mt.mem, mt.view, mt.set);
    auto res = m_materialTexCache.emplace(materialId, mt);
    out = &res.first->second;
    return true;
}

void VulkanRenderer::processPendingInvalidations()
{
    if (m_pendingMatInval.empty() && m_pendingMeshInval.empty()) return;
    // Editor-only path; a full device idle keeps the frees below trivially safe (no in-flight
    // frame references the dropped resources). The idle is a stall, but invalidation is rare
    // outside a terrain-sculpt drag — a documented follow-up (D3D12 uses a per-frame retire list).
    vkDeviceWaitIdle(m_device);

    for (const HE::UUID& id : m_pendingMatInval)
        if (auto it = m_materialTexCache.find(id); it != m_materialTexCache.end())
        { destroyMaterialTex(it->second); m_materialTexCache.erase(it); }
    m_pendingMatInval.clear();

    for (const HE::UUID& id : m_pendingMeshInval)
    {
        if (auto it = m_meshCache.find(id); it != m_meshCache.end())
        {
            GpuMesh& m = it->second;
            if (m.vbuf)       vkDestroyBuffer(m_device, m.vbuf, nullptr);
            if (m.vmem)       vkFreeMemory(m_device, m.vmem, nullptr);
            if (m.ibuf)       vkDestroyBuffer(m_device, m.ibuf, nullptr);
            if (m.imem)       vkFreeMemory(m_device, m.imem, nullptr);
            if (m.albedoSet)  vkFreeDescriptorSets(m_device, m_albedoPool, 1, &m.albedoSet);
            if (m.albedoView) vkDestroyImageView(m_device, m.albedoView, nullptr);
            if (m.albedoImage)vkDestroyImage(m_device, m.albedoImage, nullptr);
            if (m.albedoMem)  vkFreeMemory(m_device, m.albedoMem, nullptr);
            m_meshCache.erase(it);
        }
        // GI BLAS ranges live in concatenated arrays — no single-mesh splice, an
        // edited mesh drops the whole cache (rebuilds lazily; same convention
        // as the GL/Metal-SW ports).
        if (m_giBlasCache.count(id))
        {
            m_giBlasCache.clear();
            m_giNodesCpu.clear();
            m_giTrisCpu.clear();
            m_giBlasDirty = true;
        }
        // HW BLAS is per-mesh (no concatenated arrays) — drop just this one;
        // it rebuilds lazily on the next updateGiAccel. Device is idle here.
        if (auto it = m_giHwBlasCache.find(id); it != m_giHwBlasCache.end())
        {
            destroyGiHwBlas(it->second);
            m_giHwBlasCache.erase(it);
        }
        if (auto it = m_skeletalMeshCache.find(id); it != m_skeletalMeshCache.end())
        {
            GpuSkeletalMesh& m = it->second;
            if (m.vb)         { vkDestroyBuffer(m_device, m.vb, nullptr);        vkFreeMemory(m_device, m.vbMem, nullptr); }
            if (m.boneIdVb)   { vkDestroyBuffer(m_device, m.boneIdVb, nullptr);  vkFreeMemory(m_device, m.boneIdMem, nullptr); }
            if (m.boneWgtVb)  { vkDestroyBuffer(m_device, m.boneWgtVb, nullptr); vkFreeMemory(m_device, m.boneWgtMem, nullptr); }
            if (m.ib)         { vkDestroyBuffer(m_device, m.ib, nullptr);        vkFreeMemory(m_device, m.ibMem, nullptr); }
            if (m.albedoSet)  vkFreeDescriptorSets(m_device, m_albedoPool, 1, &m.albedoSet);
            if (m.texView)    vkDestroyImageView(m_device, m.texView, nullptr);
            if (m.texImage)   vkDestroyImage(m_device, m.texImage, nullptr);
            if (m.texMem)     vkFreeMemory(m_device, m.texMem, nullptr);
            m_skeletalMeshCache.erase(it);
        }
    }
    m_pendingMeshInval.clear();
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
    if (!asset || asset->indices.empty() || (asset->vertices.empty() && !asset->cooked)) return nullptr;

    // Cooked (packaged) assets ship the interleaved pos+norm+uv buffer + baked
    // AABB, built once at pack time. Loose/editor assets interleave on first draw.
    GpuMesh mesh;
    std::vector<float> built;
    const std::vector<float>* vtx = &asset->interleaved;
    if (asset->cooked)
    {
        mesh.localBounds.min = { asset->boundsMin[0], asset->boundsMin[1], asset->boundsMin[2] };
        mesh.localBounds.max = { asset->boundsMax[0], asset->boundsMax[1], asset->boundsMax[2] };
    }
    else
    {
        const size_t vertexCount = asset->vertices.size() / 3;
        built.reserve(vertexCount * 8);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            built.insert(built.end(),
                { asset->vertices[i*3+0], asset->vertices[i*3+1], asset->vertices[i*3+2] });
            if (i * 3 + 2 < asset->normals.size())
                built.insert(built.end(),
                    { asset->normals[i*3+0], asset->normals[i*3+1], asset->normals[i*3+2] });
            else
                built.insert(built.end(), { 0.0f, 0.0f, 0.0f });
            if (i * 2 + 1 < asset->uvs.size())
                built.insert(built.end(), { asset->uvs[i*2+0], asset->uvs[i*2+1] });
            else
                built.insert(built.end(), { 0.0f, 0.0f });
        }
        vtx = &built;
        mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
    }
    const std::vector<float>& interleaved = *vtx;
    if (!createMeshBuffers(mesh, interleaved, asset->indices)) return nullptr;
    // Upload the mesh's baked base-color texture (if any) up front, so the cache entry is
    // fully resolved on insert. NOTE: this uses a blocking one-shot submit + vkQueueWaitIdle
    // (see uploadRGBA8Image), a one-time GPU stall on first sight of each textured mesh.
    resolveAndUploadAlbedo(asset->materialId, asset->materialPath,
                           mesh.albedoImage, mesh.albedoMem, mesh.albedoView, mesh.albedoSet);
    return &m_meshCache.emplace(assetId, mesh).first->second;
}

void VulkanRenderer::DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool hdr)
{
    if (!m_world || m_scenePipeline == VK_NULL_HANDLE || width == 0 || height == 0) return;

    // Feed time-of-day so the extractor recomputes the sun/moon direction (otherwise the
    // sky never responds to the time slider). Mirrors OpenGL/Metal.
    m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                            m_environment.sunColor, m_environment.sunIntensity,
                            m_environment.moonColor, m_environment.moonIntensity,
                            m_environment.cloudCoverage);
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld,
                        static_cast<float>(width) / static_cast<float>(height),
                        &m_editorCamera);


    // Sky is independent of scene geometry — draw it before any early returns so it
    // always renders even when the scene is empty or fully culled (otherwise the
    // viewport falls back to the gray clear and reads as black). Mirrors the
    // D3D12/OpenGL ordering. The render pass is already active (begun in Render());
    // the sky pipeline uses dynamic viewport/scissor, so set them for its triangle.
    {
        VkViewport svp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
        VkRect2D   ssc{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(cmd, 0, 1, &svp);
        vkCmdSetScissor(cmd, 0, 1, &ssc);
        drawSky(cmd, width, height, hdr);
    }

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
    m_statTotal   = static_cast<uint32_t>(m_renderWorld.objects.size());
    m_statVisible = static_cast<uint32_t>(m_sortedIndices.size());
    if (m_sortedIndices.empty()) return;

    if (m_renderGraph.empty())
        m_renderGraph.addPass(std::make_unique<GeometryPass>());

    // GL-convention projection from the extractor → Vulkan clip space.
    const glm::mat4 viewProj =
        HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;

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
        f.lightVP       = HE::kVulkanClipFix * m_renderWorld.shadow.viewProj;
        f.shadowEnabled = glm::ivec4(m_renderWorld.shadow.enabled ? 1 : 0, 0, 0, 0);
        f.sunDir        = glm::vec4(m_renderWorld.sunDirection, 0.0f);
        f.fog           = glm::vec4(m_environment.fogDensity, m_environment.fogHeightFalloff, 0.0f, 0.0f);
        // viewport.z == 1 iff runSSAO() completed this frame (blurRT holds valid data).
        // Using m_ssaoRanThisFrame (not just m_ssaoReady) ensures we never sample stale
        // blurRT in non-HDR paths or when SSAO was skipped due to an empty scene.
        f.viewport      = glm::vec4(float(width), float(height),
                                    m_ssaoRanThisFrame ? 1.0f : 0.0f, 0.0f);
        // giParams.y == 1 iff runGi() completed this frame (mask + atlases hold
        // valid data and the scene set's bindings 4-6 point at them).
        f.giGridOrigin  = glm::vec4(m_giGridOrigin, kGIProbeSpacing);
        f.giGridCounts  = glm::vec4(float(m_giGridCounts.x), float(m_giGridCounts.y),
                                    float(m_giGridCounts.z), float(m_giProbesPerRow));
        f.giParams      = glm::vec4(m_giIndirectIntensity, m_giRanThisFrame ? 1.0f : 0.0f, 0.0f, 0.0f);
        if (m_frameUBO[m_currentFrame].mapped)
            std::memcpy(m_frameUBO[m_currentFrame].mapped, &f, sizeof(f));
    }

#if defined(HE_HAVE_SHADERC)
    // A4: recycle this frame slot's material descriptor sets (the frame fence waited on
    // in Render() guarantees the GPU finished with them) + reset the per-frame ring cursor,
    // then fill the shared HeLighting UBO once — identical for every graph-material draw.
    // DrawScene runs at most once per frame (viewport OR swapchain, gated by useViewport),
    // so this reset happens exactly once per frame. Matches OpenGLRenderer's HeLighting fill.
    if (m_matReady)
    {
        vkResetDescriptorPool(m_device, m_matPool[m_currentFrame], 0);
        m_matDrawCursor[m_currentFrame] = 0;

        HE::MaterialShaderLibrary::Lighting lit{};
        // Dominant directional (NOT the sky-dome sun/raw env colour — the
        // night/cloud lesson from the Metal/GL fill sites) + full light window.
        glm::vec3 matSunDir, matSunColor;
        m_renderWorld.dominantDirectionalLight(matSunDir, matSunColor);
        lit.sunDir[0] = matSunDir.x;
        lit.sunDir[1] = matSunDir.y;
        lit.sunDir[2] = matSunDir.z;
        // Engine seconds for the node graph's Time input (HE_SKY_TIME pins it for
        // deterministic headless captures, mirroring the sky clock and GL exactly).
        static const char* s_timeOv = std::getenv("HE_SKY_TIME");
        lit.sunDir[3] = (s_timeOv && *s_timeOv)
            ? static_cast<float>(std::atof(s_timeOv))
            : static_cast<float>(SDL_GetTicks()) / 1000.0f;
        const glm::vec3 sc = matSunColor;
        lit.sunColor[0] = sc.r; lit.sunColor[1] = sc.g; lit.sunColor[2] = sc.b;
        // Full light window for heLitP() — same first-8 order as the built-in
        // shaders. Shared fill (HE::FillMaterialLightWindow); Vulkan has no local
        // (point/spot) shadow atlas yet, so it passes false and lightParams[i].y
        // stays 0 = "casts no local shadow".
        HE::FillMaterialLightWindow(m_renderWorld, lit, /*localShadowsActive=*/false);
        lit.giParams[0] = static_cast<float>(width);
        lit.giParams[1] = static_cast<float>(height);
        lit.giParams[2] = m_giRanThisFrame ? 1.0f : 0.0f;
        lit.ambient[0] = m_renderWorld.ambient.r;
        lit.ambient[1] = m_renderWorld.ambient.g;
        lit.ambient[2] = m_renderWorld.ambient.b;
        lit.camPos[0] = m_renderWorld.camera.position.x;
        lit.camPos[1] = m_renderWorld.camera.position.y;
        lit.camPos[2] = m_renderWorld.camera.position.z;
        if (m_matLightBuf[m_currentFrame].mapped)
            std::memcpy(m_matLightBuf[m_currentFrame].mapped, &lit, sizeof(lit));
    }
#endif

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

        // Sky was already drawn at the top of DrawScene (before the empty-scene early
        // returns) so it shows even with no geometry. Re-bind scene pipeline + set.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                0, 1, &m_frameUBO[m_currentFrame].set, 0, nullptr);

        const glm::vec3 camPos = m_renderWorld.camera.position;
        // Shared opaque/blended split + back-to-front order. The split now weighs
        // the per-instance tint alpha too (this backend used to test dc.opacity
        // alone, so a particle fading out via its tint was drawn opaque).
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        RenderSorter::partitionByOpacity(cmds.drawCalls(), opaqueDCs, transparentDCs);
        RenderSorter::sortBackToFront(transparentDCs, camPos);

        // A3: real instancing applies to the opaque pass only (transparent keeps the
        // per-instance loop for blend + depth sort). instCursor sub-allocates the
        // per-frame instance buffer across the frame's instanced batches.
        bool allowInstancing = true;
        uint32_t instCursor = 0;
        // A4: the scene pipeline the CURRENT pass expects bound on entry to drawDCVk. A
        // graph-material draw binds its own per-material pipeline, so it restores THIS after
        // itself — otherwise the next built-in draw would inherit the material pipeline.
        // The opaque loop leaves it at the opaque scene pipe; the transparent loop sets it
        // to transPipe below.
        VkPipeline activeMatScenePipe = hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline;
        auto drawDCVk = [&](const DrawCall& dc) {
            const GpuMesh* mesh = resolveMesh(dc.meshAssetId);
            const GpuMesh& m    = mesh ? *mesh : m_cube;
            if (!m.indexCount) return;

#if defined(HE_HAVE_SHADERC)
            // A4: node-graph material? Render through a per-material pipeline built from the
            // MaterialShaderLibrary SPIR-V, bypassing the built-in PBR path entirely. Falls
            // through unchanged when the material has no graph shader OR resources are down.
            if (m_matReady && m_contentManager)
            {
                uint64_t matHash = 0; std::string matFrag, matVertBody;
                if (m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId,
                                                  matHash, matFrag, matVertBody))
                {
                    // Transparent graph materials get a blend-on / depth-write-off
                    // pipeline variant. MUST use the same predicate the opaque/blended
                    // partition above used, or a draw lands in the blended pass with a
                    // depth-writing pipeline (hence RenderSorter::isTransparent, tint
                    // alpha included, not a bare dc.opacity test).
                    const bool matTransp = RenderSorter::isTransparent(dc);
                    VkPipeline matPipe = GetOrBuildMaterialPipeline(matHash, matFrag, matVertBody,
                                                                    hdr, matTransp);
                    uint32_t& cursor = m_matDrawCursor[m_currentFrame];
                    if (matPipe != VK_NULL_HANDLE && cursor < k_matMaxDraws)
                    {
                        // Resolve the same PBR scalars / has-texture flag the built-in path uses.
                        bool matTextured = (m.albedoSet != VK_NULL_HANDLE);
                        const MaterialTexVk* matOvr = nullptr;
                        if (resolveMaterialOverride(dc.materialAssetId, matOvr))
                            matTextured = (matOvr->set != VK_NULL_HANDLE);

                        // Per-entity HeParams override wins over the material's shared params.
                        const MaterialAsset* ma = m_contentManager->getMaterial(dc.materialAssetId);
                        const std::vector<float>* params =
                            !dc.paramOverride.empty() ? &dc.paramOverride
                            : (ma && !ma->shaderParamData.empty() ? &ma->shaderParamData : nullptr);

                        VkDeviceSize voff = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &voff);
                        vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);

                        auto drawMatInstance = [&](const glm::mat4& model) {
                            if (cursor >= k_matMaxDraws) return;
                            const uint32_t i = cursor++;

                            // std140 U block (176 B) into ring slot i (256-B stride).
                            struct UBlock { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };
                            static_assert(sizeof(UBlock) == 176, "U block must be std140 176 B");
                            UBlock ub;
                            ub.mvp   = viewProj * model;
                            ub.model = model;
                            ub.color = glm::vec4(dc.baseColor, 1.0f);
                            ub.flags = glm::vec4(matTextured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                            ub.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity, 0.0f);
                            std::memcpy(static_cast<uint8_t*>(m_matObjBuf[m_currentFrame].mapped)
                                        + static_cast<size_t>(i) * k_matSlotStride, &ub, sizeof(ub));

                            // HeParams (16 vec4 = 64 floats = 256 B) into ring slot i, zero-padded.
                            float padded[64] = { 0.0f };
                            if (params)
                                std::memcpy(padded, params->data(),
                                            std::min(params->size(), size_t(64)) * sizeof(float));
                            std::memcpy(static_cast<uint8_t*>(m_matParBuf[m_currentFrame].mapped)
                                        + static_cast<size_t>(i) * k_matSlotStride, padded, sizeof(padded));

                            // One descriptor set per draw from this frame's pool (reset each frame).
                            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                            dsai.descriptorPool     = m_matPool[m_currentFrame];
                            dsai.descriptorSetCount = 1;
                            dsai.pSetLayouts        = &m_matSetLayout;
                            VkDescriptorSet set = VK_NULL_HANDLE;
                            if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) return;

                            const VkDeviceSize slot = static_cast<VkDeviceSize>(i) * k_matSlotStride;
                            VkDescriptorBufferInfo lightBI{ m_matLightBuf[m_currentFrame].buf, 0,
                                                            sizeof(HE::MaterialShaderLibrary::Lighting) };
                            VkDescriptorBufferInfo objBI  { m_matObjBuf[m_currentFrame].buf, slot, 176 };
                            VkDescriptorBufferInfo parBI  { m_matParBuf[m_currentFrame].buf, slot, 256 };
                            // heTex0 = the material's base texture: an override material's texture
                            // wins (A2), else the mesh's baked texture (A1), else the white default —
                            // matching the built-in path's texture selection + matTextured flag.
                            VkImageView tex0View = m_whiteAlbedoView;
                            if (matOvr) { if (matOvr->view) tex0View = matOvr->view; }
                            else if (m.albedoView) tex0View = m.albedoView;
                            VkDescriptorImageInfo tex0II{ m_albedoSampler, tex0View,
                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            // TODO A4-followup: heTexP0-3 = the graph's picked project textures
                            // (needs a UUID→view cache); bound to the white default for now.
                            VkDescriptorImageInfo whiteII{ m_albedoSampler, m_whiteAlbedoView,
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            VkWriteDescriptorSet w[13]{};
                            auto wr = [&](int idx, uint32_t binding, VkDescriptorType type,
                                          const VkDescriptorBufferInfo* bi, const VkDescriptorImageInfo* ii) {
                                w[idx].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                                w[idx].dstSet          = set;
                                w[idx].dstBinding      = binding;
                                w[idx].descriptorCount = 1;
                                w[idx].descriptorType  = type;
                                w[idx].pBufferInfo     = bi;
                                w[idx].pImageInfo      = ii;
                            };
                            wr(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &lightBI, nullptr);
                            wr(1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &objBI,   nullptr);
                            wr(2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &tex0II);
                            wr(3, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &parBI,   nullptr);
                            wr(4, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII);
                            wr(5, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII);
                            wr(6, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII);
                            wr(7, 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII);
                            wr(8, 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &lightBI, nullptr); // WPO VS
                            wr(9, 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &parBI,   nullptr); // WPO VS
                            // GI screen-space masks for heLitP(): sun mask + per-pixel
                            // local-light mask when GI ran this frame (white fallbacks
                            // otherwise; giParams.z gates sampling anyway). The local
                            // mask is a storage image and lives in GENERAL.
                            VkDescriptorImageInfo giSunII{ m_albedoSampler,
                                (m_giRanThisFrame && m_giResult.view) ? m_giResult.view : m_whiteAlbedoView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(10, 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &giSunII);
                            VkDescriptorImageInfo giLocalII{ m_albedoSampler,
                                (m_giRanThisFrame && m_giLocalMask.view) ? m_giLocalMask.view : m_whiteAlbedoView,
                                (m_giRanThisFrame && m_giLocalMask.view) ? VK_IMAGE_LAYOUT_GENERAL
                                                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(11, 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &giLocalII);
                            // heCsm (binding 12, sampler2DArray): Vulkan's shadow map is a
                            // single 2D map, so the CSM fallback stays off (csmSplits.w = 0)
                            // and the 1-layer white ARRAY view keeps the descriptor valid —
                            // a 2D view here would fail validation against the arrayed image
                            // type in the SPIR-V.
                            VkDescriptorImageInfo csmII{ m_albedoSampler, m_whiteArrayView,
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(12, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &csmII);
                            vkUpdateDescriptorSets(m_device, 13, w, 0, nullptr);

                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, matPipe);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    m_matPipelineLayout, 0, 1, &set, 0, nullptr);
                            vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
                            ++m_statDraws;
                            m_statTris += m.indexCount / 3;
                        };
                        // Instanced graph materials: draw each instance via the material path
                        // (this increment does NOT combine graph materials with A3 instancing).
                        if (!dc.instanceTransforms.empty())
                            for (const glm::mat4& t : dc.instanceTransforms) drawMatInstance(t);
                        else
                            drawMatInstance(dc.transform);

                        // Restore the pass's scene pipeline for subsequent built-in draws.
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
                        // The material path bound set 0 via m_matPipelineLayout, which is NOT
                        // compatible with m_scenePipelineLayout for set 0 (10-binding material set
                        // vs 4-binding scene set) → binding it disturbed the scene's per-frame set 0.
                        // Re-bind it so any built-in draw after a graph material reads the correct
                        // per-frame UBO (view-proj/lighting/shadow/AO), not the material descriptors.
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                m_scenePipelineLayout, 0, 1,
                                                &m_frameUBO[m_currentFrame].set, 0, nullptr);
                        return;
                    }
                }
            }
#endif
            // Base color: an explicit MaterialComponent override (dc.materialAssetId), once its
            // material is loaded, fully replaces the mesh's baked texture — even to flat.
            VkDescriptorSet albedoSet = m.albedoSet;             // baked (A1); null → flat
            bool textured = (m.albedoSet != VK_NULL_HANDLE);
            const MaterialTexVk* ovr = nullptr;
            if (resolveMaterialOverride(dc.materialAssetId, ovr))
            {
                albedoSet = ovr->set;                           // null when override has no texture
                textured  = (ovr->set != VK_NULL_HANDLE);
            }
            if (!albedoSet) albedoSet = m_whiteAlbedoSet;       // flat draws bind the white default

            if (m_matUBO)
            {
                struct MatData { float r,g,b,met; float rough,opacity,hasTex,pad; } md{
                    dc.baseColor.r, dc.baseColor.g, dc.baseColor.b, dc.metallic,
                    dc.roughness, dc.opacity, textured ? 1.0f : 0.0f, 0.0f
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

            // Bind the effective base-color texture at set 2 (albedoSet resolved above).
            // scene.frag samples set 2 unconditionally, so a valid descriptor MUST be bound;
            // if even the white default is missing, skip the draw rather than sample unbound.
            if (!albedoSet) return;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    2, 1, &albedoSet, 0, nullptr);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            auto drawOne = [&](const glm::mat4& model) {
                PushConstants pc2{ viewProj * model, model };
                vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(pc2), &pc2);
                vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
                ++m_statDraws;
                m_statTris += m.indexCount / 3;
            };
            if (!dc.instanceTransforms.empty())
            {
                // A3: real instancing — fill the per-frame instance buffer with every
                // instance's {mvp,model}, bind the instanced pipeline + binding 1, ONE draw.
                static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be mvp+model");
                const uint32_t count = static_cast<uint32_t>(dc.instanceTransforms.size());
                InstanceBuf& ib = m_instanceBuf[m_currentFrame];
                // Match the instanced pipeline to the active scene target (= what the
                // non-instanced bind resolves to). Null → fits=false → per-instance fallback
                // (never bind an LDR pipeline to the HDR render pass).
                VkPipeline instPipe = (hdr && m_scenePipelineHDR)
                                    ? m_sceneInstancedPipelineHDR : m_sceneInstancedPipeline;
                const bool fits = allowInstancing && instPipe && ib.mapped
                                  && (static_cast<uint64_t>(instCursor) + count) <= k_maxInstances;
                if (fits)
                {
                    auto* dst = static_cast<uint8_t*>(ib.mapped)
                              + static_cast<size_t>(instCursor) * k_instStride;
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        const glm::mat4& t = dc.instanceTransforms[k];
                        const glm::mat4 xf[2] = { viewProj * t, t }; // mvp, model (column-major)
                        std::memcpy(dst + static_cast<size_t>(k) * k_instStride, xf, sizeof(xf));
                    }
                    const VkDeviceSize instOff = static_cast<VkDeviceSize>(instCursor) * k_instStride;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instPipe);
                    vkCmdBindVertexBuffers(cmd, 1, 1, &ib.buf, &instOff);
                    vkCmdDrawIndexed(cmd, m.indexCount, count, 0, 0, 0);
                    // Restore the non-instanced pipeline for subsequent (non-instanced) draws.
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
                    ++m_statDraws;
                    m_statTris += (m.indexCount / 3) * count;
                    instCursor += count;
                }
                else
                {
                    for (const glm::mat4& t : dc.instanceTransforms) drawOne(t); // fallback
                }
            }
            else
                drawOne(dc.transform);
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
        for (const DrawCall* dc : opaqueDCs) drawDCVk(*dc);

        const VkPipeline transPipe = hdr && m_sceneTransparentPipelineHDR
            ? m_sceneTransparentPipelineHDR : m_sceneTransparentPipeline;
        if (!transparentDCs.empty() && transPipe) {
            allowInstancing = false; // transparent batches keep the per-instance loop (blend + depth sort)
            activeMatScenePipe = transPipe; // A4: graph-material draws restore THIS in the transparent pass
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transPipe);
            for (const DrawCall* dc : transparentDCs) drawDCVk(*dc);
        }

        // ── Skinned mesh draw loop ────────────────────────────────────────────
        // Runs after opaque+transparent geometry so blending and depth test are
        // already resolved. Uses skinned.vert + scene.frag (set=0 still bound).
        // NOTE: a single per-frame bone UBO means only the LAST skinned draw's
        // bone pose is visible if multiple skinned meshes exist in one frame.
        // Use a dynamic-offset UBO (4d.3+) to handle multiple poses correctly.
        const VkPipeline skinnedPipe = hdr && m_skinnedPipelineHDR
            ? m_skinnedPipelineHDR : m_skinnedPipeline;
        if (skinnedPipe && !cmds.skinnedDrawCalls().empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skinnedPipe);
            // set=0 (scene.frag per-frame data) is already bound from DrawScene preamble.
            // Re-bind it here because the sky / transparent passes may have changed state.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 0, nullptr);

            constexpr int          kMaxBones     = 128;
            constexpr VkDeviceSize kBoneSlotSize = 128 * sizeof(glm::mat4);
            constexpr uint32_t     kMaxSkinnedVk = 256;
            static const glm::mat4 kIdentity(1.0f);
            uint32_t skinnedIdx = 0;

            for (const SkinnedDrawCall& dc : cmds.skinnedDrawCalls())
            {
                if (skinnedIdx >= kMaxSkinnedVk) break;
                const GpuSkeletalMesh* smesh = resolveSkeletalMesh(dc.meshAssetId);
                if (!smesh || !smesh->indexCount) continue;

                // Upload bone matrices into this draw's slot of the per-frame ring buffer.
                if (m_boneUBOPtr[m_currentFrame])
                {
                    auto* dst = reinterpret_cast<glm::mat4*>(
                        static_cast<uint8_t*>(m_boneUBOPtr[m_currentFrame])
                        + skinnedIdx * kBoneSlotSize);
                    const int boneCount = static_cast<int>(
                        std::min(dc.boneMatrices.size(), static_cast<size_t>(kMaxBones)));
                    for (int b = 0; b < boneCount; ++b)
                        dst[b] = dc.boneMatrices[b];
                    for (int b = boneCount; b < kMaxBones; ++b)
                        dst[b] = kIdentity;
                }

                // Bind bones descriptor set at set=1 with a dynamic offset into the ring buffer.
                uint32_t dynOffset = static_cast<uint32_t>(skinnedIdx * kBoneSlotSize);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeLayout,
                                        1, 1, &m_boneDescSet[m_currentFrame], 1, &dynOffset);

                // Base color: MaterialComponent override wins over the baked texture (see drawDCVk).
                VkDescriptorSet albedoSet = smesh->albedoSet;
                bool textured = (smesh->albedoSet != VK_NULL_HANDLE);
                const MaterialTexVk* ovr = nullptr;
                if (resolveMaterialOverride(dc.materialAssetId, ovr))
                {
                    albedoSet = ovr->set;
                    textured  = (ovr->set != VK_NULL_HANDLE);
                }
                if (!albedoSet) albedoSet = m_whiteAlbedoSet;

                // Update material UBO (same as drawDCVk).
                if (m_matUBO)
                {
                    struct MatData { float r,g,b,met; float rough,opacity,hasTex,pad; } md{
                        dc.baseColor.r, dc.baseColor.g, dc.baseColor.b, dc.metallic,
                        dc.roughness, dc.opacity, textured ? 1.0f : 0.0f, 0.0f
                    };
                    vkCmdUpdateBuffer(cmd, m_matUBO, 0, sizeof(md), &md);
                    VkBufferMemoryBarrier bar{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                    bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                    bar.dstAccessMask       = VK_ACCESS_UNIFORM_READ_BIT;
                    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bar.buffer = m_matUBO; bar.offset = 0; bar.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 1, &bar, 0, nullptr);
                }

                // Bind the effective base-color texture at set 2 (albedoSet resolved above).
                // Skip the draw if no valid set-2 descriptor exists (see drawDCVk).
                if (!albedoSet) continue;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeLayout,
                                        2, 1, &albedoSet, 0, nullptr);

                // Bind the three vertex buffer slots and index buffer.
                const VkBuffer    vbs[3]     = { smesh->vb, smesh->boneIdVb, smesh->boneWgtVb };
                const VkDeviceSize offsets[3] = { 0, 0, 0 };
                vkCmdBindVertexBuffers(cmd, 0, 3, vbs, offsets);
                vkCmdBindIndexBuffer(cmd, smesh->ib, 0, VK_INDEX_TYPE_UINT32);

                // Push constants: MVP + model (same as drawDCVk).
                PushConstants pc2{ viewProj * dc.transform, dc.transform };
                vkCmdPushConstants(cmd, m_skinnedPipeLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(pc2), &pc2);

                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(smesh->indexCount), 1, 0, 0, 0);
                ++m_statDraws;
                m_statTris += static_cast<uint32_t>(smesh->indexCount) / 3;
                ++skinnedIdx;
            }

            // Restore scene pipeline and set=0 binding for any subsequent work.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 0, nullptr);
        }

        // Debug lines on top of opaque+transparent geometry, before post-process.
        drawDebugLines(cmd, viewProj, hdr);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Sky pipeline
// ─────────────────────────────────────────────────────────────────────────────

void VulkanRenderer::createSkyPipeline()
{
    // ── Descriptor set layout: binding 0 = SkyEnv UBO, binding 1 = moon sampler,
    //    binding 2 = 3D noise volume (volumetric clouds + nebula) ──
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 3;
    dslci.pBindings    = bindings;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_skyDSLayout),
            "sky descriptor set layout");

    // ── Pipeline layout (no push constants needed) ──────────────────────────
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &m_skyDSLayout;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_skyPipelineLayout),
            "sky pipeline layout");

    // ── Descriptor pool (k_maxFramesInFlight UBOs + 2 samplers/set: moon + noise) ─
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = k_maxFramesInFlight;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = k_maxFramesInFlight * 2;   // moon (binding 1) + noise (binding 2)
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = poolSizes;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_skyDSPool), "sky descriptor pool");

    // ── 3D noise volume (RG16: R=value hash, G=Worley) for volumetric clouds ─
    // Baked CPU-side by the shared HE::BuildSkyNoise3D (the bytes are a
    // cross-backend contract). Created + uploaded BEFORE the per-frame
    // descriptor loop so binding 2 references a live view.
    {
#ifdef NDEBUG
        const int kNoiseN = 256;
#else
        const int kNoiseN = 64;
#endif
        std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kNoiseN);
        const VkDeviceSize dataSize =
            static_cast<VkDeviceSize>(kNoiseN) * kNoiseN * kNoiseN * 4; // RG16 = 4 bytes/texel

        // Staging buffer (tightly packed — vkCmdCopyBufferToImage handles the layout).
        VkBuffer       stageBuf = VK_NULL_HANDLE;
        VkDeviceMemory stageMem = VK_NULL_HANDLE;
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = dataSize;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCreateBuffer(m_device, &bci, nullptr, &stageBuf);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(m_device, stageBuf, &mr);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_device, &mai, nullptr, &stageMem);
            vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
            void* ptr = nullptr;
            vkMapMemory(m_device, stageMem, 0, dataSize, 0, &ptr);
            std::memcpy(ptr, noise.data(), static_cast<size_t>(dataSize));
            vkUnmapMemory(m_device, stageMem);
        }

        // Device-local 3D R16G16_UNORM image.
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_3D;
        ici.format        = VK_FORMAT_R16G16_UNORM;
        ici.extent        = { static_cast<uint32_t>(kNoiseN),
                              static_cast<uint32_t>(kNoiseN),
                              static_cast<uint32_t>(kNoiseN) };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_skyNoiseImage), "sky noise image");
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(m_device, m_skyNoiseImage, &imr);
        VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imal.allocationSize  = imr.size;
        imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &imal, nullptr, &m_skyNoiseMemory), "sky noise memory");
        vkBindImageMemory(m_device, m_skyNoiseImage, m_skyNoiseMemory, 0);

        // One-shot command buffer: UNDEFINED→TRANSFER_DST, copy, TRANSFER_DST→SHADER_READ.
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool        = m_cmdPool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer oneCB = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
        VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(oneCB, &oneBI);
        {
            VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image               = m_skyNoiseImage;
            bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            bar.srcAccessMask       = 0;
            bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(oneCB,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        VkBufferImageCopy region{};
        region.bufferRowLength   = 0;   // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent       = { static_cast<uint32_t>(kNoiseN),
                                     static_cast<uint32_t>(kNoiseN),
                                     static_cast<uint32_t>(kNoiseN) };
        vkCmdCopyBufferToImage(oneCB, stageBuf, m_skyNoiseImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        {
            VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image               = m_skyNoiseImage;
            bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(oneCB,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        vkEndCommandBuffer(oneCB);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &oneCB;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);
        vkDestroyBuffer(m_device, stageBuf, nullptr);
        vkFreeMemory(m_device, stageMem, nullptr);

        // 3D image view.
        VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image            = m_skyNoiseImage;
        ivci.viewType         = VK_IMAGE_VIEW_TYPE_3D;
        ivci.format           = VK_FORMAT_R16G16_UNORM;
        ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &ivci, nullptr, &m_skyNoiseView), "sky noise image view");

        // Linear, REPEAT-on-all-axes sampler (the bake tiles seamlessly).
        VkSamplerCreateInfo nsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        nsci.magFilter    = VK_FILTER_LINEAR;
        nsci.minFilter    = VK_FILTER_LINEAR;
        nsci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        nsci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        nsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCheck(vkCreateSampler(m_device, &nsci, nullptr, &m_skyNoiseSampler), "sky noise sampler");
    }

    // ── Per-frame UBO buffers, descriptor sets ───────────────────────────────
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = sizeof(SkyUBOData);
        bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &m_skyUBO[i].buf), "sky UBO buffer");

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, m_skyUBO[i].buf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_skyUBO[i].mem), "sky UBO memory");
        vkBindBufferMemory(m_device, m_skyUBO[i].buf, m_skyUBO[i].mem, 0);
        vkMapMemory(m_device, m_skyUBO[i].mem, 0, sizeof(SkyUBOData), 0, &m_skyUBO[i].mapped);

        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_skyDSPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_skyDSLayout;
        vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &m_skyUBO[i].set), "sky descriptor set");

        VkDescriptorBufferInfo dbi{ m_skyUBO[i].buf, 0, sizeof(SkyUBOData) };
        VkWriteDescriptorSet wr[3]{};
        wr[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet          = m_skyUBO[i].set;
        wr[0].dstBinding      = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[0].pBufferInfo     = &dbi;
        // binding 1 (moon sampler) will be updated by SetMoonTexture; for now
        // point at the engine's dummy image so the set is fully valid.
        VkDescriptorImageInfo dii{};
        dii.sampler     = m_postFxSampler;   // reuse the linear sampler from PostFX
        dii.imageView   = m_dummyView;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[1].dstSet          = m_skyUBO[i].set;
        wr[1].dstBinding      = 1;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[1].pImageInfo      = &dii;
        // binding 2 = baked 3D noise volume (volumetric clouds + nebula).
        VkDescriptorImageInfo niiNoise{};
        niiNoise.sampler     = m_skyNoiseSampler;
        niiNoise.imageView   = m_skyNoiseView;
        niiNoise.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[2].dstSet          = m_skyUBO[i].set;
        wr[2].dstBinding      = 2;
        wr[2].descriptorCount = 1;
        wr[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[2].pImageInfo      = &niiNoise;
        vkUpdateDescriptorSets(m_device, 3, wr, 0, nullptr);
    }

    // ── Dummy 1×1 white moon texture (used until SetMoonTexture is called) ───
    // The real default is the dummy image already created by PostFX; nothing to do.

    // ── Create moon sampler ─────────────────────────────────────────────────
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_moonSampler), "moon sampler");

    // ── Shader modules ───────────────────────────────────────────────────────
    VkShaderModule vs = loadShaderModule("sky.vert.spv");
    VkShaderModule fs = loadShaderModule("sky.frag.spv");
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: sky shaders not found — sky disabled");
        return;
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

    // No vertex input — sky.vert generates a full-screen triangle from gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test off, depth write off — sky is the background.
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    auto makeSkyPipeline = [&](VkRenderPass rp, VkPipeline& out) {
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_skyPipelineLayout;
        pci.renderPass          = rp;
        pci.subpass             = 0;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out) != VK_SUCCESS)
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: sky pipeline creation failed");
    };

    makeSkyPipeline(m_renderPass, m_skyPipeline);
    if (m_postFxSceneRP)
        makeSkyPipeline(m_postFxSceneRP, m_skyPipelineHDR);

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
}

void VulkanRenderer::destroySkyPipeline()
{
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_skyUBO[i].mapped) { vkUnmapMemory(m_device, m_skyUBO[i].mem); m_skyUBO[i].mapped = nullptr; }
        if (m_skyUBO[i].buf)    { vkDestroyBuffer(m_device, m_skyUBO[i].buf, nullptr); m_skyUBO[i].buf = VK_NULL_HANDLE; }
        if (m_skyUBO[i].mem)    { vkFreeMemory(m_device, m_skyUBO[i].mem, nullptr); m_skyUBO[i].mem = VK_NULL_HANDLE; }
    }
    if (m_skyDSPool)        { vkDestroyDescriptorPool(m_device, m_skyDSPool, nullptr); m_skyDSPool = VK_NULL_HANDLE; }
    if (m_skyDSLayout)      { vkDestroyDescriptorSetLayout(m_device, m_skyDSLayout, nullptr); m_skyDSLayout = VK_NULL_HANDLE; }
    if (m_skyPipelineHDR)   { vkDestroyPipeline(m_device, m_skyPipelineHDR, nullptr); m_skyPipelineHDR = VK_NULL_HANDLE; }
    if (m_skyPipeline)      { vkDestroyPipeline(m_device, m_skyPipeline, nullptr); m_skyPipeline = VK_NULL_HANDLE; }
    if (m_skyPipelineLayout){ vkDestroyPipelineLayout(m_device, m_skyPipelineLayout, nullptr); m_skyPipelineLayout = VK_NULL_HANDLE; }
    // Moon texture
    if (m_moonSampler)      { vkDestroySampler(m_device, m_moonSampler, nullptr); m_moonSampler = VK_NULL_HANDLE; }
    if (m_moonView)         { vkDestroyImageView(m_device, m_moonView, nullptr); m_moonView = VK_NULL_HANDLE; }
    if (m_moonImage)        { vkDestroyImage(m_device, m_moonImage, nullptr); m_moonImage = VK_NULL_HANDLE; }
    if (m_moonMemory)       { vkFreeMemory(m_device, m_moonMemory, nullptr); m_moonMemory = VK_NULL_HANDLE; }
    // Sky 3D noise volume
    if (m_skyNoiseSampler)  { vkDestroySampler(m_device, m_skyNoiseSampler, nullptr); m_skyNoiseSampler = VK_NULL_HANDLE; }
    if (m_skyNoiseView)     { vkDestroyImageView(m_device, m_skyNoiseView, nullptr); m_skyNoiseView = VK_NULL_HANDLE; }
    if (m_skyNoiseImage)    { vkDestroyImage(m_device, m_skyNoiseImage, nullptr); m_skyNoiseImage = VK_NULL_HANDLE; }
    if (m_skyNoiseMemory)   { vkFreeMemory(m_device, m_skyNoiseMemory, nullptr); m_skyNoiseMemory = VK_NULL_HANDLE; }
}

void VulkanRenderer::drawSky(VkCommandBuffer cmd, uint32_t /*width*/, uint32_t /*height*/, bool hdr)
{
    VkPipeline pipe = hdr && m_skyPipelineHDR ? m_skyPipelineHDR : m_skyPipeline;
    if (!pipe || !m_skyPipelineLayout) return;
    if (!m_environment.skyEnabled) return; // no Sky entity → leave the cleared background

    const glm::mat4 vp = HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;

    // The one shared EnvironmentSettings → sky-constants translation. sky.frag's
    // UBO is a REDUCED copy of HE::SkyFrameParams (see the header block in
    // shaders/sky.frag), so the fields are read out by name instead of memcpy'd —
    // a blanket copy would misalign every offset past invViewProj.
    HE::SkyFrameInputs in;
    in.invViewProj    = glm::inverse(vp);
    in.sunDir         = glm::normalize(m_renderWorld.sunDirection);
    in.cameraPos      = m_renderWorld.camera.position;
    in.time           = m_wallTime;
    in.hasMoonTexture = (m_moonImage != VK_NULL_HANDLE);
    const HE::SkyFrameParams p = HE::BuildSkyFrameParams(m_environment, in);

    SkyUBOData sky{};
    sky.invViewProj   = p.invViewProj;
    sky.sunDir        = glm::vec3(p.sunDir);
    sky.timeOfDay     = p.params.x;
    sky.sunColor      = glm::vec3(p.sunColor);
    sky.cloudCoverage = p.params.y;
    // Cloud drift: world-units/sec, now HE::CloudWindVector. This backend used to
    // drop the negation on Z (vec3(sin, 0, cos)), drifting the clouds 180° away
    // from GL/Metal; the shared vector is the GL/Metal form.
    sky.wind          = glm::vec3(p.wind);
    sky.time          = p.params.z;
    sky.auroraColor   = glm::vec3(p.auroraColor);
    sky.aurora        = p.params.w;
    sky.milkyWay      = p.auroraColor.w;
    sky.flash         = p.wind.w;
    sky.hasMoonTex    = (p.sunDir.w != 0.0f) ? 1 : 0;
    sky.nebula        = p.nebulaColor.w;
    sky.nebulaColor   = glm::vec3(p.nebulaColor);
    // sky._pad2 is left zeroed by the SkyUBOData{} value-initialisation above.

    if (m_skyUBO[m_currentFrame].mapped)
        std::memcpy(m_skyUBO[m_currentFrame].mapped, &sky, sizeof(sky));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipelineLayout,
                            0, 1, &m_skyUBO[m_currentFrame].set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // full-screen triangle, no vertex buffer
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug line pipeline
// ─────────────────────────────────────────────────────────────────────────────

void VulkanRenderer::createDebugLinePipeline()
{
    // ── Descriptor set layout: binding 0 = DebugCB UBO (mat4 uVP) ───────────
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings    = &binding;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_debugDSLayout),
            "debug line descriptor set layout");

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &m_debugDSLayout;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_debugPipelineLayout),
            "debug line pipeline layout");

    // ── Descriptor pool ──────────────────────────────────────────────────────
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, k_maxFramesInFlight };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_debugDSPool),
            "debug line descriptor pool");

    // ── Per-frame UBO + host-visible vertex buffers ──────────────────────────
    constexpr VkDeviceSize kMaxDebugVerts = 65536;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        // UBO
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = sizeof(DebugUBOData);
        bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &m_debugUBO[i].buf), "debug UBO buffer");

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, m_debugUBO[i].buf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_debugUBO[i].mem), "debug UBO memory");
        vkBindBufferMemory(m_device, m_debugUBO[i].buf, m_debugUBO[i].mem, 0);
        vkMapMemory(m_device, m_debugUBO[i].mem, 0, sizeof(DebugUBOData), 0, &m_debugUBO[i].mapped);

        // Vertex buffer (interleaved vec3 pos + vec3 color = 24 bytes per vertex)
        VkBufferCreateInfo vbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        vbci.size        = kMaxDebugVerts * sizeof(float) * 6;
        vbci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        vbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(m_device, &vbci, nullptr, &m_debugVB[i]), "debug vertex buffer");
        vkGetBufferMemoryRequirements(m_device, m_debugVB[i], &mr);
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_debugVBMem[i]), "debug VB memory");
        vkBindBufferMemory(m_device, m_debugVB[i], m_debugVBMem[i], 0);
        vkMapMemory(m_device, m_debugVBMem[i], 0, vbci.size, 0, &m_debugVBMapped[i]);

        // Descriptor set
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_debugDSPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_debugDSLayout;
        vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &m_debugUBO[i].set), "debug descriptor set");

        VkDescriptorBufferInfo dbi{ m_debugUBO[i].buf, 0, sizeof(DebugUBOData) };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet          = m_debugUBO[i].set;
        wr.dstBinding      = 0;
        wr.descriptorCount = 1;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
    }

    // ── Shader modules ───────────────────────────────────────────────────────
    VkShaderModule vs = loadShaderModule("debug_line.vert.spv");
    VkShaderModule fs = loadShaderModule("debug_line.frag.spv");
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: debug line shaders not found — debug lines disabled");
        return;
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

    // location=0: vec3 aPos, location=1: vec3 aColor (interleaved, stride=24)
    VkVertexInputBindingDescription vbd{};
    vbd.binding   = 0;
    vbd.stride    = sizeof(float) * 6;
    vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = sizeof(float) * 3;

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbd;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test on so lines correctly occlude behind geometry, but no depth write.
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    auto makeDebugPipeline = [&](VkRenderPass rp, VkPipeline& out) {
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_debugPipelineLayout;
        pci.renderPass          = rp;
        pci.subpass             = 0;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out) != VK_SUCCESS)
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: debug line pipeline creation failed");
    };

    makeDebugPipeline(m_renderPass, m_debugPipeline);
    if (m_postFxSceneRP)
        makeDebugPipeline(m_postFxSceneRP, m_debugPipelineHDR);

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
}

void VulkanRenderer::destroyDebugLinePipeline()
{
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_debugVBMapped[i])  { vkUnmapMemory(m_device, m_debugVBMem[i]); m_debugVBMapped[i] = nullptr; }
        if (m_debugVB[i])        { vkDestroyBuffer(m_device, m_debugVB[i], nullptr); m_debugVB[i] = VK_NULL_HANDLE; }
        if (m_debugVBMem[i])     { vkFreeMemory(m_device, m_debugVBMem[i], nullptr); m_debugVBMem[i] = VK_NULL_HANDLE; }
        if (m_debugUBO[i].mapped){ vkUnmapMemory(m_device, m_debugUBO[i].mem); m_debugUBO[i].mapped = nullptr; }
        if (m_debugUBO[i].buf)   { vkDestroyBuffer(m_device, m_debugUBO[i].buf, nullptr); m_debugUBO[i].buf = VK_NULL_HANDLE; }
        if (m_debugUBO[i].mem)   { vkFreeMemory(m_device, m_debugUBO[i].mem, nullptr); m_debugUBO[i].mem = VK_NULL_HANDLE; }
    }
    if (m_debugDSPool)        { vkDestroyDescriptorPool(m_device, m_debugDSPool, nullptr); m_debugDSPool = VK_NULL_HANDLE; }
    if (m_debugDSLayout)      { vkDestroyDescriptorSetLayout(m_device, m_debugDSLayout, nullptr); m_debugDSLayout = VK_NULL_HANDLE; }
    if (m_debugPipelineHDR)   { vkDestroyPipeline(m_device, m_debugPipelineHDR, nullptr); m_debugPipelineHDR = VK_NULL_HANDLE; }
    if (m_debugPipeline)      { vkDestroyPipeline(m_device, m_debugPipeline, nullptr); m_debugPipeline = VK_NULL_HANDLE; }
    if (m_debugPipelineLayout){ vkDestroyPipelineLayout(m_device, m_debugPipelineLayout, nullptr); m_debugPipelineLayout = VK_NULL_HANDLE; }
}

void VulkanRenderer::drawDebugLines(VkCommandBuffer cmd, const glm::mat4& viewProj, bool hdr)
{
    if (!m_debugPipeline || !m_debugPipelineLayout) return;
    if (m_debugLines.empty()) return;

    const uint32_t fi = m_currentFrame;

    // Pack interleaved float data: [pos.x, pos.y, pos.z, col.r, col.g, col.b] per endpoint
    constexpr VkDeviceSize kMaxDebugVerts = 65536;
    const uint32_t vertCount = std::min<uint32_t>(
        static_cast<uint32_t>(m_debugLines.size() * 2), kMaxDebugVerts);

    if (m_debugVBMapped[fi] && vertCount > 0)
    {
        float* dst = static_cast<float*>(m_debugVBMapped[fi]);
        for (uint32_t li = 0; li < vertCount / 2; ++li)
        {
            const DebugLine& dl = m_debugLines[li];
            *dst++ = dl.start.x; *dst++ = dl.start.y; *dst++ = dl.start.z;
            *dst++ = dl.color.r; *dst++ = dl.color.g; *dst++ = dl.color.b;
            *dst++ = dl.end.x;   *dst++ = dl.end.y;   *dst++ = dl.end.z;
            *dst++ = dl.color.r; *dst++ = dl.color.g; *dst++ = dl.color.b;
        }
    }

    if (m_debugUBO[fi].mapped)
    {
        DebugUBOData d{ viewProj };
        std::memcpy(m_debugUBO[fi].mapped, &d, sizeof(d));
    }

    VkPipeline pipe = hdr && m_debugPipelineHDR ? m_debugPipelineHDR : m_debugPipeline;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipelineLayout,
                            0, 1, &m_debugUBO[fi].set, 0, nullptr);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_debugVB[fi], &offset);
    vkCmdDraw(cmd, vertCount, 1, 0, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// SetDebugLines / SetMoonTexture
// ─────────────────────────────────────────────────────────────────────────────

void VulkanRenderer::SetDebugLines(const std::vector<DebugLine>& lines)
{
    m_debugLines = lines;
}

void VulkanRenderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
    if (!rgba8Pixels || width <= 0 || height <= 0) return;

    vkDeviceWaitIdle(m_device);

    // Destroy any previous moon texture.
    if (m_moonView)   { vkDestroyImageView(m_device, m_moonView, nullptr);  m_moonView   = VK_NULL_HANDLE; }
    if (m_moonImage)  { vkDestroyImage    (m_device, m_moonImage, nullptr); m_moonImage  = VK_NULL_HANDLE; }
    if (m_moonMemory) { vkFreeMemory      (m_device, m_moonMemory, nullptr); m_moonMemory = VK_NULL_HANDLE; }

    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width * height * 4);

    // ── Staging buffer ───────────────────────────────────────────────────────
    VkBuffer       stageBuf  = VK_NULL_HANDLE;
    VkDeviceMemory stageMem  = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = dataSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &stageBuf);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, stageBuf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &mai, nullptr, &stageMem);
        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
        void* ptr = nullptr;
        vkMapMemory(m_device, stageMem, 0, dataSize, 0, &ptr);
        std::memcpy(ptr, rgba8Pixels, static_cast<size_t>(dataSize));
        vkUnmapMemory(m_device, stageMem);
    }

    // ── Device-local RGBA8 image ─────────────────────────────────────────────
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(m_device, &ici, nullptr, &m_moonImage);
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(m_device, m_moonImage, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(m_device, &imal, nullptr, &m_moonMemory);
    vkBindImageMemory(m_device, m_moonImage, m_moonMemory, 0);

    // ── One-shot command buffer: transition + copy ───────────────────────────
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer oneCB = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
    VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(oneCB, &oneBI);

    // UNDEFINED → TRANSFER_DST
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = m_moonImage;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    vkCmdCopyBufferToImage(oneCB, stageBuf, m_moonImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = m_moonImage;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    vkEndCommandBuffer(oneCB);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &oneCB;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);

    // Destroy staging resources.
    vkDestroyBuffer(m_device, stageBuf, nullptr);
    vkFreeMemory(m_device, stageMem, nullptr);

    // ── Image view ───────────────────────────────────────────────────────────
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = m_moonImage;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(m_device, &ivci, nullptr, &m_moonView);

    // ── Update descriptor sets to point at the real moon texture ─────────────
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (!m_skyUBO[i].set) continue;
        VkDescriptorImageInfo dii{};
        dii.sampler     = m_moonSampler;
        dii.imageView   = m_moonView;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet          = m_skyUBO[i].set;
        wr.dstBinding      = 1;
        wr.descriptorCount = 1;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo      = &dii;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui editor textures (content-browser icons + logo)
// ─────────────────────────────────────────────────────────────────────────────
// Mirrors SetMoonTexture's upload path (staging buffer → device-local
// R8G8B8A8_UNORM image, UNDEFINED→TRANSFER_DST copy → SHADER_READ_ONLY, one-shot
// command buffer + vkQueueWaitIdle) but creates its OWN linear sampler (the moon
// path reuses m_moonSampler) and does NOT touch any scene descriptor sets. The
// image/view/memory/sampler are retained in m_imguiTextures so they outlive the
// VkDescriptorSet ImGui samples from; the editor's registrar (which links ImGui)
// calls ImGui_ImplVulkan_AddTexture to build that descriptor set.
void* VulkanRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
    if (!rgba8Pixels || width <= 0 || height <= 0 || !m_device) return nullptr;

    vkDeviceWaitIdle(m_device);

    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width * height * 4);

    // ── Staging buffer ───────────────────────────────────────────────────────
    VkBuffer       stageBuf  = VK_NULL_HANDLE;
    VkDeviceMemory stageMem  = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = dataSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &stageBuf);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, stageBuf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &mai, nullptr, &stageMem);
        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
        void* ptr = nullptr;
        vkMapMemory(m_device, stageMem, 0, dataSize, 0, &ptr);
        std::memcpy(ptr, rgba8Pixels, static_cast<size_t>(dataSize));
        vkUnmapMemory(m_device, stageMem);
    }

    // ── Device-local RGBA8 image ─────────────────────────────────────────────
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(m_device, &ici, nullptr, &image);
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(m_device, &imal, nullptr, &memory);
    vkBindImageMemory(m_device, image, memory, 0);

    // ── One-shot command buffer: transition + copy ───────────────────────────
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer oneCB = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
    VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(oneCB, &oneBI);

    // UNDEFINED → TRANSFER_DST
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = image;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    vkCmdCopyBufferToImage(oneCB, stageBuf, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = image;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    vkEndCommandBuffer(oneCB);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &oneCB;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);

    // Destroy staging resources.
    vkDestroyBuffer(m_device, stageBuf, nullptr);
    vkFreeMemory(m_device, stageMem, nullptr);

    // ── Image view ───────────────────────────────────────────────────────────
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = image;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(m_device, &ivci, nullptr, &view);

    // ── Linear sampler (mirrors the moon sampler creation) ───────────────────
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_device, &sci, nullptr, &sampler);

    // Retain so the resources outlive ImGui's descriptor set.
    m_imguiTextures.push_back({ image, view, memory, sampler });

    return m_imguiTexRegistrar
        ? m_imguiTexRegistrar(reinterpret_cast<void*>(view), reinterpret_cast<void*>(sampler))
        : nullptr;
}

void VulkanRenderer::DestroyImGuiTexture(void* /*handle*/)
{
    // No-op: the editor textures live until renderer shutdown, where the stored
    // images/views/memory/samplers are destroyed (after vkDeviceWaitIdle). The
    // ImGui VkDescriptorSet is owned by ImGui's internal pool and freed on ImGui
    // shutdown, so there is nothing to free here.
}

void VulkanRenderer::SetSSAOSettings(const SSAOSettings& s)
{
    m_ssaoEnabled   = s.enabled;
    m_ssaoRadius    = s.radius;
    m_ssaoIntensity = s.intensity;
    m_ssaoMethod    = s.method;
}

void VulkanRenderer::SetGISettings(const GISettings& s)
{
    m_giEnabled             = s.enabled;
    m_giIndirectIntensity   = std::max(0.0f, s.indirectIntensity);
    m_giLightRadius         = std::clamp(s.lightRadius, 0.0f, 10.0f);
    m_giRaysPerProbe        = std::clamp(s.raysPerProbe, 8, 1024);
    m_giProbeBudgetPerFrame = std::clamp(s.probeBudgetPerFrame, 1, 4096);
}

// ─────────────────────────────────────────────────────────────────────────────
// Global Illumination — software ray tracing, Checkpoint VK-A (accel only)
// ─────────────────────────────────────────────────────────────────────────────
// CPU-built HE::GiBvh (same module + unit tests as the GL 4.3 port and Metal's
// SW fallback) uploaded into host-visible SSBOs; instances are a flat
// per-in-flight-frame buffer (TLAS analogue: invTransform + baseColor + BLAS
// offsets). The gi_shadow.comp/gi_probe.comp kernels consuming these land in
// VK-B/C — until then this is inert (GI-off rendering byte-identical) and
// GetCapabilities keeps supportsGlobalIllumination = false.

VulkanRenderer::GIBlasRange VulkanRenderer::BuildGIBlas(const HE::UUID& meshId)
{
    GIBlasRange range;
    if (!m_contentManager) return range;
    const StaticMeshAsset* asset = m_contentManager->getStaticMesh(meshId);
    if (!asset || asset->indices.empty()) return range;

    // Same two layouts the mesh upload path consumes: cooked interleaved
    // 8-float (position at offset 0) or loose tightly-packed 3-float positions.
    HE::GiBvh bvh;
    if (asset->cooked && !asset->interleaved.empty())
        bvh = HE::buildGiBvh(asset->interleaved.data(), asset->vertexCount, 8,
                             asset->indices.data(), asset->indices.size());
    else if (!asset->vertices.empty())
        bvh = HE::buildGiBvh(asset->vertices.data(), asset->vertices.size() / 3, 3,
                             asset->indices.data(), asset->indices.size());
    if (!bvh.valid()) return range;

    range.nodeOffset = static_cast<int32_t>(m_giNodesCpu.size());
    range.triOffset  = static_cast<int32_t>(m_giTrisCpu.size());
    range.valid      = true;
    m_giNodesCpu.insert(m_giNodesCpu.end(), bvh.nodes.begin(), bvh.nodes.end());
    m_giTrisCpu.insert(m_giTrisCpu.end(), bvh.triangles.begin(), bvh.triangles.end());
    m_giBlasDirty = true;
    return range;
}

bool VulkanRenderer::uploadGiBuffer(GiBuffer& b, const void* data, VkDeviceSize size,
                                    VkBufferUsageFlags usage)
{
    if (size == 0) return false;
    if (b.size < size)
    {
        if (b.mapped) { vkUnmapMemory(m_device, b.mem); b.mapped = nullptr; }
        if (b.buf)    { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        if (b.mem)    { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
        b.size = 0;

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, b.buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        // Buffers the HW-RT path takes device addresses of (TLAS instance
        // input) need the DEVICE_ADDRESS allocation flag or the address query
        // is invalid.
        VkMemoryAllocateFlagsInfo mafi{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
        {
            mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            mai.pNext  = &mafi;
        }
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &b.mem) != VK_SUCCESS)
        { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; return false; }
        vkBindBufferMemory(m_device, b.buf, b.mem, 0);
        if (vkMapMemory(m_device, b.mem, 0, size, 0, &b.mapped) != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE;
            vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE;
            return false;
        }
        b.size = size;
    }
    std::memcpy(b.mapped, data, static_cast<size_t>(size));
    return true;
}

void VulkanRenderer::updateGiAccel()
{
    m_giInstanceCount = 0;
    m_giHwInstOk      = false;
    m_giHwInstancesCpu.clear();
    if (!m_giEnabled) return;

    // Same caster filter as the shadow pass / the other backends' TLAS builds:
    // castsShadow only, UNculled (rays go in arbitrary directions).
    std::vector<GIInstanceGpu> instances;
    instances.reserve(m_renderWorld.objects.size());
    auto resolveRange = [&](const HE::UUID& id) -> GIBlasRange
    {
        auto it = m_giBlasCache.find(id);
        if (it == m_giBlasCache.end())
            it = m_giBlasCache.emplace(id, BuildGIBlas(id)).first;
        return it->second;
    };
    // HW path: build a VkAccelerationStructureInstanceKHR array in EXACTLY the
    // giInsts order (the kernels index giInsts by InstanceId). Any mesh whose
    // HW BLAS can't be built breaks that 1:1 mapping → SW kernels this frame.
    bool hwAll = m_giHwRt && m_giHwPipesReady;
    auto resolveHwBlas = [&](const HE::UUID& id) -> const GiHwBlas&
    {
        auto it = m_giHwBlasCache.find(id);
        if (it == m_giHwBlasCache.end())
            it = m_giHwBlasCache.emplace(id, buildGiHwBlas(id)).first;
        return it->second;
    };
    for (const RenderObject& obj : m_renderWorld.objects)
    {
        if (!obj.castsShadow) continue;
        // Default-cube fallback — an entity without a resolvable mesh asset
        // RENDERS as the default cube (draw-loop fallback), so it must occlude
        // as one too, or plain cube entities cast no GI shadow at all.
        HE::UUID    effectiveId = obj.meshAssetId;
        GIBlasRange range       = resolveRange(effectiveId);
        if (!range.valid) { effectiveId = HE::kDefaultCubeMeshId; range = resolveRange(effectiveId); }
        if (!range.valid) continue;
        GIInstanceGpu inst;
        inst.invTransform = glm::inverse(obj.transform);
        inst.baseColor    = glm::vec4(obj.baseColor, 1.0f);
        inst.nodeOffset   = range.nodeOffset;
        inst.triOffset    = range.triOffset;
        instances.push_back(inst);

        if (hwAll)
        {
            const GiHwBlas& hb = resolveHwBlas(effectiveId);
            if (!hb.valid)
            {
                hwAll = false; // incomplete TLAS would misalign InstanceId ↔ giInsts
                m_giHwInstancesCpu.clear();
            }
            else
            {
                VkAccelerationStructureInstanceKHR vi{};
                // VkTransformMatrixKHR is 3x4 ROW-major; glm is column-major.
                const glm::mat4& m = obj.transform;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        vi.transform.matrix[r][c] = m[c][r];
                vi.instanceCustomIndex = static_cast<uint32_t>(instances.size() - 1);
                vi.mask                = 0xFF;
                vi.instanceShaderBindingTableRecordOffset = 0;
                // Opaque (SW kernels have no any-hit shading) + no facing cull
                // (SW Möller-Trumbore is two-sided).
                vi.flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR |
                           VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
                vi.accelerationStructureReference = hb.address;
                m_giHwInstancesCpu.push_back(vi);
            }
        }
    }
    if (instances.empty()) return;

    if (m_giBlasDirty && !m_giNodesCpu.empty())
    {
        // Node/tri concatenation only ever grows (new mesh joined). The buffers
        // may still be read by an in-flight frame — but the appended layout
        // keeps all existing offsets valid, and VK-A has no readers yet anyway;
        // VK-B revisits this with a proper retire if growth-in-use shows up.
        const bool nodesOk = uploadGiBuffer(m_giNodeBuf, m_giNodesCpu.data(),
                                            m_giNodesCpu.size() * sizeof(HE::GiBvhNode));
        const bool trisOk  = uploadGiBuffer(m_giTriBuf, m_giTrisCpu.data(),
                                            m_giTrisCpu.size() * sizeof(HE::GiBvhTriangle));
        if (!nodesOk || !trisOk) return;
        m_giBlasDirty = false;
    }
    // Instances change every frame while earlier frames are in flight — ring
    // slot per in-flight frame, same convention as the bones UBO ring.
    static_assert(k_maxFramesInFlight <= sizeof(m_giInstanceBuf) / sizeof(m_giInstanceBuf[0]),
                  "GI instance ring smaller than frames in flight");
    if (!uploadGiBuffer(m_giInstanceBuf[m_currentFrame], instances.data(),
                        instances.size() * sizeof(GIInstanceGpu)))
        return;
    m_giInstanceCount = static_cast<int>(instances.size());

    // HW path: upload this frame's TLAS instance array (host-visible ring slot,
    // device-address usage for the AS build input). Failure just means SW
    // kernels this frame — the SW buffers above are already in place.
    if (hwAll && !m_giHwInstancesCpu.empty())
    {
        static_assert(k_maxFramesInFlight <= sizeof(m_giTlasInstBuf) / sizeof(m_giTlasInstBuf[0]),
                      "GI TLAS instance ring smaller than frames in flight");
        m_giHwInstOk = uploadGiBuffer(
            m_giTlasInstBuf[m_currentFrame], m_giHwInstancesCpu.data(),
            m_giHwInstancesCpu.size() * sizeof(VkAccelerationStructureInstanceKHR),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    }
}

void VulkanRenderer::destroyGiAccel()
{
    auto destroy = [&](GiBuffer& b)
    {
        if (b.mapped) { vkUnmapMemory(m_device, b.mem); b.mapped = nullptr; }
        if (b.buf)    { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        if (b.mem)    { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
        b.size = 0;
    };
    destroy(m_giNodeBuf);
    destroy(m_giTriBuf);
    for (GiBuffer& b : m_giInstanceBuf) destroy(b);
    m_giBlasCache.clear();
    m_giNodesCpu.clear();
    m_giTrisCpu.clear();
    m_giInstanceCount = 0;
    m_giBlasDirty     = false;

    // HW-RT acceleration structures (BLAS cache + per-frame TLAS ring +
    // instance upload ring). Callers guarantee the GPU is idle (Shutdown waits).
    for (auto& [id, blas] : m_giHwBlasCache) destroyGiHwBlas(blas);
    m_giHwBlasCache.clear();
    for (GiHwTlas& t : m_giTlas)
    {
        if (t.as && m_pfnDestroyAS) { m_pfnDestroyAS(m_device, t.as, nullptr); t.as = VK_NULL_HANDLE; }
        destroyGiHwBuffer(t.buffer);
        destroyGiHwBuffer(t.scratch);
    }
    for (GiBuffer& b : m_giTlasInstBuf) destroy(b);
    m_giHwInstancesCpu.clear();
    m_giHwInstOk = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// GI hardware ray tracing (VK_KHR_ray_query) — acceleration-structure plumbing
// ─────────────────────────────────────────────────────────────────────────────

bool VulkanRenderer::createGiHwBuffer(GiHwBuffer& b, VkDeviceSize size,
                                      VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
    destroyGiHwBuffer(b);
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(m_device, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, b.buf, &req);
    VkMemoryAllocateFlagsInfo mafi{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext          = &mafi;
    mai.allocationSize = req.size;
    try { mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props); }
    catch (...) { destroyGiHwBuffer(b); return false; } // findMemoryType throws on miss
    if (vkAllocateMemory(m_device, &mai, nullptr, &b.mem) != VK_SUCCESS)
    { destroyGiHwBuffer(b); return false; }
    vkBindBufferMemory(m_device, b.buf, b.mem, 0);
    VkBufferDeviceAddressInfo bdai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    bdai.buffer = b.buf;
    b.addr = m_pfnGetBufferAddress ? m_pfnGetBufferAddress(m_device, &bdai) : 0;
    if (!b.addr) { destroyGiHwBuffer(b); return false; }
    b.size = size;
    return true;
}

void VulkanRenderer::destroyGiHwBuffer(GiHwBuffer& b)
{
    if (b.buf) { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem) { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
    b.size = 0;
    b.addr = 0;
}

void VulkanRenderer::destroyGiHwBlas(GiHwBlas& b)
{
    if (b.as && m_pfnDestroyAS) { m_pfnDestroyAS(m_device, b.as, nullptr); b.as = VK_NULL_HANDLE; }
    destroyGiHwBuffer(b.buffer);
    b.address = 0;
    b.valid   = false;
}

// One-shot BLAS build for a mesh: upload positions + indices into a device-
// address input buffer, query build sizes, build on a one-time command buffer
// (QueueWaitIdle — BLAS builds are rare: once per distinct mesh), then free
// input + scratch (a built BLAS references neither). Geometry source is the
// same data BuildGIBlas consumes: cooked interleaved 8-float (position at
// offset 0 → stride 32 B feeds vertexStride directly, no repack) or loose
// tight 3-float positions.
VulkanRenderer::GiHwBlas VulkanRenderer::buildGiHwBlas(const HE::UUID& meshId)
{
    GiHwBlas out;
    if (!m_giHwRt || !m_contentManager) return out;
    const StaticMeshAsset* asset = m_contentManager->getStaticMesh(meshId);
    if (!asset || asset->indices.empty()) return out;

    const float* vdata   = nullptr;
    uint32_t     vcount  = 0;
    VkDeviceSize vstride = 0;
    if (asset->cooked && !asset->interleaved.empty())
    {
        vdata   = asset->interleaved.data();
        vcount  = static_cast<uint32_t>(asset->vertexCount);
        vstride = 8 * sizeof(float);
    }
    else if (!asset->vertices.empty())
    {
        vdata   = asset->vertices.data();
        vcount  = static_cast<uint32_t>(asset->vertices.size() / 3);
        vstride = 3 * sizeof(float);
    }
    if (!vdata || vcount == 0) return out;
    const uint32_t primCount = static_cast<uint32_t>(asset->indices.size() / 3);
    if (primCount == 0) return out;

    // Build-input buffer: vertices then (4-byte aligned) indices, host-visible.
    const VkDeviceSize vBytes = vstride * vcount;
    const VkDeviceSize iOfs   = (vBytes + 3) & ~VkDeviceSize(3);
    const VkDeviceSize iBytes = asset->indices.size() * sizeof(uint32_t);
    GiHwBuffer geo;
    if (!createGiHwBuffer(geo, iOfs + iBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: HW BLAS input buffer failed");
        return out;
    }
    void* mapped = nullptr;
    if (vkMapMemory(m_device, geo.mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    { destroyGiHwBuffer(geo); return out; }
    std::memcpy(static_cast<uint8_t*>(mapped), vdata, static_cast<size_t>(vBytes));
    std::memcpy(static_cast<uint8_t*>(mapped) + iOfs, asset->indices.data(),
                static_cast<size_t>(iBytes));
    vkUnmapMemory(m_device, geo.mem);

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& tri = geom.geometry.triangles;
    tri.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
    tri.vertexData.deviceAddress = geo.addr;
    tri.vertexStride             = vstride;
    tri.maxVertex                = vcount - 1;
    tri.indexType                = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress  = geo.addr + iOfs;

    VkAccelerationStructureBuildGeometryInfoKHR bgi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries   = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    m_pfnGetASBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                         &bgi, &primCount, &sizes);

    GiHwBuffer scratch;
    bool ok = createGiHwBuffer(out.buffer, sizes.accelerationStructureSize,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
           && createGiHwBuffer(scratch, sizes.buildScratchSize + m_giAsScratchAlign,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ok)
    {
        VkAccelerationStructureCreateInfoKHR aci{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        aci.buffer = out.buffer.buf;
        aci.offset = 0;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        ok = m_pfnCreateAS(m_device, &aci, nullptr, &out.as) == VK_SUCCESS;
    }
    if (ok)
    {
        bgi.dstAccelerationStructure  = out.as;
        bgi.scratchData.deviceAddress =
            (scratch.addr + m_giAsScratchAlign - 1) & ~(VkDeviceAddress(m_giAsScratchAlign) - 1);
        VkAccelerationStructureBuildRangeInfoKHR range{ primCount, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        ok = vkAllocateCommandBuffers(m_device, &cbai, &tmp) == VK_SUCCESS;
        if (ok)
        {
            VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmp, &cbi);
            m_pfnCmdBuildAS(tmp, 1, &bgi, &pRange);
            vkEndCommandBuffer(tmp);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
            ok = vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
            vkQueueWaitIdle(m_graphicsQueue);
            vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
        }
    }
    destroyGiHwBuffer(scratch);
    destroyGiHwBuffer(geo);
    if (!ok)
    {
        HE_LOG_WARN(RHI, "%s",
                    "VulkanRenderer: HW BLAS build failed — SW GI kernels for affected frames");
        destroyGiHwBlas(out);
        return out;
    }
    VkAccelerationStructureDeviceAddressInfoKHR adai{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    adai.accelerationStructure = out.as;
    out.address = m_pfnGetASAddress(m_device, &adai);
    out.valid   = out.address != 0;
    if (!out.valid) destroyGiHwBlas(out);
    return out;
}

// Records this frame's TLAS build (ring slot per in-flight frame — the slot's
// fence was waited on, so destroy/rebuild of a too-small slot is safe) plus
// the build → compute-read barrier. Returns true iff the HW kernels can be
// dispatched against m_giTlas[m_currentFrame] this frame.
bool VulkanRenderer::buildGiTlas(VkCommandBuffer cmd)
{
    if (!m_giHwRt || !m_giHwPipesReady || !m_giHwInstOk || m_giHwInstancesCpu.empty())
        return false;
    const uint32_t fi    = m_currentFrame;
    const uint32_t count = static_cast<uint32_t>(m_giHwInstancesCpu.size());
    GiHwTlas& t = m_giTlas[fi];

    VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    VkBufferDeviceAddressInfo bdai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    bdai.buffer = m_giTlasInstBuf[fi].buf;
    geom.geometry.instances.data.deviceAddress = m_pfnGetBufferAddress(m_device, &bdai);
    if (!geom.geometry.instances.data.deviceAddress) return false;

    VkAccelerationStructureBuildGeometryInfoKHR bgi{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bgi.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bgi.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries   = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    m_pfnGetASBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                         &bgi, &count, &sizes);

    // (Re)create the slot's AS + scratch when the required sizes outgrow it.
    if (!t.as || t.buffer.size < sizes.accelerationStructureSize ||
        t.scratch.size < sizes.buildScratchSize + m_giAsScratchAlign)
    {
        if (t.as) { m_pfnDestroyAS(m_device, t.as, nullptr); t.as = VK_NULL_HANDLE; }
        if (!createGiHwBuffer(t.buffer, sizes.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
            !createGiHwBuffer(t.scratch, sizes.buildScratchSize + m_giAsScratchAlign,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            HE_LOG_WARN(RHI, "%s",
                        "VulkanRenderer: GI TLAS buffer creation failed — SW kernels this frame");
            destroyGiHwBuffer(t.buffer);
            destroyGiHwBuffer(t.scratch);
            return false;
        }
        VkAccelerationStructureCreateInfoKHR aci{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        aci.buffer = t.buffer.buf;
        aci.offset = 0;
        aci.size   = sizes.accelerationStructureSize;
        aci.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (m_pfnCreateAS(m_device, &aci, nullptr, &t.as) != VK_SUCCESS)
        {
            HE_LOG_WARN(RHI, "%s",
                        "VulkanRenderer: GI TLAS creation failed — SW kernels this frame");
            destroyGiHwBuffer(t.buffer);
            destroyGiHwBuffer(t.scratch);
            t.as = VK_NULL_HANDLE;
            return false;
        }
    }

    bgi.dstAccelerationStructure  = t.as;
    bgi.scratchData.deviceAddress =
        (t.scratch.addr + m_giAsScratchAlign - 1) & ~(VkDeviceAddress(m_giAsScratchAlign) - 1);
    VkAccelerationStructureBuildRangeInfoKHR range{ count, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);

    // TLAS build → ray-query reads in the two GI compute dispatches.
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    return true;
}

// UBO mirrors of the gi_*.comp / gi_temporal.frag param blocks (std140: vec4
// rows only, so the C++ structs match byte-for-byte).
namespace
{
struct GiShadowUBOData
{
    glm::vec4 sunDirRadius;     // xyz = toward light, w = angular radius (radians)
    glm::vec4 frame;            // x = jitter seed, y/z = tex size, w = instance count
    glm::vec4 localPosRange[4]; // xyz = local (point/spot) light position, w = range
    glm::vec4 localExtra;       // x = local light count
};
static_assert(sizeof(GiShadowUBOData) == 7 * 16, "must match gi_shadow.comp's GiShadowUBO");
struct GiTemporalUBOData { glm::mat4 prevViewProj; glm::vec4 blend; };
struct GiProbeUBOData
{
    glm::vec4 gridOrigin, gridCounts, rayParams, sunDirRadius, sunColor, skyAmbient;
    glm::vec4 lightPosRange[8], lightColorType[8], lightDirCos[8];
};
static_assert(sizeof(GiProbeUBOData) == (6 + 24) * 16, "must match gi_probe.comp's GiProbeUBO");
} // namespace

// Builds the five GI pipelines + layouts + render passes once (first GI-active
// frame). The FIRST compute pipelines in this backend. Failure logs + leaves
// m_giReady false — GI silently off for the session, no crash (blind-port
// safety, mirrors the GL port's behaviour).
void VulkanRenderer::createGiPipelines()
{
    if (m_giPipelinesTried) return;
    m_giPipelinesTried = true;

    // NEAREST clamp sampler for G-buffer/raw/history reads (point semantics —
    // the temporal pass must not blend across texels when reprojecting).
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_NEAREST;
        sci.minFilter    = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &sci, nullptr, &m_giPointSampler) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI point sampler failed"); return; }
    }

    // ── Descriptor set layouts ────────────────────────────────────────────────
    auto makeDSL = [&](const std::vector<VkDescriptorSetLayoutBinding>& binds,
                       VkDescriptorSetLayout& out) -> bool
    {
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = static_cast<uint32_t>(binds.size());
        slci.pBindings    = binds.data();
        return vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &out) == VK_SUCCESS;
    };
    auto bindOf = [&](uint32_t b, VkDescriptorType t, VkShaderStageFlags st,
                      const VkSampler* imm = nullptr)
    {
        VkDescriptorSetLayoutBinding r{};
        r.binding = b; r.descriptorType = t; r.descriptorCount = 1;
        r.stageFlags = st; r.pImmutableSamplers = imm;
        return r;
    };

    // gi_shadow.comp: 0-2 SSBOs, 3-4 samplers (gPos/gNorm), 5 storage image,
    // 6 UBO, 8 local-mask storage image (7 reserved for the HW variant's TLAS).
    bool ok = makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT, &m_giPointSampler),
        bindOf(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT, &m_giPointSampler),
        bindOf(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_SHADER_STAGE_COMPUTE_BIT),
    }, m_giShadowDSL);
    // gi_probe.comp: 0-2 SSBOs, 3-4 storage images, 5 UBO.
    ok = ok && makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    }, m_giProbeDSL);
    // gi_temporal.frag (blur reuses the layout; unused bindings hold valid views):
    // 0-2 samplers, 3 UBO.
    ok = ok && makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, &m_giPointSampler),
        bindOf(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, &m_giPointSampler),
        bindOf(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, &m_giPointSampler),
        bindOf(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_FRAGMENT_BIT),
    }, m_giFsDSL);
    if (!ok)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI descriptor layouts failed"); return; }

    // ── Pipeline layouts ──────────────────────────────────────────────────────
    auto makePL = [&](VkDescriptorSetLayout dsl, uint32_t pcSize, VkShaderStageFlags pcStage,
                      VkPipelineLayout& out) -> bool
    {
        VkPushConstantRange pcr{ pcStage, 0, pcSize };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = dsl ? 1u : 0u;
        plci.pSetLayouts    = dsl ? &dsl : nullptr;
        plci.pushConstantRangeCount = pcSize ? 1u : 0u;
        plci.pPushConstantRanges    = pcSize ? &pcr : nullptr;
        return vkCreatePipelineLayout(m_device, &plci, nullptr, &out) == VK_SUCCESS;
    };
    ok = makePL(m_giShadowDSL, 0, 0, m_giShadowPL)
      && makePL(m_giProbeDSL,  0, 0, m_giProbePL)
      && makePL(m_giFsDSL,     0, 0, m_giFsPL)
      && makePL(VK_NULL_HANDLE, sizeof(PushConstants), VK_SHADER_STAGE_VERTEX_BIT, m_giGBufPL);
    if (!ok)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI pipeline layouts failed"); return; }

    // ── Render passes ─────────────────────────────────────────────────────────
    // G-buffer: 2x RGBA16F (CLEAR → SHADER_READ_ONLY) + depth. The end
    // dependency covers FRAGMENT **and** COMPUTE consumers — the shadow-ray
    // KERNEL reads gPos/gNorm, unlike SSAO whose consumer is a fragment pass.
    {
        VkAttachmentDescription atts[3]{};
        for (int i = 0; i < 2; ++i)
        {
            atts[i].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
            atts[i].samples        = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        atts[2].format         = m_depthFormat;
        atts[2].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[2].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[2].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[2].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[2].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[2] = {
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference depthRef{ 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 2;
        sub.pColorAttachments       = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 3; rpci.pAttachments  = atts;
        rpci.subpassCount    = 1; rpci.pSubpasses    = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_giGBufRP) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI gbuf render pass failed"); return; }
    }
    // Temporal (RGBA16F) + blur (R16F): single color, DONT_CARE → SHADER_READ_ONLY.
    auto makeFsRP = [&](VkFormat fmt, VkRenderPass& rp) -> bool
    {
        VkAttachmentDescription att{};
        att.format         = fmt;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;
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
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments  = &att;
        rpci.subpassCount    = 1; rpci.pSubpasses    = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        return vkCreateRenderPass(m_device, &rpci, nullptr, &rp) == VK_SUCCESS;
    };
    if (!makeFsRP(VK_FORMAT_R16G16B16A16_SFLOAT, m_giTemporalRP) ||
        !makeFsRP(VK_FORMAT_R16_SFLOAT,          m_giBlurRP))
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI fs render passes failed"); return; }

    // ── Shader modules ────────────────────────────────────────────────────────
    VkShaderModule gbufVS   = loadShaderModule("gi_gbuf.vert.spv");
    VkShaderModule gbufFS   = loadShaderModule("gi_gbuf.frag.spv");
    VkShaderModule fsVS     = loadShaderModule("postfx.vert.spv");
    VkShaderModule tempFS   = loadShaderModule("gi_temporal.frag.spv");
    VkShaderModule blurFS   = loadShaderModule("gi_blur.frag.spv");
    VkShaderModule shadowCS = loadShaderModule("gi_shadow.comp.spv");
    VkShaderModule probeCS  = loadShaderModule("gi_probe.comp.spv");
    auto destroyModules = [&]()
    {
        for (auto m : { gbufVS, gbufFS, fsVS, tempFS, blurFS, shadowCS, probeCS })
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!gbufVS || !gbufFS || !fsVS || !tempFS || !blurFS || !shadowCS || !probeCS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: GI shaders missing — GI disabled");
        destroyModules();
        return;
    }

    // ── Pipelines ─────────────────────────────────────────────────────────────
    VkVertexInputBindingDescription vbind{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattrs[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &vbind;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = vattrs;
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba[2]{};
    for (auto& a : cba)
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                           VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb2{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb2.attachmentCount = 2; cb2.pAttachments = cba;
    VkPipelineColorBlendStateCreateInfo cb1{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb1.attachmentCount = 1; cb1.pAttachments = cba;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    bool pipesOk = true;
    // G-buffer prepass (MRT).
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = gbufVS; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = gbufFS; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2; pci.pStages = stages;
        pci.pVertexInputState = &vi;  pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vps;    pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;  pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb2;  pci.pDynamicState = &dyn;
        pci.layout = m_giGBufPL;      pci.renderPass = m_giGBufRP;
        pipesOk = pipesOk && vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                                       &m_giGBufPipe) == VK_SUCCESS;
    }
    // Temporal + blur (attribute-less fullscreen, no depth).
    VkPipelineVertexInputStateCreateInfo fsVI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo nods{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    auto makeFsPipe = [&](VkShaderModule fs, VkRenderPass rp, VkPipeline& out) -> bool
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = fsVS; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs;   stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2; pci.pStages = stages;
        pci.pVertexInputState = &fsVI; pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vps;     pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;   pci.pDepthStencilState = &nods;
        pci.pColorBlendState = &cb1;   pci.pDynamicState = &dyn;
        pci.layout = m_giFsPL;         pci.renderPass = rp;
        return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out) == VK_SUCCESS;
    };
    pipesOk = pipesOk && makeFsPipe(tempFS, m_giTemporalRP, m_giTemporalPipe);
    pipesOk = pipesOk && makeFsPipe(blurFS, m_giBlurRP,     m_giBlurPipe);
    // Compute kernels.
    auto makeCompute = [&](VkShaderModule cs, VkPipelineLayout pl, VkPipeline& out) -> bool
    {
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = cs;
        cpi.stage.pName  = "main";
        cpi.layout       = pl;
        return vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpi, nullptr, &out) == VK_SUCCESS;
    };
    pipesOk = pipesOk && makeCompute(shadowCS, m_giShadowPL, m_giShadowPipe);
    pipesOk = pipesOk && makeCompute(probeCS,  m_giProbePL,  m_giProbePipe);
    destroyModules();
    if (!pipesOk)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI pipeline creation failed — GI disabled"); return; }

    // ── Descriptor pool + per-in-flight-frame sets + params UBOs ─────────────
    {
        VkDescriptorPoolSize ps[4] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         k_maxFramesInFlight * 6 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight * 8 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          k_maxFramesInFlight * 4 }, // shadow raw+local, probe irr+vis
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 4 },
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_maxFramesInFlight * 4;
        dpci.poolSizeCount = 4;
        dpci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_giDescPool) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI descriptor pool failed"); return; }
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        {
            VkDescriptorSetLayout layouts[4] = { m_giShadowDSL, m_giProbeDSL, m_giFsDSL, m_giFsDSL };
            VkDescriptorSet sets[4]{};
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool     = m_giDescPool;
            dsai.descriptorSetCount = 4;
            dsai.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_device, &dsai, sets) != VK_SUCCESS)
            { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI descriptor sets failed"); return; }
            m_giShadowSet[i]   = sets[0];
            m_giProbeSet[i]    = sets[1];
            m_giTemporalSet[i] = sets[2];
            m_giBlurSet[i]     = sets[3];
        }
    }
    m_giReady = true;
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: GI pipelines built (software compute ray tracing)");

    // HW ray-query kernels on top (optional): failure logs + degrades to the
    // SW kernels just built — never blocks GI as a whole.
    if (m_giHwRt) createGiHwPipelines();
}

// Builds the two HW ray-query kernel pipelines (gi_shadow_hw/gi_probe_hw) +
// their descriptor plumbing: SW bindings unchanged plus the TLAS at binding 7
// (VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR). Only called when m_giHwRt;
// any failure logs, cleans up and clears m_giHwRt — SW kernels take over.
void VulkanRenderer::createGiHwPipelines()
{
    auto fail = [&](const char* what)
    {
        HE_LOG_WARN(RHI, "%s",
            (std::string("VulkanRenderer: ") + what + " — HW ray tracing off, software GI kernels").c_str());
        if (m_giShadowHwPipe) { vkDestroyPipeline(m_device, m_giShadowHwPipe, nullptr); m_giShadowHwPipe = VK_NULL_HANDLE; }
        if (m_giProbeHwPipe)  { vkDestroyPipeline(m_device, m_giProbeHwPipe, nullptr);  m_giProbeHwPipe  = VK_NULL_HANDLE; }
        if (m_giShadowHwPL)   { vkDestroyPipelineLayout(m_device, m_giShadowHwPL, nullptr); m_giShadowHwPL = VK_NULL_HANDLE; }
        if (m_giProbeHwPL)    { vkDestroyPipelineLayout(m_device, m_giProbeHwPL, nullptr);  m_giProbeHwPL  = VK_NULL_HANDLE; }
        if (m_giHwDescPool)   { vkDestroyDescriptorPool(m_device, m_giHwDescPool, nullptr); m_giHwDescPool = VK_NULL_HANDLE; }
        if (m_giShadowHwDSL)  { vkDestroyDescriptorSetLayout(m_device, m_giShadowHwDSL, nullptr); m_giShadowHwDSL = VK_NULL_HANDLE; }
        if (m_giProbeHwDSL)   { vkDestroyDescriptorSetLayout(m_device, m_giProbeHwDSL, nullptr);  m_giProbeHwDSL  = VK_NULL_HANDLE; }
        for (auto& s : m_giShadowHwSet) s = VK_NULL_HANDLE;
        for (auto& s : m_giProbeHwSet)  s = VK_NULL_HANDLE;
        m_giHwPipesReady = false;
        m_giHwRt         = false;
    };

    auto bindOf = [&](uint32_t b, VkDescriptorType t, const VkSampler* imm = nullptr)
    {
        VkDescriptorSetLayoutBinding r{};
        r.binding = b; r.descriptorType = t; r.descriptorCount = 1;
        r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; r.pImmutableSamplers = imm;
        return r;
    };
    auto makeDSL = [&](const std::vector<VkDescriptorSetLayoutBinding>& binds,
                       VkDescriptorSetLayout& out) -> bool
    {
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = static_cast<uint32_t>(binds.size());
        slci.pBindings    = binds.data();
        return vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &out) == VK_SUCCESS;
    };
    // gi_shadow_hw.comp: SW bindings 0-6 + TLAS at 7 + local mask at 8.
    bool ok = makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &m_giPointSampler),
        bindOf(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &m_giPointSampler),
        bindOf(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        bindOf(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        bindOf(7, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
        bindOf(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
    }, m_giShadowHwDSL);
    // gi_probe_hw.comp: SW bindings 0-5 + TLAS at 7 (6 intentionally unused).
    ok = ok && makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        bindOf(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        bindOf(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        bindOf(7, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
    }, m_giProbeHwDSL);
    if (!ok) { fail("GI HW descriptor layouts failed"); return; }

    auto makePL = [&](VkDescriptorSetLayout dsl, VkPipelineLayout& out) -> bool
    {
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1;
        plci.pSetLayouts    = &dsl;
        return vkCreatePipelineLayout(m_device, &plci, nullptr, &out) == VK_SUCCESS;
    };
    if (!makePL(m_giShadowHwDSL, m_giShadowHwPL) || !makePL(m_giProbeHwDSL, m_giProbeHwPL))
    { fail("GI HW pipeline layouts failed"); return; }

    VkShaderModule shadowCS = loadShaderModule("gi_shadow_hw.comp.spv");
    VkShaderModule probeCS  = loadShaderModule("gi_probe_hw.comp.spv");
    auto destroyModules = [&]()
    {
        if (shadowCS) vkDestroyShaderModule(m_device, shadowCS, nullptr);
        if (probeCS)  vkDestroyShaderModule(m_device, probeCS, nullptr);
    };
    if (!shadowCS || !probeCS)
    { destroyModules(); fail("GI HW shaders missing"); return; }
    auto makeCompute = [&](VkShaderModule cs, VkPipelineLayout pl, VkPipeline& out) -> bool
    {
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = cs;
        cpi.stage.pName  = "main";
        cpi.layout       = pl;
        return vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpi, nullptr, &out) == VK_SUCCESS;
    };
    ok = makeCompute(shadowCS, m_giShadowHwPL, m_giShadowHwPipe)
      && makeCompute(probeCS,  m_giProbeHwPL,  m_giProbeHwPipe);
    destroyModules();
    if (!ok) { fail("GI HW pipeline creation failed"); return; }

    // Own pool: the SW pool has no ACCELERATION_STRUCTURE_KHR budget.
    {
        VkDescriptorPoolSize ps[5] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             k_maxFramesInFlight * 6 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     k_maxFramesInFlight * 2 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              k_maxFramesInFlight * 4 }, // shadow raw+local, probe irr+vis
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             k_maxFramesInFlight * 2 },
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, k_maxFramesInFlight * 2 },
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_maxFramesInFlight * 2;
        dpci.poolSizeCount = 5;
        dpci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_giHwDescPool) != VK_SUCCESS)
        { fail("GI HW descriptor pool failed"); return; }
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        {
            VkDescriptorSetLayout layouts[2] = { m_giShadowHwDSL, m_giProbeHwDSL };
            VkDescriptorSet sets[2]{};
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool     = m_giHwDescPool;
            dsai.descriptorSetCount = 2;
            dsai.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_device, &dsai, sets) != VK_SUCCESS)
            { fail("GI HW descriptor sets failed"); return; }
            m_giShadowHwSet[i] = sets[0];
            m_giProbeHwSet[i]  = sets[1];
        }
    }
    m_giHwPipesReady = true;
    HE_LOG_INFO(RHI, "%s",
                "VulkanRenderer: GI HW ray-query kernels built (VK_KHR_ray_query)");
}

void VulkanRenderer::createGiTargets(uint32_t w, uint32_t h)
{
    w = std::max(1u, w); h = std::max(1u, h);
    if (m_giGBufFB && w == m_giW && h == m_giH) return;
    destroyGiTargets();
    m_giW = w; m_giH = h;

    auto makeImg = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                       GiImage& out) -> bool
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &out.img) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, out.img, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, out.img, out.mem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = out.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { aspect, 0, 1, 0, 1 };
        return vkCreateImageView(m_device, &vci, nullptr, &out.view) == VK_SUCCESS;
    };
    const VkImageUsageFlags kRT = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giGBufPos)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giGBufNorm)
           && makeImg(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT, m_giGBufDepth)
           && makeImg(VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, m_giRaw)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, m_giLocalMask)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giHist[0])
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giHist[1])
           && makeImg(VK_FORMAT_R16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giResult);
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI target creation failed");
        destroyGiTargets();
        return;
    }

    // Framebuffers.
    {
        VkImageView gbufAtts[3] = { m_giGBufPos.view, m_giGBufNorm.view, m_giGBufDepth.view };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_giGBufRP; fci.attachmentCount = 3; fci.pAttachments = gbufAtts;
        fci.width = w; fci.height = h; fci.layers = 1;
        ok = vkCreateFramebuffer(m_device, &fci, nullptr, &m_giGBufFB) == VK_SUCCESS;
        for (int i = 0; i < 2 && ok; ++i)
        {
            VkFramebufferCreateInfo hci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            hci.renderPass = m_giTemporalRP; hci.attachmentCount = 1; hci.pAttachments = &m_giHist[i].view;
            hci.width = w; hci.height = h; hci.layers = 1;
            ok = vkCreateFramebuffer(m_device, &hci, nullptr, &m_giHistFB[i]) == VK_SUCCESS;
        }
        if (ok)
        {
            VkFramebufferCreateInfo rci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            rci.renderPass = m_giBlurRP; rci.attachmentCount = 1; rci.pAttachments = &m_giResult.view;
            rci.width = w; rci.height = h; rci.layers = 1;
            ok = vkCreateFramebuffer(m_device, &rci, nullptr, &m_giResultFB) == VK_SUCCESS;
        }
    }
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI framebuffer creation failed");
        destroyGiTargets();
        return;
    }

    // One-shot layout transitions: raw → GENERAL (storage image, stays there);
    // hist[0/1] → SHADER_READ_ONLY so the FIRST temporal read of "prev" is a
    // defined layout (contents are garbage, but uBlend == 0 on the first GI
    // frame ignores history entirely).
    {
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        auto barrier = [&](VkImage img, VkImageLayout to)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(m_giRaw.img,       VK_IMAGE_LAYOUT_GENERAL);
        barrier(m_giLocalMask.img, VK_IMAGE_LAYOUT_GENERAL); // storage image, stays GENERAL
        barrier(m_giHist[0].img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(m_giHist[1].img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
    }
    m_giHistValid = false; // fresh targets → no usable history
}

void VulkanRenderer::destroyGiTargets()
{
    auto destroy = [&](GiImage& g)
    {
        if (g.view) { vkDestroyImageView(m_device, g.view, nullptr); g.view = VK_NULL_HANDLE; }
        if (g.img)  { vkDestroyImage(m_device, g.img, nullptr);      g.img  = VK_NULL_HANDLE; }
        if (g.mem)  { vkFreeMemory(m_device, g.mem, nullptr);        g.mem  = VK_NULL_HANDLE; }
    };
    if (m_giGBufFB)   { vkDestroyFramebuffer(m_device, m_giGBufFB, nullptr);   m_giGBufFB = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; ++i)
        if (m_giHistFB[i]) { vkDestroyFramebuffer(m_device, m_giHistFB[i], nullptr); m_giHistFB[i] = VK_NULL_HANDLE; }
    if (m_giResultFB) { vkDestroyFramebuffer(m_device, m_giResultFB, nullptr); m_giResultFB = VK_NULL_HANDLE; }
    destroy(m_giGBufPos); destroy(m_giGBufNorm); destroy(m_giGBufDepth);
    destroy(m_giRaw); destroy(m_giLocalMask); destroy(m_giHist[0]); destroy(m_giHist[1]); destroy(m_giResult);
    m_giW = m_giH = 0;
    m_giHistValid = false;
}

// One-shot probe-grid fit over the scene AABB. m_renderWorld was extracted by
// runGi() with real mesh bounds refreshed (same lesson as GL/Metal: the
// extractor seeds only fallback unit cubes).
void VulkanRenderer::ensureGiProbeGrid()
{
    if (m_giProbeGridBuilt) return;
    if (m_renderWorld.objects.empty()) return;

    HE::AABB sceneBox;
    for (const RenderObject& obj : m_renderWorld.objects)
        if (obj.worldBounds.isValid())
            sceneBox.expand(obj.worldBounds);
    if (!sceneBox.isValid()) return;

    const glm::vec3 padded = sceneBox.extents() + glm::vec3(kGIProbeSpacing);
    m_giGridCounts = glm::ivec3(
        std::clamp(static_cast<int>(std::ceil(padded.x * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
        std::clamp(static_cast<int>(std::ceil(padded.y * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
        std::clamp(static_cast<int>(std::ceil(padded.z * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis));
    const glm::vec3 gridSpan = glm::vec3(m_giGridCounts - 1) * kGIProbeSpacing;
    m_giGridOrigin     = sceneBox.center() - gridSpan * 0.5f;
    m_giProbeCount     = m_giGridCounts.x * m_giGridCounts.y * m_giGridCounts.z;
    m_giProbesPerRow   = std::min(m_giProbeCount, 32);
    m_giProbeCursor    = 0;
    m_giProbeGridBuilt = true;
}

void VulkanRenderer::ensureGiProbeAtlas()
{
    if (m_giIrrAtlas.img || m_giProbeCount <= 0) return;
    const int rows = (m_giProbeCount + m_giProbesPerRow - 1) / m_giProbesPerRow;
    const uint32_t aw = static_cast<uint32_t>(m_giProbesPerRow * kGIProbeOctSize);
    const uint32_t ah = static_cast<uint32_t>(rows * kGIProbeOctSize);

    auto makeAtlas = [&](VkFormat fmt, GiImage& out) -> bool
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { aw, ah, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &out.img) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, out.img, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, out.img, out.mem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = out.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return vkCreateImageView(m_device, &vci, nullptr, &out.view) == VK_SUCCESS;
    };
    if (!makeAtlas(VK_FORMAT_R16G16B16A16_SFLOAT, m_giIrrAtlas) ||
        !makeAtlas(VK_FORMAT_R16G16_SFLOAT,       m_giVisAtlas))
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI probe atlas creation failed");
        destroyGiProbeAtlas();
        return;
    }

    // Transition both to GENERAL (imageLoad/Store + sampling live there) and
    // zero-fill — the probe kernel EMA-reads its own previous texel.
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer tmp = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &tmp);
    VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(tmp, &cbi);
    for (GiImage* g : { &m_giIrrAtlas, &m_giVisAtlas })
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g->img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(tmp, g->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    vkEndCommandBuffer(tmp);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
}

void VulkanRenderer::destroyGiProbeAtlas()
{
    auto destroy = [&](GiImage& g)
    {
        if (g.view) { vkDestroyImageView(m_device, g.view, nullptr); g.view = VK_NULL_HANDLE; }
        if (g.img)  { vkDestroyImage(m_device, g.img, nullptr);      g.img  = VK_NULL_HANDLE; }
        if (g.mem)  { vkFreeMemory(m_device, g.mem, nullptr);        g.mem  = VK_NULL_HANDLE; }
    };
    destroy(m_giIrrAtlas);
    destroy(m_giVisAtlas);
    m_giProbeGridBuilt = false;
    m_giProbeCount = 0;
    m_giProbeCursor = 0;
}

// The full GI frame: extraction → accel upload → G-buffer prepass →
// shadow-ray compute → temporal → blur → probe-update compute, all BEFORE the
// main scene render pass (compute can't nest inside one). Mirrors the GL
// port's RenderGiShadow/DispatchGiProbeUpdate structure; Metal lessons (same
// camera as the scene pass, dominant light, tight temporal tolerance) baked in.
void VulkanRenderer::runGi(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (!m_giEnabled || !m_world) return;
    createGiPipelines();
    if (!m_giReady) return;

    // Extract with the scene pass's aspect (Metal lesson 5846efc: a mismatched
    // camera misaligns the screen-space mask → swimming shadows).
    const float aspect = w > 0 && h > 0 ? float(w) / float(h) : 1.0f;
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;
    for (RenderObject& obj : m_renderWorld.objects)
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);

    updateGiAccel();
    if (m_giInstanceCount == 0) return;

    // HW ray tracing: record this frame's TLAS build (+ build→compute barrier)
    // up front. false → software kernels this frame (BVH SSBOs are in place).
    const bool useHw = buildGiTlas(cmd);

    const uint32_t gw = std::max(1u, w / 2), gh = std::max(1u, h / 2); // half-res like GL/Metal
    createGiTargets(gw, gh);
    if (!m_giGBufFB || !m_giResultFB) return;
    ensureGiProbeGrid();
    if (m_giProbeGridBuilt) ensureGiProbeAtlas();

    m_culler.cull(m_renderWorld, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
    if (m_sortedIndices.empty()) return;

    const glm::mat4 vp = HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;
    const uint32_t  fi = m_currentFrame;
    const int curIdx = m_giHistIdx, prevIdx = 1 - curIdx;

    glm::vec3 towardLight, lightColorIntensity;
    m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);

    // ── Params UBOs (host-visible ring slot for this in-flight frame) ────────
    m_giFrameSeed += 1.0f;
    GiShadowUBOData shadowUbo{};
    shadowUbo.sunDirRadius = glm::vec4(towardLight, glm::radians(m_giLightRadius));
    shadowUbo.frame        = glm::vec4(m_giFrameSeed, float(gw), float(gh), float(m_giInstanceCount));
    // First 4 local (point/spot) lights of the same 8-light window the scene
    // shader iterates — scene.frag counts non-directional lights in the SAME
    // order to index the mask channels, which is exactly what the shared packing
    // reproduces.
    {
        const HE::PackedLocalShadowLights ml = HE::BuildMaskedLocalLights(m_renderWorld);
        static_assert(sizeof(shadowUbo.localPosRange) == sizeof(ml.posRange),
                      "gi_shadow.comp's local light slots must match HE::kMaxMaskedLocalLights");
        std::memcpy(shadowUbo.localPosRange, ml.posRange, sizeof(shadowUbo.localPosRange));
        shadowUbo.localExtra = glm::vec4(float(ml.count), 0.0f, 0.0f, 0.0f);
    }
    if (!uploadGiBuffer(m_giShadowUBO[fi], &shadowUbo, sizeof(shadowUbo),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;

    GiTemporalUBOData tempUbo{};
    tempUbo.prevViewProj = m_giPrevViewProj;
    tempUbo.blend        = glm::vec4(m_giHistValid ? 0.9f : 0.0f, 0.0f, 0.0f, 0.0f);
    if (!uploadGiBuffer(m_giTemporalUBO[fi], &tempUbo, sizeof(tempUbo),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;

    const bool probesActive = m_giProbeGridBuilt && m_giIrrAtlas.img && m_giVisAtlas.img;
    const int  probeBudget  = probesActive
        ? std::min(m_giProbeBudgetPerFrame > 0 ? m_giProbeBudgetPerFrame : 1, m_giProbeCount) : 0;
    if (probesActive)
    {
        GiProbeUBOData pu{};
        pu.gridOrigin = glm::vec4(m_giGridOrigin, kGIProbeSpacing);
        pu.gridCounts = glm::vec4(float(m_giGridCounts.x), float(m_giGridCounts.y),
                                  float(m_giGridCounts.z), float(m_giProbesPerRow));
        const float maxDist = glm::length(glm::vec3(m_giGridCounts) * kGIProbeSpacing) + kGIProbeSpacing;
        pu.rayParams  = glm::vec4(maxDist, 0.92f, float(m_giProbeCursor), float(probeBudget));
        pu.skyAmbient = glm::vec4(m_renderWorld.ambient, 0.0f);
        const HE::PackedLightArray pl = HE::BuildPackedLightArray(m_renderWorld);
        static_assert(sizeof(pu.lightPosRange) == sizeof(pl.posRange),
                      "gi_probe.comp's light window must match HE::kMaxLightWindow");
        std::memcpy(pu.lightPosRange,  pl.posRange,  sizeof(pu.lightPosRange));
        std::memcpy(pu.lightColorType, pl.colorType, sizeof(pu.lightColorType));
        std::memcpy(pu.lightDirCos,    pl.dirCos,    sizeof(pu.lightDirCos));
        pu.sunDirRadius = glm::vec4(towardLight, float(pl.count));
        // .w stays Vulkan-specific plumbing (BLAS instance count for the RT path),
        // not part of the shared packing.
        pu.sunColor     = glm::vec4(lightColorIntensity, float(m_giInstanceCount));
        if (!uploadGiBuffer(m_giProbeUBO[fi], &pu, sizeof(pu),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;
    }

    // ── Rewrite this frame slot's descriptor sets (fence already waited) ─────
    // HW frames target the *_hw sets (same bindings + the TLAS at binding 7).
    const VkDescriptorSet shadowSet = useHw ? m_giShadowHwSet[fi] : m_giShadowSet[fi];
    const VkDescriptorSet probeSet  = useHw ? m_giProbeHwSet[fi]  : m_giProbeSet[fi];
    VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    tlasWrite.accelerationStructureCount = 1;
    tlasWrite.pAccelerationStructures    = &m_giTlas[fi].as;
    {
        VkDescriptorBufferInfo nodesBI{ m_giNodeBuf.buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo trisBI { m_giTriBuf.buf,  0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo instBI { m_giInstanceBuf[fi].buf, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo  posBI  { VK_NULL_HANDLE, m_giGBufPos.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  normBI { VK_NULL_HANDLE, m_giGBufNorm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  rawSI  { VK_NULL_HANDLE, m_giRaw.view,      VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo shUboBI{ m_giShadowUBO[fi].buf, 0, sizeof(GiShadowUBOData) };
        std::vector<VkWriteDescriptorSet> writes;
        auto wBuf = [&](VkDescriptorSet set, uint32_t b, VkDescriptorType t, const VkDescriptorBufferInfo* bi)
        {
            VkWriteDescriptorSet ww{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ww.dstSet = set; ww.dstBinding = b; ww.descriptorCount = 1;
            ww.descriptorType = t; ww.pBufferInfo = bi;
            writes.push_back(ww);
        };
        auto wImg = [&](VkDescriptorSet set, uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo* ii)
        {
            VkWriteDescriptorSet ww{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ww.dstSet = set; ww.dstBinding = b; ww.descriptorCount = 1;
            ww.descriptorType = t; ww.pImageInfo = ii;
            writes.push_back(ww);
        };
        auto wTlas = [&](VkDescriptorSet set, uint32_t b)
        {
            VkWriteDescriptorSet ww{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            ww.pNext = &tlasWrite;
            ww.dstSet = set; ww.dstBinding = b; ww.descriptorCount = 1;
            ww.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes.push_back(ww);
        };
        // Shadow kernel set (HW: + TLAS at binding 7).
        wBuf(shadowSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &nodesBI);
        wBuf(shadowSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &trisBI);
        wBuf(shadowSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instBI);
        wImg(shadowSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posBI);
        wImg(shadowSet, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normBI);
        wImg(shadowSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawSI);
        wBuf(shadowSet, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &shUboBI);
        if (useHw) wTlas(shadowSet, 7);
        VkDescriptorImageInfo localSI{ VK_NULL_HANDLE, m_giLocalMask.view, VK_IMAGE_LAYOUT_GENERAL };
        wImg(shadowSet, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &localSI);
        // Temporal set: gPos, raw (GENERAL), history[prev], UBO.
        VkDescriptorImageInfo rawSamp{ VK_NULL_HANDLE, m_giRaw.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo histBI { VK_NULL_HANDLE, m_giHist[prevIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo tUboBI{ m_giTemporalUBO[fi].buf, 0, sizeof(GiTemporalUBOData) };
        wImg(m_giTemporalSet[fi], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posBI);
        wImg(m_giTemporalSet[fi], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rawSamp);
        wImg(m_giTemporalSet[fi], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histBI);
        wBuf(m_giTemporalSet[fi], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &tUboBI);
        // Blur set: reads history[cur]; bindings 1/2 get valid fillers, UBO reused.
        VkDescriptorImageInfo histCurBI{ VK_NULL_HANDLE, m_giHist[curIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        wImg(m_giBlurSet[fi], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histCurBI);
        wImg(m_giBlurSet[fi], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histCurBI);
        wImg(m_giBlurSet[fi], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histCurBI);
        wBuf(m_giBlurSet[fi], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &tUboBI);
        // Probe kernel set.
        VkDescriptorBufferInfo pUboBI{ m_giProbeUBO[fi].buf, 0, sizeof(GiProbeUBOData) };
        VkDescriptorImageInfo  irrSI { VK_NULL_HANDLE, m_giIrrAtlas.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo  visSI { VK_NULL_HANDLE, m_giVisAtlas.view, VK_IMAGE_LAYOUT_GENERAL };
        if (probesActive)
        {
            wBuf(probeSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &nodesBI);
            wBuf(probeSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &trisBI);
            wBuf(probeSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instBI);
            wImg(probeSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &irrSI);
            wImg(probeSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &visSI);
            wBuf(probeSet, 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &pUboBI);
            if (useHw) wTlas(probeSet, 7);
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // ── 1. World-space G-buffer prepass ──────────────────────────────────────
    VkClearValue clears[3]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } }; // a=0 = background
    clears[1].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clears[2].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo gRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    gRPBI.renderPass        = m_giGBufRP;
    gRPBI.framebuffer       = m_giGBufFB;
    gRPBI.renderArea.extent = { gw, gh };
    gRPBI.clearValueCount   = 3;
    gRPBI.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &gRPBI, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giGBufPipe);
    VkViewport vvp{ 0, 0, float(gw), float(gh), 0, 1 };
    VkRect2D   vsc{ { 0, 0 }, { gw, gh } };
    vkCmdSetViewport(cmd, 0, 1, &vvp);
    vkCmdSetScissor(cmd, 0, 1, &vsc);
    for (uint32_t idx : m_sortedIndices)
    {
        const RenderObject& obj = m_renderWorld.objects[idx];
        if (!obj.contributesAO) continue; // precip/particles don't shade the mask
        const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
        const GpuMesh& gm   = mesh ? *mesh : m_cube;
        if (!gm.indexCount) continue;
        // gi_gbuf.vert: uMVP (clip-fixed) + uModel — same 128-byte shape.
        PushConstants pc{ vp * obj.transform, obj.transform };
        vkCmdPushConstants(cmd, m_giGBufPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbuf, &offset);
        vkCmdBindIndexBuffer(cmd, gm.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd); // colors → SHADER_READ_ONLY (dep covers COMPUTE too)

    // ── 2. Shadow rays (compute; HW ray-query kernel when the TLAS is up) ───
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      useHw ? m_giShadowHwPipe : m_giShadowPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            useHw ? m_giShadowHwPL : m_giShadowPL,
                            0, 1, &shadowSet, 0, nullptr);
    vkCmdDispatch(cmd, (gw + 7) / 8, (gh + 7) / 8, 1);
    // Raw mask: compute write → fragment read (temporal). Local mask: compute
    // write → fragment read (scene pass samples it directly — no temporal/blur
    // chain, the rays are deterministic). Both stay GENERAL.
    {
        VkImageMemoryBarrier bs[2]{};
        for (int i = 0; i < 2; ++i)
        {
            bs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bs[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL; bs[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            bs[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bs[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bs[i].image = (i == 0) ? m_giRaw.img : m_giLocalMask.img;
            bs[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            bs[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bs[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
    }

    // ── 3. Temporal accumulation (fullscreen → hist[cur]) ────────────────────
    {
        VkRenderPassBeginInfo tRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        tRPBI.renderPass        = m_giTemporalRP;
        tRPBI.framebuffer       = m_giHistFB[curIdx];
        tRPBI.renderArea.extent = { gw, gh };
        vkCmdBeginRenderPass(cmd, &tRPBI, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giTemporalPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giFsPL,
                                0, 1, &m_giTemporalSet[fi], 0, nullptr);
        vkCmdSetViewport(cmd, 0, 1, &vvp);
        vkCmdSetScissor(cmd, 0, 1, &vsc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
    m_giHistValid    = true;
    m_giHistIdx      = prevIdx;
    m_giPrevViewProj = vp; // clip-fixed, matching the G-buffer raster + temporal math

    // ── 4. Spatial blur (fullscreen → result) ────────────────────────────────
    {
        VkRenderPassBeginInfo bRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bRPBI.renderPass        = m_giBlurRP;
        bRPBI.framebuffer       = m_giResultFB;
        bRPBI.renderArea.extent = { gw, gh };
        vkCmdBeginRenderPass(cmd, &bRPBI, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giBlurPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giFsPL,
                                0, 1, &m_giBlurSet[fi], 0, nullptr);
        vkCmdSetViewport(cmd, 0, 1, &vvp);
        vkCmdSetScissor(cmd, 0, 1, &vsc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // ── 5. Probe update (compute, frame-sliced round robin) ──────────────────
    if (probesActive && probeBudget > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          useHw ? m_giProbeHwPipe : m_giProbePipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                useHw ? m_giProbeHwPL : m_giProbePL,
                                0, 1, &probeSet, 0, nullptr);
        vkCmdDispatch(cmd, static_cast<uint32_t>(probeBudget), 1, 1);
        // Atlases: compute write → fragment read (scene) AND next dispatch's EMA read.
        VkImageMemoryBarrier bs[2]{};
        for (int i = 0; i < 2; ++i)
        {
            bs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bs[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL; bs[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            bs[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bs[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bs[i].image = (i == 0) ? m_giIrrAtlas.img : m_giVisAtlas.img;
            bs[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            bs[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bs[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 2, bs);
        m_giProbeCursor = (m_giProbeCursor + probeBudget) % m_giProbeCount;
    }

    // ── 6. Point the scene set's GI bindings at this frame's outputs ─────────
    // (this frame slot's fence was waited on, so the set is rewritable).
    {
        VkDescriptorImageInfo maskBI{ m_ssaoSampler, m_giResult.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo irrBI { m_ssaoSampler,
            probesActive ? m_giIrrAtlas.view : m_ssaoWhiteView,
            probesActive ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo visBI { m_ssaoSampler,
            probesActive ? m_giVisAtlas.view : m_ssaoWhiteView,
            probesActive ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // Local mask lives in GENERAL (storage image) — legal for sampling.
        VkDescriptorImageInfo localBI{ m_ssaoSampler, m_giLocalMask.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo* infos[4] = { &maskBI, &irrBI, &visBI, &localBI };
        VkWriteDescriptorSet ws[4]{};
        for (uint32_t b = 0; b < 4; ++b)
        {
            ws[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[b].dstSet          = m_frameUBO[fi].set;
            ws[b].dstBinding      = 4 + b;
            ws[b].descriptorCount = 1;
            ws[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            ws[b].pImageInfo      = infos[b];
        }
        vkUpdateDescriptorSets(m_device, 4, ws, 0, nullptr);
    }
    m_giRanThisFrame = true;
}


void VulkanRenderer::SetBloomSettings(const BloomSettings& s)
{
    m_bloomEnabled   = s.enabled;
    m_bloomThreshold = s.threshold;
    m_bloomStrength  = s.intensity;
}

void VulkanRenderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
    m_aaMethod = IRenderer::ResolveAAMethod(s.method, GetCapabilities());
}

void VulkanRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
    // Deferred to the next Render() (render thread), drained under a device idle.
    if (materialId != HE::UUID{})
        m_pendingMatInval.push_back(materialId);
}

void VulkanRenderer::InvalidateMesh(const HE::UUID& meshId)
{
    if (meshId != HE::UUID{})
        m_pendingMeshInval.push_back(meshId);
}

// ─────────────────────────────────────────────────────────────────────────────
// Material warm-up + Content-Browser thumbnails
//
// Port of the OpenGL reference (OpenGLRenderer.cpp:8068-8104 / 8265-8460 /
// 9117-9170), structured after the D3D11 twin that landed first. Where Vulkan
// forces a different mechanism the comment says why.
//
// THE ONE FACT THAT SHAPES ALL OF IT: no frame has rendered when these are
// called. The editor asks for tiles (EditorApplication.cpp:3733/3794) BEFORE the
// first Render() (:4057), so m_viewportImage, m_hdrImage, m_postFxSceneRP and
// everything else createViewportResources() builds are still VK_NULL_HANDLE.
// Every resource here is therefore private to this path, and nothing indexed by
// m_currentFrame is touched (m_matPool[] is vkResetDescriptorPool'd wholesale
// each frame and the m_frameUBO / m_instanceBuf / m_boneUBO rings belong to the
// other in-flight frame).
// ─────────────────────────────────────────────────────────────────────────────

// ─── Material pipeline warm-up ──────────────────────────────────────────────
// Build each node-graph material's VkPipeline NOW so the first draw doesn't
// stall on a synchronous glslang + SPIRV-Cross + vkCreateGraphicsPipelines
// inside the encoder loop. Built-in-PBR materials resolve no graph shader and
// are skipped; cache hits are free.
//
// ONLY THE LDR (swapchain) VARIANT IS WARMABLE HERE, and that is not a choice.
// GetOrBuildMaterialPipeline picks `hdr ? m_postFxSceneRP : m_renderPass` and
// returns VK_NULL_HANDLE *uncached* when that pass is missing. m_renderPass is
// created in Initialize(), so it exists; m_postFxSceneRP is only ever created by
// createPostFXResources(), which is reached only from createViewportResources(),
// which is only called from Render(). So at warm-up time the HDR variant is not
// mis-keyed — it is impossible to build, and asking for it would just burn a
// cross-compile per material and cache nothing. The first HDR draw of each graph
// material therefore still hitches once; closing that needs the warm-up to be
// re-run (or deferred) after the viewport resources exist.
void VulkanRenderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
#if defined(HE_HAVE_SHADERC)
    // m_matReady gates the DRAW path, so warming without it would build pipelines
    // that can never be used.
    if (!m_matReady || !m_contentManager) return;
    if (m_renderPass == VK_NULL_HANDLE) return; // no warmable render pass — nothing to do

    int built = 0;
    for (const HE::UUID& id : materialIds)
    {
        uint64_t hash = 0; std::string frag, vertBody;
        if (!m_matShaderLib.resolveShaders(*m_contentManager, id, hash, frag, vertBody))
            continue; // built-in PBR — nothing to build
        // The cache key is hash ^ (hdr ? A : 0) ^ (transparent ? B : 0), so the
        // opaque/LDR variant's key IS the hash. Tested here (not just inside) so an
        // already-warm material isn't counted as newly built.
        if (m_materialPipelines.count(hash)) continue;
        if (GetOrBuildMaterialPipeline(hash, frag, vertBody, false /*hdr*/, false /*transparent*/))
            ++built;
    }
    if (built > 0)
        HE_LOG_INFO(RHI, "%s",
            ("VulkanRenderer: warmed up " + std::to_string(built) + " material program(s)").c_str());
#else
    (void)materialIds;
#endif
}

// ─── Thumbnail helpers ──────────────────────────────────────────────────────

namespace
{
// Descriptor pool private to the thumbnail AND preview paths. NEVER m_descPool
// (maxSets = 2, fully consumed by the scene's per-frame sets) and never
// m_matPool[] (reset wholesale every frame).
//
// Five sets live here, allocated once each and rewritten per call (safe: every
// path ends in vkQueueWaitIdle, so nothing in flight still references them):
//   mesh tile      1 UBO + 1 sampler
//   particle tile          1 sampler
//   material preview  5 UBO + 8 sampler   (the 13-binding m_matSetLayout)
//   skeletal preview  2 UBO + 1 sampler
//   bone-overlay lines 1 UBO
// = 9 UBO, 11 samplers, 5 sets. Sized with a little headroom rather than exactly,
// because an allocation failure here is silent (the feature just stops drawing).
bool thumbEnsureDescPool(VkDevice dev, VkDescriptorPool& pool)
{
    if (pool) return true;
    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         12 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = 8;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = sizes;
    return vkCreateDescriptorPool(dev, &dpci, nullptr, &pool) == VK_SUCCESS;
}

// Push-constant block of mesh_preview.vert (mat4 uMVP + mat4 uModel).
struct MeshPreviewPush
{
    glm::mat4 mvp;
    glm::mat4 model;
};
static_assert(sizeof(MeshPreviewPush) == 128, "must match mesh_preview.vert push_constant");

// set 0, binding 0 of mesh_preview.frag. NOTE this is 96 bytes, not the D3D11
// twin's 224: there the two matrices live in the same cbuffer, here they are
// push constants (mirroring scene.vert), so only the shading vec4s remain.
struct MeshPreviewUBO
{
    glm::vec4 color;    // rgb = base colour
    glm::vec4 camPos;   // xyz = camera position
    glm::vec4 pbr;      // x = metallic, y = roughness, z = hasTexture (0/1)
    glm::vec4 sun;      // w = 0 → the shader's fixed studio light (GL parity)
    glm::vec4 sunColor;
    glm::vec4 ambient;
};
static_assert(sizeof(MeshPreviewUBO) == 96, "must match mesh_preview.frag MeshPreviewCB");

// Push-constant block of the particle billboard shaders below (112 B, inside the
// 128-byte guaranteed floor — which is why the particle pass needs no UBO at all).
struct ParticlePreviewPush
{
    glm::mat4 viewProj;
    glm::vec4 camRight; // xyz = camera right (view row 0)
    glm::vec4 camUp;    // xyz = camera up    (view row 1)
    glm::vec4 flags;    // x = hasTexture
};
static_assert(sizeof(ParticlePreviewPush) == 112, "must match the particle push_constant block");

#if defined(HE_HAVE_SHADERC)
// ─── Particle-preview billboards (RenderParticleThumbnail) ───────────────────
// Port of OpenGLRenderer's kParticlePreviewVS/FS and the twin of D3D11's
// kParticlePreviewHLSL. Camera-facing quads, one INSTANCE per already-simulated
// particle; the six corner vertices come from gl_VertexIndex (GL uses
// gl_VertexID identically, D3D SV_VertexID), so the only vertex stream is the
// per-instance one — byte-identical to GL's 8-float stride pos3 + size1 +
// color3 + alpha1, read here as two vec4s. This shader only draws; simulation
// lives in ParticleSystem::stepPool.
//
// WHY A RUNTIME-COMPILED STRING RATHER THAN shaders/particle_preview.{vert,frag}:
// adding a .glsl file means adding it to CMakeLists' glslc list, and an
// unregistered shader is simply never compiled to .spv — the pipeline would be
// dead on every machine. he::shaderc::compile() is the same glslang this
// backend's graph materials already go through (and the exact analogue of the
// runtime D3DCompile the D3D11 twin uses), so the shader is built the same way a
// graph material is. Both strings were checked with glslangValidator -V.
//
// The alternative — reusing the mesh_preview pipeline with CPU-built billboard
// geometry — cannot work: mesh_preview.frag ends in `FragColor = vec4(..., 1.0)`,
// so per-particle alpha AND the per-pixel circular falloff are both unreachable,
// and every particle would composite as an opaque square over the Content
// Browser's checkerboard.
constexpr const char* kParticlePreviewVertGlsl = R"GLSL(
#version 450

layout(location = 0) in vec4 aPosSize;    // xyz = world position, w = billboard size
layout(location = 1) in vec4 aColorAlpha; // rgb = tint, a = alpha

layout(push_constant) uniform ParticlePreviewPC {
    mat4 uViewProj;
    vec4 uCamRight;
    vec4 uCamUp;
    vec4 uFlags;
} pc;

layout(location = 0) out vec3  vColor;
layout(location = 1) out float vAlpha;
layout(location = 2) out vec2  vUV;

const vec2 kCorners[6] = vec2[](
    vec2(-1,-1), vec2(1,-1), vec2(1,1),
    vec2(-1,-1), vec2(1,1),  vec2(-1,1)
);

void main()
{
    vec2 corner   = kCorners[gl_VertexIndex % 6];
    vec3 worldPos = aPosSize.xyz
                  + (pc.uCamRight.xyz * corner.x + pc.uCamUp.xyz * corner.y) * (aPosSize.w * 0.5);
    gl_Position = pc.uViewProj * vec4(worldPos, 1.0);
    vUV    = corner * 0.5 + 0.5;
    vColor = aColorAlpha.rgb;
    vAlpha = aColorAlpha.a;
}
)GLSL";

constexpr const char* kParticlePreviewFragGlsl = R"GLSL(
#version 450

layout(location = 0) in vec3  vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec2  vUV;

layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform ParticlePreviewPC {
    mat4 uViewProj;
    vec4 uCamRight;
    vec4 uCamUp;
    vec4 uFlags;    // x = hasTexture (0/1)
} pc;

layout(set = 0, binding = 0) uniform sampler2D uTex;

void main()
{
    bool  hasTex = pc.uFlags.x > 0.5;
    vec4  texc   = hasTex ? texture(uTex, vUV) : vec4(1.0);
    // No texture -> soft circular sprite instead of a flat square, so a bare
    // particle system still reads as "particles" rather than "confetti".
    float shape  = hasTex ? texc.a : smoothstep(1.0, 0.0, length(vUV * 2.0 - 1.0));
    FragColor = vec4(vColor * texc.rgb, vAlpha * shape);
}
)GLSL";
#endif // HE_HAVE_SHADERC
} // namespace

// Lazily (re)create the shared S×S thumbnail target. Its OWN image, not the
// viewport's: a tile rendered into m_viewportImage would replace what the scene
// view is showing — and on the first run that image does not exist at all.
// createViewportResources() is deliberately never called from here; it retires
// the ImGui-visible image and rebuilds PostFX + SSAO as a side effect.
bool VulkanRenderer::ensureThumbnailTarget(int S)
{
    // m_thumbUIFB is deliberately NOT part of this test: it only exists when
    // createUIPipeline built m_uiViewportRP, and requiring it would tear the whole
    // target down and rebuild it on every single call when the UI pipeline failed.
    // RenderWidgetThumbnail checks that framebuffer itself.
    if (m_thumbFB && m_thumbStageBuf && m_thumbSize == S) return true;
    destroyThumbnailTarget();
    if (!m_device || S <= 0) return false;

    const uint32_t dim = static_cast<uint32_t>(S);

    // ── Colour image: R8G8B8A8_UNORM ────────────────────────────────────────
    // Mandatory colour-attachment format in Vulkan, already the RGBA8 byte order
    // IRenderer's contract asks for (a BGRA target would need an R↔B swap on
    // readback), and the same format m_uiViewportRP declares — which is what
    // lets the widget tile reuse that render pass and m_uiViewportPipeline.
    // TRANSFER_DST is for the widget path's vkCmdClearColorImage (its render pass
    // is LOAD_OP_LOAD and cannot clear); TRANSFER_SRC for the readback copy.
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent        = { dim, dim, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &m_thumbImage) != VK_SUCCESS) return false;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_thumbImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_thumbMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_thumbImage, m_thumbMemory, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_thumbImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &m_thumbView) != VK_SUCCESS) return false;
    }

    // ── Depth image (mesh/particle tiles; the widget tile has no depth) ─────
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = m_depthFormat;
        ici.extent        = { dim, dim, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &m_thumbDepthImage) != VK_SUCCESS) return false;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_thumbDepthImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_thumbDepthMem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_thumbDepthImage, m_thumbDepthMem, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_thumbDepthImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = m_depthFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &m_thumbDepthView) != VK_SUCCESS) return false;
    }

    if (!ensurePreviewRenderPass()) return false;

    // ── Framebuffers: colour+depth for meshes, colour-only for the widget ────
    {
        VkImageView atts[2] = { m_thumbView, m_thumbDepthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_thumbRenderPass;
        fci.attachmentCount = 2;
        fci.pAttachments    = atts;
        fci.width           = dim;
        fci.height          = dim;
        fci.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_thumbFB) != VK_SUCCESS) return false;
    }
    if (m_uiViewportRP)
    {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_uiViewportRP;
        fci.attachmentCount = 1;
        fci.pAttachments    = &m_thumbView;
        fci.width           = dim;
        fci.height          = dim;
        fci.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_thumbUIFB) != VK_SUCCESS) return false;
    }

    // ── Cached readback staging buffer ──────────────────────────────────────
    // The Content Browser asks for many tiles in a row and a per-tile
    // create/allocate/free at a fixed size is pure overhead.
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = static_cast<VkDeviceSize>(S) * S * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_thumbStageBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_thumbStageBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_thumbStageMem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, m_thumbStageBuf, m_thumbStageMem, 0);
    }

    m_thumbSize = S;
    return true;
}

// The one colour+depth render pass every tile AND every preview goes through:
// clear to FULLY TRANSPARENT, end in TRANSFER_SRC_OPTIMAL. Size-independent, so
// it survives every resize — only images, framebuffers and staging buffers are
// rebuilt around it. finalLayout TRANSFER_SRC saves the tile readback a barrier;
// the previews, which need SHADER_READ_ONLY for ImGui, pay for one instead (see
// endPreviewPass) because the tiles are the hot path and the previews are not.
//
// Split out of ensureThumbnailTarget so the previews can share it. That sharing
// is what lets the P1b mesh and particle pipelines — both built against this
// pass — be bound inside a PREVIEW's framebuffer without a second variant:
// Vulkan render-pass compatibility is attachment count / formats / sample counts,
// and every one of these targets is R8G8B8A8_UNORM + m_depthFormat, 1 sample.
bool VulkanRenderer::ensurePreviewRenderPass()
{
    if (m_thumbRenderPass != VK_NULL_HANDLE) return true;
    if (!m_device) return false;
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = VK_FORMAT_R8G8B8A8_UNORM;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

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
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkAttachmentDescription atts[2] = { colorAtt, depthAtt };
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2;
        rpci.pAttachments    = atts;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_thumbRenderPass) != VK_SUCCESS)
            return false;
    }
    return true;
}

void VulkanRenderer::destroyThumbnailTarget()
{
    if (!m_device) return;
    if (m_thumbUIFB)       { vkDestroyFramebuffer(m_device, m_thumbUIFB,       nullptr); m_thumbUIFB       = VK_NULL_HANDLE; }
    if (m_thumbFB)         { vkDestroyFramebuffer(m_device, m_thumbFB,         nullptr); m_thumbFB         = VK_NULL_HANDLE; }
    if (m_thumbDepthView)  { vkDestroyImageView  (m_device, m_thumbDepthView,  nullptr); m_thumbDepthView  = VK_NULL_HANDLE; }
    if (m_thumbDepthImage) { vkDestroyImage      (m_device, m_thumbDepthImage, nullptr); m_thumbDepthImage = VK_NULL_HANDLE; }
    if (m_thumbDepthMem)   { vkFreeMemory        (m_device, m_thumbDepthMem,   nullptr); m_thumbDepthMem   = VK_NULL_HANDLE; }
    if (m_thumbView)       { vkDestroyImageView  (m_device, m_thumbView,       nullptr); m_thumbView       = VK_NULL_HANDLE; }
    if (m_thumbImage)      { vkDestroyImage      (m_device, m_thumbImage,      nullptr); m_thumbImage      = VK_NULL_HANDLE; }
    if (m_thumbMemory)     { vkFreeMemory        (m_device, m_thumbMemory,     nullptr); m_thumbMemory     = VK_NULL_HANDLE; }
    if (m_thumbStageBuf)   { vkDestroyBuffer     (m_device, m_thumbStageBuf,   nullptr); m_thumbStageBuf   = VK_NULL_HANDLE; }
    if (m_thumbStageMem)   { vkFreeMemory        (m_device, m_thumbStageMem,   nullptr); m_thumbStageMem   = VK_NULL_HANDLE; }
    m_thumbSize = 0;
}

// Everything this path ever created. Called from Shutdown() — this backend
// already leaks 10 objects at vkDestroyDevice and adding to that noise would
// make the pre-existing ones impossible to untangle.
void VulkanRenderer::destroyThumbnailResources()
{
    if (!m_device) return;
    destroyThumbnailTarget();
    if (m_thumbRenderPass)  { vkDestroyRenderPass(m_device, m_thumbRenderPass, nullptr); m_thumbRenderPass = VK_NULL_HANDLE; }

    if (m_particlePvPipe)   { vkDestroyPipeline      (m_device, m_particlePvPipe,   nullptr); m_particlePvPipe   = VK_NULL_HANDLE; }
    if (m_particlePvLayout) { vkDestroyPipelineLayout(m_device, m_particlePvLayout, nullptr); m_particlePvLayout = VK_NULL_HANDLE; }
    if (m_particlePvDSL)    { vkDestroyDescriptorSetLayout(m_device, m_particlePvDSL, nullptr); m_particlePvDSL  = VK_NULL_HANDLE; }
    if (m_particlePvVBPtr)  { vkUnmapMemory(m_device, m_particlePvVBMem); m_particlePvVBPtr = nullptr; }
    if (m_particlePvVB)     { vkDestroyBuffer(m_device, m_particlePvVB,    nullptr); m_particlePvVB    = VK_NULL_HANDLE; }
    if (m_particlePvVBMem)  { vkFreeMemory   (m_device, m_particlePvVBMem, nullptr); m_particlePvVBMem = VK_NULL_HANDLE; }
    m_particlePvCap = 0;

    if (m_meshPvPipe)   { vkDestroyPipeline      (m_device, m_meshPvPipe,   nullptr); m_meshPvPipe   = VK_NULL_HANDLE; }
    if (m_meshPvLayout) { vkDestroyPipelineLayout(m_device, m_meshPvLayout, nullptr); m_meshPvLayout = VK_NULL_HANDLE; }
    if (m_meshPvDSL)    { vkDestroyDescriptorSetLayout(m_device, m_meshPvDSL, nullptr); m_meshPvDSL  = VK_NULL_HANDLE; }
    // Unmap BEFORE destroying the buffer — a mapped allocation freed with the
    // pointer still live is exactly the shape of the leaks already in here.
    if (m_meshPvUBOPtr) { vkUnmapMemory(m_device, m_meshPvUBOMem); m_meshPvUBOPtr = nullptr; }
    if (m_meshPvUBO)    { vkDestroyBuffer(m_device, m_meshPvUBO,    nullptr); m_meshPvUBO    = VK_NULL_HANDLE; }
    if (m_meshPvUBOMem) { vkFreeMemory   (m_device, m_meshPvUBOMem, nullptr); m_meshPvUBOMem = VK_NULL_HANDLE; }

    if (m_previewSphereVB)   { vkDestroyBuffer(m_device, m_previewSphereVB,   nullptr); m_previewSphereVB   = VK_NULL_HANDLE; }
    if (m_previewSphereVMem) { vkFreeMemory   (m_device, m_previewSphereVMem, nullptr); m_previewSphereVMem = VK_NULL_HANDLE; }
    if (m_previewSphereIB)   { vkDestroyBuffer(m_device, m_previewSphereIB,   nullptr); m_previewSphereIB   = VK_NULL_HANDLE; }
    if (m_previewSphereIMem) { vkFreeMemory   (m_device, m_previewSphereIMem, nullptr); m_previewSphereIMem = VK_NULL_HANDLE; }
    m_previewSphereIdx = 0;
    m_previewShape     = -1;

    // Frees every set allocated from it (mesh/particle tiles AND the three
    // preview sets) with it.
    if (m_thumbDescPool) { vkDestroyDescriptorPool(m_device, m_thumbDescPool, nullptr); m_thumbDescPool = VK_NULL_HANDLE; }
    m_meshPvSet     = VK_NULL_HANDLE;
    m_particlePvSet = VK_NULL_HANDLE;
    m_matPvSet      = VK_NULL_HANDLE;
    m_skelPvSet     = VK_NULL_HANDLE;
    m_skelLineSet   = VK_NULL_HANDLE;
}

// One-shot primary command buffer, exactly the recipe CaptureViewport (:2888)
// and CreateImGuiTexture use: idle the device first (nothing here participates in
// the frame's sync objects), record, submit with a NULL fence, wait on the queue,
// free. m_cmdPool was created with RESET_COMMAND_BUFFER_BIT and is never
// wholesale-reset, so allocating from it out-of-frame is safe.
VkCommandBuffer VulkanRenderer::beginThumbnailCmd()
{
    vkDeviceWaitIdle(m_device);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &cbai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);
    return cmd;
}

void VulkanRenderer::endThumbnailCmd(VkCommandBuffer cmd)
{
    if (!cmd) return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);
}

// Copy the colour image into the cached staging buffer. `from` is the layout the
// image is already in when this is recorded (TRANSFER_SRC after the mesh render
// pass, SHADER_READ_ONLY after the widget one).
void VulkanRenderer::recordThumbnailReadback(VkCommandBuffer cmd, int S, VkImageLayout from)
{
    if (from != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = from;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_thumbImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        // Wide source scope on purpose: m_uiViewportRP has no explicit
        // subpass→EXTERNAL dependency, so the writes this has to wait on are the
        // colour-attachment ones, while the layout it is transitioning FROM is the
        // shader-read one that pass ends in. Covering both leaves no hazard.
        b.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { static_cast<uint32_t>(S), static_cast<uint32_t>(S), 1 };
    vkCmdCopyImageToBuffer(cmd, m_thumbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_thumbStageBuf, 1, &region);
}

// Map the staging buffer out as tightly packed RGBA8. NO VERTICAL FLIP, and that
// is not an omission: GL's `S-1-y` flip corrects for GL's bottom-up framebuffer.
// Vulkan's framebuffer row 0 IS the top row, and kVulkanClipFix already flips
// clip-space Y once, so world +Y lands on row 0 — the same picture GL produces
// after its flip. A readback flip here would mirror the tile vertically.
// vkCmdCopyImageToBuffer with bufferRowLength = 0 packs rows at exactly
// imageExtent.width texels, so there is no row pitch to de-stripe either.
bool VulkanRenderer::mapThumbnailReadback(int S, std::vector<uint8_t>& outRgba8)
{
    if (!m_thumbStageMem) return false;
    const VkDeviceSize total = static_cast<VkDeviceSize>(S) * S * 4;
    void* mapped = nullptr;
    if (vkMapMemory(m_device, m_thumbStageMem, 0, total, 0, &mapped) != VK_SUCCESS) return false;
    outRgba8.resize(static_cast<size_t>(total));
    std::memcpy(outRgba8.data(), mapped, static_cast<size_t>(total));
    vkUnmapMemory(m_device, m_thumbStageMem);
    return true;
}

// Clear to FULLY TRANSPARENT and leave the image ready to be a colour attachment.
// Only the widget tile needs this — its render pass is m_uiViewportRP, which is
// LOAD_OP_LOAD (it exists to composite UI onto a finished frame). The clear
// colour MUST be (0,0,0,0): the Content Browser composites the tile over a
// checkerboard, so an opaque background is a visible regression.
void VulkanRenderer::clearThumbnailColor(VkCommandBuffer cmd)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED; // previous contents are dead
    b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = m_thumbImage;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    const VkClearColorValue clear{ { 0.0f, 0.0f, 0.0f, 0.0f } };
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, m_thumbImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // m_uiViewportRP's initialLayout
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}

// Build the small pos/normal/uv preview program once. mesh_preview.vert/.frag
// are the Vulkan twin of the HLSL D3D11/D3D12 share and of GL's kMeshPreview*
// program, and are compiled to .spv by the same CMake glslc step as every other
// shader in this backend — so they load exactly like scene.vert.spv.
bool VulkanRenderer::ensureMeshPreviewPipeline()
{
    if (m_meshPvPipe && m_meshPvSet) return true;
    if (m_meshPvTried) return false;   // one attempt per session, like the GI pipelines
    m_meshPvTried = true;
    if (!m_device || m_thumbRenderPass == VK_NULL_HANDLE) return false;
    // Both come from createScenePipeline (Initialize, so before any Render()) and
    // are what this pass binds instead of creating a sampler / white default of
    // its own — one fewer object to destroy, one fewer chance to leak.
    if (!m_albedoSampler || !m_whiteAlbedoView) return false;

    VkShaderModule vs = loadShaderModule("mesh_preview.vert.spv");
    VkShaderModule fs = loadShaderModule("mesh_preview.frag.spv");
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: mesh_preview.spv missing — no asset thumbnails");
        return false;
    }
    // Local guard: every failure path below must still release the modules.
    struct ModGuard {
        VkDevice d; VkShaderModule a, b;
        ~ModGuard() { if (a) vkDestroyShaderModule(d, a, nullptr); if (b) vkDestroyShaderModule(d, b, nullptr); }
    } guard{ m_device, vs, fs };

    // set 0: b0 = MeshPreviewCB (FS), b1 = uTex (FS).
    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding         = 0;
        binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding         = 1;
        binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = 2;
        dslci.pBindings    = binds;
        if (vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_meshPvDSL) != VK_SUCCESS)
            return false;
    }

    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPreviewPush) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_meshPvDSL;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_meshPvLayout) != VK_SUCCESS) return false;

    // Host-visible, persistently mapped UBO — one draw per tile, so a staging
    // copy would be pure ceremony.
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = sizeof(MeshPreviewUBO);
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_meshPvUBO) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_meshPvUBO, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_meshPvUBOMem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, m_meshPvUBO, m_meshPvUBOMem, 0);
        if (vkMapMemory(m_device, m_meshPvUBOMem, 0, sizeof(MeshPreviewUBO), 0, &m_meshPvUBOPtr) != VK_SUCCESS)
            return false;
    }

    if (!thumbEnsureDescPool(m_device, m_thumbDescPool)) return false;
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_thumbDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_meshPvDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_meshPvSet) != VK_SUCCESS) return false;
    }

    // ── Pipeline ────────────────────────────────────────────────────────────
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    // Same interleaved 32-byte pos/normal/uv vertex the scene meshes and
    // HE::buildPreviewMesh both produce.
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
    rs.cullMode    = VK_CULL_MODE_NONE;  // GL disables culling for previews
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable    = VK_FALSE; // the shader writes alpha 1 exactly where it draws
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
    pci.layout              = m_meshPvLayout;
    pci.renderPass          = m_thumbRenderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_meshPvPipe) != VK_SUCCESS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: mesh-preview pipeline creation failed");
        return false;
    }
    return true;
}

// Upload the procedural preview primitive (0 sphere / 1 cube / 2 plane). ONE
// shared buffer pair, rebuilt when the requested shape changes — the same single
// m_previewVAO + m_previewShape arrangement OpenGL uses, so the Material Editor's
// shape switch and the Content Browser's always-sphere tile thrash it against
// each other identically on both backends. Rebuilding is safe because every entry
// point here ends in vkQueueWaitIdle before the next one records, so the buffers
// being destroyed are never in flight.
//
// Device-local via the shared createMeshBuffers path would need a GpuMesh; a
// host-visible pair is simpler for geometry drawn once per tile/preview.
bool VulkanRenderer::ensurePreviewMesh(int shape)
{
    if (m_previewSphereVB && m_previewSphereIB && m_previewSphereIdx && m_previewShape == shape)
        return true;
    if (!m_device) return false;

    std::vector<float>    verts;
    std::vector<uint32_t> idx;
    HE::buildPreviewMesh(shape, verts, idx);
    if (verts.empty() || idx.empty()) return false;

    // Drop the previous shape's buffers first — this is a rebuild, not a leak.
    if (m_previewSphereVB)   { vkDestroyBuffer(m_device, m_previewSphereVB,   nullptr); m_previewSphereVB   = VK_NULL_HANDLE; }
    if (m_previewSphereVMem) { vkFreeMemory   (m_device, m_previewSphereVMem, nullptr); m_previewSphereVMem = VK_NULL_HANDLE; }
    if (m_previewSphereIB)   { vkDestroyBuffer(m_device, m_previewSphereIB,   nullptr); m_previewSphereIB   = VK_NULL_HANDLE; }
    if (m_previewSphereIMem) { vkFreeMemory   (m_device, m_previewSphereIMem, nullptr); m_previewSphereIMem = VK_NULL_HANDLE; }
    m_previewSphereIdx = 0;
    m_previewShape     = -1;

    auto makeBuf = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, const void* data,
                       VkBuffer& buf, VkDeviceMemory& mem) -> bool
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = bytes;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buf, mem, 0);
        void* p = nullptr;
        if (vkMapMemory(m_device, mem, 0, bytes, 0, &p) != VK_SUCCESS) return false;
        std::memcpy(p, data, static_cast<size_t>(bytes));
        vkUnmapMemory(m_device, mem);
        return true;
    };

    if (!makeBuf(verts.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data(),
                 m_previewSphereVB, m_previewSphereVMem)) return false;
    if (!makeBuf(idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx.data(),
                 m_previewSphereIB, m_previewSphereIMem)) return false;
    m_previewSphereIdx = static_cast<uint32_t>(idx.size());
    m_previewShape     = shape;
    return true;
}

// Draw one pos/normal/uv mesh, orbit-framed on the caller's bounds. Port of
// OpenGLRenderer::DrawMeshPreviewGeometry (:8265); the caller owns the render
// pass, the framebuffer and the clear.
void VulkanRenderer::drawMeshPreviewGeometry(VkCommandBuffer cmd, int S,
                                             VkBuffer vb, VkBuffer ib, uint32_t indexCount,
                                             VkImageView texture,
                                             const glm::vec3& center, float extent,
                                             const glm::vec3& baseColor, float metallic,
                                             float roughness, float yaw, float pitch, float dist)
{
    if (!vb || !ib || indexCount == 0) return;
    if (!ensureMeshPreviewPipeline()) return;

    // HE::meshOrbit returns an OpenGL-convention projection (depth -1..1, Y up);
    // Vulkan pre-multiplies exactly ONE fix-up, which also flips clip-space Y.
    // glm::perspective must NOT be called here — GLM_FORCE_DEPTH_ZERO_TO_ONE is a
    // private compile definition on this target, so it would remap depth a second
    // time (see the note at the top of PreviewFraming.h).
    const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist);
    const glm::mat4 model(1.0f);

    MeshPreviewUBO ubo{};
    ubo.color    = glm::vec4(baseColor, 1.0f);
    ubo.camPos   = glm::vec4(cam.position, 1.0f);
    ubo.pbr      = glm::vec4(metallic, roughness, texture ? 1.0f : 0.0f, 0.0f);
    ubo.sun      = glm::vec4(0.0f); // w = 0 → the shader's fixed studio light, exactly like GL
    ubo.sunColor = glm::vec4(1.0f);
    ubo.ambient  = glm::vec4(0.32f, 0.32f, 0.32f, 0.0f);
    std::memcpy(m_meshPvUBOPtr, &ubo, sizeof(ubo));

    // The albedo VIEW, never a GpuMesh's albedoSet: that set was allocated against
    // the scene's m_albedoSetLayout out of m_albedoPool, and binding it to this
    // pipeline layout is a validation error. m_albedoSampler is linear+repeat and
    // already exists (createScenePipeline, before any Render()), so this path
    // creates no sampler of its own.
    VkDescriptorBufferInfo bi{ m_meshPvUBO, 0, sizeof(MeshPreviewUBO) };
    VkDescriptorImageInfo  ii{ m_albedoSampler, texture ? texture : m_whiteAlbedoView,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = m_meshPvSet; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = m_meshPvSet; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &ii;
    // Rewriting the set in place is safe because every tile ends in
    // vkQueueWaitIdle — no submission using it is still in flight.
    vkUpdateDescriptorSets(m_device, 2, w, 0, nullptr);

    MeshPreviewPush push{};
    push.mvp   = HE::kVulkanClipFix * cam.proj * cam.view * model;
    push.model = model;

    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(S), static_cast<float>(S), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor (cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPvPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPvLayout,
                            0, 1, &m_meshPvSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_meshPvLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}

// Build the billboard pipeline from the runtime-compiled GLSL above. Gated on
// HE_HAVE_SHADERC: without the cross-compiler there is no way to turn a shader
// string into SPIR-V here, and adding a .glsl file is not an option (it would
// need a CMakeLists entry to ever become a .spv).
bool VulkanRenderer::ensureParticlePreviewPipeline()
{
#if defined(HE_HAVE_SHADERC)
    if (m_particlePvPipe && m_particlePvSet) return true;
    if (m_particlePvTried) return false;
    m_particlePvTried = true;
    if (!m_device || m_thumbRenderPass == VK_NULL_HANDLE) return false;
    if (!m_albedoSampler || !m_whiteAlbedoView) return false; // see ensureMeshPreviewPipeline

    const he::shaderc::Result vr = he::shaderc::compile(
        kParticlePreviewVertGlsl, he::shaderc::Stage::Vertex, he::shaderc::Target::SpirvBinary);
    const he::shaderc::Result fr = he::shaderc::compile(
        kParticlePreviewFragGlsl, he::shaderc::Stage::Fragment, he::shaderc::Target::SpirvBinary);
    if (!vr.ok || !fr.ok || vr.spirv.empty() || fr.spirv.empty())
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: particle-preview shader compile failed");
        return false;
    }
    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule vs = makeModule(vr.spirv);
    VkShaderModule fs = makeModule(fr.spirv);
    struct ModGuard {
        VkDevice d; VkShaderModule a, b;
        ~ModGuard() { if (a) vkDestroyShaderModule(d, a, nullptr); if (b) vkDestroyShaderModule(d, b, nullptr); }
    } guard{ m_device, vs, fs };
    if (!vs || !fs) return false;

    {
        VkDescriptorSetLayoutBinding bind{};
        bind.binding         = 0;
        bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = 1;
        dslci.pBindings    = &bind;
        if (vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_particlePvDSL) != VK_SUCCESS)
            return false;
    }
    // 112 B fits under the guaranteed 128-byte push-constant floor, so the whole
    // per-draw state rides in push constants and this pass needs no UBO.
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(ParticlePreviewPush) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_particlePvDSL;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_particlePvLayout) != VK_SUCCESS) return false;

    if (!thumbEnsureDescPool(m_device, m_thumbDescPool)) return false;
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_thumbDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_particlePvDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_particlePvSet) != VK_SUCCESS) return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    // Per-INSTANCE only — the six corners come from gl_VertexIndex, so there is no
    // per-vertex stream. 32 bytes, byte-identical to GL's instance buffer.
    VkVertexInputBindingDescription bind{ 0, 32u, VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0  }, // pos3 + size1
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 }, // color3 + alpha1
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;   // GL: glDisable(GL_CULL_FACE)
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;   // GL: glEnable(GL_DEPTH_TEST) + glDepthFunc(GL_LESS)
    ds.depthWriteEnable = VK_FALSE;  // GL: glDepthMask(GL_FALSE)
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;
    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA on ALL FOUR channels — deliberately NOT the
    // scene's transparent blend (ONE / ZERO on alpha), which would make the tile's
    // destination alpha the LAST particle's alpha instead of accumulating. GL's
    // glBlendFunc applies to all four, and the tile's alpha is exactly what the
    // Content Browser composites with.
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
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
    pci.layout              = m_particlePvLayout;
    pci.renderPass          = m_thumbRenderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_particlePvPipe) != VK_SUCCESS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: particle-preview pipeline creation failed");
        return false;
    }
    return true;
#else
    return false;
#endif
}

// Port of OpenGLRenderer::DrawParticlePreviewGeometry (:9053). The caller owns
// the render pass, framebuffer and clear.
void VulkanRenderer::drawParticlePreviewGeometry(VkCommandBuffer cmd, int S, VkImageView texture,
                                                 const std::vector<ParticlePreviewInstance>& particles,
                                                 float yaw, float pitch, float dist)
{
    if (particles.empty() || !ensureParticlePreviewPipeline()) return;

    // Orbit camera auto-framed on the LIVE particles' bounds (an emitter's extent
    // depends on velocity/gravity/lifetime, unlike a fixed mesh). Each particle is
    // a BILLBOARD of radius size*0.5, so the bounds must include that radius —
    // framing on the centres alone crops every quad by half its width. The 0.1
    // floor is GL's and is load-bearing: HE::meshOrbit only floors at 0.05, so a
    // tight cloud of small sprites would otherwise frame closer here than on GL.
    glm::vec3 bmin(1e30f), bmax(-1e30f);
    for (const auto& q : particles)
    {
        const glm::vec3 r(q.size * 0.5f);
        bmin = glm::min(bmin, q.position - r);
        bmax = glm::max(bmax, q.position + r);
    }
    const bool      valid  = bmin.x <= bmax.x;
    const glm::vec3 center = valid ? (bmin + bmax) * 0.5f : glm::vec3(0.0f);
    const float     extent = valid ? std::max(glm::length(bmax - bmin) * 0.5f, 0.1f) : 1.0f;

    const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist);
    ParticlePreviewPush push{};
    push.viewProj = HE::kVulkanClipFix * cam.proj * cam.view; // one fix-up, see drawMeshPreviewGeometry
    // Camera-facing basis for billboard expansion (right = view row 0, up = row 1).
    push.camRight = glm::vec4(cam.view[0][0], cam.view[1][0], cam.view[2][0], 0.0f);
    push.camUp    = glm::vec4(cam.view[0][1], cam.view[1][1], cam.view[2][1], 0.0f);
    push.flags    = glm::vec4(texture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

    // Instance stream: pos3 + size1 + color3 + alpha1, the same 8 floats as GL.
    const uint32_t count = static_cast<uint32_t>(particles.size());
    if (m_particlePvCap < count)
    {
        if (m_particlePvVBPtr) { vkUnmapMemory(m_device, m_particlePvVBMem); m_particlePvVBPtr = nullptr; }
        if (m_particlePvVB)    { vkDestroyBuffer(m_device, m_particlePvVB,    nullptr); m_particlePvVB    = VK_NULL_HANDLE; }
        if (m_particlePvVBMem) { vkFreeMemory   (m_device, m_particlePvVBMem, nullptr); m_particlePvVBMem = VK_NULL_HANDLE; }
        m_particlePvCap = 0;

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = static_cast<VkDeviceSize>(count) * 32u;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_particlePvVB) != VK_SUCCESS) return;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_particlePvVB, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_particlePvVBMem) != VK_SUCCESS) return;
        vkBindBufferMemory(m_device, m_particlePvVB, m_particlePvVBMem, 0);
        if (vkMapMemory(m_device, m_particlePvVBMem, 0, bci.size, 0, &m_particlePvVBPtr) != VK_SUCCESS) return;
        m_particlePvCap = count;
    }
    std::vector<float> inst;
    inst.reserve(static_cast<size_t>(count) * 8);
    for (const auto& q : particles)
        inst.insert(inst.end(), { q.position.x, q.position.y, q.position.z, q.size,
                                  q.color.x, q.color.y, q.color.z, q.alpha });
    std::memcpy(m_particlePvVBPtr, inst.data(), inst.size() * sizeof(float));

    VkDescriptorImageInfo ii{ m_albedoSampler, texture ? texture : m_whiteAlbedoView,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = m_particlePvSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &ii;
    vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);

    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(S), static_cast<float>(S), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor (cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particlePvPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particlePvLayout,
                            0, 1, &m_particlePvSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_particlePvLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_particlePvVB, &off);
    vkCmdDraw(cmd, 6, count, 0, 0);
}

// ─── Content-Browser asset thumbnails ───────────────────────────────────────
bool VulkanRenderer::RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind,
                                          const HE::UUID& assetId, uint32_t size,
                                          std::vector<uint8_t>& outRgba8)
{
    const int S = HE::clampThumbnailSize(size);
    // Adopt the ContentManager: thumbnails are requested BEFORE the first Render(),
    // so SetContentManager may not have run yet and resolveMesh would find nothing.
    if (!m_contentManager) m_contentManager = &cm;
    if (assetId == HE::UUID{} || !m_device) return false;
    if (!ensureThumbnailTarget(S)) return false;
    if (!ensureMeshPreviewPipeline()) return false;

    // Resolve everything the draw needs BEFORE recording: resolveMesh /
    // resolveMaterialOverride upload textures through their own one-shot
    // submits + vkQueueWaitIdle, which must not happen inside our render pass.
    VkBuffer    vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    uint32_t    indexCount = 0;
    VkImageView tex = VK_NULL_HANDLE;
    glm::vec3   center(0.0f), baseColor(HE::kMeshTileBaseColor);
    float       extent = 1.0f;
    float       metallic = HE::kMeshTileMetallic, roughness = HE::kMeshTileRoughness;
    float       dist = HE::kMeshFrameDist;

    if (kind == ThumbnailKind::StaticMesh)
    {
        const GpuMesh* mesh = resolveMesh(assetId);
        if (!mesh) return false;
        vb = mesh->vbuf; ib = mesh->ibuf; indexCount = mesh->indexCount;
        tex        = mesh->albedoView;
        center     = HE::boundsCenter(mesh->localBounds);
        extent     = HE::boundsExtent(mesh->localBounds);
    }
    else if (kind == ThumbnailKind::Material)
    {
        // The BUILT-IN PBR fallback sphere for EVERY material, graph ones included.
        //
        // Deliberately NOT the graph-material pipeline DrawScene uses. That path
        // binds the white default to heTexP0..3 (see the TODO next to the heTex0
        // selection in DrawScene) because this backend has no graph-texture cache
        // yet — the A4 graph-texture-cache gap. A graph material rendered through
        // it would come out untextured-white and would NOT match the OpenGL tile,
        // which is worse than a flat-shaded sphere: it looks like a working
        // feature producing wrong pixels. A PBR sphere is the correct tile for a
        // built-in material and an honest approximation for a graph one. Close
        // this the day heTexP0..3 resolve real project textures.
        if (!ensurePreviewMesh(0 /*sphere*/)) return false;
        vb = m_previewSphereVB; ib = m_previewSphereIB; indexCount = m_previewSphereIdx;
        const MaterialAsset* ma = m_contentManager ? m_contentManager->getMaterial(assetId) : nullptr;
        // Same base-texture selection as the scene's override path (getMaterial →
        // textureIds[0]/texturePaths[0] → view), mirroring GL's ResolveGraphTexture.
        const MaterialTexVk* ovr = nullptr;
        if (resolveMaterialOverride(assetId, ovr) && ovr) tex = ovr->view;
        center    = glm::vec3(0.0f);
        extent    = 1.0f;
        baseColor = ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2])
                       : glm::vec3(0.8f);
        metallic  = ma ? ma->metallic  : 0.0f;
        roughness = ma ? ma->roughness : 0.5f;
        // Wider FOV than GL's graph path, hence the shorter distance — both land on
        // the same apparent sphere size (see the constants in PreviewFraming.h).
        dist      = HE::kMatFallbackDist;
    }
    else if (kind == ThumbnailKind::SkeletalMesh)
    {
        // BIND POSE, exactly like GL (:8433): the stored vertex positions ARE the
        // bind pose, so the plain mesh-preview pipeline is enough — no bone
        // matrices, no skinning shader. The skeletal mesh's slot-0 buffer is the
        // same interleaved pos/normal/uv, 32-byte vertex the mesh pipeline's
        // vertex input declares; its bone-ID / bone-weight buffers simply go
        // unbound, which is legal because this pipeline declares no binding for
        // them. Unblocked by GpuSkeletalMesh::localBounds (see resolveSkeletalMesh).
        const GpuSkeletalMesh* smesh = resolveSkeletalMesh(assetId);
        if (!smesh || smesh->indexCount <= 0) return false;
        vb = smesh->vb; ib = smesh->ib; indexCount = static_cast<uint32_t>(smesh->indexCount);
        tex        = smesh->texView;
        center     = HE::boundsCenter(smesh->localBounds);
        extent     = HE::boundsExtent(smesh->localBounds);
    }
    else
    {
        return false;
    }

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return false;

    // Clear colour (0,0,0,0): the Content Browser composites the tile over a
    // checkerboard, so an opaque background is a visible regression. Depth 1.0.
    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass      = m_thumbRenderPass;
    rpbi.framebuffer     = m_thumbFB;
    rpbi.renderArea      = { { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    rpbi.clearValueCount = 2;
    rpbi.pClearValues    = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    drawMeshPreviewGeometry(cmd, S, vb, ib, indexCount, tex, center, extent,
                            baseColor, metallic, roughness,
                            HE::kThumbYaw, HE::kThumbPitch, dist);
    vkCmdEndRenderPass(cmd);
    // The render pass ends in TRANSFER_SRC_OPTIMAL, so the copy needs no barrier.
    recordThumbnailReadback(cmd, S, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    endThumbnailCmd(cmd);

    return mapThumbnailReadback(S, outRgba8);
}

// ─── UI widget thumbnails ───────────────────────────────────────────────────
bool VulkanRenderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                           uint32_t size, std::vector<uint8_t>& outRgba8)
{
    const int S = HE::clampThumbnailSize(size);
    if (uiObjects.empty() || !m_device) return false;
    if (!m_uiViewportPipeline || !m_uiViewportRP) return false; // createUIPipeline failed
    if (!ensureThumbnailTarget(S) || !m_thumbUIFB) return false;

    // Warm the default font atlas OUTSIDE our recording: uiFontAtlasSet() uploads
    // through its own one-shot command buffer, which is fine mid-recording but
    // cleaner here — runUIPass's own call then hits the cache.
    if (!uiFontAtlasSet(0)) return false;

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return false;

    // m_uiViewportRP is LOAD_OP_LOAD (it exists to composite UI onto a finished
    // frame), so the transparent clear has to be an explicit image clear. Reusing
    // that render pass rather than building a private one is what lets the tile
    // reuse m_uiViewportPipeline instead of duplicating the whole UI pipeline —
    // the formats match (both R8G8B8A8_UNORM, colour-only), which is all Vulkan
    // render-pass compatibility requires.
    clearThumbnailColor(cmd);

    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass  = m_uiViewportRP;
    rpbi.framebuffer = m_thumbUIFB;
    rpbi.renderArea  = { { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(S), static_cast<float>(S), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor (cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiViewportPipeline);
    // The overload, NOT GL's swap into m_renderWorld.uiObjects — see runUIPass.
    runUIPass(cmd, S, S, uiObjects);
    vkCmdEndRenderPass(cmd);

    recordThumbnailReadback(cmd, S, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    endThumbnailCmd(cmd);

    return mapThumbnailReadback(S, outRgba8);
}

// ─── Particle-system thumbnails ─────────────────────────────────────────────
bool VulkanRenderer::RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                             const std::vector<ParticlePreviewInstance>& particles,
                                             uint32_t size, std::vector<uint8_t>& outRgba8)
{
    const int S = HE::clampThumbnailSize(size);
    if (!m_contentManager) m_contentManager = &cm;
    if (particles.empty() || !m_device) return false;
    if (!ensureThumbnailTarget(S)) return false;
    if (!ensureParticlePreviewPipeline()) return false;

    // The material's base texture, or null → the shader's soft circular sprite.
    // resolveMaterialOverride returns false only while the asset is still loading,
    // and it may upload — so it runs before any recording starts.
    VkImageView tex = VK_NULL_HANDLE;
    const MaterialTexVk* ovr = nullptr;
    if (resolveMaterialOverride(materialId, ovr) && ovr) tex = ovr->view;

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return false;

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass      = m_thumbRenderPass;
    rpbi.framebuffer     = m_thumbFB;
    rpbi.renderArea      = { { 0, 0 }, { static_cast<uint32_t>(S), static_cast<uint32_t>(S) } };
    rpbi.clearValueCount = 2;
    rpbi.pClearValues    = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    // Same fixed three-quarter framing as the mesh tiles, so a grid of assets
    // reads as one set.
    drawParticlePreviewGeometry(cmd, S, tex, particles,
                                HE::kThumbYaw, HE::kThumbPitch, HE::kParticleDist);
    vkCmdEndRenderPass(cmd);
    recordThumbnailReadback(cmd, S, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    endThumbnailCmd(cmd);

    return mapThumbnailReadback(S, outRgba8);
}

// ─────────────────────────────────────────────────────────────────────────────
// P1c — the three interactive asset previews
//
// Port of OpenGLRenderer::RenderMaterialPreview (:8457) / RenderSkeletalPreview
// (:8581) / RenderParticlePreview (:9173). Everything the thumbnails established
// carries over — private resources, one-shot command buffer + vkQueueWaitIdle,
// nothing indexed by m_currentFrame, no createViewportResources() — and two
// things are new: a PERSISTENT ImGui handle per target (registered once, because
// ImGui's Vulkan descriptor supply is a fixed 64 with no free path) and a size
// BUCKET (because the panels pass ImGui::GetContentRegionAvail(), which moves by
// a pixel as the user drags).
//
// THREE TARGETS, not one, and not the thumbnails': a tile or a second preview
// rendered into a shared image would replace whatever an open editor panel is
// currently showing. OpenGLRenderer.h:326 spells the same reasoning out.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
// set 0, binding 0 of the skeletal preview fragment shader (80 B). Deliberately
// NOT MeshPreviewUBO: GL's kSkelPreviewFS is a DIFFERENT shader from
// kMeshPreviewFS (ambient 0.35 vs 0.32, 0.65·diffuse vs 0.68, and no specular
// term at all), so sharing the block would quietly change the shading.
struct SkelPreviewUBO
{
    glm::vec4 color;    // rgb = base colour
    glm::vec4 pbr;      // x = hasTexture (0/1)
    glm::vec4 sun;      // w = 0 → the shader's fixed studio light (GL parity)
    glm::vec4 sunColor;
    glm::vec4 ambient;
};
static_assert(sizeof(SkelPreviewUBO) == 80, "must match SkelPreviewCB");

// std140 `U` block of the shared material vertex/fragment preamble — the same
// 176-byte layout DrawScene fills per graph-material draw.
struct MatPreviewUBlock
{
    glm::mat4 mvp;
    glm::mat4 model;
    glm::vec4 color;
    glm::vec4 flags;
    glm::vec4 pbr;
};
static_assert(sizeof(MatPreviewUBlock) == 176, "U block must be std140 176 B");

#if defined(HE_HAVE_SHADERC)
// ─── Skeletal preview shaders (RenderSkeletalPreview) ────────────────────────
// Port of OpenGLRenderer's kSkelPreviewVS/kSkelPreviewFS (:632/:657).
//
// WHY RUNTIME-COMPILED AND NOT shaders/skel_preview.{vert,frag}: adding a .glsl
// file means adding it to CMakeLists' glslc list, and CMakeLists is off-limits —
// an unregistered shader is simply never compiled to .spv, so the pipeline would
// be dead on every machine. This is the same route the particle tile takes, and
// he::shaderc::compile() is the same glslang the graph materials already go
// through on this backend.
//
// WHY NOT skinned.vert.spv + mesh_preview.frag.spv, both of which already exist:
// their interfaces do not line up. skinned.vert writes vWorldPos/vNormal/vUV at
// locations 0/1/2 while mesh_preview.frag reads vNormal/vUV/vWorldPos at 0/1/2,
// so location 1 would feed a vec3 normal into a vec2 UV. Vulkan matches purely
// by location, so that pairing is not a mismatch it would reject — it is a
// mismatch that draws garbage. Writing both strings removes the trap.
constexpr const char* kSkelPreviewVertGlsl = R"GLSL(
#version 450

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in uvec4 aBoneIds;
layout(location = 4) in vec4  aBoneWgts;

// Per-object transforms via push constants, mirroring scene.vert/skinned.vert.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    mat4 uModel;
} pc;

layout(set = 0, binding = 2) uniform BonesCB {
    mat4 uBoneMatrices[128];
};

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;

void main()
{
    mat4 skin = aBoneWgts.x * uBoneMatrices[aBoneIds.x]
              + aBoneWgts.y * uBoneMatrices[aBoneIds.y]
              + aBoneWgts.z * uBoneMatrices[aBoneIds.z]
              + aBoneWgts.w * uBoneMatrices[aBoneIds.w];
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vNormal     = mat3(pc.uModel) * mat3(skin) * aNormal;
    vUV         = aUV;
    gl_Position = pc.uMVP * skinnedPos;
}
)GLSL";

constexpr const char* kSkelPreviewFragGlsl = R"GLSL(
#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform SkelPreviewCB {
    vec4 uColor;    // rgb = base colour
    vec4 uPbr;      // x = hasTexture (0/1)
    vec4 uSun;      // xyz points TOWARD the light, w > 0 arms it
    vec4 uSunColor;
    vec4 uAmbient;
};
layout(set = 0, binding = 1) uniform sampler2D uTex;

void main()
{
    // Same convention as the mesh preview: w == 0 means the fixed studio light.
    bool lit = uSun.w > 0.0;
    vec3 L   = lit ? normalize(uSun.xyz) : normalize(vec3(0.45, 0.75, 0.55));
    vec3 lc  = lit ? uSunColor.rgb : vec3(1.0);
    vec3 amb = lit ? uAmbient.rgb  : vec3(0.35);
    vec3 N = normalize(vNormal);
    float diff = max(dot(N, L), 0.0);
    vec3 albedo = (uPbr.x > 0.5) ? texture(uTex, vUV).rgb * uColor.rgb : uColor.rgb;
    FragColor = vec4(albedo * (amb + lc * (lit ? diff : 0.65 * diff)), 1.0);
}
)GLSL";
#endif // HE_HAVE_SHADERC
} // namespace

// ─── Small host-visible buffer helper ───────────────────────────────────────
bool VulkanRenderer::makeHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out)
{
    destroyHostBuffer(out);
    if (!m_device || size == 0) return false;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = usage;
    if (vkCreateBuffer(m_device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, out.buf, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, out.buf, out.mem, 0);
    return vkMapMemory(m_device, out.mem, 0, size, 0, &out.mapped) == VK_SUCCESS;
}

void VulkanRenderer::destroyHostBuffer(HostBuffer& b)
{
    if (!m_device) { b = {}; return; }
    // Unmap BEFORE destroying — a mapped allocation freed with the pointer still
    // live is exactly the shape of leak the validation layer reports at
    // vkDestroyDevice, and that report is currently at zero.
    if (b.mapped) { vkUnmapMemory(m_device, b.mem); b.mapped = nullptr; }
    if (b.buf)    { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem)    { vkFreeMemory   (m_device, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
}

// ─── Preview sampler ────────────────────────────────────────────────────────
bool VulkanRenderer::ensurePreviewSampler()
{
    if (m_previewSampler) return true;
    if (!m_device) return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    // CLAMP_TO_EDGE, not m_albedoSampler's REPEAT: ImGui draws the preview as one
    // quad over UV 0..1 and the edge texels would otherwise filter against the
    // opposite edge of the image — a thin wrong-coloured border on all four sides.
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(m_device, &sci, nullptr, &m_previewSampler) == VK_SUCCESS;
}

// ─── Preview target ─────────────────────────────────────────────────────────
// `w`/`h` arrive already CLAMPED and BUCKETED by the caller.
bool VulkanRenderer::ensurePreviewTarget(PreviewTarget& t, int w, int h)
{
    if (t.fb && t.w == w && t.h == h) return true;
    // A rebuild costs one ImGui descriptor permanently: the registrar wraps
    // ImGui_ImplVulkan_AddTexture, whose pool is a fixed 64 and which
    // DestroyImGuiTexture cannot give back. That is the whole reason the caller
    // buckets — with a bucket, a pane being dragged crosses a 64-pixel boundary a
    // handful of times instead of every frame.
    destroyPreviewTarget(t);
    if (!m_device || w <= 0 || h <= 0) return false;
    if (!ensurePreviewRenderPass() || !ensurePreviewSampler()) return false;

    const uint32_t W = static_cast<uint32_t>(w), H = static_cast<uint32_t>(h);

    // Colour: R8G8B8A8_UNORM, the same format the thumbnail target and the shared
    // render pass declare (so the P1b pipelines bind here unchanged), and already
    // the byte order the PPM writer wants — no R↔B swap, unlike a BGRA target.
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent        = { W, H, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED for ImGui, TRANSFER_SRC for the HE_*_PREVIEW_DUMP copy.
        ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT
                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &t.color) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, t.color, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &t.colorMem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, t.color, t.colorMem, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = t.color;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &t.colorView) != VK_SUCCESS) return false;
    }
    // Depth.
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = m_depthFormat;
        ici.extent        = { W, H, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &t.depth) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, t.depth, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &t.depthMem) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, t.depth, t.depthMem, 0);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = t.depth;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = m_depthFormat;
        vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &t.depthView) != VK_SUCCESS) return false;
    }
    {
        VkImageView atts[2] = { t.colorView, t.depthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_thumbRenderPass;
        fci.attachmentCount = 2;
        fci.pAttachments    = atts;
        fci.width           = W;
        fci.height          = H;
        fci.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &t.fb) != VK_SUCCESS) return false;
    }
    t.w = w;
    t.h = h;
    // ONCE, here. The image is still in UNDEFINED layout at this point, which is
    // fine: the registrar only builds a descriptor over the view, and ImGui does
    // not sample it until after a preview has drawn and endPreviewPass has left
    // it in SHADER_READ_ONLY_OPTIMAL — the layout AddTexture was told to expect.
    // A null registrar (no editor / no ImGui) is not a failure: the render and
    // the PPM dump still happen, the entry point just returns nullptr.
    t.imguiTex = m_imguiTexRegistrar
        ? m_imguiTexRegistrar(reinterpret_cast<void*>(t.colorView),
                              reinterpret_cast<void*>(m_previewSampler))
        : nullptr;
    return true;
}

void VulkanRenderer::destroyPreviewTarget(PreviewTarget& t)
{
    if (!m_device) { t = PreviewTarget{}; return; }
    if (t.fb)        { vkDestroyFramebuffer(m_device, t.fb,        nullptr); t.fb        = VK_NULL_HANDLE; }
    if (t.depthView) { vkDestroyImageView  (m_device, t.depthView, nullptr); t.depthView = VK_NULL_HANDLE; }
    if (t.depth)     { vkDestroyImage      (m_device, t.depth,     nullptr); t.depth     = VK_NULL_HANDLE; }
    if (t.depthMem)  { vkFreeMemory        (m_device, t.depthMem,  nullptr); t.depthMem  = VK_NULL_HANDLE; }
    if (t.colorView) { vkDestroyImageView  (m_device, t.colorView, nullptr); t.colorView = VK_NULL_HANDLE; }
    if (t.color)     { vkDestroyImage      (m_device, t.color,     nullptr); t.color     = VK_NULL_HANDLE; }
    if (t.colorMem)  { vkFreeMemory        (m_device, t.colorMem,  nullptr); t.colorMem  = VK_NULL_HANDLE; }
    if (t.stageBuf)  { vkDestroyBuffer     (m_device, t.stageBuf,  nullptr); t.stageBuf  = VK_NULL_HANDLE; }
    if (t.stageMem)  { vkFreeMemory        (m_device, t.stageMem,  nullptr); t.stageMem  = VK_NULL_HANDLE; }
    // The ImGui descriptor built over the retired view is NOT reclaimable — the
    // registrar has no inverse and DestroyImGuiTexture is a no-op on this
    // backend. Dropping the pointer is all we can do; the bucket is what keeps
    // that from mattering.
    t.imguiTex = nullptr;
    t.w = t.h = 0;
}

void VulkanRenderer::beginPreviewPass(VkCommandBuffer cmd, const PreviewTarget& t)
{
    // Clear colour (0,0,0,0): the editor composites the preview over its own
    // backdrop, so an opaque background is a visible regression. Depth 1.0, the
    // pass's pipelines all compare LESS.
    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass      = m_thumbRenderPass;
    rpbi.framebuffer     = t.fb;
    rpbi.renderArea      = { { 0, 0 }, { static_cast<uint32_t>(t.w), static_cast<uint32_t>(t.h) } };
    rpbi.clearValueCount = 2;
    rpbi.pClearValues    = clears;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    // The FULL bucketed extent, never the requested one: ImGui::Image maps UV
    // 0..1 across the whole texture, so a narrower viewport would show the
    // content shrunk into a corner with a cleared strip beside it. The requested
    // size enters through the projection's ASPECT instead (see the entry points).
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(t.w), static_cast<float>(t.h), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { static_cast<uint32_t>(t.w), static_cast<uint32_t>(t.h) } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor (cmd, 0, 1, &sc);
}

const char* VulkanRenderer::previewDumpPath(const char* envVar)
{
    const char* p = std::getenv(envVar);
    return (p && *p) ? p : nullptr;
}

bool VulkanRenderer::recordPreviewDump(VkCommandBuffer cmd, PreviewTarget& t)
{
    if (!t.color || t.w <= 0 || t.h <= 0) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(t.w) * t.h * 4;
    if (!t.stageBuf)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &t.stageBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, t.stageBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &t.stageMem) != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, t.stageBuf, nullptr); t.stageBuf = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(m_device, t.stageBuf, t.stageMem, 0);
    }
    // No barrier: the render pass's subpass→EXTERNAL dependency already makes the
    // colour writes available to TRANSFER_READ, and finalLayout is TRANSFER_SRC.
    // bufferRowLength = 0 packs rows at exactly imageExtent.width texels, so there
    // is no row pitch to de-stripe on the way out.
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { static_cast<uint32_t>(t.w), static_cast<uint32_t>(t.h), 1 };
    vkCmdCopyImageToBuffer(cmd, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t.stageBuf, 1, &region);
    return true;
}

void VulkanRenderer::writePreviewDump(const PreviewTarget& t, const char* path)
{
    if (!t.stageMem || !path) return;
    const size_t px = static_cast<size_t>(t.w) * t.h;
    void* mapped = nullptr;
    if (vkMapMemory(m_device, t.stageMem, 0, px * 4, 0, &mapped) != VK_SUCCESS) return;
    const auto* src = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgb(px * 3);
    for (size_t i = 0; i < px; ++i)
    {
        rgb[i * 3 + 0] = src[i * 4 + 0];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + 2];
    }
    vkUnmapMemory(m_device, t.stageMem);
    if (std::ofstream f(path, std::ios::binary); f)
    {
        f << "P6\n" << t.w << " " << t.h << "\n255\n";
        // TOP-DOWN, straight through. GL writes the same bytes only AFTER its
        // `H-1-y` loop, which undoes GL's bottom-up framebuffer; Vulkan's row 0
        // already IS the top row because kVulkanClipFix flipped clip-space Y.
        f.write(reinterpret_cast<const char*>(rgb.data()),
                static_cast<std::streamsize>(rgb.size()));
    }
}

const char* VulkanRenderer::endPreviewPass(VkCommandBuffer cmd, PreviewTarget& t,
                                           const char* dumpEnv)
{
    vkCmdEndRenderPass(cmd);
    // The dump copy has to be recorded HERE: the pass's finalLayout leaves the
    // image in TRANSFER_SRC_OPTIMAL and the barrier below is about to move it out.
    const char* dumpPath = dumpEnv ? previewDumpPath(dumpEnv) : nullptr;
    if (dumpPath && !recordPreviewDump(cmd, t)) dumpPath = nullptr;

    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // the pass's finalLayout
    b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // what ImGui expects
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = t.color;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    // Source scope covers BOTH producers: the render pass's colour writes and, when
    // a dump ran, the transfer read above. Everything here ends in vkQueueWaitIdle,
    // so a slightly wide scope is free — a narrow WRONG one is what validation
    // flags, and this backend's validation output is currently at zero findings.
    b.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    return dumpPath;
}

// ─── Node-graph project textures (heTexP0..3) ───────────────────────────────
// Same key convention as OpenGLRenderer::ResolveGraphTexture (:8018) — the baked
// UUID as "hi:lo", else the loose path — so the two caches can be merged
// mechanically later. A miss is cached too (a null view), exactly like GL, so a
// texture that will never load is not retried once per frame.
VkImageView VulkanRenderer::resolveGraphTexture(const HE::UUID& id, const std::string& path)
{
    const std::string key = id != HE::UUID{}
        ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path;
    if (key.empty() || !m_contentManager) return VK_NULL_HANDLE;
    if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end()) return it->second.view;
    GraphTexVk gt;
    // uploadTextureImage submits and vkQueueWaitIdle's, which is why every caller
    // resolves BEFORE it opens a command buffer.
    if (!uploadTextureImage(m_contentManager->resolveTextureRef(id, path),
                            gt.image, gt.mem, gt.view))
        gt = GraphTexVk{};
    m_graphTexCache.emplace(key, gt);
    return gt.view;
}

// ─── Material-preview resources ─────────────────────────────────────────────
bool VulkanRenderer::ensureMaterialPreviewResources()
{
#if defined(HE_HAVE_SHADERC)
    if (m_matPvReady) return true;
    if (!m_device || !m_matReady || m_matSetLayout == VK_NULL_HANDLE) return false;
    if (!thumbEnsureDescPool(m_device, m_thumbDescPool)) return false;

    if (!makeHostBuffer(sizeof(HE::MaterialShaderLibrary::Lighting),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_matPvLightBuf)) return false;
    if (!makeHostBuffer(sizeof(MatPreviewUBlock),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_matPvObjBuf))   return false;
    if (!makeHostBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_matPvParBuf)) return false;

    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = m_thumbDescPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_matSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_matPvSet) != VK_SUCCESS) return false;

    m_matPvReady = true;
    return true;
#else
    return false;
#endif
}

// ─── RenderMaterialPreview ──────────────────────────────────────────────────
// The REAL graph-material shader, exactly like OpenGL — this backend has no
// sampler-register ceiling to hit (the D3D11/D3D12 twins fall back to a built-in
// PBR sphere because ps_5_0 caps at 16 samplers and the shared material preamble
// binds 16/17/18/31/32/33). That makes this the closest of the four to GL's
// picture and the most useful HE_PREVIEW_DUMP to diff against it.
void* VulkanRenderer::RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
                                            uint32_t size, float yaw, float pitch, float dist,
                                            int shape, const HE::UUID& meshId)
{
#if defined(HE_HAVE_SHADERC)
    // Adopt the ContentManager: previews run BEFORE the first Render(), so
    // SetContentManager may not have happened and resolveShaders would find nothing.
    if (!m_contentManager) m_contentManager = &cm;
    if (!m_device) return nullptr;

    // PROBE FIRST, before allocating anything — a built-in-PBR material has no
    // node-graph program and gets no preview at all on GL either (:8464-8470).
    // The Content-Browser TILE is the path that falls back to a shaded sphere.
    uint64_t shKey = 0; std::string shFrag, shVert;
    if (!m_matReady || !m_matShaderLib.resolveShaders(*m_contentManager, materialId,
                                                      shKey, shFrag, shVert))
        return nullptr;

    const int req = HE::clampPreviewSize(size);   // 32..1024, shared with every backend
    const int S   = previewBucket(req);
    if (!ensurePreviewTarget(m_matPreview, S, S)) return nullptr;
    if (!ensureMaterialPreviewResources())        return nullptr;

    // The graph pipeline built against OUR render pass, a cache variant of its own
    // (see GetOrBuildMaterialPipeline): the LDR variant targets m_renderPass, whose
    // colour format differs, so it is not compatible with this framebuffer.
    // transparent = false mirrors GL, which previews with blending disabled.
    VkPipeline pipe = GetOrBuildMaterialPipeline(shKey, shFrag, shVert, /*hdr=*/false,
                                                 /*transparent=*/false, m_thumbRenderPass);
    if (pipe == VK_NULL_HANDLE) return nullptr;

    // ── Geometry: a picked STATIC MESH, else the procedural primitive. Both feed
    // the same interleaved pos3/normal3/uv2 stream the material vertex declares,
    // so only the camera has to grow to the mesh's bounds. Resolved BEFORE
    // recording — resolveMesh uploads through its own submit + queue wait.
    VkBuffer  vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    uint32_t  idxCount = 0;
    glm::vec3 center(0.0f);
    float     radius = 1.0f; // what `dist` is measured in, so it frames alike
    const GpuMesh* gm = meshId != HE::UUID{} ? resolveMesh(meshId) : nullptr;
    if (gm && gm->vbuf && gm->indexCount > 0)
    {
        vb = gm->vbuf; ib = gm->ibuf; idxCount = gm->indexCount;
        if (gm->localBounds.isValid())
        {
            center = HE::boundsCenter(gm->localBounds);
            radius = std::max(HE::boundsExtent(gm->localBounds), 1e-4f);
        }
    }
    else
    {
        if (!ensurePreviewMesh(shape)) return nullptr;
        vb = m_previewSphereVB; ib = m_previewSphereIB; idxCount = m_previewSphereIdx;
    }
    if (!vb || !ib || idxCount == 0) return nullptr;

    const MaterialAsset* ma = m_contentManager->getMaterial(materialId);

    // ── heTexP0..3: the graph's picked project textures, resolved BEFORE the
    // command buffer opens (each may upload synchronously). This is the ONE place
    // on this backend that binds them for real — DrawScene still binds the white
    // default at bindings 4-7, because an upload inside its recording would be a
    // hang rather than a hitch. Without them a textured graph material previews
    // flat white here and the OpenGL A/B is meaningless.
    VkImageView texP[4] = { m_whiteAlbedoView, m_whiteAlbedoView,
                            m_whiteAlbedoView, m_whiteAlbedoView };
    if (ma)
    {
        const size_t nTex = std::min<size_t>(4, std::max(ma->graphTexturePaths.size(),
                                                         ma->graphTextureIds.size()));
        for (size_t i = 0; i < nTex; ++i)
        {
            const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
            const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
            if (VkImageView v = resolveGraphTexture(gid, gp)) texP[i] = v;
        }
    }

    // ── Camera. 32° here, NOT kPreviewFovDegrees: GL's material preview uses 32
    // (OpenGLRenderer.cpp:8176) while the mesh/skeletal/particle paths use 35, and
    // kMatGraphDist was tuned against the narrower one. HE::meshOrbit builds the
    // matrix so glm::perspective is never called on this target, where
    // GLM_FORCE_DEPTH_ZERO_TO_ONE is a private compile definition that would remap
    // depth a second time (PreviewFraming.h's header note). GL's near/far differ
    // (0.02·radius / (dist+8)·radius+1 vs meshOrbit's 0.01 / camDist·20+10) — that
    // is depth PRECISION only; neither clips the subject, so the picture matches.
    const HE::PreviewCamera cam = HE::meshOrbit(center, radius, yaw, pitch, dist,
                                                1.0f /*square*/, 32.0f);
    const glm::mat4 model(1.0f);

    // ── U block (per object).
    MatPreviewUBlock ub{};
    ub.mvp   = HE::kVulkanClipFix * cam.proj * cam.view * model; // exactly ONE fix-up
    ub.model = model;
    ub.color = glm::vec4(ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2])
                            : glm::vec3(1.0f), 1.0f);
    ub.flags = glm::vec4(0.0f); // hasTexture = 0, like GL's preview
    ub.pbr   = glm::vec4(ma ? ma->metallic : 0.0f, ma ? ma->roughness : 0.5f,
                         ma ? ma->opacity  : 1.0f, 0.0f);
    std::memcpy(m_matPvObjBuf.mapped, &ub, sizeof(ub));

    // ── HeLighting: the preview's own studio sun, ported field for field from
    // OpenGLRenderer.cpp:8188-8202. sunDir.w (the graph's Time input) stays 0
    // rather than taking the engine clock — a preview must be deterministic, and
    // GL's fill leaves it at 0 for the same reason. giParams.z and fog.z/w stay 0,
    // which is what tells heLitP() that no GI mask, sky cubemap or AO is bound.
    HE::MaterialShaderLibrary::Lighting lit{};
    const glm::vec3 sd = glm::normalize(glm::vec3(0.45f, 0.75f, 0.55f));
    lit.sunDir[0] = sd.x; lit.sunDir[1] = sd.y; lit.sunDir[2] = sd.z; lit.sunDir[3] = 0.0f;
    lit.sunColor[0] = lit.sunColor[1] = lit.sunColor[2] = 1.05f;
    lit.ambient[0]  = lit.ambient[1]  = lit.ambient[2]  = 0.28f;
    lit.camPos[0] = cam.position.x; lit.camPos[1] = cam.position.y; lit.camPos[2] = cam.position.z;
    // The studio sun as the single array light, so heLitP() previews shade too.
    lit.lightPos[0][3]   = 0.0f; // directional
    lit.lightDir[0][0]   = -sd.x; lit.lightDir[0][1] = -sd.y; lit.lightDir[0][2] = -sd.z;
    lit.lightColor[0][0] = lit.lightColor[0][1] = lit.lightColor[0][2] = 1.05f;
    lit.lightColor[0][3] = 1.0f;
    lit.counts[0]        = 1.0f;
    std::memcpy(m_matPvLightBuf.mapped, &lit, sizeof(lit));

    // ── HeParams: the material's shader params, zero-padded to 16 vec4.
    {
        float padded[64] = { 0.0f };
        if (ma && !ma->shaderParamData.empty())
            std::memcpy(padded, ma->shaderParamData.data(),
                        std::min(ma->shaderParamData.size(), size_t(64)) * sizeof(float));
        std::memcpy(m_matPvParBuf.mapped, padded, sizeof(padded));
    }

    // ── Descriptor set: the same 13 bindings DrawScene writes per graph draw.
    // Rewritten in place, which is safe because the previous preview ended in
    // vkQueueWaitIdle — no submission still references this set.
    {
        VkDescriptorBufferInfo lightBI{ m_matPvLightBuf.buf, 0,
                                        sizeof(HE::MaterialShaderLibrary::Lighting) };
        VkDescriptorBufferInfo objBI  { m_matPvObjBuf.buf,   0, sizeof(MatPreviewUBlock) };
        VkDescriptorBufferInfo parBI  { m_matPvParBuf.buf,   0, 256 };
        VkDescriptorImageInfo  whiteII{ m_albedoSampler, m_whiteAlbedoView,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  pII[4];
        for (int i = 0; i < 4; ++i)
            pII[i] = { m_albedoSampler, texP[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // heCsm is a sampler2DArray in the SPIR-V, so its default MUST be the
        // 1-layer array view — a plain 2D view fails validation against it.
        VkDescriptorImageInfo  csmII { m_albedoSampler, m_whiteArrayView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[13]{};
        auto wr = [&](int idx, uint32_t binding, VkDescriptorType type,
                      const VkDescriptorBufferInfo* bi, const VkDescriptorImageInfo* ii) {
            w[idx].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[idx].dstSet          = m_matPvSet;
            w[idx].dstBinding      = binding;
            w[idx].descriptorCount = 1;
            w[idx].descriptorType  = type;
            w[idx].pBufferInfo     = bi;
            w[idx].pImageInfo      = ii;
        };
        wr(0,  0,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &lightBI, nullptr);
        wr(1,  1,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &objBI,   nullptr);
        wr(2,  2,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII); // heTex0 (flags.x = 0)
        wr(3,  3,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &parBI,   nullptr);
        wr(4,  4,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &pII[0]);
        wr(5,  5,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &pII[1]);
        wr(6,  6,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &pII[2]);
        wr(7,  7,  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &pII[3]);
        wr(8,  8,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &lightBI, nullptr); // WPO VS
        wr(9,  9,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &parBI,   nullptr); // WPO VS
        wr(10, 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII); // heGIShadow
        wr(11, 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &whiteII); // heGILocal
        wr(12, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &csmII);   // heCsm
        vkUpdateDescriptorSets(m_device, 13, w, 0, nullptr);
    }

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return nullptr;
    beginPreviewPass(cmd, m_matPreview);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_matPipelineLayout,
                            0, 1, &m_matPvSet, 0, nullptr);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, idxCount, 1, 0, 0, 0);
    const char* dumpPath = endPreviewPass(cmd, m_matPreview, "HE_PREVIEW_DUMP");
    endThumbnailCmd(cmd);
    if (dumpPath) writePreviewDump(m_matPreview, dumpPath);

    return m_matPreview.imguiTex;
#else
    (void)cm; (void)materialId; (void)size; (void)yaw; (void)pitch;
    (void)dist; (void)shape; (void)meshId;
    return nullptr; // no cross-compiler → no graph material → nothing to preview
#endif
}

// ─── Skeletal-preview pipeline ──────────────────────────────────────────────
bool VulkanRenderer::ensureSkelPreviewPipeline()
{
#if defined(HE_HAVE_SHADERC)
    if (m_skelPvPipe && m_skelPvSet) return true;
    if (m_skelPvTried) return false;   // one attempt per session, like the GI pipelines
    m_skelPvTried = true;
    if (!m_device || !ensurePreviewRenderPass()) return false;
    if (!m_albedoSampler || !m_whiteAlbedoView) return false; // see ensureMeshPreviewPipeline

    const he::shaderc::Result vr = he::shaderc::compile(
        kSkelPreviewVertGlsl, he::shaderc::Stage::Vertex, he::shaderc::Target::SpirvBinary);
    const he::shaderc::Result fr = he::shaderc::compile(
        kSkelPreviewFragGlsl, he::shaderc::Stage::Fragment, he::shaderc::Target::SpirvBinary);
    if (!vr.ok || !fr.ok || vr.spirv.empty() || fr.spirv.empty())
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: skeletal-preview shader compile failed");
        return false;
    }
    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule vs = makeModule(vr.spirv);
    VkShaderModule fs = makeModule(fr.spirv);
    struct ModGuard {
        VkDevice d; VkShaderModule a, b;
        ~ModGuard() { if (a) vkDestroyShaderModule(d, a, nullptr); if (b) vkDestroyShaderModule(d, b, nullptr); }
    } guard{ m_device, vs, fs };
    if (!vs || !fs) return false;

    // set 0: b0 = SkelPreviewCB (FS), b1 = uTex (FS), b2 = BonesCB (VS).
    // A plain UBO, not the scene's UNIFORM_BUFFER_DYNAMIC: exactly one pose is
    // drawn per call here, so there is nothing for a dynamic offset to select.
    {
        VkDescriptorSetLayoutBinding binds[3]{};
        binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2].binding = 2; binds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[2].descriptorCount = 1; binds[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = 3;
        dslci.pBindings    = binds;
        if (vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_skelPvDSL) != VK_SUCCESS)
            return false;
    }
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(MeshPreviewPush) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_skelPvDSL;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_skelPvLayout) != VK_SUCCESS) return false;

    // 128 mat4 = 8192 B, inside the 16384-byte maxUniformBufferRange floor.
    if (!makeHostBuffer(sizeof(SkelPreviewUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_skelPvUBO))
        return false;
    if (!makeHostBuffer(128 * sizeof(glm::mat4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_skelPvBones))
        return false;

    if (!thumbEnsureDescPool(m_device, m_thumbDescPool)) return false;
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_thumbDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_skelPvDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_skelPvSet) != VK_SUCCESS) return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    // The three streams GpuSkeletalMesh uploads, identical to createSkinnedPipeline.
    VkVertexInputBindingDescription bindings[3] = {
        { 0, 32u,                   VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 4u * sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX },
        { 2, 4u * sizeof(float),    VK_VERTEX_INPUT_RATE_VERTEX },
    };
    VkVertexInputAttributeDescription attrs[5] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 },
        { 3, 1, VK_FORMAT_R32G32B32A32_UINT,   0  },
        { 4, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0  },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 3;
    vi.pVertexBindingDescriptions      = bindings;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;  // GL: glDisable(GL_CULL_FACE)
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS; // GL: glDepthFunc(GL_LESS)
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable    = VK_FALSE;            // GL: glDisable(GL_BLEND)
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
    pci.layout              = m_skelPvLayout;
    pci.renderPass          = m_thumbRenderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skelPvPipe) != VK_SUCCESS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: skeletal-preview pipeline creation failed");
        return false;
    }
    return true;
#else
    return false;
#endif
}

// ─── Bone-overlay line pipeline ─────────────────────────────────────────────
// Reuses the PRECOMPILED debug_line shaders and m_debugPipelineLayout — no new
// shader file, which matters because CMakeLists is where a .glsl becomes a .spv
// and it is off-limits here. Only the pipeline (this render pass) and the buffers
// are new; the per-frame m_debugUBO[]/m_debugVB[] rings belong to the frame loop
// and are never touched from here.
bool VulkanRenderer::ensureSkelLinePipeline()
{
    if (m_skelLinePipe && m_skelLineSet) return true;
    if (m_skelLineTried) return false;
    m_skelLineTried = true;
    if (!m_device || !ensurePreviewRenderPass()) return false;
    if (!m_debugDSLayout || !m_debugPipelineLayout) return false; // debug pipeline never built

    VkShaderModule vs = loadShaderModule("debug_line.vert.spv");
    VkShaderModule fs = loadShaderModule("debug_line.frag.spv");
    struct ModGuard {
        VkDevice d; VkShaderModule a, b;
        ~ModGuard() { if (a) vkDestroyShaderModule(d, a, nullptr); if (b) vkDestroyShaderModule(d, b, nullptr); }
    } guard{ m_device, vs, fs };
    if (!vs || !fs) return false;

    if (!makeHostBuffer(sizeof(glm::mat4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, m_skelLineUBO))
        return false;
    if (!thumbEnsureDescPool(m_device, m_thumbDescPool)) return false;
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_thumbDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_debugDSLayout;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_skelLineSet) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo bi{ m_skelLineUBO.buf, 0, sizeof(glm::mat4) };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet          = m_skelLineSet;
        w.dstBinding      = 0;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription vbd{ 0, 6u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3u * sizeof(float) },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbd;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 1.0, not GL's glLineWidth(2.0): createDevice sets pEnabledFeatures to
    // nothing, so the `wideLines` feature is off and any value != 1.0 would fail
    // pipeline creation outright. Mirrors createDebugLinePipeline, which made the
    // same call. The overlay is therefore a pixel thinner here than on OpenGL.
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;   // the overlay is occluded by the mesh, like GL's
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable    = VK_FALSE;
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
    pci.layout              = m_debugPipelineLayout;
    pci.renderPass          = m_thumbRenderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skelLinePipe) != VK_SUCCESS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: skeletal bone-overlay pipeline creation failed");
        return false;
    }
    return true;
}

// Port of OpenGLRenderer.cpp:8665-8714: a tiny 3-axis cross at every joint plus a
// segment from each joint to its parent. The world joint transform is
// boneMatrix * inverse(inverseBindMatrix), because a bone matrix is defined as
// globalJointXform * invBind (composeBoneMatrices, AnimationEval.cpp) — with an
// EMPTY boneMatrices (the bind pose, which is exactly what LevelScriptPanel:1846
// passes) every scratch entry is identity and the markers land on the bind pose
// rather than collapsing onto the origin.
//
// Runs BEFORE the command buffer opens: growing the vertex buffer destroys the
// old one, which must not happen while a recorded command buffer references it.
uint32_t VulkanRenderer::buildSkelBoneOverlay(const HE::UUID& meshId,
                                             const std::vector<glm::mat4>& boneScratch,
                                             float extent, const glm::mat4& mvpVk)
{
    if (!ensureSkelLinePipeline() || !m_contentManager) return 0;
    const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(meshId);
    if (!asset || asset->skeleton.empty()) return 0;

    std::vector<glm::vec3> jointWorld(asset->skeleton.size(), glm::vec3(0.0f));
    for (size_t i = 0; i < asset->skeleton.size(); ++i)
    {
        glm::mat4 invBind(1.0f);
        std::memcpy(&invBind, asset->skeleton[i].inverseBindMatrix.data(), sizeof(glm::mat4));
        const glm::mat4 world =
            (i < boneScratch.size() ? boneScratch[i] : glm::mat4(1.0f)) * glm::inverse(invBind);
        jointWorld[i] = glm::vec3(world[3]);
    }

    std::vector<float> lineVerts; // pos3 + color3 per vertex
    const glm::vec3 jointColor(1.0f, 0.85f, 0.15f), boneColor(0.2f, 0.9f, 1.0f);
    auto pushVert = [&](const glm::vec3& p, const glm::vec3& c) {
        lineVerts.insert(lineVerts.end(), { p.x, p.y, p.z, c.x, c.y, c.z });
    };
    const float markerSize = std::max(extent * 0.015f, 0.005f);
    for (size_t i = 0; i < asset->skeleton.size(); ++i)
    {
        const glm::vec3 p = jointWorld[i];
        pushVert(p - glm::vec3(markerSize, 0, 0), jointColor); pushVert(p + glm::vec3(markerSize, 0, 0), jointColor);
        pushVert(p - glm::vec3(0, markerSize, 0), jointColor); pushVert(p + glm::vec3(0, markerSize, 0), jointColor);
        pushVert(p - glm::vec3(0, 0, markerSize), jointColor); pushVert(p + glm::vec3(0, 0, markerSize), jointColor);

        const int32_t parent = asset->skeleton[i].parent;
        if (parent >= 0 && static_cast<size_t>(parent) < jointWorld.size())
        {
            pushVert(jointWorld[parent], boneColor);
            pushVert(p, boneColor);
        }
    }
    if (lineVerts.empty()) return 0;

    const uint32_t vertexCount = static_cast<uint32_t>(lineVerts.size() / 6);
    if (m_skelLineVerts < vertexCount)
    {
        if (!makeHostBuffer(static_cast<VkDeviceSize>(vertexCount) * 6 * sizeof(float),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_skelLineVB))
        {
            m_skelLineVerts = 0;
            return 0;
        }
        m_skelLineVerts = vertexCount;
    }
    // makeHostBuffer can fail half-way (buffer created, memory or map refused), so
    // the mapped pointer is checked rather than assumed — the overlay is optional
    // and must never take the mesh draw down with it.
    if (!m_skelLineVB.mapped || !m_skelLineUBO.mapped) return 0;
    std::memcpy(m_skelLineVB.mapped, lineVerts.data(), lineVerts.size() * sizeof(float));
    // The same clip-space matrix the mesh was drawn with, so the overlay lands on it.
    std::memcpy(m_skelLineUBO.mapped, &mvpVk, sizeof(glm::mat4));
    return vertexCount;
}

void VulkanRenderer::drawSkelBoneOverlay(VkCommandBuffer cmd, uint32_t vertexCount)
{
    if (vertexCount == 0 || !m_skelLinePipe || !m_skelLineVB.buf) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skelLinePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugPipelineLayout,
                            0, 1, &m_skelLineSet, 0, nullptr);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_skelLineVB.buf, &off);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
}

// ─── RenderSkeletalPreview ──────────────────────────────────────────────────
void* VulkanRenderer::RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
                                            const std::vector<glm::mat4>& boneMatrices,
                                            uint32_t width, uint32_t height,
                                            float yaw, float pitch, float dist,
                                            bool showSkeleton, glm::mat4* outViewProj)
{
#if defined(HE_HAVE_SHADERC)
    if (!m_contentManager) m_contentManager = &cm;
    if (!m_device) return nullptr;

    // GL clamps this ONE entry point to 32..2048 (:8588), NOT to
    // HE::clampPreviewSize's 32..1024 — the skeletal pane is a whole editor tab
    // and routinely wider than 1024. Mirroring GL is what keeps a maximised
    // Skeletal Mesh Editor from rendering at half resolution here.
    const int reqW = std::clamp(static_cast<int>(width),  32, 2048);
    const int reqH = std::clamp(static_cast<int>(height), 32, 2048);

    // Resolve + build BEFORE recording: resolveSkeletalMesh uploads geometry and
    // its albedo through their own submits + vkQueueWaitIdle.
    const GpuSkeletalMesh* smesh = resolveSkeletalMesh(meshId);
    if (!smesh || smesh->indexCount <= 0) return nullptr;
    if (!ensureSkelPreviewPipeline()) return nullptr;
    if (!ensurePreviewTarget(m_skelPreview, previewBucket(reqW), previewBucket(reqH)))
        return nullptr;

    // ── Orbit camera auto-framed on the mesh's bind-pose bounds (see
    // resolveSkeletalMesh — this backend had none until now). 35°, the shared
    // kPreviewFovDegrees, exactly like GL's :8640.
    //
    // ASPECT = THE REQUESTED SIZE, not the bucketed target's. ImGui rescales the
    // bucketed image into the requested box, so a projection built on reqW/reqH is
    // what makes a sphere come out round on screen; using the bucket's aspect
    // would squash the result by up to 64/W.
    const glm::vec3 center = HE::boundsCenter(smesh->localBounds);
    const float     extent = HE::boundsExtent(smesh->localBounds);
    const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist,
                                                static_cast<float>(reqW) / static_cast<float>(reqH),
                                                HE::kPreviewFovDegrees);
    const glm::mat4 model(1.0f);
    const glm::mat4 mvpVk = HE::kVulkanClipFix * cam.proj * cam.view * model;
    // Handed out in the OPENGL convention (no clip fix), byte-identical to GL's
    // :8646. The caller projects with `1 - (ndc.y*0.5+0.5)` (LevelScriptPanel.cpp
    // :1562), which already maps +Y to the TOP of the displayed image — feeding it
    // the Y-flipped Vulkan matrix would put every gizmo upside down relative to
    // the mesh it annotates.
    if (outViewProj) *outViewProj = cam.proj * cam.view;

    // 128 identity mat4s with the caller's pose copied over the front, exactly
    // like GL's boneScratch — an empty boneMatrices therefore draws the bind pose.
    constexpr int kMaxBones = 128;
    std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
    const int boneCount = static_cast<int>(std::min(boneMatrices.size(),
                                                    static_cast<size_t>(kMaxBones)));
    if (boneCount > 0) std::copy_n(boneMatrices.begin(), boneCount, boneScratch.begin());
    std::memcpy(m_skelPvBones.mapped, boneScratch.data(), kMaxBones * sizeof(glm::mat4));

    SkelPreviewUBO ubo{};
    ubo.color    = glm::vec4(0.75f, 0.75f, 0.75f, 1.0f); // GL: uColor = vec3(0.75)
    ubo.pbr      = glm::vec4(smesh->texView ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    ubo.sun      = glm::vec4(0.0f); // w = 0 → the shader's fixed studio light
    ubo.sunColor = glm::vec4(1.0f);
    ubo.ambient  = glm::vec4(0.35f, 0.35f, 0.35f, 0.0f);
    std::memcpy(m_skelPvUBO.mapped, &ubo, sizeof(ubo));

    {
        VkDescriptorBufferInfo cbBI  { m_skelPvUBO.buf,   0, sizeof(SkelPreviewUBO) };
        VkDescriptorBufferInfo boneBI{ m_skelPvBones.buf, 0, kMaxBones * sizeof(glm::mat4) };
        // The albedo VIEW, never the GpuSkeletalMesh's albedoSet: that set was
        // allocated against m_albedoSetLayout and belongs at set 2 of the scene
        // layout, not at binding 1 of this one.
        VkDescriptorImageInfo  texII { m_albedoSampler,
                                       smesh->texView ? smesh->texView : m_whiteAlbedoView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[3]{};
        for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                                      w[i].dstSet = m_skelPvSet; w[i].descriptorCount = 1; }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[0].pBufferInfo = &cbBI;
        w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo  = &texII;
        w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         w[2].pBufferInfo = &boneBI;
        vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
    }

    const uint32_t overlayVerts = showSkeleton
        ? buildSkelBoneOverlay(meshId, boneScratch, extent, mvpVk) : 0;

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return nullptr;
    beginPreviewPass(cmd, m_skelPreview);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skelPvPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skelPvLayout,
                            0, 1, &m_skelPvSet, 0, nullptr);
    MeshPreviewPush push{};
    push.mvp   = mvpVk;
    push.model = model;
    vkCmdPushConstants(cmd, m_skelPvLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    const VkBuffer     vbs[3] = { smesh->vb, smesh->boneIdVb, smesh->boneWgtVb };
    const VkDeviceSize offs[3] = { 0, 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 3, vbs, offs);
    vkCmdBindIndexBuffer(cmd, smesh->ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(smesh->indexCount), 1, 0, 0, 0);
    drawSkelBoneOverlay(cmd, overlayVerts);
    const char* dumpPath = endPreviewPass(cmd, m_skelPreview, "HE_SKEL_PREVIEW_DUMP");
    endThumbnailCmd(cmd);
    if (dumpPath) writePreviewDump(m_skelPreview, dumpPath);

    return m_skelPreview.imguiTex;
#else
    // No cross-compiler → the skinning shader cannot be built (skinned.vert.spv
    // exists but its varying layout does not match any fragment shader shipped
    // here, and CMakeLists is where a new .glsl would have to be registered).
    (void)cm; (void)meshId; (void)boneMatrices; (void)width; (void)height;
    (void)yaw; (void)pitch; (void)dist; (void)showSkeleton; (void)outViewProj;
    return nullptr;
#endif
}

// ─── RenderParticlePreview ──────────────────────────────────────────────────
// The mesh id is unused, exactly as on OpenGL (:9173): the preview draws
// camera-facing billboards, and the graph's Emitter Output mesh is not wired into
// either backend's preview yet.
void* VulkanRenderer::RenderParticlePreview(ContentManager& cm, const HE::UUID& /*meshId*/,
                                            const HE::UUID& materialId,
                                            const std::vector<ParticlePreviewInstance>& particles,
                                            uint32_t size, float yaw, float pitch, float dist)
{
    if (!m_contentManager) m_contentManager = &cm;
    if (!m_device) return nullptr;
    // GL bails when the program cannot be built, but NOT on an empty particle
    // list — an emitter between bursts still gets a cleared, transparent target
    // instead of the panel's "(preview unavailable on this backend)".
    if (!ensureParticlePreviewPipeline()) return nullptr;

    const int S = previewBucket(HE::clampPreviewSize(size));
    if (!ensurePreviewTarget(m_particlePreview, S, S)) return nullptr;

    // The material's base texture, or null → the shader's soft circular sprite.
    // Resolved before recording: resolveMaterialOverride may upload.
    VkImageView tex = VK_NULL_HANDLE;
    const MaterialTexVk* ovr = nullptr;
    if (resolveMaterialOverride(materialId, ovr) && ovr) tex = ovr->view;

    VkCommandBuffer cmd = beginThumbnailCmd();
    if (!cmd) return nullptr;
    beginPreviewPass(cmd, m_particlePreview);
    // Square target, so the P1b helper's own S×S viewport and aspect-1 camera are
    // exactly right — this is the same draw the Content-Browser tile makes, only
    // with the caller's orbit instead of the fixed three-quarter one.
    drawParticlePreviewGeometry(cmd, S, tex, particles, yaw, pitch, dist);
    const char* dumpPath = endPreviewPass(cmd, m_particlePreview, "HE_PARTICLE_PREVIEW_DUMP");
    endThumbnailCmd(cmd);
    if (dumpPath) writePreviewDump(m_particlePreview, dumpPath);

    return m_particlePreview.imguiTex;
}

// Everything the preview paths ever created. Called from Shutdown BEFORE
// destroyThumbnailResources(), because the three framebuffers are built over
// m_thumbRenderPass and a framebuffer must not outlive its render pass. The
// graph-material pipelines themselves are NOT freed here: they live in
// m_materialPipelines and go with destroyMaterialResources(), like every other
// variant. The descriptor sets go with m_thumbDescPool.
void VulkanRenderer::destroyPreviewResources()
{
    if (!m_device) return;
    // The shared descriptor pool goes FIRST, here rather than in
    // destroyThumbnailResources (which now finds it already null and skips it):
    // it is the only way to release sets from a pool without the FREE bit, and
    // doing it up front means every descriptor-set LAYOUT below — the preview
    // ones here, the tile ones there — is destroyed after the sets built from it.
    if (m_thumbDescPool) { vkDestroyDescriptorPool(m_device, m_thumbDescPool, nullptr); m_thumbDescPool = VK_NULL_HANDLE; }
    m_meshPvSet     = VK_NULL_HANDLE;
    m_particlePvSet = VK_NULL_HANDLE;
    m_matPvSet      = VK_NULL_HANDLE;
    m_skelPvSet     = VK_NULL_HANDLE;
    m_skelLineSet   = VK_NULL_HANDLE;

    destroyPreviewTarget(m_matPreview);
    destroyPreviewTarget(m_skelPreview);
    destroyPreviewTarget(m_particlePreview);
    if (m_previewSampler) { vkDestroySampler(m_device, m_previewSampler, nullptr); m_previewSampler = VK_NULL_HANDLE; }

    if (m_skelLinePipe) { vkDestroyPipeline(m_device, m_skelLinePipe, nullptr); m_skelLinePipe = VK_NULL_HANDLE; }
    destroyHostBuffer(m_skelLineUBO);
    destroyHostBuffer(m_skelLineVB);
    m_skelLineVerts = 0;

    if (m_skelPvPipe)   { vkDestroyPipeline      (m_device, m_skelPvPipe,   nullptr); m_skelPvPipe   = VK_NULL_HANDLE; }
    if (m_skelPvLayout) { vkDestroyPipelineLayout(m_device, m_skelPvLayout, nullptr); m_skelPvLayout = VK_NULL_HANDLE; }
    if (m_skelPvDSL)    { vkDestroyDescriptorSetLayout(m_device, m_skelPvDSL, nullptr); m_skelPvDSL  = VK_NULL_HANDLE; }
    destroyHostBuffer(m_skelPvUBO);
    destroyHostBuffer(m_skelPvBones);

    destroyHostBuffer(m_matPvLightBuf);
    destroyHostBuffer(m_matPvObjBuf);
    destroyHostBuffer(m_matPvParBuf);
    m_matPvReady = false;

    for (auto& [key, gt] : m_graphTexCache)
    {
        if (gt.view)  vkDestroyImageView(m_device, gt.view,  nullptr);
        if (gt.image) vkDestroyImage    (m_device, gt.image, nullptr);
        if (gt.mem)   vkFreeMemory      (m_device, gt.mem,   nullptr);
    }
    m_graphTexCache.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// SSAO — screen-space ambient occlusion
// ─────────────────────────────────────────────────────────────────────────────

// Helper: allocate + bind device-local image, create image view.
// Aspect is COLOR for regular images, DEPTH_BIT for depth images.
static void ssaoMakeImage(VkDevice dev, uint32_t memTypeBits,
    VkFormat fmt, uint32_t w, uint32_t h,
    VkImageUsageFlags usage, VkImageAspectFlags aspect,
    VkImage& img, VkDeviceMemory& mem, VkImageView& view,
    const std::function<uint32_t(uint32_t,VkMemoryPropertyFlags)>& findMem)
{
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = fmt;
    ici.extent      = { w, h, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = 1;
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(dev, &ici, nullptr, &img), "ssao image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(dev, &mai, nullptr, &mem), "ssao image mem");
    vkBindImageMemory(dev, img, mem, 0);
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image    = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange = { aspect, 0, 1, 0, 1 };
    vkCheck(vkCreateImageView(dev, &vci, nullptr, &view), "ssao image view");
}

// Helper: transition image layout via pipeline barrier (no renderpass needed).
static void ssaoTransition(VkCommandBuffer cmd, VkImage img,
    VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    if (from == to) return;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRenderer::createSSAOPipeline()
{
    m_ssaoReady = false;

    // ── Samplers ─────────────────────────────────────────────────────────────
    // Linear clamp for the position texture and blurred AO result.
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_ssaoSampler), "ssao sampler");
    }
    // Nearest + REPEAT for the 4x4 noise tile (must repeat over the screen).
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_NEAREST;
        sci.minFilter    = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_ssaoNoiseSampler), "ssao noise sampler");
    }

    // ── 1x1 white R8_UNORM fallback for binding=3 when SSAO is off ──────────
    // Must contain 0xFF so texture().r == 1.0 and ao == 1.0 (no occlusion).
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = VK_FORMAT_R8_UNORM;
        ici.extent      = { 1, 1, 1 };
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_ssaoWhiteTex), "ssao white tex");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_ssaoWhiteTex, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_ssaoWhiteMem), "ssao white mem");
        vkBindImageMemory(m_device, m_ssaoWhiteTex, m_ssaoWhiteMem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = m_ssaoWhiteTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = VK_FORMAT_R8_UNORM;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_ssaoWhiteView), "ssao white view");
        // Upload 0xFF via a staging buffer + copy so the sampler reads 1.0.
        uint8_t white = 0xFF;
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = 1;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf), "ssao white staging buf");
            VkMemoryRequirements sreq{};
            vkGetBufferMemoryRequirements(m_device, stagingBuf, &sreq);
            VkMemoryAllocateInfo smai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            smai.allocationSize  = sreq.size;
            smai.memoryTypeIndex = findMemoryType(sreq.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkCheck(vkAllocateMemory(m_device, &smai, nullptr, &stagingMem), "ssao white staging mem");
            vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);
            void* p = nullptr;
            vkMapMemory(m_device, stagingMem, 0, 1, 0, &p);
            std::memcpy(p, &white, 1);
            vkUnmapMemory(m_device, stagingMem);
        }
        // One-shot command buffer to transition + copy.
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        ssaoTransition(tmp, m_ssaoWhiteTex,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { 1, 1, 1 };
        vkCmdCopyBufferToImage(tmp, stagingBuf, m_ssaoWhiteTex,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ssaoTransition(tmp, m_ssaoWhiteTex,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        vkFreeMemory(m_device, stagingMem, nullptr);
    }

    // Now that m_ssaoWhiteView exists, write binding=3 on all per-frame scene
    // descriptor sets so the scene shader always has a valid AO sampler.
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (!m_frameUBO[i].set) continue;
        VkDescriptorImageInfo wdii{ m_ssaoSampler, m_ssaoWhiteView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        // Binding 3 (AO) + bindings 4-7 (GI mask/atlases/local mask): all start
        // on the 1x1 white fallback; runGi() rewrites 4-7 when it produces real
        // targets.
        VkWriteDescriptorSet aw[5]{};
        for (uint32_t b = 0; b < 5; ++b)
        {
            aw[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            aw[b].dstSet          = m_frameUBO[i].set;
            aw[b].dstBinding      = 3 + b;
            aw[b].descriptorCount = 1;
            aw[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            aw[b].pImageInfo      = &wdii;
        }
        vkUpdateDescriptorSets(m_device, 5, aw, 0, nullptr);
    }

    // ── 4x4 noise texture (RGBA32F, NEAREST+REPEAT) ──────────────────────────
    {
        const auto noise = HE::BuildSSAONoiseRGBA(HE::kSsaoNoiseCount);
        const VkDeviceSize noiseBytes = noise.size() * sizeof(glm::vec4);

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = VK_FORMAT_R32G32B32A32_SFLOAT;
        ici.extent      = { (uint32_t)HE::kSsaoNoiseTileSize, (uint32_t)HE::kSsaoNoiseTileSize, 1 };
        ici.mipLevels   = 1;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_ssaoNoiseTex), "ssao noise tex");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, m_ssaoNoiseTex, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_ssaoNoiseMem), "ssao noise mem");
        vkBindImageMemory(m_device, m_ssaoNoiseTex, m_ssaoNoiseMem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = m_ssaoNoiseTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_ssaoNoiseView), "ssao noise view");

        // Upload via staging buffer.
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = noiseBytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &stagingBuf), "ssao noise staging buf");
            VkMemoryRequirements sreq{};
            vkGetBufferMemoryRequirements(m_device, stagingBuf, &sreq);
            VkMemoryAllocateInfo smai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            smai.allocationSize  = sreq.size;
            smai.memoryTypeIndex = findMemoryType(sreq.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkCheck(vkAllocateMemory(m_device, &smai, nullptr, &stagingMem), "ssao noise staging mem");
            vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0);
            void* p = nullptr;
            vkMapMemory(m_device, stagingMem, 0, noiseBytes, 0, &p);
            std::memcpy(p, noise.data(), (size_t)noiseBytes);
            vkUnmapMemory(m_device, stagingMem);
        }
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        ssaoTransition(tmp, m_ssaoNoiseTex,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { (uint32_t)HE::kSsaoNoiseTileSize, (uint32_t)HE::kSsaoNoiseTileSize, 1 };
        vkCmdCopyBufferToImage(tmp, stagingBuf, m_ssaoNoiseTex,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ssaoTransition(tmp, m_ssaoNoiseTex,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
        vkDestroyBuffer(m_device, stagingBuf, nullptr);
        vkFreeMemory(m_device, stagingMem, nullptr);
    }

    // ── SSAO UBO (SSAOCB): mat4 + vec4 + vec4 + vec4[32] = 608 bytes ────────
    {
        // std140: mat4(64) + vec4(16) + vec4(16) + vec4[32](512) = 608
        constexpr VkDeviceSize kSSAOUBOSize = 64 + 16 + 16 + 32*16;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = kSSAOUBOSize;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &m_ssaoUBO), "ssao ubo");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_ssaoUBO, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_ssaoUBOMem), "ssao ubo mem");
        vkBindBufferMemory(m_device, m_ssaoUBO, m_ssaoUBOMem, 0);
        vkMapMemory(m_device, m_ssaoUBOMem, 0, kSSAOUBOSize, 0, &m_ssaoUBOPtr);
        // Pre-fill the kernel (static, never changes).
        const auto kernel = HE::BuildSSAOKernel(HE::kSsaoKernelSize);
        // Layout: [mat4 proj(64)] [vec4 noiseScale(16)] [vec4 params(16)] [vec4 kernel[32](512)]
        // We set params/noiseScale at runtime; fill kernel now.
        uint8_t* base = static_cast<uint8_t*>(m_ssaoUBOPtr);
        std::memcpy(base + 64 + 16 + 16, kernel.data(), HE::kSsaoKernelSize * sizeof(glm::vec3));
        // Note: kernel elements are vec3 but the shader expects vec4 (w unused).
        // Expand them to vec4 with w=0.
        // Re-copy as vec4 array.
        for (int k = HE::kSsaoKernelSize - 1; k >= 0; --k)
        {
            glm::vec4 kv4(kernel[k], 0.0f);
            std::memcpy(base + 64 + 16 + 16 + k * 16, &kv4, 16);
        }
    }

    // ── Render passes ─────────────────────────────────────────────────────────
    // Position prepass: RGBA16F color + D16_UNORM depth.
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        atts[1].format         = VK_FORMAT_D16_UNORM;
        atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 1;
        sub.pColorAttachments       = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        // Begin dependency: wait for previous fragment reads before we write color/depth.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // End dependency: make our color write visible to next fragment shader read (posRT → ssao.frag).
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2; rpci.pAttachments = atts;
        rpci.subpassCount    = 1; rpci.pSubpasses   = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_ssaoPosRenderPass), "ssao pos rp");
    }
    // AO and blur render passes: single R8_UNORM color attachment.
    auto makeR8RP = [&](VkImageLayout finalLayout, VkRenderPass& rp) {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = finalLayout;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;
        VkSubpassDependency deps[2]{};
        // Begin dependency: wait for previous fragment reads before we write color.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // End dependency: make color write visible to next fragment shader read
        // (ssaoRT → blur.frag, blurRT → scene.frag).
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments   = &att;
        rpci.subpassCount    = 1; rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies   = deps;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &rp), "ssao r8 rp");
    };
    makeR8RP(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_ssaoRenderPass);
    makeR8RP(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_ssaoBlurRenderPass);

    // ── Descriptor pool (SSAO set + blur set) ────────────────────────────────
    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 },  // SSAO UBO
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 },  // ssaoPos + noise + aoBlur input
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = poolSizes;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_ssaoDescPool), "ssao desc pool");

    // ── SSAO descriptor set layout: binding0=UBO, binding1=posRT, binding2=noise ─
    {
        VkDescriptorSetLayoutBinding binds[3]{};
        binds[0].binding         = 0;
        binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[0].pImmutableSamplers = nullptr;
        binds[1].binding         = 1;
        binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].pImmutableSamplers = &m_ssaoSampler;
        binds[2].binding         = 2;
        binds[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[2].descriptorCount = 1;
        binds[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2].pImmutableSamplers = &m_ssaoNoiseSampler;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 3;
        slci.pBindings    = binds;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_ssaoDescLayout), "ssao dsl");
    }
    // ── Blur descriptor set layout: binding0=AO input sampler ────────────────
    {
        VkDescriptorSetLayoutBinding bind{};
        bind.binding         = 0;
        bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bind.pImmutableSamplers = &m_ssaoSampler;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1;
        slci.pBindings    = &bind;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_ssaoBlurDescLayout), "ssao blur dsl");
    }

    // Allocate descriptor sets.
    {
        VkDescriptorSetLayout layouts[2] = { m_ssaoDescLayout, m_ssaoBlurDescLayout };
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_ssaoDescPool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = layouts;
        VkDescriptorSet sets[2]{};
        vkCheck(vkAllocateDescriptorSets(m_device, &dsai, sets), "ssao ds alloc");
        m_ssaoDescSet     = sets[0];
        m_ssaoBlurDescSet = sets[1];
    }

    // Write the static bindings of the SSAO descriptor set (UBO + noise).
    // binding=1 (posRT) is viewport-size-dependent → written in createSSAOTargets().
    {
        constexpr VkDeviceSize kSSAOUBOSize = 64 + 16 + 16 + 32*16;
        VkDescriptorBufferInfo ubi{ m_ssaoUBO, 0, kSSAOUBOSize };
        VkDescriptorImageInfo  noiseII{ VK_NULL_HANDLE, m_ssaoNoiseView,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = m_ssaoDescSet;
        w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubi;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = m_ssaoDescSet;
        w[1].dstBinding = 2; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &noiseII;
        vkUpdateDescriptorSets(m_device, 2, w, 0, nullptr);
    }

    // ── Pipeline layouts ──────────────────────────────────────────────────────
    // Position prepass: reuses m_scenePipelineLayout (push constants: mvp + model).
    {
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &m_ssaoDescLayout;
        plci.pushConstantRangeCount = 0;
        vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_ssaoPipeLayout), "ssao pipe layout");
    }
    {
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &m_ssaoBlurDescLayout;
        plci.pushConstantRangeCount = 0;
        vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_ssaoBlurPipeLayout), "ssao blur pipe layout");
    }

    // ── Load shaders ──────────────────────────────────────────────────────────
    VkShaderModule posVS   = loadShaderModule("ssao_pos.vert.spv");
    VkShaderModule posFS   = loadShaderModule("ssao_pos.frag.spv");
    VkShaderModule fsxVS   = loadShaderModule("postfx.vert.spv");
    VkShaderModule ssaoFS  = loadShaderModule("ssao.frag.spv");
    VkShaderModule blurFS  = loadShaderModule("ssao_blur.frag.spv");
    if (!posVS || !posFS || !fsxVS || !ssaoFS || !blurFS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: SSAO shaders missing — SSAO disabled");
        for (auto m : {posVS, posFS, fsxVS, ssaoFS, blurFS})
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
        return;
    }

    // ── Vertex input: aPos(3f) + aNormal(3f) + aUV(2f) (same as scene pass) ─
    VkVertexInputBindingDescription bind{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;    vi.pVertexBindingDescriptions   = &bind;
    vi.vertexAttributeDescriptionCount = 3;    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Depth-stencil state with depth test + write for position prepass.
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                         VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    // ── Position prepass pipeline ─────────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = posVS; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = posFS; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_scenePipelineLayout; // push constants: mvp(64) + modelView(64)
        pci.renderPass          = m_ssaoPosRenderPass;
        vkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                          &m_ssaoPosGfxPipeline), "ssao pos pipeline");
    }

    // Fullscreen (attribute-less) pipeline template — no vertex input, no depth.
    VkPipelineVertexInputStateCreateInfo fsVI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo nods{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo fsRS{ rs };
    fsRS.cullMode = VK_CULL_MODE_NONE;

    // ── SSAO fullscreen pipeline ──────────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = fsxVS;  stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ssaoFS; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &fsVI;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &fsRS;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &nods;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_ssaoPipeLayout;
        pci.renderPass          = m_ssaoRenderPass;
        vkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                          &m_ssaoGfxPipeline), "ssao pipeline");
    }

    // ── Blur fullscreen pipeline ──────────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = fsxVS;  stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = blurFS; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &fsVI;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &fsRS;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &nods;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_ssaoBlurPipeLayout;
        pci.renderPass          = m_ssaoBlurRenderPass;
        vkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                          &m_ssaoBlurGfxPipeline), "ssao blur pipeline");
    }

    for (auto m : {posVS, posFS, fsxVS, ssaoFS, blurFS})
        vkDestroyShaderModule(m_device, m, nullptr);

    m_ssaoReady = true;
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: SSAO pipeline ready");
}

void VulkanRenderer::createSSAOTargets(uint32_t w, uint32_t h)
{
    if (!m_ssaoReady) return;
    destroySSAOTargets();
    m_ssaoW = w; m_ssaoH = h;

    auto fmem = [&](uint32_t bits, VkMemoryPropertyFlags f) {
        return findMemoryType(bits, f);
    };

    const VkImageUsageFlags colorRT = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags depthRT = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // Position prepass color: RGBA16F
    ssaoMakeImage(m_device, 0, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT, m_ssaoPosRT.image, m_ssaoPosRT.memory, m_ssaoPosRT.view, fmem);
    // Position prepass depth: D16_UNORM
    ssaoMakeImage(m_device, 0, VK_FORMAT_D16_UNORM, w, h, depthRT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, m_ssaoPosDepth.image, m_ssaoPosDepth.memory, m_ssaoPosDepth.view, fmem);
    // SSAO raw AO: R8_UNORM
    ssaoMakeImage(m_device, 0, VK_FORMAT_R8_UNORM, w, h, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT, m_ssaoRT.image, m_ssaoRT.memory, m_ssaoRT.view, fmem);
    // Blurred AO: R8_UNORM
    ssaoMakeImage(m_device, 0, VK_FORMAT_R8_UNORM, w, h, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT, m_ssaoBlurRT.image, m_ssaoBlurRT.memory, m_ssaoBlurRT.view, fmem);

    // Framebuffers.
    {
        VkImageView atts[2] = { m_ssaoPosRT.view, m_ssaoPosDepth.view };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_ssaoPosRenderPass; fci.attachmentCount = 2; fci.pAttachments = atts;
        fci.width = w; fci.height = h; fci.layers = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_ssaoPosRT.fb), "ssao pos fb");
    }
    {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_ssaoRenderPass; fci.attachmentCount = 1; fci.pAttachments = &m_ssaoRT.view;
        fci.width = w; fci.height = h; fci.layers = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_ssaoRT.fb), "ssao ao fb");
    }
    {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_ssaoBlurRenderPass; fci.attachmentCount = 1; fci.pAttachments = &m_ssaoBlurRT.view;
        fci.width = w; fci.height = h; fci.layers = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_ssaoBlurRT.fb), "ssao blur fb");
    }

    // Update SSAO descriptor set binding=1 to the new posRT view.
    {
        VkDescriptorImageInfo pii{ VK_NULL_HANDLE, m_ssaoPosRT.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w1{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w1.dstSet = m_ssaoDescSet; w1.dstBinding = 1; w1.descriptorCount = 1;
        w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w1.pImageInfo = &pii;
        vkUpdateDescriptorSets(m_device, 1, &w1, 0, nullptr);
    }
    // Update blur descriptor set binding=0 to the new SSAO raw view.
    {
        VkDescriptorImageInfo aii{ VK_NULL_HANDLE, m_ssaoRT.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w1{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w1.dstSet = m_ssaoBlurDescSet; w1.dstBinding = 0; w1.descriptorCount = 1;
        w1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w1.pImageInfo = &aii;
        vkUpdateDescriptorSets(m_device, 1, &w1, 0, nullptr);
    }
    // Re-point scene binding=3 → blurred AO view (live result).
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (!m_frameUBO[i].set) continue;
        VkDescriptorImageInfo bii{ m_ssaoSampler, m_ssaoBlurRT.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet bw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        bw.dstSet = m_frameUBO[i].set; bw.dstBinding = 3; bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bw.pImageInfo = &bii;
        vkUpdateDescriptorSets(m_device, 1, &bw, 0, nullptr);
    }
}

void VulkanRenderer::destroySSAOTargets()
{
    if (!m_device) return;
    // Note: descriptors referencing these views become stale; caller must re-point
    // them (createSSAOTargets does this on next creation).
    for (auto* rt : { &m_ssaoPosRT, &m_ssaoRT, &m_ssaoBlurRT })
    {
        if (rt->fb)     { vkDestroyFramebuffer(m_device, rt->fb,     nullptr); rt->fb     = VK_NULL_HANDLE; }
        if (rt->view)   { vkDestroyImageView  (m_device, rt->view,   nullptr); rt->view   = VK_NULL_HANDLE; }
        if (rt->image)  { vkDestroyImage      (m_device, rt->image,  nullptr); rt->image  = VK_NULL_HANDLE; }
        if (rt->memory) { vkFreeMemory        (m_device, rt->memory, nullptr); rt->memory = VK_NULL_HANDLE; }
    }
    // Depth has a view but the fb is shared with ssaoPosRT.
    if (m_ssaoPosDepth.view)   { vkDestroyImageView(m_device, m_ssaoPosDepth.view,   nullptr); m_ssaoPosDepth.view   = VK_NULL_HANDLE; }
    if (m_ssaoPosDepth.image)  { vkDestroyImage    (m_device, m_ssaoPosDepth.image,  nullptr); m_ssaoPosDepth.image  = VK_NULL_HANDLE; }
    if (m_ssaoPosDepth.memory) { vkFreeMemory      (m_device, m_ssaoPosDepth.memory, nullptr); m_ssaoPosDepth.memory = VK_NULL_HANDLE; }
    m_ssaoW = m_ssaoH = 0;
}

void VulkanRenderer::runSSAO(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (!m_ssaoReady || !m_ssaoEnabled) return;
    if (!m_ssaoPosRT.fb || !m_ssaoRT.fb || !m_ssaoBlurRT.fb) return;
    if (!m_world) return;

    // Extract + cull the scene against the camera frustum (same as DrawScene).
    const float aspect = w > 0 && h > 0 ? float(w) / float(h) : 1.0f;
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;

    for (RenderObject& obj : m_renderWorld.objects)
        if (const GpuMesh* mesh = resolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);

    m_culler.cull(m_renderWorld, m_visible);
    m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
    if (m_sortedIndices.empty()) return;

    const glm::mat4 view     = m_renderWorld.camera.view;
    const glm::mat4 proj     = m_renderWorld.camera.projection;
    const glm::mat4 clipProj = HE::kVulkanClipFix * proj; // clip-space proj (no view)
    const glm::mat4 vp       = HE::kVulkanClipFix * proj * view;

    // ── Update SSAO UBO (proj, noiseScale, params) ───────────────────────────
    if (m_ssaoUBOPtr)
    {
        uint8_t* base = static_cast<uint8_t*>(m_ssaoUBOPtr);
        // [0..63]:  uSSAOProj = HE::kVulkanClipFix * camera.projection
        std::memcpy(base, &clipProj, 64);
        // [64..79]: uSSAONoiseScale.xy = viewport / noiseSize
        glm::vec4 noiseScale(float(w) / float(HE::kSsaoNoiseTileSize),
                             float(h) / float(HE::kSsaoNoiseTileSize), 0.0f, 0.0f);
        std::memcpy(base + 64, &noiseScale, 16);
        // [80..95]: uSSAOParams = (radius, bias, intensity, 0)
        glm::vec4 params(m_ssaoRadius, m_ssaoBias, m_ssaoIntensity, float(m_ssaoMethod));
        std::memcpy(base + 80, &params, 16);
        // [96..607]: kernel (pre-filled in createSSAOPipeline, not changed here)
    }

    // ── Pass 1: position prepass ──────────────────────────────────────────────
    VkClearValue posClear[2]{};
    posClear[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };  // a=0 = background
    posClear[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo posRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    posRPBI.renderPass        = m_ssaoPosRenderPass;
    posRPBI.framebuffer       = m_ssaoPosRT.fb;
    posRPBI.renderArea.extent = { w, h };
    posRPBI.clearValueCount   = 2;
    posRPBI.pClearValues      = posClear;
    vkCmdBeginRenderPass(cmd, &posRPBI, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoPosGfxPipeline);
    VkViewport vp2{ 0, 0, float(w), float(h), 0, 1 };
    VkRect2D sc{ {0,0}, {w, h} };
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    for (uint32_t idx : m_sortedIndices)
    {
        const RenderObject& obj = m_renderWorld.objects[idx];
        if (!obj.contributesAO) continue;   // skip particles/precipitation
        const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
        const GpuMesh& gm   = mesh ? *mesh : m_cube;
        if (!gm.indexCount) continue;

        // Push constants: mvp (clip-space) + modelView (view-space).
        // The ssao_pos.vert shader uses push_constant block with same layout as PushConstants.
        const glm::mat4 modelView = view * obj.transform;
        PushConstants pc{ vp * obj.transform, modelView };
        vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(pc), &pc);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbuf, &offset);
        vkCmdBindIndexBuffer(cmd, gm.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    // posRT transitions to SHADER_READ_ONLY via renderpass finalLayout.

    // ── Pass 2: SSAO fullscreen ───────────────────────────────────────────────
    VkRenderPassBeginInfo aoRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    aoRPBI.renderPass        = m_ssaoRenderPass;
    aoRPBI.framebuffer       = m_ssaoRT.fb;
    aoRPBI.renderArea.extent = { w, h };
    vkCmdBeginRenderPass(cmd, &aoRPBI, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoGfxPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_ssaoPipeLayout, 0, 1, &m_ssaoDescSet, 0, nullptr);
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // attribute-less fullscreen triangle
    vkCmdEndRenderPass(cmd);
    // ssaoRT transitions to SHADER_READ_ONLY via renderpass finalLayout.

    // ── Pass 3: blur ──────────────────────────────────────────────────────────
    VkRenderPassBeginInfo blurRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    blurRPBI.renderPass        = m_ssaoBlurRenderPass;
    blurRPBI.framebuffer       = m_ssaoBlurRT.fb;
    blurRPBI.renderArea.extent = { w, h };
    vkCmdBeginRenderPass(cmd, &blurRPBI, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoBlurGfxPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_ssaoBlurPipeLayout, 0, 1, &m_ssaoBlurDescSet, 0, nullptr);
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    // ssaoBlurRT transitions to SHADER_READ_ONLY via renderpass finalLayout.
    // It is now safe for the scene pass to sample it at binding=3.
    m_ssaoRanThisFrame = true;  // blurRT is valid; DrawScene will enable AO sampling
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU skeletal-mesh skinning
// ─────────────────────────────────────────────────────────────────────────────

void VulkanRenderer::createSkinnedPipeline()
{
    // ── Bones descriptor set layout (set=1, binding=0, VERTEX stage) ─────────
    VkDescriptorSetLayoutBinding bonesBind{};
    bonesBind.binding         = 0;
    bonesBind.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bonesBind.descriptorCount = 1;
    bonesBind.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo bonesDslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    bonesDslci.bindingCount = 1;
    bonesDslci.pBindings    = &bonesBind;
    if (vkCreateDescriptorSetLayout(m_device, &bonesDslci, nullptr, &m_skinnedBonesDSL) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned bones DSL creation failed");
        return;
    }

    // ── Pipeline layout: set=0 (scene.frag data) + set=1 (bones) + set=2 (albedo) + push const ─
    // Set 2 = base-color table, shared with the scene pipeline (same shared scene.frag).
    VkDescriptorSetLayout sets[3] = { m_sceneSetLayout, m_skinnedBonesDSL, m_albedoSetLayout };
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 3;
    plci.pSetLayouts            = sets;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_skinnedPipeLayout) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned pipeline layout failed");
        return;
    }

    // ── Descriptor pool for per-frame bones sets ──────────────────────────────
    VkDescriptorPoolSize bonePooSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_maxFramesInFlight };
    VkDescriptorPoolCreateInfo bonesPoolci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    bonesPoolci.maxSets       = k_maxFramesInFlight;
    bonesPoolci.poolSizeCount = 1;
    bonesPoolci.pPoolSizes    = &bonePooSize;
    if (vkCreateDescriptorPool(m_device, &bonesPoolci, nullptr, &m_skinnedDescPool) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned descriptor pool failed");
        return;
    }

    // ── Per-frame bones UBO (256 slots × 8192 bytes = 2MB, dynamic offsets) ──
    // Each skinned draw gets its own slot so the GPU sees the correct pose
    // when multiple skinned meshes are drawn in one frame.
    constexpr VkDeviceSize kBoneSlotSize  = 128 * sizeof(glm::mat4);  // 8192 bytes per draw
    constexpr uint32_t     k_maxSkinnedVk = 256;
    constexpr VkDeviceSize kBoneUBOTotal  = k_maxSkinnedVk * kBoneSlotSize;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = kBoneUBOTotal;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &m_boneUBO[i]) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: bone UBO buffer failed");
            return;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, m_boneUBO[i], &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &m_boneUBOMem[i]) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: bone UBO memory failed");
            return;
        }
        vkBindBufferMemory(m_device, m_boneUBO[i], m_boneUBOMem[i], 0);
        vkMapMemory(m_device, m_boneUBOMem[i], 0, kBoneUBOTotal, 0, &m_boneUBOPtr[i]);

        // Allocate and write the per-frame descriptor set for the bones UBO.
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_skinnedDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_skinnedBonesDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_boneDescSet[i]) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned descriptor set alloc failed");
            return;
        }
        // range = one slot; the dynamic offset selects which slot the shader reads.
        VkDescriptorBufferInfo dbi{ m_boneUBO[i], 0, kBoneSlotSize };
        VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet          = m_boneDescSet[i];
        wr.dstBinding      = 0;
        wr.descriptorCount = 1;
        wr.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
    }

    // ── Shaders: skinned.vert.spv + scene.frag.spv ───────────────────────────
    VkShaderModule vs = loadShaderModule("skinned.vert.spv");
    VkShaderModule fs = loadShaderModule("scene.frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
    {
        HE_LOG_WARN(RHI, "%s",
            "VulkanRenderer: skinned.vert.spv or scene.frag.spv not found — skinned draws will be skipped");
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        return;
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

    // ── Vertex input: 3 bindings ──────────────────────────────────────────────
    //   slot 0 — interleaved pos(12) + norm(12) + uv(8)  = 32 bytes/vertex
    //   slot 1 — bone IDs:     uvec4 = 16 bytes/vertex
    //   slot 2 — bone weights: vec4  = 16 bytes/vertex
    VkVertexInputBindingDescription bindings[3] = {
        { 0, 32u,                  VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, 4u * sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX },
        { 2, 4u * sizeof(float),   VK_VERTEX_INPUT_RATE_VERTEX },
    };
    VkVertexInputAttributeDescription attrs[5] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // aPos
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 },   // aNormal
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 },   // aUV
        { 3, 1, VK_FORMAT_R32G32B32A32_UINT,   0  },   // aBoneIds
        { 4, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0  },   // aBoneWgts
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 3;
    vi.pVertexBindingDescriptions      = bindings;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions    = attrs;

    // ── Remaining pipeline state (mirrors createScenePipeline) ───────────────
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
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
    pci.layout              = m_skinnedPipeLayout;
    pci.renderPass          = m_renderPass;
    pci.subpass             = 0;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedPipeline) != VK_SUCCESS)
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned pipeline creation failed");

    // ── HDR variant (renderPass = m_postFxSceneRP, RGBA16F) ──────────────────
    if (m_postFxSceneRP != VK_NULL_HANDLE)
    {
        VkGraphicsPipelineCreateInfo hdrPci = pci;
        hdrPci.renderPass = m_postFxSceneRP;
        if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &hdrPci, nullptr, &m_skinnedPipelineHDR) != VK_SUCCESS)
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned HDR pipeline creation failed");
    }

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
}

const VulkanRenderer::GpuSkeletalMesh*
VulkanRenderer::resolveSkeletalMesh(const HE::UUID& id)
{
    if (id == HE::UUID{} || !m_contentManager) return nullptr;
    if (auto it = m_skeletalMeshCache.find(id); it != m_skeletalMeshCache.end())
        return &it->second;

    const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(id);
    if (!asset || asset->vertices.empty() || asset->indices.empty()) return nullptr;

    const size_t vertexCount = asset->vertices.size() / 3;

    // Slot 0: interleaved pos + norm + uv (32 bytes/vertex), same layout as scene.vert.
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

    // Slot 1: bone IDs (flat uint32 array, 4 per vertex).
    // Pad to vertexCount*4 entries with zeros if the asset is short.
    std::vector<uint32_t> boneIds(vertexCount * 4, 0u);
    {
        const size_t copy = std::min(asset->boneIDs.size(), vertexCount * 4);
        std::copy_n(asset->boneIDs.begin(), copy, boneIds.begin());
    }

    // Slot 2: bone weights (flat float array, 4 per vertex).
    std::vector<float> boneWgts(vertexCount * 4, 0.0f);
    {
        const size_t copy = std::min(asset->boneWeights.size(), vertexCount * 4);
        std::copy_n(asset->boneWeights.begin(), copy, boneWgts.begin());
    }

    // Helper: create a host-visible/coherent buffer and memcpy data into it.
    auto makeHostBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                            const void* data, VkBuffer& buf, VkDeviceMemory& mem) -> bool
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, buf, &req);
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

    GpuSkeletalMesh sm;
    bool ok = true;
    ok = ok && makeHostBuf(interleaved.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           interleaved.data(), sm.vb, sm.vbMem);
    ok = ok && makeHostBuf(boneIds.size()  * sizeof(uint32_t), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           boneIds.data(),  sm.boneIdVb, sm.boneIdMem);
    ok = ok && makeHostBuf(boneWgts.size() * sizeof(float),    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           boneWgts.data(), sm.boneWgtVb, sm.boneWgtMem);
    ok = ok && makeHostBuf(asset->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           asset->indices.data(), sm.ib, sm.ibMem);
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: resolveSkeletalMesh buffer creation failed");
        // Partial cleanup: destroy whatever was created before the failure.
        if (sm.vb)       { vkDestroyBuffer(m_device, sm.vb,       nullptr); vkFreeMemory(m_device, sm.vbMem,      nullptr); }
        if (sm.boneIdVb) { vkDestroyBuffer(m_device, sm.boneIdVb, nullptr); vkFreeMemory(m_device, sm.boneIdMem,  nullptr); }
        if (sm.boneWgtVb){ vkDestroyBuffer(m_device, sm.boneWgtVb,nullptr); vkFreeMemory(m_device, sm.boneWgtMem, nullptr); }
        if (sm.ib)       { vkDestroyBuffer(m_device, sm.ib,       nullptr); vkFreeMemory(m_device, sm.ibMem,      nullptr); }
        return nullptr;
    }

    sm.indexCount = static_cast<int>(asset->indices.size());
    // Bind-pose bounds, the same way the loose static-mesh path builds its own
    // (resolveMesh, :3510). A SkeletalMeshAsset ships no baked AABB — unlike a
    // cooked StaticMeshAsset, which carries boundsMin/boundsMax — so this is the
    // only place the numbers exist, and without them nothing can frame a skeletal
    // thumbnail or a skeletal preview.
    sm.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
    // Upload the skeletal mesh's baked base-color texture (if any), same as static meshes.
    sm.hasTex = resolveAndUploadAlbedo(asset->materialId, asset->materialPath,
                                       sm.texImage, sm.texMem, sm.texView, sm.albedoSet);
    return &m_skeletalMeshCache.emplace(id, sm).first->second;
}

void VulkanRenderer::destroySkeletalMeshCache()
{
    for (auto& [id, sm] : m_skeletalMeshCache)
    {
        if (sm.vb)        { vkDestroyBuffer(m_device, sm.vb,        nullptr); vkFreeMemory(m_device, sm.vbMem,       nullptr); }
        if (sm.boneIdVb)  { vkDestroyBuffer(m_device, sm.boneIdVb,  nullptr); vkFreeMemory(m_device, sm.boneIdMem,   nullptr); }
        if (sm.boneWgtVb) { vkDestroyBuffer(m_device, sm.boneWgtVb, nullptr); vkFreeMemory(m_device, sm.boneWgtMem,  nullptr); }
        if (sm.ib)        { vkDestroyBuffer(m_device, sm.ib,        nullptr); vkFreeMemory(m_device, sm.ibMem,        nullptr); }
        // Base-color texture (descriptor set freed with m_albedoPool in destroyScenePipeline).
        if (sm.texView)   vkDestroyImageView(m_device, sm.texView,  nullptr);
        if (sm.texImage)  vkDestroyImage    (m_device, sm.texImage, nullptr);
        if (sm.texMem)    vkFreeMemory      (m_device, sm.texMem,   nullptr);
    }
    m_skeletalMeshCache.clear();
}

// ─── 2D UI canvas rendering ───────────────────────────────────────────────────

void VulkanRenderer::createUIPipeline()
{
    VkShaderModule vs = loadShaderModule("ui.vert.spv");
    VkShaderModule fs = loadShaderModule("ui.frag.spv");
    if (!vs || !fs)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: UI shaders missing — in-game UI disabled");
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        return;
    }

    // Linear clamp sampler for the R8 font atlases (immutable in the set layout).
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_uiFontSampler), "ui font sampler");
    }

    // set=0 binding=0: the font atlas (uFontAtlas). One descriptor set per atlas
    // key, allocated lazily by uiFontAtlasSet().
    {
        VkDescriptorSetLayoutBinding bind{};
        bind.binding            = 0;
        bind.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount    = 1;
        bind.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
        bind.pImmutableSamplers = &m_uiFontSampler;
        VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslci.bindingCount = 1;
        dslci.pBindings    = &bind;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_uiAtlasDSLayout), "ui atlas DS layout");

        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = 32;   // shared font + up to 31 imported fonts
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &ps;
        vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_uiAtlasDescPool), "ui atlas desc pool");
    }

    // Push constant layout: UIPush (64 bytes) visible to both stages.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset     = 0;
    pcr.size       = 64; // vec4 rect + vec4 color + vec4 uvRect + vec2 viewport + vec2 params

    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_uiAtlasDSLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    vkCheck(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_uiPipeLayout), "ui pipe layout");

    // Render pass for the viewport/editor path: single RGBA8 color attachment,
    // LOAD_OP_LOAD (preserve FXAA output), STORE_OP_STORE, no depth.
    // initialLayout = COLOR_ATTACHMENT_OPTIMAL (we barrier into it before the pass),
    // finalLayout   = SHADER_READ_ONLY_OPTIMAL (ImGui reads it after).
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_uiViewportRP), "ui viewport render pass");
    }

    // Helper: build the graphics pipeline for a given render pass.
    auto buildPipeline = [&](VkRenderPass rp, VkPipeline& out) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName  = "main";

        // No vertex buffer — positions are computed from gl_VertexIndex + push constants.
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1;
        vps.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth test and write both disabled — UI always on top.
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable  = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;

        // Alpha blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable         = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
        cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments    = &cba;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vps;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.layout              = m_uiPipeLayout;
        pci.renderPass          = rp;
        pci.subpass             = 0;
        vkCheck(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &out), "ui pipeline");
    };

    // Swapchain pipeline: m_renderPass (swapchain format, has depth attachment).
    buildPipeline(m_renderPass, m_uiPipeline);

    // Viewport pipeline: m_uiViewportRP (RGBA8, color-only, LOAD_OP_LOAD).
    buildPipeline(m_uiViewportRP, m_uiViewportPipeline);

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);

    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: UI canvas pipelines created (swapchain + viewport)");
}

void VulkanRenderer::runUIPass(VkCommandBuffer cmd, int width, int height)
{
    runUIPass(cmd, width, height, m_renderWorld.uiObjects);
}

// The list is a PARAMETER rather than always m_renderWorld.uiObjects because
// RenderWidgetThumbnail runs OUT OF FRAME. GL's trick (OpenGLRenderer.cpp:9155)
// is to swap the caller's vector into m_renderWorld.uiObjects, run the pass and
// swap back — safe there because GL's thumbnail path is on the same thread as
// the only reader. Here the member is read by DrawScene/Render for the frame
// that may still be in flight, so writing it from a tile request is a data race
// on the renderer's own state. Passing the vector down keeps it on the stack.
void VulkanRenderer::runUIPass(VkCommandBuffer cmd, int width, int height,
                               const std::vector<UIRenderObject>& objects)
{
    // Caller is responsible for binding the correct pipeline and setting
    // viewport/scissor before calling. This function only loops over UI objects
    // and issues draw calls — it does NOT begin/end a render pass.
    struct UIPush { glm::vec4 rect; glm::vec4 color; glm::vec4 uvRect; glm::vec2 viewport; glm::vec2 params; };

    // The atlas set must be bound for EVERY draw (the fragment shader statically
    // uses the sampler even for solid quads). Default to the shared font (key 0);
    // glyph quads re-bind when they reference an imported font's atlas.
    VkDescriptorSet atlasSet = uiFontAtlasSet(0);
    if (!atlasSet) return;  // device-level upload failure — nothing valid to bind
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeLayout,
                            0, 1, &atlasSet, 0, nullptr);
    uint32_t boundAtlasKey = 0;

    for (const UIRenderObject& obj : objects)
    {
        // A glyph quad may use an imported font's atlas — bind its set.
        if (obj.type == 2 && obj.fontAtlasKey != boundAtlasKey)
        {
            if (VkDescriptorSet s = uiFontAtlasSet(obj.fontAtlasKey))
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeLayout,
                                        0, 1, &s, 0, nullptr);
                boundAtlasKey = obj.fontAtlasKey;
            }
        }
        UIPush push{};
        push.rect     = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
        push.color    = glm::vec4(obj.color.r, obj.color.g, obj.color.b, obj.color.a);
        push.uvRect   = glm::vec4(obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y);
        push.viewport = glm::vec2(float(width), float(height));
        push.params   = glm::vec2(obj.type == 2 ? 1.0f : 0.0f, 0.0f);
        vkCmdPushConstants(cmd, m_uiPipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(UIPush), &push);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
}

// The descriptor set (R8 atlas image + immutable sampler) for a font key
// (0 = the shared default), uploaded lazily from the CPU-baked bitmap the first
// time a glyph quad references it. Unknown / failed keys fall back to the shared
// font's set so the pipeline always has a valid binding. The upload runs on its
// own one-shot command buffer (mirrors SetMoonTexture), which is safe mid-frame:
// the primary command buffer is still recording, not submitted.
VkDescriptorSet VulkanRenderer::uiFontAtlasSet(uint32_t key)
{
    if (auto it = m_uiFontAtlases.find(key); it != m_uiFontAtlases.end())
        return it->second.set;
    if (!m_uiAtlasDSLayout || !m_uiAtlasDescPool) return VK_NULL_HANDLE;

    const uint8_t* pixels = nullptr;
    int w = 0, h = 0;
    if (key == 0)
    {
        if (const HE::BakedUIFont& f = HE::sharedUIFont(); f.ok)
        {
            pixels = f.pixels.data();
            w = f.atlasW; h = f.atlasH;
        }
    }
    else if (const HE::BakedUIFont* f = HE::UIFontCache::find(key); f && f->ok)
    {
        pixels = f->pixels.data();
        w = f->atlasW; h = f->atlasH;
    }
    // Imported-font key without a baked atlas → shared font (don't cache the
    // fallback: GL re-resolves each frame too). A missing SHARED font is cached
    // as a 1×1 transparent atlas so glyphs simply don't render.
    static const uint8_t kZeroPixel = 0;
    if (!pixels)
    {
        if (key != 0) return uiFontAtlasSet(0);
        pixels = &kZeroPixel; w = h = 1;
    }

    UIFontAtlas atlas;

    // ── Staging buffer ───────────────────────────────────────────────────────
    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(w) * h;
    VkBuffer       stageBuf = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = dataSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_device, &bci, nullptr, &stageBuf);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, stageBuf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &mai, nullptr, &stageMem);
        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
        void* ptr = nullptr;
        vkMapMemory(m_device, stageMem, 0, dataSize, 0, &ptr);
        std::memcpy(ptr, pixels, static_cast<size_t>(dataSize));
        vkUnmapMemory(m_device, stageMem);
    }

    // ── Device-local R8 image (glyph coverage in .r, like GL's GL_R8 atlas) ──
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8_UNORM;
    ici.extent        = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &atlas.image) != VK_SUCCESS)
    {
        vkDestroyBuffer(m_device, stageBuf, nullptr);
        vkFreeMemory(m_device, stageMem, nullptr);
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(m_device, atlas.image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(m_device, &imal, nullptr, &atlas.memory);
    vkBindImageMemory(m_device, atlas.image, atlas.memory, 0);

    // ── One-shot command buffer: transition + copy ───────────────────────────
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer oneCB = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &cbai, &oneCB);
    VkCommandBufferBeginInfo oneBI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    oneBI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(oneCB, &oneBI);

    // UNDEFINED → TRANSFER_DST
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = atlas.image;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent      = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
    vkCmdCopyBufferToImage(oneCB, stageBuf, atlas.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ_ONLY
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = atlas.image;
        bar.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bar.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);
    }

    vkEndCommandBuffer(oneCB);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &oneCB;
    vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &oneCB);

    vkDestroyBuffer(m_device, stageBuf, nullptr);
    vkFreeMemory(m_device, stageMem, nullptr);

    // ── Image view + descriptor set ──────────────────────────────────────────
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = atlas.image;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = VK_FORMAT_R8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(m_device, &ivci, nullptr, &atlas.view);

    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = m_uiAtlasDescPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_uiAtlasDSLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &atlas.set) != VK_SUCCESS)
    {
        // Pool exhausted (>32 fonts): keep the shared font working, drop this one.
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: UI font atlas descriptor pool exhausted");
        vkDestroyImageView(m_device, atlas.view, nullptr);
        vkDestroyImage(m_device, atlas.image, nullptr);
        vkFreeMemory(m_device, atlas.memory, nullptr);
        return key != 0 ? uiFontAtlasSet(0) : VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo dii{ VK_NULL_HANDLE, atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet          = atlas.set;
    wr.dstBinding      = 0;
    wr.descriptorCount = 1;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo      = &dii;
    vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);

    m_uiFontAtlases[key] = atlas;
    return atlas.set;
}

void VulkanRenderer::destroyUIFontAtlases()
{
    for (auto& [key, a] : m_uiFontAtlases)
    {
        if (a.view)   vkDestroyImageView(m_device, a.view,   nullptr);
        if (a.image)  vkDestroyImage    (m_device, a.image,  nullptr);
        if (a.memory) vkFreeMemory      (m_device, a.memory, nullptr);
        // Descriptor sets are freed with m_uiAtlasDescPool in Shutdown().
    }
    m_uiFontAtlases.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU frame timing (VkQueryPool timestamps)
// ─────────────────────────────────────────────────────────────────────────────

void VulkanRenderer::gpuTimerInit()
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physDevice, &props);
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDevice, &n, families.data());
    const uint32_t validBits =
        m_graphicsFamily < n ? families[m_graphicsFamily].timestampValidBits : 0;

    // timestampValidBits == 0 → this queue cannot write timestamps at all;
    // timestampPeriod == 0 would make the tick→ns conversion meaningless.
    if (validBits == 0 || props.limits.timestampPeriod <= 0.0f)
    {
        HE_LOG_INFO(RHI, "%s",
            "VulkanRenderer: GPU timestamps unsupported on this queue — gpuFrameMs stays -1");
        return;
    }
    m_tsPeriodNs  = props.limits.timestampPeriod;
    m_tsValidMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);

    VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kGpuTimerRing * 2;   // begin/end per ring slot
    if (vkCreateQueryPool(m_device, &qpci, nullptr, &m_tsQueryPool) != VK_SUCCESS)
    {
        m_tsQueryPool = VK_NULL_HANDLE;
        return;
    }
    m_tsSupported = true;
}

void VulkanRenderer::gpuTimerBegin(VkCommandBuffer cmd)
{
    if (!m_tsSupported) return;
    const uint32_t slot = static_cast<uint32_t>(m_tsFrameIdx % kGpuTimerRing);

    // This slot was written kGpuTimerRing frames ago; with only
    // k_maxFramesInFlight frames in flight its submit has been fence-waited, so
    // the availability check succeeds without ever using VK_QUERY_RESULT_WAIT.
    if (m_tsPending[slot])
    {
        uint64_t r[4] = {};   // {value, availability} × {begin, end}
        const VkResult res = vkGetQueryPoolResults(m_device, m_tsQueryPool, slot * 2, 2,
            sizeof(r), r, 2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (res == VK_SUCCESS && r[1] && r[3])
        {
            const uint64_t t0 = r[0] & m_tsValidMask;
            const uint64_t t1 = r[2] & m_tsValidMask;
            FrameGpuStats fs;
            fs.gpuFrameMs = (t1 > t0)
                ? static_cast<double>(t1 - t0) * static_cast<double>(m_tsPeriodNs) * 1e-6
                : 0.0;
            fs.gpuTimingMode = "whole-frame";   // no per-pass breakdown on Vulkan yet
            m_lastGpuStats = fs;                // CPU counters merged by GetFrameGpuStats
        }
        m_tsPending[slot] = false;   // reset below invalidates the old results either way
    }

    vkCmdResetQueryPool(cmd, m_tsQueryPool, slot * 2, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_tsQueryPool, slot * 2);
}

void VulkanRenderer::gpuTimerEnd(VkCommandBuffer cmd)
{
    if (!m_tsSupported) return;
    const uint32_t slot = static_cast<uint32_t>(m_tsFrameIdx % kGpuTimerRing);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_tsQueryPool, slot * 2 + 1);
    m_tsPending[slot] = true;
    ++m_tsFrameIdx;
}

IRenderer::FrameGpuStats VulkanRenderer::GetFrameGpuStats() const
{
    // GPU time comes from the newest reaped timestamp slot (1–N frames late; -1
    // before the first reap / when timestamps are unsupported). CPU counters are
    // this frame's — the same split the OpenGL backend reports.
    FrameGpuStats s = m_lastGpuStats;
    s.drawCalls      = m_statDraws;
    s.triangles      = m_statTris;
    s.visibleObjects = m_statVisible;
    s.totalObjects   = m_statTotal;
    return s;
}
