#include "Backends/OpenGL/OpenGLRenderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <vector>
#include <Diagnostics/Logger.h>
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

// CPU port of the shader's analytic skyColor(dir,sunDir) — used to bake the
// image-based-ambient cubemap so the scene shader samples it once instead of
// re-evaluating this function twice per lit pixel. Mirrors kSkyFuncGLSL exactly.
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

// Builds the six cube faces of the image-based-ambient environment map for the
// given sun direction (face = +X,-X,+Y,-Y,+Z,-Z in GL order). Returns tightly
// packed RGBA32F, faces back to back, faceN texels each.
static std::vector<float> BuildSkyEnvCube(int faceN, const glm::vec3& sunDir)
{
	std::vector<float> px(static_cast<size_t>(faceN) * faceN * 6 * 4);
	for (int f = 0; f < 6; ++f)
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
				size_t i = ((static_cast<size_t>(f) * faceN + t) * faceN + s) * 4;
				px[i+0] = c.r; px[i+1] = c.g; px[i+2] = c.b; px[i+3] = 1.0f;
			}
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
uniform vec3      uSunDir;      // direction toward the sun (for image-based ambient)
uniform samplerCube uSkyEnv;   // baked skyColor cubemap (image-based ambient)
uniform vec3      uAmbient;     // flat ambient fill (never-black floor + overcast)
uniform float     uFogDensity;       // atmospheric fog amount (0 = off)
uniform float     uFogHeightFalloff; // >0 = fog pools near the ground
uniform sampler2D uAO;               // SSAO occlusion (screen-space); 1 = unoccluded
uniform vec2      uViewport;         // output size, for the screen-space AO lookup
uniform int       uSSAOEnabled;      // 1 = darken the ambient by SSAO

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

// Directional-light shadow map
uniform mat4      uLightVP;
uniform sampler2D uShadowMap;
uniform int       uShadowEnabled;

out vec4 FragColor;

float computeShadow(vec3 worldPos, vec3 N, vec3 L)
{
	if (uShadowEnabled == 0) return 1.0;
	vec4 lp = uLightVP * vec4(worldPos, 1.0);
	vec3 p  = lp.xyz / lp.w;
	p = p * 0.5 + 0.5;                       // NDC [-1,1] → [0,1]
	if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
		return 1.0;                          // outside the map → lit
	// Slope-scaled bias: grows toward grazing sun angles (low sun / day-night
	// sunsets) to stop shadow acne, clamped so high sun keeps crisp contact.
	float ndl     = clamp(dot(N, L), 0.0, 1.0);
	float bias    = clamp(0.0016 * tan(acos(ndl)), 0.0005, 0.02);
	// 3×3 PCF: averaging neighbouring texels softens the edge and hides the
	// per-texel flicker the hard test produced as the day-night light rotates.
	vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
	float vis = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
		{
			float c = texture(uShadowMap, p.xy + vec2(x, y) * texel).r;
			vis += (p.z - bias > c) ? 0.0 : 1.0;
		}
	vis /= 9.0;
	return mix(0.35, 1.0, vis);
}

