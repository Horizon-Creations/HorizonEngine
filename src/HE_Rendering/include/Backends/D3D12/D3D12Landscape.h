#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// ─── The world-scale landscape on D3D12 (Thema 326, step 2, part B) ──────────
// The same job as D3D11Landscape (one HLSL source, kLandscapeHLSL in HorizonRendering/LandscapeShaderSource.h): copies a
// HE::LandscapeRenderFrame's uploads into the tile atlas and draws its patches with the shared 65 x 65 grid. The class owns its root signature
// (b12 root CBV with the frame's constants, b13 twelve root constants with the patch, a table of nine SRVs t32-t40), its pipeline states (one
// per colour format), a descriptor heap of its own, the atlas images and the buffers that feed them. D3D12Renderer calls upload() once per
// scene frame, as soon as it has the frame, and draw() in the opaque pass; the capture tool (landscape_gpu_check) calls both on a device of
// its own.
//
// Everything is recorded into the command list the caller hands in. The copies go through upload buffers of a small ring (the frame's own
// bytes are written when upload() runs, the GPU reads them when the list executes); the ring is deep enough for the renderer's frames in flight, and
// when a buffer or an image has to be replaced (a landscape began, the ring has to grow) the caller's `waitIdle` empties the queue first.
//
// What it does NOT take part in: shadows, SSAO, GI, fog, the material graph. It writes lit colour (flat layer colours, sun and ambient)
// into whatever target is bound, with depth, so the sky fills what the landscape leaves.
class D3D12Landscape
{
public:
	struct Shading
	{
		glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };   // towards the sun
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
		glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
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

	D3D12Landscape();
	~D3D12Landscape();
	D3D12Landscape(const D3D12Landscape&) = delete;
	D3D12Landscape& operator=(const D3D12Landscape&) = delete;

	// Compiles the two shaders and makes the root signature and descriptor heap. `waitIdle` blocks until the GPU has finished every command list
	// submitted so far (the renderer's waitForAllFrames); it is called when images or upload buffers are replaced. False with `error` set when
	// the device cannot do it (the landscape is then skipped).
	bool init(ID3D12Device* device, std::function<void()> waitIdle, std::string* error = nullptr);
	void shutdown();
	bool ready() const;

	// Creates the images when a landscape began, then records the frame's uploads and table updates into `cl` and CLEARS them from `frame`:
	// every frame the application built has to be executed once, the runtime counts its uploads as done. Draws nothing. The images leave in
	// the shader-resource state they came in; the barriers around the copies are recorded here.
	void upload(ID3D12GraphicsCommandList* cl, HE::LandscapeRenderFrame& frame);

	// Records the draws of the frame's patches with `viewProj` (the matrix the frame was built for) into the bound render target and depth
	// buffer (colour format `colorFormat`, a DXGI_FORMAT value; depth D32_FLOAT), depth test LESS with writes, no culling, no blending.
	// Sets its own root signature, pipeline, descriptor heap, topology and index buffer: the caller rebuilds its own state afterwards.
	void draw(ID3D12GraphicsCommandList* cl, const HE::LandscapeRenderFrame& frame, const glm::mat4& viewProj, const Shading& shading,
	          unsigned colorFormat);

	const Stats& stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
