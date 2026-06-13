#include "Backends/Metal/MetalRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <vector>
#include <glm/glm.hpp>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// Swapchain / depth formats shared by every window target, the scene
// pipeline and the ImGui pass descriptor — they must all match.
static constexpr MTLPixelFormat kSwapchainFormat = MTLPixelFormatBGRA8Unorm;
static constexpr MTLPixelFormat kDepthFormat     = MTLPixelFormatDepth32Float;

// ─── Embedded unlit shader ────────────────────────────────────────────────────
// Mirrors the OpenGL backend's GLSL unlit shader (same light dir / ambient).
static const char* kUnlitMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
	packed_float3 position;
	packed_float3 normal;
	packed_float2 uv;
};

struct Uniforms {
	float4x4 mvp;
	float4x4 model;
	float4   color;
	float4   flags;   // x: hasTexture
};

struct LightGPU {
	float4 posType;        // xyz = position,  w = type (0 dir / 1 point / 2 spot)
	float4 dirSpot;        // xyz = direction, w = cos(spot half angle)
	float4 colorIntensity; // rgb = color,     w = intensity
	float4 params;         // x = range
};

struct SceneUniforms {
	float4   cameraPos;    // xyz used
	int      lightCount;
	int      pad0, pad1, pad2;
	LightGPU lights[8];
	float4x4 lightVP;        // directional-light view-proj (already in Metal clip)
	int      shadowEnabled;
	int      pad3, pad4, pad5;
};

struct VSOut {
	float4 position [[position]];
	float3 normal;
	float2 uv;
	float3 worldPos;
	float3 color;
	float  hasTexture;
};

vertex VSOut vertexMain(uint vid [[vertex_id]],
                        const device VertexIn* verts [[buffer(0)]],
                        constant Uniforms&     u     [[buffer(1)]])
{
	VSOut out;
	float4 world   = u.model * float4(float3(verts[vid].position), 1.0);
	out.position   = u.mvp * float4(float3(verts[vid].position), 1.0);
	out.worldPos   = world.xyz;
	float3x3 m3    = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz);
	out.normal     = m3 * float3(verts[vid].normal);
	out.uv         = float2(verts[vid].uv);
	out.color      = u.color.rgb;
	out.hasTexture = u.flags.x;
	return out;
}

// Depth-only vertex shader for the shadow pass: u.mvp carries lightVP * model.
vertex float4 vertexShadow(uint vid [[vertex_id]],
                           const device VertexIn* verts [[buffer(0)]],
                           constant Uniforms&     u     [[buffer(1)]])
{
	return u.mvp * float4(float3(verts[vid].position), 1.0);
}

float shadowFactor(constant SceneUniforms& scene, float3 worldPos, float3 N, float3 L,
                   texture2d<float> shadowMap, sampler shadowSmp)
{
	if (scene.shadowEnabled == 0) return 1.0;
	float4 lp = scene.lightVP * float4(worldPos, 1.0);
	float3 p  = lp.xyz / lp.w;            // z already [0,1] (Metal clip); xy in [-1,1]
	float2 uv = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5)); // tex origin top-left
	if (p.z > 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;
	float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
	float closest = shadowMap.sample(shadowSmp, uv).r;
	return (p.z - bias > closest) ? 0.35 : 1.0;
}

// Blinn-Phong over up to 8 scene lights; lightCount == 0 falls back to the
// fixed "headlight" so unlit scenes don't render black.
fragment float4 fragmentMain(VSOut in [[stage_in]],
                             constant SceneUniforms& scene [[buffer(0)]],
                             texture2d<float> baseColor [[texture(0)]],
                             sampler          smp       [[sampler(0)]],
                             texture2d<float> shadowMap [[texture(1)]],
                             sampler          shadowSmp [[sampler(1)]])
{
	float3 base = (in.hasTexture > 0.5)
		? baseColor.sample(smp, float2(in.uv.x, 1.0 - in.uv.y)).rgb
		: in.color;
	float3 N = normalize(in.normal);

	if (scene.lightCount == 0)
	{
		float3 L    = normalize(float3(0.5, 0.8, 0.6));
		float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		return float4(base * diff, 1.0);
	}

	float3 V      = normalize(scene.cameraPos.xyz - in.worldPos);
	float3 result = 0.08 * base; // ambient floor

	for (int i = 0; i < scene.lightCount; ++i)
	{
		constant LightGPU& l = scene.lights[i];
		int    type  = int(l.posType.w);
		float3 L;
		float  atten = 1.0;

		if (type == 0) // directional
			L = normalize(-l.dirSpot.xyz);
		else
		{
			float3 d    = l.posType.xyz - in.worldPos;
			float  dist = max(length(d), 1e-4);
			L = d / dist;
			float range = max(l.params.x, 1e-4);
			atten = clamp(1.0 - dist / range, 0.0, 1.0);
			atten *= atten;
			if (type == 2) // spot cone
			{
				float c       = dot(-L, normalize(l.dirSpot.xyz));
				float cosCone = l.dirSpot.w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
		}

		// Only the (first) directional light casts shadows.
		float sh = (type == 0) ? shadowFactor(scene, in.worldPos, N, L, shadowMap, shadowSmp) : 1.0;

		float diff = max(dot(N, L), 0.0);
		float3 H   = normalize(L + V);
		float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;
		result += (base * diff + float3(spec))
		        * l.colorIntensity.rgb * l.colorIntensity.w * atten * sh;
	}
	return float4(result, 1.0);
}
)MSL";

