#pragma once
#include <Renderer/IRenderer.h>
#include "OpenGLShaderManager.h"
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

class OpenGLRenderer : public IRenderer
{
public:
	OpenGLRenderer();
	~OpenGLRenderer();
	void Initialize(HE::Window* window) override;
	void Shutdown()                      override;
	void Render()                        override;
	Capabilities GetCapabilities() const override;
	// GPU timing stays unavailable on macOS GL (timestamp queries are unreliable —
	// reported as gpuFrameMs = -1), but the CPU render counters are filled so the
	// profiler/editor show draws/triangles/visible/total on the GL backend too.
	FrameGpuStats GetFrameGpuStats() const override;

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
	void  SetShadowDebug(bool on) override { m_debugShadowCascades = on; }
	void  SetGpuParticleParams(const GpuParticleParams& p) override;
	void  SetDebugLines(const std::vector<DebugLine>& lines) override;

	// Multi-window support
	void AttachWindow(HE::Window* window) override;
	void DetachWindow(HE::Window* window) override;
	void RenderWindow(HE::Window* window) override;

private:
	// Per-frame render counters (main thread), reset + filled by the scene render
	// and returned by GetFrameGpuStats. draws/tris count actual GL draws (instanced
	// batches = 1 draw, tris scaled by instance count); visible/total = culled vs
	// extracted static objects.
	struct FrameCounters { uint32_t draws = 0, tris = 0, visible = 0, total = 0; };
	FrameCounters m_counters;

	// ── GPU timing (profiler per-pass trace) ────────────────────────────────
	// Real per-pass + whole-frame GPU time via GL timer queries: GL_TIME_ELAPSED
	// per pass (exact, exclusive, additive on an immediate-mode GPU — no TBDR
	// overlap, so the sum is meaningful) + a GL_TIMESTAMP pair for the whole frame.
	// Results come back 1–N frames late, so a small ring of query-sets is recycled
	// and reaped when a slot comes back around (no stall); a single-frame / detailed
	// capture does glFinish + same-frame reap so its numbers are that exact frame.
	// Active only while the profiler is recording or its live HUD is open (zero cost
	// otherwise). Disabled on Apple GL, where timestamp queries are unreliable
	// (m_gpuTimerSupported = false → GetFrameGpuStats reports gpuFrameMs = -1).
	struct GpuTimerPass { const char* name = ""; unsigned int query = 0; };
	struct GpuTimerSlot
	{
		unsigned int              tsStart = 0, tsEnd = 0; // GL_TIMESTAMP pair (whole frame)
		std::vector<unsigned int> pool;                  // reusable GL_TIME_ELAPSED query ids
		std::vector<GpuTimerPass> passes;                // (name, query) issued this use
		size_t   poolUsed   = 0;                         // high-water of pool used this frame
		uint64_t frameIdx   = 0;
		bool     pending    = false;                     // has un-reaped results
	};
	static constexpr int kGpuTimerRing = 4;
	GpuTimerSlot  m_gpuSlots[kGpuTimerRing];
	bool          m_gpuTimerSupported   = false;  // false on Apple GL (unreliable)
	bool          m_gpuTimerInit        = false;  // timestamp queries allocated
	uint64_t      m_gpuFrameIdx         = 0;
	int           m_gpuCurSlot          = -1;     // slot in flight this frame (-1 = none)
	bool          m_gpuTimingActive     = false;  // whole-frame timing this (primary) frame
	bool          m_gpuWasActive        = false;  // timing was active last frame (fresh-activation edge)
	bool          m_gpuPerPass          = false;  // per-pass GL_TIME_ELAPSED this frame
	bool          m_gpuDetailed         = false;  // glFinish + same-frame reap this frame
	int           m_gpuActiveQuery      = -1;     // pool index of the open elapsed query (-1 none)
	FrameGpuStats m_lastGpuStats;                 // newest reaped GPU times (merged w/ counters)

