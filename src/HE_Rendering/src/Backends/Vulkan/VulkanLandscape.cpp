#include "Backends/Vulkan/VulkanLandscape.h"

#include "Backends/Vulkan/VulkanLandscapeLayout.h"

#include <HorizonRendering/LandscapeShaderSource.h>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(HE_HAVE_SHADERC)
#include "ShaderCompiler.h"   // he::shaderc::compile
#endif

namespace HE
{

namespace
{
// One image of the landscape's own: device-local, sampled, written by copies.
struct Img
{
	VkImage        image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView    view = VK_NULL_HANDLE;
	VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;   // as of the end of the batch recorded so far
};

constexpr unsigned kHeights = 0, kTable = 1, kCoarseH = 2, kWeights = 3 /* x3 */, kCoarseW = 6 /* x3 */, kImages = 9;
// The baked far image is not one of these: its mip count varies with the world, so it gets its own Img (see Impl::farAlbedo).
} // namespace

struct VulkanLandscape::Impl
{
	Env env;
	VkShaderModule        vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
	VkPipelineLayout      pipeLayout = VK_NULL_HANDLE;
	VkDescriptorPool      pool = VK_NULL_HANDLE;
	static constexpr uint32_t kMaxFrames = 4;
	VkDescriptorSet       sets[kMaxFrames] = {};
	VkBuffer              ubo[kMaxFrames] = {};
	VkDeviceMemory        uboMem[kMaxFrames] = {};
	void*                 uboMapped[kMaxFrames] = {};
	VkSampler             nearest = VK_NULL_HANDLE;
	VkSampler             farSampler = VK_NULL_HANDLE;   // linear + mipped + clamp, for the baked far image only (its uv does not repeat)
	// One pipeline per render pass it is drawn in (the LDR swapchain pass and the HDR scene pass differ in colour format only; a pass that was
	// recreated has a new handle, which rebuilds the pipeline).
	struct Pipe { VkRenderPass pass = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; };
	std::vector<Pipe>     pipes;
	// The ground in other passes of the frame (shadow layers, depth pre-passes): depth only, each with its OWN LsGlobals (the matrix of
	// the pass) and its own set. A frame's uniform buffer holds the scene's block and then kMaxPasses more; passSets[frame][i] names
	// block i + 1. Handed out round robin (passNext): a frame draws fewer passes than there are.
	static constexpr uint32_t     kMaxPasses = 32;
	static constexpr VkDeviceSize kGlobalsStride = (sizeof(vkls::Globals) + 255) & ~VkDeviceSize(255);   // 256 >= any minUniformBufferOffsetAlignment
	VkDescriptorSet       passSets[kMaxFrames][kMaxPasses] = {};
	uint32_t              passNext = 0;
	std::vector<Pipe>     depthPipes;

	Img                   img[kImages];
	Img                   farAlbedo;   // the baked far image (sRGB, mipped); view is null when the world has none
	VkBuffer              vbo = VK_NULL_HANDLE, ibo = VK_NULL_HANDLE;
	VkDeviceMemory        vboMem = VK_NULL_HANDLE, iboMem = VK_NULL_HANDLE;
	std::vector<uint16_t> table;      // CPU mirror of the slot table: a change is one copy of the whole (small) image
	std::shared_ptr<const LandscapeRenderStatic> world;   // what the images were made for
	uint64_t coarseRevision = 0;                           // ... and the far data's state the coarse images hold
	Stats stats;
	bool ok = false;

