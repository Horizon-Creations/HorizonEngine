#pragma once
#include <Renderer/IRenderer.h>
#include "MetalShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
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

	void  SetViewportSize(uint32_t width, uint32_t height) override;
	void* GetViewportTexture() override;

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

	// GPU-side mesh, uploaded on first sight from ContentManager data.
	// All void* are retained Objective-C objects.
	struct GpuMesh
	{
		void* vertexBuf  = nullptr; // id<MTLBuffer>, interleaved pos3+normal3+uv2
		void* indexBuf   = nullptr; // id<MTLBuffer>, uint32
		int   indexCount = 0;
		void* texture    = nullptr; // id<MTLTexture>, base color (nullptr = none)
		HE::AABB localBounds;       // object-space bounds for culling
	};

	void CreateTarget(SDL_Window* sdlWin, WindowTarget& out);
	void DestroyTarget(WindowTarget& target);
	void EnsureDepthTexture(WindowTarget& target, int width, int height);
	void CreateScenePipeline();
	void CreateCubeMesh();
	void EncodeFrame(SDL_Window* sdlWin, WindowTarget& target, bool isPrimary);
	// Encodes the scene draw calls into the given encoder (any render pass
	// whose attachments match the scene pipeline formats).
	void EncodeScene(void* renderEncoder, int width, int height);
	// (Re)creates the offscreen viewport textures at the requested size.
	void EnsureViewportTarget();
	void DestroyViewportTarget();
	// Returns the GPU mesh for the asset, uploading it on first use.
	// nullptr when the UUID is invalid or the asset is not loaded.
	const GpuMesh* ResolveMesh(const HE::UUID& assetId);

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
	FrustumCuller   m_culler;
	RenderSorter    m_sorter;
	RenderGraph     m_renderGraph;   // pass pipeline (GeometryPass today)
	CommandBuffer   m_cmds;          // draw calls produced this frame
	std::vector<bool>     m_visible;       // per-frame culling results
	std::vector<uint32_t> m_sortedIndices; // per-frame draw order

	// Unlit pipeline + built-in cube (fallback for entities whose mesh asset
	// is missing or not loaded). All id<MTL…>, retained.
	void* m_scenePipeline   = nullptr; // id<MTLRenderPipelineState>
	void* m_sceneDepthState = nullptr; // id<MTLDepthStencilState> (test+write)
	void* m_noDepthState    = nullptr; // id<MTLDepthStencilState> (overlay)
	void* m_cubeVertexBuf   = nullptr; // id<MTLBuffer>
	void* m_cubeIndexBuf    = nullptr; // id<MTLBuffer>
	int   m_cubeIndexCount  = 0;
	void* m_dummyTexture    = nullptr; // id<MTLTexture>, 1×1 white — bound when a mesh has no texture
	void* m_linearSampler   = nullptr; // id<MTLSamplerState>

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;

	// ── Offscreen viewport (editor scene view) ──────────────────────────────
	uint32_t m_viewportReqW    = 0;  // requested by the UI, 0 = direct to window
	uint32_t m_viewportReqH    = 0;
	void*    m_viewportColor   = nullptr; // id<MTLTexture> (retained), doubles as ImTextureID
	void*    m_viewportDepth   = nullptr; // id<MTLTexture> (retained)

	// Textures replaced on viewport resize. The current frame's ImGui draw
	// list (and in-flight GPU work) may still reference the old texture, so
	// it is released a few frames later — never in the frame it was retired.
	struct RetiredTexture { void* texture; int framesLeft; };
	std::vector<RetiredTexture> m_retiredTextures;
	void RetireTexture(void* texture);     // hand a retained id<MTLTexture> over
	void AgeRetiredTextures();             // called once per frame
	void DrainRetiredTextures();           // immediate release (shutdown only)

	MetalShaderManager m_shaderManager;
};
