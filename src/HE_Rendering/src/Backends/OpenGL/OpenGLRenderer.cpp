#include "Backends/OpenGL/OpenGLRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <Diagnostics/Logger.h>
#include <Diagnostics/EngineProfiler.h>
#include <JobSystem/JobSystem.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

// Builds a tiling NxNxN two-channel noise volume (interleaved RG16):
//   R = value noise whose lattice values are exactly the sky shader's
//       starHash(i,j,k). With the shader pre-smoothstepping the fractional sample
//       coordinate, the GPU's trilinear filter reproduces the old smoothstep value
//       noise exactly (within one tile) — so starFbm3 stays a texture fetch with no
//       visible change (nebula/stars unaffected, byte-for-byte).
//   G = tiling inverted-Worley (cellular) field, bright at the cell feature points.
//       fBm of this is the billowy "cauliflower" shape that turns the clouds from
//       wispy value-noise blobs into rounded cumuli. Worley is C0-smooth so plain
//       trilinear sampling of the bake is fine (no pre-smoothstep trick needed).
// R16-per-channel keeps the threshold ramps band-free. Shared with the Metal gen.
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

	// Serial nested loops (one-time init): each voxel is fully independent.
	// Matches the D3D11/D3D12/Vulkan bakes — portable across compilers without
	// the parallel STL (<execution> is unimplemented in libc++/Apple Clang).
	for (int z = 0; z < n; ++z)
		for (int y = 0; y < n; ++y)
			for (int x = 0; x < n; ++x)
			{
				const size_t idx = ((static_cast<size_t>(z) * n + y) * n + x) * 2;
				glm::vec3 uv((x + 0.5f) * inv, (y + 0.5f) * inv, (z + 0.5f) * inv);
				d[idx + 0] = static_cast<uint16_t>(
					glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
				d[idx + 1] = static_cast<uint16_t>(worley(uv) * 65535.0f + 0.5f);
			}
	return d;
}

// CPU port of the shader's analytic skyColor(dir,sunDir) — used to bake the
// image-based-ambient cubemap so the scene shader samples it once instead of
// re-evaluating this function twice per lit pixel. Mirrors kSkyFuncGLSL exactly.
static glm::vec3 SkyColorCPU(glm::vec3 dir, glm::vec3 sunDir)
{
	dir = glm::normalize(dir); sunDir = glm::normalize(sunDir);
	float sunY = glm::clamp(sunDir.y, -0.3f, 1.0f);
	float day  = glm::smoothstep(-0.10f, 0.10f, sunY);
	// Mirror the GLSL skyColor: extended warm-horizon + 3-stage day→blue-hour→night blend
	// so the baked ambient/reflection lighting matches the visible sky (incl. the blue hour).
	float dusk = glm::smoothstep(-0.14f, 0.04f, sunY) * (1.0f - glm::smoothstep(0.04f, 0.26f, sunY));
	float toDay   = glm::smoothstep(-0.08f, 0.10f, sunY);
	float toNight = 1.0f - glm::smoothstep(-0.24f, -0.06f, sunY);
	glm::vec3 zenith  = glm::mix(glm::mix(glm::vec3(0.030f,0.055f,0.17f), glm::vec3(0.09f,0.30f,0.78f), toDay), glm::vec3(0.003f,0.005f,0.015f), toNight);
	glm::vec3 horizon = glm::mix(glm::mix(glm::vec3(0.055f,0.075f,0.19f), glm::vec3(0.50f,0.66f,0.90f), toDay), glm::vec3(0.006f,0.009f,0.024f), toNight);
	glm::vec2 sunAz = glm::normalize(glm::vec2(sunDir.x, sunDir.z) + glm::vec2(1e-5f));
	float toward = glm::dot(glm::normalize(glm::vec2(dir.x, dir.z) + glm::vec2(1e-5f)), sunAz) * 0.5f + 0.5f;
	toward = std::pow(glm::clamp(toward, 0.0f, 1.0f), 1.8f);
	glm::vec3 duskHoriz = glm::mix(glm::vec3(0.26f,0.18f,0.40f), glm::vec3(0.92f,0.42f,0.14f), toward);
	horizon = glm::mix(horizon, duskHoriz, dusk);
	zenith  = glm::mix(zenith, glm::vec3(0.11f,0.11f,0.30f), dusk * 0.6f);
	float h = glm::clamp(dir.y, 0.0f, 1.0f);
	glm::vec3 sky = glm::mix(zenith, horizon, std::pow(1.0f - h, 2.5f));
	sky += glm::vec3(0.95f,0.50f,0.16f) * (std::pow(1.0f - h, 8.0f) * toward * dusk * 0.70f);
	sky += glm::vec3(0.60f,0.34f,0.14f) * (std::pow(1.0f - h, 3.5f) * toward * dusk * 0.30f);
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
	sky += glm::vec3(0.015f,0.018f,0.030f) * night;
	return sky;
}

// Builds the six cube faces of the image-based-ambient environment map for the
// given sun direction (face = +X,-X,+Y,-Y,+Z,-Z in GL order). Returns tightly
// packed RGBA32F, faces back to back, faceN texels each.
static std::vector<float> BuildSkyEnvCube(int faceN, const glm::vec3& sunDir)
{
	std::vector<float> px(static_cast<size_t>(faceN) * faceN * 6 * 4);
	// Parallelize over the 6*faceN rows (face,row): each row is independent and
	// SkyColorCPU is a pure function, so this is data-race-free. Uses the engine's
	// portable thread pool — NOT std::execution::par (which libc++/macOS lacks). At
	// 128² this serial bake was ~47 ms (a per-frame stall under day-night auto-advance);
	// parallel it is a few ms.
	parallel_for(static_cast<size_t>(6) * faceN, [&](size_t idx)
	{
		const int f = static_cast<int>(idx / faceN);
		const int t = static_cast<int>(idx % faceN);
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
			size_t i = ((static_cast<size_t>(f) * faceN + t) * faceN + s) * 4;
			px[i+0] = c.r; px[i+1] = c.g; px[i+2] = c.b; px[i+3] = 1.0f;
		}
	});
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

// shared skyColor() is injected at the marker below (CreateUnlitPipeline)
//#SKYFUNC#

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
	float bias = clamp(0.0008 * tan(acos(ndl)), 0.0002, 0.02) * float(c + 1);
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

void main()
{
	vec3 albedo = uHasTexture ? texture(uTexture, vUV).rgb * uColor : uColor;
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
	vec3 ambSpec = texture(uSkyEnv, Rrough).rgb * specColor;
	vec3 ambient = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * wRough);
	// Screen-space ambient occlusion darkens only the IBL indirect term in
	// crevices; the direct lighting added below is left untouched. 1.0 = fully lit.
	float ao = (uSSAOEnabled == 1) ? texture(uAO, gl_FragCoord.xy / uViewport).r : 1.0;
	// Flat ambient fill (never-black floor + overcast replacement) is intentionally
	// kept outside the AO product so SSAO over-darkening at grazing angles cannot
	// zero it out. It is the minimum guaranteed brightness on any surface.
	vec3 result  = ambient * ao + uAmbient * diffuseColor;

	int dbgCascade = 0;   // cascade chosen by the directional shadow (debug tint)
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
		float sh = 1.0;
		if (type == 0) sh = computeShadow(vWorldPos, N, L, dbgCascade);

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

static const char* kSkyFS = R"GLSL(
#version 410 core
in vec2 vNDC;
uniform mat4 uInvViewProj;
uniform vec3 uSunDir;
uniform sampler2D uMoonTex;
uniform bool      uHasMoonTex;
uniform float     uMoonPhase;   // lunar phase: 0/1 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter
uniform float     uTimeOfDay;   // day phase 0..1 (celestial rotation)
uniform float     uCloudCoverage; // cloud amount (0 = clear … 1 = full overcast)
uniform float     uTime;        // wall-clock seconds (star twinkle)
uniform vec3      uSunColor;    // sun light colour (tints the clouds)
uniform float     uAurora;      // aurora intensity (0 = off)
uniform float     uAuroraHeight;   // aurora band elevation (0 low … 1 high)
uniform float     uAuroraFragment; // aurora streak fragmentation (0 solid … 1 broken)
uniform float     uMilkyWay;    // milky-way star-lane density/brightness
uniform float     uNebula;      // space-nebula intensity (0 = off)
uniform vec3      uNebulaColor; // space-nebula colour 1 (cool regions)
uniform vec3      uNebulaColor2; // space-nebula colour 2 (mid regions)
uniform vec3      uNebulaColor3; // space-nebula colour 3 (warm regions)
uniform float     uNebulaSeed;  // space-nebula randomisation seed
uniform float     uNebulaHiFi;  // 0 = high-performance nebula, 1 = high-fidelity (sharp forms)
uniform vec3      uAuroraColor;    // aurora lower/base colour (e.g. green)
uniform vec3      uAuroraColorTop; // aurora upper colour (e.g. purple)
uniform vec3      uWind;        // cloud drift vector (world units / s, horizontal)
uniform sampler3D uNoise;       // tiling 3D value-noise (replaces the hash fbm)
uniform sampler2D uCloudTex;    // quarter-res cloud buffer (rgb = L, a = T) for low-res clouds
uniform float     uLowResClouds; // 1 = composite uCloudTex instead of the inline raymarch
uniform float     uCloudPrepass; // 1 = this draw outputs (L, T) only (the quarter-res cloud pass)
uniform float     uFlash;       // lightning flash (0 = none … 1 = full strike)
uniform int       uCloudMode;   // 0 = sky-dome clouds, 1 = 3D volumetric (world-anchored)
uniform int       uCloudQuality; // cloud raymarch quality: 0 Low, 1 Med, 2 High (perf knob)
uniform vec3      uCameraPos;   // camera world position (for 3D-cloud parallax)
uniform float     uCloudHeight; // 3D cloud layer height above the camera (world units)
uniform float     uCloudDensity;    // cloud opacity/density multiplier (1 = default)
uniform float     uCloudFluffiness; // cauliflower erosion strength (0 sheet … 1 billowy)
uniform vec3      uCloudTint;        // cloud colour tint
uniform float     uContrails;        // contrail (vapour-trail) amount (0 = off)
uniform float     uCirrus;           // thin high 2D cirrus cloud amount (0 = off)
uniform float     uCirrusSeed;       // cirrus pattern seed
uniform float     uStarBright;       // star field brightness multiplier
uniform vec3      uStarColor;        // star field colour tint
uniform float     uStarSize;         // overall star size multiplier
uniform float     uStarSizeVar;      // star size variation (0 = uniform … 1 = wide spread)
uniform float     uStarGlow;         // glow/halo around stars (0 = points only)
uniform float     uStarTwinkle;      // twinkle amount (0 = steady … 1 = strong blink)
uniform float     uStarDensity;      // amount of stars (0 = few … 1 = many; 0.5 = default)
out vec4 FragColor;
//#SKYFUNC#

// Self-contained 2D value-noise fBm for the moon surface (the cloud/star noise helpers are
// defined further down the shader, and GLSL has no forward declaration, so the moon — one of
// the first functions — needs its own).
float moonHash(vec2 p){ p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 34.56); return fract(p.x * p.y); }
float moonNoise(vec2 p){ vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
	return mix(mix(moonHash(i), moonHash(i + vec2(1,0)), u.x),
	           mix(moonHash(i + vec2(0,1)), moonHash(i + vec2(1,1)), u.x), u.y); }
float moonFbm(vec2 p){ float v = 0.0, a = 0.5; for (int i = 0; i < 4; ++i){ v += a * moonNoise(p); p *= 2.03; a *= 0.5; } return v; }

// Textured moon disk — drawn only in the sky pass (kept out of the shared
// skyColor() so the scene's image-based ambient needn't bind the texture).
// Smaller than the sun and shaded as a sphere so the grayscale map reads as
// craters on a lit body. Falls back to a plain disk when no texture is set.
vec3 moonDisk(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day   = smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	float night = 1.0 - day;
	if (night <= 0.0) return vec3(0.0);

	vec3 moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return vec3(0.0);

	// Local tangent frame so the disk gets 2D UVs for the texture.
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), moonDir));
	vec3 up    = cross(moonDir, right);
	const float kRadius = 0.030;                   // angular radius (< the sun disk)
	vec2  q = vec2(dot(dir, right), dot(dir, up)) / kRadius;
	float r = length(q);
	if (r > 1.0) return vec3(0.0);

	// Sphere normal (z toward viewer) + a UV that bulges toward the limb for a rounder wrap.
	float z   = sqrt(max(1.0 - r * r, 0.0));
	vec2  uv  = q / (0.55 + 0.45 * z);
	// ---- Procedural lunar SURFACE ALBEDO (maria seas + cratered highlands + ray system) ----
	float hl    = moonFbm(uv * 2.0 + 11.0);                       // highland mottle (bright base)
	float albedo = 0.74 + 0.16 * (hl - 0.5);
	// Maria: large dark basaltic seas — irregular, smooth, with faint internal variation.
	float mar   = moonFbm(uv * 0.95 + 4.0);
	float maria = smoothstep(0.44, 0.60, mar);
	albedo = mix(albedo, 0.22 + 0.07 * (moonFbm(uv * 3.0 + 20.0) - 0.5), maria);
	// Medium cratering: darker mottling + a touch of bright rim where the field peaks.
	float cm    = moonFbm(uv * 6.0 + 31.0);
	albedo *= 0.82 + 0.20 * smoothstep(0.30, 0.72, cm);
	albedo += 0.05 * (moonFbm(uv * 16.0 + 50.0) - 0.5);          // fine grain
	// A bright young crater with a RAY system (Tycho-like) — bright streaks radiating out.
	vec2  tc   = vec2(0.10, -0.40);
	float td   = length(uv - tc);
	float tang = atan(uv.y - tc.y, uv.x - tc.x);
	float rayN = moonFbm(vec2(tang * 3.0, 1.7));
	float rays = pow(0.5 + 0.5 * sin(tang * 22.0 + rayN * 9.0), 3.0);
	rays *= smoothstep(0.85, 0.12, td) * smoothstep(0.05, 0.10, td); // fade out + hollow centre
	albedo += rays * 0.20;
	albedo += smoothstep(0.060, 0.048, td) * 0.22;               // bright crater rim
	albedo -= smoothstep(0.048, 0.022, td) * 0.14;               // darker crater floor
	albedo = clamp(albedo, 0.12, 1.05);
	float tex  = uHasMoonTex ? texture(uMoonTex, q * 0.5 + 0.5).r : 1.0;
	albedo *= mix(1.0, tex, uHasMoonTex ? 0.55 : 0.0);           // blend the real texture if present
	// ---- PHASE: light the sphere from the sun's direction in the moon-view frame ----
	// uMoonPhase 0/1 = new (dark), 0.25 = first quarter (right lit), 0.5 = full, 0.75 = last quarter.
	vec3  N   = vec3(q, z);                                       // surface normal toward the viewer
	float ph  = uMoonPhase * 6.2831853;
	vec3  L   = vec3(sin(ph), 0.0, -cos(ph));                     // sun direction across the disk
	float ndl = dot(normalize(N), L);
	float illum = smoothstep(-0.06, 0.08, ndl);                  // soft day/night terminator
	illum = max(illum, 0.025 * (1.0 - illum));                   // faint earthshine on the dark side
	float limb = 0.55 + 0.45 * z;                                // mild edge darkening (rough body, not limb-darkened)
	float edge = smoothstep(1.0, 0.93, r);                       // soft anti-aliased rim
	vec3  tint = vec3(0.92, 0.93, 0.99);
	return tint * (albedo * illum * limb * edge * 1.3 * night); // lower brightness so maria/rays read
}

// Procedural star field + Milky Way — drawn only in the sky pass (like the
// moon). The whole celestial sphere turns about a tilted pole with the time of
// day (celestialDir) so the stars, the dense galactic star lane and the nebulae
// drift across the sky as the Earth rotates. Night/horizon gating uses the real
// view ray; the pattern is sampled in the rotated frame. Mirrors Metal exactly.
float starHash(vec3 p)
{
	p  = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}
