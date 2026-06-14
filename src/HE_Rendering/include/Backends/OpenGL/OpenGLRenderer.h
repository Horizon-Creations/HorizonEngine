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

	void  SetViewportSize(uint32_t width, uint32_t height) override;
	void* GetViewportTexture() override;
	bool  CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
	void  InvalidateMaterial(const HE::UUID& materialId) override;
	void  SetBloomSettings(const BloomSettings& settings) override;

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

	void CreateUnlitPipeline();
	void CreateCubeMesh();
	void DrawScene(int width, int height);
	// (Re)creates the offscreen viewport FBO at the requested size.
	void EnsureViewportTarget();
	void DestroyViewportTarget();
	// Returns the GPU mesh for the asset, uploading it on first use.
	// nullptr when the UUID is invalid or the asset is not loaded.
	const GpuMesh* ResolveMesh(const HE::UUID& assetId);

	// Resolves the base-color texture of an explicit MaterialComponent override,
	// uploading it on first sight and caching by material UUID. Returns true if
	// the material was found (outTex may still be 0 = material has no texture);
	// false when the UUID is null or the material is not loaded yet.
	bool ResolveMaterialTexture(const HE::UUID& materialId, unsigned int& outTex);

	// Resolves a material override's PBR scalars (baseColor/metallic/roughness).
	// Returns true if the material is loaded; leaves the outputs untouched
	// otherwise (caller keeps its defaults).
	bool ResolveMaterialParams(const HE::UUID& materialId,
	                           glm::vec3& outBaseColor, float& outMetallic, float& outRoughness);

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
	std::vector<bool>     m_visible;       // per-frame culling results
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
	int          m_uLightCount    = -1;
	int          m_uLightPos      = -1;
	int          m_uLightDir      = -1;
	int          m_uLightColor    = -1;
	int          m_uLightParams   = -1;
	int          m_uCameraPos     = -1;
	int          m_uLightVP       = -1;   // directional-light view-proj (shadow)
	int          m_uShadowMap     = -1;   // shadow map sampler unit
	int          m_uShadowEnabled = -1;
	unsigned int m_cubeVAO        = 0;
	unsigned int m_cubeVBO        = 0;
	unsigned int m_cubeEBO        = 0;
	int          m_cubeIndexCount = 0;

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;

	// Base-color textures for MaterialComponent overrides, keyed by material
	// UUID. A present entry of 0 means "resolved, no texture". Drained/cleared
	// by InvalidateMaterial via m_pendingMaterialInvalidations.
	std::unordered_map<HE::UUID, unsigned int> m_materialTexCache;
	std::vector<HE::UUID>                       m_pendingMaterialInvalidations;

	// ── Shadow map (single directional light) ───────────────────────────────
	unsigned int m_shadowFBO      = 0;
	unsigned int m_shadowDepthTex = 0;
	int          m_shadowSize     = 2048;
	unsigned int m_depthProgram   = 0;   // depth-only pass (lightVP * model * pos)
	int          m_uDepthMVP      = -1;
	void CreateShadowResources();

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
