#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

// ─── The world-scale landscape on OpenGL (Thema 326, step 2, part B) ─────────
// Draws a HE::LandscapeRenderFrame: the patches of the quadtree, each one the shared 65 x 65 grid whose vertex shader fetches its heights
// from the tile atlas (HorizonRendering/LandscapeShaderSource.h, GLSL 4.10 as written). The class owns its program, grid buffers and
// the atlas images, and nothing else of the renderer: OpenGLRenderer calls draw() in the opaque pass, and the capture tool calls it on
// a context of its own. A GL 4.1 core context must be current and glad loaded.
//
// Images (units kFirstUnit .. kFirstUnit + 9, above everything the renderer uses): tile heights R16UI array, slot table R16UI,
// coarse heights R16UI, three RGBA8 weight arrays, three RGBA8 coarse weight images, and (borrowed from the renderer) the albedo array of the world-UV ground. Integer formats are read with texelFetch, so there is no filtering to set up and
// no sampler state to leak.
//
// What it does NOT take part in: shadows, SSAO, GI, fog, the material graph. It writes lit colour (flat layer colours, sun and ambient)
// into whatever target is bound, with depth, so the sky fills what the landscape leaves.
class OpenGLLandscape
{
public:
	static constexpr int kFirstUnit = 21;

	struct Shading
	{
		glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };   // towards the sun
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
		glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
		unsigned int normalArray = 0, maskArray = 0;   // the same group's normal and mask arrays (0 = not used)
		unsigned int albedoArray = 0;   // GL texture array of the world-UV ground (LandscapeRenderStatic::albedoArrayPath, resolved by the renderer); 0 = flat colours
	};
	struct Stats
	{
		uint64_t uploads = 0;        // tiles copied into the atlas
		uint64_t tableUpdates = 0;
		uint64_t rejected = 0;       // uploads or updates that did not fit the images
		uint64_t patches = 0;        // drawn in the last draw()
		uint64_t drawCalls = 0;
		uint64_t generations = 0;    // times the images were made
	};

	OpenGLLandscape();
	~OpenGLLandscape();
	OpenGLLandscape(const OpenGLLandscape&) = delete;
	OpenGLLandscape& operator=(const OpenGLLandscape&) = delete;

	// Compiles the program and makes the grid. False with `error` set when the context cannot do it (the landscape is then skipped).
	bool init(std::string* error = nullptr);
	void shutdown();
	bool ready() const;

	// Executes the frame's uploads and table updates, then draws its patches with `viewProj` (the matrix the frame was built for).
	// GL state it touches (program, VAO, active texture, depth state, cull, blend, unpack alignment) is put back.
	void draw(const HE::LandscapeRenderFrame& frame, const glm::mat4& viewProj, const Shading& shading);

	const Stats& stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