// Rotate a view ray into the slowly turning celestial frame (one full turn per
// day about a tilted pole) — Rodrigues' rotation.
vec3 celestialDir(vec3 dir, float timeOfDay)
{
	float a    = timeOfDay * 6.2831853;
	vec3  axis = normalize(vec3(0.22, 0.92, 0.32));
	float c = cos(a), s = sin(a);
	return dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0 - c);
}
// Gaussian galactic band: ~1 on the Milky-Way plane, 0 toward the poles.
float galacticBand(vec3 cdir)
{
	const vec3 galN = normalize(vec3(0.46, 0.52, -0.72));
	float d = dot(normalize(cdir), galN);
	return exp(-d * d * 7.0);
}
// 3D value noise (trilinear) from the star hash + a small fBm. The nebula is
// sampled in 3D on the celestial sphere so it reads as isotropic blobs instead
// of the radial streaks a 2D plane projection produces at grazing angles.
// Trilinear value noise sampled from the precomputed uNoise volume (texels hold
// starHash at the integer lattice). Pre-smoothstepping the fractional coordinate
// makes the hardware linear filter reproduce the old smoothstep interpolation,
// and +0.5 lands integer lattice points on texel centres — so the result matches
// the former hash-based starNoise3 (within the 128-unit tile) at far less ALU.
float starNoise3(vec3 p)
{
	vec3 f = fract(p);
	vec3 q = floor(p) + f * f * (3.0 - 2.0 * f) + 0.5;
	return texture(uNoise, q * (1.0 / 256.0)).r;
}
float starFbm3(vec3 p, int oct)
{
	float v = 0.0, amp = 0.5;
	for (int i = 0; i < oct; ++i) { v += amp * starNoise3(p); p *= 2.03; amp *= 0.5; }
	return v;
}
// Dark dust lanes of the Milky Way — the "Great Rift": broad winding dark bands that block the
// starlight and nebula glow behind them (the dark structure threading through the bright band
// in real photos). A RIDGED low-frequency field gives a meandering dark centreline; returned as
// a 0..1 "amount of dust" (0 = clear, 1 = deep in a lane). Sampled in the celestial frame so it
// turns with the stars, and shared by starField + nebula so the lane darkens BOTH coherently.
float mwRift(vec3 cN)
{
	cN = normalize(cN);
	float n  = starFbm3(cN * 1.9 + 211.0, 2);
	float r  = 1.0 - abs(n - 0.5) * 2.0;          // ridge at n≈0.5 → a winding dark centreline
	float lane = smoothstep(0.72, 0.96, r);       // NARROW, distinct winding rift (not broad mottle)
	// a couple of fainter branching threads so the dust isn't one clean line
	float n2 = starFbm3(cN * 3.4 + 67.0, 2);
	lane = max(lane, smoothstep(0.80, 0.99, 1.0 - abs(n2 - 0.5) * 2.0) * 0.75);
	return clamp(lane, 0.0, 1.0);
}
vec3 starField(vec3 dir, vec3 cdir, vec3 sunDir, float time, float milkyWay)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// Stars fade in through twilight (the brightest first), reaching full strength only
	// in a properly dark sky — a touch deeper than before so dusk stays a clean blue hour.
	float night = 1.0 - smoothstep(-0.14, 0.06, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

	// Stars cluster densely along the galactic band: the cell-occupancy threshold
	// is lowered there so the Milky Way reads as a dense lane of stars (not a
	// smear). The milky-way control drives how dense/bright the lane is. Sampled
	// in the rotating celestial frame so the whole field drifts.
	float band   = galacticBand(cdir);
	float mw     = clamp(milkyWay, 0.0, 1.0);
	// Star-amount control sets the BASE threshold for the WHOLE sky, so lowering it thins the
	// field EVENLY across the dome (not just outside the Milky-Way lane). At amount 0 the base
	// threshold goes just ABOVE 1.0 (a cell hash never reaches it) → ZERO stars, and the band
	// subtraction is scaled by amount too, so even the galactic lane is empty at 0. The band
	// then lowers the threshold further once there ARE stars → the lane stays denser. 0.5 ≈
	// the default field.
	float dens   = clamp(uStarDensity, 0.0, 1.0);
	float baseTh = mix(1.001, 0.79, dens);                 // amount 0 → no cell qualifies (no stars)
	// Great-Rift dark dust lanes: only meaningful in/near the galactic band, so gate the fetch.
	float rift   = band > 0.04 ? mwRift(cdir) : 0.0;
	float thresh = baseTh - band * mix(0.07, 0.20, mw) * dens // lane denser (only when amount > 0)
	             + rift * band * 0.22;                        // …but the dust lanes thin it back out
	vec3  p       = cdir * 105.0;                  // denser cells → more, finer stars
	// Screen-space footprint of the cell coordinate (for the per-pixel AA floor).
	// Measured before any branch so the derivative is well-defined.
	float pix     = max(length(fwidth(p)), 1e-4);
	vec3  ip      = floor(p);
	float horizon = smoothstep(0.0, 0.15, dir.y);  // fade into the horizon haze
	float szVar   = clamp(uStarSizeVar, 0.0, 1.0);

	// Splat stars from the 3x3x3 neighbourhood in ABSOLUTE p-space. Measuring distance
	// to the star's true position (not fract(p)) means a star whose disk overflows its
	// own cell is drawn fully instead of being clipped at the cell boundary — that hard
	// clip was the "cut off / pixelated" look. Each cell holds at most one star.
	vec3 acc = vec3(0.0);
	for (int gz = -1; gz <= 1; ++gz)
	for (int gy = -1; gy <= 1; ++gy)
	for (int gx = -1; gx <= 1; ++gx)
	{
		vec3  cell    = ip + vec3(float(gx), float(gy), float(gz));
		float present = starHash(cell);
		if (present < thresh) continue;                       // empty cell

		vec3  sp = cell + vec3(starHash(cell + 1.7), starHash(cell + 4.3), starHash(cell + 8.9));
		float d  = length(p - sp);                            // absolute distance → no clip
		// Per-star size. The variation control scales the spread: at 0 every star is the
		// same mid size; toward 1 a cubic skew makes most stars small with a few large.
		float sizeH = starHash(cell + 5.7);
		float skew  = mix(sizeH, sizeH * sizeH * sizeH, 0.7);
		float sz    = mix(0.45, skew, szVar);                 // 0..~1 size class
		// uStarSize controls the on-screen DIAMETER: it scales the gaussian radius
		// directly, and the screen-space term is demoted to a sub-pixel anti-alias FLOOR
		// (not a hard size). Previously the floor (pix*1.6) sat above the radius across
		// the whole slider, so the slider only nudged the very largest stars.
		float radius = mix(0.16, 0.40, sz) * uStarSize;
		float sigma  = clamp(max(radius, pix * 0.6), 0.0, 0.70);
		float core  = exp(-(d * d) / (sigma * sigma));
		core = core * core;                                   // crisp centre, but wide enough to stay round
		// Small, dim halo (windowed to zero by one cell so a wide glow can't be clipped into
		// a square "glow box" by the 3x3x3 neighbourhood).
		// Glow halo around the star, user-scaled: uStarGlow 0 → pure points, higher → more
		// glow. (Windowed to zero by one cell so a wide glow can't be clipped to a square.)
		float halo  = exp(-(d * d) / (sigma * sigma * 3.5)) * sz * sz * 0.14 * uStarGlow;
		float win   = smoothstep(1.0, 0.6, d);                 // fully gone by the cell boundary
		float shape = (core * 1.8 + halo) * win;              // ×1.8 → centres clip to white (crisp)
		float mag   = (0.4 + 0.6 * smoothstep(thresh, 1.0, present)) * mix(0.8, 2.6, sz);
		// Per-star twinkle: own phase + frequency so the field shimmers in real time. The
		// amount is user-controlled: uStarTwinkle 0 = steady, 0.6 = the classic look,
		// 1 = strong blink. Mean = 1-0.5a, amplitude = 0.5a.
		float twa     = clamp(uStarTwinkle, 0.0, 1.0);
		float twPhase = starHash(cell + 23.5) * 6.2831;
		float twFreq  = 2.0 + 4.0 * starHash(cell + 47.1);
		float tw      = (1.0 - 0.5 * twa) + 0.5 * twa * sin(time * twFreq + twPhase);
		vec3  tint    = mix(vec3(0.80, 0.88, 1.0), vec3(1.0, 0.93, 0.82), starHash(cell + 12.1));
		acc += tint * (shape * mag * tw);
	}
	// The dense band stars sit fainter en masse so the lane reads as many small stars.
	float bandDim = mix(1.6, mix(0.9, 1.5, mw), band);
	// Dust lanes also dim the stars that DO survive in them (blocked background starlight).
	return acc * (horizon * night * bandDim * (1.0 - 0.6 * rift * band));
}

