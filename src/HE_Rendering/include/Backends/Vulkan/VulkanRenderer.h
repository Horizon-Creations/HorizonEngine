#pragma once
#include <Renderer/IRenderer.h>
#include "VulkanShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>

struct SDL_Window;

class VulkanRenderer : public IRenderer
{
public:
	VulkanRenderer();
	~VulkanRenderer();
	void Initialize(HE::Window* window) override;
	void Shutdown()                      override;
	void Render()                        override;
	Capabilities GetCapabilities() const override;

	// Multi-window support
	void AttachWindow(HE::Window* window) override;
	void DetachWindow(HE::Window* window) override;
	void RenderWindow(HE::Window* window) override;

	void*    GetInstance()       const;
	void*    GetPhysicalDevice() const;
	void*    GetDevice()         const;
	void*    GetQueue()          const;
	uint64_t GetRenderPass()     const;
	uint32_t GetQueueFamily()    const;
	uint32_t GetImageCount()     const;

	void SetVSync(bool enabled) override;

	void SetDebugLines(const std::vector<DebugLine>& lines) override;
	void SetMoonTexture(const void* rgba8Pixels, int width, int height) override;

	// Offscreen viewport (editor scene view)
	void  SetViewportSize(uint32_t width, uint32_t height) override;
	// GetViewportTexture() is inherited from IRenderer; returns m_viewportImGuiHandle
	// which is a VkDescriptorSet registered by the editor via SetViewportImGuiHandle.
	bool  CaptureViewport(std::vector<uint8_t>& rgba,
	                      uint32_t& width, uint32_t& height) override;
	// Returns VkImageView for the viewport color image (for ImGui_ImplVulkan_AddTexture).
	void* GetViewportVkImageView() const;
	void* GetViewportVkSampler()   const;
	bool  HasViewportResourceChanged() const;
	void  ClearViewportResourceChanged();

private:
	void createInstance();
	void createSurface();
	void pickPhysicalDevice();
	void createDevice();
	void createSwapchain(uint32_t w, uint32_t h);
	void createRenderPass();
	void createFramebuffers();
	void createCommandBuffers();
	void createSyncObjects();
	void destroySwapchain();
	// Rebuild the swapchain + depth + framebuffers + command buffers for the
	// window's current size. Called on resize (vkAcquire/Present reports
	// OUT_OF_DATE/SUBOPTIMAL) and on VSync changes.
	void recreateSwapchain();