void main()
{
	vec3 albedo = uHasTexture ? texture(uTexture, vUV).rgb * uColor : uColor;
	vec3 N      = normalize(vNormal);

	if (uLightCount == 0)
	{
		vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
		float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
		FragColor  = vec4(albedo * diff, 1.0);
		return;
	}

	// Metallic-roughness split: metals lose diffuse and tint the specular F0;
	// roughness widens + dims the Blinn-Phong highlight (cheap PBR stand-in).
	vec3  diffuseColor = albedo * (1.0 - uMetallic);
	vec3  specColor    = mix(vec3(0.04), albedo, uMetallic);
	float shininess    = mix(128.0, 8.0, uRoughness);
	float specScale    = mix(0.5, 0.03, uRoughness);

	vec3 V = normalize(uCameraPos - vWorldPos);

	// Image-based ambient from the procedural sky (replaces the flat floor):
	// diffuse from the surface normal, specular from the reflection vector
	// (bent toward the normal as roughness grows = crude prefilter).
	vec3 Rrough  = normalize(mix(reflect(-V, N), N, uRoughness));
	vec3 ambDiff = texture(uSkyEnv, N).rgb      * diffuseColor;
	vec3 ambSpec = texture(uSkyEnv, Rrough).rgb * specColor;
	vec3 ambient = ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * uRoughness);
	// Flat ambient fill (never-black floor + overcast replacement for the
	// switched-off sun/moon light), applied to the diffuse albedo.
	ambient += uAmbient * diffuseColor;
	// Screen-space ambient occlusion darkens only the ambient/indirect term in
	// crevices; the direct lighting added below is left untouched. 1.0 = fully lit.
	float ao = (uSSAOEnabled == 1) ? texture(uAO, gl_FragCoord.xy / uViewport).r : 1.0;
	vec3 result  = ambient * ao;

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

		// Only the (first) directional light casts shadows.
		float sh = (type == 0) ? computeShadow(vWorldPos, N, L) : 1.0;

		float diff = max(dot(N, L), 0.0);
		vec3  H    = normalize(L + V);
		float spec = pow(max(dot(N, H), 0.0), shininess) * specScale;
		result += (diffuseColor * diff + specColor * spec)
		        * uLightColor[i].rgb * uLightColor[i].w * atten * sh;
	}
	result = applyFog(result, uCameraPos, vWorldPos, uSunDir);
	FragColor = vec4(result, 1.0);
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
uniform float     uTimeOfDay;   // day phase 0..1 (celestial rotation)
uniform float     uCloudCoverage; // cloud amount (0 = clear … 1 = full overcast)
uniform float     uTime;        // wall-clock seconds (star twinkle)
uniform vec3      uSunColor;    // sun light colour (tints the clouds)
uniform float     uAurora;      // aurora intensity (0 = off)
uniform float     uMilkyWay;    // milky-way star-lane density/brightness
uniform float     uNebula;      // space-nebula intensity (0 = off)
uniform vec3      uNebulaColor; // space-nebula base colour
uniform vec3      uAuroraColor; // aurora base colour
uniform vec3      uWind;        // cloud drift vector (world units / s, horizontal)
uniform sampler3D uNoise;       // tiling 3D value-noise (replaces the hash fbm)
out vec4 FragColor;
//#SKYFUNC#

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

	float tex  = uHasMoonTex ? texture(uMoonTex, q * 0.5 + 0.5).r : 1.0;
	float limb = sqrt(max(1.0 - r * r, 0.0));      // spherical brightness falloff
	float edge = smoothstep(1.0, 0.90, r);         // soft anti-aliased rim
	vec3  tint = vec3(0.92, 0.94, 1.00);
	return tint * tex * limb * edge * 3.0 * night;
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
vec3 starField(vec3 dir, vec3 cdir, vec3 sunDir, float time, float milkyWay)
{
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

	// Stars cluster densely along the galactic band: the cell-occupancy threshold
	// is lowered there so the Milky Way reads as a dense lane of stars (not a
	// smear). The milky-way control drives how dense/bright the lane is. Sampled
	// in the rotating celestial frame so the whole field drifts.
	float band   = galacticBand(cdir);
	float mw     = clamp(milkyWay, 0.0, 1.0);
	float thresh = mix(0.92, mix(0.86, 0.72, mw), band);
	vec3  p       = cdir * 70.0;
	vec3  cell    = floor(p);
	float present = starHash(cell);
	if (present < thresh) return vec3(0.0);

	vec3  sp   = vec3(starHash(cell + 1.7), starHash(cell + 4.3), starHash(cell + 8.9));
	float d    = length(fract(p) - sp);
	// Per-star size class: cubic skew so most stars are tiny pinpoints, a few are
	// medium, and the rare brightest are larger with a faint halo — a realistic
	// apparent-magnitude spread instead of one fixed size.
	float sizeH  = starHash(cell + 5.7);
	float big    = sizeH * sizeH * sizeH;
	float radius = mix(0.05, 0.17, big);
	float core   = smoothstep(radius, 0.0, d);
	core *= core;                                  // tighten the core, keep a faint glow
	float halo   = smoothstep(radius * 3.0, radius, d) * (big * big) * 0.35; // only big stars
	float shape  = core + halo;
	float mag  = (0.4 + 0.6 * smoothstep(thresh, 1.0, present)) * mix(0.7, 2.7, big); // size→brightness
	// Random per-star twinkle: each star gets its own phase + frequency so the
	// field shimmers randomly in real time (wall clock, not the slow time-of-day).
	float twPhase = starHash(cell + 23.5) * 6.2831;
	float twFreq  = 2.0 + 4.0 * starHash(cell + 47.1);
	float tw      = 0.7 + 0.3 * sin(time * twFreq + twPhase);
	float horizon = smoothstep(0.0, 0.15, dir.y);  // fade into the horizon haze
	vec3  tint = mix(vec3(0.80, 0.88, 1.0), vec3(1.0, 0.93, 0.82), starHash(cell + 12.1));
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
vec3 applyClouds(vec3 baseSky, vec3 dir, vec3 sunDir, float time, float coverage, vec3 sunColor, vec3 wind)
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
	float jitter = cloudHash(dir.xz * 173.3 + vec2(dir.y * 37.1, dir.y * 19.7));

	// Day/night/dusk drive the cloud colour (independent of the drift clock).
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);
	float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));

	// Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	float T = 1.0;                                 // transmittance along the view ray
	vec3  L = vec3(0.0);                           // accumulated in-scattered colour
	for (int i = 0; i < N; ++i)
	{
		float s   = s0 + (float(i) + jitter) * ds;
		vec3  pos = dir * s;
		float dens = cloudDensity(pos, time, coverage, wind);
		if (dens > 0.001)
		{
			// Light-march toward the sun: Beer's-law self-shadowing (3 steps for a
			// smooth shadow gradient; fewer steps undersample and flicker).
			float shadow = 0.0;
			for (int j = 1; j <= 3; ++j)
				shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25), time, coverage, wind);
			float sun    = exp(-shadow * 1.7);
			float powder = 1.0 - exp(-dens * 3.0); // dark soft edges (powder effect)
			float lit    = sun * powder;

			// Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
			vec3 dayCol   = mix(vec3(0.17, 0.20, 0.29), sunColor * 1.12, lit);
			vec3 nightCol = mix(vec3(0.04, 0.05, 0.09), vec3(0.18, 0.21, 0.30), lit);
			vec3 cloudCol = mix(nightCol, dayCol, day);
			vec3 duskTop  = sunColor * vec3(1.25, 0.55, 0.28);
			cloudCol = mix(cloudCol, duskTop, dusk * lit * 0.9);
			// Forward-scatter glow: Henyey-Greenstein-weighted direct sunlight makes
			// the sun-facing edges flare gold (the silver lining), strongest when
			// looking toward the sun and where the cloud isn't self-shadowed.
			cloudCol += sunColor * (phase * sun * 0.9 * max(day, dusk));
			// Cheap vertical depth: tops catch the light (bright crown), the base
			// sits in self-shadow (darker, cooler) — fakes the volumetric
			// "cauliflower" relief from just the sample's height in the slab.
			float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
			cloudCol *= mix(0.5, 1.15, hTone);
			cloudCol += vec3(0.07, 0.10, 0.17) * ((1.0 - hTone) * day * 0.25);

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
// bleed into one another. Night/horizon gated, occluded by clouds. Mirrors Metal.
vec3 nebula(vec3 dir, vec3 cdir, vec3 sunDir, float intensity, vec3 nebColor)
{
	if (intensity <= 0.0) return vec3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.0) return vec3(0.0);

	vec3  cN   = normalize(cdir);
	const vec3 galN = normalize(vec3(0.46, 0.52, -0.72));
	float bd   = dot(cN, galN);
	float band = exp(-bd * bd * 2.3);           // wide soft milky-way bias
	vec3  P    = cN * 3.4;
	float big  = starFbm3(P * 0.7 + 11.0, 4);   // large clouds
	float med  = starFbm3(P * 1.7 + 27.0, 3);   // medium clumps
	float fine = starFbm3(P * 4.0 + 41.0, 2);   // fine mottle / embedded dust
	float blob   = smoothstep(0.46, 0.74, big * 0.5 + med * 0.6);
	// Structural character per region: dense puffy bodies vs. wispy filaments.
	float charF  = starFbm3(P * 0.4 + 150.0, 2);
	float wispy  = smoothstep(0.42, 0.70, charF);
	float fila   = smoothstep(0.55, 0.86, starFbm3(P * 5.5 + 97.0, 2));   // fine filaments
	float detail = (0.30 + 0.70 * smoothstep(0.32, 0.86, fine)) * mix(1.0, 0.65 + 0.9 * fila, wispy);
	float dust   = 1.0 - 0.5 * smoothstep(0.50, 0.88, starFbm3(P * 2.6 + 63.0, 3));
	float density = blob * detail * dust;
	float core   = smoothstep(0.62, 0.95, big * 0.55 + med * 0.55);   // bright centres
	float glow   = (band * 0.85 + 0.15) * (density + 0.6 * core);     // baseline -> off-band patches
	if (glow <= 0.0) return vec3(0.0);

	// Hue wheel across neighbouring blobs: cool blue → teal → green → gold →
	// magenta so regions differ in colour and bleed together. A large-scale field
	// biases whole regions warm (emission) vs. cool (reflection) for more variety.
	float h = clamp(starFbm3(P * 0.5 + 71.0, 3) * 1.7 - 0.35
	              + 0.25 * (starFbm3(P * 1.1 + 83.0, 2) - 0.5), 0.0, 1.0);
	float warm = smoothstep(0.40, 0.72, starFbm3(P * 0.32 + 131.0, 2));
	h = clamp(h + warm * 0.30, 0.0, 1.0);
	vec3  colA = nebColor * vec3(0.42, 0.62, 1.50);   // cool blue
	vec3  colB = nebColor * vec3(0.34, 1.42, 1.18);   // teal/cyan
	vec3  colC = nebColor * vec3(0.55, 1.42, 0.55);   // green
	vec3  colD = nebColor * vec3(1.75, 1.10, 0.40);   // gold/amber
	vec3  colE = nebColor * vec3(1.85, 0.42, 0.95);   // magenta/pink
	vec3  col  = colA;
	col = mix(col, colB, smoothstep(0.14, 0.36, h));
	col = mix(col, colC, smoothstep(0.36, 0.54, h));
	col = mix(col, colD, smoothstep(0.54, 0.72, h));
	col = mix(col, colE, smoothstep(0.72, 0.92, h));
	float horizon = smoothstep(0.0, 0.16, dir.y);
	return col * (glow * 2.1 * horizon * night * intensity);
}

