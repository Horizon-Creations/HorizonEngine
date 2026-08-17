#pragma once
#include <cstdint>
#include <Renderer/IRenderer.h>
#include "OpenGLShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <HorizonRendering/RenderConstants.h>
#include <HorizonRendering/GiBvh.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <material/MaterialShaderLibrary.h> // shared cross-backend material shader layer
#include <unordered_map>
#include <string>

struct SDL_Window;
struct MaterialShaderVariant; // ContentManager/Assets.h — baked per-backend material shader
struct ParticleShaderVariant; // ContentManager/Assets.h — baked per-backend particle shader

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
	void  SetGISettings(const GISettings& settings) override;
	void  SetGIReflectionSettings(const GIReflectionSettings& settings) override;
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
	GpuTimerSlot  m_gpuSlots[HE::kGpuTimerRing];
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

	// ── Per-frame scene lighting + CSM uniforms (one block, three programs) ──
	// The unlit, instanced and skinned programs are all linked from the shared
	// kUnlitFS text, so they declare byte-identical light/shadow uniforms and
	// only the location integers differ. One location set per program + one
	// writer, instead of three copies of the fill loop inside DrawScene.
	struct SceneLightingLocs
	{
		int lightCount, lightPos, lightDir, lightColor, lightParams, cameraPos;
		int shadowEnabled, shadowDebug, cascadeVP, cascadeSplits, cameraFwd, shadowMap;
		int localShadowMap, localShadowVP;
	};
	// The per-frame shadow inputs the block needs (all DrawScene locals).
	struct SceneShadowFrame
	{
		const float* cascadeVPData;  // kGLCsmCascades contiguous mat4 (GL clip space)
		glm::vec4    cascadeSplits;  // xyz = far distance per cascade, w = count
		glm::vec3    cameraFwd;      // world forward, for planar cascade selection
		const float* localVPData;    // nLocalLayers contiguous mat4
		int          nLocalLayers;
		bool         shadows;
		bool         localShadows;
	};
	void BindSceneLighting(const SceneLightingLocs& locs, const SceneShadowFrame& frame) const;
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
	unsigned int ResolveGraphTexture(const HE::UUID& id, const std::string& path);

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

	// Per-material GL programs cross-compiled from MaterialAsset::customShaderFragGlsl via
	// the shared MaterialShaderLibrary (→ GLSL 410, attribute vertex + lighting-UBO fragment).
	// Same VAO/attribs as the unlit program; per-object + lighting fed via UBOs (blocks
	// "U" @ binding 1, "HeLighting" @ binding 0). Selected per-draw in the opaque loop.
	HE::MaterialShaderLibrary                  m_matShaderLib;
	std::unordered_map<uint64_t, unsigned int> m_materialPrograms; // hash → program (0 = failed)
	unsigned int m_matObjUBO   = 0;  // per-object U   (mvp/model/color/flags/pbr), 176 B
	unsigned int m_matLightUBO = 0;  // HeLighting     (sunDir/sunColor/ambient/camPos), 64 B
	unsigned int m_matParamUBO = 0;  // HeParams       (exposed graph parameters), 256 B
	// Material-preview target (RenderMaterialPreview) — a small dedicated FBO + a
	// lazily-built unit sphere, independent of the main viewport.
	unsigned int m_previewFBO = 0, m_previewColor = 0, m_previewDepth = 0;
	int          m_previewSize = 0;
	unsigned int m_previewVAO = 0, m_previewVBO = 0, m_previewIBO = 0;
	int          m_previewIdxCount = 0;
	int          m_previewShape    = -1; // which primitive the VBO/IBO hold (-1 = none)
	// Per-draw UBO-upload dedup: the lighting UBO is frame-constant (upload once per
	// frame), and HeParams rarely changes (upload only when its 64 floats differ from
	// what the UBO already holds — still correct for per-entity param overrides).
	bool         m_matLightUploadedThisFrame = false;
	float        m_lastMatParams[64] = { 0 };
	bool         m_haveMatParams = false;

	// Skeletal-mesh-preview target (RenderSkeletalPreview) — own dedicated FBO,
	// independent of both the main viewport and the material-preview target
	// (different vertex layout/program, may be a different size). Deliberately a
	// MINIMAL self-contained skinning shader (fixed sun+ambient, no shadow/SSAO/
	// fog/sky-env) rather than the full scene-integrated m_skinnedProgram — same
	// "isolated preview, own tiny lighting model" idea as the material preview.
	unsigned int m_skelPreviewFBO = 0, m_skelPreviewColor = 0, m_skelPreviewDepth = 0;
	int          m_skelPreviewW = 0;
	int          m_skelPreviewH = 0;
	unsigned int m_skelPreviewProgram = 0;
	int          m_uSkelPvMVP = -1, m_uSkelPvModel = -1, m_uSkelPvBones = -1;
	int          m_uSkelPvColor = -1, m_uSkelPvHasTex = -1;
	// Optional sun for the world preview (w == 0 = the fixed studio light every
	// thumbnail was rendered with — see the shader).
	int          m_uSkelPvSun = -1, m_uSkelPvSunColor = -1, m_uSkelPvAmbient = -1;
	// Simple immediate-mode line pass for the bone overlay, drawn into the same
	// target right after the skinned mesh (own tiny program — DebugDrawBuffer's
	// pipeline targets the backbuffer scene, not an arbitrary offscreen FBO).
	unsigned int m_skelPreviewLineProgram = 0, m_skelPreviewLineVAO = 0, m_skelPreviewLineVBO = 0;
	int          m_uSkelPvLineMVP = -1;
	// Compile the skinning + line programs (and the line VAO/VBO) once. Both the
	// skeletal preview and the world preview need them, and a second copy of the
	// compile block is exactly how the two would drift apart.
	bool EnsureSkelPreviewPrograms();
	// Same for the unskinned kMeshPreview* program.
	bool EnsureMeshPreviewProgram();

	// World-preview target (RenderWorldPreview) — its own FBO again, for the same
	// reason as all the others: the Class Editor's viewport is live at the same
	// time as thumbnails are being rendered. ONE target, because only one asset
	// tab is ever active. Unlike the per-asset previews this one clears to an
	// opaque gray and draws a ground plane + grid, so it reads as a scene view.
	unsigned int m_worldPreviewFBO = 0, m_worldPreviewColor = 0, m_worldPreviewDepth = 0;
	int          m_worldPreviewW = 0;
	int          m_worldPreviewH = 0;

	// Particle-preview target (RenderParticlePreview) — own dedicated FBO; camera-
	// facing billboard quads via gl_VertexID (no per-vertex buffer, matching the
	// fullscreen-tri convention used elsewhere in this file), one dynamic instance
	// buffer of {pos3,size1,color3,alpha1} re-uploaded every call.
	unsigned int m_particlePreviewFBO = 0, m_particlePreviewColor = 0, m_particlePreviewDepth = 0;
	int          m_particlePreviewSize = 0;
	unsigned int m_particlePreviewProgram = 0, m_particlePreviewInstVBO = 0, m_particlePreviewVAO = 0;
	int          m_uPPvViewProj = -1, m_uPPvCamRight = -1, m_uPPvCamUp = -1, m_uPPvHasTex = -1;

	// ── Content-Browser thumbnails (RenderAssetThumbnail) ────────────────────
	// Its own FBO, deliberately NOT any of the preview targets above: a thumbnail
	// is rendered while the Material Editor may be showing its live preview, and
	// sharing m_previewFBO would replace that preview's contents with whatever
	// asset the grid happened to ask for. Read back to RGBA8 and cached by the
	// editor, so this target only ever holds one thumbnail at a time.
	unsigned int m_thumbFBO = 0, m_thumbColor = 0, m_thumbDepth = 0;
	int          m_thumbSize = 0;
	// Unlit-ish mesh program shared by the mesh thumbnails and by materials that
	// have no node graph (built-in PBR): pos/normal/uv in, fixed sun + ambient
	// plus a roughness/metallic-driven highlight, so a flat material still reads
	// as a shaded sphere instead of a silhouette.
	unsigned int m_meshPreviewProgram = 0;
	int          m_uMeshPvMVP = -1, m_uMeshPvModel = -1, m_uMeshPvColor = -1;
	int          m_uMeshPvHasTex = -1, m_uMeshPvCamPos = -1, m_uMeshPvPbr = -1;
	int          m_uMeshPvSun = -1, m_uMeshPvSunColor = -1, m_uMeshPvAmbient = -1;
	// Lazily (re)create the thumbnail target at S×S; false if it could not be made.
	bool EnsureThumbnailTarget(int S);
	// Read the bound thumbnail target back as tightly packed, TOP-DOWN RGBA8.
	void ReadThumbnailTarget(int S, std::vector<uint8_t>& outRgba8);
	// Draws one material-graph preview primitive into the CURRENTLY BOUND target
	// (caller owns FBO/viewport/clear). False when the material has no node-graph
	// program — the thumbnail path then falls back to m_meshPreviewProgram.
	// `meshId` (optional) draws that static mesh instead of the primitive, framed
	// on its own bounds; an unresolvable mesh falls back to `shape`.
	bool DrawMaterialPreviewGeometry(const HE::UUID& materialId, float yaw, float pitch,
	                                 float dist, int shape,
	                                 const HE::UUID& meshId = HE::UUID{});
	// Compile the billboard program + instance VAO once; false on failure.
	bool EnsureParticlePreviewProgram();
	// Draw the particle cloud into the CURRENTLY BOUND target — shared by the
	// interactive preview and the thumbnail so the two can never drift.
	void DrawParticlePreviewGeometry(const HE::UUID& materialId,
	                                 const std::vector<ParticlePreviewInstance>& particles,
	                                 float yaw, float pitch, float dist);
	// Same contract for the mesh program: `vao`/`indexCount`/`texture` describe the
	// geometry, `center`/`extent` frame the orbit camera.
	void DrawMeshPreviewGeometry(unsigned int vao, int indexCount, unsigned int texture,
	                             const glm::vec3& center, float extent, const glm::vec3& baseColor,
	                             float metallic, float roughness, float yaw, float pitch, float dist);

	// GPU-instanced ParticleGraph particle rendering (the real scene draw path, see
	// RenderWorld::particleBatches) — one compiled program per unique color/alpha-
	// over-life config, hash-keyed cache mirroring m_materialPrograms; a single
	// shared instance VAO/VBO re-uploaded per batch (batches don't overlap within a
	// frame, so one scratch buffer suffices). Drawn in the transparent pass.
	std::unordered_map<uint64_t, unsigned int> m_particlePrograms; // hash → program (0 = failed)
	unsigned int m_particleInstVBO = 0, m_particleVAO = 0;
	unsigned int GetOrBuildParticleProgram(uint64_t key, const HE::ParticleEmitterConfig& config,
	                                       const ParticleShaderVariant* precompiled);

	unsigned int GetOrBuildMaterialProgram(uint64_t key, const std::string& fragGlsl,
	                                       const std::string& vertBody = {},
	                                       const MaterialShaderVariant* precompiled = nullptr);
	// Create the three UBOs every material program draws through (U / HeLighting /
	// HeParams) unless they already exist. Called by BOTH material-program getters
	// before they can return — including via a memo hit or the on-disk program
	// cache, neither of which compiles anything (see the definition).
	void         EnsureMaterialUBOs();
	bool         resolveMaterialShader(const HE::UUID& materialId, uint64_t& key, std::string& frag,
	                                   std::string& vertBody);
	// Deferred G-buffer variant (customShaderGBufGlsl): false → the material has
	// none and its draws run forward in the lighting pass. The returned key is a
	// hash of the G-buffer SOURCE, so its programs share m_materialPrograms
	// without ever colliding with the forward key space.
	bool         resolveMaterialShaderGB(const HE::UUID& materialId, uint64_t& key, std::string& frag,
	                                     std::string& vertBody);
	// UI-quad material programs: same fragment hash, but linked against the
	// screen-space uiVertex instead of the mesh vertex → own cache.
	std::unordered_map<uint64_t, unsigned int> m_uiMaterialPrograms; // hash → program (0 = failed)
	unsigned int GetOrBuildUIMaterialProgram(const HE::UUID& materialId);

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
	int          m_uLocalShadowVP  = -1;  // mat4[16] local (point/spot) shadow view-projs
	int          m_uLocalShadowMap = -1;  // local shadow atlas sampler unit
	int          m_uAO            = -1;   // SSAO occlusion sampler unit
	int          m_uViewport      = -1;   // viewport size (screen-space AO lookup)
	int          m_uSSAOEnabled   = -1;   // 1 = modulate ambient by SSAO
	int          m_uWetness       = -1;   // weather wet-surface response
	int          m_uSnow          = -1;   // weather snow cover
	int          m_uCloudShadowMap = -1;  // cloud-shadow transmittance map sampler unit (19)
	int          m_uCloudShadowA   = -1;  // vec4: region origin XZ, 1/size, mid-plane Y
	int          m_uCloudShadowB   = -1;  // vec4: x = strength (0 = off)
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
	int          m_uSkinnedLocalShadowVP   = -1;
	int          m_uSkinnedLocalShadowMap  = -1;
	int          m_uSkinnedAO              = -1;
	int          m_uSkinnedViewport        = -1;
	int          m_uSkinnedSSAOEnabled     = -1;
	int          m_uSkinnedCloudShadowMap  = -1;
	int          m_uSkinnedCloudShadowA    = -1;
	int          m_uSkinnedCloudShadowB    = -1;

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
	int          m_uInstLocalShadowVP       = -1;
	int          m_uInstLocalShadowMap      = -1;
	int          m_uInstShadowEnabled       = -1;
	int          m_uInstAO                  = -1;
	int          m_uInstViewport            = -1;
	int          m_uInstSSAOEnabled         = -1;
	int          m_uInstWetness             = -1;
	int          m_uInstSnow                = -1;
	int          m_uInstCloudShadowMap      = -1;
	int          m_uInstCloudShadowA        = -1;
	int          m_uInstCloudShadowB        = -1;

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
	// Draws RenderWorld::particleBatches (ParticleGraph particles), one GPU-
	// instanced glDrawArraysInstanced per batch — see GetOrBuildParticleProgram.
	void DrawParticleGraphBatches(const glm::mat4& viewProj, const glm::mat4& view);

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID. A present entry of 0 means "resolved, no texture". Drained/cleared
	// by InvalidateMaterial via m_pendingMaterialInvalidations.
	std::unordered_map<HE::UUID, unsigned int> m_materialTexCache;
	std::unordered_map<std::string, unsigned int> m_graphTexCache;
	std::vector<HE::UUID>                       m_pendingMaterialInvalidations;
	std::vector<HE::UUID>                       m_pendingMeshInvalidations;
	std::vector<HE::UUID>                       m_pendingTexInvalidations;

	// ── Shadow map (cascaded; directional light) ────────────────────────────
	// m_shadowDepthTex is a GL_TEXTURE_2D_ARRAY (one Depth24 layer per cascade);
	// the shadow pass renders each cascade into its layer and the scene shader
	// samples the array with planar view-Z cascade selection. Mirrors the Metal
	// backend's CSM (D3D/Vulkan still use the legacy single map).
	unsigned int m_shadowFBO      = 0;
	unsigned int m_shadowDepthTex = 0;   // GL_TEXTURE_2D_ARRAY, Depth24, one layer/cascade
	int          m_shadowSize     = HE::kShadowMapResolution;
	// Local (point/spot) shadow atlas: 2D depth ARRAY, one layer per spot view /
	// point cube face (16 layers, see ShadowData::kMaxLocalShadowLayers).
	unsigned int m_localShadowDepthTex = 0; // GL_TEXTURE_2D_ARRAY, Depth24
	int          m_localShadowSize     = 1024;
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
	int          m_uSkyNebulaCover = -1;  // space-nebula sky coverage
	int          m_uSkyAuroraColor = -1;  // aurora base colour
	int          m_uSkyAuroraColorTop = -1; // aurora upper colour
	int          m_uSkyAuroraHeight   = -1; // aurora band elevation
	int          m_uSkyAuroraFragment = -1; // aurora streak fragmentation
	int          m_uSkyWind        = -1;  // cloud drift vector
	int          m_uSkyNoise       = -1;  // 3D value-noise sampler
	int          m_uSkyCloudShadowPass   = -1; // 1 = render the cloud-shadow map (transmittance only)
	int          m_uSkyCloudShadowRegion = -1; // vec4: origin XZ, region size, map size px
	int          m_uSkyFlash       = -1;  // lightning flash brightness
	int          m_uSkyCloudMode   = -1;  // 0 = sky-dome clouds, 1 = 3D volumetric
	int          m_uSkyCloudQuality = -1; // cloud raymarch quality: 0 Low, 1 Med, 2 High
	int          m_uSkyCloudStyle       = -1; // 3D clouds: 0 Classic, 1 Realistic
	int          m_uSkyCloudInterShadows = -1; // 1 = towers darken clouds behind them
	int          m_uSkyCloudEvolution   = -1; // shape-evolution speed (0 frozen … 2 time-lapse)
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
	int          m_uLensFlare     = -1;
	unsigned int m_fsVAO          = 0;  // empty VAO for the fullscreen triangle
	void CreateTonemapPipeline();
	void EnsureHDRTarget(int width, int height);
	void DestroyHDRTarget();

	// ── Deferred render path (G-buffer + fullscreen lighting resolve) ────────
	// docs/deferred-renderer-plan.md. MRT FBO: GB0 = SRGB8_ALPHA8 (BaseColor +
	// Metallic, written with GL_FRAMEBUFFER_SRGB enabled), GB1/GB2 = RGBA16F
	// (oct Normal/Roughness/Specular, HDR Emissive/Material-AO), depth as a
	// TEXTURE (sampled by the resolve; blitted into m_hdrDepth for the forward
	// tail). Pipelines are built lazily on the first deferred frame; a build
	// failure logs once and the renderer stays forward.
	unsigned int m_gbFBO      = 0;
	unsigned int m_gbColor0   = 0, m_gbColor1 = 0, m_gbColor2 = 0;
	unsigned int m_gbDepthTex = 0;
	int          m_gbW = 0, m_gbH = 0;
	unsigned int m_gbufferProgram          = 0; // built-in PBR → G-buffer (kUnlitVS + kGBufFS)
	unsigned int m_gbufferInstancedProgram = 0; // instanced variant (kInstancedVS + kGBufFS)
	unsigned int m_deferredResolveProgram  = 0; // fullscreen heLitP resolve (shared preamble)
	unsigned int m_resolveUBO      = 0;         // HeResolve block (binding 3)
	unsigned int m_resolveLightUBO = 0;         // resolve-only HeLighting fill (incl. CSM matrices)
	bool         m_deferredPipelinesTried = false;
	int          m_gbufferDebugView       = 0;  // HE_DUMP_GBUFFER (1..4)
	// Built-in G-buffer program uniform locations (same names as the unlit set).
	int m_uGBMVP = -1, m_uGBModel = -1, m_uGBColor = -1, m_uGBMetallic = -1,
	    m_uGBRoughness = -1, m_uGBHasTexture = -1, m_uGBTexture = -1;
	int m_uGBInstViewProj = -1, m_uGBInstColor = -1, m_uGBInstMetallic = -1,
	    m_uGBInstRoughness = -1, m_uGBInstHasTexture = -1, m_uGBInstTexture = -1;
	void EnsureGBufferTargets(int width, int height);
	void DestroyGBufferTargets();
	bool EnsureDeferredPipelines(); // true when the G-buffer + resolve programs exist

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
	int          m_uUIUVRect     = -1;  // glyph quads: atlas UV rect
	int          m_uUIMode       = -1;  // 0 = solid color, 1 = font-atlas glyph
	int          m_uUICornerRadius = -1; // px; min(w,h)/2 → circle (rounded rects)
	unsigned int m_uiFontTexture = 0;   // R8 UI font atlas (HE::sharedUIFont), lazy
	// Imported Font asset atlases, uploaded lazily on first sight (key → R8 tex).
	std::unordered_map<uint32_t, unsigned int> m_uiFontAtlases;
	unsigned int UIFontAtlasTexture(uint32_t key); // key 0 → the shared atlas
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
	// a=T) at quarter res; the sky pass upsamples + composites it. Runs with the CURRENT
	// camera (this backend draws the sky after extraction), so there is no 1-frame lag.
	unsigned int m_cloudFBO = 0;
	unsigned int m_cloudTex = 0;               // RGBA16F, quarter-res (L, T)
	int          m_cloudW   = 0;
	int          m_cloudH   = 0;
	int          m_uSkyCloudTex     = -1;
	int          m_uSkyLowResClouds = -1;
	int          m_uSkyCloudPrepass = -1;
	int          m_uSkyRainAmount   = -1;
	int          m_uSkyGodRays      = -1;
	int          m_uSkyShootingStars = -1;
	void         EnsureCloudTarget(int width, int height);
	void         DestroyCloudTarget();
	// ── Cloud shadows (EnvironmentSettings.cloudShadows) ────────────────────
	// One 512² R8 pass per frame (m_skyProgram with uCloudShadowPass=1) renders
	// the cloud slab's sun transmittance over a world-space XZ region around
	// the camera; the lit shaders + heLitP sample it on unit 19 and darken the
	// directional light. m_cloudShadowParams* carry the region/strength the
	// pass computed this frame (strength 0 = pass skipped).
	unsigned int m_cloudShadowFBO = 0;
	unsigned int m_cloudShadowTex = 0;         // R8, kCloudShadowMapSize²
	glm::vec4    m_cloudShadowParamsA = glm::vec4(0.0f); // xy origin, z 1/size, w mid-plane Y
	glm::vec4    m_cloudShadowParamsB = glm::vec4(0.0f); // x strength (0 = off this frame)
	void         EnsureCloudShadowTarget();
	void         DestroyCloudShadowTarget();
	void         RenderCloudShadowMap();
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
	// Deferred P5: view-pos reconstruction from the G-buffer depth (fullscreen).
	unsigned int m_ssaoDepthPosProgram = 0;
	int          m_uDepthPosDepth   = -1;
	int          m_uDepthPosInvProj = -1;
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
	unsigned int m_blackTex       = 0;   // 1×1 transparent black (bound as the reflection result when disabled)
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
	// fromGBufferDepth (deferred, plan P5): reconstruct the view-space positions
	// from m_gbDepthTex in one fullscreen draw instead of the geometry pre-pass.
	unsigned int RenderSSAO(const CommandBuffer& cmds, int pw, int ph,
	                        const glm::mat4& viewProj, const glm::mat4& view,
	                        const glm::mat4& proj, bool fromGBufferDepth = false);

	// ── Global Illumination (GL 4.3+ compute port, Windows/Linux — blind) ──────
	// Software counterpart of the Metal ray-traced DDGI path: CPU-built BVH
	// (HE::GiBvh, unit-tested BLAS per mesh) concatenated into shared SSBOs +
	// a flat per-frame instance array (the TLAS analogue; kernels transform the
	// ray by invTransform and traverse the referenced BLAS range). Gated on a
	// GL 4.3 context (GLAD_GL_VERSION_4_3) — macOS GL is 4.1 and never enters.
	// Checkpoint GL-A: capability + settings + accel upload only, nothing
	// samples these buffers yet (GI-off rendering stays byte-identical).
	struct GIBlasRange
	{
		int32_t nodeOffset = 0, nodeCount = 0;
		int32_t triOffset  = 0, triCount  = 0;
		bool    valid      = false;
	};
	// Matches the std430 GiInst block the kernels declare (kGiTraversalGLSL):
	// mat4 + two shading rows + BLAS offsets, 112 bytes, 16-byte aligned.
	// The shading pair is HE::giInstanceSurface's output — the probe kernel uses
	// only baseColor.rgb, the reflection kernel all four fields.
	struct GIInstanceGpu
	{
		glm::mat4 invTransform{1.0f};   // world → object (rays enter BLAS space)
		glm::vec4 baseColor{1.0f};      // rgb = flat albedo (probe bounce tint), a = metallic
		glm::vec4 emissive{0.0f};       // rgb = emissive (reflections only), a = roughness
		// landIndex → m_giLandSSBO: a terrain chunk's hit is coloured from the
		// PAINT at the hit point, not from this flat baseColor (GiLandscape.h).
		int32_t   nodeOffset = 0, triOffset = 0, landIndex = -1, pad1 = 0;
	};
	// GPU mirror of HE::GiLandscape — must match the kernel's GiLand (std430).
	struct GILandGpu
	{
		glm::mat4 worldToLocal{1.0f};
		glm::vec4 cfg{0.0f};      // xy = 1/(sizeX,sizeZ), z = uvTiling, w = layer count
		glm::vec4 layer[4]{};     // per-layer folded colour (rgb)
	};
	static_assert(sizeof(GILandGpu) == 64 + 5 * 16, "must match the GLSL GiLand layout");
	// How far, in SCREEN pixels, the widest allowed lobe scatters — the span the
	// blur must cover for the rays not to show as noise. Same constant and same
	// meaning as MetalRenderer::kGIReflLobeScreenPx; keep them together.
	static constexpr float kGIReflLobeScreenPx = 24.0f;
	bool         m_giReflBlurEnabled = true; // IRenderer::GIReflectionSettings::blur
	unsigned int m_giReflMixProgram = 0; // sharp/blurred roughness lerp (kGiReflMixFS)
	unsigned int m_giLandSSBO = 0;
	int          m_giLandCount = 0;
	std::vector<unsigned int> m_giLandWeightTex; // weightmap per landscape, same order
	GIBlasRange  BuildGIBlas(const HE::UUID& meshId); // CPU build from ContentManager data
	void         UpdateGIAccel();                     // lazy BLAS append + per-frame instance upload
	void         DestroyGIAccel();

	// GL-B/GL-C: shadow-ray + probe passes. Pipelines are built lazily on the
	// first GI-active frame (4.3 compute programs — never compiled on 4.1).
	void         CreateGIPipelines();
	void         EnsureGIShadowTargets(int width, int height);
	void         DestroyGIShadowTargets();
	void         EnsureGIProbeGrid();   // one-shot grid fit over the scene AABB
	void         EnsureGIProbeAtlas();
	void         DestroyGIProbeAtlas();
	// Half-res world-space G-buffer (position + normal + roughness/metallic).
	// Shared by the shadow-ray and reflection kernels — either can be the only
	// consumer, so it is not folded into RenderGIShadow. Uses THIS frame's
	// extraction (Metal lesson: mask and scene pass must share one camera).
	bool         RenderGIPrepass(const CommandBuffer& cmds, int width, int height,
	                             const glm::mat4& viewProj);
	// Compute shadow rays → temporal → blur on the pre-pass targets. Returns the
	// blurred mask texture (0 if unavailable).
	unsigned int RenderGIShadow(int width, int height, const glm::mat4& viewProj);
	// Specular trace → (temporal) → (blur) on the pre-pass targets. Returns the
	// texture the shading pass samples (rgb radiance, a confidence), 0 = the
	// pass could not run. probesValid = the DDGI atlases hold real data.
	unsigned int RenderGIReflections(int width, int height, const glm::mat4& viewProj,
	                                 bool probesValid);
	void         DispatchGIProbeUpdate();

	static constexpr float kGIProbeSpacing     = 4.0f; // metres between probes
	static constexpr int   kGIMaxProbesPerAxis = 10;   // grid clamp (matches Metal)
	static constexpr int   kGIProbeOctSize     = 8;    // octahedral tile size

	bool         m_giPipelinesBuilt   = false;
	unsigned int m_giGBufProgram      = 0;
	unsigned int m_giShadowCSProgram  = 0;
	unsigned int m_giTemporalProgram  = 0;
	unsigned int m_giBlurProgram      = 0;
	unsigned int m_giProbeCSProgram   = 0;
	unsigned int m_giReflCSProgram       = 0; // specular trace (GLSL 430 compute)
	unsigned int m_giReflTemporalProgram = 0; // MRT: radiance+confidence / receiver pos
	unsigned int m_giReflBlurProgram     = 0; // separable, confidence-weighted
	// G-buffer + mask targets (all half-res).
	unsigned int m_giGBufFBO   = 0, m_giGBufPosTex = 0, m_giGBufNormTex = 0, m_giGBufDepth = 0;
	unsigned int m_giGBufMatTex = 0;                      // rgba16f, r = roughness, g = metallic (reflection kernel only)
	unsigned int m_giRawTex    = 0;                       // r16f, compute image store
	unsigned int m_giLocalMaskTex = 0;                    // rgba16f, per-pixel local-light visibility (1 channel per light, first 4)
	unsigned int m_giHistFBO[2] = { 0, 0 }, m_giHistTex[2] = { 0, 0 }; // RGBA16F ping-pong
	unsigned int m_giResultFBO = 0, m_giResultTex = 0;    // r16f, sampled by the scene
	// Reflection chain: raw compute output → optional temporal ping-pong →
	// optional separable blur ending in m_giReflTex. All rgba16f half-res
	// (rgb = radiance arriving along the mirror ray, a = confidence).
	unsigned int m_giReflRawTex = 0;
	unsigned int m_giReflFBO = 0, m_giReflTex = 0;
	unsigned int m_giReflBlurFBO = 0, m_giReflBlurTex = 0;
	unsigned int m_giReflHistFBO[2]    = { 0, 0 };
	unsigned int m_giReflHistTex[2]    = { 0, 0 };
	unsigned int m_giReflHistPosTex[2] = { 0, 0 }; // receiver world pos (disocclusion reject)
	int          m_giShadowW = 0, m_giShadowH = 0;
	int          m_giHistIdx = 0;
	bool         m_giHistValid = false;
	glm::mat4    m_giPrevViewProj{1.0f};
	float        m_giFrameSeed = 0.0f;
	int          m_giReflHistIdx = 0;
	bool         m_giReflHistValid = false;
	glm::mat4    m_giReflPrevViewProj{1.0f};
	float        m_giReflFrameSeed = 0.0f;
	// Probe grid + atlases.
	glm::vec3    m_giGridOrigin{0.0f};
	glm::ivec3   m_giGridCounts{0};
	int          m_giProbeCount = 0, m_giProbesPerRow = 0, m_giProbeCursor = 0;
	bool         m_giProbeGridBuilt = false;
	unsigned int m_giIrrAtlas = 0, m_giVisAtlas = 0;
	// Per-program GI uniform locations for the three programs sharing kUnlitFS.
	struct GISceneLocs
	{
		int enabled = -1, shadowTex = -1, irrTex = -1, visTex = -1, localTex = -1;
		int gridOrigin = -1, gridCounts = -1, intensity = -1;
		int reflTex = -1, reflParams = -1;   // ray-traced reflections (own toggle)
	};
	GISceneLocs  m_giLocsUnlit, m_giLocsSkinned, m_giLocsInstanced;
	GISceneLocs  FetchGISceneLocs(unsigned int program) const;
	void         PushGISceneUniforms(const GISceneLocs& locs, bool active, bool reflActive);
	std::unordered_map<HE::UUID, GIBlasRange> m_giBlasCache;
	std::vector<HE::GiBvhNode>     m_giNodesCpu;      // concatenated BLAS nodes (all meshes)
	std::vector<HE::GiBvhTriangle> m_giTrisCpu;       // concatenated BLAS triangles
	std::vector<GIInstanceGpu>     m_giInstancesCpu;  // rebuilt per frame
	bool         m_giBlasDirty       = false;         // node/tri SSBOs need re-upload
	unsigned int m_giNodeSSBO        = 0;
	unsigned int m_giTriSSBO         = 0;
	unsigned int m_giInstanceSSBO    = 0;
	int          m_giInstanceCount   = 0;
	bool         m_giSupported       = false;         // GL >= 4.3, cached at Initialize
	bool         m_giEnabled         = false;
	float        m_giIndirectIntensity = 1.0f;
	float        m_giLightRadius       = 0.5f;        // degrees, shadow-ray cone
	int          m_giRaysPerProbe        = 128;
	int          m_giProbeBudgetPerFrame = 256;
	// ── Ray-traced GI reflections (docs/gi-reflections-plan.md §10) ──────────
	// Independent of m_giEnabled: the pass needs the acceleration structures and
	// the half-res pre-pass, not the diffuse probe field (which it uses when it
	// is there and falls back to sun + ambient floor when it is not).
	bool         m_giReflEnabled      = false;
	float        m_giReflIntensity    = 1.0f;   // 0…1 mix against the sky cubemap
	float        m_giReflMaxRoughness = 0.6f;   // above this the sky term stands alone
	float        m_giReflMaxDistance  = 200.0f; // world-space ray length
	int          m_giReflQuality      = 1;      // 0 raw / 1 + blur / 2 + cone jitter + temporal

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