	void GpuTimerBeginFrame();   // primary Render() only: latch flags, recycle+reap a slot, tsStart
	void GpuTimerEndFrame();     // tsEnd, mark pending; detailed → glFinish + same-frame reap
	bool GpuTimerBeginPass(const char* name); // begin a GL_TIME_ELAPSED query; true if one was begun
	void GpuTimerEndPass();
	void GpuTimerReap(GpuTimerSlot& slot);    // read a slot's results into m_lastGpuStats
	void DestroyGpuTimer();
	// RAII pass timer: pairs Begin/EndPass across early-returns so an unbalanced
	// begin (→ GL_INVALID_OPERATION on the next glBeginQuery) is impossible.
	struct GpuPassScope
	{
		OpenGLRenderer* r; bool active;
		GpuPassScope(OpenGLRenderer* r_, const char* name) : r(r_), active(r_->GpuTimerBeginPass(name)) {}
		~GpuPassScope() { if (active) r->GpuTimerEndPass(); }
		GpuPassScope(const GpuPassScope&) = delete;
		GpuPassScope& operator=(const GpuPassScope&) = delete;
	};

	// GPU-side mesh, uploaded on first sight from ContentManager data.
	struct GpuMesh
	{
		unsigned int vao        = 0;
		unsigned int vbo        = 0;
		unsigned int ebo        = 0;
		int          indexCount = 0;
		unsigned int texture    = 0;   // base color, 0 = none
		HE::AABB     localBounds;      // object-space bounds for culling
	};

	// GPU-side skeletal mesh: same as GpuMesh but has separate VBOs for bone
	// IDs (uvec4, integer attrib) and bone weights (vec4, float attrib).
	struct GpuSkeletalMesh
	{
		unsigned int vao         = 0;
		unsigned int vbo         = 0;   // interleaved: pos(3)+norm(3)+uv(2) float
		unsigned int boneIdVbo   = 0;   // 4 × uint32 per vertex  → attrib loc 3
		unsigned int boneWgtVbo  = 0;   // 4 × float  per vertex  → attrib loc 4
		unsigned int ebo         = 0;
		int          indexCount  = 0;
		unsigned int texture     = 0;
		HE::AABB     localBounds;
	};

	void CreateUnlitPipeline();
	void CreateSkinnedPipeline();
	void CreateInstancedPipeline();
	void UpdateSkyEnvCube(const glm::vec3& sunDir); // rebuild the IBL cubemap on sun move
	void DrawScene(int width, int height);
	// (Re)creates the offscreen viewport FBO at the requested size.
	void EnsureViewportTarget();
	void DestroyViewportTarget();
	// Returns the GPU mesh for the asset, uploading it on first use.
	// nullptr when the UUID is invalid or the asset is not loaded.
	const GpuMesh*         ResolveMesh        (const HE::UUID& assetId);
	const GpuSkeletalMesh* ResolveSkeletalMesh(const HE::UUID& assetId);

	// Resolves the base-color texture of an explicit MaterialComponent override,
	// uploading it on first sight and caching by material UUID. Returns true if
	// the material was found (outTex may still be 0 = material has no texture);
	// false when the UUID is null or the material is not loaded yet.
	bool ResolveMaterialTexture(const HE::UUID& materialId, unsigned int& outTex);

	// Resolves a material override's PBR scalars (baseColor/metallic/roughness/
	// opacity). Returns true if the material is loaded; leaves the outputs
	// untouched otherwise (caller keeps its defaults).
	bool ResolveMaterialParams(const HE::UUID& materialId,
	                           glm::vec3& outBaseColor, float& outMetallic, float& outRoughness,
	                           float& outOpacity);

	SDL_Window* m_primarySdlWindow = nullptr;   // needed to restore current context
	void*       m_glContext        = nullptr;   // borrowed — owned by primary HE::Window
	// Secondary windows: SDL_Window* → shared SDL_GLContext (owned here)
	std::unordered_map<SDL_Window*, void*> m_secondaryContexts;
	OpenGLShaderManager m_shaderManager;

	// ── Scene rendering ─────────────────────────────────────────────────────
	RenderExtractor m_extractor;
	RenderWorld     m_renderWorld;
	FrustumCuller   m_culler;
	RenderSorter    m_sorter;
	RenderGraph     m_renderGraph;   // pass pipeline (GeometryPass today)
	CommandBuffer   m_cmds;          // draw calls produced this frame
	std::vector<uint8_t>  m_visible;       // per-frame culling results
	std::vector<uint32_t> m_sortedIndices; // per-frame draw order