// Aurora borealis — drifting light curtains, night only, intensity + colour
// user-controlled. Modelled as wavy ribbons projected onto a high curtain plane
// that run along one axis and stack along the other, so they sweep across the
// whole sky from one side to the other (not a single ring around the camera).
// Fine vertical striations + drift give the rayed, volumetric structure.
// Mirrors Metal exactly.
vec3 aurora(vec3 dir, vec3 sunDir, float time, float intensity, vec3 auroraCol)
{
	if (intensity <= 0.0) return vec3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y <= 0.04) return vec3(0.0);

	// Project onto a high curtain plane; ribbons run along x and stack along z.
	vec2  P      = dir.xz / (dir.y + 0.45);
	float along  = P.x;
	float across = P.y;
	float wave   = 0.40 * sin(along * 0.7 + time * 0.15)
	             + 0.30 * cloudFbm(vec2(along * 0.35 - time * 0.04, 3.0));
	float phase  = across * 0.5 + wave;
	float f      = abs(fract(phase) - 0.5);             // distance to the nearest ribbon
	float ribbon = smoothstep(0.22, 0.48, f);
	float stri   = cloudFbm(vec2(along * 6.0 + time * 0.25, across * 1.2));
	float curtain = ribbon * (0.45 + 0.55 * smoothstep(0.30, 0.80, stri));
	float patches = 0.55 + 0.45 * smoothstep(0.25, 0.85,
	               cloudFbm(vec2(along * 0.45 + time * 0.03, across * 0.4 + 9.0)));
	// Base colour low, shifting toward violet tips with elevation.
	float hcol   = smoothstep(0.05, 0.60, dir.y);
	vec3  topCol = auroraCol * vec3(0.55, 0.40, 1.5);
	vec3  col    = mix(auroraCol, topCol, hcol);
	float fade   = smoothstep(0.03, 0.16, dir.y) * (1.0 - smoothstep(0.78, 1.0, dir.y));
	return col * (curtain * patches * fade * intensity * night * 2.4);
}

