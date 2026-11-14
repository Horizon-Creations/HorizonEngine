#include "Backends/Metal/MetalLandscape.h"

#include <HorizonRendering/LandscapeShaderSource.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace HE;

namespace
{
// The C++ mirror of LsGlobals / LsPatch in kLandscapeMSL. Every member is 16-byte aligned in both languages (float4x4, float4, int4).
struct alignas(16) Globals
{
	float   viewProj[16];
	float   cameraRel[4];
	float   world[4];
	float   heightInfo[4];
	float   origin[4];
	int32_t tile[4];
	int32_t heightRect[8][4];
	int32_t coarseGrid[16][4];
	float   layerColor[12][4];
	float   sunDir[4];
	float   sunColor[4];
	float   ambient[4];
	float   texMaps[4];
	float   tex[4];
};
struct alignas(16) PatchData
{
	int32_t patch[4];
	float   patchPos[4];
	float   morph[4];
};
static_assert(sizeof(Globals) == 64 + 4 * 16 + 16 + 8 * 16 + 16 * 16 + 12 * 16 + 5 * 16, "LsGlobals: the MSL struct and this one have to be the same size");
static_assert(sizeof(PatchData) == 48, "LsPatch: the MSL struct and this one have to be the same size");

constexpr NSUInteger kBuf = 20, kPatchBuf = 21;      // constant buffers (vertex: 20, 21; fragment: 20)
constexpr NSUInteger kTex = 20;                       // first texture index (vertex: 20-22; fragment: 20-29, 27-29 = the albedo, normal and mask arrays)
} // namespace

struct MetalLandscape::Impl
{
	id<MTLDevice> device = nil;
	id<MTLRenderPipelineState> pipeline = nil;
	id<MTLDepthStencilState> depth = nil;
	id<MTLBuffer> indices = nil;
	id<MTLTexture> tileHeights = nil, slotTable = nil, coarseHeights = nil;
	id<MTLTexture> weights[3] = { nil, nil, nil };
	id<MTLTexture> coarseWeights[3] = { nil, nil, nil };
	id<MTLTexture> noArray = nil;   // bound at 27 when there is no albedo array: the shader does not read it, Metal wants something there
	std::shared_ptr<const LandscapeRenderStatic> world;
	Stats stats;
	bool ok = false;

	void destroyImages()
	{
		tileHeights = slotTable = coarseHeights = nil;
		for (int i = 0; i < 3; ++i) weights[i] = coarseWeights[i] = nil;
		indices = nil;
		world.reset();
	}

	MTLStorageMode storage() const { return device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged; }

	id<MTLTexture> make2D(MTLPixelFormat fmt, int w, int h, int layers)
	{
		MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
		d.textureType = layers > 0 ? MTLTextureType2DArray : MTLTextureType2D;
		d.pixelFormat = fmt;
		d.width = static_cast<NSUInteger>(std::max(w, 1));
		d.height = static_cast<NSUInteger>(std::max(h, 1));
		d.arrayLength = static_cast<NSUInteger>(std::max(layers, 1));
		d.mipmapLevelCount = 1;
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = storage();
		return [device newTextureWithDescriptor:d];
	}
};

MetalLandscape::MetalLandscape() : m_impl(std::make_unique<Impl>()) {}
MetalLandscape::~MetalLandscape() = default;
bool MetalLandscape::ready() const { return m_impl->ok; }
const MetalLandscape::Stats& MetalLandscape::stats() const { return m_impl->stats; }