// Matches the MSL Uniforms struct above (float4x4 is column-major like glm).
struct UnlitUniforms
{
	glm::mat4 mvp;
	glm::mat4 model;
	glm::vec4 color;
	glm::vec4 flags;   // x: hasTexture
};

// Matches the MSL LightGPU/SceneUniforms structs above.
struct LightGPU
{
	glm::vec4 posType;
	glm::vec4 dirSpot;
	glm::vec4 colorIntensity;
	glm::vec4 params;
};
struct SceneUniforms
{
	glm::vec4 cameraPos;
	int32_t   lightCount = 0;
	int32_t   pad0 = 0, pad1 = 0, pad2 = 0;
	LightGPU  lights[8];
	glm::mat4 lightVP = glm::mat4(1.0f);
	int32_t   shadowEnabled = 0;
	int32_t   pad3 = 0, pad4 = 0, pad5 = 0;
};

// Remaps the extractor's GL-convention light projection (depth -1..1) to Metal
// clip space (depth 0..1). Metal NDC y is up like GL, so no y flip here — the
// flip happens when sampling (texture origin is top-left).
static const glm::mat4 kMetalClipFix = glm::mat4(
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.5f, 0.0f,
	0.0f, 0.0f, 0.5f, 1.0f);

MetalRenderer::MetalRenderer()  = default;
MetalRenderer::~MetalRenderer() = default;

void MetalRenderer::Initialize(HE::Window* window)
{
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device)
		throw std::runtime_error("MetalRenderer: MTLCreateSystemDefaultDevice failed");
	m_device = (void*)CFBridgingRetain(device);

	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (!queue)
		throw std::runtime_error("MetalRenderer: newCommandQueue failed");
	m_commandQueue = (void*)CFBridgingRetain(queue);

	CreateTarget(m_primarySdlWindow, m_primaryTarget);
	CreateScenePipeline();
	CreateCubeMesh();

	// Persistent pass descriptor describing the swapchain attachment layout.
	// ImGui_ImplMetal_NewFrame() only inspects attachment formats / sample
	// count, so 1×1 placeholder textures suffice — the real per-frame
	// descriptor carries the actual drawable. Color AND depth must match the
	// scene pass or ImGui builds an incompatible pipeline.
	MTLRenderPassDescriptor* imguiDesc = [MTLRenderPassDescriptor renderPassDescriptor];
	{
		MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kSwapchainFormat width:1 height:1 mipmapped:NO];
		colorDesc.usage = MTLTextureUsageRenderTarget;
		imguiDesc.colorAttachments[0].texture     = [device newTextureWithDescriptor:colorDesc];
		imguiDesc.colorAttachments[0].loadAction  = MTLLoadActionLoad;
		imguiDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

		MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kDepthFormat width:1 height:1 mipmapped:NO];
		depthDesc.usage       = MTLTextureUsageRenderTarget;
		depthDesc.storageMode = MTLStorageModePrivate;
		imguiDesc.depthAttachment.texture = [device newTextureWithDescriptor:depthDesc];
	}
	m_imguiPassDescriptor = (void*)CFBridgingRetain(imguiDesc);

	m_shaderManager.setDevice(m_device);

	Logger::Log(Logger::LogLevel::Info,
		(std::string("MetalRenderer: initialized on ") + [[device name] UTF8String]).c_str());
}