void main()
{
	vec4 wp1 = uInvViewProj * vec4(vNDC,  1.0, 1.0);
	vec4 wp0 = uInvViewProj * vec4(vNDC, -1.0, 1.0);
	vec3 dir = wp1.xyz / wp1.w - wp0.xyz / wp0.w;
	vec3 col  = skyColor(dir, uSunDir);
	// Night-sky elements (stars/Milky Way/nebula/aurora/moon) + the celestial
	// rotation are skipped entirely by day. The branch is coherent — sunDir is a
	// uniform, so every pixel in the frame takes the same path — so it is cheap.
	float nightF = 1.0 - smoothstep(-0.10, 0.10, clamp(normalize(uSunDir).y, -0.2, 1.0));
	if (nightF > 0.0)
	{
		vec3 cdir = celestialDir(dir, uTimeOfDay);   // turns with the day-night cycle
		col += starField(dir, cdir, uSunDir, uTime, uMilkyWay);
		col += nebula(dir, cdir, uSunDir, uNebula, uNebulaColor);
		col += aurora(dir, uSunDir, uTime, uAurora, uAuroraColor);
		col += moonDisk(dir, uSunDir);
	}
	col = applyClouds(col, dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind);
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
	float sunY = clamp(sunDir.y, -0.2, 1.0);
	float day  = smoothstep(-0.10, 0.10, sunY);                 // 0 night → 1 day
	float dusk = smoothstep(-0.06, 0.05, sunY)
	           * (1.0 - smoothstep(0.05, 0.28, sunY));          // peaks near horizon

	vec3 zenithDay  = vec3(0.08, 0.28, 0.72);
	vec3 horizDay   = vec3(0.42, 0.62, 0.88);
	vec3 zenithNite = vec3(0.012, 0.016, 0.05);
	vec3 horizNite  = vec3(0.03, 0.04, 0.10);
	vec3 zenith  = mix(zenithNite, zenithDay, day);
	vec3 horizon = mix(horizNite,  horizDay,  day);

	// Directional sunset warmth: the warm band is concentrated toward the sun's
	// azimuth (golden near the sun, cooler magenta away) instead of a flat ring,
	// and the zenith picks up a touch of dusk purple for atmospheric depth.
	vec2  sunAz  = normalize(sunDir.xz + vec2(1e-5));
	float toward = dot(normalize(dir.xz + vec2(1e-5)), sunAz) * 0.5 + 0.5; // 0 away → 1 toward
	toward = pow(clamp(toward, 0.0, 1.0), 1.5);
	vec3  duskHoriz = mix(vec3(0.52, 0.30, 0.52), vec3(1.20, 0.50, 0.16), toward);
	horizon = mix(horizon, duskHoriz, dusk);                    // warm directional sunset band
	zenith  = mix(zenith,  vec3(0.20, 0.16, 0.40), dusk * 0.6);

	float h    = clamp(dir.y, 0.0, 1.0);
	float grad = pow(1.0 - h, 2.5);                             // horizon-weighted
	vec3 sky = mix(zenith, horizon, grad);

	// Concentrated golden scattering band hugging the horizon toward the sun.
	float band = pow(1.0 - h, 8.0) * toward;
	sky += vec3(1.25, 0.62, 0.26) * (band * dusk * 0.8);

	// Below the horizon: ease into a soft ground haze over a wide band so the
	// sky stays atmospheric just under the horizon line.
	vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));

	// Layered sun aureole — a crisp disk plus tight/mid blooms and a broad warm
	// scatter that survive through sunset for a cinematic, volumetric glow.
	vec3  sunTint = mix(vec3(1.0, 0.42, 0.20), vec3(1.0, 0.96, 0.88),
	                    smoothstep(0.0, 0.25, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	sky += sunTint * (pow(s, 1800.0) * 14.0) * day;             // crisp disk (blooms)
	sky += sunTint * (pow(s, 180.0)  * 2.2) * sunVis;           // tight bloom
	sky += sunTint * (pow(s, 22.0)   * 0.7) * sunVis;           // mid aureole
	sky += vec3(1.0, 0.5, 0.25) * (pow(s, 5.0) * 0.5) * dusk;   // broad warm scatter

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
	sky += vec3(0.04, 0.05, 0.08) * night;                      // faint moonlit fill
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
// For each pixel: reconstruct the view-space normal from neighbouring positions,
// build a TBN from a tiled random rotation, then sample a hemisphere kernel and
// count how many samples are occluded by nearer geometry. Mirrors the Metal pass
// exactly except for the NDC→UV y-flip (GL framebuffers are bottom-up).
static const char* kSSAOFS = R"GLSL(
#version 410 core
in vec2 vUV;
uniform sampler2D uViewPos;     // RGBA16F: xyz view-space pos, a = valid
uniform sampler2D uNoise;       // 4×4 random rotation vectors (xy in [-1,1])
uniform mat4      uProj;        // camera projection (GL convention)
uniform vec2      uNoiseScale;  // viewport / 4 (tiles the noise across the screen)
uniform float     uRadius;
uniform float     uBias;
uniform float     uIntensity;
uniform vec3      uKernel[32];
out vec4 FragColor;
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

	vec3 randv = texture(uNoise, vUV * uNoiseScale).xyz;
	vec3 T = normalize(randv - N * dot(randv, N)); // Gram-Schmidt
	vec3 B = cross(N, T);
	mat3 TBN = mat3(T, B, N);

	float occ = 0.0;
	for (int i = 0; i < 32; ++i)
	{
		vec3 sp = P + (TBN * uKernel[i]) * uRadius;       // view-space sample point
		vec4 clip = uProj * vec4(sp, 1.0);
		vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;        // GL: ndc.y up → uv.y up
		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
		vec4 sv = texture(uViewPos, suv);
		if (sv.a < 0.5) continue;                          // sampled the background
		// View space looks down -Z, so geometry nearer the camera has the larger z.
		float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sv.z), 1e-4));
		occ += ((sv.z >= sp.z + uBias) ? 1.0 : 0.0) * rangeCheck;
	}
	float ao = 1.0 - (occ / 32.0) * uIntensity;
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
	CreateShadowResources();
	CreateSkyPipeline();
	CreateTonemapPipeline();
	CreateBloomPipeline();
	CreateSSAOPipeline();
	CreateCubeMesh();
	Logger::Log(Logger::LogLevel::Info, "OpenGLRenderer: initialized successfully");
}