	// ── small helpers ──────────────────────────────────────────────────────
	bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data, VkBuffer& buf, VkDeviceMemory& mem)
	{
		VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.size = size;
		bci.usage = usage;
		if (vkCreateBuffer(env.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(env.device, buf, &req);
		VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = env.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(env.device, &mai, nullptr, &mem) != VK_SUCCESS) return false;
		if (vkBindBufferMemory(env.device, buf, mem, 0) != VK_SUCCESS) return false;
		if (data)
		{
			void* p = nullptr;
			if (vkMapMemory(env.device, mem, 0, size, 0, &p) != VK_SUCCESS) return false;
			std::memcpy(p, data, static_cast<size_t>(size));
			vkUnmapMemory(env.device, mem);
		}
		return true;
	}
	void destroyBuffer(VkBuffer& b, VkDeviceMemory& m)
	{
		if (b) vkDestroyBuffer(env.device, b, nullptr);
		if (m) vkFreeMemory(env.device, m, nullptr);
		b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
	}

	bool makeImage(Img& im, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, bool array, uint32_t mipLevels = 1)
	{
		VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = fmt;
		ici.extent = { std::max(w, 1u), std::max(h, 1u), 1 };
		ici.mipLevels = std::max(mipLevels, 1u);
		ici.arrayLayers = std::max(layers, 1u);
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(env.device, &ici, nullptr, &im.image) != VK_SUCCESS) return false;
		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(env.device, im.image, &req);
		VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = env.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vkAllocateMemory(env.device, &mai, nullptr, &im.mem) != VK_SUCCESS) return false;
		if (vkBindImageMemory(env.device, im.image, im.mem, 0) != VK_SUCCESS) return false;
		VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vci.image = im.image;
		vci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		vci.format = fmt;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, ici.mipLevels, 0, ici.arrayLayers };
		if (vkCreateImageView(env.device, &vci, nullptr, &im.view) != VK_SUCCESS) return false;
		im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
		return true;
	}
	void destroyImage(Img& im)
	{
		if (im.view) vkDestroyImageView(env.device, im.view, nullptr);
		if (im.image) vkDestroyImage(env.device, im.image, nullptr);
		if (im.mem) vkFreeMemory(env.device, im.mem, nullptr);
		im = Img{};
	}
	void destroyImages()
	{
		for (Img& im : img) destroyImage(im);
		destroyImage(farAlbedo);
		destroyBuffer(vbo, vboMem);
		destroyBuffer(ibo, iboMem);
		table.clear();
		world.reset();
	}

	// SHADER_READ_ONLY (or UNDEFINED the first time) <-> TRANSFER_DST for the whole image. The first direction waits for every earlier frame's reads
	// (they were submitted before this batch, on the same queue), the second makes the copies visible to the shaders of the frames after it.
	void toTransfer(VkCommandBuffer cb, Img& im)
	{
		if (im.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
		VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		const bool fresh = im.layout == VK_IMAGE_LAYOUT_UNDEFINED;
		b.srcAccessMask = fresh ? 0 : VK_ACCESS_SHADER_READ_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = im.layout;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = im.image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
		vkCmdPipelineBarrier(cb, fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT),
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		im.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	void toRead(VkCommandBuffer cb, Img& im)
	{
		if (im.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
		VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = im.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : 0;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		b.oldLayout = im.layout;
		b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = im.image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
		                     nullptr, 0, nullptr, 1, &b);
		im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	// Stages `bytes` and copies them into one layer and mip level (and the whole extent of that level) of `im`.
	bool copyLayer(VkCommandBuffer cb, Img& im, uint32_t layer, uint32_t w, uint32_t h, const void* data, size_t bytes, uint32_t mip = 0)
	{
		VkBuffer src = VK_NULL_HANDLE; VkDeviceSize off = 0;
		if (!env.stage(data, bytes, src, off)) return false;
		toTransfer(cb, im);
		VkBufferImageCopy r{};
		r.bufferOffset = off;
		r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 };
		r.imageExtent = { w, h, 1 };
		vkCmdCopyBufferToImage(cb, src, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		return true;
	}
	// The baked far image (LandscapeRenderStatic::farMips), when the world has one: the only image here with more than one mip level, made
	// with the landscape's images and again whenever the coarse revision says the far data changed (an edit, a render window that moved).
	// A level of the wrong size leaves the world without a far image, like OpenGLLandscape::makeFar / MetalLandscape's.
	void makeFar(VkCommandBuffer cb, const LandscapeRenderStatic& w)
	{
		destroyImage(farAlbedo);
		if (w.farW <= 0 || w.farH <= 0 || w.farMips.empty()) return;
		const uint32_t mips = static_cast<uint32_t>(w.farMips.size());
		bool whole = makeImage(farAlbedo, VK_FORMAT_R8G8B8A8_SRGB, static_cast<uint32_t>(w.farW), static_cast<uint32_t>(w.farH), 1, false, mips);
		for (uint32_t l = 0; l < mips && whole; ++l)
		{
			const int lw = std::max(1, w.farW >> l), lh = std::max(1, w.farH >> l);
			const size_t bytes = static_cast<size_t>(lw) * static_cast<size_t>(lh) * 4;
			whole = w.farMips[l].size() == bytes && copyLayer(cb, farAlbedo, 0, static_cast<uint32_t>(lw), static_cast<uint32_t>(lh), w.farMips[l].data(), bytes, l);
		}
		if (whole) toRead(cb, farAlbedo);
		else { destroyImage(farAlbedo); ++stats.rejected; }
	}
	void clearImage(VkCommandBuffer cb, Img& im)
	{
		toTransfer(cb, im);
		VkClearColorValue zero{};
		const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
		vkCmdClearColorImage(cb, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
	}

	// `depthOnly`: the vertex stage alone into a pass without colour attachments, with the depth bias left to the caller
	// (vkCmdSetDepthBias: a shadow layer sets its own, a pre-pass sets zero).
	VkPipeline pipelineFor(VkRenderPass pass, bool depthOnly = false)
	{
		std::vector<Pipe>& pipes = depthOnly ? depthPipes : this->pipes;
		for (Pipe& p : pipes)
			if (p.pass == pass) return p.pipe;
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

		VkVertexInputBindingDescription bind{ 0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription attr{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
		VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
		vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attr;
		VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		vps.viewportCount = 1; vps.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		cb.attachmentCount = depthOnly ? 0u : 1u; cb.pAttachments = depthOnly ? nullptr : &cba;
		rs.depthBiasEnable = depthOnly ? VK_TRUE : VK_FALSE;
		const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
		VkPipelineDynamicStateCreateInfo dsi{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dsi.dynamicStateCount = depthOnly ? 3u : 2u; dsi.pDynamicStates = dyn;

		VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pci.stageCount = depthOnly ? 1u : 2u; pci.pStages = stages;
		pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps; pci.pRasterizationState = &rs;
		pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cb; pci.pDynamicState = &dsi;
		pci.layout = pipeLayout; pci.renderPass = pass; pci.subpass = 0;
		Pipe p{ pass, VK_NULL_HANDLE };
		if (env.createPipeline(pci, p.pipe) != VK_SUCCESS) p.pipe = VK_NULL_HANDLE;   // remembered: not retried every frame
		pipes.push_back(p);
		return p.pipe;
	}
};

VulkanLandscape::VulkanLandscape() : m_impl(std::make_unique<Impl>()) {}
VulkanLandscape::~VulkanLandscape() = default;
bool VulkanLandscape::ready() const { return m_impl->ok; }
const VulkanLandscape::Stats& VulkanLandscape::stats() const { return m_impl->stats; }

bool VulkanLandscape::init(const Env& env, std::string* error)
{
	Impl& m = *m_impl;
	if (m.ok) return true;
	auto fail = [&](const std::string& why) { if (error) *error = why; shutdown(); return false; };
	if (!env.device || !env.findMemoryType || !env.uploadCmd || !env.stage || !env.waitIdle || !env.flush || !env.createPipeline || !env.arraySampler || !env.whiteArray)
		return fail("landscape: the renderer did not lend everything it needs");
	if (env.framesInFlight == 0 || env.framesInFlight > Impl::kMaxFrames) return fail("landscape: unsupported number of frames in flight");
	m.env = env;
#if !defined(HE_HAVE_SHADERC)
	return fail("landscape: this build has no runtime shader compiler (HE_HAVE_SHADERC)");
#else
	// The shaders: the GL text behind the Vulkan prelude, to SPIR-V.
	auto compileStage = [&](const std::string& glsl, he::shaderc::Stage stage, const char* what, VkShaderModule& out) {
		if (glsl.empty()) { if (error) *error = std::string("landscape ") + what + ": the //#LANDSCAPE_BODY# marker is missing from the GLSL"; return false; }
		const he::shaderc::Result spv = he::shaderc::compile(glsl, stage, he::shaderc::Target::SpirvBinary);
		if (!spv.ok || spv.spirv.empty()) { if (error) *error = std::string("landscape ") + what + ": " + spv.log; return false; }
		VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = spv.spirv.size() * sizeof(uint32_t);
		ci.pCode = spv.spirv.data();
		if (vkCreateShaderModule(m.env.device, &ci, nullptr, &out) != VK_SUCCESS) { if (error) *error = std::string("landscape ") + what + ": vkCreateShaderModule failed"; return false; }
		return true;
	};
	if (!compileStage(glsl::BuildLandscapeVertexGLSL450(), he::shaderc::Stage::Vertex, "vertex shader", m.vs)
	    || !compileStage(glsl::BuildLandscapeFragmentGLSL450(), he::shaderc::Stage::Fragment, "fragment shader", m.fs))
	{
		const std::string e = error ? *error : std::string();
		shutdown();
		if (error) *error = e;
		return false;
	}

	// Set 0: the block and the thirteen images (binding numbers in LandscapeShaderSource.h).
	VkDescriptorSetLayoutBinding b[glsl::kLandscapeVkBindingCount]{};
	for (unsigned i = 0; i < glsl::kLandscapeVkBindingCount; ++i)
	{
		b[i].binding = i;
		b[i].descriptorType = i == glsl::kLandscapeVkBindingGlobals ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[i].descriptorCount = 1;
		const bool vertex = i == glsl::kLandscapeVkBindingGlobals || i == glsl::kLandscapeVkBindingTileHeights || i == glsl::kLandscapeVkBindingSlotTable
		                    || i == glsl::kLandscapeVkBindingCoarseHeights;
		const bool fragment = i != glsl::kLandscapeVkBindingTileHeights && i != glsl::kLandscapeVkBindingCoarseHeights;
		b[i].stageFlags = (vertex ? VK_SHADER_STAGE_VERTEX_BIT : 0u) | (fragment ? VK_SHADER_STAGE_FRAGMENT_BIT : 0u);
	}
	VkDescriptorSetLayoutCreateInfo slci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	slci.bindingCount = glsl::kLandscapeVkBindingCount; slci.pBindings = b;
	if (vkCreateDescriptorSetLayout(m.env.device, &slci, nullptr, &m.setLayout) != VK_SUCCESS) return fail("landscape: descriptor set layout");
	const VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vkls::PatchPush) };
	VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1; plci.pSetLayouts = &m.setLayout;
	plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
	if (vkCreatePipelineLayout(m.env.device, &plci, nullptr, &m.pipeLayout) != VK_SUCCESS) return fail("landscape: pipeline layout");

	const uint32_t setCount = m.env.framesInFlight * (1 + Impl::kMaxPasses);   // the scene's and the passes' (Impl::passSets)
	const VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount },
	                                     { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount * (glsl::kLandscapeVkBindingCount - 1) } };
	VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = setCount; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps;
	if (vkCreateDescriptorPool(m.env.device, &dpci, nullptr, &m.pool) != VK_SUCCESS) return fail("landscape: descriptor pool");
	for (uint32_t i = 0; i < m.env.framesInFlight; ++i)
	{
		VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		dsai.descriptorPool = m.pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m.setLayout;
		if (vkAllocateDescriptorSets(m.env.device, &dsai, &m.sets[i]) != VK_SUCCESS) return fail("landscape: descriptor set");
		for (uint32_t p = 0; p < Impl::kMaxPasses; ++p)
			if (vkAllocateDescriptorSets(m.env.device, &dsai, &m.passSets[i][p]) != VK_SUCCESS) return fail("landscape: pass descriptor set");
		if (!m.makeBuffer(Impl::kGlobalsStride * (1 + Impl::kMaxPasses), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, m.ubo[i], m.uboMem[i])
		    || vkMapMemory(m.env.device, m.uboMem[i], 0, VK_WHOLE_SIZE, 0, &m.uboMapped[i]) != VK_SUCCESS)
			return fail("landscape: uniform buffer");
	}
	VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(m.env.device, &sci, nullptr, &m.nearest) != VK_SUCCESS) return fail("landscape: sampler");
	VkSamplerCreateInfo fsci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // the baked far image: linear + mipped, clamped (its uv does not repeat)
	fsci.magFilter = VK_FILTER_LINEAR; fsci.minFilter = VK_FILTER_LINEAR; fsci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	fsci.addressModeU = fsci.addressModeV = fsci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	fsci.maxLod = VK_LOD_CLAMP_NONE;
	if (vkCreateSampler(m.env.device, &fsci, nullptr, &m.farSampler) != VK_SUCCESS) return fail("landscape: far sampler");
	m.ok = true;
	return true;
#endif
}

void VulkanLandscape::shutdown()
{
	Impl& m = *m_impl;
	if (!m.env.device) { m.ok = false; return; }
	if (m.env.waitIdle) m.env.waitIdle();
	for (Impl::Pipe& p : m.pipes)
		if (p.pipe) vkDestroyPipeline(m.env.device, p.pipe, nullptr);
	m.pipes.clear();
	for (Impl::Pipe& p : m.depthPipes)
		if (p.pipe) vkDestroyPipeline(m.env.device, p.pipe, nullptr);
	m.depthPipes.clear();
	m.destroyImages();
	for (uint32_t i = 0; i < Impl::kMaxFrames; ++i)
	{
		if (m.uboMapped[i]) vkUnmapMemory(m.env.device, m.uboMem[i]);
		m.uboMapped[i] = nullptr;
		m.destroyBuffer(m.ubo[i], m.uboMem[i]);
		m.sets[i] = VK_NULL_HANDLE;
	}
	if (m.nearest) vkDestroySampler(m.env.device, m.nearest, nullptr);
	if (m.farSampler) vkDestroySampler(m.env.device, m.farSampler, nullptr);
	if (m.pool) vkDestroyDescriptorPool(m.env.device, m.pool, nullptr);
	if (m.pipeLayout) vkDestroyPipelineLayout(m.env.device, m.pipeLayout, nullptr);
	if (m.setLayout) vkDestroyDescriptorSetLayout(m.env.device, m.setLayout, nullptr);
	if (m.vs) vkDestroyShaderModule(m.env.device, m.vs, nullptr);
	if (m.fs) vkDestroyShaderModule(m.env.device, m.fs, nullptr);
	m.nearest = VK_NULL_HANDLE; m.farSampler = VK_NULL_HANDLE; m.pool = VK_NULL_HANDLE; m.pipeLayout = VK_NULL_HANDLE; m.setLayout = VK_NULL_HANDLE;
	m.vs = VK_NULL_HANDLE; m.fs = VK_NULL_HANDLE;
	m.ok = false;
}

void VulkanLandscape::upload(HE::LandscapeRenderFrame& f)
{
	Impl& m = *m_impl;
	auto drop = [&]() { f.uploads.clear(); f.tableUpdates.clear(); };
	if (!m.ok || !f.active || !f.world) { drop(); return; }
	const LandscapeRenderStatic& w = *f.world;
	if (!vkls::worldUsable(w) || static_cast<uint32_t>(w.slotCapacity) > m.env.maxImageArrayLayers)
	{
		++m.stats.rejected;
		drop();
		return;
	}
	VkCommandBuffer cb = VK_NULL_HANDLE;
	auto batch = [&]() { if (!cb) cb = m.env.uploadCmd(); return cb; };

	// ── the images of a landscape that began (or the first frame) ─────────────
	if (m.world != f.world && (!m.world || m.world->generation != w.generation))
	{
		if (m.world)
		{
			// The old landscape's images may still be read by a frame in flight, and the open upload batch may already name them: submit it, then wait.
			m.env.flush();
			m.env.waitIdle();
		}
		m.destroyImages();
		if (!batch()) { ++m.stats.rejected; drop(); return; }
		const uint32_t slots = static_cast<uint32_t>(w.slotCapacity);
		bool good = m.makeImage(m.img[kHeights], VK_FORMAT_R16_UINT, static_cast<uint32_t>(w.heightW), static_cast<uint32_t>(w.heightH), slots, true);
		for (unsigned p = 0; p < 3; ++p)
			good = good && m.makeImage(m.img[kWeights + p], VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(w.weightW), static_cast<uint32_t>(w.weightH), slots, true);
		good = good && m.makeImage(m.img[kTable], VK_FORMAT_R16_UINT, static_cast<uint32_t>(w.tableWindow), static_cast<uint32_t>(w.tableWindow), 1, false);
		const bool haveCoarse = w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH) && w.coarseW > 0 && w.coarseH > 0;
		if (!haveCoarse) ++m.stats.rejected;
		const uint32_t chw = haveCoarse ? static_cast<uint32_t>(w.coarseW) : 1u, chh = haveCoarse ? static_cast<uint32_t>(w.coarseH) : 1u;
		good = good && m.makeImage(m.img[kCoarseH], VK_FORMAT_R16_UINT, chw, chh, 1, false);
		const uint32_t cww = static_cast<uint32_t>(std::max(w.coarseWeightW, 1)), cwh = static_cast<uint32_t>(std::max(w.coarseWeightH, 1));
		for (unsigned p = 0; p < 3; ++p)
			good = good && m.makeImage(m.img[kCoarseW + p], VK_FORMAT_R8G8B8A8_UNORM, cww, cwh, 1, false);

		// the grid: (i, j) as floats (the shader's aGrid), and the shared 16-bit index buffer
		const int n = vkls::kGridVerts;
		std::vector<float> grid(static_cast<size_t>(n) * n * 2);
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				grid[(static_cast<size_t>(j) * n + i) * 2 + 0] = static_cast<float>(i);
				grid[(static_cast<size_t>(j) * n + i) * 2 + 1] = static_cast<float>(j);
			}
		good = good && m.makeBuffer(grid.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, grid.data(), m.vbo, m.vboMem)
		       && m.makeBuffer(w.patchIndices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, w.patchIndices.data(), m.ibo, m.iboMem);
		if (good)
		{
			// Pages the tiles do not have stay zero (a landscape of fewer than 9 channels; the shader reads all three): Vulkan does not zero memory.
			for (Img& im : m.img) m.clearImage(cb, im);
			m.table.assign(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
			good = m.copyLayer(cb, m.img[kTable], 0, static_cast<uint32_t>(w.tableWindow), static_cast<uint32_t>(w.tableWindow), m.table.data(), m.table.size() * 2);
			if (haveCoarse)
				good = good && m.copyLayer(cb, m.img[kCoarseH], 0, chw, chh, w.coarseHeights.data(), w.coarseHeights.size() * 2);
			for (unsigned p = 0; p < 3 && good; ++p)
			{
				const size_t bytes = static_cast<size_t>(cww) * cwh * 4;
				const bool have = p < w.coarseWeights.size() && w.coarseWeights[p].size() == bytes;
				if (have) good = m.copyLayer(cb, m.img[kCoarseW + p], 0, cww, cwh, w.coarseWeights[p].data(), bytes);   // else it stays the zeros of the clear
			}
		}
		if (!good)
		{
			// The batch names the images that are about to go: submit it, wait for the GPU, then free them.
			m.env.flush();
			m.env.waitIdle();
			m.destroyImages();
			++m.stats.rejected;
			drop();
			return;
		}
		for (Img& im : m.img) m.toRead(cb, im);
		m.makeFar(cb, w);
		m.world = f.world;
		m.coarseRevision = w.coarseRevision;
		++m.stats.generations;
	}
	else if (m.world && m.coarseRevision != w.coarseRevision)
	{
		// The far data changed and nothing else: an edited tile, or the render window moved (then the coarse images may have another
		// size). The coarse images and the far image are made again; the tile atlas and the slot table stay exactly as they are, which
		// is the point: the near ground never falls back to the far levels for this. A frame in flight may still read the old ones:
		// submit, wait (once per stroke or per 32 km travelled), then free them.
		m.env.flush();
		m.env.waitIdle();
		m.destroyImage(m.img[kCoarseH]);
		for (unsigned p = 0; p < 3; ++p) m.destroyImage(m.img[kCoarseW + p]);
		const bool haveCoarse = w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH) && w.coarseW > 0 && w.coarseH > 0;
		if (!haveCoarse) ++m.stats.rejected;
		const uint32_t chw = haveCoarse ? static_cast<uint32_t>(w.coarseW) : 1u, chh = haveCoarse ? static_cast<uint32_t>(w.coarseH) : 1u;
		const uint32_t cww = static_cast<uint32_t>(std::max(w.coarseWeightW, 1)), cwh = static_cast<uint32_t>(std::max(w.coarseWeightH, 1));
		bool good = batch() != VK_NULL_HANDLE && m.makeImage(m.img[kCoarseH], VK_FORMAT_R16_UINT, chw, chh, 1, false);
		for (unsigned p = 0; p < 3; ++p) good = good && m.makeImage(m.img[kCoarseW + p], VK_FORMAT_R8G8B8A8_UNORM, cww, cwh, 1, false);
		if (good)
		{
			m.clearImage(cb, m.img[kCoarseH]);
			for (unsigned p = 0; p < 3; ++p) m.clearImage(cb, m.img[kCoarseW + p]);
			if (haveCoarse) good = m.copyLayer(cb, m.img[kCoarseH], 0, chw, chh, w.coarseHeights.data(), w.coarseHeights.size() * 2);
			for (unsigned p = 0; p < 3 && good; ++p)
			{
				const size_t bytes = static_cast<size_t>(cww) * cwh * 4;
				if (p < w.coarseWeights.size() && w.coarseWeights[p].size() == bytes) good = m.copyLayer(cb, m.img[kCoarseW + p], 0, cww, cwh, w.coarseWeights[p].data(), bytes);
			}
		}
		if (!good)
		{
			// Without coarse images nothing can be drawn: the landscape starts over at the next frame (its generation block).
			m.env.flush();
			m.env.waitIdle();
			m.destroyImages();
			++m.stats.rejected;
			drop();
			return;
		}
		m.toRead(cb, m.img[kCoarseH]);
		for (unsigned p = 0; p < 3; ++p) m.toRead(cb, m.img[kCoarseW + p]);
		m.makeFar(cb, w);
		m.world = f.world;
		m.coarseRevision = w.coarseRevision;
	}
	if (!m.world) { drop(); return; }

	// ── the frame's uploads, then its table changes (a slot's image is in before the table says so) ──
	const size_t hBytes = static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH);
	const size_t wBytes = static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4;
	for (const LandscapeRenderUpload& u : f.uploads)
	{
		if (u.slot >= w.slotCapacity || u.heights.size() != hBytes || !batch()) { ++m.stats.rejected; continue; }
		if (!m.copyLayer(cb, m.img[kHeights], u.slot, static_cast<uint32_t>(w.heightW), static_cast<uint32_t>(w.heightH), u.heights.data(), hBytes * 2))
		{ ++m.stats.rejected; continue; }
		for (size_t p = 0; p < u.pages.size() && p < 3; ++p)
		{
			if (u.pages[p].size() != wBytes) { ++m.stats.rejected; continue; }
			if (!m.copyLayer(cb, m.img[kWeights + p], u.slot, static_cast<uint32_t>(w.weightW), static_cast<uint32_t>(w.weightH), u.pages[p].data(), wBytes))
				++m.stats.rejected;
		}
		++m.stats.uploads;
	}
	bool tableDirty = false;
	for (const LandscapeRenderTableUpdate& t : f.tableUpdates)
	{
		if (t.index >= m.table.size()) { ++m.stats.rejected; continue; }
		m.table[t.index] = t.slot;
		tableDirty = true;
		++m.stats.tableUpdates;
	}
	if (tableDirty && batch()
	    && !m.copyLayer(cb, m.img[kTable], 0, static_cast<uint32_t>(w.tableWindow), static_cast<uint32_t>(w.tableWindow), m.table.data(), m.table.size() * 2))
		++m.stats.rejected;
	if (cb)
		for (Img& im : m.img) m.toRead(cb, im);   // only the images that were written are in TRANSFER_DST
	drop();
}

