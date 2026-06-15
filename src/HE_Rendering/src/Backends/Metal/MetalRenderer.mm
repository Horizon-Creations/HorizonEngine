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
// so hardware trilinear filtering reproduces the old smoothstep value noise.
// Identical to the OpenGL backend's BuildSkyNoise3D.
static std::vector<uint16_t> BuildSkyNoise3D(int n)
{
	auto hash = [](glm::vec3 p) {
		p = glm::fract(p * 0.1031f);
		p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + 31.32f);
		return glm::fract((p.x + p.y) * p.z);
	};
	std::vector<uint16_t> d(static_cast<size_t>(n) * n * n);
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
				d[(static_cast<size_t>(z) * n + y) * n + x] = static_cast<uint16_t>(
					glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
	return d;
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
	// Slope-scaled bias: grows toward grazing sun angles (day-night sunsets) to
	// stop shadow acne, clamped so a high sun keeps crisp contact shadows.
	float ndl     = clamp(dot(N, L), 0.0, 1.0);
	float bias    = clamp(0.0016 * tan(acos(ndl)), 0.0005, 0.02);
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
	return mix(0.35, 1.0, vis);
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
                             sampler          shadowSmp [[sampler(1)]])
{
	float3 albedo = (in.hasTexture > 0.5)
		? baseColor.sample(smp, float2(in.uv.x, 1.0 - in.uv.y)).rgb * in.color
		: in.color;
	float3 N = normalize(in.normal);

	if (scene.lightCount == 0)
	{
		float3 L    = normalize(float3(0.5, 0.8, 0.6));
		float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		return float4(albedo * diff, 1.0);
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
	float3 ambDiff = skyColor(N, scene.sunDir.xyz)      * diffuseColor;
	float3 ambSpec = skyColor(Rrough, scene.sunDir.xyz) * specColor;
	float3 result  = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * in.roughness);
	// Flat ambient fill (never-black floor + overcast replacement for the
	// switched-off sun/moon light), applied to the diffuse albedo.
	result += scene.ambient.xyz * diffuseColor;

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
	return float4(result, 1.0);
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

// ─── Procedural skybox (drawn into the HDR target behind the scene) ─────────
static const char* kSkyMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct SkyOut { float4 position [[position]]; float2 ndc; };
struct SkyParams { float4x4 invViewProj; float4 sunDir; float4 sunColor; float4 params; float4 nebulaColor; float4 auroraColor; float4 wind; }; // params: x=timeOfDay y=coverage z=time w=aurora; nebulaColor.w=nebula intensity; auroraColor.w=milkyWay; wind.xyz=cloud drift/s

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
constant float kCloudBase = 1.0;
constant float kCloudTop  = 2.0;
// Cloud drift direction/speed comes from the user wind control (SkyParams.wind),
// passed down as a parameter so the noise field scrolls the clouds across the sky.
// Rounded vertical density taper so the slab reads as puffy bodies, not a sheet.
float cloudHeightGrad(float y)
{
	float hf = clamp((y - kCloudBase) / (kCloudTop - kCloudBase), 0.0, 1.0);
	return smoothstep(0.0, 0.25, hf) * (1.0 - smoothstep(0.65, 1.0, hf));
}
// Full density at a world point: animated 3D fbm + detail erosion, thresholded by
// the coverage slider, shaped by the slab height. time = continuous wall clock.
float cloudDensity(float3 pos, float time, float coverage, float3 wind,
                   texture3d<float> noiseTex, sampler noiseSamp)
{
	float3 p     = pos * 0.85 + wind * time;
	float  morph = time * 0.030;                      // slow in-place forming/dissolving
	float  base  = starFbm3(p + float3(0.0, morph, 0.0), 4, noiseTex, noiseSamp);
	float  detail= starFbm3(p * 3.1 + float3(morph, 0.0, 0.0), 2, noiseTex, noiseSamp);
	base        -= 0.08 * detail;                      // gentle erosion (keep edges soft)
	float  lo    = mix(0.95, 0.05, clamp(coverage, 0.0, 1.0));
	float  d     = smoothstep(lo, lo + 0.30, base);    // wider ramp = fuller, less torn
	return d * cloudHeightGrad(pos.y);
}
// Cheaper density for the sun light-march (skips the detail octave).
float cloudShadowDensity(float3 pos, float time, float coverage, float3 wind,
                         texture3d<float> noiseTex, sampler noiseSamp)
{
	float3 p     = pos * 0.85 + wind * time;
	float  morph = time * 0.030;
	float  base  = starFbm3(p + float3(0.0, morph, 0.0), 3, noiseTex, noiseSamp);
	float  lo    = mix(0.95, 0.05, clamp(coverage, 0.0, 1.0));
	float  d     = smoothstep(lo, lo + 0.30, base);
	return d * cloudHeightGrad(pos.y);
}
float3 applyClouds(float3 baseSky, float3 dir, float3 sunDir, float time, float coverage, float3 sunColor, float3 wind,
                   texture3d<float> noiseTex, sampler noiseSamp)
{
	if (coverage <= 0.0) return baseSky;          // clear sky → skip the whole raymarch
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

	// March the view ray through the cloud slab between base and top heights.
	float s0 = kCloudBase / max(dir.y, 1e-3);
	float s1 = kCloudTop  / max(dir.y, 1e-3);
	const int N = 5;
	float ds = (s1 - s0) / float(N);

	// Day/night/dusk drive the cloud colour (independent of the drift clock).
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	float  T = 1.0;                                // transmittance along the view ray
	float3 L = float3(0.0);                        // accumulated in-scattered colour
	for (int i = 0; i < N; ++i)
	{
		float  s   = s0 + (float(i) + 0.5) * ds;
		float3 pos = dir * s;
		float  dens = cloudDensity(pos, time, coverage, wind, noiseTex, noiseSamp);
		if (dens > 0.001)
		{
			// Light-march toward the sun: Beer's-law self-shadowing.
			float shadow = 0.0;
			for (int j = 1; j <= 2; ++j)
				shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.22), time, coverage, wind, noiseTex, noiseSamp);
			float sun    = exp(-shadow * 1.1);
			float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
			float lit    = sun * powder;

			// Higher-contrast shading: dark shaded base, sun-coloured lit tops.
			float3 dayCol   = mix(float3(0.30, 0.33, 0.40), sunColor * 1.15, lit);
			float3 nightCol = mix(float3(0.04, 0.05, 0.09), float3(0.18, 0.21, 0.30), lit);
			float3 cloudCol = mix(nightCol, dayCol, day);
			float3 duskTop  = sunColor * float3(1.25, 0.55, 0.28);
			cloudCol = mix(cloudCol, duskTop, dusk * lit * 0.85);
			// Silver/golden lining: thin, back-lit edges toward the sun glow as the
			// sunlight scatters through them (gentle so wisps don't tear).
			float toSun = max(dot(dir, sunDir), 0.0);
			cloudCol += sunColor * (pow(toSun, 8.0) * sun * 0.45 * max(day, dusk));
			// Cheap vertical depth: tops catch the light (bright crown), the base
			// sits in self-shadow (darker, cooler) — fakes the volumetric
			// "cauliflower" relief from just the sample's height in the slab.
			float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
			cloudCol *= mix(0.55, 1.12, hTone);
			cloudCol += float3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.5);

			float a = clamp(dens * ds * 2.4, 0.0, 1.0);
			L += T * a * cloudCol;
			T *= exp(-a);
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
	float  band = exp(-bd * bd * 2.3);           // wide soft milky-way bias
	float3 P    = cN * 3.4;
	float  big  = starFbm3(P * 0.7 + 11.0, 4, noiseTex, noiseSamp);   // large clouds
	float  med  = starFbm3(P * 1.7 + 27.0, 3, noiseTex, noiseSamp);   // medium clumps
	float  fine = starFbm3(P * 4.0 + 41.0, 2, noiseTex, noiseSamp);   // fine mottle / embedded dust
	float  blob   = smoothstep(0.46, 0.74, big * 0.5 + med * 0.6);
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
	return col * (glow * 2.1 * horizon * night * intensity);
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
	float  phase  = across * 0.5 + wave;
	float  f      = abs(fract(phase) - 0.5);            // distance to the nearest ribbon
	float  ribbon = smoothstep(0.22, 0.48, f);
	float  stri   = cloudFbm(float2(along * 6.0 + time * 0.25, across * 1.2));
	float  curtain = ribbon * (0.45 + 0.55 * smoothstep(0.30, 0.80, stri));
	float  patches = 0.55 + 0.45 * smoothstep(0.25, 0.85,
	                cloudFbm(float2(along * 0.45 + time * 0.03, across * 0.4 + 9.0)));
	// Base colour low, shifting toward violet tips with elevation.
	float  hcol   = smoothstep(0.05, 0.60, dir.y);
	float3 topCol = auroraCol * float3(0.55, 0.40, 1.5);
	float3 col    = mix(auroraCol, topCol, hcol);
	float  fade   = smoothstep(0.03, 0.16, dir.y) * (1.0 - smoothstep(0.78, 1.0, dir.y));
	return col * (curtain * patches * fade * intensity * night * 2.4);
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
	float3 zenithNite = float3(0.012, 0.016, 0.05);
	float3 horizNite  = float3(0.03, 0.04, 0.10);
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
	sky += float3(0.04, 0.05, 0.08) * night;
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
	EnsureShadowResources();
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

	for (auto& [id, tex] : m_materialTexCache)
		if (tex) CFBridgingRelease(tex);
	m_materialTexCache.clear();

	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyBloomTargets();
	DrainRetiredTextures();
	if (m_tonemapPipeline)      { CFBridgingRelease(m_tonemapPipeline);      m_tonemapPipeline = nullptr; }
	if (m_bloomBrightPipeline)  { CFBridgingRelease(m_bloomBrightPipeline);  m_bloomBrightPipeline = nullptr; }
	if (m_blurPipeline)         { CFBridgingRelease(m_blurPipeline);         m_blurPipeline = nullptr; }
	if (m_skyPipeline)          { CFBridgingRelease(m_skyPipeline);          m_skyPipeline = nullptr; }
	if (m_moonTexture)          { CFBridgingRelease(m_moonTexture);          m_moonTexture = nullptr; }
	if (m_dummyTexture)    { CFBridgingRelease(m_dummyTexture);    m_dummyTexture = nullptr; }
	if (m_linearSampler)   { CFBridgingRelease(m_linearSampler);   m_linearSampler = nullptr; }
	if (m_noiseTexture)    { CFBridgingRelease(m_noiseTexture);    m_noiseTexture = nullptr; }
	if (m_noiseSampler)    { CFBridgingRelease(m_noiseSampler);    m_noiseSampler = nullptr; }
	if (m_cubeVertexBuf)   { CFBridgingRelease(m_cubeVertexBuf);   m_cubeVertexBuf = nullptr; }
	if (m_cubeIndexBuf)    { CFBridgingRelease(m_cubeIndexBuf);    m_cubeIndexBuf = nullptr; }
	if (m_scenePipeline)   { CFBridgingRelease(m_scenePipeline);   m_scenePipeline = nullptr; }
	if (m_sceneDepthState) { CFBridgingRelease(m_sceneDepthState); m_sceneDepthState = nullptr; }
	if (m_shadowPipeline)  { CFBridgingRelease(m_shadowPipeline);  m_shadowPipeline = nullptr; }
	if (m_shadowDepthTex)  { CFBridgingRelease(m_shadowDepthTex);  m_shadowDepthTex = nullptr; }
	if (m_noDepthState)    { CFBridgingRelease(m_noDepthState);    m_noDepthState = nullptr; }
	if (m_skyDepthState)   { CFBridgingRelease(m_skyDepthState);   m_skyDepthState = nullptr; }

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

		// 3D value-noise volume the sky's starFbm3 samples (clouds + nebula), built
		// once on the CPU. R16Unorm + linear + repeat so it tiles seamlessly.
		constexpr int kNoiseN = 256;
		const std::vector<uint16_t> noise = BuildSkyNoise3D(kNoiseN);
		MTLTextureDescriptor* noiseDesc = [[MTLTextureDescriptor alloc] init];
		noiseDesc.textureType = MTLTextureType3D;
		noiseDesc.pixelFormat = MTLPixelFormatR16Unorm;
		noiseDesc.width = kNoiseN; noiseDesc.height = kNoiseN; noiseDesc.depth = kNoiseN;
		noiseDesc.usage = MTLTextureUsageShaderRead;
		noiseDesc.storageMode = MTLStorageModeShared;
		id<MTLTexture> noiseTex = [device newTextureWithDescriptor:noiseDesc];
		[noiseTex replaceRegion:MTLRegionMake3D(0, 0, 0, kNoiseN, kNoiseN, kNoiseN)
		            mipmapLevel:0
		                  slice:0
		              withBytes:noise.data()
		            bytesPerRow:kNoiseN * sizeof(uint16_t)
		          bytesPerImage:kNoiseN * kNoiseN * sizeof(uint16_t)];
		m_noiseTexture = (void*)CFBridgingRetain(noiseTex);

		MTLSamplerDescriptor* noiseSampDesc = [[MTLSamplerDescriptor alloc] init];
		noiseSampDesc.minFilter = MTLSamplerMinMagFilterLinear;
		noiseSampDesc.magFilter = MTLSamplerMinMagFilterLinear;
		noiseSampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
		noiseSampDesc.tAddressMode = MTLSamplerAddressModeRepeat;
		noiseSampDesc.rAddressMode = MTLSamplerAddressModeRepeat;
		m_noiseSampler = (void*)CFBridgingRetain([device newSamplerStateWithDescriptor:noiseSampDesc]);
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
	glm::vec3& outBaseColor, float& outMetallic, float& outRoughness)
{
	if (materialId == HE::UUID{} || !m_contentManager)
		return false;
	const MaterialAsset* mat = m_contentManager->getMaterial(materialId);
	if (!mat)
		return false; // not loaded yet — caller keeps defaults
	outBaseColor = glm::vec3(mat->baseColor[0], mat->baseColor[1], mat->baseColor[2]);
	outMetallic  = mat->metallic;
	outRoughness = mat->roughness;
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
	p.wind        = glm::vec4(wind, 0.0f);
	[enc setFragmentBytes:&p length:sizeof(p) atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
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
		scene.lightVP       = kMetalClipFix * m_renderWorld.shadow.viewProj;
		scene.shadowEnabled = shadows ? 1 : 0;
		scene.sunDir        = glm::vec4(sunDir, 0.0f);
		scene.ambient       = glm::vec4(m_renderWorld.ambient, 0.0f);
		scene.fog           = glm::vec4(GetEnvironment().fogDensity,
		                                GetEnvironment().fogHeightFalloff, 0.0f, 0.0f);
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

			// An explicit MaterialComponent override wins over the mesh's own
			// base-color texture when present and resolvable.
			void*      overrideTex = nullptr;
			const bool hasOverride = ResolveMaterialTexture(dc.materialAssetId, overrideTex);

			// PBR scalars from the material override; defaults otherwise.
			glm::vec3 baseColor(1.0f);
			float     metallic = 0.0f, roughness = 0.5f;
			const bool hasMat = ResolveMaterialParams(dc.materialAssetId, baseColor, metallic, roughness);
			u.pbr = glm::vec4(metallic, roughness, 0.0f, 0.0f);

			// Resolve the asset; entities without one fall back to the built-in cube.
			id<MTLBuffer> vertexBuf;
			id<MTLBuffer> indexBuf;
			NSUInteger    indexCount;
			void*         meshTex = nullptr;
			if (const GpuMesh* mesh = ResolveMesh(dc.meshAssetId))
			{
				vertexBuf  = (__bridge id<MTLBuffer>)mesh->vertexBuf;
				indexBuf   = (__bridge id<MTLBuffer>)mesh->indexBuf;
				indexCount = (NSUInteger)mesh->indexCount;
				meshTex    = mesh->texture;
			}
			else
			{
				vertexBuf  = (__bridge id<MTLBuffer>)m_cubeVertexBuf;
				indexBuf   = (__bridge id<MTLBuffer>)m_cubeIndexBuf;
				indexCount = (NSUInteger)m_cubeIndexCount;
			}

			void* effectiveTex = hasOverride ? overrideTex : meshTex;
			id<MTLTexture> texture = effectiveTex
				? (__bridge id<MTLTexture>)effectiveTex
				: (__bridge id<MTLTexture>)m_dummyTexture;
			u.flags = glm::vec4(effectiveTex ? 1.0f : 0.0f, 0, 0, 0);

			// Base tint: material baseColor if assigned, else white when textured
			// (texture unchanged) or the flat fallback color when not.
			if (!hasMat)
				baseColor = effectiveTex ? glm::vec3(1.0f) : glm::vec3(0.85f, 0.55f, 0.25f);
			u.color = glm::vec4(baseColor, 1.0f);

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

	// Sky LAST — fills the background pixels the geometry didn't cover.
	drawSky();
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

		id<MTLCommandQueue>  queue   = (__bridge id<MTLCommandQueue>)m_commandQueue;
		id<MTLCommandBuffer> cmdBuf  = [queue commandBuffer];

		// ── Shadow map + scene → HDR target + offscreen tonemap ─────────────
		// Encoded before acquiring the drawable so the editor viewport texture
		// is produced even when the window has no drawable (occluded/background).
		// Only the swapchain present below needs the drawable.
		if (isPrimary)
			EncodeShadowMap((__bridge void*)cmdBuf);

		const bool offscreen = isPrimary && m_viewportReqW > 0 && m_viewportReqH > 0;
		if (isPrimary)
		{
			const int sceneW = offscreen ? (int)m_viewportReqW : pw;
			const int sceneH = offscreen ? (int)m_viewportReqH : ph;
			EnsureHDRTarget(sceneW, sceneH);

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
			[sceneEncoder endEncoding];

			// Bright-pass + blur the HDR target into the half-res bloom buffer;
			// EncodeTonemap (offscreen or direct below) composites it back in.
			// Skipped when bloom is disabled (m_bloomResult stays null → no glow).
			m_bloomResult = m_bloomEnabled ? EncodeBloom((__bridge void*)cmdBuf, sceneW, sceneH)
			                               : nullptr;

			// Tonemap HDR → offscreen viewport texture (shown by the editor).
			if (offscreen)
			{
				EnsureViewportTarget();
				MTLRenderPassDescriptor* tmPass = [MTLRenderPassDescriptor renderPassDescriptor];
				tmPass.colorAttachments[0].texture     = (__bridge id<MTLTexture>)m_viewportColor;
				tmPass.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
				tmPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				tmPass.depthAttachment.texture     = (__bridge id<MTLTexture>)m_viewportDepth;
				tmPass.depthAttachment.loadAction  = MTLLoadActionDontCare;
				tmPass.depthAttachment.storeAction = MTLStoreActionDontCare;

				id<MTLRenderCommandEncoder> tmEncoder =
					[cmdBuf renderCommandEncoderWithDescriptor:tmPass];
				EncodeTonemap((__bridge void*)tmEncoder);
				[tmEncoder endEncoding];
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

			// Direct-to-window (game/no editor viewport): tonemap HDR → drawable.
			if (isPrimary && !offscreen)
				EncodeTonemap((__bridge void*)encoder);

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