	// ── Scene draw path ─────────────────────────────────────────────────────
	void           createDepthResources();
	void           destroyDepthResources();
	void           createScenePipeline();
	void           destroyScenePipeline();
	void           DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool hdr = false);
	VkShaderModule loadShaderModule(const char* spvFileName);
	uint32_t       findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

	// ── Shadow map ──────────────────────────────────────────────────────────
	void createShadowResources();
	void destroyShadowResources();
	void EncodeShadowMap(VkCommandBuffer cmd); // own render pass, before the scene
	VkImage        m_shadowImage    = VK_NULL_HANDLE;
	VkDeviceMemory m_shadowMemory   = VK_NULL_HANDLE;
	VkImageView    m_shadowView     = VK_NULL_HANDLE;
	VkSampler      m_shadowSampler  = VK_NULL_HANDLE;
	VkRenderPass   m_shadowPass     = VK_NULL_HANDLE;
	VkFramebuffer  m_shadowFB       = VK_NULL_HANDLE;
	VkPipeline     m_shadowPipeline = VK_NULL_HANDLE;
	uint32_t       m_shadowSize     = 2048;

	VkInstance               m_instance       = VK_NULL_HANDLE;
	VkPhysicalDevice         m_physDevice     = VK_NULL_HANDLE;
	VkDevice                 m_device         = VK_NULL_HANDLE;
	VkQueue                  m_graphicsQueue  = VK_NULL_HANDLE;
	uint32_t                 m_graphicsFamily = 0;
	VkSurfaceKHR             m_surface        = VK_NULL_HANDLE;
	VkSwapchainKHR           m_swapchain      = VK_NULL_HANDLE;
	VkFormat                 m_swapFormat{};
	VkExtent2D               m_swapExtent{};
	std::vector<VkImage>     m_swapImages;
	std::vector<VkImageView> m_swapViews;
	VkRenderPass             m_renderPass     = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> m_framebuffers;
	VkCommandPool            m_cmdPool        = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_cmdBufs;

	VkSemaphore m_imageReady[2]{};
	VkSemaphore m_renderDone[2]{};
	VkFence     m_frameFence[2]{};
	uint32_t    m_currentFrame = 0;

	// ── Depth buffer (shared, transient) ────────────────────────────────────
	VkImage        m_depthImage  = VK_NULL_HANDLE;
	VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
	VkImageView    m_depthView   = VK_NULL_HANDLE;
	VkFormat       m_depthFormat = VK_FORMAT_D32_SFLOAT;

	// ── Scene pipeline ──────────────────────────────────────────────────────
	// Per-object data (MVP + model) goes through push constants; per-frame data
	// (camera + lights) through a host-visible UBO bound via a descriptor set,
	// one per frame in flight.
	VkDescriptorSetLayout m_sceneSetLayout             = VK_NULL_HANDLE;
	VkPipelineLayout      m_scenePipelineLayout        = VK_NULL_HANDLE;
	VkPipeline            m_scenePipeline              = VK_NULL_HANDLE;
	VkPipeline            m_sceneTransparentPipeline   = VK_NULL_HANDLE;
	VkPipeline            m_scenePipelineHDR           = VK_NULL_HANDLE;
	VkPipeline            m_sceneTransparentPipelineHDR= VK_NULL_HANDLE;
	VkDescriptorPool      m_descPool            = VK_NULL_HANDLE;
	struct FrameUBO
	{
		VkBuffer        buf    = VK_NULL_HANDLE;
		VkDeviceMemory  mem    = VK_NULL_HANDLE;
		void*           mapped = nullptr;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	FrameUBO m_frameUBO[2];

	// Per-draw material data (32 bytes: baseColor(rgb)+metallic(a) + roughness + pad).
	// Updated per-draw via vkCmdUpdateBuffer; binding 2 in scene descriptor set.
	VkBuffer       m_matUBO    = VK_NULL_HANDLE;
	VkDeviceMemory m_matMem    = VK_NULL_HANDLE;

	struct GpuMesh
	{
		VkBuffer       vbuf       = VK_NULL_HANDLE;
		VkDeviceMemory vmem       = VK_NULL_HANDLE;
		VkBuffer       ibuf       = VK_NULL_HANDLE;
		VkDeviceMemory imem       = VK_NULL_HANDLE;
		uint32_t       indexCount = 0;
		HE::AABB       localBounds;
	};
	GpuMesh                               m_cube;
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;
	bool createMeshBuffers(GpuMesh& mesh, const std::vector<float>& interleaved,
	                       const std::vector<uint32_t>& indices);
	const GpuMesh* resolveMesh(const HE::UUID& assetId);
	void createCube();

	RenderExtractor m_extractor;
	RenderWorld     m_renderWorld;
	FrustumCuller   m_culler;
	RenderSorter    m_sorter;
	RenderGraph     m_renderGraph;
	CommandBuffer   m_cmds;
	std::vector<uint8_t>  m_visible;
	std::vector<uint32_t> m_sortedIndices;

	// ── Viewport offscreen render target ────────────────────────────────────
	// Color image sampled by ImGui; depth image for the viewport render pass.
	void createViewportResources(uint32_t w, uint32_t h);
	void destroyViewportResources();
	VkImage        m_viewportImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_viewportMemory  = VK_NULL_HANDLE;
	VkImageView    m_viewportView    = VK_NULL_HANDLE;
	VkImage        m_viewportDepthImage  = VK_NULL_HANDLE;
	VkDeviceMemory m_viewportDepthMemory = VK_NULL_HANDLE;
	VkImageView    m_viewportDepthView   = VK_NULL_HANDLE;
	VkRenderPass   m_viewportRenderPass  = VK_NULL_HANDLE;
	VkFramebuffer  m_viewportFramebuffer = VK_NULL_HANDLE;
	VkSampler      m_viewportSampler     = VK_NULL_HANDLE;
	uint32_t       m_viewportW        = 0;
	uint32_t       m_viewportH        = 0;
	uint32_t       m_viewportReqW     = 0;
	uint32_t       m_viewportReqH     = 0;
	bool           m_viewportResChanged = false;
	VkImageLayout  m_viewportLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

	// ── PostFX pipeline ─────────────────────────────────────────────────────
	void createPostFXResources(uint32_t w, uint32_t h);
	void destroyPostFXResources();
	void createPostFXPipelines();
	void destroyPostFXPipelines();
	void runPostFXBarrier(VkCommandBuffer cmd, VkImage img,
	                      VkImageLayout from, VkImageLayout to,
	                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

	VkRenderPass m_postFxSceneRP  = VK_NULL_HANDLE;
	VkRenderPass m_postFxBlitF16  = VK_NULL_HANDLE;
	VkRenderPass m_postFxBlitF8   = VK_NULL_HANDLE;
	VkRenderPass m_postFxFinalRP  = VK_NULL_HANDLE;

	VkImage        m_hdrImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_hdrMemory  = VK_NULL_HANDLE;
	VkImageView    m_hdrView    = VK_NULL_HANDLE;
	VkFramebuffer  m_hdrFB      = VK_NULL_HANDLE;
	VkImageLayout  m_hdrLayout  = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage        m_bloomImage[2]  = {};
	VkDeviceMemory m_bloomMemory[2] = {};
	VkImageView    m_bloomView[2]   = {};
	VkFramebuffer  m_bloomFB[2]     = {};
	VkImageLayout  m_bloomLayout[2] = { VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED };

	VkImage        m_ldrImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_ldrMemory  = VK_NULL_HANDLE;
	VkImageView    m_ldrView    = VK_NULL_HANDLE;
	VkFramebuffer  m_ldrFB      = VK_NULL_HANDLE;
	VkImageLayout  m_ldrLayout  = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage        m_dummyImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_dummyMemory  = VK_NULL_HANDLE;
	VkImageView    m_dummyView    = VK_NULL_HANDLE;

	VkFramebuffer  m_fxaaFB    = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_postFxDSLayout   = VK_NULL_HANDLE;
	VkDescriptorPool      m_postFxDSPool     = VK_NULL_HANDLE;
	VkDescriptorSet       m_postFxDS[5]      = {};

	VkSampler             m_postFxSampler    = VK_NULL_HANDLE;
	VkPipelineLayout      m_postFxPipeLayout = VK_NULL_HANDLE;
	VkPipeline            m_bloomBrightPipe  = VK_NULL_HANDLE;
	VkPipeline            m_bloomBlurPipe    = VK_NULL_HANDLE;
	VkPipeline            m_tonemapPipe      = VK_NULL_HANDLE;
	VkPipeline            m_fxaaPipe         = VK_NULL_HANDLE;

	bool   m_postFxReady     = false;
	float  m_exposure        = 1.0f;
	float  m_bloomStrength   = 0.25f;
	float  m_bloomThreshold  = 1.0f;
	float  m_bloomKnee       = 0.1f;

	// ── Per-secondary-window resources ──────────────────────────────────────
	struct WindowData
	{
		VkSurfaceKHR             surface     = VK_NULL_HANDLE;
		VkSwapchainKHR           swapchain   = VK_NULL_HANDLE;
		VkFormat                 swapFormat{};
		VkExtent2D               swapExtent{};
		std::vector<VkImage>     swapImages;
		std::vector<VkImageView> swapViews;
		VkRenderPass             renderPass  = VK_NULL_HANDLE;
		std::vector<VkFramebuffer> framebuffers;
		VkCommandPool            cmdPool     = VK_NULL_HANDLE;
		std::vector<VkCommandBuffer> cmdBufs;
		VkSemaphore imageReady[2]{};
		VkSemaphore renderDone[2]{};
		VkFence     frameFence[2]{};
		uint32_t    currentFrame = 0;
	};

	// Extra windows keyed by their native SDL_Window pointer
	std::unordered_map<SDL_Window*, WindowData> m_extraWindows;

	// Helpers for secondary windows
	void createWindowData(SDL_Window* sdlWin, uint32_t w, uint32_t h, WindowData& wd);
	void destroyWindowData(WindowData& wd);
	void renderWindowData(WindowData& wd);

	SDL_Window* m_sdlWindow = nullptr;
	bool        m_vsync = true;

	VulkanShaderManager m_shaderManager;

	// ── Sky rendering ────────────────────────────────────────────────────────
	void createSkyPipeline();
	void destroySkyPipeline();
	void drawSky(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool hdr);

	// Per-frame sky UBO (mirrors FrameUBO pattern).
	struct SkyUBO
	{
		VkBuffer        buf    = VK_NULL_HANDLE;
		VkDeviceMemory  mem    = VK_NULL_HANDLE;
		void*           mapped = nullptr;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	SkyUBO                m_skyUBO[2];
	VkDescriptorSetLayout m_skyDSLayout        = VK_NULL_HANDLE;
	VkDescriptorPool      m_skyDSPool          = VK_NULL_HANDLE;
	VkPipelineLayout      m_skyPipelineLayout  = VK_NULL_HANDLE;
	VkPipeline            m_skyPipeline        = VK_NULL_HANDLE;
	VkPipeline            m_skyPipelineHDR     = VK_NULL_HANDLE;

	// Moon texture (uploaded once via SetMoonTexture).
	VkImage        m_moonImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_moonMemory  = VK_NULL_HANDLE;
	VkImageView    m_moonView    = VK_NULL_HANDLE;
	VkSampler      m_moonSampler = VK_NULL_HANDLE;

	// Wall-clock time (seconds) updated each frame by Render().
	float m_wallTime = 0.0f;

	// ── Debug line rendering ─────────────────────────────────────────────────
	void createDebugLinePipeline();
	void destroyDebugLinePipeline();
	void drawDebugLines(VkCommandBuffer cmd, const glm::mat4& viewProj, bool hdr = false);

	// Per-frame debug UBO + vertex buffer.
	struct DebugUBO
	{
		VkBuffer        buf    = VK_NULL_HANDLE;
		VkDeviceMemory  mem    = VK_NULL_HANDLE;
		void*           mapped = nullptr;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	DebugUBO              m_debugUBO[2];
	VkBuffer              m_debugVB[2]        = {};
	VkDeviceMemory        m_debugVBMem[2]     = {};
	void*                 m_debugVBMapped[2]  = {};
	VkDescriptorSetLayout m_debugDSLayout       = VK_NULL_HANDLE;
	VkDescriptorPool      m_debugDSPool         = VK_NULL_HANDLE;
	VkPipelineLayout      m_debugPipelineLayout  = VK_NULL_HANDLE;
	VkPipeline            m_debugPipeline        = VK_NULL_HANDLE;
	VkPipeline            m_debugPipelineHDR     = VK_NULL_HANDLE;
	std::vector<DebugLine> m_debugLines;

	// ── SSAO (screen-space ambient occlusion) ───────────────────────────────
	// Three render passes: (1) view-space position prepass, (2) SSAO compute,
	// (3) box blur.  All run before the scene HDR pass each frame.
	void createSSAOPipeline();
	void createSSAOTargets(uint32_t w, uint32_t h);
	void destroySSAOTargets();
	void runSSAO(VkCommandBuffer cmd, uint32_t w, uint32_t h);

	// Render-target bundle: color image + optional depth + framebuffer.
	struct SSAORenderTarget {
		VkImage        image  = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView    view   = VK_NULL_HANDLE;
		VkFramebuffer  fb     = VK_NULL_HANDLE;
	};
	SSAORenderTarget m_ssaoPosRT;    // RGBA16F: view-space position (color)
	SSAORenderTarget m_ssaoPosDepth; // D16_UNORM: depth for position prepass
	SSAORenderTarget m_ssaoRT;       // R8_UNORM: raw occlusion
	SSAORenderTarget m_ssaoBlurRT;   // R8_UNORM: blurred occlusion (bound to scene set)

	VkRenderPass m_ssaoPosRenderPass  = VK_NULL_HANDLE; // color(RGBA16F) + depth(D16)
	VkRenderPass m_ssaoRenderPass     = VK_NULL_HANDLE; // fullscreen AO pass  (R8)
	VkRenderPass m_ssaoBlurRenderPass = VK_NULL_HANDLE; // fullscreen blur pass (R8)

	// Position prepass: push-constant layout (reuses scene m_scenePipelineLayout).
	VkPipeline   m_ssaoPosGfxPipeline  = VK_NULL_HANDLE;

	// SSAO fullscreen pass descriptors (set=0: UBO + posRT + noise).
	VkDescriptorSetLayout m_ssaoDescLayout     = VK_NULL_HANDLE;
	VkPipelineLayout      m_ssaoPipeLayout     = VK_NULL_HANDLE;
	VkPipeline            m_ssaoGfxPipeline    = VK_NULL_HANDLE;

	// Blur fullscreen pass descriptors (set=0: AO input sampler).
	VkDescriptorSetLayout m_ssaoBlurDescLayout = VK_NULL_HANDLE;
	VkPipelineLayout      m_ssaoBlurPipeLayout = VK_NULL_HANDLE;
	VkPipeline            m_ssaoBlurGfxPipeline= VK_NULL_HANDLE;

	// Shared descriptor pool for SSAO + blur sets.
	VkDescriptorPool  m_ssaoDescPool    = VK_NULL_HANDLE;
	VkDescriptorSet   m_ssaoDescSet     = VK_NULL_HANDLE;
	VkDescriptorSet   m_ssaoBlurDescSet = VK_NULL_HANDLE;

	// SSAO UBO: SSAOCB (608 bytes: mat4 + vec4 + vec4 + vec4[32]).
	VkBuffer       m_ssaoUBO    = VK_NULL_HANDLE;
	VkDeviceMemory m_ssaoUBOMem = VK_NULL_HANDLE;
	void*          m_ssaoUBOPtr = nullptr;

	// 4x4 rotation noise texture (RGBA32F, NEAREST, REPEAT).
	VkImage        m_ssaoNoiseTex     = VK_NULL_HANDLE;
	VkDeviceMemory m_ssaoNoiseMem     = VK_NULL_HANDLE;
	VkImageView    m_ssaoNoiseView    = VK_NULL_HANDLE;
	VkSampler      m_ssaoNoiseSampler = VK_NULL_HANDLE; // NEAREST + REPEAT

	// 1x1 white R8_UNORM fallback bound at scene binding=3 when SSAO is off.
	VkImage        m_ssaoWhiteTex  = VK_NULL_HANDLE;
	VkDeviceMemory m_ssaoWhiteMem  = VK_NULL_HANDLE;
	VkImageView    m_ssaoWhiteView = VK_NULL_HANDLE;
	// Linear clamp sampler shared for all SSAO fullscreen passes.
	VkSampler      m_ssaoSampler   = VK_NULL_HANDLE;

	bool     m_ssaoReady        = false; // true once createSSAOPipeline() succeeded
	bool     m_ssaoEnabled      = true;
	bool     m_ssaoRanThisFrame = false; // set true by runSSAO(); cleared at top of Render()
	float    m_ssaoRadius   = 0.5f;
	float    m_ssaoBias     = 0.025f;
	float    m_ssaoIntensity= 1.5f;
	uint32_t m_ssaoW = 0, m_ssaoH = 0;
};