void VulkanLandscape::draw(VkCommandBuffer cmd, VkRenderPass pass, VkExtent2D extent, uint32_t frameIndex, const HE::LandscapeRenderFrame& f,
                           const glm::mat4& viewProj, const Shading& shading)
{
	Impl& m = *m_impl;
	m.stats.patches = m.stats.drawCalls = 0;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation || frameIndex >= m.env.framesInFlight) return;
	const LandscapeRenderStatic& w = *m.world;
	const VkPipeline pipe = m.pipelineFor(pass);
	if (!pipe) return;

	vkls::Globals g;
	vkls::fillGlobals(g, f, viewProj, shading.sunDir, shading.sunColor, shading.ambient, shading.albedoArray != VK_NULL_HANDLE,
	                  shading.normalArray != VK_NULL_HANDLE, shading.maskArray != VK_NULL_HANDLE, m.farAlbedo.view != VK_NULL_HANDLE);
	std::memcpy(m.uboMapped[frameIndex], &g, sizeof(g));

	// Set 0 of this frame in flight (its fence has been waited on): the block and the thirteen images; an array the renderer could not resolve is the
	// white stand-in (the shader reads it only where uTex / uTexMaps say so).
	VkDescriptorBufferInfo bi{ m.ubo[frameIndex], 0, sizeof(vkls::Globals) };
	VkDescriptorImageInfo ii[glsl::kLandscapeVkBindingCount] = {};
	const VkImageView views[glsl::kLandscapeVkBindingCount] = {
		VK_NULL_HANDLE,
		m.img[kHeights].view, m.img[kTable].view, m.img[kCoarseH].view,
		m.img[kWeights].view, m.img[kWeights + 1].view, m.img[kWeights + 2].view,
		m.img[kCoarseW].view, m.img[kCoarseW + 1].view, m.img[kCoarseW + 2].view,
		shading.albedoArray ? shading.albedoArray : m.env.whiteArray,
		shading.normalArray ? shading.normalArray : m.env.whiteArray,
		shading.maskArray ? shading.maskArray : m.env.whiteArray,
		// No far image: any complete 2D texture, the shader does not read it (uTexMaps.z is then 0).
		m.farAlbedo.view ? m.farAlbedo.view : m.img[kCoarseW].view };
	VkWriteDescriptorSet wr[glsl::kLandscapeVkBindingCount] = {};
	for (unsigned i = 0; i < glsl::kLandscapeVkBindingCount; ++i)
	{
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = m.sets[frameIndex];
		wr[i].dstBinding = i;
		wr[i].descriptorCount = 1;
		if (i == glsl::kLandscapeVkBindingGlobals)
		{
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			wr[i].pBufferInfo = &bi;
		}
		else
		{
			const VkSampler smp = i == glsl::kLandscapeVkBindingFar ? m.farSampler : i >= glsl::kLandscapeVkBindingAlbedo ? m.env.arraySampler : m.nearest;
			ii[i] = { smp, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr[i].pImageInfo = &ii[i];
		}
	}
	vkUpdateDescriptorSets(m.env.device, glsl::kLandscapeVkBindingCount, wr, 0, nullptr);

	const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f };
	const VkRect2D sc{ { 0, 0 }, extent };
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeLayout, 0, 1, &m.sets[frameIndex], 0, nullptr);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbo, &zero);
	drawPatches(cmd, m.pipeLayout, f);
}

