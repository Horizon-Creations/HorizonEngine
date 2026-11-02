#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

// ─── The world-scale landscape on Metal (Thema 326, step 2, part B) ──────────
// The same job as OpenGLLandscape: executes a HE::LandscapeRenderFrame's uploads and draws its patches with the shared 65 x 65 grid, the
// vertex shader fetching heights from the tile atlas (HorizonRendering/LandscapeShaderSource.h, kLandscapeMSL). The class owns its
// pipeline, depth state, index buffer and the atlas textures; MetalRenderer calls encode() inside its scene pass and the capture tool
// (landscape_gpu_check) calls it in a pass of its own.
//
// Header in plain C++ (Objective-C types are void*), so it can be included from any translation unit.
//
// Uploads are written into the textures from the CPU (replaceRegion) when encode() runs, not through the command buffer: a frame still
// executing on the GPU may read a slot a tile of this frame has just taken over. That slot belonged to a tile that left the loaded disc,
// which no fine patch of the current view shows, so at worst one in-flight frame of a far tile shows the wrong height; ring-buffered
// staging copies would close it and are not here.
class MetalLandscape
{
public:
	struct Shading
	{
		glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
		glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
		void* normalArray = nullptr, *maskArray = nullptr;   // the same group's normal and mask arrays (nullptr = not used)
		void* albedoArray = nullptr;   // id<MTLTexture> (2D array) of the world-UV ground, resolved by the renderer; nullptr = flat colours
	};
	struct Stats
	{
		uint64_t uploads = 0;
		uint64_t tableUpdates = 0;
		uint64_t rejected = 0;
		uint64_t patches = 0;       // in the last encode()
		uint64_t drawCalls = 0;
		uint64_t generations = 0;
	};

	MetalLandscape();
	~MetalLandscape();
	MetalLandscape(const MetalLandscape&) = delete;
	MetalLandscape& operator=(const MetalLandscape&) = delete;

	// `device`: an id<MTLDevice>. The pipeline is made for one colour format and one depth format (the target of the scene pass).
	// `colorFormat` / `depthFormat`: MTLPixelFormat values.
	bool init(void* device, unsigned long colorFormat, unsigned long depthFormat, std::string* error = nullptr);
	void shutdown();
	bool ready() const;

	// Executes the frame's uploads and table updates, then encodes its patches into `renderEncoder` (an id<MTLRenderCommandEncoder>)
	// with `viewProj`: the plain GL-convention view-projection (origin-relative world to clip, depth -1..1), the one MetalRenderer's scene pass
	// rasterises with (kMetalClipFix is NOT applied there; its depth buffer holds the GL ndc z, which the landscape then shares with the
	// scene's objects and the sky; geometry nearer than about twice the near plane is clipped, as it is for every scene draw). Sets its own pipeline,
	// depth state and bindings (buffers 20-21, textures 20-26, above everything MetalRenderer uses); the caller sets its own pipeline and
	// depth state before its next draw, as every pass of MetalRenderer already does.
	void encode(void* renderEncoder, const HE::LandscapeRenderFrame& frame, const glm::mat4& viewProj, const Shading& shading);

	const Stats& stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
