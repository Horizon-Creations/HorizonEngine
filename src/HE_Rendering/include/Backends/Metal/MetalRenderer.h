#pragma once
#include <cstdint>
#include <Renderer/IRenderer.h>
#include <Renderer/GpuPassAccumulator.h>
#include "MetalShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/RenderConstants.h> // HE::kShadowMapResolution
#include <HorizonRendering/GiBvh.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <material/MaterialShaderLibrary.h> // shared cross-backend material shader layer
#include <unordered_map>
#include <atomic>
#include <memory>
#include <mutex>

// Shared state for Metal GPU timing. Held by shared_ptr so an in-flight
// command-buffer completion handler (which captures a copy) keeps it alive even
// if the renderer is destroyed before the GPU drains — no use-after-free, no
// forced GPU stall on shutdown. `last` is the most recently completed frame's
// GPU stats (whole-frame + per-pass); `gpuTicksToMs` is the CPU/GPU timestamp
// correlation factor, refreshed each captured frame on the main thread.
struct MetalGpuTimerShared
{
	std::mutex                 mutex;
	IRenderer::FrameGpuStats   last;             // newest published frame (any mode)
	std::atomic<double>        gpuTicksToMs{ 0.0 };
	// Detailed-capture (one command buffer per pass) bookkeeping. Its completion
	// handlers call accum.report(); a completed frame is mirrored into `last`.
	GpuPassAccumulator         accum;
};

// One GPU pass timed at its encoder's stage boundaries: timestamp sample at
// `base` (start of vertex) and base+1 (end of fragment), so end-start is the
// pass's GPU duration. Built per captured frame, copied into the completion
// handler. `name` is a static string literal.
struct GpuTimedPair  { const char* name; uint32_t base; };
// One draw-boundary timestamp sampled between draws inside a single render
// encoder (an intra-"Scene" element split). Duration of element i is
// sample[i] - sample[i-1]; the first point is the anchor with no interval.
struct GpuTimedPoint { const char* name; uint32_t slot; };

