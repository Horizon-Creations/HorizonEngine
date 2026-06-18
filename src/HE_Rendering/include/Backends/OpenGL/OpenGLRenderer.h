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

private:
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
	int          m_uLightVP       = -1;   // directional-light view-proj (shadow)
	int          m_uShadowMap     = -1;   // shadow map sampler unit
	int          m_uShadowEnabled = -1;
	int          m_uAO            = -1;   // SSAO occlusion sampler unit
	int          m_uViewport      = -1;   // viewport size (screen-space AO lookup)
	int          m_uSSAOEnabled   = -1;   // 1 = modulate ambient by SSAO
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
	int          m_uSkinnedLightVP         = -1;
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
	int          m_uInstLightVP             = -1;
	int          m_uInstShadowMap           = -1;
	int          m_uInstShadowEnabled       = -1;
	int          m_uInstAO                  = -1;
	int          m_uInstViewport            = -1;
	int          m_uInstSSAOEnabled         = -1;

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID. A present entry of 0 means "resolved, no texture". Drained/cleared
	// by InvalidateMaterial via m_pendingMaterialInvalidations.
	std::unordered_map<HE::UUID, unsigned int> m_materialTexCache;
	std::vector<HE::UUID>                       m_pendingMaterialInvalidations;
	std::vector<HE::UUID>                       m_pendingMeshInvalidations;

	// ── Shadow map (single directional light) ───────────────────────────────
	unsigned int m_shadowFBO      = 0;
	unsigned int m_shadowDepthTex = 0;
	int          m_shadowSize     = 2048;
	unsigned int m_depthProgram   = 0;   // depth-only pass (lightVP * model * pos)
	int          m_uDepthMVP      = -1;
	void CreateShadowResources();

	// ── Procedural skybox (drawn into the HDR target behind the scene) ───────
	unsigned int m_skyProgram     = 0;
	int          m_uSkyInvVP      = -1;
	int          m_uSkySunDir     = -1;
	int          m_uSkyMoonTex    = -1;   // moon texture sampler unit
	int          m_uSkyHasMoon    = -1;   // 1 when a moon texture is bound
	int          m_uSkyTime       = -1;   // time of day (cloud scroll phase)
	int          m_uSkyCoverage   = -1;   // cloud amount (0 clear … 1 overcast)
	int          m_uSkyClock      = -1;   // wall-clock seconds (star twinkle)
	int          m_uSkySunColor   = -1;   // sun light colour (cloud tint)
	int          m_uSkyAurora     = -1;   // aurora intensity (0 = off)
	int          m_uSkyMilkyWay    = -1;  // milky-way (dense star band) intensity
	int          m_uSkyNebula      = -1;  // space-nebula intensity
	int          m_uSkyNebulaColor = -1;  // space-nebula base colour
	int          m_uSkyAuroraColor = -1;  // aurora base colour
	int          m_uSkyWind        = -1;  // cloud drift vector
	int          m_uSkyNoise       = -1;  // 3D value-noise sampler
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
