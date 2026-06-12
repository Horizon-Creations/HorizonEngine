#pragma once
#include <Renderer/IRenderer.h>
#include "MetalShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <unordered_map>

struct SDL_Window;

// Passed as the overlay-callback context so ImGui (or any other overlay) can
// encode into the active render pass. All pointers are Objective-C objects
// (__bridge-casted) and only valid for the duration of the callback.
struct MetalOverlayContext
{
	void* commandBuffer;         // id<MTLCommandBuffer>
	void* renderEncoder;         // id<MTLRenderCommandEncoder>
	void* renderPassDescriptor;  // MTLRenderPassDescriptor*
};

// Implementation lives in MetalRenderer.mm (Objective-C++). This header stays
// plain C++ so RendererFactory and the editor can include it from .cpp files.
class MetalRenderer : public IRenderer
{
public:
	MetalRenderer();
	~MetalRenderer() override;

	void Initialize(HE::Window* window) override;
	void Shutdown()                      override;
	void Render()                        override;
	Capabilities GetCapabilities() const override;

	void SetVSync(bool enabled) override;

	void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
	void  DestroyImGuiTexture(void* handle) override;

	// Multi-window support
	void AttachWindow(HE::Window* window) override;
	void DetachWindow(HE::Window* window) override;
	void RenderWindow(HE::Window* window) override;

	// ── Accessors for the editor's ImGui Metal backend ─────────────────────
	void* GetDevice() const;              // id<MTLDevice>
	void* GetCommandQueue() const;        // id<MTLCommandQueue>
	// Render pass descriptor matching the swapchain format. Needed by
	// ImGui_ImplMetal_NewFrame() before the frame's real pass exists.
	void* GetFramePassDescriptor() const; // MTLRenderPassDescriptor*

private:
	struct WindowTarget
	{
		void* metalView    = nullptr; // SDL_MetalView
		void* metalLayer   = nullptr; // CAMetalLayer* (borrowed from the view)
		void* depthTexture = nullptr; // id<MTLTexture> (retained, resized with drawable)
	};

	void CreateTarget(SDL_Window* sdlWin, WindowTarget& out);
	void DestroyTarget(WindowTarget& target);
	void EnsureDepthTexture(WindowTarget& target, int width, int height);
	void CreateScenePipeline();
	void CreateCubeMesh();
	void EncodeFrame(SDL_Window* sdlWin, WindowTarget& target, bool isPrimary);

	SDL_Window* m_primarySdlWindow = nullptr;
	WindowTarget m_primaryTarget;
	std::unordered_map<SDL_Window*, WindowTarget> m_secondaryTargets;

	void* m_device       = nullptr; // id<MTLDevice>        (retained)
	void* m_commandQueue = nullptr; // id<MTLCommandQueue>  (retained)
	void* m_imguiPassDescriptor = nullptr; // MTLRenderPassDescriptor* (retained)
	bool  m_vsync = true;

	// ── Scene rendering ─────────────────────────────────────────────────────
	RenderExtractor m_extractor;
	RenderWorld     m_renderWorld;

	// Unlit pipeline + built-in cube (bootstrap mesh until the
	// RenderResourceManager uploads real assets). All id<MTL…>, retained.
	void* m_scenePipeline   = nullptr; // id<MTLRenderPipelineState>
	void* m_sceneDepthState = nullptr; // id<MTLDepthStencilState> (test+write)
	void* m_noDepthState    = nullptr; // id<MTLDepthStencilState> (overlay)
	void* m_cubeVertexBuf   = nullptr; // id<MTLBuffer>
	void* m_cubeIndexBuf    = nullptr; // id<MTLBuffer>
	int   m_cubeIndexCount  = 0;

	MetalShaderManager m_shaderManager;
};