struct SDL_Window;
struct MaterialShaderVariant; // ContentManager/Assets.h — baked per-backend material shader
struct ParticleShaderVariant; // ContentManager/Assets.h — baked per-backend particle shader
// Per-frame hand-off between the deferred G-buffer pass and the lighting pass
// (defined in MetalRenderer.mm — it carries draw structs built on .mm-local
// types). Lives on EncodeFrame's stack, never across frames.
struct MetalDeferredFrame;

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
	FrameGpuStats GetFrameGpuStats() const override;

	void SetVSync(bool enabled) override;

	void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height) override;
	void  DestroyImGuiTexture(void* handle) override;
	void  SetMoonTexture(const void* rgba8Pixels, int width, int height) override;

	void  SetViewportSize(uint32_t width, uint32_t height) override;
	void* GetViewportTexture() override;
	bool  CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
	void  InvalidateMaterial(const HE::UUID& materialId) override;
	void  WarmupMaterials(const std::vector<HE::UUID>& materialIds) override;
	void* RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
	                            uint32_t size, float yaw, float pitch, float dist,
	                            int shape = 0, const HE::UUID& meshId = HE::UUID{}) override;
	void* RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
	                            const std::vector<glm::mat4>& boneMatrices,
	                            uint32_t width, uint32_t height,
	                            float yaw, float pitch, float dist,
	                            bool showSkeleton = true,
	                            glm::mat4* outViewProj = nullptr) override;
	void* RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
	                         uint32_t width, uint32_t height,
	                         const EditorCameraOverride& camera,
	                         const glm::vec3& origin = glm::vec3(0.0f),
	                         const WorldPreviewEnv& env = {},
	                         glm::mat4* outViewProj = nullptr) override;
	void* RenderParticlePreview(ContentManager& cm, const HE::UUID& meshId, const HE::UUID& materialId,
	                            const std::vector<ParticlePreviewInstance>& particles,
	                            uint32_t size, float yaw, float pitch, float dist) override;
	bool  RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind, const HE::UUID& assetId,
	                           uint32_t size, std::vector<uint8_t>& outRgba8) override;
	bool  RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
	                              const std::vector<ParticlePreviewInstance>& particles,
	                              uint32_t size, std::vector<uint8_t>& outRgba8) override;
	bool  RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
	                            uint32_t size, std::vector<uint8_t>& outRgba8) override;
	void  InvalidateMesh    (const HE::UUID& meshId)     override;
	void  InvalidateTexture (const HE::UUID& textureId)  override;
	void  SetBloomSettings(const BloomSettings& settings) override;
	void  SetSSAOSettings(const SSAOSettings& settings) override;
	void  SetAntiAliasingSettings(const AntiAliasingSettings& settings) override;
	void  SetGISettings(const GISettings& settings) override;
	void  SetSSRSettings(const SSRSettings& settings) override;
	void  SetGIReflectionSettings(const GIReflectionSettings& settings) override;
	void  SetShadowDebug(bool on) override { m_debugShadowCascades = on; }
	void  SetGpuParticleParams(const GpuParticleParams& p) override;
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
		// Bottom-level acceleration structure for ray-traced GI, built lazily the
		// first time this mesh is seen while GI is active (BuildBLAS). Never
		// rebuilt except via InvalidateMesh (sculpt/edit re-upload). id<MTLAccelerationStructure>.
		void* blas = nullptr;
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
	// whose attachments match the scene pipeline formats). When `deferred` is
	// non-null the opaque geometry was already rasterized into the G-buffer by
	// EncodeGBuffer this frame: instead of the opaque loop, a fullscreen
	// lighting resolve is drawn (plus the forward-routed opaque replay), and the
	// transparency list is taken from the hand-off. Everything else (skinned,
	// sky, transparency, particles) is the SAME code as forward.
	void EncodeScene(void* renderEncoder, int width, int height,
	                 MetalDeferredFrame* deferred = nullptr);

	// ── Deferred render path (G-buffer + lighting resolve) ───────────────────
	// docs/deferred-renderer-plan.md. Active when SetRenderPath(Deferred) and the
	// pipelines could be built (needs HE_HAVE_SHADERC for the resolve shader).
	void* m_gbColor0 = nullptr; // id<MTLTexture> RGBA8Unorm_sRGB — BaseColor + Metallic
	void* m_gbColor1 = nullptr; // id<MTLTexture> RGBA16Float — oct Normal + Roughness + Specular
	void* m_gbColor2 = nullptr; // id<MTLTexture> RGBA16Float — Emissive (HDR) + Material-AO
	void* m_gbDepth  = nullptr; // id<MTLTexture> Depth32Float — two-pass mode only: sampled by
	                            // the resolve, blitted into m_hdrDepth for the lighting pass
	// NDC depth as a COLOUR target (R32Float, attachment 3): written by every
	// G-buffer fragment so the tile resolve can framebuffer-fetch it — Metal
	// cannot fetch the depth buffer itself. In two-pass mode it is attached
	// (pipeline/pass formats must match) but unused.
	void* m_gbDepthLin = nullptr;
	int   m_gbW = 0, m_gbH = 0;
	void* m_gbufferPipeline        = nullptr; // id<MTLRenderPipelineState> — built-in PBR G-buffer
	void* m_deferredResolvePipeline = nullptr; // id<MTLRenderPipelineState> — fullscreen heLitP resolve
	// Tile-memory single pass (plan P6, Apple Silicon): the G-buffer attachments
	// are MTLStorageModeMemoryless and the resolve runs INSIDE the G-buffer pass
	// via framebuffer fetch, writing the HDR colour to attachment 4 — the
	// G-buffer never leaves tile storage (bandwidth ≈ 0). Decided once at
	// Initialize (Apple-GPU family; HE_DEFERRED_TILE=0/1 overrides). SSAO keeps
	// its classic geometry pre-pass in this mode: the resolve consumes AO inside
	// pass 1, so there is no stored depth to reconstruct it from (P5 stays for
	// the two-pass fallback).
	bool  m_deferredTileMode            = false;
	void* m_deferredResolveTilePipeline = nullptr; // id<MTLRenderPipelineState>
	// Fullscreen tile resolve, encoded into the OPEN G-buffer pass encoder.
	void  EncodeDeferredResolveTile(void* renderEncoder, int width, int height);
	bool  m_deferredPipelinesTried  = false;   // build attempted once; failure logs + falls back forward

	// ── Screen-space reflections (docs/ssr-plan.md §4.5, deferred-tile v1) ───
	// After the tile resolve (which SKIPS its specular-IBL term via
	// heLight.ssr.w) two extra passes run: a half-res world-space trace against
	// the stored G-buffer depth sampling the CURRENT frame's resolved HDR (no
	// lag, no history), and an additive fullscreen composite that mixes the SSR
	// hit against the sky cubemap with heLitP's exact weather/AO/fog factors.
	// SSR forces the tile G-buffer to STORED (non-memoryless) attachments —
	// screen-space reflections need the data after the pass; that trade is
	// inherent to the technique.
	bool  m_ssrEnabled      = false;
	float m_ssrIntensity    = 1.0f;
	float m_ssrMaxRoughness = 0.6f;
	float m_ssrMaxDistance  = 30.0f;
	float m_ssrThickness    = 0.5f;
	int   m_ssrQuality      = 1;      // 0 = 16 steps, 1 = 32, 2 = 64
	bool  m_ssrFrameActive  = false;  // this frame runs SSR (tile deferred + enabled + pipelines)
	void* m_ssrTracePipeline     = nullptr; // id<MTLRenderPipelineState>
	void* m_ssrCompositePipeline = nullptr; // id<MTLRenderPipelineState> (ONE/ONE additive)
	void* m_ssrBlurPipeline      = nullptr; // id<MTLRenderPipelineState> separable 5-tap (plan P4)
	bool  m_ssrPipelinesTried    = false;
	void* m_ssrReflTex = nullptr; // id<MTLTexture> RGBA16F half-res: rgb radiance, a confidence
	void* m_ssrPingTex = nullptr; // id<MTLTexture> same format, ping target of the separable blur
	void* m_ssrRoughTex = nullptr; // id<MTLTexture> wide second blur (High tier glossy lerp)
	// ── FORWARD reflections (ssr-plan Option A / P3-Forward) ─────────────────
	// The SSAO prepass runs as MRT (view-pos + oct-normal/rough + NDC depth)
	// when the forward path wants reflections; the SSR trace then reads the
	// prepass pair exactly like the deferred G-buffer, sampling LAST frame's
	// HDR copy (1 frame of content lag, camera-motion reprojected). Results
	// are consumed by heLitP/fragmentMain via the heSSR/heGIRefl samplers
	// (Metal slots 9/10) instead of a dedicated composite pass.
	void* m_reflPosPipeline = nullptr; // id<MTLRenderPipelineState> MRT prepass
	void* m_reflNormTex  = nullptr;    // id<MTLTexture> RGBA16F: oct normal, rough 0
	void* m_reflDepthTex = nullptr;    // id<MTLTexture> R32F: NDC depth (gbufferMain convention)
	void* m_ssrColorHist = nullptr;    // id<MTLTexture> full-res RGBA16F, last frame's HDR
	int   m_ssrColorHistW = 0, m_ssrColorHistH = 0;
	bool  m_ssrColorHistValid = false;
	void* m_fwdReflSsrTex = nullptr;   // this frame's forward SSR result (null → dummy + gate 0)
	void* m_fwdReflGiTex  = nullptr;   // this frame's forward GI-refl result
	bool  m_fwdReflPrepassWanted = false; // EncodeSSAO renders the MRT prepass
	bool  m_fwdReflPrepassOnly   = false; // …and skips occlusion+blur (SSAO off/replaced)
	void  EncodeForwardSSR(void* cmdBuf, int width, int height);

	// Temporal accumulation (quality High): the trace renders MRT into
	// histRad/histPos[cur] (blended radiance + receiver world pos), sampling
	// [prev] — the blur chain then reads histRad[cur]. Same scheme as the
	// GI-reflection kernel, only as a fragment pass instead of compute.
	void* m_ssrHistRad[2] = { nullptr, nullptr }; // id<MTLTexture> RGBA16F
	void* m_ssrHistPos[2] = { nullptr, nullptr }; // id<MTLTexture> RGBA16F (world pos + valid)
	int   m_ssrHistIdx    = 0;
	bool  m_ssrHistValid  = false;    // fresh targets → first frame skips the blend
	float m_ssrFrameSeed  = 0.0f;
	glm::mat4 m_ssrPrevViewProj{1.0f};
	int   m_ssrReflW = 0, m_ssrReflH = 0;
	bool  m_gbStored = false;     // current G-buffer allocation: stored vs memoryless
	bool  EnsureSSRPipelines();
	void  EnsureSSRTarget(int width, int height);
	void  DestroySSRTarget();
	// Trace + composite, own passes on cmdBuf (after the tile G-buffer pass).
	// Despite the name this is the shared ENV-SPECULAR composite: it also mixes
	// the ray-traced GI reflection result (m_giReflTex) under the SSR hit, and
	// runs whenever EITHER source is active this frame (the inactive one binds
	// a dummy with its mix weight forced to 0).
	void  EncodeSSRPasses(void* cmdBuf, int width, int height);

	// ── Ray-traced GI reflections (docs/gi-reflections-plan.md, P0-P5) ───────
	// One reflection ray per half-res pixel from the stored G-buffer against
	// the GI acceleration structure (TLAS on HW RT, the CPU-built GiBvh buffers
	// on the SW path — EncodeGIAccelBuild runs earlier in the frame for both);
	// hits are shaded flat-albedo × (sun visibility + DDGI field irradiance),
	// so reflections agree with the diffuse GI about lighting. Output RGBA16F:
	// rgb = radiance, a = confidence (roughness/distance fade; 0 = miss → the
	// composite keeps the sky cubemap). Quality tiers mirror SSR's: 0 = raw
	// mirror trace; 1 = + confidence-weighted separable blur; 2 = + roughness-
	// jittered cone rays with camera-reprojected temporal accumulation (history
	// carries the receiver world pos for disocclusion reject — reflected-object
	// parallax is NOT reprojected, mild ghosting under motion accepted) and a
	// wide second blur for the composite's glossy roughness lerp. Hit normals:
	// HW path fetches true interpolated vertex normals through a tier-2
	// argument buffer of mesh pointers (macOS 13+, m_giMeshPtrBuf); SW path
	// uses the hit triangle's geometric normal; both fall back to -rayDir.
	// Deferred tile mode only (stored G-buffer + composite onto resolved HDR).
	bool  m_giReflEnabled      = false;
	float m_giReflIntensity    = 1.0f;
	float m_giReflMaxRoughness = 0.6f;
	float m_giReflMaxDistance  = 200.0f;
	// How much blur even a mirror gets, purely to hide the trace resolution
	// (0 at full res). Set with the target size in EncodeGIReflections and read
	// by both composites — see kSSRRoughMixFS.
	float m_giReflBlurFloor    = 0.0f;
	bool  m_giReflBlurEnabled  = true;  // IRenderer::GIReflectionSettings::blur
	// How far, in SCREEN pixels, the widest allowed lobe (roughness =
	// GIReflMaxRoughness) scatters when ONE ray samples it. Divided by the tier's
	// ray count at use: the filter only stands in for the part of the lobe the
	// rays missed. Absent entirely, every higher tier was noisier than the one
	// below; held constant across tiers, every higher tier was blurrier.
	static constexpr float kGIReflLobeScreenPx = 24.0f;
	// Forward-path glossy mix (quality High): bakes the narrow/wide roughness
	// lerp the DEFERRED composite does per pixel into the half-res texture the
	// forward scene shader samples — it only gets one. Optional; null just means
	// the forward path keeps the narrow blur.
	void* m_ssrRoughMixPipeline = nullptr; // id<MTLRenderPipelineState> (retained)
	void* m_giReflGlossyTex     = nullptr; // borrowed: this frame's lerped result, or null
	int   m_giReflBounces      = 1;   // 1-4 mirror bounces (extra.w in the kernels)
	int   m_giReflQuality      = 1;      // 0 raw / 1 blur / 2 glossy+temporal
	bool  m_giReflFrameActive  = false;  // this frame traces GI reflections (tile deferred + accel built)
	void* m_giReflPipeline     = nullptr; // id<MTLComputePipelineState> (HW or SW kernel, per m_giHwRt)
	bool  m_giReflPipelineTried = false;
	void* m_giReflTex     = nullptr; // id<MTLTexture> RGBA16F half-res display: rgb radiance, a confidence
	void* m_giReflPingTex = nullptr; // id<MTLTexture> blur ping target
	void* m_giReflRoughTex = nullptr; // id<MTLTexture> wide second blur (quality 2 glossy lerp)
	// Temporal history ping-pong (quality 2): radiance+confidence, and the
	// receiver world position the value was written for (disocclusion reject —
	// same scheme as m_giShadowHistory).
	void* m_giReflHistRad[2] = { nullptr, nullptr };
	void* m_giReflHistPos[2] = { nullptr, nullptr };
	int   m_giReflHistIdx    = 0;
	bool  m_giReflHistValid  = false;
	glm::mat4 m_giReflPrevViewProj = glm::mat4(1.0f);
	float m_giReflFrameSeed  = 0.0f;
	int   m_giReflW = 0, m_giReflH = 0;
	// P4 (HW only): per-unique-BLAS vertex/index buffer GPU addresses (tier-2
	// argument buffer, macOS 13+) + per-TLAS-instance BLAS index, rebuilt with
	// the TLAS each frame; m_giMeshResources lists the raw MTLBuffers for the
	// mandatory useResource: declarations (argument-buffer indirection is not
	// residency-tracked). Null when unavailable → kernel falls back to -rayDir.
	void* m_giMeshPtrBuf      = nullptr; // id<MTLBuffer> (retained)
	void* m_giInstanceMeshBuf = nullptr; // id<MTLBuffer> (retained), uint per instance
	std::vector<void*> m_giMeshResources; // __bridge id<MTLBuffer>, not retained
	bool  EnsureGIReflPipeline();
	void  EnsureGIReflTarget(int width, int height);
	void  DestroyGIReflTarget();
	// Compute trace (+ temporal) into m_giReflTex + history, then the blur
	// chain, own encoders on cmdBuf (after the tile G-buffer pass, before the
	// composite inside EncodeSSRPasses).
	void  EncodeGIReflections(void* cmdBuf, int width, int height);

	// ── Deferred decals (P7 follow-up, tile mode v1) ─────────────────────────
	// Unit-cube projectors (DecalComponent) rasterized INSIDE the G-buffer pass
	// right after the geometry: the fragment framebuffer-fetches the NDC depth
	// (attachment 3), clips against the box and alpha-blends into GB0.rgb.
	// Apple-GPU only (fetch) — the two-pass fallback and GL ignore decals in v1.
	void* m_decalPipeline      = nullptr; // id<MTLRenderPipelineState>
	bool  m_decalPipelineTried = false;
	bool  EnsureDecalPipeline();
	void  EncodeDecals(void* renderEncoder, int width, int height);

	// ── Clustered lighting (plan P7) ─────────────────────────────────────────
	// In the deferred resolve ALL point/spot lights come from per-cluster light
	// lists (CPU-built each frame, scattered by projected bounds) instead of the
	// 8-light window — the light limit falls. The heLight window then carries
	// directional lights only. Default on for deferred; HE_DEFERRED_CLUSTER=0
	// forces the 8-light resolve (A/B guard). GL keeps the 8-light resolve
	// (no SSBOs in GL 4.1).
	static constexpr int   kClusterGridX = 16;
	static constexpr int   kClusterGridY = 9;
	static constexpr int   kClusterGridZ = 24;
	static constexpr float kClusterNear  = 0.1f;
	static constexpr float kClusterFar   = 1000.0f;
	static constexpr int   kMaxClusteredLights = 256;
	bool  m_deferredClustered = true;
	// Build this frame's cluster data from m_renderWorld, bind the three SSBO
	// buffers on the encoder (fragment buffers 4/5/6), fill ru's cluster fields
	// and rewrite matLight's window to DIRECTIONAL lights only.
	void  EncodeClusterData(void* renderEncoder,
	                        HE::MaterialShaderLibrary::Lighting& matLight,
	                        HE::MaterialShaderLibrary::ResolveUniforms& ru);
	int   m_gbufferDebugView        = 0;       // HE_DUMP_GBUFFER (1..4), read once at Initialize
	bool  m_deferredFrameActive     = false;   // this frame renders deferred (set before SSAO — P5 reads it)
	void  EnsureGBufferTargets(int width, int height);
	void  DestroyGBufferTargets();
	bool  EnsureDeferredPipelines();  // true when both PSOs exist
	// Rasterize the opaque scene into the G-buffer (extract/cull/sort of its own,
	// deterministic — same set EncodeScene sees). Fills `out` with the draws that
	// must run forward in the lighting pass (translucent + custom materials
	// without a G-buffer variant).
	void  EncodeGBuffer(void* renderEncoder, int width, int height, MetalDeferredFrame& out);
	// The heLitP lighting-ABI fill for custom-material draws AND the deferred
	// resolve — one implementation, so the two can never drift (extracted from
	// EncodeScene when the tile resolve needed it outside that function).
	void  FillMaterialLighting(HE::MaterialShaderLibrary::Lighting& matLight,
	                           int width, int height, bool giActive, bool ssaoActive,
	                           bool shadows, float skyClock);
	// Procedural skybox: fills the HDR target's background before the scene.
	void* m_skyPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_moonTexture = nullptr; // id<MTLTexture>, night-sky moon (or null)
	// Everything else the sky shader needs comes from `env` via the shared
	// HE::BuildSkyFrameParams; the per-frame camera/clock values are passed in.
	// `env`/`camPos` are parameters rather than GetEnvironment()/m_renderWorld so
	// the world PREVIEW can draw the very same sky under its own time of day —
	// there is one sky in this engine, and a cheaper stand-in for the preview was
	// tried and looked like one. `lowResClouds` false forces the inline raymarch
	// (the preview runs no quarter-res pre-pass, so there is no buffer to composite).
	void  EncodeSky(void* renderEncoder, const glm::mat4& invViewProj, const glm::vec3& sunDir,
	                float time, const IRenderer::EnvironmentSettings& env,
	                const glm::vec3& camPos, bool lowResClouds);
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
	// Node-graph project texture (Texture Sample nodes), cached by UUID/path key.
	void* ResolveGraphTexture(const HE::UUID& texId, const std::string& path);

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

	// Whole-frame GPU time (ms) fallback, used when per-pass counter sampling is
	// unavailable. Written from the command-buffer completion handler (background
	// thread), read by GetFrameGpuStats(). -1 = not measured. Vsync-immune.
	std::shared_ptr<std::atomic<double>> m_gpuFrameMs =
		std::make_shared<std::atomic<double>>(-1.0);

	// ── Per-pass GPU timing (MTLCounterSampleBuffer, stage-boundary) ────────
	// Lazily probed once; null if the device/driver can't sample counters, in
	// which case GetFrameGpuStats falls back to whole-frame timing above. Only
	// active while a profiler capture is recording (zero overhead otherwise).
	void  EnsureGpuTimer();                       // probe support, build shared state
	bool  m_gpuTimerChecked      = false;
	void* m_timestampCounterSet  = nullptr;       // id<MTLCounterSet> (CFBridgingRetain'd) or null
	std::shared_ptr<MetalGpuTimerShared> m_gpuTimer;   // always created; holds whole-frame + detailed accum
	bool  m_counterSamplingOk = false;            // stage-boundary MTLCounterSampleBuffer available
	bool  m_drawBoundary = false;                 // MTLCounterSamplingPointAtDrawBoundary supported
	uint64_t m_detailFrameIdx = 0;                // monotonic frame index for detailed-capture accum
	uint64_t m_prevCpuTs = 0;                     // CPU/GPU timestamp correlation (main thread)
	uint64_t m_prevGpuTs = 0;

	// ── Profiler render counters (current frame, main thread) ───────────────
	// Filled while encoding the scene; returned (merged with the 1-2-frame-late
	// GPU times) by GetFrameGpuStats. Reset at the top of each primary EncodeFrame.
	struct FrameCounters { uint32_t draws = 0, tris = 0, visible = 0, total = 0; };
	FrameCounters m_counters;

	// ── Per-frame GPU timing context ────────────────────────────────────────
	// Valid only inside one EncodeFrame; the major encoders read it to attach
	// stage-boundary timers (pairs) or place draw-boundary samples (points). The
	// completion handler captures copies, so it is rebuilt each captured frame.
	// `sampleBuf` is borrowed — EncodeFrame owns the strong ref until commit.
	struct GpuFrameTiming
	{
		void*    sampleBuf = nullptr; // id<MTLCounterSampleBuffer> (borrowed)
		uint32_t next      = 0;       // next free sample slot (high-water mark)
		bool     stage     = false;   // per-encoder timing active this frame
		bool     draw      = false;   // intra-encoder sampling active this frame
		std::vector<GpuTimedPair>  pairs;
		std::vector<GpuTimedPoint> points;
		void reset() { sampleBuf = nullptr; next = 0; stage = draw = false; pairs.clear(); points.clear(); }
	};
	GpuFrameTiming m_ft;
	uint32_t ftPair(const char* name);   // reserve a start/end sample pair, returns base slot
	uint32_t ftPoint(const char* name);  // reserve one draw-boundary sample, returns slot
	// Stage-boundary attach helpers (no-op unless stage timing is active):
	void     ftAttachPass(void* passDesc, const char* name); // single-encoder pass (start+end)
	uint32_t ftBeginMulti(const char* name);                 // multi-encoder pass: reserve base
	void     ftAttachStart(void* passDesc, uint32_t base);   // first encoder of a multi-encoder pass
	void     ftAttachEnd  (void* passDesc, uint32_t base);   // last encoder of a multi-encoder pass
	// Draw-boundary sample inside one render encoder (intra-Scene element split):
	void     SamplePoint(void* encoder, const char* name);

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

	// ── Shadow map (cascaded; directional light) ────────────────────────────
	void* m_shadowDepthTex = nullptr;  // id<MTLTexture>, Depth32Float 2D ARRAY (one layer/cascade)
	void* m_shadowPipeline = nullptr;  // id<MTLRenderPipelineState>, depth-only
	int   m_shadowSize     = HE::kShadowMapResolution;
	// Local (point/spot) shadow atlas: 2D depth ARRAY, one layer per spot view /
	// point cube face (16 layers total, see ShadowData::kMaxLocalShadowLayers).
	void* m_localShadowTex  = nullptr; // id<MTLTexture>, Depth32Float 2D ARRAY
	int   m_localShadowSize = 1024;
	bool  m_debugShadowCascades = false; // tint fragments by cascade index (debug)
	void  EnsureShadowResources();
	void  EncodeShadowMap(void* cmdBuf, float aspect); // CSM depth maps; aspect MUST match the scene extract

	// ── HDR scene color + tonemap (PostProcessPass) ─────────────────────────
	// The scene is rendered into an RGBA16Float target; EncodeTonemap then maps
	// it down to the LDR output (viewport texture or drawable). Sized to the
	// current scene output, recreated on resize.
	void* m_tonemapPipeline = nullptr; // id<MTLRenderPipelineState>

	// Per-material pipeline cache (M1): a MaterialAsset with a customShaderFragGlsl gets
	// its fragment cross-compiled (he::shaderc) and spliced onto the standard drop-in
	// vertex (verts@0, Uniforms@1), then cached here keyed by a hash of the source. The
	// opaque loop selects a material's pipeline per-draw; materials without a custom
	// shader use the built-in PBR m_scenePipeline. Value may be null (build failed →
	// cached so we don't retry every frame).
	std::unordered_map<uint64_t, void*> m_materialPipelineCache; // hash → id<MTLRenderPipelineState>
	HE::MaterialShaderLibrary           m_matShaderLib;          // shared GLSL→MSL cross-compile + cache
	// On-disk pipeline cache (MTLBinaryArchive): persists compiled material pipeline
	// functions across launches so the Metal compiler doesn't re-run every start.
	// Lazily loaded from disk; best-effort (any failure falls back to normal build).
	void*       m_matBinaryArchive = nullptr;  // id<MTLBinaryArchive> (retained), null = unavailable
	bool        m_matArchiveTried  = false;    // ensured once
	std::string m_matArchivePath;              // serialize target on disk
	// Returns the material binary archive (loading/creating it on first use), or nil.
	void* EnsureMaterialArchive();
	// Material-preview target (RenderMaterialPreview): a small RGBA16F color texture
	// (matches the material PSO's HDR format) + depth, plus a lazily-built unit sphere,
	// independent of the main viewport.
	void* m_previewColorTex = nullptr; // id<MTLTexture> (retained) — shown by ImGui::Image
	void* m_previewDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_previewSize     = 0;
	void* m_previewVB = nullptr, *m_previewIB = nullptr; // id<MTLBuffer> (retained)
	int   m_previewIdxCount = 0;
	int   m_previewShape    = -1; // which primitive the VB/IB currently hold (-1 = none)

	// Skeletal-mesh-preview target (RenderSkeletalPreview) — own dedicated RGBA16F
	// color + depth texture (same format as the material preview, so the bone-line
	// overlay can reuse m_debugLinePipeline/m_sceneDepthState verbatim). Deliberately
	// a MINIMAL self-contained skinning pipeline (fixed sun+ambient, no shadow/SSAO/
	// fog/sky-env) rather than the scene-integrated skinnedVertex+fragmentMain pair.
	void* m_skelPreviewColorTex = nullptr; // id<MTLTexture> (retained)
	void* m_skelPreviewDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_skelPreviewW        = 0;
	int   m_skelPreviewH        = 0;
	void* m_skelPreviewPipeline = nullptr; // id<MTLRenderPipelineState> (retained)
	// Build that pipeline once — the skeletal preview and the world preview both
	// need it, and a second copy of the shader is how the two would drift apart.
	bool EnsureSkelPreviewPipeline();

	// World-preview target (RenderWorldPreview) — same RGBA16F color + depth as
	// its siblings, so the debug-line pipeline can draw the ground/grid into it
	// verbatim. ONE target, because only one asset tab is ever active. Unlike the
	// per-asset previews this one clears to an opaque gray and draws a ground
	// plane + grid, so it reads as a scene view rather than a cut-out asset.
	// Two colour targets, mirroring the scene: the pass renders HDR, the tonemap
	// resolves into the LDR one ImGui shows. Handing ImGui raw HDR is what made
	// the first sky-lit preview a white mesh under a blown-out sky.
	void* m_worldPreviewHdrTex   = nullptr; // id<MTLTexture> (retained), kSceneColorFormat
	void* m_worldPreviewColorTex = nullptr; // id<MTLTexture> (retained), kSwapchainFormat
	void* m_worldPreviewDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_worldPreviewW        = 0;
	int   m_worldPreviewH        = 0;

	// Particle-preview target (RenderParticlePreview) — own dedicated RGBA16F
	// color + depth texture; camera-facing billboards via vertex_id (no vertex
	// buffer needed for the corners, matching the debug-line/skinned-preview
	// "raw buffer indexed by id" convention already used in this file).
	void* m_particlePreviewColorTex = nullptr; // id<MTLTexture> (retained)
	void* m_particlePreviewDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_particlePreviewSize     = 0;
	void* m_particlePreviewPipeline = nullptr; // id<MTLRenderPipelineState> (retained)

	// ── Content-Browser thumbnails (RenderAssetThumbnail) ────────────────────
	// Own target, deliberately NOT any of the preview targets above: a thumbnail
	// is rendered while the Material Editor may be showing its live preview, and
	// sharing m_previewColorTex would replace that preview's contents with
	// whatever asset the grid happened to ask for. Read back to RGBA8 and cached
	// by the editor, so this holds one thumbnail at a time.
	void* m_thumbColorTex = nullptr; // id<MTLTexture> (retained), RGBA16F like the previews
	void* m_thumbDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_thumbSize     = 0;
	// A SECOND target for widget tiles, in the SWAPCHAIN format: every pipeline
	// the UI pass uses is built against kSwapchainFormat and Metal requires the
	// pipeline's colour format to match the pass's attachment, so UI drawn into
	// the RGBA16F target above renders nothing at all.
	void* m_thumbUIColorTex = nullptr; // id<MTLTexture> (retained), BGRA8
	void* m_thumbUIDepthTex = nullptr; // id<MTLTexture> (retained)
	int   m_thumbUISize     = 0;
	// Unskinned mesh pipeline shared by the mesh thumbnails and by materials that
	// have no node graph (built-in PBR): the counterpart of m_skelPreviewPipeline
	// with the bone buffers removed and a metallic/roughness-driven highlight.
	void* m_meshPreviewPipeline = nullptr; // id<MTLRenderPipelineState> (retained)
	// Build that pipeline once; shared with the world preview for the same reason.
	bool EnsureMeshPreviewPipeline();
	// Lazily (re)create the thumbnail target at S×S; false if it could not be made.
	bool EnsureThumbnailTarget(int S);
	// Blit the thumbnail target to staging on `commandBuffer`, commit, wait and
	// decode the RGBA16F into top-down RGBA8.
	bool CommitAndReadThumbnail(void* commandBuffer, int S, std::vector<uint8_t>& out);
	// Encode one material-graph preview primitive into an OPEN encoder (the caller
	// owns the render pass + its clear). False when the material has no node-graph
	// pipeline — the thumbnail path then falls back to m_meshPreviewPipeline.
	// `meshId` (optional) draws that static mesh instead of the primitive, framed
	// on its own bounds; an unresolvable mesh falls back to `shape`.
	bool EncodeMaterialPreview(void* renderEncoder, const HE::UUID& materialId,
	                           float yaw, float pitch, float dist, int shape,
	                           const HE::UUID& meshId = HE::UUID{});
	// Build the billboard pipeline once; false on failure.
	bool EnsureParticlePreviewPipeline();
	// Encode the particle cloud into an OPEN encoder — shared by the interactive
	// preview and the thumbnail so the two can never drift.
	void EncodeParticleBillboards(void* renderEncoder, const HE::UUID& materialId,
	                              const std::vector<ParticlePreviewInstance>& particles,
	                              const glm::mat4& viewProj, const glm::vec3& camRight,
	                              const glm::vec3& camUp);
	// Same contract for the mesh pipeline; `center`/`extent` frame the orbit camera.
	void EncodeMeshPreview(void* renderEncoder, void* vertexBuf, void* indexBuf, int indexCount,
	                       void* texture, const glm::vec3& center, float extent,
	                       const glm::vec3& baseColor, float metallic, float roughness,
	                       float yaw, float pitch, float dist);

	// GPU-instanced ParticleGraph particle rendering (the real scene draw path, see
	// RenderWorld::particleBatches) — one compiled pipeline per unique color/alpha-
	// over-life config, hash-keyed cache mirroring m_materialPipelineCache. Each
	// batch's instance MTLBuffer is allocated fresh per draw (newBufferWithBytes,
	// same convention as RenderParticlePreview) rather than reusing one persistent
	// buffer across frames — Metal gives no implicit CPU/GPU sync on a shared-mode
	// buffer, so overwriting one in flight would race the previous frame's draw; a
	// few emitters' worth of small per-frame allocations is a non-issue. No
	// HE_HAVE_SHADERC dependency: the source is hand-templated (HorizonRendering::
	// ParticleShaderTemplates), never cross-compiled. Drawn in the transparency
	// pass, alpha-blended.
	std::unordered_map<uint64_t, void*> m_particlePipelineCache; // hash → id<MTLRenderPipelineState>
	// precompiled != null → build directly from a baked MSL source (CHUNK_PPSD),
	// no template splice. Returns null on compile/link failure (also cached).
	void* GetOrBuildParticlePipeline(uint64_t key, const HE::ParticleEmitterConfig& config,
	                                 const ParticleShaderVariant* precompiled = nullptr);
	void  DrawParticleGraphBatches(void* renderEncoder, const glm::mat4& viewProj, const glm::mat4& view);

	void* m_hdrColor        = nullptr; // id<MTLTexture>, RGBA16Float (retained)
	void* m_hdrDepth        = nullptr; // id<MTLTexture>, Depth32Float (retained)
	int   m_hdrW            = 0;
	int   m_hdrH            = 0;
	void  EnsureHDRTarget(int width, int height);
	void  DestroyHDRTarget();
	// Fullscreen tonemap of an HDR target → LDR. `sourceHdr` null = the scene's
	// m_hdrColor; the world preview passes its own and turns bloom/flare off.
	void  EncodeTonemap(void* renderEncoder, void* sourceHdr = nullptr, bool withBloom = true);
