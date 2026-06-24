#include "Backends/Metal/MetalRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <glm/glm.hpp>
#include <simd/simd.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cstdint>

// Builds a tiling NxNxN value-noise volume whose lattice values are exactly the
// sky shader's starHash(i,j,k); the shader pre-smoothsteps the sample coordinate
// so hardware trilinear filtering reproduces the old smoothstep value noise. The G
// channel is a tiling inverted-Worley field whose fBm is the billowy cumulus cloud
// shape. Identical to the OpenGL backend's BuildSkyNoise3D (interleaved RG16).
static std::vector<uint16_t> BuildSkyNoise3D(int n)
{
	auto hash = [](glm::vec3 p) {
		p = glm::fract(p * 0.1031f);
		p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + 31.32f);
		return glm::fract((p.x + p.y) * p.z);
	};
	// Decorrelated per-cell jitter for the Worley feature points (sin-free so it is
	// bit-deterministic across compilers — both backends bake CPU-side).
	auto hash3 = [](glm::vec3 c) {
		glm::vec3 p = glm::fract(c * glm::vec3(0.1031f, 0.1030f, 0.0973f));
		p += glm::dot(p, glm::vec3(p.y, p.z, p.x) + 33.33f);
		return glm::fract(glm::vec3((p.x + p.y) * p.z, (p.x + p.z) * p.y, (p.y + p.z) * p.x));
	};
	const int kWorleyGrid = 48;   // feature cells per axis across the tile
	auto worley = [&](glm::vec3 uv) {
		glm::vec3 pc = uv * static_cast<float>(kWorleyGrid);
		glm::vec3 id = glm::floor(pc);
		glm::vec3 fp = pc - id;
		float f1 = 1e9f;
		for (int k = -1; k <= 1; ++k)
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i)
				{
					glm::vec3 off(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k));
					glm::vec3 wrapped = glm::mod(id + off, static_cast<float>(kWorleyGrid)); // seamless tile
					glm::vec3 d = (off + hash3(wrapped)) - fp;
					f1 = std::min(f1, glm::dot(d, d));   // nearest feature (squared)
				}
		return glm::clamp(1.0f - std::sqrt(f1), 0.0f, 1.0f);
	};
	std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);
	const float inv = 1.0f / static_cast<float>(n);
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				size_t idx = ((static_cast<size_t>(z) * n + y) * n + x) * 2;
				glm::vec3 uv((x + 0.5f) * inv, (y + 0.5f) * inv, (z + 0.5f) * inv);
				d[idx + 0] = static_cast<uint16_t>(
					glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
				d[idx + 1] = static_cast<uint16_t>(worley(uv) * 65535.0f + 0.5f);
			}
	return d;
}

// CPU port of the shader skyColor(dir,sunDir) — bakes the image-based-ambient
// cubemap so fragmentMain samples it once instead of evaluating skyColor twice
// per lit pixel. Mirrors kSkyFuncMSL / the GL SkyColorCPU exactly.
static glm::vec3 SkyColorCPU(glm::vec3 dir, glm::vec3 sunDir)
{
	dir = glm::normalize(dir); sunDir = glm::normalize(sunDir);
	float sunY = glm::clamp(sunDir.y, -0.2f, 1.0f);
	float day  = glm::smoothstep(-0.10f, 0.10f, sunY);
	float dusk = glm::smoothstep(-0.06f, 0.05f, sunY) * (1.0f - glm::smoothstep(0.05f, 0.28f, sunY));
	glm::vec3 zenith  = glm::mix(glm::vec3(0.012f,0.016f,0.05f), glm::vec3(0.08f,0.28f,0.72f), day);
	glm::vec3 horizon = glm::mix(glm::vec3(0.03f,0.04f,0.10f),   glm::vec3(0.42f,0.62f,0.88f), day);
	glm::vec2 sunAz = glm::normalize(glm::vec2(sunDir.x, sunDir.z) + glm::vec2(1e-5f));
	float toward = glm::dot(glm::normalize(glm::vec2(dir.x, dir.z) + glm::vec2(1e-5f)), sunAz) * 0.5f + 0.5f;
	toward = std::pow(glm::clamp(toward, 0.0f, 1.0f), 1.5f);
	glm::vec3 duskHoriz = glm::mix(glm::vec3(0.52f,0.30f,0.52f), glm::vec3(1.20f,0.50f,0.16f), toward);
	horizon = glm::mix(horizon, duskHoriz, dusk);
	zenith  = glm::mix(zenith, glm::vec3(0.20f,0.16f,0.40f), dusk * 0.6f);
	float h = glm::clamp(dir.y, 0.0f, 1.0f);
	glm::vec3 sky = glm::mix(zenith, horizon, std::pow(1.0f - h, 2.5f));
	sky += glm::vec3(1.25f,0.62f,0.26f) * (std::pow(1.0f - h, 8.0f) * toward * dusk * 0.8f);
	glm::vec3 ground = glm::mix(glm::vec3(0.02f,0.02f,0.03f), glm::vec3(0.24f,0.23f,0.21f), day);
	sky = glm::mix(sky, ground, glm::smoothstep(0.0f, -0.25f, dir.y));
	glm::vec3 sunTint = glm::mix(glm::vec3(1.0f,0.42f,0.20f), glm::vec3(1.0f,0.96f,0.88f), glm::smoothstep(0.0f,0.25f,sunY));
	float s = std::max(glm::dot(dir, sunDir), 0.0f);
	float sunVis = std::max(day, dusk);
	sky += sunTint * (std::pow(s,1800.0f) * 14.0f * day);
	sky += sunTint * (std::pow(s,180.0f)  * 2.2f * sunVis);
	sky += sunTint * (std::pow(s,22.0f)   * 0.7f * sunVis);
	sky += glm::vec3(1.0f,0.5f,0.25f) * (std::pow(s,5.0f) * 0.5f * dusk);
	float night = 1.0f - day;
	glm::vec3 moonDir = glm::normalize(glm::vec3(-sunDir.x, -sunDir.y, sunDir.z));
	float mdot = std::max(glm::dot(dir, moonDir), 0.0f);
	sky += glm::vec3(0.80f,0.86f,1.00f) * (std::pow(mdot,60.0f) * 0.05f * night);
	sky += glm::vec3(0.04f,0.05f,0.08f) * night;
	return sky;
}

// One cube face (slice order +X,-X,+Y,-Y,+Z,-Z) of the IBL env map as tightly
// packed RGBA32F. Metal cube maps use the same face/texel convention as GL, so
// no axis flip — verified lossless (max 1/255 vs per-pixel skyColor).
static std::vector<float> BuildSkyEnvFace(int faceN, int f, const glm::vec3& sunDir)
{
	std::vector<float> px(static_cast<size_t>(faceN) * faceN * 4);
	for (int t = 0; t < faceN; ++t)
		for (int s = 0; s < faceN; ++s)
		{
			float u = (s + 0.5f) / faceN * 2.0f - 1.0f;
			float v = (t + 0.5f) / faceN * 2.0f - 1.0f;
			glm::vec3 d;
			switch (f) {
				case 0: d = glm::vec3( 1.0f, -v, -u); break; // +X
				case 1: d = glm::vec3(-1.0f, -v,  u); break; // -X
				case 2: d = glm::vec3( u,  1.0f,  v); break; // +Y
				case 3: d = glm::vec3( u, -1.0f, -v); break; // -Y
				case 4: d = glm::vec3( u, -v,  1.0f); break; // +Z
				default:d = glm::vec3(-u, -v, -1.0f); break; // -Z
			}
			glm::vec3 c = SkyColorCPU(glm::normalize(d), sunDir);
			size_t i = (static_cast<size_t>(t) * faceN + s) * 4;
			px[i+0] = c.r; px[i+1] = c.g; px[i+2] = c.b; px[i+3] = 1.0f;
		}
	return px;
}

// Swapchain / depth formats shared by every window target, the scene
// pipeline and the ImGui pass descriptor — they must all match.
static constexpr MTLPixelFormat kSwapchainFormat = MTLPixelFormatBGRA8Unorm;
static constexpr MTLPixelFormat kDepthFormat     = MTLPixelFormatDepth32Float;
static constexpr MTLPixelFormat kSceneColorFormat = MTLPixelFormatRGBA16Float; // HDR scene color

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
	float4   flags;   // x: hasTexture
	float4   pbr;     // x: metallic, y: roughness
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
	float4   sunDir;         // xyz = direction toward the sun (image-based ambient)
	float4   ambient;        // xyz = flat ambient fill (floor + overcast); w unused
	float4   fog;            // x = density (0 = off), y = height falloff
	float4   viewport;       // xy = output size (screen-space AO lookup), z = ssaoEnabled
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
    return out;
}