void MetalRenderer::Shutdown()
{
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: shutdown");
	m_shaderManager.cleanup();

	for (auto& [sdlWin, target] : m_secondaryTargets)
		DestroyTarget(target);
	m_secondaryTargets.clear();
	DestroyTarget(m_primaryTarget);

	for (auto& [id, mesh] : m_meshCache)
	{
		if (mesh.vertexBuf) CFBridgingRelease(mesh.vertexBuf);
		if (mesh.indexBuf)  CFBridgingRelease(mesh.indexBuf);
		if (mesh.texture)   CFBridgingRelease(mesh.texture);
	}
	m_meshCache.clear();

	DestroyViewportTarget();
	DrainRetiredTextures();
	if (m_dummyTexture)    { CFBridgingRelease(m_dummyTexture);    m_dummyTexture = nullptr; }
	if (m_linearSampler)   { CFBridgingRelease(m_linearSampler);   m_linearSampler = nullptr; }
	if (m_cubeVertexBuf)   { CFBridgingRelease(m_cubeVertexBuf);   m_cubeVertexBuf = nullptr; }
	if (m_cubeIndexBuf)    { CFBridgingRelease(m_cubeIndexBuf);    m_cubeIndexBuf = nullptr; }
	if (m_scenePipeline)   { CFBridgingRelease(m_scenePipeline);   m_scenePipeline = nullptr; }
	if (m_sceneDepthState) { CFBridgingRelease(m_sceneDepthState); m_sceneDepthState = nullptr; }
	if (m_noDepthState)    { CFBridgingRelease(m_noDepthState);    m_noDepthState = nullptr; }

	if (m_imguiPassDescriptor) { CFBridgingRelease(m_imguiPassDescriptor); m_imguiPassDescriptor = nullptr; }
	if (m_commandQueue)        { CFBridgingRelease(m_commandQueue);        m_commandQueue = nullptr; }
	if (m_device)              { CFBridgingRelease(m_device);              m_device = nullptr; }
	m_primarySdlWindow = nullptr;
}

// ─── Pipeline / mesh setup ────────────────────────────────────────────────────

void MetalRenderer::CreateScenePipeline()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		NSError* error = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kUnlitMSL] options:nil error:&error];
		if (!lib)
			throw std::runtime_error(std::string("MetalRenderer: unlit shader compile failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));

		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction   = [lib newFunctionWithName:@"vertexMain"];
		desc.fragmentFunction = [lib newFunctionWithName:@"fragmentMain"];
		desc.colorAttachments[0].pixelFormat = kSwapchainFormat;
		desc.depthAttachmentPixelFormat      = kDepthFormat;

		id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!pso)
			throw std::runtime_error(std::string("MetalRenderer: pipeline creation failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_scenePipeline = (void*)CFBridgingRetain(pso);

		MTLDepthStencilDescriptor* depthDesc = [[MTLDepthStencilDescriptor alloc] init];
		depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
		depthDesc.depthWriteEnabled    = YES;
		m_sceneDepthState = (void*)CFBridgingRetain([device newDepthStencilStateWithDescriptor:depthDesc]);

		// Overlay (ImGui) draws on top of everything — no depth test/write.
		depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
		depthDesc.depthWriteEnabled    = NO;
		m_noDepthState = (void*)CFBridgingRetain([device newDepthStencilStateWithDescriptor:depthDesc]);

		// 1×1 white dummy — always bound so untextured draws never sample an
		// unbound texture (Metal validation rejects that).
		MTLTextureDescriptor* dummyDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
		dummyDesc.usage       = MTLTextureUsageShaderRead;
		dummyDesc.storageMode = MTLStorageModeShared;
		id<MTLTexture> dummy = [device newTextureWithDescriptor:dummyDesc];
		const uint32_t white = 0xFFFFFFFF;
		[dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
		m_dummyTexture = (void*)CFBridgingRetain(dummy);

		MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
		sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
		sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
		m_linearSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:sampDesc]);
	}
}

void MetalRenderer::EnsureShadowResources()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		// Depth texture, sampled by the scene pass.
		MTLTextureDescriptor* td = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kDepthFormat
			width:m_shadowSize height:m_shadowSize mipmapped:NO];
		td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		m_shadowDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:td]);

		// Depth-only pipeline (no color attachment, depth attachment only).
		NSError* error = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kUnlitMSL] options:nil error:&error];
		if (!lib) { Logger::Log(Logger::LogLevel::Error, "MetalRenderer: shadow shader compile failed"); return; }
		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction             = [lib newFunctionWithName:@"vertexShadow"];
		desc.fragmentFunction           = nil; // depth only
		desc.depthAttachmentPixelFormat = kDepthFormat;
		id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (pso) m_shadowPipeline = (void*)CFBridgingRetain(pso);
		else     Logger::Log(Logger::LogLevel::Error, "MetalRenderer: shadow pipeline creation failed");
	}
}

