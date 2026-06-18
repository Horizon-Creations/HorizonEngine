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
	void  InvalidateMesh    (const HE::UUID& meshId)     override;
	void  SetBloomSettings(const BloomSettings& settings) override;
	void  SetSSAOSettings(const SSAOSettings& settings) override;
	void  SetDebugLines(const std::vector<DebugLine>& lines) override;

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

	// GPU-side skeletal mesh: separate bone-ID and bone-weight buffers on top of
	// the regular interleaved vertex buffer (pos+normal+uv, same stride as GpuMesh).
	struct GpuSkeletalMesh
	{
		void* vertexBuf  = nullptr; // id<MTLBuffer>, interleaved pos3+normal3+uv2
		void* boneIdBuf  = nullptr; // id<MTLBuffer>, uint4 per vertex (joint indices)
		void* boneWgtBuf = nullptr; // id<MTLBuffer>, float4 per vertex (blend weights)
		void* indexBuf   = nullptr; // id<MTLBuffer>, uint32 triangle indices
		int   indexCount = 0;
		void* texture    = nullptr; // id<MTLTexture>, base color (nullptr = none)
		HE::AABB localBounds;
	};

	void CreateTarget(SDL_Window* sdlWin, WindowTarget& out);
	void DestroyTarget(WindowTarget& target);
	void EnsureDepthTexture(WindowTarget& target, int width, int height);
	void CreateScenePipeline();
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
	                const glm::vec3& auroraColor, float milkyWayIntensity, const glm::vec3& wind);
	// (Re)creates the offscreen viewport textures at the requested size.
	void EnsureViewportTarget();
	void DestroyViewportTarget();
	// Returns the GPU mesh for the asset, uploading it on first use.
	// nullptr when the UUID is invalid or the asset is not loaded.
	const GpuMesh*         ResolveMesh        (const HE::UUID& assetId);
	const GpuSkeletalMesh* ResolveSkeletalMesh(const HE::UUID& assetId);

	// Skinned geometry pass: draws all skeletal mesh objects from the current
	// render world using the linear blend-skinning vertex shader.
	void EncodeSkinnedObjects(void* renderEncoder, const glm::mat4& viewProj,
	                          bool shadows, const void* sceneUniformsPtr);

	// Resolves the base-color texture of an explicit MaterialComponent override,
	// uploading it on first sight and caching by material UUID. Returns true if
	// the material was found (outTex may still be nullptr = no texture); false
	// when the UUID is null or the material is not loaded yet. outTex is an
	// (unretained, autoreleased) id<MTLTexture> owned by the cache.
	bool ResolveMaterialTexture(const HE::UUID& materialId, void*& outTex);

	// Resolves a material override's PBR scalars (baseColor/metallic/roughness/
	// opacity). Returns true if the material is loaded; leaves outputs untouched.
	bool ResolveMaterialParams(const HE::UUID& materialId,
	                           glm::vec3& outBaseColor, float& outMetallic, float& outRoughness,
	                           float& outOpacity);

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
	std::vector<uint8_t>  m_visible;       // per-frame culling results
	std::vector<uint32_t> m_sortedIndices; // per-frame draw order

	// Unlit pipeline. All id<MTL…>, retained.
	void* m_scenePipeline        = nullptr; // id<MTLRenderPipelineState>
	void* m_sceneBlendPipeline   = nullptr; // id<MTLRenderPipelineState> (alpha-blended transparency)
	void* m_skinnedPipeline      = nullptr; // id<MTLRenderPipelineState> (LBS skinning vertex shader)
	void* m_sceneDepthState = nullptr; // id<MTLDepthStencilState> (test+write)
	void* m_noDepthState    = nullptr; // id<MTLDepthStencilState> (overlay)
	void* m_skyDepthState   = nullptr; // id<MTLDepthStencilState> (sky: LessEqual, no write)
	void* m_dummyTexture    = nullptr; // id<MTLTexture>, 1×1 white — bound when shadow/AO/moon texture is absent
	void* m_linearSampler   = nullptr; // id<MTLSamplerState>
	void* m_noiseTexture    = nullptr; // id<MTLTexture>, 3D R16 value noise (sky)
	void* m_noiseSampler    = nullptr; // id<MTLSamplerState>, linear + repeat
	void* m_skyEnvCube      = nullptr; // id<MTLTexture>, baked skyColor IBL cubemap
	glm::vec3 m_skyEnvSunDir = glm::vec3(0.0f); // sun dir the cubemap was baked for
	bool  m_skyEnvValid     = false;
	void  UpdateSkyEnvCube(const glm::vec3& sunDir); // rebuild the IBL cubemap on sun move

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
	void  EncodeTonemap(void* renderEncoder); // fullscreen tonemap of m_hdrColor → LDR

	// ── FXAA (edge antialiasing) ─────────────────────────────────────────────
	// Tonemap writes to m_ldrColor; this pass reads it, runs FXAA on the gamma-space
	// luma and writes the antialiased result to the output (viewport or drawable).
	void* m_fxaaPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_ldrColor     = nullptr; // id<MTLTexture> (retained), tonemap out / FXAA in
	int   m_ldrW         = 0;
	int   m_ldrH         = 0;
	void  EnsureLdrTarget(int width, int height);
	void  DestroyLdrTarget();
	void  EncodeFxaa(void* renderEncoder, int width, int height); // FXAA of m_ldrColor

	// ── In-Game UI (2D canvas elements, drawn after FXAA) ───────────────────
	void* m_uiPipeline = nullptr; // id<MTLRenderPipelineState>
	void  EncodeUIPass(void* renderEncoder, int width, int height);

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

	// ── SSAO (screen-space ambient occlusion) ───────────────────────────────
	// Mirrors the GL backend: a view-space position pre-pass feeds a hemisphere-
	// kernel occlusion estimate, blurred and then sampled by the scene shader to
	// darken the image-based ambient in crevices. Encoded before the HDR scene
	// pass (it owns its own render encoders); skipped entirely when disabled.
	void* m_ssaoPosPipeline  = nullptr; // id<MTLRenderPipelineState> (writes view pos)
	void* m_ssaoPipeline     = nullptr; // fullscreen occlusion estimate
	void* m_ssaoBlurPipeline = nullptr; // fullscreen box blur
	void* m_ssaoPosTex       = nullptr; // id<MTLTexture> RGBA16F view position
	void* m_ssaoPosDepth     = nullptr; // id<MTLTexture> Depth32Float (nearest surface)
	void* m_ssaoTex          = nullptr; // id<MTLTexture> R8 raw occlusion
	void* m_ssaoBlurTex      = nullptr; // id<MTLTexture> R8 blurred (scene-shader read)
	void* m_ssaoNoiseTex     = nullptr; // id<MTLTexture> RGBA32F 4×4 rotation noise
	void* m_ssaoPointSampler = nullptr; // nearest + clamp (position lookups)
	void* m_ssaoNoiseSampler = nullptr; // nearest + repeat (tiled noise)
	void* m_ssaoResult       = nullptr; // this frame's AO texture, or null when off
	int   m_ssaoW = 0;
	int   m_ssaoH = 0;
	bool  m_ssaoEnabled   = true;
	float m_ssaoRadius    = 0.5f;
	float m_ssaoIntensity = 1.0f;
	void  EnsureSSAOTargets(int width, int height);
	void  DestroySSAOTargets();
	// Pre-pass + occlusion + blur for the current scene into m_ssaoBlurTex; sets
	// m_ssaoResult (or null). Runs its own extract/cull/sort (deterministic, so it
	// matches EncodeScene's draw set) and its own render encoders on cmdBuf.
	void  EncodeSSAO(void* cmdBuf, int width, int height);

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh>         m_meshCache;
	std::unordered_map<HE::UUID, GpuSkeletalMesh> m_skeletalMeshCache;
	std::vector<HE::UUID>                 m_pendingMeshInvalidations;

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

	// ── Debug line overlay ───────────────────────────────────────────────────
	void* m_debugLinePipeline = nullptr; // id<MTLRenderPipelineState>
	std::vector<DebugLine> m_debugLines;
	void  CreateDebugLinePipeline();
	void  EncodeDebugLines(void* renderEncoder, const glm::mat4& viewProj);

	MetalShaderManager m_shaderManager;
};