float shadowFactor(constant SceneUniforms& scene, float3 worldPos, float3 N, float3 L,
                   texture2d<float> shadowMap, sampler shadowSmp)
{
	if (scene.shadowEnabled == 0) return 1.0;
	// Normal-offset bias: push the sample point off the surface along its normal
	// before projecting into shadow space. This eliminates acne on steep terrain
	// (slope-scaled depth bias alone doesn't handle large dz/dx well).
	float4 lp = scene.lightVP * float4(worldPos + N * 0.06, 1.0);
	float3 p  = lp.xyz / lp.w;            // z already [0,1] (Metal clip); xy in [-1,1]
	float2 uv = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5)); // tex origin top-left
	if (p.z > 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;
	// Small residual depth bias for sub-texel precision.
	float ndl  = clamp(dot(N, L), 0.0, 1.0);
	float bias = clamp(0.0008 * tan(acos(ndl)), 0.0002, 0.02);
	// 3×3 PCF: averaging neighbouring texels softens the edge and hides the
	// per-texel flicker the hard test produced as the day-night light rotates.
	float2 texel = 1.0 / float2(shadowMap.get_width(), shadowMap.get_height());
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float c = shadowMap.sample(shadowSmp, uv + float2(x, y) * texel).r;
			vis += (p.z - bias > c) ? 0.0 : 1.0;
		}
	vis /= 9.0;
	// No direct-light floor in shadow — the IBL + flat ambient already provide
	// the minimum indirect illumination. A non-zero floor bleeds warm sun colour
	// into fully-shadowed areas and causes the yellow/orange cast at dusk.
	return vis;
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

// Blinn-Phong over up to 8 scene lights; lightCount == 0 falls back to the
// fixed "headlight" so unlit scenes don't render black.
fragment float4 fragmentMain(VSOut in [[stage_in]],
                             constant SceneUniforms& scene [[buffer(0)]],
                             texture2d<float> baseColor [[texture(0)]],
                             sampler          smp       [[sampler(0)]],
                             texture2d<float> shadowMap [[texture(1)]],
                             sampler          shadowSmp [[sampler(1)]],
                             texturecube<float> skyEnv  [[texture(2)]],
                             sampler          skyEnvSmp [[sampler(2)]],
                             texture2d<float> aoTex     [[texture(3)]],
                             sampler          aoSmp     [[sampler(3)]])
{
	float3 albedo = (in.hasTexture > 0.5)
		? baseColor.sample(smp, float2(in.uv.x, 1.0 - in.uv.y)).rgb * in.color
		: in.color;
	float3 N = normalize(in.normal);

	if (scene.lightCount == 0)
	{
		float3 L    = normalize(float3(0.5, 0.8, 0.6));
		float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		return float4(albedo * diff, in.opacity);
	}

	// Metallic-roughness split (matches the GL backend).
	float3 diffuseColor = albedo * (1.0 - in.metallic);
	float3 specColor    = mix(float3(0.04), albedo, in.metallic);
	float  shininess    = mix(128.0, 8.0, in.roughness);
	float  specScale    = mix(0.5, 0.03, in.roughness);

	float3 V = normalize(scene.cameraPos.xyz - in.worldPos);

	// Image-based ambient from the procedural sky (matches the GL backend):
	// diffuse from the normal, specular from the reflection (bent toward N by
	// roughness as a crude prefilter).
	float3 Rrough  = normalize(mix(reflect(-V, N), N, in.roughness));
	// Clamp the diffuse IBL lookup at least 5° above the horizon. Sampling near
	// or at the horizon (N.y ≈ 0) returns the warm/orange sunset band of the sky
	// even at noon. A floor of 0.1 keeps the sample safely in the cool sky dome.
	float3 Nup     = normalize(float3(N.x, max(N.y, 0.1), N.z));
	float3 ambDiff = skyEnv.sample(skyEnvSmp, Nup).rgb    * diffuseColor;
	float3 ambSpec = skyEnv.sample(skyEnvSmp, Rrough).rgb * specColor;
	float3 ambient = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * in.roughness);
	// Screen-space ambient occlusion darkens only the IBL indirect term in
	// crevices; the direct lighting added below is left untouched. 1.0 = fully lit.
	float ao = (scene.viewport.z > 0.5)
		? aoTex.sample(aoSmp, in.position.xy / scene.viewport.xy).r : 1.0;
	// Flat ambient fill (never-black floor + overcast replacement) kept outside AO
	// so grazing-angle SSAO over-darkening cannot zero it out.
	float3 result  = ambient * ao + scene.ambient.xyz * diffuseColor;

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
		float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;
		result += (diffuseColor * diff + specColor * spec)
		        * l.colorIntensity.rgb * l.colorIntensity.w * atten * sh;
	}
	result = applyFog(result, scene.cameraPos.xyz, in.worldPos, scene.sunDir.xyz, scene.fog.xy);
	return float4(result, in.opacity);
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

fragment float4 tonemapFragment(TMOut in [[stage_in]],
                                texture2d<float> hdr   [[texture(0)]],
                                texture2d<float> bloom [[texture(1)]],
                                constant float2& params [[buffer(0)]]) // x: exposure, y: bloomStrength
{
	constexpr sampler s(filter::linear);
	float3 c = hdr.sample(s, in.uv).rgb;
	c += bloom.sample(s, in.uv).rgb * params.y;
	c *= params.x;
	c = aces(c);
	c = pow(c, float3(1.0 / 2.2));
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
)MSL";