	// Unlit pipeline + built-in cube (fallback for entities whose mesh
	// asset is missing or not loaded)
	unsigned int m_unlitProgram   = 0;
	int          m_uMVP           = -1;
	int          m_uModel         = -1;
	int          m_uColor         = -1;
	int          m_uHasTexture    = -1;
	int          m_uTexture       = -1;
	int          m_uMetallic      = -1;
	int          m_uRoughness     = -1;
	int          m_uOpacity       = -1;   // surface alpha (transparency pass)
	int          m_uLightCount    = -1;
	int          m_uLightPos      = -1;
	int          m_uLightDir      = -1;
	int          m_uLightColor    = -1;
	int          m_uLightParams   = -1;
	int          m_uCameraPos     = -1;
	int          m_uSunDir        = -1;   // toward-sun dir for image-based ambient
	int          m_uAmbient       = -1;   // flat ambient fill (floor + overcast)
	int          m_uFogDensity       = -1; // atmospheric fog amount (0 = off)
	int          m_uFogHeightFalloff = -1; // fog height falloff
	int          m_uCascadeVP     = -1;   // mat4[kGLCsmCascades] per-cascade light view-proj
	int          m_uCascadeSplits = -1;   // vec4: xyz = cascade far dist (view), w = count
	int          m_uCameraFwd     = -1;   // world forward (planar view-Z cascade select)
	int          m_uShadowMap     = -1;   // CSM shadow-map array sampler unit
	int          m_uShadowEnabled = -1;
	int          m_uShadowDebug   = -1;   // 1 = tint fragments by cascade index
	int          m_uAO            = -1;   // SSAO occlusion sampler unit
	int          m_uViewport      = -1;   // viewport size (screen-space AO lookup)
	int          m_uSSAOEnabled   = -1;   // 1 = modulate ambient by SSAO
	int          m_uWetness       = -1;   // weather wet-surface response
	int          m_uSnow          = -1;   // weather snow cover
	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh>         m_meshCache;
	std::unordered_map<HE::UUID, GpuSkeletalMesh> m_skeletalMeshCache;

	// ── Skinned mesh pipeline ────────────────────────────────────────────────
	unsigned int m_skinnedProgram   = 0;
	int          m_uSkinnedMVP      = -1;
	int          m_uSkinnedModel    = -1;
	int          m_uSkinnedBones    = -1;   // mat4[128] uniform array location
	int          m_uSkinnedColor    = -1;
	int          m_uSkinnedHasTex   = -1;
	int          m_uSkinnedTex      = -1;
	int          m_uSkinnedMetallic = -1;
	int          m_uSkinnedRoughness= -1;
	int          m_uSkinnedOpacity  = -1;
	int          m_uSkinnedLightCount= -1;
	int          m_uSkinnedLightPos  = -1;
	int          m_uSkinnedLightDir  = -1;
	int          m_uSkinnedLightColor= -1;
	int          m_uSkinnedLightParams=-1;
	int          m_uSkinnedCameraPos = -1;
	int          m_uSkinnedAmbient   = -1;
	int          m_uSkinnedSunDir    = -1;
	int          m_uSkinnedSkyEnv    = -1;
	int          m_uSkinnedFogDensity      = -1;
	int          m_uSkinnedFogHeightFalloff= -1;
	int          m_uSkinnedShadowEnabled   = -1;
	int          m_uSkinnedCascadeVP       = -1;
	int          m_uSkinnedCascadeSplits   = -1;
	int          m_uSkinnedCameraFwd       = -1;
	int          m_uSkinnedShadowDebug     = -1;
	int          m_uSkinnedShadowMap       = -1;
	int          m_uSkinnedAO              = -1;
	int          m_uSkinnedViewport        = -1;
	int          m_uSkinnedSSAOEnabled     = -1;