bool MetalLandscape::init(void* devicePtr, unsigned long colorFormat, unsigned long depthFormat, std::string* error)
{
	Impl& m = *m_impl;
	if (m.ok) return true;
	@autoreleasepool
	{
		m.device = (__bridge id<MTLDevice>)devicePtr;
		if (!m.device) { if (error) *error = "landscape: no MTLDevice"; return false; }
		NSError* err = nil;
		id<MTLLibrary> lib = [m.device newLibraryWithSource:[NSString stringWithUTF8String:glsl::kLandscapeMSL] options:nil error:&err];
		if (!lib)
		{
			if (error) *error = std::string("landscape shader: ") + (err ? [[err localizedDescription] UTF8String] : "unknown");
			return false;
		}
		MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
		pd.vertexFunction = [lib newFunctionWithName:@"vertexMain"];
		pd.fragmentFunction = [lib newFunctionWithName:@"fragmentMain"];
		pd.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(colorFormat);
		pd.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(depthFormat);
		m.pipeline = [m.device newRenderPipelineStateWithDescriptor:pd error:&err];
		if (!m.pipeline)
		{
			if (error) *error = std::string("landscape pipeline: ") + (err ? [[err localizedDescription] UTF8String] : "unknown");
			return false;
		}
		MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
		dd.depthCompareFunction = MTLCompareFunctionLess;
		dd.depthWriteEnabled = YES;
		m.depth = [m.device newDepthStencilStateWithDescriptor:dd];
		m.noArray = m.make2D(MTLPixelFormatRGBA8Unorm, 1, 1, 1);
	}
	m.ok = true;
	return true;
}

void MetalLandscape::shutdown()
{
	Impl& m = *m_impl;
	m.destroyImages();
	m.pipeline = nil;
	m.depth = nil;
	m.device = nil;
	m.ok = false;
}

