#include "Backends/Metal/MetalRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/ParticleShaderTemplates.h>
#include <HorizonRendering/ClipSpace.h>       // HE::kMetalClipFix
#include <HorizonRendering/LightPacking.h>    // HE::BuildPackedLightArray / BuildMaskedLocalLights
#include <HorizonRendering/WeatherParticleSeed.h> // shared rain/snow pool seeding
#include <HorizonRendering/WorldPreviewGrid.h>    // shared world-preview ground + grid
#include <HorizonRendering/SkyEnvBake.h>      // shared CPU sky bake for the IBL ambient cubemap
#include <JobSystem/JobSystem.h>              // parallel_for — the bake is per-row parallel
#include <HorizonRendering/SkyFrameParams.h>  // HE::SkyFrameParams / BuildSkyFrameParams (folds in CloudWindVector)
#include <HorizonRendering/SkyNoise3D.h>      // HE::BuildSkyNoise3D
#include <HorizonRendering/SsaoKernel.h>      // HE::BuildSSAOKernel / BuildSSAONoise
#include <MaterialGraph/MaterialGraph.h> // kMatMaxGraphTextures
#include <HorizonRendering/GiInstanceSurface.h> // shared GI-hit surface resolution
#include <Renderer/UIFont.h>             // shared baked UI font atlas
#include <material/PreviewMesh.h> // shared preview primitives (sphere/cube/plane)
#include <Diagnostics/Logger.h>
#include <cstdlib> // std::getenv / atoi / atof (HE_* debug + capture knobs)
#include <Diagnostics/EngineProfiler.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <fstream> // HE_DUMP_* headless capture writes PNG/PPM dumps straight to disk
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp> // glm::lookAt / glm::perspective (preview cameras)
#include <glm/gtc/type_ptr.hpp>         // glm::make_mat4 (skeletal-preview bone overlay)
#include <simd/simd.h>

#import <Metal/Metal.h>
// MetalFX (A5): weak-linked, and its presence is a COMPILE-time question as well
// as a runtime one — an older SDK has no header at all. Everything guarded by
// this macro degrades to "MetalFX unsupported", which resolves to our own TAA.
#if __has_include(<MetalFX/MetalFX.h>)
	#import <MetalFX/MetalFX.h>
	#define HE_HAS_METALFX 1
#else
	#define HE_HAS_METALFX 0
#endif
#import <QuartzCore/CAMetalLayer.h>
#include <cstdint>
#include <cstring>

// ── Per-pass GPU timing helpers (Metal stage-boundary counter sampling) ──────
namespace {
// Generous fixed sample-buffer capacity so EncodeFrame can hand out timer slots
// dynamically without pre-counting the active passes; unused slots are simply
// never resolved. Stage pairs (2 slots each: Shadow/SSAO/Scene/Bloom/Tonemap/
// Present) plus the ~7 intra-Scene draw-boundary points fit comfortably (~19).
constexpr NSUInteger kMaxGpuSamples = 32;
constexpr uint32_t   kInvalidSlot   = 0xFFFFFFFFu;   // ftPair/ftPoint when over capacity
// Detailed capture submits the frame as this many command buffers, one per pass:
// Shadow, GIAccel, GIShadow, GIProbes, SSAO, Scene, Bloom, Tonemap, Present
// (some may be empty → 0 ms).
constexpr int        kDetailedPassCount = 9;
// Cascaded shadow maps: layer count of the shadow depth-texture array. Must match
// the extractor's kCascadeCount and the shader's cascade arrays (3).
constexpr int        kCsmCascades = 3;

// Make a render pass descriptor sample the GPU timestamp at start-of-vertex and
// end-of-fragment, so (end - start) is the pass's GPU duration. No-op if sb nil.
// Used for single-encoder passes (Shadow / Scene / Tonemap / Present).
API_AVAILABLE(macos(11.0))
void attachPassTimer(MTLRenderPassDescriptor* d, id<MTLCounterSampleBuffer> sb, NSUInteger base)
{
	if (!sb) return;
	MTLRenderPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
	a.sampleBuffer               = sb;
	a.startOfVertexSampleIndex   = base;
	a.endOfVertexSampleIndex     = MTLCounterDontSample;
	a.startOfFragmentSampleIndex = MTLCounterDontSample;
	a.endOfFragmentSampleIndex   = base + 1;
}
// Half-timers for a MULTI-encoder pass (SSAO = pos/occlusion/blur; Bloom =
// bright + N blur): start sampled on the FIRST encoder, end on the LAST, so the
// pair spans the whole feature.
API_AVAILABLE(macos(11.0))
void attachPassStart(MTLRenderPassDescriptor* d, id<MTLCounterSampleBuffer> sb, NSUInteger slot)
{
	if (!sb) return;
	MTLRenderPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
	a.sampleBuffer               = sb;
	a.startOfVertexSampleIndex   = slot;
	a.endOfVertexSampleIndex     = MTLCounterDontSample;
	a.startOfFragmentSampleIndex = MTLCounterDontSample;
	a.endOfFragmentSampleIndex   = MTLCounterDontSample;
}
API_AVAILABLE(macos(11.0))
void attachPassEnd(MTLRenderPassDescriptor* d, id<MTLCounterSampleBuffer> sb, NSUInteger slot)
{
	if (!sb) return;
	MTLRenderPassSampleBufferAttachmentDescriptor* a = d.sampleBufferAttachments[0];
	a.sampleBuffer               = sb;
	a.startOfVertexSampleIndex   = MTLCounterDontSample;
	a.endOfVertexSampleIndex     = MTLCounterDontSample;
	a.startOfFragmentSampleIndex = MTLCounterDontSample;
	a.endOfFragmentSampleIndex   = slot;
}
} // namespace

// ─── Per-frame GPU timer slot allocation + attachment helpers ─────────────────
// All no-ops unless a capture with stage/draw timing is active this frame.
uint32_t MetalRenderer::ftPair(const char* name)
{
	if (m_ft.next + 2 > kMaxGpuSamples) return kInvalidSlot;
	uint32_t base = m_ft.next; m_ft.next += 2;
	m_ft.pairs.push_back({ name, base });
	return base;
}
uint32_t MetalRenderer::ftPoint(const char* name)
{
	if (m_ft.next + 1 > kMaxGpuSamples) return kInvalidSlot;
	uint32_t slot = m_ft.next++;
	m_ft.points.push_back({ name, slot });
	return slot;
}
// Single-encoder pass: reserve a pair and attach start+end to its descriptor.
void MetalRenderer::ftAttachPass(void* passDescPtr, const char* name)
{
	if (!m_ft.stage || !m_ft.sampleBuf || !passDescPtr) return;
	if (@available(macOS 11.0, *))
	{
		uint32_t base = ftPair(name);
		if (base != kInvalidSlot)
			attachPassTimer((__bridge MTLRenderPassDescriptor*)passDescPtr,
			                (__bridge id<MTLCounterSampleBuffer>)m_ft.sampleBuf, base);
	}
}
// Multi-encoder pass: reserve a pair up front (returns base, or kInvalidSlot),
// then ftAttachStart on the first descriptor and ftAttachEnd on the last.
uint32_t MetalRenderer::ftBeginMulti(const char* name)
{
	if (!m_ft.stage || !m_ft.sampleBuf) return kInvalidSlot;
	return ftPair(name);
}
void MetalRenderer::ftAttachStart(void* passDescPtr, uint32_t base)
{
	if (base == kInvalidSlot || !m_ft.sampleBuf || !passDescPtr) return;
	if (@available(macOS 11.0, *))
		attachPassStart((__bridge MTLRenderPassDescriptor*)passDescPtr,
		                (__bridge id<MTLCounterSampleBuffer>)m_ft.sampleBuf, base);
}
void MetalRenderer::ftAttachEnd(void* passDescPtr, uint32_t base)
{
	if (base == kInvalidSlot || !m_ft.sampleBuf || !passDescPtr) return;
	if (@available(macOS 11.0, *))
		attachPassEnd((__bridge MTLRenderPassDescriptor*)passDescPtr,
		              (__bridge id<MTLCounterSampleBuffer>)m_ft.sampleBuf, base + 1);
}
// Draw-boundary sample inside one render encoder (intra-Scene element split).
void MetalRenderer::SamplePoint(void* encoderPtr, const char* name)
{
	if (!m_ft.draw || !m_ft.sampleBuf || !encoderPtr) return;
	if (@available(macOS 11.0, *))
	{
		uint32_t slot = ftPoint(name);
		if (slot != kInvalidSlot)
			[(__bridge id<MTLRenderCommandEncoder>)encoderPtr
			    sampleCountersInBuffer:(__bridge id<MTLCounterSampleBuffer>)m_ft.sampleBuf
			              atSampleIndex:slot
			                withBarrier:NO];
	}
}

// The CPU port of the shader skyColor(dir,sunDir) that bakes the image-based-
// ambient cubemap — 88 lines that used to be duplicated verbatim in the GL
// backend — now lives in HorizonRendering/SkyEnvBake.h. Both backends call
// HE::BuildSkyEnvFace / HE::SkyColorCPU from there; the one caller in this file
// is MetalRenderer::UpdateSkyEnvCube.

// Swapchain / depth formats shared by every window target, the scene
// pipeline and the ImGui pass descriptor — they must all match.
static constexpr MTLPixelFormat kSwapchainFormat = MTLPixelFormatBGRA8Unorm;
static constexpr MTLPixelFormat kDepthFormat     = MTLPixelFormatDepth32Float;
static constexpr MTLPixelFormat kSceneColorFormat = MTLPixelFormatRGBA16Float; // HDR scene color
// Deferred G-buffer layout (docs/deferred-renderer-plan.md §3): BaseColor+Metallic
// in sRGB8, oct-Normal/Roughness/Specular and HDR-Emissive/AO in RGBA16F.
static constexpr MTLPixelFormat kGBuf0Format    = MTLPixelFormatRGBA8Unorm_sRGB;
static constexpr MTLPixelFormat kGBufAttrFormat = MTLPixelFormatRGBA16Float;

// GPU instancing (A3 contract, docs/gpu-instancing-cross-backend-plan.md §3):
// the same capacity and the same 128-byte {mvp, model} stride D3D11/D3D12/Vulkan
// use, so the layout is one decision rather than four.
static constexpr uint32_t k_maxInstances = 65536; // per-batch instance ceiling
static constexpr size_t   k_instStride   = 128;   // bytes per instance = 2 × float4x4

// HE_MTL_INSTANCING=0 forces every batch back onto the per-instance loop. The
// fallback is the pre-A3 code path unchanged, so this is the A/B that answers
// "does the instanced draw put the pixels where the loop did?" — the one
// question a screenshot alone cannot settle. Read once.
static bool metalInstancingEnabled()
{
	static const bool on = []{
		const char* v = std::getenv("HE_MTL_INSTANCING");
		return !(v && *v && std::atoi(v) == 0);
	}();
	return on;
}

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
	float4   color;   // rgb: base-color tint
	// x: hasTexture
	// y: 1 = this object IGNORES shadows (MeshComponent::receivesShadow == false).
	//    Stored inverted on purpose: every fill site that zero-fills `flags` —
	//    previews, thumbnails, the shadow depth pass — then keeps shadowing.
	float4   flags;
	float4   pbr;     // x: metallic, y: roughness
};

struct LightGPU {
	float4 posType;        // xyz = position,  w = type (0 dir / 1 point / 2 spot)
	float4 dirSpot;        // xyz = direction, w = cos(spot half angle)
	float4 colorIntensity; // rgb = color,     w = intensity
	float4 params;         // x = range, y = local shadow base layer (-1 = none)
};

struct SceneUniforms {
	float4   cameraPos;    // xyz used
	float4   cameraFwd;    // xyz = world forward (planar view-Z cascade select)
	int      lightCount;
	int      pad0, pad1, pad2;
	LightGPU lights[8];
	float4x4 cascadeVP[3];   // CSM: per-cascade light view-proj (already Metal clip)
	float4   cascadeSplits;  // xyz = cascade far distance (view space); w = count
	int      shadowEnabled;
	int      debugCascades;  // 1 = tint fragments by cascade index
	int      pad3, pad4;
	float4   sunDir;         // xyz = direction toward the sun (image-based ambient)
	float4   ambient;        // xyz = flat ambient fill (floor + overcast); w unused
	float4   fog;            // x = density (0 = off), y = height falloff
	float4   viewport;       // xy = output size (screen-space AO lookup), z = ssaoEnabled
	float4   weather;        // x = wetness, y = snow cover (ground response)
	// FORWARD reflection cascade (textures 9/10): x = SSR bound, y = SSR
	// intensity, z = SSR max roughness, w = GI-refl bound.
	float4   reflCfg;
	float4   reflCfg2;       // x = GI-refl intensity, y = GI-refl max roughness
	// Local-light (point/spot) shadow atlas: per-layer light view-proj (already
	// Metal clip). Spot = 1 layer, point = 6 cube-face layers (+X −X +Y −Y +Z −Z);
	// a light's first layer is in its LightGPU.params.y.
	float4x4 localShadowVP[16];
	// Cloud shadows (texture 16): the sky's cloud layer projected along the sun.
	//   cloudShadowA: xy = region origin (world XZ), z = 1/region size,
	//                 w = cloud-slab mid-plane world Y
	//   cloudShadowB: x = strength (0 = off)
	float4   cloudShadowA;
	float4   cloudShadowB;
	// Specular AA (docs/anti-aliasing-plan.md A6): x = strength (0 = off).
	float4   aaParams;
};

// Widen the roughness by how much the normal turns inside this pixel, so a
// highlight on a curved or normal-mapped surface stops crawling (Kaplanyan /
// Filament normal filtering). Only valid where `N` is the fragment's OWN
// interpolated normal — the forward pass and the G-buffer pass, never the
// deferred resolve, whose normal comes from a texel and jumps at silhouettes.
// SYNC: byte-identical twins in the GL scene + G-buffer shaders and in
// MaterialShaderLibrary's preamble (heSpecAARoughness).
static float specAARoughness(float3 N, float perceptualRough, float strength)
{
	if (strength <= 0.0) return perceptualRough;
	float3 du = dfdx(N);
	float3 dv = dfdy(N);
	float variance = 0.15 * strength * (dot(du, du) + dot(dv, dv));
	// NOT named `kernel`: that is a reserved function qualifier in MSL, and the
	// twins are kept identical, so the name is avoided in all of them.
	float kernelRough = min(2.0 * variance, 0.25);       // cap: never fully diffuse
	float alpha       = perceptualRough * perceptualRough; // GGX alpha
	float widened     = clamp(alpha * alpha + kernelRough, 0.0, 1.0);
	return max(perceptualRough, sqrt(sqrt(widened)));
}

// GI (Checkpoint B/C): fragmentMain buffer(3). Deliberately separate from
// SceneUniforms — buffer(1) is claimed by the custom-material lighting path
// (HE::MaterialShaderLibrary::kMetalLightingBufferIndex).
struct GIUniforms {
	int enabled; int pad0, pad1, pad2;
	float4 gridOrigin; // xyz = world-space probe-grid origin, w = spacing
	float4 gridCounts; // xyz = probe counts per axis, w = probesPerRow (atlas tile layout)
	float4 params;     // x = indirectIntensity, y/z/w reserved
};

// shared skyColor() injected at the marker below (newLibraryWithSource)
//#SKYFUNC#

struct VSOut {
	float4 position [[position]];
	float3 normal;
	float2 uv;
	float3 worldPos;
	float3 color;
	float  hasTexture;
	float  metallic;
	float  roughness;
	float  opacity;
	float  noShadow;   // u.flags.y — constant over the draw, so interpolation is a no-op
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
	out.metallic   = u.pbr.x;
	out.roughness  = u.pbr.y;
	out.opacity    = u.pbr.z;
	out.noShadow   = u.flags.y;
	return out;
}

// GPU instancing (docs/gpu-instancing-cross-backend-plan.md §4): one transform
// pair per instance in a device array at buffer 5, indexed by [[instance_id]].
// 128 bytes per instance, {mvp, model}, column-major — byte-identical to the
// D3D11/D3D12/Vulkan instance stride (k_instStride), so the layout is one
// contract across all four backends.
struct InstXform {
	float4x4 mvp;
	float4x4 model;
};

// vertexMain with the two per-object matrices taken from the instance array
// instead of Uniforms. Everything else — color, flags, pbr — stays in `u`,
// because GeometryPass only batches draws over which those are constant.
// VSOut is unchanged, so fragmentMain and gbufferMain are reused as they are.
vertex VSOut vertexMainInstanced(uint vid [[vertex_id]],
                                 uint iid [[instance_id]],
                                 const device VertexIn*  verts [[buffer(0)]],
                                 constant Uniforms&      u     [[buffer(1)]],
                                 const device InstXform* inst  [[buffer(5)]])
{
	VSOut out;
	float4x4 model = inst[iid].model;
	float4 world   = model * float4(float3(verts[vid].position), 1.0);
	out.position   = inst[iid].mvp * float4(float3(verts[vid].position), 1.0);
	out.worldPos   = world.xyz;
	float3x3 m3    = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
	out.normal     = m3 * float3(verts[vid].normal);
	out.uv         = float2(verts[vid].uv);
	out.color      = u.color.rgb;
	out.hasTexture = u.flags.x;
	out.metallic   = u.pbr.x;
	out.roughness  = u.pbr.y;
	out.opacity    = u.pbr.z;
	out.noShadow   = u.flags.y;
	return out;
}

// Depth-only vertex shader for the shadow pass: u.mvp carries lightVP * model.
vertex float4 vertexShadow(uint vid [[vertex_id]],
                           const device VertexIn* verts [[buffer(0)]],
                           constant Uniforms&     u     [[buffer(1)]])
{
	return u.mvp * float4(float3(verts[vid].position), 1.0);
}

// Linear blend-skinning vertex shader. Bone matrices arrive in a dedicated buffer
// (buffer 4) so they are not limited by the 4 KB setVertexBytes ceiling.
// Outputs the same VSOut as vertexMain so fragmentMain is reused unchanged.
vertex VSOut skinnedVertex(uint vid [[vertex_id]],
                           const device VertexIn*   verts       [[buffer(0)]],
                           constant Uniforms&        u           [[buffer(1)]],
                           const device uint4*       boneIds     [[buffer(2)]],
                           const device float4*      boneWeights [[buffer(3)]],
                           constant float4x4*        boneMats    [[buffer(4)]])
{
    uint4  ids  = boneIds[vid];
    float4 wgts = boneWeights[vid];
    float4x4 skin = wgts.x * boneMats[ids.x]
                  + wgts.y * boneMats[ids.y]
                  + wgts.z * boneMats[ids.z]
                  + wgts.w * boneMats[ids.w];
    float4 skinnedPos = skin * float4(float3(verts[vid].position), 1.0);
    float4 world      = u.model * skinnedPos;
    float3x3 m3 = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz);
    float3x3 s3 = float3x3(skin[0].xyz,    skin[1].xyz,    skin[2].xyz);
    VSOut out;
    out.position   = u.mvp * skinnedPos;
    out.worldPos   = world.xyz;
    out.normal     = normalize(m3 * (s3 * float3(verts[vid].normal)));
    out.uv         = float2(verts[vid].uv);
    out.color      = u.color.rgb;
    out.hasTexture = u.flags.x;
    out.metallic   = u.pbr.x;
    out.roughness  = u.pbr.y;
    out.opacity    = u.pbr.z;
    out.noShadow   = u.flags.y;
    return out;
}

// Cascaded shadows: pick the first cascade whose far distance covers the fragment
// (by camera distance), project into that cascade's light clip and 3×3-PCF sample
// its layer of the shadow-map array. outCascade returns the chosen index (debug).
float shadowFactor(constant SceneUniforms& scene, float3 worldPos, float3 N, float3 L,
                   float viewDist, texture2d_array<float> shadowMap, sampler shadowSmp,
                   thread int& outCascade)
{
	outCascade = 0;
	if (scene.shadowEnabled == 0) return 1.0;
	const int count = int(scene.cascadeSplits.w);
	// Pick the first cascade whose far distance covers this fragment. Explicit .x/.y/.z
	// (not dynamic vector indexing, which is unreliable in MSL).
	int c = (count > 0) ? count - 1 : 0;
	if      (count > 0 && viewDist < scene.cascadeSplits.x) c = 0;
	else if (count > 1 && viewDist < scene.cascadeSplits.y) c = 1;
	else if (count > 2 && viewDist < scene.cascadeSplits.z) c = 2;
	c = clamp(c, 0, 2);
	outCascade = c;

	// Normal-offset bias scaled by cascade — coarser (farther) cascades have larger
	// texels and need a bigger offset to avoid acne.
	float4 lp = scene.cascadeVP[c] * float4(worldPos + N * (0.06 * float(c + 1)), 1.0);
	float3 p  = lp.xyz / lp.w;            // z already [0,1] (Metal clip); xy in [-1,1]
	float2 uv = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5)); // tex origin top-left
	float2 texel = 1.0 / float2(shadowMap.get_width(), shadowMap.get_height());
	// Reject one texel inside the border so the 3×3 PCF kernel never reads outside
	// this cascade (clamped/neighbour texels → edge fringes).
	if (p.z > 1.0 || any(uv < texel) || any(uv > 1.0 - texel)) return 1.0;
	float ndl  = clamp(dot(N, L), 0.0, 1.0);
	float bias = clamp(0.0008 * tan(acos(ndl)), 0.0002, 0.02) * float(c + 1);
	// 3×3 PCF over the chosen cascade's array layer.
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float cd = shadowMap.sample(shadowSmp, uv + float2(x, y) * texel, uint(c)).r;
			vis += (p.z - bias > cd) ? 0.0 : 1.0;
		}
	return vis / 9.0;
}

// Point/spot shadow lookup in the local shadow atlas. Spot lights project into
// their single perspective layer; point lights first pick the cube face from the
// fragment→light vector's major axis (faces stored as 6 consecutive array layers,
// +X −X +Y −Y +Z −Z), then project into that face's layer. Same 3×3 PCF and
// normal-offset bias family as the directional CSM above.
float localShadowFactor(constant SceneUniforms& scene, constant LightGPU& l,
                        float3 worldPos, float3 N,
                        texture2d_array<float> localMap, sampler shadowSmp)
{
	int base = int(l.params.y);
	if (base < 0) return 1.0;
	int layer = base;
	if (int(l.posType.w) == 1) // point: major-axis cube-face pick
	{
		float3 d = worldPos - l.posType.xyz;
		float3 a = abs(d);
		int face;
		if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
		else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
		else                               face = (d.z > 0.0) ? 4 : 5;
		layer = base + face;
	}
	float3 toL  = normalize(l.posType.xyz - worldPos);
	float  ndl  = clamp(dot(N, toL), 0.0, 1.0);
	float4 lp = scene.localShadowVP[layer] * float4(worldPos + N * 0.02, 1.0);
	if (lp.w <= 0.0) return 1.0;                  // behind the light's near plane
	float3 p  = lp.xyz / lp.w;                    // z in [0,1] (Metal clip)
	float2 uv = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
	float2 texel = 1.0 / float2(localMap.get_width(), localMap.get_height());
	if (p.z > 1.0 || p.z < 0.0 || any(uv < texel) || any(uv > 1.0 - texel)) return 1.0;
	float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float cd = localMap.sample(shadowSmp, uv + float2(x, y) * texel, uint(layer)).r;
			vis += (p.z - bias > cd) ? 0.0 : 1.0;
		}
	return vis / 9.0;
}

// Standard signed-octahedral mapping (Meyer et al. 2010) — direction -> texel
// UV. Duplicated from kGIProbeMSL's octDecode (direction<-texel): each embedded
// MSL string is compiled as its own library, no shared headers between them.
static float2 octEncode(float3 n)
{
	float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

constant int kGIProbeOctSizeShade = 8; // must match MetalRenderer::kGIProbeOctSize

// DDGI probe sampling (Majercik et al. 2019, minus adaptive probe relocation/
// classification and bicubic filtering — v1 simplifications). Trilinearly
// blends the 8 probes surrounding P, weighted by a soft backface term and a
// Chebyshev visibility test (suppresses light leaking through thin occluders).
// NOTE on what "irradiance" means here: EncodeGIProbeUpdate stores ONE raw
// radiance sample per octahedral direction (a gather, not the DDGI paper's
// cosine-weighted hemisphere-integral scatter+resolve), so this is closer to a
// small per-probe environment map than true pre-integrated irradiance — still
// directionally correct and energy-plausible (brighter/tinted near lit/coloured
// surfaces), just not radiometrically exact. A documented follow-up, not a bug.
static float3 sampleDDGIIrradiance(constant GIUniforms& gi,
                                   texture2d<float> irrAtlas, sampler irrSmp,
                                   texture2d<float> visAtlas, sampler visSmp,
                                   float3 P, float3 N)
{
	const int gx = int(gi.gridCounts.x), gy = int(gi.gridCounts.y), gz = int(gi.gridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0);
	const int probesPerRow = max(1, int(gi.gridCounts.w));
	const int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
	const float2 atlasSizeTexels = float2(float(probesPerRow), float(probeRows)) * float(kGIProbeOctSizeShade);
	const float spacing = max(gi.gridOrigin.w, 1e-4);

	const float3 gridSpace = (P - gi.gridOrigin.xyz) / spacing;
	const float3 base      = floor(gridSpace);
	const float3 frac      = gridSpace - base;

	float3 sumColor  = float3(0.0);
	float  sumWeight = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		const float3 cell = base + offs;
		if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		const int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

		const float3 trilinear = mix(1.0 - frac, frac, offs);
		float weight = trilinear.x * trilinear.y * trilinear.z;
		if (weight <= 1e-5) continue;

		const float3 probePos  = gi.gridOrigin.xyz + cell * spacing;
		const float3 toProbe   = probePos - P;
		const float  dist      = max(length(toProbe), 1e-4);
		const float3 dirToProbe = toProbe / dist;

		// Soft backface term (never fully zero) — avoids the classic hard-cutoff
		// DDGI seam at the normal's tangent plane.
		weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

		const int   tileX = probeIndex % probesPerRow;
		const int   tileY = probeIndex / probesPerRow;
		const float2 tileOrigin = float2(float(tileX), float(tileY)) * float(kGIProbeOctSizeShade);

		// Visibility (Chebyshev): sample THIS probe's own recorded distance in the
		// direction from the probe toward P (-dirToProbe), compare against the
		// actual probe-to-P distance to down-weight probes that see through walls.
		const float2 visUV = (tileOrigin + (octEncode(-dirToProbe) * 0.5 + 0.5) * float(kGIProbeOctSizeShade)) / atlasSizeTexels;
		const float2 visSample = visAtlas.sample(visSmp, visUV).rg;
		const float mean = visSample.x, mean2 = visSample.y;
		const float variance = abs(mean2 - mean * mean);
		float chebyshev = 1.0;
		if (dist > mean)
		{
			const float d = dist - mean;
			chebyshev = variance / (variance + d * d);
			chebyshev = chebyshev * chebyshev * chebyshev; // cube to sharpen the falloff, per the DDGI paper
		}
		weight *= max(chebyshev, 0.05);

		const float2 irrUV = (tileOrigin + (octEncode(N) * 0.5 + 0.5) * float(kGIProbeOctSizeShade)) / atlasSizeTexels;
		sumColor  += irrAtlas.sample(irrSmp, irrUV).rgb * weight;
		sumWeight += weight;
	}
	return sumColor / max(sumWeight, 1e-4);
}

// Atmospheric fog / aerial perspective (mirrors the GL applyFog()): blend the
// lit colour toward the sky in the fragment's view direction so distant geometry
// melts into the horizon. The opacity is an analytic exponential height-fog
// integral along the view ray (density*exp(-falloff*y)), so fog pools low and
// thins with altitude; falloff == 0 → plain exp distance fog.
float3 applyFog(float3 color, float3 camPos, float3 worldPos, float3 sunDir, float2 fog)
{
	if (fog.x <= 0.0) return color;
	float3 ray  = worldPos - camPos;
	float  dist = length(ray);
	float  k    = fog.y * ray.y;
	float  t    = (abs(k) > 1e-4) ? (1.0 - exp(-k)) / k : 1.0; // mean height attenuation
	float  optical = fog.x * dist * exp(-fog.y * camPos.y) * t;
	float  f       = 1.0 - exp(-optical);
	float3 fogCol  = skyColor(ray / max(dist, 1e-4), sunDir);
	return mix(color, fogCol, clamp(f, 0.0, 1.0));
}

// Cloud-shadow visibility for the directional light (1 = fully lit): project
// the fragment along L (toward the light) onto the cloud slab's mid-plane and
// sample the per-frame transmittance map. Edge fade hides the region border.
// Samples through a constexpr sampler — the map is bound as a TEXTURE only
// (slot 16), matching the material preamble's heCloudShadow pin.
// SYNC: mirror of heCloudShadowFactor (MaterialShaderLibrary.cpp) and the GL
// kUnlitFS cloudShadowFactor — change one, change all.
float cloudShadowFactor(constant SceneUniforms& scene, float3 worldPos, float3 L,
                        texture2d<float> cloudShadowTex)
{
	constexpr sampler cs(filter::linear, address::clamp_to_edge);
	float s = scene.cloudShadowB.x;
	if (s <= 0.0 || L.y <= 0.05) return 1.0;
	float t = (scene.cloudShadowA.w - worldPos.y) / L.y;
	if (t <= 0.0) return 1.0;
	float2 uv = (worldPos.xz + L.xz * t - scene.cloudShadowA.xy) * scene.cloudShadowA.z;
	float2 e = min(uv, 1.0 - uv);
	float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
	if (edge <= 0.0) return 1.0;
	float T = cloudShadowTex.sample(cs, uv).r;
	return mix(1.0, T, s * edge);
}

// Blinn-Phong over up to 8 scene lights; lightCount == 0 falls back to the
// fixed "headlight" so unlit scenes don't render black.
fragment float4 fragmentMain(VSOut in [[stage_in]],
                             constant SceneUniforms& scene [[buffer(0)]],
                             texture2d<float> baseColor [[texture(0)]],
                             sampler          smp       [[sampler(0)]],
                             texture2d_array<float> shadowMap [[texture(1)]],
                             sampler          shadowSmp [[sampler(1)]],
                             texturecube<float> skyEnv  [[texture(2)]],
                             sampler          skyEnvSmp [[sampler(2)]],
                             texture2d<float> aoTex     [[texture(3)]],
                             sampler          aoSmp     [[sampler(3)]],
                             // Local (point/spot) shadow atlas — pinned at 12, above the
                             // custom-material graph-texture window (1-4) and the GI pins
                             // (5-11), so material draws can never clobber it.
                             texture2d_array<float> localShadowMap [[texture(12)]],
                             constant GIUniforms& gi     [[buffer(3)]],
                             texture2d<float> giShadowTex [[texture(5)]],
                             sampler          giShadowSmp [[sampler(5)]],
                             texture2d<float> giIrrTex    [[texture(6)]],
                             sampler          giIrrSmp    [[sampler(6)]],
                             texture2d<float> giVisTex    [[texture(7)]],
                             sampler          giVisSmp    [[sampler(7)]],
                             texture2d<float> giLocalMask [[texture(8)]],
                             sampler          giLocalSmp  [[sampler(8)]],
                             // FORWARD reflection results (9/10, shared with the
                             // material pipelines' heSSR/heGIRefl pins; dummies +
                             // reflCfg gates 0 when the passes did not run).
                             texture2d<float> ssrTex      [[texture(9)]],
                             sampler          ssrSmp      [[sampler(9)]],
                             texture2d<float> giReflTex   [[texture(10)]],
                             sampler          giReflSmp   [[sampler(10)]],
                             // Cloud-shadow transmittance map — texture 16, NO
                             // sampler argument (constexpr sampler in
                             // cloudShadowFactor): the fragment stage is at
                             // Metal's 16-sampler cap. Shares the slot with the
                             // material preamble's heCloudShadow pin.
                             texture2d<float> cloudShadowTex [[texture(16)]])
{
	float3 albedo = (in.hasTexture > 0.5)
		? baseColor.sample(smp, float2(in.uv.x, 1.0 - in.uv.y)).rgb * in.color
		: in.color;
	float3 N = normalize(in.normal);

	// Weather ground response (matches the GL backend): snow on up-facing surfaces,
	// wetness darkens + glosses the rest. Driven by the EnvironmentComponent.
	float snowMask = smoothstep(0.25, 0.75, clamp(N.y, 0.0, 1.0)) * clamp(scene.weather.y, 0.0, 1.0);
	float wet      = clamp(scene.weather.x, 0.0, 1.0) * (1.0 - snowMask);
	albedo = mix(albedo, float3(0.90, 0.93, 0.97), snowMask);
	albedo *= (1.0 - 0.30 * wet);
	float wRough = mix(in.roughness, 0.08, wet);
	wRough = mix(wRough, 0.85, snowMask);
	// Specular AA (docs/anti-aliasing-plan.md A6) — twin of the GL scene shader.
	// The forward pass owns its interpolated normal, so the derivatives are real
	// here. scene.aaParams.x = 0 → exact no-op.
	wRough = specAARoughness(N, wRough, scene.aaParams.x);

	if (scene.lightCount == 0)
	{
		float3 L    = normalize(float3(0.5, 0.8, 0.6));
		float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		return float4(albedo * diff, in.opacity);
	}

	// Metallic-roughness split (matches the GL backend).
	float3 diffuseColor = albedo * (1.0 - in.metallic);
	float3 specColor    = mix(float3(0.04), albedo, in.metallic);
	float  shininess    = mix(128.0, 8.0, wRough);
	float  specScale    = mix(0.5, 0.03, wRough) + 0.25 * wet; // wet sheen
	specColor           = mix(specColor, float3(0.08), wet);

	float3 V = normalize(scene.cameraPos.xyz - in.worldPos);

	// Image-based ambient from the procedural sky (matches the GL backend):
	// diffuse from the normal, specular from the reflection (bent toward N by
	// roughness as a crude prefilter).
	float3 Rrough  = normalize(mix(reflect(-V, N), N, wRough));
	// Clamp the diffuse IBL lookup at least 5° above the horizon. Sampling near
	// or at the horizon (N.y ≈ 0) returns the warm/orange sunset band of the sky
	// even at noon. A floor of 0.1 keeps the sample safely in the cool sky dome.
	float3 Nup     = normalize(float3(N.x, max(N.y, 0.1), N.z));
	float3 ambDiff = skyEnv.sample(skyEnvSmp, Nup).rgb    * diffuseColor;
	// FORWARD reflection cascade (sky → ray-traced GI refl → SSR) — the
	// built-in twin of heLitP's cascade; per-pixel roughness fade here because
	// the half-res trace carries none. Gates 0 → dead code.
	float3 envSpec = skyEnv.sample(skyEnvSmp, Rrough).rgb;
	if (scene.reflCfg.w > 0.5)
	{
		float4 rr = giReflTex.sample(giReflSmp, in.position.xy / max(scene.viewport.xy, float2(1.0)));
		float fade = 1.0 - smoothstep(scene.reflCfg2.y * 0.7, scene.reflCfg2.y, wRough);
		envSpec = mix(envSpec, rr.rgb, rr.a * scene.reflCfg2.x * fade);
	}
	if (scene.reflCfg.x > 0.5)
	{
		float4 r = ssrTex.sample(ssrSmp, in.position.xy / max(scene.viewport.xy, float2(1.0)));
		float fade = 1.0 - smoothstep(scene.reflCfg.z * 0.7, scene.reflCfg.z, wRough);
		envSpec = mix(envSpec, r.rgb, r.a * scene.reflCfg.y * fade);
	}
	// Fresnel (Schlick, roughness-aware — same term as heLitP, ssr-plan P4).
	float  NdV = saturate(dot(N, V));
	float3 fresnelSpec = specColor
		+ (max(float3(1.0 - wRough), specColor) - specColor) * pow(1.0 - NdV, 5.0);
	float3 ambSpec = envSpec * fresnelSpec;
	float3 ambient = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * wRough);
	// Screen-space ambient occlusion darkens only the IBL indirect term in
	// crevices; the direct lighting added below is left untouched. 1.0 = fully lit.
	float ao = (scene.viewport.z > 0.5)
		? aoTex.sample(aoSmp, in.position.xy / scene.viewport.xy).r : 1.0;
	// Flat ambient fill (never-black floor + overcast replacement) kept outside AO
	// so grazing-angle SSAO over-darkening cannot zero it out.
	// GI (Checkpoint C) replaces the AO-gated IBL term with probe-sampled
	// indirect diffuse when active — AO is bypassed entirely (EncodeSSAO isn't
	// even dispatched; aoTex/ao above are dummy-bound and unused in this branch).
	// The flat scene.ambient floor stays in BOTH branches: probes bounce only
	// actual lights (sun/moon/point/spot), so full overcast or night would
	// otherwise converge to 100% black — the floor is what keeps the never-black
	// guarantee, exactly like the non-GI path.
	// Specular IBL (ambSpec) is kept either way — this GI slice is diffuse-only.
	float3 result = (gi.enabled != 0)
		? sampleDDGIIrradiance(gi, giIrrTex, giIrrSmp, giVisTex, giVisSmp, in.worldPos, N)
		      * diffuseColor * gi.params.x + ambSpec * (1.0 - 0.6 * wRough)
		      + scene.ambient.xyz * diffuseColor
		: ambient * ao + scene.ambient.xyz * diffuseColor;

	int dbgCascade = 0;   // cascade chosen by the directional shadow (debug tint)
	int giLocalIdx = 0;   // counter over non-directional lights → local-mask channel
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

		// Directional lights: CSM (or the temporally-accumulated GI sun mask).
		// Planar view-space depth (along camera forward) — matches the cascade splits,
		// which are planar view-Z far distances (NOT euclidean radius). Using euclidean
		// distance here pushes screen-edge pixels into a too-coarse cascade → dropouts.
		float viewZ = dot(in.worldPos - scene.cameraPos.xyz, scene.cameraFwd.xyz);
		float sh = 1.0;
		if (type == 0)
		{
			sh = (gi.enabled != 0
				? giShadowTex.sample(giShadowSmp, in.position.xy / scene.viewport.xy).r
				: shadowFactor(scene, in.worldPos, N, L, viewZ, shadowMap, shadowSmp, dbgCascade));
			// Cloud shadows multiply on top of both shadow sources — the
			// geometry shadow and the cloud layer occlude independently.
			sh *= cloudShadowFactor(scene, in.worldPos, L, cloudShadowTex);
		}
		else
		{
			// Local (point/spot) lights: shadow-mapped when the light casts
			// shadows (params.y = atlas base layer, set by the extractor).
			// When GI is active the ray-traced hard mask (first 4 local lights)
			// is combined in via min() — the map covers lights the mask can't.
			sh = localShadowFactor(scene, l, in.worldPos, N, localShadowMap, shadowSmp);
			if (gi.enabled != 0 && giLocalIdx < 4)
				sh = min(sh, giLocalMask.sample(giLocalSmp, in.position.xy / scene.viewport.xy)[giLocalIdx]);
			giLocalIdx++;
		}
		// "Receives Shadow" off: the object is lit as if nothing occluded it.
		// Placed after BOTH branches so it covers every source at once — cascades,
		// GI sun/local masks, the cloud layer and the local atlas.
		if (in.noShadow > 0.5) sh = 1.0;

		float diff = max(dot(N, L), 0.0);
		float3 H   = normalize(L + V);
		float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;
		result += (diffuseColor * diff + specColor * spec)
		        * l.colorIntensity.rgb * l.colorIntensity.w * atten * sh;
	}
	result = applyFog(result, scene.cameraPos.xyz, in.worldPos, scene.sunDir.xyz, scene.fog.xy);

	// Debug: tint each fragment by its shadow cascade (red / green / blue / yellow)
	// so the cascade split placement is verifiable at a glance.
	if (scene.debugCascades != 0 && scene.shadowEnabled != 0)
	{
		const float3 tint[4] = { float3(1.0,0.4,0.4), float3(0.4,1.0,0.4),
		                         float3(0.4,0.6,1.0), float3(1.0,1.0,0.4) };
		result *= tint[min(dbgCascade, 3)];
	}
	return float4(result, in.opacity);
}

// ─── Deferred G-buffer fragment (built-in PBR materials) ─────────────────────
// Writes the surface ATTRIBUTES instead of shading them; the fullscreen resolve
// (MaterialShaderLibrary::deferredResolve → heLitP) lights them once per visible
// pixel. Weather/fog/IBL deliberately NOT applied here — they are pure functions
// of these attributes + uniforms and run in the resolve, so the result matches
// forward by construction. Specular 0.5 = the dielectric F0 0.04 the built-in
// forward shader uses; emissive 0, material AO 1 (built-ins have neither).
struct GBufOut {
	float4 gb0 [[color(0)]]; // rgb BaseColor, a Metallic
	float4 gb1 [[color(1)]]; // rg oct Normal, b Roughness, a Specular
	float4 gb2 [[color(2)]]; // rgb Emissive, a Material-AO
	float4 gb3 [[color(3)]]; // r = NDC depth (tile-memory resolve, P6)
};

fragment GBufOut gbufferMain(VSOut in [[stage_in]],
                             texture2d<float> baseColor [[texture(0)]],
                             sampler          smp       [[sampler(0)]],
                             constant SceneUniforms& scene [[buffer(0)]])
{
	float3 albedo = (in.hasTexture > 0.5)
		? baseColor.sample(smp, float2(in.uv.x, 1.0 - in.uv.y)).rgb * in.color
		: in.color;
	float3 N = normalize(in.normal);
	GBufOut o;
	o.gb0 = float4(albedo, clamp(in.metallic, 0.0, 1.0));
	// Specular AA (A6) belongs HERE, not in the resolve: this is the last stage
	// that still has the fragment's own normal. scene.aaParams.x = 0 → no-op.
	o.gb1 = float4(octEncode(N) * 0.5 + 0.5,
	               specAARoughness(N, clamp(in.roughness, 0.0, 1.0), scene.aaParams.x), 0.5);
	o.gb2 = float4(0.0, 0.0, 0.0, 1.0);
	o.gb3 = float4(in.position.z, 0.0, 0.0, 0.0);
	return o;
}
)MSL";

// ─── HDR tonemap (PostProcessPass) ──────────────────────────────────────────
// Fullscreen triangle; samples the RGBA16Float scene color, applies the ACES
// filmic curve + sRGB gamma and writes LDR. Mirrors the GL tonemap shader.
static const char* kTonemapMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct TMOut { float4 position [[position]]; float2 uv; };

vertex TMOut tonemapVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;   // 0->-1, 1->3, 2->-1
	float y = float((vid & 2) << 1) - 1.0;   // 0->-1, 1->-1, 2->3
	TMOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5)); // texture origin is top-left
	return o;
}

float3 aces(float3 v)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((v * (a * v + b)) / (v * (c * v + d) + e), 0.0, 1.0);
}

// Camera lens flare (default OFF): a post-process overlay added in gamma/LDR space after
// ACES, so the chromatic ghosts/halo stay vivid. lf = (sunNDC.xy, aspect, strength) with
// strength already folding behind-camera / off-screen / below-horizon on the CPU. Occlusion
// (behind cloud / geometry) is a cheap luma probe of the HDR at the sun's screen position.
// All element math runs in a canonical y-up aspect-NDC space so it is byte-identical to GL.
float3 lensFlareOverlay(texture2d<float> hdr, sampler s, float2 uv, float4 lf)
{
	float S = lf.w;
	if (S <= 0.0) return float3(0.0);
	float  aspect = lf.z;
	float2 sunNDC = lf.xy;
	float2 pNDC = float2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);   // canonical y-up (Metal uv is top-left)
	float2 P    = float2(pNDC.x * aspect, pNDC.y);
	float2 Sc   = float2(sunNDC.x * aspect, sunNDC.y);
	float2 toSun = P - Sc; float sunDist = length(toSun);
	float2 axis  = -Sc;                                         // sun → screen centre (and beyond)

	// Occlusion: 5-tap HDR luma at the sun's screen uv (Metal top-left). The sky pass draws
	// the sun disc very bright; cloud/opaque geometry in front drops the luma → flare fades.
	float2 sunUV = float2(sunNDC.x * 0.5 + 0.5, 1.0 - (sunNDC.y * 0.5 + 0.5));
	const float2 off[5] = { float2(0.0,0.0), float2(0.006,0.0), float2(-0.006,0.0),
	                        float2(0.0,0.006), float2(0.0,-0.006) };
	float lum = 0.0;
	for (int i = 0; i < 5; ++i)
		lum += dot(hdr.sample(s, clamp(sunUV + off[i], 0.0, 1.0)).rgb, float3(0.2126, 0.7152, 0.0722));
	float vis = smoothstep(2.0, 7.0, lum * 0.2);               // sun disc >> bright sky/cloud

	float3 warm  = float3(1.0, 0.92, 0.80);
	float  core  = 0.22 * exp(-sunDist * sunDist * 45.0);       // subtle — the sky already draws a bright sun
	float  streak = 0.10 * exp(-toSun.x * toSun.x * 5.0) * exp(-toSun.y * toSun.y * 800.0);
	float3 flare = warm * (core + streak);                      // no halo ring (removed per feedback)
	// Ghost disc chain along the sun→centre axis (aperture reflections) — the signature element.
	const float t[5]   = { 0.30, 0.55, 0.80, 1.20, 1.55 };
	const float rad[5] = { 0.09, 0.14, 0.06, 0.20, 0.11 };
	const float amp[5] = { 0.22, 0.15, 0.28, 0.10, 0.18 };
	const float3 gcol[5] = { float3(1.0,0.85,0.6), float3(0.6,0.8,1.0), float3(1.0,0.7,0.7),
	                         float3(0.7,1.0,0.8), float3(0.8,0.7,1.0) };
	for (int i = 0; i < 5; ++i)
	{
		float d = length(P - (Sc + axis * t[i]));
		flare += amp[i] * gcol[i] * smoothstep(rad[i], 0.0, d);
	}
	return flare * (S * vis);
}

fragment float4 tonemapFragment(TMOut in [[stage_in]],
                                texture2d<float> hdr   [[texture(0)]],
                                texture2d<float> bloom [[texture(1)]],
                                constant float2& params [[buffer(0)]], // x: exposure, y: bloomStrength
                                constant float4& lf     [[buffer(1)]]) // lens flare: xy sunNDC, z aspect, w strength
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	float3 c = hdr.sample(s, in.uv).rgb;
	c += bloom.sample(s, in.uv).rgb * params.y;
	c *= params.x;
	c = aces(c);
	c = pow(c, float3(1.0 / 2.2));
	c = clamp(c + lensFlareOverlay(hdr, s, in.uv, lf), 0.0, 1.0); // camera sun flare (OFF when lf.w<=0)
	return float4(c, 1.0);
}
)MSL";

// FXAA (Timothy Lottes' classic edge-blend variant) — mirrors the GL kFxaaFS.
// Runs on the tonemapped (gamma-space) LDR image; also softens the single-pixel
// raymarch speckle the clouds leave in near-clear sky. Same fullscreen-tri UV
// convention as the other post passes (1:1 mapping, no double flip).
static const char* kFxaaMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct FXOut { float4 position [[position]]; float2 uv; };
vertex FXOut fxaaVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	FXOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));
	return o;
}
static float fxLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
fragment float4 fxaaFragment(FXOut in [[stage_in]],
                             texture2d<float> scene [[texture(0)]],
                             constant float2& rcpFrame [[buffer(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	const float EDGE_MIN = 1.0 / 24.0;
	const float EDGE_MAX = 1.0 / 8.0;
	const float SPAN_MAX = 8.0;
	float2 uv  = in.uv;
	float3 rgbM = scene.sample(s, uv).rgb;
	float lM  = fxLuma(rgbM);
	float lNW = fxLuma(scene.sample(s, uv, int2(-1, -1)).rgb);
	float lNE = fxLuma(scene.sample(s, uv, int2( 1, -1)).rgb);
	float lSW = fxLuma(scene.sample(s, uv, int2(-1,  1)).rgb);
	float lSE = fxLuma(scene.sample(s, uv, int2( 1,  1)).rgb);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	float range = lMax - lMin;
	if (range < max(EDGE_MIN, lMax * EDGE_MAX)) return float4(rgbM, 1.0);
	float2 dir;
	dir.x = -((lNW + lNE) - (lSW + lSE));
	dir.y =  ((lNW + lSW) - (lNE + lSE));
	float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * rcpFrame;
	float3 rgbA = 0.5 * (scene.sample(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb
	                   + scene.sample(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
	float3 rgbB = rgbA * 0.5 + 0.25 * (scene.sample(s, uv + dir * -0.5).rgb
	                                 + scene.sample(s, uv + dir *  0.5).rgb);
	float lB = fxLuma(rgbB);
	return (lB < lMin || lB > lMax) ? float4(rgbA, 1.0) : float4(rgbB, 1.0);
}

// AA = Off. The resolve pass is what moves the LDR image into the output target,
// so it still runs — with this passthrough instead of the filter.
fragment float4 aaBlitFragment(FXOut in [[stage_in]],
                               texture2d<float> scene [[texture(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	return float4(scene.sample(s, in.uv).rgb, 1.0);
}

// ── SMAA-style spatial AA (A1) — mirrors the GL kSmaaFS line for line ────────
// Where FXAA guesses an edge tangent and blurs along it, this finds the span the
// pixel's boundary belongs to, classifies both of its ends and derives the
// coverage analytically from the position inside that span. Orthogonal (L/Z/U)
// patterns only — no AreaTex, so no diagonals and no corner rounding; MLAA-tier
// quality, clearly above FXAA. Keep IDENTICAL to the GL version: the two are
// meant to produce the same image, and that is only checkable if they read the
// same.
constant float kSmaaEdgeMin   = 1.0 / 24.0;
constant float kSmaaEdgeRel   = 1.0 / 8.0;
// Single texel steps first, then double: 8 + 12*2 = 32 texels of reach. Reach
// decides whether shallow edges get antialiased at all — see the GL twin.
constant int   kSmaaFineSteps   = 8;
constant int   kSmaaSearchIters = 20;

static float smaaLumaAt(texture2d<float> scene, sampler s, float2 uv)
{
	return fxLuma(scene.sample(s, uv).rgb);
}

// Distance to the last texel that still carries the boundary. `ended` is false
// when the walk hit the cap — an unseen end means "no crossing", never a guess.
static float smaaSearch(texture2d<float> scene, sampler s, float2 uv,
                        float2 along, float2 across, float thr,
                        thread bool& ended, thread float& outerLuma)
{
	ended = false; outerLuma = 0.0;
	float dist = 0.0;
	for (int i = 0; i < kSmaaSearchIters; ++i)
	{
		float st = (i < kSmaaFineSteps) ? 1.0 : 2.0;
		dist += st;
		float2 p = uv + along * dist;
		float  a = smaaLumaAt(scene, s, p);
		float  b = smaaLumaAt(scene, s, p + across);
		if (fabs(a - b) < thr) { ended = true; outerLuma = a; return dist - st; }
	}
	return dist;
}

static float smaaCover(float t, float y1, float y2, bool split)
{
	float f = split ? ((t < 0.5) ? mix(y1, 0.0, t * 2.0) : mix(0.0, y2, (t - 0.5) * 2.0))
	                : mix(y1, y2, t);
	return max(0.0, -f);
}

static float smaaWeight(texture2d<float> scene, sampler s, float2 uv,
                        float2 along, float2 across, float lumaP, float lumaO, float thr)
{
	bool  e1, e2;
	float o1, o2;
	float d1 = smaaSearch(scene, s, uv, -along, across, thr, e1, o1);
	float d2 = smaaSearch(scene, s, uv,  along, across, thr, e2, o2);
	float len = d1 + d2 + 1.0;
	float t   = (d1 + 0.5) / len;
	float y1  = e1 ? (fabs(o1 - lumaO) < fabs(o1 - lumaP) ? -0.5 : 0.5) : 0.0;
	float y2  = e2 ? (fabs(o2 - lumaO) < fabs(o2 - lumaP) ? -0.5 : 0.5) : 0.0;
	bool  split = (e1 && e2 && y1 == y2);
	// Quadrature across the pixel, not one sample at its centre — see the GL twin.
	float dt = 0.25 / len;
	return 0.5 * (smaaCover(clamp(t - dt, 0.0, 1.0), y1, y2, split)
	            + smaaCover(clamp(t + dt, 0.0, 1.0), y1, y2, split));
}

fragment float4 smaaFragment(FXOut in [[stage_in]],
                             texture2d<float> scene [[texture(0)]],
                             constant float2& rcpFrame [[buffer(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	float2 rcp = rcpFrame;
	float2 uv  = in.uv;
	float3 C   = scene.sample(s, uv).rgb;
	float  lC  = fxLuma(C);
	float  lW  = smaaLumaAt(scene, s, uv + float2(-rcp.x, 0.0));
	float  lE  = smaaLumaAt(scene, s, uv + float2( rcp.x, 0.0));
	float  lN  = smaaLumaAt(scene, s, uv + float2(0.0, -rcp.y));
	float  lS  = smaaLumaAt(scene, s, uv + float2(0.0,  rcp.y));
	float lMax = max(lC, max(max(lW, lE), max(lN, lS)));
	float lMin = min(lC, min(min(lW, lE), min(lN, lS)));
	float thr  = max(kSmaaEdgeMin, lMax * kSmaaEdgeRel);
	if (lMax - lMin < thr) return float4(C, 1.0);

	float edgeH = fabs(lN - 2.0 * lC + lS);
	float edgeV = fabs(lW - 2.0 * lC + lE);
	float wA = 0.0, wB = 0.0;
	float2 offA, offB;
	if (edgeH >= edgeV)
	{
		offA = float2(0.0, -rcp.y); offB = float2(0.0, rcp.y);
		if (fabs(lC - lN) >= thr) wA = smaaWeight(scene, s, uv, float2(rcp.x, 0.0), offA, lC, lN, thr);
		if (fabs(lC - lS) >= thr) wB = smaaWeight(scene, s, uv, float2(rcp.x, 0.0), offB, lC, lS, thr);
	}
	else
	{
		offA = float2(-rcp.x, 0.0); offB = float2(rcp.x, 0.0);
		if (fabs(lC - lW) >= thr) wA = smaaWeight(scene, s, uv, float2(0.0, rcp.y), offA, lC, lW, thr);
		if (fabs(lC - lE) >= thr) wB = smaaWeight(scene, s, uv, float2(0.0, rcp.y), offB, lC, lE, thr);
	}

	float sum = wA + wB;
	if (sum > 1.0) { wA /= sum; wB /= sum; sum = 1.0; }
	float3 outC = C * (1.0 - sum)
	            + wA * scene.sample(s, uv + offA).rgb
	            + wB * scene.sample(s, uv + offB).rgb;
	return float4(outC, 1.0);
}
)MSL";

// ─── Temporal AA (docs/anti-aliasing-plan.md A2/A3) ─────────────────────────
// Two shaders and one rule: the geometry is RASTERIZED with the jittered matrix
// (so the subpixel offset lands in the image, which is the whole point), but the
// MOTION is measured with unjittered ones. Mixing those up makes every static
// pixel report the jitter as movement, and TAA then chases its own offset.
static const char* kTaaMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { packed_float3 position; packed_float3 normal; packed_float2 uv; };

// mvpJitter rasterizes; mvpNow / mvpPrev (both unjittered) measure.
struct VelocityUniforms {
	float4x4 mvpJitter;
	float4x4 mvpNow;
	float4x4 mvpPrev;
};

struct VelOut {
	float4 position [[position]];
	float4 clipNow;
	float4 clipPrev;
};

vertex VelOut velocityVertex(uint vid [[vertex_id]],
                             const device VertexIn* verts [[buffer(0)]],
                             constant VelocityUniforms& u [[buffer(1)]])
{
	const float4 p = float4(float3(verts[vid].position), 1.0);
	VelOut o;
	o.position = u.mvpJitter * p;
	o.clipNow  = u.mvpNow  * p;
	o.clipPrev = u.mvpPrev * p;
	return o;
}

// Screen-space motion in TEXTURE-UV units, so the resolve can subtract it from
// its own uv without knowing anything about clip space or the y flip.
fragment float2 velocityFragment(VelOut in [[stage_in]])
{
	const float2 ndcNow  = in.clipNow.xy  / max(in.clipNow.w,  1e-6);
	const float2 ndcPrev = in.clipPrev.xy / max(in.clipPrev.w, 1e-6);
	float2 uvNow  = float2(ndcNow.x,  -ndcNow.y)  * 0.5 + 0.5;
	float2 uvPrev = float2(ndcPrev.x, -ndcPrev.y) * 0.5 + 0.5;
	return uvNow - uvPrev;
}

struct TaaOut { float4 position [[position]]; float2 uv; };

vertex TaaOut taaVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	TaaOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));
	return o;
}

// params: x/y = 1/resolution, z = history blend weight (0 = history unusable
// this frame — resize, camera jump, first frame), w unused.
fragment float4 taaFragment(TaaOut in [[stage_in]],
                            texture2d<float> current  [[texture(0)]],
                            texture2d<float> history  [[texture(1)]],
                            texture2d<float> velocity [[texture(2)]],
                            constant float4& params   [[buffer(0)]])
{
	constexpr sampler linearS(filter::linear, address::clamp_to_edge);
	constexpr sampler pointS(filter::nearest, address::clamp_to_edge);
	const float2 rcp = params.xy;
	const float3 cur = current.sample(pointS, in.uv).rgb;
	if (params.z <= 0.0) return float4(cur, 1.0);

	// Motion of the CLOSEST fragment in the neighbourhood, not this pixel's own:
	// on a silhouette the pixel itself may carry the background's motion while
	// the eye follows the object, and picking the nearest keeps the edge with
	// the object instead of smearing it against the background.
	float2 vel = velocity.sample(pointS, in.uv).rg;
	{
		float best = length(vel);
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x)
			{
				const float2 v = velocity.sample(pointS, in.uv + float2(x, y) * rcp).rg;
				const float  l = length(v);
				if (l > best) { best = l; vel = v; }
			}
	}

	const float2 histUV = in.uv - vel;
	// Off-screen history is no history: nothing was ever accumulated there.
	if (any(histUV < float2(0.0)) || any(histUV > float2(1.0)))
		return float4(cur, 1.0);

	float3 hist = history.sample(linearS, histUV).rgb;

	// Neighbourhood clamp — the whole defence against ghosting. Whatever the
	// history says, the result has to stay inside the colours this frame
	// actually produced around this pixel; a disoccluded surface therefore
	// cannot keep showing what used to be in front of it.
	float3 lo = cur, hi = cur;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			const float3 c = current.sample(pointS, in.uv + float2(x, y) * rcp).rgb;
			lo = min(lo, c);
			hi = max(hi, c);
		}
	hist = clamp(hist, lo, hi);

	// Fast motion means less history: the further the reprojection reached, the
	// less it can be trusted (and the less a stale sample is worth).
	const float motion = saturate(length(vel * float2(current.get_width(), current.get_height())) / 32.0);
	const float blend  = mix(params.z, 0.0, motion);
	return float4(mix(cur, hist, blend), 1.0);
}

// The temporal average is softer than a single frame by construction — this is
// the sharpen that buys that back, and the only reason the AA-resolve slot still
// runs a shader for TAA instead of a plain blit. sharpness 0 = exact passthrough.
fragment float4 taaSharpenFragment(TaaOut in [[stage_in]],
                                   texture2d<float> src [[texture(0)]],
                                   constant float4& params [[buffer(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	const float2 rcp = params.xy;
	const float  amount = params.z;
	const float3 c = src.sample(s, in.uv).rgb;
	if (amount <= 0.0) return float4(c, 1.0);
	const float3 blur = 0.25 * (src.sample(s, in.uv + float2( rcp.x, 0.0)).rgb
	                          + src.sample(s, in.uv + float2(-rcp.x, 0.0)).rgb
	                          + src.sample(s, in.uv + float2(0.0,  rcp.y)).rgb
	                          + src.sample(s, in.uv + float2(0.0, -rcp.y)).rgb);
	return float4(clamp(c + (c - blur) * amount, 0.0, 1.0), 1.0);
}
)MSL";

// In-Game UI 2D pass: quads derived from vertex_id + uniforms.
// rect = {x, y, w, h} pixels;  viewport = {vpW, vpH} pixels;
// uvrect = {u0, v0, u1, v1} into the font atlas (glyph quads).
// mode: 0 = solid color, 1 = font-atlas glyph (alpha from atlas R channel).
static const char* kUIMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct UIVert { float4 position [[position]]; float2 uv; float2 luv; };
// rot = { angle(radians), pivotX(px), pivotY(px), unused }; angle 0 = upright.
vertex UIVert uiVertex(uint vid [[vertex_id]],
                       constant float4& rect     [[buffer(0)]],
                       constant float2& viewport [[buffer(1)]],
                       constant float4& uvrect   [[buffer(2)]],
                       constant float4& rot      [[buffer(3)]])
{
    const float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    float2 uv = c[vid];
    float2 sp = rect.xy + uv * rect.zw;
    if (rot.x != 0.0) {
        float s = sin(rot.x), co = cos(rot.x);
        float2 d = sp - rot.yz;
        sp = rot.yz + float2(d.x * co - d.y * s, d.x * s + d.y * co);
    }
    float2 ndc = float2(sp.x / viewport.x * 2.0 - 1.0,
                        1.0 - sp.y / viewport.y * 2.0);
    UIVert o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv  = mix(uvrect.xy, uvrect.zw, uv);
    o.luv = uv;                 // 0..1 across the quad (for the rounded-rect SDF)
    return o;
}
// shape = { mode, cornerRadius(px), rectW(px), rectH(px) }.
// mode 0 = solid colour, 1 = font-atlas glyph (alpha from .r), 2 = textured
// quad (RGBA from texture(0), tinted by colour). Modes 1 and 2 share the slot:
// a glyph run binds the atlas there, an image its own texture.
fragment float4 uiFragment(UIVert in [[stage_in]],
                           constant float4& color [[buffer(0)]],
                           constant float4& shape [[buffer(1)]],
                           texture2d<float> atlas [[texture(0)]])
{
    if (shape.x > 0.5 && shape.x < 1.5) {
        constexpr sampler s(filter::linear);
        float a = atlas.sample(s, in.uv).r;
        return float4(color.rgb, color.a * a);
    }
    if (shape.x > 1.5) {
        constexpr sampler s(filter::linear);
        float4 t = atlas.sample(s, in.uv);
        float4 c = float4(color.rgb * t.rgb, color.a * t.a);
        if (shape.y <= 0.0) return c;
        // A rounded image is the same SDF the solid path uses, applied to the
        // sampled alpha — that is what makes a rounded avatar possible at all.
        float2 halfSzT = shape.zw * 0.5;
        float  rT      = min(shape.y, min(halfSzT.x, halfSzT.y));
        float2 pT      = (in.luv - 0.5) * shape.zw;
        float2 qT      = abs(pT) - (halfSzT - rT);
        float  dT      = length(max(qT, 0.0)) + min(max(qT.x, qT.y), 0.0) - rT;
        return float4(c.rgb, c.a * clamp(0.5 - dT, 0.0, 1.0));
    }
    if (shape.y <= 0.0) return color; // square quad → crisp, no SDF/AA
    // Solid quad with rounded corners (radius = min(w,h)/2 → circle).
    float2 halfSz = shape.zw * 0.5;
    float  r      = min(shape.y, min(halfSz.x, halfSz.y));
    float2 p      = (in.luv - 0.5) * shape.zw;
    float2 q      = abs(p) - (halfSz - r);
    float  d      = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float  cov  = clamp(0.5 - d, 0.0, 1.0); // ~1px antialiased edge (d is in pixels)
    return float4(color.rgb, color.a * cov);
}
)MSL";

// Bloom bright-pass + separable Gaussian blur. Reuses the fullscreen-triangle VS
// (1:1 upright mapping, same convention as the tonemap pass). Mirrors the GL
// kBloomBrightFS / kBloomBlurFS shaders.
static const char* kBloomMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FSOut { float4 position [[position]]; float2 uv; };

vertex FSOut fsVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	FSOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));
	return o;
}

fragment float4 brightFragment(FSOut in [[stage_in]],
                               texture2d<float> hdr [[texture(0)]],
                               constant float2& params [[buffer(0)]]) // x: threshold, y: knee
{
	constexpr sampler s(filter::linear);
	float3 c  = hdr.sample(s, in.uv).rgb;
	float  br = max(c.r, max(c.g, c.b));
	float  threshold = params.x, knee = params.y;
	float  soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
	soft = (soft * soft) / (4.0 * knee + 1e-4);
	float contrib = max(soft, br - threshold) / max(br, 1e-4);
	return float4(c * contrib, 1.0);
}

fragment float4 blurFragment(FSOut in [[stage_in]],
                             texture2d<float> img [[texture(0)]],
                             constant float4& cfg [[buffer(0)]]) // xy: texel, z: horizontal
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	float w[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
	float2 dir = (cfg.z > 0.5) ? float2(cfg.x, 0.0) : float2(0.0, cfg.y);
	float3 result = img.sample(s, in.uv).rgb * w[0];
	for (int i = 1; i < 5; ++i)
	{
		result += img.sample(s, in.uv + dir * float(i)).rgb * w[i];
		result += img.sample(s, in.uv - dir * float(i)).rgb * w[i];
	}
	return float4(result, 1.0);
}
)MSL";

// ─── SSAO (screen-space ambient occlusion) ──────────────────────────────────
// Mirrors the GL backend. Working in view space makes the maths identical across
// backends; the only difference is the NDC→UV y-flip (Metal textures are top-left
// origin), which exactly compensates the top-left rasterisation, so the sampled
// view positions match GL's.
static const char* kSSAOMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { packed_float3 position; packed_float3 normal; packed_float2 uv; };
struct SSAOPosUniforms { float4x4 mvp; float4x4 modelView; float4x4 model; };
struct SSAOPosOut { float4 position [[position]]; float3 viewPos; };

// Pre-pass: rasterise the scene, write the per-pixel view-space position.
vertex SSAOPosOut ssaoPosVertex(uint vid [[vertex_id]],
                                const device VertexIn*    verts [[buffer(0)]],
                                constant SSAOPosUniforms& u     [[buffer(1)]])
{
	SSAOPosOut o;
	float4 p   = float4(float3(verts[vid].position), 1.0);
	o.position = u.mvp * p;
	o.viewPos  = (u.modelView * p).xyz;
	return o;
}
fragment float4 ssaoPosFragment(SSAOPosOut in [[stage_in]])
{
	return float4(in.viewPos, 1.0); // a = 1 → valid geometry
}

// The FORWARD-reflections variant of this pre-pass (MRT: view position + oct
// world normal + NDC depth) used to live here as reflPosVertex/reflPosFragment.
// It moved into the shared MaterialShaderLibrary — every backend porting SSR
// needs the identical encoding, and the trace decodes it with the preamble's
// heOctDecode (docs/ssr-cross-backend-plan.md §3.2 way (a)). See
// MaterialShaderLibrary::reflPrepassVertex / reflPrepassFragment; the pipeline
// is built from those in CreateScenePipeline, with the SSBO vertex-pull pinned
// to the same buffer(0)/buffer(1) this pass's encoder has always used.

struct SSAOOut { float4 position [[position]]; float2 uv; };
vertex SSAOOut ssaoVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	SSAOOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));
	return o;
}

// Deferred (plan P5): reconstruct the view-space position from the G-buffer
// depth instead of re-rasterizing the scene — the whole extract/cull/sort +
// geometry pre-pass disappears when the G-buffer exists this frame. Stored
// depth is the GL-convention ndc z (the scene raster uses the unfixed
// projection), so invProj is the plain inverse of the GL projection; uv origin
// is top-left → ndc.y flips.
struct SSAODepthPosParams { float4x4 invProj; };
fragment float4 ssaoDepthPosFragment(SSAOOut in [[stage_in]],
                                     texture2d<float> depthTex [[texture(0)]],
                                     sampler          smp      [[sampler(0)]],
                                     constant SSAODepthPosParams& P [[buffer(0)]])
{
	float d = depthTex.sample(smp, in.uv).r;
	if (d >= 1.0) return float4(0.0);              // background → a = 0
	float4 clip = float4(in.uv.x * 2.0 - 1.0, 1.0 - 2.0 * in.uv.y, d, 1.0);
	float4 v = P.invProj * clip;
	return float4(v.xyz / max(v.w, 1e-8), 1.0);    // a = 1 → valid geometry
}

struct SSAOParams {
	float4x4 proj;        // camera projection (GL convention)
	float4   cfg;         // x,y = noise scale (viewport/4), z = radius, w = bias
	float4   cfg2;        // x = intensity, y = AO method (0 SSAO, 1 HBAO, 2 GTAO)
	float4   samples[32]; // hemisphere kernel (xyz)  — 'kernel' is reserved in MSL
};

// HBAO: OR the angular sectors [minH,maxH] (each normalised to [0,1] across the
// hemisphere arc) into a 32-bit visibility bitmask.
static uint hbaoSectors(float minH, float maxH, uint mask)
{
	uint startBit = min(uint(clamp(minH, 0.0, 1.0) * 32.0), 31u);
	uint count    = uint(ceil(clamp(maxH - minH, 0.0, 1.0) * 32.0));
	uint bits     = (count > 0u) ? (0xFFFFFFFFu >> (32u - count)) : 0u;
	return mask | (bits << startBit);
}
// Interleaved-gradient noise for the per-pixel slice/step jitter (Jimenez 2014).
static float ssaoIgn(float2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

fragment float4 ssaoFragment(SSAOOut in [[stage_in]],
                             texture2d<float> posTex   [[texture(0)]],
                             sampler          posSmp   [[sampler(0)]],
                             texture2d<float> noiseTex [[texture(1)]],
                             sampler          noiseSmp [[sampler(1)]],
                             constant SSAOParams& P     [[buffer(0)]])
{
	float4 pv = posTex.sample(posSmp, in.uv);
	if (pv.a < 0.5) return float4(1.0);            // background → unoccluded
	float3 Pp = pv.xyz;

	float2 texel = 1.0 / float2(posTex.get_width(), posTex.get_height());
	float3 Pr = posTex.sample(posSmp, in.uv + float2(texel.x, 0.0)).xyz;
	float3 Pl = posTex.sample(posSmp, in.uv - float2(texel.x, 0.0)).xyz;
	float3 Pu = posTex.sample(posSmp, in.uv + float2(0.0, texel.y)).xyz;
	float3 Pd = posTex.sample(posSmp, in.uv - float2(0.0, texel.y)).xyz;
	float3 ddx = (abs(Pr.z - Pp.z) < abs(Pp.z - Pl.z)) ? (Pr - Pp) : (Pp - Pl);
	float3 ddy = (abs(Pu.z - Pp.z) < abs(Pp.z - Pd.z)) ? (Pu - Pp) : (Pp - Pd);
	float3 N = normalize(cross(ddx, ddy));
	if (N.z < 0.0) N = -N;                          // face the camera (+Z view space)

	float ao;
	if (int(P.cfg2.y + 0.5) == 1)
	{
		// ── HBAO: horizon-based AO via a 32-sector visibility bitmask (mirrors GL) ──
		const int   SLICES = 3;
		const int   STEPS  = 8;
		const float THICKNESS = 0.5;                 // assumed occluder depth (view units)
		float3 V = normalize(-Pp);                   // camera at the view-space origin
		float2 fragCoord = in.uv * float2(float(posTex.get_width()), float(posTex.get_height()));
		float  jitter = ssaoIgn(fragCoord) - 0.5;
		float  depthScale = 0.5 * P.cfg.z / max(-Pp.z, 1e-4);   // cfg.z = radius
		float  visibility = 0.0;
		for (int s = 0; s < SLICES; ++s)
		{
			float  phi = (float(s) + jitter) * (6.28318530718 / float(SLICES));
			float2 omega = float2(cos(phi), sin(phi));
			float3 dir = float3(omega, 0.0);
			float3 orthoDir = dir - dot(dir, V) * V;
			float3 axis = cross(dir, V);
			float3 projN = N - axis * dot(N, axis);  // normal projected into the slice plane
			float  projLen = length(projN);
			if (projLen < 1e-5) { visibility += 1.0; continue; }
			float  nAng = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, 0.0, 1.0));
			// Metal: uv.y is top-left, so negate the y of the UV march so the sampled
			// neighbours stay in the same view-space slice plane that nAng assumes.
			float2 omegaUV = float2(P.proj[0][0] * omega.x, -P.proj[1][1] * omega.y);
			uint occ = 0u;
			for (int i = 0; i < STEPS; ++i)
			{
				float  t   = (float(i) + jitter) / float(STEPS) + 0.01;
				float2 sUV = in.uv - t * depthScale * omegaUV;
				float4 sp  = posTex.sample(posSmp, sUV);
				if (sp.a < 0.5) continue;
				float3 d   = sp.xyz - Pp;
				float  len = length(d);
				float2 fb;
				fb.x = dot(d / max(len, 1e-5), V);                    // front horizon
				fb.y = dot(normalize(d - V * THICKNESS), V);          // back (thickness)
				fb   = acos(clamp(fb, -1.0, 1.0));
				fb   = clamp((fb + nAng + 1.57079632679) / 3.14159265359, 0.0, 1.0);
				occ  = hbaoSectors(min(fb.x, fb.y), max(fb.x, fb.y), occ);
			}
			visibility += 1.0 - float(popcount(occ)) / 32.0;
		}
		visibility /= float(SLICES);
		ao = 1.0 - (1.0 - visibility) * P.cfg2.x;    // cfg2.x = intensity
		ao = max(ao, 0.1);                           // backstop against pure black
	}
	else if (int(P.cfg2.y + 0.5) == 2)
	{
		// ── GTAO: Ground-Truth AO (Jiménez et al. 2016, mirrors GL) ─────────────
		// Per slice find the max horizon angle on each side, project the normal into
		// the slice plane (γ), then integrate visibility analytically over the
		// cosine-weighted arc between the two horizons. Slices span [0,π) (each line
		// covers both ± directions).
		const int SLICES = 3;
		const int STEPS  = 8;
		const float PI = 3.14159265359, HALF_PI = 1.57079632679;
		float3 V = normalize(-Pp);
		float2 fragCoord = in.uv * float2(float(posTex.get_width()), float(posTex.get_height()));
		float  jitter = ssaoIgn(fragCoord);
		float  depthScale = 0.5 * P.cfg.z / max(-Pp.z, 1e-4);   // cfg.z = radius
		float  visAccum = 0.0;
		for (int s = 0; s < SLICES; ++s)
		{
			float  phi = (float(s) + jitter) * (PI / float(SLICES));
			float2 omega = float2(cos(phi), sin(phi));
			float3 dir = float3(omega, 0.0);
			float3 axis = cross(dir, V);
			float  axisLen = length(axis);
			if (axisLen < 1e-5) { visAccum += 1.0; continue; }
			axis /= axisLen;
			float3 orthoDir = normalize(dir - dot(dir, V) * V);  // in-plane ⟂ V, toward +omega
			float3 projN = N - axis * dot(N, axis);              // normal into slice plane
			float  projLen = length(projN);
			if (projLen < 1e-5) continue;                        // normal ⟂ slice → no AO here
			float  gamma = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, -1.0, 1.0));
			// Metal: uv.y top-left, negate the y of the UV march (same as HBAO).
			float2 omegaUV = float2(P.proj[0][0] * omega.x, -P.proj[1][1] * omega.y);
			float  cH1 = 0.0;   // +omega side horizon cosine (vs V); 0 ⇒ no occluder
			float  cH2 = 0.0;   // -omega side
			for (int i = 0; i < STEPS; ++i)
			{
				float  t = (float(i) + jitter) / float(STEPS) + 0.02;
				float4 sp1 = posTex.sample(posSmp, in.uv + t * depthScale * omegaUV);
				if (sp1.a >= 0.5) {
					float3 d = sp1.xyz - Pp; float len = length(d);
					float fall = clamp(1.0 - len / P.cfg.z, 0.0, 1.0);
					cH1 = max(cH1, (dot(d, V) / max(len, 1e-5)) * fall);
				}
				float4 sp2 = posTex.sample(posSmp, in.uv - t * depthScale * omegaUV);
				if (sp2.a >= 0.5) {
					float3 d = sp2.xyz - Pp; float len = length(d);
					float fall = clamp(1.0 - len / P.cfg.z, 0.0, 1.0);
					cH2 = max(cH2, (dot(d, V) / max(len, 1e-5)) * fall);
				}
			}
			float h1 =  acos(clamp(cH1, -1.0, 1.0));  // +side, ≥0
			float h2 = -acos(clamp(cH2, -1.0, 1.0));  // -side, ≤0
			h1 = gamma + min(h1 - gamma,  HALF_PI);   // clamp to normal's hemisphere
			h2 = gamma + max(h2 - gamma, -HALF_PI);
			float cosG = cos(gamma), sinG = sin(gamma);
			float arc = (-cos(2.0 * h1 - gamma) + cosG + 2.0 * h1 * sinG)
			          + (-cos(2.0 * h2 - gamma) + cosG + 2.0 * h2 * sinG);
			visAccum += projLen * 0.25 * arc;
		}
		float visibility = clamp(visAccum / float(SLICES), 0.0, 1.0);
		ao = 1.0 - (1.0 - visibility) * P.cfg2.x;
		ao = max(ao, 0.1);                           // backstop against pure black
	}
	else
	{
		// ── SSAO: slope-invariant tangent-plane kernel ─────────────────────────
		float3 randv = noiseTex.sample(noiseSmp, in.uv * P.cfg.xy).xyz;
		float3 T = normalize(randv - N * dot(randv, N));
		float3 B = cross(N, T);
		float3x3 TBN = float3x3(T, B, N);
		float occ = 0.0;
		for (int i = 0; i < 32; ++i)
		{
			// Kernel only picks which nearby screen pixels to inspect (hemisphere footprint).
			float3 sp = Pp + (TBN * P.samples[i].xyz) * P.cfg.z;
			float4 clip = P.proj * float4(sp, 1.0);
			float2 suv = clip.xy / clip.w;
			suv = float2(suv.x * 0.5 + 0.5, 1.0 - (suv.y * 0.5 + 0.5)); // Metal: top-left origin
			if (any(suv < 0.0) || any(suv > 1.0)) continue;
			float4 sv = posTex.sample(posSmp, suv);
			if (sv.a < 0.5) continue;                    // sampled the background
			// Slope-invariant occlusion: how far the neighbour rises above this fragment's
			// tangent plane (Pp, N). A flat surface — even edge-on — has neighbours IN the
			// plane (dot ≈ 0) and can't occlude itself.
			float3 toOcc = sv.xyz - Pp;
			float  above = dot(toOcc, N);
			float  rangeCheck = smoothstep(0.0, 1.0, P.cfg.z / max(length(toOcc), 1e-4));
			occ += (above > P.cfg.w ? 1.0 : 0.0) * rangeCheck;
		}
		ao = 1.0 - (occ / 32.0) * P.cfg2.x;
		ao = max(ao, 0.5);                           // conservative backstop
	}
	return float4(ao, ao, ao, 1.0);
}

fragment float4 ssaoBlurFragment(SSAOOut in [[stage_in]],
                                 texture2d<float> ao [[texture(0)]],
                                 sampler          s  [[sampler(0)]])
{
	float2 texel = 1.0 / float2(ao.get_width(), ao.get_height());
	float sum = 0.0;
	for (int x = -2; x < 2; ++x)
		for (int y = -2; y < 2; ++y)
			sum += ao.sample(s, in.uv + float2(float(x), float(y)) * texel).r;
	float v = sum / 16.0;
	return float4(v, v, v, 1.0);
}
)MSL";

// ─── Global Illumination: ray-traced shadow pass ─────────────────────────────
// World-space G-buffer pre-pass + 1-ray/pixel occlusion query against the TLAS
// EncodeGIAccelBuild built this frame + temporal accumulation + spatial blur.
// Requires MSL 2.4 (macOS 12+) for intersection_query — matches the capability
// gate MetalRenderer::EnsureRaytracingSupport already enforces.
// Raster stages of the GI shadow pass (G-buffer prepass, temporal
// accumulation, spatial blur) — split from the ray kernel so they compile
// on EVERY Metal device: the HW kernel needs MSL 2.4 (intersection_query)
// and would take the whole library down on older OSes, but these stages are
// shared by the hardware AND software ray-tracing paths.
static const char* kGIShadowRasterMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GIVertexIn { packed_float3 position; packed_float3 normal; packed_float2 uv; };
struct GIPosUniforms { float4x4 mvp; float4x4 model; };
struct GIPosOut { float4 position [[position]]; float3 worldPos; float3 normal; };

// World-space position + normal, unlike ssaoPosVertex (view-space, no normal) —
// ray tracing needs a world-space origin/direction.
vertex GIPosOut giGBufVertex(uint vid [[vertex_id]],
                             const device GIVertexIn* verts [[buffer(0)]],
                             constant GIPosUniforms&  u     [[buffer(1)]])
{
	GIPosOut o;
	float4 p = float4(float3(verts[vid].position), 1.0);
	o.position = u.mvp * p;
	o.worldPos = (u.model * p).xyz;
	float3x3 m3 = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz);
	o.normal   = m3 * float3(verts[vid].normal);
	return o;
}
struct GIGBufOut { float4 posOut [[color(0)]]; float4 normOut [[color(1)]]; };
fragment GIGBufOut giGBufFragment(GIPosOut in [[stage_in]])
{
	GIGBufOut o;
	o.posOut  = float4(in.worldPos, 1.0);            // a = 1 → valid geometry
	o.normOut = float4(normalize(in.normal), 0.0);
	return o;
}

struct GIFsOut { float4 position [[position]]; float2 uv; };
vertex GIFsOut giFsVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	GIFsOut o;
	o.position = float4(x, y, 0.0, 1.0);
	o.uv       = float2(x * 0.5 + 0.5, 1.0 - (y * 0.5 + 0.5));
	return o;
}

struct GITemporalParams { float4x4 prevViewProj; float4 blend; }; // blend.x = history weight (0 on first activation frame)

// Reproject last frame's accumulated shadow value via this pixel's world
// position and blend it with the new raw sample. The history texture carries
// the WORLD POSITION the value was written for (rgb) alongside the shadow
// scalar (a), so a disoccluded/parallax-revealed pixel — whose reprojected UV
// lands on an unrelated surface, extremely common while orbiting the camera —
// is rejected instead of blending in a wrong value at a heavy (~0.9) weight.
// Earlier versions only checked UV-bounds, not "is this actually the same
// surface", which visibly ghosted/swam on any camera rotation.
fragment float4 giShadowTemporal(GIFsOut in [[stage_in]],
                                 texture2d<float> gPos    [[texture(0)]],
                                 texture2d<float> raw     [[texture(1)]],
                                 texture2d<float> history [[texture(2)]],
                                 sampler          smp     [[sampler(0)]],
                                 constant GITemporalParams& P [[buffer(0)]])
{
	float4 pv   = gPos.sample(smp, in.uv);
	float  rawV = raw.sample(smp, in.uv).r;
	if (pv.a < 0.5) return float4(0.0, 0.0, 0.0, rawV); // background: no history to reproject

	float4 clip = P.prevViewProj * float4(pv.xyz, 1.0);
	if (clip.w <= 0.0) return float4(pv.xyz, rawV);
	float2 ndc    = clip.xy / clip.w;
	float2 prevUV = float2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
	if (any(prevUV < 0.0) || any(prevUV > 1.0))
		return float4(pv.xyz, rawV); // off-screen last frame → no history

	float4 hist = history.sample(smp, prevUV);
	// Reject history whose recorded world position is far from THIS pixel's —
	// a disoccluded/wrong-surface reproject, not the same point one frame ago.
	// MUST be tight: two points can be spatially close in world units while
	// being on completely different-facing surfaces (e.g. either side of a cube
	// edge, right where a "lit face reprojects from/to the adjacent shadowed
	// face" artifact would show up) — a loose, depth-scaled tolerance (the
	// original version of this check used up to ~0.5 units at typical test
	// distances, comparable to the whole object) accepted exactly that case as
	// "close enough". Capped in the few-centimetre range regardless of depth.
	const float posError = length(pv.xyz - hist.rgb);
	const float tolerance = clamp(0.02 * clip.w, 0.01, 0.06);
	const float w = (posError < tolerance) ? clamp(P.blend.x, 0.0, 0.98) : 0.0;
	// Neighbourhood clamp: the position check above only guards RECEIVER
	// motion — when the OCCLUDER moves, the receiving surface is unchanged and
	// stale history blends in at 0.9, smearing the old shadow across the floor
	// for ~30 frames. Clamping history to the current frame's 3x3 raw
	// neighbourhood bounds it by present reality: a moved shadow edge updates
	// within 1-2 frames, while static-noise smoothing is unaffected (the
	// neighbourhood spans the jitter noise range anyway).
	const float2 texel = 1.0 / float2(raw.get_width(), raw.get_height());
	float nMin = rawV, nMax = rawV;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
		{
			const float r = raw.sample(smp, in.uv + float2(float(x), float(y)) * texel).r;
			nMin = min(nMin, r);
			nMax = max(nMax, r);
		}
	float result = mix(rawV, clamp(hist.a, nMin, nMax), w);
	return float4(pv.xyz, result);
}

// Reads the shadow scalar from the temporal history's alpha channel (rgb there
// is the world position used for next frame's disocclusion check, not colour).
fragment float4 giShadowBlur(GIFsOut in [[stage_in]],
                             texture2d<float> src [[texture(0)]],
                             sampler          smp [[sampler(0)]])
{
	float2 texel = 1.0 / float2(src.get_width(), src.get_height());
	float sum = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			sum += src.sample(smp, in.uv + float2(float(x), float(y)) * texel).a;
	float v = sum / 9.0;
	return float4(v, 0.0, 0.0, 1.0);
}
)MSL";

static const char* kGIShadowMSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct GIShadowParams {
	float4 sunDirRadius; // xyz = direction TOWARD the light (world space), w = angular radius (radians)
	float4 frame;        // x = jitter seed, y = tex width, z = tex height, w = SW instance count
	float4 localPosRange[4]; // xyz = local (point/spot) light position, w = range
	float4 extra;            // x = local light count
};

// Interleaved-gradient-noise-style hash → two independent [0,1) values per
// pixel/frame, so successive frames sample different points in the light cone
// (the temporal pass turns this into a soft penumbra without more rays/pixel).
static float2 giHash2(uint2 gid, float seed)
{
	float2 p = float2(gid) + seed * 13.37;
	return float2(fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453),
	              fract(sin(dot(p, float2(39.3468, 11.1352))) * 24634.6345));
}
// Uniform disk sample mapped into a cone around L (small-angle approximation —
// fine for the sun's ~0.25-3° angular radius this drives).
static float3 giConeSample(float3 L, float angleRad, float2 xi)
{
	float3 up = (abs(L.y) < 0.99) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 T  = normalize(cross(up, L));
	float3 B  = cross(L, T);
	float  r   = sin(angleRad) * sqrt(xi.x);
	float  phi = 6.28318530718 * xi.y;
	return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

// One shadow ray per pixel toward the dominant directional light. Every BLAS in
// the TLAS is opaque (BuildBLAS sets geom.opaque=YES) and accept_any_intersection
// is set, so the first hit commits — a boolean occlusion test, no closest-hit
// search needed.
kernel void giShadowRay(uint2 gid [[thread_position_in_grid]],
                        texture2d<float, access::read>  gPos      [[texture(0)]],
                        texture2d<float, access::read>  gNorm     [[texture(1)]],
                        texture2d<float, access::write> outShadow [[texture(2)]],
                        texture2d<float, access::write> outLocal  [[texture(3)]],
                        instance_acceleration_structure accel     [[buffer(0)]],
                        constant GIShadowParams&         P         [[buffer(1)]])
{
	if (float(gid.x) >= P.frame.y || float(gid.y) >= P.frame.z) return;
	float4 pv = gPos.read(gid);
	if (pv.a < 0.5) // background → everything unoccluded
	{ outShadow.write(float4(1.0), gid); outLocal.write(float4(1.0), gid); return; }
	float3 N = normalize(gNorm.read(gid).xyz);
	float3 L = P.sunDirRadius.xyz;

	// ── Directional light (cone-jittered, temporally accumulated) ─────────
	float sunVis = 0.0;
	// Grazing/back-facing relative to the light: direct lighting's dot(N,L)
	// term already zeroes this out, so skip the trace entirely.
	if (dot(N, L) > 0.0)
	{
		float2 xi  = giHash2(gid, P.frame.x);
		float3 dir = giConeSample(L, max(P.sunDirRadius.w, 1e-4), xi);
		// Normal-offset bias + min_distance floor guard self-intersection
		// ("shadow acne") independently.
		ray r;
		r.origin       = pv.xyz + N * 0.05;
		r.direction    = dir;
		r.min_distance = 0.02;
		r.max_distance = 10000.0;
		intersection_params params;
		params.accept_any_intersection(true);
		intersection_query<triangle_data, instancing> q;
		q.reset(r, accel, params);
		q.next();
		sunVis = (q.get_committed_intersection_type() == intersection_type::none) ? 1.0 : 0.0;
	}
	outShadow.write(float4(sunVis), gid);

	// ── Local (point/spot) lights: one HARD occlusion ray each toward the
	// first 4 — point/spot lights previously had NO shadowing at all (CSM
	// never covered them), so they shone straight through geometry. The rays
	// are deliberately UNjittered: deterministic → no noise → no temporal
	// pass needed, the mask reacts instantly and artefact-free. One visibility
	// channel per light, fragmentMain indexes by its local-light counter.
	float4 localVis = float4(1.0);
	const int localCount = clamp(int(P.extra.x), 0, 4);
	for (int i = 0; i < localCount; ++i)
	{
		const float3 toL   = P.localPosRange[i].xyz - pv.xyz;
		const float  distL = length(toL);
		if (distL <= 0.05) continue; // on top of the light → lit
		if (distL >= P.localPosRange[i].w) continue; // outside the attenuation radius → contributes nothing, skip the ray
		const float3 dirL = toL / distL;
		if (dot(N, dirL) <= 0.0) { localVis[i] = 0.0; continue; }
		ray lr;
		lr.origin       = pv.xyz + N * 0.05;
		lr.direction    = dirL;
		lr.min_distance = 0.02;
		lr.max_distance = max(distL - 0.1, 0.02); // stop short of the light itself
		intersection_params lparams;
		lparams.accept_any_intersection(true);
		intersection_query<triangle_data, instancing> lq;
		lq.reset(lr, accel, lparams);
		lq.next();
		if (lq.get_committed_intersection_type() != intersection_type::none)
			localVis[i] = 0.0;
	}
	outLocal.write(localVis, gid);
}

)MSL";

// ─── Global Illumination: DDGI probe update ──────────────────────────────────
// One thread per octahedral-map output texel ("gather": each thread traces its
// OWN ray in its own texel's direction) rather than scattering N random rays
// into M texels — needs no atomics/resolve pass, since every thread in a
// probe's update owns exactly one texel this dispatch. Both irradiance and
// visibility are written from the SAME ray (v1 simplification — see the header
// comment on EncodeGIProbeUpdate for the full list of documented shortcuts).
// read_write texture access needs MTLReadWriteTextureTier2 for RGBA16Float/
// RG16Float — universal on Apple Silicon, and this kernel is already gated on
// device.supportsRaytracing (effectively Apple-Silicon-only), so no separate
// runtime check.
static const char* kGIProbeMSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct GIProbeParams {
	float4 gridOrigin;   // xyz = world-space grid origin, w = spacing
	float4 gridCounts;   // xyz = probe counts per axis (float, cast in-shader), w = probesPerRow
	float4 rayParams;    // x = max ray distance, y = hysteresis (EMA blend), z = cursor start, w = probes this batch
	float4 sunDirRadius; // xyz = direction TOWARD the sun, w = local light count
	float4 sunColor;     // rgb = sun colour * intensity, w unused
	float4 skyAmbient;   // rgb = flat ambient/sky colour used on ray miss, w unused
	// Local lights (point/spot) for the one-bounce estimate — without these,
	// scenes keyed by point lights converge to pitch-black probes (only the
	// directional light fed the bounce). Same attenuation model as fragmentMain.
	float4 lightPosRange[8];  // xyz = world position, w = range
	float4 lightColorType[8]; // rgb = colour * intensity, w = type (1 point, 2 spot)
	float4 lightDirCos[8];    // xyz = spot travel direction, w = cos(half angle)
};

// Standard signed-octahedral mapping (Meyer et al. 2010, Clarberg-style). This
// kernel only ever goes texel -> direction (octDecode); octEncode (direction ->
// texel) is only needed at shading time, in kUnlitMSL's sampleDDGIIrradiance.
static float3 octDecode(float2 e)
{
	float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0)
	{
		float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * signN;
	}
	return normalize(n);
}

constant int kGIProbeOctSize = 8; // must match MetalRenderer::kGIProbeOctSize

// direction -> octahedral UV, inverse of octDecode (needed for the
// multi-bounce field lookup below; matches kUnlitMSL's octEncode).
static float2 octEncodeP(float3 n)
{
	float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

// PREVIOUS-frame irradiance field at an arbitrary surface point: trilinear
// over the 8 surrounding probes, point-read of each probe's octahedral tile
// in the hit normal's direction. No Chebyshev here — this feeds the low-
// frequency multi-bounce term, where leaking is dampened by albedo anyway.
static float3 giSampleFieldIrradiance(texture2d<float, access::read_write> irradiance,
                                      constant GIProbeParams& P, float3 pos, float3 n)
{
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0);
	const int probesPerRow = max(1, int(P.gridCounts.w));
	const float spacing = max(P.gridOrigin.w, 1e-4);
	const float3 gridSpace = (pos - P.gridOrigin.xyz) / spacing;
	const float3 base  = floor(gridSpace);
	const float3 fracP = gridSpace - base;
	const float2 oct = octEncodeP(n) * 0.5 + 0.5;
	const uint2 octTexel = uint2(clamp(oct * float(kGIProbeOctSize),
	                                   0.0, float(kGIProbeOctSize) - 1.0));
	float3 sum = float3(0.0);
	float  sumW = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		const float3 cell = base + offs;
		if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		const float3 tri = mix(1.0 - fracP, fracP, offs);
		const float w = tri.x * tri.y * tri.z;
		if (w <= 1e-5) continue;
		const int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
		const uint2 tile = uint2(uint((probeIndex % probesPerRow) * kGIProbeOctSize),
		                         uint((probeIndex / probesPerRow) * kGIProbeOctSize));
		sum  += irradiance.read(tile + octTexel).rgb * w;
		sumW += w;
	}
	return sum / max(sumW, 1e-4);
}

kernel void giProbeUpdate(uint2 texel   [[thread_position_in_threadgroup]],
                          uint2 batchIdx [[threadgroup_position_in_grid]],
                          texture2d<float, access::read_write> irradiance [[texture(0)]],
                          texture2d<float, access::read_write> visibility [[texture(1)]],
                          instance_acceleration_structure accel [[buffer(0)]],
                          const device float4* instanceColors  [[buffer(1)]],
                          constant GIProbeParams& P             [[buffer(2)]])
{
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	const int probeCount = gx * gy * gz;
	const int budget = int(P.rayParams.w);
	if (probeCount <= 0 || int(batchIdx.x) >= budget) return;
	const int cursorStart = int(P.rayParams.z);
	const int probeIndex  = (cursorStart + int(batchIdx.x)) % probeCount;

	// 1D probe index -> 3D grid cell -> world-space probe centre.
	const int pz = probeIndex / (gx * gy);
	const int py = (probeIndex / gx) % gy;
	const int px = probeIndex % gx;
	const float3 probePos = P.gridOrigin.xyz + float3(float(px), float(py), float(pz)) * P.gridOrigin.w;

	// Texel -> octahedral direction (texel-centre UV in [-1,1]).
	const float2 uv  = (float2(texel) + 0.5) / float(kGIProbeOctSize) * 2.0 - 1.0;
	const float3 dir = octDecode(uv);

	ray r;
	r.origin       = probePos;
	r.direction    = dir;
	r.min_distance = 0.01;
	r.max_distance = max(P.rayParams.x, 1.0);

	intersection_params params;
	intersection_query<triangle_data, instancing> q;
	q.reset(r, accel, params);
	q.next();

	float3 radiance;
	float  dist;
	if (q.get_committed_intersection_type() == intersection_type::none)
	{
		radiance = P.skyAmbient.rgb;
		dist     = P.rayParams.x; // sentinel: "far" for the visibility test
	}
	else
	{
		const uint instId  = q.get_committed_instance_id();
		// 2 float4 per instance (albedo, emissive) — this bounce uses the albedo
		// only; emissive bounce into the probe field is a deliberate non-goal
		// (it would change the scene's global lighting balance, not reflections).
		const float3 albedo = instanceColors[instId * 2].rgb;
		// One-bounce direct-light estimate: the hit normal is approximated as
		// facing back along the ray (no per-triangle normal fetch — that needs
		// binding each mesh's vertex buffer to this kernel too, a follow-up), and
		// the hit surface is treated as fully lit (no secondary shadow ray).
		// Good enough for a diffuse, low-frequency bounce estimate; not
		// physically exact — see EncodeGIProbeUpdate's header comment.
		const float3 hitNormal = -dir;
		dist = q.get_committed_distance();
		float ndl = max(dot(hitNormal, P.sunDirRadius.xyz), 0.0);
		// Secondary shadow ray: without it every hit surface counts as fully
		// sun-lit, so probes flood shadowed regions (e.g. under a large
		// occluder) with bright sun bounce — objects inside a shadow volume
		// visibly glow. One occlusion ray per texel fixes the estimate.
		if (ndl > 0.0)
		{
			ray sr;
			sr.origin       = probePos + dir * dist + hitNormal * 0.05;
			sr.direction    = P.sunDirRadius.xyz;
			sr.min_distance = 0.02;
			sr.max_distance = 10000.0;
			intersection_params sparams;
			sparams.accept_any_intersection(true);
			intersection_query<triangle_data, instancing> sq;
			sq.reset(sr, accel, sparams);
			sq.next();
			if (sq.get_committed_intersection_type() != intersection_type::none)
				ndl = 0.0;
		}
		radiance = albedo * P.sunColor.rgb * ndl;
		// Local (point/spot) lights bounce too — attenuation matches
		// fragmentMain's direct model ((1 - d/range)^2 + spot cone smoothstep).
		const float3 hitPos = probePos + dir * dist;
		const int lightCount = int(P.sunDirRadius.w);
		for (int i = 0; i < lightCount; ++i)
		{
			const float3 toL = P.lightPosRange[i].xyz - hitPos;
			const float  d   = max(length(toL), 1e-4);
			const float  range = max(P.lightPosRange[i].w, 1e-4);
			if (d >= range) continue; // outside the attenuation radius
			const float3 L = toL / d;
			const float ndl2 = max(dot(hitNormal, L), 0.0);
			if (ndl2 <= 0.0) continue;
			float atten = 1.0 - d / range;
			atten *= atten;
			if (P.lightColorType[i].w > 1.5) // spot cone
			{
				const float c       = dot(-L, normalize(P.lightDirCos[i].xyz));
				const float cosCone = P.lightDirCos[i].w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
			if (atten <= 0.0) continue;
			// Secondary occlusion ray to the light — bounce light no longer
			// leaks through geometry from local lights either.
			ray lr;
			lr.origin       = hitPos + hitNormal * 0.05;
			lr.direction    = L;
			lr.min_distance = 0.02;
			lr.max_distance = max(d - 0.1, 0.02);
			intersection_params lparams;
			lparams.accept_any_intersection(true);
			intersection_query<triangle_data, instancing> lq;
			lq.reset(lr, accel, lparams);
			lq.next();
			if (lq.get_committed_intersection_type() != intersection_type::none)
				continue;
			radiance += albedo * P.lightColorType[i].rgb * ndl2 * atten;
		}
		// Multi-bounce feedback (DDGI recursion): light already gathered in the
		// probe field re-reflects off this surface — a red wall visibly bleeds
		// red onto neighbouring geometry, and the series converges toward
		// infinite bounces through the EMA. albedo < 1 keeps it stable.
		radiance += albedo * giSampleFieldIrradiance(irradiance, P, hitPos, hitNormal);
	}

	const int probesPerRow = max(1, int(P.gridCounts.w));
	const int tileX = probeIndex % probesPerRow;
	const int tileY = probeIndex / probesPerRow;
	const uint2 outCoord = uint2(uint(tileX * kGIProbeOctSize) + texel.x,
	                             uint(tileY * kGIProbeOctSize) + texel.y);

	// Adaptive hysteresis: gather rays are DETERMINISTIC per texel (fixed
	// octahedral direction, no jitter), so a frame-to-frame delta is a REAL
	// scene change, never sampling noise. Small deltas (slow lighting drift)
	// keep the smooth base blend — no flicker; large deltas (an occluder
	// moved) drop the hysteresis so the probe converges in a few frames
	// instead of ~2 s — still blended, so no hard pop either. Static scenes
	// are byte-identical to before (delta 0 → base hysteresis).
	const float baseH = clamp(P.rayParams.y, 0.0, 0.98);
	const float4 oldIrr = irradiance.read(outCoord);
	const float irrDelta = length(radiance - oldIrr.rgb);
	const float hIrr = mix(baseH, 0.3, clamp(irrDelta * 4.0, 0.0, 1.0));
	irradiance.write(float4(mix(radiance, oldIrr.rgb, hIrr), 1.0), outCoord);
	const float4 oldVis = visibility.read(outCoord);
	const float2 newVisSample = float2(dist, dist * dist);
	const float visDelta = abs(dist - oldVis.x) / max(P.gridOrigin.w, 1.0);
	const float hVis = mix(baseH, 0.3, clamp(visDelta, 0.0, 1.0));
	visibility.write(float4(mix(newVisSample, oldVis.rg, hVis), 0.0, 0.0), outCoord);
}
)MSL";

// ─── Global Illumination: ray-traced reflections (docs/gi-reflections-plan.md) ─
// One mirror ray per half-res output pixel, reconstructed from the STORED scene
// G-buffer (GB1 normals/roughness + the R32F NDC-depth attachment), traced
// against the same TLAS the GI shadow/probe kernels use. The hit is shaded the
// way the probe kernel shades its bounce: flat instance albedo × (one sun
// occlusion ray + the DDGI probe field's irradiance at the hit) — so what a
// mirror shows agrees with how the diffuse GI lights the same surface, and
// multi-bounce arrives through the field for free. v1 shortcuts (mirrors of
// the probe kernel's documented ones): hit normal ≈ -rayDir, no per-hit UV/
// texture sample, hard sun ray (no cone jitter — the output is deterministic,
// so no temporal pass is needed either). Misses write confidence 0 — the
// composite then keeps the sky cubemap, which IS the correct sky reflection.
static const char* kGIReflMSL = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct GIReflParams {
	float4x4 invViewProj;   // scene inverse view-proj (world reconstruction, Metal conventions)
	float4x4 prevViewProj;  // LAST frame's view-proj (temporal reprojection of the receiver)
	float4 camPos;          // xyz = camera world pos, w = max ray distance
	float4 texSize;         // xy = output size, z = max roughness, w = 1 when the probe field is valid this frame
	float4 sunDirRadius;    // xyz = direction TOWARD the dominant light, w = indirect intensity
	float4 sunColor;        // rgb = dominant light colour * intensity, w = frame seed (glossy jitter)
	float4 skyAmbient;      // rgb = flat ambient (hit-shading floor outside the probe grid), w = history blend (0 = temporal off)
	float4 gridOrigin;      // xyz = probe-grid origin, w = spacing
	float4 gridCounts;      // xyz = probes per axis, w = probesPerRow
	float4 extra;           // x = glossy cone jitter on (quality 2), y = mesh data valid (true hit normals), z = SW instance count, w = max bounces (1-4)
	float4 land;            // x = landscape count (painted-terrain table), y = rays per pixel (quality tier)
};

// P4 (HW only): tier-2 argument buffer of per-unique-BLAS mesh pointers —
// written CPU-side as raw gpuAddress values (macOS 13+). Vertex layout is the
// engine's interleaved pos3+normal3+uv2 (stride 8 floats, normal at offset 3).
struct GIMeshPtrs {
	device const float* vtx;
	device const uint*  idx;
};

// Painted landscape (HE::GiLandscape) — see GiLandscape.h. Mirrored byte-for-byte
// by MetalRenderer::GILandGpu.
struct GILand {
	float4x4 worldToLocal;
	float4   cfg;      // xy = 1/(sizeX,sizeZ), z = uvTiling, w = layer count
	float4   layer[4]; // per-layer folded colour (rgb)
};

constant int kGIProbeOctSize = 8; // must match MetalRenderer::kGIProbeOctSize

// Albedo of a landscape hit, sampled at the PAINT rather than averaged over the
// whole terrain. A landscape is a heightfield over an axis-aligned local rect
// whose mesh UVs are linear in it, so the hit's UV comes straight out of the hit
// POSITION — no per-vertex UV in the acceleration structure. Weights are read
// with the same clamp+linear the rasterizer uses, so mirror and surface agree.
// Returns false when this instance is not a landscape (caller keeps its flat
// per-instance albedo).
static bool giLandscapeAlbedo(const device GILand* lands, int li, int landCount,
                              array<texture2d<float>, 4> weightTex,
                              float3 hitPos, thread float3& albedoOut)
{
	if (li < 0 || li >= landCount || li >= 4) return false;
	const device GILand& L = lands[li];
	const int layers = int(L.cfg.w);
	if (layers <= 0) return false;
	const float3 lp = (L.worldToLocal * float4(hitPos, 1.0)).xyz;
	// Local XZ → 0..1 across the terrain, then the terrain's own UV tiling —
	// exactly TerrainMeshGenerator's mapping, which is what the mesh UVs carry.
	const float2 uv = (float2(lp.x, lp.z) * L.cfg.xy + 0.5) * L.cfg.z;
	constexpr sampler wsmp(coord::normalized, address::clamp_to_edge, filter::linear);
	const float4 w = weightTex[li].sample(wsmp, uv);
	float3 sum = float3(0.0);
	float  wsum = 0.0;
	for (int i = 0; i < layers && i < 4; ++i)
	{
		const float wi = w[i];
		if (wi <= 0.0) continue;
		sum  += L.layer[i].rgb * wi;
		wsum += wi;
	}
	// Blank texel: the rasterizer's own 1e-4 floor falls back to layer 0.
	albedoOut = (wsum > 1e-4) ? sum / wsum : L.layer[0].rgb;
	return true;
}

// Same signed-octahedral mapping as kGIProbeMSL/kUnlitMSL (each MSL string is
// its own compilation unit — copies are the established pattern here).
static float3 octDecodeR(float2 e)
{
	float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0)
	{
		float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * signN;
	}
	return normalize(n);
}
static float2 octEncodeR(float3 n)
{
	float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

// Per-pixel/per-frame hash + cone sample for the quality-2 glossy jitter —
// same constructions as the GI shadow kernel (giHash2/giConeSample copies).
static float2 giHash2R(uint2 gid, float seed)
{
	float2 p = float2(gid) + seed * 13.37;
	return float2(fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453),
	              fract(sin(dot(p, float2(39.3468, 11.1352))) * 24634.6345));
}
static float3 giConeSampleR(float3 L, float angleRad, float2 xi)
{
	float3 up = (abs(L.y) < 0.99) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 T  = normalize(cross(up, L));
	float3 B  = cross(L, T);
	float  r   = sin(angleRad) * sqrt(xi.x);
	float  phi = 6.28318530718 * xi.y;
	return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

// Irradiance field at the hit point — the same trilinear 8-probe lookup the
// probe kernel's multi-bounce term uses (giSampleFieldIrradiance), with the
// accumulated weight exposed so hits OUTSIDE the probe grid can fall back to
// the flat ambient floor instead of silently reading zero.
static float3 giReflField(texture2d<float, access::read> irradiance,
                          constant GIReflParams& P, float3 pos, float3 n,
                          thread float& outW)
{
	outW = 0.0;
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0);
	const int probesPerRow = max(1, int(P.gridCounts.w));
	const float spacing = max(P.gridOrigin.w, 1e-4);
	const float3 gridSpace = (pos - P.gridOrigin.xyz) / spacing;
	const float3 base  = floor(gridSpace);
	const float3 fracP = gridSpace - base;
	const float2 oct = octEncodeR(n) * 0.5 + 0.5;
	const uint2 octTexel = uint2(clamp(oct * float(kGIProbeOctSize),
	                                   0.0, float(kGIProbeOctSize) - 1.0));
	float3 sum = float3(0.0);
	float  sumW = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		const float3 cell = base + offs;
		if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		const float3 tri = mix(1.0 - fracP, fracP, offs);
		const float w = tri.x * tri.y * tri.z;
		if (w <= 1e-5) continue;
		const int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
		const uint2 tile = uint2(uint((probeIndex % probesPerRow) * kGIProbeOctSize),
		                         uint((probeIndex / probesPerRow) * kGIProbeOctSize));
		sum  += irradiance.read(tile + octTexel).rgb * w;
		sumW += w;
	}
	outW = sumW;
	return sum / max(sumW, 1e-4);
}

kernel void giReflRay(uint2 gid [[thread_position_in_grid]],
                      texture2d<float, access::sample> gbAttr    [[texture(0)]],
                      texture2d<float, access::sample> gbDepth   [[texture(1)]],
                      texture2d<float, access::write>  outRefl   [[texture(2)]],
                      texture2d<float, access::read>   giIrr     [[texture(3)]],
                      texture2d<float, access::read>   histRad   [[texture(4)]],
                      texture2d<float, access::read>   histPos   [[texture(5)]],
                      texture2d<float, access::write>  outHistRad [[texture(6)]],
                      texture2d<float, access::write>  outHistPos [[texture(7)]],
                      texturecube<float>               skyEnv     [[texture(8)]],
                      instance_acceleration_structure  accel     [[buffer(0)]],
                      const device float4* instanceColors        [[buffer(1)]],
                      constant GIReflParams& P                    [[buffer(2)]],
                      const device GIMeshPtrs* meshes             [[buffer(3)]],
                      const device uint* instanceMesh             [[buffer(4)]],
                      const device GILand* lands                  [[buffer(5)]],
                      const device int* instanceLand              [[buffer(6)]],
                      array<texture2d<float>, 4> landWeights      [[texture(9)]])
{
	if (float(gid.x) >= P.texSize.x || float(gid.y) >= P.texSize.y) return;
	// Nearest sampling: interpolated oct-encoded normals / depths at geometry
	// edges decode to garbage directions, so pick one real texel instead.
	constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::nearest);
	constexpr sampler skySmp(coord::normalized, address::clamp_to_edge, filter::linear);
	const float2 uv = (float2(gid) + 0.5) / P.texSize.xy;
	const float d = gbDepth.sample(smp, uv).r;
	const float4 g1 = gbAttr.sample(smp, uv);
	const float rough = clamp(g1.b, 0.0, 1.0);
	// Same fade window as the SSR trace (cfg.z*0.7 → cfg.z), so both
	// reflection sources agree on which surfaces reflect at all.
	const float roughFade = 1.0 - smoothstep(P.texSize.z * 0.7, P.texSize.z, rough);
	if (d >= 1.0 || roughFade <= 0.0) // background / above the roughness cutoff
	{
		outRefl.write(float4(0.0), gid);
		outHistRad.write(float4(0.0), gid);
		outHistPos.write(float4(0.0), gid); // w = 0 → never accepted as history
		return;
	}
	// World position: Metal conventions hardcoded (top-left uv origin → NDC y
	// negated; stored NDC depth used as-is) — matches the resolve/SSR fills.
	const float4 clip = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), d, 1.0);
	const float4 wp = P.invViewProj * clip;
	const float3 Pw = wp.xyz / max(wp.w, 1e-8);
	const float3 N = octDecodeR(g1.rg * 2.0 - 1.0);
	const float3 V = normalize(Pw - P.camPos.xyz);
	float3 R = reflect(V, N);
	// Glossy sampling. The quality tier sets exactly two things (see
	// GIReflectionSettings): how many RAYS this pixel gets, and how strong the
	// blur afterwards is. Everything the reflection knows therefore comes from
	// rays it actually traced — the temporal EMA below is a bonus on top, not
	// the integrator the image depends on. It used to be the latter: ONE
	// jittered ray per pixel with the EMA expected to resolve the lobe, which
	// falls apart the moment that EMA has to be damped (camera motion, no
	// motion vectors) and made the higher tier look noisier than the lower one.
	//
	// A near-mirror cone is narrower than a pixel — every sample would trace the
	// same ray — so it stays at one, and mirrors cost exactly what they did.
	const float coneW = (P.extra.x > 0.5 && rough > 0.03)
		? min(0.30, rough * rough * 1.2) : 0.0;
	const int rays = (coneW > 1e-3) ? clamp(int(P.land.y), 1, 8) : 1;

	// Bounce loop (P.extra.w = max bounces, 1-4): a mirror-like hit (metallic,
	// low roughness — packed in the instance-shading pair) reflects ONWARD
	// instead of flattening to its base colour; `throughput` carries the metal
	// tint. A primary miss keeps confidence 0 (the composite's cubemap is the
	// exact fallback); a SECONDARY miss samples the sky cube directly — that
	// ray genuinely reflects the sky.
	float4 sampleOut = float4(0.0);
	for (int sIdx = 0; sIdx < rays; ++sIdx)
	{
		// Stratified over the sample index AND the frame, so this frame's N rays
		// spread across the lobe instead of clumping, and successive frames keep
		// filling the gaps for the EMA.
		float3 rayDir = R;
		if (coneW > 1e-3)
		{
			const float2 xi = giHash2R(gid, P.sunColor.w + float(sIdx) * 7.13);
			const float2 st = float2((float(sIdx) + xi.x) / float(rays), xi.y);
			const float3 jit = giConeSampleR(R, coneW, st);
			if (dot(jit, N) > 0.0) rayDir = jit; // keep the sample above the surface
		}
		float3 accum      = float3(0.0);
		float3 throughput = float3(1.0);
		float  conf       = 0.0;
		float3 ro = Pw + N * 0.05;
		float3 rd = rayDir;
		const int maxBounce = clamp(int(P.extra.w), 1, 4);
		for (int b = 0; b < maxBounce; ++b)
		{
			ray r;
			r.origin       = ro;
			r.direction    = rd;
			r.min_distance = 0.02;
			r.max_distance = max(P.camPos.w, 1.0);
			intersection_params params; // committed closest hit (no accept_any)
			intersection_query<triangle_data, instancing> q;
			q.reset(r, accel, params);
			q.next();
			if (q.get_committed_intersection_type() == intersection_type::none)
			{
				if (b > 0) accum += throughput * skyEnv.sample(skySmp, rd).rgb;
				break;
			}
			const uint   instId   = q.get_committed_instance_id();
			const float4 alb4     = instanceColors[instId * 2];     // rgb albedo, a metallic
			const float4 emi4     = instanceColors[instId * 2 + 1]; // rgb emissive, a roughness
			float3       albedo   = alb4.rgb;
			const float3 emissive = emi4.rgb;
			const float  dist   = q.get_committed_distance();
			const float3 hitPos = ro + rd * dist;
			// Landscape hit: replace the flat per-instance tint with the paint at
			// THIS point, or a red ridge on a green hillside mirrors as one
			// averaged colour. Non-landscape hits leave `albedo` untouched.
			{
				float3 painted;
				if (giLandscapeAlbedo(lands, instanceLand[instId], int(P.land.x),
				                      landWeights, hitPos, painted))
					albedo = painted;
			}

			// Hit normal: true interpolated vertex normal through the mesh-pointer
			// argument buffer when available (P4); -rayDir fallback otherwise
			// (same approximation the probe kernel documents).
			float3 hitN = -rd;
			if (P.extra.y > 0.5)
			{
				const GIMeshPtrs m = meshes[instanceMesh[instId]];
				const uint prim = q.get_committed_primitive_id();
				const float2 bc = q.get_committed_triangle_barycentric_coord();
				const uint i0 = m.idx[prim * 3 + 0];
				const uint i1 = m.idx[prim * 3 + 1];
				const uint i2 = m.idx[prim * 3 + 2];
				const float3 n0 = float3(m.vtx[i0 * 8 + 3], m.vtx[i0 * 8 + 4], m.vtx[i0 * 8 + 5]);
				const float3 n1 = float3(m.vtx[i1 * 8 + 3], m.vtx[i1 * 8 + 4], m.vtx[i1 * 8 + 5]);
				const float3 n2 = float3(m.vtx[i2 * 8 + 3], m.vtx[i2 * 8 + 4], m.vtx[i2 * 8 + 5]);
				const float3 nObj = n0 * (1.0 - bc.x - bc.y) + n1 * bc.x + n2 * bc.y;
				// Object → world with the committed instance transform's linear
				// part (columns 0-2 of the float4x3). Plain multiply, not the
				// inverse-transpose — fine for the engine's (near-)uniform scales.
				const float4x3 o2w = q.get_committed_object_to_world_transform();
				const float3x3 nm = float3x3(o2w[0], o2w[1], o2w[2]);
				const float3 nW = nm * nObj;
				if (dot(nW, nW) > 1e-12)
				{
					hitN = normalize(nW);
					if (dot(hitN, rd) > 0.0) hitN = -hitN; // face the incoming ray (two-sided)
				}
			}

			// Direct sun at the hit: one hard occlusion ray. Skipped when the
			// sun term is black anyway (night) — the visibility result would
			// be multiplied by zero, so the query is pure waste then.
			float ndl = max(dot(hitN, P.sunDirRadius.xyz), 0.0);
			if (dot(P.sunColor.rgb, float3(1.0)) <= 1e-4) ndl = 0.0;
			if (ndl > 0.0)
			{
				ray sr;
				sr.origin       = hitPos + hitN * 0.05;
				sr.direction    = P.sunDirRadius.xyz;
				sr.min_distance = 0.02;
				sr.max_distance = 10000.0;
				intersection_params sp2;
				sp2.accept_any_intersection(true);
				intersection_query<triangle_data, instancing> sq;
				sq.reset(sr, accel, sp2);
				sq.next();
				if (sq.get_committed_intersection_type() != intersection_type::none)
					ndl = 0.0;
			}
			float3 Lsurf = albedo * P.sunColor.rgb * ndl;
			// Indirect at the hit from the DDGI field — scaled by the same indirect
			// intensity heLitP applies, so the reflected image of a surface matches
			// how that surface is actually shaded. Outside the grid (or with GI
			// probes off) a flat ambient floor keeps reflections from going black.
			float  fieldW = 0.0;
			float3 field  = float3(0.0);
			if (P.texSize.w > 0.5)
				field = giReflField(giIrr, P, hitPos, hitN, fieldW);
			if (fieldW > 1e-4) Lsurf += albedo * field * P.sunDirRadius.w;
			else               Lsurf += albedo * P.skyAmbient.rgb;

			// Confidence (first hit — deeper bounces ride on the primary hit's):
			// the roughness cutoff, plus a fade only in the LAST QUARTER of the
			// traced range, where running out of range is the real failure mode.
			// It used to carry SSR's falloff instead — 1 - 0.25·dist/maxDist,
			// which never reaches 1 at all. SSR loses confidence as it marches
			// because its evidence thins out; a traced hit is EXACT at any
			// range, so that fade only diluted perfectly good reflections with
			// up to 25% procedural sky, as a gradient across the surface. Same
			// shape as the GL kernel's, which had it right.
			if (b == 0)
				conf = roughFade * (1.0 - smoothstep(max(P.camPos.w, 1.0) * 0.75,
				                                     max(P.camPos.w, 1.0), dist));
			// Mirror-ness of the HIT surface: metals with low roughness bounce
			// on, everything else terminates. The mirror fraction of the local
			// shading is withheld — the NEXT segment supplies it.
			const float mirror = (b + 1 < maxBounce)
				? alb4.a * (1.0 - smoothstep(0.2, 0.6, emi4.a)) : 0.0;
			accum      += throughput * (Lsurf * (1.0 - mirror) + emissive);
			throughput *= albedo * mirror;
			if (mirror <= 0.05 ||
			    (throughput.r + throughput.g + throughput.b) < 0.01) break;
			ro = hitPos + hitN * 0.05;
			rd = reflect(rd, hitN);
		}
		sampleOut += float4(accum, conf) / float(rays);
	}

	// Temporal accumulation (quality 2, gated by skyAmbient.w): reproject the
	// RECEIVER point into last frame's history and EMA-blend when the stored
	// world position matches (disocclusion reject, same scheme as the GI
	// shadow temporal). Reflected-object parallax is not reprojected (no
	// motion vectors in the engine) — instead the blend is ADAPTIVE: a large
	// luminance difference between history and the fresh sample means the
	// reflection CONTENT changed (a moving object, a light flip), so the
	// history weight collapses and the trail dies in a couple of frames,
	// while small differences (glossy jitter noise) keep the full EMA.
	float4 resultOut = sampleOut;
	const float h = P.skyAmbient.w;
	if (h > 0.0)
	{
		const float4 pc = P.prevViewProj * float4(Pw, 1.0);
		if (pc.w > 1e-4)
		{
			const float2 ndcP = pc.xy / pc.w;
			const float2 puv  = float2(ndcP.x * 0.5 + 0.5, 0.5 - ndcP.y * 0.5);
			if (all(puv >= 0.0) && all(puv <= 1.0))
			{
				const uint2 pcoord = uint2(clamp(puv * P.texSize.xy,
				                                 0.0, P.texSize.xy - 1.0));
				const float4 hp  = histPos.read(pcoord);
				const float  tol = max(0.05 * length(Pw - P.camPos.xyz), 0.1);
				if (hp.w > 0.5 && length(hp.xyz - Pw) < tol)
				{
					const float4 hist = histRad.read(pcoord);
					const float lc  = dot(sampleOut.rgb, float3(0.299, 0.587, 0.114));
					const float lh  = dot(hist.rgb,      float3(0.299, 0.587, 0.114));
					const float rel = abs(lh - lc) / (max(lh, lc) + 0.05);
					// The collapse exists for MOVING CONTENT under a still
					// camera. But with the glossy cone open, a large frame-to-
					// frame luminance difference IS the sampling noise —
					// punishing it kills the very integration that noise needs,
					// and it is self-sustaining (noisy → collapse → noisier).
					// So the collapse fades out as the cone opens: a mirror
					// (cone 0) keeps the full moving-object rejection, a glossy
					// surface trusts its history and converges.
					//
					// The GL kernel solves the same problem properly, with a 3x3
					// NEIGHBOURHOOD CLAMP (see kGiReflTemporalFS) — which keeps
					// both guarantees instead of trading one for the other. It
					// can: its temporal is a separate fullscreen pass over the
					// raw trace, so neighbouring samples exist. Here the temporal
					// is fused INTO the trace kernel, where each thread only has
					// its own sample; a threadgroup exchange would need a barrier
					// this kernel's early-outs make unsafe. Splitting the pass is
					// the real fix and is worth doing if glossy reflections of
					// MOVING objects ever smear noticeably.
					const float collapse = (coneW > 1e-3) ? 0.0 : 0.75;
					const float hEff = h * (1.0 - collapse * smoothstep(0.2, 0.8, rel));
					resultOut = mix(sampleOut, hist, hEff);
				}
			}
		}
	}
	outRefl.write(resultOut, gid);
	outHistRad.write(resultOut, gid);
	outHistPos.write(float4(Pw, 1.0), gid);
}
)MSL";

// ─── Global Illumination: SOFTWARE ray tracing (no-HW-RT fallback) ───────────
// Base-Metal compute kernels for devices/OSes without intersection_query
// support (pre-macOS-12 or !device.supportsRaytracing; HE_GI_FORCE_SW forces
// this path on RT hardware for real-HW verification). Traverses the CPU-built
// HE::GiBvh in plain buffers — the traversal mirrors GiBvh.cpp's
// giBvhIntersect() 1:1 (same slab test, Möller-Trumbore, 64-entry stack), the
// SAME algorithm the GL 4.3 port runs and tests/test_gi_bvh.cpp verifies.
// Node/instance ints travel as float bit patterns (as_type) exactly like the
// GLSL variant. Everything around these kernels (G-buffer, temporal, blur,
// probe atlases, shading) is shared with the HW path.
static const char* kGISWMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GiNode { float4 d0; float4 d1; }; // d0.xyz bmin, d0.w leftFirst (int bits), d1.xyz bmax, d1.w triCount (int bits)
struct GiTri  { float4 v0; float4 v1; float4 v2; };
struct GiInst { float4x4 invTransform; float4 baseColor; float4 emissive; int4 offsets; }; // offsets.x nodeOffset, .y triOffset, .z landscape index (-1 = none) — must match GISwInstanceCPU

// Painted landscape — same contract as the HW kernel's GILand (GiLandscape.h);
// each MSL string is its own compilation unit, so the pair is duplicated here
// exactly like GIReflParams and the octahedral helpers are.
struct GILand {
	float4x4 worldToLocal;
	float4   cfg;      // xy = 1/(sizeX,sizeZ), z = uvTiling, w = layer count
	float4   layer[4];
};

// Albedo of a landscape hit sampled at the PAINT — see the HW kernel's copy for
// why the hit POSITION is enough to recover the UV. false = not a landscape.
static bool giLandscapeAlbedo(const device GILand* lands, int li, int landCount,
                              array<texture2d<float>, 4> weightTex,
                              float3 hitPos, thread float3& albedoOut)
{
	if (li < 0 || li >= landCount || li >= 4) return false;
	const device GILand& L = lands[li];
	const int layers = int(L.cfg.w);
	if (layers <= 0) return false;
	const float3 lp = (L.worldToLocal * float4(hitPos, 1.0)).xyz;
	const float2 uv = (float2(lp.x, lp.z) * L.cfg.xy + 0.5) * L.cfg.z;
	constexpr sampler wsmp(coord::normalized, address::clamp_to_edge, filter::linear);
	const float4 w = weightTex[li].sample(wsmp, uv);
	float3 sum = float3(0.0);
	float  wsum = 0.0;
	for (int i = 0; i < layers && i < 4; ++i)
	{
		const float wi = w[i];
		if (wi <= 0.0) continue;
		sum  += L.layer[i].rgb * wi;
		wsum += wi;
	}
	albedoOut = (wsum > 1e-4) ? sum / wsum : L.layer[0].rgb;
	return true;
}

static bool giTriHit(GiTri tri, float3 o, float3 d, float tMin, float tMax, thread float& tOut)
{
	tOut = 0.0;
	const float3 e1 = tri.v1.xyz - tri.v0.xyz;
	const float3 e2 = tri.v2.xyz - tri.v0.xyz;
	const float3 p  = cross(d, e2);
	const float det = dot(e1, p);
	if (abs(det) < 1e-9) return false;
	const float invDet = 1.0 / det;
	const float3 s = o - tri.v0.xyz;
	const float u = dot(s, p) * invDet;
	if (u < 0.0 || u > 1.0) return false;
	const float3 q = cross(s, e1);
	const float v = dot(d, q) * invDet;
	if (v < 0.0 || u + v > 1.0) return false;
	const float t = dot(e2, q) * invDet;
	if (t <= tMin || t >= tMax) return false;
	tOut = t;
	return true;
}

static bool giBlasHit(const device GiNode* nodes, const device GiTri* tris,
                      int nodeOfs, int triOfs, float3 o, float3 d,
                      float tMin, float tMax, bool anyHit, thread float& tOut)
{
	tOut = tMax;
	const float3 invD = 1.0 / d;
	int stack[64];
	int sp = 0;
	stack[sp++] = nodeOfs;
	bool hit = false;
	float best = tMax;
	while (sp > 0)
	{
		GiNode n = nodes[stack[--sp]];
		const float3 t0 = (n.d0.xyz - o) * invD;
		const float3 t1 = (n.d1.xyz - o) * invD;
		const float3 lo = min(t0, t1);
		const float3 hi = max(t0, t1);
		const float tN = max(max(lo.x, lo.y), max(lo.z, tMin));
		const float tF = min(min(hi.x, hi.y), min(hi.z, best));
		if (tN > tF) continue;
		const int leftFirst = as_type<int>(n.d0.w);
		const int triCount  = as_type<int>(n.d1.w);
		if (triCount > 0)
		{
			for (int i = 0; i < triCount; ++i)
			{
				float t;
				if (giTriHit(tris[triOfs + leftFirst + i], o, d, tMin, best, t))
				{
					hit = true; best = t; tOut = t;
					if (anyHit) return true;
				}
			}
		}
		else if (sp + 2 <= 64)
		{
			stack[sp++] = nodeOfs + leftFirst;
			stack[sp++] = nodeOfs + leftFirst + 1;
		}
	}
	return hit;
}

// Linear instance loop (TLAS analogue). Object-space ray with UNNORMALISED
// direction keeps t world-comparable across instances.
static bool giSceneAnyHit(const device GiNode* nodes, const device GiTri* tris,
                          const device GiInst* insts, int instCount,
                          float3 o, float3 d, float tMin, float tMax)
{
	for (int i = 0; i < instCount; ++i)
	{
		const float3 oL = (insts[i].invTransform * float4(o, 1.0)).xyz;
		const float3 dL = (insts[i].invTransform * float4(d, 0.0)).xyz;
		float t;
		if (giBlasHit(nodes, tris, insts[i].offsets.x, insts[i].offsets.y, oL, dL, tMin, tMax, true, t))
			return true;
	}
	return false;
}

static int giSceneClosestHit(const device GiNode* nodes, const device GiTri* tris,
                             const device GiInst* insts, int instCount,
                             float3 o, float3 d, float tMin, float tMax, thread float& tOut)
{
	int   bestInst = -1;
	float best     = tMax;
	for (int i = 0; i < instCount; ++i)
	{
		const float3 oL = (insts[i].invTransform * float4(o, 1.0)).xyz;
		const float3 dL = (insts[i].invTransform * float4(d, 0.0)).xyz;
		float t;
		if (giBlasHit(nodes, tris, insts[i].offsets.x, insts[i].offsets.y, oL, dL, tMin, best, false, t))
		{
			best = t; bestInst = i;
		}
	}
	tOut = best;
	return bestInst;
}

// ── Shadow rays (same params/logic as the HW giShadowRay; frame.w carries the
// instance count on this path) ────────────────────────────────────────────────
struct GIShadowParams {
	float4 sunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
	float4 frame;        // x = jitter seed, y = tex width, z = tex height, w = instance count
	float4 localPosRange[4]; // xyz = local (point/spot) light position, w = range
	float4 extra;            // x = local light count
};
static float2 giHash2(uint2 gid, float seed)
{
	float2 p = float2(gid) + seed * 13.37;
	return float2(fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453),
	              fract(sin(dot(p, float2(39.3468, 11.1352))) * 24634.6345));
}
static float3 giConeSample(float3 L, float angleRad, float2 xi)
{
	float3 up = (abs(L.y) < 0.99) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	float3 T  = normalize(cross(up, L));
	float3 B  = cross(L, T);
	float  r   = sin(angleRad) * sqrt(xi.x);
	float  phi = 6.28318530718 * xi.y;
	return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

kernel void giShadowRaySw(uint2 gid [[thread_position_in_grid]],
                          texture2d<float, access::read>  gPos      [[texture(0)]],
                          texture2d<float, access::read>  gNorm     [[texture(1)]],
                          texture2d<float, access::write> outShadow [[texture(2)]],
                          texture2d<float, access::write> outLocal  [[texture(3)]],
                          const device GiNode* nodes [[buffer(0)]],
                          const device GiTri*  tris  [[buffer(1)]],
                          const device GiInst* insts [[buffer(2)]],
                          constant GIShadowParams& P  [[buffer(3)]])
{
	if (float(gid.x) >= P.frame.y || float(gid.y) >= P.frame.z) return;
	float4 pv = gPos.read(gid);
	if (pv.a < 0.5)
	{ outShadow.write(float4(1.0), gid); outLocal.write(float4(1.0), gid); return; }
	float3 N = normalize(gNorm.read(gid).xyz);
	float3 L = P.sunDirRadius.xyz;
	const int instCount = int(P.frame.w);

	float sunVis = 0.0;
	if (dot(N, L) > 0.0)
	{
		float2 xi  = giHash2(gid, P.frame.x);
		float3 dir = giConeSample(L, max(P.sunDirRadius.w, 1e-4), xi);
		float3 origin = pv.xyz + N * 0.05;
		sunVis = giSceneAnyHit(nodes, tris, insts, instCount, origin, dir, 0.02, 10000.0) ? 0.0 : 1.0;
	}
	outShadow.write(float4(sunVis), gid);

	// Local (point/spot) lights: hard, UNjittered occlusion rays toward the
	// first 4 (see the HW kernel's comment) — deterministic, no temporal.
	float4 localVis = float4(1.0);
	const int localCount = clamp(int(P.extra.x), 0, 4);
	for (int i = 0; i < localCount; ++i)
	{
		const float3 toL   = P.localPosRange[i].xyz - pv.xyz;
		const float  distL = length(toL);
		if (distL <= 0.05) continue;
		if (distL >= P.localPosRange[i].w) continue; // outside the attenuation radius → contributes nothing
		const float3 dirL = toL / distL;
		if (dot(N, dirL) <= 0.0) { localVis[i] = 0.0; continue; }
		if (giSceneAnyHit(nodes, tris, insts, instCount,
		                  pv.xyz + N * 0.05, dirL, 0.02, max(distL - 0.1, 0.02)))
			localVis[i] = 0.0;
	}
	outLocal.write(localVis, gid);
}

// ── Probe update (same params/logic as the HW giProbeUpdate; sunColor.w
// carries the instance count, albedo comes from the instance itself) ─────────
struct GIProbeParams {
	float4 gridOrigin;
	float4 gridCounts;
	float4 rayParams;
	float4 sunDirRadius; // xyz = toward light, w = local light count
	float4 sunColor;     // rgb = colour * intensity, w = instance count
	float4 skyAmbient;
	float4 lightPosRange[8];
	float4 lightColorType[8];
	float4 lightDirCos[8];
};
static float3 octDecode(float2 e)
{
	float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0)
	{
		float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * signN;
	}
	return normalize(n);
}
constant int kGIProbeOctSize = 8; // must match MetalRenderer::kGIProbeOctSize

// direction -> octahedral UV + previous-frame field lookup for the
// multi-bounce term — same helpers as the HW kernel (each embedded MSL
// string compiles as its own library, no shared headers).
static float2 octEncodeP(float3 n)
{
	float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

static float3 giSampleFieldIrradiance(texture2d<float, access::read_write> irradiance,
                                      constant GIProbeParams& P, float3 pos, float3 n)
{
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0);
	const int probesPerRow = max(1, int(P.gridCounts.w));
	const float spacing = max(P.gridOrigin.w, 1e-4);
	const float3 gridSpace = (pos - P.gridOrigin.xyz) / spacing;
	const float3 base  = floor(gridSpace);
	const float3 fracP = gridSpace - base;
	const float2 oct = octEncodeP(n) * 0.5 + 0.5;
	const uint2 octTexel = uint2(clamp(oct * float(kGIProbeOctSize),
	                                   0.0, float(kGIProbeOctSize) - 1.0));
	float3 sum = float3(0.0);
	float  sumW = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		const float3 cell = base + offs;
		if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		const float3 tri = mix(1.0 - fracP, fracP, offs);
		const float w = tri.x * tri.y * tri.z;
		if (w <= 1e-5) continue;
		const int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
		const uint2 tile = uint2(uint((probeIndex % probesPerRow) * kGIProbeOctSize),
		                         uint((probeIndex / probesPerRow) * kGIProbeOctSize));
		sum  += irradiance.read(tile + octTexel).rgb * w;
		sumW += w;
	}
	return sum / max(sumW, 1e-4);
}

kernel void giProbeUpdateSw(uint2 texel   [[thread_position_in_threadgroup]],
                            uint2 batchIdx [[threadgroup_position_in_grid]],
                            texture2d<float, access::read_write> irradiance [[texture(0)]],
                            texture2d<float, access::read_write> visibility [[texture(1)]],
                            const device GiNode* nodes [[buffer(0)]],
                            const device GiTri*  tris  [[buffer(1)]],
                            const device GiInst* insts [[buffer(2)]],
                            constant GIProbeParams& P   [[buffer(3)]])
{
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	const int probeCount = gx * gy * gz;
	const int budget = int(P.rayParams.w);
	if (probeCount <= 0 || int(batchIdx.x) >= budget) return;
	const int cursorStart = int(P.rayParams.z);
	const int probeIndex  = (cursorStart + int(batchIdx.x)) % probeCount;

	const int pz = probeIndex / (gx * gy);
	const int py = (probeIndex / gx) % gy;
	const int px = probeIndex % gx;
	const float3 probePos = P.gridOrigin.xyz + float3(float(px), float(py), float(pz)) * P.gridOrigin.w;

	const float2 uv  = (float2(texel) + 0.5) / float(kGIProbeOctSize) * 2.0 - 1.0;
	const float3 dir = octDecode(uv);

	float dist;
	const int instCount = int(P.sunColor.w);
	const int hitInst = giSceneClosestHit(nodes, tris, insts, instCount,
	                                      probePos, dir, 0.01, max(P.rayParams.x, 1.0), dist);

	float3 radiance;
	if (hitInst < 0)
	{
		radiance = P.skyAmbient.rgb;
		dist     = P.rayParams.x;
	}
	else
	{
		const float3 albedo    = insts[hitInst].baseColor.rgb;
		const float3 hitNormal = -dir;
		const float3 hitPos = probePos + dir * dist;
		float ndl = max(dot(hitNormal, P.sunDirRadius.xyz), 0.0);
		// Secondary shadow ray (see the HW kernel's comment): hit surfaces are
		// no longer assumed fully sun-lit.
		if (ndl > 0.0 && giSceneAnyHit(nodes, tris, insts, instCount,
		                               hitPos + hitNormal * 0.05, P.sunDirRadius.xyz, 0.02, 10000.0))
			ndl = 0.0;
		radiance = albedo * P.sunColor.rgb * ndl;
		const int lightCount = int(P.sunDirRadius.w);
		for (int i = 0; i < lightCount; ++i)
		{
			const float3 toL = P.lightPosRange[i].xyz - hitPos;
			const float  d   = max(length(toL), 1e-4);
			const float  range = max(P.lightPosRange[i].w, 1e-4);
			if (d >= range) continue; // outside the attenuation radius
			const float3 L = toL / d;
			const float ndl2 = max(dot(hitNormal, L), 0.0);
			if (ndl2 <= 0.0) continue;
			float atten = 1.0 - d / range;
			atten *= atten;
			if (P.lightColorType[i].w > 1.5)
			{
				const float c       = dot(-L, normalize(P.lightDirCos[i].xyz));
				const float cosCone = P.lightDirCos[i].w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
			if (atten <= 0.0) continue;
			// Secondary occlusion ray to the light (see the HW kernel).
			if (giSceneAnyHit(nodes, tris, insts, instCount,
			                  hitPos + hitNormal * 0.05, L, 0.02, max(d - 0.1, 0.02)))
				continue;
			radiance += albedo * P.lightColorType[i].rgb * ndl2 * atten;
		}
		// Multi-bounce feedback (DDGI recursion) — see the HW kernel.
		radiance += albedo * giSampleFieldIrradiance(irradiance, P, hitPos, hitNormal);
	}

	const int probesPerRow = max(1, int(P.gridCounts.w));
	const int tileX = probeIndex % probesPerRow;
	const int tileY = probeIndex / probesPerRow;
	const uint2 outCoord = uint2(uint(tileX * kGIProbeOctSize) + texel.x,
	                             uint(tileY * kGIProbeOctSize) + texel.y);

	// Adaptive hysteresis: gather rays are DETERMINISTIC per texel (fixed
	// octahedral direction, no jitter), so a frame-to-frame delta is a REAL
	// scene change, never sampling noise. Small deltas (slow lighting drift)
	// keep the smooth base blend — no flicker; large deltas (an occluder
	// moved) drop the hysteresis so the probe converges in a few frames
	// instead of ~2 s — still blended, so no hard pop either. Static scenes
	// are byte-identical to before (delta 0 → base hysteresis).
	const float baseH = clamp(P.rayParams.y, 0.0, 0.98);
	const float4 oldIrr = irradiance.read(outCoord);
	const float irrDelta = length(radiance - oldIrr.rgb);
	const float hIrr = mix(baseH, 0.3, clamp(irrDelta * 4.0, 0.0, 1.0));
	irradiance.write(float4(mix(radiance, oldIrr.rgb, hIrr), 1.0), outCoord);
	const float4 oldVis = visibility.read(outCoord);
	const float2 newVisSample = float2(dist, dist * dist);
	const float visDelta = abs(dist - oldVis.x) / max(P.gridOrigin.w, 1.0);
	const float hVis = mix(baseH, 0.3, clamp(visDelta, 0.0, 1.0));
	visibility.write(float4(mix(newVisSample, oldVis.rg, hVis), 0.0, 0.0), outCoord);
}

// ── Ray-traced GI reflections, SOFTWARE variant (gi-reflections-plan P5) ─────
// Mirrors the HW giReflRay kernel (kGIReflMSL) against the CPU-built BVH; the
// hit normal is the triangle's GEOMETRIC normal (the SW BVH stores full
// vertices, so it is free) instead of the HW path's interpolated vertex
// normal — flat-shaded reflections, still far better than -rayDir.
struct GIReflParams {
	float4x4 invViewProj;
	float4x4 prevViewProj;
	float4 camPos;          // xyz = camera world pos, w = max ray distance
	float4 texSize;         // xy = output size, z = max roughness, w = field valid
	float4 sunDirRadius;    // xyz = toward light, w = indirect intensity
	float4 sunColor;        // rgb = colour * intensity, w = frame seed
	float4 skyAmbient;      // rgb = ambient floor, w = history blend
	float4 gridOrigin;
	float4 gridCounts;
	float4 extra;           // x = glossy jitter on, y = (HW-only, unused), z = instance count, w = max bounces
	float4 land;            // x = landscape count (painted-terrain table), y = rays per pixel (quality tier)
};

// Closest hit that also reports WHICH triangle was hit (global index into the
// concatenated tri buffer) — the reflection kernel needs it for the normal.
static bool giBlasHitTri(const device GiNode* nodes, const device GiTri* tris,
                         int nodeOfs, int triOfs, float3 o, float3 d,
                         float tMin, float tMax, thread float& tOut, thread int& triOut)
{
	tOut = tMax;
	triOut = -1;
	const float3 invD = 1.0 / d;
	int stack[64];
	int sp = 0;
	stack[sp++] = nodeOfs;
	bool hit = false;
	float best = tMax;
	while (sp > 0)
	{
		GiNode n = nodes[stack[--sp]];
		const float3 t0 = (n.d0.xyz - o) * invD;
		const float3 t1 = (n.d1.xyz - o) * invD;
		const float3 lo = min(t0, t1);
		const float3 hi = max(t0, t1);
		const float tN = max(max(lo.x, lo.y), max(lo.z, tMin));
		const float tF = min(min(hi.x, hi.y), min(hi.z, best));
		if (tN > tF) continue;
		const int leftFirst = as_type<int>(n.d0.w);
		const int triCount  = as_type<int>(n.d1.w);
		if (triCount > 0)
		{
			for (int i = 0; i < triCount; ++i)
			{
				float t;
				if (giTriHit(tris[triOfs + leftFirst + i], o, d, tMin, best, t))
				{
					hit = true; best = t; tOut = t;
					triOut = triOfs + leftFirst + i;
				}
			}
		}
		else if (sp + 2 <= 64)
		{
			stack[sp++] = nodeOfs + leftFirst;
			stack[sp++] = nodeOfs + leftFirst + 1;
		}
	}
	return hit;
}

static int giSceneClosestHitTri(const device GiNode* nodes, const device GiTri* tris,
                                const device GiInst* insts, int instCount,
                                float3 o, float3 d, float tMin, float tMax,
                                thread float& tOut, thread int& triOut)
{
	int   bestInst = -1;
	float best     = tMax;
	triOut = -1;
	for (int i = 0; i < instCount; ++i)
	{
		const float3 oL = (insts[i].invTransform * float4(o, 1.0)).xyz;
		const float3 dL = (insts[i].invTransform * float4(d, 0.0)).xyz;
		float t;
		int tri;
		if (giBlasHitTri(nodes, tris, insts[i].offsets.x, insts[i].offsets.y,
		                 oL, dL, tMin, best, t, tri))
		{
			best = t; bestInst = i; triOut = tri;
		}
	}
	tOut = best;
	return bestInst;
}

// Field lookup with the accumulated weight exposed (out-of-grid detection) —
// the read-only twin of giSampleFieldIrradiance above.
static float3 giReflFieldSw(texture2d<float, access::read> irradiance,
                            constant GIReflParams& P, float3 pos, float3 n,
                            thread float& outW)
{
	outW = 0.0;
	const int gx = int(P.gridCounts.x), gy = int(P.gridCounts.y), gz = int(P.gridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0);
	const int probesPerRow = max(1, int(P.gridCounts.w));
	const float spacing = max(P.gridOrigin.w, 1e-4);
	const float3 gridSpace = (pos - P.gridOrigin.xyz) / spacing;
	const float3 base  = floor(gridSpace);
	const float3 fracP = gridSpace - base;
	const float2 oct = octEncodeP(n) * 0.5 + 0.5;
	const uint2 octTexel = uint2(clamp(oct * float(kGIProbeOctSize),
	                                   0.0, float(kGIProbeOctSize) - 1.0));
	float3 sum = float3(0.0);
	float  sumW = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		const float3 cell = base + offs;
		if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		const float3 tri = mix(1.0 - fracP, fracP, offs);
		const float w = tri.x * tri.y * tri.z;
		if (w <= 1e-5) continue;
		const int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
		const uint2 tile = uint2(uint((probeIndex % probesPerRow) * kGIProbeOctSize),
		                         uint((probeIndex / probesPerRow) * kGIProbeOctSize));
		sum  += irradiance.read(tile + octTexel).rgb * w;
		sumW += w;
	}
	outW = sumW;
	return sum / max(sumW, 1e-4);
}

kernel void giReflRaySw(uint2 gid [[thread_position_in_grid]],
                        texture2d<float, access::sample> gbAttr    [[texture(0)]],
                        texture2d<float, access::sample> gbDepth   [[texture(1)]],
                        texture2d<float, access::write>  outRefl   [[texture(2)]],
                        texture2d<float, access::read>   giIrr     [[texture(3)]],
                        texture2d<float, access::read>   histRad   [[texture(4)]],
                        texture2d<float, access::read>   histPos   [[texture(5)]],
                        texture2d<float, access::write>  outHistRad [[texture(6)]],
                        texture2d<float, access::write>  outHistPos [[texture(7)]],
                        texturecube<float>               skyEnv     [[texture(8)]],
                        const device GiNode* nodes [[buffer(0)]],
                        const device GiTri*  tris  [[buffer(1)]],
                        const device GiInst* insts [[buffer(2)]],
                        constant GIReflParams& P    [[buffer(3)]],
                        const device GILand* lands  [[buffer(4)]],
                        array<texture2d<float>, 4> landWeights [[texture(9)]])
{
	if (float(gid.x) >= P.texSize.x || float(gid.y) >= P.texSize.y) return;
	constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::nearest);
	constexpr sampler skySmp(coord::normalized, address::clamp_to_edge, filter::linear);
	const float2 uv = (float2(gid) + 0.5) / P.texSize.xy;
	const float d = gbDepth.sample(smp, uv).r;
	const float4 g1 = gbAttr.sample(smp, uv);
	const float rough = clamp(g1.b, 0.0, 1.0);
	const float roughFade = 1.0 - smoothstep(P.texSize.z * 0.7, P.texSize.z, rough);
	if (d >= 1.0 || roughFade <= 0.0)
	{
		outRefl.write(float4(0.0), gid);
		outHistRad.write(float4(0.0), gid);
		outHistPos.write(float4(0.0), gid);
		return;
	}
	const float4 clip = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), d, 1.0);
	const float4 wp = P.invViewProj * clip;
	const float3 Pw = wp.xyz / max(wp.w, 1e-8);
	const float3 N = octDecode(g1.rg * 2.0 - 1.0);
	const float3 V = normalize(Pw - P.camPos.xyz);
	float3 R = reflect(V, N);
	// Rays per pixel + cone width from the quality tier — see the HW kernel.
	const float coneW = (P.extra.x > 0.5 && rough > 0.03)
		? min(0.30, rough * rough * 1.2) : 0.0;
	const int rays = (coneW > 1e-3) ? clamp(int(P.land.y), 1, 8) : 1;

	// Bounce loop — mirrors the HW kernel (see its comment); geometric triangle
	// normals instead of interpolated vertex normals.
	const int instCount = int(P.extra.z);
	float4 sampleOut = float4(0.0);
	for (int sIdx = 0; sIdx < rays; ++sIdx)
	{
		float3 rayDir = R;
		if (coneW > 1e-3)
		{
			const float2 xi = giHash2(gid, P.sunColor.w + float(sIdx) * 7.13);
			const float2 st = float2((float(sIdx) + xi.x) / float(rays), xi.y);
			const float3 jit = giConeSample(R, coneW, st);
			if (dot(jit, N) > 0.0) rayDir = jit;
		}
		float3 accum      = float3(0.0);
		float3 throughput = float3(1.0);
		float  conf       = 0.0;
		float3 ro = Pw + N * 0.05;
		float3 rd = rayDir;
		const int maxBounce = clamp(int(P.extra.w), 1, 4);
		for (int b = 0; b < maxBounce; ++b)
		{
			float dist;
			int   triIdx;
			const int hitInst = giSceneClosestHitTri(nodes, tris, insts, instCount,
			                                         ro, rd, 0.02, max(P.camPos.w, 1.0),
			                                         dist, triIdx);
			if (hitInst < 0)
			{
				if (b > 0) accum += throughput * skyEnv.sample(skySmp, rd).rgb;
				break;
			}
			const float4 alb4     = insts[hitInst].baseColor; // rgb albedo, a metallic
			const float4 emi4     = insts[hitInst].emissive;  // rgb emissive, a roughness
			float3       albedo   = alb4.rgb;
			const float3 hitPos = ro + rd * dist;
			// Landscape hit → the paint at THIS point, not the terrain-wide mean.
			{
				float3 painted;
				if (giLandscapeAlbedo(lands, insts[hitInst].offsets.z, int(P.land.x),
				                      landWeights, hitPos, painted))
					albedo = painted;
			}
			// Geometric triangle normal, object → world via the row-vector product
			// with the stored INVERSE transform ((M^-1)^T · n) — two-sided.
			float3 hitN = -rd;
			if (triIdx >= 0)
			{
				const GiTri tri = tris[triIdx];
				const float3 nObj = cross(tri.v1.xyz - tri.v0.xyz, tri.v2.xyz - tri.v0.xyz);
				const float3 nW = (float4(nObj, 0.0) * insts[hitInst].invTransform).xyz;
				if (dot(nW, nW) > 1e-12)
				{
					hitN = normalize(nW);
					if (dot(hitN, rd) > 0.0) hitN = -hitN;
				}
			}
			// Occlusion ray skipped when the sun term is black anyway (night) —
			// same shortcut as the HW kernel.
			float ndl = max(dot(hitN, P.sunDirRadius.xyz), 0.0);
			if (dot(P.sunColor.rgb, float3(1.0)) <= 1e-4) ndl = 0.0;
			if (ndl > 0.0 && giSceneAnyHit(nodes, tris, insts, instCount,
			                               hitPos + hitN * 0.05, P.sunDirRadius.xyz, 0.02, 10000.0))
				ndl = 0.0;
			float3 Lsurf = albedo * P.sunColor.rgb * ndl;
			float  fieldW = 0.0;
			float3 field  = float3(0.0);
			if (P.texSize.w > 0.5)
				field = giReflFieldSw(giIrr, P, hitPos, hitN, fieldW);
			if (fieldW > 1e-4) Lsurf += albedo * field * P.sunDirRadius.w;
			else               Lsurf += albedo * P.skyAmbient.rgb;
			if (b == 0) // range-edge fade only — see the HW kernel
				conf = roughFade * (1.0 - smoothstep(max(P.camPos.w, 1.0) * 0.75,
				                                     max(P.camPos.w, 1.0), dist));
			const float mirror = (b + 1 < maxBounce)
				? alb4.a * (1.0 - smoothstep(0.2, 0.6, emi4.a)) : 0.0;
			accum      += throughput * (Lsurf * (1.0 - mirror) + emi4.rgb);
			throughput *= albedo * mirror;
			if (mirror <= 0.05 ||
			    (throughput.r + throughput.g + throughput.b) < 0.01) break;
			ro = hitPos + hitN * 0.05;
			rd = reflect(rd, hitN);
		}
		sampleOut += float4(accum, conf) / float(rays);
	}

	// Temporal accumulation — byte-identical logic to the HW kernel (adaptive
	// blend: luminance change collapses the history weight, jitter noise keeps it).
	float4 resultOut = sampleOut;
	const float h = P.skyAmbient.w;
	if (h > 0.0)
	{
		const float4 pc = P.prevViewProj * float4(Pw, 1.0);
		if (pc.w > 1e-4)
		{
			const float2 ndcP = pc.xy / pc.w;
			const float2 puv  = float2(ndcP.x * 0.5 + 0.5, 0.5 - ndcP.y * 0.5);
			if (all(puv >= 0.0) && all(puv <= 1.0))
			{
				const uint2 pcoord = uint2(clamp(puv * P.texSize.xy,
				                                 0.0, P.texSize.xy - 1.0));
				const float4 hp  = histPos.read(pcoord);
				const float  tol = max(0.05 * length(Pw - P.camPos.xyz), 0.1);
				if (hp.w > 0.5 && length(hp.xyz - Pw) < tol)
				{
					const float4 hist = histRad.read(pcoord);
					const float lc  = dot(sampleOut.rgb, float3(0.299, 0.587, 0.114));
					const float lh  = dot(hist.rgb,      float3(0.299, 0.587, 0.114));
					const float rel = abs(lh - lc) / (max(lh, lc) + 0.05);
					// The collapse exists for MOVING CONTENT under a still
					// camera. But with the glossy cone open, a large frame-to-
					// frame luminance difference IS the sampling noise —
					// punishing it kills the very integration that noise needs,
					// and it is self-sustaining (noisy → collapse → noisier).
					// So the collapse fades out as the cone opens: a mirror
					// (cone 0) keeps the full moving-object rejection, a glossy
					// surface trusts its history and converges.
					//
					// The GL kernel solves the same problem properly, with a 3x3
					// NEIGHBOURHOOD CLAMP (see kGiReflTemporalFS) — which keeps
					// both guarantees instead of trading one for the other. It
					// can: its temporal is a separate fullscreen pass over the
					// raw trace, so neighbouring samples exist. Here the temporal
					// is fused INTO the trace kernel, where each thread only has
					// its own sample; a threadgroup exchange would need a barrier
					// this kernel's early-outs make unsafe. Splitting the pass is
					// the real fix and is worth doing if glossy reflections of
					// MOVING objects ever smear noticeably.
					const float collapse = (coneW > 1e-3) ? 0.0 : 0.75;
					const float hEff = h * (1.0 - collapse * smoothstep(0.2, 0.8, rel));
					resultOut = mix(sampleOut, hist, hEff);
				}
			}
		}
	}
	outRefl.write(resultOut, gid);
	outHistRad.write(resultOut, gid);
	outHistPos.write(float4(Pw, 1.0), gid);
}
)MSL";

// ─── Procedural skybox (drawn into the HDR target behind the scene) ─────────
static const char* kSkyMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SkyOut { float4 position [[position]]; float2 ndc; };
struct SkyParams {
	float4x4 invViewProj;
	float4 sunDir;        // xyz = sun dir, w = hasMoon (>0.5)
	float4 sunColor;      // xyz = sun colour, w = moonPhase (0/1 new .. 0.5 full)
	float4 params;        // x = timeOfDay, y = coverage, z = wall-clock time, w = aurora intensity
	float4 nebulaColor;   // xyz = nebula colour 1, w = nebula intensity
	float4 auroraColor;   // xyz = aurora base colour, w = milkyWay intensity
	float4 wind;          // xyz = cloud drift /s, w = lightning flash
	float4 cameraPos;     // xyz = camera world pos, w = cloudMode (>0.5 = 3D volumetric)
	float4 cloud;         // x = cloudHeight, y = cloudDensity, z = cloudFluffiness, w = contrailAmount
	float4 cloudTint;     // xyz = cloud tint, w = cirrusAmount
	float4 cirrus;        // x = cirrusSeed, y = auroraHeight, z = auroraFragmentation, w = nebulaSeed
	float4 nebulaColor2;  // xyz = nebula colour 2, w = nebulaQuality (0 Perf / 1 High / 2 Max)
	float4 nebulaColor3;  // xyz = nebula colour 3, w = god-ray (crepuscular) strength
	float4 auroraColorTop;// xyz = aurora top colour, w = shooting-star (meteor) frequency
	float4 starColor;     // xyz = star colour, w = starBrightness
	float4 star;          // x = starSize, y = starSizeVariation, z = starDensity, w = starGlow
	float4 star2;         // x = starTwinkle, y = cloudQuality, z = low-res-cloud flag, w = rainAmount
	float4 neb2;          // x = nebulaCoverage (0 none .. 1 whole band)
};

vertex SkyOut skyVertex(uint vid [[vertex_id]])
{
	float x = float((vid & 1) << 2) - 1.0;
	float y = float((vid & 2) << 1) - 1.0;
	SkyOut o;
	o.position = float4(x, y, 1.0, 1.0); // far plane
	o.ndc      = float2(x, y);
	return o;
}

//#SKYFUNC#

// Self-contained 2D value-noise fBm for the procedural lunar surface. Mirrors GL.
float moonHash(float2 p){ p = fract(p * float2(127.1, 311.7)); p += dot(p, p + 34.56); return fract(p.x * p.y); }
float moonNoise(float2 p){ float2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
	return mix(mix(moonHash(i), moonHash(i + float2(1,0)), u.x),
	           mix(moonHash(i + float2(0,1)), moonHash(i + float2(1,1)), u.x), u.y); }
float moonFbm(float2 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 4; ++i){ v += a * moonNoise(p); p *= 2.03; a *= 0.5; } return v; }

// Textured moon disk — drawn only in the sky pass (kept out of the shared
// skyColor() so the scene's image-based ambient needn't bind the texture).
// Procedural lunar albedo (maria seas + cratered highlands + a Tycho-like ray
// system) blended with the optional real texture, lit by the lunar phase.
// Mirrors the GL moonDisk() exactly.
float3 moonDisk(float3 dir, float3 sunDir, bool hasMoon, float moonPhase,
                texture2d<float> moonTex, sampler moonSamp)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day   = smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	float night = 1.0 - day;
	if (night <= 0.0) return float3(0.0);

	float3 moonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return float3(0.0);

	// Local tangent frame so the disk gets 2D UVs for the texture.
	float3 right = normalize(cross(float3(0.0, 1.0, 0.0), moonDir));
	float3 up    = cross(moonDir, right);
	const float kRadius = 0.030;                   // angular radius (< the sun disk)
	float2 q = float2(dot(dir, right), dot(dir, up)) / kRadius;
	float  r = length(q);
	if (r > 1.0) return float3(0.0);

	// Sphere normal (z toward viewer) + a UV that bulges toward the limb for a rounder wrap.
	float  z   = sqrt(max(1.0 - r * r, 0.0));
	float2 uv  = q / (0.55 + 0.45 * z);
	// ---- Procedural lunar SURFACE ALBEDO (maria seas + cratered highlands + ray system) ----
	float hl    = moonFbm(uv * 2.0 + 11.0);                       // highland mottle (bright base)
	float albedo = 0.74 + 0.16 * (hl - 0.5);
	float mar   = moonFbm(uv * 0.95 + 4.0);
	float maria = smoothstep(0.44, 0.60, mar);
	albedo = mix(albedo, 0.22 + 0.07 * (moonFbm(uv * 3.0 + 20.0) - 0.5), maria);
	float cm    = moonFbm(uv * 6.0 + 31.0);
	albedo *= 0.82 + 0.20 * smoothstep(0.30, 0.72, cm);
	albedo += 0.05 * (moonFbm(uv * 16.0 + 50.0) - 0.5);          // fine grain
	// A bright young crater with a RAY system (Tycho-like).
	float2 tc   = float2(0.10, -0.40);
	float  td   = length(uv - tc);
	float  tang = atan2(uv.y - tc.y, uv.x - tc.x);              // GLSL atan(y,x) -> MSL atan2
	float  rayN = moonFbm(float2(tang * 3.0, 1.7));
	float  rays = pow(0.5 + 0.5 * sin(tang * 22.0 + rayN * 9.0), 3.0);
	rays *= smoothstep(0.85, 0.12, td) * smoothstep(0.05, 0.10, td);
	albedo += rays * 0.20;
	albedo += smoothstep(0.060, 0.048, td) * 0.22;               // bright crater rim
	albedo -= smoothstep(0.048, 0.022, td) * 0.14;               // darker crater floor
	albedo = clamp(albedo, 0.12, 1.05);
	float tex  = hasMoon ? moonTex.sample(moonSamp, q * 0.5 + 0.5).r : 1.0;
	albedo *= mix(1.0, tex, hasMoon ? 0.55 : 0.0);              // blend the real texture if present
	// ---- PHASE: light the sphere from the sun direction in the moon-view frame ----
	float3 N   = float3(q, z);                                   // surface normal toward the viewer
	float  ph  = moonPhase * 6.2831853;
	float3 L   = float3(sin(ph), 0.0, -cos(ph));                 // sun direction across the disk
	float  ndl = dot(normalize(N), L);
	float  illum = smoothstep(-0.06, 0.08, ndl);                // soft day/night terminator
	illum = max(illum, 0.025 * (1.0 - illum));                  // faint earthshine on the dark side
	float  limb = 0.55 + 0.45 * z;                              // mild edge darkening
	float  edge = smoothstep(1.0, 0.93, r);                     // soft anti-aliased rim
	float3 tint = float3(0.92, 0.93, 0.99);
	return tint * (albedo * illum * limb * edge * 1.3 * night);
}

// Procedural star field — drawn only in the sky pass (like the moon). Fades in
// at night, sits above the horizon and is occluded by clouds (applied before
// applyClouds()). Each view ray lands in one cell of a fixed grid on a large
// sphere shell (stable, pole-skew free); the rarest cells host a small round
// star at a hashed sub-cell position. Mirrors the GL starField() exactly.
float starHash(float3 p)
{
	p  = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}
// Rotate a view ray into the slowly turning celestial frame (one full turn per
// day about a tilted pole) — Rodrigues' rotation. Mirrors GL celestialDir().
float3 celestialDir(float3 dir, float timeOfDay)
{
	float  a    = timeOfDay * 6.2831853;
	float3 axis = normalize(float3(0.22, 0.92, 0.32));
	float c = cos(a), s = sin(a);
	return dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0 - c);
}
// Gaussian galactic band: ~1 on the Milky-Way plane, 0 toward the poles.
float galacticBand(float3 cdir)
{
	const float3 galN = normalize(float3(0.46, 0.52, -0.72));
	float d = dot(normalize(cdir), galN);
	return exp(-d * d * 7.0);
}
// 3D value noise (trilinear) from the star hash + a small fBm. The nebula is
// sampled in 3D on the celestial sphere so it reads as isotropic blobs instead
// of the radial streaks a 2D plane projection produces at grazing angles.
// Trilinear value noise from the precomputed 3D volume (texels hold starHash at
// the integer lattice). Pre-smoothstepping the fractional coordinate makes the
// hardware filter reproduce the former smoothstep value noise (within the 256
// tile); +0.5 lands lattice points on texel centres. Mirrors the GL starNoise3.
float starNoise3(float3 p, texture3d<float> noiseTex, sampler noiseSamp)
{
	float3 f = fract(p);
	float3 q = floor(p) + f * f * (3.0 - 2.0 * f) + 0.5;
	return noiseTex.sample(noiseSamp, q * (1.0 / 256.0)).r;
}
float starFbm3(float3 p, int oct, texture3d<float> noiseTex, sampler noiseSamp)
{
	float v = 0.0, amp = 0.5;
	for (int i = 0; i < oct; ++i) { v += amp * starNoise3(p, noiseTex, noiseSamp); p *= 2.03; amp *= 0.5; }
	return v;
}
// Dark dust lanes of the Milky Way (the "Great Rift"). Shared by starField + nebula
// so the lane darkens both coherently. Mirrors the GL mwRift().
float mwRift(float3 cN, texture3d<float> noiseTex, sampler noiseSamp)
{
	cN = normalize(cN);
	float n  = starFbm3(cN * 1.9 + 211.0, 2, noiseTex, noiseSamp);
	float r  = 1.0 - abs(n - 0.5) * 2.0;          // ridge at n≈0.5 → a winding dark centreline
	float lane = smoothstep(0.72, 0.96, r);       // narrow, distinct winding rift
	float n2 = starFbm3(cN * 3.4 + 67.0, 2, noiseTex, noiseSamp);
	lane = max(lane, smoothstep(0.80, 0.99, 1.0 - abs(n2 - 0.5) * 2.0) * 0.75);
	return clamp(lane, 0.0, 1.0);
}
// Star brightness + colour tint are applied at the call site (* starColor * starBright).
float3 starField(float3 dir, float3 cdir, float3 sunDir, float time, float milkyWay,
                 float starSize, float starSizeVar, float starDensity, float starGlow, float starTwinkle,
                 texture3d<float> noiseTex, sampler noiseSamp)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.14, 0.06, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return float3(0.0);

	float  band   = galacticBand(cdir);
	float  mw     = clamp(milkyWay, 0.0, 1.0);
	// Density sets the BASE threshold for the whole sky: at 0 it goes above 1.0 so no
	// cell qualifies (zero stars); the band lowers it once there ARE stars.
	float  dens   = clamp(starDensity, 0.0, 1.0);
	float  baseTh = mix(1.001, 0.79, dens);
	float  rift   = band > 0.04 ? mwRift(cdir, noiseTex, noiseSamp) : 0.0;
	float  thresh = baseTh - band * mix(0.07, 0.20, mw) * dens
	              + rift * band * 0.22;
	float3 p      = cdir * 105.0;                  // denser cells → more, finer stars
	float  pix    = max(length(fwidth(p)), 1e-4); // screen-space footprint (AA floor)
	float3 ip     = floor(p);
	float  horizon = smoothstep(0.0, 0.15, dir.y);
	float  szVar   = clamp(starSizeVar, 0.0, 1.0);

	// Splat stars from the 3×3×3 neighbourhood in ABSOLUTE p-space (no cell-boundary clip).
	float3 acc = float3(0.0);
	for (int gz = -1; gz <= 1; ++gz)
	for (int gy = -1; gy <= 1; ++gy)
	for (int gx = -1; gx <= 1; ++gx)
	{
		float3 cell    = ip + float3(float(gx), float(gy), float(gz));
		float  present = starHash(cell);
		if (present < thresh) continue;

		float3 sp = cell + float3(starHash(cell + 1.7), starHash(cell + 4.3), starHash(cell + 8.9));
		float  d  = length(p - sp);                    // absolute distance → no clip
		float  sizeH = starHash(cell + 5.7);
		float  skew  = mix(sizeH, sizeH * sizeH * sizeH, 0.7);
		float  sz    = mix(0.45, skew, szVar);
		// starSize controls the on-screen DIAMETER: it scales the gaussian radius
		// directly, and the screen-space term is demoted to a sub-pixel anti-alias
		// FLOOR (not a hard size). Previously the floor (pix*1.6) sat above the radius
		// across the whole slider, so the slider only nudged the very largest stars.
		float  radius = mix(0.16, 0.40, sz) * starSize;
		float  sigma  = clamp(max(radius, pix * 0.6), 0.0, 0.70);
		float  core  = exp(-(d * d) / (sigma * sigma));
		core = core * core;
		float  halo  = exp(-(d * d) / (sigma * sigma * 3.5)) * sz * sz * 0.14 * starGlow;
		float  win   = smoothstep(1.0, 0.6, d);
		float  shape = (core * 1.8 + halo) * win;
		float  mag   = (0.4 + 0.6 * smoothstep(thresh, 1.0, present)) * mix(0.8, 2.6, sz);
		float  twa     = clamp(starTwinkle, 0.0, 1.0);
		float  twPhase = starHash(cell + 23.5) * 6.2831;
		float  twFreq  = 2.0 + 4.0 * starHash(cell + 47.1);
		float  tw      = (1.0 - 0.5 * twa) + 0.5 * twa * sin(time * twFreq + twPhase);
		float3 tint    = mix(float3(0.80, 0.88, 1.0), float3(1.0, 0.93, 0.82), starHash(cell + 12.1));
		acc += tint * (shape * mag * tw);
	}
	float bandDim = mix(1.6, mix(0.9, 1.5, mw), band);
	return acc * (horizon * night * bandDim * (1.0 - 0.6 * rift * band));
}

// Shooting stars / meteors. A few independent "slots" each spawn a meteor once per cycle;
// the meteor is a thin bright streak (head + short tail) that arcs across the upper sky and
// fades over its short life. Deterministic from the sky clock so it animates smoothly and
// reproduces in headless captures. Night-only. rate (0..1) scales frequency + concurrency.
// Mirrors the GL shootingStars().
float3 shootingStars(float3 dir, float3 sunDir, float time, float rate,
                     float3 starTint, float starBright, float starSize, float starSizeVar)
{
	if (rate <= 0.0) return float3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.05) return float3(0.0);

	float  r      = clamp(rate, 0.0, 1.0);
	int    slots  = 1 + int(r * 3.0);                  // 1..4 concurrent meteor slots
	float  period = mix(9.0, 3.5, r);                  // seconds between meteors per slot
	// ONE shared RADIANT per "shower" (re-rolled every few minutes): all meteors
	// stream away from the same sky point — near-parallel trails far from it,
	// gently diverging around it — instead of criss-crossing at random.
	float shower = floor(time / 700.0);
	float azR = starHash(float3(shower + 0.5, 4.2, 9.1)) * 6.2831853;
	float elR = 0.45 + 0.75 * starHash(float3(shower + 0.5, 2.8, 5.5));
	float3 R   = normalize(float3(cos(azR) * cos(elR), sin(elR), sin(azR) * cos(elR)));
	float3 Ru  = normalize(cross(float3(0.0, 1.0, 0.0), R));   // tangent basis at the radiant
	float3 Rv  = cross(R, Ru);
	float3 col    = float3(0.0);
	for (int k = 0; k < slots; ++k)
	{
		float tk  = time / period + float(k) * 1.37;
		float idx = floor(tk);
		float ph  = fract(tk);
		float dur = 0.16;                              // visible fraction of the cycle
		if (ph > dur) continue;
		float t = ph / dur;                            // 0..1 along the streak's life

		float3 seed = float3(idx * 1.7 + 0.3, float(k) * 7.3 + 1.1, idx * 0.31 + float(k) * 3.9);
		// Spawn at a random bearing/distance AROUND the radiant, then travel
		// along the great circle AWAY from it (real shower geometry).
		float  phiS = starHash(seed) * 6.2831853;
		float  dst  = 0.35 + 0.75 * starHash(seed + 2.1);  // angular distance from the radiant
		// Meteor size follows the star settings: starSize scales width/head,
		// starSizeVar spreads individual meteors between small and large.
		float  mSz  = clamp(starSize, 0.25, 3.0)
		            * mix(1.0, mix(0.6, 1.8, starHash(seed + 7.7)), clamp(starSizeVar, 0.0, 1.0));
		float3 p0   = normalize(R * cos(dst) + (Ru * cos(phiS) + Rv * sin(phiS)) * sin(dst));
		float3 tdir = normalize(p0 * dot(R, p0) - R);      // tangent pointing away from the radiant
		// Tiny per-meteor tilt so the trails aren't machine-parallel.
		tdir = normalize(tdir + cross(p0, tdir) * ((starHash(seed + 5.7) - 0.5) * 0.12));
		float  arc  = 0.5 + 0.4 * starHash(seed + 9.9); // angular travel over the life
		float3 head = normalize(p0 + tdir * (t * arc));
		float3 tail = normalize(p0 + tdir * (t * arc - 0.30)); // tail end trailing behind the head

		// Closest point on the head→tail chord (small-arc approximation in direction space).
		float3 seg = tail - head;
		float  s   = clamp(dot(dir - head, seg) / max(dot(seg, seg), 1e-5), 0.0, 1.0);
		float  dd  = length(dir - (head + seg * s));
		float  w      = mix(0.0045, 0.0014, s) * mSz;                  // taper: wider at the head, thin at the tail
		float  streak = exp(-(dd * dd) / (w * w)) * pow(1.0 - s, 1.6); // brightest at the head, fading down the tail
		float  dh     = length(dir - head);
		float  headG  = exp(-(dh * dh) / (0.00006 * mSz * mSz));       // small sharp head (≈0.45° at size 1)
		float  life   = smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.55, 1.0, t));
		// Colour + brightness follow the star settings (same knobs as starField).
		float3 mcol   = float3(0.78, 0.88, 1.0) * starTint;            // cool blue-white meteor, user-tinted
		col += mcol * ((streak * 1.7 + headG * 1.1) * life * starBright);
	}
	float horizon = smoothstep(0.0, 0.12, dir.y);
	return col * night * horizon;
}

// Procedural volumetric clouds — drawn only in the sky pass (kept out of the
// shared skyColor() so the scene's image-based ambient stays cheap). Density is
// a 3D noise field (reusing starNoise3/starFbm3) animated by the continuous wall
// clock — NOT the looping time-of-day — so clouds drift, form and dissolve with
// their own lifecycle and never snap at the 0h/24h day wrap. A short raymarch
// through a cloud slab with Beer's-law transmittance + a sun light-march gives a
// soft, self-shadowed volumetric look. Mirrors the GL applyClouds() exactly.
float cloudHash(float2 p)
{
	p  = fract(p * float2(127.1, 311.7));
	p += dot(p, p + 34.56);
	return fract(p.x * p.y);
}
float cloudNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = fract(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	float a = cloudHash(i);
	float b = cloudHash(i + float2(1.0, 0.0));
	float c = cloudHash(i + float2(0.0, 1.0));
	float d = cloudHash(i + float2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float cloudFbm(float2 p)
{
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 5; ++i)
	{
		v += a * cloudNoise(p);
		p  = p * 2.02;
		a *= 0.5;
	}
	return v;
}
// Cirrus fBm: a SEPARATE 2D fBm (per-octave domain rotation + detuned lacunarity 1.92)
// for the high fibrous mare's-tail streaks. Mirrors the GL cirrusFbm().
float cirrusFbm(float2 p)
{
	float v = 0.0, a = 0.5;
	float2x2 rot = float2x2(float2(0.80, 0.60), float2(-0.60, 0.80));
	for (int i = 0; i < 5; ++i) { v += a * cloudNoise(p); p = rot * p * 1.92; a *= 0.5; }
	return v;
}
// Cloud slab heights (arbitrary world units in the sky-ray hemisphere model).
// Taller slab than a thin sheet so the billows have vertical room to read as
// towering cumuli instead of a flat horizon band.
constant float kCloudBase  = 1.0;
constant float kCloudTop   = 2.6;
constant float kCloudScale = 1.2;    // spatial frequency of the cloud field
// Worley (cellular) lookup from the noise volume's G channel — bright at the cell
// feature points. fBm of it is the billowy cumulus shape. The bake already tiles,
// so a plain trilinear fetch is enough (Worley is C0-smooth).
float worleyNoise3(float3 p, texture3d<float> noiseTex, sampler noiseSamp)
{
	return noiseTex.sample(noiseSamp, p * (1.0 / 256.0)).g;
}
float worleyFbm(float3 p, texture3d<float> noiseTex, sampler noiseSamp)
{
	return worleyNoise3(p, noiseTex, noiseSamp)        * 0.625
	     + worleyNoise3(p * 2.03, noiseTex, noiseSamp) * 0.25
	     + worleyNoise3(p * 4.06, noiseTex, noiseSamp) * 0.125;
}
// Henyey-Greenstein phase: forward-biased scattering so the cloud edges facing the
// sun glow (the golden sunset rim / silver lining). g>0 peaks toward the light.
float hgPhase(float cosT, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}
// Cloud drift direction/speed comes from the user wind control (SkyParams.wind),
// passed down as a parameter so the noise field scrolls the clouds across the sky.
// Rounded vertical density taper so the slab reads as puffy bodies, not a sheet.
float cloudHeightGrad(float y)
{
	float hf = clamp((y - kCloudBase) / (kCloudTop - kCloudBase), 0.0, 1.0);
	return smoothstep(0.0, 0.25, hf) * (1.0 - smoothstep(0.6, 1.0, hf));
}
// Full density at a world point: billowy Worley (the cauliflower shape) over a
// large-scale perlin coverage field, thresholded by the coverage slider and shaped
// by the slab height. time = continuous wall clock. The slab-height taper is a pure
// analytic function of pos.y, so test it FIRST and bail with zero texture fetches
// when the sample is outside the slab (matters most for the sun light-march, whose
// samples step up out of the slab toward the sun). Mirrors the GL cloudDensity.
float cloudDensity(float3 pos, float time, float coverage, float3 wind,
                   texture3d<float> noiseTex, sampler noiseSamp)
{
	float hgrad = cloudHeightGrad(pos.y);
	if (hgrad <= 0.0) return 0.0;                                  // outside slab → no fetches
	float3 p      = pos * kCloudScale + wind * time;
	float  morph  = time * 0.030;                                 // slow forming/dissolving
	float  perlin = starFbm3(p + float3(0.0, morph, 0.0), 4, noiseTex, noiseSamp); // coverage
	float  billow = worleyFbm(p * 0.9 + float3(morph, 0.0, 0.0), noiseTex, noiseSamp); // fine cauliflower
	float  base   = perlin * 0.5 + billow * 0.55;
	float  lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
	return smoothstep(lo, lo + 0.13, base) * hgrad;
}
// Density for the sun light-march. Slightly fewer octaves than the view density
// (shadows are lower-frequency); the slab-height test bails with zero fetches when
// the sun-ward sample steps out of the slab.
float cloudShadowDensity(float3 pos, float time, float coverage, float3 wind,
                         texture3d<float> noiseTex, sampler noiseSamp)
{
	float hgrad = cloudHeightGrad(pos.y);
	if (hgrad <= 0.0) return 0.0;
	float3 p      = pos * kCloudScale + wind * time;
	float  morph  = time * 0.030;
	float  perlin = starFbm3(p + float3(0.0, morph, 0.0), 3, noiseTex, noiseSamp);
	float  billow = worleyNoise3(p * 0.9 + float3(morph, 0.0, 0.0), noiseTex, noiseSamp) * 0.7
	              + worleyNoise3(p * 1.8, noiseTex, noiseSamp) * 0.3;
	float  base   = perlin * 0.5 + billow * 0.55;
	float  lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
	return smoothstep(lo, lo + 0.13, base) * hgrad;
}
float3 applyClouds(float3 baseSky, float3 dir, float3 sunDir, float time, float coverage, float3 sunColor, float3 wind,
                   float3 cloudTint, float densityMul, float quality,
                   texture3d<float> noiseTex, sampler noiseSamp, thread float& outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;          // clear sky → skip the whole raymarch
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

	// Quality (perf knob, star2.y): 0 Low, 1 Med, 2 High. High == the original
	// step counts; Med/Low trade horizon detail for frames. The cheap gate below
	// makes every level cheaper than the old always-full-density march.
	int qBaseN  = (quality < 0.5) ? 8  : (quality < 1.5 ? 12 : 16);
	int qMaxN   = (quality < 0.5) ? 18 : (quality < 1.5 ? 32 : 64);
	int qShadow = (quality < 0.5) ? 1  : (quality < 1.5 ? 2  : 3);

	// March the view ray through the cloud slab between base and top heights.
	// A deterministic per-ray offset breaks up otherwise coherent sample planes
	// that show up as visible horizontal cloud layers near grazing view angles.
	float s0 = kCloudBase / max(dir.y, 1e-3);
	float s1 = kCloudTop  / max(dir.y, 1e-3);
	int N = int(clamp(float(qBaseN) / max(dir.y, 0.12), float(qBaseN), float(qMaxN))); // denser toward horizon
	float ds = (s1 - s0) / float(N);
	float jitter = cloudHash(dir.xz * 173.3 + float2(dir.y * 37.1, dir.y * 19.7));

	// Day/night/dusk drive the cloud colour (independent of the drift clock).
	// Same windows skyColor() uses, so the clouds and the sky behind them enter and
	// leave sunset together instead of the clouds snapping to moonlit blue while
	// the sky is still glowing.
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.14, 0.04, sunY) * (1.0 - smoothstep(0.04, 0.26, sunY));

	// Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	float lo = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0)); // coverage threshold (for the cheap gate)
	float  T = 1.0;                                // transmittance along the view ray
	float3 L = float3(0.0);                        // accumulated in-scattered colour
	for (int i = 0; i < N; ++i)
	{
		float  s   = s0 + (float(i) + jitter) * ds;
		float3 pos = dir * s;
		float  hgrad = cloudHeightGrad(pos.y);
		if (hgrad <= 0.0) continue;
		// Inline cloudDensity() with an EXACT coverage gate. base = perlin*0.5 +
		// billow*0.55 and billow ≤ 1, so (perlin*0.5 + 0.55) is a true upper bound:
		// where it can't reach the threshold, no cloud can form here → skip the Worley
		// fetch + the sun light-march. Uses the SAME 4-octave perlin as cloudDensity,
		// so it never culls a real cloud (a lower-octave estimate could). The dome slab
		// is fully within s0..s1, so before this every step paid full density+shadow.
		float3 pp     = pos * kCloudScale + wind * time;
		float  morph  = time * 0.030;
		float  perlin = starFbm3(pp + float3(0.0, morph, 0.0), 4, noiseTex, noiseSamp);
		if (perlin * 0.5 + 0.55 < lo) continue;
		float  billow = worleyFbm(pp * 0.9 + float3(morph, 0.0, 0.0), noiseTex, noiseSamp);
		float  dens   = smoothstep(lo, lo + 0.13, perlin * 0.5 + billow * 0.55) * hgrad;
		if (dens > 0.001)
		{
			// Light-march toward the sun: Beer's-law self-shadowing (qShadow steps;
			// scaled by 3/qShadow so fewer steps don't brighten the clouds).
			float shadow = 0.0;
			for (int j = 1; j <= qShadow; ++j)
				shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25), time, coverage, wind, noiseTex, noiseSamp);
			float sun    = exp(-shadow * 1.7 * (3.0 / float(qShadow)));
			float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
			float lit    = sun * powder;

			// Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
			float3 dayCol   = mix(float3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			// Moonlit crown. Was nearly twenty times the night sky's own radiance,
			// which is what made night clouds read as a lit overcast floating over
			// a black sky; a real moonlit cloud is a few times the sky, not twenty.
			float3 nightCol = mix(float3(0.015, 0.018, 0.035), float3(0.13, 0.15, 0.24), lit);
			float3 cloudCol = mix(nightCol, dayCol, day);
			float3 duskTop  = sunColor * float3(1.5, 0.85, 0.42);
			// 0.35 floor so the whole body glows golden at dawn/dusk, lit faces more.
			cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
			// Moonlit silver: moon rises on the opposite arc from the sun.
			float3 cMoonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
			float  cMoonUp  = clamp((cMoonDir.y + 0.10) / 0.25, 0.0, 1.0);
			cloudCol += float3(0.20, 0.22, 0.38) * lit * cMoonUp * (1.0 - day) * 0.25;
			// Forward-scatter glow: Henyey-Greenstein-weighted direct sunlight makes
			// the sun-facing edges flare gold (the silver lining), strongest when
			// looking toward the sun and where the cloud isn't self-shadowed.
			cloudCol += sunColor * mix(float3(1.0), float3(1.25, 0.78, 0.42), dusk) * (phase * sun * 0.75 * max(day, dusk));
			// Cheap vertical depth: tops catch the light (bright crown), the base
			// sits in self-shadow (darker, cooler) — fakes the volumetric
			// "cauliflower" relief from just the sample's height in the slab.
			float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
			cloudCol *= mix(0.5, 1.15, hTone);
			cloudCol += float3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.25);
			cloudCol *= cloudTint;                          // user colour tint (dome path)

			float opticalDepth = dens * ds * 7.0 * clamp(densityMul, 0.0, 3.0);
			float a = 1.0 - exp(-opticalDepth);
			L += T * a * cloudCol;
			T *= 1.0 - a;
			if (T < 0.02) break;
		}
	}

	// Fade the whole cloud layer out into the horizon haze (wider so the grazing band
	// melts into the haze instead of showing undersampling speckle).
	float horizon = smoothstep(0.03, 0.22, dir.y);
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// Interleaved-gradient noise — blue-noise-like screen-space dither for raymarch
// ray-start jitter (shared by applyClouds3D + applyAurora3D). Mirrors GL skyIgn().
float skyIgn(float2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

// Coverage value-noise fBm with DISTANCE OCTAVE-LOD: the two highest-frequency octaves
// fade to zero far away (farW: 1 near → 0 far) so distant coarsely-stepped clouds keep
// only the smooth base shape and stop aliasing into speckle. Matches starFbm3(p,4).
float cloudCoverFbm(float3 p, float farW, texture3d<float> noiseTex, sampler noiseSamp)
{
	float v = 0.5 * starNoise3(p, noiseTex, noiseSamp);
	p *= 2.03; v += 0.25   * starNoise3(p, noiseTex, noiseSamp);
	p *= 2.03; v += 0.125  * starNoise3(p, noiseTex, noiseSamp) * farW;
	p *= 2.03; v += 0.0625 * starNoise3(p, noiseTex, noiseSamp) * farW * farW;
	return v;
}
// Worley billow fBm with the fine octave distance-faded (same procedural-LOD idea).
float cloudBillowFbm(float3 p, float farW, texture3d<float> noiseTex, sampler noiseSamp)
{
	return worleyNoise3(p, noiseTex, noiseSamp)        * 0.625
	     + worleyNoise3(p * 2.03, noiseTex, noiseSamp) * 0.25
	     + worleyNoise3(p * 4.06, noiseTex, noiseSamp) * 0.125 * farW;
}

// ── Cloud deck: what the altitude does, and what it must NOT do ─────────────
// cloudHeight is the deck's ABSOLUTE world altitude. Cloud SIZE, slab THICKNESS
// and the grazing-angle fade are pinned to this reference instead of to the
// altitude — otherwise raising the slider scales height, cloud size and
// thickness together, which is a similarity transform: the sky looks
// unchanged, and the only visible effect is the horizon fade eating the deck
// from below. (The 1/cloudH size compensation made sense while the layer hung
// camera-relative; with an absolute altitude it is exactly wrong.) The value
// is the previous default, so a scene at 200 renders as before and every
// higher value now genuinely lifts the deck: clouds move up and shrink.
constant float kCloudRefAltitude = 200.0;
constant float kCloudElevFloor   = 0.06;   // was clamp((cloudH-50)/2500) — grew with altitude

// ── Cloud slab intersection ──────────────────────────────────────────────────
// Entry/exit distance of a view ray through the cloud deck, for ANY camera
// position: below it (the normal case), inside it (flying through — the march
// then starts at the camera) and above it looking down (the ray enters at the
// TOP, so the near hit is the far plane in Y terms). The deck is at an ABSOLUTE
// world altitude, which is what makes climbing above it possible at all: while
// it hung at camera.y + cloudHeight it rose with the viewer and could never be
// reached. Returns false when the ray never meets the slab in front of the
// camera. Mirrored verbatim in the GL shader.
bool cloudSlabRange(float3 camPos, float3 dir, float baseY, float topY, float maxDist,
                    thread float& tNear, thread float& tFar)
{
	if (abs(dir.y) < 1e-4)
	{
		// Horizontal ray: either we are inside the deck (march the full
		// distance through it) or we never enter it.
		if (camPos.y < baseY || camPos.y > topY) return false;
		tNear = 0.0;
		tFar  = maxDist;
		return true;
	}
	float ta = (baseY - camPos.y) / dir.y;
	float tb = (topY  - camPos.y) / dir.y;
	tNear = min(ta, tb);
	tFar  = max(ta, tb);
	tNear = max(tNear, 0.0);          // inside the slab, or it starts behind us
	tFar  = min(tFar, maxDist);
	return tFar > tNear;
}

// ── Shared cloud shape field ─────────────────────────────────────────────────
// Coarse density (presence × vertical profile × billow erosion, no fine octave)
// of the 3D cloud slab at a world position. ONE shape model for the realistic
// view march, the sun light-march and the cloud-shadow map, so ground shadows
// and cloud self-shading always match the shapes overhead.
//   style 0 (Classic): the original formula — rounded fBm blobs, soft base.
//   style 1 (Realistic, HZD-style): the low-frequency Worley field carves the
//     value-noise coverage into CONNECTED cauliflower formations (Perlin-Worley
//     in spirit), the base is a flat condensation level, tops lean downwind
//     (wind shear), the erosion noise boils upward (convection) and a slow
//     FORMATION field makes local coverage breathe — because tower height is
//     coupled to coverage, clouds visibly grow, tower and dissolve as they
//     drift. evo scales all evolution SPEEDS (0 = frozen shapes, drift only).
// (The classic VIEW march below stays untouched/byte-identical — it does not
// call this; only the classic shadow map shares the classic branch here.)
float cloudFieldDensity(float3 pos, float baseY, float thick, float nscale, float lo,
                        float time, float3 wind, float fluff, float style, float evo,
                        texture3d<float> noiseTex, sampler noiseSamp)
{
	float hf = clamp((pos.y - baseY) / thick, 0.0, 1.0);
	if (style > 0.5)
	{
		// Wind shear: tops lean downwind. World-space offset before noise
		// scaling; strength follows the actual wind speed (|wind| is per-sec).
		float wlen = length(wind.xz);
		if (wlen > 1e-5)
			pos.xz += wind.xz * ((hf * hf * (0.35 / nscale) * min(wlen * 40.0, 1.0)) / wlen);
		// COLUMN-WISE coverage (HZD weather-map idea): presence + tower height
		// are sampled at LOW frequency on a FIXED noise slice, constant along
		// the column — a true 2D weather map. Fixed (not baseY): the slab rides
		// camera-relative in Y, and sampling the columns at baseY made the
		// whole cloud PATTERN morph when the camera climbed — and the ground
		// shadows with it. The 3D body/erosion below still vary with height.
		float3 npc = float3(pos.x, 0.0, pos.z) * (nscale * 0.55) + wind * time;
		npc.y += 37.0; // fixed weather-map slice (wind is horizontal; evolution scrolls y at sample time)
		// DOMAIN WARP: bend the column field with a low-frequency vector noise
		// so cells stop reading as same-sized round balls — outlines become
		// irregular, stretched, organic.
		npc.x += (starNoise3(npc * 0.35 + 17.0, noiseTex, noiseSamp) - 0.5) * 1.7;
		npc.z += (starNoise3(npc * 0.35 + 71.0, noiseTex, noiseSamp) - 0.5) * 1.7;
		// farW = 0 → only the two broad octaves decide WHERE clouds are;
		// renormalized by the dropped octaves' amplitude (0.75) so the coverage
		// threshold keeps the same meaning as the 4-octave classic field.
		float cover = cloudCoverFbm(npc + float3(0.0, time * 0.02 * evo, 0.0), 0.0, noiseTex, noiseSamp)
		            * (1.0 / 0.75);
		// Extra MACRO octave: very-low-frequency variation so formations differ
		// in size — lone puffs next to long banks, not a uniform sprinkle.
		float macro = starNoise3(npc * 0.20 + 53.0, noiseTex, noiseSamp);
		cover += (macro - 0.5) * 0.26;
		// Perlin-Worley base: the low-frequency Worley field CLUSTERS the
		// coverage around its cell centres (zero-mean, mild — a strong weight
		// here is what made every cloud the same round ball).
		float w = worleyNoise3(npc * 0.75, noiseTex, noiseSamp);
		cover += (w - 0.5) * 0.16;
		// Slow formation field: local coverage breathes over time (zero-mean).
		float form = starNoise3(npc * 0.22 + float3(time * 0.013 * evo, 31.0, -time * 0.009 * evo),
		                        noiseTex, noiseSamp);
		cover += (form - 0.5) * 0.14;
		// WIDE presence transition (and early onset to keep the coverage
		// calibration): a narrow window leaves only a paper-thin shell where
		// env is intermediate, and the carve erosion below then has no volume
		// to sculpt — the clouds stay smooth balls with rough skin. The wide
		// band gives the erosion a thick rind to cut real lobes out of.
		float pres = smoothstep(lo - 0.05, lo + 0.30, cover);
		if (pres <= 0.0) return 0.0;
		// Tower height varies per formation (macro): some stay flat banks,
		// others billow into towers — not one uniform dome height.
		float towerTop = mix(0.28, 1.0, smoothstep(lo, lo + 0.30, cover))
		               * mix(0.55, 1.25, macro);
		float rise  = smoothstep(0.0, 0.06, hf);               // FLAT base
		float crown = 1.0 - smoothstep(towerTop * 0.55, towerTop, hf);
		if (crown <= 0.0) return 0.0;
		// 3D body: rounded interior lumps inside the column envelope. Fuller
		// where the column is strong, broken toward the formation edges.
		float3 np   = pos * nscale + wind * time;
		float body  = cloudCoverFbm(np + float3(0.0, time * 0.02 * evo, 0.0), 1.0, noiseTex, noiseSamp);
		float bodyD = smoothstep(0.30, 0.60, body + pres * 0.10);
		float env   = pres * rise * crown * bodyD;             // smooth envelope 0..1
		if (env <= 0.0) return 0.0;
		// HZD-style REMAP erosion: the Worley billow carves the envelope from
		// the OUTSIDE — a thin shell (env < carve) breaks into rounded lobes,
		// the deep core survives. This is what sculpts the cauliflower
		// silhouette; a plain multiply only dims, it never re-shapes.
		// TWO-SCALE carve: the coarse billow shapes the big lobes, a second
		// higher-frequency Worley octave cuts V-shaped creases along its cell
		// borders — the angular edges and corners real cumulus have, instead
		// of one smooth rounded shell.
		float billow = cloudBillowFbm(np * 1.2 + float3(0.0, -time * 0.08 * evo, 0.0), 1.0,
		                              noiseTex, noiseSamp) * 0.62
		             + worleyNoise3(np * 2.4 + float3(3.0, -time * 0.06 * evo, 0.0),
		                            noiseTex, noiseSamp) * 0.38;
		float carve  = billow * mix(0.55, 0.85, fluff);
		float x      = (env - carve) / max(1.0 - carve, 1e-3);
		// MIST SKIRT: a low-density veil in the zone the carve just cut away —
		// it hugs every lobe as a soft evaporating fringe, so the crisp cores
		// keep their shape but the outline stops being knife-sharp.
		float mist = 0.17 * smoothstep(-0.40, 0.0, min(x, 0.0)) * rise;
		return clamp(max(x, 0.0) + mist, 0.0, 1.0);
	}
	// Classic branch — MUST stay term-for-term the cloud-shadow map's original
	// formula (the ground shadows of classic scenes must not change).
	float3 np = pos * nscale + wind * time;
	float cover = cloudCoverFbm(np + float3(0.0, time * 0.03, 0.0), 1.0, noiseTex, noiseSamp);
	float pres  = smoothstep(lo, lo + 0.26, cover);
	if (pres <= 0.0) return 0.0;
	float towerTop = mix(0.32, 1.0, smoothstep(lo, lo + 0.30, cover));
	float rise     = smoothstep(0.0, 0.18, hf);
	rise *= rise;
	float crown    = 1.0 - smoothstep(towerTop * 0.55, towerTop, hf);
	float vshape   = rise * crown;
	if (vshape <= 0.0) return 0.0;
	float billow = cloudBillowFbm(np * 1.2 + float3(time * 0.03, 0.0, 0.0), 1.0, noiseTex, noiseSamp);
	float erLo   = mix(0.30, 0.14, fluff);
	float erBite = mix(0.30, 0.62, fluff);
	float erode  = mix(1.0, smoothstep(erLo, erLo + erBite, billow),
	                   mix(0.45, 1.0, hf) * (0.55 + 0.45 * fluff));
	return pres * vshape * erode;
}

// 3D volumetric clouds (cloud mode 1): a WORLD-ANCHORED slab cloudH world units above
// the camera, sampled at absolute world positions so clouds parallax as the camera
// moves. Noise frequency scales with cloudH so angular cloud size is height-invariant.
// fragCoord = SkyOut.position.xy (gl_FragCoord equivalent). Mirrors the GL applyClouds3D().
float3 applyClouds3D(float3 baseSky, float3 dir, float3 camPos, float3 sunDir, float time,
                     float coverage, float3 sunColor, float3 wind, float cloudH,
                     float cloudFluffiness, float cloudDensity, float quality, float3 cloudTint, float2 fragCoord,
                     texture3d<float> noiseTex, sampler noiseSamp, thread float& outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// Quality (perf knob, star2.y): 0 Low, 1 Med, 2 High. High == original counts.
	float qStepF  = (quality < 0.5) ? 0.40 : (quality < 1.5 ? 0.30 : 0.22); // larger → fewer steps
	float qMinN   = (quality < 0.5) ? 12.0 : (quality < 1.5 ? 18.0 : 24.0);
	float qMaxN   = (quality < 0.5) ? 40.0 : (quality < 1.5 ? 72.0 : 128.0);
	int   qShadow = (quality < 0.5) ? 1    : (quality < 1.5 ? 2    : 3);

	cloudH      = max(cloudH, 1.0);
	float thick = kCloudRefAltitude * 1.5;                   // TALL slab so cumuli can billow upward (3D)
	float baseY = cloudH;                         // ABSOLUTE world altitude of the deck
	float maxDist = cloudH * 60.0;                // fade clouds beyond this (∝ altitude)
	float tNear, tFar;
	if (!cloudSlabRange(camPos, dir, baseY, baseY + thick, maxDist, tNear, tFar))
		return baseSky;

	int   N  = int(clamp((tFar - tNear) / (thick * qStepF), qMinN, qMaxN));
	float ds = (tFar - tNear) / float(N);
	float jitter = skyIgn(fragCoord);             // blue-noise-like dither, not coarse speckle

	// Same windows skyColor() uses, so the clouds and the sky behind them enter and
	// leave sunset together instead of the clouds snapping to moonlit blue while
	// the sky is still glowing.
	float sunY  = clamp(sunDir.y, -0.3, 1.0);
	float day   = smoothstep(-0.10, 0.10, sunY);
	float dusk  = smoothstep(-0.14, 0.04, sunY) * (1.0 - smoothstep(0.04, 0.26, sunY));
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	float nscale = 1.6 / kCloudRefAltitude;                  // FULL inverse compensation → height-invariant size
	float elevFloor = kCloudElevFloor;
	float fluff   = clamp(cloudFluffiness, 0.0, 1.0);
	float densMul = clamp(cloudDensity, 0.0, 3.0);
	float lo      = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));

	float  T = 1.0;
	float3 L = float3(0.0);
	for (int i = 0; i < N; ++i)
	{
		float  t   = tNear + (float(i) + jitter) * ds;
		float3 pos = camPos + dir * t;            // WORLD position → parallax
		float  hf  = clamp((pos.y - baseY) / thick, 0.0, 1.0);
		float3 np  = pos * nscale + wind * time;
		float  detailFade = 1.0 - smoothstep(maxDist * 0.10, maxDist * 0.40, t);
		float  cover = cloudCoverFbm(np + float3(0.0, time * 0.03, 0.0), detailFade, noiseTex, noiseSamp);
		float  pres  = smoothstep(lo, lo + mix(0.42, 0.20, detailFade), cover);
		if (pres <= 0.0) continue;
		float  towerTop = mix(0.32, 1.0, smoothstep(lo, lo + 0.30, cover));
		float  rise     = smoothstep(0.0, 0.18, hf);
		rise *= rise;                                            // rounder, fuller bottom
		float  crown    = 1.0 - smoothstep(towerTop * 0.55, towerTop, hf);
		float  vshape   = rise * crown;
		if (vshape <= 0.0) continue;
		float  billow  = cloudBillowFbm(np * 1.2 + float3(time * 0.03, 0.0, 0.0), detailFade, noiseTex, noiseSamp);
		float  billow2 = worleyNoise3(np * 2.8 + float3(0.0, time * 0.05, 7.0), noiseTex, noiseSamp);
		float  fineW   = 0.40 * fluff * detailFade;
		float  billowM = billow * (1.0 - fineW) + billow2 * fineW;
		float  erLo    = mix(0.30, 0.14, fluff);
		float  erBite  = mix(0.30, 0.62, fluff);
		erBite        = mix(0.80, erBite, detailFade);
		float  erode   = mix(1.0, smoothstep(erLo, erLo + erBite, billowM),
		                     mix(0.45, 1.0, hf) * (0.55 + 0.45 * fluff));
		float  dens    = pres * vshape * erode;
		if (dens > 0.001)
		{
			float shadow = 0.0;
			for (int j = 1; j <= qShadow; ++j)
			{
				float3 sp  = pos + sunDir * (float(j) * thick * 0.22);
				float  shf = clamp((sp.y - baseY) / thick, 0.0, 1.0);
				float  shg = smoothstep(0.0, 0.25, shf) * (1.0 - smoothstep(0.6, 1.0, shf));
				if (shg <= 0.0) continue;
				float3 snp = sp * nscale + wind * time;
				float  p2  = starFbm3(snp + float3(0.0, time * 0.03, 0.0), 3, noiseTex, noiseSamp);
				float  b2  = worleyNoise3(snp * 0.9 + float3(time * 0.03, 0.0, 0.0), noiseTex, noiseSamp) * 0.7
				           + worleyNoise3(snp * 1.8, noiseTex, noiseSamp) * 0.3;
				shadow += smoothstep(lo, lo + 0.13, p2 * 0.5 + b2 * 0.55) * shg;
			}
			float sun    = exp(-shadow * 1.7 * (3.0 / float(qShadow)));
			float powder = 1.0 - exp(-dens * mix(3.0, 4.5, fluff));
			float lit    = sun * powder;
			float3 dayCol   = mix(float3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			// Moonlit crown. Was nearly twenty times the night sky's own radiance,
			// which is what made night clouds read as a lit overcast floating over
			// a black sky; a real moonlit cloud is a few times the sky, not twenty.
			float3 nightCol = mix(float3(0.015, 0.018, 0.035), float3(0.13, 0.15, 0.24), lit);
			float3 cloudCol = mix(nightCol, dayCol, day);
			float3 duskTop  = sunColor * float3(1.5, 0.85, 0.42);
			cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
			// Twilight fill: with the sun down, what lights a cloud IS the twilight
			// sky around it. Taking it from baseSky rather than a constant ties the
			// two together — the clouds cannot stay lit once the sky has gone out.
			// (3D path only: the dome path's low-res pre-pass calls it with
			// baseSky = 0, so the same term there would not survive the round trip.)
			cloudCol += baseSky * ((1.0 - day) * (0.30 + 0.50 * lit));
			cloudCol += sunColor * mix(float3(1.0), float3(1.25, 0.78, 0.42), dusk) * (phase * sun * 0.75 * max(day, dusk));
			cloudCol *= mix(0.30, 1.32, hf);                     // strong base→crown contrast (3D relief)
			cloudCol += float3(0.07, 0.10, 0.17) * ((1.0 - hf) * day * 0.25);
			cloudCol *= cloudTint;                               // user colour tint
			float hazeFar = smoothstep(maxDist * 0.35, maxDist, t);
			cloudCol = mix(cloudCol, baseSky, hazeFar * 0.6);    // aerial perspective

			float distFade     = 1.0 - smoothstep(maxDist * 0.5, maxDist, t);
			float opticalDepth = dens * (ds / thick) * 7.0 * distFade * densMul;
			float a = 1.0 - exp(-opticalDepth);
			L += T * a * cloudCol;
			T *= 1.0 - a;
			if (T < 0.02) break;
		}
	}
	// Grazing-angle fade, symmetric in |dir.y|: near-horizontal rays cross an
	// enormous stretch of slab and would otherwise pile into a hard wall of
	// cloud at the horizon. abs() so it works looking DOWN from above the deck
	// exactly as it always did looking up from below.
	float horizon = smoothstep(elevFloor, elevFloor + 0.14, abs(dir.y));
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// ── Realistic 3D clouds (cloudStyle 1) ───────────────────────────────────────
// Same slab geometry as applyClouds3D, but shape + lighting rebuilt from the
// HZD/Nubis playbook against reference photos:
//  * shapes/evolution from cloudFieldDensity (Perlin-Worley formations, flat
//    bases, wind shear, convective boil, slow formation field) + a fine
//    upward-boiling erosion octave for the crisp cauliflower silhouette,
//  * SUN MARCH over the SAME density field with exponentially growing steps —
//    with interShadows the reach extends past the cloud's own body, so a tall
//    tower visibly darkens the clouds behind it,
//  * Wrenninge-style normalized multi-scatter (3 octaves) keeps thick cores
//    luminous instead of clamping to black,
//  * deeper blue-grey shading for the bellies, near-white sunlit tops, and a
//    strong forward lobe for the silver lining around the sun.
float3 applyClouds3DReal(float3 baseSky, float3 dir, float3 camPos, float3 sunDir, float time,
                         float coverage, float3 sunColor, float3 wind, float cloudH,
                         float cloudFluffiness, float cloudDensity, float quality, float3 cloudTint,
                         float interSh, float evo, float2 fragCoord,
                         texture3d<float> noiseTex, sampler noiseSamp, thread float& outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// Slightly higher minimum step count than classic: the sharper silhouettes
	// show the IGN dither earlier than the soft classic bodies do.
	float qStepF  = (quality < 0.5) ? 0.40 : (quality < 1.5 ? 0.28 : 0.20);
	float qMinN   = (quality < 0.5) ? 14.0 : (quality < 1.5 ? 26.0 : 32.0);
	float qMaxN   = (quality < 0.5) ? 44.0 : (quality < 1.5 ? 80.0 : 128.0);
	int   qShadow = (quality < 0.5) ? 3    : (quality < 1.5 ? 4    : 6);
	if (interSh < 0.5) qShadow = min(qShadow, 3); // short march: own body only

	cloudH      = max(cloudH, 1.0);
	float thick = kCloudRefAltitude * 1.5;
	float baseY = cloudH;                         // ABSOLUTE world altitude of the deck
	float maxDist = cloudH * 60.0;
	float tNear, tFar;
	if (!cloudSlabRange(camPos, dir, baseY, baseY + thick, maxDist, tNear, tFar))
		return baseSky;

	int   N  = int(clamp((tFar - tNear) / (thick * qStepF), qMinN, qMaxN));
	float ds = (tFar - tNear) / float(N);
	float jitter = skyIgn(fragCoord);

	float sunY  = clamp(sunDir.y, -0.3, 1.0);
	float day   = smoothstep(-0.10, 0.10, sunY);
	// NARROWER dusk window than classic: it must not start while the sky is
	// still night — pre-dawn the clouds were glowing fully orange under a
	// starry sky (the realistic powder floor amplified what classic hid).
	// Alpenglow now begins only just before the sun actually clears -3°.
	float dusk  = smoothstep(-0.05, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.26, sunY));
	float costh = max(dot(dir, sunDir), 0.0);
	// Dual lobe + a strong forward peak → silver lining hugging the sun.
	float phase = mix(hgPhase(costh, 0.65), hgPhase(costh, -0.35), 0.30)
	            + hgPhase(costh, 0.93) * 0.35;

	float nscale    = 1.6 / kCloudRefAltitude;
	float elevFloor = kCloudElevFloor;
	float fluff     = clamp(cloudFluffiness, 0.0, 1.0);
	float densMul   = clamp(cloudDensity, 0.0, 3.0);
	float lo        = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));

	float  T = 1.0;
	float3 L = float3(0.0);
	for (int i = 0; i < N; ++i)
	{
		float  t   = tNear + (float(i) + jitter) * ds;
		float3 pos = camPos + dir * t;
		float  hf  = clamp((pos.y - baseY) / thick, 0.0, 1.0);
		float  detailFade = 1.0 - smoothstep(maxDist * 0.10, maxDist * 0.40, t);
		float dens = cloudFieldDensity(pos, baseY, thick, nscale, lo, time, wind,
		                               fluff, 1.0, evo, noiseTex, noiseSamp);
		if (dens <= 0.002) continue;
		// Fine cauliflower octave, boiling upward with the convection. Bites
		// hardest at the silhouette (low density) so the outline breaks into
		// crisp lobes while the core stays solid.
		float3 np    = pos * nscale + wind * time;
		float  bfine = worleyNoise3(np * 2.6 + float3(0.0, -time * 0.10 * evo, 5.0),
		                            noiseTex, noiseSamp);
		float  fineW = (0.50 + 0.35 * fluff) * detailFade
		             * (1.0 - 0.45 * clamp(dens * 2.5, 0.0, 1.0));
		dens *= mix(1.0, smoothstep(0.15, 0.55, bfine), fineW);
		dens *= smoothstep(0.0, 0.05, dens); // kill only true dust — the mist skirt stays
		if (dens <= 0.002) continue;

		// Sun march over the same field; the FIRST step is tight so each
		// cauliflower lobe shades its neighbour (per-lobe relief), the
		// exponential tail reaches ~0.5 thick (3 steps, own body) or ~2.3 thick
		// (6 steps — interShadows: neighbouring towers darken this cloud).
		float od = 0.0;
		{
			float  st = thick * 0.05;
			float3 sp = pos;
			for (int j = 0; j < qShadow; ++j)
			{
				sp += sunDir * st;
				float shf = (sp.y - baseY) / thick;
				if (shf > 1.35) break;                 // left the slab upward
				od += cloudFieldDensity(sp, baseY, thick, nscale, lo, time, wind,
				                        fluff, 1.0, evo, noiseTex, noiseSamp) * (st / thick) * 10.0;
				st *= 1.85;
			}
		}
		// Normalized multi-scatter (Wrenninge): Σ aⁱ·exp(-od·k·bⁱ) / Σ aⁱ —
		// normalized so od = 0 → exactly 1 (unnormalized it would overbrighten
		// the tops by 1.75× and burn out before the tonemap).
		float ms = 0.0, wsum = 0.0, aa = 1.0, bb = 1.0;
		for (int o = 0; o < 3; ++o)
		{
			ms += aa * exp(-od * 5.5 * bb);
			wsum += aa;
			aa *= 0.5; bb *= 0.35;
		}
		float sun    = ms / wsum;
		float powder = 1.0 - exp(-dens * mix(3.5, 5.0, fluff));
		// Powder FLOOR: the thin mist skirt must read as luminous sunlit haze,
		// not as grey soot — darkness should come from the sun march (od), not
		// from low local density.
		float lit    = sun * mix(0.55, 1.0, powder);
		// Blue-grey skylit belly → near-white sunlit top; night/dusk logic kept
		// from classic so the clouds enter and leave sunset with the sky.
		float3 dayCol   = mix(float3(0.30, 0.35, 0.46), sunColor * 1.30, lit);
		float3 nightCol = mix(float3(0.015, 0.018, 0.035), float3(0.13, 0.15, 0.24), lit);
		float3 cloudCol = mix(nightCol, dayCol, day);
		float3 duskTop  = sunColor * float3(1.5, 0.85, 0.42);
		cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
		cloudCol += baseSky * ((1.0 - day) * (0.30 + 0.50 * lit));
		cloudCol += sunColor * mix(float3(1.0), float3(1.25, 0.78, 0.42), dusk)
		          * (phase * sun * 0.9 * max(day, dusk));
		// Flat dark base → bright crown. Milder than classic's 0.30..1.32 ramp —
		// the sun march already carries most of the vertical contrast.
		cloudCol *= mix(0.45, 1.15, smoothstep(0.0, 0.55, hf));
		cloudCol += float3(0.06, 0.09, 0.15) * ((1.0 - hf) * day * 0.20); // sky bounce under the base
		cloudCol *= cloudTint;
		float hazeFar = smoothstep(maxDist * 0.35, maxDist, t);
		cloudCol = mix(cloudCol, baseSky, hazeFar * 0.6);

		float distFade     = 1.0 - smoothstep(maxDist * 0.5, maxDist, t);
		float opticalDepth = dens * (ds / thick) * 12.0 * distFade * densMul;
		float a = 1.0 - exp(-opticalDepth);
		L += T * a * cloudCol;
		T *= 1.0 - a;
		if (T < 0.02) break;
	}
	// Grazing-angle fade, symmetric in |dir.y|: near-horizontal rays cross an
	// enormous stretch of slab and would otherwise pile into a hard wall of
	// cloud at the horizon. abs() so it works looking DOWN from above the deck
	// exactly as it always did looking up from below.
	float horizon = smoothstep(elevFloor, elevFloor + 0.14, abs(dir.y));
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// ── Cloud-shadow map pass ────────────────────────────────────────────────────
// Renders the cloud slab's sun transmittance over a world-space XZ region
// around the camera into a small R8 target (one fullscreen triangle per frame).
// Each texel = a point on the slab's MID-PLANE; a short march along the sun
// direction through the slab accumulates the SAME density field applyClouds3D
// raymarches (coverage fBm → presence → tower profile → billow erosion, fine
// octave skipped — map texels are ~20 m), so ground shadows line up with the
// clouds overhead. The lit shaders project fragments along L onto the
// mid-plane and sample this map (cloudShadowFactor / heCloudShadowFactor).
// region: xy = region origin (world XZ), z = region world size, w = map size px.
fragment float4 cloudShadowFragment(SkyOut in [[stage_in]],
                                    constant SkyParams& p [[buffer(0)]],
                                    constant float4& region [[buffer(1)]],
                                    texture3d<float> noiseTex [[texture(1)]],
                                    sampler noiseSamp [[sampler(1)]])
{
	float2 uv = in.position.xy / max(region.w, 1.0);
	float2 xz = region.xy + uv * region.z;
	float3 sd = normalize(p.sunDir.xyz);
	if (sd.y <= 0.05) return float4(1.0);
	float coverage = clamp(p.params.y, 0.0, 1.0);
	if (coverage <= 0.0) return float4(1.0);
	float time    = p.params.z;
	float3 wind   = p.wind.xyz;
	float cloudH  = max(p.cloud.x, 1.0);
	float thick   = cloudH * 1.5;
	float baseY   = cloudH;   // ABSOLUTE deck altitude — same slab the view march uses
	float midY    = baseY + 0.5 * thick;
	float fluff   = clamp(p.cloud.z, 0.0, 1.0);
	float densMul = clamp(p.cloud.y, 0.0, 3.0);
	float lo      = mix(0.70, 0.22, coverage);
	float nscale  = 1.6 / kCloudRefAltitude;
	// Slab entry/exit along the sun ray through the mid-plane point. Density
	// comes from the SHARED cloudFieldDensity (style/evolution included), so
	// the ground shadows always match the shapes overhead.
	float t0 = (baseY - midY) / sd.y;
	float t1 = (baseY + thick - midY) / sd.y;
	const int M = 6;
	float ds = (t1 - t0) / float(M);
	float od = 0.0;
	for (int i = 0; i < M; ++i)
	{
		float3 pos = float3(xz.x, midY, xz.y) + sd * (t0 + (float(i) + 0.5) * ds);
		// Same optical-depth normalisation as the view march (ds/thick * 7).
		od += cloudFieldDensity(pos, baseY, thick, nscale, lo, time, wind, fluff,
		                        p.neb2.y, p.neb2.w, noiseTex, noiseSamp)
		    * (ds / thick) * 7.0 * densMul;
	}
	return float4(exp(-od));
}

// Nebula filament line: a single thin iso-contour of a value-noise field.
// Level sets of a smooth field are closed loops around its extrema; the web is
// built from the UNION of several INDEPENDENT single-iso families (different
// fetches/offsets) so loops cross instead of nesting — crossing walls read as
// a cracked cellular cage (JWST-Crab), never as concentric onion rings.
// starFbm3(p,1) is bell-shaped around ~0.25, so the iso sits on the histogram
// FLANK (thin loops; an iso at the mode would flood). Mirrors the GL helper.
float nebIso(float n, float iso, float w)
{
	return 1.0 - smoothstep(w * 0.7, w * 1.7, abs(n - iso));
}

// Space nebula v3 — DISCRETE GAS PIECES, modelled on the JWST Crab image:
//   * the nebula is a set of solid Worley-cell CHUNKS (warped + eroded), each
//     with a defined RIM band in the warm cage colours, a cool interior that
//     whitens toward the centre, and SILK striations layered along the piece's
//     own silhouette (fixed constellations inside the formation),
//   * a CRACKLE CAGE of thin warm filaments — iso-contour cell webs at 2/3/4
//     scales (Perf/High/Max) — forms the VEINS threading each interior and
//     runs densest on the rim; the strongest wall centres run ionization-hot,
//   * COVERAGE sets how MANY pieces exist and how BIG they grow; as pieces
//     densify, their F1 skirts fuse at the cell saddles → NECKS with fibrous
//     strands CONNECT neighbouring pieces automatically,
//   * BEADS — Worley corner knots — stud the filament junctions (High/Max),
//   * thick emissive AMBER DUST concentrations sit on the pieces in rare
//     patches, plus thin reddening lanes with a warm backlit rim,
//   * (Max) a dimmed BACK web across the halo zone and extra neck fray.
// Night/horizon gated, band-gated, seeded; occluded by clouds. Mirrors GL.
float3 nebula(float3 dir, float3 cdir, float3 sunDir, float intensity, float3 nebColor,
              float3 nebColor2, float3 nebColor3, float nebulaSeed, float nebQuality,
              float nebCover, texture3d<float> noiseTex, sampler noiseSamp)
{
	bool hifi = nebQuality >= 0.5;   // 1 High, 2 Max → richer warp/web/silk
	bool maxq = nebQuality >= 1.5;   // 2 Max → back web + neck fray + extra silk/beads
	float cover = clamp(nebCover, 0.0, 1.0);
	if (intensity <= 0.0 || cover <= 0.0) return float3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// DEEP-night gate: real Milky-Way nebulosity is only visible once the sun is well
	// below the horizon (astronomical twilight) — NOT at dusk, where the sky is still a
	// bright blue hour. Gating it deeper than the stars stops the "teal flood at sunset".
	float night = 1.0 - smoothstep(-0.22, -0.04, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return float3(0.0);

	float3  cN   = normalize(cdir);
	const float3 galN = normalize(float3(0.46, 0.52, -0.72));
	float bd   = dot(cN, galN);
	// COVERAGE widens the galactic lane: 0.5 = the classic tight band (exp 4.5),
	// 0 → a narrow sliver, 1 → nebulosity spreads across most of the sky.
	float band = exp(-bd * bd * mix(8.2, 0.8, cover));
	float3  P    = cN * 3.4;
	// SEED: shift the sample window into the noise field so the piece layout (and the
	// colour layout below) re-randomises. The band stays put — it comes from cN.
	P += float3(nebulaSeed * 13.1, nebulaSeed * 7.7, nebulaSeed * 19.3);
	float sd = nebulaSeed;

	// (1) FLOW WARP — advects every later field so chunks/wisps shear organically.
	float3 w1p = P * 0.55 + sd * 0.31;
	float3 Q1 = float3(starFbm3(w1p,                          2, noiseTex, noiseSamp),
	                   starFbm3(w1p + float3(19.3, 7.1, 3.7), 2, noiseTex, noiseSamp),
	                   starFbm3(w1p + float3(5.2, 1.9, 11.4), 2, noiseTex, noiseSamp)) - 0.5;
	float3 Q2 = float3(0.0);
	if (hifi)
	{
		float3 w2p = P * 1.10 + 3.1 * Q1 + sd * 1.7 + 41.0;
		Q2 = float3(starFbm3(w2p,                            2, noiseTex, noiseSamp),
		            starFbm3(w2p + float3(27.6, 13.2, 8.8),  2, noiseTex, noiseSamp),
		            starFbm3(w2p + float3(3.3, 21.7, 5.1),   2, noiseTex, noiseSamp)) - 0.5;
	}
	float3 Pw = P + 0.90 * Q1 + 0.42 * Q2;   // advected → flowing structure
	float3 Pc = P + 0.30 * Q1;               // steadier coord for the cluster gate
	// Anti-alias weight for the finest layers: fade toward 0 where their screen
	// footprint nears pixel-Nyquist so they never shimmer on camera rotation.
	float aaFine = 1.0 - smoothstep(0.30, 0.70, length(fwidth(Pw)) * 520.0);

	// (2) GAS PIECES — the nebula is a set of DISCRETE Worley-cell chunks, not a
	// diffuse fBm field. pd = signed "depth into the piece" (F1 blob, warped and
	// fractally eroded). COVERAGE drives BOTH the piece count (cluster-existence
	// gate) and the piece size (radius threshold). Because F1 rises toward the
	// saddle between two nearby features, the dilated SKIRTS of close pieces
	// meet there first → NECKS form and neighbouring pieces connect
	// automatically as coverage grows, before their cores ever merge.
	float3 Pp = P + 0.65 * Q1 + 0.30 * Q2;   // warped piece coords (organic silhouettes)
	float ero = starFbm3(Pw * 2.3 + 33.0, 2, noiseTex, noiseSamp) - 0.375;
	float thr   = mix(0.72, 0.40, cover);    // piece radius: cover 0 tiny .. 1 huge
	float exist = smoothstep(mix(0.44, 0.10, cover), mix(0.74, 0.40, cover),
	                         starFbm3(Pc * 0.60 + sd * 0.60 + 60.0, hifi ? 4 : 3, noiseTex, noiseSamp));
	float pd = worleyNoise3(Pp * 2.2 + sd * 17.0, noiseTex, noiseSamp) + ero * 0.45 - thr;
	// Half-size satellite pieces between the big ones — ALL tiers (they carry
	// most of the visible piece count at mid coverage; one extra tap).
	pd = max(pd, (worleyNoise3(Pp * 4.4 + 91.0, noiseTex, noiseSamp) + ero * 0.36 - thr - 0.05) * 0.92);
	pd -= (1.0 - exist) * 0.35;              // gated-out clusters never surface
	float core   = smoothstep(0.000, 0.030, pd);   // solid chunk body (crisp silhouette)
	float depth  = smoothstep(0.030, 0.240, pd);   // deep interior (brightens/whitens)
	float border = smoothstep(-0.018, 0.010, pd) * (1.0 - smoothstep(0.035, 0.095, pd)); // the piece's RIM band
	float skirt  = smoothstep(-0.110, -0.015, pd); // dilated halo — fuses into necks
	float neck   = skirt * (1.0 - core);           // connection zone between close pieces

	// (3) SILK — interior schlieren as soft level sets of the PIECE field itself,
	// so every streak layers along its chunk's own silhouette (fixed
	// constellations inside the piece, they live and re-randomise with it). The
	// ridged wisps only feather brightness ALONG each striation, and a hard gate
	// gives true zero-crossings → arcs, never closed onion rings.
	float stri = 1.0 - smoothstep(0.012, 0.050, abs(pd - 0.060));
	stri = max(stri, (1.0 - smoothstep(0.012, 0.050, abs(pd - 0.125))) * 0.85);
	stri = max(stri, (1.0 - smoothstep(0.012, 0.050, abs(pd - 0.190))) * 0.70);
	if (maxq)
		stri = max(stri, (1.0 - smoothstep(0.010, 0.042, abs(pd - 0.255))) * 0.55);
	float3 Ps = P + 0.55 * Q1;
	float r1 = 1.0 - abs(4.0 * starFbm3(float3(Ps.x, Ps.y * 0.25, Ps.z) * 3.2 + 210.0 + sd, 2, noiseTex, noiseSamp) - 1.0);
	float wisp = smoothstep(0.35, 0.95, r1);
	if (hifi)
	{
		float r2 = 1.0 - abs(4.0 * starFbm3(float3(Ps.x * 0.25, Ps.y, Ps.z) * 4.6 + 610.0 + sd, 2, noiseTex, noiseSamp) - 1.0);
		wisp = max(wisp, smoothstep(0.40, 0.95, r2) * 0.85);
	}
	if (maxq)
	{
		float r3 = 1.0 - abs(4.0 * starFbm3(float3(Pw.x, Pw.y * 0.30, Pw.z) * 7.3 + 950.0, 2, noiseTex, noiseSamp) - 1.0);
		wisp = max(wisp, smoothstep(0.45, 0.95, r3) * 0.70 * aaFine);
	}
	float silk = clamp(stri * smoothstep(0.10, 0.45, wisp) * (0.35 + 0.65 * wisp) + wisp * 0.28, 0.0, 1.2);

	// (4) CRACKLE CAGE — the filament web: the VEINS (Adern) threading each
	// piece's interior, densest along the border, and the strands that bridge
	// the necks. Each scale is TWO independent single-iso families (nebIso)
	// whose loops CROSS → cellular walls, no concentric nesting. A high-freq
	// CRINKLE warp (High/Max) wiggles the walls at small scale.
	float3 Pk = Pw;
	if (hifi)
		Pk += (float3(starFbm3(Pw * 7.5 + 331.0, 1, noiseTex, noiseSamp),
		              starFbm3(Pw * 7.5 + 337.7, 1, noiseTex, noiseSamp),
		              starFbm3(Pw * 7.5 + 343.3, 1, noiseTex, noiseSamp)) - 0.25) * 0.40;
	float nC    = starFbm3(Pk * 2.6 + 501.0 + sd * 0.5, 1, noiseTex, noiseSamp);
	float cage  = nebIso(nC, 0.260, 0.014);
	cage        = max(cage, nebIso(starFbm3(Pk * 3.1 + 517.7, 1, noiseTex, noiseSamp), 0.245, 0.012) * 0.90);
	cage       += nebIso(starFbm3(Pk * 5.3 + 622.0, 1, noiseTex, noiseSamp), 0.262, 0.010) * 0.65;
	if (hifi)
		cage   += nebIso(starFbm3(Pk * 6.4 + 651.3 + sd * 0.8, 1, noiseTex, noiseSamp), 0.248, 0.009) * 0.55;
	float cF = 0.0;
	if (hifi)
	{
		cF = nebIso(starFbm3(Pk * 10.7 + 743.0, 1, noiseTex, noiseSamp), 0.255, 0.008);
		cage += cF * 0.55;
	}
	if (maxq)
		cage += nebIso(starFbm3(Pk * 20.6 + 864.0, 1, noiseTex, noiseSamp), 0.258, 0.007) * 0.48 * aaFine;
	// TWO independent length-breakers fade walls in/out ALONG their run → broken
	// arcs and braids instead of closed onion rings around every noise extremum.
	float lenFade = smoothstep(0.30, 0.72, starFbm3(Pw * 2.6 + 777.0, hifi ? 2 : 1, noiseTex, noiseSamp) * (hifi ? 1.6 : 3.2));
	lenFade *= smoothstep(0.20, 0.62, starFbm3(Pw * 1.15 + 888.0 + sd * 0.3, hifi ? 2 : 1, noiseTex, noiseSamp) * (hifi ? 1.8 : 3.6));
	// Fine GRAIN → matte, fibrous texture on gas and filaments (anti-shiny).
	float grain = clamp(starFbm3(Pw * 6.5 + 400.0, hifi ? 2 : 1, noiseTex, noiseSamp) * (hifi ? 2.0 : 4.0), 0.0, 1.4);
	cage *= lenFade * (0.62 + 0.48 * grain);
	// STRIPES — a sky-wide random pattern of long vein strands: stretched ridged
	// layers at crossing orientations. Computed for the WHOLE sky so the pattern
	// runs seamlessly from piece to piece — the pieces merely act as its alpha
	// mask in the composite below.
	// Thin flank ISO lines (not ridge peaks — those are fat at the noise mode)
	// on axis-stretched fbm → long crisp strands.
	float sA = starFbm3(float3(Pw.x, Pw.y * 0.22, Pw.z) * 2.4 + 1300.0 + sd, 2, noiseTex, noiseSamp);
	float stripes = nebIso(sA, 0.55, 0.018);
	if (hifi)
	{
		float sB = starFbm3(float3(Pw.x * 0.22, Pw.y, Pw.z) * 2.9 + 1450.0, 2, noiseTex, noiseSamp);
		stripes = max(stripes, nebIso(sB, 0.55, 0.016) * 0.85);
	}
	stripes *= 0.55 + 0.40 * grain;
	cage = max(cage, stripes);
	// Interior MOTTLE — mid-frequency patchiness so the gas inside a piece has
	// visible texture instead of a flat airbrush fill.
	float mott = clamp(starFbm3(Pw * 3.8 + 271.0 + sd, hifi ? 3 : 2, noiseTex, noiseSamp) * 2.3, 0.0, 1.5);
	// ALPHA mask: the global vein/stripe pattern shows through the piece BODY
	// (uniformly, not just the rim) and through the necks; the border only adds
	// a mild extra so the rim web still reads a touch denser.
	float reach = core * 0.95 + neck * 0.80 + border * 0.30;
	// At high coverage the piece body approaches the whole sky — ease the vein
	// alpha back so a full nebula sky doesn't drown in web (0.5 unchanged).
	float covWeb = mix(1.0, 0.62, smoothstep(0.60, 1.00, cover));
	reach *= covWeb;
	float fil = cage * reach * (maxq ? 1.60 : (hifi ? 1.50 : 1.30));
	// Ionization: the strongest wall centres run cream-hot, mostly on the rim.
	float ion = (1.0 - smoothstep(0.003, 0.010, abs(nC - 0.260))) * smoothstep(0.50, 1.00, core * 0.90 + border * 0.40);

	// (5) BEADS — Worley corner pockets (far-from-all-features) land at cell
	// junctions; masked by the cage so they read as knots ON the filaments.
	float beads = 0.0;
	if (hifi)  beads  = pow(smoothstep(0.34, 0.16, worleyNoise3(Pk * 26.0 + sd * 31.0, noiseTex, noiseSamp)), 2.0) * 0.9;
	if (maxq)  beads += pow(smoothstep(0.32, 0.14, worleyNoise3(Pk * 46.0 + 77.0, noiseTex, noiseSamp)), 2.0) * 0.6;
	beads *= smoothstep(0.30, 0.90, cage) * (core * 0.8 + neck * 0.5 + border * 0.3) * aaFine;

	// (6) Max extras: dim BACK web across the halo zone (depth cue) + extra
	// fray strands riding the necks so the connections read fibrous.
	float back = 0.0, neckFray = 0.0;
	if (maxq)
	{
		back     = nebIso(starFbm3(Pw * 1.7 + 953.0 + sd * 0.7, 1, noiseTex, noiseSamp), 0.252, 0.020) * skirt * lenFade;
		neckFray = cF * neck * lenFade;
	}

	// (7) DUST — thin reddening lanes (one-sided backlit rim, see below) + rare
	// THICK emissive amber concentrations sitting ON the pieces (MIRI's warm dust).
	float dLn = starFbm3(Pw * 0.85 + 130.0 + sd * 0.9, 2, noiseTex, noiseSamp) - 0.55;
	float tau = (1.0 - smoothstep(0.015, 0.060, abs(dLn))) * (hifi ? 2.0 : 1.7);
	// ONE-SIDED edge band (n > iso only): a |n−iso| annulus would cross the noise
	// distribution's mode on the low side and flood the piece with warm rim.
	float rim = smoothstep(0.015, 0.045, dLn) * (1.0 - smoothstep(0.060, 0.100, dLn));
	float dustA = smoothstep(0.72, 0.94, starFbm3(Pc * 0.95 + 313.0 + sd * 0.4, hifi ? 3 : 2, noiseTex, noiseSamp) + border * 0.08);
	dustA *= (0.45 + 0.55 * smoothstep(0.20, 0.60, core + border))
	       * mix(1.0, 0.55, smoothstep(0.60, 1.00, cover)); // high cover: don't drown the sky in amber
	tau += dustA * 1.5;                       // the thick patches also redden the gas behind

	// ---- shared astro composition (all tiers) ----
	// Region colour field: which piece leans colour-2 vs colour-3 on its veins.
	float h = clamp(starFbm3(P * 0.5 + 71.0 + sd * 5.0, maxq ? 4 : (hifi ? 3 : 2), noiseTex, noiseSamp) * 1.7 - 0.35, 0.0, 1.0);
	float regionW = smoothstep(0.12, 0.88, h);
	float3 veilCol = nebColor;                                   // interior gas colour (colour 1)
	float3 filBase = mix(nebColor2, nebColor3, regionW);         // vein/rim colours (colour 2 ↔ 3)
	float3 hotCol  = float3(1.02, 0.96, 0.80) * 0.85 + filBase * 0.25; // ionized crests → cream
	float3 filCol  = mix(filBase, hotCol, clamp(ion, 0.0, 1.0));
	// Wavelength-dependent dust extinction: blue is extinguished first, so the lanes
	// silhouette the gas in brown/amber instead of flat grey (interstellar reddening).
	float3 Td = exp(-tau * float3(0.55, 1.05, 1.90));
	// High coverage floods the sky with gas — pull the interior gain down a little
	// so a full nebula sky keeps depth instead of washing to white (0.5 unchanged).
	float hiCov = 1.0 - 0.40 * smoothstep(0.60, 1.00, cover);
	// BACK→FRONT: faint halo fog + glowing neck haze (the connections), then the
	// solid piece — blue silk-layered interior that whitens toward the centre ...
	float3 C = veilCol * (skirt * (1.0 - core) * 0.06)
	         + mix(veilCol, filBase, 0.30) * (neck * 0.09);
	C += mix(veilCol, filBase, 0.55) * (back * 0.22);            // (Max) dim web behind the pieces
	// Dark contrast ring just OUTSIDE the border: pinches the halo fog at the
	// silhouette so every piece reads as a clearly defined, solid form.
	float ring = smoothstep(-0.055, -0.014, pd) * (1.0 - smoothstep(-0.014, 0.000, pd));
	C *= 1.0 - ring * 0.45;
	// FILLED interior: the shape reads solid (high base fill), and its colour
	// varies WITHIN the user colour — a deep shade blended toward a pale tint
	// of colour 1 by the mottle — while silk/veins pattern the fill on top.
	float3 innerDeep = veilCol * float3(0.60, 0.70, 1.02);
	float3 innerPale = veilCol * float3(1.28, 1.16, 0.96) + float3(0.05, 0.05, 0.06);
	float3 innerCol  = mix(innerDeep, innerPale, clamp(mott * 0.75, 0.0, 1.0));
	C += innerCol * hiCov * (core * (0.62 + 0.50 * depth) * (0.68 + 0.32 * silk))
	   + float3(0.88, 0.94, 1.06) * hiCov * (depth * depth * (0.12 + 0.30 * silk) * (0.60 + 0.40 * mott));
	C *= Td;                                                      // ... absorbed by the dust ...
	C += (nebColor2 * float3(1.10, 0.72, 0.45)) * (dustA * (0.45 + 0.40 * grain)); // thick amber glow
	// Backlit dust edges: warm translucent rim where a lane crosses the glow behind it.
	C += (filBase * 0.55 + float3(0.30, 0.16, 0.06)) * (rim * 0.30 * (core * 0.9 + depth * 0.5));
	// The RAND: a soft solid glow under the rim web so the border reads
	// continuous even where the web momentarily thins.
	C += filBase * (border * (0.22 + 0.30 * grain));
	// ... then the VEINS + rim web thread over everything. The filaments are
	// quasi-OPAQUE (they occlude the glow behind before adding their own
	// emission) so they stay saturated over the bright interior instead of
	// washing to pastel — in the JWST image the cage reads solid.
	C *= 1.0 - clamp(cage * reach, 0.0, 1.0) * 0.45;
	C += filCol * (fil * (0.80 + 0.20 * grain) * mix(1.0, (Td.x + Td.y + Td.z) * (1.0 / 3.0), 0.25));
	C += hotCol * (ion * fil * 0.25);                             // extra punch on the hot crests
	C += hotCol * (beads * 0.55);                                 // junction knots
	C += filBase * (neckFray * 0.12);                             // (Max) fibrous connection strands

	// Band/rift gating, intensity, then a LUMINANCE-preserving rolloff (a per-channel
	// rolloff would wash dense areas into pastel — this compresses brightness, keeps hue).
	C *= (band * 1.05 + 0.05) * (1.0 - 0.90 * mwRift(cN, noiseTex, noiseSamp));
	C *= 2.05 * intensity * smoothstep(0.0, 0.05, cover);   // smooth kill toward cover = 0
	float lum = dot(C, float3(0.30, 0.59, 0.11));
	if (lum > 1e-5) C *= (lum / (1.0 + lum * 0.22)) / lum;
	float horizon = smoothstep(0.0, 0.16, dir.y);
	return max(C, float3(0.0)) * (horizon * night);
}

// Aurora borealis — world-anchored volumetric curtains (night only). Each curtain is an
// independent finite ribbon with its own placement, heading, length, thickness, meander,
// ALTITUDE and motion, so they cross into a net instead of stacking as parallel bars;
// auroraWeb() braids each ribbon internally, the ray striation is sheared along the
// geomagnetic field line (corona convergence) and aerial perspective separates the depths.
// Mirrors the GL applyAurora3D() exactly — see the long comment there for the morphology
// research, the colour/altitude spectrum and the sampling scheme.
float auroraRnd(float k, float s)
{
	return starHash(float3(k * 0.7331 + 1.13, s * 1.9137 + 7.71, 19.37));
}
// Ridged "vein" field: the zero-set of the difference of two decorrelated noise fields is a
// network of thin branching filaments — the marble/vein pattern that gives the net filigree.
float auroraWeb(float2 p)
{
	float a = cloudNoise(p);
	float b = cloudNoise(p * 1.43 + float2(37.2, 11.9));
	return smoothstep(0.30, 0.0, abs(a - b));       // 1 on a vein, 0 in between
}
// Clip the ray interval [t0,t1] against the slab |x0 + dx*t| <= w; false when it misses.
bool auroraSlab(float x0, float dx, float w, thread float &t0, thread float &t1)
{
	if (abs(dx) < 1e-7) return abs(x0) <= w;
	float ta = (-w - x0) / dx, tb = (w - x0) / dx;
	t0 = max(t0, min(ta, tb));
	t1 = min(t1, max(ta, tb));
	return t1 > t0;
}
float3 applyAurora3D(float3 dir, float3 camPos, float time, float intensity,
                     float3 colBase, float3 colTop, float3 sunDir,
                     float auroraHeight, float auroraFragment, float2 fragCoord)
{
	if (intensity <= 0.0) return float3(0.0);
	dir = normalize(dir);
	if (dir.y < 0.02) return float3(0.0);                       // horizon → ray never reaches the curtains
	float night = 1.0 - smoothstep(-0.20, -0.02, clamp(normalize(sunDir).y, -0.3, 1.0));
	if (night <= 0.0) return float3(0.0);

	// World-space altitude envelope. Height control drives the curtain ELEVATION; each curtain
	// picks its OWN altitude and extent inside this bracket (they are not all on one shelf).
	float altitude = mix(1500.0, 7000.0, clamp(auroraHeight, 0.0, 1.0));
	float invY  = 1.0 / dir.y;
	float tNear = max(altitude * 0.72 * invY, 0.0);
	float tFar  = altitude * 3.75 * invY;
	float layoutR = altitude * 13.5;                            // the layout reaches this far out
	tFar = min(tFar, layoutR * 2.2);
	if (tFar <= tNear) return float3(0.0);

	float frag = clamp(auroraFragment, 0.0, 1.0);
	float jit  = skyIgn(fragCoord);
	float2 o   = camPos.xz;                                     // ray origin in world XZ
	float2 dxz = dir.xz;                                        // ray XZ velocity (|dxz| = cos(elev))

	float3 acc = float3(0.0);
	for (int k = 0; k < 22; ++k)
	{
		float fk = float(k);
		// Layout: stratified ring radius (near-zenith … horizon) + golden-angle azimuth.
		float rr     = (fk + auroraRnd(fk, 0.0)) / 22.0;
		float radius = altitude * (0.18 + 13.3 * rr * rr);
		float phi    = fk * 2.39996323 + auroraRnd(fk, 1.0) * 1.9;
		float2 cen   = float2(cos(phi), sin(phi)) * radius;
		// Heading: tangential to the ring (the oval runs E-W) ± 74° → curtains cross.
		float head = phi + 1.5707963 + (auroraRnd(fk, 2.0) - 0.5) * 2.6;
		float2 tang = float2(cos(head), sin(head));
		float2 nrm  = float2(-tang.y, tang.x);
		// Everything scales with the ring radius → constant apparent size on the dome.
		float scl     = 0.16 * radius + 420.0;
		float halfLen = scl * (2.6 + 3.8 * auroraRnd(fk, 3.0));
		float sigma0  = scl * (0.05 + 0.08 * auroraRnd(fk, 4.0)); // curtains are THIN ribbons
		float a1 = scl * (0.55 + 0.85 * auroraRnd(fk,  5.0));   // broad arc sweep
		float a2 = scl * (0.18 + 0.34 * auroraRnd(fk,  6.0));   // folds
		float a3 = scl * (0.05 + 0.11 * auroraRnd(fk,  7.0));   // curls
		float f1 = (0.55 / scl) * (0.6 + 0.8 * auroraRnd(fk,  8.0));
		float f2 = (1.70 / scl) * (0.6 + 0.9 * auroraRnd(fk,  9.0));
		float f3 = (5.20 / scl) * (0.7 + 0.9 * auroraRnd(fk, 10.0));
		float p1 = auroraRnd(fk, 11.0) * 6.2831853;
		float p2 = auroraRnd(fk, 12.0) * 6.2831853;
		float p3 = auroraRnd(fk, 13.0) * 6.2831853;
		// This curtain's own altitude and vertical extent.
		float altK   = altitude * (0.72 + 0.62 * auroraRnd(fk, 14.0));
		float thickK = altK * (1.00 + 0.80 * auroraRnd(fk, 18.0));
		float hDec   = 4.6 - 2.4 * auroraRnd(fk, 15.0);         // how fast it fades upward
		float bright = 0.55 + 0.70 * auroraRnd(fk, 16.0);
		// Motion: the arc drifts (meander time terms), surges race ALONG it, the fine rays
		// stream sideways, and each curtain pulses on its own clock — never in unison.
		float tph    = auroraRnd(fk, 19.0) * 6.2831853;
		float puls   = 0.80 + 0.20 * sin(time * (0.30 + 0.55 * auroraRnd(fk, 20.0)) + tph);
		float surgeF = (1.5 + 1.1 * auroraRnd(fk, 21.0)) / scl;
		float surgeV = (0.7 + 1.3 * auroraRnd(fk, 22.0)) * (auroraRnd(fk, 23.0) < 0.5 ? -1.0 : 1.0);
		float rayDrift = (0.5 + 1.1 * auroraRnd(fk, 24.0)) * (auroraRnd(fk, 25.0) < 0.5 ? -1.0 : 1.0);
		float fringeAmt = (0.35 + 0.55 * frag) * (0.45 + 0.55 * auroraRnd(fk, 26.0));
		float3 hueK = mix(float3(0.92, 1.02, 0.94), float3(1.07, 0.97, 1.06), auroraRnd(fk, 17.0));
		// Rays follow the geomagnetic FIELD LINE, not local vertical → all parallel in 3D, so
		// perspective converges them on the magnetic zenith (the corona).
		float shearU = dot(float2(0.22, 0.13), tang);

		// Clip the ray to this ribbon's box: own altitude band, |across| <= meander + 3σ,
		// |along| <= len.
		float u0 = dot(o - cen, tang), du = dot(dxz, tang);
		float v0 = dot(o - cen, nrm ), dv = dot(dxz, nrm );
		float halfW = a1 + a2 + a3 + sigma0 * 3.0;
		float t0 = tNear, t1 = tFar;
		if (!auroraSlab(-(altK + thickK * 0.5), dir.y, thickK * 0.5, t0, t1)) continue;
		if (!auroraSlab(v0, dv, halfW,                                t0, t1)) continue;
		if (!auroraSlab(u0, du, halfLen * 1.4,                        t0, t1)) continue;

		// What matters is how fast the distance to the centreline changes, and that has TWO
		// parts: the ray crossing the curtain (dv) AND the centreline sliding away under it as
		// the ray travels along the curtain (meander slope × du). Ignoring the second term
		// leaves near-edge-on rays undersampled — that is what rings distant curtains with
		// moiré. The slope is the RMS of the three meander derivatives; scl cancels out.
		float mndSlope = sqrt(0.5 * (a1 * f1 * a1 * f1 + a2 * f2 * a2 * f2 + a3 * f3 * a3 * f3));
		float dRate = max(abs(dv) + mndSlope * abs(du), 0.02);  // |d| change per unit t
		int   Nk = int(clamp((t1 - t0) / (sigma0 * 1.1 / dRate), 6.0, 64.0));
		float ds = (t1 - t0) / float(Nk);
		float dAcross = ds * dRate;                             // how far d moves per step

		for (int i = 0; i < Nk; ++i)
		{
			float t = t0 + (float(i) + jit) * ds;
			float3 pos = camPos + dir * t;                      // WORLD position → real parallax
			float u = dot(pos.xz - cen, tang);
			float e = abs(u) / halfLen;
			if (e > 1.4) continue;
			// Density envelope toward the tips — MUST decay smoothly to zero, not stop at a
			// boundary: viewed near edge-on the ray's chord jumps from nothing to a long bright
			// path across a hard |u| = halfLen plane, and that plane projects to a straight line
			// (the ribbon tears off mid-sky). Flat middle, steep shoulder, ~0 by e = 1.2.
			float e2 = e * e;
			float ends = exp(-3.0 * e2 * e2 * e2);
			float hf = clamp((pos.y - camPos.y - altK) / thickK, 0.0, 1.0); // 0 = this curtain's foot
			// Sharp bright lower edge at this curtain's own foot, exponential fade upward, and a
			// soft top so the ribbon dissolves instead of ending on a flat lid. The fade steepens
			// toward the tips, so an arc thins to a point rather than vanishing.
			float hDecE = hDec * (1.0 + 2.2 * (1.0 - ends));
			float Ev = smoothstep(0.0, 0.05, hf)
			         * exp(-hf * hDecE)
			         * (1.0 - smoothstep(0.62, 1.00, hf));
			if (Ev <= 0.002) continue;
			float v = dot(pos.xz - cen, nrm);
			float mnd = a1 * sin(u * f1 + p1 + time * 0.21)     // arc sweep + folds + curls
			          + a2 * sin(u * f2 + p2 - time * 0.33)
			          + a3 * sin(u * f3 + p3 + time * 0.52);
			float d = v - mnd;
			float distLOD = smoothstep(altitude * 1.5, layoutR, t);
			float sigmaG  = sigma0 * (1.0 + 0.8 * distLOD);
			// Pre-filter against the step size: widen σ, renormalise → blur, not moiré (and no
			// IGN dither from the per-pixel jitter).
			float sigmaE = max(sigmaG, dAcross * 1.55);
			float sheet  = exp(-(d * d) / (2.0 * sigmaE * sigmaE)) * (sigmaG / sigmaE);
			if (sheet < 0.004) continue;
			// Structures are indexed by the FOOT of the field line through this sample, which
			// leans the whole striation along the field instead of standing dead vertical.
			float uRay = u - hf * thickK * shearU;
			// Detail frequencies are in units of scl so near and far curtains carry the same
			// amount of structure (absolute world frequencies alias the far ones). The striation
			// also smooths out toward the top: 630 nm oxygen has a ~110 s lifetime, so the high
			// part of a curtain is genuinely diffuse and never rayed.
			float rays = 0.62 + 0.38 * cloudNoise(float2(uRay / scl * 30.0 + fk * 17.0 - time * rayDrift,
			                                             hf * 2.2 + fk));
			rays = mix(rays, 1.0, max(distLOD, smoothstep(0.30, 0.80, hf)));
			float web = auroraWeb(float2(uRay / scl * 6.0 + fk * 9.0 - time * 0.06 * rayDrift,
			                             hf * 1.3 + fk * 3.0 + time * 0.04));
			float weave = mix(1.0, 0.10 + 1.45 * web, 0.35 + 0.65 * frag);
			// Surges of brightness racing along the arc — the part that reads as "dancing".
			float surge = 0.70 + 0.55 * sin(u * surgeF - time * surgeV + tph);
			// Colour by altitude, following the real auroral spectrum: magenta N2+ fringe on the
			// bottom edge (<100 km), green 557.7 nm oxygen body, diffuse red/violet 630 nm top
			// (>200 km). The fringe is derived from the user's top colour, not hard-coded.
			float3 cCol = mix(colBase, colTop, smoothstep(0.22, 0.70, hf));
			float3 fringeCol = colTop * float3(1.55, 0.62, 1.05) + float3(0.10, 0.0, 0.06);
			cCol = mix(cCol, fringeCol, (1.0 - smoothstep(0.02, 0.19, hf)) * fringeAmt);
			cCol *= hueK;
			// Aerial perspective — dims and cools with distance, so the curtains separate in
			// depth instead of all reading as if they sat at the same range.
			float aer = exp(-t / (layoutR * 0.95));
			cCol = mix(cCol * float3(0.58, 0.72, 1.02), cCol, aer);
			float fade = mix(0.35, 1.0, aer) * (1.0 - smoothstep(layoutR * 1.5, layoutR * 2.1, t));
			// Normalise by the curtain's OWN thickness: the sheet's line integral is ∝ σ, so a
			// shared divisor would make a fat distant curtain far brighter, not merely wider.
			acc += cCol * (sheet * Ev * rays * weave * ends * bright * puls * surge
			               * (ds / (sigma0 * 6.0)) * fade); // pure ADD (emissive, no extinction)
		}
	}
	// LUMINANCE-preserving rolloff (FragColor is LDR). A per-channel rolloff clips the strongest
	// channel first, washing a saturated green curtain out to pale mint; compressing on
	// luminance holds the hue and only the hottest cores desaturate. Same trick as the nebula.
	// Each ray integrates through the WHOLE vertical colour ramp, so green and violet average
	// toward grey along it and the display reads as pale mint rather than the deep green/violet
	// the palette actually asks for. Push saturation back up about luminance (which the step
	// below then preserves) before compressing.
	float lum = dot(acc, float3(0.30, 0.59, 0.11));
	acc = max(mix(float3(lum), acc, 1.35), float3(0.0));
	if (lum > 1e-5) acc *= (lum / (1.0 + lum * 0.80)) / lum;
	float horizonFade = clamp(dir.y * 8.0, 0.0, 1.0);
	return acc * (intensity * night * horizonFade * 1.50);
}

// Contrails (Kondensstreifen) — scattered finite vapour-trail segments at hashed
// positions/headings on the sky-plane projection; day-gated, faded as cloud cover rises.
// Mirrors the GL contrails().
float3 contrails(float3 baseSky, float3 dir, float3 sunDir, float amount, float coverage)
{
	if (amount <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.05) return baseSky;
	float day = smoothstep(-0.04, 0.16, clamp(sunDir.y, -0.2, 1.0));
	if (day <= 0.0) return baseSky;
	float2 P = dir.xz / (dir.y + 0.22);                   // sky-plane projection (like the aurora)
	float aAcc = 0.0;
	for (int i = 0; i < 9; ++i)
	{
		float fi = float(i);
		float a0 = starHash(float3(fi, 11.0,  3.0));      // heading
		float a1 = starHash(float3(fi,  5.0, 19.0));      // centre x
		float a2 = starHash(float3(fi, 23.0,  7.0));      // centre y
		float a3 = starHash(float3(fi,  2.0, 31.0));      // length
		float ang = a0 * 6.2831853;
		float2 d2 = float2(cos(ang), sin(ang));
		float2 c  = (float2(a1, a2) - 0.5) * 7.5;
		float L   = 1.0 + 2.4 * a3;
		float2 rel = P - c;
		float t    = clamp(dot(rel, d2), -L, L);
		float perp = length(rel - d2 * t);
		float u    = smoothstep(-L, L, t);                // 1 fresh tip … 0 old tip
		float width = mix(0.075, 0.013, u);
		float x     = perp / width;
		float prof  = exp(-x * x * 1.6);
		float fuzz  = 0.5 + 0.5 * cloudFbm(float2(t * 3.5 + fi * 9.0, u * 6.0 + perp * 4.0));
		float along = mix(0.14, 0.95, u);
		float tip   = 1.0 - smoothstep(L * 0.6, L, abs(t));
		float seg   = clamp(prof * fuzz * along * tip, 0.0, 1.0);
		aAcc += seg * (1.0 - aAcc);                       // over-composite overlapping trails
	}
	float fade  = smoothstep(0.05, 0.30, dir.y) * (1.0 - smoothstep(0.85, 1.0, dir.y));
	float clear = 1.0 - smoothstep(0.25, 0.65, coverage);
	float alpha = clamp(aAcc * amount * day * fade * clear, 0.0, 0.72);
	float toSun = max(dot(dir, sunDir), 0.0);
	float3 white = mix(float3(0.86, 0.89, 0.94), float3(1.0, 0.99, 0.96), toSun * toSun);
	return mix(baseSky, white, alpha);
}

// Thin high cirrus — fibrous mare's-tail streaks on a high horizontal sheet, drifted by
// wind, day-gated, gold/pink at low sun. Mirrors the GL cirrus().
float3 cirrus(float3 baseSky, float3 dir, float3 sunDir, float3 sunColor, float amount, float seed, float time, float2 windXZ)
{
	if (amount <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.04) return baseSky;
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.06, 0.14, sunY);
	if (day <= 0.0) return baseSky;
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));
	float2 so = float2(seed * 13.1, seed * 7.3);
	float2 P  = dir.xz / (dir.y + 0.12) + windXZ * time * 0.5 + so;
	float2 q  = P * float2(0.30, 3.0);                    // strong anisotropy → long fibres
	q += float2(0.12, 0.95) * (cirrusFbm(q * 0.5 + so) - 0.5); // bend straight strands
	float baseN = cirrusFbm(q * 1.15);
	float ridge = 1.0 - abs(2.0 * baseN - 1.0);
	ridge = pow(clamp(ridge, 0.0, 1.0), 1.6);
	float fineW  = smoothstep(0.06, 0.26, dir.y);
	float fibers = cirrusFbm(P * float2(0.8, 7.0) + so);
	fibers = mix(0.5, smoothstep(0.32, 0.82, fibers), fineW);
	float thr    = mix(0.60, 0.40, clamp(amount, 0.0, 1.0));
	float mask   = smoothstep(thr, thr + 0.22, ridge);
	float streak = mask * (0.28 + 0.72 * fibers);
	streak *= 0.6 + 0.4 * cirrusFbm(q * 0.7 + so + 11.0);
	float fade  = smoothstep(0.04, 0.20, dir.y) * (1.0 - smoothstep(0.92, 1.0, dir.y));
	float alpha = clamp(streak * day * fade * (0.40 + 0.65 * clamp(amount, 0.0, 1.0)), 0.0, 0.66);
	float3 white = mix(float3(0.92, 0.95, 1.0), sunColor * float3(1.35, 1.0, 0.78), dusk * 0.7);
	float fwd   = pow(max(dot(dir, sunDir), 0.0), 12.0);
	white += sunColor * (fwd * 0.45 * max(day, dusk));
	white = mix(white, baseSky * 1.2 + 0.08, 0.15);
	return mix(baseSky, white, alpha);
}

// The bright sun BODY (crisp disk + tight bloom) factored out of skyColor() so the
// cloud pass can occlude it. skyFragment subtracts this, runs the clouds, then re-adds
// it weighted by pow(cloudTransmittance, k): an opaque cloud (T≈0.1) then fully hides
// the sun instead of leaking a ~14× ghost through a plain *T. The expressions below
// MUST stay byte-identical to the matching disk+bloom lines in kSkyFuncMSL skyColor()
// so that (col -= sunGlare) cancels exactly and a clear sky is unchanged.
// Spectral helper (hue 0 = red … 0.78 ≈ violet) for the rainbow arc.
float3 skyHsv(float h, float s, float v)
{
	float3 p = abs(fract(h + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
	return v * mix(float3(1.0), clamp(p - 1.0, 0.0, 1.0), s);
}

// Primary + secondary rainbow centred on the anti-solar point (−sunDir). Only while it
// is raining (rainAmt) with the sun up but not too high (above ~46° the arc sinks below
// the horizon). Subtle + additive; added before the cloud composite so clouds occlude it.
float3 rainbow(float3 dir, float3 sunDir, float rainAmt)
{
	if (rainAmt <= 0.0) return float3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day    = smoothstep(-0.02, 0.12, sunDir.y);           // daytime only
	float lowSun = 1.0 - smoothstep(0.45, 0.72, sunDir.y);      // sun low enough to throw an arc
	float vis    = day * lowSun;
	if (vis <= 0.0 || dir.y < 0.0) return float3(0.0);
	float ang = acos(clamp(dot(dir, -sunDir), -1.0, 1.0)) * 57.29578; // degrees from anti-solar point
	// Primary bow: violet (inner, ~40.5°) → red (outer, ~42.4°).
	float pBand = smoothstep(39.6, 40.6, ang) * (1.0 - smoothstep(42.2, 43.2, ang));
	float tp    = clamp((ang - 40.5) / 1.9, 0.0, 1.0);
	float3 cP   = skyHsv(0.78 * (1.0 - tp), 1.0, 1.0);
	// Secondary bow: reversed order, fainter, ~50.5..53.5°.
	float sBand = smoothstep(50.2, 51.0, ang) * (1.0 - smoothstep(53.3, 54.3, ang));
	float ts    = clamp((ang - 51.0) / 2.5, 0.0, 1.0);
	float3 cS   = skyHsv(0.78 * ts, 1.0, 1.0) * 0.5;
	float horizon = smoothstep(0.0, 0.12, dir.y);
	return (cP * pBand + cS * sBand) * (vis * clamp(rainAmt, 0.0, 1.0) * horizon * 0.45);
}

float3 sunGlare(float3 dir, float3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.14, 0.04, sunY) * (1.0 - smoothstep(0.04, 0.26, sunY));
	float3 sunTint = mix(float3(1.0, 0.58, 0.24), float3(1.0, 0.96, 0.88), smoothstep(0.0, 0.28, sunY));
	float  s         = max(dot(dir, sunDir), 0.0);
	float  sunVis    = max(day, dusk);
	float  bloomDamp = mix(1.0, 0.28, dusk);
	// The crisp daytime disk is now a geometric body (sunDisk, below) — only the
	// cloud-occludable tight bloom remains here, so the col -= sunGlare / re-add dance
	// still cancels byte-for-byte against skyColor()'s matching bloom line.
	float3 g  = sunTint * (pow(s, 220.0)  * 1.1 * bloomDamp) * sunVis; // tight bloom
	return g;
}

// Geometric sun disk — a real limb-darkened body (like moonDisk) replacing the old
// pow(dot(dir,sunDir)) glare lobe. Eddington limb darkening dims the edge; atmospheric
// refraction flattens it into a wider-than-tall, reddened ellipse near the horizon (a
// proper setting sun). Emissive; sky-pass only (kept out of skyColor/IBL, like the
// moon) and composited after the clouds in skyFragment, weighted by cloud transmittance.
float3 sunDisk(float3 dir, float3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	// Visible from noon down to just below the horizon (the setting sun), then gone.
	float vis = smoothstep(-0.06, 0.02, sunY);
	if (vis <= 0.0 || dot(dir, sunDir) <= 0.0) return float3(0.0);
	// Tangent frame: right = horizontal, upv = vertical, so the disk can squash vertically.
	float3 right = normalize(cross(float3(0.0, 1.0, 0.0), sunDir));
	float3 upv   = cross(sunDir, right);
	const float kRadius = 0.027;                                   // angular radius (~ the moon)
	// Refraction flattening: near the horizon the lower limb lifts → wider-than-tall.
	float squash = mix(0.62, 1.0, smoothstep(0.0, 0.14, sunY));    // <1 ⇒ vertically compressed
	float qx = dot(dir, right) / kRadius;
	float qy = dot(dir, upv)   / (kRadius * squash);
	float r  = length(float2(qx, qy));
	if (r > 1.0) return float3(0.0);
	// Eddington limb darkening: I(mu) = 1 - u(1 - mu), mu = cos(angle) = sqrt(1 - r^2), u = 0.6.
	float mu   = sqrt(max(1.0 - r * r, 0.0));
	float limb = 1.0 - 0.6 * (1.0 - mu);                           // centre 1.0 → limb 0.4
	float edge = smoothstep(1.0, 0.96, r);                         // soft anti-aliased rim
	// Reddens toward the horizon (more atmosphere), warm-white when high. Kept DIM at low
	// sun so ACES doesn't desaturate the core to flat white (it must read as a red-orange
	// ellipse — same lesson as the moon's ×3.0→×1.3 fix).
	float3 tint  = mix(float3(1.0, 0.38, 0.14), float3(1.0, 0.95, 0.88), smoothstep(0.0, 0.22, sunY));
	float bright = mix(2.8, 11.0, smoothstep(0.0, 0.22, sunY));
	return tint * (limb * edge * bright * vis);
}

// ── Low-res cloud pass support ───────────────────────────────────────────────
// The cloud raymarch as a standalone (L, T): scattered light L (rgb) + view-ray
// transmittance T (a), WITHOUT the baseSky composite (the caller does baseSky*T+L).
// Thin wrappers over the unchanged applyClouds()/applyClouds3D() so the inline path
// stays byte-identical: dome passes baseSky=0 (→ returns L directly); 3D needs a
// baseSky for its aerial-perspective haze, so it uses skyColor(dir) and recovers
// L = composited − baseSky*T (exact, since applyClouds3D returns baseSky*T + L).
float4 cloudsDomeLT(float3 dir, float3 sunDir, float time, float coverage, float3 sunColor,
                    float3 wind, float3 cloudTint, float densityMul, float quality,
                    texture3d<float> noiseTex, sampler noiseSamp)
{
	float T = 1.0;
	float3 L = applyClouds(float3(0.0), dir, sunDir, time, coverage, sunColor, wind,
	                       cloudTint, densityMul, quality, noiseTex, noiseSamp, T);
	return float4(L, T);
}
float4 clouds3DLT(float3 dir, float3 camPos, float3 sunDir, float time, float coverage,
                  float3 sunColor, float3 wind, float cloudH, float cloudFluffiness,
                  float cloudDensity, float quality, float3 cloudTint, float2 fragCoord,
                  float style, float interSh, float evo,
                  texture3d<float> noiseTex, sampler noiseSamp)
{
	float3 hazeSky = skyColor(dir, sunDir);   // aerial-perspective reference (quarter-res ok)
	float  T = 1.0;
	float3 comp = (style > 0.5)
		? applyClouds3DReal(hazeSky, dir, camPos, sunDir, time, coverage, sunColor, wind,
		                    cloudH, cloudFluffiness, cloudDensity, quality, cloudTint,
		                    interSh, evo, fragCoord, noiseTex, noiseSamp, T)
		: applyClouds3D(hazeSky, dir, camPos, sunDir, time, coverage, sunColor, wind,
		                cloudH, cloudFluffiness, cloudDensity, quality, cloudTint,
		                fragCoord, noiseTex, noiseSamp, T);
	return float4(comp - hazeSky * T, T);     // recover L
}

// Standalone cloud pass (drawn at quarter resolution → upsampled + composited in
// skyFragment when low-res clouds are enabled). Output: rgb = L, a = T.
fragment float4 cloudFragment(SkyOut in [[stage_in]],
                              constant SkyParams& p [[buffer(0)]],
                              texture3d<float> noiseTex [[texture(1)]],
                              sampler noiseSamp [[sampler(1)]])
{
	float4 wp1 = p.invViewProj * float4(in.ndc,  1.0, 1.0);
	float4 wp0 = p.invViewProj * float4(in.ndc, -1.0, 1.0);
	float3 dir = normalize(wp1.xyz / wp1.w - wp0.xyz / wp0.w);
	if (p.cameraPos.w > 0.5)
		return clouds3DLT(dir, p.cameraPos.xyz, p.sunDir.xyz, p.params.z, p.params.y,
		                  p.sunColor.xyz, p.wind.xyz, p.cloud.x, p.cloud.z, p.cloud.y,
		                  p.star2.y, p.cloudTint.xyz, in.position.xy,
		                  p.neb2.y, p.neb2.z, p.neb2.w, noiseTex, noiseSamp);
	return cloudsDomeLT(dir, p.sunDir.xyz, p.params.z, p.params.y, p.sunColor.xyz, p.wind.xyz,
	                    p.cloudTint.xyz, p.cloud.y, p.star2.y, noiseTex, noiseSamp);
}

// Crepuscular rays (god-rays). Cheap dome-cloud occlusion proxy: how much sunlight
// passes along a single direction d (1 = clear sky, 0 = blocked by cloud). Mirrors the
// dome cloud's perlin coverage gate (applyClouds) with the Worley billow approximated by
// its mean, so shafts line up with the visible dome clouds for a fraction of the cost.
float godrayClear(float3 d, float time, float coverage, float3 wind,
                  texture3d<float> noiseTex, sampler noiseSamp)
{
	d = normalize(d);
	if (d.y < 0.05) return 1.0;                       // toward the horizon → no cloud slab hit
	float  s   = kCloudBase / max(d.y, 1e-3);
	float3 pos = d * s;
	float3 pp  = pos * kCloudScale + wind * time;
	float  perlin = starFbm3(pp + float3(0.0, time * 0.030, 0.0), 4, noiseTex, noiseSamp);
	float  lo   = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0)); // same threshold as applyClouds
	float  dens = smoothstep(lo, lo + 0.10, perlin * 0.5 + 0.275); // billow≈0.5 mean (sharper gap/cloud edge)
	return 1.0 - clamp(dens, 0.0, 1.0);
}

// Sun shafts: march from the view direction toward the sun, accumulating the clear-sky
// fraction so light streaks through the gaps between clouds. Gated to a cone around the
// sun by day, and only when there is partial cloud cover (overcast or clear = no shafts).
// Additive, sun-coloured. Mirrors the GL crepuscular().
float3 crepuscular(float3 dir, float3 sunDir, float3 sunColor, float time,
                   float coverage, float3 wind, float strength,
                   texture3d<float> noiseTex, sampler noiseSamp)
{
	if (strength <= 0.0) return float3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float day = smoothstep(-0.02, 0.12, sunDir.y);
	if (day <= 0.0) return float3(0.0);
	float ct = dot(dir, sunDir);
	if (ct < 0.15) return float3(0.0);                           // near-sun cone (past ~80° contributes ~0)
	float coverGate = smoothstep(0.05, 0.35, coverage) * (1.0 - smoothstep(0.85, 1.0, coverage));
	if (coverGate <= 0.0) return float3(0.0);                    // need broken cloud for gaps
	const int GN = 8;
	float light = 0.0;
	float3 d = dir;
	for (int i = 0; i < GN; ++i)
	{
		d = normalize(mix(d, sunDir, 0.12));                     // step toward the sun
		light += godrayClear(d, time, coverage, wind, noiseTex, noiseSamp);
	}
	light /= float(GN);
	float cone  = pow(clamp(ct, 0.0, 1.0), 2.0);                 // falloff away from the sun (extends shaft reach)
	float shaft = light * cone * day * coverGate * clamp(strength, 0.0, 1.0);
	return sunColor * shaft * 0.55;
}

// Subtle moon glow: one soft luminous ring hugging the moon's disk — a gentle aureole that
// makes the moon read as glowing rather than a flat cut-out. Deliberately understated
// (dezent), always present at night, cool white, and SHAPED BY THE PHASE: it glows on the lit
// limb and fades across the terminator (a crescent glows only on its bright side, a full moon
// all around). NOT the wide 22° halo. Mirrors GL moonCorona().
float3 moonCorona(float3 dir, float3 sunDir, bool hasMoon, float moonPhase)
{
	if (!hasMoon) return float3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y < 0.0) return float3(0.0);
	float3 moonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return float3(0.0);
	float vis = night * smoothstep(0.0, 0.04, dir.y) * smoothstep(0.0, 0.10, moonDir.y);
	if (vis <= 0.0) return float3(0.0);
	const float kMoonR = 0.030;                                   // moon angular radius (matches moonDisk)
	float ang  = acos(clamp(dot(dir, moonDir), -1.0, 1.0));       // radians from moon centre
	float ring = exp(-((ang - kMoonR * 1.15) * (ang - kMoonR * 1.15)) / (0.016 * 0.016)); // soft ring at the limb
	// Phase shaping: build the moon-view frame (as moonDisk), take the outward direction of
	// this ring point, and light a just-inside-the-limb normal by the same sun direction L.
	float3 right = normalize(cross(float3(0.0, 1.0, 0.0), moonDir));
	float3 up    = cross(moonDir, right);
	float2 rad   = normalize(float2(dot(dir, right), dot(dir, up)) + float2(1e-6));
	float  ph    = moonPhase * 6.2831853;
	float3 L     = float3(sin(ph), 0.0, -cos(ph));               // sun direction across the disk (== moonDisk)
	float3 Nlimb = normalize(float3(rad * 0.85, 0.53));          // normal just inside the lit limb
	float  lit   = smoothstep(0.0, 0.55, dot(Nlimb, L));         // 0 dark limb .. 1 lit limb
	return float3(0.85, 0.90, 1.0) * (ring * lit * 0.17 * vis);  // dezent, phase-shaped
}

fragment float4 skyFragment(SkyOut in [[stage_in]],
                            constant SkyParams& p [[buffer(0)]],
                            texture2d<float> moonTex [[texture(0)]],
                            sampler moonSamp [[sampler(0)]],
                            texture3d<float> noiseTex [[texture(1)]],
                            sampler noiseSamp [[sampler(1)]],
                            texture2d<float> cloudTex [[texture(2)]],
                            sampler cloudSamp [[sampler(2)]],
                            constant float4x4& prevVP [[buffer(1)]]) // pre-pass camera (low-res cloud reprojection)
{
	float4 wp1 = p.invViewProj * float4(in.ndc,  1.0, 1.0);
	float4 wp0 = p.invViewProj * float4(in.ndc, -1.0, 1.0);
	// normalize → stars/nebula/celestial frame don't jitter as the camera turns (GL parity).
	float3 dir = normalize(wp1.xyz / wp1.w - wp0.xyz / wp0.w);
	float3 col  = skyColor(dir, p.sunDir.xyz);
	// Star-free atmosphere base for the REALISTIC cloud path's ambient/twilight
	// fill: feeding the full `col` (stars/nebula/moon already added) into the
	// cloud march paints the star field ONTO the cloud bodies via the twilight
	// term. The clouds are lit by THIS instead; the celestial layer is then
	// occluded by the cloud transmittance at the composite (same recipe the
	// low-res pre-pass always used).
	float3 atmoBase = col;
	// Lift the sun's cloud-occludable bloom out (re-added below) and compute the
	// geometric sun disk (a sky-only body, like the moon) to add on top of it.
	float3 sunGlareCol = sunGlare(dir, p.sunDir.xyz);
	float3 sunBodyCol  = sunDisk(dir, p.sunDir.xyz);
	col -= sunGlareCol;
	// Night-sky elements + the celestial rotation are skipped entirely by day. The
	// branch is coherent (sunDir is uniform → every pixel takes the same path).
	float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(p.sunDir.xyz).y, -0.2, 1.0));
	if (nightF > 0.0)
	{
		float3 cdir = celestialDir(dir, p.params.x); // turns with the day-night cycle
		// Star brightness + colour tint applied at the call site (GL parity).
		col += starField(dir, cdir, p.sunDir.xyz, p.params.z, p.auroraColor.w,
		                 p.star.x, p.star.y, p.star.z, p.star.w, p.star2.x,
		                 noiseTex, noiseSamp) * p.starColor.xyz * p.starColor.w;
		col += nebula(dir, cdir, p.sunDir.xyz, p.nebulaColor.w, p.nebulaColor.xyz,
		              p.nebulaColor2.xyz, p.nebulaColor3.xyz, p.cirrus.w, p.nebulaColor2.w,
		              p.neb2.x, noiseTex, noiseSamp);
		col += applyAurora3D(dir, p.cameraPos.xyz, p.params.z, p.params.w,
		                     p.auroraColor.xyz, p.auroraColorTop.xyz, p.sunDir.xyz,
		                     p.cirrus.y, p.cirrus.z, in.position.xy);
		col += moonDisk(dir, p.sunDir.xyz, p.sunDir.w > 0.5, p.sunColor.w, moonTex, moonSamp);
		col += shootingStars(dir, p.sunDir.xyz, p.params.z, p.auroraColorTop.w,
		                     p.starColor.xyz, p.starColor.w, p.star.x, p.star.y); // meteors (clouds occlude below)
	}
	// High thin layers first, then the cumulus clouds in front so lower clouds occlude them.
	col = cirrus(col, dir, p.sunDir.xyz, p.sunColor.xyz, p.cloudTint.w, p.cirrus.x, p.params.z, p.wind.xz);
	col = contrails(col, dir, p.sunDir.xyz, p.cloud.w, p.params.y);
	col += rainbow(dir, p.sunDir.xyz, p.star2.w);   // rain + sun → spectral arc (clouds occlude it below)
	col += moonCorona(dir, p.sunDir.xyz, p.sunDir.w > 0.5, p.sunColor.w); // subtle phase-shaped glow ring around the moon
	float cloudT = 1.0;                                     // view-ray cloud transmittance
	if (p.star2.z > 0.5)
	{
		// Low-res clouds: composite the upsampled (L, T) from the quarter-res pre-pass. The
		// pre-pass was rendered with a DIFFERENT (previous-frame) camera, so reproject this
		// pixel's world view direction into that camera's screen space — otherwise the clouds
		// lag/swim relative to the freshly-drawn sky while panning. Identity when cameras match.
		float4 pc = prevVP * float4(dir, 0.0);           // direction → point at infinity
		float2 uv = (pc.w > 1e-4)
			? float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5)
			: float2(in.ndc.x * 0.5 + 0.5, 0.5 - in.ndc.y * 0.5);
		float4 lt = cloudTex.sample(cloudSamp, clamp(uv, 0.0, 1.0));
		col = col * lt.a + lt.rgb;
		cloudT = lt.a;
	}
	else if (p.cameraPos.w > 0.5 && p.neb2.y > 0.5)
	{
		// Clouds lit by the star-free atmosphere; stars/nebula/moon (in `col`)
		// are occluded by the transmittance instead of being painted onto the
		// cloud bodies. L = comp − atmoBase·T recovers the clouds' own light.
		float3 comp = applyClouds3DReal(atmoBase, dir, p.cameraPos.xyz, p.sunDir.xyz, p.params.z,
		                                p.params.y, p.sunColor.xyz, p.wind.xyz, p.cloud.x, p.cloud.z,
		                                p.cloud.y, p.star2.y, p.cloudTint.xyz, p.neb2.z, p.neb2.w,
		                                in.position.xy, noiseTex, noiseSamp, cloudT);
		col = col * cloudT + (comp - atmoBase * cloudT);
	}
	else if (p.cameraPos.w > 0.5)
		col = applyClouds3D(col, dir, p.cameraPos.xyz, p.sunDir.xyz, p.params.z, p.params.y,
		                    p.sunColor.xyz, p.wind.xyz, p.cloud.x, p.cloud.z, p.cloud.y, p.star2.y,
		                    p.cloudTint.xyz, in.position.xy, noiseTex, noiseSamp, cloudT);
	else
		col = applyClouds(col, dir, p.sunDir.xyz, p.params.z, p.params.y, p.sunColor.xyz, p.wind.xyz,
		                  p.cloudTint.xyz, p.cloud.y, p.star2.y, noiseTex, noiseSamp, cloudT);
	// Re-add the sun, steeply occluded by cloud opacity so a solid cloud fully hides it.
	col += (sunGlareCol + sunBodyCol) * pow(cloudT, 2.5);
	// God-rays: sun shafts through cloud gaps. Scaled by cloudT so a cloud directly in
	// front dims the shaft (you see the rays in the clear air, not painted over the cloud).
	col += crepuscular(dir, p.sunDir.xyz, p.sunColor.xyz, p.params.z, p.params.y, p.wind.xyz,
	                   p.nebulaColor3.w, noiseTex, noiseSamp) * cloudT;
	col += p.wind.w * float3(0.85, 0.90, 1.0); // lightning lights up the sky/clouds
	return float4(col, 1.0);
}
)MSL";

// Shared analytic sky, injected (via the //#SKYFUNC# marker) into the scene and
// skybox MSL so background and image-based ambient match. Mirrors the GL
// kSkyFuncGLSL exactly: the mood follows the sun's elevation (day↔sunset↔night).
static const char* kSkyFuncMSL = R"MSL(
// ---- Physically-based single-scattering atmosphere (Rayleigh + Mie + ozone) ----
// Compact fixed-step single scatter for a ground-level camera: 12 view samples,
// each with a 5-sample sun-transmittance march. Sunset reddening, the blue hour
// and the horizon's pale saturation all EMERGE from the optical-depth integrals
// instead of hand-tuned gradient blends. Mirrors GL + the CPU IBL bakes — keep
// all four copies in sync.
// "No hit" returns a NEGATIVE near distance. That sign matters: the caller's
// sun-visibility test is `atmoRaySphere(p, sunDir, Rg).x > 0.0` → shadowed, and a
// ray that misses the planet entirely is the one case where the sun is certainly
// VISIBLE. A positive miss sentinel therefore marked every such sample shadowed,
// which is most of the sky the moment the sun nears the horizon — atmoScatter
// collapsed to exactly zero at sunY = 0 and the sky snapped to black at sunset.
// The other two callers only test for a hit IN FRONT, so a negative sentinel is
// correct for them too.
float2 atmoRaySphere(float3 ro, float3 rd, float R)
{
	float b = dot(ro, rd);
	float c = dot(ro, ro) - R * R;
	float d = b * b - c;
	if (d < 0.0) return float2(-1.0e9, -1.0e9);
	d = sqrt(d);
	return float2(-b - d, -b + d);
}
float3 atmoScatter(float3 dir, float3 sunDir)
{
	const float Rg = 6360.0e3, Ra = 6440.0e3;                  // ground / atmosphere-top radius
	const float3 bR = float3(5.802e-6, 13.558e-6, 33.1e-6);    // Rayleigh scattering
	const float bM = 3.996e-6;                                 // Mie scattering
	const float3 bO = float3(0.650e-6, 1.881e-6, 0.085e-6);    // ozone absorption
	const float HR = 8500.0, HM = 1200.0;                      // scale heights
	float3 ro = float3(0.0, Rg + 200.0, 0.0);
	float2 tA = atmoRaySphere(ro, dir, Ra);
	if (tA.y <= 0.0) return float3(0.0);
	float t0 = max(tA.x, 0.0), t1 = tA.y;
	float2 tG = atmoRaySphere(ro, dir, Rg);
	if (tG.x > 0.0) t1 = min(t1, tG.x);                        // stop at the ground
	float ds = (t1 - t0) / 12.0;
	float mu = dot(dir, sunDir);
	float phR = 0.05968310 * (1.0 + mu * mu);                  // Rayleigh phase 3/(16π)
	const float g = 0.76, g2 = g * g;
	float phM = 0.11936620 * ((1.0 - g2) * (1.0 + mu * mu)) /  // Cornette-Shanks
	            ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
	float3 sumR = float3(0.0), sumM = float3(0.0);
	float odR = 0.0, odM = 0.0, odO = 0.0;                     // view-path optical depths
	for (int i = 0; i < 12; ++i)
	{
		float3 p   = ro + dir * (t0 + (float(i) + 0.5) * ds);
		float  hgt = length(p) - Rg;
		float  dR  = exp(-hgt / HR) * ds;
		float  dM  = exp(-hgt / HM) * ds;
		float  dO  = max(0.0, 1.0 - abs(hgt - 25.0e3) / 15.0e3) * ds;  // ozone tent layer @25km
		odR += dR; odM += dM; odO += dO;
		if (atmoRaySphere(p, sunDir, Rg).x > 0.0) continue;    // sun below local horizon → shadowed
		float sl = atmoRaySphere(p, sunDir, Ra).y * 0.2;       // 5-sample sun march
		float sR = 0.0, sM = 0.0, sO = 0.0;
		for (int j = 0; j < 5; ++j)
		{
			float3 q  = p + sunDir * ((float(j) + 0.5) * sl);
			float  hq = length(q) - Rg;
			sR += exp(-hq / HR) * sl;
			sM += exp(-hq / HM) * sl;
			sO += max(0.0, 1.0 - abs(hq - 25.0e3) / 15.0e3) * sl;
		}
		float3 tau = bR * (odR + sR) + (bM * 1.11) * (odM + sM) + bO * (odO + sO);
		float3 tr  = exp(-tau);
		sumR += tr * dR;
		sumM += tr * dM;
	}
	float3 L = (sumR * bR * phR + sumM * bM * phM) * 20.0;     // sun irradiance → engine exposure
	// Fake MULTIPLE scattering: single scatter alone leaves long grazing paths
	// yellow/dark at noon (the in-filled skylight is missing). Fill proportional
	// to how opaque the view path is, fading out toward sunset so dusk stays warm.
	float3 Tcam = exp(-(bR * odR + (bM * 1.11) * odM + bO * odO));
	L += (float3(1.0) - Tcam) * float3(0.30, 0.42, 0.60) * (0.35 * smoothstep(0.0, 0.35, sunDir.y));
	return L;
}
float3 skyColor(float3 dir, float3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	// The clamp above pins everything below -0.3, which is fine for the day/dusk
	// tints but useless for "how deep into the night are we" — at true midnight it
	// still reads -0.3. The two night ramps therefore use the RAW elevation, so
	// twilight actually reaches zero instead of leaving a permanent glow on the
	// midnight horizon.
	float sunYd = sunDir.y;
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.14, 0.04, sunY)
	           * (1.0 - smoothstep(0.04, 0.26, sunY));
	// Handed over to only once the twilight wedge below has faded, so the two
	// never leave a dark gap between them.
	float toNight = 1.0 - smoothstep(-0.34, -0.14, sunYd);

	// Physically-based base sky: day blue, sunset reddening and the blue hour all
	// come from the single-scattering integral above. Below-horizon rays reuse the
	// horizon colour (the ground-haze blend takes over there) — without the clamp a
	// hard navy "ocean band" appears where the ray hits the planet after a short path.
	float3 sky = atmoScatter(normalize(float3(dir.x, max(dir.y, 0.004), dir.z)), sunDir);

	// ── Twilight wedge ──────────────────────────────────────────────────────
	// Once the sun is under the horizon the 12-step single-scatter march has
	// almost nothing left to integrate: what still lights the sky comes from
	// hundreds of kilometres away, high up, after several scattering events —
	// which is the whole of civil and nautical twilight. Without it the sky drops
	// to the night floor within a couple of degrees of sunset while the clouds
	// are still catching the sun, and the horizon reads as a hard black edge.
	// Put back as an explicit wedge: warm at the horizon toward the sun, violet
	// as it climbs, deep blue on the far side, fading out into the night floor.
	float twi = smoothstep(0.10, -0.01, sunY) * smoothstep(-0.36, -0.12, sunYd);
	if (twi > 0.0)
	{
		float2 sunAz = normalize(float2(sunDir.x, sunDir.z) + float2(1e-5));
		float2 dxz   = float2(dir.x, dir.z);
		float  hlen  = length(dxz);
		// Straight up and straight down have no azimuth; normalising a ~zero
		// vector snaps to an arbitrary fixed heading, which would put the full
		// sun-side glow on the poles. Fade to neutral instead.
		float  toward = (hlen > 1e-4)
			? clamp(dot(dxz / hlen, sunAz) * 0.5 + 0.5, 0.0, 1.0) : 0.5;
		toward = mix(0.5, toward, smoothstep(0.0, 0.06, hlen));
		float  el    = dir.y;                              // SIGNED — see `above`
		float  band  = exp(-max(el, 0.0) * 5.2);           // hugs the horizon
		float  climb = clamp(max(el, 0.0) * 3.4, 0.0, 1.0);// horizon → overhead
		// The wedge is a SKY term: below the horizon it hands over to the ground
		// blend. Clamping el to 0 instead gave every downward ray the peak
		// horizon glow, and the ground lit up like a desert in the middle of
		// nautical twilight.
		float  above = smoothstep(-0.22, -0.01, el);
		float3 warm  = float3(1.00, 0.45, 0.17);
		float3 mid   = float3(0.62, 0.34, 0.52);
		float3 cool  = float3(0.20, 0.30, 0.62);
		float3 col   = mix(mix(warm, mid, smoothstep(0.0, 0.50, climb)),
		                   cool, smoothstep(0.35, 1.0, climb));
		col = mix(cool, col, toward * toward);     // anti-sun side stays blue
		sky += col * (twi * above * (0.065 + 0.34 * band * toward * toward));
	}

	// Deep-night floor (the scattering term → 0 once the sun is far below the
	// horizon): faint blue gradient so night reflections aren't pitch black.
	float h = clamp(dir.y, 0.0, 1.0);
	sky += mix(float3(0.006, 0.009, 0.024), float3(0.003, 0.005, 0.015), h) * toNight;

	// Below the horizon: ease into ground haze. `sky` still holds the HORIZON
	// colour down here (the dir.y clamp above), so the haze is built out of it —
	// the old fixed grey sat brighter than a twilight sky and drew a hard bright
	// band across the horizon line, and it stayed grey while the sky went warm.
	float3 ground = mix(sky * 0.32, float3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.20, dir.y));

	// Sun aureole ON TOP of the physical Mie glow — just the tight glare blooms now;
	// the broad golden scatter comes from the Cornette-Shanks phase itself.
	float3 sunTint = mix(float3(1.0, 0.58, 0.24), float3(1.0, 0.96, 0.88),
	                     smoothstep(0.0, 0.28, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	float bloomDamp = mix(1.0, 0.28, dusk);                        // dimmer at dusk → no white blob
	sky += sunTint * (pow(s, 220.0)  * 0.9  * bloomDamp) * sunVis; // tight bloom
	sky += sunTint * (pow(s, 30.0)   * 0.12 * bloomDamp) * sunVis; // mid aureole

	// Moon: opposite the sun, fading in at night. The lit disk itself is drawn
	// (textured) in the sky pass; here we keep only the soft halo and a faint
	// fill so the night ambient/reflections aren't pitch black (matches GL).
	float  night    = 1.0 - day;
	float3 moonDir  = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
	float  m        = max(dot(dir, moonDir), 0.0);
	float3 moonTint = float3(0.80, 0.86, 1.00);
	sky += moonTint * (pow(m, 60.0)   * 0.05) * night;
	sky += float3(0.015, 0.018, 0.030) * night;
	return sky;
}
)MSL";

// Replaces the //#SKYFUNC# marker with the shared skyColor() MSL.
static std::string injectSkyMSL(const char* src)
{
	std::string s = src;
	const std::string marker = "//#SKYFUNC#";
	if (size_t pos = s.find(marker); pos != std::string::npos)
		s.replace(pos, marker.size(), kSkyFuncMSL);
	return s;
}

// Matches the MSL Uniforms struct above (float4x4 is column-major like glm).
struct UnlitUniforms
{
	glm::mat4 mvp;
	glm::mat4 model;
	glm::vec4 color;   // rgb: base-color tint
	glm::vec4 flags;   // x: hasTexture
	glm::vec4 pbr;     // x: metallic, y: roughness
	// Optional sun for the world preview: xyz points TOWARD the light, w > 0
	// arms it. w == 0 keeps the fixed studio light every thumbnail was rendered
	// with, so a cached tile does not change the day a preview gains a sky —
	// which is why this is APPENDED rather than folded into a spare lane.
	glm::vec4 sun        = glm::vec4(0.0f);
	glm::vec4 skyAmbient = glm::vec4(0.0f);   // rgb ambient, a unused
	glm::vec4 sunColor   = glm::vec4(1.0f);   // rgb color × intensity
};

// One collected draw for a later replay pass. Used by the transparency pass
// (blended pipeline variant, sorted back-to-front) and — in deferred mode — for
// opaque draws that must run forward in the lighting pass (custom materials
// without a G-buffer variant; `pipeline` is then the OPAQUE material PSO).
// Was a local struct of EncodeScene; hoisted so EncodeGBuffer can hand its
// collected lists over via MetalDeferredFrame.
struct TPDraw { UnlitUniforms u; void* vbuf; void* ibuf; NSUInteger indexCount; void* tex; float distSq;
                void* pipeline = nullptr; std::vector<float> params; bool wpo = false;
                void* gtex[HE::kMatMaxGraphTextures] = { nullptr }; int gtexCount = 0; };

// Per-frame hand-off from the deferred G-buffer pass to the lighting pass (see
// MetalRenderer.h forward declaration). Stack-local to EncodeFrame.
struct MetalDeferredFrame
{
	std::vector<TPDraw> transparent;    // translucent draws → sorted blend replay
	std::vector<TPDraw> forwardOpaque;  // opaque forward-routed draws (no G-buffer variant)
	bool resolveDone = false;           // tile mode: the resolve already ran inside pass 1
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
	glm::vec4 cameraFwd = glm::vec4(0, 0, -1, 0);  // world forward (for planar view-Z cascade select)
	int32_t   lightCount = 0;
	int32_t   pad0 = 0, pad1 = 0, pad2 = 0;
	LightGPU  lights[8];
	// Cascaded Shadow Maps: per-cascade light view-proj (already Metal clip) + split
	// far distances (view space) in xyz, cascade count in w. Replaces the old single
	// lightVP. Layout must stay byte-identical to the MSL SceneUniforms above.
	glm::mat4 cascadeVP[3] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
	glm::vec4 cascadeSplits = glm::vec4(0.0f);
	int32_t   shadowEnabled = 0;
	int32_t   debugCascades = 0;   // 1 = tint fragments by cascade index (debug)
	int32_t   pad3 = 0, pad4 = 0;
	glm::vec4 sunDir = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	glm::vec4 ambient = glm::vec4(0.0f);
	glm::vec4 fog = glm::vec4(0.0f); // x = density (0 = off), y = height falloff
	glm::vec4 viewport = glm::vec4(0.0f); // xy = output size, z = ssaoEnabled
	glm::vec4 weather = glm::vec4(0.0f); // x = wetness, y = snow cover
	// Forward reflection cascade — see the MSL struct above.
	glm::vec4 reflCfg  = glm::vec4(0.0f);
	glm::vec4 reflCfg2 = glm::vec4(0.0f);
	// Local (point/spot) shadow atlas view-projs (already Metal clip); a light's
	// first layer index rides in its LightGPU params.y (-1 = casts no shadow).
	glm::mat4 localShadowVP[16] = {};
	// Cloud shadows (must mirror the MSL struct): A = region origin XZ /
	// 1/size / slab mid-plane Y; B.x = strength (0 = off).
	glm::vec4 cloudShadowA = glm::vec4(0.0f);
	glm::vec4 cloudShadowB = glm::vec4(0.0f);
	// Specular AA (docs/anti-aliasing-plan.md A6): x = strength (0 = off). Every
	// fill site that leaves this zero keeps its image byte-identical.
	glm::vec4 aaParams = glm::vec4(0.0f);
};
static_assert(sizeof(SceneUniforms) ==
              2 * 16 + 16 + 8 * 64 + 3 * 64 + 16 + 16 + 7 * 16 + 16 * 64 + 3 * 16,
              "SceneUniforms must stay byte-identical to its MSL twin");

// Matches the MSL SSAOPosUniforms / SSAOParams structs.
struct SSAOPosUniforms
{
	glm::mat4 mvp;
	glm::mat4 modelView;
	glm::mat4 model; // world transform (reflPosVertex world-space normals)
};
struct SSAOParamsCPU
{
	glm::mat4 proj;
	glm::vec4 cfg;          // xy = noise scale, z = radius, w = bias
	glm::vec4 cfg2;         // x = intensity, y = AO method (0 SSAO, 1 HBAO, 2 GTAO)
	glm::vec4 samples[32];  // hemisphere kernel (xyz)
};

// GI (Checkpoint B): matches the MSL GIUniforms struct at fragmentMain's
// buffer(3). Deliberately a SEPARATE struct from SceneUniforms (not grown into
// it) — buffer(1) is already claimed by HE::MaterialShaderLibrary::
// kMetalLightingBufferIndex for the custom-material path, so GI uses buffer(3)
// (verified free — see EncodeScene's binding table).
struct GIUniforms
{
	int32_t enabled = 0;
	int32_t pad0 = 0, pad1 = 0, pad2 = 0;
	glm::vec4 gridOrigin = glm::vec4(0.0f); // xyz = world-space probe-grid origin, w = spacing
	glm::vec4 gridCounts = glm::vec4(0.0f); // xyz = probe counts per axis, w = probesPerRow
	glm::vec4 params     = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // x = indirectIntensity
};
static_assert(sizeof(GIUniforms) == 64, "GIUniforms must stay byte-identical to its MSL twin");

// Shared by all 3 GIUniforms call sites (EncodeScene's opaque + transparency
// passes, EncodeSkinnedObjects) so the grid fields can't drift between them.
static GIUniforms BuildGIUniforms(bool active, const glm::vec3& gridOrigin, float spacing,
                                  const glm::ivec3& gridCounts, int probesPerRow, float indirectIntensity)
{
	GIUniforms gi{};
	gi.enabled    = active ? 1 : 0;
	gi.gridOrigin = glm::vec4(gridOrigin, spacing);
	gi.gridCounts = glm::vec4(static_cast<float>(gridCounts.x), static_cast<float>(gridCounts.y),
	                         static_cast<float>(gridCounts.z), static_cast<float>(probesPerRow));
	gi.params     = glm::vec4(indirectIntensity, 0.0f, 0.0f, 0.0f);
	return gi;
}

// Matches the MSL GIPosUniforms / GIShadowParams / GITemporalParams structs
// (kGIShadowMSL, used only by EncodeGIShadowRays).
struct GIPosUniformsCPU { glm::mat4 mvp; glm::mat4 model; };
struct GIShadowParamsCPU
{
	glm::vec4 sunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
	glm::vec4 frame;        // x = jitter seed, y = tex width, z = tex height, w = SW instance count
	glm::vec4 localPosRange[4]; // xyz = local light position, w = range
	glm::vec4 extra;            // x = local light count
};
static_assert(sizeof(GIShadowParamsCPU) == 7 * 16, "must match the MSL GIShadowParams layout");
// Matches the MSL GIReflParams struct (kGIReflMSL + kGISWMSL's copy, used only
// by EncodeGIReflections — keep all three in sync).
struct GIReflParamsCPU
{
	glm::mat4 invViewProj;
	glm::mat4 prevViewProj; // LAST frame's view-proj (temporal reprojection)
	glm::vec4 camPos;       // xyz = camera world pos, w = max ray distance
	glm::vec4 texSize;      // xy = output size, z = max roughness, w = probe field valid
	glm::vec4 sunDirRadius; // xyz = direction TOWARD the dominant light, w = indirect intensity
	glm::vec4 sunColor;     // rgb = dominant light colour * intensity, w = frame seed
	glm::vec4 skyAmbient;   // rgb = flat ambient floor, w = history blend (0 = temporal off)
	glm::vec4 gridOrigin;   // xyz = probe-grid origin, w = spacing
	glm::vec4 gridCounts;   // xyz = probes per axis, w = probesPerRow
	glm::vec4 extra;        // x = glossy jitter on, y = mesh data valid, z = SW instance count, w = max bounces
	glm::vec4 land;         // x = landscape count (capped at kGiMaxLandscapes), y = rays per pixel
};
static_assert(sizeof(GIReflParamsCPU) == 2 * 64 + 9 * 16, "must match the MSL GIReflParams layout");
struct GITemporalParamsCPU
{
	glm::mat4 prevViewProj;
	glm::vec4 blend; // x = history weight (0 on first activation frame), y/z = tex width/height, w unused
};

// Matches the MSL GIProbeParams struct (kGIProbeMSL, EncodeGIProbeUpdate only).
struct GIProbeParamsCPU
{
	glm::vec4 gridOrigin;   // xyz = world-space grid origin, w = spacing
	glm::vec4 gridCounts;   // xyz = probe counts per axis, w = probesPerRow
	glm::vec4 rayParams;    // x = max ray distance, y = hysteresis, z = cursor start, w = probes this batch
	glm::vec4 sunDirRadius; // xyz = direction TOWARD the sun, w = local light count
	glm::vec4 sunColor;     // rgb = sun colour * intensity, w unused
	glm::vec4 skyAmbient;   // rgb = flat ambient/sky colour used on ray miss, w unused
	glm::vec4 lightPosRange[8];  // xyz = world position, w = range
	glm::vec4 lightColorType[8]; // rgb = colour * intensity, w = type (1 point, 2 spot)
	glm::vec4 lightDirCos[8];    // xyz = spot travel direction, w = cos(half angle)
};
static_assert(sizeof(GIProbeParamsCPU) == (6 + 24) * 16, "GIProbeParamsCPU must match the MSL GIProbeParams layout");

// The MSL SkyParams twin (mat4 + 17×float4, with the byte-layout static_assert) is
// HE::SkyFrameParams in <HorizonRendering/SkyFrameParams.h>, and the one
// EnvironmentSettings → sky-constants translation is HE::BuildSkyFrameParams.
// The GL-clip → Metal-clip depth remap (0..1) is HE::kMetalClipFix in
// <HorizonRendering/ClipSpace.h>. Metal NDC y is up like GL, so it holds no y flip
// — that flip happens when SAMPLING (texture origin is top-left).

MetalRenderer::MetalRenderer()  = default;
MetalRenderer::~MetalRenderer() = default;

void MetalRenderer::Initialize(HE::Window* window)
{
	HE_LOG_INFO(RHI, "%s", "MetalRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device)
		throw std::runtime_error("MetalRenderer: MTLCreateSystemDefaultDevice failed");
	m_device = (void*)CFBridgingRetain(device);
	EnsureRaytracingSupport();

	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (!queue)
		throw std::runtime_error("MetalRenderer: newCommandQueue failed");
	m_commandQueue = (void*)CFBridgingRetain(queue);

	CreateTarget(m_primarySdlWindow, m_primaryTarget);
	CreateScenePipeline();
	CreateDebugLinePipeline();
	CreateParticlePipeline();
	EnsureShadowResources();
	// GI shadow-ray / probe-update pipelines use MSL 2.4 (intersection_query,
	// macOS 12+) — only build them on devices/OS that actually support it, so
	// unsupported systems never pay the compile cost or risk a compile failure on
	// older toolchains. (Probe atlases + grid are still built lazily on first use,
	// same as before — only the pipeline is warmed up eagerly here.)
	if (m_giSupported) { EnsureGIShadowPipelines(); EnsureGIProbePipeline(); }

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

	// Deferred-path debug/headless knobs: HE_RENDER_PATH=1/deferred forces the
	// path without touching config (he_shot A/B), HE_DUMP_GBUFFER=1..4 makes the
	// resolve output a raw G-buffer view (BaseColor/Normal/RoughSpecMetal/Emissive).
	if (const char* rp = std::getenv("HE_RENDER_PATH"); rp && *rp)
		m_renderPath = (std::string(rp) == "1" || std::string(rp) == "deferred")
			? HE::RenderPath::Deferred : HE::RenderPath::Forward;
	if (const char* dv = std::getenv("HE_DUMP_GBUFFER"); dv && *dv)
		m_gbufferDebugView = std::clamp(std::atoi(dv), 0, 4);
	// P6 tile mode: single-pass memoryless G-buffer via framebuffer fetch —
	// Apple-GPU family only ([[color(n)]] fragment inputs). HE_DEFERRED_TILE=0/1
	// overrides (0 forces the two-pass stored path for A/B and debugging).
	if (@available(macOS 10.15, *))
		m_deferredTileMode = [device supportsFamily:MTLGPUFamilyApple1];
	if (const char* tm = std::getenv("HE_DEFERRED_TILE"); tm && *tm)
		m_deferredTileMode = std::atoi(tm) != 0;
	// P7 clustered lighting in the deferred resolve — on by default;
	// HE_DEFERRED_CLUSTER=0 forces the 8-light resolve (A/B guard).
	if (const char* cl = std::getenv("HE_DEFERRED_CLUSTER"); cl && *cl)
		m_deferredClustered = std::atoi(cl) != 0;

	HE_LOG_INFO(RHI, "%s",
		(std::string("MetalRenderer: initialized on ") + [[device name] UTF8String]).c_str());
}

void MetalRenderer::Shutdown()
{
	HE_LOG_INFO(RHI, "%s", "MetalRenderer: shutdown");
	m_shaderManager.cleanup();

	if (m_timestampCounterSet)
	{
		CFBridgingRelease(m_timestampCounterSet);
		m_timestampCounterSet = nullptr;
	}
	// Reset so a re-Initialize() re-probes counter support instead of silently
	// falling back to whole-frame timing forever.
	m_gpuTimerChecked = false;
	m_gpuTimer        = nullptr;
	m_prevCpuTs = 0;
	m_prevGpuTs = 0;

	for (auto& [sdlWin, target] : m_secondaryTargets)
		DestroyTarget(target);
	m_secondaryTargets.clear();
	DestroyTarget(m_primaryTarget);

	for (auto& [id, mesh] : m_meshCache)
	{
		if (mesh.vertexBuf) CFBridgingRelease(mesh.vertexBuf);
		if (mesh.indexBuf)  CFBridgingRelease(mesh.indexBuf);
		if (mesh.texture)   CFBridgingRelease(mesh.texture);
		if (mesh.blas)      CFBridgingRelease(mesh.blas);
	}
	m_meshCache.clear();

	for (auto& [id, smesh] : m_skeletalMeshCache)
	{
		if (smesh.vertexBuf)  CFBridgingRelease(smesh.vertexBuf);
		if (smesh.boneIdBuf)  CFBridgingRelease(smesh.boneIdBuf);
		if (smesh.boneWgtBuf) CFBridgingRelease(smesh.boneWgtBuf);
		if (smesh.indexBuf)   CFBridgingRelease(smesh.indexBuf);
		if (smesh.texture)    CFBridgingRelease(smesh.texture);
	}
	m_skeletalMeshCache.clear();

	for (auto& [id, tex] : m_materialTexCache)
		if (tex) CFBridgingRelease(tex);
	m_materialTexCache.clear();
	for (auto& [k, tex] : m_graphTexCache)
		if (tex) CFBridgingRelease(tex);
	m_graphTexCache.clear();

	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyGBufferTargets();
	if (m_gbufferPipeline)             { CFBridgingRelease(m_gbufferPipeline);             m_gbufferPipeline = nullptr; }
	// Dropped with its twin: EnsureDeferredPipelines keys "already built" off
	// m_gbufferPipeline, so a surviving instanced PSO would be a stale descriptor.
	if (m_gbufferInstancedPipeline)    { CFBridgingRelease(m_gbufferInstancedPipeline);    m_gbufferInstancedPipeline = nullptr; }
	if (m_deferredResolvePipeline)     { CFBridgingRelease(m_deferredResolvePipeline);     m_deferredResolvePipeline = nullptr; }
	if (m_deferredResolveTilePipeline) { CFBridgingRelease(m_deferredResolveTilePipeline); m_deferredResolveTilePipeline = nullptr; }
	if (m_decalPipeline) { CFBridgingRelease(m_decalPipeline); m_decalPipeline = nullptr; }
	m_decalPipelineTried = false;
	if (m_ssrTracePipeline)     { CFBridgingRelease(m_ssrTracePipeline);     m_ssrTracePipeline = nullptr; }
	if (m_ssrCompositePipeline) { CFBridgingRelease(m_ssrCompositePipeline); m_ssrCompositePipeline = nullptr; }
	if (m_ssrBlurPipeline)      { CFBridgingRelease(m_ssrBlurPipeline);      m_ssrBlurPipeline = nullptr; }
	if (m_ssrColorHist)         { CFBridgingRelease(m_ssrColorHist);         m_ssrColorHist = nullptr; }
	m_ssrColorHistValid = false;
	m_ssrColorHistW = m_ssrColorHistH = 0;
	m_ssrPipelinesTried = false;
	DestroySSRTarget();
	if (m_giReflPipeline) { CFBridgingRelease(m_giReflPipeline); m_giReflPipeline = nullptr; }
	m_giReflPipelineTried = false;
	DestroyGIReflTarget();
	m_deferredPipelinesTried = false; // re-Initialize() rebuilds instead of staying forward
	DestroyBloomTargets();
	DestroyCloudTarget();
	DestroyLdrTarget();
	DestroySSAOTargets();
	DestroyGIShadowTargets();
	DestroyGIProbeAtlas();
	DrainRetiredTextures();
	DrainRetiredGIObjects();
	if (m_giTlas)                { CFBridgingRelease(m_giTlas);                m_giTlas = nullptr; }
	if (m_giInstanceBuffer)      { CFBridgingRelease(m_giInstanceBuffer);      m_giInstanceBuffer = nullptr; }
	if (m_giInstanceColorBuffer) { CFBridgingRelease(m_giInstanceColorBuffer); m_giInstanceColorBuffer = nullptr; }
	if (m_giLandBuf)         { CFBridgingRelease(m_giLandBuf);         m_giLandBuf         = nullptr; }
	if (m_giInstanceLandBuf) { CFBridgingRelease(m_giInstanceLandBuf); m_giInstanceLandBuf = nullptr; }
	m_giUniqueBlas.clear();
	if (m_giGBufPipeline)           { CFBridgingRelease(m_giGBufPipeline);           m_giGBufPipeline = nullptr; }
	if (m_giShadowRayPipeline)      { CFBridgingRelease(m_giShadowRayPipeline);      m_giShadowRayPipeline = nullptr; }
	if (m_giShadowTemporalPipeline) { CFBridgingRelease(m_giShadowTemporalPipeline); m_giShadowTemporalPipeline = nullptr; }
	if (m_giShadowBlurPipeline)     { CFBridgingRelease(m_giShadowBlurPipeline);     m_giShadowBlurPipeline = nullptr; }
	if (m_giProbeUpdatePipeline)    { CFBridgingRelease(m_giProbeUpdatePipeline);    m_giProbeUpdatePipeline = nullptr; }
	if (m_giShadowRaySwPipeline)    { CFBridgingRelease(m_giShadowRaySwPipeline);    m_giShadowRaySwPipeline = nullptr; }
	if (m_giProbeUpdateSwPipeline)  { CFBridgingRelease(m_giProbeUpdateSwPipeline);  m_giProbeUpdateSwPipeline = nullptr; }
	if (m_giSwNodeBuf)     { CFBridgingRelease(m_giSwNodeBuf);     m_giSwNodeBuf = nullptr; }
	if (m_giSwTriBuf)      { CFBridgingRelease(m_giSwTriBuf);      m_giSwTriBuf = nullptr; }
	if (m_giSwInstanceBuf) { CFBridgingRelease(m_giSwInstanceBuf); m_giSwInstanceBuf = nullptr; }
	m_giSwBlasCache.clear();
	m_giSwNodesCpu.clear();
	m_giSwTrisCpu.clear();
	m_giSwInstanceCount = 0;
	m_giSwBlasDirty     = false;
	// Reset so a re-Initialize() rebuilds the grid/re-probes support instead of
	// keeping a stale (possibly wrong-device, wrong-scene) cached result.
	m_giRaytracingChecked = false;
	m_giSupported         = false;
	m_giHwRt              = false;
	m_giProbeGridBuilt    = false;
	m_giProbeCount        = 0;
	m_giProbeUpdateCursor = 0;
	if (m_tonemapPipeline)      { CFBridgingRelease(m_tonemapPipeline);      m_tonemapPipeline = nullptr; }
	for (auto& [k, pso] : m_materialPipelineCache) if (pso) CFBridgingRelease(pso);
	m_materialPipelineCache.clear();
	for (auto& [k, pso] : m_particlePipelineCache) if (pso) CFBridgingRelease(pso);
	m_particlePipelineCache.clear();
	if (m_matBinaryArchive) { CFBridgingRelease(m_matBinaryArchive); m_matBinaryArchive = nullptr; }
	m_matArchiveTried = false;
	if (m_previewColorTex) { CFBridgingRelease(m_previewColorTex); m_previewColorTex = nullptr; }
	if (m_previewDepthTex) { CFBridgingRelease(m_previewDepthTex); m_previewDepthTex = nullptr; }
	if (m_previewVB)       { CFBridgingRelease(m_previewVB);       m_previewVB = nullptr; }
	if (m_previewIB)       { CFBridgingRelease(m_previewIB);       m_previewIB = nullptr; }
	m_previewSize = 0;
	// Content-Browser thumbnail target + its mesh pipeline.
	if (m_thumbColorTex)       { CFBridgingRelease(m_thumbColorTex);       m_thumbColorTex = nullptr; }
	if (m_thumbDepthTex)       { CFBridgingRelease(m_thumbDepthTex);       m_thumbDepthTex = nullptr; }
	if (m_meshPreviewPipeline) { CFBridgingRelease(m_meshPreviewPipeline); m_meshPreviewPipeline = nullptr; }
	if (m_thumbUIColorTex)     { CFBridgingRelease(m_thumbUIColorTex);     m_thumbUIColorTex = nullptr; }
	if (m_thumbUIDepthTex)     { CFBridgingRelease(m_thumbUIDepthTex);     m_thumbUIDepthTex = nullptr; }
	m_thumbSize = 0;
	m_thumbUISize = 0;
	// World-preview target (RenderWorldPreview); its pipelines are the skeletal /
	// mesh preview's, released with those.
	if (m_worldPreviewColorTex) { CFBridgingRelease(m_worldPreviewColorTex); m_worldPreviewColorTex = nullptr; }
	if (m_worldPreviewHdrTex)   { CFBridgingRelease(m_worldPreviewHdrTex);   m_worldPreviewHdrTex = nullptr; }
	if (m_worldPreviewDepthTex) { CFBridgingRelease(m_worldPreviewDepthTex); m_worldPreviewDepthTex = nullptr; }
	m_worldPreviewW = 0;
	m_worldPreviewH = 0;
	if (m_fxaaPipeline)         { CFBridgingRelease(m_fxaaPipeline);         m_fxaaPipeline = nullptr; }
	if (m_aaBlitPipeline)       { CFBridgingRelease(m_aaBlitPipeline);       m_aaBlitPipeline = nullptr; }
	if (m_smaaPipeline)         { CFBridgingRelease(m_smaaPipeline);         m_smaaPipeline = nullptr; }
	if (m_velocityPipeline)     { CFBridgingRelease(m_velocityPipeline);     m_velocityPipeline = nullptr; }
	if (m_taaPipeline)          { CFBridgingRelease(m_taaPipeline);          m_taaPipeline = nullptr; }
	if (m_taaSharpenPipeline)   { CFBridgingRelease(m_taaSharpenPipeline);   m_taaSharpenPipeline = nullptr; }
	DestroyTaaTargets();
	if (m_uiPipeline)           { CFBridgingRelease(m_uiPipeline);           m_uiPipeline = nullptr; }
	if (m_uiFontTexture)        { CFBridgingRelease(m_uiFontTexture);        m_uiFontTexture = nullptr; }
	for (auto& [k, t] : m_uiFontAtlases) if (t) CFBridgingRelease(t);
	m_uiFontAtlases.clear();
	for (auto& [k, pso] : m_uiMaterialPipelines) if (pso) CFBridgingRelease(pso);
	m_uiMaterialPipelines.clear();
	if (m_bloomBrightPipeline)  { CFBridgingRelease(m_bloomBrightPipeline);  m_bloomBrightPipeline = nullptr; }
	if (m_blurPipeline)         { CFBridgingRelease(m_blurPipeline);         m_blurPipeline = nullptr; }
	if (m_skyPipeline)          { CFBridgingRelease(m_skyPipeline);          m_skyPipeline = nullptr; }
	if (m_cloudPipeline)        { CFBridgingRelease(m_cloudPipeline);        m_cloudPipeline = nullptr; }
	if (m_cloudShadowPipeline)  { CFBridgingRelease(m_cloudShadowPipeline);  m_cloudShadowPipeline = nullptr; }
	DestroyCloudShadowTarget();
	if (m_moonTexture)          { CFBridgingRelease(m_moonTexture);          m_moonTexture = nullptr; }
	if (m_dummyTexture)    { CFBridgingRelease(m_dummyTexture);    m_dummyTexture = nullptr; }
	if (m_linearSampler)   { CFBridgingRelease(m_linearSampler);   m_linearSampler = nullptr; }
	if (m_noiseTexture)    { CFBridgingRelease(m_noiseTexture);    m_noiseTexture = nullptr; }
	if (m_noiseSampler)    { CFBridgingRelease(m_noiseSampler);    m_noiseSampler = nullptr; }
	if (m_skyEnvCube)      { CFBridgingRelease(m_skyEnvCube);      m_skyEnvCube = nullptr; }
	if (m_scenePipeline)        { CFBridgingRelease(m_scenePipeline);        m_scenePipeline = nullptr; }
	if (m_sceneInstancedPipeline) { CFBridgingRelease(m_sceneInstancedPipeline); m_sceneInstancedPipeline = nullptr; }
	if (m_sceneBlendPipeline)   { CFBridgingRelease(m_sceneBlendPipeline);   m_sceneBlendPipeline = nullptr; }
	if (m_skinnedPipeline)      { CFBridgingRelease(m_skinnedPipeline);      m_skinnedPipeline = nullptr; }
	if (m_particleSimPipeline)  { CFBridgingRelease(m_particleSimPipeline);  m_particleSimPipeline = nullptr; }
	if (m_particleDrawPipeline) { CFBridgingRelease(m_particleDrawPipeline); m_particleDrawPipeline = nullptr; }
	if (m_particleBuffer)       { CFBridgingRelease(m_particleBuffer);       m_particleBuffer = nullptr; }
	if (m_sceneDepthState) { CFBridgingRelease(m_sceneDepthState); m_sceneDepthState = nullptr; }
	if (m_shadowPipeline)  { CFBridgingRelease(m_shadowPipeline);  m_shadowPipeline = nullptr; }
	if (m_shadowDepthTex)  { CFBridgingRelease(m_shadowDepthTex);  m_shadowDepthTex = nullptr; }
	if (m_localShadowTex)  { CFBridgingRelease(m_localShadowTex);  m_localShadowTex = nullptr; }
	if (m_noDepthState)    { CFBridgingRelease(m_noDepthState);    m_noDepthState = nullptr; }
	if (m_skyDepthState)   { CFBridgingRelease(m_skyDepthState);   m_skyDepthState = nullptr; }
	if (m_ssaoPosPipeline)  { CFBridgingRelease(m_ssaoPosPipeline);  m_ssaoPosPipeline = nullptr; }
	if (m_reflPosPipeline)  { CFBridgingRelease(m_reflPosPipeline);  m_reflPosPipeline = nullptr; }
	if (m_ssaoDepthPosPipeline) { CFBridgingRelease(m_ssaoDepthPosPipeline); m_ssaoDepthPosPipeline = nullptr; }
	if (m_ssaoPipeline)     { CFBridgingRelease(m_ssaoPipeline);     m_ssaoPipeline = nullptr; }
	if (m_ssaoBlurPipeline) { CFBridgingRelease(m_ssaoBlurPipeline); m_ssaoBlurPipeline = nullptr; }
	if (m_ssaoNoiseTex)     { CFBridgingRelease(m_ssaoNoiseTex);     m_ssaoNoiseTex = nullptr; }
	if (m_ssaoPointSampler) { CFBridgingRelease(m_ssaoPointSampler); m_ssaoPointSampler = nullptr; }
	if (m_ssaoNoiseSampler)  { CFBridgingRelease(m_ssaoNoiseSampler);  m_ssaoNoiseSampler = nullptr; }
	if (m_debugLinePipeline) { CFBridgingRelease(m_debugLinePipeline); m_debugLinePipeline = nullptr; }
	ReleaseRibbonBuffers();

	if (m_imguiPassDescriptor) { CFBridgingRelease(m_imguiPassDescriptor); m_imguiPassDescriptor = nullptr; }
	if (m_commandQueue)        { CFBridgingRelease(m_commandQueue);        m_commandQueue = nullptr; }
	if (m_device)              { CFBridgingRelease(m_device);              m_device = nullptr; }
	m_primarySdlWindow = nullptr;
}

// ─── Pipeline / mesh setup ────────────────────────────────────────────────────

// The SSAO kernel + 4×4 rotation-noise tile are HE::BuildSSAOKernel /
// HE::BuildSSAONoise in <HorizonRendering/SsaoKernel.h> — one deterministic
// generator for every backend, which is what makes GL == Metal == D3D == Vulkan.

void MetalRenderer::SetDebugLines(const std::vector<DebugLine>& lines)
{
	m_debugLines = lines;
}

void MetalRenderer::CreateDebugLinePipeline()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* error = nil;
		// Minimal per-vertex-color line shader targeting the RGBA16F HDR target
		NSString* src = @R"(
#include <metal_stdlib>
using namespace metal;
struct DebugVIn  { float3 pos   [[attribute(0)]]; float3 color [[attribute(1)]]; };
struct DebugVOut { float4 pos   [[position]];     float3 color; };
vertex DebugVOut debugLineVert(DebugVIn in [[stage_in]], constant float4x4& vp [[buffer(1)]])
{
    DebugVOut o; o.pos = vp * float4(in.pos, 1.0); o.color = in.color; return o;
}
fragment float4 debugLineFrag(DebugVOut in [[stage_in]])
{
    return float4(in.color, 1.0);
}
)";
		id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&error];
		if (!lib)
		{
			HE_LOG_ERROR(RHI, "%s", "MetalRenderer: debug line shader compile failed");
			return;
		}

		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction   = [lib newFunctionWithName:@"debugLineVert"];
		desc.fragmentFunction = [lib newFunctionWithName:@"debugLineFrag"];
		desc.colorAttachments[0].pixelFormat = kSceneColorFormat; // RGBA16F HDR target
		desc.depthAttachmentPixelFormat      = kDepthFormat;

		// Vertex layout: float3 pos at offset 0, float3 color at offset 12
		MTLVertexDescriptor* vtxDesc = [[MTLVertexDescriptor alloc] init];
		vtxDesc.attributes[0].format      = MTLVertexFormatFloat3;
		vtxDesc.attributes[0].offset      = 0;
		vtxDesc.attributes[0].bufferIndex = 0;
		vtxDesc.attributes[1].format      = MTLVertexFormatFloat3;
		vtxDesc.attributes[1].offset      = 12;
		vtxDesc.attributes[1].bufferIndex = 0;
		vtxDesc.layouts[0].stride         = 24; // 6 floats × 4 bytes
		vtxDesc.layouts[0].stepFunction   = MTLVertexStepFunctionPerVertex;
		desc.vertexDescriptor = vtxDesc;

		id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (pso)
			m_debugLinePipeline = (void*)CFBridgingRetain(pso);
		else
			HE_LOG_ERROR(RHI, "%s", "MetalRenderer: debug line pipeline creation failed");
	}
}

void MetalRenderer::EncodeDebugLines(void* renderEncoderPtr, const glm::mat4& viewProj)
{
	if (m_debugLines.empty() || !m_debugLinePipeline) return;

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;

		// Pack line endpoints: [pos3 color3] per vertex
		const size_t vertCount = m_debugLines.size() * 2;
		const size_t byteSize  = vertCount * 6 * sizeof(float);
		id<MTLBuffer> vbuf = [device newBufferWithLength:byteSize options:MTLResourceStorageModeShared];
		float* ptr = (float*)vbuf.contents;
		for (const DebugLine& l : m_debugLines)
		{
			*ptr++ = l.start.x; *ptr++ = l.start.y; *ptr++ = l.start.z;
			*ptr++ = l.color.r; *ptr++ = l.color.g; *ptr++ = l.color.b;
			*ptr++ = l.end.x;   *ptr++ = l.end.y;   *ptr++ = l.end.z;
			*ptr++ = l.color.r; *ptr++ = l.color.g; *ptr++ = l.color.b;
		}

		// Apply Metal's NDC fix (same as scene pass)
		glm::mat4 vp = HE::kMetalClipFix * viewProj;

		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_debugLinePipeline];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		[enc setVertexBuffer:vbuf offset:0 atIndex:0];
		[enc setVertexBytes:&vp length:sizeof(vp) atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:vertCount];
	}
}

void MetalRenderer::ReleaseRibbonBuffers()
{
	for (void* b : m_ribbonBuffers)
		if (b) CFBridgingRelease(b);
	m_ribbonBuffers.clear();
}

// ─── Motion trails → collected blended draws ─────────────────────────────────
// The whole point of RibbonBatch is that this costs no pipeline: the vertices
// arrive in the cooked layout the scene vertex shader already reads (pos3 +
// norm3 + uv2) and in WORLD space, so the model matrix is the identity and the
// draw is just another entry in the list EncodeScene's transparency pass sorts
// and replays. A trail whose material carries a node graph gets that material's
// blended pipeline, which is what makes the age in uv.v drive colour and fade
// without a single trail-specific uniform.
void MetalRenderer::CollectRibbonDraws(std::vector<TPDraw>& out, const glm::mat4& viewProj,
                                       const glm::vec3& cameraPos)
{
	if (m_renderWorld.ribbonBatches.empty()) return;
	// Last frame's staging buffers go now, not at the end of the frame that used
	// them: the draws below hold bare pointers and are replayed much later.
	ReleaseRibbonBuffers();

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		for (const RibbonBatch& rb : m_renderWorld.ribbonBatches)
		{
			if (rb.vertices.empty() || rb.indices.empty()) continue;

			// Material first, buffers after: every Resolve* below can LOAD, and a
			// load moves the ContentManager's dense pools out from under any
			// pointer taken before it.
			void* tex = nullptr;
			const bool hasTex = ResolveMaterialTexture(rb.materialAssetId, tex);
			glm::vec3 baseColor(1.0f);
			float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
			if (!ResolveMaterialParams(rb.materialAssetId, baseColor, metallic, roughness, opacity))
				baseColor = hasTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);

			TPDraw t{};
			t.u.mvp   = viewProj;              // world vertices ⇒ model = identity
			t.u.model = glm::mat4(1.0f);
			t.u.color = glm::vec4(baseColor, 1.0f);
			t.u.flags = glm::vec4(hasTex ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
			t.u.pbr   = glm::vec4(metallic, roughness, opacity, 0.0f);
			t.tex     = tex ? tex : m_dummyTexture;
			t.indexCount = static_cast<NSUInteger>(rb.indices.size());
			// Sort key from the band's own centre. RenderSorter::backToFrontKey on
			// an identity transform would measure every trail's distance to the
			// WORLD ORIGIN instead, and they would all sort the same.
			const glm::vec3 c = rb.worldBounds.isValid() ? rb.worldBounds.center() : cameraPos;
			t.distSq  = glm::dot(c - cameraPos, c - cameraPos);

#if defined(HE_HAVE_SHADERC)
			// Custom material: the BLENDED pipeline variant, same as a translucent
			// mesh takes in the opaque loop. Null pipeline = the built-in blend PSO.
			uint64_t shKey = 0; std::string shFrag, shVert;
			if (ResolveMaterialShader(rb.materialAssetId, shKey, shFrag, shVert))
			{
				std::vector<HE::UUID>    gtexIds;
				std::vector<std::string> gtexPaths;
				const MaterialShaderVariant* pre = nullptr;
				if (const MaterialAsset* ma = m_contentManager
					? m_contentManager->getMaterial(rb.materialAssetId) : nullptr)
				{
					for (const auto& var : ma->precompiledShaders)
						if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { pre = &var; break; }
					if (!ma->shaderParamData.empty()) t.params = ma->shaderParamData;
					// Snapshot the graph texture slots BEFORE resolving any of them —
					// ResolveGraphTexture loads, and `ma` would not survive it.
					const size_t nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
						std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
					for (size_t i = 0; i < nTex; ++i)
					{
						gtexIds.push_back(i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{});
						gtexPaths.push_back(i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{});
					}
				}
				t.pipeline = GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre, /*blend=*/true);
				t.wpo      = !shVert.empty();
				for (size_t i = 0; i < gtexIds.size(); ++i)
					t.gtex[t.gtexCount++] = ResolveGraphTexture(gtexIds[i], gtexPaths[i]);
			}
#endif

			id<MTLBuffer> vbuf = [device newBufferWithBytes:rb.vertices.data()
			                                         length:rb.vertices.size() * sizeof(float)
			                                        options:MTLResourceStorageModeShared];
			id<MTLBuffer> ibuf = [device newBufferWithBytes:rb.indices.data()
			                                         length:rb.indices.size() * sizeof(uint32_t)
			                                        options:MTLResourceStorageModeShared];
			if (!vbuf || !ibuf) continue;
			m_ribbonBuffers.push_back((void*)CFBridgingRetain(vbuf));
			m_ribbonBuffers.push_back((void*)CFBridgingRetain(ibuf));
			t.vbuf = (__bridge void*)vbuf;
			t.ibuf = (__bridge void*)ibuf;
			out.push_back(std::move(t));
		}
	}
}

void MetalRenderer::CreateScenePipeline()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		NSError* error = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:injectSkyMSL(kUnlitMSL).c_str()] options:nil error:&error];
		if (!lib)
			throw std::runtime_error(std::string("MetalRenderer: unlit shader compile failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));

		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction   = [lib newFunctionWithName:@"vertexMain"];
		desc.fragmentFunction = [lib newFunctionWithName:@"fragmentMain"];
		desc.colorAttachments[0].pixelFormat = kSceneColorFormat; // render into HDR target
		desc.depthAttachmentPixelFormat      = kDepthFormat;

		id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!pso)
			throw std::runtime_error(std::string("MetalRenderer: pipeline creation failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_scenePipeline = (void*)CFBridgingRetain(pso);

		// GPU-instanced variant: identical target/depth formats, only the vertex
		// function differs. Not fatal if it fails — instanced batches then simply
		// keep taking the per-instance loop.
		desc.vertexFunction = [lib newFunctionWithName:@"vertexMainInstanced"];
		NSError* iErr = nil;
		id<MTLRenderPipelineState> instPso =
			[device newRenderPipelineStateWithDescriptor:desc error:&iErr];
		if (instPso) m_sceneInstancedPipeline = (void*)CFBridgingRetain(instPso);
		else
			HE_LOG_WARN(RHI, "%s",
				(std::string("MetalRenderer: instanced scene pipeline build failed — "
				             "batches stay on the per-instance loop: ")
				 + (iErr ? iErr.localizedDescription.UTF8String : "?")).c_str());
		// Back to vertexMain BEFORE the blend PSO: the descriptor is reused, and
		// the transparency pass binds nothing at vertex buffer 5.
		desc.vertexFunction = [lib newFunctionWithName:@"vertexMain"];

		// Alpha-blended variant of the scene pipeline for the transparency pass
		// (same shaders, src-alpha / one-minus-src-alpha over the HDR target).
		desc.colorAttachments[0].blendingEnabled             = YES;
		desc.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
		desc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
		desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
		desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
		desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
		desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		id<MTLRenderPipelineState> blendPso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!blendPso)
			throw std::runtime_error(std::string("MetalRenderer: blend pipeline creation failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_sceneBlendPipeline = (void*)CFBridgingRetain(blendPso);

		// ── Skinned geometry pipeline (linear blend skinning, same fragment shader) ──
		desc.colorAttachments[0].blendingEnabled = NO;
		desc.vertexFunction   = [lib newFunctionWithName:@"skinnedVertex"];
		desc.fragmentFunction = [lib newFunctionWithName:@"fragmentMain"];
		id<MTLRenderPipelineState> skinPso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!skinPso)
			throw std::runtime_error(std::string("MetalRenderer: skinned pipeline creation failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_skinnedPipeline = (void*)CFBridgingRetain(skinPso);

		// ── HDR tonemap pipeline (RGBA16F scene color → swapchain LDR) ──────
		NSError* tmError = nil;
		id<MTLLibrary> tmLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kTonemapMSL] options:nil error:&tmError];
		if (!tmLib)
			throw std::runtime_error(std::string("MetalRenderer: tonemap shader compile failed: ")
				+ (tmError ? [[tmError localizedDescription] UTF8String] : "unknown"));
		MTLRenderPipelineDescriptor* tmDesc = [[MTLRenderPipelineDescriptor alloc] init];
		tmDesc.vertexFunction   = [tmLib newFunctionWithName:@"tonemapVertex"];
		tmDesc.fragmentFunction = [tmLib newFunctionWithName:@"tonemapFragment"];
		tmDesc.colorAttachments[0].pixelFormat = kSwapchainFormat; // LDR output
		// Both tonemap passes carry a (DontCare) depth attachment so this single
		// pipeline is valid whether it runs into the viewport or the drawable.
		tmDesc.depthAttachmentPixelFormat      = kDepthFormat;
		id<MTLRenderPipelineState> tmPso = [device newRenderPipelineStateWithDescriptor:tmDesc error:&tmError];
		if (!tmPso)
			throw std::runtime_error(std::string("MetalRenderer: tonemap pipeline creation failed: ")
				+ (tmError ? [[tmError localizedDescription] UTF8String] : "unknown"));
		m_tonemapPipeline = (void*)CFBridgingRetain(tmPso);

		// ── FXAA pipeline (LDR → LDR, same output format + depth as tonemap) ─
		NSError* fxError = nil;
		id<MTLLibrary> fxLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kFxaaMSL] options:nil error:&fxError];
		if (!fxLib)
			throw std::runtime_error(std::string("MetalRenderer: FXAA shader compile failed: ")
				+ (fxError ? [[fxError localizedDescription] UTF8String] : "unknown"));
		MTLRenderPipelineDescriptor* fxDesc = [[MTLRenderPipelineDescriptor alloc] init];
		fxDesc.vertexFunction   = [fxLib newFunctionWithName:@"fxaaVertex"];
		fxDesc.fragmentFunction = [fxLib newFunctionWithName:@"fxaaFragment"];
		fxDesc.colorAttachments[0].pixelFormat = kSwapchainFormat; // LDR output
		fxDesc.depthAttachmentPixelFormat      = kDepthFormat;     // both targets carry depth
		id<MTLRenderPipelineState> fxPso = [device newRenderPipelineStateWithDescriptor:fxDesc error:&fxError];
		if (!fxPso)
			throw std::runtime_error(std::string("MetalRenderer: FXAA pipeline creation failed: ")
				+ (fxError ? [[fxError localizedDescription] UTF8String] : "unknown"));
		m_fxaaPipeline = (void*)CFBridgingRetain(fxPso);

		// SMAA: same pass, same attachments, different fragment function.
		fxDesc.fragmentFunction = [fxLib newFunctionWithName:@"smaaFragment"];
		id<MTLRenderPipelineState> smaaPso = [device newRenderPipelineStateWithDescriptor:fxDesc error:&fxError];
		if (!smaaPso)
			throw std::runtime_error(std::string("MetalRenderer: SMAA pipeline creation failed: ")
				+ (fxError ? [[fxError localizedDescription] UTF8String] : "unknown"));
		m_smaaPipeline = (void*)CFBridgingRetain(smaaPso);

		// ── TAA: velocity pass + temporal resolve + sharpen (A2/A3) ─────────
		{
			NSError* taaError = nil;
			id<MTLLibrary> taaLib = [device newLibraryWithSource:
				[NSString stringWithUTF8String:kTaaMSL] options:nil error:&taaError];
			if (!taaLib)
				throw std::runtime_error(std::string("MetalRenderer: TAA shader compile failed: ")
					+ (taaError ? [[taaError localizedDescription] UTF8String] : "unknown"));

			// Velocity: RG16F colour, depth-tested against the depth the G-buffer
			// pass already wrote (LessEqual, no write) so only visible surfaces
			// report motion.
			MTLRenderPipelineDescriptor* velDesc = [[MTLRenderPipelineDescriptor alloc] init];
			velDesc.vertexFunction   = [taaLib newFunctionWithName:@"velocityVertex"];
			velDesc.fragmentFunction = [taaLib newFunctionWithName:@"velocityFragment"];
			velDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRG16Float;
			velDesc.depthAttachmentPixelFormat      = kDepthFormat;
			id<MTLRenderPipelineState> velPso =
				[device newRenderPipelineStateWithDescriptor:velDesc error:&taaError];
			if (!velPso)
				throw std::runtime_error(std::string("MetalRenderer: velocity pipeline creation failed: ")
					+ (taaError ? [[taaError localizedDescription] UTF8String] : "unknown"));
			m_velocityPipeline = (void*)CFBridgingRetain(velPso);

			// Temporal resolve: LDR in, LDR out, no depth (its own pass).
			MTLRenderPipelineDescriptor* taaDesc = [[MTLRenderPipelineDescriptor alloc] init];
			taaDesc.vertexFunction   = [taaLib newFunctionWithName:@"taaVertex"];
			taaDesc.fragmentFunction = [taaLib newFunctionWithName:@"taaFragment"];
			taaDesc.colorAttachments[0].pixelFormat = kSwapchainFormat;
			id<MTLRenderPipelineState> taaPso =
				[device newRenderPipelineStateWithDescriptor:taaDesc error:&taaError];
			if (!taaPso)
				throw std::runtime_error(std::string("MetalRenderer: TAA pipeline creation failed: ")
					+ (taaError ? [[taaError localizedDescription] UTF8String] : "unknown"));
			m_taaPipeline = (void*)CFBridgingRetain(taaPso);

			// Sharpen runs in the shared AA-resolve slot, so it needs that slot's
			// attachment formats (colour + depth), like the FXAA/SMAA pipelines.
			MTLRenderPipelineDescriptor* shDesc = [[MTLRenderPipelineDescriptor alloc] init];
			shDesc.vertexFunction   = [taaLib newFunctionWithName:@"taaVertex"];
			shDesc.fragmentFunction = [taaLib newFunctionWithName:@"taaSharpenFragment"];
			shDesc.colorAttachments[0].pixelFormat = kSwapchainFormat;
			shDesc.depthAttachmentPixelFormat      = kDepthFormat;
			id<MTLRenderPipelineState> shPso =
				[device newRenderPipelineStateWithDescriptor:shDesc error:&taaError];
			if (!shPso)
				throw std::runtime_error(std::string("MetalRenderer: TAA sharpen pipeline creation failed: ")
					+ (taaError ? [[taaError localizedDescription] UTF8String] : "unknown"));
			m_taaSharpenPipeline = (void*)CFBridgingRetain(shPso);
		}

		// AA = Off: same pass, same attachments, passthrough fragment.
		fxDesc.fragmentFunction = [fxLib newFunctionWithName:@"aaBlitFragment"];
		id<MTLRenderPipelineState> blitPso = [device newRenderPipelineStateWithDescriptor:fxDesc error:&fxError];
		if (!blitPso)
			throw std::runtime_error(std::string("MetalRenderer: AA blit pipeline creation failed: ")
				+ (fxError ? [[fxError localizedDescription] UTF8String] : "unknown"));
		m_aaBlitPipeline = (void*)CFBridgingRetain(blitPso);

		// ── UI pipeline (2D colored quads, LDR swapchain format, no depth) ─
		NSError* uiError = nil;
		id<MTLLibrary> uiLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kUIMSL] options:nil error:&uiError];
		if (!uiLib)
			throw std::runtime_error(std::string("MetalRenderer: UI shader compile failed: ")
				+ (uiError ? [[uiError localizedDescription] UTF8String] : "unknown"));
		MTLRenderPipelineDescriptor* uiDesc = [[MTLRenderPipelineDescriptor alloc] init];
		uiDesc.vertexFunction   = [uiLib newFunctionWithName:@"uiVertex"];
		uiDesc.fragmentFunction = [uiLib newFunctionWithName:@"uiFragment"];
		uiDesc.colorAttachments[0].pixelFormat     = kSwapchainFormat;
		uiDesc.colorAttachments[0].blendingEnabled = YES;
		uiDesc.colorAttachments[0].rgbBlendOperation       = MTLBlendOperationAdd;
		uiDesc.colorAttachments[0].alphaBlendOperation     = MTLBlendOperationAdd;
		uiDesc.colorAttachments[0].sourceRGBBlendFactor    = MTLBlendFactorSourceAlpha;
		uiDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		uiDesc.colorAttachments[0].sourceAlphaBlendFactor  = MTLBlendFactorOne;
		// Proper "over" for the alpha channel too — was Zero, which OVERWROTE the
		// framebuffer alpha with the source coverage, punching alpha holes (0) in
		// transparent UI regions (between glyphs, a rounded handle's corners). On a
		// non-opaque Metal layer those holes composited to black. OneMinusSourceAlpha
		// keeps the opaque scene's alpha = 1 where the UI is transparent.
		uiDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		uiDesc.depthAttachmentPixelFormat = kDepthFormat;
		id<MTLRenderPipelineState> uiPso = [device newRenderPipelineStateWithDescriptor:uiDesc error:&uiError];
		if (!uiPso)
			throw std::runtime_error(std::string("MetalRenderer: UI pipeline creation failed: ")
				+ (uiError ? [[uiError localizedDescription] UTF8String] : "unknown"));
		m_uiPipeline = (void*)CFBridgingRetain(uiPso);

		// ── Bloom pipelines (bright-pass + blur, into RGBA16F, no depth) ────
		NSError* blError = nil;
		id<MTLLibrary> blLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kBloomMSL] options:nil error:&blError];
		if (!blLib)
			throw std::runtime_error(std::string("MetalRenderer: bloom shader compile failed: ")
				+ (blError ? [[blError localizedDescription] UTF8String] : "unknown"));

		MTLRenderPipelineDescriptor* brDesc = [[MTLRenderPipelineDescriptor alloc] init];
		brDesc.vertexFunction   = [blLib newFunctionWithName:@"fsVertex"];
		brDesc.fragmentFunction = [blLib newFunctionWithName:@"brightFragment"];
		brDesc.colorAttachments[0].pixelFormat = kSceneColorFormat; // half-res HDR
		id<MTLRenderPipelineState> brPso = [device newRenderPipelineStateWithDescriptor:brDesc error:&blError];
		if (!brPso)
			throw std::runtime_error(std::string("MetalRenderer: bloom bright pipeline creation failed: ")
				+ (blError ? [[blError localizedDescription] UTF8String] : "unknown"));
		m_bloomBrightPipeline = (void*)CFBridgingRetain(brPso);

		MTLRenderPipelineDescriptor* bdDesc = [[MTLRenderPipelineDescriptor alloc] init];
		bdDesc.vertexFunction   = [blLib newFunctionWithName:@"fsVertex"];
		bdDesc.fragmentFunction = [blLib newFunctionWithName:@"blurFragment"];
		bdDesc.colorAttachments[0].pixelFormat = kSceneColorFormat;
		id<MTLRenderPipelineState> bdPso = [device newRenderPipelineStateWithDescriptor:bdDesc error:&blError];
		if (!bdPso)
			throw std::runtime_error(std::string("MetalRenderer: bloom blur pipeline creation failed: ")
				+ (blError ? [[blError localizedDescription] UTF8String] : "unknown"));
		m_blurPipeline = (void*)CFBridgingRetain(bdPso);

		// ── Skybox pipeline (into the HDR target; carries the scene depth fmt) ──
		NSError* skyError = nil;
		id<MTLLibrary> skyLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:injectSkyMSL(kSkyMSL).c_str()] options:nil error:&skyError];
		if (!skyLib)
			throw std::runtime_error(std::string("MetalRenderer: sky shader compile failed: ")
				+ (skyError ? [[skyError localizedDescription] UTF8String] : "unknown"));
		MTLRenderPipelineDescriptor* skyDesc = [[MTLRenderPipelineDescriptor alloc] init];
		skyDesc.vertexFunction   = [skyLib newFunctionWithName:@"skyVertex"];
		skyDesc.fragmentFunction = [skyLib newFunctionWithName:@"skyFragment"];
		skyDesc.colorAttachments[0].pixelFormat = kSceneColorFormat; // HDR target
		skyDesc.depthAttachmentPixelFormat      = kDepthFormat;      // pass has depth
		id<MTLRenderPipelineState> skyPso = [device newRenderPipelineStateWithDescriptor:skyDesc error:&skyError];
		if (!skyPso)
			throw std::runtime_error(std::string("MetalRenderer: sky pipeline creation failed: ")
				+ (skyError ? [[skyError localizedDescription] UTF8String] : "unknown"));
		m_skyPipeline = (void*)CFBridgingRetain(skyPso);

		// ── Cloud pre-pass pipeline (quarter-res clouds-only → RGBA16F (L,T), no depth) ──
		MTLRenderPipelineDescriptor* cloudDesc = [[MTLRenderPipelineDescriptor alloc] init];
		cloudDesc.vertexFunction   = [skyLib newFunctionWithName:@"skyVertex"];
		cloudDesc.fragmentFunction = [skyLib newFunctionWithName:@"cloudFragment"];
		cloudDesc.colorAttachments[0].pixelFormat = kSceneColorFormat; // RGBA16F (rgb=L, a=T)
		id<MTLRenderPipelineState> cloudPso = [device newRenderPipelineStateWithDescriptor:cloudDesc error:&skyError];
		if (!cloudPso)
			throw std::runtime_error(std::string("MetalRenderer: cloud pipeline creation failed: ")
				+ (skyError ? [[skyError localizedDescription] UTF8String] : "unknown"));
		m_cloudPipeline = (void*)CFBridgingRetain(cloudPso);

		// ── Cloud-shadow pipeline (world-region cloud transmittance → R8, no depth) ──
		MTLRenderPipelineDescriptor* csDesc = [[MTLRenderPipelineDescriptor alloc] init];
		csDesc.vertexFunction   = [skyLib newFunctionWithName:@"skyVertex"];
		csDesc.fragmentFunction = [skyLib newFunctionWithName:@"cloudShadowFragment"];
		csDesc.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
		id<MTLRenderPipelineState> csPso = [device newRenderPipelineStateWithDescriptor:csDesc error:&skyError];
		if (!csPso)
			throw std::runtime_error(std::string("MetalRenderer: cloud-shadow pipeline creation failed: ")
				+ (skyError ? [[skyError localizedDescription] UTF8String] : "unknown"));
		m_cloudShadowPipeline = (void*)CFBridgingRetain(csPso);

		MTLDepthStencilDescriptor* depthDesc = [[MTLDepthStencilDescriptor alloc] init];
		depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
		depthDesc.depthWriteEnabled    = YES;
		m_sceneDepthState = (void*)CFBridgingRetain([device newDepthStencilStateWithDescriptor:depthDesc]);

		// Overlay (ImGui) draws on top of everything — no depth test/write.
		depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
		depthDesc.depthWriteEnabled    = NO;
		m_noDepthState = (void*)CFBridgingRetain([device newDepthStencilStateWithDescriptor:depthDesc]);

		// Sky drawn LAST: depth-test == far (LessEqual vs the z=1 fullscreen tri),
		// no write — the sky shader only runs on the background pixels the scene
		// didn't cover, not behind solid geometry.
		depthDesc.depthCompareFunction = MTLCompareFunctionLessEqual;
		depthDesc.depthWriteEnabled    = NO;
		m_skyDepthState = (void*)CFBridgingRetain([device newDepthStencilStateWithDescriptor:depthDesc]);

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
		sampDesc.mipFilter = MTLSamplerMipFilterLinear; // use baked mip chains (else level 0 only)
		m_linearSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:sampDesc]);

		// 3D noise volume the sky's starFbm3/worleyFbm sample (clouds + nebula), built
		// once on the CPU. RG16Unorm (R=value noise, G=Worley billows) + linear +
		// repeat so it tiles seamlessly.
		constexpr int kNoiseN = 256;
		const std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kNoiseN);
		MTLTextureDescriptor* noiseDesc = [[MTLTextureDescriptor alloc] init];
		noiseDesc.textureType = MTLTextureType3D;
		noiseDesc.pixelFormat = MTLPixelFormatRG16Unorm;
		noiseDesc.width = kNoiseN; noiseDesc.height = kNoiseN; noiseDesc.depth = kNoiseN;
		noiseDesc.usage = MTLTextureUsageShaderRead;
		noiseDesc.storageMode = MTLStorageModeShared;
		id<MTLTexture> noiseTex = [device newTextureWithDescriptor:noiseDesc];
		[noiseTex replaceRegion:MTLRegionMake3D(0, 0, 0, kNoiseN, kNoiseN, kNoiseN)
		            mipmapLevel:0
		                  slice:0
		              withBytes:noise.data()
		            bytesPerRow:kNoiseN * 2 * sizeof(uint16_t)
		          bytesPerImage:kNoiseN * kNoiseN * 2 * sizeof(uint16_t)];
		m_noiseTexture = (void*)CFBridgingRetain(noiseTex);

		MTLSamplerDescriptor* noiseSampDesc = [[MTLSamplerDescriptor alloc] init];
		noiseSampDesc.minFilter = MTLSamplerMinMagFilterLinear;
		noiseSampDesc.magFilter = MTLSamplerMinMagFilterLinear;
		noiseSampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
		noiseSampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
		noiseSampDesc.rAddressMode = MTLSamplerAddressModeRepeat;
		m_noiseSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:noiseSampDesc]);

		// UI font atlas (R8): the shared baked ProggyClean atlas glyph quads sample.
		{
			const HE::BakedUIFont& uiFont = HE::sharedUIFont();
			if (uiFont.ok)
			{
				MTLTextureDescriptor* fontDesc = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
					                              width:HE::BakedUIFont::kWidth
					                             height:HE::BakedUIFont::kHeight
					                          mipmapped:NO];
				fontDesc.usage       = MTLTextureUsageShaderRead;
				fontDesc.storageMode = MTLStorageModeShared;
				id<MTLTexture> fontTex = [device newTextureWithDescriptor:fontDesc];
				[fontTex replaceRegion:MTLRegionMake2D(0, 0, HE::BakedUIFont::kWidth,
				                                       HE::BakedUIFont::kHeight)
				           mipmapLevel:0
				             withBytes:uiFont.pixels.data()
				           bytesPerRow:HE::BakedUIFont::kWidth];
				m_uiFontTexture = (void*)CFBridgingRetain(fontTex);
			}
		}

		// Empty image-based-ambient cubemap (RGBA32F); filled per frame from the
		// analytic skyColor (HE::SkyColorCPU) when the sun direction changes. The
		// existing clamp+linear sampler (m_linearSampler) samples it.
		MTLTextureDescriptor* envDesc = [MTLTextureDescriptor
			textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float size:128 mipmapped:NO];
		envDesc.usage = MTLTextureUsageShaderRead;
		envDesc.storageMode = MTLStorageModeShared;
		m_skyEnvCube = (void*)CFBridgingRetain([device newTextureWithDescriptor:envDesc]);

		// ── SSAO pipelines (position pre-pass + occlusion + blur) ───────────
		NSError* ssError = nil;
		id<MTLLibrary> ssLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kSSAOMSL] options:nil error:&ssError];
		if (!ssLib)
			throw std::runtime_error(std::string("MetalRenderer: SSAO shader compile failed: ")
				+ (ssError ? [[ssError localizedDescription] UTF8String] : "unknown"));

		MTLRenderPipelineDescriptor* posDesc = [[MTLRenderPipelineDescriptor alloc] init];
		posDesc.vertexFunction   = [ssLib newFunctionWithName:@"ssaoPosVertex"];
		posDesc.fragmentFunction = [ssLib newFunctionWithName:@"ssaoPosFragment"];
		posDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float; // view position
		posDesc.depthAttachmentPixelFormat      = kDepthFormat;
		id<MTLRenderPipelineState> posPso = [device newRenderPipelineStateWithDescriptor:posDesc error:&ssError];
		if (!posPso)
			throw std::runtime_error(std::string("MetalRenderer: SSAO pos pipeline failed: ")
				+ (ssError ? [[ssError localizedDescription] UTF8String] : "unknown"));
		m_ssaoPosPipeline = (void*)CFBridgingRetain(posPso);

		// FORWARD-reflections MRT variant of the pre-pass (view-pos + oct
		// normal/rough + NDC depth) — used instead of the plain pre-pass when
		// the forward path runs SSR / GI reflections.
		//
		// Cross-compiled from the SHARED library rather than from kSSAOMSL
		// above: every backend porting SSR needs the identical encoding, and
		// four hand-kept copies of that contract drift
		// (docs/ssr-cross-backend-plan.md §3.2 way (a)). The vertex is the SSBO
		// pull variant, pinned so the mesh buffer stays at [[buffer(0)]] and the
		// matrices at [[buffer(1)]] — the exact bind points the pre-pass encoder
		// already issues, so EncodeSSAO is untouched by this.
		//
		// A failure here is NOT fatal: the MRT branch checks m_reflPosPipeline
		// and falls back to the plain view-position pre-pass, which costs the
		// forward path its reflections and nothing else. (The embedded-MSL
		// version threw, but it could not fail for a reason the shipping build
		// would ever see; a cross-compile can.)
#if defined(HE_HAVE_SHADERC)
		{
			using LibBackend = HE::MaterialShaderLibrary::Backend;
			const auto& rpv = m_matShaderLib.reflPrepassVertex(LibBackend::Metal);
			const auto& rpf = m_matShaderLib.reflPrepassFragment(LibBackend::Metal);
			if (!(rpv.ok && rpf.ok))
				HE_LOG_ERROR(RHI, "%s",
					(std::string("MetalRenderer: refl pre-pass cross-compile failed\n")
					 + rpv.log + rpf.log).c_str());
			else
			{
				NSError* rpErr = nil;
				id<MTLLibrary> rpvLib = [device newLibraryWithSource:
					[NSString stringWithUTF8String:rpv.source.c_str()] options:nil error:&rpErr];
				id<MTLLibrary> rpfLib = rpErr ? nil : [device newLibraryWithSource:
					[NSString stringWithUTF8String:rpf.source.c_str()] options:nil error:&rpErr];
				if (rpvLib && rpfLib)
				{
					MTLRenderPipelineDescriptor* rpDesc = [[MTLRenderPipelineDescriptor alloc] init];
					rpDesc.vertexFunction   = [rpvLib newFunctionWithName:@"main0"];
					rpDesc.fragmentFunction = [rpfLib newFunctionWithName:@"main0"];
					rpDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float; // view position
					rpDesc.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float; // oct normal + rough
					rpDesc.colorAttachments[2].pixelFormat = MTLPixelFormatR32Float;    // NDC depth
					rpDesc.depthAttachmentPixelFormat      = kDepthFormat;
					id<MTLRenderPipelineState> rpPso =
						[device newRenderPipelineStateWithDescriptor:rpDesc error:&rpErr];
					if (rpPso) m_reflPosPipeline = (void*)CFBridgingRetain(rpPso);
				}
				if (!m_reflPosPipeline)
					HE_LOG_ERROR(RHI, "%s",
						(std::string("MetalRenderer: refl pos pipeline failed: ")
						 + (rpErr ? rpErr.localizedDescription.UTF8String : "unknown")).c_str());
			}
		}
#endif

		// Deferred P5 variant: view-pos reconstructed from the G-buffer depth by a
		// fullscreen draw — no depth attachment, no geometry.
		MTLRenderPipelineDescriptor* dpDesc = [[MTLRenderPipelineDescriptor alloc] init];
		dpDesc.vertexFunction   = [ssLib newFunctionWithName:@"ssaoVertex"];
		dpDesc.fragmentFunction = [ssLib newFunctionWithName:@"ssaoDepthPosFragment"];
		dpDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
		id<MTLRenderPipelineState> dpPso = [device newRenderPipelineStateWithDescriptor:dpDesc error:&ssError];
		if (!dpPso)
			throw std::runtime_error(std::string("MetalRenderer: SSAO depth-pos pipeline failed: ")
				+ (ssError ? [[ssError localizedDescription] UTF8String] : "unknown"));
		m_ssaoDepthPosPipeline = (void*)CFBridgingRetain(dpPso);

		MTLRenderPipelineDescriptor* occDesc = [[MTLRenderPipelineDescriptor alloc] init];
		occDesc.vertexFunction   = [ssLib newFunctionWithName:@"ssaoVertex"];
		occDesc.fragmentFunction = [ssLib newFunctionWithName:@"ssaoFragment"];
		occDesc.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
		id<MTLRenderPipelineState> occPso = [device newRenderPipelineStateWithDescriptor:occDesc error:&ssError];
		if (!occPso)
			throw std::runtime_error(std::string("MetalRenderer: SSAO pipeline failed: ")
				+ (ssError ? [[ssError localizedDescription] UTF8String] : "unknown"));
		m_ssaoPipeline = (void*)CFBridgingRetain(occPso);

		MTLRenderPipelineDescriptor* sblDesc = [[MTLRenderPipelineDescriptor alloc] init];
		sblDesc.vertexFunction   = [ssLib newFunctionWithName:@"ssaoVertex"];
		sblDesc.fragmentFunction = [ssLib newFunctionWithName:@"ssaoBlurFragment"];
		sblDesc.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
		id<MTLRenderPipelineState> sblPso = [device newRenderPipelineStateWithDescriptor:sblDesc error:&ssError];
		if (!sblPso)
			throw std::runtime_error(std::string("MetalRenderer: SSAO blur pipeline failed: ")
				+ (ssError ? [[ssError localizedDescription] UTF8String] : "unknown"));
		m_ssaoBlurPipeline = (void*)CFBridgingRetain(sblPso);

		// Samplers: nearest+clamp for the position buffer, nearest+repeat for noise.
		MTLSamplerDescriptor* ptDesc = [[MTLSamplerDescriptor alloc] init];
		ptDesc.minFilter = MTLSamplerMinMagFilterNearest;
		ptDesc.magFilter = MTLSamplerMinMagFilterNearest;
		ptDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
		ptDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
		m_ssaoPointSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:ptDesc]);
		MTLSamplerDescriptor* nsDesc = [[MTLSamplerDescriptor alloc] init];
		nsDesc.minFilter = MTLSamplerMinMagFilterNearest;
		nsDesc.magFilter = MTLSamplerMinMagFilterNearest;
		nsDesc.sAddressMode = MTLSamplerAddressModeRepeat;
		nsDesc.tAddressMode = MTLSamplerAddressModeRepeat;
		m_ssaoNoiseSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:nsDesc]);

		// 4×4 rotation-noise texture (RGBA32F so the values match GL's bit-for-bit).
		const std::vector<glm::vec3> ssaoNoise = HE::BuildSSAONoise(16);
		float ssaoNoisePx[16 * 4];
		for (int i = 0; i < 16; ++i)
		{ ssaoNoisePx[i*4+0] = ssaoNoise[i].x; ssaoNoisePx[i*4+1] = ssaoNoise[i].y; ssaoNoisePx[i*4+2] = 0.0f; ssaoNoisePx[i*4+3] = 0.0f; }
		MTLTextureDescriptor* nDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
		nDesc.usage = MTLTextureUsageShaderRead;
		nDesc.storageMode = MTLStorageModeShared;
		id<MTLTexture> ssaoNoiseTex = [device newTextureWithDescriptor:nDesc];
		[ssaoNoiseTex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0
		                  withBytes:ssaoNoisePx bytesPerRow:4 * 4 * sizeof(float)];
		m_ssaoNoiseTex = (void*)CFBridgingRetain(ssaoNoiseTex);
	}
}

void MetalRenderer::EnsureShadowResources()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		// Cascaded shadow map: a depth-texture ARRAY (one layer per cascade), sampled
		// by the scene pass. Each cascade renders into its own layer.
		MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
		td.textureType = MTLTextureType2DArray;
		td.pixelFormat = kDepthFormat;
		td.width       = (NSUInteger)m_shadowSize;
		td.height      = (NSUInteger)m_shadowSize;
		td.arrayLength = (NSUInteger)kCsmCascades;
		td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		m_shadowDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:td]);

		// Local (point/spot) shadow atlas: same depth-array pattern, 16 layers
		// (spot = 1 layer, point = 6 cube-face layers), lower per-view resolution.
		MTLTextureDescriptor* ltd = [[MTLTextureDescriptor alloc] init];
		ltd.textureType = MTLTextureType2DArray;
		ltd.pixelFormat = kDepthFormat;
		ltd.width       = (NSUInteger)m_localShadowSize;
		ltd.height      = (NSUInteger)m_localShadowSize;
		ltd.arrayLength = (NSUInteger)ShadowData::kMaxLocalShadowLayers;
		ltd.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		ltd.storageMode = MTLStorageModePrivate;
		m_localShadowTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:ltd]);

		// Depth-only pipeline (no color attachment, depth attachment only).
		NSError* error = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:injectSkyMSL(kUnlitMSL).c_str()] options:nil error:&error];
		if (!lib) { HE_LOG_ERROR(RHI, "%s", "MetalRenderer: shadow shader compile failed"); return; }
		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction             = [lib newFunctionWithName:@"vertexShadow"];
		desc.fragmentFunction           = nil; // depth only
		desc.depthAttachmentPixelFormat = kDepthFormat;
		id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (pso) m_shadowPipeline = (void*)CFBridgingRetain(pso);
		else     HE_LOG_ERROR(RHI, "%s", "MetalRenderer: shadow pipeline creation failed");
	}
}

void MetalRenderer::EncodeShadowMap(void* cmdBufPtr, float aspect)
{
	if (!m_world || !m_shadowPipeline || !m_shadowDepthTex) return;

	// Re-extract to get the cascade light matrices + caster geometry. CRITICAL for
	// CSM: this MUST use the SAME day-night state and the SAME aspect ratio as the
	// scene pass's extract (EncodeScene), because the cascade fit depends on both.
	// If they differ, the depth maps are rendered with different cascade matrices
	// than the shader samples with → shadows slide with the camera (swimming).
	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);
	// CSM needs a directional light; the local (point/spot) atlas is independent
	// of it — night scenes with only shadow-casting point lights still render.
	const bool wantCsm   = m_renderWorld.shadow.enabled;
	const bool wantLocal = m_renderWorld.shadow.localLayerCount > 0 && m_localShadowTex;
	if ((!wantCsm && !wantLocal) || m_renderWorld.objects.empty()) return;
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);
	const int cascades    = wantCsm ? std::clamp(m_renderWorld.shadow.cascadeCount, 1, kCsmCascades) : 0;
	const int localLayers = wantLocal
		? std::clamp(m_renderWorld.shadow.localLayerCount, 0, ShadowData::kMaxLocalShadowLayers) : 0;
	const int totalViews  = cascades + localLayers;

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		// One profiler bucket for the whole shadow pass (cascades + local layers):
		// start timer on the first encoder, end on the last.
		const uint32_t shBase = ftBeginMulti("Shadow");
		int viewIdx = 0;

		// Depth-only render of every shadow caster into one layer of `target`,
		// shared by the CSM cascades and the local (point/spot) shadow views.
		// `skipEntity` keeps ONE entity's geometry out of this layer: the entity the
		// local light itself sits on. Without it a light authored onto a mesh entity
		// renders that mesh at z≈0 into its own depth map and shadows itself out
		// completely. kNoOwnerEntity (every cascade) skips nothing.
		auto encodeDepthLayer = [&](void* target, int layer, int size, const glm::mat4& viewProj,
		                            uint32_t skipEntity)
		{
			m_culler.cull(m_renderWorld, viewProj, m_visible);
			m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
			const glm::mat4 lightClip = HE::kMetalClipFix * viewProj;

			MTLRenderPassDescriptor* sp = [MTLRenderPassDescriptor renderPassDescriptor];
			sp.depthAttachment.texture       = (__bridge id<MTLTexture>)target;
			sp.depthAttachment.slice         = (NSUInteger)layer;
			sp.depthAttachment.loadAction    = MTLLoadActionClear;
			sp.depthAttachment.storeAction   = MTLStoreActionStore;
			sp.depthAttachment.clearDepth    = 1.0;
			if (viewIdx == 0)              ftAttachStart((__bridge void*)sp, shBase);
			if (viewIdx == totalViews - 1) ftAttachEnd  ((__bridge void*)sp, shBase);
			++viewIdx;

			id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:sp];
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_shadowPipeline];
			[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
			[enc setViewport:(MTLViewport){ 0.0, 0.0, (double)size, (double)size, 0.0, 1.0 }];

			HE::UUID shMeshId{}; const GpuMesh* shMesh = nullptr; bool shMeshValid = false;
			for (uint32_t idx : m_sortedIndices)
			{
				const RenderObject& obj = m_renderWorld.objects[idx];
				if (!obj.castsShadow) continue; // billboards (precip/particles) cast no shadow
				if (obj.entityId == skipEntity) continue; // the light's own mesh
				UnlitUniforms u;
				u.mvp = lightClip * obj.transform;

				if (!shMeshValid || obj.meshAssetId != shMeshId)
				{
					shMesh      = ResolveMesh(obj.meshAssetId);
					shMeshId    = obj.meshAssetId; shMeshValid = true;
				}
				const GpuMesh* drawMesh = shMesh ? shMesh : ResolveMesh(HE::kDefaultCubeMeshId);
				if (!drawMesh) continue;
				id<MTLBuffer> vbuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
				id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
				NSUInteger    ic   = (NSUInteger)drawMesh->indexCount;
				[enc setVertexBuffer:vbuf offset:0 atIndex:0];
				[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
				[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                indexCount:ic
				                 indexType:MTLIndexTypeUInt32
				               indexBuffer:ibuf
				         indexBufferOffset:0];
			}
			[enc endEncoding];
		};

		for (int c = 0; c < cascades; ++c)
			encodeDepthLayer(m_shadowDepthTex, c, m_shadowSize,
			                 m_renderWorld.shadow.cascadeViewProj[c], kNoOwnerEntity);
		for (int v = 0; v < localLayers; ++v)
			encodeDepthLayer(m_localShadowTex, v, m_localShadowSize,
			                 m_renderWorld.shadow.localViewProj[v],
			                 m_renderWorld.shadow.localOwnerEntity[v]);
	}
}
// ─── Global Illumination (ray-traced DDGI) — acceleration structures ──────────

void MetalRenderer::EnsureRaytracingSupport()
{
	if (m_giRaytracingChecked) return;
	m_giRaytracingChecked = true;

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return;

	// HARDWARE ray tracing (intersection_query, MSL 2.4 / macOS 12 +
	// device.supportsRaytracing) is the preferred path. Devices/OSes without it
	// fall back to SOFTWARE ray tracing: the CPU-built HE::GiBvh in plain
	// MTLBuffers, traversed by base compute kernels (same approach as the GL
	// 4.3 port) — so GI is supported on EVERY Metal device, only the kernels
	// differ. HE_GI_FORCE_SW=1 forces the software path on RT hardware, which
	// is how the SW kernels get real-hardware verification on this machine.
	if (@available(macOS 12.0, *))
		m_giHwRt = device.supportsRaytracing;
	if (const char* force = std::getenv("HE_GI_FORCE_SW"); force && *force && *force != '0')
	{
		m_giHwRt = false;
		HE_LOG_INFO(RHI, "%s", "MetalRenderer: HE_GI_FORCE_SW set — software GI path forced");
	}
	m_giSupported = true; // compute is base Metal; SW path covers the no-HW-RT case

	HE_LOG_INFO(RHI, "%s",
		(std::string("MetalRenderer: ray-traced GI supported (")
		 + (m_giHwRt ? "hardware" : "software") + " ray tracing)").c_str());

	// MetalFX (A5): asked ONCE, here — the answer cannot change while the app
	// runs, and asking per frame would put a framework call in the hot path.
	//
	// OPT-IN (HE_METALFX=1) on purpose. The path is fully wired and the device
	// reports support, but on this machine the scaler writes its result 1:1 into
	// the corner of the output instead of upscaling — with the sizes verified
	// correct in the log below (858x482 -> 1280x720). Until that is understood,
	// reporting "supported" would hand users a visibly broken mode; reporting
	// false makes the AA combo fall back to our own TAA, which works. The two
	// open suspects are the descriptor's input-content properties
	// (inputContentPropertiesEnabled + min/max scale) and this being a 27-beta
	// behaviour — the same kind of beta trap MTLBinaryArchive turned out to be.
	bool mfxDevice = false;
#if HE_HAS_METALFX
	if (@available(macOS 13.0, *))
		mfxDevice = [MTLFXTemporalScalerDescriptor supportsDevice:device];
#endif
	const char* mfxOptIn = std::getenv("HE_METALFX");
	m_mfxSupported = mfxDevice && mfxOptIn && *mfxOptIn && *mfxOptIn != '0';
	HE_LOG_INFO(RHI, "%s", mfxDevice
		? (m_mfxSupported
			? "MetalRenderer: MetalFX temporal scaling ENABLED (HE_METALFX)"
			: "MetalRenderer: MetalFX present but off — set HE_METALFX=1 to try it (see A5)")
		: "MetalRenderer: MetalFX temporal scaling unavailable — the TAA mode covers it");
}

void* MetalRenderer::BuildBLAS(const GpuMesh& mesh)
{
	if (!mesh.vertexBuf || !mesh.indexBuf || mesh.indexCount <= 0) return nullptr;

	void* result = nullptr;
	if (@available(macOS 12.0, *))
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		MTLAccelerationStructureTriangleGeometryDescriptor* geom =
			[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		geom.vertexBuffer       = (__bridge id<MTLBuffer>)mesh.vertexBuf;
		geom.vertexBufferOffset = 0;
		geom.vertexStride       = sizeof(float) * 8; // interleaved pos3+normal3+uv2, position at offset 0
		geom.vertexFormat       = MTLAttributeFormatFloat3;
		geom.indexBuffer        = (__bridge id<MTLBuffer>)mesh.indexBuf;
		geom.indexBufferOffset  = 0;
		geom.indexType          = MTLIndexTypeUInt32;
		geom.triangleCount      = (NSUInteger)(mesh.indexCount / 3);
		geom.opaque              = YES; // casters are opaque occluders for shadow/GI rays (no alpha test yet)

		MTLPrimitiveAccelerationStructureDescriptor* accelDesc =
			[MTLPrimitiveAccelerationStructureDescriptor descriptor];
		accelDesc.geometryDescriptors = @[ geom ];

		MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:accelDesc];
		id<MTLAccelerationStructure> blas =
			[device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
		if (blas)
		{
			id<MTLBuffer> scratch = [device newBufferWithLength:std::max((NSUInteger)1, sizes.buildScratchBufferSize)
			                                              options:MTLResourceStorageModePrivate];
			id<MTLCommandQueue>  queue  = (__bridge id<MTLCommandQueue>)m_commandQueue;
			id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
			id<MTLAccelerationStructureCommandEncoder> enc = [cmdBuf accelerationStructureCommandEncoder];
			[enc buildAccelerationStructure:blas descriptor:accelDesc
			                  scratchBuffer:scratch scratchBufferOffset:0];
			[enc endEncoding];
			[cmdBuf commit];
			// One-shot, lazy (first-sight-only) build — a small hitch on the frame a
			// new mesh first participates in GI is acceptable; waiting here keeps the
			// BLAS fully built before any TLAS references it, no extra bookkeeping.
			[cmdBuf waitUntilCompleted];
			result = (void*)CFBridgingRetain(blas);
		}
	}
	return result;
}

// ─── Software-RT acceleration build (no-HW-RT / HE_GI_FORCE_SW path) ─────────
// CPU BVH per mesh (HE::buildGiBvh — the traversal the kGISWMSL kernels mirror
// is unit-tested in tests/test_gi_bvh.cpp), concatenated into two shared
// MTLBuffers; instances are a flat per-frame buffer. Mirrors the GL 4.3 port's
// BuildGiBlas/UpdateGiAccel structure.

// Effective flat albedo for a GI instance (probe bounce tint + reflection hit
// shading). The extractor leaves RenderObject::baseColor at its white default —
// material colour is resolved at DRAW time from the MaterialComponent's asset —
// so resolve it here the same way, or every ray hit reads white and the GI
// bounce/reflections lose the object's colour entirely.
//
// The resolution itself is HE::giInstanceSurface (shared with the GL 4.3 port):
// this used to be a hand-kept second copy, and the two drifting apart showed up
// as the same scene reflecting different colours per backend. Thin adapter only.
static void giInstanceShading(const RenderObject& obj, const ContentManager* cm,
                              glm::vec3& albedoOut, glm::vec3& emissiveOut,
                              float& metallicOut, float& roughnessOut)
{
	const HE::GiInstanceSurface s = HE::giInstanceSurface(obj, cm);
	albedoOut    = s.albedo;
	emissiveOut  = s.emissive;
	metallicOut  = s.metallic;
	roughnessOut = s.roughness;
}

MetalRenderer::GISwBlasRange MetalRenderer::BuildGISwBlas(const HE::UUID& meshId)
{
	GISwBlasRange range;
	if (!m_contentManager) return range;
	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(meshId);
	if (!asset || asset->indices.empty()) return range;

	// Same two layouts ResolveMesh uploads: cooked interleaved 8-float
	// (position at offset 0) or loose tightly-packed 3-float positions.
	HE::GiBvh bvh;
	if (asset->cooked && !asset->interleaved.empty())
		bvh = HE::buildGiBvh(asset->interleaved.data(), asset->vertexCount, 8,
		                     asset->indices.data(), asset->indices.size());
	else if (!asset->vertices.empty())
		bvh = HE::buildGiBvh(asset->vertices.data(), asset->vertices.size() / 3, 3,
		                     asset->indices.data(), asset->indices.size());
	if (!bvh.valid()) return range;

	range.nodeOffset = static_cast<int32_t>(m_giSwNodesCpu.size());
	range.triOffset  = static_cast<int32_t>(m_giSwTrisCpu.size());
	range.valid      = true;
	m_giSwNodesCpu.insert(m_giSwNodesCpu.end(), bvh.nodes.begin(), bvh.nodes.end());
	m_giSwTrisCpu.insert(m_giSwTrisCpu.end(), bvh.triangles.begin(), bvh.triangles.end());
	m_giSwBlasDirty = true;
	return range;
}

void MetalRenderer::EncodeGISwAccelBuild()
{
	// Same caster filter as the HW TLAS: castsShadow, unculled.
	std::vector<GISwInstanceCPU> instances;
	instances.reserve(m_renderWorld.objects.size());
	auto resolveSwRange = [&](const HE::UUID& id) -> GISwBlasRange
	{
		auto it = m_giSwBlasCache.find(id);
		if (it == m_giSwBlasCache.end())
			it = m_giSwBlasCache.emplace(id, BuildGISwBlas(id)).first;
		return it->second;
	};
	for (RenderObject& obj : m_renderWorld.objects)
	{
		if (!obj.castsShadow) continue;
		// Default-cube fallback — must match the draw loops (see the HW path).
		GISwBlasRange range = resolveSwRange(obj.meshAssetId);
		if (!range.valid) range = resolveSwRange(HE::kDefaultCubeMeshId);
		if (!range.valid) continue;
		GISwInstanceCPU inst;
		inst.invTransform = glm::inverse(obj.transform);
		glm::vec3 albedo, emissive;
		float metallic, roughness;
		giInstanceShading(obj, m_contentManager, albedo, emissive, metallic, roughness);
		inst.baseColor    = glm::vec4(albedo, metallic);   // w = metallic (bounce loop)
		inst.emissive     = glm::vec4(emissive, roughness); // w = roughness
		inst.nodeOffset   = range.nodeOffset;
		inst.triOffset    = range.triOffset;
		inst.landIndex    = obj.landscapeIndex;             // paint sampling at the hit
		instances.push_back(inst);
	}
	m_giSwInstanceCount = static_cast<int>(instances.size());
	if (m_giSwInstanceCount == 0) return;

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (m_giSwBlasDirty && !m_giSwNodesCpu.empty())
		{
			RetireGIObject(m_giSwNodeBuf); m_giSwNodeBuf = nullptr;
			RetireGIObject(m_giSwTriBuf);  m_giSwTriBuf  = nullptr;
			id<MTLBuffer> nb = [device newBufferWithBytes:m_giSwNodesCpu.data()
			                                       length:m_giSwNodesCpu.size() * sizeof(HE::GiBvhNode)
			                                      options:MTLResourceStorageModeShared];
			id<MTLBuffer> tb = [device newBufferWithBytes:m_giSwTrisCpu.data()
			                                       length:m_giSwTrisCpu.size() * sizeof(HE::GiBvhTriangle)
			                                      options:MTLResourceStorageModeShared];
			if (nb) m_giSwNodeBuf = (void*)CFBridgingRetain(nb);
			if (tb) m_giSwTriBuf  = (void*)CFBridgingRetain(tb);
			m_giSwBlasDirty = false;
		}
		// Instance buffer is rebuilt FRESH every frame (same in-flight-GPU-race
		// reasoning as the HW TLAS instance buffer — see the member comment).
		RetireGIObject(m_giSwInstanceBuf); m_giSwInstanceBuf = nullptr;
		id<MTLBuffer> ib = [device newBufferWithBytes:instances.data()
		                                       length:instances.size() * sizeof(GISwInstanceCPU)
		                                      options:MTLResourceStorageModeShared];
		if (ib) m_giSwInstanceBuf = (void*)CFBridgingRetain(ib);
	}
}

// ─── Painted-landscape table for the reflection kernels ──────────────────────
// One entry per terrain the extractor found paintable (a layer-blend material
// whose layers folded, plus a weightmap), plus that terrain's weightmap texture
// for the kernel's texture array. Without this a landscape hit can only be one
// flat colour for the whole terrain — a red ridge on a green hillside mirrors as
// the average of the two, which is visibly not what the terrain looks like.
// Rebuilt per frame: it is a handful of 144-byte entries, and the weightmap
// TEXTURES come from the same UUID-keyed cache the raster path uses (uploaded
// once, re-uploaded only when the paint changes).
int MetalRenderer::BuildGILandscapeTable(std::vector<void*>& outWeightTex)
{
	outWeightTex.clear();
	RetireGIObject(m_giLandBuf); m_giLandBuf = nullptr;
	if (m_renderWorld.landscapes.empty() || !m_device) return 0;

	std::vector<GILandGpu> gpu;
	gpu.reserve(m_renderWorld.landscapes.size());
	for (const HE::GiLandscape& ls : m_renderWorld.landscapes)
	{
		if (static_cast<int>(gpu.size()) >= HE::kGiMaxLandscapes) break;
		// No resident weightmap (still streaming, or unpainted) → skip the entry;
		// those chunks keep their flat per-instance colour, which for an unpainted
		// terrain is layer 0 and therefore already right.
		void* wm = ResolveGraphTexture(ls.weightmapId, {});
		if (!wm) continue;
		GILandGpu g;
		g.worldToLocal = ls.worldToLocal;
		g.cfg = glm::vec4(ls.invSize.x, ls.invSize.y, ls.uvTiling,
		                  static_cast<float>(ls.layerCount));
		for (int i = 0; i < 4; ++i) g.layer[i] = ls.layerColor[i];
		gpu.push_back(g);
		outWeightTex.push_back(wm);
	}
	if (gpu.empty()) return 0;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLBuffer> buf = [device newBufferWithBytes:gpu.data()
	                                        length:gpu.size() * sizeof(GILandGpu)
	                                       options:MTLResourceStorageModeShared];
	if (!buf) return 0;
	m_giLandBuf = (void*)CFBridgingRetain(buf);
	return static_cast<int>(gpu.size());
}

void MetalRenderer::EncodeGIAccelBuild(void* cmdBufPtr, float aspect)
{
	// GI reflections reuse the acceleration structures without the probe/
	// shadow passes, so the build also runs when ONLY they are enabled
	// (either path — the reflection kernel has HW and SW-BVH variants).
	const bool wantAccel = m_giSupported && (m_giEnabled || m_giReflEnabled);
	if (!wantAccel || !m_world)
	{
		// GI just turned off (or was never on this run): release any TLAS/instance
		// buffer left over from the last GI-active frame instead of holding them
		// until Shutdown(). No-op (RetireGIObject ignores null) once already clear.
		RetireGIObject(m_giTlas);              m_giTlas               = nullptr;
		RetireGIObject(m_giInstanceBuffer);    m_giInstanceBuffer     = nullptr;
		RetireGIObject(m_giInstanceColorBuffer); m_giInstanceColorBuffer = nullptr;
		RetireGIObject(m_giLandBuf);           m_giLandBuf            = nullptr;
		RetireGIObject(m_giInstanceLandBuf);   m_giInstanceLandBuf    = nullptr;
		RetireGIObject(m_giMeshPtrBuf);        m_giMeshPtrBuf         = nullptr;
		RetireGIObject(m_giInstanceMeshBuf);   m_giInstanceMeshBuf    = nullptr;
		m_giMeshResources.clear();
		m_giUniqueBlas.clear();
		RetireGIObject(m_giSwNodeBuf);     m_giSwNodeBuf     = nullptr;
		RetireGIObject(m_giSwTriBuf);      m_giSwTriBuf      = nullptr;
		RetireGIObject(m_giSwInstanceBuf); m_giSwInstanceBuf = nullptr;
		m_giSwInstanceCount = 0;
		m_giSwBlasDirty     = true; // node/tri buffers gone → re-upload on next GI-on
		return;
	}

	// Re-extract with the SAME day-night params EncodeShadowMap uses so the caster
	// set matches — each pass independently re-extracts, the established
	// convention in this file (see EncodeShadowMap/EncodeSSAO). The TLAS itself
	// doesn't care about the camera, but EncodeGIShadowRays REUSES this extraction's
	// m_renderWorld.camera to frustum-cull and rasterize its screen-space G-buffer,
	// which fragmentMain then samples at the SCENE pass's UVs. Extracting with a
	// wrong aspect (this used to pass 1.0) horizontally misaligns the whole shadow
	// mask against the main view — lit faces pick up the hard 0.0 the ray kernel
	// writes for light-back-facing pixels, and the error field sweeps across
	// objects as the camera rotates (the exact "shadows swim with the camera"
	// failure EncodeShadowMap's header comment warns about for CSM).
	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld, aspect, &m_editorCamera);

	RetireGIObject(m_giTlas);                m_giTlas                 = nullptr;
	RetireGIObject(m_giInstanceBuffer);      m_giInstanceBuffer       = nullptr;
	RetireGIObject(m_giInstanceColorBuffer); m_giInstanceColorBuffer  = nullptr;
	RetireGIObject(m_giMeshPtrBuf);          m_giMeshPtrBuf           = nullptr;
	RetireGIObject(m_giInstanceMeshBuf);     m_giInstanceMeshBuf      = nullptr;
	m_giMeshResources.clear();
	m_giUniqueBlas.clear();
	if (m_renderWorld.objects.empty()) return;

	// Software path (no HW inline ray tracing, or HE_GI_FORCE_SW): CPU BVH into
	// plain buffers, no command encoding needed — everything downstream is
	// shared with the HW path except the ray-dispatch kernels.
	if (!m_giHwRt)
	{
		EncodeGISwAccelBuild();
		return;
	}

	if (@available(macOS 12.0, *))
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

		// Same caster filter as EncodeShadowMap's cascade loop (castsShadow flag;
		// skinned objects are never in m_renderWorld.objects, so they're already
		// excluded — matching the existing "skinned meshes are shaded but never
		// cast shadows" behaviour).
		std::vector<id<MTLAccelerationStructure>> uniqueBlas;
		// Per-unique-BLAS vertex/index buffers, same order as uniqueBlas — the
		// reflection kernel's true-hit-normal fetch (P4) reads them through a
		// tier-2 argument buffer of raw GPU addresses.
		std::vector<std::pair<id<MTLBuffer>, id<MTLBuffer>>> uniqueMeshBufs;
		std::vector<uint32_t> instanceBlasIndex;
		std::vector<glm::mat4> instanceTransform;
		// Bounce tint + reflection shading: TWO float4 per instance
		// (albedo, emissive) — see giInstanceShading.
		std::vector<glm::vec4> instanceBaseColor;
		// Parallel to the instance array: index into m_giLandBuf, -1 = not a
		// landscape chunk. Kept OUT of instanceBaseColor so the hot 2-float4
		// stride stays what it is.
		std::vector<int32_t> instanceLandIndex;
		instanceBlasIndex.reserve(m_renderWorld.objects.size());
		instanceTransform.reserve(m_renderWorld.objects.size());
		instanceBaseColor.reserve(m_renderWorld.objects.size() * 2);
		instanceLandIndex.reserve(m_renderWorld.objects.size());

		for (RenderObject& obj : m_renderWorld.objects)
		{
			if (!obj.castsShadow) continue;
			// Same fallback the shadow-caster/G-buffer DRAW loops use: an entity
			// without a resolvable mesh asset renders as the default cube, so it
			// must occlude as one too — skipping it here made such objects
			// (plain cube entities) receive lighting but cast NOTHING.
			const GpuMesh* resolved = ResolveMesh(obj.meshAssetId);
			if (!resolved) resolved = ResolveMesh(HE::kDefaultCubeMeshId);
			if (!resolved) continue;
			GpuMesh& mesh = const_cast<GpuMesh&>(*resolved); // m_meshCache entry; safe, owned by this class
			if (!mesh.blas) mesh.blas = BuildBLAS(mesh);
			if (!mesh.blas) continue;

			id<MTLAccelerationStructure> blas = (__bridge id<MTLAccelerationStructure>)mesh.blas;
			int idx = -1;
			for (size_t u = 0; u < uniqueBlas.size(); ++u)
				if (uniqueBlas[u] == blas) { idx = (int)u; break; }
			if (idx < 0)
			{
				idx = (int)uniqueBlas.size();
				uniqueBlas.push_back(blas);
				uniqueMeshBufs.emplace_back((__bridge id<MTLBuffer>)mesh.vertexBuf,
				                            (__bridge id<MTLBuffer>)mesh.indexBuf);
			}

			instanceBlasIndex.push_back((uint32_t)idx);
			instanceTransform.push_back(obj.transform);
			// TWO float4 per instance — the kernels index instanceColors
			// [instId * 2 (+ 1)]: (albedo.rgb, metallic) + (emissive.rgb,
			// roughness). Metallic/roughness drive the bounce loop's mirror-ness.
			glm::vec3 albedo, emissive;
			float metallic, roughness;
			giInstanceShading(obj, m_contentManager, albedo, emissive, metallic, roughness);
			instanceBaseColor.push_back(glm::vec4(albedo, metallic));
			instanceBaseColor.push_back(glm::vec4(emissive, roughness));
			instanceLandIndex.push_back(obj.landscapeIndex); // paint sampling at the hit
		}
		if (uniqueBlas.empty()) return;

		const NSUInteger count = (NSUInteger)instanceBlasIndex.size();
		id<MTLBuffer> instanceBuf = [device
			newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) * count
			            options:MTLResourceStorageModeShared];
		auto* instances = (MTLAccelerationStructureInstanceDescriptor*)instanceBuf.contents;
		// Parallel per-instance shading buffer (2 float4 each), SAME index order —
		// see the header comment on m_giInstanceColorBuffer for why
		// get_committed_instance_id() reliably matches this array's position.
		id<MTLBuffer> colorBuf = [device newBufferWithBytes:instanceBaseColor.data()
		                                              length:sizeof(glm::vec4) * count * 2
		                                             options:MTLResourceStorageModeShared];
		id<MTLBuffer> landIdxBuf = [device newBufferWithBytes:instanceLandIndex.data()
		                                               length:sizeof(int32_t) * count
		                                              options:MTLResourceStorageModeShared];
		for (NSUInteger i = 0; i < count; ++i)
		{
			instances[i].accelerationStructureIndex        = instanceBlasIndex[i];
			instances[i].options                            = MTLAccelerationStructureInstanceOptionOpaque;
			instances[i].mask                               = 0xFF;
			instances[i].intersectionFunctionTableOffset    = 0;
			const glm::mat4& t = instanceTransform[i];
			// MTLPackedFloat4x3: 3 rows x 4 columns (Metal's instance-transform
			// convention), filled from our column-major glm::mat4.
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 4; ++c)
					instances[i].transformationMatrix.columns[c][r] = t[c][r];
		}

		NSMutableArray<id<MTLAccelerationStructure>>* accelArray =
			[NSMutableArray arrayWithCapacity:uniqueBlas.size()];
		for (id<MTLAccelerationStructure> b : uniqueBlas) [accelArray addObject:b];

		MTLInstanceAccelerationStructureDescriptor* tlasDesc =
			[MTLInstanceAccelerationStructureDescriptor descriptor];
		tlasDesc.instancedAccelerationStructures = accelArray;
		tlasDesc.instanceCount                   = count;
		tlasDesc.instanceDescriptorBuffer        = instanceBuf;
		tlasDesc.instanceDescriptorType          = MTLAccelerationStructureInstanceDescriptorTypeDefault;

		MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:tlasDesc];
		id<MTLAccelerationStructure> tlas =
			[device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
		if (!tlas) return;

		id<MTLBuffer> scratch = [device newBufferWithLength:std::max((NSUInteger)1, sizes.buildScratchBufferSize)
		                                              options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		id<MTLAccelerationStructureCommandEncoder> enc = [cmdBuf accelerationStructureCommandEncoder];
		// The TLAS build reads every referenced BLAS; Metal does not auto-track
		// residency through acceleration structures the way it does for directly
		// bound buffers, so each one must be explicitly declared used.
		for (id<MTLAccelerationStructure> b : uniqueBlas)
			[enc useResource:b usage:MTLResourceUsageRead];
		[enc buildAccelerationStructure:tlas descriptor:tlasDesc
		                  scratchBuffer:scratch scratchBufferOffset:0];
		[enc endEncoding];

		m_giTlas                = (void*)CFBridgingRetain(tlas);
		m_giInstanceBuffer      = (void*)CFBridgingRetain(instanceBuf);
		m_giInstanceColorBuffer = (void*)CFBridgingRetain(colorBuf);
		RetireGIObject(m_giInstanceLandBuf);
		m_giInstanceLandBuf     = landIdxBuf ? (void*)CFBridgingRetain(landIdxBuf) : nullptr;
		// Cache the unique-BLAS set for EncodeGIShadowRays/EncodeGIProbeUpdate — every
		// compute encoder that traces against m_giTlas must useResource: each of
		// these too (same residency requirement as the build encoder above).
		m_giUniqueBlas.clear();
		m_giUniqueBlas.reserve(uniqueBlas.size());
		for (id<MTLAccelerationStructure> b : uniqueBlas)
			m_giUniqueBlas.push_back((__bridge void*)b);

		// P4: mesh-pointer argument buffer for the reflection kernel's true
		// hit normals — raw gpuAddress values (tier-2 argument buffers are
		// plain GPU VAs; guaranteed on the Apple-Silicon devices m_giHwRt
		// implies). gpuAddress needs macOS 13; older OSes simply keep the
		// -rayDir fallback (m_giMeshPtrBuf stays null).
		if (@available(macOS 13.0, *))
		{
			struct MeshPtrGPU { uint64_t vtx; uint64_t idx; };
			std::vector<MeshPtrGPU> ptrs;
			ptrs.reserve(uniqueMeshBufs.size());
			bool complete = true;
			for (const auto& [vb, ib] : uniqueMeshBufs)
			{
				if (!vb || !ib) { complete = false; break; }
				ptrs.push_back({ vb.gpuAddress, ib.gpuAddress });
			}
			if (complete && !ptrs.empty())
			{
				id<MTLBuffer> pb = [device newBufferWithBytes:ptrs.data()
				                                       length:ptrs.size() * sizeof(MeshPtrGPU)
				                                      options:MTLResourceStorageModeShared];
				id<MTLBuffer> imb = [device newBufferWithBytes:instanceBlasIndex.data()
				                                        length:instanceBlasIndex.size() * sizeof(uint32_t)
				                                       options:MTLResourceStorageModeShared];
				if (pb && imb)
				{
					m_giMeshPtrBuf      = (void*)CFBridgingRetain(pb);
					m_giInstanceMeshBuf = (void*)CFBridgingRetain(imb);
					m_giMeshResources.reserve(uniqueMeshBufs.size() * 2);
					for (const auto& [vb, ib] : uniqueMeshBufs)
					{
						m_giMeshResources.push_back((__bridge void*)vb);
						m_giMeshResources.push_back((__bridge void*)ib);
					}
				}
			}
		}
	}
}

void MetalRenderer::EnsureGIShadowPipelines()
{
	if (m_giGBufPipeline) return; // already built
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* error = nil;
		// Raster stages compile on every device (no raytracing include) — shared
		// by the hardware and software ray paths.
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:kGIShadowRasterMSL] options:nil error:&error];
		if (!lib)
		{
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: GI shadow shader compile failed: ")
				 + (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
			return;
		}

		// G-buffer pre-pass: MRT (world pos + normal), own small depth target.
		MTLRenderPipelineDescriptor* gDesc = [[MTLRenderPipelineDescriptor alloc] init];
		gDesc.vertexFunction   = [lib newFunctionWithName:@"giGBufVertex"];
		gDesc.fragmentFunction = [lib newFunctionWithName:@"giGBufFragment"];
		gDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
		gDesc.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float;
		gDesc.depthAttachmentPixelFormat      = kDepthFormat;
		id<MTLRenderPipelineState> gPso = [device newRenderPipelineStateWithDescriptor:gDesc error:&error];
		if (gPso) m_giGBufPipeline = (void*)CFBridgingRetain(gPso);
		else      HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI G-buffer pipeline creation failed");

		// Ray dispatch kernel — HARDWARE (intersection_query, MSL 2.4 library) or
		// SOFTWARE (base compute traversal of the CPU BVH, kGISWMSL).
		if (m_giHwRt)
		{
			id<MTLLibrary> rayLib = [device newLibraryWithSource:
				[NSString stringWithUTF8String:kGIShadowMSL] options:nil error:&error];
			if (rayLib)
			{
				id<MTLFunction> rayFn = [rayLib newFunctionWithName:@"giShadowRay"];
				id<MTLComputePipelineState> rayPso = [device newComputePipelineStateWithFunction:rayFn error:&error];
				if (rayPso) m_giShadowRayPipeline = (void*)CFBridgingRetain(rayPso);
			}
			if (!m_giShadowRayPipeline)
				HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI HW shadow-ray pipeline creation failed");
		}
		else
		{
			id<MTLLibrary> swLib = [device newLibraryWithSource:
				[NSString stringWithUTF8String:kGISWMSL] options:nil error:&error];
			if (swLib)
			{
				id<MTLFunction> rayFn = [swLib newFunctionWithName:@"giShadowRaySw"];
				id<MTLComputePipelineState> rayPso = [device newComputePipelineStateWithFunction:rayFn error:&error];
				if (rayPso) m_giShadowRaySwPipeline = (void*)CFBridgingRetain(rayPso);
			}
			if (!m_giShadowRaySwPipeline)
				HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI SW shadow-ray pipeline creation failed");
		}

		// Temporal accumulation (fullscreen triangle). RGBA: rgb = world position
		// (for next frame's disocclusion check), a = the shadow scalar itself.
		MTLRenderPipelineDescriptor* tDesc = [[MTLRenderPipelineDescriptor alloc] init];
		tDesc.vertexFunction   = [lib newFunctionWithName:@"giFsVertex"];
		tDesc.fragmentFunction = [lib newFunctionWithName:@"giShadowTemporal"];
		tDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
		id<MTLRenderPipelineState> tPso = [device newRenderPipelineStateWithDescriptor:tDesc error:&error];
		if (tPso) m_giShadowTemporalPipeline = (void*)CFBridgingRetain(tPso);
		else      HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI shadow-temporal pipeline creation failed");

		// Spatial blur (fullscreen triangle, single R-channel output).
		MTLRenderPipelineDescriptor* bDesc = [[MTLRenderPipelineDescriptor alloc] init];
		bDesc.vertexFunction   = [lib newFunctionWithName:@"giFsVertex"];
		bDesc.fragmentFunction = [lib newFunctionWithName:@"giShadowBlur"];
		bDesc.colorAttachments[0].pixelFormat = MTLPixelFormatR16Float;
		id<MTLRenderPipelineState> bPso = [device newRenderPipelineStateWithDescriptor:bDesc error:&error];
		if (bPso) m_giShadowBlurPipeline = (void*)CFBridgingRetain(bPso);
		else      HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI shadow-blur pipeline creation failed");
	}
}

void MetalRenderer::EnsureGIShadowTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_giGBufPosTex && width == m_giShadowW && height == m_giShadowH) return;
	DestroyGIShadowTargets();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	MTLTextureDescriptor* posDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
	posDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	posDesc.storageMode = MTLStorageModePrivate;
	m_giGBufPosTex  = (void*)CFBridgingRetain([device newTextureWithDescriptor:posDesc]);
	m_giGBufNormTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:posDesc]);

	MTLTextureDescriptor* dDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kDepthFormat width:width height:height mipmapped:NO];
	dDesc.usage       = MTLTextureUsageRenderTarget;
	dDesc.storageMode = MTLStorageModePrivate;
	m_giGBufDepth = (void*)CFBridgingRetain([device newTextureWithDescriptor:dDesc]);

	MTLTextureDescriptor* rawDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:width height:height mipmapped:NO];
	rawDesc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	rawDesc.storageMode = MTLStorageModePrivate;
	m_giShadowRawTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:rawDesc]);

	// Local-light visibility mask (4 channels, one per local light).
	MTLTextureDescriptor* localDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
	localDesc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	localDesc.storageMode = MTLStorageModePrivate;
	m_giLocalMaskTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:localDesc]);

	// History carries rgb = world position (this value was written for) + a =
	// shadow scalar, so giShadowTemporal can reject a reprojection that landed on
	// an unrelated/disoccluded surface instead of blending in a wrong value.
	MTLTextureDescriptor* histDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
	histDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	histDesc.storageMode = MTLStorageModePrivate;
	m_giShadowHistory[0] = (void*)CFBridgingRetain([device newTextureWithDescriptor:histDesc]);
	m_giShadowHistory[1] = (void*)CFBridgingRetain([device newTextureWithDescriptor:histDesc]);
	// Final result is a plain scalar (fragmentMain only ever samples .r), written
	// by a render pass (giShadowBlur) rather than a compute kernel — RenderTarget,
	// not ShaderWrite, and back to the smaller R16Float format.
	MTLTextureDescriptor* resultDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float width:width height:height mipmapped:NO];
	resultDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	resultDesc.storageMode = MTLStorageModePrivate;
	m_giShadowResult = (void*)CFBridgingRetain([device newTextureWithDescriptor:resultDesc]);

	m_giShadowHistoryIdx   = 0;
	m_giShadowHistoryValid = false; // fresh (undefined-content) textures — first frame skips the history blend
	m_giShadowW = width; m_giShadowH = height;
}

void MetalRenderer::DestroyGIShadowTargets()
{
	if (m_giGBufPosTex)       { CFBridgingRelease(m_giGBufPosTex);       m_giGBufPosTex = nullptr; }
	if (m_giGBufNormTex)      { CFBridgingRelease(m_giGBufNormTex);      m_giGBufNormTex = nullptr; }
	if (m_giGBufDepth)        { CFBridgingRelease(m_giGBufDepth);        m_giGBufDepth = nullptr; }
	if (m_giShadowRawTex)     { CFBridgingRelease(m_giShadowRawTex);     m_giShadowRawTex = nullptr; }
	if (m_giLocalMaskTex)     { CFBridgingRelease(m_giLocalMaskTex);     m_giLocalMaskTex = nullptr; }
	if (m_giShadowHistory[0]) { CFBridgingRelease(m_giShadowHistory[0]); m_giShadowHistory[0] = nullptr; }
	if (m_giShadowHistory[1]) { CFBridgingRelease(m_giShadowHistory[1]); m_giShadowHistory[1] = nullptr; }
	if (m_giShadowResult)     { CFBridgingRelease(m_giShadowResult);     m_giShadowResult = nullptr; }
	m_giShadowHistoryValid = false;
	m_giShadowW = m_giShadowH = 0;
}

void MetalRenderer::EncodeGIShadowRays(void* cmdBufPtr, int width, int height)
{
	if (!m_giEnabled || !m_giSupported || !m_world) return;
	// The active path's acceleration structure must exist (built this frame by
	// EncodeGIAccelBuild): HW = TLAS, SW = CPU-BVH buffers + at least 1 instance.
	if (m_giHwRt ? !m_giTlas
	             : (!m_giSwNodeBuf || !m_giSwTriBuf || !m_giSwInstanceBuf || m_giSwInstanceCount == 0))
		return;
	EnsureGIShadowPipelines();
	if (!m_giGBufPipeline || !m_giShadowTemporalPipeline || !m_giShadowBlurPipeline)
		return;
	if (m_giHwRt ? !m_giShadowRayPipeline : !m_giShadowRaySwPipeline)
		return;
	if (width <= 0 || height <= 0) return;

	EnsureGIShadowTargets(width, height);

	// m_renderWorld was already extracted this frame by EncodeGIAccelBuild (same
	// day-night/camera state); this pass just needs its own frustum cull/sort for
	// the G-buffer rasterization (EncodeGIAccelBuild's caster loop is unculled —
	// rays go in arbitrary directions — but the G-buffer is a normal camera-facing
	// raster pass, and covers ALL objects, not just casters, since every shaded
	// pixel needs a shadow value, not just occluders).
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;

	const glm::mat4 viewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	// Raster matrix needs the GL→Metal depth remap (like EncodeShadowMap's
	// lightClip) or geometry closer than ~2× the near plane falls outside Metal's
	// [0,w] clip range and drops out of the G-buffer. The temporal reprojection
	// below keeps using the UNfixed viewProj: kMetalClipFix only rescales z, and
	// giShadowTemporal's ndc math uses clip.xy/clip.w exclusively.
	const glm::mat4 viewProjRaster = HE::kMetalClipFix * viewProj;

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

		// ── 1. World-space G-buffer pre-pass (position + normal, half-res) ──────
		MTLRenderPassDescriptor* gp = [MTLRenderPassDescriptor renderPassDescriptor];
		gp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_giGBufPosTex;
		gp.colorAttachments[0].loadAction  = MTLLoadActionClear;
		gp.colorAttachments[0].storeAction = MTLStoreActionStore;
		gp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // a = 0 → background
		gp.colorAttachments[1].texture     = (__bridge id<MTLTexture>)m_giGBufNormTex;
		gp.colorAttachments[1].loadAction  = MTLLoadActionClear;
		gp.colorAttachments[1].storeAction = MTLStoreActionStore;
		gp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_giGBufDepth;
		gp.depthAttachment.loadAction  = MTLLoadActionClear;
		gp.depthAttachment.storeAction = MTLStoreActionDontCare;
		gp.depthAttachment.clearDepth  = 1.0;
		id<MTLRenderCommandEncoder> genc = [cmdBuf renderCommandEncoderWithDescriptor:gp];
		[genc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_giGBufPipeline];
		[genc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		HE::UUID lastId{}; const GpuMesh* cMesh = nullptr; bool valid = false;
		for (uint32_t idx : m_sortedIndices)
		{
			const RenderObject& obj = m_renderWorld.objects[idx];
			GIPosUniformsCPU u;
			u.mvp   = viewProjRaster * obj.transform;
			u.model = obj.transform;
			if (!valid || obj.meshAssetId != lastId)
			{ cMesh = ResolveMesh(obj.meshAssetId); lastId = obj.meshAssetId; valid = true; }
			const GpuMesh* drawMesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!drawMesh) continue;
			id<MTLBuffer> vbuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
			id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
			NSUInteger    ic   = (NSUInteger)drawMesh->indexCount;
			[genc setVertexBuffer:vbuf offset:0 atIndex:0];
			[genc setVertexBytes:&u length:sizeof(u) atIndex:1];
			[genc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ic
			                  indexType:MTLIndexTypeUInt32 indexBuffer:ibuf indexBufferOffset:0];
		}
		[genc endEncoding];

		// ── 2. Ray-traced occlusion (compute) — HW intersection_query against
		// the TLAS, or SW BVH traversal against the CPU-built buffers. Same
		// params block; frame.w carries the instance count on the SW path.
		id<MTLComputeCommandEncoder> cenc = [cmdBuf computeCommandEncoder];
		GIShadowParamsCPU sp{};
		// Trace toward the brightest directional light — the light fragmentMain's
		// loop actually shades with — NOT the sky-dome sunDirection (see
		// RenderWorld::dominantDirectionalLight's comment for the night-scene failure mode).
		glm::vec3 towardLight, lightColorIntensity;
		m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
		sp.sunDirRadius = glm::vec4(towardLight, glm::radians(m_giLightRadius));
		m_giShadowFrameSeed += 1.0f;
		sp.frame = glm::vec4(m_giShadowFrameSeed, static_cast<float>(width), static_cast<float>(height),
		                     static_cast<float>(m_giSwInstanceCount));
		// First 4 local (point/spot) lights of the same 8-light window the scene
		// shader iterates — fragmentMain counts non-directional lights in the
		// SAME order to index the mask channels (shared with every backend).
		{
			const HE::PackedLocalShadowLights lm = HE::BuildMaskedLocalLights(m_renderWorld);
			static_assert(HE::kMaxMaskedLocalLights == 4, "GIShadowParams has 4 mask channels");
			for (int i = 0; i < HE::kMaxMaskedLocalLights; ++i)
				sp.localPosRange[i] = lm.posRange[i];
			sp.extra = glm::vec4(static_cast<float>(lm.count), 0.0f, 0.0f, 0.0f);
		}
		[cenc setTexture:(__bridge id<MTLTexture>)m_giGBufPosTex atIndex:0];
		[cenc setTexture:(__bridge id<MTLTexture>)m_giGBufNormTex atIndex:1];
		[cenc setTexture:(__bridge id<MTLTexture>)m_giShadowRawTex atIndex:2];
		[cenc setTexture:(__bridge id<MTLTexture>)m_giLocalMaskTex atIndex:3];
		if (m_giHwRt)
		{
			[cenc setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_giShadowRayPipeline];
			[cenc setAccelerationStructure:(__bridge id<MTLAccelerationStructure>)m_giTlas atBufferIndex:0];
			[cenc setBytes:&sp length:sizeof(sp) atIndex:1];
			// Every BLAS the TLAS references must be explicitly declared used — Metal
			// does not auto-track residency through an acceleration structure.
			[cenc useResource:(__bridge id<MTLAccelerationStructure>)m_giTlas usage:MTLResourceUsageRead];
			for (void* b : m_giUniqueBlas)
				[cenc useResource:(__bridge id<MTLAccelerationStructure>)b usage:MTLResourceUsageRead];
		}
		else
		{
			[cenc setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_giShadowRaySwPipeline];
			[cenc setBuffer:(__bridge id<MTLBuffer>)m_giSwNodeBuf     offset:0 atIndex:0];
			[cenc setBuffer:(__bridge id<MTLBuffer>)m_giSwTriBuf      offset:0 atIndex:1];
			[cenc setBuffer:(__bridge id<MTLBuffer>)m_giSwInstanceBuf offset:0 atIndex:2];
			[cenc setBytes:&sp length:sizeof(sp) atIndex:3];
		}
		const MTLSize tgSize  = MTLSizeMake(8, 8, 1);
		const MTLSize tgCount = MTLSizeMake((NSUInteger)((width + 7) / 8), (NSUInteger)((height + 7) / 8), 1);
		[cenc dispatchThreadgroups:tgCount threadsPerThreadgroup:tgSize];
		[cenc endEncoding];

		// ── 3. Temporal accumulation (reproject + blend into ping-pong history) ─
		const int curIdx  = m_giShadowHistoryIdx;
		const int prevIdx = 1 - curIdx;
		{
			MTLRenderPassDescriptor* tp = [MTLRenderPassDescriptor renderPassDescriptor];
			tp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_giShadowHistory[curIdx];
			tp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			tp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> tenc = [cmdBuf renderCommandEncoderWithDescriptor:tp];
			[tenc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_giShadowTemporalPipeline];
			[tenc setFragmentTexture:(__bridge id<MTLTexture>)m_giGBufPosTex atIndex:0];
			[tenc setFragmentTexture:(__bridge id<MTLTexture>)m_giShadowRawTex atIndex:1];
			[tenc setFragmentTexture:(__bridge id<MTLTexture>)m_giShadowHistory[prevIdx] atIndex:2];
			[tenc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_ssaoPointSampler atIndex:0];
			GITemporalParamsCPU tparams;
			tparams.prevViewProj = m_giPrevViewProj;
			tparams.blend = glm::vec4(m_giShadowHistoryValid ? 0.9f : 0.0f,
			                          static_cast<float>(width), static_cast<float>(height), 0.0f);
			[tenc setFragmentBytes:&tparams length:sizeof(tparams) atIndex:0];
			[tenc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[tenc endEncoding];
		}
		m_giShadowHistoryValid = true;
		m_giShadowHistoryIdx   = prevIdx;
		m_giPrevViewProj       = viewProj; // for NEXT frame's reprojection

		// ── 4. Spatial blur → final result fragmentMain samples ─────────────
		{
			MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor];
			bp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_giShadowResult;
			bp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			bp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> benc = [cmdBuf renderCommandEncoderWithDescriptor:bp];
			[benc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_giShadowBlurPipeline];
			[benc setFragmentTexture:(__bridge id<MTLTexture>)m_giShadowHistory[curIdx] atIndex:0];
			[benc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
			[benc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[benc endEncoding];
		}
	}
}

void MetalRenderer::EnsureGIProbeGrid()
{
	if (m_giProbeGridBuilt) return;
	if (m_renderWorld.objects.empty()) return; // wait for real geometry before committing to a grid

	// m_renderWorld was re-extracted by EncodeGIAccelBuild's m_extractor.extract()
	// call earlier this frame, which creates BRAND NEW RenderObjects whose
	// worldBounds are whatever the extractor could produce — invalid for meshes it
	// could not resolve, a unit-cube proxy for particles/skinned (see
	// RenderObject.h) — NOT the real per-mesh bounds EncodeShadowMap/EncodeSSAO
	// refresh in their own passes. Those refreshes don't survive the re-extraction,
	// so refresh here too before unioning, or the grid ends up sized to a handful
	// of proxy boxes instead of the actual scene.
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// KNOWN v1 LIMITATION: this only unions m_renderWorld.objects (the generic
	// mesh-asset RenderObject set) — Landscape/Terrain chunks are a separate
	// rendering system (see [[terrain-lod-chunking]]) and are NOT included, so a
	// scene dominated by terrain will get a probe grid sized to its small props
	// only, leaving the terrain surfaces themselves outside grid coverage (they
	// sample zero indirect diffuse — safe, just visibly under-lit, not a crash).
	// Confirmed empirically: the ShadowValidation test scene's large background
	// surface stays outside the grid even after the worldBounds refresh above.
	// Extending probe coverage to terrain is a follow-up, not in this slice.
	//
	// Union of every object's (now-correct) world bounds.
	HE::AABB bounds;
	for (const RenderObject& obj : m_renderWorld.objects)
		if (obj.worldBounds.isValid())
			bounds.expand(obj.worldBounds);
	if (!bounds.isValid())
	{
		// Fallback: a modest default volume around the origin so GI still does
		// something sane in an empty/primitive-only scene.
		bounds.min = glm::vec3(-10.0f);
		bounds.max = glm::vec3(10.0f);
	}

	const glm::vec3 extent = bounds.max - bounds.min;
	glm::ivec3 counts;
	counts.x = std::clamp(static_cast<int>(std::ceil(extent.x / kGIProbeSpacing)) + 1, 1, kGIMaxProbesPerAxis);
	counts.y = std::clamp(static_cast<int>(std::ceil(extent.y / kGIProbeSpacing)) + 1, 1, kGIMaxProbesPerAxis);
	counts.z = std::clamp(static_cast<int>(std::ceil(extent.z / kGIProbeSpacing)) + 1, 1, kGIMaxProbesPerAxis);

	m_giGridOrigin        = bounds.min;
	m_giGridCounts        = counts;
	m_giProbeCount        = counts.x * counts.y * counts.z;
	m_giProbesPerRow      = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(m_giProbeCount))));
	m_giProbeUpdateCursor = 0;
	m_giProbeGridBuilt    = true;

	EnsureGIProbeAtlas();

	HE_LOG_INFO(RHI, "%s",
		("MetalRenderer: GI probe grid built — " + std::to_string(m_giProbeCount) + " probes ("
		 + std::to_string(counts.x) + "x" + std::to_string(counts.y) + "x" + std::to_string(counts.z)
		 + "), spacing " + std::to_string(kGIProbeSpacing)).c_str());
}

void MetalRenderer::EnsureGIProbePipeline()
{
	// HW and SW variants are mutually exclusive per session (m_giHwRt is fixed
	// after EnsureRaytracingSupport), so one built pipeline means done.
	if (m_giProbeUpdatePipeline || m_giProbeUpdateSwPipeline) return;
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* error = nil;
		const char* src = m_giHwRt ? kGIProbeMSL : kGISWMSL;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:src] options:nil error:&error];
		if (!lib)
		{
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: GI probe shader compile failed: ")
				 + (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
			return;
		}
		id<MTLFunction> fn = [lib newFunctionWithName:(m_giHwRt ? @"giProbeUpdate" : @"giProbeUpdateSw")];
		id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn error:&error];
		if (!pso)
		{
			HE_LOG_ERROR(RHI, "%s", "MetalRenderer: GI probe-update pipeline creation failed");
			return;
		}
		if (m_giHwRt) m_giProbeUpdatePipeline   = (void*)CFBridgingRetain(pso);
		else          m_giProbeUpdateSwPipeline = (void*)CFBridgingRetain(pso);
	}
}

void MetalRenderer::EnsureGIProbeAtlas()
{
	if (m_giProbeCount <= 0) return;
	const int probeRows = static_cast<int>(std::ceil(static_cast<float>(m_giProbeCount) / static_cast<float>(m_giProbesPerRow)));
	const int atlasW = m_giProbesPerRow * kGIProbeOctSize;
	const int atlasH = probeRows * kGIProbeOctSize;

	DestroyGIProbeAtlas();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	// read_write access (in-place EMA blend, one thread per texel, no cross-thread
	// aliasing within a dispatch — see kGIProbeMSL's header comment) needs
	// MTLReadWriteTextureTier2 for these 16-bit float formats; universal on the
	// Apple Silicon this path is already gated to (device.supportsRaytracing).
	MTLTextureDescriptor* irrDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:atlasW height:atlasH mipmapped:NO];
	irrDesc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	irrDesc.storageMode = MTLStorageModePrivate;
	m_giIrradianceAtlas = (void*)CFBridgingRetain([device newTextureWithDescriptor:irrDesc]);

	MTLTextureDescriptor* visDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:atlasW height:atlasH mipmapped:NO];
	visDesc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	visDesc.storageMode = MTLStorageModePrivate;
	m_giVisibilityAtlas = (void*)CFBridgingRetain([device newTextureWithDescriptor:visDesc]);
}

void MetalRenderer::DestroyGIProbeAtlas()
{
	if (m_giIrradianceAtlas) { CFBridgingRelease(m_giIrradianceAtlas); m_giIrradianceAtlas = nullptr; }
	if (m_giVisibilityAtlas) { CFBridgingRelease(m_giVisibilityAtlas); m_giVisibilityAtlas = nullptr; }
}

void MetalRenderer::EncodeGIProbeUpdate(void* cmdBufPtr)
{
	if (!m_giEnabled || !m_giSupported || !m_world) return;
	if (m_giHwRt ? !m_giTlas
	             : (!m_giSwNodeBuf || !m_giSwTriBuf || !m_giSwInstanceBuf || m_giSwInstanceCount == 0))
		return;
	EnsureGIProbeGrid();
	if (!m_giProbeGridBuilt || m_giProbeCount <= 0) return;
	EnsureGIProbePipeline();
	if (m_giHwRt ? !m_giProbeUpdatePipeline : !m_giProbeUpdateSwPipeline) return;
	if (!m_giIrradianceAtlas || !m_giVisibilityAtlas) return;

	const int budget = std::min(m_giProbeBudgetPerFrame > 0 ? m_giProbeBudgetPerFrame : 1, m_giProbeCount);

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
		[enc setTexture:(__bridge id<MTLTexture>)m_giIrradianceAtlas atIndex:0];
		[enc setTexture:(__bridge id<MTLTexture>)m_giVisibilityAtlas atIndex:1];
		if (m_giHwRt)
		{
			[enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_giProbeUpdatePipeline];
			[enc setAccelerationStructure:(__bridge id<MTLAccelerationStructure>)m_giTlas atBufferIndex:0];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giInstanceColorBuffer offset:0 atIndex:1];
		}
		else
		{
			// SW path: BVH buffers instead of the TLAS; albedo comes from the
			// instance itself (baseColor), no separate colour buffer needed.
			[enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_giProbeUpdateSwPipeline];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwNodeBuf     offset:0 atIndex:0];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwTriBuf      offset:0 atIndex:1];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwInstanceBuf offset:0 atIndex:2];
		}

		GIProbeParamsCPU pp{};
		pp.gridOrigin = glm::vec4(m_giGridOrigin, kGIProbeSpacing);
		pp.gridCounts = glm::vec4(static_cast<float>(m_giGridCounts.x), static_cast<float>(m_giGridCounts.y),
		                         static_cast<float>(m_giGridCounts.z), static_cast<float>(m_giProbesPerRow));
		// Max ray distance: comfortably covers the grid's own diagonal so rays can
		// reach across the whole probed volume; hysteresis matches the shadow
		// pass's temporal-accumulation feel (converges over ~1-2s at 60fps).
		const float maxDist = glm::length(glm::vec3(m_giGridCounts) * kGIProbeSpacing) + kGIProbeSpacing;
		pp.rayParams    = glm::vec4(maxDist, 0.92f, static_cast<float>(m_giProbeUpdateCursor), static_cast<float>(budget));
		// Same dominant-directional pick as EncodeGIShadowRays: the one-bounce
		// estimate must bounce the light the scene is actually lit by, with THAT
		// light's colour*intensity — not the sky-dome sun + environment settings.
		glm::vec3 towardLight, lightColorIntensity;
		m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
		pp.sunColor     = glm::vec4(lightColorIntensity, 0.0f);
		pp.skyAmbient   = glm::vec4(m_renderWorld.ambient, 0.0f);
		// Local (point/spot) lights feed the one-bounce estimate — a scene keyed
		// by point lights otherwise converges to pitch-black probes. Same 8-light
		// window EncodeScene binds for direct shading (shared with every backend).
		const HE::PackedLightArray lp = HE::BuildPackedLightArray(m_renderWorld);
		static_assert(HE::kMaxLightWindow == 8, "GIProbeParams has 8 light slots");
		for (int i = 0; i < HE::kMaxLightWindow; ++i)
		{
			pp.lightPosRange[i]  = lp.posRange[i];
			pp.lightColorType[i] = lp.colorType[i];
			pp.lightDirCos[i]    = lp.dirCos[i];
		}
		pp.sunDirRadius = glm::vec4(towardLight, static_cast<float>(lp.count));
		if (m_giHwRt)
		{
			[enc setBytes:&pp length:sizeof(pp) atIndex:2];
			// Every BLAS the TLAS references must be explicitly declared used — Metal
			// does not auto-track residency through an acceleration structure.
			[enc useResource:(__bridge id<MTLAccelerationStructure>)m_giTlas usage:MTLResourceUsageRead];
			for (void* b : m_giUniqueBlas)
				[enc useResource:(__bridge id<MTLAccelerationStructure>)b usage:MTLResourceUsageRead];
		}
		else
		{
			// SW kernel: params at buffer(3); sunColor.w carries the instance count.
			pp.sunColor.w = static_cast<float>(m_giSwInstanceCount);
			[enc setBytes:&pp length:sizeof(pp) atIndex:3];
		}

		// One threadgroup per probe in this frame's batch (kGIProbeOctSize^2
		// threads/group = one thread per output texel — the "gather", see
		// kGIProbeMSL's header comment).
		const MTLSize tgSize  = MTLSizeMake(kGIProbeOctSize, kGIProbeOctSize, 1);
		const MTLSize tgCount = MTLSizeMake(static_cast<NSUInteger>(budget), 1, 1);
		[enc dispatchThreadgroups:tgCount threadsPerThreadgroup:tgSize];
		[enc endEncoding];
	}

	m_giProbeUpdateCursor = (m_giProbeUpdateCursor + budget) % m_giProbeCount;
}

// ─── Asset mesh upload ────────────────────────────────────────────────────────

// Upload a TextureAsset (RGBA8 or a cooked ASTC/BC7/BC3 block format) into a
// retained id<MTLTexture>, or nullptr when unusable / this GPU can't sample the
// shipped format. Defined below; forward-declared so the mesh uploads can share it.
static void* uploadMetalTexture(id<MTLDevice> device, const TextureAsset* tex);

const MetalRenderer::GpuMesh* MetalRenderer::ResolveMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_meshCache.find(assetId); it != m_meshCache.end())
		return &it->second;

	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(assetId);
	if (!asset || asset->indices.empty() || (asset->vertices.empty() && !asset->cooked))
		return nullptr;

	GpuMesh mesh;
	mesh.indexCount = static_cast<int>(asset->indices.size());
	const size_t vertexCount = asset->cooked ? asset->vertexCount : asset->vertices.size() / 3;

	// Cooked (packaged) assets ship the interleaved pos+norm+uv buffer + baked
	// AABB, built once at pack time — upload it as-is. Loose/editor assets are
	// interleaved here on first draw (must match the MSL VertexIn layout,
	// zero-filling missing normals/uvs).
	std::vector<float> built;
	const std::vector<float>* vtx = &asset->interleaved;
	if (asset->cooked)
	{
		mesh.localBounds.min = { asset->boundsMin[0], asset->boundsMin[1], asset->boundsMin[2] };
		mesh.localBounds.max = { asset->boundsMax[0], asset->boundsMax[1], asset->boundsMax[2] };
	}
	else
	{
		built.reserve(vertexCount * 8);
		for (size_t v = 0; v < vertexCount; ++v)
		{
			built.insert(built.end(),
				{ asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
			if (v * 3 + 2 < asset->normals.size())
				built.insert(built.end(),
					{ asset->normals[v*3+0], asset->normals[v*3+1], asset->normals[v*3+2] });
			else
				built.insert(built.end(), { 0.0f, 0.0f, 0.0f });
			if (v * 2 + 1 < asset->uvs.size())
				built.insert(built.end(), { asset->uvs[v*2+0], asset->uvs[v*2+1] });
			else
				built.insert(built.end(), { 0.0f, 0.0f });
		}
		vtx = &built;
		mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
	}
	const std::vector<float>& interleaved = *vtx;

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	mesh.vertexBuf  = (void*)CFBridgingRetain(
		[device newBufferWithBytes:interleaved.data()
		                    length:interleaved.size() * sizeof(float)
		                   options:MTLResourceStorageModeShared]);
	mesh.indexBuf   = (void*)CFBridgingRetain(
		[device newBufferWithBytes:asset->indices.data()
		                    length:asset->indices.size() * sizeof(uint32_t)
		                   options:MTLResourceStorageModeShared]);

	// Base color texture via the mesh's material — baked UUID (packed builds)
	// with the editor path as fallback (loose content).
	if (const MaterialAsset* mat =
	        m_contentManager->resolveMaterialRef(asset->materialId, asset->materialPath))
	{
		const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
		const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
		// RGBA8 + cooked ASTC/BC7/BC3 via the shared uploader (skips a block format
		// this GPU can't sample — e.g. BC on Apple Silicon, ASTC on Intel).
		mesh.texture = uploadMetalTexture(device, m_contentManager->resolveTextureRef(texId0, texPath0));
	}

	HE_LOG_INFO(RHI, "%s",
		("MetalRenderer: uploaded mesh '" + asset->name + "' ("
		 + std::to_string(vertexCount) + " verts"
		 + (mesh.texture ? ", textured" : "") + ")").c_str());

	return &m_meshCache.emplace(assetId, mesh).first->second;
}

// ─── Skeletal mesh upload ─────────────────────────────────────────────────────
const MetalRenderer::GpuSkeletalMesh*
MetalRenderer::ResolveSkeletalMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_skeletalMeshCache.find(assetId); it != m_skeletalMeshCache.end())
		return &it->second;

	const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(assetId);
	if (!asset || asset->vertices.empty() || asset->indices.empty())
		return nullptr;

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	const size_t vertexCount = asset->vertices.size() / 3;

	// Interleaved pos+normal+uv (same layout as GpuMesh, buffer 0)
	std::vector<float> interleaved;
	interleaved.reserve(vertexCount * 8);
	for (size_t v = 0; v < vertexCount; ++v)
	{
		interleaved.insert(interleaved.end(),
			{ asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
		if (v*3+2 < asset->normals.size())
			interleaved.insert(interleaved.end(),
				{ asset->normals[v*3+0], asset->normals[v*3+1], asset->normals[v*3+2] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
		if (v*2+1 < asset->uvs.size())
			interleaved.insert(interleaved.end(), { asset->uvs[v*2+0], asset->uvs[v*2+1] });
		else
			interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
	}

	// Bone IDs (uint4 per vertex, buffer 2) — zero-padded when absent
	std::vector<uint32_t> boneIds(vertexCount * 4, 0u);
	if (!asset->boneIDs.empty())
		std::copy_n(asset->boneIDs.begin(),
		            std::min(asset->boneIDs.size(), vertexCount * 4),
		            boneIds.begin());

	// Bone weights (float4 per vertex, buffer 3) — default 100% joint 0
	std::vector<float> boneWgts(vertexCount * 4, 0.0f);
	for (size_t v = 0; v < vertexCount; ++v) boneWgts[v * 4] = 1.0f;
	if (!asset->boneWeights.empty())
		std::copy_n(asset->boneWeights.begin(),
		            std::min(asset->boneWeights.size(), vertexCount * 4),
		            boneWgts.begin());

	GpuSkeletalMesh mesh;
	mesh.indexCount  = static_cast<int>(asset->indices.size());
	mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
	mesh.vertexBuf  = (void*)CFBridgingRetain(
		[device newBufferWithBytes:interleaved.data()
		                    length:interleaved.size() * sizeof(float)
		                   options:MTLResourceStorageModeShared]);
	mesh.boneIdBuf  = (void*)CFBridgingRetain(
		[device newBufferWithBytes:boneIds.data()
		                    length:boneIds.size() * sizeof(uint32_t)
		                   options:MTLResourceStorageModeShared]);
	mesh.boneWgtBuf = (void*)CFBridgingRetain(
		[device newBufferWithBytes:boneWgts.data()
		                    length:boneWgts.size() * sizeof(float)
		                   options:MTLResourceStorageModeShared]);
	mesh.indexBuf   = (void*)CFBridgingRetain(
		[device newBufferWithBytes:asset->indices.data()
		                    length:asset->indices.size() * sizeof(uint32_t)
		                   options:MTLResourceStorageModeShared]);

	if (const MaterialAsset* mat =
	        m_contentManager->resolveMaterialRef(asset->materialId, asset->materialPath))
	{
		const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
		const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
		// RGBA8 + cooked ASTC/BC7/BC3 via the shared uploader (skips a block format
		// this GPU can't sample — e.g. BC on Apple Silicon, ASTC on Intel).
		mesh.texture = uploadMetalTexture(device, m_contentManager->resolveTextureRef(texId0, texPath0));
	}

	return &m_skeletalMeshCache.emplace(assetId, mesh).first->second;
}

// ─── Material override texture ──────────────────────────────────────────────
// Map a cooked TextureFormat to its Metal pixel format, whether this DEVICE can
// sample it, and whether it's block-compressed (all block formats here are
// 16 B / 4x4 so they share the upload byte-math). ASTC needs Apple-family GPUs
// (Apple Silicon); BC7/BC3 need BC support (Intel/AMD Macs, macOS 11+). These are
// mutually exclusive on a given GPU — which is why one pak can't serve both Metal
// and OpenGL on Apple Silicon (see EditorUI texture-compression selection).
static bool metalTexPixelFormat(id<MTLDevice> device, TextureFormat fmt,
                                MTLPixelFormat& outFmt, bool& outIsBlock, bool& outSupported)
{
	outIsBlock = textureFormatIsBlock4x4(fmt);
	switch (fmt)
	{
	case TextureFormat::RGBA8:
		outFmt = MTLPixelFormatRGBA8Unorm;  outSupported = true; return true;
	case TextureFormat::ASTC_4x4:
		outFmt = MTLPixelFormatASTC_4x4_LDR; outSupported = [device supportsFamily:MTLGPUFamilyApple2]; return true;
	case TextureFormat::BC7:
		outFmt = MTLPixelFormatBC7_RGBAUnorm; outSupported = false;
		if (@available(macOS 11.0, *)) outSupported = device.supportsBCTextureCompression;
		return true;
	case TextureFormat::BC3:
		outFmt = MTLPixelFormatBC3_RGBA;      outSupported = false;
		if (@available(macOS 11.0, *)) outSupported = device.supportsBCTextureCompression;
		return true;
	}
	outFmt = MTLPixelFormatRGBA8Unorm; outSupported = false; return false;
}

// Upload a TextureAsset into a retained id<MTLTexture> (nullptr if unusable or this
// GPU can't sample the shipped format). Shared by the material base texture, the
// node-graph project textures, and both mesh uploads.
static void* uploadMetalTexture(id<MTLDevice> device, const TextureAsset* tex)
{
	if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
		return nullptr;
	const uint32_t mips = tex->mipLevels > 0 ? tex->mipLevels : 1;

	MTLPixelFormat pf = MTLPixelFormatRGBA8Unorm; bool isBlock = false, supported = false;
	if (!metalTexPixelFormat(device, tex->format, pf, isBlock, supported) || !supported)
		return nullptr; // unknown format, or this GPU can't sample it → flat

	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:pf width:tex->width height:tex->height mipmapped:(mips > 1)];
	desc.mipmapLevelCount = mips;
	desc.usage       = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeShared;
	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (texture)
	{
		// Upload the pre-baked mip chain (level 0 first). Block formats are
		// compressed (16 B / 4x4 block); RGBA8 is 4 B / texel.
		size_t off = 0; uint32_t lw = (uint32_t)tex->width, lh = (uint32_t)tex->height;
		for (uint32_t l = 0; l < mips; ++l)
		{
			const size_t bpr = isBlock ? ((size_t)((lw + 3) / 4) * 16) : ((size_t)lw * 4);
			const size_t lvl = isBlock ? ((size_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16)
			                           : ((size_t)lw * lh * 4);
			if (off + lvl > tex->data.size()) break; // truncated payload guard
			[texture replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:l
			             withBytes:tex->data.data() + off bytesPerRow:bpr];
			off += lvl; lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1;
		}
	}
	return (void*)CFBridgingRetain(texture);
}

bool MetalRenderer::ResolveMaterialTexture(const HE::UUID& materialId, void*& outTex)
{
	outTex = nullptr;
	if (materialId == HE::UUID{} || !m_contentManager)
		return false;

	if (auto it = m_materialTexCache.find(materialId); it != m_materialTexCache.end())
	{
		outTex = it->second;
		return true;
	}

	const MaterialAsset* mat = m_contentManager->getMaterial(materialId);
	if (!mat)
		return false; // not loaded yet — retry next frame without caching

	const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
	const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
	void* retained = uploadMetalTexture((__bridge id<MTLDevice>)m_device,
		m_contentManager->resolveTextureRef(texId0, texPath0));

	m_materialTexCache.emplace(materialId, retained);
	outTex = retained;
	return true;
}

// Resolve a node-graph project texture (UUID for packed assets, path for loose editor
// assets) to a retained id<MTLTexture>, cached by a stable key. nullptr if not loadable.
void* MetalRenderer::ResolveGraphTexture(const HE::UUID& texId, const std::string& path)
{
	const std::string key = texId != HE::UUID{}
		? (std::to_string(texId.hi) + ":" + std::to_string(texId.lo)) : path;
	if (key.empty() || !m_contentManager) return nullptr;
	if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end()) return it->second;
	void* retained = uploadMetalTexture((__bridge id<MTLDevice>)m_device,
		m_contentManager->resolveTextureRef(texId, path));
	m_graphTexCache.emplace(key, retained);
	return retained;
}

bool MetalRenderer::ResolveMaterialParams(const HE::UUID& materialId,
	glm::vec3& outBaseColor, float& outMetallic, float& outRoughness, float& outOpacity)
{
	if (materialId == HE::UUID{} || !m_contentManager)
		return false;
	const MaterialAsset* mat = m_contentManager->getMaterial(materialId);
	if (!mat)
		return false; // not loaded yet — caller keeps defaults
	outBaseColor = glm::vec3(mat->baseColor[0], mat->baseColor[1], mat->baseColor[2]);
	outMetallic  = mat->metallic;
	outRoughness = mat->roughness;
	// Translucent blend mode forces the sorted alpha-blend pass even at opacity 1 —
	// the shader's own oColor.a then does the actual blending.
	outOpacity   = mat->blendMode == 2 ? std::min(mat->opacity, 0.998f) : mat->opacity;
	return true;
}

void MetalRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
	if (materialId == HE::UUID{}) return;
	if (auto it = m_materialTexCache.find(materialId); it != m_materialTexCache.end())
	{
		// In-flight GPU work may still sample it — retire (released a few frames
		// later) rather than freeing now.
		if (it->second) RetireTexture(it->second);
		m_materialTexCache.erase(it);
	}
}

void MetalRenderer::InvalidateMesh(const HE::UUID& meshId)
{
	// Defer to the render loop where the Metal device context is active.
	if (meshId != HE::UUID{})
		m_pendingMeshInvalidations.push_back(meshId);
}

void MetalRenderer::InvalidateTexture(const HE::UUID& textureId)
{
	// Same deferral as meshes — the graph-texture cache is keyed by UUID string.
	if (textureId != HE::UUID{})
		m_pendingTexInvalidations.push_back(textureId);
}

void MetalRenderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
	// Build each custom-shader material's pipeline state NOW so the first draw
	// doesn't stall on cross-compile + PSO creation inside the encoder loop. The
	// Metal device is always available; cache hits are cheap; built-in-PBR
	// materials resolve no shader and are skipped.
	int built = 0;
	for (const HE::UUID& id : materialIds)
	{
		uint64_t shKey; std::string shFrag;
		std::string shVert;
		if (!ResolveMaterialShader(id, shKey, shFrag, shVert)) continue;
		if (m_materialPipelineCache.find(shKey) != m_materialPipelineCache.end()) continue; // warm
		const MaterialShaderVariant* pre = nullptr;
		if (const MaterialAsset* ma = m_contentManager ? m_contentManager->getMaterial(id) : nullptr)
			for (const auto& var : ma->precompiledShaders)
				if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { pre = &var; break; }
		if (GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre)) ++built;
		// Deferred path active → also warm the G-buffer variant so the first
		// deferred frame doesn't hitch on its cross-compile.
		if (m_renderPath == HE::RenderPath::Deferred)
		{
			uint64_t gbKey; std::string gbFrag, gbVert;
			if (ResolveMaterialShaderGB(id, gbKey, gbFrag, gbVert)
			    && GetOrBuildMaterialPipeline(gbKey, gbFrag, gbVert, nullptr,
			                                  /*blend=*/false, /*gbuffer=*/true))
				++built;
		}
	}
	if (built > 0)
		HE_LOG_INFO(RHI, "%s",
			("MetalRenderer: warmed up " + std::to_string(built) + " material pipeline(s)").c_str());
}

// Encode one material-graph preview primitive into an already-open encoder. Split
// out of RenderMaterialPreview so the interactive preview and the Content-Browser
// thumbnail share the exact same shading while rendering into DIFFERENT targets —
// a thumbnail that reused m_previewColorTex would silently replace whatever the
// Material Editor is showing. The caller owns the render pass, its clear and the
// depth-stencil state; this only sets the pipeline, buffers and textures.
bool MetalRenderer::EncodeMaterialPreview(void* renderEncoder, const HE::UUID& materialId,
                                          float yaw, float pitch, float dist, int shape,
                                          const HE::UUID& meshId)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!enc || !device || !m_contentManager) return false;

	// Resolve the node-graph material pipeline (built-in-PBR materials have none).
	uint64_t shKey; std::string shFrag, shVert;
	if (!ResolveMaterialShader(materialId, shKey, shFrag, shVert)) return false;
	const MaterialAsset* ma = m_contentManager->getMaterial(materialId);
	const MaterialShaderVariant* pre = nullptr;
	if (ma)
		for (const auto& var : ma->precompiledShaders)
			if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { pre = &var; break; }
	id<MTLRenderPipelineState> pso = (__bridge id<MTLRenderPipelineState>)GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre);
	if (!pso) return false;

	// ── Geometry: a picked STATIC MESH, else the procedural primitive. A GpuMesh
	// carries the same interleaved pos3/normal3/uv2 buffer the material pipeline
	// reads in the scene, so it binds here unchanged — only the camera has to grow
	// to the mesh's bounds instead of the unit sphere's.
	const GpuMesh* gm = meshId != HE::UUID{} ? ResolveMesh(meshId) : nullptr;
	// ResolveMesh loads the picked mesh's own baked material, which grows the
	// material pool — a dense vector — and moves `ma`, which is read for the
	// preview's colour and params all the way to the end of this function. The
	// first render after picking a preview mesh is exactly when it happens.
	if (ma) ma = m_contentManager->getMaterial(materialId);
	void* vertBuf = nullptr; void* idxBuf = nullptr; int idxCount = 0;
	glm::vec3 center(0.0f);
	float     radius = 1.0f; // what `dist` is measured in, so it frames alike
	if (gm && gm->vertexBuf && gm->indexBuf && gm->indexCount > 0)
	{
		vertBuf = gm->vertexBuf; idxBuf = gm->indexBuf; idxCount = gm->indexCount;
		if (gm->localBounds.isValid())
		{
			center = (gm->localBounds.min + gm->localBounds.max) * 0.5f;
			radius = std::max(glm::length(gm->localBounds.max - gm->localBounds.min) * 0.5f, 1e-4f);
		}
	}
	else
	{
		// Lazy preview primitive (interleaved pos3/normal3/uv2 + uint32 indices),
		// rebuilt when the requested shape changes. Geometry is shared with the GL
		// path via buildPreviewMesh so the two backends can never drift apart.
		if (!m_previewVB || m_previewShape != shape)
		{
			if (m_previewVB) { CFBridgingRelease(m_previewVB); m_previewVB = nullptr; }
			if (m_previewIB) { CFBridgingRelease(m_previewIB); m_previewIB = nullptr; }
			std::vector<float> verts; std::vector<uint32_t> idx;
			HE::buildPreviewMesh(shape, verts, idx);
			m_previewIdxCount = (int)idx.size();
			m_previewShape    = shape;
			m_previewVB = (void*)CFBridgingRetain([device newBufferWithBytes:verts.data()
				length:verts.size() * sizeof(float) options:MTLResourceStorageModeShared]);
			m_previewIB = (void*)CFBridgingRetain([device newBufferWithBytes:idx.data()
				length:idx.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared]);
		}
		vertBuf = m_previewVB; idxBuf = m_previewIB; idxCount = m_previewIdxCount;
	}
	if (!vertBuf || !idxBuf || idxCount <= 0) return false;

	[enc setRenderPipelineState:pso];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];

	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center
		+ glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * (dist * radius);
	const glm::mat4 view  = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	// Clip planes scale with the subject: a 400 m mesh must not sit behind the
	// unit sphere's fixed 50 m far plane, and a 5 cm one must not clip on near.
	const glm::mat4 proj  = glm::perspective(glm::radians(32.0f), 1.0f,
		std::max(0.001f, 0.02f * radius), (dist + 8.0f) * radius + 1.0f);
	const glm::mat4 model(1.0f);
	UnlitUniforms ui;
	ui.mvp   = proj * view * model;
	ui.model = model;
	ui.color = glm::vec4(ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2]) : glm::vec3(1.0f), 1.0f);
	ui.flags = glm::vec4(0.0f);
	ui.pbr   = glm::vec4(ma ? ma->metallic : 0.0f, ma ? ma->roughness : 0.5f, ma ? ma->opacity : 1.0f, 0.0f);
	[enc setVertexBuffer:(__bridge id<MTLBuffer>)vertBuf offset:0 atIndex:0];
	[enc setVertexBytes:&ui length:sizeof(ui) atIndex:1];

	HE::MaterialShaderLibrary::Lighting lit{};
	const glm::vec3 sd = glm::normalize(glm::vec3(0.45f, 0.75f, 0.55f));
	lit.sunDir[0] = sd.x; lit.sunDir[1] = sd.y; lit.sunDir[2] = sd.z; lit.sunDir[3] = 0.0f;
	lit.sunColor[0] = lit.sunColor[1] = lit.sunColor[2] = 1.05f;
	lit.ambient[0] = lit.ambient[1] = lit.ambient[2] = 0.28f;
	lit.camPos[0] = camPos.x; lit.camPos[1] = camPos.y; lit.camPos[2] = camPos.z;
	// Studio sun as the single array light so heLitP() previews shade correctly
	// (same seed as the GL backend's material preview). heLitP has NO separate
	// sun term — sunDir/sunColor above only feed the legacy heLit() — so leaving
	// counts at 0 rendered every graph material as a flat, unshaded ambient disc.
	lit.lightPos[0][3]   = 0.0f; // directional
	lit.lightDir[0][0]   = -sd.x; lit.lightDir[0][1] = -sd.y; lit.lightDir[0][2] = -sd.z;
	lit.lightColor[0][0] = lit.lightColor[0][1] = lit.lightColor[0][2] = 1.05f;
	lit.lightColor[0][3] = 1.0f;
	lit.counts[0]        = 1.0f;
	[enc setFragmentBytes:&lit length:sizeof(lit) atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
	// WPO materials read HeLighting/HeParams in the VERTEX stage (buffers 2/3).
	if (!shVert.empty())
	{
		[enc setVertexBytes:&lit length:sizeof(lit) atIndex:2];
		float vpad[64] = { 0 };
		if (ma && !ma->shaderParamData.empty())
			std::memcpy(vpad, ma->shaderParamData.data(),
			            std::min(ma->shaderParamData.size(), size_t(64)) * sizeof(float));
		[enc setVertexBytes:vpad length:sizeof(vpad) atIndex:3];
	}

	if (ma && !ma->shaderParamData.empty())
	{
		float padded[64] = { 0 };
		std::memcpy(padded, ma->shaderParamData.data(),
		            std::min(ma->shaderParamData.size(), size_t(64)) * sizeof(float));
		[enc setFragmentBytes:padded length:sizeof(padded) atIndex:2];
	}
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_dummyTexture atIndex:0]; // heTex0
	if (ma)
	{
		const size_t nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
			std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
		for (size_t i = 0; i < nTex; ++i)
		{
			const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
			const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
			if (void* t = ResolveGraphTexture(gid, gp))
			{
				[enc setFragmentTexture:(__bridge id<MTLTexture>)t atIndex:(i + 1)];
				[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:(i + 1)];
			}
		}
	}
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)idxCount
	                 indexType:MTLIndexTypeUInt32 indexBuffer:(__bridge id<MTLBuffer>)idxBuf
	         indexBufferOffset:0];
	return true;
}

// Build the small unskinned preview pipeline once. Shared by the mesh/material
// previews and the world preview, so there is exactly one place that knows this
// shader — a second copy is how two previews start lighting differently.
bool MetalRenderer::EnsureMeshPreviewPipeline()
{
	if (m_meshPreviewPipeline) return true;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return false;
	@autoreleasepool
	{
		{
			NSError* error = nil;
			NSString* src = @R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { packed_float3 position; packed_float3 normal; packed_float2 uv; };
// The tail (sun/skyAmbient/sunColor) is optional: every other shader sharing
// UnlitUniforms declares only the prefix, which is why appending was safe.
struct Uniforms { float4x4 mvp; float4x4 model; float4 color; float4 flags; float4 pbr;
                  float4 sun; float4 skyAmbient; float4 sunColor; };
struct VOut { float4 position [[position]]; float3 normal; float2 uv; float3 worldPos; };

vertex VOut meshPreviewVertex(uint vid [[vertex_id]],
                              const device VertexIn* verts [[buffer(0)]],
                              constant Uniforms&     u     [[buffer(1)]])
{
    float4 p = float4(float3(verts[vid].position), 1.0);
    float3x3 m3 = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz);
    VOut o;
    o.position = u.mvp * p;
    o.normal   = m3 * float3(verts[vid].normal);
    o.uv       = float2(verts[vid].uv);
    o.worldPos = (u.model * p).xyz;
    return o;
}

fragment float4 meshPreviewFragment(VOut in [[stage_in]],
                                    constant Uniforms& u [[buffer(1)]],
                                    texture2d<float> tex [[texture(0)]],
                                    sampler samp [[sampler(0)]])
{
    bool   lit = u.sun.w > 0.0;
    float3 L   = lit ? normalize(u.sun.xyz) : normalize(float3(0.45, 0.75, 0.55));
    float3 lc  = lit ? u.sunColor.rgb : float3(1.0);
    float3 amb = lit ? u.skyAmbient.rgb : float3(0.32);
    float3 N = normalize(in.normal);
    float3 V = normalize(u.flags.yzw - in.worldPos);
    float3 H = normalize(L + V);
    float diff  = max(dot(N, L), 0.0);
    float rough = clamp(u.pbr.y, 0.05, 1.0);
    float spec  = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough))
                * (1.0 - rough) * mix(0.25, 1.0, clamp(u.pbr.x, 0.0, 1.0));
    float3 albedo = u.flags.x > 0.5 ? tex.sample(samp, in.uv).rgb * u.color.rgb : u.color.rgb;
    float3 lightIn = amb + lc * (lit ? diff : 0.68 * diff);
    return float4(albedo * lightIn + float3(spec) * lc, 1.0);
}
)";
			id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&error];
			if (lib)
			{
				MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
				desc.vertexFunction   = [lib newFunctionWithName:@"meshPreviewVertex"];
				desc.fragmentFunction = [lib newFunctionWithName:@"meshPreviewFragment"];
				desc.colorAttachments[0].pixelFormat = kSceneColorFormat;
				desc.depthAttachmentPixelFormat      = kDepthFormat;
				id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
				if (pso) m_meshPreviewPipeline = (void*)CFBridgingRetain(pso);
				else
					HE_LOG_ERROR(RHI, "%s",
						(std::string("MetalRenderer: mesh-preview pipeline creation failed: ")
							+ (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
			}
			else
				HE_LOG_ERROR(RHI, "%s", "MetalRenderer: mesh-preview shader compile failed");
		}
	}
	return m_meshPreviewPipeline != nullptr;
}

// The non-graph counterpart: any interleaved pos3/normal3/uv2 buffer, shaded by a
// small lambert + metallic/roughness-driven highlight. Used for mesh thumbnails and
// for materials whose only description is their PBR scalars. Same caller contract.
void MetalRenderer::EncodeMeshPreview(void* renderEncoder, void* vertexBuf, void* indexBuf,
                                      int indexCount, void* texture, const glm::vec3& center,
                                      float extent, const glm::vec3& baseColor, float metallic,
                                      float roughness, float yaw, float pitch, float dist)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	if (!enc || !vertexBuf || !indexBuf || indexCount <= 0) return;
	if (!EnsureMeshPreviewPipeline()) return;

	// Orbit camera auto-framed on the caller's bounds — meshes vary wildly in size
	// and pivot, so `dist` scales the extent rather than being an absolute distance.
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.05f);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 model(1.0f);

	// flags = (hasTexture, camPos) — the shared UnlitUniforms layout has no camera
	// slot of its own and the preview shader is the only consumer of these fields.
	UnlitUniforms u;
	u.mvp   = proj * view * model;
	u.model = model;
	u.color = glm::vec4(baseColor, 1.0f);
	u.flags = glm::vec4(texture ? 1.0f : 0.0f, camPos.x, camPos.y, camPos.z);
	u.pbr   = glm::vec4(metallic, roughness, 0.0f, 0.0f);

	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_meshPreviewPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	[enc setVertexBuffer:(__bridge id<MTLBuffer>)vertexBuf offset:0 atIndex:0];
	[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
	[enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)(texture ? texture : m_dummyTexture) atIndex:0];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
	                indexCount:(NSUInteger)indexCount
	                 indexType:MTLIndexTypeUInt32
	               indexBuffer:(__bridge id<MTLBuffer>)indexBuf
	         indexBufferOffset:0];
}

void* MetalRenderer::RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
                                           uint32_t size, float yaw, float pitch, float dist,
                                           int shape, const HE::UUID& meshId)
{
	const int S = std::clamp(static_cast<int>(size), 32, 1024);
	if (!m_contentManager) m_contentManager = &cm;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue) return nullptr;

	// Nothing to preview for a built-in-PBR material (no node-graph pipeline) — bail
	// out BEFORE allocating the target, so a material without a graph never costs a
	// texture. The thumbnail path handles that case with its own fallback pipeline.
	{
		uint64_t probeKey; std::string probeFrag, probeVert;
		if (!ResolveMaterialShader(materialId, probeKey, probeFrag, probeVert)) return nullptr;
	}

	// ── Lazy / resized target: RGBA16F color (matches the material PSO) + depth.
	if (!m_previewColorTex || m_previewSize != S)
	{
		if (m_previewColorTex) { CFBridgingRelease(m_previewColorTex); m_previewColorTex = nullptr; }
		if (m_previewDepthTex) { CFBridgingRelease(m_previewDepthTex); m_previewDepthTex = nullptr; }
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:S height:S mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_previewColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:S height:S mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_previewDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_previewSize = S;
	}
	id<MTLTexture> colorTex = (__bridge id<MTLTexture>)m_previewColorTex;

	// ── Encode one sphere draw into the preview target.
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = colorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // transparent
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_previewDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	const bool encoded = EncodeMaterialPreview((__bridge void*)enc, materialId, yaw, pitch, dist,
	                                           shape, meshId);
	[enc endEncoding];
	if (!encoded) { [cb commit]; return nullptr; }

	// Headless witness (HE_PREVIEW_DUMP=path): blit to a managed staging texture, read
	// back the RGBA16F, decode halves → PPM. No-op in normal editor use.
	const char* dp = std::getenv("HE_PREVIEW_DUMP");
	id<MTLTexture> staging = nil;
	if (dp && *dp)
	{
		MTLTextureDescriptor* sd2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:S height:S mipmapped:NO];
		sd2.storageMode = MTLStorageModeManaged; sd2.usage = MTLTextureUsageShaderRead;
		staging = [device newTextureWithDescriptor:sd2];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
		           sourceSize:MTLSizeMake(S,S,1) toTexture:staging destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0,0,0)];
		[blit synchronizeResource:staging];
		[blit endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];

	if (staging)
	{
		std::vector<uint16_t> half((size_t)S * S * 4);
		[staging getBytes:half.data() bytesPerRow:S * 4 * sizeof(uint16_t)
		       fromRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0];
		auto h2f = [](uint16_t h) -> float {
			uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
			if (e == 0) { if (m == 0) f = s << 31; else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = (s << 31) | (e << 23) | (m << 13); } }
			else if (e == 0x1f) f = (s << 31) | (0xffu << 23) | (m << 13);
			else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
			float o; std::memcpy(&o, &f, 4); return o;
		};
		if (std::ofstream fo(dp, std::ios::binary); fo)
		{
			fo << "P6\n" << S << " " << S << "\n255\n";
			for (int y = 0; y < S; ++y)
				for (int x = 0; x < S; ++x)
				{
					const uint16_t* pxl = &half[((size_t)y * S + x) * 4];
					for (int c = 0; c < 3; ++c)
					{
						float v = h2f(pxl[c]); v = v < 0 ? 0 : (v > 1 ? 1 : v);
						uint8_t b = (uint8_t)(v * 255.0f + 0.5f);
						fo.write(reinterpret_cast<const char*>(&b), 1);
					}
				}
		}
	}
	return m_previewColorTex; // id<MTLTexture> for ImGui::Image
}

// Lazily (re)create the shared thumbnail target at S×S. Its own textures, see
// the header note: a thumbnail rendered into one of the interactive preview
// targets would replace whatever editor panel is showing that preview.
bool MetalRenderer::EnsureThumbnailTarget(int S)
{
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return false;
	if (!m_thumbColorTex || m_thumbSize != S)
	{
		if (m_thumbColorTex) { CFBridgingRelease(m_thumbColorTex); m_thumbColorTex = nullptr; }
		if (m_thumbDepthTex) { CFBridgingRelease(m_thumbDepthTex); m_thumbDepthTex = nullptr; }
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:S height:S mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_thumbColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:S height:S mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_thumbDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_thumbSize = S;
	}
	return m_thumbColorTex != nullptr;
}

// Blit the thumbnail target into a staging texture on `commandBuffer`, commit,
// wait, and decode the RGBA16F to top-down RGBA8. The target is RGBA16F because
// that is the format every material PSO is built against, so a thumbnail always
// pays this half-float decode.
bool MetalRenderer::CommitAndReadThumbnail(void* commandBuffer, int S, std::vector<uint8_t>& out)
{
	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)commandBuffer;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!cb || !device || !m_thumbColorTex) return false;

	MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
		width:S height:S mipmapped:NO];
	sd.storageMode = MTLStorageModeManaged; sd.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> staging = [device newTextureWithDescriptor:sd];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:(__bridge id<MTLTexture>)m_thumbColorTex
	          sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
	           sourceSize:MTLSizeMake(S,S,1) toTexture:staging destinationSlice:0 destinationLevel:0
	    destinationOrigin:MTLOriginMake(0,0,0)];
	[blit synchronizeResource:staging];
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	std::vector<uint16_t> half((size_t)S * S * 4);
	[staging getBytes:half.data() bytesPerRow:S * 4 * sizeof(uint16_t)
	       fromRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0];
	auto h2f = [](uint16_t h) -> float {
		uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
		if (e == 0) { if (m == 0) f = s << 31; else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = (s << 31) | (e << 23) | (m << 13); } }
		else if (e == 0x1f) f = (s << 31) | (0xffu << 23) | (m << 13);
		else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
		float o; std::memcpy(&o, &f, 4); return o;
	};
	// Metal texture rows are already top-down — no flip, unlike the GL path.
	out.resize((size_t)S * S * 4);
	for (size_t i = 0; i < (size_t)S * S * 4; ++i)
	{
		float v = h2f(half[i]);
		v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
		out[i] = (uint8_t)(v * 255.0f + 0.5f);
	}
	return true;
}

bool MetalRenderer::RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                            const std::vector<ParticlePreviewInstance>& particles,
                                            uint32_t size, std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	if (!m_contentManager) m_contentManager = &cm;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!queue || particles.empty()) return false;
	if (!EnsureParticlePreviewPipeline() || !EnsureThumbnailTarget(S)) return false;

	// Same fixed three-quarter framing as the mesh tiles, so a grid of assets
	// reads as one set. 2.6 leaves the cloud a margin at the 35° FOV.
	// Each particle is a BILLBOARD of radius `size`, so the bounds have to include
	// that radius — framing on the centres alone crops every quad by half its
	// width, which for a tight cloud of large sprites fills the whole frame.
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	for (const auto& p : particles)
	{
		const glm::vec3 r(p.size * 0.5f);
		bmin = glm::min(bmin, p.position - r);
		bmax = glm::max(bmax, p.position + r);
	}
	const bool valid = bmin.x <= bmax.x;
	const glm::vec3 center = valid ? (bmin + bmax) * 0.5f : glm::vec3(0.0f);
	const float extent  = valid ? glm::max(glm::length(bmax - bmin) * 0.5f, 0.1f) : 1.0f;
	const float camDist = 2.9f * std::max(extent, 0.1f);
	const float cp = std::cos(0.45f), sp = std::sin(0.45f);
	const glm::vec3 camPos = center + glm::vec3(std::sin(0.7f) * cp, sp, std::cos(0.7f) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 viewProj = HE::kMetalClipFix * proj * view;
	const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
	const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

	// Built inline at both thumbnail call sites rather than in a helper: this file
	// is ARC, so handing a descriptor back as a raw void* would leave the caller
	// with a reference the helper's scope exit already released.
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_thumbColorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // transparent
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_thumbDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	EncodeParticleBillboards((__bridge void*)enc, materialId, particles, viewProj, camRight, camUp);
	[enc endEncoding];
	return CommitAndReadThumbnail((__bridge void*)cb, S, outRgba8);
}

bool MetalRenderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                          uint32_t size, std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue || uiObjects.empty()) return false;

	// ── Its OWN target, in the SWAPCHAIN format ──────────────────────────────
	// Every pipeline the UI pass uses (m_uiPipeline and the per-material UI
	// pipelines) is built against kSwapchainFormat, and Metal requires the
	// pipeline's colour format to match the render pass's attachment. Drawing UI
	// into the shared RGBA16F thumbnail target silently produced an empty tile.
	if (!m_thumbUIColorTex || m_thumbUISize != S)
	{
		if (m_thumbUIColorTex) { CFBridgingRelease(m_thumbUIColorTex); m_thumbUIColorTex = nullptr; }
		if (m_thumbUIDepthTex) { CFBridgingRelease(m_thumbUIDepthTex); m_thumbUIDepthTex = nullptr; }
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSwapchainFormat
			width:S height:S mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_thumbUIColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:S height:S mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_thumbUIDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_thumbUISize = S;
	}
	if (!m_thumbUIColorTex) return false;

	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_thumbUIColorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_thumbUIDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

	// EncodeUIPass draws m_renderWorld.uiObjects into the given encoder, so the
	// tile borrows that list for one pass and puts the frame's own back.
	std::vector<UIRenderObject> saved;
	saved.swap(m_renderWorld.uiObjects);
	m_renderWorld.uiObjects = uiObjects;
	EncodeUIPass((__bridge void*)enc, S, S);
	m_renderWorld.uiObjects.swap(saved);
	[enc endEncoding];

	// BGRA8 readback — one byte swap rather than the half-float decode the
	// RGBA16F targets need.
	MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSwapchainFormat
		width:S height:S mipmapped:NO];
	sd.storageMode = MTLStorageModeManaged; sd.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> staging = [device newTextureWithDescriptor:sd];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:(__bridge id<MTLTexture>)m_thumbUIColorTex
	          sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
	           sourceSize:MTLSizeMake(S,S,1) toTexture:staging destinationSlice:0 destinationLevel:0
	    destinationOrigin:MTLOriginMake(0,0,0)];
	[blit synchronizeResource:staging];
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	outRgba8.resize((size_t)S * S * 4);
	[staging getBytes:outRgba8.data() bytesPerRow:S * 4
	       fromRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0];
	for (size_t i = 0; i < outRgba8.size(); i += 4)
		std::swap(outRgba8[i], outRgba8[i + 2]);   // BGRA → RGBA
	return true;
}

bool MetalRenderer::RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind,
                                         const HE::UUID& assetId, uint32_t size,
                                         std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	if (!m_contentManager) m_contentManager = &cm;
	if (assetId == HE::UUID{}) return false;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue) return false;
	if (!EnsureThumbnailTarget(S)) return false;

	// Built inline at both thumbnail call sites rather than in a helper: this file
	// is ARC, so handing a descriptor back as a raw void* would leave the caller
	// with a reference the helper's scope exit already released.
	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_thumbColorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // transparent
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_thumbDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	// Fixed three-quarter view — a thumbnail has no orbit interaction, and one
	// shared angle makes a grid of tiles comparable at a glance.
	//
	// The three framing distances all target the same ~90%-of-half-frame fill, so
	// mesh and material tiles sit equally in their cells. They differ because the
	// paths differ in FOV and in what the distance is measured against:
	//   • kMeshFrameDist scales the bounds' half-DIAGONAL, and fitting that into
	//     the mesh path's 35° FOV takes ≥ extent/tan(17.5°) ≈ 3.17·extent.
	//   • the two sphere distances are absolute (unit radius). A sphere's
	//     silhouette fills tan(asin(r/D))/tan(fov/2) of the half-frame, NOT r/D —
	//     the naive form is what left the material tiles cropped: the old 3.1 at
	//     32° works out to 119%, i.e. a fifth of the sphere outside the tile.
	constexpr float kYaw = 0.7f, kPitch = 0.45f;
	constexpr float kMeshFrameDist    = 3.6f;  // × extent, 35° FOV
	constexpr float kMatGraphDist     = 4.0f;  // unit sphere, 32° FOV → 90.0%
	constexpr float kMatFallbackDist  = 3.66f; // unit sphere, 35° FOV → 90.1%

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	bool drew = false;
	if (kind == ThumbnailKind::Material)
	{
		// Graph material → the real shader; otherwise the built-in PBR scalars on
		// the same sphere, so EVERY material asset produces a tile.
		drew = EncodeMaterialPreview((__bridge void*)enc, assetId, kYaw, kPitch, kMatGraphDist, 0 /*sphere*/);
		if (!drew)
		{
			const MaterialAsset* ma = m_contentManager->getMaterial(assetId);
			if (!m_previewVB || m_previewShape != 0)
			{
				// Reuse the material preview's sphere — building it here keeps the
				// fallback independent of whether an interactive preview ever ran.
				if (m_previewVB) { CFBridgingRelease(m_previewVB); m_previewVB = nullptr; }
				if (m_previewIB) { CFBridgingRelease(m_previewIB); m_previewIB = nullptr; }
				std::vector<float> verts; std::vector<uint32_t> idx;
				HE::buildPreviewMesh(0, verts, idx);
				m_previewIdxCount = (int)idx.size();
				m_previewShape    = 0;
				m_previewVB = (void*)CFBridgingRetain([device newBufferWithBytes:verts.data()
					length:verts.size() * sizeof(float) options:MTLResourceStorageModeShared]);
				m_previewIB = (void*)CFBridgingRetain([device newBufferWithBytes:idx.data()
					length:idx.size() * sizeof(uint32_t) options:MTLResourceStorageModeShared]);
			}
			void* baseTex = nullptr;
			if (ma)
			{
				const HE::UUID    tid = ma->textureIds.empty()   ? HE::UUID{}   : ma->textureIds[0];
				const std::string tp  = ma->texturePaths.empty() ? std::string{} : ma->texturePaths[0];
				if (tid != HE::UUID{} || !tp.empty()) baseTex = ResolveGraphTexture(tid, tp);
			}
			// Wider FOV here than the graph path, hence the shorter distance — both
			// land on the same apparent sphere size (see the constants above).
			EncodeMeshPreview((__bridge void*)enc, m_previewVB, m_previewIB, m_previewIdxCount,
				baseTex, glm::vec3(0.0f), 1.0f,
				ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2]) : glm::vec3(0.8f),
				ma ? ma->metallic : 0.0f, ma ? ma->roughness : 0.5f,
				kYaw, kPitch, kMatFallbackDist);
			drew = true;
		}
	}
	else if (kind == ThumbnailKind::StaticMesh)
	{
		if (const GpuMesh* mesh = ResolveMesh(assetId))
		{
			const HE::AABB& b = mesh->localBounds;
			const glm::vec3 c = b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
			const float e = b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
			EncodeMeshPreview((__bridge void*)enc, mesh->vertexBuf, mesh->indexBuf, mesh->indexCount,
				mesh->texture, c, e, glm::vec3(0.78f), 0.0f, 0.55f, kYaw, kPitch, kMeshFrameDist);
			drew = true;
		}
	}
	else if (kind == ThumbnailKind::SkeletalMesh)
	{
		// Bind pose: the stored vertex positions ARE the bind pose, so the plain mesh
		// pipeline is enough — no bone matrices, no skinning shader. Its vertex buffer
		// has the same pos3/normal3/uv2 layout the mesh pipeline expects.
		if (const GpuSkeletalMesh* mesh = ResolveSkeletalMesh(assetId))
		{
			const HE::AABB& b = mesh->localBounds;
			const glm::vec3 c = b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
			const float e = b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
			EncodeMeshPreview((__bridge void*)enc, mesh->vertexBuf, mesh->indexBuf, mesh->indexCount,
				mesh->texture, c, e, glm::vec3(0.78f), 0.0f, 0.55f, kYaw, kPitch, kMeshFrameDist);
			drew = true;
		}
	}
	[enc endEncoding];

	if (!drew) { [cb commit]; return false; }
	return CommitAndReadThumbnail((__bridge void*)cb, S, outRgba8);
}

// Build the minimal skinning pipeline once — own fixed sun+ambient lighting (no
// shadow/SSAO/fog/sky-env), same "isolated preview" idea as the material
// preview. Raw-buffer-indexed (no [[stage_in]]), matching skinnedVertex. Shared
// by the skeletal preview and the world preview.
bool MetalRenderer::EnsureSkelPreviewPipeline()
{
	if (m_skelPreviewPipeline) return true;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return false;
	@autoreleasepool
	{
		{
			NSError* error = nil;
			NSString* src = @R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { packed_float3 position; packed_float3 normal; packed_float2 uv; };
struct Uniforms { float4x4 mvp; float4x4 model; float4 color; float4 flags; float4 pbr;
                  float4 sun; float4 skyAmbient; float4 sunColor; };
struct VOut { float4 position [[position]]; float3 normal; float2 uv; };

vertex VOut skelPreviewVertex(uint vid [[vertex_id]],
                               const device VertexIn* verts       [[buffer(0)]],
                               constant Uniforms&      u          [[buffer(1)]],
                               const device uint4*     boneIds     [[buffer(2)]],
                               const device float4*    boneWeights [[buffer(3)]],
                               constant float4x4*      boneMats    [[buffer(4)]])
{
    uint4  ids  = boneIds[vid];
    float4 wgts = boneWeights[vid];
    float4x4 skin = wgts.x * boneMats[ids.x] + wgts.y * boneMats[ids.y]
                  + wgts.z * boneMats[ids.z] + wgts.w * boneMats[ids.w];
    float4 skinnedPos = skin * float4(float3(verts[vid].position), 1.0);
    float3x3 m3 = float3x3(u.model[0].xyz, u.model[1].xyz, u.model[2].xyz);
    float3x3 s3 = float3x3(skin[0].xyz,    skin[1].xyz,    skin[2].xyz);
    VOut o;
    o.position = u.mvp * skinnedPos;
    o.normal   = normalize(m3 * (s3 * float3(verts[vid].normal)));
    o.uv       = float2(verts[vid].uv);
    return o;
}

fragment float4 skelPreviewFragment(VOut in [[stage_in]],
                                     constant Uniforms& u [[buffer(1)]],
                                     texture2d<float> tex [[texture(0)]],
                                     sampler samp [[sampler(0)]])
{
    bool   lit = u.sun.w > 0.0;
    float3 L   = lit ? normalize(u.sun.xyz) : normalize(float3(0.45, 0.75, 0.55));
    float3 lc  = lit ? u.sunColor.rgb : float3(1.0);
    float3 amb = lit ? u.skyAmbient.rgb : float3(0.35);
    float3 N = normalize(in.normal);
    float diff = max(dot(N, L), 0.0);
    float3 albedo = u.flags.x > 0.5 ? tex.sample(samp, in.uv).rgb * u.color.rgb : u.color.rgb;
    return float4(albedo * (amb + lc * (lit ? diff : 0.65 * diff)), 1.0);
}
)";
			id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&error];
			if (lib)
			{
				MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
				desc.vertexFunction   = [lib newFunctionWithName:@"skelPreviewVertex"];
				desc.fragmentFunction = [lib newFunctionWithName:@"skelPreviewFragment"];
				desc.colorAttachments[0].pixelFormat = kSceneColorFormat;
				desc.depthAttachmentPixelFormat      = kDepthFormat;
				id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
				if (pso) m_skelPreviewPipeline = (void*)CFBridgingRetain(pso);
				else
					HE_LOG_ERROR(RHI, "%s",
						(std::string("MetalRenderer: skeletal-preview pipeline creation failed: ")
							+ (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
			}
			else
				HE_LOG_ERROR(RHI, "%s", "MetalRenderer: skeletal-preview shader compile failed");
		}
	}
	return m_skelPreviewPipeline != nullptr;
}

void* MetalRenderer::RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
                                           const std::vector<glm::mat4>& boneMatrices,
                                           uint32_t width, uint32_t height,
                                           float yaw, float pitch, float dist,
                                           bool showSkeleton,
                                           glm::mat4* outViewProj)
{
	const int W = std::clamp(static_cast<int>(width),  32, 2048);
	const int H = std::clamp(static_cast<int>(height), 32, 2048);
	if (!m_contentManager) m_contentManager = &cm;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue) return nullptr;

	const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(meshId);
	if (!smesh) return nullptr;
	if (!EnsureSkelPreviewPipeline()) return nullptr;

	// ── Lazy / resized target: RGBA16F color (same format as m_debugLinePipeline
	// expects, so the bone overlay below can reuse it verbatim) + depth.
	if (!m_skelPreviewColorTex || m_skelPreviewW != W || m_skelPreviewH != H)
	{
		if (m_skelPreviewColorTex) { CFBridgingRelease(m_skelPreviewColorTex); m_skelPreviewColorTex = nullptr; }
		if (m_skelPreviewDepthTex) { CFBridgingRelease(m_skelPreviewDepthTex); m_skelPreviewDepthTex = nullptr; }
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:W height:H mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_skelPreviewColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:W height:H mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_skelPreviewDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_skelPreviewW = W;
		m_skelPreviewH = H;
	}
	id<MTLTexture> colorTex = (__bridge id<MTLTexture>)m_skelPreviewColorTex;

	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = colorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // transparent
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_skelPreviewDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_skelPreviewPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];

	// ── Orbit camera auto-framed around the mesh's local bounds (arbitrary
	// meshes vary in size/pivot, unlike the material preview's fixed unit shapes).
	const HE::AABB& b = smesh->localBounds;
	const glm::vec3 center = b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
	const float extent = b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.05f);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f),
		static_cast<float>(W) / static_cast<float>(H), 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 model(1.0f);

	// Hand the framing out so a caller can overlay in the same space (model is
	// identity here, so the view-projection is the whole transform).
	if (outViewProj) *outViewProj = proj * view;

	UnlitUniforms u;
	u.mvp   = proj * view * model;
	u.model = model;
	u.color = glm::vec4(0.75f, 0.75f, 0.75f, 1.0f);
	u.flags = glm::vec4(smesh->texture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	u.pbr   = glm::vec4(0.0f);

	constexpr int kMaxBones = 128;
	std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
	const int boneCount = static_cast<int>(std::min(boneMatrices.size(), static_cast<size_t>(kMaxBones)));
	if (boneCount > 0) std::copy_n(boneMatrices.begin(), boneCount, boneScratch.begin());
	id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
	                                            length:kMaxBones * sizeof(glm::mat4)
	                                           options:MTLResourceStorageModeShared];

	[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->vertexBuf  offset:0 atIndex:0];
	[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
	[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneIdBuf  offset:0 atIndex:2];
	[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneWgtBuf offset:0 atIndex:3];
	[enc setVertexBuffer:boneBuf                                   offset:0 atIndex:4];
	[enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)(smesh->texture ? smesh->texture : m_dummyTexture) atIndex:0];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
	                indexCount:(NSUInteger)smesh->indexCount
	                 indexType:MTLIndexTypeUInt32
	               indexBuffer:(__bridge id<MTLBuffer>)smesh->indexBuf
	         indexBufferOffset:0];

	// ── Bone overlay: joint markers + parent→child segments, reusing the
	// existing debug-line pipeline verbatim (same RGBA16F/depth pixel formats).
	// World joint xform = boneMatrix * inverse(inverseBindMatrix), since
	// boneMatrix is defined as globalJointXform * invBind (composeBoneMatrices).
	if (showSkeleton && m_debugLinePipeline)
	{
		if (const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(meshId))
		{
			std::vector<glm::vec3> jointWorld(asset->skeleton.size(), glm::vec3(0.0f));
			for (size_t i = 0; i < asset->skeleton.size(); ++i)
			{
				const glm::mat4 invBind = glm::make_mat4(asset->skeleton[i].inverseBindMatrix.data());
				const glm::mat4 world =
					(i < boneScratch.size() ? boneScratch[i] : glm::mat4(1.0f)) * glm::inverse(invBind);
				jointWorld[i] = glm::vec3(world[3]);
			}

			std::vector<float> lineVerts; // pos3 + color3 per vertex
			const glm::vec3 jointColor(1.0f, 0.85f, 0.15f), boneColor(0.2f, 0.9f, 1.0f);
			auto pushVert = [&](const glm::vec3& p, const glm::vec3& c) {
				lineVerts.insert(lineVerts.end(), { p.x, p.y, p.z, c.x, c.y, c.z });
			};
			const float markerSize = std::max(extent * 0.015f, 0.005f);
			for (size_t i = 0; i < asset->skeleton.size(); ++i)
			{
				const glm::vec3 p = jointWorld[i];
				pushVert(p - glm::vec3(markerSize, 0, 0), jointColor); pushVert(p + glm::vec3(markerSize, 0, 0), jointColor);
				pushVert(p - glm::vec3(0, markerSize, 0), jointColor); pushVert(p + glm::vec3(0, markerSize, 0), jointColor);
				pushVert(p - glm::vec3(0, 0, markerSize), jointColor); pushVert(p + glm::vec3(0, 0, markerSize), jointColor);

				const int32_t parent = asset->skeleton[i].parent;
				if (parent >= 0 && static_cast<size_t>(parent) < jointWorld.size())
				{
					pushVert(jointWorld[parent], boneColor);
					pushVert(p, boneColor);
				}
			}

			if (!lineVerts.empty())
			{
				id<MTLBuffer> lineBuf = [device newBufferWithBytes:lineVerts.data()
				                                             length:lineVerts.size() * sizeof(float)
				                                            options:MTLResourceStorageModeShared];
				const glm::mat4 lineMvp = u.mvp; // same camera — preview-local, no scene NDC clip-fix needed
				[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_debugLinePipeline];
				[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
				[enc setVertexBuffer:lineBuf offset:0 atIndex:0];
				[enc setVertexBytes:&lineMvp length:sizeof(lineMvp) atIndex:1];
				[enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:(NSUInteger)(lineVerts.size() / 6)];
			}
		}
	}

	[enc endEncoding];

	// Headless witness (HE_SKEL_PREVIEW_DUMP=path) — same blit+half-float readback
	// convention as RenderMaterialPreview.
	const char* dp = std::getenv("HE_SKEL_PREVIEW_DUMP");
	id<MTLTexture> staging = nil;
	if (dp && *dp)
	{
		MTLTextureDescriptor* sd2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:W height:H mipmapped:NO];
		sd2.storageMode = MTLStorageModeManaged; sd2.usage = MTLTextureUsageShaderRead;
		staging = [device newTextureWithDescriptor:sd2];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
		           sourceSize:MTLSizeMake(W,H,1) toTexture:staging destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0,0,0)];
		[blit synchronizeResource:staging];
		[blit endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];

	if (staging)
	{
		std::vector<uint16_t> half((size_t)W * H * 4);
		[staging getBytes:half.data() bytesPerRow:W * 4 * sizeof(uint16_t)
		       fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
		auto h2f = [](uint16_t h) -> float {
			uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
			if (e == 0) { if (m == 0) f = s << 31; else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = (s << 31) | (e << 23) | (m << 13); } }
			else if (e == 0x1f) f = (s << 31) | (0xffu << 23) | (m << 13);
			else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
			float o; std::memcpy(&o, &f, 4); return o;
		};
		if (std::ofstream fo(dp, std::ios::binary); fo)
		{
			fo << "P6\n" << W << " " << H << "\n255\n";
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const uint16_t* pxl = &half[((size_t)y * W + x) * 4];
					for (int c = 0; c < 3; ++c)
					{
						float v = h2f(pxl[c]); v = v < 0 ? 0 : (v > 1 ? 1 : v);
						uint8_t b = (uint8_t)(v * 255.0f + 0.5f);
						fo.write(reinterpret_cast<const char*>(&b), 1);
					}
				}
		}
	}
	return m_skelPreviewColorTex; // id<MTLTexture> for ImGui::Image
}

void* MetalRenderer::RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
                                        uint32_t width, uint32_t height,
                                        const EditorCameraOverride& camera,
                                        const glm::vec3& origin,
                                        const WorldPreviewEnv& env,
                                        glm::mat4* outViewProj)
{
	const int W = std::clamp(static_cast<int>(width),  32, 4096);
	const int H = std::clamp(static_cast<int>(height), 32, 4096);
	if (!m_contentManager) m_contentManager = &cm;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue) return nullptr;
	if (!EnsureSkelPreviewPipeline()) return nullptr;
	if (!EnsureMeshPreviewPipeline()) return nullptr;

	// ── Camera: the caller's, verbatim. Only the projection is built here,
	// because only here is the target's aspect known — through the shared rule,
	// so the panel drawing gizmos over this picture builds the same matrix
	// instead of a lookalike.
	const float aspect = static_cast<float>(W) / static_cast<float>(H);
	const glm::vec3 camPos = camera.position;
	const glm::mat4 view = camera.view;
	const glm::mat4 proj = worldPreviewProjection(camera, aspect);
	const glm::mat4 viewProj = proj * view;
	if (outViewProj) *outViewProj = viewProj;

	// ── Snapshot the caller's world. Its OWN extractor, not m_extractor: that
	// one carries the main scene's day-night state (setDayNight), and a preview
	// inheriting the scene's sunset tint would be a puzzling surprise.
	RenderExtractor previewExtractor;
	previewExtractor.setContentManager(m_contentManager);
	// One description of the preview's sky, built the way the editor and the
	// packaged game build theirs. Hand-assembling it here would be a fourth copy
	// of "what does a default sky look like" and would drift from the Sky panel.
	IRenderer::EnvironmentSettings previewEnvSettings{};
	if (env.sky)
	{
		previewEnvSettings = HE::makeWorldPreviewEnvironment(env.timeOfDay, env.cloudCoverage);
		// The sun is not computed here either: setDayNight puts it where the
		// SCENE would have it at this hour, so preview and level agree about
		// what 0.75 looks like — and it reads the same numbers the sky pixels do.
		previewExtractor.setDayNight(true, env.timeOfDay,
		                             previewEnvSettings.sunColor, previewEnvSettings.sunIntensity,
		                             previewEnvSettings.moonColor, previewEnvSettings.moonIntensity,
		                             previewEnvSettings.cloudCoverage);
	}
	EditorCameraOverride previewCam = camera;
	previewCam.active = true;
	RenderWorld snapshot;
	previewExtractor.extract(world, snapshot, aspect, &previewCam);

	// Lighting from the extracted sun. dominantDirectionalLight, NOT
	// sunDirection: the latter is the SKY-DOME sun and sits below the horizon at
	// night, which would light the mesh from underneath. The dome itself does
	// want the sky sun — that is the one it draws.
	glm::vec4 sunUniform(0.0f);
	glm::vec3 sunColor(1.0f), ambient(0.0f);
	if (env.sky)
	{
		glm::vec3 toward(0.0f, 1.0f, 0.0f), colorIntensity(0.0f);
		if (snapshot.dominantDirectionalLight(toward, colorIntensity))
		{
			sunUniform = glm::vec4(toward, 1.0f);
			sunColor   = colorIntensity;
		}
		else
		{
			// Night with nothing shining: arm the uniform anyway (w > 0) so the
			// ambient below is what lights the mesh, instead of the studio light
			// snapping back on and making midnight look like noon.
			sunUniform = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			sunColor   = glm::vec3(0.0f);
		}
		// A floor under the extractor's ambient: a mesh viewer that goes fully
		// black at midnight is a viewer you cannot use at midnight.
		ambient = glm::max(snapshot.ambient, glm::vec3(0.10f, 0.11f, 0.13f));
	}

	// ── Lazy / resized targets. TWO of them, mirroring the scene: the pass
	// renders HDR (the sky's radiance and a sun at intensity 2.2 both run well
	// past 1.0), then a tonemap resolves that into the LDR texture ImGui shows.
	// Handing ImGui the raw HDR texture is what made the first sky-lit preview a
	// uniformly white mesh under a blown-out sky.
	if (!m_worldPreviewColorTex || m_worldPreviewW != W || m_worldPreviewH != H)
	{
		if (m_worldPreviewColorTex) { CFBridgingRelease(m_worldPreviewColorTex); m_worldPreviewColorTex = nullptr; }
		if (m_worldPreviewHdrTex)   { CFBridgingRelease(m_worldPreviewHdrTex);   m_worldPreviewHdrTex = nullptr; }
		if (m_worldPreviewDepthTex) { CFBridgingRelease(m_worldPreviewDepthTex); m_worldPreviewDepthTex = nullptr; }
		MTLTextureDescriptor* hd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:W height:H mipmapped:NO];
		hd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		hd.storageMode = MTLStorageModePrivate;
		m_worldPreviewHdrTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:hd]);
		// LDR in the SWAPCHAIN format, because that is what the tonemap pipeline
		// targets — a pipeline's colour format must match its pass's attachment.
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSwapchainFormat
			width:W height:H mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_worldPreviewColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:W height:H mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_worldPreviewDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_worldPreviewW = W;
		m_worldPreviewH = H;
	}
	id<MTLTexture> colorTex = (__bridge id<MTLTexture>)m_worldPreviewColorTex;

	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_worldPreviewHdrTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	// Studio gray — covered by the sky when there is one. LINEAR: this is resolved
	// through ACES + gamma below, which lifts it a long way (see kPreviewBackground).
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(HE::kPreviewBackground[0],
	                                                       HE::kPreviewBackground[1],
	                                                       HE::kPreviewBackground[2], 1.0);
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_worldPreviewDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];

	// ── Sky FIRST, unlike the scene, which draws it last to skip the shading
	// behind solid geometry. Here the ground must not occlude the mesh (see
	// below), so it writes no depth — and a sky drawn afterwards would then
	// paint straight over it. A preview target is small; the overdraw is not
	// worth the ordering trap.
	if (env.sky)
	{
		float skyClock = 0.0f;
		if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov)
			skyClock = static_cast<float>(std::atof(ov));
		EncodeSky((__bridge void*)enc, glm::inverse(viewProj), snapshot.sunDirection,
		          skyClock, previewEnvSettings, camPos, /*lowResClouds=*/false);
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	}

	// ── Grid + origin marker. LINES ONLY — a filled floor in a viewer is only
	// ever in the way, hiding the underside of the mesh and, with a sky on, the
	// entire lower half of the world. Drawn through the debug-line pipeline,
	// which is exactly a pos3+color3 vertex buffer with an MVP.
	if (env.grid && m_debugLinePipeline)
	{
		const float halfExtent = std::clamp(
			std::ceil(glm::length(camPos - origin) * 2.0f), 10.0f, 200.0f);
		std::vector<float> verts;
		HE::buildPreviewGrid(halfExtent, 1.0f, verts, origin);

		id<MTLBuffer> lineBuf = [device newBufferWithBytes:verts.data()
		                                            length:verts.size() * sizeof(float)
		                                           options:MTLResourceStorageModeShared];
		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_debugLinePipeline];
		// No depth WRITE, so the lines cannot occlude the object either.
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
		[enc setVertexBuffer:lineBuf offset:0 atIndex:0];
		[enc setVertexBytes:&viewProj length:sizeof(viewProj) atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0
		        vertexCount:(NSUInteger)(verts.size() / 6)];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	}

	// ── Static meshes.
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_meshPreviewPipeline];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	for (const RenderObject& obj : snapshot.objects)
	{
		const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		if (!mesh || !mesh->vertexBuf || !mesh->indexBuf || mesh->indexCount <= 0) continue;
		// flags = (hasTexture, camPos) — the shared UnlitUniforms layout has no
		// camera slot of its own, matching EncodeMeshPreview.
		UnlitUniforms u;
		u.mvp   = viewProj * obj.transform;
		u.model = obj.transform;
		u.color = glm::vec4(obj.baseColor, 1.0f);
		u.flags = glm::vec4(mesh->texture ? 1.0f : 0.0f, camPos.x, camPos.y, camPos.z);
		u.pbr   = glm::vec4(obj.metallic, obj.roughness, 0.0f, 0.0f);
		u.sun        = sunUniform;
		u.sunColor   = glm::vec4(sunColor, 1.0f);
		u.skyAmbient = glm::vec4(ambient, 1.0f);
		[enc setVertexBuffer:(__bridge id<MTLBuffer>)mesh->vertexBuf offset:0 atIndex:0];
		[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
		[enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
		[enc setFragmentTexture:(__bridge id<MTLTexture>)(mesh->texture ? mesh->texture : m_dummyTexture) atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                indexCount:(NSUInteger)mesh->indexCount
		                 indexType:MTLIndexTypeUInt32
		               indexBuffer:(__bridge id<MTLBuffer>)mesh->indexBuf
		         indexBufferOffset:0];
	}

	// ── Skinned meshes (the pose the AnimatorHost last wrote, or the bind pose).
	constexpr int kMaxBones = 128;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_skelPreviewPipeline];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	for (const SkinnedRenderObject& obj : snapshot.skinnedObjects)
	{
		const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(obj.meshAssetId);
		if (!smesh || !smesh->vertexBuf || !smesh->indexBuf || smesh->indexCount <= 0) continue;
		std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
		const int boneCount = static_cast<int>(std::min(obj.boneMatrices.size(),
		                                                static_cast<size_t>(kMaxBones)));
		if (boneCount > 0) std::copy_n(obj.boneMatrices.begin(), boneCount, boneScratch.begin());
		id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
		                                            length:kMaxBones * sizeof(glm::mat4)
		                                           options:MTLResourceStorageModeShared];
		UnlitUniforms u;
		u.mvp   = viewProj * obj.transform;
		u.model = obj.transform;
		u.color = glm::vec4(obj.baseColor, 1.0f);
		u.flags = glm::vec4(smesh->texture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		u.pbr   = glm::vec4(0.0f);
		u.sun        = sunUniform;
		u.sunColor   = glm::vec4(sunColor, 1.0f);
		u.skyAmbient = glm::vec4(ambient, 1.0f);
		[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->vertexBuf  offset:0 atIndex:0];
		[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
		[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneIdBuf  offset:0 atIndex:2];
		[enc setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneWgtBuf offset:0 atIndex:3];
		[enc setVertexBuffer:boneBuf                                   offset:0 atIndex:4];
		[enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
		[enc setFragmentTexture:(__bridge id<MTLTexture>)(smesh->texture ? smesh->texture : m_dummyTexture) atIndex:0];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                indexCount:(NSUInteger)smesh->indexCount
		                 indexType:MTLIndexTypeUInt32
		               indexBuffer:(__bridge id<MTLBuffer>)smesh->indexBuf
		         indexBufferOffset:0];
	}

	[enc endEncoding];

	// ── Tonemap resolve: HDR → the texture ImGui shows, through the very same
	// pipeline the scene's frame ends with. Its own encoder, because it targets
	// a different attachment; the tonemap pipeline was built with a depth format,
	// so the pass still needs a depth attachment even though it ignores it.
	{
		MTLRenderPassDescriptor* tp = [MTLRenderPassDescriptor renderPassDescriptor];
		tp.colorAttachments[0].texture     = colorTex;
		tp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		tp.colorAttachments[0].storeAction = MTLStoreActionStore;
		tp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_worldPreviewDepthTex;
		tp.depthAttachment.loadAction  = MTLLoadActionDontCare;
		tp.depthAttachment.storeAction = MTLStoreActionDontCare;
		id<MTLRenderCommandEncoder> tenc = [cb renderCommandEncoderWithDescriptor:tp];
		EncodeTonemap((__bridge void*)tenc, m_worldPreviewHdrTex, /*withBloom=*/false);
		[tenc endEncoding];
	}

	// Headless witness (HE_WORLD_PREVIEW_DUMP=path). Reads the LDR result — what
	// the editor actually shows — so the channel order is the swapchain's (BGRA).
	const char* dp = std::getenv("HE_WORLD_PREVIEW_DUMP");
	id<MTLTexture> staging = nil;
	if (dp && *dp)
	{
		MTLTextureDescriptor* sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSwapchainFormat
			width:W height:H mipmapped:NO];
		sd.storageMode = MTLStorageModeManaged; sd.usage = MTLTextureUsageShaderRead;
		staging = [device newTextureWithDescriptor:sd];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
		           sourceSize:MTLSizeMake(W,H,1) toTexture:staging destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0,0,0)];
		[blit synchronizeResource:staging];
		[blit endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];

	if (staging)
	{
		std::vector<uint8_t> bgra((size_t)W * H * 4);
		[staging getBytes:bgra.data() bytesPerRow:W * 4
		       fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
		if (std::ofstream fo(dp, std::ios::binary); fo)
		{
			fo << "P6\n" << W << " " << H << "\n255\n";
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const uint8_t* pxl = &bgra[((size_t)y * W + x) * 4];
					const uint8_t rgb[3] = { pxl[2], pxl[1], pxl[0] }; // BGRA → RGB
					fo.write(reinterpret_cast<const char*>(rgb), 3);
				}
		}
	}
	return m_worldPreviewColorTex; // id<MTLTexture> for ImGui::Image
}

// Encode the particle cloud into an already-open encoder. Shared by the
// interactive preview and the Content-Browser thumbnail so the two can never
// drift; the thumbnail must NOT reuse the preview's own target or it would
// overwrite what the Particle Graph Editor is showing.
void MetalRenderer::EncodeParticleBillboards(void* renderEncoder, const HE::UUID& materialId,
                                             const std::vector<ParticlePreviewInstance>& particles,
                                             const glm::mat4& viewProj, const glm::vec3& camRight,
                                             const glm::vec3& camUp)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!enc || !device || particles.empty() || !m_particlePreviewPipeline) return;

	std::vector<float> inst;
	inst.reserve(particles.size() * 8);
	for (const auto& p : particles)
		inst.insert(inst.end(), { p.position.x, p.position.y, p.position.z, p.size,
		                          p.color.r, p.color.g, p.color.b, p.alpha });
	id<MTLBuffer> instBuf = [device newBufferWithBytes:inst.data()
	                                            length:inst.size() * sizeof(float)
	                                           options:MTLResourceStorageModeShared];

	void* matTex = nullptr;
	const bool hasTex = ResolveMaterialTexture(materialId, matTex);

	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_particlePreviewPipeline];
	[enc setVertexBuffer:instBuf offset:0 atIndex:0];
	[enc setVertexBytes:&viewProj length:sizeof(viewProj) atIndex:1];
	[enc setVertexBytes:&camRight length:sizeof(camRight) atIndex:2];
	[enc setVertexBytes:&camUp    length:sizeof(camUp)    atIndex:3];
	bool hasTexFlag = hasTex;
	[enc setFragmentBytes:&hasTexFlag length:sizeof(hasTexFlag) atIndex:1];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)(hasTex ? matTex : m_dummyTexture) atIndex:0];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
	      instanceCount:static_cast<NSUInteger>(particles.size())];
}

// Build the billboard pipeline once. Split out so both the interactive preview
// and the Content-Browser thumbnail can reach it.
bool MetalRenderer::EnsureParticlePreviewPipeline()
{
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return false;
	// ── Lazy billboard pipeline — camera-facing quads via vertex_id (no vertex
	// buffer for the corners), alpha-blended (unlike the opaque skeletal preview).
	if (!m_particlePreviewPipeline)
	{
		@autoreleasepool
		{
			NSError* error = nil;
			NSString* src = @R"(
#include <metal_stdlib>
using namespace metal;
struct Instance { packed_float3 pos; float size; packed_float3 color; float alpha; };
struct VOut { float4 position [[position]]; float3 color; float alpha; float2 uv; };

constant float2 kCorners[6] = {
    float2(-1,-1), float2(1,-1), float2(1,1),
    float2(-1,-1), float2(1,1),  float2(-1,1)
};

vertex VOut particlePreviewVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                   const device Instance* insts [[buffer(0)]],
                                   constant float4x4& viewProj [[buffer(1)]],
                                   constant float3&   camRight [[buffer(2)]],
                                   constant float3&   camUp    [[buffer(3)]])
{
    Instance inst = insts[iid];
    float2 corner = kCorners[vid % 6];
    float3 worldPos = float3(inst.pos) + (camRight * corner.x + camUp * corner.y) * (inst.size * 0.5);
    VOut o;
    o.position = viewProj * float4(worldPos, 1.0);
    o.color = float3(inst.color);
    o.alpha = inst.alpha;
    o.uv = corner * 0.5 + 0.5;
    return o;
}

fragment float4 particlePreviewFragment(VOut in [[stage_in]],
                                         constant bool& hasTex [[buffer(1)]],
                                         texture2d<float> tex [[texture(0)]],
                                         sampler samp [[sampler(0)]])
{
    float4 texc  = hasTex ? tex.sample(samp, in.uv) : float4(1.0);
    float  shape = hasTex ? texc.a : smoothstep(1.0, 0.0, length(in.uv * 2.0 - 1.0));
    return float4(in.color * texc.rgb, in.alpha * shape);
}
)";
			id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&error];
			if (lib)
			{
				MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
				desc.vertexFunction   = [lib newFunctionWithName:@"particlePreviewVertex"];
				desc.fragmentFunction = [lib newFunctionWithName:@"particlePreviewFragment"];
				desc.colorAttachments[0].pixelFormat = kSceneColorFormat;
				desc.depthAttachmentPixelFormat      = kDepthFormat;
				desc.colorAttachments[0].blendingEnabled             = YES;
				desc.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
				desc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
				desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
				desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
				desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
				desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
				id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
				if (pso) m_particlePreviewPipeline = (void*)CFBridgingRetain(pso);
				else
					HE_LOG_ERROR(RHI, "%s",
						(std::string("MetalRenderer: particle-preview pipeline creation failed: ")
							+ (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
			}
			else
				HE_LOG_ERROR(RHI, "%s", "MetalRenderer: particle-preview shader compile failed");
		}
	}
	return m_particlePreviewPipeline != nullptr;
}

void* MetalRenderer::RenderParticlePreview(ContentManager& cm, const HE::UUID& /*meshId*/,
                                           const HE::UUID& materialId,
                                           const std::vector<ParticlePreviewInstance>& particles,
                                           uint32_t size, float yaw, float pitch, float dist)
{
	const int S = std::clamp(static_cast<int>(size), 32, 1024);
	if (!m_contentManager) m_contentManager = &cm;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_commandQueue;
	if (!device || !queue) return nullptr;
	if (!EnsureParticlePreviewPipeline()) return nullptr;

	// ── Lazy / resized target.
	if (!m_particlePreviewColorTex || m_particlePreviewSize != S)
	{
		if (m_particlePreviewColorTex) { CFBridgingRelease(m_particlePreviewColorTex); m_particlePreviewColorTex = nullptr; }
		if (m_particlePreviewDepthTex) { CFBridgingRelease(m_particlePreviewDepthTex); m_particlePreviewDepthTex = nullptr; }
		MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:S height:S mipmapped:NO];
		cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		cd.storageMode = MTLStorageModePrivate;
		m_particlePreviewColorTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:cd]);
		MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
			width:S height:S mipmapped:NO];
		dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
		m_particlePreviewDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:dd]);
		m_particlePreviewSize = S;
	}
	id<MTLTexture> colorTex = (__bridge id<MTLTexture>)m_particlePreviewColorTex;

	// ── Orbit camera auto-framed around the LIVE particles' bounds.
	// Each particle is a BILLBOARD of radius `size`, so the bounds have to include
	// that radius — framing on the centres alone crops every quad by half its
	// width, which for a tight cloud of large sprites fills the whole frame.
	glm::vec3 bmin(1e30f), bmax(-1e30f);
	for (const auto& p : particles)
	{
		const glm::vec3 r(p.size * 0.5f);
		bmin = glm::min(bmin, p.position - r);
		bmax = glm::max(bmax, p.position + r);
	}
	const bool valid = !particles.empty() && bmin.x <= bmax.x;
	const glm::vec3 center = valid ? (bmin + bmax) * 0.5f : glm::vec3(0.0f);
	const float extent = valid ? glm::max(glm::length(bmax - bmin) * 0.5f, 0.1f) : 1.0f;
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.1f);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 viewProj = HE::kMetalClipFix * proj * view;
	const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
	const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

	MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture     = colorTex;
	rp.colorAttachments[0].loadAction  = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	rp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_particlePreviewDepthTex;
	rp.depthAttachment.loadAction  = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth  = 1.0;

	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	EncodeParticleBillboards((__bridge void*)enc, materialId, particles, viewProj, camRight, camUp);
	[enc endEncoding];

	const char* dp = std::getenv("HE_PARTICLE_PREVIEW_DUMP");
	id<MTLTexture> staging = nil;
	if (dp && *dp)
	{
		MTLTextureDescriptor* sd2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:S height:S mipmapped:NO];
		sd2.storageMode = MTLStorageModeManaged; sd2.usage = MTLTextureUsageShaderRead;
		staging = [device newTextureWithDescriptor:sd2];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:colorTex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
		           sourceSize:MTLSizeMake(S,S,1) toTexture:staging destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0,0,0)];
		[blit synchronizeResource:staging];
		[blit endEncoding];
	}
	[cb commit];
	[cb waitUntilCompleted];

	if (staging)
	{
		std::vector<uint16_t> half((size_t)S * S * 4);
		[staging getBytes:half.data() bytesPerRow:S * 4 * sizeof(uint16_t)
		       fromRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0];
		auto h2f = [](uint16_t h) -> float {
			uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
			if (e == 0) { if (m == 0) f = s << 31; else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3ff; f = (s << 31) | (e << 23) | (m << 13); } }
			else if (e == 0x1f) f = (s << 31) | (0xffu << 23) | (m << 13);
			else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
			float o; std::memcpy(&o, &f, 4); return o;
		};
		if (std::ofstream fo(dp, std::ios::binary); fo)
		{
			fo << "P6\n" << S << " " << S << "\n255\n";
			for (int y = 0; y < S; ++y)
				for (int x = 0; x < S; ++x)
				{
					const uint16_t* pxl = &half[((size_t)y * S + x) * 4];
					for (int c = 0; c < 3; ++c)
					{
						float v = h2f(pxl[c]); v = v < 0 ? 0 : (v > 1 ? 1 : v);
						uint8_t b = (uint8_t)(v * 255.0f + 0.5f);
						fo.write(reinterpret_cast<const char*>(&b), 1);
					}
				}
		}
	}
	return m_particlePreviewColorTex;
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
	layer.opaque             = YES; // game window fully covers the framebuffer —
	                                // ignore the alpha channel so transparent UI
	                                // (glyph gaps, rounded corners) shows the scene
	                                // behind it, not a black alpha hole.
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

bool MetalRenderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
	if (!m_viewportColor || !m_device || !m_commandQueue) return false;

	@autoreleasepool
	{
		id<MTLTexture> src = (__bridge id<MTLTexture>)m_viewportColor;
		const NSUInteger w = src.width, h = src.height;
		if (w == 0 || h == 0) return false;

		id<MTLDevice>       device = (__bridge id<MTLDevice>)m_device;
		id<MTLCommandQueue> queue  = (__bridge id<MTLCommandQueue>)m_commandQueue;

		// Private render-target textures can't be read on the CPU; blit into a
		// managed staging texture, synchronise it, then read it back.
		MTLTextureDescriptor* desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kSwapchainFormat width:w height:h mipmapped:NO];
		desc.storageMode = MTLStorageModeManaged;
		desc.usage       = MTLTextureUsageShaderRead;
		id<MTLTexture> staging = [device newTextureWithDescriptor:desc];
		if (!staging) return false;

		id<MTLCommandBuffer>      cb   = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:src
		          sourceSlice:0 sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(w, h, 1)
		            toTexture:staging
		     destinationSlice:0 destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit synchronizeTexture:staging slice:0 level:0];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];

		const NSUInteger rowBytes = w * 4;
		std::vector<uint8_t> bgra(static_cast<size_t>(rowBytes) * h);
		[staging getBytes:bgra.data()
		      bytesPerRow:rowBytes
		       fromRegion:MTLRegionMake2D(0, 0, w, h)
		      mipmapLevel:0];

		// kSwapchainFormat is BGRA8; the caller wants RGBA8. Metal textures are
		// top-row-first already, so no vertical flip is needed.
		rgba.resize(static_cast<size_t>(rowBytes) * h);
		for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
		{
			rgba[i * 4 + 0] = bgra[i * 4 + 2];
			rgba[i * 4 + 1] = bgra[i * 4 + 1];
			rgba[i * 4 + 2] = bgra[i * 4 + 0];
			rgba[i * 4 + 3] = bgra[i * 4 + 3];
		}
		width  = static_cast<uint32_t>(w);
		height = static_cast<uint32_t>(h);
		return true;
	}
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

// Same deferred-release problem as RetireTexture above (GPU work may still
// reference the object this frame's build just replaced), generalised to any
// retained Objective-C object — used for the GI TLAS/instance buffer, which are
// reallocated fresh every GI-active frame rather than mutated in place.
void MetalRenderer::RetireGIObject(void* obj)
{
	if (!obj) return;
	m_retiredGIObjects.push_back({ obj, 3 });
}

void MetalRenderer::AgeRetiredGIObjects()
{
	for (auto it = m_retiredGIObjects.begin(); it != m_retiredGIObjects.end(); )
	{
		if (--it->framesLeft <= 0)
		{
			CFBridgingRelease(it->object);
			it = m_retiredGIObjects.erase(it);
		}
		else
			++it;
	}
}

void MetalRenderer::DrainRetiredGIObjects()
{
	for (auto& r : m_retiredGIObjects)
		CFBridgingRelease(r.object);
	m_retiredGIObjects.clear();
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

void MetalRenderer::EnsureHDRTarget(int width, int height)
{
	if (m_hdrColor && width == m_hdrW && height == m_hdrH)
		return;
	DestroyHDRTarget();

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSceneColorFormat width:width height:height mipmapped:NO];
	colorDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	colorDesc.storageMode = MTLStorageModePrivate;
	m_hdrColor = (void*)CFBridgingRetain([device newTextureWithDescriptor:colorDesc]);

	MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kDepthFormat width:width height:height mipmapped:NO];
	depthDesc.usage       = MTLTextureUsageRenderTarget;
	depthDesc.storageMode = MTLStorageModePrivate;
	m_hdrDepth = (void*)CFBridgingRetain([device newTextureWithDescriptor:depthDesc]);

	m_hdrW = width;
	m_hdrH = height;
}

void MetalRenderer::DestroyHDRTarget()
{
	if (m_hdrColor) { CFBridgingRelease(m_hdrColor); m_hdrColor = nullptr; }
	if (m_hdrDepth) { CFBridgingRelease(m_hdrDepth); m_hdrDepth = nullptr; }
	m_hdrW = m_hdrH = 0;
}

void MetalRenderer::EnsureGBufferTargets(int width, int height)
{
	// Tile mode keeps the G-buffer memoryless — EXCEPT when SSR or the
	// ray-traced GI reflections run this frame: their trace/composite passes
	// sample the stored attachments after the pass, so they must exist in DRAM
	// (inherent to any technique that reads the G-buffer post-pass).
	const bool stored = !m_deferredTileMode || m_ssrFrameActive || m_giReflFrameActive;
	if (m_gbColor0 && width == m_gbW && height == m_gbH && stored == m_gbStored)
		return;
	DestroyGBufferTargets();
	m_gbStored = stored;

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	auto makeColor = [&](MTLPixelFormat fmt) -> void*
	{
		MTLTextureDescriptor* d = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:fmt width:width height:height mipmapped:NO];
		d.usage       = stored ? (MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
		                       : MTLTextureUsageRenderTarget;
		d.storageMode = stored ? MTLStorageModePrivate : MTLStorageModeMemoryless;
		return (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	};
	m_gbColor0   = makeColor(kGBuf0Format);
	m_gbColor1   = makeColor(kGBufAttrFormat);
	m_gbColor2   = makeColor(kGBufAttrFormat);
	// NDC depth as attachment 3 (R32F): the tile resolve framebuffer-fetches it;
	// the two-pass fallback attaches it only for pipeline/pass format parity.
	m_gbDepthLin = makeColor(MTLPixelFormatR32Float);
	if (!m_deferredTileMode)
	{
		// Depth is SAMPLED by the two-pass resolve (world-pos reconstruction) —
		// unlike m_hdrDepth it needs ShaderRead on top of RenderTarget. The tile
		// mode has no separate G-buffer depth: its pass writes m_hdrDepth
		// directly and the resolve reads m_gbDepthLin from tile storage.
		MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kDepthFormat width:width height:height mipmapped:NO];
		depthDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		depthDesc.storageMode = MTLStorageModePrivate;
		m_gbDepth = (void*)CFBridgingRetain([device newTextureWithDescriptor:depthDesc]);
	}

	m_gbW = width;
	m_gbH = height;
}

void MetalRenderer::DestroyGBufferTargets()
{
	if (m_gbColor0)   { CFBridgingRelease(m_gbColor0);   m_gbColor0 = nullptr; }
	if (m_gbColor1)   { CFBridgingRelease(m_gbColor1);   m_gbColor1 = nullptr; }
	if (m_gbColor2)   { CFBridgingRelease(m_gbColor2);   m_gbColor2 = nullptr; }
	if (m_gbDepthLin) { CFBridgingRelease(m_gbDepthLin); m_gbDepthLin = nullptr; }
	if (m_gbDepth)    { CFBridgingRelease(m_gbDepth);    m_gbDepth  = nullptr; }
	m_gbW = m_gbH = 0;
}

bool MetalRenderer::EnsureDeferredPipelines()
{
	// Tile mode builds m_deferredResolveTilePipeline INSTEAD of the sampling
	// resolve — check the one this mode actually uses, or the second frame
	// falls back to forward forever.
	if (m_gbufferPipeline &&
	    (m_deferredTileMode ? m_deferredResolveTilePipeline != nullptr
	                        : m_deferredResolvePipeline != nullptr))
		return true;
	if (m_deferredPipelinesTried) return false; // failed once — stay forward, don't retry per frame
	m_deferredPipelinesTried = true;

#if !defined(HE_HAVE_SHADERC)
	// The resolve shader is generated from the shared lighting preamble at
	// runtime — without the cross-compiler there is no deferred path.
	return false;
#else
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (!device) return false;
		NSError* err = nil;

		// Built-in-PBR G-buffer pipeline: same library as the scene pipeline
		// (vertexMain + gbufferMain live in kUnlitMSL).
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:injectSkyMSL(kUnlitMSL).c_str()] options:nil error:&err];
		if (lib)
		{
			MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
			desc.vertexFunction   = [lib newFunctionWithName:@"vertexMain"];
			desc.fragmentFunction = [lib newFunctionWithName:@"gbufferMain"];
			desc.colorAttachments[0].pixelFormat = kGBuf0Format;
			desc.colorAttachments[1].pixelFormat = kGBufAttrFormat;
			desc.colorAttachments[2].pixelFormat = kGBufAttrFormat;
			desc.colorAttachments[3].pixelFormat = MTLPixelFormatR32Float; // NDC depth (P6 fetch)
			if (m_deferredTileMode)
			{
				// Single pass: the HDR target rides as attachment 4; the G-buffer
				// stage never writes it.
				desc.colorAttachments[4].pixelFormat = kSceneColorFormat;
				desc.colorAttachments[4].writeMask   = MTLColorWriteMaskNone;
			}
			desc.depthAttachmentPixelFormat      = kDepthFormat;
			id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
			if (pso) m_gbufferPipeline = (void*)CFBridgingRetain(pso);

			// GPU-instanced twin, built from the SAME descriptor — so the tile
			// mode's fifth attachment above is carried over rather than
			// duplicated, and Metal accepts the bind inside the running encoder.
			desc.vertexFunction = [lib newFunctionWithName:@"vertexMainInstanced"];
			NSError* iErr = nil;
			id<MTLRenderPipelineState> instPso =
				[device newRenderPipelineStateWithDescriptor:desc error:&iErr];
			if (instPso) m_gbufferInstancedPipeline = (void*)CFBridgingRetain(instPso);
			else
				HE_LOG_WARN(RHI, "%s",
					(std::string("MetalRenderer: instanced G-buffer pipeline build failed — "
					             "batches stay on the per-instance loop: ")
					 + (iErr ? iErr.localizedDescription.UTF8String : "?")).c_str());
		}

		// Fullscreen lighting resolve: cross-compiled from the SAME lighting
		// preamble as every material fragment (one shading source, §4.2). Tile
		// mode uses the framebuffer-fetch variant inside the shared 5-attachment
		// pass; the fallback samples the stored G-buffer in its own pass.
		using Backend = HE::MaterialShaderLibrary::Backend;
		const auto& v = m_matShaderLib.fullscreenVertex(Backend::Metal);
		const auto& f = m_deferredTileMode
			? (m_deferredClustered ? m_matShaderLib.deferredResolveTileClustered(Backend::Metal)
			                       : m_matShaderLib.deferredResolveTile(Backend::Metal))
			: (m_deferredClustered ? m_matShaderLib.deferredResolveClustered(Backend::Metal)
			                       : m_matShaderLib.deferredResolve(Backend::Metal));
		if (v.ok && f.ok)
		{
			NSError* verr = nil;
			id<MTLLibrary> vLib = [device newLibraryWithSource:
				[NSString stringWithUTF8String:v.source.c_str()] options:nil error:&verr];
			id<MTLLibrary> fLib = verr ? nil : [device newLibraryWithSource:
				[NSString stringWithUTF8String:f.source.c_str()] options:nil error:&verr];
			if (vLib && fLib)
			{
				MTLRenderPipelineDescriptor* rdesc = [[MTLRenderPipelineDescriptor alloc] init];
				rdesc.vertexFunction   = [vLib newFunctionWithName:@"main0"];
				rdesc.fragmentFunction = [fLib newFunctionWithName:@"main0"];
				if (m_deferredTileMode)
				{
					// Shared single pass: reads attachments 0..3 (fetch), writes 4.
					rdesc.colorAttachments[0].pixelFormat = kGBuf0Format;
					rdesc.colorAttachments[1].pixelFormat = kGBufAttrFormat;
					rdesc.colorAttachments[2].pixelFormat = kGBufAttrFormat;
					rdesc.colorAttachments[3].pixelFormat = MTLPixelFormatR32Float;
					for (int a = 0; a < 4; ++a)
						rdesc.colorAttachments[a].writeMask = MTLColorWriteMaskNone;
					rdesc.colorAttachments[4].pixelFormat = kSceneColorFormat;
				}
				else
					rdesc.colorAttachments[0].pixelFormat = kSceneColorFormat;
				rdesc.depthAttachmentPixelFormat      = kDepthFormat;
				id<MTLRenderPipelineState> rpso = [device newRenderPipelineStateWithDescriptor:rdesc error:&verr];
				if (rpso)
				{
					if (m_deferredTileMode) m_deferredResolveTilePipeline = (void*)CFBridgingRetain(rpso);
					else                    m_deferredResolvePipeline     = (void*)CFBridgingRetain(rpso);
				}
			}
			if (!(m_deferredTileMode ? m_deferredResolveTilePipeline : m_deferredResolvePipeline))
				HE_LOG_ERROR(RHI, "%s",
					(std::string("MetalRenderer: deferred resolve pipeline build failed: ")
					 + (verr ? verr.localizedDescription.UTF8String : "?")).c_str());
		}
		else
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: deferred resolve shader compile failed\n")
				 + v.log + f.log).c_str());

		if (!m_gbufferPipeline)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: G-buffer pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());

		const bool ok = m_gbufferPipeline &&
			(m_deferredTileMode ? m_deferredResolveTilePipeline != nullptr
			                    : m_deferredResolvePipeline != nullptr);
		if (!ok)
			HE_LOG_WARN(RHI, "%s",
				"MetalRenderer: deferred render path unavailable — staying on forward");
		else
			HE_LOG_INFO(RHI, "%s", m_deferredTileMode
				? "MetalRenderer: deferred path ready (single-pass tile-memory G-buffer)"
				: "MetalRenderer: deferred path ready (two-pass stored G-buffer)");
		return ok;
	}
#endif
}

void MetalRenderer::EnsureBloomTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_bloomColor[0] && width == m_bloomW && height == m_bloomH)
		return;
	DestroyBloomTargets();

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSceneColorFormat width:width height:height mipmapped:NO];
	desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	for (int i = 0; i < 2; ++i)
		m_bloomColor[i] = (void*)CFBridgingRetain([device newTextureWithDescriptor:desc]);
	m_bloomW = width;
	m_bloomH = height;
}

void MetalRenderer::DestroyBloomTargets()
{
	for (int i = 0; i < 2; ++i)
		if (m_bloomColor[i]) { CFBridgingRelease(m_bloomColor[i]); m_bloomColor[i] = nullptr; }
	m_bloomResult = nullptr;
	m_bloomW = m_bloomH = 0;
}

void MetalRenderer::EnsureCloudTarget(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_cloudColor && width == m_cloudW && height == m_cloudH) return;
	DestroyCloudTarget();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSceneColorFormat width:width height:height mipmapped:NO];
	desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	m_cloudColor = (void*)CFBridgingRetain([device newTextureWithDescriptor:desc]);
	m_cloudW = width;
	m_cloudH = height;
}

void MetalRenderer::DestroyCloudTarget()
{
	if (m_cloudColor) { CFBridgingRelease(m_cloudColor); m_cloudColor = nullptr; }
	m_cloudW = m_cloudH = 0;
}

// Quarter-res clouds-only pass → m_cloudColor (rgb = scattered L, a = transmittance T).
// The sky pass upsamples + composites it (bg*T + L) when EnvironmentSettings.lowResClouds
// is on. The constants MUST stay identical to EncodeSky() so the clouds match the sky —
// which is now structural: both go through the one HE::BuildSkyFrameParams translation.
void MetalRenderer::EncodeCloudPrepass(void* cmdBufPtr, const glm::mat4& invViewProj,
	const glm::vec3& sunDir, float time, int width, int height)
{
	if (!m_cloudPipeline || width <= 0 || height <= 0) return;
	EnsureCloudTarget(width, height);
	if (!m_cloudColor) return;
	id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_cloudColor;
	pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0); // L=0, T=1 (clear sky)
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_cloudPipeline];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_noiseTexture atIndex:1];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_noiseSampler atIndex:1];
	HE::SkyFrameInputs in;
	in.invViewProj    = invViewProj;
	in.sunDir         = sunDir;
	in.cameraPos      = m_renderWorld.camera.position;
	in.time           = time;
	in.hasMoonTexture = m_moonTexture != nullptr;
	// The pre-pass IS the low-res cloud buffer, so it always raymarches (star2.z is
	// read only by skyFragment, which decides whether to composite what lands here).
	in.lowResClouds   = true;
	const HE::SkyFrameParams p = HE::BuildSkyFrameParams(GetEnvironment(), in);
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
}

// ── Cloud shadows ────────────────────────────────────────────────────────────
static constexpr int kCloudShadowMapSize = 512;

void MetalRenderer::EnsureCloudShadowTarget()
{
	if (m_cloudShadowTex) return;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	if (!device) return;
	MTLTextureDescriptor* d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		width:kCloudShadowMapSize height:kCloudShadowMapSize mipmapped:NO];
	d.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModePrivate;
	m_cloudShadowTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
}

void MetalRenderer::DestroyCloudShadowTarget()
{
	if (m_cloudShadowTex) { CFBridgingRelease(m_cloudShadowTex); m_cloudShadowTex = nullptr; }
}

// Render the cloud slab's sun transmittance over a world-space XZ region around
// the camera (kSkyMSL cloudShadowFragment — the sky's own density field). Runs
// right after the shadow map, BEFORE the G-buffer/scene passes, which sample the
// result at texture 16. Computes and stores the region + strength every frame
// (m_cloudShadowParamsA/B) for the SceneUniforms/Lighting fills; strength 0 =
// feature off this frame (the shaders then never sample the map).
void MetalRenderer::EncodeCloudShadow(void* cmdBufPtr)
{
	m_cloudShadowParamsB = glm::vec4(0.0f);
	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	if (!(env.skyEnabled && env.cloudShadows && env.cloudShadowStrength > 0.0f
	      && env.cloudCoverage > 0.001f && m_cloudShadowPipeline))
	{
		DestroyCloudShadowTarget(); // freed when toggled off
		return;
	}
	// Shadows are cast along the DOMINANT directional light (the same pick CSM
	// and the material lighting shadow along), not the raw sky-dome sun.
	glm::vec3 toward(0.0f, 1.0f, 0.0f), lightColor(0.0f);
	if (!m_renderWorld.dominantDirectionalLight(toward, lightColor)) return;
	toward = glm::normalize(toward);
	// Fade out toward the horizon: near-horizontal projections stretch to
	// infinity and the direct light is dim there anyway.
	const float strength = glm::clamp(env.cloudShadowStrength, 0.0f, 1.0f)
	                     * glm::smoothstep(0.08f, 0.18f, toward.y);
	if (strength <= 0.001f) return;
	EnsureCloudShadowTarget();
	if (!m_cloudShadowTex) return;

	const float     cloudH = std::max(env.cloudHeight, 1.0f);
	const float     thick  = cloudH * 1.5f;
	const glm::vec3 cam    = m_renderWorld.camera.position;
	// The deck sits at an ABSOLUTE world altitude, so the shadow slab needs no
	// anchoring trick any more: it simply IS the layer the view march samples,
	// and the camera cannot influence it by construction (the earlier
	// camera-relative layer forced a whole dead-band machinery here just to
	// keep the shadows from sliding when the viewer changed altitude).
	const float midY = cloudH + 0.5f * thick;
	// Ground level of the receivers — used ONLY to centre the map where the
	// shadows actually land (a rough value is fine; the region spans ±30 cloud
	// heights). Kept off the camera and lightly hysteresised so objects
	// streaming in and out of the scene bounds cannot make the region — and
	// with it the map's edge fade — breathe.
	{
		HE::AABB sceneBox;
		for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
		const float groundY = sceneBox.isValid() ? sceneBox.min.y : 0.0f;
		if (std::isnan(m_cloudShadowGroundY)) m_cloudShadowGroundY = groundY;
		const float dead = cloudH * 0.35f;
		const float d    = groundY - m_cloudShadowGroundY;
		if (d >  dead) m_cloudShadowGroundY = groundY - dead;
		if (d < -dead) m_cloudShadowGroundY = groundY + dead;
	}
	const float groundY = m_cloudShadowGroundY;
	// Region: ±30 cloud-heights around where the deck projects down onto the
	// receivers along the light (~6 km at the default altitude 200).
	const float half  = cloudH * 30.0f;
	const float size  = half * 2.0f;
	const float texel = size / static_cast<float>(kCloudShadowMapSize);
	glm::vec2 offs = glm::vec2(toward.x, toward.z) / std::max(toward.y, 0.05f) * (midY - groundY);
	if (glm::length(offs) > half * 4.0f) offs = glm::normalize(offs) * (half * 4.0f);
	glm::vec2 origin = glm::vec2(cam.x, cam.z) + offs - glm::vec2(half);
	origin = glm::floor(origin / texel) * texel; // texel snap — no swimming on camera moves
	m_cloudShadowParamsA = glm::vec4(origin.x, origin.y, 1.0f / size, midY);
	m_cloudShadowParamsB = glm::vec4(strength, 0.0f, 0.0f, 0.0f);

	id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_cloudShadowTex;
	pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor  = MTLClearColorMake(1.0, 1.0, 1.0, 1.0); // T=1 (no shadow)
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_cloudShadowPipeline];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_noiseTexture atIndex:1];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_noiseSampler atIndex:1];
	// BuildSkyFrameParams supplies everything the density formula needs
	// (coverage/wind/time/altitude/density/fluffiness) — sunDir overridden with
	// the dominant light so the march direction matches the shading. cameraPos
	// only carries the horizontal sample origin now; the map's slab altitude is
	// absolute (cloudShadowFragment reads it straight from cloud.x).
	HE::SkyFrameInputs in;
	in.sunDir    = toward;
	in.cameraPos = glm::vec3(cam.x, groundY, cam.z);
	in.time      = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov)
		in.time = static_cast<float>(std::atof(ov)); // deterministic headless captures
	const HE::SkyFrameParams p = HE::BuildSkyFrameParams(env, in);
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
	const glm::vec4 region(origin.x, origin.y, size, static_cast<float>(kCloudShadowMapSize));
	[enc setFragmentBytes:&region length:sizeof(region) atIndex:1];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];
}

void MetalRenderer::SetBloomSettings(const BloomSettings& s)
{
	m_bloomEnabled   = s.enabled;
	m_bloomThreshold = s.threshold;
	m_bloomStrength  = s.intensity;
}

void MetalRenderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
	// Resolve once, here, against what this backend can do — the render path
	// then only ever sees a runnable method.
	m_aaMethodRequested = s.method;   // resolved at use time — see ActiveAAMethod()
	m_renderScale       = s.renderScale;
	m_aaSharpness       = s.sharpness;
	m_specularAA        = s.specularAA;
	m_specularAAStrength = s.specularAAStrength;
}

// Bright-pass the HDR color then ping-pong blur (even pass count ends in
// m_bloomColor[0]). Each fullscreen pass is its own encoder. Returns the result
// texture, or nullptr if bloom is unavailable.
void* MetalRenderer::EncodeBloom(void* cmdBufPtr, int fullW, int fullH)
{
	if (!m_bloomBrightPipeline || !m_blurPipeline || !m_hdrColor) return nullptr;
	id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
	EnsureBloomTargets(fullW / 2, fullH / 2);
	if (!m_bloomColor[0]) return nullptr;

	// Bloom is 1 bright pass + 10 blur encoders; time the whole feature by
	// sampling start on the first encoder and end on the last (capture only).
	const uint32_t bloomBase = ftBeginMulti("Bloom");

	auto fullscreenPass = [&](id<MTLTexture> dst, id<MTLRenderPipelineState> pso,
	                          id<MTLTexture> src, const void* bytes, size_t len,
	                          uint32_t startSlot, uint32_t endSlot)
	{
		MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
		p.colorAttachments[0].texture     = dst;
		p.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		p.colorAttachments[0].storeAction = MTLStoreActionStore;
		if (startSlot != kInvalidSlot) ftAttachStart((__bridge void*)p, startSlot);
		if (endSlot   != kInvalidSlot) ftAttachEnd  ((__bridge void*)p, endSlot);
		id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:p];
		[enc setRenderPipelineState:pso];
		[enc setFragmentTexture:src atIndex:0];
		[enc setFragmentBytes:bytes length:len atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
	};

	id<MTLTexture> tex0 = (__bridge id<MTLTexture>)m_bloomColor[0];
	id<MTLTexture> tex1 = (__bridge id<MTLTexture>)m_bloomColor[1];

	// Bright pass: HDR scene color → m_bloomColor[0]. Carries the start timer slot.
	const simd::float2 brightParams = { m_bloomThreshold, m_bloomKnee };
	fullscreenPass(tex0, (__bridge id<MTLRenderPipelineState>)m_bloomBrightPipeline,
	               (__bridge id<MTLTexture>)m_hdrColor, &brightParams, sizeof(brightParams),
	               bloomBase, kInvalidSlot);

	// Ping-pong Gaussian blur. The last pass carries the end timer slot.
	const simd::float2 texel = { 1.0f / (float)m_bloomW, 1.0f / (float)m_bloomH };
	bool horizontal = true;
	constexpr int kBlurPasses = 10; // 5 horizontal + 5 vertical
	for (int i = 0; i < kBlurPasses; ++i)
	{
		id<MTLTexture> dst = horizontal ? tex1 : tex0;
		id<MTLTexture> src = horizontal ? tex0 : tex1;
		const simd::float4 cfg = { texel.x, texel.y, horizontal ? 1.0f : 0.0f, 0.0f };
		fullscreenPass(dst, (__bridge id<MTLRenderPipelineState>)m_blurPipeline,
		               src, &cfg, sizeof(cfg),
		               kInvalidSlot, (i == kBlurPasses - 1) ? bloomBase : kInvalidSlot);
		horizontal = !horizontal;
	}
	return m_bloomColor[0];
}

void MetalRenderer::SetSSAOSettings(const SSAOSettings& s)
{
	m_ssaoEnabled   = s.enabled;
	m_ssaoRadius    = s.radius;
	m_ssaoIntensity = s.intensity;
	m_ssaoMethod    = s.method;
}

void MetalRenderer::EnsureSSAOTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_ssaoPosTex && width == m_ssaoW && height == m_ssaoH) return;
	DestroySSAOTargets();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	MTLTextureDescriptor* posDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
	posDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	posDesc.storageMode = MTLStorageModePrivate;
	m_ssaoPosTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:posDesc]);

	MTLTextureDescriptor* dDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kDepthFormat width:width height:height mipmapped:NO];
	dDesc.usage       = MTLTextureUsageRenderTarget;
	dDesc.storageMode = MTLStorageModePrivate;
	m_ssaoPosDepth = (void*)CFBridgingRetain([device newTextureWithDescriptor:dDesc]);

	MTLTextureDescriptor* aoDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:width height:height mipmapped:NO];
	aoDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	aoDesc.storageMode = MTLStorageModePrivate;
	m_ssaoTex     = (void*)CFBridgingRetain([device newTextureWithDescriptor:aoDesc]);
	m_ssaoBlurTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:aoDesc]);

	// Forward-reflections MRT pair (oct normal + NDC depth) — same size, so the
	// SSR trace / GI-refl kernels sample it exactly like the deferred G-buffer.
	m_reflNormTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:posDesc]);
	MTLTextureDescriptor* rdDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:width height:height mipmapped:NO];
	rdDesc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	rdDesc.storageMode = MTLStorageModePrivate;
	m_reflDepthTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:rdDesc]);

	m_ssaoW = width; m_ssaoH = height;
}

void MetalRenderer::DestroySSAOTargets()
{
	if (m_ssaoPosTex)   { CFBridgingRelease(m_ssaoPosTex);   m_ssaoPosTex = nullptr; }
	if (m_ssaoPosDepth) { CFBridgingRelease(m_ssaoPosDepth); m_ssaoPosDepth = nullptr; }
	if (m_ssaoTex)      { CFBridgingRelease(m_ssaoTex);      m_ssaoTex = nullptr; }
	if (m_ssaoBlurTex)  { CFBridgingRelease(m_ssaoBlurTex);  m_ssaoBlurTex = nullptr; }
	if (m_reflNormTex)  { CFBridgingRelease(m_reflNormTex);  m_reflNormTex = nullptr; }
	if (m_reflDepthTex) { CFBridgingRelease(m_reflDepthTex); m_reflDepthTex = nullptr; }
	m_ssaoResult = nullptr;
	m_ssaoW = m_ssaoH = 0;
}

// View-space position pre-pass → occlusion → blur. Runs its own extract/cull/sort
// (deterministic → matches EncodeScene's draw set) and its own render encoders.
// Sets m_ssaoResult to the blurred AO texture (or null) for EncodeScene to bind.
void MetalRenderer::EncodeSSAO(void* cmdBufPtr, int width, int height)
{
	m_ssaoResult = nullptr;
	if (!m_ssaoPosPipeline || !m_ssaoPipeline || !m_ssaoBlurPipeline || !m_world) return;
	if (width <= 0 || height <= 0) return;

	// Deferred P5: the G-buffer was rasterized this frame — reconstruct the
	// view-space positions from its depth in one fullscreen draw instead of
	// re-extracting/culling/sorting and re-rasterizing the whole scene.
	// m_renderWorld.camera is current (EncodeGBuffer extracted right before).
	const bool fromGBuffer = m_deferredFrameActive && m_gbDepth && m_ssaoDepthPosPipeline;

	if (!fromGBuffer)
	{
	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height), &m_editorCamera);
	if (m_renderWorld.objects.empty()) return;
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;
	}

	EnsureSSAOTargets(width, height);
	const glm::mat4 viewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 view     = m_renderWorld.camera.view;
	// SSAO is three encoders (pos pre-pass → occlusion → blur); time the whole
	// feature by sampling start on the first and end on the last (capture only).
	const uint32_t ssaoBase = ftBeginMulti("SSAO");

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

		// ── 1. View-space position pre-pass ────────────────────────────────
		if (fromGBuffer)
		{
			// Deferred P5: one fullscreen draw reading the G-buffer depth.
			MTLRenderPassDescriptor* pp = [MTLRenderPassDescriptor renderPassDescriptor];
			pp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssaoPosTex;
			pp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			pp.colorAttachments[0].storeAction = MTLStoreActionStore;
			ftAttachStart((__bridge void*)pp, ssaoBase);
			id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pp];
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssaoDepthPosPipeline];
			struct { glm::mat4 invProj; } dp;
			dp.invProj = glm::inverse(m_renderWorld.camera.projection);
			[enc setFragmentBytes:&dp length:sizeof(dp) atIndex:0];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_gbDepth atIndex:0];
			[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_ssaoPointSampler atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
		}
		else
		{
		// Forward reflections extend the pre-pass to MRT (oct normal + NDC
		// depth alongside the view position) — see reflPosFragment.
		const bool mrt = m_fwdReflPrepassWanted && m_reflPosPipeline
		              && m_reflNormTex && m_reflDepthTex;
		MTLRenderPassDescriptor* pp = [MTLRenderPassDescriptor renderPassDescriptor];
		pp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssaoPosTex;
		pp.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pp.colorAttachments[0].storeAction = MTLStoreActionStore;
		pp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // a = 0 → background
		if (mrt)
		{
			pp.colorAttachments[1].texture     = (__bridge id<MTLTexture>)m_reflNormTex;
			pp.colorAttachments[1].loadAction  = MTLLoadActionClear;
			pp.colorAttachments[1].storeAction = MTLStoreActionStore;
			pp.colorAttachments[1].clearColor  = MTLClearColorMake(0.5, 0.5, 1.0, 0.0);
			pp.colorAttachments[2].texture     = (__bridge id<MTLTexture>)m_reflDepthTex;
			pp.colorAttachments[2].loadAction  = MTLLoadActionClear;
			pp.colorAttachments[2].storeAction = MTLStoreActionStore;
			pp.colorAttachments[2].clearColor  = MTLClearColorMake(1.0, 0.0, 0.0, 0.0); // depth 1 → background
		}
		pp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_ssaoPosDepth;
		pp.depthAttachment.loadAction  = MTLLoadActionClear;
		pp.depthAttachment.storeAction = MTLStoreActionDontCare;
		pp.depthAttachment.clearDepth  = 1.0;
		ftAttachStart((__bridge void*)pp, ssaoBase);
		id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pp];
		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)
			(mrt ? m_reflPosPipeline : m_ssaoPosPipeline)];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		HE::UUID lastId{}; const GpuMesh* cMesh = nullptr; bool valid = false;
		for (uint32_t idx : m_sortedIndices)
		{
			const RenderObject& obj = m_renderWorld.objects[idx];
			if (!obj.contributesAO) continue; // precip/particles: skip the SSAO prepass
			SSAOPosUniforms u;
			u.mvp       = viewProj * obj.transform;
			u.modelView = view * obj.transform;
			u.model     = obj.transform;
			if (!valid || obj.meshAssetId != lastId)
			{ cMesh = ResolveMesh(obj.meshAssetId); lastId = obj.meshAssetId; valid = true; }
			const GpuMesh* drawMesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!drawMesh) continue;
			id<MTLBuffer> vbuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
			id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
			NSUInteger    ic   = (NSUInteger)drawMesh->indexCount;
			[enc setVertexBuffer:vbuf offset:0 atIndex:0];
			[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
			[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ic
			                 indexType:MTLIndexTypeUInt32 indexBuffer:ibuf indexBufferOffset:0];
		}
		[enc endEncoding];
		}

		// Reflections-only run: SSAO itself is off (or replaced by GI) — the
		// MRT pre-pass outputs are all the forward reflection passes need,
		// occlusion + blur are skipped entirely.
		if (m_fwdReflPrepassOnly) return;

		// ── 2. Occlusion (fullscreen) ──────────────────────────────────────
		SSAOParamsCPU params;
		params.proj = m_renderWorld.camera.projection;
		params.cfg  = glm::vec4(static_cast<float>(width) / 4.0f, static_cast<float>(height) / 4.0f,
		                        m_ssaoRadius, 0.025f);
		params.cfg2 = glm::vec4(m_ssaoIntensity, static_cast<float>(m_ssaoMethod), 0.0f, 0.0f);
		const std::vector<glm::vec3> kernel = HE::BuildSSAOKernel(32);
		for (int i = 0; i < 32; ++i) params.samples[i] = glm::vec4(kernel[i], 0.0f);
		{
			MTLRenderPassDescriptor* sp = [MTLRenderPassDescriptor renderPassDescriptor];
			sp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssaoTex;
			sp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			sp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> e2 = [cmdBuf renderCommandEncoderWithDescriptor:sp];
			[e2 setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssaoPipeline];
			[e2 setFragmentTexture:(__bridge id<MTLTexture>)m_ssaoPosTex atIndex:0];
			[e2 setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_ssaoPointSampler atIndex:0];
			[e2 setFragmentTexture:(__bridge id<MTLTexture>)m_ssaoNoiseTex atIndex:1];
			[e2 setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_ssaoNoiseSampler atIndex:1];
			[e2 setFragmentBytes:&params length:sizeof(params) atIndex:0];
			[e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[e2 endEncoding];
		}

		// ── 3. Box blur (fullscreen) ───────────────────────────────────────
		{
			MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor];
			bp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssaoBlurTex;
			bp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			bp.colorAttachments[0].storeAction = MTLStoreActionStore;
			ftAttachEnd((__bridge void*)bp, ssaoBase);
			id<MTLRenderCommandEncoder> e3 = [cmdBuf renderCommandEncoderWithDescriptor:bp];
			[e3 setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssaoBlurPipeline];
			[e3 setFragmentTexture:(__bridge id<MTLTexture>)m_ssaoTex atIndex:0];
			[e3 setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
			[e3 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[e3 endEncoding];
		}
	}
	m_ssaoResult = m_ssaoBlurTex;
}

#if defined(HE_HAVE_SHADERC)
void* MetalRenderer::EnsureMaterialArchive()
{
	if (m_matArchiveTried) return m_matBinaryArchive;
	m_matArchiveTried = true;

	// MTLBinaryArchive's serializeToURL: segfaults inside Metal on macOS 26+ (Tahoe/beta)
	// when a warmup re-serializes the growing archive. The on-disk archive is only a
	// COLD-START optimization — in-session pipelines are still cached in
	// m_materialPipelineCache — so disable it entirely on the affected systems to keep the
	// editor from crashing at project load. HE_MTL_ARCHIVE=1 forces it back on (older/fixed
	// OSes), =0 forces it off everywhere.
	{
		bool enable;
		if (const char* e = std::getenv("HE_MTL_ARCHIVE")) enable = (std::atoi(e) != 0);
		else enable = (NSProcessInfo.processInfo.operatingSystemVersion.majorVersion < 26);
		if (!enable)
		{
			HE_LOG_INFO(RHI, "%s",
				"MetalRenderer: on-disk material pipeline archive disabled "
				"(MTLBinaryArchive serialize is unstable on this macOS; in-session cache still active)");
			return nullptr; // m_matBinaryArchive stays null → GetOrBuildMaterialPipeline skips it
		}
	}

	if (@available(macOS 11.0, *))
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (!device) return nullptr;
		NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
			NSApplicationSupportDirectory, NSUserDomainMask, YES);
		NSString* base = dirs.count ? dirs[0] : NSTemporaryDirectory();
		NSString* dir  = [base stringByAppendingPathComponent:@"HorizonEngine"];
		[[NSFileManager defaultManager] createDirectoryAtPath:dir
			withIntermediateDirectories:YES attributes:nil error:nil];
		NSString* path = [dir stringByAppendingPathComponent:@"material-pipelines.metalar"];
		m_matArchivePath = path.UTF8String;
		const BOOL exists = [[NSFileManager defaultManager] fileExistsAtPath:path];
		MTLBinaryArchiveDescriptor* d = [[MTLBinaryArchiveDescriptor alloc] init];
		if (exists) d.url = [NSURL fileURLWithPath:path];
		NSError* err = nil;
		id<MTLBinaryArchive> arch = [device newBinaryArchiveWithDescriptor:d error:&err];
		if (!arch && exists)
		{
			// Stale / incompatible archive (driver or OS changed) → start fresh.
			d.url = nil;
			arch = [device newBinaryArchiveWithDescriptor:d error:&err];
		}
		if (arch)
		{
			m_matBinaryArchive = (void*)CFBridgingRetain(arch);
			HE_LOG_INFO(RHI, "%s", exists
				? "MetalRenderer: loaded material pipeline archive from disk"
				: "MetalRenderer: created a new material pipeline archive");
		}
		return m_matBinaryArchive;
	}
	return nullptr;
}

// Material-system M1: a DROP-IN replacement for m_scenePipeline whose MSL is cross-
// compiled from a canonical-GLSL "standard" surface template. Pinned so the vertex
// buffer lands at [[buffer(0)]] and Uniforms at [[buffer(1)]] — exactly the bind points
// the opaque draw loop already uses (verts@0, setVertexBytes u@1) — so it needs no
// changes to the per-draw binds. Deliberately simple hemispheric lighting (no sky/fog/
// Build (or fetch) a Metal pipeline for a material's custom fragment. The shared
// MaterialShaderLibrary owns the canonical GLSL, the standard drop-in vertex, and the
// glslang→SPIRV-Cross cross-compile (so every backend shares them); this function only
// turns the emitted MSL into an MTLRenderPipelineState. Cached by `key` (fragment source
// hash); a null result is cached too so a broken shader isn't rebuilt every frame.
void* MetalRenderer::GetOrBuildMaterialPipeline(uint64_t key, const std::string& fragGlsl,
                                               const std::string& vertBody,
                                               const MaterialShaderVariant* precompiled,
                                               bool blend, bool gbuffer)
{
	if (blend)   key ^= 0xB1E4DB1E4DB1E4DULL; // blended variant gets its own cache slot
	if (gbuffer) key ^= 0x6BB0F6BB0F6BB0F6ULL; // deferred G-buffer variant, own cache slot
	if (auto it = m_materialPipelineCache.find(key); it != m_materialPipelineCache.end())
		return it->second;

	using Backend = HE::MaterialShaderLibrary::Backend;
	std::string vertMSL, fragMSL, log;
	bool ok = false;
	if (precompiled)
	{
		// Baked at export time — no runtime cross-compile.
		vertMSL = precompiled->vertex; fragMSL = precompiled->fragment;
		ok = !vertMSL.empty() && !fragMSL.empty();
	}
	else
	{
		// WPO materials get the graph-generated vertex; everything else the shared one.
		const auto& v = vertBody.empty()
			? m_matShaderLib.standardVertex(Backend::Metal)
			: m_matShaderLib.customVertex(std::hash<std::string>{}(vertBody), vertBody, Backend::Metal);
		const auto& f = m_matShaderLib.fragment(key, fragGlsl, Backend::Metal); // shared, cached MSL
		vertMSL = v.source; fragMSL = f.source; log = v.log + f.log;
		ok = v.ok && f.ok;
	}

	void* result = nullptr;
	if (ok)
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* err = nil;
		id<MTLLibrary> vLib = [device newLibraryWithSource:[NSString stringWithUTF8String:vertMSL.c_str()]
		                                           options:nil error:&err];
		id<MTLLibrary> fLib = err ? nil
			: [device newLibraryWithSource:[NSString stringWithUTF8String:fragMSL.c_str()]
			                       options:nil error:&err];
		if (vLib && fLib)
		{
			MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
			desc.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			desc.fragmentFunction = [fLib newFunctionWithName:@"main0"];
			if (gbuffer) // deferred variant: the G-buffer targets, never blended
			{
				desc.colorAttachments[0].pixelFormat = kGBuf0Format;
				desc.colorAttachments[1].pixelFormat = kGBufAttrFormat;
				desc.colorAttachments[2].pixelFormat = kGBufAttrFormat;
				desc.colorAttachments[3].pixelFormat = MTLPixelFormatR32Float; // NDC depth (P6)
				if (m_deferredTileMode)
				{
					desc.colorAttachments[4].pixelFormat = kSceneColorFormat;
					desc.colorAttachments[4].writeMask   = MTLColorWriteMaskNone;
				}
			}
			else
				desc.colorAttachments[0].pixelFormat = kSceneColorFormat; // same HDR target as m_scenePipeline
			desc.depthAttachmentPixelFormat      = kDepthFormat;
			if (blend && !gbuffer) // transparency-pass variant: standard back-to-front alpha blending
			{
				desc.colorAttachments[0].blendingEnabled             = YES;
				desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
				desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
				desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
				desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			}
			// On-disk pipeline cache: point the descriptor at the binary archive so
			// Metal reuses previously-compiled functions (fast) when present. Best-
			// effort — if the function isn't cached, Metal compiles it as usual.
			id<MTLBinaryArchive> arch = nil;
			if (@available(macOS 11.0, *))
			{
				arch = (__bridge id<MTLBinaryArchive>)EnsureMaterialArchive();
				if (arch) desc.binaryArchives = @[arch];
			}
			id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
			if (pso)
			{
				result = (void*)CFBridgingRetain(pso);
				// Add this pipeline's functions to the archive + persist, so the next
				// launch loads them instead of recompiling. Best-effort.
				if (@available(macOS 11.0, *))
					if (arch)
					{
						NSError* aerr = nil;
						if ([arch addRenderPipelineFunctionsWithDescriptor:desc error:&aerr]
						    && !m_matArchivePath.empty())
							[arch serializeToURL:[NSURL fileURLWithPath:@(m_matArchivePath.c_str())] error:nil];
					}
			}
		}
		if (!result)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: material pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());
	}
	else
		HE_LOG_ERROR(RHI, "%s",
			(std::string("MetalRenderer: material shader compile failed\n") + log).c_str());

	m_materialPipelineCache[key] = result; // cache success AND failure (null)
	return result;
}

// Delegate to the shared, backend-agnostic library (reads the MaterialAsset).
bool MetalRenderer::ResolveMaterialShader(const HE::UUID& materialId, uint64_t& key, std::string& frag,
                                          std::string& vertBody)
{
	if (!m_contentManager) return false;
	return m_matShaderLib.resolveShaders(*m_contentManager, materialId, key, frag, vertBody);
}

bool MetalRenderer::ResolveMaterialShaderGB(const HE::UUID& materialId, uint64_t& key, std::string& frag,
                                            std::string& vertBody)
{
	if (!m_contentManager) return false;
	return m_matShaderLib.resolveGBufferShaders(*m_contentManager, materialId, key, frag, vertBody);
}

#endif // HE_HAVE_SHADERC

// Build (or fetch) a Metal pipeline for GPU-instanced ParticleGraph particle
// rendering (RenderWorld::particleBatches — the real scene path, not
// RenderParticlePreview). Unlike GetOrBuildMaterialPipeline this needs no
// HE_HAVE_SHADERC / cross-compile step: the MSL is hand-templated, either baked
// at export time (`precompiled`) or generated right now from `config` via
// HE::generateParticleShaderSource + ParticleShaderTemplates. Always alpha-
// blended (particles have no opaque variant). Cached by `key`; null cached too.
void* MetalRenderer::GetOrBuildParticlePipeline(uint64_t key, const HE::ParticleEmitterConfig& config,
                                                const ParticleShaderVariant* precompiled)
{
	if (auto it = m_particlePipelineCache.find(key); it != m_particlePipelineCache.end())
		return it->second;

	std::string vertMSL, fragMSL;
	if (precompiled)
	{
		vertMSL = precompiled->vertex;
		fragMSL = precompiled->fragment;
	}
	else
	{
		const HE::ParticleShaderGen gen = HE::generateParticleShaderSource(config, /*metalSyntax*/true);
		vertMSL = HE::buildParticleVertexMSL(gen.colorFn, gen.alphaFn);
		fragMSL = HE::buildParticleFragmentMSL();
	}

	void* result = nullptr;
	if (!vertMSL.empty() && !fragMSL.empty())
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* err = nil;
		id<MTLLibrary> vLib = [device newLibraryWithSource:[NSString stringWithUTF8String:vertMSL.c_str()]
		                                           options:nil error:&err];
		id<MTLLibrary> fLib = err ? nil
			: [device newLibraryWithSource:[NSString stringWithUTF8String:fragMSL.c_str()]
			                       options:nil error:&err];
		if (vLib && fLib)
		{
			MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
			desc.vertexFunction   = [vLib newFunctionWithName:@"heParticleGraphVertex"];
			desc.fragmentFunction = [fLib newFunctionWithName:@"heParticleGraphFragment"];
			desc.colorAttachments[0].pixelFormat = kSceneColorFormat;
			desc.depthAttachmentPixelFormat      = kDepthFormat;
			desc.colorAttachments[0].blendingEnabled             = YES;
			desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
			desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
			desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
			desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
			if (pso) result = (void*)CFBridgingRetain(pso);
		}
		if (!result)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: particle pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());
	}

	m_particlePipelineCache[key] = result; // cache success AND failure (null)
	return result;
}

// Fullscreen tonemap of an HDR color target (+ bloom) into the bound encoder's
// target. `sourceHdr` defaults to the scene's; the world preview passes its own,
// because it renders HDR too and must go through the SAME curve — writing sky
// radiance and a sun at intensity 2.2 straight to display is what turned the
// first sky-lit preview into a uniformly white mesh under a blown-out sky.
void MetalRenderer::EncodeTonemap(void* renderEncoderPtr, void* sourceHdr, bool withBloom)
{
	void* hdr = sourceHdr ? sourceHdr : m_hdrColor;
	if (!m_tonemapPipeline || !hdr) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_tonemapPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)hdr atIndex:0];
	// Bloom on texture slot 1 (fall back to the HDR texture with 0 strength so the
	// shader's sampler always has a valid binding). m_bloomResult is null when
	// bloom was disabled or unavailable this frame — and a preview never has one.
	const bool bloom = withBloom && m_bloomResult;
	id<MTLTexture> bloomTex = bloom
		? (__bridge id<MTLTexture>)m_bloomResult
		: (__bridge id<MTLTexture>)hdr;
	[enc setFragmentTexture:bloomTex atIndex:1];
	const simd::float2 params = { 1.0f, bloom ? m_bloomStrength : 0.0f };
	[enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
	static const float kNoFlare[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	[enc setFragmentBytes:(withBloom ? m_lensFlareParams : kNoFlare)
	               length:sizeof(m_lensFlareParams) atIndex:1];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// LDR intermediate the tonemap writes to and FXAA reads from. kSwapchainFormat so
// the tonemap pipeline (which targets that format) is valid; recreated on resize.
void MetalRenderer::EnsureLdrTarget(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_ldrColor && width == m_ldrW && height == m_ldrH) return;
	DestroyLdrTarget();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:kSwapchainFormat width:width height:height mipmapped:NO];
	desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	m_ldrColor = (void*)CFBridgingRetain([device newTextureWithDescriptor:desc]);
	m_ldrW = width;
	m_ldrH = height;
}

void MetalRenderer::DestroyLdrTarget()
{
	if (m_ldrColor) { CFBridgingRelease(m_ldrColor); m_ldrColor = nullptr; }
	m_ldrW = m_ldrH = 0;
}

// Draw the frame's opaque geometry once more, writing only where each surface
// moved on screen. A separate pass on purpose: the alternative is a fifth
// G-buffer attachment, which would mean teaching FOUR pipeline descriptors and
// the graph-material codegen to write velocity — a material that forgot would
// leave undefined motion and ghost. This pass is material-agnostic by
// construction: it only ever reads positions.
void MetalRenderer::EncodeVelocity(void* cmdBufPtr, int width, int height)
{
	if (!TemporalActive() || !m_velocityTex || m_sortedIndices.empty()) return;
	id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_velocityTex;
	pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	// Cleared to zero = "did not move", which is also what the sky and every
	// pixel this pass does not reach should report.
	pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	// Depth-test against what the G-buffer pass already wrote, without touching
	// it: only the surfaces that are actually visible get to report motion.
	pass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
	pass.depthAttachment.loadAction  = MTLLoadActionLoad;
	pass.depthAttachment.storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_velocityPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState]; // LessEqual, no write

	const glm::mat4 viewProjClean = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 viewProjJit   = JitteredViewProj(viewProjClean, width, height);

	struct VelocityUniformsCPU { glm::mat4 mvpJitter, mvpNow, mvpPrev; };
	m_taaCurTransforms.clear();
	for (const uint32_t idx : m_sortedIndices)
	{
		const RenderObject& obj = m_renderWorld.objects[idx];
		const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		if (!mesh || !mesh->vertexBuf || !mesh->indexBuf) continue;

		// An object seen for the first time reports no motion — its "previous"
		// position is where it is now. Anything else invents a streak out of
		// nowhere on the frame something spawns.
		const auto it = m_taaPrevTransforms.find(obj.entityId);
		const glm::mat4 prevModel = (it != m_taaPrevTransforms.end()) ? it->second : obj.transform;
		m_taaCurTransforms[obj.entityId] = obj.transform;

		VelocityUniformsCPU u;
		u.mvpJitter = viewProjJit   * obj.transform;
		u.mvpNow    = viewProjClean * obj.transform;
		u.mvpPrev   = m_taaPrevViewProj * prevModel;
		[enc setVertexBuffer:(__bridge id<MTLBuffer>)mesh->vertexBuf offset:0 atIndex:0];
		[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
		[enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                indexCount:mesh->indexCount
		                 indexType:MTLIndexTypeUInt32
		               indexBuffer:(__bridge id<MTLBuffer>)mesh->indexBuf
		         indexBufferOffset:0];
	}
	[enc endEncoding];

	// Advance the history HERE, at the end of the one pass that consumed it —
	// not at some "frame end" further out. The extractor runs several times per
	// frame; anchoring the step to this pass is what keeps a frame from being
	// compared against itself.
	m_taaPrevViewProj = viewProjClean;
	m_taaPrevTransforms.swap(m_taaCurTransforms);
}

// Blend this frame's tonemapped image with the reprojected history. Runs AFTER
// the tonemap and BEFORE the AA-resolve slot, so the history lives in the same
// LDR space the user sees — which also keeps a single bright HDR sample from
// poisoning the accumulation for the next dozen frames.
void MetalRenderer::EncodeTaa(void* cmdBufPtr, int width, int height)
{
	if (!TaaActive() || !m_taaResolved || !m_ldrColor) return;
	id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

	MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_taaResolved;
	pass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pass];
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_taaPipeline];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ldrColor    atIndex:0];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_taaHistory  atIndex:1];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_velocityTex atIndex:2];
	// 0.9 keeps ~10 frames of samples: enough to converge on an edge, short
	// enough that a mis-reprojected pixel does not linger.
	const simd::float4 params = { 1.0f / (float)std::max(1, width),
	                              1.0f / (float)std::max(1, height),
	                              m_taaHistoryValid ? 0.9f : 0.0f,
	                              0.0f };
	[enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[enc endEncoding];

	// This frame's result IS next frame's history.
	id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
	[blit copyFromTexture:(__bridge id<MTLTexture>)m_taaResolved
	          sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(m_taaW, m_taaH, 1)
	            toTexture:(__bridge id<MTLTexture>)m_taaHistory
	     destinationSlice:0 destinationLevel:0
	    destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	m_taaHistoryValid = true;
}

// ─── MetalFX temporal scaling (A5) ──────────────────────────────────────────
bool MetalRenderer::MetalFxActive() const
{
	return m_mfxSupported && ActiveAAMethod() == HE::AAMethod::MetalFX &&
	       m_renderPath == HE::RenderPath::Deferred && m_velocityPipeline;
}

void MetalRenderer::EnsureMetalFX(int inW, int inH, int outW, int outH)
{
#if HE_HAS_METALFX
	if (m_mfxScaler && inW == m_mfxInW && inH == m_mfxInH &&
	    outW == m_mfxOutW && outH == m_mfxOutH)
		return;
	DestroyMetalFX();
	if (@available(macOS 13.0, *))
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		MTLFXTemporalScalerDescriptor* d = [[MTLFXTemporalScalerDescriptor alloc] init];
		d.colorTextureFormat  = kSceneColorFormat;      // HDR, pre-tonemap
		d.depthTextureFormat  = kDepthFormat;
		d.motionTextureFormat = MTLPixelFormatRG16Float;
		d.outputTextureFormat = kSceneColorFormat;
		d.inputWidth   = inW;  d.inputHeight  = inH;
		d.outputWidth  = outW; d.outputHeight = outH;
		id<MTLFXTemporalScaler> scaler = [d newTemporalScalerWithDevice:device];
		if (!scaler) return;
		m_mfxScaler = (void*)CFBridgingRetain(scaler);

		MTLTextureDescriptor* td = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:kSceneColorFormat
			width:outW height:outH mipmapped:NO];
		td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
		                 MTLTextureUsageShaderWrite;   // the scaler writes it
		td.storageMode = MTLStorageModePrivate;
		m_mfxOutput = (void*)CFBridgingRetain([device newTextureWithDescriptor:td]);
		m_mfxInW = inW; m_mfxInH = inH; m_mfxOutW = outW; m_mfxOutH = outH;
		HE_LOG_INFO(RHI, "%s", ("MetalRenderer: MetalFX scaler " + std::to_string(inW) + "x"
			+ std::to_string(inH) + " -> " + std::to_string(outW) + "x" + std::to_string(outH)
			+ " (color " + std::to_string((int)((__bridge id<MTLTexture>)m_hdrColor).width) + "x"
			+ std::to_string((int)((__bridge id<MTLTexture>)m_hdrColor).height) + ")").c_str());
		// A new scaler has no history — same rule as our own TAA target.
		m_mfxReset = true;
	}
#else
	(void)inW; (void)inH; (void)outW; (void)outH;
#endif
}

void MetalRenderer::DestroyMetalFX()
{
	if (m_mfxScaler) { CFBridgingRelease(m_mfxScaler); m_mfxScaler = nullptr; }
	if (m_mfxOutput) { CFBridgingRelease(m_mfxOutput); m_mfxOutput = nullptr; }
	m_mfxInW = m_mfxInH = m_mfxOutW = m_mfxOutH = 0;
	m_mfxReset = true;
}

void* MetalRenderer::EncodeMetalFX(void* cmdBufPtr, int inW, int inH, int outW, int outH)
{
#if HE_HAS_METALFX
	if (!MetalFxActive() || !m_hdrColor || !m_hdrDepth || !m_velocityTex) return nullptr;
	EnsureMetalFX(inW, inH, outW, outH);
	if (!m_mfxScaler || !m_mfxOutput) return nullptr;
	if (@available(macOS 13.0, *))
	{
		id<MTLFXTemporalScaler> scaler = (__bridge id<MTLFXTemporalScaler>)m_mfxScaler;
		scaler.colorTexture  = (__bridge id<MTLTexture>)m_hdrColor;
		scaler.depthTexture  = (__bridge id<MTLTexture>)m_hdrDepth;
		scaler.motionTexture = (__bridge id<MTLTexture>)m_velocityTex;
		scaler.outputTexture = (__bridge id<MTLTexture>)m_mfxOutput;
		// Which part of the input textures actually holds this frame. NOT
		// optional: left unset the scaler copies the input 1:1 into the corner of
		// the output instead of upscaling, which looks exactly like "the scene
		// renders small in the top-left of a black frame".
		scaler.inputContentWidth  = inW;
		scaler.inputContentHeight = inH;
		// Our velocity texture holds "where this pixel moved TO", in UV units.
		// MetalFX wants "where it came FROM", in input pixels — so the scale
		// carries both the unit conversion AND the sign flip. Two constants, one
		// place: if a future capture shows motion tracking the wrong way, this
		// is the line, not the shader.
		scaler.motionVectorScaleX = -static_cast<float>(inW);
		scaler.motionVectorScaleY = -static_cast<float>(inH);
		// The jitter the frame was rendered with, in pixels, so the scaler can
		// undo it — the same offset JitteredViewProj applied.
		scaler.jitterOffsetX = -m_taaJitter.x;
		scaler.jitterOffsetY = -m_taaJitter.y;
		scaler.reset = m_mfxReset;
		[scaler encodeToCommandBuffer:(__bridge id<MTLCommandBuffer>)cmdBufPtr];
		m_mfxReset = false;
		return m_mfxOutput;
	}
	return nullptr;
#else
	(void)cmdBufPtr; (void)inW; (void)inH; (void)outW; (void)outH;
	return nullptr;
#endif
}

// Everything temporal needs the same two things — a jittered raster and a
// velocity buffer — whether the accumulation is ours or the OS scaler's. Asking
// it once here is what keeps pass setup, jitter and shader binding from
// disagreeing about whether this frame has motion data.
bool MetalRenderer::TemporalActive() const
{
	const HE::AAMethod m = ActiveAAMethod();
	return (m == HE::AAMethod::TAA || m == HE::AAMethod::MetalFX) &&
	       m_renderPath == HE::RenderPath::Deferred && m_velocityPipeline;
}

bool MetalRenderer::TaaActive() const
{
	return TemporalActive() && ActiveAAMethod() == HE::AAMethod::TAA && m_taaPipeline;
}

// The rasterisation matrix. The offset is applied in CLIP space (a translation
// of the projected x/y by a fraction of a pixel), which is the same thing as
// shifting the sample grid — and it leaves the caller's matrix untouched, so the
// unjittered one stays available for motion and reprojection.
glm::mat4 MetalRenderer::JitteredViewProj(const glm::mat4& viewProj, int width, int height) const
{
	if (!TemporalActive() || width <= 0 || height <= 0) return viewProj;
	glm::mat4 j(1.0f);
	j[3][0] = m_taaJitter.x * 2.0f / static_cast<float>(width);
	j[3][1] = m_taaJitter.y * 2.0f / static_cast<float>(height);
	return j * viewProj;
}

void MetalRenderer::EnsureTaaTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_taaHistory && width == m_taaW && height == m_taaH) return;
	DestroyTaaTargets();

	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	auto make = [&](MTLPixelFormat fmt) -> void* {
		MTLTextureDescriptor* d = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:fmt width:width height:height mipmapped:NO];
		d.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModePrivate;
		return (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	};
	m_velocityTex = make(MTLPixelFormatRG16Float);
	m_taaHistory  = make(kSwapchainFormat);
	m_taaResolved = make(kSwapchainFormat);
	m_taaW = width;
	m_taaH = height;
	// A fresh history is garbage, not history: the first frame after a resize
	// must show the current frame only, or it blends against uninitialised
	// memory. Same reason a camera jump has to clear this.
	m_taaHistoryValid = false;
}

void MetalRenderer::DestroyTaaTargets()
{
	if (m_velocityTex) { CFBridgingRelease(m_velocityTex); m_velocityTex = nullptr; }
	if (m_taaHistory)  { CFBridgingRelease(m_taaHistory);  m_taaHistory  = nullptr; }
	if (m_taaResolved) { CFBridgingRelease(m_taaResolved); m_taaResolved = nullptr; }
	m_taaW = m_taaH = 0;
	m_taaHistoryValid = false;
	m_taaPrevTransforms.clear();
	m_taaCurTransforms.clear();
}

// Fullscreen AA resolve of the tonemapped LDR image into the bound encoder's
// target. This is the pass that FILLS that target, so it always draws — the AA
// method only picks the pipeline (FXAA filter vs. straight blit).
void MetalRenderer::EncodeFxaa(void* renderEncoderPtr, int width, int height)
{
	const HE::AAMethod method = ActiveAAMethod();
	// TAA already produced the finished image in its own pass; this slot only
	// moves it to the target (with the optional sharpen the temporal blur asks
	// for). Off = passthrough, SMAA/FXAA filter the tonemapped LDR.
	void* pipeline = (method == HE::AAMethod::TAA && m_taaResolved) ? m_taaSharpenPipeline
	               : (method == HE::AAMethod::Off)                  ? m_aaBlitPipeline
	               : (method == HE::AAMethod::SMAA)                 ? m_smaaPipeline
	                                                                : m_fxaaPipeline;
	if (!pipeline || !m_ldrColor) return;
	// The TAA slot reads its own output, everything else the tonemapped LDR.
	void* source = (pipeline == m_taaSharpenPipeline) ? m_taaResolved : m_ldrColor;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)source atIndex:0];
	// float4, not float2: the sharpen used by the TAA slot needs its amount in
	// .z. The FXAA/SMAA fragments declare a float2 and simply ignore the tail.
	const simd::float4 rcpFrame = { 1.0f / (float)std::max(1, width),
	                                1.0f / (float)std::max(1, height),
	                                (pipeline == m_taaSharpenPipeline) ? m_aaSharpness : 0.0f,
	                                0.0f };
	[enc setFragmentBytes:&rcpFrame length:sizeof(rcpFrame) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// 2D UI quads (solid color) into the bound render encoder's target.
// Build (or fetch) the pipeline that draws a UI quad with a node-graph material:
// the material's shared fragment MSL paired with the screen-space uiVertex, alpha-
// blended into the LDR UI target. Cache key = the material's shader hash.
void* MetalRenderer::GetOrBuildUIMaterialPipeline(const HE::UUID& materialId)
{
	uint64_t key = 0; std::string fragGlsl, vertBody;
	if (!ResolveMaterialShader(materialId, key, fragGlsl, vertBody))
		return nullptr; // no custom shader → solid-color quad
	if (auto it = m_uiMaterialPipelines.find(key); it != m_uiMaterialPipelines.end())
		return it->second;

	using Backend = HE::MaterialShaderLibrary::Backend;
	const auto& v = m_matShaderLib.uiVertex(Backend::Metal);
	const auto& f = m_matShaderLib.fragment(key, fragGlsl, Backend::Metal);

	void* result = nullptr;
	if (v.ok && f.ok)
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* err = nil;
		id<MTLLibrary> vLib = [device newLibraryWithSource:[NSString stringWithUTF8String:v.source.c_str()]
		                                           options:nil error:&err];
		id<MTLLibrary> fLib = err ? nil
			: [device newLibraryWithSource:[NSString stringWithUTF8String:f.source.c_str()]
			                       options:nil error:&err];
		if (vLib && fLib)
		{
			MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
			desc.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			desc.fragmentFunction = [fLib newFunctionWithName:@"main0"];
			desc.colorAttachments[0].pixelFormat     = kSwapchainFormat; // UI target (LDR)
			desc.colorAttachments[0].blendingEnabled = YES;
			desc.colorAttachments[0].rgbBlendOperation         = MTLBlendOperationAdd;
			desc.colorAttachments[0].alphaBlendOperation       = MTLBlendOperationAdd;
			desc.colorAttachments[0].sourceRGBBlendFactor      = MTLBlendFactorSourceAlpha;
			desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			desc.colorAttachments[0].sourceAlphaBlendFactor    = MTLBlendFactorOne;
			desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
			desc.depthAttachmentPixelFormat = kDepthFormat;
			id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&err];
			if (pso) result = (void*)CFBridgingRetain(pso);
		}
		if (!result)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: UI material pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());
	}
	else
		HE_LOG_ERROR(RHI, "%s",
			(std::string("MetalRenderer: UI material shader compile failed\n") + v.log + f.log).c_str());

	m_uiMaterialPipelines[key] = result; // cache success AND failure (null)
	return result;
}

void* MetalRenderer::UIFontAtlasTexture(uint32_t key)
{
	if (key == 0) return m_uiFontTexture;
	if (auto it = m_uiFontAtlases.find(key); it != m_uiFontAtlases.end()) return it->second;
	const HE::BakedUIFont* f = HE::UIFontCache::find(key);
	if (!f || !f->ok) return m_uiFontTexture;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
		                             width:f->atlasW height:f->atlasH mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
	id<MTLTexture> tex = [device newTextureWithDescriptor:d];
	[tex replaceRegion:MTLRegionMake2D(0, 0, f->atlasW, f->atlasH)
	       mipmapLevel:0 withBytes:f->pixels.data() bytesPerRow:f->atlasW];
	void* stored = (void*)CFBridgingRetain(tex);
	m_uiFontAtlases[key] = stored;
	return stored;
}

void MetalRenderer::EncodeUIPass(void* renderEncoderPtr, int width, int height)
{
	if (!m_uiPipeline || m_renderWorld.uiObjects.empty()) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	const simd::float2 vp = { (float)std::max(1, width), (float)std::max(1, height) };

	// Lighting block for material quads (heLit sun/ambient + the Time input).
	// Same dominant-directional fill as EncodeScene's matLight — the raw
	// environment sunColor is never night/cloud-modulated (permanently sun-lit).
	HE::MaterialShaderLibrary::Lighting matLight;
	{
		glm::vec3 sd, sc;
		m_renderWorld.dominantDirectionalLight(sd, sc);
		const glm::vec3 am = m_renderWorld.ambient;
		matLight.sunDir[0]   = sd.x; matLight.sunDir[1] = sd.y; matLight.sunDir[2] = sd.z;
		matLight.sunDir[3]   = static_cast<float>(SDL_GetTicks()) / 1000.0f;
		matLight.sunColor[0] = sc.r; matLight.sunColor[1] = sc.g; matLight.sunColor[2] = sc.b;
		matLight.ambient[0]  = am.r; matLight.ambient[1]  = am.g; matLight.ambient[2]  = am.b;
		matLight.camPos[0]   = m_renderWorld.camera.position.x;
		matLight.camPos[1]   = m_renderWorld.camera.position.y;
		matLight.camPos[2]   = m_renderWorld.camera.position.z;
		// Full light window for heLitP() — same first-8 order as the built-in
		// PBR shaders. Shared fill (HE::FillMaterialLightWindow); the UI pass has
		// no local shadow atlas, so it passes false.
		HE::FillMaterialLightWindow(m_renderWorld, matLight, /*localShadowsActive=*/false);
	}

	// The uiVertex's repurposed U block (see MaterialShaderLibrary::uiVertex).
	struct UIU { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };

	bool basicBound = false;        // solid/glyph pipeline currently set?
	void* boundMaterial = nullptr;  // material PSO currently set
	uint32_t boundAtlasKey = 0;     // font atlas currently bound at texture(0)

	// Clipping is a scissor rectangle, set only when it CHANGES — a widget tree
	// emits its quads in tree order, so equally-clipped quads arrive in runs.
	// Metal asserts on a scissor outside the render target, hence the clamp.
	glm::vec4 appliedClip(-1.0f);
	auto applyClip = [&](const glm::vec4& c)
	{
		if (c == appliedClip) return;
		appliedClip = c;
		MTLScissorRect s;
		if (c.z <= 0.0f)   // unclipped: the whole target
			s = MTLScissorRect{ 0, 0, (NSUInteger)std::max(1, width), (NSUInteger)std::max(1, height) };
		else
		{
			const float x0 = std::clamp(c.x, 0.0f, (float)width);
			const float y0 = std::clamp(c.y, 0.0f, (float)height);
			const float x1 = std::clamp(c.x + c.z, 0.0f, (float)width);
			const float y1 = std::clamp(c.y + c.w, 0.0f, (float)height);
			s = MTLScissorRect{ (NSUInteger)x0, (NSUInteger)y0,
			                    (NSUInteger)std::max(0.0f, x1 - x0),
			                    (NSUInteger)std::max(0.0f, y1 - y0) };
		}
		[enc setScissorRect:s];
	};

	for (const UIRenderObject& obj : m_renderWorld.uiObjects)
	{
		applyClip(obj.clipRect);
		// Custom material on an image quad → material pipeline (solid path below
		// stays the fallback when the material has no custom shader / failed).
		void* matPso = obj.type == 0 && obj.materialAssetId != HE::UUID{}
			? GetOrBuildUIMaterialPipeline(obj.materialAssetId) : nullptr;
		if (matPso)
		{
			if (boundMaterial != matPso)
			{
				[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)matPso];
				[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
				boundMaterial = matPso; basicBound = false;
			}
			UIU u{};
			u.model[0] = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
			u.model[1] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
			u.model[2] = glm::vec4(vp.x, vp.y, 0.0f, 0.0f);
			// Render rotation for the material path, same row the UI vertex reads.
			u.model[3] = glm::vec4(obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y, 0.0f);
			u.color    = obj.color;
			[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
			[enc setFragmentBytes:&matLight length:sizeof(matLight)
			              atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];

			// HeParams + graph textures, mirroring the mesh path's bind points.
			if (const MaterialAsset* ma = m_contentManager
				? m_contentManager->getMaterial(obj.materialAssetId) : nullptr)
			{
				float padded[64] = { 0 };
				const size_t n = std::min(ma->shaderParamData.size(), size_t(64));
				std::memcpy(padded, ma->shaderParamData.data(), n * sizeof(float));
				[enc setFragmentBytes:padded length:sizeof(padded) atIndex:2];
				for (size_t i = 0; i < HE::kMatMaxGraphTextures; ++i)
				{
					const HE::UUID tid = i < ma->graphTextureIds.size() ? ma->graphTextureIds[i] : HE::UUID{};
					const std::string tp = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string();
					void* gt = ResolveGraphTexture(tid, tp);
					id<MTLTexture> tex = gt ? (__bridge id<MTLTexture>)gt
					                        : (__bridge id<MTLTexture>)m_dummyTexture;
					[enc setFragmentTexture:tex atIndex:(NSUInteger)(i + 1)];
					[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler
					                     atIndex:(NSUInteger)(i + 1)];
				}
				// Legacy/mesh texture slot 0 must be bound too (pinned unconditionally).
				[enc setFragmentTexture:(__bridge id<MTLTexture>)m_dummyTexture atIndex:0];
				[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
			}
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			continue;
		}

		if (!basicBound)
		{
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_uiPipeline];
			[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
			[enc setVertexBytes:&vp length:sizeof(vp) atIndex:1];
			void* a0 = UIFontAtlasTexture(0);
			id<MTLTexture> atlas = a0 ? (__bridge id<MTLTexture>)a0
			                          : (__bridge id<MTLTexture>)m_dummyTexture;
			[enc setFragmentTexture:atlas atIndex:0];
			boundAtlasKey = 0;
			basicBound = true; boundMaterial = nullptr;
		}
		// A glyph quad may use an imported font's atlas — bind it at texture(0).
		if (obj.type == 2 && obj.fontAtlasKey != boundAtlasKey)
		{
			void* a = UIFontAtlasTexture(obj.fontAtlasKey);
			id<MTLTexture> atlas = a ? (__bridge id<MTLTexture>)a
			                         : (__bridge id<MTLTexture>)m_dummyTexture;
			[enc setFragmentTexture:atlas atIndex:0];
			boundAtlasKey = obj.fontAtlasKey;
		}
		// A textured quad borrows the same slot; the next glyph rebinds its
		// atlas because boundAtlasKey is invalidated here.
		bool textured = false;
		if (obj.type == 0 && obj.textureAssetId != HE::UUID{})
		{
			if (void* t = ResolveGraphTexture(obj.textureAssetId, std::string()))
			{
				[enc setFragmentTexture:(__bridge id<MTLTexture>)t atIndex:0];
				boundAtlasKey = 0xFFFFFFFFu;   // not a font atlas any more
				textured = true;
			}
		}
		const simd::float4 rect  = { obj.position.x, obj.position.y, obj.size.x, obj.size.y };
		const simd::float4 color = { obj.color.r, obj.color.g, obj.color.b, obj.color.a };
		const simd::float4 uvr   = { obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y };
		// shape = { mode, cornerRadius, rectW, rectH } (see uiFragment).
		const float mode = obj.type == 2 ? 1.0f : (textured ? 2.0f : 0.0f);
		const simd::float4 shape = { mode, obj.cornerRadius, obj.size.x, obj.size.y };
		const simd::float4 rot = { obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y, 0.0f };
		[enc setVertexBytes:&rect  length:sizeof(rect)  atIndex:0];
		[enc setVertexBytes:&uvr   length:sizeof(uvr)   atIndex:2];
		[enc setVertexBytes:&rot   length:sizeof(rot)   atIndex:3];
		[enc setFragmentBytes:&color length:sizeof(color) atIndex:0];
		[enc setFragmentBytes:&shape length:sizeof(shape) atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
	// Hand the encoder back in the state it was given in: the scissor is
	// encoder-wide, and whatever draws after this pass never asked for one.
	applyClip(glm::vec4(0.0f));
}

// ─── Frame encoding ───────────────────────────────────────────────────────────

void MetalRenderer::EncodeSky(void* renderEncoder, const glm::mat4& invViewProj,
                             const glm::vec3& sunDir, float time,
                             const IRenderer::EnvironmentSettings& env,
                             const glm::vec3& camPos, bool lowResClouds)
{
	if (!m_skyPipeline) return;
	if (!env.skyEnabled) return; // no Sky entity → leave the cleared background
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_skyPipeline];
	// Depth-test == far, no write — only fills background pixels (drawn after scene).
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState];
	id<MTLTexture> moon = m_moonTexture
		? (__bridge id<MTLTexture>)m_moonTexture
		: (__bridge id<MTLTexture>)m_dummyTexture;
	[enc setFragmentTexture:moon atIndex:0];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_noiseTexture atIndex:1];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_noiseSampler atIndex:1];
	HE::SkyFrameInputs in;
	in.invViewProj    = invViewProj;
	in.sunDir         = sunDir;
	in.cameraPos      = camPos;
	in.time           = time;
	in.hasMoonTexture = m_moonTexture != nullptr;
	// Composite the low-res clouds only when the pre-pass actually produced a buffer
	// (else fall back to the inline raymarch so nothing breaks if the target is missing).
	in.lowResClouds   = (lowResClouds && env.lowResClouds && m_cloudColor);
	const HE::SkyFrameParams p = HE::BuildSkyFrameParams(env, in);
	// Quarter-res cloud buffer (rgb=L, a=T) on slot 2; dummy when unused (must be bound).
	[enc setFragmentTexture:(__bridge id<MTLTexture>)(m_cloudColor ? m_cloudColor : m_dummyTexture) atIndex:2];
	[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
	[enc setFragmentBytes:&m_prepassViewProj[0][0] length:sizeof(glm::mat4) atIndex:1]; // low-res cloud reprojection
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

void MetalRenderer::UpdateSkyEnvCube(const glm::vec3& sunDir)
{
	if (!m_skyEnvCube) return;
	// The baked sky only changes with the sun direction — skip the rebuild + upload
	// when it hasn't moved.
	if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 1e-4f) return;
	m_skyEnvSunDir = sunDir; m_skyEnvValid = true;
	id<MTLTexture> cube = (__bridge id<MTLTexture>)m_skyEnvCube;
	constexpr int N = 128;
	// Parallel over rows, like the GL backend: each row is independent and
	// HE::SkyColorCPU is pure. This runs on EVERY frame the sun moves (day-night
	// auto-advance), and serially it was ~60 ms — a hard per-frame stall. It used
	// to be cheap at night only because the atmosphere integral was wrongly
	// short-circuiting there (see atmoRaySphere); with that fixed, twilight costs
	// what daylight always did, so the parallelisation is no longer optional.
	std::vector<float> px(static_cast<size_t>(N) * N * 6 * 4);
	parallel_for(static_cast<size_t>(6) * N, [&](size_t idx)
	{
		const int f = static_cast<int>(idx / N);
		const int t = static_cast<int>(idx % N);
		HE::BuildSkyEnvFaceRow(N, f, t, sunDir,
		                       &px[((static_cast<size_t>(f) * N + t) * N) * 4]);
	}, "SkyEnvBake");
	for (int f = 0; f < 6; ++f)
		[cube replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 slice:f
		          withBytes:&px[(static_cast<size_t>(f) * N * N) * 4]
		        bytesPerRow:N * 4 * sizeof(float) bytesPerImage:0];
}

// ─── Skinned geometry pass ────────────────────────────────────────────────────
// Draws all SkinnedRenderObjects from the current render world using the
// linear blend-skinning vertex shader. Must be called inside the HDR scene
// render encoder (same attachments as the opaque geometry pass).
// sceneUniformsPtr is a const SceneUniforms* (opaque to avoid pulling the struct
// into the header).
void MetalRenderer::EncodeSkinnedObjects(void* renderEncoder, const glm::mat4& viewProj,
                                         bool shadows, const void* sceneUniformsPtr)
{
	if (!m_skinnedPipeline || m_renderWorld.skinnedObjects.empty())
		return;

	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	id<MTLDevice>               device  = (__bridge id<MTLDevice>)m_device;
	const SceneUniforms& scene = *static_cast<const SceneUniforms*>(sceneUniformsPtr);

	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_skinnedPipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	// Scene-wide fragment state: SceneUniforms + shadow map + skyEnv + AO
	[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:1]; // CSM array; sampling gated by shadowEnabled
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:1];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];
	const bool ssaoActive = m_ssaoEnabled && m_ssaoResult;
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:3];
	const bool giActive = m_giEnabled && m_giSupported && m_giShadowResult
	                    && m_giIrradianceAtlas && m_giVisibilityAtlas;
	GIUniforms giUniforms = BuildGIUniforms(giActive, m_giGridOrigin, kGIProbeSpacing,
	                                        m_giGridCounts, m_giProbesPerRow, m_giIndirectIntensity);
	[encoder setFragmentBytes:&giUniforms length:sizeof(giUniforms) atIndex:3];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giShadowResult : m_dummyTexture) atIndex:5];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:5];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giIrradianceAtlas : m_dummyTexture) atIndex:6];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:6];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giVisibilityAtlas : m_dummyTexture) atIndex:7];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:7];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giLocalMaskTex : m_dummyTexture) atIndex:8];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:8];
	// FORWARD reflection results at 9/10 (heLitP pins; the GI masks moved onto
	// the shared 5/8 slots — ssr-plan P0). Dummies when the passes didn't run;
	// the ssr.x / giRefl.z gates are 0 then, the samples fold dead.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflSsrTex ? m_fwdReflSsrTex : m_dummyTexture) atIndex:9];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:9];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflGiTex ? m_fwdReflGiTex : m_dummyTexture) atIndex:10];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:10];
	// CSM array for the material preamble's GI-off fallback, pinned at 11
	// (sampling gated by heLight.csmSplits.w — same convention as slot 1).
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:11];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:11];
	// Local (point/spot) shadow atlas, pinned at 12 (sampling gated per light by params.y).
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_localShadowTex ? m_localShadowTex : m_shadowDepthTex) atIndex:12];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:12];
	// Sky env cubemap (14) + screen-space AO (15) for the material preamble's
	// image-based ambient and fog — the SAME textures the built-in shaders read
	// at 2/3, re-pinned clear of the material-texture window. Gated by
	// heLight.fog.z/.w gate the samples, so an absent cubemap (nil — legal in
	// Metal, reads zero) or a dummy AO bind here is inert. The cube slot must NOT
	// get the 2D dummy: Metal validates the texture TYPE.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:14];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:14];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:15];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:15];
	// Cloud-shadow map at texture 16 — TEXTURE only, deliberately no sampler
	// (both fragmentMain and the material preamble sample it through an inline
	// constexpr sampler; sampler indices stop at 15). White dummy when the
	// pass didn't run, and the strength gate is 0 then anyway.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_cloudShadowTex ? m_cloudShadowTex : m_dummyTexture) atIndex:16];
	// The material preamble's DDGI atlases map onto slots 6/7 — the pins the
	// scene pass already set above for the built-in shaders (Metal caps the
	// fragment stage at 16 samplers, so they cannot get their own). Nothing to
	// bind here.
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];

	constexpr int kMaxBones = 128;
	std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));

	for (const SkinnedRenderObject& obj : m_renderWorld.skinnedObjects)
	{
		const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(obj.meshAssetId);
		if (!smesh) continue;

		// Per-draw uniforms (mvp, model, color, pbr)
		UnlitUniforms u;
		u.mvp   = viewProj * obj.transform;
		u.model = obj.transform;
		void* matTex = nullptr;
		bool  hasTex = ResolveMaterialTexture(obj.materialAssetId, matTex);
		void* effectiveTex = hasTex ? matTex : smesh->texture;
		void* texPtr = effectiveTex ? effectiveTex : m_dummyTexture;
		u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, obj.receivesShadow ? 0.0f : 1.0f, 0, 0);
		glm::vec3 baseColor(1.0f); float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
		bool hasMat = ResolveMaterialParams(obj.materialAssetId, baseColor, metallic, roughness, opacity);
		if (!hasMat) baseColor = effectiveTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);
		u.color = glm::vec4(baseColor, 1.0f);
		u.pbr   = glm::vec4(metallic, roughness, opacity, 0.0f);

		// Upload bone matrices for this draw — allocate a temporary buffer so
		// each draw call gets its own range (the encoder retains it until GPU completion).
		const int boneCount = static_cast<int>(
		    std::min(obj.boneMatrices.size(), static_cast<size_t>(kMaxBones)));
		std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
		if (boneCount > 0)
			std::copy_n(obj.boneMatrices.begin(), boneCount, boneScratch.begin());

		id<MTLBuffer> boneBuf = [device newBufferWithBytes:boneScratch.data()
		                                            length:kMaxBones * sizeof(glm::mat4)
		                                           options:MTLResourceStorageModeShared];

		[encoder setVertexBuffer:(__bridge id<MTLBuffer>)smesh->vertexBuf  offset:0 atIndex:0];
		[encoder setVertexBytes:&u length:sizeof(u) atIndex:1];
		[encoder setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneIdBuf  offset:0 atIndex:2];
		[encoder setVertexBuffer:(__bridge id<MTLBuffer>)smesh->boneWgtBuf offset:0 atIndex:3];
		[encoder setVertexBuffer:boneBuf                                   offset:0 atIndex:4];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)texPtr atIndex:0];
		[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
		                    indexCount:(NSUInteger)smesh->indexCount
		                     indexType:MTLIndexTypeUInt32
		                   indexBuffer:(__bridge id<MTLBuffer>)smesh->indexBuf
		             indexBufferOffset:0];
		++m_counters.draws;
		m_counters.tris += static_cast<uint32_t>(smesh->indexCount / 3);
		// boneBuf is released here (ARC); the encoder holds its own strong reference
	}

	// Restore the regular scene pipeline for subsequent passes
	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_scenePipeline];
}

void MetalRenderer::EncodeScene(void* renderEncoder, int width, int height,
                                MetalDeferredFrame* deferred)
{
	if (!m_world || !m_scenePipeline || width <= 0 || height <= 0)
		return;

	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height),
	                    &m_editorCamera);
	m_extractor.extractUI(*m_world, static_cast<float>(width), static_cast<float>(height),
	                      m_renderWorld);

	// Jittered for TAA (no-op otherwise): the forward tail — sky, transparency,
	// the resolve's own fullscreen draw — has to sit on the same sample grid as
	// the G-buffer geometry, or the temporal filter sees them fighting.
	const glm::mat4 viewProj = JitteredViewProj(
		m_renderWorld.camera.projection * m_renderWorld.camera.view, width, height);

	// Direction toward the sun for sky + image-based ambient — resolved by the
	// extractor (scene directional light, or the day-night cycle when enabled).
	const glm::vec3 sunDir = m_renderWorld.sunDirection;

	// Skybox is drawn LAST (after the geometry) with a depth-test == far, so the
	// heavy sky shader only runs on the background pixels the scene didn't cover.
	// This lambda is invoked at every exit so the background is always filled.
	// HE_SKY_TIME overrides the animation clock (deterministic headless capture of
	// time-animated sky elements — clouds/aurora — so an A/B differs only by the knob
	// under test). Normal runs use the wall clock. Mirrors the OpenGL backend.
	float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
	auto drawSky = [&]() {
		EncodeSky(renderEncoder, glm::inverse(viewProj), sunDir, skyClock,
		          GetEnvironment(), m_renderWorld.camera.position, /*lowResClouds=*/true);
	};

	// Intra-Scene element timing (draw-boundary): anchor before the first element,
	// then a sample after each element so element[i] = sample[i] - sample[i-1].
	// No-op unless a capture with draw-boundary timing is active. The "Scene"
	// total still comes from the exact stage-boundary pair on the encoder.
	m_counters.total = static_cast<uint32_t>(m_renderWorld.objects.size());

	// Trails are not in `objects` (they are their own per-frame band list), so an
	// otherwise empty scene that has one still has something to draw.
	if (m_renderWorld.objects.empty() && m_renderWorld.ribbonBatches.empty())
	{
		SamplePoint(renderEncoder, "(scene)");   // anchor
		drawSky();
		SamplePoint(renderEncoder, "Sky+Clouds");
		return;
	}

	// ── Refine bounds with real mesh AABBs (also uploads new meshes) ────────
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// ── Cull → sort → submit ────────────────────────────────────────────────
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	m_counters.visible = static_cast<uint32_t>(m_sortedIndices.size());
	if (m_sortedIndices.empty() && m_renderWorld.ribbonBatches.empty())
	{
		SamplePoint(renderEncoder, "(scene)");   // anchor
		drawSky(); // nothing visible — fill the whole background with sky
		SamplePoint(renderEncoder, "Sky+Clouds");
		return;
	}

	SamplePoint(renderEncoder, "(scene)");   // anchor before the opaque element

	// M1: the opaque pass defaults to the built-in PBR uber-shader. A material whose
	// MaterialAsset carries a custom shader overrides it PER-DRAW from the pipeline cache
	// (selected inside the loop below); materials without one keep the default.
	void* const defaultPipeline = m_scenePipeline;
	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)defaultPipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];

	// CSM shadow-map array on slot 1 (filled by EncodeShadowMap). Always bound (the
	// shader expects a texture2d_array); sampling is gated by scene.shadowEnabled.
	const bool shadows = m_renderWorld.shadow.enabled && m_shadowDepthTex;
	if (m_shadowDepthTex)
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:1];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:1];

	// Image-based-ambient cubemap on slot 2 — rebuilt from skyColor when the sun
	// moved; the scene shader samples it instead of evaluating skyColor per pixel.
	UpdateSkyEnvCube(sunDir);
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];

	// SSAO occlusion on slot 3 (filled by EncodeSSAO before this pass). Bound to the
	// white dummy when off so the sampler stays valid and ao reads as 1 (no change).
	const bool ssaoActive = m_ssaoEnabled && m_ssaoResult;
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:3];

	// GI shadow mask + probe atlases on slots 5/6/7 (filled by EncodeGIShadowRays/
	// EncodeGIProbeUpdate before this pass, only when GI is active). Bound to the
	// white dummy otherwise, same convention as aoTex. giActive gates whether the
	// shader's gi.enabled branch actually samples these — it MUST require the
	// probe atlases too, not just the shadow result: on the first GI-active frame
	// (before EnsureGIProbeGrid has run), the atlases can still be null, and
	// letting the shader sample a null-bound texture would be a GPU-side bug.
	// Slot 4 deliberately skipped — custom-material node graphs can occupy
	// fragment texture/sampler 1-4 with up to kMatMaxGraphTextures graph textures.
	const bool giActive = m_giEnabled && m_giSupported && m_giShadowResult
	                    && m_giIrradianceAtlas && m_giVisibilityAtlas;
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giShadowResult : m_dummyTexture) atIndex:5];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:5];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giIrradianceAtlas : m_dummyTexture) atIndex:6];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:6];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giVisibilityAtlas : m_dummyTexture) atIndex:7];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:7];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giLocalMaskTex : m_dummyTexture) atIndex:8];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:8];
	// FORWARD reflection results at 9/10 (heLitP pins; the GI masks moved onto
	// the shared 5/8 slots — ssr-plan P0). Dummies when the passes didn't run;
	// the ssr.x / giRefl.z gates are 0 then, the samples fold dead.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflSsrTex ? m_fwdReflSsrTex : m_dummyTexture) atIndex:9];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:9];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflGiTex ? m_fwdReflGiTex : m_dummyTexture) atIndex:10];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:10];
	// CSM array for the material preamble's GI-off fallback, pinned at 11
	// (sampling gated by heLight.csmSplits.w — same convention as slot 1).
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:11];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:11];
	// Local (point/spot) shadow atlas, pinned at 12 (sampling gated per light by params.y).
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_localShadowTex ? m_localShadowTex : m_shadowDepthTex) atIndex:12];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:12];
	// Sky env cubemap (14) + screen-space AO (15) for the material preamble's
	// image-based ambient and fog — the SAME textures the built-in shaders read
	// at 2/3, re-pinned clear of the material-texture window. Gated by
	// heLight.fog.z/.w gate the samples, so an absent cubemap (nil — legal in
	// Metal, reads zero) or a dummy AO bind here is inert. The cube slot must NOT
	// get the 2D dummy: Metal validates the texture TYPE.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:14];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:14];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:15];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:15];
	// Cloud-shadow map at texture 16 — TEXTURE only, deliberately no sampler
	// (both fragmentMain and the material preamble sample it through an inline
	// constexpr sampler; sampler indices stop at 15). White dummy when the
	// pass didn't run, and the strength gate is 0 then anyway.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_cloudShadowTex ? m_cloudShadowTex : m_dummyTexture) atIndex:16];
	// The material preamble's DDGI atlases map onto slots 6/7 — the pins the
	// scene pass already set above for the built-in shaders (Metal caps the
	// fragment stage at 16 samplers, so they cannot get their own). Nothing to
	// bind here.

	// ── Lights (clamped to the shader's 8) ──────────────────────────────────
	// Kept at function scope so the transparency pass below can re-bind it after
	// the sky pass clobbers the fragment buffer.
	SceneUniforms scene;
	scene.cameraPos  = glm::vec4(m_renderWorld.camera.position, 1.0f);
	// World forward (−Z of the camera-to-world matrix) for planar view-Z cascade
	// selection — must match the planar splits the cascades were fit with.
	scene.cameraFwd  = glm::vec4(
		-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2])), 0.0f);
	scene.lightCount = std::min(static_cast<int>(m_renderWorld.lights.size()), 8);
	for (int i = 0; i < scene.lightCount; ++i)
	{
		const LightData& l = m_renderWorld.lights[i];
		scene.lights[i].posType        = glm::vec4(l.position,  static_cast<float>(l.type));
		scene.lights[i].dirSpot        = glm::vec4(l.direction, l.spotAngleCos);
		scene.lights[i].colorIntensity = glm::vec4(l.color,     l.intensity);
		// params.y = local shadow atlas base layer; force -1 (no shadow) when the
		// atlas texture doesn't exist so the shader never samples an unbound layer.
		scene.lights[i].params         = glm::vec4(l.range,
		                                           m_localShadowTex ? static_cast<float>(l.shadowLayer) : -1.0f,
		                                           0.0f, 0.0f);
	}
	{
		const ShadowData& sh = m_renderWorld.shadow;
		const int nl = std::clamp(sh.localLayerCount, 0, ShadowData::kMaxLocalShadowLayers);
		for (int c = 0; c < nl; ++c)
			scene.localShadowVP[c] = HE::kMetalClipFix * sh.localViewProj[c];
		const int nc = std::clamp(sh.cascadeCount, 0, kCsmCascades);
		for (int c = 0; c < kCsmCascades; ++c)
			scene.cascadeVP[c] = (c < nc) ? (HE::kMetalClipFix * sh.cascadeViewProj[c])
			                              : glm::mat4(1.0f);
		scene.cascadeSplits = glm::vec4(nc > 0 ? sh.cascadeSplit[0] : 1e9f,
		                                nc > 1 ? sh.cascadeSplit[1] : 1e9f,
		                                nc > 2 ? sh.cascadeSplit[2] : 1e9f,
		                                static_cast<float>(nc));
	}
	scene.shadowEnabled = shadows ? 1 : 0;
	scene.debugCascades = m_debugShadowCascades ? 1 : 0;
	scene.sunDir        = glm::vec4(sunDir, 0.0f);
	scene.ambient       = glm::vec4(m_renderWorld.ambient, 0.0f);
	scene.fog           = glm::vec4(GetEnvironment().fogDensity,
	                                GetEnvironment().fogHeightFalloff, 0.0f, 0.0f);
	scene.viewport      = glm::vec4(static_cast<float>(width), static_cast<float>(height),
	                                ssaoActive ? 1.0f : 0.0f, 0.0f);
	scene.weather       = glm::vec4(GetEnvironment().wetness, GetEnvironment().snowAmount, 0.0f, 0.0f);
	// Forward reflection cascade (textures 9/10) — gates 0 in the deferred
	// path (its composite pass supplies the term) and when the passes didn't run.
	scene.reflCfg  = glm::vec4(m_fwdReflSsrTex ? 1.0f : 0.0f, m_ssrIntensity,
	                           m_ssrMaxRoughness, m_fwdReflGiTex ? 1.0f : 0.0f);
	scene.reflCfg2 = glm::vec4(m_giReflIntensity, m_giReflMaxRoughness,
	                           0.0f, m_giReflBlurFloor);
	// Cloud shadows — region + strength EncodeCloudShadow computed this frame
	// (strength 0 when the pass didn't run → the shader never samples the map).
	scene.cloudShadowA = m_cloudShadowParamsA;
	scene.cloudShadowB = m_cloudShadowTex ? m_cloudShadowParamsB : glm::vec4(0.0f);
	// Specular AA (A6): valid in this forward pass — it shades from the
	// fragment's own interpolated normal. 0 = off, image identical to before.
	scene.aaParams = glm::vec4(m_specularAA ? m_specularAAStrength : 0.0f, 0.0f, 0.0f, 0.0f);
	[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];

	GIUniforms giUniforms = BuildGIUniforms(giActive, m_giGridOrigin, kGIProbeSpacing,
	                                        m_giGridCounts, m_giProbesPerRow, m_giIndirectIntensity);
	[encoder setFragmentBytes:&giUniforms length:sizeof(giUniforms) atIndex:3];

#if defined(HE_HAVE_SHADERC)
	// Compact "material lighting ABI" for custom-shader materials (M2 std-lit). Bound at
	// fragment buffer 1 so the shared MaterialShaderLibrary preamble's heLit() has sun +
	// ambient. Harmless for the default PBR pipeline (which doesn't read buffer 1).
	// Filled from the DOMINANT DIRECTIONAL LIGHT (colour × intensity — the same
	// pick CSM/GI shadow along), NOT the raw environment sunColor + sky-dome
	// sunDir: the raw values are never modulated by sunUp/night, cloud cover or
	// intensity, so heLit() materials rendered permanently sun-lit at night.
	// Zero when nothing shines → heLit() correctly degrades to its ambient term.
	HE::MaterialShaderLibrary::Lighting matLight; // reused by WPO vertex-stage binds below
	FillMaterialLighting(matLight, width, height, giActive, ssaoActive, shadows, skyClock);
	[encoder setFragmentBytes:&matLight length:sizeof(matLight)
	                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif

	// Transparent (opacity < 1) draws collected during the opaque loop and replayed
	// sorted back-to-front, alpha-blended, after the sky. In deferred mode the
	// G-buffer pass already collected them (EncodeGBuffer) — take that list.
	std::vector<TPDraw> transparent;
	if (deferred) transparent = std::move(deferred->transparent);
	const glm::vec3 camPos = m_renderWorld.camera.position;

	// ── Deferred: the opaque geometry is already in the G-buffer. Draw the
	// fullscreen lighting resolve (heLitP over the G-buffer attributes — every
	// lighting input bound above is exactly what it samples), then replay the
	// forward-routed opaque draws. Skinned/sky/transparency/particles below run
	// unchanged — they are forward in both paths.
	if (deferred)
	{
		// Nothing visible means EncodeGBuffer wrote nothing this frame, so there is
		// no G-buffer to resolve — the sky fills the frame and a trail (the only
		// reason we got past the early returns above) draws over it. Before ribbons
		// this case simply returned early, so the gate reproduces that exactly.
		if (!deferred->resolveDone && !m_sortedIndices.empty())
		{
		[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_deferredResolvePipeline];
		[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
		// G-buffer inputs on the material-texture slots the resolve was pinned to.
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_gbColor0 atIndex:0];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_gbColor1 atIndex:1];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_gbColor2 atIndex:2];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_gbDepth  atIndex:3];
		for (int s = 0; s <= 3; ++s)
			[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:s];
		// World-pos reconstruction: the raster used the UNfixed GL-convention
		// viewProj (the scene pass's long-standing convention), so the stored
		// depth IS the GL ndc z — scale 1 / bias 0, y flipped (Metal's
		// gl_FragCoord origin is top-left).
		HE::MaterialShaderLibrary::ResolveUniforms ru;
		const glm::mat4 ivp = glm::inverse(viewProj);
		std::memcpy(ru.invViewProj, &ivp[0][0], 16 * sizeof(float));
		ru.depthParams[0] = -1.0f;
		ru.depthParams[1] = 1.0f;
		ru.depthParams[2] = 0.0f;
		ru.depthParams[3] = static_cast<float>(m_gbufferDebugView); // HE_DUMP_GBUFFER
#if defined(HE_HAVE_SHADERC)
		// P7: point/spot lights come from the cluster lists; the resolve's
		// heLight window shrinks to directional-only (a COPY — the full fill in
		// `matLight` keeps serving the forward-routed and transparent draws).
		if (m_deferredClustered)
		{
			HE::MaterialShaderLibrary::Lighting clusterLight = matLight;
			EncodeClusterData(renderEncoder, clusterLight, ru);
			[encoder setFragmentBytes:&clusterLight length:sizeof(clusterLight)
			                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
		}
#endif
		[encoder setFragmentBytes:&ru length:sizeof(ru) atIndex:3];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		++m_counters.draws;
#if defined(HE_HAVE_SHADERC)
		// Restore the FULL light window for everything drawn after the resolve.
		if (m_deferredClustered)
			[encoder setFragmentBytes:&matLight length:sizeof(matLight)
			                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif

		// Restore what the resolve draw clobbered for the passes below: the
		// scene-pass texture slots 1-3 and the GI uniforms on fragment buffer 3.
		if (m_shadowDepthTex)
			[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:1];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
		[encoder setFragmentBytes:&giUniforms length:sizeof(giUniforms) atIndex:3];
		} // !resolveDone — tile mode resolved inside the G-buffer pass already

		// Forward-routed opaque draws (custom materials without a G-buffer
		// variant): normal depth test + write against the blitted G-buffer depth.
		if (!deferred->forwardOpaque.empty())
		{
			[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
			void* fwdBound = nullptr;
			for (const TPDraw& t : deferred->forwardOpaque)
			{
				void* want = t.pipeline ? t.pipeline : m_scenePipeline;
				if (want != fwdBound)
				{
					[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)want];
					fwdBound = want;
				}
				if (t.pipeline)
				{
#if defined(HE_HAVE_SHADERC)
					[encoder setFragmentBytes:&matLight length:sizeof(matLight)
					                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif
					float padded[64] = { 0 };
					std::memcpy(padded, t.params.data(),
					            std::min(t.params.size(), size_t(64)) * sizeof(float));
					[encoder setFragmentBytes:padded length:sizeof(padded) atIndex:2];
					for (int i = 0; i < t.gtexCount; ++i)
						if (t.gtex[i])
						{
							[encoder setFragmentTexture:(__bridge id<MTLTexture>)t.gtex[i] atIndex:(i + 1)];
							[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:(i + 1)];
						}
					if (t.wpo)
					{
#if defined(HE_HAVE_SHADERC)
						[encoder setVertexBytes:&matLight length:sizeof(matLight) atIndex:2];
#endif
						[encoder setVertexBytes:padded length:sizeof(padded) atIndex:3];
					}
				}
				[encoder setVertexBuffer:(__bridge id<MTLBuffer>)t.vbuf offset:0 atIndex:0];
				[encoder setVertexBytes:&t.u length:sizeof(t.u) atIndex:1];
				[encoder setFragmentTexture:(__bridge id<MTLTexture>)t.tex atIndex:0];
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                    indexCount:t.indexCount
				                     indexType:MTLIndexTypeUInt32
				                   indexBuffer:(__bridge id<MTLBuffer>)t.ibuf
				             indexBufferOffset:0];
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
			}
			// Restore the shared-slot state the material draws may have replaced.
			[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_scenePipeline];
		}
		[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	}

	// Build this frame's draw calls through the render graph, then replay them.
	// GeometryPass turns the sorted visible objects into DrawCalls; the encoder
	// state (pipeline, lights, camera) is set up above and the meshes are
	// resolved by UUID here, exactly as the immediate loop used to.
	if (!deferred && m_renderGraph.empty())
		m_renderGraph.addPass(std::make_unique<GeometryPass>());

	// Per-pass sink: bind the pass's target, then replay its draws. Today the
	// only pass renders to the backbuffer (the active scene encoder); offscreen
	// targets (id != backbuffer) arrive with shadows/HDR. Skipped in deferred
	// mode — the resolve above replaced the opaque loop.
	if (!deferred)
	m_renderGraph.execute(m_renderWorld, m_sortedIndices,
		[&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
	{
		if (io.output.id != kBackbufferTarget) return;
		// Draws arrive sorted by mesh id, so consecutive draws usually share the
		// same mesh (and often material). Memoise the last resolved mesh/material
		// to skip repeated cache + content-manager lookups (ResolveMaterialParams
		// re-fetches the material every call). Pure per-id resolves → identical
		// results, so this is behaviour-preserving.
		HE::UUID  lastMeshId{};       const GpuMesh* cMesh = nullptr; bool meshValid = false;
		HE::UUID  lastMatId{};        bool matValid = false;
		void*     cOverrideTex = nullptr; bool cHasOverride = false;
		glm::vec3 cBaseColor(1.0f);   float cMetallic = 0.0f, cRoughness = 0.5f; bool cHasMat = false;
		float     cOpacity = 1.0f;
		// Per-material pipeline (M1): resolved when the material changes; null = the
		// default PBR pipeline. boundPipeline tracks what's set on the encoder so we
		// only re-bind on an actual change (draws arrive sorted, so this is rare).
		void*     cMaterialPipeline      = nullptr;
		void*     cMaterialPipelineBlend = nullptr; // alpha-blended variant (transparency pass)
		bool      cMaterialWpo           = false;   // custom WPO vertex → vertex-stage binds
		void*     boundPipeline     = defaultPipeline;
		const std::vector<float>* cMaterialParams = nullptr; // HeParams data (buffer 2)
		void* cGraphTex[HE::kMatMaxGraphTextures] = { nullptr }; // node-graph textures (units 1..4)
		int   cGraphTexCount = 0;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			UnlitUniforms u;
			u.mvp   = viewProj * dc.transform;
			u.model = dc.transform;

			// An explicit MaterialComponent override wins over the mesh's own
			// base-color texture when present and resolvable.
			// PBR scalars from the material override; defaults otherwise.
			if (!matValid || dc.materialAssetId != lastMatId)
			{
				cOverrideTex = nullptr;
				cHasOverride = ResolveMaterialTexture(dc.materialAssetId, cOverrideTex);
				cBaseColor   = glm::vec3(1.0f); cMetallic = 0.0f; cRoughness = 0.5f; cOpacity = 1.0f;
				cHasMat      = ResolveMaterialParams(dc.materialAssetId, cBaseColor, cMetallic, cRoughness, cOpacity);
				lastMatId    = dc.materialAssetId; matValid = true;
#if defined(HE_HAVE_SHADERC)
				// Per-material shader: a MaterialAsset with customShaderFragGlsl gets its
				// own cross-compiled pipeline (cached by source hash); else the default.
				cMaterialPipeline = nullptr;
				cMaterialPipelineBlend = nullptr;
				cMaterialWpo      = false;
				cMaterialParams   = nullptr;
				cGraphTexCount    = 0;
				{
					uint64_t shKey; std::string shFrag, shVert;
					if (ResolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert))
					{
						// Prefer a shader precompiled into the pack for this backend (no
						// runtime cross-compile); else cross-compile via the shared library.
						const MaterialShaderVariant* pre = nullptr;
						if (const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
							for (const auto& var : ma->precompiledShaders)
								if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { pre = &var; break; }
						cMaterialWpo      = !shVert.empty();
						cMaterialPipeline = GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre);
						// Translucent-routed materials additionally need the alpha-blended
						// pipeline variant for the transparency pass. Built unconditionally
						// (not gated on this material's own opacity): a per-instance tint
						// (RenderObject::instanceTint — particle color/alpha-over-life) can
						// push an otherwise-opaque material's effective alpha below 1 on a
						// per-draw basis, after this cache-miss-only block has already run.
						if (cMaterialPipeline)
							cMaterialPipelineBlend =
								GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre, /*blend=*/true);
						if (const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
						{
							if (!ma->shaderParamData.empty()) cMaterialParams = &ma->shaderParamData;
							// Node-graph project textures → fragment texture units 1..4.
							const size_t nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
								std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
							for (size_t i = 0; i < nTex; ++i)
							{
								const HE::UUID    id = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
								const std::string p  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
								cGraphTex[cGraphTexCount++] = ResolveGraphTexture(id, p);
							}
						}
					}
				}
#endif
			}
			u.pbr = glm::vec4(cMetallic, cRoughness, cOpacity, 0.0f);

			// Resolve the mesh; entities without one fall back to the default cube.
			bool meshWasResolved = false;
			if (!meshValid || dc.meshAssetId != lastMeshId)
			{
				cMesh      = ResolveMesh(dc.meshAssetId);
				lastMeshId = dc.meshAssetId; meshValid = true;
				meshWasResolved = true;
			}
			const GpuMesh* drawMesh = cMesh;
			if (!drawMesh) { drawMesh = ResolveMesh(HE::kDefaultCubeMeshId); meshWasResolved = true; }
			if (!drawMesh) continue;
			// ResolveMesh LOADS — the mesh's own baked material, in loose content
			// through loadAsset and in a packaged build by streaming it out of the
			// pak. Either way the material pool grows, and it is a dense vector:
			// cMaterialParams, taken a few lines above, then points into moved
			// memory and is dereferenced below for every draw of this material.
			// Re-taken rather than reordered, and only on a mesh miss, so the
			// steady state costs nothing.
			if (meshWasResolved && cMaterialParams)
			{
				const MaterialAsset* ma = m_contentManager
					? m_contentManager->getMaterial(dc.materialAssetId) : nullptr;
				cMaterialParams = (ma && !ma->shaderParamData.empty()) ? &ma->shaderParamData : nullptr;
			}
			id<MTLBuffer> vertexBuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
			id<MTLBuffer> indexBuf  = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
			NSUInteger    indexCount = (NSUInteger)drawMesh->indexCount;
			void*         meshTex = drawMesh->texture;

			void* effectiveTex = cHasOverride ? cOverrideTex : meshTex;
			void* texPtr = effectiveTex ? effectiveTex : m_dummyTexture;
			id<MTLTexture> texture = (__bridge id<MTLTexture>)texPtr;
			u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, dc.receivesShadow ? 0.0f : 1.0f, 0, 0);

			// Base tint: material baseColor if assigned, else white when textured
			// (texture unchanged) or the flat fallback color when not.
			glm::vec3 baseColor = cBaseColor;
			if (!cHasMat)
				baseColor = effectiveTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);
			// Per-instance tint (particle color/alpha-over-life, see
			// RenderObject::instanceTint) — identity for everything else, so this is
			// a no-op outside particles. u/u.pbr are per-draw-call locals (not the
			// memoised cBaseColor/cOpacity), so mutating them here is safe.
			baseColor *= glm::vec3(dc.instanceTint);
			u.pbr.z   *= dc.instanceTint.a;
			u.color = glm::vec4(baseColor, 1.0f);

			// Draw one instance at its own world transform. GeometryPass batches consecutive
			// same-mesh + same-material objects into ONE DrawCall carrying every transform in
			// dc.instanceTransforms; the shared uniforms (color/pbr/flags/texture) are set
			// above, only mvp/model differ per instance. Without this only dc.transform drew
			// and every OTHER copy of an identical mesh vanished (the other backends already
			// iterate instanceTransforms).
			auto drawInstance = [&](const glm::mat4& xform)
			{
				UnlitUniforms ui = u;
				ui.mvp   = viewProj * xform;
				ui.model = xform;
				// Tinted opacity (u.pbr.z), not the raw material cOpacity — Metal
				// resolves the material×instance-tint product itself above, so the
				// shared threshold is applied to that instead of RenderSorter::
				// isTransparent (which re-derives it from a DrawCall).
				if (ui.pbr.z < RenderSorter::kOpaqueOpacityThreshold)
				{
					TPDraw t{ ui, (__bridge void*)vertexBuf, (__bridge void*)indexBuf,
					          indexCount, texPtr, RenderSorter::backToFrontKey(xform, camPos) };
					// Translucent graph materials keep their own (blended) pipeline + state.
					if (cMaterialPipelineBlend)
					{
						t.pipeline = cMaterialPipelineBlend;
						t.wpo      = cMaterialWpo;
						if (cMaterialParams) t.params = *cMaterialParams;
						if (!dc.paramOverride.empty()) t.params = dc.paramOverride;
						for (int i = 0; i < cGraphTexCount; ++i) t.gtex[i] = cGraphTex[i];
						t.gtexCount = cGraphTexCount;
					}
					transparent.push_back(std::move(t));
					return; // drawn in the transparency pass below
				}
				// M1: switch to this material's cross-compiled pipeline if it has one,
				// else the default PBR pipeline. Re-bound only on a real change.
				void* wantPipeline = cMaterialPipeline ? cMaterialPipeline : defaultPipeline;
				if (wantPipeline != boundPipeline)
				{
					[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)wantPipeline];
					boundPipeline = wantPipeline;
				}
				// Exposed graph parameters (HeParams UBO, fragment buffer 2) — uploaded per
				// draw so parameter edits take effect without any shader recompile. Padded
				// to the shader's declared vec4 v[16] so the debug layer never sees a
				// shorter-than-declared buffer.
				if (cMaterialPipeline && (cMaterialParams || !dc.paramOverride.empty()))
				{
					// Per-entity override (already the full merged block) wins over the
					// material's shared shaderParamData — never batched, so per-draw.
					const std::vector<float>& src =
						!dc.paramOverride.empty() ? dc.paramOverride : *cMaterialParams;
					float padded[64] = { 0 };
					std::memcpy(padded, src.data(),
					            std::min(src.size(), size_t(64)) * sizeof(float));
					[encoder setFragmentBytes:padded length:sizeof(padded) atIndex:2];
				}
				// Node-graph project textures at fragment texture units 1..4 (+ linear
				// sampler). heTexP{k} = texture (k+1); heTex0 stays at unit 0 (bound below).
				if (cMaterialPipeline)
					for (int i = 0; i < cGraphTexCount; ++i)
						if (cGraphTex[i])
						{
							[encoder setFragmentTexture:(__bridge id<MTLTexture>)cGraphTex[i] atIndex:(i + 1)];
							[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:(i + 1)];
						}
				// Landscape layer weightmap → MSL texture 13 (preamble binding 14).
				// PER DRAW, not per material: it belongs to the terrain the chunk is
				// part of, so two landscapes can share a material and paint apart.
				// Objects that aren't landscape chunks get the 1x1 (1,0,0,0) default,
				// which makes a layer-blend node resolve to layer 0 instead of black.
				if (cMaterialPipeline)
				{
					void* wm = dc.weightmapTextureId != HE::UUID{}
						? ResolveGraphTexture(dc.weightmapTextureId, {})
						: nullptr;
					if (!wm) wm = ResolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {});
					if (wm)
					{
						[encoder setFragmentTexture:(__bridge id<MTLTexture>)wm atIndex:13];
						[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:13];
					}
				}
				[encoder setVertexBuffer:vertexBuf offset:0 atIndex:0];
				[encoder setVertexBytes:&ui length:sizeof(ui) atIndex:1];
				// WPO materials read HeLighting (time) + HeParams in the VERTEX stage too
				// (custom vertex pins them to buffers 2/3).
				if (cMaterialPipeline && cMaterialWpo)
				{
					[encoder setVertexBytes:&matLight length:sizeof(matLight) atIndex:2];
					float vpad[64] = { 0 };
					const std::vector<float>* vsrc =
						!dc.paramOverride.empty() ? &dc.paramOverride : cMaterialParams;
					if (vsrc)
						std::memcpy(vpad, vsrc->data(),
						            std::min(vsrc->size(), size_t(64)) * sizeof(float));
					[encoder setVertexBytes:vpad length:sizeof(vpad) atIndex:3];
				}
				[encoder setFragmentTexture:texture atIndex:0];
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                    indexCount:indexCount
				                     indexType:MTLIndexTypeUInt32
				                   indexBuffer:indexBuf
				             indexBufferOffset:0];
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3);
			};
			// A3 on Metal (plan §4): one draw for the whole batch, every instance's
			// {mvp, model} in a device array at vertex buffer 5. `fits` is decided
			// once per DrawCall because every input to it is constant over the batch —
			// that is exactly the condition under which GeometryPass batched at all.
			static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be mvp+model");
			const size_t instCount = dc.instanceTransforms.size();
			const bool fits = instCount > 0
			               && metalInstancingEnabled()
			               && m_sceneInstancedPipeline
			               && instCount <= k_maxInstances
			               // Only built-in PBR is instanced. A node-graph material has
			               // its own pipeline and its own vertex stage; instancing it
			               // would silently swap the look (the GL path's open bug,
			               // plan §2/§6.4). Optics before draw-call savings.
			               && cMaterialPipeline == nullptr
			               && dc.paramOverride.empty()
			               // Translucent batches must reach the sorted back-to-front
			               // replay the loop feeds — the instanced draw cannot sort.
			               && u.pbr.z >= RenderSorter::kOpaqueOpacityThreshold;
			if (fits)
			{
				if (m_sceneInstancedPipeline != boundPipeline)
				{
					[encoder setRenderPipelineState:
						(__bridge id<MTLRenderPipelineState>)m_sceneInstancedPipeline];
					boundPipeline = m_sceneInstancedPipeline;
				}
				std::vector<glm::mat4> xf;
				xf.reserve(instCount * 2);
				for (const glm::mat4& t : dc.instanceTransforms)
				{
					xf.push_back(viewProj * t); // mvp
					xf.push_back(t);            // model
				}
				// Fresh buffer per batch, same convention as the particle path: the
				// encoder holds every bound resource until its command buffer
				// completes, so there is nothing to hand-synchronise. A per-frame ring
				// would need frame-in-flight tracking this backend does not have.
				id<MTLDevice> dev = (__bridge id<MTLDevice>)m_device;
				id<MTLBuffer> instBuf = [dev newBufferWithBytes:xf.data()
					length:xf.size() * sizeof(glm::mat4)
					options:MTLResourceStorageModeShared];
				[encoder setVertexBuffer:vertexBuf offset:0 atIndex:0];
				[encoder setVertexBytes:&u length:sizeof(u) atIndex:1]; // mvp/model unused here
				[encoder setVertexBuffer:instBuf offset:0 atIndex:5];
				[encoder setFragmentTexture:texture atIndex:0];
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                    indexCount:indexCount
				                     indexType:MTLIndexTypeUInt32
				                   indexBuffer:indexBuf
				             indexBufferOffset:0
				                 instanceCount:(NSUInteger)instCount];
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3)
				                 * static_cast<uint32_t>(instCount);
			}
			else if (dc.instanceTransforms.empty())
				drawInstance(dc.transform);
			else
				for (const glm::mat4& t : dc.instanceTransforms)
					drawInstance(t);
		}
	});
	SamplePoint(renderEncoder, "Opaque");

	// ── Skinned geometry: drawn after opaque, before sky so they occlude the background.
	EncodeSkinnedObjects(renderEncoder, viewProj, shadows, &scene);
	SamplePoint(renderEncoder, "Skinned");

	// Sky LAST — fills the background pixels the geometry didn't cover.
	drawSky();
	SamplePoint(renderEncoder, "Sky+Clouds");

	// Motion trails join the blended list here rather than getting a pass of
	// their own: same vertex layout, same sort, same state. Collected AFTER the
	// opaque loop so they cannot influence the shadow fit or the sort of anything
	// else, and before the sort below so they take part in it.
	CollectRibbonDraws(transparent, viewProj, camPos);

	// ── Transparency pass: sorted, alpha-blended draws over the opaque scene +
	// sky. Back-to-front; depth-tested against the opaque geometry but no depth
	// write (reuses the sky's LessEqual/no-write state). The sky pass clobbered the
	// fragment bindings, so re-bind the scene's shadow/ambient/AO state + uniforms.
	if (!transparent.empty())
	{
		// Farthest first (distSq = RenderSorter::backToFrontKey). Not stable — draws
		// at exactly equal distance may come out in either order, as in every backend.
		std::sort(transparent.begin(), transparent.end(),
		          [](const TPDraw& a, const TPDraw& b) { return a.distSq > b.distSq; });
		[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_sceneBlendPipeline];
		[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState]; // LessEqual, no write
		[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:1]; // CSM array; sampling gated by shadowEnabled
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:1];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:3];
		[encoder setFragmentBytes:&giUniforms length:sizeof(giUniforms) atIndex:3];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giShadowResult : m_dummyTexture) atIndex:5];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:5];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giIrradianceAtlas : m_dummyTexture) atIndex:6];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:6];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giVisibilityAtlas : m_dummyTexture) atIndex:7];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:7];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(giActive ? m_giLocalMaskTex : m_dummyTexture) atIndex:8];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:8];
		// FORWARD reflection results at 9/10 (heLitP pins; GI masks moved onto
		// the shared 5/8 slots — ssr-plan P0). Dummies when the passes didn't run.
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflSsrTex ? m_fwdReflSsrTex : m_dummyTexture) atIndex:9];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:9];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_fwdReflGiTex ? m_fwdReflGiTex : m_dummyTexture) atIndex:10];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:10];
		// CSM array for the material preamble's GI-off fallback, pinned at 11.
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:11];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:11];
		// Local (point/spot) shadow atlas, pinned at 12.
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_localShadowTex ? m_localShadowTex : m_shadowDepthTex) atIndex:12];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:12];
		void* tpBound = (__bridge void*)(__bridge id<MTLRenderPipelineState>)m_sceneBlendPipeline;
		for (const TPDraw& t : transparent)
		{
			// Custom translucent materials bind their own blended pipeline + state; the
			// engine's default blend PSO covers everything else. matLight@1 is still bound.
			void* want = t.pipeline ? t.pipeline : (__bridge void*)(__bridge id<MTLRenderPipelineState>)m_sceneBlendPipeline;
			if (want != tpBound)
			{
				[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)want];
				tpBound = want;
			}
			if (t.pipeline)
			{
				[encoder setFragmentBytes:&matLight length:sizeof(matLight)
				                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
				float padded[64] = { 0 };
				std::memcpy(padded, t.params.data(),
				            std::min(t.params.size(), size_t(64)) * sizeof(float));
				[encoder setFragmentBytes:padded length:sizeof(padded) atIndex:2];
				for (int i = 0; i < t.gtexCount; ++i)
					if (t.gtex[i])
					{
						[encoder setFragmentTexture:(__bridge id<MTLTexture>)t.gtex[i] atIndex:(i + 1)];
						[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:(i + 1)];
					}
				if (t.wpo)
				{
					[encoder setVertexBytes:&matLight length:sizeof(matLight) atIndex:2];
					[encoder setVertexBytes:padded length:sizeof(padded) atIndex:3];
				}
			}
			[encoder setVertexBuffer:(__bridge id<MTLBuffer>)t.vbuf offset:0 atIndex:0];
			[encoder setVertexBytes:&t.u length:sizeof(t.u) atIndex:1];
			[encoder setFragmentTexture:(__bridge id<MTLTexture>)t.tex atIndex:0];
			[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                    indexCount:t.indexCount
			                     indexType:MTLIndexTypeUInt32
			                   indexBuffer:(__bridge id<MTLBuffer>)t.ibuf
			             indexBufferOffset:0];
			++m_counters.draws;
			m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
		}
	}
	SamplePoint(renderEncoder, "Transparent");

	// GPU weather particles: simulated by the compute pass (EncodeFrame), drawn here
	// as alpha-blended billboards over the opaque scene + sky.
	DrawGpuParticles(renderEncoder, viewProj, m_renderWorld.camera.position);
	DrawParticleGraphBatches(renderEncoder, viewProj, m_renderWorld.camera.view);
	SamplePoint(renderEncoder, "Particles");
}

// ─── heLitP lighting-ABI fill (custom materials + deferred resolve) ──────────
// Extracted verbatim from EncodeScene so the tile-memory resolve (which runs
// inside the G-buffer pass, before EncodeScene) fills the SAME block — one
// implementation, no drift. Filled from the DOMINANT DIRECTIONAL LIGHT (colour ×
// intensity — the same pick CSM/GI shadow along), NOT the raw environment
// sunColor: the raw values are never modulated by sunUp/night or cloud cover.
void MetalRenderer::FillMaterialLighting(HE::MaterialShaderLibrary::Lighting& matLight,
                                         int width, int height, bool giActive,
                                         bool ssaoActive, bool shadows, float skyClock)
{
	glm::vec3 matSunDir, matSunColor;
	m_renderWorld.dominantDirectionalLight(matSunDir, matSunColor);
	const glm::vec3 am = m_renderWorld.ambient;
	matLight.sunDir[0]   = matSunDir.x; matLight.sunDir[1] = matSunDir.y; matLight.sunDir[2] = matSunDir.z;
	matLight.sunDir[3]   = skyClock; // engine seconds — the node graph's Time input
	matLight.camPos[0]   = m_renderWorld.camera.position.x;
	matLight.camPos[1]   = m_renderWorld.camera.position.y;
	matLight.camPos[2]   = m_renderWorld.camera.position.z;
	matLight.sunColor[0] = matSunColor.r; matLight.sunColor[1] = matSunColor.g; matLight.sunColor[2] = matSunColor.b;
	matLight.ambient[0]  = am.r;          matLight.ambient[1]  = am.g;          matLight.ambient[2]  = am.b;
	matLight.giParams[0] = static_cast<float>(width);
	matLight.giParams[1] = static_cast<float>(height);
	// FORWARD reflection cascade gates for heLitP (samplers 31/32 → slots 9/10).
	// Null in the deferred path (its composite pass owns the term — the resolve
	// callers additionally set ssr.w to skip ambSpec entirely).
	matLight.ssr[0]    = m_fwdReflSsrTex ? 1.0f : 0.0f;
	matLight.ssr[1]    = m_ssrIntensity;
	matLight.ssr[2]    = m_ssrMaxRoughness;
	matLight.giRefl[0] = m_giReflIntensity;
	matLight.giRefl[1] = m_giReflMaxRoughness;
	matLight.giRefl[2] = m_fwdReflGiTex ? 1.0f : 0.0f;
	matLight.giRefl[3] = m_giReflBlurFloor; // resolution blur floor (deferred composite)
	matLight.giParams[2] = giActive ? 1.0f : 0.0f;
	// Full light window for heLitP() — same first-8 order as the built-in
	// PBR shaders. Shared fill (HE::FillMaterialLightWindow); it also writes
	// the per-light atlas layer into lightParams[i].y when the local shadow
	// texture is bound this frame.
	HE::FillMaterialLightWindow(m_renderWorld, matLight,
	                            /*localShadowsActive=*/m_localShadowTex != nullptr);
	// Local (point/spot) shadow atlas for heLitP — the same matrices the
	// built-in shaders sample with, Metal depth remap AND top-left UV origin
	// pre-baked (uvFlipY * kMetalClipFix, exactly like csmVP below) so the
	// shared preamble's heLocalShadowFactor stays convention-free. The layer
	// gate rides in lightParams[li].y (layer+1, 0 = none), written by the
	// HE::FillMaterialLightWindow call above via its localShadowsActive flag.
	if (m_localShadowTex)
	{
		const ShadowData& lsh = m_renderWorld.shadow;
		const int nlLoc = std::clamp(lsh.localLayerCount, 0, ShadowData::kMaxLocalShadowLayers);
		glm::mat4 lsFlipY(1.0f);
		lsFlipY[1][1] = -1.0f;
		for (int c = 0; c < nlLoc; ++c)
		{
			const glm::mat4 m = lsFlipY * HE::kMetalClipFix * lsh.localViewProj[c];
			std::memcpy(matLight.localShadowVP[c], &m[0][0], 16 * sizeof(float));
		}
	}
	// CSM fallback for graph materials (v2.2): only meaningful when the GI
	// masks are absent this frame — heLitP's directional lights then sample
	// the SAME cascade array as the built-in shaders (texture 11). Metal's
	// depth remap (kMetalClipFix) AND the shadow map's top-left UV origin
	// are pre-baked into the matrices, so the shared preamble's
	// heCsmShadow() stays convention-free (uv = p.xy*0.5+0.5, z in [0,1]).
	if (!giActive && shadows && m_shadowDepthTex)
	{
		const ShadowData& sh = m_renderWorld.shadow;
		const int nc = std::min(std::clamp(sh.cascadeCount, 0, kCsmCascades), 3);
		glm::mat4 uvFlipY(1.0f);
		uvFlipY[1][1] = -1.0f;
		for (int c = 0; c < nc; ++c)
		{
			const glm::mat4 m = uvFlipY * HE::kMetalClipFix * sh.cascadeViewProj[c];
			std::memcpy(matLight.csmVP[c], &m[0][0], 16 * sizeof(float));
		}
		matLight.csmSplits[0] = nc > 0 ? sh.cascadeSplit[0] : 1e9f;
		matLight.csmSplits[1] = nc > 1 ? sh.cascadeSplit[1] : 1e9f;
		matLight.csmSplits[2] = nc > 2 ? sh.cascadeSplit[2] : 1e9f;
		matLight.csmSplits[3] = static_cast<float>(nc);
		const glm::vec3 camFwd =
			-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
		matLight.camFwd[0] = camFwd.x;
		matLight.camFwd[1] = camFwd.y;
		matLight.camFwd[2] = camFwd.z;
	}
	// Aerial perspective + the "is it bound" gates for the shared ambient
	// inputs (MSL 14/15); heLitP falls back to the flat ambient and skips fog
	// when they are absent (preview/UI passes).
	matLight.fog[0] = GetEnvironment().fogDensity;
	matLight.fog[1] = GetEnvironment().fogHeightFalloff;
	matLight.fog[2] = m_skyEnvCube ? 1.0f : 0.0f;
	matLight.fog[3] = ssaoActive   ? 1.0f : 0.0f;
	// Weather surface response — the same EnvironmentComponent values the
	// built-in shaders read (SceneUniforms::weather).
	matLight.weather[0] = GetEnvironment().wetness;
	matLight.weather[1] = GetEnvironment().snowAmount;
	// Cloud shadows — the same region/strength the built-in shaders get
	// (SceneUniforms::cloudShadowA/B), so heLitP materials darken identically.
	{
		const glm::vec4 csA = m_cloudShadowParamsA;
		const glm::vec4 csB = m_cloudShadowTex ? m_cloudShadowParamsB : glm::vec4(0.0f);
		std::memcpy(matLight.cloudShadowA, &csA[0], 4 * sizeof(float));
		std::memcpy(matLight.cloudShadowB, &csB[0], 4 * sizeof(float));
	}
	// DDGI probe grid — the SAME values BuildGIUniforms hands the built-in
	// shaders, so heLitP's indirect diffuse matches theirs instead of
	// falling back to flat ambient while GI is on.
	{
		const GIUniforms gu = BuildGIUniforms(giActive, m_giGridOrigin, kGIProbeSpacing,
		                                      m_giGridCounts, m_giProbesPerRow,
		                                      m_giIndirectIntensity);
		for (int k = 0; k < 4; ++k)
		{
			matLight.giGridOrigin[k] = gu.gridOrigin[k];
			matLight.giGridCounts[k] = gu.gridCounts[k];
		}
		matLight.giProbe[0] = gu.params.x;
		matLight.giProbe[1] = (giActive && m_giIrradianceAtlas && m_giVisibilityAtlas)
			? 1.0f : 0.0f;
	}
	// Specular AA (A6). y = 1 means "this fill feeds a GEOMETRY pass", where the
	// fragment's own normal and its derivatives exist. The forward pass is one;
	// the deferred resolve and the SSR composite are NOT and clear it right after
	// calling this.
	matLight.specAA[0] = m_specularAA ? m_specularAAStrength : 0.0f;
	matLight.specAA[1] = 1.0f;
}

// ─── Clustered lighting build (plan P7) ──────────────────────────────────────
// CPU scatter: every point/spot light's projected bounds mark the screen-tile ×
// log-z-slice clusters it can touch; the resolve then shades only its cluster's
// list. Cheap (few hundred lights × few touched clusters) and re-built fresh
// per frame like the particle instance buffers — no CPU/GPU sync hazards.
// Rewrites matLight's window to DIRECTIONAL lights only: point/spot shading now
// belongs exclusively to the cluster list, or every windowed light would be
// counted twice.
void MetalRenderer::EncodeClusterData(void* renderEncoder,
                                      HE::MaterialShaderLibrary::Lighting& matLight,
                                      HE::MaterialShaderLibrary::ResolveUniforms& ru)
{
	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	const glm::mat4 viewProj =
		m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::vec3 camPos = m_renderWorld.camera.position;
	const glm::vec3 camFwd =
		-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
	const float sliceScale =
		static_cast<float>(kClusterGridZ) / std::log(kClusterFar / kClusterNear);

	// Pack the local lights (4 vec4 each) + scatter them into the grid.
	std::vector<glm::vec4> lightData;
	const int gridTotal = kClusterGridX * kClusterGridY * kClusterGridZ;
	std::vector<std::vector<uint32_t>> cells(gridTotal);
	int lightCount = 0;
	// GI local-mask channel bookkeeping: the ray-traced mask (heGILocal) covers
	// the first 4 NON-directional lights of the first-8 window, counted exactly
	// like heLitP's localIdx — reproduce that scan so cluster lights keep their
	// mask channel when GI is on.
	const bool giMasksValid = m_giEnabled && m_giSupported && m_giShadowResult
	                       && m_giIrradianceAtlas && m_giVisibilityAtlas;
	int extractorIndex = -1;
	int windowLocalIdx = 0;
	for (const LightData& l : m_renderWorld.lights)
	{
		++extractorIndex;
		int maskChannel = -1; // -1 = no ray-traced mask for this light
		if (l.type != 0 && extractorIndex < 8)
		{
			if (giMasksValid && windowLocalIdx < 4) maskChannel = windowLocalIdx;
			++windowLocalIdx;
		}
		if (l.type == 0) continue; // directional stays in the heLight window
		if (lightCount >= kMaxClusteredLights) break;
		const float range = std::max(l.range, 1e-4f);
		// Depth slice range along the camera forward.
		const float viewZ = glm::dot(l.position - camPos, camFwd);
		const float zMin  = viewZ - range, zMax = viewZ + range;
		if (zMax < kClusterNear || zMin > kClusterFar) continue; // outside the grid
		auto slice = [&](float z) {
			return std::clamp(static_cast<int>(std::log(std::max(z, kClusterNear) / kClusterNear)
			                                   * sliceScale), 0, kClusterGridZ - 1);
		};
		const int z0 = slice(zMin), z1 = slice(zMax);
		// Screen rect: project the 8 corners of the world-space bounding box.
		// A corner at/behind the near plane makes the projection unusable →
		// conservatively cover the full screen for that light.
		float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
		bool fullRect = false;
		for (int c = 0; c < 8 && !fullRect; ++c)
		{
			const glm::vec3 corner = l.position + range * glm::vec3(
				(c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f);
			const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
			if (clip.w <= kClusterNear) { fullRect = true; break; }
			// Metal's gl_FragCoord/uv origin is TOP-LEFT — flip v so the CPU
			// scatter and the shader's cluster pick agree.
			const float u = clip.x / clip.w * 0.5f + 0.5f;
			const float v = 1.0f - (clip.y / clip.w * 0.5f + 0.5f);
			u0 = std::min(u0, u); u1 = std::max(u1, u);
			v0 = std::min(v0, v); v1 = std::max(v1, v);
		}
		int x0 = 0, x1 = kClusterGridX - 1, y0 = 0, y1 = kClusterGridY - 1;
		if (!fullRect)
		{
			if (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f) continue; // off-screen
			x0 = std::clamp(static_cast<int>(u0 * kClusterGridX), 0, kClusterGridX - 1);
			x1 = std::clamp(static_cast<int>(u1 * kClusterGridX), 0, kClusterGridX - 1);
			y0 = std::clamp(static_cast<int>(v0 * kClusterGridY), 0, kClusterGridY - 1);
			y1 = std::clamp(static_cast<int>(v1 * kClusterGridY), 0, kClusterGridY - 1);
		}
		const uint32_t li = static_cast<uint32_t>(lightCount++);
		lightData.push_back(glm::vec4(l.position, static_cast<float>(l.type)));
		lightData.push_back(glm::vec4(l.direction, l.spotAngleCos));
		lightData.push_back(glm::vec4(l.color, l.intensity));
		lightData.push_back(glm::vec4(range,
			(m_localShadowTex && l.shadowLayer >= 0) ? static_cast<float>(l.shadowLayer + 1) : 0.0f,
			static_cast<float>(maskChannel + 1), // GI mask channel + 1 (0 = none)
			0.0f));
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					cells[(z * kClusterGridY + y) * kClusterGridX + x].push_back(li);
	}

	// Flatten: per-cluster {offset, count} + one index list.
	std::vector<uint32_t> grid(gridTotal * 2, 0u);
	std::vector<uint32_t> indices;
	for (int cIdx = 0; cIdx < gridTotal; ++cIdx)
	{
		grid[cIdx * 2 + 0] = static_cast<uint32_t>(indices.size());
		grid[cIdx * 2 + 1] = static_cast<uint32_t>(cells[cIdx].size());
		indices.insert(indices.end(), cells[cIdx].begin(), cells[cIdx].end());
	}
	if (lightData.empty()) lightData.push_back(glm::vec4(0.0f)); // never a 0-byte buffer
	if (indices.empty())   indices.push_back(0u);

	// Fresh per-frame buffers (newBufferWithBytes) — the encoder retains them.
	id<MTLBuffer> lightBuf = [device newBufferWithBytes:lightData.data()
	                                             length:lightData.size() * sizeof(glm::vec4)
	                                            options:MTLResourceStorageModeShared];
	id<MTLBuffer> gridBuf  = [device newBufferWithBytes:grid.data()
	                                             length:grid.size() * sizeof(uint32_t)
	                                            options:MTLResourceStorageModeShared];
	id<MTLBuffer> idxBuf   = [device newBufferWithBytes:indices.data()
	                                             length:indices.size() * sizeof(uint32_t)
	                                            options:MTLResourceStorageModeShared];
	[encoder setFragmentBuffer:lightBuf offset:0 atIndex:4];
	[encoder setFragmentBuffer:gridBuf  offset:0 atIndex:5];
	[encoder setFragmentBuffer:idxBuf   offset:0 atIndex:6];

	ru.clusterParams[0]  = static_cast<float>(kClusterGridX);
	ru.clusterParams[1]  = static_cast<float>(kClusterGridY);
	ru.clusterParams[2]  = static_cast<float>(kClusterGridZ);
	ru.clusterParams[3]  = sliceScale;
	ru.clusterCamFwd[0]  = camFwd.x;
	ru.clusterCamFwd[1]  = camFwd.y;
	ru.clusterCamFwd[2]  = camFwd.z;
	ru.clusterCamFwd[3]  = kClusterNear;

	// heLight window → directional lights only (same field conventions as
	// HE::FillMaterialLightWindow, which packed the mixed first-8 window).
	int nDir = 0;
	for (const LightData& l : m_renderWorld.lights)
	{
		if (l.type != 0 || nDir >= 8) continue;
		matLight.lightPos[nDir][0] = l.position.x;
		matLight.lightPos[nDir][1] = l.position.y;
		matLight.lightPos[nDir][2] = l.position.z;
		matLight.lightPos[nDir][3] = 0.0f; // directional
		matLight.lightDir[nDir][0] = l.direction.x;
		matLight.lightDir[nDir][1] = l.direction.y;
		matLight.lightDir[nDir][2] = l.direction.z;
		matLight.lightDir[nDir][3] = l.spotAngleCos;
		matLight.lightColor[nDir][0] = l.color.r;
		matLight.lightColor[nDir][1] = l.color.g;
		matLight.lightColor[nDir][2] = l.color.b;
		matLight.lightColor[nDir][3] = l.intensity;
		matLight.lightParams[nDir][0] = l.range;
		matLight.lightParams[nDir][1] = 0.0f;
		matLight.lightParams[nDir][2] = 0.0f;
		matLight.lightParams[nDir][3] = 0.0f;
		++nDir;
	}
	for (int i = nDir; i < 8; ++i)
		for (int k = 0; k < 4; ++k)
		{
			matLight.lightPos[i][k] = 0.0f; matLight.lightDir[i][k] = 0.0f;
			matLight.lightColor[i][k] = 0.0f; matLight.lightParams[i][k] = 0.0f;
		}
	matLight.counts[0] = static_cast<float>(nDir);
}

// ─── Tile-memory deferred resolve (plan P6) ──────────────────────────────────
// Encoded into the OPEN G-buffer pass encoder on Apple Silicon: the fragment
// framebuffer-fetches the four memoryless G-buffer attachments and writes the
// lit colour to attachment 4 (the HDR target). All lighting inputs (matLight,
// CSM/local atlases, sky env, AO, GI) are bound here — the G-buffer pass itself
// binds none of them.
void MetalRenderer::EncodeDeferredResolveTile(void* renderEncoder, int width, int height)
{
	if (!m_deferredResolveTilePipeline) return;
	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	// Same gates EncodeScene computes for its binds — m_renderWorld is current
	// (EncodeGBuffer extracted right before this call, same camera).
	const bool ssaoActive = m_ssaoEnabled && m_ssaoResult;
	const bool giActive   = m_giEnabled && m_giSupported && m_giShadowResult
	                     && m_giIrradianceAtlas && m_giVisibilityAtlas;
	const bool shadows    = m_renderWorld.shadow.enabled && m_shadowDepthTex;
	float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
	UpdateSkyEnvCube(m_renderWorld.sunDirection); // resolve samples the IBL cube (14)

	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_deferredResolveTilePipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];

	HE::MaterialShaderLibrary::ResolveUniforms ru;
	const glm::mat4 viewProj =
		m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 ivp = glm::inverse(viewProj);
	std::memcpy(ru.invViewProj, &ivp[0][0], 16 * sizeof(float));
	ru.depthParams[0] = -1.0f; // same conventions as the two-pass resolve
	ru.depthParams[1] = 1.0f;
	ru.depthParams[2] = 0.0f;
	ru.depthParams[3] = static_cast<float>(m_gbufferDebugView);
#if defined(HE_HAVE_SHADERC)
	HE::MaterialShaderLibrary::Lighting matLight;
	FillMaterialLighting(matLight, width, height, giActive, ssaoActive, shadows, skyClock);
	// The resolve is not a geometry pass: its normal is a G-buffer texel whose
	// derivative jumps at every silhouette, so specular AA stays off here. The
	// G-buffer pass already widened the roughness it stored (A6).
	matLight.specAA[1] = 0.0f;
	// SSR / GI reflections: the resolve SKIPS its specular-IBL term — the
	// dedicated reflection composite after this pass supplies it (sky cubemap
	// mixed with the SSR hit and/or the ray-traced GI result).
	if (m_ssrFrameActive || m_giReflFrameActive)
	{
		matLight.ssr[1] = m_ssrIntensity;
		matLight.ssr[2] = m_ssrMaxRoughness;
		matLight.ssr[3] = 1.0f;
	}
	// P7: point/spot lights move into the cluster lists; matLight's window is
	// rewritten to directional-only so no light is counted twice.
	if (m_deferredClustered)
		EncodeClusterData(renderEncoder, matLight, ru);
	[encoder setFragmentBytes:&matLight length:sizeof(matLight)
	                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif
	[encoder setFragmentBytes:&ru length:sizeof(ru) atIndex:3];

	// The preamble's lighting textures on their scene-pass slots.
	auto bindTex = [&](void* tex, int slot)
	{
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)tex atIndex:slot];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:slot];
	};
	bindTex(giActive ? m_giShadowResult    : m_dummyTexture, 5);
	bindTex(giActive ? m_giIrradianceAtlas : m_dummyTexture, 6);
	bindTex(giActive ? m_giVisibilityAtlas : m_dummyTexture, 7);
	bindTex(giActive ? m_giLocalMaskTex    : m_dummyTexture, 8);
	// Forward reflection results on 9/10 — the resolve zeroes the term via
	// ssr.w, but the gated samples still execute when the refl passes ran, so
	// the slots must hold a valid texture (dummies otherwise).
	bindTex(m_fwdReflSsrTex ? m_fwdReflSsrTex : m_dummyTexture, 9);
	bindTex(m_fwdReflGiTex  ? m_fwdReflGiTex  : m_dummyTexture, 10);
	bindTex(m_shadowDepthTex, 11);
	bindTex(m_localShadowTex ? m_localShadowTex : m_shadowDepthTex, 12);
	bindTex(m_skyEnvCube, 14);
	bindTex(ssaoActive ? m_ssaoResult : m_dummyTexture, 15);
	// Cloud-shadow map: TEXTURE only at 16 — sampler indices stop at 15; the
	// preamble samples it through an inline constexpr sampler.
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(m_cloudShadowTex ? m_cloudShadowTex : m_dummyTexture)
	                    atIndex:16];

	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	++m_counters.draws;
}

// ─── Deferred decals (P7 follow-up, tile mode) ───────────────────────────────
bool MetalRenderer::EnsureDecalPipeline()
{
	if (m_decalPipeline) return true;
	if (m_decalPipelineTried) return false;
	m_decalPipelineTried = true;
#if !defined(HE_HAVE_SHADERC)
	return false;
#else
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (!device || !m_deferredTileMode) return false;
		using Backend = HE::MaterialShaderLibrary::Backend;
		const auto& v = m_matShaderLib.decalVertex(Backend::Metal);
		const auto& f = m_matShaderLib.decalFragment(Backend::Metal);
		if (!(v.ok && f.ok))
		{
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: decal shader compile failed\n") + v.log + f.log).c_str());
			return false;
		}
		NSError* err = nil;
		id<MTLLibrary> vLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:v.source.c_str()] options:nil error:&err];
		id<MTLLibrary> fLib = err ? nil : [device newLibraryWithSource:
			[NSString stringWithUTF8String:f.source.c_str()] options:nil error:&err];
		if (vLib && fLib)
		{
			// Full 5-attachment single-pass layout; only GB0's rgb is written
			// (alpha-blended — GB0.a keeps the metallic value).
			MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
			d.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			d.fragmentFunction = [fLib newFunctionWithName:@"main0"];
			d.colorAttachments[0].pixelFormat = kGBuf0Format;
			d.colorAttachments[0].blendingEnabled             = YES;
			d.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
			d.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
			d.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
			d.colorAttachments[1].pixelFormat = kGBufAttrFormat;
			d.colorAttachments[1].writeMask   = MTLColorWriteMaskNone;
			d.colorAttachments[2].pixelFormat = kGBufAttrFormat;
			d.colorAttachments[2].writeMask   = MTLColorWriteMaskNone;
			d.colorAttachments[3].pixelFormat = MTLPixelFormatR32Float;
			d.colorAttachments[3].writeMask   = MTLColorWriteMaskNone;
			d.colorAttachments[4].pixelFormat = kSceneColorFormat;
			d.colorAttachments[4].writeMask   = MTLColorWriteMaskNone;
			d.depthAttachmentPixelFormat      = kDepthFormat;
			id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
			if (pso) m_decalPipeline = (void*)CFBridgingRetain(pso);
		}
		if (!m_decalPipeline)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: decal pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());
		return m_decalPipeline != nullptr;
	}
#endif
}

// Rasterize this frame's decal projectors into the OPEN G-buffer pass encoder,
// after the geometry and before the resolve (which fetches the blended GB0).
void MetalRenderer::EncodeDecals(void* renderEncoder, int width, int height)
{
	if (m_renderWorld.decals.empty() || !EnsureDecalPipeline()) return;
	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	const glm::mat4 viewProj =
		m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 ivp = glm::inverse(viewProj);

	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_decalPipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	// Cull FRONT faces so the projector still draws when the camera is inside
	// its box; the box-space clip in the fragment decides coverage.
	[encoder setCullMode:MTLCullModeFront];

	for (const DecalData& dc : m_renderWorld.decals)
	{
		HE::MaterialShaderLibrary::DecalUniforms du;
		const glm::mat4 invModel = glm::inverse(dc.transform);
		std::memcpy(du.viewProj,    &viewProj[0][0],     16 * sizeof(float));
		std::memcpy(du.model,       &dc.transform[0][0], 16 * sizeof(float));
		std::memcpy(du.invModel,    &invModel[0][0],     16 * sizeof(float));
		std::memcpy(du.invViewProj, &ivp[0][0],          16 * sizeof(float));
		du.color[0] = dc.color.r; du.color[1] = dc.color.g;
		du.color[2] = dc.color.b; du.color[3] = dc.color.a;
		void* tex = dc.textureId != HE::UUID{} ? ResolveGraphTexture(dc.textureId, {}) : nullptr;
		du.params[0] = tex ? 1.0f : 0.0f;
		du.params[1] = -1.0f; // Metal uv origin top-left (same conv as the resolve)
		du.params[2] = 1.0f;
		du.params[3] = 0.0f;
		du.vp[0] = static_cast<float>(width);
		du.vp[1] = static_cast<float>(height);
		[encoder setVertexBytes:&du length:sizeof(du) atIndex:0];
		[encoder setFragmentBytes:&du length:sizeof(du) atIndex:0];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(tex ? tex : m_dummyTexture) atIndex:0];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
		++m_counters.draws;
	}
	[encoder setCullMode:MTLCullModeNone]; // restore for the fullscreen resolve
}

// ─── Screen-space reflections (docs/ssr-plan.md §4.5, deferred tile mode) ────
bool MetalRenderer::EnsureSSRPipelines()
{
	if (m_ssrTracePipeline && m_ssrCompositePipeline) return true;
	if (m_ssrPipelinesTried) return false;
	m_ssrPipelinesTried = true;
#if !defined(HE_HAVE_SHADERC)
	return false;
#else
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (!device) return false;
		using Backend = HE::MaterialShaderLibrary::Backend;
		const auto& v  = m_matShaderLib.fullscreenVertex(Backend::Metal);
		const auto& ft = m_matShaderLib.ssrTrace(Backend::Metal);
		const auto& fc = m_matShaderLib.ssrComposite(Backend::Metal);
		const auto& fb = m_matShaderLib.ssrBlur(Backend::Metal);
		const auto& fm = m_matShaderLib.ssrRoughMix(Backend::Metal);
		if (!(v.ok && ft.ok && fc.ok && fb.ok))
		{
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: SSR shader compile failed\n")
				 + v.log + ft.log + fc.log + fb.log).c_str());
			return false;
		}
		NSError* err = nil;
		id<MTLLibrary> vLib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:v.source.c_str()] options:nil error:&err];
		id<MTLLibrary> tLib = err ? nil : [device newLibraryWithSource:
			[NSString stringWithUTF8String:ft.source.c_str()] options:nil error:&err];
		id<MTLLibrary> cLib = err ? nil : [device newLibraryWithSource:
			[NSString stringWithUTF8String:fc.source.c_str()] options:nil error:&err];
		id<MTLLibrary> bLib = err ? nil : [device newLibraryWithSource:
			[NSString stringWithUTF8String:fb.source.c_str()] options:nil error:&err];
		if (vLib && tLib && cLib && bLib)
		{
			MTLRenderPipelineDescriptor* td = [[MTLRenderPipelineDescriptor alloc] init];
			td.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			td.fragmentFunction = [tLib newFunctionWithName:@"main0"];
			td.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float; // radiance+conf (history)
			td.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Float; // receiver world pos
			id<MTLRenderPipelineState> tp = [device newRenderPipelineStateWithDescriptor:td error:&err];
			if (tp) m_ssrTracePipeline = (void*)CFBridgingRetain(tp);

			MTLRenderPipelineDescriptor* cd = [[MTLRenderPipelineDescriptor alloc] init];
			cd.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			cd.fragmentFunction = [cLib newFunctionWithName:@"main0"];
			cd.colorAttachments[0].pixelFormat = kSceneColorFormat;
			cd.colorAttachments[0].blendingEnabled           = YES; // additive over the resolve
			cd.colorAttachments[0].rgbBlendOperation         = MTLBlendOperationAdd;
			cd.colorAttachments[0].alphaBlendOperation       = MTLBlendOperationAdd;
			cd.colorAttachments[0].sourceRGBBlendFactor      = MTLBlendFactorOne;
			cd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
			cd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorZero;
			cd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
			id<MTLRenderPipelineState> cp = [device newRenderPipelineStateWithDescriptor:cd error:&err];
			if (cp) m_ssrCompositePipeline = (void*)CFBridgingRetain(cp);

			MTLRenderPipelineDescriptor* bd = [[MTLRenderPipelineDescriptor alloc] init];
			bd.vertexFunction   = [vLib newFunctionWithName:@"main0"];
			bd.fragmentFunction = [bLib newFunctionWithName:@"main0"];
			bd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
			id<MTLRenderPipelineState> bp = [device newRenderPipelineStateWithDescriptor:bd error:&err];
			if (bp) m_ssrBlurPipeline = (void*)CFBridgingRetain(bp);

			// Forward glossy mix — same fullscreen shape as the blur. OPTIONAL:
			// a failure here costs the forward path its roughness lerp, nothing
			// else, so it gets its own error object and never fails the build.
			if (fm.ok)
			{
				NSError* mErr = nil;
				id<MTLLibrary> mLib = [device newLibraryWithSource:
					[NSString stringWithUTF8String:fm.source.c_str()] options:nil error:&mErr];
				if (mLib)
				{
					MTLRenderPipelineDescriptor* md = [[MTLRenderPipelineDescriptor alloc] init];
					md.vertexFunction   = [vLib newFunctionWithName:@"main0"];
					md.fragmentFunction = [mLib newFunctionWithName:@"main0"];
					md.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
					id<MTLRenderPipelineState> mp =
						[device newRenderPipelineStateWithDescriptor:md error:&mErr];
					if (mp) m_ssrRoughMixPipeline = (void*)CFBridgingRetain(mp);
				}
			}
		}
		const bool ok = m_ssrTracePipeline && m_ssrCompositePipeline && m_ssrBlurPipeline;
		if (!ok)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: SSR pipeline build failed: ")
				 + (err ? err.localizedDescription.UTF8String : "?")).c_str());
		return ok;
	}
#endif
}

void MetalRenderer::EnsureSSRTarget(int width, int height)
{
	if (m_ssrReflTex && width == m_ssrReflW && height == m_ssrReflH) return;
	DestroySSRTarget();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	MTLTextureDescriptor* d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
		width:width height:height mipmapped:NO];
	d.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModePrivate;
	m_ssrReflTex  = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	m_ssrPingTex  = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	m_ssrRoughTex = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	for (int i = 0; i < 2; ++i)
	{
		m_ssrHistRad[i] = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
		m_ssrHistPos[i] = (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	}
	m_ssrHistIdx   = 0;
	m_ssrHistValid = false; // fresh (undefined-content) textures — first frame skips the blend
	m_ssrReflW = width;
	m_ssrReflH = height;
}

void MetalRenderer::DestroySSRTarget()
{
	if (m_ssrReflTex)  { CFBridgingRelease(m_ssrReflTex);  m_ssrReflTex = nullptr; }
	if (m_ssrPingTex)  { CFBridgingRelease(m_ssrPingTex);  m_ssrPingTex = nullptr; }
	if (m_ssrRoughTex) { CFBridgingRelease(m_ssrRoughTex); m_ssrRoughTex = nullptr; }
	for (int i = 0; i < 2; ++i)
	{
		if (m_ssrHistRad[i]) { CFBridgingRelease(m_ssrHistRad[i]); m_ssrHistRad[i] = nullptr; }
		if (m_ssrHistPos[i]) { CFBridgingRelease(m_ssrHistPos[i]); m_ssrHistPos[i] = nullptr; }
	}
	m_ssrHistValid = false;
	m_ssrReflW = m_ssrReflH = 0;
}

// Trace (half-res) + additive composite, own passes after the tile G-buffer
// pass. m_renderWorld.camera is current (EncodeGBuffer extracted this frame).
// Shared env-specular composite: also runs for GI-reflections-only frames (the
// SSR trace/blur are then skipped and the SSR samplers get a dummy with mix
// weight 0 — see the m_giReflFrameActive wiring in EncodeFrame).
void MetalRenderer::EncodeSSRPasses(void* cmdBufPtr, int width, int height)
{
	if (!m_ssrCompositePipeline || !m_gbStored) return;
	const bool ssrOn    = m_ssrFrameActive && m_ssrTracePipeline;
	const bool giReflOn = m_giReflFrameActive && m_giReflTex;
	if (!ssrOn && !giReflOn) return;
	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		const int tw = std::max(1, width / 2), th = std::max(1, height / 2);
		if (ssrOn) EnsureSSRTarget(tw, th);

		const glm::mat4 viewProj =
			m_renderWorld.camera.projection * m_renderWorld.camera.view;
		const glm::vec3 camFwd =
			-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));

		// ── 1. Trace (MRT into the history pair: blended radiance + receiver
		// world pos; quality High rotates the start jitter per frame and
		// EMA-blends against last frame's pair — see kSSRTraceFS) ──────────
		const int curIdx  = m_ssrHistIdx;
		const int prevIdx = 1 - curIdx;
		void* const traceOut = ssrOn ? m_ssrHistRad[curIdx] : nullptr;
		if (ssrOn)
		{
			const bool temporal = m_ssrQuality >= 2;
			// Same damping scheme as the GI reflections: full 0.85 EMA while
			// the camera is still, 0.55 the moment the view matrix changes
			// (the reflected content is not reprojected — no motion vectors).
			float hist = 0.0f;
			if (temporal && m_ssrHistValid)
			{
				float delta = 0.0f;
				for (int c = 0; c < 4; ++c)
					for (int r = 0; r < 4; ++r)
						delta = std::max(delta,
							std::abs(viewProj[c][r] - m_ssrPrevViewProj[c][r]));
				hist = delta > 1e-5f ? 0.55f : 0.85f;
			}
			m_ssrFrameSeed += 1.0f;

			HE::MaterialShaderLibrary::SSRTraceUniforms tu;
			const glm::mat4 ivp = glm::inverse(viewProj);
			std::memcpy(tu.viewProj,     &viewProj[0][0],          16 * sizeof(float));
			std::memcpy(tu.invViewProj,  &ivp[0][0],               16 * sizeof(float));
			std::memcpy(tu.prevViewProj, &m_ssrPrevViewProj[0][0], 16 * sizeof(float));
			tu.cfg2[0] = m_ssrFrameSeed;
			tu.cfg2[1] = hist;
			// Glossy cone jitter rides the temporal EMA — High tier only.
			tu.cfg2[3] = temporal ? 1.0f : 0.0f;
			tu.camPos[0] = m_renderWorld.camera.position.x;
			tu.camPos[1] = m_renderWorld.camera.position.y;
			tu.camPos[2] = m_renderWorld.camera.position.z;
			tu.camFwd[0] = camFwd.x; tu.camFwd[1] = camFwd.y; tu.camFwd[2] = camFwd.z;
			tu.cfg[0] = m_ssrMaxDistance;
			tu.cfg[1] = m_ssrThickness;
			tu.cfg[2] = m_ssrMaxRoughness;
			tu.cfg[3] = static_cast<float>(m_ssrQuality <= 0 ? 16 : m_ssrQuality == 1 ? 32 : 64);
			tu.conv[0] = -1.0f; // Metal uv origin top-left (same as the resolve)
			tu.conv[1] = 1.0f;
			tu.conv[2] = 0.0f;
			tu.conv[3] = 0.1f;  // edge fade over the outer 10 % of the screen
			tu.vp[0] = static_cast<float>(tw);
			tu.vp[1] = static_cast<float>(th);

			MTLRenderPassDescriptor* tp = [MTLRenderPassDescriptor renderPassDescriptor];
			tp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssrHistRad[curIdx];
			tp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			tp.colorAttachments[0].storeAction = MTLStoreActionStore;
			tp.colorAttachments[1].texture     = (__bridge id<MTLTexture>)m_ssrHistPos[curIdx];
			tp.colorAttachments[1].loadAction  = MTLLoadActionDontCare;
			tp.colorAttachments[1].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:tp];
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrTracePipeline];
			[enc setFragmentBytes:&tu length:sizeof(tu) atIndex:0];
			id<MTLSamplerState> pointSmp = (__bridge id<MTLSamplerState>)
				(m_ssaoPointSampler ? m_ssaoPointSampler : m_linearSampler);
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_hdrColor atIndex:0];
			[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
			// Attributes point-sampled: linearly interpolated oct normals decode
			// to garbage directions at geometry edges (the GI kernel's rule).
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_gbColor1 atIndex:1];
			[enc setFragmentSamplerState:pointSmp atIndex:1];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_gbDepthLin atIndex:2];
			[enc setFragmentSamplerState:pointSmp atIndex:2];
			// History (prev pair) — point-sampled: the disocclusion test needs
			// exact stored positions, not positions blended across depth edges.
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ssrHistRad[prevIdx] atIndex:3];
			[enc setFragmentSamplerState:pointSmp atIndex:3];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ssrHistPos[prevIdx] atIndex:4];
			[enc setFragmentSamplerState:pointSmp atIndex:4];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];

			m_ssrHistIdx      = prevIdx;   // next frame reads what we just wrote
			m_ssrHistValid    = true;
			m_ssrPrevViewProj = viewProj;
		}

		// ── 1b. Separable 5-tap blur (ssr-plan §4.3 / P4) ──────────────────
		// Smooths the jittered march's IGN dithering. Quality Low (16 steps)
		// skips it, matching the plan's quality tiers; the composite then
		// samples the raw trace. H pass refl→ping, V pass ping→refl, so the
		// composite below reads m_ssrReflTex either way.
		if (ssrOn && m_ssrQuality >= 1)
		{
			auto blurPass = [&](void* src, void* dst, float dx, float dy)
			{
				HE::MaterialShaderLibrary::SSRBlurUniforms bu;
				bu.dir[0] = dx;
				bu.dir[1] = dy;
				bu.dir[2] = 1.0f / static_cast<float>(tw);
				bu.dir[3] = 1.0f / static_cast<float>(th);
				MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor];
				bp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)dst;
				bp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				bp.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:bp];
				[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrBlurPipeline];
				[enc setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
				[enc setFragmentTexture:(__bridge id<MTLTexture>)src atIndex:0];
				[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
			};
			// Tier policy: the blur is the DENOISER at Med (static jitter →
			// two 5-tap iterations, smooth but soft), while High denoises via
			// the temporal accumulation and keeps only ONE mild iteration —
			// mirror-sharp surfaces stay sharp instead of being washed out.
			blurPass(traceOut,     m_ssrPingTex, 1.0f / static_cast<float>(tw), 0.0f);
			blurPass(m_ssrPingTex, m_ssrReflTex, 0.0f, 1.0f / static_cast<float>(th));
			m_counters.draws += 2;
			if (m_ssrQuality == 1)
			{
				// Second iteration (Med only): an isolated confident speckle
				// pixel keeps its colour under the confidence weighting — the
				// repeat flattens it.
				blurPass(m_ssrReflTex, m_ssrPingTex, 1.0f / static_cast<float>(tw), 0.0f);
				blurPass(m_ssrPingTex, m_ssrReflTex, 0.0f, 1.0f / static_cast<float>(th));
				m_counters.draws += 2;
			}
			// High tier: a second, wide pass (3-texel spacing) into m_ssrRoughTex
			// — the mip-chain substitute. The composite lerps between the two by
			// G-buffer roughness (glossy instead of mirror-only, plan §4.3 v2).
			if (m_ssrQuality >= 2)
			{
				blurPass(m_ssrReflTex, m_ssrPingTex,  3.0f / static_cast<float>(tw), 0.0f);
				blurPass(m_ssrPingTex, m_ssrRoughTex, 0.0f, 3.0f / static_cast<float>(th));
				m_counters.draws += 2;
			}
		}

		// ── 2. Additive composite onto the resolved HDR ────────────────────
		{
			const bool ssaoActive = m_ssaoEnabled && m_ssaoResult;
			const bool giActive   = m_giEnabled && m_giSupported && m_giShadowResult
			                     && m_giIrradianceAtlas && m_giVisibilityAtlas;
			const bool shadows    = m_renderWorld.shadow.enabled && m_shadowDepthTex;
			float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
			if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));

			MTLRenderPassDescriptor* cp = [MTLRenderPassDescriptor renderPassDescriptor];
			cp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_hdrColor;
			cp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
			cp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:cp];
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrCompositePipeline];
#if defined(HE_HAVE_SHADERC)
			HE::MaterialShaderLibrary::Lighting matLight;
			FillMaterialLighting(matLight, width, height, giActive, ssaoActive, shadows, skyClock);
			matLight.specAA[1] = 0.0f;   // fullscreen composite, not a geometry pass (A6)
			// Inactive sources keep mix weight 0 — the bound dummy (1×1 white,
			// a = 1) then contributes nothing regardless of its content.
			matLight.ssr[1]    = ssrOn ? m_ssrIntensity : 0.0f;
			matLight.ssr[2]    = m_ssrMaxRoughness;
			matLight.ssr[3]    = 1.0f;
			matLight.giRefl[0] = giReflOn ? m_giReflIntensity : 0.0f;
			matLight.giRefl[1] = m_giReflMaxRoughness;
			[enc setFragmentBytes:&matLight length:sizeof(matLight)
			              atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif
			HE::MaterialShaderLibrary::ResolveUniforms ru;
			const glm::mat4 ivp = glm::inverse(viewProj);
			std::memcpy(ru.invViewProj, &ivp[0][0], 16 * sizeof(float));
			ru.depthParams[0] = -1.0f;
			ru.depthParams[1] = 1.0f;
			ru.depthParams[2] = 0.0f;
			[enc setFragmentBytes:&ru length:sizeof(ru) atIndex:3];
			auto bindTex = [&](void* tex, int slot, void* sampler)
			{
				[enc setFragmentTexture:(__bridge id<MTLTexture>)tex atIndex:slot];
				[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)sampler atIndex:slot];
			};
			bindTex(m_gbColor0,   0, m_linearSampler);
			bindTex(m_gbColor1,   1, m_linearSampler);
			bindTex(m_gbColor2,   2, m_linearSampler);
			bindTex(m_gbDepthLin, 3, m_ssaoPointSampler ? m_ssaoPointSampler : m_linearSampler);
			// Quality Low has no blur chain — the composite reads the raw
			// trace (history) target directly then.
			void* const ssrSharp = ssrOn
				? (m_ssrQuality >= 1 ? m_ssrReflTex : traceOut) : m_dummyTexture;
			bindTex(ssrSharp, 4, m_linearSampler);
			bindTex(ssrOn ? (m_ssrQuality >= 2 ? m_ssrRoughTex : ssrSharp)
			              : m_dummyTexture, 5, m_linearSampler);
			// Sharp trace + its blurred copy; the composite lerps by roughness.
			// Every tier now produces the pair (the blur WIDTH is what the tier
			// changes — wide when few rays sampled the lobe, narrow when many
			// did), so this no longer collapses both slots onto one texture.
			bindTex(giReflOn ? m_giReflTex      : m_dummyTexture, 6, m_linearSampler);
			bindTex(giReflOn ? m_giReflRoughTex : m_dummyTexture, 7, m_linearSampler);
			bindTex(m_skyEnvCube, 14, m_linearSampler);
			bindTex(ssaoActive ? m_ssaoResult : m_dummyTexture, 15, m_linearSampler);
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
			m_counters.draws += 2;
		}
	}
}

// ─── Forward SSR (ssr-plan Option A / P3-Forward) ────────────────────────────
// The SAME trace/blur/temporal chain as the deferred variant, fed from the MRT
// pre-pass (oct normals + NDC depth, gbufferMain-compatible by construction)
// and LAST frame's HDR copy — the hit reprojects into it via prevViewProj
// (cfg2.z), one frame of content lag being the accepted forward trade. There
// is no composite pass: heLitP/fragmentMain sample the result at slot 9.
void MetalRenderer::EncodeForwardSSR(void* cmdBufPtr, int width, int height)
{
	m_fwdReflSsrTex = nullptr;
	if (!m_ssrFrameActive || m_deferredFrameActive) return;
	if (!m_ssrTracePipeline || !m_ssrBlurPipeline)  return;
	if (!m_reflNormTex || !m_reflDepthTex)          return;
	if (!m_ssrColorHist || !m_ssrColorHistValid)    return; // frame 1 seeds the copy first
	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		const int tw = std::max(1, width / 2), th = std::max(1, height / 2);
		EnsureSSRTarget(tw, th);
		const glm::mat4 viewProj =
			m_renderWorld.camera.projection * m_renderWorld.camera.view;
		const glm::vec3 camFwd =
			-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));

		const int curIdx  = m_ssrHistIdx;
		const int prevIdx = 1 - curIdx;
		void* const traceOut = m_ssrHistRad[curIdx];
		{
			const bool temporal = m_ssrQuality >= 2;
			float hist = 0.0f;
			if (temporal && m_ssrHistValid)
			{
				float delta = 0.0f;
				for (int c = 0; c < 4; ++c)
					for (int r = 0; r < 4; ++r)
						delta = std::max(delta,
							std::abs(viewProj[c][r] - m_ssrPrevViewProj[c][r]));
				hist = delta > 1e-5f ? 0.55f : 0.85f;
			}
			m_ssrFrameSeed += 1.0f;

			HE::MaterialShaderLibrary::SSRTraceUniforms tu;
			const glm::mat4 ivp = glm::inverse(viewProj);
			std::memcpy(tu.viewProj,     &viewProj[0][0],          16 * sizeof(float));
			std::memcpy(tu.invViewProj,  &ivp[0][0],               16 * sizeof(float));
			std::memcpy(tu.prevViewProj, &m_ssrPrevViewProj[0][0], 16 * sizeof(float));
			tu.cfg2[0] = m_ssrFrameSeed;
			tu.cfg2[1] = hist;
			tu.cfg2[2] = 1.0f; // forward: colour from the previous frame's copy
			tu.cfg2[3] = temporal ? 1.0f : 0.0f; // glossy cone jitter (High)
			tu.camPos[0] = m_renderWorld.camera.position.x;
			tu.camPos[1] = m_renderWorld.camera.position.y;
			tu.camPos[2] = m_renderWorld.camera.position.z;
			tu.camFwd[0] = camFwd.x; tu.camFwd[1] = camFwd.y; tu.camFwd[2] = camFwd.z;
			tu.cfg[0] = m_ssrMaxDistance;
			tu.cfg[1] = m_ssrThickness;
			tu.cfg[2] = m_ssrMaxRoughness;
			tu.cfg[3] = static_cast<float>(m_ssrQuality <= 0 ? 16 : m_ssrQuality == 1 ? 32 : 64);
			tu.conv[0] = -1.0f;
			tu.conv[1] = 1.0f;
			tu.conv[2] = 0.0f;
			tu.conv[3] = 0.1f;
			tu.vp[0] = static_cast<float>(tw);
			tu.vp[1] = static_cast<float>(th);

			MTLRenderPassDescriptor* tp = [MTLRenderPassDescriptor renderPassDescriptor];
			tp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssrHistRad[curIdx];
			tp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
			tp.colorAttachments[0].storeAction = MTLStoreActionStore;
			tp.colorAttachments[1].texture     = (__bridge id<MTLTexture>)m_ssrHistPos[curIdx];
			tp.colorAttachments[1].loadAction  = MTLLoadActionDontCare;
			tp.colorAttachments[1].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:tp];
			[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrTracePipeline];
			[enc setFragmentBytes:&tu length:sizeof(tu) atIndex:0];
			id<MTLSamplerState> pointSmp = (__bridge id<MTLSamplerState>)
				(m_ssaoPointSampler ? m_ssaoPointSampler : m_linearSampler);
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ssrColorHist atIndex:0];
			[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
			// Point-sampled for the same reason as the deferred variant: lerped
			// oct normals decode to garbage at geometry edges.
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_reflNormTex atIndex:1];
			[enc setFragmentSamplerState:pointSmp atIndex:1];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_reflDepthTex atIndex:2];
			[enc setFragmentSamplerState:pointSmp atIndex:2];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ssrHistRad[prevIdx] atIndex:3];
			[enc setFragmentSamplerState:pointSmp atIndex:3];
			[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ssrHistPos[prevIdx] atIndex:4];
			[enc setFragmentSamplerState:pointSmp atIndex:4];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];

			m_ssrHistIdx      = prevIdx;
			m_ssrHistValid    = true;
			m_ssrPrevViewProj = viewProj;
		}

		// Blur chain — same tier policy as the deferred variant.
		if (m_ssrQuality >= 1)
		{
			auto blurPass = [&](void* src, void* dst, float dx, float dy)
			{
				HE::MaterialShaderLibrary::SSRBlurUniforms bu;
				bu.dir[0] = dx;
				bu.dir[1] = dy;
				bu.dir[2] = 1.0f / static_cast<float>(tw);
				bu.dir[3] = 1.0f / static_cast<float>(th);
				MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor];
				bp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)dst;
				bp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				bp.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:bp];
				[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrBlurPipeline];
				[enc setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
				[enc setFragmentTexture:(__bridge id<MTLTexture>)src atIndex:0];
				[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
				[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[enc endEncoding];
				++m_counters.draws;
			};
			blurPass(traceOut,     m_ssrPingTex, 1.0f / static_cast<float>(tw), 0.0f);
			blurPass(m_ssrPingTex, m_ssrReflTex, 0.0f, 1.0f / static_cast<float>(th));
			if (m_ssrQuality == 1)
			{
				blurPass(m_ssrReflTex, m_ssrPingTex, 1.0f / static_cast<float>(tw), 0.0f);
				blurPass(m_ssrPingTex, m_ssrReflTex, 0.0f, 1.0f / static_cast<float>(th));
			}
			// No glossy wide stage in the forward path: it would cost a THIRD
			// material sampler slot and the budget is spent (9 SSR, 10 GI refl).
		}
		m_fwdReflSsrTex = m_ssrQuality >= 1 ? m_ssrReflTex : traceOut;
	}
}

// ─── Ray-traced GI reflections (docs/gi-reflections-plan.md) ─────────────────
bool MetalRenderer::EnsureGIReflPipeline()
{
	if (m_giReflPipeline) return true;
	if (m_giReflPipelineTried) return false;
	m_giReflPipelineTried = true;
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (!device) return false;
		NSError* error = nil;
		// HW = intersection_query against the TLAS (kGIReflMSL); SW = the
		// CPU-BVH traversal variant living in kGISWMSL (P5) — same selection
		// the GI shadow/probe pipelines make.
		const char* src = m_giHwRt ? kGIReflMSL : kGISWMSL;
		id<MTLLibrary> lib = [device newLibraryWithSource:
			[NSString stringWithUTF8String:src] options:nil error:&error];
		if (lib)
		{
			id<MTLFunction> fn = [lib newFunctionWithName:(m_giHwRt ? @"giReflRay" : @"giReflRaySw")];
			id<MTLComputePipelineState> pso =
				fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
			if (pso) m_giReflPipeline = (void*)CFBridgingRetain(pso);
		}
		if (!m_giReflPipeline)
			HE_LOG_ERROR(RHI, "%s",
				(std::string("MetalRenderer: GI reflection pipeline creation failed: ")
				 + (error ? [[error localizedDescription] UTF8String] : "unknown")).c_str());
		return m_giReflPipeline != nullptr;
	}
}

void MetalRenderer::EnsureGIReflTarget(int width, int height)
{
	if (m_giReflTex && width == m_giReflW && height == m_giReflH) return;
	DestroyGIReflTarget();
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
	// Display/ping/rough are blur render targets AND compute-written; the
	// history pairs are compute-only (read previous, write current).
	auto makeTex = [&](bool renderTarget) -> void*
	{
		MTLTextureDescriptor* d = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
			width:width height:height mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite
		        | (renderTarget ? MTLTextureUsageRenderTarget : 0);
		d.storageMode = MTLStorageModePrivate;
		return (void*)CFBridgingRetain([device newTextureWithDescriptor:d]);
	};
	m_giReflTex      = makeTex(true);
	m_giReflPingTex  = makeTex(true);
	m_giReflRoughTex = makeTex(true);
	m_giReflHistRad[0] = makeTex(false);
	m_giReflHistRad[1] = makeTex(false);
	m_giReflHistPos[0] = makeTex(false);
	m_giReflHistPos[1] = makeTex(false);
	m_giReflHistIdx   = 0;
	m_giReflHistValid = false; // fresh (undefined-content) textures — first frame skips the blend
	m_giReflW = width;
	m_giReflH = height;
}

void MetalRenderer::DestroyGIReflTarget()
{
	auto rel = [](void*& t) { if (t) { CFBridgingRelease(t); t = nullptr; } };
	rel(m_giReflTex);
	rel(m_giReflPingTex);
	rel(m_giReflRoughTex);
	rel(m_giReflHistRad[0]);
	rel(m_giReflHistRad[1]);
	rel(m_giReflHistPos[0]);
	rel(m_giReflHistPos[1]);
	m_giReflHistValid = false;
	m_giReflW = m_giReflH = 0;
}

// Half-res compute trace (+ in-kernel temporal at quality 2) into m_giReflTex
// and the history pair, then the confidence-weighted blur chain (quality ≥ 1,
// reusing the SSR blur pipeline — same RGBA16F fullscreen pass). Runs after
// the tile G-buffer pass ended (stored attachments) and before EncodeSSRPasses'
// composite reads the result. The acceleration structures were built earlier
// this frame by EncodeGIAccelBuild (which also runs for reflections-only
// frames) and m_renderWorld.camera is current.
void MetalRenderer::EncodeGIReflections(void* cmdBufPtr, int width, int height)
{
	if (!m_giReflFrameActive || !m_giReflPipeline) return;
	// Input pair: the stored G-buffer in the deferred tile mode, the MRT
	// pre-pass in the forward path — identical formats/conventions, so the
	// kernels are byte-shared between both.
	const bool deferredIn = m_deferredFrameActive;
	void* const attrTex  = deferredIn ? m_gbColor1   : m_reflNormTex;
	void* const depthTex = deferredIn ? m_gbDepthLin : m_reflDepthTex;
	if (deferredIn ? !m_gbStored : !(attrTex && depthTex)) return;
	if (m_giHwRt ? !(m_giTlas && m_giInstanceColorBuffer)
	             : !(m_giSwNodeBuf && m_giSwTriBuf && m_giSwInstanceBuf && m_giSwInstanceCount > 0))
		return;
	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
		// RESOLUTION is the tier's second axis (rays being the first): Low traces
		// at quarter, Medium at half, High at full screen resolution. The blur
		// below then exists only to stand in for the pixels a lower resolution
		// did not trace, so its width follows resDiv — quarter-res gets four
		// texels of it, full res one. Higher tier = more rays AND more pixels =
		// sharper, which is the direction a quality setting should move in.
		const int resDiv = m_giReflQuality >= 2 ? 1 : (m_giReflQuality >= 1 ? 2 : 4);
		const int tw = std::max(1, width / resDiv), th = std::max(1, height / resDiv);
		// How much blur even a MIRROR gets, purely to hide the resolution:
		// none at full res, most at quarter. Without it a quarter-res mirror
		// keeps its stair-steps — "sharp" is not the same as "detailed".
		const float giReflBlurFloor = m_giReflBlurEnabled
			? (resDiv >= 4 ? 0.75f : (resDiv >= 2 ? 0.35f : 0.0f)) : 0.0f;
		m_giReflBlurFloor = giReflBlurFloor;
		EnsureGIReflTarget(tw, th);
		if (!m_giReflTex) return;

		// The probe field feeds the hit shading only while the probes are
		// actually being updated (GI on) — otherwise the atlas is stale or
		// blank and the kernel falls back to the flat ambient floor.
		const bool fieldValid = m_giEnabled && m_giProbeGridBuilt && m_giIrradianceAtlas;
		// Quality 2 jitters the ray per frame; only then is the temporal EMA
		// worth anything (lower tiers are deterministic → blend 0, but the
		// history is still written so a mid-session quality switch just works).
		// The quality tier means exactly two things now: RAYS per pixel and how
		// wide the blur afterwards is. Low = 1 deterministic ray, no blur;
		// Medium = 2 rays + the narrow blur; High = 4 rays + narrow, plus the
		// wide pass and its roughness lerp. Nothing else keys off the tier —
		// which is what makes the ladder monotone. It was not: the top tier used
		// to switch ON a stochastic cone whose only integrator was the temporal
		// EMA, so it looked WORSE than the tier below whenever that EMA had to
		// be damped. A near-mirror still traces one ray (its cone is narrower
		// than a pixel), so mirrors cost the same at every tier.
		const int  rays     = m_giReflQuality >= 2 ? 4 : (m_giReflQuality >= 1 ? 2 : 1);
		const bool jitter   = rays > 1;
		const bool meshData = m_giHwRt && m_giMeshPtrBuf && m_giInstanceMeshBuf;
		m_giReflFrameSeed += 1.0f;

		GIReflParamsCPU rp{};
		const glm::mat4 viewProj =
			m_renderWorld.camera.projection * m_renderWorld.camera.view;
		rp.invViewProj  = glm::inverse(viewProj);
		rp.prevViewProj = m_giReflPrevViewProj;
		rp.camPos       = glm::vec4(m_renderWorld.camera.position, m_giReflMaxDistance);
		rp.texSize      = glm::vec4(static_cast<float>(tw), static_cast<float>(th),
		                            m_giReflMaxRoughness, fieldValid ? 1.0f : 0.0f);
		// Same dominant-directional pick as the GI shadow/probe passes — the
		// hit must be lit by the light the scene is actually lit by.
		glm::vec3 towardLight, lightColorIntensity;
		m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
		rp.sunDirRadius = glm::vec4(towardLight, m_giIndirectIntensity);
		rp.sunColor     = glm::vec4(lightColorIntensity, m_giReflFrameSeed);
		// History blend: 0.85 EMA while the camera is still (glossy jitter
		// converges), damped to 0.55 the moment the view matrix changes —
		// reflected-object parallax is not reprojected (no motion vectors), so
		// a strong EMA under camera motion is exactly the "reflection drags
		// behind" artefact. The kernel additionally collapses the weight on
		// luminance breaks (moving CONTENT with a still camera).
		// NO temporal accumulation. It was the last thing keying off the tier
		// that was neither rays nor resolution, and it was the LATENCY: an EMA
		// weight of 0.94 is ~17 frames of history, so the reflection visibly
		// dragged behind the rest of the frame — and dragged MORE the higher the
		// tier, because only the upper tiers ran it. That is why Low felt the
		// most responsive. Rays carry the estimate now and the blur covers the
		// resolution; a reflection that is a little noisier beats one that is
		// several frames old. The history targets stay allocated and the kernels
		// still write them, so switching it back on is one line (hist > 0
		// re-enables the whole block in both kernels).
		const float hist = 0.0f;
		rp.skyAmbient   = glm::vec4(m_renderWorld.ambient, hist);
		rp.gridOrigin   = glm::vec4(m_giGridOrigin, kGIProbeSpacing);
		rp.gridCounts   = glm::vec4(static_cast<float>(m_giGridCounts.x),
		                            static_cast<float>(m_giGridCounts.y),
		                            static_cast<float>(m_giGridCounts.z),
		                            static_cast<float>(m_giProbesPerRow));
		rp.extra        = glm::vec4(jitter ? 1.0f : 0.0f, meshData ? 1.0f : 0.0f,
		                            static_cast<float>(m_giSwInstanceCount),
		                            static_cast<float>(m_giReflBounces));
		// Painted landscapes: the table + one weightmap texture each, so a hit on
		// a terrain reads the PAINT at that point instead of one averaged colour
		// for the whole landscape (see GiLandscape.h).
		std::vector<void*> landTex;
		const int landCount = BuildGILandscapeTable(landTex);
		rp.land         = glm::vec4(static_cast<float>(landCount),
		                            static_cast<float>(rays), 0.0f, 0.0f);

		const int curIdx  = m_giReflHistIdx;
		const int prevIdx = 1 - curIdx;

		id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
		[enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_giReflPipeline];
		[enc setTexture:(__bridge id<MTLTexture>)attrTex  atIndex:0];
		[enc setTexture:(__bridge id<MTLTexture>)depthTex atIndex:1];
		[enc setTexture:(__bridge id<MTLTexture>)m_giReflTex  atIndex:2];
		[enc setTexture:(__bridge id<MTLTexture>)(fieldValid ? m_giIrradianceAtlas
		                                                     : m_dummyTexture) atIndex:3];
		[enc setTexture:(__bridge id<MTLTexture>)m_giReflHistRad[prevIdx] atIndex:4];
		[enc setTexture:(__bridge id<MTLTexture>)m_giReflHistPos[prevIdx] atIndex:5];
		[enc setTexture:(__bridge id<MTLTexture>)m_giReflHistRad[curIdx]  atIndex:6];
		[enc setTexture:(__bridge id<MTLTexture>)m_giReflHistPos[curIdx]  atIndex:7];
		// Sky cubemap for SECONDARY-bounce misses (a mirror seen in a mirror
		// reflecting the sky) — the primary miss keeps the composite's fallback.
		[enc setTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:8];
		if (m_giHwRt)
		{
			[enc setAccelerationStructure:(__bridge id<MTLAccelerationStructure>)m_giTlas
			                atBufferIndex:0];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giInstanceColorBuffer offset:0 atIndex:1];
			[enc setBytes:&rp length:sizeof(rp) atIndex:2];
			// The mesh-pointer argument buffer must always be bound (the
			// pipeline declares it); a null m_giMeshPtrBuf simply keeps
			// extra.y = 0 so the kernel never dereferences it — bind the
			// instance-colour buffer as a harmless placeholder then.
			[enc setBuffer:(__bridge id<MTLBuffer>)(m_giMeshPtrBuf ? m_giMeshPtrBuf
			                                                       : m_giInstanceColorBuffer)
			        offset:0 atIndex:3];
			[enc setBuffer:(__bridge id<MTLBuffer>)(m_giInstanceMeshBuf ? m_giInstanceMeshBuf
			                                                            : m_giInstanceColorBuffer)
			        offset:0 atIndex:4];
			// Landscape table + per-instance index. Both are declared by the
			// pipeline, so bind a harmless placeholder when there is no terrain —
			// rp.land.x = 0 then keeps the kernel from ever dereferencing them.
			[enc setBuffer:(__bridge id<MTLBuffer>)(m_giLandBuf ? m_giLandBuf
			                                                    : m_giInstanceColorBuffer)
			        offset:0 atIndex:5];
			[enc setBuffer:(__bridge id<MTLBuffer>)(m_giInstanceLandBuf ? m_giInstanceLandBuf
			                                                           : m_giInstanceColorBuffer)
			        offset:0 atIndex:6];
			// Every BLAS the TLAS references must be explicitly declared used — Metal
			// does not auto-track residency through an acceleration structure. The
			// mesh vertex/index buffers reached through the argument buffer need
			// the same declaration (indirect access is not auto-tracked either).
			[enc useResource:(__bridge id<MTLAccelerationStructure>)m_giTlas usage:MTLResourceUsageRead];
			for (void* b : m_giUniqueBlas)
				[enc useResource:(__bridge id<MTLAccelerationStructure>)b usage:MTLResourceUsageRead];
			if (meshData)
				for (void* b : m_giMeshResources)
					[enc useResource:(__bridge id<MTLBuffer>)b usage:MTLResourceUsageRead];
		}
		else
		{
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwNodeBuf     offset:0 atIndex:0];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwTriBuf      offset:0 atIndex:1];
			[enc setBuffer:(__bridge id<MTLBuffer>)m_giSwInstanceBuf offset:0 atIndex:2];
			[enc setBytes:&rp length:sizeof(rp) atIndex:3];
			// SW instances carry their landscape index in offsets.z; the table
			// itself binds here (placeholder when there is no terrain).
			[enc setBuffer:(__bridge id<MTLBuffer>)(m_giLandBuf ? m_giLandBuf
			                                                    : m_giSwInstanceBuf)
			        offset:0 atIndex:4];
		}
		// Weightmap per landscape, slots 9..9+kGiMaxLandscapes-1. Unused slots
		// take the 1x1 dummy: the pipeline declares the whole array, and an
		// unbound texture in it is undefined behaviour even when never sampled.
		for (int i = 0; i < HE::kGiMaxLandscapes; ++i)
			[enc setTexture:(__bridge id<MTLTexture>)(i < (int)landTex.size() && landTex[i]
			                                          ? landTex[i] : m_dummyTexture)
			        atIndex:9 + i];
		const MTLSize tgSize  = MTLSizeMake(8, 8, 1);
		const MTLSize tgCount = MTLSizeMake(static_cast<NSUInteger>((tw + 7) / 8),
		                                    static_cast<NSUInteger>((th + 7) / 8), 1);
		[enc dispatchThreadgroups:tgCount threadsPerThreadgroup:tgSize];
		[enc endEncoding];

		m_giReflHistIdx      = prevIdx;
		m_giReflHistValid    = true;
		m_giReflPrevViewProj = viewProj; // for NEXT frame's reprojection

		// ── Blur chain: the SSR blur pipeline verbatim — same RGBA16F fullscreen
		// confidence-weighted 5-tap, run at EVERY tier (Low needs it most, see
		// the width comment below).
		//
		// SKIPPED when its result provably cannot be used. In the FORWARD path
		// the reflection prepass writes roughness 0 (it has no material data —
		// reflPosFragment), so the per-pixel lerp between sharp and blurred can
		// only ever return the resolution floor; at full resolution that floor is
		// 0, i.e. the blurred copy is discarded. Without this the High forward
		// tier encoded a dozen full-screen RGBA16F passes per frame and threw
		// every one of them away.
		// m_giReflBlurEnabled is the user's switch; the rest is "would the result
		// be used at all" (see below).
		const bool blurUsed = m_giReflBlurEnabled
		                   && (deferredIn || m_giReflBlurFloor > 0.0f);
		if (m_ssrBlurPipeline && blurUsed)
		{
			auto blurPass = [&](void* src, void* dst, float dx, float dy)
			{
				HE::MaterialShaderLibrary::SSRBlurUniforms bu;
				bu.dir[0] = dx;
				bu.dir[1] = dy;
				bu.dir[2] = 1.0f / static_cast<float>(tw);
				bu.dir[3] = 1.0f / static_cast<float>(th);
				MTLRenderPassDescriptor* bp = [MTLRenderPassDescriptor renderPassDescriptor];
				bp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)dst;
				bp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				bp.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> benc = [cmdBuf renderCommandEncoderWithDescriptor:bp];
				[benc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrBlurPipeline];
				[benc setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
				[benc setFragmentTexture:(__bridge id<MTLTexture>)src atIndex:0];
				[benc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
				[benc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[benc endEncoding];
				++m_counters.draws;
			};
			// The blur stands in for TWO things the trace did not sample, and
			// sizing it to only one of them is what kept inverting this ladder:
			//   · the RESOLUTION — one reflection texel covers resDiv screen
			//     pixels, so a tap must reach that far. Constant per tier.
			//   · the LOBE — a glossy surface scatters over a cone that a handful
			//     of rays only sparsely sample, and the leftover variance IS the
			//     noise. This term was missing entirely, so a rough surface got a
			//     1–4 texel blur over a lobe tens of pixels across, and every
			//     extra ray sharpened the trace without widening the filter that
			//     has to clean up what the rays still missed. It scales with the
			//     widest lobe the settings allow (maxRoughness), divided by the
			//     RAY COUNT: the blur only has to stand in for the part of the
			//     lobe the rays did not sample, and four rays leave a quarter of
			//     what one leaves. Keeping this term constant across tiers (the
			//     previous attempt) meant High blurred as hard as Low despite
			//     tracing sixteen times the samples per screen area — measurably
			//     softer, which is exactly what it was reported as.
			// The reach is built as an À-TROUS CHAIN of the same 5-tap kernel at
			// doubling strides, NOT as one wide 5-tap: five taps spread over 16
			// texels leave visible banding between them, while 1+2+4+8 covers the
			// same span densely for the same number of passes.
			//
			// m_giReflTex keeps the UNBLURRED trace and m_giReflRoughTex the
			// blurred copy; the roughness lerp between them (deferred: in the
			// composite, forward: the mix pass below) is what keeps a MIRROR
			// mirror-sharp at every tier while a rough surface gets the lobe.
			// Screen pixels → reflection texels: one texel spans resDiv of them.
			const float lobeScreen = kGIReflLobeScreenPx
			                       * std::clamp(m_giReflMaxRoughness, 0.0f, 1.0f)
			                       / static_cast<float>(std::max(rays, 1));
			const float reach = std::clamp(1.0f + lobeScreen / static_cast<float>(resDiv),
			                               1.0f, 512.0f); // also pins a NaN setting
			// Levels, then strides SCALED to land on `reach` exactly. Doubling raw
			// strides 1,2,4,… and stopping at the first one past the target snaps
			// the total to 2^n − 1, so the "tier-independent" screen reach came out
			// quantized — Medium could end up blurring WIDER than Low, which is the
			// inversion this whole rework exists to remove. n levels of scaled
			// strides sum to `reach` for any value.
			const int levels = std::clamp(
				static_cast<int>(std::ceil(std::log2(reach + 1.0f))), 1, 6);
			const float unit = reach / (std::exp2(static_cast<float>(levels)) - 1.0f);
			// Every pass reads ONE texture and writes a DIFFERENT one: H always
			// lands in ping, V always in rough, and the next level reads rough.
			// Letting a non-final level write back into ping is a read-write
			// hazard on the same texture — on a tile GPU that does not fail, it
			// silently produces large BLACK TILES, which is what the first
			// version of this chain rendered.
			void* blurSrc = m_giReflTex;
			for (int lvl = 0; lvl < levels; ++lvl)
			{
				const float stride = unit * std::exp2(static_cast<float>(lvl));
				blurPass(blurSrc, m_giReflPingTex, stride / static_cast<float>(tw), 0.0f);
				blurPass(m_giReflPingTex, m_giReflRoughTex, 0.0f, stride / static_cast<float>(th));
				blurSrc = m_giReflRoughTex;
			}
			{
				// FORWARD path only: bake the sharp/blurred roughness lerp into
				// the texture the scene shader samples — it only gets ONE. The
				// deferred composite does the same lerp per pixel itself, so
				// doing it here too would blur twice. Result lands in ping,
				// which the blur chain is finished with.
				if (!deferredIn && m_ssrRoughMixPipeline)
				{
					HE::MaterialShaderLibrary::SSRBlurUniforms bu;
					bu.dir[0] = m_giReflMaxRoughness; // lerp cutoff (see kSSRRoughMixFS)
					bu.dir[1] = giReflBlurFloor;      // resolution floor
					bu.dir[2] = 1.0f / static_cast<float>(tw);
					bu.dir[3] = 1.0f / static_cast<float>(th);
					MTLRenderPassDescriptor* mp = [MTLRenderPassDescriptor renderPassDescriptor];
					mp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_giReflPingTex;
					mp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
					mp.colorAttachments[0].storeAction = MTLStoreActionStore;
					id<MTLRenderCommandEncoder> menc = [cmdBuf renderCommandEncoderWithDescriptor:mp];
					[menc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssrRoughMixPipeline];
					[menc setFragmentBytes:&bu length:sizeof(bu) atIndex:0];
					[menc setFragmentTexture:(__bridge id<MTLTexture>)m_giReflTex      atIndex:0];
					[menc setFragmentTexture:(__bridge id<MTLTexture>)m_giReflRoughTex atIndex:1];
					[menc setFragmentTexture:(__bridge id<MTLTexture>)attrTex          atIndex:2];
					for (int i = 0; i < 3; ++i)
						[menc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:i];
					[menc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
					[menc endEncoding];
					++m_counters.draws;
					m_giReflGlossyTex = m_giReflPingTex;
				}
			}
		}
	}
}

// ─── Deferred G-buffer pass ──────────────────────────────────────────────────
// The plan's §4.1: the same draw loop as the forward opaque pass, only with
// G-buffer pipelines and WITHOUT the lighting binds (CSM/SkyEnv/AO/GI belong to
// the resolve alone). Runs its own extract/cull/sort — deterministic, so it
// produces exactly the draw set EncodeScene's later extract sees (the same
// convention EncodeSSAO already relies on). Translucent draws and custom
// materials without a G-buffer variant are collected into `out` and replayed
// forward in the lighting pass.
void MetalRenderer::EncodeGBuffer(void* renderEncoder, int width, int height, MetalDeferredFrame& out)
{
	if (!m_world || !m_gbufferPipeline || width <= 0 || height <= 0)
		return;

	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height),
	                    &m_editorCamera);

	// Rasterised with this frame's subpixel jitter when TAA is on (no-op
	// otherwise). The clean matrix stays available for the velocity pass, which
	// must measure motion, not jitter.
	const glm::mat4 viewProj = JitteredViewProj(
		m_renderWorld.camera.projection * m_renderWorld.camera.view, width, height);

	if (m_renderWorld.objects.empty()) return;

	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;

	void* const defaultPipeline = m_gbufferPipeline;
	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)defaultPipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];

#if defined(HE_HAVE_SHADERC)
	// Minimal lighting ABI for the G-buffer stage: graph bodies may read the
	// Time input (heLight.sunDir.w) and camera position, and WPO vertices bind
	// the block at vertex buffer 2. Shadow/GI matrices stay zero — nothing in a
	// G-buffer fragment samples them (the resolve owns the full fill).
	float gbClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) gbClock = static_cast<float>(std::atof(ov));
	HE::MaterialShaderLibrary::Lighting matLight;
	{
		glm::vec3 matSunDir, matSunColor;
		m_renderWorld.dominantDirectionalLight(matSunDir, matSunColor);
		const glm::vec3 am = m_renderWorld.ambient;
		matLight.sunDir[0]   = matSunDir.x; matLight.sunDir[1] = matSunDir.y; matLight.sunDir[2] = matSunDir.z;
		matLight.sunDir[3]   = gbClock;
		matLight.camPos[0]   = m_renderWorld.camera.position.x;
		matLight.camPos[1]   = m_renderWorld.camera.position.y;
		matLight.camPos[2]   = m_renderWorld.camera.position.z;
		matLight.sunColor[0] = matSunColor.r; matLight.sunColor[1] = matSunColor.g; matLight.sunColor[2] = matSunColor.b;
		matLight.ambient[0]  = am.r;          matLight.ambient[1]  = am.g;          matLight.ambient[2]  = am.b;
		matLight.giParams[0] = static_cast<float>(width);
		matLight.giParams[1] = static_cast<float>(height);
		// Specular AA (A6): the G-buffer IS a geometry pass, so graph materials
		// widen the roughness they store here — the resolve could not, its normal
		// is already an encoded texel.
		matLight.specAA[0] = m_specularAA ? m_specularAAStrength : 0.0f;
		matLight.specAA[1] = 1.0f;
	}
	[encoder setFragmentBytes:&matLight length:sizeof(matLight)
	                  atIndex:HE::MaterialShaderLibrary::kMetalLightingBufferIndex];
#endif

	// Specular AA for the BUILT-IN G-buffer shader (gbufferMain reads
	// aaParams.x from SceneUniforms at fragment buffer 0). Only this one field
	// matters here — the G-buffer writes attributes, it does not shade.
	{
		SceneUniforms gbScene{};
		gbScene.aaParams = glm::vec4(m_specularAA ? m_specularAAStrength : 0.0f, 0.0f, 0.0f, 0.0f);
		[encoder setFragmentBytes:&gbScene length:sizeof(gbScene) atIndex:0];
	}

	const glm::vec3 camPos = m_renderWorld.camera.position;

	if (m_renderGraph.empty())
		m_renderGraph.addPass(std::make_unique<GeometryPass>());

	m_renderGraph.execute(m_renderWorld, m_sortedIndices,
		[&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
	{
		if (io.output.id != kBackbufferTarget) return;
		// Same memoisation as the forward loop (draws arrive mesh/material-sorted).
		HE::UUID  lastMeshId{};       const GpuMesh* cMesh = nullptr; bool meshValid = false;
		HE::UUID  lastMatId{};        bool matValid = false;
		void*     cOverrideTex = nullptr; bool cHasOverride = false;
		glm::vec3 cBaseColor(1.0f);   float cMetallic = 0.0f, cRoughness = 0.5f; bool cHasMat = false;
		float     cOpacity = 1.0f;
		void*     cMaterialPipeline      = nullptr; // forward PSO (forward-routed replay)
		void*     cMaterialPipelineBlend = nullptr; // blended forward PSO (transparency)
		void*     cMaterialPipelineGB    = nullptr; // G-buffer PSO (this pass)
		bool      cMaterialWpo           = false;
		bool      cMaterialCustom        = false;   // material HAS a custom shader at all
		void*     boundPipeline     = defaultPipeline;
		const std::vector<float>* cMaterialParams = nullptr;
		void* cGraphTex[HE::kMatMaxGraphTextures] = { nullptr };
		int   cGraphTexCount = 0;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			UnlitUniforms u;
			u.mvp   = viewProj * dc.transform;
			u.model = dc.transform;

			if (!matValid || dc.materialAssetId != lastMatId)
			{
				cOverrideTex = nullptr;
				cHasOverride = ResolveMaterialTexture(dc.materialAssetId, cOverrideTex);
				cBaseColor   = glm::vec3(1.0f); cMetallic = 0.0f; cRoughness = 0.5f; cOpacity = 1.0f;
				cHasMat      = ResolveMaterialParams(dc.materialAssetId, cBaseColor, cMetallic, cRoughness, cOpacity);
				lastMatId    = dc.materialAssetId; matValid = true;
				cMaterialPipeline = nullptr;
				cMaterialPipelineBlend = nullptr;
				cMaterialPipelineGB = nullptr;
				cMaterialWpo      = false;
				cMaterialCustom   = false;
				cMaterialParams   = nullptr;
				cGraphTexCount    = 0;
#if defined(HE_HAVE_SHADERC)
				{
					uint64_t shKey; std::string shFrag, shVert;
					if (ResolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert))
					{
						cMaterialCustom = true;
						const MaterialShaderVariant* pre = nullptr;
						if (const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
							for (const auto& var : ma->precompiledShaders)
								if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { pre = &var; break; }
						cMaterialWpo      = !shVert.empty();
						// Forward PSOs still needed: blended for the transparency
						// replay, opaque for the forward-routed fallback.
						cMaterialPipeline = GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre);
						if (cMaterialPipeline)
							cMaterialPipelineBlend =
								GetOrBuildMaterialPipeline(shKey, shFrag, shVert, pre, /*blend=*/true);
						// G-buffer variant — always runtime cross-compiled (baked
						// packs carry no G-buffer MSL yet), never blended.
						uint64_t gbKey; std::string gbFrag, gbVert;
						if (ResolveMaterialShaderGB(dc.materialAssetId, gbKey, gbFrag, gbVert))
							cMaterialPipelineGB = GetOrBuildMaterialPipeline(
								gbKey, gbFrag, gbVert, nullptr, /*blend=*/false, /*gbuffer=*/true);
						if (const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
						{
							if (!ma->shaderParamData.empty()) cMaterialParams = &ma->shaderParamData;
							const size_t nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
								std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
							for (size_t i = 0; i < nTex; ++i)
							{
								const HE::UUID    id = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
								const std::string p  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
								cGraphTex[cGraphTexCount++] = ResolveGraphTexture(id, p);
							}
						}
					}
				}
#endif
			}
			u.pbr = glm::vec4(cMetallic, cRoughness, cOpacity, 0.0f);

			bool meshWasResolved = false;
			if (!meshValid || dc.meshAssetId != lastMeshId)
			{
				cMesh      = ResolveMesh(dc.meshAssetId);
				lastMeshId = dc.meshAssetId; meshValid = true;
				meshWasResolved = true;
			}
			const GpuMesh* drawMesh = cMesh;
			if (!drawMesh) { drawMesh = ResolveMesh(HE::kDefaultCubeMeshId); meshWasResolved = true; }
			if (!drawMesh) continue;
			// The G-buffer twin of the forward pass's re-take — same reason, same
			// shape: ResolveMesh loads the mesh's material and moves the pool the
			// cached pointer lives in.
			if (meshWasResolved && cMaterialParams)
			{
				const MaterialAsset* ma = m_contentManager
					? m_contentManager->getMaterial(dc.materialAssetId) : nullptr;
				cMaterialParams = (ma && !ma->shaderParamData.empty()) ? &ma->shaderParamData : nullptr;
			}
			id<MTLBuffer> vertexBuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
			id<MTLBuffer> indexBuf  = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
			NSUInteger    indexCount = (NSUInteger)drawMesh->indexCount;
			void*         meshTex = drawMesh->texture;

			void* effectiveTex = cHasOverride ? cOverrideTex : meshTex;
			void* texPtr = effectiveTex ? effectiveTex : m_dummyTexture;
			id<MTLTexture> texture = (__bridge id<MTLTexture>)texPtr;
			// gbufferMain drops the shadow lane (the resolve has no per-object
			// channel), but translucent draws leave this loop as TPDraws replayed
			// through the FORWARD fragment, which reads it.
			u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, dc.receivesShadow ? 0.0f : 1.0f, 0, 0);

			glm::vec3 baseColor = cBaseColor;
			if (!cHasMat)
				baseColor = effectiveTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);
			baseColor *= glm::vec3(dc.instanceTint);
			u.pbr.z   *= dc.instanceTint.a;
			u.color = glm::vec4(baseColor, 1.0f);

			auto drawInstance = [&](const glm::mat4& xform)
			{
				UnlitUniforms ui = u;
				ui.mvp   = viewProj * xform;
				ui.model = xform;
				// Same routing threshold as the forward loop — translucent draws
				// go to the lighting pass's sorted blend replay.
				if (ui.pbr.z < RenderSorter::kOpaqueOpacityThreshold)
				{
					TPDraw t{ ui, (__bridge void*)vertexBuf, (__bridge void*)indexBuf,
					          indexCount, texPtr, RenderSorter::backToFrontKey(xform, camPos) };
					if (cMaterialPipelineBlend)
					{
						t.pipeline = cMaterialPipelineBlend;
						t.wpo      = cMaterialWpo;
						if (cMaterialParams) t.params = *cMaterialParams;
						if (!dc.paramOverride.empty()) t.params = dc.paramOverride;
						for (int i = 0; i < cGraphTexCount; ++i) t.gtex[i] = cGraphTex[i];
						t.gtexCount = cGraphTexCount;
					}
					out.transparent.push_back(std::move(t));
					return;
				}
				// Custom material without a G-buffer variant (hand-written GLSL,
				// packaged without a graph, or its G-buffer PSO failed to build):
				// route the draw forward into the lighting pass (plan §8).
				if (cMaterialCustom && !cMaterialPipelineGB)
				{
					TPDraw t{ ui, (__bridge void*)vertexBuf, (__bridge void*)indexBuf,
					          indexCount, texPtr, 0.0f };
					t.pipeline = cMaterialPipeline; // may be null → built-in forward PBR
					t.wpo      = cMaterialWpo;
					if (cMaterialParams) t.params = *cMaterialParams;
					if (!dc.paramOverride.empty()) t.params = dc.paramOverride;
					for (int i = 0; i < cGraphTexCount; ++i) t.gtex[i] = cGraphTex[i];
					t.gtexCount = cGraphTexCount;
					out.forwardOpaque.push_back(std::move(t));
					return;
				}
				void* wantPipeline = cMaterialPipelineGB ? cMaterialPipelineGB : defaultPipeline;
				if (wantPipeline != boundPipeline)
				{
					[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)wantPipeline];
					boundPipeline = wantPipeline;
				}
				if (cMaterialPipelineGB && (cMaterialParams || !dc.paramOverride.empty()))
				{
					const std::vector<float>& src =
						!dc.paramOverride.empty() ? dc.paramOverride : *cMaterialParams;
					float padded[64] = { 0 };
					std::memcpy(padded, src.data(),
					            std::min(src.size(), size_t(64)) * sizeof(float));
					[encoder setFragmentBytes:padded length:sizeof(padded) atIndex:2];
				}
				if (cMaterialPipelineGB)
					for (int i = 0; i < cGraphTexCount; ++i)
						if (cGraphTex[i])
						{
							[encoder setFragmentTexture:(__bridge id<MTLTexture>)cGraphTex[i] atIndex:(i + 1)];
							[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:(i + 1)];
						}
				// Landscape layer weightmap → MSL texture 13, per draw (same as forward).
				if (cMaterialPipelineGB)
				{
					void* wm = dc.weightmapTextureId != HE::UUID{}
						? ResolveGraphTexture(dc.weightmapTextureId, {})
						: nullptr;
					if (!wm) wm = ResolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {});
					if (wm)
					{
						[encoder setFragmentTexture:(__bridge id<MTLTexture>)wm atIndex:13];
						[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:13];
					}
				}
				[encoder setVertexBuffer:vertexBuf offset:0 atIndex:0];
				[encoder setVertexBytes:&ui length:sizeof(ui) atIndex:1];
#if defined(HE_HAVE_SHADERC)
				if (cMaterialPipelineGB && cMaterialWpo)
				{
					[encoder setVertexBytes:&matLight length:sizeof(matLight) atIndex:2];
					float vpad[64] = { 0 };
					const std::vector<float>* vsrc =
						!dc.paramOverride.empty() ? &dc.paramOverride : cMaterialParams;
					if (vsrc)
						std::memcpy(vpad, vsrc->data(),
						            std::min(vsrc->size(), size_t(64)) * sizeof(float));
					[encoder setVertexBytes:vpad length:sizeof(vpad) atIndex:3];
				}
#endif
				[encoder setFragmentTexture:texture atIndex:0];
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                    indexCount:indexCount
				                     indexType:MTLIndexTypeUInt32
				                   indexBuffer:indexBuf
				             indexBufferOffset:0];
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3);
			};
			// The forward pass's twin (plan §4), with one criterion of its own:
			// cMaterialCustom. A custom material without a G-buffer variant leaves
			// this loop as a forwardOpaque re-route, which an instanced draw has no
			// way to perform — and one WITH a variant carries its own vertex stage.
			static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be mvp+model");
			const size_t instCount = dc.instanceTransforms.size();
			const bool fits = instCount > 0
			               && metalInstancingEnabled()
			               && m_gbufferInstancedPipeline
			               && instCount <= k_maxInstances
			               && !cMaterialCustom
			               && cMaterialPipelineGB == nullptr
			               && dc.paramOverride.empty()
			               && u.pbr.z >= RenderSorter::kOpaqueOpacityThreshold;
			if (fits)
			{
				if (m_gbufferInstancedPipeline != boundPipeline)
				{
					[encoder setRenderPipelineState:
						(__bridge id<MTLRenderPipelineState>)m_gbufferInstancedPipeline];
					boundPipeline = m_gbufferInstancedPipeline;
				}
				std::vector<glm::mat4> xf;
				xf.reserve(instCount * 2);
				for (const glm::mat4& t : dc.instanceTransforms)
				{
					xf.push_back(viewProj * t); // mvp
					xf.push_back(t);            // model
				}
				id<MTLDevice> dev = (__bridge id<MTLDevice>)m_device;
				id<MTLBuffer> instBuf = [dev newBufferWithBytes:xf.data()
					length:xf.size() * sizeof(glm::mat4)
					options:MTLResourceStorageModeShared];
				[encoder setVertexBuffer:vertexBuf offset:0 atIndex:0];
				[encoder setVertexBytes:&u length:sizeof(u) atIndex:1]; // mvp/model unused here
				[encoder setVertexBuffer:instBuf offset:0 atIndex:5];
				[encoder setFragmentTexture:texture atIndex:0];
				[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
				                    indexCount:indexCount
				                     indexType:MTLIndexTypeUInt32
				                   indexBuffer:indexBuf
				             indexBufferOffset:0
				                 instanceCount:(NSUInteger)instCount];
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3)
				                 * static_cast<uint32_t>(instCount);
			}
			else if (dc.instanceTransforms.empty())
				drawInstance(dc.transform);
			else
				for (const glm::mat4& t : dc.instanceTransforms)
					drawInstance(t);
		}
	});
}

void MetalRenderer::EncodeFrame(SDL_Window* sdlWin, WindowTarget& target, bool isPrimary)
{
	@autoreleasepool
	{
		if (isPrimary)
		{
			// Reset the render counters before any early-return below, so a frame
			// that bails out (e.g. minimized / zero-size window) honestly reports
			// zeros instead of last frame's draws/tris/visible/total.
			m_counters = FrameCounters{};

			AgeRetiredTextures();
			AgeRetiredGIObjects();

			// Release cached GPU buffers for any mesh invalidated since last frame
			// (e.g. sculpted terrain). In-flight GPU work may reference them, so
			// release via CFBridgingRelease (ARC autoreleasepool handles safety here).
			for (const HE::UUID& id : m_pendingMeshInvalidations)
			{
				if (auto it = m_meshCache.find(id); it != m_meshCache.end())
				{
					if (it->second.vertexBuf) CFBridgingRelease(it->second.vertexBuf);
					if (it->second.indexBuf)  CFBridgingRelease(it->second.indexBuf);
					if (it->second.blas)      CFBridgingRelease(it->second.blas);
					m_meshCache.erase(it);
				}
				// SW-RT BLAS ranges live in concatenated arrays — a single mesh
				// can't be spliced out, so an edited mesh drops the whole SW cache;
				// it rebuilds lazily on the next GI-active frame (same convention
				// as the GL port).
				if (m_giSwBlasCache.count(id))
				{
					m_giSwBlasCache.clear();
					m_giSwNodesCpu.clear();
					m_giSwTrisCpu.clear();
					m_giSwBlasDirty = true;
				}
			}
			m_pendingMeshInvalidations.clear();

			// Same for textures rewritten in place (landscape weightmap paints).
			// m_graphTexCache is keyed by "hi:lo" for UUID-resolved entries.
			for (const HE::UUID& id : m_pendingTexInvalidations)
			{
				const std::string key = std::to_string(id.hi) + ":" + std::to_string(id.lo);
				if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end())
				{
					if (it->second) RetireTexture(it->second);
					m_graphTexCache.erase(it);
				}
			}
			m_pendingTexInvalidations.clear();
		}

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

		id<MTLCommandQueue>  queue   = (__bridge id<MTLCommandQueue>)m_commandQueue;
		id<MTLCommandBuffer> cmdBuf  = [queue commandBuffer];

		// ── Per-pass GPU timing setup (only while a profiler capture records) ──
		// Builds one over-allocated counter sample buffer for this frame; the major
		// passes hand themselves slots via m_ft (ftPair/ftPoint) as they encode, and
		// the completion handler resolves whatever was used. (The render counters were
		// reset above, before the early returns; EncodeScene fills them this frame.)
		EnsureGpuTimer();
		m_ft.reset();

		// ── Detailed capture (one command buffer per pass) ─────────────────
		// Each render pass committed as its own command buffer → its GPUStartTime/
		// GPUEndTime give exclusive, additive per-pass GPU time (the only reliable
		// per-pass GPU on tile-deferred GPUs). flushPass() commits the current
		// buffer with a timing handler and starts the next; in normal mode it is a
		// no-op, so the fast single-command-buffer path below is unchanged.
		const bool detailed = isPrimary && m_gpuTimer
		                   && EngineProfiler::instance().isRecording()
		                   && EngineProfiler::instance().detailedGpuCapture();
		if (detailed)
		{
			static bool s_loggedDetailed = false;
			if (!s_loggedDetailed)
			{
				HE_LOG_INFO(RHI, "%s",
					"Metal: detailed GPU capture ENGAGED — one command buffer per pass, "
					"serialized with waitUntilCompleted (per-pass exclusive GPU time; capture is slow)");
				s_loggedDetailed = true;
			}
		}
		const uint64_t detailFrame = detailed ? m_detailFrameIdx++ : 0;
		std::shared_ptr<MetalGpuTimerShared> detailShared = m_gpuTimer;
		const char* curPass = "Shadow";   // the first command buffer covers the shadow pass
		auto attachDetail = [detailShared, detailFrame](id<MTLCommandBuffer> cb, const char* passName)
		{
			[cb addCompletedHandler:^(id<MTLCommandBuffer> done)
			{
				IRenderer::FrameGpuStats out;
				if (detailShared->accum.report(detailFrame, passName,
				                               done.GPUStartTime, done.GPUEndTime,
				                               kDetailedPassCount, out))
				{
					out.gpuTimingMode = "detailed";   // stamp the path that actually ran
					std::lock_guard<std::mutex> lk(detailShared->mutex);
					detailShared->last = out;
				}
			}];
		};
		// flushPass commits the current pass's command buffer and — crucially —
		// waitUntilCompleted before starting the next, so the passes do NOT overlap on
		// the GPU timeline (commit order alone does not prevent overlap on Apple GPUs).
		// That makes each pass's GPUStartTime/GPUEndTime an exclusive, additive cost.
		// No-op in normal mode → the fast single-command-buffer path is unchanged.
		auto flushPass = [&](const char* nextPass)
		{
			if (!detailed) return;
			attachDetail(cmdBuf, curPass);
			[cmdBuf commit];
			[cmdBuf waitUntilCompleted];
			cmdBuf  = [queue commandBuffer];
			curPass = nextPass;
		};

		// Stage-boundary counter sampling — the NON-detailed per-encoder path. Off in
		// detailed mode and where the GPU lacks counter support. (These spans overlap
		// on TBDR; the profiler flags that. Detailed capture is the reliable per-pass.)
		id<MTLCounterSampleBuffer> sampleBuf = nil;   // strong ref kept alive until commit
		if (isPrimary && m_counterSamplingOk && !detailed && EngineProfiler::instance().isRecording())
		{
			if (@available(macOS 11.0, *))
			{
				id<MTLDevice> dev = (__bridge id<MTLDevice>)m_device;
				// Refresh CPU↔GPU timestamp correlation (ns per GPU tick) from the
				// delta between this frame's and the previous frame's sample pair.
				MTLTimestamp cpuTs = 0, gpuTs = 0;
				[dev sampleTimestamps:&cpuTs gpuTimestamp:&gpuTs];
				if (m_prevGpuTs != 0 && gpuTs > m_prevGpuTs && cpuTs > m_prevCpuTs)
				{
					const double nsPerTick =
						(double)(cpuTs - m_prevCpuTs) / (double)(gpuTs - m_prevGpuTs);
					m_gpuTimer->gpuTicksToMs.store(nsPerTick / 1.0e6, std::memory_order_relaxed);
				}
				m_prevCpuTs = cpuTs; m_prevGpuTs = gpuTs;

				MTLCounterSampleBufferDescriptor* sd = [[MTLCounterSampleBufferDescriptor alloc] init];
				sd.counterSet  = (__bridge id<MTLCounterSet>)m_timestampCounterSet;
				sd.storageMode = MTLStorageModeShared;
				sd.sampleCount = kMaxGpuSamples;
				NSError* err = nil;
				sampleBuf = [dev newCounterSampleBufferWithDescriptor:sd error:&err];
				if (sampleBuf)
				{
					m_ft.sampleBuf = (__bridge void*)sampleBuf;
					m_ft.stage     = true;             // per-encoder timing this frame
					m_ft.draw      = m_drawBoundary;   // intra-Scene element splits if supported
				}
			}
		}

		// ── Shadow map + scene → HDR target + offscreen tonemap ─────────────
		// Encoded before acquiring the drawable so the editor viewport texture
		// is produced even when the window has no drawable (occluded/background).
		// Only the swapchain present below needs the drawable.
		// Hoisted out of the isPrimary block below (not just const-local there) so
		// EncodeGIShadowRays further down can reuse the same offscreen-viewport-or-
		// window sizing without recomputing it — harmless (== pw/ph) when !isPrimary.
		int shW = pw, shH = ph;
		if (isPrimary)
		{
			// Shadow cascades MUST be fit with the SAME aspect the scene pass uses
			// (below), else render≠sample cascade matrices → shadow swimming.
			const bool shOff = m_viewportReqW > 0 && m_viewportReqH > 0;
			shW = shOff ? (int)m_viewportReqW : pw;
			shH = shOff ? (int)m_viewportReqH : ph;
			EncodeShadowMap((__bridge void*)cmdBuf,
			                shH > 0 ? static_cast<float>(shW) / static_cast<float>(shH) : 1.0f);
			// Cloud-shadow map: rendered before the G-buffer/scene passes (both
			// sample it at texture 16). Uses the extraction EncodeShadowMap just
			// ran (dominant light + camera).
			EncodeCloudShadow((__bridge void*)cmdBuf);
		}

		// Step the GPU weather-particle pool once per frame (primary only), before the
		// scene render encoder reads it. Metal tracks the compute→vertex dependency on
		// the shared buffer within this command buffer. No-op when the path is disabled.
		if (isPrimary)
			SimulateGpuParticles((__bridge void*)cmdBuf);
		flushPass("GIAccel");   // detailed: commit the Shadow (+ particle sim) command buffer

		// Ray-traced GI acceleration structures (BLAS/TLAS): lazy per-mesh BLAS +
		// a full TLAS rebuild from this frame's caster set. No-op (early return)
		// unless GI is enabled and the device/OS supports ray tracing — CSM/AO stay
		// byte-identical to today in that case. Same aspect as EncodeShadowMap/the
		// scene pass — EncodeGIShadowRays rasterizes its G-buffer with THIS
		// extraction's camera, and a mismatched aspect misaligns the shadow mask
		// against the scene pass's screen-space sampling.
		if (isPrimary)
			EncodeGIAccelBuild((__bridge void*)cmdBuf,
			                   shH > 0 ? static_cast<float>(shW) / static_cast<float>(shH) : 1.0f);
		flushPass("GIShadow");   // detailed: commit the GIAccel command buffer (empty if GI off)

		// Ray-traced shadows (replaces CSM's shadowFactor() sampling when GI is
		// active): half-res world-space G-buffer + 1 ray/pixel + temporal
		// accumulation + spatial blur — same half-res convention as EncodeSSAO
		// below (halved at the call site, not inside the function). shW/shH match
		// what the scene pass will use (offscreen-viewport-or-window sizing),
		// computed above for the CSM cascade fit. No-op (early return) unless GI
		// is enabled/supported and EncodeGIAccelBuild actually built a TLAS.
		if (isPrimary)
			EncodeGIShadowRays((__bridge void*)cmdBuf, std::max(1, shW / 2), std::max(1, shH / 2));
		flushPass("GIProbes");   // detailed: commit the GIShadow command buffer (empty if GI off)

		// DDGI probe update (Checkpoint C): frame-sliced — updates up to
		// probeBudgetPerFrame probes/frame, round-robin. No-op (early return)
		// unless GI is enabled/supported/has a built TLAS; lazily builds the probe
		// grid + atlases on first call. Replaces AO + flat/IBL ambient in
		// fragmentMain when active — see EncodeScene's giActive gate below.
		if (isPrimary)
			EncodeGIProbeUpdate((__bridge void*)cmdBuf);
		flushPass("SSAO");   // detailed: commit the GIProbes command buffer (empty if GI off)

		const bool offscreen = isPrimary && m_viewportReqW > 0 && m_viewportReqH > 0;
		if (isPrimary)
		{
			// Output size (what the user sees) vs. RENDER size (what the scene is
			// rasterized at). Render scale (A4) separates the two: everything
			// scene-side below uses sceneW/sceneH, and the AA-resolve pass — a
			// fullscreen triangle sampling in normalized UV — rescales to the
			// output for free. < 1 upscales, > 1 is plain supersampling.
			const int outW = offscreen ? (int)m_viewportReqW : pw;
			const int outH = offscreen ? (int)m_viewportReqH : ph;
			const float rscale = std::clamp(m_renderScale, 0.25f, 2.0f);
			const int sceneW = std::max(1, (int)std::lround(outW * rscale));
			const int sceneH = std::max(1, (int)std::lround(outH * rscale));
			EnsureHDRTarget(sceneW, sceneH);

			// ── Deferred G-buffer pass (docs/deferred-renderer-plan.md) ─────────
			// When the render path is Deferred (and the pipelines built), the
			// opaque scene is rasterized into the G-buffer first; the HDR pass
			// below then starts with a fullscreen lighting resolve instead of the
			// opaque loop and LOADS the blitted G-buffer depth so sky/skinned/
			// transparency depth-test against the deferred geometry. Encoded
			// BEFORE SSAO (plan P5): the AO pre-pass then reconstructs its
			// view-space positions from the G-buffer depth in a single fullscreen
			// draw instead of re-rasterizing the whole scene.
			MetalDeferredFrame deferredFrame;
			const bool deferredActive =
				m_renderPath == HE::RenderPath::Deferred && EnsureDeferredPipelines();
			m_deferredFrameActive = deferredActive;

			// ── TAA: this frame's jitter (A2) ───────────────────────────────
			// Halton(2,3), 8 positions: a low-discrepancy sequence covers the
			// pixel evenly in few frames, where a random offset clumps and a
			// regular grid re-aliases. Chosen BEFORE anything builds a matrix,
			// because every rasterising pass this frame must share one offset.
			if (TemporalActive())
			{
				EnsureTaaTargets(sceneW, sceneH);
				auto halton = [](uint32_t i, uint32_t base) {
					float f = 1.0f, r = 0.0f;
					while (i > 0) { f /= static_cast<float>(base); r += f * (i % base); i /= base; }
					return r;
				};
				const uint32_t n = (m_taaFrameIndex % 8u) + 1u;
				m_taaJitter = glm::vec2(halton(n, 2) - 0.5f, halton(n, 3) - 0.5f);
				++m_taaFrameIndex;
			}
			else if (m_taaHistory)
			{
				// Freed as soon as the mode is off, and the history is dropped
				// with it — a stale one would blend against a different world
				// the moment TAA comes back on.
				DestroyTaaTargets();
				DestroyMetalFX();
				m_taaJitter = glm::vec2(0.0f);
			}
			const bool deferredTile = deferredActive && m_deferredTileMode;
			// SSR: deferred TILE mode traces the stored G-buffer + this frame's
			// resolved HDR (§4.5, lag-free); the FORWARD path traces the MRT
			// pre-pass + last frame's HDR copy (Option A, 1 frame content lag).
			// Only the two-pass deferred fallback has no SSR.
			m_ssrFrameActive = m_ssrEnabled && EnsureSSRPipelines()
			                && (deferredTile || !deferredActive);
			// Ray-traced GI reflections: tile mode composites onto the resolved
			// HDR; the forward path shades from the same MRT pre-pass and the
			// scene shaders sample the result (slot 10). Both need the accel
			// structures EncodeGIAccelBuild built earlier this frame.
			m_giReflFrameActive = (deferredTile || !deferredActive) && m_giReflEnabled
			                   && (m_giHwRt
			                           ? m_giTlas != nullptr
			                           : (m_giSwNodeBuf && m_giSwTriBuf && m_giSwInstanceBuf
			                              && m_giSwInstanceCount > 0))
			                   && EnsureSSRPipelines() && EnsureGIReflPipeline();
			// Forward reflections want the MRT pre-pass (and EncodeSSAO runs
			// even with SSAO off/replaced — occlusion+blur skipped then).
			const bool giReplacesAOEarly = m_giEnabled && m_giSupported;
			m_fwdReflPrepassWanted = !deferredActive
			                      && (m_ssrFrameActive || m_giReflFrameActive);
			m_fwdReflPrepassOnly   = m_fwdReflPrepassWanted
			                      && !(m_ssaoEnabled && !giReplacesAOEarly);
			m_fwdReflSsrTex = nullptr;
			m_fwdReflGiTex  = nullptr;
			{
				// One-shot diagnostic: which scene-pass mode actually runs. Logged
				// on every CHANGE (not per frame) so a mid-session flip is visible.
				static int s_lastMode = -1;
				const int mode = deferredTile ? 2 : (deferredActive ? 1 : 0);
				if (mode != s_lastMode)
				{
					s_lastMode = mode;
					HE_LOG_INFO(RHI, "%s", mode == 2
						? "MetalRenderer: scene pass mode → deferred (tile single-pass)"
						: mode == 1 ? "MetalRenderer: scene pass mode → deferred (two-pass)"
						            : "MetalRenderer: scene pass mode → forward");
				}
			}

			// SSAO occlusion (its own pre-pass + encoders) before the shading pass,
			// so the scene shader can darken its ambient. Skipped (zero cost) off —
			// including when GI is active, since GI's probe-sampled indirect diffuse
			// replaces AO entirely (EncodeScene's giActive gate skips aoTex too).
			// Rendered at HALF resolution (~4× cheaper, no visible quality loss —
			// AO is low-frequency and blurred; sampled with normalized coords).
			// Ordering: the two-pass deferred fallback encodes the G-buffer FIRST
			// so the pre-pass can read its depth (P5); the tile mode must run
			// SSAO BEFORE its single pass (the resolve inside that pass consumes
			// the result) and keeps the classic geometry pre-pass — its
			// memoryless G-buffer has no stored depth to reconstruct from.
			const bool giReplacesAO = m_giEnabled && m_giSupported;
			auto runSSAO = [&]{
				if ((m_ssaoEnabled && !giReplacesAO) || m_fwdReflPrepassWanted)
					EncodeSSAO((__bridge void*)cmdBuf,
					           std::max(1, sceneW / 2), std::max(1, sceneH / 2));
				if (!(m_ssaoEnabled && !giReplacesAO)) m_ssaoResult = nullptr;
			};
			if (deferredTile) runSSAO();

			if (deferredActive)
			{
				EnsureGBufferTargets(sceneW, sceneH);
				MTLRenderPassDescriptor* gbPass = [MTLRenderPassDescriptor renderPassDescriptor];
				id<MTLTexture> gbTex[4] = { (__bridge id<MTLTexture>)m_gbColor0,
				                            (__bridge id<MTLTexture>)m_gbColor1,
				                            (__bridge id<MTLTexture>)m_gbColor2,
				                            (__bridge id<MTLTexture>)m_gbDepthLin };
				for (int a = 0; a < 4; ++a)
				{
					gbPass.colorAttachments[a].texture     = gbTex[a];
					gbPass.colorAttachments[a].loadAction  = MTLLoadActionClear;
					// Memoryless attachments (tile mode) must not store; the
					// two-pass fallback — and the tile mode with SSR or GI
					// reflections, whose trace/composite sample the G-buffer
					// after the pass — store them.
					gbPass.colorAttachments[a].storeAction =
						(deferredTile && !m_ssrFrameActive && !m_giReflFrameActive)
							? MTLStoreActionDontCare
							: MTLStoreActionStore;
					gbPass.colorAttachments[a].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
				}
				// Clear GB1's normal channels to the encoded +Z so untouched pixels
				// decode to a valid direction, and the NDC-depth attachment to 1
				// (background — the resolve discards there).
				gbPass.colorAttachments[1].clearColor = MTLClearColorMake(0.5, 0.5, 1.0, 0.5);
				gbPass.colorAttachments[3].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 0.0);
				if (deferredTile)
				{
					// Single pass (P6): the HDR colour rides as attachment 4 and
					// the pass writes m_hdrDepth directly — no blit, and the
					// G-buffer never leaves tile storage.
					gbPass.colorAttachments[4].texture     = (__bridge id<MTLTexture>)m_hdrColor;
					gbPass.colorAttachments[4].loadAction  = MTLLoadActionClear;
					gbPass.colorAttachments[4].storeAction = MTLStoreActionStore;
					gbPass.colorAttachments[4].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
					gbPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
					gbPass.depthAttachment.loadAction  = MTLLoadActionClear;
					gbPass.depthAttachment.storeAction = MTLStoreActionStore; // pass 2 loads it
					gbPass.depthAttachment.clearDepth  = 1.0;
				}
				else
				{
					gbPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_gbDepth;
					gbPass.depthAttachment.loadAction  = MTLLoadActionClear;
					gbPass.depthAttachment.storeAction = MTLStoreActionStore; // sampled by the resolve
					gbPass.depthAttachment.clearDepth  = 1.0;
				}
				id<MTLRenderCommandEncoder> gbEncoder =
					[cmdBuf renderCommandEncoderWithDescriptor:gbPass];
				EncodeGBuffer((__bridge void*)gbEncoder, sceneW, sceneH, deferredFrame);
				if (deferredTile)
				{
					// Decal projectors blend into GB0 (framebuffer-fetched depth)
					// BEFORE the resolve reads the attributes.
					EncodeDecals((__bridge void*)gbEncoder, sceneW, sceneH);
					// Lighting resolve INSIDE the pass, via framebuffer fetch.
					EncodeDeferredResolveTile((__bridge void*)gbEncoder, sceneW, sceneH);
					deferredFrame.resolveDone = true;
				}
				[gbEncoder endEncoding];

				// Screen-space motion, straight after the pass whose depth it
				// tests against (A2). Only runs when TAA is the active mode.
				EncodeVelocity((__bridge void*)cmdBuf, sceneW, sceneH);

				// Reflections: the GI compute trace fills m_giReflTex first,
				// then the shared composite (SSR trace + additive env-specular
				// pass) adds the skipped specular-IBL term back — before the
				// forward tail so transparency draws over the reflections.
				if (deferredTile && (m_ssrFrameActive || m_giReflFrameActive))
				{
					EncodeGIReflections((__bridge void*)cmdBuf, sceneW, sceneH);
					EncodeSSRPasses((__bridge void*)cmdBuf, sceneW, sceneH);
				}

				if (!deferredTile)
				{
					// Two-pass fallback: copy the G-buffer depth into the HDR
					// pass's depth attachment so the forward tail tests against
					// it while the resolve SAMPLES m_gbDepth (a texture cannot
					// be both attachment and sampled input of one pass).
					id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
					[blit copyFromTexture:(__bridge id<MTLTexture>)m_gbDepth
					          sourceSlice:0 sourceLevel:0
					         sourceOrigin:MTLOriginMake(0, 0, 0)
					           sourceSize:MTLSizeMake(m_gbW, m_gbH, 1)
					            toTexture:(__bridge id<MTLTexture>)m_hdrDepth
					     destinationSlice:0 destinationLevel:0
					    destinationOrigin:MTLOriginMake(0, 0, 0)];
					[blit endEncoding];
				}
			}
			else if (m_gbColor0 && m_renderPath != HE::RenderPath::Deferred)
				DestroyGBufferTargets(); // freed when the user switches back to forward

			if (!deferredTile) runSSAO(); // forward, or two-pass deferred (P5 depth read)

			// FORWARD reflections: the passes consume the MRT pre-pass the
			// runSSAO call above just rendered; results are sampled by the
			// scene shaders (slots 9/10) — no composite pass.
			if (!deferredActive)
			{
				if (m_giReflFrameActive)
				{
					m_giReflGlossyTex = nullptr;
					EncodeGIReflections((__bridge void*)cmdBuf, sceneW, sceneH);
					// Quality 2 hands back the roughness-lerped copy; the lower
					// tiers (and a missing mix pipeline) keep the narrow blur.
					m_fwdReflGiTex = m_giReflGlossyTex ? m_giReflGlossyTex : m_giReflTex;
				}
				EncodeForwardSSR((__bridge void*)cmdBuf, sceneW, sceneH);
			}
			flushPass("Scene");   // detailed: commit the SSAO command buffer (empty if SSAO off)

			// Scene → RGBA16Float HDR target. Tile mode already CONTAINS the
			// resolved lighting (pass 1 wrote it) → load instead of clear.
			MTLRenderPassDescriptor* hdrPass = [MTLRenderPassDescriptor renderPassDescriptor];
			hdrPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_hdrColor;
			hdrPass.colorAttachments[0].loadAction  = deferredTile ? MTLLoadActionLoad : MTLLoadActionClear;
			hdrPass.colorAttachments[0].storeAction = MTLStoreActionStore;
			hdrPass.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
			hdrPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
			hdrPass.depthAttachment.loadAction  = deferredActive ? MTLLoadActionLoad : MTLLoadActionClear;
			hdrPass.depthAttachment.storeAction = MTLStoreActionDontCare;
			hdrPass.depthAttachment.clearDepth  = 1.0;

			// "Scene" pass = sky + clouds + opaque + skinned + particles + debug.
			// In an empty world this isolates the sky/cloud GPU cost from post-FX.
			// Authoritative per-encoder span; EncodeScene additionally places
			// draw-boundary points (when m_ft.draw) for the approximate per-element
			// breakdown — both write to the same sample buffer/encoder.
			// HW-VERIFY (unverified in sandbox): this is the one place a stage-boundary
			// timer and intra-encoder sampleCounters share an encoder+buffer. If the
			// Metal API validation layer (MTL_DEBUG_LAYER=1) ever rejects that combo at
			// capture start, the fix is: when m_ft.draw is active, DON'T attach this
			// "Scene" stage pair — instead derive the Scene total from the first/last
			// draw-boundary points in the completion handler (it becomes approx too).
			ftAttachPass((__bridge void*)hdrPass, "Scene");

			// Low-res clouds: raymarch the clouds into the quarter-res buffer BEFORE the
			// scene encoder (its own pass), rendered from THIS frame's camera (extracted in
			// the block below). The sky pass then upsamples + composites it 1:1.
			{
				const IRenderer::EnvironmentSettings& cenv = GetEnvironment();
				if (cenv.lowResClouds && cenv.cloudCoverage > 0.0f && m_cloudPipeline)
				{
					// Render the pre-pass with THIS frame's camera — like the GL backend. The
					// shadow/SSAO extracts above are skippable, so extract explicitly here to make
					// m_renderWorld.camera current; the quarter-res clouds then line up 1:1 with the
					// sky composited in EncodeScene (the sky's reprojection collapses to identity). No
					// previous-frame reprojection means the clouds no longer smear/tear at the screen
					// edges on a fast turn — the disoccluded edge band used to sample a clamped edge
					// texel. The extra extract runs only on the opt-in low-res-cloud path; EncodeScene
					// re-extracts identically below.
					m_extractor.setDayNight(cenv.dayNightCycle, cenv.timeOfDay,
					                        cenv.sunColor, cenv.sunIntensity,
					                        cenv.moonColor, cenv.moonIntensity, cenv.cloudCoverage);
					m_extractor.setContentManager(m_contentManager);
					m_extractor.extract(*m_world, m_renderWorld,
					                    static_cast<float>(sceneW) / static_cast<float>(std::max(1, sceneH)),
					                    &m_editorCamera);
					m_prepassViewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;
					EncodeCloudPrepass((__bridge void*)cmdBuf, glm::inverse(m_prepassViewProj),
						m_renderWorld.sunDirection,
						static_cast<float>(SDL_GetTicks()) / 1000.0f,
						std::max(1, sceneW / 2), std::max(1, sceneH / 2));
				}
				else if (m_cloudColor) DestroyCloudTarget(); // freed when toggled off
			}

			id<MTLRenderCommandEncoder> sceneEncoder =
				[cmdBuf renderCommandEncoderWithDescriptor:hdrPass];
			EncodeScene((__bridge void*)sceneEncoder, sceneW, sceneH,
			            deferredActive ? &deferredFrame : nullptr);
			// Debug lines on top of the opaque scene, still in the HDR pass.
			if (!m_debugLines.empty())
			{
				const glm::mat4 vp = m_renderWorld.camera.projection * m_renderWorld.camera.view;
				EncodeDebugLines((__bridge void*)sceneEncoder, vp);
				SamplePoint((__bridge void*)sceneEncoder, "Debug");   // closes the Debug interval
			}
			[sceneEncoder endEncoding];

			// Forward SSR: keep a full-res copy of this frame's HDR (incl. sky
			// and transparency) — next frame's trace reprojects into it.
			if (!deferredActive && m_ssrFrameActive)
			{
				if (!m_ssrColorHist || m_ssrColorHistW != sceneW || m_ssrColorHistH != sceneH)
				{
					if (m_ssrColorHist) { CFBridgingRelease(m_ssrColorHist); m_ssrColorHist = nullptr; }
					m_ssrColorHistValid = false;
					id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
					MTLTextureDescriptor* hd = [MTLTextureDescriptor
						texture2DDescriptorWithPixelFormat:kSceneColorFormat
						width:sceneW height:sceneH mipmapped:NO];
					hd.usage       = MTLTextureUsageShaderRead;
					hd.storageMode = MTLStorageModePrivate;
					m_ssrColorHist = (void*)CFBridgingRetain([device newTextureWithDescriptor:hd]);
					m_ssrColorHistW = sceneW; m_ssrColorHistH = sceneH;
				}
				if (m_ssrColorHist)
				{
					id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
					[blit copyFromTexture:(__bridge id<MTLTexture>)m_hdrColor
					          sourceSlice:0 sourceLevel:0
					         sourceOrigin:MTLOriginMake(0, 0, 0)
					           sourceSize:MTLSizeMake(sceneW, sceneH, 1)
					            toTexture:(__bridge id<MTLTexture>)m_ssrColorHist
					     destinationSlice:0 destinationLevel:0
					    destinationOrigin:MTLOriginMake(0, 0, 0)];
					[blit endEncoding];
					m_ssrColorHistValid = true;
				}
			}
			flushPass("Bloom");   // detailed: commit the Scene command buffer

			// Bright-pass + blur the HDR target into the half-res bloom buffer;
			// the tonemap below composites it back in. Skipped when bloom is
			// disabled (m_bloomResult stays null → no glow).
			m_bloomResult = m_bloomEnabled ? EncodeBloom((__bridge void*)cmdBuf, sceneW, sceneH)
			                               : nullptr;
			flushPass("Tonemap");   // detailed: commit the Bloom command buffer (empty if bloom off)

			// Camera lens flare: project the sun (a point at infinity, w=0 drops the view
			// translation) to NDC and fold behind-camera / off-screen / below-horizon into a
			// single strength scalar. The tonemap shader reads m_lensFlareParams; w<=0 = OFF.
			{
				const float lfAmt = GetEnvironment().lensFlare;
				const glm::mat4 vp = m_renderWorld.camera.projection * m_renderWorld.camera.view;
				glm::vec3 sd = glm::normalize(m_renderWorld.sunDirection);
				glm::vec4 clip = vp * glm::vec4(sd, 0.0f);
				glm::vec2 sunNDC(0.0f);
				float strength = 0.0f;
				if (lfAmt > 0.0f && clip.w > 1e-4f)
				{
					sunNDC = glm::vec2(clip) / clip.w;
					const float onScreen = 1.0f - glm::smoothstep(1.0f, 1.7f, glm::length(sunNDC));
					const float horizon  = glm::smoothstep(-0.02f, 0.10f, sd.y);
					strength = lfAmt * onScreen * horizon;
				}
				const float aspect = (sceneH > 0) ? (float)sceneW / (float)sceneH : 1.0f;
				m_lensFlareParams[0] = sunNDC.x; m_lensFlareParams[1] = sunNDC.y;
				m_lensFlareParams[2] = aspect;   m_lensFlareParams[3] = strength;
			}

			// MetalFX (A5): upscale the HDR image BEFORE the tonemap — that is the
			// stage Apple's model expects, and it means the tonemap (and every
			// filter after it) then works at output resolution. Returns null
			// unless MetalFX is the active mode and the device has it.
			void* const mfxHdr = EncodeMetalFX((__bridge void*)cmdBuf, sceneW, sceneH, outW, outH);
			// Everything downstream of the tonemap works at THIS size: the scene
			// resolution normally, the output resolution when MetalFX ran.
			const int postW = mfxHdr ? outW : sceneW;
			const int postH = mfxHdr ? outH : sceneH;

			// Tonemap HDR → LDR intermediate; FXAA reads it next (for both the editor
			// viewport and the direct-to-drawable path). m_hdrDepth is a DontCare
			// depth so the (depth-carrying) tonemap pipeline stays valid.
			EnsureLdrTarget(postW, postH);
			{
				MTLRenderPassDescriptor* tmPass = [MTLRenderPassDescriptor renderPassDescriptor];
				tmPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ldrColor;
				tmPass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				tmPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				tmPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
				tmPass.depthAttachment.loadAction  = MTLLoadActionDontCare;
				tmPass.depthAttachment.storeAction = MTLStoreActionDontCare;
				ftAttachPass((__bridge void*)tmPass, "Tonemap");
				id<MTLRenderCommandEncoder> tmEncoder =
					[cmdBuf renderCommandEncoderWithDescriptor:tmPass];
				EncodeTonemap((__bridge void*)tmEncoder, mfxHdr);
				[tmEncoder endEncoding];
			}

			// Temporal accumulation on the tonemapped image (A3). No-op unless
			// TAA is the active mode; the AA-resolve slot below then reads
			// m_taaResolved instead of m_ldrColor.
			EncodeTaa((__bridge void*)cmdBuf, postW, postH);

			// FXAA LDR → offscreen viewport texture (shown by the editor).
			if (offscreen)
			{
				EnsureViewportTarget();
				MTLRenderPassDescriptor* fxPass = [MTLRenderPassDescriptor renderPassDescriptor];
				fxPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_viewportColor;
				fxPass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				fxPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				fxPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_viewportDepth;
				fxPass.depthAttachment.loadAction  = MTLLoadActionDontCare;
				fxPass.depthAttachment.storeAction = MTLStoreActionDontCare;
				id<MTLRenderCommandEncoder> fxEncoder =
					[cmdBuf renderCommandEncoderWithDescriptor:fxPass];
				// Source resolution, not output: the spatial filters step in
				// TEXELS of the image they read.
				EncodeFxaa((__bridge void*)fxEncoder, postW, postH);
				// UI is laid out in OUTPUT pixels — it is drawn onto the viewport
				// texture after the rescale, so a render scale must not reach it
				// (or every widget would be placed as if the screen were smaller).
				EncodeUIPass((__bridge void*)fxEncoder, outW, outH);
				[fxEncoder endEncoding];
			}
			else if (m_viewportColor)
				DestroyViewportTarget();

			// detailed: commit the Tonemap (+ offscreen FXAA/UI) command buffer; the
			// Present command buffer (acquired below) is the last one.
			flushPass("Present");
		}

		// ── Swapchain pass (direct-mode tonemap and/or overlay) ─────────────
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (drawable)
		{
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture     = drawable.texture;
			pass.colorAttachments[0].loadAction  = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
			pass.depthAttachment.texture     = (__bridge id<MTLTexture>)target.depthTexture;
			pass.depthAttachment.loadAction  = MTLLoadActionClear;
			pass.depthAttachment.storeAction = MTLStoreActionDontCare;
			pass.depthAttachment.clearDepth  = 1.0;

			// "Present" pass = direct-mode FXAA + in-game UI + ImGui overlay.
			ftAttachPass((__bridge void*)pass, "Present");

			id<MTLRenderCommandEncoder> encoder = [cmdBuf renderCommandEncoderWithDescriptor:pass];

			// Direct-to-window (game/no editor viewport): FXAA the tonemapped LDR → drawable.
			if (isPrimary && !offscreen)
				EncodeFxaa((__bridge void*)encoder, pw, ph);

			// ── In-Game UI ──────────────────────────────────────────────────
			if (isPrimary && !offscreen)
				EncodeUIPass((__bridge void*)encoder, pw, ph);

			// ── Overlay (ImGui) ─────────────────────────────────────────────
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
		}

		// Publish GPU timing once the buffer completes (background thread). All
		// captured state is by-value / shared_ptr so it outlives the renderer if it
		// is destroyed with a frame still in flight (no use-after-free, no GPU drain).
		// GPUStartTime/GPUEndTime give true GPU execution time, immune to vsync.
		if (detailed)
		{
			// Final pass (Present): the earlier 5 command buffers were committed +
			// waited by flushPass(); commit + wait this one too so its timing is
			// exclusive and the frame is isolated from the next.
			attachDetail(cmdBuf, curPass);
			[cmdBuf commit];
			[cmdBuf waitUntilCompleted];
		}
		else if (isPrimary && sampleBuf && m_gpuTimer)
		{
			std::shared_ptr<MetalGpuTimerShared> shared = m_gpuTimer;
			id<MTLCounterSampleBuffer>           sb      = sampleBuf;
			std::vector<GpuTimedPair>            pairs   = m_ft.pairs;   // exact per-encoder spans
			std::vector<GpuTimedPoint>           points  = m_ft.points;  // approx intra-Scene splits
			const NSUInteger                     count   = m_ft.next;    // slots actually used
			[cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb)
			{
				IRenderer::FrameGpuStats fs;
				const double s0 = cb.GPUStartTime, s1 = cb.GPUEndTime;
				if (s1 > s0) fs.gpuFrameMs = (s1 - s0) * 1000.0;
				const double toMs = shared->gpuTicksToMs.load(std::memory_order_relaxed);
				if (toMs > 0.0 && count > 0)
				{
					if (@available(macOS 11.0, *))
					{
						NSData* data = [sb resolveCounterRange:NSMakeRange(0, count)];
						if (data && data.length >= count * sizeof(MTLCounterResultTimestamp))
						{
							const MTLCounterResultTimestamp* ts =
								(const MTLCounterResultTimestamp*)data.bytes;
							auto interval = [&](uint32_t a, uint32_t b, const char* name, bool approx)
							{
								const uint64_t t0 = ts[a].timestamp, t1 = ts[b].timestamp;
								if (t1 <= t0) return;
								const double ms = (double)(t1 - t0) * toMs;
								if (ms >= 0.0 && ms < 1000.0)  // reject absurd/garbage slots
									fs.passes.push_back({ name, ms, approx });
							};
							// Stage-boundary pairs: exact per-encoder GPU spans.
							for (const GpuTimedPair& p : pairs)
								interval(p.base, p.base + 1, p.name, /*approx=*/false);
							// Draw-boundary points: element[i] = sample[i] - sample[i-1].
							// The first point is the anchor (no interval). Marked approx —
							// tile-deferred fragment work makes these estimates, not exact.
							for (size_t i = 1; i < points.size(); ++i)
								interval(points[i - 1].slot, points[i].slot, points[i].name, /*approx=*/true);
						}
					}
				}
				fs.gpuTimingMode = "counter";   // stamp the path that produced these passes
				std::lock_guard<std::mutex> lk(shared->mutex);
				shared->last = fs;
			}];
		}
		else if (isPrimary && m_gpuTimer)
		{
			// Timer exists but not sampling this frame (idle, or alloc failed):
			// still publish whole-frame time so GetFrameGpuStats isn't stale.
			std::shared_ptr<MetalGpuTimerShared> shared = m_gpuTimer;
			[cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb)
			{
				const double s0 = cb.GPUStartTime, s1 = cb.GPUEndTime;
				if (s1 > s0)
				{
					IRenderer::FrameGpuStats fs;
					fs.gpuFrameMs = (s1 - s0) * 1000.0;
					fs.gpuTimingMode = "whole-frame";
					std::lock_guard<std::mutex> lk(shared->mutex);
					shared->last = fs;
				}
			}];
		}
		else if (isPrimary)
		{
			// No per-pass timer at all → whole-frame atomic fallback.
			std::shared_ptr<std::atomic<double>> sink = m_gpuFrameMs;
			[cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> cb)
			{
				const double s0 = cb.GPUStartTime, s1 = cb.GPUEndTime;
				if (s1 > s0) sink->store((s1 - s0) * 1000.0, std::memory_order_relaxed);
			}];
		}

		if (!detailed) [cmdBuf commit];   // detailed committed + waited each pass above
	}
}

// Probe once. The shared timer is ALWAYS created — it carries whole-frame GPU
// time and the detailed-capture (one-cmdbuf-per-pass) accumulator, neither of
// which needs counter sampling. Stage-boundary counter sampling is an optional
// extra (the non-detailed per-encoder path); draw-boundary is a further option.
void MetalRenderer::EnsureGpuTimer()
{
	if (m_gpuTimerChecked) return;
	m_gpuTimerChecked = true;

	m_gpuTimer = std::make_shared<MetalGpuTimerShared>();   // always available

	if (@available(macOS 11.0, *))
	{
		id<MTLDevice> dev = (__bridge id<MTLDevice>)m_device;
		id<MTLCounterSet> tsSet = nil;
		if (dev && [dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
			for (id<MTLCounterSet> cs in dev.counterSets)
				if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) { tsSet = cs; break; }
		if (tsSet)
		{
			m_timestampCounterSet = (void*)CFBridgingRetain(tsSet);
			m_counterSamplingOk   = true;
			// Draw-boundary sampling (intra-encoder) is a separate capability; when
			// present it would let the Scene encoder be split per-element.
			m_drawBoundary = [dev supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary];
			HE_LOG_INFO(RHI, "%s",
				m_drawBoundary
					? "Metal: GPU timing — whole-frame + detailed-capture; counter sampling stage + draw-boundary"
					: "Metal: GPU timing — whole-frame + detailed-capture; counter sampling stage-boundary only");
		}
		else
		{
			HE_LOG_INFO(RHI, "%s",
				"Metal: GPU timing — whole-frame + detailed-capture available; stage-boundary counter sampling unsupported "
				"(NB: per-encoder counter spans overlap on TBDR anyway — use the detailed-capture toggle for reliable per-pass)");
		}
	}
	else
	{
		HE_LOG_INFO(RHI, "%s",
			"Metal: GPU timing — whole-frame + detailed-capture (counter sampling needs macOS 11+)");
	}
}

IRenderer::FrameGpuStats MetalRenderer::GetFrameGpuStats() const
{
	FrameGpuStats s;
	if (m_gpuTimer)
	{
		std::lock_guard<std::mutex> lk(m_gpuTimer->mutex);
		s = m_gpuTimer->last;   // GPU times (per-pass + whole-frame), 1–2 frames late
	}
	else
	{
		s.gpuFrameMs = m_gpuFrameMs->load(std::memory_order_relaxed);
	}
	// Current-frame CPU counters (filled by this frame's EncodeScene, main thread).
	s.drawCalls      = m_counters.draws;
	s.triangles      = m_counters.tris;
	s.visibleObjects = m_counters.visible;
	s.totalObjects   = m_counters.total;
	return s;
}

void MetalRenderer::Render()
{
	if (!m_primarySdlWindow || !m_primaryTarget.metalLayer) return;
	EncodeFrame(m_primarySdlWindow, m_primaryTarget, /*isPrimary=*/true);
}

// ─── GPU weather particles (compute simulation + vertex-pull billboards) ──────
// Metal has no transform feedback, so the pool is integrated + recycled by a
// compute kernel in place (one MTLBuffer, particle = float4(pos,life)+float4(vel,
// seed)). The draw stage pulls the buffer per-instance and expands an attribute-
// less triangle-strip into camera-facing billboards. Math mirrors the GL path.
namespace {
constexpr int kParticleMax = 1000000;

// setBytes layouts — all-float4 so MSL float4 alignment matches byte-for-byte.
struct PSimParams { glm::vec4 a, b, c, camPos, wind; };
//   a = (dt, time, coverage, fallSpeed)   b = (lifeSpan, groundLevel, boxHalf, boxTop)
//   c = (isSnow, count, 0, 0)             camPos.xyz, wind.xyz
struct PDrawParams { glm::mat4 viewProj; glm::vec4 camPos; glm::vec4 snow; };

const char* kParticleMSL = R"(#include <metal_stdlib>
using namespace metal;
struct Particle { float4 p0; float4 p1; };   // p0=(pos,life)  p1=(vel,seed)
struct PSim  { float4 a, b, c, camPos, wind; };
struct PDraw { float4x4 viewProj; float4 camPos; float4 snow; };

static float h21(float2 p){ float3 p3=fract(float3(p.x,p.y,p.x)*0.1031);
    p3+=dot(p3,float3(p3.y,p3.z,p3.x)+33.33); return fract((p3.x+p3.y)*p3.z); }

kernel void particleSim(device Particle* parts [[buffer(0)]],
                        constant PSim&   u     [[buffer(1)]],
                        uint id [[thread_position_in_grid]])
{
    if (float(id) >= u.c.y) return;                         // c.y = count
    float dt=u.a.x, time=u.a.y, coverage=u.a.z, fallSpeed=u.a.w;
    float lifeSpan=u.b.x, groundLevel=u.b.y, boxHalf=u.b.z, boxTop=u.b.w;
    float isSnow=u.c.x;
    float3 camPos=u.camPos.xyz, wind=u.wind.xyz;
    Particle pt = parts[id];
    float3 pos=pt.p0.xyz; float life=pt.p0.w;
    float3 vel=pt.p1.xyz; float seed=pt.p1.w;
    float alive = step(seed, coverage);
    pos += vel * dt;
    life -= dt;
    if (isSnow > 0.5) pos.x += sin((lifeSpan - life) * 2.2 + seed * 6.2831) * 0.5 * dt;
    bool dead = life <= 0.0 || pos.y <= groundLevel;
    if (dead) {
        if (alive > 0.5) {
            float ep = floor(time * 7.0) + seed * 131.0;
            float rx = h21(float2(seed * 91.7, ep)) * 2.0 - 1.0;
            float rz = h21(float2(ep, seed * 57.3)) * 2.0 - 1.0;
            pos = float3(camPos.x + rx * boxHalf, camPos.y + boxTop, camPos.z + rz * boxHalf);
            vel = float3(0.0, -fallSpeed, 0.0);
            if (isSnow > 0.5) {
                vel.x += (h21(float2(ep, seed)) * 2.0 - 1.0) * 0.6 + wind.x * 0.3;
                vel.z += (h21(float2(seed, ep)) * 2.0 - 1.0) * 0.6 + wind.z * 0.3;
            } else {
                vel.x += wind.x * 1.2;
                vel.z += wind.z * 1.2;
            }
            life = lifeSpan * (0.6 + 0.4 * seed);
        } else {
            life = -1.0; pos = camPos + float3(0.0, -100000.0, 0.0); vel = float3(0.0);
        }
    }
    parts[id].p0 = float4(pos, life);
    parts[id].p1 = float4(vel, seed);
}

struct VOut { float4 pos [[position]]; float2 uv; float snow; };
vertex VOut particleVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                           device const Particle* parts [[buffer(0)]],
                           constant PDraw& u [[buffer(1)]])
{
    Particle pt = parts[iid];
    float life = pt.p0.w;
    VOut o; o.snow = u.snow.x;
    if (life <= 0.0) { o.pos = float4(2.0, 2.0, 2.0, 1.0); o.uv = float2(0.0); return o; }
    float2 c = float2(float(vid & 1u), float((vid >> 1u) & 1u)) - 0.5;
    o.uv = c;
    float3 ppos = pt.p0.xyz, vel = pt.p1.xyz;
    float3 look = u.camPos.xyz - ppos;
    float d = length(look);
    look = (d > 1e-4) ? look / d : float3(0.0, 0.0, 1.0);
    float3 worldPos;
    if (u.snow.x > 0.5) {
        float s = 0.16;
        float3 right = normalize(cross(float3(0.0, 1.0, 0.0), look));
        float3 up    = cross(look, right);
        worldPos = ppos + (right * c.x + up * c.y) * s;
    } else {
        float3 vdir = vel; float vl = length(vdir);
        vdir = (vl > 1e-4) ? vdir / vl : float3(0.0, -1.0, 0.0);
        float3 up = vdir - look * dot(vdir, look);
        up = (length(up) > 1e-4) ? normalize(up) : float3(0.0, 1.0, 0.0);
        float3 right = normalize(cross(up, look));
        worldPos = ppos + right * (c.x * 0.02) + up * (c.y * 0.6);
    }
    o.pos = u.viewProj * float4(worldPos, 1.0);
    return o;
}
fragment float4 particleFragment(VOut in [[stage_in]])
{
    if (in.snow > 0.5) {
        float a = (1.0 - smoothstep(0.15, 0.5, length(in.uv))) * 0.9;
        return float4(0.92, 0.95, 1.0, a);
    }
    float a = (1.0 - smoothstep(0.0, 0.5, abs(in.uv.x))) * 0.45;
    return float4(0.55, 0.62, 0.78, a);
}
)";
} // namespace

void MetalRenderer::CreateParticlePipeline()
{
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		NSError* error = nil;
		id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kParticleMSL]
		                                          options:nil error:&error];
		if (!lib)
			throw std::runtime_error(std::string("MetalRenderer: particle shader compile failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));

		id<MTLComputePipelineState> simPso =
			[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"particleSim"] error:&error];
		if (!simPso)
			throw std::runtime_error(std::string("MetalRenderer: particle sim pipeline failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_particleSimPipeline = (void*)CFBridgingRetain(simPso);

		MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
		desc.vertexFunction   = [lib newFunctionWithName:@"particleVertex"];
		desc.fragmentFunction = [lib newFunctionWithName:@"particleFragment"];
		desc.colorAttachments[0].pixelFormat = kSceneColorFormat;
		desc.depthAttachmentPixelFormat      = kDepthFormat;
		desc.colorAttachments[0].blendingEnabled             = YES;
		desc.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
		desc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
		desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
		desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
		desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
		desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		id<MTLRenderPipelineState> drawPso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
		if (!drawPso)
			throw std::runtime_error(std::string("MetalRenderer: particle draw pipeline failed: ")
				+ (error ? [[error localizedDescription] UTF8String] : "unknown"));
		m_particleDrawPipeline = (void*)CFBridgingRetain(drawPso);
	}
}

void MetalRenderer::EnsureParticleBuffer(int count)
{
	if (count == m_particleCapacity && m_particleBuffer) return;
	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
		if (m_particleBuffer) { CFBridgingRelease(m_particleBuffer); m_particleBuffer = nullptr; }
		// Record width comes from the shared seeder so the two cannot drift apart.
		id<MTLBuffer> buf = [device newBufferWithLength:(NSUInteger)count * HE::kWeatherParticleFloats * sizeof(float)
		                                       options:MTLResourceStorageModeShared];
		m_particleBuffer = (void*)CFBridgingRetain(buf);
	}
	m_particleCapacity = count;
	m_particleSeeded   = false;
}

void MetalRenderer::SeedParticleBuffer(int count)
{
	// Shared with OpenGL (HE::SeedWeatherParticles) — the RNG, the (i+0.5)/count
	// spread and the wind/life maths are a cross-backend contract, only the lane
	// order differs. This backend's compute kernel reads two float4s:
	// p0 = pos.xyz + life, p1 = vel.xyz + seed.
	float* d = (float*)[(__bridge id<MTLBuffer>)m_particleBuffer contents];
	HE::SeedWeatherParticles(m_gpuParticleParams, count,
	                         HE::WeatherParticleLayout::PosLifeVelSeed, d);
	m_particleSeeded = true;
}

void MetalRenderer::SimulateGpuParticles(void* cmdBuf)
{
	const GpuParticleParams& p = m_gpuParticleParams;
	if (!p.enabled || !m_particleSimPipeline) return;
	const int count = std::clamp(p.count, 0, kParticleMax);
	if (count <= 0) return;
	EnsureParticleBuffer(count);
	if (!m_particleSeeded) SeedParticleBuffer(count);

	@autoreleasepool
	{
		PSimParams u;
		u.a      = glm::vec4(p.dt, p.time, p.coverage, p.fallSpeed);
		u.b      = glm::vec4(p.lifeSpan, p.groundLevel, p.boxHalf, p.boxTop);
		u.c      = glm::vec4(p.isSnow ? 1.0f : 0.0f, static_cast<float>(count), 0.0f, 0.0f);
		u.camPos = glm::vec4(p.cameraPos, 0.0f);
		u.wind   = glm::vec4(p.windVec, 0.0f);

		id<MTLComputeCommandEncoder> ce = [(__bridge id<MTLCommandBuffer>)cmdBuf computeCommandEncoder];
		[ce setComputePipelineState:(__bridge id<MTLComputePipelineState>)m_particleSimPipeline];
		[ce setBuffer:(__bridge id<MTLBuffer>)m_particleBuffer offset:0 atIndex:0];
		[ce setBytes:&u length:sizeof(u) atIndex:1];
		const NSUInteger tg     = 64;
		const NSUInteger groups = ((NSUInteger)count + tg - 1) / tg;
		[ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
		[ce endEncoding];
	}
}

void MetalRenderer::DrawGpuParticles(void* renderEncoder, const glm::mat4& viewProj, const glm::vec3& camPos)
{
	const GpuParticleParams& p = m_gpuParticleParams;
	if (!p.enabled || !m_particleDrawPipeline || !m_particleBuffer) return;
	const int count = std::clamp(p.count, 0, m_particleCapacity);
	if (count <= 0) return;

	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	PDrawParams u;
	u.viewProj = viewProj;
	u.camPos   = glm::vec4(camPos, 0.0f);
	u.snow     = glm::vec4(p.isSnow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_particleDrawPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState]; // LessEqual, no write
	[enc setVertexBuffer:(__bridge id<MTLBuffer>)m_particleBuffer offset:0 atIndex:0];
	[enc setVertexBytes:&u length:sizeof(u) atIndex:1];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:(NSUInteger)count];
}

void MetalRenderer::DrawParticleGraphBatches(void* renderEncoder, const glm::mat4& viewProj, const glm::mat4& view)
{
	if (m_renderWorld.particleBatches.empty()) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
	id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;

	// Camera-facing basis for billboard expansion — same convention as RenderParticlePreview.
	const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
	const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState]; // LessEqual, no write

	for (const ParticleBatch& batch : m_renderWorld.particleBatches)
	{
		if (batch.instances.empty()) continue;

		// A precompiled Metal variant (export-baked, CHUNK_PPSD) wins over an
		// on-demand-compiled + hash-cached one — see GetOrBuildParticlePipeline.
		const ParticleShaderVariant* precompiled = nullptr;
		if (const ParticleGraphAsset* asset = m_contentManager ? m_contentManager->getParticleGraph(batch.particleAssetId) : nullptr)
			for (const auto& var : asset->precompiledShaders)
				if (var.backend == static_cast<uint8_t>(HE::RendererBackend::Metal)) { precompiled = &var; break; }

		const uint64_t key = precompiled
			? (0x50505344ull /*"PPSD"*/ ^ (static_cast<uint64_t>(batch.particleAssetId.hi) * 0x9E3779B97F4A7C15ULL) ^ batch.particleAssetId.lo)
			: HE::hashParticleShaderConfig(batch.config);
		id<MTLRenderPipelineState> pso = (__bridge id<MTLRenderPipelineState>)GetOrBuildParticlePipeline(key, batch.config, precompiled);
		if (!pso) continue;

		void* matTex = nullptr;
		const bool hasTex = ResolveMaterialTexture(batch.materialAssetId, matTex);

		std::vector<float> inst;
		inst.reserve(batch.instances.size() * 5);
		for (const ParticleInstance& p : batch.instances)
			inst.insert(inst.end(), { p.position.x, p.position.y, p.position.z, p.size, p.t01 });
		id<MTLBuffer> instBuf = [device newBufferWithBytes:inst.data()
			length:inst.size() * sizeof(float) options:MTLResourceStorageModeShared];

		glm::mat4 vp = viewProj;
		[enc setRenderPipelineState:pso];
		[enc setVertexBuffer:instBuf offset:0 atIndex:0];
		[enc setVertexBytes:&vp       length:sizeof(vp)       atIndex:1];
		[enc setVertexBytes:&camRight length:sizeof(camRight) atIndex:2];
		[enc setVertexBytes:&camUp    length:sizeof(camUp)    atIndex:3];
		bool hasTexFlag = hasTex;
		[enc setFragmentBytes:&hasTexFlag length:sizeof(hasTexFlag) atIndex:0];
		[enc setFragmentTexture:(__bridge id<MTLTexture>)(hasTex ? matTex : m_dummyTexture) atIndex:0];
		[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6
		      instanceCount:(NSUInteger)batch.instances.size()];
		++m_counters.draws;
		m_counters.tris += static_cast<uint32_t>(batch.instances.size()) * 2;
	}
}

IRenderer::Capabilities MetalRenderer::GetCapabilities() const
{
	Capabilities c;
	c.supportsShadows            = true;
	c.supportsPostProcessing     = true;
	c.supportsHDR                = true;
	c.supportsGpuParticles       = true;
	// Cached at Initialize() by EnsureRaytracingSupport(): true only on devices +
	// OS versions that actually support Metal ray tracing.
	c.supportsGlobalIllumination = m_giSupported;
#if defined(HE_HAVE_SHADERC)
	// The deferred resolve shader is generated from the shared lighting preamble
	// at runtime — no cross-compiler, no deferred path.
	c.supportsDeferredRendering  = true;
	// SSR: deferred tile mode traces the stored G-buffer lag-free; every other
	// Metal configuration (forward, or non-Apple GPUs' two-pass deferred falls
	// back to forward SSR too once the path is forward) runs the Option-A
	// forward variant off the MRT pre-pass + last frame's HDR copy.
	c.supportsScreenSpaceReflections = true;
	// Ray-traced GI reflections: tile-deferred AND forward (same kernels, fed
	// from the pre-pass); the SW-BVH kernel covers devices without HW RT.
	c.supportsGIReflections = true;
	// TAA needs a velocity buffer, and the velocity pass depth-tests against the
	// G-buffer's depth — so it exists in the DEFERRED path only (same shape as
	// the SSR gate above). Reported as a capability rather than silently falling
	// back, so the editor can grey the entry out and say why.
	c.supportsTemporalAA = (m_renderPath == HE::RenderPath::Deferred);
	// MetalFX rides on the same velocity buffer, so it inherits the same gate on
	// top of the device/OS check made at Initialize.
	c.supportsMetalFX    = m_mfxSupported && c.supportsTemporalAA;
#endif
	return c;
}

void MetalRenderer::SetGpuParticleParams(const GpuParticleParams& p)
{
	m_gpuParticleParams = p;
}

void MetalRenderer::SetSSRSettings(const SSRSettings& s)
{
	m_ssrEnabled      = s.enabled;
	m_ssrIntensity    = s.intensity;
	m_ssrMaxRoughness = s.maxRoughness;
	m_ssrMaxDistance  = s.maxDistance;
	m_ssrThickness    = s.thickness;
	m_ssrQuality      = s.quality;
}

void MetalRenderer::SetGIReflectionSettings(const GIReflectionSettings& s)
{
	m_giReflEnabled      = s.enabled && m_giSupported;
	m_giReflIntensity    = s.intensity;
	m_giReflMaxRoughness = s.maxRoughness;
	m_giReflMaxDistance  = s.maxDistance;
	m_giReflBlurEnabled  = s.blur;
	m_giReflQuality      = s.quality;
	m_giReflBounces      = std::clamp(s.bounces, 1, 4);
}

void MetalRenderer::SetGISettings(const GISettings& s)
{
	m_giEnabled             = s.enabled && m_giSupported;
	m_giIndirectIntensity   = s.indirectIntensity;
	m_giLightRadius         = s.lightRadius;
	m_giRaysPerProbe        = s.raysPerProbe;
	m_giProbeBudgetPerFrame = s.probeBudgetPerFrame;
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
	HE_LOG_INFO(RHI, "%s", "MetalRenderer: secondary window attached");
}

void MetalRenderer::DetachWindow(HE::Window* window)
{
	auto it = m_secondaryTargets.find(window->GetNativeWindow());
	if (it == m_secondaryTargets.end()) return;
	DestroyTarget(it->second);
	m_secondaryTargets.erase(it);
	HE_LOG_INFO(RHI, "%s", "MetalRenderer: secondary window detached");
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

void MetalRenderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
	if (!m_device || !rgba8Pixels || width <= 0 || height <= 0) return;

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
		if (!texture) return;

		[texture replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
		           mipmapLevel:0
		             withBytes:rgba8Pixels
		           bytesPerRow:(NSUInteger)width * 4];

		if (m_moonTexture) CFBridgingRelease(m_moonTexture);
		m_moonTexture = (void*)CFBridgingRetain(texture);
	}
}

// ─── Accessors ────────────────────────────────────────────────────────────────

void* MetalRenderer::GetDevice() const              { return m_device; }
void* MetalRenderer::GetCommandQueue() const        { return m_commandQueue; }
void* MetalRenderer::GetFramePassDescriptor() const { return m_imguiPassDescriptor; }
