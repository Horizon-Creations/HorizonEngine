#include "Backends/Vulkan/VulkanRenderer.h"
#include <chrono>
#include <cstdint>
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/TextureArrayBuild.h> // texture-array payload layout (Thema 158)
#include <ContentManager/TextureBlockDecode.h> // CPU BC1/BC3/BC4/BC5 expand when the device cannot sample them
#include <Renderer/UIFont.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <functional>
#include <Diagnostics/Logger.h>
#include <Diagnostics/Profiler.h>   // HE_PROFILE_SCOPE_N — GI accel split of Render()
#include <HorizonRendering/ClipSpace.h>
#include <HorizonRendering/GiCasterRange.h>
#include <HorizonRendering/GiHeightfield.h>
#include <Diagnostics/EngineProfiler.h>
#include <HorizonRendering/GIJitter.h>       // GI: wrapped cone-jitter frame index (all backends)
#include <HorizonRendering/GIReflectionTier.h> // GI reflections: quality tier + blur reach (shared, D3D/Vulkan)
#include <HorizonRendering/UnderwaterSurfaceShared.h>  // the tessellated surface for the underwater fog
#include <HorizonRendering/GiInstanceSurface.h> // GI reflections: a hit's albedo/emissive (all backends)
#include <HorizonRendering/TemporalAA.h>      // TAA jitter sequence + jittered matrix (shared with D3D11/D3D12)
#include <HorizonRendering/WorldPreviewGrid.h>  // RenderWorldPreview: grid, background, dump
#include <HorizonRendering/WorldPreviewFrame.h> // RenderWorldPreview: camera, snapshot, light
#include <HorizonRendering/LightPacking.h>
#include <Backends/Vulkan/VulkanMaterialLayout.h> // graph-material set 0 (shared with he_tests)
#include <Backends/Vulkan/VulkanPipelineCacheFile.h> // on-disk VkPipelineCache (REN-808)
#include <HorizonRendering/MaterialScalars.h>
#include <HorizonRendering/RenderConstants.h>
#include <HorizonRendering/SkyFrameParams.h>
#include <HorizonRendering/SkyNoise3D.h>
#include <HorizonRendering/SkyShaderSource.h> // the GL sky, compiled to SPIR-V for the sky pass
#include <HorizonRendering/SkyEnvBake.h>     // shared CPU sky bake (graph materials' heSkyEnv)
#include <JobSystem/JobSystem.h>             // parallel_for for that bake
#include <glm/gtc/packing.hpp>               // packHalf1x16 (RGBA16F cube upload)
#if defined(HE_HAVE_SHADERC)
#include "ShaderCompiler.h"                   // he::shaderc::compile (sky pass)
#endif
#include <HorizonRendering/SsaoKernel.h>
#include <HorizonRendering/GraphTextureMisses.h> // graph-texture misses forgotten per content epoch
#include <MaterialGraph/MaterialGraph.h>      // kMatMaxGraphTextures (heTexP0..3)

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

    // The index range a DrawCall covers on the mesh it ends up drawing — the
    // Vulkan twin of GL's/Metal's/D3D11's/D3D12's DrawIndexRange. A whole-mesh
    // draw (indexCount 0 — every one-section mesh, every ribbon, every draw
    // before sections existed) spans the buffer; a section draw takes its own
    // [offset, count), CLAMPED to the buffer: drawDCVk substitutes the default
    // cube when the real mesh is not resident yet, and a range taken from the
    // real asset must not read past the cube's index buffer. `start` is in
    // INDICES, which is what vkCmdDrawIndexed's firstIndex takes (same as
    // D3D's StartIndexLocation; GL wants a byte pointer, Metal a byte offset).
    struct VulkanIndexRange { uint32_t count; uint32_t start; };
    inline VulkanIndexRange DrawIndexRange(const DrawCall& dc, uint32_t meshIndexCount)
    {
        if (dc.indexCount == 0) return { meshIndexCount, 0u };
        const uint32_t off = std::min<uint32_t>(dc.indexOffset, meshIndexCount);
        const uint32_t cnt = std::min<uint32_t>(dc.indexCount, meshIndexCount - off);
        return { cnt, off };
    }

    // Vertex input of the instanced depth twins (scene_shadow_instanced.vert,
    // ssao_pos_instanced.vert, gi_gbuf_instanced.vert): the mesh at binding 0
    // (pos, normal, uv) and the scene's 128-byte {A, B} instance pair at
    // binding 1 (VK_VERTEX_INPUT_RATE_INSTANCE) — A at locations 3..6, B at
    // 7..10, the layout scene_instanced.vert reads. `pairColumns` = 4 declares
    // A only (the shadow twin never reads B), 8 declares both. `info` points
    // into this object, hence no copies.
    struct InstancedDepthInput
    {
        VkVertexInputBindingDescription      binds[2];
        VkVertexInputAttributeDescription    attrs[11];
        VkPipelineVertexInputStateCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        InstancedDepthInput(uint32_t instStride, uint32_t pairColumns)
        {
            binds[0] = { 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
            binds[1] = { 1, instStride,         VK_VERTEX_INPUT_RATE_INSTANCE };
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0 };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
            attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 };
            for (uint32_t c = 0; c < pairColumns; ++c)
                attrs[3 + c] = { 3 + c, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16u * c };
            info.vertexBindingDescriptionCount   = 2;
            info.pVertexBindingDescriptions      = binds;
            info.vertexAttributeDescriptionCount = 3 + pairColumns;
            info.pVertexAttributeDescriptions    = attrs;
        }
        InstancedDepthInput(const InstancedDepthInput&)            = delete;
        InstancedDepthInput& operator=(const InstancedDepthInput&) = delete;
    };

    // Per-frame UBO, std140 layout (matches the GLSL `Frame` block).
    struct FrameUBOData
    {
        glm::vec4  cameraPos;
        glm::ivec4 lightCount;
        glm::vec4  lightPos[8];
        glm::vec4  lightDir[8];
        glm::vec4  lightColor[8];
        glm::vec4  lightParams[8];
        // Cascaded shadow maps — must match scene.frag's Frame block exactly:
        // per-cascade light view-proj (kVulkanClipFix pre-applied), the planar
        // view-space far distance of each cascade (w = count), the camera
        // forward the pick measures along (w = 1 / shadow map size) and the
        // project receiver bias (x = slope-scaled factor, y = minimum).
        glm::mat4  cascadeVP[3];
        glm::vec4  cascadeSplits;
        glm::vec4  cameraFwd;
        glm::vec4  shadowBias;
        glm::ivec4 shadowEnabled; // x = 0/1, y = 1 → tint fragments by cascade (debug)
        // sky/fog additions — must match scene.frag Frame block exactly
        glm::vec4  sunDir;    // xyz = sun direction, w = 0
        glm::vec4  fog;       // x = fogDensity, y = fogHeightFalloff, zw = 0
        glm::vec4  viewport;  // x=W, y=H, z=ssaoEnabled(1.0), w=0
        glm::vec4  giGridOrigin; // xyz = probe grid origin, w = spacing
        glm::vec4  giGridCounts; // xyz = probe counts, w = probesPerRow
        glm::vec4  giParams;     // x = indirectIntensity, y = giEnabled(1.0), zw = 0
        // Forward SSR (docs/ssr-cross-backend-plan.md B5): x = the gate,
        // y = intensity, z = max roughness, w = 0. Same four lanes heLitP reads
        // out of Lighting::ssr, so a graph material and a built-in one next to
        // each other mix the same reflection the same way.
        glm::vec4  ssrParams;
        // Local (point/spot) shadow atlas — must match scene.frag's Frame block
        // exactly: per-layer light view-proj (kVulkanClipFix pre-applied),
        // x = 1 / atlas size. Appended last.
        glm::mat4  localShadowVP[16];
        glm::vec4  localShadowParams;
        // Clustered lighting — must match scene.frag's Frame block exactly:
        // x/y/z = grid dims (0 = off), w = slice scale; camFwd.xyz = depth
        // axis, w = near (HE::ClusterLightBuild). Appended last.
        glm::vec4  clusterParams;
        glm::vec4  clusterCamFwd;
        // rgb = RenderWorld::ambient, read only by scene.frag's GI branch
        // (Thema 159) — must match scene.frag's Frame block. Appended last.
        glm::vec4  ambient;
    };

    // Sky pass UBO of the FALLBACK shader (set=0 binding=0 in shaders/sky.frag)
    // — must match std140 exactly. The GL sky compiled at startup reads a whole
    // HE::SkyFrameParams instead (kSkyVulkanPrelude); see createSkyPipeline.
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
    // The per-frame sky UBO is sized for the larger layout, so either shader fits.
    constexpr VkDeviceSize kSkyUBOBytes = std::max(sizeof(SkyUBOData), sizeof(HE::SkyFrameParams));

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
    const auto initT0 = std::chrono::steady_clock::now();
    m_sdlWindow = window->GetNativeWindow();
    createInstance();       HE_LOG_INFO(RHI, "%s", "VulkanRenderer: instance created");
    createSurface();        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: surface created");
    pickPhysicalDevice();   HE_LOG_INFO(RHI, "%s", "VulkanRenderer: physical device selected");
    createDevice();         HE_LOG_INFO(RHI, "%s", "VulkanRenderer: logical device created");
    createPipelineCache();  // before the first vkCreate*Pipelines (REN-808)
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
    // The deferred path (Thema 150) after the scene pipeline (material resources,
    // cluster SSBOs) and the PostFX pipelines (m_postFxSceneRP): the flag it
    // feeds is read together with m_postFxReady (GetCapabilities).
    createDeferredPipelines();
    // Deferred-path debug/headless knobs, GL's, Metal's and D3D's:
    // HE_RENDER_PATH=1/deferred forces the path without touching config,
    // HE_DUMP_GBUFFER=1..4 seeds a raw G-buffer view (the editor pushes its own
    // view mode every frame, the packaged game never does).
    if (const char* rp = std::getenv("HE_RENDER_PATH"); rp && *rp)
        m_renderPath = (std::string(rp) == "1" || std::string(rp) == "deferred")
            ? HE::RenderPath::Deferred : HE::RenderPath::Forward;
    if (const char* dv = std::getenv("HE_DUMP_GBUFFER"); dv && *dv)
        if (const int n = std::clamp(std::atoi(dv), 0, 4); n > 0)
            m_viewMode = static_cast<HE::ViewMode>(
                static_cast<int>(HE::ViewMode::GBufferBaseColor) + n - 1);
	m_shaderManager = VulkanShaderManager();
    // The built-in pipelines are all made by now; graph materials follow at their
    // first draw (or in the warm-up) and only show up in the shutdown line.
    HE_LOG_INFO(RHI, "[startup] Vulkan Initialize: ms=%.1f pipelines=%u pipelineMs=%.1f cache=%s",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initT0).count(),
                m_pipelinesCreated, m_pipelineMs, m_pipelineCacheState);
}

void VulkanRenderer::Shutdown()
{
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: shutdown — waiting for GPU");
    if (m_device) deviceWaitIdle();
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
    // Before the render passes its framebuffers were made against go.
    destroyWorldPreview();
    destroySkyPipeline();
    destroyDebugLinePipeline();
    destroyRibbonMeshes();
    destroyPostFXResources();      // the G-buffer targets with them
    destroyDeferredPipelines();    // before the material pipelines' render passes go
    destroyPostFXPipelines();
    // SSAO viewport-size targets (already freed by destroyViewportResources below,
    // but call explicitly in case viewport was never created).
    destroySSAOTargets();
    destroyGiAccel();
    destroyGiReflTargets();
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
    if (m_giGBufInstancedPipe) { vkDestroyPipeline(m_device, m_giGBufInstancedPipe, nullptr); m_giGBufInstancedPipe = VK_NULL_HANDLE; }
    if (m_giTemporalPipe){ vkDestroyPipeline(m_device, m_giTemporalPipe, nullptr); m_giTemporalPipe = VK_NULL_HANDLE; }
    if (m_giAtrousPipe)     { vkDestroyPipeline(m_device, m_giAtrousPipe, nullptr);     m_giAtrousPipe = VK_NULL_HANDLE; }
    if (m_giShadowPipe)   { vkDestroyPipeline(m_device, m_giShadowPipe, nullptr);   m_giShadowPipe = VK_NULL_HANDLE; }
    if (m_giProbePipe)    { vkDestroyPipeline(m_device, m_giProbePipe, nullptr);    m_giProbePipe = VK_NULL_HANDLE; }
    if (m_giGBufRP)       { vkDestroyRenderPass(m_device, m_giGBufRP, nullptr);     m_giGBufRP = VK_NULL_HANDLE; }
    if (m_giTemporalRP)   { vkDestroyRenderPass(m_device, m_giTemporalRP, nullptr); m_giTemporalRP = VK_NULL_HANDLE; }
    if (m_giAtrousRP)       { vkDestroyRenderPass(m_device, m_giAtrousRP, nullptr);     m_giAtrousRP = VK_NULL_HANDLE; }
    if (m_giShadowPL)     { vkDestroyPipelineLayout(m_device, m_giShadowPL, nullptr); m_giShadowPL = VK_NULL_HANDLE; }
    if (m_giProbePL)      { vkDestroyPipelineLayout(m_device, m_giProbePL, nullptr);  m_giProbePL = VK_NULL_HANDLE; }
    if (m_giFsPL)         { vkDestroyPipelineLayout(m_device, m_giFsPL, nullptr);     m_giFsPL = VK_NULL_HANDLE; }
    if (m_giAtrousPL)     { vkDestroyPipelineLayout(m_device, m_giAtrousPL, nullptr); m_giAtrousPL = VK_NULL_HANDLE; }
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
    // GI reflections: own pool/layouts/pipelines/render pass (createGiReflPipelines).
    for (GiBuffer& b : m_giReflUBO)
    {
        if (b.mapped) { vkUnmapMemory(m_device, b.mem); b.mapped = nullptr; }
        if (b.buf)    { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        if (b.mem)    { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
        b.size = 0;
    }
    if (m_giReflHwPipe)   { vkDestroyPipeline(m_device, m_giReflHwPipe, nullptr);   m_giReflHwPipe = VK_NULL_HANDLE; }
    if (m_giReflHwPL)     { vkDestroyPipelineLayout(m_device, m_giReflHwPL, nullptr); m_giReflHwPL = VK_NULL_HANDLE; }
    if (m_giReflHwDSL)    { vkDestroyDescriptorSetLayout(m_device, m_giReflHwDSL, nullptr); m_giReflHwDSL = VK_NULL_HANDLE; }
    if (m_giReflHwPool)   { vkDestroyDescriptorPool(m_device, m_giReflHwPool, nullptr); m_giReflHwPool = VK_NULL_HANDLE; }
    for (auto& hs : m_giReflHwSet) hs = VK_NULL_HANDLE;
    if (m_giReflPipe)     { vkDestroyPipeline(m_device, m_giReflPipe, nullptr);     m_giReflPipe = VK_NULL_HANDLE; }
    if (m_giReflBlurPipe) { vkDestroyPipeline(m_device, m_giReflBlurPipe, nullptr); m_giReflBlurPipe = VK_NULL_HANDLE; }
    if (m_giReflMixPipe)  { vkDestroyPipeline(m_device, m_giReflMixPipe, nullptr);  m_giReflMixPipe = VK_NULL_HANDLE; }
    if (m_giReflFsRP)     { vkDestroyRenderPass(m_device, m_giReflFsRP, nullptr);   m_giReflFsRP = VK_NULL_HANDLE; }
    if (m_giReflPL)       { vkDestroyPipelineLayout(m_device, m_giReflPL, nullptr);   m_giReflPL = VK_NULL_HANDLE; }
    if (m_giReflFsPL)     { vkDestroyPipelineLayout(m_device, m_giReflFsPL, nullptr); m_giReflFsPL = VK_NULL_HANDLE; }
    if (m_giReflDSL)      { vkDestroyDescriptorSetLayout(m_device, m_giReflDSL, nullptr);   m_giReflDSL = VK_NULL_HANDLE; }
    if (m_giReflFsDSL)    { vkDestroyDescriptorSetLayout(m_device, m_giReflFsDSL, nullptr); m_giReflFsDSL = VK_NULL_HANDLE; }
    if (m_giReflDescPool) { vkDestroyDescriptorPool(m_device, m_giReflDescPool, nullptr);   m_giReflDescPool = VK_NULL_HANDLE; }
    for (auto& s : m_giReflSet) s = VK_NULL_HANDLE;
    m_giReflTabRaw = m_giReflTabPing = m_giReflTabBlur = m_giReflTabMix = VK_NULL_HANDLE;
    m_giReflPipesTried = false;
    m_giReflWhyNot.clear();
    m_giReflReady      = false;
    if (m_giPointSampler) { vkDestroySampler(m_device, m_giPointSampler, nullptr);          m_giPointSampler = VK_NULL_HANDLE; }
    m_giPipelinesTried = false;
    m_giReady          = false;
    // SSAO pipeline-level resources (static, not viewport-size-dependent).
    if (m_ssaoPosGfxPipeline)   { vkDestroyPipeline      (m_device, m_ssaoPosGfxPipeline,   nullptr); m_ssaoPosGfxPipeline   = VK_NULL_HANDLE; }
    if (m_ssaoPosInstancedPipeline) { vkDestroyPipeline  (m_device, m_ssaoPosInstancedPipeline, nullptr); m_ssaoPosInstancedPipeline = VK_NULL_HANDLE; }
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
    // UI canvas pipeline + font atlases + quad images
    destroyUIFontAtlases();
    destroyUIImages();
    if (m_uiImageDescPool)    { vkDestroyDescriptorPool (m_device, m_uiImageDescPool,   nullptr); m_uiImageDescPool   = VK_NULL_HANDLE; }
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
        m_frameFence[i] = VK_NULL_HANDLE;   // GetCompletedFrameSerial() skips null fences
    }
    for (VkSemaphore s : m_renderDone) if (s) vkDestroySemaphore(m_device, s, nullptr);
    m_renderDone.clear();
    m_imagesInFlight.clear();   // fences are owned by m_frameFence — don't destroy here
    // Scene resources
    for (auto& [id, mesh] : m_meshCache)
    {
        destroyPooledBuffer(mesh.vbuf, mesh.valloc);
        destroyPooledBuffer(mesh.ibuf, mesh.ialloc);
        // Base-color texture (descriptor set is freed with m_albedoPool in destroyScenePipeline).
        if (mesh.albedoView)  vkDestroyImageView(m_device, mesh.albedoView,  nullptr);
        if (mesh.albedoImage) vkDestroyImage    (m_device, mesh.albedoImage, nullptr);
        if (mesh.albedoMem)   vkFreeMemory      (m_device, mesh.albedoMem,   nullptr);
    }
    m_meshCache.clear();
    destroyPooledBuffer(m_cube.vbuf, m_cube.valloc);
    destroyPooledBuffer(m_cube.ibuf, m_cube.ialloc);
    m_cube = {};
    // Override-material textures (before destroyScenePipeline destroys m_albedoPool, since
    // destroyMaterialTex frees descriptor sets from it). Device is already idle here.
    for (auto& [id, mt] : m_materialTexCache) destroyMaterialTex(mt);
    m_materialTexCache.clear();
    for (auto& [key, mt] : m_graphTexCache) destroyMaterialTex(mt);
    m_graphTexCache.clear();
    m_pendingMatInval.clear();
    m_pendingMeshInval.clear();
    m_pendingTexInval.clear();
    m_pendingMatWarmup.clear();
    destroyDecalPipelines(); // before destroyScenePipeline — it borrows m_albedoSampler
    // Forward SSR: pipelines, passes, ring buffers and every target it owns.
    // Before destroySSAOTargets below, because the pre-pass framebuffer holds
    // SSAO's position and depth views.
    destroySSRPipelines();
    destroyScenePipeline();

    destroyViewportResources();
    for (auto& r : m_retiredViewports)   // flush any pending retired viewport images
    {
        if (r.view) vkDestroyImageView(m_device, r.view, nullptr);
        if (r.img)  vkDestroyImage    (m_device, r.img,  nullptr);
        if (r.mem)  vkFreeMemory       (m_device, r.mem,  nullptr);
    }
    m_retiredViewports.clear();
    for (auto& r : m_retiredMeshes) destroyGpuMesh(r.mesh);   // flush pending retired meshes
    m_retiredMeshes.clear();
    destroyShadowResources();
    destroyUploadResources(); // after every pooled mesh buffer is gone, before the device
    if (m_cmdPool)     vkDestroyCommandPool(m_device,   m_cmdPool,     nullptr);
    if (m_renderPass)  vkDestroyRenderPass (m_device,   m_renderPass,  nullptr);
    destroySwapchain();
    savePipelineCache();     // every pipeline is built by now; ImGui shut down before us
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
    // Every pass below that needs the scene (cascades, decal depth pre-pass, GI, SSAO, the scene
    // itself) extracts on its own so its draw set and cascade fit agree with the others — five
    // full walks of the registry a frame, ~26 ms each at 50k entities. Inside this scope the world
    // does not change, so the extractor walks it once and the other four find that walk in
    // m_renderWorld (RenderExtractor::beginFrame). Closed on every return. Exactly two of these
    // exist, here and in RenderSceneImage(): a third one inside DrawViewportFrame would end the
    // outer scope early (tests/test_culling.cpp pins it).
    RenderExtractor::FrameScope extractOncePerFrame(m_extractor);

    m_wallTime = static_cast<float>(SDL_GetTicks()) * 0.001f;
    ++m_frameStamp;
    instanceWorldCacheNextFrame();   // frees the unfolded foliage blocks nothing read for a while
    m_ssaoRanThisFrame = false;  // cleared each frame; set true only inside runSSAO()
    m_giRanThisFrame   = false;  // cleared each frame; set true only inside runGi()
    m_ssrRanThisFrame  = false;  // cleared each frame; set true only by RenderForwardSSR()
    m_ssrResultView    = VK_NULL_HANDLE;
    m_statDraws = m_statTris = m_statVisible = m_statTotal = 0;  // rebuilt by DrawScene
    beginInstanceFrame(); // this frame's instance-buffer slots start from the top
    m_sceneMatCursor = 0; m_sceneMatOffset = 0; // ...and its built-in material-ring slots

    // This frame's sun, once, before the first extraction. The cascades, the
    // decal depth pre-pass, GI and SSAO all extract on their own ahead of
    // DrawScene(); with the day-night state fed only there they fit and traced
    // against the previous frame's sun (Thema 131 for GI, 146 for the rest).
    // RenderSceneImage() does the same at its top.
    m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                            m_environment.sunColor, m_environment.sunIntensity,
                            m_environment.moonColor, m_environment.moonIntensity,
                            m_environment.cloudCoverage);

    // Drop caches for materials/meshes edited since last frame, before any recording — the
    // frame's DrawScene then re-resolves them fresh from the ContentManager.
    processPendingInvalidations();

    for (auto it = m_retiredHf.begin(); it != m_retiredHf.end(); )
    {
        if (--it->frames <= 0)
        {
            if (it->view) vkDestroyImageView(m_device, it->view, nullptr);
            if (it->img)  vkDestroyImage(m_device, it->img, nullptr);
            if (it->mem)  vkFreeMemory(m_device, it->mem, nullptr);
            it = m_retiredHf.erase(it);
        }
        else ++it;
    }
    // Retired meshes (processPendingInvalidations) go the same way.
    for (auto it = m_retiredMeshes.begin(); it != m_retiredMeshes.end(); )
    {
        if (--it->frames <= 0) { destroyGpuMesh(it->mesh); it = m_retiredMeshes.erase(it); }
        else ++it;
    }
    for (auto it = m_retiredGiBufs.begin(); it != m_retiredGiBufs.end(); )
    {
        if (--it->frames <= 0) { destroyGiBuffer(it->buf); it = m_retiredGiBufs.erase(it); }
        else ++it;
    }
    // ... and so do replaced hardware BLAS and the input/scratch of builds the GPU has finished.
    for (auto it = m_retiredHwBlas.begin(); it != m_retiredHwBlas.end(); )
    {
        if (--it->frames <= 0) { destroyGiHwBlas(it->blas); it = m_retiredHwBlas.erase(it); }
        else ++it;
    }
    for (auto it = m_giHwBlasTemp.begin(); it != m_giHwBlasTemp.end(); )
    {
        if (--it->frames <= 0) { destroyGiHwBuffer(it->geo); destroyGiHwBuffer(it->scratch); it = m_giHwBlasTemp.erase(it); }
        else ++it;
    }
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
    // The instance arena's overflow chunks and replaced buffers, the same way.
    for (auto it = m_instRetired.begin(); it != m_instRetired.end(); )
    {
        if (--it->frames <= 0)
        {
            destroyInstanceBuf(it->b);
            it = m_instRetired.erase(it);
        }
        else ++it;
    }

    // The packaged game (SetSwapchainPostProcessing): the post chain — HDR,
    // bloom, tonemap, AA, TAA, SSR, and on Vulkan GI and SSAO too — exists only
    // in the viewport frame, so the game runs that frame at the swapchain's
    // size and draws the finished RGBA8 image into the swapchain pass below.
    // Sized from m_swapExtent (physical pixels), not the window; a recreate at
    // acquire/present time is picked up here on the next frame. An editor
    // request (HE_CAPTURE_FRAME asks for one at the window's logical size) is
    // ignored meanwhile, and the capture reads this very image. An sRGB
    // swapchain is left on the old path: the present pass would encode the
    // already gamma-encoded image a second time.
    const bool swapIsSrgb = m_swapFormat == VK_FORMAT_B8G8R8A8_SRGB
                         || m_swapFormat == VK_FORMAT_R8G8B8A8_SRGB
                         || m_swapFormat == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    // A minimised window reports a 0×0 surface: the set stays as it is (no
    // rebuild, no teardown) and the frame below falls through to the direct
    // path, which is what that frame did before.
    const bool gameChain = m_swapchainPostFx && m_postFxReady && m_presentPipe && m_presentDS
                        && !swapIsSrgb;
    if (gameChain)
    {
        if (m_swapExtent.width > 0 && m_swapExtent.height > 0
            && (!m_gameViewport || m_viewportW != m_swapExtent.width || m_viewportH != m_swapExtent.height))
        {
            deviceWaitIdle();
            createViewportResources(m_swapExtent.width, m_swapExtent.height);
            m_gameViewport = true;
            if (m_viewportImage && m_viewportW == m_swapExtent.width && m_viewportH == m_swapExtent.height)
                HE_LOG_INFO(RHI, "VulkanRenderer: swapchain post chain active (%ux%u)",
                            m_swapExtent.width, m_swapExtent.height);
        }
    }
    else if (m_gameViewport)
    {
        // The game stopped asking (application mode): drop its set so the
        // branch below draws straight into the swapchain again. Scene binding 3
        // pointed at the SSAO blur target that goes with it, and the temporal
        // histories would blend a stale world when the chain returns. One wait,
        // on the switch only.
        deviceWaitIdle();
        destroyViewportResources();
        pointSceneAoAtWhite();
        m_gameViewport      = false;
        m_taaHistoryValid   = false;
        m_ssrColorHistValid = false;
        m_ssrHistValid      = false;
    }
    else if (m_swapchainPostFx && swapIsSrgb)
    {
        static bool s_srgbLogged = false;
        if (!s_srgbLogged)
        {
            s_srgbLogged = true;
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: sRGB-only swapchain — the game runs without its post chain");
        }
    }

    // Resize viewport resources if the editor requested a different size.
    if (!gameChain && m_viewportReqW > 0 && m_viewportReqH > 0 &&
        (m_viewportReqW != m_viewportW || m_viewportReqH != m_viewportH))
    {
        deviceWaitIdle();
        createViewportResources(m_viewportReqW, m_viewportReqH);
    }
    // TAA targets to the mode in force — before any command of this frame
    // names them; only waits for the GPU on a toggle, never per frame.
    syncTaaTargets();

    // Project shadow resolution changed (SetShadowSettings): rebuild the
    // cascade images here, before any command of this frame names them and
    // after the GPU is idle — the frames still in flight sample the old
    // array — then repoint every scene set at the new view. Same shape as
    // the viewport resize above; the render pass and sampler stay.
    if (m_shadowSizeDirty && m_shadowPass)
    {
        deviceWaitIdle();
        destroyShadowImages();
        m_shadowSize = static_cast<uint32_t>(std::clamp(m_shadowSettings.resolution, 256, 8192));
        createShadowImages();
        writeShadowDescriptors();
        m_shadowSizeDirty = false;
    }

    // The game frame needs the full HDR set: without it DrawViewportFrame takes
    // its LDR fallback, whose pipelines are built against the swapchain pass.
    const bool gameFrame   = gameChain && m_viewportImage && m_hdrFB && m_ldrFB && m_fxaaFB
                          && m_viewportW == m_swapExtent.width && m_viewportH == m_swapExtent.height;
    const bool useViewport = !gameChain && m_viewportImage != VK_NULL_HANDLE
                          && m_viewportW > 0 && m_viewportH > 0;

    const uint32_t fi = m_currentFrame;

    {
        HE_PROFILE_SCOPE_N("WaitFrame");   // the CPU idles here when the GPU is the slower side
        vkWaitForFences(m_device, 1, &m_frameFence[fi], VK_TRUE, UINT64_MAX);
    }

    uint32_t imageIndex = 0;
    VkResult acq;
    {
        HE_PROFILE_SCOPE_N("AcquireImage");
        acq = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                    m_imageReady[fi], VK_NULL_HANDLE, &imageIndex);
    }
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

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };

    if (useViewport || gameFrame)
    {
        // The whole offscreen half of the frame — cascades, decal depth pre-pass,
        // GI/SSAO/SSR, scene, PostFX, AA resolve, UI canvas. Lives in its own
        // method because RenderSceneImage() records exactly this and nothing of
        // the swapchain part below.
        DrawViewportFrame(cmd);
        if (gameFrame)
        {
            // The image is SHADER_READ_ONLY already (final AA / UI pass), but
            // neither pass has a dependency to EXTERNAL that makes its writes
            // visible to the present pass's fragment shader — and
            // runPostFXBarrier skips a barrier whose layouts match.
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_viewportImage;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
    }
    else
    {
        // No viewport: the scene goes straight into the swapchain pass below, so
        // the cascades are fit to the swapchain aspect and the decal pre-pass
        // fills the swapchain-sized depth. The same two calls as before the
        // split, with `useViewport` resolved to false. No post chain on this
        // path, so nothing would resolve a TAA jitter.
        m_taaFrame = false;
        updateSkyEnvCube(cmd); // before any pass — graph materials' heSkyEnv
        const float shadowAspect = float(std::max(m_swapExtent.width,  1u))
                                 / float(std::max(m_swapExtent.height, 1u));
        extractFrame(shadowAspect); // the frame's one walk; the cascade pass finds it in m_renderWorld
        if (m_world) refineObjectBounds();   // ...and the real bound of every object, once
        EncodeShadowMap(cmd, shadowAspect);
        m_decalDepthActive = &m_decalDepth;
        EncodeDecalDepth(cmd, m_decalDepth);
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
    if (gameFrame)
    {
        // The game's frame, finished (UI canvas included): one fullscreen
        // triangle samples it into the swapchain image — the format swizzle
        // RGBA8 → swapchain format happens in the write. Same size, so every
        // fragment lands on a texel centre. The overlay (if any) draws on top.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_presentPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_postFxPipeLayout, 0, 1, &m_presentDS, 0, nullptr);
        const float p[4] = { 0, 0, 0, 0 };
        vkCmdPushConstants(cmd, m_postFxPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, p);
        VkViewport vp{ 0, 0, float(m_swapExtent.width), float(m_swapExtent.height), 0, 1 };
        VkRect2D   sc{ { 0, 0 }, m_swapExtent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd,  0, 1, &sc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    else if (!useViewport)
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
    if (m_overlayCallback)
    {
        HE_PROFILE_SCOPE_N("Overlay");   // the editor's ImGui draw lists
        m_overlayCallback(cmd);
    }
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
    m_frameSerial[fi] = ++m_submitSerial;
    {
        HE_PROFILE_SCOPE_N("Submit");
        submitGraphics(si, m_frameFence[fi]);
    }

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderDone[imageIndex];     // per-image present semaphore
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &imageIndex;
    VkResult pres;
    {
        HE_PROFILE_SCOPE_N("Present");
        pres = vkQueuePresentKHR(m_graphicsQueue, &pi);
    }
    // OUT_OF_DATE/SUBOPTIMAL after present: the image was still shown, but the
    // swapchain no longer matches the surface — rebuild for the next frame.
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
        recreateSwapchain();
    else if (pres != VK_SUCCESS)
        vkCheck(pres, "vkQueuePresentKHR");

    m_currentFrame = (m_currentFrame + 1) % k_maxFramesInFlight;
}

// ─── The offscreen half of a frame ───────────────────────────────────────────
// Everything Render() records when the editor's viewport target exists, and
// nothing that belongs to the window: no acquire, no backbuffer pass, no ImGui
// overlay, no timestamp pair, no present. Split out of Render() unchanged (only
// `useViewport` folded away, which is true for every caller) so that
// RenderSceneImage() can record the same frame into a one-shot command buffer
// from somebody else's camera. Same cut as D3D11Renderer::DrawViewportFrame and
// D3D12Renderer::DrawViewportFrame.
//
// Draws at m_viewportW × m_viewportH into m_viewportImage; the caller has
// already made sure the viewport resources exist at that size.
void VulkanRenderer::DrawViewportFrame(VkCommandBuffer cmd)
{
    GpuPassScope _frame(this, "Scene");   // the passes inside it (shadow-free: SSAO, GI, SSR, decals) interrupt it
    // The shadow, GI and SSAO depth passes and the geometry pass below all take
    // instance-buffer slots from this frame's arena (beginInstanceFrame). Reset here too, not
    // only in Render(): RenderSceneImage records this frame on its own. Same for
    // the built-in material ring (m_sceneMatBuf) the geometry pass fills.
    beginInstanceFrame();
    m_sceneMatCursor = 0;
    m_sceneMatOffset = 0;

    const bool useHDR = m_postFxReady && m_hdrFB && m_ldrFB && m_fxaaFB;

    // ── TAA: this frame's jitter (A2) ───────────────────────────────────────
    // Chosen HERE, before the first pass: the decal depth pre-pass, the sky and
    // the scene each rebuild the camera matrix from their own extraction, and
    // all of them must rasterise with the same offset. Only a frame whose post
    // chain resolves it (the HDR branch below) is jittered; the targets were
    // synced to the mode at the top of Render (syncTaaTargets).
    m_taaFrame = useHDR && taaWanted() && m_taaHistoryFB[0] && m_taaVelocityFB
              && m_taaW == m_viewportW && m_taaH == m_viewportH;
    if (m_taaFrame)
        m_taaJitter = HE::taaJitter(m_taaFrameIndex++);
    m_taaSceneSorted = false;
    // ── Deferred render path (Thema 150): decided before the first pass, so a
    // deferred frame can skip the decal depth pre-pass below (its decals read
    // GB3). Only on the HDR frame, like SSR and TAA. The G-buffer is built
    // lazily at the viewport size; a failed allocation keeps the frame forward
    // (one log line).
    m_deferredFrame = useHDR && m_renderPath == HE::RenderPath::Deferred && m_deferredReady && m_matReady
                   && ensureGBufferTargets(m_viewportW, m_viewportH);

    // Graph materials' sky cube (heSkyEnv): a transfer, so before every pass.
    updateSkyEnvCube(cmd);

    // Cascade shadow maps first, in their own render passes (before the scene
    // pass), fit against the aspect of the target the scene will be drawn into.
    // The frame's one walk of the world comes right before them (extractFrame), with the
    // real bound of every object: the cascade pass reads both from m_renderWorld, as every
    // pass after it does.
    const float shadowAspect = float(m_viewportW) / float(m_viewportH);
    extractFrame(shadowAspect);
    if (m_world) refineObjectBounds();
    EncodeShadowMap(cmd, shadowAspect);

    // Camera-depth pre-pass for screen-space decals. It must run before the scene
    // pass opens, because the decal draw sits INSIDE that pass and samples this
    // image — the pass's own depth attachment cannot be sampled while bound.
    // Skips itself when the frame has no decals. A deferred frame needs none:
    // its decals reconstruct from GB3, which the G-buffer pass writes anyway.
    m_decalDepthActive = m_deferredFrame ? nullptr : &m_decalDepthVp;
    if (!m_deferredFrame)
        EncodeDecalDepth(cmd, m_decalDepthVp);

    VkClearValue clears[2]{};
    clears[0].color        = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };

    if (useHDR)
    {
        // ── Ray-traced GI (software compute): G-buffer + shadow rays +
        // temporal + blur + probe update. Extracts the scene itself.
        // Replaces the CSM lookup AND SSAO in scene.frag when it runs.
        runGi(cmd, m_viewportW, m_viewportH);

        // ── Deferred (Thema 150, decided at the top): a deferred frame runs the
        // SAME forward trace below, and the resolve composites it through
        // heLitP's heSSRFwd stage (binding 31), the stage graph materials
        // already use here (step 5, plan §10.12).

        // ── Forward SSR (docs/ssr-cross-backend-plan.md checkpoint B) ──
        // Built lazily, and only here: the trace's radiance source is the
        // HDR target of THIS branch. The packaged game reaches it too when it
        // asks for the chain (SetSwapchainPostProcessing, plan C6 closed); only
        // the direct swapchain fallback (application mode, sRGB-only
        // swapchain) has no SSR.
        if (m_ssrEnabled) EnsureSSRPipelines();
        const bool ssrWanted = ssrWantedThisFrame();

        // ── SSAO position prepass + occlusion compute + blur ───────────
        // runSSAO() extracts the scene itself (like EncodeShadowMap) so it
        // doesn't depend on DrawScene having run first. Skipped when GI
        // shades this frame (probe indirect replaces AO) — but the pass runs
        // anyway when SSR wants the reflection MRT out of it, which is the
        // whole point of the aoWanted flag.
        const bool aoWanted = !m_giRanThisFrame && m_ssaoEnabled;
        if (aoWanted || ssrWanted)
            runSSAO(cmd, m_viewportW, m_viewportH, /*reflMrt=*/ssrWanted, aoWanted);

        // ── SSR trace + blur chain (own render passes, before the scene) ─
        if (ssrWanted)
        {
            m_ssrResultView  = RenderForwardSSR(cmd, m_viewportW, m_viewportH);
            m_ssrRanThisFrame = m_ssrResultView != VK_NULL_HANDLE;
        }

        // ── Scene → HDR RT (RGBA16F) ───────────────────────────────────
        // A deferred frame leaves this pass after the sky: DrawScene ends it,
        // runs the G-buffer pass and reopens the HDR target as m_hdrLoadRP for
        // the resolve and the forward tail — the pass the End below closes.
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

        // ── TAA velocity (A2): its own pass right after the scene, against the
        // depth the scene pass stored. Runs on every TAA frame, even an empty
        // one — the pass's clear is what keeps last frame's motion out of the
        // resolve.
        if (m_taaFrame) encodeTaaVelocity(cmd);

        // Forward SSR: keep a full-res copy of the finished HDR frame
        // (opaque + sky + transparency) — NEXT frame's trace reprojects its
        // hits into it. Taken here, at the very end of the scene pass, for
        // the same reason Metal takes it after its scene encoder and GL at
        // the end of the geometry pass: earlier and the reflection would
        // show a half-drawn world.
        if (ssrWanted) CaptureSSRColorHistory(cmd, m_viewportW, m_viewportH);

        // Underwater fog (Thema 185 Schritt 2): straight onto m_hdrView while it
        // is still COLOR_ATTACHMENT_OPTIMAL (before the barrier below hands it to
        // bloom as a sampled image) — alpha-blended, no ping-pong target.
        // Skipped whole on a dry frame: the only cost a scene without water
        // pays here is this one branch. See postfx_underwaterfog.frag for the
        // per-pixel math (kUnderwaterFogHLSL's GLSL twin).
        // Gated on the two knobs, not wet: density stays above 0 for a dry camera in the
        // waterline band just above the surface (HE::fogWaterlineBand, Thema 185 Schritt 5),
        // and the net (drawn inside this same render pass) is 0 whenever dry. A fog draw at
        // density 0 blends nothing (premultiplied, alpha 0).
        const UnderwaterCameraData& uwCam = m_renderWorld.underwaterCamera;
        if ((uwCam.density > 0.0f || uwCam.causticsIntensity > 0.0f)
            && m_underwaterFogReady && m_underwaterFogFB && m_viewportDepthImage)
        {
            // The matrix the scene was RASTERISED with (kVulkanClipFix: NDC y down, depth 0..1,
            // line ~7842), inverted — what the decal, SSR and deferred-resolve passes reconstruct
            // with, and what makes depthParams 1 / 1 / 0 below a valid Vulkan NDC point. The plain
            // projection * view (GL clip) put every reconstructed position metres off (Thema 185
            // Schritt 4: numeric check on three points, 8 to 14 m), so the fog's segment lengths
            // were wrong on any Vulkan frame under water.
            const glm::mat4 viewProjUw = HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;
            const glm::mat4 invViewProjUw = glm::inverse(viewProjUw);
            struct UnderwaterFogPC
            {
                float invViewProj[16];
                float camPosSurfaceY[4];
                float tintDensity[4];
                float surfWave[4];
                float surfParams[4];
            } pc{};
            static_assert(sizeof(UnderwaterFogPC) == 128,
                          "must match the push-constant block of postfx_underwaterfog.frag and the 128-byte range");
            std::memcpy(pc.invViewProj, &invViewProjUw[0][0], 16 * sizeof(float));
            pc.camPosSurfaceY[0] = m_renderWorld.camera.position.x;
            pc.camPosSurfaceY[1] = m_renderWorld.camera.position.y;
            pc.camPosSurfaceY[2] = m_renderWorld.camera.position.z;
            pc.camPosSurfaceY[3] = uwCam.surfaceY;
            pc.tintDensity[0] = uwCam.tintColor.x; pc.tintDensity[1] = uwCam.tintColor.y;
            pc.tintDensity[2] = uwCam.tintColor.z; pc.tintDensity[3] = uwCam.density;
            {
                static const char* s_uwTimeOv = std::getenv("HE_SKY_TIME");   // pins the wave clock for captures
                const float uwSeconds = (s_uwTimeOv && *s_uwTimeOv) ? static_cast<float>(std::atof(s_uwTimeOv))
                                                                    : static_cast<float>(SDL_GetTicks()) / 1000.0f;
                const HE::UnderwaterSurfaceUniforms su = HE::underwaterSurfaceUniforms(uwCam, uwSeconds);
                std::memcpy(pc.surfWave, su.wave, sizeof(su.wave));
                std::memcpy(pc.surfParams, su.params, sizeof(su.params));
            }

            // Depth off the attachment before it is readable as a sampler — the
            // same flip EncodeDecalDepth's consumers use, just on the live
            // viewport depth instead of a ping-pong copy (nothing else this
            // frame still needs it as an attachment: bloom/tonemap/FXAA below
            // are all colour-only). Not runPostFXBarrier: that helper hardcodes
            // the COLOR aspect and knows nothing about depth-attachment layouts.
            auto depthBarrier = [&](VkImageLayout from, VkImageLayout to,
                                    VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
            {
                VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                b.oldLayout = from; b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = m_viewportDepthImage;
                b.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
                vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
            };
            depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            bi.renderPass = m_underwaterFogRP; bi.framebuffer = m_underwaterFogFB;
            bi.renderArea.extent = { m_viewportW, m_viewportH };
            vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

            // Underwater caustics (Thema 185 Schritt 3), drawn FIRST in this same pass so the
            // fog below swallows the net with distance: light redistributed on m_hdrView by the
            // fog's own blend state (ONE / ONE_MINUS_SRC_ALPHA: dst' = rgb + dst * (1 - a), the
            // cells' darkening in a, Thema 205). Two draws to one attachment in one subpass blend in primitive
            // order, so nothing sits between them; the depth is already flipped to
            // SHADER_READ_ONLY for the fog and the caustics read the very same set. Runs only
            // with the net switched on (the extractor's waterline fade-in makes
            // causticsIntensity exactly 0 on the line). See postfx_underwatercaustics.frag.
            if (uwCam.causticsIntensity > 0.0f && m_underwaterCausticsReady)
            {
                glm::vec3 ucSunDir, ucSunColor;
                m_renderWorld.dominantDirectionalLight(ucSunDir, ucSunColor);   // colour is 0 at night
                const glm::vec3 towardSun = glm::normalize(m_renderWorld.sunDirection);
                static const char* s_ucTimeOv = std::getenv("HE_SKY_TIME");   // pins the wave clock for captures
                const float ucSeconds = (s_ucTimeOv && *s_ucTimeOv) ? static_cast<float>(std::atof(s_ucTimeOv))
                                                                    : static_cast<float>(SDL_GetTicks()) / 1000.0f;
                // The matrix the scene was rasterised with (kVulkanClipFix: NDC y down, depth
                // 0..1), inverted — the same reconstruction the decal, SSR and deferred-resolve
                // passes use, so the shader's (uv * 2 - 1, depth) with the constants 1 / 1 / 0
                // baked in is a valid Vulkan NDC point. The clean matrix, not the jittered one:
                // a TAA frame's sub-pixel offset moves P by less than a pixel.
                const glm::mat4 invViewProjUc = glm::inverse(
                    HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view);
                struct UnderwaterCausticsPC
                {
                    float invViewProj[16];
                    float camSurface[4];
                    float sun[4];
                    float sunColor[4];
                    float caustics[4];
                } ucpc{};
                static_assert(sizeof(UnderwaterCausticsPC) == 128,
                              "must match the push-constant block of postfx_underwatercaustics.frag and the 128-byte range");
                std::memcpy(ucpc.invViewProj, &invViewProjUc[0][0], 16 * sizeof(float));
                ucpc.camSurface[0] = m_renderWorld.camera.position.x;
                ucpc.camSurface[1] = m_renderWorld.camera.position.y;
                ucpc.camSurface[2] = m_renderWorld.camera.position.z;
                ucpc.camSurface[3] = uwCam.surfaceY;
                ucpc.sun[0] = towardSun.x; ucpc.sun[1] = towardSun.y; ucpc.sun[2] = towardSun.z; ucpc.sun[3] = uwCam.causticsWindTurn;
                ucpc.sunColor[0] = ucSunColor.r; ucpc.sunColor[1] = ucSunColor.g; ucpc.sunColor[2] = ucSunColor.b;
                ucpc.caustics[0] = uwCam.causticsIntensity;
                ucpc.caustics[1] = uwCam.causticsScale;
                ucpc.caustics[2] = ucSeconds * uwCam.causticsSpeed;
                ucpc.caustics[3] = uwCam.causticsDepth;

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_underwaterCausticsPipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_underwaterCausticsPipeLayout, 0, 1, &m_underwaterFogDS, 0, nullptr);
                vkCmdPushConstants(cmd, m_underwaterCausticsPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, &ucpc);
                VkViewport ucvp{0,0,(float)m_viewportW,(float)m_viewportH,0,1}; vkCmdSetViewport(cmd,0,1,&ucvp);
                VkRect2D ucsc{{0,0},{m_viewportW,m_viewportH}}; vkCmdSetScissor(cmd,0,1,&ucsc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_underwaterFogPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_underwaterFogPipeLayout, 0, 1, &m_underwaterFogDS, 0, nullptr);
            vkCmdPushConstants(cmd, m_underwaterFogPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, &pc);
            VkViewport vp{0,0,(float)m_viewportW,(float)m_viewportH,0,1}; vkCmdSetViewport(cmd,0,1,&vp);
            VkRect2D sc{{0,0},{m_viewportW,m_viewportH}}; vkCmdSetScissor(cmd,0,1,&sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);

            depthBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        }

        _frame.next("Bloom");
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

        _frame.next("Tonemap");
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

        _frame.next("TAA");
        // ── TAA resolve (A3): (ldr, history[prev], velocity) → history[cur] ─
        // On the tonemapped image, so a single bright HDR sample cannot poison
        // the next dozen frames. The AA slot below then sharpens history[cur]
        // instead of filtering ldr. Every history image is SHADER_READ_ONLY
        // outside this block (primed at creation, re-parked after each write).
        VkDescriptorSet aaSet = m_postFxDS[4];
        const bool aaTaa = m_taaFrame && m_taaHistoryFB[0]
                        && m_taaW == m_viewportW && m_taaH == m_viewportH;
        if (aaTaa)
        {
            const int cur  = m_taaHistoryCur;
            const int prev = 1 - cur;
            VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            bi.renderPass = m_postFxBlitF8; bi.framebuffer = m_taaHistoryFB[cur];
            bi.renderArea.extent = { m_viewportW, m_viewportH };
            vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaResolvePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_taaResolvePipeLayout, 0, 1, &m_taaResolveDS[cur], 0, nullptr);
            const float p[4] = { 1.0f / float(m_viewportW), 1.0f / float(m_viewportH),
                                 m_taaHistoryValid ? HE::kTaaHistoryBlend : 0.0f, 0.0f };
            vkCmdPushConstants(cmd, m_taaResolvePipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, p);
            VkViewport vp{ 0, 0, (float)m_viewportW, (float)m_viewportH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D   sc{ { 0, 0 }, { m_viewportW, m_viewportH } };          vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            runPostFXBarrier(cmd, m_taaHistoryImage[cur],
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            aaSet = m_taaSharpenDS[cur];
            // One line per session, so a capture log shows TAA really ran.
            static bool s_taaLogged = false;
            if (!s_taaLogged)
            {
                s_taaLogged = true;
                HE_LOG_INFO(RHI, "VulkanRenderer: TAA resolve active (%ux%u)", m_viewportW, m_viewportH);
            }
            // This frame's result IS next frame's history: flip the ping-pong.
            m_taaHistoryCur   = prev;
            m_taaHistoryValid = true;
        }

        _frame.next(aaTaa ? "TAA Sharpen" : "AA");
        // ── AA resolve: ldrSRV → viewportFB ───────────────────────────
        // Always runs — it is what writes m_viewportImage. The AA method
        // only decides which pipeline does it (FXAA vs. passthrough); TAA's
        // slot is the sharpen the temporal blur asks for.
        { const float p[4]={1.0f/float(m_viewportW),1.0f/float(m_viewportH),
                            aaTaa ? m_aaSharpness : 0.0f, 0};
          const VkPipeline aaPipe =
              aaTaa                                                ? m_taaSharpenPipe :
              (m_aaMethod == HE::AAMethod::Off  && m_aaBlitPipe) ? m_aaBlitPipe :
              (m_aaMethod == HE::AAMethod::SMAA && m_smaaPipe)   ? m_smaaPipe   :
                                                                   m_fxaaPipe;
          blitPass(m_postFxFinalRP, m_fxaaFB, m_viewportW, m_viewportH, aaPipe, aaSet, p); }
        m_viewportLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        _frame.next("UI");
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
    // The jitter belongs to this frame's viewport passes only; whatever draws
    // after them (a world preview, the swapchain pass — the game's present
    // blit or the direct fallback) rasterises unjittered.
    m_taaFrame = false;
    m_deferredFrame = false; // likewise the deferred path (the world previews draw forward)
}

IRenderer::Capabilities VulkanRenderer::GetCapabilities() const
{
    Capabilities c;
    c.supportsShadows        = true;
    c.supportsPostProcessing = m_postFxReady;
    // The HDR scene target lives and dies with the post chain, which the editor
    // viewport and the packaged game (SetSwapchainPostProcessing) both run.
    c.supportsHDR            = m_postFxReady;
    c.supportsGpuParticles   = false;
    // Software compute ray tracing (gi_*.comp) — compute is core Vulkan, so no
    // extension gate. If pipeline creation fails at runtime, runGi() leaves
    // m_giRanThisFrame false and rendering falls back to CSM+SSAO.
    c.supportsGlobalIllumination = true;
    // GI reflections: same software-compute story as the diffuse GI above —
    // compute is core Vulkan, no extension gate. A pipeline-creation failure
    // leaves m_giReflReady false and the pass silently stays off (giRefl.z
    // never rises), same contract as GL/D3D11/D3D12.
    c.supportsGIReflections = true;
#if defined(HE_HAVE_SHADERC)
    // SSR: the FORWARD variant only (docs/ssr-cross-backend-plan.md checkpoint
    // B) — the reflection MRT pre-pass plus last frame's HDR copy, Metal's
    // Option-A path. There is no deferred composite here because there is no
    // G-buffer. Reported alongside supportsPostProcessing on purpose: the HDR
    // target the trace reads is the PostFX scene target, which the editor
    // viewport and the packaged game (SetSwapchainPostProcessing, plan C6) both
    // run; only the direct swapchain fallback (application mode, sRGB-only
    // swapchain) has none. The shaders come from the cross-compiler, hence the #if.
    c.supportsScreenSpaceReflections = m_postFxReady;
#endif
    // TAA (A2/A3): velocity pass + temporal resolve + sharpen, on the same post
    // chain — false only if a TAA shader (.spv) or pipeline is missing. Only
    // the direct swapchain fallback renders unjittered (m_taaFrame).
    c.supportsTemporalAA = taaReady();
    // Deferred (Thema 150): G-buffer pass + resolve built at Initialize. Like
    // SSR/TAA it lives on the HDR viewport frame (DrawViewportFrame's useHDR
    // branch), which the editor viewport and the packaged game both run; the
    // direct swapchain fallback and the world previews stay forward.
    c.supportsDeferredRendering = m_postFxReady && m_deferredReady;
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

    // GPU debug instrumentation (validation layer): debug builds, or any build with HE_GPU_DEBUG
    // set. It was forced ON for every build while Vulkan still rendered black; the layer then
    // costs several times the CPU time of a descriptor-heavy frame (a 230-draw landscape took
    // 15 ms of CPU per frame, perf capture 2026-10-28), so a release run must not pay for it.
#ifdef _DEBUG
    const bool gpuDebug = true;
#else
    const bool gpuDebug = std::getenv("HE_GPU_DEBUG") != nullptr;
#endif
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
    if (haveDebugUtils)
        m_setObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(m_instance, "vkSetDebugUtilsObjectNameEXT"));
}

void VulkanRenderer::nameImage(VkImage img, const char* name) const
{
    if (!m_setObjectName || !m_device || !img) return;
    VkDebugUtilsObjectNameInfoEXT ni{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
    ni.objectType   = VK_OBJECT_TYPE_IMAGE;
    ni.objectHandle = reinterpret_cast<uint64_t>(img);
    ni.pObjectName  = name;
    m_setObjectName(m_device, &ni);
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
    // Name the device: a CI image test on a software ICD (lavapipe) must be able
    // to show in its log that "llvmpipe" — and not some other ICD — drew it.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physDevice, &props);
    char line[384];
    std::snprintf(line, sizeof line,
        "VulkanRenderer: device 0 of %u: %s (Vulkan %u.%u.%u, driver 0x%x)",
        count, props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
        props.driverVersion);
    HE_LOG_INFO(RHI, "%s", line);
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

    // The one core feature: tessellationShader, for the material Displacement pin
    // (Thema 207). Optional — without it every displacement material draws
    // untessellated, exactly as with the pin unconnected. HE_TESSELLATION=0 leaves
    // it off (GL's switch, the A/B reference on the same binary).
    VkPhysicalDeviceFeatures coreEnable{};
    {
        VkPhysicalDeviceFeatures have{};
        vkGetPhysicalDeviceFeatures(m_physDevice, &have);
        const char* tessEnv = std::getenv("HE_TESSELLATION");
        m_tessSupported = have.tessellationShader == VK_TRUE && !(tessEnv && std::strcmp(tessEnv, "0") == 0);
        coreEnable.tessellationShader = m_tessSupported ? VK_TRUE : VK_FALSE;
        HE::setWaterTessellationAvailable(m_tessSupported);
        HE_LOG_INFO(RHI, "%s", m_tessSupported
            ? "VulkanRenderer: tessellationShader enabled (material Displacement pin)"
            : "VulkanRenderer: no tessellation (feature missing or HE_TESSELLATION=0), "
              "displacement materials draw untessellated");
    }

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = wantHwRt ? &asEnable : nullptr;
    dci.pEnabledFeatures        = &coreEnable;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    vkCheck(vkCreateDevice(m_physDevice, &dci, nullptr, &m_device), "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetPhysicalDeviceMemoryProperties(m_physDevice, &m_memoryProperties);

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
    // UNORM first: everything that reaches this image is already display-encoded
    // (the tonemap pass writes gamma), an _SRGB format would encode it again.
    VkSurfaceFormatKHR chosen = fmts[0];
    bool unorm = false;
    for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
    {
        for (auto& f : fmts)
            if (f.format == want) { chosen = f; unorm = true; break; }
        if (unorm) break;
    }
    if (!unorm)
        HE_LOG_WARN(RHI, "VulkanRenderer: no UNORM swapchain format offered, using %d", int(chosen.format));
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

    // Sampled copy of the same depth for the decal pass (see DecalDepth in the
    // header). Its lifetime is this depth buffer's — both die on a resize.
    createDecalDepth(m_decalDepth, m_swapExtent.width, m_swapExtent.height);
}

void VulkanRenderer::destroyDepthResources()
{
    destroyDecalDepth(m_decalDepth);
    if (m_depthView)   { vkDestroyImageView(m_device, m_depthView, nullptr);   m_depthView = VK_NULL_HANDLE; }
    if (m_depthImage)  { vkDestroyImage    (m_device, m_depthImage, nullptr);  m_depthImage = VK_NULL_HANDLE; }
    if (m_depthMemory) { vkFreeMemory      (m_device, m_depthMemory, nullptr); m_depthMemory = VK_NULL_HANDLE; }
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & props) == props)
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

    deviceWaitIdle();
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
    deviceWaitIdle();
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
    wd.frameSerial[fi] = ++m_submitSerial;   // same counter as the main window
    submitGraphics(si, wd.frameFence[fi]);

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
    // What this draws is the SCENE again, not the second window's widget tree
    // (A5, docs/he-apps-plan.md §13.3) — the same world from the same camera,
    // which is not what a tool window is for. Said once rather than left to be
    // discovered; GetCapabilities().supportsSecondaryWindows is false here, so
    // createSecondaryWindow refuses before anything reaches this line.
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        HE_LOG_WARN(RHI, "%s",
            "VulkanRenderer: a second window redraws the SCENE, it has no UI-only "
            "path — Software and Metal have one");
    }
    renderWindowData(it->second);
}

// ─── Scene draw path ─────────────────────────────────────────────────────────

VkShaderModule VulkanRenderer::loadShaderModule(const char* spvFileName)
{
    // SPIR-V is loaded from <exe dir>/HorizonEngine/Shaders/<name>.spv (an
    // exported game, Platform/GameLayout.h), else <exe dir>/Shaders/ (the editor
    // deploy, older exports, a .app's Resources). The .spv files are
    // produced from src/HE_Rendering/shaders/*.{vert,frag} by glslc at build
    // time (see CMakeLists) — they cannot be generated on a machine without a
    // Vulkan/glslc toolchain, which is also why this backend isn't built there.
    const char* base = SDL_GetBasePath();
    const std::string baseDir = base ? base : "";
    std::string path = baseDir + "HorizonEngine/Shaders/" + spvFileName;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
    {
        path = baseDir + "Shaders/" + spvFileName;
        f.clear();
        f.open(path, std::ios::binary | std::ios::ate);
    }
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
    //                 binding 8 = forward SSR result (transparent-black fallback).
    //                 binding 9 = local (point/spot) shadow atlas (2D array).
    //                 bindings 10..12 = clustered-lighting SSBOs (lights/grid/indices).
    VkDescriptorSetLayoutBinding binds[13]{};
    binds[0].binding         = 0;
    binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding         = 1;
    binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].binding         = 2;  // per-draw material block: dynamic offset into m_sceneMatBuf
    binds[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    binds[2].descriptorCount = 1;
    binds[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[3].binding         = 3;  // uAO: SSAO occlusion (1x1 white fallback when disabled)
    binds[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[3].descriptorCount = 1;
    binds[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Bindings 4-7: ray-traced GI mask + DDGI probe atlases + per-pixel
    // local-light mask (white fallbacks when GI is off — giParams.y == 0
    // gates all sampling in scene.frag).
    // Binding 8: the half-res forward-SSR result, sampled per gl_FragCoord like
    // uAO. Gated by ssrParams.x; a 1×1 transparent black stands in when no trace
    // ran, so a sample contributes nothing even if the gate were ever wrong.
    // Binding 9: the local (point/spot) shadow atlas, a sampler2DArray like
    // binding 1 (written by writeShadowDescriptors; the shader gates every
    // sample on lightParams.y, which stays -1 until the atlas was rendered).
    for (uint32_t gb = 4; gb <= 9; ++gb)
    {
        binds[gb].binding         = gb;
        binds[gb].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[gb].descriptorCount = 1;
        binds[gb].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // Bindings 10..12: the clustered-lighting SSBOs (HE::BuildClusterLights),
    // std430 readonly buffers in scene.frag. Written once per frame slot;
    // the shader gates every read on clusterParams.x.
    for (uint32_t cb = 10; cb <= 12; ++cb)
    {
        binds[cb].binding         = cb;
        binds[cb].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[cb].descriptorCount = 1;
        binds[cb].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 13;
    slci.pBindings    = binds;
    vkCheck(vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_sceneSetLayout), "descriptor set layout");

    // The per-draw material ring behind binding 2 (see m_sceneMatBuf): one per frame in
    // flight, host-visible + coherent and persistently mapped like the Frame UBO, so a
    // draw's block is written while recording and never by a transfer inside the pass.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physDevice, &props);
        const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
        m_sceneMatStride = (32 + align - 1) / align * align;
        const VkDeviceSize bytes = m_sceneMatStride * k_sceneMatSlots;
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        {
            MatFrameBuf& rb = m_sceneMatBuf[i];
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = bytes;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &rb.buf), "scene material ring");
            VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, rb.buf, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize  = req.size;
            mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &rb.mem), "scene material ring mem");
            vkBindBufferMemory(m_device, rb.buf, rb.mem, 0);
            vkCheck(vkMapMemory(m_device, rb.mem, 0, bytes, 0, &rb.mapped), "scene material ring map");
            // Slot 0 holds a valid block before the first draw writes one: the
            // binds ahead of any built-in draw use offset 0.
            std::memset(rb.mapped, 0, static_cast<size_t>(m_sceneMatStride));
        }
        m_sceneMatCursor = 0;
        m_sceneMatOffset = 0;
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
    VkDescriptorPoolSize ps[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 1 },  // binding0
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_maxFramesInFlight * 1 },  // binding2 (material ring)
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight * 8 },  // binding1(shadow) + binding3(AO) + bindings4-7(GI) + binding8(SSR) + binding9(local atlas)
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         k_maxFramesInFlight * 3 },  // bindings10-12 (cluster lists)
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes    = ps;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_descPool), "descriptor pool");

    // Clustered-lighting SSBOs (bindings 10..12), one triple per frame in
    // flight, host-visible + coherent and persistently mapped like the Frame
    // UBO. Sized from the LightPacking caps: 256 lights × 4 vec4, 3456 cells ×
    // uvec2, 65536 indices. All or nothing — the scene set writes all three.
    {
        auto makeSSBO = [&](VkDeviceSize bytes, ClusterBuffer& out) -> bool
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = bytes;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (vkCreateBuffer(m_device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
            VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, out.buf, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize  = req.size;
            mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(m_device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, out.buf, out.mem, 0);
            return vkMapMemory(m_device, out.mem, 0, bytes, 0, &out.mapped) == VK_SUCCESS;
        };
        m_clusterReady = true;
        for (uint32_t i = 0; i < k_maxFramesInFlight && m_clusterReady; ++i)
            m_clusterReady =
                makeSSBO(static_cast<VkDeviceSize>(HE::kMaxClusteredLights) * 4u * sizeof(glm::vec4), m_clusterLights[i])
             && makeSSBO(static_cast<VkDeviceSize>(HE::kClusterCount) * sizeof(glm::uvec2),           m_clusterGrid[i])
             && makeSSBO(static_cast<VkDeviceSize>(HE::kMaxClusterIndices) * sizeof(uint32_t),       m_clusterIdx[i]);
        if (!m_clusterReady)
        {
            m_forwardClustered = false;
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: cluster light SSBOs failed — "
                                   "staying on the 8-light window");
        }
        // HE_FORWARD_CLUSTER=0 keeps the mixed 8-light window (A/B guard, the
        // forward twin of Metal's HE_DEFERRED_CLUSTER). The descriptors stay
        // written either way — a bound set must be complete.
        if (const char* cl = std::getenv("HE_FORWARD_CLUSTER"); cl && *cl && std::atoi(cl) == 0)
            m_forwardClustered = false;
    }

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
        VkDescriptorImageInfo  lii{ m_shadowSampler, m_localShadowView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo matDbi{ m_sceneMatBuf[i].buf, 0, 32 };  // + dynamic offset per draw
        // Binding 3 (AO): filled in by createSSAOPipeline() with the white fallback
        // view after it creates the 1x1 white texture; left unwritten here.
        // Binding 9 (local atlas) goes in with binding 1 — both exist once
        // createShadowResources ran (writeShadowDescriptors repeats this).
        VkWriteDescriptorSet w[4]{};
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
        w[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[2].pBufferInfo     = &matDbi;
        w[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3].dstSet          = m_frameUBO[i].set;
        w[3].dstBinding      = 9;
        w[3].descriptorCount = 1;
        w[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo      = &lii;
        // Bindings 1 and 9 only when their views exist (both come from
        // createShadowResources); without the cascade array the local one is
        // skipped too — the two are created together.
        vkUpdateDescriptorSets(m_device, (m_shadowView && m_localShadowView) ? 4 : (m_shadowView ? 3 : 2),
                               w, 0, nullptr);

        // Bindings 10..12: this frame slot's cluster SSBOs. Written once — the
        // buffers are persistent; only their CONTENTS change per frame.
        if (m_clusterReady)
        {
            const ClusterBuffer* cbs[3] = { &m_clusterLights[i], &m_clusterGrid[i], &m_clusterIdx[i] };
            VkDescriptorBufferInfo cbi[3]{};
            VkWriteDescriptorSet   cw[3]{};
            for (uint32_t c = 0; c < 3; ++c)
            {
                cbi[c] = { cbs[c]->buf, 0, VK_WHOLE_SIZE };
                cw[c].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                cw[c].dstSet          = m_frameUBO[i].set;
                cw[c].dstBinding      = 10 + c;
                cw[c].descriptorCount = 1;
                cw[c].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                cw[c].pBufferInfo     = &cbi[c];
            }
            vkUpdateDescriptorSets(m_device, 3, cw, 0, nullptr);
        }
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
    // the instanced scene pipeline at binding 1. The main chunk of the instance arena.
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (createInstanceBuf(k_maxInstances, m_instanceBuf[i]))
            m_instMainCap[i] = k_maxInstances;
        else
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instance buffer failed — instanced batches draw per object");
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
    if (createGraphicsPipelines(1, &pci, nullptr, &m_scenePipeline) != VK_SUCCESS)
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
        if (createGraphicsPipelines(1, &ipci, nullptr, &m_sceneInstancedPipeline) != VK_SUCCESS)
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
        if (createGraphicsPipelines(1, &tpci, nullptr, &m_sceneTransparentPipeline) != VK_SUCCESS)
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

            // Caster-side depth bias, the same the other CSM backends apply
            // (GL: glPolygonOffset(2, 4); Metal: setDepthBias; D3D11/D3D12:
            // their depth rasterizer state). DYNAMIC, because EncodeDecalDepth
            // binds this very pipeline for the camera depth pre-pass and must
            // run it with zero bias (a biased depth shifts every decal).
            VkPipelineRasterizationStateCreateInfo srs = rs;
            srs.depthBiasEnable = VK_TRUE;
            VkDynamicState sdynStates[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_BIAS };
            VkPipelineDynamicStateCreateInfo sdyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            sdyn.dynamicStateCount = 3;
            sdyn.pDynamicStates    = sdynStates;

            VkGraphicsPipelineCreateInfo spci = pci; // reuse vertex input / depth state
            spci.stageCount      = 1;            // vertex only — depth-only pass
            spci.pStages         = &sstage;
            spci.pColorBlendState = nullptr;     // no color attachment
            spci.pRasterizationState = &srs;
            spci.pDynamicState   = &sdyn;
            spci.renderPass      = m_shadowPass;
            if (createGraphicsPipelines(1, &spci, nullptr, &m_shadowPipeline) != VK_SUCCESS)
                HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: shadow pipeline creation failed");
            vkDestroyShaderModule(m_device, svs, nullptr);

            // Instanced twin: same states, the same dynamic depth bias (set once
            // per layer, kept across the rebind because both pipelines declare
            // it dynamic), VS swapped. Optional — without it every caster run
            // loops through m_shadowPipeline.
            if (VkShaderModule sivs = loadShaderModule("scene_shadow_instanced.vert.spv"))
            {
                VkPipelineShaderStageCreateInfo sistage = sstage;
                sistage.module = sivs;
                InstancedDepthInput sin(k_instStride, 4); // reads the clip * model half only
                VkGraphicsPipelineCreateInfo sipci = spci;
                sipci.pStages           = &sistage;
                sipci.pVertexInputState = &sin.info;
                if (createGraphicsPipelines(1, &sipci, nullptr,
                                              &m_shadowInstancedPipeline) != VK_SUCCESS)
                    HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instanced shadow pipeline creation failed");
                vkDestroyShaderModule(m_device, sivs, nullptr);
            }
        }
    }

    // A4: build the material-graph descriptor-set layout, pipeline layout, per-frame
    // descriptor pools and UBO ring buffers once (the white default + albedo sampler this
    // path binds already exist from the block above).
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
        destroyInstanceBuf(m_instanceBuf[i]);
        m_instMainCap[i] = 0;
        if (i == 0) // the arena's overflow chunks and replaced buffers (m_instCur only borrows)
        {
            for (RetiredInstBuf& r : m_instRetired) destroyInstanceBuf(r.b);
            m_instRetired.clear();
            m_instCur = {};
        }
        if (m_sceneMatBuf[i].mapped) vkUnmapMemory(m_device, m_sceneMatBuf[i].mem);
        if (m_sceneMatBuf[i].buf)    vkDestroyBuffer(m_device, m_sceneMatBuf[i].buf, nullptr);
        if (m_sceneMatBuf[i].mem)    vkFreeMemory   (m_device, m_sceneMatBuf[i].mem, nullptr);
        m_sceneMatBuf[i] = {};
        for (ClusterBuffer* cb : { &m_clusterLights[i], &m_clusterGrid[i], &m_clusterIdx[i] })
        {
            if (cb->mapped) vkUnmapMemory(m_device, cb->mem);
            if (cb->buf)    vkDestroyBuffer(m_device, cb->buf, nullptr);
            if (cb->mem)    vkFreeMemory   (m_device, cb->mem, nullptr);
            *cb = {};
        }
    }
    m_clusterReady = false;
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
    if (m_shadowInstancedPipeline)       { vkDestroyPipeline(m_device, m_shadowInstancedPipeline, nullptr);       m_shadowInstancedPipeline = VK_NULL_HANDLE; }
    if (m_sceneTransparentPipelineHDR)  { vkDestroyPipeline(m_device, m_sceneTransparentPipelineHDR, nullptr);   m_sceneTransparentPipelineHDR = VK_NULL_HANDLE; }
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
    // The white default fills every heTex0/heTexP0..3 slot a material leaves empty;
    // without it (or the shared albedo sampler) there is nothing valid to bind, so
    // leave the path disabled rather than sample an unbound descriptor.
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

    // 1x1 white CUBE for heSkyEnv (binding 15, samplerCube). The white 2D image
    // above cannot carry a cube view (one layer, not CUBE_COMPATIBLE), so this is
    // its own six-layer image, cleared to white and parked in SHADER_READ_ONLY.
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent        = { 1, 1, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 6;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ok = vkCreateImage(m_device, &ici, nullptr, &m_whiteCubeImage) == VK_SUCCESS;
        if (ok)
        {
            VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, m_whiteCubeImage, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize  = req.size;
            mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ok = vkAllocateMemory(m_device, &mai, nullptr, &m_whiteCubeMem) == VK_SUCCESS;
            if (ok) vkBindImageMemory(m_device, m_whiteCubeImage, m_whiteCubeMem, 0);
        }
        if (ok)
        {
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image            = m_whiteCubeImage;
            vci.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
            vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
            ok = vkCreateImageView(m_device, &vci, nullptr, &m_whiteCubeView) == VK_SUCCESS;
        }
        if (!ok)
        {
            HE_LOG_WARN(RHI, "%s",
                "VulkanRenderer: A4 material path disabled — white cube default failed");
            return;
        }

        // One-shot: UNDEFINED → TRANSFER_DST → clear white → SHADER_READ_ONLY.
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_whiteCubeImage; b.subresourceRange = range;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue white{}; white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
        vkCmdClearColorImage(tmp, m_whiteCubeImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        submitGraphics(si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
    }
    // The real sky cube that replaces the white one once baked (optional —
    // a failure keeps fog.z at 0, see createSkyEnvCube).
    createSkyEnvCube();

    // ── Descriptor set 0 layout: HE::vkmat::kBindings (VulkanMaterialLayout.h), the
    //    table he_tests reflects every node shader's SPIR-V against. Canonical bindings
    //    0-7, 8/9 for the WPO custom vertex, the GI/CSM/SSR/local-atlas samplers, and
    //    (Thema 117) fragmentClustered's three light lists at 24/25/26. Extra bindings
    //    are harmless for a shader that references none of them (the standard vertex,
    //    the plain fragment(), a baked pak blob). ──
    static_assert(HE::vkmat::kStageVertex   == VK_SHADER_STAGE_VERTEX_BIT,   "vkmat stage bits are Vulkan's");
    static_assert(HE::vkmat::kStageFragment == VK_SHADER_STAGE_FRAGMENT_BIT, "vkmat stage bits are Vulkan's");
    static_assert(HE::vkmat::kStageTessControl == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "vkmat stage bits are Vulkan's");
    static_assert(HE::vkmat::kStageTessEval == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "vkmat stage bits are Vulkan's");
    auto vkType = [](HE::vkmat::DescKind k) -> VkDescriptorType {
        switch (k)
        {
        case HE::vkmat::DescKind::UniformBuffer:        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case HE::vkmat::DescKind::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case HE::vkmat::DescKind::StorageBuffer:        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };
    // heLandscapeWeights brings the fragment stage to 17 combined samplers, one over
    // the spec minimum of the per-stage limits. A device AT that minimum gets the
    // layout without its row (and layer-blend materials draw built-in, see drawDCVk)
    // rather than a layout it cannot create. The line is the evidence either way.
    // HE_VK_MAX_STAGE_SAMPLERS=<n> caps both limits as if the device reported n — the
    // only way to walk the fallback on hardware that reports a million.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physDevice, &props);
        uint32_t maxSamplers = props.limits.maxPerStageDescriptorSamplers;
        uint32_t maxImages   = props.limits.maxPerStageDescriptorSampledImages;
        const char* cap = std::getenv("HE_VK_MAX_STAGE_SAMPLERS");
        if (cap && *cap)
        {
            const uint32_t n = static_cast<uint32_t>(std::strtoul(cap, nullptr, 10));
            maxSamplers = std::min(maxSamplers, n);
            maxImages   = std::min(maxImages, n);
        }
        m_matLandscapeWeights = HE::vkmat::landscapeWeightsFit(maxSamplers, maxImages);
        char line[384];
        std::snprintf(line, sizeof line,
            "VulkanRenderer: material set 0 needs %u fragment samplers (%u without heLandscapeWeights); "
            "device maxPerStageDescriptorSamplers %u, maxPerStageDescriptorSampledImages %u%s -> heLandscapeWeights %s",
            HE::vkmat::fragmentSamplerCount(true), HE::vkmat::fragmentSamplerCount(false),
            maxSamplers, maxImages, (cap && *cap) ? " (capped by HE_VK_MAX_STAGE_SAMPLERS)" : "",
            m_matLandscapeWeights ? "bound" : "LEFT OUT (layer-blend materials draw built-in)");
        if (m_matLandscapeWeights) HE_LOG_INFO(RHI, "%s", line);
        else                       HE_LOG_WARN(RHI, "%s", line);
    }
    VkDescriptorSetLayoutBinding b[HE::vkmat::kBindingCount]{};
    uint32_t nb = 0;
    for (uint32_t i = 0; i < HE::vkmat::kBindingCount; ++i)
    {
        if (!m_matLandscapeWeights && HE::vkmat::kBindings[i].binding == HE::vkmat::kLandscapeWeightsBinding)
            continue;
        b[nb].binding         = HE::vkmat::kBindings[i].binding;
        b[nb].descriptorType  = HE::vkmat::kBindings[i].dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                                 : vkType(HE::vkmat::kBindings[i].kind);
        b[nb].descriptorCount = 1;
        // The tessellation bits only where the device enabled the feature.
        b[nb].stageFlags      = HE::vkmat::kBindings[i].stages
                              & (m_tessSupported ? ~0u : ~HE::vkmat::kStageTess);
        ++nb;
    }
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = nb;
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
    // Each allocated set consumes one descriptor per layout row of its kind, so the
    // pool covers k_matMaxDraws sets worth of each — counted from the same table as
    // the layout. A kind missing here fails vkAllocateDescriptorSets, and the draw
    // loop then skips EVERY graph material without a word.
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
        using HE::vkmat::DescKind;
        using HE::vkmat::countOf;
        static_assert(HE::vkmat::dynamicCount() == HE::vkmat::kDynamicOffsetCount, "bindings 1, 3, 9");
        VkDescriptorPoolSize ps[4] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         (countOf(DescKind::UniformBuffer) - HE::vkmat::dynamicCount()) * k_matMaxDraws }, // b0,b8
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, HE::vkmat::dynamicCount()                       * k_matMaxDraws }, // b1,b3,b9
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, countOf(DescKind::CombinedImageSampler) * k_matMaxDraws }, // b2,b4-b7,b10-b18,b31-b33
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         countOf(DescKind::StorageBuffer)        * k_matMaxDraws }, // b24-b26 (cluster lists)
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_matMaxDraws;
        dpci.poolSizeCount = 4;
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
}

void VulkanRenderer::destroyMaterialResources()
{
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
        m_matSetCache[f].clear();
    }
    if (m_matPipelineLayout) { vkDestroyPipelineLayout(m_device, m_matPipelineLayout, nullptr); m_matPipelineLayout = VK_NULL_HANDLE; }
    if (m_matSetLayout)      { vkDestroyDescriptorSetLayout(m_device, m_matSetLayout, nullptr); m_matSetLayout = VK_NULL_HANDLE; }
    if (m_whiteArrayView)    { vkDestroyImageView(m_device, m_whiteArrayView, nullptr);         m_whiteArrayView = VK_NULL_HANDLE; }
    if (m_whiteCubeView)     { vkDestroyImageView(m_device, m_whiteCubeView, nullptr);          m_whiteCubeView  = VK_NULL_HANDLE; }
    if (m_whiteCubeImage)    { vkDestroyImage(m_device, m_whiteCubeImage, nullptr);             m_whiteCubeImage = VK_NULL_HANDLE; }
    if (m_whiteCubeMem)      { vkFreeMemory(m_device, m_whiteCubeMem, nullptr);                 m_whiteCubeMem   = VK_NULL_HANDLE; }
    destroySkyEnvCube();
}

// ── Image-based-ambient sky cube for graph materials (heSkyEnv, binding 15) ──
// GL and Metal bake HE::SkyColorCPU into a 128² cube and raise heLight.fog.z;
// this backend bound a white cube and kept fog.z at 0, so graph materials had
// no sky ambient, no specular IBL and no fog (heApplyFog needs the cube for its
// colour). The bake below is the same shared row function at the same size;
// only the upload is Vulkan's: a persistently mapped staging buffer per frame
// in flight and a copy recorded into the frame's own command buffer, so a
// moving sun never stalls the queue the way a one-shot submit would.
void VulkanRenderer::createSkyEnvCube()
{
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = { static_cast<uint32_t>(k_skyEnvFace), static_cast<uint32_t>(k_skyEnvFace), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 6;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool ok = vkCreateImage(m_device, &ici, nullptr, &m_skyEnvImage) == VK_SUCCESS;
    if (ok)
    {
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, m_skyEnvImage, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = vkAllocateMemory(m_device, &mai, nullptr, &m_skyEnvMem) == VK_SUCCESS;
        if (ok) vkBindImageMemory(m_device, m_skyEnvImage, m_skyEnvMem, 0);
    }
    if (ok)
    {
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_skyEnvImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
        vci.format           = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        ok = vkCreateImageView(m_device, &vci, nullptr, &m_skyEnvView) == VK_SUCCESS;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(k_skyEnvFace) * k_skyEnvFace * 6 * 4 * sizeof(uint16_t);
    for (uint32_t f = 0; ok && f < k_maxFramesInFlight; ++f)
    {
        MatFrameBuf& sb = m_skyEnvStaging[f];
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ok = vkCreateBuffer(m_device, &bci, nullptr, &sb.buf) == VK_SUCCESS;
        if (!ok) break;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, sb.buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok = vkAllocateMemory(m_device, &mai, nullptr, &sb.mem) == VK_SUCCESS;
        if (!ok) break;
        vkBindBufferMemory(m_device, sb.buf, sb.mem, 0);
        ok = vkMapMemory(m_device, sb.mem, 0, bytes, 0, &sb.mapped) == VK_SUCCESS;
    }
    if (!ok)
    {
        // Not fatal: graph materials keep the white stand-in with fog.z = 0,
        // exactly the state before this cube existed.
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: sky environment cube unavailable — graph materials keep flat ambient");
        destroySkyEnvCube();
    }
    m_skyEnvValid = false;
}

void VulkanRenderer::destroySkyEnvCube()
{
    for (MatFrameBuf& sb : m_skyEnvStaging)
    {
        if (sb.mapped) vkUnmapMemory(m_device, sb.mem);
        if (sb.buf)    vkDestroyBuffer(m_device, sb.buf, nullptr);
        if (sb.mem)    vkFreeMemory(m_device, sb.mem, nullptr);
        sb = {};
    }
    if (m_skyEnvView)  { vkDestroyImageView(m_device, m_skyEnvView, nullptr); m_skyEnvView  = VK_NULL_HANDLE; }
    if (m_skyEnvImage) { vkDestroyImage(m_device, m_skyEnvImage, nullptr);    m_skyEnvImage = VK_NULL_HANDLE; }
    if (m_skyEnvMem)   { vkFreeMemory(m_device, m_skyEnvMem, nullptr);        m_skyEnvMem   = VK_NULL_HANDLE; }
    m_skyEnvValid = false;
}

// Recorded OUTSIDE any render pass, before the first pass of the frame. The sun
// is m_renderWorld.sunDirection — what GL and Metal bake from — but on this
// backend the extractor runs inside DrawScene, i.e. AFTER this point, so the
// bake can trail the sun by one frame (EncodeShadowMap re-extracts first when
// shadows are on). For a low-frequency ambient cube under GL's own 0.11°
// dead-band that is invisible; a static sun is exact from the second frame.
void VulkanRenderer::updateSkyEnvCube(VkCommandBuffer cmd)
{
    if (!m_skyEnvView || !m_skyEnvStaging[m_currentFrame].mapped) return;
    const glm::vec3 sunDir = m_renderWorld.sunDirection;
    if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 2.0e-3f) return;

    constexpr int N = k_skyEnvFace;
    std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
    // Parallel over (face, row) like GL/Metal: rows are independent and
    // HE::SkyColorCPU is pure. Grain 1 — a row is tens of µs.
    parallel_for(static_cast<size_t>(6) * N, [&](size_t idx)
    {
        const int f = static_cast<int>(idx / N);
        const int t = static_cast<int>(idx % N);
        HE::BuildSkyEnvFaceRow(N, f, t, sunDir, &px[((static_cast<size_t>(f) * N + t) * N) * 4]);
    }, "SkyEnvBake", 1);
    uint16_t* dst = static_cast<uint16_t*>(m_skyEnvStaging[m_currentFrame].mapped);
    for (size_t i = 0; i < px.size(); ++i)
        dst[i] = glm::packHalf1x16(px[i]);

    // Vulkan cube faces are +X,-X,+Y,-Y,+Z,-Z with GL's (s,t) table, and row 0 of
    // the upload is t = 0 in both APIs — the bake's face order and texel layout
    // go in unchanged, no flip (the same claim SkyEnvBake.h makes for Metal).
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_skyEnvImage; b.subresourceRange = range;
    // The previous frame (still in flight on the same queue) may be sampling the
    // cube: the barrier's first scope covers every earlier submission, so its
    // fragment reads finish before the copy overwrites the texels. The first
    // bake comes from UNDEFINED — nothing has sampled it yet (fog.z was 0).
    b.oldLayout     = m_skyEnvValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = m_skyEnvValid ? VK_ACCESS_SHADER_READ_BIT : 0u;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        m_skyEnvValid ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6 };
    region.imageExtent      = { static_cast<uint32_t>(N), static_cast<uint32_t>(N), 1 };
    vkCmdCopyBufferToImage(cmd, m_skyEnvStaging[m_currentFrame].buf, m_skyEnvImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    m_skyEnvSunDir = sunDir;
    m_skyEnvValid  = true;
}

// Displacement pin (Thema 207), OpenGLRenderer::foldDisplacement's rule: the body
// goes into the shader hash only where it can take effect, so a device without
// tessellation keeps the unconnected pin's key and pipeline.
void VulkanRenderer::foldDisplacement(const HE::UUID& materialId, uint64_t& hash)
{
    if (!m_tessSupported) return;
    const MaterialAsset* ma = m_contentManager->getMaterial(materialId);
    if (!ma || ma->customShaderDispGlsl.empty()) return;
    hash ^= std::hash<std::string>{}(ma->customShaderDispGlsl) * 0xC2B2AE3D27D4EB4FULL;
    m_tessDispByKey.try_emplace(hash, ma->customShaderDispGlsl);
}

// `precompiled` (the pak's baked SPIR-V for Vulkan, MaterialShaderLibrary::precompiledFor)
// wins over the runtime cross-compile — same split as GL's GetOrBuildMaterialProgram and
// Metal's GetOrBuildMaterialPipeline, and the only source of shaders in a flavour built
// without glslang.
VkPipeline VulkanRenderer::GetOrBuildMaterialPipeline(uint64_t hash, const std::string& frag,
                                                      const std::string& vertBody,
                                                      const MaterialShaderVariant* precompiled,
                                                      bool hdr, bool transparent, bool gbuffer,
                                                      bool instanced)
{
    // `gbuffer` (Thema 150): `frag` is the material's G-BUFFER tail
    // (resolveGBufferShaders), drawn into m_gbufferRP's four targets. Its own
    // key space (D3D11's/D3D12's salt, plan §10.7 #4) — a G-buffer pipeline
    // handed out as a forward one would write attributes into the lit colour.
    // Cross-compiled only and never the clustered variant: the pak bakes no
    // G-buffer SPIR-V, and its forward variant would write LIT colour into GB0.
    // Displacement (Thema 207): foldDisplacement filed the body under this very
    // hash (before the G-buffer salt below), only on a device with tessellation.
    const std::string* disp = nullptr;
    if (m_tessSupported)
        if (auto it = m_tessDispByKey.find(hash); it != m_tessDispByKey.end()) disp = &it->second;
    // A height that samples heLandscapeWeights on a device whose layout left binding
    // 14 out (see below) would make the pipeline invalid: draw it untessellated.
    if (disp && !m_matLandscapeWeights && disp->find("heLandscapeWeights") != std::string::npos)
        disp = nullptr;
    if (gbuffer)
    {
        if (m_gbufferRP == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        precompiled = nullptr; transparent = false; hdr = true;
        hash ^= 0x6742756666657231ULL;
    }
    // `instanced` (Thema 182, 2f): the twin with the material's instanced vertex
    // (customVertexInstanced / standardVertexInstanced) that draws a GeometryPass
    // batch in ONE call, the model per instance at locations 4..7 from the instance
    // buffer at binding 1. Its own pipeline key; the fragment is the plain one, so
    // the library's fragment caches keep the unsalted `libHash`. The pak bakes no
    // instanced vertex: no twin, the caller loops (as on GL).
    if (instanced && precompiled) return VK_NULL_HANDLE;
    const uint64_t libHash = hash;
    if (instanced) hash ^= 0x1D57A9CEDB0B1E55ULL;
    // A layer-blend graph samples heLandscapeWeights; on a device whose layout had to
    // leave binding 14 out (createMaterialResources) its pipeline would be invalid. No
    // pipeline then — drawDCVk draws it built-in PBR, the warm-up skips it. Checked
    // HERE so both callers go through it.
    if (!m_matLandscapeWeights && frag.find("heLandscapeWeights") != std::string::npos)
    {
        if (!m_matLandscapeWarned)
        {
            m_matLandscapeWarned = true;
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: a Landscape Layer Blend material draws built-in "
                                   "PBR — this device has no sampler slot left for heLandscapeWeights");
        }
        return VK_NULL_HANDLE;
    }
    // Cache key mixes the shader hash with the render-target + blend variant so LDR (swapchain)
    // / HDR (RGBA16F offscreen) / opaque / transparent pipelines never collide.
    const uint64_t key = hash ^ (hdr ? 0x9E3779B97F4A7C15ULL : 0ULL)
                              ^ (transparent ? 0xD1B54A32D192ED03ULL : 0ULL);
    if (auto it = m_materialPipelines.find(key); it != m_materialPipelines.end()) return it->second;

    VkRenderPass rp = gbuffer ? m_gbufferRP : hdr ? m_postFxSceneRP : m_renderPass;
    if (rp == VK_NULL_HANDLE)
        return VK_NULL_HANDLE; // target pass not ready yet — retry next frame (not cached)

    using Backend = HE::MaterialShaderLibrary::Backend;
    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    auto makePair = [&](const std::vector<uint32_t>& vsWords, const std::vector<uint32_t>& fsWords,
                        const char* origin) -> bool {
        vs = makeModule(vsWords);
        fs = makeModule(fsWords);
        if (vs && fs) return true;
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        vs = fs = VK_NULL_HANDLE;
        HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: A4 material shader module creation failed (")
            + origin + ")").c_str());
        return false;
    };

    // The baked variant first: its string fields are the SPIR-V words as raw bytes. A
    // torn blob (odd byte count) or a module the driver rejects is not the end: fall
    // through to the runtime cross-compile, which is what rendered that pak before
    // variants were consumed here at all. Only when both roads are closed is the miss
    // cached.
    // The baked clustered twin (Thema 117) goes first, under the same rule as the
    // cross-compiled one below (cluster SSBOs exist, A/B guard not set): only then
    // does the draw loop write bindings 24..26 and DrawScene open the gate.
    bool built = false;
    if (precompiled && m_forwardClustered && m_clusterReady && !precompiled->fragmentClustered.empty())
    {
        std::vector<uint32_t> preVs, preFs;
        const std::string& vsBytes = precompiled->vertexClustered.empty()
            ? precompiled->vertex : precompiled->vertexClustered;
        if (HE::MaterialShaderLibrary::spirvFromBytes(vsBytes, preVs)
            && HE::MaterialShaderLibrary::spirvFromBytes(precompiled->fragmentClustered, preFs))
            built = makePair(preVs, preFs, "baked variant, clustered");
        if (!built)
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: A4 baked clustered material variant rejected — "
                "trying the baked 8-light window variant");
    }
    if (!built && precompiled)
    {
        std::vector<uint32_t> preVs, preFs;
        if (HE::MaterialShaderLibrary::spirvFromBytes(precompiled->vertex, preVs)
            && HE::MaterialShaderLibrary::spirvFromBytes(precompiled->fragment, preFs))
            built = makePair(preVs, preFs, "baked variant");
        if (!built)
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: A4 baked material variant rejected — cross-compiling instead");
    }
    if (!built)
    {
        // Standard vertex (no WPO) or the graph's custom vertex body, cross-compiled to
        // SPIR-V — or their instanced twins.
        const uint64_t vh = vertBody.empty() ? 0 : std::hash<std::string>{}(vertBody);
        const HE::MaterialShaderLibrary::Compiled& vc = instanced
            ? (vertBody.empty() ? m_matShaderLib.standardVertexInstanced(Backend::SpirV)
                                : m_matShaderLib.customVertexInstanced(vh, vertBody, Backend::SpirV))
            : (vertBody.empty() ? m_matShaderLib.standardVertex(Backend::SpirV)
                                : m_matShaderLib.customVertex(vh, vertBody, Backend::SpirV));
        // Thema 117: the clustered twin while the cluster SSBOs exist and the A/B
        // guard is not set — the draw loop then writes them at set 0 bindings
        // 24..26 and DrawScene opens the gate (FillMaterialClusterParams). A
        // variant that does not compile falls back to the plain window shader,
        // which is exactly what rendered before (the window stays full, §2.1/1).
        // Decided once in createScenePipeline, so the cache key needs no bit.
        const HE::MaterialShaderLibrary::Compiled* fcp = nullptr;
        if (!gbuffer && m_forwardClustered && m_clusterReady)
        {
            const HE::MaterialShaderLibrary::Compiled& cc = m_matShaderLib.fragmentClustered(libHash, frag, Backend::SpirV);
            if (cc.ok && !cc.spirv.empty())
                fcp = &cc;
            else
                HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: clustered material variant failed, "
                                                    "using the 8-light window: ") + cc.log).c_str());
        }
        const HE::MaterialShaderLibrary::Compiled& fc =
            fcp ? *fcp : m_matShaderLib.fragment(libHash, frag, Backend::SpirV);
        if (!vc.ok || !fc.ok || vc.spirv.empty() || fc.spirv.empty())
        {
            HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: A4 material shader cross-compile failed: ")
                + vc.log + " " + fc.log).c_str());
            m_materialPipelines.emplace(key, VK_NULL_HANDLE); // cache the miss — don't retry every draw
            return VK_NULL_HANDLE;
        }
        built = makePair(vc.spirv, fc.spirv, "cross-compiled");
    }
    if (!built)
    {
        m_materialPipelines.emplace(key, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }

    // Displacement: the two tessellation stages between the (unchanged) vertex and
    // fragment stage. Cross-compiled even next to a baked pair (a pak carries no
    // tessellation stages yet), so without shaderc the material simply draws
    // untessellated — the fallback, never a missing draw.
    VkShaderModule tcs = VK_NULL_HANDLE, tes = VK_NULL_HANDLE;
    if (disp)
    {
        const uint64_t dh = std::hash<std::string>{}(*disp);
        const HE::MaterialShaderLibrary::Compiled& tc = m_matShaderLib.tessControl(dh, *disp, Backend::SpirV);
        const HE::MaterialShaderLibrary::Compiled& te = m_matShaderLib.tessEval(dh, *disp, Backend::SpirV);
        if (tc.ok && te.ok && !tc.spirv.empty() && !te.spirv.empty())
        {
            tcs = makeModule(tc.spirv);
            tes = makeModule(te.spirv);
        }
        if (!tcs || !tes)
        {
            if (tcs) vkDestroyShaderModule(m_device, tcs, nullptr);
            if (tes) vkDestroyShaderModule(m_device, tes, nullptr);
            tcs = tes = VK_NULL_HANDLE;
            HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: displacement stages failed to cross-compile, "
                "drawing the material untessellated\n") + tc.log + te.log).c_str());
        }
    }

    VkPipelineShaderStageCreateInfo stages[4]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
    stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;    stages[2].module = tcs; stages[2].pName = "main";
    stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; stages[3].module = tes; stages[3].pName = "main";

    // Vertex input identical to the scene pipeline (binding 0 = interleaved pos/normal/uv,
    // 32 B; the standard/custom vertex reads attribute locations 0/1/2). The instanced
    // twin adds the model at locations 4..7 from binding 1: the 128-byte {mvp, model}
    // slots of m_instanceBuf, the model half at +64 (the twin forms heMvp = U.mvp *
    // model itself, the mvp half stays unread).
    static_assert(k_instStride == 2 * sizeof(glm::mat4), "the twin reads the model half of {mvp, model}");
    const VkVertexInputBindingDescription binds[2] = {
        { 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, k_instStride,       VK_VERTEX_INPUT_RATE_INSTANCE },
    };
    const VkVertexInputAttributeDescription attrs[7] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
        { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,  64 },
        { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,  80 },
        { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,  96 },
        { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 112 },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = instanced ? 2u : 1u;
    vi.pVertexBindingDescriptions      = binds;
    vi.vertexAttributeDescriptionCount = instanced ? 7u : 3u;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Patches of the mesh's own triangles; the topology is baked into the
    // pipeline, so the draw loop's vkCmdDrawIndexed stays as it is.
    VkPipelineTessellationStateCreateInfo ts{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
    ts.patchControlPoints = 3;
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
    // The G-buffer tail writes oGB0..oGB3 at locations 0..3: one blend state
    // per target, all identical (no blend), so independentBlend — not enabled
    // on this device (createDevice) — is never needed.
    VkPipelineColorBlendAttachmentState gbBlend[k_gbTargets];
    if (gbuffer)
    {
        for (VkPipelineColorBlendAttachmentState& g : gbBlend) g = cba;
        cb.attachmentCount = k_gbTargets;
        cb.pAttachments    = gbBlend;
    }
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

    // Four stages as one PATCH_LIST pipeline; one the tessellation stages broke is
    // built again without them (displacement ignored, logged — cached either way).
    auto setTess = [&](bool on) {
        pci.stageCount         = on ? 4u : 2u;
        pci.pTessellationState = on ? &ts : nullptr;
        ia.topology            = on ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    };
    setTess(tcs != VK_NULL_HANDLE);
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = createGraphicsPipelines(1, &pci, nullptr, &pipe);
    if (pr != VK_SUCCESS && tcs != VK_NULL_HANDLE)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: material pipeline with tessellation stages failed, "
            "drawing it untessellated");
        setTess(false);
        pipe = VK_NULL_HANDLE;
        pr = createGraphicsPipelines(1, &pci, nullptr, &pipe);
    }
    if (pr != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", instanced
            ? "VulkanRenderer: A4 instanced material pipeline creation failed, its batches draw per instance"
            : "VulkanRenderer: A4 material pipeline creation failed");
        pipe = VK_NULL_HANDLE;
    }
    else if (pci.stageCount == 4u)
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: built a tessellated material pipeline (Displacement pin)");
    else if (instanced)
        HE_LOG_INFO(RHI, "%s", gbuffer
            ? "VulkanRenderer: built an INSTANCED G-buffer material twin (one draw per batch)"
            : "VulkanRenderer: built an INSTANCED material twin (one draw per batch)");
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
    if (tcs) vkDestroyShaderModule(m_device, tcs, nullptr);
    if (tes) vkDestroyShaderModule(m_device, tes, nullptr);
    m_materialPipelines.emplace(key, pipe); // cache success OR failure (null → no per-draw retry)
    return pipe;
}

void VulkanRenderer::createShadowResources()
{
    // Nearest + clamp-to-border(white): each PCF tap must read ONE texel's
    // depth (linear would compare against an interpolated depth), and a tap
    // that leaves the map reads "lit". Sampler and render pass are size-
    // independent and live for the renderer; only the images are rebuilt on a
    // resolution change (createShadowImages).
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside the map = lit
    vkCheck(vkCreateSampler(m_device, &sci, nullptr, &m_shadowSampler), "shadow sampler");

    // Depth-only render pass; final layout is shader-readable for sampling.
    // Layer-agnostic: the per-cascade framebuffers below each attach ONE layer
    // of the array, and EncodeDecalDepth reuses this pass for its own 2D image.
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

    createShadowImages();
    createLocalShadowImages();
}

void VulkanRenderer::createLocalShadowImages()
{
    // The local (point/spot) atlas: same depth-array pattern as the cascades,
    // kLocalShadowLayers layers at a fixed kLocalShadowSize, rendered one layer
    // at a time through the shared depth-only pass.
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = m_depthFormat;
    ici.extent      = { kLocalShadowSize, kLocalShadowSize, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = static_cast<uint32_t>(kLocalShadowLayers);
    ici.samples     = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ici.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCheck(vkCreateImage(m_device, &ici, nullptr, &m_localShadowImage), "local shadow image");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_localShadowImage, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &m_localShadowMemory), "local shadow memory");
    vkBindImageMemory(m_device, m_localShadowImage, m_localShadowMemory, 0);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image    = m_localShadowImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format   = m_depthFormat;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = static_cast<uint32_t>(kLocalShadowLayers);
    vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_localShadowView), "local shadow array view");

    for (int v = 0; v < kLocalShadowLayers; ++v)
    {
        VkImageViewCreateInfo lci = vci;
        lci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        lci.subresourceRange.baseArrayLayer = static_cast<uint32_t>(v);
        lci.subresourceRange.layerCount     = 1;
        vkCheck(vkCreateImageView(m_device, &lci, nullptr, &m_localShadowLayerView[v]), "local shadow layer view");

        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_shadowPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &m_localShadowLayerView[v];
        fci.width           = kLocalShadowSize;
        fci.height          = kLocalShadowSize;
        fci.layers          = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_localShadowFB[v]), "local shadow framebuffer");
    }
}

void VulkanRenderer::destroyLocalShadowImages()
{
    m_localShadowLayoutValid = false;
    for (int v = 0; v < kLocalShadowLayers; ++v)
    {
        if (m_localShadowFB[v])        { vkDestroyFramebuffer(m_device, m_localShadowFB[v], nullptr);        m_localShadowFB[v] = VK_NULL_HANDLE; }
        if (m_localShadowLayerView[v]) { vkDestroyImageView  (m_device, m_localShadowLayerView[v], nullptr); m_localShadowLayerView[v] = VK_NULL_HANDLE; }
    }
    if (m_localShadowView)   { vkDestroyImageView(m_device, m_localShadowView, nullptr);   m_localShadowView = VK_NULL_HANDLE; }
    if (m_localShadowImage)  { vkDestroyImage    (m_device, m_localShadowImage, nullptr);  m_localShadowImage = VK_NULL_HANDLE; }
    if (m_localShadowMemory) { vkFreeMemory      (m_device, m_localShadowMemory, nullptr); m_localShadowMemory = VK_NULL_HANDLE; }
}

void VulkanRenderer::createShadowImages()
{
    // Depth image with one array layer per cascade, sampled by the scene pass
    // as a 2D array; rendered one layer at a time through the per-layer views.
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType   = VK_IMAGE_TYPE_2D;
    ici.format      = m_depthFormat;
    ici.extent      = { m_shadowSize, m_shadowSize, 1 };
    ici.mipLevels   = 1;
    ici.arrayLayers = static_cast<uint32_t>(kCsmCascades);
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
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format   = m_depthFormat;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = static_cast<uint32_t>(kCsmCascades);
    vkCheck(vkCreateImageView(m_device, &vci, nullptr, &m_shadowView), "shadow array view");

    for (int c = 0; c < kCsmCascades; ++c)
    {
        VkImageViewCreateInfo lci = vci;
        lci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        lci.subresourceRange.baseArrayLayer = static_cast<uint32_t>(c);
        lci.subresourceRange.layerCount     = 1;
        vkCheck(vkCreateImageView(m_device, &lci, nullptr, &m_shadowLayerView[c]), "shadow layer view");

        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_shadowPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &m_shadowLayerView[c];
        fci.width           = m_shadowSize;
        fci.height          = m_shadowSize;
        fci.layers          = 1;
        vkCheck(vkCreateFramebuffer(m_device, &fci, nullptr, &m_shadowFB[c]), "shadow framebuffer");
    }
}

void VulkanRenderer::destroyShadowImages()
{
    m_shadowLayoutValid = false;
    for (int c = 0; c < kCsmCascades; ++c)
    {
        if (m_shadowFB[c])        { vkDestroyFramebuffer(m_device, m_shadowFB[c], nullptr);        m_shadowFB[c] = VK_NULL_HANDLE; }
        if (m_shadowLayerView[c]) { vkDestroyImageView  (m_device, m_shadowLayerView[c], nullptr); m_shadowLayerView[c] = VK_NULL_HANDLE; }
    }
    if (m_shadowView)    { vkDestroyImageView  (m_device, m_shadowView, nullptr); m_shadowView = VK_NULL_HANDLE; }
    if (m_shadowImage)   { vkDestroyImage      (m_device, m_shadowImage, nullptr); m_shadowImage = VK_NULL_HANDLE; }
    if (m_shadowMemory)  { vkFreeMemory        (m_device, m_shadowMemory, nullptr); m_shadowMemory = VK_NULL_HANDLE; }
}

void VulkanRenderer::destroyShadowResources()
{
    destroyShadowImages();
    destroyLocalShadowImages();
    if (m_shadowPass)    { vkDestroyRenderPass (m_device, m_shadowPass, nullptr); m_shadowPass = VK_NULL_HANDLE; }
    if (m_shadowSampler) { vkDestroySampler    (m_device, m_shadowSampler, nullptr); m_shadowSampler = VK_NULL_HANDLE; }
}

// Points binding 1 of every frame slot's scene set at the cascade array. Called
// once the sets exist (createScenePipeline) and again after a resolution swap
// rebuilt the image — the GPU is idle then, so rewriting a set that a queued
// frame could still reference is safe.
void VulkanRenderer::writeShadowDescriptors()
{
    if (!m_shadowView) return;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_frameUBO[i].set == VK_NULL_HANDLE) continue;
        VkDescriptorImageInfo dii{ m_shadowSampler, m_shadowView,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo lii{ m_shadowSampler, m_localShadowView,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[2]{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = m_frameUBO[i].set;
        w[0].dstBinding      = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo      = &dii;
        // Binding 9: the local (point/spot) atlas — size-fixed, so a cascade
        // resolution swap rewrites it unchanged; harmless.
        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = m_frameUBO[i].set;
        w[1].dstBinding      = 9;
        w[1].descriptorCount = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo      = &lii;
        vkUpdateDescriptorSets(m_device, m_localShadowView ? 2 : 1, w, 0, nullptr);
    }
}

// ─── Screen-space decals (docs/decals-cross-backend-plan.md checkpoint E) ─────
// Metal and GL composite decals into the G-buffer's base colour, so the decal is
// lit by the same resolve as everything else. Vulkan has no G-buffer; here the
// projector is blended into the already-lit forward colour instead. That is the
// documented optical deviation of this backend — see the plan and
// docs/backend-parity-plan.md.

bool VulkanRenderer::createDecalDepth(DecalDepth& d, uint32_t w, uint32_t h)
{
    destroyDecalDepth(d);
    if (!m_device || w == 0 || h == 0) return false;
#if !defined(HE_HAVE_SHADERC)
    // No cross-compiler → no decal shader → don't pay for a second full-size
    // depth image per render target.
    return false;
#else

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = m_depthFormat;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &d.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, d.image, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &mai, nullptr, &d.mem) != VK_SUCCESS) { destroyDecalDepth(d); return false; }
    vkBindImageMemory(m_device, d.image, d.mem, 0);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image            = d.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = m_depthFormat;
    vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_device, &vci, nullptr, &d.view) != VK_SUCCESS) { destroyDecalDepth(d); return false; }

    // The framebuffer needs m_shadowPass, which Initialize() creates AFTER the
    // swapchain (and therefore after createDepthResources) — build it on first use.
    d.w = w; d.h = h;
    return true;
#endif
}

void VulkanRenderer::destroyDecalDepth(DecalDepth& d)
{
    if (!m_device) { d = {}; return; }
    if (d.fb)    { vkDestroyFramebuffer(m_device, d.fb, nullptr);  }
    if (d.view)  { vkDestroyImageView  (m_device, d.view, nullptr); }
    if (d.image) { vkDestroyImage      (m_device, d.image, nullptr); }
    if (d.mem)   { vkFreeMemory        (m_device, d.mem, nullptr);   }
    d = {};
}

const VkImageView* VulkanRenderer::resolveDecalTexture(const HE::UUID& textureId)
{
    if (textureId == HE::UUID{} || !m_contentManager) return nullptr;
    if (auto it = m_decalTexCache.find(textureId); it != m_decalTexCache.end())
        return it->second.view ? &it->second.view : nullptr;
    // Not resident yet → retry next frame rather than caching a miss (mirrors
    // resolveMaterialOverride). resolveTextureRef triggers the load.
    const TextureAsset* tex = m_contentManager->resolveTextureRef(textureId, std::string{});
    if (!tex) return nullptr;
    MaterialTexVk mt;
    // Unsupported format → cache the failure (view stays null) so we don't retry per frame.
    uploadTextureImage(tex, mt.image, mt.mem, mt.view);
    auto res = m_decalTexCache.emplace(textureId, mt);
    return res.first->second.view ? &res.first->second.view : nullptr;
}

VkImageView VulkanRenderer::resolveGraphTexture(const HE::UUID& id, const std::string& path, bool array)
{
    std::string key = graphTexKey(id, path);
    if (key.empty() || !m_contentManager) return VK_NULL_HANDLE;
    if (array) key += "#arr"; // the same asset can be 2D in one material, an array in another
    // Unlike the decal cache this caches a null asset too, so an unloadable one does
    // not open its file every frame. But "still null" can also mean "not yet": an
    // EngineContent download a path load started. Misses are dropped once the
    // content epoch moved (Thema 210, mirrors GL); a null view was never bound, so
    // whatever a failed upload left behind is freed right away.
    HE::forgetGraphTextureMisses(m_graphTexCache, m_graphTexMissEpoch, m_contentManager->contentEpoch(),
                                 [this](MaterialTexVk& mt) {
                                     if (mt.view) return false;
                                     destroyMaterialTex(mt);
                                     return true;
                                 });
    if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end())
        return it->second.view;
    MaterialTexVk mt;
    if (const TextureAsset* tex = m_contentManager->resolveTextureRef(id, path))
        uploadTextureImage(tex, mt.image, mt.mem, mt.view, array); // unsupported format → view stays null
    auto res = m_graphTexCache.emplace(key, mt);
    return res.first->second.view;
}

bool VulkanRenderer::EnsureDecalPipelines()
{
#if !defined(HE_HAVE_SHADERC)
    return false;
#else
    if (m_decalPipeline && m_decalPipelineHDR) return true;
    if (m_decalTried) return m_decalPipeline != VK_NULL_HANDLE;
    // Both target passes must exist before we build; m_postFxSceneRP appears only
    // once the editor viewport is sized, so an early frame retries instead of
    // burning the one attempt (same reasoning as GetOrBuildMaterialPipeline).
    if (m_renderPass == VK_NULL_HANDLE || m_postFxSceneRP == VK_NULL_HANDLE) return false;
    m_decalTried = true;

    using Backend = HE::MaterialShaderLibrary::Backend;
    const auto& v = m_matShaderLib.decalVertex(Backend::SpirV);
    // The FORWARD variant: it shades the pixel itself, because there is no
    // G-buffer resolve here that would light it afterwards.
    const auto& f = m_matShaderLib.decalFragmentForward(Backend::SpirV);
    if (!v.ok || !f.ok || v.spirv.empty() || f.spirv.empty())
    {
        HE_LOG_ERROR(RHI, "%s",
            (std::string("VulkanRenderer: decal shader cross-compile failed\n") + v.log + f.log).c_str());
        return false;
    }

    // Depth sampler: NEAREST. Filtering depth would interpolate ACROSS silhouettes
    // and reconstruct world positions that exist nowhere in the scene.
    if (!m_decalDepthSampler)
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_NEAREST;
        sci.minFilter    = VK_FILTER_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &sci, nullptr, &m_decalDepthSampler) != VK_SUCCESS) return false;
    }

    // Set 0 exactly as the generated SPIR-V declares it: b19 decal texture,
    // b22 scene depth, b23 HeDecal UBO (read by BOTH stages).
    if (!m_decalSetLayout)
    {
        VkDescriptorSetLayoutBinding binds[3]{};
        binds[0].binding = 19; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 22; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2].binding = 23; binds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[2].descriptorCount = 1;
        binds[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 3; slci.pBindings = binds;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_decalSetLayout) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_decalSetLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_decalPipeLayout) != VK_SUCCESS) return false;
    }

    // Per-frame descriptor pool + UBO ring, one slot per decal draw.
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_decalPool[i] == VK_NULL_HANDLE)
        {
            VkDescriptorPoolSize ps[2] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxDecals * 2 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxDecals     },
            };
            VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpci.maxSets = k_maxDecals; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
            if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_decalPool[i]) != VK_SUCCESS) return false;
        }
        if (m_decalUBO[i].buf == VK_NULL_HANDLE)
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size  = static_cast<VkDeviceSize>(k_maxDecals) * k_decalSlotStride;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            if (vkCreateBuffer(m_device, &bci, nullptr, &m_decalUBO[i].buf) != VK_SUCCESS) return false;
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(m_device, m_decalUBO[i].buf, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize  = req.size;
            mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(m_device, &mai, nullptr, &m_decalUBO[i].mem) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, m_decalUBO[i].buf, m_decalUBO[i].mem, 0);
            vkMapMemory(m_device, m_decalUBO[i].mem, 0, VK_WHOLE_SIZE, 0, &m_decalUBO[i].mapped);
        }
    }

    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule vs = makeModule(v.spirv);
    VkShaderModule fs = makeModule(f.spirv);
    if (!vs || !fs)
    {
        if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: decal shader module creation failed");
        return false;
    }

    auto buildPipeline = [&](VkRenderPass rp, VkPipeline& out) -> bool {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        // Buffer-less: the 36 cube vertices come from gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        // Cull the FRONT faces so the projector still rasterizes when the camera
        // sits inside its box — exactly one triangle layer must survive, two would
        // blend the decal twice. Both GL and Vulkan call a triangle front-facing
        // when it is counter-clockwise AS SEEN ON SCREEN (Vulkan's signed-area
        // formula carries the leading minus for its y-down framebuffer precisely
        // so that this holds), and kVulkanClipFix is what makes the image on
        // screen identical to GL's. Same picture, same visual winding, same cull:
        // GL_CCW + GL_FRONT here reads as CCW + FRONT_BIT.
        rs.cullMode    = VK_CULL_MODE_FRONT_BIT;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // No depth test and no depth write: the box-space clip against the
        // reconstructed world position is what decides coverage.
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable  = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable         = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp        = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp        = VK_BLEND_OP_ADD;
        // RGB only. The viewport image's alpha is what ImGui composites with, and
        // on Metal/GL the same mask keeps GB0.a (metallic) intact.
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

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
        pci.layout              = m_decalPipeLayout;
        pci.renderPass          = rp;
        pci.subpass             = 0;
        return createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
    };

    // Two pipelines, picked by `hdr` exactly like the scene pipelines: the
    // editor's no-PostFX fallback pass reuses the LDR one, as DrawScene does.
    const bool okLdr = buildPipeline(m_renderPass,    m_decalPipeline);
    const bool okHdr = buildPipeline(m_postFxSceneRP, m_decalPipelineHDR);
    // The deferred variant (Thema 150 S5): decalFragmentSampled, the UNSHADED
    // colour into GB0 inside m_gbDecalRP — same layout, blend and cull. The
    // SRGB attachment makes the blend linear, as GL's FRAMEBUFFER_SRGB and
    // Metal's sRGB target do; the RGB mask keeps GB0.a (metallic). Optional:
    // without it a deferred frame draws the forward decal over the resolve.
    if (m_gbDecalRP)
    {
        const auto& g = m_matShaderLib.decalFragmentSampled(Backend::SpirV);
        VkShaderModule gfs = (g.ok && !g.spirv.empty()) ? makeModule(g.spirv) : VK_NULL_HANDLE;
        if (gfs)
        {
            std::swap(fs, gfs);
            if (!buildPipeline(m_gbDecalRP, m_decalPipelineGB)) m_decalPipelineGB = VK_NULL_HANDLE;
            std::swap(fs, gfs);
            vkDestroyShaderModule(m_device, gfs, nullptr);
        }
        if (!m_decalPipelineGB)
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: deferred decal pipeline unavailable, "
                                   "deferred frames draw the forward decal");
    }
    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
    if (!okLdr || !okHdr)
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: decal pipeline creation failed");
    else
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: decal pipelines created (forward screen-space)");
    return m_decalPipeline != VK_NULL_HANDLE;
#endif
}

void VulkanRenderer::destroyDecalPipelines()
{
    if (!m_device) return;
    for (auto& mt : m_decalTexCache) destroyMaterialTex(mt.second);
    m_decalTexCache.clear();
    destroyDecalDepth(m_decalDepth);
    destroyDecalDepth(m_decalDepthVp);
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (m_decalUBO[i].mem)  { vkUnmapMemory(m_device, m_decalUBO[i].mem); }
        if (m_decalUBO[i].buf)  { vkDestroyBuffer(m_device, m_decalUBO[i].buf, nullptr); }
        if (m_decalUBO[i].mem)  { vkFreeMemory(m_device, m_decalUBO[i].mem, nullptr); }
        m_decalUBO[i] = {};
        if (m_decalPool[i])     { vkDestroyDescriptorPool(m_device, m_decalPool[i], nullptr); m_decalPool[i] = VK_NULL_HANDLE; }
    }
    if (m_decalPipeline)     { vkDestroyPipeline(m_device, m_decalPipeline, nullptr);     m_decalPipeline = VK_NULL_HANDLE; }
    if (m_decalPipelineHDR)  { vkDestroyPipeline(m_device, m_decalPipelineHDR, nullptr);  m_decalPipelineHDR = VK_NULL_HANDLE; }
    if (m_decalPipelineGB)   { vkDestroyPipeline(m_device, m_decalPipelineGB, nullptr);   m_decalPipelineGB = VK_NULL_HANDLE; }
    if (m_decalPipeLayout)   { vkDestroyPipelineLayout(m_device, m_decalPipeLayout, nullptr); m_decalPipeLayout = VK_NULL_HANDLE; }
    if (m_decalSetLayout)    { vkDestroyDescriptorSetLayout(m_device, m_decalSetLayout, nullptr); m_decalSetLayout = VK_NULL_HANDLE; }
    if (m_decalDepthSampler) { vkDestroySampler(m_device, m_decalDepthSampler, nullptr);  m_decalDepthSampler = VK_NULL_HANDLE; }
    m_decalTried = false;
}

void VulkanRenderer::EncodeDecalDepth(VkCommandBuffer cmd, DecalDepth& d)
{
    if (!m_world || m_shadowPipeline == VK_NULL_HANDLE || d.image == VK_NULL_HANDLE) return;

    // Cheapest possible exit for the overwhelmingly common decal-free frame:
    // EncodeShadowMap ran two lines earlier and left a freshly extracted
    // m_renderWorld behind, so the decal list is already known here — no need to
    // walk the world a second time just to find out there is nothing to do. (A
    // decal appearing in a frame where the shadow map did not extract shows up
    // one frame late; that is invisible.)
    if (m_renderWorld.decals.empty()) return;

    // Now the extract for this image's aspect. Inside the frame's scope the walk is already in
    // m_renderWorld (EncodeShadowMap's), so this only redoes what depends on the aspect, and only
    // when the depth image's differs from the cascades'. Same reason runSSAO extracts for itself.
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld,
                        static_cast<float>(d.w) / static_cast<float>(d.h), &m_editorCamera);
    if (m_renderWorld.decals.empty() || m_renderWorld.objects.empty()) return;

    refineObjectBounds();
    // The material asset overrides the component's opacity, and DrawScene
    // splits opaque from transparent AFTER applying it. Without the same
    // resolve here (Translucent clamp included) a material-driven glass pane
    // would count as opaque in this pre-pass and transparent in the scene pass
    // — depth where the scene writes none, and the decal lands on the glass.
    { HE_PROFILE_SCOPE_N("MaterialScalars"); HE::resolveWorldMaterialScalars(m_renderWorld, m_contentManager); }
    cullCameraObjects();
    if (m_sortedIndices.empty()) return;

    if (d.fb == VK_NULL_HANDLE)
    {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_shadowPass;   // depth-only, m_depthFormat → compatible
        fci.attachmentCount = 1;
        fci.pAttachments    = &d.view;
        fci.width           = d.w;
        fci.height          = d.h;
        fci.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &d.fb) != VK_SUCCESS) { d.fb = VK_NULL_HANDLE; return; }
    }

    // Jittered on a TAA frame: the decals reconstruct against this depth and
    // rasterise with the scene's (jittered) matrix — both must agree.
    const glm::mat4 camClip =
        taaJittered(HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view);

    VkClearValue clear{};
    clear.depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass        = m_shadowPass;
    rpbi.framebuffer       = d.fb;
    rpbi.renderArea.extent = { d.w, d.h };
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(d.w), static_cast<float>(d.h), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { d.w, d.h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    // The shadow pipeline's depth bias is dynamic: the cascade pass sets it,
    // this camera depth must be exact (decals reconstruct positions from it).
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);

    for (uint32_t idx : m_sortedIndices)
    {
        const RenderObject& obj = m_renderWorld.objects[idx];
        // Opaque only. Transparent geometry does not write depth in the scene
        // pass either, so including it would project decals onto glass.
        if (obj.opacity < 1.0f) continue;
        const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
        const GpuMesh& m    = mesh ? *mesh : m_cube;
        if (!m.indexCount) continue;
        // A foliage cluster stands for a bucket of plants (Thema 182, 2f): all of them
        // leave depth, in one draw through the shadow pipeline's instanced twin (same
        // pass, same dynamic zero bias), else one push-constant draw per plant.
        if (obj.isCluster())
        {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            const InstanceRun run = m_renderWorld.instanceRun(obj);
            if (drawDepthInstanced(cmd, m_shadowInstancedPipeline, m_shadowPipeline, m.indexCount, run,
                    [&](const glm::mat4& model, glm::mat4* pair) {
                        pair[0] = camClip * model;
                        pair[1] = model;
                    }))
                continue;
            run.forEach([&](const glm::mat4& model)
            {
                PushConstants ipc{ camClip * model, model };
                vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ipc), &ipc);
                vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
            });
            continue;
        }
        PushConstants pc{ camClip * obj.transform, obj.transform };
        vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
        vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd); // final layout SHADER_READ_ONLY (m_shadowPass's own dependency)
}

void VulkanRenderer::EncodeDecals(VkCommandBuffer cmd, VkImageView depthView,
                                  uint32_t width, uint32_t height, bool hdr, bool intoGBuffer)
{
    GpuPassScope _passTimer(this, "Decals");
#if !defined(HE_HAVE_SHADERC)
    (void)cmd; (void)depthView; (void)width; (void)height; (void)hdr; (void)intoGBuffer;
#else
    if (m_renderWorld.decals.empty() || depthView == VK_NULL_HANDLE) return;
    if (!EnsureDecalPipelines()) return;
    const VkPipeline pipe = intoGBuffer ? m_decalPipelineGB : hdr ? m_decalPipelineHDR : m_decalPipeline;
    if (pipe == VK_NULL_HANDLE) return;

    const uint32_t fi = m_currentFrame;
    if (!m_decalPool[fi] || !m_decalUBO[fi].mapped) return;
    // DrawScene runs at most once per frame (viewport OR swapchain), so resetting
    // the whole pool here is safe — the frame fence already waited for the GPU.
    vkResetDescriptorPool(m_device, m_decalPool[fi], 0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { width, height } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    const glm::mat4 viewProj =
        taaJittered(HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view);
    const glm::mat4 invViewProj = glm::inverse(viewProj);

    // The forward variant shades the decal itself. Same source as the material
    // HeLighting fill: the DOMINANT directional light, not the sky-dome sun —
    // the night/cloud lesson from the Metal/GL fill sites.
    glm::vec3 sunDir(0.0f), sunColor(0.0f);
    m_renderWorld.dominantDirectionalLight(sunDir, sunColor);

    uint32_t slot = 0;
    for (const DecalData& dcl : m_renderWorld.decals)
    {
        if (slot >= k_maxDecals)
        {
            HE_LOG_ONCE(RHI, Warning,
                "VulkanRenderer: more than %u decals in a frame — the rest are skipped", k_maxDecals);
            break;
        }

        HE::MaterialShaderLibrary::DecalUniforms du;
        const glm::mat4 invModel = glm::inverse(dcl.transform);
        std::memcpy(du.viewProj,    &viewProj[0][0],      16 * sizeof(float));
        std::memcpy(du.model,       &dcl.transform[0][0], 16 * sizeof(float));
        std::memcpy(du.invModel,    &invModel[0][0],      16 * sizeof(float));
        std::memcpy(du.invViewProj, &invViewProj[0][0],   16 * sizeof(float));
        du.color[0] = dcl.color.r; du.color[1] = dcl.color.g;
        du.color[2] = dcl.color.b; du.color[3] = dcl.color.a;
        const VkImageView* texView = resolveDecalTexture(dcl.textureId);
        du.params[0] = texView ? 1.0f : 0.0f;
        // Vulkan depth conventions: NDC y points DOWN (kVulkanClipFix flips it),
        // so uv.y = 0 at the top must map to clip.y = -1 → sign +1. Depth is
        // already 0..1 in both the texture and NDC → scale 1, bias 0. (Metal:
        // -1/1/0 — its NDC y points up; GL: +1/2/-1 — its NDC z is -1..1.)
        du.params[1] = 1.0f;
        du.params[2] = 1.0f;
        du.params[3] = 0.0f;
        du.vp[0] = static_cast<float>(width);
        du.vp[1] = static_cast<float>(height);
        du.sunDir[0]   = sunDir.x;   du.sunDir[1]   = sunDir.y;   du.sunDir[2]   = sunDir.z;
        du.sunColor[0] = sunColor.r; du.sunColor[1] = sunColor.g; du.sunColor[2] = sunColor.b;
        du.ambient[0]  = m_renderWorld.ambient.r;
        du.ambient[1]  = m_renderWorld.ambient.g;
        du.ambient[2]  = m_renderWorld.ambient.b;
        du.camPos[0]   = m_renderWorld.camera.position.x;
        du.camPos[1]   = m_renderWorld.camera.position.y;
        du.camPos[2]   = m_renderWorld.camera.position.z;

        const VkDeviceSize off = static_cast<VkDeviceSize>(slot) * k_decalSlotStride;
        std::memcpy(static_cast<uint8_t*>(m_decalUBO[fi].mapped) + off, &du, sizeof(du));

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_decalPool[fi];
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_decalSetLayout;
        if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) break;

        // heDecalTex must be bound even when the decal is untextured — the shader
        // statically references the sampler; the white default makes it a no-op.
        VkDescriptorImageInfo texInfo{ m_albedoSampler,
            texView ? *texView : m_whiteAlbedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo depthInfo{ m_decalDepthSampler, depthView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo bufInfo{ m_decalUBO[fi].buf, off, sizeof(du) };
        VkWriteDescriptorSet w[3]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 19;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &texInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = set; w[1].dstBinding = 22;
        w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &depthInfo;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = set; w[2].dstBinding = 23;
        w[2].descriptorCount = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[2].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_decalPipeLayout,
                                0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 36, 1, 0, 0); // buffer-less unit cube
        ++m_statDraws;
        m_statTris += 12;
        ++slot;
    }
#endif
}

// ─── PostFX pipeline ──────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Deferred render path (Thema 150): passes, pipelines, G-buffer targets
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
// §3's G-buffer plus GB3 (plan §10.5 way B): the resolve samples GB3 for its
// world-position reconstruction, so the depth attachment stays an attachment.
constexpr VkFormat kGBufferFormats[4] = { VK_FORMAT_R8G8B8A8_SRGB,
                                          VK_FORMAT_R16G16B16A16_SFLOAT,
                                          VK_FORMAT_R16G16B16A16_SFLOAT,
                                          VK_FORMAT_R32_SFLOAT };
} // namespace

bool VulkanRenderer::createDeferredPipelines()
{
    m_deferredReady = false;
#if !defined(HE_HAVE_SHADERC)
    // The resolve is cross-compiled at runtime (like SSR and the decals); a
    // flavour without the compiler keeps the capability off.
    return false;
#else
    if (!m_postFxReady || !m_matReady || !m_postFxSceneRP || !m_scenePipelineLayout)
        return false;
    static_assert(k_gbTargets == sizeof(kGBufferFormats) / sizeof(kGBufferFormats[0]),
                  "one format per G-buffer target");
    auto fail = [&](const char* why) -> bool {
        HE_LOG_WARN(RHI, "VulkanRenderer: %s — the deferred path stays off", why);
        destroyDeferredPipelines();
        return false;
    };
    using Backend = HE::MaterialShaderLibrary::Backend;
    using MSL     = HE::MaterialShaderLibrary;

    // ── G-buffer pass: four colour targets + the viewport depth, all cleared.
    // The colours END in SHADER_READ_ONLY (finalLayout), so the resolve can
    // sample them with no barrier of its own; the depth ends as an attachment
    // for m_hdrLoadRP. Dependencies: in — the previous frame's resolve read
    // the targets (write-after-read) and the sky pass wrote the depth; out —
    // the resolve's reads and the tail's depth test.
    {
        VkAttachmentDescription atts[k_gbTargets + 1]{};
        VkAttachmentReference   colorRefs[k_gbTargets]{};
        for (uint32_t i = 0; i < k_gbTargets; ++i)
        {
            atts[i].format         = kGBufferFormats[i];
            atts[i].samples        = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            colorRefs[i]           = { i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        }
        VkAttachmentDescription& d = atts[k_gbTargets];
        d.format         = m_depthFormat;
        d.samples        = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        d.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        d.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        d.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depthRef{ k_gbTargets, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = k_gbTargets;
        sub.pColorAttachments       = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        const VkPipelineStageFlags fragTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                             | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                              | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragTests;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragTests;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragTests;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | fragTests;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = k_gbTargets + 1;
        rpci.pAttachments    = atts;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_gbufferRP) != VK_SUCCESS)
            return fail("G-buffer render pass failed");
    }

    // ── HDR scene pass with LOAD on both attachments: the sky stays in the
    // colour, the G-buffer pass's depth is what the tail tests against.
    // Compatible with m_postFxSceneRP, so m_hdrFB and every HDR pipeline run
    // in it: same formats and samples (load/store ops and layouts do not
    // count) AND the same subpass dependency — dependencies DO count, the
    // validation layer rejects every draw otherwise (measured on the RTX 4070:
    // "pDependencies[0].srcStageMask is incompatible"). The pass therefore
    // carries m_postFxSceneRP's dependency verbatim; ordering the sky and
    // G-buffer writes before its loads is DrawScene's explicit barrier.
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[1].format         = m_depthFormat;
        atts[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE; // the TAA velocity pass loads it
        atts[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 1;
        sub.pColorAttachments       = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        // KEEP IN SYNC with m_postFxSceneRP's dependency (createPostFXPipelines).
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2;
        rpci.pAttachments    = atts;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_hdrLoadRP) != VK_SUCCESS)
            return fail("HDR load render pass failed");
    }

    // ── Deferred decals (step 5): GB0 alone, LOADed and stored, SHADER_READ_ONLY
    // on both ends (m_gbufferRP's finalLayout in, the resolve's sample out). Its
    // own pass, not a subpass of m_gbufferRP: the decal SAMPLES GB3, which is an
    // attachment there. In: the G-buffer pass's colour writes before this pass
    // loads/blends GB0 and samples GB3. Out: the blend before the resolve's read.
    {
        VkAttachmentDescription a{};
        a.format         = kGBufferFormats[0];
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &colorRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                              | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &a;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_gbDecalRP) != VK_SUCCESS)
            m_gbDecalRP = VK_NULL_HANDLE; // optional: decals stay forward in a deferred frame
    }

    // Point-clamp for the G-buffer reads: an interpolated oct normal or depth
    // across a silhouette is a surface that never existed (the SSR rule).
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_NEAREST;
        sci.minFilter    = VK_FILTER_NEAREST;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &sci, nullptr, &m_gbSampler) != VK_SUCCESS)
            return fail("G-buffer sampler failed");
    }

    // ── Shared fixed-function state of the three new pipeline kinds.
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE; // the scene pipeline's rule
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState noBlend{};
    noBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    // ── Built-in G-buffer pipelines: scene.vert (+ the instanced twin) with
    // gbuffer.frag on the scene layout, four targets against the viewport depth.
    {
        VkShaderModule vs  = loadShaderModule("scene.vert.spv");
        VkShaderModule fs  = loadShaderModule("gbuffer.frag.spv");
        if (!vs || !fs)
        {
            if (vs) vkDestroyShaderModule(m_device, vs, nullptr);
            if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
            return fail("gbuffer.frag.spv / scene.vert.spv missing");
        }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
        VkVertexInputBindingDescription vib{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vias[3] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 } };
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &vib;
        vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = vias;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState gbBlend[k_gbTargets] = { noBlend, noBlend, noBlend, noBlend };
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = k_gbTargets;
        cb.pAttachments    = gbBlend;
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2;                  pci.pStages             = st;
        pci.pVertexInputState   = &vi;       pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;       pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;       pci.pDepthStencilState  = &ds;
        pci.pColorBlendState    = &cb;       pci.pDynamicState       = &dyn;
        pci.layout = m_scenePipelineLayout;  pci.renderPass = m_gbufferRP; pci.subpass = 0;
        const bool ok = createGraphicsPipelines(1, &pci, nullptr,
                                                  &m_gbufPipeline) == VK_SUCCESS;
        // Instanced twin (A3): optional — without it a batch loops per instance.
        if (ok)
            if (VkShaderModule ivs = loadShaderModule("scene_instanced.vert.spv"))
            {
                VkPipelineShaderStageCreateInfo ist[2] = { st[0], st[1] };
                ist[0].module = ivs;
                VkVertexInputBindingDescription ibnd[2] = {
                    { 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
                    { 1, k_instStride,       VK_VERTEX_INPUT_RATE_INSTANCE } };
                VkVertexInputAttributeDescription iat[11] = {
                    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
                    { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 },
                    { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT,  0 }, { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
                    { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 }, { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 },
                    { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64 }, { 8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 80 },
                    { 9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 96 }, { 10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 112 } };
                VkPipelineVertexInputStateCreateInfo ivi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
                ivi.vertexBindingDescriptionCount   = 2;  ivi.pVertexBindingDescriptions   = ibnd;
                ivi.vertexAttributeDescriptionCount = 11; ivi.pVertexAttributeDescriptions = iat;
                VkGraphicsPipelineCreateInfo ipci = pci;
                ipci.pStages = ist; ipci.pVertexInputState = &ivi;
                if (createGraphicsPipelines(1, &ipci, nullptr,
                                              &m_gbufInstancedPipeline) != VK_SUCCESS)
                    m_gbufInstancedPipeline = VK_NULL_HANDLE;
                vkDestroyShaderModule(m_device, ivs, nullptr);
            }
        vkDestroyShaderModule(m_device, vs, nullptr);
        vkDestroyShaderModule(m_device, fs, nullptr);
        if (!ok)
        {
            m_gbufPipeline = VK_NULL_HANDLE;
            return fail("G-buffer pipeline creation failed");
        }
    }

    // ── Resolve set: HE::vkmat::kResolveBindings (VulkanMaterialLayout.h, the
    // table he_tests reflects both resolve variants against), one persistent
    // set per frame slot, its own HeLighting + HeResolve buffers.
    {
        auto vkType = [](HE::vkmat::DescKind k) -> VkDescriptorType {
            switch (k)
            {
            case HE::vkmat::DescKind::UniformBuffer:        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case HE::vkmat::DescKind::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            case HE::vkmat::DescKind::StorageBuffer:        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        };
        VkDescriptorSetLayoutBinding b[HE::vkmat::kResolveBindingCount]{};
        for (uint32_t i = 0; i < HE::vkmat::kResolveBindingCount; ++i)
        {
            b[i].binding         = HE::vkmat::kResolveBindings[i].binding;
            b[i].descriptorType  = vkType(HE::vkmat::kResolveBindings[i].kind);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = HE::vkmat::kResolveBindings[i].stages;
        }
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = HE::vkmat::kResolveBindingCount;
        slci.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_resolveSetLayout) != VK_SUCCESS)
            return fail("resolve descriptor-set layout failed");
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1;
        plci.pSetLayouts    = &m_resolveSetLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_resolvePipeLayout) != VK_SUCCESS)
            return fail("resolve pipeline layout failed");
        using HE::vkmat::DescKind;
        using HE::vkmat::resolveCountOf;
        const VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         resolveCountOf(DescKind::UniformBuffer)        * k_maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, resolveCountOf(DescKind::CombinedImageSampler) * k_maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         resolveCountOf(DescKind::StorageBuffer)        * k_maxFramesInFlight },
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_maxFramesInFlight;
        dpci.poolSizeCount = 3;
        dpci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_resolvePool) != VK_SUCCESS)
            return fail("resolve descriptor pool failed");
        static_assert(sizeof(m_resolveSet) / sizeof(m_resolveSet[0]) == k_maxFramesInFlight,
                      "one resolve set per frame slot");
        VkDescriptorSetLayout layouts[k_maxFramesInFlight];
        for (VkDescriptorSetLayout& l : layouts) l = m_resolveSetLayout;
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_resolvePool;
        dsai.descriptorSetCount = k_maxFramesInFlight;
        dsai.pSetLayouts        = layouts;
        if (vkAllocateDescriptorSets(m_device, &dsai, m_resolveSet) != VK_SUCCESS)
            return fail("resolve descriptor sets failed");

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
        for (uint32_t f = 0; f < k_maxFramesInFlight; ++f)
            if (!makeBuf(sizeof(MSL::Lighting), m_resolveLightBuf[f])
                || !makeBuf(sizeof(MSL::ResolveUniforms), m_resolveUboBuf[f]))
                return fail("resolve uniform buffers failed");
    }

    // ── Resolve pipelines: fullscreen triangle (no vertex input, no varyings)
    // into the HDR target, no depth test — the sky drawn before stays where the
    // resolve discards d >= 1. Built against m_postFxSceneRP, run in m_hdrLoadRP.
    {
        const MSL::Compiled& vc = m_matShaderLib.fullscreenVertex(Backend::SpirV);
        const MSL::Compiled& rc = m_matShaderLib.deferredResolve(Backend::SpirV);
        if (!vc.ok || !rc.ok || vc.spirv.empty() || rc.spirv.empty())
        {
            HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: deferred resolve cross-compile failed: ")
                + vc.log + " " + rc.log).c_str());
            return fail("deferred resolve cross-compile failed");
        }
        auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = spv.size() * sizeof(uint32_t);
            ci.pCode    = spv.data();
            VkShaderModule mod = VK_NULL_HANDLE;
            return vkCreateShaderModule(m_device, &ci, nullptr, &mod) == VK_SUCCESS ? mod : VK_NULL_HANDLE;
        };
        VkShaderModule rvs = makeModule(vc.spirv);
        auto makeResolve = [&](const std::vector<uint32_t>& fsWords, VkPipeline& out) -> bool {
            VkShaderModule rfs = makeModule(fsWords);
            if (!rvs || !rfs) { if (rfs) vkDestroyShaderModule(m_device, rfs, nullptr); return false; }
            VkPipelineShaderStageCreateInfo st[2]{};
            st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = rvs; st[0].pName = "main";
            st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rfs; st[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable  = VK_FALSE; // the depth is GB3 here, not a test
            ds.depthWriteEnable = VK_FALSE;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1;
            cb.pAttachments    = &noBlend;
            VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pci.stageCount = 2;                pci.pStages             = st;
            pci.pVertexInputState   = &vi;     pci.pInputAssemblyState = &ia;
            pci.pViewportState      = &vp;     pci.pRasterizationState = &rs;
            pci.pMultisampleState   = &ms;     pci.pDepthStencilState  = &ds;
            pci.pColorBlendState    = &cb;     pci.pDynamicState       = &dyn;
            pci.layout = m_resolvePipeLayout;  pci.renderPass = m_postFxSceneRP; pci.subpass = 0;
            const bool ok = createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
            vkDestroyShaderModule(m_device, rfs, nullptr);
            if (!ok) out = VK_NULL_HANDLE;
            return ok;
        };
        const bool ok = makeResolve(rc.spirv, m_resolvePipe);
        // The clustered resolve only where the cluster SSBOs exist (the same
        // rule as the clustered graph materials). A failure costs the
        // clustering, not the path: the window resolve still lights 8 lights.
        if (ok && m_forwardClustered && m_clusterReady)
        {
            const MSL::Compiled& cc = m_matShaderLib.deferredResolveClustered(Backend::SpirV);
            if (!cc.ok || cc.spirv.empty())
                HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: clustered resolve cross-compile failed — "
                    "deferred keeps the 8-light window: ") + cc.log).c_str());
            else if (!makeResolve(cc.spirv, m_resolveClusteredPipe))
                HE_LOG_WARN(RHI, "%s", "VulkanRenderer: clustered resolve pipeline failed — "
                                       "deferred keeps the 8-light window");
        }
        if (rvs) vkDestroyShaderModule(m_device, rvs, nullptr);
        if (!ok) return fail("deferred resolve pipeline creation failed");
    }

    m_deferredReady = true;
    HE_LOG_INFO(RHI, "VulkanRenderer: deferred path ready (G-buffer + %s resolve)",
                m_resolveClusteredPipe ? "clustered" : "8-light window");
    return true;
#endif
}

// Device idle (Shutdown, or the failed build above). The G-buffer targets go
// with destroyPostFXResources.
void VulkanRenderer::destroyDeferredPipelines()
{
    m_deferredReady = false;
    for (VkPipeline* p : { &m_gbufPipeline, &m_gbufInstancedPipeline, &m_resolvePipe, &m_resolveClusteredPipe })
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_resolvePipeLayout) { vkDestroyPipelineLayout(m_device, m_resolvePipeLayout, nullptr); m_resolvePipeLayout = VK_NULL_HANDLE; }
    if (m_resolvePool)       { vkDestroyDescriptorPool(m_device, m_resolvePool, nullptr);       m_resolvePool       = VK_NULL_HANDLE; }
    for (VkDescriptorSet& s : m_resolveSet) s = VK_NULL_HANDLE; // freed with the pool
    if (m_resolveSetLayout)  { vkDestroyDescriptorSetLayout(m_device, m_resolveSetLayout, nullptr); m_resolveSetLayout = VK_NULL_HANDLE; }
    for (uint32_t f = 0; f < k_maxFramesInFlight; ++f)
        for (MatFrameBuf* mb : { &m_resolveLightBuf[f], &m_resolveUboBuf[f] })
        {
            if (mb->mapped) vkUnmapMemory(m_device, mb->mem);
            if (mb->buf)    vkDestroyBuffer(m_device, mb->buf, nullptr);
            if (mb->mem)    vkFreeMemory(m_device, mb->mem, nullptr);
            *mb = {};
        }
    if (m_gbSampler) { vkDestroySampler(m_device, m_gbSampler, nullptr);     m_gbSampler = VK_NULL_HANDLE; }
    if (m_hdrLoadRP) { vkDestroyRenderPass(m_device, m_hdrLoadRP, nullptr);  m_hdrLoadRP = VK_NULL_HANDLE; }
    if (m_gbDecalRP) { vkDestroyRenderPass(m_device, m_gbDecalRP, nullptr);  m_gbDecalRP = VK_NULL_HANDLE; }
    if (m_gbufferRP) { vkDestroyRenderPass(m_device, m_gbufferRP, nullptr);  m_gbufferRP = VK_NULL_HANDLE; }
    // The G-buffer variants of the graph materials were built against
    // m_gbufferRP; they live in m_materialPipelines and go with it.
}

// The G-buffer at the viewport size, built on the first deferred frame (a
// forward-only session never allocates it) and dropped with the PostFX
// targets on every resize (destroyPostFXResources, GPU idle there), so a size
// change rebuilds it lazily against the new viewport depth.
bool VulkanRenderer::ensureGBufferTargets(uint32_t w, uint32_t h)
{
    if (m_gbFB && m_gbW == w && m_gbH == h) return true;
    if (!m_gbufferRP || !m_viewportDepthView || w == 0 || h == 0) return false;
    if (m_gbFB)
    {
        // Only reachable if the viewport changed size without the PostFX
        // targets being rebuilt — frames in flight still use the old ones.
        deviceWaitIdle();
        destroyGBufferTargets();
    }
    static const char* const kNames[k_gbTargets] = { "G-buffer 0 (base, metallic)",
                                                     "G-buffer 1 (normal, rough, spec)",
                                                     "G-buffer 2 (emissive, AO)",
                                                     "G-buffer 3 (depth)" };
    bool ok = true;
    for (uint32_t i = 0; ok && i < k_gbTargets; ++i)
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = kGBufferFormats[i];
        ici.extent        = { w, h, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ok = vkCreateImage(m_device, &ici, nullptr, &m_gbImage[i]) == VK_SUCCESS;
        if (!ok) break;
        nameImage(m_gbImage[i], kNames[i]);
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, m_gbImage[i], &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = vkAllocateMemory(m_device, &mai, nullptr, &m_gbMem[i]) == VK_SUCCESS;
        if (!ok) break;
        vkBindImageMemory(m_device, m_gbImage[i], m_gbMem[i], 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image            = m_gbImage[i];
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = kGBufferFormats[i];
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        ok = vkCreateImageView(m_device, &vci, nullptr, &m_gbView[i]) == VK_SUCCESS;
    }
    if (ok)
    {
        // The SAME depth image as m_hdrFB: the tail tests against what the
        // G-buffer pass wrote.
        VkImageView atts[k_gbTargets + 1] = { m_gbView[0], m_gbView[1], m_gbView[2], m_gbView[3],
                                              m_viewportDepthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_gbufferRP;
        fci.attachmentCount = k_gbTargets + 1;
        fci.pAttachments    = atts;
        fci.width           = w;
        fci.height          = h;
        fci.layers          = 1;
        ok = vkCreateFramebuffer(m_device, &fci, nullptr, &m_gbFB) == VK_SUCCESS;
    }
    if (ok && m_gbDecalRP)
    {
        // GB0 alone for the deferred decals. Optional like the pass.
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = m_gbDecalRP;
        fci.attachmentCount = 1;
        fci.pAttachments    = &m_gbView[0];
        fci.width           = w;
        fci.height          = h;
        fci.layers          = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_gbDecalFB) != VK_SUCCESS)
            m_gbDecalFB = VK_NULL_HANDLE;
    }
    if (!ok)
    {
        destroyGBufferTargets();
        if (!m_gbFailed)
        {
            m_gbFailed = true;
            HE_LOG_ERROR(RHI, "VulkanRenderer: G-buffer allocation failed (%ux%u) — frame stays forward", w, h);
        }
        return false;
    }
    m_gbW = w;
    m_gbH = h;
    return true;
}

void VulkanRenderer::destroyGBufferTargets()
{
    if (!m_device) return;
    if (m_gbFB) { vkDestroyFramebuffer(m_device, m_gbFB, nullptr); m_gbFB = VK_NULL_HANDLE; }
    if (m_gbDecalFB) { vkDestroyFramebuffer(m_device, m_gbDecalFB, nullptr); m_gbDecalFB = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < k_gbTargets; ++i)
    {
        if (m_gbView[i])  { vkDestroyImageView(m_device, m_gbView[i], nullptr); m_gbView[i]  = VK_NULL_HANDLE; }
        if (m_gbImage[i]) { vkDestroyImage(m_device, m_gbImage[i], nullptr);    m_gbImage[i] = VK_NULL_HANDLE; }
        if (m_gbMem[i])   { vkFreeMemory(m_device, m_gbMem[i], nullptr);        m_gbMem[i]   = VK_NULL_HANDLE; }
    }
    m_gbW = m_gbH = 0;
}

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

    // Underwater fog's own 1-binding layout (depth only — see the header's
    // m_underwaterFogDSLayout comment for why it does not share m_postFxDSLayout).
    {
        VkDescriptorSetLayoutBinding b0{};
        b0.binding = 0; b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b0.descriptorCount = 1; b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b0.pImmutableSamplers = &m_postFxSampler;
        VkDescriptorSetLayoutCreateInfo slci1{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci1.bindingCount = 1; slci1.pBindings = &b0;
        vkCheck(vkCreateDescriptorSetLayout(m_device, &slci1, nullptr, &m_underwaterFogDSLayout), "underwaterfog dsl");
    }

    // 5 chain sets + m_presentDS (the game's present pass), 2 samplers each,
    // + 1 underwater-fog set, 1 sampler.
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 13 };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 7; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    vkCheck(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_postFxDSPool), "postfx pool");

    VkDescriptorSetLayout layouts[5]; for (auto& l : layouts) l = m_postFxDSLayout;
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = m_postFxDSPool; dsai.descriptorSetCount = 5; dsai.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(m_device, &dsai, m_postFxDS), "postfx ds alloc");
    dsai.descriptorSetCount = 1;
    vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &m_presentDS), "present ds alloc");
    dsai.pSetLayouts = &m_underwaterFogDSLayout;
    vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &m_underwaterFogDS), "underwaterfog ds alloc");

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
        submitGraphics(si, VK_NULL_HANDLE);
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
        // STORE, not DONT_CARE: the TAA velocity pass (m_taaVelocityRP) loads
        // this depth to test against. Load/store ops do not take part in
        // render-pass compatibility, so every HDR pipeline stays valid.
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        // KEEP IN SYNC with m_hdrLoadRP (createDeferredPipelines): subpass
        // dependencies are part of render-pass compatibility.
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
    // Not in the "missing → abort the whole chain" check below: the underwater
    // fog pass is optional (m_underwaterFogReady gates it independently, like
    // D3D11/D3D12's underwaterFogReady) — a scene with no water should not lose
    // bloom/tonemap/FXAA over a missing fog shader.
    VkShaderModule uwFS = loadShaderModule("postfx_underwaterfog.frag.spv");
    // Same for the caustics (Thema 185 Schritt 3): optional, gated by
    // m_underwaterCausticsReady on its own.
    VkShaderModule ucFS = loadShaderModule("postfx_underwatercaustics.frag.spv");

    if (!vsM || !tmFS || !fxFS || !smFS || !btFS || !brFS || !blFS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: PostFX shaders missing — no HDR/bloom/FXAA");
        for (auto m : {vsM,tmFS,fxFS,smFS,btFS,brFS,blFS,uwFS,ucFS}) if (m) vkDestroyShaderModule(m_device,m,nullptr);
        return;
    }

    auto makePipe = [&](VkShaderModule fs, VkRenderPass rp, VkPipeline& out,
                        const VkPipelineDepthStencilStateCreateInfo* depth = nullptr) {
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
        pci.pDepthStencilState = depth;
        pci.pDynamicState = &dyn; pci.layout = m_postFxPipeLayout;
        pci.renderPass = rp; pci.subpass = 0;
        vkCheck(createGraphicsPipelines(1, &pci, nullptr, &out), "postfx pipeline");
    };

    makePipe(brFS, m_postFxBlitF16, m_bloomBrightPipe);
    makePipe(blFS, m_postFxBlitF16, m_bloomBlurPipe);
    makePipe(tmFS, m_postFxBlitF8,  m_tonemapPipe);
    makePipe(fxFS, m_postFxFinalRP, m_fxaaPipe);
    makePipe(smFS, m_postFxFinalRP, m_smaaPipe);     // AA = SMAA
    makePipe(btFS, m_postFxFinalRP, m_aaBlitPipe);   // AA = Off
    // The game's present (SetSwapchainPostProcessing): the finished RGBA8
    // viewport image into the swapchain pass. A pass, not vkCmdCopyImage —
    // RGBA→BGRA would swap red and blue — and not a blit, which would need
    // TRANSFER_DST on the swapchain and a LOAD variant of m_renderPass. That
    // subpass has a depth attachment, so the pipeline states one (off).
    {
        VkPipelineDepthStencilStateCreateInfo noDepth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        makePipe(btFS, m_renderPass, m_presentPipe, &noDepth);
    }

    // Underwater fog (Thema 185 Schritt 2): own render pass (loadOp=LOAD, every
    // other postFx pass here fully overwrites its target) + own pipeline layout
    // (112-byte fragment push constant + the 1-binding depth-only set above).
    if (uwFS)
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R16G16B16A16_SFLOAT; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        vkCheck(vkCreateRenderPass(m_device, &rpci, nullptr, &m_underwaterFogRP), "underwaterfog render pass");

        VkPushConstantRange pcr2{};
        pcr2.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr2.size = 128;   // mat4 + 4×vec4, see the .frag
        VkPipelineLayoutCreateInfo plci2{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci2.setLayoutCount = 1; plci2.pSetLayouts = &m_underwaterFogDSLayout;
        plci2.pushConstantRangeCount = 1; plci2.pPushConstantRanges = &pcr2;
        vkCheck(vkCreatePipelineLayout(m_device, &plci2, nullptr, &m_underwaterFogPipeLayout), "underwaterfog pipe layout");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vsM; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = uwFS; stages[1].pName = "main";
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
        // Premultiplied shader output: dst' = src*1 + dst*(1-fogAmount).
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
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
        pci.pDynamicState = &dyn; pci.layout = m_underwaterFogPipeLayout;
        pci.renderPass = m_underwaterFogRP; pci.subpass = 0;
        m_underwaterFogReady = m_underwaterFogRP && m_underwaterFogPipeLayout &&
            createGraphicsPipelines(1, &pci, nullptr, &m_underwaterFogPipe) == VK_SUCCESS;

        // Underwater caustics (Thema 185 Schritt 3): the fog's render pass, descriptor set
        // layout and every fixed-function state above (incl. the blend — ONE / ONE_MINUS_SRC_ALPHA
        // gives dst' = rgb + dst * (1 - a): the lines' light, the cells' darkening), only the
        // fragment shader and the push-constant range differ: 128 bytes = mat4 + 4×vec4, see
        // the .frag. That is why it has its own pipeline layout and sits in this block (it
        // needs m_underwaterFogRP, which exists only here).
        if (ucFS)
        {
            VkPushConstantRange pcr3{};
            pcr3.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr3.size = 128;
            VkPipelineLayoutCreateInfo plci3{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci3.setLayoutCount = 1; plci3.pSetLayouts = &m_underwaterFogDSLayout;
            plci3.pushConstantRangeCount = 1; plci3.pPushConstantRanges = &pcr3;
            vkCheck(vkCreatePipelineLayout(m_device, &plci3, nullptr, &m_underwaterCausticsPipeLayout), "underwatercaustics pipe layout");

            stages[1].module = ucFS;
            pci.layout = m_underwaterCausticsPipeLayout;
            m_underwaterCausticsReady = m_underwaterFogRP && m_underwaterCausticsPipeLayout &&
                createGraphicsPipelines(1, &pci, nullptr, &m_underwaterCausticsPipe) == VK_SUCCESS;
        }
    }

    createTaaPipelines(vsM);                         // optional — see taaReady()

    for (auto m : {vsM,tmFS,fxFS,smFS,btFS,brFS,blFS,uwFS,ucFS}) if (m) vkDestroyShaderModule(m_device,m,nullptr);

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
            if (createGraphicsPipelines(1, &pci2, nullptr, &m_scenePipelineHDR) != VK_SUCCESS)
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
                if (createGraphicsPipelines(1, &ipci2, nullptr, &m_sceneInstancedPipelineHDR) != VK_SUCCESS)
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
                if (createGraphicsPipelines(1, &tpci2, nullptr, &m_sceneTransparentPipelineHDR) != VK_SUCCESS)
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
    destroyTaaPipelines();
    for (auto p : {m_bloomBrightPipe,m_bloomBlurPipe,m_tonemapPipe,m_fxaaPipe,m_smaaPipe,m_aaBlitPipe,m_presentPipe})
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    m_bloomBrightPipe=m_bloomBlurPipe=m_tonemapPipe=m_fxaaPipe=m_smaaPipe=m_aaBlitPipe=m_presentPipe=VK_NULL_HANDLE;
    m_presentDS = VK_NULL_HANDLE;   // freed with the pool below
    m_underwaterCausticsReady = false;
    if (m_underwaterCausticsPipe)       { vkDestroyPipeline(m_device, m_underwaterCausticsPipe, nullptr); m_underwaterCausticsPipe=VK_NULL_HANDLE; }
    if (m_underwaterCausticsPipeLayout) { vkDestroyPipelineLayout(m_device, m_underwaterCausticsPipeLayout, nullptr); m_underwaterCausticsPipeLayout=VK_NULL_HANDLE; }
    m_underwaterFogReady = false;
    m_underwaterFogDS = VK_NULL_HANDLE;   // freed with the pool below
    if (m_underwaterFogPipe)       { vkDestroyPipeline(m_device, m_underwaterFogPipe, nullptr); m_underwaterFogPipe=VK_NULL_HANDLE; }
    if (m_underwaterFogPipeLayout) { vkDestroyPipelineLayout(m_device, m_underwaterFogPipeLayout, nullptr); m_underwaterFogPipeLayout=VK_NULL_HANDLE; }
    if (m_underwaterFogDSLayout)   { vkDestroyDescriptorSetLayout(m_device, m_underwaterFogDSLayout, nullptr); m_underwaterFogDSLayout=VK_NULL_HANDLE; }
    if (m_underwaterFogRP)         { vkDestroyRenderPass(m_device, m_underwaterFogRP, nullptr); m_underwaterFogRP=VK_NULL_HANDLE; }
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
    // TRANSFER_SRC on the HDR target only: forward SSR copies the finished frame
    // into m_ssrColorHist for the NEXT frame's trace (CaptureSSRColorHistory).
    makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, w,  h,  RTf | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            m_hdrImage, m_hdrMemory, m_hdrView);
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
    // Underwater fog (Thema 185 Schritt 2): a second, LOAD-op framebuffer over
    // the SAME m_hdrView — valid in Vulkan since load/store ops are not part of
    // render-pass compatibility (see m_postFxSceneRP's depth attachment comment).
    if (m_underwaterFogRP)
        makeFB(m_underwaterFogRP, m_hdrView, w, h, m_underwaterFogFB);
    // UI overlay framebuffer — uses the same viewport image with LOAD_OP_LOAD render pass.
    if (m_uiViewportRP && m_viewportView)
        makeFB(m_uiViewportRP, m_viewportView, w, h, m_uiViewportFB);

    // Write descriptor sets: [0]=bloomBright, [1]=blurH(bloom[0]), [2]=blurV(bloom[1]),
    //                         [3]=tonemap, [4]=fxaa
    //                         and the game's present pass (the finished viewport image)
    struct DSEntry { VkDescriptorSet set; VkImageView t0; VkImageView t1; };
    DSEntry dss[6] = {
        { m_postFxDS[0], m_hdrView,      m_dummyView    },
        { m_postFxDS[1], m_bloomView[0], m_dummyView    },
        { m_postFxDS[2], m_bloomView[1], m_dummyView    },
        { m_postFxDS[3], m_hdrView,      m_bloomView[0] },
        { m_postFxDS[4], m_ldrView,      m_dummyView    },
        { m_presentDS,   m_viewportView ? m_viewportView : m_dummyView, m_dummyView },
    };
    for (auto& d : dss)
    {
        if (!d.set) continue;
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

    // Underwater fog's own 1-binding set: the viewport depth, read via a
    // layout flip around the draw (see the Render() call site), not a
    // ping-pong copy — so this points at m_viewportDepthView directly.
    if (m_underwaterFogDS && m_viewportDepthView)
    {
        VkDescriptorImageInfo dii{ VK_NULL_HANDLE, m_viewportDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet dw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        dw.dstSet = m_underwaterFogDS; dw.dstBinding = 0; dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; dw.pImageInfo = &dii;
        vkUpdateDescriptorSets(m_device, 1, &dw, 0, nullptr);
    }

    // TAA's targets follow the same size and read m_ldrView / the viewport
    // depth just (re)built; the GPU is idle (destroyPostFXResources above).
    if (taaWanted()) createTaaTargets(w, h);
}

void VulkanRenderer::destroyPostFXResources()
{
    deviceWaitIdle();
    destroyTaaTargets();
    // The G-buffer framebuffer holds the viewport depth view: it goes before
    // that (destroyViewportResources) and is rebuilt lazily at the new size.
    destroyGBufferTargets();
    if (m_uiViewportFB) { vkDestroyFramebuffer(m_device, m_uiViewportFB, nullptr); m_uiViewportFB = VK_NULL_HANDLE; }
    if (m_fxaaFB)     { vkDestroyFramebuffer(m_device, m_fxaaFB,     nullptr); m_fxaaFB=VK_NULL_HANDLE; }
    if (m_ldrFB)      { vkDestroyFramebuffer(m_device, m_ldrFB,      nullptr); m_ldrFB=VK_NULL_HANDLE; }
    if (m_bloomFB[1]) { vkDestroyFramebuffer(m_device, m_bloomFB[1], nullptr); m_bloomFB[1]=VK_NULL_HANDLE; }
    if (m_bloomFB[0]) { vkDestroyFramebuffer(m_device, m_bloomFB[0], nullptr); m_bloomFB[0]=VK_NULL_HANDLE; }
    if (m_underwaterFogFB) { vkDestroyFramebuffer(m_device, m_underwaterFogFB, nullptr); m_underwaterFogFB=VK_NULL_HANDLE; }
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

// ─── Temporal AA (docs/anti-aliasing-plan.md A2/A3) ───────────────────────────
// Three pipelines, all optional: a missing .spv or a failed create leaves
// taaReady() false, GetCapabilities then reports no temporal AA and
// ResolveAAMethod falls back to SMAA, exactly as on a backend without TAA.
void VulkanRenderer::createTaaPipelines(VkShaderModule fullscreenVS)
{
    VkShaderModule velVS = loadShaderModule("taa_velocity.vert.spv");
    VkShaderModule velFS = loadShaderModule("taa_velocity.frag.spv");
    VkShaderModule resFS = loadShaderModule("taa_resolve.frag.spv");
    VkShaderModule shpFS = loadShaderModule("taa_sharpen.frag.spv");
    auto releaseModules = [&]() {
        for (VkShaderModule m : { velVS, velFS, resFS, shpFS })
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!fullscreenVS || !velVS || !velFS || !resFS || !shpFS || !m_scenePipelineLayout)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: TAA shaders missing — temporal AA unavailable");
        releaseModules();
        return;
    }

    // Velocity pass: RG16F cleared to zero ("did not move") + the scene depth,
    // LOADed from the scene pass (which STOREs it) and only tested. The external
    // dependency makes the scene's depth writes visible to the depth test and
    // orders the colour write after last frame's resolve read.
    {
        VkAttachmentDescription atts[2]{};
        atts[0].format = VK_FORMAT_R16G16_SFLOAT; atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[1].format = m_depthFormat; atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                          | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 2; rpci.pAttachments = atts;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_taaVelocityRP) != VK_SUCCESS)
            m_taaVelocityRP = VK_NULL_HANDLE;
    }

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    // Rasterizer exactly as the scene pipelines (no culling, CCW, no depth
    // clamp), so the velocity raster reproduces the scene's depth.
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    // Velocity pipeline: positions only (location 0 of the scene's 32-byte
    // vertex), the scene pipeline layout's 128 B of vertex push constants.
    if (m_taaVelocityRP)
    {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = velVS; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = velFS; st[1].pName = "main";
        VkVertexInputBindingDescription   vib{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription via{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
        vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &via;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2; pci.pStages = st;
        pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vps; pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb; pci.pDynamicState = &dyn;
        pci.layout = m_scenePipelineLayout; pci.renderPass = m_taaVelocityRP; pci.subpass = 0;
        if (createGraphicsPipelines(1, &pci, nullptr, &m_taaVelocityPipe) != VK_SUCCESS)
            m_taaVelocityPipe = VK_NULL_HANDLE;
    }

    // Resolve: three samplers (current, history, velocity) through the postFx
    // sampler — texelFetch ignores it, the history's linear fetch uses it.
    {
        VkDescriptorSetLayoutBinding binds[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            binds[i].binding = i; binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[i].descriptorCount = 1; binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            binds[i].pImmutableSamplers = &m_postFxSampler;
        }
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 3; slci.pBindings = binds;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_taaResolveDSL) != VK_SUCCESS)
            m_taaResolveDSL = VK_NULL_HANDLE;
    }
    if (m_taaResolveDSL)
    {
        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_taaResolveDSL;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_taaResolvePipeLayout) != VK_SUCCESS)
            m_taaResolvePipeLayout = VK_NULL_HANDLE;
    }
    // Two resolve sets (one per ping-pong direction) + two sharpen sets in the
    // postFx layout ({history[cur], dummy}); written by createTaaTargets.
    if (m_taaResolveDSL && m_postFxDSLayout)
    {
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 3 + 2 * 2 };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 4; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_taaDSPool) == VK_SUCCESS)
        {
            const VkDescriptorSetLayout layouts[4] = { m_taaResolveDSL, m_taaResolveDSL,
                                                       m_postFxDSLayout, m_postFxDSLayout };
            VkDescriptorSet sets[4]{};
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = m_taaDSPool; dsai.descriptorSetCount = 4; dsai.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(m_device, &dsai, sets) == VK_SUCCESS)
            {
                m_taaResolveDS[0] = sets[0]; m_taaResolveDS[1] = sets[1];
                m_taaSharpenDS[0] = sets[2]; m_taaSharpenDS[1] = sets[3];
            }
            else
            {
                vkDestroyDescriptorPool(m_device, m_taaDSPool, nullptr);
                m_taaDSPool = VK_NULL_HANDLE;
            }
        }
        else m_taaDSPool = VK_NULL_HANDLE;
    }

    // The two fullscreen passes: postfx.vert + no vertex input, no depth.
    auto makeFullscreen = [&](VkShaderModule fs, VkPipelineLayout layout, VkRenderPass rp,
                              VkPipeline& out) {
        if (!layout || !rp) return;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = fullscreenVS; st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;           st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2; pci.pStages = st;
        pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vps; pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
        pci.pDynamicState = &dyn; pci.layout = layout;
        pci.renderPass = rp; pci.subpass = 0;
        if (createGraphicsPipelines(1, &pci, nullptr, &out) != VK_SUCCESS)
            out = VK_NULL_HANDLE;
    };
    // The resolve writes a history image through the RGBA8 blit pass (left in
    // COLOR_ATTACHMENT, parked in SHADER_READ_ONLY by an explicit barrier, the
    // tonemap's pattern); the sharpen writes the viewport image in the AA slot.
    makeFullscreen(resFS, m_taaResolvePipeLayout, m_postFxBlitF8,  m_taaResolvePipe);
    makeFullscreen(shpFS, m_postFxPipeLayout,     m_postFxFinalRP, m_taaSharpenPipe);
    releaseModules();

    // Not taaReady(): that also asks m_postFxReady, which createPostFXPipelines
    // sets only AFTER this call — it would tear down a complete set.
    if (!(m_taaVelocityRP && m_taaVelocityPipe && m_taaResolvePipe && m_taaSharpenPipe && m_taaDSPool))
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: TAA pipelines incomplete — temporal AA unavailable");
        destroyTaaPipelines();
    }
}

void VulkanRenderer::destroyTaaPipelines()
{
    for (VkPipeline p : { m_taaVelocityPipe, m_taaResolvePipe, m_taaSharpenPipe })
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    m_taaVelocityPipe = m_taaResolvePipe = m_taaSharpenPipe = VK_NULL_HANDLE;
    if (m_taaResolvePipeLayout) { vkDestroyPipelineLayout(m_device, m_taaResolvePipeLayout, nullptr); m_taaResolvePipeLayout = VK_NULL_HANDLE; }
    if (m_taaDSPool)            { vkDestroyDescriptorPool(m_device, m_taaDSPool, nullptr); m_taaDSPool = VK_NULL_HANDLE; }
    m_taaResolveDS[0] = m_taaResolveDS[1] = m_taaSharpenDS[0] = m_taaSharpenDS[1] = VK_NULL_HANDLE;
    if (m_taaResolveDSL)        { vkDestroyDescriptorSetLayout(m_device, m_taaResolveDSL, nullptr); m_taaResolveDSL = VK_NULL_HANDLE; }
    if (m_taaVelocityRP)        { vkDestroyRenderPass(m_device, m_taaVelocityRP, nullptr); m_taaVelocityRP = VK_NULL_HANDLE; }
}

// Velocity + two history images at the viewport size, their framebuffers and
// the four descriptor sets. Caller guarantees the GPU is idle (the sets may
// have been in flight). A fresh history is garbage, not history: the first
// frame after a (re)create shows the current frame only.
void VulkanRenderer::createTaaTargets(uint32_t w, uint32_t h)
{
    destroyTaaTargets();
    if (!taaReady() || !m_ldrView || !m_viewportDepthView || !m_dummyView || w == 0 || h == 0) return;

    auto makeImg = [&](VkFormat fmt, VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &img) != VK_SUCCESS) { img = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(m_device, img, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) { mem = VK_NULL_HANDLE; return false; }
        vkBindImageMemory(m_device, img, mem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return vkCreateImageView(m_device, &vci, nullptr, &view) == VK_SUCCESS;
    };
    auto makeFB = [&](VkRenderPass rp, const VkImageView* views, uint32_t count, VkFramebuffer& fb) -> bool {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = rp; fci.attachmentCount = count; fci.pAttachments = views;
        fci.width = w; fci.height = h; fci.layers = 1;
        return vkCreateFramebuffer(m_device, &fci, nullptr, &fb) == VK_SUCCESS;
    };
    bool ok = makeImg(VK_FORMAT_R16G16_SFLOAT, m_taaVelocityImage, m_taaVelocityMemory, m_taaVelocityView)
           && makeImg(VK_FORMAT_R8G8B8A8_UNORM, m_taaHistoryImage[0], m_taaHistoryMemory[0], m_taaHistoryView[0])
           && makeImg(VK_FORMAT_R8G8B8A8_UNORM, m_taaHistoryImage[1], m_taaHistoryMemory[1], m_taaHistoryView[1]);
    if (ok)
    {
        const VkImageView va[2] = { m_taaVelocityView, m_viewportDepthView };
        ok = makeFB(m_taaVelocityRP, va, 2, m_taaVelocityFB)
          && makeFB(m_postFxBlitF8, &m_taaHistoryView[0], 1, m_taaHistoryFB[0])
          && makeFB(m_postFxBlitF8, &m_taaHistoryView[1], 1, m_taaHistoryFB[1]);
    }
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: TAA targets could not be created");
        destroyTaaTargets();
        return;
    }

    // Prime all three into SHADER_READ_ONLY: the resolve binds history[prev]
    // (and the velocity) from the very first frame, and sampling an image
    // still in UNDEFINED is invalid even at blend weight 0.
    {
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        VkImageMemoryBarrier b[3]{};
        const VkImage imgs[3] = { m_taaVelocityImage, m_taaHistoryImage[0], m_taaHistoryImage[1] };
        for (int i = 0; i < 3; ++i)
        {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = imgs[i]; b[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 3, b);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        submitGraphics(si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
    }

    // resolve[cur] = {ldr, history[1-cur], velocity}; sharpen[cur] = {history[cur], dummy}.
    auto writeSet = [&](VkDescriptorSet set, std::initializer_list<VkImageView> views) {
        VkDescriptorImageInfo infos[3]{};
        VkWriteDescriptorSet  writes[3]{};
        uint32_t n = 0;
        for (VkImageView v : views)
        {
            infos[n] = { VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[n].dstSet = set; writes[n].dstBinding = n; writes[n].descriptorCount = 1;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkUpdateDescriptorSets(m_device, n, writes, 0, nullptr);
    };
    writeSet(m_taaResolveDS[0], { m_ldrView, m_taaHistoryView[1], m_taaVelocityView });
    writeSet(m_taaResolveDS[1], { m_ldrView, m_taaHistoryView[0], m_taaVelocityView });
    writeSet(m_taaSharpenDS[0], { m_taaHistoryView[0], m_dummyView });
    writeSet(m_taaSharpenDS[1], { m_taaHistoryView[1], m_dummyView });

    m_taaW = w; m_taaH = h;
    m_taaHistoryCur   = 0;
    m_taaHistoryValid = false;
}

// Caller guarantees the GPU no longer uses the targets.
void VulkanRenderer::destroyTaaTargets()
{
    if (m_taaVelocityFB) { vkDestroyFramebuffer(m_device, m_taaVelocityFB, nullptr); m_taaVelocityFB = VK_NULL_HANDLE; }
    if (m_taaVelocityView)   { vkDestroyImageView(m_device, m_taaVelocityView, nullptr); m_taaVelocityView = VK_NULL_HANDLE; }
    if (m_taaVelocityImage)  { vkDestroyImage    (m_device, m_taaVelocityImage, nullptr); m_taaVelocityImage = VK_NULL_HANDLE; }
    if (m_taaVelocityMemory) { vkFreeMemory      (m_device, m_taaVelocityMemory, nullptr); m_taaVelocityMemory = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; ++i)
    {
        if (m_taaHistoryFB[i])     { vkDestroyFramebuffer(m_device, m_taaHistoryFB[i], nullptr); m_taaHistoryFB[i] = VK_NULL_HANDLE; }
        if (m_taaHistoryView[i])   { vkDestroyImageView  (m_device, m_taaHistoryView[i], nullptr); m_taaHistoryView[i] = VK_NULL_HANDLE; }
        if (m_taaHistoryImage[i])  { vkDestroyImage      (m_device, m_taaHistoryImage[i], nullptr); m_taaHistoryImage[i] = VK_NULL_HANDLE; }
        if (m_taaHistoryMemory[i]) { vkFreeMemory        (m_device, m_taaHistoryMemory[i], nullptr); m_taaHistoryMemory[i] = VK_NULL_HANDLE; }
    }
    m_taaW = m_taaH = 0;
    m_taaHistoryValid = false;
    m_taaPrevTransforms.clear();
    m_taaCurTransforms.clear();
}

// Targets while TAA is on, none while it is off — freed as soon as the mode is
// off, and the history with them (a stale one would blend against a different
// world the moment TAA comes back on). Waits for the GPU only when something
// actually changes, i.e. on a toggle. Must run before this frame's commands.
void VulkanRenderer::syncTaaTargets()
{
    const bool want = taaWanted() && m_ldrView && m_viewportW > 0 && m_viewportH > 0;
    const bool have = m_taaHistoryImage[0] && m_taaW == m_viewportW && m_taaH == m_viewportH;
    if (want ? have : !m_taaHistoryImage[0]) return;
    deviceWaitIdle();
    if (want) createTaaTargets(m_viewportW, m_viewportH);
    else      { destroyTaaTargets(); m_taaJitter = glm::vec2(0.0f); }
}

glm::mat4 VulkanRenderer::taaJittered(const glm::mat4& clipViewProj) const
{
    return m_taaFrame
        ? HE::taaJitteredViewProj(clipViewProj, m_taaJitter, static_cast<int>(m_viewportW),
                                  static_cast<int>(m_viewportH))
        : clipViewProj;
}

// Screen-space motion of the opaque geometry: for every visible object, where
// its vertices are now vs. where they were last frame (camera AND object
// motion). Depth-tested LESS_OR_EQUAL, no write, against the depth the scene
// pass stored — so only visible surfaces report; the sky, skinned meshes and
// the blended tail keep the clear's zero, as on GL. The raster matrix is the
// scene draws' own product (m_taaViewProjJit * transform), so it reproduces
// their depth. Leaves the velocity image in SHADER_READ_ONLY for the resolve.
void VulkanRenderer::encodeTaaVelocity(VkCommandBuffer cmd)
{
    GpuPassScope _passTimer(this, "Velocity");
    VkClearValue clears[2]{};
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };   // zero = "did not move"
    VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = m_taaVelocityRP; bi.framebuffer = m_taaVelocityFB;
    bi.renderArea.extent = { m_viewportW, m_viewportH };
    bi.clearValueCount = 2; bi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

    // Only when DrawScene reached its sort THIS frame — an empty scene returns
    // before it, and m_sortedIndices would still hold last frame's objects.
    if (m_taaSceneSorted)
    {
        VkViewport vp{ 0, 0, (float)m_viewportW, (float)m_viewportH, 0, 1 }; vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D   sc{ { 0, 0 }, { m_viewportW, m_viewportH } };          vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaVelocityPipe);

        // Last frame's clean matrix with THIS frame's jitter — see
        // taa_velocity.vert: the jitter then cancels out of the difference.
        const glm::mat4 prevJit = taaJittered(m_taaPrevViewProj);
        m_taaCurTransforms.clear();
        for (const uint32_t idx : m_sortedIndices)
        {
            const RenderObject& obj = m_renderWorld.objects[idx];
            const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
            if (!mesh || !mesh->vbuf || !mesh->ibuf || mesh->indexCount == 0) continue;

            // A foliage cluster stands for a bucket of plants: draw every one of them,
            // static (previous pose = this pose, so the velocity is the camera's own
            // motion), as GL and Metal do (Thema 182, 2f). The entity-keyed history
            // below holds ONE pose per entity, which a cluster must not use: every
            // plant of a layer shares the terrain's id.
            if (obj.isCluster())
            {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vbuf, &offset);
                vkCmdBindIndexBuffer(cmd, mesh->ibuf, 0, VK_INDEX_TYPE_UINT32);
                m_renderWorld.forEachInstance(obj, [&](const glm::mat4& model) {
                    const PushConstants pc{ m_taaViewProjJit * model, prevJit * model };
                    vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
                });
                continue;
            }

            // An object seen for the first time reports no motion — its
            // "previous" position is where it is now.
            const auto it = m_taaPrevTransforms.find(obj.entityId);
            const glm::mat4 prevModel = (it != m_taaPrevTransforms.end()) ? it->second : obj.transform;
            m_taaCurTransforms[obj.entityId] = obj.transform;

            const PushConstants pc{ m_taaViewProjJit * obj.transform, prevJit * prevModel };
            vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, mesh->ibuf, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh->indexCount, 1, 0, 0, 0);
        }
        // Advance the history HERE, at the end of the one pass that consumed
        // it, so a frame is never compared against itself.
        m_taaPrevViewProj = m_taaViewProjClean;
        m_taaPrevTransforms.swap(m_taaCurTransforms);
    }
    vkCmdEndRenderPass(cmd);
    runPostFXBarrier(cmd, m_taaVelocityImage,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// ─── Viewport offscreen render target ─────────────────────────────────────────

void VulkanRenderer::destroyViewportResources()
{
    destroySSAOTargets();
    destroyPostFXResources();
    destroyDecalDepth(m_decalDepthVp);
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

// Back to the 1×1 white AO fallback every scene set starts on, once the SSAO
// blur target that createSSAOTargets pointed binding 3 at is gone. Bindings
// 4–7 (GI) are untouched by destroyViewportResources and binding 8 is
// rewritten by every DrawScene.
void VulkanRenderer::pointSceneAoAtWhite()
{
    if (!m_ssaoWhiteView || !m_ssaoSampler) return;
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (!m_frameUBO[i].set) continue;
        VkDescriptorImageInfo wdii{ m_ssaoSampler, m_ssaoWhiteView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet aw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        aw.dstSet = m_frameUBO[i].set; aw.dstBinding = 3; aw.descriptorCount = 1;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        aw.pImageInfo = &wdii;
        vkUpdateDescriptorSets(m_device, 1, &aw, 0, nullptr);
    }
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
        // SAMPLED_BIT: the underwater fog pass (Thema 185 Schritt 2) samples this
        // same image directly as a depth texture — without it, binding
        // m_viewportDepthView into a COMBINED_IMAGE_SAMPLER descriptor is invalid
        // usage (VUID-VkWriteDescriptorSet-descriptorType-00337), which validation
        // layers (lavapipe in CI) treat as fatal and abort the whole process.
        ici.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
    createDecalDepth(m_decalDepthVp, w, h);
}

void  VulkanRenderer::SetViewportSize(uint32_t w, uint32_t h)
{
    m_viewportReqW = w;
    m_viewportReqH = h;
}

void VulkanRenderer::SetSwapchainPostProcessing(bool enabled)
{
    m_swapchainPostFx = enabled;   // Render() builds or drops the set
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
    deviceWaitIdle();
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
    submitGraphics(si, VK_NULL_HANDLE);
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

// ─── One still from somebody else's camera ────────────────────────────────────
// The contract is in IRenderer.h; the shape is D3D11's/D3D12's, which is Metal's
// without the capture flag: the live viewport set is SET ASIDE (the members are
// nulled, not released), createViewportResources builds a fresh set at the
// requested size, one DrawViewportFrame is recorded into a one-shot command
// buffer and submitted (the same offscreen half Render() records — minus the
// acquire, the backbuffer pass, the ImGui overlay, the timestamp pair and the
// present), CaptureViewport reads it back, the screenshot set is destroyed and
// the live set moved back. The editor's ImGui descriptor never saw the
// screenshot image: m_viewportResChanged goes back to its saved value before the
// editor's next look, so GetViewportVkImageView stays the live view.
//
// Command buffer and ring slot: the still records on its OWN one-shot buffer
// from m_cmdPool (CaptureViewport's pattern), not on m_cmdBufs[imageIndex] —
// those belong to a swapchain image, and no image is acquired here. The
// per-frame rings (m_frameUBO, m_matPool, m_instanceBuf, the GI per-slot
// buffers, …) are still indexed by m_currentFrame, the slot the NEXT real frame
// will take. That is safe only because of the vkDeviceWaitIdle below: nothing is
// in flight, DrawScene's vkResetDescriptorPool(m_matPool[m_currentFrame]) frees
// sets no GPU is reading, and since nothing is presented m_currentFrame does not
// advance — the next Render() lands on the same slot, whose fence is signalled
// and whose command buffer it re-records from scratch, throwing the still's
// bookkeeping away.
//
// Three repairs Vulkan has to do itself, which Metal and GL get for free:
//
// * The PostFX targets, the SSAO targets and the viewport decal depth are built
//   ONLY by createViewportResources, and Render() only calls that when the
//   request differs from the live size — which it will not, once the live size
//   is back. Unlike D3D11/D3D12 they are rebuilt here UNCONDITIONALLY, not just
//   on a size change: m_hdrFB, m_fxaaFB and m_uiViewportFB are framebuffers
//   built ON m_viewportView / m_viewportDepthView (createPostFXResources), so
//   after the screenshot's views are destroyed they dangle even when the
//   request was exactly viewport-sized.
// * createSSAOTargets rewrites scene binding 3 (the AO sampler) on every
//   per-frame descriptor set, so it has to run again for the live blur target.
//   Without a live viewport there is no size to rebuild against and binding 3
//   goes back to the 1×1 white fallback the renderer starts on.
// * The per-camera temporal state is TAA's history and forward SSR's (last
//   frame's full-res HDR copy plus its half-res ping-pong). Both get GL's TAA
//   rule: invalid before the still (implicitly — createViewportResources
//   rebuilds the TAA targets with the postFx ones, CaptureSSRColorHistory
//   reallocates on a size change, destroySSAOTargets drops the reflection
//   pre-pass) and invalid after, so the screenshot does not blend or reflect
//   the viewport's past and the viewport not the screenshot's. One unconverged
//   / SSR-less frame on each side, nothing out of a foreign camera. The GI shadow
//   accumulation (m_giHistValid, world-space reprojected) stays, as on Metal/GL.
//
// Not parity with Metal/GL, and stated as such: the GI targets and the SSR
// ping-pong resize lazily inside runGi/RenderForwardSSR, so a request that
// differs from the live size costs extra reallocations on both sides. A request
// in viewport size pays none of that.
//
// Not run: the retired-viewport ageing sweep, the timestamp pair, the overlay,
// the present — those count in REAL frames. Per-request path, not per-frame.
bool VulkanRenderer::RenderSceneImage(const EditorCameraOverride& camera, uint32_t width,
                                      uint32_t height, std::vector<uint8_t>& rgba)
{
    if (!m_device || !m_graphicsQueue || !m_cmdPool) return false;
    if (width == 0 || height == 0) return false;

    // Everything the frame reads that the request changes, saved by value. The
    // handles go into locals and the members are nulled BEFORE
    // createViewportResources runs: it opens by pushing the old colour image onto
    // m_retiredViewports and then destroying the rest of the set, and the live
    // one must survive both untouched.
    VkImage        liveImage   = m_viewportImage;
    VkDeviceMemory liveMemory  = m_viewportMemory;
    VkImageView    liveView    = m_viewportView;
    VkImage        liveDepthImage  = m_viewportDepthImage;
    VkDeviceMemory liveDepthMemory = m_viewportDepthMemory;
    VkImageView    liveDepthView   = m_viewportDepthView;
    VkRenderPass   liveRenderPass  = m_viewportRenderPass;
    VkFramebuffer  liveFramebuffer = m_viewportFramebuffer;
    VkSampler      liveSampler     = m_viewportSampler;
    const uint32_t      liveW          = m_viewportW;
    const uint32_t      liveH          = m_viewportH;
    const uint32_t      liveReqW       = m_viewportReqW;
    const uint32_t      liveReqH       = m_viewportReqH;
    const VkImageLayout liveLayout     = m_viewportLayout;
    const bool          liveResChanged = m_viewportResChanged;
    const EditorCameraOverride liveCam = m_editorCamera;
    // The counters are the profiler's picture of the last real frame; the
    // screenshot's draws would sit there until the next Render() otherwise.
    const uint32_t liveDraws = m_statDraws, liveTris = m_statTris,
                   liveVisible = m_statVisible, liveTotal = m_statTotal;

    m_viewportImage   = VK_NULL_HANDLE;
    m_viewportMemory  = VK_NULL_HANDLE;
    m_viewportView    = VK_NULL_HANDLE;
    m_viewportDepthImage  = VK_NULL_HANDLE;
    m_viewportDepthMemory = VK_NULL_HANDLE;
    m_viewportDepthView   = VK_NULL_HANDLE;
    m_viewportRenderPass  = VK_NULL_HANDLE;
    m_viewportFramebuffer = VK_NULL_HANDLE;
    m_viewportSampler     = VK_NULL_HANDLE;
    m_viewportW    = 0;
    m_viewportH    = 0;
    m_viewportReqW = width;
    m_viewportReqH = height;
    m_viewportLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_editorCamera   = camera;

    bool ok = false;
    try
    {
        // Same order as the top of Render(): drop the caches for assets edited
        // since the last frame (own device idle inside), then start the frame's
        // per-pass flags from zero — m_giRanThisFrame left over from the live
        // frame would decide this frame's aoWanted.
        m_wallTime = static_cast<float>(SDL_GetTicks()) * 0.001f;
        processPendingInvalidations();
        m_ssaoRanThisFrame = false;
        m_giRanThisFrame   = false;
        m_ssrRanThisFrame  = false;
        m_ssrResultView    = VK_NULL_HANDLE;
        m_statDraws = m_statTris = m_statVisible = m_statTotal = 0;
        // This frame's sun before DrawViewportFrame's first extraction, as in Render().
        m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                                m_environment.sunColor, m_environment.sunIntensity,
                                m_environment.moonColor, m_environment.moonIntensity,
                                m_environment.cloudCoverage);
        // The same one-walk scope as Render(): DrawViewportFrame's passes extract on their own.
        RenderExtractor::FrameScope extractOncePerFrame(m_extractor);

        // Nothing may be in flight while the live set's siblings (PostFX, SSAO,
        // decal depth) are torn down and rebuilt, and the one-shot buffer below
        // records on m_currentFrame's rings.
        deviceWaitIdle();
        createViewportResources(width, height);

        if (m_viewportImage && m_viewportFramebuffer && m_viewportW == width && m_viewportH == height)
        {
            VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbai.commandPool        = m_cmdPool;
            cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (vkAllocateCommandBuffers(m_device, &cbai, &cmd) == VK_SUCCESS && cmd)
            {
                VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &cbbi);
                DrawViewportFrame(cmd);
                vkEndCommandBuffer(cmd);

                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1;
                si.pCommandBuffers    = &cmd;
                submitGraphics(si, VK_NULL_HANDLE);
                vkQueueWaitIdle(m_graphicsQueue);
                vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);

                // CaptureViewport waits for the GPU and maps a staging buffer; no
                // world → the cleared target, honestly black, which is what the
                // viewport would show too.
                uint32_t gotW = 0, gotH = 0;
                ok = CaptureViewport(rgba, gotW, gotH) && gotW == width && gotH == height;
            }
        }
    }
    catch (const std::exception& e)
    {
        // vkCheck throws; a request too large to allocate must not take the live
        // viewport down with it — fall through to the give-back below.
        HE_LOG_ERROR(RHI, "VulkanRenderer::RenderSceneImage failed: %s", e.what());
        ok = false;
    }

    // ── Give the live viewport back ─────────────────────────────────────────
    // The screenshot's set goes away whole (its colour image is NOT retired: the
    // editor's descriptor never pointed at it, and the device is idle). The live
    // handles go back into the members BEFORE the intermediates are rebuilt —
    // createPostFXResources builds m_hdrFB/m_fxaaFB/m_uiViewportFB on them.
    deviceWaitIdle();
    destroyViewportResources();
    m_viewportImage       = liveImage;
    m_viewportMemory      = liveMemory;
    m_viewportView        = liveView;
    m_viewportDepthImage  = liveDepthImage;
    m_viewportDepthMemory = liveDepthMemory;
    m_viewportDepthView   = liveDepthView;
    m_viewportRenderPass  = liveRenderPass;
    m_viewportFramebuffer = liveFramebuffer;
    m_viewportSampler     = liveSampler;
    m_viewportW          = liveW;
    m_viewportH          = liveH;
    m_viewportReqW       = liveReqW;
    m_viewportReqH       = liveReqH;
    m_viewportLayout     = liveLayout;
    m_viewportResChanged = liveResChanged;
    m_editorCamera       = liveCam;
    m_statDraws = liveDraws; m_statTris = liveTris;
    m_statVisible = liveVisible; m_statTotal = liveTotal;

    if (liveW && liveH)
    {
        // The tail of createViewportResources, at the live size — unconditionally,
        // see the framebuffer note above.
        createPostFXResources(liveW, liveH);
        createSSAOTargets(liveW, liveH);
        createDecalDepth(m_decalDepthVp, liveW, liveH);
    }
    else
    {
        // No live viewport (the direct-to-swapchain path): there is no size to
        // rebuild against, and createSSAOTargets pointed scene binding 3 at the
        // screenshot's blur target, which is gone.
        pointSceneAoAtWhite();
    }
    // The SSR history is the screenshot camera's now (a same-size request kept
    // the copy, and the frame just captured into it). So is TAA's — rebuilt
    // with the postFx targets above already, stated here so it does not hinge
    // on that rebuild staying unconditional.
    m_ssrColorHistValid = false;
    m_ssrHistValid      = false;
    m_taaHistoryValid   = false;

    if (!ok) rgba.clear();
    return ok;
}

// ─── Any world into a preview target ──────────────────────────────────────────
// The contract is in IRenderer.h, the steps are GL's (OpenGLRenderer.cpp) and
// the D3D backends': sky first, grid lines that write no depth, static and
// skinned meshes through the preview shaders (shaders/world_preview.*), then
// the scene's tonemap pipeline into the RGBA8 image ImGui samples. The
// resources are described at WorldPreviewTargetVk in the header.

// Host-visible ring, grown (never shrunk) to `size`. The device is idle when
// this runs, so the old buffer can go at once.
bool VulkanRenderer::ensurePreviewRing(PreviewRing& ring, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (ring.buf && ring.size >= size) return true;
    if (ring.mapped) vkUnmapMemory(m_device, ring.mem);
    if (ring.buf)    vkDestroyBuffer(m_device, ring.buf, nullptr);
    if (ring.mem)    vkFreeMemory(m_device, ring.mem, nullptr);
    ring = PreviewRing{};

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size        = size;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &ring.buf) != VK_SUCCESS) { ring = PreviewRing{}; return false; }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, ring.buf, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &mai, nullptr, &ring.mem) != VK_SUCCESS
        || vkBindBufferMemory(m_device, ring.buf, ring.mem, 0) != VK_SUCCESS
        || vkMapMemory(m_device, ring.mem, 0, VK_WHOLE_SIZE, 0, &ring.mapped) != VK_SUCCESS)
    {
        if (ring.buf) vkDestroyBuffer(m_device, ring.buf, nullptr);
        if (ring.mem) vkFreeMemory(m_device, ring.mem, nullptr);
        ring = PreviewRing{};
        return false;
    }
    ring.size = size;
    return true;
}

// Set layout + pipeline layout + the two pipelines + pool/sets + sampler +
// the initial rings. Everything the targets do not own; built on first use.
bool VulkanRenderer::ensureWorldPreviewPipeline()
{
    if (m_previewReady)  return true;
    if (m_previewFailed) return false;
    m_previewFailed = true; // until everything below succeeded
    if (!m_postFxSceneRP || !m_postFxFinalRP || !m_tonemapPipe || !m_postFxDSLayout
        || !m_postFxPipeLayout || !m_dummyView || !m_albedoSetLayout || !m_whiteAlbedoSet)
        return false;

    // set 0: b0 PerObject (dynamic, VS+FS), b1 PreviewLight (FS), b2 bones (dynamic, VS).
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 3; slci.pBindings = b;
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_previewSetLayout) != VK_SUCCESS) return false;

    // set 1 = the scene's per-mesh base-colour layout, so the mesh caches'
    // albedo sets (and m_whiteAlbedoSet) bind here unchanged.
    const VkDescriptorSetLayout setLayouts[2] = { m_previewSetLayout, m_albedoSetLayout };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_previewPipeLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kWorldPreviewSlots },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1 + kWorldPreviewSlots; dpci.poolSizeCount = 3; dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_previewPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = m_previewPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_previewSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &m_previewSet) != VK_SUCCESS) return false;
    for (WorldPreviewTargetVk& wp : m_worldPreview)
    {
        dsai.pSetLayouts = &m_postFxDSLayout;
        if (vkAllocateDescriptorSets(m_device, &dsai, &wp.tonemapSet) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_previewSampler) != VK_SUCCESS) return false;

    VkShaderModule vs  = loadShaderModule("world_preview.vert.spv");
    VkShaderModule svs = loadShaderModule("world_preview_skinned.vert.spv");
    VkShaderModule fs  = loadShaderModule("world_preview.frag.spv");
    bool ok = vs && svs && fs;
    if (ok)
    {
        auto makePipe = [&](VkShaderModule vert, const VkVertexInputBindingDescription* binds, uint32_t nBinds,
                            const VkVertexInputAttributeDescription* attrs, uint32_t nAttrs,
                            VkPipeline& out) -> bool
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs;   stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount   = nBinds; vi.pVertexBindingDescriptions   = binds;
            vi.vertexAttributeDescriptionCount = nAttrs; vi.pVertexAttributeDescriptions = attrs;
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1; vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_NONE; // as GL's preview: winding is not guaranteed
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState cba{};
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;
            VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            pci.stageCount = 2; pci.pStages = stages;
            pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
            pci.pViewportState = &vp; pci.pRasterizationState = &rs;
            pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
            pci.pColorBlendState = &cb; pci.pDynamicState = &dyn;
            pci.layout = m_previewPipeLayout;
            pci.renderPass = m_postFxSceneRP; pci.subpass = 0;
            return createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
        };
        const VkVertexInputBindingDescription meshBind = { 0, 32u, VK_VERTEX_INPUT_RATE_VERTEX };
        const VkVertexInputAttributeDescription meshAttrs[3] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
        };
        // The three vertex streams of skinned.vert (see createSkinnedPipeline).
        const VkVertexInputBindingDescription skinBinds[3] = {
            { 0, 32u,                   VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, 4u * sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX },
            { 2, 4u * sizeof(float),    VK_VERTEX_INPUT_RATE_VERTEX },
        };
        const VkVertexInputAttributeDescription skinAttrs[5] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    12 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,       24 },
            { 3, 1, VK_FORMAT_R32G32B32A32_UINT,   0  },
            { 4, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0  },
        };
        ok = makePipe(vs, &meshBind, 1, meshAttrs, 3, m_previewMeshPipe)
          && makePipe(svs, skinBinds, 3, skinAttrs, 5, m_previewSkinnedPipe);
        if (!ok) HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: world preview pipeline creation failed");
    }
    else
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: world preview shaders missing (world_preview*.spv)");
    if (vs)  vkDestroyShaderModule(m_device, vs,  nullptr);
    if (svs) vkDestroyShaderModule(m_device, svs, nullptr);
    if (fs)  vkDestroyShaderModule(m_device, fs,  nullptr);
    if (!ok) return false;

    if (!ensurePreviewRing(m_previewLightBuf, 4 * sizeof(glm::vec4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
        return false;
    m_previewReady  = true;
    m_previewFailed = false;
    return true;
}

void VulkanRenderer::destroyWorldPreviewTarget(WorldPreviewTargetVk& wp)
{
    if (wp.sceneFB)    vkDestroyFramebuffer(m_device, wp.sceneFB, nullptr);
    if (wp.ldrFB)      vkDestroyFramebuffer(m_device, wp.ldrFB, nullptr);
    for (VkImageView v : { wp.hdrView, wp.ldrView, wp.depthView }) if (v) vkDestroyImageView(m_device, v, nullptr);
    for (VkImage i : { wp.hdrImage, wp.ldrImage, wp.depthImage })   if (i) vkDestroyImage(m_device, i, nullptr);
    for (VkDeviceMemory m : { wp.hdrMem, wp.ldrMem, wp.depthMem })  if (m) vkFreeMemory(m_device, m, nullptr);
    // The tonemap set (pool-owned) and the ImGui handle (ImGui-owned) stay:
    // both are rewritten for the next target instead of reallocated.
    const VkDescriptorSet tonemapSet  = wp.tonemapSet;
    void* const           imguiHandle = wp.imguiHandle;
    wp = WorldPreviewTargetVk{};
    wp.tonemapSet  = tonemapSet;
    wp.imguiHandle = imguiHandle;
}

// (Re)creates a slot's images at w×h. The device is idle (the caller waited).
bool VulkanRenderer::ensureWorldPreviewTarget(WorldPreviewTargetVk& wp, uint32_t w, uint32_t h)
{
    if (wp.ldrImage && wp.w == w && wp.h == h) return true;
    destroyWorldPreviewTarget(wp);

    auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &ici, nullptr, &img) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device, img, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        if (vkBindImageMemory(m_device, img, mem, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { aspect, 0, 1, 0, 1 };
        return vkCreateImageView(m_device, &vci, nullptr, &view) == VK_SUCCESS;
    };
    bool ok = makeImage(VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, wp.hdrImage, wp.hdrMem, wp.hdrView)
           && makeImage(VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, wp.ldrImage, wp.ldrMem, wp.ldrView)
           && makeImage(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT, wp.depthImage, wp.depthMem, wp.depthView);
    if (ok)
    {
        const VkImageView sceneViews[2] = { wp.hdrView, wp.depthView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_postFxSceneRP; fci.attachmentCount = 2; fci.pAttachments = sceneViews;
        fci.width = w; fci.height = h; fci.layers = 1;
        ok = vkCreateFramebuffer(m_device, &fci, nullptr, &wp.sceneFB) == VK_SUCCESS;
        fci.renderPass = m_postFxFinalRP; fci.attachmentCount = 1; fci.pAttachments = &wp.ldrView;
        ok = ok && vkCreateFramebuffer(m_device, &fci, nullptr, &wp.ldrFB) == VK_SUCCESS;
    }
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "VulkanRenderer: world preview target %ux%u creation failed", w, h);
        destroyWorldPreviewTarget(wp);
        return false;
    }

    // Tonemap inputs: this slot's HDR image, the dummy for the bloom slot
    // (strength 0). The layout's samplers are immutable.
    VkDescriptorImageInfo tii[2]{};
    tii[0].imageView = wp.hdrView;   tii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tii[1].imageView = m_dummyView;  tii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet tw[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        tw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tw[i].dstSet = wp.tonemapSet; tw[i].dstBinding = i; tw[i].descriptorCount = 1;
        tw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tw[i].pImageInfo = &tii[i];
    }
    vkUpdateDescriptorSets(m_device, 2, tw, 0, nullptr);

    // ImGui: the first target registers a set, a resize rewrites that set
    // (binding 0, what ImGui_ImplVulkan_AddTexture writes). No registrar (a run
    // without the editor's ImGui) → drawn anyway, just nothing to hand back.
    if (!wp.imguiHandle && m_imguiTexRegistrar)
        wp.imguiHandle = m_imguiTexRegistrar(reinterpret_cast<void*>(wp.ldrView),
                                             reinterpret_cast<void*>(m_previewSampler));
    else if (wp.imguiHandle)
    {
        VkDescriptorImageInfo ii{ m_previewSampler, wp.ldrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet iw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        iw.dstSet = reinterpret_cast<VkDescriptorSet>(wp.imguiHandle);
        iw.dstBinding = 0; iw.descriptorCount = 1;
        iw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iw.pImageInfo = &ii;
        vkUpdateDescriptorSets(m_device, 1, &iw, 0, nullptr);
    }
    wp.w = w; wp.h = h;
    return true;
}

void VulkanRenderer::destroyWorldPreview()
{
    if (!m_device) return;
    for (WorldPreviewTargetVk& wp : m_worldPreview)
    {
        destroyWorldPreviewTarget(wp);
        wp = WorldPreviewTargetVk{};   // the sets go with the pool below; ImGui frees its own
    }
    for (PreviewRing* r : { &m_previewObjRing, &m_previewLightBuf, &m_previewBoneRing })
    {
        if (r->mapped) vkUnmapMemory(m_device, r->mem);
        if (r->buf)    vkDestroyBuffer(m_device, r->buf, nullptr);
        if (r->mem)    vkFreeMemory(m_device, r->mem, nullptr);
        *r = PreviewRing{};
    }
    if (m_previewMeshPipe)    vkDestroyPipeline(m_device, m_previewMeshPipe, nullptr);
    if (m_previewSkinnedPipe) vkDestroyPipeline(m_device, m_previewSkinnedPipe, nullptr);
    if (m_previewPipeLayout)  vkDestroyPipelineLayout(m_device, m_previewPipeLayout, nullptr);
    if (m_previewPool)        vkDestroyDescriptorPool(m_device, m_previewPool, nullptr);
    if (m_previewSetLayout)   vkDestroyDescriptorSetLayout(m_device, m_previewSetLayout, nullptr);
    if (m_previewSampler)     vkDestroySampler(m_device, m_previewSampler, nullptr);
    m_previewMeshPipe = m_previewSkinnedPipe = VK_NULL_HANDLE;
    m_previewPipeLayout = VK_NULL_HANDLE;
    m_previewPool       = VK_NULL_HANDLE;
    m_previewSet        = VK_NULL_HANDLE;
    m_previewSetLayout  = VK_NULL_HANDLE;
    m_previewSampler    = VK_NULL_HANDLE;
    m_previewReady = m_previewFailed = false;
}

void* VulkanRenderer::RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
                                         uint32_t width, uint32_t height,
                                         const EditorCameraOverride& camera,
                                         const glm::vec3& origin,
                                         const WorldPreviewEnv& env,
                                         glm::mat4* outViewProj,
                                         uint32_t slot)
{
    if (!m_device || !m_graphicsQueue || !m_cmdPool) return nullptr;
    const uint32_t W = std::clamp(width,  32u, 4096u);
    const uint32_t H = std::clamp(height, 32u, 4096u);
    if (!m_contentManager) m_contentManager = &cm;
    WorldPreviewTargetVk& wp = m_worldPreview[std::min(slot, kWorldPreviewSlots - 1)];
    constexpr VkDeviceSize kObjStride  = 256;                     // ≥ minUniformBufferOffsetAlignment
    constexpr VkDeviceSize kBoneStride = 128 * sizeof(glm::mat4); // 8192, a multiple of 256
    try
    {
        // Nothing of a frame or an earlier preview may still read what is
        // touched below: the shared sky/debug-line buffers, the rings, the
        // descriptor sets, a target about to be replaced.
        deviceWaitIdle();
        if (!ensureWorldPreviewPipeline()) return nullptr;
        if (!ensureWorldPreviewTarget(wp, W, H)) return nullptr;

        // Camera, snapshot, sky and light — shared with D3D11/D3D12. viewProj
        // is GL clip (what the caller rebuilds); the draws get kVulkanClipFix
        // (depth 0..1 and the y flip) exactly once.
        HE::WorldPreviewFrame frame;
        HE::buildWorldPreviewFrame(m_contentManager, world, camera, env,
                                   static_cast<float>(W) / static_cast<float>(H), frame);
        const RenderWorld& snapshot = frame.snapshot;
        const glm::mat4    drawVP   = HE::kVulkanClipFix * frame.viewProj;
        if (outViewProj) *outViewProj = frame.viewProj;

        // Resolve (and upload — resolveMesh submits its own one-shot copies)
        // every mesh BEFORE this call's command buffer is opened.
        struct MeshDraw    { const RenderObject* obj; const GpuMesh* mesh; };
        struct SkinnedDraw { const SkinnedRenderObject* obj; const GpuSkeletalMesh* mesh; };
        std::vector<MeshDraw>    meshes;
        std::vector<SkinnedDraw> skinned;
        for (const RenderObject& obj : snapshot.objects)
            if (const GpuMesh* m = resolveMesh(obj.meshAssetId); m && m->vbuf && m->ibuf && m->indexCount)
                meshes.push_back({ &obj, m });
        for (const SkinnedRenderObject& obj : snapshot.skinnedObjects)
            if (const GpuSkeletalMesh* m = resolveSkeletalMesh(obj.meshAssetId); m && m->vb && m->ib && m->indexCount > 0)
                skinned.push_back({ &obj, m });

        // Rings grown to fit; a regrown ring means the set's buffers changed.
        const VkBuffer oldObj = m_previewObjRing.buf, oldBone = m_previewBoneRing.buf;
        if (!ensurePreviewRing(m_previewObjRing, std::max<size_t>(1, meshes.size() + skinned.size()) * kObjStride,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
            || !ensurePreviewRing(m_previewBoneRing, std::max<size_t>(1, skinned.size()) * kBoneStride,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return nullptr;
        if (m_previewObjRing.buf != oldObj || m_previewBoneRing.buf != oldBone)
        {
            const VkDescriptorBufferInfo bi[3] = {
                { m_previewObjRing.buf,  0, 2 * sizeof(glm::mat4) + 2 * sizeof(glm::vec4) },
                { m_previewLightBuf.buf, 0, 4 * sizeof(glm::vec4) },
                { m_previewBoneRing.buf, 0, kBoneStride },
            };
            VkWriteDescriptorSet w[3]{};
            const VkDescriptorType types[3] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC };
            for (uint32_t i = 0; i < 3; ++i)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = m_previewSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
                w[i].descriptorType = types[i]; w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
        }

        // Fill the light and every draw's blocks up front (host-coherent).
        {
            const glm::vec4 light[4] = { glm::vec4(frame.camPos, 1.0f), frame.sun,
                                         glm::vec4(frame.sunColor, 1.0f), glm::vec4(frame.ambient, 1.0f) };
            std::memcpy(m_previewLightBuf.mapped, light, sizeof(light));
        }
        struct PerObject { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 pbr; };
        auto writeObject = [&](size_t i, const glm::mat4& model, const glm::vec3& color, bool hasTex,
                               float metallic, float roughness, float skinnedLook)
        {
            PerObject o{ drawVP * model, model, glm::vec4(color, hasTex ? 1.0f : 0.0f),
                         glm::vec4(metallic, roughness, 1.0f, skinnedLook) };
            std::memcpy(static_cast<uint8_t*>(m_previewObjRing.mapped) + i * kObjStride, &o, sizeof(o));
        };
        for (size_t i = 0; i < meshes.size(); ++i)
            writeObject(i, meshes[i].obj->transform, meshes[i].obj->baseColor,
                        meshes[i].mesh->albedoSet != VK_NULL_HANDLE,
                        meshes[i].obj->metallic, meshes[i].obj->roughness, 0.0f);
        for (size_t i = 0; i < skinned.size(); ++i)
        {
            // Pbr zero and the skinned lighting (uPBR.w), as GL's skinned preview.
            writeObject(meshes.size() + i, skinned[i].obj->transform, skinned[i].obj->baseColor,
                        skinned[i].mesh->albedoSet != VK_NULL_HANDLE, 0.0f, 0.0f, 1.0f);
            glm::mat4* bones = reinterpret_cast<glm::mat4*>(
                static_cast<uint8_t*>(m_previewBoneRing.mapped) + i * kBoneStride);
            const std::vector<glm::mat4>& pose = skinned[i].obj->boneMatrices;
            for (size_t b = 0; b < 128; ++b)
                bones[b] = b < pose.size() ? pose[b] : glm::mat4(1.0f);  // identity past the pose, as GL
        }

        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device, &cbai, &cmd) != VK_SUCCESS || !cmd) return nullptr;
        VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbbi);

        const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H), 0.0f, 1.0f };
        const VkRect2D   sc{ { 0, 0 }, { W, H } };

        // ── Scene half into the HDR image (m_postFxSceneRP clears both).
        {
            VkClearValue clears[2]{};
            // Studio background, LINEAR — the tonemap lifts it (kPreviewBackground).
            clears[0].color = { { HE::kPreviewBackground[0], HE::kPreviewBackground[1],
                                  HE::kPreviewBackground[2], 1.0f } };
            clears[1].depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass = m_postFxSceneRP; rpbi.framebuffer = wp.sceneFB;
            rpbi.renderArea = sc; rpbi.clearValueCount = 2; rpbi.pClearValues = clears;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        }
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Sky FIRST: the grid writes no depth, so a sky drawn after it would
        // paint straight over it. Clock stopped (HE_SKY_TIME for a reproducible
        // headless shot), as on the other backends.
        if (env.sky)
        {
            float skyClock = 0.0f;
            if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov)
                skyClock = static_cast<float>(std::atof(ov));
            drawSkyFrom(cmd, /*hdr=*/true, drawVP, snapshot.sunDirection, frame.camPos, skyClock, frame.sky);
        }
        // Grid + origin marker through the debug-line pipeline (no depth write).
        if (env.grid)
        {
            std::vector<float> verts;
            HE::buildPreviewGrid(HE::worldPreviewGridExtent(frame.camPos, origin), 1.0f, verts, origin);
            std::vector<DebugLine> lines;
            lines.reserve(verts.size() / 12);
            for (size_t i = 0; i + 11 < verts.size(); i += 12)
                lines.push_back({ { verts[i],     verts[i + 1], verts[i + 2] },
                                  { verts[i + 6], verts[i + 7], verts[i + 8] },
                                  { verts[i + 3], verts[i + 4], verts[i + 5] } });
            drawLineList(cmd, drawVP, lines, /*hdr=*/true);
        }

        auto bindDraw = [&](size_t objIndex, size_t boneIndex, VkDescriptorSet albedoSet)
        {
            const uint32_t offsets[2] = { static_cast<uint32_t>(objIndex * kObjStride),
                                          static_cast<uint32_t>(boneIndex * kBoneStride) };
            const VkDescriptorSet sets[2] = { m_previewSet, albedoSet ? albedoSet : m_whiteAlbedoSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_previewPipeLayout,
                                    0, 2, sets, 2, offsets);
        };
        if (!meshes.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_previewMeshPipe);
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                const GpuMesh& m = *meshes[i].mesh;
                bindDraw(i, 0, m.albedoSet);
                const VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &off);
                vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
            }
        }
        if (!skinned.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_previewSkinnedPipe);
            for (size_t i = 0; i < skinned.size(); ++i)
            {
                const GpuSkeletalMesh& m = *skinned[i].mesh;
                bindDraw(meshes.size() + i, i, m.albedoSet);
                const VkBuffer     vbs[3]  = { m.vb, m.boneIdVb, m.boneWgtVb };
                const VkDeviceSize offs[3] = { 0, 0, 0 };
                vkCmdBindVertexBuffers(cmd, 0, 3, vbs, offs);
                vkCmdBindIndexBuffer(cmd, m.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(m.indexCount), 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(cmd);

        // HDR: attachment → sampled, for the tonemap.
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = wp.hdrImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // ── Tonemap resolve into the RGBA8 image, through the scene's own
        // tonemap pipeline (exposure 1, bloom strength 0). m_postFxFinalRP
        // leaves it in SHADER_READ_ONLY, which is what ImGui samples.
        {
            VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass = m_postFxFinalRP; rpbi.framebuffer = wp.ldrFB; rpbi.renderArea = sc;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postFxPipeLayout,
                                    0, 1, &wp.tonemapSet, 0, nullptr);
            const float pc[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
            vkCmdPushConstants(cmd, m_postFxPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }

        // Headless witness (HE_WORLD_PREVIEW_DUMP=<file.ppm>), as on the other
        // backends: the LDR result, copied out before ImGui gets it back.
        const char* dumpPath = std::getenv("HE_WORLD_PREVIEW_DUMP");
        PreviewRing readback;
        if (dumpPath && *dumpPath
            && ensurePreviewRing(readback, static_cast<VkDeviceSize>(W) * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = wp.ldrImage; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent      = { W, H, 1 };
            vkCmdCopyImageToBuffer(cmd, wp.ldrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.buf, 1, &region);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        submitGraphics(si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &cmd);

        if (readback.mapped)
            HE::writeWorldPreviewDump(dumpPath, static_cast<const uint8_t*>(readback.mapped),
                                      static_cast<int>(W), static_cast<int>(H), static_cast<size_t>(W) * 4);
        if (readback.mapped) vkUnmapMemory(m_device, readback.mem);
        if (readback.buf)    vkDestroyBuffer(m_device, readback.buf, nullptr);
        if (readback.mem)    vkFreeMemory(m_device, readback.mem, nullptr);
    }
    catch (const std::exception& e)
    {
        HE_LOG_ERROR(RHI, "VulkanRenderer::RenderWorldPreview failed: %s", e.what());
        return nullptr;
    }
    return wp.imguiHandle;
}

void* VulkanRenderer::GetViewportVkImageView() const { return static_cast<void*>(m_viewportView); }
void* VulkanRenderer::GetViewportVkSampler()   const { return static_cast<void*>(m_viewportSampler); }
bool  VulkanRenderer::HasViewportResourceChanged() const { return m_viewportResChanged; }
void  VulkanRenderer::ClearViewportResourceChanged()     { m_viewportResChanged = false; }

uint64_t VulkanRenderer::GetCompletedFrameSerial()
{
    // A fence signalled by vkQueueSubmit covers every command submitted to that
    // queue earlier in submission order (Vulkan spec, "Fences"), and all frame
    // submits go to m_graphicsQueue — so the highest serial among the signalled
    // frame fences says every frame up to it is done. A fence reset for a
    // submit that has not happened yet just does not count; that only delays.
    if (!m_device) return m_completedSerial;
    auto consider = [this](VkFence fence, uint64_t serial)
    {
        if (fence && serial > m_completedSerial &&
            vkGetFenceStatus(m_device, fence) == VK_SUCCESS)
            m_completedSerial = serial;
    };
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        consider(m_frameFence[i], m_frameSerial[i]);
    for (auto& [sdlWin, wd] : m_extraWindows)
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
            consider(wd.frameFence[i], wd.frameSerial[i]);
    return m_completedSerial;
}

// One host-visible, host-coherent, persistently mapped vertex buffer of `instances`
// {mvp, model} slots: a chunk of the instance arena (binding 1 of every instanced pipeline).
bool VulkanRenderer::createInstanceBuf(uint32_t instances, InstanceBuf& out)
{
    out = {};
    VkBufferCreateInfo ibci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ibci.size  = static_cast<VkDeviceSize>(instances) * k_instStride;
    ibci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(m_device, &ibci, nullptr, &out.buf) != VK_SUCCESS) { out = {}; return false; }
    VkMemoryRequirements ireq{}; vkGetBufferMemoryRequirements(m_device, out.buf, &ireq);
    VkMemoryAllocateInfo imai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imai.allocationSize  = ireq.size;
    imai.memoryTypeIndex = findMemoryType(ireq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &imai, nullptr, &out.mem) != VK_SUCCESS
        || vkBindBufferMemory(m_device, out.buf, out.mem, 0) != VK_SUCCESS
        || vkMapMemory(m_device, out.mem, 0, ibci.size, 0, &out.mapped) != VK_SUCCESS)
    {
        destroyInstanceBuf(out);
        return false;
    }
    return true;
}

void VulkanRenderer::destroyInstanceBuf(InstanceBuf& b)
{
    if (b.mapped) vkUnmapMemory(m_device, b.mem);
    if (b.buf)    vkDestroyBuffer(m_device, b.buf, nullptr);
    if (b.mem)    vkFreeMemory   (m_device, b.mem, nullptr);
    b = {};
}

// Where a frame's instance slots start (Render() and DrawViewportFrame()): the main
// buffer of m_currentFrame, which grows to what the largest frame before asked
// (m_instArena still holds the last one). The replaced buffer may still be read by
// its last frame — it ages out in m_instRetired like every overflow chunk.
void VulkanRenderer::beginInstanceFrame()
{
    if (m_instArena.overflowed())
    {
        m_instWant = std::max(m_instWant, m_instArena.nextMainCapacity());
        if (m_instArena.refused() && !m_instArenaWarned)
        {
            m_instArenaWarned = true;
            HE_LOG_WARN(RHI, "VulkanRenderer: instance arena full — a frame asked %u instances, %u drew "
                        "per object (budget %u per frame, HE_INST_ARENA_MIB)",
                        m_instArena.asked(), m_instArena.refused(), m_instArena.budget());
        }
    }
    InstanceBuf& main = m_instanceBuf[m_currentFrame];
    uint32_t&    cap  = m_instMainCap[m_currentFrame];
    if (m_instWant > cap && main.buf)
    {
        InstanceBuf grown;
        if (createInstanceBuf(m_instWant, grown))
        {
            HE_LOG_INFO(RHI, "VulkanRenderer: instance buffer of frame slot %u grows from %u to %u instances",
                        m_currentFrame, cap, m_instWant);
            m_instRetired.push_back({ main, static_cast<int>(k_maxFramesInFlight) + 1 });
            main = grown;
            cap  = m_instWant;
        }
    }
    m_instCur = main;
    m_instArena.beginFrame(main.mapped ? cap : 0);
}

// `count` consecutive {mvp, model} slots of this frame: the buffer to bind at binding 1,
// the byte offset, and where the CPU writes them. false = no arena, or the frame would
// pass the arena's budget: the caller draws per object.
bool VulkanRenderer::allocInstances(uint32_t count, VkBuffer& buf, VkDeviceSize& offset, uint8_t*& cpu)
{
    if (!m_instCur.mapped || count == 0) return false;
    const uint32_t chunkCap = m_instArena.chunkFor(count);
    if (chunkCap == HE::InstanceArena::kRefused) { m_instArena.refuse(count); return false; }
    if (chunkCap)
    {
        InstanceBuf chunk;
        if (!createInstanceBuf(chunkCap, chunk)) { m_instArena.refuse(count); return false; }
        // In use from now until this frame retired: ages out like a replaced main buffer.
        m_instRetired.push_back({ chunk, static_cast<int>(k_maxFramesInFlight) + 1 });
        m_instCur = chunk;
        m_instArena.openChunk(chunkCap);
    }
    const uint32_t first = m_instArena.take(count);
    buf    = m_instCur.buf;
    offset = static_cast<VkDeviceSize>(first) * k_instStride;
    cpu    = static_cast<uint8_t*>(m_instCur.mapped) + offset;
    return true;
}

// The Vulkan twin of D3D11/D3D12's drawDepthInstanced: every caller writes the
// products its per-object loop pushes as constants, so the instanced frame is
// the loop's frame. The buffer is host-coherent and read at submit, which is
// why the slots come from the frame's arena (allocInstances) and never get
// reused inside one frame.
template <class Fill>
bool VulkanRenderer::drawDepthInstanced(VkCommandBuffer cmd, VkPipeline instPipe, VkPipeline restorePipe,
                                        uint32_t indexCount, const InstanceRun& run, Fill&& fill)
{
    static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be two mat4");
    const uint32_t count = run.size();
    VkBuffer ibuf = VK_NULL_HANDLE; VkDeviceSize off = 0; uint8_t* dst = nullptr;
    if (!instPipe || count < 2 || !RenderSorter::depthInstancingEnabled()
        || !allocInstances(count, ibuf, off, dst))
        return false;
    RenderSorter::fillInstancePairs(dst, run, fill);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instPipe);
    vkCmdBindVertexBuffers(cmd, 1, 1, &ibuf, &off);
    vkCmdDrawIndexed(cmd, indexCount, count, 0, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, restorePipe);
    return true;
}

void VulkanRenderer::extractFrame(float aspect)
{
    if (!m_world) return;
    // The cascades are fit against the camera frustum, so this extract needs
    // the real aspect (the old whole-scene single map never cared) and the
    // project shadow settings — the extractor hands back the size that is
    // actually ALLOCATED, the texel snap must match the texture.
    m_extractor.setShadowSettings(m_shadowSettings.distance, m_shadowSettings.cascadeCount,
                                  m_shadowSettings.splitLambda, static_cast<int>(m_shadowSize));
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
}

void VulkanRenderer::refineObjectBounds()
{
    // What the loop writes depends only on an object's transform and its mesh's bound, and the
    // walk stays in m_renderWorld for the whole frame (RenderExtractor::beginFrame): a second loop
    // over the same walk writes the same values again, and a Vulkan frame ran it up to five times
    // (decal depth, cascades, scene, GI, SSAO). So: once per walk. The frame refines right after
    // the walk, every pass calls this again as its guarantee and finds it done; a new walk (the
    // count moves) or another object array starts over. A mesh that could not be resolved when
    // the loop ran keeps the bound the walk gave it for
    // the rest of the walk, invalid at worst, and an object with an invalid bound is never culled:
    // the conservative side, the draw loops resolve the mesh on their own.
    m_frameCull.refine(m_renderWorld, m_extractor.fullExtractCount(), [this](const RenderObject& obj) -> const HE::AABB*
    {
        const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
        return mesh ? &mesh->localBounds : nullptr;
    });
}

void VulkanRenderer::cullCameraObjects()
{
    m_frameCull.cullCamera(m_culler, m_sorter, m_renderWorld, m_extractor.fullExtractCount(), m_frameStamp,
                           /*variant=*/0, m_visible, m_sortedIndices);
}

void VulkanRenderer::EncodeShadowMap(VkCommandBuffer cmd, float aspect)
{
    GpuPassScope _passTimer(this, "Shadow");
    m_shadowRenderedThisFrame = false;
    m_localShadowLayerCount   = 0;
    if (m_shadowPipeline == VK_NULL_HANDLE || m_shadowImage == VK_NULL_HANDLE) return;
    // A fresh array (first frame, or right after a resolution swap) sits in
    // UNDEFINED until the depth pass ran once — but the scene set already
    // samples it. On a frame that does not render the cascades (no world, no
    // sun, empty scene) put every layer into the sampled layout explicitly so
    // no frame ever samples an undefined image (the single map used to).
    auto ensureSampledLayout = [&]()
    {
        if (m_shadowLayoutValid) return;
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_shadowImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, static_cast<uint32_t>(kCsmCascades) };
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        m_shadowLayoutValid = true;
    };
    // The local (point/spot) atlas: same rule, but ONCE for all 16 layers and
    // never per frame — only the layers the extractor assigned are rendered
    // (a render pass takes its attachment from UNDEFINED regardless of the
    // layer's actual layout, and every other layer already sits in the sampled
    // layout from this barrier or from its last pass). Clearing 16 × 1024²
    // every frame like the cascade tail would be pure waste.
    auto ensureLocalSampledLayout = [&]()
    {
        if (m_localShadowLayoutValid || m_localShadowImage == VK_NULL_HANDLE) return;
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_localShadowImage;
        b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, static_cast<uint32_t>(kLocalShadowLayers) };
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        m_localShadowLayoutValid = true;
    };
    ensureLocalSampledLayout();
    if (!m_world) { ensureSampledLayout(); return; }

    // The cascades come from the frame's walk, which the caller took just before this pass
    // (extractFrame); this call is the key check that m_renderWorld holds that walk at THIS
    // pass's aspect — no walk of its own. The day-night state is pushed once at the top of
    // Render()/RenderSceneImage() and, like the shadow settings, part of the extractor's
    // frame key: a sun that moved or settings pushed in between would be a new walk, never
    // last frame's cascades.
    extractFrame(aspect);
    // CSM needs a directional light; the local (point/spot) atlas is
    // independent of it — night scenes with only shadow-casting point lights
    // still render their atlas (mirrors Metal/GL/D3D).
    const bool wantCsm   = m_renderWorld.shadow.enabled;
    const bool wantLocal = m_renderWorld.shadow.localLayerCount > 0 && m_localShadowImage != VK_NULL_HANDLE;
    if ((!wantCsm && !wantLocal) || m_renderWorld.objects.empty()) { ensureSampledLayout(); return; }
    refineObjectBounds();

    // ── Cascade frame constants (mirrors GL's shadowFrame / D3D11) ─────────
    // The extractor's cascade matrices are GL clip (y up, z∈[-1,1]);
    // kVulkanClipFix flips y and remaps depth to [0,1] — the SAME fix the old
    // single map got, and scene.frag samples with uv = p.xy*0.5+0.5 under the
    // same matrix, so no separate UV flip exists anywhere. The planar camera
    // forward (−Z of camera-to-world) MUST match the planar splits the
    // cascades were fit with; unused tail cascades get identity (never
    // sampled — the shader clamps the pick to the count) and a far split of
    // 1e9 so the pick never lands on them.
    const ShadowData& csm = m_renderWorld.shadow;
    const int nCascades  = wantCsm ? std::clamp(csm.cascadeCount, 0, kCsmCascades) : 0;
    for (int c = 0; c < kCsmCascades; ++c)
        m_cascadeClip[c] = c < nCascades ? HE::kVulkanClipFix * csm.cascadeViewProj[c] : glm::mat4(1.0f);
    m_cascadeSplits = glm::vec4(nCascades > 0 ? csm.cascadeSplit[0] : 1e9f,
                                nCascades > 1 ? csm.cascadeSplit[1] : 1e9f,
                                nCascades > 2 ? csm.cascadeSplit[2] : 1e9f,
                                static_cast<float>(nCascades));
    m_cascadeCamFwd = -glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
    // GL's glPolygonOffset(2, 4) in Vulkan terms: slope factor 2, constant 4.
    const float kShadowBiasConstant = 4.0f, kShadowBiasSlope = 2.0f;

    // Depth-only draws of every shadow caster into the pass that is currently
    // open, shared by the cascades and the local (point/spot) layers. Cull +
    // sort against the view's light frustum into scratch buffers; the sort
    // groups draws by mesh so the resolve stays memoised. batchDepthCasters
    // keeps castsShadow objects only and drops `skipEntity`'s geometry: the
    // entity the local light itself sits on (a light authored onto a mesh
    // entity would otherwise render that mesh at z≈0 into its own map and
    // shadow itself out); kNoOwnerEntity (every cascade) skips nothing. A run
    // of more than one caster of the same mesh is ONE instanced draw
    // (m_shadowInstancedPipeline, clip * model per caster in the instance
    // buffer — the push-constant path's own product); a run of one, or one the
    // buffer cannot take, stays on the per-caster push-constant draw. Mirrors
    // GL's renderDepthLayer / Metal's encodeDepthLayer / D3D11 + D3D12.
    // Every view (the cascades, then the local layers) is culled, sorted by mesh and cut into
    // runs before the first is drawn, side by side on the workers (FrameCull::prepareShadowViews);
    // the Vulkan calls stay on this thread, layer after layer.
    const int nLocalViews = wantLocal ? std::clamp(csm.localLayerCount, 0, kLocalShadowLayers) : 0;
    if (m_shadowViews.size() < static_cast<size_t>(nCascades + nLocalViews))
        m_shadowViews.resize(static_cast<size_t>(nCascades + nLocalViews));
    for (int v = 0; v < nCascades + nLocalViews; ++v)
    {
        ShadowViewCpu& sv = m_shadowViews[static_cast<size_t>(v)];
        sv.viewProj = v < nCascades ? csm.cascadeViewProj[v] : csm.localViewProj[v - nCascades];
        sv.skip     = v < nCascades ? kNoOwnerEntity : csm.localOwnerEntity[v - nCascades];
    }
    m_frameCull.prepareShadowViews(m_culler, m_renderWorld, m_extractor.fullExtractCount(),
                                   m_shadowViews.data(), static_cast<size_t>(nCascades + nLocalViews),
                                   "VkShadowViews");
    auto drawDepthLayer = [&](uint32_t size, const ShadowViewCpu& view, const glm::mat4& clip)
    {
        VkViewport vp{ 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
        VkRect2D   sc{ { 0, 0 }, { size, size } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdSetDepthBias(cmd, kShadowBiasConstant, 0.0f, kShadowBiasSlope);
        for (const RenderSorter::DepthBatch& b : view.batches.batches)
        {
            const GpuMesh* mesh = resolveMesh(b.meshAssetId);
            const GpuMesh& m    = mesh ? *mesh : m_cube;
            if (!m.indexCount) continue;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);
            const InstanceRun run = view.batches.run(b);
            if (drawDepthInstanced(cmd, m_shadowInstancedPipeline, m_shadowPipeline, m.indexCount, run,
                    [&](const glm::mat4& model, glm::mat4* pair) {
                        pair[0] = clip * model;
                        pair[1] = model;
                    }))
            {
                static bool loggedOnce = false; // the runtime witness on Windows
                if (!loggedOnce)
                {
                    loggedOnce = true;
                    HE_LOG_INFO(RHI, "VulkanRenderer: shadow pass instanced (first run: %u casters)", b.count);
                }
                continue;
            }
            run.forEach([&](const glm::mat4& model)
            {
                PushConstants pc{ clip * model, model };
                vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
            });
        }
    };

    // One depth render per cascade into its own array layer. Each cascade
    // re-culls the casters against ITS light frustum (not the camera): an
    // off-screen object still casts into the visible scene while it sits
    // inside the cascade coverage. Mirrors GL's renderDepthLayer / Metal's
    // encodeDepthLayer / the D3D11+D3D12 passes.
    //
    // EVERY layer runs through the pass, also the ones above the project's
    // cascade count (cleared, no draws): the pass takes the attachment from
    // UNDEFINED to SHADER_READ_ONLY, and the scene set samples the WHOLE array
    // in that layout — a layer the pass never touched would be sampled in an
    // undefined layout (validation error, GPU fault on some drivers). A clear
    // of an unused 2048² layer is cheap. Without a directional light the
    // cascade array is left alone (its layout is ensured above).
    if (wantCsm)
    {
        for (int c = 0; c < kCsmCascades; ++c)
        {
            VkClearValue clear{};
            clear.depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass        = m_shadowPass;
            rpbi.framebuffer       = m_shadowFB[c];
            rpbi.renderArea.extent = { m_shadowSize, m_shadowSize };
            rpbi.clearValueCount   = 1;
            rpbi.pClearValues      = &clear;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            if (c < nCascades)
                drawDepthLayer(m_shadowSize, m_shadowViews[static_cast<size_t>(c)], m_cascadeClip[c]);
            vkCmdEndRenderPass(cmd);
        }
        m_shadowLayoutValid       = true; // every layer went through the pass → SHADER_READ_ONLY
        m_shadowRenderedThisFrame = true;
    }
    else
        ensureSampledLayout();

    // ── Local (point/spot) atlas layers ─────────────────────────────────────
    // Only the layers the extractor assigned (spot = 1, point = 6 cube faces),
    // each into its own layer of the atlas with its own cull and the owning
    // entity skipped. The clip transforms are kept for DrawScene's UBO fill
    // (same reason as m_cascadeClip).
    if (wantLocal)
    {
        const int nLocal = nLocalViews;
        for (int v = 0; v < nLocal; ++v)
        {
            m_localShadowClip[v] = HE::kVulkanClipFix * csm.localViewProj[v];
            VkClearValue clear{};
            clear.depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass        = m_shadowPass;
            rpbi.framebuffer       = m_localShadowFB[v];
            rpbi.renderArea.extent = { kLocalShadowSize, kLocalShadowSize };
            rpbi.clearValueCount   = 1;
            rpbi.pClearValues      = &clear;
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            drawDepthLayer(kLocalShadowSize, m_shadowViews[static_cast<size_t>(nCascades + v)], m_localShadowClip[v]);
            vkCmdEndRenderPass(cmd);
        }
        for (int v = nLocal; v < kLocalShadowLayers; ++v) m_localShadowClip[v] = glm::mat4(1.0f);
        m_localShadowLayerCount = nLocal;
    }
}

// ─── Asset upload path (REN-802/803) ─────────────────────────────────────────
// See the header for the scheme. Single-threaded like the rest of the renderer:
// every caller is the render thread.
namespace
{
uint64_t uploadClockNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool envOn(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}
} // namespace

bool VulkanRenderer::allocPooled(const VkMemoryRequirements& req, VkMemoryPropertyFlags props,
                                 PoolAlloc& out)
{
    out = PoolAlloc{};
    const uint32_t type = findMemoryType(req.memoryTypeBits, props);
    // Big buffers get their own allocation: a quarter block is where a free list
    // starts losing more to fragmentation than it saves in allocations.
    if (req.size > k_memBlockBytes / 4)
    {
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(m_device, &mai, nullptr, &out.mem) != VK_SUCCESS)
        { out.mem = VK_NULL_HANDLE; return false; }
        out.size = req.size;
        return true;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (size_t i = 0; i < m_memBlocks.size(); ++i)
        {
            MemBlock& b = m_memBlocks[i];
            if (!b.mem || b.type != type) continue;
            const uint64_t off = b.ranges.allocate(req.size, req.alignment);
            if (off == HE::vkup::kFail) continue;
            out.mem = b.mem; out.offset = off; out.size = req.size; out.block = int32_t(i);
            return true;
        }
        if (pass == 1) break;
        // No block of this type has room: open a new one (reusing a freed slot).
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = k_memBlockBytes;
        mai.memoryTypeIndex = type;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
        auto slot = std::find_if(m_memBlocks.begin(), m_memBlocks.end(),
                                 [](const MemBlock& b) { return b.mem == VK_NULL_HANDLE; });
        if (slot == m_memBlocks.end()) slot = m_memBlocks.insert(m_memBlocks.end(), MemBlock{});
        slot->mem  = mem;
        slot->type = type;
        slot->ranges.reset(k_memBlockBytes);
    }
    return false;
}

void VulkanRenderer::freePooled(PoolAlloc& a)
{
    if (!a.mem) { a = PoolAlloc{}; return; }
    if (a.block < 0) vkFreeMemory(m_device, a.mem, nullptr);
    else if (size_t(a.block) < m_memBlocks.size() && m_memBlocks[a.block].mem == a.mem)
    {
        MemBlock& b = m_memBlocks[a.block];
        b.ranges.free(a.offset, a.size);
        // An empty block goes back to the driver: a level change should not keep
        // the last level's peak. Callers free only after a wait-idle (or with the
        // buffer never submitted), so nothing in flight reads the block.
        if (b.ranges.empty()) { vkFreeMemory(m_device, b.mem, nullptr); b = MemBlock{}; }
    }
    a = PoolAlloc{};
}

bool VulkanRenderer::createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                             const void* data, VkBuffer& buf, PoolAlloc& alloc)
{
    buf = VK_NULL_HANDLE; alloc = PoolAlloc{};
    if (size == 0 || !data) return false;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) { buf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, buf, &req);
    if (!allocPooled(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, alloc))
    { vkDestroyBuffer(m_device, buf, nullptr); buf = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(m_device, buf, alloc.mem, alloc.offset);
    // Always through staging, also where DEVICE_LOCAL memory is host-visible
    // (UMA, lavapipe): that keeps the path CI runs the one a dGPU runs.
    VkBuffer src = VK_NULL_HANDLE; VkDeviceSize srcOff = 0;
    if (!stageUpload(data, size, src, srcOff))
    { destroyPooledBuffer(buf, alloc); return false; }
    const VkBufferCopy region{ srcOff, 0, size };
    vkCmdCopyBuffer(uploadCmd(), src, buf, 1, &region);
    return true;
}

void VulkanRenderer::destroyPooledBuffer(VkBuffer& buf, PoolAlloc& alloc)
{
    if (buf) vkDestroyBuffer(m_device, buf, nullptr);
    buf = VK_NULL_HANDLE;
    freePooled(alloc);
}

bool VulkanRenderer::ensureUploadContext()
{
    if (m_uploadPool) return true;
    if (m_upContextTried || !m_device) return false;
    m_upContextTried = true;
    m_upSync       = envOn("HE_VK_UPLOAD_SYNC");
    m_upLogBatches = envOn("HE_VK_UPLOAD_STATS");

    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = m_graphicsFamily;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                          | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(m_device, &cpci, nullptr, &m_uploadPool) != VK_SUCCESS)
    {
        m_uploadPool = VK_NULL_HANDLE;
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: upload command pool creation failed");
        return false;
    }

    // The staging ring. Without it every upload stages through a buffer of its
    // own (still no wait), so a failure here only costs allocations.
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = k_stagingRingBytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bool ringOk = vkCreateBuffer(m_device, &bci, nullptr, &m_stageBuf) == VK_SUCCESS;
    if (ringOk)
    {
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, m_stageBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped = nullptr;
        ringOk = vkAllocateMemory(m_device, &mai, nullptr, &m_stageMem) == VK_SUCCESS
              && vkBindBufferMemory(m_device, m_stageBuf, m_stageMem, 0) == VK_SUCCESS
              && vkMapMemory(m_device, m_stageMem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
        m_stageMapped = static_cast<uint8_t*>(mapped);
    }
    if (!ringOk)
    {
        if (m_stageMem) vkFreeMemory(m_device, m_stageMem, nullptr);
        if (m_stageBuf) vkDestroyBuffer(m_device, m_stageBuf, nullptr);
        m_stageMem = VK_NULL_HANDLE; m_stageBuf = VK_NULL_HANDLE; m_stageMapped = nullptr;
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: staging ring unavailable — uploads stage one buffer each");
    }
    m_stageRing.reset(ringOk ? k_stagingRingBytes : 0);
    if (m_upSync)
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: HE_VK_UPLOAD_SYNC — waiting for the queue after every upload");
    return true;
}

VkCommandBuffer VulkanRenderer::uploadCmd()
{
    if (m_upRecording) return m_upOpen.cb;
    if (!ensureUploadContext()) return VK_NULL_HANDLE;
    retireUploads();
    UploadBatch b;
    if (!m_upSpare.empty()) { b = std::move(m_upSpare.back()); m_upSpare.pop_back(); }
    if (!b.cb)
    {
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool        = m_uploadPool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkAllocateCommandBuffers(m_device, &cbai, &b.cb) != VK_SUCCESS
            || vkCreateFence(m_device, &fci, nullptr, &b.fence) != VK_SUCCESS)
        {
            if (b.cb) vkFreeCommandBuffers(m_device, m_uploadPool, 1, &b.cb);
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: upload command buffer allocation failed");
            return VK_NULL_HANDLE;
        }
    }
    else
    {
        vkResetFences(m_device, 1, &b.fence);
        vkResetCommandBuffer(b.cb, 0);
    }
    VkCommandBufferBeginInfo cbbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(b.cb, &cbbi);
    b.ticket  = ++m_upTicket;
    b.uploads = 0;
    b.bytes   = 0;
    m_upOpen      = std::move(b);
    m_upRecording = true;
    return m_upOpen.cb;
}

bool VulkanRenderer::stageUpload(const void* data, VkDeviceSize size, VkBuffer& src, VkDeviceSize& srcOffset)
{
    src = VK_NULL_HANDLE; srcOffset = 0;
    if (!data || size == 0 || !uploadCmd()) return false;
    // 16: a BC block, and a multiple of 4 for vkCmdCopyBufferToImage's offset rule.
    uint64_t off = m_stageRing.allocate(size, 16, m_upOpen.ticket);
    if (off == HE::vkup::kFail && !m_upInFlight.empty())
    {
        retireUploads();
        off = m_stageRing.allocate(size, 16, m_upOpen.ticket);
    }
    m_upOpen.uploads += 1;
    m_upOpen.bytes   += size;
    if (off != HE::vkup::kFail)
    {
        std::memcpy(m_stageMapped + off, data, static_cast<size_t>(size));
        src = m_stageBuf; srcOffset = off;
        return true;
    }
    // Bigger than the ring, or the ring is full of copies the GPU has not done
    // yet: a staging buffer of its own, freed with the batch — never a wait.
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, buf, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* p = nullptr;
    if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS)
    { vkDestroyBuffer(m_device, buf, nullptr); return false; }
    if (vkBindBufferMemory(m_device, buf, mem, 0) != VK_SUCCESS
        || vkMapMemory(m_device, mem, 0, size, 0, &p) != VK_SUCCESS)
    { vkDestroyBuffer(m_device, buf, nullptr); vkFreeMemory(m_device, mem, nullptr); return false; }
    std::memcpy(p, data, static_cast<size_t>(size));
    vkUnmapMemory(m_device, mem);
    m_upOpen.ownStaging.emplace_back(buf, mem);
    m_upStats.ownStaging += 1;
    src = buf;
    return true;
}

void VulkanRenderer::flushUploads()
{
    if (!m_upRecording) return;
    m_upRecording = false;
    // One barrier closes the batch: every copy (buffers and images) visible to
    // every later stage — vertex/index fetch, any shader, the AS builds. The
    // images' own barriers did the layout transitions.
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                     | VK_ACCESS_INDEX_READ_BIT  | VK_ACCESS_SHADER_READ_BIT
                     | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(m_upOpen.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(m_upOpen.cb);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &m_upOpen.cb;
    vkCheck(vkQueueSubmit(m_graphicsQueue, 1, &si, m_upOpen.fence), "upload batch submit");
    m_upStats.batches += 1;
    if (m_upLogBatches)
        HE_LOG_INFO(RHI, "VulkanRenderer: upload batch %llu — %u uploads, %.2f MB, %zu own staging buffers",
                    static_cast<unsigned long long>(m_upOpen.ticket), m_upOpen.uploads,
                    double(m_upOpen.bytes) / (1024.0 * 1024.0), m_upOpen.ownStaging.size());
    m_upInFlight.push_back(std::move(m_upOpen));
    m_upOpen = UploadBatch{};
}

void VulkanRenderer::retireUploads()
{
    while (!m_upInFlight.empty()
           && vkGetFenceStatus(m_device, m_upInFlight.front().fence) == VK_SUCCESS)
    {
        UploadBatch b = std::move(m_upInFlight.front());
        m_upInFlight.pop_front();
        for (auto& [buf, mem] : b.ownStaging)
        {
            vkDestroyBuffer(m_device, buf, nullptr);
            vkFreeMemory(m_device, mem, nullptr);
        }
        b.ownStaging.clear();
        m_stageRing.retire(b.ticket);
        m_upSpare.push_back(std::move(b));
    }
}

void VulkanRenderer::noteUpload(bool mesh, VkDeviceSize bytes, uint64_t startNs)
{
    (mesh ? m_upStats.meshes : m_upStats.textures) += 1;
    m_upStats.bytes += bytes;
    if (m_upSync)
    {
        // The old behaviour, for the A/B: submit and wait for the queue to drain.
        const uint64_t w0 = uploadClockNs();
        flushUploads();
        vkQueueWaitIdle(m_graphicsQueue);
        retireUploads();
        m_upStats.syncWaits += 1;
        m_upStats.waitMs += double(uploadClockNs() - w0) * 1e-6;
    }
    m_upStats.cpuMs += double(uploadClockNs() - startNs) * 1e-6;
}

VkResult VulkanRenderer::submitGraphics(const VkSubmitInfo& si, VkFence fence)
{
    flushUploads();
    return vkQueueSubmit(m_graphicsQueue, 1, &si, fence);
}

void VulkanRenderer::deviceWaitIdle()
{
    if (!m_device) return;
    flushUploads();
    vkDeviceWaitIdle(m_device);
    retireUploads();
}

void VulkanRenderer::destroyUploadResources()
{
    if (!m_device) return;
    // Shutdown already waited for the device; an upload recorded since then is
    // dropped unsubmitted.
    if (m_upRecording) { m_upInFlight.push_back(std::move(m_upOpen)); m_upOpen = UploadBatch{}; m_upRecording = false; }
    for (UploadBatch& b : m_upInFlight)  m_upSpare.push_back(std::move(b));
    m_upInFlight.clear();
    for (UploadBatch& b : m_upSpare)
    {
        for (auto& [buf, mem] : b.ownStaging)
        {
            vkDestroyBuffer(m_device, buf, nullptr);
            vkFreeMemory(m_device, mem, nullptr);
        }
        if (b.fence) vkDestroyFence(m_device, b.fence, nullptr);
    }
    m_upSpare.clear();
    if (m_uploadPool) vkDestroyCommandPool(m_device, m_uploadPool, nullptr); // frees the CBs
    m_uploadPool = VK_NULL_HANDLE;
    if (m_stageMapped) vkUnmapMemory(m_device, m_stageMem);
    if (m_stageBuf)    vkDestroyBuffer(m_device, m_stageBuf, nullptr);
    if (m_stageMem)    vkFreeMemory(m_device, m_stageMem, nullptr);
    m_stageMapped = nullptr; m_stageBuf = VK_NULL_HANDLE; m_stageMem = VK_NULL_HANDLE;
    m_stageRing.reset(0);
    // Whatever is still in a block was never freed by its owner; the device goes
    // next, so release the memory rather than leak it.
    size_t leftover = 0;
    for (MemBlock& b : m_memBlocks)
        if (b.mem) { leftover += b.ranges.used() ? 1 : 0; vkFreeMemory(m_device, b.mem, nullptr); }
    m_memBlocks.clear();
    if (leftover)
        HE_LOG_WARN(RHI, "VulkanRenderer: %zu mesh memory blocks still held buffers at shutdown", leftover);
    HE_LOG_INFO(RHI,
        "VulkanRenderer: uploads — %llu meshes, %llu textures, %.1f MB in %llu batches "
        "(%llu oversize staging buffers); %.1f ms CPU in upload calls, %llu waits / %.1f ms waiting%s",
        static_cast<unsigned long long>(m_upStats.meshes), static_cast<unsigned long long>(m_upStats.textures),
        double(m_upStats.bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(m_upStats.batches),
        static_cast<unsigned long long>(m_upStats.ownStaging), m_upStats.cpuMs,
        static_cast<unsigned long long>(m_upStats.syncWaits), m_upStats.waitMs,
        m_upSync ? " (HE_VK_UPLOAD_SYNC)" : "");
}

// ─── Pipeline cache (REN-808) ────────────────────────────────────────────────
// See the header. The file lives beside the GL program-binary and D3D bytecode
// caches; SDL_GetPrefPath asks the shell for the roaming folder and ignores
// %APPDATA%, so a scratch APPDATA does not isolate it — HE_VK_PIPELINE_CACHE_DIR does.
VkResult VulkanRenderer::createGraphicsPipelines(uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                 const VkAllocationCallbacks* alloc, VkPipeline* out)
{
    const uint64_t t0 = uploadClockNs();
    const VkResult r = vkCreateGraphicsPipelines(m_device, m_pipelineCache, n, ci, alloc, out);
    m_pipelineMs += double(uploadClockNs() - t0) * 1e-6;
    m_pipelinesCreated += n;
    return r;
}

VkResult VulkanRenderer::createComputePipelines(uint32_t n, const VkComputePipelineCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkPipeline* out)
{
    const uint64_t t0 = uploadClockNs();
    const VkResult r = vkCreateComputePipelines(m_device, m_pipelineCache, n, ci, alloc, out);
    m_pipelineMs += double(uploadClockNs() - t0) * 1e-6;
    m_pipelinesCreated += n;
    return r;
}

uint64_t VulkanRenderer::GetPipelineCache() const
{
    uint64_t v = 0;
    static_assert(sizeof(m_pipelineCache) == sizeof(v), "VkPipelineCache size mismatch");
    std::memcpy(&v, &m_pipelineCache, sizeof(v));
    return v;
}

void VulkanRenderer::createPipelineCache()
{
    if (const char* v = std::getenv("HE_VK_PIPELINE_CACHE"); v && v[0] == '0')
    {
        m_pipelineCacheState = "off (HE_VK_PIPELINE_CACHE=0)";
        HE_LOG_INFO(RHI, "%s", "[startup] Vulkan pipeline cache: off (HE_VK_PIPELINE_CACHE=0)");
        return;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physDevice, &props);
    HE::vkpc::DeviceId dev;
    dev.vendorID      = props.vendorID;
    dev.deviceID      = props.deviceID;
    dev.driverVersion = props.driverVersion;
    std::memcpy(dev.uuid, props.pipelineCacheUUID, sizeof(dev.uuid));

    namespace fs = std::filesystem;
    fs::path dir;
    if (const char* over = std::getenv("HE_VK_PIPELINE_CACHE_DIR"); over && *over)
        dir = over;
    else if (char* pref = SDL_GetPrefPath("HorizonCreations", "HorizonEngine"))
    {
        dir = fs::path(pref) / "vkpipelinecache"; SDL_free(pref);
    }
    else
        dir = fs::temp_directory_path() / "HorizonEngine" / "vkpipelinecache";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // One file per GPU: switching between an iGPU and a dGPU keeps both caches.
    char name[32];
    std::snprintf(name, sizeof(name), "%04x_%04x.bin", props.vendorID, props.deviceID);
    m_pipelineCachePath = (dir / name).string();

    std::vector<uint8_t> file;
    if (std::ifstream in(m_pipelineCachePath, std::ios::binary | std::ios::ate); in)
    {
        const std::streamoff size = in.tellg();
        if (size > 0)
        {
            file.resize(static_cast<size_t>(size));
            in.seekg(0);
            if (!in.read(reinterpret_cast<char*>(file.data()), size)) file.clear();
        }
    }
    const uint8_t* blob = nullptr;
    size_t blobSize = 0;
    const HE::vkpc::Verdict verdict = HE::vkpc::unwrap(dev, file.data(), file.size(), blob, blobSize);

    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (verdict == HE::vkpc::Verdict::Ok)
    {
        ci.initialDataSize = blobSize;
        ci.pInitialData    = blob;
    }
    VkResult r = vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache);
    if (r != VK_SUCCESS && ci.initialDataSize)
    {
        // The driver refused the data after all: start empty, the file is rewritten at shutdown.
        ci.initialDataSize = 0; ci.pInitialData = nullptr;
        r = vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache);
        m_pipelineCacheState = "rejected by driver";
    }
    else
    {
        m_pipelineCacheState = verdict == HE::vkpc::Verdict::Ok ? "loaded" : HE::vkpc::verdictName(verdict);
    }
    if (r != VK_SUCCESS)
    {
        m_pipelineCache = VK_NULL_HANDLE;
        m_pipelineCacheState = "vkCreatePipelineCache failed";
    }
    else if (ci.initialDataSize)
    {
        m_pipelineCacheInBytes = blobSize;
        m_pipelineCacheInHash  = HE::vkpc::blobHash(blob, blobSize);
    }
    HE_LOG_INFO(RHI, "[startup] Vulkan pipeline cache: %s, %zu KB from %s",
                m_pipelineCacheState, m_pipelineCacheInBytes / 1024, m_pipelineCachePath.c_str());
}

void VulkanRenderer::savePipelineCache()
{
    // The totals are logged with the cache off too: they are the A/B's other side.
    size_t size = 0;
    std::vector<uint8_t> blob;
    if (m_pipelineCache)
    {
        if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) == VK_SUCCESS && size)
        {
            blob.resize(size);
            if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, blob.data()) != VK_SUCCESS) blob.clear();
            blob.resize(std::min(size, blob.size()));
        }
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }

    const char* what = "unchanged";
    if (m_pipelineCachePath.empty())
        what = "not saved";
    else if (blob.empty())
        what = "no data";
    else if (blob.size() != m_pipelineCacheInBytes ||
             HE::vkpc::blobHash(blob.data(), blob.size()) != m_pipelineCacheInHash)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physDevice, &props);
        HE::vkpc::DeviceId dev;
        dev.vendorID      = props.vendorID;
        dev.deviceID      = props.deviceID;
        dev.driverVersion = props.driverVersion;
        std::memcpy(dev.uuid, props.pipelineCacheUUID, sizeof(dev.uuid));
        const std::vector<uint8_t> file = HE::vkpc::wrap(dev, blob.data(), blob.size());
        // Write beside it and rename over: a second editor reading it at that
        // moment, or a crash mid-write, never sees half a file.
        std::filesystem::path tmp = m_pipelineCachePath;
        tmp += ".tmp" + std::to_string(static_cast<unsigned long long>(uploadClockNs() & 0xFFFFFF));
        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            ok = out && out.write(reinterpret_cast<const char*>(file.data()),
                                  static_cast<std::streamsize>(file.size())).good();
        }
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, m_pipelineCachePath, ec);
        if (!ok || ec) { std::filesystem::remove(tmp, ec); what = "write failed"; }
        else what = "written";
    }
    HE_LOG_INFO(RHI, "VulkanRenderer: pipelines — %u created, %.1f ms in vkCreate*Pipelines; "
                "cache %s (in %zu KB, out %zu KB, %s)",
                m_pipelinesCreated, m_pipelineMs, m_pipelineCacheState,
                m_pipelineCacheInBytes / 1024, blob.size() / 1024, what);
}

bool VulkanRenderer::createMeshBuffers(GpuMesh& mesh, const std::vector<float>& interleaved,
                                       const std::vector<uint32_t>& indices)
{
    const uint64_t t0 = uploadClockNs();
    const VkDeviceSize vsize = interleaved.size() * sizeof(float);
    const VkDeviceSize isize = indices.size() * sizeof(uint32_t);
    meshStreamCounters().meshUploads.fetch_add(1, std::memory_order_relaxed);
    meshStreamCounters().meshUploadBytes.fetch_add(vsize + isize, std::memory_order_relaxed);
    if (!createDeviceLocalBuffer(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, interleaved.data(),
                                 mesh.vbuf, mesh.valloc))
        return false;
    if (!createDeviceLocalBuffer(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(),
                                 mesh.ibuf, mesh.ialloc))
    {
        // The vertex copy is already recorded: the buffer must outlive the batch.
        // Rare enough (out of device memory) to settle with a wait.
        deviceWaitIdle();
        destroyPooledBuffer(mesh.vbuf, mesh.valloc);
        return false;
    }
    mesh.indexCount = static_cast<uint32_t>(indices.size());
    noteUpload(true, vsize + isize, t0);
    return true;
}

// ─── Motion trails ───────────────────────────────────────────────────────────
// One host-visible vertex/index buffer pair per band per frame in flight, kept
// mapped and refilled every frame. Growing means destroying and recreating that
// slot's buffer, which is safe here and only here: Render() waited on this
// frame slot's fence before recording, so the GPU is done with what the slot
// held two frames ago.
bool VulkanRenderer::uploadRibbon(uint32_t frame, size_t index,
                                  const std::vector<float>& verts,
                                  const std::vector<uint32_t>& indices, GpuMesh& out)
{
    if (frame >= k_maxFramesInFlight || verts.empty() || indices.empty()) return false;
    auto& pool = m_ribbonMeshes[frame];
    if (pool.size() <= index) pool.resize(index + 1);
    RibbonMeshVk& rm = pool[index];

    auto ensure = [&](VkDeviceSize need, VkBufferUsageFlags usage, VkBuffer& buf,
                      VkDeviceMemory& mem, void*& mapped, VkDeviceSize& cap) -> bool
    {
        if (buf != VK_NULL_HANDLE && mapped && cap >= need) return true;
        if (mapped) { vkUnmapMemory(m_device, mem); mapped = nullptr; }
        if (buf)    { vkDestroyBuffer(m_device, buf, nullptr); buf = VK_NULL_HANDLE; }
        if (mem)    { vkFreeMemory(m_device, mem, nullptr);    mem = VK_NULL_HANDLE; }
        cap = 0;
        // Head-room so a trail that grows a point per frame does not reallocate
        // every frame while it fills up.
        const VkDeviceSize size = need + need / 2 + 256;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = usage;
        if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS) { buf = VK_NULL_HANDLE; return false; }
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, buf, nullptr); buf = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
            return false;
        }
        vkBindBufferMemory(m_device, buf, mem, 0);
        if (vkMapMemory(m_device, mem, 0, size, 0, &mapped) != VK_SUCCESS) { mapped = nullptr; return false; }
        cap = size;
        return true;
    };

    const VkDeviceSize vsize = verts.size()   * sizeof(float);
    const VkDeviceSize isize = indices.size() * sizeof(uint32_t);
    if (!ensure(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rm.vbuf, rm.vmem, rm.vmapped, rm.vcap)) return false;
    if (!ensure(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  rm.ibuf, rm.imem, rm.imapped, rm.icap)) return false;
    std::memcpy(rm.vmapped, verts.data(),   static_cast<size_t>(vsize));
    std::memcpy(rm.imapped, indices.data(), static_cast<size_t>(isize));

    // The pool owns the memory — `out` only borrows the handles, so its vmem/imem
    // stay null and nothing downstream can free them.
    out = GpuMesh{};
    out.vbuf       = rm.vbuf;
    out.ibuf       = rm.ibuf;
    out.indexCount = static_cast<uint32_t>(indices.size());
    return true;
}

void VulkanRenderer::destroyRibbonMeshes()
{
    for (uint32_t f = 0; f < k_maxFramesInFlight; ++f)
    {
        for (RibbonMeshVk& rm : m_ribbonMeshes[f])
        {
            if (rm.vmapped) vkUnmapMemory(m_device, rm.vmem);
            if (rm.vbuf)    vkDestroyBuffer(m_device, rm.vbuf, nullptr);
            if (rm.vmem)    vkFreeMemory(m_device, rm.vmem, nullptr);
            if (rm.imapped) vkUnmapMemory(m_device, rm.imem);
            if (rm.ibuf)    vkDestroyBuffer(m_device, rm.ibuf, nullptr);
            if (rm.imem)    vkFreeMemory(m_device, rm.imem, nullptr);
        }
        m_ribbonMeshes[f].clear();
    }
}

bool VulkanRenderer::uploadRGBA8Image(const uint8_t* rgba, uint32_t width, uint32_t height,
                                      VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; view = VK_NULL_HANDLE;
    if (!rgba || width == 0 || height == 0) return false;
    const uint64_t t0 = uploadClockNs();
    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * 4u;

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
    { image = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements imr{}; vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imal, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    // View and staging first: every failure exit lies before the copy is recorded,
    // so nothing destroyed here is referenced by the open upload batch.
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = image;
    ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBuffer stageBuf = VK_NULL_HANDLE; VkDeviceSize stageOff = 0;
    if (vkCreateImageView(m_device, &ivci, nullptr, &view) != VK_SUCCESS
        || !stageUpload(rgba, dataSize, stageBuf, stageOff))
    {
        if (view) vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);  memory = VK_NULL_HANDLE;
        return false;
    }

    // Into the upload batch: UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY.
    // The read side is ALL_COMMANDS: the batch is submitted ahead of whatever
    // samples the image next, and that may be a vertex or compute stage.
    VkCommandBuffer oneCB = uploadCmd();
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
    region.bufferOffset     = stageOff;
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
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    noteUpload(false, dataSize, t0);
    return true;
}

// Upload a cooked TextureAsset (RGBA8 or block-compressed BC7/BC3/BC1/BC4/BC5) with its full
// pre-baked mip chain to a device-local sampled image + view. All levels are staged
// into one buffer and copied per-mip; block formats use no runtime mip generation.
// Returns false when the device can't sample the shipped format (→ untextured draw).
bool VulkanRenderer::uploadTextureImage(const TextureAsset* tex,
                                        VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                                        bool asArray, bool honourSrgb)
{
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; view = VK_NULL_HANDLE;
    if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
        return false;
    const uint32_t width  = static_cast<uint32_t>(tex->width);
    const uint32_t height = static_cast<uint32_t>(tex->height);
    const uint32_t mips   = tex->mipLevels > 0 ? tex->mipLevels : 1;

    // Resolve the shipped format to a VkFormat + block flag. ASTC is Metal-only and
    // never packed for Vulkan; treat it (and anything unknown) as unsupported.
    // TextureAsset::srgb (the importer's colour-vs-data decision) selects the _SRGB
    // twin, used for image and view alike: the sampler then decodes to linear, so
    // colour textures shade in linear light and only the tonemap's gamma encode
    // re-curves them. Twins share block/byte layout, so the level math is unchanged
    // and the format-properties check below validates whichever twin we picked.
    const bool srgb = tex->srgb && honourSrgb;
    VkFormat vkFmt; bool isBlock;
    switch (tex->format)
    {
    case TextureFormat::RGBA8: vkFmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB   : VK_FORMAT_R8G8B8A8_UNORM;  isBlock = false; break;
    case TextureFormat::BC7:   vkFmt = srgb ? VK_FORMAT_BC7_SRGB_BLOCK  : VK_FORMAT_BC7_UNORM_BLOCK; isBlock = true;  break;
    case TextureFormat::BC3:   vkFmt = srgb ? VK_FORMAT_BC3_SRGB_BLOCK  : VK_FORMAT_BC3_UNORM_BLOCK; isBlock = true;  break;
    // BC1 as the opaque RGB variant (alpha reads 1), like GL's RGB_S3TC_DXT1.
    case TextureFormat::BC1:   vkFmt = srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK; isBlock = true; break;
    // BC4 / BC5 have no sRGB twin: they carry data (single channel, normal XY).
    case TextureFormat::BC4:   vkFmt = VK_FORMAT_BC4_UNORM_BLOCK; isBlock = true; break;
    case TextureFormat::BC5:   vkFmt = VK_FORMAT_BC5_UNORM_BLOCK; isBlock = true; break;
    default: return false;
    }
    // Texture ARRAY (Thema 158): every slice of a layers > 1 asset, one after the
    // other in the staging buffer exactly as HE::buildTextureArray stores them
    // (slice-major, each slice with its full chain), RGBA8 or a block format. A 2D asset in an
    // array slot is one slice; an array asset in a 2D slot reads slice 0 -- its
    // leading bytes -- through the plain path below.
    const uint32_t layers = asArray ? std::max<uint32_t>(1, tex->layers) : 1;
    if (layers > 1 && !HE::textureArrayPayloadValid(*tex)) return false;
    // Device must actually sample this format (BC support is optional in Vulkan). A
    // format the CPU can expand (BC1/BC3/BC4/BC5) is uploaded as RGBA8 instead of
    // leaving the draw untextured.
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_physDevice, vkFmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    {
        TextureAsset rgba;
        if (!isBlock || !HE::textureFormatCpuDecodable(tex->format) || !HE::decodeTextureToRGBA8(*tex, rgba))
            return false;
        HE_LOG_WARN(RHI, "VulkanRenderer: device can't sample %s textures; uploading them as RGBA8",
                    textureFormatName(tex->format));
        return uploadTextureImage(&rgba, image, memory, view, asArray, honourSrgb);
    }

    // Per-level byte size + running offsets (level 0 first). Block: 8 B (BC1/BC4) or
    // 16 B per 4x4 block.
    const TextureFormat texFmt = tex->format;
    auto levelBytes = [texFmt](uint32_t w, uint32_t h) -> VkDeviceSize {
        return static_cast<VkDeviceSize>(textureLevelBytes(texFmt, w, h));
    };
    std::vector<VkDeviceSize> offsets(mips);
    VkDeviceSize total = 0;
    { uint32_t lw = width, lh = height;
      for (uint32_t l = 0; l < mips; ++l)
      { offsets[l] = total; total += levelBytes(lw, lh);
        lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1; } }
    const VkDeviceSize sliceBytes = total;     // one slice's chain
    total *= layers;
    if (tex->data.size() < total) return false; // truncated payload
    const uint64_t t0 = uploadClockNs();

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkFmt;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = mips;
    ici.arrayLayers   = layers;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &image) != VK_SUCCESS)
    { image = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements imr{}; vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imal, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(m_device, image, memory, 0);

    // View and staging (the whole, already tightly packed chain) before anything
    // is recorded, so a failure destroys nothing the upload batch references.
    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image            = image;
    ivci.viewType         = asArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    ivci.format           = vkFmt;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers };
    VkBuffer stageBuf = VK_NULL_HANDLE; VkDeviceSize stageOff = 0;
    if (vkCreateImageView(m_device, &ivci, nullptr, &view) != VK_SUCCESS
        || !stageUpload(tex->data.data(), total, stageBuf, stageOff))
    {
        if (view) vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);  memory = VK_NULL_HANDLE;
        return false;
    }

    // Into the upload batch: UNDEFINED → TRANSFER_DST → copy every mip → SHADER_READ
    // (ALL_COMMANDS on the read side, see uploadRGBA8Image).
    VkCommandBuffer oneCB = uploadCmd();
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers };
        bar.srcAccessMask    = 0;
        bar.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    // One copy command for the whole chain (all slices, all levels).
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(size_t(layers) * mips);
    for (uint32_t s = 0; s < layers; ++s)
    { uint32_t lw = width, lh = height;
      for (uint32_t l = 0; l < mips; ++l)
      {
        VkBufferImageCopy region{};
        // Must be a multiple of the texel block size: 8 or 16 for blocks, 4 for RGBA8.
        // stageOff is 16-aligned and every level/slice size is a whole number of
        // blocks, so the sum keeps it.
        region.bufferOffset     = stageOff + s * sliceBytes + offsets[l];
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, s, 1 };
        region.imageExtent      = { lw, lh, 1 };
        regions.push_back(region);
        lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1;
      } }
    vkCmdCopyBufferToImage(oneCB, stageBuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image            = image;
        bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers };
        bar.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(oneCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
    }
    noteUpload(false, total, t0);
    return true;
}

// Move the current height-field image to the retire list (frames in flight may still sample it).
void VulkanRenderer::retireGiHeightfield()
{
    if (m_giHfImage || m_giHfView || m_giHfMem)
        m_retiredHf.push_back({ m_giHfImage, m_giHfView, m_giHfMem, static_cast<int>(k_maxFramesInFlight) + 2 });
    m_giHfImage = VK_NULL_HANDLE; m_giHfView = VK_NULL_HANDLE; m_giHfMem = VK_NULL_HANDLE;
}

// R32_SFLOAT image whose mips are the pyramid's levels (GiHeightfield.h), sampled with texelFetch by gi_shadow.comp.
bool VulkanRenderer::uploadHeightPyramid(const HE::GiHeightPyramid& pyr, VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; view = VK_NULL_HANDLE;
    if (pyr.levels.empty()) return false;
    const uint32_t mips = static_cast<uint32_t>(pyr.levels.size());
    const uint32_t dim0 = static_cast<uint32_t>(pyr.dims[0]);
    std::vector<VkDeviceSize> offsets(mips);
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < mips; ++l) { offsets[l] = total; total += pyr.levels[l].size() * sizeof(float); }
    std::vector<uint8_t> packed(static_cast<size_t>(total));
    for (uint32_t l = 0; l < mips; ++l)
        std::memcpy(packed.data() + offsets[l], pyr.levels[l].data(), pyr.levels[l].size() * sizeof(float));
    const uint64_t t0 = uploadClockNs();

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R32_SFLOAT;
    ici.extent = { dim0, dim0, 1 }; ici.mipLevels = mips; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ici, nullptr, &image) != VK_SUCCESS) { image = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements imr{}; vkGetImageMemoryRequirements(m_device, image, &imr);
    VkMemoryAllocateInfo imal{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imal.allocationSize  = imr.size;
    imal.memoryTypeIndex = findMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imal, nullptr, &memory) != VK_SUCCESS)
    { vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; return false; }
    vkBindImageMemory(m_device, image, memory, 0);

    VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = image; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_R32_SFLOAT;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    VkBuffer stageBuf = VK_NULL_HANDLE; VkDeviceSize stageOff = 0;
    if (vkCreateImageView(m_device, &ivci, nullptr, &view) != VK_SUCCESS || !stageUpload(packed.data(), total, stageBuf, stageOff))
    {
        if (view) vkDestroyImageView(m_device, view, nullptr);
        view = VK_NULL_HANDLE;
        vkDestroyImage(m_device, image, nullptr); image = VK_NULL_HANDLE;
        vkFreeMemory(m_device, memory, nullptr);  memory = VK_NULL_HANDLE;
        return false;
    }
    VkCommandBuffer oneCB = uploadCmd();
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier bar{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        bar.oldLayout = from; bar.newLayout = to;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = image; bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
        bar.srcAccessMask = src; bar.dstAccessMask = dst;
        vkCmdPipelineBarrier(oneCB, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &bar);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::vector<VkBufferImageCopy> regions(mips);
    for (uint32_t l = 0; l < mips; ++l)
    {
        regions[l] = {};
        regions[l].bufferOffset     = stageOff + offsets[l];   // 4-byte floats: every offset is a multiple of 4
        regions[l].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
        regions[l].imageExtent      = { static_cast<uint32_t>(pyr.dims[l]), static_cast<uint32_t>(pyr.dims[l]), 1 };
    }
    vkCmdCopyBufferToImage(oneCB, stageBuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions.data());
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    noteUpload(false, total, t0);
    return true;
}

// Upload the height field when it is new or rebuilt. False = unusable (the kernel keeps the chunk meshes).
bool VulkanRenderer::ensureGiHeightfield(const GiHeightfield& hf)
{
    HE_PROFILE_SCOPE_N("Vulkan::GiHeightfield");
    if (!hf.data || hf.res < 8 || hf.data->size() < size_t(hf.res) * hf.res) return false;
    if (m_giHfView && m_giHfVersion == hf.version && m_giHfKey == hf.key) return true;
    const HE::GiHeightPyramid pyr = HE::BuildGiHeightPyramid(hf);
    VkImage img; VkDeviceMemory mem; VkImageView view;
    if (!uploadHeightPyramid(pyr, img, mem, view)) return false;
    retireGiHeightfield();
    m_giHfImage = img; m_giHfMem = mem; m_giHfView = view;
    m_giHfVersion = hf.version; m_giHfKey = hf.key;
    m_giHfTopLevel = static_cast<int>(pyr.levels.size()) - 1;
    m_giHfMaxH = pyr.maxHeight;
    HE_LOG_INFO(RHI, "VulkanRenderer: GI height field %dx%d, %zu mips, max height %.2f",
                pyr.dims[0], pyr.dims[0], pyr.levels.size(), pyr.maxHeight);
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
    if (!tex) return false;

    // The set first: the upload only records into the open upload batch, so an
    // image dropped after it (pool full) would be one a pending copy still writes.
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = m_albedoPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_albedoSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS)
    {
        // Pool exhausted (> k_maxMeshTextures unique textured meshes) — drop to flat.
        set = VK_NULL_HANDLE;
        return false;
    }
    // RGBA8 + cooked BC7/BC3 (full pre-baked mip chain); uploadTextureImage returns
    // false when this device can't sample the shipped format → untextured fallback.
    if (!uploadTextureImage(tex, image, memory, view))
    {
        vkFreeDescriptorSets(m_device, m_albedoPool, 1, &set); // pool has the FREE bit
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

void VulkanRenderer::destroyGpuMesh(GpuMesh& m)
{
    destroyPooledBuffer(m.vbuf, m.valloc);
    destroyPooledBuffer(m.ibuf, m.ialloc);
    if (m.albedoSet)  vkFreeDescriptorSets(m_device, m_albedoPool, 1, &m.albedoSet);
    if (m.albedoView) vkDestroyImageView(m_device, m.albedoView, nullptr);
    if (m.albedoImage)vkDestroyImage(m_device, m.albedoImage, nullptr);
    if (m.albedoMem)  vkFreeMemory(m_device, m.albedoMem, nullptr);
}

void VulkanRenderer::processPendingInvalidations()
{
    if (m_pendingMatInval.empty() && m_pendingMeshInval.empty() && m_pendingTexInval.empty()) return;
    HE_PROFILE_SCOPE_N("Vulkan::DrainInvalidations");
    // Editor-only path; a full device idle keeps the frees below trivially safe (no in-flight
    // frame references the dropped resources). The idle is a stall, but invalidation is rare
    // outside a terrain-sculpt drag — a documented follow-up (D3D12 uses a per-frame retire list).
    meshStreamCounters().meshInvalidations.fetch_add(
        static_cast<uint32_t>(m_pendingMeshInval.size()), std::memory_order_relaxed);
    // Static meshes alone (terrain LOD / stitch / sculpt, the per-frame case) retire their
    // buffers AND their hardware BLAS instead of idling the device: a frame in flight may
    // still trace the old BLAS through its TLAS, so it waits in m_retiredHwBlas for as many
    // frames as a mesh does. Anything else in the queue — a material or texture, a skeletal
    // mesh — keeps the idle, so those paths are untouched. (The idle for a BLAS cost ~3 ms of
    // CPU in one frame in five while flying over a landscape, perf capture 2026-10-28.)
    bool needIdle = !m_pendingMatInval.empty() || !m_pendingTexInval.empty();
    for (const HE::UUID& id : m_pendingMeshInval)
        if (m_skeletalMeshCache.count(id)) { needIdle = true; break; }
    if (needIdle)
    {
        meshStreamCounters().gpuStalls.fetch_add(1, std::memory_order_relaxed);
        deviceWaitIdle();
    }

    for (const HE::UUID& id : m_pendingMatInval)
        if (auto it = m_materialTexCache.find(id); it != m_materialTexCache.end())
        { destroyMaterialTex(it->second); m_materialTexCache.erase(it); }
    m_pendingMatInval.clear();
    for (const HE::UUID& id : m_pendingTexInval)
    {
        // The texture-array upload of the same asset lives under "#arr" (Thema 158).
        for (const std::string& key : { graphTexKey(id, {}), graphTexKey(id, {}) + "#arr" })
            if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end())
            { destroyMaterialTex(it->second); m_graphTexCache.erase(it); }
        if (auto it = m_uiImageCache.find(graphTexKey(id, {})); it != m_uiImageCache.end())
        { destroyUIImage(it->second); m_uiImageCache.erase(it); }
    }
    m_pendingTexInval.clear();

    for (const HE::UUID& id : m_pendingMeshInval)
    {
        if (auto it = m_meshCache.find(id); it != m_meshCache.end())
        {
            if (needIdle) destroyGpuMesh(it->second);
            else          m_retiredMeshes.push_back({ std::move(it->second), static_cast<int>(k_maxFramesInFlight) + 2 });
            m_meshCache.erase(it);
        }
        // Just this mesh's SW BLAS: its range turns into dead space and it
        // rebuilds lazily. Dropping the whole cache here cost every BVH of the
        // scene per re-stitched terrain chunk (Thema 221, GiBlasStore.h).
        m_giBlas.erase(id);
        // HW BLAS is per-mesh (no concatenated arrays) — drop just this one;
        // it rebuilds lazily on the next updateGiAccel.
        if (auto it = m_giHwBlasCache.find(id); it != m_giHwBlasCache.end())
        {
            if (needIdle) destroyGiHwBlas(it->second);   // device is idle
            else          m_retiredHwBlas.push_back({ std::move(it->second), static_cast<int>(k_maxFramesInFlight) + 2 });
            m_giHwBlasCache.erase(it);
        }
        if (auto it = m_skeletalMeshCache.find(id); it != m_skeletalMeshCache.end())
        {
            GpuSkeletalMesh& m = it->second;
            destroyPooledBuffer(m.vb,        m.vbAlloc);
            destroyPooledBuffer(m.boneIdVb,  m.boneIdAlloc);
            destroyPooledBuffer(m.boneWgtVb, m.boneWgtAlloc);
            destroyPooledBuffer(m.ib,        m.ibAlloc);
            if (m.albedoSet)  vkFreeDescriptorSets(m_device, m_albedoPool, 1, &m.albedoSet);
            if (m.texView)    vkDestroyImageView(m_device, m.texView, nullptr);
            if (m.texImage)   vkDestroyImage(m_device, m.texImage, nullptr);
            if (m.texMem)     vkFreeMemory(m_device, m.texMem, nullptr);
            m_skeletalMeshCache.erase(it);
        }
    }
    // A rebuilt mesh (sculpt, terrain LOD/tessellation) can change the scene
    // box without changing which objects exist → re-check the probe grid fit.
    if (!m_pendingMeshInval.empty()) m_giGridTrack.meshRebuilt = true;
    m_pendingMeshInval.clear();
}

// Build the queued materials' pipelines for the pass THIS frame draws into. Runs at
// DrawScene top, i.e. before the first draw that would otherwise pay the cross-compile
// + vkCreateGraphicsPipelines inside the loop. A material instance shares its master's
// hash, so a whole family warms on the first id; the rest are cache hits. The variant
// is the one the draw loop will pick: the material's own blend class (a per-entity tint
// alpha can still ask for the other one later — that is a single build, not a stall
// per frame). Built-in-PBR materials resolve no shader and are skipped.
void VulkanRenderer::drainMaterialWarmup(bool hdr)
{
    if (m_pendingMatWarmup.empty()) return;
    if (!m_matReady || !m_contentManager) { m_pendingMatWarmup.clear(); return; }
    std::vector<HE::UUID> ids;
    ids.swap(m_pendingMatWarmup);
    int built = 0;
    for (const HE::UUID& id : ids)
    {
        uint64_t hash = 0; std::string frag, vertBody;
        if (!m_matShaderLib.resolveShaders(*m_contentManager, id, hash, frag, vertBody)) continue;
        foldDisplacement(id, hash);
        // Deferred (Thema 150): the G-buffer variant too, so the first deferred
        // frame does not hitch on its cross-compile (GL's WarmupMaterials rule).
        // Only for the HDR viewport frame, the one that renders deferred.
        if (hdr && m_deferredReady && m_renderPath == HE::RenderPath::Deferred)
        {
            uint64_t gbHash = 0; std::string gbFrag, gbVert;
            if (m_matShaderLib.resolveGBufferShaders(*m_contentManager, id, gbHash, gbFrag, gbVert))
            {
                foldDisplacement(id, gbHash);
                if (GetOrBuildMaterialPipeline(gbHash, gbFrag, gbVert, nullptr, true, false, /*gbuffer=*/true))
                    ++built;
            }
        }
        const MaterialAsset* ma = m_contentManager->getMaterial(id);
        const bool transparent = ma && (ma->blendMode == 2
                                        || ma->opacity < RenderSorter::kOpaqueOpacityThreshold);
        const uint64_t key = hash ^ (hdr ? 0x9E3779B97F4A7C15ULL : 0ULL)
                                  ^ (transparent ? 0xD1B54A32D192ED03ULL : 0ULL);
        if (m_materialPipelines.count(key)) continue; // already warm (or a cached miss)
        const MaterialShaderVariant* pre =
            HE::MaterialShaderLibrary::precompiledFor(ma, HE::RendererBackend::Vulkan);
        if (GetOrBuildMaterialPipeline(hash, frag, vertBody, pre, hdr, transparent) != VK_NULL_HANDLE)
            ++built;
    }
    if (built > 0)
        HE_LOG_INFO(RHI, "%s",
            ("VulkanRenderer: warmed up " + std::to_string(built) + " material pipeline(s)").c_str());
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
    // fully resolved on insert. Both uploads only record into the open upload batch
    // (no stall); it is submitted ahead of the command buffer that draws the mesh.
    resolveAndUploadAlbedo(asset->materialId, asset->materialPath,
                           mesh.albedoImage, mesh.albedoMem, mesh.albedoView, mesh.albedoSet);
    return &m_meshCache.emplace(assetId, mesh).first->second;
}

uint32_t VulkanRenderer::pushSceneMaterial(const void* data, size_t bytes)
{
    MatFrameBuf& rb = m_sceneMatBuf[m_currentFrame];
    if (!rb.mapped || bytes > sizeof(m_sceneMatLast)) return m_sceneMatOffset;
    auto* base = static_cast<uint8_t*>(rb.mapped);
    // Same block as the draw before → same slot. Compared against a CPU copy:
    // the ring is mapped write-combined on discrete GPUs, slow to read back.
    if (m_sceneMatCursor > 0 && std::memcmp(m_sceneMatLast, data, bytes) == 0)
        return m_sceneMatOffset;
    if (m_sceneMatCursor >= k_sceneMatSlots)
    {
        if (!m_sceneMatWarned)
        {
            m_sceneMatWarned = true;
            HE_LOG_WARN(RHI, "%s",
                ("VulkanRenderer: more than " + std::to_string(k_sceneMatSlots)
                 + " built-in material blocks in one frame — the surplus draws reuse the "
                   "last one's material; raise k_sceneMatSlots").c_str());
        }
        return m_sceneMatOffset;
    }
    const VkDeviceSize off = static_cast<VkDeviceSize>(m_sceneMatCursor++) * m_sceneMatStride;
    std::memcpy(base + off, data, bytes);
    std::memcpy(m_sceneMatLast, data, bytes);
    m_sceneMatOffset = static_cast<uint32_t>(off);
    return m_sceneMatOffset;
}

void VulkanRenderer::DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool hdr)
{
    if (!m_world || m_scenePipeline == VK_NULL_HANDLE || width == 0 || height == 0) return;

    // Graph-material pipelines queued by WarmupMaterials, built against THIS frame's pass
    // before any draw below can stall on them.
    drainMaterialWarmup(hdr);

    // The day-night state (sun/moon direction for the sky and the lights) was
    // fed once at the top of Render() / RenderSceneImage(), so this extraction
    // sees the same sun as the cascades, GI and SSAO before it.
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld,
                        static_cast<float>(width) / static_cast<float>(height),
                        &m_editorCamera);
    // The UI canvas (Entity-UI + WidgetManager widgets) rides in the same
    // RenderWorld — extract() just cleared it. Mirrors GL/Metal; without this
    // call every UI pass below sees an empty list and draws nothing.
    m_extractor.extractUI(*m_world, static_cast<float>(width), static_cast<float>(height),
                          m_renderWorld);


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

    // Trails live in their own per-frame band list, not in `objects`, so a scene
    // that has nothing BUT a trail still has something to draw past here.
    if (m_renderWorld.objects.empty() && m_renderWorld.ribbonBatches.empty()) return;

    refineObjectBounds();
    // PBR scalars per object, per material slot and per skinned object, each from
    // its own material (+ the Translucent clamp) — what GL/Metal's per-draw
    // ResolveMaterialParams gives them, in time for partitionByOpacity.
    { HE_PROFILE_SCOPE_N("MaterialScalars"); HE::resolveWorldMaterialScalars(m_renderWorld, m_contentManager); }

    cullCameraObjects();
    m_statTotal   = static_cast<uint32_t>(m_renderWorld.objects.size());
    m_statVisible = static_cast<uint32_t>(m_sortedIndices.size());
    if (m_sortedIndices.empty() && m_renderWorld.ribbonBatches.empty()) return;

    if (m_renderGraph.empty())
    {
        auto geometry = std::make_unique<GeometryPass>();
        geometry->setInstanceSpans(RenderSorter::instanceSpansEnabled());   // read in place, see InstanceRun
        m_renderGraph.addPass(std::move(geometry));
    }

    // GL-convention projection from the extractor → Vulkan clip space. On a TAA
    // frame the scene rasterises with the jittered matrix (taaJittered); the
    // clean one is kept for the velocity pass, which measures with it.
    const glm::mat4 viewProjClean =
        HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;
    const glm::mat4 viewProj = taaJittered(viewProjClean);
    if (m_taaFrame)
    {
        m_taaViewProjClean = viewProjClean;
        m_taaViewProjJit   = viewProj;
        m_taaSceneSorted   = true;
    }

    // The local (point/spot) atlas is valid this frame when EncodeShadowMap
    // rendered at least one layer into it (and the array view is bound).
    const bool localShadows = m_localShadowLayerCount > 0 && m_localShadowView != VK_NULL_HANDLE;

    // Clustered lighting (plan P7 on the forward path): point/spot lights
    // leave the 8-light window for this frame slot's cluster SSBOs
    // (HE::BuildClusterLights). ONE build per frame serves the built-in scene
    // shader (Frame UBO below) and the graph materials (HeLighting gate,
    // Thema 117) — both read the same three SSBOs.
    const bool clustered = m_forwardClustered && m_clusterReady
                        && !m_renderWorld.lights.empty();
    HE::ClusterLightBuild frameClusters;
    if (clustered)
        frameClusters = HE::BuildClusterLights(m_renderWorld, localShadows, m_giRanThisFrame);

    // Per-frame UBO for this in-flight slot.
    {
        FrameUBOData f{};
        f.cameraPos     = glm::vec4(m_renderWorld.camera.position, 1.0f);
        // The built-in window then carries directional lights only. The slot's
        // fence was waited on in Render(), so the mapped buffers are free to
        // rewrite.
        if (clustered)
        {
            const HE::ClusterLightBuild& cb = frameClusters;
            if (cb.droppedLights > 0 && !m_clusterCapWarned)
            {
                m_clusterCapWarned = true;
                HE_LOG_WARN(RHI, "VulkanRenderer: %d point/spot light(s) exceed the cluster caps "
                                 "(%d lights / %d indices) and are not lit",
                            cb.droppedLights, HE::kMaxClusteredLights, HE::kMaxClusterIndices);
            }
            std::memcpy(m_clusterLights[m_currentFrame].mapped, cb.lights.data(),  cb.lights.size()  * sizeof(glm::vec4));
            std::memcpy(m_clusterGrid[m_currentFrame].mapped,   cb.grid.data(),    cb.grid.size()    * sizeof(glm::uvec2));
            std::memcpy(m_clusterIdx[m_currentFrame].mapped,    cb.indices.data(), cb.indices.size() * sizeof(uint32_t));
            f.clusterParams = cb.params;
            f.clusterCamFwd = cb.camFwd;
            const HE::DirectionalLightWindow w = HE::BuildDirectionalLightWindow(m_renderWorld);
            f.lightCount = glm::ivec4(w.count, 0, 0, 0);
            for (int i = 0; i < w.count; ++i)
            {
                f.lightPos[i]    = w.pos[i];
                f.lightDir[i]    = w.dir[i];
                f.lightColor[i]  = w.color[i];
                f.lightParams[i] = w.params[i];
            }
        }
        else
        {
            // clusterParams stays zero → the shader's window loop lights everything.
            const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), 8);
            f.lightCount    = glm::ivec4(count, 0, 0, 0);
            for (int i = 0; i < count; ++i)
            {
                const LightData& l = m_renderWorld.lights[i];
                f.lightPos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
                f.lightDir[i]    = glm::vec4(l.direction, l.spotAngleCos);
                f.lightColor[i]  = glm::vec4(l.color,     l.intensity);
                // y = the light's base layer in the local shadow atlas, -1 = no
                // local shadow (built-in convention, same as Metal/GL/D3D). The
                // layer assignment is deterministic per world state + camera, so
                // this extract's layers match the ones EncodeShadowMap rendered.
                f.lightParams[i] = glm::vec4(l.range,
                                             localShadows ? static_cast<float>(l.shadowLayer) : -1.0f,
                                             0.0f, 0.0f);
            }
        }
        // Local atlas constants from EncodeShadowMap (same reason as the cascades).
        for (int v = 0; v < kLocalShadowLayers; ++v) f.localShadowVP[v] = m_localShadowClip[v];
        f.localShadowParams = glm::vec4(1.0f / static_cast<float>(kLocalShadowSize),
                                        localShadows ? 1.0f : 0.0f, 0.0f, 0.0f);
        // Cascade constants from EncodeShadowMap's extract (the one the slices
        // were rendered with). Shadows are on only when that pass actually
        // ran this frame — the array holds nothing usable otherwise.
        for (int c = 0; c < kCsmCascades; ++c) f.cascadeVP[c] = m_cascadeClip[c];
        f.cascadeSplits = m_cascadeSplits;
        f.cameraFwd     = glm::vec4(m_cascadeCamFwd, 1.0f / float(std::max(m_shadowSize, 1u)));
        f.shadowBias    = glm::vec4(m_shadowSettings.slopeBias, m_shadowSettings.minBias, 0.0f, 0.0f);
        f.shadowEnabled = glm::ivec4(m_shadowRenderedThisFrame ? 1 : 0,
                                     m_debugShadowCascades ? 1 : 0, 0, 0);
        static_assert(sizeof(f.localShadowVP) / sizeof(f.localShadowVP[0]) == kLocalShadowLayers,
                      "Frame UBO local atlas array must match kLocalShadowLayers");
        static_assert(kLocalShadowLayers == ShadowData::kMaxLocalShadowLayers,
                      "Vulkan local atlas layer count must match the extractor's");
        f.sunDir        = glm::vec4(m_renderWorld.sunDirection, 0.0f);
        f.fog           = glm::vec4(m_environment.fogDensity, m_environment.fogHeightFalloff, 0.0f, 0.0f);
        // viewport.z == 1 iff runSSAO() completed this frame (blurRT holds valid data).
        // Using m_ssaoRanThisFrame (not just m_ssaoReady) ensures we never sample stale
        // blurRT in non-HDR paths or when SSAO was skipped due to an empty scene.
        f.viewport      = glm::vec4(float(width), float(height),
                                    m_ssaoRanThisFrame ? 1.0f : 0.0f, 0.0f);
        // giParams.y == 1 iff runGi() completed this frame (mask + atlases hold
        // valid data and the scene set's bindings 4-6 point at them).
        f.giGridOrigin  = glm::vec4(m_giGridOrigin, m_giProbeSpacing);
        f.giGridCounts  = glm::vec4(float(m_giGridCounts.x), float(m_giGridCounts.y),
                                    float(m_giGridCounts.z), float(m_giProbesPerRow));
        f.giParams      = glm::vec4(m_giIndirectIntensity, m_giRanThisFrame ? 1.0f : 0.0f, 0.0f, 0.0f);
        f.ambient       = glm::vec4(m_renderWorld.ambient, 0.0f); // GI-branch floor only
        // Forward SSR: the gate is a REAL trace result this frame, nothing weaker
        // — the same shape as m_ssaoRanThisFrame above.
        f.ssrParams     = glm::vec4(m_ssrRanThisFrame ? 1.0f : 0.0f, m_ssrIntensity,
                                    m_ssrMaxRoughness, 0.0f);
        if (m_frameUBO[m_currentFrame].mapped)
            std::memcpy(m_frameUBO[m_currentFrame].mapped, &f, sizeof(f));
    }

    // Point the scene set's binding 8 at this frame's reflection: the trace
    // result, or the transparent-black stand-in when no trace ran (this frame
    // slot's fence was waited on in Render(), so the set is rewritable). The
    // white AO fallback serves until the SSR pipelines have ever been built.
    {
        VkImageView ssrView = m_ssrRanThisFrame && m_ssrResultView ? m_ssrResultView
                            : (m_ssrBlackRT.view ? m_ssrBlackRT.view : m_ssaoWhiteView);
        if (ssrView)
        {
            VkDescriptorImageInfo sii{ m_ssrLinearSampler ? m_ssrLinearSampler : m_ssaoSampler,
                                       ssrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet sw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            sw.dstSet = m_frameUBO[m_currentFrame].set;
            sw.dstBinding = 8; sw.descriptorCount = 1;
            sw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sw.pImageInfo = &sii;
            vkUpdateDescriptorSets(m_device, 1, &sw, 0, nullptr);
        }
    }

    // A4: recycle this frame slot's material descriptor sets (the frame fence waited on
    // in Render() guarantees the GPU finished with them) + reset the per-frame ring cursor,
    // then fill the shared HeLighting UBO once — identical for every graph-material draw.
    // DrawScene runs at most once per frame (viewport OR swapchain, gated by useViewport),
    // so this reset happens exactly once per frame. Matches OpenGLRenderer's HeLighting fill.
    // DDGI for graph materials: ONE predicate opens heLitP's probe gate and picks
    // the real atlases for bindings 17/18 per draw below — the gate can never be
    // up while white sits there, nor an atlas be bound in a layout the gate does
    // not vouch for. runGi() ran earlier this frame (its step-6 write points the
    // scene set's 5/6 at the same images under the same condition).
    const bool matGiProbes = m_giRanThisFrame && m_giProbeGridBuilt
                          && m_giIrrAtlas.view && m_giVisAtlas.view;
    // The fill below, kept: the deferred resolve starts from exactly it (Thema 150).
    HE::MaterialShaderLibrary::Lighting frameMatLight{};
    if (m_matReady)
    {
        vkResetDescriptorPool(m_device, m_matPool[m_currentFrame], 0);
        m_matDrawCursor[m_currentFrame] = 0;
        m_matSetCache[m_currentFrame].clear();   // the sets it names went with the pool

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
        // shaders. Shared fill (HE::FillMaterialLightWindow); with the local
        // (point/spot) atlas rendered this frame, lightParams[i].y carries the
        // light's base layer + 1 (0 = "casts no local shadow").
        HE::FillMaterialLightWindow(m_renderWorld, lit, /*localShadowsActive=*/localShadows);
        // Thema 117: the cluster gate + grid for fragmentClustered's heLitP, from
        // the SAME build as the built-in shader. Only when that build ran: the
        // draw loop writes the SSBOs at bindings 24..26 for every set (contract
        // §2.1/2), and the window above stays full (§2.1/1) — a plain or baked
        // material shader reading this buffer still sees its eight lights.
        // giParams.xy (the viewport the cell pick divides by) is set below.
        if (clustered)
            HE::FillMaterialClusterParams(frameClusters, lit);
        HE::FillMaterialWind(GetEnvironment(), lit); // Wind / Wind Sway nodes, next to Time
        HE::FillMaterialWeather(GetEnvironment(), lit); // Weather node + the generic wet/snow response
        HE::FillMaterialWater(GetWaterRipples(), lit); // Water Ripple node: the rings the physics raised
        // Local atlas view-projs for heLocalShadowFactor (heLocalShadow, set 0
        // binding 13, bound per draw below). kVulkanClipFix is already in
        // m_localShadowClip and is the whole convention — same as csmVP.
        if (localShadows)
            for (int v = 0; v < m_localShadowLayerCount && v < kLocalShadowLayers; ++v)
                std::memcpy(lit.localShadowVP[v], &m_localShadowClip[v][0][0], 16 * sizeof(float));
        lit.giParams[0] = static_cast<float>(width);
        lit.giParams[1] = static_cast<float>(height);
        lit.giParams[2] = m_giRanThisFrame ? 1.0f : 0.0f;
        // DDGI probe field (Lighting v2.5) — the same grid the frame UBO hands
        // scene.frag's sampleDDGIIrradiance, so a graph material's indirect
        // diffuse matches a built-in neighbour instead of flat ambient while GI
        // is on (heGIIrradianceAt is its line-for-line twin).
        HE::FillMaterialGIProbe(lit, m_giGridOrigin, m_giProbeSpacing, m_giGridCounts,
                                m_giProbesPerRow, m_giIndirectIntensity, matGiProbes);
        // CSM fallback for graph materials (Lighting v2.2): only meaningful
        // when the GI masks are absent this frame — heLitP's directional
        // lights then sample the SAME cascade array as the built-in shader
        // (heCsm, binding 12, bound per draw below). kVulkanClipFix is already
        // in m_cascadeClip and is the whole convention here: scene.frag samples
        // uv = p.xy*0.5+0.5 with z in [0,1] under it, which is exactly what the
        // shared preamble's heCsmShadow() expects — no extra UV flip (unlike
        // the D3D/Metal fills, whose texture origin is top-left).
        if (!m_giRanThisFrame && m_shadowRenderedThisFrame)
        {
            const int nCascades = static_cast<int>(m_cascadeSplits.w);
            for (int c = 0; c < nCascades && c < kCsmCascades; ++c)
                std::memcpy(lit.csmVP[c], &m_cascadeClip[c][0][0], 16 * sizeof(float));
            lit.csmSplits[0] = m_cascadeSplits.x;
            lit.csmSplits[1] = m_cascadeSplits.y;
            lit.csmSplits[2] = m_cascadeSplits.z;
            lit.csmSplits[3] = m_cascadeSplits.w;
            lit.camFwd[0] = m_cascadeCamFwd.x;
            lit.camFwd[1] = m_cascadeCamFwd.y;
            lit.camFwd[2] = m_cascadeCamFwd.z;
            lit.shadowBias[0] = m_shadowSettings.slopeBias;
            lit.shadowBias[1] = m_shadowSettings.minBias;
        }
        // Forward SSR for heLitP's reflection cascade (heSSRFwd, set 0 binding
        // 31). x = the gate, y = intensity, z = max roughness; w is the DEFERRED
        // marker and stays zero — the forward path never zeroes ambSpec, and
        // this backend has no deferred composite at all.
        lit.ssr[0] = m_ssrRanThisFrame ? 1.0f : 0.0f;
        lit.ssr[1] = m_ssrIntensity;
        lit.ssr[2] = m_ssrMaxRoughness;
        lit.ssr[3] = 0.0f;
        // GI reflections for heLitP's GIRefl cascade (heGIReflFwd, binding 32):
        // x = intensity, y = max roughness, z = the gate (a real trace result
        // this frame, not just "enabled" — same shape as m_ssrRanThisFrame
        // above), w = the resolution-blur floor. Stays zero when the pass
        // never ran, so a graph material is byte-identical to the feature
        // never having existed (IRenderer.h's contract).
        lit.giRefl[0] = m_giReflIntensity;
        lit.giRefl[1] = m_giReflMaxRoughness;
        lit.giRefl[2] = m_giReflReady ? 1.0f : 0.0f;
        lit.giRefl[3] = 0.0f;
        // Height fog + the two image gates, the GL/Metal fill verbatim. z: the
        // baked sky cube is bound at binding 15 (image-based ambient, specular
        // IBL, fog colour). w: this frame's blurred SSAO is bound at binding 16 —
        // the exact gate the built-in shader's viewport.z uses, so a graph
        // material is occluded on precisely the frames a built-in one is.
        lit.fog[0] = m_environment.fogDensity;
        lit.fog[1] = m_environment.fogHeightFalloff;
        lit.fog[2] = m_skyEnvValid      ? 1.0f : 0.0f;
        lit.fog[3] = m_ssaoRanThisFrame ? 1.0f : 0.0f;
        lit.ambient[0] = m_renderWorld.ambient.r;
        lit.ambient[1] = m_renderWorld.ambient.g;
        lit.ambient[2] = m_renderWorld.ambient.b;
        lit.camPos[0] = m_renderWorld.camera.position.x;
        lit.camPos[1] = m_renderWorld.camera.position.y;
        lit.camPos[2] = m_renderWorld.camera.position.z;
        if (m_matLightBuf[m_currentFrame].mapped)
            std::memcpy(m_matLightBuf[m_currentFrame].mapped, &lit, sizeof(lit));
        frameMatLight = lit;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { width, height } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                            0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);

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
                                0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);

        const glm::vec3 camPos = m_renderWorld.camera.position;
        // Shared opaque/blended split + back-to-front order. The split now weighs
        // the per-instance tint alpha too (this backend used to test dc.opacity
        // alone, so a particle fading out via its tint was drawn opaque).
        // sectionAware: every draw in drawDCVk applies DrawCall::indexOffset/
        // indexCount (DrawIndexRange), so a multi-section mesh's per-slot DCs
        // all come through — one draw per material slot, as on GL, Metal, D3D11
        // and D3D12. Each slot now takes a k_matMaxDraws graph-material slot
        // and (instanced) a k_maxInstances run of its own, so both per-frame
        // caps are reached at fewer MESHES than before.
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        RenderSorter::partitionByOpacity(cmds.drawCalls(), opaqueDCs, transparentDCs,
                                         /*sectionAware=*/true);

        // ── Motion trails join the blended list ──────────────────────────────
        // A RibbonBatch is per-frame CPU geometry in the cooked vertex layout, so
        // it needs no pass and no pipeline of its own: upload it into this frame's
        // staging pool and append a perfectly ordinary DrawCall, which then takes
        // part in the sort below and goes through drawDCVk exactly like a
        // translucent mesh — including the graph-material path, which is what lets
        // the age in uv.v drive colour and fade (docs/rope-trail-plan.md §6.2).
        // Appended AFTER the partition on purpose: a trail is blended by nature,
        // never opaque, so it bypasses partitionByOpacity the way Metal and GL do.
        std::vector<DrawCall> ribbonDCs;
        std::vector<GpuMesh>  ribbonMeshes;
        std::unordered_map<const DrawCall*, const GpuMesh*> ribbonMeshByDC;
        if (!m_renderWorld.ribbonBatches.empty())
        {
            // Reserve BOTH before taking any address: transparentDCs below holds
            // bare pointers into ribbonDCs, and a growth reallocation kills them.
            ribbonDCs.reserve(m_renderWorld.ribbonBatches.size());
            ribbonMeshes.reserve(m_renderWorld.ribbonBatches.size());
            std::vector<float> rebased;
            for (size_t rbi = 0; rbi < m_renderWorld.ribbonBatches.size(); ++rbi)
            {
                const RibbonBatch& rb = m_renderWorld.ribbonBatches[rbi];
                if (rb.vertices.empty() || rb.indices.empty()) continue;
                const glm::vec3 pivot = rebaseRibbonVertices(rb, rebased);
                GpuMesh rmesh{};
                if (!uploadRibbon(m_currentFrame, rbi, rebased, rb.indices, rmesh)) continue;

                DrawCall dc{};
                dc.materialAssetId = rb.materialAssetId;
                dc.transform       = glm::mat4(1.0f);
                dc.transform[3]    = glm::vec4(pivot, 1.0f);   // pure translation, see rebaseRibbonVertices
                dc.entityId        = rb.entityId;
                dc.contributesAO   = false;   // trails never enter the AO prepass
                // receivesShadow stays TRUE: Metal and GL leave the shadow flag at its
                // default for a ribbon, and a trail that is shadowed on two backends
                // and unshadowed on three is exactly the kind of split this port exists
                // to avoid. Trails do not CAST a shadow (they are not in `objects`).
                if (const MaterialAsset* mat = (m_contentManager && rb.materialAssetId != HE::UUID{})
                        ? m_contentManager->getMaterial(rb.materialAssetId) : nullptr)
                {
                    dc.baseColor = { mat->baseColor[0], mat->baseColor[1], mat->baseColor[2] };
                    dc.metallic  = mat->metallic;
                    dc.roughness = mat->roughness;
                    dc.opacity   = mat->opacity;
                }
                else
                    dc.baseColor = glm::vec3(0.55f);   // material not loaded yet — flat grey, as Metal/GL
                // Force the blended class. RenderSorter::isTransparent decides both
                // which pass a draw belongs to and which graph-material pipeline
                // variant it gets, so a trail at opacity 1 would otherwise pick the
                // depth-WRITING variant inside the blended pass. This is the same
                // clamp ResolveMaterialParams applies to blendMode == 2 materials.
                dc.opacity = std::min(dc.opacity, 0.998f);

                ribbonMeshes.push_back(rmesh);
                ribbonDCs.push_back(std::move(dc));
                ribbonMeshByDC.emplace(&ribbonDCs.back(), &ribbonMeshes.back());
                transparentDCs.push_back(&ribbonDCs.back());
            }
        }

        RenderSorter::sortBackToFront(transparentDCs, camPos);

        // A3: real instancing applies to the opaque pass only (transparent keeps the
        // per-instance loop for blend + depth sort). allocInstances sub-allocates the
        // frame's instance arena across the frame's instanced batches — frame-wide,
        // because the instanced depth draws of the shadow pass, runGi and runSSAO
        // already hold the slots below and the GPU reads all of them only at submit.
        bool allowInstancing = true;
        // A4: the scene pipeline the CURRENT pass expects bound on entry to drawDCVk. A
        // graph-material draw binds its own per-material pipeline, so it restores THIS after
        // itself — otherwise the next built-in draw would inherit the material pipeline.
        // The opaque loop leaves it at the opaque scene pipe; the transparent loop sets it
        // to transPipe below.
        VkPipeline activeMatScenePipe = hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline;
        // Deferred (Thema 150): `gbufferPass` routes drawDCVk into m_gbufferRP —
        // graph materials through their G-buffer tail, built-in ones through
        // gbuffer.frag (activeMatScenePipe = m_gbufPipeline, which every restore
        // puts back). A graph material WITHOUT a usable G-buffer variant is
        // collected into deferredForwardDCs and replayed, lit, right after the
        // resolve — GL's `deferredForward`, Metal's `forwardOpaque`, D3D's replay.
        const bool deferred = m_deferredFrame && hdr;
        bool gbufferPass = false;
        std::vector<const DrawCall*> deferredForwardDCs;
        // resolveShaders / resolveGBufferShaders COPY the material's whole GLSL and hash it, and
        // foldDisplacement re-files the displacement body: ~27 us per draw, which a landscape of
        // 230 chunks on ONE material paid 230 times a frame (6 ms of CPU, perf capture
        // 2026-10-28). The material cannot change while this frame is recorded, so the first draw
        // of a material resolves it and every later draw of the frame reuses the result.
        struct MatResolved { bool ok = false; uint64_t hash = 0; std::string frag, vertBody; };
        std::unordered_map<HE::UUID, MatResolved> matFwdMemo, matGbMemo;
        auto drawDCVk = [&](const DrawCall& dc) {
            // A trail carries no mesh asset — its geometry sits in this frame's
            // ribbon pool, looked up by the DrawCall that was synthesised for it.
            const GpuMesh* mesh = nullptr;
            if (!ribbonMeshByDC.empty())
            {
                if (auto it = ribbonMeshByDC.find(&dc); it != ribbonMeshByDC.end())
                    mesh = it->second;
            }
            if (!mesh) mesh = resolveMesh(dc.meshAssetId);
            const GpuMesh& m    = mesh ? *mesh : m_cube;
            if (!m.indexCount) return;
            // Section draw → its own slice of the index buffer; whole-mesh draw
            // (every one-section mesh, every ribbon) → all of it. Both the
            // graph-material path and the built-in one below draw this range.
            const VulkanIndexRange range = DrawIndexRange(dc, m.indexCount);
            if (range.count == 0) return; // a slot clamped away on the fallback cube

            // A4: node-graph material? Render through a per-material pipeline built from the
            // MaterialShaderLibrary SPIR-V, bypassing the built-in PBR path entirely. Falls
            // through unchanged when the material has no graph shader OR resources are down.
            if (m_matReady && m_contentManager)
            {
                const MatResolved* matRes = nullptr;
                {
                    auto it = matFwdMemo.find(dc.materialAssetId);
                    if (it == matFwdMemo.end())
                    {
                        MatResolved r;
                        r.ok = m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId,
                                                             r.hash, r.frag, r.vertBody);
                        if (r.ok) foldDisplacement(dc.materialAssetId, r.hash);
                        it = matFwdMemo.emplace(dc.materialAssetId, std::move(r)).first;
                    }
                    matRes = &it->second;
                }
                if (matRes->ok)
                {
                    const uint64_t     matHash     = matRes->hash;
                    const std::string& matFrag     = matRes->frag;
                    const std::string& matVertBody = matRes->vertBody;
                    // Transparent graph materials get a blend-on / depth-write-off
                    // pipeline variant. MUST use the same predicate the opaque/blended
                    // partition above used, or a draw lands in the blended pass with a
                    // depth-writing pipeline (hence RenderSorter::isTransparent, tint
                    // alpha included, not a bare dc.opacity test).
                    const bool matTransp = RenderSorter::isTransparent(dc);
                    // The pak's baked SPIR-V when the export carried one, else cross-compile
                    // now. resolveShaders just fetched this material, so the pointer is
                    // fresh; it is consumed before anything can load.
                    const MaterialShaderVariant* matPre = HE::MaterialShaderLibrary::precompiledFor(
                        m_contentManager->getMaterial(dc.materialAssetId), HE::RendererBackend::Vulkan);
                    VkPipeline matPipe = VK_NULL_HANDLE;
                    // A GeometryPass batch in the opaque or G-buffer pass draws ONCE with
                    // the material's instanced twin (Thema 182, 2f; GL's and Metal's 2e):
                    // built only for batches, null (baked pak, HE_GRAPH_INSTANCING=0,
                    // build failed) sends it back to the per-instance loop.
                    const bool wantTwin = dc.instanceTotal() > 1 && allowInstancing && !matTransp
                                       && m_instCur.mapped
                                       && RenderSorter::graphInstancingEnabled();
                    VkPipeline matInstPipe = VK_NULL_HANDLE;
                    if (gbufferPass)
                    {
                        // The G-buffer tail of the same graph (same expressions,
                        // attributes instead of heLitP). No variant, or one that
                        // does not build → the lit forward replay after the resolve.
                        auto gbIt = matGbMemo.find(dc.materialAssetId);
                        if (gbIt == matGbMemo.end())
                        {
                            MatResolved r;
                            r.ok = m_matShaderLib.resolveGBufferShaders(*m_contentManager, dc.materialAssetId,
                                                                        r.hash, r.frag, r.vertBody);
                            if (r.ok) foldDisplacement(dc.materialAssetId, r.hash);
                            gbIt = matGbMemo.emplace(dc.materialAssetId, std::move(r)).first;
                        }
                        const bool         gbOk       = gbIt->second.ok;
                        const uint64_t     gbHash     = gbIt->second.hash;
                        const std::string& gbFrag     = gbIt->second.frag;
                        const std::string& gbVertBody = gbIt->second.vertBody;
                        if (gbOk)
                        {
                            matPipe = GetOrBuildMaterialPipeline(gbHash, gbFrag, gbVertBody, nullptr,
                                                                 true, false, /*gbuffer=*/true);
                        }
                        if (matPipe == VK_NULL_HANDLE)
                        {
                            deferredForwardDCs.push_back(&dc);
                            return;
                        }
                        if (wantTwin)
                            matInstPipe = GetOrBuildMaterialPipeline(gbHash, gbFrag, gbVertBody, nullptr,
                                                                     true, false, /*gbuffer=*/true,
                                                                     /*instanced=*/true);
                    }
                    else
                    {
                        matPipe = GetOrBuildMaterialPipeline(matHash, matFrag, matVertBody,
                                                             matPre, hdr, matTransp);
                        if (matPipe != VK_NULL_HANDLE && wantTwin)
                            matInstPipe = GetOrBuildMaterialPipeline(matHash, matFrag, matVertBody,
                                                                     matPre, hdr, false,
                                                                     /*gbuffer=*/false, /*instanced=*/true);
                    }
                    uint32_t& cursor = m_matDrawCursor[m_currentFrame];
                    // A full ring is a silent optics change (built-in look, or a skipped
                    // instance tail) — say so once per session instead of never.
                    auto noteRingFull = [&]() {
                        if (m_matRingWarned) return;
                        m_matRingWarned = true;
                        HE_LOG_WARN(RHI, "%s",
                            ("VulkanRenderer: more than " + std::to_string(k_matMaxDraws)
                             + " graph-material draws in one frame — the surplus renders built-in "
                               "PBR (or is skipped inside an instanced batch); raise k_matMaxDraws").c_str());
                    };
                    if (matPipe != VK_NULL_HANDLE && cursor >= k_matMaxDraws) noteRingFull();
                    if (matPipe != VK_NULL_HANDLE && cursor < k_matMaxDraws)
                    {
                        // Resolve the same PBR scalars / has-texture flag the built-in path uses.
                        bool matTextured = (m.albedoSet != VK_NULL_HANDLE);
                        const MaterialTexVk* matOvr = nullptr;
                        if (resolveMaterialOverride(dc.materialAssetId, matOvr))
                            matTextured = (matOvr->set != VK_NULL_HANDLE);

                        // heTexP0..3 = the graph's project textures (Texture Sample nodes),
                        // the white default where a slot is empty or unloadable. The slot
                        // list is snapshotted BEFORE any resolve: a resolve may load, and a
                        // load can move the material asset out from under a held pointer —
                        // which is also why `ma` below is fetched only AFTER this block.
                        //
                        // A sampler2DArray slot (Texture Array Sample, Thema 158) needs a
                        // 2D_ARRAY view -- the white ARRAY view when empty -- or validation
                        // flags the view type at the draw.
                        VkImageView heTexP[HE::kMatMaxGraphTextures] = {
                            m_whiteAlbedoView, m_whiteAlbedoView, m_whiteAlbedoView, m_whiteAlbedoView };
                        {
                            HE::UUID    gIds[HE::kMatMaxGraphTextures]{};
                            std::string gPaths[HE::kMatMaxGraphTextures];
                            size_t nTex = 0;
                            uint32_t arrMask = 0;
                            if (const MaterialAsset* ma0 = m_contentManager->getMaterial(dc.materialAssetId))
                            {
                                arrMask = HE::matGlslTextureArrayMask(ma0->customShaderFragGlsl);
                                for (int i = 0; i < HE::kMatMaxGraphTextures; ++i)
                                    if ((arrMask >> i) & 1u) heTexP[i] = m_whiteArrayView;
                                nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
                                    std::max(ma0->graphTexturePaths.size(), ma0->graphTextureIds.size()));
                                for (size_t i = 0; i < nTex; ++i)
                                {
                                    if (i < ma0->graphTextureIds.size())   gIds[i]   = ma0->graphTextureIds[i];
                                    if (i < ma0->graphTexturePaths.size()) gPaths[i] = ma0->graphTexturePaths[i];
                                }
                            }
                            for (size_t i = 0; i < nTex; ++i)
                                if (VkImageView v = resolveGraphTexture(gIds[i], gPaths[i], (arrMask >> i) & 1u))
                                    heTexP[i] = v;
                        }
                        // heLandscapeWeights (binding 14) = the object's landscape weightmap,
                        // PER DRAW like D3D11 t14 / GL unit 13 / Metal slot 13: it belongs to
                        // the terrain the chunk is part of, not to the material. Anything that
                        // is not a landscape chunk gets the 1x1 (1,0,0,0) default, so a layer
                        // blend resolves to layer 0 instead of black; white is the last
                        // resort. Resolved before `ma` below for the same reason as heTexP.
                        VkImageView heWeights = VK_NULL_HANDLE;
                        if (dc.weightmapTextureId != HE::UUID{})
                            heWeights = resolveGraphTexture(dc.weightmapTextureId, {});
                        if (!heWeights)
                            heWeights = resolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {});
                        if (!heWeights) heWeights = m_whiteAlbedoView;

                        // Per-entity HeParams override wins over the material's shared params.
                        const MaterialAsset* ma = m_contentManager->getMaterial(dc.materialAssetId);
                        const std::vector<float>* params =
                            !dc.paramOverride.empty() ? &dc.paramOverride
                            : (ma && !ma->shaderParamData.empty() ? &ma->shaderParamData : nullptr);

                        VkDeviceSize voff = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbuf, &voff);
                        vkCmdBindIndexBuffer(cmd, m.ibuf, 0, VK_INDEX_TYPE_UINT32);

                        // One U/HeParams ring slot and its descriptor set, bound with
                        // `pipe`: the per-draw half of every material draw. false =
                        // the ring is full (noted once) or the pool ran dry.
                        auto bindMatSlot = [&](VkPipeline pipe, const glm::mat4& mvp, const glm::mat4& model) -> bool {
                            if (cursor >= k_matMaxDraws) { noteRingFull(); return false; }
                            const uint32_t i = cursor++;

                            // std140 U block (176 B) into ring slot i (256-B stride).
                            struct UBlock { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };
                            static_assert(sizeof(UBlock) == 176, "U block must be std140 176 B");
                            UBlock ub;
                            ub.mvp   = mvp;
                            ub.model = model;
                            // Per-instance tint (RenderObject::instanceTint), as Metal and GL
                            // multiply it in: a graph reads it through Vertex Color (the editor
                            // icons wear their light's colour that way). Identity for the rest.
                            ub.color = glm::vec4(dc.baseColor * glm::vec3(dc.instanceTint), 1.0f);
                            ub.flags = glm::vec4(matTextured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                            ub.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity * dc.instanceTint.a, 0.0f);
                            std::memcpy(static_cast<uint8_t*>(m_matObjBuf[m_currentFrame].mapped)
                                        + static_cast<size_t>(i) * k_matSlotStride, &ub, sizeof(ub));

                            // HeParams (16 vec4 = 64 floats = 256 B) into ring slot i, zero-padded.
                            float padded[64] = { 0.0f };
                            if (params)
                                std::memcpy(padded, params->data(),
                                            std::min(params->size(), size_t(64)) * sizeof(float));
                            std::memcpy(static_cast<uint8_t*>(m_matParBuf[m_currentFrame].mapped)
                                        + static_cast<size_t>(i) * k_matSlotStride, padded, sizeof(padded));

                            // The descriptor set is shared by every draw whose bindings are identical
                            // (found by hashing the writes below); the ring slot reaches the shader as
                            // the DYNAMIC offset of bindings 1, 3 and 9 instead of a per-draw buffer
                            // range. A landscape of 230 chunks on one material needs one set, not 230.
                            VkDescriptorSet set = VK_NULL_HANDLE;

                            const VkDeviceSize slot = static_cast<VkDeviceSize>(i) * k_matSlotStride;
                            VkDescriptorBufferInfo lightBI{ m_matLightBuf[m_currentFrame].buf, 0,
                                                            sizeof(HE::MaterialShaderLibrary::Lighting) };
                            VkDescriptorBufferInfo objBI  { m_matObjBuf[m_currentFrame].buf, 0, 176 };
                            VkDescriptorBufferInfo parBI  { m_matParBuf[m_currentFrame].buf, 0, 256 };
                            // heTex0 = the material's base texture: an override material's texture
                            // wins (A2), else the mesh's baked texture (A1), else the white default —
                            // matching the built-in path's texture selection + matTextured flag.
                            VkImageView tex0View = m_whiteAlbedoView;
                            if (matOvr) { if (matOvr->view) tex0View = matOvr->view; }
                            else if (m.albedoView) tex0View = m.albedoView;
                            VkDescriptorImageInfo tex0II{ m_albedoSampler, tex0View,
                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            // heTexP0..3 (bindings 4..7): the graph's project textures resolved
                            // above, sampled linear-wrap like heTex0 (the white default where
                            // a slot is empty).
                            static_assert(HE::kMatMaxGraphTextures == 4, "heTexP0..3 occupy bindings 4..7");
                            VkDescriptorImageInfo texPII[HE::kMatMaxGraphTextures];
                            for (int k = 0; k < HE::kMatMaxGraphTextures; ++k)
                                texPII[k] = { m_albedoSampler, heTexP[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            VkWriteDescriptorSet w[HE::vkmat::kBindingCount]{};
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
                            wr(1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &objBI,   nullptr);
                            wr(2, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &tex0II);
                            wr(3, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &parBI,   nullptr);
                            wr(4, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &texPII[0]);
                            wr(5, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &texPII[1]);
                            wr(6, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &texPII[2]);
                            wr(7, 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr,  &texPII[3]);
                            wr(8, 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         &lightBI, nullptr); // WPO VS
                            wr(9, 9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &parBI,   nullptr); // WPO VS
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
                            // heCsm (binding 12, sampler2DArray): the cascade shadow array
                            // the built-in scene shader samples at binding 1, through the
                            // same nearest/clamp sampler (PCF taps compare single texels).
                            // The 1-layer white ARRAY view stands in while the array does
                            // not exist — csmSplits.w is 0 then, and the view DIMENSION
                            // must still match the arrayed sampler in the SPIR-V.
                            VkDescriptorImageInfo csmII{
                                (m_shadowView && m_shadowLayoutValid) ? m_shadowSampler : m_albedoSampler,
                                (m_shadowView && m_shadowLayoutValid) ? m_shadowView    : m_whiteArrayView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(12, 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &csmII);
                            // heSSRFwd (binding 31): the SAME image the built-in
                            // shaders read at scene binding 8, so a graph
                            // material and a built-in one next to each other
                            // reflect the same trace. Transparent black when no
                            // trace ran — heLight.ssr.x is 0 then anyway.
                            VkDescriptorImageInfo ssrII{
                                m_ssrLinearSampler ? m_ssrLinearSampler : m_albedoSampler,
                                (m_ssrRanThisFrame && m_ssrResultView) ? m_ssrResultView
                                    : (m_ssrBlackRT.view ? m_ssrBlackRT.view : m_whiteAlbedoView),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(13, 31, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &ssrII);
                            // heLocalShadow (binding 13, sampler2DArray): the local
                            // (point/spot) atlas, same nearest/clamp sampler as heCsm.
                            // The white ARRAY view stands in until the atlas was
                            // rendered — lightParams[i].y is 0 then (never sampled).
                            VkDescriptorImageInfo localII{
                                localShadows ? m_shadowSampler   : m_albedoSampler,
                                localShadows ? m_localShadowView : m_whiteArrayView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(14, 13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &localII);
                            // heSkyEnv (15): the baked sky cube once updateSkyEnvCube
                            // filled it (fog.z = 1 exactly then), the white cube before.
                            // Linear-CLAMP like GL/Metal's cube sampler; the repeat
                            // address mode of m_albedoSampler is moot on a cube, but the
                            // SSAO sampler is the clamp one this backend already has.
                            VkDescriptorImageInfo skyII{
                                m_skyEnvValid ? (m_ssaoSampler ? m_ssaoSampler : m_albedoSampler) : m_albedoSampler,
                                m_skyEnvValid ? m_skyEnvView : m_whiteCubeView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(15, 15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &skyII);
                            // heAO (16): this frame's blurred SSAO — the image the
                            // built-in shader reads at scene binding 3, parked in
                            // SHADER_READ_ONLY by the blur pass's finalLayout. White
                            // otherwise; fog.w is 0 then. heLitP texelFetches it, so the
                            // sampler is only there because the descriptor is combined.
                            VkDescriptorImageInfo whiteII{ m_albedoSampler, m_whiteAlbedoView,
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            VkDescriptorImageInfo aoII{
                                m_ssaoRanThisFrame ? m_ssaoSampler      : m_albedoSampler,
                                m_ssaoRanThisFrame ? m_ssaoBlurRT.view  : m_whiteAlbedoView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(16, 16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &aoII);
                            // heGIIrradiance / heGIVisibility (17/18): the probe atlases
                            // under matGiProbes (the gate FillMaterialGIProbe raised), in
                            // GENERAL like everywhere else they are sampled; white
                            // otherwise. Linear-CLAMP like the scene set's 5/6 — a
                            // repeating sampler would wrap edge taps across the atlas.
                            const VkSampler giAtlasSampler = m_ssaoSampler ? m_ssaoSampler : m_albedoSampler;
                            VkDescriptorImageInfo giIrrII{ giAtlasSampler,
                                matGiProbes ? m_giIrrAtlas.view : m_whiteAlbedoView,
                                matGiProbes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            VkDescriptorImageInfo giVisII{ giAtlasSampler,
                                matGiProbes ? m_giVisAtlas.view : m_whiteAlbedoView,
                                matGiProbes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(17, 17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &giIrrII);
                            wr(18, 18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &giVisII);
                            // heGIReflFwd (32): this frame's reflection result (frameMatLight
                            // carries giRefl.z from the same m_giReflReady), white otherwise —
                            // mirrors heSSRFwd's binding-8 pattern above. heCloudShadow (33):
                            // white, its gate (cloudShadowB.x) stays 0.
                            VkDescriptorImageInfo giReflII{ giAtlasSampler,
                                m_giReflReady ? m_giReflFinal.view : m_whiteAlbedoView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            wr(19, 32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &giReflII);
                            wr(20, 33, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &whiteII);
                            // fragmentClustered's light lists (bindings 24..26, Thema
                            // 117): this frame slot's cluster SSBOs, the very buffers
                            // the built-in shader reads at scene bindings 10..12. Written
                            // whenever they exist — also with the gate at 0 (no lights,
                            // HE_FORWARD_CLUSTER=0): a clustered pipeline statically uses
                            // them, so its set must be complete. Without them every
                            // pipeline is the plain variant, which uses none of the three.
                            // heLandscapeWeights (14): the weightmap resolved above,
                            // linear-CLAMP like D3D11's s0 (a [0,1] weight field; a
                            // repeating sampler would bleed the far edge's paint in).
                            // Only when the layout has the row (m_matLandscapeWeights).
                            static_assert(HE::vkmat::kPreClusterBindingCount == 22,
                                          "fixed writes w[0..20] + w[21] heLandscapeWeights, one per non-cluster row");
                            uint32_t nWrites = HE::vkmat::kPreClusterBindingCount - 1;
                            VkDescriptorImageInfo weightsII{ m_ssaoSampler ? m_ssaoSampler : m_albedoSampler,
                                                             heWeights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                            if (m_matLandscapeWeights)
                                wr(static_cast<int>(nWrites++), HE::vkmat::kLandscapeWeightsBinding,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &weightsII);
                            VkDescriptorBufferInfo clusterBI[3]{};
                            if (m_clusterReady)
                            {
                                const ClusterBuffer* cbs[3] = { &m_clusterLights[m_currentFrame],
                                                                &m_clusterGrid[m_currentFrame],
                                                                &m_clusterIdx[m_currentFrame] };
                                const uint32_t cbind[3] = { HE::vkmat::kClusterLightsBinding,
                                                            HE::vkmat::kClusterGridBinding,
                                                            HE::vkmat::kClusterIdxBinding };
                                for (uint32_t c = 0; c < 3; ++c)
                                {
                                    clusterBI[c] = { cbs[c]->buf, 0, VK_WHOLE_SIZE };
                                    wr(static_cast<int>(nWrites++), cbind[c],
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &clusterBI[c], nullptr);
                                }
                            }
                            // Key = everything the writes name (FNV-1a over binding, type and the
                            // handles/ranges), so a changed texture, AO/GI view or cluster buffer gets
                            // its own set and nothing can be bound stale.
                            uint64_t key = 0xcbf29ce484222325ULL;
                            auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001b3ULL; };
                            for (uint32_t k = 0; k < nWrites; ++k)
                            {
                                mix(w[k].dstBinding);
                                mix(static_cast<uint64_t>(w[k].descriptorType));
                                if (w[k].pImageInfo)
                                {
                                    mix(reinterpret_cast<uint64_t>(w[k].pImageInfo->sampler));
                                    mix(reinterpret_cast<uint64_t>(w[k].pImageInfo->imageView));
                                    mix(static_cast<uint64_t>(w[k].pImageInfo->imageLayout));
                                }
                                if (w[k].pBufferInfo)
                                {
                                    mix(reinterpret_cast<uint64_t>(w[k].pBufferInfo->buffer));
                                    mix(w[k].pBufferInfo->offset);
                                    mix(w[k].pBufferInfo->range);
                                }
                            }
                            auto& setCache = m_matSetCache[m_currentFrame];
                            if (auto hit = setCache.find(key); hit != setCache.end())
                                set = hit->second;
                            else
                            {
                                VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                                dsai.descriptorPool     = m_matPool[m_currentFrame];
                                dsai.descriptorSetCount = 1;
                                dsai.pSetLayouts        = &m_matSetLayout;
                                if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) return false;
                                for (uint32_t k = 0; k < nWrites; ++k) w[k].dstSet = set;
                                vkUpdateDescriptorSets(m_device, nWrites, w, 0, nullptr);
                                setCache.emplace(key, set);
                            }

                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                            // Dynamic offsets in ascending binding order: 1 (U), 3 (HeParams), 9 (HeParams, WPO VS).
                            const uint32_t dynOffsets[HE::vkmat::kDynamicOffsetCount] = {
                                static_cast<uint32_t>(slot), static_cast<uint32_t>(slot), static_cast<uint32_t>(slot) };
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                    m_matPipelineLayout, 0, 1, &set,
                                                    HE::vkmat::kDynamicOffsetCount, dynOffsets);
                            return true;
                        };
                        auto drawMatInstance = [&](const glm::mat4& model) {
                            if (!bindMatSlot(matPipe, viewProj * model, model)) return;
                            vkCmdDrawIndexed(cmd, range.count, 1, range.start, 0, 0);
                            ++m_statDraws;
                            m_statTris += range.count / 3;
                        };
                        const uint32_t instCount = static_cast<uint32_t>(dc.instanceTotal());
                        VkBuffer ibuf = VK_NULL_HANDLE; VkDeviceSize instOff = 0; uint8_t* dst = nullptr;
                        if (matInstPipe != VK_NULL_HANDLE && allocInstances(instCount, ibuf, instOff, dst))
                        {
                            // The batch: every model into the model half of its slot in the
                            // frame's instance arena, ONE U slot + descriptor set with the
                            // batch-constant half (U.mvp = viewProj, the twin multiplies the
                            // model in), one instanced draw.
                            if (bindMatSlot(matInstPipe, viewProj, glm::mat4(1.0f)))
                            {
                                RenderSorter::copyInstanceModels(dst + sizeof(glm::mat4), k_instStride,
                                                                 dc.instanceRun());
                                vkCmdBindVertexBuffers(cmd, 1, 1, &ibuf, &instOff);
                                vkCmdDrawIndexed(cmd, range.count, instCount, range.start, 0, 0);
                                ++m_statDraws;
                                m_statTris += (range.count / 3) * instCount;
                            }
                        }
                        else if (dc.instanceTotal())
                            // No twin (or the instance buffer is full): each instance through
                            // the material path, one ring slot apiece.
                            dc.forEachInstance(drawMatInstance);
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
                                                &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
                        return;
                    }
                }
            }
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

            // This draw's material block: its own slot in the frame's material ring,
            // selected by set 0's dynamic offset (no transfer inside the render pass).
            {
                // The colour the built-in shader gets. Forward keeps this
                // backend's own rule (the draw's baseColor; a texture replaces
                // it in scene.frag). The G-buffer takes GL's and Metal's
                // (kGBufFS / gbufferMain, as D3D11/D3D12 do since Thema 150):
                // no material → 0.55 grey, or 1.0 under a texture gbuffer.frag
                // multiplies in; the instance tint folded in.
                glm::vec3 drawBase = dc.baseColor;
                if (gbufferPass)
                {
                    const bool hasMat = m_contentManager && dc.materialAssetId != HE::UUID{}
                                     && m_contentManager->getMaterial(dc.materialAssetId);
                    if (!hasMat) drawBase = textured ? glm::vec3(1.0f) : glm::vec3(0.55f);
                    drawBase *= glm::vec3(dc.instanceTint);
                }
                const SceneMatData md{
                    drawBase.r, drawBase.g, drawBase.b, dc.metallic,
                    dc.roughness, dc.opacity, textured ? 1.0f : 0.0f,
                    dc.receivesShadow ? 0.0f : 1.0f
                };
                pushSceneMaterial(&md, sizeof(md));
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                        0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
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
                vkCmdDrawIndexed(cmd, range.count, 1, range.start, 0, 0);
                ++m_statDraws;
                m_statTris += range.count / 3;
            };
            if (dc.instanceTotal())
            {
                // A3: real instancing — fill the per-frame instance buffer with every
                // instance's {mvp,model}, bind the instanced pipeline + binding 1, ONE draw.
                static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be mvp+model");
                const uint32_t count = static_cast<uint32_t>(dc.instanceTotal());
                // Match the instanced pipeline to the active scene target (= what the
                // non-instanced bind resolves to). Null → fits=false → per-instance fallback
                // (never bind an LDR pipeline to the HDR render pass).
                VkPipeline instPipe = gbufferPass ? m_gbufInstancedPipeline      // 4 targets (Thema 150)
                                    : (hdr && m_scenePipelineHDR)
                                    ? m_sceneInstancedPipelineHDR : m_sceneInstancedPipeline;
                VkBuffer ibuf = VK_NULL_HANDLE; VkDeviceSize instOff = 0; uint8_t* dst = nullptr;
                const bool fits = allowInstancing && instPipe && allocInstances(count, ibuf, instOff, dst);
                if (fits)
                {
                    RenderSorter::fillInstancePairs(dst, dc.instanceRun(),
                        [&](const glm::mat4& t, glm::mat4* pair) {
                            pair[0] = viewProj * t;   // mvp, model (column-major)
                            pair[1] = t;
                        });
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, instPipe);
                    vkCmdBindVertexBuffers(cmd, 1, 1, &ibuf, &instOff);
                    vkCmdDrawIndexed(cmd, range.count, count, range.start, 0, 0);
                    // Restore the pass's non-instanced pipeline for subsequent draws
                    // (instancing runs in the opaque and G-buffer passes only —
                    // activeMatScenePipe is the one of the two).
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
                    ++m_statDraws;
                    m_statTris += (range.count / 3) * count;
                }
                else
                {
                    dc.forEachInstance(drawOne); // fallback
                }
            }
            else
                drawOne(dc.transform);
        };

        // Decals into GB0 when this frame is deferred and the variant built;
        // otherwise the forward decal over the lit colour, as before.
        const bool gbDecals = deferred && !m_renderWorld.decals.empty() && EnsureDecalPipelines()
                           && m_decalPipelineGB && m_gbDecalFB;
        if (!deferred)
        {
            GpuPassScope _opaqueTimer(this, "Opaque");
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
            for (const DrawCall* dc : opaqueDCs) drawDCVk(*dc);
        }
        else
        {
            // ── Deferred (Thema 150): G-buffer pass → lighting resolve → forward
            // replay. The sky is in the HDR target already (drawn first in
            // m_postFxSceneRP, no depth test); that pass ends here and its
            // colour survives into m_hdrLoadRP, where the resolve discards
            // d >= 1. Nothing between the two passes binds or clears it.
            vkCmdEndRenderPass(cmd);

            // G-buffer pass: four targets + the viewport depth, cleared like GL's
            // (GB1 an encoded +Z normal with mid rough/spec, GB3 the far plane so
            // the resolve discards untouched pixels).
            VkClearValue gbClears[k_gbTargets + 1]{};
            gbClears[1].color        = { { 0.5f, 0.5f, 1.0f, 0.5f } };
            gbClears[3].color        = { { 1.0f, 0.0f, 0.0f, 0.0f } };
            gbClears[4].depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo gbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            gbi.renderPass        = m_gbufferRP;
            gbi.framebuffer       = m_gbFB;
            gbi.renderArea.extent = { width, height };
            gbi.clearValueCount   = k_gbTargets + 1;
            gbi.pClearValues      = gbClears;
            GpuPassScope _gbufTimer(this, "GBuffer");
            vkCmdBeginRenderPass(cmd, &gbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            activeMatScenePipe = m_gbufPipeline;
            gbufferPass        = true;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gbufPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
            for (const DrawCall* dc : opaqueDCs) drawDCVk(*dc);
            gbufferPass        = false;
            activeMatScenePipe = hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline;
            vkCmdEndRenderPass(cmd); // GB0..3 → SHADER_READ_ONLY (finalLayout)
            _gbufTimer.next("Resolve");   // decals, the lighting resolve and the forward replay of the deferred branch

            // ── Decals into GB0 (step 5): after the geometry, before the resolve
            // reads the attributes — GL's slot. The decal colour becomes base
            // colour and is lit like everything else; the forward tail below
            // skips its self-lit decal. m_gbDecalRP's own dependencies order it
            // between the G-buffer pass and the resolve.
            if (gbDecals)
            {
                VkRenderPassBeginInfo dbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                dbi.renderPass        = m_gbDecalRP;
                dbi.framebuffer       = m_gbDecalFB;
                dbi.renderArea.extent = { width, height };
                vkCmdBeginRenderPass(cmd, &dbi, VK_SUBPASS_CONTENTS_INLINE);
                EncodeDecals(cmd, m_gbView[3], width, height, hdr, /*intoGBuffer=*/true);
                vkCmdEndRenderPass(cmd);
            }

            // The sky's colour writes (pass 1) and the G-buffer pass's depth
            // writes must be visible to m_hdrLoadRP's LOADs, its depth test and
            // the resolve's samples. m_hdrLoadRP's own dependency cannot say so
            // (it must equal m_postFxSceneRP's, see createDeferredPipelines), so
            // one global memory barrier between the passes does.
            {
                VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                 | VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 1, &mb, 0, nullptr, 0, nullptr);
            }

            // The HDR target again, colour (sky) and depth (G-buffer) loaded.
            VkRenderPassBeginInfo hbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            hbi.renderPass        = m_hdrLoadRP;
            hbi.framebuffer       = m_hdrFB;
            hbi.renderArea.extent = { width, height };
            vkCmdBeginRenderPass(cmd, &hbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            // ── Lighting resolve: fullscreen, into the HDR target.
            {
                using MSL = HE::MaterialShaderLibrary;
                const uint32_t f = m_currentFrame;
                // The resolve's OWN lighting block: the frame's graph-material
                // fill, specular AA off (its "normal" is a G-buffer texel whose
                // derivative jumps at every silhouette — plan §10.7 #1), and in
                // the clustered variant a directional-only window (heLitP walks
                // the whole window; the point/spot lights come from the lists).
                MSL::Lighting rl = frameMatLight;
                rl.specAA[1] = 0.0f;
                MSL::ResolveUniforms ru;
                // World-pos reconstruction from the depth this frame rasterised
                // with: the JITTERED viewProj (§10.7 #3) — kVulkanClipFix included,
                // so its inverse maps Vulkan NDC straight back. The decal pass's
                // convention: gl_FragCoord.y counts down and so does Vulkan's NDC
                // y (+1), depth already 0..1 (scale 1, bias 0).
                const glm::mat4 invViewProj = glm::inverse(viewProj);
                std::memcpy(ru.invViewProj, &invViewProj[0][0], 16 * sizeof(float));
                ru.depthParams[0] = 1.0f;
                ru.depthParams[1] = 1.0f;
                ru.depthParams[2] = 0.0f;
                ru.depthParams[3] = static_cast<float>(HE::viewModeGBufferIndex(m_viewMode));
                VkPipeline rpipe = m_resolvePipe;
                if (clustered && m_resolveClusteredPipe)
                {
                    // The lists at 24..26 are this slot's cluster SSBOs, filled
                    // above from the same build (with this frame's GI decision).
                    rpipe = m_resolveClusteredPipe;
                    HE::FillMaterialDirectionalWindow(m_renderWorld, rl);
                    for (int c = 0; c < 4; ++c)
                    {
                        ru.clusterParams[c] = frameClusters.params[c];
                        ru.clusterCamFwd[c] = frameClusters.camFwd[c];
                    }
                }
                std::memcpy(m_resolveLightBuf[f].mapped, &rl, sizeof(rl));
                std::memcpy(m_resolveUboBuf[f].mapped,   &ru, sizeof(ru));

                // The set: the material inputs exactly as a graph-material draw
                // binds them (same views, samplers, layouts and gates), then the
                // four G-buffer targets and HeResolve. This slot's fence was
                // waited on in Render(), so the set is free to rewrite.
                VkDescriptorBufferInfo lightBI{ m_resolveLightBuf[f].buf, 0, sizeof(MSL::Lighting) };
                VkDescriptorBufferInfo uboBI  { m_resolveUboBuf[f].buf,   0, sizeof(MSL::ResolveUniforms) };
                const VkSampler clampSampler = m_ssaoSampler ? m_ssaoSampler : m_albedoSampler;
                const VkDescriptorImageInfo whiteII{ m_albedoSampler, m_whiteAlbedoView,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo giSunII{ m_albedoSampler,
                    (m_giRanThisFrame && m_giResult.view) ? m_giResult.view : m_whiteAlbedoView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo giLocalII{ m_albedoSampler,
                    (m_giRanThisFrame && m_giLocalMask.view) ? m_giLocalMask.view : m_whiteAlbedoView,
                    (m_giRanThisFrame && m_giLocalMask.view) ? VK_IMAGE_LAYOUT_GENERAL
                                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo csmII{
                    (m_shadowView && m_shadowLayoutValid) ? m_shadowSampler : m_albedoSampler,
                    (m_shadowView && m_shadowLayoutValid) ? m_shadowView    : m_whiteArrayView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo localII{
                    localShadows ? m_shadowSampler   : m_albedoSampler,
                    localShadows ? m_localShadowView : m_whiteArrayView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo skyII{
                    m_skyEnvValid ? clampSampler  : m_albedoSampler,
                    m_skyEnvValid ? m_skyEnvView  : m_whiteCubeView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo aoII{
                    m_ssaoRanThisFrame ? m_ssaoSampler     : m_albedoSampler,
                    m_ssaoRanThisFrame ? m_ssaoBlurRT.view : m_whiteAlbedoView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo giIrrII{ clampSampler,
                    matGiProbes ? m_giIrrAtlas.view : m_whiteAlbedoView,
                    matGiProbes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorImageInfo giVisII{ clampSampler,
                    matGiProbes ? m_giVisAtlas.view : m_whiteAlbedoView,
                    matGiProbes ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                // heSSRFwd: this frame's forward trace, exactly what a graph-
                // material draw binds (frameMatLight carries ssr.x from the same
                // m_ssrRanThisFrame); transparent black when no trace ran.
                const VkDescriptorImageInfo ssrII{
                    m_ssrLinearSampler ? m_ssrLinearSampler : m_albedoSampler,
                    (m_ssrRanThisFrame && m_ssrResultView) ? m_ssrResultView
                        : (m_ssrBlackRT.view ? m_ssrBlackRT.view : m_whiteAlbedoView),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                // heGIReflFwd: this frame's reflection result, the deferred-resolve
                // twin of the forward per-draw binding above.
                const VkDescriptorImageInfo giReflII{ clampSampler,
                    m_giReflReady ? m_giReflFinal.view : m_whiteAlbedoView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkDescriptorImageInfo gbII[k_gbTargets];
                for (uint32_t i = 0; i < k_gbTargets; ++i)
                    gbII[i] = { m_gbSampler, m_gbView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkDescriptorBufferInfo clusterBI[3]{};

                VkWriteDescriptorSet w[HE::vkmat::kResolveBindingCount]{};
                uint32_t n = 0;
                auto wr = [&](uint32_t binding, VkDescriptorType type,
                              const VkDescriptorBufferInfo* bi, const VkDescriptorImageInfo* ii) {
                    w[n].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[n].dstSet          = m_resolveSet[f];
                    w[n].dstBinding      = binding;
                    w[n].descriptorCount = 1;
                    w[n].descriptorType  = type;
                    w[n].pBufferInfo     = bi;
                    w[n].pImageInfo      = ii;
                    ++n;
                };
                constexpr VkDescriptorType kUbo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                constexpr VkDescriptorType kCis = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                wr( 0, kUbo, &lightBI, nullptr);
                wr(10, kCis, nullptr, &giSunII);
                wr(11, kCis, nullptr, &giLocalII);
                wr(12, kCis, nullptr, &csmII);
                wr(13, kCis, nullptr, &localII);
                wr(15, kCis, nullptr, &skyII);
                wr(16, kCis, nullptr, &aoII);
                wr(17, kCis, nullptr, &giIrrII);
                wr(18, kCis, nullptr, &giVisII);
                wr(HE::vkmat::kResolveGB0Binding,   kCis, nullptr, &gbII[0]);
                wr(HE::vkmat::kResolveGB1Binding,   kCis, nullptr, &gbII[1]);
                wr(HE::vkmat::kResolveGB2Binding,   kCis, nullptr, &gbII[2]);
                wr(HE::vkmat::kResolveDepthBinding, kCis, nullptr, &gbII[3]);
                wr(HE::vkmat::kResolveUboBinding,   kUbo, &uboBI, nullptr);
                wr(31, kCis, nullptr, &ssrII);
                wr(32, kCis, nullptr, &giReflII); // heGIReflFwd (frameMatLight carries giRefl.z)
                wr(33, kCis, nullptr, &whiteII); // heCloudShadow: cloudShadowB.x stays 0
                static_assert(HE::vkmat::kResolvePreClusterBindingCount == 17,
                              "one fixed write per non-cluster row of kResolveBindings");
                // The lists whenever they exist — the clustered resolve uses them
                // statically, the 8-light one ignores them.
                if (m_clusterReady)
                {
                    const ClusterBuffer* cbs[3] = { &m_clusterLights[f], &m_clusterGrid[f], &m_clusterIdx[f] };
                    const uint32_t cbind[3] = { HE::vkmat::kClusterLightsBinding,
                                                HE::vkmat::kClusterGridBinding,
                                                HE::vkmat::kClusterIdxBinding };
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        clusterBI[c] = { cbs[c]->buf, 0, VK_WHOLE_SIZE };
                        wr(cbind[c], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &clusterBI[c], nullptr);
                    }
                }
                vkUpdateDescriptorSets(m_device, n, w, 0, nullptr);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rpipe);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeLayout,
                                        0, 1, &m_resolveSet[f], 0, nullptr);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                ++m_statDraws;
                ++m_statTris;
                static bool s_resolveLogged = false; // the runtime witness
                if (!s_resolveLogged)
                {
                    s_resolveLogged = true;
                    HE_LOG_INFO(RHI, "VulkanRenderer: deferred frame (%ux%u, %s resolve, G-buffer view %d)",
                                width, height, rpipe == m_resolveClusteredPipe ? "clustered" : "8-light",
                                HE::viewModeGBufferIndex(m_viewMode));
                }
            }

            // Back to the scene pipeline + set 0 for the tail.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
            // Forward replay: graph materials without a G-buffer variant, lit by
            // their own forward shader against the G-buffer depth (test + write).
            for (const DrawCall* dc : deferredForwardDCs) drawDCVk(*dc);
        }

        // Screen-space decals: after the opaque geometry (which is what the depth
        // pre-pass captured), before the transparent pass — the same slot the
        // Metal/GL paths use inside their G-buffer pass. Binds its own pipeline
        // and set 0, so the scene state is restored right after.
        // A deferred frame put them into GB0 already (gbDecals); if it could
        // not, the forward decal reconstructs from GB3 (no pre-pass ran).
        const VkImageView fwdDecalDepth = deferred ? m_gbView[3]
                                        : m_decalDepthActive ? m_decalDepthActive->view : VK_NULL_HANDLE;
        if (fwdDecalDepth && !gbDecals && !m_renderWorld.decals.empty())
        {
            EncodeDecals(cmd, fwdDecalDepth, width, height, hdr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeMatScenePipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
        }

        // ── Skinned mesh draw loop ────────────────────────────────────────────
        // Opaque, so BEFORE the transparent pass (Thema 150, as on Metal/GL): a
        // glass pane in front of a character blends over it instead of being
        // overdrawn by it. In a deferred frame it is part of the forward tail
        // (skinned stays forward there, like on Metal/GL). Uses skinned.vert +
        // scene.frag (set=0 still bound).
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
                                    0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);

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

                // Material block → its ring slot + set 0 at that offset (same as drawDCVk).
                {
                    const SceneMatData md{
                        dc.baseColor.r, dc.baseColor.g, dc.baseColor.b, dc.metallic,
                        dc.roughness, dc.opacity, textured ? 1.0f : 0.0f,
                        dc.receivesShadow ? 0.0f : 1.0f
                    };
                    pushSceneMaterial(&md, sizeof(md));
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeLayout,
                                            0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
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

                // Section or whole — a multi-section skinned mesh arrives as one
                // SkinnedDrawCall per slot (GeometryPass), each with its range;
                // every slot spends one bone-ring slot (skinnedIdx) of its own.
                const VulkanIndexRange range =
                    DrawIndexRange(dc, static_cast<uint32_t>(smesh->indexCount));
                if (range.count > 0)
                {
                    vkCmdDrawIndexed(cmd, range.count, 1, range.start, 0, 0);
                    ++m_statDraws;
                    m_statTris += range.count / 3;
                }
                ++skinnedIdx;
            }

            // Restore scene pipeline and set=0 binding for any subsequent work.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                hdr && m_scenePipelineHDR ? m_scenePipelineHDR : m_scenePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipelineLayout,
                                    0, 1, &m_frameUBO[m_currentFrame].set, 1, &m_sceneMatOffset);
        }

        const VkPipeline transPipe = hdr && m_sceneTransparentPipelineHDR
            ? m_sceneTransparentPipelineHDR : m_sceneTransparentPipeline;
        if (!transparentDCs.empty() && transPipe) {
            allowInstancing = false; // transparent batches keep the per-instance loop (blend + depth sort)
            GpuPassScope _transTimer(this, "Transparent");
            activeMatScenePipe = transPipe; // A4: graph-material draws restore THIS in the transparent pass
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transPipe);
            for (const DrawCall* dc : transparentDCs) drawDCVk(*dc);
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
    poolSizes[0].descriptorCount = k_maxFramesInFlight * 7;   // the main sky + six reflection-cube faces per frame
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = k_maxFramesInFlight * 7 * 2;   // moon (binding 1) + noise (binding 2)
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight * 7;
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
        submitGraphics(si, VK_NULL_HANDLE);
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
    auto makeSkyUboSet = [&](SkyUBO& u)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = kSkyUBOBytes;
        bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(m_device, &bci, nullptr, &u.buf), "sky UBO buffer");

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, u.buf, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(m_device, &mai, nullptr, &u.mem), "sky UBO memory");
        vkBindBufferMemory(m_device, u.buf, u.mem, 0);
        vkMapMemory(m_device, u.mem, 0, kSkyUBOBytes, 0, &u.mapped);

        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_skyDSPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_skyDSLayout;
        vkCheck(vkAllocateDescriptorSets(m_device, &dsai, &u.set), "sky descriptor set");

        VkDescriptorBufferInfo dbi{ u.buf, 0, kSkyUBOBytes };
        VkWriteDescriptorSet wr[3]{};
        wr[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet          = u.set;
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
        wr[1].dstSet          = u.set;
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
        wr[2].dstSet          = u.set;
        wr[2].dstBinding      = 2;
        wr[2].descriptorCount = 1;
        wr[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[2].pImageInfo      = &niiNoise;
        vkUpdateDescriptorSets(m_device, 3, wr, 0, nullptr);
    };
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        makeSkyUboSet(m_skyUBO[i]);
        for (SkyUBO& face : m_skyReflUBO[i]) makeSkyUboSet(face);
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
    // Fragment: the GL sky (HorizonRendering/SkyShaderSource.h) compiled to
    // SPIR-V here — atmosphere, three-colour nebula, 3D aurora, phased moon,
    // cirrus, 3D volumetric clouds, god rays, the same text GL runs. Its prelude
    // uses exactly this set layout (0 = UBO, 1 = moon, 2 = noise). Without the
    // cross-compiler, or if the compile fails, the reduced shaders/sky.frag
    // (built by glslc) takes over.
    VkShaderModule vs = loadShaderModule("sky.vert.spv");
    VkShaderModule fs = VK_NULL_HANDLE;
    m_skyFullModel = false;
#if defined(HE_HAVE_SHADERC)
    {
        const he::shaderc::Result spv = he::shaderc::compile(HE::glsl::BuildSkyFragmentGLSL450(),
                                                             he::shaderc::Stage::Fragment,
                                                             he::shaderc::Target::SpirvBinary);
        if (spv.ok && !spv.spirv.empty())
        {
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = spv.spirv.size() * sizeof(uint32_t);
            ci.pCode    = spv.spirv.data();
            m_skyFullModel = vkCreateShaderModule(m_device, &ci, nullptr, &fs) == VK_SUCCESS;
        }
        if (!m_skyFullModel)
        {
            fs = VK_NULL_HANDLE;
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: GL sky did not compile to SPIR-V, "
                                   "falling back to the reduced sky.frag");
        }
    }
#endif
    if (!fs) fs = loadShaderModule("sky.frag.spv");
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
        if (createGraphicsPipelines(1, &pci, nullptr, &out) != VK_SUCCESS)
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: sky pipeline creation failed");
    };

    makeSkyPipeline(m_renderPass, m_skyPipeline);
    if (m_postFxSceneRP)
        makeSkyPipeline(m_postFxSceneRP, m_skyPipelineHDR);
    // The reflection cube's faces: ONE RGBA16F colour attachment, cleared, left for the reflection kernels to sample.
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;     // every face is rewritten whole each frame
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;   // last frame's reads
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1;    rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_skyReflRP) == VK_SUCCESS)
            makeSkyPipeline(m_skyReflRP, m_skyReflPipe);
        else
            m_skyReflRP = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(m_device, vs, nullptr);
    vkDestroyShaderModule(m_device, fs, nullptr);
}

void VulkanRenderer::destroySkyPipeline()
{
    destroySkyReflCube();
    if (m_skyReflPipe) { vkDestroyPipeline(m_device, m_skyReflPipe, nullptr);   m_skyReflPipe = VK_NULL_HANDLE; }
    if (m_skyReflRP)   { vkDestroyRenderPass(m_device, m_skyReflRP, nullptr);    m_skyReflRP   = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        for (SkyUBO& face : m_skyReflUBO[i])
        {
            if (face.mapped) { vkUnmapMemory(m_device, face.mem); face.mapped = nullptr; }
            if (face.buf)    { vkDestroyBuffer(m_device, face.buf, nullptr); face.buf = VK_NULL_HANDLE; }
            if (face.mem)    { vkFreeMemory(m_device, face.mem, nullptr);    face.mem = VK_NULL_HANDLE; }
        }
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
    drawSkyFrom(cmd, hdr,
                taaJittered(HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view),
                m_renderWorld.sunDirection, m_renderWorld.camera.position, m_wallTime, m_environment);
}

void VulkanRenderer::drawSkyFrom(VkCommandBuffer cmd, bool hdr, const glm::mat4& clipViewProj,
                                 const glm::vec3& sunDir, const glm::vec3& cameraPos, float time,
                                 const EnvironmentSettings& env, SkyUBO* ubo, VkPipeline pipeOverride,
                                 const char* timerName)
{
    VkPipeline pipe = pipeOverride ? pipeOverride : (hdr && m_skyPipelineHDR ? m_skyPipelineHDR : m_skyPipeline);
    if (!pipe || !m_skyPipelineLayout) return;
    if (!env.skyEnabled) return; // no Sky entity → leave the cleared background
    GpuPassScope _skyTimer(this, timerName);
    SkyUBO& skyUbo = ubo ? *ubo : m_skyUBO[m_currentFrame];

    // The one shared EnvironmentSettings → sky-constants translation. The GL sky
    // compiled at startup reads HE::SkyFrameParams as its UBO (one memcpy). The
    // fallback sky.frag's UBO is a REDUCED copy of it (see the header block in
    // shaders/sky.frag), so there the fields are read out by name — a blanket
    // copy would misalign every offset past invViewProj.
    HE::SkyFrameInputs in;
    in.invViewProj    = glm::inverse(clipViewProj);
    in.sunDir         = glm::normalize(sunDir);
    in.cameraPos      = cameraPos;
    in.time           = time;
    in.hasMoonTexture = (m_moonImage != VK_NULL_HANDLE);
    const HE::SkyFrameParams p = HE::BuildSkyFrameParams(env, in);

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

    if (skyUbo.mapped)
    {
        if (m_skyFullModel) std::memcpy(skyUbo.mapped, &p, sizeof(p));
        else                std::memcpy(skyUbo.mapped, &sky, sizeof(sky));
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipelineLayout,
                            0, 1, &skyUbo.set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // full-screen triangle, no vertex buffer
}

// The cube's image, face views, framebuffers and sampler, created on the first frame the reflections run (it costs
// 6 x 128 x 128 x 8 bytes). false = unusable; the kernels then get the dummy cube and a zero flag.
bool VulkanRenderer::ensureSkyReflCube()
{
    if (m_skyReflCubeView) return true;
    if (!m_skyReflRP || !m_skyReflPipe) return false;
    constexpr uint32_t N = kSkyReflCubeSize;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
    ici.extent        = { N, N, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 6;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto fail = [&]() { destroySkyReflCube(); return false; };
    if (vkCreateImage(m_device, &ici, nullptr, &m_skyReflImage) != VK_SUCCESS) return fail();
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, m_skyReflImage, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    try { mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); }
    catch (...) { return fail(); }
    if (vkAllocateMemory(m_device, &mai, nullptr, &m_skyReflMem) != VK_SUCCESS) return fail();
    vkBindImageMemory(m_device, m_skyReflImage, m_skyReflMem, 0);
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image    = m_skyReflImage;
    vci.format   = ici.format;
    vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    if (vkCreateImageView(m_device, &vci, nullptr, &m_skyReflCubeView) != VK_SUCCESS) return fail();
    for (uint32_t f = 0; f < 6; ++f)
    {
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1 };
        if (vkCreateImageView(m_device, &vci, nullptr, &m_skyReflFaceView[f]) != VK_SUCCESS) return fail();
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_skyReflRP; fci.attachmentCount = 1; fci.pAttachments = &m_skyReflFaceView[f];
        fci.width = N; fci.height = N; fci.layers = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_skyReflFB[f]) != VK_SUCCESS) return fail();
    }
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &sci, nullptr, &m_skyReflSampler) != VK_SUCCESS) return fail();
    return true;
}

void VulkanRenderer::destroySkyReflCube()
{
    for (VkFramebuffer& fb : m_skyReflFB)  { if (fb) vkDestroyFramebuffer(m_device, fb, nullptr); fb = VK_NULL_HANDLE; }
    for (VkImageView& v : m_skyReflFaceView) { if (v) vkDestroyImageView(m_device, v, nullptr); v = VK_NULL_HANDLE; }
    if (m_skyReflCubeView) { vkDestroyImageView(m_device, m_skyReflCubeView, nullptr); m_skyReflCubeView = VK_NULL_HANDLE; }
    if (m_skyReflImage)    { vkDestroyImage(m_device, m_skyReflImage, nullptr);        m_skyReflImage    = VK_NULL_HANDLE; }
    if (m_skyReflMem)      { vkFreeMemory(m_device, m_skyReflMem, nullptr);            m_skyReflMem      = VK_NULL_HANDLE; }
    if (m_skyReflSampler)  { vkDestroySampler(m_device, m_skyReflSampler, nullptr);    m_skyReflSampler  = VK_NULL_HANDLE; }
    m_skyReflValid = false;
    m_skyReflBaked = false;
}

// Six faces of the real sky around the camera, low-res and without the screen-space low-res cloud buffer (a face is
// not a screen). Same matrices as OpenGL/Metal: SkyCubeFaceInvViewProj(face, camera, rowZeroAtTop = false) - NDC y = -1
// lands in texel row 0 on Vulkan as on OpenGL, and the cube-face table is GL's (see updateSkyEnvCube). drawSkyFrom
// takes the forward matrix and inverts it, hence the inverse of the face's inverse.
void VulkanRenderer::renderSkyReflCube(VkCommandBuffer cmd)
{
    m_skyReflValid = false;
    static const bool s_off = [] { const char* e = std::getenv("HE_GIREFL_SKY"); return e && e[0] == '0'; }();
    if (s_off || !m_environment.skyEnabled || !ensureSkyReflCube()) return;
    static_assert(k_maxFramesInFlight <= 2, "m_skyReflUBO holds two frames");
    // The faces this frame redraws: all six the first time, then a rotating slice. A face NOT drawn keeps its last picture
    // (its layout is SHADER_READ_ONLY from the pass that wrote it; layouts are per layer).
    const uint32_t firstFace = m_skyReflBaked ? m_skyReflPhase * kSkyReflFacesPerFrame : 0u;
    const uint32_t endFace   = m_skyReflBaked ? firstFace + kSkyReflFacesPerFrame : 6u;
    const glm::vec3 camPos = m_renderWorld.camera.position;
    constexpr uint32_t N = kSkyReflCubeSize;
    VkClearValue clear{};
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    const VkViewport vp{ 0, 0, float(N), float(N), 0, 1 };
    const VkRect2D   sc{ { 0, 0 }, { N, N } };
    for (uint32_t f = firstFace; f < endFace; ++f)
    {
        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpbi.renderPass = m_skyReflRP; rpbi.framebuffer = m_skyReflFB[f];
        rpbi.renderArea = sc; rpbi.clearValueCount = 1; rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        drawSkyFrom(cmd, /*hdr=*/true, glm::inverse(HE::SkyCubeFaceInvViewProj(static_cast<int>(f), camPos, false)),
                    m_renderWorld.sunDirection, camPos, m_wallTime, m_environment,
                    &m_skyReflUBO[m_currentFrame][f], m_skyReflPipe, "SkyRefl");
        vkCmdEndRenderPass(cmd);
    }
    m_skyReflValid = true;
    m_skyReflPhase = m_skyReflBaked ? (m_skyReflPhase + 1) % (6 / kSkyReflFacesPerFrame) : 0u;
    m_skyReflBaked = true;
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
        if (createGraphicsPipelines(1, &pci, nullptr, &out) != VK_SUCCESS)
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
    drawLineList(cmd, viewProj, m_debugLines, hdr);
}

void VulkanRenderer::drawLineList(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                  const std::vector<DebugLine>& lines, bool hdr)
{
    if (!m_debugPipeline || !m_debugPipelineLayout) return;
    if (lines.empty()) return;

    const uint32_t fi = m_currentFrame;

    // Pack interleaved float data: [pos.x, pos.y, pos.z, col.r, col.g, col.b] per endpoint
    constexpr VkDeviceSize kMaxDebugVerts = 65536;
    const uint32_t vertCount = std::min<uint32_t>(
        static_cast<uint32_t>(lines.size() * 2), kMaxDebugVerts);

    if (m_debugVBMapped[fi] && vertCount > 0)
    {
        float* dst = static_cast<float*>(m_debugVBMapped[fi]);
        for (uint32_t li = 0; li < vertCount / 2; ++li)
        {
            const DebugLine& dl = lines[li];
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

    deviceWaitIdle();

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
    submitGraphics(si, VK_NULL_HANDLE);
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
        for (SkyUBO& face : m_skyReflUBO[i])   // the reflection cube's faces show the moon as well
            if (face.set)
            {
                wr.dstSet = face.set;
                vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
            }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui editor textures (content-browser icons + logo)
// ─────────────────────────────────────────────────────────────────────────────
// A device-local R8G8B8A8_UNORM image through uploadRGBA8Image (the upload
// batch), plus its OWN linear sampler (the moon path reuses m_moonSampler); it
// does NOT touch any scene descriptor sets. The
// image/view/memory/sampler are retained in m_imguiTextures so they outlive the
// VkDescriptorSet ImGui samples from; the editor's registrar (which links ImGui)
// calls ImGui_ImplVulkan_AddTexture to build that descriptor set.
void* VulkanRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
    if (!rgba8Pixels || width <= 0 || height <= 0 || !m_device) return nullptr;

    // Through the upload batch like every other texture (REN-802): no device
    // idle and no queue wait per icon. Submitted right away, though: with
    // multi-viewport on, imgui_impl_vulkan submits the platform windows itself
    // (RenderPlatformWindowsDefault), past submitGraphics, and may draw the
    // icon there first.
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
    if (!uploadRGBA8Image(static_cast<const uint8_t*>(rgba8Pixels), static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height), image, memory, view))
        return nullptr;
    flushUploads();

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
    m_giShadowRays          = std::clamp(s.shadowRays, 1, 256);
    m_giShadowHistoryWeight       = std::clamp(s.shadowHistory, 0.0f, 0.98f);
    m_giShadowFilter        = s.shadowFilter;
}

// GI reflections. Same clamps as GL/D3D11/D3D12; `bounces` is ignored here too
// (one traced segment), and the quality tier only sets rays + blur reach (the
// trace resolution is the prepass', the documented GL gap — see
// GIReflectionTier.h and docs/gi-reflections-cross-backend-plan-2026-10-20.md).
void VulkanRenderer::SetGIReflectionSettings(const GIReflectionSettings& s)
{
    m_giReflEnabled      = s.enabled;
    m_giReflIntensity    = std::clamp(s.intensity, 0.0f, 1.0f);
    m_giReflMaxRoughness = std::clamp(s.maxRoughness, 0.0f, 1.0f);
    m_giReflMaxDistance  = std::max(1.0f, s.maxDistance);
    m_giReflQuality      = std::clamp(s.quality, 0, 2);
    m_giReflBlurEnabled  = s.blur;
}

// Screen-space reflections (docs/ssr-cross-backend-plan.md checkpoint B1). The
// trace is a fragment shader over a rasterized pre-pass — no compute, no BVH,
// no extension — so unlike the ray-traced GI it has no device gate here. What
// it does need is the shader cross-compiler and the post chain's HDR target
// (editor viewport or packaged game); both are checked in ssrWantedThisFrame().
void VulkanRenderer::SetShadowSettings(const ShadowSettings& s)
{
    // The images are not touched here: this is called from the editor's frame
    // push, which may land between passes. Render() re-creates them at its top
    // (after vkDeviceWaitIdle) when the size no longer matches, and hands the
    // extractor the size that is actually allocated. Mirrors GL/Metal/D3D.
    m_shadowSizeDirty |= (s.resolution != m_shadowSettings.resolution);
    m_shadowSettings   = s;
}

void VulkanRenderer::SetShadowDebug(bool on)
{
    m_debugShadowCascades = on;
}

void VulkanRenderer::SetSSRSettings(const SSRSettings& s)
{
    m_ssrEnabled      = s.enabled;
    m_ssrIntensity    = std::clamp(s.intensity, 0.0f, 1.0f);
    m_ssrMaxRoughness = std::clamp(s.maxRoughness, 0.0f, 1.0f);
    m_ssrMaxDistance  = std::max(1.0f, s.maxDistance);
    m_ssrThickness    = std::max(1e-3f, s.thickness);
    m_ssrQuality      = std::clamp(s.quality, 0, 2);
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

HE::GiBvh VulkanRenderer::BuildGIBvh(const HE::UUID& meshId)
{
    HE_PROFILE_SCOPE_N("Vulkan::GiBuildBlas");
    HE::GiBvh bvh;
    if (!m_contentManager) return bvh;
    const StaticMeshAsset* asset = m_contentManager->getStaticMesh(meshId);
    if (!asset || asset->indices.empty()) return bvh;
    meshStreamCounters().blasBuilds.fetch_add(1, std::memory_order_relaxed);
    meshStreamCounters().blasTris.fetch_add(asset->indices.size() / 3, std::memory_order_relaxed);

    // Same two layouts the mesh upload path consumes: cooked interleaved
    // 8-float (position at offset 0) or loose tightly-packed 3-float positions.
    if (asset->cooked && !asset->interleaved.empty())
        bvh = m_giBvhCache.build(meshId, asset->interleaved.data(), asset->vertexCount, 8,
                             asset->indices.data(), asset->indices.size());
    else if (!asset->vertices.empty())
        bvh = m_giBvhCache.build(meshId, asset->vertices.data(), asset->vertices.size() / 3, 3,
                             asset->indices.data(), asset->indices.size());
    return bvh;
}

void VulkanRenderer::destroyGiBuffer(GiBuffer& b)
{
    if (b.mapped) { vkUnmapMemory(m_device, b.mem); b.mapped = nullptr; }
    if (b.buf)    { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem)    { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
    b.size = 0;
}

bool VulkanRenderer::uploadGiBuffer(GiBuffer& b, const void* data, VkDeviceSize size,
                                    VkBufferUsageFlags usage, VkDeviceSize from, VkDeviceSize reserve, bool fresh)
{
    if (size == 0) return false;
    if (b.size < size || (fresh && b.buf && from == 0))
    {
        if (fresh && b.buf) m_retiredGiBufs.push_back({ b, static_cast<int>(k_maxFramesInFlight) + 2 });
        else                destroyGiBuffer(b);
        b = GiBuffer{};
        from   = 0;                      // a fresh buffer holds nothing yet
        const VkDeviceSize alloc = std::max(size, reserve);

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = alloc;
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
        if (vkMapMemory(m_device, b.mem, 0, alloc, 0, &b.mapped) != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE;
            vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE;
            return false;
        }
        b.size = alloc;
    }
    if (from < size)
        std::memcpy(static_cast<uint8_t*>(b.mapped) + from, static_cast<const uint8_t*>(data) + from,
                    static_cast<size_t>(size - from));
    return true;
}

void VulkanRenderer::updateGiAccel()
{
    HE_PROFILE_SCOPE_N("Vulkan::GiUpdateAccel");
    m_giInstanceCount = 0;
    m_giHwInstOk      = false;
    m_giHwInstancesCpu.clear();
    // The reflection pass traces the same BVH, so it needs the instances even
    // with the diffuse GI off (D3D11/D3D12 gate the same way).
    if (!(m_giEnabled || m_giReflEnabled)) return;

    // Before any instance reads an offset: a compaction moves the ranges.
    if (m_giBlas.compactIfWasteful())
        HE_LOG_INFO(RHI, "VulkanRenderer: GI BLAS store compacted (%zu meshes, %zu triangles)",
                    m_giBlas.entries(), m_giBlas.tris().size());

    // Same caster filter as the shadow pass / the other backends' TLAS builds:
    // castsShadow only, UNculled (rays go in arbitrary directions).
    std::vector<GIInstanceGpu> instances;
    instances.reserve(m_renderWorld.objects.size());
    // gi_refl.comp's own copy (m_giReflInstanceBuf), same order/index as
    // `instances` — only built while reflections are on. Keeping the folded
    // graph-material colour out of `instances` means gi_probe.comp's bounce
    // colour never changes just because reflections got toggled (Thema 191
    // step 11; `instances` alone feeds gi_shadow.comp/gi_probe.comp/the DXR
    // InstanceId mapping, unchanged from before this feature existed).
    std::vector<GIInstanceGpu> reflInstances;
    if (m_giReflEnabled) reflInstances.reserve(m_renderWorld.objects.size());
    // Landscape table for gi_refl.comp: an entry (and its weightmap view) per terrain the extractor found, so a hit
    // reads the paint / the slope and height instead of one flat colour for the whole terrain. Same contract and
    // cap as the GL and Metal paths; the extractor's index maps to the table's through `landRemap` (-1 = left
    // out: over the cap, or its weightmap is not resident yet — it keeps the flat colour).
    std::vector<GILandGpu> lands;
    std::vector<int>       landRemap(m_renderWorld.landscapes.size(), -1);
    if (m_giReflEnabled)
    {
        for (VkImageView& v : m_giLandWeightView) v = VK_NULL_HANDLE;
        for (size_t i = 0; i < m_renderWorld.landscapes.size(); ++i)
        {
            if (static_cast<int>(lands.size()) >= HE::kGiMaxLandscapes) break;
            const HE::GiLandscape& ls = m_renderWorld.landscapes[i];
            const bool autoLand = ls.layerCount == HE::kGiLandAuto;
            VkImageView wm = m_whiteAlbedoView;   // an auto entry has no weightmap: its unit is never sampled
            if (!autoLand)
            {
                wm = resolveGraphTexture(ls.weightmapId, {});
                if (!wm) continue;
            }
            GILandGpu g;
            g.worldToLocal = ls.worldToLocal;
            g.cfg = glm::vec4(ls.invSize.x, ls.invSize.y, ls.uvTiling, static_cast<float>(ls.layerCount));
            for (int k = 0; k < 4; ++k) g.layer[k] = ls.layerColor[k];
            g.autoWet   = ls.autoWet;
            g.autoSlope = ls.autoSlope;
            g.autoSnow  = ls.autoSnow;
            landRemap[i] = static_cast<int>(lands.size());
            m_giLandWeightView[lands.size()] = wm;
            lands.push_back(g);
        }
    }
    auto resolveRange = [&](const HE::UUID& id) -> GIBlasRange
    {
        if (const GIBlasRange* r = m_giBlas.find(id)) return *r;
        return m_giBlas.add(id, BuildGIBvh(id));
    };
    // HW path: build a VkAccelerationStructureInstanceKHR array in EXACTLY the
    // giInsts order (the kernels index giInsts by InstanceId). Any mesh whose
    // HW BLAS can't be built breaks that 1:1 mapping → SW kernels this frame.
    // Reflections use this TLAS through gi_refl_hw.comp when the diffuse GI is on too; a reflections-only frame
    // (GI off) builds no TLAS and traces the SW BVH with gi_refl.comp.
    bool hwAll = m_giHwRt && m_giHwPipesReady && m_giEnabled;
    auto resolveHwBlas = [&](const HE::UUID& id, const GIBlasRange& range) -> const GiHwBlas&
    {
        auto it = m_giHwBlasCache.find(id);
        if (it == m_giHwBlasCache.end())
            it = m_giHwBlasCache.emplace(id, buildGiHwBlas(id, range)).first;
        return it->second;
    };
    // HE_GI_LOG_INSTANCES=N: log every instance's bounce colour once, on the
    // N-th call — what the probe kernels multiply in (Thema 154 witness).
    static const int s_giLogAt = [] {
        const char* v = std::getenv("HE_GI_LOG_INSTANCES");
        return v && *v ? std::atoi(v) : 0;
    }();
    static int s_giLogCall = 0;
    const bool logInst = s_giLogAt > 0 && ++s_giLogCall == s_giLogAt;
    const HE::GiCasterFilter casterFilter = HE::GiCasterFilter::For(m_renderWorld, m_giReflEnabled);   // GiCasterRange.h
    // The software shadow kernel has its own list, thinned by distance even while reflections keep the far scene
    // in the list the probes and mirrors walk.
    const HE::GiCasterFilter shadowFilter = HE::GiCasterFilter::For(m_renderWorld, false);
    static const bool s_hfOn = [] { const char* v = std::getenv("HE_GI_HEIGHTFIELD"); return !(v && v[0] == '0'); }();
    const GiHeightfield* gHf = (s_hfOn && m_renderWorld.heightfields.size() == 1) ? &m_renderWorld.heightfields[0] : nullptr;
    m_giHfActive = gHf && ensureGiHeightfield(*gHf);
    std::vector<GIInstanceGpu> shadowInstances;   // without the terrain chunks while m_giHfActive (software shadow kernel)
    for (const RenderObject& obj : m_renderWorld.objects)
    {
        // A foliage cluster is one plant's transform with a whole bucket's box: not a GI occluder.
        if (!obj.castsShadow || obj.isCluster() || !(casterFilter.keeps(obj) || shadowFilter.keeps(obj))) continue;
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
        if (shadowFilter.keeps(obj) && !(m_giHfActive && obj.terrainChunk)) shadowInstances.push_back(inst);
        if (!casterFilter.keeps(obj)) continue;
        instances.push_back(inst);
        if (m_giReflEnabled)
        {
            // The shared per-instance surface (HE::giInstanceSurface): a graph
            // material's folded BaseColor/Emissive instead of the mesh's own
            // colour, which is plain white for every graph material and would
            // make each of them reflect white. Written into reflInstances only
            // (gi_refl.comp's own buffer) — `instances` above keeps
            // obj.baseColor regardless, so the GI-only image really does stay
            // exactly what it was. Roughness and metallic of the HIT are not
            // carried: gi_refl.comp (one segment, no bounce loop) does not
            // read them either (matches D3D11/D3D12/GL).
            const HE::GiInstanceSurface surf = HE::giInstanceSurface(obj, m_contentManager, &m_renderWorld.landscapes);
            GIInstanceGpu reflInst = inst;
            // .a carries the landscape table index + 1 (0 = none); gi_refl.comp never reads it as an alpha.
            const int landSlot = (obj.landscapeIndex >= 0 && obj.landscapeIndex < static_cast<int32_t>(landRemap.size()))
                                     ? landRemap[static_cast<size_t>(obj.landscapeIndex)] : -1;
            reflInst.baseColor  = glm::vec4(surf.albedo, static_cast<float>(landSlot + 1));
            reflInst.emissiveRG = static_cast<int32_t>(
                glm::packHalf2x16(glm::vec2(surf.emissive.r, surf.emissive.g)));
            reflInst.emissiveB  = static_cast<int32_t>(
                glm::packHalf2x16(glm::vec2(surf.emissive.b, 0.0f)));
            reflInstances.push_back(reflInst);
        }
        if (logInst)
            HE_LOG_INFO(RHI, "VulkanRenderer: GI instance %zu pos (%.2f, %.2f, %.2f) mat %016llx "
                        "baseColor (%.3f, %.3f, %.3f)", instances.size() - 1,
                        obj.transform[3].x, obj.transform[3].y, obj.transform[3].z,
                        static_cast<unsigned long long>(obj.materialAssetId.lo),
                        inst.baseColor.r, inst.baseColor.g, inst.baseColor.b);

        if (hwAll)
        {
            const GiHwBlas& hb = resolveHwBlas(effectiveId, range);
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

    if (m_giBlas.dirty() && !m_giBlas.nodes().empty())
    {
        HE_PROFILE_SCOPE_N("Vulkan::GiUploadBlas");
        // Between rewrites the store only appends: frames in flight read the
        // unchanged [0, from) while the new tail goes in behind it. A rewrite
        // (compaction moved every range) or a grown buffer would pull bytes out
        // from under them, so those (rare) cases go into a NEW buffer (`fresh`) and the
        // old one is retired; nothing idles the device any more (that was 34-57 ms in
        // the frame the buffer grew, perf capture 2026-10-28).
        const VkDeviceSize nodeBytes = m_giBlas.nodes().size() * sizeof(HE::GiBvhNode);
        const VkDeviceSize triBytes  = m_giBlas.tris().size()  * sizeof(HE::GiBvhTriangle);
        const VkDeviceSize nodeFrom  = m_giBlas.uploadFromNodes() * sizeof(HE::GiBvhNode);
        const VkDeviceSize triFrom   = m_giBlas.uploadFromTris()  * sizeof(HE::GiBvhTriangle);
        const bool nodesOk = uploadGiBuffer(m_giNodeBuf, m_giBlas.nodes().data(), nodeBytes,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nodeFrom,
                                            nodeBytes + nodeBytes / 2, /*fresh=*/true);
        const bool trisOk  = uploadGiBuffer(m_giTriBuf, m_giBlas.tris().data(), triBytes,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, triFrom,
                                            triBytes + triBytes / 2, /*fresh=*/true);
        if (!nodesOk || !trisOk) return;
        m_giBlas.markUploaded();
    }
    // Instances change every frame while earlier frames are in flight — ring
    // slot per in-flight frame, same convention as the bones UBO ring.
    static_assert(k_maxFramesInFlight <= sizeof(m_giInstanceBuf) / sizeof(m_giInstanceBuf[0]),
                  "GI instance ring smaller than frames in flight");
    if (!uploadGiBuffer(m_giInstanceBuf[m_currentFrame], instances.data(),
                        instances.size() * sizeof(GIInstanceGpu)))
        return;
    m_giShadowSep = shadowInstances.size() != instances.size();
    if (m_giShadowSep)
    {
        const GIInstanceGpu dummy{};   // never size 0
        m_giShadowInstanceCount = static_cast<int>(shadowInstances.size());
        if (!uploadGiBuffer(m_giShadowInstanceBuf[m_currentFrame], shadowInstances.empty() ? &dummy : shadowInstances.data(),
                            std::max<size_t>(shadowInstances.size(), 1) * sizeof(GIInstanceGpu)))
            { m_giShadowSep = false; m_giHfActive = false; }
    }
    m_giInstanceCount = static_cast<int>(instances.size());
    if (!m_giShadowSep) m_giShadowInstanceCount = m_giInstanceCount;
    meshStreamCounters().giInstances.store(static_cast<uint32_t>(m_giInstanceCount), std::memory_order_relaxed);
    // gi_refl.comp's own ring, same slot convention — only kept current while
    // reflections are enabled (runGiReflections doesn't run otherwise).
    if (m_giReflEnabled)
    {
        uploadGiBuffer(m_giReflInstanceBuf[m_currentFrame], reflInstances.data(),
                       reflInstances.size() * sizeof(GIInstanceGpu));
        // Never size 0: an empty storage-buffer binding is a validation error, and the kernel gates every read on
        // giLands.length() and the instance's table index anyway.
        const GILandGpu dummy{};
        uploadGiBuffer(m_giLandBuf[m_currentFrame], lands.empty() ? &dummy : lands.data(),
                       std::max<size_t>(lands.size(), 1) * sizeof(GILandGpu));
    }

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
    for (GiBuffer& b : m_giShadowInstanceBuf) destroy(b);
    retireGiHeightfield();
    for (auto& r : m_retiredHf)   // destroyGiAccel runs with the device idle (shutdown / GI off)
    {
        if (r.view) vkDestroyImageView(m_device, r.view, nullptr);
        if (r.img)  vkDestroyImage(m_device, r.img, nullptr);
        if (r.mem)  vkFreeMemory(m_device, r.mem, nullptr);
    }
    m_retiredHf.clear();
    m_giHfVersion = 0; m_giHfKey = ~0ull; m_giHfActive = false; m_giShadowSep = false; m_giShadowInstanceCount = 0;
    for (GiBuffer& b : m_giReflInstanceBuf) destroy(b);
    for (GiBuffer& b : m_giLandBuf) destroy(b);
    m_giBlas.clear();
    m_giInstanceCount = 0;

    // HW-RT acceleration structures (BLAS cache + per-frame TLAS ring +
    // instance upload ring). Callers guarantee the GPU is idle (Shutdown waits).
    for (auto& [id, blas] : m_giHwBlasCache) destroyGiHwBlas(blas);
    m_giHwBlasCache.clear();
    for (RetiredGiBuf& r : m_retiredGiBufs) destroyGiBuffer(r.buf);
    m_retiredGiBufs.clear();
    struct PoolFlush { VulkanRenderer* r; ~PoolFlush() { r->releaseGiHwBufPool(); } } poolFlush{ this };   // after every GiHwBuffer below has been parked
    for (GiHwBlasPending& p : m_giHwBlasPending) { destroyGiHwBuffer(p.geo); destroyGiHwBuffer(p.scratch); }
    m_giHwBlasPending.clear();
    for (GiHwBlasTemp& t : m_giHwBlasTemp) { destroyGiHwBuffer(t.geo); destroyGiHwBuffer(t.scratch); }
    m_giHwBlasTemp.clear();
    for (RetiredHwBlas& r : m_retiredHwBlas) destroyGiHwBlas(r.blas);
    m_retiredHwBlas.clear();
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

namespace
{
constexpr VkDeviceSize kGiHwBufPoolMax = VkDeviceSize(256) << 20;   // bytes parked in the pool
// Size classes: powers of two from 64 KiB, so a buffer freed by one BLAS fits the next one of about its size.
VkDeviceSize giHwBufClass(VkDeviceSize size)
{
    VkDeviceSize c = VkDeviceSize(64) << 10;
    while (c < size) c <<= 1;
    return c;
}
} // namespace

bool VulkanRenderer::createGiHwBuffer(GiHwBuffer& b, VkDeviceSize size,
                                      VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
    destroyGiHwBuffer(b);
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkDeviceSize cls = giHwBufClass(size);
    // A parked buffer of this kind and size class: no vkCreateBuffer / vkAllocateMemory at all.
    for (size_t i = 0; i < m_giHwBufPool.size(); ++i)
    {
        const GiHwBuffer& p = m_giHwBufPool[i];
        if (p.usage == usage && p.props == props && p.size == cls)
        {
            b = p;
            m_giHwBufPoolBytes -= p.size;
            m_giHwBufPool.erase(m_giHwBufPool.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    size = cls;
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = size;
    bci.usage = usage;
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
    b.size  = size;
    b.usage = bci.usage;
    b.props = props;
    return true;
}

void VulkanRenderer::destroyGiHwBuffer(GiHwBuffer& b)
{
    // Park a complete buffer for reuse (the caller guarantees the GPU is done with it); a half-made one is freed.
    if (b.buf && b.mem && b.addr && b.size && b.size <= kGiHwBufPoolMax)
    {
        while (!m_giHwBufPool.empty() && m_giHwBufPoolBytes + b.size > kGiHwBufPoolMax)
        {
            GiHwBuffer& old = m_giHwBufPool.front();
            vkDestroyBuffer(m_device, old.buf, nullptr);
            vkFreeMemory(m_device, old.mem, nullptr);
            m_giHwBufPoolBytes -= old.size;
            m_giHwBufPool.erase(m_giHwBufPool.begin());
        }
        m_giHwBufPool.push_back(b);
        m_giHwBufPoolBytes += b.size;
        b = GiHwBuffer{};
        return;
    }
    if (b.buf) { vkDestroyBuffer(m_device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
    if (b.mem) { vkFreeMemory(m_device, b.mem, nullptr);    b.mem = VK_NULL_HANDLE; }
    b = GiHwBuffer{};
}

void VulkanRenderer::releaseGiHwBufPool()
{
    for (GiHwBuffer& p : m_giHwBufPool)
    {
        if (p.buf) vkDestroyBuffer(m_device, p.buf, nullptr);
        if (p.mem) vkFreeMemory(m_device, p.mem, nullptr);
    }
    m_giHwBufPool.clear();
    m_giHwBufPoolBytes = 0;
}

void VulkanRenderer::destroyGiHwBlas(GiHwBlas& b)
{
    if (b.as && m_pfnDestroyAS) { m_pfnDestroyAS(m_device, b.as, nullptr); b.as = VK_NULL_HANDLE; }
    destroyGiHwBuffer(b.buffer);
    b.address = 0;
    b.valid   = false;
}

// BLAS for a mesh: copy the mesh's triangles from the SW BVH (`range`, leaf order, three vec4 per triangle = a
// non-indexed vertex array of stride 16) into a device-address input buffer, query build sizes, create the AS and
// queue its build (flushGiHwBlasBuilds records it into the frame's command buffer ahead of the TLAS build; input +
// scratch live on in m_giHwBlasTemp until the GPU is done with them). Building from the SW triangles instead of the
// asset makes a hit's primitive index the triangle's number inside the range, so gi_refl_hw.comp reads the hit's
// vertices from the very giTris[] the SW kernels use; the shadow and probe kernels never look at it.
VulkanRenderer::GiHwBlas VulkanRenderer::buildGiHwBlas(const HE::UUID& /*meshId*/, const GIBlasRange& range)
{
    HE_PROFILE_SCOPE_N("Vulkan::GiBuildHwBlas");
    GiHwBlas out;
    if (!m_giHwRt || !range.valid || range.triCount <= 0) return out;
    if (static_cast<size_t>(range.triOffset) + static_cast<size_t>(range.triCount) > m_giBlas.tris().size()) return out;
    const uint32_t primCount = static_cast<uint32_t>(range.triCount);
    const uint32_t vcount    = primCount * 3;
    const VkDeviceSize vstride = 16;   // sizeof(GiBvhTriangle) / 3: v0, v1, v2 are consecutive vec4 (w = padding)
    static_assert(sizeof(HE::GiBvhTriangle) == 3 * 16, "the BLAS input reads a GiBvhTriangle as three vec4 vertices");
    const void* vdata = m_giBlas.tris().data() + range.triOffset;

    // Build-input buffer: the vertices only, host-visible.
    const VkDeviceSize vBytes = vstride * vcount;
    const VkDeviceSize iOfs   = vBytes;
    const VkDeviceSize iBytes = 0;
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
    tri.indexType                = VK_INDEX_TYPE_NONE_KHR;

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
        // The build is recorded later (flushGiHwBlasBuilds); the AS and its address exist now.
        GiHwBlasPending p;
        p.as           = out.as;
        p.geo          = geo;
        p.scratch      = scratch;
        p.vertexAddr   = geo.addr;
        p.indexAddr    = 0;   // non-indexed
        p.scratchAddr  = (scratch.addr + m_giAsScratchAlign - 1) & ~(VkDeviceAddress(m_giAsScratchAlign) - 1);
        p.vertexStride = vstride;
        p.maxVertex    = vcount - 1;
        p.primCount    = primCount;
        m_giHwBlasPending.push_back(p);
    }
    else
    {
        destroyGiHwBuffer(scratch);
        destroyGiHwBuffer(geo);
    }
    if (!ok)
    {
        HE_LOG_WARN(RHI, "%s",
                    "VulkanRenderer: HW BLAS creation failed — SW GI kernels for affected frames");
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

// Records the queued BLAS builds into the frame's command buffer, then the barrier that lets the TLAS
// build read them. Runs every frame that created a BLAS, also when the hardware kernels end up not
// being used: a BLAS that was created but never built would be traced as garbage later.
void VulkanRenderer::flushGiHwBlasBuilds(VkCommandBuffer cmd)
{
    if (m_giHwBlasPending.empty()) return;
    HE_PROFILE_SCOPE_N("Vulkan::GiFlushBlas");
    for (GiHwBlasPending& p : m_giHwBlasPending)
    {
        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags        = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geom.geometry.triangles;
        tri.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = p.vertexAddr;
        tri.vertexStride             = p.vertexStride;
        tri.maxVertex                = p.maxVertex;
        tri.indexType                = p.indexAddr ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR;
        tri.indexData.deviceAddress  = p.indexAddr;
        VkAccelerationStructureBuildGeometryInfoKHR bgi{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bgi.type                      = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bgi.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bgi.mode                      = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount             = 1;
        bgi.pGeometries               = &geom;
        bgi.dstAccelerationStructure  = p.as;
        bgi.scratchData.deviceAddress = p.scratchAddr;
        VkAccelerationStructureBuildRangeInfoKHR range{ p.primCount, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        m_pfnCmdBuildAS(cmd, 1, &bgi, &pRange);
        m_giHwBlasTemp.push_back({ p.geo, p.scratch, static_cast<int>(k_maxFramesInFlight) + 1 });
    }
    m_giHwBlasPending.clear();
    // BLAS writes -> the TLAS build that reads them (the TLAS -> compute barrier after it publishes both).
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
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
    glm::vec4 localExtra;       // x = local light count, y = sun rays per pixel
    glm::mat4 hfWorldToLocal;   // height field: landscape entity inverse world matrix
    glm::vec4 hfParams;         // x = 1/sizeX, y = 1/sizeZ, z = res-1, w = enabled
    glm::vec4 hfParams2;        // x = top mip level, y = max height, w = max march steps
};
static_assert(sizeof(GiShadowUBOData) == 13 * 16, "must match gi_shadow.comp's GiShadowUBO (the HW kernel declares only the first 7 rows)");
struct GiTemporalUBOData { glm::mat4 prevViewProj; glm::mat4 curViewProj; glm::vec4 blend; };
static_assert(sizeof(GiTemporalUBOData) == 2 * 64 + 16, "must match gi_temporal.frag's GiTemporalUBO");
struct GiProbeUBOData
{
    glm::vec4 gridOrigin, gridCounts, rayParams, sunDirRadius, sunColor, skyAmbient;
    glm::vec4 lightPosRange[8], lightColorType[8], lightDirCos[8];
};
static_assert(sizeof(GiProbeUBOData) == (6 + 24) * 16, "must match gi_probe.comp's GiProbeUBO");
struct GiReflUBOData
{
    glm::vec4 camPos, sunDir, sunColor, ambient, gridOrigin, gridCounts, reflParams, frame, rays;
    glm::vec4 lightPosRange[8], lightColorType[8], lightDirCos[8];
};
static_assert(sizeof(GiReflUBOData) == (9 + 24) * 16, "must match gi_refl.comp's GiReflUBO");
// The G-buffer position is the shadow-ray ORIGIN (pos + N*0.05). It holds the
// ABSOLUTE world position, so it must be fp32: as RGBA16F the ULP passes the
// 5 cm normal offset at |coord| >= ~100 m and every surface self-shadows in
// height bands (Thema 159). Every consumer point-samples/texelFetches it, so
// 32F filterability does not matter. Normals stay RGBA16F.
constexpr VkFormat kGiGBufPosFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
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
        bindOf(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT, &m_giPointSampler),
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
      && makePL(m_giFsDSL,     sizeof(HE::GIShadowAtrousStep), VK_SHADER_STAGE_FRAGMENT_BIT, m_giAtrousPL)
      && makePL(VK_NULL_HANDLE, sizeof(PushConstants), VK_SHADER_STAGE_VERTEX_BIT, m_giGBufPL);
    if (!ok)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI pipeline layouts failed"); return; }

    // ── Render passes ─────────────────────────────────────────────────────────
    // G-buffer: RGBA32F pos + RGBA16F normal (CLEAR → SHADER_READ_ONLY) + depth. The end
    // dependency covers FRAGMENT **and** COMPUTE consumers — the shadow-ray
    // KERNEL reads gPos/gNorm, unlike SSAO whose consumer is a fragment pass.
    {
        VkAttachmentDescription atts[3]{};
        for (int i = 0; i < 2; ++i)
        {
            atts[i].format         = (i == 0) ? kGiGBufPosFormat : VK_FORMAT_R16G16B16A16_SFLOAT;
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
        !makeFsRP(VK_FORMAT_R16_SFLOAT,          m_giAtrousRP))
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI fs render passes failed"); return; }

    // ── Shader modules ────────────────────────────────────────────────────────
    VkShaderModule gbufVS   = loadShaderModule("gi_gbuf.vert.spv");
    VkShaderModule gbufFS   = loadShaderModule("gi_gbuf.frag.spv");
    VkShaderModule fsVS     = loadShaderModule("postfx.vert.spv");
    VkShaderModule tempFS   = loadShaderModule("gi_temporal.frag.spv");
    VkShaderModule atrousFS = loadShaderModule("gi_atrous.frag.spv");
    VkShaderModule shadowCS = loadShaderModule("gi_shadow.comp.spv");
    VkShaderModule probeCS  = loadShaderModule("gi_probe.comp.spv");
    auto destroyModules = [&]()
    {
        for (auto m : { gbufVS, gbufFS, fsVS, tempFS, atrousFS, shadowCS, probeCS })
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!gbufVS || !gbufFS || !fsVS || !tempFS || !atrousFS || !shadowCS || !probeCS)
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
        pipesOk = pipesOk && createGraphicsPipelines(1, &pci, nullptr,
                                                       &m_giGBufPipe) == VK_SUCCESS;
        // Instanced twin for same-mesh runs, VS swapped. Kept out of pipesOk on
        // purpose: a failure here must not take GI down with it — every run
        // then loops through m_giGBufPipe.
        if (pipesOk)
            if (VkShaderModule gbufIVS = loadShaderModule("gi_gbuf_instanced.vert.spv"))
            {
                VkPipelineShaderStageCreateInfo istages[2] = { stages[0], stages[1] };
                istages[0].module = gbufIVS;
                InstancedDepthInput iin(k_instStride, 8); // {mvp, model}
                VkGraphicsPipelineCreateInfo ipci = pci;
                ipci.pStages           = istages;
                ipci.pVertexInputState = &iin.info;
                if (createGraphicsPipelines(1, &ipci, nullptr,
                                              &m_giGBufInstancedPipe) != VK_SUCCESS)
                    HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instanced GI G-buffer pipeline creation failed");
                vkDestroyShaderModule(m_device, gbufIVS, nullptr);
            }
    }
    // Temporal + a-trous (attribute-less fullscreen, no depth).
    VkPipelineVertexInputStateCreateInfo fsVI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo nods{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    auto makeFsPipe = [&](VkShaderModule fs, VkRenderPass rp, VkPipeline& out,
                          VkPipelineLayout layout) -> bool
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
        pci.layout = layout;           pci.renderPass = rp;
        return createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
    };
    pipesOk = pipesOk && makeFsPipe(tempFS,   m_giTemporalRP, m_giTemporalPipe, m_giFsPL);
    pipesOk = pipesOk && makeFsPipe(atrousFS, m_giAtrousRP,   m_giAtrousPipe,   m_giAtrousPL);
    // Compute kernels.
    auto makeCompute = [&](VkShaderModule cs, VkPipelineLayout pl, VkPipeline& out) -> bool
    {
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = cs;
        cpi.stage.pName  = "main";
        cpi.layout       = pl;
        return createComputePipelines(1, &cpi, nullptr, &out) == VK_SUCCESS;
    };
    pipesOk = pipesOk && makeCompute(shadowCS, m_giShadowPL, m_giShadowPipe);
    pipesOk = pipesOk && makeCompute(probeCS,  m_giProbePL,  m_giProbePipe);
    destroyModules();
    if (!pipesOk)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI pipeline creation failed — GI disabled"); return; }

    // ── Descriptor pool + per-in-flight-frame sets + params UBOs ─────────────
    {
        // Per frame: shadow, probe, temporal and one a-trous set per iteration.
        VkDescriptorPoolSize ps[4] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         k_maxFramesInFlight * 6 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight * 11 }, // shadow 2, temporal 3, a-trous 2x3
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          k_maxFramesInFlight * 4 }, // shadow raw+local, probe irr+vis
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 5 },
        };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets       = k_maxFramesInFlight * 5;
        dpci.poolSizeCount = 4;
        dpci.pPoolSizes    = ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_giDescPool) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI descriptor pool failed"); return; }
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        {
            VkDescriptorSetLayout layouts[5] = { m_giShadowDSL, m_giProbeDSL, m_giFsDSL, m_giFsDSL, m_giFsDSL };
            VkDescriptorSet sets[5]{};
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool     = m_giDescPool;
            dsai.descriptorSetCount = 5;
            dsai.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_device, &dsai, sets) != VK_SUCCESS)
            { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI descriptor sets failed"); return; }
            m_giShadowSet[i]    = sets[0];
            m_giProbeSet[i]     = sets[1];
            m_giTemporalSet[i]  = sets[2];
            m_giAtrousSet[i][0] = sets[3];
            m_giAtrousSet[i][1] = sets[4];
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
        return createComputePipelines(1, &cpi, nullptr, &out) == VK_SUCCESS;
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
    bool ok = makeImg(kGiGBufPosFormat, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giGBufPos)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giGBufNorm)
           && makeImg(m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT, m_giGBufDepth)
           && makeImg(VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, m_giRaw)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, m_giLocalMask)
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giHist[0])
           && makeImg(VK_FORMAT_R16G16B16A16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giHist[1])
           && makeImg(VK_FORMAT_R16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giResult)
           && makeImg(VK_FORMAT_R16_SFLOAT, kRT, VK_IMAGE_ASPECT_COLOR_BIT, m_giFilterTmp);
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI target creation failed");
        destroyGiTargets();
        return;
    }
    nameImage(m_giGBufPos.img,   "GI gbuffer position");
    nameImage(m_giGBufNorm.img,  "GI gbuffer normal");
    nameImage(m_giGBufDepth.img, "GI gbuffer depth");
    nameImage(m_giRaw.img,       "GI raw mask");
    nameImage(m_giLocalMask.img, "GI local mask");
    nameImage(m_giHist[0].img,   "GI history 0");
    nameImage(m_giHist[1].img,   "GI history 1");
    nameImage(m_giResult.img,    "GI mask");
    nameImage(m_giFilterTmp.img, "GI filter scratch");

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
            rci.renderPass = m_giAtrousRP; rci.attachmentCount = 1; rci.pAttachments = &m_giResult.view;
            rci.width = w; rci.height = h; rci.layers = 1;
            ok = vkCreateFramebuffer(m_device, &rci, nullptr, &m_giResultFB) == VK_SUCCESS;
            rci.pAttachments = &m_giFilterTmp.view;
            ok = ok && vkCreateFramebuffer(m_device, &rci, nullptr, &m_giFilterTmpFB) == VK_SUCCESS;
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
        submitGraphics(si, VK_NULL_HANDLE);
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
    if (m_giFilterTmpFB) { vkDestroyFramebuffer(m_device, m_giFilterTmpFB, nullptr); m_giFilterTmpFB = VK_NULL_HANDLE; }
    destroy(m_giGBufPos); destroy(m_giGBufNorm); destroy(m_giGBufDepth);
    destroy(m_giRaw); destroy(m_giLocalMask); destroy(m_giHist[0]); destroy(m_giHist[1]); destroy(m_giResult);
    destroy(m_giFilterTmp);
    m_giW = m_giH = 0;
    m_giHistValid = false;
}

// Probe-grid fit over the scene AABB (GIProbeGrid.h: the spacing grows so the
// whole scene — a 100 m terrain included — fits the probe budget).
// m_renderWorld was extracted by runGi() with real mesh bounds refreshed (same
// lesson as GL/Metal: the extractor seeds only fallback unit cubes). Once
// built, the fit is only re-checked when the scene's geometry changed
// (signature or an InvalidateMesh), and replaced when the scene left it.
void VulkanRenderer::ensureGiProbeGrid()
{
    if (m_renderWorld.objects.empty()) return;
    const uint64_t sig = HE::GIProbeSceneSignature(m_renderWorld.objects);
    if (m_giGridTrack.canSkip(m_giProbeGridBuilt, sig)) return;

    int unresolved = 0;
    const HE::AABB sceneBox = HE::GIProbeSceneBounds(m_renderWorld.objects, &unresolved);
    if (!sceneBox.isValid()) return;
    if (!m_giGridTrack.shouldEvaluate(m_giProbeGridBuilt, sig, unresolved)) return;

    if (m_giProbeGridBuilt)
    {
        HE::GIProbeGridFit current;
        current.origin = m_giGridOrigin; current.counts = m_giGridCounts; current.spacing = m_giProbeSpacing;
        if (!HE::GIProbeGridNeedsRefit(current, sceneBox)) return;
        // New counts → new atlas. Frames in flight still sample the old one
        // and every slot's scene set points at it: drain the GPU (refits are
        // rare), free it, and park bindings 5/6 on the white fallback until
        // runGi() rewrites them for the slot it records. This frame's command
        // buffer has not touched the atlas yet — runGi() calls us first.
        deviceWaitIdle();
        destroyGiProbeAtlas();
        VkDescriptorImageInfo white{ m_ssaoSampler, m_ssaoWhiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
        {
            if (!m_frameUBO[i].set || !m_ssaoWhiteView) continue;
            VkWriteDescriptorSet ws[2]{};
            for (uint32_t b = 0; b < 2; ++b)
            {
                ws[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                ws[b].dstSet          = m_frameUBO[i].set;
                ws[b].dstBinding      = 5 + b;   // irradiance, visibility atlas
                ws[b].descriptorCount = 1;
                ws[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ws[b].pImageInfo      = &white;
            }
            vkUpdateDescriptorSets(m_device, 2, ws, 0, nullptr);
        }
    }
    const HE::GIProbeGridFit fit = HE::FitGIProbeGrid(sceneBox);
    if (!fit.valid()) return;

    m_giGridCounts     = fit.counts;
    m_giGridOrigin     = fit.origin;
    m_giProbeSpacing   = fit.spacing;
    m_giProbeCount     = fit.probeCount();
    m_giProbesPerRow   = std::min(m_giProbeCount, 32);
    m_giProbeCursor    = 0;
    m_giProbeGridBuilt = true;
    HE_LOG_INFO(RHI, "%s",
                ("VulkanRenderer: GI probe grid " + std::to_string(m_giGridCounts.x) + "x"
                 + std::to_string(m_giGridCounts.y) + "x" + std::to_string(m_giGridCounts.z)
                 + " (" + std::to_string(m_giProbeCount) + " probes), spacing "
                 + std::to_string(m_giProbeSpacing)).c_str());
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
    nameImage(m_giIrrAtlas.img, "GI irradiance atlas");
    nameImage(m_giVisAtlas.img, "GI visibility atlas");

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
    submitGraphics(si, VK_NULL_HANDLE);
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
    GpuPassScope _passTimer(this, "GI");
    // Reflections trace the same BVH and read the G-buffer prepass this
    // function also builds, so a reflections-only session (diffuse GI off)
    // still needs it to run — same gate as updateGiAccel(). Step 7 below runs
    // only `m_giReflEnabled`'s chain; steps 2-6 (shadow mask / probe update)
    // stay inside `if (m_giEnabled)`.
    if (!(m_giEnabled || m_giReflEnabled) || !m_world) return;
    createGiPipelines();
    if (!m_giReady) return;

    // Extract with the scene pass's aspect (Metal lesson 5846efc: a mismatched
    // camera misaligns the screen-space mask → swimming shadows).
    const float aspect = w > 0 && h > 0 ? float(w) / float(h) : 1.0f;
    // This frame's sun comes from the setDayNight() at the top of Render() /
    // RenderSceneImage(): runGi() runs before DrawScene(), which used to be the
    // only place feeding it, so the GI mask traced against the previous
    // frame's sun (Thema 131).
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;
    refineObjectBounds();
    // The extractor leaves baseColor at white, and this extraction throws away
    // DrawScene's resolve (which runs later anyway). Without it every GI
    // instance bounced white: a red and a grey floor gave the same probe field,
    // colour bleed exactly 0 (Thema 154). D3D11/D3D12 resolve right before
    // their updateGiAccel as well.
    { HE_PROFILE_SCOPE_N("MaterialScalars"); HE::resolveWorldMaterialScalars(m_renderWorld, m_contentManager); }

    updateGiAccel();
    flushGiHwBlasBuilds(cmd);   // BLAS created above get built in this frame's command buffer
    if (m_giInstanceCount == 0) return;

    // HW ray tracing: record this frame's TLAS build (+ build→compute barrier)
    // up front. false → software kernels this frame (BVH SSBOs are in place).
    const bool useHw = buildGiTlas(cmd);

    const uint32_t gw = std::max(1u, w / 2), gh = std::max(1u, h / 2); // half-res like GL/Metal
    createGiTargets(gw, gh);
    if (!m_giGBufFB || !m_giResultFB || !m_giFilterTmpFB) return;
    ensureGiProbeGrid();
    if (m_giProbeGridBuilt) ensureGiProbeAtlas();

    cullCameraObjects();
    if (m_sortedIndices.empty()) return;

    const glm::mat4 vp = HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;
    const uint32_t  fi = m_currentFrame;
    const int curIdx = m_giHistIdx, prevIdx = 1 - curIdx;

    glm::vec3 towardLight, lightColorIntensity;
    m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);

    // ── Params UBOs (host-visible ring slot for this in-flight frame) ────────
    m_giFrameSeed = HE::NextGIJitterSeed(m_giFrameSeed); // wraps: GIJitter.h
    GiShadowUBOData shadowUbo{};
    shadowUbo.sunDirRadius = glm::vec4(towardLight, glm::radians(m_giLightRadius));
    // The software kernel's own instance list (without the terrain chunks) while the height field stands in for them.
    const bool sepOn = !useHw && m_giShadowSep;
    const bool hfOn  = sepOn && m_giHfActive && m_giHfView && !m_renderWorld.heightfields.empty();
    shadowUbo.frame        = glm::vec4(m_giFrameSeed, float(gw), float(gh),
                                       float(sepOn ? m_giShadowInstanceCount : m_giInstanceCount));
    {
        const GiHeightfield* h0 = m_renderWorld.heightfields.empty() ? nullptr : &m_renderWorld.heightfields[0];
        shadowUbo.hfWorldToLocal = h0 ? h0->worldToLocal : glm::mat4(1.0f);
        shadowUbo.hfParams  = glm::vec4(h0 ? 1.0f / std::max(h0->size.x, 1e-4f) : 1.0f, h0 ? 1.0f / std::max(h0->size.y, 1e-4f) : 1.0f,
                                        h0 ? float(h0->res - 1) : 1.0f, hfOn ? 1.0f : 0.0f);
        shadowUbo.hfParams2 = glm::vec4(float(m_giHfTopLevel), m_giHfMaxH, 0.0f, 512.0f);
    }
    // First 4 local (point/spot) lights of the same 8-light window the scene
    // shader iterates — scene.frag counts non-directional lights in the SAME
    // order to index the mask channels, which is exactly what the shared packing
    // reproduces.
    {
        const HE::PackedLocalShadowLights ml = HE::BuildMaskedLocalLights(m_renderWorld);
        static_assert(sizeof(shadowUbo.localPosRange) == sizeof(ml.posRange),
                      "gi_shadow.comp's local light slots must match HE::kMaxMaskedLocalLights");
        std::memcpy(shadowUbo.localPosRange, ml.posRange, sizeof(shadowUbo.localPosRange));
        shadowUbo.localExtra = glm::vec4(float(ml.count), float(m_giShadowRays), 0.0f, 0.0f);
    }
    if (!uploadGiBuffer(m_giShadowUBO[fi], &shadowUbo, sizeof(shadowUbo),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;

    GiTemporalUBOData tempUbo{};
    tempUbo.prevViewProj = m_giPrevViewProj;
    tempUbo.curViewProj  = vp; // clip-fixed like prevViewProj; becomes it below
    tempUbo.blend        = glm::vec4(m_giHistValid ? m_giShadowHistoryWeight : 0.0f, 0.0f, 0.0f, 0.0f);
    if (!uploadGiBuffer(m_giTemporalUBO[fi], &tempUbo, sizeof(tempUbo),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return;

    const bool probesActive = m_giProbeGridBuilt && m_giIrrAtlas.img && m_giVisAtlas.img;
    const int  probeBudget  = probesActive
        ? std::min(m_giProbeBudgetPerFrame > 0 ? m_giProbeBudgetPerFrame : 1, m_giProbeCount) : 0;
    if (probesActive)
    {
        GiProbeUBOData pu{};
        pu.gridOrigin = glm::vec4(m_giGridOrigin, m_giProbeSpacing);
        pu.gridCounts = glm::vec4(float(m_giGridCounts.x), float(m_giGridCounts.y),
                                  float(m_giGridCounts.z), float(m_giProbesPerRow));
        const float maxDist = glm::length(glm::vec3(m_giGridCounts) * m_giProbeSpacing) + m_giProbeSpacing;
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
        VkDescriptorBufferInfo shadowInstBI { m_giShadowInstanceBuf[fi].buf, 0, VK_WHOLE_SIZE };
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
        wBuf(shadowSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sepOn ? &shadowInstBI : &instBI);
        wImg(shadowSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posBI);
        wImg(shadowSet, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normBI);
        wImg(shadowSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &rawSI);
        wBuf(shadowSet, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &shUboBI);
        if (useHw) wTlas(shadowSet, 7);
        VkDescriptorImageInfo localSI{ VK_NULL_HANDLE, m_giLocalMask.view, VK_IMAGE_LAYOUT_GENERAL };
        wImg(shadowSet, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &localSI);
        // Binding 9 exists on the software kernel's layout only; off, any valid float view fills it (never fetched).
        VkDescriptorImageInfo hfSI{ VK_NULL_HANDLE, hfOn ? m_giHfView : m_giGBufPos.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        if (!useHw) wImg(shadowSet, 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hfSI);
        // Temporal set: gPos, raw (GENERAL), history[prev], UBO.
        VkDescriptorImageInfo rawSamp{ VK_NULL_HANDLE, m_giRaw.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorImageInfo histBI { VK_NULL_HANDLE, m_giHist[prevIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo tUboBI{ m_giTemporalUBO[fi].buf, 0, sizeof(GiTemporalUBOData) };
        wImg(m_giTemporalSet[fi], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posBI);
        wImg(m_giTemporalSet[fi], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rawSamp);
        wImg(m_giTemporalSet[fi], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &histBI);
        wBuf(m_giTemporalSet[fi], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &tUboBI);
        // A-trous sets: source (iteration 0 history[cur], iteration 1 the
        // scratch), gPos, gNorm; the UBO binding is unused, valid filler.
        VkDescriptorImageInfo histCurBI{ VK_NULL_HANDLE, m_giHist[curIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo tmpBI    { VK_NULL_HANDLE, m_giFilterTmp.view,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        for (int it = 0; it < HE::kGIShadowAtrousIterations; ++it)
        {
            wImg(m_giAtrousSet[fi][it], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, it == 0 ? &histCurBI : &tmpBI);
            wImg(m_giAtrousSet[fi][it], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posBI);
            wImg(m_giAtrousSet[fi][it], 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normBI);
            wBuf(m_giAtrousSet[fi][it], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &tUboBI);
        }
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
    // Runs of consecutive same-mesh objects (precip/particles don't shade the
    // mask — AoContributors); a run of more than one is ONE instanced draw with
    // the push-constant path's {mvp, model} per instance.
    m_preBatches.useSpans = RenderSorter::instanceSpansEnabled();   // read in place, see InstanceRun
    RenderSorter::batchDepthRuns(m_renderWorld, m_sortedIndices,
                                 RenderSorter::DepthFilter::AoContributors, kNoOwnerEntity, m_preBatches);
    for (const RenderSorter::DepthBatch& b : m_preBatches.batches)
    {
        const GpuMesh* mesh = resolveMesh(b.meshAssetId);
        const GpuMesh& gm   = mesh ? *mesh : m_cube;
        if (!gm.indexCount) continue;
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbuf, &offset);
        vkCmdBindIndexBuffer(cmd, gm.ibuf, 0, VK_INDEX_TYPE_UINT32);
        const InstanceRun run = m_preBatches.run(b);
        if (drawDepthInstanced(cmd, m_giGBufInstancedPipe, m_giGBufPipe, gm.indexCount, run,
                [&](const glm::mat4& model, glm::mat4* pair) {
                    pair[0] = vp * model;
                    pair[1] = model;
                }))
        {
            static bool loggedOnce = false;
            if (!loggedOnce)
            {
                loggedOnce = true;
                HE_LOG_INFO(RHI, "VulkanRenderer: GI pre-pass instanced (first batch: %u instances)", b.count);
            }
            continue;
        }
        run.forEach([&](const glm::mat4& model)
        {
            // gi_gbuf.vert: uMVP (clip-fixed) + uModel — same 128-byte shape.
            PushConstants pc{ vp * model, model };
            vkCmdPushConstants(cmd, m_giGBufPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
        });
    }
    vkCmdEndRenderPass(cmd); // colors → SHADER_READ_ONLY (dep covers COMPUTE too)

    // ── 2-6. Shadow mask + DDGI probe update: diffuse GI only. A reflections-
    // only frame (m_giEnabled false) skips straight to step 7 below with just
    // the G-buffer prepass this frame produced.
    if (m_giEnabled)
    {
    // ── 2. Shadow rays (compute; HW ray-query kernel when the TLAS is up) ───
    _passTimer.next("GIShadow");
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
    _passTimer.next("GIFilter");
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

    // ── 4. Edge-aware a-trous (fullscreen: hist[cur] → scratch → result) ────
    // The render pass's external dependencies order the scratch write before
    // the second iteration's read (COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER).
    static_assert(HE::kGIShadowAtrousIterations == 2, "scratch → result ping assumes two iterations");
    for (int it = 0; it < HE::kGIShadowAtrousIterations; ++it)
    {
        VkRenderPassBeginInfo bRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bRPBI.renderPass        = m_giAtrousRP;
        bRPBI.framebuffer       = it == 0 ? m_giFilterTmpFB : m_giResultFB;
        bRPBI.renderArea.extent = { gw, gh };
        vkCmdBeginRenderPass(cmd, &bRPBI, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giAtrousPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giAtrousPL,
                                0, 1, &m_giAtrousSet[fi][it], 0, nullptr);
        const HE::GIShadowAtrousStep step = HE::GIShadowAtrousParams(it, m_giShadowFilter,
                                                                     m_giShadowHistoryWeight, m_giShadowRays);
        vkCmdPushConstants(cmd, m_giAtrousPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(step), &step);
        vkCmdSetViewport(cmd, 0, 1, &vvp);
        vkCmdSetScissor(cmd, 0, 1, &vsc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // ── 5. Probe update (compute, frame-sliced round robin) ──────────────────
    if (probesActive && probeBudget > 0)
    {
        _passTimer.next("GIProbes");
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
    } // if (m_giEnabled)

    // ── 7. GI reflections (trace + blur + roughness mix) ────────────────────
    // Runs with or without the diffuse GI; `probesActive` (computed above,
    // independent of m_giEnabled — the atlases are zero-filled at creation,
    // see ensureGiProbeAtlas) gates the indirect term the same way D3D12's
    // `probesValid` parameter does.
    if (m_giReflEnabled)
    {
        _passTimer.next("GIRefl");
        runGiReflections(cmd, probesActive, useHw);
    }
}

// Builds the GI-reflection pipelines + layouts + render pass once (first
// reflections-active frame). Deliberately OUTSIDE createGiPipelines()'s
// ok-chain and with its OWN descriptor pool/DSLs: a failure here costs only
// the reflections, never the shadow mask / probe GI. Requires the diffuse GI's
// point sampler (createGiPipelines must have run and succeeded — runGi()
// already gates on m_giReady before ever reaching step 7), same dependency
// D3D12's createGiReflPipelines has on giComputeRS/giFsRS.
void VulkanRenderer::createGiReflPipelines()
{
    if (m_giReflPipesTried) return;
    m_giReflPipesTried = true;
    if (!m_giPointSampler) return;

    auto makeDSL = [&](const std::vector<VkDescriptorSetLayoutBinding>& binds,
                       VkDescriptorSetLayout& out) -> bool
    {
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = static_cast<uint32_t>(binds.size());
        slci.pBindings    = binds.data();
        return vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &out) == VK_SUCCESS;
    };
    auto bindOf = [&](uint32_t b, VkDescriptorType t, VkShaderStageFlags st)
    {
        VkDescriptorSetLayoutBinding r{};
        r.binding = b; r.descriptorType = t; r.descriptorCount = 1; r.stageFlags = st;
        return r;
    };
    // gi_refl.comp: 0-2 SSBOs, 3-6 samplers (gPos/gNorm/gIIrr/gIVis), 7 the
    // trace's storage image, 8 the UBO, 9 the landscape table (SSBO), 10 the
    // four landscape weightmaps (a sampler array).
    VkDescriptorSetLayoutBinding landWeights = bindOf(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT);
    landWeights.descriptorCount = 4;
    bool ok = makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),
        bindOf(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_SHADER_STAGE_COMPUTE_BIT),   // landscape table
        landWeights,                                                                          // 10: 4 weightmaps
        // 11 stays free (the HW layout's TLAS); 12: the sky cube a missed ray reads (gi_refl*.comp, uRays.z gates it)
        bindOf(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    }, m_giReflDSL);
    // gi_refl_blur.frag / gi_refl_mix.frag: 3 samplers (blur uses only binding
    // 0; shared layout, unused bindings hold valid views — same convention as
    // gi_atrous.frag reusing gi_temporal.frag's DSL).
    ok = ok && makeDSL({
        bindOf(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
        bindOf(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
        bindOf(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    }, m_giReflFsDSL);
    if (!ok)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection descriptor layouts failed"); return; }

    VkPipelineLayoutCreateInfo plci0{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci0.setLayoutCount = 1; plci0.pSetLayouts = &m_giReflDSL;
    ok = vkCreatePipelineLayout(m_device, &plci0, nullptr, &m_giReflPL) == VK_SUCCESS;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4) };
    VkPipelineLayoutCreateInfo plci1{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci1.setLayoutCount = 1; plci1.pSetLayouts = &m_giReflFsDSL;
    plci1.pushConstantRangeCount = 1; plci1.pPushConstantRanges = &pcr;
    ok = ok && vkCreatePipelineLayout(m_device, &plci1, nullptr, &m_giReflFsPL) == VK_SUCCESS;
    if (!ok)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection pipeline layouts failed"); return; }

    // Render pass for the blur/mix fullscreen passes: RGBA16F, DONT_CARE →
    // SHADER_READ_ONLY (same shape as createGiPipelines' local makeFsRP).
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
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
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_giReflFsRP) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection render pass failed"); return; }
    }

    VkShaderModule reflCS = loadShaderModule("gi_refl.comp.spv");
    VkShaderModule fsVS   = loadShaderModule("postfx.vert.spv");
    VkShaderModule blurFS = loadShaderModule("gi_refl_blur.frag.spv");
    VkShaderModule mixFS  = loadShaderModule("gi_refl_mix.frag.spv");
    auto destroyModules = [&]()
    {
        for (auto m : { reflCS, fsVS, blurFS, mixFS })
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!reflCS || !fsVS || !blurFS || !mixFS)
    {
        HE_LOG_WARN(RHI, "%s", "VulkanRenderer: GI reflection shaders missing — reflections disabled");
        m_giReflWhyNot = "The reflection shaders (gi_refl*.spv) are missing from the Shaders folder.";
        destroyModules();
        return;
    }

    VkPipelineVertexInputStateCreateInfo fsVI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo nods{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                         VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    bool pipesOk = true;
    auto makeFsPipe = [&](VkShaderModule fs, VkPipeline& out) -> bool
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
        pci.pColorBlendState = &cb;    pci.pDynamicState = &dyn;
        pci.layout = m_giReflFsPL;     pci.renderPass = m_giReflFsRP;
        return createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
    };
    pipesOk = pipesOk && makeFsPipe(blurFS, m_giReflBlurPipe);
    pipesOk = pipesOk && makeFsPipe(mixFS,  m_giReflMixPipe);
    if (pipesOk)
    {
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = reflCS;
        cpi.stage.pName  = "main";
        cpi.layout       = m_giReflPL;
        pipesOk = createComputePipelines(1, &cpi, nullptr, &m_giReflPipe) == VK_SUCCESS;
    }
    destroyModules();
    if (!pipesOk)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection pipeline creation failed — reflections disabled"); return; }

    // ── Descriptor pool: per-in-flight-frame compute set + 4 fixed fullscreen
    // "tables" (raw/ping/blur/mix — rewritten only by ensureGiReflTargets, not
    // per frame; only the push-constant stride/floor changes between draws).
    VkDescriptorPoolSize ps[4] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         k_maxFramesInFlight * 4 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_maxFramesInFlight * 9 + 4 * 3 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          k_maxFramesInFlight * 1 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         k_maxFramesInFlight * 1 },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets       = k_maxFramesInFlight + 4;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes    = ps;
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_giReflDescPool) != VK_SUCCESS)
    { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection descriptor pool failed"); return; }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_giReflDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &m_giReflDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_giReflSet[i]) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection compute set failed"); return; }
    }
    {
        VkDescriptorSetLayout layouts[4] = { m_giReflFsDSL, m_giReflFsDSL, m_giReflFsDSL, m_giReflFsDSL };
        VkDescriptorSet sets[4]{};
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_giReflDescPool;
        dsai.descriptorSetCount = 4;
        dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(m_device, &dsai, sets) != VK_SUCCESS)
        { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection table sets failed"); return; }
        m_giReflTabRaw = sets[0]; m_giReflTabPing = sets[1];
        m_giReflTabBlur = sets[2]; m_giReflTabMix = sets[3];
    }

    // Per-in-flight-frame CB (host-visible, persistently mapped); runGiReflections
    // overwrites it every frame before use, this just needs a valid allocation.
    {
        const GiReflUBOData zero{};
        for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
            if (!uploadGiBuffer(m_giReflUBO[i], &zero, sizeof(zero), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            { HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: GI reflection UBO failed"); return; }
    }

    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: GI reflection pipelines built");
    createGiReflHwPipeline();
}

// gi_refl_hw.comp: the SW reflection layout (bindings 0-10) + the TLAS at 11. Own pool and sets. Any failure leaves
// m_giReflHwPipe null and the reflections on the SW kernel. HE_GI_REFL_HW=0 keeps the SW kernel on purpose (A/B).
void VulkanRenderer::createGiReflHwPipeline()
{
    if (!m_giHwRt || !m_giHwPipesReady || !m_giReflPipe) return;
    if (const char* v = std::getenv("HE_GI_REFL_HW"); v && v[0] == '0')
    {
        HE_LOG_INFO(RHI, "%s", "VulkanRenderer: HE_GI_REFL_HW=0 — GI reflections stay on the software kernel");
        return;
    }
    auto fail = [&](const char* what)
    {
        HE_LOG_WARN(RHI, "%s", (std::string("VulkanRenderer: ") + what + " — GI reflections stay on the software kernel").c_str());
        if (m_giReflHwPipe) { vkDestroyPipeline(m_device, m_giReflHwPipe, nullptr);         m_giReflHwPipe = VK_NULL_HANDLE; }
        if (m_giReflHwPL)   { vkDestroyPipelineLayout(m_device, m_giReflHwPL, nullptr);     m_giReflHwPL   = VK_NULL_HANDLE; }
        if (m_giReflHwDSL)  { vkDestroyDescriptorSetLayout(m_device, m_giReflHwDSL, nullptr); m_giReflHwDSL = VK_NULL_HANDLE; }
        if (m_giReflHwPool) { vkDestroyDescriptorPool(m_device, m_giReflHwPool, nullptr);   m_giReflHwPool = VK_NULL_HANDLE; }
        for (auto& hs : m_giReflHwSet) hs = VK_NULL_HANDLE;
    };
    auto bindOf = [&](uint32_t b, VkDescriptorType t, uint32_t count = 1)
    {
        VkDescriptorSetLayoutBinding r{};
        r.binding = b; r.descriptorType = t; r.descriptorCount = count; r.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        return r;
    };
    const std::vector<VkDescriptorSetLayoutBinding> binds = {
        bindOf(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        bindOf(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        bindOf(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        bindOf(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        bindOf(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        bindOf(8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        bindOf(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        bindOf(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4),
        bindOf(11, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
        bindOf(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),   // the sky cube
    };
    VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = static_cast<uint32_t>(binds.size());
    slci.pBindings    = binds.data();
    if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_giReflHwDSL) != VK_SUCCESS)
    { fail("GI HW reflection descriptor layout failed"); return; }
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &m_giReflHwDSL;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_giReflHwPL) != VK_SUCCESS)
    { fail("GI HW reflection pipeline layout failed"); return; }
    VkShaderModule cs = loadShaderModule("gi_refl_hw.comp.spv");
    if (!cs) { fail("gi_refl_hw.comp.spv missing"); return; }
    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = cs;
    cpi.stage.pName  = "main";
    cpi.layout       = m_giReflHwPL;
    const bool pipeOk = createComputePipelines(1, &cpi, nullptr, &m_giReflHwPipe) == VK_SUCCESS;
    vkDestroyShaderModule(m_device, cs, nullptr);
    if (!pipeOk) { fail("GI HW reflection pipeline creation failed"); return; }
    VkDescriptorPoolSize ps[5] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             k_maxFramesInFlight * 4 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     k_maxFramesInFlight * 9 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              k_maxFramesInFlight * 1 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             k_maxFramesInFlight * 1 },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, k_maxFramesInFlight * 1 },
    };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = k_maxFramesInFlight; dpci.poolSizeCount = 5; dpci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_giReflHwPool) != VK_SUCCESS)
    { fail("GI HW reflection descriptor pool failed"); return; }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool = m_giReflHwPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_giReflHwDSL;
        if (vkAllocateDescriptorSets(m_device, &dsai, &m_giReflHwSet[i]) != VK_SUCCESS)
        { fail("GI HW reflection descriptor set failed"); return; }
    }
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: GI HW reflection kernel built (VK_KHR_ray_query)");
}

// Result chain at the prepass' size (m_giW/m_giH — the GL/D3D gap, resDivUsed
// = 2: the prepass is half the screen). Rebuilt whenever the prepass resized;
// every descriptor naming it is rewritten here.
void VulkanRenderer::ensureGiReflTargets()
{
    if (m_giReflRaw.img && m_giReflW == static_cast<int>(m_giW) && m_giReflH == static_cast<int>(m_giH)) return;
    if (!m_giGBufPos.view || !m_giGBufNorm.view) return;
    deviceWaitIdle();
    destroyGiReflTargets();
    m_giReflW = static_cast<int>(m_giW);
    m_giReflH = static_cast<int>(m_giH);
    if (m_giReflW <= 0 || m_giReflH <= 0) return;

    auto makeImg = [&](VkImageUsageFlags usage, GiImage& out) -> bool
    {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = { static_cast<uint32_t>(m_giReflW), static_cast<uint32_t>(m_giReflH), 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
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
        vci.image = out.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return vkCreateImageView(m_device, &vci, nullptr, &out.view) == VK_SUCCESS;
    };
    const VkImageUsageFlags kRT = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = makeImg(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_giReflRaw)
           && makeImg(kRT, m_giReflPing)
           && makeImg(kRT, m_giReflBlur)
           && makeImg(kRT, m_giReflFinal);
    if (!ok) { destroyGiReflTargets(); return; }
    nameImage(m_giReflRaw.img,   "GI reflection raw trace");
    nameImage(m_giReflPing.img,  "GI reflection blur ping");
    nameImage(m_giReflBlur.img,  "GI reflection blur");
    nameImage(m_giReflFinal.img, "GI reflection final (roughness mix)");

    auto makeFB = [&](VkImageView view, VkFramebuffer& fb) -> bool
    {
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_giReflFsRP; fci.attachmentCount = 1; fci.pAttachments = &view;
        fci.width = static_cast<uint32_t>(m_giReflW); fci.height = static_cast<uint32_t>(m_giReflH); fci.layers = 1;
        return vkCreateFramebuffer(m_device, &fci, nullptr, &fb) == VK_SUCCESS;
    };
    ok = makeFB(m_giReflPing.view,  m_giReflPingFB)
      && makeFB(m_giReflBlur.view,  m_giReflBlurFB)
      && makeFB(m_giReflFinal.view, m_giReflFinalFB);
    if (!ok) { destroyGiReflTargets(); return; }

    // Raw → GENERAL for its first write (it ping-pongs every frame after
    // this: GENERAL for the compute write, SHADER_READ_ONLY_OPTIMAL for the
    // blur/mix draws' read — runGiReflections transitions it both ways each
    // frame, tracked in m_giReflRawLayout).
    {
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_device, &cbai, &tmp);
        VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(tmp, &cbi);
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_giReflRaw.img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(tmp);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
        submitGraphics(si, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
        m_giReflRawLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Fixed fullscreen "tables" (D3D12's SRV-table analogue): binding 0 = the
    // draw's own source, 1 = a second source (mix only), 2 = gNorm (roughness,
    // the mix shader's only real user — blur/raw/ping/blur tables carry it
    // only as valid filler, same convention as the shadow pass's shared DSL).
    // All three read in SHADER_READ_ONLY_OPTIMAL: the compute pass's own
    // separate descriptor set (m_giReflSet) is what samples/writes raw in
    // GENERAL; by the time any of these fullscreen draws run, runGiReflections
    // has already transitioned raw to SHADER_READ_ONLY_OPTIMAL (see its step-1
    // barrier), same as ping/blur right after their own draw's render pass
    // (finalLayout = SHADER_READ_ONLY_OPTIMAL on m_giReflFsRP).
    auto writeTable = [&](VkDescriptorSet set, VkImageView a, VkImageView b)
    {
        VkDescriptorImageInfo ii[3] = {
            { m_giPointSampler, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { m_giPointSampler, b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { m_giPointSampler, m_giGBufNorm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        };
        VkWriteDescriptorSet w[3]{};
        for (int k = 0; k < 3; ++k)
        {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = set; w[k].dstBinding = static_cast<uint32_t>(k);
            w[k].descriptorCount = 1; w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(m_device, 3, w, 0, nullptr);
    };
    writeTable(m_giReflTabRaw,  m_giReflRaw.view,  m_giReflRaw.view);
    writeTable(m_giReflTabPing, m_giReflPing.view, m_giReflPing.view);
    writeTable(m_giReflTabBlur, m_giReflBlur.view, m_giReflBlur.view);
    writeTable(m_giReflTabMix,  m_giReflRaw.view,  m_giReflBlur.view);
    m_giReflReady = false; // fresh targets → nothing produced yet this session
}

void VulkanRenderer::destroyGiReflTargets()
{
    auto destroy = [&](GiImage& g)
    {
        if (g.view) { vkDestroyImageView(m_device, g.view, nullptr); g.view = VK_NULL_HANDLE; }
        if (g.img)  { vkDestroyImage(m_device, g.img, nullptr);      g.img  = VK_NULL_HANDLE; }
        if (g.mem)  { vkFreeMemory(m_device, g.mem, nullptr);        g.mem  = VK_NULL_HANDLE; }
    };
    if (m_giReflPingFB)  { vkDestroyFramebuffer(m_device, m_giReflPingFB, nullptr);  m_giReflPingFB  = VK_NULL_HANDLE; }
    if (m_giReflBlurFB)  { vkDestroyFramebuffer(m_device, m_giReflBlurFB, nullptr);  m_giReflBlurFB  = VK_NULL_HANDLE; }
    if (m_giReflFinalFB) { vkDestroyFramebuffer(m_device, m_giReflFinalFB, nullptr); m_giReflFinalFB = VK_NULL_HANDLE; }
    destroy(m_giReflRaw); destroy(m_giReflPing); destroy(m_giReflBlur); destroy(m_giReflFinal);
    m_giReflW = m_giReflH = 0;
    m_giReflReady = false;
    m_giReflRawLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// Trace + separable confidence-weighted blur (a-trous chain) + roughness mix.
// Needs the G-buffer prepass (this frame's gi_gbuf pass, step 1 of runGi) and
// the BVH buffers; the probe atlases only when `probesValid`. Leaves
// m_giReflFinal in SHADER_READ_ONLY_OPTIMAL. true = it holds this frame's result.
bool VulkanRenderer::runGiReflections(VkCommandBuffer cmd, bool probesValid, bool useHw)
{
    createGiReflPipelines();
    m_giReflReady = false;
    if (!m_giReflPipe || !m_giReflBlurPipe || !m_giReflMixPipe)
    {
        // createGiReflPipelines already logged which step failed; the shader-missing case
        // wrote its own text. Anything else gets the general one.
        if (m_giReflWhyNot.empty())
            m_giReflWhyNot = "The reflection pipelines could not be created on this GPU/driver (see HorizonEngine.log).";
        return false;
    }
    ensureGiReflTargets();
    if (!m_giReflRaw.img || !m_giNodeBuf.buf || !m_giTriBuf.buf || m_giInstanceCount == 0) return false;

    const uint32_t fi = m_currentFrame;
    if (!m_giReflSet[fi] || !m_giReflInstanceBuf[fi].buf || !m_giLandBuf[fi].buf) return false;
    const HE::GIReflTier tier = HE::giReflTierFor(m_giReflQuality, m_giReflBlurEnabled);
    // The ray-query kernel when this frame's TLAS is up (it holds the same instances in the same order as the
    // reflection instance list); otherwise the software kernel over the BVH SSBOs.
    const bool hwRefl = useHw && m_giReflHwPipe && m_giReflHwSet[fi] && m_giTlas[fi].as;
    const VkDescriptorSet reflSet = hwRefl ? m_giReflHwSet[fi] : m_giReflSet[fi];
    // The real sky for rays that miss (clouds, weather): six small faces drawn before the trace reads them.
    renderSkyReflCube(cmd);

    // ── Rewrite this frame slot's compute set (fence already waited) ────────
    GiReflUBOData cb{};
    glm::vec3 towardLight, lightColorIntensity;
    m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
    static_assert(HE::kMaxLightWindow == 8, "cb light arrays hold 8 entries");
    const HE::PackedLightArray lights = HE::BuildPackedLightArray(m_renderWorld);
    for (int i = 0; i < HE::kMaxLightWindow; ++i)
    {
        cb.lightPosRange[i]  = lights.posRange[i];
        cb.lightColorType[i] = lights.colorType[i];
        cb.lightDirCos[i]    = lights.dirCos[i];
    }
    m_giReflFrameSeed = HE::NextGIJitterSeed(m_giReflFrameSeed);
    const bool irrValid = probesValid && m_giIrrAtlas.view && m_giVisAtlas.view;
    cb.camPos     = glm::vec4(m_renderWorld.camera.position, 0.0f);
    cb.sunDir     = glm::vec4(towardLight, static_cast<float>(lights.count));
    cb.sunColor   = glm::vec4(lightColorIntensity, 0.0f);
    cb.ambient    = glm::vec4(m_renderWorld.ambient, 0.0f);
    cb.gridOrigin = glm::vec4(m_giGridOrigin, m_giProbeSpacing);
    cb.gridCounts = glm::vec4(glm::vec3(m_giGridCounts), static_cast<float>(m_giProbesPerRow));
    // The ray-query kernel pays next to nothing for a long ray (GIRefl 0.02 ms at 200, 1500 and 4000 m on the RTX 4070),
    // and the 200 m the software kernels need to stay cheap cut a hill 300 m away in half in a lake's mirror and left
    // the far shore and horizon out of every water reflection: what lies past the limit fell back to the sky cube,
    // which has no terrain in it. So the HW kernel traces at least kGiHwReflMinDistance; the confidence fade
    // (0.75..1 of the distance) then happens far out, over the sky.
    constexpr float kGiHwReflMinDistance = 4000.0f;
    const float reflDistance = hwRefl ? std::max(m_giReflMaxDistance, kGiHwReflMinDistance) : m_giReflMaxDistance;
    cb.reflParams = glm::vec4(reflDistance, m_giReflMaxRoughness, m_giIndirectIntensity,
                              irrValid ? 1.0f : 0.0f);
    cb.frame      = glm::vec4(m_giReflFrameSeed, static_cast<float>(m_giReflW),
                              static_cast<float>(m_giReflH), tier.rays > 1 ? 1.0f : 0.0f);
    cb.rays       = glm::vec4(static_cast<float>(tier.rays), static_cast<float>(m_giInstanceCount),
                              m_skyReflValid ? 1.0f : 0.0f, 0.0f);
    if (!uploadGiBuffer(m_giReflUBO[fi], &cb, sizeof(cb), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;

    {
        VkDescriptorBufferInfo nodesBI{ m_giNodeBuf.buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo trisBI { m_giTriBuf.buf,  0, VK_WHOLE_SIZE };
        // The reflection-only copy (folded graph-material colour), NOT
        // m_giInstanceBuf — gi_probe.comp reads that one and must not see it
        // change when reflections toggle (Thema 191 step 11).
        VkDescriptorBufferInfo instBI { m_giReflInstanceBuf[fi].buf, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo  posII  { m_giPointSampler, m_giGBufPos.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  normII { m_giPointSampler, m_giGBufNorm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  irrII  { m_giPointSampler,
            irrValid ? m_giIrrAtlas.view : m_giGBufNorm.view,
            irrValid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  visII  { m_giPointSampler,
            irrValid ? m_giVisAtlas.view : m_giGBufNorm.view,
            irrValid ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo  outII  { VK_NULL_HANDLE, m_giReflRaw.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo uboBI  { m_giReflUBO[fi].buf, 0, sizeof(GiReflUBOData) };
        VkDescriptorBufferInfo landBI { m_giLandBuf[fi].buf, 0, VK_WHOLE_SIZE };
        // The four weightmap slots: the table's views, the white fallback behind them (all four must be valid).
        VkSampler landSampler = m_albedoSampler ? m_albedoSampler : (m_ssrLinearSampler ? m_ssrLinearSampler : m_giPointSampler);
        VkDescriptorImageInfo landII[4];
        for (int k = 0; k < 4; ++k)
            landII[k] = { landSampler, m_giLandWeightView[k] ? m_giLandWeightView[k] : m_whiteAlbedoView,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[13]{};
        auto wBuf = [&](int idx, uint32_t b, VkDescriptorType t, const VkDescriptorBufferInfo* bi)
        { w[idx] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[idx].dstSet = reflSet;
          w[idx].dstBinding = b; w[idx].descriptorCount = 1; w[idx].descriptorType = t; w[idx].pBufferInfo = bi; };
        auto wImg = [&](int idx, uint32_t b, VkDescriptorType t, const VkDescriptorImageInfo* ii)
        { w[idx] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[idx].dstSet = reflSet;
          w[idx].dstBinding = b; w[idx].descriptorCount = 1; w[idx].descriptorType = t; w[idx].pImageInfo = ii; };
        wBuf(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &nodesBI);
        wBuf(1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &trisBI);
        wBuf(2, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instBI);
        wImg(3, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &posII);
        wImg(4, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normII);
        wImg(5, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irrII);
        wImg(6, 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &visII);
        wImg(7, 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outII);
        wBuf(8, 8, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboBI);
        wBuf(9, 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &landBI);
        w[10] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[10].dstSet = reflSet;
        w[10].dstBinding = 10; w[10].descriptorCount = 4;
        w[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[10].pImageInfo = landII;
        uint32_t nW = 11;
        VkWriteDescriptorSetAccelerationStructureKHR tlasW{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        if (hwRefl)
        {
            tlasW.accelerationStructureCount = 1;
            tlasW.pAccelerationStructures    = &m_giTlas[fi].as;
            w[nW] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[nW].pNext = &tlasW; w[nW].dstSet = reflSet;
            w[nW].dstBinding = 11; w[nW].descriptorCount = 1;
            w[nW].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            ++nW;
        }
        // The sky cube (a valid one even while the bake is off: the dummy cube, with uRays.z = 0 keeping the kernel from reading it).
        VkDescriptorImageInfo skyII{ m_skyReflValid ? m_skyReflSampler : m_albedoSampler,
                                     m_skyReflValid ? m_skyReflCubeView : m_whiteCubeView,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        wImg(static_cast<int>(nW++), 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &skyII);
        vkUpdateDescriptorSets(m_device, nW, w, 0, nullptr);
    }

    // ── 1. Trace ─────────────────────────────────────────────────────────────
    // Raw ping-pongs every frame (GENERAL for this write, SHADER_READ_ONLY for
    // the blur/mix reads below); after frame 1 it is left in the latter, so
    // the compute dispatch needs its own transition back — a fixed "always
    // GENERAL" assumption here is exactly the bug the RTX 4070's validation
    // layer caught (vkQueueSubmit expects GENERAL, finds SHADER_READ_ONLY).
    if (m_giReflRawLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = m_giReflRawLayout; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_giReflRaw.img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        m_giReflRawLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hwRefl ? m_giReflHwPipe : m_giReflPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hwRefl ? m_giReflHwPL : m_giReflPL,
                            0, 1, &reflSet, 0, nullptr);
    vkCmdDispatch(cmd, (static_cast<uint32_t>(m_giReflW) + 7) / 8,
                       (static_cast<uint32_t>(m_giReflH) + 7) / 8, 1);
    // Raw: compute write (GENERAL) → the blur/mix draws' sampled read. The
    // tables above already name it SHADER_READ_ONLY, so the image itself must
    // follow — same transition the diffuse GI's raw mask does not need
    // (it stays GENERAL throughout because nothing else samples it there).
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_giReflRaw.img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        m_giReflRawLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // ── 2. Separable confidence-weighted blur (an a-trous chain) + mix ──────
    VkViewport vvp{ 0, 0, float(m_giReflW), float(m_giReflH), 0.0f, 1.0f };
    VkRect2D   vsc{ { 0, 0 }, { static_cast<uint32_t>(m_giReflW), static_cast<uint32_t>(m_giReflH) } };
    auto drawFs = [&](VkPipeline pipe, VkFramebuffer fb, VkDescriptorSet set, glm::vec4 pc)
    {
        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpbi.renderPass = m_giReflFsRP; rpbi.framebuffer = fb;
        rpbi.renderArea.extent = { static_cast<uint32_t>(m_giReflW), static_cast<uint32_t>(m_giReflH) };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_giReflFsPL, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_giReflFsPL, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdSetViewport(cmd, 0, 1, &vvp);
        vkCmdSetScissor(cmd, 0, 1, &vsc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    };
    const bool blur = m_giReflBlurEnabled;
    if (blur)
    {
        // Reach measured against the half-resolution trace (resDivUsed = 2:
        // the prepass is half the screen, the GL gap the plan documents).
        const HE::GIReflBlurReach br = HE::giReflBlurReachFor(m_giReflMaxRoughness, tier.rays, 2);
        for (int lvl = 0; lvl < br.levels; ++lvl)
        {
            const float stride = br.unit * std::exp2(static_cast<float>(lvl));
            const float tx = stride / static_cast<float>(std::max(1, m_giReflW));
            const float ty = stride / static_cast<float>(std::max(1, m_giReflH));
            // H: the previous level's result (raw at level 0) → ping.
            drawFs(m_giReflBlurPipe, m_giReflPingFB, lvl == 0 ? m_giReflTabRaw : m_giReflTabBlur,
                   glm::vec4(tx, 0.0f, 0.0f, 0.0f));
            // V: ping → blur.
            drawFs(m_giReflBlurPipe, m_giReflBlurFB, m_giReflTabPing, glm::vec4(0.0f, ty, 0.0f, 0.0f));
        }
    }
    // Mix the SHARP trace back in per pixel by roughness so a mirror stays a
    // mirror (floor 0.35 = the half-resolution floor, as D3D/GL). With the
    // blur off this is a plain copy: the "blurred" input is the raw trace.
    drawFs(m_giReflMixPipe, m_giReflFinalFB, blur ? m_giReflTabMix : m_giReflTabRaw,
           glm::vec4(0.35f, 0.0f, 0.0f, 0.0f));
    m_giReflReady = true;
    return true;
}


void VulkanRenderer::SetBloomSettings(const BloomSettings& s)
{
    m_bloomEnabled   = s.enabled;
    m_bloomThreshold = s.threshold;
    m_bloomStrength  = s.intensity;
}

void VulkanRenderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
    // The TAA targets follow at the top of the next Render (syncTaaTargets) —
    // not here, between two passes of a frame being recorded.
    m_aaMethod    = IRenderer::ResolveAAMethod(s.method, GetCapabilities());
    m_aaSharpness = std::clamp(s.sharpness, 0.0f, 1.0f);
}

void VulkanRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
    // Deferred to the next Render() (render thread), drained under a device idle.
    if (materialId != HE::UUID{})
        m_pendingMatInval.push_back(materialId);
}

void VulkanRenderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
    // Queued, not built here: the pipeline must target the pass the next DrawScene
    // renders into (HDR offscreen vs swapchain), which only that call knows. Non-material
    // ids (the streaming poll hands over everything it registered) resolve no shader in
    // the drain and cost a map lookup each.
    for (const HE::UUID& id : materialIds)
        if (id != HE::UUID{}) m_pendingMatWarmup.push_back(id);
}

void VulkanRenderer::InvalidateMesh(const HE::UUID& meshId)
{
    if (meshId != HE::UUID{})
        m_pendingMeshInval.push_back(meshId);
}

void VulkanRenderer::InvalidateTexture(const HE::UUID& textureId)
{
    // Same deferral — the graph-texture cache is keyed by "hi:lo" for UUIDs.
    if (textureId != HE::UUID{})
        m_pendingTexInval.push_back(textureId);
}

// ─────────────────────────────────────────────────────────────────────────────
// SSAO — screen-space ambient occlusion
// ─────────────────────────────────────────────────────────────────────────────

// The position pre-pass's own depth, D32 like m_depthFormat (Thema 182 step 6,
// decision 110): as D16 a depth step is z^2/6554 m at near 0.1 (0.4 m at 50 m),
// so at 50-90 m a plant and the ground rounded to one value and the draw order
// decided which landed in the position target. The image, m_ssaoPosRenderPass
// and m_reflPrepassRP (which attaches the same image) use this constant.
static constexpr VkFormat kSsaoPosDepthFormat = VK_FORMAT_D32_SFLOAT;

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
        submitGraphics(si, VK_NULL_HANDLE);
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
        // Binding 3 (AO) + bindings 4-7 (GI mask/atlases/local mask) + binding 8
        // (forward SSR): all start on the 1x1 white fallback so every sampler in
        // the set is valid from the first frame; runGi() rewrites 4-7 when it
        // produces real targets, and DrawScene rewrites 8 every frame with the
        // trace result or the transparent-black stand-in.
        VkWriteDescriptorSet aw[6]{};
        for (uint32_t b = 0; b < 6; ++b)
        {
            aw[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            aw[b].dstSet          = m_frameUBO[i].set;
            aw[b].dstBinding      = 3 + b;
            aw[b].descriptorCount = 1;
            aw[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            aw[b].pImageInfo      = &wdii;
        }
        vkUpdateDescriptorSets(m_device, 6, aw, 0, nullptr);
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
        submitGraphics(si, VK_NULL_HANDLE);
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
    // Position prepass: RGBA16F color + D32 depth (kSsaoPosDepthFormat).
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
        atts[1].format         = kSsaoPosDepthFormat;
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
        vkCheck(createGraphicsPipelines(1, &pci, nullptr,
                                          &m_ssaoPosGfxPipeline), "ssao pos pipeline");
        // Instanced twin for same-mesh runs, VS swapped. Optional — without it
        // every run loops through m_ssaoPosGfxPipeline.
        if (VkShaderModule posIVS = loadShaderModule("ssao_pos_instanced.vert.spv"))
        {
            VkPipelineShaderStageCreateInfo istages[2] = { stages[0], stages[1] };
            istages[0].module = posIVS;
            InstancedDepthInput iin(k_instStride, 8); // {mvp, modelView}
            VkGraphicsPipelineCreateInfo ipci = pci;
            ipci.pStages           = istages;
            ipci.pVertexInputState = &iin.info;
            if (createGraphicsPipelines(1, &ipci, nullptr,
                                          &m_ssaoPosInstancedPipeline) != VK_SUCCESS)
                HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: instanced SSAO pos pipeline creation failed");
            vkDestroyShaderModule(m_device, posIVS, nullptr);
        }
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
        vkCheck(createGraphicsPipelines(1, &pci, nullptr,
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
        vkCheck(createGraphicsPipelines(1, &pci, nullptr,
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
    // Position prepass depth: D32 (kSsaoPosDepthFormat, not D16)
    ssaoMakeImage(m_device, 0, kSsaoPosDepthFormat, w, h, depthRT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, m_ssaoPosDepth.image, m_ssaoPosDepth.memory, m_ssaoPosDepth.view, fmem);
    // SSAO raw AO: R8_UNORM
    ssaoMakeImage(m_device, 0, VK_FORMAT_R8_UNORM, w, h, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT, m_ssaoRT.image, m_ssaoRT.memory, m_ssaoRT.view, fmem);
    // Blurred AO: R8_UNORM
    ssaoMakeImage(m_device, 0, VK_FORMAT_R8_UNORM, w, h, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT, m_ssaoBlurRT.image, m_ssaoBlurRT.memory, m_ssaoBlurRT.view, fmem);
    nameImage(m_ssaoPosRT.image,    "SSAO position");
    nameImage(m_ssaoPosDepth.image, "SSAO position depth");
    nameImage(m_ssaoRT.image,       "SSAO raw");
    nameImage(m_ssaoBlurRT.image,   "SSAO blur");
    // Scene binding 3 is pointed at the blur target below, for every frame —
    // also the ones runSSAO skips (SSAO off, or GI on: the probes replace AO).
    // Its render pass is then the only thing that would ever leave UNDEFINED, so
    // every such frame sampled an UNDEFINED image (validation: InvalidImageLayout
    // once per submit, Thema 145). scene.frag ignores the value while
    // viewport.z == 0; the layout must be valid regardless.
    primeShaderReadLayout(m_ssaoBlurRT.image);

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
    // The reflection pre-pass framebuffer holds the position and depth views
    // destroyed below, so it goes first — SSR rebuilds it lazily against the new
    // ones on its next frame.
    destroyReflPrepassTargets();
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

void VulkanRenderer::runSSAO(VkCommandBuffer cmd, uint32_t w, uint32_t h,
                             bool reflMrt, bool aoWanted)
{
    GpuPassScope _passTimer(this, "SSAO");
    if (!m_ssaoReady) return;
    // The two consumers are independent: the reflection MRT pre-pass has its own
    // pipeline and its own attachments, so a missing one must not take the
    // occlusion path down with it — nor the other way round.
    if (reflMrt && !(m_reflPrepassPipeline && ensureReflPrepassTargets())) reflMrt = false;
    if (aoWanted && !m_ssaoEnabled) aoWanted = false;
    if (!reflMrt && !aoWanted) return;
    if (!m_ssaoPosRT.fb || !m_ssaoRT.fb || !m_ssaoBlurRT.fb) return;
    if (!m_world) return;

    // Extract + cull the scene against the camera frustum (same as DrawScene).
    const float aspect = w > 0 && h > 0 ? float(w) / float(h) : 1.0f;
    m_extractor.setContentManager(m_contentManager);
    m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
    if (m_renderWorld.objects.empty()) return;

    refineObjectBounds();

    cullCameraObjects();
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
    // Two shapes of the SAME geometry loop. `reflMrt` swaps the render pass, the
    // framebuffer and the pipeline for the three-attachment variant drawn with
    // the SHARED reflection pre-pass shader (plan B2 / §3.2 way (a)); attachment
    // 0 is the identical view-space position in both, which is why passes 2 and
    // 3 below need no branch at all. Per-draw data differs: the hand-written AO
    // vertex takes push constants, the shared one a three-mat4 UBO block —
    // exactly the split OpenGLRenderer::RenderSSAO makes.
    VkClearValue posClear[4]{};
    posClear[0].color        = { { 0.0f, 0.0f, 0.0f, 0.0f } };  // a=0 = background
    if (reflMrt)
    {
        // Per-attachment clears, not one colour: attachment 2 is NDC depth and its
        // background is 1.0 (the far plane). Zero-cleared it would read as geometry
        // sitting on the near plane and the trace would hit it everywhere.
        posClear[1].color        = { { 0.5f, 0.5f, 0.0f, 0.0f } }; // oct(0,0,·) encoded
        posClear[2].color        = { { 1.0f, 1.0f, 1.0f, 1.0f } }; // far plane
        posClear[3].depthStencil = { 1.0f, 0 };
    }
    else
        posClear[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo posRPBI{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    posRPBI.renderPass        = reflMrt ? m_reflPrepassRP : m_ssaoPosRenderPass;
    posRPBI.framebuffer       = reflMrt ? m_reflPrepassFB : m_ssaoPosRT.fb;
    posRPBI.renderArea.extent = { w, h };
    posRPBI.clearValueCount   = reflMrt ? 4u : 2u;
    posRPBI.pClearValues      = posClear;
    vkCmdBeginRenderPass(cmd, &posRPBI, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      reflMrt ? m_reflPrepassPipeline : m_ssaoPosGfxPipeline);
    VkViewport vp2{ 0, 0, float(w), float(h), 0, 1 };
    VkRect2D sc{ {0,0}, {w, h} };
    vkCmdSetViewport(cmd, 0, 1, &vp2);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    const uint32_t prepassFrame = m_currentFrame;
    uint32_t       prepassSlot  = 0;
    if (!reflMrt)
    {
        // Position-only path: runs of consecutive same-mesh AO contributors
        // (particles/precipitation opt out), a run of more than one is ONE
        // instanced draw with the push-constant path's {mvp, modelView} per
        // instance. The MRT reflection pre-pass below keeps the loop: its VS is
        // the library's cross-compiled GLSL with a per-draw UBO slot, not the
        // 128-byte instance buffer (same split as D3D11/D3D12).
        m_preBatches.useSpans = RenderSorter::instanceSpansEnabled();   // read in place, see InstanceRun
        RenderSorter::batchDepthRuns(m_renderWorld, m_sortedIndices,
                                     RenderSorter::DepthFilter::AoContributors, kNoOwnerEntity, m_preBatches);
        for (const RenderSorter::DepthBatch& b : m_preBatches.batches)
        {
            const GpuMesh* mesh = resolveMesh(b.meshAssetId);
            const GpuMesh& gm   = mesh ? *mesh : m_cube;
            if (!gm.indexCount) continue;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, gm.ibuf, 0, VK_INDEX_TYPE_UINT32);
            const InstanceRun run = m_preBatches.run(b);
            if (drawDepthInstanced(cmd, m_ssaoPosInstancedPipeline, m_ssaoPosGfxPipeline, gm.indexCount, run,
                    [&](const glm::mat4& model, glm::mat4* pair) {
                        pair[0] = vp   * model;
                        pair[1] = view * model;
                    }))
            {
                static bool loggedOnce = false; // the runtime witness on Windows
                if (!loggedOnce)
                {
                    loggedOnce = true;
                    HE_LOG_INFO(RHI, "VulkanRenderer: SSAO pre-pass instanced (first batch: %u instances)", b.count);
                }
                continue;
            }
            run.forEach([&](const glm::mat4& model)
            {
                // Push constants: mvp (clip-space) + modelView (view-space) —
                // ssao_pos.vert's push_constant block has the PushConstants layout.
                PushConstants pc{ vp * model, view * model };
                vkCmdPushConstants(cmd, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(pc), &pc);
                vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
            });
        }
    }
    else
    {
        for (uint32_t idx : m_sortedIndices)
        {
            const RenderObject& obj = m_renderWorld.objects[idx];
            if (!obj.contributesAO) continue;   // skip particles/precipitation
            const GpuMesh* mesh = resolveMesh(obj.meshAssetId);
            const GpuMesh& gm   = mesh ? *mesh : m_cube;
            if (!gm.indexCount) continue;

            if (prepassSlot >= k_ssrMaxPrepassDraws)
            {
                HE_LOG_ONCE(RHI, Warning,
                    "VulkanRenderer: more than %u reflection pre-pass draws in a frame — "
                    "the rest are skipped (they will not reflect)", k_ssrMaxPrepassDraws);
                break;
            }
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vbuf, &offset);
            vkCmdBindIndexBuffer(cmd, gm.ibuf, 0, VK_INDEX_TYPE_UINT32);

            // One slot of the pre-pass ring, the descriptor set re-aimed at it through
            // a DYNAMIC offset — the shader sees a plain uniform block either way.
            auto bindSlot = [&](const void* block, size_t bytes) {
                const uint32_t off = prepassSlot * k_ssrPrepassStride;
                std::memcpy(static_cast<uint8_t*>(m_reflPrepassUBO[prepassFrame].mapped) + off, block, bytes);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_reflPrepassPipeLayout, 0, 1, &m_reflPrepassSet[prepassFrame], 1, &off);
                ++prepassSlot;
            };
            auto drawOne = [&](const glm::mat4& model) {
                const glm::mat4 modelView = view * model;
                HE::MaterialShaderLibrary::ReflPrepassUniforms ru;
                const glm::mat4 mvp = vp * model;
                std::memcpy(ru.mvp,       &mvp[0][0],       16 * sizeof(float));
                std::memcpy(ru.modelView, &modelView[0][0], 16 * sizeof(float));
                std::memcpy(ru.model,     &model[0][0],     16 * sizeof(float));
                bindSlot(&ru, sizeof(ru));
                vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
            };

            // A foliage cluster stands for a bucket of plants (Thema 182, 2f): ONE draw
            // through the instanced twin, the models in the frame's instance arena and
            // {viewProj, view} in the slot; without the twin, plant by plant (one slot
            // each, up to the ring's end).
            if (obj.isCluster())
            {
                const InstanceRun run = m_renderWorld.instanceRun(obj);
                const uint32_t n = run.size();
                VkBuffer ibuf = VK_NULL_HANDLE; VkDeviceSize ioff = 0; uint8_t* dst = nullptr;
                if (m_reflPrepassInstPipeline && n > 1 && allocInstances(n, ibuf, ioff, dst))
                {
                    RenderSorter::copyInstanceModels(dst + sizeof(glm::mat4), k_instStride, run);
                    HE::MaterialShaderLibrary::ReflPrepassInstUniforms iu;
                    std::memcpy(iu.viewProj, &vp[0][0],   16 * sizeof(float));
                    std::memcpy(iu.view,     &view[0][0], 16 * sizeof(float));
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_reflPrepassInstPipeline);
                    bindSlot(&iu, sizeof(iu));
                    vkCmdBindVertexBuffers(cmd, 1, 1, &ibuf, &ioff);
                    vkCmdDrawIndexed(cmd, gm.indexCount, n, 0, 0, 0);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_reflPrepassPipeline);
                    static bool loggedOnce = false; // the runtime witness
                    if (!loggedOnce)
                    {
                        loggedOnce = true;
                        HE_LOG_INFO(RHI, "VulkanRenderer: reflection pre-pass instanced (first cluster: %u plants)", n);
                    }
                    continue;
                }
                bool slotsLeft = true;
                run.forEach([&](const glm::mat4& model)
                {
                    if (!slotsLeft || prepassSlot >= k_ssrMaxPrepassDraws) { slotsLeft = false; return; } // said once above, at the next object
                    drawOne(model);
                });
                continue;
            }
            drawOne(obj.transform);
        }
    }
    vkCmdEndRenderPass(cmd);
    // posRT transitions to SHADER_READ_ONLY via renderpass finalLayout.

    // The pre-pass can be the whole job: SSR wants it on frames where SSAO is
    // off or the GI probes replaced it. m_ssaoRanThisFrame stays FALSE then, so
    // scene.frag keeps its white AO fallback (viewport.z = 0).
    if (!aoWanted) return;

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
// Screen-space reflections — forward path
// docs/ssr-cross-backend-plan.md checkpoint B
// ─────────────────────────────────────────────────────────────────────────────
//
// The Vulkan twin of MetalRenderer::EncodeForwardSSR and OpenGLRenderer::
// RenderForwardSSR, built from the SAME shared trace and blur shaders.
// Deliberately not a second implementation: everything that decides how a
// reflection LOOKS lives in MaterialShaderLibrary, and this file only decides
// which image goes into which slot.

// Park a freshly created image in SHADER_READ_ONLY. Needed for every SSR target
// before its first use, because the trace samples the previous history pair even
// on frame one (the blend weight is zero, the READ is not), and sampling an
// image still in UNDEFINED is invalid however the value is used afterwards.
// Same one-time-submit shape as the dummy image in createPostFXPipelines.
void VulkanRenderer::primeShaderReadLayout(VkImage img)
{
    if (!img || !m_cmdPool) return;
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer tmp = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &cbai, &tmp) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(tmp, &cbi);
    ssaoTransition(tmp, img, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   0, VK_ACCESS_SHADER_READ_BIT);
    vkEndCommandBuffer(tmp);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
    submitGraphics(si, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
}

bool VulkanRenderer::EnsureSSRPipelines()
{
#if !defined(HE_HAVE_SHADERC)
    return false;
#else
    if (m_ssrReady) return true;
    if (m_ssrPipelinesTried) return false;
    // The trace and the pre-pass both hang off SSAO's position pass, and the
    // blur/trace pipelines need their own passes only — but the pre-pass
    // pipeline reuses SSAO's vertex layout and depth format, so wait until the
    // SSAO stage exists rather than burning the single attempt.
    if (!m_ssaoReady) return false;
    m_ssrPipelinesTried = true;

    using Backend = HE::MaterialShaderLibrary::Backend;
    const auto& pv = m_matShaderLib.reflPrepassVertex(Backend::SpirV);
    const auto& pf = m_matShaderLib.reflPrepassFragment(Backend::SpirV);
    const auto& fv = m_matShaderLib.fullscreenVertex(Backend::SpirV);
    const auto& tf = m_matShaderLib.ssrTrace(Backend::SpirV);
    const auto& bf = m_matShaderLib.ssrBlur(Backend::SpirV);
    for (const HE::MaterialShaderLibrary::Compiled* c : { &pv, &pf, &fv, &tf, &bf })
        if (!c->ok || c->spirv.empty())
        {
            HE_LOG_ERROR(RHI, "%s",
                (std::string("VulkanRenderer: SSR shader cross-compile failed\n")
                 + pv.log + pf.log + fv.log + tf.log + bf.log).c_str());
            return false;
        }

    // ── Samplers ─────────────────────────────────────────────────────────────
    // Point sampling is not an optimisation here: an interpolated oct normal
    // straddling a geometry edge decodes to garbage, an interpolated depth is a
    // surface that never existed, and an interpolated receiver position would
    // make the disocclusion test accept a pixel it should reject.
    auto makeSampler = [&](VkFilter f, VkSampler& out) -> bool {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = f;
        sci.minFilter    = f;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        return vkCreateSampler(m_device, &sci, nullptr, &out) == VK_SUCCESS;
    };
    if (!m_ssrLinearSampler && !makeSampler(VK_FILTER_LINEAR, m_ssrLinearSampler)) return false;
    if (!m_ssrPointSampler  && !makeSampler(VK_FILTER_NEAREST, m_ssrPointSampler))  return false;

    // ── Render passes ────────────────────────────────────────────────────────
    // Reflection pre-pass: SSAO's position attachment + the two reflection
    // attachments + SSAO's depth. Attachment 0 keeps the SSAO pass's exact
    // description (RGBA16F, CLEAR/STORE, final SHADER_READ_ONLY) so the
    // occlusion stage cannot tell which of the two passes filled it.
    if (!m_reflPrepassRP)
    {
        VkAttachmentDescription atts[4]{};
        auto colorAtt = [](VkFormat fmt) {
            VkAttachmentDescription a{};
            a.format         = fmt;
            a.samples        = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return a;
        };
        atts[0] = colorAtt(VK_FORMAT_R16G16B16A16_SFLOAT); // view-space position (SSAO's)
        atts[1] = colorAtt(VK_FORMAT_R16G16B16A16_SFLOAT); // GB1: oct normal + roughness
        atts[2] = colorAtt(VK_FORMAT_R32_SFLOAT);          // NDC depth
        atts[3].format         = kSsaoPosDepthFormat;   // = m_ssaoPosDepth
        atts[3].samples        = VK_SAMPLE_COUNT_1_BIT;
        atts[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[3].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[3] = {
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkAttachmentReference depthRef{ 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 3;
        sub.pColorAttachments       = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 4; rpci.pAttachments  = atts;
        rpci.subpassCount    = 1; rpci.pSubpasses    = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_reflPrepassRP) != VK_SUCCESS)
            return false;
    }
    // Trace (2 × RGBA16F) and blur (1 × RGBA16F). DONT_CARE on load: the
    // fullscreen triangle covers every pixel, so nothing is inherited.
    auto makeF16RP = [&](uint32_t count, VkRenderPass& rp) -> bool {
        VkAttachmentDescription atts[2]{};
        for (uint32_t i = 0; i < count; ++i)
        {
            atts[i].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
            atts[i].samples        = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[i].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkAttachmentReference refs[2] = {
            { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = count;
        sub.pColorAttachments    = refs;
        // The chain reads what the previous pass wrote and writes over what the
        // pass before that read — both hazards, both across EXTERNAL.
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
        rpci.attachmentCount = count; rpci.pAttachments  = atts;
        rpci.subpassCount    = 1;     rpci.pSubpasses    = &sub;
        rpci.dependencyCount = 2;     rpci.pDependencies = deps;
        return vkCreateRenderPass(m_device, &rpci, nullptr, &rp) == VK_SUCCESS;
    };
    if (!m_ssrTraceRP && !makeF16RP(2, m_ssrTraceRP)) return false;
    if (!m_ssrBlurRP  && !makeF16RP(1, m_ssrBlurRP))  return false;

    // ── Descriptor set layouts, exactly as the generated SPIR-V declares them ─
    if (!m_reflPrepassSetLayout)
    {
        // binding 1 = the shared pre-pass's `U` block. DYNAMIC so one set serves
        // every draw in the pass; the shader sees a plain uniform block.
        VkDescriptorSetLayoutBinding b{};
        b.binding = 1;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 1; slci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_reflPrepassSetLayout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_reflPrepassSetLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_reflPrepassPipeLayout) != VK_SUCCESS)
            return false;
    }
    if (!m_ssrTraceSetLayout)
    {
        // heSceneColor 19, heGB1 20, heGBDepth 22, HeSSRTrace 23, heSSRHistRad 24,
        // heSSRHistPos 25 — kSSRTraceFS's canonical bindings, unchanged.
        VkDescriptorSetLayoutBinding b[6]{};
        const uint32_t bindings[6] = { 19, 20, 22, 23, 24, 25 };
        for (int i = 0; i < 6; ++i)
        {
            b[i].binding         = bindings[i];
            b[i].descriptorType  = bindings[i] == 23 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 6; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_ssrTraceSetLayout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_ssrTraceSetLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_ssrTracePipeLayout) != VK_SUCCESS)
            return false;
    }
    if (!m_ssrBlurSetLayout)
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 19; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 23; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        slci.bindingCount = 2; slci.pBindings = b;
        if (vkCreateDescriptorSetLayout(m_device, &slci, nullptr, &m_ssrBlurSetLayout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &m_ssrBlurSetLayout;
        if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_ssrBlurPipeLayout) != VK_SUCCESS)
            return false;
    }

    // ── Per-frame buffers, pools and the pre-pass sets ───────────────────────
    auto makeBuf = [&](VkDeviceSize size, SSRFrameBuf& fb) -> bool {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size  = size;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(m_device, &bci, nullptr, &fb.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(m_device, fb.buf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &mai, nullptr, &fb.mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, fb.buf, fb.mem, 0);
        return vkMapMemory(m_device, fb.mem, 0, size, 0, &fb.mapped) == VK_SUCCESS;
    };
    if (!m_reflPrepassPool)
    {
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_maxFramesInFlight };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = k_maxFramesInFlight; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_reflPrepassPool) != VK_SUCCESS)
            return false;
    }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        if (!m_reflPrepassUBO[i].buf
            && !makeBuf(static_cast<VkDeviceSize>(k_ssrMaxPrepassDraws) * k_ssrPrepassStride,
                        m_reflPrepassUBO[i]))
            return false;
        if (!m_ssrUBO[i].buf
            && !makeBuf(static_cast<VkDeviceSize>(k_ssrUboSlots) * k_ssrUboStride, m_ssrUBO[i]))
            return false;
        if (m_reflPrepassSet[i] == VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool     = m_reflPrepassPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts        = &m_reflPrepassSetLayout;
            if (vkAllocateDescriptorSets(m_device, &dsai, &m_reflPrepassSet[i]) != VK_SUCCESS)
                return false;
            // The range is ONE slot: a dynamic offset re-aims the same set per draw.
            VkDescriptorBufferInfo bi{ m_reflPrepassUBO[i].buf, 0,
                sizeof(HE::MaterialShaderLibrary::ReflPrepassUniforms) };
            VkWriteDescriptorSet wd{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            wd.dstSet = m_reflPrepassSet[i]; wd.dstBinding = 1; wd.descriptorCount = 1;
            wd.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; wd.pBufferInfo = &bi;
            vkUpdateDescriptorSets(m_device, 1, &wd, 0, nullptr);
        }
        if (m_ssrPool[i] == VK_NULL_HANDLE)
        {
            // One trace set (5 samplers + 1 UBO) and up to four blur sets
            // (1 sampler + 1 UBO each) per frame; reset whole, like the decals.
            VkDescriptorPoolSize ps[2] = {
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          8 },
            };
            VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpci.maxSets = 8; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
            if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_ssrPool[i]) != VK_SUCCESS)
                return false;
        }
    }

    // ── 1×1 transparent black ────────────────────────────────────────────────
    // Bound wherever a reflection texture is expected but no pass ran. The
    // gates fold the branch away, but a declared sampler still needs a valid
    // descriptor — and a sample of THIS one contributes nothing even if a gate
    // were ever wrong. (m_dummyImage cannot serve: it is transitioned but never
    // written, so what it holds is undefined.)
    if (!m_ssrBlackRT.image)
    {
        auto fmem = [&](uint32_t bits, VkMemoryPropertyFlags f) { return findMemoryType(bits, f); };
        ssaoMakeImage(m_device, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT,
                      m_ssrBlackRT.image, m_ssrBlackRT.memory, m_ssrBlackRT.view, fmem);
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbai.commandPool = m_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer tmp = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device, &cbai, &tmp) == VK_SUCCESS)
        {
            VkCommandBufferBeginInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tmp, &cbi);
            ssaoTransition(tmp, m_ssrBlackRT.image, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkClearColorValue zero{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(tmp, m_ssrBlackRT.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &zero, 1, &range);
            ssaoTransition(tmp, m_ssrBlackRT.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            vkEndCommandBuffer(tmp);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &tmp;
            submitGraphics(si, VK_NULL_HANDLE);
            vkQueueWaitIdle(m_graphicsQueue);
            vkFreeCommandBuffers(m_device, m_cmdPool, 1, &tmp);
        }
    }

    // ── Pipelines ────────────────────────────────────────────────────────────
    auto makeModule = [&](const std::vector<uint32_t>& spv) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode    = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    };
    VkShaderModule mPV = makeModule(pv.spirv), mPF = makeModule(pf.spirv);
    VkShaderModule mFV = makeModule(fv.spirv), mTF = makeModule(tf.spirv), mBF = makeModule(bf.spirv);
    auto dropModules = [&] {
        for (VkShaderModule m : { mPV, mPF, mFV, mTF, mBF })
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
    };
    if (!mPV || !mPF || !mFV || !mTF || !mBF)
    {
        dropModules();
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: SSR shader module creation failed");
        return false;
    }

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba[3]{};
    for (auto& a : cba)
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                         | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    auto buildPipeline = [&](VkShaderModule vsm, VkShaderModule fsm, bool geometry,
                             uint32_t colorCount, VkPipelineLayout layout,
                             VkRenderPass rp, VkPipeline& out, bool instanced = false) -> bool {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vsm; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fsm; stages[1].pName = "main";
        // Geometry passes take the scene's interleaved vertex buffer; the
        // fullscreen ones are attribute-less (gl_VertexIndex).
        VkVertexInputBindingDescription bind{ 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[3] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,    24 },
        };
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        if (geometry)
        {
            vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &bind;
            vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;
        }
        // The instanced pre-pass (Thema 182, 2f): position + normal (it reads no UV,
        // and an attribute the shader does not consume is a validation warning) and
        // the model at locations 4..7 from binding 1: the arena's 128-byte
        // {mvp, model} slots, the model half at +64, as for the graph-material twin.
        const VkVertexInputBindingDescription ibinds[2] = {
            { 0, 8u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, k_instStride,       VK_VERTEX_INPUT_RATE_INSTANCE },
        };
        const VkVertexInputAttributeDescription iattrs[6] = {
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,       0 },
            { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,      12 },
            { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,   64 },
            { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,   80 },
            { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,   96 },
            { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT,  112 },
        };
        if (instanced)
        {
            vi.vertexBindingDescriptionCount   = 2; vi.pVertexBindingDescriptions   = ibinds;
            vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = iattrs;
        }
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable  = geometry ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = geometry ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = colorCount; cb.pAttachments = cba;
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
        pci.layout              = layout;
        pci.renderPass          = rp;
        return createGraphicsPipelines(1, &pci, nullptr, &out) == VK_SUCCESS;
    };
    const bool okPrepass = buildPipeline(mPV, mPF, /*geometry=*/true,  3,
                                         m_reflPrepassPipeLayout, m_reflPrepassRP, m_reflPrepassPipeline);
    const bool okTrace   = buildPipeline(mFV, mTF, /*geometry=*/false, 2,
                                         m_ssrTracePipeLayout, m_ssrTraceRP, m_ssrTracePipeline);
    const bool okBlur    = buildPipeline(mFV, mBF, /*geometry=*/false, 1,
                                         m_ssrBlurPipeLayout, m_ssrBlurRP, m_ssrBlurPipeline);
    // The instanced pre-pass twin for foliage clusters, optional (none: plant by plant).
    {
        const auto& piv = m_matShaderLib.reflPrepassVertexInstanced(Backend::SpirV);
        VkShaderModule mPIV = (piv.ok && !piv.spirv.empty()) ? makeModule(piv.spirv) : VK_NULL_HANDLE;
        if (!mPIV || !buildPipeline(mPIV, mPF, /*geometry=*/true, 3, m_reflPrepassPipeLayout,
                                    m_reflPrepassRP, m_reflPrepassInstPipeline, /*instanced=*/true))
        {
            m_reflPrepassInstPipeline = VK_NULL_HANDLE;
            HE_LOG_WARN(RHI, "%s", "VulkanRenderer: no instanced reflection pre-pass, foliage "
                                   "clusters enter it plant by plant");
        }
        if (mPIV) vkDestroyShaderModule(m_device, mPIV, nullptr);
    }
    dropModules();
    if (!okPrepass || !okTrace || !okBlur)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: SSR pipeline creation failed");
        return false;
    }
    m_ssrReady = true;
    HE_LOG_INFO(RHI, "%s", "VulkanRenderer: SSR pipelines created (forward, half-res trace)");
    return true;
#endif
}

void VulkanRenderer::destroySSRPipelines()
{
    if (!m_device) return;
    destroySSRTargets();
    destroyReflPrepassTargets();
    if (m_ssrColorHist.view)   { vkDestroyImageView(m_device, m_ssrColorHist.view,   nullptr); m_ssrColorHist.view   = VK_NULL_HANDLE; }
    if (m_ssrColorHist.image)  { vkDestroyImage    (m_device, m_ssrColorHist.image,  nullptr); m_ssrColorHist.image  = VK_NULL_HANDLE; }
    if (m_ssrColorHist.memory) { vkFreeMemory      (m_device, m_ssrColorHist.memory, nullptr); m_ssrColorHist.memory = VK_NULL_HANDLE; }
    m_ssrColorHistW = m_ssrColorHistH = 0;
    m_ssrColorHistValid = false;
    if (m_ssrBlackRT.view)   { vkDestroyImageView(m_device, m_ssrBlackRT.view,   nullptr); m_ssrBlackRT.view   = VK_NULL_HANDLE; }
    if (m_ssrBlackRT.image)  { vkDestroyImage    (m_device, m_ssrBlackRT.image,  nullptr); m_ssrBlackRT.image  = VK_NULL_HANDLE; }
    if (m_ssrBlackRT.memory) { vkFreeMemory      (m_device, m_ssrBlackRT.memory, nullptr); m_ssrBlackRT.memory = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < k_maxFramesInFlight; ++i)
    {
        for (SSRFrameBuf* fb : { &m_reflPrepassUBO[i], &m_ssrUBO[i] })
        {
            if (fb->mem) vkUnmapMemory(m_device, fb->mem);
            if (fb->buf) vkDestroyBuffer(m_device, fb->buf, nullptr);
            if (fb->mem) vkFreeMemory(m_device, fb->mem, nullptr);
            *fb = {};
        }
        m_reflPrepassSet[i] = VK_NULL_HANDLE; // freed with the pool below
        if (m_ssrPool[i]) { vkDestroyDescriptorPool(m_device, m_ssrPool[i], nullptr); m_ssrPool[i] = VK_NULL_HANDLE; }
    }
    if (m_reflPrepassPool)   { vkDestroyDescriptorPool(m_device, m_reflPrepassPool, nullptr); m_reflPrepassPool = VK_NULL_HANDLE; }
    if (m_reflPrepassPipeline) { vkDestroyPipeline(m_device, m_reflPrepassPipeline, nullptr); m_reflPrepassPipeline = VK_NULL_HANDLE; }
    if (m_reflPrepassInstPipeline) { vkDestroyPipeline(m_device, m_reflPrepassInstPipeline, nullptr); m_reflPrepassInstPipeline = VK_NULL_HANDLE; }
    if (m_ssrTracePipeline)  { vkDestroyPipeline(m_device, m_ssrTracePipeline, nullptr); m_ssrTracePipeline = VK_NULL_HANDLE; }
    if (m_ssrBlurPipeline)   { vkDestroyPipeline(m_device, m_ssrBlurPipeline,  nullptr); m_ssrBlurPipeline  = VK_NULL_HANDLE; }
    if (m_reflPrepassPipeLayout) { vkDestroyPipelineLayout(m_device, m_reflPrepassPipeLayout, nullptr); m_reflPrepassPipeLayout = VK_NULL_HANDLE; }
    if (m_ssrTracePipeLayout){ vkDestroyPipelineLayout(m_device, m_ssrTracePipeLayout, nullptr); m_ssrTracePipeLayout = VK_NULL_HANDLE; }
    if (m_ssrBlurPipeLayout) { vkDestroyPipelineLayout(m_device, m_ssrBlurPipeLayout,  nullptr); m_ssrBlurPipeLayout  = VK_NULL_HANDLE; }
    if (m_reflPrepassSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_reflPrepassSetLayout, nullptr); m_reflPrepassSetLayout = VK_NULL_HANDLE; }
    if (m_ssrTraceSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_ssrTraceSetLayout, nullptr); m_ssrTraceSetLayout = VK_NULL_HANDLE; }
    if (m_ssrBlurSetLayout)  { vkDestroyDescriptorSetLayout(m_device, m_ssrBlurSetLayout,  nullptr); m_ssrBlurSetLayout  = VK_NULL_HANDLE; }
    if (m_reflPrepassRP)     { vkDestroyRenderPass(m_device, m_reflPrepassRP, nullptr); m_reflPrepassRP = VK_NULL_HANDLE; }
    if (m_ssrTraceRP)        { vkDestroyRenderPass(m_device, m_ssrTraceRP, nullptr); m_ssrTraceRP = VK_NULL_HANDLE; }
    if (m_ssrBlurRP)         { vkDestroyRenderPass(m_device, m_ssrBlurRP,  nullptr); m_ssrBlurRP  = VK_NULL_HANDLE; }
    if (m_ssrLinearSampler)  { vkDestroySampler(m_device, m_ssrLinearSampler, nullptr); m_ssrLinearSampler = VK_NULL_HANDLE; }
    if (m_ssrPointSampler)   { vkDestroySampler(m_device, m_ssrPointSampler,  nullptr); m_ssrPointSampler  = VK_NULL_HANDLE; }
    m_ssrReady          = false;
    m_ssrPipelinesTried = false; // a re-Initialize() rebuilds instead of staying disabled
    m_ssrResultView     = VK_NULL_HANDLE;
}

// The two extra pre-pass attachments, at SSAO's resolution and hanging off
// SSAO's position and depth views. Released by destroySSAOTargets — a
// framebuffer holds views, and SSAO recreates its own on every resize.
bool VulkanRenderer::ensureReflPrepassTargets()
{
    if (!m_reflPrepassRP || !m_ssaoPosRT.view || !m_ssaoPosDepth.view) return false;
    if (m_ssaoW == 0 || m_ssaoH == 0) return false;
    if (m_reflPrepassFB) return true;
    auto fmem = [&](uint32_t bits, VkMemoryPropertyFlags f) { return findMemoryType(bits, f); };
    const VkImageUsageFlags colorRT = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ssaoMakeImage(m_device, 0, VK_FORMAT_R16G16B16A16_SFLOAT, m_ssaoW, m_ssaoH, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT,
                  m_reflAttrRT.image, m_reflAttrRT.memory, m_reflAttrRT.view, fmem);
    ssaoMakeImage(m_device, 0, VK_FORMAT_R32_SFLOAT, m_ssaoW, m_ssaoH, colorRT,
                  VK_IMAGE_ASPECT_COLOR_BIT,
                  m_reflNdcRT.image, m_reflNdcRT.memory, m_reflNdcRT.view, fmem);
    // Both are written by the pass before they are read, so they need no priming
    // — but the trace's descriptor is written before the first pass on a frame
    // where the pre-pass was skipped, so park them anyway.
    primeShaderReadLayout(m_reflAttrRT.image);
    primeShaderReadLayout(m_reflNdcRT.image);
    VkImageView atts[4] = { m_ssaoPosRT.view, m_reflAttrRT.view, m_reflNdcRT.view,
                            m_ssaoPosDepth.view };
    VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = m_reflPrepassRP; fci.attachmentCount = 4; fci.pAttachments = atts;
    fci.width = m_ssaoW; fci.height = m_ssaoH; fci.layers = 1;
    if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_reflPrepassFB) != VK_SUCCESS)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: reflection pre-pass framebuffer failed");
        destroyReflPrepassTargets();
        return false;
    }
    return true;
}

void VulkanRenderer::destroyReflPrepassTargets()
{
    if (!m_device) return;
    if (m_reflPrepassFB) { vkDestroyFramebuffer(m_device, m_reflPrepassFB, nullptr); m_reflPrepassFB = VK_NULL_HANDLE; }
    for (SSAORenderTarget* rt : { &m_reflAttrRT, &m_reflNdcRT })
    {
        if (rt->view)   { vkDestroyImageView(m_device, rt->view,   nullptr); rt->view   = VK_NULL_HANDLE; }
        if (rt->image)  { vkDestroyImage    (m_device, rt->image,  nullptr); rt->image  = VK_NULL_HANDLE; }
        if (rt->memory) { vkFreeMemory      (m_device, rt->memory, nullptr); rt->memory = VK_NULL_HANDLE; }
    }
}

// Destroying and rebuilding the half-res chain mid-frame is safe for exactly one
// reason: the only thing that changes its size is a viewport resize, and
// createViewportResources runs behind a vkDeviceWaitIdle. Nothing here waits on
// its own — do not "fix" that by dropping the wait upstream.
void VulkanRenderer::createSSRTargets(uint32_t w, uint32_t h)
{
    if (!m_ssrTraceRP || !m_ssrBlurRP) return;
    w = std::max(1u, w); h = std::max(1u, h);
    if (m_ssrHistFB[0] && w == m_ssrW && h == m_ssrH) return;
    destroySSRTargets();
    m_ssrW = w; m_ssrH = h;
    auto fmem = [&](uint32_t bits, VkMemoryPropertyFlags f) { return findMemoryType(bits, f); };
    const VkImageUsageFlags colorRT = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto make = [&](SSAORenderTarget& rt) {
        ssaoMakeImage(m_device, 0, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, colorRT,
                      VK_IMAGE_ASPECT_COLOR_BIT, rt.image, rt.memory, rt.view, fmem);
        primeShaderReadLayout(rt.image);
    };
    for (int i = 0; i < 2; ++i)
    {
        make(m_ssrHistRad[i]);
        make(m_ssrHistPos[i]);
        VkImageView atts[2] = { m_ssrHistRad[i].view, m_ssrHistPos[i].view };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_ssrTraceRP; fci.attachmentCount = 2; fci.pAttachments = atts;
        fci.width = w; fci.height = h; fci.layers = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &m_ssrHistFB[i]) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: SSR history framebuffer failed");
            destroySSRTargets();
            return;
        }
    }
    for (SSAORenderTarget* rt : { &m_ssrPingRT, &m_ssrReflRT })
    {
        make(*rt);
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = m_ssrBlurRP; fci.attachmentCount = 1; fci.pAttachments = &rt->view;
        fci.width = w; fci.height = h; fci.layers = 1;
        if (vkCreateFramebuffer(m_device, &fci, nullptr, &rt->fb) != VK_SUCCESS)
        {
            HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: SSR blur framebuffer failed");
            destroySSRTargets();
            return;
        }
    }
    // Fresh targets — the first frame must not blend against them, the same rule
    // the Metal path states at EnsureSSRTarget.
    m_ssrHistIdx   = 0;
    m_ssrHistValid = false;
}

void VulkanRenderer::destroySSRTargets()
{
    if (!m_device) return;
    for (int i = 0; i < 2; ++i)
        if (m_ssrHistFB[i]) { vkDestroyFramebuffer(m_device, m_ssrHistFB[i], nullptr); m_ssrHistFB[i] = VK_NULL_HANDLE; }
    // NOT m_ssrColorHist: it is full-res, sized and invalidated by
    // CaptureSSRColorHistory alone, and RenderForwardSSR reads it across the
    // createSSRTargets call below — dropping it here would free a view that the
    // same frame still binds.
    for (SSAORenderTarget* rt : { &m_ssrHistRad[0], &m_ssrHistRad[1], &m_ssrHistPos[0],
                                  &m_ssrHistPos[1], &m_ssrPingRT, &m_ssrReflRT })
    {
        if (rt->fb)     { vkDestroyFramebuffer(m_device, rt->fb,     nullptr); rt->fb     = VK_NULL_HANDLE; }
        if (rt->view)   { vkDestroyImageView  (m_device, rt->view,   nullptr); rt->view   = VK_NULL_HANDLE; }
        if (rt->image)  { vkDestroyImage      (m_device, rt->image,  nullptr); rt->image  = VK_NULL_HANDLE; }
        if (rt->memory) { vkFreeMemory        (m_device, rt->memory, nullptr); rt->memory = VK_NULL_HANDLE; }
    }
    m_ssrW = m_ssrH = 0;
    m_ssrHistValid  = false;
    m_ssrResultView = VK_NULL_HANDLE;
}

// One gate for the whole frame, asked before the pre-pass and again before the
// trace. `m_postFxReady`/`m_hdrImage` are the honest part: this backend's only
// HDR radiance source is the PostFX target (plan §2.2) — the editor viewport's,
// or the packaged game's since it runs the chain too (plan C6) — so in the
// direct swapchain fallback SSR is silently inactive rather than half-wired.
bool VulkanRenderer::ssrWantedThisFrame() const
{
    return m_ssrEnabled && m_ssrIntensity > 0.0f && m_ssrReady
        && m_postFxReady && m_hdrImage != VK_NULL_HANDLE;
}

// Full-res copy of the finished HDR frame (opaque + sky + transparency), taken
// right after the scene render pass ends. NEXT frame's trace reprojects its hits
// into it — the one frame of content lag is the accepted forward trade
// (Option A, docs/ssr-plan.md §2). Called while m_hdrImage is still
// COLOR_ATTACHMENT_OPTIMAL and leaves it that way.
void VulkanRenderer::CaptureSSRColorHistory(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    if (!m_hdrImage || w == 0 || h == 0) return;
    if (!m_ssrColorHist.image || m_ssrColorHistW != w || m_ssrColorHistH != h)
    {
        // A resize invalidates the copy: it is the frame BEFORE the resize.
        if (m_ssrColorHist.view)   { vkDestroyImageView(m_device, m_ssrColorHist.view,   nullptr); m_ssrColorHist.view   = VK_NULL_HANDLE; }
        if (m_ssrColorHist.image)  { vkDestroyImage    (m_device, m_ssrColorHist.image,  nullptr); m_ssrColorHist.image  = VK_NULL_HANDLE; }
        if (m_ssrColorHist.memory) { vkFreeMemory      (m_device, m_ssrColorHist.memory, nullptr); m_ssrColorHist.memory = VK_NULL_HANDLE; }
        m_ssrColorHistValid = false;
        auto fmem = [&](uint32_t bits, VkMemoryPropertyFlags f) { return findMemoryType(bits, f); };
        ssaoMakeImage(m_device, 0, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT,
                      m_ssrColorHist.image, m_ssrColorHist.memory, m_ssrColorHist.view, fmem);
        primeShaderReadLayout(m_ssrColorHist.image);
        m_ssrColorHistW = w; m_ssrColorHistH = h;
    }
    if (!m_ssrColorHist.image) return;

    // Hand-written barriers: runPostFXBarrier knows only the colour/shader-read
    // pair, and both ends of this copy are TRANSFER layouts.
    ssaoTransition(cmd, m_hdrImage, m_hdrLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ssaoTransition(cmd, m_ssrColorHist.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { w, h, 1 };
    vkCmdCopyImage(cmd, m_hdrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_ssrColorHist.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ssaoTransition(cmd, m_ssrColorHist.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    ssaoTransition(cmd, m_hdrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_hdrLayout,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    m_ssrColorHistValid = true;
}

VkImageView VulkanRenderer::RenderForwardSSR(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
    GpuPassScope _passTimer(this, "SSR");
#if !defined(HE_HAVE_SHADERC)
    (void)cmd; (void)w; (void)h;
    return VK_NULL_HANDLE;
#else
    if (!m_ssrReady || !m_reflPrepassFB) return VK_NULL_HANDLE;
    if (!m_ssrColorHist.image || !m_ssrColorHistValid) return VK_NULL_HANDLE; // frame 1 seeds the copy
    if (m_ssrColorHistW != w || m_ssrColorHistH != h)  return VK_NULL_HANDLE; // stale after a resize
    const uint32_t tw = std::max(1u, w / 2), th = std::max(1u, h / 2);
    createSSRTargets(tw, th);
    if (!m_ssrHistFB[0]) return VK_NULL_HANDLE;

    const uint32_t fi = m_currentFrame;
    if (!m_ssrPool[fi] || !m_ssrUBO[fi].mapped) return VK_NULL_HANDLE;
    // DrawScene runs at most once per frame, so resetting the whole pool here is
    // safe — the frame fence already waited for the GPU.
    vkResetDescriptorPool(m_device, m_ssrPool[fi], 0);

    const int curIdx  = m_ssrHistIdx;
    const int prevIdx = 1 - curIdx;
    const glm::mat4 viewProj =
        HE::kVulkanClipFix * m_renderWorld.camera.projection * m_renderWorld.camera.view;
    const glm::vec3 camFwd =
        -glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));

    VkViewport vp{ 0.0f, 0.0f, float(tw), float(th), 0.0f, 1.0f };
    VkRect2D   sc{ { 0, 0 }, { tw, th } };

    // ── 1. Trace (MRT: blended radiance + receiver position) ─────────────────
    {
        const bool temporal = m_ssrQuality >= 2;
        float hist = 0.0f;
        if (temporal && m_ssrHistValid)
        {
            // Camera-motion damping, the same measure Metal and GL take: a moved
            // view means the un-reprojected reflection CONTENT is stale.
            float delta = 0.0f;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    delta = std::max(delta, std::abs(viewProj[c][r] - m_ssrPrevViewProj[c][r]));
            hist = delta > 1e-5f ? 0.55f : 0.85f;
        }
        m_ssrFrameSeed += 1.0f;

        HE::MaterialShaderLibrary::SSRTraceUniforms tu;
        const glm::mat4 ivp = glm::inverse(viewProj);
        std::memcpy(tu.viewProj,     &viewProj[0][0],           16 * sizeof(float));
        std::memcpy(tu.invViewProj,  &ivp[0][0],                16 * sizeof(float));
        std::memcpy(tu.prevViewProj, &m_ssrPrevViewProj[0][0],  16 * sizeof(float));
        tu.cfg2[0] = m_ssrFrameSeed;
        tu.cfg2[1] = hist;
        tu.cfg2[2] = 1.0f;                   // forward: colour from the previous frame's copy
        tu.cfg2[3] = temporal ? 1.0f : 0.0f; // glossy cone jitter (High)
        tu.camPos[0] = m_renderWorld.camera.position.x;
        tu.camPos[1] = m_renderWorld.camera.position.y;
        tu.camPos[2] = m_renderWorld.camera.position.z;
        tu.camFwd[0] = camFwd.x; tu.camFwd[1] = camFwd.y; tu.camFwd[2] = camFwd.z;
        tu.cfg[0] = m_ssrMaxDistance;
        tu.cfg[1] = m_ssrThickness;
        tu.cfg[2] = m_ssrMaxRoughness;
        tu.cfg[3] = static_cast<float>(m_ssrQuality <= 0 ? 16 : m_ssrQuality == 1 ? 32 : 64);
        // Vulkan conventions (docs/ssr-cross-backend-plan.md §1.4, inherited from
        // the decal port): kVulkanClipFix flips NDC y DOWN, so a uv.y of 0 at the
        // top maps to clip.y = -1 → sign +1; the sampled NDC depth is already
        // 0..1 → scale 1, bias 0. (Metal: -1/1/0. GL: +1/2/-1.)
        tu.conv[0] = 1.0f;
        tu.conv[1] = 1.0f;
        tu.conv[2] = 0.0f;
        tu.conv[3] = 0.1f;
        tu.vp[0] = static_cast<float>(tw);
        tu.vp[1] = static_cast<float>(th);
        std::memcpy(m_ssrUBO[fi].mapped, &tu, sizeof(tu));

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_ssrPool[fi];
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_ssrTraceSetLayout;
        if (vkAllocateDescriptorSets(m_device, &dsai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

        // The radiance history is point-sampled from the temporal tier upward
        // (Metal binds its point sampler there); at quality 0 the very same image
        // is what the scene shader upsamples to full screen, and NEAREST would
        // show as half-res stair-steps in the mirror.
        const VkSampler histSampler = temporal ? m_ssrPointSampler : m_ssrLinearSampler;
        VkDescriptorImageInfo colorII{ m_ssrLinearSampler, m_ssrColorHist.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo attrII { m_ssrPointSampler,  m_reflAttrRT.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo ndcII  { m_ssrPointSampler,  m_reflNdcRT.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo hRadII { histSampler,        m_ssrHistRad[prevIdx].view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo hPosII { m_ssrPointSampler,  m_ssrHistPos[prevIdx].view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorBufferInfo uboBI { m_ssrUBO[fi].buf, 0, sizeof(tu) };
        VkWriteDescriptorSet wr[6]{};
        auto sampWrite = [&](int i, uint32_t binding, const VkDescriptorImageInfo* ii) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = set;
            wr[i].dstBinding = binding; wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = ii;
        };
        sampWrite(0, 19, &colorII);
        sampWrite(1, 20, &attrII);
        sampWrite(2, 22, &ndcII);
        sampWrite(3, 24, &hRadII);
        sampWrite(4, 25, &hPosII);
        wr[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[5].dstSet = set;
        wr[5].dstBinding = 23; wr[5].descriptorCount = 1;
        wr[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[5].pBufferInfo = &uboBI;
        vkUpdateDescriptorSets(m_device, 6, wr, 0, nullptr);

        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpbi.renderPass        = m_ssrTraceRP;
        rpbi.framebuffer       = m_ssrHistFB[curIdx];
        rpbi.renderArea.extent = { tw, th };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrTracePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrTracePipeLayout,
                                0, 1, &set, 0, nullptr);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        ++m_statDraws;

        m_ssrHistIdx      = prevIdx;
        m_ssrHistValid    = true;
        m_ssrPrevViewProj = viewProj;
    }

    VkImageView result = m_ssrHistRad[curIdx].view;

    // ── 2. Blur chain — the same tier policy as Metal's and GL's forward
    // variant. No wide/roughness-mix stage: the forward path has none there
    // either (that shader serves the GI reflections), and the pre-pass carries
    // no roughness to lerp against.
    if (m_ssrQuality >= 1)
    {
        uint32_t slot = 1; // slot 0 is the trace's UBO
        auto blurPass = [&](VkImageView src, const SSAORenderTarget& dst, float dx, float dy) -> bool {
            if (slot >= k_ssrUboSlots) return false;
            HE::MaterialShaderLibrary::SSRBlurUniforms bu;
            bu.dir[0] = dx;
            bu.dir[1] = dy;
            bu.dir[2] = 1.0f / static_cast<float>(tw);
            bu.dir[3] = 1.0f / static_cast<float>(th);
            const VkDeviceSize off = static_cast<VkDeviceSize>(slot) * k_ssrUboStride;
            std::memcpy(static_cast<uint8_t*>(m_ssrUBO[fi].mapped) + off, &bu, sizeof(bu));
            ++slot;

            VkDescriptorSet bset = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo bdsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            bdsai.descriptorPool     = m_ssrPool[fi];
            bdsai.descriptorSetCount = 1;
            bdsai.pSetLayouts        = &m_ssrBlurSetLayout;
            if (vkAllocateDescriptorSets(m_device, &bdsai, &bset) != VK_SUCCESS) return false;
            VkDescriptorImageInfo srcII{ m_ssrLinearSampler, src,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bBI{ m_ssrUBO[fi].buf, off, sizeof(bu) };
            VkWriteDescriptorSet bw[2]{};
            bw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; bw[0].dstSet = bset;
            bw[0].dstBinding = 19; bw[0].descriptorCount = 1;
            bw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bw[0].pImageInfo = &srcII;
            bw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; bw[1].dstSet = bset;
            bw[1].dstBinding = 23; bw[1].descriptorCount = 1;
            bw[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; bw[1].pBufferInfo = &bBI;
            vkUpdateDescriptorSets(m_device, 2, bw, 0, nullptr);

            VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpbi.renderPass        = m_ssrBlurRP;
            rpbi.framebuffer       = dst.fb;
            rpbi.renderArea.extent = { tw, th };
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrBlurPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssrBlurPipeLayout,
                                    0, 1, &bset, 0, nullptr);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            ++m_statDraws;
            return true;
        };
        const float sx = 1.0f / static_cast<float>(tw);
        const float sy = 1.0f / static_cast<float>(th);
        bool ok = blurPass(result, m_ssrPingRT, sx, 0.0f)
               && blurPass(m_ssrPingRT.view, m_ssrReflRT, 0.0f, sy);
        if (ok && m_ssrQuality == 1)
            ok = blurPass(m_ssrReflRT.view, m_ssrPingRT, sx, 0.0f)
              && blurPass(m_ssrPingRT.view, m_ssrReflRT, 0.0f, sy);
        if (ok) result = m_ssrReflRT.view;
    }
    return result;
#endif
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
    if (createGraphicsPipelines(1, &pci, nullptr, &m_skinnedPipeline) != VK_SUCCESS)
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: skinned pipeline creation failed");

    // ── HDR variant (renderPass = m_postFxSceneRP, RGBA16F) ──────────────────
    if (m_postFxSceneRP != VK_NULL_HANDLE)
    {
        VkGraphicsPipelineCreateInfo hdrPci = pci;
        hdrPci.renderPass = m_postFxSceneRP;
        if (createGraphicsPipelines(1, &hdrPci, nullptr, &m_skinnedPipelineHDR) != VK_SUCCESS)
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

    // Device-local, filled through the upload batch (REN-803): the GPU skins
    // from these, the CPU never writes them again.
    const uint64_t t0 = uploadClockNs();
    const VkDeviceSize vbBytes  = interleaved.size() * sizeof(float);
    const VkDeviceSize idBytes  = boneIds.size()  * sizeof(uint32_t);
    const VkDeviceSize wgtBytes = boneWgts.size() * sizeof(float);
    const VkDeviceSize ibBytes  = asset->indices.size() * sizeof(uint32_t);
    GpuSkeletalMesh sm;
    bool ok = true;
    ok = ok && createDeviceLocalBuffer(vbBytes,  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       interleaved.data(), sm.vb, sm.vbAlloc);
    ok = ok && createDeviceLocalBuffer(idBytes,  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       boneIds.data(),  sm.boneIdVb, sm.boneIdAlloc);
    ok = ok && createDeviceLocalBuffer(wgtBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       boneWgts.data(), sm.boneWgtVb, sm.boneWgtAlloc);
    ok = ok && createDeviceLocalBuffer(ibBytes,  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       asset->indices.data(), sm.ib, sm.ibAlloc);
    if (!ok)
    {
        HE_LOG_ERROR(RHI, "%s", "VulkanRenderer: resolveSkeletalMesh buffer creation failed");
        // Partial cleanup: the copies of whatever was created are recorded in the
        // open upload batch, so let that finish first (rare: out of memory).
        deviceWaitIdle();
        destroyPooledBuffer(sm.vb,        sm.vbAlloc);
        destroyPooledBuffer(sm.boneIdVb,  sm.boneIdAlloc);
        destroyPooledBuffer(sm.boneWgtVb, sm.boneWgtAlloc);
        destroyPooledBuffer(sm.ib,        sm.ibAlloc);
        return nullptr;
    }
    noteUpload(true, vbBytes + idBytes + wgtBytes + ibBytes, t0);

    sm.indexCount = static_cast<int>(asset->indices.size());
    // Upload the skeletal mesh's baked base-color texture (if any), same as static meshes.
    sm.hasTex = resolveAndUploadAlbedo(asset->materialId, asset->materialPath,
                                       sm.texImage, sm.texMem, sm.texView, sm.albedoSet);
    return &m_skeletalMeshCache.emplace(id, sm).first->second;
}

void VulkanRenderer::destroySkeletalMeshCache()
{
    for (auto& [id, sm] : m_skeletalMeshCache)
    {
        destroyPooledBuffer(sm.vb,        sm.vbAlloc);
        destroyPooledBuffer(sm.boneIdVb,  sm.boneIdAlloc);
        destroyPooledBuffer(sm.boneWgtVb, sm.boneWgtAlloc);
        destroyPooledBuffer(sm.ib,        sm.ibAlloc);
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

    // Linear clamp sampler for the R8 font atlases and the UI quad images
    // (immutable in the set layout). Trilinear like GL's UI textures: an image
    // brings its cooked mip chain, an atlas has one level and reads it as before.
    {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
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

        // UI quad images (resolveUIImageSet): same layout, own pool, FREE bit so
        // an invalidated texture gives its set back.
        VkDescriptorPoolSize ips{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 };
        VkDescriptorPoolCreateInfo ipci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ipci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        ipci.maxSets       = 256;
        ipci.poolSizeCount = 1;
        ipci.pPoolSizes    = &ips;
        vkCheck(vkCreateDescriptorPool(m_device, &ipci, nullptr, &m_uiImageDescPool), "ui image desc pool");
    }

    // Push constant layout: UIPush (128 bytes, the guaranteed minimum of
    // maxPushConstantsSize) visible to both stages.
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset     = 0;
    // vec4 rect + vec4 color + vec4 uvRect + vec2 viewport + vec2 params + vec4 rotation
    // + vec4 cornerRadius + vec4 style + uvec4 packed colours (see ui.frag)
    pcr.size       = 128;

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
        vkCheck(createGraphicsPipelines(1, &pci, nullptr, &out), "ui pipeline");
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
    // Caller is responsible for binding the correct pipeline and setting
    // viewport/scissor before calling. This function only loops over UI objects
    // and issues draw calls — it does NOT begin/end a render pass.
    struct UIPush { glm::vec4 rect; glm::vec4 color; glm::vec4 uvRect; glm::vec2 viewport;
                    glm::vec2 params; glm::vec4 rotation;
                    glm::vec4 cornerRadius; glm::vec4 style; glm::uvec4 colors; };
    static_assert(sizeof(UIPush) == 128, "UIPush must match ui.vert/ui.frag and the 128-byte range");

    // A set must be bound for EVERY draw (the fragment shader statically uses
    // the sampler even for solid quads). Default to the shared font (key 0);
    // glyph quads re-bind their font's atlas, textured quads their image, and
    // solid quads keep whatever is bound (mode 0 ignores it).
    VkDescriptorSet atlasSet = uiFontAtlasSet(0);
    if (!atlasSet) return;  // device-level upload failure — nothing valid to bind
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeLayout,
                            0, 1, &atlasSet, 0, nullptr);
    VkDescriptorSet boundSet = atlasSet;
    auto bindSet = [&](VkDescriptorSet s)
    {
        if (s == boundSet) return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_uiPipeLayout,
                                0, 1, &s, 0, nullptr);
        boundSet = s;
    };

    // Clipping is a scissor rectangle, set only when it CHANGES — a widget tree
    // emits its quads in tree order, so equally-clipped quads arrive in runs.
    // The UI pipeline declares scissor dynamic, so this needs no pipeline of its
    // own; the rect is clamped because Vulkan requires it inside the framebuffer.
    glm::vec4 appliedClip(-1.0f);
    auto applyClip = [&](const glm::vec4& c)
    {
        if (c == appliedClip) return;
        appliedClip = c;
        VkRect2D s{};
        if (c.z <= 0.0f)
        {
            s.offset = { 0, 0 };
            s.extent = { static_cast<uint32_t>(std::max(0, width)),
                         static_cast<uint32_t>(std::max(0, height)) };
        }
        else
        {
            const float x0 = std::clamp(c.x, 0.0f, static_cast<float>(width));
            const float y0 = std::clamp(c.y, 0.0f, static_cast<float>(height));
            const float x1 = std::clamp(c.x + c.z, 0.0f, static_cast<float>(width));
            const float y1 = std::clamp(c.y + c.w, 0.0f, static_cast<float>(height));
            s.offset = { static_cast<int32_t>(x0), static_cast<int32_t>(y0) };
            s.extent = { static_cast<uint32_t>(std::max(0.0f, x1 - x0)),
                         static_cast<uint32_t>(std::max(0.0f, y1 - y0)) };
        }
        vkCmdSetScissor(cmd, 0, 1, &s);
    };

    for (const UIRenderObject& obj : m_renderWorld.uiObjects)
    {
        applyClip(obj.clipRect);
        // A glyph quad may use an imported font's atlas — bind its set.
        if (obj.type == 2)
        {
            if (VkDescriptorSet s = uiFontAtlasSet(obj.fontAtlasKey)) bindSet(s);
        }
        // A textured quad (Image widget, textured Panel/Border/Button) samples its
        // own image in mode 2; unresolvable → its tint, as on GL (mode 0).
        bool textured = false;
        if (obj.type == 0 && obj.textureAssetId != HE::UUID{})
        {
            if (VkDescriptorSet s = resolveUIImageSet(obj.textureAssetId))
            {
                bindSet(s);
                textured = true;
            }
        }
        UIPush push{};
        push.rect     = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
        push.color    = glm::vec4(obj.color.r, obj.color.g, obj.color.b, obj.color.a);
        push.uvRect   = glm::vec4(obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y);
        push.viewport = glm::vec2(float(width), float(height));
        push.params   = glm::vec2(obj.type == 2 ? 1.0f : (textured ? 2.0f : 0.0f), obj.borderWidth);
        push.rotation = glm::vec4(obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y,
                                  obj.gradientAngleDeg);
        push.cornerRadius = obj.cornerRadius;
        push.style    = glm::vec4(obj.blur, obj.innerShadowBlur, obj.gradient ? 1.0f : 0.0f,
                                  obj.gradientShape == 1 ? 1.0f : 0.0f);
        push.colors   = glm::uvec4(glm::packUnorm4x8(obj.borderColor),
                                   glm::packUnorm4x8(obj.gradientColor),
                                   glm::packUnorm4x8(obj.innerShadowColor), 0u);
        vkCmdPushConstants(cmd, m_uiPipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(UIPush), &push);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
    // The scissor is command-buffer state: hand it back covering everything.
    applyClip(glm::vec4(0.0f));
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
    submitGraphics(si, VK_NULL_HANDLE);
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

// The image of a UI quad (Image widget, textured Border/Button) as a set in the
// font-atlas layout — Vulkan's ResolveUITexture. Uploaded UNORM even for an
// sRGB-flagged asset: UI colours are sRGB numbers end to end, an _SRGB view would
// decode them and the picture comes out too dark ("the orange logo turns red",
// Thema 107). Not shared with m_graphTexCache on purpose: a material samples the
// same asset in linear light, which is right there. A miss is cached like GL's
// (resolveTextureRef loads synchronously: still null = unloadable).
VkDescriptorSet VulkanRenderer::resolveUIImageSet(const HE::UUID& id)
{
    const std::string key = graphTexKey(id, {});
    if (auto it = m_uiImageCache.find(key); it != m_uiImageCache.end())
        return it->second.set;
    if (!m_contentManager || !m_uiAtlasDSLayout || !m_uiImageDescPool) return VK_NULL_HANDLE;

    UIFontAtlas img;
    if (const TextureAsset* tex = m_contentManager->resolveTextureRef(id, std::string{}))
        uploadTextureImage(tex, img.image, img.memory, img.view, /*asArray=*/false, /*honourSrgb=*/false);
    if (img.view)
    {
        VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsai.descriptorPool     = m_uiImageDescPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_uiAtlasDSLayout;
        if (vkAllocateDescriptorSets(m_device, &dsai, &img.set) != VK_SUCCESS)
        {
            img.set = VK_NULL_HANDLE;
            if (!m_uiImagePoolWarned)
            {
                m_uiImagePoolWarned = true;
                HE_LOG_WARN(RHI, "%s", "VulkanRenderer: UI image descriptor pool exhausted "
                                       "(256 images) — further UI images draw their tint");
            }
        }
        else
        {
            VkDescriptorImageInfo dii{ VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet wr{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            wr.dstSet          = img.set;
            wr.dstBinding      = 0;
            wr.descriptorCount = 1;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr.pImageInfo      = &dii;
            vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
        }
    }
    return m_uiImageCache.emplace(key, img).first->second.set;
}

void VulkanRenderer::destroyUIImage(UIFontAtlas& img)
{
    if (img.set)    vkFreeDescriptorSets(m_device, m_uiImageDescPool, 1, &img.set); // pool has the FREE bit
    if (img.view)   vkDestroyImageView(m_device, img.view,   nullptr);
    if (img.image)  vkDestroyImage    (m_device, img.image,  nullptr);
    if (img.memory) vkFreeMemory      (m_device, img.memory, nullptr);
    img = UIFontAtlas{};
}

void VulkanRenderer::destroyUIImages()
{
    for (auto& [key, img] : m_uiImageCache) destroyUIImage(img);
    m_uiImageCache.clear();
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
    qpci.queryCount = kGpuTimerRing * GpuPassTimeline::kMaxTimestamps;   // the marks of every ring slot
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
    GpuPassTimeline& tl = m_tsTimeline[slot];
    if (m_tsPending[slot])
    {
        const uint32_t n = tl.used();
        uint64_t r[GpuPassTimeline::kMaxTimestamps * 2] = {};   // {value, availability} per mark
        const VkResult res = vkGetQueryPoolResults(m_device, m_tsQueryPool,
            slot * GpuPassTimeline::kMaxTimestamps, n, sizeof(r), r, 2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        bool all = res == VK_SUCCESS && n >= 2;
        uint64_t ticks[GpuPassTimeline::kMaxTimestamps];
        for (uint32_t i = 0; all && i < n; ++i)
        {
            all = r[2 * i + 1] != 0;
            ticks[i] = r[2 * i] & m_tsValidMask;
        }
        if (all)
        {
            const uint64_t t0 = ticks[0], t1 = ticks[n - 1];
            FrameGpuStats fs;
            fs.gpuFrameMs = (t1 > t0)
                ? static_cast<double>(t1 - t0) * static_cast<double>(m_tsPeriodNs) * 1e-6
                : 0.0;
            tl.toPasses(ticks, 1e9 / static_cast<double>(m_tsPeriodNs), fs.passes);
            fs.gpuTimingMode = fs.passes.empty() ? "whole-frame" : "vk-ts";
            m_lastGpuStats = fs;                // CPU counters merged by GetFrameGpuStats
        }
        m_tsPending[slot] = false;   // reset below invalidates the old results either way
    }

    vkCmdResetQueryPool(cmd, m_tsQueryPool, slot * GpuPassTimeline::kMaxTimestamps,
                        GpuPassTimeline::kMaxTimestamps);
    tl.reset();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_tsQueryPool,
                        slot * GpuPassTimeline::kMaxTimestamps + tl.start());
    m_tsCmd = cmd;
    // Per-pass marks only for a frame somebody reads them from: they make the GPU drain at every pass boundary.
    const EngineProfiler& prof = EngineProfiler::instance();
    m_tsPassOn = (prof.isRecording() || prof.liveEnabled()) && GpuPassTimeline::envEnabled();
}

void VulkanRenderer::gpuMark(uint32_t i)
{
    const uint32_t slot = static_cast<uint32_t>(m_tsFrameIdx % kGpuTimerRing);
    vkCmdWriteTimestamp(m_tsCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_tsQueryPool,
                        slot * GpuPassTimeline::kMaxTimestamps + i);
}

bool VulkanRenderer::gpuPassBegin(const char* name)
{
    if (!m_tsSupported || m_tsCmd == VK_NULL_HANDLE || !m_tsPassOn) return false;
    uint32_t i = 0;
    if (!m_tsTimeline[m_tsFrameIdx % kGpuTimerRing].push(name, i)) return false;
    gpuMark(i);
    return true;
}

void VulkanRenderer::gpuPassEnd()
{
    uint32_t i = 0;
    if (m_tsCmd != VK_NULL_HANDLE && m_tsTimeline[m_tsFrameIdx % kGpuTimerRing].pop(i)) gpuMark(i);
}

void VulkanRenderer::gpuTimerEnd(VkCommandBuffer cmd)
{
    if (!m_tsSupported) return;
    const uint32_t slot = static_cast<uint32_t>(m_tsFrameIdx % kGpuTimerRing);
    gpuMark(m_tsTimeline[slot].finish());
    m_tsCmd = VK_NULL_HANDLE;
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
