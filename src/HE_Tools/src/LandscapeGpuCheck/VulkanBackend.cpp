#include "GpuCheck.h"

#include <Backends/Vulkan/VulkanLandscape.h>
#include <HorizonRendering/ClipSpace.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <vector>

namespace gpucheck
{
namespace
{
// Vulkan offscreen: RGBA8 colour + a 32-bit float depth buffer, read back through host-visible buffers. No window and no swapchain: an instance, a device
// with one graphics queue, and VulkanLandscape (the renderer's class, the one the engine draws the landscape with) lent what VulkanRenderer lends it. The
// scene pass of VulkanRenderer rasterises HE::kVulkanClipFix * the GL matrix (y down, depth 0..1), and so does this: what lands in the depth buffer is the
// plain window depth, the same as OpenGL's (depthIsNdc() stays false), and row 0 of the image is the top.
class VulkanBackend final : public Backend
{
public:
	~VulkanBackend() override
	{
		if (m_device)
		{
			vkDeviceWaitIdle(m_device);
			m_landscape.shutdown();
			destroyTarget();
			dropWhite();
			if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
			if (m_queryPool) vkDestroyQueryPool(m_device, m_queryPool, nullptr);
			if (m_fence) vkDestroyFence(m_device, m_fence, nullptr);
			if (m_pool) vkDestroyCommandPool(m_device, m_pool, nullptr);
			vkDestroyDevice(m_device, nullptr);
		}
		if (m_instance) vkDestroyInstance(m_instance, nullptr);
	}
	const char* name() const override { return "Vulkan"; }

	bool init(std::string& error) override
	{
		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "landscape_gpu_check";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &app;
		if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) { error = "vkCreateInstance failed (no Vulkan loader or driver)"; return false; }
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
		if (n == 0) { error = "no Vulkan device"; return false; }
		std::vector<VkPhysicalDevice> devs(n);
		vkEnumeratePhysicalDevices(m_instance, &n, devs.data());
		int bestScore = -1;
		for (VkPhysicalDevice d : devs)
		{
			uint32_t qn = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
			std::vector<VkQueueFamilyProperties> qs(qn);
			vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
			int family = -1;
			for (uint32_t i = 0; i < qn; ++i)
				if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = static_cast<int>(i); break; }
			if (family < 0) continue;
			VkPhysicalDeviceProperties p{};
			vkGetPhysicalDeviceProperties(d, &p);
			const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
			if (score > bestScore) { bestScore = score; m_phys = d; m_family = static_cast<uint32_t>(family); m_props = p; }
		}
		if (!m_phys) { error = "no Vulkan device with a graphics queue"; return false; }
		vkGetPhysicalDeviceMemoryProperties(m_phys, &m_mem);
		const float prio = 1.0f;
		VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		qci.queueFamilyIndex = m_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
		VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
		if (vkCreateDevice(m_phys, &dci, nullptr, &m_device) != VK_SUCCESS) { error = "vkCreateDevice failed"; return false; }
		vkGetDeviceQueue(m_device, m_family, 0, &m_queue);
		VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = m_family;
		VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if (vkCreateCommandPool(m_device, &pci, nullptr, &m_pool) != VK_SUCCESS || vkCreateFence(m_device, &fci, nullptr, &m_fence) != VK_SUCCESS)
		{ error = "command pool / fence"; return false; }
		VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // the renderer's albedo sampler: linear, repeat, mips
		sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		sci.maxLod = VK_LOD_CLAMP_NONE;
		if (vkCreateSampler(m_device, &sci, nullptr, &m_sampler) != VK_SUCCESS) { error = "sampler"; return false; }
		if (m_props.limits.timestampComputeAndGraphics)
		{
			VkQueryPoolCreateInfo qp{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			qp.queryType = VK_QUERY_TYPE_TIMESTAMP; qp.queryCount = 2;
			vkCreateQueryPool(m_device, &qp, nullptr, &m_queryPool);
		}
		// the white 1x1 array the landscape binds in place of an array that is not there
		if (!makeArray(nullptr, &m_white, 1, 1, 1)) { error = "white array"; return false; }

		HE::VulkanLandscape::Env env;
		env.device = m_device;
		env.framesInFlight = 1;
		env.maxImageArrayLayers = m_props.limits.maxImageArrayLayers;
		env.arraySampler = m_sampler;
		env.whiteArray = m_white.view;
		env.findMemoryType = [this](uint32_t bits, VkMemoryPropertyFlags f) { return findMemory(bits, f); };
		env.uploadCmd = [this]() { return batch(); };
		env.stage = [this](const void* d, VkDeviceSize s, VkBuffer& b, VkDeviceSize& o) { return stage(d, s, b, o); };
		env.flush = [this]() { flush(); };
		env.waitIdle = [this]() { flush(); vkDeviceWaitIdle(m_device); };
		env.createPipeline = [this](const VkGraphicsPipelineCreateInfo& ci, VkPipeline& out) { return vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &out); };
		return m_landscape.init(env, &error);
	}

