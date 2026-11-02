#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The two halves of landscape_gpu_check: one backend renders a LandscapeRenderFrame offscreen (colour + depth), the checker (main.cpp)
// compares the result with the geometry it knows. A backend keeps its images between render() calls, like the renderer does.
namespace gpucheck
{
// An albedo texture array for the world-UV ground: `layers` slices of size x size RGBA8, no mips (the same on every backend).
struct TexArray
{
	int size = 0, layers = 0;
	std::vector<uint8_t> rgba;
};

struct RenderIn
{
	int       width = 0, height = 0;
	glm::mat4 viewProj{ 1.0f };           // origin-relative world to the backend's clip space
	glm::dvec3 origin{ 0.0 };
	bool      depthIsNdc = false;         // the depth buffer holds the GL ndc z (-1..1) rather than window depth 0..1 (the Metal scene pass's convention)
	glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };
	glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
	glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
	glm::vec4 clear{ 0.5f, 0.7f, 1.0f, 1.0f };
	const TexArray* albedo = nullptr;     // given: the frame's world-UV ground uses it (when the frame names an array); null: flat colours
	HE::LandscapeProvider provider;
};

struct RenderOut
{
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;            // row 0 = top
	std::vector<float>   depth;           // row 0 = top; window depth 0..1 (1 = nothing drawn)
	uint64_t uploads = 0;                 // tiles the backend copied into its atlas, cumulative
	uint64_t patches = 0;                 // drawn in this frame
	uint64_t drawCalls = 0;
	uint64_t rejected = 0;
	std::string error;
};

class Backend
{
public:
	virtual ~Backend() = default;
	virtual const char* name() const = 0;
	// Metal's scene pass rasterises with the plain GL view-projection (HE::kMetalClipFix is NOT applied), so its depth buffer holds the GL ndc z;
	// landscape patches have to follow, or they would not sort with the scene's objects and the sky.
	virtual bool depthIsNdc() const { return false; }
	virtual bool init(std::string& error) = 0;
	virtual bool render(const RenderIn& in, RenderOut& out) = 0;
};

// Writes an RGBA8 PNG (main.cpp holds the one stb implementation).
void writePngFile(const std::string& path, int width, int height, const uint8_t* rgba);

std::unique_ptr<Backend> makeGlBackend();
#ifdef HE_CHECK_METAL
std::unique_ptr<Backend> makeMetalBackend();
#endif
} // namespace gpucheck