// In-Game UI 2D pass: solid-color quads derived from vertex_id + uniforms.
// rect = {x, y, w, h} pixels;  viewport = {vpW, vpH} pixels.
static const char* kUIMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct UIVert { float4 position [[position]]; float2 uv; };
vertex UIVert uiVertex(uint vid [[vertex_id]],
                       constant float4& rect     [[buffer(0)]],
                       constant float2& viewport [[buffer(1)]])
{
    const float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    float2 uv = c[vid];
    float2 sp = rect.xy + uv * rect.zw;
    float2 ndc = float2(sp.x / viewport.x * 2.0 - 1.0,
                        1.0 - sp.y / viewport.y * 2.0);
    UIVert o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv = uv;
    return o;
}
fragment float4 uiFragment(UIVert in [[stage_in]],
                           constant float4& color [[buffer(0)]])
{
    return color;
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
struct SSAOPosUniforms { float4x4 mvp; float4x4 modelView; };
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

// ─── Procedural skybox (drawn into the HDR target behind the scene) ─────────
static const char* kSkyMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SkyOut { float4 position [[position]]; float2 ndc; };
struct SkyParams { float4x4 invViewProj; float4 sunDir; float4 sunColor; float4 params; float4 nebulaColor; float4 auroraColor; float4 wind; }; // params: x=timeOfDay y=coverage z=time w=aurora; nebulaColor.w=nebula intensity; auroraColor.w=milkyWay; wind.xyz=cloud drift/s; wind.w=lightning flash

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

// Textured moon disk — drawn only in the sky pass (kept out of the shared
// skyColor() so the scene's image-based ambient needn't bind the texture).
// Smaller than the sun and shaded as a sphere so the grayscale map reads as
// craters on a lit body. Mirrors the GL moonDisk() exactly.
float3 moonDisk(float3 dir, float3 sunDir, bool hasMoon,
                texture2d<float> moonTex, sampler moonSamp)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day   = smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	float night = 1.0 - day;
	if (night <= 0.0) return float3(0.0);

	float3 moonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return float3(0.0);

	float3 right = normalize(cross(float3(0.0, 1.0, 0.0), moonDir));
	float3 up    = cross(moonDir, right);
	const float kRadius = 0.030;                   // angular radius (< the sun disk)
	float2 q = float2(dot(dir, right), dot(dir, up)) / kRadius;
	float  r = length(q);
	if (r > 1.0) return float3(0.0);

	float tex  = hasMoon ? moonTex.sample(moonSamp, q * 0.5 + 0.5).r : 1.0;
	float limb = sqrt(max(1.0 - r * r, 0.0));      // spherical brightness falloff
	float edge = smoothstep(1.0, 0.90, r);         // soft anti-aliased rim
	float3 tint = float3(0.92, 0.94, 1.00);
	return tint * tex * limb * edge * 3.0 * night;
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
float3 starField(float3 dir, float3 cdir, float3 sunDir, float time, float milkyWay)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return float3(0.0);

	// Stars cluster densely along the galactic band: the cell-occupancy threshold
	// is lowered there so the Milky Way reads as a dense lane of stars (not a
	// smear). The milky-way control drives how dense/bright the lane is. Sampled
	// in the rotating celestial frame so the whole field drifts.
	float  band   = galacticBand(cdir);
	float  mw     = clamp(milkyWay, 0.0, 1.0);
	float  thresh = mix(0.92, mix(0.86, 0.72, mw), band);
	float3 p       = cdir * 70.0;
	float3 cell    = floor(p);
	float  present = starHash(cell);
	if (present < thresh) return float3(0.0);

	float3 sp   = float3(starHash(cell + 1.7), starHash(cell + 4.3), starHash(cell + 8.9));
	float  d    = length(fract(p) - sp);
	// Per-star size class: cubic skew so most stars are tiny pinpoints, a few are
	// medium, and the rare brightest are larger with a faint halo — a realistic
	// apparent-magnitude spread instead of one fixed size.
	float  sizeH  = starHash(cell + 5.7);
	float  big    = sizeH * sizeH * sizeH;
	float  radius = mix(0.05, 0.17, big);
	float  core   = smoothstep(radius, 0.0, d);
	core *= core;                                  // tighten the core, keep a faint glow
	float  halo   = smoothstep(radius * 3.0, radius, d) * (big * big) * 0.35; // only big stars
	float  shape  = core + halo;
	float  mag  = (0.4 + 0.6 * smoothstep(thresh, 1.0, present)) * mix(0.7, 2.7, big); // size→brightness
	// Random per-star twinkle: each star gets its own phase + frequency so the
	// field shimmers randomly in real time (wall clock, not the slow time-of-day).
	float  twPhase = starHash(cell + 23.5) * 6.2831;
	float  twFreq  = 2.0 + 4.0 * starHash(cell + 47.1);
	float  tw      = 0.7 + 0.3 * sin(time * twFreq + twPhase);
	float  horizon = smoothstep(0.0, 0.15, dir.y); // fade into the horizon haze
	float3 tint = mix(float3(0.80, 0.88, 1.0), float3(1.0, 0.93, 0.82), starHash(cell + 12.1));
	// The dense band stars sit fainter en masse so the lane reads as many small
	// stars; the intensity control scales that mass brightness.
	float bandDim = mix(1.6, mix(0.9, 1.5, mw), band);
	return tint * (shape * mag * tw * horizon * night * bandDim);
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
                   texture3d<float> noiseTex, sampler noiseSamp)
{
	if (coverage <= 0.0) return baseSky;          // clear sky → skip the whole raymarch
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

	// March the view ray through the cloud slab between base and top heights.
	// A deterministic per-ray offset breaks up otherwise coherent sample planes
	// that show up as visible horizontal cloud layers near grazing view angles.
	float s0 = kCloudBase / max(dir.y, 1e-3);
	float s1 = kCloudTop  / max(dir.y, 1e-3);
	const int N = 16;
	float ds = (s1 - s0) / float(N);
	float jitter = cloudHash(dir.xz * 173.3 + float2(dir.y * 37.1, dir.y * 19.7));

	// Day/night/dusk drive the cloud colour (independent of the drift clock).
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	// Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	float  T = 1.0;                                // transmittance along the view ray
	float3 L = float3(0.0);                        // accumulated in-scattered colour
	for (int i = 0; i < N; ++i)
	{
		float  s   = s0 + (float(i) + jitter) * ds;
		float3 pos = dir * s;
		float  dens = cloudDensity(pos, time, coverage, wind, noiseTex, noiseSamp);
		if (dens > 0.001)
		{
			// Light-march toward the sun: Beer's-law self-shadowing (3 steps for a
			// smooth shadow gradient; fewer steps undersample and flicker).
			float shadow = 0.0;
			for (int j = 1; j <= 3; ++j)
				shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25), time, coverage, wind, noiseTex, noiseSamp);
			float sun    = exp(-shadow * 1.7);
			float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
			float lit    = sun * powder;

			// Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
			float3 dayCol   = mix(float3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			float3 nightCol = mix(float3(0.015, 0.018, 0.035), float3(0.26, 0.29, 0.45), lit);
			float3 cloudCol = mix(nightCol, dayCol, day);
			float3 duskTop  = sunColor * float3(1.25, 0.55, 0.28);
			cloudCol = mix(cloudCol, duskTop, dusk * lit * 0.9);
			// Moonlit silver: moon rises on the opposite arc from the sun.
			float3 cMoonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
			float  cMoonUp  = clamp((cMoonDir.y + 0.10) / 0.25, 0.0, 1.0);
			cloudCol += float3(0.20, 0.22, 0.38) * lit * cMoonUp * (1.0 - day) * 0.25;
			// Forward-scatter glow: Henyey-Greenstein-weighted direct sunlight makes
			// the sun-facing edges flare gold (the silver lining), strongest when
			// looking toward the sun and where the cloud isn't self-shadowed.
			cloudCol += sunColor * (phase * sun * 0.9 * max(day, dusk));
			// Cheap vertical depth: tops catch the light (bright crown), the base
			// sits in self-shadow (darker, cooler) — fakes the volumetric
			// "cauliflower" relief from just the sample's height in the slab.
			float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
			cloudCol *= mix(0.5, 1.15, hTone);
			cloudCol += float3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.25);

			float opticalDepth = dens * ds * 7.0;
			float a = 1.0 - exp(-opticalDepth);
			L += T * a * cloudCol;
			T *= 1.0 - a;
			if (T < 0.02) break;
		}
	}

	// Fade the whole cloud layer out into the horizon haze.
	float horizon = smoothstep(0.02, 0.16, dir.y);
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	return baseSky * T + L;
}

// Space nebula — drifting coloured emission clouds gathered toward the galactic
// band. Sampled as 3D blobs on the celestial sphere (rotates with the stars):
// isolated rounded patches of varying size with bright cores, dark dust lanes,
// and a blue->magenta->teal hue wheel so neighbouring blobs differ in colour and
// bleed into one another. Night/horizon gated, occluded by clouds. Mirrors GL.
float3 nebula(float3 dir, float3 cdir, float3 sunDir, float intensity, float3 nebColor,
              texture3d<float> noiseTex, sampler noiseSamp)
{
	if (intensity <= 0.0) return float3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return float3(0.0);

	float3 cN   = normalize(cdir);
	const float3 galN = normalize(float3(0.46, 0.52, -0.72));
	float  bd   = dot(cN, galN);
	float  band = exp(-bd * bd * 1.5);           // wide soft milky-way bias
	float3 P    = cN * 3.4;
	float  big  = starFbm3(P * 0.7 + 11.0, 4, noiseTex, noiseSamp);   // large clouds
	float  med  = starFbm3(P * 1.7 + 27.0, 3, noiseTex, noiseSamp);   // medium clumps
	float  fine = starFbm3(P * 4.0 + 41.0, 2, noiseTex, noiseSamp);   // fine mottle / embedded dust
	float  blob   = smoothstep(0.35, 0.70, big * 0.5 + med * 0.6);
	// Structural character per region: dense puffy bodies vs. wispy filaments.
	float  charF  = starFbm3(P * 0.4 + 150.0, 2, noiseTex, noiseSamp);
	float  wispy  = smoothstep(0.42, 0.70, charF);
	float  fila   = smoothstep(0.55, 0.86, starFbm3(P * 5.5 + 97.0, 2, noiseTex, noiseSamp));   // fine filaments
	float  detail = (0.30 + 0.70 * smoothstep(0.32, 0.86, fine)) * mix(1.0, 0.65 + 0.9 * fila, wispy);
	float  dust   = 1.0 - 0.5 * smoothstep(0.50, 0.88, starFbm3(P * 2.6 + 63.0, 3, noiseTex, noiseSamp));
	float  density = blob * detail * dust;
	float  core   = smoothstep(0.62, 0.95, big * 0.55 + med * 0.55);   // bright centres
	float  glow   = (band * 0.85 + 0.15) * (density + 0.6 * core);     // baseline -> off-band patches
	if (glow <= 0.0) return float3(0.0);

	// Hue wheel across neighbouring blobs: cool blue → teal → green → gold →
	// magenta so regions differ in colour and bleed together. A large-scale field
	// biases whole regions warm (emission) vs. cool (reflection) for more variety.
	float  h = clamp(starFbm3(P * 0.5 + 71.0, 3, noiseTex, noiseSamp) * 1.7 - 0.35
	               + 0.25 * (starFbm3(P * 1.1 + 83.0, 2, noiseTex, noiseSamp) - 0.5), 0.0, 1.0);
	float  warm = smoothstep(0.40, 0.72, starFbm3(P * 0.32 + 131.0, 2, noiseTex, noiseSamp));
	h = clamp(h + warm * 0.30, 0.0, 1.0);
	float3 colA = nebColor * float3(0.42, 0.62, 1.50);   // cool blue
	float3 colB = nebColor * float3(0.34, 1.42, 1.18);   // teal/cyan
	float3 colC = nebColor * float3(0.55, 1.42, 0.55);   // green
	float3 colD = nebColor * float3(1.75, 1.10, 0.40);   // gold/amber
	float3 colE = nebColor * float3(1.85, 0.42, 0.95);   // magenta/pink
	float3 col  = colA;
	col = mix(col, colB, smoothstep(0.14, 0.36, h));
	col = mix(col, colC, smoothstep(0.36, 0.54, h));
	col = mix(col, colD, smoothstep(0.54, 0.72, h));
	col = mix(col, colE, smoothstep(0.72, 0.92, h));
	float  horizon = smoothstep(0.0, 0.16, dir.y);
	return col * (glow * 6.0 * horizon * night * intensity);
}