#if defined(HE_HAVE_SHADERC)
	// Build (or fetch cached) a pipeline for a material's custom fragment GLSL, spliced
	// onto the standard drop-in vertex. Returns null on compile/link failure (also cached).
	// precompiled != null → build directly from baked MSL (no runtime cross-compile).
	// vertBody = WPO vertex body ("" → standard vertex); blend = alpha-blended variant
	// for the transparency pass (cached separately — same key space, blend bit mixed in).
	// gbuffer = MRT G-buffer variant for the deferred path: fragGlsl is then the
	// material's customShaderGBufGlsl and the PSO targets the three G-buffer
	// formats (never blended). Own key space via a salt, like blend.
	void* GetOrBuildMaterialPipeline(uint64_t key, const std::string& fragGlsl,
	                                 const std::string& vertBody = {},
	                                 const MaterialShaderVariant* precompiled = nullptr,
	                                 bool blend = false, bool gbuffer = false);
	// Resolve a material's custom shader: true + (key,frag) if it has customShaderFragGlsl.
	bool  ResolveMaterialShader(const HE::UUID& materialId, uint64_t& key, std::string& frag,
	                            std::string& vertBody);
	// Same for the deferred G-buffer variant (customShaderGBufGlsl); false → the
	// material has none and its draws are routed through the forward extra pass.
	bool  ResolveMaterialShaderGB(const HE::UUID& materialId, uint64_t& key, std::string& frag,
	                              std::string& vertBody);