	// ── GPU-instanced pipeline (same-mesh batching via glDrawElementsInstanced) ─
	// kInstancedVS reads per-instance model matrices from a VBO at attrib locs 4–7
	// (divisor = 1); kUnlitFS is shared with the unlit pipeline.
	unsigned int m_instancedProgram         = 0;
	unsigned int m_instanceVBO              = 0;   // scratch instance-transform VBO
	int          m_uInstViewProj            = -1;  // vertex: uViewProj
	int          m_uInstColor               = -1;
	int          m_uInstHasTexture          = -1;
	int          m_uInstTexture             = -1;
	int          m_uInstMetallic            = -1;
	int          m_uInstRoughness           = -1;
	int          m_uInstOpacity             = -1;
	int          m_uInstLightCount          = -1;
	int          m_uInstLightPos            = -1;
	int          m_uInstLightDir            = -1;
	int          m_uInstLightColor          = -1;
	int          m_uInstLightParams         = -1;
	int          m_uInstCameraPos           = -1;
	int          m_uInstSunDir              = -1;
	int          m_uInstSkyEnv              = -1;
	int          m_uInstAmbient             = -1;
	int          m_uInstFogDensity          = -1;
	int          m_uInstFogHeightFalloff    = -1;
	int          m_uInstCascadeVP           = -1;
	int          m_uInstCascadeSplits       = -1;
	int          m_uInstCameraFwd           = -1;
	int          m_uInstShadowDebug         = -1;
	int          m_uInstShadowMap           = -1;
	int          m_uInstShadowEnabled       = -1;
	int          m_uInstAO                  = -1;
	int          m_uInstViewport            = -1;
	int          m_uInstSSAOEnabled         = -1;
	int          m_uInstWetness             = -1;
	int          m_uInstSnow                = -1;

	// ── GPU weather particles (transform-feedback precipitation) ────────────
	// A fixed pool of rain/snow drops lives in two ping-pong VBOs (interleaved
	// pos/vel/life/seed, 8 floats each). m_particleSimProgram integrates + recycles
	// them via transform feedback (rasterizer discard); m_particleDrawProgram pulls
	// the written buffer as per-instance data and expands an attribute-less quad
	// (gl_VertexID) into camera-facing billboards. See SetGpuParticleParams / the
	// SimulateGpuParticles + DrawGpuParticles passes.
	unsigned int m_particleSimProgram  = 0;   // VS-only, transform feedback
	unsigned int m_particleDrawProgram = 0;   // billboard VS + FS
	unsigned int m_particleBuf[2]      = {0, 0};
	unsigned int m_particleSimVAO[2]   = {0, 0};  // buf as per-vertex attribs (sim input)
	unsigned int m_particleDrawVAO[2]  = {0, 0};  // buf as per-instance attribs (draw)
	int          m_particleCur         = 0;   // index of the freshest buffer
	int          m_particleCapacity    = 0;   // allocated pool size
	bool         m_particleInit        = false; // buffers seeded with the starting pool
	GpuParticleParams m_gpuParticles;          // latest params pushed from the scene tick
	// sim uniforms
	int m_uPSimDt = -1, m_uPSimTime = -1, m_uPSimCamPos = -1, m_uPSimWind = -1;
	int m_uPSimCoverage = -1, m_uPSimFall = -1, m_uPSimLife = -1, m_uPSimGround = -1;
	int m_uPSimBoxHalf = -1, m_uPSimBoxTop = -1, m_uPSimSnow = -1;
	// draw uniforms
	int m_uPDrawViewProj = -1, m_uPDrawCamRight = -1, m_uPDrawCamUp = -1;
	int m_uPDrawCamPos = -1, m_uPDrawSnow = -1, m_uPDrawLife = -1;

	void CreateParticlePipeline();
	void EnsureParticleBuffers(int count);
	void SeedParticleBuffer(int count);
	void DestroyParticleResources();
	void SimulateGpuParticles();
	void DrawGpuParticles(const glm::mat4& viewProj, const glm::vec3& camPos);

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID. A present entry of 0 means "resolved, no texture". Drained/cleared
	// by InvalidateMaterial via m_pendingMaterialInvalidations.
	std::unordered_map<HE::UUID, unsigned int> m_materialTexCache;
	std::vector<HE::UUID>                       m_pendingMaterialInvalidations;
	std::vector<HE::UUID>                       m_pendingMeshInvalidations;

	// ── Shadow map (cascaded; directional light) ────────────────────────────
	// m_shadowDepthTex is a GL_TEXTURE_2D_ARRAY (one Depth24 layer per cascade);
	// the shadow pass renders each cascade into its layer and the scene shader
	// samples the array with planar view-Z cascade selection. Mirrors the Metal
	// backend's CSM (D3D/Vulkan still use the legacy single map).
	unsigned int m_shadowFBO      = 0;
	unsigned int m_shadowDepthTex = 0;   // GL_TEXTURE_2D_ARRAY, Depth24, one layer/cascade
	int          m_shadowSize     = 2048;
	unsigned int m_depthProgram   = 0;   // depth-only pass (cascadeVP * model * pos)
	int          m_uDepthMVP      = -1;
	bool         m_debugShadowCascades = false; // tint fragments by cascade index (debug)
	// Per-cascade caster culling scratch (kept off m_visible/m_sortedIndices so the
	// shadow pass never clobbers the camera cull the geometry pass relies on).
	std::vector<uint8_t>  m_shadowVisible;
	std::vector<uint32_t> m_shadowSorted;
	void CreateShadowResources();