// Procedural volumetric clouds — drawn only in the sky pass (kept out of the
// shared skyColor() so the scene's image-based ambient stays cheap). Density is
// a 3D noise field (reusing starNoise3/starFbm3) animated by the continuous wall
// clock — NOT the looping time-of-day — so clouds drift, form and dissolve with
// their own lifecycle and never snap at the 0h/24h day wrap. A short raymarch
// through a cloud slab with Beer's-law transmittance + a sun light-march gives a
// soft, self-shadowed volumetric look. Mirrors the Metal applyClouds() exactly.
float cloudHash(vec2 p)
{
	p  = fract(p * vec2(127.1, 311.7));
	p += dot(p, p + 34.56);
	return fract(p.x * p.y);
}
float cloudNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = cloudHash(i);
	float b = cloudHash(i + vec2(1.0, 0.0));
	float c = cloudHash(i + vec2(0.0, 1.0));
	float d = cloudHash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float cloudFbm(vec2 p)
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
const float kCloudBase  = 1.0;
const float kCloudTop   = 2.6;
const float kCloudScale = 1.2;    // spatial frequency of the cloud field
// Worley (cellular) lookup from the noise volume's G channel — bright at the cell
// feature points. fBm of it is the billowy cumulus shape. The bake already tiles,
// so a plain trilinear fetch is enough (Worley is C0-smooth).
float worleyNoise3(vec3 p)
{
	return texture(uNoise, p * (1.0 / 256.0)).g;
}
float worleyFbm(vec3 p)
{
	return worleyNoise3(p)        * 0.625
	     + worleyNoise3(p * 2.03) * 0.25
	     + worleyNoise3(p * 4.06) * 0.125;
}
// Henyey-Greenstein phase: forward-biased scattering so the cloud edges facing the
// sun glow (the golden sunset rim / silver lining). g>0 peaks toward the light.
float hgPhase(float cosT, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}
// Cloud drift direction/speed comes from the user wind control (uWind), passed
// down as a parameter so the noise field scrolls the clouds across the sky.
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
// samples step up out of the slab toward the sun).
float cloudDensity(vec3 pos, float time, float coverage, vec3 wind)
{
	float hgrad = cloudHeightGrad(pos.y);
	if (hgrad <= 0.0) return 0.0;                                  // outside slab → no fetches
	vec3  p      = pos * kCloudScale + wind * time;
	float morph  = time * 0.030;                                  // slow forming/dissolving
	float perlin = starFbm3(p + vec3(0.0, morph, 0.0), 4);        // large-scale coverage
	float billow = worleyFbm(p * 0.9 + vec3(morph, 0.0, 0.0));    // fine cauliflower detail
	float base   = perlin * 0.5 + billow * 0.55;
	float lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
	return smoothstep(lo, lo + 0.13, base) * hgrad;
}
// Density for the sun light-march. Slightly fewer octaves than the view density
// (shadows are lower-frequency); the slab-height test bails with zero fetches when
// the sun-ward sample steps out of the slab.
float cloudShadowDensity(vec3 pos, float time, float coverage, vec3 wind)
{
	float hgrad = cloudHeightGrad(pos.y);
	if (hgrad <= 0.0) return 0.0;
	vec3  p      = pos * kCloudScale + wind * time;
	float morph  = time * 0.030;
	float perlin = starFbm3(p + vec3(0.0, morph, 0.0), 3);
	float billow = worleyNoise3(p * 0.9 + vec3(morph, 0.0, 0.0)) * 0.7
	             + worleyNoise3(p * 1.8) * 0.3;
	float base   = perlin * 0.5 + billow * 0.55;
	float lo     = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));
	return smoothstep(lo, lo + 0.13, base) * hgrad;
}
// Procedural volumetric clouds composited over the base sky (returns the blend).
// Marches the view ray through a slab on the sky hemisphere with a 3-step sun
// light-march (Beer's-law self-shadowing). Mirrors the Metal applyClouds().
vec3 applyClouds(vec3 baseSky, vec3 dir, vec3 sunDir, float time, float coverage, vec3 sunColor, vec3 wind, out float outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;          // clear sky → skip the whole raymarch
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // no clouds at/below the horizon

	// Quality (perf knob, uCloudQuality): 0 Low, 1 Med, 2 High. High == original counts.
	int qBaseN  = (uCloudQuality <= 0) ? 8  : (uCloudQuality == 1 ? 12 : 16);
	int qMaxN   = (uCloudQuality <= 0) ? 18 : (uCloudQuality == 1 ? 32 : 64);
	int qShadow = (uCloudQuality <= 0) ? 1  : (uCloudQuality == 1 ? 2  : 3);

	// March the view ray through the cloud slab between base and top heights.
	// A deterministic per-ray offset breaks up otherwise coherent sample planes
	// that show up as visible horizontal cloud layers near grazing view angles.
	float s0 = kCloudBase / max(dir.y, 1e-3);
	float s1 = kCloudTop  / max(dir.y, 1e-3);
	// Near the horizon the slab span (s1-s0) grows ~1/dir.y, so a fixed step count
	// undersamples the noise → the "pixelated"/speckled distant clouds. Scale the
	// step count with 1/dir.y (capped at 64) so the world-space sample spacing stays
	// roughly constant down toward the horizon — that is the actual anti-aliasing.
	int   N  = int(clamp(float(qBaseN) / max(dir.y, 0.12), float(qBaseN), float(qMaxN)));
	float ds = (s1 - s0) / float(N);
	float jitter = cloudHash(dir.xz * 173.3 + vec2(dir.y * 37.1, dir.y * 19.7));

	// Day/night/dusk drive the cloud colour (independent of the drift clock).
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	// Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	float lo = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0)); // coverage threshold (for the cheap gate)
	float T = 1.0;                                 // transmittance along the view ray
	vec3  L = vec3(0.0);                           // accumulated in-scattered colour
	for (int i = 0; i < N; ++i)
	{
		float s   = s0 + (float(i) + jitter) * ds;
		vec3  pos = dir * s;
		float hgrad = cloudHeightGrad(pos.y);
		if (hgrad <= 0.0) continue;
		// Inline cloudDensity() with an EXACT coverage gate: base = perlin*0.5 + billow*0.55
		// and billow <= 1, so (perlin*0.5 + 0.55) upper-bounds it. Where that can't reach the
		// threshold, skip the Worley fetch + the sun light-march. Uses the SAME 4-octave
		// perlin as cloudDensity, so it never culls a real cloud.
		vec3  pp     = pos * kCloudScale + wind * time;
		float morph  = time * 0.030;
		float perlin = starFbm3(pp + vec3(0.0, morph, 0.0), 4);
		if (perlin * 0.5 + 0.55 < lo) continue;
		float billow = worleyFbm(pp * 0.9 + vec3(morph, 0.0, 0.0));
		float dens   = smoothstep(lo, lo + 0.13, perlin * 0.5 + billow * 0.55) * hgrad;
		if (dens > 0.001)
		{
			// Light-march toward the sun: Beer's-law self-shadowing (qShadow steps;
			// scaled by 3/qShadow so fewer steps don't brighten the clouds).
			float shadow = 0.0;
			for (int j = 1; j <= qShadow; ++j)
				shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25), time, coverage, wind);
			float sun    = exp(-shadow * 1.7 * (3.0 / float(qShadow)));
			float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
			float lit    = sun * powder;

			// Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
			vec3 dayCol   = mix(vec3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.26, 0.29, 0.45), lit);
			vec3 cloudCol = mix(nightCol, dayCol, day);
			vec3 duskTop  = sunColor * vec3(1.5, 0.85, 0.42);
			// Even shaded cloud bodies pick up sunset warmth (0.35 floor), lit faces more —
			// so the whole cloud glows golden/orange at dawn & dusk, not just the rim.
			cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
			// Moonlit silver: moon rises on the opposite arc from the sun.
			vec3  cMoonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
			float cMoonUp  = clamp((cMoonDir.y + 0.10) / 0.25, 0.0, 1.0);
			cloudCol += vec3(0.20, 0.22, 0.38) * lit * cMoonUp * (1.0 - day) * 0.25;
			// Forward-scatter glow: Henyey-Greenstein-weighted direct sunlight makes
			// the sun-facing edges flare gold (the silver lining), strongest when
			// looking toward the sun and where the cloud isn't self-shadowed.
			cloudCol += sunColor * mix(vec3(1.0), vec3(1.25, 0.78, 0.42), dusk) * (phase * sun * 0.75 * max(day, dusk));
			// Cheap vertical depth: tops catch the light (bright crown), the base
			// sits in self-shadow (darker, cooler) — fakes the volumetric
			// "cauliflower" relief from just the sample's height in the slab.
			float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
			cloudCol *= mix(0.5, 1.15, hTone);
			cloudCol += vec3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.25);
			cloudCol *= uCloudTint;                          // user colour tint (dome path)

			float opticalDepth = dens * ds * 7.0 * clamp(uCloudDensity, 0.0, 3.0);
			float a = 1.0 - exp(-opticalDepth);
			L += T * a * cloudCol;
			T *= 1.0 - a;
			if (T < 0.02) break;
		}
	}

	// Fade the whole cloud layer out into the horizon haze. Start higher + wider than
	// before so the grazing band (coarsest sampling even with the extra steps) melts
	// into the haze instead of showing residual undersampling speckle.
	float horizon = smoothstep(0.03, 0.22, dir.y);
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// Interleaved-gradient noise — a well-distributed screen-space dither, far better than
// white noise for raymarch ray-start jitter (its energy is high-frequency so the eye
// rejects it, leaving no low-frequency blotches/speckle). Static per pixel (not animated).
float skyIgn(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

// Cloud coverage value-noise fBm with DISTANCE OCTAVE-LOD (procedural mip): the two
// highest-frequency octaves are faded to zero far away (farW: 1 near → 0 far) so the
// coarsely-stepped distant clouds keep only the smooth base shape and stop aliasing into
// speckle. Amplitude-fading (not texture mip) avoids the DC-offset seam — a faded octave
// contributes nothing, rather than collapsing to its 0.5 mean. Matches starFbm3(p,4).
float cloudCoverFbm(vec3 p, float farW)
{
	float v = 0.5 * starNoise3(p);
	p *= 2.03; v += 0.25   * starNoise3(p);
	p *= 2.03; v += 0.125  * starNoise3(p) * farW;
	p *= 2.03; v += 0.0625 * starNoise3(p) * farW * farW;
	return v;
}
// Worley billow fBm with the fine octave distance-faded (same procedural-LOD idea).
float cloudBillowFbm(vec3 p, float farW)
{
	return worleyNoise3(p)        * 0.625
	     + worleyNoise3(p * 2.03) * 0.25
	     + worleyNoise3(p * 4.06) * 0.125 * farW;
}
// Cirrus-local 2D fBm: rotates the domain per octave (≈37°) and detunes the lacunarity
// (1.92, not 2.0) so the sharpened, thresholded cirrus strands don't reveal the noise
// lattice. SEPARATE from the shared cloudFbm (which aurora/contrails depend on).
float cirrusFbm(vec2 p)
{
	float v = 0.0, a = 0.5;
	mat2 rot = mat2(0.80, 0.60, -0.60, 0.80);
	for (int i = 0; i < 5; ++i) { v += a * cloudNoise(p); p = rot * p * 1.92; a *= 0.5; }
	return v;
}

// 3D volumetric clouds (cloud mode 1): a WORLD-ANCHORED slab so the clouds parallax /
// shift as the camera moves through the world. The slab sits `cloudH` world units ABOVE
// the camera (camera-relative altitude → scale-robust at any world size), but the
// density is sampled at absolute WORLD positions, so moving horizontally slides
// different clouds overhead (the parallax). The noise frequency scales with cloudH so
// the angular cloud size stays the same regardless of cloudH. Distant clouds fade into
// the horizon haze. Self-contained (its own raymarch + shading) so the dome path is
// left untouched. Same cloud LOOK/lighting as the dome, just world-projected.
vec3 applyClouds3D(vec3 baseSky, vec3 dir, vec3 camPos, vec3 sunDir, float time,
                   float coverage, vec3 sunColor, vec3 wind, float cloudH, out float outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.02) return baseSky;             // at/below the horizon → ray misses the slab above

	// Quality (perf knob, uCloudQuality): 0 Low, 1 Med, 2 High. High == original counts.
	float qStepF  = (uCloudQuality <= 0) ? 0.40 : (uCloudQuality == 1 ? 0.30 : 0.22);
	float qMinN   = (uCloudQuality <= 0) ? 12.0 : (uCloudQuality == 1 ? 18.0 : 24.0);
	float qMaxN   = (uCloudQuality <= 0) ? 40.0 : (uCloudQuality == 1 ? 72.0 : 128.0);
	int   qShadow = (uCloudQuality <= 0) ? 1    : (uCloudQuality == 1 ? 2    : 3);

	cloudH      = max(cloudH, 1.0);
	float thick = cloudH * 1.5;                   // TALL slab so cumuli can billow upward (3D)
	float baseY = camPos.y + cloudH;
	float tNear = cloudH / dir.y;                 // (baseY - camPos.y)/dir.y, dir.y>0
	float tFar  = (cloudH + thick) / dir.y;
	float maxDist = cloudH * 60.0;                // fade clouds beyond this (∝ altitude)
	tFar = min(tFar, maxDist);
	if (tFar <= tNear) return baseSky;

	// Step count grows with how much slab the ray crosses (much more near the horizon)
	// so the world-space sample spacing stays roughly constant — undersampling near the
	// horizon is what speckles/"pixelates" the distant clouds.
	int   N  = int(clamp((tFar - tNear) / (thick * qStepF), qMinN, qMaxN));
	float ds = (tFar - tNear) / float(N);
	// Interleaved-gradient jitter (blue-noise-like) instead of white noise → the residual
	// undersampling shows as fine filterable dither, not coarse speckle/grain.
	float jitter = skyIgn(gl_FragCoord.xy);

	float sunY  = clamp(sunDir.y, -0.2, 1.0);
	float day   = smoothstep(-0.10, 0.10, sunY);
	float dusk  = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	// FULL inverse compensation so the clouds' apparent SIZE & SHAPE stay EXACTLY the
	// same at any height — the height slider must not alter the clouds themselves, only
	// where the layer sits (the band's elevation, applied via elevFloor below).
	// (1.6/cloudH = 0.008 at the reference height 200, matching the canonical look.)
	float nscale = 1.6 / cloudH;
	// Cloud-band elevation floor: raising the height lifts the band higher in the sky
	// (clear sky opens up toward the horizon); lowering it brings clouds down to the
	// horizon. This is the ONLY thing the height changes about the look — the cloud
	// bodies are identical. Mapped from the slider's ~20..2000 range.
	float elevFloor = clamp((cloudH - 50.0) / 2500.0, 0.0, 0.6);
	// Appearance knobs (global uniforms): fluffiness drives the cauliflower erosion,
	// density scales the opacity/thickness. They tweak the LOOK without moving the
	// sample positions, so they never re-roll the cloud pattern (unlike the height).
	float fluff   = clamp(uCloudFluffiness, 0.0, 1.0);
	float densMul = clamp(uCloudDensity, 0.0, 3.0);
	float lo      = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));

	float T = 1.0;
	vec3  L = vec3(0.0);
	for (int i = 0; i < N; ++i)
	{
		float t   = tNear + (float(i) + jitter) * ds;
		vec3  pos = camPos + dir * t;             // WORLD position → parallax
		float hf  = clamp((pos.y - baseY) / thick, 0.0, 1.0);
		vec3  np  = pos * nscale + wind * time;
		// Distance LOD weight (1 near → 0 far): used to fade fine detail and widen the
		// noise thresholds far away so the coarsely-sampled distant clouds don't speckle.
		float detailFade = 1.0 - smoothstep(maxDist * 0.10, maxDist * 0.40, t);
		// Coverage field (large-scale): WHERE clouds are and HOW HIGH they tower. The
		// presence edge widens with distance (softer = anti-aliased) so the far cloud
		// outlines stop crawling/pixelating under the coarse sampling.
		float cover = cloudCoverFbm(np + vec3(0.0, time * 0.03, 0.0), detailFade); // octave-LOD
		float pres  = smoothstep(lo, lo + mix(0.42, 0.20, detailFade), cover); // 0..1 presence
		if (pres <= 0.0) continue;
		// Towering-cumulus vertical profile: denser columns reach higher; round bottom,
		// billowing eroded top — this is what gives the clouds 3D HEIGHT (not a flat sheet).
		// A rounded (smoothstep²) bottom + a soft dome top read as a swelling puffy body
		// rather than a slab with sharp cut edges.
		float towerTop = mix(0.32, 1.0, smoothstep(lo, lo + 0.30, cover));
		float rise     = smoothstep(0.0, 0.18, hf);
		rise *= rise;                                            // rounder, fuller bottom
		float crown    = 1.0 - smoothstep(towerTop * 0.55, towerTop, hf);
		float vshape   = rise * crown;
		if (vshape <= 0.0) continue;
		// Cauliflower fluff: two Worley billow octaves eroded into the body. Fluffiness
		// adds the finer octave and erodes from a lower threshold over a wider range, so
		// the body breaks into rounded lumps + softer fraying instead of a smooth blob.
		// The erosion now bites at ALL heights (not only the top) so the whole cloud is
		// billowy, strongest up in the crown.
		// Distance LOD: the FINE erosion octave aliases into speckle/"pixelation" once the
		// world-space step (ds) outgrows its small features far away / near the horizon, so
		// fade it out with distance (detailFade, above) — distant clouds keep only the
		// smooth coarse shape, and the erosion threshold widens (softer = AA) there too.
		float billow  = cloudBillowFbm(np * 1.2 + vec3(time * 0.03, 0.0, 0.0), detailFade);
		float billow2 = worleyNoise3(np * 2.8 + vec3(0.0, time * 0.05, 7.0));
		float fineW   = 0.40 * fluff * detailFade;
		float billowM = billow * (1.0 - fineW) + billow2 * fineW;
		float erLo    = mix(0.30, 0.14, fluff);                 // fluffier → erode from lower
		float erBite  = mix(0.30, 0.62, fluff);                 // fluffier → wider erosion range
		erBite        = mix(0.80, erBite, detailFade);          // far → wider/softer (anti-alias)
		float erode   = mix(1.0, smoothstep(erLo, erLo + erBite, billowM),
		                    mix(0.45, 1.0, hf) * (0.55 + 0.45 * fluff));
		float dens    = pres * vshape * erode;
		if (dens > 0.001)
		{
			// Sun light-march (Beer's law) toward the sun through the slab.
			float shadow = 0.0;
			for (int j = 1; j <= qShadow; ++j)
			{
				vec3  sp  = pos + sunDir * (float(j) * thick * 0.22);
				float shf = clamp((sp.y - baseY) / thick, 0.0, 1.0);
				float shg = smoothstep(0.0, 0.25, shf) * (1.0 - smoothstep(0.6, 1.0, shf));
				if (shg <= 0.0) continue;
				vec3  snp = sp * nscale + wind * time;
				float p2  = starFbm3(snp + vec3(0.0, time * 0.03, 0.0), 3);
				float b2  = worleyNoise3(snp * 0.9 + vec3(time * 0.03, 0.0, 0.0)) * 0.7
				          + worleyNoise3(snp * 1.8) * 0.3;
				shadow += smoothstep(lo, lo + 0.13, p2 * 0.5 + b2 * 0.55) * shg;
			}
			float sun    = exp(-shadow * 1.7 * (3.0 / float(qShadow)));
			float powder = 1.0 - exp(-dens * mix(3.0, 4.5, fluff)); // softer fraying edges when fluffy
			float lit    = sun * powder;
			vec3 dayCol   = mix(vec3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.26, 0.29, 0.45), lit);
			vec3 cloudCol = mix(nightCol, dayCol, day);
			vec3 duskTop  = sunColor * vec3(1.5, 0.85, 0.42);
			// Even shaded cloud bodies pick up sunset warmth (0.35 floor), lit faces more —
			// so the whole cloud glows golden/orange at dawn & dusk, not just the rim.
			cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
			cloudCol += sunColor * mix(vec3(1.0), vec3(1.25, 0.78, 0.42), dusk) * (phase * sun * 0.75 * max(day, dusk));
			cloudCol *= mix(0.30, 1.32, hf);                      // strong base→crown contrast (3D relief)
			cloudCol += vec3(0.07, 0.10, 0.17) * ((1.0 - hf) * day * 0.25);
			cloudCol *= uCloudTint;                               // user colour tint
			// Aerial perspective: bleed far clouds toward the sky colour so they lose
			// CONTRAST (not just opacity) with distance — low contrast hides any residual
			// horizon speckle and reads as natural haze.
			float hazeFar = smoothstep(maxDist * 0.35, maxDist, t);
			cloudCol = mix(cloudCol, baseSky, hazeFar * 0.6);

			// Fade far clouds into the horizon haze, and normalise the optical depth by
			// the slab thickness (ds is in world units, unlike the dome's unit slab).
			// The density knob scales opacity here (not coverage) so it thickens the
			// existing clouds rather than re-rolling where they are.
			float distFade     = 1.0 - smoothstep(maxDist * 0.5, maxDist, t);
			float opticalDepth = dens * (ds / thick) * 7.0 * distFade * densMul;
			float a = 1.0 - exp(-opticalDepth);
			L += T * a * cloudCol;
			T *= 1.0 - a;
			if (T < 0.02) break;
		}
	}
	// Soft lower edge of the cloud band. Its elevation is driven by the height slider
	// (elevFloor): higher layer → the band starts higher up and clear sky opens toward
	// the horizon; lower → clouds reach down to the horizon. The clouds above the edge
	// are unchanged (same size/shape) — only WHERE the band begins moves.
	float horizon = smoothstep(elevFloor, elevFloor + 0.14, dir.y);
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// Space nebula — drifting coloured emission clouds gathered toward the galactic
// band. Sampled as 3D blobs on the celestial sphere (rotates with the stars):
// isolated rounded patches of varying size with bright cores, dark dust lanes,
// and a blue->magenta->teal hue wheel so neighbouring blobs differ in colour and
// bleed into one another. Night/horizon gated, occluded by clouds. Mirrors Metal.
vec3 nebula(vec3 dir, vec3 cdir, vec3 sunDir, float intensity, vec3 nebColor)
{
	if (intensity <= 0.0) return vec3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// DEEP-night gate: real Milky-Way nebulosity is only visible once the sun is well
	// below the horizon (astronomical twilight) — NOT at dusk, where the sky is still a
	// bright blue hour. Gating it deeper than the stars stops the "teal flood at sunset".
	float night = 1.0 - smoothstep(-0.22, -0.04, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

	vec3  cN   = normalize(cdir);
	const vec3 galN = normalize(vec3(0.46, 0.52, -0.72));
	float bd   = dot(cN, galN);
	float band = exp(-bd * bd * 4.5);           // TIGHT milky-way lane (not a full-sky glow)
	vec3  P    = cN * 3.4;
	// SEED: shift the sample window into the noise field so the cloud SHAPES (and the colour
	// layout below) re-randomise. The band stays put — it comes from cN — only the gas moves.
	P += vec3(uNebulaSeed * 13.1, uNebulaSeed * 7.7, uNebulaSeed * 19.3);
	bool  hifi = uNebulaHiFi >= 0.5;   // 1 High, 2 Max → detailed filament branch
	bool  maxq = uNebulaHiFi >= 1.5;   // 2 Max → extra crisping + faded fine octaves
	float density, core;
	if (hifi)
	{
		// ===== HIGH FIDELITY: ridged-multifractal filament nebula (astrophoto detail) =====
		// Flowing 2-level domain warp (swirling gas) + offset-weighted ridged MULTIFRACTAL
		// (Musgrave) so fine detail RIDES the filament crests; two crossing ridge fields → a
		// Crab-like web; low-freq bodies cluster the gas into discrete regions; ridged dust
		// lanes carve dark absorption channels. NB starFbm3(p,1) is value noise in [0,0.5], so
		// the ridge fold uses 4·n−1 to span ±1 (a bare 2·n−1 would barely fire). ~37 fetches.
		float hfSeed = uNebulaSeed;
		// (1) Flowing domain warp: swirling / sheared gas (IQ 2-level advection)
		vec3 hfw1 = P * 0.55 + hfSeed * 0.31;
		vec3 hfQ1 = vec3(starFbm3(hfw1 + vec3( 0.0,  0.0,  0.0), 2),
		                 starFbm3(hfw1 + vec3(19.3,  7.1,  3.7), 2),
		                 starFbm3(hfw1 + vec3( 5.2,  1.9, 11.4), 2)) - 0.5;
		vec3 hfw2 = P * 1.10 + 3.1 * hfQ1 + hfSeed * 1.7 + 41.0;
		vec3 hfQ2 = vec3(starFbm3(hfw2 + vec3( 0.0,  0.0,  0.0), 2),
		                 starFbm3(hfw2 + vec3(27.6, 13.2,  8.8), 2),
		                 starFbm3(hfw2 + vec3( 3.3, 21.7,  5.1), 2)) - 0.5;
		vec3 Pw = P + 0.90 * hfQ1 + 0.42 * hfQ2;   // advected → flowing tendrils
		vec3 Pc = P + 0.30 * hfQ1;                  // steadier coord for cloud bodies
		// Max-only anti-alias weight for the extra fine octaves: fade them toward 0 where
		// the finest sample's screen footprint nears pixel-Nyquist, so they add detail when
		// the nebula fills the view but never shimmer on camera rotation.
		float aaFine = maxq ? (1.0 - smoothstep(0.30, 0.70, length(fwidth(Pw)) * 520.0)) : 0.0;
		// (2) Ridged MULTIFRACTAL #1: fine filament network (each octave gated by the prev ridge)
		vec3  rp = Pw * 1.45 + hfSeed * 0.37;
		float rsum = 0.0, ramp = 1.0, rw = 1.0, rs, rn;
		const float RLAC = 1.93, ROFF = 1.0, RGAIN = 2.10, RSW = 0.60;
		rn = starFbm3(rp, 1); rs = ROFF - abs(4.0*rn - 1.0); rs *= rs;            rsum += ramp*rs; ramp *= RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs; ramp*=RSW;
		if (maxq)   // Max: two extra fine octaves, fwidth-faded so they never alias
		{
			rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs*aaFine; ramp*=RSW;
			rp *= RLAC; rw = clamp(rs*RGAIN,0.0,1.0); rn = starFbm3(rp,1); rs = ROFF-abs(4.0*rn-1.0); rs*=rs; rs*=rw; rsum += ramp*rs*aaFine; ramp*=RSW;
		}
		// (3) Ridged MULTIFRACTAL #2: broad crossing tendrils (lower freq)
		vec3  rp2 = Pw * 0.78 + hfSeed * 0.19 + 113.0;
		float r2sum = 0.0, r2amp = 1.0, r2w = 1.0, r2s, r2n;
		const float R2LAC = 2.02, R2OFF = 1.0, R2GAIN = 2.20, R2SW = 0.62;
		r2n = starFbm3(rp2,1); r2s = R2OFF-abs(4.0*r2n-1.0); r2s*=r2s;                 r2sum += r2amp*r2s; r2amp*=R2SW;
		rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s; r2amp*=R2SW;
		rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s; r2amp*=R2SW;
		rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s; r2amp*=R2SW;
		rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s; r2amp*=R2SW;
		rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s; r2amp*=R2SW;
		if (maxq)   // Max: two extra fine octaves, fwidth-faded
		{
			rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s*aaFine; r2amp*=R2SW;
			rp2*=R2LAC; r2w=clamp(r2s*R2GAIN,0.0,1.0); r2n=starFbm3(rp2,1); r2s=R2OFF-abs(4.0*r2n-1.0); r2s*=r2s; r2s*=r2w; r2sum+=r2amp*r2s*aaFine; r2amp*=R2SW;
		}
		float hfFil = max(rsum, r2sum);                 // union → crossing filament web
		float filN  = clamp(hfFil / 1.45, 0.0, 1.0);
		float lines = pow(smoothstep(maxq ? 0.33 : 0.30, maxq ? 0.55 : 0.58, filN), maxq ? 3.5 : 3.2);  // crisper thin filaments at Max
		float wisp  = smoothstep(0.05, 0.40, filN);            // faint diffuse halo around them
		// Anisotropic STREAKS: ridged noise sampled with a stretched axis → elongated filament
		// lines that the domain warp bends into curves — reads more line-like than round ridges.
		vec3  sp = vec3(Pw.x, Pw.y * 4.5, Pw.z) * 1.3 + 500.0 + hfSeed;
		float streak = 1.0 - abs(4.0 * starFbm3(sp, 2) - 1.0);
		streak = pow(smoothstep(0.45, 0.82, streak), 2.6);
		lines = max(lines, streak * 0.9);                      // merge into the filament line field
		// (4) FINE VOID DETAIL: a high-freq ridged layer that fills the dark GAPS with faint thin
		// structure (so the voids aren't smooth/empty) → finer lines + more depth.
		vec3  vp = Pw * 3.3 + hfSeed * 0.7 + 250.0;
		float vd = 0.0, va = 0.55, vn, vs;
		vn=starFbm3(vp,1); vs=1.0-abs(4.0*vn-1.0); vs*=vs; vd+=va*vs; vp*=2.07; va*=0.55;
		vn=starFbm3(vp,1); vs=1.0-abs(4.0*vn-1.0); vs*=vs; vd+=va*vs; vp*=2.07; va*=0.55;
		vn=starFbm3(vp,1); vs=1.0-abs(4.0*vn-1.0); vs*=vs; vd+=va*vs; vp*=2.07; va*=0.55;
		vn=starFbm3(vp,1); vs=1.0-abs(4.0*vn-1.0); vs*=vs; vd+=va*vs;
		float voidDetail = pow(clamp(vd*1.25, 0.0, 1.0), 1.7);
		// (5) Fine GRAIN → breaks up smooth gas so it reads matte/dusty, not shiny.
		float grain = clamp(starFbm3(Pw*6.5 + 400.0, 2) * 2.0, 0.0, 1.4);
		// (6) Cloud bodies / regions + dense centres (low-freq billows)
		float hfBodyLo = starFbm3(Pc*0.46 + hfSeed*0.60 + 60.0, 4);
		float hfBodyMi = starFbm3(Pc*1.12 + 88.0, 3);
		float hfCloud  = smoothstep(0.32, 0.76, hfBodyLo*0.72 + hfBodyMi*0.42);
		float hfCoreM  = smoothstep(0.60, 0.93, hfBodyLo*0.70 + hfBodyMi*0.50);
		// (7) LAYERED dust at two scales (broad + fine) → overlapping dark structure = DEPTH.
		float dBroad = 1.0 - abs(4.0*starFbm3(Pw*1.05 + 130.0 + hfSeed*0.9, 2) - 1.0);
		float dFine  = 1.0 - abs(4.0*starFbm3(Pw*2.9  + 311.0 + hfSeed*0.5, 1) - 1.0);
		float hfDust = (1.0 - 0.62*smoothstep(0.52,0.90,dBroad)) * (1.0 - 0.42*smoothstep(0.60,0.95,dFine));
		// (8) Compose — filament-DOMINANT crisp lines + faint halo + fine detail in the gaps,
		// textured by grain (matte), then layered dust absorption for depth. Matte cores (no bloom).
		float reach = hfCloud*0.85 + 0.15;
		float gas = lines * (maxq ? 1.5 : 1.35) * reach // BRIGHT crisp filaments (dominant; bolder at Max)
		          + wisp  * 0.30 * reach
		          + voidDetail * (maxq ? 0.60 : 0.42) * (1.0 - hfCloud*0.35) // more fine gap detail at Max
		          + hfCloud * 0.13;
		gas *= mix(1.0, grain, 0.70);                   // strong matte texture (anti-shiny)
		gas *= hfDust;                                  // layered absorption → depth
		density = clamp(gas, 0.0, 1.2);
		core    = clamp(hfCoreM * (0.26 + 0.5*lines) + 0.18*hfCoreM*filN, 0.0, 0.65); // dim matte cores
	}
	else
	{
		// ===== HIGH PERFORMANCE: ridged filaments (the FORMER high-fidelity — it cost almost the
		// same as plain clouds, so it's the baseline-quality path now). 2-level warp + two ridged
		// samples + fine grain + dust lanes. =====
		vec3  w1 = vec3(starFbm3(P * 0.6 + 17.0, 3), starFbm3(P * 0.6 + 53.0, 3),
		                starFbm3(P * 0.6 + 91.0, 3)) - 0.5;
		vec3  Pp = P + w1 * 2.0;
		vec3  w2 = vec3(starFbm3(Pp * 1.9 + 211.0, 2), starFbm3(Pp * 1.9 + 167.0, 2),
		                starFbm3(Pp * 1.9 + 123.0, 2)) - 0.5;
		Pp += w2 * 0.7;
		float big    = starFbm3(Pp * 0.7 + 11.0, 4);
		float med    = starFbm3(Pp * 1.6 + 27.0, 3);
		float baseN  = big * 0.55 + med * 0.55;
		float blob   = smoothstep(0.30, 0.62, baseN);
		float ridge1 = 1.0 - abs(2.0 * starFbm3(Pp * 2.4 + 97.0,  3) - 1.0);
		float ridge2 = 1.0 - abs(2.0 * starFbm3(Pp * 5.2 + 131.0, 2) - 1.0);
		float fil    = pow(ridge1, 2.2) * 0.85 + pow(ridge2, 3.0) * 0.55;
		float fine   = clamp(0.62 + 0.7 * (starFbm3(Pp * 8.5 + 59.0, 2) - 0.35), 0.2, 1.4);
		float dust   = 1.0 - 0.55 * smoothstep(0.50, 0.85, starFbm3(Pp * 2.6 + 63.0, 2));
		density = blob * (0.30 + 0.95 * fil) * fine * dust;
		core    = smoothstep(0.54, 0.88, baseN);
	}
	float glow   = (band * 1.05 + 0.05) * (density + 0.7 * core);         // more concentrated in the band
	glow *= 1.0 - 0.90 * mwRift(cN);                                      // dark dust lanes cut through
	glow = max(glow, 0.0);
	if (glow <= 0.0) return vec3(0.0);

	// Per-region hue field → blend the THREE user-adjustable colours so neighbouring regions
	// differ clearly in colour. Seeded so the colour layout also re-randomises.
	float h = clamp(starFbm3(P * 0.5 + 71.0 + uNebulaSeed * 5.0, maxq ? 4 : (hifi ? 3 : 2)) * 1.7 - 0.35, 0.0, 1.0);
	vec3 col = mix(nebColor, uNebulaColor2, smoothstep(0.05, 0.50, h));   // colour 1 → 2
	col = mix(col, uNebulaColor3, smoothstep(0.50, 0.95, h));             // → colour 3
	// Light desaturation only (the user wants VISIBLE colour contrast, not a whitish glow).
	float lum = dot(col, vec3(0.3, 0.59, 0.11));
	col = mix(col, vec3(lum), 0.18);
	col = mix(col, col + vec3(0.55), core * 0.35);                        // dense cores brighten toward white
	float horizon = smoothstep(0.0, 0.16, dir.y);
	float neb = glow * 2.05 * intensity;
	neb = neb / (1.0 + neb * 0.22);   // gentle highlight rolloff → dense cores keep colour, don't clip flat-white
	return col * (neb * horizon * night);
}

// Meander offset of an aurora band as a function of world X (the band RUNS along world-X;
// this returns its slowly-varying Z offset so the band gently snakes/arcs as it crosses the
// sky). PURE SINES (no fbm) — cheap, since this is called per-band per-march-step. Three
// detuned sines give a quasi-organic, non-repeating snake; the time terms waft it sideways.
float auroraMeander(float x, float k, float t)
{
	return  370.0 * sin(x * 0.00090 + 0.20 * t + k * 1.7)
	     +  170.0 * sin(x * 0.00300 - 0.14 * t + k * 3.3)
	     +   85.0 * sin(x * 0.00760 + 0.28 * t + k * 5.1);
}

// Aurora borealis — WORLD-ANCHORED volumetric curtains, built like the 3D cloud slab so the
// bands are PLACED IN THE WORLD (real parallax as the camera moves) and have NO azimuth seam
// (sampled at world XZ, not atan-azimuth). A slab raymarch accumulates thin Gaussian SHEETS
// standing along meandering world-XZ centrelines (auroraCx) — true vertical curtains at any
// view angle, converging into a corona overhead. Sharp bright lower edge fading up; base→top
// colour gradient by altitude. PURELY EMISSIVE: additive, no Beer's law, no light-march. The
// soft rolloff keeps overlapping sheets from clipping to white (FragColor is LDR). Returns the
// emission to ADD to the sky BEFORE the clouds (so the nearer cloud layer occludes it).
vec3 applyAurora3D(vec3 dir, vec3 camPos, float time, float intensity, vec3 colBase, vec3 colTop)
{
	if (intensity <= 0.0) return vec3(0.0);
	dir = normalize(dir);
	if (dir.y < 0.02) return vec3(0.0);                         // horizon → ray never reaches the band
	float night = 1.0 - smoothstep(-0.20, -0.02, clamp(normalize(uSunDir).y, -0.3, 1.0));
	if (night <= 0.0) return vec3(0.0);

	// World-space altitude slab. Height control drives the band ELEVATION (kept high so the
	// parallax stays subtle — aurora is near-infinite; lower = more exaggerated parallax).
	float altitude = mix(1500.0, 7000.0, clamp(uAuroraHeight, 0.0, 1.0));
	float baseY = camPos.y + altitude;
	float thick = altitude * 1.4;
	float invY  = 1.0 / dir.y;
	float tNear = max((baseY - camPos.y) * invY, 0.0);
	float tFar  = (baseY + thick - camPos.y) * invY;
	float maxDist = altitude * 80.0;
	tFar = min(tFar, maxDist);
	if (tFar <= tNear) return vec3(0.0);

	float frag = clamp(uAuroraFragment, 0.0, 1.0);
	// Step count — coarser than the cloud march (sheets are anti-aliased by the σ-widening +
	// IGN jitter, so fewer steps suffice). Capped low for performance (night-only raymarch).
	int   N  = int(clamp((tFar - tNear) / 72.0, 20.0, 46.0));
	float ds = (tFar - tNear) / float(N);
	float jit = skyIgn(gl_FragCoord.xy);

	vec3 acc = vec3(0.0);
	for (int i = 0; i < N; ++i)
	{
		float t   = tNear + (float(i) + jit) * ds;
		vec3  pos = camPos + dir * t;                          // WORLD position
		vec2  pw  = pos.xz;                                    // ABSOLUTE world XZ → real parallax
		                                                       // (position-dependent, exactly like the cloud slab)
		float hf  = clamp((pos.y - baseY) / thick, 0.0, 1.0);  // 0 base … 1 top
		// Vertical emission: sharp bright lower edge, long exponential fade up (fall-streaks).
		float Evert = smoothstep(0.0, 0.05, hf) * exp(-hf * 2.4);
		if (Evert <= 0.0015) continue;
		float distLOD  = smoothstep(maxDist * 0.05, maxDist * 0.5, t); // 0 near → 1 far (widen σ far → AA)
		float distFade = 1.0 - smoothstep(maxDist * 0.45, maxDist, t);
		vec3  cCol = mix(colBase, colTop, smoothstep(0.0, 0.55, hf));  // green base → purple top
		vec3  emitCol = vec3(0.0);
		// Bands run along world-X and are placed on BOTH sides of the camera (alternating ±Z)
		// at GEOMETRICALLY growing, IRREGULARLY-spaced distances → they cover the whole sky
		// dome (near-zenith for the close ones, down to the horizon for the far ones, in front
		// AND behind), with asymmetric gaps. Absolute world XZ → parallax. The early-out keeps
		// it cheap (only the 1-2 bands near this sample's Z run the sine meander).
		for (int k = 0; k < 14; ++k)
		{
			float fk   = float(k);
			float side = mod(fk, 2.0) < 0.5 ? 1.0 : -1.0;
			float rank = floor(fk * 0.5);
			float z0   = side * (380.0 + rank * rank * 430.0 + 480.0 * sin(fk * 2.3 + 1.0)); // irregular
			if (abs(pw.y - z0) > 1400.0) continue;            // can't reach this sample → skip BEFORE meander
			float bandZ = z0 + auroraMeander(pw.x, fk, time);  // band centre Z (snakes/winds in X)
			float d = pw.y - bandZ;                            // signed distance to the band (in Z)
			// Organic WIDTH variation → billowy, non-uniform bands (not a clean uniform bar).
			float widthVar = 0.55 + 0.80 * cloudNoise(vec2(pw.x * 0.0011 + fk * 7.0, fk * 1.3 + time * 0.04));
			float sigma = mix(55.0, 175.0, distLOD) * widthVar; // thin near, widened far, varied
			float sheet = exp(-(d * d) / (2.0 * sigma * sigma));
			if (sheet < 0.004) continue;
			// Fine vertical RAY texture inside the curtain + irregular brightness PATCHES so the
			// band has natural varied form, not a smooth bar. Cheap 1-octave noise (not fbm).
			float rays   = 0.68 + 0.32 * cloudNoise(vec2(pw.x * 0.016 + fk, hf * 3.5 + fk * 5.0));
			float patchN = 0.58 + 0.42 * cloudNoise(vec2(pw.x * 0.0040 - fk * 3.0, fk + 9.0 + time * 0.05));
			// Fragmentation: extra gaps along the band when the user raises the slider.
			float g = sin(pw.x * 0.00075 + fk * 4.0 + time * 0.1)
			        * sin(pw.x * 0.00210 - fk * 2.0);
			float broken = mix(1.0, smoothstep(-0.15, 0.55, g), frag);
			emitCol += cCol * (sheet * rays * patchN * broken);
		}
		acc += emitCol * Evert * (ds / thick) * distFade;      // pure ADD (emissive, no extinction)
	}
	acc = acc / (1.0 + acc * 0.6);                             // soft rolloff → no clip-to-white (LDR)
	float horizonFade = clamp(dir.y * 8.0, 0.0, 1.0);          // mask the 1/dir.y blow-up
	return acc * (intensity * night * horizonFade * 3.5);
}

// Contrails (Kondensstreifen) — vapour-trail lines that fill an empty daytime sky.
// Each trail is a SHORT FINITE SEGMENT at a random position with a random heading (full
// circle), so they scatter in all directions instead of sweeping side-to-side. Modelled
// on real persistent contrails (NWS/Wikipedia): thin & sharp at the fresh tip, broadening
// into a fuzzy, eroded, FADING band toward the old tip as it dissipates. They are
// TRANSLUCENT — composited as an alpha blend (sky shows through) toward a near-sky white
// rather than added as opaque paint, with soft feathered/noise-eroded edges, so they melt
// into the sky instead of looking pasted on. Takes the base sky in, returns it blended.
vec3 contrails(vec3 baseSky, vec3 dir, vec3 sunDir, float amount, float coverage)
{
	if (amount <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.05) return baseSky;
	float day = smoothstep(-0.04, 0.16, clamp(sunDir.y, -0.2, 1.0));
	if (day <= 0.0) return baseSky;

	// Sky-plane projection (same mapping as the aurora): straight trails stay straight.
	vec2 P = dir.xz / (dir.y + 0.22);

	float aAcc = 0.0;                                     // accumulated coverage (alpha)
	for (int i = 0; i < 9; ++i)
	{
		float fi = float(i);
		float a0 = starHash(vec3(fi, 11.0,  3.0));        // heading
		float a1 = starHash(vec3(fi,  5.0, 19.0));        // centre x
		float a2 = starHash(vec3(fi, 23.0,  7.0));        // centre y
		float a3 = starHash(vec3(fi,  2.0, 31.0));        // length
		float ang = a0 * 6.2831853;                       // FULL circle → scattered headings
		vec2  d2  = vec2(cos(ang), sin(ang));
		vec2  c   = (vec2(a1, a2) - 0.5) * 7.5;           // scattered centre on the sky plane
		float L   = 1.0 + 2.4 * a3;                       // half-length varies per trail
		vec2  rel = P - c;
		float t   = clamp(dot(rel, d2), -L, L);           // nearest point ALONG the segment
		float perp = length(rel - d2 * t);                // perpendicular distance to it
		float u    = smoothstep(-L, L, t);                // 1 at fresh tip, 0 at old (dissipating) tip
		// Old end is wide + fuzzy, fresh end thin + sharp; soft Gaussian cross-section.
		float width = mix(0.075, 0.013, u);
		float x     = perp / width;
		float prof  = exp(-x * x * 1.6);                  // feathered, no hard edge
		// Erode/break up along the trail so it isn't a clean ruler line (dissipating puffs).
		float fuzz  = 0.5 + 0.5 * cloudFbm(vec2(t * 3.5 + fi * 9.0, u * 6.0 + perp * 4.0));
		// Opacity along: fresh end fuller, old end faint (evaporating); soft tips both ends.
		float along = mix(0.14, 0.95, u);
		float tip   = 1.0 - smoothstep(L * 0.6, L, abs(t));
		float seg   = clamp(prof * fuzz * along * tip, 0.0, 1.0);
		aAcc += seg * (1.0 - aAcc);                       // over-composite overlapping trails
	}

	float fade  = smoothstep(0.05, 0.30, dir.y) * (1.0 - smoothstep(0.85, 1.0, dir.y));
	float clear = 1.0 - smoothstep(0.25, 0.65, coverage); // real clouds take over
	// Translucent: cap well below 1 so the sky always shows through (the natural look).
	float alpha = clamp(aAcc * amount * day * fade * clear, 0.0, 0.72);
	// Ice-cloud white, slightly brighter toward the sun; alpha-blend so thin parts read
	// as faint sky-haze and only the dense fresh line approaches white.
	float toSun = max(dot(dir, sunDir), 0.0);
	vec3  white = mix(vec3(0.86, 0.89, 0.94), vec3(1.0, 0.99, 0.96), toSun * toSun);
	return mix(baseSky, white, alpha);
}

// Thin high 2D cirrus clouds — modelled on real cirrus (NOAA/Wikipedia + photo reference):
// fibrous, hair-like "mare's-tail" streaks all aligned to the high-altitude wind, curving
// into hooked filaments (uncinus), translucent so the blue sky shows through, soft-edged,
// flat and high (no puffiness/shadow). Built from a strongly ANISOTROPIC ridged field
// (the fibres) on the flat sky-plane projection, bent by a cross-wind domain warp (the
// hooks), with fine across-strand striations — all on a SEPARATE cirrusFbm so the strands
// don't reveal the noise lattice. Translucent alpha-composite; white at noon, gold/pink at
// low sun, with a subtle forward-scatter sheen toward the sun. Drifts slowly. Daytime.
vec3 cirrus(vec3 baseSky, vec3 dir, vec3 sunDir, vec3 sunColor, float amount, float seed, float time, vec2 windXZ)
{
	if (amount <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	if (dir.y < 0.04) return baseSky;
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.06, 0.14, sunY);
	if (day <= 0.0) return baseSky;
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	// Flat high sheet: project onto a horizontal plane (dir.xz/dir.y) so the streaks
	// foreshorten and converge toward the horizon for free. Seed offsets; slow drift.
	vec2  so = vec2(seed * 13.1, seed * 7.3);
	vec2  P  = dir.xz / (dir.y + 0.12) + windXZ * time * 0.5 + so;

	// Strong anisotropy → long fibres along x. Then a mostly-cross-wind domain warp bends
	// the straight strands into wavy/hooked mare's-tails (stretch FIRST, then warp small).
	vec2  q  = P * vec2(0.30, 3.0);
	q += vec2(0.12, 0.95) * (cirrusFbm(q * 0.5 + so) - 0.5);

	// Ridged basis → fibres (not rounded blobs); softened so creases aren't razor edges.
	float baseN = cirrusFbm(q * 1.15);
	float ridge = 1.0 - abs(2.0 * baseN - 1.0);
	ridge = pow(clamp(ridge, 0.0, 1.0), 1.6);

	// Fine across-strand striations (HIGH freq across the fibre, LOW along it) — this is
	// what splits a streak into many parallel hairs. Faded near the horizon (anti-alias).
	float fineW   = smoothstep(0.06, 0.26, dir.y);
	float fibers  = cirrusFbm(P * vec2(0.8, 7.0) + so);
	fibers = mix(0.5, smoothstep(0.32, 0.82, fibers), fineW);

	// Sparse thin coverage: only the ridge peaks survive as wisps, lots of open sky.
	float thr    = mix(0.60, 0.40, clamp(amount, 0.0, 1.0));
	float mask   = smoothstep(thr, thr + 0.22, ridge);
	float streak = mask * (0.28 + 0.72 * fibers);
	streak *= 0.6 + 0.4 * cirrusFbm(q * 0.7 + so + 11.0);   // slow large-scale breakup

	float fade  = smoothstep(0.04, 0.20, dir.y) * (1.0 - smoothstep(0.92, 1.0, dir.y));
	// Translucent (capped so the sky always shows through): thin wisps read as faint haze.
	float alpha = clamp(streak * day * fade * (0.40 + 0.65 * clamp(amount, 0.0, 1.0)), 0.0, 0.66);

	// Colour: white near noon → gold/pink at low sun (gated to dusk so midday stays white);
	// a subtle forward-scatter sheen brightens the wisps toward the sun (the silky look);
	// then a touch of the local sky so they aren't pasted-white at dawn/dusk.
	vec3  white = mix(vec3(0.92, 0.95, 1.0), sunColor * vec3(1.35, 1.0, 0.78), dusk * 0.7);
	float fwd   = pow(max(dot(dir, sunDir), 0.0), 12.0);
	white += sunColor * (fwd * 0.45 * max(day, dusk));
	white = mix(white, baseSky * 1.2 + 0.08, 0.15);
	return mix(baseSky, white, alpha);
}

// The bright sun BODY (crisp disk + tight bloom) factored out of skyColor() so the
// cloud pass can occlude it. main() subtracts this, runs the clouds, then re-adds it
// weighted by pow(cloudTransmittance, k): an opaque cloud (T~0.1) then fully hides the
// sun instead of leaking a ~14x ghost through a plain *T. The expressions below MUST
// stay byte-identical to the matching disk+bloom lines in kSkyFuncGLSL skyColor() so
// that (col -= sunGlare) cancels exactly and a clear sky is unchanged.
vec3 sunGlare(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.14, 0.04, sunY) * (1.0 - smoothstep(0.04, 0.26, sunY));
	vec3  sunTint = mix(vec3(1.0, 0.58, 0.24), vec3(1.0, 0.96, 0.88), smoothstep(0.0, 0.28, sunY));
	float s         = max(dot(dir, sunDir), 0.0);
	float sunVis    = max(day, dusk);
	float bloomDamp = mix(1.0, 0.28, dusk);
	// The crisp daytime disk is now a geometric body (sunDisk, below) — only the
	// cloud-occludable tight bloom remains here, so the col -= sunGlare / re-add dance
	// still cancels byte-for-byte against skyColor()'s matching bloom line.
	vec3  g  = sunTint * (pow(s, 220.0)  * 1.1 * bloomDamp) * sunVis; // tight bloom
	return g;
}