void MetalRenderer::EncodeShadowMap(void* cmdBufPtr)
{
	if (!m_world || !m_shadowPipeline || !m_shadowDepthTex) return;

	// Re-extract (cheap; same data the scene pass uses) to get the light VP and
	// the visible geometry for the depth pass.
	m_extractor.extract(*m_world, m_renderWorld, 1.0f, &m_editorCamera);
	if (!m_renderWorld.shadow.enabled || m_renderWorld.objects.empty()) return;
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;

	const glm::mat4 lightClip = kMetalClipFix * m_renderWorld.shadow.viewProj;

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		MTLRenderPassDescriptor* sp = [MTLRenderPassDescriptor renderPassDescriptor];
		sp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_shadowDepthTex;
		sp.depthAttachment.loadAction  = MTLLoadActionClear;
		sp.depthAttachment.storeAction = MTLStoreActionStore;
		sp.depthAttachment.clearDepth  = 1.0;

		id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:sp];
		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_shadowPipeline];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		[enc setViewport:(MTLViewport){ 0.0, 0.0, (double)m_shadowSize, (double)m_shadowSize, 0.0, 1.0 }];

		for (uint32_t idx : m_sortedIndices)
		{
			const RenderObject& obj = m_renderWorld.objects[idx];
			UnlitUniforms u;
			u.mvp = lightClip * obj.transform;

			id<MTLBuffer> vbuf; id<MTLBuffer> ibuf; NSUInteger ic;
			if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId))
			{
				vbuf = (__bridge id<MTLBuffer>)mesh->vertexBuf;
				ibuf = (__bridge id<MTLBuffer>)mesh->indexBuf;
				ic   = (NSUInteger)mesh->indexCount;
			}
			else
			{
				vbuf = (__bridge id<MTLBuffer>)m_cubeVertexBuf;
				ibuf = (__bridge id<MTLBuffer>)m_cubeIndexBuf;
				ic   = (NSUInteger)m_cubeIndexCount;
			}
			[enc setVertexBuffer:vbuf offset:0 atIndex:0];
			[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                indexCount:ic
			                 indexType:MTLIndexTypeUInt32
			               indexBuffer:ibuf
			         indexBufferOffset:0];
		}
		[enc endEncoding];
	}
}