void VulkanLandscape::drawDepth(VkCommandBuffer cmd, VkRenderPass pass, VkExtent2D extent, uint32_t frameIndex, const HE::LandscapeRenderFrame& f,
                                const glm::mat4& clip)
{
	Impl& m = *m_impl;
	if (!m.ok || !f.active || !f.world || f.patches.empty() || !m.world || m.world->generation != f.world->generation || frameIndex >= m.env.framesInFlight) return;
	const VkPipeline pipe = m.pipelineFor(pass, /*depthOnly=*/true);
	if (!pipe) return;
	const uint32_t slot = m.passNext++ % Impl::kMaxPasses;
	const VkDeviceSize offset = Impl::kGlobalsStride * (1 + slot);
	vkls::Globals g;
	vkls::fillGlobals(g, f, clip, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), glm::vec3(0.0f), false, false, false, false);
	std::memcpy(static_cast<uint8_t*>(m.uboMapped[frameIndex]) + offset, &g, sizeof(g));

	// The whole set, as the scene's: the vertex stage reads the block, the heights and the slot table; the rest is what the layout
	// names, valid and unread.
	const VkDescriptorSet set = m.passSets[frameIndex][slot];
	VkDescriptorBufferInfo bi{ m.ubo[frameIndex], offset, sizeof(vkls::Globals) };
	VkDescriptorImageInfo ii[glsl::kLandscapeVkBindingCount] = {};
	const VkImageView views[glsl::kLandscapeVkBindingCount] = {
		VK_NULL_HANDLE,
		m.img[kHeights].view, m.img[kTable].view, m.img[kCoarseH].view,
		m.img[kWeights].view, m.img[kWeights + 1].view, m.img[kWeights + 2].view,
		m.img[kCoarseW].view, m.img[kCoarseW + 1].view, m.img[kCoarseW + 2].view,
		m.env.whiteArray, m.env.whiteArray, m.env.whiteArray, m.img[kCoarseW].view };
	VkWriteDescriptorSet wr[glsl::kLandscapeVkBindingCount] = {};
	for (unsigned i = 0; i < glsl::kLandscapeVkBindingCount; ++i)
	{
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = set;
		wr[i].dstBinding = i;
		wr[i].descriptorCount = 1;
		if (i == glsl::kLandscapeVkBindingGlobals)
		{
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			wr[i].pBufferInfo = &bi;
		}
		else
		{
			const VkSampler smp = i == glsl::kLandscapeVkBindingFar ? m.farSampler : i >= glsl::kLandscapeVkBindingAlbedo ? m.env.arraySampler : m.nearest;
			ii[i] = { smp, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			wr[i].pImageInfo = &ii[i];
		}
	}
	vkUpdateDescriptorSets(m.env.device, glsl::kLandscapeVkBindingCount, wr, 0, nullptr);

	const VkViewport vp{ 0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f };
	const VkRect2D sc{ { 0, 0 }, extent };
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m.pipeLayout, 0, 1, &set, 0, nullptr);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m.vbo, &zero);
	drawPatches(cmd, m.pipeLayout, f);
}

