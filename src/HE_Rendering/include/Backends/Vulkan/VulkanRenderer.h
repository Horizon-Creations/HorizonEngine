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
	void           DrawScene(VkCommandBuffer cmd, uint32_t width, uint32_t height);
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
	VkDescriptorSetLayout m_sceneSetLayout      = VK_NULL_HANDLE;
	VkPipelineLayout      m_scenePipelineLayout = VK_NULL_HANDLE;
	VkPipeline            m_scenePipeline       = VK_NULL_HANDLE;
	VkDescriptorPool      m_descPool            = VK_NULL_HANDLE;
	struct FrameUBO
	{
		VkBuffer        buf    = VK_NULL_HANDLE;
		VkDeviceMemory  mem    = VK_NULL_HANDLE;
		void*           mapped = nullptr;
		VkDescriptorSet set    = VK_NULL_HANDLE;
	};
	FrameUBO m_frameUBO[2];

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
};