void MetalRenderer::CreateCubeMesh()
{
	// Identical geometry to the OpenGL backend's built-in cube:
	// 24 vertices (position + normal per face), interleaved per face pair.
	static const float verts[] = {
		// +X                          // -X
		 0.5f,-0.5f,-0.5f, 1,0,0,      -0.5f,-0.5f, 0.5f,-1,0,0,
		 0.5f, 0.5f,-0.5f, 1,0,0,      -0.5f, 0.5f, 0.5f,-1,0,0,
		 0.5f, 0.5f, 0.5f, 1,0,0,      -0.5f, 0.5f,-0.5f,-1,0,0,
		 0.5f,-0.5f, 0.5f, 1,0,0,      -0.5f,-0.5f,-0.5f,-1,0,0,
		// +Y                          // -Y
		-0.5f, 0.5f,-0.5f, 0,1,0,      -0.5f,-0.5f, 0.5f, 0,-1,0,
		-0.5f, 0.5f, 0.5f, 0,1,0,      -0.5f,-0.5f,-0.5f, 0,-1,0,
		 0.5f, 0.5f, 0.5f, 0,1,0,       0.5f,-0.5f,-0.5f, 0,-1,0,
		 0.5f, 0.5f,-0.5f, 0,1,0,       0.5f,-0.5f, 0.5f, 0,-1,0,
		// +Z                          // -Z
		-0.5f,-0.5f, 0.5f, 0,0,1,       0.5f,-0.5f,-0.5f, 0,0,-1,
		 0.5f,-0.5f, 0.5f, 0,0,1,      -0.5f,-0.5f,-0.5f, 0,0,-1,
		 0.5f, 0.5f, 0.5f, 0,0,1,      -0.5f, 0.5f,-0.5f, 0,0,-1,
		-0.5f, 0.5f, 0.5f, 0,0,1,       0.5f, 0.5f,-0.5f, 0,0,-1,
	};
	static const uint32_t indices[] = {
		 0, 2, 4,  0, 4, 6,    1, 3, 5,  1, 5, 7,   // +X -X
		 8,10,12,  8,12,14,    9,11,13,  9,13,15,   // +Y -Y
		16,18,20, 16,20,22,   17,19,21, 17,21,23,   // +Z -Z
	};
	m_cubeIndexCount = static_cast<int>(sizeof(indices) / sizeof(indices[0]));

	// Expand the 6-float (pos+normal) source data to the pipeline's 8-float
	// vertex layout (pos+normal+uv) with zeroed UVs.
	const size_t vertexCount = sizeof(verts) / (6 * sizeof(float));
	std::vector<float> interleaved;
	interleaved.reserve(vertexCount * 8);
	for (size_t v = 0; v < vertexCount; ++v)
	{
		interleaved.insert(interleaved.end(), &verts[v*6], &verts[v*6] + 6);
		interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	m_cubeVertexBuf = (void*)CFBridgingRetain(
		[device newBufferWithBytes:interleaved.data()
		                    length:interleaved.size() * sizeof(float)
		                   options:MTLResourceStorageModeShared]);
	m_cubeIndexBuf = (void*)CFBridgingRetain(
		[device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared]);
}

// ─── Asset mesh upload ────────────────────────────────────────────────────────

const MetalRenderer::GpuMesh* MetalRenderer::ResolveMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_meshCache.find(assetId); it != m_meshCache.end())
		return &it->second;

	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(assetId);
	if (!asset || asset->vertices.empty() || asset->indices.empty())
		return nullptr;

	// Interleave position + normal + uv (8 floats per vertex), zero-filling
	// missing attributes — must match the MSL VertexIn layout.
	const size_t vertexCount = asset->vertices.size() / 3;
	std::vector<float> interleaved;
	interleaved.reserve(vertexCount * 8);
	for (size_t v = 0; v < vertexCount; ++v)
	{
		interleaved.insert(interleaved.end(),
			{ asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
		if (v * 3 + 2 < asset->normals.size())
			interleaved.insert(interleaved.end(),
				{ asset->normals[v*3+0], asset->normals[v*3+1], asset->normals[v*3+2] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
		if (v * 2 + 1 < asset->uvs.size())
			interleaved.insert(interleaved.end(), { asset->uvs[v*2+0], asset->uvs[v*2+1] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	GpuMesh mesh;
	mesh.indexCount  = static_cast<int>(asset->indices.size());
	mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
	mesh.vertexBuf  = (void*)CFBridgingRetain(
		[device newBufferWithBytes:interleaved.data()
		                    length:interleaved.size() * sizeof(float)
		                   options:MTLResourceStorageModeShared]);
	mesh.indexBuf   = (void*)CFBridgingRetain(
		[device newBufferWithBytes:asset->indices.data()
		                    length:asset->indices.size() * sizeof(uint32_t)
		                   options:MTLResourceStorageModeShared]);

	// Base color texture via the mesh's material (load on demand by path)
	if (!asset->materialPath.empty())
	{
		const HE::UUID matId = m_contentManager->loadAsset(asset->materialPath);
		if (const MaterialAsset* mat = m_contentManager->getMaterial(matId);
		    mat && !mat->texturePaths.empty())
		{
			const HE::UUID texId = m_contentManager->loadAsset(mat->texturePaths[0]);
			if (const TextureAsset* tex = m_contentManager->getTexture(texId);
			    tex && !tex->data.empty() && tex->channels == 4)
			{
				MTLTextureDescriptor* desc = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
					                             width:tex->width
					                            height:tex->height
					                         mipmapped:NO];
				desc.usage       = MTLTextureUsageShaderRead;
				desc.storageMode = MTLStorageModeShared;
				id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
				[texture replaceRegion:MTLRegionMake2D(0, 0, tex->width, tex->height)
				           mipmapLevel:0
				             withBytes:tex->data.data()
				           bytesPerRow:tex->width * 4];
				mesh.texture = (void*)CFBridgingRetain(texture);
			}
		}
	}

	Logger::Log(Logger::LogLevel::Info,
		("MetalRenderer: uploaded mesh '" + asset->name + "' ("
		 + std::to_string(vertexCount) + " verts"
		 + (mesh.texture ? ", textured" : "") + ")").c_str());

	return &m_meshCache.emplace(assetId, mesh).first->second;
}

// ─── Window targets ───────────────────────────────────────────────────────────

void MetalRenderer::CreateTarget(SDL_Window* sdlWin, WindowTarget& out)
{
	SDL_MetalView view = SDL_Metal_CreateView(sdlWin);
	if (!view)
		throw std::runtime_error(std::string("MetalRenderer: SDL_Metal_CreateView failed: ") + SDL_GetError());

	CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
	if (!layer)
	{
		SDL_Metal_DestroyView(view);
		throw std::runtime_error("MetalRenderer: SDL_Metal_GetLayer returned null");
	}

	layer.device             = (__bridge id<MTLDevice>)m_device;
	layer.pixelFormat        = kSwapchainFormat;
	layer.framebufferOnly    = YES;
	layer.displaySyncEnabled = m_vsync;

	out.metalView  = (void*)view;
	out.metalLayer = (__bridge void*)layer; // borrowed — the view keeps it alive
}

void MetalRenderer::DestroyTarget(WindowTarget& target)
{
	if (target.depthTexture)
		CFBridgingRelease(target.depthTexture);
	if (target.metalView)
		SDL_Metal_DestroyView((SDL_MetalView)target.metalView);
	target.metalView    = nullptr;
	target.metalLayer   = nullptr;
	target.depthTexture = nullptr;
}

void MetalRenderer::EnsureDepthTexture(WindowTarget& target, int width, int height)
{
	if (target.depthTexture)
	{
		id<MTLTexture> existing = (__bridge id<MTLTexture>)target.depthTexture;
		if ((int)existing.width == width && (int)existing.height == height)
			return;
		CFBridgingRelease(target.depthTexture);
		target.depthTexture = nullptr;
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kDepthFormat
		                             width:(NSUInteger)width
		                            height:(NSUInteger)height
		                         mipmapped:NO];
	desc.usage       = MTLTextureUsageRenderTarget;
	desc.storageMode = MTLStorageModePrivate;
	target.depthTexture = (void*)CFBridgingRetain([device newTextureWithDescriptor:desc]);
}

// ─── Offscreen viewport target ────────────────────────────────────────────────

void MetalRenderer::SetViewportSize(uint32_t width, uint32_t height)
{
	m_viewportReqW = width;
	m_viewportReqH = height;
}

void* MetalRenderer::GetViewportTexture()
{
	return m_viewportColor;
}

void MetalRenderer::RetireTexture(void* texture)
{
	if (!texture) return;
	// 3 frames: current draw list + Metal's in-flight buffers (triple buffering)
	m_retiredTextures.push_back({ texture, 3 });
}

void MetalRenderer::AgeRetiredTextures()
{
	for (auto it = m_retiredTextures.begin(); it != m_retiredTextures.end(); )
	{
		if (--it->framesLeft <= 0)
		{
			CFBridgingRelease(it->texture);
			it = m_retiredTextures.erase(it);
		}
		else
			++it;
	}
}

void MetalRenderer::DrainRetiredTextures()
{
	for (auto& r : m_retiredTextures)
		CFBridgingRelease(r.texture);
	m_retiredTextures.clear();
}

void MetalRenderer::EnsureViewportTarget()
{
	if (m_viewportColor)
	{
		id<MTLTexture> existing = (__bridge id<MTLTexture>)m_viewportColor;
		if (existing.width == m_viewportReqW && existing.height == m_viewportReqH)
			return;
	}
	DestroyViewportTarget();

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSwapchainFormat
		                             width:m_viewportReqW
		                            height:m_viewportReqH
		                         mipmapped:NO];
	colorDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	colorDesc.storageMode = MTLStorageModePrivate;
	m_viewportColor = (void*)CFBridgingRetain([device newTextureWithDescriptor:colorDesc]);

	MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kDepthFormat
		                             width:m_viewportReqW
		                            height:m_viewportReqH
		                         mipmapped:NO];
	depthDesc.usage       = MTLTextureUsageRenderTarget;
	depthDesc.storageMode = MTLStorageModePrivate;
	m_viewportDepth = (void*)CFBridgingRetain([device newTextureWithDescriptor:depthDesc]);
}

void MetalRenderer::DestroyViewportTarget()
{
	// Deferred release — the ImGui draw list recorded this frame and the
	// GPU's in-flight work may still reference these textures.
	RetireTexture(m_viewportColor); m_viewportColor = nullptr;
	RetireTexture(m_viewportDepth); m_viewportDepth = nullptr;
}

// ─── Frame encoding ───────────────────────────────────────────────────────────

void MetalRenderer::EncodeScene(void* renderEncoder, int width, int height)
{
	if (!m_world || !m_scenePipeline || width <= 0 || height <= 0)
		return;

	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height),
	                    &m_editorCamera);
	if (m_renderWorld.objects.empty())
		return;

	// ── Refine bounds with real mesh AABBs (also uploads new meshes) ────────
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// ── Cull → sort → submit ────────────────────────────────────────────────
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty())
		return;

	const glm::mat4 viewProj =
		m_renderWorld.camera.projection * m_renderWorld.camera.view;

	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_scenePipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];

	// ── Lights (clamped to the shader's 8) ──────────────────────────────────
	{
		SceneUniforms scene;
		scene.cameraPos  = glm::vec4(m_renderWorld.camera.position, 1.0f);
		scene.lightCount = std::min(static_cast<int>(m_renderWorld.lights.size()), 8);
		for (int i = 0; i < scene.lightCount; ++i)
		{
			const LightData& l = m_renderWorld.lights[i];
			scene.lights[i].posType        = glm::vec4(l.position,  static_cast<float>(l.type));
			scene.lights[i].dirSpot        = glm::vec4(l.direction, l.spotAngleCos);
			scene.lights[i].colorIntensity = glm::vec4(l.color,     l.intensity);
			scene.lights[i].params         = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
		}
		[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
	}

	// Build this frame's draw calls through the render graph, then replay them.
	// GeometryPass turns the sorted visible objects into DrawCalls; the encoder
	// state (pipeline, lights, camera) is set up above and the meshes are
	// resolved by UUID here, exactly as the immediate loop used to.
	if (m_renderGraph.empty())
		m_renderGraph.addPass(std::make_unique<GeometryPass>());

	// Per-pass sink: bind the pass's target, then replay its draws. Today the
	// only pass renders to the backbuffer (the active scene encoder); offscreen
	// targets (id != backbuffer) arrive with shadows/HDR.
	m_renderGraph.execute(m_renderWorld, m_sortedIndices,
		[&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
	{
		if (io.output.id != kBackbufferTarget) return;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			UnlitUniforms u;
			u.mvp   = viewProj * dc.transform;
			u.model = dc.transform;
			u.color = glm::vec4(0.85f, 0.55f, 0.25f, 1.0f);

			// Resolve the asset; entities without one fall back to the built-in cube.
			id<MTLBuffer> vertexBuf;
			id<MTLBuffer> indexBuf;
			NSUInteger    indexCount;
			id<MTLTexture> texture;
			if (const GpuMesh* mesh = ResolveMesh(dc.meshAssetId))
			{
				vertexBuf  = (__bridge id<MTLBuffer>)mesh->vertexBuf;
				indexBuf   = (__bridge id<MTLBuffer>)mesh->indexBuf;
				indexCount = (NSUInteger)mesh->indexCount;
				texture    = mesh->texture
					? (__bridge id<MTLTexture>)mesh->texture
					: (__bridge id<MTLTexture>)m_dummyTexture;
				u.flags    = glm::vec4(mesh->texture ? 1.0f : 0.0f, 0, 0, 0);
			}
			else
			{
				vertexBuf  = (__bridge id<MTLBuffer>)m_cubeVertexBuf;
				indexBuf   = (__bridge id<MTLBuffer>)m_cubeIndexBuf;
				indexCount = (NSUInteger)m_cubeIndexCount;
				texture    = (__bridge id<MTLTexture>)m_dummyTexture;
				u.flags    = glm::vec4(0.0f);
			}

			[encoder setVertexBuffer:vertexBuf offset:0 atIndex:0];
			[encoder setVertexBytes:&u length:sizeof(u) atIndex:1];
			[encoder setFragmentTexture:texture atIndex:0];
			[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                    indexCount:indexCount
			                     indexType:MTLIndexTypeUInt32
			                   indexBuffer:indexBuf
			             indexBufferOffset:0];
		}
	});
}