#endif

	// ── Anti-aliasing resolve (docs/anti-aliasing-plan.md) ───────────────────
	// Tonemap writes to m_ldrColor; this pass reads it and writes the final image
	// to the output (viewport or drawable). The METHOD picks the pipeline — FXAA
	// on the gamma-space luma, or a passthrough when the user chose Off. The pass
	// is never skipped: it is what fills the output.
	HE::AAMethod m_aaMethod = HE::AAMethod::FXAA;
	float m_aaSharpness     = 0.35f;  // temporal modes (A3+)
	bool  m_specularAA      = true;   // A6 — roughness regularization in shading
	float m_specularAAStrength = 1.0f;
	void* m_fxaaPipeline   = nullptr; // id<MTLRenderPipelineState>
	void* m_aaBlitPipeline = nullptr; // id<MTLRenderPipelineState> — AA = Off
	void* m_ldrColor     = nullptr; // id<MTLTexture> (retained), tonemap out / FXAA in
	int   m_ldrW         = 0;
	int   m_ldrH         = 0;
	void  EnsureLdrTarget(int width, int height);
	void  DestroyLdrTarget();
	void  EncodeFxaa(void* renderEncoder, int width, int height); // FXAA of m_ldrColor

	// ── In-Game UI (2D canvas elements, drawn after FXAA) ───────────────────
	void* m_uiPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_uiFontTexture = nullptr; // id<MTLTexture>, R8 UI font atlas (UISystem::sharedFont)
	// Imported Font asset atlases, uploaded lazily on first sight (key → id<MTLTexture>).
	std::unordered_map<uint32_t, void*> m_uiFontAtlases;
	void* UIFontAtlasTexture(uint32_t key); // key 0 → the shared atlas
	// Material-on-UI-quad pipelines: same material fragments as the mesh path,
	// paired with the screen-space uiVertex and the LDR/blend target of the UI
	// pass — so they need their own cache (key = material shader hash).
	std::unordered_map<uint64_t, void*> m_uiMaterialPipelines;
	void* GetOrBuildUIMaterialPipeline(const HE::UUID& materialId);
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
	float m_lensFlareParams[4]  = { 0.0f, 0.0f, 1.0f, 0.0f }; // xy sunNDC, z aspect, w strength (tonemap flare)
	glm::mat4 m_prepassViewProj = glm::mat4(1.0f); // camera the low-res cloud pre-pass used → sky pass reprojects it
	void  EnsureBloomTargets(int width, int height);
	void  DestroyBloomTargets();
	// Bright-pass + blur m_hdrColor into m_bloomColor[0]; returns its texture ptr.
	void* EncodeBloom(void* cmdBuf, int fullW, int fullH);

	// ── Low-res clouds (quarter-res cloud pre-pass; EnvironmentSettings.lowResClouds) ──
	// Raymarch the clouds into m_cloudColor (rgb = L, a = T) at quarter resolution; the
	// sky pass then bilinear-upsamples + composites. Uses the PREVIOUS frame's view/sun
	// (clouds are soft; a 1-frame lag in this perf mode is imperceptible) so the pre-pass
	// can run before the scene encoder without re-running the extractor.
	void* m_cloudPipeline = nullptr;  // id<MTLRenderPipelineState> (skyVertex + cloudFragment)
	void* m_cloudColor    = nullptr;  // id<MTLTexture> RGBA16F, quarter-res (L, T)
	int   m_cloudW        = 0;
	int   m_cloudH        = 0;
	void  EnsureCloudTarget(int width, int height);
	void  DestroyCloudTarget();
	void  EncodeCloudPrepass(void* cmdBuf, const glm::mat4& invViewProj, const glm::vec3& sunDir,
	                         float time, int width, int height);

	// ── Cloud shadows (EnvironmentSettings.cloudShadows) ────────────────────
	// One 512² R8 pass per frame renders the cloud slab's sun transmittance
	// over a world-space XZ region around the camera (cloudShadowFragment in
	// kSkyMSL — the same density field the sky raymarches). The lit shaders +
	// heLitP project fragments along the light onto the slab mid-plane and
	// darken the directional term (SceneUniforms/Lighting cloudShadowA/B,
	// texture 16). m_cloudShadowParams* carry the region/strength the encode
	// computed this frame for every fill site; strength 0 = pass skipped.
	void*     m_cloudShadowPipeline = nullptr; // id<MTLRenderPipelineState> (skyVertex + cloudShadowFragment)
	void*     m_cloudShadowTex      = nullptr; // id<MTLTexture> R8, kCloudShadowMapSize²
	glm::vec4 m_cloudShadowParamsA  = glm::vec4(0.0f); // xy origin, z 1/size, w mid-plane Y
	glm::vec4 m_cloudShadowParamsB  = glm::vec4(0.0f); // x strength (0 = off this frame)
	void  EnsureCloudShadowTarget();
	void  DestroyCloudShadowTarget();
	void  EncodeCloudShadow(void* cmdBuf);

	// ── SSAO (screen-space ambient occlusion) ───────────────────────────────
	// Mirrors the GL backend: a view-space position pre-pass feeds a hemisphere-
	// kernel occlusion estimate, blurred and then sampled by the scene shader to
	// darken the image-based ambient in crevices. Encoded before the HDR scene
	// pass (it owns its own render encoders); skipped entirely when disabled.
	void* m_ssaoPosPipeline  = nullptr; // id<MTLRenderPipelineState> (writes view pos)
	void* m_ssaoDepthPosPipeline = nullptr; // deferred P5: view pos from G-buffer depth (fullscreen)
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
	int   m_ssaoMethod    = 0;   // 0 = SSAO, 1 = HBAO, 2 = GTAO
	void  EnsureSSAOTargets(int width, int height);
	void  DestroySSAOTargets();
	// Pre-pass + occlusion + blur for the current scene into m_ssaoBlurTex; sets
	// m_ssaoResult (or null). Runs its own extract/cull/sort (deterministic, so it
	// matches EncodeScene's draw set) and its own render encoders on cmdBuf.
	void  EncodeSSAO(void* cmdBuf, int width, int height);

	// ── Global Illumination (ray-traced DDGI) ───────────────────────────────
	// Metal-only, and only on devices + OS versions that support GPU ray tracing
	// (checked once at Initialize(), see EnsureRaytracingSupport). When enabled on
	// a supported device this COMPLETELY REPLACES CSM shadows and AO/ambient with
	// one ray-traced pipeline: a bottom-level acceleration structure (BLAS) per
	// unique mesh + a top-level acceleration structure (TLAS) rebuilt every frame
	// from the same non-skinned, castsShadow-flagged object set EncodeShadowMap
	// already uses (unculled by camera frustum — rays go in arbitrary directions).
	// This checkpoint only builds the structures; the ray-traced shadow pass and
	// DDGI probe pass that consume them land in later checkpoints.
	bool  m_giRaytracingChecked = false; // EnsureRaytracingSupport() run-once guard
	bool  m_giSupported         = false; // GI available at all (always true once probed — SW path covers no-HW-RT)
	bool  m_giHwRt              = false; // hardware inline ray tracing (macOS 12 + supportsRaytracing, minus HE_GI_FORCE_SW)
	bool  m_giEnabled           = false; // latest SetGISettings().enabled
	float m_giIndirectIntensity = 1.0f;
	float m_giLightRadius       = 0.5f;  // degrees — sun angular radius (shadow penumbra softness)
	int   m_giRaysPerProbe        = 128;
	int   m_giProbeBudgetPerFrame = 256;
	// TLAS + its instance-descriptor buffer are reallocated FRESH every GI-active
	// frame (never mutated/resized in place): the previous frame's build may still
	// be executing on the GPU when this frame starts encoding a new one, and
	// MTLResourceStorageModeShared buffers have no automatic CPU/GPU sync, so
	// overwriting one in place would race an in-flight build. Simpler and safer
	// than tracking capacity/growth; instance buffers are tiny (tens of KB).
	void* m_giTlas           = nullptr; // id<MTLAccelerationStructure> (retained), this frame's build
	void* m_giInstanceBuffer = nullptr; // id<MTLBuffer> (retained), this frame's build
	// Per-instance flat baseColor (float4), SAME index order as the TLAS instance
	// array — get_committed_instance_id() in a ray hit returns that array index
	// (confirmed: MTLAccelerationStructureInstanceDescriptorTypeDefault has no
	// separate user-ID field, so the id IS the array position), letting the DDGI
	// probe-update kernel (Checkpoint C) look up which object it hit and tint the
	// one-bounce estimate by that object's own colour instead of a flat grey.
	// Layout: TWO float4 per instance — [instId*2] = albedo, [instId*2+1] =
	// emissive (giInstanceShading: graph materials via matGraphApproxSurface,
	// Param-driven pins resolved live). The probe bounce reads the albedo, the
	// reflection kernel both.
	void* m_giInstanceColorBuffer = nullptr; // id<MTLBuffer> (retained), this frame's build

	// ── Software ray tracing (no-HW-RT fallback, or HE_GI_FORCE_SW) ──────────
	// The CPU-built HE::GiBvh (same module + unit tests as the GL 4.3 port) in
	// plain MTLBuffers, traversed by base compute kernels (kGISWMSL) that
	// mirror GiBvh.cpp::giBvhIntersect 1:1. Node/tri buffers are concatenated
	// BLASes (append-only, re-uploaded when a new mesh joins); the instance
	// buffer (invTransform + baseColor + BLAS offsets) is rebuilt every frame
	// like the HW TLAS. Everything downstream (G-buffer, temporal, blur,
	// shading, probe atlases) is IDENTICAL to the HW path — only the ray
	// dispatch kernel differs.
	struct GISwBlasRange
	{
		int32_t nodeOffset = 0, triOffset = 0;
		bool    valid      = false;
	};
	struct GISwInstanceCPU // must match kGISWMSL's GiInst (112 bytes)
	{
		glm::mat4 invTransform{1.0f};
		glm::vec4 baseColor{1.0f};
		glm::vec4 emissive{0.0f}; // rgb — reflected emissive surfaces keep their glow
		// landIndex → m_giLandBuf: a terrain chunk's hit is coloured from the
		// PAINT at the hit point, not from this flat baseColor (GiLandscape.h).
		int32_t   nodeOffset = 0, triOffset = 0, landIndex = -1, pad1 = 0;
	};
	// GPU mirror of HE::GiLandscape — must match kGIReflMSL/kGISWMSL's GILand.
	struct GILandGpu
	{
		glm::mat4 worldToLocal{1.0f};
		glm::vec4 cfg{0.0f};       // xy = 1/(sizeX,sizeZ), z = uvTiling, w = layer count
		glm::vec4 layer[4]{};      // per-layer folded colour (rgb)
	};
	static_assert(sizeof(GILandGpu) == 64 + 5 * 16, "must match the MSL GILand layout");
	void* m_giLandBuf         = nullptr; // id<MTLBuffer> (retained), rebuilt per frame
	void* m_giInstanceLandBuf = nullptr; // id<MTLBuffer> (retained), HW per-instance index
	// Packs RenderWorld::landscapes into m_giLandBuf and returns how many made it
	// (capped at HE::kGiMaxLandscapes); `outWeightTex` receives each one's
	// weightmap texture in the same order, for the kernel's texture array.
	int BuildGILandscapeTable(std::vector<void*>& outWeightTex);
	std::unordered_map<HE::UUID, GISwBlasRange> m_giSwBlasCache;
	std::vector<HE::GiBvhNode>     m_giSwNodesCpu;
	std::vector<HE::GiBvhTriangle> m_giSwTrisCpu;
	bool  m_giSwBlasDirty     = false;
	void* m_giSwNodeBuf       = nullptr; // id<MTLBuffer> (retained)
	void* m_giSwTriBuf        = nullptr; // id<MTLBuffer> (retained)
	void* m_giSwInstanceBuf   = nullptr; // id<MTLBuffer> (retained), rebuilt per frame
	int   m_giSwInstanceCount = 0;
	void* m_giShadowRaySwPipeline  = nullptr; // id<MTLComputePipelineState>
	void* m_giProbeUpdateSwPipeline = nullptr; // id<MTLComputePipelineState>
	GISwBlasRange BuildGISwBlas(const HE::UUID& meshId);
	void          EncodeGISwAccelBuild(); // CPU BVH build + buffer upload (no cmd encoding needed)
	// Objects a build just replaced (old TLAS/instance/scratch buffers). GPU work
	// from this frame's build (or a still in-flight prior frame) may still
	// reference them, so they are released a few frames later — same lifetime
	// problem and fix as m_retiredTextures/RetireTexture, generalised to any
	// retained Objective-C object (not texture-specific despite that helper's name).
	struct RetiredGIObject { void* object; int framesLeft; };
	std::vector<RetiredGIObject> m_retiredGIObjects;
	void  RetireGIObject(void* obj);      // hand a retained object over (nullptr-safe no-op)
	void  AgeRetiredGIObjects();          // called once per frame
	void  DrainRetiredGIObjects();        // immediate release (shutdown only)
	void  EnsureRaytracingSupport();          // probes + caches m_giSupported, called once from Initialize()
	void* BuildBLAS(const GpuMesh& mesh);     // one-shot; returns a retained id<MTLAccelerationStructure> or null
	// Lazily builds a BLAS for any not-yet-seen caster mesh, then rebuilds the TLAS
	// from the current frame's caster set. No-op (early return) unless GI is
	// enabled and supported. Shares cmdBuf with EncodeShadowMap/SimulateGpuParticles.
	// aspect MUST be the scene pass's aspect: this call's extract() seeds the
	// m_renderWorld.camera that EncodeGIShadowRays rasterizes its G-buffer with.
	void  EncodeGIAccelBuild(void* cmdBuf, float aspect);
	// Unique BLAS instances the TLAS built this frame actually references (set by
	// EncodeGIAccelBuild, consumed by EncodeGIShadowRays/EncodeGIProbeUpdate) — every
	// compute encoder that traces against m_giTlas must useResource: each of these,
	// since Metal doesn't auto-track residency through an acceleration structure.
	std::vector<void*> m_giUniqueBlas;

	// ── Global Illumination: ray-traced shadow pass (Checkpoint B) ──────────────
	// Replaces CSM's shadowFactor() sampling when GI is active: a half-res
	// world-space G-buffer (position+normal) pre-pass, one ray-traced occlusion
	// sample per pixel (jittered within a cone around the sun for a soft
	// penumbra), temporally accumulated against a ping-pong history (reprojected
	// via the true previous frame's view-proj — NOT the same-frame m_prepassViewProj
	// pattern), then a small spatial blur. Result sampled by fragmentMain exactly
	// like aoTex (screen-space UV, free bilinear upsample from half-res).
	void* m_giGBufPipeline        = nullptr; // id<MTLRenderPipelineState> (MRT: world pos + normal)
	void* m_giShadowRayPipeline   = nullptr; // id<MTLComputePipelineState>
	void* m_giShadowTemporalPipeline = nullptr; // id<MTLRenderPipelineState>
	void* m_giShadowBlurPipeline  = nullptr; // id<MTLRenderPipelineState>
	void* m_giGBufPosTex  = nullptr; // id<MTLTexture> RGBA16F world pos, a=1 valid geometry
	void* m_giGBufNormTex = nullptr; // id<MTLTexture> RGBA16F world normal
	void* m_giGBufDepth   = nullptr; // id<MTLTexture> depth for the prepass only
	void* m_giShadowRawTex = nullptr; // id<MTLTexture> R16F raw 1-ray/pixel result (compute-written)
	// RGBA16F per-pixel visibility of the first 4 LOCAL (point/spot) lights —
	// one hard, unjittered occlusion ray per light per pixel (deterministic →
	// no temporal pass, instant response). Sampled by fragmentMain at
	// texture(8); channel = the shader's non-directional light counter.
	void* m_giLocalMaskTex = nullptr; // id<MTLTexture> (compute-written)
	void* m_giShadowHistory[2] = { nullptr, nullptr }; // id<MTLTexture> R16F ping-pong temporal history
	int   m_giShadowHistoryIdx   = 0;
	bool  m_giShadowHistoryValid = false; // false right after (re)alloc — first frame skips history blend
	void* m_giShadowResult = nullptr; // id<MTLTexture> R16F final blurred result, sampled by fragmentMain
	int   m_giShadowW = 0, m_giShadowH = 0;
	// TRUE previous-frame view-proj (unlike m_prepassViewProj, which is written and
	// read within the SAME frame for the low-res cloud pre-pass) — written at the
	// END of EncodeGIShadowRays from the value used THIS frame, so next frame's
	// reprojection reads last frame's camera, not this frame's.
	glm::mat4 m_giPrevViewProj    = glm::mat4(1.0f);
	float     m_giShadowFrameSeed = 0.0f; // increments every GI-active frame, drives cone jitter
	void  EnsureGIShadowPipelines();              // builds the 4 pipelines above once, only if m_giSupported
	void  EnsureGIShadowTargets(int width, int height);
	void  DestroyGIShadowTargets();
	// No-op (early return) unless GI is enabled/supported/has a built TLAS. Shares
	// m_renderWorld/m_sortedIndices with EncodeGIAccelBuild (called immediately
	// before this in the frame, same extract).
	void  EncodeGIShadowRays(void* cmdBuf, int width, int height);

	// ── Global Illumination: DDGI probes (Checkpoint C) ──────────────────────────
	// Replaces AO + flat/IBL ambient when GI is active: a fixed probe grid over the
	// scene AABB (built once, not every frame — probes encode static indirect
	// light), each probe an 8x8-texel octahedral map. v1 simplifications
	// (documented, not the fully hardened DDGI paper): irradiance AND visibility
	// share ONE octahedral resolution/ray set (not separately super-sampled) —
	// each output texel traces its OWN ray in its own octahedral direction (a
	// "gather", one thread per texel) rather than scattering N random rays into M
	// texels, which needs no atomics/resolve pass since every thread in a probe's
	// update owns exactly one texel; bounce colour is the hit object's flat
	// baseColor (m_giInstanceColorBuffer), not a per-texel material/UV sample; no
	// secondary shadow ray at the hit point (hit surfaces are treated as fully
	// sun-lit); no border-texel wrap (accepts minor bilinear seams at probe tile
	// edges). All are straightforward follow-ups once the base algorithm is
	// visually verified, not correctness bugs.
	static constexpr float kGIProbeSpacing      = 4.0f; // world units between probes
	static constexpr int   kGIMaxProbesPerAxis  = 10;   // caps total probes/memory/cost
	static constexpr int   kGIProbeOctSize      = 8;    // texels/side of each probe's octahedral tile (no border)
	glm::vec3 m_giGridOrigin  = glm::vec3(0.0f); // world-space position of probe (0,0,0)
	glm::ivec3 m_giGridCounts = glm::ivec3(0);   // probe counts per axis
	int   m_giProbeCount   = 0;                  // gridCounts.x*y*z
	int   m_giProbesPerRow = 0;                  // atlas tile layout (ceil(sqrt(probeCount)))
	bool  m_giProbeGridBuilt = false;            // built lazily once; NOT rebuilt on scene change (v1 limitation)
	int   m_giProbeUpdateCursor = 0;             // round-robin index into [0, probeCount) for frame-sliced updates
	void* m_giProbeUpdatePipeline = nullptr;     // id<MTLComputePipelineState>
	void* m_giIrradianceAtlas = nullptr;         // id<MTLTexture> RGBA16F, read_write (in-place EMA blend)
	void* m_giVisibilityAtlas = nullptr;         // id<MTLTexture> RG16F (mean, mean^2 hit distance), read_write
	void  EnsureGIProbeGrid();                   // computes the grid from the scene AABB, once
	void  EnsureGIProbePipeline();                // builds m_giProbeUpdatePipeline once, only if m_giSupported
	void  EnsureGIProbeAtlas();                   // (re)allocates the 2 atlas textures for the current grid
	void  DestroyGIProbeAtlas();
	// No-op (early return) unless GI is enabled/supported/has a built TLAS. Updates
	// up to probeBudgetPerFrame probes this frame (round-robin), tracing
	// raysPerProbe-ish rays each (== kGIProbeOctSize^2, the gather formulation).
	void  EncodeGIProbeUpdate(void* cmdBuf);

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh>         m_meshCache;
	std::unordered_map<HE::UUID, GpuSkeletalMesh> m_skeletalMeshCache;
	std::vector<HE::UUID>                 m_pendingMeshInvalidations;
	std::vector<HE::UUID>                 m_pendingTexInvalidations;

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID (id<MTLTexture>, retained; nullptr = resolved, no texture).
	// InvalidateMaterial retires the texture and drops the entry.
	std::unordered_map<HE::UUID, void*>    m_materialTexCache;
	std::unordered_map<std::string, void*> m_graphTexCache; // node-graph textures by UUID/path key

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

	// ── GPU weather particles (compute simulation + vertex-pull billboards) ──
	// A fixed camera-following rain/snow pool lives in one MTLBuffer (interleaved
	// float4(pos,life) + float4(vel,seed) = 32 B/particle). A compute kernel
	// integrates + recycles it in place once per frame; an attribute-less instanced
	// triangle-strip pulls the buffer in the vertex stage and expands camera-facing
	// billboards. Mirrors the OpenGL transform-feedback path (compute instead of TF).
	void* m_particleSimPipeline  = nullptr; // id<MTLComputePipelineState>
	void* m_particleDrawPipeline = nullptr; // id<MTLRenderPipelineState> (blended billboard)
	void* m_particleBuffer       = nullptr; // id<MTLBuffer>, the pool
	int   m_particleCapacity     = 0;
	bool  m_particleSeeded       = false;
	GpuParticleParams m_gpuParticleParams;  // latest params pushed from the scene tick
	void  CreateParticlePipeline();
	void  EnsureParticleBuffer(int count);
	void  SeedParticleBuffer(int count);
	void  SimulateGpuParticles(void* cmdBuf);                            // compute dispatch
	void  DrawGpuParticles(void* renderEncoder, const glm::mat4& viewProj,
	                       const glm::vec3& camPos);                     // billboard draw

	MetalShaderManager m_shaderManager;
};