// Aurora borealis — drifting light curtains, night only, intensity + colour
// user-controlled. Modelled as wavy ribbons projected onto a high curtain plane
// that run along one axis and stack along the other, so they sweep across the
// whole sky from one side to the other (not a single ring around the camera).
// Fine vertical striations + drift give the rayed, volumetric structure.
// Mirrors the GL aurora() exactly.
float3 aurora(float3 dir, float3 sunDir, float time, float intensity, float3 auroraCol)
{
	if (intensity <= 0.0) return float3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.04) return float3(0.0);

	// Project onto a high curtain plane; ribbons run along x and stack along z.
	float2 P      = dir.xz / (dir.y + 0.45);
	float  along  = P.x;
	float  across = P.y;
	float  wave   = 0.40 * sin(along * 0.7 + time * 0.15)
	              + 0.30 * cloudFbm(float2(along * 0.35 - time * 0.04, 3.0));
	float  phase  = across * 0.30 + wave;
	float  f      = abs(fract(phase) - 0.5);            // distance to the nearest ribbon
	float  ribbon = smoothstep(0.10, 0.45, f);
	float  stri   = cloudFbm(float2(along * 6.0 + time * 0.25, across * 1.2));
	float  curtain = ribbon * (0.45 + 0.55 * smoothstep(0.30, 0.80, stri));
	float  patches = 0.65 + 0.35 * smoothstep(0.25, 0.85,
	                cloudFbm(float2(along * 0.45 + time * 0.03, across * 0.4 + 9.0)));
	// 3-stop colour: purple base → green body → teal tips
	float  hcol    = smoothstep(0.05, 0.60, dir.y);
	float3 baseCol = auroraCol * float3(0.60, 0.15, 0.90);
	float3 topCol  = auroraCol * float3(0.30, 0.90, 0.70);
	float3 col     = mix(baseCol, auroraCol, smoothstep(0.0, 0.5, hcol));
	col            = mix(col,     topCol,    smoothstep(0.5, 1.0, hcol));
	float  fade   = smoothstep(0.03, 0.16, dir.y) * (1.0 - smoothstep(0.78, 1.0, dir.y));
	return col * (curtain * patches * fade * intensity * night * 5.0);
}

fragment float4 skyFragment(SkyOut in [[stage_in]],
                            constant SkyParams& p [[buffer(0)]],
                            texture2d<float> moonTex [[texture(0)]],
                            sampler moonSamp [[sampler(0)]],
                            texture3d<float> noiseTex [[texture(1)]],
                            sampler noiseSamp [[sampler(1)]])
{
	float4 wp1 = p.invViewProj * float4(in.ndc,  1.0, 1.0);
	float4 wp0 = p.invViewProj * float4(in.ndc, -1.0, 1.0);
	float3 dir = wp1.xyz / wp1.w - wp0.xyz / wp0.w;
	float3 col  = skyColor(dir, p.sunDir.xyz);
	// Night-sky elements + the celestial rotation are skipped entirely by day. The
	// branch is coherent (sunDir is uniform → every pixel takes the same path).
	float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(p.sunDir.xyz).y, -0.2, 1.0));
	if (nightF > 0.0)
	{
		float3 cdir = celestialDir(dir, p.params.x); // turns with the day-night cycle
		col += starField(dir, cdir, p.sunDir.xyz, p.params.z, p.auroraColor.w);
		col += nebula(dir, cdir, p.sunDir.xyz, p.nebulaColor.w, p.nebulaColor.xyz, noiseTex, noiseSamp);
		col += aurora(dir, p.sunDir.xyz, p.params.z, p.params.w, p.auroraColor.xyz);
		col += moonDisk(dir, p.sunDir.xyz, p.sunDir.w > 0.5, moonTex, moonSamp);
	}
	col = applyClouds(col, dir, p.sunDir.xyz, p.params.z, p.params.y, p.sunColor.xyz, p.wind.xyz, noiseTex, noiseSamp);
	col += p.wind.w * float3(0.85, 0.90, 1.0); // lightning lights up the sky/clouds
	return float4(col, 1.0);
}
)MSL";