void MetalRenderer::EncodeFrame(SDL_Window* sdlWin, WindowTarget& target, bool isPrimary)
{
	@autoreleasepool
	{
		if (isPrimary)
			AgeRetiredTextures();

		CAMetalLayer* layer = (__bridge CAMetalLayer*)target.metalLayer;

		// Keep the drawable size in sync with the window's pixel size (HiDPI / resize)
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(sdlWin, &pw, &ph);
		if (pw <= 0 || ph <= 0) return;
		{
			CGSize size = layer.drawableSize;
			if ((int)size.width != pw || (int)size.height != ph)
				layer.drawableSize = CGSizeMake(pw, ph);
		}
		EnsureDepthTexture(target, pw, ph);

		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (!drawable) return; // window hidden/minimized — skip the frame

		id<MTLCommandQueue>  queue   = (__bridge id<MTLCommandQueue>)m_commandQueue;
		id<MTLCommandBuffer> cmdBuf  = [queue commandBuffer];

		// ── Offscreen scene pass (editor viewport) ──────────────────────────
		const bool offscreen = isPrimary && m_viewportReqW > 0 && m_viewportReqH > 0;
		if (offscreen)
		{
			EnsureViewportTarget();

			MTLRenderPassDescriptor* scenePass = [MTLRenderPassDescriptor renderPassDescriptor];
			scenePass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_viewportColor;
			scenePass.colorAttachments[0].loadAction  = MTLLoadActionClear;
			scenePass.colorAttachments[0].storeAction = MTLStoreActionStore;
			scenePass.colorAttachments[0].clearColor  = MTLClearColorMake(0.18, 0.18, 0.20, 1.0);
			scenePass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_viewportDepth;
			scenePass.depthAttachment.loadAction  = MTLLoadActionClear;
			scenePass.depthAttachment.storeAction = MTLStoreActionDontCare;
			scenePass.depthAttachment.clearDepth  = 1.0;

			id<MTLRenderCommandEncoder> sceneEncoder =
				[cmdBuf renderCommandEncoderWithDescriptor:scenePass];
			EncodeScene((__bridge void*)sceneEncoder,
			            (int)m_viewportReqW, (int)m_viewportReqH);
			[sceneEncoder endEncoding];
		}
		else if (isPrimary && m_viewportColor)
			DestroyViewportTarget();

		// ── Swapchain pass (direct scene and/or overlay) ────────────────────
		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture     = drawable.texture;
		pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.18, 0.18, 0.20, 1.0);
		pass.depthAttachment.texture     = (__bridge id<MTLTexture>)target.depthTexture;
		pass.depthAttachment.loadAction  = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionDontCare;
		pass.depthAttachment.clearDepth  = 1.0;

		id<MTLRenderCommandEncoder> encoder = [cmdBuf renderCommandEncoderWithDescriptor:pass];

		// ── Scene direct to window (primary, no offscreen viewport) ─────────
		if (isPrimary && !offscreen)
			EncodeScene((__bridge void*)encoder, pw, ph);

		// ── Overlay (ImGui) ─────────────────────────────────────────────────
		if (isPrimary && m_overlayCallback)
		{
			[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
			MetalOverlayContext ctx{
				(__bridge void*)cmdBuf,
				(__bridge void*)encoder,
				(__bridge void*)pass,
			};
			m_overlayCallback(&ctx);
		}

		[encoder endEncoding];
		[cmdBuf presentDrawable:drawable];
		[cmdBuf commit];
	}
}