	// ── Procedural skybox (drawn into the HDR target behind the scene) ───────
	unsigned int m_skyProgram     = 0;
	int          m_uSkyInvVP      = -1;
	int          m_uSkySunDir     = -1;
	int          m_uSkyMoonTex    = -1;   // moon texture sampler unit
	int          m_uSkyHasMoon    = -1;   // 1 when a moon texture is bound
	int          m_uSkyMoonPhase  = -1;   // lunar phase 0..1
	int          m_uSkyTime       = -1;   // time of day (cloud scroll phase)
	int          m_uSkyCoverage   = -1;   // cloud amount (0 clear … 1 overcast)
	int          m_uSkyClock      = -1;   // wall-clock seconds (star twinkle)
	int          m_uSkySunColor   = -1;   // sun light colour (cloud tint)
	int          m_uSkyAurora     = -1;   // aurora intensity (0 = off)
	int          m_uSkyMilkyWay    = -1;  // milky-way (dense star band) intensity
	int          m_uSkyNebula      = -1;  // space-nebula intensity
	int          m_uSkyNebulaColor = -1;  // space-nebula colour 1
	int          m_uSkyNebulaColor2 = -1; // space-nebula colour 2
	int          m_uSkyNebulaColor3 = -1; // space-nebula colour 3
	int          m_uSkyNebulaSeed  = -1;  // space-nebula seed
	int          m_uSkyNebulaHiFi  = -1;  // space-nebula fidelity mode
	int          m_uSkyAuroraColor = -1;  // aurora base colour
	int          m_uSkyAuroraColorTop = -1; // aurora upper colour
	int          m_uSkyAuroraHeight   = -1; // aurora band elevation
	int          m_uSkyAuroraFragment = -1; // aurora streak fragmentation
	int          m_uSkyWind        = -1;  // cloud drift vector
	int          m_uSkyNoise       = -1;  // 3D value-noise sampler
	int          m_uSkyFlash       = -1;  // lightning flash brightness
	int          m_uSkyCloudMode   = -1;  // 0 = sky-dome clouds, 1 = 3D volumetric
	int          m_uSkyCloudQuality = -1; // cloud raymarch quality: 0 Low, 1 Med, 2 High
	int          m_uSkyCameraPos   = -1;  // camera world position (3D-cloud parallax)
	int          m_uSkyCloudHeight = -1;  // 3D cloud layer height above the camera
	int          m_uSkyCloudDensity   = -1; // cloud opacity/density multiplier
	int          m_uSkyCloudFluffiness = -1; // cloud erosion / billow strength
	int          m_uSkyCloudTint      = -1; // cloud colour tint
	int          m_uSkyContrails      = -1; // contrail (vapour-trail) amount
	int          m_uSkyCirrus         = -1; // thin high cirrus cloud amount
	int          m_uSkyCirrusSeed     = -1; // cirrus pattern seed
	int          m_uSkyStarBright      = -1; // star field brightness multiplier
	int          m_uSkyStarColor       = -1; // star field colour tint
	int          m_uSkyStarSize        = -1; // star size multiplier
	int          m_uSkyStarSizeVar     = -1; // star size variation
	int          m_uSkyStarDensity     = -1; // star amount/density
	int          m_uSkyStarGlow        = -1; // star glow/halo amount
	int          m_uSkyStarTwinkle     = -1; // star twinkle amount
	unsigned int m_noiseTex        = 0;   // GL_TEXTURE_3D, R16 value noise
	int          m_uSkyEnv         = -1;  // image-based-ambient cubemap sampler
	unsigned int m_skyEnvCube      = 0;   // GL_TEXTURE_CUBE_MAP, baked skyColor
	glm::vec3    m_skyEnvSunDir    = glm::vec3(0.0f); // sun dir the cubemap was baked for
	bool         m_skyEnvValid     = false;
	unsigned int m_moonTex        = 0;    // night-sky moon texture (or 0)
	void CreateSkyPipeline();