	bool render(const RenderIn& in, RenderOut& out) override
	{
		if ((in.width != m_w || in.height != m_h) && !makeTarget(in.width, in.height)) { out.error = "target"; return false; }
		using clock = std::chrono::steady_clock;
		const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

		// The matrix the Vulkan scene pass rasterises with: the GL one with the clip fix (y down, depth 0..1).
		const glm::mat4 vp = HE::kVulkanClipFix * in.viewProj;
		const auto t0 = clock::now();
		HE::LandscapeRenderFrame frame;
		in.provider(glm::dmat4(vp), /*zeroToOne=*/true, frame);
		const auto t1 = clock::now();

		// the group's arrays, RGBA8 without mips (the other backends' tool images are the same)
		Arr albedo, normal, mask;
		HE::VulkanLandscape::Shading sh;
		sh.sunDir = in.sunDir; sh.sunColor = in.sunColor; sh.ambient = in.ambient;
		const auto arr = [&](const TexArray* t, Arr& a) -> VkImageView {
			if (!t || t->size <= 0 || t->layers <= 0) return VK_NULL_HANDLE;
			return makeArray(t->rgba.data(), &a, static_cast<uint32_t>(t->size), static_cast<uint32_t>(t->size), static_cast<uint32_t>(t->layers)) ? a.view : VK_NULL_HANDLE;
		};
		sh.albedoArray = arr(in.albedo, albedo);
		sh.normalArray = arr(in.normal, normal);
		sh.maskArray = arr(in.mask, mask);

		m_landscape.upload(frame);   // into the batch
		flush();                     // executed before the draw is recorded: the batch is a command buffer of its own, submitted first

		VkCommandBuffer cb = frameCmd();
		if (!cb) { out.error = "command buffer"; return false; }
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cb, &bi);
		if (m_queryPool) { vkCmdResetQueryPool(cb, m_queryPool, 0, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0); }
		VkClearValue clears[2]{};
		clears[0].color = { { in.clear.r, in.clear.g, in.clear.b, in.clear.a } };
		clears[1].depthStencil = { 1.0f, 0 };
		VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rp.renderPass = m_rp; rp.framebuffer = m_fb; rp.renderArea.extent = { static_cast<uint32_t>(m_w), static_cast<uint32_t>(m_h) };
		rp.clearValueCount = 2; rp.pClearValues = clears;
		vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
		m_landscape.draw(cb, m_rp, rp.renderArea.extent, 0, frame, vp, sh);
		vkCmdEndRenderPass(cb);   // final layouts: TRANSFER_SRC
		if (m_queryPool) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);
		if (!in.timing)
		{
			VkBufferImageCopy c{};
			c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			c.imageExtent = { static_cast<uint32_t>(m_w), static_cast<uint32_t>(m_h), 1 };
			vkCmdCopyImageToBuffer(cb, m_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_colorBuf, 1, &c);
			c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			vkCmdCopyImageToBuffer(cb, m_depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_depthBuf, 1, &c);
			VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
		}
		vkEndCommandBuffer(cb);
		const auto t2 = clock::now();
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1; si.pCommandBuffers = &cb;
		vkResetFences(m_device, 1, &m_fence);
		if (vkQueueSubmit(m_queue, 1, &si, m_fence) != VK_SUCCESS || vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, ~0ull) != VK_SUCCESS)
		{ out.error = "submit"; return false; }
		const auto t3 = clock::now();
		out.providerMs = ms(t0, t1);
		out.encodeMs = ms(t1, t2);
		out.gpuMs = ms(t1, t3);   // wall time from the provider to the fence: an upper bound, unless the timestamps below are there
		if (m_queryPool)
		{
			uint64_t ts[2] = {};
			if (vkGetQueryPoolResults(m_device, m_queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && ts[1] >= ts[0])
				out.gpuMs = static_cast<double>(ts[1] - ts[0]) * static_cast<double>(m_props.limits.timestampPeriod) * 1e-6;
		}
		const auto& st = m_landscape.stats();
		out.uploads = st.uploads; out.patches = st.patches; out.drawCalls = st.drawCalls; out.rejected = st.rejected;
		for (Arr* a : { &albedo, &normal, &mask }) destroyArr(*a);
		if (in.timing) return true;

		out.width = m_w;
		out.height = m_h;
		out.rgba.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4, 0);
		out.depth.assign(static_cast<size_t>(m_w) * static_cast<size_t>(m_h), 1.0f);
		void* p = nullptr;
		if (vkMapMemory(m_device, m_colorBufMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { out.error = "map colour"; return false; }
		std::memcpy(out.rgba.data(), p, out.rgba.size());
		vkUnmapMemory(m_device, m_colorBufMem);
		if (vkMapMemory(m_device, m_depthBufMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { out.error = "map depth"; return false; }
		std::memcpy(out.depth.data(), p, out.depth.size() * 4);
		vkUnmapMemory(m_device, m_depthBufMem);
		return true;
	}

private:
	struct Arr { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
	struct Stager { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; };

	uint32_t findMemory(uint32_t bits, VkMemoryPropertyFlags f) const
	{
		for (uint32_t i = 0; i < m_mem.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (m_mem.memoryTypes[i].propertyFlags & f) == f) return i;
		return 0;
	}
	bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& b, VkDeviceMemory& mem)
	{
		VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.size = size; bci.usage = usage;
		if (vkCreateBuffer(m_device, &bci, nullptr, &b) != VK_SUCCESS) return false;
		VkMemoryRequirements r{};
		vkGetBufferMemoryRequirements(m_device, b, &r);
		VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = r.size;
		mai.memoryTypeIndex = findMemory(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		return vkAllocateMemory(m_device, &mai, nullptr, &mem) == VK_SUCCESS && vkBindBufferMemory(m_device, b, mem, 0) == VK_SUCCESS;
	}
	bool makeImage(Arr& a, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool array)
	{
		VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt; ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = layers;
		ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(m_device, &ici, nullptr, &a.image) != VK_SUCCESS) return false;
		VkMemoryRequirements r{};
		vkGetImageMemoryRequirements(m_device, a.image, &r);
		VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = r.size;
		mai.memoryTypeIndex = findMemory(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vkAllocateMemory(m_device, &mai, nullptr, &a.mem) != VK_SUCCESS || vkBindImageMemory(m_device, a.image, a.mem, 0) != VK_SUCCESS) return false;
		VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vci.image = a.image; vci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
		vci.subresourceRange = { aspect, 0, 1, 0, layers };
		return vkCreateImageView(m_device, &vci, nullptr, &a.view) == VK_SUCCESS;
	}
	void destroyArr(Arr& a)
	{
		if (a.view || a.image || a.mem) vkDeviceWaitIdle(m_device);
		if (a.view) vkDestroyImageView(m_device, a.view, nullptr);
		if (a.image) vkDestroyImage(m_device, a.image, nullptr);
		if (a.mem) vkFreeMemory(m_device, a.mem, nullptr);
		a = Arr{};
	}
	// An RGBA8 2D array of `layers` slices of w x h, filled with `rgba` (white when null), in SHADER_READ_ONLY layout once its upload ran.
	VkImageView makeArray(const uint8_t* rgba, Arr* a, uint32_t w, uint32_t h, uint32_t layers)
	{
		if (!makeImage(*a, VK_FORMAT_R8G8B8A8_UNORM, w, h, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, true)) return VK_NULL_HANDLE;
		const size_t bytes = static_cast<size_t>(w) * h * layers * 4;
		std::vector<uint8_t> white;
		if (!rgba) { white.assign(bytes, 0xFF); rgba = white.data(); }
		VkBuffer src = VK_NULL_HANDLE; VkDeviceSize off = 0;
		VkCommandBuffer cb = batch();
		if (!cb || !stage(rgba, bytes, src, off)) return VK_NULL_HANDLE;
		VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = a->image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		VkBufferImageCopy c{};
		c.bufferOffset = off; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers }; c.imageExtent = { w, h, 1 };
		vkCmdCopyBufferToImage(cb, src, a->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		return a->view;
	}
	void dropWhite() { destroyArr(m_white); }

	// The upload batch the landscape records into: opened on first use, submitted and waited for by flush().
	VkCommandBuffer batch()
	{
		if (m_batchOpen) return m_batchCmd;
		if (!m_batchCmd)
		{
			VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			ai.commandPool = m_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
			if (vkAllocateCommandBuffers(m_device, &ai, &m_batchCmd) != VK_SUCCESS) return VK_NULL_HANDLE;
		}
		VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(m_batchCmd, 0);
		vkBeginCommandBuffer(m_batchCmd, &bi);
		m_batchOpen = true;
		return m_batchCmd;
	}
	bool stage(const void* data, VkDeviceSize size, VkBuffer& buf, VkDeviceSize& off)
	{
		off = 0;
		Stager s;
		if (!makeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, s.buf, s.mem)) { dropStager(s); return false; }
		void* p = nullptr;
		if (vkMapMemory(m_device, s.mem, 0, size, 0, &p) != VK_SUCCESS) { dropStager(s); return false; }
		std::memcpy(p, data, static_cast<size_t>(size));
		vkUnmapMemory(m_device, s.mem);
		buf = s.buf;
		m_stagers.push_back(s);
		return true;
	}
	void dropStager(Stager& s)
	{
		if (s.buf) vkDestroyBuffer(m_device, s.buf, nullptr);
		if (s.mem) vkFreeMemory(m_device, s.mem, nullptr);
		s = Stager{};
	}
	void flush()
	{
		if (!m_batchOpen) return;
		m_batchOpen = false;
		VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };   // like VulkanRenderer::flushUploads: every copy visible to every later stage
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(m_batchCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
		vkEndCommandBuffer(m_batchCmd);
		VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1; si.pCommandBuffers = &m_batchCmd;
		vkResetFences(m_device, 1, &m_fence);
		vkQueueSubmit(m_queue, 1, &si, m_fence);
		vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, ~0ull);
		for (Stager& s : m_stagers) dropStager(s);
		m_stagers.clear();
	}
	VkCommandBuffer frameCmd()
	{
		if (!m_frameCmd)
		{
			VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			ai.commandPool = m_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
			if (vkAllocateCommandBuffers(m_device, &ai, &m_frameCmd) != VK_SUCCESS) return VK_NULL_HANDLE;
		}
		vkResetCommandBuffer(m_frameCmd, 0);
		return m_frameCmd;
	}

	void destroyTarget()
	{
		if (m_fb) vkDestroyFramebuffer(m_device, m_fb, nullptr);
		if (m_rp) vkDestroyRenderPass(m_device, m_rp, nullptr);
		destroyArr(m_color);
		destroyArr(m_depth);
		if (m_colorBuf) vkDestroyBuffer(m_device, m_colorBuf, nullptr);
		if (m_colorBufMem) vkFreeMemory(m_device, m_colorBufMem, nullptr);
		if (m_depthBuf) vkDestroyBuffer(m_device, m_depthBuf, nullptr);
		if (m_depthBufMem) vkFreeMemory(m_device, m_depthBufMem, nullptr);
		m_fb = VK_NULL_HANDLE; m_rp = VK_NULL_HANDLE;
		m_colorBuf = m_depthBuf = VK_NULL_HANDLE; m_colorBufMem = m_depthBufMem = VK_NULL_HANDLE;
	}
	bool makeTarget(int w, int h)
	{
		vkDeviceWaitIdle(m_device);
		destroyTarget();
		m_w = w; m_h = h;
		const uint32_t uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(h);
		bool ok = makeImage(m_color, VK_FORMAT_R8G8B8A8_UNORM, uw, uh, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, false)
		          && makeImage(m_depth, VK_FORMAT_D32_SFLOAT, uw, uh, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, false)
		          && makeBuffer(static_cast<VkDeviceSize>(uw) * uh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_colorBuf, m_colorBufMem)
		          && makeBuffer(static_cast<VkDeviceSize>(uw) * uh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, m_depthBuf, m_depthBufMem);
		if (!ok) return false;
		VkAttachmentDescription att[2]{};
		att[0].format = VK_FORMAT_R8G8B8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
		att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		att[1] = att[0];
		att[1].format = VK_FORMAT_D32_SFLOAT;
		const VkAttachmentReference cref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = &dref;
		VkSubpassDependency dep{};   // the copies after the pass wait for its writes
		dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
		dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		rpci.attachmentCount = 2; rpci.pAttachments = att; rpci.subpassCount = 1; rpci.pSubpasses = &sub; rpci.dependencyCount = 1; rpci.pDependencies = &dep;
		if (vkCreateRenderPass(m_device, &rpci, nullptr, &m_rp) != VK_SUCCESS) return false;
		const VkImageView views[2] = { m_color.view, m_depth.view };
		VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fbci.renderPass = m_rp; fbci.attachmentCount = 2; fbci.pAttachments = views; fbci.width = uw; fbci.height = uh; fbci.layers = 1;
		return vkCreateFramebuffer(m_device, &fbci, nullptr, &m_fb) == VK_SUCCESS;
	}

	VkInstance       m_instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_phys = VK_NULL_HANDLE;
	VkDevice         m_device = VK_NULL_HANDLE;
	VkQueue          m_queue = VK_NULL_HANDLE;
	uint32_t         m_family = 0;
	VkPhysicalDeviceProperties       m_props{};
	VkPhysicalDeviceMemoryProperties m_mem{};
	VkCommandPool    m_pool = VK_NULL_HANDLE;
	VkCommandBuffer  m_batchCmd = VK_NULL_HANDLE, m_frameCmd = VK_NULL_HANDLE;
	bool             m_batchOpen = false;
	std::vector<Stager> m_stagers;
	VkFence          m_fence = VK_NULL_HANDLE;
	VkSampler        m_sampler = VK_NULL_HANDLE;
	VkQueryPool      m_queryPool = VK_NULL_HANDLE;
	Arr              m_white, m_color, m_depth;
	VkRenderPass     m_rp = VK_NULL_HANDLE;
	VkFramebuffer    m_fb = VK_NULL_HANDLE;
	VkBuffer         m_colorBuf = VK_NULL_HANDLE, m_depthBuf = VK_NULL_HANDLE;
	VkDeviceMemory   m_colorBufMem = VK_NULL_HANDLE, m_depthBufMem = VK_NULL_HANDLE;
	int m_w = 0, m_h = 0;
	HE::VulkanLandscape m_landscape;
};
} // namespace

std::unique_ptr<Backend> makeVulkanBackend() { return std::make_unique<VulkanBackend>(); }
} // namespace gpucheck
