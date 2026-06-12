#pragma once
#include <Renderer/IRenderer.h>
#include "VulkanShaderManager.h"
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