// Geometric sun disk — a real limb-darkened body (like moonDisk) replacing the old
// pow(dot(dir,sunDir)) glare lobe. Eddington limb darkening dims the edge; atmospheric
// refraction flattens it into a wider-than-tall, reddened ellipse near the horizon (a
// proper setting sun). Emissive; sky-pass only (kept out of skyColor/IBL, like the
// moon) and composited after the clouds in main(), weighted by cloud transmittance.
vec3 sunDisk(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	// Visible from noon down to just below the horizon (the setting sun), then gone.
	float vis = smoothstep(-0.06, 0.02, sunY);
	if (vis <= 0.0 || dot(dir, sunDir) <= 0.0) return vec3(0.0);
	// Tangent frame: right = horizontal, upv = vertical, so the disk can squash vertically.
	vec3  right = normalize(cross(vec3(0.0, 1.0, 0.0), sunDir));
	vec3  upv   = cross(sunDir, right);
	const float kRadius = 0.027;                                   // angular radius (~ the moon)
	// Refraction flattening: near the horizon the lower limb lifts → wider-than-tall.
	float squash = mix(0.62, 1.0, smoothstep(0.0, 0.14, sunY));    // <1 ⇒ vertically compressed
	float qx = dot(dir, right) / kRadius;
	float qy = dot(dir, upv)   / (kRadius * squash);
	float r  = length(vec2(qx, qy));
	if (r > 1.0) return vec3(0.0);
	// Eddington limb darkening: I(mu) = 1 - u(1 - mu), mu = cos(angle) = sqrt(1 - r^2), u = 0.6.
	float mu   = sqrt(max(1.0 - r * r, 0.0));
	float limb = 1.0 - 0.6 * (1.0 - mu);                           // centre 1.0 → limb 0.4
	float edge = smoothstep(1.0, 0.96, r);                         // soft anti-aliased rim
	// Reddens toward the horizon (more atmosphere), warm-white when high. The low-sun
	// disk is kept DIM on purpose: a hard limb-darkened disk integrates far more energy
	// than the old falloff lobe, so at full brightness ACES desaturates the core to flat
	// white and the reddening/limb shading never reads (the setting sun must stay dim
	// enough to read as a red-orange ellipse — same lesson as the moon's ×3.0→×1.3 fix).
	vec3  tint   = mix(vec3(1.0, 0.38, 0.14), vec3(1.0, 0.95, 0.88), smoothstep(0.0, 0.22, sunY));
	float bright = mix(2.8, 11.0, smoothstep(0.0, 0.22, sunY));
	return tint * (limb * edge * bright * vis);
}

