#include "GpuCheck.h"

#include <Backends/Metal/MetalLandscape.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

namespace gpucheck
{
namespace
{
// IEEE half to float (RGBA16Float read back as raw bits).
float halfToFloat(uint16_t h)
{
	const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
	float v;
	if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
	else if (exp == 31) v = man ? NAN : INFINITY;
	else v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
	return sign ? -v : v;
}

class MetalBackend final : public Backend
{
public:
	~MetalBackend() override { m_landscape.shutdown(); }
	const char* name() const override { return "Metal"; }
	bool depthIsNdc() const override { return true; }

	bool init(std::string& error) override
	{
		m_device = MTLCreateSystemDefaultDevice();
		if (!m_device) { error = "no Metal device"; return false; }
		m_queue = [m_device newCommandQueue];
		// the engine's scene pass: an HDR colour target and a 32-bit float depth buffer
		return m_landscape.init((__bridge void*)m_device, MTLPixelFormatRGBA16Float, MTLPixelFormatDepth32Float, &error);
	}

	bool render(const RenderIn& in, RenderOut& out) override
	{
		@autoreleasepool
		{
			const MTLStorageMode storage = m_device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
			MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
			                                                                              width:static_cast<NSUInteger>(in.width) height:static_cast<NSUInteger>(in.height) mipmapped:NO];
			cd.usage = MTLTextureUsageRenderTarget;
			cd.storageMode = storage;
			MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
			                                                                              width:static_cast<NSUInteger>(in.width) height:static_cast<NSUInteger>(in.height) mipmapped:NO];
			dd.usage = MTLTextureUsageRenderTarget;
			dd.storageMode = storage;
			id<MTLTexture> color = [m_device newTextureWithDescriptor:cd];
			id<MTLTexture> depth = [m_device newTextureWithDescriptor:dd];

			MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = color;
			rp.colorAttachments[0].loadAction = MTLLoadActionClear;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			rp.colorAttachments[0].clearColor = MTLClearColorMake(in.clear.r, in.clear.g, in.clear.b, in.clear.a);
			rp.depthAttachment.texture = depth;
			rp.depthAttachment.loadAction = MTLLoadActionClear;
			rp.depthAttachment.storeAction = MTLStoreActionStore;
			rp.depthAttachment.clearDepth = 1.0;

			id<MTLCommandBuffer> cb = [m_queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
			const auto t0 = std::chrono::steady_clock::now();
			HE::LandscapeRenderFrame frame;
			in.provider(glm::dmat4(in.viewProj), false, frame);   // the plain GL matrix, like the renderer's scene pass
			const auto t1 = std::chrono::steady_clock::now();
			MetalLandscape::Shading sh;
			sh.sunDir = in.sunDir;
			sh.sunColor = in.sunColor;
			sh.ambient = in.ambient;
			const auto upload = [&](const TexArray* t) -> id<MTLTexture> {
				if (!t || t->size <= 0 || t->layers <= 0) return nil;
				MTLTextureDescriptor* ad = [[MTLTextureDescriptor alloc] init];
				ad.textureType = MTLTextureType2DArray;
				ad.pixelFormat = MTLPixelFormatRGBA8Unorm;
				ad.width = static_cast<NSUInteger>(t->size);
				ad.height = static_cast<NSUInteger>(t->size);
				ad.arrayLength = static_cast<NSUInteger>(t->layers);
				ad.usage = MTLTextureUsageShaderRead;
				ad.storageMode = storage;
				id<MTLTexture> tex = [m_device newTextureWithDescriptor:ad];
				const NSUInteger row = static_cast<NSUInteger>(t->size) * 4, slice = row * static_cast<NSUInteger>(t->size);
				for (int l = 0; l < t->layers; ++l)
					[tex replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(t->size), static_cast<NSUInteger>(t->size))
					       mipmapLevel:0 slice:static_cast<NSUInteger>(l) withBytes:t->rgba.data() + static_cast<size_t>(l) * slice
					       bytesPerRow:row bytesPerImage:slice];
				return tex;
			};
			id<MTLTexture> array = upload(in.albedo), narray = upload(in.normal), marray = upload(in.mask);
			sh.albedoArray = array ? (__bridge void*)array : nullptr;
			sh.normalArray = narray ? (__bridge void*)narray : nullptr;
			sh.maskArray = marray ? (__bridge void*)marray : nullptr;
			m_landscape.encode((__bridge void*)enc, frame, in.viewProj, sh);
			[enc endEncoding];
			if (storage == MTLStorageModeManaged)
			{
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
				[blit synchronizeResource:color];
				[blit synchronizeResource:depth];
				[blit endEncoding];
			}
			const auto t2 = std::chrono::steady_clock::now();
			[cb commit];
			[cb waitUntilCompleted];
			if (cb.error) { out.error = [[cb.error localizedDescription] UTF8String]; return false; }
			out.providerMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
			out.encodeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
			out.gpuMs = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
			if (in.timing)
			{
				const auto& st = m_landscape.stats();
				out.uploads = st.uploads; out.patches = st.patches; out.drawCalls = st.drawCalls; out.rejected = st.rejected;
				return true;
			}

			const size_t w = static_cast<size_t>(in.width), h = static_cast<size_t>(in.height);
			std::vector<uint16_t> half(w * h * 4);
			std::vector<float> dp(w * h);
			[color getBytes:half.data() bytesPerRow:w * 8 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
			[depth getBytes:dp.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
			out.width = in.width;
			out.height = in.height;
			out.rgba.resize(w * h * 4);
			for (size_t i = 0; i < w * h * 4; ++i)   // linear HDR to 8 bit, clamped: the same numbers the GL target holds
				out.rgba[i] = static_cast<uint8_t>(std::lround(std::clamp(halfToFloat(half[i]), 0.0f, 1.0f) * 255.0f));
			out.depth = std::move(dp);                // Metal's row 0 is already the top
			const auto& st = m_landscape.stats();
			out.uploads = st.uploads;
			out.patches = st.patches;
			out.drawCalls = st.drawCalls;
			out.rejected = st.rejected;
		}
		return true;
	}

private:
	id<MTLDevice> m_device = nil;
	id<MTLCommandQueue> m_queue = nil;
	MetalLandscape m_landscape;
};
} // namespace

std::unique_ptr<Backend> makeMetalBackend() { return std::make_unique<MetalBackend>(); }
} // namespace gpucheck
