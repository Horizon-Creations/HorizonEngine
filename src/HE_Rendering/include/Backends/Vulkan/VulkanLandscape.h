#pragma once
#include <Renderer/LandscapeRenderFrame.h>
#include <glm/glm.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// ─── The world-scale landscape on Vulkan (Thema 326, step 2, part B) ─────────
// The same job as OpenGLLandscape, MetalLandscape and D3D11/D3D12Landscape: copies a HE::LandscapeRenderFrame's uploads into the tile atlas and
// draws its patches with the shared 65 x 65 grid, the vertex shader fetching heights from the atlas. The shaders are the GL text behind a Vulkan
// prelude (HorizonRendering/LandscapeShaderSource.h, BuildLandscapeVertexGLSL450 / BuildLandscapeFragmentGLSL450), compiled to SPIR-V at start
// with he::shaderc. The class owns its shaders, pipelines, descriptor set layout, buffers and the atlas images, and nothing else of the renderer;
// what it needs from the renderer it is lent (Env).
//
// The renderer calls upload() once per scene frame, as soon as it has the frame (whatever else the frame does), and draw() inside the scene's
// render pass. upload() records into the renderer's upload batch (a command buffer of its own that is submitted before the frame's, on the same
// queue), never into the frame's command buffer: a render pass is open there and a copy cannot be recorded in it. The barriers in that batch
// order the copies after the earlier frames' reads of the images and before the frame's own, so a slot that was given to another tile is never
// read half-written (unlike Metal's replaceRegion).
//
// Images (set 0, see kLandscapeVkBinding* in LandscapeShaderSource.h): tile heights R16_UINT 2D array, slot table R16_UINT, coarse heights R16_UINT,
// three RGBA8 weight arrays, three RGBA8 coarse weight images, all read with texelFetch through a NEAREST sampler, and the texture group's albedo,
// normal and mask arrays (borrowed from the renderer, sampled linearly with the renderer's repeat/mip sampler). Uniforms: the std140 block
// vkls::Globals (VulkanLandscapeLayout.h) per frame in flight, and 48 bytes of push constants per patch.
//
// What it does NOT take part in: shadows, SSAO, GI, fog, the material graph. It writes lit colour (flat layer colours or the group's textures, sun
// and ambient) into whatever target is bound, with depth, so the sky fills what the landscape leaves.
namespace HE
{
class VulkanLandscape
{
public:
	// What the renderer lends. All of it outlives this object.
	struct Env
	{
		VkDevice         device = VK_NULL_HANDLE;
		uint32_t         framesInFlight = 2;
		uint32_t         maxImageArrayLayers = 256;       // VkPhysicalDeviceLimits::maxImageArrayLayers: a world with more atlas slots is refused
		VkSampler        arraySampler = VK_NULL_HANDLE;   // linear, repeat, mips: for the texture group's arrays
		VkImageView      whiteArray = VK_NULL_HANDLE;     // bound in place of an array the renderer could not resolve (never a null descriptor)
		std::function<uint32_t(uint32_t typeBits, VkMemoryPropertyFlags props)> findMemoryType;
		std::function<VkCommandBuffer()> uploadCmd;       // the open upload batch (null: none could be opened)
		std::function<bool(const void*, VkDeviceSize, VkBuffer&, VkDeviceSize&)> stage;   // copy bytes into the batch's staging memory
		std::function<void()> flush;                      // submit the open upload batch now (called before images that it may name are destroyed)
		std::function<void()> waitIdle;                   // the GPU is done with everything (called when a landscape ends: its images go)
		std::function<VkResult(const VkGraphicsPipelineCreateInfo&, VkPipeline&)> createPipeline;
	};
	struct Shading
	{
		glm::vec3 sunDir{ 0.45f, 0.80f, 0.55f };   // towards the sun
		glm::vec3 sunColor{ 1.0f, 0.95f, 0.85f };
		glm::vec3 ambient{ 0.12f, 0.14f, 0.18f };
		// The texture group's arrays (LandscapeRenderStatic::albedoArrayPath / normalArrayPath / maskArrayPath), resolved by the renderer; a view with its
		// mips, not owned here. Null = not available: flat layer colours (albedo), a flat normal (normal), no occlusion (mask).
		VkImageView albedoArray = VK_NULL_HANDLE;
		VkImageView normalArray = VK_NULL_HANDLE;
		VkImageView maskArray = VK_NULL_HANDLE;
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