void MetalLandscape::encode(void* renderEncoder, const LandscapeRenderFrame& f, const glm::mat4& viewProj, const Shading& shading)
{
	Impl& m = *m_impl;
	m.stats.patches = m.stats.drawCalls = 0;
	if (!m.ok || !f.active || !f.world || !renderEncoder) return;
	const LandscapeRenderStatic& w = *f.world;

	@autoreleasepool
	{
		// ── the images of a landscape that began (or the first frame) ─────────────
		if (m.world != f.world && (!m.world || m.world->generation != w.generation))
		{
			m.destroyImages();
			m.tileHeights = m.make2D(MTLPixelFormatR16Uint, w.heightW, w.heightH, w.slotCapacity);
			for (int p = 0; p < 3; ++p) m.weights[p] = m.make2D(MTLPixelFormatRGBA8Unorm, w.weightW, w.weightH, w.slotCapacity);
			m.slotTable = m.make2D(MTLPixelFormatR16Uint, w.tableWindow, w.tableWindow, 0);
			const std::vector<uint16_t> empty(static_cast<size_t>(w.tableWindow) * static_cast<size_t>(w.tableWindow), 0xFFFF);
			[m.slotTable replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w.tableWindow), static_cast<NSUInteger>(w.tableWindow))
			               mipmapLevel:0 withBytes:empty.data() bytesPerRow:static_cast<NSUInteger>(w.tableWindow) * 2];
			m.coarseHeights = m.make2D(MTLPixelFormatR16Uint, w.coarseW, w.coarseH, 0);
			if (w.coarseHeights.size() == static_cast<size_t>(w.coarseW) * static_cast<size_t>(w.coarseH))
				[m.coarseHeights replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w.coarseW), static_cast<NSUInteger>(w.coarseH))
				                   mipmapLevel:0 withBytes:w.coarseHeights.data() bytesPerRow:static_cast<NSUInteger>(w.coarseW) * 2];
			else
				++m.stats.rejected;
			for (int p = 0; p < 3; ++p)
			{
				m.coarseWeights[p] = m.make2D(MTLPixelFormatRGBA8Unorm, w.coarseWeightW, w.coarseWeightH, 0);
				const bool have = static_cast<size_t>(p) < w.coarseWeights.size()
				                  && w.coarseWeights[static_cast<size_t>(p)].size() == static_cast<size_t>(w.coarseWeightW) * static_cast<size_t>(w.coarseWeightH) * 4;
				const std::vector<uint8_t> none(have ? 0 : static_cast<size_t>(std::max(w.coarseWeightW, 1)) * static_cast<size_t>(std::max(w.coarseWeightH, 1)) * 4, 0);
				[m.coarseWeights[p] replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(std::max(w.coarseWeightW, 1)), static_cast<NSUInteger>(std::max(w.coarseWeightH, 1)))
				                      mipmapLevel:0 withBytes:(have ? w.coarseWeights[static_cast<size_t>(p)].data() : none.data())
				                      bytesPerRow:static_cast<NSUInteger>(std::max(w.coarseWeightW, 1)) * 4];   // a page the world does not have is zero
			}
			m.indices = [m.device newBufferWithBytes:w.patchIndices.data() length:w.patchIndices.size() * sizeof(uint16_t) options:MTLResourceStorageModeShared];
			m.world = f.world;
			++m.stats.generations;
		}

		// ── the frame's uploads, then its table changes ──
		for (const LandscapeRenderUpload& u : f.uploads)
		{
			if (u.slot >= w.slotCapacity || u.heights.size() != static_cast<size_t>(w.heightW) * static_cast<size_t>(w.heightH)) { ++m.stats.rejected; continue; }
			[m.tileHeights replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w.heightW), static_cast<NSUInteger>(w.heightH))
			                 mipmapLevel:0 slice:u.slot withBytes:u.heights.data()
			                 bytesPerRow:static_cast<NSUInteger>(w.heightW) * 2 bytesPerImage:static_cast<NSUInteger>(w.heightW) * static_cast<NSUInteger>(w.heightH) * 2];
			for (size_t p = 0; p < u.pages.size() && p < 3; ++p)
			{
				if (u.pages[p].size() != static_cast<size_t>(w.weightW) * static_cast<size_t>(w.weightH) * 4) { ++m.stats.rejected; continue; }
				[m.weights[p] replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w.weightW), static_cast<NSUInteger>(w.weightH))
				                mipmapLevel:0 slice:u.slot withBytes:u.pages[p].data()
				                bytesPerRow:static_cast<NSUInteger>(w.weightW) * 4 bytesPerImage:static_cast<NSUInteger>(w.weightW) * static_cast<NSUInteger>(w.weightH) * 4];
			}
			++m.stats.uploads;
		}
		for (const LandscapeRenderTableUpdate& t : f.tableUpdates)
		{
			if (t.index >= static_cast<uint32_t>(w.tableWindow) * static_cast<uint32_t>(w.tableWindow)) { ++m.stats.rejected; continue; }
			const uint16_t v = t.slot;
			[m.slotTable replaceRegion:MTLRegionMake2D(t.index % static_cast<uint32_t>(w.tableWindow), t.index / static_cast<uint32_t>(w.tableWindow), 1, 1)
			               mipmapLevel:0 withBytes:&v bytesPerRow:2];
			++m.stats.tableUpdates;
		}
		if (f.patches.empty()) return;

		// ── encode ──
		Globals g{};
		std::memcpy(g.viewProj, &viewProj[0][0], sizeof(g.viewProj));
		g.cameraRel[0] = f.cameraRel.x; g.cameraRel[1] = f.cameraRel.y; g.cameraRel[2] = f.cameraRel.z; g.cameraRel[3] = f.slabDistance;
		g.world[0] = w.worldMaxX; g.world[1] = w.worldMaxZ; g.world[2] = w.tileM; g.world[3] = w.heightScaleM;
		g.heightInfo[0] = w.heightMinM; g.heightInfo[1] = static_cast<float>(w.tilesX); g.heightInfo[2] = static_cast<float>(w.tilesZ); g.heightInfo[3] = static_cast<float>(w.tableWindow);
		g.origin[0] = static_cast<float>(f.origin.x); g.origin[1] = static_cast<float>(f.origin.y); g.origin[2] = static_cast<float>(f.origin.z);
		g.tile[0] = w.hCells; g.tile[1] = w.wTexels; g.tile[2] = w.tileLevels;
		for (int l = 0; l < w.heightLevels && l < 8; ++l)
			for (int c = 0; c < 4; ++c) g.heightRect[l][c] = w.heightRect[l][c];
		for (int gi = 0; gi < w.coarseGridCount && gi < 16; ++gi)
			for (int c = 0; c < 4; ++c) g.coarseGrid[gi][c] = w.coarseGrid[gi][c];
		std::memcpy(g.layerColor, w.layerColor, sizeof(g.layerColor));
		g.sunDir[0] = shading.sunDir.x; g.sunDir[1] = shading.sunDir.y; g.sunDir[2] = shading.sunDir.z;
		g.sunColor[0] = shading.sunColor.x; g.sunColor[1] = shading.sunColor.y; g.sunColor[2] = shading.sunColor.z;
		g.ambient[0] = shading.ambient.x; g.ambient[1] = shading.ambient.y; g.ambient[2] = shading.ambient.z;
		// World UVs: a texture tile every `textureMeters` of absolute x/z; the origin's share is a fraction made here in double (see OpenGLLandscape).
		const bool tex = shading.albedoArray != nullptr && !w.albedoArrayPath.empty() && w.textureMeters > 0.0f;
		const double k = tex ? 1.0 / static_cast<double>(w.textureMeters) : 1.0;
		g.texMaps[0] = tex && shading.normalArray && !w.normalArrayPath.empty() ? 1.0f : 0.0f;
		g.texMaps[1] = tex && shading.maskArray && !w.maskArrayPath.empty() ? 1.0f : 0.0f;
		g.tex[0] = static_cast<float>(k); g.tex[1] = tex ? 1.0f : 0.0f;
		g.tex[2] = static_cast<float>(f.origin.x * k - std::floor(f.origin.x * k)); g.tex[3] = static_cast<float>(f.origin.z * k - std::floor(f.origin.z * k));

		id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
		[enc setRenderPipelineState:m.pipeline];
		[enc setDepthStencilState:m.depth];
		[enc setVertexBytes:&g length:sizeof(g) atIndex:kBuf];
		[enc setFragmentBytes:&g length:sizeof(g) atIndex:kBuf];
		[enc setVertexTexture:m.tileHeights atIndex:kTex + 0];
		[enc setVertexTexture:m.slotTable atIndex:kTex + 1];
		[enc setVertexTexture:m.coarseHeights atIndex:kTex + 2];
		[enc setFragmentTexture:m.slotTable atIndex:kTex + 0];
		for (int p = 0; p < 3; ++p)
		{
			[enc setFragmentTexture:m.weights[p] atIndex:kTex + 1 + static_cast<NSUInteger>(p)];
			[enc setFragmentTexture:m.coarseWeights[p] atIndex:kTex + 4 + static_cast<NSUInteger>(p)];
		}
		[enc setFragmentTexture:tex ? (__bridge id<MTLTexture>)shading.albedoArray : m.noArray atIndex:kTex + 7];
		[enc setFragmentTexture:g.texMaps[0] > 0.5f ? (__bridge id<MTLTexture>)shading.normalArray : m.noArray atIndex:kTex + 8];
		[enc setFragmentTexture:g.texMaps[1] > 0.5f ? (__bridge id<MTLTexture>)shading.maskArray : m.noArray atIndex:kTex + 9];

		const uint32_t total = w.quadrantFirst[3] + w.quadrantCount[3];
		for (const LandscapeRenderPatch& p : f.patches)
		{
			PatchData pd{};
			pd.patch[0] = p.level; pd.patch[1] = p.cornerX; pd.patch[2] = p.cornerZ; pd.patch[3] = p.coarse ? 1 : 0;
			pd.patchPos[0] = p.x0; pd.patchPos[1] = p.z0; pd.patchPos[2] = p.size; pd.patchPos[3] = static_cast<float>(p.gridIndex);
			pd.morph[0] = p.morphStart; pd.morph[1] = p.morphEnd;
			[enc setVertexBytes:&pd length:sizeof(pd) atIndex:kPatchBuf];
			if ((p.quadrants & 15) == 15)
			{
				[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:total indexType:MTLIndexTypeUInt16 indexBuffer:m.indices indexBufferOffset:0];
				++m.stats.drawCalls;
			}
			else
			{
				for (int q = 0; q < 4; ++q)
				{
					if (!(p.quadrants & (1 << q)) || w.quadrantCount[q] == 0) continue;
					[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:w.quadrantCount[q] indexType:MTLIndexTypeUInt16 indexBuffer:m.indices
					         indexBufferOffset:static_cast<NSUInteger>(w.quadrantFirst[q]) * sizeof(uint16_t)];
					++m.stats.drawCalls;
				}
			}
			++m.stats.patches;
		}
	}
}