// Shared analytic sky, injected (via the //#SKYFUNC# marker) into the scene and
// skybox MSL so background and image-based ambient match. Mirrors the GL
// kSkyFuncGLSL exactly: the mood follows the sun's elevation (day↔sunset↔night).
static const char* kSkyFuncMSL = R"MSL(
float3 skyColor(float3 dir, float3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	float3 zenithDay  = float3(0.08, 0.28, 0.72);
	float3 horizDay   = float3(0.42, 0.62, 0.88);
	float3 zenithNite = float3(0.003, 0.005, 0.015);
	float3 horizNite  = float3(0.006, 0.009, 0.024);
	float3 zenith  = mix(zenithNite, zenithDay, day);
	float3 horizon = mix(horizNite,  horizDay,  day);

	// Directional sunset warmth: the warm band is concentrated toward the sun's
	// azimuth (golden near the sun, cooler magenta away) instead of a flat ring,
	// and the zenith picks up a touch of dusk purple for atmospheric depth.
	float2 sunAz  = normalize(sunDir.xz + float2(1e-5));
	float  toward = dot(normalize(dir.xz + float2(1e-5)), sunAz) * 0.5 + 0.5; // 0 away → 1 toward
	toward = pow(clamp(toward, 0.0, 1.0), 1.5);
	float3 duskHoriz = mix(float3(0.52, 0.30, 0.52), float3(1.20, 0.50, 0.16), toward);
	horizon = mix(horizon, duskHoriz, dusk);
	zenith  = mix(zenith,  float3(0.20, 0.16, 0.40), dusk * 0.6);

	float h    = clamp(dir.y, 0.0, 1.0);
	float grad = pow(1.0 - h, 2.5);
	float3 sky = mix(zenith, horizon, grad);

	// Concentrated golden scattering band hugging the horizon toward the sun.
	float band = pow(1.0 - h, 8.0) * toward;
	sky += float3(1.25, 0.62, 0.26) * (band * dusk * 0.8);

	float3 ground = mix(float3(0.02, 0.02, 0.03), float3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));

	// Layered sun aureole — a crisp disk plus tight/mid blooms and a broad warm
	// scatter that survive through sunset for a cinematic, volumetric glow.
	float3 sunTint = mix(float3(1.0, 0.42, 0.20), float3(1.0, 0.96, 0.88),
	                     smoothstep(0.0, 0.25, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	sky += sunTint * (pow(s, 1800.0) * 14.0) * day;
	sky += sunTint * (pow(s, 180.0)  * 2.2) * sunVis;
	sky += sunTint * (pow(s, 22.0)   * 0.7) * sunVis;
	sky += float3(1.0, 0.5, 0.25) * (pow(s, 5.0) * 0.5) * dusk;

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
	glm::vec4 sunDir = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	glm::vec4 ambient = glm::vec4(0.0f);
	glm::vec4 fog = glm::vec4(0.0f); // x = density (0 = off), y = height falloff
	glm::vec4 viewport = glm::vec4(0.0f); // xy = output size, z = ssaoEnabled
};

// Matches the MSL SSAOPosUniforms / SSAOParams structs.
struct SSAOPosUniforms
{
	glm::mat4 mvp;
	glm::mat4 modelView;
};
struct SSAOParamsCPU
{
	glm::mat4 proj;
	glm::vec4 cfg;          // xy = noise scale, z = radius, w = bias
	glm::vec4 cfg2;         // x = intensity, y = AO method (0 SSAO, 1 HBAO, 2 GTAO)
	glm::vec4 samples[32];  // hemisphere kernel (xyz)
};

// Matches the MSL SkyParams struct.
struct SkyParams
{
	glm::mat4 invViewProj = glm::mat4(1.0f);
	glm::vec4 sunDir      = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
	glm::vec4 sunColor    = glm::vec4(1.0f);
	glm::vec4 params      = glm::vec4(0.0f); // x = timeOfDay (cloud scroll), y = coverage, z = wall-clock time, w = aurora
	glm::vec4 nebulaColor = glm::vec4(0.42f, 0.45f, 0.92f, 0.5f); // xyz = colour, w = nebula intensity
	glm::vec4 auroraColor = glm::vec4(0.25f, 0.95f, 0.50f, 0.6f); // xyz = colour, w = milky-way intensity
	glm::vec4 wind        = glm::vec4(0.0f); // xyz = horizontal cloud drift (world units / s)
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
	CreateDebugLinePipeline();
	CreateParticlePipeline();
	EnsureShadowResources();

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

	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyBloomTargets();
	DestroyLdrTarget();
	DestroySSAOTargets();
	DrainRetiredTextures();
	if (m_tonemapPipeline)      { CFBridgingRelease(m_tonemapPipeline);      m_tonemapPipeline = nullptr; }
	if (m_fxaaPipeline)         { CFBridgingRelease(m_fxaaPipeline);         m_fxaaPipeline = nullptr; }
	if (m_uiPipeline)           { CFBridgingRelease(m_uiPipeline);           m_uiPipeline = nullptr; }
	if (m_bloomBrightPipeline)  { CFBridgingRelease(m_bloomBrightPipeline);  m_bloomBrightPipeline = nullptr; }
	if (m_blurPipeline)         { CFBridgingRelease(m_blurPipeline);         m_blurPipeline = nullptr; }
	if (m_skyPipeline)          { CFBridgingRelease(m_skyPipeline);          m_skyPipeline = nullptr; }
	if (m_moonTexture)          { CFBridgingRelease(m_moonTexture);          m_moonTexture = nullptr; }
	if (m_dummyTexture)    { CFBridgingRelease(m_dummyTexture);    m_dummyTexture = nullptr; }
	if (m_linearSampler)   { CFBridgingRelease(m_linearSampler);   m_linearSampler = nullptr; }
	if (m_noiseTexture)    { CFBridgingRelease(m_noiseTexture);    m_noiseTexture = nullptr; }
	if (m_noiseSampler)    { CFBridgingRelease(m_noiseSampler);    m_noiseSampler = nullptr; }
	if (m_skyEnvCube)      { CFBridgingRelease(m_skyEnvCube);      m_skyEnvCube = nullptr; }
	if (m_scenePipeline)        { CFBridgingRelease(m_scenePipeline);        m_scenePipeline = nullptr; }
	if (m_sceneBlendPipeline)   { CFBridgingRelease(m_sceneBlendPipeline);   m_sceneBlendPipeline = nullptr; }
	if (m_skinnedPipeline)      { CFBridgingRelease(m_skinnedPipeline);      m_skinnedPipeline = nullptr; }
	if (m_particleSimPipeline)  { CFBridgingRelease(m_particleSimPipeline);  m_particleSimPipeline = nullptr; }
	if (m_particleDrawPipeline) { CFBridgingRelease(m_particleDrawPipeline); m_particleDrawPipeline = nullptr; }
	if (m_particleBuffer)       { CFBridgingRelease(m_particleBuffer);       m_particleBuffer = nullptr; }
	if (m_sceneDepthState) { CFBridgingRelease(m_sceneDepthState); m_sceneDepthState = nullptr; }
	if (m_shadowPipeline)  { CFBridgingRelease(m_shadowPipeline);  m_shadowPipeline = nullptr; }
	if (m_shadowDepthTex)  { CFBridgingRelease(m_shadowDepthTex);  m_shadowDepthTex = nullptr; }
	if (m_noDepthState)    { CFBridgingRelease(m_noDepthState);    m_noDepthState = nullptr; }
	if (m_skyDepthState)   { CFBridgingRelease(m_skyDepthState);   m_skyDepthState = nullptr; }
	if (m_ssaoPosPipeline)  { CFBridgingRelease(m_ssaoPosPipeline);  m_ssaoPosPipeline = nullptr; }
	if (m_ssaoPipeline)     { CFBridgingRelease(m_ssaoPipeline);     m_ssaoPipeline = nullptr; }
	if (m_ssaoBlurPipeline) { CFBridgingRelease(m_ssaoBlurPipeline); m_ssaoBlurPipeline = nullptr; }
	if (m_ssaoNoiseTex)     { CFBridgingRelease(m_ssaoNoiseTex);     m_ssaoNoiseTex = nullptr; }
	if (m_ssaoPointSampler) { CFBridgingRelease(m_ssaoPointSampler); m_ssaoPointSampler = nullptr; }
	if (m_ssaoNoiseSampler)  { CFBridgingRelease(m_ssaoNoiseSampler);  m_ssaoNoiseSampler = nullptr; }
	if (m_debugLinePipeline) { CFBridgingRelease(m_debugLinePipeline); m_debugLinePipeline = nullptr; }

	if (m_imguiPassDescriptor) { CFBridgingRelease(m_imguiPassDescriptor); m_imguiPassDescriptor = nullptr; }
	if (m_commandQueue)        { CFBridgingRelease(m_commandQueue);        m_commandQueue = nullptr; }
	if (m_device)              { CFBridgingRelease(m_device);              m_device = nullptr; }
	m_primarySdlWindow = nullptr;
}

// ─── Pipeline / mesh setup ────────────────────────────────────────────────────

// Deterministic [0,1) RNG so this backend builds the *identical* SSAO kernel and
// rotation noise as the GL backend — a prerequisite for GL == Metal parity.
struct SsaoRng { uint32_t s; float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); } };

// Cosine-ish hemisphere kernel oriented to +Z, packed toward the origin so close
// occluders dominate. Identical to the OpenGL backend's BuildSSAOKernel.
static std::vector<glm::vec3> BuildSSAOKernel(int n)
{
	SsaoRng rng{ 0x9E3779B9u };
	std::vector<glm::vec3> k(n);
	for (int i = 0; i < n; ++i)
	{
		glm::vec3 s(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next());
		s = glm::normalize(s) * rng.next();
		float t = static_cast<float>(i) / static_cast<float>(n);
		s *= 0.1f + 0.9f * t * t;
		k[i] = s;
	}
	return k;
}
// 4×4 tile of random tangent-plane rotation vectors (z = 0). Identical to GL.
static std::vector<glm::vec3> BuildSSAONoise(int n)
{
	SsaoRng rng{ 0x2545F491u };
	std::vector<glm::vec3> v(n);
	for (int i = 0; i < n; ++i)
		v[i] = glm::vec3(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, 0.0f);
	return v;
}

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
			Logger::Log(Logger::LogLevel::Error, "MetalRenderer: debug line shader compile failed");
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
			Logger::Log(Logger::LogLevel::Error, "MetalRenderer: debug line pipeline creation failed");
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
		glm::mat4 vp = kMetalClipFix * viewProj;

		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_debugLinePipeline];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		[enc setVertexBuffer:vbuf offset:0 atIndex:0];
		[enc setVertexBytes:&vp length:sizeof(vp) atIndex:1];
		[enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:vertCount];
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
		uiDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
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
		m_linearSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:sampDesc]);

		// 3D noise volume the sky's starFbm3/worleyFbm sample (clouds + nebula), built
		// once on the CPU. RG16Unorm (R=value noise, G=Worley billows) + linear +
		// repeat so it tiles seamlessly.
		constexpr int kNoiseN = 256;
		const std::vector<uint16_t> noise = BuildSkyNoise3D(kNoiseN);
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

		// Empty image-based-ambient cubemap (RGBA32F); filled per frame from the
		// analytic skyColor (SkyColorCPU) when the sun direction changes. The
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
		const std::vector<glm::vec3> ssaoNoise = BuildSSAONoise(16);
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
			[NSString stringWithUTF8String:injectSkyMSL(kUnlitMSL).c_str()] options:nil error:&error];
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
	// Cull casters against the LIGHT frustum, not the camera — an off-screen
	// object still casts a shadow into the visible scene while it lies within the
	// shadow map's coverage. (Camera-culling the casters made shadows pop out as
	// their caster left the screen.)
	m_culler.cull(m_renderWorld, m_renderWorld.shadow.viewProj, m_visible);
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

		HE::UUID shMeshId{}; const GpuMesh* shMesh = nullptr; bool shMeshValid = false;
		for (uint32_t idx : m_sortedIndices)
		{
			const RenderObject& obj = m_renderWorld.objects[idx];
			if (!obj.castsShadow) continue; // billboards (precip/particles) cast no shadow
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
	}
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

	return &m_skeletalMeshCache.emplace(assetId, mesh).first->second;
}