	// ── Debug line overlay ───────────────────────────────────────────────────
	unsigned int m_debugLineProgram = 0;
	int          m_uDebugVP         = -1;
	unsigned int m_debugLineVAO     = 0;
	unsigned int m_debugLineVBO     = 0;
	std::vector<DebugLine> m_debugLines;
	void CreateDebugLinePipeline();
	void DrawDebugLines(const glm::mat4& viewProj);

	// ── HDR scene color + tonemap (PostProcessPass) ─────────────────────────
	// GeometryPass renders into an RGBA16F target; PostProcessPass tonemaps it
	// to the backbuffer/viewport. Sized to the current output, recreated on resize.
	unsigned int m_hdrFBO        = 0;
	unsigned int m_hdrColor      = 0;   // RGBA16F
	unsigned int m_hdrDepth      = 0;   // renderbuffer
	int          m_hdrW          = 0;
	int          m_hdrH          = 0;
	unsigned int m_tonemapProgram = 0;
	int          m_uHDRTex        = -1;
	int          m_uExposure      = -1;
	int          m_uBloomTex      = -1;
	int          m_uBloomStrength = -1;
	unsigned int m_fsVAO          = 0;  // empty VAO for the fullscreen triangle
	void CreateTonemapPipeline();
	void EnsureHDRTarget(int width, int height);
	void DestroyHDRTarget();

	// ── FXAA (edge antialiasing) ─────────────────────────────────────────────
	// The tonemap pass writes its LDR result into m_ldrColor instead of straight
	// to the output; this pass reads that texture, runs FXAA on the perceptual
	// (gamma-space) luma and writes the antialiased result to the output. Always on.
	unsigned int m_fxaaProgram   = 0;
	int          m_uFxaaScene    = -1;
	int          m_uFxaaRcpFrame = -1;
	unsigned int m_ldrFBO        = 0;
	unsigned int m_ldrColor      = 0;   // RGBA8 tonemap output, FXAA input
	int          m_ldrW          = 0;
	int          m_ldrH          = 0;
	void EnsureLdrTarget(int width, int height);
	void DestroyLdrTarget();

	// ── In-Game UI (2D canvas elements, drawn after FXAA) ───────────────────
	unsigned int m_uiProgram     = 0;
	int          m_uUIRect       = -1;
	int          m_uUIViewport   = -1;
	int          m_uUIColor      = -1;
	void         RenderUIPass(int pw, int ph);

	// ── Bloom (bright-pass + separable Gaussian blur on the HDR target) ──────
	// The bright pass extracts highlights above a soft-knee threshold into a
	// half-res RGBA16F target; two ping-pong buffers blur it; the tonemap pass
	// adds the result back. Always on, mirrors the GL/Metal HDR convention.
	unsigned int m_bloomBrightProgram = 0;
	int          m_uBrightHDR       = -1;
	int          m_uBrightThreshold = -1;
	int          m_uBrightKnee      = -1;
	unsigned int m_blurProgram      = 0;
	int          m_uBlurImage      = -1;
	int          m_uBlurTexel      = -1;
	int          m_uBlurHorizontal = -1;
	unsigned int m_bloomFBO[2]   = { 0, 0 };
	unsigned int m_bloomColor[2] = { 0, 0 };   // RGBA16F, half-res
	int          m_bloomW        = 0;
	int          m_bloomH        = 0;
	// ── Low-res clouds (quarter-res cloud pre-pass; EnvironmentSettings.lowResClouds) ──
	// Reuses m_skyProgram with uCloudPrepass=1 to raymarch clouds into m_cloudTex (rgb=L,
	// a=T) at quarter res; the sky pass upsamples + composites it. Uses the previous
	// frame's view/sun (1-frame lag, imperceptible) so the pre-pass needs no re-extract.
	unsigned int m_cloudFBO = 0;
	unsigned int m_cloudTex = 0;               // RGBA16F, quarter-res (L, T)
	int          m_cloudW   = 0;
	int          m_cloudH   = 0;
	int          m_uSkyCloudTex     = -1;
	int          m_uSkyLowResClouds = -1;
	int          m_uSkyCloudPrepass = -1;
	int          m_uSkyRainAmount   = -1;
	glm::mat4    m_lastInvViewProj = glm::mat4(1.0f); // previous frame, for the cloud pre-pass
	glm::vec3    m_lastSunDir      = glm::vec3(0.0f, 1.0f, 0.0f);
	void         EnsureCloudFBO(int width, int height);
	void         DestroyCloudFBO();
	bool         m_bloomEnabled   = true;
	float        m_bloomThreshold = 1.0f;
	float        m_bloomKnee      = 0.5f;
	float        m_bloomStrength  = 0.6f;
	void CreateBloomPipeline();
	void EnsureBloomTargets(int width, int height);
	void DestroyBloomTargets();
	// Runs bright-pass + blur into m_bloomColor[0]; returns its texture id (or 0).
	unsigned int RenderBloom(int fullW, int fullH);

