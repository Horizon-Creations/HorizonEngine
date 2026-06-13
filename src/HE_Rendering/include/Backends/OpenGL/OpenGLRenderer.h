#pragma once
#include <Renderer/IRenderer.h>
#include "OpenGLShaderManager.h"
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
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
	int          m_uLightCount    = -1;
	int          m_uLightPos      = -1;
	int          m_uLightDir      = -1;
	int          m_uLightColor    = -1;
	int          m_uLightParams   = -1;
	int          m_uCameraPos     = -1;
	unsigned int m_cubeVAO        = 0;
	unsigned int m_cubeVBO        = 0;
	unsigned int m_cubeEBO        = 0;
	int          m_cubeIndexCount = 0;

	// Uploaded asset meshes, keyed by asset UUID
	std::unordered_map<HE::UUID, GpuMesh> m_meshCache;

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