// ─── Material override texture ──────────────────────────────────────────────
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

	void* retained = nullptr;
	if (!mat->texturePaths.empty())
	{
		const HE::UUID texId = m_contentManager->loadAsset(mat->texturePaths[0]);
		if (const TextureAsset* tex = m_contentManager->getTexture(texId);
		    tex && !tex->data.empty() && tex->channels == 4)
		{
			id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
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
			retained = (void*)CFBridgingRetain(texture);
		}
	}

	m_materialTexCache.emplace(materialId, retained);
	outTex = retained;
	return true;
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
	outOpacity   = mat->opacity;
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

void MetalRenderer::SetBloomSettings(const BloomSettings& s)
{
	m_bloomEnabled   = s.enabled;
	m_bloomThreshold = s.threshold;
	m_bloomStrength  = s.intensity;
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

	auto fullscreenPass = [&](id<MTLTexture> dst, id<MTLRenderPipelineState> pso,
	                          id<MTLTexture> src, const void* bytes, size_t len)
	{
		MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
		p.colorAttachments[0].texture     = dst;
		p.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
		p.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:p];
		[enc setRenderPipelineState:pso];
		[enc setFragmentTexture:src atIndex:0];
		[enc setFragmentBytes:bytes length:len atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
	};

	id<MTLTexture> tex0 = (__bridge id<MTLTexture>)m_bloomColor[0];
	id<MTLTexture> tex1 = (__bridge id<MTLTexture>)m_bloomColor[1];

	// Bright pass: HDR scene color → m_bloomColor[0].
	const simd::float2 brightParams = { m_bloomThreshold, m_bloomKnee };
	fullscreenPass(tex0, (__bridge id<MTLRenderPipelineState>)m_bloomBrightPipeline,
	               (__bridge id<MTLTexture>)m_hdrColor, &brightParams, sizeof(brightParams));

	// Ping-pong Gaussian blur.
	const simd::float2 texel = { 1.0f / (float)m_bloomW, 1.0f / (float)m_bloomH };
	bool horizontal = true;
	constexpr int kBlurPasses = 10; // 5 horizontal + 5 vertical
	for (int i = 0; i < kBlurPasses; ++i)
	{
		id<MTLTexture> dst = horizontal ? tex1 : tex0;
		id<MTLTexture> src = horizontal ? tex0 : tex1;
		const simd::float4 cfg = { texel.x, texel.y, horizontal ? 1.0f : 0.0f, 0.0f };
		fullscreenPass(dst, (__bridge id<MTLRenderPipelineState>)m_blurPipeline,
		               src, &cfg, sizeof(cfg));
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

	m_ssaoW = width; m_ssaoH = height;
}

void MetalRenderer::DestroySSAOTargets()
{
	if (m_ssaoPosTex)   { CFBridgingRelease(m_ssaoPosTex);   m_ssaoPosTex = nullptr; }
	if (m_ssaoPosDepth) { CFBridgingRelease(m_ssaoPosDepth); m_ssaoPosDepth = nullptr; }
	if (m_ssaoTex)      { CFBridgingRelease(m_ssaoTex);      m_ssaoTex = nullptr; }
	if (m_ssaoBlurTex)  { CFBridgingRelease(m_ssaoBlurTex);  m_ssaoBlurTex = nullptr; }
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

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height), &m_editorCamera);
	if (m_renderWorld.objects.empty()) return;
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);
	m_culler.cull(m_renderWorld, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	if (m_sortedIndices.empty()) return;

	EnsureSSAOTargets(width, height);
	const glm::mat4 viewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 view     = m_renderWorld.camera.view;

	@autoreleasepool
	{
		id<MTLCommandBuffer> cmdBuf = (__bridge id<MTLCommandBuffer>)cmdBufPtr;

		// ── 1. View-space position pre-pass ────────────────────────────────
		MTLRenderPassDescriptor* pp = [MTLRenderPassDescriptor renderPassDescriptor];
		pp.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ssaoPosTex;
		pp.colorAttachments[0].loadAction  = MTLLoadActionClear;
		pp.colorAttachments[0].storeAction = MTLStoreActionStore;
		pp.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0); // a = 0 → background
		pp.depthAttachment.texture     = (__bridge id<MTLTexture>)m_ssaoPosDepth;
		pp.depthAttachment.loadAction  = MTLLoadActionClear;
		pp.depthAttachment.storeAction = MTLStoreActionDontCare;
		pp.depthAttachment.clearDepth  = 1.0;
		id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:pp];
		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_ssaoPosPipeline];
		[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
		HE::UUID lastId{}; const GpuMesh* cMesh = nullptr; bool valid = false;
		for (uint32_t idx : m_sortedIndices)
		{
			const RenderObject& obj = m_renderWorld.objects[idx];
			if (!obj.contributesAO) continue; // precip/particles: skip the SSAO prepass
			SSAOPosUniforms u;
			u.mvp       = viewProj * obj.transform;
			u.modelView = view * obj.transform;
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

		// ── 2. Occlusion (fullscreen) ──────────────────────────────────────
		SSAOParamsCPU params;
		params.proj = m_renderWorld.camera.projection;
		params.cfg  = glm::vec4(static_cast<float>(width) / 4.0f, static_cast<float>(height) / 4.0f,
		                        m_ssaoRadius, 0.025f);
		params.cfg2 = glm::vec4(m_ssaoIntensity, static_cast<float>(m_ssaoMethod), 0.0f, 0.0f);
		const std::vector<glm::vec3> kernel = BuildSSAOKernel(32);
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

// Fullscreen tonemap of the HDR scene color (+ bloom) into the bound encoder's target.
void MetalRenderer::EncodeTonemap(void* renderEncoderPtr)
{
	if (!m_tonemapPipeline || !m_hdrColor) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_tonemapPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_hdrColor atIndex:0];
	// Bloom on texture slot 1 (fall back to the HDR texture with 0 strength so the
	// shader's sampler always has a valid binding). m_bloomResult is null when
	// bloom was disabled or unavailable this frame.
	id<MTLTexture> bloomTex = m_bloomResult
		? (__bridge id<MTLTexture>)m_bloomResult
		: (__bridge id<MTLTexture>)m_hdrColor;
	[enc setFragmentTexture:bloomTex atIndex:1];
	const simd::float2 params = { 1.0f, m_bloomResult ? m_bloomStrength : 0.0f };
	[enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
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

// Fullscreen FXAA of the tonemapped LDR image into the bound encoder's target.
void MetalRenderer::EncodeFxaa(void* renderEncoderPtr, int width, int height)
{
	if (!m_fxaaPipeline || !m_ldrColor) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_fxaaPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	[enc setFragmentTexture:(__bridge id<MTLTexture>)m_ldrColor atIndex:0];
	const simd::float2 rcpFrame = { 1.0f / (float)std::max(1, width), 1.0f / (float)std::max(1, height) };
	[enc setFragmentBytes:&rcpFrame length:sizeof(rcpFrame) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// 2D UI quads (solid color) into the bound render encoder's target.
void MetalRenderer::EncodeUIPass(void* renderEncoderPtr, int width, int height)
{
	if (!m_uiPipeline || m_renderWorld.uiObjects.empty()) return;
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)renderEncoderPtr;
	[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_uiPipeline];
	[enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_noDepthState];
	const simd::float2 vp = { (float)std::max(1, width), (float)std::max(1, height) };
	[enc setVertexBytes:&vp length:sizeof(vp) atIndex:1];
	for (const UIRenderObject& obj : m_renderWorld.uiObjects)
	{
		const simd::float4 rect  = { obj.position.x, obj.position.y, obj.size.x, obj.size.y };
		const simd::float4 color = { obj.color.r, obj.color.g, obj.color.b, obj.color.a };
		[enc setVertexBytes:&rect  length:sizeof(rect)  atIndex:0];
		[enc setFragmentBytes:&color length:sizeof(color) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
}

// ─── Frame encoding ───────────────────────────────────────────────────────────

void MetalRenderer::EncodeSky(void* renderEncoder, const glm::mat4& invViewProj,
                             const glm::vec3& sunDir, const glm::vec3& sunColor,
                             float timeOfDay, float cloudCoverage, float time,
                             float auroraIntensity, const glm::vec3& nebulaColor,
                             float nebulaIntensity, const glm::vec3& auroraColor,
                             float milkyWayIntensity, const glm::vec3& wind)
{
	if (!m_skyPipeline) return;
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
	SkyParams p;
	p.invViewProj = invViewProj;
	p.sunDir      = glm::vec4(sunDir, m_moonTexture ? 1.0f : 0.0f); // w = has-moon flag
	p.sunColor    = glm::vec4(sunColor, 0.0f);
	p.params      = glm::vec4(timeOfDay, cloudCoverage, time, auroraIntensity);
	p.nebulaColor = glm::vec4(nebulaColor, nebulaIntensity);
	p.auroraColor = glm::vec4(auroraColor, milkyWayIntensity);
	p.wind        = glm::vec4(wind, GetEnvironment().flash); // w = lightning flash
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
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
	for (int f = 0; f < 6; ++f)
	{
		const std::vector<float> face = BuildSkyEnvFace(N, f, sunDir);
		[cube replaceRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0 slice:f
		          withBytes:face.data() bytesPerRow:N * 4 * sizeof(float) bytesPerImage:0];
	}
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
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(shadows ? m_shadowDepthTex : m_dummyTexture) atIndex:1];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:1];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];
	const bool ssaoActive = m_ssaoEnabled && m_ssaoResult;
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:3];
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
		u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, 0, 0, 0);
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
		// boneBuf is released here (ARC); the encoder holds its own strong reference
	}

	// Restore the regular scene pipeline for subsequent passes
	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_scenePipeline];
}