bool VulkanLandscape::materialImages(uint32_t frameIndex, const HE::LandscapeRenderFrame& f, const glm::mat4& viewProj, MaterialImages& out)
{
	Impl& m = *m_impl;
	out = MaterialImages{};
	if (!m.ok || !f.active || !f.world || !m.world || m.world->generation != f.world->generation || frameIndex >= m.env.framesInFlight) return false;
	vkls::Globals g;
	vkls::fillGlobals(g, f, viewProj, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), glm::vec3(0.0f), false, false, false, false);   // the material lights, not these
	std::memcpy(m.uboMapped[frameIndex], &g, sizeof(g));
	out.globals = { m.ubo[frameIndex], 0, sizeof(vkls::Globals) };
	out.heights = m.img[kHeights].view;
	out.table = m.img[kTable].view;
	out.coarseHeights = m.img[kCoarseH].view;
	for (unsigned p = 0; p < 3; ++p) { out.weights[p] = m.img[kWeights + p].view; out.coarseWeights[p] = m.img[kCoarseW + p].view; }
	out.nearest = m.nearest;
	return out.heights && out.table && out.coarseHeights && out.weights[0] && out.coarseWeights[0];
}

void VulkanLandscape::drawPatches(VkCommandBuffer cmd, VkPipelineLayout layout, const HE::LandscapeRenderFrame& f)
{
	Impl& m = *m_impl;
	if (!m.ok || !m.world || f.patches.empty()) return;
	const LandscapeRenderStatic& w = *m.world;
	vkCmdBindIndexBuffer(cmd, m.ibo, 0, VK_INDEX_TYPE_UINT16);

	const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
	for (const LandscapeRenderPatch& p : f.patches)
	{
		vkls::PatchPush pd;
		vkls::fillPatch(pd, p);
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pd), &pd);
		if ((p.quadrants & 15) == 15)
		{
			vkCmdDrawIndexed(cmd, total, 1, 0, 0, 0);
			++m.stats.drawCalls;
		}
		else
		{
			for (int q = 0; q < 4; ++q)
			{
				if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
				vkCmdDrawIndexed(cmd, w.quadrantCount[q], 1, w.quadrantFirst[q], 0, 0);
				++m.stats.drawCalls;
			}
		}
		++m.stats.patches;
	}
}

} // namespace HE