void MetalRenderer::Render()
{
	if (!m_primarySdlWindow || !m_primaryTarget.metalLayer) return;
	EncodeFrame(m_primarySdlWindow, m_primaryTarget, /*isPrimary=*/true);
}

IRenderer::Capabilities MetalRenderer::GetCapabilities() const
{
	return { true, true, true };
}

void MetalRenderer::SetVSync(bool enabled)
{
	m_vsync = enabled;
	if (m_primaryTarget.metalLayer)
		((__bridge CAMetalLayer*)m_primaryTarget.metalLayer).displaySyncEnabled = enabled;
	for (auto& [sdlWin, target] : m_secondaryTargets)
		((__bridge CAMetalLayer*)target.metalLayer).displaySyncEnabled = enabled;
}

// ─── Multi-window support ─────────────────────────────────────────────────────

void MetalRenderer::AttachWindow(HE::Window* window)
{
	SDL_Window* sdlWin = window->GetNativeWindow();
	if (m_secondaryTargets.count(sdlWin)) return; // already attached

	WindowTarget target;
	CreateTarget(sdlWin, target);
	m_secondaryTargets[sdlWin] = target;
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: secondary window attached");
}

void MetalRenderer::DetachWindow(HE::Window* window)
{
	auto it = m_secondaryTargets.find(window->GetNativeWindow());
	if (it == m_secondaryTargets.end()) return;
	DestroyTarget(it->second);
	m_secondaryTargets.erase(it);
	Logger::Log(Logger::LogLevel::Info, "MetalRenderer: secondary window detached");
}