void MetalRenderer::EncodeScene(void* renderEncoder, int width, int height)
{
	if (!m_world || !m_scenePipeline || width <= 0 || height <= 0)
		return;

	id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(width) / static_cast<float>(height),
	                    &m_editorCamera);
	m_extractor.extractUI(*m_world, static_cast<float>(width), static_cast<float>(height),
	                      m_renderWorld);

	const glm::mat4 viewProj =
		m_renderWorld.camera.projection * m_renderWorld.camera.view;

	// Direction toward the sun for sky + image-based ambient — resolved by the
	// extractor (scene directional light, or the day-night cycle when enabled).
	const glm::vec3 sunDir = m_renderWorld.sunDirection;

	// Skybox is drawn LAST (after the geometry) with a depth-test == far, so the
	// heavy sky shader only runs on the background pixels the scene didn't cover.
	// This lambda is invoked at every exit so the background is always filled.
	const float windRad = glm::radians(GetEnvironment().windDirection);
	const glm::vec3 windVec = glm::vec3(std::sin(windRad), 0.0f, -std::cos(windRad))
	                        * (GetEnvironment().windSpeed * 0.025f);
	auto drawSky = [&]() {
		EncodeSky(renderEncoder, glm::inverse(viewProj), sunDir, GetEnvironment().sunColor,
		          GetEnvironment().timeOfDay, GetEnvironment().cloudCoverage,
		          static_cast<float>(SDL_GetTicks()) / 1000.0f,
		          GetEnvironment().auroraIntensity, GetEnvironment().nebulaColor,
		          GetEnvironment().nebulaIntensity, GetEnvironment().auroraColor,
		          GetEnvironment().milkyWayIntensity, windVec);
	};

	if (m_renderWorld.objects.empty())
	{
		drawSky();
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
	if (m_sortedIndices.empty())
	{
		drawSky(); // nothing visible — fill the whole background with sky
		return;
	}

	[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_scenePipeline];
	[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_sceneDepthState];
	[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];

	// Shadow map on fragment texture/sampler slot 1 (filled by EncodeShadowMap).
	const bool shadows = m_renderWorld.shadow.enabled && m_shadowDepthTex;
	if (shadows)
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_shadowDepthTex atIndex:1];
	else
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_dummyTexture atIndex:1];
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

	// ── Lights (clamped to the shader's 8) ──────────────────────────────────
	// Kept at function scope so the transparency pass below can re-bind it after
	// the sky pass clobbers the fragment buffer.
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
	scene.lightVP       = kMetalClipFix * m_renderWorld.shadow.viewProj;
	scene.shadowEnabled = shadows ? 1 : 0;
	scene.sunDir        = glm::vec4(sunDir, 0.0f);
	scene.ambient       = glm::vec4(m_renderWorld.ambient, 0.0f);
	scene.fog           = glm::vec4(GetEnvironment().fogDensity,
	                                GetEnvironment().fogHeightFalloff, 0.0f, 0.0f);
	scene.viewport      = glm::vec4(static_cast<float>(width), static_cast<float>(height),
	                                ssaoActive ? 1.0f : 0.0f, 0.0f);
	[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];

	// Transparent (opacity < 1) draws collected during the opaque loop and replayed
	// sorted back-to-front, alpha-blended, after the sky.
	struct TPDraw { UnlitUniforms u; void* vbuf; void* ibuf; NSUInteger indexCount; void* tex; float distSq; };
	std::vector<TPDraw> transparent;
	const glm::vec3 camPos = m_renderWorld.camera.position;

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
			}
			u.pbr = glm::vec4(cMetallic, cRoughness, cOpacity, 0.0f);

			// Resolve the mesh; entities without one fall back to the default cube.
			if (!meshValid || dc.meshAssetId != lastMeshId)
			{
				cMesh      = ResolveMesh(dc.meshAssetId);
				lastMeshId = dc.meshAssetId; meshValid = true;
			}
			const GpuMesh* drawMesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!drawMesh) continue;
			id<MTLBuffer> vertexBuf = (__bridge id<MTLBuffer>)drawMesh->vertexBuf;
			id<MTLBuffer> indexBuf  = (__bridge id<MTLBuffer>)drawMesh->indexBuf;
			NSUInteger    indexCount = (NSUInteger)drawMesh->indexCount;
			void*         meshTex = drawMesh->texture;

			void* effectiveTex = cHasOverride ? cOverrideTex : meshTex;
			void* texPtr = effectiveTex ? effectiveTex : m_dummyTexture;
			id<MTLTexture> texture = (__bridge id<MTLTexture>)texPtr;
			u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, 0, 0, 0);

			// Base tint: material baseColor if assigned, else white when textured
			// (texture unchanged) or the flat fallback color when not.
			glm::vec3 baseColor = cBaseColor;
			if (!cHasMat)
				baseColor = effectiveTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);
			u.color = glm::vec4(baseColor, 1.0f);

			if (cOpacity < 0.999f)
			{
				const glm::vec3 d = glm::vec3(dc.transform[3]) - camPos;
				transparent.push_back({ u, (__bridge void*)vertexBuf, (__bridge void*)indexBuf,
				                        indexCount, texPtr, glm::dot(d, d) });
				continue; // drawn in the transparency pass below
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

	// ── Skinned geometry: drawn after opaque, before sky so they occlude the background.
	EncodeSkinnedObjects(renderEncoder, viewProj, shadows, &scene);

	// Sky LAST — fills the background pixels the geometry didn't cover.
	drawSky();

	// ── Transparency pass: sorted, alpha-blended draws over the opaque scene +
	// sky. Back-to-front; depth-tested against the opaque geometry but no depth
	// write (reuses the sky's LessEqual/no-write state). The sky pass clobbered the
	// fragment bindings, so re-bind the scene's shadow/ambient/AO state + uniforms.
	if (!transparent.empty())
	{
		std::sort(transparent.begin(), transparent.end(),
		          [](const TPDraw& a, const TPDraw& b) { return a.distSq > b.distSq; });
		[encoder setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)m_sceneBlendPipeline];
		[encoder setDepthStencilState:(__bridge id<MTLDepthStencilState>)m_skyDepthState]; // LessEqual, no write
		[encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:0];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(shadows ? m_shadowDepthTex : m_dummyTexture) atIndex:1];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:1];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)m_skyEnvCube atIndex:2];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:2];
		[encoder setFragmentTexture:(__bridge id<MTLTexture>)(ssaoActive ? m_ssaoResult : m_dummyTexture) atIndex:3];
		[encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)m_linearSampler atIndex:3];
		for (const TPDraw& t : transparent)
		{
			[encoder setVertexBuffer:(__bridge id<MTLBuffer>)t.vbuf offset:0 atIndex:0];
			[encoder setVertexBytes:&t.u length:sizeof(t.u) atIndex:1];
			[encoder setFragmentTexture:(__bridge id<MTLTexture>)t.tex atIndex:0];
			[encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                    indexCount:t.indexCount
			                     indexType:MTLIndexTypeUInt32
			                   indexBuffer:(__bridge id<MTLBuffer>)t.ibuf
			             indexBufferOffset:0];
		}
	}

	// GPU weather particles: simulated by the compute pass (EncodeFrame), drawn here
	// as alpha-blended billboards over the opaque scene + sky.
	DrawGpuParticles(renderEncoder, viewProj, m_renderWorld.camera.position);
}

