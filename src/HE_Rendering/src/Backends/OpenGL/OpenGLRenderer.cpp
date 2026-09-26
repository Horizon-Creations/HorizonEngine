#include "Backends/OpenGL/OpenGLRenderer.h"
#include <material/PreviewMesh.h> // shared preview primitives (sphere/cube/plane)
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/ParticleShaderTemplates.h>
#include <HorizonRendering/SsaoKernel.h>     // shared SSAO kernel + rotation noise
#include <HorizonRendering/SkyNoise3D.h>     // shared procedural sky/cloud noise volume
#include <HorizonRendering/SkyFrameParams.h> // shared cloud wind vector
#include <HorizonRendering/SkyShaderSource.h> // kSkyFS + kSkyFuncGLSL, shared with Vulkan/D3D
#include <HorizonRendering/LightPacking.h>   // shared GPU light packing
#include <HorizonRendering/GiInstanceSurface.h> // shared flat surface for a GI/reflection hit
#include <HorizonRendering/WeatherParticleSeed.h> // shared rain/snow pool seeding
#include <HorizonRendering/WorldPreviewGrid.h>    // shared world-preview ground + grid
#include <HorizonRendering/SkyEnvBake.h>     // shared CPU sky bake for the IBL ambient cubemap
#include <Renderer/UIFont.h>             // shared baked UI font atlas
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <Diagnostics/Logger.h>
#include <Diagnostics/EngineProfiler.h>
#include <JobSystem/JobSystem.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The index range a DrawCall covers on the mesh it ends up drawing. A whole-mesh
// draw (indexCount 0 — every one-section mesh, every draw before sections
// existed) spans the buffer; a section draw takes its own [offset, count),
// CLAMPED to the buffer: the draw loops substitute the default cube when the
// real mesh is not resident yet, and a range taken from the real asset must not
// read past the cube's EBO. Count is what glDrawElements takes, offset the byte
// pointer it takes.
struct GlIndexRange { int count; const void* offset; };
static inline GlIndexRange DrawIndexRange(const DrawCall& dc, int meshIndexCount)
{
	if (dc.indexCount == 0) return { meshIndexCount, nullptr };
	const uint32_t total = meshIndexCount > 0 ? static_cast<uint32_t>(meshIndexCount) : 0u;
	const uint32_t off   = std::min(dc.indexOffset, total);
	const uint32_t cnt   = std::min(dc.indexCount, total - off);
	return { static_cast<int>(cnt),
	         reinterpret_cast<const void*>(static_cast<uintptr_t>(off) * sizeof(uint32_t)) };
}

// Builds the six cube faces of the image-based-ambient environment map for the
// given sun direction (face = +X,-X,+Y,-Y,+Z,-Z in GL order). Returns tightly
// packed RGBA32F, faces back to back, faceN texels each.
//
// The maths (AtmoRaySphereCPU / AtmoScatterCPU / SkyColorCPU and the face
// direction switch) used to be an 88-line verbatim copy of the Metal backend's;
// it now lives in HorizonRendering/SkyEnvBake.h and both backends call it. What
// stays here is only the PARALLELISATION, which is GL-side on purpose: Metal
// bakes one face at a time on demand, this backend bakes all six at once.
static std::vector<float> BuildSkyEnvCube(int faceN, const glm::vec3& sunDir)
{
	std::vector<float> px(static_cast<size_t>(faceN) * faceN * 6 * 4);
	// Parallelize over the 6*faceN rows (face,row): each row is independent and
	// HE::SkyColorCPU is a pure function, so this is data-race-free. Uses the engine's
	// portable thread pool — NOT std::execution::par (which libc++/macOS lacks). At
	// 128² this serial bake was ~47 ms (a per-frame stall under day-night auto-advance);
	// parallel it is a few ms.
	parallel_for(static_cast<size_t>(6) * faceN, [&](size_t idx)
	{
		const int f = static_cast<int>(idx / faceN);
		const int t = static_cast<int>(idx % faceN);
		HE::BuildSkyEnvFaceRow(faceN, f, t, sunDir,
		                       &px[((static_cast<size_t>(f) * faceN + t) * faceN) * 4]);
	}, "SkyEnvBake");
	return px;
}

// ─── Embedded unlit shader ────────────────────────────────────────────────────
// GLSL 410: the macOS Core Profile ceiling — works everywhere we run.
static const char* kUnlitVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;
void main()
{
	vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
	vNormal     = mat3(uModel) * aNormal;
	vUV         = aUV;
	gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

// Blinn-Phong over up to 8 scene lights. Scenes without lights fall back to
// the fixed "headlight" so nothing renders black.
static const char* kUnlitFS = R"GLSL(
#version 410 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;

const int MAX_LIGHTS = 8;
uniform int  uLightCount;
uniform vec4 uLightPos[MAX_LIGHTS];    // xyz = position,  w = type (0 dir / 1 point / 2 spot)
uniform vec4 uLightDir[MAX_LIGHTS];    // xyz = direction, w = cos(spot half angle)
uniform vec4 uLightColor[MAX_LIGHTS];  // rgb = color,     w = intensity
uniform vec4 uLightParams[MAX_LIGHTS]; // x = range
uniform vec3 uCameraPos;

uniform vec3      uColor;       // base-color tint (material baseColor, or flat fallback)
uniform bool      uHasTexture;
uniform sampler2D uTexture;
uniform float     uMetallic;    // 0 dielectric … 1 metal
uniform float     uRoughness;   // 0 mirror … 1 fully rough
uniform float     uOpacity;     // surface alpha (1 = opaque; < 1 = blended pass)
uniform vec3      uSunDir;      // direction toward the sun (for image-based ambient)
uniform samplerCube uSkyEnv;   // baked skyColor cubemap (image-based ambient)
uniform vec3      uAmbient;     // flat ambient fill (never-black floor + overcast)
uniform float     uFogDensity;       // atmospheric fog amount (0 = off)
uniform float     uFogHeightFalloff; // >0 = fog pools near the ground
uniform sampler2D uAO;               // SSAO occlusion (screen-space); 1 = unoccluded
uniform vec2      uViewport;         // output size, for the screen-space AO lookup
uniform int       uSSAOEnabled;      // 1 = darken the ambient by SSAO
uniform float     uWetness;          // 0..1 wet-surface darken + gloss
uniform float     uSnow;             // 0..1 snow cover on up-facing surfaces
uniform float     uSpecAA;           // specular-AA strength (0 = off)
// true = this object IGNORES shadows (MeshComponent::receivesShadow == false).
// Stored inverted on purpose: an un-set uniform reads false, so any draw path
// that forgets it keeps shadowing rather than silently losing every shadow.
uniform bool      uNoShadow;

// ─── Specular anti-aliasing (docs/anti-aliasing-plan.md A6) ──────────────────
// A curved or normal-mapped surface packs more than one normal into one pixel;
// shading it from a single sample makes the highlight jump between frames. That
// aliasing lives in the SHADING, so no edge filter can reach it. Feeding the
// in-pixel normal variance into the roughness (Kaplanyan/Filament normal
// filtering) makes the lobe as wide as the surface the pixel actually covers.
// SYNC: byte-identical twins in the Metal scene/G-buffer shaders and in
// MaterialShaderLibrary's preamble (heSpecAARoughness).
float heSpecAARoughness(vec3 N, float perceptualRough)
{
	if (uSpecAA <= 0.0) return perceptualRough;
	vec3  du = dFdx(N);
	vec3  dv = dFdy(N);
	float variance = 0.15 * uSpecAA * (dot(du, du) + dot(dv, dv));
	// NOT named `kernel`: that is a reserved function qualifier in MSL, and the
	// twins are kept identical, so the name is avoided in all of them.
	float kernelRough = min(2.0 * variance, 0.25);          // cap: never fully diffuse
	float alpha       = perceptualRough * perceptualRough;  // GGX alpha
	float widened     = clamp(alpha * alpha + kernelRough, 0.0, 1.0);
	return max(perceptualRough, sqrt(sqrt(widened)));
}

// ── Ray-traced GI (GL 4.3 compute port; samplers/uniforms are 4.1-safe, the
// compute kernels that FILL them are gated on m_giSupported). When enabled,
// the mask replaces the CSM lookup and the probe atlases replace the
// AO-gated IBL ambient — mirrors the Metal fragmentMain branches.
uniform int       uGIEnabled;
uniform sampler2D uGIShadow;      // half-res screen-space shadow mask
uniform sampler2D uGIIrr;         // DDGI irradiance atlas (RGBA16F)
uniform sampler2D uGIVis;         // DDGI visibility atlas (RG16F)
uniform sampler2D uGILocal;       // half-res local-light visibility mask (1 channel per light, first 4)
uniform vec4      uGIGridOrigin;  // xyz = grid origin, w = spacing
uniform vec4      uGIGridCounts;  // xyz = probe counts, w = probesPerRow
uniform float     uGIIntensity;

// ── Ray-traced GI reflections (docs/gi-reflections-plan.md §10) ──────────────
// Half-res trace result, sampled per gl_FragCoord like uAO/uGIShadow. Its own
// toggle, independent of uGIEnabled: the pass needs the BVH, not the probes.
uniform sampler2D uGIRefl;        // rgb = radiance along the mirror ray, a = confidence
uniform vec4      uGIReflParams;  // x = intensity, y = max roughness, z = 1 → bound and active

// ── Screen-space reflections, forward path (docs/ssr-cross-backend-plan.md A5)
// The half-res trace result, sampled per gl_FragCoord like uGIRefl and sitting
// one stage BELOW it in the same cascade. Its own toggle: SSR needs the
// reflection pre-pass and last frame's HDR copy, not the BVH.
uniform sampler2D uSSRFwd;        // rgb = reflected radiance, a = confidence
uniform vec4      uSSRParams;     // x = 1 → bound and active, y = intensity, z = max roughness

// shared skyColor() is injected at the marker below (CreateUnlitPipeline)
//#SKYFUNC#

// Signed-octahedral mapping (direction → texel UV) — must match the probe
// kernel's octDecode (kGiProbeCS) and the Metal octEncode byte-for-byte.
vec2 giOctEncode(vec3 n)
{
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

const int GI_PROBE_OCT = 8; // must match OpenGLRenderer::kGIProbeOctSize

// DDGI probe sampling — trilinear over the 8 surrounding probes × soft
// backface × Chebyshev visibility. Direct port of Metal's
// sampleDDGIIrradiance (same v1 notes: raw per-direction radiance, not
// cosine-preintegrated).
vec3 sampleDDGIIrradiance(vec3 P, vec3 N)
{
	int gx = int(uGIGridCounts.x), gy = int(uGIGridCounts.y), gz = int(uGIGridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
	int probesPerRow = max(1, int(uGIGridCounts.w));
	int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
	vec2 atlasSizeTexels = vec2(float(probesPerRow), float(probeRows)) * float(GI_PROBE_OCT);
	float spacing = max(uGIGridOrigin.w, 1e-4);

	vec3 gridSpace = (P - uGIGridOrigin.xyz) / spacing;
	vec3 base      = floor(gridSpace);
	vec3 fracP     = gridSpace - base;

	vec3  sumColor  = vec3(0.0);
	float sumWeight = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		vec3 offs = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		vec3 cell = base + offs;
		if (any(lessThan(cell, vec3(0.0))) ||
		    cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

		vec3 trilinear = mix(1.0 - fracP, fracP, offs);
		float weight = trilinear.x * trilinear.y * trilinear.z;
		if (weight <= 1e-5) continue;

		vec3 probePos   = uGIGridOrigin.xyz + cell * spacing;
		vec3 toProbe    = probePos - P;
		float dist      = max(length(toProbe), 1e-4);
		vec3 dirToProbe = toProbe / dist;

		weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

		vec2 tileOrigin = vec2(float(probeIndex % probesPerRow),
		                       float(probeIndex / probesPerRow)) * float(GI_PROBE_OCT);

		vec2 visUV = (tileOrigin + (giOctEncode(-dirToProbe) * 0.5 + 0.5) * float(GI_PROBE_OCT)) / atlasSizeTexels;
		vec2 visSample = texture(uGIVis, visUV).rg;
		float mean = visSample.x, mean2 = visSample.y;
		float variance = abs(mean2 - mean * mean);
		float chebyshev = 1.0;
		if (dist > mean)
		{
			float d = dist - mean;
			chebyshev = variance / (variance + d * d);
			chebyshev = chebyshev * chebyshev * chebyshev;
		}
		weight *= max(chebyshev, 0.05);

		vec2 irrUV = (tileOrigin + (giOctEncode(N) * 0.5 + 0.5) * float(GI_PROBE_OCT)) / atlasSizeTexels;
		sumColor  += texture(uGIIrr, irrUV).rgb * weight;
		sumWeight += weight;
	}
	return sumColor / max(sumWeight, 1e-4);
}

// Atmospheric fog / aerial perspective: blend the lit colour toward the sky in
// the fragment's view direction, so distant geometry melts into the horizon
// (and warms toward the sun at sunset, since the fog samples the same sky). The
// opacity is an analytic exponential height-fog integral along the view ray —
// density*exp(-falloff*y) integrated from the camera to the fragment — so fog
// pools low and thins with altitude. falloff == 0 → plain exp distance fog.
vec3 applyFog(vec3 color, vec3 camPos, vec3 worldPos, vec3 sunDir)
{
	if (uFogDensity <= 0.0) return color;
	vec3  ray  = worldPos - camPos;
	float dist = length(ray);
	float k    = uFogHeightFalloff * ray.y;
	float t    = (abs(k) > 1e-4) ? (1.0 - exp(-k)) / k : 1.0; // mean height attenuation
	float optical = uFogDensity * dist * exp(-uFogHeightFalloff * camPos.y) * t;
	float f       = 1.0 - exp(-optical);
	vec3  fogCol  = skyColor(ray / max(dist, 1e-4), sunDir);
	return mix(color, fogCol, clamp(f, 0.0, 1.0));
}

// Directional-light cascaded shadow maps (CSM). uShadowMap is a depth-texture
// ARRAY (one layer per cascade); the cascade is picked per fragment by its planar
// camera-forward distance. The cascade matrices arrive in GL clip (z∈[-1,1]), so
// no clip-fix is needed here — this mirrors the Metal backend's shadowFactor()
// (Metal applies a [-1,1]→[0,1] clip-fix to the same extractor matrices instead).
const int CSM_CASCADES = 3;
uniform sampler2DArray uShadowMap;
uniform int   uShadowEnabled;
uniform mat4  uCascadeVP[CSM_CASCADES]; // per-cascade light view-proj (GL clip)
uniform vec4  uCascadeSplits;           // xyz = cascade far distance (view space); w = count
uniform vec3  uCameraFwd;               // world forward, for planar view-Z cascade select
uniform int   uShadowDebug;             // 1 = tint fragments by cascade index
uniform int   uUnlit;                   // 1 = base colour only (Unlit / Wireframe view mode)
// Receiver depth bias (project ShadowSettings): x = slope-scaled factor,
// y = minimum. Defaults (0.0008, 0.0002) are the literals this used to carry.
uniform vec2  uShadowBias;

out vec4 FragColor;

// Cascaded shadows: pick the first cascade whose far distance covers the fragment
// (by planar camera-forward distance), project into that cascade's light clip and
// 3×3-PCF sample its layer of the shadow-map array. outCascade returns the chosen
// index (for the debug tint).
float computeShadow(vec3 worldPos, vec3 N, vec3 L, out int outCascade)
{
	outCascade = 0;
	if (uShadowEnabled == 0) return 1.0;

	// Planar view-space depth along the camera forward — matches the cascade splits
	// (planar view-Z far distances, NOT euclidean radius). Euclidean distance here
	// would push screen-edge pixels into a too-coarse cascade → dropouts.
	float viewDist = dot(worldPos - uCameraPos, uCameraFwd);
	int count = int(uCascadeSplits.w);
	int c = (count > 0) ? count - 1 : 0;
	if      (count > 0 && viewDist < uCascadeSplits.x) c = 0;
	else if (count > 1 && viewDist < uCascadeSplits.y) c = 1;
	else if (count > 2 && viewDist < uCascadeSplits.z) c = 2;
	c = clamp(c, 0, CSM_CASCADES - 1);
	outCascade = c;

	// Normal-offset bias scaled by cascade — coarser (farther) cascades have larger
	// texels and need a bigger offset to avoid acne.
	vec4 lp = uCascadeVP[c] * vec4(worldPos + N * (0.06 * float(c + 1)), 1.0);
	vec3 p  = lp.xyz / lp.w;
	p = p * 0.5 + 0.5;                       // NDC [-1,1] → [0,1] (GL convention)
	if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
		return 1.0;                          // outside this cascade → lit
	// Slope-scaled residual depth bias for sub-texel precision, scaled by cascade.
	float ndl  = clamp(dot(N, L), 0.0, 1.0);
	float bias = clamp(uShadowBias.x * tan(acos(ndl)), uShadowBias.y, 0.02) * float(c + 1);
	// 3×3 PCF over the chosen cascade's array layer. textureSize on a sampler2DArray
	// returns ivec3 (w,h,layers); .xy is the per-layer 2D size.
	vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float cd = texture(uShadowMap, vec3(p.xy + vec2(x, y) * texel, float(c))).r;
			vis += (p.z - bias > cd) ? 0.0 : 1.0;
		}
	vis /= 9.0;
	// No direct-light floor in shadow — the IBL + flat ambient already provide
	// the minimum indirect illumination. A non-zero floor bleeds warm sun colour
	// into fully-shadowed areas and causes the yellow/orange cast at dusk.
	return vis;
}

// Local-light (point/spot) shadow atlas: one sampler2DArray shared by all
// shadow-casting point/spot lights. A spot light owns 1 perspective layer, a
// point light 6 cube-face layers (+X −X +Y −Y +Z −Z — face picked here by the
// fragment→light vector's major axis). The light's first layer index rides in
// uLightParams[i].y (-1 = casts no shadow). Mirrors Metal's localShadowFactor.
const int LOCAL_SHADOW_LAYERS = 16;
uniform sampler2DArray uLocalShadowMap;
uniform mat4 uLocalShadowVP[LOCAL_SHADOW_LAYERS]; // per-layer light view-proj (GL clip)

// Cloud-shadow transmittance map (unit 19): the sky's procedural cloud layer
// evaluated once per frame over a world-space XZ region around the camera (the
// sky program's uCloudShadowPass mode). uCloudShadowA: xy = region origin
// (world XZ), z = 1/region size, w = slab mid-plane Y. uCloudShadowB.x =
// strength (0 = off).
uniform sampler2D uCloudShadowMap;
uniform vec4 uCloudShadowA;
uniform vec4 uCloudShadowB;

// Cloud-shadow visibility for the directional light (1 = fully lit): project
// the fragment along L (toward the light) onto the cloud slab's mid-plane and
// sample the map; edge fade hides the region border. SYNC: mirror of
// heCloudShadowFactor (MaterialShaderLibrary preamble) and Metal fragmentMain's
// cloudShadowFactor — change one, change all.
float cloudShadowFactor(vec3 worldPos, vec3 L)
{
	float s = uCloudShadowB.x;
	if (s <= 0.0 || L.y <= 0.05) return 1.0;
	float t = (uCloudShadowA.w - worldPos.y) / L.y;
	if (t <= 0.0) return 1.0;
	vec2 uv = (worldPos.xz + L.xz * t - uCloudShadowA.xy) * uCloudShadowA.z;
	vec2 e = min(uv, vec2(1.0) - uv);
	float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
	if (edge <= 0.0) return 1.0;
	float T = texture(uCloudShadowMap, uv).r;
	return mix(1.0, T, s * edge);
}

float localShadowFactor(int i, vec3 worldPos, vec3 N)
{
	int base = int(uLightParams[i].y);
	if (base < 0) return 1.0;
	int layer = base;
	if (int(uLightPos[i].w) == 1) // point: major-axis cube-face pick
	{
		vec3 d = worldPos - uLightPos[i].xyz;
		vec3 a = abs(d);
		int face;
		if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
		else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
		else                               face = (d.z > 0.0) ? 4 : 5;
		layer = base + face;
	}
	vec3  toL = normalize(uLightPos[i].xyz - worldPos);
	float ndl = clamp(dot(N, toL), 0.0, 1.0);
	vec4 lp = uLocalShadowVP[layer] * vec4(worldPos + N * 0.02, 1.0);
	if (lp.w <= 0.0) return 1.0;             // behind the light's near plane
	vec3 p = lp.xyz / lp.w;
	p = p * 0.5 + 0.5;                       // GL NDC [-1,1] → [0,1]
	vec2 texel = 1.0 / vec2(textureSize(uLocalShadowMap, 0).xy);
	if (p.z > 1.0 || p.z < 0.0
	    || any(lessThan(p.xy, texel)) || any(greaterThan(p.xy, 1.0 - texel)))
		return 1.0;
	float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float cd = texture(uLocalShadowMap, vec3(p.xy + vec2(x, y) * texel, float(layer))).r;
			vis += (p.z - bias > cd) ? 0.0 : 1.0;
		}
	return vis / 9.0;
}

void main()
{
	vec3 albedo = uHasTexture ? texture(uTexture, vUV).rgb * uColor : uColor;
	// Unlit view mode: the material's base colour and nothing else — before
	// the weather, which is lighting's business too. Twin of Metal's scene.unlit.
	if (uUnlit != 0)
	{
		FragColor = vec4(albedo, uOpacity);
		return;
	}
	vec3 N      = normalize(vNormal);

	// ── Weather ground response ──────────────────────────────────────────────
	// Snow lies on up-facing surfaces (matte white); wetness darkens + glosses the
	// rest. Driven by the EnvironmentComponent (preset or manual); 0 = no effect.
	float snowMask = smoothstep(0.25, 0.75, clamp(N.y, 0.0, 1.0)) * clamp(uSnow, 0.0, 1.0);
	float wet      = clamp(uWetness, 0.0, 1.0) * (1.0 - snowMask);
	albedo = mix(albedo, vec3(0.90, 0.93, 0.97), snowMask);
	albedo *= (1.0 - 0.30 * wet);
	// Wet = glossier (sharper highlight); snow = matte.
	float wRough = mix(uRoughness, 0.08, wet);
	wRough = mix(wRough, 0.85, snowMask);
	// Specular AA (docs/anti-aliasing-plan.md A6): widen the roughness by how much
	// this pixel's normal turns inside itself, so a highlight on a curved or
	// normal-mapped surface stops crawling. uSpecAA.x = 0 → exact no-op.
	wRough = heSpecAARoughness(N, wRough);

	if (uLightCount == 0)
	{
		vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
		float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		FragColor  = vec4(albedo * diff, uOpacity);
		return;
	}

	// Metallic-roughness split: metals lose diffuse and tint the specular F0;
	// roughness widens + dims the Blinn-Phong highlight (cheap PBR stand-in).
	vec3  diffuseColor = albedo * (1.0 - uMetallic);
	vec3  specColor    = mix(vec3(0.04), albedo, uMetallic);
	float shininess    = mix(128.0, 8.0, wRough);
	float specScale    = mix(0.5, 0.03, wRough) + 0.25 * wet; // wet sheen
	specColor          = mix(specColor, vec3(0.08), wet);     // water-like F0 on wet ground

	vec3 V = normalize(uCameraPos - vWorldPos);

	// Image-based ambient from the procedural sky (replaces the flat floor):
	// diffuse from the surface normal, specular from the reflection vector
	// (bent toward the normal as roughness grows = crude prefilter).
	vec3 Rrough  = normalize(mix(reflect(-V, N), N, wRough));
	// Clamp the diffuse IBL lookup at least 5° above the horizon. Sampling near
	// or at the horizon (N.y ≈ 0) returns the warm/orange sunset band of the sky
	// even at noon. A floor of 0.1 keeps the sample safely in the cool sky dome.
	vec3 Nup     = normalize(vec3(N.x, max(N.y, 0.1), N.z));
	vec3 ambDiff = texture(uSkyEnv, Nup).rgb    * diffuseColor;
	// Fresnel (Schlick, roughness-aware — same term as heLitP, ssr-plan P4).
	float NdV = clamp(dot(N, V), 0.0, 1.0);
	vec3 fresnelSpec = specColor
		+ (max(vec3(1.0 - wRough), specColor) - specColor) * pow(1.0 - NdV, 5.0);
	vec3 envSpec = texture(uSkyEnv, Rrough).rgb;
	// FORWARD reflection cascade (sky → ray-traced GI reflections → SSR). SYNC:
	// this is the heLitP twin in MaterialShaderLibrary.cpp — a graph material
	// next to a built-in one must mix the sources identically and in the SAME
	// ORDER, or the same mirror changes appearance with the material type. The
	// traces carry no per-pixel roughness (the half-res pre-pass is flat per
	// draw), so the roughness fade lives here, with the exact shading value.
	if (uGIReflParams.z > 0.5)
	{
		vec4  rr   = texture(uGIRefl, gl_FragCoord.xy / uViewport);
		float fade = 1.0 - smoothstep(uGIReflParams.y * 0.7, uGIReflParams.y, wRough);
		envSpec = mix(envSpec, rr.rgb, rr.a * uGIReflParams.x * fade);
	}
	// Screen-space reflections sit ON TOP of the traced ones: where the screen
	// has the answer it is the sharper and cheaper of the two, and where it does
	// not (a = 0) the GI/sky term below stays untouched.
	if (uSSRParams.x > 0.5)
	{
		vec4  sr   = texture(uSSRFwd, gl_FragCoord.xy / uViewport);
		float fade = 1.0 - smoothstep(uSSRParams.z * 0.7, uSSRParams.z, wRough);
		envSpec = mix(envSpec, sr.rgb, sr.a * uSSRParams.y * fade);
	}
	vec3 ambSpec = envSpec * fresnelSpec;
	vec3 ambient = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * wRough);
	// Screen-space ambient occlusion darkens only the IBL indirect term in
	// crevices; the direct lighting added below is left untouched. 1.0 = fully lit.
	float ao = (uSSAOEnabled == 1) ? texture(uAO, gl_FragCoord.xy / uViewport).r : 1.0;
	// Flat ambient fill (never-black floor + overcast replacement) is intentionally
	// kept outside the AO product so SSAO over-darkening at grazing angles cannot
	// zero it out. It is the minimum guaranteed brightness on any surface.
	// GI replaces the AO-gated IBL term with probe-sampled indirect diffuse; the
	// flat floor stays in BOTH branches (never-black guarantee — probes bounce
	// only actual lights and go dark under full overcast/night). Specular IBL
	// (ambSpec) is kept either way — the GI slice is diffuse-only.
	vec3 result = (uGIEnabled == 1)
		? sampleDDGIIrradiance(vWorldPos, N) * diffuseColor * uGIIntensity
		      + ambSpec * (1.0 - 0.6 * wRough) + uAmbient * diffuseColor
		: ambient * ao + uAmbient * diffuseColor;

	int dbgCascade = 0;   // cascade chosen by the directional shadow (debug tint)
	int giLocalIdx = 0;   // counter over non-directional lights → local-mask channel
	for (int i = 0; i < uLightCount; ++i)
	{
		int   type  = int(uLightPos[i].w);
		vec3  L;
		float atten = 1.0;

		if (type == 0) // directional
			L = normalize(-uLightDir[i].xyz);
		else
		{
			vec3  d    = uLightPos[i].xyz - vWorldPos;
			float dist = max(length(d), 1e-4);
			L = d / dist;
			float range = max(uLightParams[i].x, 1e-4);
			atten = clamp(1.0 - dist / range, 0.0, 1.0);
			atten *= atten;
			if (type == 2) // spot cone
			{
				float c = dot(-L, normalize(uLightDir[i].xyz));
				float cosCone = uLightDir[i].w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
		}

		// Only the (first) directional light casts shadows. Explicit if (not a
		// ternary) so the `out` cascade index is written only on the directional
		// branch — strict GLSL compilers reject out-params inside a ?: selection.
		// GI replaces CSM entirely when active: the ray-traced mask is sampled at
		// the same screen-space UV convention the AO texture uses.
		float sh = 1.0;
		if (type == 0)
		{
			if (uGIEnabled == 1) sh = texture(uGIShadow, gl_FragCoord.xy / uViewport).r;
			else                 sh = computeShadow(vWorldPos, N, L, dbgCascade);
			// Cloud shadows multiply on top of both shadow sources — the
			// geometry shadow and the cloud layer occlude independently.
			sh *= cloudShadowFactor(vWorldPos, L);
		}
		else
		{
			// Local (point/spot) lights: shadow-mapped when the light casts
			// shadows (uLightParams[i].y = atlas base layer, set by the
			// extractor). When GI is active the ray-traced hard mask (first 4
			// local lights) is combined in via min() — the map covers lights
			// the mask can't.
			sh = localShadowFactor(i, vWorldPos, N);
			if (uGIEnabled == 1 && giLocalIdx < 4)
				sh = min(sh, texture(uGILocal, gl_FragCoord.xy / uViewport)[giLocalIdx]);
			giLocalIdx++;
		}
		// "Receives Shadow" off: the object is lit as if nothing occluded it.
		// Placed after BOTH branches so it covers every source at once — cascades,
		// GI sun/local masks, the cloud layer and the local atlas.
		if (uNoShadow) sh = 1.0;

		float diff = max(dot(N, L), 0.0);
		vec3  H    = normalize(L + V);
		float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;
		result += (diffuseColor * diff + specColor * spec)
		        * uLightColor[i].rgb * uLightColor[i].w * atten * sh;
	}
	result = applyFog(result, uCameraPos, vWorldPos, uSunDir);

	// Debug: tint each fragment by its shadow cascade (red / green / blue / yellow)
	// so the cascade split placement is verifiable at a glance. Mirrors Metal.
	if (uShadowDebug != 0 && uShadowEnabled != 0)
	{
		vec3 tint[4] = vec3[4](vec3(1.0, 0.4, 0.4), vec3(0.4, 1.0, 0.4),
		                       vec3(0.4, 0.6, 1.0), vec3(1.0, 1.0, 0.4));
		result *= tint[min(dbgCascade, 3)];
	}
	FragColor = vec4(result, uOpacity);
}
)GLSL";

// ─── Deferred G-buffer fragment (built-in PBR materials) ─────────────────────
// Writes surface ATTRIBUTES instead of shading them; the fullscreen resolve
// (MaterialShaderLibrary::deferredResolve → heLitP) lights them once per
// visible pixel. Weather/fog/IBL deliberately NOT applied here — they run in
// the resolve on these attributes, so the result matches forward by
// construction. Specular 0.5 = the dielectric F0 0.04 the built-in forward
// shader uses; emissive 0, material AO 1. Links against kUnlitVS (single) and
// kInstancedVS (instanced batches) — both emit the same varyings.
static const char* kGBufFS = R"GLSL(
#version 410 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;
uniform vec3      uColor;
uniform bool      uHasTexture;
uniform sampler2D uTexture;
uniform float     uMetallic;
uniform float     uRoughness;
uniform float     uSpecAA;          // specular-AA strength (0 = off), see A6
layout(location = 0) out vec4 oGB0; // rgb BaseColor, a Metallic
layout(location = 1) out vec4 oGB1; // rg oct Normal, b Roughness, a Specular
layout(location = 2) out vec4 oGB2; // rgb Emissive, a Material-AO
// Signed-octahedral mapping (Meyer et al. 2010) — same encode as the shared
// lighting preamble's heOctEncode, so the resolve's decode round-trips.
vec2 octEncode(vec3 n)
{
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}
// Specular AA (A6) — the G-buffer is the LAST place the fragment's own normal
// exists, so the widening has to happen here: the resolve only ever sees an
// encoded texel, whose derivative jumps at every silhouette. SYNC: twin of the
// scene shader's heSpecAARoughness above.
float heSpecAARoughness(vec3 N, float perceptualRough)
{
	if (uSpecAA <= 0.0) return perceptualRough;
	vec3  du = dFdx(N);
	vec3  dv = dFdy(N);
	float variance = 0.15 * uSpecAA * (dot(du, du) + dot(dv, dv));
	float kernelRough = min(2.0 * variance, 0.25);
	float alpha       = perceptualRough * perceptualRough;
	float widened     = clamp(alpha * alpha + kernelRough, 0.0, 1.0);
	return max(perceptualRough, sqrt(sqrt(widened)));
}
void main()
{
	vec3 albedo = uHasTexture ? texture(uTexture, vUV).rgb * uColor : uColor;
	vec3 N = normalize(vNormal);
	oGB0 = vec4(albedo, clamp(uMetallic, 0.0, 1.0));
	oGB1 = vec4(octEncode(N) * 0.5 + 0.5,
	            heSpecAARoughness(N, clamp(uRoughness, 0.0, 1.0)), 0.5);
	oGB2 = vec4(0.0, 0.0, 0.0, 1.0);
}
)GLSL";

// ─── Skinned vertex shader ────────────────────────────────────────────────────
// Identical shading to kUnlitVS/kUnlitFS but blends the vertex by up to 4 bone
// matrices before applying the model+MVP.  The fragment shader is shared with
// the unlit path (same uniforms), so only the vertex stage needs to change.
static const char* kSkinnedVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uvec4 aBoneIDs;
layout(location = 4) in vec4  aBoneWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uBoneMatrices[128];
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;
void main()
{
    mat4 skin = aBoneWeights.x * uBoneMatrices[aBoneIDs.x]
              + aBoneWeights.y * uBoneMatrices[aBoneIDs.y]
              + aBoneWeights.z * uBoneMatrices[aBoneIDs.z]
              + aBoneWeights.w * uBoneMatrices[aBoneIDs.w];
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vWorldPos   = (uModel * skinnedPos).xyz;
    vNormal     = mat3(uModel) * mat3(skin) * aNormal;
    vUV         = aUV;
    gl_Position = uMVP * skinnedPos;
}
)GLSL";

// ─── Skeletal-mesh-preview shaders (RenderSkeletalPreview) ──────────────────
// Deliberately separate from kSkinnedVS+kUnlitFS: the preview is an isolated
// offscreen target with no shadow map / SSAO / sky-IBL / fog bound, so it uses
// its own minimal fixed sun+ambient lighting (same philosophy as the material
// preview's own small Lighting UBO instead of the full scene pipeline).
static const char* kSkelPreviewVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uvec4 aBoneIDs;
layout(location = 4) in vec4  aBoneWeights;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform mat4 uBoneMatrices[128];
out vec3 vNormal;
out vec2 vUV;
void main()
{
    mat4 skin = aBoneWeights.x * uBoneMatrices[aBoneIDs.x]
              + aBoneWeights.y * uBoneMatrices[aBoneIDs.y]
              + aBoneWeights.z * uBoneMatrices[aBoneIDs.z]
              + aBoneWeights.w * uBoneMatrices[aBoneIDs.w];
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vNormal     = mat3(uModel) * mat3(skin) * aNormal;
    vUV         = aUV;
    gl_Position = uMVP * skinnedPos;
}
)GLSL";

static const char* kSkelPreviewFS = R"GLSL(
#version 410 core
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;
uniform vec3 uColor;
uniform bool uHasTex;
uniform sampler2D uTex;
// Same convention as the mesh preview: w == 0 means the fixed studio light.
uniform vec4 uSun;
uniform vec3 uSunColor;
uniform vec3 uAmbient;
void main()
{
    bool  lit = uSun.w > 0.0;
    vec3  L   = lit ? normalize(uSun.xyz) : normalize(vec3(0.45, 0.75, 0.55));
    vec3  lc  = lit ? uSunColor : vec3(1.0);
    vec3  amb = lit ? uAmbient  : vec3(0.35);
    vec3 N = normalize(vNormal);
    float diff = max(dot(N, L), 0.0);
    vec3 albedo = uHasTex ? texture(uTex, vUV).rgb * uColor : uColor;
    FragColor = vec4(albedo * (amb + lc * (lit ? diff : 0.65 * diff)), 1.0);
}
)GLSL";

// ─── Mesh-thumbnail shaders (RenderAssetThumbnail) ──────────────────────────
// The unskinned counterpart of kSkelPreview*: static meshes, skeletal meshes in
// bind pose, and the preview sphere for materials that have NO node graph (the
// built-in PBR ones — resolveMaterialShader finds nothing to compile for them,
// and a Content Browser tile still has to show something). Same sun direction
// and ambient as the skeletal preview so every thumbnail is lit alike; the
// highlight is driven by metallic/roughness so two flat materials that differ
// only in their PBR scalars still produce different tiles.
static const char* kMeshPreviewVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;
void main()
{
    vNormal     = mat3(uModel) * aNormal;
    vUV         = aUV;
    vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* kMeshPreviewFS = R"GLSL(
#version 410 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vWorldPos;
out vec4 FragColor;
uniform vec3  uColor;
uniform bool  uHasTex;
uniform sampler2D uTex;
uniform vec3  uCamPos;
uniform vec2  uPbr;   // x = metallic, y = roughness
// Sun for the world preview: xyz points TOWARD the light, w > 0 arms it.
// w == 0 keeps the fixed studio light every thumbnail was rendered with, so a
// cached tile does not change the day someone adds a sky to a preview.
uniform vec4  uSun;
uniform vec3  uSunColor;
uniform vec3  uAmbient;
void main()
{
    bool  lit = uSun.w > 0.0;
    vec3  L   = lit ? normalize(uSun.xyz) : normalize(vec3(0.45, 0.75, 0.55));
    vec3  lc  = lit ? uSunColor : vec3(1.0);
    vec3  amb = lit ? uAmbient  : vec3(0.32);
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    vec3 H = normalize(L + V);
    float diff  = max(dot(N, L), 0.0);
    float rough = clamp(uPbr.y, 0.05, 1.0);
    float spec  = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough))
                * (1.0 - rough) * mix(0.25, 1.0, clamp(uPbr.x, 0.0, 1.0));
    vec3 albedo = uHasTex ? texture(uTex, vUV).rgb * uColor : uColor;
    vec3 lightIn = amb + lc * (lit ? diff : 0.68 * diff);
    FragColor = vec4(albedo * lightIn + vec3(spec) * lc, 1.0);
}
)GLSL";

// Immediate-mode line pass for the bone overlay (joint markers drawn as tiny
// crosses + parent→child segments) — its own tiny program, since the shared
// DebugDrawBuffer pipeline targets the backbuffer scene, not an arbitrary
// offscreen preview FBO.
static const char* kSkelPreviewLineVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uMVP * vec4(aPos, 1.0); }
)GLSL";

static const char* kSkelPreviewLineFS = R"GLSL(
#version 410 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)GLSL";

// ─── Particle-preview shaders (RenderParticlePreview) ───────────────────────
// Camera-facing billboards, attribute-less (gl_VertexID picks one of 6 corner
// verts per instance — same "no per-vertex buffer" trick as the sky/fullscreen
// passes), one instance = one already-simulated particle (position/size/color/
// alpha resolved by the Particle Graph Editor's live preview, ParticleSystem::
// stepPool — this shader only draws, it never simulates).
static const char* kParticlePreviewVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3  iPos;
layout(location = 1) in float iSize;
layout(location = 2) in vec3  iColor;
layout(location = 3) in float iAlpha;
uniform mat4 uViewProj;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
out vec3  vColor;
out float vAlpha;
out vec2  vUV;
const vec2 kCorners[6] = vec2[](
    vec2(-1,-1), vec2(1,-1), vec2(1,1),
    vec2(-1,-1), vec2(1,1),  vec2(-1,1)
);
void main()
{
    vec2 corner = kCorners[gl_VertexID % 6];
    vec3 worldPos = iPos + (uCamRight * corner.x + uCamUp * corner.y) * (iSize * 0.5);
    gl_Position = uViewProj * vec4(worldPos, 1.0);
    vUV    = corner * 0.5 + 0.5;
    vColor = iColor;
    vAlpha = iAlpha;
}
)GLSL";

static const char* kParticlePreviewFS = R"GLSL(
#version 410 core
in vec3  vColor;
in float vAlpha;
in vec2  vUV;
out vec4 FragColor;
uniform bool      uHasTex;
uniform sampler2D uTex;
void main()
{
    vec4  texc  = uHasTex ? texture(uTex, vUV) : vec4(1.0);
    // No texture → soft circular sprite instead of a flat square, so a bare
    // particle system still reads as "particles" rather than "confetti".
    float shape = uHasTex ? texc.a : smoothstep(1.0, 0.0, length(vUV * 2.0 - 1.0));
    FragColor = vec4(vColor * texc.rgb, vAlpha * shape);
}
)GLSL";

// ─── GPU-instanced vertex shader ─────────────────────────────────────────────
// Per-instance model matrix supplied via a VBO at attribute locations 4–7
// (one mat4 = 4 × vec4, divisor = 1). The fragment shader is kUnlitFS (shared).
static const char* kInstancedVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 4) in vec4 aInstCol0;
layout(location = 5) in vec4 aInstCol1;
layout(location = 6) in vec4 aInstCol2;
layout(location = 7) in vec4 aInstCol3;
uniform mat4 uViewProj;
out vec3 vNormal;
out vec2 vUV;
out vec3 vWorldPos;
void main()
{
    mat4 model  = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
    vWorldPos   = (model * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(model) * aNormal;
    vUV         = aUV;
    gl_Position = uViewProj * vec4(vWorldPos, 1.0);
}
)GLSL";

// ─── Procedural skybox (drawn into the HDR target behind the scene) ─────────
// Fullscreen triangle at the far plane; reconstructs a world-space ray per
// pixel from the inverse view-projection and evaluates the same skyColor() the
// scene shader uses for ambient, so background and reflections match.
static const char* kSkyVS = R"GLSL(
#version 410 core
out vec2 vNDC;
void main()
{
	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0,
	              float((gl_VertexID & 2) << 1) - 1.0);
	vNDC = p;
	gl_Position = vec4(p, 1.0, 1.0); // z = far plane
}
)GLSL";

// The sky fragment shader (kSkyFS) and the shared analytic sky spliced into it
// and into kUnlitFS (kSkyFuncGLSL) live in HorizonRendering/SkyShaderSource.h:
// Vulkan, D3D11 and D3D12 compile the same text through a Vulkan-GLSL prelude.
using HE::glsl::kSkyFS;
using HE::glsl::kSkyFuncGLSL;

// Depth-only shader for the shadow pass — transforms by the light's view-proj.
static const char* kDepthVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uDepthMVP;
void main() { gl_Position = uDepthMVP * vec4(aPos, 1.0); }
)GLSL";

static const char* kDepthFS = R"GLSL(
#version 410 core
void main() {}
)GLSL";

// Instanced twin of kDepthVS for a run of same-mesh casters: the per-instance
// model matrix comes from the same attrib locs 4–7 / m_instanceVBO binding every
// mesh VAO already carries for kInstancedVS, so the shadow pass reuses the scene
// pass's instance buffer as is. uDepthVP is the light's view-proj alone.
static const char* kDepthInstancedVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aInstCol0;
layout(location = 5) in vec4 aInstCol1;
layout(location = 6) in vec4 aInstCol2;
layout(location = 7) in vec4 aInstCol3;
uniform mat4 uDepthVP;
void main()
{
    mat4 model  = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
    gl_Position = uDepthVP * model * vec4(aPos, 1.0);
}
)GLSL";

// ─── HDR tonemap (PostProcessPass) ──────────────────────────────────────────
// Fullscreen triangle generated from gl_VertexID — no vertex buffer needed.
static const char* kTonemapVS = R"GLSL(
#version 410 core
out vec2 vUV;
void main()
{
	vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0,
	              float((gl_VertexID & 2) << 1) - 1.0);
	vUV = p * 0.5 + 0.5;
	gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Samples the RGBA16F scene color, adds the blurred bloom, applies exposure, the
// ACES filmic curve and sRGB gamma, then writes LDR. This is where HDR highlights
// stop clipping and where bloom glow is composited back in.
static const char* kTonemapFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uHDR;
uniform sampler2D uBloom;
uniform float     uExposure;
uniform float     uBloomStrength;
uniform vec4      uLensFlare;   // xy sunNDC, z aspect, w strength (0 = OFF)
out vec4 FragColor;
vec3 aces(vec3 x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
// Camera lens flare (default OFF): post-process overlay added in gamma/LDR space after ACES.
// Mirrors the Metal lensFlareOverlay() byte-for-byte; only the pNDC reconstruction + probe uv
// convention differ (GL bottom-left uv → no y-flip).
vec3 lensFlareOverlay(vec2 uv, vec4 lf)
{
	float S = lf.w;
	if (S <= 0.0) return vec3(0.0);
	float aspect = lf.z;
	vec2  sunNDC = lf.xy;
	vec2  pNDC = uv * 2.0 - 1.0;                        // canonical y-up (GL uv is bottom-left)
	vec2  P    = vec2(pNDC.x * aspect, pNDC.y);
	vec2  Sc   = vec2(sunNDC.x * aspect, sunNDC.y);
	vec2  toSun = P - Sc; float sunDist = length(toSun);
	vec2  axis  = -Sc;                                  // sun → screen centre (and beyond)

	vec2  sunUV = sunNDC * 0.5 + 0.5;                   // GL bottom-left uv
	vec2  off[5] = vec2[5]( vec2(0.0,0.0), vec2(0.006,0.0), vec2(-0.006,0.0),
	                        vec2(0.0,0.006), vec2(0.0,-0.006) );
	float lum = 0.0;
	for (int i = 0; i < 5; ++i)
		lum += dot(texture(uHDR, clamp(sunUV + off[i], 0.0, 1.0)).rgb, vec3(0.2126, 0.7152, 0.0722));
	float vis = smoothstep(2.0, 7.0, lum * 0.2);

	vec3  warm  = vec3(1.0, 0.92, 0.80);
	float core  = 0.22 * exp(-sunDist * sunDist * 45.0);
	float streak = 0.10 * exp(-toSun.x * toSun.x * 5.0) * exp(-toSun.y * toSun.y * 800.0);
	vec3  flare = warm * (core + streak);                       // no halo ring (removed per feedback)
	float t[5]   = float[5]( 0.30, 0.55, 0.80, 1.20, 1.55 );
	float rad[5] = float[5]( 0.09, 0.14, 0.06, 0.20, 0.11 );
	float amp[5] = float[5]( 0.22, 0.15, 0.28, 0.10, 0.18 );
	vec3  gcol[5] = vec3[5]( vec3(1.0,0.85,0.6), vec3(0.6,0.8,1.0), vec3(1.0,0.7,0.7),
	                         vec3(0.7,1.0,0.8), vec3(0.8,0.7,1.0) );
	for (int i = 0; i < 5; ++i)
	{
		float d = length(P - (Sc + axis * t[i]));
		flare += amp[i] * gcol[i] * smoothstep(rad[i], 0.0, d);
	}
	return flare * (S * vis);
}
void main()
{
	vec3 hdr    = texture(uHDR, vUV).rgb;
	hdr        += texture(uBloom, vUV).rgb * uBloomStrength;
	hdr        *= uExposure;
	vec3 mapped = aces(hdr);
	mapped      = pow(mapped, vec3(1.0 / 2.2));
	mapped      = clamp(mapped + lensFlareOverlay(vUV, uLensFlare), 0.0, 1.0); // camera sun flare
	FragColor   = vec4(mapped, 1.0);
}
)GLSL";

// FXAA (Timothy Lottes' classic edge-blend variant): detect luma edges from the
// 3x3 neighbourhood and blend along them, leaving flat areas untouched. Run on the
// tonemapped (gamma-space) LDR image so its luma is perceptual. Also softens the
// single-pixel raymarch speckle the clouds leave in near-clear sky.
static const char* kFxaaFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uScene;
uniform vec2      uRcpFrame;   // 1.0 / resolution
out vec4 FragColor;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main()
{
	const float EDGE_MIN = 1.0 / 24.0;  // ignore tiny luma differences (no edge)
	const float EDGE_MAX = 1.0 / 8.0;   // relative threshold scaled by local max luma
	const float SPAN_MAX = 8.0;         // clamp on the blur search length (texels)

	vec3  rgbM = texture(uScene, vUV).rgb;
	float lM   = luma(rgbM);
	float lNW  = luma(textureOffset(uScene, vUV, ivec2(-1, -1)).rgb);
	float lNE  = luma(textureOffset(uScene, vUV, ivec2( 1, -1)).rgb);
	float lSW  = luma(textureOffset(uScene, vUV, ivec2(-1,  1)).rgb);
	float lSE  = luma(textureOffset(uScene, vUV, ivec2( 1,  1)).rgb);

	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	float range = lMax - lMin;
	if (range < max(EDGE_MIN, lMax * EDGE_MAX)) { FragColor = vec4(rgbM, 1.0); return; }

	// Edge tangent from the diagonal luma gradients.
	vec2 dir;
	dir.x = -((lNW + lNE) - (lSW + lSE));
	dir.y =  ((lNW + lSW) - (lNE + lSE));
	float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
	float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
	dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * uRcpFrame;

	vec3 rgbA = 0.5 * (texture(uScene, vUV + dir * (1.0 / 3.0 - 0.5)).rgb
	                 + texture(uScene, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
	vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(uScene, vUV + dir * -0.5).rgb
	                               + texture(uScene, vUV + dir *  0.5).rgb);
	float lB = luma(rgbB);
	FragColor = (lB < lMin || lB > lMax) ? vec4(rgbA, 1.0) : vec4(rgbB, 1.0);
}
)GLSL";

// SMAA-style spatial AA (docs/anti-aliasing-plan.md, A1), fused into ONE pass.
//
// What it does, and how it differs from FXAA above: FXAA estimates an edge
// TANGENT from four diagonal lumas and blurs along it — it never learns how long
// the edge is, so a 2-texel step and a 40-texel one get the same treatment. This
// pass instead does what MLAA/SMAA do: find the boundary the pixel sits on,
// SEARCH along it for both of its ends, classify what the edge does at those
// ends, and derive the pixel's coverage analytically from its position inside
// that span. The result is a perpendicular blend of a computed fraction, not a
// smear along a guessed direction — long edges stay straight and flat areas are
// never touched.
//
// Honest scope: this is the orthogonal (L/Z/U) half of SMAA 1x with the coverage
// computed in closed form instead of read from SMAA's precomputed AreaTex. There
// are no diagonal patterns and no corner rounding, and the span search steps one
// texel at a time up to kSmaaMaxSearch (no SearchTex to jump with). Quality sits
// where MLAA sits: clearly sharper than FXAA, below real SMAA 1x. Upgrading
// later means generating the AreaTex at startup — not vendoring binaries.
static const char* kSmaaFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uScene;
uniform vec2      uRcpFrame;   // 1.0 / resolution
out vec4 FragColor;

const float kEdgeMin   = 1.0 / 24.0; // absolute floor — ignore imperceptible steps
const float kEdgeRel   = 1.0 / 8.0;  // relative threshold, scaled by local max luma
// Span search: single texel steps first (that is where the coverage ramp is
// steepest and a ±1 texel error shows), then double steps. 8 + 12*2 = 32 texels
// of reach for 20 iterations. Reach is not a nicety: a shallow edge has LONG
// spans, and a span whose ends are both out of reach gets no pattern and no
// blending at all — measured, raising this from 12 took a 33-texel-span test
// edge from 23/60 antialiased columns to 57/60.
const int   kFineSteps   = 8;
const int   kSearchIters = 20;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) { return luma(texture(uScene, uv).rgb); }

// Walk from `uv` in `along` while the boundary to `across` survives. Returns the
// distance to the last texel that still carries it. `ended` is false when the
// walk ran into the cap — a span whose end we never saw has no usable pattern,
// so the caller treats that side as "no crossing" instead of inventing one.
float smaaSearch(vec2 uv, vec2 along, vec2 across, float thr,
                 out bool ended, out float outerLuma)
{
	ended = false; outerLuma = 0.0;
	float dist = 0.0;
	for (int i = 0; i < kSearchIters; ++i)
	{
		float st = (i < kFineSteps) ? 1.0 : 2.0;
		dist += st;
		vec2  p = uv + along * dist;
		float a = lumaAt(p);
		float b = lumaAt(p + across);
		if (abs(a - b) < thr) { ended = true; outerLuma = a; return dist - st; }
	}
	return dist;
}

// Height of the revectorized edge at `t` (0..1 along the span), as a fraction of
// a texel. Each end is -0.5 when the OTHER surface steps into our row (it covers
// part of us → we owe it coverage), +0.5 when we step into theirs (their
// problem, computed when that pixel runs this for its own boundary), 0 when the
// span end was never found. Two ends of equal sign are a notch, and a straight
// line cannot describe it — the line is split at the middle instead (the U case).
float smaaCover(float t, float y1, float y2, bool split)
{
	float f = split ? ((t < 0.5) ? mix(y1, 0.0, t * 2.0) : mix(0.0, y2, (t - 0.5) * 2.0))
	                : mix(y1, y2, t);
	return max(0.0, -f);
}

// How much this pixel blends toward the neighbour across `across`.
float smaaWeight(vec2 uv, vec2 along, vec2 across, float lumaP, float lumaO, float thr)
{
	bool  e1, e2;
	float o1, o2;
	float d1 = smaaSearch(uv, -along, across, thr, e1, o1);
	float d2 = smaaSearch(uv,  along, across, thr, e2, o2);
	float len = d1 + d2 + 1.0;
	float t   = (d1 + 0.5) / len;   // where this pixel sits inside the span, 0..1
	float y1  = e1 ? (abs(o1 - lumaO) < abs(o1 - lumaP) ? -0.5 : 0.5) : 0.0;
	float y2  = e2 ? (abs(o2 - lumaO) < abs(o2 - lumaP) ? -0.5 : 0.5) : 0.0;
	bool  split = (e1 && e2 && y1 == y2);
	// Two-point quadrature ACROSS THE PIXEL instead of one sample at its centre.
	// Without it the corner texel of a one-texel span sits exactly on the line's
	// zero crossing and gets nothing — the one place a staircase is most visible.
	// Measured: 46/60 → 56/60 antialiased columns on a test edge.
	float dt = 0.25 / len;
	return 0.5 * (smaaCover(clamp(t - dt, 0.0, 1.0), y1, y2, split)
	            + smaaCover(clamp(t + dt, 0.0, 1.0), y1, y2, split));
}

void main()
{
	vec2  rcp = uRcpFrame;
	vec3  C   = texture(uScene, vUV).rgb;
	float lC  = luma(C);
	float lW  = lumaAt(vUV + vec2(-rcp.x, 0.0));
	float lE  = lumaAt(vUV + vec2( rcp.x, 0.0));
	float lN  = lumaAt(vUV + vec2(0.0, -rcp.y));
	float lS  = lumaAt(vUV + vec2(0.0,  rcp.y));
	float lMax = max(lC, max(max(lW, lE), max(lN, lS)));
	float lMin = min(lC, min(min(lW, lE), min(lN, lS)));
	float thr  = max(kEdgeMin, lMax * kEdgeRel);
	if (lMax - lMin < thr) { FragColor = vec4(C, 1.0); return; }

	// One orientation per pixel: the axis with the stronger second derivative is
	// the one the edge actually runs across. Doing both would double the search
	// cost to fix cases the second axis gets wrong anyway.
	float edgeH = abs(lN - 2.0 * lC + lS);   // varies vertically → horizontal edge
	float edgeV = abs(lW - 2.0 * lC + lE);
	float wA = 0.0, wB = 0.0;
	vec2  offA, offB;
	if (edgeH >= edgeV)
	{
		offA = vec2(0.0, -rcp.y); offB = vec2(0.0, rcp.y);
		if (abs(lC - lN) >= thr) wA = smaaWeight(vUV, vec2(rcp.x, 0.0), offA, lC, lN, thr);
		if (abs(lC - lS) >= thr) wB = smaaWeight(vUV, vec2(rcp.x, 0.0), offB, lC, lS, thr);
	}
	else
	{
		offA = vec2(-rcp.x, 0.0); offB = vec2(rcp.x, 0.0);
		if (abs(lC - lW) >= thr) wA = smaaWeight(vUV, vec2(0.0, rcp.y), offA, lC, lW, thr);
		if (abs(lC - lE) >= thr) wB = smaaWeight(vUV, vec2(0.0, rcp.y), offB, lC, lE, thr);
	}

	float sum = wA + wB;
	if (sum > 1.0) { wA /= sum; wB /= sum; sum = 1.0; }
	vec3 outC = C * (1.0 - sum)
	          + wA * texture(uScene, vUV + offA).rgb
	          + wB * texture(uScene, vUV + offB).rgb;
	FragColor = vec4(outC, 1.0);
}
)GLSL";

// AA = Off: the resolve pass still has to fill the output target, so it runs
// this passthrough instead of skipping. Same inputs as FXAA, no filtering.
static const char* kBlitFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uScene;
out vec4 FragColor;
void main() { FragColor = vec4(texture(uScene, vUV).rgb, 1.0); }
)GLSL";

// ─── Temporal AA (docs/anti-aliasing-plan.md A2/A3) ─────────────────────────
// The GL twin of kTaaMSL in the Metal backend, same rule: the geometry is
// RASTERIZED with the jittered matrix (so the subpixel offset lands in the
// image, which is the whole point), but the MOTION is measured with unjittered
// ones. Mixing those up makes every static pixel report the jitter as movement,
// and TAA then chases its own offset.
//
// Velocity is its own pass over the opaque draw list (positions only, depth-
// tested LEQUAL against the scene depth so only the visible surface reports),
// not a G-buffer attachment — material-agnostic by construction, so a custom
// material that never heard of velocity cannot leave undefined motion behind.
static const char* kTaaVelocityVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMvpJitter;   // rasterizes
uniform mat4 uMvpNow;      // measures (unjittered)
uniform mat4 uMvpPrev;     // measures (unjittered, last frame's camera + model)
out vec4 vClipNow;
out vec4 vClipPrev;
void main()
{
	vec4 p = vec4(aPos, 1.0);
	gl_Position = uMvpJitter * p;
	vClipNow    = uMvpNow  * p;
	vClipPrev   = uMvpPrev * p;
}
)GLSL";

// Screen-space motion in TEXTURE-UV units, so the resolve can subtract it from
// its own uv. GL textures are bottom-up like its NDC, so unlike the Metal twin
// there is no y flip here — the same convention kMbVelocityFS and vUV use.
static const char* kTaaVelocityFS = R"GLSL(
#version 410 core
in vec4 vClipNow;
in vec4 vClipPrev;
out vec2 FragColor;          // RG16F: uvNow - uvPrev
void main()
{
	vec2 ndcNow  = vClipNow.xy  / max(vClipNow.w,  1e-6);
	vec2 ndcPrev = vClipPrev.xy / max(vClipPrev.w, 1e-6);
	FragColor = (ndcNow - ndcPrev) * 0.5;
}
)GLSL";

// Blend this frame's tonemapped image with the reprojected history. Current
// and velocity are read with texelFetch (point, clamped by hand) — the LDR
// texture is LINEAR-filtered for FXAA/SMAA and must stay so; the history
// textures are linear on purpose (subpixel reprojection).
// uParams: x/y = 1/resolution, z = history blend weight (0 = history unusable
// this frame — resize, first frame), w unused.
static const char* kTaaResolveFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform sampler2D uVelocity;
uniform vec4      uParams;
out vec4 FragColor;
void main()
{
	ivec2 size = textureSize(uCurrent, 0);
	ivec2 px   = ivec2(gl_FragCoord.xy);
	vec3  cur  = texelFetch(uCurrent, px, 0).rgb;
	if (uParams.z <= 0.0) { FragColor = vec4(cur, 1.0); return; }

	// Motion of the CLOSEST fragment in the neighbourhood, not this pixel's own:
	// on a silhouette the pixel itself may carry the background's motion while
	// the eye follows the object, and picking the nearest keeps the edge with
	// the object instead of smearing it against the background.
	vec2  vel  = texelFetch(uVelocity, px, 0).rg;
	float best = length(vel);
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			ivec2 q = clamp(px + ivec2(x, y), ivec2(0), size - 1);
			vec2  v = texelFetch(uVelocity, q, 0).rg;
			float l = length(v);
			if (l > best) { best = l; vel = v; }
		}

	vec2 histUV = vUV - vel;
	// Off-screen history is no history: nothing was ever accumulated there.
	if (any(lessThan(histUV, vec2(0.0))) || any(greaterThan(histUV, vec2(1.0))))
	{
		FragColor = vec4(cur, 1.0);
		return;
	}
	vec3 hist = texture(uHistory, histUV).rgb;

	// Neighbourhood clamp — the whole defence against ghosting. Whatever the
	// history says, the result has to stay inside the colours this frame
	// actually produced around this pixel; a disoccluded surface therefore
	// cannot keep showing what used to be in front of it.
	vec3 lo = cur, hi = cur;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			ivec2 q = clamp(px + ivec2(x, y), ivec2(0), size - 1);
			vec3  c = texelFetch(uCurrent, q, 0).rgb;
			lo = min(lo, c);
			hi = max(hi, c);
		}
	hist = clamp(hist, lo, hi);

	// Fast motion means less history: the further the reprojection reached, the
	// less it can be trusted (and the less a stale sample is worth).
	float motion = clamp(length(vel / uParams.xy) / 32.0, 0.0, 1.0);
	float blend  = mix(uParams.z, 0.0, motion);
	FragColor = vec4(mix(cur, hist, blend), 1.0);
}
)GLSL";

// The temporal average is softer than a single frame by construction — this is
// the sharpen that buys that back, and the only reason the AA-resolve slot still
// runs a shader for TAA instead of the plain blit. uParams.z = amount, 0 = exact
// passthrough.
static const char* kTaaSharpenFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uScene;
uniform vec4      uParams;   // xy = 1/resolution, z = sharpen amount
out vec4 FragColor;
void main()
{
	vec2 rcp = uParams.xy;
	vec3 c   = texture(uScene, vUV).rgb;
	if (uParams.z <= 0.0) { FragColor = vec4(c, 1.0); return; }
	vec3 blur = 0.25 * (texture(uScene, vUV + vec2( rcp.x, 0.0)).rgb
	                  + texture(uScene, vUV + vec2(-rcp.x, 0.0)).rgb
	                  + texture(uScene, vUV + vec2(0.0,  rcp.y)).rgb
	                  + texture(uScene, vUV + vec2(0.0, -rcp.y)).rgb);
	FragColor = vec4(clamp(c + (c - blur) * uParams.z, 0.0, 1.0), 1.0);
}
)GLSL";

// Bloom bright-pass: keep only the part of each pixel above a soft-knee
// threshold (Call-of-Duty-style curve), preserving hue. Feeds the blur chain.
static const char* kBloomBrightFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uHDR;
uniform float     uThreshold;
uniform float     uKnee;
out vec4 FragColor;
void main()
{
	vec3  c  = texture(uHDR, vUV).rgb;
	float br = max(c.r, max(c.g, c.b));
	float soft = clamp(br - uThreshold + uKnee, 0.0, 2.0 * uKnee);
	soft = (soft * soft) / (4.0 * uKnee + 1e-4);
	float contrib = max(soft, br - uThreshold) / max(br, 1e-4);
	FragColor = vec4(c * contrib, 1.0);
}
)GLSL";

// Separable 9-tap Gaussian blur. uHorizontal picks the axis; run as ping-pong
// horizontal/vertical pairs to approximate a 2D blur.
static const char* kBloomBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uImage;
uniform vec2      uTexel;       // 1 / textureSize
uniform int       uHorizontal;
out vec4 FragColor;
void main()
{
	float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
	vec2 dir = (uHorizontal == 1) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
	vec3 result = texture(uImage, vUV).rgb * w[0];
	for (int i = 1; i < 5; ++i)
	{
		result += texture(uImage, vUV + dir * float(i)).rgb * w[i];
		result += texture(uImage, vUV - dir * float(i)).rgb * w[i];
	}
	FragColor = vec4(result, 1.0);
}
)GLSL";

// ─── Depth of field ──────────────────────────────────────────────────────────
// Three fragment programs on the fullscreen triangle, mirrored 1:1 by kDofMSL in
// the Metal backend (same maths, same tap count, same weights):
//
//   CoC        half-res: scene depth → (signed blur radius in half-res texels,
//              linear depth). Of the 2×2 full-res texels under a half-res texel
//              the one with the LARGEST radius wins, so a thin blurry sliver is
//              never lost to the downsample.
//   Blur       half-res, run twice (horizontal, then vertical). A "scatter as
//              gather": every tap is weighted by whether ITS OWN circle of
//              confusion reaches the pixel being shaded. A tap in front of the
//              centre may spill with its full radius (near objects bleed over the
//              sharp background, as a lens does); a tap behind it only with the
//              centre's radius (the blurred background never bleeds over a sharp
//              foreground). Alpha carries the gathered blurriness so the
//              composite can show that near-object spill on top of pixels whose
//              own CoC says "sharp".
//   Composite  full-res: lerp sharp ↔ blurred by max(own CoC, gathered
//              blurriness). Writes the new HDR image bloom/tonemap then read.
//
// Depth is linearised from the projection's own terms (proj[2][2], proj[3][2]):
// both backends rasterise with the extractor's GL-style matrix (Metal remaps
// -1..1 → 0..1 with kMetalClipFix, which the shader undoes), so the linear depth
// is the same number on both. Orthographic cameras have no depth of field
// (proj[3][3] == 1 makes the formula meaningless) — the pass is skipped for them.
static const char* kDofCocFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uDepth;     // full-res scene depth (window z, 0..1)
uniform vec4 uDofParams;
uniform vec2 uDofProj;
out vec2 FragColor;           // RG16F: signed radius, linear depth
float dofLinearDepth(float d) { float ndc = d * 2.0 - 1.0; return uDofProj.y / (ndc + uDofProj.x); }
float dofSignedRadius(float linearDepth)
{
	float band  = max(uDofParams.y * 0.5, 1e-3);
	float delta = linearDepth - uDofParams.x;
	float t     = clamp((abs(delta) - band) / band, 0.0, 1.0);
	return sign(delta) * t * uDofParams.z;
}
void main()
{
	// The 2×2 full-res block under this half-res texel; keep the blurriest.
	vec4  d4   = textureGather(uDepth, vUV, 0);
	float best = 0.0, bestD = 0.0;
	for (int i = 0; i < 4; ++i)
	{
		float D = dofLinearDepth(d4[i]);
		float r = dofSignedRadius(D);
		if (i == 0 || abs(r) > abs(best)) { best = r; bestD = D; }
	}
	FragColor = vec2(best, bestD);
}
)GLSL";

static const char* kDofBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uImage;      // H pass: full-res HDR (bilinear = 2×2 box); V pass: the H result
uniform sampler2D uCoc;        // half-res (signed radius, linear depth)
uniform vec2      uTexel;      // 1 / half-res size
uniform int       uHorizontal; // 1 = first (horizontal) pass, alpha is built from the CoC
uniform vec4      uDofParams;
out vec4 FragColor;
const int kTaps = 8;           // per side; the kernel always spans the max radius
void main()
{
	vec2  cc   = texture(uCoc, vUV).rg;
	float rC   = abs(cc.x);
	float dC   = cc.y;
	float stepPx = max(uDofParams.z, 1e-3) / float(kTaps);
	vec2  dir  = (uHorizontal == 1) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
	vec4  c0   = texture(uImage, vUV);
	float a0   = (uHorizontal == 1) ? clamp(rC, 0.0, 1.0) : c0.a;
	vec4  sum  = vec4(c0.rgb, a0);
	float wsum = 1.0;
	for (int i = 1; i <= kTaps; ++i)
	{
		float dist = float(i) * stepPx;
		for (int s = -1; s <= 1; s += 2)
		{
			vec2  uv  = vUV + dir * (dist * float(s));
			vec2  ct  = texture(uCoc, uv).rg;
			float rT  = abs(ct.x);
			float dT  = ct.y;
			// In front of the centre: spill with its own radius. Behind it:
			// only as far as the centre itself is blurred.
			bool  front = dT < dC - max(0.05, 0.02 * dC);
			float rEff  = front ? rT : min(rT, rC);
			float w     = clamp((rEff - dist) / stepPx + 1.0, 0.0, 1.0);
			vec4  c     = texture(uImage, uv);
			float a     = (uHorizontal == 1) ? clamp(rT, 0.0, 1.0) : c.a;
			sum  += vec4(c.rgb, a) * w;
			wsum += w;
		}
	}
	FragColor = sum / wsum;
}
)GLSL";

static const char* kDofCompositeFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uSharp;     // full-res HDR
uniform sampler2D uBlurred;   // half-res blur result (rgb colour, a gathered blurriness)
uniform sampler2D uDepth;     // full-res scene depth
uniform vec4 uDofParams;
uniform vec2 uDofProj;
out vec4 FragColor;
float dofLinearDepth(float d) { float ndc = d * 2.0 - 1.0; return uDofProj.y / (ndc + uDofProj.x); }
float dofSignedRadius(float linearDepth)
{
	float band  = max(uDofParams.y * 0.5, 1e-3);
	float delta = linearDepth - uDofParams.x;
	float t     = clamp((abs(delta) - band) / band, 0.0, 1.0);
	return sign(delta) * t * uDofParams.z;
}
void main()
{
	float r     = abs(dofSignedRadius(dofLinearDepth(texture(uDepth, vUV).r)));
	vec4  blur  = texture(uBlurred, vUV);
	vec3  sharp = texture(uSharp, vUV).rgb;
	float t     = clamp(max(r, blur.a), 0.0, 1.0);
	FragColor   = vec4(mix(sharp, blur.rgb, t), 1.0);
}
)GLSL";

// ─── Motion blur ─────────────────────────────────────────────────────────────
// Two fragment programs on the fullscreen triangle, mirrored 1:1 by kMotionBlurMSL
// in the Metal backend:
//
//   Velocity   full-res: the pixel's ndc position (uv + scene depth) is carried
//              into the PREVIOUS frame's clip space by one matrix
//              (prevViewProj · inverse(viewProj)); the difference of the two
//              screen positions, times the shutter fraction, is the velocity in
//              PIXELS (RG16F — pixels, not uv, so a 0.3-px motion at 4K is not
//              lost to half-float rounding). Capped at the max blur length.
//              Camera motion only: the depth says where the pixel IS, the two
//              camera matrices say how the CAMERA moved; an object moving under
//              a still camera has zero velocity here.
//   Blur       full-res: kTaps taps spread over the centre pixel's velocity,
//              half behind and half ahead. A tap is weighted by how fast IT is
//              moving relative to the centre: a still foreground object next to
//              a streaking background keeps its edge instead of being dragged
//              along. Pixels with under half a pixel of motion pass through
//              untouched — a still camera gives the exact input image.
//
// The depth convention differs per backend and the shaders own it: GL holds
// window z (0..1 → ndc via ·2-1), Metal already holds GL ndc z (see kDofMSL).
static const char* kMbVelocityFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uDepth;      // full-res scene depth (window z, 0..1)
uniform mat4 uReproject;       // prevViewProj * inverse(viewProj), GL-style clip both sides
uniform vec4 uMbParams;        // x shutter fraction, y max blur (px), zw target size (px)
out vec2 FragColor;            // RG16F: velocity in pixels (+x right, +y up in GL's uv)
void main()
{
	float d    = texture(uDepth, vUV).r;
	vec4  ndc  = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
	vec4  prev = uReproject * ndc;
	if (prev.w <= 1e-5) { FragColor = vec2(0.0); return; }   // behind the previous camera
	vec2  prevUV = (prev.xy / prev.w) * 0.5 + 0.5;
	vec2  vPx    = (vUV - prevUV) * uMbParams.zw * uMbParams.x;
	float len    = length(vPx);
	if (len > uMbParams.y) vPx *= uMbParams.y / len;
	FragColor = vPx;
}
)GLSL";

static const char* kMbBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uImage;      // full-res HDR (the DoF result when that ran)
uniform sampler2D uVelocity;   // full-res velocity in pixels
uniform vec2      uTexel;      // 1 / target size
out vec4 FragColor;
const int kTaps = 6;           // per side; 12 taps + centre over the velocity's length
void main()
{
	vec2  vC = texture(uVelocity, vUV).rg;
	float lC = length(vC);
	vec4  c0 = texture(uImage, vUV);
	if (lC < 0.5) { FragColor = vec4(c0.rgb, 1.0); return; }   // still: pass through, bit-exact
	vec3  sum  = c0.rgb;
	float wsum = 1.0;
	for (int i = 1; i <= kTaps; ++i)
	{
		float t = float(i) / float(kTaps + 1);   // (0,1): fraction of the half-length
		for (int s = -1; s <= 1; s += 2)
		{
			vec2  uv = vUV + vC * (0.5 * t * float(s)) * uTexel;
			vec2  vT = texture(uVelocity, uv).rg;
			// A tap moving at least half as fast as the centre counts fully; a
			// (near) still tap barely — it belongs to something that is not
			// streaking, and must not be smeared into what is.
			float w  = clamp(length(vT) * 2.0 / lC, 0.0, 1.0);
			sum  += texture(uImage, uv).rgb * w;
			wsum += w;
		}
	}
	FragColor = vec4(sum / wsum, 1.0);
}
)GLSL";

// ─── SSAO (screen-space ambient occlusion) ──────────────────────────────────
// The hemisphere kernel sample count is HE::kSsaoKernelSize (SsaoKernel.h) —
// the uKernel[32] declaration below must stay in step with it.

// Pre-pass: rasterise the scene and write the per-pixel VIEW-SPACE position
// (xyz, with a = 1 marking valid geometry vs. the cleared background). Working in
// view space sidesteps every depth-buffer / clip-space convention difference
// between the backends — the SSAO maths is then identical on GL and Metal.
// ─── Global Illumination (GL 4.3 compute port, Windows/Linux) ────────────────
// Software ray tracing against the CPU-built HE::GiBvh (see GiBvh.h): the GLSL
// traversal below mirrors giBvhIntersect() 1:1 — same slab test, same
// Möller-Trumbore (two-sided), same 64-entry stack — so tests/test_gi_bvh.cpp
// validates the exact algorithm these kernels run. Node/instance ints travel
// as float bit patterns in vec4 rows (floatBitsToInt) so the std430 blocks are
// layout-proof across drivers. The raster/temporal/blur stages stay #version
// 410 (compilable everywhere incl. macOS); only the two compute kernels are
// 430 and only ever compiled behind m_giSupported.

// World-space G-buffer pre-pass (position + normal, MRT) — the GI counterpart
// of kSSAOPosVS/FS. CRITICAL (Metal lesson, commit 5846efc): rendered with the
// SAME extraction/camera as the scene pass, or the screen-space mask
// misaligns and shadows swim with camera rotation.
static const char* kGiGBufVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorldPos;
out vec3 vNormal;
void main()
{
	vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
	vNormal     = mat3(uModel) * aNormal;
	gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

// Instanced twin of kGiGBufVS for a GeometryPass batch: model from attrib locs
// 4–7 / m_instanceVBO (the kInstancedVS binding every mesh VAO carries), the
// camera view-proj as the one uniform. Same fragment stage, same outputs.
static const char* kGiGBufInstancedVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 4) in vec4 aInstCol0;
layout(location = 5) in vec4 aInstCol1;
layout(location = 6) in vec4 aInstCol2;
layout(location = 7) in vec4 aInstCol3;
uniform mat4 uViewProj;
out vec3 vWorldPos;
out vec3 vNormal;
void main()
{
	mat4 model  = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
	vWorldPos   = (model * vec4(aPos, 1.0)).xyz;
	vNormal     = mat3(model) * aNormal;
	gl_Position = uViewProj * vec4(vWorldPos, 1.0);
}
)GLSL";

static const char* kGiGBufFS = R"GLSL(
#version 410 core
in vec3 vWorldPos;
in vec3 vNormal;
uniform vec2 uRoughMetal;                    // per draw: x = roughness, y = metallic
layout(location = 0) out vec4 oPos;
layout(location = 1) out vec4 oNorm;
layout(location = 2) out vec4 oMat;
void main()
{
	oPos  = vec4(vWorldPos, 1.0);            // a = 1 → valid geometry
	oNorm = vec4(normalize(vNormal), 0.0);
	// Surface response for the reflection kernel (attachment 2). The shadow
	// kernel ignores it — it only needs pos/normal. Flat per DRAW, so a graph
	// material's per-pixel roughness is not represented here; the composite
	// applies the roughness fade again with the shader's exact value, this only
	// decides whether a ray is traced at all.
	oMat  = vec4(clamp(uRoughMetal.x, 0.0, 1.0), clamp(uRoughMetal.y, 0.0, 1.0), 0.0, 1.0);
}
)GLSL";

// Shared BVH declarations + traversal, string-prepended into both compute
// kernels (GLSL has no #include). Instances are the TLAS analogue: the ray is
// transformed into object space by invTransform with an UNNORMALISED
// direction, so the parametric t stays world-comparable across instances.
static const char* kGiTraversalGLSL = R"GLSL(
struct GiNode { vec4 d0; vec4 d1; }; // d0.xyz bmin, d0.w leftFirst (int bits), d1.xyz bmax, d1.w triCount (int bits)
struct GiTri  { vec4 v0; vec4 v1; vec4 v2; };
// baseColor.a = metallic, emissive.a = roughness (HE::giInstanceSurface —
// mirrors OpenGLRenderer::GIInstanceGpu; 112 bytes, std430 stride 112).
struct GiInst { mat4 invTransform; vec4 baseColor; vec4 emissive; ivec4 offsets; }; // offsets.x = nodeOffset, .y = triOffset, .z = landscape index (-1 = none)
// Painted landscape — HE::GiLandscape / GILandGpu. std430 keeps the mat4 and the
// vec4s naturally aligned, so the CPU struct maps 1:1.
struct GiLand { mat4 worldToLocal; vec4 cfg; vec4 layer[4]; }; // cfg: xy = 1/(sizeX,sizeZ), z = uvTiling, w = layer count
layout(std430, binding = 0) readonly buffer GiNodes { GiNode giNodes[]; };
layout(std430, binding = 1) readonly buffer GiTris  { GiTri  giTris[];  };
layout(std430, binding = 2) readonly buffer GiInsts { GiInst giInsts[]; };
layout(std430, binding = 3) readonly buffer GiLands { GiLand giLands[]; };
uniform int uGiInstanceCount;
uniform int uGiLandCount;              // 0 = no painted terrain in this scene
uniform float uRays;                   // rays per pixel (quality tier)
uniform sampler2D uLandWeights[4];     // one per landscape, in giLands order

// Möller-Trumbore, both faces — mirrors GiBvh.cpp's triHit().
bool giTriHit(GiTri tri, vec3 o, vec3 d, float tMin, float tMax, out float tOut)
{
	tOut = 0.0;
	vec3 e1 = tri.v1.xyz - tri.v0.xyz;
	vec3 e2 = tri.v2.xyz - tri.v0.xyz;
	vec3 p  = cross(d, e2);
	float det = dot(e1, p);
	if (abs(det) < 1e-9) return false;
	float invDet = 1.0 / det;
	vec3 s = o - tri.v0.xyz;
	float u = dot(s, p) * invDet;
	if (u < 0.0 || u > 1.0) return false;
	vec3 q = cross(s, e1);
	float v = dot(d, q) * invDet;
	if (v < 0.0 || u + v > 1.0) return false;
	float t = dot(e2, q) * invDet;
	if (t <= tMin || t >= tMax) return false;
	tOut = t;
	return true;
}

// BLAS traversal (one instance), object-space ray. anyHit: first accepted hit
// wins. Mirrors GiBvh.cpp's giBvhIntersect() — same stack bound.
bool giBlasHit(int nodeOfs, int triOfs, vec3 o, vec3 d, float tMin, float tMax,
               bool anyHit, out float tOut)
{
	tOut = tMax;
	vec3 invD = 1.0 / d;
	int stack[64];
	int sp = 0;
	stack[sp++] = nodeOfs;
	bool hit = false;
	float best = tMax;
	while (sp > 0)
	{
		GiNode n = giNodes[stack[--sp]];
		vec3 t0 = (n.d0.xyz - o) * invD;
		vec3 t1 = (n.d1.xyz - o) * invD;
		vec3 lo = min(t0, t1);
		vec3 hi = max(t0, t1);
		float tN = max(max(lo.x, lo.y), max(lo.z, tMin));
		float tF = min(min(hi.x, hi.y), min(hi.z, best));
		if (tN > tF) continue;
		int leftFirst = floatBitsToInt(n.d0.w);
		int triCount  = floatBitsToInt(n.d1.w);
		if (triCount > 0)
		{
			for (int i = 0; i < triCount; ++i)
			{
				float t;
				if (giTriHit(giTris[triOfs + leftFirst + i], o, d, tMin, best, t))
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

// TLAS analogue: linear instance loop (v1 — instance counts here are small;
// a top-level BVH is a documented perf follow-up). World-space ray in/out.
bool giSceneAnyHit(vec3 o, vec3 d, float tMin, float tMax)
{
	for (int i = 0; i < uGiInstanceCount; ++i)
	{
		vec3 oL = (giInsts[i].invTransform * vec4(o, 1.0)).xyz;
		vec3 dL = mat3(giInsts[i].invTransform) * d;
		float t;
		if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, tMax, true, t))
			return true;
	}
	return false;
}

// Closest hit across all instances; returns instance index (-1 = miss).
int giSceneClosestHit(vec3 o, vec3 d, float tMin, float tMax, out float tOut)
{
	int   bestInst = -1;
	float best     = tMax;
	for (int i = 0; i < uGiInstanceCount; ++i)
	{
		vec3 oL = (giInsts[i].invTransform * vec4(o, 1.0)).xyz;
		vec3 dL = mat3(giInsts[i].invTransform) * d;
		float t;
		if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, best, false, t))
		{
			best = t; bestInst = i;
		}
	}
	tOut = best;
	return bestInst;
}

// ── Closest hit WITH the triangle index ──────────────────────────────────────
// Same traversal as giBlasHit/giSceneClosestHit, but it also reports which
// triangle of giTris[] was hit, so the caller can build a real geometric normal
// instead of the `-rayDir` stand-in the probe kernel uses. Reflections need it:
// with `-rayDir` a reflected surface is lit as if it always faced the ray, and a
// mirror shows a flat-shaded ghost of the scene rather than the scene.
// The probe/shadow kernels deliberately keep calling the cheaper pair above —
// their hit shading is low-frequency and the extra register pressure is not
// worth it (docs/gi-reflections-plan.md P4/P5).
bool giBlasHitTri(int nodeOfs, int triOfs, vec3 o, vec3 d, float tMin, float tMax,
                  out float tOut, out int triOut)
{
	tOut   = tMax;
	triOut = -1;
	vec3 invD = 1.0 / d;
	int stack[64];
	int sp = 0;
	stack[sp++] = nodeOfs;
	bool hit = false;
	float best = tMax;
	while (sp > 0)
	{
		GiNode n = giNodes[stack[--sp]];
		vec3 t0 = (n.d0.xyz - o) * invD;
		vec3 t1 = (n.d1.xyz - o) * invD;
		vec3 lo = min(t0, t1);
		vec3 hi = max(t0, t1);
		float tN = max(max(lo.x, lo.y), max(lo.z, tMin));
		float tF = min(min(hi.x, hi.y), min(hi.z, best));
		if (tN > tF) continue;
		int leftFirst = floatBitsToInt(n.d0.w);
		int triCount  = floatBitsToInt(n.d1.w);
		if (triCount > 0)
		{
			for (int i = 0; i < triCount; ++i)
			{
				int   ti = triOfs + leftFirst + i;
				float t;
				if (giTriHit(giTris[ti], o, d, tMin, best, t))
				{
					hit = true; best = t; tOut = t; triOut = ti;
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

// Closest hit across all instances with the triangle index; -1 = miss.
int giSceneClosestHitTri(vec3 o, vec3 d, float tMin, float tMax, out float tOut, out int triOut)
{
	int   bestInst = -1;
	float best     = tMax;
	triOut = -1;
	for (int i = 0; i < uGiInstanceCount; ++i)
	{
		vec3 oL = (giInsts[i].invTransform * vec4(o, 1.0)).xyz;
		vec3 dL = mat3(giInsts[i].invTransform) * d;
		float t; int tri;
		if (giBlasHitTri(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, best, t, tri))
		{
			best = t; bestInst = i; triOut = tri;
		}
	}
	tOut = best;
	return bestInst;
}

// World-space geometric normal of triangle `tri` on instance `inst`, facing
// AGAINST the incoming ray. The BVH triangles are OBJECT space, so the normal
// transforms with the inverse-transpose of object→world — and invTransform IS
// world→object, so transpose(invTransform) is exactly that matrix. (Möller-
// Trumbore is two-sided here, so back faces have to be flipped explicitly.)
vec3 giHitNormal(int inst, int tri, vec3 rayDir)
{
	GiTri t = giTris[tri];
	vec3 nL = cross(t.v1.xyz - t.v0.xyz, t.v2.xyz - t.v0.xyz);
	vec3 nW = transpose(mat3(giInsts[inst].invTransform)) * nL;
	float len = length(nW);
	if (len < 1e-12) return -rayDir;         // degenerate triangle → v1 stand-in
	nW /= len;
	return (dot(nW, rayDir) > 0.0) ? -nW : nW;
}
)GLSL";

// Shadow-ray kernel: 1 cone-jittered ray/pixel toward the dominant directional
// light (giDominantDirectionalLight pick — Metal lesson 5e45643: NEVER the
// sky-dome sun). Same hash/cone/bias constants as Metal's giShadowRay.
static const char* kGiShadowCS = R"GLSL(
uniform sampler2D uGPos;
uniform sampler2D uGNorm;
layout(r16f,    binding = 0) uniform writeonly image2D uOut;
layout(rgba16f, binding = 1) uniform writeonly image2D uOutLocal;
uniform vec4 uSunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
uniform vec4 uFrame;        // x = jitter seed, y = tex width, z = tex height
uniform vec4 uLocalPosRange[4]; // xyz = local (point/spot) light position, w = range
uniform vec4 uLocalExtra;       // x = local light count

vec2 giHash2(uvec2 gid, float seed)
{
	vec2 p = vec2(gid) + seed * 13.37;
	return vec2(fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453),
	            fract(sin(dot(p, vec2(39.3468, 11.1352))) * 24634.6345));
}
vec3 giConeSample(vec3 L, float angleRad, vec2 xi)
{
	vec3 up = (abs(L.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 T  = normalize(cross(up, L));
	vec3 B  = cross(L, T);
	float r   = sin(angleRad) * sqrt(xi.x);
	float phi = 6.28318530718 * xi.y;
	return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
	uvec2 gid = gl_GlobalInvocationID.xy;
	if (float(gid.x) >= uFrame.y || float(gid.y) >= uFrame.z) return;
	vec4 pv = texelFetch(uGPos, ivec2(gid), 0);
	if (pv.a < 0.5) // background → everything unoccluded
	{
		imageStore(uOut,      ivec2(gid), vec4(1.0));
		imageStore(uOutLocal, ivec2(gid), vec4(1.0));
		return;
	}
	vec3 N = normalize(texelFetch(uGNorm, ivec2(gid), 0).xyz);
	vec3 L = uSunDirRadius.xyz;

	// ── Directional light (cone-jittered, temporally accumulated) ─────────
	float sunVis = 0.0;
	// Grazing/back-facing relative to the light: direct lighting's dot(N,L)
	// term already zeroes this out, so skip the trace entirely.
	if (dot(N, L) > 0.0)
	{
		vec2 xi  = giHash2(gid, uFrame.x);
		vec3 dir = giConeSample(L, max(uSunDirRadius.w, 1e-4), xi);
		// Same self-intersection guards as Metal: normal-offset origin + min t.
		vec3 origin = pv.xyz + N * 0.05;
		sunVis = giSceneAnyHit(origin, dir, 0.02, 10000.0) ? 0.0 : 1.0;
	}
	imageStore(uOut, ivec2(gid), vec4(sunVis));

	// ── Local (point/spot) lights: one HARD occlusion ray each toward the
	// first 4 — point/spot lights previously had NO shadowing at all (CSM
	// never covered them), so they shone straight through geometry. The rays
	// are deliberately UNjittered: deterministic → no noise → no temporal
	// pass needed, the mask reacts instantly and artefact-free. One visibility
	// channel per light, the scene shader indexes by its local-light counter.
	vec4 localVis = vec4(1.0);
	int localCount = clamp(int(uLocalExtra.x), 0, 4);
	for (int i = 0; i < localCount; ++i)
	{
		vec3  toL   = uLocalPosRange[i].xyz - pv.xyz;
		float distL = length(toL);
		if (distL <= 0.05) continue; // on top of the light → lit
		if (distL >= uLocalPosRange[i].w) continue; // outside the attenuation radius → contributes nothing, skip the ray
		vec3 dirL = toL / distL;
		if (dot(N, dirL) <= 0.0) { localVis[i] = 0.0; continue; }
		if (giSceneAnyHit(pv.xyz + N * 0.05, dirL, 0.02, max(distL - 0.1, 0.02)))
			localVis[i] = 0.0;
	}
	imageStore(uOutLocal, ivec2(gid), localVis);
}
)GLSL";

// Temporal accumulation: reproject via last frame's viewProj, history carries
// the world position (rgb) + shadow scalar (a). The tolerance is deliberately
// TIGHT (Metal lesson 58ee312: a loose depth-scaled tolerance accepts
// wrong-surface reprojects at cube edges). GL NDC → UV has NO y-flip.
static const char* kGiTemporalFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uGPos;
uniform sampler2D uRaw;
uniform sampler2D uHistory;
uniform mat4  uPrevViewProj;
uniform float uBlend; // history weight (0 on first GI frame)
out vec4 FragColor;
void main()
{
	vec4  pv   = texture(uGPos, vUV);
	float rawV = texture(uRaw, vUV).r;
	if (pv.a < 0.5) { FragColor = vec4(0.0, 0.0, 0.0, rawV); return; }

	vec4 clip = uPrevViewProj * vec4(pv.xyz, 1.0);
	if (clip.w <= 0.0) { FragColor = vec4(pv.xyz, rawV); return; }
	vec2 ndc    = clip.xy / clip.w;
	vec2 prevUV = ndc * 0.5 + 0.5;
	if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
	{ FragColor = vec4(pv.xyz, rawV); return; }

	vec4  hist      = texture(uHistory, prevUV);
	float posError  = length(pv.xyz - hist.rgb);
	float tolerance = clamp(0.02 * clip.w, 0.01, 0.06);
	float w = (posError < tolerance) ? clamp(uBlend, 0.0, 0.98) : 0.0;
	// Neighbourhood clamp: guards OCCLUDER motion (the position check above
	// only covers receiver/camera motion) — moved shadows update in 1-2 frames
	// instead of smearing for ~30.
	vec2 texel = 1.0 / vec2(textureSize(uRaw, 0));
	float nMin = rawV, nMax = rawV;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
		{
			float r = texture(uRaw, vUV + vec2(float(x), float(y)) * texel).r;
			nMin = min(nMin, r);
			nMax = max(nMax, r);
		}
	FragColor = vec4(pv.xyz, mix(rawV, clamp(hist.a, nMin, nMax), w));
}
)GLSL";

static const char* kGiBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uSrc; // temporal history: rgb = world pos, a = shadow
out vec4 FragColor;
void main()
{
	vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
	float sum = 0.0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			sum += texture(uSrc, vUV + vec2(float(x), float(y)) * texel).a;
	FragColor = vec4(sum / 9.0, 0.0, 0.0, 1.0);
}
)GLSL";

// DDGI probe update — gather formulation like Metal's giProbeUpdate: one
// thread per octahedral texel traces ITS OWN ray (no atomics; each thread owns
// exactly one texel per dispatch). One workgroup per probe in the frame's
// round-robin batch. Bounce estimate = dominant directional + up to 8 local
// lights (Metal lessons 5e45643/0787c23 baked in from the start).
static const char* kGiProbeCS = R"GLSL(
layout(rgba16f, binding = 0) uniform image2D uIrr;
layout(rg16f,   binding = 1) uniform image2D uVis;
uniform vec4 uGridOrigin;   // xyz = grid origin, w = spacing
uniform vec4 uGridCounts;   // xyz = probe counts, w = probesPerRow
uniform vec4 uRayParams;    // x = max dist, y = hysteresis, z = cursor start, w = probes this batch
uniform vec4 uSunDirRadius; // xyz = direction TOWARD the light, w = local light count
uniform vec4 uSunColor;     // rgb = colour * intensity
uniform vec4 uSkyAmbient;   // rgb = miss colour
uniform vec4 uLightPosRange[8];  // xyz pos, w range
uniform vec4 uLightColorType[8]; // rgb colour*intensity, w type (1 point, 2 spot)
uniform vec4 uLightDirCos[8];    // xyz spot travel dir, w cos(half angle)

const int kOctSize = 8; // must match OpenGLRenderer::kGIProbeOctSize

vec3 octDecode(vec2 e)
{
	vec3 n = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0)
	{
		vec2 signN = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (1.0 - abs(n.yx)) * signN;
	}
	return normalize(n);
}

// direction -> octahedral UV, inverse of octDecode (needed for the
// multi-bounce field lookup below; matches the scene shader's giOctEncode
// byte-for-byte).
vec2 octEncodeP(vec3 n)
{
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

// PREVIOUS-frame irradiance field at an arbitrary surface point: trilinear
// over the 8 surrounding probes, point-read of each probe's octahedral tile
// in the hit normal's direction. No Chebyshev here — this feeds the low-
// frequency multi-bounce term, where leaking is dampened by albedo anyway.
// uIrr is the READ_WRITE image this kernel EMA-updates — imageLoad gives the
// previous frame's values (only this thread's own texel is written later).
vec3 giSampleFieldIrradiance(vec3 pos, vec3 n)
{
	int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
	int probesPerRow = max(1, int(uGridCounts.w));
	float spacing = max(uGridOrigin.w, 1e-4);
	vec3 gridSpace = (pos - uGridOrigin.xyz) / spacing;
	vec3 base  = floor(gridSpace);
	vec3 fracP = gridSpace - base;
	vec2 oct = octEncodeP(n) * 0.5 + 0.5;
	ivec2 octTexel = ivec2(clamp(oct * float(kOctSize), 0.0, float(kOctSize) - 1.0));
	vec3  sum  = vec3(0.0);
	float sumW = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		vec3 offs = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		vec3 cell = base + offs;
		if (any(lessThan(cell, vec3(0.0))) ||
		    cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		vec3 tri = mix(1.0 - fracP, fracP, offs);
		float w = tri.x * tri.y * tri.z;
		if (w <= 1e-5) continue;
		int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
		ivec2 tile = ivec2((probeIndex % probesPerRow) * kOctSize,
		                   (probeIndex / probesPerRow) * kOctSize);
		sum  += imageLoad(uIrr, tile + octTexel).rgb * w;
		sumW += w;
	}
	return sum / max(sumW, 1e-4);
}

layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
	ivec2 texel   = ivec2(gl_LocalInvocationID.xy);
	int   batchIdx = int(gl_WorkGroupID.x);
	int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
	int probeCount = gx * gy * gz;
	if (probeCount <= 0 || batchIdx >= int(uRayParams.w)) return;
	int probeIndex = (int(uRayParams.z) + batchIdx) % probeCount;

	int pz = probeIndex / (gx * gy);
	int py = (probeIndex / gx) % gy;
	int px = probeIndex % gx;
	vec3 probePos = uGridOrigin.xyz + vec3(float(px), float(py), float(pz)) * uGridOrigin.w;

	vec2 uv  = (vec2(texel) + 0.5) / float(kOctSize) * 2.0 - 1.0;
	vec3 dir = octDecode(uv);

	float dist;
	int hitInst = giSceneClosestHit(probePos, dir, 0.01, max(uRayParams.x, 1.0), dist);

	vec3 radiance;
	if (hitInst < 0)
	{
		radiance = uSkyAmbient.rgb;
		dist     = uRayParams.x;
	}
	else
	{
		vec3 albedo    = giInsts[hitInst].baseColor.rgb;
		vec3 hitNormal = -dir;
		vec3 hitPos = probePos + dir * dist;
		float ndl = max(dot(hitNormal, uSunDirRadius.xyz), 0.0);
		// Secondary shadow ray: hit surfaces are no longer assumed fully
		// sun-lit — without this, probes flood shadowed regions with bright
		// sun bounce (objects under a large occluder visibly glow).
		if (ndl > 0.0 && giSceneAnyHit(hitPos + hitNormal * 0.05, uSunDirRadius.xyz, 0.02, 10000.0))
			ndl = 0.0;
		radiance = albedo * uSunColor.rgb * ndl;
		int lightCount = int(uSunDirRadius.w);
		for (int i = 0; i < lightCount; ++i)
		{
			vec3 toL = uLightPosRange[i].xyz - hitPos;
			float d  = max(length(toL), 1e-4);
			float range = max(uLightPosRange[i].w, 1e-4);
			if (d >= range) continue; // outside the attenuation radius
			vec3 L = toL / d;
			float ndl2 = max(dot(hitNormal, L), 0.0);
			if (ndl2 <= 0.0) continue;
			float atten = 1.0 - d / range;
			atten *= atten;
			if (uLightColorType[i].w > 1.5)
			{
				float c       = dot(-L, normalize(uLightDirCos[i].xyz));
				float cosCone = uLightDirCos[i].w;
				atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
			}
			if (atten <= 0.0) continue;
			// Secondary occlusion ray to the light — no bounce leaking.
			if (giSceneAnyHit(hitPos + hitNormal * 0.05, L, 0.02, max(d - 0.1, 0.02)))
				continue;
			radiance += albedo * uLightColorType[i].rgb * ndl2 * atten;
		}
		// Multi-bounce feedback (DDGI recursion): light already gathered in the
		// probe field re-reflects off this surface — a red wall visibly bleeds
		// red onto neighbouring geometry, and the series converges toward
		// infinite bounces through the EMA. albedo < 1 keeps it stable.
		radiance += albedo * giSampleFieldIrradiance(hitPos, hitNormal);
	}

	int probesPerRow = max(1, int(uGridCounts.w));
	ivec2 outCoord = ivec2((probeIndex % probesPerRow) * kOctSize + texel.x,
	                       (probeIndex / probesPerRow) * kOctSize + texel.y);

	// Adaptive hysteresis (see the Metal kernels): deterministic gather rays →
	// deltas are real scene changes; converge fast on change, stay smooth else.
	float baseH = clamp(uRayParams.y, 0.0, 0.98);
	vec4 oldIrr = imageLoad(uIrr, outCoord);
	float hIrr = mix(baseH, 0.3, clamp(length(radiance - oldIrr.rgb) * 4.0, 0.0, 1.0));
	imageStore(uIrr, outCoord, vec4(mix(radiance, oldIrr.rgb, hIrr), 1.0));
	vec4 oldVis = imageLoad(uVis, outCoord);
	vec2 newVisSample = vec2(dist, dist * dist);
	float hVis = mix(baseH, 0.3, clamp(abs(dist - oldVis.x) / max(uGridOrigin.w, 1.0), 0.0, 1.0));
	imageStore(uVis, outCoord, vec4(mix(newVisSample, oldVis.rg, hVis), 0.0, 0.0));
}
)GLSL";

// ─── Ray-traced GI reflections (docs/gi-reflections-plan.md §7 GL port) ───────
// One specular ray per half-res pixel against the SAME CPU BVH the shadow/probe
// kernels use, shaded from the sun + the DDGI probe field, so a reflection agrees
// with the diffuse GI around it. The result is composited UNDER the sky cubemap
// term in the shading pass (kUnlitFS and heLitP both), never inside the material
// preamble's sampler budget.
//
// Differences from the Metal original, all forced by the platform (there is no
// hardware ray query in GL): the trace is the software BVH walk, and there is no
// bounce loop — a mirror seen IN a mirror shows its flat base colour. What GL
// gets for free instead is a REAL hit normal (giSceneClosestHitTri returns the
// triangle, so no `-rayDir` stand-in) and an exact temporal reprojection (the
// pre-pass stores world positions, so no depth-based reconstruction).
static const char* kGiReflCS = R"GLSL(
uniform sampler2D uGPos;      // half-res world position, a = 1 valid geometry
uniform sampler2D uGNorm;     // half-res world normal
uniform sampler2D uGMat;      // r = roughness, g = metallic (receiver)
uniform sampler2D uGIIrr;     // DDGI irradiance atlas (dummy when GI diffuse is off)
uniform sampler2D uGIVis;     // DDGI visibility atlas
layout(rgba16f, binding = 0) uniform writeonly image2D uOut; // rgb radiance, a confidence
uniform vec4 uCamPos;      // xyz = camera world position
uniform vec4 uSunDir;      // xyz = direction TOWARD the sun, w = local light count
uniform vec4 uSunColor;    // rgb = sun radiance * intensity
uniform vec4 uAmbient;     // rgb = never-black floor applied at a hit
uniform vec4 uGridOrigin;  // xyz = probe-grid origin, w = spacing
uniform vec4 uGridCounts;  // xyz = probes per axis, w = probes per atlas row
uniform vec4 uReflParams;  // x = max ray distance, y = max roughness, z = indirect intensity, w = probe atlases valid
uniform vec4 uFrame;       // x = jitter seed, y = width, z = height, w = 1 → glossy cone jitter
uniform vec4 uLightPosRange[8];  // xyz pos, w range
uniform vec4 uLightColorType[8]; // rgb colour*intensity, w type (1 point, 2 spot)
uniform vec4 uLightDirCos[8];    // xyz spot travel dir, w cos(half angle)

const int kOctSize = 8; // must match OpenGLRenderer::kGIProbeOctSize

// SYNC: byte-for-byte the scene shader's giOctEncode / sampleDDGIIrradiance
// (kUnlitFS above) — the sampled variant WITH the Chebyshev visibility test, not
// the probe kernel's imageLoad twin. A drift here shows up as a reflected
// surface disagreeing with the same surface seen directly.
vec2 giOctEncode(vec3 n)
{
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

vec3 sampleDDGIIrradiance(vec3 P, vec3 N)
{
	int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
	if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
	int probesPerRow = max(1, int(uGridCounts.w));
	int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
	vec2 atlasSizeTexels = vec2(float(probesPerRow), float(probeRows)) * float(kOctSize);
	float spacing = max(uGridOrigin.w, 1e-4);

	vec3 gridSpace = (P - uGridOrigin.xyz) / spacing;
	vec3 base      = floor(gridSpace);
	vec3 fracP     = gridSpace - base;

	vec3  sumColor  = vec3(0.0);
	float sumWeight = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		vec3 offs = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		vec3 cell = base + offs;
		if (any(lessThan(cell, vec3(0.0))) ||
		    cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
			continue;
		int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

		vec3 trilinear = mix(1.0 - fracP, fracP, offs);
		float weight = trilinear.x * trilinear.y * trilinear.z;
		if (weight <= 1e-5) continue;

		vec3 probePos   = uGridOrigin.xyz + cell * spacing;
		vec3 toProbe    = probePos - P;
		float dist      = max(length(toProbe), 1e-4);
		vec3 dirToProbe = toProbe / dist;

		weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

		vec2 tileOrigin = vec2(float(probeIndex % probesPerRow),
		                       float(probeIndex / probesPerRow)) * float(kOctSize);

		vec2 visUV = (tileOrigin + (giOctEncode(-dirToProbe) * 0.5 + 0.5) * float(kOctSize)) / atlasSizeTexels;
		vec2 visSample = texture(uGIVis, visUV).rg;
		float mean = visSample.x, mean2 = visSample.y;
		float variance = abs(mean2 - mean * mean);
		float chebyshev = 1.0;
		if (dist > mean)
		{
			float d = dist - mean;
			chebyshev = variance / (variance + d * d);
			chebyshev = chebyshev * chebyshev * chebyshev;
		}
		weight *= max(chebyshev, 0.05);

		vec2 irrUV = (tileOrigin + (giOctEncode(N) * 0.5 + 0.5) * float(kOctSize)) / atlasSizeTexels;
		sumColor  += texture(uGIIrr, irrUV).rgb * weight;
		sumWeight += weight;
	}
	return sumColor / max(sumWeight, 1e-4);
}

// SYNC: the same hash/cone pair as kGiShadowCS (separate compilation unit —
// GLSL has no #include, and the traversal prefix is shared but these are not).
vec2 giHash2(uvec2 gid, float seed)
{
	vec2 p = vec2(gid) + seed * 13.37;
	return vec2(fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453),
	            fract(sin(dot(p, vec2(39.3468, 11.1352))) * 24634.6345));
}
vec3 giConeSample(vec3 L, float angleRad, vec2 xi)
{
	vec3 up = (abs(L.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 T  = normalize(cross(up, L));
	vec3 B  = cross(L, T);
	float r   = sin(angleRad) * sqrt(xi.x);
	float phi = 6.28318530718 * xi.y;
	return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
	uvec2 gid = gl_GlobalInvocationID.xy;
	if (float(gid.x) >= uFrame.y || float(gid.y) >= uFrame.z) return;
	ivec2 px = ivec2(gid);
	// NORMALIZED sample of the prepass, not a texel fetch: the reflection target
	// has its own resolution now (the quality tier picks it), so the two grids no
	// longer line up 1:1. Nearest filtering keeps the old behaviour where they do.
	vec2 guv = (vec2(gid) + 0.5) / vec2(uFrame.y, uFrame.z);

	vec4 pv = texture(uGPos, guv);
	// Background, or a receiver too rough to show a traced reflection: confidence
	// 0 means the composite keeps the sky cubemap term unchanged, which is the
	// correct answer for both.
	if (pv.a < 0.5) { imageStore(uOut, px, vec4(0.0)); return; }
	vec2 rm = texture(uGMat, guv).rg;
	if (rm.x > uReflParams.y) { imageStore(uOut, px, vec4(0.0)); return; }

	vec3 N = normalize(texture(uGNorm, guv).xyz);
	vec3 V = normalize(uCamPos.xyz - pv.xyz);
	vec3 R = reflect(-V, N);
	// Near-mirrors keep a single deterministic ray: their specular lobe is
	// essentially a delta, and even a 1° cone walks the hit point far enough at
	// distance to flip across a silhouette — a dithered band exactly where the
	// reflection should be sharpest. 0.08 is below any surface a viewer would
	// call glossy rather than mirrored.
	// Rays per pixel + cone width come from the quality tier (uRays): the tier
	// means how many rays and how much blur, nothing else. ONE jittered ray with
	// the temporal EMA as its only integrator is what made the top tier look
	// noisier than the one below it. A near-mirror cone is narrower than a pixel,
	// so it stays at a single deterministic ray and costs what it always did.
	float coneW = (uFrame.w > 0.5 && rm.x > 0.08) ? rm.x * 0.5 : 0.0;
	int rays = (coneW > 1e-3) ? clamp(int(uRays), 1, 8) : 1;
	// Normal-offset origin + a min-t, the same self-intersection guards the
	// shadow kernel uses.
	vec3 origin = pv.xyz + N * 0.05;

	vec3  outRadiance = vec3(0.0);
	float outConf     = 0.0;
	for (int sIdx = 0; sIdx < rays; ++sIdx)
	{
	// Stratified over the sample index AND the frame, so this frame's rays
	// spread across the lobe instead of clumping.
	vec3 Rs = R;
	if (coneW > 1e-3)
	{
		vec2 xi = giHash2(gid, uFrame.x + float(sIdx) * 7.13);
		Rs = giConeSample(R, coneW, vec2((float(sIdx) + xi.x) / float(rays), xi.y));
		if (dot(Rs, N) <= 0.0) Rs = R;
	}

	float dist; int tri;
	int hitInst = giSceneClosestHitTri(origin, Rs, 0.02, max(uReflParams.x, 1.0), dist, tri);
	if (hitInst < 0) continue; // miss → this sample contributes nothing (sky)

	vec3 hitPos = origin + Rs * dist;
	vec3 hitN   = giHitNormal(hitInst, tri, Rs);
	vec3 albedo = giInsts[hitInst].baseColor.rgb;
	// Landscape hit: the paint at THIS point instead of one flat colour for the
	// whole terrain. A landscape is a heightfield over an axis-aligned local
	// rect whose mesh UVs are linear in it, so the hit POSITION recovers the UV
	// — no per-vertex UV in the BVH. Mirrors the Metal kernels (GiLandscape.h).
	int li = giInsts[hitInst].offsets.z;
	if (li >= 0 && li < uGiLandCount && li < 4 && int(giLands[li].cfg.w) > 0)
	{
		vec3 lp = (giLands[li].worldToLocal * vec4(hitPos, 1.0)).xyz;
		vec2 luv = (vec2(lp.x, lp.z) * giLands[li].cfg.xy + 0.5) * giLands[li].cfg.z;
		// Dynamic indexing of a sampler array is not allowed below GL 4.0 rules
		// for non-uniform indices, so branch on the (uniform-per-instance) index.
		vec4 lw = (li == 0) ? texture(uLandWeights[0], luv)
		        : (li == 1) ? texture(uLandWeights[1], luv)
		        : (li == 2) ? texture(uLandWeights[2], luv)
		                    : texture(uLandWeights[3], luv);
		vec3 lsum = vec3(0.0);
		float wsum = 0.0;
		int layers = int(giLands[li].cfg.w);
		for (int k = 0; k < layers && k < 4; ++k)
		{
			if (lw[k] <= 0.0) continue;
			lsum += giLands[li].layer[k].rgb * lw[k];
			wsum += lw[k];
		}
		albedo = (wsum > 1e-4) ? lsum / wsum : giLands[li].layer[0].rgb;
	}

	// ── Hit shading: one bounce, then the probe field ────────────────────────
	// Emissive first (a glowing object is visible in a mirror even unlit); the
	// probe field deliberately does NOT carry emissive, so this is the only
	// place it enters.
	vec3 radiance = giInsts[hitInst].emissive.rgb;
	float ndl = max(dot(hitN, uSunDir.xyz), 0.0);
	// Skip the occlusion ray when the sun term is black anyway (night) — the
	// visibility result would be multiplied by zero.
	if (ndl > 0.0 && dot(uSunColor.rgb, vec3(1.0)) > 1e-5)
	{
		if (giSceneAnyHit(hitPos + hitN * 0.05, uSunDir.xyz, 0.02, 10000.0)) ndl = 0.0;
		radiance += albedo * uSunColor.rgb * ndl;
	}
	// Local (point/spot) lights — the same loop, attenuation and occlusion ray
	// as the probe kernel, so a lamp lights a reflected wall like a real one.
	int lightCount = int(uSunDir.w);
	for (int i = 0; i < lightCount; ++i)
	{
		vec3 toL = uLightPosRange[i].xyz - hitPos;
		float d  = max(length(toL), 1e-4);
		float range = max(uLightPosRange[i].w, 1e-4);
		if (d >= range) continue;
		vec3 L = toL / d;
		float ndl2 = max(dot(hitN, L), 0.0);
		if (ndl2 <= 0.0) continue;
		float atten = 1.0 - d / range;
		atten *= atten;
		if (uLightColorType[i].w > 1.5)
		{
			float c       = dot(-L, normalize(uLightDirCos[i].xyz));
			float cosCone = uLightDirCos[i].w;
			atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
		}
		if (atten <= 0.0) continue;
		if (giSceneAnyHit(hitPos + hitN * 0.05, L, 0.02, max(d - 0.1, 0.02))) continue;
		radiance += albedo * uLightColorType[i].rgb * ndl2 * atten;
	}
	// Indirect: the probe field at the hit — this is what makes the reflection
	// "GI-based" rather than a second, differently-lit scene. Only when the
	// atlases are actually filled (GI diffuse on); the flat floor is added
	// either way, exactly like the scene shader's never-black guarantee.
	if (uReflParams.w > 0.5)
		radiance += albedo * sampleDDGIIrradiance(hitPos, hitN) * uReflParams.z;
	radiance += albedo * uAmbient.rgb;

	// Confidence fades out toward the ray's far end: a hit at the very edge of
	// the traced range is as likely to be wrong (probe grid ends, geometry
	// outside the BVH) as right, and fading into the sky term is the graceful
	// failure. The composite multiplies this by the intensity setting.
	float conf = 1.0 - smoothstep(uReflParams.x * 0.75, uReflParams.x, dist);
	outRadiance += radiance;
	outConf     += conf;
	}
	// Averaged over the rays this pixel actually traced. A sample that MISSED
	// contributed nothing to either, so a partly-missing lobe comes out with a
	// proportionally lower confidence and the composite blends that much sky in
	// — which is exactly what a lobe half off the geometry should look like.
	imageStore(uOut, px, vec4(outRadiance / float(rays), outConf / float(rays)));
}
)GLSL";

// Temporal accumulation for the reflection trace (quality High). Same shape as
// kGiTemporalFS, but the history is RGB radiance + confidence, so the receiver
// world position it rejects against needs its own attachment instead of riding
// in the unused channels.
static const char* kGiReflTemporalFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uGPos;
uniform sampler2D uRaw;
uniform sampler2D uHistory;
uniform sampler2D uHistPos;
uniform mat4  uPrevViewProj;
uniform float uBlend;  // history weight (0 = no usable history / camera cut)
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oPos;
void main()
{
	vec4 pv  = texture(uGPos, vUV);
	vec4 raw = texture(uRaw,  vUV);
	oPos = vec4(pv.xyz, pv.a);            // this frame's receiver, for the NEXT frame
	if (pv.a < 0.5) { oColor = vec4(0.0); return; }

	vec4 clip = uPrevViewProj * vec4(pv.xyz, 1.0);
	if (clip.w <= 0.0) { oColor = raw; return; }
	vec2 prevUV = (clip.xy / clip.w) * 0.5 + 0.5;
	if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
	{ oColor = raw; return; }

	vec4  hist = texture(uHistory, prevUV);
	vec4  hp   = texture(uHistPos, prevUV);
	float posError  = length(pv.xyz - hp.xyz);
	float tolerance = clamp(0.02 * clip.w, 0.01, 0.06);
	float w = (hp.a > 0.5 && posError < tolerance) ? clamp(uBlend, 0.0, 0.98) : 0.0;
	// Neighbourhood clamp — the SAME guard kGiTemporalFS uses, and the reason it
	// is needed here is stronger: the position test above only validates the
	// RECEIVER, and reflected content is never reprojected (the engine has no
	// motion vectors). Rejecting outright on a radiance break would also throw
	// away the accumulation at every hit/miss boundary of the jittered glossy
	// rays — which is exactly where it is needed, and shows up as a band of
	// bright speckles. Clamping instead keeps jitter noise (inside the local
	// range) accumulating in full while a genuinely changed reflection is pulled
	// onto its new value within a frame or two.
	vec2 texel = 1.0 / vec2(textureSize(uRaw, 0));
	vec4 nMin = raw, nMax = raw;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
		{
			vec4 s = texture(uRaw, vUV + vec2(float(x), float(y)) * texel);
			nMin = min(nMin, s);
			nMax = max(nMax, s);
		}
	oColor = mix(raw, clamp(hist, nMin, nMax), w);
}
)GLSL";

// Separable 5-tap Gaussian over the half-res reflection result (quality Medium
// and up). CONFIDENCE-WEIGHTED: a miss (a = 0) contributes no colour, so the
// edges of a hit region do not bleed toward black — the blurred confidence
// feathers them instead. Same reasoning as the SSR blur in the shared library.
static const char* kGiReflBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uSrc;
uniform vec2 uDir; // one-texel UV step along the blur axis
out vec4 FragColor;
void main()
{
	float w[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);
	vec3  sumC = vec3(0.0);
	float sumA = 0.0;
	for (int i = -2; i <= 2; ++i)
	{
		vec4  s  = texture(uSrc, vUV + uDir * float(i));
		float wi = w[i + 2];
		sumC += s.rgb * s.a * wi;
		sumA += s.a * wi;
	}
	FragColor = vec4(sumC / max(sumA, 1e-4), sumA);
}
)GLSL";

// Roughness lerp between the SHARP trace and the blurred copy — GL's counterpart
// of Metal's kSSRRoughMixFS. Without it GL had only ONE reflection texture, so
// the blur (now sized to the glossy lobe, i.e. wide) landed on mirrors too and a
// mirror came out soft at every tier. The sharp copy may only win where the trace
// is DETERMINISTIC: the kernel opens its cone above roughness 0.08, and from
// there every ray is a random sample, so the sharp copy is raw noise, not detail.
// uFloor is the resolution floor — below full resolution even a mirror needs some
// blur, or its upsampled stair-steps show.
static const char* kGiReflMixFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uSharp;
uniform sampler2D uBlurred;
uniform sampler2D uGMat;   // r = roughness, g = metallic (the trace's own prepass)
uniform float uFloor;
out vec4 FragColor;
void main()
{
	float rough = clamp(texture(uGMat, vUV).r, 0.0, 1.0);
	float t = max(clamp(uFloor, 0.0, 1.0), smoothstep(0.08, 0.25, rough));
	FragColor = mix(texture(uSharp, vUV), texture(uBlurred, vUV), t);
}
)GLSL";

static const char* kSSAOPosVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;        // clip-space (viewProj * model) — matches the scene pass
uniform mat4 uModelView;  // view * model — gives the view-space position
out vec3 vViewPos;
void main()
{
	vViewPos    = (uModelView * vec4(aPos, 1.0)).xyz;
	gl_Position = uMVP * vec4(aPos, 1.0);
}
)GLSL";

// Instanced twin of kSSAOPosVS for a GeometryPass batch (dc.instanceTransforms):
// the per-instance model matrix comes from attrib locs 4–7 / m_instanceVBO, the
// binding every mesh VAO already carries for kInstancedVS, so the pre-pass
// reuses the scene pass's instance buffer as is. View and view-proj are the
// batch-constant uniforms; the two products happen per vertex.
static const char* kSSAOPosInstancedVS = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aInstCol0;
layout(location = 5) in vec4 aInstCol1;
layout(location = 6) in vec4 aInstCol2;
layout(location = 7) in vec4 aInstCol3;
uniform mat4 uViewProj;
uniform mat4 uView;
out vec3 vViewPos;
void main()
{
	mat4 model  = mat4(aInstCol0, aInstCol1, aInstCol2, aInstCol3);
	vec4 world  = model * vec4(aPos, 1.0);
	vViewPos    = (uView * world).xyz;
	gl_Position = uViewProj * world;
}
)GLSL";

static const char* kSSAOPosFS = R"GLSL(
#version 410 core
in vec3 vViewPos;
out vec4 FragColor;
void main() { FragColor = vec4(vViewPos, 1.0); } // a = 1 → valid geometry
)GLSL";

// Deferred (plan P5): reconstruct the view-space position from the G-buffer
// depth instead of re-rasterizing the scene — the whole geometry pre-pass above
// collapses into one fullscreen draw when the G-buffer exists this frame.
// Shares the tonemap fullscreen-triangle VS (vUV, bottom-left origin, matching
// the GL depth convention d*2-1 → ndc z).
static const char* kSSAODepthPosFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uDepth;    // G-buffer depth texture
uniform mat4      uInvProj;  // inverse camera projection (GL convention)
out vec4 FragColor;
void main()
{
	float d = texture(uDepth, vUV).r;
	if (d >= 1.0) { FragColor = vec4(0.0); return; } // background → a = 0
	vec4 clip = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
	vec4 v = uInvProj * clip;
	FragColor = vec4(v.xyz / max(v.w, 1e-8), 1.0);   // a = 1 → valid geometry
}
)GLSL";

// Occlusion estimate (fullscreen, shares the tonemap fullscreen-triangle VS).
// Reconstructs the view-space normal from neighbouring positions, then runs the
// AO method selected by uAOMethod (0 = SSAO tangent-plane kernel, 1 = HBAO
// horizon/visibility-bitmask, 2 = GTAO analytic horizon-arc integral).
// uAOMethod is a *uniform* so the branch is coherent
// across the whole pass (no divergence). Mirrors the Metal pass except the UV y
// flip (GL framebuffers are bottom-up; Metal is top-left).
static const char* kSSAOFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uViewPos;     // RGBA16F: xyz view-space pos, a = valid
uniform sampler2D uNoise;       // 4×4 random rotation vectors (xy in [-1,1]) — SSAO only
uniform mat4      uProj;        // camera projection (GL convention)
uniform vec2      uNoiseScale;  // viewport / 4 (tiles the noise across the screen)
uniform float     uRadius;
uniform float     uBias;
uniform float     uIntensity;
uniform vec3      uKernel[32];  // hemisphere kernel — SSAO only
uniform int       uAOMethod;    // 0 = SSAO, 1 = HBAO, 2 = GTAO
out vec4 FragColor;

const float PI = 3.14159265359, TWO_PI = 6.28318530718, HALF_PI = 1.57079632679;

// HBAO: OR the angular sectors [minH,maxH] (each normalised to [0,1] across the
// hemisphere arc) into a 32-bit visibility bitmask.
uint hbaoSectors(float minH, float maxH, uint mask)
{
	uint startBit = min(uint(clamp(minH, 0.0, 1.0) * 32.0), 31u);
	uint count    = uint(ceil(clamp(maxH - minH, 0.0, 1.0) * 32.0));
	uint bits     = (count > 0u) ? (0xFFFFFFFFu >> (32u - count)) : 0u;
	return mask | (bits << startBit);
}
// Interleaved-gradient noise for the per-pixel slice/step jitter (Jimenez 2014).
float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

void main()
{
	vec4 pv = texture(uViewPos, vUV);
	if (pv.a < 0.5) { FragColor = vec4(1.0); return; } // background → unoccluded
	vec3 P = pv.xyz;

	// View-space normal from neighbouring positions, picking the nearer side on
	// each axis so silhouettes don't bleed a wrong normal across depth edges.
	vec2 texel = 1.0 / vec2(textureSize(uViewPos, 0));
	vec3 Pr = texture(uViewPos, vUV + vec2(texel.x, 0.0)).xyz;
	vec3 Pl = texture(uViewPos, vUV - vec2(texel.x, 0.0)).xyz;
	vec3 Pu = texture(uViewPos, vUV + vec2(0.0, texel.y)).xyz;
	vec3 Pd = texture(uViewPos, vUV - vec2(0.0, texel.y)).xyz;
	vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
	vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
	vec3 N = normalize(cross(ddx, ddy));
	if (N.z < 0.0) N = -N;                       // face the camera (+Z in view space)

	float ao;
	if (uAOMethod == 1)
	{
		// ── HBAO: horizon-based AO via a 32-sector visibility bitmask ───────────
		// (Therrien et al., "Screen Space Indirect Lighting with Visibility Bitmask",
		// AO-only.) Per slice we OR the sectors blocked by each marched neighbour into
		// a mask; visibility = fraction of unblocked sectors. Horizon-based ⇒ a flat
		// surface reads visibility ≈ 1 at any view angle (no grazing self-occlusion).
		const int   SLICES = 3;
		const int   STEPS  = 8;
		const float THICKNESS = 0.5;                 // assumed occluder depth (view units)
		vec3  V = normalize(-P);                      // camera at the view-space origin
		float jitter = ign(gl_FragCoord.xy) - 0.5;
		float depthScale = 0.5 * uRadius / max(-P.z, 1e-4);
		float visibility = 0.0;
		for (int s = 0; s < SLICES; ++s)
		{
			float phi = (float(s) + jitter) * (TWO_PI / float(SLICES));
			vec2  omega = vec2(cos(phi), sin(phi));
			vec3  dir = vec3(omega, 0.0);
			vec3  orthoDir = dir - dot(dir, V) * V;
			vec3  axis = cross(dir, V);
			vec3  projN = N - axis * dot(N, axis);   // normal projected into the slice plane
			float projLen = length(projN);
			if (projLen < 1e-5) { visibility += 1.0; continue; }
			float nAng = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, 0.0, 1.0));
			// March the slice direction in UV — proj scales x/y so the footprint is the
			// correct view-space circle and the samples lie in this slice's plane.
			vec2 omegaUV = vec2(uProj[0][0] * omega.x, uProj[1][1] * omega.y);
			uint occ = 0u;
			for (int i = 0; i < STEPS; ++i)
			{
				float t   = (float(i) + jitter) / float(STEPS) + 0.01;
				vec2  sUV = vUV - t * depthScale * omegaUV;   // GL: uv.y up
				vec4  sp  = texture(uViewPos, sUV);
				if (sp.a < 0.5) continue;
				vec3  d   = sp.xyz - P;
				float len = length(d);
				vec2  fb;
				fb.x = dot(d / max(len, 1e-5), V);                    // front horizon
				fb.y = dot(normalize(d - V * THICKNESS), V);         // back (thickness)
				fb   = acos(clamp(fb, -1.0, 1.0));
				fb   = clamp((fb + nAng + HALF_PI) / PI, 0.0, 1.0);  // → sector space
				occ  = hbaoSectors(min(fb.x, fb.y), max(fb.x, fb.y), occ);
			}
			visibility += 1.0 - float(bitCount(occ)) / 32.0;
		}
		visibility /= float(SLICES);
		ao = 1.0 - (1.0 - visibility) * uIntensity;
		ao = max(ao, 0.1);                            // backstop against pure black
	}
	else if (uAOMethod == 2)
	{
		// ── GTAO: Ground-Truth AO (Jiménez et al. 2016) ────────────────────────
		// Reuses the HBAO slice setup, but instead of a coverage bitmask it finds
		// the max horizon angle on EACH side of the slice line and integrates
		// visibility analytically over the cosine-weighted hemisphere arc between
		// them: V = 0.25·|projN|·Σ(−cos(2h−γ)+cos γ+2h·sin γ). γ = angle of the
		// surface normal projected into the slice plane (relative to V). Slices
		// span [0,π) since each line covers both ± directions.
		const int SLICES = 3;
		const int STEPS  = 8;
		vec3  V = normalize(-P);
		float jitter = ign(gl_FragCoord.xy);
		float depthScale = 0.5 * uRadius / max(-P.z, 1e-4);
		float visAccum = 0.0;
		for (int s = 0; s < SLICES; ++s)
		{
			float phi = (float(s) + jitter) * (PI / float(SLICES));
			vec2  omega = vec2(cos(phi), sin(phi));
			vec3  dir = vec3(omega, 0.0);
			vec3  axis = cross(dir, V);
			float axisLen = length(axis);
			if (axisLen < 1e-5) { visAccum += 1.0; continue; }
			axis /= axisLen;
			vec3  orthoDir = normalize(dir - dot(dir, V) * V); // in-plane ⟂ V, toward +omega
			vec3  projN = N - axis * dot(N, axis);             // normal into slice plane
			float projLen = length(projN);
			if (projLen < 1e-5) continue;                      // normal ⟂ slice → no AO here
			float gamma = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, -1.0, 1.0));
			vec2  omegaUV = vec2(uProj[0][0] * omega.x, uProj[1][1] * omega.y);
			float cH1 = 0.0;   // +omega side horizon cosine (vs V); 0 ⇒ no occluder
			float cH2 = 0.0;   // -omega side
			for (int i = 0; i < STEPS; ++i)
			{
				float t = (float(i) + jitter) / float(STEPS) + 0.02;
				vec4  sp1 = texture(uViewPos, vUV + t * depthScale * omegaUV); // GL uv.y up
				if (sp1.a >= 0.5) {
					vec3 d = sp1.xyz - P; float len = length(d);
					float fall = clamp(1.0 - len / uRadius, 0.0, 1.0);
					cH1 = max(cH1, (dot(d, V) / max(len, 1e-5)) * fall);
				}
				vec4  sp2 = texture(uViewPos, vUV - t * depthScale * omegaUV);
				if (sp2.a >= 0.5) {
					vec3 d = sp2.xyz - P; float len = length(d);
					float fall = clamp(1.0 - len / uRadius, 0.0, 1.0);
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
		ao = 1.0 - (1.0 - visibility) * uIntensity;
		ao = max(ao, 0.1);                            // backstop against pure black
	}
	else
	{
		// ── SSAO: slope-invariant tangent-plane kernel ─────────────────────────
		vec3 randv = texture(uNoise, vUV * uNoiseScale).xyz;
		vec3 T = normalize(randv - N * dot(randv, N)); // Gram-Schmidt
		vec3 B = cross(N, T);
		mat3 TBN = mat3(T, B, N);
		float occ = 0.0;
		for (int i = 0; i < 32; ++i)
		{
			// The kernel only chooses WHICH nearby screen pixels to inspect (a hemisphere
			// footprint of radius uRadius around P, oriented to the surface).
			vec3 sp = P + (TBN * uKernel[i]) * uRadius;
			vec4 clip = uProj * vec4(sp, 1.0);
			vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;        // GL: ndc.y up → uv.y up
			if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
			vec4 sv = texture(uViewPos, suv);
			if (sv.a < 0.5) continue;                          // sampled the background
			// Slope-invariant occlusion: how far the sampled neighbour rises ABOVE this
			// fragment's own tangent plane (P, N). A flat surface — even edge-on — has
			// its neighbours IN the plane (dot ≈ 0) and can't occlude itself.
			vec3  toOcc = sv.xyz - P;
			float above = dot(toOcc, N);
			float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(length(toOcc), 1e-4));
			occ += (above > uBias ? 1.0 : 0.0) * rangeCheck;
		}
		ao = 1.0 - (occ / 32.0) * uIntensity;
		ao = max(ao, 0.5);                            // conservative backstop
	}
	FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

// 4×4 box blur to remove the noise-rotation pattern. Single channel (R).
static const char* kSSAOBlurFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uAOInput;
out vec4 FragColor;
void main()
{
	vec2 texel = 1.0 / vec2(textureSize(uAOInput, 0));
	float sum = 0.0;
	for (int x = -2; x < 2; ++x)
		for (int y = -2; y < 2; ++y)
			sum += texture(uAOInput, vUV + vec2(float(x), float(y)) * texel).r;
	float ao = sum / 16.0;
	FragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";

// ── In-Game UI (2D canvas) ──────────────────────────────────────────────────
// Attribute-less: position is derived from gl_VertexID (0-3 for TRIANGLE_STRIP)
// and the per-quad uniforms uRect (x,y,w,h pixels) + uViewport (vpW,vpH pixels).
// uUVRect = {u0, v0, u1, v1} into the font atlas (glyph quads); uMode: 0 = solid
// color, 1 = font-atlas glyph (alpha from the atlas R channel). Mirrors kUIMSL.
static const char* kUIVS = R"GLSL(
#version 410 core
uniform vec4 uRect;
uniform vec2 uViewport;
uniform vec4 uUVRect;
// { angle(radians), pivotX(px), pivotY(px), unused }; angle 0 = upright.
uniform vec4 uRotation;
out vec2 vUV;
out vec2 vLocal;
void main()
{
    const vec2 c[4] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 uv = c[gl_VertexID];
    vec2 sp = uRect.xy + uv * uRect.zw;
    if (uRotation.x != 0.0)
    {
        float sa = sin(uRotation.x), ca = cos(uRotation.x);
        vec2 d = sp - uRotation.yz;
        sp = uRotation.yz + vec2(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
    }
    vUV = mix(uUVRect.xy, uUVRect.zw, uv);
    vLocal = uv;                 // 0..1 across the quad (for the rounded-rect SDF)
    gl_Position = vec4(sp.x / uViewport.x * 2.0 - 1.0,
                       1.0 - sp.y / uViewport.y * 2.0,
                       0.0, 1.0);
}
)GLSL";

static const char* kUIFS = R"GLSL(
#version 410 core
in vec2 vUV;
in vec2 vLocal;
uniform vec4 uColor;
uniform float uMode;
uniform vec4 uRect;         // xy=pos, zw=size (px) — for the SDF
uniform vec4  uCornerRadius; // px per corner: TL, TR, BR, BL; all at min(w,h)/2 → circle
uniform float uBorderWidth;  // px, drawn INSIDE the quad; 0 = none
uniform vec4  uBorderColor;
uniform float uGradient;      // 0 = solid, 1 = linear fade to uGradientColor
uniform vec4  uGradientColor;
uniform float uGradientAngle; // degrees, clockwise from "down"
uniform float uGradientShape; // 0 = linear along the angle, 1 = radial from the centre
uniform float uBlur;          // px: > 0 = this quad IS a drop shadow (soft edge)
uniform float uInnerBlur;     // px: > 0 = a shadow cast inwards from the edge
uniform vec4  uInnerColor;
uniform sampler2D uFontAtlas;
out vec4 FragColor;
// One rounded box, four radii. `p` is relative to the box's centre (y down),
// `radii` is TL, TR, BR, BL: the quadrant `p` falls in picks its corner and the
// rest is the ordinary rounded-box distance. Mirrors heRoundedBoxSDF in the
// Metal path exactly — one rule, two languages. All four equal gives the
// formula that stood here before, unchanged.
float heRoundedBoxSDF(vec2 p, vec2 halfSz, vec4 radii)
{
    float r = (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y)
                          : ((p.y > 0.0) ? radii.w : radii.x);
    r = min(r, min(halfSz.x, halfSz.y));
    vec2 q = abs(p) - (halfSz - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float heMaxRadius(vec4 radii)
{
    return max(max(radii.x, radii.y), max(radii.z, radii.w));
}
// uMode: 0 = solid colour, 1 = font-atlas glyph (alpha from .r), 2 = textured
// quad (RGBA, tinted by uColor). Modes 1 and 2 share the sampler: a glyph run
// binds the atlas on unit 0, an image its own texture.
void main()
{
    if (uMode > 0.5 && uMode < 1.5)
    {
        float a = texture(uFontAtlas, vUV).r;
        FragColor = vec4(uColor.rgb, uColor.a * a);
        return;
    }
    if (uMode > 1.5)
    {
        vec4 t = texture(uFontAtlas, vUV);
        vec4 c = vec4(uColor.rgb * t.rgb, uColor.a * t.a);
        if (heMaxRadius(uCornerRadius) <= 0.0) { FragColor = c; return; }
        // A rounded image is the solid path's SDF applied to the sampled alpha.
        float dd = heRoundedBoxSDF((vLocal - 0.5) * uRect.zw, uRect.zw * 0.5, uCornerRadius);
        FragColor = vec4(c.rgb, c.a * clamp(0.5 - dd, 0.0, 1.0));
        return;
    }
    // The surface colour before any shape is cut out of it. Same rule as the
    // Metal path (uiFragment): a fade along an angle clockwise from "down",
    // projected onto the quad's own 0..1 space so it follows the box.
    vec4 fill = uColor;
    if (uGradient > 0.5)
    {
        float t;
        if (uGradientShape > 0.5)
        {
            // Radial: centre out to the FARTHEST CORNER, measured in pixels so
            // the circle follows the box instead of coming out an ellipse.
            vec2 dpx = (vLocal - 0.5) * uRect.zw;
            t = clamp(length(dpx) / max(1e-4, length(uRect.zw * 0.5)), 0.0, 1.0);
        }
        else
        {
            float a = uGradientAngle * 0.017453292;
            vec2  dir = vec2(sin(a), cos(a));
            t = clamp(dot(vLocal - 0.5, dir) + 0.5, 0.0, 1.0);
        }
        fill = mix(uColor, uGradientColor, t);
    }
    // Square AND borderless needs no distance field at all — the crisp fast path.
    if (heMaxRadius(uCornerRadius) <= 0.0 && uBorderWidth <= 0.0 &&
        uBlur <= 0.0 && uInnerBlur <= 0.0) { FragColor = fill; return; }
    // A blurred quad IS a drop shadow: the producer grew the rect by the blur on
    // every side, so the shape sits inset by exactly that much. Nothing else
    // applies to it — one colour with a soft edge. Mirrors the Metal path.
    vec2 halfsz = uRect.zw * 0.5 - uBlur;
    float d = heRoundedBoxSDF((vLocal - 0.5) * uRect.zw, halfsz, uCornerRadius);
    float cov = (uBlur > 0.0) ? (1.0 - smoothstep(-uBlur, uBlur, d))
                              : clamp(0.5 - d, 0.0, 1.0);
    if (uBlur > 0.0) { FragColor = vec4(fill.rgb, fill.a * cov); return; }
    // The inner shadow: `d` is negative inside, so -d is how deep in this pixel
    // is and the same falloff read the other way darkens the rim.
    if (uInnerBlur > 0.0)
    {
        float t = 1.0 - smoothstep(0.0, uInnerBlur, -d);
        float ia = uInnerColor.a * clamp(t, 0.0, 1.0);
        fill = vec4(mix(fill.rgb, uInnerColor.rgb, ia), fill.a);
    }
    if (uBorderWidth <= 0.0) { FragColor = vec4(fill.rgb, fill.a * cov); return; }
    // The ring between the shape and itself shrunk by the border width: `d` is a
    // signed distance in pixels, so the inner edge is d + width. Mirrors the
    // Metal path exactly (uiFragment) — one rule, two languages. The gradient is
    // the fill's, not the outline's.
    float inner = clamp(0.5 - (d + uBorderWidth), 0.0, 1.0);
    vec3  rgb   = mix(uBorderColor.rgb, fill.rgb, inner);
    float a     = mix(uBorderColor.a, fill.a, inner);
    FragColor = vec4(rgb, a * cov);
}
)GLSL";

static GLuint CompileStage(GLenum stage, const char* src)
{
	GLuint shader = glCreateShader(stage);
	glShaderSource(shader, 1, &src, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		glDeleteShader(shader);
		throw std::runtime_error(std::string("OpenGLRenderer: shader compile failed: ") + log);
	}
	return shader;
}

// Replaces the //#SKYFUNC# marker with the shared skyColor() so the skybox and
// scene shaders stay in sync from one source.
static std::string injectSkyFunc(const char* src)
{
	std::string s = src;
	const std::string marker = "//#SKYFUNC#";
	if (size_t pos = s.find(marker); pos != std::string::npos)
		s.replace(pos, marker.size(), kSkyFuncGLSL);
	return s;
}

// KHR_debug sink (see the HE_GL_DEBUG block in Initialize). Runs on the calling
// thread while debug output is synchronous, so the message names the GL call that
// produced it — the one thing the end-of-frame glGetError below can never say.
static void APIENTRY HeGLDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity,
                                      GLsizei /*length*/, const GLchar* message,
                                      const void* /*userParam*/)
{
	const char* sev = severity == GL_DEBUG_SEVERITY_HIGH   ? "HIGH"
	                : severity == GL_DEBUG_SEVERITY_MEDIUM ? "MEDIUM"
	                : severity == GL_DEBUG_SEVERITY_LOW    ? "LOW"
	                                                       : "NOTIFY";
	char buf[1024];
	std::snprintf(buf, sizeof(buf),
	              "OpenGL debug [%s] source=0x%04X type=0x%04X id=%u: %s",
	              sev, static_cast<unsigned>(source), static_cast<unsigned>(type),
	              id, message ? message : "");
	// Severity decides the LEVEL, so a real error stays findable. Drivers emit a
	// steady LOW stream of hints (buffer usage, renderbuffer allocation, unbound
	// texture units) that would otherwise bury the one message you turned this on
	// for — those land at Info; only MEDIUM and above reach the warning stream.
	const Logger::LogLevel level =
		(type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) ? Logger::LogLevel::Error
		: severity == GL_DEBUG_SEVERITY_MEDIUM                              ? Logger::LogLevel::Warning
		                                                                    : Logger::LogLevel::Info;
	Logger::LogTo(HE::Log::Cat::RHI, level, buf);
}

OpenGLRenderer::OpenGLRenderer()  = default;
OpenGLRenderer::~OpenGLRenderer() = default;

void OpenGLRenderer::Initialize(HE::Window* window)
{
	HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();
	m_glContext        = window->GetGLContext();
	if (!m_glContext)
		throw std::runtime_error("OpenGLRenderer: no GL context on window");

	if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)))
		throw std::runtime_error("OpenGLRenderer: gladLoadGLLoader failed");

	// Ray-traced GI needs compute shaders + SSBOs + image load/store — all core
	// in GL 4.3. Windows/Linux drivers give 4.3+; macOS GL is capped at 4.1, so
	// this stays false there and the Metal backend covers GI on Apple instead.
	m_giSupported = (GLAD_GL_VERSION_4_3 != 0);
	HE_LOG_INFO(RHI, "%s",
	            m_giSupported ? "OpenGLRenderer: GL 4.3+ — GI (compute) supported"
	                          : "OpenGLRenderer: GL < 4.3 — GI unavailable (CSM/AO fallback)");

	// ── HE_GL_DEBUG=1: driver-reported GL errors, named at the offending call ──
	// GL errors are otherwise silent until the one-shot glGetError at the end of
	// DrawScene, which reports a single code for the whole frame and names no
	// call — enough to know something broke, useless for finding what. KHR_debug
	// (core in 4.3) hands over the driver's own message; SYNCHRONOUS keeps the
	// callback on the calling thread, so a breakpoint in HeGLDebugMessage lands
	// in the culprit's stack. Off by default: the synchronous mode serialises the
	// driver. Primary context only — that is where the scene is drawn.
	if (const char* gd = std::getenv("HE_GL_DEBUG");
	    gd && *gd && GLAD_GL_VERSION_4_3 && glDebugMessageCallback)
	{
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(&HeGLDebugMessage, nullptr);
		// Everything except the chatty NOTIFICATION stream (buffer-hint chatter).
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
		                      0, nullptr, GL_FALSE);
		HE_LOG_INFO(RHI, "%s",
			"OpenGLRenderer: HE_GL_DEBUG — KHR_debug output installed (synchronous)");
	}

	m_shaderManager = OpenGLShaderManager();

	// Deferred-path debug/headless knobs (mirrors the Metal backend):
	// HE_RENDER_PATH=1/deferred forces the path without touching config,
	// HE_DUMP_GBUFFER=1..4 makes the resolve output a raw G-buffer view — it
	// seeds the view mode (the editor pushes its own every frame, the packaged
	// game never does).
	if (const char* rp = std::getenv("HE_RENDER_PATH"); rp && *rp)
		m_renderPath = (std::string(rp) == "1" || std::string(rp) == "deferred")
			? HE::RenderPath::Deferred : HE::RenderPath::Forward;
	if (const char* dv = std::getenv("HE_DUMP_GBUFFER"); dv && *dv)
		if (const int n = std::clamp(std::atoi(dv), 0, 4); n > 0)
			m_viewMode = static_cast<HE::ViewMode>(
				static_cast<int>(HE::ViewMode::GBufferBaseColor) + n - 1);

	glEnable(GL_DEPTH_TEST);
	CreateUnlitPipeline();
	CreateSkinnedPipeline();
	CreateInstancedPipeline();

	// Scratch VBO for per-instance transform matrices (mat4, GL_STREAM_DRAW).
	// Allocated with one identity matrix so attrib locs 4–7 always point to
	// valid storage even on the first single-instance draw via m_unlitProgram.
	glGenBuffers(1, &m_instanceVBO);
	glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
	const glm::mat4 identity(1.0f);
	glBufferData(GL_ARRAY_BUFFER, sizeof(glm::mat4), &identity, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	CreateShadowResources();
	CreateSkyPipeline();
	CreateTonemapPipeline();
	CreateTaaPipeline();
	CreateBloomPipeline();
	CreateDepthOfFieldPipeline();
	CreateMotionBlurPipeline();
	CreateSSAOPipeline();
	CreateDebugLinePipeline();
	CreateParticlePipeline();

	// Profiler GPU timing via GL timer queries. Reliable on desktop GL (Windows /
	// Linux); left off on Apple GL, where GL_TIMESTAMP / GL_TIME_ELAPSED queries are
	// unreliable — there GetFrameGpuStats keeps reporting gpuFrameMs = -1.
#ifdef __APPLE__
	m_gpuTimerSupported = false;
#else
	m_gpuTimerSupported = true;
#endif

	HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: initialized successfully");
}

static constexpr int kSkyEnvFace = 128; // image-based-ambient cubemap face size

// Cascaded shadow maps: number of depth-array layers / cascades. MUST match the
// shader's CSM_CASCADES (kUnlitFS) and stay ≤ ShadowData::kMaxCascades. The
// extractor fits the project's cascade count (setShadowSettings, 1..3); this
// caps the GL side to the same number it renders + samples.
static constexpr int kGLCsmCascades = 3;

// Wireframe view (IRenderer::SetViewMode): rasterise ONE mesh loop as lines.
// Scoped per loop, never "set once and restore before X" — the fullscreen
// draws between the loops (G-buffer resolve, sky, decal boxes, composites)
// must stay filled, and each of them would otherwise need its own reset.
// GL_FRONT_AND_BACK is the only mode a core profile (4.1 on macOS) accepts.
struct GLWireScope
{
	const bool on;
	explicit GLWireScope(bool wire) : on(wire)
	{
		if (on) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	~GLWireScope()
	{
		if (on) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
};

void OpenGLRenderer::CreateUnlitPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kUnlitVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());

	m_unlitProgram = glCreateProgram();
	glAttachShader(m_unlitProgram, vs);
	glAttachShader(m_unlitProgram, fs);
	glLinkProgram(m_unlitProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_unlitProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_unlitProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: program link failed: ") + log);
	}

	m_uMVP         = glGetUniformLocation(m_unlitProgram, "uMVP");
	m_uModel       = glGetUniformLocation(m_unlitProgram, "uModel");
	m_uColor       = glGetUniformLocation(m_unlitProgram, "uColor");
	m_uHasTexture  = glGetUniformLocation(m_unlitProgram, "uHasTexture");
	m_uTexture     = glGetUniformLocation(m_unlitProgram, "uTexture");
	m_uMetallic    = glGetUniformLocation(m_unlitProgram, "uMetallic");
	m_uRoughness   = glGetUniformLocation(m_unlitProgram, "uRoughness");
	m_uOpacity     = glGetUniformLocation(m_unlitProgram, "uOpacity");
	m_uNoShadow    = glGetUniformLocation(m_unlitProgram, "uNoShadow");
	m_uLightCount  = glGetUniformLocation(m_unlitProgram, "uLightCount");
	m_uLightPos    = glGetUniformLocation(m_unlitProgram, "uLightPos");
	m_uLightDir    = glGetUniformLocation(m_unlitProgram, "uLightDir");
	m_uLightColor  = glGetUniformLocation(m_unlitProgram, "uLightColor");
	m_uLightParams = glGetUniformLocation(m_unlitProgram, "uLightParams");
	m_uCameraPos   = glGetUniformLocation(m_unlitProgram, "uCameraPos");
	m_uSunDir      = glGetUniformLocation(m_unlitProgram, "uSunDir");
	m_uSkyEnv      = glGetUniformLocation(m_unlitProgram, "uSkyEnv");
	m_uAmbient     = glGetUniformLocation(m_unlitProgram, "uAmbient");

	// Empty image-based-ambient cubemap (RGBA32F, 6 faces); filled per frame from
	// the analytic skyColor (SkyColorCPU) whenever the sun direction changes.
	glGenTextures(1, &m_skyEnvCube);
	glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
	for (int f = 0; f < 6; ++f)
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA32F,
		             kSkyEnvFace, kSkyEnvFace, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
	m_uFogDensity       = glGetUniformLocation(m_unlitProgram, "uFogDensity");
	m_uFogHeightFalloff = glGetUniformLocation(m_unlitProgram, "uFogHeightFalloff");
	m_uWetness          = glGetUniformLocation(m_unlitProgram, "uWetness");
	m_uSnow             = glGetUniformLocation(m_unlitProgram, "uSnow");
	m_uSpecAA           = glGetUniformLocation(m_unlitProgram, "uSpecAA");
	m_uCloudShadowMap   = glGetUniformLocation(m_unlitProgram, "uCloudShadowMap");
	m_uCloudShadowA     = glGetUniformLocation(m_unlitProgram, "uCloudShadowA");
	m_uCloudShadowB     = glGetUniformLocation(m_unlitProgram, "uCloudShadowB");
	m_uCascadeVP     = glGetUniformLocation(m_unlitProgram, "uCascadeVP[0]");
	m_uCascadeSplits = glGetUniformLocation(m_unlitProgram, "uCascadeSplits");
	m_uCameraFwd     = glGetUniformLocation(m_unlitProgram, "uCameraFwd");
	m_uShadowMap     = glGetUniformLocation(m_unlitProgram, "uShadowMap");
	m_uShadowEnabled = glGetUniformLocation(m_unlitProgram, "uShadowEnabled");
	m_uShadowDebug   = glGetUniformLocation(m_unlitProgram, "uShadowDebug");
	m_uUnlit         = glGetUniformLocation(m_unlitProgram, "uUnlit");
	m_uLocalShadowVP  = glGetUniformLocation(m_unlitProgram, "uLocalShadowVP[0]");
	m_uLocalShadowMap = glGetUniformLocation(m_unlitProgram, "uLocalShadowMap");
	m_uShadowBias     = glGetUniformLocation(m_unlitProgram, "uShadowBias");
	m_uAO            = glGetUniformLocation(m_unlitProgram, "uAO");
	m_uViewport      = glGetUniformLocation(m_unlitProgram, "uViewport");
	m_uSSAOEnabled   = glGetUniformLocation(m_unlitProgram, "uSSAOEnabled");
	m_giLocsUnlit    = FetchGISceneLocs(m_unlitProgram);
}

// ─── Node-graph material path ────────────────────────────────────────────────
// Compiled into EVERY build, HE_ENABLE_SHADERC=OFF included (docs/he-apps-plan.md
// A3b): a packaged build gets its shaders from MaterialAsset::precompiledShaders,
// and only the FALLBACK below — cross-compiling at load — needs the translator.
// Gating this region out is what used to leave an application-sized build without
// a UI material path at all instead of with a precompiled one; the shader library
// links either way now (he_shadercompiler is the stub there) and simply answers
// every compile with ok = false.

// Resolve a material's custom shader via the shared, backend-agnostic library.
bool OpenGLRenderer::resolveMaterialShader(const HE::UUID& materialId, uint64_t& key, std::string& frag,
                                           std::string& vertBody)
{
	if (!m_contentManager) return false;
	return m_matShaderLib.resolveShaders(*m_contentManager, materialId, key, frag, vertBody);
}

bool OpenGLRenderer::resolveMaterialShaderGB(const HE::UUID& materialId, uint64_t& key, std::string& frag,
                                             std::string& vertBody)
{
	if (!m_contentManager) return false;
	return m_matShaderLib.resolveGBufferShaders(*m_contentManager, materialId, key, frag, vertBody);
}

// Build (or fetch) a GL program for a material's custom fragment. The shared library
// cross-compiles the standard attribute vertex + the material fragment to GLSL 410; here we
// compile+link them and wire the two UBO blocks to fixed binding points (macOS GL 4.1 has no
// layout(binding), so bind by block name). Cached by hash; 0 cached on failure so a broken
// shader isn't rebuilt every frame.
namespace {
// ── On-disk GL program-binary cache ─────────────────────────────────────────
// Persists linked material programs across launches (glGetProgramBinary /
// glProgramBinary), keyed by hash(vertSrc, fragSrc, GL device signature) so a
// driver/GPU change invalidates it. A safe no-op where the driver exposes no
// program-binary formats (e.g. Apple's GL 4.1) — falls back to compile+link.
bool glProgramBinarySupported()
{
	static int n = [] { GLint v = 0; glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &v); return (int)v; }();
	return n > 0;
}
std::filesystem::path glProgramCacheDir()
{
	static std::filesystem::path dir = [] {
		std::filesystem::path d;
		if (char* pref = SDL_GetPrefPath("HorizonCreations", "HorizonEngine")) {
			d = std::filesystem::path(pref) / "glprogcache"; SDL_free(pref);
		} else {
			d = std::filesystem::temp_directory_path() / "HorizonEngine" / "glprogcache";
		}
		std::error_code ec; std::filesystem::create_directories(d, ec);
		return d;
	}();
	return dir;
}
uint64_t glDeviceSig()
{
	static uint64_t sig = [] {
		std::string s;
		auto add = [&](GLenum e){ const GLubyte* p = glGetString(e); if (p) s += reinterpret_cast<const char*>(p); s += '|'; };
		add(GL_VENDOR); add(GL_RENDERER); add(GL_VERSION);
		return (uint64_t)std::hash<std::string>{}(s);
	}();
	return sig;
}
std::filesystem::path glProgramCachePath(const std::string& vertSrc, const std::string& fragSrc)
{
	const uint64_t h = (uint64_t)std::hash<std::string>{}(vertSrc + "\x1e" + fragSrc)
	                 ^ (glDeviceSig() * 0x9E3779B97F4A7C15ULL);
	char name[40]; std::snprintf(name, sizeof(name), "%016llx.glprog", (unsigned long long)h);
	return glProgramCacheDir() / name;
}
// Create a linked program from a cached binary; 0 on miss/failure (stale → recompile).
GLuint glTryLoadCachedProgram(const std::filesystem::path& path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return 0;
	uint32_t fmt = 0, len = 0;
	f.read(reinterpret_cast<char*>(&fmt), 4);
	f.read(reinterpret_cast<char*>(&len), 4);
	if (!f || len == 0 || len > (64u << 20)) return 0;
	std::vector<uint8_t> bin(len);
	f.read(reinterpret_cast<char*>(bin.data()), len);
	if (!f) return 0;
	GLuint prog = glCreateProgram();
	glProgramBinary(prog, (GLenum)fmt, bin.data(), (GLsizei)len);
	GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) { glDeleteProgram(prog); return 0; }
	return prog;
}
void glSaveCachedProgram(const std::filesystem::path& path, GLuint prog)
{
	GLint len = 0; glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
	if (len <= 0) return;
	std::vector<uint8_t> bin(len);
	GLenum fmt = 0; GLsizei got = 0;
	glGetProgramBinary(prog, len, &got, &fmt, bin.data());
	if (got <= 0) return;
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) return;
	uint32_t fmt32 = (uint32_t)fmt, len32 = (uint32_t)got;
	f.write(reinterpret_cast<const char*>(&fmt32), 4);
	f.write(reinterpret_cast<const char*>(&len32), 4);
	f.write(reinterpret_cast<const char*>(bin.data()), got);
}
} // namespace

// The three UBOs every material draw feeds through: per-object "U" (mvp/model/
// colour/flags/pbr), the shared "HeLighting" block and "HeParams" (the graph's
// exposed parameters). Created on demand — a session that never touches a graph
// material never needs them — but NOT from any one build path: a program handed
// back by the memo table or restored from the on-disk binary cache is as much a
// user of these buffers as a freshly linked one. Left at 0 they poison every
// material draw: glBufferSubData against the default buffer is a
// GL_INVALID_OPERATION, glBindBufferBase pins block "U" to buffer 0, and the
// draw runs with no MVP — the object is issued and lands nowhere on screen.
void OpenGLRenderer::EnsureMaterialUBOs()
{
	if (m_matObjUBO) return;
	glGenBuffers(1, &m_matObjUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
	glBufferData(GL_UNIFORM_BUFFER, 176, nullptr, GL_DYNAMIC_DRAW); // mat4 mvp+model + 3×vec4
	glGenBuffers(1, &m_matLightUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
	glBufferData(GL_UNIFORM_BUFFER,
		static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::Lighting)), nullptr, GL_DYNAMIC_DRAW);
	glGenBuffers(1, &m_matParamUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
	glBufferData(GL_UNIFORM_BUFFER, 256, nullptr, GL_DYNAMIC_DRAW); // vec4 v[16]
	// HeUI (D5 Schicht 1): rect, corner radii, interaction state — per QUAD, so
	// unlike HeParams it is re-uploaded inside the UI loop rather than per draw
	// call's material.
	glGenBuffers(1, &m_matUIUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_matUIUBO);
	glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW); // 4 x vec4
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

unsigned int OpenGLRenderer::GetOrBuildMaterialProgram(uint64_t key, const std::string& fragGlsl,
                                                       const std::string& vertBody,
                                                       const MaterialShaderVariant* precompiled)
{
	EnsureMaterialUBOs(); // before every return below — memo hit and cache hit included
	if (auto it = m_materialPrograms.find(key); it != m_materialPrograms.end()) return it->second;

	using Backend = HE::MaterialShaderLibrary::Backend;
	std::string vertSrc, fragSrc, log; bool ok = false;
	if (precompiled)
	{
		vertSrc = precompiled->vertex; fragSrc = precompiled->fragment;
		ok = !vertSrc.empty() && !fragSrc.empty(); // baked GLSL 410 — no runtime cross-compile
	}
	else
	{
		// WPO materials use the graph-generated vertex; UBO blocks bind by NAME below,
		// so the custom vertex's HeLighting/HeParams resolve without extra plumbing.
		const auto& v = vertBody.empty()
			? m_matShaderLib.standardVertex(Backend::GLSL410)
			: m_matShaderLib.customVertex(std::hash<std::string>{}(vertBody), vertBody,
			                              Backend::GLSL410);
		const auto& f = m_matShaderLib.fragment(key, fragGlsl, Backend::GLSL410);
		vertSrc = v.source; fragSrc = f.source; log = v.log + f.log; ok = v.ok && f.ok;
	}
	// Uniform-block bindings + sampler-unit assignments. Program state (not always
	// captured in a program binary), so re-applied whether the program was linked
	// fresh or restored from the on-disk cache.
	auto setupProgram = [](GLuint prog) {
		const GLuint uIdx = glGetUniformBlockIndex(prog, "U");
		if (uIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, uIdx, 1);
		const GLuint lIdx = glGetUniformBlockIndex(prog, "HeLighting");
		if (lIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, lIdx, 0);
		const GLuint pIdx = glGetUniformBlockIndex(prog, "HeParams");
		if (pIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, pIdx, 2);
		// Sampler uniforms → texture units: heTex0 (legacy/mesh) = 0, node-graph
		// project textures heTexP0..3 = units 1..4. GL 4.1 has no layout(binding).
		glUseProgram(prog);
		if (GLint l = glGetUniformLocation(prog, "heTex0"); l >= 0) glUniform1i(l, 0);
		for (int k = 0; k < 4; ++k)
		{
			const std::string nm = "heTexP" + std::to_string(k);
			if (GLint l = glGetUniformLocation(prog, nm.c_str()); l >= 0) glUniform1i(l, k + 1);
		}
		// GI screen-space shadow masks for heLitP() — units 9/10 (0-8 are taken
		// by the material/scene inputs).
		if (GLint l = glGetUniformLocation(prog, "heGIShadow"); l >= 0) glUniform1i(l, 9);
		if (GLint l = glGetUniformLocation(prog, "heGILocal");  l >= 0) glUniform1i(l, 10);
		// heCsm (CSM fallback, sampler2DArray) — unit 11. GL keeps csmSplits.w = 0
		// (single shadow map, never sampled), but the unit MUST be assigned: left
		// at 0 it would alias heTex0's unit with a different sampler type, which
		// is a draw-time validation error on strict drivers.
		if (GLint l = glGetUniformLocation(prog, "heCsm");      l >= 0) glUniform1i(l, 11);
		// heLocalShadow (local point/spot shadow atlas, sampler2DArray) — unit 12,
		// where DrawScene binds the atlas alongside the built-in shaders' unit 11.
		if (GLint l = glGetUniformLocation(prog, "heLocalShadow"); l >= 0) glUniform1i(l, 12);
		// heLandscapeWeights (landscape layer weightmap) — unit 13. Bound PER DRAW
		// from the terrain chunk's parent landscape, so two landscapes can share a
		// material and still carry their own paint.
		if (GLint l = glGetUniformLocation(prog, "heLandscapeWeights"); l >= 0) glUniform1i(l, 13);
		// heSkyEnv (samplerCube, image-based ambient + fog colour) = unit 14,
		// heAO (screen-space SSAO/HBAO/GTAO result) = unit 15. Per-FRAME state,
		// bound alongside the other shared material inputs in DrawScene.
		if (GLint l = glGetUniformLocation(prog, "heSkyEnv"); l >= 0) glUniform1i(l, 14);
		if (GLint l = glGetUniformLocation(prog, "heAO");     l >= 0) glUniform1i(l, 15);
		// DDGI probe atlases = units 16/17 (irradiance / visibility).
		if (GLint l = glGetUniformLocation(prog, "heGIIrradiance"); l >= 0) glUniform1i(l, 16);
		if (GLint l = glGetUniformLocation(prog, "heGIVisibility"); l >= 0) glUniform1i(l, 17);
		// Forward reflection cascade: heGIReflFwd = unit 18 and heSSRFwd = unit 20,
		// the SAME units the built-in shaders read as uGIRefl/uSSRFwd, so the
		// scene pass binds each trace result once for both. (An old PRECOMPILED
		// material baked before the cascade existed simply has no such uniform;
		// l < 0 then skips it and the gate never reaches a sampler.)
		if (GLint l = glGetUniformLocation(prog, "heGIReflFwd"); l >= 0) glUniform1i(l, 18);
		if (GLint l = glGetUniformLocation(prog, "heSSRFwd");    l >= 0) glUniform1i(l, 20);
		// Cloud-shadow transmittance map = unit 19 (per-frame bind in DrawScene,
		// shared with the built-in shaders' uCloudShadowMap).
		if (GLint l = glGetUniformLocation(prog, "heCloudShadow"); l >= 0) glUniform1i(l, 19);
		glUseProgram(0);
	};

	unsigned int program = 0;
	if (ok)
	{
		// Fast path: a linked binary cached from a previous launch — skips the
		// GLSL compile + link entirely. Only when the driver supports binaries.
		const bool cacheable = glProgramBinarySupported();
		const std::filesystem::path cachePath =
			cacheable ? glProgramCachePath(vertSrc, fragSrc) : std::filesystem::path{};
		if (cacheable)
			if (GLuint cached = glTryLoadCachedProgram(cachePath))
			{
				setupProgram(cached);
				program = cached;
				m_materialPrograms[key] = program;
				HE_LOG_INFO(RHI, "%s",
					"OpenGLRenderer: loaded a material program from the on-disk binary cache");
				return program;
			}

		GLuint vs = CompileStage(GL_VERTEX_SHADER,   vertSrc.c_str());
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragSrc.c_str());
		GLuint prog = glCreateProgram();
		if (cacheable) glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs); glDeleteShader(fs);
		GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (ok)
		{
			setupProgram(prog);
			program = prog;
			if (cacheable) glSaveCachedProgram(cachePath, prog); // persist for next launch
			HE_LOG_INFO(RHI, "%s", precompiled
				? "OpenGLRenderer: built a material program from a PRECOMPILED variant (no runtime cross-compile)"
				: "OpenGLRenderer: built a material program from canonical GLSL via he::shaderc");
		}
		else
		{
			char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: material program link failed: ") + log).c_str());
			glDeleteProgram(prog);
		}
	}
	else
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: material shader cross-compile failed\n") + log).c_str());

	m_materialPrograms[key] = program; // cache success AND failure (0)
	return program;
}

// Build (or fetch) the GL program that draws GPU-instanced ParticleGraph particles
// (RenderWorld::particleBatches — the real scene path, not RenderParticlePreview).
// `precompiled` (an export-baked ParticleShaderVariant for OpenGL) skips template
// splicing + relies on the driver compiler only; null → generate the templates from
// `config` right now via HE::generateParticleShaderSource (see that function's
// comment on why it takes the resolved config, not the graph).
unsigned int OpenGLRenderer::GetOrBuildParticleProgram(uint64_t key, const HE::ParticleEmitterConfig& config,
                                                       const ParticleShaderVariant* precompiled)
{
	if (auto it = m_particlePrograms.find(key); it != m_particlePrograms.end()) return it->second;

	std::string vertSrc, fragSrc;
	if (precompiled)
	{
		vertSrc = precompiled->vertex;
		fragSrc = precompiled->fragment;
	}
	else
	{
		const HE::ParticleShaderGen gen = HE::generateParticleShaderSource(config, /*metalSyntax*/false);
		vertSrc = HE::buildParticleVertexGLSL(gen.colorFn, gen.alphaFn);
		fragSrc = HE::buildParticleFragmentGLSL();
	}

	GLuint vs = CompileStage(GL_VERTEX_SHADER,   vertSrc.c_str());
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragSrc.c_str());
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	unsigned int program = 0;
	if (ok)
	{
		program = prog;
		HE_LOG_INFO(RHI, "%s", precompiled
			? "OpenGLRenderer: built a particle program from a PRECOMPILED variant"
			: "OpenGLRenderer: built a particle program from a freshly baked template");
	}
	else
	{
		char log[1024]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: particle program link failed: ") + log).c_str());
		glDeleteProgram(prog);
	}
	m_particlePrograms[key] = program; // cache success AND failure (0)
	return program;
}

// Build (or fetch) the GL program that draws a UI quad with a node-graph material:
// the material's shared fragment paired with the screen-space uiVertex (its U block
// repurposed — model[0]=rect px, model[1]=uvRect, model[2].xy=viewport px, color=
// tint; see MaterialShaderLibrary::uiVertex). Same fragment hash as the mesh path,
// but a different vertex → own cache. Cached by hash; 0 cached on failure.
unsigned int OpenGLRenderer::GetOrBuildUIMaterialProgram(const HE::UUID& materialId,
                                                        bool* usesBackdrop)
{
	uint64_t key = 0; std::string fragGlsl, vertBody;
	if (usesBackdrop) *usesBackdrop = false;
	if (!resolveMaterialShader(materialId, key, fragGlsl, vertBody))
		return 0; // no custom shader → solid-color quad
	// Read off the SOURCE, not off a field on the asset: a packaged build skips
	// the graph regeneration entirely (precompiled blobs) and a material instance
	// takes a second road through syncMaterialInstance — the generated shader is
	// the one thing all three configurations share.
	if (usesBackdrop) *usesBackdrop = fragGlsl.find("heBackdrop") != std::string::npos;
	// A UI quad can be the first material user of the session; same "before every
	// return, memo hit included" rule as the mesh path.
	EnsureMaterialUBOs();
	if (auto it = m_uiMaterialPrograms.find(key); it != m_uiMaterialPrograms.end()) return it->second;

	// Baked at export time (A3b): the same variant the MESH path already reads,
	// paired with its UI vertex instead of the standard one — the fragment is
	// literally the same string, which is why one variant can serve both. Present
	// → no runtime cross-compile, so a packaged application needs no glslang to
	// draw a widget with a material on it. Read out of the asset and copied at
	// once: ContentManager's getters point into a dense vector, and the next load
	// invalidates them.
	using Backend = HE::MaterialShaderLibrary::Backend;
	std::string vertSrc, fragSrc, log;
	bool ok = false;
	if (const MaterialAsset* ma = m_contentManager ? m_contentManager->getMaterial(materialId) : nullptr)
		for (const auto& var : ma->precompiledShaders)
			if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL) && !var.uiVertex.empty())
			{
				vertSrc = var.uiVertex; fragSrc = var.fragment;
				ok = !fragSrc.empty();
				break;
			}
	if (!ok)
	{
		// No baked UI half — loose editor assets, a pak from before the field
		// existed, a backend the export skipped. Cross-compile, as before; in a
		// build without the translator this simply fails and the quad falls back
		// to its solid colour.
		const auto& v = m_matShaderLib.uiVertex(Backend::GLSL410);
		const auto& f = m_matShaderLib.fragment(key, fragGlsl, Backend::GLSL410);
		vertSrc = v.source; fragSrc = f.source; log = v.log + f.log;
		ok = v.ok && f.ok;
	}

	unsigned int program = 0;
	if (ok)
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   vertSrc.c_str());
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragSrc.c_str());
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs); glDeleteShader(fs);
		GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (ok)
		{
			// Same block bindings + sampler units as the mesh material path (GL 4.1
			// has no layout(binding), so wire them by name post-link).
			const GLuint uIdx = glGetUniformBlockIndex(prog, "U");
			if (uIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, uIdx, 1);
			const GLuint lIdx = glGetUniformBlockIndex(prog, "HeLighting");
			if (lIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, lIdx, 0);
			const GLuint pIdx = glGetUniformBlockIndex(prog, "HeParams");
			if (pIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, pIdx, 2);
			// HeUI (D5 Schicht 1). Wiring it by name is not optional on GL 4.1:
			// a block whose binding is never set defaults to 0, which here is
			// HeLighting — the element would read the sun instead of itself.
			const GLuint wIdx = glGetUniformBlockIndex(prog, "HeUI");
			if (wIdx != GL_INVALID_INDEX) glUniformBlockBinding(prog, wIdx, 8);
			glUseProgram(prog);
			if (GLint l = glGetUniformLocation(prog, "heTex0"); l >= 0) glUniform1i(l, 0);
			for (int k = 0; k < 4; ++k)
			{
				const std::string nm = "heTexP" + std::to_string(k);
				if (GLint l = glGetUniformLocation(prog, nm.c_str()); l >= 0) glUniform1i(l, k + 1);
			}
			// heBackdrop — unit 5, the first one free between the graph textures
			// (1..4) and the GI/shadow maps (9..12).
			if (GLint l = glGetUniformLocation(prog, "heBackdrop"); l >= 0) glUniform1i(l, 5);
			// GI masks for heLitP() — units 9/10 (UI materials never sample them:
			// giParams.z stays 0 on the UI path, but the units must be assigned).
			if (GLint l = glGetUniformLocation(prog, "heGIShadow"); l >= 0) glUniform1i(l, 9);
			if (GLint l = glGetUniformLocation(prog, "heGILocal");  l >= 0) glUniform1i(l, 10);
			// heCsm — unit 11, same aliasing rationale as the mesh-material path.
			if (GLint l = glGetUniformLocation(prog, "heCsm");      l >= 0) glUniform1i(l, 11);
			if (GLint l = glGetUniformLocation(prog, "heLocalShadow"); l >= 0) glUniform1i(l, 12);
			// heCloudShadow — unit 19, same rationale (never sampled on the UI
			// path: cloudShadowB.x stays 0, but the unit must be assigned).
			if (GLint l = glGetUniformLocation(prog, "heCloudShadow"); l >= 0) glUniform1i(l, 19);
			glUseProgram(0);
			program = prog;
		}
		else
		{
			char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: UI material program link failed: ") + log).c_str());
			glDeleteProgram(prog);
		}
	}
	else
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: UI material shader cross-compile failed\n") + log).c_str());

	m_uiMaterialPrograms[key] = program; // cache success AND failure (0)
	return program;
}

void OpenGLRenderer::CreateSkinnedPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSkinnedVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());

	m_skinnedProgram = glCreateProgram();
	glAttachShader(m_skinnedProgram, vs);
	glAttachShader(m_skinnedProgram, fs);
	glLinkProgram(m_skinnedProgram);
	glDeleteShader(vs); glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_skinnedProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[512];
		glGetProgramInfoLog(m_skinnedProgram, sizeof(log), nullptr, log);
		HE_LOG_ERROR(RHI, "%s",
		    (std::string("OpenGLRenderer: skinned link error: ") + log).c_str());
		return;
	}

	auto loc = [&](const char* n){ return glGetUniformLocation(m_skinnedProgram, n); };
	m_uSkinnedMVP          = loc("uMVP");
	m_uSkinnedModel        = loc("uModel");
	m_uSkinnedBones        = loc("uBoneMatrices");
	m_uSkinnedColor        = loc("uColor");
	m_uSkinnedHasTex       = loc("uHasTexture");
	m_uSkinnedTex          = loc("uTexture");
	m_uSkinnedMetallic     = loc("uMetallic");
	m_uSkinnedRoughness    = loc("uRoughness");
	m_uSkinnedOpacity      = loc("uOpacity");
	m_uSkinnedNoShadow     = loc("uNoShadow");
	m_uSkinnedLightCount   = loc("uLightCount");
	m_uSkinnedLightPos     = loc("uLightPos");
	m_uSkinnedLightDir     = loc("uLightDir");
	m_uSkinnedLightColor   = loc("uLightColor");
	m_uSkinnedLightParams  = loc("uLightParams");
	m_uSkinnedCameraPos    = loc("uCameraPos");
	m_uSkinnedAmbient      = loc("uAmbient");
	m_uSkinnedSunDir       = loc("uSunDir");
	m_uSkinnedSkyEnv       = loc("uSkyEnv");
	m_uSkinnedFogDensity         = loc("uFogDensity");
	m_uSkinnedFogHeightFalloff   = loc("uFogHeightFalloff");
	m_uSkinnedShadowEnabled      = loc("uShadowEnabled");
	m_uSkinnedCascadeVP          = loc("uCascadeVP[0]");
	m_uSkinnedCascadeSplits      = loc("uCascadeSplits");
	m_uSkinnedCameraFwd          = loc("uCameraFwd");
	m_uSkinnedShadowDebug        = loc("uShadowDebug");
	m_uSkinnedUnlit              = loc("uUnlit");
	m_uSkinnedShadowMap          = loc("uShadowMap");
	m_uSkinnedLocalShadowVP      = loc("uLocalShadowVP[0]");
	m_uSkinnedLocalShadowMap     = loc("uLocalShadowMap");
	m_uSkinnedShadowBias         = loc("uShadowBias");
	m_uSkinnedAO                 = loc("uAO");
	m_uSkinnedViewport           = loc("uViewport");
	m_uSkinnedSSAOEnabled        = loc("uSSAOEnabled");
	m_uSkinnedCloudShadowMap     = loc("uCloudShadowMap");
	m_uSkinnedCloudShadowA       = loc("uCloudShadowA");
	m_uSkinnedCloudShadowB       = loc("uCloudShadowB");
	m_giLocsSkinned              = FetchGISceneLocs(m_skinnedProgram);
}

void OpenGLRenderer::CreateInstancedPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kInstancedVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kUnlitFS).c_str());

	m_instancedProgram = glCreateProgram();
	glAttachShader(m_instancedProgram, vs);
	glAttachShader(m_instancedProgram, fs);
	glLinkProgram(m_instancedProgram);
	glDeleteShader(vs); glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_instancedProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char log[512];
		glGetProgramInfoLog(m_instancedProgram, sizeof(log), nullptr, log);
		HE_LOG_ERROR(RHI, "%s",
		    (std::string("OpenGLRenderer: instanced program link error: ") + log).c_str());
		return;
	}

	auto loc = [&](const char* n){ return glGetUniformLocation(m_instancedProgram, n); };
	m_uInstViewProj         = loc("uViewProj");
	m_uInstColor            = loc("uColor");
	m_uInstHasTexture       = loc("uHasTexture");
	m_uInstTexture          = loc("uTexture");
	m_uInstMetallic         = loc("uMetallic");
	m_uInstRoughness        = loc("uRoughness");
	m_uInstOpacity          = loc("uOpacity");
	m_uInstNoShadow         = loc("uNoShadow");
	m_uInstLightCount       = loc("uLightCount");
	m_uInstLightPos         = loc("uLightPos");
	m_uInstLightDir         = loc("uLightDir");
	m_uInstLightColor       = loc("uLightColor");
	m_uInstLightParams      = loc("uLightParams");
	m_uInstCameraPos        = loc("uCameraPos");
	m_uInstSunDir           = loc("uSunDir");
	m_uInstSkyEnv           = loc("uSkyEnv");
	m_uInstAmbient          = loc("uAmbient");
	m_uInstFogDensity       = loc("uFogDensity");
	m_uInstFogHeightFalloff = loc("uFogHeightFalloff");
	m_uInstWetness          = loc("uWetness");
	m_uInstSnow             = loc("uSnow");
	m_uInstSpecAA           = loc("uSpecAA");
	m_uInstCascadeVP        = loc("uCascadeVP[0]");
	m_uInstCascadeSplits    = loc("uCascadeSplits");
	m_uInstCameraFwd        = loc("uCameraFwd");
	m_uInstShadowDebug      = loc("uShadowDebug");
	m_uInstUnlit            = loc("uUnlit");
	m_uInstShadowMap        = loc("uShadowMap");
	m_uInstShadowEnabled    = loc("uShadowEnabled");
	m_uInstLocalShadowVP    = loc("uLocalShadowVP[0]");
	m_uInstLocalShadowMap   = loc("uLocalShadowMap");
	m_uInstShadowBias       = loc("uShadowBias");
	m_uInstAO               = loc("uAO");
	m_uInstViewport         = loc("uViewport");
	m_uInstSSAOEnabled      = loc("uSSAOEnabled");
	m_uInstCloudShadowMap   = loc("uCloudShadowMap");
	m_uInstCloudShadowA     = loc("uCloudShadowA");
	m_uInstCloudShadowB     = loc("uCloudShadowB");
	m_giLocsInstanced       = FetchGISceneLocs(m_instancedProgram);
}

void OpenGLRenderer::UpdateSkyEnvCube(const glm::vec3& sunDir)
{
	// The baked sky only changes with the sun direction — skip the CPU rebuild + upload
	// when it has barely moved. The IBL ambient is very low-frequency, so a small dead-
	// band (≈0.11°) is imperceptible but, under day-night auto-advance, cuts the rebuild
	// rate to roughly every other frame instead of every frame.
	if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 2.0e-3f)
		return;
	m_skyEnvSunDir = sunDir;
	m_skyEnvValid  = true;
	const std::vector<float> px = BuildSkyEnvCube(kSkyEnvFace, sunDir);
	const size_t faceFloats = static_cast<size_t>(kSkyEnvFace) * kSkyEnvFace * 4;
	glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
	for (int f = 0; f < 6; ++f)
		glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0,
		                kSkyEnvFace, kSkyEnvFace, GL_RGBA, GL_FLOAT,
		                px.data() + f * faceFloats);
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void OpenGLRenderer::CreateShadowResources()
{
	// Depth-only program for the shadow pass.
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kDepthVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kDepthFS);
	m_depthProgram = glCreateProgram();
	glAttachShader(m_depthProgram, vs);
	glAttachShader(m_depthProgram, fs);
	glLinkProgram(m_depthProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);
	m_uDepthMVP = glGetUniformLocation(m_depthProgram, "uDepthMVP");

	// Instanced depth-only program for same-mesh caster runs (see
	// RenderSorter::batchDepthCasters). Optional: a link failure only sends
	// every run back through the per-caster loop above.
	{
		GLuint ivs = CompileStage(GL_VERTEX_SHADER,   kDepthInstancedVS);
		GLuint ifs = CompileStage(GL_FRAGMENT_SHADER, kDepthFS);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, ivs);
		glAttachShader(prog, ifs);
		glLinkProgram(prog);
		glDeleteShader(ivs);
		glDeleteShader(ifs);
		GLint ok = 0;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (ok)
		{
			m_depthInstancedProgram = prog;
			m_uDepthInstVP = glGetUniformLocation(prog, "uDepthVP");
		}
		else
		{
			char log[1024] = {};
			glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "OpenGLRenderer: instanced depth program link failed: %s", log);
			glDeleteProgram(prog);
		}
	}

	// Cascaded shadow map: a Depth24 texture ARRAY (one layer per cascade), sampled
	// by the scene shader. Each cascade renders into its own layer (attached per
	// cascade in the shadow pass via glFramebufferTextureLayer). Border color 1.0
	// so samples outside a cascade read as "fully lit" (depth 1).
	glGenTextures(1, &m_shadowDepthTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
	             m_shadowSize, m_shadowSize, kGLCsmCascades,
	             0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);

	// Local (point/spot) shadow atlas: same depth-array pattern, 16 layers
	// (spot = 1 layer, point = 6 cube-face layers), lower per-view resolution.
	glGenTextures(1, &m_localShadowDepthTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_localShadowDepthTex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
	             m_localShadowSize, m_localShadowSize, ShadowData::kMaxLocalShadowLayers,
	             0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);

	// FBO is completed per cascade in the pass; attach layer 0 here so the initial
	// completeness check passes.
	glGenFramebuffers(1, &m_shadowFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadowDepthTex, 0, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderer::CreateSkyPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSkyVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, injectSkyFunc(kSkyFS).c_str());
	m_skyProgram = glCreateProgram();
	glAttachShader(m_skyProgram, vs);
	glAttachShader(m_skyProgram, fs);
	glLinkProgram(m_skyProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_skyProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_skyProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: sky link failed: ") + log);
	}
	m_uSkyInvVP  = glGetUniformLocation(m_skyProgram, "uInvViewProj");
	m_uSkySunDir = glGetUniformLocation(m_skyProgram, "uSunDir");
	m_uSkyMoonTex = glGetUniformLocation(m_skyProgram, "uMoonTex");
	m_uSkyHasMoon = glGetUniformLocation(m_skyProgram, "uHasMoonTex");
	m_uSkyMoonPhase = glGetUniformLocation(m_skyProgram, "uMoonPhase");
	m_uSkyTime    = glGetUniformLocation(m_skyProgram, "uTimeOfDay");
	m_uSkyCoverage = glGetUniformLocation(m_skyProgram, "uCloudCoverage");
	m_uSkyClock    = glGetUniformLocation(m_skyProgram, "uTime");
	m_uSkySunColor = glGetUniformLocation(m_skyProgram, "uSunColor");
	m_uSkyAurora   = glGetUniformLocation(m_skyProgram, "uAurora");
	m_uSkyMilkyWay    = glGetUniformLocation(m_skyProgram, "uMilkyWay");
	m_uSkyNebula      = glGetUniformLocation(m_skyProgram, "uNebula");
	m_uSkyNebulaColor = glGetUniformLocation(m_skyProgram, "uNebulaColor");
	m_uSkyNebulaColor2 = glGetUniformLocation(m_skyProgram, "uNebulaColor2");
	m_uSkyNebulaColor3 = glGetUniformLocation(m_skyProgram, "uNebulaColor3");
	m_uSkyNebulaSeed  = glGetUniformLocation(m_skyProgram, "uNebulaSeed");
	m_uSkyNebulaHiFi  = glGetUniformLocation(m_skyProgram, "uNebulaHiFi");
	m_uSkyNebulaCover = glGetUniformLocation(m_skyProgram, "uNebulaCover");
	m_uSkyAuroraColor = glGetUniformLocation(m_skyProgram, "uAuroraColor");
	m_uSkyAuroraColorTop = glGetUniformLocation(m_skyProgram, "uAuroraColorTop");
	m_uSkyAuroraHeight   = glGetUniformLocation(m_skyProgram, "uAuroraHeight");
	m_uSkyAuroraFragment = glGetUniformLocation(m_skyProgram, "uAuroraFragment");
	m_uSkyWind        = glGetUniformLocation(m_skyProgram, "uWind");
	m_uSkyNoise       = glGetUniformLocation(m_skyProgram, "uNoise");
	m_uSkyCloudShadowPass   = glGetUniformLocation(m_skyProgram, "uCloudShadowPass");
	m_uSkyCloudShadowRegion = glGetUniformLocation(m_skyProgram, "uCloudShadowRegion");
	m_uSkyCloudTex     = glGetUniformLocation(m_skyProgram, "uCloudTex");
	m_uSkyLowResClouds = glGetUniformLocation(m_skyProgram, "uLowResClouds");
	m_uSkyCloudPrepass = glGetUniformLocation(m_skyProgram, "uCloudPrepass");
	m_uSkyRainAmount   = glGetUniformLocation(m_skyProgram, "uRainAmount");
	m_uSkyGodRays      = glGetUniformLocation(m_skyProgram, "uGodRays");
	m_uSkyShootingStars = glGetUniformLocation(m_skyProgram, "uShootingStars");
	m_uSkyFlash       = glGetUniformLocation(m_skyProgram, "uFlash");
	m_uSkyCloudMode   = glGetUniformLocation(m_skyProgram, "uCloudMode");
	m_uSkyCloudQuality = glGetUniformLocation(m_skyProgram, "uCloudQuality");
	m_uSkyCloudStyle        = glGetUniformLocation(m_skyProgram, "uCloudStyle");
	m_uSkyCloudInterShadows = glGetUniformLocation(m_skyProgram, "uCloudInterShadows");
	m_uSkyCloudEvolution    = glGetUniformLocation(m_skyProgram, "uCloudEvolution");
	m_uSkyCameraPos   = glGetUniformLocation(m_skyProgram, "uCameraPos");
	m_uSkyCloudHeight = glGetUniformLocation(m_skyProgram, "uCloudHeight");
	m_uSkyCloudDensity    = glGetUniformLocation(m_skyProgram, "uCloudDensity");
	m_uSkyCloudFluffiness = glGetUniformLocation(m_skyProgram, "uCloudFluffiness");
	m_uSkyCloudTint       = glGetUniformLocation(m_skyProgram, "uCloudTint");
	m_uSkyContrails       = glGetUniformLocation(m_skyProgram, "uContrails");
	m_uSkyCirrus          = glGetUniformLocation(m_skyProgram, "uCirrus");
	m_uSkyCirrusSeed      = glGetUniformLocation(m_skyProgram, "uCirrusSeed");
	m_uSkyStarBright      = glGetUniformLocation(m_skyProgram, "uStarBright");
	m_uSkyStarColor       = glGetUniformLocation(m_skyProgram, "uStarColor");
	m_uSkyStarSize        = glGetUniformLocation(m_skyProgram, "uStarSize");
	m_uSkyStarSizeVar     = glGetUniformLocation(m_skyProgram, "uStarSizeVar");
	m_uSkyStarDensity     = glGetUniformLocation(m_skyProgram, "uStarDensity");
	m_uSkyStarGlow        = glGetUniformLocation(m_skyProgram, "uStarGlow");
	m_uSkyStarTwinkle     = glGetUniformLocation(m_skyProgram, "uStarTwinkle");

	// Procedural 3D noise volume the sky's starFbm3/worleyFbm sample (clouds +
	// nebula) — built once on the CPU. RG16 (R=value noise, G=Worley billows) +
	// LINEAR + REPEAT so it tiles seamlessly.
	// Release: full 256³ tile so sky fBm octaves don't visibly repeat.
	// Debug: 64³ (64× fewer voxels) so the CPU bake takes < 1s instead of 30min
	// without SIMD optimisation in MSVC Debug mode.
#ifdef NDEBUG
	constexpr int kNoiseN = 256;
#else
	constexpr int kNoiseN = 64;
#endif
	const std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kNoiseN);
	glGenTextures(1, &m_noiseTex);
	glBindTexture(GL_TEXTURE_3D, m_noiseTex);
	glTexImage3D(GL_TEXTURE_3D, 0, GL_RG16, kNoiseN, kNoiseN, kNoiseN, 0,
	             GL_RG, GL_UNSIGNED_SHORT, noise.data());
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glBindTexture(GL_TEXTURE_3D, 0);
}

void OpenGLRenderer::CreateTonemapPipeline()
{
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kTonemapFS);
	m_tonemapProgram = glCreateProgram();
	glAttachShader(m_tonemapProgram, vs);
	glAttachShader(m_tonemapProgram, fs);
	glLinkProgram(m_tonemapProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(m_tonemapProgram, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		GLchar log[512];
		glGetProgramInfoLog(m_tonemapProgram, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("OpenGLRenderer: tonemap link failed: ") + log);
	}
	m_uHDRTex        = glGetUniformLocation(m_tonemapProgram, "uHDR");
	m_uExposure      = glGetUniformLocation(m_tonemapProgram, "uExposure");
	m_uBloomTex      = glGetUniformLocation(m_tonemapProgram, "uBloom");
	m_uBloomStrength = glGetUniformLocation(m_tonemapProgram, "uBloomStrength");
	m_uLensFlare     = glGetUniformLocation(m_tonemapProgram, "uLensFlare");

	// FXAA program (shares the fullscreen-triangle VS).
	{
		GLuint fvs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint ffs = CompileStage(GL_FRAGMENT_SHADER, kFxaaFS);
		m_fxaaProgram = glCreateProgram();
		glAttachShader(m_fxaaProgram, fvs);
		glAttachShader(m_fxaaProgram, ffs);
		glLinkProgram(m_fxaaProgram);
		glDeleteShader(fvs);
		glDeleteShader(ffs);
		GLint fok = 0;
		glGetProgramiv(m_fxaaProgram, GL_LINK_STATUS, &fok);
		if (!fok)
		{
			GLchar log[512];
			glGetProgramInfoLog(m_fxaaProgram, sizeof(log), nullptr, log);
			throw std::runtime_error(std::string("OpenGLRenderer: FXAA link failed: ") + log);
		}
		m_uFxaaScene    = glGetUniformLocation(m_fxaaProgram, "uScene");
		m_uFxaaRcpFrame = glGetUniformLocation(m_fxaaProgram, "uRcpFrame");
	}

	// SMAA program (same VS + uniforms as FXAA; see kSmaaFS).
	{
		GLuint svs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint sfs = CompileStage(GL_FRAGMENT_SHADER, kSmaaFS);
		m_smaaProgram = glCreateProgram();
		glAttachShader(m_smaaProgram, svs);
		glAttachShader(m_smaaProgram, sfs);
		glLinkProgram(m_smaaProgram);
		glDeleteShader(svs);
		glDeleteShader(sfs);
		GLint sok = 0;
		glGetProgramiv(m_smaaProgram, GL_LINK_STATUS, &sok);
		if (!sok)
		{
			GLchar log[512];
			glGetProgramInfoLog(m_smaaProgram, sizeof(log), nullptr, log);
			throw std::runtime_error(std::string("OpenGLRenderer: SMAA link failed: ") + log);
		}
		m_uSmaaScene    = glGetUniformLocation(m_smaaProgram, "uScene");
		m_uSmaaRcpFrame = glGetUniformLocation(m_smaaProgram, "uRcpFrame");
	}

	// Passthrough program for AA = Off (same VS, same input texture).
	{
		GLuint bvs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint bfs = CompileStage(GL_FRAGMENT_SHADER, kBlitFS);
		m_blitProgram = glCreateProgram();
		glAttachShader(m_blitProgram, bvs);
		glAttachShader(m_blitProgram, bfs);
		glLinkProgram(m_blitProgram);
		glDeleteShader(bvs);
		glDeleteShader(bfs);
		GLint bok = 0;
		glGetProgramiv(m_blitProgram, GL_LINK_STATUS, &bok);
		if (!bok)
		{
			GLchar log[512];
			glGetProgramInfoLog(m_blitProgram, sizeof(log), nullptr, log);
			throw std::runtime_error(std::string("OpenGLRenderer: blit link failed: ") + log);
		}
		m_uBlitScene = glGetUniformLocation(m_blitProgram, "uScene");
	}

	// Core profile needs a bound VAO for glDrawArrays even with no attributes.
	glGenVertexArrays(1, &m_fsVAO);

	// ── 2D UI pipeline ──────────────────────────────────────────────────────
	{
		GLuint uvs = CompileStage(GL_VERTEX_SHADER,   kUIVS);
		GLuint ufs = CompileStage(GL_FRAGMENT_SHADER, kUIFS);
		m_uiProgram = glCreateProgram();
		glAttachShader(m_uiProgram, uvs);
		glAttachShader(m_uiProgram, ufs);
		glLinkProgram(m_uiProgram);
		glDeleteShader(uvs);
		glDeleteShader(ufs);
		GLint uok = 0;
		glGetProgramiv(m_uiProgram, GL_LINK_STATUS, &uok);
		if (!uok)
		{
			GLchar log[512];
			glGetProgramInfoLog(m_uiProgram, sizeof(log), nullptr, log);
			throw std::runtime_error(std::string("OpenGLRenderer: UI program link failed: ") + log);
		}
		m_uUIRect     = glGetUniformLocation(m_uiProgram, "uRect");
		m_uUIViewport = glGetUniformLocation(m_uiProgram, "uViewport");
		m_uUIColor    = glGetUniformLocation(m_uiProgram, "uColor");
		m_uUIUVRect   = glGetUniformLocation(m_uiProgram, "uUVRect");
		m_uUIRotation = glGetUniformLocation(m_uiProgram, "uRotation");
		m_uUIMode     = glGetUniformLocation(m_uiProgram, "uMode");
		m_uUICornerRadius = glGetUniformLocation(m_uiProgram, "uCornerRadius");
		m_uUIBorderWidth  = glGetUniformLocation(m_uiProgram, "uBorderWidth");
		m_uUIBorderColor  = glGetUniformLocation(m_uiProgram, "uBorderColor");
		m_uUIGradient      = glGetUniformLocation(m_uiProgram, "uGradient");
		m_uUIGradientColor = glGetUniformLocation(m_uiProgram, "uGradientColor");
		m_uUIGradientAngle = glGetUniformLocation(m_uiProgram, "uGradientAngle");
		m_uUIGradientShape = glGetUniformLocation(m_uiProgram, "uGradientShape");
		m_uUIBlur       = glGetUniformLocation(m_uiProgram, "uBlur");
		m_uUIInnerBlur  = glGetUniformLocation(m_uiProgram, "uInnerBlur");
		m_uUIInnerColor = glGetUniformLocation(m_uiProgram, "uInnerColor");
		// Font atlas always samples from texture unit 0 (bound in RenderUIPass).
		glUseProgram(m_uiProgram);
		if (GLint l = glGetUniformLocation(m_uiProgram, "uFontAtlas"); l >= 0) glUniform1i(l, 0);
		glUseProgram(0);
	}
}

// LDR intermediate the tonemap writes to and FXAA reads from. RGBA8, sized to the
// output; recreated on resize like the HDR/bloom targets.
void OpenGLRenderer::EnsureLdrTarget(int width, int height)
{
	if (m_ldrFBO && width == m_ldrW && height == m_ldrH) return;
	DestroyLdrTarget();
	m_ldrW = width; m_ldrH = height;
	glGenFramebuffers(1, &m_ldrFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_ldrFBO);
	glGenTextures(1, &m_ldrColor);
	glBindTexture(GL_TEXTURE_2D, m_ldrColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ldrColor, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderer::DestroyLdrTarget()
{
	if (m_ldrColor) { glDeleteTextures(1, &m_ldrColor); m_ldrColor = 0; }
	if (m_ldrFBO)   { glDeleteFramebuffers(1, &m_ldrFBO); m_ldrFBO = 0; }
	m_ldrW = m_ldrH = 0;
}

// ─── Temporal AA (A2/A3) ─────────────────────────────────────────────────────
// Three programs, all optional: a link failure logs and leaves the program 0,
// and GetCapabilities then reports no temporal AA, so ResolveAAMethod falls
// back to SMAA exactly as on a backend that never had it.
void OpenGLRenderer::CreateTaaPipeline()
{
	auto link = [&](const char* vsSrc, const char* fsSrc, const char* what) -> GLuint
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   vsSrc);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fsSrc);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			GLchar log[512];
			glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "OpenGLRenderer: TAA %s program failed to link: %s", what, log);
			glDeleteProgram(prog);
			return 0;
		}
		return prog;
	};
	m_taaVelocityProgram = link(kTaaVelocityVS, kTaaVelocityFS, "velocity");
	if (m_taaVelocityProgram)
	{
		m_uTaaVelMvpJitter = glGetUniformLocation(m_taaVelocityProgram, "uMvpJitter");
		m_uTaaVelMvpNow    = glGetUniformLocation(m_taaVelocityProgram, "uMvpNow");
		m_uTaaVelMvpPrev   = glGetUniformLocation(m_taaVelocityProgram, "uMvpPrev");
	}
	m_taaProgram = link(kTonemapVS, kTaaResolveFS, "resolve");
	if (m_taaProgram)
	{
		m_uTaaCurrent  = glGetUniformLocation(m_taaProgram, "uCurrent");
		m_uTaaHistory  = glGetUniformLocation(m_taaProgram, "uHistory");
		m_uTaaVelocity = glGetUniformLocation(m_taaProgram, "uVelocity");
		m_uTaaParams   = glGetUniformLocation(m_taaProgram, "uParams");
	}
	m_taaSharpenProgram = link(kTonemapVS, kTaaSharpenFS, "sharpen");
	if (m_taaSharpenProgram)
	{
		m_uTaaSharpScene  = glGetUniformLocation(m_taaSharpenProgram, "uScene");
		m_uTaaSharpParams = glGetUniformLocation(m_taaSharpenProgram, "uParams");
	}
}

// m_aaMethod is already resolved against GetCapabilities(), so TAA here means
// the programs exist; the target check is what the per-frame passes need.
bool OpenGLRenderer::TaaActive() const
{
	return m_aaMethod == HE::AAMethod::TAA && m_taaVelocityProgram && m_taaProgram
	    && m_taaSharpenProgram;
}

// The rasterisation matrix. The offset is applied in CLIP space (a translation
// of the projected x/y by a fraction of a pixel), which is the same thing as
// shifting the sample grid — and it leaves the caller's matrix untouched, so the
// unjittered one stays available for motion and reprojection.
glm::mat4 OpenGLRenderer::JitteredViewProj(const glm::mat4& viewProj, int width, int height) const
{
	if (!TaaActive() || width <= 0 || height <= 0) return viewProj;
	glm::mat4 j(1.0f);
	j[3][0] = m_taaJitter.x * 2.0f / static_cast<float>(width);
	j[3][1] = m_taaJitter.y * 2.0f / static_cast<float>(height);
	return j * viewProj;
}

void OpenGLRenderer::EnsureTaaTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_taaHistoryTex[0] && width == m_taaW && height == m_taaH) return;
	DestroyTaaTargets();

	auto makeTarget = [&](unsigned int& fbo, unsigned int& tex, GLenum internalFmt,
	                      GLenum fmt, GLenum type, GLint filter, const char* what)
	{
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, width, height, 0, fmt, type, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			HE_LOG_ERROR(RHI, "OpenGLRenderer: TAA %s FBO incomplete", what);
	};
	// The velocity FBO gets m_hdrDepth attached at draw time (RenderVelocity):
	// EnsureHDRTarget recreates that texture on resize, and re-attaching per
	// pass is cheaper than tracking it. Completeness is checked there.
	makeTarget(m_velocityFBO, m_velocityTex, GL_RG16F, GL_RG, GL_FLOAT, GL_NEAREST, "velocity");
	makeTarget(m_taaHistoryFBO[0], m_taaHistoryTex[0], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
	           GL_LINEAR, "history 0");
	makeTarget(m_taaHistoryFBO[1], m_taaHistoryTex[1], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
	           GL_LINEAR, "history 1");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_taaW = width;
	m_taaH = height;
	m_taaHistoryCur = 0;
	// A fresh history is garbage, not history: the first frame after a resize
	// must show the current frame only, or it blends against uninitialised
	// memory.
	m_taaHistoryValid = false;
}

void OpenGLRenderer::DestroyTaaTargets()
{
	if (m_velocityFBO) { glDeleteFramebuffers(1, &m_velocityFBO); m_velocityFBO = 0; }
	if (m_velocityTex) { glDeleteTextures(1, &m_velocityTex);     m_velocityTex = 0; }
	for (int i = 0; i < 2; ++i)
	{
		if (m_taaHistoryFBO[i]) { glDeleteFramebuffers(1, &m_taaHistoryFBO[i]); m_taaHistoryFBO[i] = 0; }
		if (m_taaHistoryTex[i]) { glDeleteTextures(1, &m_taaHistoryTex[i]);     m_taaHistoryTex[i] = 0; }
	}
	m_taaW = m_taaH = 0;
	m_taaHistoryValid = false;
	m_taaPrevTransforms.clear();
	m_taaCurTransforms.clear();
}

// Screen-space motion of the opaque geometry: for every visible object, where
// its vertices are now vs. where they were last frame (camera AND object
// motion). Depth-tested LEQUAL, no write, against the depth the opaque pass
// left in m_hdrDepth — on the forward path directly, on the deferred path via
// the G-buffer → HDR blit — so only the surfaces that are actually visible
// report. Runs between the "Opaque" and "Sky+Clouds" passes; the sky and the
// blended tail stay at zero velocity (the clear), which is what they should
// report. Restores the opaque pass's FBO/state for the sky that follows.
void OpenGLRenderer::RenderVelocity(int pw, int ph, const glm::mat4& viewProjClean,
                                    const glm::mat4& viewProjJit)
{
	if (!TaaActive() || !m_velocityFBO || !m_hdrDepth) return;

	glBindFramebuffer(GL_FRAMEBUFFER, m_velocityFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_hdrDepth, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: TAA velocity FBO incomplete");
		glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
		return;
	}
	glViewport(0, 0, pw, ph);
	// Colour only — the depth attachment IS the scene depth the sky and the
	// transparent tail still test against. Zero = "did not move".
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glUseProgram(m_taaVelocityProgram);

	m_taaCurTransforms.clear();
	for (const uint32_t idx : m_sortedIndices)
	{
		const RenderObject& obj = m_renderWorld.objects[idx];
		const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		if (!mesh || !mesh->vao || mesh->indexCount <= 0) continue;

		// An object seen for the first time reports no motion — its "previous"
		// position is where it is now. Anything else invents a streak out of
		// nowhere on the frame something spawns.
		const auto it = m_taaPrevTransforms.find(obj.entityId);
		const glm::mat4 prevModel = (it != m_taaPrevTransforms.end()) ? it->second : obj.transform;
		m_taaCurTransforms[obj.entityId] = obj.transform;

		glUniformMatrix4fv(m_uTaaVelMvpJitter, 1, GL_FALSE, glm::value_ptr(viewProjJit * obj.transform));
		glUniformMatrix4fv(m_uTaaVelMvpNow,    1, GL_FALSE, glm::value_ptr(viewProjClean * obj.transform));
		glUniformMatrix4fv(m_uTaaVelMvpPrev,   1, GL_FALSE, glm::value_ptr(m_taaPrevViewProj * prevModel));
		glBindVertexArray(mesh->vao);
		glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
	}

	// Advance the history HERE, at the end of the one pass that consumed it —
	// not at some "frame end" further out, so a frame is never compared
	// against itself.
	m_taaPrevViewProj = viewProjClean;
	m_taaPrevTransforms.swap(m_taaCurTransforms);

	// Back to what the opaque pass had set, for the sky + transparent tail.
	glBindVertexArray(0);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glUseProgram(m_unlitProgram);
	glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
	glViewport(0, 0, pw, ph);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

// Blend this frame's tonemapped image with the reprojected history. Runs AFTER
// the tonemap and BEFORE the AA-resolve slot, so the history lives in the same
// LDR space the user sees — which also keeps a single bright HDR sample from
// poisoning the accumulation for the next dozen frames. Assumes m_fsVAO is
// bound and the depth test is off (the post-process pass's state).
unsigned int OpenGLRenderer::RenderTaa(int pw, int ph)
{
	if (!TaaActive() || !m_taaHistoryTex[0] || !m_velocityTex || !m_ldrColor) return 0;
	if (pw != m_taaW || ph != m_taaH) return 0; // targets follow the scene size (DrawScene)

	const int cur  = m_taaHistoryCur;
	const int prev = 1 - cur;
	glBindFramebuffer(GL_FRAMEBUFFER, m_taaHistoryFBO[cur]);
	glViewport(0, 0, pw, ph);
	glUseProgram(m_taaProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ldrColor);
	glUniform1i(m_uTaaCurrent, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_taaHistoryTex[prev]);
	glUniform1i(m_uTaaHistory, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_velocityTex);
	glUniform1i(m_uTaaVelocity, 2);
	// 0.9 keeps ~10 frames of samples: enough to converge on an edge, short
	// enough that a mis-reprojected pixel does not linger.
	glUniform4f(m_uTaaParams, 1.0f / static_cast<float>(pw), 1.0f / static_cast<float>(ph),
	            m_taaHistoryValid ? 0.9f : 0.0f, 0.0f);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

	// This frame's result IS next frame's history: flip the ping-pong.
	m_taaHistoryCur   = prev;
	m_taaHistoryValid = true;
	return m_taaHistoryTex[cur];
}

void OpenGLRenderer::CreateBloomPipeline()
{
	// Bright pass (reuses the fullscreen-triangle VS).
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBrightFS);
		m_bloomBrightProgram = glCreateProgram();
		glAttachShader(m_bloomBrightProgram, vs);
		glAttachShader(m_bloomBrightProgram, fs);
		glLinkProgram(m_bloomBrightProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		m_uBrightHDR       = glGetUniformLocation(m_bloomBrightProgram, "uHDR");
		m_uBrightThreshold = glGetUniformLocation(m_bloomBrightProgram, "uThreshold");
		m_uBrightKnee      = glGetUniformLocation(m_bloomBrightProgram, "uKnee");
	}
	// Separable blur.
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kBloomBlurFS);
		m_blurProgram = glCreateProgram();
		glAttachShader(m_blurProgram, vs);
		glAttachShader(m_blurProgram, fs);
		glLinkProgram(m_blurProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		m_uBlurImage      = glGetUniformLocation(m_blurProgram, "uImage");
		m_uBlurTexel      = glGetUniformLocation(m_blurProgram, "uTexel");
		m_uBlurHorizontal = glGetUniformLocation(m_blurProgram, "uHorizontal");
	}
}

void OpenGLRenderer::SetBloomSettings(const BloomSettings& s)
{
	m_bloomEnabled   = s.enabled;
	m_bloomThreshold = s.threshold;
	m_bloomStrength  = s.intensity;
}

void OpenGLRenderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
	// Resolve against our own capabilities once, here, so the render path only
	// ever sees a method this backend can actually run.
	m_aaMethod           = IRenderer::ResolveAAMethod(s.method, GetCapabilities());
	m_aaSharpness        = s.sharpness;
	m_specularAA         = s.specularAA;
	m_specularAAStrength = s.specularAAStrength;
}

void OpenGLRenderer::EnsureBloomTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_bloomFBO[0] && width == m_bloomW && height == m_bloomH)
		return;
	DestroyBloomTargets();

	glGenFramebuffers(2, m_bloomFBO);
	glGenTextures(2, m_bloomColor);
	for (int i = 0; i < 2; ++i)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[i]);
		glBindTexture(GL_TEXTURE_2D, m_bloomColor[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bloomColor[i], 0);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: bloom FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_bloomW = width;
	m_bloomH = height;
}

void OpenGLRenderer::DestroyBloomTargets()
{
	if (m_bloomFBO[0])   glDeleteFramebuffers(2, m_bloomFBO);
	if (m_bloomColor[0]) glDeleteTextures(2, m_bloomColor);
	m_bloomFBO[0] = m_bloomFBO[1] = 0;
	m_bloomColor[0] = m_bloomColor[1] = 0;
	m_bloomW = m_bloomH = 0;
}

void OpenGLRenderer::EnsureCloudTarget(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_cloudFBO && width == m_cloudW && height == m_cloudH) return;
	DestroyCloudTarget();
	glGenFramebuffers(1, &m_cloudFBO);
	glGenTextures(1, &m_cloudTex);
	glBindFramebuffer(GL_FRAMEBUFFER, m_cloudFBO);
	glBindTexture(GL_TEXTURE_2D, m_cloudTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // bilinear upsample
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_cloudTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: cloud FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_cloudW = width;
	m_cloudH = height;
}

void OpenGLRenderer::DestroyCloudTarget()
{
	if (m_cloudFBO) glDeleteFramebuffers(1, &m_cloudFBO);
	if (m_cloudTex) glDeleteTextures(1, &m_cloudTex);
	m_cloudFBO = 0; m_cloudTex = 0;
	m_cloudW = m_cloudH = 0;
}

// ── Cloud shadows ────────────────────────────────────────────────────────────
static constexpr int kCloudShadowMapSize = 512;

void OpenGLRenderer::EnsureCloudShadowTarget()
{
	if (m_cloudShadowFBO) return;
	glGenFramebuffers(1, &m_cloudShadowFBO);
	glGenTextures(1, &m_cloudShadowTex);
	glBindFramebuffer(GL_FRAMEBUFFER, m_cloudShadowFBO);
	glBindTexture(GL_TEXTURE_2D, m_cloudShadowTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kCloudShadowMapSize, kCloudShadowMapSize,
	             0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_cloudShadowTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: cloud-shadow FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::DestroyCloudShadowTarget()
{
	if (m_cloudShadowFBO) glDeleteFramebuffers(1, &m_cloudShadowFBO);
	if (m_cloudShadowTex) glDeleteTextures(1, &m_cloudShadowTex);
	m_cloudShadowFBO = 0; m_cloudShadowTex = 0;
}

// Render the cloud slab's sun transmittance over a world-space XZ region around
// the camera (m_skyProgram with uCloudShadowPass = 1 — the sky's own density
// field, noise texture and cloud uniforms). Called from DrawScene BEFORE the
// opaque pass; the lit shaders + heLitP sample the result on unit 19. Computes
// and stores the region + strength every frame (m_cloudShadowParamsA/B);
// strength 0 = feature off this frame (the shaders never sample the map then).
// Caller restores the framebuffer/viewport (mirrors the shadow-map pass).
void OpenGLRenderer::RenderCloudShadowMap()
{
	m_cloudShadowParamsB = glm::vec4(0.0f);
	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	if (!(env.skyEnabled && env.cloudShadows && env.cloudShadowStrength > 0.0f
	      && env.cloudCoverage > 0.001f && m_skyProgram))
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
	if (!m_cloudShadowFBO) return;

	const float     cloudH = std::max(env.cloudHeight, 1.0f);
	const float     thick  = cloudH * 1.5f;
	const glm::vec3 cam    = m_renderWorld.camera.position;
	// The deck sits at an ABSOLUTE world altitude (mirrors Metal), so the
	// shadow slab needs no anchoring trick: it IS the layer the view march
	// samples, and the camera cannot influence it by construction.
	const float midY = cloudH + 0.5f * thick;
	// Ground level of the receivers — used ONLY to centre the map where the
	// shadows land. Off the camera, lightly hysteresised against streaming
	// jitter in the scene bounds.
	{
		HE::AABB sceneBox;
		for (const RenderObject& o : m_renderWorld.objects) sceneBox.expand(o.worldBounds);
		const float g = sceneBox.isValid() ? sceneBox.min.y : 0.0f;
		if (std::isnan(m_cloudShadowGroundY)) m_cloudShadowGroundY = g;
		const float dead = cloudH * 0.35f;
		const float d    = g - m_cloudShadowGroundY;
		if (d >  dead) m_cloudShadowGroundY = g - dead;
		if (d < -dead) m_cloudShadowGroundY = g + dead;
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

	// The pass needs only the density-formula uniforms — set them here (the sky
	// block later in the frame re-sets them all for the sky draw itself).
	glUseProgram(m_skyProgram);
	glUniform1f(m_uSkyCloudShadowPass, 1.0f);
	glUniform4f(m_uSkyCloudShadowRegion, origin.x, origin.y, size,
	            static_cast<float>(kCloudShadowMapSize));
	glUniform1i(m_uSkyCloudStyle, env.cloudStyle);
	glUniform1f(m_uSkyCloudEvolution, env.cloudEvolution);
	glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(toward));
	// cameraPos only carries the horizontal sample origin now — the map's slab
	// altitude is absolute (the shader reads it from uCloudHeight).
	const glm::vec3 mapOrigin(cam.x, groundY, cam.z);
	glUniform3fv(m_uSkyCameraPos, 1, glm::value_ptr(mapOrigin));
	glUniform1f(m_uSkyCoverage, env.cloudCoverage);
	glUniform1f(m_uSkyCloudHeight, env.cloudHeight);
	glUniform1f(m_uSkyCloudDensity, env.cloudDensity);
	glUniform1f(m_uSkyCloudFluffiness, env.cloudFluffiness);
	float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov)
		skyClock = static_cast<float>(std::atof(ov)); // deterministic headless captures
	glUniform1f(m_uSkyClock, skyClock);
	{
		const glm::vec3 wind = HE::CloudWindVector(env);
		glUniform3fv(m_uSkyWind, 1, glm::value_ptr(wind));
	}
	glActiveTexture(GL_TEXTURE2);             // 3D value-noise on unit 2 (as in the sky pass)
	glBindTexture(GL_TEXTURE_3D, m_noiseTex);
	glUniform1i(m_uSkyNoise, 2);
	glActiveTexture(GL_TEXTURE0);

	glBindFramebuffer(GL_FRAMEBUFFER, m_cloudShadowFBO);
	glViewport(0, 0, kCloudShadowMapSize, kCloudShadowMapSize);
	glDisable(GL_DEPTH_TEST);
	glBindVertexArray(m_fsVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glEnable(GL_DEPTH_TEST);
	glUniform1f(m_uSkyCloudShadowPass, 0.0f); // the sky draw reuses this program
	glUseProgram(0);
}

// Bright-pass the HDR color, then ping-pong blur. Leaves the result in
// m_bloomColor[0] and returns its id. Assumes m_fsVAO is the active VAO and
// depth test is already disabled. Restores nothing (caller rebinds output).
unsigned int OpenGLRenderer::RenderBloom(unsigned int sourceHdr, int fullW, int fullH)
{
	EnsureBloomTargets(fullW / 2, fullH / 2);
	if (!m_bloomFBO[0]) return 0;

	glViewport(0, 0, m_bloomW, m_bloomH);

	// Bright pass: HDR scene color → m_bloomColor[0].
	glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]);
	glUseProgram(m_bloomBrightProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sourceHdr);
	glUniform1i(m_uBrightHDR, 0);
	glUniform1f(m_uBrightThreshold, m_bloomThreshold);
	glUniform1f(m_uBrightKnee, m_bloomKnee);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Ping-pong Gaussian blur. Even pass count ends back in m_bloomColor[0].
	glUseProgram(m_blurProgram);
	glUniform1i(m_uBlurImage, 0);
	glUniform2f(m_uBlurTexel, 1.0f / static_cast<float>(m_bloomW),
	                          1.0f / static_cast<float>(m_bloomH));
	bool horizontal = true;
	constexpr int kBlurPasses = 10; // 5 horizontal + 5 vertical
	for (int i = 0; i < kBlurPasses; ++i)
	{
		const int dst = horizontal ? 1 : 0;
		const int src = horizontal ? 0 : 1;
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[dst]);
		glUniform1i(m_uBlurHorizontal, horizontal ? 1 : 0);
		glBindTexture(GL_TEXTURE_2D, m_bloomColor[src]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		horizontal = !horizontal;
	}
	return m_bloomColor[0];
}

// ─── Depth of field ──────────────────────────────────────────────────────────
void OpenGLRenderer::CreateDepthOfFieldPipeline()
{
	auto link = [&](const char* fsSrc, const char* what) -> GLuint
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fsSrc);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			HE_LOG_ERROR(RHI, "OpenGLRenderer: DoF %s program failed to link", what);
			glDeleteProgram(prog);
			return 0;
		}
		return prog;
	};
	m_dofCocProgram = link(kDofCocFS, "CoC");
	if (m_dofCocProgram)
	{
		m_uDofCocDepth  = glGetUniformLocation(m_dofCocProgram, "uDepth");
		m_uDofCocParams = glGetUniformLocation(m_dofCocProgram, "uDofParams");
		m_uDofCocProj   = glGetUniformLocation(m_dofCocProgram, "uDofProj");
	}
	m_dofBlurProgram = link(kDofBlurFS, "blur");
	if (m_dofBlurProgram)
	{
		m_uDofBlurImage      = glGetUniformLocation(m_dofBlurProgram, "uImage");
		m_uDofBlurCoc        = glGetUniformLocation(m_dofBlurProgram, "uCoc");
		m_uDofBlurTexel      = glGetUniformLocation(m_dofBlurProgram, "uTexel");
		m_uDofBlurHorizontal = glGetUniformLocation(m_dofBlurProgram, "uHorizontal");
		m_uDofBlurParams     = glGetUniformLocation(m_dofBlurProgram, "uDofParams");
	}
	m_dofCompositeProgram = link(kDofCompositeFS, "composite");
	if (m_dofCompositeProgram)
	{
		m_uDofCompSharp   = glGetUniformLocation(m_dofCompositeProgram, "uSharp");
		m_uDofCompBlurred = glGetUniformLocation(m_dofCompositeProgram, "uBlurred");
		m_uDofCompDepth   = glGetUniformLocation(m_dofCompositeProgram, "uDepth");
		m_uDofCompParams  = glGetUniformLocation(m_dofCompositeProgram, "uDofParams");
		m_uDofCompProj    = glGetUniformLocation(m_dofCompositeProgram, "uDofProj");
	}
}

void OpenGLRenderer::SetDepthOfFieldSettings(const DepthOfFieldSettings& s)
{
	m_dofEnabled       = s.enabled;
	m_dofFocusDistance = s.focusDistance;
	m_dofFocusRange    = s.focusRange;
	m_dofAperture      = s.aperture;
}

void OpenGLRenderer::EnsureDepthOfFieldTargets(int fullW, int fullH)
{
	fullW = std::max(2, fullW);
	fullH = std::max(2, fullH);
	if (m_dofFBO && fullW == m_dofW && fullH == m_dofH) return;
	DestroyDepthOfFieldTargets();

	const int halfW = fullW / 2, halfH = fullH / 2;
	auto makeTarget = [&](unsigned int& fbo, unsigned int& tex, GLenum internalFmt,
	                      GLenum fmt, int w, int h, const char* what)
	{
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0, fmt, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			HE_LOG_ERROR(RHI, "OpenGLRenderer: DoF %s FBO incomplete", what);
	};
	makeTarget(m_dofCocFBO,     m_dofCocTex,     GL_RG16F,   GL_RG,   halfW, halfH, "CoC");
	makeTarget(m_dofBlurFBO[0], m_dofBlurTex[0], GL_RGBA16F, GL_RGBA, halfW, halfH, "blur A");
	makeTarget(m_dofBlurFBO[1], m_dofBlurTex[1], GL_RGBA16F, GL_RGBA, halfW, halfH, "blur B");
	makeTarget(m_dofFBO,        m_dofColor,      GL_RGBA16F, GL_RGBA, fullW, fullH, "composite");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_dofW = fullW;
	m_dofH = fullH;
}

void OpenGLRenderer::DestroyDepthOfFieldTargets()
{
	if (m_dofCocFBO)     { glDeleteFramebuffers(1, &m_dofCocFBO);   m_dofCocFBO = 0; }
	if (m_dofCocTex)     { glDeleteTextures(1, &m_dofCocTex);       m_dofCocTex = 0; }
	if (m_dofBlurFBO[0]) { glDeleteFramebuffers(2, m_dofBlurFBO);   m_dofBlurFBO[0] = m_dofBlurFBO[1] = 0; }
	if (m_dofBlurTex[0]) { glDeleteTextures(2, m_dofBlurTex);       m_dofBlurTex[0] = m_dofBlurTex[1] = 0; }
	if (m_dofFBO)        { glDeleteFramebuffers(1, &m_dofFBO);      m_dofFBO = 0; }
	if (m_dofColor)      { glDeleteTextures(1, &m_dofColor);        m_dofColor = 0; }
	m_dofW = m_dofH = 0;
}

// CoC → blur H → blur V → composite. Assumes m_fsVAO is bound and the depth
// test is off (the post-process pass's state); restores nothing, the tonemap
// rebinds its own output. The max blur radius follows the f-number (f/2.8 ≈ 10
// half-res texels at 720p, f/1.4 twice that, f/22 next to nothing) and scales
// with the target height so a 4K frame is not sharper than a 720p one.
unsigned int OpenGLRenderer::RenderDepthOfField(int fullW, int fullH, const glm::mat4& proj)
{
	if (!m_dofCocProgram || !m_dofBlurProgram || !m_dofCompositeProgram || !m_hdrColor) return 0;
	if (proj[3][3] != 0.0f) return 0;   // orthographic: no lens, no depth of field
	EnsureDepthOfFieldTargets(fullW, fullH);
	if (!m_dofFBO) return 0;

	const int   halfW = m_dofW / 2, halfH = m_dofH / 2;
	const float maxRadius = std::clamp((28.0f / std::max(m_dofAperture, 0.5f))
	                                   * (static_cast<float>(halfH) / 360.0f), 0.0f, 32.0f);
	const float params[4] = { m_dofFocusDistance, std::max(m_dofFocusRange, 0.0f), maxRadius, 0.0f };
	const float projTerms[2] = { proj[2][2], proj[3][2] };

	// 1. CoC at half res from the full-res depth.
	glViewport(0, 0, halfW, halfH);
	glBindFramebuffer(GL_FRAMEBUFFER, m_dofCocFBO);
	glUseProgram(m_dofCocProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrDepth);
	glUniform1i(m_uDofCocDepth, 0);
	glUniform4fv(m_uDofCocParams, 1, params);
	glUniform2fv(m_uDofCocProj, 1, projTerms);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// 2./3. Separable CoC-weighted blur: HDR → blur[0] (H) → blur[1] (V).
	glUseProgram(m_dofBlurProgram);
	glUniform1i(m_uDofBlurImage, 0);
	glUniform1i(m_uDofBlurCoc, 1);
	glUniform2f(m_uDofBlurTexel, 1.0f / static_cast<float>(halfW), 1.0f / static_cast<float>(halfH));
	glUniform4fv(m_uDofBlurParams, 1, params);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_dofCocTex);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_FRAMEBUFFER, m_dofBlurFBO[0]);
	glUniform1i(m_uDofBlurHorizontal, 1);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindFramebuffer(GL_FRAMEBUFFER, m_dofBlurFBO[1]);
	glUniform1i(m_uDofBlurHorizontal, 0);
	glBindTexture(GL_TEXTURE_2D, m_dofBlurTex[0]);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// 4. Composite at full res.
	glViewport(0, 0, m_dofW, m_dofH);
	glBindFramebuffer(GL_FRAMEBUFFER, m_dofFBO);
	glUseProgram(m_dofCompositeProgram);
	glUniform1i(m_uDofCompSharp, 0);
	glUniform1i(m_uDofCompBlurred, 1);
	glUniform1i(m_uDofCompDepth, 2);
	glUniform4fv(m_uDofCompParams, 1, params);
	glUniform2fv(m_uDofCompProj, 1, projTerms);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_hdrDepth);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_dofBlurTex[1]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Leave units 1/2 clean: the tonemap binds its own unit 1 (bloom), and a
	// stale depth texture on unit 2 would be a feedback hazard for the next
	// scene pass that renders into m_hdrFBO.
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	return m_dofColor;
}

// ─── Motion blur ─────────────────────────────────────────────────────────────
void OpenGLRenderer::CreateMotionBlurPipeline()
{
	auto link = [&](const char* fsSrc, const char* what) -> GLuint
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fsSrc);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = GL_FALSE;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			HE_LOG_ERROR(RHI, "OpenGLRenderer: motion blur %s program failed to link", what);
			glDeleteProgram(prog);
			return 0;
		}
		return prog;
	};
	m_mbVelocityProgram = link(kMbVelocityFS, "velocity");
	if (m_mbVelocityProgram)
	{
		m_uMbVelDepth     = glGetUniformLocation(m_mbVelocityProgram, "uDepth");
		m_uMbVelReproject = glGetUniformLocation(m_mbVelocityProgram, "uReproject");
		m_uMbVelParams    = glGetUniformLocation(m_mbVelocityProgram, "uMbParams");
	}
	m_mbBlurProgram = link(kMbBlurFS, "blur");
	if (m_mbBlurProgram)
	{
		m_uMbBlurImage    = glGetUniformLocation(m_mbBlurProgram, "uImage");
		m_uMbBlurVelocity = glGetUniformLocation(m_mbBlurProgram, "uVelocity");
		m_uMbBlurTexel    = glGetUniformLocation(m_mbBlurProgram, "uTexel");
	}
}

void OpenGLRenderer::SetMotionBlurSettings(const MotionBlurSettings& s)
{
	m_mbEnabled   = s.enabled;
	m_mbIntensity = s.intensity;
	m_mbMaxBlur   = s.maxBlur;
}

void OpenGLRenderer::EnsureMotionBlurTargets(int fullW, int fullH)
{
	fullW = std::max(2, fullW);
	fullH = std::max(2, fullH);
	if (m_mbFBO && fullW == m_mbW && fullH == m_mbH) return;
	DestroyMotionBlurTargets();

	auto makeTarget = [&](unsigned int& fbo, unsigned int& tex, GLenum internalFmt,
	                      GLenum fmt, const char* what)
	{
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, fullW, fullH, 0, fmt, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			HE_LOG_ERROR(RHI, "OpenGLRenderer: motion blur %s FBO incomplete", what);
	};
	makeTarget(m_mbVelocityFBO, m_mbVelocityTex, GL_RG16F,   GL_RG,   "velocity");
	makeTarget(m_mbFBO,         m_mbColor,       GL_RGBA16F, GL_RGBA, "blur");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_mbW = fullW;
	m_mbH = fullH;
}

void OpenGLRenderer::DestroyMotionBlurTargets()
{
	if (m_mbVelocityFBO) { glDeleteFramebuffers(1, &m_mbVelocityFBO); m_mbVelocityFBO = 0; }
	if (m_mbVelocityTex) { glDeleteTextures(1, &m_mbVelocityTex);     m_mbVelocityTex = 0; }
	if (m_mbFBO)         { glDeleteFramebuffers(1, &m_mbFBO);         m_mbFBO = 0; }
	if (m_mbColor)       { glDeleteTextures(1, &m_mbColor);           m_mbColor = 0; }
	m_mbW = m_mbH = 0;
}

// Velocity → smear. Assumes m_fsVAO is bound and the depth test is off (the
// post-process pass's state); restores nothing, the tonemap rebinds its own
// output. The first frame (no previous matrix yet) reprojects with the current
// one → zero velocity → the pass is an exact copy.
unsigned int OpenGLRenderer::RenderMotionBlur(unsigned int sourceHdr, int fullW, int fullH,
                                              const glm::mat4& view, const glm::mat4& proj)
{
	if (!m_mbVelocityProgram || !m_mbBlurProgram || !sourceHdr || !m_hdrDepth) return 0;
	EnsureMotionBlurTargets(fullW, fullH);
	if (!m_mbFBO) return 0;

	const glm::mat4 viewProj  = proj * view;
	const glm::mat4 reproject = (m_mbHasPrev ? m_mbPrevViewProj : viewProj) * glm::inverse(viewProj);
	const float maxBlurPx = std::max(m_mbMaxBlur, 0.0f) * (static_cast<float>(m_mbH) / 720.0f);
	const float params[4] = { std::max(m_mbIntensity, 0.0f), maxBlurPx,
	                          static_cast<float>(m_mbW), static_cast<float>(m_mbH) };

	// 1. Velocity from the depth + the two camera matrices.
	glViewport(0, 0, m_mbW, m_mbH);
	glBindFramebuffer(GL_FRAMEBUFFER, m_mbVelocityFBO);
	glUseProgram(m_mbVelocityProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrDepth);
	glUniform1i(m_uMbVelDepth, 0);
	glUniformMatrix4fv(m_uMbVelReproject, 1, GL_FALSE, glm::value_ptr(reproject));
	glUniform4fv(m_uMbVelParams, 1, params);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// 2. Smear along it.
	glBindFramebuffer(GL_FRAMEBUFFER, m_mbFBO);
	glUseProgram(m_mbBlurProgram);
	glUniform1i(m_uMbBlurImage, 0);
	glUniform1i(m_uMbBlurVelocity, 1);
	glUniform2f(m_uMbBlurTexel, 1.0f / static_cast<float>(m_mbW), 1.0f / static_cast<float>(m_mbH));
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_mbVelocityTex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sourceHdr);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Unit 1 clean for the tonemap's bloom binding; unit 0 held the depth
	// texture a moment ago, which the next scene pass renders into.
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	return m_mbColor;
}

// ─── SSAO ────────────────────────────────────────────────────────────────────
// Kernel + rotation noise come from HorizonRendering/SsaoKernel.h: the same
// deterministic samples every backend bakes, which is what makes GL == Metal ==
// Vulkan == D3D SSAO parity possible.

void OpenGLRenderer::CreateSSAOPipeline()
{
	// Pre-pass program (writes view-space position).
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSSAOPosVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSSAOPosFS);
		m_ssaoPosProgram = glCreateProgram();
		glAttachShader(m_ssaoPosProgram, vs);
		glAttachShader(m_ssaoPosProgram, fs);
		glLinkProgram(m_ssaoPosProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		m_uPosMVP       = glGetUniformLocation(m_ssaoPosProgram, "uMVP");
		m_uPosModelView = glGetUniformLocation(m_ssaoPosProgram, "uModelView");
	}
	// Instanced pre-pass twin for GeometryPass batches. Optional: a link failure
	// only sends every batch back through the per-instance loop.
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSSAOPosInstancedVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSSAOPosFS);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs); glDeleteShader(fs);
		GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (ok)
		{
			m_ssaoPosInstancedProgram = prog;
			m_uPosInstViewProj = glGetUniformLocation(prog, "uViewProj");
			m_uPosInstView     = glGetUniformLocation(prog, "uView");
		}
		else
		{
			GLchar log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "OpenGLRenderer: instanced SSAO pre-pass link failed: %s", log);
			glDeleteProgram(prog);
		}
	}
	// Deferred P5 pre-pass variant: view-pos from the G-buffer depth (fullscreen).
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSSAODepthPosFS);
		m_ssaoDepthPosProgram = glCreateProgram();
		glAttachShader(m_ssaoDepthPosProgram, vs);
		glAttachShader(m_ssaoDepthPosProgram, fs);
		glLinkProgram(m_ssaoDepthPosProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		m_uDepthPosDepth   = glGetUniformLocation(m_ssaoDepthPosProgram, "uDepth");
		m_uDepthPosInvProj = glGetUniformLocation(m_ssaoDepthPosProgram, "uInvProj");
	}
	// Occlusion program (reuses the tonemap fullscreen-triangle VS).
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSSAOFS);
		m_ssaoProgram = glCreateProgram();
		glAttachShader(m_ssaoProgram, vs);
		glAttachShader(m_ssaoProgram, fs);
		glLinkProgram(m_ssaoProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		GLint ok = 0; glGetProgramiv(m_ssaoProgram, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			GLchar log[512]; glGetProgramInfoLog(m_ssaoProgram, sizeof(log), nullptr, log);
			throw std::runtime_error(std::string("OpenGLRenderer: SSAO link failed: ") + log);
		}
		m_uSsaoViewPos    = glGetUniformLocation(m_ssaoProgram, "uViewPos");
		m_uSsaoNoise      = glGetUniformLocation(m_ssaoProgram, "uNoise");
		m_uSsaoProj       = glGetUniformLocation(m_ssaoProgram, "uProj");
		m_uSsaoNoiseScale = glGetUniformLocation(m_ssaoProgram, "uNoiseScale");
		m_uSsaoRadius     = glGetUniformLocation(m_ssaoProgram, "uRadius");
		m_uSsaoBias       = glGetUniformLocation(m_ssaoProgram, "uBias");
		m_uSsaoIntensity  = glGetUniformLocation(m_ssaoProgram, "uIntensity");
		m_uSsaoKernel     = glGetUniformLocation(m_ssaoProgram, "uKernel");
		m_uAOMethod       = glGetUniformLocation(m_ssaoProgram, "uAOMethod");
	}
	// Blur program.
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kTonemapVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSSAOBlurFS);
		m_ssaoBlurProgram = glCreateProgram();
		glAttachShader(m_ssaoBlurProgram, vs);
		glAttachShader(m_ssaoBlurProgram, fs);
		glLinkProgram(m_ssaoBlurProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		m_uBlurAO = glGetUniformLocation(m_ssaoBlurProgram, "uAOInput");
	}
	// Upload the (constant) hemisphere kernel once.
	{
		const std::vector<glm::vec3> kernel = HE::BuildSSAOKernel(HE::kSsaoKernelSize);
		glUseProgram(m_ssaoProgram);
		glUniform3fv(m_uSsaoKernel, HE::kSsaoKernelSize, glm::value_ptr(kernel[0]));
		glUseProgram(0);
	}
	// 4×4 rotation-noise texture (NEAREST + REPEAT so it tiles per 4×4 screen block).
	{
		const std::vector<glm::vec3> noise = HE::BuildSSAONoise(HE::kSsaoNoiseCount);
		glGenTextures(1, &m_ssaoNoiseTex);
		glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
		// RGBA32F so the rotation vectors are bit-identical to the Metal backend's.
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	// 1×1 white — bound as the AO source when SSAO is off (keeps the sampler valid).
	{
		const uint8_t white[4] = { 255, 255, 255, 255 };
		glGenTextures(1, &m_whiteTex);
		glBindTexture(GL_TEXTURE_2D, m_whiteTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	// 1×1 transparent black — the neutral element for anything sampled as
	// "radiance + confidence" (the reflection result) or as an additive light
	// source: alpha 0 means "no data", so a bound dummy contributes nothing.
	// White would be a full-strength WRONG contribution in exactly those spots.
	{
		const uint8_t black[4] = { 0, 0, 0, 0 };
		glGenTextures(1, &m_blackTex);
		glBindTexture(GL_TEXTURE_2D, m_blackTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}

void OpenGLRenderer::SetSSAOSettings(const SSAOSettings& s)
{
	m_ssaoEnabled   = s.enabled;
	m_ssaoRadius    = s.radius;
	m_ssaoIntensity = s.intensity;
	m_ssaoMethod    = s.method;
}

void OpenGLRenderer::SetShadowSettings(const ShadowSettings& s)
{
	// The texture is not touched here: this is called from the editor's frame
	// push, which may land between passes. RenderScene re-specifies the array
	// at its top when the size no longer matches.
	m_shadowSizeDirty |= (s.resolution != m_shadowSettings.resolution);
	m_shadowSettings   = s;
}

void OpenGLRenderer::SetGISettings(const GISettings& s)
{
	m_giEnabled            = s.enabled && m_giSupported;
	m_giIndirectIntensity  = std::max(0.0f, s.indirectIntensity);
	m_giLightRadius        = std::clamp(s.lightRadius, 0.0f, 10.0f);
	m_giRaysPerProbe       = std::clamp(s.raysPerProbe, 8, 1024);
	m_giProbeBudgetPerFrame = std::clamp(s.probeBudgetPerFrame, 1, 4096);
}

void OpenGLRenderer::SetGIReflectionSettings(const GIReflectionSettings& s)
{
	m_giReflEnabled      = s.enabled && m_giSupported;
	m_giReflIntensity    = std::clamp(s.intensity, 0.0f, 1.0f);
	m_giReflMaxRoughness = std::clamp(s.maxRoughness, 0.0f, 1.0f);
	m_giReflMaxDistance  = std::max(1.0f, s.maxDistance);
	m_giReflQuality      = std::clamp(s.quality, 0, 2);
	m_giReflBlurEnabled  = s.blur;
}

// Screen-space reflections (docs/ssr-cross-backend-plan.md checkpoint A). No
// m_giSupported gate: the trace is a fragment shader over a rasterized pre-pass,
// so unlike the ray-traced reflections it needs neither compute nor a BVH — GL
// 4.1 (macOS included) runs it.
void OpenGLRenderer::SetOcclusionCullingSettings(const OcclusionCullingSettings& s)
{
	OcclusionCuller::Settings oc = m_occlusionCuller.settings();
	oc.enabled = s.enabled;
	m_occlusionCuller.setSettings(oc);
}

void OpenGLRenderer::SetSSRSettings(const SSRSettings& s)
{
	m_ssrEnabled      = s.enabled;
	m_ssrIntensity    = std::clamp(s.intensity, 0.0f, 1.0f);
	m_ssrMaxRoughness = std::clamp(s.maxRoughness, 0.0f, 1.0f);
	m_ssrMaxDistance  = std::max(1.0f, s.maxDistance);
	m_ssrThickness    = std::max(1e-3f, s.thickness);
	m_ssrQuality      = std::clamp(s.quality, 0, 2);
	// GIReflectionSettings::bounces is deliberately ignored here: the GL kernel
	// traces a single segment (no bounce loop — see docs/gi-reflections-plan.md
	// §10), so honouring the slider would promise something the image does not
	// deliver. Metal is the backend that implements it.
}

// ─── Global Illumination: CPU BVH acceleration structures (GL-A) ─────────────
// The software counterpart of Metal's EncodeGIAccelBuild: per-mesh BLASes are
// built once on the CPU (HE::buildGiBvh — the traversal the GLSL kernels will
// mirror is unit-tested in test_gi_bvh.cpp) and concatenated into two shared
// SSBOs; instances are a flat per-frame array referencing BLAS ranges. In
// GL-A nothing samples these yet — upload only, zero visual change.

OpenGLRenderer::GIBlasRange OpenGLRenderer::BuildGIBlas(const HE::UUID& meshId)
{
	GIBlasRange range;
	if (!m_contentManager) return range;
	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(meshId);
	if (!asset || asset->indices.empty()) return range;

	// Same two layouts ResolveMesh uploads: cooked = interleaved 8-float
	// (position at offset 0, matching Metal's BLAS vertex descriptor), loose =
	// tightly packed 3-float positions.
	HE::GiBvh bvh;
	if (asset->cooked && !asset->interleaved.empty())
		bvh = HE::buildGiBvh(asset->interleaved.data(), asset->vertexCount, 8,
		                     asset->indices.data(), asset->indices.size());
	else if (!asset->vertices.empty())
		bvh = HE::buildGiBvh(asset->vertices.data(), asset->vertices.size() / 3, 3,
		                     asset->indices.data(), asset->indices.size());
	if (!bvh.valid()) return range;

	range.nodeOffset = static_cast<int32_t>(m_giNodesCpu.size());
	range.nodeCount  = static_cast<int32_t>(bvh.nodes.size());
	range.triOffset  = static_cast<int32_t>(m_giTrisCpu.size());
	range.triCount   = static_cast<int32_t>(bvh.triangles.size());
	range.valid      = true;
	m_giNodesCpu.insert(m_giNodesCpu.end(), bvh.nodes.begin(), bvh.nodes.end());
	m_giTrisCpu.insert(m_giTrisCpu.end(), bvh.triangles.begin(), bvh.triangles.end());
	m_giBlasDirty = true;
	return range;
}

void OpenGLRenderer::UpdateGIAccel()
{
	m_giInstanceCount = 0;
	// Reflections reuse the acceleration structures without the probe/shadow
	// passes, so the build also runs when ONLY they are enabled.
	if (!(m_giEnabled || m_giReflEnabled) || !m_giSupported) return;

	// Same caster filter as the shadow pass / Metal's TLAS: castsShadow only,
	// skinned meshes are never in m_renderWorld.objects. Unculled — rays go in
	// arbitrary directions, an off-screen caster still occludes/bounces.
	m_giInstancesCpu.clear();
	auto resolveRange = [&](const HE::UUID& id) -> GIBlasRange
	{
		auto it = m_giBlasCache.find(id);
		if (it == m_giBlasCache.end())
			it = m_giBlasCache.emplace(id, BuildGIBlas(id)).first;
		return it->second;
	};
	for (const RenderObject& obj : m_renderWorld.objects)
	{
		if (!obj.castsShadow) continue;
		// Default-cube fallback — an entity without a resolvable mesh asset
		// RENDERS as the default cube (draw-loop fallback), so it must occlude
		// as one too, or plain cube entities cast no GI shadow at all.
		GIBlasRange range = resolveRange(obj.meshAssetId);
		if (!range.valid) range = resolveRange(HE::kDefaultCubeMeshId);
		if (!range.valid) continue;
		// Flat per-instance surface (albedo/emissive/metallic/roughness): the
		// shared resolution, so a graph material's folded BaseColor reaches the
		// kernels instead of plain white — see HE::giInstanceSurface.
		const HE::GiInstanceSurface surf = HE::giInstanceSurface(obj, m_contentManager);
		GIInstanceGpu inst;
		inst.invTransform = glm::inverse(obj.transform);
		inst.baseColor    = glm::vec4(surf.albedo,   surf.metallic);
		inst.emissive     = glm::vec4(surf.emissive, surf.roughness);
		inst.nodeOffset   = range.nodeOffset;
		inst.triOffset    = range.triOffset;
		inst.landIndex    = obj.landscapeIndex;   // paint sampling at the hit
		m_giInstancesCpu.push_back(inst);
	}
	m_giInstanceCount = static_cast<int>(m_giInstancesCpu.size());
	if (m_giInstanceCount == 0) return;

	// ── Painted-landscape table ──────────────────────────────────────────────
	// One entry (+ its weightmap texture) per terrain the extractor found
	// paintable, so a landscape hit reads the paint instead of one flat colour
	// for the whole terrain. Same contract and cap as the Metal path.
	{
		std::vector<GILandGpu> lands;
		m_giLandWeightTex.clear();
		for (const HE::GiLandscape& ls : m_renderWorld.landscapes)
		{
			if (static_cast<int>(lands.size()) >= HE::kGiMaxLandscapes) break;
			const unsigned int wm = ResolveGraphTexture(ls.weightmapId, {});
			if (!wm) continue;   // not resident yet → keep the flat colour
			GILandGpu g;
			g.worldToLocal = ls.worldToLocal;
			g.cfg = glm::vec4(ls.invSize.x, ls.invSize.y, ls.uvTiling,
			                  static_cast<float>(ls.layerCount));
			for (int i = 0; i < 4; ++i) g.layer[i] = ls.layerColor[i];
			lands.push_back(g);
			m_giLandWeightTex.push_back(wm);
		}
		m_giLandCount = static_cast<int>(lands.size());
		if (!m_giLandSSBO) glGenBuffers(1, &m_giLandSSBO);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_giLandSSBO);
		// Never size 0: an empty SSBO binding is a validation error on some
		// drivers, and uGiLandCount = 0 already gates every read.
		const GILandGpu dummy{};
		glBufferData(GL_SHADER_STORAGE_BUFFER,
		             static_cast<GLsizeiptr>(std::max<size_t>(lands.size(), 1) * sizeof(GILandGpu)),
		             lands.empty() ? &dummy : lands.data(), GL_DYNAMIC_DRAW);
	}

	// SSBO uploads: nodes/tris only when a new BLAS was appended, instances
	// every frame (transforms move). GL_SHADER_STORAGE_BUFFER is a 4.3 enum but
	// only reached behind m_giSupported.
	if (!m_giNodeSSBO)     glGenBuffers(1, &m_giNodeSSBO);
	if (!m_giTriSSBO)      glGenBuffers(1, &m_giTriSSBO);
	if (!m_giInstanceSSBO) glGenBuffers(1, &m_giInstanceSSBO);
	if (m_giBlasDirty)
	{
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_giNodeSSBO);
		glBufferData(GL_SHADER_STORAGE_BUFFER,
		             static_cast<GLsizeiptr>(m_giNodesCpu.size() * sizeof(HE::GiBvhNode)),
		             m_giNodesCpu.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_giTriSSBO);
		glBufferData(GL_SHADER_STORAGE_BUFFER,
		             static_cast<GLsizeiptr>(m_giTrisCpu.size() * sizeof(HE::GiBvhTriangle)),
		             m_giTrisCpu.data(), GL_STATIC_DRAW);
		m_giBlasDirty = false;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_giInstanceSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
	             static_cast<GLsizeiptr>(m_giInstancesCpu.size() * sizeof(GIInstanceGpu)),
	             m_giInstancesCpu.data(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void OpenGLRenderer::DestroyGIAccel()
{
	if (m_giNodeSSBO)     { glDeleteBuffers(1, &m_giNodeSSBO);     m_giNodeSSBO = 0; }
	if (m_giTriSSBO)      { glDeleteBuffers(1, &m_giTriSSBO);      m_giTriSSBO = 0; }
	if (m_giInstanceSSBO) { glDeleteBuffers(1, &m_giInstanceSSBO); m_giInstanceSSBO = 0; }
	if (m_giLandSSBO)     { glDeleteBuffers(1, &m_giLandSSBO);     m_giLandSSBO     = 0; }
	m_giLandWeightTex.clear();
	m_giLandCount = 0;
	m_giBlasCache.clear();
	m_giNodesCpu.clear();
	m_giTrisCpu.clear();
	m_giInstancesCpu.clear();
	m_giInstanceCount = 0;
	m_giBlasDirty     = false;
}

OpenGLRenderer::GISceneLocs OpenGLRenderer::FetchGISceneLocs(unsigned int program) const
{
	GISceneLocs l;
	l.enabled    = glGetUniformLocation(program, "uGIEnabled");
	l.shadowTex  = glGetUniformLocation(program, "uGIShadow");
	l.irrTex     = glGetUniformLocation(program, "uGIIrr");
	l.visTex     = glGetUniformLocation(program, "uGIVis");
	l.localTex   = glGetUniformLocation(program, "uGILocal");
	l.gridOrigin = glGetUniformLocation(program, "uGIGridOrigin");
	l.gridCounts = glGetUniformLocation(program, "uGIGridCounts");
	l.intensity  = glGetUniformLocation(program, "uGIIntensity");
	l.reflTex    = glGetUniformLocation(program, "uGIRefl");
	l.reflParams = glGetUniformLocation(program, "uGIReflParams");
	l.ssrTex     = glGetUniformLocation(program, "uSSRFwd");
	l.ssrParams  = glGetUniformLocation(program, "uSSRParams");
	return l;
}

// Pushes the GI scene uniforms onto the CURRENTLY BOUND program (texture
// units are shared; only location integers differ between the three programs
// sharing kUnlitFS). Inactive → just flips uGIEnabled off.
//
// `reflActive` and `ssrActive` are SEPARATE gates: both reflection sources have
// their own toggle and run with the diffuse GI switched off, so they are pushed
// before the early-out below.
void OpenGLRenderer::PushGISceneUniforms(const GISceneLocs& L, bool active, bool reflActive,
                                         bool ssrActive)
{
	if (L.reflTex >= 0) glUniform1i(L.reflTex, 18);
	if (L.reflParams >= 0)
		glUniform4f(L.reflParams, m_giReflIntensity, m_giReflMaxRoughness,
		            reflActive ? 1.0f : 0.0f, 0.0f);
	// Screen-space reflections on unit 20 — the same unit setupProgram assigns
	// heSSRFwd, so the scene pass binds the trace result once for both.
	if (L.ssrTex >= 0) glUniform1i(L.ssrTex, 20);
	if (L.ssrParams >= 0)
		glUniform4f(L.ssrParams, ssrActive ? 1.0f : 0.0f, m_ssrIntensity,
		            m_ssrMaxRoughness, 0.0f);
	if (L.enabled >= 0) glUniform1i(L.enabled, active ? 1 : 0);
	if (!active) return;
	glUniform1i(L.shadowTex, 5);
	glUniform1i(L.irrTex,    6);
	glUniform1i(L.visTex,    7);
	glUniform1i(L.localTex,  8);
	glUniform4f(L.gridOrigin, m_giGridOrigin.x, m_giGridOrigin.y, m_giGridOrigin.z, m_giProbeSpacing);
	glUniform4f(L.gridCounts, static_cast<float>(m_giGridCounts.x), static_cast<float>(m_giGridCounts.y),
	            static_cast<float>(m_giGridCounts.z), static_cast<float>(m_giProbesPerRow));
	glUniform1f(L.intensity, m_giIndirectIntensity);
}

// Lazily builds the five GI programs on the first GI-active frame. The two
// compute stages are GLSL 430 (traversal prefix + kernel, string-concatenated
// — GLSL has no #include) and only ever reach the compiler behind
// m_giSupported. A compile/link failure on an exotic driver logs + disables GI
// for the session instead of throwing the app down (blind-port safety).
void OpenGLRenderer::CreateGIPipelines()
{
	if (m_giPipelinesBuilt) return;
	m_giPipelinesBuilt = true; // one attempt per session, success or not
	try
	{
		auto link = [](GLuint vs, GLuint fs) -> GLuint
		{
			GLuint prog = glCreateProgram();
			glAttachShader(prog, vs);
			glAttachShader(prog, fs);
			glLinkProgram(prog);
			glDeleteShader(vs); glDeleteShader(fs);
			GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
			if (!ok)
			{
				GLchar log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
				glDeleteProgram(prog);
				throw std::runtime_error(std::string("GI program link failed: ") + log);
			}
			return prog;
		};
		auto linkCompute = [](const std::string& src) -> GLuint
		{
			GLuint cs   = CompileStage(GL_COMPUTE_SHADER, src.c_str());
			GLuint prog = glCreateProgram();
			glAttachShader(prog, cs);
			glLinkProgram(prog);
			glDeleteShader(cs);
			GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
			if (!ok)
			{
				GLchar log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
				glDeleteProgram(prog);
				throw std::runtime_error(std::string("GI compute link failed: ") + log);
			}
			return prog;
		};

		m_giGBufProgram = link(CompileStage(GL_VERTEX_SHADER,   kGiGBufVS),
		                       CompileStage(GL_FRAGMENT_SHADER, kGiGBufFS));
		m_giTemporalProgram = link(CompileStage(GL_VERTEX_SHADER,   kTonemapVS),
		                           CompileStage(GL_FRAGMENT_SHADER, kGiTemporalFS));
		m_giBlurProgram = link(CompileStage(GL_VERTEX_SHADER,   kTonemapVS),
		                       CompileStage(GL_FRAGMENT_SHADER, kGiBlurFS));
		m_giReflTemporalProgram = link(CompileStage(GL_VERTEX_SHADER,   kTonemapVS),
		                               CompileStage(GL_FRAGMENT_SHADER, kGiReflTemporalFS));
		m_giReflBlurProgram = link(CompileStage(GL_VERTEX_SHADER,   kTonemapVS),
		                           CompileStage(GL_FRAGMENT_SHADER, kGiReflBlurFS));
		m_giReflMixProgram  = link(CompileStage(GL_VERTEX_SHADER,   kTonemapVS),
		                           CompileStage(GL_FRAGMENT_SHADER, kGiReflMixFS));
		const std::string header = "#version 430 core\n";
		m_giShadowCSProgram = linkCompute(header + kGiTraversalGLSL + kGiShadowCS);
		m_giProbeCSProgram  = linkCompute(header + kGiTraversalGLSL + kGiProbeCS);
		m_giReflCSProgram   = linkCompute(header + kGiTraversalGLSL + kGiReflCS);
		HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: GI pipelines built (compute ray tracing active)");
	}
	catch (const std::exception& e)
	{
		HE_LOG_ERROR(RHI, "%s",
		            (std::string("OpenGLRenderer: GI pipeline build failed — GI disabled: ") + e.what()).c_str());
		if (m_giGBufProgram)     { glDeleteProgram(m_giGBufProgram);     m_giGBufProgram = 0; }
		if (m_giTemporalProgram) { glDeleteProgram(m_giTemporalProgram); m_giTemporalProgram = 0; }
		if (m_giBlurProgram)     { glDeleteProgram(m_giBlurProgram);     m_giBlurProgram = 0; }
		if (m_giShadowCSProgram) { glDeleteProgram(m_giShadowCSProgram); m_giShadowCSProgram = 0; }
		if (m_giProbeCSProgram)  { glDeleteProgram(m_giProbeCSProgram);  m_giProbeCSProgram = 0; }
		if (m_giReflCSProgram)   { glDeleteProgram(m_giReflCSProgram);   m_giReflCSProgram = 0; }
		if (m_giReflTemporalProgram) { glDeleteProgram(m_giReflTemporalProgram); m_giReflTemporalProgram = 0; }
		if (m_giReflBlurProgram)     { glDeleteProgram(m_giReflBlurProgram);     m_giReflBlurProgram = 0; }
		if (m_giReflMixProgram)      { glDeleteProgram(m_giReflMixProgram);      m_giReflMixProgram = 0; }
		m_giSupported = false;
	}

	// Instanced G-buffer pre-pass twin for GeometryPass batches. Built OUTSIDE
	// the try above on purpose: it is optional, and a link failure here must
	// not take GI down with it — every batch then loops through m_giGBufProgram.
	if (m_giGBufProgram && !m_giGBufInstancedProgram)
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kGiGBufInstancedVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kGiGBufFS);
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		glDeleteShader(vs); glDeleteShader(fs);
		GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (ok)
		{
			m_giGBufInstancedProgram = prog;
			m_uGiGBufInstViewProj = glGetUniformLocation(prog, "uViewProj");
		}
		else
		{
			GLchar log[512]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "OpenGLRenderer: instanced GI pre-pass link failed: %s", log);
			glDeleteProgram(prog);
		}
	}
}

void OpenGLRenderer::EnsureGIShadowTargets(int width, int height)
{
	width = std::max(1, width); height = std::max(1, height);
	if (m_giGBufFBO && width == m_giShadowW && height == m_giShadowH) return;
	DestroyGIShadowTargets();
	m_giShadowW = width; m_giShadowH = height;

	auto makeTex = [&](GLenum internal, GLenum filter) -> GLuint
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		// Immutable storage (4.2+, behind the 4.3 gate) — image load/store safe.
		glTexStorage2D(GL_TEXTURE_2D, 1, internal, width, height);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return t;
	};

	// World-space G-buffer: pos + normal + surface response MRT + depth.
	m_giGBufPosTex  = makeTex(GL_RGBA16F, GL_NEAREST);
	m_giGBufNormTex = makeTex(GL_RGBA16F, GL_NEAREST);
	m_giGBufMatTex  = makeTex(GL_RGBA16F, GL_NEAREST); // r = roughness, g = metallic
	glGenTextures(1, &m_giGBufDepth);
	glBindTexture(GL_TEXTURE_2D, m_giGBufDepth);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, width, height);
	glGenFramebuffers(1, &m_giGBufFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_giGBufFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giGBufPosTex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_giGBufNormTex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_giGBufMatTex, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, m_giGBufDepth, 0);
	const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	glDrawBuffers(3, bufs);

	// Raw mask (compute image store) + ping-pong temporal history + blurred result.
	m_giRawTex = makeTex(GL_R16F, GL_NEAREST);
	// Per-pixel local (point/spot) light visibility — 1 channel per light,
	// first 4. Deterministic hard rays → no temporal/blur chain; the scene
	// shader samples it directly (LINEAR = free bilinear upsample, like the
	// blurred sun mask).
	m_giLocalMaskTex = makeTex(GL_RGBA16F, GL_LINEAR);
	for (int i = 0; i < 2; ++i)
	{
		m_giHistTex[i] = makeTex(GL_RGBA16F, GL_NEAREST);
		glGenFramebuffers(1, &m_giHistFBO[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_giHistFBO[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giHistTex[i], 0);
	}
	// The scene shader samples the result full-res — LINEAR = free bilinear upsample.
	m_giResultTex = makeTex(GL_R16F, GL_LINEAR);
	glGenFramebuffers(1, &m_giResultFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_giResultFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giResultTex, 0);

	// Ray-traced reflections: the compute kernel image-stores the raw trace, the
	// (optional) temporal pass ping-pongs radiance + receiver position through an
	// MRT pair, and the (optional) blur ends in m_giReflTex. Everything the
	// shading pass samples is LINEAR — it upsamples half-res to full-res.
	m_giReflRawTex  = makeTex(GL_RGBA16F, GL_LINEAR);
	m_giReflTex     = makeTex(GL_RGBA16F, GL_LINEAR);
	m_giReflBlurTex = makeTex(GL_RGBA16F, GL_LINEAR);
	glGenFramebuffers(1, &m_giReflFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_giReflFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giReflTex, 0);
	glGenFramebuffers(1, &m_giReflBlurFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_giReflBlurFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giReflBlurTex, 0);
	for (int i = 0; i < 2; ++i)
	{
		m_giReflHistTex[i]    = makeTex(GL_RGBA16F, GL_LINEAR);
		m_giReflHistPosTex[i] = makeTex(GL_RGBA16F, GL_NEAREST); // exact positions, never interpolated across an edge
		glGenFramebuffers(1, &m_giReflHistFBO[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_giReflHistFBO[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giReflHistTex[i], 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_giReflHistPosTex[i], 0);
		const GLenum histBufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, histBufs);
		// glTexStorage2D contents are undefined and the FIRST temporal frame
		// samples both of these. m_giReflHistValid = false already forces the
		// blend weight to 0 that frame, but a zeroed history costs one clear and
		// removes the reliance (same reasoning as EnsureGIProbeAtlas above).
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	m_giReflHistValid = false;
	glBindTexture(GL_TEXTURE_2D, 0);
	m_giHistValid = false; // fresh targets → no usable history
}

void OpenGLRenderer::DestroyGIShadowTargets()
{
	if (m_giGBufFBO)     { glDeleteFramebuffers(1, &m_giGBufFBO);   m_giGBufFBO = 0; }
	if (m_giGBufPosTex)  { glDeleteTextures(1, &m_giGBufPosTex);    m_giGBufPosTex = 0; }
	if (m_giGBufNormTex) { glDeleteTextures(1, &m_giGBufNormTex);   m_giGBufNormTex = 0; }
	if (m_giGBufMatTex)  { glDeleteTextures(1, &m_giGBufMatTex);    m_giGBufMatTex = 0; }
	if (m_giGBufDepth)   { glDeleteTextures(1, &m_giGBufDepth);     m_giGBufDepth = 0; }
	if (m_giRawTex)      { glDeleteTextures(1, &m_giRawTex);        m_giRawTex = 0; }
	if (m_giLocalMaskTex) { glDeleteTextures(1, &m_giLocalMaskTex); m_giLocalMaskTex = 0; }
	for (int i = 0; i < 2; ++i)
	{
		if (m_giHistFBO[i]) { glDeleteFramebuffers(1, &m_giHistFBO[i]); m_giHistFBO[i] = 0; }
		if (m_giHistTex[i]) { glDeleteTextures(1, &m_giHistTex[i]);     m_giHistTex[i] = 0; }
	}
	if (m_giResultFBO) { glDeleteFramebuffers(1, &m_giResultFBO); m_giResultFBO = 0; }
	if (m_giResultTex) { glDeleteTextures(1, &m_giResultTex);     m_giResultTex = 0; }
	if (m_giReflFBO)     { glDeleteFramebuffers(1, &m_giReflFBO);     m_giReflFBO = 0; }
	if (m_giReflTex)     { glDeleteTextures(1, &m_giReflTex);         m_giReflTex = 0; }
	if (m_giReflRawTex)  { glDeleteTextures(1, &m_giReflRawTex);      m_giReflRawTex = 0; }
	if (m_giReflBlurFBO) { glDeleteFramebuffers(1, &m_giReflBlurFBO); m_giReflBlurFBO = 0; }
	if (m_giReflBlurTex) { glDeleteTextures(1, &m_giReflBlurTex);     m_giReflBlurTex = 0; }
	for (int i = 0; i < 2; ++i)
	{
		if (m_giReflHistFBO[i])    { glDeleteFramebuffers(1, &m_giReflHistFBO[i]); m_giReflHistFBO[i] = 0; }
		if (m_giReflHistTex[i])    { glDeleteTextures(1, &m_giReflHistTex[i]);     m_giReflHistTex[i] = 0; }
		if (m_giReflHistPosTex[i]) { glDeleteTextures(1, &m_giReflHistPosTex[i]);  m_giReflHistPosTex[i] = 0; }
	}
	m_giShadowW = m_giShadowH = 0;
	m_giHistValid = false;
	m_giReflHistValid = false;
}

// Probe-grid fit over the scene AABB (GIProbeGrid.h: spacing grows so the
// whole scene — a 100 m terrain included — fits the probe budget). Metal
// lesson: refresh worldBounds from the real mesh bounds first — the extractor
// leaves them invalid or proxy-sized, so unioning them raw undersizes the grid.
// Once built, the fit is only re-checked when the scene's geometry changed
// (signature or an InvalidateMesh), and replaced when the scene left it.
void OpenGLRenderer::EnsureGIProbeGrid()
{
	if (m_renderWorld.objects.empty()) return;
	const uint64_t sig = HE::GIProbeSceneSignature(m_renderWorld.objects);
	if (m_giGridTrack.canSkip(m_giProbeGridBuilt, sig)) return;

	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId); mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);
	int unresolved = 0;
	const HE::AABB sceneBox = HE::GIProbeSceneBounds(m_renderWorld.objects, &unresolved);
	if (!sceneBox.isValid()) return;
	if (!m_giGridTrack.shouldEvaluate(m_giProbeGridBuilt, sig, unresolved)) return;

	if (m_giProbeGridBuilt)
	{
		HE::GIProbeGridFit current;
		current.origin = m_giGridOrigin; current.counts = m_giGridCounts; current.spacing = m_giProbeSpacing;
		if (!HE::GIProbeGridNeedsRefit(current, sceneBox)) return;
		DestroyGIProbeAtlas(); // new counts → new atlas; probes re-converge
	}
	const HE::GIProbeGridFit fit = HE::FitGIProbeGrid(sceneBox);
	if (!fit.valid()) return;

	m_giGridCounts   = fit.counts;
	m_giGridOrigin   = fit.origin;
	m_giProbeSpacing = fit.spacing;
	m_giProbeCount   = fit.probeCount();
	m_giProbesPerRow = std::min(m_giProbeCount, 32);
	m_giProbeCursor  = 0;
	m_giProbeGridBuilt = true;
	HE_LOG_INFO(RHI, "%s",
	            ("OpenGLRenderer: GI probe grid " + std::to_string(m_giGridCounts.x) + "x"
	             + std::to_string(m_giGridCounts.y) + "x" + std::to_string(m_giGridCounts.z)
	             + " (" + std::to_string(m_giProbeCount) + " probes), spacing "
	             + std::to_string(m_giProbeSpacing)).c_str());
}

void OpenGLRenderer::EnsureGIProbeAtlas()
{
	if (m_giIrrAtlas || m_giProbeCount <= 0) return;
	const int rows = (m_giProbeCount + m_giProbesPerRow - 1) / m_giProbesPerRow;
	const int w = m_giProbesPerRow * kGIProbeOctSize;
	const int h = rows * kGIProbeOctSize;

	auto makeAtlas = [&](GLenum internal) -> GLuint
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return t;
	};
	m_giIrrAtlas = makeAtlas(GL_RGBA16F);
	m_giVisAtlas = makeAtlas(GL_RG16F);

	// glTexStorage2D contents are undefined and the probe kernel EMA-reads its
	// own previous value — clear both once via a throwaway FBO
	// (glClearTexImage is 4.4, one step above the 4.3 gate).
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giIrrAtlas, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_giVisAtlas, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::DestroyGIProbeAtlas()
{
	if (m_giIrrAtlas) { glDeleteTextures(1, &m_giIrrAtlas); m_giIrrAtlas = 0; }
	if (m_giVisAtlas) { glDeleteTextures(1, &m_giVisAtlas); m_giVisAtlas = 0; }
	m_giProbeGridBuilt = false;
	m_giProbeCount = 0;
	m_giProbeCursor = 0;
}

// Half-res world-space G-buffer (position + normal + roughness/metallic MRT).
// Shared by the shadow-ray kernel and the reflection kernel, which is why it is
// its own function: GI reflections can run with the DIFFUSE GI switched off, and
// then this is the only GI raster pass in the frame. Same draw set as the scene
// pass — every shaded pixel needs a value. Returns false when the pipelines or
// targets are unavailable (GI then stays off for the frame).
bool OpenGLRenderer::RenderGIPrepass(const CommandBuffer& cmds, int width, int height,
                                     const glm::mat4& viewProj)
{
	CreateGIPipelines();
	if (!m_giGBufProgram) return false;
	EnsureGIShadowTargets(width, height);
	if (!m_giGBufFBO) return false;

	glBindFramebuffer(GL_FRAMEBUFFER, m_giGBufFBO);
	glViewport(0, 0, width, height);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // a = 0 → background
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(m_giGBufProgram);
	const GLint uMVP        = glGetUniformLocation(m_giGBufProgram, "uMVP");
	const GLint uModel      = glGetUniformLocation(m_giGBufProgram, "uModel");
	const GLint uRoughMetal = glGetUniformLocation(m_giGBufProgram, "uRoughMetal");
	// The instanced twin shares kGiGBufFS, so it has its own uRoughMetal lane.
	const GLint uInstRoughMetal = m_giGBufInstancedProgram
		? glGetUniformLocation(m_giGBufInstancedProgram, "uRoughMetal") : -1;
	const bool  canInstance = m_giGBufInstancedProgram && m_instanceVBO;
	{
		unsigned int boundProgram = m_giGBufProgram;
		HE::UUID lastId{}; const GpuMesh* cMesh = nullptr; bool valid = false;
		for (const DrawCall& dc : cmds.drawCalls())
		{
			if (!dc.contributesAO) continue; // precip/particles don't shade the mask
			if (!valid || dc.meshAssetId != lastId)
			{ cMesh = ResolveMesh(dc.meshAssetId); lastId = dc.meshAssetId; valid = true; }
			const GpuMesh* mesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!mesh) continue;
			// Surface response for the reflection kernel — the same override
			// resolution the scene pass does, so a mirror material reads as a
			// mirror here too (unused by the shadow kernel).
			glm::vec3 dcBase = dc.baseColor;
			float     dcMetal = dc.metallic, dcRough = dc.roughness, dcOpacity = dc.opacity;
			ResolveMaterialParams(dc.materialAssetId, dcBase, dcMetal, dcRough, dcOpacity);
			glBindVertexArray(mesh->vao);
			const GlIndexRange range = DrawIndexRange(dc, mesh->indexCount); // section or whole

			// A GeometryPass batch (instanceTransforms non-empty ⇔ run > 1) is one
			// instanced draw: the transforms go through the scene pass's scratch
			// VBO, which every mesh VAO reads at attribs 4–7 with divisor 1.
			if (!dc.instanceTransforms.empty() && canInstance)
			{
				glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
				glBufferData(GL_ARRAY_BUFFER,
				             static_cast<GLsizeiptr>(dc.instanceTransforms.size() * sizeof(glm::mat4)),
				             dc.instanceTransforms.data(), GL_STREAM_DRAW);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				if (boundProgram != m_giGBufInstancedProgram)
				{
					glUseProgram(m_giGBufInstancedProgram);
					boundProgram = m_giGBufInstancedProgram;
					glUniformMatrix4fv(m_uGiGBufInstViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
				}
				if (uInstRoughMetal >= 0) glUniform2f(uInstRoughMetal, dcRough, dcMetal);
				glDrawElementsInstanced(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset,
				                        static_cast<GLsizei>(dc.instanceTransforms.size()));
				static bool loggedOnce = false; // once per session, like the shadow pass
				if (!loggedOnce)
				{
					loggedOnce = true;
					HE_LOG_INFO(RHI, "OpenGLRenderer: GI pre-pass instanced (first batch: %u instances)",
					            static_cast<unsigned>(dc.instanceTransforms.size()));
				}
				continue;
			}
			if (boundProgram != m_giGBufProgram)
			{
				glUseProgram(m_giGBufProgram);
				boundProgram = m_giGBufProgram;
			}
			if (uRoughMetal >= 0) glUniform2f(uRoughMetal, dcRough, dcMetal);
			auto drawOne = [&](const glm::mat4& t)
			{
				glUniformMatrix4fv(uMVP,   1, GL_FALSE, glm::value_ptr(viewProj * t));
				glUniformMatrix4fv(uModel, 1, GL_FALSE, glm::value_ptr(t));
				glDrawElements(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset);
			};
			if (!dc.instanceTransforms.empty())
				for (const glm::mat4& t : dc.instanceTransforms) drawOne(t);
			else
				drawOne(dc.transform);
		}
	}
	return true;
}

// Shadow-ray kernel → temporal → blur, on the pre-pass targets RenderGIPrepass
// filled this frame. Returns the blurred mask texture (0 if unavailable).
unsigned int OpenGLRenderer::RenderGIShadow(int width, int height, const glm::mat4& viewProj)
{
	if (!m_giShadowCSProgram || !m_giTemporalProgram || !m_giBlurProgram) return 0;
	if (!m_giGBufFBO) return 0;
	// Explicit: the temporal/blur fullscreen draws below are half-res, and the
	// reflection pass may have run between the pre-pass and here.
	glViewport(0, 0, width, height);

	// ── 2. Shadow rays (compute, 1 ray/pixel against the BVH SSBOs) ──────────
	glUseProgram(m_giShadowCSProgram);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_giNodeSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_giTriSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_giInstanceSSBO);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_giGBufPosTex);
	glUniform1i(glGetUniformLocation(m_giShadowCSProgram, "uGPos"), 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_giGBufNormTex);
	glUniform1i(glGetUniformLocation(m_giShadowCSProgram, "uGNorm"), 1);
	glBindImageTexture(0, m_giRawTex,       0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
	glBindImageTexture(1, m_giLocalMaskTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glm::vec3 towardLight, lightColorIntensity;
	m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
	m_giFrameSeed += 1.0f;
	glUniform4f(glGetUniformLocation(m_giShadowCSProgram, "uSunDirRadius"),
	            towardLight.x, towardLight.y, towardLight.z, glm::radians(m_giLightRadius));
	glUniform4f(glGetUniformLocation(m_giShadowCSProgram, "uFrame"),
	            m_giFrameSeed, static_cast<float>(width), static_cast<float>(height), 0.0f);
	// First 4 local (point/spot) lights of the same 8-light window the scene
	// shader iterates — its channel index is a plain counter over type != 0
	// in the SAME order (see HE::BuildMaskedLocalLights for the full why).
	{
		const HE::PackedLocalShadowLights masked = HE::BuildMaskedLocalLights(m_renderWorld);
		glUniform4fv(glGetUniformLocation(m_giShadowCSProgram, "uLocalPosRange"),
		             HE::kMaxMaskedLocalLights, glm::value_ptr(masked.posRange[0]));
		glUniform4f(glGetUniformLocation(m_giShadowCSProgram, "uLocalExtra"),
		            static_cast<float>(masked.count), 0.0f, 0.0f, 0.0f);
	}
	glUniform1i(glGetUniformLocation(m_giShadowCSProgram, "uGiInstanceCount"), m_giInstanceCount);
	glDispatchCompute(static_cast<GLuint>((width + 7) / 8), static_cast<GLuint>((height + 7) / 8), 1);
	// The temporal pass SAMPLES the image-stored raw mask next.
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

	// ── 3. Temporal accumulation (fullscreen, ping-pong history) ─────────────
	const int curIdx = m_giHistIdx, prevIdx = 1 - curIdx;
	glDisable(GL_DEPTH_TEST);
	glBindVertexArray(m_fsVAO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_giHistFBO[curIdx]);
	glUseProgram(m_giTemporalProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_giGBufPosTex);
	glUniform1i(glGetUniformLocation(m_giTemporalProgram, "uGPos"), 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_giRawTex);
	glUniform1i(glGetUniformLocation(m_giTemporalProgram, "uRaw"), 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, m_giHistTex[prevIdx]);
	glUniform1i(glGetUniformLocation(m_giTemporalProgram, "uHistory"), 2);
	glUniformMatrix4fv(glGetUniformLocation(m_giTemporalProgram, "uPrevViewProj"),
	                   1, GL_FALSE, glm::value_ptr(m_giPrevViewProj));
	glUniform1f(glGetUniformLocation(m_giTemporalProgram, "uBlend"), m_giHistValid ? 0.9f : 0.0f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	m_giHistValid   = true;
	m_giHistIdx     = prevIdx;
	m_giPrevViewProj = viewProj; // for NEXT frame's reprojection

	// ── 4. Spatial blur → the mask the scene shader samples ─────────────────
	glBindFramebuffer(GL_FRAMEBUFFER, m_giResultFBO);
	glUseProgram(m_giBlurProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_giHistTex[curIdx]);
	glUniform1i(glGetUniformLocation(m_giBlurProgram, "uSrc"), 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_DEPTH_TEST);
	return m_giResultTex;
}

// ─── Ray-traced GI reflections (docs/gi-reflections-plan.md §10, GL port) ─────
// Trace → (temporal) → (blur), all half-res on the pre-pass targets. Returns the
// texture the shading pass should sample (rgb radiance, a confidence), or 0 when
// the pass could not run — the caller then binds a black dummy and leaves the
// gate off, so the image is exactly as if the feature did not exist.
//
// `probesValid` = the DDGI atlases hold real data this frame. Reflections run
// WITHOUT them (sun + local lights + the flat ambient floor at each hit), which
// is why the whole pass is independent of the diffuse GI toggle.
unsigned int OpenGLRenderer::RenderGIReflections(int width, int height,
                                                 const glm::mat4& viewProj, bool probesValid)
{
	if (!m_giReflCSProgram || !m_giGBufFBO || !m_giReflRawTex) return 0;

	// ── 1. Trace: one specular ray per pixel against the BVH SSBOs ───────────
	glUseProgram(m_giReflCSProgram);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_giNodeSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_giTriSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_giInstanceSSBO);
	auto loc = [&](const char* n) { return glGetUniformLocation(m_giReflCSProgram, n); };
	auto bindTex = [&](GLenum unit, const char* name, unsigned int tex, int slot)
	{
		glActiveTexture(unit);
		glBindTexture(GL_TEXTURE_2D, tex);
		glUniform1i(loc(name), slot);
	};
	bindTex(GL_TEXTURE0, "uGPos",  m_giGBufPosTex,  0);
	bindTex(GL_TEXTURE1, "uGNorm", m_giGBufNormTex, 1);
	bindTex(GL_TEXTURE2, "uGMat",  m_giGBufMatTex,  2);
	// Black (not white) when the atlases are absent: the kernel gates on
	// uReflParams.w anyway, but a white dummy would be a bright indirect term if
	// that gate ever regressed.
	bindTex(GL_TEXTURE3, "uGIIrr", (probesValid && m_giIrrAtlas) ? m_giIrrAtlas : m_blackTex, 3);
	bindTex(GL_TEXTURE4, "uGIVis", (probesValid && m_giVisAtlas) ? m_giVisAtlas : m_blackTex, 4);
	glBindImageTexture(0, m_giReflRawTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	glm::vec3 towardLight, lightColorIntensity;
	m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
	const HE::PackedLightArray lights = HE::BuildPackedLightArray(m_renderWorld);
	const glm::vec3 camPos = m_renderWorld.camera.position;
	m_giReflFrameSeed += 1.0f;
	glUniform4f(loc("uCamPos"), camPos.x, camPos.y, camPos.z, 0.0f);
	glUniform4f(loc("uSunDir"), towardLight.x, towardLight.y, towardLight.z,
	            static_cast<float>(lights.count));
	glUniform4f(loc("uSunColor"), lightColorIntensity.r, lightColorIntensity.g, lightColorIntensity.b, 0.0f);
	glUniform4f(loc("uAmbient"), m_renderWorld.ambient.r, m_renderWorld.ambient.g, m_renderWorld.ambient.b, 0.0f);
	glUniform4f(loc("uGridOrigin"), m_giGridOrigin.x, m_giGridOrigin.y, m_giGridOrigin.z, m_giProbeSpacing);
	glUniform4f(loc("uGridCounts"), static_cast<float>(m_giGridCounts.x), static_cast<float>(m_giGridCounts.y),
	            static_cast<float>(m_giGridCounts.z), static_cast<float>(m_giProbesPerRow));
	glUniform4f(loc("uReflParams"), m_giReflMaxDistance, m_giReflMaxRoughness,
	            m_giIndirectIntensity, probesValid ? 1.0f : 0.0f);
	// uFrame.w gates the glossy cone. Deriving it from the TIER (quality >= 2)
	// meant Medium uploaded uRays = 2 and then traced 1, because the gate closed
	// the cone and the kernel collapses rays to 1 with no cone — the ray ladder
	// read 1/1/4 on GL and 1/2/4 on Metal for the same setting. Derive it from
	// the ray count instead, exactly as Metal derives `jitter = rays > 1`.
	const float giReflRays = m_giReflQuality >= 2 ? 4.0f
	                       : (m_giReflQuality >= 1 ? 2.0f : 1.0f);
	glUniform4f(loc("uFrame"), m_giReflFrameSeed, static_cast<float>(width),
	            static_cast<float>(height), (giReflRays > 1.0f) ? 1.0f : 0.0f);
	if (lights.count > 0)
	{
		glUniform4fv(loc("uLightPosRange"),  lights.count, glm::value_ptr(lights.posRange[0]));
		glUniform4fv(loc("uLightColorType"), lights.count, glm::value_ptr(lights.colorType[0]));
		glUniform4fv(loc("uLightDirCos"),    lights.count, glm::value_ptr(lights.dirCos[0]));
	}
	glUniform1i(loc("uGiInstanceCount"), m_giInstanceCount);
	// Painted landscapes: table + one weightmap each (units 5..8). Unused units
	// take the black dummy; uGiLandCount gates the kernel, which never samples
	// a slot it was not given.
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_giLandSSBO);
	glUniform1i(loc("uGiLandCount"), m_giLandCount);
	// Rays per pixel from the tier — Low 1, Medium 2, High 4; a near-mirror
	// traces one regardless (its cone is narrower than a pixel).
	glUniform1f(loc("uRays"), giReflRays);
	for (int i = 0; i < HE::kGiMaxLandscapes; ++i)
	{
		const std::string nm = "uLandWeights[" + std::to_string(i) + "]";
		bindTex(GL_TEXTURE5 + i, nm.c_str(),
		        i < static_cast<int>(m_giLandWeightTex.size()) ? m_giLandWeightTex[i] : m_blackTex,
		        5 + i);
	}
	glDispatchCompute(static_cast<GLuint>((width + 7) / 8), static_cast<GLuint>((height + 7) / 8), 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

	unsigned int src = m_giReflRawTex;
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, width, height);
	glBindVertexArray(m_fsVAO);

	// ── 2. Temporal accumulation (quality High) ──────────────────────────────
	// Temporal pass disabled with the blend below — see there. Left in the build
	// (and still gated on the tier) so re-enabling is one constant.
	if (false && m_giReflQuality >= 2 && m_giReflTemporalProgram && m_giReflHistFBO[0])
	{
		const int curIdx = m_giReflHistIdx, prevIdx = 1 - curIdx;
		glBindFramebuffer(GL_FRAMEBUFFER, m_giReflHistFBO[curIdx]);
		glUseProgram(m_giReflTemporalProgram);
		auto tloc = [&](const char* n) { return glGetUniformLocation(m_giReflTemporalProgram, n); };
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_giGBufPosTex);
		glUniform1i(tloc("uGPos"), 0);
		glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, src);
		glUniform1i(tloc("uRaw"), 1);
		glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_giReflHistTex[prevIdx]);
		glUniform1i(tloc("uHistory"), 2);
		glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_giReflHistPosTex[prevIdx]);
		glUniform1i(tloc("uHistPos"), 3);
		glUniformMatrix4fv(tloc("uPrevViewProj"), 1, GL_FALSE, glm::value_ptr(m_giReflPrevViewProj));
		// Adaptive EMA (Metal lesson, gi-reflections-plan P4): with a moving
		// camera a stiff history makes the reflection visibly lag behind the
		// surface carrying it — reflected content is never reprojected, only the
		// receiver is. Damp on any view change, hold the long history when still.
		const bool camMoved = (viewProj != m_giReflPrevViewProj);
		// Temporal accumulation is OFF: it was the reflection's latency (an EMA of
		// 0.85 is several frames of history, and only the upper tiers ran it, so
		// the better the tier the more it dragged). Rays carry the estimate, the
		// blur covers the resolution. Kept wired so it is one constant to undo.
		const float blend = 0.0f;
		(void)camMoved;
		glUniform1f(tloc("uBlend"), blend);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		src = m_giReflHistTex[curIdx];
		m_giReflHistIdx   = prevIdx;
		m_giReflHistValid = true;
	}
	m_giReflPrevViewProj = viewProj; // for NEXT frame's reprojection

	// ── 3. Separable confidence-weighted blur ────────────────────────────────
	// WIDTH is inverse to the ray count — the other half of what the quality
	// tier means. The blur stands in for a glossy lobe the trace did not sample:
	// one deterministic ray knows nothing about the lobe and needs a wide blur
	// to fake one; four stratified rays have sampled it and need barely any, so
	// the reflection stays SHARP. Higher tier = sharper, not softer.
	//
	// KNOWN GAP vs Metal: there the sharp trace and the blurred copy are kept
	// apart and lerped per pixel by roughness, so a mirror stays mirror-sharp at
	// every tier. The GL scene shader samples ONE reflection texture (uGIRefl),
	// so the blur lands on mirrors too. Closing it needs the same roughness-mix
	// pass Metal got (kSSRRoughMixFS) plus one more render target.
	if (m_giReflBlurEnabled && m_giReflBlurProgram && m_giReflFBO && m_giReflBlurFBO)
	{
		// The blur stands in for TWO things the trace did not sample, and sizing
		// it to only one of them is what kept the ladder inverted (see Metal's
		// copy of this comment for the full reasoning):
		//   · the RESOLUTION — one texel spans resDiv screen pixels.
		//   · the LOBE — constant in SCREEN pixels, because how far a rough
		//     surface scatters is a property of the material, not of the tier.
		// Built as an A-TROUS CHAIN at doubling strides rather than one wide
		// 5-tap, which would band. Each pass reads one texture and writes a
		// different one (H → blurTex, V → reflTex, next level reads reflTex):
		// letting a level write back into what it reads is a read-write hazard.
		// resDiv is 2 unconditionally here: GL traces at the prepass resolution
		// (see the gap note in DrawScene), so one texel always spans two screen
		// pixels. Reach is otherwise Metal's formula, and the strides are SCALED
		// to land on it exactly — doubling raw strides and stopping past the
		// target snaps the total to 2^n − 1, which quantizes the "tier-
		// independent" screen reach and can make a better tier blur wider.
		const float resDiv = 2.0f;
		// Lobe term divided by the RAY COUNT, like Metal: the blur only stands in
		// for the part of the lobe the rays did not sample, so a tier that traces
		// four of them needs a quarter of the filter. Holding it constant across
		// tiers made the better tier measurably softer.
		const float reach = std::clamp(
			1.0f + (kGIReflLobeScreenPx * std::clamp(m_giReflMaxRoughness, 0.0f, 1.0f))
			     / (resDiv * std::max(giReflRays, 1.0f)), 1.0f, 512.0f);
		const int   levels = std::clamp(
			static_cast<int>(std::ceil(std::log2(reach + 1.0f))), 1, 6);
		const float unit = reach / (std::exp2(static_cast<float>(levels)) - 1.0f);
		glUseProgram(m_giReflBlurProgram);
		const GLint uSrc = glGetUniformLocation(m_giReflBlurProgram, "uSrc");
		const GLint uDir = glGetUniformLocation(m_giReflBlurProgram, "uDir");
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(uSrc, 0);
		for (int lvl = 0; lvl < levels; ++lvl)
		{
			const float stride = unit * std::exp2(static_cast<float>(lvl));
			const float tx = stride / static_cast<float>(std::max(1, width));
			const float ty = stride / static_cast<float>(std::max(1, height));
			glBindFramebuffer(GL_FRAMEBUFFER, m_giReflBlurFBO);
			glBindTexture(GL_TEXTURE_2D, src);
			glUniform2f(uDir, tx, 0.0f);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glBindFramebuffer(GL_FRAMEBUFFER, m_giReflFBO);
			glBindTexture(GL_TEXTURE_2D, m_giReflBlurTex);
			glUniform2f(uDir, 0.0f, ty);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			src = m_giReflTex;
		}
		// Lerp the SHARP trace back in per pixel, so a mirror stays a mirror.
		// m_giReflRawTex still holds the unblurred trace (the chain only ever
		// read it); m_giReflBlurTex is the H ping, dead once the chain is done,
		// so the mix needs no extra target. Reads raw/blurred/prepass, writes
		// blurTex — no pass reads and writes the same texture.
		if (m_giReflMixProgram)
		{
			glUseProgram(m_giReflMixProgram);
			glBindFramebuffer(GL_FRAMEBUFFER, m_giReflBlurFBO);
			glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_giReflRawTex);
			glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_giReflTex);
			glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_giGBufMatTex);
			glUniform1i(glGetUniformLocation(m_giReflMixProgram, "uSharp"),    0);
			glUniform1i(glGetUniformLocation(m_giReflMixProgram, "uBlurred"),  1);
			glUniform1i(glGetUniformLocation(m_giReflMixProgram, "uGMat"),     2);
			// Resolution floor: GL always traces at half res (see the gap note),
			// so a mirror keeps the same mild floor Metal's Medium tier uses.
			glUniform1f(glGetUniformLocation(m_giReflMixProgram, "uFloor"), 0.35f);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glActiveTexture(GL_TEXTURE0);
			src = m_giReflBlurTex;
		}
	}

	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_DEPTH_TEST);
	return src;
}

void OpenGLRenderer::DispatchGIProbeUpdate()
{
	if (!m_giProbeCSProgram || m_giInstanceCount == 0) return;
	EnsureGIProbeGrid();
	if (!m_giProbeGridBuilt) return;
	EnsureGIProbeAtlas();
	if (!m_giIrrAtlas || !m_giVisAtlas) return;

	const int budget = std::min(m_giProbeBudgetPerFrame > 0 ? m_giProbeBudgetPerFrame : 1, m_giProbeCount);

	glUseProgram(m_giProbeCSProgram);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_giNodeSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_giTriSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_giInstanceSSBO);
	glBindImageTexture(0, m_giIrrAtlas, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
	glBindImageTexture(1, m_giVisAtlas, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);

	auto loc = [&](const char* n) { return glGetUniformLocation(m_giProbeCSProgram, n); };
	glUniform4f(loc("uGridOrigin"), m_giGridOrigin.x, m_giGridOrigin.y, m_giGridOrigin.z, m_giProbeSpacing);
	glUniform4f(loc("uGridCounts"), static_cast<float>(m_giGridCounts.x), static_cast<float>(m_giGridCounts.y),
	            static_cast<float>(m_giGridCounts.z), static_cast<float>(m_giProbesPerRow));
	const float maxDist = glm::length(glm::vec3(m_giGridCounts) * m_giProbeSpacing) + m_giProbeSpacing;
	glUniform4f(loc("uRayParams"), maxDist, 0.92f,
	            static_cast<float>(m_giProbeCursor), static_cast<float>(budget));

	// Dominant directional + up to 8 local lights — the same bounce estimate
	// (and the same night/local-light lessons) as Metal's EncodeGIProbeUpdate.
	glm::vec3 towardLight, lightColorIntensity;
	m_renderWorld.dominantDirectionalLight(towardLight, lightColorIntensity);
	const HE::PackedLightArray lights = HE::BuildPackedLightArray(m_renderWorld);
	glUniform4f(loc("uSunDirRadius"), towardLight.x, towardLight.y, towardLight.z,
	            static_cast<float>(lights.count));
	glUniform4f(loc("uSunColor"), lightColorIntensity.r, lightColorIntensity.g, lightColorIntensity.b, 0.0f);
	glUniform4f(loc("uSkyAmbient"), m_renderWorld.ambient.r, m_renderWorld.ambient.g, m_renderWorld.ambient.b, 0.0f);
	if (lights.count > 0)
	{
		glUniform4fv(loc("uLightPosRange"),  lights.count, glm::value_ptr(lights.posRange[0]));
		glUniform4fv(loc("uLightColorType"), lights.count, glm::value_ptr(lights.colorType[0]));
		glUniform4fv(loc("uLightDirCos"),    lights.count, glm::value_ptr(lights.dirCos[0]));
	}
	glUniform1i(loc("uGiInstanceCount"), m_giInstanceCount);

	glDispatchCompute(static_cast<GLuint>(budget), 1, 1);
	// Next frame's dispatch EMA-reads these texels, and the scene pass samples
	// them as textures right after — both need the barrier.
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	m_giProbeCursor = (m_giProbeCursor + budget) % m_giProbeCount;
}

// ─── Reflection pre-pass program (forward SSR, plan A2/§3.2 way (a)) ─────────
// The SHARED library shader, cross-compiled to GLSL 410 — the same text Metal
// builds its MRT pre-pass from, so the oct encoding and the depth convention
// cannot drift between the two backends. Built lazily on the first SSR frame;
// a failure logs once and SSR simply never runs.
//
// UBO binding points, continuing the decal comment's ledger: 0/1/2/3/8 belong to
// HeLighting/U/HeParams/HeResolve/HeUI and 4 to HeDecal, so this pre-pass takes
// 5 and the two SSR passes below take 6 and 7. The pre-pass block is ALSO named
// `U` — same name as the material vertex block, different program, and the
// index lookup is per program, so nothing aliases as long as it never claims
// point 1.
bool OpenGLRenderer::EnsureReflPrepassProgram()
{
	if (m_reflPrepassProgram) return true;
	if (m_reflPrepassTried) return false;
	m_reflPrepassTried = true;
#if !defined(HE_HAVE_SHADERC)
	return false;
#else
	using Backend = HE::MaterialShaderLibrary::Backend;
	const auto& v = m_matShaderLib.reflPrepassVertex(Backend::GLSL410);
	const auto& f = m_matShaderLib.reflPrepassFragment(Backend::GLSL410);
	if (!(v.ok && f.ok))
	{
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: reflection pre-pass shader compile failed\n")
			 + v.log + f.log).c_str());
		return false;
	}
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   v.source.c_str());
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, f.source.c_str());
	GLuint prog = 0;
	if (vs && fs)
	{
		prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
		if (!linked)
		{
			char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: reflection pre-pass link failed: ") + log).c_str());
			glDeleteProgram(prog);
			prog = 0;
		}
	}
	if (vs) glDeleteShader(vs);
	if (fs) glDeleteShader(fs);
	if (!prog) return false;
	m_reflPrepassProgram = prog;
	if (const GLuint idx = glGetUniformBlockIndex(m_reflPrepassProgram, "U");
	    idx != GL_INVALID_INDEX)
		glUniformBlockBinding(m_reflPrepassProgram, idx, 5);
	glGenBuffers(1, &m_reflPrepassUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_reflPrepassUBO);
	glBufferData(GL_UNIFORM_BUFFER,
		static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::ReflPrepassUniforms)),
		nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	// Instanced twin (library variant: model from attribs 4–7, camera pair in
	// its own block). Optional — without it every batch loops through the plain
	// program above, so a failure here is logged, not returned.
	{
		const auto& iv = m_matShaderLib.reflPrepassVertexInstanced(Backend::GLSL410);
		GLuint ivs = iv.ok ? CompileStage(GL_VERTEX_SHADER,   iv.source.c_str()) : 0;
		GLuint ifs = iv.ok ? CompileStage(GL_FRAGMENT_SHADER, f.source.c_str())  : 0;
		GLuint iprog = 0;
		if (ivs && ifs)
		{
			iprog = glCreateProgram();
			glAttachShader(iprog, ivs);
			glAttachShader(iprog, ifs);
			glLinkProgram(iprog);
			GLint linked = 0; glGetProgramiv(iprog, GL_LINK_STATUS, &linked);
			if (!linked)
			{
				char log[2048]; glGetProgramInfoLog(iprog, sizeof(log), nullptr, log);
				HE_LOG_ERROR(RHI, "%s",
					(std::string("OpenGLRenderer: instanced reflection pre-pass link failed: ") + log).c_str());
				glDeleteProgram(iprog);
				iprog = 0;
			}
		}
		else
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: instanced reflection pre-pass shader compile failed\n")
				 + iv.log).c_str());
		if (ivs) glDeleteShader(ivs);
		if (ifs) glDeleteShader(ifs);
		if (iprog)
		{
			m_reflPrepassInstProgram = iprog;
			if (const GLuint idx = glGetUniformBlockIndex(iprog, "UI"); idx != GL_INVALID_INDEX)
				glUniformBlockBinding(iprog, idx, 5);
			glGenBuffers(1, &m_reflPrepassInstUBO);
			glBindBuffer(GL_UNIFORM_BUFFER, m_reflPrepassInstUBO);
			glBufferData(GL_UNIFORM_BUFFER,
				static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::ReflPrepassInstUniforms)),
				nullptr, GL_DYNAMIC_DRAW);
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
		}
	}
	return true;
#endif
}

void OpenGLRenderer::EnsureSSAOTargets(int width, int height, bool withRefl)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	const bool sized = m_ssaoPosFBO && width == m_ssaoW && height == m_ssaoH;
	// The reflection attachments are added to the SAME FBO the first frame SSR
	// asks for them and then simply stay: they cost two half-screen textures and
	// re-attaching per frame would only churn driver state.
	if (sized && (!withRefl || m_reflAttrTex)) return;
	if (sized)
	{
		// Right size, only the two reflection attachments are missing.
		glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
	}
	else
	{
	DestroySSAOTargets();
	m_ssaoW = width; m_ssaoH = height;

	// Position pre-pass target (RGBA16F view position) + depth (nearest surface).
	// NEAREST so the kernel reprojection reads exact stored positions, never an
	// interpolated value straddling a depth edge.
	glGenFramebuffers(1, &m_ssaoPosFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
	glGenTextures(1, &m_ssaoPosTex);
	glBindTexture(GL_TEXTURE_2D, m_ssaoPosTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssaoPosTex, 0);
	glGenRenderbuffers(1, &m_ssaoPosDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_ssaoPosDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_ssaoPosDepth);

	// Raw + blurred occlusion (single-channel R8, LINEAR for the scene-shader read).
	auto makeR8 = [&](unsigned int& fbo, unsigned int& tex)
	{
		glGenFramebuffers(1, &fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	};
	makeR8(m_ssaoFBO, m_ssaoTex);
	makeR8(m_ssaoBlurFBO, m_ssaoBlurTex);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: SSAO FBO incomplete");
	}

	// ── Reflection MRT attachments (forward SSR, plan A2) ──────────────────
	// Attachment 1 = the GB1 encoding (oct normal in rg, roughness in b),
	// attachment 2 = NDC depth as R32F — exactly what the shared trace would
	// read out of a G-buffer. NEAREST on both for the same reason the deferred
	// path point-samples them: interpolating an oct normal across a geometry
	// edge decodes to garbage, and an interpolated depth is a surface that
	// never existed.
	if (withRefl && !m_reflAttrTex)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
		auto makeTex = [&](unsigned int& tex, unsigned int internalFmt,
		                   unsigned int fmt, int attachment)
		{
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFmt), m_ssaoW, m_ssaoH, 0,
			             fmt, GL_FLOAT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glFramebufferTexture2D(GL_FRAMEBUFFER,
			                       static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + attachment),
			                       GL_TEXTURE_2D, tex, 0);
		};
		makeTex(m_reflAttrTex, GL_RGBA16F, GL_RGBA, 1);
		makeTex(m_reflNdcTex,  GL_R32F,    GL_RED,  2);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: reflection pre-pass FBO incomplete");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::DestroySSAOTargets()
{
	if (m_ssaoPosFBO)   { glDeleteFramebuffers(1, &m_ssaoPosFBO);    m_ssaoPosFBO = 0; }
	if (m_ssaoPosTex)   { glDeleteTextures(1, &m_ssaoPosTex);        m_ssaoPosTex = 0; }
	if (m_ssaoPosDepth) { glDeleteRenderbuffers(1, &m_ssaoPosDepth); m_ssaoPosDepth = 0; }
	if (m_reflAttrTex)  { glDeleteTextures(1, &m_reflAttrTex);       m_reflAttrTex = 0; }
	if (m_reflNdcTex)   { glDeleteTextures(1, &m_reflNdcTex);        m_reflNdcTex = 0; }
	if (m_ssaoFBO)      { glDeleteFramebuffers(1, &m_ssaoFBO);       m_ssaoFBO = 0; }
	if (m_ssaoTex)      { glDeleteTextures(1, &m_ssaoTex);           m_ssaoTex = 0; }
	if (m_ssaoBlurFBO)  { glDeleteFramebuffers(1, &m_ssaoBlurFBO);   m_ssaoBlurFBO = 0; }
	if (m_ssaoBlurTex)  { glDeleteTextures(1, &m_ssaoBlurTex);       m_ssaoBlurTex = 0; }
	m_ssaoW = m_ssaoH = 0;
}

// Pre-pass (view position) → occlusion → blur. Leaves GL_TEXTURE0 active and the
// depth test disabled; the caller re-binds its own target + depth state.
unsigned int OpenGLRenderer::RenderSSAO(const CommandBuffer& cmds, int pw, int ph,
	const glm::mat4& viewProj, const glm::mat4& view, const glm::mat4& proj,
	bool fromGBufferDepth, bool reflMrt, bool aoWanted)
{
	// The MRT pre-pass has its own program, so a missing SSAO program must not
	// take the reflection path down with it — and vice versa.
	if (reflMrt && !EnsureReflPrepassProgram()) reflMrt = false;
	if (!reflMrt && !m_ssaoPosProgram) return 0;
	if (aoWanted && (!m_ssaoProgram || !m_ssaoBlurProgram)) aoWanted = false;
	if (!reflMrt && !aoWanted) return 0;
	// The G-buffer-depth shortcut reconstructs positions only; it cannot produce
	// the normal/depth attachments, so it never combines with the MRT pre-pass
	// (and does not have to — the forward SSR path is the only caller that asks
	// for them, and it does not run in deferred mode).
	if (fromGBufferDepth && (!m_ssaoDepthPosProgram || !m_gbDepthTex)) fromGBufferDepth = false;
	if (reflMrt) fromGBufferDepth = false;
	EnsureSSAOTargets(pw, ph, reflMrt);
	if (!m_ssaoPosFBO) return 0;
	if (reflMrt && !m_reflAttrTex) reflMrt = false;

	// ── 1. View-space position pre-pass ────────────────────────────────────
	if (fromGBufferDepth)
	{
		// Deferred P5: one fullscreen draw reading the G-buffer depth — no
		// extract/cull/sort, no geometry re-rasterization.
		glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
		glViewport(0, 0, pw, ph);
		glDisable(GL_DEPTH_TEST);
		glUseProgram(m_ssaoDepthPosProgram);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_gbDepthTex);
		glUniform1i(m_uDepthPosDepth, 0);
		glUniformMatrix4fv(m_uDepthPosInvProj, 1, GL_FALSE,
		                   glm::value_ptr(glm::inverse(proj)));
		glBindVertexArray(m_fsVAO);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	else
	{
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
	glViewport(0, 0, pw, ph);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	// Draw-buffer state lives on the FBO, so it is set on EVERY entry: an
	// AO-only frame after an SSR frame would otherwise still be pointed at the
	// two reflection attachments and write undefined values into them.
	const GLenum mrt[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	glDrawBuffers(reflMrt ? 3 : 1, mrt);
	// Per-attachment clears, not one glClearColor: attachment 2 is NDC depth and
	// its background value is 1.0 (the far plane). Zero-cleared it would read as
	// geometry sitting on the near plane and the trace would hit it everywhere.
	const float cPos[4]  = { 0.0f, 0.0f, 0.0f, 0.0f }; // a = 0 → background
	const float cAttr[4] = { 0.5f, 0.5f, 0.0f, 0.0f }; // oct(0,0,·) encoded
	const float cNdc[4]  = { 1.0f, 1.0f, 1.0f, 1.0f }; // far plane → trace early-outs
	glClearBufferfv(GL_COLOR, 0, cPos);
	if (reflMrt)
	{
		glClearBufferfv(GL_COLOR, 1, cAttr);
		glClearBufferfv(GL_COLOR, 2, cNdc);
	}
	const float d1 = 1.0f;
	glClearBufferfv(GL_DEPTH, 0, &d1);
	if (reflMrt)
	{
		glUseProgram(m_reflPrepassProgram);
		glBindBufferBase(GL_UNIFORM_BUFFER, 5, m_reflPrepassUBO);
	}
	else
		glUseProgram(m_ssaoPosProgram);
	// One per-draw push, two shapes: the hand-written AO program takes loose
	// uniforms, the shared reflection program the three-mat4 block it shares
	// with Metal's encoder.
	auto pushDraw = [&](const glm::mat4& model)
	{
		if (reflMrt)
		{
			HE::MaterialShaderLibrary::ReflPrepassUniforms u;
			const glm::mat4 mvp = viewProj * model;
			const glm::mat4 mv  = view * model;
			std::memcpy(u.mvp,       glm::value_ptr(mvp),   16 * sizeof(float));
			std::memcpy(u.modelView, glm::value_ptr(mv),    16 * sizeof(float));
			std::memcpy(u.model,     glm::value_ptr(model), 16 * sizeof(float));
			glBindBuffer(GL_UNIFORM_BUFFER, m_reflPrepassUBO);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(u)), &u);
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
		}
		else
		{
			glUniformMatrix4fv(m_uPosMVP,       1, GL_FALSE, glm::value_ptr(viewProj * model));
			glUniformMatrix4fv(m_uPosModelView, 1, GL_FALSE, glm::value_ptr(view * model));
		}
	};
	// The instanced twin of whichever program is active: the hand-written AO
	// program has kSSAOPosInstancedVS, the shared reflection program has the
	// library's instanced vertex variant. 0 = every batch loops (link failed, or
	// the variant is not built).
	const unsigned int plainProgram = reflMrt ? m_reflPrepassProgram : m_ssaoPosProgram;
	const unsigned int instProgram  = m_instanceVBO
		? (reflMrt ? m_reflPrepassInstProgram : m_ssaoPosInstancedProgram) : 0u;
	unsigned int boundProgram = plainProgram;
	HE::UUID lastId{}; const GpuMesh* cMesh = nullptr; bool valid = false;
	for (const DrawCall& dc : cmds.drawCalls())
	{
		if (!dc.contributesAO) continue; // precip/particles: skip the SSAO position prepass
		if (!valid || dc.meshAssetId != lastId)
		{
			cMesh = ResolveMesh(dc.meshAssetId);
			lastId = dc.meshAssetId; valid = true;
		}
		const GpuMesh* mesh = cMesh ? cMesh : ResolveMesh(HE::kDefaultCubeMeshId);
		if (!mesh) continue;
		glBindVertexArray(mesh->vao);
		const GlIndexRange range = DrawIndexRange(dc, mesh->indexCount); // section or whole

		// A GeometryPass batch (instanceTransforms non-empty ⇔ run > 1) is one
		// instanced draw over the scene pass's scratch VBO — every mesh VAO
		// reads it at attribs 4–7 with divisor 1. The batch-constant camera
		// matrices are pushed once, on the first switch to the instanced program.
		if (!dc.instanceTransforms.empty() && instProgram)
		{
			glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
			glBufferData(GL_ARRAY_BUFFER,
			             static_cast<GLsizeiptr>(dc.instanceTransforms.size() * sizeof(glm::mat4)),
			             dc.instanceTransforms.data(), GL_STREAM_DRAW);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			if (boundProgram != instProgram)
			{
				glUseProgram(instProgram);
				boundProgram = instProgram;
				if (reflMrt)
				{
					HE::MaterialShaderLibrary::ReflPrepassInstUniforms u;
					std::memcpy(u.viewProj, glm::value_ptr(viewProj), 16 * sizeof(float));
					std::memcpy(u.view,     glm::value_ptr(view),     16 * sizeof(float));
					glBindBuffer(GL_UNIFORM_BUFFER, m_reflPrepassInstUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(u)), &u);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
					glBindBufferBase(GL_UNIFORM_BUFFER, 5, m_reflPrepassInstUBO);
				}
				else
				{
					glUniformMatrix4fv(m_uPosInstViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
					glUniformMatrix4fv(m_uPosInstView,     1, GL_FALSE, glm::value_ptr(view));
				}
			}
			glDrawElementsInstanced(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset,
			                        static_cast<GLsizei>(dc.instanceTransforms.size()));
			static bool loggedOnce = false; // once per session, like the shadow pass
			if (!loggedOnce)
			{
				loggedOnce = true;
				HE_LOG_INFO(RHI, "OpenGLRenderer: SSAO pre-pass instanced (first batch: %u instances, %s)",
				            static_cast<unsigned>(dc.instanceTransforms.size()),
				            reflMrt ? "MRT" : "plain");
			}
			continue;
		}
		if (boundProgram != plainProgram)
		{
			glUseProgram(plainProgram);
			boundProgram = plainProgram;
			// The two reflection programs share binding 5; hand it back to the
			// per-draw block before the loop writes into it again.
			if (reflMrt) glBindBufferBase(GL_UNIFORM_BUFFER, 5, m_reflPrepassUBO);
		}
		if (!dc.instanceTransforms.empty())
		{
			for (const glm::mat4& t : dc.instanceTransforms)
			{
				pushDraw(t);
				glDrawElements(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset);
			}
		}
		else
		{
			pushDraw(dc.transform);
			glDrawElements(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset);
		}
	}
	// Back to a single draw buffer so nothing downstream inherits the MRT state.
	if (reflMrt) glDrawBuffers(1, mrt);
	}

	// The reflection pre-pass can be the whole job: SSR wants it on frames where
	// SSAO is off or the GI probes replaced it.
	if (!aoWanted)
	{
		glDisable(GL_DEPTH_TEST);
		glActiveTexture(GL_TEXTURE0);
		return 0;
	}

	// ── 2. Occlusion (fullscreen) ──────────────────────────────────────────
	glDisable(GL_DEPTH_TEST);
	glBindVertexArray(m_fsVAO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoFBO);
	glUseProgram(m_ssaoProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ssaoPosTex);
	glUniform1i(m_uSsaoViewPos, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTex);
	glUniform1i(m_uSsaoNoise, 1);
	glUniformMatrix4fv(m_uSsaoProj, 1, GL_FALSE, glm::value_ptr(proj));
	glUniform2f(m_uSsaoNoiseScale, static_cast<float>(pw) / 4.0f, static_cast<float>(ph) / 4.0f);
	glUniform1f(m_uSsaoRadius,    m_ssaoRadius);
	glUniform1f(m_uSsaoBias,      0.025f);
	glUniform1f(m_uSsaoIntensity, m_ssaoIntensity);
	glUniform1i(m_uAOMethod,      m_ssaoMethod);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// ── 3. Box blur (fullscreen) ───────────────────────────────────────────
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFBO);
	glUseProgram(m_ssaoBlurProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ssaoTex);
	glUniform1i(m_uBlurAO, 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glActiveTexture(GL_TEXTURE0);
	return m_ssaoBlurTex;
}

// ─── Forward screen-space reflections (docs/ssr-cross-backend-plan.md A3/A4) ──
// The GL twin of MetalRenderer::EncodeForwardSSR, built from the SAME shared
// trace and blur shaders. Deliberately NOT a second implementation: everything
// that decides how a reflection looks lives in MaterialShaderLibrary, and this
// file only decides which texture goes into which slot.
bool OpenGLRenderer::EnsureSSRPrograms()
{
	if (m_ssrTraceProgram && m_ssrBlurProgram) return true;
	if (m_ssrProgramsTried) return false;
	m_ssrProgramsTried = true;
#if !defined(HE_HAVE_SHADERC)
	return false;
#else
	using Backend = HE::MaterialShaderLibrary::Backend;
	// The attribute-less fullscreen triangle the deferred resolve already uses —
	// the trace and the blur both read gl_FragCoord and take no varyings.
	const auto& v  = m_matShaderLib.fullscreenVertex(Backend::GLSL410);
	const auto& ft = m_matShaderLib.ssrTrace(Backend::GLSL410);
	const auto& fb = m_matShaderLib.ssrBlur(Backend::GLSL410);
	if (!(v.ok && ft.ok && fb.ok))
	{
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: SSR shader compile failed\n")
			 + v.log + ft.log + fb.log).c_str());
		return false;
	}
	auto build = [&](const char* what, const char* fragSrc) -> unsigned int
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   v.source.c_str());
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragSrc);
		GLuint prog = 0;
		if (vs && fs)
		{
			prog = glCreateProgram();
			glAttachShader(prog, vs);
			glAttachShader(prog, fs);
			glLinkProgram(prog);
			GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
			if (!linked)
			{
				char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
				HE_LOG_ERROR(RHI, "%s",
					(std::string("OpenGLRenderer: SSR ") + what + " link failed: " + log).c_str());
				glDeleteProgram(prog);
				prog = 0;
			}
		}
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return prog;
	};
	m_ssrTraceProgram = build("trace", ft.source.c_str());
	m_ssrBlurProgram  = build("blur",  fb.source.c_str());
	if (!m_ssrTraceProgram || !m_ssrBlurProgram)
	{
		if (m_ssrTraceProgram) { glDeleteProgram(m_ssrTraceProgram); m_ssrTraceProgram = 0; }
		if (m_ssrBlurProgram)  { glDeleteProgram(m_ssrBlurProgram);  m_ssrBlurProgram = 0; }
		return false;
	}
	// UBO blocks: HeSSRTrace → binding point 6, HeSSRBlur → 7 (see the ledger on
	// EnsureReflPrepassProgram). Samplers by name, the way every GLSL 410
	// program in this backend does it — 420pack is off in the cross-compiler, so
	// the emitted source carries no binding qualifiers.
	if (const GLuint idx = glGetUniformBlockIndex(m_ssrTraceProgram, "HeSSRTrace");
	    idx != GL_INVALID_INDEX)
		glUniformBlockBinding(m_ssrTraceProgram, idx, 6);
	if (const GLuint idx = glGetUniformBlockIndex(m_ssrBlurProgram, "HeSSRBlur");
	    idx != GL_INVALID_INDEX)
		glUniformBlockBinding(m_ssrBlurProgram, idx, 7);
	glUseProgram(m_ssrTraceProgram);
	auto smp = [&](unsigned int prog, const char* nm, int unit) {
		if (GLint l = glGetUniformLocation(prog, nm); l >= 0) glUniform1i(l, unit);
	};
	smp(m_ssrTraceProgram, "heSceneColor",  0);
	smp(m_ssrTraceProgram, "heGB1",         1);
	smp(m_ssrTraceProgram, "heGBDepth",     2);
	smp(m_ssrTraceProgram, "heSSRHistRad",  3);
	smp(m_ssrTraceProgram, "heSSRHistPos",  4);
	glUseProgram(m_ssrBlurProgram);
	smp(m_ssrBlurProgram,  "heSSRIn",       0);
	glUseProgram(0);

	glGenBuffers(1, &m_ssrTraceUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_ssrTraceUBO);
	glBufferData(GL_UNIFORM_BUFFER,
		static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::SSRTraceUniforms)),
		nullptr, GL_DYNAMIC_DRAW);
	glGenBuffers(1, &m_ssrBlurUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_ssrBlurUBO);
	glBufferData(GL_UNIFORM_BUFFER,
		static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::SSRBlurUniforms)),
		nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	return true;
#endif
}

void OpenGLRenderer::EnsureSSRTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_ssrHistFBO[0] && width == m_ssrW && height == m_ssrH) return;
	DestroySSRTargets();
	m_ssrW = width; m_ssrH = height;
	auto makeTex = [&](unsigned int filter) -> unsigned int
	{
		unsigned int tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return tex;
	};
	for (int i = 0; i < 2; ++i)
	{
		// Radiance: the filter is re-set per frame in RenderForwardSSR — at
		// quality 0 this texture IS what the scene shader upsamples, at the
		// higher tiers it is a point-sampled temporal history.
		m_ssrHistRadTex[i] = makeTex(GL_LINEAR);
		// Positions are never interpolated: a value straddling a depth edge is a
		// surface that does not exist, and the disocclusion test would accept it.
		m_ssrHistPosTex[i] = makeTex(GL_NEAREST);
		glGenFramebuffers(1, &m_ssrHistFBO[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_ssrHistFBO[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssrHistRadTex[i], 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_ssrHistPosTex[i], 0);
	}
	m_ssrPingTex = makeTex(GL_LINEAR);
	glGenFramebuffers(1, &m_ssrPingFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssrPingFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssrPingTex, 0);
	m_ssrReflTex = makeTex(GL_LINEAR);
	glGenFramebuffers(1, &m_ssrReflFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssrReflFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssrReflTex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: SSR FBO incomplete");
	// Fresh (undefined-content) targets — the first frame must not blend against
	// them, the same rule the Metal path states at EnsureSSRTarget.
	m_ssrHistIdx   = 0;
	m_ssrHistValid = false;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::DestroySSRTargets()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_ssrHistFBO[i])    { glDeleteFramebuffers(1, &m_ssrHistFBO[i]);  m_ssrHistFBO[i] = 0; }
		if (m_ssrHistRadTex[i]) { glDeleteTextures(1, &m_ssrHistRadTex[i]);   m_ssrHistRadTex[i] = 0; }
		if (m_ssrHistPosTex[i]) { glDeleteTextures(1, &m_ssrHistPosTex[i]);   m_ssrHistPosTex[i] = 0; }
	}
	if (m_ssrPingFBO) { glDeleteFramebuffers(1, &m_ssrPingFBO); m_ssrPingFBO = 0; }
	if (m_ssrPingTex) { glDeleteTextures(1, &m_ssrPingTex);     m_ssrPingTex = 0; }
	if (m_ssrReflFBO) { glDeleteFramebuffers(1, &m_ssrReflFBO); m_ssrReflFBO = 0; }
	if (m_ssrReflTex) { glDeleteTextures(1, &m_ssrReflTex);     m_ssrReflTex = 0; }
	m_ssrW = m_ssrH = 0;
	m_ssrHistValid = false;
}

// Full-res copy of the finished HDR frame (opaque + sky + transparency), taken
// at the end of the geometry pass. NEXT frame's trace reprojects its hits into
// it — the one frame of content lag is the accepted forward trade (Option A).
void OpenGLRenderer::CaptureSSRColorHistory(int width, int height)
{
	if (!m_hdrColor) return;
	if (!m_ssrColorHistTex || m_ssrColorHistW != width || m_ssrColorHistH != height)
	{
		if (m_ssrColorHistFBO) { glDeleteFramebuffers(1, &m_ssrColorHistFBO); m_ssrColorHistFBO = 0; }
		if (m_ssrColorHistTex) { glDeleteTextures(1, &m_ssrColorHistTex);     m_ssrColorHistTex = 0; }
		m_ssrColorHistValid = false;
		glGenTextures(1, &m_ssrColorHistTex);
		glBindTexture(GL_TEXTURE_2D, m_ssrColorHistTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &m_ssrColorHistFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_ssrColorHistFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		                       m_ssrColorHistTex, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		m_ssrColorHistW = width; m_ssrColorHistH = height;
	}
	if (!m_ssrColorHistFBO) return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_hdrFBO);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ssrColorHistFBO);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
	m_ssrColorHistValid = true;
}

unsigned int OpenGLRenderer::RenderForwardSSR(int pw, int ph, const glm::mat4& viewProj)
{
	if (!m_ssrTraceProgram || !m_ssrBlurProgram) return 0;
	if (!m_reflAttrTex || !m_reflNdcTex)         return 0;
	if (!m_ssrColorHistTex || !m_ssrColorHistValid) return 0; // frame 1 seeds the copy first
	const int tw = std::max(1, pw / 2), th = std::max(1, ph / 2);
	EnsureSSRTargets(tw, th);
	if (!m_ssrHistFBO[0]) return 0;

	const glm::vec3 camFwd =
		-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
	const int curIdx  = m_ssrHistIdx;
	const int prevIdx = 1 - curIdx;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glBindVertexArray(m_fsVAO);
	glViewport(0, 0, tw, th);

	// ── 1. Trace (MRT: blended radiance + receiver position) ───────────────
	{
		const bool temporal = m_ssrQuality >= 2;
		float hist = 0.0f;
		if (temporal && m_ssrHistValid)
		{
			// Camera-motion damping, the same measure Metal takes: a moved view
			// means the un-reprojected reflection CONTENT is stale.
			float delta = 0.0f;
			for (int c = 0; c < 4; ++c)
				for (int r = 0; r < 4; ++r)
					delta = std::max(delta, std::abs(viewProj[c][r] - m_ssrPrevViewProj[c][r]));
			hist = delta > 1e-5f ? 0.55f : 0.85f;
		}
		m_ssrFrameSeed += 1.0f;

		HE::MaterialShaderLibrary::SSRTraceUniforms tu;
		const glm::mat4 ivp = glm::inverse(viewProj);
		std::memcpy(tu.viewProj,     glm::value_ptr(viewProj),          16 * sizeof(float));
		std::memcpy(tu.invViewProj,  glm::value_ptr(ivp),               16 * sizeof(float));
		std::memcpy(tu.prevViewProj, glm::value_ptr(m_ssrPrevViewProj), 16 * sizeof(float));
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
		// GL conventions (docs/ssr-cross-backend-plan.md §1.4, inherited from the
		// decal port): NDC y points UP, and the sampled depth is [0,1] while NDC z
		// is [-1,1] — hence scale 2 / bias -1. Metal reads (-1, 1, 0) here.
		tu.conv[0] =  1.0f;
		tu.conv[1] =  2.0f;
		tu.conv[2] = -1.0f;
		tu.conv[3] =  0.1f;
		tu.vp[0] = static_cast<float>(tw);
		tu.vp[1] = static_cast<float>(th);

		glBindBuffer(GL_UNIFORM_BUFFER, m_ssrTraceUBO);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(tu)), &tu);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, 6, m_ssrTraceUBO);

		// The radiance history is point-sampled from the temporal tier upward
		// (Metal binds its point sampler there); at quality 0 the very same
		// texture is what the scene shader upsamples to full screen, and NEAREST
		// would show as half-res stair-steps in the mirror.
		const GLint histFilter = m_ssrQuality >= 2 ? GL_NEAREST : GL_LINEAR;
		for (int i = 0; i < 2; ++i)
		{
			glBindTexture(GL_TEXTURE_2D, m_ssrHistRadTex[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, histFilter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, histFilter);
		}

		glBindFramebuffer(GL_FRAMEBUFFER, m_ssrHistFBO[curIdx]);
		const GLenum mrt[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, mrt);
		glUseProgram(m_ssrTraceProgram);
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_ssrColorHistTex);
		glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_reflAttrTex);
		glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_reflNdcTex);
		glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_ssrHistRadTex[prevIdx]);
		glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, m_ssrHistPosTex[prevIdx]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		++m_counters.draws;
		glDrawBuffers(1, mrt);

		m_ssrHistIdx      = prevIdx;
		m_ssrHistValid    = true;
		m_ssrPrevViewProj = viewProj;
	}

	unsigned int result = m_ssrHistRadTex[curIdx];

	// ── 2. Blur chain — the same tier policy as Metal's forward variant. No
	// wide/roughness-mix stage: the forward path has none there either (the mix
	// shader serves the GI reflections), and the pre-pass carries no roughness.
	if (m_ssrQuality >= 1)
	{
		glBindBufferBase(GL_UNIFORM_BUFFER, 7, m_ssrBlurUBO);
		glUseProgram(m_ssrBlurProgram);
		auto blurPass = [&](unsigned int src, unsigned int dstFBO, float dx, float dy)
		{
			HE::MaterialShaderLibrary::SSRBlurUniforms bu;
			bu.dir[0] = dx;
			bu.dir[1] = dy;
			bu.dir[2] = 1.0f / static_cast<float>(tw);
			bu.dir[3] = 1.0f / static_cast<float>(th);
			glBindBuffer(GL_UNIFORM_BUFFER, m_ssrBlurUBO);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(bu)), &bu);
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, dstFBO);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, src);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			++m_counters.draws;
		};
		const float sx = 1.0f / static_cast<float>(tw);
		const float sy = 1.0f / static_cast<float>(th);
		blurPass(result,       m_ssrPingFBO, sx,   0.0f);
		blurPass(m_ssrPingTex, m_ssrReflFBO, 0.0f, sy);
		if (m_ssrQuality == 1)
		{
			blurPass(m_ssrReflTex, m_ssrPingFBO, sx,   0.0f);
			blurPass(m_ssrPingTex, m_ssrReflFBO, 0.0f, sy);
		}
		result = m_ssrReflTex;
	}

	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, pw, ph);
	return result;
}

// The R8 atlas texture for a font key (0 = the shared default), uploaded lazily
// from UIFontCache the first time a glyph quad references it.
unsigned int OpenGLRenderer::UIFontAtlasTexture(uint32_t key)
{
	if (key == 0) return m_uiFontTexture ? m_uiFontTexture : m_whiteTex;
	if (auto it = m_uiFontAtlases.find(key); it != m_uiFontAtlases.end()) return it->second;
	const HE::BakedUIFont* f = HE::UIFontCache::find(key);
	if (!f || !f->ok) return m_uiFontTexture ? m_uiFontTexture : m_whiteTex;
	unsigned int tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, f->atlasW, f->atlasH, 0, GL_RED, GL_UNSIGNED_BYTE, f->pixels.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_uiFontAtlases[key] = tex;
	return tex;
}

void OpenGLRenderer::RenderUIPass(int pw, int ph)
{
	if (!m_uiProgram || m_renderWorld.uiObjects.empty()) return;

	// Lazy one-time upload of the shared baked font atlas (R8, glyph alpha in .r).
	if (!m_uiFontTexture)
	{
		const HE::BakedUIFont& uiFont = HE::sharedUIFont();
		if (uiFont.ok)
		{
			glGenTextures(1, &m_uiFontTexture);
			glBindTexture(GL_TEXTURE_2D, m_uiFontTexture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, HE::BakedUIFont::kWidth, HE::BakedUIFont::kHeight,
			             0, GL_RED, GL_UNSIGNED_BYTE, uiFont.pixels.data());
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(m_fsVAO);

	bool         basicBound    = false; // solid/glyph program currently active?
	uint32_t     boundAtlasKey = 0;     // font atlas currently bound on unit 0
	unsigned int boundMaterial = 0;     // material program currently active
	bool uiLightUploaded = false;       // HeLighting uploaded once per UI pass
	// Clipping is a scissor rectangle, set only when it CHANGES — a widget tree
	// emits its quads in tree order, so equally-clipped quads arrive in runs.
	// GL's origin is bottom-left, the UI's is top-left, hence the flip.
	glm::vec4 appliedClip(-1.0f);
	bool scissorOn = false;
	auto applyClip = [&](const glm::vec4& c)
	{
		if (c == appliedClip) return;
		appliedClip = c;
		if (c.z <= 0.0f)
		{
			if (scissorOn) { glDisable(GL_SCISSOR_TEST); scissorOn = false; }
			return;
		}
		if (!scissorOn) { glEnable(GL_SCISSOR_TEST); scissorOn = true; }
		const float x0 = std::clamp(c.x, 0.0f, static_cast<float>(pw));
		const float y0 = std::clamp(c.y, 0.0f, static_cast<float>(ph));
		const float x1 = std::clamp(c.x + c.z, 0.0f, static_cast<float>(pw));
		const float y1 = std::clamp(c.y + c.w, 0.0f, static_cast<float>(ph));
		glScissor(static_cast<GLint>(x0), static_cast<GLint>(ph - y1),
		          static_cast<GLsizei>(std::max(0.0f, x1 - x0)),
		          static_cast<GLsizei>(std::max(0.0f, y1 - y0)));
	};

	// ── The backdrop snapshot (D5 Schicht 1) ─────────────────────────────────
	// "Stale" from the start: the pass has drawn nothing yet, so the first
	// frosted element still needs a picture — of the scene, which is exactly
	// what is under it. Every quad drawn afterwards makes it stale again, so a
	// dialog over a panel blurs the PANEL, not the scene the panel covers.
	bool backdropStale = true;
	auto snapshotBackdrop = [&]()
	{
		if (!m_uiBackdropTex || m_uiBackdropW != pw || m_uiBackdropH != ph)
		{
			if (!m_uiBackdropTex) glGenTextures(1, &m_uiBackdropTex);
			glBindTexture(GL_TEXTURE_2D, m_uiBackdropTex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			// LINEAR_MIPMAP_LINEAR is the whole point: the blur radius picks a
			// level, and a texture without a mip filter would answer every
			// radius with level 0 — nine taps of noise instead of a blur.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			m_uiBackdropW = pw; m_uiBackdropH = ph;
		}
		else
			glBindTexture(GL_TEXTURE_2D, m_uiBackdropTex);
		// Reads the bound framebuffer, and a copy is not a draw: the scissor
		// this pass leaves standing does not clip it.
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		backdropStale = false;
	};

	for (const UIRenderObject& obj : m_renderWorld.uiObjects)
	{
		// Custom material on an image quad → material program (the solid path below
		// stays the fallback when the material has no custom shader / failed).
		bool wantsBackdrop = false;
		const unsigned int matProg = obj.type == 0 && obj.materialAssetId != HE::UUID{}
			? GetOrBuildUIMaterialProgram(obj.materialAssetId, &wantsBackdrop) : 0;
		if (matProg && wantsBackdrop && backdropStale) snapshotBackdrop();
		applyClip(obj.clipRect);
		if (matProg)
		{
			if (boundMaterial != matProg)
			{
				glUseProgram(matProg);
				boundMaterial = matProg; basicBound = false;
			}
			// The uiVertex's repurposed U block (see MaterialShaderLibrary::uiVertex).
			struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } u{};
			u.model[0] = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
			u.model[1] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
			u.model[2] = glm::vec4(static_cast<float>(pw), static_cast<float>(ph), 0.0f, 0.0f);
			// Render rotation for the material path, same row the UI vertex reads.
			u.model[3] = glm::vec4(obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y, 0.0f);
			u.color    = obj.color;
			glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
			// Lighting block (heLit sun/ambient + the Time input) — identical for
			// every UI quad this frame, so upload it once per pass.
			if (!uiLightUploaded)
			{
				HE::MaterialShaderLibrary::Lighting lit{};
				glm::vec3 sd, sc;
				m_renderWorld.dominantDirectionalLight(sd, sc);
				const glm::vec3 am = m_renderWorld.ambient;
				lit.sunDir[0]   = sd.x; lit.sunDir[1] = sd.y; lit.sunDir[2] = sd.z;
				lit.sunDir[3]   = static_cast<float>(SDL_GetTicks()) / 1000.0f;
				lit.sunColor[0] = sc.r; lit.sunColor[1] = sc.g; lit.sunColor[2] = sc.b;
				lit.ambient[0]  = am.r; lit.ambient[1]  = am.g; lit.ambient[2]  = am.b;
				lit.camPos[0]   = m_renderWorld.camera.position.x;
				lit.camPos[1]   = m_renderWorld.camera.position.y;
				lit.camPos[2]   = m_renderWorld.camera.position.z;
				// Full light window for heLitP() — same first-8 order as the built-in
				// PBR shaders. Shared fill (HE::FillMaterialLightWindow); the UI pass
				// has no local shadow atlas, so it passes false.
				HE::FillMaterialLightWindow(m_renderWorld, lit, /*localShadowsActive=*/false);
				HE::FillMaterialWind(GetEnvironment(), lit); // Wind nodes, next to Time
				glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
				glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
				uiLightUploaded = true;
				m_matLightUploadedThisFrame = false; // invalidate the mesh loop's per-frame dedup
			}
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);   // block "U"
			glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO); // block "HeLighting"
			// HeUI: the element under the pixel, per QUAD (D5 Schicht 1).
			// screen.z = 1: the backdrop snapshot is a copy OUT of the
			// framebuffer, whose first row is the bottom of the screen — the
			// shader flips its lookup rather than this backend flipping a texture.
			const glm::vec4 heUI[4] = {
				{ obj.size.x, obj.size.y, obj.position.x, obj.position.y },
				obj.cornerRadius,
				obj.uiState,
				{ static_cast<float>(pw), static_cast<float>(ph), 1.0f, 0.0f },
			};
			glBindBuffer(GL_UNIFORM_BUFFER, m_matUIUBO);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(heUI), heUI);
			glBindBufferBase(GL_UNIFORM_BUFFER, 8, m_matUIUBO);    // block "HeUI"

			// HeParams + graph textures, mirroring the mesh path's bind points.
			if (const MaterialAsset* ma = m_contentManager
				? m_contentManager->getMaterial(obj.materialAssetId) : nullptr)
			{
				float padded[64] = { 0 };
				if (const size_t n = std::min(ma->shaderParamData.size(), size_t(64)))
					std::memcpy(padded, ma->shaderParamData.data(), n * sizeof(float));
				glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
				glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
				m_haveMatParams = false; // invalidate the mesh loop's content-skip
				// Node-graph project textures on units 1..4 (heTexP0..3).
				const size_t nTex = std::min<size_t>(4,
					std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
				for (size_t i = 0; i < nTex; ++i)
				{
					const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
					const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
					glActiveTexture(GL_TEXTURE1 + (GLenum)i);
					glBindTexture(GL_TEXTURE_2D, ResolveGraphTexture(gid, gp));
				}
			}
			glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO); // block "HeParams"
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
			// The backdrop snapshot on unit 5 — white when none was taken (the
			// material asks for it, but nothing was behind it yet).
			glActiveTexture(GL_TEXTURE5);
			glBindTexture(GL_TEXTURE_2D, m_uiBackdropTex ? m_uiBackdropTex : m_whiteTex);
			// Legacy/mesh texture slot 0 (heTex0) must be bound too — white dummy.
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_whiteTex);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			backdropStale = true; // this quad is now part of what is "behind"
			continue;
		}
		if (!basicBound)
		{
			glUseProgram(m_uiProgram);
			glUniform2f(m_uUIViewport, static_cast<float>(pw), static_cast<float>(ph));
			// Font atlas on unit 0 (uFontAtlas); glyphs sample it, solid quads ignore it.
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, UIFontAtlasTexture(0));
			boundAtlasKey = 0;
			basicBound = true; boundMaterial = 0;
		}
		// A glyph quad may use an imported font's atlas — bind it on unit 0.
		if (obj.type == 2 && obj.fontAtlasKey != boundAtlasKey)
		{
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, UIFontAtlasTexture(obj.fontAtlasKey));
			boundAtlasKey = obj.fontAtlasKey;
		}
		// A textured quad borrows the same unit; the next glyph rebinds its
		// atlas because boundAtlasKey is invalidated here.
		bool textured = false;
		if (obj.type == 0 && obj.textureAssetId != HE::UUID{})
		{
			if (const unsigned int t = ResolveGraphTexture(obj.textureAssetId, std::string()))
			{
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, t);
				boundAtlasKey = 0xFFFFFFFFu;   // not a font atlas any more
				textured = true;
			}
		}
		glUniform4f(m_uUIRect,  obj.position.x, obj.position.y, obj.size.x, obj.size.y);
		glUniform4f(m_uUIColor, obj.color.r, obj.color.g, obj.color.b, obj.color.a);
		glUniform4f(m_uUIUVRect, obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y);
		glUniform4f(m_uUIRotation, obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y, 0.0f);
		glUniform1f(m_uUIMode, obj.type == 2 ? 1.0f : (textured ? 2.0f : 0.0f));
		glUniform4f(m_uUICornerRadius, obj.cornerRadius.x, obj.cornerRadius.y,
		            obj.cornerRadius.z, obj.cornerRadius.w);
		glUniform1f(m_uUIBorderWidth, obj.borderWidth);
		glUniform4f(m_uUIBorderColor, obj.borderColor.r, obj.borderColor.g,
		            obj.borderColor.b, obj.borderColor.a);
		glUniform1f(m_uUIGradient, obj.gradient ? 1.0f : 0.0f);
		glUniform4f(m_uUIGradientColor, obj.gradientColor.r, obj.gradientColor.g,
		            obj.gradientColor.b, obj.gradientColor.a);
		glUniform1f(m_uUIGradientAngle, obj.gradientAngleDeg);
		glUniform1f(m_uUIGradientShape, obj.gradientShape == 1 ? 1.0f : 0.0f);
		glUniform1f(m_uUIBlur, obj.blur);
		glUniform1f(m_uUIInnerBlur, obj.innerShadowBlur);
		glUniform4f(m_uUIInnerColor, obj.innerShadowColor.r, obj.innerShadowColor.g,
		            obj.innerShadowColor.b, obj.innerShadowColor.a);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		backdropStale = true; // this quad is now part of what is "behind"
	}

	// Leave the state as it was found: the scissor test is global and nothing
	// after this pass asked for one.
	if (scissorOn) glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
}

void OpenGLRenderer::EnsureHDRTarget(int width, int height)
{
	if (m_hdrFBO && width == m_hdrW && height == m_hdrH)
		return;
	DestroyHDRTarget();

	glGenFramebuffers(1, &m_hdrFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);

	glGenTextures(1, &m_hdrColor);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_hdrColor, 0);

	// Depth as a TEXTURE (was a renderbuffer): the depth-of-field pass samples
	// it for its circle of confusion. Same internal format as m_gbDepthTex so
	// the deferred path's depth blit into it stays legal.
	glGenTextures(1, &m_hdrDepth);
	glBindTexture(GL_TEXTURE_2D, m_hdrDepth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
	             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_hdrDepth, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: HDR FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_hdrW = width;
	m_hdrH = height;
}

void OpenGLRenderer::DestroyHDRTarget()
{
	if (m_hdrFBO)   { glDeleteFramebuffers(1, &m_hdrFBO);   m_hdrFBO = 0; }
	if (m_hdrColor) { glDeleteTextures(1, &m_hdrColor);     m_hdrColor = 0; }
	if (m_hdrDepth) { glDeleteTextures(1, &m_hdrDepth);     m_hdrDepth = 0; }
	m_hdrW = m_hdrH = 0;
}

void OpenGLRenderer::EnsureGBufferTargets(int width, int height)
{
	if (m_gbFBO && width == m_gbW && height == m_gbH)
		return;
	DestroyGBufferTargets();

	glGenFramebuffers(1, &m_gbFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_gbFBO);

	auto makeColor = [&](unsigned int& tex, GLenum internalFmt, int attachment)
	{
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, width, height, 0, GL_RGBA,
		             internalFmt == GL_SRGB8_ALPHA8 ? GL_UNSIGNED_BYTE : GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachment,
		                       GL_TEXTURE_2D, tex, 0);
	};
	// GB0 sRGB (BaseColor is perceptual 8-bit — written with GL_FRAMEBUFFER_SRGB
	// enabled during the G-buffer pass, decoded automatically on sampling), the
	// attribute targets RGBA16F. Matches the Metal G-buffer formats.
	makeColor(m_gbColor0, GL_SRGB8_ALPHA8, 0);
	makeColor(m_gbColor1, GL_RGBA16F,      1);
	makeColor(m_gbColor2, GL_RGBA16F,      2);
	const GLenum bufs[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	glDrawBuffers(3, bufs);

	// Depth as a TEXTURE (the resolve samples it for world-pos reconstruction);
	// same internal format as m_hdrDepth so the depth blit between them is legal.
	glGenTextures(1, &m_gbDepthTex);
	glBindTexture(GL_TEXTURE_2D, m_gbDepthTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
	             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_gbDepthTex, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: G-buffer FBO incomplete");

	// Decal target: GB0 ALONE, no depth attachment. The decal fragment samples
	// m_gbDepthTex, which hangs on m_gbFBO — writing to an FBO that also has the
	// sampled texture attached is a feedback loop (same reason the resolve blits
	// the depth out before reading it).
	glGenFramebuffers(1, &m_gbDecalFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_gbDecalFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbColor0, 0);
	const GLenum decalBufs[1] = { GL_COLOR_ATTACHMENT0 };
	glDrawBuffers(1, decalBufs);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: decal FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_gbW = width;
	m_gbH = height;
}

void OpenGLRenderer::DestroyGBufferTargets()
{
	if (m_gbFBO)      { glDeleteFramebuffers(1, &m_gbFBO); m_gbFBO = 0; }
	if (m_gbDecalFBO) { glDeleteFramebuffers(1, &m_gbDecalFBO); m_gbDecalFBO = 0; }
	if (m_gbColor0)   { glDeleteTextures(1, &m_gbColor0);  m_gbColor0 = 0; }
	if (m_gbColor1)   { glDeleteTextures(1, &m_gbColor1);  m_gbColor1 = 0; }
	if (m_gbColor2)   { glDeleteTextures(1, &m_gbColor2);  m_gbColor2 = 0; }
	if (m_gbDepthTex) { glDeleteTextures(1, &m_gbDepthTex);m_gbDepthTex = 0; }
	m_gbW = m_gbH = 0;
}

bool OpenGLRenderer::EnsureDeferredPipelines()
{
	if (m_gbufferProgram && m_deferredResolveProgram) return true;
	if (m_deferredPipelinesTried) return false; // failed once — stay forward
	m_deferredPipelinesTried = true;

#if !defined(HE_HAVE_SHADERC)
	// The resolve shader is generated from the shared lighting preamble at
	// runtime — without the cross-compiler there is no deferred path.
	return false;
#else
	auto link = [](const char* what, GLuint vs, GLuint fs) -> GLuint
	{
		GLuint prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: ") + what + " link failed: " + log).c_str());
			glDeleteProgram(prog);
			return 0;
		}
		return prog;
	};

	try
	{
		// Built-in-PBR G-buffer programs: the unlit/instanced vertex stages with
		// the MRT attribute fragment.
		GLuint gbFS = CompileStage(GL_FRAGMENT_SHADER, kGBufFS);
		{
			GLuint vs = CompileStage(GL_VERTEX_SHADER, kUnlitVS);
			m_gbufferProgram = link("G-buffer program", vs, gbFS);
			glDeleteShader(vs);
		}
		{
			GLuint vs = CompileStage(GL_VERTEX_SHADER, kInstancedVS);
			m_gbufferInstancedProgram = link("instanced G-buffer program", vs, gbFS);
			glDeleteShader(vs);
		}
		glDeleteShader(gbFS);
		if (m_gbufferProgram)
		{
			glUseProgram(m_gbufferProgram);
			m_uGBMVP        = glGetUniformLocation(m_gbufferProgram, "uMVP");
			m_uGBModel      = glGetUniformLocation(m_gbufferProgram, "uModel");
			m_uGBColor      = glGetUniformLocation(m_gbufferProgram, "uColor");
			m_uGBMetallic   = glGetUniformLocation(m_gbufferProgram, "uMetallic");
			m_uGBRoughness  = glGetUniformLocation(m_gbufferProgram, "uRoughness");
			m_uGBSpecAA     = glGetUniformLocation(m_gbufferProgram, "uSpecAA");
			m_uGBHasTexture = glGetUniformLocation(m_gbufferProgram, "uHasTexture");
			m_uGBTexture    = glGetUniformLocation(m_gbufferProgram, "uTexture");
			glUniform1i(m_uGBTexture, 0);
		}
		if (m_gbufferInstancedProgram)
		{
			glUseProgram(m_gbufferInstancedProgram);
			m_uGBInstViewProj   = glGetUniformLocation(m_gbufferInstancedProgram, "uViewProj");
			m_uGBInstColor      = glGetUniformLocation(m_gbufferInstancedProgram, "uColor");
			m_uGBInstMetallic   = glGetUniformLocation(m_gbufferInstancedProgram, "uMetallic");
			m_uGBInstRoughness  = glGetUniformLocation(m_gbufferInstancedProgram, "uRoughness");
			m_uGBInstHasTexture = glGetUniformLocation(m_gbufferInstancedProgram, "uHasTexture");
			m_uGBInstTexture    = glGetUniformLocation(m_gbufferInstancedProgram, "uTexture");
			glUniform1i(m_uGBInstTexture, 0);
		}
		glUseProgram(0);

		// Fullscreen lighting resolve — cross-compiled from the SAME lighting
		// preamble as every material fragment (one shading source, plan §4.2).
		using Backend = HE::MaterialShaderLibrary::Backend;
		const auto& v = m_matShaderLib.fullscreenVertex(Backend::GLSL410);
		const auto& f = m_matShaderLib.deferredResolve(Backend::GLSL410);
		if (v.ok && f.ok)
		{
			GLuint vs = CompileStage(GL_VERTEX_SHADER,   v.source.c_str());
			GLuint fs = CompileStage(GL_FRAGMENT_SHADER, f.source.c_str());
			m_deferredResolveProgram = link("deferred resolve program", vs, fs);
			glDeleteShader(vs); glDeleteShader(fs);
			if (m_deferredResolveProgram)
			{
				// UBO blocks: HeLighting → 0 (the resolve-only fill buffer is bound
				// there during the resolve draw), HeResolve → 3. Samplers: the
				// G-buffer inputs on units 0..3, everything the preamble declares
				// on the SAME units as material programs (setupProgram convention),
				// so the frame's shared binds cover them.
				const GLuint lIdx = glGetUniformBlockIndex(m_deferredResolveProgram, "HeLighting");
				if (lIdx != GL_INVALID_INDEX) glUniformBlockBinding(m_deferredResolveProgram, lIdx, 0);
				const GLuint rIdx = glGetUniformBlockIndex(m_deferredResolveProgram, "HeResolve");
				if (rIdx != GL_INVALID_INDEX) glUniformBlockBinding(m_deferredResolveProgram, rIdx, 3);
				glUseProgram(m_deferredResolveProgram);
				auto smp = [&](const char* nm, int unit) {
					if (GLint l = glGetUniformLocation(m_deferredResolveProgram, nm); l >= 0)
						glUniform1i(l, unit);
				};
				smp("heGB0", 0); smp("heGB1", 1); smp("heGB2", 2); smp("heGBDepth", 3);
				smp("heGIShadow", 9);  smp("heGILocal", 10);
				smp("heCsm", 11);      smp("heLocalShadow", 12);
				smp("heSkyEnv", 14);   smp("heAO", 15);
				smp("heGIIrradiance", 16); smp("heGIVisibility", 17);
				// The resolve shades through the SAME heLitP, so the forward
				// reflection cascade covers the deferred path too — the pre-pass
				// and trace run before the G-buffer either way.
				smp("heGIReflFwd", 18);
				// heSSRFwd gets its unit too even though the DEFERRED path never
				// lights its gate (A6): left at 0 the sampler would alias heGB0's
				// unit, which is the kind of thing a strict driver refuses.
				smp("heSSRFwd", 20);
				// Cloud-shadow transmittance map — unit 19 (per-frame bind).
				smp("heCloudShadow", 19);
				glUseProgram(0);
			}
		}
		else
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: deferred resolve shader compile failed\n")
				 + v.log + f.log).c_str());
	}
	catch (const std::exception& e)
	{
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: deferred pipeline build failed: ") + e.what()).c_str());
	}

	// UBOs for the resolve draw: HeResolve (mat4 + vec4) and the resolve-only
	// HeLighting fill (the shared m_matLightUBO must keep the material fill —
	// custom-material programs alias heCsm onto the local-atlas unit, so THEY
	// must keep csmSplits.w = 0 while the resolve gets real CSM matrices).
	if (m_deferredResolveProgram && !m_resolveUBO)
	{
		glGenBuffers(1, &m_resolveUBO);
		glBindBuffer(GL_UNIFORM_BUFFER, m_resolveUBO);
		glBufferData(GL_UNIFORM_BUFFER,
			static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::ResolveUniforms)),
			nullptr, GL_DYNAMIC_DRAW);
		glGenBuffers(1, &m_resolveLightUBO);
		glBindBuffer(GL_UNIFORM_BUFFER, m_resolveLightUBO);
		glBufferData(GL_UNIFORM_BUFFER,
			static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::Lighting)),
			nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	const bool ok = m_gbufferProgram && m_gbufferInstancedProgram && m_deferredResolveProgram;
	if (!ok)
		HE_LOG_WARN(RHI, "%s",
			"OpenGLRenderer: deferred render path unavailable — staying on forward");
	return ok;
#endif
}

// ─── Deferred decals (docs/decals-cross-backend-plan.md checkpoint A) ─────────
// The same two stages the Metal path uses, only with the SAMPLED depth variant:
// GL has no framebuffer fetch, so heGBDepth is a sampler2D on m_gbDepthTex.
// Built lazily on the first frame that actually has a decal; a build failure
// logs once and the frame simply draws no decals.
bool OpenGLRenderer::EnsureDecalProgram()
{
	if (m_decalProgram) return true;
	if (m_decalProgramTried) return false;
	m_decalProgramTried = true;
#if !defined(HE_HAVE_SHADERC)
	return false;
#else
	using Backend = HE::MaterialShaderLibrary::Backend;
	const auto& v = m_matShaderLib.decalVertex(Backend::GLSL410);
	const auto& f = m_matShaderLib.decalFragmentSampled(Backend::GLSL410);
	if (!(v.ok && f.ok))
	{
		HE_LOG_ERROR(RHI, "%s",
			(std::string("OpenGLRenderer: decal shader compile failed\n") + v.log + f.log).c_str());
		return false;
	}
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   v.source.c_str());
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, f.source.c_str());
	GLuint prog = 0;
	if (vs && fs)
	{
		prog = glCreateProgram();
		glAttachShader(prog, vs);
		glAttachShader(prog, fs);
		glLinkProgram(prog);
		GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
		if (!linked)
		{
			char log[2048]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
			HE_LOG_ERROR(RHI, "%s",
				(std::string("OpenGLRenderer: decal program link failed: ") + log).c_str());
			glDeleteProgram(prog);
			prog = 0;
		}
	}
	if (vs) glDeleteShader(vs);
	if (fs) glDeleteShader(fs);
	if (!prog) return false;
	m_decalProgram = prog;

	// HeDecal → UBO binding point 4. 0/1/2/3/8 are taken (HeLighting, U,
	// HeParams, HeResolve, HeUI), so the decal draw does not disturb any of the
	// per-frame binds the material and resolve programs rely on.
	if (const GLuint dIdx = glGetUniformBlockIndex(m_decalProgram, "HeDecal");
	    dIdx != GL_INVALID_INDEX)
		glUniformBlockBinding(m_decalProgram, dIdx, 4);
	glUseProgram(m_decalProgram);
	// heDecalTex on unit 0 (free while the decal program is the active one),
	// heGBDepth on unit 3 — the same unit the resolve reads m_gbDepthTex from.
	if (GLint l = glGetUniformLocation(m_decalProgram, "heDecalTex"); l >= 0) glUniform1i(l, 0);
	if (GLint l = glGetUniformLocation(m_decalProgram, "heGBDepth");  l >= 0) glUniform1i(l, 3);
	glUseProgram(0);

	glGenBuffers(1, &m_decalUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_decalUBO);
	glBufferData(GL_UNIFORM_BUFFER,
		static_cast<GLsizeiptr>(sizeof(HE::MaterialShaderLibrary::DecalUniforms)),
		nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	return true;
#endif
}

// ─── Block-compressed texture support ─────────────────────────────────────────
// glad only exposes the BPTC enum; S3TC comes from an EXT so define it locally
// (the sRGB twin is from EXT_texture_sRGB, same extension family).
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#  define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif

namespace {

// Whether the current GL context exposes a named extension (core-profile safe:
// GL_EXTENSIONS-as-a-string was removed, so enumerate via glGetStringi). Cached.
bool glHasExtension(const char* name)
{
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; ++i)
	{
		const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
		if (e && std::strcmp(reinterpret_cast<const char*>(e), name) == 0) return true;
	}
	return false;
}

// BC7/BPTC: core in GL 4.2, else the ARB extension. macOS GL is 4.1 → false
// (which is why Apple GL targets ship BC3, not BC7 — see EditorUI selection).
bool glSupportsBptc()
{
	static const bool has = [] {
		GLint major = 0, minor = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &major);
		glGetIntegerv(GL_MINOR_VERSION, &minor);
		if (major > 4 || (major == 4 && minor >= 2)) return true;
		return glHasExtension("GL_ARB_texture_compression_bptc");
	}();
	return has;
}

// BC3/S3TC: the DXT5 EXT — present on effectively every desktop GL incl. macOS 4.1.
bool glSupportsS3tc()
{
	static const bool has = glHasExtension("GL_EXT_texture_compression_s3tc")
	                     || glHasExtension("GL_ANGLE_texture_compression_dxt5");
	return has;
}

// Upload a TextureAsset (RGBA8 or a cooked block format) to a fresh GL texture and
// return its id (0 if unusable / the GPU can't sample the shipped format). Cooked
// textures ship a pre-baked mip chain — for block formats there is no runtime mip
// generation (impossible on compressed data); the cook baked every level. Shared by
// every base-color upload site (static/skeletal mesh, material override, graph tex).
//
// TextureAsset::srgb picks the sRGB internalformat: the importer marks colour
// textures (base colour, emissive) sRGB and data textures (normal, ORM, masks)
// linear, and the hardware decodes the former to linear on sample — so the
// shading works in linear light and the tonemap's gamma encode at the end is
// the only transfer curve applied. Linear-flagged textures upload unchanged,
// which is also what every pre-flag asset does (the loader defaults srgb=false).
unsigned int uploadTextureAssetGL(const TextureAsset* tex)
{
	if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
		return 0;
	const uint32_t mips = tex->mipLevels > 0 ? tex->mipLevels : 1;
	const bool srgb = tex->srgb;

	// Resolve the block format's GL internalformat, or bail (→ flat) when this GL
	// context can't sample it. ASTC is Metal-only and never shipped to GL.
	GLenum blockFmt = 0;
	switch (tex->format)
	{
	case TextureFormat::RGBA8: break;
	case TextureFormat::BC7:
		if (!glSupportsBptc()) return 0;
		blockFmt = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM; break;
	case TextureFormat::BC3:
		if (!glSupportsS3tc()) return 0;
		blockFmt = srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
	default: return 0; // ASTC_4x4 / unknown → GL can't sample it
	}
	const GLenum rgbaFmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;

	unsigned int id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);

	if (blockFmt != 0)
	{
		// Pre-baked, pre-compressed mip chain (16 B / 4x4 block, level 0 first).
		size_t off = 0; uint32_t lw = static_cast<uint32_t>(tex->width), lh = static_cast<uint32_t>(tex->height);
		for (uint32_t l = 0; l < mips; ++l)
		{
			const size_t bytes = static_cast<size_t>((lw + 3) / 4) * ((lh + 3) / 4) * 16;
			if (off + bytes > tex->data.size()) break; // truncated payload guard
			glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), blockFmt,
			                       static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
			                       static_cast<GLsizei>(bytes), tex->data.data() + off);
			off += bytes; lw = std::max<uint32_t>(1, lw >> 1); lh = std::max<uint32_t>(1, lh >> 1);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mips - 1));
	}
	else if (mips > 1)
	{
		// Cooked RGBA8: pre-baked mip chain, no runtime glGenerateMipmap.
		size_t off = 0; uint32_t lw = static_cast<uint32_t>(tex->width), lh = static_cast<uint32_t>(tex->height);
		for (uint32_t l = 0; l < mips; ++l)
		{
			glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), static_cast<GLint>(rgbaFmt),
			             static_cast<GLsizei>(lw), static_cast<GLsizei>(lh),
			             0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data() + off);
			off += static_cast<size_t>(lw) * lh * 4; lw = std::max<uint32_t>(1, lw >> 1); lh = std::max<uint32_t>(1, lh >> 1);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mips - 1));
	}
	else
	{
		// Loose/editor RGBA8: single level + GPU-generated mips (for an sRGB
		// internalformat GL filters the chain in linear space, per spec).
		glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(rgbaFmt),
		             static_cast<GLsizei>(tex->width), static_cast<GLsizei>(tex->height),
		             0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	return id;
}

} // namespace

// ─── Asset mesh upload ────────────────────────────────────────────────────────
const OpenGLRenderer::GpuMesh* OpenGLRenderer::ResolveMesh(const HE::UUID& assetId)
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
	// interleaved here on first draw (the same 8-float layout, zero-filling
	// missing normals/uvs so every mesh fits the unlit layout).
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

	glGenVertexArrays(1, &mesh.vao);
	glGenBuffers(1, &mesh.vbo);
	glGenBuffers(1, &mesh.ebo);

	glBindVertexArray(mesh.vao);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
	             interleaved.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(asset->indices.size() * sizeof(uint32_t)),
	             asset->indices.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));

	// Per-instance transform (mat4 = 4 × vec4) at attrib locs 4–7.
	// The VAO remembers the binding so future instanced draws just bind the VAO.
	if (m_instanceVBO)
	{
		glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
		for (int col = 0; col < 4; ++col)
		{
			const GLuint attrLoc = static_cast<GLuint>(4 + col);
			glEnableVertexAttribArray(attrLoc);
			glVertexAttribPointer(attrLoc, 4, GL_FLOAT, GL_FALSE,
			                      sizeof(glm::mat4),
			                      (void*)(static_cast<GLintptr>(col) * sizeof(glm::vec4)));
			glVertexAttribDivisor(attrLoc, 1);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
	glBindVertexArray(0);

	// Base color texture via the mesh's material — baked UUID (packed builds)
	// with the editor path as fallback (loose content).
	if (const MaterialAsset* mat =
	        m_contentManager->resolveMaterialRef(asset->materialId, asset->materialPath))
	{
		const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
		const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
		// Handles RGBA8 + cooked BC7/BC3 (skips a block format this GL can't sample).
		mesh.texture = uploadTextureAssetGL(m_contentManager->resolveTextureRef(texId0, texPath0));
	}

	HE_LOG_INFO(RHI, "%s",
		("OpenGLRenderer: uploaded mesh '" + asset->name + "' ("
		 + std::to_string(vertexCount) + " verts"
		 + (mesh.texture ? ", textured" : "") + ")").c_str());

	return &m_meshCache.emplace(assetId, mesh).first->second;
}

// ─── Skeletal mesh upload ─────────────────────────────────────────────────────
const OpenGLRenderer::GpuSkeletalMesh*
OpenGLRenderer::ResolveSkeletalMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_skeletalMeshCache.find(assetId); it != m_skeletalMeshCache.end())
		return &it->second;

	const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(assetId);
	if (!asset || asset->vertices.empty() || asset->indices.empty())
		return nullptr;

	const size_t vertexCount = asset->vertices.size() / 3;

	// Interleaved pos + norm + uv  (attrib locs 0/1/2)
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

	// Bone IDs per vertex  (4 × uint32, attrib loc 3) — zero-padded if missing
	std::vector<uint32_t> boneIds(vertexCount * 4, 0u);
	if (!asset->boneIDs.empty())
		std::copy_n(asset->boneIDs.begin(),
		            std::min(asset->boneIDs.size(), vertexCount * 4),
		            boneIds.begin());

	// Bone weights per vertex  (4 × float, attrib loc 4) — default 100% joint 0
	std::vector<float> boneWgts(vertexCount * 4, 0.0f);
	for (size_t v = 0; v < vertexCount; ++v) boneWgts[v * 4] = 1.0f;
	if (!asset->boneWeights.empty())
		std::copy_n(asset->boneWeights.begin(),
		            std::min(asset->boneWeights.size(), vertexCount * 4),
		            boneWgts.begin());

	GpuSkeletalMesh mesh;
	mesh.indexCount  = static_cast<int>(asset->indices.size());
	mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);

	glGenVertexArrays(1, &mesh.vao);
	glGenBuffers(1, &mesh.vbo);
	glGenBuffers(1, &mesh.boneIdVbo);
	glGenBuffers(1, &mesh.boneWgtVbo);
	glGenBuffers(1, &mesh.ebo);

	glBindVertexArray(mesh.vao);

	// Base geometry (locs 0/1/2)
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
	             interleaved.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));

	// Bone IDs (loc 3) — integer attrib so they arrive as uvec4 in the shader
	glBindBuffer(GL_ARRAY_BUFFER, mesh.boneIdVbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(boneIds.size() * sizeof(uint32_t)),
	             boneIds.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(3);
	glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, 4 * sizeof(uint32_t), (void*)0);

	// Bone weights (loc 4)
	glBindBuffer(GL_ARRAY_BUFFER, mesh.boneWgtVbo);
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(boneWgts.size() * sizeof(float)),
	             boneWgts.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(asset->indices.size() * sizeof(uint32_t)),
	             asset->indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0);

	// Base color texture — baked UUID (packed) with editor-path fallback (loose).
	if (const MaterialAsset* mat =
	        m_contentManager->resolveMaterialRef(asset->materialId, asset->materialPath))
	{
		const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
		const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
		// Handles RGBA8 + cooked BC7/BC3 (skips a block format this GL can't sample).
		mesh.texture = uploadTextureAssetGL(m_contentManager->resolveTextureRef(texId0, texPath0));
	}

	HE_LOG_INFO(RHI, "%s",
		("OpenGLRenderer: uploaded skeletal mesh '" + asset->name + "' ("
		 + std::to_string(vertexCount) + " verts, "
		 + std::to_string(asset->skeleton.size()) + " joints)").c_str());

	return &m_skeletalMeshCache.emplace(assetId, mesh).first->second;
}

// ─── Material override texture ──────────────────────────────────────────────
bool OpenGLRenderer::ResolveMaterialTexture(const HE::UUID& materialId, unsigned int& outTex)
{
	outTex = 0;
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

	unsigned int tex = 0;
	{
		const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
		const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
		// RGBA8 + cooked BC7/BC3 (previously this override path assumed RGBA8 and
		// would have uploaded a compressed payload as garbage — now handled).
		tex = uploadTextureAssetGL(m_contentManager->resolveTextureRef(texId0, texPath0));
	}

	m_materialTexCache.emplace(materialId, tex);
	outTex = tex;
	return true;
}

// Resolve a node-graph project texture (UUID for packed assets, path for loose editor
// assets) to a GL texture, cached by a stable key. 0 if not loadable.
unsigned int OpenGLRenderer::ResolveGraphTexture(const HE::UUID& id, const std::string& path)
{
	const std::string key = id != HE::UUID{}
		? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path;
	if (key.empty() || !m_contentManager) return 0;
	if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end()) return it->second;
	// RGBA8 + cooked BC7/BC3 (skips a block format this GL context can't sample).
	unsigned int tex = uploadTextureAssetGL(m_contentManager->resolveTextureRef(id, path));
	m_graphTexCache.emplace(key, tex);
	return tex;
}

bool OpenGLRenderer::ResolveMaterialParams(const HE::UUID& materialId,
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

void OpenGLRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
	// Defer the actual glDelete to DrawScene, where the GL context is current.
	if (materialId != HE::UUID{})
		m_pendingMaterialInvalidations.push_back(materialId);
}

void OpenGLRenderer::InvalidateMesh(const HE::UUID& meshId)
{
	// Defer glDelete* to DrawScene where the GL context is guaranteed current.
	if (meshId != HE::UUID{})
		m_pendingMeshInvalidations.push_back(meshId);
}

void OpenGLRenderer::InvalidateTexture(const HE::UUID& textureId)
{
	// Same deferral — the graph-texture cache is keyed by "hi:lo" for UUIDs.
	if (textureId != HE::UUID{})
		m_pendingTexInvalidations.push_back(textureId);
}

void OpenGLRenderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
	// Build each custom-shader material's GL program NOW so the first draw doesn't
	// stall on compile+link. Caller guarantees the GL context is current (called
	// on the render thread after a scene load). Cache hits are cheap; built-in-PBR
	// materials resolve no shader and are skipped.
	int built = 0;
	for (const HE::UUID& id : materialIds)
	{
		uint64_t shKey; std::string shFrag;
		std::string shVert;
		if (!resolveMaterialShader(id, shKey, shFrag, shVert)) continue;
		if (m_materialPrograms.count(shKey)) continue; // already warm
		const MaterialShaderVariant* pre = nullptr;
		if (const MaterialAsset* ma = m_contentManager ? m_contentManager->getMaterial(id) : nullptr)
			for (const auto& var : ma->precompiledShaders)
				if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
		if (GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre)) ++built;
		// Deferred path active → also warm the G-buffer variant so the first
		// deferred frame doesn't hitch on its cross-compile.
		if (m_renderPath == HE::RenderPath::Deferred)
		{
			uint64_t gbKey; std::string gbFrag, gbVert;
			if (resolveMaterialShaderGB(id, gbKey, gbFrag, gbVert)
			    && !m_materialPrograms.count(gbKey)
			    && GetOrBuildMaterialProgram(gbKey, gbFrag, gbVert, nullptr))
				++built;
		}
	}
	if (built > 0)
		HE_LOG_INFO(RHI, "%s",
			("OpenGLRenderer: warmed up " + std::to_string(built) + " material program(s)").c_str());
}

// Draw one material-graph preview primitive into whatever target is bound. Split
// out of RenderMaterialPreview so the interactive preview and the Content-Browser
// thumbnail can share the exact same shading while rendering into DIFFERENT
// targets — a thumbnail that reused m_previewFBO would silently replace whatever
// the Material Editor is showing. The caller owns FBO/viewport/clear and the
// depth/blend/cull state; this only touches program, UBOs, textures and the VAO.
bool OpenGLRenderer::DrawMaterialPreviewGeometry(const HE::UUID& materialId, float yaw, float pitch,
                                                 float dist, int shape, const HE::UUID& meshId)
{
	// Resolve the material's node-graph program (built-in-PBR materials have none →
	// nothing to draw). Reuses the same program cache + precompiled-variant path.
	uint64_t shKey; std::string shFrag, shVert;
	if (!resolveMaterialShader(materialId, shKey, shFrag, shVert)) return false;
	const MaterialShaderVariant* pre = nullptr;
	const MaterialAsset* ma = m_contentManager ? m_contentManager->getMaterial(materialId) : nullptr;
	if (ma)
		for (const auto& var : ma->precompiledShaders)
			if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
	const unsigned int prog = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
	if (!prog) return false;

	// ── Geometry: a picked STATIC MESH, else the procedural primitive. A GpuMesh's
	// VAO carries the same pos3/normal3/uv2 attributes at locations 0/1/2 that the
	// material program reads in the scene, so it binds here unchanged — only the
	// camera has to grow to the mesh's bounds instead of the unit sphere's.
	const GpuMesh* gm = meshId != HE::UUID{} ? ResolveMesh(meshId) : nullptr;
	// The GL mirror of the Metal preview's re-take: ResolveMesh loads the picked
	// mesh's baked material, the material pool is a dense vector, and `ma` is read
	// for colour and params below.
	if (ma) ma = m_contentManager->getMaterial(materialId);
	unsigned int drawVAO = 0;
	int          drawIdxCount = 0;
	glm::vec3    center(0.0f);
	float        radius = 1.0f; // what `dist` is measured in, so it frames alike
	if (gm && gm->vao && gm->indexCount > 0)
	{
		drawVAO = gm->vao; drawIdxCount = gm->indexCount;
		if (gm->localBounds.isValid())
		{
			center = (gm->localBounds.min + gm->localBounds.max) * 0.5f;
			radius = std::max(glm::length(gm->localBounds.max - gm->localBounds.min) * 0.5f, 1e-4f);
		}
	}
	// Lazy preview primitive (interleaved pos3/normal3/uv2 — the material vertex
	// layout), rebuilt when the requested shape changes. Geometry is shared with the
	// Metal path via buildPreviewMesh so the two backends can never drift apart.
	if (!drawVAO && (!m_previewVAO || m_previewShape != shape))
	{
		std::vector<float> verts; std::vector<uint32_t> idx;
		HE::buildPreviewMesh(shape, verts, idx);
		m_previewIdxCount = (int)idx.size();
		m_previewShape    = shape;
		if (!m_previewVAO) glGenVertexArrays(1, &m_previewVAO);
		glBindVertexArray(m_previewVAO);
		if (!m_previewVBO) glGenBuffers(1, &m_previewVBO);
		glBindBuffer(GL_ARRAY_BUFFER, m_previewVBO);
		glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
		if (!m_previewIBO) glGenBuffers(1, &m_previewIBO);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_previewIBO);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
		glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
		glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
		glBindVertexArray(0);
	}
	if (!drawVAO) { drawVAO = m_previewVAO; drawIdxCount = m_previewIdxCount; }
	if (!drawVAO || drawIdxCount <= 0) return false;

	glUseProgram(prog);
	// Orbit camera around the subject (yaw/pitch/dist from the editor), which is the
	// unit sphere at the origin for a primitive and the mesh's bounds centre for a
	// picked mesh.
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center
		+ glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * (dist * radius);
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	// Clip planes scale with the subject: a 400 m mesh must not sit behind the unit
	// sphere's fixed 50 m far plane, and a 5 cm one must not clip on near.
	const glm::mat4 proj = glm::perspective(glm::radians(32.0f), 1.0f,
		std::max(0.001f, 0.02f * radius), (dist + 8.0f) * radius + 1.0f);
	const glm::mat4 model(1.0f);
	struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } obj;
	obj.mvp = proj * view * model; obj.model = model;
	obj.color = glm::vec4(ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2]) : glm::vec3(1.0f), 1.0f);
	obj.flags = glm::vec4(0.0f);
	obj.pbr   = glm::vec4(ma ? ma->metallic : 0.0f, ma ? ma->roughness : 0.5f, ma ? ma->opacity : 1.0f, 0.0f);
	glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj);
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);

	HE::MaterialShaderLibrary::Lighting lit{};
	const glm::vec3 sd = glm::normalize(glm::vec3(0.45f, 0.75f, 0.55f));
	lit.sunDir[0] = sd.x; lit.sunDir[1] = sd.y; lit.sunDir[2] = sd.z; lit.sunDir[3] = 0.0f;
	lit.sunColor[0] = lit.sunColor[1] = lit.sunColor[2] = 1.05f;
	lit.ambient[0] = lit.ambient[1] = lit.ambient[2] = 0.28f;
	lit.camPos[0] = camPos.x; lit.camPos[1] = camPos.y; lit.camPos[2] = camPos.z;
	// Studio sun as the single array light so heLitP() previews shade correctly.
	lit.lightPos[0][3]   = 0.0f; // directional
	lit.lightDir[0][0]   = -sd.x; lit.lightDir[0][1] = -sd.y; lit.lightDir[0][2] = -sd.z;
	lit.lightColor[0][0] = lit.lightColor[0][1] = lit.lightColor[0][2] = 1.05f;
	lit.lightColor[0][3] = 1.0f;
	lit.counts[0]        = 1.0f;
	glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO);
	m_matLightUploadedThisFrame = false; // invalidate the main loop's per-frame dedup

	if (ma && !ma->shaderParamData.empty())
	{
		float padded[64] = { 0 };
		std::memcpy(padded, ma->shaderParamData.data(),
		            std::min(ma->shaderParamData.size(), size_t(64)) * sizeof(float));
		glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
		m_haveMatParams = false; // invalidate the main loop's content-skip
	}
	glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	// Node-graph project textures at units 1..4 (heTexP0..3).
	if (ma)
	{
		const size_t nTex = std::min<size_t>(4, std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
		for (size_t i = 0; i < nTex; ++i)
		{
			const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
			const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
			glActiveTexture(GL_TEXTURE1 + (GLenum)i);
			glBindTexture(GL_TEXTURE_2D, ResolveGraphTexture(gid, gp));
		}
	}

	glBindVertexArray(drawVAO);
	glDrawElements(GL_TRIANGLES, drawIdxCount, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE0);
	return true;
}

// Compile + link the small unskinned preview program once and cache its uniform
// locations. Shared by the mesh/material previews and the world preview, so
// there is exactly one place that knows this program's uniform names.
bool OpenGLRenderer::EnsureMeshPreviewProgram()
{
	if (m_meshPreviewProgram) return true;
	GLuint vs = CompileStage(GL_VERTEX_SHADER,   kMeshPreviewVS);
	GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kMeshPreviewFS);
	m_meshPreviewProgram = glCreateProgram();
	glAttachShader(m_meshPreviewProgram, vs);
	glAttachShader(m_meshPreviewProgram, fs);
	glLinkProgram(m_meshPreviewProgram);
	glDeleteShader(vs); glDeleteShader(fs);
	m_uMeshPvMVP    = glGetUniformLocation(m_meshPreviewProgram, "uMVP");
	m_uMeshPvModel  = glGetUniformLocation(m_meshPreviewProgram, "uModel");
	m_uMeshPvColor  = glGetUniformLocation(m_meshPreviewProgram, "uColor");
	m_uMeshPvHasTex = glGetUniformLocation(m_meshPreviewProgram, "uHasTex");
	m_uMeshPvCamPos = glGetUniformLocation(m_meshPreviewProgram, "uCamPos");
	m_uMeshPvPbr    = glGetUniformLocation(m_meshPreviewProgram, "uPbr");
	m_uMeshPvSun      = glGetUniformLocation(m_meshPreviewProgram, "uSun");
	m_uMeshPvSunColor = glGetUniformLocation(m_meshPreviewProgram, "uSunColor");
	m_uMeshPvAmbient  = glGetUniformLocation(m_meshPreviewProgram, "uAmbient");
	return m_meshPreviewProgram != 0;
}

// The non-graph counterpart: any pos/normal/uv VAO, shaded by the small
// kMeshPreview* program. Used for mesh thumbnails and for materials whose only
// description is their PBR scalars. Same caller contract as above.
void OpenGLRenderer::DrawMeshPreviewGeometry(unsigned int vao, int indexCount, unsigned int texture,
                                             const glm::vec3& center, float extent,
                                             const glm::vec3& baseColor, float metallic,
                                             float roughness, float yaw, float pitch, float dist)
{
	if (!vao || indexCount <= 0) return;
	if (!EnsureMeshPreviewProgram()) return;

	// Orbit camera auto-framed on the caller's bounds — meshes vary wildly in size
	// and pivot, so `dist` scales the extent rather than being an absolute distance.
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.05f);
	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 model(1.0f);
	const glm::mat4 mvp = proj * view * model;

	glUseProgram(m_meshPreviewProgram);
	glUniformMatrix4fv(m_uMeshPvMVP,   1, GL_FALSE, glm::value_ptr(mvp));
	glUniformMatrix4fv(m_uMeshPvModel, 1, GL_FALSE, glm::value_ptr(model));
	glUniform3fv(m_uMeshPvColor,  1, glm::value_ptr(baseColor));
	glUniform3fv(m_uMeshPvCamPos, 1, glm::value_ptr(camPos));
	glUniform2f(m_uMeshPvPbr, metallic, roughness);
	glUniform1i(m_uMeshPvHasTex, texture != 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindVertexArray(vao);
	glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);
}

// Lazily (re)create the shared thumbnail target at `S`×`S`. Its own FBO, see the
// header note: a thumbnail rendered into one of the interactive preview targets
// would replace whatever editor panel is currently showing that preview.
bool OpenGLRenderer::EnsureThumbnailTarget(int S)
{
	if (!m_thumbFBO || m_thumbSize != S)
	{
		if (m_thumbColor) glDeleteTextures(1, &m_thumbColor);
		if (m_thumbDepth) glDeleteRenderbuffers(1, &m_thumbDepth);
		if (!m_thumbFBO) glGenFramebuffers(1, &m_thumbFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_thumbFBO);
		glGenTextures(1, &m_thumbColor);
		glBindTexture(GL_TEXTURE_2D, m_thumbColor);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_thumbColor, 0);
		glGenRenderbuffers(1, &m_thumbDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_thumbDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, S, S);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_thumbDepth);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		m_thumbSize = S;
	}
	return m_thumbFBO != 0;
}

// Read the thumbnail target back as tightly packed, TOP-DOWN RGBA8. GL reads
// bottom-up; the caller's contract (and every image format it will be written
// to) is top-down, so flip row by row on the way out.
void OpenGLRenderer::ReadThumbnailTarget(int S, std::vector<uint8_t>& outRgba8)
{
	outRgba8.assign(static_cast<size_t>(S) * S * 4, 0);
	std::vector<uint8_t> raw(static_cast<size_t>(S) * S * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
	const size_t rowBytes = static_cast<size_t>(S) * 4;
	for (int y = 0; y < S; ++y)
		std::memcpy(outRgba8.data() + static_cast<size_t>(y) * rowBytes,
		            raw.data() + static_cast<size_t>(S - 1 - y) * rowBytes, rowBytes);
}

bool OpenGLRenderer::RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind,
                                          const HE::UUID& assetId, uint32_t size,
                                          std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	if (!m_contentManager) m_contentManager = &cm;
	if (assetId == HE::UUID{}) return false;
	if (!EnsureThumbnailTarget(S)) return false;

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_thumbFBO);
	glViewport(0, 0, S, S);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent — the grid composites over its own tile
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
	glDisable(GL_BLEND); glDisable(GL_CULL_FACE);

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
	bool drew = false;
	if (kind == ThumbnailKind::Material)
	{
		// Graph material → the real shader; otherwise the built-in PBR scalars on
		// the same sphere, so EVERY material asset produces a tile.
		drew = DrawMaterialPreviewGeometry(assetId, kYaw, kPitch, kMatGraphDist, 0 /*sphere*/);
		if (!drew)
		{
			const MaterialAsset* ma = m_contentManager->getMaterial(assetId);
			if (!m_previewVAO || m_previewShape != 0)
			{
				// Reuse the material preview's sphere VAO — building it here keeps the
				// fallback independent of whether an interactive preview ever ran.
				std::vector<float> verts; std::vector<uint32_t> idx;
				HE::buildPreviewMesh(0, verts, idx);
				m_previewIdxCount = (int)idx.size();
				m_previewShape    = 0;
				if (!m_previewVAO) glGenVertexArrays(1, &m_previewVAO);
				glBindVertexArray(m_previewVAO);
				if (!m_previewVBO) glGenBuffers(1, &m_previewVBO);
				glBindBuffer(GL_ARRAY_BUFFER, m_previewVBO);
				glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
				if (!m_previewIBO) glGenBuffers(1, &m_previewIBO);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_previewIBO);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
				glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
				glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
				glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
				glBindVertexArray(0);
			}
			unsigned int baseTex = 0;
			if (ma)
			{
				const HE::UUID tid = ma->textureIds.empty()   ? HE::UUID{} : ma->textureIds[0];
				const std::string tp = ma->texturePaths.empty() ? std::string{} : ma->texturePaths[0];
				if (tid != HE::UUID{} || !tp.empty()) baseTex = ResolveGraphTexture(tid, tp);
			}
			// Wider FOV here than the graph path, hence the shorter distance — both
			// land on the same apparent sphere size (see the constants above).
			DrawMeshPreviewGeometry(m_previewVAO, m_previewIdxCount, baseTex,
				glm::vec3(0.0f), 1.0f,
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
			DrawMeshPreviewGeometry(mesh->vao, mesh->indexCount, mesh->texture, c, e,
				glm::vec3(0.78f), 0.0f, 0.55f, kYaw, kPitch, kMeshFrameDist);
			drew = true;
		}
	}
	else if (kind == ThumbnailKind::SkeletalMesh)
	{
		// Bind pose: the stored vertex positions ARE the bind pose, so the plain
		// mesh program is enough — no bone matrices, no skinning shader. The extra
		// bone attributes on this VAO are simply unused by that program.
		if (const GpuSkeletalMesh* mesh = ResolveSkeletalMesh(assetId))
		{
			const HE::AABB& b = mesh->localBounds;
			const glm::vec3 c = b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
			const float e = b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
			DrawMeshPreviewGeometry(mesh->vao, mesh->indexCount, mesh->texture, c, e,
				glm::vec3(0.78f), 0.0f, 0.55f, kYaw, kPitch, kMeshFrameDist);
			drew = true;
		}
	}

	if (drew) ReadThumbnailTarget(S, outRgba8);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return drew;
}

void* OpenGLRenderer::RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
                                           uint32_t size, float yaw, float pitch, float dist,
                                           int shape, const HE::UUID& meshId)
{
	const int S = std::clamp(static_cast<int>(size), 32, 1024);
	if (!m_contentManager) m_contentManager = &cm;

	// Nothing to preview for a built-in-PBR material (no node-graph program) — bail
	// out BEFORE allocating the target, so a material without a graph never costs an
	// FBO. The thumbnail path handles that case with its own fallback program.
	{
		uint64_t probeKey; std::string probeFrag, probeVert;
		if (!resolveMaterialShader(materialId, probeKey, probeFrag, probeVert)) return nullptr;
	}

	// ── Lazy / resized offscreen target.
	if (!m_previewFBO || m_previewSize != S)
	{
		if (m_previewColor) glDeleteTextures(1, &m_previewColor);
		if (m_previewDepth) glDeleteRenderbuffers(1, &m_previewDepth);
		if (!m_previewFBO) glGenFramebuffers(1, &m_previewFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_previewFBO);
		glGenTextures(1, &m_previewColor);
		glBindTexture(GL_TEXTURE_2D, m_previewColor);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_previewColor, 0);
		glGenRenderbuffers(1, &m_previewDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_previewDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, S, S);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_previewDepth);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		m_previewSize = S;
	}

	// ── Draw the sphere into the preview target. Save/restore the caller's FBO+viewport.
	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_previewFBO);
	glViewport(0, 0, S, S);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent — editor composites over its own backdrop
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
	glDisable(GL_BLEND); glDisable(GL_CULL_FACE);

	if (!DrawMaterialPreviewGeometry(materialId, yaw, pitch, dist, shape, meshId))
	{
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
		glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
		glUseProgram(0);
		return nullptr;
	}

	// Headless witness: dump the preview target to a PPM (HE_PREVIEW_DUMP=path).
	if (const char* dp = std::getenv("HE_PREVIEW_DUMP"); dp && *dp)
	{
		std::vector<uint8_t> px((size_t)S * S * 3);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, S, S, GL_RGB, GL_UNSIGNED_BYTE, px.data());
		if (std::ofstream f(dp, std::ios::binary); f)
		{
			f << "P6\n" << S << " " << S << "\n255\n";
			for (int y = S - 1; y >= 0; --y) // flip to top-down
				f.write(reinterpret_cast<const char*>(px.data() + (size_t)y * S * 3), (std::streamsize)S * 3);
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return reinterpret_cast<void*>(static_cast<intptr_t>(m_previewColor));
}

// Compile the minimal skinning program AND the immediate-mode line program
// (with its shared VAO/VBO) once — they are built together because every caller
// that draws a preview mesh also wants to draw lines over it (bone overlay,
// world-preview grid). One place, so the two previews cannot drift.
bool OpenGLRenderer::EnsureSkelPreviewPrograms()
{
	if (m_skelPreviewProgram && m_skelPreviewLineProgram) return true;
	if (!m_skelPreviewProgram)
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kSkelPreviewVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kSkelPreviewFS);
		m_skelPreviewProgram = glCreateProgram();
		glAttachShader(m_skelPreviewProgram, vs);
		glAttachShader(m_skelPreviewProgram, fs);
		glLinkProgram(m_skelPreviewProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		m_uSkelPvMVP    = glGetUniformLocation(m_skelPreviewProgram, "uMVP");
		m_uSkelPvModel  = glGetUniformLocation(m_skelPreviewProgram, "uModel");
		m_uSkelPvBones  = glGetUniformLocation(m_skelPreviewProgram, "uBoneMatrices");
		m_uSkelPvColor  = glGetUniformLocation(m_skelPreviewProgram, "uColor");
		m_uSkelPvHasTex = glGetUniformLocation(m_skelPreviewProgram, "uHasTex");
		m_uSkelPvSun      = glGetUniformLocation(m_skelPreviewProgram, "uSun");
		m_uSkelPvSunColor = glGetUniformLocation(m_skelPreviewProgram, "uSunColor");
		m_uSkelPvAmbient  = glGetUniformLocation(m_skelPreviewProgram, "uAmbient");
	}
	if (!m_skelPreviewLineProgram)
	{
		GLuint lvs = CompileStage(GL_VERTEX_SHADER,   kSkelPreviewLineVS);
		GLuint lfs = CompileStage(GL_FRAGMENT_SHADER, kSkelPreviewLineFS);
		m_skelPreviewLineProgram = glCreateProgram();
		glAttachShader(m_skelPreviewLineProgram, lvs);
		glAttachShader(m_skelPreviewLineProgram, lfs);
		glLinkProgram(m_skelPreviewLineProgram);
		glDeleteShader(lvs); glDeleteShader(lfs);
		m_uSkelPvLineMVP = glGetUniformLocation(m_skelPreviewLineProgram, "uMVP");

		glGenVertexArrays(1, &m_skelPreviewLineVAO);
		glGenBuffers(1, &m_skelPreviewLineVBO);
		glBindVertexArray(m_skelPreviewLineVAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_skelPreviewLineVBO);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
		glBindVertexArray(0);
	}
	return m_skelPreviewProgram != 0 && m_skelPreviewLineProgram != 0;
}

void* OpenGLRenderer::RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
                                           const std::vector<glm::mat4>& boneMatrices,
                                           uint32_t width, uint32_t height,
                                           float yaw, float pitch, float dist,
                                           bool showSkeleton,
                                           glm::mat4* outViewProj)
{
	const int W = std::clamp(static_cast<int>(width),  32, 2048);
	const int H = std::clamp(static_cast<int>(height), 32, 2048);
	if (!m_contentManager) m_contentManager = &cm;

	const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(meshId);
	if (!smesh) return nullptr;

	// ── Lazy minimal skinning + line programs.
	if (!EnsureSkelPreviewPrograms()) return nullptr;

	// ── Lazy / resized offscreen target.
	if (!m_skelPreviewFBO || m_skelPreviewW != W || m_skelPreviewH != H)
	{
		if (m_skelPreviewColor) glDeleteTextures(1, &m_skelPreviewColor);
		if (m_skelPreviewDepth) glDeleteRenderbuffers(1, &m_skelPreviewDepth);
		if (!m_skelPreviewFBO) glGenFramebuffers(1, &m_skelPreviewFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_skelPreviewFBO);
		glGenTextures(1, &m_skelPreviewColor);
		glBindTexture(GL_TEXTURE_2D, m_skelPreviewColor);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_skelPreviewColor, 0);
		glGenRenderbuffers(1, &m_skelPreviewDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_skelPreviewDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_skelPreviewDepth);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		m_skelPreviewW = W;
		m_skelPreviewH = H;
	}

	// ── Orbit camera auto-framed around the mesh's local bounds (arbitrary
	// meshes vary in size/pivot, unlike the material preview's fixed unit shapes).
	const HE::AABB& b = smesh->localBounds;
	const glm::vec3 center = b.isValid() ? (b.min + b.max) * 0.5f : glm::vec3(0.0f);
	const float extent = b.isValid() ? glm::length(b.max - b.min) * 0.5f : 1.0f;
	const float camDist = std::max(0.05f, dist) * std::max(extent, 0.05f);

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_skelPreviewFBO);
	glViewport(0, 0, W, H);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent — editor composites over its own backdrop
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
	glDisable(GL_BLEND); glDisable(GL_CULL_FACE);

	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f),
		static_cast<float>(W) / static_cast<float>(H), 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 model(1.0f);
	const glm::mat4 mvp = proj * view * model;
	// Hand the framing out so a caller can overlay in the same space (model is
	// identity here, so the view-projection is the whole transform).
	if (outViewProj) *outViewProj = proj * view;

	constexpr int kMaxBones = 128;
	std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
	const int boneCount = static_cast<int>(std::min(boneMatrices.size(), static_cast<size_t>(kMaxBones)));
	if (boneCount > 0) std::copy_n(boneMatrices.begin(), boneCount, boneScratch.begin());

	glUseProgram(m_skelPreviewProgram);
	glUniformMatrix4fv(m_uSkelPvMVP,   1, GL_FALSE, glm::value_ptr(mvp));
	glUniformMatrix4fv(m_uSkelPvModel, 1, GL_FALSE, glm::value_ptr(model));
	glUniformMatrix4fv(m_uSkelPvBones, kMaxBones, GL_FALSE, glm::value_ptr(boneScratch[0]));
	glUniform3fv(m_uSkelPvColor, 1, glm::value_ptr(glm::vec3(0.75f)));
	glUniform1i(m_uSkelPvHasTex, smesh->texture != 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, smesh->texture);
	glBindVertexArray(smesh->vao);
	glDrawElements(GL_TRIANGLES, smesh->indexCount, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);

	// ── Bone overlay: joint markers + parent→child segments. World joint xform
	// = boneMatrix * inverse(inverseBindMatrix), since boneMatrix is defined as
	// globalJointXform * invBind (composeBoneMatrices, AnimationEval.cpp).
	if (showSkeleton)
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
				glBindBuffer(GL_ARRAY_BUFFER, m_skelPreviewLineVBO);
				glBufferData(GL_ARRAY_BUFFER,
				             static_cast<GLsizeiptr>(lineVerts.size() * sizeof(float)),
				             lineVerts.data(), GL_DYNAMIC_DRAW);
				glUseProgram(m_skelPreviewLineProgram);
				glUniformMatrix4fv(m_uSkelPvLineMVP, 1, GL_FALSE, glm::value_ptr(mvp));
				glBindVertexArray(m_skelPreviewLineVAO);
				glLineWidth(2.0f);
				glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lineVerts.size() / 6));
				glBindVertexArray(0);
			}
		}
	}

	// Headless witness (same convention as RenderMaterialPreview): HE_SKEL_PREVIEW_DUMP=path.
	if (const char* dp = std::getenv("HE_SKEL_PREVIEW_DUMP"); dp && *dp)
	{
		std::vector<uint8_t> px((size_t)W * H * 3);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px.data());
		if (std::ofstream f(dp, std::ios::binary); f)
		{
			f << "P6\n" << W << " " << H << "\n255\n";
			for (int y = H - 1; y >= 0; --y) // flip to top-down
				f.write(reinterpret_cast<const char*>(px.data() + (size_t)y * W * 3), (std::streamsize)W * 3);
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return reinterpret_cast<void*>(static_cast<intptr_t>(m_skelPreviewColor));
}

void* OpenGLRenderer::RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
                                         uint32_t width, uint32_t height,
                                         const EditorCameraOverride& camera,
                                         const glm::vec3& origin,
                                         const WorldPreviewEnv& env,
                                         glm::mat4* outViewProj,
                                         uint32_t slot)
{
	WorldPreviewTarget& wp = m_worldPreview[std::min(slot, kWorldPreviewSlots - 1)];
	const int W = std::clamp(static_cast<int>(width),  32, 4096);
	const int H = std::clamp(static_cast<int>(height), 32, 4096);
	if (!m_contentManager) m_contentManager = &cm;
	if (!EnsureSkelPreviewPrograms()) return nullptr;
	if (!EnsureMeshPreviewProgram())  return nullptr;

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
	// packaged game build theirs — from an EnvironmentComponent through
	// makeEnvironmentSettings. Hand-assembling the struct here would be a fourth
	// copy of "what does a default sky look like", and it would drift from the
	// Sky panel's defaults the first time one of them changed. dt = 0: nothing
	// may advance in a preview (same convention as the headless dump path).
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

	// ── Lazy / resized offscreen targets. TWO of them, mirroring the scene: the
	// pass renders HDR (the sky's radiance runs well past 1.0, and so does a sun
	// at intensity 2.2), then a tonemap resolves that into the LDR texture ImGui
	// shows. Writing the HDR values straight into an 8-bit target is what made
	// the first sky-lit preview a uniformly white mesh under a blown-out sky.
	if (!wp.fbo || wp.w != W || wp.h != H)
	{
		if (wp.color) glDeleteTextures(1, &wp.color);
		if (wp.hdr)   glDeleteTextures(1, &wp.hdr);
		if (wp.depth) glDeleteRenderbuffers(1, &wp.depth);
		if (!wp.fbo)    glGenFramebuffers(1, &wp.fbo);
		if (!wp.ldrFBO) glGenFramebuffers(1, &wp.ldrFBO);

		glBindFramebuffer(GL_FRAMEBUFFER, wp.fbo);
		glGenTextures(1, &wp.hdr);
		glBindTexture(GL_TEXTURE_2D, wp.hdr);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wp.hdr, 0);
		glGenRenderbuffers(1, &wp.depth);
		glBindRenderbuffer(GL_RENDERBUFFER, wp.depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, wp.depth);

		glBindFramebuffer(GL_FRAMEBUFFER, wp.ldrFBO);
		glGenTextures(1, &wp.color);
		glBindTexture(GL_TEXTURE_2D, wp.color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wp.color, 0);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		wp.w = W;
		wp.h = H;
	}

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, wp.fbo);
	glViewport(0, 0, W, H);
	// Studio gray — covered by the sky when there is one. LINEAR: this is resolved
	// through ACES + gamma below, which lifts it a long way (see kPreviewBackground).
	glClearColor(HE::kPreviewBackground[0], HE::kPreviewBackground[1],
	             HE::kPreviewBackground[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
	glDisable(GL_BLEND); glDisable(GL_CULL_FACE);

	// ── Sky FIRST, unlike the scene, which draws it last to skip the shading
	// behind solid geometry. Here the ground must not occlude the mesh (see
	// below), so it writes no depth — and a sky drawn afterwards would then
	// paint straight over it. A preview target is small; the overdraw is not
	// worth the ordering trap.
	if (env.sky)
		DrawSkyFullscreen(glm::inverse(viewProj), snapshot.sunDirection, camPos,
		                  previewEnvSettings, /*allowLowResClouds=*/false, W, H);

	// ── Grid + origin marker. LINES ONLY — a filled floor in a viewer is only
	// ever in the way, hiding the underside of the mesh and, with a sky on, the
	// entire lower half of the world. Drawn without depth writes so the lines
	// cannot occlude the object either.
	if (env.grid)
	{
		// Extent follows how far the camera has flown from the origin so the grid
		// never runs out — but capped, because the line count grows with it and a
		// free camera can end up two thousand units away.
		const float halfExtent = std::clamp(
			std::ceil(glm::length(camPos - origin) * 2.0f), 10.0f, 200.0f);
		std::vector<float> verts;
		HE::buildPreviewGrid(halfExtent, 1.0f, verts, origin);

		glBindBuffer(GL_ARRAY_BUFFER, m_skelPreviewLineVBO);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
		             verts.data(), GL_DYNAMIC_DRAW);
		glUseProgram(m_skelPreviewLineProgram);
		glUniformMatrix4fv(m_uSkelPvLineMVP, 1, GL_FALSE, glm::value_ptr(viewProj));
		glBindVertexArray(m_skelPreviewLineVAO);
		glDepthMask(GL_FALSE);
		glLineWidth(1.0f);
		glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 6));
		glDepthMask(GL_TRUE);
		glBindVertexArray(0);
	}

	// ── Static meshes.
	glUseProgram(m_meshPreviewProgram);
	glUniform3fv(m_uMeshPvCamPos, 1, glm::value_ptr(camPos));
	glUniform4fv(m_uMeshPvSun,      1, glm::value_ptr(sunUniform));
	glUniform3fv(m_uMeshPvSunColor, 1, glm::value_ptr(sunColor));
	glUniform3fv(m_uMeshPvAmbient,  1, glm::value_ptr(ambient));
	glActiveTexture(GL_TEXTURE0);
	for (const RenderObject& obj : snapshot.objects)
	{
		const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		if (!mesh || !mesh->vao || mesh->indexCount <= 0) continue;
		const glm::mat4 mvp = viewProj * obj.transform;
		glUniformMatrix4fv(m_uMeshPvMVP,   1, GL_FALSE, glm::value_ptr(mvp));
		glUniformMatrix4fv(m_uMeshPvModel, 1, GL_FALSE, glm::value_ptr(obj.transform));
		glUniform3fv(m_uMeshPvColor, 1, glm::value_ptr(obj.baseColor));
		glUniform2f(m_uMeshPvPbr, obj.metallic, obj.roughness);
		glUniform1i(m_uMeshPvHasTex, mesh->texture != 0);
		glBindTexture(GL_TEXTURE_2D, mesh->texture);
		glBindVertexArray(mesh->vao);
		glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
	}
	glBindVertexArray(0);

	// ── Skinned meshes (the pose the AnimatorHost last wrote, or the bind pose).
	constexpr int kMaxBones = 128;
	glUseProgram(m_skelPreviewProgram);
	glUniform4fv(m_uSkelPvSun,      1, glm::value_ptr(sunUniform));
	glUniform3fv(m_uSkelPvSunColor, 1, glm::value_ptr(sunColor));
	glUniform3fv(m_uSkelPvAmbient,  1, glm::value_ptr(ambient));
	glActiveTexture(GL_TEXTURE0);
	for (const SkinnedRenderObject& obj : snapshot.skinnedObjects)
	{
		const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(obj.meshAssetId);
		if (!smesh || !smesh->vao || smesh->indexCount <= 0) continue;
		std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
		const int boneCount = static_cast<int>(std::min(obj.boneMatrices.size(),
		                                                static_cast<size_t>(kMaxBones)));
		if (boneCount > 0) std::copy_n(obj.boneMatrices.begin(), boneCount, boneScratch.begin());
		const glm::mat4 mvp = viewProj * obj.transform;
		glUniformMatrix4fv(m_uSkelPvMVP,   1, GL_FALSE, glm::value_ptr(mvp));
		glUniformMatrix4fv(m_uSkelPvModel, 1, GL_FALSE, glm::value_ptr(obj.transform));
		glUniformMatrix4fv(m_uSkelPvBones, kMaxBones, GL_FALSE, glm::value_ptr(boneScratch[0]));
		glUniform3fv(m_uSkelPvColor, 1, glm::value_ptr(obj.baseColor));
		glUniform1i(m_uSkelPvHasTex, smesh->texture != 0);
		glBindTexture(GL_TEXTURE_2D, smesh->texture);
		glBindVertexArray(smesh->vao);
		glDrawElements(GL_TRIANGLES, smesh->indexCount, GL_UNSIGNED_INT, nullptr);
	}
	glBindVertexArray(0);

	// ── Tonemap resolve: HDR → the 8-bit texture ImGui shows, through the very
	// same program the scene's frame ends with. Without it the sky's radiance
	// and a sun at intensity 2.2 both clip, and everything lit lands on white.
	// Bloom is bound at strength 0 (a preview is not a film camera) and the lens
	// flare zeroed; the sampler still needs a valid binding, so the HDR texture
	// stands in for the bloom buffer.
	if (m_tonemapProgram && wp.ldrFBO)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, wp.ldrFBO);
		glViewport(0, 0, W, H);
		glDisable(GL_DEPTH_TEST);
		glUseProgram(m_tonemapProgram);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, wp.hdr);
		glUniform1i(m_uHDRTex, 0);
		glUniform1f(m_uExposure, 1.0f);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, wp.hdr);
		glUniform1i(m_uBloomTex, 1);
		glUniform1f(m_uBloomStrength, 0.0f);
		if (m_uLensFlare >= 0)
		{
			const float noFlare[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			glUniform4fv(m_uLensFlare, 1, noFlare);
		}
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(m_fsVAO);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
		glEnable(GL_DEPTH_TEST);
	}

	// Headless witness, same convention as the sibling previews. Reads the LDR
	// result, i.e. what the editor actually shows.
	if (const char* dp = std::getenv("HE_WORLD_PREVIEW_DUMP"); dp && *dp)
	{
		std::vector<uint8_t> px((size_t)W * H * 3);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, px.data());
		if (std::ofstream f(dp, std::ios::binary); f)
		{
			f << "P6\n" << W << " " << H << "\n255\n";
			for (int y = H - 1; y >= 0; --y) // flip to top-down
				f.write(reinterpret_cast<const char*>(px.data() + (size_t)y * W * 3), (std::streamsize)W * 3);
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return reinterpret_cast<void*>(static_cast<intptr_t>(wp.color));
}

// Compile the billboard program + its instance VAO once. Split out so both the
// interactive preview and the Content-Browser thumbnail can reach it.
bool OpenGLRenderer::EnsureParticlePreviewProgram()
{
	if (!m_particlePreviewProgram)
	{
		GLuint vs = CompileStage(GL_VERTEX_SHADER,   kParticlePreviewVS);
		GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kParticlePreviewFS);
		m_particlePreviewProgram = glCreateProgram();
		glAttachShader(m_particlePreviewProgram, vs);
		glAttachShader(m_particlePreviewProgram, fs);
		glLinkProgram(m_particlePreviewProgram);
		glDeleteShader(vs); glDeleteShader(fs);
		m_uPPvViewProj = glGetUniformLocation(m_particlePreviewProgram, "uViewProj");
		m_uPPvCamRight = glGetUniformLocation(m_particlePreviewProgram, "uCamRight");
		m_uPPvCamUp    = glGetUniformLocation(m_particlePreviewProgram, "uCamUp");
		m_uPPvHasTex   = glGetUniformLocation(m_particlePreviewProgram, "uHasTex");

		glGenVertexArrays(1, &m_particlePreviewVAO);
		glGenBuffers(1, &m_particlePreviewInstVBO);
		glBindVertexArray(m_particlePreviewVAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_particlePreviewInstVBO);
		constexpr GLsizei kStride = 8 * sizeof(float); // pos3 + size1 + color3 + alpha1
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, (void*)0);
		glVertexAttribDivisor(0, 1);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(3 * sizeof(float)));
		glVertexAttribDivisor(1, 1);
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride, (void*)(4 * sizeof(float)));
		glVertexAttribDivisor(2, 1);
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(7 * sizeof(float)));
		glVertexAttribDivisor(3, 1);
		glBindVertexArray(0);
	}
	return m_particlePreviewProgram != 0;
}

// Draw the particle cloud into whatever target is bound (caller owns FBO,
// viewport and clear). Shared by RenderParticlePreview and the thumbnail path so
// the two can never drift; the thumbnail must NOT reuse the preview's own target
// or it would overwrite what the Particle Graph Editor is showing.
void OpenGLRenderer::DrawParticlePreviewGeometry(const HE::UUID& materialId,
                                                const std::vector<ParticlePreviewInstance>& particles,
                                                float yaw, float pitch, float dist)
{
	// ── Orbit camera auto-framed around the LIVE particles' bounds (an emitter's
	// extent depends entirely on velocity/gravity/lifetime, unlike a fixed mesh).
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

	glEnable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); glDepthFunc(GL_LESS);
	glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);

	const float cp = std::cos(pitch), sp = std::sin(pitch);
	const glm::vec3 camPos = center + glm::vec3(std::sin(yaw) * cp, sp, std::cos(yaw) * cp) * camDist;
	const glm::mat4 view = glm::lookAt(camPos, center, glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 proj = glm::perspective(glm::radians(35.0f), 1.0f, 0.01f, camDist * 20.0f + 10.0f);
	const glm::mat4 viewProj = proj * view;
	// Camera-facing basis for billboard expansion (right = view row 0, up = view row 1).
	const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
	const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

	unsigned int tex = 0;
	const bool hasTex = ResolveMaterialTexture(materialId, tex);

	if (!particles.empty())
	{
		std::vector<float> inst;
		inst.reserve(particles.size() * 8);
		for (const auto& p : particles)
			inst.insert(inst.end(), { p.position.x, p.position.y, p.position.z, p.size,
			                          p.color.r, p.color.g, p.color.b, p.alpha });

		glBindBuffer(GL_ARRAY_BUFFER, m_particlePreviewInstVBO);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(inst.size() * sizeof(float)),
		             inst.data(), GL_DYNAMIC_DRAW);

		glUseProgram(m_particlePreviewProgram);
		glUniformMatrix4fv(m_uPPvViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
		glUniform3fv(m_uPPvCamRight, 1, glm::value_ptr(camRight));
		glUniform3fv(m_uPPvCamUp,    1, glm::value_ptr(camUp));
		glUniform1i(m_uPPvHasTex, hasTex);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hasTex ? tex : 0);
		glBindVertexArray(m_particlePreviewVAO);
		glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(particles.size()));
		glBindVertexArray(0);
	}

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

bool OpenGLRenderer::RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                             const std::vector<ParticlePreviewInstance>& particles,
                                             uint32_t size, std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	if (!m_contentManager) m_contentManager = &cm;
	if (particles.empty() || !EnsureParticlePreviewProgram()) return false;
	if (!EnsureThumbnailTarget(S)) return false;

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_thumbFBO);
	glViewport(0, 0, S, S);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Same fixed three-quarter framing as the mesh tiles, so a grid of assets
	// reads as one set. 2.6 leaves the cloud a margin at the 35° FOV.
	DrawParticlePreviewGeometry(materialId, particles, 0.7f, 0.45f, 2.9f);
	ReadThumbnailTarget(S, outRgba8);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return true;
}

bool OpenGLRenderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                           uint32_t size, std::vector<uint8_t>& outRgba8)
{
	const int S = std::clamp(static_cast<int>(size), 16, 512);
	if (uiObjects.empty() || !EnsureThumbnailTarget(S)) return false;

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_thumbFBO);
	glViewport(0, 0, S, S);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// RenderUIPass draws m_renderWorld.uiObjects into whatever is bound, so the
	// tile borrows that list for one pass and puts the frame's own back. Swapping
	// (not copying) keeps the scene's objects intact even if this throws.
	std::vector<UIRenderObject> saved;
	saved.swap(m_renderWorld.uiObjects);
	m_renderWorld.uiObjects = uiObjects;
	RenderUIPass(S, S);
	m_renderWorld.uiObjects.swap(saved);

	ReadThumbnailTarget(S, outRgba8);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return true;
}

void* OpenGLRenderer::RenderParticlePreview(ContentManager& cm, const HE::UUID& /*meshId*/,
                                           const HE::UUID& materialId,
                                           const std::vector<ParticlePreviewInstance>& particles,
                                           uint32_t size, float yaw, float pitch, float dist)
{
	const int S = std::clamp(static_cast<int>(size), 32, 1024);
	if (!m_contentManager) m_contentManager = &cm;
	if (!EnsureParticlePreviewProgram()) return nullptr;

	// ── Lazy / resized offscreen target.
	if (!m_particlePreviewFBO || m_particlePreviewSize != S)
	{
		if (m_particlePreviewColor) glDeleteTextures(1, &m_particlePreviewColor);
		if (m_particlePreviewDepth) glDeleteRenderbuffers(1, &m_particlePreviewDepth);
		if (!m_particlePreviewFBO) glGenFramebuffers(1, &m_particlePreviewFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, m_particlePreviewFBO);
		glGenTextures(1, &m_particlePreviewColor);
		glBindTexture(GL_TEXTURE_2D, m_particlePreviewColor);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_particlePreviewColor, 0);
		glGenRenderbuffers(1, &m_particlePreviewDepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_particlePreviewDepth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, S, S);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_particlePreviewDepth);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		m_particlePreviewSize = S;
	}

	GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	GLint prevVP[4]; glGetIntegerv(GL_VIEWPORT, prevVP);
	glBindFramebuffer(GL_FRAMEBUFFER, m_particlePreviewFBO);
	glViewport(0, 0, S, S);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	DrawParticlePreviewGeometry(materialId, particles, yaw, pitch, dist);

	if (const char* dp = std::getenv("HE_PARTICLE_PREVIEW_DUMP"); dp && *dp)
	{
		std::vector<uint8_t> px((size_t)S * S * 3);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, S, S, GL_RGB, GL_UNSIGNED_BYTE, px.data());
		if (std::ofstream f(dp, std::ios::binary); f)
		{
			f << "P6\n" << S << " " << S << "\n255\n";
			for (int y = S - 1; y >= 0; --y)
				f.write(reinterpret_cast<const char*>(px.data() + (size_t)y * S * 3), (std::streamsize)S * 3);
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
	glUseProgram(0);
	return reinterpret_cast<void*>(static_cast<intptr_t>(m_particlePreviewColor));
}

void OpenGLRenderer::SetDebugLines(const std::vector<DebugLine>& lines)
{
	m_debugLines = lines;
}

void OpenGLRenderer::CreateDebugLinePipeline()
{
	const char* vs = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
uniform mat4 uVP;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uVP * vec4(aPos, 1.0); }
)";
	const char* fs = R"(
#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor, 1.0); }
)";
	auto compile = [](const char* src, GLenum type) -> unsigned int
	{
		unsigned int sh = glCreateShader(type);
		glShaderSource(sh, 1, &src, nullptr);
		glCompileShader(sh);
		return sh;
	};
	unsigned int v = compile(vs, GL_VERTEX_SHADER);
	unsigned int f = compile(fs, GL_FRAGMENT_SHADER);
	m_debugLineProgram = glCreateProgram();
	glAttachShader(m_debugLineProgram, v);
	glAttachShader(m_debugLineProgram, f);
	glLinkProgram(m_debugLineProgram);
	glDeleteShader(v); glDeleteShader(f);
	m_uDebugVP = glGetUniformLocation(m_debugLineProgram, "uVP");

	glGenVertexArrays(1, &m_debugLineVAO);
	glGenBuffers(1, &m_debugLineVBO);
	glBindVertexArray(m_debugLineVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_debugLineVBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindVertexArray(0);
}

void OpenGLRenderer::DrawDebugLines(const glm::mat4& viewProj)
{
	if (m_debugLines.empty() || !m_debugLineProgram) return;

	// Pack line endpoints into a flat float buffer: [pos3 color3] per vertex
	std::vector<float> verts;
	verts.reserve(m_debugLines.size() * 12); // 2 verts * 6 floats
	for (const DebugLine& l : m_debugLines)
	{
		verts.insert(verts.end(), { l.start.x, l.start.y, l.start.z,
		                            l.color.r,  l.color.g,  l.color.b });
		verts.insert(verts.end(), { l.end.x,   l.end.y,   l.end.z,
		                            l.color.r,  l.color.g,  l.color.b });
	}

	glBindBuffer(GL_ARRAY_BUFFER, m_debugLineVBO);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)),
	             verts.data(), GL_STREAM_DRAW);

	glUseProgram(m_debugLineProgram);
	glUniformMatrix4fv(m_uDebugVP, 1, GL_FALSE, glm::value_ptr(viewProj));
	glBindVertexArray(m_debugLineVAO);
	glDrawArrays(GL_LINES, 0, (GLsizei)(m_debugLines.size() * 2));
	glBindVertexArray(0);
}

// ─── Motion trails ───────────────────────────────────────────────────────────
// One pool slot per band, re-uploaded every frame with GL_STREAM_DRAW. The
// attribute layout is the mesh layout verbatim (pos3 + norm3 + uv2, stride 32),
// which is exactly why a trail needs no program of its own: the scene's own
// vertex shader reads these buffers unchanged.
unsigned int OpenGLRenderer::UploadRibbon(size_t index, const RibbonBatch& batch)
{
	if (batch.vertices.empty() || batch.indices.empty()) return 0;
	if (m_ribbonMeshes.size() <= index) m_ribbonMeshes.resize(index + 1);
	RibbonMesh& rm = m_ribbonMeshes[index];
	if (!rm.vao)
	{
		glGenVertexArrays(1, &rm.vao);
		glGenBuffers(1, &rm.vbo);
		glGenBuffers(1, &rm.ebo);
		if (!rm.vao || !rm.vbo || !rm.ebo) return 0;
		glBindVertexArray(rm.vao);
		glBindBuffer(GL_ARRAY_BUFFER, rm.vbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rm.ebo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
	}
	else
	{
		glBindVertexArray(rm.vao);
		glBindBuffer(GL_ARRAY_BUFFER, rm.vbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rm.ebo);
	}
	// Whole-buffer orphaning: the band changes size as points are dropped and
	// added, so there is no fixed allocation to fill in place.
	glBufferData(GL_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(batch.vertices.size() * sizeof(float)),
	             batch.vertices.data(), GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             static_cast<GLsizeiptr>(batch.indices.size() * sizeof(uint32_t)),
	             batch.indices.data(), GL_STREAM_DRAW);
	glBindVertexArray(0);
	return rm.vao;
}

void OpenGLRenderer::DestroyRibbonMeshes()
{
	for (RibbonMesh& rm : m_ribbonMeshes)
	{
		if (rm.vao) glDeleteVertexArrays(1, &rm.vao);
		if (rm.vbo) glDeleteBuffers(1, &rm.vbo);
		if (rm.ebo) glDeleteBuffers(1, &rm.ebo);
	}
	m_ribbonMeshes.clear();
}

void OpenGLRenderer::Shutdown()
{
	HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: shutdown");

	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	for (auto& [id, mesh] : m_meshCache)
	{
		if (mesh.vao)     glDeleteVertexArrays(1, &mesh.vao);
		if (mesh.vbo)     glDeleteBuffers(1, &mesh.vbo);
		if (mesh.ebo)     glDeleteBuffers(1, &mesh.ebo);
		if (mesh.texture) glDeleteTextures(1, &mesh.texture);
	}
	m_meshCache.clear();
	DestroyGIAccel();
	for (auto& [id, tex] : m_materialTexCache)
		if (tex) glDeleteTextures(1, &tex);
	m_materialTexCache.clear();
	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyGBufferTargets();
	if (m_ssaoDepthPosProgram)     { glDeleteProgram(m_ssaoDepthPosProgram);     m_ssaoDepthPosProgram = 0; }
	if (m_gbufferProgram)          { glDeleteProgram(m_gbufferProgram);          m_gbufferProgram = 0; }
	if (m_gbufferInstancedProgram) { glDeleteProgram(m_gbufferInstancedProgram); m_gbufferInstancedProgram = 0; }
	if (m_deferredResolveProgram)  { glDeleteProgram(m_deferredResolveProgram);  m_deferredResolveProgram = 0; }
	if (m_resolveUBO)      { glDeleteBuffers(1, &m_resolveUBO);      m_resolveUBO = 0; }
	if (m_resolveLightUBO) { glDeleteBuffers(1, &m_resolveLightUBO); m_resolveLightUBO = 0; }
	if (m_decalProgram)    { glDeleteProgram(m_decalProgram);        m_decalProgram = 0; }
	if (m_decalUBO)        { glDeleteBuffers(1, &m_decalUBO);        m_decalUBO = 0; }
	// Forward SSR + its reflection pre-pass (the targets go with DestroySSAOTargets
	// / DestroySSRTargets below; the "tried" flags reset so a re-Initialize()
	// gets a fresh build attempt instead of a permanently disabled feature).
	if (m_reflPrepassProgram) { glDeleteProgram(m_reflPrepassProgram); m_reflPrepassProgram = 0; }
	if (m_reflPrepassUBO)     { glDeleteBuffers(1, &m_reflPrepassUBO); m_reflPrepassUBO = 0; }
	if (m_reflPrepassInstProgram) { glDeleteProgram(m_reflPrepassInstProgram); m_reflPrepassInstProgram = 0; }
	if (m_reflPrepassInstUBO)     { glDeleteBuffers(1, &m_reflPrepassInstUBO); m_reflPrepassInstUBO = 0; }
	if (m_ssrTraceProgram)   { glDeleteProgram(m_ssrTraceProgram);    m_ssrTraceProgram = 0; }
	if (m_ssrBlurProgram)     { glDeleteProgram(m_ssrBlurProgram);     m_ssrBlurProgram = 0; }
	if (m_ssrTraceUBO)        { glDeleteBuffers(1, &m_ssrTraceUBO);    m_ssrTraceUBO = 0; }
	if (m_ssrBlurUBO)         { glDeleteBuffers(1, &m_ssrBlurUBO);     m_ssrBlurUBO = 0; }
	if (m_ssrColorHistFBO)    { glDeleteFramebuffers(1, &m_ssrColorHistFBO); m_ssrColorHistFBO = 0; }
	if (m_ssrColorHistTex)    { glDeleteTextures(1, &m_ssrColorHistTex);     m_ssrColorHistTex = 0; }
	m_ssrColorHistW = m_ssrColorHistH = 0;
	m_ssrColorHistValid = false;
	m_reflPrepassTried  = false;
	m_ssrProgramsTried  = false;
	DestroySSRTargets();
	m_decalProgramTried      = false;
	m_deferredPipelinesTried = false; // re-Initialize() rebuilds instead of staying forward
	DestroyBloomTargets();
	DestroyDepthOfFieldTargets();
	DestroyMotionBlurTargets();
	DestroyTaaTargets();
	DestroyCloudTarget();
	DestroyCloudShadowTarget();
	DestroyLdrTarget();
	DestroyGpuTimer();
	DestroySSAOTargets();
	DestroyGIShadowTargets();
	DestroyGIProbeAtlas();
	if (m_giGBufProgram)     { glDeleteProgram(m_giGBufProgram);     m_giGBufProgram = 0; }
	if (m_giGBufInstancedProgram) { glDeleteProgram(m_giGBufInstancedProgram); m_giGBufInstancedProgram = 0; }
	if (m_giTemporalProgram) { glDeleteProgram(m_giTemporalProgram); m_giTemporalProgram = 0; }
	if (m_giBlurProgram)     { glDeleteProgram(m_giBlurProgram);     m_giBlurProgram = 0; }
	if (m_giShadowCSProgram) { glDeleteProgram(m_giShadowCSProgram); m_giShadowCSProgram = 0; }
	if (m_giProbeCSProgram)  { glDeleteProgram(m_giProbeCSProgram);  m_giProbeCSProgram = 0; }
	for (auto& r : m_retiredTextures)
		glDeleteTextures(1, &r.texture);
	m_retiredTextures.clear();

	if (m_unlitProgram)     { glDeleteProgram(m_unlitProgram);     m_unlitProgram = 0; }
	// Material path — unconditional, like the path itself: these programs, UBOs and
	// the backdrop snapshot exist in a shaderc-free build too (from precompiled
	// variants), and a cleanup that skipped them there would simply leak.
	for (auto& [k, prog] : m_materialPrograms) if (prog) glDeleteProgram(prog);
	m_materialPrograms.clear();
	for (auto& [k, prog] : m_uiMaterialPrograms) if (prog) glDeleteProgram(prog);
	m_uiMaterialPrograms.clear();
	if (m_matObjUBO)   { glDeleteBuffers(1, &m_matObjUBO);   m_matObjUBO = 0; }
	if (m_matLightUBO) { glDeleteBuffers(1, &m_matLightUBO); m_matLightUBO = 0; }
	if (m_matParamUBO) { glDeleteBuffers(1, &m_matParamUBO); m_matParamUBO = 0; }
	if (m_matUIUBO)    { glDeleteBuffers(1, &m_matUIUBO);    m_matUIUBO    = 0; }
	if (m_uiBackdropTex) { glDeleteTextures(1, &m_uiBackdropTex); m_uiBackdropTex = 0;
	                       m_uiBackdropW = m_uiBackdropH = 0; }
	if (m_previewColor) { glDeleteTextures(1, &m_previewColor);      m_previewColor = 0; }
	if (m_previewDepth) { glDeleteRenderbuffers(1, &m_previewDepth); m_previewDepth = 0; }
	if (m_previewFBO)   { glDeleteFramebuffers(1, &m_previewFBO);    m_previewFBO = 0; }
	if (m_previewVBO)   { glDeleteBuffers(1, &m_previewVBO);         m_previewVBO = 0; }
	if (m_previewIBO)   { glDeleteBuffers(1, &m_previewIBO);         m_previewIBO = 0; }
	if (m_previewVAO)   { glDeleteVertexArrays(1, &m_previewVAO);    m_previewVAO = 0; }
	for (auto& [k, t] : m_graphTexCache) if (t) glDeleteTextures(1, &t);
	m_graphTexCache.clear();
	// Content-Browser thumbnail target + its mesh program.
	if (m_thumbColor)         { glDeleteTextures(1, &m_thumbColor);       m_thumbColor = 0; }
	if (m_thumbDepth)         { glDeleteRenderbuffers(1, &m_thumbDepth);  m_thumbDepth = 0; }
	if (m_thumbFBO)           { glDeleteFramebuffers(1, &m_thumbFBO);     m_thumbFBO = 0; }
	if (m_meshPreviewProgram) { glDeleteProgram(m_meshPreviewProgram);    m_meshPreviewProgram = 0; }
	// World-preview targets (RenderWorldPreview), every slot; their programs
	// are the skeletal preview's, freed with those.
	for (WorldPreviewTarget& wp : m_worldPreview)
	{
		if (wp.color) { glDeleteTextures(1, &wp.color);      wp.color = 0; }
		if (wp.hdr)   { glDeleteTextures(1, &wp.hdr);        wp.hdr = 0; }
		if (wp.depth) { glDeleteRenderbuffers(1, &wp.depth); wp.depth = 0; }
		if (wp.fbo)   { glDeleteFramebuffers(1, &wp.fbo);    wp.fbo = 0; }
		if (wp.ldrFBO){ glDeleteFramebuffers(1, &wp.ldrFBO); wp.ldrFBO = 0; }
		wp.w = 0;
		wp.h = 0;
	}
	if (m_instancedProgram) { glDeleteProgram(m_instancedProgram); m_instancedProgram = 0; }
	if (m_depthInstancedProgram) { glDeleteProgram(m_depthInstancedProgram); m_depthInstancedProgram = 0; }
	if (m_instanceVBO)      { glDeleteBuffers(1, &m_instanceVBO);  m_instanceVBO = 0; }
	for (auto& [k, prog] : m_particlePrograms) if (prog) glDeleteProgram(prog);
	m_particlePrograms.clear();
	if (m_particleVAO)     { glDeleteVertexArrays(1, &m_particleVAO);  m_particleVAO = 0; }
	if (m_particleInstVBO) { glDeleteBuffers(1, &m_particleInstVBO);   m_particleInstVBO = 0; }
	DestroyParticleResources();
	if (m_depthProgram) { glDeleteProgram(m_depthProgram);     m_depthProgram = 0; }
	if (m_skyProgram)   { glDeleteProgram(m_skyProgram);       m_skyProgram = 0; }
	if (m_tonemapProgram) { glDeleteProgram(m_tonemapProgram); m_tonemapProgram = 0; }
	if (m_fxaaProgram)    { glDeleteProgram(m_fxaaProgram);    m_fxaaProgram = 0; }
	if (m_smaaProgram)    { glDeleteProgram(m_smaaProgram);    m_smaaProgram = 0; }
	if (m_blitProgram)    { glDeleteProgram(m_blitProgram);    m_blitProgram = 0; }
	if (m_taaVelocityProgram) { glDeleteProgram(m_taaVelocityProgram); m_taaVelocityProgram = 0; }
	if (m_taaProgram)         { glDeleteProgram(m_taaProgram);         m_taaProgram = 0; }
	if (m_taaSharpenProgram)  { glDeleteProgram(m_taaSharpenProgram);  m_taaSharpenProgram = 0; }
	if (m_uiProgram)      { glDeleteProgram(m_uiProgram);      m_uiProgram = 0; }
	if (m_uiFontTexture)  { glDeleteTextures(1, &m_uiFontTexture); m_uiFontTexture = 0; }
	if (m_bloomBrightProgram) { glDeleteProgram(m_bloomBrightProgram); m_bloomBrightProgram = 0; }
	if (m_blurProgram)    { glDeleteProgram(m_blurProgram);    m_blurProgram = 0; }
	if (m_dofCocProgram)       { glDeleteProgram(m_dofCocProgram);       m_dofCocProgram = 0; }
	if (m_dofBlurProgram)      { glDeleteProgram(m_dofBlurProgram);      m_dofBlurProgram = 0; }
	if (m_dofCompositeProgram) { glDeleteProgram(m_dofCompositeProgram); m_dofCompositeProgram = 0; }
	if (m_mbVelocityProgram)   { glDeleteProgram(m_mbVelocityProgram);   m_mbVelocityProgram = 0; }
	if (m_mbBlurProgram)       { glDeleteProgram(m_mbBlurProgram);       m_mbBlurProgram = 0; }
	m_mbHasPrev = false;
	if (m_fsVAO)          { glDeleteVertexArrays(1, &m_fsVAO);  m_fsVAO = 0; }
	if (m_shadowFBO)      { glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0; }
	if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex);  m_shadowDepthTex = 0; }
	if (m_localShadowDepthTex) { glDeleteTextures(1, &m_localShadowDepthTex); m_localShadowDepthTex = 0; }
	if (m_moonTex)        { glDeleteTextures(1, &m_moonTex);         m_moonTex = 0; }
	if (m_noiseTex)       { glDeleteTextures(1, &m_noiseTex);        m_noiseTex = 0; }
	if (m_skyEnvCube)     { glDeleteTextures(1, &m_skyEnvCube);      m_skyEnvCube = 0; }
	if (m_ssaoNoiseTex)   { glDeleteTextures(1, &m_ssaoNoiseTex);    m_ssaoNoiseTex = 0; }
	if (m_whiteTex)       { glDeleteTextures(1, &m_whiteTex);        m_whiteTex = 0; }
	if (m_blackTex)       { glDeleteTextures(1, &m_blackTex);        m_blackTex = 0; }
	if (m_ssaoPosProgram)  { glDeleteProgram(m_ssaoPosProgram);  m_ssaoPosProgram = 0; }
	if (m_ssaoPosInstancedProgram) { glDeleteProgram(m_ssaoPosInstancedProgram); m_ssaoPosInstancedProgram = 0; }
	if (m_ssaoProgram)     { glDeleteProgram(m_ssaoProgram);     m_ssaoProgram = 0; }
	if (m_ssaoBlurProgram) { glDeleteProgram(m_ssaoBlurProgram); m_ssaoBlurProgram = 0; }
	if (m_debugLineProgram) { glDeleteProgram(m_debugLineProgram); m_debugLineProgram = 0; }
	if (m_debugLineVAO)     { glDeleteVertexArrays(1, &m_debugLineVAO); m_debugLineVAO = 0; }
	if (m_debugLineVBO)     { glDeleteBuffers(1, &m_debugLineVBO);      m_debugLineVBO = 0; }
	DestroyRibbonMeshes();

	// Destroy secondary contexts (secondary windows' SDL_GLContexts are owned by us)
	for (auto& [sdlWin, ctx] : m_secondaryContexts)
		if (ctx) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(ctx));
	m_secondaryContexts.clear();
	m_glContext        = nullptr;
	m_primarySdlWindow = nullptr;
}

// ─── Offscreen viewport target ────────────────────────────────────────────────

void OpenGLRenderer::SetViewportSize(uint32_t width, uint32_t height)
{
	m_viewportReqW = width;
	m_viewportReqH = height;
}

void* OpenGLRenderer::GetViewportTexture()
{
	return reinterpret_cast<void*>(static_cast<intptr_t>(m_viewportColor));
}

bool OpenGLRenderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
	if (!m_viewportFBO || m_viewportW <= 0 || m_viewportH <= 0)
		return false;

	const int w = m_viewportW;
	const int h = m_viewportH;
	rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

	glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// GL returns bottom-row-first; flip to top-row-first for the caller.
	const size_t rowBytes = static_cast<size_t>(w) * 4;
	std::vector<uint8_t> tmp(rowBytes);
	for (int y = 0; y < h / 2; ++y)
	{
		uint8_t* top = rgba.data() + static_cast<size_t>(y) * rowBytes;
		uint8_t* bot = rgba.data() + static_cast<size_t>(h - 1 - y) * rowBytes;
		std::memcpy(tmp.data(), top, rowBytes);
		std::memcpy(top, bot, rowBytes);
		std::memcpy(bot, tmp.data(), rowBytes);
	}

	width  = static_cast<uint32_t>(w);
	height = static_cast<uint32_t>(h);
	return true;
}

// ─── One still from somebody else's camera ────────────────────────────────────
// The contract is in IRenderer.h; the shape is Metal's (MetalRenderer.mm,
// RenderSceneImage). GL is the simpler case: Render() is the only thing that
// touches the window's framebuffer (the clear of FBO 0, the direct-mode draw,
// the ImGui overlay callback) and the buffer swap lives in the application, so
// there is no swapchain pass to skip and no capture flag — this path just does
// not call Render(). DrawScene draws into whatever framebuffer is bound and
// its post-process chain hands the finished image back to that same binding
// (prevFBO in the pass lambda), so a fresh FBO at the requested size is the
// whole trick.
//
// The live viewport's FBO triple and size are SET ASIDE (not destroyed — they
// are handed straight back), so EnsureViewportTarget builds a fresh pair at the
// requested size; one DrawScene fills it; CaptureViewport reads it back; the
// pair is destroyed (its colour texture goes the retired way, like every
// viewport texture) and the live triple restored. GetViewportTexture never
// answers with the screenshot's texture, so the UI built later this frame
// shows the editor's own view, not the client's.
//
// Not set aside: the intermediate targets (HDR, LDR, G-buffer, TAA, SSR
// history) — DrawScene resizes them to the request and the next real frame
// resizes them back. Not run: the retire ageing, the GPU particle step and
// the profiler's GPU-timer frame — those count in REAL frames, and a still on
// request is not one of those (the GpuPassScopes inside DrawScene are no-ops
// outside a timer frame). Per-request path, not per-frame.
bool OpenGLRenderer::RenderSceneImage(const EditorCameraOverride& camera, uint32_t width,
                                      uint32_t height, std::vector<uint8_t>& rgba)
{
	if (!m_primarySdlWindow || !m_glContext) return false;
	if (width == 0 || height == 0) return false;
	SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	// Everything the frame reads that the request changes, saved by value.
	const unsigned int         liveFBO   = m_viewportFBO;
	const unsigned int         liveColor = m_viewportColor;
	const unsigned int         liveDepth = m_viewportDepth;
	const int                  liveW     = m_viewportW;
	const int                  liveH     = m_viewportH;
	const uint32_t             liveReqW  = m_viewportReqW;
	const uint32_t             liveReqH  = m_viewportReqH;
	const EditorCameraOverride liveCam   = m_editorCamera;
	// The counters are the profiler's picture of the last real frame; the
	// screenshot's draws would sit there until the next Render() otherwise.
	const FrameCounters        liveCounters = m_counters;

	m_viewportFBO   = 0;
	m_viewportColor = 0;
	m_viewportDepth = 0;
	m_viewportW     = 0;
	m_viewportH     = 0;
	m_viewportReqW  = width;
	m_viewportReqH  = height;
	m_editorCamera  = camera;
	// TAA history is one camera's past. The screenshot must not blend against
	// the viewport's (a request at exactly the viewport's size would reuse the
	// targets, so the resize rule alone does not cover it), and the viewport
	// must not blend against the screenshot's afterwards. Both frames take the
	// current image only — an unconverged edge for one frame, no ghost.
	m_taaHistoryValid = false;

	// Same three lines as the offscreen branch of Render().
	EnsureViewportTarget();
	glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	DrawScene(m_viewportW, m_viewportH);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// glReadPixels waits for the GPU; no world → the cleared target, honestly
	// black, which is what the viewport would show too.
	uint32_t   gotW = 0, gotH = 0;
	const bool ok   = CaptureViewport(rgba, gotW, gotH) && gotW == width && gotH == height;

	// The screenshot pair goes the way every viewport target goes (the colour
	// texture is retired, not deleted — one release path, not two). The live
	// triple comes straight back.
	DestroyViewportTarget();
	m_viewportFBO   = liveFBO;
	m_viewportColor = liveColor;
	m_viewportDepth = liveDepth;
	m_viewportW     = liveW;
	m_viewportH     = liveH;
	m_viewportReqW  = liveReqW;
	m_viewportReqH  = liveReqH;
	m_editorCamera  = liveCam;
	m_counters      = liveCounters;
	// DrawScene's TAA resolve marks the history valid again — that history is
	// the screenshot camera's now, and the next real frame must not use it.
	m_taaHistoryValid = false;

	if (!ok) rgba.clear();
	return ok;
}

void OpenGLRenderer::EnsureViewportTarget()
{
	const int w = static_cast<int>(m_viewportReqW);
	const int h = static_cast<int>(m_viewportReqH);
	if (m_viewportFBO && w == m_viewportW && h == m_viewportH)
		return;

	DestroyViewportTarget();

	glGenFramebuffers(1, &m_viewportFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);

	glGenTextures(1, &m_viewportColor);
	glBindTexture(GL_TEXTURE_2D, m_viewportColor);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_viewportColor, 0);

	glGenRenderbuffers(1, &m_viewportDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_viewportDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_viewportDepth);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		HE_LOG_ERROR(RHI, "%s", "OpenGLRenderer: viewport FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	m_viewportW = w;
	m_viewportH = h;
}

void OpenGLRenderer::AgeRetiredTextures()
{
	for (auto it = m_retiredTextures.begin(); it != m_retiredTextures.end(); )
	{
		if (--it->framesLeft <= 0)
		{
			glDeleteTextures(1, &it->texture);
			it = m_retiredTextures.erase(it);
		}
		else
			++it;
	}
}

void OpenGLRenderer::DestroyViewportTarget()
{
	if (m_viewportFBO)   { glDeleteFramebuffers(1, &m_viewportFBO);   m_viewportFBO = 0; }
	if (m_viewportColor)
	{
		// Deferred — this frame's ImGui draw list still references the id
		m_retiredTextures.push_back({ m_viewportColor, 3 });
		m_viewportColor = 0;
	}
	if (m_viewportDepth) { glDeleteRenderbuffers(1, &m_viewportDepth);m_viewportDepth = 0; }
	m_viewportW = m_viewportH = 0;
}

// Per-frame light window + CSM/local-shadow uniforms for ONE of the three scene
// programs. The unlit, instanced and skinned programs are linked from the same
// shared kUnlitFS text, so they declare byte-identical uniforms and only the
// location integers differ — this used to be three verbatim copies of the fill
// loop inside DrawScene (which had already drifted apart in indentation).
// The matching sampler TEXTURE BINDS stay at the call sites: they are shared
// state that only some of the three re-assert, each for its own documented
// reason. The caller must have the target program bound.
void OpenGLRenderer::BindSceneLighting(const SceneLightingLocs& L, const SceneShadowFrame& F) const
{
	// Lights (clamped to the shader's MAX_LIGHTS, which IS the engine's shared
	// light window — a local `constexpr int kMaxLights = 8` here could drift away
	// from it silently).
	using HE::kMaxLightWindow;
	const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), kMaxLightWindow);
	glm::vec4 pos[kMaxLightWindow], dir[kMaxLightWindow], color[kMaxLightWindow], params[kMaxLightWindow];
	for (int i = 0; i < count; ++i)
	{
		const LightData& l = m_renderWorld.lights[i];
		pos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
		dir[i]    = glm::vec4(l.direction, l.spotAngleCos);
		color[i]  = glm::vec4(l.color,     l.intensity);
		// y = local shadow atlas base layer (-1 = no shadow); forced off
		// when the atlas texture is unavailable this frame.
		params[i] = glm::vec4(l.range,
		                      F.localShadows ? static_cast<float>(l.shadowLayer) : -1.0f,
		                      0.0f, 0.0f);
	}
	glUniform1i(L.lightCount, count);
	if (count > 0)
	{
		glUniform4fv(L.lightPos,    count, glm::value_ptr(pos[0]));
		glUniform4fv(L.lightDir,    count, glm::value_ptr(dir[0]));
		glUniform4fv(L.lightColor,  count, glm::value_ptr(color[0]));
		glUniform4fv(L.lightParams, count, glm::value_ptr(params[0]));
	}
	glUniform3fv(L.cameraPos, 1, glm::value_ptr(m_renderWorld.camera.position));

	// CSM: the sampling is gated by uShadowEnabled, the per-cascade
	// matrices/splits/forward drive the cascade selection.
	glUniform1i(L.shadowEnabled, F.shadows ? 1 : 0);
	glUniform1i(L.shadowDebug,   m_debugShadowCascades ? 1 : 0);
	glUniform1i(L.unlit,         UnlitViewActive() ? 1 : 0);
	glUniformMatrix4fv(L.cascadeVP, kGLCsmCascades, GL_FALSE, F.cascadeVPData);
	glUniform4fv(L.cascadeSplits, 1, glm::value_ptr(F.cascadeSplits));
	glUniform3fv(L.cameraFwd, 1, glm::value_ptr(F.cameraFwd));
	glUniform2f(L.shadowBias, m_shadowSettings.slopeBias, m_shadowSettings.minBias);
	glUniform1i(L.shadowMap, 1);
	// Local (point/spot) shadow atlas on unit 11 — the sampler is always assigned
	// (sampling is gated per light by uLightParams[i].y), the matrices only when
	// there are any layers.
	glUniform1i(L.localShadowMap, 11);
	if (F.localShadows)
		glUniformMatrix4fv(L.localShadowVP, F.nLocalLayers, GL_FALSE, F.localVPData);
}

// The procedural sky, as a fullscreen pass into the CURRENTLY BOUND target.
// Lifted out of the frame so the world PREVIEW can draw the very same sky.
//
// A cheaper stand-in was tried first — a coarse dome coloured per vertex from
// SkyColorCPU — and it looked like a cheaper stand-in: the sun disc is a
// fraction of a degree wide, so interpolated across triangles it became a huge
// faceted white polygon. Nothing short of the per-pixel shader resolves it.
// There is one sky in this engine, and this is it.
//
// The caller owns the target, the depth state it was left in, and the viewport;
// this restores GL_LESS + depth writes on the way out, as the frame expects.
// `allowLowResClouds` is false for previews: the quarter-res pre-pass rebinds
// its own FBO and viewport, which a small offscreen render has no business
// doing mid-pass.
void OpenGLRenderer::DrawSkyFullscreen(const glm::mat4& invViewProj, const glm::vec3& sunDir,
                                       const glm::vec3& camPos,
                                       const IRenderer::EnvironmentSettings& env,
                                       bool allowLowResClouds, int pw, int ph)
{
	if (!m_skyProgram || !env.skyEnabled) return;

	glUseProgram(m_skyProgram);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(invViewProj));
	glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(sunDir));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_moonTex);
	glUniform1i(m_uSkyMoonTex, 0);
	glUniform1i(m_uSkyHasMoon, m_moonTex ? 1 : 0);
	glUniform1f(m_uSkyMoonPhase, env.moonPhase);
	glUniform1f(m_uSkyTime, env.timeOfDay);
	glUniform1f(m_uSkyCoverage, env.cloudCoverage);
	// Cloud render mode + 3D-cloud parallax inputs (camera world pos + layer height).
	glUniform1i(m_uSkyCloudMode,   env.cloudMode);
	glUniform1i(m_uSkyCloudQuality, env.cloudQuality);
	glUniform1i(m_uSkyCloudStyle,        env.cloudStyle);
	glUniform1i(m_uSkyCloudInterShadows, env.cloudInterShadows ? 1 : 0);
	glUniform1f(m_uSkyCloudEvolution,    env.cloudEvolution);
	glUniform3fv(m_uSkyCameraPos,  1, glm::value_ptr(camPos));
	glUniform1f(m_uSkyCloudHeight, env.cloudHeight);
	glUniform1f(m_uSkyCloudDensity,    env.cloudDensity);
	glUniform1f(m_uSkyCloudFluffiness, env.cloudFluffiness);
	glUniform3fv(m_uSkyCloudTint, 1, glm::value_ptr(env.cloudTint));
	glUniform1f(m_uSkyContrails,  env.contrailAmount);
	glUniform1f(m_uSkyCirrus,     env.cirrusAmount);
	glUniform1f(m_uSkyCirrusSeed, env.cirrusSeed);
	glUniform1f(m_uSkyStarBright, env.starBrightness);
	glUniform3fv(m_uSkyStarColor, 1, glm::value_ptr(env.starColor));
	glUniform1f(m_uSkyStarSize,    env.starSize);
	glUniform1f(m_uSkyStarSizeVar, env.starSizeVariation);
	glUniform1f(m_uSkyStarDensity, env.starDensity);
	glUniform1f(m_uSkyStarGlow,    env.starGlow);
	glUniform1f(m_uSkyStarTwinkle, env.starTwinkle);
	glUniform1f(m_uSkyRainAmount,  env.rainAmount);
	glUniform1f(m_uSkyGodRays,     env.godRays);
	glUniform1f(m_uSkyShootingStars, env.shootingStars);
	// HE_SKY_TIME overrides the animation clock (for deterministic headless capture
	// of time-animated sky elements like the aurora); normal runs use the wall clock.
	float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
	if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
	glUniform1f(m_uSkyClock, skyClock);
	glUniform3fv(m_uSkySunColor, 1, glm::value_ptr(env.sunColor));
	glUniform1f(m_uSkyAurora, env.auroraIntensity);
	glUniform1f(m_uSkyAuroraHeight,   env.auroraHeight);
	glUniform1f(m_uSkyAuroraFragment, env.auroraFragmentation);
	glUniform1f(m_uSkyMilkyWay, env.milkyWayIntensity);
	glUniform1f(m_uSkyNebula, env.nebulaIntensity);
	glUniform3fv(m_uSkyNebulaColor, 1, glm::value_ptr(env.nebulaColor));
	glUniform3fv(m_uSkyNebulaColor2, 1, glm::value_ptr(env.nebulaColor2));
	glUniform3fv(m_uSkyNebulaColor3, 1, glm::value_ptr(env.nebulaColor3));
	glUniform1f(m_uSkyNebulaSeed, env.nebulaSeed);
	glUniform1f(m_uSkyNebulaHiFi, (float)env.nebulaQuality); // 0/1/2 (carried in uNebulaHiFi)
	glUniform1f(m_uSkyNebulaCover, env.nebulaCoverage);
	glUniform3fv(m_uSkyAuroraColor, 1, glm::value_ptr(env.auroraColor));
	glUniform3fv(m_uSkyAuroraColorTop, 1, glm::value_ptr(env.auroraColorTop));
	glUniform1f(m_uSkyFlash, env.flash);
	{
		// Wind control → horizontal cloud drift vector. Direction 0° drifts
		// toward -Z (north), increasing clockwise; speed scales the rate.
		const glm::vec3 wind = HE::CloudWindVector(env);
		glUniform3fv(m_uSkyWind, 1, glm::value_ptr(wind));
	}
	glActiveTexture(GL_TEXTURE2);             // 3D value-noise on unit 2
	glBindTexture(GL_TEXTURE_3D, m_noiseTex);
	glUniform1i(m_uSkyNoise, 2);
	// ── Low-res clouds: quarter-res clouds-only pre-pass (previous-frame camera so
	// it can run without re-extracting; 1-frame lag is imperceptible on soft clouds).
	const bool lowRes = allowLowResClouds && env.lowResClouds && env.cloudCoverage > 0.0f;
	if (lowRes)
	{
		GLint prevFBO = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
		EnsureCloudTarget(std::max(1, pw / 2), std::max(1, ph / 2));
		// Render the pre-pass with the CURRENT camera (this backend draws the sky after
		// extraction, so the current view is available inline). Compositing at the current
		// screen UV then lines the clouds up 1:1 with the sky — no lag/swim when panning.
		// (uInvVP/uSunDir are already the current values here; set explicitly for clarity.)
		glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(invViewProj));
		glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(sunDir));
		glUniform1f(m_uSkyCloudPrepass, 1.0f);
		glUniform1f(m_uSkyLowResClouds, 0.0f);
		glBindFramebuffer(GL_FRAMEBUFFER, m_cloudFBO);
		glViewport(0, 0, m_cloudW, m_cloudH);
		glDisable(GL_DEPTH_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);    // L=0, T=1 (clear sky)
		glClear(GL_COLOR_BUFFER_BIT);
		glBindVertexArray(m_fsVAO);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glEnable(GL_DEPTH_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
		glViewport(0, 0, pw, ph);
		glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(invViewProj)); // restore this frame
		glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(sunDir));
	}
	glUniform1f(m_uSkyCloudPrepass, 0.0f);
	glUniform1f(m_uSkyLowResClouds, (lowRes && m_cloudTex) ? 1.0f : 0.0f);
	glActiveTexture(GL_TEXTURE3);            // quarter-res cloud buffer on unit 3
	glBindTexture(GL_TEXTURE_2D, m_cloudTex);
	glUniform1i(m_uSkyCloudTex, 3);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(m_fsVAO);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDepthFunc(GL_LESS);   // restore default for the next pass
	glDepthMask(GL_TRUE);
}

void OpenGLRenderer::DrawScene(int pw, int ph)
{
	// Reset the render counters before the early-return guards so a non-rendered
	// frame (no world / zero-size) honestly reports zeros, not last frame's values.
	// total/visible are filled after the cull below; draws/tris at the draw sites.
	m_counters = FrameCounters{};
	if (!m_world) return;
	if (pw <= 0 || ph <= 0) return;
	glViewport(0, 0, pw, ph);

	// Drop GPU textures for materials edited/re-assigned since last frame so the
	// loop below re-resolves them (deferred here where the GL context is current).
	for (const HE::UUID& id : m_pendingMaterialInvalidations)
		if (auto it = m_materialTexCache.find(id); it != m_materialTexCache.end())
		{
			if (it->second) glDeleteTextures(1, &it->second);
			m_materialTexCache.erase(it);
		}
	m_pendingMaterialInvalidations.clear();

	for (const HE::UUID& id : m_pendingMeshInvalidations)
	{
		if (auto it = m_meshCache.find(id); it != m_meshCache.end())
		{
			auto& m = it->second;
			if (m.vao) glDeleteVertexArrays(1, &m.vao);
			if (m.vbo) glDeleteBuffers(1, &m.vbo);
			if (m.ebo) glDeleteBuffers(1, &m.ebo);
			m_meshCache.erase(it);
		}
		// GI BLAS ranges live in concatenated arrays — a single mesh can't be
		// spliced out cheaply, so an edited mesh drops the whole cache; it
		// rebuilds lazily on the next GI-active frame (mirrors Metal's per-mesh
		// BLAS release, just coarser).
		if (m_giBlasCache.count(id))
		{
			m_giBlasCache.clear();
			m_giNodesCpu.clear();
			m_giTrisCpu.clear();
			m_giBlasDirty = true;
		}
	}
	// A rebuilt mesh (sculpt, terrain LOD/tessellation) can change the scene
	// box without changing which objects exist → re-check the probe grid fit.
	if (!m_pendingMeshInvalidations.empty()) m_giGridTrack.meshRebuilt = true;
	m_pendingMeshInvalidations.clear();

	// Textures rewritten in place (landscape weightmap paints) drop their cached
	// GL texture so the next ResolveGraphTexture re-uploads the new pixels.
	for (const HE::UUID& id : m_pendingTexInvalidations)
	{
		const std::string key = std::to_string(id.hi) + ":" + std::to_string(id.lo);
		if (auto it = m_graphTexCache.find(key); it != m_graphTexCache.end())
		{
			if (it->second) glDeleteTextures(1, &it->second);
			m_graphTexCache.erase(it);
		}
	}
	m_pendingTexInvalidations.clear();

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	// Project shadow settings (SetShadowSettings): a changed resolution
	// re-specifies the cascade array's storage — mutable glTexImage3D storage,
	// the per-cascade layer attach happens in the shadow pass anyway — and the
	// extractor fits its cascades against the size that is actually allocated.
	if (m_shadowSizeDirty && m_shadowDepthTex)
	{
		m_shadowSize = std::clamp(m_shadowSettings.resolution, 256, 8192);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
		             m_shadowSize, m_shadowSize, kGLCsmCascades,
		             0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		m_shadowSizeDirty = false;
	}
	m_extractor.setShadowSettings(m_shadowSettings.distance, m_shadowSettings.cascadeCount,
	                              m_shadowSettings.splitLambda, m_shadowSize);
	m_extractor.setContentManager(m_contentManager);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(pw) / static_cast<float>(ph),
	                    &m_editorCamera);
	m_extractor.extractUI(*m_world, static_cast<float>(pw), static_cast<float>(ph),
	                      m_renderWorld);

	// GI acceleration structures (GL 4.3 compute port, Checkpoint GL-A): CPU
	// BLAS build + SSBO upload from THIS extraction — no-op unless GI is on and
	// the context is 4.3+. Nothing samples the buffers yet (GL-B adds the
	// shadow-ray kernel), so GI-off rendering stays byte-identical.
	UpdateGIAccel();
	// NB: do NOT early-out when there are no (visible) objects — the skybox is the
	// background and must still be drawn, or the viewport falls back to a stale
	// gray clear when the camera looks away from the scene.

	// ── TAA: this frame's jitter (A2) ───────────────────────────────────────
	// Halton(2,3), 8 positions: a low-discrepancy sequence covers the pixel
	// evenly in few frames, where a random offset clumps and a regular grid
	// re-aliases. Chosen BEFORE anything builds a matrix, because every
	// rasterising pass this frame must share one offset.
	if (TaaActive())
	{
		EnsureTaaTargets(pw, ph);
		auto halton = [](uint32_t i, uint32_t base) {
			float f = 1.0f, r = 0.0f;
			while (i > 0) { f /= static_cast<float>(base); r += f * (i % base); i /= base; }
			return r;
		};
		const uint32_t n = (m_taaFrameIndex % 8u) + 1u;
		m_taaJitter = glm::vec2(halton(n, 2) - 0.5f, halton(n, 3) - 0.5f);
		++m_taaFrameIndex;
	}
	else if (m_taaHistoryTex[0])
	{
		// Freed as soon as the mode is off, and the history is dropped with
		// it — a stale one would blend against a different world the moment
		// TAA comes back on.
		DestroyTaaTargets();
		m_taaJitter = glm::vec2(0.0f);
	}

	// Two view-projections, one rule (docs/anti-aliasing-plan.md): the CLEAN
	// one measures — velocity, the motion-blur/SSR/GI reprojections, the SSAO
	// sample projection — and the JITTERED one rasterises everything that
	// lands in the image: geometry, G-buffer, decals, the deferred resolve's
	// reconstruction, sky, transparency, particles, debug lines. With TAA off
	// the two are the same matrix.
	const glm::mat4 viewProjClean = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 viewProj      = JitteredViewProj(viewProjClean, pw, ph);
	const glm::mat4 invViewProj   = glm::inverse(viewProj);

	// Direction toward the sun for the sky + image-based ambient — resolved by the
	// extractor (scene directional light, or the day-night cycle when enabled).
	const glm::vec3 sunDir = m_renderWorld.sunDirection;

	// ── Refine bounds with real mesh AABBs (also uploads new meshes) ────────
	for (RenderObject& obj : m_renderWorld.objects)
		if (const GpuMesh* mesh = ResolveMesh(obj.meshAssetId);
		    mesh && mesh->localBounds.isValid())
			obj.worldBounds = mesh->localBounds.transformed(obj.transform);

	// ── Cull → sort → submit ────────────────────────────────────────────────
	m_culler.cull(m_renderWorld, m_visible);
	// Occlusion (off by default): drops what the frustum kept but a nearer
	// opaque surface hides — the same m_visible the sort and every camera pass
	// consume, so the SSAO/GI pre-passes and the deferred G-buffer follow suit.
	// The shadow pass below culls into its own m_shadowVisible and is untouched.
	m_counters.occlusionCulled =
		m_occlusionCuller.refine(m_renderWorld, m_contentManager, m_visible);
	m_sorter.sort(m_renderWorld, m_visible, m_sortedIndices);
	// (no early-out on empty: the geometry pass still draws the skybox background
	// and the post-process still tonemaps it, even with zero visible objects.)

	// Profiler render counters: draws/tris are tallied at the draw sites below;
	// visible/total come from the cull result vs the extracted set (already reset
	// to zero at the top of DrawScene).
	m_counters.total   = static_cast<uint32_t>(m_renderWorld.objects.size());
	m_counters.visible = static_cast<uint32_t>(m_sortedIndices.size());

	// Snapshot the active target (window or editor-viewport FBO) so the shadow
	// pass can render into the shadow map and then restore it for the main pass.
	GLint prevFBO = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

	// Cloud-shadow map: rendered BEFORE the opaque pass (the lit shaders sample
	// it on unit 19). Restores the snapshotted target like the shadow pass.
	RenderCloudShadowMap();
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
	glViewport(0, 0, pw, ph);

	const bool shadows = m_renderWorld.shadow.enabled && m_shadowFBO != 0;
	// Local (point/spot) shadow atlas — independent of the directional CSM, so
	// night scenes with only shadow-casting point lights still get their maps.
	const int  nLocalLayers = std::clamp(m_renderWorld.shadow.localLayerCount, 0,
	                                     ShadowData::kMaxLocalShadowLayers);
	const bool localShadows = nLocalLayers > 0 && m_shadowFBO != 0 && m_localShadowDepthTex != 0;
	const float* localVPData = glm::value_ptr(m_renderWorld.shadow.localViewProj[0]);

	// ── Cascaded shadow map uniforms (shared across unlit/skinned/instanced) ──
	// World forward (−Z of the camera-to-world matrix) for planar view-Z cascade
	// selection — MUST match the planar splits the cascades were fit with (same
	// formula as the extractor). The cascade matrices arrive in GL clip space, so
	// they're uploaded as-is (no clip-fix, unlike Metal).
	const ShadowData& sh = m_renderWorld.shadow;
	const int   nCascades = std::clamp(sh.cascadeCount, 0, kGLCsmCascades);
	const glm::vec3 camFwd =
		-glm::normalize(glm::vec3(glm::inverse(m_renderWorld.camera.view)[2]));
	const glm::vec4 cascadeSplits(
		nCascades > 0 ? sh.cascadeSplit[0] : 1e9f,
		nCascades > 1 ? sh.cascadeSplit[1] : 1e9f,
		nCascades > 2 ? sh.cascadeSplit[2] : 1e9f,
		static_cast<float>(nCascades));
	// Upload kGLCsmCascades matrices (contiguous in ShadowData); unused tail slots
	// are identity and never sampled (the shader clamps the cascade index to count).
	const float* cascadeVPData = glm::value_ptr(sh.cascadeViewProj[0]);
	// Everything BindSceneLighting needs that is not renderer state — built once,
	// pushed onto each of the three scene programs below.
	const SceneShadowFrame shadowFrame{ cascadeVPData, cascadeSplits, camFwd,
	                                    localVPData, nLocalLayers, shadows, localShadows };

	// ShadowPass → depth map; GeometryPass → HDR scene color; PostProcessPass
	// tonemaps that into the backbuffer/viewport.
	if (m_renderGraph.empty())
	{
		m_renderGraph.addPass(std::make_unique<ShadowPass>());
		m_renderGraph.addPass(std::make_unique<GeometryPass>());
		m_renderGraph.addPass(std::make_unique<PostProcessPass>());
	}

	EnsureHDRTarget(pw, ph);

	m_renderGraph.execute(m_renderWorld, m_sortedIndices,
		[&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
	{
		// ── Shadow pass: cascaded depth maps from the light's POV ───────────
		// One depth render per cascade into its own array layer. Each cascade
		// re-culls casters against ITS light frustum (not the camera), so an
		// off-screen object still casts into the visible scene while it sits
		// inside the cascade coverage. Mirrors the Metal backend's EncodeShadowMap.
		if (io.output.id == kShadowMapTarget)
		{
			if (!shadows && !localShadows) return;
			GpuPassScope _shadowTimer(this, "Shadow"); // ends (glEndQuery) at this branch's return
			glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
			glViewport(0, 0, m_shadowSize, m_shadowSize);
			glUseProgram(m_depthProgram);
			// Push depth values away from the light camera so shadow-map samples
			// for back-lit fragments never self-shadow the surface (shadow acne).
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.0f, 4.0f);

			// Depth-only render of every shadow caster into one layer of `target` —
			// shared by the CSM cascades and the local (point/spot) shadow views.
			// Cull + sort against the view's light frustum into scratch buffers
			// (NOT m_visible/m_sortedIndices — those hold the camera cull the
			// geometry pass consumes). Sorting keeps draws grouped by mesh id so
			// the per-mesh resolve memoisation stays valid.
			// `skipEntity` keeps ONE entity's geometry out of this layer: the entity
			// the local light itself sits on. Without it a light authored onto a mesh
			// entity renders that mesh at z≈0 into its own depth map and shadows
			// itself out completely. kNoOwnerEntity (every cascade) skips nothing.
			// Mirrors the Metal backend's encodeDepthLayer.
			auto renderDepthLayer = [&](unsigned int target, int layer, const glm::mat4& vp,
			                            uint32_t skipEntity)
			{
				glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, 0, layer);
				glClear(GL_DEPTH_BUFFER_BIT);
				m_culler.cull(m_renderWorld, vp, m_shadowVisible);
				m_sorter.sort(m_renderWorld, m_shadowVisible, m_shadowSorted);
				// Same-mesh runs → one instanced draw each. A run of one keeps the
				// plain depth program (no instance upload for a single caster).
				RenderSorter::batchDepthCasters(m_renderWorld, m_shadowSorted, skipEntity,
				                                m_shadowBatches);
				const bool canInstance = m_depthInstancedProgram && m_instanceVBO;
				unsigned int boundProgram = m_depthProgram; // glUseProgram'd by the caller
				for (const RenderSorter::DepthBatch& b : m_shadowBatches.batches)
				{
					const GpuMesh* mesh = ResolveMesh(b.meshAssetId);
					if (!mesh) mesh = ResolveMesh(HE::kDefaultCubeMeshId);
					if (!mesh) continue;
					const glm::mat4* xf = m_shadowBatches.transforms.data() + b.first;
					glBindVertexArray(mesh->vao);
					if (b.count > 1 && canInstance)
					{
						// Scene-pass convention: orphan the scratch VBO per batch.
						// Every mesh VAO reads attribs 4–7 from it with divisor 1.
						glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
						glBufferData(GL_ARRAY_BUFFER,
						             static_cast<GLsizeiptr>(b.count * sizeof(glm::mat4)),
						             xf, GL_STREAM_DRAW);
						glBindBuffer(GL_ARRAY_BUFFER, 0);
						if (boundProgram != m_depthInstancedProgram)
						{
							glUseProgram(m_depthInstancedProgram);
							boundProgram = m_depthInstancedProgram;
							glUniformMatrix4fv(m_uDepthInstVP, 1, GL_FALSE, glm::value_ptr(vp));
						}
						glDrawElementsInstanced(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT,
						                        nullptr, static_cast<GLsizei>(b.count));
						static bool loggedOnce = false; // once per session, like the Metal twin
						if (!loggedOnce)
						{
							loggedOnce = true;
							HE_LOG_INFO(RHI, "OpenGLRenderer: shadow pass instanced (first run: %u casters)",
							            static_cast<unsigned>(b.count));
						}
						continue;
					}
					if (boundProgram != m_depthProgram)
					{
						glUseProgram(m_depthProgram);
						boundProgram = m_depthProgram;
					}
					for (uint32_t k = 0; k < b.count; ++k)
					{
						glUniformMatrix4fv(m_uDepthMVP, 1, GL_FALSE, glm::value_ptr(vp * xf[k]));
						glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
					}
				}
				// Leave the plain depth program bound: the next layer's lambda
				// entry assumes it, like the caller's glUseProgram above.
				if (boundProgram != m_depthProgram) glUseProgram(m_depthProgram);
			};

			if (shadows)
			{
				const int cascades = std::clamp(m_renderWorld.shadow.cascadeCount, 1, kGLCsmCascades);
				for (int c = 0; c < cascades; ++c)
					renderDepthLayer(m_shadowDepthTex, c, m_renderWorld.shadow.cascadeViewProj[c],
					                 kNoOwnerEntity);
			}
			if (localShadows)
			{
				glViewport(0, 0, m_localShadowSize, m_localShadowSize);
				for (int v = 0; v < nLocalLayers; ++v)
					renderDepthLayer(m_localShadowDepthTex, v, m_renderWorld.shadow.localViewProj[v],
					                 m_renderWorld.shadow.localOwnerEntity[v]);
			}
			glDisable(GL_POLYGON_OFFSET_FILL);
			glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
			glViewport(0, 0, pw, ph);
			return;
		}

		// ── PostProcess pass: bloom + tonemap the HDR scene color to output ─
		if (io.inputCount > 0 && io.inputs[0] == kSceneColorTarget)
		{
			glDisable(GL_DEPTH_TEST);
			glBindVertexArray(m_fsVAO);

			// Depth of field first: it rewrites the HDR image (CoC from the scene
			// depth → half-res blur → composite), and bloom + tonemap then read
			// THAT. Off → sceneHdr stays m_hdrColor and nothing here changes.
			// Sibling scope, not nested in Bloom's: GL_TIME_ELAPSED cannot nest.
			unsigned int sceneHdr = m_hdrColor;
			if (m_dofEnabled)
			{
				GpuPassScope _dofTimer(this, "DoF");
				if (const unsigned int dofTex = RenderDepthOfField(pw, ph, m_renderWorld.camera.projection))
					sceneHdr = dofTex;
			}

			// Motion blur next, on whatever DoF left: the camera's frame-to-frame
			// motion smears the image (velocity from depth + previous view-
			// projection), and bloom + tonemap read the smeared result. The
			// previous matrix is refreshed at the end of DrawScene, on or off.
			if (m_mbEnabled)
			{
				GpuPassScope _mbTimer(this, "MotionBlur");
				if (const unsigned int mbTex = RenderMotionBlur(sceneHdr, pw, ph,
				                                                m_renderWorld.camera.view,
				                                                m_renderWorld.camera.projection))
					sceneHdr = mbTex;
			}

			// Bright-pass + blur the HDR target into the half-res bloom buffer
			// (skipped when bloom is disabled → strength 0 below).
			unsigned int bloomTex = 0u;
			if (m_bloomEnabled)
			{
				GpuPassScope _bloomTimer(this, "Bloom");
				bloomTex = RenderBloom(sceneHdr, pw, ph);
			}

			// Tonemap HDR scene color + bloom into the LDR intermediate (FXAA reads it).
			{
				GpuPassScope _tonemapTimer(this, "Tonemap");
				EnsureLdrTarget(pw, ph);
				glBindFramebuffer(GL_FRAMEBUFFER, m_ldrFBO);
				glViewport(0, 0, pw, ph);
				glUseProgram(m_tonemapProgram);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, sceneHdr);
				glUniform1i(m_uHDRTex, 0);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, bloomTex);
				glUniform1i(m_uBloomTex, 1);
				glUniform1f(m_uExposure, 1.0f);
				glUniform1f(m_uBloomStrength, bloomTex ? m_bloomStrength : 0.0f);
				// Camera lens flare: project the sun (a point at infinity, w=0 drops the view
				// translation) to y-up NDC and fold behind-camera / off-screen / below-horizon
				// into a single strength. Matches the Metal CPU path; the shader is byte-identical.
				{
					const float lfAmt = GetEnvironment().lensFlare;
					glm::vec3 sd = glm::normalize(sunDir);
					glm::vec4 clip = viewProj * glm::vec4(sd, 0.0f);
					glm::vec2 sunNDC(0.0f);
					float strength = 0.0f;
					if (lfAmt > 0.0f && clip.w > 1e-4f)
					{
						sunNDC = glm::vec2(clip) / clip.w;
						const float onScreen = 1.0f - glm::smoothstep(1.0f, 1.7f, glm::length(sunNDC));
						const float horizon  = glm::smoothstep(-0.02f, 0.10f, sd.y);
						strength = lfAmt * onScreen * horizon;
					}
					const float aspect = (ph > 0) ? (float)pw / (float)ph : 1.0f;
					glUniform4f(m_uLensFlare, sunNDC.x, sunNDC.y, aspect, strength);
				}
				glActiveTexture(GL_TEXTURE0);
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}

			// Temporal accumulation on the tonemapped image (A3). 0 unless TAA
			// is the active mode; the AA-resolve slot below then reads the
			// resolved history target instead of m_ldrColor. Sibling scope.
			unsigned int taaTex = 0u;
			if (TaaActive())
			{
				GpuPassScope _taaTimer(this, "TAA");
				taaTex = RenderTaa(pw, ph);
			}

			// AA resolve: move the tonemapped LDR image into the actual output.
			// The pass ALWAYS runs — it is what fills the output target; the AA
			// method only decides which program does it. TAA already produced
			// the finished image in its own pass; its slot only moves it to the
			// target, with the sharpen the temporal blur asks for.
			{
				const bool aaTaa  = taaTex != 0;
				const bool aaOff  = (m_aaMethod == HE::AAMethod::Off);
				const bool aaSmaa = (m_aaMethod == HE::AAMethod::SMAA);
				GpuPassScope _fxaaTimer(this, aaTaa ? "TAA Sharpen"
				                            : aaOff ? "AA Resolve" : (aaSmaa ? "SMAA" : "FXAA"));
				glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
				glViewport(0, 0, pw, ph);
				glUseProgram(aaTaa ? m_taaSharpenProgram
				           : aaOff ? m_blitProgram : (aaSmaa ? m_smaaProgram : m_fxaaProgram));
				glBindTexture(GL_TEXTURE_2D, aaTaa ? taaTex : m_ldrColor);
				if (aaTaa)
				{
					glUniform1i(m_uTaaSharpScene, 0);
					glUniform4f(m_uTaaSharpParams, 1.0f / float(pw), 1.0f / float(ph),
					            m_aaSharpness, 0.0f);
				}
				else if (aaOff)
				{
					glUniform1i(m_uBlitScene, 0);
				}
				else if (aaSmaa)
				{
					glUniform1i(m_uSmaaScene, 0);
					glUniform2f(m_uSmaaRcpFrame, 1.0f / float(pw), 1.0f / float(ph));
				}
				else
				{
					glUniform1i(m_uFxaaScene, 0);
					glUniform2f(m_uFxaaRcpFrame, 1.0f / float(pw), 1.0f / float(ph));
				}
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}

			{
				GpuPassScope _uiTimer(this, "UI");
				RenderUIPass(pw, ph);
			}

			glEnable(GL_DEPTH_TEST);
			return;
		}

		// ── Ray-traced GI (compute): shadow mask + probe update + reflections ─
		// Replaces CSM sampling AND SSAO/IBL-ambient in the scene shader when
		// active. Uses THIS extraction's camera/draw set (Metal aspect lesson:
		// mask and scene pass must share one camera). Half-res like Metal.
		// The shared half-res pre-pass runs when EITHER consumer wants it: GI
		// reflections are their own toggle and work with the diffuse GI off.
		unsigned int giShadowTex = 0u;
		unsigned int giReflTex   = 0u;
		const int  giW = std::max(1, pw / 2), giH = std::max(1, ph / 2);
		// GAP vs Metal — the tier's RESOLUTION axis is Metal-only for now. Two
		// things pin the GL trace to the prepass size: every reflection target is
		// allocated inside EnsureGIShadowTargets (there is no EnsureGIReflTarget
		// analogue), and the kernel reads that same half-res prepass for position,
		// normal and roughness, so tracing finer would buy no geometric detail
		// anyway. Dispatching at a tier resolution the targets were never resized
		// to writes outside them: at quarter res only a quarter of the texture is
		// filled and the rest is stale, at full res one screen quadrant is
		// stretched over everything. Closing this needs the prepass resized too,
		// which is shared with the GI shadow pass — its own change, and one that
		// cannot be verified on this machine (no GL display). GL's tier therefore
		// varies rays + blur only.
		const bool giAnyActive =
			(m_giEnabled || m_giReflEnabled) && m_giSupported && m_giInstanceCount > 0;
		bool giPrepassOk = false;
		// Sibling (not nested) profiler scopes — GL_TIME_ELAPSED cannot nest, see
		// GpuTimerBeginPass. The pre-pass is its own row because it is the shared
		// cost: it runs for the shadow mask, the reflections, or both.
		// Clean matrices throughout the GI chain: the temporal passes reproject
		// with last frame's view-proj, and a jittered "previous" would read
		// the jitter as motion (same rule as the SSAO/SSR pre-pass below).
		if (giAnyActive)
		{
			GpuPassScope _giPrepassTimer(this, "GIPrepass");
			giPrepassOk = RenderGIPrepass(cmds, giW, giH, viewProjClean);
		}
		if (giPrepassOk && m_giEnabled)
		{
			GpuPassScope _giTimer(this, "GIShadow");
			giShadowTex = RenderGIShadow(giW, giH, viewProjClean);
			DispatchGIProbeUpdate();
		}
		const bool giShadingActive = giShadowTex != 0 && m_giIrrAtlas != 0 && m_giVisAtlas != 0;
		if (giPrepassOk && m_giReflEnabled)
		{
			GpuPassScope _giReflTimer(this, "GIRefl");
			giReflTex = RenderGIReflections(giW, giH, viewProjClean, giShadingActive);
		}
		// Gate for both shading paths: a valid trace result AND a non-zero
		// intensity. Off → a 1×1 transparent-black dummy is bound and the gate
		// uniform stays 0, so the cascade folds away and the image is identical
		// to the feature never having existed.
		const bool giReflActive = giReflTex != 0 && m_giReflIntensity > 0.0f;

		// ── SSAO: view-space position pre-pass → occlusion → blur ───────────
		// Computed before shading (using these same geometry draw calls) so the
		// scene shader can darken its ambient term. Skipped (zero cost) when off
		// — including when GI shades this frame (probe indirect replaces AO).
		// Deferred (docs/deferred-renderer-plan.md): decided here, before SSAO —
		// in deferred mode the AO pre-pass reads the G-buffer depth (plan P5), so
		// SSAO runs AFTER the G-buffer loop below instead of here.
		const bool deferredActive =
			m_renderPath == HE::RenderPath::Deferred && EnsureDeferredPipelines();
		// Forward SSR (docs/ssr-cross-backend-plan.md checkpoint A): the FORWARD
		// path only. The deferred composite is A6 and not built, so in deferred
		// mode the gate below stays 0 and the frame is byte-identical to SSR
		// never having existed.
		const bool ssrFrameActive =
			m_ssrEnabled && !deferredActive && EnsureSSRPrograms();
		// The reflection MRT pre-pass is the trace's only geometry input, so it
		// runs whenever SSR does — including on frames where SSAO is off or the
		// GI probes replaced it, which is exactly the case RenderSSAO used to
		// skip entirely (Metal's m_fwdReflPrepassWanted / …Only pair).
		const bool aoWanted = m_ssaoEnabled && !giShadingActive && !deferredActive;
		unsigned int aoTex = 0u;
		// Deliberately UNjittered while TAA is on (same call as Metal's
		// EncodeSSAO): the occlusion pass projects its sample positions with
		// the clean projection and reads them back from this pre-pass, so both
		// sides must agree, and the SSR trace downstream reprojects the same
		// MRT with clean matrices. The cost is a <= half-pixel offset between
		// the AO/reflection lookups and the jittered scene raster — invisible
		// under the AO blur, and the temporal filter averages the wobble away.
		if (aoWanted || ssrFrameActive)
		{
			GpuPassScope _ssaoTimer(this, "SSAO");
			aoTex = RenderSSAO(cmds, pw, ph, viewProjClean, m_renderWorld.camera.view,
			                   m_renderWorld.camera.projection, /*fromGBufferDepth=*/false,
			                   /*reflMrt=*/ssrFrameActive, aoWanted);
		}
		unsigned int ssrTex = 0u;
		if (ssrFrameActive)
		{
			GpuPassScope _ssrTimer(this, "SSR");
			ssrTex = RenderForwardSSR(pw, ph, viewProjClean);
		}
		// Same gate shape as giReflActive: a real trace result AND a non-zero
		// intensity. Off → the black dummy is bound and the cascade folds away.
		const bool ssrActive = ssrTex != 0 && m_ssrIntensity > 0.0f;

		// ── Geometry pass: scene program + per-frame state, into HDR target ─
		// Set here (not before the graph) because the shadow pass switched the
		// active program. Opaque geometry, the procedural sky/clouds and transparency
		// are timed as SIBLING GPU passes (GL_TIME_ELAPSED cannot nest — see
		// GpuTimerBeginPass), breaking the heavy sky/cloud cost out of the old single
		// "Scene" scope so it is individually measurable (matches the Metal backend's
		// "Opaque" / "Sky+Clouds" markers). The geometry branch has no early return, so
		// these explicit begin/end pairs always balance.
		GpuTimerBeginPass("Opaque");
		// ── Deferred render path: rasterize the opaque geometry into the G-buffer
		// instead of shading it here; the lighting happens once, in the fullscreen
		// resolve below (deferredActive was decided above, before the SSAO block).
		if (deferredActive)
		{
			EnsureGBufferTargets(pw, ph);
			glBindFramebuffer(GL_FRAMEBUFFER, m_gbFBO);
			glViewport(0, 0, pw, ph);
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glDepthMask(GL_TRUE);
			const float c0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const float c1[4] = { 0.5f, 0.5f, 1.0f, 0.5f }; // encoded +Z normal, mid rough/spec
			const float c2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			const float d1    = 1.0f;
			glClearBufferfv(GL_COLOR, 0, c0);
			glClearBufferfv(GL_COLOR, 1, c1);
			glClearBufferfv(GL_COLOR, 2, c2);
			glClearBufferfv(GL_DEPTH, 0, &d1);
			// GB0 is SRGB8 — enable the encode-on-write so the linear shader
			// output round-trips through the 8-bit target (disabled again after
			// the G-buffer loop; GB1/GB2 are float targets, unaffected).
			glEnable(GL_FRAMEBUFFER_SRGB);
		}
		else
		{
		glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
		glViewport(0, 0, pw, ph);
		// Explicit depth state for the opaque geometry: test on, write on, LESS, so
		// the depth clear takes and the sky (drawn last) can test against it.
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}

		// (The procedural skybox is drawn AFTER the geometry below, with a
		// depth-test == far, so the heavy sky shader only runs on the background
		// pixels the scene didn't cover.)
		glUseProgram(m_unlitProgram);
		glUniform1i(m_uTexture, 0); // base color tint (uColor) is set per draw below
		glUniform3fv(m_uSunDir, 1, glm::value_ptr(sunDir));
		// Refresh the baked skyColor ambient cubemap when the sun moved, then bind
		// it on unit 3 so the scene shader samples it instead of evaluating
		// skyColor twice per pixel.
		UpdateSkyEnvCube(sunDir);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
		glUniform1i(m_uSkyEnv, 3);
		glActiveTexture(GL_TEXTURE0);
		glUniform3fv(m_uAmbient, 1, glm::value_ptr(m_renderWorld.ambient));
		glUniform1f(m_uFogDensity,       GetEnvironment().fogDensity);
		glUniform1f(m_uFogHeightFalloff, GetEnvironment().fogHeightFalloff);
		glUniform1f(m_uWetness,          GetEnvironment().wetness);
		glUniform1f(m_uSnow,             GetEnvironment().snowAmount);
		// Specular AA: the forward scene pass owns its own normals, so the
		// widening is valid here (unlike a deferred resolve — see A6).
		glUniform1f(m_uSpecAA,           m_specularAA ? m_specularAAStrength : 0.0f);
		// SSAO occlusion on unit 4 (white fallback when off → ao = 1, no change).
		// Mutable (not const): in deferred mode SSAO runs AFTER the G-buffer loop
		// (P5, reads its depth) and updates aoTex/aoActive there — every consumer
		// below the resolve (skinned/transparency binds, fillMatLight) reads the
		// refreshed values.
		bool aoActive = m_ssaoEnabled && aoTex != 0;
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
		glUniform1i(m_uAO, 4);
		glActiveTexture(GL_TEXTURE0);
		glUniform2f(m_uViewport, static_cast<float>(pw), static_cast<float>(ph));
		glUniform1i(m_uSSAOEnabled, aoActive ? 1 : 0);
		// GI inputs on units 5/6/7 (white fallbacks keep the samplers valid for
		// every program sharing kUnlitFS when GI is off this frame).
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, giShadingActive ? giShadowTex : m_whiteTex);
		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giIrrAtlas : m_whiteTex);
		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giVisAtlas : m_whiteTex);
		// Per-pixel local (point/spot) light visibility mask on unit 8 — written
		// by the shadow kernel alongside the sun mask (white = unoccluded when off).
		glActiveTexture(GL_TEXTURE8);
		glBindTexture(GL_TEXTURE_2D, (giShadingActive && m_giLocalMaskTex) ? m_giLocalMaskTex : m_whiteTex);
		// heLitP() masks (custom materials), units 9/10: sun mask + local mask.
		glActiveTexture(GL_TEXTURE9);
		glBindTexture(GL_TEXTURE_2D, giShadingActive ? giShadowTex : m_whiteTex);
		glActiveTexture(GL_TEXTURE10);
		glBindTexture(GL_TEXTURE_2D, (giShadingActive && m_giLocalMaskTex) ? m_giLocalMaskTex : m_whiteTex);
		// Ray-traced reflection result on unit 18 — ONE bind for every program in
		// this pass: the built-in shaders read it as uGIRefl and the graph
		// materials as heGIReflFwd (setupProgram assigns both to 18). Transparent
		// black when inactive, so a sample would contribute nothing even if a
		// gate uniform were ever wrong.
		glActiveTexture(GL_TEXTURE18);
		glBindTexture(GL_TEXTURE_2D, giReflActive ? giReflTex : m_blackTex);
		// Screen-space reflection result on unit 20 — one bind for every program
		// in this pass (built-ins read uSSRFwd, graph materials heSSRFwd). Same
		// transparent-black fallback: a sample would contribute nothing even if a
		// gate uniform were ever wrong.
		glActiveTexture(GL_TEXTURE20);
		glBindTexture(GL_TEXTURE_2D, ssrActive ? ssrTex : m_blackTex);
		// Cloud-shadow transmittance map on unit 19 — ONE bind for every program
		// in this pass (built-ins read uCloudShadowMap, graph materials + the
		// deferred resolve heCloudShadow). White = no shadow when the pass
		// didn't run; the strength gate is 0 then anyway.
		glActiveTexture(GL_TEXTURE19);
		glBindTexture(GL_TEXTURE_2D, m_cloudShadowTex ? m_cloudShadowTex : m_whiteTex);
		glActiveTexture(GL_TEXTURE0);
		const glm::vec4 cloudShA = m_cloudShadowParamsA;
		const glm::vec4 cloudShB = m_cloudShadowTex ? m_cloudShadowParamsB : glm::vec4(0.0f);
		glUniform1i(m_uCloudShadowMap, 19);
		glUniform4fv(m_uCloudShadowA, 1, glm::value_ptr(cloudShA));
		glUniform4fv(m_uCloudShadowB, 1, glm::value_ptr(cloudShB));
		PushGISceneUniforms(m_giLocsUnlit, giShadingActive, giReflActive, ssrActive);

		// Lights + CSM/local-shadow uniforms (clamped to the shader's MAX_LIGHTS).
		BindSceneLighting({ m_uLightCount, m_uLightPos, m_uLightDir, m_uLightColor, m_uLightParams,
		                    m_uCameraPos, m_uShadowEnabled, m_uShadowDebug, m_uCascadeVP,
		                    m_uCascadeSplits, m_uCameraFwd, m_uShadowMap,
		                    m_uLocalShadowMap, m_uLocalShadowVP, m_uShadowBias, m_uUnlit }, shadowFrame);

		// CSM shadow-map array bound on texture unit 1. Always bound (the sampling
		// is gated by uShadowEnabled) so the sampler2DArray never reads a mismatched
		// target; the per-cascade matrices/splits/forward drive the selection.
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
		// Local (point/spot) shadow atlas on unit 11 — always bound (sampling is
		// gated per light by uLightParams[i].y), matrices only when there are any.
		glActiveTexture(GL_TEXTURE11);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_localShadowDepthTex ? m_localShadowDepthTex : m_shadowDepthTex);
		// Same atlas on unit 12 for CUSTOM-material programs (their heCsm alias
		// occupies unit 11 in the preamble's sampler assignment).
		glActiveTexture(GL_TEXTURE12);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_localShadowDepthTex ? m_localShadowDepthTex : m_shadowDepthTex);
		glActiveTexture(GL_TEXTURE0); // base color binds here in the loop

		// Mirror per-frame uniforms onto the instanced program (texture units are
		// shared; only the location integers differ between programs).
		if (m_instancedProgram)
		{
			glUseProgram(m_instancedProgram);
			glUniform1i(m_uInstTexture, 0);
			glUniform3fv(m_uInstSunDir,    1, glm::value_ptr(sunDir));
			glUniform1i(m_uInstSkyEnv, 3);
			glUniform3fv(m_uInstAmbient,   1, glm::value_ptr(m_renderWorld.ambient));
			glUniform1f(m_uInstFogDensity,       GetEnvironment().fogDensity);
			glUniform1f(m_uInstFogHeightFalloff, GetEnvironment().fogHeightFalloff);
			glUniform1f(m_uInstWetness,          GetEnvironment().wetness);
			glUniform1f(m_uInstSnow,             GetEnvironment().snowAmount);
			glUniform1f(m_uInstSpecAA,           m_specularAA ? m_specularAAStrength : 0.0f);
			glUniform1i(m_uInstAO, 4);
			glUniform2f(m_uInstViewport, static_cast<float>(pw), static_cast<float>(ph));
			glUniform1i(m_uInstSSAOEnabled, aoActive ? 1 : 0);
			glUniform1i(m_uInstCloudShadowMap, 19);
			glUniform4fv(m_uInstCloudShadowA, 1, glm::value_ptr(cloudShA));
			glUniform4fv(m_uInstCloudShadowB, 1, glm::value_ptr(cloudShB));
			PushGISceneUniforms(m_giLocsInstanced, giShadingActive, giReflActive, ssrActive);
			// Same lights + CSM block as the unlit program; the shadow ATLASES are
			// already bound on units 1/11 above (texture units are shared).
			BindSceneLighting({ m_uInstLightCount, m_uInstLightPos, m_uInstLightDir,
			                    m_uInstLightColor, m_uInstLightParams, m_uInstCameraPos,
			                    m_uInstShadowEnabled, m_uInstShadowDebug, m_uInstCascadeVP,
			                    m_uInstCascadeSplits, m_uInstCameraFwd, m_uInstShadowMap,
			                    m_uInstLocalShadowMap, m_uInstLocalShadowVP, m_uInstShadowBias,
			                    m_uInstUnlit }, shadowFrame);
			glUseProgram(m_unlitProgram); // restore for the per-object loop
		}

		// Draws arrive sorted by mesh id, so consecutive draws usually share the
		// same mesh (and often the same material). Memoise the last resolved
		// mesh/material so repeated draws skip the cache + content-manager
		// lookups (ResolveMaterialParams in particular re-fetches the material
		// every call). The resolves are pure functions of the id within a frame,
		// so reusing the cached result is behaviour-preserving.
		HE::UUID       lastMeshId{};      const GpuMesh* cMesh = nullptr; bool meshValid = false;
		HE::UUID       lastMatId{};       bool matValid = false;
		unsigned int   cOverrideTex = 0;  bool cHasOverride = false;
		glm::vec3      cBaseColor(1.0f);  float cMetallic = 0.0f, cRoughness = 0.5f; bool cHasMat = false;
		float          cOpacity = 1.0f;

		// Transparent (opacity < 1) draws are deferred to a sorted, alpha-blended
		// pass after the opaque geometry + sky so they composite correctly.
		struct TPDraw { glm::mat4 mvp, model; glm::vec3 baseColor; float metallic, roughness, opacity;
		                unsigned int tex, vao; int indexCount; float distSq;
		                // DrawCall::receivesShadow, carried to the replay — the pass
		                // runs long after the draw's own uniforms were overwritten.
		                bool receivesShadow = true;
		                // Custom translucent material: its GL program + param block +
		                // node-graph textures (0 → the built-in blend program).
		                unsigned int matProg = 0; std::vector<float> params;
		                unsigned int gtex[4] = { 0, 0, 0, 0 }; int gtexCount = 0;
		                // Deferred forward-routed opaque draws only: the landscape
		                // weightmap resolved at collect time (0 → layer-0 default).
		                unsigned int wmTex = 0;
		                // Section draw: byte offset into the EBO (nullptr = from the
		                // start, which is every whole-mesh draw). Trailing + defaulted
		                // so the positional initialisers above stay as they are.
		                const void* indexOffset = nullptr; };
		std::vector<TPDraw> transparent;
		// Deferred: opaque draws whose custom material has no G-buffer variant —
		// replayed forward right after the lighting resolve.
		std::vector<TPDraw> deferredForward;
		const glm::vec3 camPos = m_renderWorld.camera.position;

#if defined(HE_HAVE_SHADERC)
		// The heLitP lighting ABI fill for custom-material programs — shared by
		// the forward opaque loop, the deferred G-buffer loop (Time input) and
		// the deferred replay passes, so the values can never drift between them.
		auto fillMatLight = [&](HE::MaterialShaderLibrary::Lighting& lit)
		{
			// Dominant directional (NOT the raw env sun — night/cloud lesson,
			// see RenderWorld::dominantDirectionalLight) + the full light window.
			glm::vec3 matSunDir, matSunColor;
			m_renderWorld.dominantDirectionalLight(matSunDir, matSunColor);
			const glm::vec3 sc = matSunColor;
			lit.sunDir[0]=matSunDir.x; lit.sunDir[1]=matSunDir.y; lit.sunDir[2]=matSunDir.z;
			// Engine seconds for the node graph's Time input (HE_SKY_TIME pins it
			// for deterministic headless captures, mirroring the sky clock).
			static const char* s_timeOv = std::getenv("HE_SKY_TIME");
			lit.sunDir[3] = s_timeOv && *s_timeOv
				? static_cast<float>(std::atof(s_timeOv))
				: static_cast<float>(SDL_GetTicks()) / 1000.0f;
			lit.camPos[0] = m_renderWorld.camera.position.x;
			lit.camPos[1] = m_renderWorld.camera.position.y;
			lit.camPos[2] = m_renderWorld.camera.position.z;
			lit.sunColor[0]=sc.r; lit.sunColor[1]=sc.g; lit.sunColor[2]=sc.b;
			lit.ambient[0]=m_renderWorld.ambient.r; lit.ambient[1]=m_renderWorld.ambient.g; lit.ambient[2]=m_renderWorld.ambient.b;
			lit.giParams[0] = static_cast<float>(pw);
			lit.giParams[1] = static_cast<float>(ph);
			lit.giParams[2] = giShadingActive ? 1.0f : 0.0f;
			// Aerial perspective + the "is it bound" gates for the shared
			// ambient inputs (units 14/15).
			lit.fog[0] = GetEnvironment().fogDensity;
			lit.fog[1] = GetEnvironment().fogHeightFalloff;
			lit.fog[2] = m_skyEnvCube ? 1.0f : 0.0f;
			lit.fog[3] = aoActive     ? 1.0f : 0.0f;
			// Weather surface response (same values as the built-in programs).
			lit.weather[0] = GetEnvironment().wetness;
			lit.weather[1] = GetEnvironment().snowAmount;
			// Cloud shadows — the same region/strength the built-in programs get
			// (uCloudShadowA/B), so heLitP materials darken identically.
			{
				const glm::vec4 csA = m_cloudShadowParamsA;
				const glm::vec4 csB = m_cloudShadowTex ? m_cloudShadowParamsB : glm::vec4(0.0f);
				std::memcpy(lit.cloudShadowA, glm::value_ptr(csA), 4 * sizeof(float));
				std::memcpy(lit.cloudShadowB, glm::value_ptr(csB), 4 * sizeof(float));
			}
			// DDGI probe grid — the same values PushGISceneUniforms hands
			// the built-in programs, so heLitP's indirect diffuse matches.
			lit.giGridOrigin[0] = m_giGridOrigin.x;
			lit.giGridOrigin[1] = m_giGridOrigin.y;
			lit.giGridOrigin[2] = m_giGridOrigin.z;
			lit.giGridOrigin[3] = m_giProbeSpacing;
			lit.giGridCounts[0] = static_cast<float>(m_giGridCounts.x);
			lit.giGridCounts[1] = static_cast<float>(m_giGridCounts.y);
			lit.giGridCounts[2] = static_cast<float>(m_giGridCounts.z);
			lit.giGridCounts[3] = static_cast<float>(m_giProbesPerRow);
			lit.giProbe[0] = m_giIndirectIntensity;
			lit.giProbe[1] = (giShadingActive && m_giIrrAtlas && m_giVisAtlas) ? 1.0f : 0.0f;
			// Ray-traced reflections for heLitP's forward cascade (heGIReflFwd,
			// unit 18). x = intensity, y = max roughness, z = the gate.
			lit.giRefl[0] = m_giReflIntensity;
			lit.giRefl[1] = m_giReflMaxRoughness;
			lit.giRefl[2] = giReflActive ? 1.0f : 0.0f;
			// Screen-space reflections, the next stage of the SAME cascade
			// (heSSRFwd, unit 20). x = the gate, y = intensity, z = max
			// roughness; w is the DEFERRED marker and stays zero — the forward
			// path never zeroes ambSpec, and GL has no deferred composite (A6).
			lit.ssr[0] = ssrActive ? 1.0f : 0.0f;
			lit.ssr[1] = m_ssrIntensity;
			lit.ssr[2] = m_ssrMaxRoughness;
			lit.ssr[3] = 0.0f;
			// Full light window for heLitP() — same first-8 order as the built-in
			// PBR shaders. Shared fill (HE::FillMaterialLightWindow); it also
			// writes the per-light atlas layer into lightParams[i].y when
			// `localShadows` says the atlas is bound this frame.
			HE::FillMaterialLightWindow(m_renderWorld, lit, localShadows);
			HE::FillMaterialWind(GetEnvironment(), lit); // Wind / Wind Sway nodes, next to Time
			// Local (point/spot) shadow atlas for heLitP — same matrices the
			// built-in shaders use, with the GL depth remap (z: [-1,1]→[0,1])
			// PRE-BAKED so the shared preamble stays convention-free.
			if (localShadows)
			{
				glm::mat4 zRemap(1.0f);
				zRemap[2][2] = 0.5f; zRemap[3][2] = 0.5f;
				for (int c = 0; c < nLocalLayers; ++c)
				{
					const glm::mat4 m = zRemap * m_renderWorld.shadow.localViewProj[c];
					std::memcpy(lit.localShadowVP[c], &m[0][0], 16 * sizeof(float));
				}
			}
			// Specular AA (A6). y = 1 says "this fill is for a GEOMETRY pass",
			// where the fragment's own normal and its derivatives exist. Every
			// caller of this lambda is one — except the deferred resolve, which
			// clears y again right after calling it.
			lit.specAA[0] = m_specularAA ? m_specularAAStrength : 0.0f;
			lit.specAA[1] = 1.0f;
			// Viewport view mode (v3.2): Unlit/Wireframe hand heLitP's base
			// colour back untouched — graph materials and the deferred resolve
			// both read this. Scene-pass fill only; previews keep the zero.
			lit.viewMode[0] = UnlitViewActive() ? 1.0f : 0.0f;
		};
#endif

		glUniform1f(m_uOpacity, 1.0f); // opaque pass writes alpha 1
		m_matLightUploadedThisFrame = false; // lighting UBO re-uploaded at most once this frame

		// ── Deferred G-buffer loop: the same routing as the forward loop below,
		// only with G-buffer programs and without any lighting binds (plan §4.1).
		// Translucent draws collect into `transparent` exactly like forward;
		// custom materials WITHOUT a G-buffer variant collect into
		// `deferredForward` and replay right after the lighting resolve.
		if (deferredActive)
		{
#if defined(HE_HAVE_SHADERC)
			// G-buffer graph fragments may read the Time input (heLight.sunDir.w)
			// — upload the standard fill once, before any custom G-buffer draw.
			{
				HE::MaterialShaderLibrary::Lighting lit{};
				fillMatLight(lit);
				if (m_matLightUBO)
				{
					glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
					m_matLightUploadedThisFrame = true;
				}
			}
#endif
			glUseProgram(m_gbufferProgram);
			GLWireScope _gbWire(WireframeViewActive());
			for (const DrawCall& dc : cmds.drawCalls())
			{
				if (!matValid || dc.materialAssetId != lastMatId)
				{
					cHasOverride = ResolveMaterialTexture(dc.materialAssetId, cOverrideTex);
					cBaseColor   = glm::vec3(1.0f); cMetallic = 0.0f; cRoughness = 0.5f; cOpacity = 1.0f;
					cHasMat      = ResolveMaterialParams(dc.materialAssetId, cBaseColor, cMetallic, cRoughness, cOpacity);
					lastMatId    = dc.materialAssetId; matValid = true;
				}
				if (!meshValid || dc.meshAssetId != lastMeshId)
				{
					cMesh      = ResolveMesh(dc.meshAssetId);
					lastMeshId = dc.meshAssetId; meshValid = true;
				}
				const GpuMesh*     mesh = cMesh;
				const unsigned int tex  = cHasOverride ? cOverrideTex
				                                       : (mesh ? mesh->texture : 0u);
				glm::vec3 baseColor = cBaseColor;
				if (!cHasMat)
					baseColor = (tex != 0) ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);
				baseColor *= glm::vec3(dc.instanceTint);
				const float opacity = cOpacity * dc.instanceTint.a;
				const GpuMesh* drawMesh = mesh ? mesh : ResolveMesh(HE::kDefaultCubeMeshId);
				if (!drawMesh) continue;
				const unsigned int vao        = drawMesh->vao;
				// Section draw → its own slice of the EBO; whole-mesh draw → all of it.
				const GlIndexRange range      = DrawIndexRange(dc, drawMesh->indexCount);
				const int          indexCount = range.count;
				const void* const  indexOffset = range.offset;

				// Custom-material programs: forward (transparency + forward-routing)
				// and, when the material has a G-buffer variant, its MRT program.
				unsigned int matProg = 0, gbProg = 0; bool hasCustom = false;
				std::vector<float> mParams; unsigned int mGtex[4] = { 0, 0, 0, 0 }; int mGtexCount = 0;
#if defined(HE_HAVE_SHADERC)
				{
					uint64_t shKey; std::string shFrag, shVert;
					if (resolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert))
					{
						hasCustom = true;
						const MaterialShaderVariant* pre = nullptr;
						const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr;
						if (ma)
							for (const auto& var : ma->precompiledShaders)
								if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
						matProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
						// G-buffer variant — runtime cross-compiled only (baked packs
						// carry no G-buffer GLSL yet).
						uint64_t gbKey; std::string gbFrag, gbVert;
						if (resolveMaterialShaderGB(dc.materialAssetId, gbKey, gbFrag, gbVert))
							gbProg = GetOrBuildMaterialProgram(gbKey, gbFrag, gbVert, nullptr);
						if (ma)
						{
							mParams = !dc.paramOverride.empty() ? dc.paramOverride
							                                    : ma->shaderParamData;
							const size_t nTex = std::min<size_t>(4,
								std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
							for (size_t i = 0; i < nTex; ++i)
							{
								const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
								const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
								mGtex[mGtexCount++] = ResolveGraphTexture(gid, gp);
							}
						}
					}
				}
#endif
				if (opacity < RenderSorter::kOpaqueOpacityThreshold)
				{
					// Same collection as the forward loop's transparent branch.
					auto pushTP = [&](const glm::mat4& t) {
						TPDraw tp{ viewProj * t, t, baseColor,
						           cMetallic, cRoughness, opacity, tex, vao, indexCount,
						           RenderSorter::backToFrontKey(t, camPos) };
						tp.indexOffset    = indexOffset;
						tp.receivesShadow = dc.receivesShadow;
						tp.matProg = matProg;
						tp.params  = mParams;
						for (int i = 0; i < mGtexCount; ++i) tp.gtex[i] = mGtex[i];
						tp.gtexCount = mGtexCount;
						transparent.push_back(std::move(tp));
					};
					if (!dc.instanceTransforms.empty())
						for (const glm::mat4& t : dc.instanceTransforms) pushTP(t);
					else
						pushTP(dc.transform);
					continue;
				}
				if (!dc.instanceTransforms.empty() && m_gbufferInstancedProgram && m_instanceVBO)
				{
					// Instanced batches always take the built-in instanced program —
					// the same routing the forward loop uses.
					glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
					glBufferData(GL_ARRAY_BUFFER,
					             static_cast<GLsizeiptr>(dc.instanceTransforms.size() * sizeof(glm::mat4)),
					             dc.instanceTransforms.data(), GL_STREAM_DRAW);
					glBindBuffer(GL_ARRAY_BUFFER, 0);
					glUseProgram(m_gbufferInstancedProgram);
					glUniformMatrix4fv(m_uGBInstViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
					glUniform3fv(m_uGBInstColor,     1, glm::value_ptr(baseColor));
					glUniform1f(m_uGBInstMetallic,   cMetallic);
					glUniform1f(m_uGBInstRoughness,  cRoughness);
					glBindVertexArray(vao);
					glUniform1i(m_uGBInstHasTexture, tex != 0);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, tex);
					glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset,
					                        static_cast<GLsizei>(dc.instanceTransforms.size()));
					++m_counters.draws;
					m_counters.tris += static_cast<uint32_t>(indexCount / 3) *
					                   static_cast<uint32_t>(dc.instanceTransforms.size());
					glUseProgram(m_gbufferProgram);
					continue;
				}
				if (hasCustom && !gbProg)
				{
					// No G-buffer variant → forward replay after the resolve.
					TPDraw tp{ viewProj * dc.transform, dc.transform, baseColor,
					           cMetallic, cRoughness, opacity, tex, vao, indexCount, 0.0f };
					tp.indexOffset    = indexOffset;
					tp.receivesShadow = dc.receivesShadow;
					tp.matProg = matProg;
					tp.params  = mParams;
					for (int i = 0; i < mGtexCount; ++i) tp.gtex[i] = mGtex[i];
					tp.gtexCount = mGtexCount;
					{
						const HE::UUID wid = dc.weightmapTextureId != HE::UUID{}
							? dc.weightmapTextureId : HE::kDefaultLayer0WeightTextureId;
						tp.wmTex = ResolveGraphTexture(wid, {});
					}
					deferredForward.push_back(std::move(tp));
					continue;
				}
#if defined(HE_HAVE_SHADERC)
				if (gbProg)
				{
					// Custom material → its G-buffer MRT program, same per-draw
					// binds as the forward custom branch (minus the lighting-only
					// inputs the resolve owns).
					glUseProgram(gbProg);
					struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } obj;
					obj.mvp   = viewProj * dc.transform;
					obj.model = dc.transform;
					obj.color = glm::vec4(baseColor, 1.0f);
					obj.flags = glm::vec4(tex != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
					obj.pbr   = glm::vec4(cMetallic, cRoughness, opacity, 0.0f);
					glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
					glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);   // block "U"
					glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO); // block "HeLighting" (Time)
					if (!mParams.empty())
					{
						float padded[64] = { 0 };
						std::memcpy(padded, mParams.data(),
						            std::min(mParams.size(), size_t(64)) * sizeof(float));
						if (!m_haveMatParams || std::memcmp(padded, m_lastMatParams, sizeof(padded)) != 0)
						{
							std::memcpy(m_lastMatParams, padded, sizeof(padded));
							m_haveMatParams = true;
							glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
							glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
							glBindBuffer(GL_UNIFORM_BUFFER, 0);
						}
					}
					glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO); // block "HeParams"
					glBindVertexArray(vao);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, tex);
					for (int i = 0; i < mGtexCount; ++i)
					{
						glActiveTexture(GL_TEXTURE1 + (GLenum)i);
						glBindTexture(GL_TEXTURE_2D, mGtex[i]);
					}
					// Landscape layer weightmap on unit 13, per draw (as forward).
					{
						const HE::UUID wid = dc.weightmapTextureId != HE::UUID{}
							? dc.weightmapTextureId : HE::kDefaultLayer0WeightTextureId;
						glActiveTexture(GL_TEXTURE13);
						glBindTexture(GL_TEXTURE_2D, ResolveGraphTexture(wid, {}));
					}
					glActiveTexture(GL_TEXTURE0);
					glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset);
					glUseProgram(m_gbufferProgram);
					++m_counters.draws;
					m_counters.tris += static_cast<uint32_t>(indexCount / 3);
					continue;
				}
#endif
				// Built-in PBR → the built-in G-buffer program.
				glUniformMatrix4fv(m_uGBMVP,   1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
				glUniformMatrix4fv(m_uGBModel, 1, GL_FALSE, glm::value_ptr(dc.transform));
				glUniform3fv(m_uGBColor, 1, glm::value_ptr(baseColor));
				glUniform1f(m_uGBMetallic,  cMetallic);
				glUniform1f(m_uGBRoughness, cRoughness);
				glUniform1f(m_uGBSpecAA,    m_specularAA ? m_specularAAStrength : 0.0f);
				glBindVertexArray(vao);
				glUniform1i(m_uGBHasTexture, tex != 0);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3);
			}
			glUseProgram(m_unlitProgram);
		}
		else
		{
		GLWireScope _fwdWire(WireframeViewActive());
		for (const DrawCall& dc : cmds.drawCalls())
		{
			// An explicit MaterialComponent override wins over the mesh's own
			// base-color texture; otherwise fall back to the mesh's (or none).
			// PBR scalars come from the material override; defaults otherwise.
			if (!matValid || dc.materialAssetId != lastMatId)
			{
				cHasOverride = ResolveMaterialTexture(dc.materialAssetId, cOverrideTex);
				cBaseColor   = glm::vec3(1.0f); cMetallic = 0.0f; cRoughness = 0.5f; cOpacity = 1.0f;
				cHasMat      = ResolveMaterialParams(dc.materialAssetId, cBaseColor, cMetallic, cRoughness, cOpacity);
				lastMatId    = dc.materialAssetId; matValid = true;
			}

			// Resolve the asset; entities without one fall back to the built-in cube.
			if (!meshValid || dc.meshAssetId != lastMeshId)
			{
				cMesh      = ResolveMesh(dc.meshAssetId);
				lastMeshId = dc.meshAssetId; meshValid = true;
			}
			const GpuMesh*     mesh = cMesh;
			const unsigned int tex  = cHasOverride ? cOverrideTex
			                                       : (mesh ? mesh->texture : 0u);

			// The base tint is the material baseColor if assigned, else white when
			// textured (so the texture is unchanged) or the flat fallback color.
			glm::vec3 baseColor = cBaseColor;
			if (!cHasMat)
				baseColor = (tex != 0) ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);

			// Per-instance tint (particle color/alpha-over-life, see
			// RenderObject::instanceTint) — identity for everything else, so this
			// is a no-op outside particles. Applied to a per-draw copy, never to
			// the memoised cBaseColor/cOpacity (those are cached across draws that
			// share a material and must stay untinted).
			baseColor *= glm::vec3(dc.instanceTint);
			const float opacity = cOpacity * dc.instanceTint.a;

			const GpuMesh* drawMesh  = mesh ? mesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!drawMesh) continue;
			const unsigned int vao        = drawMesh->vao;
			// Section draw → its own slice of the EBO; whole-mesh draw → all of it.
			const GlIndexRange range      = DrawIndexRange(dc, drawMesh->indexCount);
			const int          indexCount = range.count;
			const void* const  indexOffset = range.offset;

			if (opacity < RenderSorter::kOpaqueOpacityThreshold)
			{
				// Translucent graph materials keep their own program + state in the pass.
				unsigned int tpProg = 0; std::vector<float> tpParams;
				unsigned int tpGtex[4] = { 0, 0, 0, 0 }; int tpGtexCount = 0;
#if defined(HE_HAVE_SHADERC)
				{
					uint64_t shKey; std::string shFrag, shVert;
					if (resolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert))
					{
						const MaterialShaderVariant* pre = nullptr;
						const MaterialAsset* ma = m_contentManager
							? m_contentManager->getMaterial(dc.materialAssetId) : nullptr;
						if (ma)
							for (const auto& var : ma->precompiledShaders)
								if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
						tpProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
						if (tpProg && ma)
						{
							tpParams = !dc.paramOverride.empty() ? dc.paramOverride
							                                     : ma->shaderParamData;
							const size_t nTex = std::min<size_t>(4,
								std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
							for (size_t i = 0; i < nTex; ++i)
							{
								const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
								const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
								tpGtex[tpGtexCount++] = ResolveGraphTexture(gid, gp);
							}
						}
					}
				}
#endif
				// Transparent instanced batches: push one TPDraw per instance so
				// each object is sorted individually by distance.
				auto pushTP = [&](const glm::mat4& t) {
					TPDraw tp{ viewProj * t, t, baseColor,
					           cMetallic, cRoughness, opacity, tex, vao, indexCount,
					           RenderSorter::backToFrontKey(t, camPos) };
					tp.indexOffset    = indexOffset;
					tp.receivesShadow = dc.receivesShadow;
					tp.matProg = tpProg;
					tp.params  = tpParams;
					for (int i = 0; i < tpGtexCount; ++i) tp.gtex[i] = tpGtex[i];
					tp.gtexCount = tpGtexCount;
					transparent.push_back(std::move(tp));
				};
				if (!dc.instanceTransforms.empty())
					for (const glm::mat4& t : dc.instanceTransforms) pushTP(t);
				else
					pushTP(dc.transform);
				continue; // drawn in the transparency pass below
			}

			if (!dc.instanceTransforms.empty() && m_instancedProgram && m_instanceVBO)
			{
				// GPU-instanced opaque draw: upload all transforms to the scratch VBO,
				// then call glDrawElementsInstanced with the instanced program.
				glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
				glBufferData(GL_ARRAY_BUFFER,
				             static_cast<GLsizeiptr>(dc.instanceTransforms.size() * sizeof(glm::mat4)),
				             dc.instanceTransforms.data(), GL_STREAM_DRAW);
				glBindBuffer(GL_ARRAY_BUFFER, 0);

				glUseProgram(m_instancedProgram);
				glUniformMatrix4fv(m_uInstViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
				glUniform3fv(m_uInstColor,     1, glm::value_ptr(baseColor));
				glUniform1f(m_uInstMetallic,   cMetallic);
				glUniform1f(m_uInstRoughness,  cRoughness);
				glUniform1f(m_uInstOpacity,    1.0f);
				glUniform1i(m_uInstNoShadow,   !dc.receivesShadow);
				glBindVertexArray(vao);
				glUniform1i(m_uInstHasTexture, tex != 0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset,
				                        static_cast<GLsizei>(dc.instanceTransforms.size()));
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3) *
				                   static_cast<uint32_t>(dc.instanceTransforms.size());
				glUseProgram(m_unlitProgram); // restore for the next single-draw
			}
			else
			{
#if defined(HE_HAVE_SHADERC)
				// Per-material GL program (MaterialAsset custom shader), cross-compiled from
				// canonical GLSL via the shared library. Feeds per-object + lighting through
				// UBOs; same VAO/attribs as the unlit program. Materials without one fall
				// through to the built-in shader below.
				uint64_t shKey; std::string shFrag, shVert; unsigned int matProg = 0;
				if (resolveMaterialShader(dc.materialAssetId, shKey, shFrag, shVert))
				{
					const MaterialShaderVariant* pre = nullptr;
					if (const MaterialAsset* ma = m_contentManager
						? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
						for (const auto& var : ma->precompiledShaders)
							if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
					matProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
				}
				if (matProg)
				{
					glUseProgram(matProg);
					struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } obj;
					obj.mvp   = viewProj * dc.transform;
					obj.model = dc.transform;
					obj.color = glm::vec4(baseColor, 1.0f);
					obj.flags = glm::vec4(tex != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
					obj.pbr   = glm::vec4(cMetallic, cRoughness, opacity, 0.0f);
					glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj);
					HE::MaterialShaderLibrary::Lighting lit{};
					fillMatLight(lit); // shared heLitP ABI fill (see the lambda above)
					// Lighting is identical for every material draw this frame → upload once.
					if (!m_matLightUploadedThisFrame)
					{
						glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
						glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
						glBindBuffer(GL_UNIFORM_BUFFER, 0);
						m_matLightUploadedThisFrame = true;
					}
					glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);   // block "U"
					glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO); // block "HeLighting"
					// Exposed graph parameters (HeParams @ binding 2) — value edits reach the
					// shader without any recompile.
					// A per-entity override (already the full merged block, never batched)
					// wins over the material's shared shaderParamData.
					const MaterialAsset* ma = m_contentManager
						? m_contentManager->getMaterial(dc.materialAssetId) : nullptr;
					const std::vector<float>* params =
						!dc.paramOverride.empty() ? &dc.paramOverride
						: (ma && !ma->shaderParamData.empty() ? &ma->shaderParamData : nullptr);
					if (params)
					{
						float padded[64] = { 0 };
						std::memcpy(padded, params->data(),
						            std::min(params->size(), size_t(64)) * sizeof(float));
						// Skip the upload when the UBO already holds these exact params
						// (the common case: many draws of the same material). Still correct
						// for per-entity overrides — their block differs, so it re-uploads.
						if (!m_haveMatParams || std::memcmp(padded, m_lastMatParams, sizeof(padded)) != 0)
						{
							std::memcpy(m_lastMatParams, padded, sizeof(padded));
							m_haveMatParams = true;
							glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
							glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
							glBindBuffer(GL_UNIFORM_BUFFER, 0);
						}
					}
					glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO); // block "HeParams"
					glBindVertexArray(vao);
					// Legacy/mesh texture on unit 0 (heTex0).
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, tex);
					// Node-graph project textures on units 1..4 (heTexP0..3).
					if (const MaterialAsset* ma = m_contentManager
						? m_contentManager->getMaterial(dc.materialAssetId) : nullptr)
					{
						const size_t nTex = std::min<size_t>(4,
							std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
						for (size_t i = 0; i < nTex; ++i)
						{
							const HE::UUID    gid = i < ma->graphTextureIds.size()   ? ma->graphTextureIds[i]   : HE::UUID{};
							const std::string gp  = i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i] : std::string{};
							glActiveTexture(GL_TEXTURE1 + (GLenum)i);
							glBindTexture(GL_TEXTURE_2D, ResolveGraphTexture(gid, gp));
						}
						glActiveTexture(GL_TEXTURE0);
					}
					// Shared material inputs: sky env cubemap (14) for image-based
					// ambient + the fog colour, screen-space AO (15). Per-FRAME state,
					// re-asserted here because the units are only ever used by these
					// material programs. Gated by heLight.fog.z/.w.
					glActiveTexture(GL_TEXTURE14);
					glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
					glActiveTexture(GL_TEXTURE15);
					glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
					// DDGI probe atlases on 16/17 (white fallbacks keep the samplers
					// valid when GI is off; heLight.giProbe.y gates the reads).
					glActiveTexture(GL_TEXTURE16);
					glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giIrrAtlas : m_whiteTex);
					glActiveTexture(GL_TEXTURE17);
					glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giVisAtlas : m_whiteTex);
					glActiveTexture(GL_TEXTURE0);
					// Landscape layer weightmap on unit 13 — PER DRAW (it belongs to
					// the terrain the chunk is part of, not to the material). Objects
					// that aren't landscape chunks get the 1x1 (1,0,0,0) default, so a
					// layer-blend node resolves to layer 0 instead of black.
					{
						const HE::UUID wid = dc.weightmapTextureId != HE::UUID{}
							? dc.weightmapTextureId : HE::kDefaultLayer0WeightTextureId;
						glActiveTexture(GL_TEXTURE13);
						glBindTexture(GL_TEXTURE_2D, ResolveGraphTexture(wid, {}));
						glActiveTexture(GL_TEXTURE0);
					}
					glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset);
					glUseProgram(m_unlitProgram); // restore for the next single-draw
					++m_counters.draws;
					m_counters.tris += static_cast<uint32_t>(indexCount / 3);
					continue;
				}
#endif
				glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
				glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(dc.transform));
				glUniform3fv(m_uColor, 1, glm::value_ptr(baseColor));
				glUniform1f(m_uMetallic,  cMetallic);
				glUniform1f(m_uRoughness, cRoughness);
				glUniform1i(m_uNoShadow,  !dc.receivesShadow);
				glBindVertexArray(vao);
				glUniform1i(m_uHasTexture, tex != 0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, indexOffset);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3);
			}
		}
		} // forward loop (wireframe scope)

#if defined(HE_HAVE_SHADERC)
		// ── Deferred decals ─────────────────────────────────────────────────
		// docs/decals-cross-backend-plan.md checkpoint A, the GL counterpart of
		// MetalRenderer::EncodeDecals. Unit-cube projectors blended into GB0.rgb
		// AFTER the geometry and BEFORE the resolve reads the attributes, so the
		// decal colour is BaseColor and gets lit like everything else — one
		// shading source, no second lighting path.
		if (deferredActive && !m_renderWorld.decals.empty() && EnsureDecalProgram())
		{
			GpuPassScope _decalTimer(this, "Decals");
			// GB0 alone, without the depth attachment the fragment samples.
			glBindFramebuffer(GL_FRAMEBUFFER, m_gbDecalFBO);
			glViewport(0, 0, pw, ph);
			// GB0 is SRGB8_ALPHA8: the encode-on-write is still on from the
			// G-buffer pass, and it is also what makes the blend happen in LINEAR
			// space — the same thing Metal's RGBA8Unorm_sRGB target does.
			glEnable(GL_FRAMEBUFFER_SRGB);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE); // metallic in GB0.a survives
			glDisable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);                            // the box clip decides coverage
			// Cull the FRONT faces so the projector still rasterizes with the
			// camera inside its box. GL's default GL_CCW front-face selects the
			// SAME triangles as Metal's default MTLWindingClockwise: winding is
			// evaluated in framebuffer coordinates, and Metal's framebuffer origin
			// is top-left where GL's is bottom-left, so the two conventions cancel
			// (the decal shader's params.y, -1 on Metal and +1 here, is the same
			// y-flip seen from the reconstruction side). Exactly one triangle
			// layer must survive — both would blend the decal twice.
			glEnable(GL_CULL_FACE);
			glCullFace(GL_FRONT);

			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, m_gbDepthTex);
			glActiveTexture(GL_TEXTURE0);
			glUseProgram(m_decalProgram);
			glBindBufferBase(GL_UNIFORM_BUFFER, 4, m_decalUBO);
			glBindVertexArray(m_fsVAO); // buffer-less draw still needs SOME VAO

			const glm::mat4 ivp = glm::inverse(viewProj);
			for (const DecalData& dcl : m_renderWorld.decals)
			{
				HE::MaterialShaderLibrary::DecalUniforms du;
				const glm::mat4 invModel = glm::inverse(dcl.transform);
				std::memcpy(du.viewProj,    &viewProj[0][0],      16 * sizeof(float));
				std::memcpy(du.model,       &dcl.transform[0][0], 16 * sizeof(float));
				std::memcpy(du.invModel,    &invModel[0][0],      16 * sizeof(float));
				std::memcpy(du.invViewProj, &ivp[0][0],           16 * sizeof(float));
				du.color[0] = dcl.color.r; du.color[1] = dcl.color.g;
				du.color[2] = dcl.color.b; du.color[3] = dcl.color.a;
				const unsigned int dtex = dcl.textureId != HE::UUID{}
					? ResolveGraphTexture(dcl.textureId, {}) : 0u;
				du.params[0] = dtex ? 1.0f : 0.0f;
				// GL depth conventions, copied from the resolve's depthParams
				// (ndc-y sign +1, depth [0,1] → ndc z = d*2-1) rather than
				// re-derived. Metal's are (-1, 1, 0).
				du.params[1] =  1.0f;
				du.params[2] =  2.0f;
				du.params[3] = -1.0f;
				du.vp[0] = static_cast<float>(pw);
				du.vp[1] = static_cast<float>(ph);
				glBindBuffer(GL_UNIFORM_BUFFER, m_decalUBO);
				glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(du), &du);
				glBindBuffer(GL_UNIFORM_BUFFER, 0);
				glBindTexture(GL_TEXTURE_2D, dtex ? dtex : m_whiteTex); // unit 0 is active
				glDrawArrays(GL_TRIANGLES, 0, 36);
				++m_counters.draws;
				m_counters.tris += 12;
			}

			// Back to what the G-buffer pass had set, so the resolve below and
			// the forward tail find the state they assume.
			glBindVertexArray(0);
			glUseProgram(0);
			glDisable(GL_CULL_FACE);
			glDisable(GL_BLEND);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glDepthMask(GL_TRUE);
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, 0);
			glActiveTexture(GL_TEXTURE0);
			glBindFramebuffer(GL_FRAMEBUFFER, m_gbFBO);
		}
#endif

		// ── Deferred: fullscreen lighting resolve + forward-routed replay ────
		if (deferredActive)
		{
			glDisable(GL_FRAMEBUFFER_SRGB);

			// P5: SSAO from the G-buffer depth (one fullscreen reconstruction
			// instead of the geometry pre-pass). Runs here — after the G-buffer,
			// before the resolve that samples the result on unit 15.
			if (m_ssaoEnabled && !giShadingActive)
			{
				GpuPassScope _ssaoTimer(this, "SSAO");
				// Clean projection against the jittered G-buffer depth: <= half
				// a pixel off, as on Metal's deferred path (see forward above).
				aoTex = RenderSSAO(cmds, pw, ph, viewProjClean, m_renderWorld.camera.view,
				                   m_renderWorld.camera.projection, /*fromGBufferDepth=*/true);
				aoActive = m_ssaoEnabled && aoTex != 0;
				// Refresh the AO binds the per-frame setup made while aoTex was
				// still 0: built-in unit 4 (uAO) + its enable flag on the unlit
				// program (transparency replay reads it), custom-material unit 15.
				glActiveTexture(GL_TEXTURE4);
				glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
				glActiveTexture(GL_TEXTURE0);
				glUseProgram(m_unlitProgram);
				glUniform1i(m_uSSAOEnabled, aoActive ? 1 : 0);
#if defined(HE_HAVE_SHADERC)
				// The pre-loop lighting upload (Time for G-buffer graphs) carried
				// fog.w = 0 — re-upload with the fresh AO gate for the forward-
				// routed and transparent custom-material draws.
				if (m_matLightUBO)
				{
					HE::MaterialShaderLibrary::Lighting lit{};
					fillMatLight(lit);
					glBindBuffer(GL_UNIFORM_BUFFER, m_matLightUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
				}
#endif
			}

			// Depth: G-buffer → HDR FBO, so the forward tail (skinned/sky/
			// transparency) depth-tests against the deferred geometry while the
			// resolve SAMPLES the depth texture (attachment + sampled input of
			// one FBO would be a feedback loop).
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbFBO);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_hdrFBO);
			glBlitFramebuffer(0, 0, pw, ph, 0, 0, pw, ph, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
			glViewport(0, 0, pw, ph);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT); // depth was just blitted — keep it

#if defined(HE_HAVE_SHADERC)
			if (m_deferredResolveProgram && m_resolveLightUBO)
			{
				// Resolve-only lighting fill: the standard heLitP ABI PLUS the CSM
				// cascade matrices (GL z remap pre-baked, like localShadowVP). A
				// SEPARATE buffer from m_matLightUBO: custom-material programs
				// alias heCsm onto the local-atlas unit, so THEIR fill must keep
				// csmSplits.w = 0 — only the resolve binds the real CSM array.
				HE::MaterialShaderLibrary::Lighting lit{};
				fillMatLight(lit);
				// The resolve is NOT a geometry pass: its "normal" is a G-buffer
				// texel whose derivative jumps at every silhouette, so specular AA
				// must stay off here. The G-buffer pass already widened the
				// roughness it stored (A6).
				lit.specAA[1] = 0.0f;
				if (!giShadingActive && shadowFrame.shadows && m_shadowDepthTex)
				{
					glm::mat4 zRemap(1.0f);
					zRemap[2][2] = 0.5f; zRemap[3][2] = 0.5f;
					const int nc = std::min(nCascades, 3);
					for (int c = 0; c < nc; ++c)
					{
						const glm::mat4 m = zRemap * m_renderWorld.shadow.cascadeViewProj[c];
						std::memcpy(lit.csmVP[c], &m[0][0], 16 * sizeof(float));
					}
					lit.csmSplits[0] = cascadeSplits.x;
					lit.csmSplits[1] = cascadeSplits.y;
					lit.csmSplits[2] = cascadeSplits.z;
					lit.csmSplits[3] = static_cast<float>(nc);
					lit.camFwd[0] = camFwd.x; lit.camFwd[1] = camFwd.y; lit.camFwd[2] = camFwd.z;
					lit.shadowBias[0] = m_shadowSettings.slopeBias;
					lit.shadowBias[1] = m_shadowSettings.minBias;
				}
				glBindBuffer(GL_UNIFORM_BUFFER, m_resolveLightUBO);
				glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(lit), &lit);

				// HeResolve: world-pos reconstruction (GL: ndc.y sign +1, depth
				// [0,1] → ndc z = d*2-1) + the G-buffer view (SetViewMode /
				// HE_DUMP_GBUFFER).
				HE::MaterialShaderLibrary::ResolveUniforms ru;
				std::memcpy(ru.invViewProj, glm::value_ptr(invViewProj), 16 * sizeof(float));
				ru.depthParams[0] = 1.0f;
				ru.depthParams[1] = 2.0f;
				ru.depthParams[2] = -1.0f;
				ru.depthParams[3] = static_cast<float>(HE::viewModeGBufferIndex(m_viewMode));
				glBindBuffer(GL_UNIFORM_BUFFER, m_resolveUBO);
				glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ru), &ru);
				glBindBuffer(GL_UNIFORM_BUFFER, 0);

				// G-buffer inputs on units 0..3; the real CSM array replaces the
				// local atlas on unit 11 for the duration of the resolve draw.
				glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gbColor0);
				glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gbColor1);
				glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_gbColor2);
				glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_gbDepthTex);
				glActiveTexture(GL_TEXTURE11);
				glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
				// Shared material inputs the preamble samples (sky env / AO / DDGI
				// atlases) — the same units the custom-material draws use.
				glActiveTexture(GL_TEXTURE14);
				glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
				glActiveTexture(GL_TEXTURE15);
				glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
				glActiveTexture(GL_TEXTURE16);
				glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giIrrAtlas : m_whiteTex);
				glActiveTexture(GL_TEXTURE17);
				glBindTexture(GL_TEXTURE_2D, giShadingActive ? m_giVisAtlas : m_whiteTex);
				glActiveTexture(GL_TEXTURE0);

				glUseProgram(m_deferredResolveProgram);
				glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_resolveLightUBO);
				glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_resolveUBO);
				glDisable(GL_DEPTH_TEST);
				glDepthMask(GL_FALSE);
				glBindVertexArray(m_fsVAO);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				++m_counters.draws;
				glEnable(GL_DEPTH_TEST);
				glDepthMask(GL_TRUE);
				glDepthFunc(GL_LESS);
				// Restore the material-programs' state: their lighting UBO on
				// binding 0 and the local atlas back on unit 11.
				glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO);
				glActiveTexture(GL_TEXTURE11);
				glBindTexture(GL_TEXTURE_2D_ARRAY,
				              m_localShadowDepthTex ? m_localShadowDepthTex : m_shadowDepthTex);
				glActiveTexture(GL_TEXTURE0);
			}

			// Forward-routed opaque draws (custom materials without a G-buffer
			// variant): full depth test + write against the blitted depth, no
			// blending — the same custom-material draw the forward loop performs.
			GLWireScope _replayWire(WireframeViewActive());
			for (const TPDraw& t : deferredForward)
			{
				if (t.matProg)
				{
					glUseProgram(t.matProg);
					struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } obj;
					obj.mvp   = t.mvp;
					obj.model = t.model;
					obj.color = glm::vec4(t.baseColor, 1.0f);
					obj.flags = glm::vec4(t.tex != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
					obj.pbr   = glm::vec4(t.metallic, t.roughness, t.opacity, 0.0f);
					glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj);
					float padded[64] = { 0 };
					std::memcpy(padded, t.params.data(),
					            std::min(t.params.size(), size_t(64)) * sizeof(float));
					glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
					m_haveMatParams = false;
					glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO);
					glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);
					glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO);
					glBindVertexArray(t.vao);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, t.tex);
					for (int i = 0; i < t.gtexCount; ++i)
					{
						glActiveTexture(GL_TEXTURE1 + (GLenum)i);
						glBindTexture(GL_TEXTURE_2D, t.gtex[i]);
					}
					if (t.wmTex)
					{
						glActiveTexture(GL_TEXTURE13);
						glBindTexture(GL_TEXTURE_2D, t.wmTex);
					}
					glActiveTexture(GL_TEXTURE0);
					glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, t.indexOffset);
					++m_counters.draws;
					m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
					continue;
				}
				// Custom shader failed to build entirely → built-in forward PBR.
				glUseProgram(m_unlitProgram);
				glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(t.mvp));
				glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(t.model));
				glUniform3fv(m_uColor, 1, glm::value_ptr(t.baseColor));
				glUniform1f(m_uMetallic,  t.metallic);
				glUniform1f(m_uRoughness, t.roughness);
				glUniform1f(m_uOpacity,   1.0f);
				glUniform1i(m_uNoShadow,  !t.receivesShadow);
				glUniform1i(m_uHasTexture, t.tex != 0);
				glBindVertexArray(t.vao);
				glBindTexture(GL_TEXTURE_2D, t.tex);
				glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, t.indexOffset);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
			}
#endif
			glUseProgram(m_unlitProgram); // restore for the skinned/sky passes
		}

		// ── Skinned draws (after opaque statics, before sky) ────────────────────
		if (m_skinnedProgram && !cmds.skinnedDrawCalls().empty())
		{
			glUseProgram(m_skinnedProgram);
			// Mirror per-frame uniforms from the unlit program.
			glUniform1i(m_uSkinnedTex, 0);
			glUniform3fv(m_uSkinnedSunDir,    1, glm::value_ptr(sunDir));
			glUniform3fv(m_uSkinnedAmbient,   1, glm::value_ptr(m_renderWorld.ambient));
			glUniform1f(m_uSkinnedFogDensity,       GetEnvironment().fogDensity);
			glUniform1f(m_uSkinnedFogHeightFalloff, GetEnvironment().fogHeightFalloff);
			// Same lights + CSM block as the unlit program.
			BindSceneLighting({ m_uSkinnedLightCount, m_uSkinnedLightPos, m_uSkinnedLightDir,
			                    m_uSkinnedLightColor, m_uSkinnedLightParams, m_uSkinnedCameraPos,
			                    m_uSkinnedShadowEnabled, m_uSkinnedShadowDebug, m_uSkinnedCascadeVP,
			                    m_uSkinnedCascadeSplits, m_uSkinnedCameraFwd, m_uSkinnedShadowMap,
			                    m_uSkinnedLocalShadowMap, m_uSkinnedLocalShadowVP, m_uSkinnedShadowBias,
			                    m_uSkinnedUnlit }, shadowFrame);
			// Re-assert the CSM array on unit 1 — opaque/instanced draws and the AO
			// bind run between the unlit setup and here; this guarantees the skinned
			// sampler2DArray reads the shadow array, not a stale 2D texture.
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
			// Local (point/spot) shadow atlas on unit 11 (same re-assert rationale).
			glActiveTexture(GL_TEXTURE11);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_localShadowDepthTex ? m_localShadowDepthTex : m_shadowDepthTex);
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
			glUniform1i(m_uSkinnedSkyEnv, 3);
			glActiveTexture(GL_TEXTURE4);
			glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
			glUniform1i(m_uSkinnedAO, 4);
			glActiveTexture(GL_TEXTURE0);
			glUniform2f(m_uSkinnedViewport, static_cast<float>(pw), static_cast<float>(ph));
			glUniform1i(m_uSkinnedSSAOEnabled, aoActive ? 1 : 0);
			glUniform1i(m_uSkinnedCloudShadowMap, 19);
			glUniform4fv(m_uSkinnedCloudShadowA, 1, glm::value_ptr(cloudShA));
			glUniform4fv(m_uSkinnedCloudShadowB, 1, glm::value_ptr(cloudShB));
			PushGISceneUniforms(m_giLocsSkinned, giShadingActive, giReflActive, ssrActive);

			constexpr int kMaxBones = 128;
			// Scratch buffer for the full bone matrix upload per draw call.
			// Filled with the draw's matrices, rest is identity (safe default).
			std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));

			GLWireScope _skinWire(WireframeViewActive());
			for (const SkinnedDrawCall& dc : cmds.skinnedDrawCalls())
			{
				const GpuSkeletalMesh* smesh = ResolveSkeletalMesh(dc.meshAssetId);
				if (!smesh) continue;

				// Refill scratch with identity, then copy the actual joint matrices.
				std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
				const int boneCount = static_cast<int>(
				    std::min(dc.boneMatrices.size(), static_cast<size_t>(kMaxBones)));
				if (boneCount > 0)
					std::copy_n(dc.boneMatrices.begin(), boneCount, boneScratch.begin());
				glUniformMatrix4fv(m_uSkinnedBones, kMaxBones, GL_FALSE,
				                   glm::value_ptr(boneScratch[0]));

				glm::vec3 baseColor(1.0f);
				unsigned int tex = smesh->texture;
				unsigned int overrideTex = 0;
				bool hasOverride = ResolveMaterialTexture(dc.materialAssetId, overrideTex);
				if (hasOverride) tex = overrideTex;
				float metallic = 0.0f, roughness = 0.5f, opacity = 1.0f;
				bool hasMat = ResolveMaterialParams(dc.materialAssetId, baseColor, metallic, roughness, opacity);
				if (!hasMat)
					baseColor = (tex != 0) ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);

				glUniformMatrix4fv(m_uSkinnedMVP,  1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
				glUniformMatrix4fv(m_uSkinnedModel, 1, GL_FALSE, glm::value_ptr(dc.transform));
				glUniform3fv(m_uSkinnedColor,       1, glm::value_ptr(baseColor));
				glUniform1f(m_uSkinnedMetallic,     metallic);
				glUniform1f(m_uSkinnedRoughness,    roughness);
				glUniform1f(m_uSkinnedOpacity,      1.0f);
				glUniform1i(m_uSkinnedNoShadow,     !dc.receivesShadow);
				glUniform1i(m_uSkinnedHasTex,       tex != 0);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glBindVertexArray(smesh->vao);
				// Section or whole — a multi-section skinned mesh arrives as one
				// SkinnedDrawCall per slot (GeometryPass), each with its range.
				const GlIndexRange range = DrawIndexRange(dc, smesh->indexCount);
				if (range.count <= 0) continue;
				glDrawElements(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(range.count / 3);
			}

			glUseProgram(m_unlitProgram); // restore for the sky + transparent passes
		}
		GpuTimerEndPass();                 // end "Opaque"

		// ── TAA velocity (A2): screen-space motion of the opaque geometry, right
		// after the pass whose depth it tests against. One spot serves both
		// paths: m_hdrDepth holds the opaque depth here on the forward path
		// directly and on the deferred path via the G-buffer blit above (plus
		// the forward-routed replay). Sibling timer scope — GL cannot nest.
		if (TaaActive())
		{
			GpuPassScope _velocityTimer(this, "Velocity");
			RenderVelocity(pw, ph, viewProjClean, viewProj);
		}

		GpuTimerBeginPass("Sky+Clouds");   // sibling (matches Metal)

		// ── Skybox (drawn LAST): fill the remaining background with the procedural
		// sky. The fullscreen triangle sits at z = 1 (far plane); with GL_LEQUAL and
		// no depth write it passes only where the geometry left depth == 1 (i.e. the
		// background), so the heavy sky shader is never paid for behind solid
		// objects. With no geometry the whole frame is background → full sky.
		// skyEnabled == false (no Sky entity) skips the pass → the cleared background shows.
		DrawSkyFullscreen(invViewProj, sunDir, m_renderWorld.camera.position,
		                  GetEnvironment(), /*allowLowResClouds=*/true, pw, ph);
		GpuTimerEndPass();                 // end "Sky+Clouds"
		GpuTimerBeginPass("Transparent");  // transparency + particles + debug lines

		// ── Motion trails join the blended list ─────────────────────────────
		// A RibbonBatch is world-space CPU geometry in the cooked vertex layout,
		// so it gets no pass and no program of its own: upload, resolve the
		// material exactly as a translucent mesh does, and hand it to the same
		// replay below (docs/rope-trail-plan.md §6.2). The model matrix is the
		// identity — the vertices are already in world space.
		for (size_t rbi = 0; rbi < m_renderWorld.ribbonBatches.size(); ++rbi)
		{
			const RibbonBatch& rb = m_renderWorld.ribbonBatches[rbi];
			// Material before the upload: every Resolve* below can LOAD, and a
			// load moves the ContentManager's dense pools.
			unsigned int rbTex = 0;
			const bool   rbHasTex = ResolveMaterialTexture(rb.materialAssetId, rbTex);
			glm::vec3 rbBase(1.0f);
			float rbMetal = 0.0f, rbRough = 0.5f, rbOpacity = 1.0f;
			if (!ResolveMaterialParams(rb.materialAssetId, rbBase, rbMetal, rbRough, rbOpacity))
				rbBase = rbHasTex ? glm::vec3(1.0f) : glm::vec3(0.55f, 0.55f, 0.55f);

			unsigned int rbProg = 0; std::vector<float> rbParams;
			unsigned int rbGtex[4] = { 0, 0, 0, 0 }; int rbGtexCount = 0;
#if defined(HE_HAVE_SHADERC)
			{
				uint64_t shKey = 0; std::string shFrag, shVert;
				if (resolveMaterialShader(rb.materialAssetId, shKey, shFrag, shVert))
				{
					std::vector<HE::UUID>    gIds;
					std::vector<std::string> gPaths;
					const MaterialShaderVariant* pre = nullptr;
					const MaterialAsset* ma = m_contentManager
						? m_contentManager->getMaterial(rb.materialAssetId) : nullptr;
					if (ma)
					{
						for (const auto& var : ma->precompiledShaders)
							if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { pre = &var; break; }
						rbParams = ma->shaderParamData;
						// Snapshot the graph texture slots BEFORE resolving any of
						// them — ResolveGraphTexture loads, and `ma` would not survive.
						const size_t nTex = std::min<size_t>(4,
							std::max(ma->graphTexturePaths.size(), ma->graphTextureIds.size()));
						for (size_t i = 0; i < nTex; ++i)
						{
							gIds.push_back(i < ma->graphTextureIds.size()     ? ma->graphTextureIds[i]     : HE::UUID{});
							gPaths.push_back(i < ma->graphTexturePaths.size() ? ma->graphTexturePaths[i]   : std::string{});
						}
					}
					rbProg = GetOrBuildMaterialProgram(shKey, shFrag, shVert, pre);
					if (rbProg)
						for (size_t i = 0; i < gIds.size(); ++i)
							rbGtex[rbGtexCount++] = ResolveGraphTexture(gIds[i], gPaths[i]);
					else
						rbParams.clear();
				}
			}
#endif
			const unsigned int rbVao = UploadRibbon(rbi, rb);
			if (!rbVao) continue;
			// Sort key from the band's own centre: backToFrontKey on an identity
			// transform would measure every trail's distance to the WORLD ORIGIN.
			const glm::vec3 rbC = rb.worldBounds.isValid() ? rb.worldBounds.center() : camPos;
			TPDraw tp{ viewProj, glm::mat4(1.0f), rbBase, rbMetal, rbRough, rbOpacity,
			           rbTex, rbVao, static_cast<int>(rb.indices.size()),
			           glm::dot(rbC - camPos, rbC - camPos) };
			tp.matProg = rbProg;
			tp.params  = std::move(rbParams);
			for (int i = 0; i < rbGtexCount; ++i) tp.gtex[i] = rbGtex[i];
			tp.gtexCount = rbGtexCount;
			transparent.push_back(std::move(tp));
		}

		// ── Transparency pass: sorted alpha-blended draws over the opaque scene +
		// sky. Back-to-front so the blend order is correct; depth-tested against the
		// opaque geometry (so transparent surfaces are occluded by closer solids)
		// but no depth write (so they don't occlude each other). The scene program's
		// per-frame uniforms (lights, ambient, shadow, AO) persist from the opaque
		// pass; only the per-draw material + alpha change.
		if (!transparent.empty())
		{
			std::sort(transparent.begin(), transparent.end(),
			          [](const TPDraw& a, const TPDraw& b) { return a.distSq > b.distSq; });
			glUseProgram(m_unlitProgram); // sky switched the active program
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDepthMask(GL_FALSE);
			glActiveTexture(GL_TEXTURE0);
			GLWireScope _tpWire(WireframeViewActive());
			for (const TPDraw& t : transparent)
			{
				if (t.matProg)
				{
					// Custom translucent material: its own program + UBOs (blend state is
					// global GL state, already enabled for this pass).
					glUseProgram(t.matProg);
					struct { glm::mat4 mvp, model; glm::vec4 color, flags, pbr; } obj;
					obj.mvp   = t.mvp;
					obj.model = t.model;
					obj.color = glm::vec4(t.baseColor, 1.0f);
					obj.flags = glm::vec4(t.tex != 0 ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
					obj.pbr   = glm::vec4(t.metallic, t.roughness, t.opacity, 0.0f);
					glBindBuffer(GL_UNIFORM_BUFFER, m_matObjUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(obj), &obj);
					float padded[64] = { 0 };
					std::memcpy(padded, t.params.data(),
					            std::min(t.params.size(), size_t(64)) * sizeof(float));
					glBindBuffer(GL_UNIFORM_BUFFER, m_matParamUBO);
					glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(padded), padded);
					glBindBuffer(GL_UNIFORM_BUFFER, 0);
					m_haveMatParams = false; // opaque-pass dedup cache no longer matches
					glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_matLightUBO);
					glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_matObjUBO);
					glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_matParamUBO);
					glBindVertexArray(t.vao);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, t.tex);
					for (int i = 0; i < t.gtexCount; ++i)
					{
						glActiveTexture(GL_TEXTURE1 + (GLenum)i);
						glBindTexture(GL_TEXTURE_2D, t.gtex[i]);
					}
					glActiveTexture(GL_TEXTURE0);
					glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, t.indexOffset);
					glUseProgram(m_unlitProgram); // restore the built-in blend program
					++m_counters.draws;
					m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
					continue;
				}
				glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(t.mvp));
				glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(t.model));
				glUniform3fv(m_uColor, 1, glm::value_ptr(t.baseColor));
				glUniform1f(m_uMetallic,  t.metallic);
				glUniform1f(m_uRoughness, t.roughness);
				glUniform1f(m_uOpacity,   t.opacity);
				glUniform1i(m_uNoShadow,  !t.receivesShadow);
				glUniform1i(m_uHasTexture, t.tex != 0);
				glBindVertexArray(t.vao);
				glBindTexture(GL_TEXTURE_2D, t.tex);
				glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, t.indexOffset);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
			}
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
		}

		// ── GPU weather particles: simulated by transform feedback (once per
		// frame in Render), drawn here as alpha-blended billboards over the scene.
		DrawGpuParticles(viewProj, m_renderWorld.camera.position);

		// ── ParticleGraph particles: GPU-instanced billboards, one draw call per
		// emitter (see RenderWorld::particleBatches / HE::generateParticleShaderSource).
		DrawParticleGraphBatches(viewProj, m_renderWorld.camera.view);

		// ── Debug line overlay: world-space segments over the opaque scene ────
		// Depth-test on so lines are occluded by geometry; depth-write off so
		// they don't mask later transparent objects.
		if (!m_debugLines.empty())
		{
			glDepthMask(GL_FALSE);
			DrawDebugLines(viewProj);
			glDepthMask(GL_TRUE);
		}
		GpuTimerEndPass();                 // end "Transparent"

		// Forward SSR: keep a full-res copy of the finished HDR frame (opaque +
		// sky + transparency) — NEXT frame's trace reprojects its hits into it.
		// Taken here, at the very end of the geometry pass, for the same reason
		// Metal takes it after its scene encoder: earlier and the reflection
		// would show a half-drawn world.
		if (!deferredActive && ssrFrameActive)
			CaptureSSRColorHistory(pw, ph);
	});

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	// This frame's camera becomes the motion-blur pass's "previous" one —
	// unconditionally, so the first frame after the pass is switched on
	// measures against last frame's camera, not against whatever was stored
	// when it was last on.
	m_mbPrevViewProj = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	m_mbHasPrev      = true;

	// One-time sanity check — GL errors are silent otherwise and a broken
	// draw path would just render nothing.
	static bool s_checkedFirstFrame = false;
	if (!s_checkedFirstFrame)
	{
		s_checkedFirstFrame = true;
		const GLenum err = glGetError();
		// Objects AND draws: an object that reaches the render world but issues no
		// draw, and one that draws into nothing, are different defects. The error
		// code is printed as real hex (it used to be decimal behind a literal "0x",
		// so GL_INVALID_OPERATION read as the nonexistent "0x1282"). Set
		// HE_GL_DEBUG=1 to have the driver name the call behind a non-zero code.
		char msg[192];
		std::snprintf(msg, sizeof(msg),
			"OpenGLRenderer: first scene frame drew %zu object(s) in %u draw call(s), glGetError=0x%04X",
			m_renderWorld.objects.size(), m_counters.draws, static_cast<unsigned>(err));
		Logger::LogTo(HE::Log::Cat::RHI,
			err == GL_NO_ERROR ? Logger::LogLevel::Info : Logger::LogLevel::Error, msg);
	}
}

void OpenGLRenderer::Render()
{
	// Make sure we are rendering into the primary window each frame
	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));

	// Profiler GPU-timer frame (primary window only — secondary RenderWindow shares
	// DrawScene but must not re-issue queries against this frame's slot). No-op unless
	// a capture is recording or the live HUD is open.
	GpuTimerBeginFrame();

	AgeRetiredTextures();

	// Step the GPU particle pool once per frame (before any DrawScene, which may run
	// for both the offscreen viewport and the window — drawing reads, only this steps).
	{
		GpuPassScope _ps(this, "ParticleSim");
		SimulateGpuParticles();
	}

	const bool offscreen = m_viewportReqW > 0 && m_viewportReqH > 0;

	if (offscreen)
	{
		// Scene → offscreen viewport target (shown by the editor as an image)
		EnsureViewportTarget();
		glBindFramebuffer(GL_FRAMEBUFFER, m_viewportFBO);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		DrawScene(m_viewportW, m_viewportH);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	else if (m_viewportFBO)
		DestroyViewportTarget();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (!offscreen)
	{
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(m_primarySdlWindow, &pw, &ph);
		DrawScene(pw, ph);
	}
	if (m_overlayCallback) m_overlayCallback(nullptr);

	// Close the GPU-timer frame (tsEnd; detailed capture flushes + reaps now).
	GpuTimerEndFrame();
}

// ─── GPU weather particles (transform-feedback precipitation) ─────────────────
// A fixed pool of drops lives in two interleaved VBOs (pos.xyz, vel.xyz, life,
// seed = 8 floats). The sim VS integrates + recycles each drop and writes the new
// state through transform feedback (rasterizer discard, no fragments). The draw VS
// pulls the freshest buffer as per-instance data and expands an attribute-less
// triangle-strip (gl_VertexID) into a camera-facing billboard. Runs on GL 4.1.
namespace {
constexpr int kParticleFloats = 8;   // pos3 vel3 life seed
static_assert(kParticleFloats == HE::kWeatherParticleFloats,
              "the VAO strides below and the shared seeder must agree on the record width");
constexpr int kParticleMax    = 1000000;

const char* kParticleSimVS = R"(#version 410 core
layout(location=0) in vec3  iPos;
layout(location=1) in vec3  iVel;
layout(location=2) in float iLife;
layout(location=3) in float iSeed;
out vec3  oPos;
out vec3  oVel;
out float oLife;
out float oSeed;
uniform float dt, time, coverage, fallSpeed, lifeSpan, groundLevel, boxHalf, boxTop, isSnow;
uniform vec3  camPos, wind;
float h21(vec2 p){ vec3 p3=fract(vec3(p.xyx)*0.1031); p3+=dot(p3,p3.yzx+33.33); return fract((p3.x+p3.y)*p3.z); }
void main(){
    oSeed = iSeed;
    float alive = step(iSeed, coverage);        // this slot participates at the current density
    vec3  pos = iPos + iVel * dt;
    vec3  vel = iVel;
    float life = iLife - dt;
    if (isSnow > 0.5) pos.x += sin((lifeSpan - life) * 2.2 + iSeed * 6.2831) * 0.5 * dt;
    bool dead = life <= 0.0 || pos.y <= groundLevel;
    if (dead) {
        if (alive > 0.5) {
            float ep = floor(time * 7.0) + iSeed * 131.0;          // respawn epoch
            float rx = h21(vec2(iSeed * 91.7, ep)) * 2.0 - 1.0;
            float rz = h21(vec2(ep, iSeed * 57.3)) * 2.0 - 1.0;
            pos = vec3(camPos.x + rx * boxHalf, camPos.y + boxTop, camPos.z + rz * boxHalf);
            vel = vec3(0.0, -fallSpeed, 0.0);
            if (isSnow > 0.5) {
                vel.x += (h21(vec2(ep, iSeed)) * 2.0 - 1.0) * 0.6 + wind.x * 0.3;
                vel.z += (h21(vec2(iSeed, ep)) * 2.0 - 1.0) * 0.6 + wind.z * 0.3;
            } else {
                vel.x += wind.x * 1.2;
                vel.z += wind.z * 1.2;
            }
            life = lifeSpan * (0.6 + 0.4 * iSeed);
        } else {
            life = -1.0;                                            // parked: stays invisible
            pos  = camPos + vec3(0.0, -100000.0, 0.0);
            vel  = vec3(0.0);
        }
    }
    oPos = pos; oVel = vel; oLife = life;
    gl_Position = vec4(0.0);   // unused (rasterizer discarded)
}
)";

const char* kParticleDrawVS = R"(#version 410 core
layout(location=0) in vec3  iPos;
layout(location=1) in vec3  iVel;
layout(location=2) in float iLife;
layout(location=3) in float iSeed;
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform float uSnow;
out vec2  vUV;
out float vSnow;
void main(){
    if (iLife <= 0.0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vUV = vec2(0.0); vSnow = uSnow; return; }
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) - 0.5; // quad corner [-0.5,0.5]
    vUV = c; vSnow = uSnow;
    vec3 look = uCamPos - iPos;
    float d = length(look);
    look = (d > 1e-4) ? look / d : vec3(0.0, 0.0, 1.0);
    vec3 worldPos;
    if (uSnow > 0.5) {
        const float s = 0.16;
        vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), look));
        vec3 up    = cross(look, right);
        worldPos = iPos + (right * c.x + up * c.y) * s;
    } else {
        vec3 vdir = iVel; float vl = length(vdir);
        vdir = (vl > 1e-4) ? vdir / vl : vec3(0.0, -1.0, 0.0);
        vec3 up = vdir - look * dot(vdir, look);
        up = (length(up) > 1e-4) ? normalize(up) : vec3(0.0, 1.0, 0.0);
        vec3 right = normalize(cross(up, look));
        worldPos = iPos + right * (c.x * 0.02) + up * (c.y * 0.6);
    }
    gl_Position = uViewProj * vec4(worldPos, 1.0);
}
)";

const char* kParticleDrawFS = R"(#version 410 core
in vec2  vUV;
in float vSnow;
out vec4 Frag;
void main(){
    if (vSnow > 0.5) {
        float a = smoothstep(0.5, 0.15, length(vUV)) * 0.9;       // soft round flake
        Frag = vec4(vec3(0.92, 0.95, 1.0), a);
    } else {
        float a = smoothstep(0.5, 0.0, abs(vUV.x)) * 0.45;        // soft thin streak
        Frag = vec4(vec3(0.55, 0.62, 0.78), a);
    }
}
)";

void setupParticleVAO(unsigned int vao, unsigned int buf, unsigned int divisor)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    const GLsizei stride = kParticleFloats * sizeof(float);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(7 * sizeof(float)));
    glVertexAttribDivisor(0, divisor); glVertexAttribDivisor(1, divisor);
    glVertexAttribDivisor(2, divisor); glVertexAttribDivisor(3, divisor);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
} // namespace

void OpenGLRenderer::CreateParticlePipeline()
{
    // Sim program: VS only, outputs captured by transform feedback (interleaved).
    {
        GLuint vs = CompileStage(GL_VERTEX_SHADER, kParticleSimVS);
        m_particleSimProgram = glCreateProgram();
        glAttachShader(m_particleSimProgram, vs);
        const char* varyings[] = { "oPos", "oVel", "oLife", "oSeed" };
        glTransformFeedbackVaryings(m_particleSimProgram, 4, varyings, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(m_particleSimProgram);
        glDeleteShader(vs);
        GLint ok = 0; glGetProgramiv(m_particleSimProgram, GL_LINK_STATUS, &ok);
        if (!ok) { GLchar log[512]; glGetProgramInfoLog(m_particleSimProgram, sizeof(log), nullptr, log);
                   throw std::runtime_error(std::string("OpenGLRenderer: particle sim link failed: ") + log); }
        glUseProgram(m_particleSimProgram);
        m_uPSimDt = glGetUniformLocation(m_particleSimProgram, "dt");
        m_uPSimTime = glGetUniformLocation(m_particleSimProgram, "time");
        m_uPSimCamPos = glGetUniformLocation(m_particleSimProgram, "camPos");
        m_uPSimWind = glGetUniformLocation(m_particleSimProgram, "wind");
        m_uPSimCoverage = glGetUniformLocation(m_particleSimProgram, "coverage");
        m_uPSimFall = glGetUniformLocation(m_particleSimProgram, "fallSpeed");
        m_uPSimLife = glGetUniformLocation(m_particleSimProgram, "lifeSpan");
        m_uPSimGround = glGetUniformLocation(m_particleSimProgram, "groundLevel");
        m_uPSimBoxHalf = glGetUniformLocation(m_particleSimProgram, "boxHalf");
        m_uPSimBoxTop = glGetUniformLocation(m_particleSimProgram, "boxTop");
        m_uPSimSnow = glGetUniformLocation(m_particleSimProgram, "isSnow");
    }
    // Draw program: billboard expansion + soft sprite shading.
    {
        GLuint vs = CompileStage(GL_VERTEX_SHADER, kParticleDrawVS);
        GLuint fs = CompileStage(GL_FRAGMENT_SHADER, kParticleDrawFS);
        m_particleDrawProgram = glCreateProgram();
        glAttachShader(m_particleDrawProgram, vs);
        glAttachShader(m_particleDrawProgram, fs);
        glLinkProgram(m_particleDrawProgram);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint ok = 0; glGetProgramiv(m_particleDrawProgram, GL_LINK_STATUS, &ok);
        if (!ok) { GLchar log[512]; glGetProgramInfoLog(m_particleDrawProgram, sizeof(log), nullptr, log);
                   throw std::runtime_error(std::string("OpenGLRenderer: particle draw link failed: ") + log); }
        m_uPDrawViewProj = glGetUniformLocation(m_particleDrawProgram, "uViewProj");
        m_uPDrawCamPos = glGetUniformLocation(m_particleDrawProgram, "uCamPos");
        m_uPDrawSnow = glGetUniformLocation(m_particleDrawProgram, "uSnow");
    }
    glGenBuffers(2, m_particleBuf);
    glGenVertexArrays(2, m_particleSimVAO);
    glGenVertexArrays(2, m_particleDrawVAO);
    glUseProgram(0);
}

void OpenGLRenderer::EnsureParticleBuffers(int count)
{
    if (count == m_particleCapacity) return;
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(count) * kParticleFloats * sizeof(float);
    for (int k = 0; k < 2; ++k)
    {
        glBindBuffer(GL_ARRAY_BUFFER, m_particleBuf[k]);
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_COPY);
        setupParticleVAO(m_particleSimVAO[k],  m_particleBuf[k], 0); // sim: per-vertex
        setupParticleVAO(m_particleDrawVAO[k], m_particleBuf[k], 1); // draw: per-instance
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_particleCapacity = count;
    m_particleCur      = 0;
    m_particleInit     = false;
}

void OpenGLRenderer::SeedParticleBuffer(int count)
{
    // Shared with Metal (HE::SeedWeatherParticles) — the RNG, the (i+0.5)/count
    // spread and the wind/life maths are a cross-backend contract, only the lane
    // order differs. This backend's TF sim shader reads pos, vel, life, seed.
    std::vector<float> data(static_cast<size_t>(count) * kParticleFloats);
    HE::SeedWeatherParticles(m_gpuParticles, count,
                             HE::WeatherParticleLayout::PosVelLifeSeed, data.data());
    glBindBuffer(GL_ARRAY_BUFFER, m_particleBuf[m_particleCur]);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_particleInit = true;
}

void OpenGLRenderer::SimulateGpuParticles()
{
    const GpuParticleParams& p = m_gpuParticles;
    if (!p.enabled) return;
    const int count = std::clamp(p.count, 0, kParticleMax);
    if (count <= 0) return;
    EnsureParticleBuffers(count);
    if (!m_particleInit) SeedParticleBuffer(count);

    const int src = m_particleCur, dst = 1 - m_particleCur;
    glUseProgram(m_particleSimProgram);
    glUniform1f(m_uPSimDt, p.dt);
    glUniform1f(m_uPSimTime, p.time);
    glUniform3f(m_uPSimCamPos, p.cameraPos.x, p.cameraPos.y, p.cameraPos.z);
    glUniform3f(m_uPSimWind, p.windVec.x, p.windVec.y, p.windVec.z);
    glUniform1f(m_uPSimCoverage, p.coverage);
    glUniform1f(m_uPSimFall, p.fallSpeed);
    glUniform1f(m_uPSimLife, p.lifeSpan);
    glUniform1f(m_uPSimGround, p.groundLevel);
    glUniform1f(m_uPSimBoxHalf, p.boxHalf);
    glUniform1f(m_uPSimBoxTop, p.boxTop);
    glUniform1f(m_uPSimSnow, p.isSnow ? 1.0f : 0.0f);

    glEnable(GL_RASTERIZER_DISCARD);
    glBindVertexArray(m_particleSimVAO[src]);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_particleBuf[dst]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, count);
    glEndTransformFeedback();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindVertexArray(0);
    glDisable(GL_RASTERIZER_DISCARD);
    glUseProgram(0);
    m_particleCur = dst;
}

void OpenGLRenderer::DrawGpuParticles(const glm::mat4& viewProj, const glm::vec3& camPos)
{
    const GpuParticleParams& p = m_gpuParticles;
    if (!p.enabled || m_particleCapacity <= 0) return;
    const int count = std::clamp(p.count, 0, m_particleCapacity);
    if (count <= 0) return;

    glUseProgram(m_particleDrawProgram);
    glUniformMatrix4fv(m_uPDrawViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
    glUniform3f(m_uPDrawCamPos, camPos.x, camPos.y, camPos.z);
    glUniform1f(m_uPDrawSnow, p.isSnow ? 1.0f : 0.0f);

    // Depth test stays as the scene left it (drops are occluded by closer geometry);
    // we only add alpha blending + disable depth write, like the transparency pass.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);                          // blend over the scene, don't occlude
    glBindVertexArray(m_particleDrawVAO[m_particleCur]);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void OpenGLRenderer::DrawParticleGraphBatches(const glm::mat4& viewProj, const glm::mat4& view)
{
	if (m_renderWorld.particleBatches.empty()) return;

	if (!m_particleVAO)
	{
		glGenVertexArrays(1, &m_particleVAO);
		glGenBuffers(1, &m_particleInstVBO);
		glBindVertexArray(m_particleVAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_particleInstVBO);
		constexpr GLsizei kStride = 5 * sizeof(float); // pos3 + size1 + t01(1)
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, (void*)0);
		glVertexAttribDivisor(0, 1);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(3 * sizeof(float)));
		glVertexAttribDivisor(1, 1);
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(4 * sizeof(float)));
		glVertexAttribDivisor(2, 1);
		glBindVertexArray(0);
	}

	// Camera-facing basis for billboard expansion (right = view row 0, up = view row 1) —
	// same convention as RenderParticlePreview.
	const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
	const glm::vec3 camUp   (view[0][1], view[1][1], view[2][1]);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE); // blend over the scene, occluded by (but doesn't occlude) opaque geometry
	glBindVertexArray(m_particleVAO);
	glActiveTexture(GL_TEXTURE0);

	std::vector<float> inst;
	for (const ParticleBatch& batch : m_renderWorld.particleBatches)
	{
		if (batch.instances.empty()) continue;

		// A precompiled OpenGL variant (export-baked, CHUNK_PPSD) wins over an
		// on-demand-compiled + hash-cached one — see GetOrBuildParticleProgram.
		const ParticleShaderVariant* precompiled = nullptr;
		if (const ParticleGraphAsset* asset = m_contentManager ? m_contentManager->getParticleGraph(batch.particleAssetId) : nullptr)
			for (const auto& var : asset->precompiledShaders)
				if (var.backend == static_cast<uint8_t>(HE::RendererBackend::OpenGL)) { precompiled = &var; break; }

		const uint64_t key = precompiled
			? (0x50505344ull /*"PPSD"*/ ^ (static_cast<uint64_t>(batch.particleAssetId.hi) * 0x9E3779B97F4A7C15ULL) ^ batch.particleAssetId.lo)
			: HE::hashParticleShaderConfig(batch.config);
		const unsigned int program = GetOrBuildParticleProgram(key, batch.config, precompiled);
		if (!program) continue;

		unsigned int tex = 0;
		const bool hasTex = ResolveMaterialTexture(batch.materialAssetId, tex);

		inst.clear();
		inst.reserve(batch.instances.size() * 5);
		for (const ParticleInstance& p : batch.instances)
			inst.insert(inst.end(), { p.position.x, p.position.y, p.position.z, p.size, p.t01 });

		glBindBuffer(GL_ARRAY_BUFFER, m_particleInstVBO);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(inst.size() * sizeof(float)),
		             inst.data(), GL_DYNAMIC_DRAW);

		glUseProgram(program);
		glUniformMatrix4fv(glGetUniformLocation(program, "uViewProj"), 1, GL_FALSE, glm::value_ptr(viewProj));
		glUniform3fv(glGetUniformLocation(program, "uCamRight"), 1, glm::value_ptr(camRight));
		glUniform3fv(glGetUniformLocation(program, "uCamUp"),    1, glm::value_ptr(camUp));
		glUniform1i(glGetUniformLocation(program, "uHasTex"), hasTex);
		glBindTexture(GL_TEXTURE_2D, hasTex ? tex : 0);
		glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(batch.instances.size()));
		++m_counters.draws;
		m_counters.tris += static_cast<uint32_t>(batch.instances.size()) * 2;
	}

	glBindVertexArray(0);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glUseProgram(0);
}

void OpenGLRenderer::DestroyParticleResources()
{
    if (m_particleSimProgram)  { glDeleteProgram(m_particleSimProgram);  m_particleSimProgram = 0; }
    if (m_particleDrawProgram) { glDeleteProgram(m_particleDrawProgram); m_particleDrawProgram = 0; }
    if (m_particleBuf[0]) glDeleteBuffers(2, m_particleBuf);
    if (m_particleSimVAO[0]) glDeleteVertexArrays(2, m_particleSimVAO);
    if (m_particleDrawVAO[0]) glDeleteVertexArrays(2, m_particleDrawVAO);
    m_particleBuf[0] = m_particleBuf[1] = 0;
    m_particleSimVAO[0] = m_particleSimVAO[1] = 0;
    m_particleDrawVAO[0] = m_particleDrawVAO[1] = 0;
    m_particleCapacity = 0;
    m_particleInit = false;
}

IRenderer::Capabilities OpenGLRenderer::GetCapabilities() const
{
	Capabilities c;
	c.supportsShadows        = true;
	c.supportsPostProcessing = true;
	c.supportsHDR            = true;
	// Transform feedback is core in GL 4.1, so the GPU precipitation path runs on
	// every GL context this backend creates (incl. macOS 4.1).
	c.supportsGpuParticles   = true;
	// Ray-traced GI via compute + CPU BVH: needs a GL 4.3 context (Windows/
	// Linux). macOS GL is 4.1 → false there (Metal covers GI on Apple).
	c.supportsGlobalIllumination = m_giSupported;
	// Ray-traced GI reflections ride the same compute + BVH infrastructure and
	// composite in the shading pass, so they need neither a deferred G-buffer
	// nor hardware ray tracing — same 4.3 gate, both render paths.
	c.supportsGIReflections      = m_giSupported;
#if defined(HE_HAVE_SHADERC)
	// Deferred needs only MRT + a runtime-generated resolve shader — GL 4.1 is
	// enough, but the shader cross-compiler must be built in.
	c.supportsDeferredRendering  = true;
	// SSR: the FORWARD variant only (docs/ssr-cross-backend-plan.md checkpoint
	// A) — the reflection MRT pre-pass plus last frame's HDR copy, exactly
	// Metal's Option-A path. The deferred composite (A6) is not built, so in
	// deferred mode the gate stays 0 and the image is unchanged. Programs come
	// from the shared cross-compiler, hence the #if.
	c.supportsScreenSpaceReflections = true;
#endif
	// TAA (A2/A3): velocity pass + temporal resolve + sharpen, all #version 410
	// fullscreen/positions-only programs — both render paths, every GL context
	// this backend creates. False only if one of the three failed to link.
	c.supportsTemporalAA = m_taaVelocityProgram != 0 && m_taaProgram != 0
	                    && m_taaSharpenProgram != 0;
	return c;
}

// ─── GPU timing (profiler per-pass trace) ─────────────────────────────────────
// GL timer queries: GL_TIME_ELAPSED per pass (exact, exclusive, additive — no TBDR
// overlap on an immediate-mode GPU, so the per-pass sum is meaningful) + a
// GL_TIMESTAMP pair for the whole frame. Single-threaded: every call is on the
// GL/main thread inside Render(), so no locking is needed (unlike Metal's async
// completion handlers / GpuPassAccumulator).

void OpenGLRenderer::GpuTimerReap(GpuTimerSlot& slot)
{
	if (!slot.pending) return;
	GLuint64 t0 = 0, t1 = 0;
	glGetQueryObjectui64v(slot.tsStart, GL_QUERY_RESULT, &t0); // ns; blocks iff not ready
	glGetQueryObjectui64v(slot.tsEnd,   GL_QUERY_RESULT, &t1);
	FrameGpuStats fs;
	fs.gpuFrameMs    = (t1 > t0) ? static_cast<double>(t1 - t0) * 1e-6 : 0.0;
	fs.gpuTimingMode = "gl-timer";
	fs.passes.reserve(slot.passes.size());
	for (const GpuTimerPass& p : slot.passes)
	{
		GLuint64 elapsed = 0;
		glGetQueryObjectui64v(p.query, GL_QUERY_RESULT, &elapsed);
		fs.passes.push_back({ p.name, static_cast<double>(elapsed) * 1e-6, /*approx=*/false });
	}
	m_lastGpuStats = fs;   // CPU counters are merged in by GetFrameGpuStats
	slot.pending   = false;
}

void OpenGLRenderer::GpuTimerBeginFrame()
{
	// Latch the profiler decision once per primary frame so Begin/EndPass + EndFrame
	// agree (a mid-frame toggle can never unbalance a query begin/end pair).
	EngineProfiler& prof = EngineProfiler::instance();
	const bool rec  = prof.isRecording();
	const bool live = prof.liveEnabled();
	m_gpuTimingActive = m_gpuTimerSupported && (rec || live);
	m_gpuPerPass      = m_gpuTimingActive && rec;
	// Same-frame reap (glFinish) for the detailed-capture toggle AND for a one-shot
	// single-frame capture. A single-frame capture records exactly one frame and the
	// profiler reads its stats that same frame, so the async ring (results 1–N frames
	// late) would attribute a DIFFERENT frame's GPU times to it. The flush costs one
	// stall on that single frame only; the per-pass GL_TIME_ELAPSED values stay exact.
	m_gpuDetailed     = m_gpuPerPass && (prof.detailedGpuCapture() || prof.isSingleFrameCapture());
	const bool freshActivation = m_gpuTimingActive && !m_gpuWasActive;
	m_gpuWasActive    = m_gpuTimingActive;
	m_gpuCurSlot      = -1;
	m_gpuActiveQuery  = -1;
	if (!m_gpuTimingActive) return;

	if (!m_gpuTimerInit)
	{
		for (GpuTimerSlot& s : m_gpuSlots)
		{
			glGenQueries(1, &s.tsStart);
			glGenQueries(1, &s.tsEnd);
		}
		m_gpuTimerInit = true;
	}

	// Fresh activation (profiler just turned on after being idle): slots left pending
	// by a PRIOR session hold that session's results. Drop them (don't reap) and clear
	// the published stats, so the first frames honestly report "warming up"
	// (gpuFrameMs = -1) instead of stale cross-session numbers, until the ring fills.
	if (freshActivation)
	{
		for (GpuTimerSlot& s : m_gpuSlots) s.pending = false;
		m_lastGpuStats = FrameGpuStats{};
	}

	const int slotIdx  = static_cast<int>(m_gpuFrameIdx % HE::kGpuTimerRing);
	GpuTimerSlot& slot = m_gpuSlots[slotIdx];
	// This slot was last used HE::kGpuTimerRing frames ago, so its results are certainly
	// ready — reaping here never stalls (no-op if the detailed path already reaped it).
	GpuTimerReap(slot);
	slot.passes.clear();
	slot.poolUsed = 0;
	slot.frameIdx = m_gpuFrameIdx;
	m_gpuCurSlot  = slotIdx;

	glQueryCounter(slot.tsStart, GL_TIMESTAMP);
}

void OpenGLRenderer::GpuTimerEndFrame()
{
	if (!m_gpuTimingActive || m_gpuCurSlot < 0) { ++m_gpuFrameIdx; return; }
	GpuTimerSlot& slot = m_gpuSlots[m_gpuCurSlot];
	glQueryCounter(slot.tsEnd, GL_TIMESTAMP);
	slot.pending = true;

	if (m_gpuDetailed)
	{
		// Detailed / single-frame capture: attribute THIS frame's GPU numbers to THIS
		// frame (the profiler reads GetFrameGpuStats right after this in the loop).
		// Flush so the queries are done, then reap now. One glFinish per frame, only
		// while the user opted into detailed capture (mirrors Metal's serialization
		// trade-off — the per-pass GL_TIME_ELAPSED values themselves stay exact).
		glFinish();
		GpuTimerReap(slot);
	}
	m_gpuCurSlot = -1;
	++m_gpuFrameIdx;
}

bool OpenGLRenderer::GpuTimerBeginPass(const char* name)
{
	if (!m_gpuPerPass || m_gpuCurSlot < 0) return false;
	if (m_gpuActiveQuery >= 0) return false;   // GL_TIME_ELAPSED cannot nest
	GpuTimerSlot& slot = m_gpuSlots[m_gpuCurSlot];
	if (slot.poolUsed >= slot.pool.size())
	{
		GLuint q = 0;
		glGenQueries(1, &q);
		slot.pool.push_back(q);
	}
	const unsigned int q = slot.pool[slot.poolUsed];
	m_gpuActiveQuery = static_cast<int>(slot.poolUsed);
	++slot.poolUsed;
	slot.passes.push_back({ name, q });
	glBeginQuery(GL_TIME_ELAPSED, q);
	return true;
}

void OpenGLRenderer::GpuTimerEndPass()
{
	if (m_gpuActiveQuery < 0) return;
	glEndQuery(GL_TIME_ELAPSED);
	m_gpuActiveQuery = -1;
}

void OpenGLRenderer::DestroyGpuTimer()
{
	if (!m_gpuTimerInit) return;
	for (GpuTimerSlot& s : m_gpuSlots)
	{
		if (s.tsStart) glDeleteQueries(1, &s.tsStart);
		if (s.tsEnd)   glDeleteQueries(1, &s.tsEnd);
		if (!s.pool.empty())
			glDeleteQueries(static_cast<GLsizei>(s.pool.size()), s.pool.data());
		s = GpuTimerSlot{};
	}
	m_gpuTimerInit = false;
}

IRenderer::FrameGpuStats OpenGLRenderer::GetFrameGpuStats() const
{
	// GPU times come from the newest reaped timer slot (1–N frames late; -1 and no
	// passes on Apple GL / before the first reap). CPU counters are this frame's.
	FrameGpuStats s = m_lastGpuStats;
	s.drawCalls      = m_counters.draws;
	s.triangles      = m_counters.tris;
	s.visibleObjects  = m_counters.visible;
	s.totalObjects    = m_counters.total;
	s.occlusionCulled = m_counters.occlusionCulled;
	return s;
}

void OpenGLRenderer::SetGpuParticleParams(const GpuParticleParams& p)
{
	m_gpuParticles = p;
}

// ─── Multi-window support ─────────────────────────────────────────────────────

void OpenGLRenderer::AttachWindow(HE::Window* window)
{
	SDL_Window* sdlWin = window->GetNativeWindow();
	if (m_secondaryContexts.count(sdlWin)) return; // already attached

	// Create a new GL context that shares display lists / textures with the primary
	SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	SDL_GLContext sharedCtx = SDL_GL_CreateContext(sdlWin);
	if (!sharedCtx)
		throw std::runtime_error(std::string("OpenGLRenderer: SDL_GL_CreateContext failed for secondary window: ")
								 + SDL_GetError());
	m_secondaryContexts[sdlWin] = static_cast<void*>(sharedCtx);

	// Restore primary context
	SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: secondary window attached");
}

void OpenGLRenderer::DetachWindow(HE::Window* window)
{
	auto it = m_secondaryContexts.find(window->GetNativeWindow());
	if (it == m_secondaryContexts.end()) return;
	SDL_GL_DestroyContext(static_cast<SDL_GLContext>(it->second));
	m_secondaryContexts.erase(it);
	// Restore primary context so the next Render() call works
	if (m_primarySdlWindow && m_glContext)
		SDL_GL_MakeCurrent(m_primarySdlWindow, static_cast<SDL_GLContext>(m_glContext));
	HE_LOG_INFO(RHI, "%s", "OpenGLRenderer: secondary window detached");
}

void OpenGLRenderer::RenderWindow(HE::Window* window)
{
	auto it = m_secondaryContexts.find(window->GetNativeWindow());
	if (it == m_secondaryContexts.end()) return;

	// It says so, once, instead of painting a black rectangle (A5,
	// docs/he-apps-plan.md §13.3). There is no UI-only path here yet: Metal and
	// the software rasterizer have one, this backend has a cleared buffer and a
	// TODO, and a window that opens onto that is worse than one that does not
	// open. GetCapabilities().supportsSecondaryWindows is false for exactly that
	// reason, so createSecondaryWindow refuses before anything gets this far —
	// this is the second line of defence, for a host that attached one anyway.
	static bool warned = false;
	if (!warned)
	{
		warned = true;
		HE_LOG_WARN(RHI, "%s",
			"OpenGLRenderer: no second-window draw path — nothing is drawn there "
			"(Software and Metal have one)");
	}
	SDL_GL_MakeCurrent(window->GetNativeWindow(), static_cast<SDL_GLContext>(it->second));
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	// SwapBuffers is called by Application::Run after this method returns
}

void* OpenGLRenderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	GLuint texId = 0;
	glGenTextures(1, &texId);
	glBindTexture(GL_TEXTURE_2D, texId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8Pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return reinterpret_cast<void*>(static_cast<uintptr_t>(texId));
}

void OpenGLRenderer::DestroyImGuiTexture(void* handle)
{
	if (!handle) return;
	GLuint texId = static_cast<GLuint>(reinterpret_cast<uintptr_t>(handle));
	glDeleteTextures(1, &texId);
}

void OpenGLRenderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
	if (!rgba8Pixels || width <= 0 || height <= 0) return;
	if (m_moonTex) { glDeleteTextures(1, &m_moonTex); m_moonTex = 0; }
	glGenTextures(1, &m_moonTex);
	glBindTexture(GL_TEXTURE_2D, m_moonTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8Pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
}
