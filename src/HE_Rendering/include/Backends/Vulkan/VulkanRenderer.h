#pragma once
#include <cstdint>
#include <Renderer/IRenderer.h>
#include "VulkanShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/GiBvh.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <material/MaterialShaderLibrary.h> // A4: shared cross-backend material shader layer
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <unordered_map>

struct SDL_Window;
struct TextureAsset; // ContentManager/Assets.h (full def included in the .cpp)

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
	void SetSSAOSettings(const SSAOSettings& s) override;
	void SetBloomSettings(const BloomSettings& s) override;
	void SetGISettings(const GISettings& s) override;

	// Editor material/mesh hot-reload: drop the cached override-material texture / mesh GPU
	// state so the next frame re-resolves it from the ContentManager (mirrors GL/Metal).
	void InvalidateMaterial(const HE::UUID& materialId) override;
	void InvalidateMesh(const HE::UUID& meshId) override;

	FrameGpuStats GetFrameGpuStats() const override;

	// ImGui editor textures (content-browser icons + logo). Uploads the RGBA8
	// pixels to a sampled VkImage (+ view + linear sampler), then hands the view +
	// sampler to the editor-installed registrar (m_imguiTexRegistrar) which calls
	// ImGui_ImplVulkan_AddTexture. Returns the VkDescriptorSet-backed ImTextureID,
	// or nullptr if no registrar is installed.
	void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
	void  DestroyImGuiTexture(void* handle) override;

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
	void createImageSyncObjects();   // per-swapchain-image present semaphores + in-flight fences
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
	// A4: node-graph material pipelines (built from MaterialShaderLibrary SPIR-V).
	// createMaterialResources()/destroyMaterialResources() are no-ops when the shader
	// cross-compiler (HE_HAVE_SHADERC) is absent; getOrBuildMaterialPipeline returns null.
	void           createMaterialResources();
	void           destroyMaterialResources();
	VkPipeline     getOrBuildMaterialPipeline(uint64_t hash, const std::string& frag,
	                                           const std::string& vertBody, bool hdr,
	                                           bool transparent);
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
	VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE; // validation → Logger (debug only)
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

	VkSemaphore m_imageReady[2]{};              // per frame-in-flight (acquire signal)
	std::vector<VkSemaphore> m_renderDone;      // per SWAPCHAIN IMAGE (present wait) —
	                                            // a per-frame present semaphore is reused
	                                            // while the swapchain may still hold it.
	VkFence     m_frameFence[2]{};              // per frame-in-flight (submit fence)
	std::vector<VkFence>     m_imagesInFlight;  // per image: the fence currently using it
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
	VkPipeline            m_sceneInstancedPipeline     = VK_NULL_HANDLE; // A3: real GPU instancing
	VkPipeline            m_sceneInstancedPipelineHDR  = VK_NULL_HANDLE; // A3
	// A3: per-frame instance-transform vertex buffer (host-visible, mapped; binding 1,
	// VK_VERTEX_INPUT_RATE_INSTANCE for the instanced scene pipeline). Ring per frame.
	struct InstanceBuf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr; };
	InstanceBuf           m_instanceBuf[2];
	static constexpr uint32_t k_maxInstances = 65536; // instance-buffer capacity (A3)
	static constexpr uint32_t k_instStride   = 128;   // bytes per instance = 2 × mat4 (mvp, model)
	VkDescriptorPool      m_descPool            = VK_NULL_HANDLE;
	struct FrameUBO
	{
		VkBuffer        buf    = VK_NULL_HANDLE;
		VkDeviceMemory  mem    = VK_NULL_HANDLE;
		void*           mapped = nullptr;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	FrameUBO m_frameUBO[2];

	// ── A4: node-graph material pipelines ────────────────────────────────────
	// Graph materials (Material-Node editor) render through per-material VkPipelines
	// built at draw time from MaterialShaderLibrary SPIR-V. All of this is dead weight
	// (never touched) when HE_HAVE_SHADERC is off: the member is default-constructed and
	// the draw path never calls it, so behaviour is identical to the built-in PBR path.
	// Canonical descriptor set 0 layout (matches the generated SPIR-V exactly):
	//   b0 UBO(FS) HeLighting | b1 UBO(VS) U | b2 tex(FS) heTex0 | b3 UBO(FS) HeParams
	//   b4..7 tex(FS) heTexP0..3 | b8/b9 UBO(VS) HeLighting/HeParams (WPO custom vertex).
	HE::MaterialShaderLibrary m_matShaderLib;
	std::unordered_map<uint64_t, VkPipeline> m_materialPipelines; // key = hash ^ hdr-bit
	VkDescriptorSetLayout m_matSetLayout      = VK_NULL_HANDLE;
	VkPipelineLayout      m_matPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorPool      m_matPool[2]        = {};  // per frame; reset whole each frame
	struct MatFrameBuf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr; };
	MatFrameBuf m_matLightBuf[2];   // HeLighting (64 B, filled once per frame)
	MatFrameBuf m_matObjBuf[2];     // U ring      (k_matMaxDraws × 256 B, one slot per draw)
	MatFrameBuf m_matParBuf[2];     // HeParams ring (k_matMaxDraws × 256 B, one slot per draw)
	uint32_t    m_matDrawCursor[2]  = {};    // per-frame ring/descriptor-set cursor
	bool        m_matReady          = false; // true once createMaterialResources() succeeded
	static constexpr uint32_t k_matMaxDraws   = 1024;
	static constexpr uint32_t k_matSlotStride = 256; // 256-B stride/slot for U + HeParams

	// Per-draw material data (32 bytes: baseColor(rgb)+metallic(a) + roughness + opacity
	// + hasTexture). Updated per-draw via vkCmdUpdateBuffer; binding 2 in scene descriptor set.
	VkBuffer       m_matUBO    = VK_NULL_HANDLE;
	VkDeviceMemory m_matMem    = VK_NULL_HANDLE;

	// ── Per-mesh base-color texture (descriptor set = 2) ─────────────────────
	// Each textured mesh owns a device-local RGBA8 image + view + a single combined-
	// image-sampler descriptor set, bound per draw. Untextured meshes share
	// m_whiteAlbedoSet (a 1x1 white default; MatUBO.roughPad.z = 0 selects flat colour).
	// m_emptySetLayout fills the scene pipeline's set-1 slot (skinned uses set 1 for bones),
	// so the shared scene.frag can reference uAlbedo at set 2 in both pipelines.
	static constexpr uint32_t k_maxMeshTextures = 1024; // per-mesh albedo descriptor-set cap
	VkDescriptorSetLayout m_albedoSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_emptySetLayout  = VK_NULL_HANDLE;
	VkSampler             m_albedoSampler   = VK_NULL_HANDLE; // linear + repeat
	VkDescriptorPool      m_albedoPool      = VK_NULL_HANDLE; // per-mesh albedo sets
	VkImage         m_whiteAlbedoImage = VK_NULL_HANDLE;
	VkDeviceMemory  m_whiteAlbedoMem   = VK_NULL_HANDLE;
	VkImageView     m_whiteAlbedoView  = VK_NULL_HANDLE;
	VkDescriptorSet m_whiteAlbedoSet   = VK_NULL_HANDLE;

	// ── MaterialComponent override + hot-reload (A2) ─────────────────────────
	// Override-material textures cached by material UUID (parallel to the baked per-mesh
	// texture): a draw's dc.materialAssetId, when its material asset is loaded, fully replaces
	// the mesh's baked texture — even to flat when the override has no texture (set == null →
	// m_whiteAlbedoSet), mirroring GL. Editor edits push UUIDs to the pending lists, drained by
	// processPendingInvalidations() at Render() top under a vkDeviceWaitIdle (invalidation is
	// editor-only; the idle keeps the free trivially safe). m_albedoPool has the FREE bit so
	// invalidated sets are recycled — no leak. slot placeholder in MaterialTexVk unused on Vk.
	struct MaterialTexVk {
		VkImage         image = VK_NULL_HANDLE;
		VkDeviceMemory  mem   = VK_NULL_HANDLE;
		VkImageView     view  = VK_NULL_HANDLE;
		VkDescriptorSet set   = VK_NULL_HANDLE; // null → override material has no texture (draw flat)
	};
	std::unordered_map<HE::UUID, MaterialTexVk> m_materialTexCache;
	std::vector<HE::UUID> m_pendingMatInval;
	std::vector<HE::UUID> m_pendingMeshInval;
	// Resolve an override material's texture (dc.materialAssetId), cached by UUID. Returns true
	// iff the material asset is loaded (out->set may be null = no texture → flat); false while
	// still loading (retry next frame, baked texture stays). Mirrors GL's ResolveMaterialTexture.
	bool resolveMaterialOverride(const HE::UUID& materialId, const MaterialTexVk*& out);
	void processPendingInvalidations();
	void destroyMaterialTex(MaterialTexVk& mt);

	struct GpuMesh
	{
		VkBuffer       vbuf       = VK_NULL_HANDLE;
		VkDeviceMemory vmem       = VK_NULL_HANDLE;
		VkBuffer       ibuf       = VK_NULL_HANDLE;
		VkDeviceMemory imem       = VK_NULL_HANDLE;
		uint32_t       indexCount = 0;
		HE::AABB       localBounds;
		// Base-color texture (null → untextured, draws with m_whiteAlbedoSet + flat colour).
		VkImage         albedoImage = VK_NULL_HANDLE;
		VkDeviceMemory  albedoMem   = VK_NULL_HANDLE;
		VkImageView     albedoView  = VK_NULL_HANDLE;
		VkDescriptorSet albedoSet   = VK_NULL_HANDLE; // set=2, owned by m_albedoPool
	};
	GpuMesh                               m_cube;
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;
	bool createMeshBuffers(GpuMesh& mesh, const std::vector<float>& interleaved,
	                       const std::vector<uint32_t>& indices);
	// Upload tightly-packed RGBA8 pixels to a device-local sampled image + view via a
	// one-shot command buffer (transition → copy → transition), mirroring the moon upload.
	bool uploadRGBA8Image(const uint8_t* rgba, uint32_t w, uint32_t h,
	                      VkImage& image, VkDeviceMemory& mem, VkImageView& view);
	// Upload a cooked TextureAsset — RGBA8 or a block format (BC7/BC3) — with its full
	// pre-baked mip chain to a device-local sampled image + view. Returns false when the
	// format isn't RGBA8/BC and this device can't sample it (caller then draws untextured).
	// Block formats need no runtime mip generation; the cook baked every level.
	bool uploadTextureImage(const TextureAsset* tex,
	                        VkImage& image, VkDeviceMemory& mem, VkImageView& view);
	// Resolve a mesh/skeletal asset's baked base-color texture (material → textureIds[0]) and
	// upload it to a device-local image + a set=2 descriptor set (shared by static + skinned).
	// Fills the out-params and returns true on success; leaves them null and returns false on
	// any miss, so the caller draws untextured. Mirrors D3D11/D3D12.
	bool resolveAndUploadAlbedo(const HE::UUID& materialId, const std::string& materialPath,
	                            VkImage& image, VkDeviceMemory& mem, VkImageView& view,
	                            VkDescriptorSet& set);
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
	// Old viewport color image/view/memory retired on resize: ImGui's descriptor is one
	// frame behind (editor updates it next frame), so the old image must outlive the
	// current frame's ImGui draw — else it samples a destroyed image (null view → TDR).
	struct RetiredViewport { VkImage img; VkImageView view; VkDeviceMemory mem; int frames; };
	std::vector<RetiredViewport> m_retiredViewports;
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
	bool   m_bloomEnabled    = true;
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

	// Sky 3D noise volume (RG16: R=value hash, G=Worley) baked once in
	// createSkyPipeline; sampled by sky.frag (binding 2) for the volumetric clouds.
	VkImage        m_skyNoiseImage   = VK_NULL_HANDLE;
	VkDeviceMemory m_skyNoiseMemory  = VK_NULL_HANDLE;
	VkImageView    m_skyNoiseView    = VK_NULL_HANDLE;
	VkSampler      m_skyNoiseSampler = VK_NULL_HANDLE;

	// ImGui editor textures (logo + content-browser icons). Each owns its own
	// image/view/memory/sampler; held here so they outlive ImGui's use of the
	// VkDescriptorSet the editor creates over them. Destroyed in Shutdown().
	struct ImGuiTexture {
		VkImage        image   = VK_NULL_HANDLE;
		VkImageView    view    = VK_NULL_HANDLE;
		VkDeviceMemory memory  = VK_NULL_HANDLE;
		VkSampler      sampler = VK_NULL_HANDLE;
	};
	std::vector<ImGuiTexture> m_imguiTextures;

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
	int      m_ssaoMethod   = 0;
	uint32_t m_ssaoW = 0, m_ssaoH = 0;

	// ── Global Illumination (software ray tracing, Checkpoint VK-A) ──────────
	// The CPU-built HE::GiBvh (same module + unit tests as the GL 4.3 port and
	// Metal's SW fallback) in SSBOs + a flat per-frame instance buffer (TLAS
	// analogue). The gi_shadow.comp/gi_probe.comp kernels that traverse these
	// land in VK-B/C — VK-A uploads only (inert, GI-off rendering unchanged),
	// which is why GetCapabilities still reports supportsGlobalIllumination
	// = false: flipping it on is the LAST step, once the full pipeline exists.
	// Compute is core Vulkan, so no extension/feature gate is needed.
	struct GiBlasRange
	{
		int32_t nodeOffset = 0, triOffset = 0;
		bool    valid      = false;
	};
	struct GiInstanceGpu // must match gi_shadow.comp/gi_probe.comp's GiInst (std430, 96 bytes)
	{
		glm::mat4 invTransform{1.0f};
		glm::vec4 baseColor{1.0f};
		int32_t   nodeOffset = 0, triOffset = 0, pad0 = 0, pad1 = 0;
	};
	struct GiBuffer // host-visible, persistently mapped
	{
		VkBuffer       buf    = VK_NULL_HANDLE;
		VkDeviceMemory mem    = VK_NULL_HANDLE;
		void*          mapped = nullptr;
		VkDeviceSize   size   = 0;
	};
	GiBlasRange  buildGiBlas(const HE::UUID& meshId);
	void         updateGiAccel();  // lazy BLAS append + per-frame instance upload
	void         destroyGiAccel();
	// (Re)creates a host-visible STORAGE_BUFFER of at least `size` bytes and
	// memcpys `data` into it. Grows by recreation; never shrinks.
	bool         uploadGiBuffer(GiBuffer& b, const void* data, VkDeviceSize size);
	std::unordered_map<HE::UUID, GiBlasRange> m_giBlasCache;
	std::vector<HE::GiBvhNode>     m_giNodesCpu;
	std::vector<HE::GiBvhTriangle> m_giTrisCpu;
	bool     m_giBlasDirty = false;
	GiBuffer m_giNodeBuf;
	GiBuffer m_giTriBuf;
	// Instance data changes every frame while earlier frames may still be in
	// flight — one buffer per in-flight frame, same ring convention as the
	// bones UBO ring.
	GiBuffer m_giInstanceBuf[3]; // k_maxFramesInFlight (static_assert at use site)
	int      m_giInstanceCount = 0;
	bool     m_giEnabled            = false;
	float    m_giIndirectIntensity  = 1.0f;
	float    m_giLightRadius        = 0.5f;
	int      m_giRaysPerProbe        = 128;
	int      m_giProbeBudgetPerFrame = 256;

	// ── GPU skeletal-mesh skinning ───────────────────────────────────────────
	// Each skeletal mesh uploaded to the GPU gets three vertex buffers:
	//   slot 0 — interleaved pos+norm+uv (32 bytes/vertex, matches scene.vert binding)
	//   slot 1 — bone IDs  (uvec4, 16 bytes/vertex)
	//   slot 2 — bone weights (vec4, 16 bytes/vertex)
	// plus one index buffer. Textures are unused in this initial implementation
	// (scene.frag drives material from the per-draw UBO, not a texture).
	struct GpuSkeletalMesh {
		VkBuffer       vb          = VK_NULL_HANDLE;  // slot 0: interleaved pos+norm+uv
		VkDeviceMemory vbMem       = VK_NULL_HANDLE;
		VkBuffer       boneIdVb    = VK_NULL_HANDLE;  // slot 1: uvec4 bone IDs
		VkDeviceMemory boneIdMem   = VK_NULL_HANDLE;
		VkBuffer       boneWgtVb   = VK_NULL_HANDLE;  // slot 2: vec4 bone weights
		VkDeviceMemory boneWgtMem  = VK_NULL_HANDLE;
		VkBuffer       ib          = VK_NULL_HANDLE;
		VkDeviceMemory ibMem       = VK_NULL_HANDLE;
		VkImageView    texView     = VK_NULL_HANDLE;  // base-color texture (set=2)
		VkImage        texImage    = VK_NULL_HANDLE;
		VkDeviceMemory texMem      = VK_NULL_HANDLE;
		VkDescriptorSet albedoSet  = VK_NULL_HANDLE;  // set=2, owned by m_albedoPool
		bool           hasTex      = false;
		int            indexCount  = 0;
	};

	// Two pipelines to match the two render passes (swapchain vs. HDR RGBA16F).
	VkPipeline            m_skinnedPipeline      = VK_NULL_HANDLE; // renderPass = m_renderPass
	VkPipeline            m_skinnedPipelineHDR   = VK_NULL_HANDLE; // renderPass = m_postFxSceneRP
	VkPipelineLayout      m_skinnedPipeLayout    = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_skinnedBonesDSL      = VK_NULL_HANDLE; // set=1: bones UBO
	VkDescriptorPool      m_skinnedDescPool      = VK_NULL_HANDLE;

	// Per-frame bones UBO ring (k_maxFramesInFlight slots).
	// Holds 128 mat4s = 8192 bytes per slot. NOTE: a single-slot design means only
	// one skinned mesh pose renders correctly per frame; later work can move to a
	// dynamic-offset UBO to support multiple skinned meshes per frame.
	VkBuffer              m_boneUBO[2]           = {};
	VkDeviceMemory        m_boneUBOMem[2]        = {};
	void*                 m_boneUBOPtr[2]        = {};
	VkDescriptorSet       m_boneDescSet[2]       = {}; // one set per frame, bound at set=1

	std::unordered_map<HE::UUID, GpuSkeletalMesh> m_skeletalMeshCache;

	void                    createSkinnedPipeline();
	const GpuSkeletalMesh*  resolveSkeletalMesh(const HE::UUID& id);
	void                    destroySkeletalMeshCache();

	// ── 2D UI canvas rendering ──────────────────────────────────────────────────
	// Draws solid-color quads (UIRenderObject) over the final image.
	// In the game path (no editor viewport) the draw is inline inside the
	// swapchain render pass.  In the editor viewport path a separate render pass
	// with LOAD_OP_LOAD is opened on m_viewportImage after FXAA completes.
	void createUIPipeline();
	void runUIPass(VkCommandBuffer cmd, int width, int height);

	// Pipeline that targets the swapchain render pass (m_renderPass, BGRA8/etc.).
	VkPipeline       m_uiPipeline          = VK_NULL_HANDLE;
	// Pipeline that targets the viewport RGBA8 image (m_uiViewportRP).
	VkPipeline       m_uiViewportPipeline  = VK_NULL_HANDLE;
	VkPipelineLayout m_uiPipeLayout        = VK_NULL_HANDLE;
	// Load-preserving render pass for compositing UI onto the viewport image.
	VkRenderPass     m_uiViewportRP        = VK_NULL_HANDLE;
	// Single-frame framebuffer wrapping m_viewportView for the UI viewport pass.
	VkFramebuffer    m_uiViewportFB        = VK_NULL_HANDLE;

	// ── UI font atlases (glyph quads, obj.type == 2) ────────────────────────
	// One R8 VkImage per UIFontCache atlas key (0 = the shared default font),
	// uploaded lazily from the CPU-baked bitmap the first time a glyph quad
	// references the key. Each atlas gets a descriptor set (set=0 binding=0,
	// immutable m_uiFontSampler) that runUIPass binds per glyph run; solid
	// quads keep whatever atlas is bound (the shader ignores it in mode 0).
	struct UIFontAtlas
	{
		VkImage         image  = VK_NULL_HANDLE;
		VkDeviceMemory  memory = VK_NULL_HANDLE;
		VkImageView     view   = VK_NULL_HANDLE;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	VkDescriptorSet uiFontAtlasSet(uint32_t key);   // creates/caches on demand
	void            destroyUIFontAtlases();
	VkDescriptorSetLayout m_uiAtlasDSLayout = VK_NULL_HANDLE;
	VkDescriptorPool      m_uiAtlasDescPool = VK_NULL_HANDLE;
	VkSampler             m_uiFontSampler   = VK_NULL_HANDLE;
	std::unordered_map<uint32_t, UIFontAtlas> m_uiFontAtlases;

	// ── GPU frame timing (VkQueryPool timestamps) ───────────────────────────
	// Two timestamps (frame begin/end) per ring slot. The ring is deeper than
	// k_maxFramesInFlight so the slot reused each frame finished at least one
	// fence-wait ago — its readback (availability-checked, no RESULT_WAIT)
	// never stalls. GetFrameGpuStats therefore reports 1–N frames late,
	// matching the OpenGL backend's async timer ring.
	void gpuTimerInit();                       // after device creation
	void gpuTimerBegin(VkCommandBuffer cmd);   // reap oldest slot + start stamp
	void gpuTimerEnd(VkCommandBuffer cmd);     // end stamp + advance the ring
	static constexpr uint32_t kGpuTimerRing = 4;
	VkQueryPool m_tsQueryPool = VK_NULL_HANDLE;
	bool        m_tsSupported = false;
	float       m_tsPeriodNs  = 0.0f;               // ns per timestamp tick
	uint64_t    m_tsValidMask = 0;                  // queue timestampValidBits mask
	bool        m_tsPending[kGpuTimerRing] = {};    // slot has unread results
	uint64_t    m_tsFrameIdx  = 0;
	FrameGpuStats m_lastGpuStats;                   // newest reaped GPU time
	// CPU-side per-frame counters (reset in Render, merged by GetFrameGpuStats).
	uint32_t m_statDraws = 0, m_statTris = 0, m_statVisible = 0, m_statTotal = 0;
};
