#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

// ─── The world-scale landscape on D3D11 (Thema 326, step 2, part B) ──────────
// The same job as OpenGLLandscape and MetalLandscape: copies a HE::LandscapeRenderFrame's uploads into the tile atlas and draws its patches
// with the shared 65 x 65 grid, the vertex shader fetching heights from the atlas (HorizonRendering/LandscapeShaderSource.h, kLandscapeHLSL, one
// source for D3D11 and D3D12). The class owns its two shaders, states, index buffer, constant buffers and the atlas images, and nothing else
// of the renderer. D3D11Renderer calls upload() once per scene frame (as soon as it has the frame, whatever else the frame does) and draw()
// in the opaque pass; the capture tool (landscape_gpu_check) calls both on a device of its own.
//
// Images (SRV registers t32 .. t41, above everything the renderer and its graph materials bind): tile heights R16_UINT array, slot table
// R16_UINT, coarse heights R16_UINT, three RGBA8 weight arrays, three RGBA8 coarse weight images, all read with Load(), and the borrowed albedo
// array (t41, sampled through a sampler state of this class at s13). The grid has no vertex buffer (SV_VertexID is the index value), only a
// 16-bit index buffer.
//
// What it does NOT take part in: shadows, SSAO, GI, fog, the material graph. It writes lit colour (flat layer colours, sun and ambient)
// into whatever target is bound, with depth, so the sky fills what the landscape leaves.
class D3D11Landscape
{
public:
	struct Shading
	{
		glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };   // towards the sun
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
		glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
		// The texture group's albedo array of the world-UV ground (LandscapeRenderStatic::albedoArrayPath, resolved by the renderer; a Texture2DArray
		// view with its mips, not owned here). Null = flat layer colours.
		ID3D11ShaderResourceView* albedoArray = nullptr;
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

	D3D11Landscape();
	~D3D11Landscape();
	D3D11Landscape(const D3D11Landscape&) = delete;
	D3D11Landscape& operator=(const D3D11Landscape&) = delete;

	// Compiles the two shaders and makes the states. False with `error` set when the device cannot do it (the landscape is then skipped).
	bool init(ID3D11Device* device, std::string* error = nullptr);
	void shutdown();
	bool ready() const;

	// Creates the images when a landscape began, then executes the frame's uploads and table updates, and CLEARS them from `frame`: every
	// frame the application built has to be executed once, the runtime counts its uploads as done. Draws nothing, touches no pipeline state.
	void upload(ID3D11DeviceContext* ctx, HE::LandscapeRenderFrame& frame);

	// Draws the frame's patches with `viewProj` (the matrix the frame was built for) into the bound render target and depth buffer, depth
	// test LESS with writes, no culling, no blending. Everything it sets on the context (shaders, input assembler, rasteriser, depth and blend
	// state, constant buffers b12-b13, SRVs t32-t41, sampler s13) is put back as it was found.
	void draw(ID3D11DeviceContext* ctx, const HE::LandscapeRenderFrame& frame, const glm::mat4& viewProj, const Shading& shading);

	const Stats& stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