void main()
{
	vec4 wp1 = uInvViewProj * vec4(vNDC,  1.0, 1.0);
	vec4 wp0 = uInvViewProj * vec4(vNDC, -1.0, 1.0);
	// NORMALIZE the reconstructed ray before the celestial/star path. The star + nebula
	// fields are sampled in cells of `cdir * 70` (cdir = celestialDir(dir)); the raw ray
	// (far-point − near-point) has a magnitude that changes with the camera orientation,
	// so an un-normalized dir makes a FIXED sky direction fall into DIFFERENT cells frame
	// to frame → stars flicker / jump as the camera turns. (skyColor/applyClouds
	// normalize internally, so nothing downstream needs the raw vector.)
	vec3 dir = normalize(wp1.xyz / wp1.w - wp0.xyz / wp0.w);
	if (uCloudPrepass > 0.5)
	{
		// Quarter-res clouds-only pass: output (L, T); the sky pass upsamples + composites.
		float T = 1.0; vec3 L;
		if (uCloudMode == 1)
		{
			vec3 hazeSky = skyColor(dir, uSunDir);   // aerial-perspective reference
			vec3 comp = applyClouds3D(hazeSky, dir, uCameraPos, uSunDir, uTime, uCloudCoverage,
			                          uSunColor, uWind, uCloudHeight, T);
			L = comp - hazeSky * T;                  // recover L
		}
		else
			L = applyClouds(vec3(0.0), dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, T);
		FragColor = vec4(L, T);
		return;
	}
	vec3 col  = skyColor(dir, uSunDir);
	// Lift the sun's cloud-occludable bloom out (re-added below) and compute the
	// geometric sun disk (a sky-only body, like the moon) to add on top of it.
	vec3 sunGlareCol = sunGlare(dir, uSunDir);
	vec3 sunBodyCol  = sunDisk(dir, uSunDir);
	col -= sunGlareCol;
	// Night-sky elements (stars/Milky Way/nebula/aurora/moon) + the celestial
	// rotation are skipped entirely by day. The branch is coherent — sunDir is a
	// uniform, so every pixel in the frame takes the same path — so it is cheap.
	float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(uSunDir).y, -0.2, 1.0));
	if (nightF > 0.0)
	{
		vec3 cdir = celestialDir(dir, uTimeOfDay);   // turns with the day-night cycle
		col += starField(dir, cdir, uSunDir, uTime, uMilkyWay) * uStarColor * uStarBright;
		col += nebula(dir, cdir, uSunDir, uNebula, uNebulaColor);
		col += applyAurora3D(dir, uCameraPos, uTime, uAurora, uAuroraColor, uAuroraColorTop);
		col += moonDisk(dir, uSunDir);
	}
	// High thin cirrus sits highest (farthest), then contrails, then the cumulus layer
	// in front — so the lower clouds correctly occlude the thin upper layers.
	col  = cirrus(col, dir, uSunDir, uSunColor, uCirrus, uCirrusSeed, uTime, uWind.xz); // alpha-blended
	col  = contrails(col, dir, uSunDir, uContrails, uCloudCoverage); // alpha-blended into the sky
	float cloudT = 1.0;                                   // view-ray cloud transmittance
	if (uLowResClouds > 0.5)
	{
		// Low-res clouds: composite the upsampled (L, T) from the quarter-res pre-pass.
		vec4 lt = texture(uCloudTex, vNDC * 0.5 + 0.5);
		col = col * lt.a + lt.rgb;
		cloudT = lt.a;
	}
	else if (uCloudMode == 1)
		col = applyClouds3D(col, dir, uCameraPos, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, uCloudHeight, cloudT);
	else
		col = applyClouds(col, dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, cloudT);
	// Re-add the sun, steeply occluded by cloud opacity so a solid cloud fully hides it.
	col += (sunGlareCol + sunBodyCol) * pow(cloudT, 2.5);
	col += uFlash * vec3(0.85, 0.90, 1.0); // lightning lights up the sky/clouds
	FragColor = vec4(col, 1.0);
}
)GLSL";

