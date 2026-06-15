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
	void  SetMoonTexture(const void* rgba8Pixels, int width, int height) override;

	void  SetViewportSize(uint32_t width, uint32_t height) override;
	void* GetViewportTexture() override;
	bool  CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
	void  InvalidateMaterial(const HE::UUID& materialId) override;
	void  SetBloomSettings(const BloomSettings& settings) override;

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
	// Procedural skybox: fills the HDR target's background before the scene.
	void* m_skyPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_moonTexture = nullptr; // id<MTLTexture>, night-sky moon (or null)
	void  EncodeSky(void* renderEncoder, const glm::mat4& invViewProj, const glm::vec3& sunDir,
	                const glm::vec3& sunColor, float timeOfDay, float cloudCoverage, float time,
	                float auroraIntensity, const glm::vec3& nebulaColor, float nebulaIntensity,
	                const glm::vec3& auroraColor, float milkyWayIntensity);
	// (Re)creates the offscreen viewport textures at the requested size.
	void EnsureViewportTarget();
	void DestroyViewportTarget();
	// Returns the GPU mesh for the asset, uploading it on first use.
	// nullptr when the UUID is invalid or the asset is not loaded.
	const GpuMesh* ResolveMesh(const HE::UUID& assetId);

	// Resolves the base-color texture of an explicit MaterialComponent override,
	// uploading it on first sight and caching by material UUID. Returns true if
	// the material was found (outTex may still be nullptr = no texture); false
	// when the UUID is null or the material is not loaded yet. outTex is an
	// (unretained, autoreleased) id<MTLTexture> owned by the cache.
	bool ResolveMaterialTexture(const HE::UUID& materialId, void*& outTex);

	// Resolves a material override's PBR scalars (baseColor/metallic/roughness).
	// Returns true if the material is loaded; leaves outputs untouched otherwise.
	bool ResolveMaterialParams(const HE::UUID& materialId,
	                           glm::vec3& outBaseColor, float& outMetallic, float& outRoughness);

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

	// ── Shadow map (single directional light) ───────────────────────────────
	void* m_shadowDepthTex = nullptr;  // id<MTLTexture>, Depth32Float (retained)
	void* m_shadowPipeline = nullptr;  // id<MTLRenderPipelineState>, depth-only
	int   m_shadowSize     = 2048;
	void  EnsureShadowResources();
	void  EncodeShadowMap(void* cmdBuf); // renders the depth map before the scene

	// ── HDR scene color + tonemap (PostProcessPass) ─────────────────────────
	// The scene is rendered into an RGBA16Float target; EncodeTonemap then maps
	// it down to the LDR output (viewport texture or drawable). Sized to the
	// current scene output, recreated on resize.
	void* m_tonemapPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_hdrColor        = nullptr; // id<MTLTexture>, RGBA16Float (retained)
	void* m_hdrDepth        = nullptr; // id<MTLTexture>, Depth32Float (retained)
	int   m_hdrW            = 0;
	int   m_hdrH            = 0;
	void  EnsureHDRTarget(int width, int height);
	void  DestroyHDRTarget();
	void  EncodeTonemap(void* renderEncoder); // fullscreen tonemap of m_hdrColor

	// ── Bloom (bright-pass + separable Gaussian blur on the HDR target) ──────
	// Mirrors the GL backend: highlights above a soft-knee threshold are blurred
	// into a half-res target and added back during tonemap.
	void* m_bloomBrightPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_blurPipeline        = nullptr; // id<MTLRenderPipelineState>
	void* m_bloomColor[2]       = { nullptr, nullptr }; // id<MTLTexture> RGBA16F half-res
	void* m_bloomResult         = nullptr; // this frame's blurred bloom (or null = off)
	int   m_bloomW              = 0;
	int   m_bloomH              = 0;
	bool  m_bloomEnabled        = true;
	float m_bloomThreshold      = 1.0f;
	float m_bloomKnee           = 0.5f;
	float m_bloomStrength       = 0.6f;
	void  EnsureBloomTargets(int width, int height);
	void  DestroyBloomTargets();
	// Bright-pass + blur m_hdrColor into m_bloomColor[0]; returns its texture ptr.
	void* EncodeBloom(void* cmdBuf, int fullW, int fullH);

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID (id<MTLTexture>, retained; nullptr = resolved, no texture).
	// InvalidateMaterial retires the texture and drops the entry.
	std::unordered_map<HE::UUID, void*> m_materialTexCache;

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