	// ── SSAO (screen-space ambient occlusion) ───────────────────────────────
	// A view-space position pre-pass (camera POV) feeds a hemisphere-kernel
	// occlusion estimate, blurred and then sampled by the scene shader to darken
	// the image-based ambient in crevices. Runs before the geometry pass; skipped
	// entirely (zero cost) when disabled. Always full-resolution.
	unsigned int m_ssaoPosProgram = 0;   // pre-pass: writes view-space position
	int          m_uPosMVP        = -1;   // clip = viewProj * model
	int          m_uPosModelView  = -1;   // view * model (view-space position out)
	unsigned int m_ssaoProgram    = 0;   // fullscreen occlusion estimate
	int          m_uSsaoViewPos   = -1;
	int          m_uSsaoNoise     = -1;
	int          m_uSsaoProj      = -1;
	int          m_uSsaoNoiseScale = -1;
	int          m_uSsaoRadius    = -1;
	int          m_uSsaoBias      = -1;
	int          m_uSsaoIntensity = -1;
	int          m_uSsaoKernel    = -1;
	int          m_uAOMethod      = -1;  // 0 = SSAO, 1 = HBAO, 2 = GTAO
	unsigned int m_ssaoBlurProgram = 0;  // fullscreen 4×4 box blur
	int          m_uBlurAO        = -1;
	unsigned int m_ssaoPosFBO     = 0;   // view-space position target + depth
	unsigned int m_ssaoPosTex     = 0;   // RGBA16F: xyz view pos, a = valid
	unsigned int m_ssaoPosDepth   = 0;   // depth renderbuffer (nearest surface)
	unsigned int m_ssaoFBO        = 0;   // raw occlusion target
	unsigned int m_ssaoTex        = 0;   // R8
	unsigned int m_ssaoBlurFBO    = 0;   // blurred occlusion target
	unsigned int m_ssaoBlurTex    = 0;   // R8 (sampled by the scene shader)
	unsigned int m_ssaoNoiseTex   = 0;   // 4×4 random rotation vectors
	unsigned int m_whiteTex       = 0;   // 1×1 white (bound as AO when disabled)
	int          m_ssaoW          = 0;
	int          m_ssaoH          = 0;
	bool         m_ssaoEnabled    = true;
	float        m_ssaoRadius     = 0.5f;
	float        m_ssaoIntensity  = 1.0f;
	int          m_ssaoMethod     = 0;   // 0 = SSAO, 1 = HBAO, 2 = GTAO
	void CreateSSAOPipeline();           // programs + kernel + noise texture
	void EnsureSSAOTargets(int width, int height);
	void DestroySSAOTargets();
	// Pre-pass + occlusion + blur using the geometry draw calls; returns the
	// blurred AO texture id (or 0 if unavailable). Restores GL_TEXTURE0 active.
	unsigned int RenderSSAO(const CommandBuffer& cmds, int pw, int ph,
	                        const glm::mat4& viewProj, const glm::mat4& view,
	                        const glm::mat4& proj);

	// ── Offscreen viewport (editor scene view) ──────────────────────────────
	uint32_t     m_viewportReqW  = 0;   // requested by the UI, 0 = direct to window
	uint32_t     m_viewportReqH  = 0;
	int          m_viewportW     = 0;   // current FBO size
	int          m_viewportH     = 0;
	unsigned int m_viewportFBO   = 0;
	unsigned int m_viewportColor = 0;   // GL_TEXTURE_2D, doubles as ImTextureID
	unsigned int m_viewportDepth = 0;   // renderbuffer

	// Color textures replaced on viewport resize. The current frame's ImGui
	// draw list still references the old GL texture id, so deletion is
	// deferred a few frames — never done in the frame it was retired.
	struct RetiredTexture { unsigned int texture; int framesLeft; };
	std::vector<RetiredTexture> m_retiredTextures;
	void AgeRetiredTextures();
};