// Shared analytic sky, injected (via the //#SKYFUNC# marker) into both the
// skybox FS and the scene FS so background and image-based ambient match. The
// sky's mood is driven by the sun's elevation (sunDir.y): a daytime blue sky
// warms and reddens at the horizon as the sun sets and dims into night.
static const char* kSkyFuncGLSL = R"GLSL(
vec3 skyColor(vec3 dir, vec3 sunDir)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float sunY = clamp(sunDir.y, -0.3, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);                 // 0 night → 1 day
	// Warm-horizon (sunset/sunrise) factor — extended a touch BELOW the horizon so the
	// golden band lingers into the early blue hour like it does in reality.
	float dusk = smoothstep(-0.14, 0.04, sunY)
	           * (1.0 - smoothstep(0.04, 0.26, sunY));

	vec3 zenithDay  = vec3(0.09, 0.30, 0.78);                   // richer noon blue
	vec3 horizDay   = vec3(0.50, 0.66, 0.90);
	vec3 zenithTwi  = vec3(0.030, 0.055, 0.17);                 // blue-hour zenith (deep blue)
	vec3 horizTwi   = vec3(0.055, 0.075, 0.19);                 // blue-hour horizon base
	vec3 zenithNite = vec3(0.003, 0.005, 0.015);
	vec3 horizNite  = vec3(0.006, 0.009, 0.024);
	// 3-stage blend: full day → BLUE HOUR → deep night. The extended twilight range
	// (sun 0 → ~-0.24) keeps the sky a deep blue for a while after sunset instead of
	// snapping to black the instant the sun dips below the horizon.
	float toDay   = smoothstep(-0.08, 0.10, sunY);             // day vs twilight
	float toNight = 1.0 - smoothstep(-0.24, -0.06, sunY);      // twilight vs deep night
	vec3 zenith  = mix(mix(zenithTwi, zenithDay, toDay), zenithNite, toNight);
	vec3 horizon = mix(mix(horizTwi,  horizDay,  toDay), horizNite,  toNight);

	// Directional sunset warmth: the warm band is concentrated toward the sun's
	// azimuth (golden near the sun, cooler magenta away) instead of a flat ring,
	// and the zenith picks up a touch of dusk purple for atmospheric depth.
	vec2  sunAz  = normalize(sunDir.xz + vec2(1e-5));
	float toward = dot(normalize(dir.xz + vec2(1e-5)), sunAz) * 0.5 + 0.5; // 0 away → 1 toward
	toward = pow(clamp(toward, 0.0, 1.0), 1.8);                 // tighter warm wedge: only near the sun glows
	// Deep blue-purple away from the sun, burnt orange toward it. Kept well under 1.0 so
	// the ACES tonemap renders COLOUR, not a washed white.
	vec3  duskHoriz = mix(vec3(0.26, 0.18, 0.40), vec3(0.92, 0.42, 0.14), toward);
	horizon = mix(horizon, duskHoriz, dusk);                    // warm directional sunset band
	zenith  = mix(zenith,  vec3(0.11, 0.11, 0.30), dusk * 0.6); // dusk purple at zenith

	float h    = clamp(dir.y, 0.0, 1.0);
	float grad = pow(1.0 - h, 2.5);                             // horizon-weighted
	vec3 sky = mix(zenith, horizon, grad);

	// Warm horizon bands — modest so the sky stays a rich golden, not a bright wash.
	float band  = pow(1.0 - h, 8.0) * toward;
	float band2 = pow(1.0 - h, 3.5) * toward;
	sky += vec3(0.95, 0.50, 0.16) * (band  * dusk * 0.70);
	sky += vec3(0.60, 0.34, 0.14) * (band2 * dusk * 0.30);

	// Below the horizon: ease into a soft ground haze over a wide band so the
	// sky stays atmospheric just under the horizon line.
	vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));

	// Layered sun aureole — a crisp disk plus tight/mid blooms and a broad warm
	// scatter that survive through sunset for a cinematic, volumetric glow.
	// Warm golden-orange at low sun, easing to white when high. Low-sun tint is a
	// yellow-gold (not a hot near-white) so sunrise/sunset reads as COLOUR, not glare.
	vec3  sunTint = mix(vec3(1.0, 0.58, 0.24), vec3(1.0, 0.96, 0.88),
	                    smoothstep(0.0, 0.28, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	// Tame the near-sun bloom toward the horizon so it GLOWS instead of blinding; the
	// crisp daytime disk is unaffected (it is gated by `day`).
	float bloomDamp = mix(1.0, 0.28, dusk);                        // bloom much dimmer at dusk → no white blob
	// Crisp disk removed — the sun is now a geometric body (sunDisk in the sky FS),
	// kept out of skyColor so the shared IBL/fog reference isn't a razor-thin spike.
	sky += sunTint * (pow(s, 220.0)  * 1.1 * bloomDamp) * sunVis;  // tight bloom
	sky += sunTint * (pow(s, 30.0)   * 0.22 * bloomDamp) * sunVis; // mid aureole
	// Broad golden scatter — saturated + modest, so it tints the sun side gold instead
	// of blowing it out to white.
	sky += vec3(1.10, 0.46, 0.13) * (pow(s, 4.0) * 0.40) * dusk;

	// Moon: opposite the sun, fading in at night. The lit disk itself is drawn
	// (textured) in the sky pass; here we keep only the soft halo and a faint
	// fill so the night ambient/reflections aren't pitch black.
	// Opposite the sun in azimuth + elevation, but kept on the same hemisphere
	// (z sign) so it rises into the visible sky rather than behind the viewer.
	float night   = 1.0 - day;
	vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	float m       = max(dot(dir, moonDir), 0.0);
	vec3  moonTint= vec3(0.80, 0.86, 1.00);
	sky += moonTint * (pow(m, 60.0)   * 0.05) * night;          // soft halo
	sky += vec3(0.015, 0.018, 0.030) * night;                   // faint moonlit fill
	return sky;
}
)GLSL";

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
out vec4 FragColor;
vec3 aces(vec3 x)
{
	const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main()
{
	vec3 hdr    = texture(uHDR, vUV).rgb;
	hdr        += texture(uBloom, vUV).rgb * uBloomStrength;
	hdr        *= uExposure;
	vec3 mapped = aces(hdr);
	mapped      = pow(mapped, vec3(1.0 / 2.2));
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

// ─── SSAO (screen-space ambient occlusion) ──────────────────────────────────
// Number of hemisphere kernel samples; shared by the C++ kernel and the shader.
static constexpr int kSSAOKernel = 32;

// Pre-pass: rasterise the scene and write the per-pixel VIEW-SPACE position
// (xyz, with a = 1 marking valid geometry vs. the cleared background). Working in
// view space sidesteps every depth-buffer / clip-space convention difference
// between the backends — the SSAO maths is then identical on GL and Metal.
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

static const char* kSSAOPosFS = R"GLSL(
#version 410 core
in vec3 vViewPos;
out vec4 FragColor;
void main() { FragColor = vec4(vViewPos, 1.0); } // a = 1 → valid geometry
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
static const char* kUIVS = R"GLSL(
#version 410 core
uniform vec4 uRect;
uniform vec2 uViewport;
out vec2 vUV;
void main()
{
    const vec2 c[4] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 uv = c[gl_VertexID];
    vec2 sp = uRect.xy + uv * uRect.zw;
    vUV = uv;
    gl_Position = vec4(sp.x / uViewport.x * 2.0 - 1.0,
                       1.0 - sp.y / uViewport.y * 2.0,
                       0.0, 1.0);
}
)GLSL";

static const char* kUIFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform vec4 uColor;
out vec4 FragColor;
void main()
{
    FragColor = uColor;
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

OpenGLRenderer::OpenGLRenderer()  = default;
OpenGLRenderer::~OpenGLRenderer() = default;

void OpenGLRenderer::Initialize(HE::Window* window)
{
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initializing");
	m_primarySdlWindow = window->GetNativeWindow();
	m_glContext        = window->GetGLContext();
	if (!m_glContext)
		throw std::runtime_error("OpenGLRenderer: no GL context on window");

	if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)))
		throw std::runtime_error("OpenGLRenderer: gladLoadGLLoader failed");

	m_shaderManager = OpenGLShaderManager();

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
	CreateBloomPipeline();
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

	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initialized successfully");
}

static constexpr int kSkyEnvFace = 128; // image-based-ambient cubemap face size

// Cascaded shadow maps: number of depth-array layers / cascades. MUST match the
// shader's CSM_CASCADES (kUnlitFS) and stay ≤ ShadowData::kMaxCascades. The
// extractor fits ShadowData::cascadeCount (3) cascades; this caps the GL side to
// the same number it renders + samples.
static constexpr int kGLCsmCascades = 3;

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
	m_uCascadeVP     = glGetUniformLocation(m_unlitProgram, "uCascadeVP[0]");
	m_uCascadeSplits = glGetUniformLocation(m_unlitProgram, "uCascadeSplits");
	m_uCameraFwd     = glGetUniformLocation(m_unlitProgram, "uCameraFwd");
	m_uShadowMap     = glGetUniformLocation(m_unlitProgram, "uShadowMap");
	m_uShadowEnabled = glGetUniformLocation(m_unlitProgram, "uShadowEnabled");
	m_uShadowDebug   = glGetUniformLocation(m_unlitProgram, "uShadowDebug");
	m_uAO            = glGetUniformLocation(m_unlitProgram, "uAO");
	m_uViewport      = glGetUniformLocation(m_unlitProgram, "uViewport");
	m_uSSAOEnabled   = glGetUniformLocation(m_unlitProgram, "uSSAOEnabled");
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
		Logger::Log(Logger::LogLevel::Error,
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
	m_uSkinnedShadowMap          = loc("uShadowMap");
	m_uSkinnedAO                 = loc("uAO");
	m_uSkinnedViewport           = loc("uViewport");
	m_uSkinnedSSAOEnabled        = loc("uSSAOEnabled");
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
		Logger::Log(Logger::LogLevel::Error,
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
	m_uInstCascadeVP        = loc("uCascadeVP[0]");
	m_uInstCascadeSplits    = loc("uCascadeSplits");
	m_uInstCameraFwd        = loc("uCameraFwd");
	m_uInstShadowDebug      = loc("uShadowDebug");
	m_uInstShadowMap        = loc("uShadowMap");
	m_uInstShadowEnabled    = loc("uShadowEnabled");
	m_uInstAO               = loc("uAO");
	m_uInstViewport         = loc("uViewport");
	m_uInstSSAOEnabled      = loc("uSSAOEnabled");
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
	m_uSkyAuroraColor = glGetUniformLocation(m_skyProgram, "uAuroraColor");
	m_uSkyAuroraColorTop = glGetUniformLocation(m_skyProgram, "uAuroraColorTop");
	m_uSkyAuroraHeight   = glGetUniformLocation(m_skyProgram, "uAuroraHeight");
	m_uSkyAuroraFragment = glGetUniformLocation(m_skyProgram, "uAuroraFragment");
	m_uSkyWind        = glGetUniformLocation(m_skyProgram, "uWind");
	m_uSkyNoise       = glGetUniformLocation(m_skyProgram, "uNoise");
	m_uSkyCloudTex     = glGetUniformLocation(m_skyProgram, "uCloudTex");
	m_uSkyLowResClouds = glGetUniformLocation(m_skyProgram, "uLowResClouds");
	m_uSkyCloudPrepass = glGetUniformLocation(m_skyProgram, "uCloudPrepass");
	m_uSkyFlash       = glGetUniformLocation(m_skyProgram, "uFlash");
	m_uSkyCloudMode   = glGetUniformLocation(m_skyProgram, "uCloudMode");
	m_uSkyCloudQuality = glGetUniformLocation(m_skyProgram, "uCloudQuality");
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
	const std::vector<uint16_t> noise = BuildSkyNoise3D(kNoiseN);
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
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: bloom FBO incomplete");
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

void OpenGLRenderer::EnsureCloudFBO(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_cloudFBO && width == m_cloudW && height == m_cloudH) return;
	DestroyCloudFBO();
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
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: cloud FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_cloudW = width;
	m_cloudH = height;
}

void OpenGLRenderer::DestroyCloudFBO()
{
	if (m_cloudFBO) glDeleteFramebuffers(1, &m_cloudFBO);
	if (m_cloudTex) glDeleteTextures(1, &m_cloudTex);
	m_cloudFBO = 0; m_cloudTex = 0;
	m_cloudW = m_cloudH = 0;
}

// Bright-pass the HDR color, then ping-pong blur. Leaves the result in
// m_bloomColor[0] and returns its id. Assumes m_fsVAO is the active VAO and
// depth test is already disabled. Restores nothing (caller rebinds output).
unsigned int OpenGLRenderer::RenderBloom(int fullW, int fullH)
{
	EnsureBloomTargets(fullW / 2, fullH / 2);
	if (!m_bloomFBO[0]) return 0;

	glViewport(0, 0, m_bloomW, m_bloomH);

	// Bright pass: HDR scene color → m_bloomColor[0].
	glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFBO[0]);
	glUseProgram(m_bloomBrightProgram);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_hdrColor);
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

// ─── SSAO ────────────────────────────────────────────────────────────────────
// Deterministic [0,1) RNG so the GL and Metal backends build the *identical*
// hemisphere kernel and rotation noise — a prerequisite for GL == Metal parity.
struct SsaoRng { uint32_t s; float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); } };

// Cosine-ish hemisphere kernel oriented to +Z, packed toward the origin so close
// occluders dominate. Shared verbatim with the Metal backend.
static std::vector<glm::vec3> BuildSSAOKernel(int n)
{
	SsaoRng rng{ 0x9E3779B9u };
	std::vector<glm::vec3> k(n);
	for (int i = 0; i < n; ++i)
	{
		glm::vec3 s(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next());
		s = glm::normalize(s) * rng.next();
		float t = static_cast<float>(i) / static_cast<float>(n);
		s *= 0.1f + 0.9f * t * t;   // accelerate the distribution toward the centre
		k[i] = s;
	}
	return k;
}
// 4×4 tile of random rotation vectors in the tangent plane (z = 0). Returned as
// vec3 so the upload matches GL_RGB. Shared verbatim with the Metal backend.
static std::vector<glm::vec3> BuildSSAONoise(int n)
{
	SsaoRng rng{ 0x2545F491u };
	std::vector<glm::vec3> v(n);
	for (int i = 0; i < n; ++i)
		v[i] = glm::vec3(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, 0.0f);
	return v;
}

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
		const std::vector<glm::vec3> kernel = BuildSSAOKernel(kSSAOKernel);
		glUseProgram(m_ssaoProgram);
		glUniform3fv(m_uSsaoKernel, kSSAOKernel, glm::value_ptr(kernel[0]));
		glUseProgram(0);
	}
	// 4×4 rotation-noise texture (NEAREST + REPEAT so it tiles per 4×4 screen block).
	{
		const std::vector<glm::vec3> noise = BuildSSAONoise(16);
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
}

void OpenGLRenderer::SetSSAOSettings(const SSAOSettings& s)
{
	m_ssaoEnabled   = s.enabled;
	m_ssaoRadius    = s.radius;
	m_ssaoIntensity = s.intensity;
	m_ssaoMethod    = s.method;
}