static constexpr int kSkyEnvFace = 128; // image-based-ambient cubemap face size

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
	m_uLightVP       = glGetUniformLocation(m_unlitProgram, "uLightVP");
	m_uShadowMap     = glGetUniformLocation(m_unlitProgram, "uShadowMap");
	m_uShadowEnabled = glGetUniformLocation(m_unlitProgram, "uShadowEnabled");
	m_uAO            = glGetUniformLocation(m_unlitProgram, "uAO");
	m_uViewport      = glGetUniformLocation(m_unlitProgram, "uViewport");
	m_uSSAOEnabled   = glGetUniformLocation(m_unlitProgram, "uSSAOEnabled");
}

void OpenGLRenderer::UpdateSkyEnvCube(const glm::vec3& sunDir)
{
	// The baked sky only changes with the sun direction — skip the CPU rebuild +
	// upload when it hasn't moved.
	if (m_skyEnvValid && glm::distance(sunDir, m_skyEnvSunDir) < 1e-4f)
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

	// Depth texture + FBO. Border color 1.0 so samples outside the map read as
	// "fully lit" (depth 1).
	glGenTextures(1, &m_shadowDepthTex);
	glBindTexture(GL_TEXTURE_2D, m_shadowDepthTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize,
	             0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

	glGenFramebuffers(1, &m_shadowFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowDepthTex, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindTexture(GL_TEXTURE_2D, 0);
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
	m_uSkyTime    = glGetUniformLocation(m_skyProgram, "uTimeOfDay");
	m_uSkyCoverage = glGetUniformLocation(m_skyProgram, "uCloudCoverage");
	m_uSkyClock    = glGetUniformLocation(m_skyProgram, "uTime");
	m_uSkySunColor = glGetUniformLocation(m_skyProgram, "uSunColor");
	m_uSkyAurora   = glGetUniformLocation(m_skyProgram, "uAurora");
	m_uSkyMilkyWay    = glGetUniformLocation(m_skyProgram, "uMilkyWay");
	m_uSkyNebula      = glGetUniformLocation(m_skyProgram, "uNebula");
	m_uSkyNebulaColor = glGetUniformLocation(m_skyProgram, "uNebulaColor");
	m_uSkyAuroraColor = glGetUniformLocation(m_skyProgram, "uAuroraColor");
	m_uSkyWind        = glGetUniformLocation(m_skyProgram, "uWind");
	m_uSkyNoise       = glGetUniformLocation(m_skyProgram, "uNoise");

	// Procedural 3D noise volume the sky's starFbm3/worleyFbm sample (clouds +
	// nebula) — built once on the CPU. RG16 (R=value noise, G=Worley billows) +
	// LINEAR + REPEAT so it tiles seamlessly.
	constexpr int kNoiseN = 256;   // large tile so the sky's offset/octave coords fit
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
		glUniformMatrix4fv(m_uPosMVP,       1, GL_FALSE, glm::value_ptr(viewProj * dc.transform));
		glUniformMatrix4fv(m_uPosModelView, 1, GL_FALSE, glm::value_ptr(view * dc.transform));
		if (!valid || dc.meshAssetId != lastId)
		{
			cMesh = ResolveMesh(dc.meshAssetId);
			lastId = dc.meshAssetId; valid = true;
		}
		const GpuMesh* mesh = cMesh;
		glBindVertexArray(mesh ? mesh->vao : m_cubeVAO);
		glDrawElements(GL_TRIANGLES, mesh ? mesh->indexCount : m_cubeIndexCount,
		               GL_UNSIGNED_INT, nullptr);
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

void OpenGLRenderer::CreateCubeMesh()
{
	// Unit cube, 24 vertices (position + normal per face)
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

	glGenVertexArrays(1, &m_cubeVAO);
	glGenBuffers(1, &m_cubeVBO);
	glGenBuffers(1, &m_cubeEBO);

	glBindVertexArray(m_cubeVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_cubeEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glBindVertexArray(0);
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

void OpenGLRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
	// Defer the actual glDelete to DrawScene, where the GL context is current.
	if (materialId != HE::UUID{})
		m_pendingMaterialInvalidations.push_back(materialId);
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
	DestroyLdrTarget();
	DestroySSAOTargets();
	for (auto& r : m_retiredTextures)
		glDeleteTextures(1, &r.texture);
	m_retiredTextures.clear();

	if (m_cubeVAO)      { glDeleteVertexArrays(1, &m_cubeVAO); m_cubeVAO = 0; }
	if (m_cubeVBO)      { glDeleteBuffers(1, &m_cubeVBO);      m_cubeVBO = 0; }
	if (m_cubeEBO)      { glDeleteBuffers(1, &m_cubeEBO);      m_cubeEBO = 0; }
	if (m_unlitProgram) { glDeleteProgram(m_unlitProgram);     m_unlitProgram = 0; }
	if (m_depthProgram) { glDeleteProgram(m_depthProgram);     m_depthProgram = 0; }
	if (m_skyProgram)   { glDeleteProgram(m_skyProgram);       m_skyProgram = 0; }
	if (m_tonemapProgram) { glDeleteProgram(m_tonemapProgram); m_tonemapProgram = 0; }
	if (m_fxaaProgram)    { glDeleteProgram(m_fxaaProgram);    m_fxaaProgram = 0; }
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

	const IRenderer::EnvironmentSettings& env = GetEnvironment();
	m_extractor.setDayNight(env.dayNightCycle, env.timeOfDay,
	                        env.sunColor, env.sunIntensity,
	                        env.moonColor, env.moonIntensity,
	                        env.cloudCoverage);
	m_extractor.extract(*m_world, m_renderWorld,
	                    static_cast<float>(pw) / static_cast<float>(ph),
	                    &m_editorCamera);
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

	// Snapshot the active target (window or editor-viewport FBO) so the shadow
	// pass can render into the shadow map and then restore it for the main pass.
	GLint prevFBO = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

	const bool      shadows = m_renderWorld.shadow.enabled && m_shadowFBO != 0;
	const glm::mat4 lightVP = m_renderWorld.shadow.viewProj;

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
		// ── Shadow pass: depth from the light's POV into the shadow map ──────
		if (io.output.id == kShadowMapTarget)
		{
			if (!shadows) return;
			glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
			glViewport(0, 0, m_shadowSize, m_shadowSize);
			glClear(GL_DEPTH_BUFFER_BIT);
			glUseProgram(m_depthProgram);
			HE::UUID shMeshId{}; const GpuMesh* shMesh = nullptr; bool shMeshValid = false;
			for (const DrawCall& dc : cmds.drawCalls())
			{
				glUniformMatrix4fv(m_uDepthMVP, 1, GL_FALSE, glm::value_ptr(lightVP * dc.transform));
				if (!shMeshValid || dc.meshAssetId != shMeshId)
				{
					shMesh      = ResolveMesh(dc.meshAssetId);
					shMeshId    = dc.meshAssetId; shMeshValid = true;
				}
				const GpuMesh* mesh = shMesh;
				glBindVertexArray(mesh ? mesh->vao : m_cubeVAO);
				glDrawElements(GL_TRIANGLES, mesh ? mesh->indexCount : m_cubeIndexCount,
				               GL_UNSIGNED_INT, nullptr);
			}
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
			const unsigned int bloomTex = m_bloomEnabled ? RenderBloom(pw, ph) : 0u;

			// Tonemap HDR scene color + bloom into the LDR intermediate (FXAA reads it).
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

			// FXAA the tonemapped LDR image into the actual output.
			glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
			glViewport(0, 0, pw, ph);
			glUseProgram(m_fxaaProgram);
			glBindTexture(GL_TEXTURE_2D, m_ldrColor);
			glUniform1i(m_uFxaaScene, 0);
			glUniform2f(m_uFxaaRcpFrame, 1.0f / float(pw), 1.0f / float(ph));
			glDrawArrays(GL_TRIANGLES, 0, 3);

			glEnable(GL_DEPTH_TEST);
			return;
		}

		// ── SSAO: view-space position pre-pass → occlusion → blur ───────────
		// Computed before shading (using these same geometry draw calls) so the
		// scene shader can darken its ambient term. Skipped (zero cost) when off.
		const unsigned int aoTex = m_ssaoEnabled
			? RenderSSAO(cmds, pw, ph, viewProj, m_renderWorld.camera.view,
			             m_renderWorld.camera.projection)
			: 0u;

		// ── Geometry pass: scene program + per-frame state, into HDR target ─
		// Set here (not before the graph) because the shadow pass switched the
		// active program.
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

		// Shadow map bound on texture unit 1.
		glUniform1i(m_uShadowEnabled, shadows ? 1 : 0);
		glUniformMatrix4fv(m_uLightVP, 1, GL_FALSE, glm::value_ptr(lightVP));
		glUniform1i(m_uShadowMap, 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, shadows ? m_shadowDepthTex : 0);
		glActiveTexture(GL_TEXTURE0); // base color binds here in the loop

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

		for (const DrawCall& dc : cmds.drawCalls())
		{
			const glm::mat4 mvp = viewProj * dc.transform;
			glUniformMatrix4fv(m_uMVP,   1, GL_FALSE, glm::value_ptr(mvp));
			glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(dc.transform));

			// An explicit MaterialComponent override wins over the mesh's own
			// base-color texture; otherwise fall back to the mesh's (or none).
			// PBR scalars come from the material override; defaults otherwise.
			if (!matValid || dc.materialAssetId != lastMatId)
			{
				cHasOverride = ResolveMaterialTexture(dc.materialAssetId, cOverrideTex);
				cBaseColor   = glm::vec3(1.0f); cMetallic = 0.0f; cRoughness = 0.5f;
				cHasMat      = ResolveMaterialParams(dc.materialAssetId, cBaseColor, cMetallic, cRoughness);
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
				baseColor = (tex != 0) ? glm::vec3(1.0f) : glm::vec3(0.85f, 0.55f, 0.25f);
			glUniform3fv(m_uColor, 1, glm::value_ptr(baseColor));
			glUniform1f(m_uMetallic,  cMetallic);
			glUniform1f(m_uRoughness, cRoughness);

			glBindVertexArray(mesh ? mesh->vao : m_cubeVAO);
			glUniform1i(m_uHasTexture, tex != 0);
			glBindTexture(GL_TEXTURE_2D, tex);
			glDrawElements(GL_TRIANGLES, mesh ? mesh->indexCount : m_cubeIndexCount,
			               GL_UNSIGNED_INT, nullptr);
		}

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
			glUniform1f(m_uSkyTime, GetEnvironment().timeOfDay);
			glUniform1f(m_uSkyCoverage, GetEnvironment().cloudCoverage);
			glUniform1f(m_uSkyClock, static_cast<float>(SDL_GetTicks()) / 1000.0f);
			glUniform3fv(m_uSkySunColor, 1, glm::value_ptr(GetEnvironment().sunColor));
			glUniform1f(m_uSkyAurora, GetEnvironment().auroraIntensity);
			glUniform1f(m_uSkyMilkyWay, GetEnvironment().milkyWayIntensity);
			glUniform1f(m_uSkyNebula, GetEnvironment().nebulaIntensity);
			glUniform3fv(m_uSkyNebulaColor, 1, glm::value_ptr(GetEnvironment().nebulaColor));
			glUniform3fv(m_uSkyAuroraColor, 1, glm::value_ptr(GetEnvironment().auroraColor));
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
			glActiveTexture(GL_TEXTURE0);
			glBindVertexArray(m_fsVAO);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glDepthFunc(GL_LESS);   // restore default for the next pass
			glDepthMask(GL_TRUE);
		}
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

	AgeRetiredTextures();

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
}

IRenderer::Capabilities OpenGLRenderer::GetCapabilities() const
{
	return { true, true, true };
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