void MetalRenderer::RenderWindow(HE::Window* window)
{
	auto it = m_secondaryTargets.find(window->GetNativeWindow());
	if (it == m_secondaryTargets.end()) return;
	EncodeFrame(window->GetNativeWindow(), it->second, /*isPrimary=*/false);
}

// ─── ImGui texture helpers ────────────────────────────────────────────────────

void* MetalRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	if (!m_device || !rgba8Pixels || width <= 0 || height <= 0) return nullptr;

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		MTLTextureDescriptor* desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			                             width:(NSUInteger)width
			                            height:(NSUInteger)height
			                         mipmapped:NO];
		desc.usage       = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;

		id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
		if (!texture) return nullptr;

		[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
		           mipmapLevel:0
		             withBytes:rgba8Pixels
		           bytesPerRow:(NSUInteger)width * 4];

		// Retained — released in DestroyImGuiTexture. The pointer doubles as
		// the ImTextureID the editor hands to ImGui_ImplMetal_RenderDrawData.
		return (void*)CFBridgingRetain(texture);
	}
}

void MetalRenderer::DestroyImGuiTexture(void* handle)
{
	if (handle) CFBridgingRelease(handle);
}

// ─── Accessors ────────────────────────────────────────────────────────────────

void* MetalRenderer::GetDevice() const              { return m_device; }
void* MetalRenderer::GetCommandQueue() const        { return m_commandQueue; }
void* MetalRenderer::GetFramePassDescriptor() const { return m_imguiPassDescriptor; }