void OpenGLRenderer::EnsureSSAOTargets(int width, int height)
{
	width  = std::max(1, width);
	height = std::max(1, height);
	if (m_ssaoPosFBO && width == m_ssaoW && height == m_ssaoH)
		return;
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
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: SSAO FBO incomplete");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::DestroySSAOTargets()
{
	if (m_ssaoPosFBO)   { glDeleteFramebuffers(1, &m_ssaoPosFBO);    m_ssaoPosFBO = 0; }
	if (m_ssaoPosTex)   { glDeleteTextures(1, &m_ssaoPosTex);        m_ssaoPosTex = 0; }
	if (m_ssaoPosDepth) { glDeleteRenderbuffers(1, &m_ssaoPosDepth); m_ssaoPosDepth = 0; }
	if (m_ssaoFBO)      { glDeleteFramebuffers(1, &m_ssaoFBO);       m_ssaoFBO = 0; }
	if (m_ssaoTex)      { glDeleteTextures(1, &m_ssaoTex);           m_ssaoTex = 0; }
	if (m_ssaoBlurFBO)  { glDeleteFramebuffers(1, &m_ssaoBlurFBO);   m_ssaoBlurFBO = 0; }
	if (m_ssaoBlurTex)  { glDeleteTextures(1, &m_ssaoBlurTex);       m_ssaoBlurTex = 0; }
	m_ssaoW = m_ssaoH = 0;
}

// Pre-pass (view position) → occlusion → blur. Leaves GL_TEXTURE0 active and the
// depth test disabled; the caller re-binds its own target + depth state.
unsigned int OpenGLRenderer::RenderSSAO(const CommandBuffer& cmds, int pw, int ph,
	const glm::mat4& viewProj, const glm::mat4& view, const glm::mat4& proj)
{
	if (!m_ssaoPosProgram || !m_ssaoProgram || !m_ssaoBlurProgram) return 0;
	EnsureSSAOTargets(pw, ph);
	if (!m_ssaoPosFBO) return 0;

	// ── 1. View-space position pre-pass ────────────────────────────────────
	glBindFramebuffer(GL_FRAMEBUFFER, m_ssaoPosFBO);
	glViewport(0, 0, pw, ph);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // a = 0 → background
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(m_ssaoPosProgram);
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

		// Instanced batches: draw each instance separately with its own transform.
		// The SSAO pre-pass uses per-draw uniforms, not the instance VBO.
		if (!dc.instanceTransforms.empty())
		{
			for (const glm::mat4& t : dc.instanceTransforms)
			{
				glUniformMatrix4fv(m_uPosMVP,       1, GL_FALSE, glm::value_ptr(viewProj * t));
				glUniformMatrix4fv(m_uPosModelView, 1, GL_FALSE, glm::value_ptr(view * t));
				glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
			}
		}
		else
		{
			glUniformMatrix4fv(m_uPosMVP,       1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
			glUniformMatrix4fv(m_uPosModelView, 1, GL_FALSE, glm::value_ptr(view * dc.transform));
			glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
		}
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

void OpenGLRenderer::RenderUIPass(int pw, int ph)
{
	if (!m_uiProgram || m_renderWorld.uiObjects.empty()) return;

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(m_uiProgram);
	glBindVertexArray(m_fsVAO);
	glUniform2f(m_uUIViewport, static_cast<float>(pw), static_cast<float>(ph));

	for (const UIRenderObject& obj : m_renderWorld.uiObjects)
	{
		glUniform4f(m_uUIRect,  obj.position.x, obj.position.y, obj.size.x, obj.size.y);
		glUniform4f(m_uUIColor, obj.color.r, obj.color.g, obj.color.b, obj.color.a);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}

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

	glGenRenderbuffers(1, &m_hdrDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_hdrDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_hdrDepth);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: HDR FBO incomplete");

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	m_hdrW = width;
	m_hdrH = height;
}

void OpenGLRenderer::DestroyHDRTarget()
{
	if (m_hdrFBO)   { glDeleteFramebuffers(1, &m_hdrFBO);   m_hdrFBO = 0; }
	if (m_hdrColor) { glDeleteTextures(1, &m_hdrColor);     m_hdrColor = 0; }
	if (m_hdrDepth) { glDeleteRenderbuffers(1, &m_hdrDepth);m_hdrDepth = 0; }
	m_hdrW = m_hdrH = 0;
}

// ─── Asset mesh upload ────────────────────────────────────────────────────────
const OpenGLRenderer::GpuMesh* OpenGLRenderer::ResolveMesh(const HE::UUID& assetId)
{
	if (assetId == HE::UUID{} || !m_contentManager)
		return nullptr;

	if (auto it = m_meshCache.find(assetId); it != m_meshCache.end())
		return &it->second;

	const StaticMeshAsset* asset = m_contentManager->getStaticMesh(assetId);
	if (!asset || asset->vertices.empty() || asset->indices.empty())
		return nullptr;

	// Interleave position + normal + uv (8 floats per vertex). Missing
	// normals/uvs are zero-filled so every mesh fits the unlit layout.
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

	GpuMesh mesh;
	mesh.indexCount  = static_cast<int>(asset->indices.size());
	mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);

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
				glGenTextures(1, &mesh.texture);
				glBindTexture(GL_TEXTURE_2D, mesh.texture);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				             static_cast<GLsizei>(tex->width), static_cast<GLsizei>(tex->height),
				             0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
				glGenerateMipmap(GL_TEXTURE_2D);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
	}

	Logger::Log(Logger::LogLevel::Info,
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

	// Base color texture
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
				glGenTextures(1, &mesh.texture);
				glBindTexture(GL_TEXTURE_2D, mesh.texture);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
				             static_cast<GLsizei>(tex->width), static_cast<GLsizei>(tex->height),
				             0, GL_RGBA, GL_UNSIGNED_BYTE, tex->data.data());
				glGenerateMipmap(GL_TEXTURE_2D);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glBindTexture(GL_TEXTURE_2D, 0);
			}
		}
	}

	Logger::Log(Logger::LogLevel::Info,
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
	if (!mat->texturePaths.empty())
	{
		const HE::UUID texId = m_contentManager->loadAsset(mat->texturePaths[0]);
		if (const TextureAsset* t = m_contentManager->getTexture(texId);
		    t && !t->data.empty() && t->channels == 4)
		{
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			             static_cast<GLsizei>(t->width), static_cast<GLsizei>(t->height),
			             0, GL_RGBA, GL_UNSIGNED_BYTE, t->data.data());
			glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}

	m_materialTexCache.emplace(materialId, tex);
	outTex = tex;
	return true;
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
	outOpacity   = mat->opacity;
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

void OpenGLRenderer::Shutdown()
{
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: shutdown");

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
	for (auto& [id, tex] : m_materialTexCache)
		if (tex) glDeleteTextures(1, &tex);
	m_materialTexCache.clear();
	DestroyViewportTarget();
	DestroyHDRTarget();
	DestroyBloomTargets();
	DestroyCloudFBO();
	DestroyLdrTarget();
	DestroyGpuTimer();
	DestroySSAOTargets();
	for (auto& r : m_retiredTextures)
		glDeleteTextures(1, &r.texture);
	m_retiredTextures.clear();

	if (m_unlitProgram)     { glDeleteProgram(m_unlitProgram);     m_unlitProgram = 0; }
	if (m_instancedProgram) { glDeleteProgram(m_instancedProgram); m_instancedProgram = 0; }
	if (m_instanceVBO)      { glDeleteBuffers(1, &m_instanceVBO);  m_instanceVBO = 0; }
	DestroyParticleResources();
	if (m_depthProgram) { glDeleteProgram(m_depthProgram);     m_depthProgram = 0; }
	if (m_skyProgram)   { glDeleteProgram(m_skyProgram);       m_skyProgram = 0; }
	if (m_tonemapProgram) { glDeleteProgram(m_tonemapProgram); m_tonemapProgram = 0; }
	if (m_fxaaProgram)    { glDeleteProgram(m_fxaaProgram);    m_fxaaProgram = 0; }
	if (m_uiProgram)      { glDeleteProgram(m_uiProgram);      m_uiProgram = 0; }
	if (m_bloomBrightProgram) { glDeleteProgram(m_bloomBrightProgram); m_bloomBrightProgram = 0; }
	if (m_blurProgram)    { glDeleteProgram(m_blurProgram);    m_blurProgram = 0; }
	if (m_fsVAO)          { glDeleteVertexArrays(1, &m_fsVAO);  m_fsVAO = 0; }
	if (m_shadowFBO)      { glDeleteFramebuffers(1, &m_shadowFBO);   m_shadowFBO = 0; }
	if (m_shadowDepthTex) { glDeleteTextures(1, &m_shadowDepthTex);  m_shadowDepthTex = 0; }
	if (m_moonTex)        { glDeleteTextures(1, &m_moonTex);         m_moonTex = 0; }
	if (m_noiseTex)       { glDeleteTextures(1, &m_noiseTex);        m_noiseTex = 0; }
	if (m_skyEnvCube)     { glDeleteTextures(1, &m_skyEnvCube);      m_skyEnvCube = 0; }
	if (m_ssaoNoiseTex)   { glDeleteTextures(1, &m_ssaoNoiseTex);    m_ssaoNoiseTex = 0; }
	if (m_whiteTex)       { glDeleteTextures(1, &m_whiteTex);        m_whiteTex = 0; }
	if (m_ssaoPosProgram)  { glDeleteProgram(m_ssaoPosProgram);  m_ssaoPosProgram = 0; }
	if (m_ssaoProgram)     { glDeleteProgram(m_ssaoProgram);     m_ssaoProgram = 0; }
	if (m_ssaoBlurProgram) { glDeleteProgram(m_ssaoBlurProgram); m_ssaoBlurProgram = 0; }
	if (m_debugLineProgram) { glDeleteProgram(m_debugLineProgram); m_debugLineProgram = 0; }
	if (m_debugLineVAO)     { glDeleteVertexArrays(1, &m_debugLineVAO); m_debugLineVAO = 0; }
	if (m_debugLineVBO)     { glDeleteBuffers(1, &m_debugLineVBO);      m_debugLineVBO = 0; }

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
		Logger::Log(Logger::LogLevel::Error, "OpenGLRenderer: viewport FBO incomplete");

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
		if (auto it = m_meshCache.find(id); it != m_meshCache.end())
		{
			auto& m = it->second;
			if (m.vao) glDeleteVertexArrays(1, &m.vao);
			if (m.vbo) glDeleteBuffers(1, &m.vbo);
			if (m.ebo) glDeleteBuffers(1, &m.ebo);
			m_meshCache.erase(it);
		}
	m_pendingMeshInvalidations.clear();

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(pw) / static_cast<float>(ph),
	                    &m_editorCamera);
	m_extractor.extractUI(*m_world, static_cast<float>(pw), static_cast<float>(ph),
	                      m_renderWorld);
	// NB: do NOT early-out when there are no (visible) objects — the skybox is the
	// background and must still be drawn, or the viewport falls back to a stale
	// gray clear when the camera looks away from the scene.

	const glm::mat4 viewProj    = m_renderWorld.camera.projection * m_renderWorld.camera.view;
	const glm::mat4 invViewProj = glm::inverse(viewProj);

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

	const bool shadows = m_renderWorld.shadow.enabled && m_shadowFBO != 0;

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
			if (!shadows) return;
			GpuPassScope _shadowTimer(this, "Shadow"); // ends (glEndQuery) at this branch's return
			glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
			glViewport(0, 0, m_shadowSize, m_shadowSize);
			glUseProgram(m_depthProgram);
			// Push depth values away from the light camera so shadow-map samples
			// for back-lit fragments never self-shadow the surface (shadow acne).
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.0f, 4.0f);

			const int cascades = std::clamp(m_renderWorld.shadow.cascadeCount, 1, kGLCsmCascades);
			for (int c = 0; c < cascades; ++c)
			{
				// Render this cascade into its own depth-array layer.
				glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				                          m_shadowDepthTex, 0, c);
				glClear(GL_DEPTH_BUFFER_BIT);

				// Cull + sort against this cascade's light frustum into scratch
				// buffers (NOT m_visible/m_sortedIndices — those hold the camera cull
				// the geometry pass consumes). Sorting keeps draws grouped by mesh id
				// so the per-mesh resolve memoisation below stays valid.
				const glm::mat4& cvp = m_renderWorld.shadow.cascadeViewProj[c];
				m_culler.cull(m_renderWorld, cvp, m_shadowVisible);
				m_sorter.sort(m_renderWorld, m_shadowVisible, m_shadowSorted);

				HE::UUID shMeshId{}; const GpuMesh* shMesh = nullptr; bool shMeshValid = false;
				for (uint32_t idx : m_shadowSorted)
				{
					const RenderObject& obj = m_renderWorld.objects[idx];
					if (!obj.castsShadow) continue; // billboards (precip/particles) cast none
					glUniformMatrix4fv(m_uDepthMVP, 1, GL_FALSE,
					                   glm::value_ptr(cvp * obj.transform));
					if (!shMeshValid || obj.meshAssetId != shMeshId)
					{
						shMesh      = ResolveMesh(obj.meshAssetId);
						shMeshId    = obj.meshAssetId; shMeshValid = true;
					}
					const GpuMesh* mesh = shMesh ? shMesh : ResolveMesh(HE::kDefaultCubeMeshId);
					if (!mesh) continue;
					glBindVertexArray(mesh->vao);
					glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
				}
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

			// Bright-pass + blur the HDR target into the half-res bloom buffer
			// (skipped when bloom is disabled → strength 0 below).
			unsigned int bloomTex = 0u;
			if (m_bloomEnabled)
			{
				GpuPassScope _bloomTimer(this, "Bloom");
				bloomTex = RenderBloom(pw, ph);
			}

			// Tonemap HDR scene color + bloom into the LDR intermediate (FXAA reads it).
			{
				GpuPassScope _tonemapTimer(this, "Tonemap");
				EnsureLdrTarget(pw, ph);
				glBindFramebuffer(GL_FRAMEBUFFER, m_ldrFBO);
				glViewport(0, 0, pw, ph);
				glUseProgram(m_tonemapProgram);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, m_hdrColor);
				glUniform1i(m_uHDRTex, 0);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, bloomTex);
				glUniform1i(m_uBloomTex, 1);
				glUniform1f(m_uExposure, 1.0f);
				glUniform1f(m_uBloomStrength, bloomTex ? m_bloomStrength : 0.0f);
				glActiveTexture(GL_TEXTURE0);
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}

			// FXAA the tonemapped LDR image into the actual output.
			{
				GpuPassScope _fxaaTimer(this, "FXAA");
				glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
				glViewport(0, 0, pw, ph);
				glUseProgram(m_fxaaProgram);
				glBindTexture(GL_TEXTURE_2D, m_ldrColor);
				glUniform1i(m_uFxaaScene, 0);
				glUniform2f(m_uFxaaRcpFrame, 1.0f / float(pw), 1.0f / float(ph));
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}

			{
				GpuPassScope _uiTimer(this, "UI");
				RenderUIPass(pw, ph);
			}

			glEnable(GL_DEPTH_TEST);
			return;
		}

		// ── SSAO: view-space position pre-pass → occlusion → blur ───────────
		// Computed before shading (using these same geometry draw calls) so the
		// scene shader can darken its ambient term. Skipped (zero cost) when off.
		unsigned int aoTex = 0u;
		if (m_ssaoEnabled)
		{
			GpuPassScope _ssaoTimer(this, "SSAO");
			aoTex = RenderSSAO(cmds, pw, ph, viewProj, m_renderWorld.camera.view,
			                   m_renderWorld.camera.projection);
		}

		// ── Geometry pass: scene program + per-frame state, into HDR target ─
		// Set here (not before the graph) because the shadow pass switched the
		// active program. Opaque geometry, the procedural sky/clouds and transparency
		// are timed as SIBLING GPU passes (GL_TIME_ELAPSED cannot nest — see
		// GpuTimerBeginPass), breaking the heavy sky/cloud cost out of the old single
		// "Scene" scope so it is individually measurable (matches the Metal backend's
		// "Opaque" / "Sky+Clouds" markers). The geometry branch has no early return, so
		// these explicit begin/end pairs always balance.
		GpuTimerBeginPass("Opaque");
		glBindFramebuffer(GL_FRAMEBUFFER, m_hdrFBO);
		glViewport(0, 0, pw, ph);
		// Explicit depth state for the opaque geometry: test on, write on, LESS, so
		// the depth clear takes and the sky (drawn last) can test against it.
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

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
		// SSAO occlusion on unit 4 (white fallback when off → ao = 1, no change).
		const bool aoActive = m_ssaoEnabled && aoTex != 0;
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
		glUniform1i(m_uAO, 4);
		glActiveTexture(GL_TEXTURE0);
		glUniform2f(m_uViewport, static_cast<float>(pw), static_cast<float>(ph));
		glUniform1i(m_uSSAOEnabled, aoActive ? 1 : 0);

		// Lights (clamped to the shader's MAX_LIGHTS)
		{
			constexpr int kMaxLights = 8;
			const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), kMaxLights);
			glm::vec4 pos[kMaxLights], dir[kMaxLights], color[kMaxLights], params[kMaxLights];
			for (int i = 0; i < count; ++i)
			{
				const LightData& l = m_renderWorld.lights[i];
				pos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
				dir[i]    = glm::vec4(l.direction, l.spotAngleCos);
				color[i]  = glm::vec4(l.color,     l.intensity);
				params[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
			}
			glUniform1i(m_uLightCount, count);
			if (count > 0)
			{
				glUniform4fv(m_uLightPos,    count, glm::value_ptr(pos[0]));
				glUniform4fv(m_uLightDir,    count, glm::value_ptr(dir[0]));
				glUniform4fv(m_uLightColor,  count, glm::value_ptr(color[0]));
				glUniform4fv(m_uLightParams, count, glm::value_ptr(params[0]));
			}
			glUniform3fv(m_uCameraPos, 1, glm::value_ptr(m_renderWorld.camera.position));
		}

		// CSM shadow-map array bound on texture unit 1. Always bound (the sampling
		// is gated by uShadowEnabled) so the sampler2DArray never reads a mismatched
		// target; the per-cascade matrices/splits/forward drive the selection.
		glUniform1i(m_uShadowEnabled, shadows ? 1 : 0);
		glUniform1i(m_uShadowDebug,   m_debugShadowCascades ? 1 : 0);
		glUniformMatrix4fv(m_uCascadeVP, kGLCsmCascades, GL_FALSE, cascadeVPData);
		glUniform4fv(m_uCascadeSplits, 1, glm::value_ptr(cascadeSplits));
		glUniform3fv(m_uCameraFwd, 1, glm::value_ptr(camFwd));
		glUniform1i(m_uShadowMap, 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
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
			glUniform1i(m_uInstAO, 4);
			glUniform2f(m_uInstViewport, static_cast<float>(pw), static_cast<float>(ph));
			glUniform1i(m_uInstSSAOEnabled, aoActive ? 1 : 0);
			{
				constexpr int kMaxLights = 8;
				const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), kMaxLights);
				glm::vec4 pos[kMaxLights], dir[kMaxLights], color[kMaxLights], params[kMaxLights];
				for (int i = 0; i < count; ++i)
				{
					const LightData& l = m_renderWorld.lights[i];
					pos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
					dir[i]    = glm::vec4(l.direction, l.spotAngleCos);
					color[i]  = glm::vec4(l.color,     l.intensity);
					params[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
				}
				glUniform1i(m_uInstLightCount, count);
				if (count > 0)
				{
					glUniform4fv(m_uInstLightPos,    count, glm::value_ptr(pos[0]));
					glUniform4fv(m_uInstLightDir,    count, glm::value_ptr(dir[0]));
					glUniform4fv(m_uInstLightColor,  count, glm::value_ptr(color[0]));
					glUniform4fv(m_uInstLightParams, count, glm::value_ptr(params[0]));
				}
				glUniform3fv(m_uInstCameraPos, 1, glm::value_ptr(m_renderWorld.camera.position));
			}
			glUniform1i(m_uInstShadowEnabled, shadows ? 1 : 0);
			glUniform1i(m_uInstShadowDebug,   m_debugShadowCascades ? 1 : 0);
			glUniformMatrix4fv(m_uInstCascadeVP, kGLCsmCascades, GL_FALSE, cascadeVPData);
			glUniform4fv(m_uInstCascadeSplits, 1, glm::value_ptr(cascadeSplits));
			glUniform3fv(m_uInstCameraFwd, 1, glm::value_ptr(camFwd));
			glUniform1i(m_uInstShadowMap, 1);
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
		                unsigned int tex, vao; int indexCount; float distSq; };
		std::vector<TPDraw> transparent;
		const glm::vec3 camPos = m_renderWorld.camera.position;

		glUniform1f(m_uOpacity, 1.0f); // opaque pass writes alpha 1
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

			const GpuMesh* drawMesh  = mesh ? mesh : ResolveMesh(HE::kDefaultCubeMeshId);
			if (!drawMesh) continue;
			const unsigned int vao        = drawMesh->vao;
			const int          indexCount = drawMesh->indexCount;

			if (cOpacity < 0.999f)
			{
				// Transparent instanced batches: push one TPDraw per instance so
				// each object is sorted individually by distance.
				auto pushTP = [&](const glm::mat4& t) {
					const glm::vec3 d = glm::vec3(t[3]) - camPos;
					transparent.push_back({ viewProj * t, t, baseColor,
					                        cMetallic, cRoughness, cOpacity, tex, vao, indexCount,
					                        glm::dot(d, d) });
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
				glBindVertexArray(vao);
				glUniform1i(m_uInstHasTexture, tex != 0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr,
				                        static_cast<GLsizei>(dc.instanceTransforms.size()));
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3) *
				                   static_cast<uint32_t>(dc.instanceTransforms.size());
				glUseProgram(m_unlitProgram); // restore for the next single-draw
			}
			else
			{
				glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
				glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(dc.transform));
				glUniform3fv(m_uColor, 1, glm::value_ptr(baseColor));
				glUniform1f(m_uMetallic,  cMetallic);
				glUniform1f(m_uRoughness, cRoughness);
				glBindVertexArray(vao);
				glUniform1i(m_uHasTexture, tex != 0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(indexCount / 3);
			}
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
			glUniform1i(m_uSkinnedShadowEnabled, shadows ? 1 : 0);
			glUniform1i(m_uSkinnedShadowDebug,   m_debugShadowCascades ? 1 : 0);
			glUniformMatrix4fv(m_uSkinnedCascadeVP, kGLCsmCascades, GL_FALSE, cascadeVPData);
			glUniform4fv(m_uSkinnedCascadeSplits, 1, glm::value_ptr(cascadeSplits));
			glUniform3fv(m_uSkinnedCameraFwd, 1, glm::value_ptr(camFwd));
			glUniform1i(m_uSkinnedShadowMap, 1);
			// Re-assert the CSM array on unit 1 — opaque/instanced draws and the AO
			// bind run between the unlit setup and here; this guarantees the skinned
			// sampler2DArray reads the shadow array, not a stale 2D texture.
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowDepthTex);
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_CUBE_MAP, m_skyEnvCube);
			glUniform1i(m_uSkinnedSkyEnv, 3);
			glActiveTexture(GL_TEXTURE4);
			glBindTexture(GL_TEXTURE_2D, aoActive ? aoTex : m_whiteTex);
			glUniform1i(m_uSkinnedAO, 4);
			glActiveTexture(GL_TEXTURE0);
			glUniform2f(m_uSkinnedViewport, static_cast<float>(pw), static_cast<float>(ph));
			glUniform1i(m_uSkinnedSSAOEnabled, aoActive ? 1 : 0);
			glUniform3fv(m_uSkinnedCameraPos, 1, glm::value_ptr(m_renderWorld.camera.position));
			{
				constexpr int kMaxLights = 8;
				const int count = std::min(static_cast<int>(m_renderWorld.lights.size()), kMaxLights);
				glm::vec4 pos[kMaxLights], dir[kMaxLights], color[kMaxLights], params[kMaxLights];
				for (int i = 0; i < count; ++i)
				{
					const LightData& l = m_renderWorld.lights[i];
					pos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
					dir[i]    = glm::vec4(l.direction, l.spotAngleCos);
					color[i]  = glm::vec4(l.color,     l.intensity);
					params[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
				}
				glUniform1i(m_uSkinnedLightCount, count);
				if (count > 0)
				{
					glUniform4fv(m_uSkinnedLightPos,    count, glm::value_ptr(pos[0]));
					glUniform4fv(m_uSkinnedLightDir,    count, glm::value_ptr(dir[0]));
					glUniform4fv(m_uSkinnedLightColor,  count, glm::value_ptr(color[0]));
					glUniform4fv(m_uSkinnedLightParams, count, glm::value_ptr(params[0]));
				}
			}

			constexpr int kMaxBones = 128;
			// Scratch buffer for the full bone matrix upload per draw call.
			// Filled with the draw's matrices, rest is identity (safe default).
			std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));

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
				glUniform1i(m_uSkinnedHasTex,       tex != 0);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, tex);
				glBindVertexArray(smesh->vao);
				glDrawElements(GL_TRIANGLES, smesh->indexCount, GL_UNSIGNED_INT, nullptr);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(smesh->indexCount / 3);
			}

			glUseProgram(m_unlitProgram); // restore for the sky + transparent passes
		}
		GpuTimerEndPass();                 // end "Opaque"
		GpuTimerBeginPass("Sky+Clouds");   // sibling (matches Metal)

		// ── Skybox (drawn LAST): fill the remaining background with the procedural
		// sky. The fullscreen triangle sits at z = 1 (far plane); with GL_LEQUAL and
		// no depth write it passes only where the geometry left depth == 1 (i.e. the
		// background), so the heavy sky shader is never paid for behind solid
		// objects. With no geometry the whole frame is background → full sky.
		if (m_skyProgram)
		{
			glUseProgram(m_skyProgram);
			glDepthFunc(GL_LEQUAL);
			glDepthMask(GL_FALSE);
			glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(invViewProj));
			glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(sunDir));
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_moonTex);
			glUniform1i(m_uSkyMoonTex, 0);
			glUniform1i(m_uSkyHasMoon, m_moonTex ? 1 : 0);
			glUniform1f(m_uSkyMoonPhase, GetEnvironment().moonPhase);
			glUniform1f(m_uSkyTime, GetEnvironment().timeOfDay);
			glUniform1f(m_uSkyCoverage, GetEnvironment().cloudCoverage);
			// Cloud render mode + 3D-cloud parallax inputs (camera world pos + layer height).
			glUniform1i(m_uSkyCloudMode,   GetEnvironment().cloudMode);
			glUniform1i(m_uSkyCloudQuality, GetEnvironment().cloudQuality);
			glUniform3fv(m_uSkyCameraPos,  1, glm::value_ptr(m_renderWorld.camera.position));
			glUniform1f(m_uSkyCloudHeight, GetEnvironment().cloudHeight);
			glUniform1f(m_uSkyCloudDensity,    GetEnvironment().cloudDensity);
			glUniform1f(m_uSkyCloudFluffiness, GetEnvironment().cloudFluffiness);
			glUniform3fv(m_uSkyCloudTint, 1, glm::value_ptr(GetEnvironment().cloudTint));
			glUniform1f(m_uSkyContrails,  GetEnvironment().contrailAmount);
			glUniform1f(m_uSkyCirrus,     GetEnvironment().cirrusAmount);
			glUniform1f(m_uSkyCirrusSeed, GetEnvironment().cirrusSeed);
			glUniform1f(m_uSkyStarBright, GetEnvironment().starBrightness);
			glUniform3fv(m_uSkyStarColor, 1, glm::value_ptr(GetEnvironment().starColor));
			glUniform1f(m_uSkyStarSize,    GetEnvironment().starSize);
			glUniform1f(m_uSkyStarSizeVar, GetEnvironment().starSizeVariation);
			glUniform1f(m_uSkyStarDensity, GetEnvironment().starDensity);
			glUniform1f(m_uSkyStarGlow,    GetEnvironment().starGlow);
			glUniform1f(m_uSkyStarTwinkle, GetEnvironment().starTwinkle);
			// HE_SKY_TIME overrides the animation clock (for deterministic headless capture
			// of time-animated sky elements like the aurora); normal runs use the wall clock.
			float skyClock = static_cast<float>(SDL_GetTicks()) / 1000.0f;
			if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov) skyClock = static_cast<float>(std::atof(ov));
			glUniform1f(m_uSkyClock, skyClock);
			glUniform3fv(m_uSkySunColor, 1, glm::value_ptr(GetEnvironment().sunColor));
			glUniform1f(m_uSkyAurora, GetEnvironment().auroraIntensity);
			glUniform1f(m_uSkyAuroraHeight,   GetEnvironment().auroraHeight);
			glUniform1f(m_uSkyAuroraFragment, GetEnvironment().auroraFragmentation);
			glUniform1f(m_uSkyMilkyWay, GetEnvironment().milkyWayIntensity);
			glUniform1f(m_uSkyNebula, GetEnvironment().nebulaIntensity);
			glUniform3fv(m_uSkyNebulaColor, 1, glm::value_ptr(GetEnvironment().nebulaColor));
			glUniform3fv(m_uSkyNebulaColor2, 1, glm::value_ptr(GetEnvironment().nebulaColor2));
			glUniform3fv(m_uSkyNebulaColor3, 1, glm::value_ptr(GetEnvironment().nebulaColor3));
			glUniform1f(m_uSkyNebulaSeed, GetEnvironment().nebulaSeed);
			glUniform1f(m_uSkyNebulaHiFi, (float)GetEnvironment().nebulaQuality); // 0/1/2 (carried in uNebulaHiFi)
			glUniform3fv(m_uSkyAuroraColor, 1, glm::value_ptr(GetEnvironment().auroraColor));
			glUniform3fv(m_uSkyAuroraColorTop, 1, glm::value_ptr(GetEnvironment().auroraColorTop));
			glUniform1f(m_uSkyFlash, GetEnvironment().flash);
			{
				// Wind control → horizontal cloud drift vector. Direction 0° drifts
				// toward -Z (north), increasing clockwise; speed scales the rate.
				const float wr = glm::radians(GetEnvironment().windDirection);
				const glm::vec3 wind = glm::vec3(std::sin(wr), 0.0f, -std::cos(wr))
				                     * (GetEnvironment().windSpeed * 0.025f);
				glUniform3fv(m_uSkyWind, 1, glm::value_ptr(wind));
			}
			glActiveTexture(GL_TEXTURE2);             // 3D value-noise on unit 2
			glBindTexture(GL_TEXTURE_3D, m_noiseTex);
			glUniform1i(m_uSkyNoise, 2);
			// ── Low-res clouds: quarter-res clouds-only pre-pass (previous-frame camera so
			// it can run without re-extracting; 1-frame lag is imperceptible on soft clouds).
			const bool lowRes = GetEnvironment().lowResClouds && GetEnvironment().cloudCoverage > 0.0f;
			if (lowRes)
			{
				GLint prevFBO = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFBO);
				EnsureCloudFBO(std::max(1, pw / 2), std::max(1, ph / 2));
				glUniformMatrix4fv(m_uSkyInvVP, 1, GL_FALSE, glm::value_ptr(m_lastInvViewProj));
				glUniform3fv(m_uSkySunDir, 1, glm::value_ptr(m_lastSunDir));
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
			m_lastInvViewProj = invViewProj;   // remembered for next frame's cloud pre-pass
			m_lastSunDir      = sunDir;
		}
		GpuTimerEndPass();                 // end "Sky+Clouds"
		GpuTimerBeginPass("Transparent");  // transparency + particles + debug lines

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
			for (const TPDraw& t : transparent)
			{
				glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(t.mvp));
				glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(t.model));
				glUniform3fv(m_uColor, 1, glm::value_ptr(t.baseColor));
				glUniform1f(m_uMetallic,  t.metallic);
				glUniform1f(m_uRoughness, t.roughness);
				glUniform1f(m_uOpacity,   t.opacity);
				glUniform1i(m_uHasTexture, t.tex != 0);
				glBindVertexArray(t.vao);
				glBindTexture(GL_TEXTURE_2D, t.tex);
				glDrawElements(GL_TRIANGLES, t.indexCount, GL_UNSIGNED_INT, nullptr);
				++m_counters.draws;
				m_counters.tris += static_cast<uint32_t>(t.indexCount / 3);
			}
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
		}

		// ── GPU weather particles: simulated by transform feedback (once per
		// frame in Render), drawn here as alpha-blended billboards over the scene.
		DrawGpuParticles(viewProj, m_renderWorld.camera.position);

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
	});

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	// One-time sanity check — GL errors are silent otherwise and a broken
	// draw path would just render nothing.
	static bool s_checkedFirstFrame = false;
	if (!s_checkedFirstFrame)
	{
		s_checkedFirstFrame = true;
		const GLenum err = glGetError();
		Logger::Log(err == GL_NO_ERROR ? Logger::LogLevel::Info : Logger::LogLevel::Error,
			("OpenGLRenderer: first scene frame drew "
			 + std::to_string(m_renderWorld.objects.size()) + " object(s), glGetError=0x"
			 + std::to_string(err)).c_str());
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
		glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		DrawScene(m_viewportW, m_viewportH);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	else if (m_viewportFBO)
		DestroyViewportTarget();

	glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
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
    // Pre-distribute the pool down the fall column so it doesn't all spawn at once.
    // seed = (i+0.5)/count gives an even spread, so step(seed,coverage) yields exactly
    // a `coverage` fraction of live drops.
    const GpuParticleParams& p = m_gpuParticles;
    std::vector<float> data(static_cast<size_t>(count) * kParticleFloats);
    const float top = p.cameraPos.y + p.boxTop;
    uint32_t rng = 0x9E3779B9u;
    auto frand = [&]() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < count; ++i)
    {
        const float seed = (i + 0.5f) / static_cast<float>(count);
        const float y    = p.groundLevel + frand() * std::max(top - p.groundLevel, 1.0f);
        float* d = &data[static_cast<size_t>(i) * kParticleFloats];
        d[0] = p.cameraPos.x + (frand() * 2.0f - 1.0f) * p.boxHalf;
        d[1] = y;
        d[2] = p.cameraPos.z + (frand() * 2.0f - 1.0f) * p.boxHalf;
        d[3] = p.windVec.x * (p.isSnow ? 0.3f : 1.2f);
        d[4] = -p.fallSpeed;
        d[5] = p.windVec.z * (p.isSnow ? 0.3f : 1.2f);
        d[6] = (y - p.groundLevel) / std::max(p.fallSpeed, 0.01f); // life so it dies at the ground
        d[7] = seed;
    }
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

	const int slotIdx  = static_cast<int>(m_gpuFrameIdx % kGpuTimerRing);
	GpuTimerSlot& slot = m_gpuSlots[slotIdx];
	// This slot was last used kGpuTimerRing frames ago, so its results are certainly
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
	s.visibleObjects = m_counters.visible;
	s.totalObjects   = m_counters.total;
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
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: secondary window attached");
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
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: secondary window detached");
}

void OpenGLRenderer::RenderWindow(HE::Window* window)
{
	auto it = m_secondaryContexts.find(window->GetNativeWindow());
	if (it == m_secondaryContexts.end()) return;

	SDL_GL_MakeCurrent(window->GetNativeWindow(), static_cast<SDL_GLContext>(it->second));
	glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	// TODO: secondary-window draw calls
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