void MetalRenderer::EncodeFrame(SDL_Window* sdlWin, WindowTarget& target, bool isPrimary)
{
	@autoreleasepool
	{
		if (isPrimary)
		{
			AgeRetiredTextures();

			// Release cached GPU buffers for any mesh invalidated since last frame
			// (e.g. sculpted terrain). In-flight GPU work may reference them, so
			// release via CFBridgingRelease (ARC autoreleasepool handles safety here).
			for (const HE::UUID& id : m_pendingMeshInvalidations)
				if (auto it = m_meshCache.find(id); it != m_meshCache.end())
				{
					if (it->second.vertexBuf) CFBridgingRelease(it->second.vertexBuf);
					if (it->second.indexBuf)  CFBridgingRelease(it->second.indexBuf);
					m_meshCache.erase(it);
				}
			m_pendingMeshInvalidations.clear();
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

		// ── Shadow map + scene → HDR target + offscreen tonemap ─────────────
		// Encoded before acquiring the drawable so the editor viewport texture
		// is produced even when the window has no drawable (occluded/background).
		// Only the swapchain present below needs the drawable.
		if (isPrimary)
			EncodeShadowMap((__bridge void*)cmdBuf);

		// Step the GPU weather-particle pool once per frame (primary only), before the
		// scene render encoder reads it. Metal tracks the compute→vertex dependency on
		// the shared buffer within this command buffer. No-op when the path is disabled.
		if (isPrimary)
			SimulateGpuParticles((__bridge void*)cmdBuf);

		const bool offscreen = isPrimary && m_viewportReqW > 0 && m_viewportReqH > 0;
		if (isPrimary)
		{
			const int sceneW = offscreen ? (int)m_viewportReqW : pw;
			const int sceneH = offscreen ? (int)m_viewportReqH : ph;
			EnsureHDRTarget(sceneW, sceneH);

			// SSAO occlusion (its own pre-pass + encoders) before the shading pass,
			// so the scene shader can darken its ambient. Skipped (zero cost) off.
			if (m_ssaoEnabled) EncodeSSAO((__bridge void*)cmdBuf, sceneW, sceneH);
			else               m_ssaoResult = nullptr;

			// Scene → RGBA16Float HDR target.
			MTLRenderPassDescriptor* hdrPass = [MTLRenderPassDescriptor renderPassDescriptor];
			hdrPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_hdrColor;
			hdrPass.colorAttachments[0].loadAction  = MTLLoadActionClear;
			hdrPass.colorAttachments[0].storeAction = MTLStoreActionStore;
			hdrPass.colorAttachments[0].clearColor  = MTLClearColorMake(0.18, 0.18, 0.20, 1.0);
			hdrPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
			hdrPass.depthAttachment.loadAction  = MTLLoadActionClear;
			hdrPass.depthAttachment.storeAction = MTLStoreActionDontCare;
			hdrPass.depthAttachment.clearDepth  = 1.0;

			id<MTLRenderCommandEncoder> sceneEncoder =
				[cmdBuf renderCommandEncoderWithDescriptor:hdrPass];
			EncodeScene((__bridge void*)sceneEncoder, sceneW, sceneH);
			// Debug lines on top of the opaque scene, still in the HDR pass.
			if (!m_debugLines.empty())
			{
				const glm::mat4 vp = m_renderWorld.camera.projection * m_renderWorld.camera.view;
				EncodeDebugLines((__bridge void*)sceneEncoder, vp);
			}
			[sceneEncoder endEncoding];

			// Bright-pass + blur the HDR target into the half-res bloom buffer;
			// the tonemap below composites it back in. Skipped when bloom is
			// disabled (m_bloomResult stays null → no glow).
			m_bloomResult = m_bloomEnabled ? EncodeBloom((__bridge void*)cmdBuf, sceneW, sceneH)
			                               : nullptr;

			// Tonemap HDR → LDR intermediate; FXAA reads it next (for both the editor
			// viewport and the direct-to-drawable path). m_hdrDepth is a DontCare
			// depth so the (depth-carrying) tonemap pipeline stays valid.
			EnsureLdrTarget(sceneW, sceneH);
			{
				MTLRenderPassDescriptor* tmPass = [MTLRenderPassDescriptor renderPassDescriptor];
				tmPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_ldrColor;
				tmPass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				tmPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				tmPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_hdrDepth;
				tmPass.depthAttachment.loadAction  = MTLLoadActionDontCare;
				tmPass.depthAttachment.storeAction = MTLStoreActionDontCare;
				id<MTLRenderCommandEncoder> tmEncoder =
					[cmdBuf renderCommandEncoderWithDescriptor:tmPass];
				EncodeTonemap((__bridge void*)tmEncoder);
				[tmEncoder endEncoding];
			}

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
				EncodeFxaa((__bridge void*)fxEncoder, sceneW, sceneH);
				EncodeUIPass((__bridge void*)fxEncoder, sceneW, sceneH);
				[fxEncoder endEncoding];
			}
			else if (m_viewportColor)
				DestroyViewportTarget();
		}

		// ── Swapchain pass (direct-mode tonemap and/or overlay) ─────────────
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (drawable)
		{
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

		[cmdBuf commit];
	}
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
		id<MTLBuffer> buf = [device newBufferWithLength:(NSUInteger)count * 8 * sizeof(float)
		                                       options:MTLResourceStorageModeShared];
		m_particleBuffer = (void*)CFBridgingRetain(buf);
	}
	m_particleCapacity = count;
	m_particleSeeded   = false;
}

void MetalRenderer::SeedParticleBuffer(int count)
{
	// Pre-distribute the pool down the fall column (same scheme as the GL backend):
	// seed = (i+0.5)/count so step(seed,coverage) keeps exactly a `coverage` fraction.
	const GpuParticleParams& p = m_gpuParticleParams;
	float* d = (float*)[(__bridge id<MTLBuffer>)m_particleBuffer contents];
	const float top = p.cameraPos.y + p.boxTop;
	uint32_t rng = 0x9E3779B9u;
	auto frand = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); };
	for (int i = 0; i < count; ++i)
	{
		const float seed = (i + 0.5f) / static_cast<float>(count);
		const float y    = p.groundLevel + frand() * std::max(top - p.groundLevel, 1.0f);
		float* e = &d[static_cast<size_t>(i) * 8];
		e[0] = p.cameraPos.x + (frand() * 2.0f - 1.0f) * p.boxHalf;  // p0 = pos.xyz, life
		e[1] = y;
		e[2] = p.cameraPos.z + (frand() * 2.0f - 1.0f) * p.boxHalf;
		e[3] = (y - p.groundLevel) / std::max(p.fallSpeed, 0.01f);
		e[4] = p.windVec.x * (p.isSnow ? 0.3f : 1.2f);              // p1 = vel.xyz, seed
		e[5] = -p.fallSpeed;
		e[6] = p.windVec.z * (p.isSnow ? 0.3f : 1.2f);
		e[7] = seed;
	}
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

IRenderer::Capabilities MetalRenderer::GetCapabilities() const
{
	return { true, true, true, true /* supportsGpuParticles */ };
}

void MetalRenderer::SetGpuParticleParams(const GpuParticleParams& p)
{
	m_gpuParticleParams = p;
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