	VulkanLandscape();
	~VulkanLandscape();
	VulkanLandscape(const VulkanLandscape&) = delete;
	VulkanLandscape& operator=(const VulkanLandscape&) = delete;

	// ── For a draw the RENDERER records with a material's pipeline (MaterialShaderLibrary::landscapeVertex / landscapeFragment) ──
	// What that material's set binds for the landscape: LsGlobals at 50, the slot table at 51, the tile weight pages at 52-54, the
	// coarse ones at 55-57, the tile heights at 59, the coarse heights at 60, all with `nearest`.
	struct MaterialImages
	{
		VkDescriptorBufferInfo globals{};
		VkImageView heights = VK_NULL_HANDLE, table = VK_NULL_HANDLE, coarseHeights = VK_NULL_HANDLE;
		VkImageView weights[3] = {}, coarseWeights[3] = {};
		VkSampler   nearest = VK_NULL_HANDLE;
	};
	// Writes this frame's LsGlobals for `viewProj` and fills `out`. False: nothing to draw with (no images for this frame's landscape).
	// One LsGlobals a frame in flight: after this, draw() of the same frame would overwrite it.
	bool materialImages(uint32_t frameIndex, const HE::LandscapeRenderFrame& frame, const glm::mat4& viewProj, MaterialImages& out);
	// The frame's patches with whatever pipeline and set the caller has bound. `layout` is that pipeline's, with a vertex-stage
	// push-constant range of 48 bytes at 0 (LsPatch). Binds the index buffer; the pipeline needs no vertex buffer.
	void drawPatches(VkCommandBuffer cmd, VkPipelineLayout layout, const HE::LandscapeRenderFrame& frame);

	// Compiles the two shaders and makes the layouts. False with `error` set when this build or device cannot do it (the landscape is then skipped).
	bool init(const Env& env, std::string* error = nullptr);
	void shutdown();
	bool ready() const;

	// Creates the images when a landscape began, then executes the frame's uploads and table updates in the renderer's upload batch, and CLEARS them
	// from `frame`: every frame the application built has to be executed once, the runtime counts its uploads as done. Records nothing in a frame.
	void upload(HE::LandscapeRenderFrame& frame);

	// Records the frame's patches into `cmd`, which is inside a render pass `pass` (colour + depth, one sample): depth test LESS with writes, no
	// culling, no blending. `viewProj` is the matrix the frame was built for, with HE::kVulkanClipFix applied. Sets viewport and scissor to `extent`;
	// binds its own pipeline, set 0 and buffers: the caller rebinds its own afterwards. `frameIndex` picks the descriptor set / uniform buffer of the
	// frame in flight (its fence has been waited on).
	void draw(VkCommandBuffer cmd, VkRenderPass pass, VkExtent2D extent, uint32_t frameIndex, const HE::LandscapeRenderFrame& frame,
	          const glm::mat4& viewProj, const Shading& shading);

	// The ground's depth into another pass of the frame (a shadow layer, a depth pre-pass) that is open on `cmd`: vertex stage only,
	// the pass's own matrix. `f` is the provider's frame for THAT matrix (its patches are culled for it) and has been through upload().
	// The caller sets the depth bias (vkCmdSetDepthBias) before, and binds its own pipeline again after.
	// `viewForPosition`: not depth only but the SSAO position pre-pass, a pass with ONE colour attachment that takes the view-space
	// position (this matrix times the origin-relative position), alpha 1; no depth bias is used then.
	// `giGBuf`: the GI pre-pass, a pass with TWO colour attachments: the origin-relative position (a = 1) and the surface normal
	// (a = roughness 1), what that pass writes for a mesh.
	// `velocityPrev`: the TAA velocity pass (one colour attachment, the scene's depth tested and not written): `clip` rasterises
	// (pulled towards the camera, vkls::velocityClip), `viewForPosition` is then this frame's view-projection and `velocityPrev`
	// last frame's, both with this frame's jitter; uvNow - uvPrev is written.
	void drawDepth(VkCommandBuffer cmd, VkRenderPass pass, VkExtent2D extent, uint32_t frameIndex, const HE::LandscapeRenderFrame& frame,
	               const glm::mat4& clip, const glm::mat4* viewForPosition = nullptr, bool giGBuf = false, const glm::mat4* velocityPrev = nullptr);
	const Stats& stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace HE
