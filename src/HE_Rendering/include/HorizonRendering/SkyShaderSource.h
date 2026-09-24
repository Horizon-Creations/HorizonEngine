#pragma once
#include <cstdint>
#include <string>

// ─── The procedural sky shader: one source, four backends ────────────────────
// kSkyFS + kSkyFuncGLSL below are THE sky: the single-scattering atmosphere,
// stars and Milky Way, the three-colour nebula, 3D aurora, the phased moon,
// meteors, cirrus, contrails, rainbow, sky-dome and 3D volumetric clouds
// (Classic and Realistic) and god rays.
//
//   OpenGL          compiles kSkyFS as written: GLSL 4.10, loose uniforms, the
//                   //#SKYFUNC# marker replaced by kSkyFuncGLSL (injectSkyFunc).
//   Vulkan, D3D11,  compile THE SAME TEXT through BuildSkyFragmentGLSL450():
//   D3D12           kSkyVulkanPrelude replaces the GL declaration block (all of
//                   kSkyFS above the marker) with a Vulkan-GLSL header whose
//                   uniform block IS HE::SkyFrameParams, and #defines every GL
//                   uniform name onto its field, so the body compiles unchanged.
//                   he::shaderc turns it into SPIR-V (Vulkan) or HLSL SM 5.0 via
//                   SPIRV-Cross (D3D11/D3D12). Without shaderc those three fall
//                   back to their older hand-written sky shaders.
//   Metal           its own MSL transliteration (kSkyMSL in MetalRenderer.mm).
//
// Editing rules:
//   * A new uniform in kSkyFS needs a SkyFrameParams slot (BuildSkyFrameParams),
//     a #define in kSkyVulkanPrelude and the glUniform* upload in
//     OpenGLRenderer::DrawSkyFullscreen. tests/test_sky_shader.cpp fails for a
//     kSkyFS uniform the prelude does not map.
//   * Everything below the //#SKYFUNC# marker must stay valid in BOTH GLSL 4.10
//     and Vulkan GLSL 4.50: no layout qualifiers, no GL-only built-ins.
//   * The quarter-res cloud pre-pass (uCloudPrepass / uLowResClouds) and the
//     cloud-shadow map (uCloudShadowPass) are GL/Metal passes; the prelude pins
//     them to 0, so the Vulkan/D3D sky always raymarches its clouds inline.
namespace HE::glsl
{

inline constexpr const char* kSkyFS = R"GLSL(
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
uniform vec3      uNebulaColor; // space-nebula colour 1 (interior synchrotron veil)
uniform vec3      uNebulaColor2; // space-nebula colour 2 (filament cage, gold/amber)
uniform vec3      uNebulaColor3; // space-nebula colour 3 (filament cage, rust/red)
uniform float     uNebulaSeed;  // space-nebula randomisation seed
uniform float     uNebulaHiFi;  // nebula quality: 0 Performance / 1 High / 2 Max
uniform float     uNebulaCover; // nebula sky coverage: 0 none .. 1 nearly the whole band
uniform vec3      uAuroraColor;    // aurora lower/base colour (e.g. green)
uniform vec3      uAuroraColorTop; // aurora upper colour (e.g. purple)
uniform vec3      uWind;        // cloud drift vector (world units / s, horizontal)
uniform sampler3D uNoise;       // tiling 3D value-noise (replaces the hash fbm)
uniform sampler2D uCloudTex;    // quarter-res cloud buffer (rgb = L, a = T) for low-res clouds
uniform float     uLowResClouds; // 1 = composite uCloudTex instead of the inline raymarch
uniform float     uCloudPrepass; // 1 = this draw outputs (L, T) only (the quarter-res cloud pass)
uniform float     uRainAmount;   // rain (0..1) → rainbow arc when the sun is up + low
uniform float     uGodRays;      // crepuscular sun-shaft strength (0 = off)
uniform float     uShootingStars; // meteor frequency (0 = none) — night only
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
uniform float     uCloudShadowPass;   // 1 = this draw renders the cloud-shadow transmittance map
uniform vec4      uCloudShadowRegion; // xy = region origin (world XZ), z = region size, w = map size (px)
uniform int       uCloudStyle;        // 3D clouds: 0 = Classic, 1 = Realistic (HZD-style shapes/lighting)
uniform int       uCloudInterShadows; // 1 = extended sun march (towers darken clouds behind them)
uniform float     uCloudEvolution;    // shape-evolution speed (0 frozen … 1 natural … 2 time-lapse)
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

// Shooting stars / meteors. A few independent "slots" each spawn a meteor once per cycle;
// the meteor is a thin bright streak (small sharp head + tapering tail) that arcs across the
// upper sky and fades over its short life. Deterministic from the sky clock so it animates
// smoothly and reproduces in headless captures. Night-only. rate (0..1) scales frequency +
// concurrency. Mirrors the Metal shootingStars().
vec3 shootingStars(vec3 dir, vec3 sunDir, float time, float rate)
{
	if (rate <= 0.0) return vec3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.3, 1.0));
	if (night <= 0.0 || dir.y <= 0.05) return vec3(0.0);

	float r      = clamp(rate, 0.0, 1.0);
	int   slots  = 1 + int(r * 3.0);                  // 1..4 concurrent meteor slots
	float period = mix(9.0, 3.5, r);                  // seconds between meteors per slot
	// ONE shared RADIANT per "shower" (re-rolled every few minutes): all meteors
	// stream away from the same sky point — near-parallel trails far from it,
	// gently diverging around it — instead of criss-crossing at random.
	float shower = floor(time / 700.0);
	float azR = starHash(vec3(shower + 0.5, 4.2, 9.1)) * 6.2831853;
	float elR = 0.45 + 0.75 * starHash(vec3(shower + 0.5, 2.8, 5.5));
	vec3  R   = normalize(vec3(cos(azR) * cos(elR), sin(elR), sin(azR) * cos(elR)));
	vec3  Ru  = normalize(cross(vec3(0.0, 1.0, 0.0), R));   // tangent basis at the radiant
	vec3  Rv  = cross(R, Ru);
	vec3  col    = vec3(0.0);
	for (int k = 0; k < slots; ++k)
	{
		float tk  = time / period + float(k) * 1.37;
		float idx = floor(tk);
		float ph  = fract(tk);
		float dur = 0.16;                             // visible fraction of the cycle
		if (ph > dur) continue;
		float t = ph / dur;                           // 0..1 along the streak's life

		vec3  seed = vec3(idx * 1.7 + 0.3, float(k) * 7.3 + 1.1, idx * 0.31 + float(k) * 3.9);
		// Spawn at a random bearing/distance AROUND the radiant, then travel
		// along the great circle AWAY from it (real shower geometry).
		float phiS = starHash(seed) * 6.2831853;
		float dst  = 0.35 + 0.75 * starHash(seed + 2.1);  // angular distance from the radiant
		// Meteor size follows the star settings: uStarSize scales width/head,
		// uStarSizeVar spreads individual meteors between small and large.
		float mSz  = clamp(uStarSize, 0.25, 3.0)
		           * mix(1.0, mix(0.6, 1.8, starHash(seed + 7.7)), clamp(uStarSizeVar, 0.0, 1.0));
		vec3  p0   = normalize(R * cos(dst) + (Ru * cos(phiS) + Rv * sin(phiS)) * sin(dst));
		vec3  tdir = normalize(p0 * dot(R, p0) - R);      // tangent pointing away from the radiant
		// Tiny per-meteor tilt so the trails aren't machine-parallel.
		tdir = normalize(tdir + cross(p0, tdir) * ((starHash(seed + 5.7) - 0.5) * 0.12));
		float arc  = 0.5 + 0.4 * starHash(seed + 9.9); // angular travel over the life
		vec3  head = normalize(p0 + tdir * (t * arc));
		vec3  tail = normalize(p0 + tdir * (t * arc - 0.30)); // tail end trailing behind the head

		// Closest point on the head→tail chord (small-arc approximation in direction space).
		vec3  seg = tail - head;
		float s   = clamp(dot(dir - head, seg) / max(dot(seg, seg), 1e-5), 0.0, 1.0);
		float dd  = length(dir - (head + seg * s));
		float w      = mix(0.0045, 0.0014, s) * mSz;                 // taper: wider at head, thin at tail
		float streak = exp(-(dd * dd) / (w * w)) * pow(1.0 - s, 1.6); // brightest at the head
		float dh     = length(dir - head);
		float headG  = exp(-(dh * dh) / (0.00006 * mSz * mSz));      // small sharp head (≈0.45° at size 1)
		float life   = smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.55, 1.0, t));
		// Colour + brightness follow the star settings (same knobs as starField).
		vec3  mcol   = vec3(0.78, 0.88, 1.0) * uStarColor;           // cool blue-white meteor, user-tinted
		col += mcol * ((streak * 1.7 + headG * 1.1) * life * uStarBright);
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
			// Moonlit crown. Was nearly twenty times the night sky's own radiance,
			// which is what made night clouds read as a lit overcast floating over
			// a black sky; a real moonlit cloud is a few times the sky, not twenty.
			vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.13, 0.15, 0.24), lit);
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
const float kCloudRefAltitude = 200.0;
const float kCloudElevFloor   = 0.06;   // was clamp((cloudH-50)/2500) — grew with altitude

// ── Cloud slab intersection ──────────────────────────────────────────────────
// Entry/exit distance of a view ray through the cloud deck, for ANY camera
// position: below it, inside it (the march then starts at the camera) and above
// it looking down. The deck sits at an ABSOLUTE world altitude — that is what
// makes climbing above it possible at all; while it hung at camera.y +
// cloudHeight it rose with the viewer and could never be reached. Returns false
// when the ray never meets the slab in front of the camera. Mirror of the Metal
// cloudSlabRange — change one, change both.
bool cloudSlabRange(vec3 camPos, vec3 dir, float baseY, float topY, float maxDist,
                    out float tNear, out float tFar)
{
	if (abs(dir.y) < 1e-4)
	{
		// Horizontal ray: inside the deck → march through it; else no hit.
		if (camPos.y < baseY || camPos.y > topY) { tNear = 0.0; tFar = 0.0; return false; }
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
// and cloud self-shading always match the shapes overhead. Mirrors the Metal
// cloudFieldDensity EXACTLY — change one, change both.
//   style 0 (Classic): the original formula — rounded fBm blobs, soft base.
//   style 1 (Realistic, HZD-style): column-wise low-frequency coverage under a
//     flat condensation level, Perlin-Worley clustering, wind shear, convective
//     boil, a slow formation field (clouds grow/tower/dissolve as they drift)
//     and remap-style Worley erosion for the cauliflower silhouette.
// (The classic VIEW march below stays untouched/byte-identical — it does not
// call this; only the classic shadow map shares the classic branch here.)
float cloudFieldDensity(vec3 pos, float baseY, float thick, float nscale, float lo,
                        float time, vec3 wind, float fluff, float style, float evo)
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
		vec3 npc = vec3(pos.x, 0.0, pos.z) * (nscale * 0.55) + wind * time;
		npc.y += 37.0; // fixed weather-map slice (wind is horizontal; evolution scrolls y at sample time)
		// DOMAIN WARP: bend the column field with a low-frequency vector noise
		// so cells stop reading as same-sized round balls — outlines become
		// irregular, stretched, organic.
		npc.x += (starNoise3(npc * 0.35 + 17.0) - 0.5) * 1.7;
		npc.z += (starNoise3(npc * 0.35 + 71.0) - 0.5) * 1.7;
		// farW = 0 → only the two broad octaves decide WHERE clouds are;
		// renormalized by the dropped octaves' amplitude (0.75) so the coverage
		// threshold keeps the same meaning as the 4-octave classic field.
		float cover = cloudCoverFbm(npc + vec3(0.0, time * 0.02 * evo, 0.0), 0.0) * (1.0 / 0.75);
		// Extra MACRO octave: very-low-frequency variation so formations differ
		// in size — lone puffs next to long banks, not a uniform sprinkle.
		float macro = starNoise3(npc * 0.20 + 53.0);
		cover += (macro - 0.5) * 0.26;
		// Perlin-Worley base: the low-frequency Worley field CLUSTERS the
		// coverage around its cell centres (zero-mean, mild — a strong weight
		// here is what made every cloud the same round ball).
		float w = worleyNoise3(npc * 0.75);
		cover += (w - 0.5) * 0.16;
		// Slow formation field: local coverage breathes over time (zero-mean).
		float form = starNoise3(npc * 0.22 + vec3(time * 0.013 * evo, 31.0, -time * 0.009 * evo));
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
		// 3D body: rounded interior lumps inside the column envelope.
		vec3 np    = pos * nscale + wind * time;
		float body  = cloudCoverFbm(np + vec3(0.0, time * 0.02 * evo, 0.0), 1.0);
		float bodyD = smoothstep(0.30, 0.60, body + pres * 0.10);
		float env   = pres * rise * crown * bodyD;             // smooth envelope 0..1
		if (env <= 0.0) return 0.0;
		// HZD-style REMAP erosion: the Worley billow carves the envelope from
		// the OUTSIDE — a thin shell breaks into rounded lobes, the deep core
		// survives (a plain multiply only dims, it never re-shapes).
		// TWO-SCALE carve: the coarse billow shapes the big lobes, a second
		// higher-frequency Worley octave cuts V-shaped creases along its cell
		// borders — the angular edges and corners real cumulus have, instead
		// of one smooth rounded shell.
		float billow = cloudBillowFbm(np * 1.2 + vec3(0.0, -time * 0.08 * evo, 0.0), 1.0) * 0.62
		             + worleyNoise3(np * 2.4 + vec3(3.0, -time * 0.06 * evo, 0.0)) * 0.38;
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
	vec3 np = pos * nscale + wind * time;
	float cover = cloudCoverFbm(np + vec3(0.0, time * 0.03, 0.0), 1.0);
	float pres  = smoothstep(lo, lo + 0.26, cover);
	if (pres <= 0.0) return 0.0;
	float towerTop = mix(0.32, 1.0, smoothstep(lo, lo + 0.30, cover));
	float rise     = smoothstep(0.0, 0.18, hf);
	rise *= rise;
	float crown    = 1.0 - smoothstep(towerTop * 0.55, towerTop, hf);
	float vshape   = rise * crown;
	if (vshape <= 0.0) return 0.0;
	float billow = cloudBillowFbm(np * 1.2 + vec3(time * 0.03, 0.0, 0.0), 1.0);
	float erLo   = mix(0.30, 0.14, fluff);
	float erBite = mix(0.30, 0.62, fluff);
	float erode  = mix(1.0, smoothstep(erLo, erLo + erBite, billow),
	                   mix(0.45, 1.0, hf) * (0.55 + 0.45 * fluff));
	return pres * vshape * erode;
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
	float thick = kCloudRefAltitude * 1.5;                   // TALL slab so cumuli can billow upward (3D)
	float baseY = cloudH;                         // ABSOLUTE world altitude of the deck
	float maxDist = cloudH * 60.0;                // fade clouds beyond this (∝ altitude)
	float tNear, tFar;
	if (!cloudSlabRange(camPos, dir, baseY, baseY + thick, maxDist, tNear, tFar))
		return baseSky;

	// Step count grows with how much slab the ray crosses (much more near the horizon)
	// so the world-space sample spacing stays roughly constant — undersampling near the
	// horizon is what speckles/"pixelates" the distant clouds.
	int   N  = int(clamp((tFar - tNear) / (thick * qStepF), qMinN, qMaxN));
	float ds = (tFar - tNear) / float(N);
	// Interleaved-gradient jitter (blue-noise-like) instead of white noise → the residual
	// undersampling shows as fine filterable dither, not coarse speckle/grain.
	float jitter = skyIgn(gl_FragCoord.xy);

	// Same windows skyColor() uses, so the clouds and the sky behind them enter and
	// leave sunset together instead of the clouds snapping to moonlit blue while
	// the sky is still glowing.
	float sunY  = clamp(sunDir.y, -0.3, 1.0);
	float day   = smoothstep(-0.10, 0.10, sunY);
	float dusk  = smoothstep(-0.14, 0.04, sunY) * (1.0 - smoothstep(0.04, 0.26, sunY));
	float costh = max(dot(dir, sunDir), 0.0);
	float phase = mix(hgPhase(costh, 0.6), hgPhase(costh, -0.3), 0.25);

	// FULL inverse compensation so the clouds' apparent SIZE & SHAPE stay EXACTLY the
	// same at any height — the height slider must not alter the clouds themselves, only
	// where the layer sits (the band's elevation, applied via elevFloor below).
	// (1.6/cloudH = 0.008 at the reference height 200, matching the canonical look.)
	float nscale = 1.6 / kCloudRefAltitude;
	// Cloud-band elevation floor: raising the height lifts the band higher in the sky
	// (clear sky opens up toward the horizon); lowering it brings clouds down to the
	// horizon. This is the ONLY thing the height changes about the look — the cloud
	// bodies are identical. Mapped from the slider's ~20..2000 range.
	float elevFloor = kCloudElevFloor;
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
			// Moonlit crown. Was nearly twenty times the night sky's own radiance,
			// which is what made night clouds read as a lit overcast floating over
			// a black sky; a real moonlit cloud is a few times the sky, not twenty.
			vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.13, 0.15, 0.24), lit);
			vec3 cloudCol = mix(nightCol, dayCol, day);
			vec3 duskTop  = sunColor * vec3(1.5, 0.85, 0.42);
			// Even shaded cloud bodies pick up sunset warmth (0.35 floor), lit faces more —
			// so the whole cloud glows golden/orange at dawn & dusk, not just the rim.
			cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
			// Twilight fill: with the sun down, what lights a cloud IS the twilight
			// sky around it. Taking it from baseSky rather than a constant ties the
			// two together — the clouds cannot stay lit once the sky has gone out.
			// (3D path only: the dome path's low-res pre-pass calls it with
			// baseSky = 0, so the same term there would not survive the round trip.)
			cloudCol += baseSky * ((1.0 - day) * (0.30 + 0.50 * lit));
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
	// Grazing-angle fade, symmetric in |dir.y| (see the Metal note): works the
	// same looking down from above the deck as looking up from below.
	float horizon = smoothstep(elevFloor, elevFloor + 0.14, abs(dir.y));
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// ── Realistic 3D clouds (uCloudStyle == 1) ───────────────────────────────────
// Same slab geometry as applyClouds3D, but shape + lighting rebuilt from the
// HZD/Nubis playbook against reference photos. Mirrors the Metal
// applyClouds3DReal EXACTLY — change one, change both:
//  * shapes/evolution from cloudFieldDensity + a fine upward-boiling erosion
//    octave for the crisp cauliflower silhouette,
//  * sun march over the SAME density field with exponentially growing steps
//    (uCloudInterShadows extends the reach so towers darken clouds behind),
//  * Wrenninge-style normalized multi-scatter (3 octaves),
//  * blue-grey bellies, near-white sunlit tops, silver lining near the sun.
vec3 applyClouds3DReal(vec3 baseSky, vec3 dir, vec3 camPos, vec3 sunDir, float time,
                       float coverage, vec3 sunColor, vec3 wind, float cloudH, out float outT)
{
	outT = 1.0;
	if (coverage <= 0.0) return baseSky;
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	// Slightly higher minimum step count than classic: the sharper silhouettes
	// show the IGN dither earlier than the soft classic bodies do.
	float qStepF  = (uCloudQuality <= 0) ? 0.40 : (uCloudQuality == 1 ? 0.28 : 0.20);
	float qMinN   = (uCloudQuality <= 0) ? 14.0 : (uCloudQuality == 1 ? 26.0 : 32.0);
	float qMaxN   = (uCloudQuality <= 0) ? 44.0 : (uCloudQuality == 1 ? 80.0 : 128.0);
	int   qShadow = (uCloudQuality <= 0) ? 3    : (uCloudQuality == 1 ? 4    : 6);
	if (uCloudInterShadows == 0) qShadow = min(qShadow, 3); // short march: own body only
	float evo = uCloudEvolution;

	cloudH      = max(cloudH, 1.0);
	float thick = kCloudRefAltitude * 1.5;
	float baseY = cloudH;                         // ABSOLUTE world altitude of the deck
	float maxDist = cloudH * 60.0;
	float tNear, tFar;
	if (!cloudSlabRange(camPos, dir, baseY, baseY + thick, maxDist, tNear, tFar))
		return baseSky;

	int   N  = int(clamp((tFar - tNear) / (thick * qStepF), qMinN, qMaxN));
	float ds = (tFar - tNear) / float(N);
	float jitter = skyIgn(gl_FragCoord.xy);

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
	float fluff     = clamp(uCloudFluffiness, 0.0, 1.0);
	float densMul   = clamp(uCloudDensity, 0.0, 3.0);
	float lo        = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0));

	float T = 1.0;
	vec3  L = vec3(0.0);
	for (int i = 0; i < N; ++i)
	{
		float t   = tNear + (float(i) + jitter) * ds;
		vec3  pos = camPos + dir * t;
		float hf  = clamp((pos.y - baseY) / thick, 0.0, 1.0);
		float detailFade = 1.0 - smoothstep(maxDist * 0.10, maxDist * 0.40, t);
		float dens = cloudFieldDensity(pos, baseY, thick, nscale, lo, time, wind,
		                               fluff, 1.0, evo);
		if (dens <= 0.002) continue;
		// Fine cauliflower octave, boiling upward with the convection. Bites
		// hardest at the silhouette (low density) so the outline breaks into
		// crisp lobes while the core stays solid.
		vec3  np    = pos * nscale + wind * time;
		float bfine = worleyNoise3(np * 2.6 + vec3(0.0, -time * 0.10 * evo, 5.0));
		float fineW = (0.50 + 0.35 * fluff) * detailFade
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
			float st = thick * 0.05;
			vec3  sp = pos;
			for (int j = 0; j < qShadow; ++j)
			{
				sp += sunDir * st;
				float shf = (sp.y - baseY) / thick;
				if (shf > 1.35) break;                 // left the slab upward
				od += cloudFieldDensity(sp, baseY, thick, nscale, lo, time, wind,
				                        fluff, 1.0, evo) * (st / thick) * 10.0;
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
		vec3 dayCol   = mix(vec3(0.30, 0.35, 0.46), sunColor * 1.30, lit);
		vec3 nightCol = mix(vec3(0.015, 0.018, 0.035), vec3(0.13, 0.15, 0.24), lit);
		vec3 cloudCol = mix(nightCol, dayCol, day);
		vec3 duskTop  = sunColor * vec3(1.5, 0.85, 0.42);
		cloudCol = mix(cloudCol, duskTop, dusk * (0.35 + 0.65 * lit));
		cloudCol += baseSky * ((1.0 - day) * (0.30 + 0.50 * lit));
		cloudCol += sunColor * mix(vec3(1.0), vec3(1.25, 0.78, 0.42), dusk)
		          * (phase * sun * 0.9 * max(day, dusk));
		// Flat dark base → bright crown. Milder than classic's 0.30..1.32 ramp —
		// the sun march already carries most of the vertical contrast.
		cloudCol *= mix(0.45, 1.15, smoothstep(0.0, 0.55, hf));
		cloudCol += vec3(0.06, 0.09, 0.15) * ((1.0 - hf) * day * 0.20); // sky bounce under the base
		cloudCol *= uCloudTint;
		float hazeFar = smoothstep(maxDist * 0.35, maxDist, t);
		cloudCol = mix(cloudCol, baseSky, hazeFar * 0.6);

		float distFade     = 1.0 - smoothstep(maxDist * 0.5, maxDist, t);
		float opticalDepth = dens * (ds / thick) * 12.0 * distFade * densMul;
		float a = 1.0 - exp(-opticalDepth);
		L += T * a * cloudCol;
		T *= 1.0 - a;
		if (T < 0.02) break;
	}
	// Grazing-angle fade, symmetric in |dir.y| (see the Metal note): works the
	// same looking down from above the deck as looking up from below.
	float horizon = smoothstep(elevFloor, elevFloor + 0.14, abs(dir.y));
	T = 1.0 - (1.0 - T) * horizon;
	L *= horizon;
	outT = T;
	return baseSky * T + L;
}

// ── Cloud-shadow map pass (uCloudShadowPass == 1) ────────────────────────────
// Sun transmittance of the cloud slab over a world-space XZ region around the
// camera, one texel = a point on the slab's MID-PLANE, short march along the
// sun through the slab with the SAME density field applyClouds3D raymarches
// (coverage fBm → presence → tower profile → billow erosion; fine octave
// skipped — map texels are ~20 m). The lit shaders project fragments along L
// onto the mid-plane and sample the map (cloudShadowFactor in kUnlitFS /
// heCloudShadowFactor in the material preamble). Mirrors the Metal
// cloudShadowFragment exactly.
float cloudShadowTransmittance(vec2 fragCoord)
{
	vec2 uv = fragCoord / max(uCloudShadowRegion.w, 1.0);
	vec2 xz = uCloudShadowRegion.xy + uv * uCloudShadowRegion.z;
	vec3 sd = normalize(uSunDir);
	if (sd.y <= 0.05) return 1.0;
	float coverage = clamp(uCloudCoverage, 0.0, 1.0);
	if (coverage <= 0.0) return 1.0;
	float cloudH  = max(uCloudHeight, 1.0);
	float thick   = cloudH * 1.5;
	float baseY   = cloudH;   // ABSOLUTE deck altitude — same slab the view march uses
	float midY    = baseY + 0.5 * thick;
	float fluff   = clamp(uCloudFluffiness, 0.0, 1.0);
	float densMul = clamp(uCloudDensity, 0.0, 3.0);
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
		vec3 pos = vec3(xz.x, midY, xz.y) + sd * (t0 + (float(i) + 0.5) * ds);
		// Same optical-depth normalisation as the view march (ds/thick * 7).
		od += cloudFieldDensity(pos, baseY, thick, nscale, lo, uTime, uWind, fluff,
		                        float(uCloudStyle), uCloudEvolution)
		    * (ds / thick) * 7.0 * densMul;
	}
	return exp(-od);
}

// Nebula filament line: a single thin iso-contour of a value-noise field.
// Level sets of a smooth field are closed loops around its extrema; the web is
// built from the UNION of several INDEPENDENT single-iso families (different
// fetches/offsets) so loops cross instead of nesting — crossing walls read as
// a cracked cellular cage (JWST-Crab), never as concentric onion rings.
// starFbm3(p,1) is bell-shaped around ~0.25, so the iso sits on the histogram
// FLANK (thin loops; an iso at the mode would flood). Mirrors the Metal helper.
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
// Night/horizon gated, band-gated, seeded; occluded by clouds. Mirrors Metal.
vec3 nebula(vec3 dir, vec3 cdir, vec3 sunDir, float intensity, vec3 nebColor)
{
	bool hifi = uNebulaHiFi >= 0.5;   // 1 High, 2 Max → richer warp/web/silk
	bool maxq = uNebulaHiFi >= 1.5;   // 2 Max → back web + neck fray + extra silk/beads
	float cover = clamp(uNebulaCover, 0.0, 1.0);
	if (intensity <= 0.0 || cover <= 0.0) return vec3(0.0);
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
	// COVERAGE widens the galactic lane: 0.5 = the classic tight band (exp 4.5),
	// 0 → a narrow sliver, 1 → nebulosity spreads across most of the sky.
	float band = exp(-bd * bd * mix(8.2, 0.8, cover));
	vec3  P    = cN * 3.4;
	// SEED: shift the sample window into the noise field so the piece layout (and the
	// colour layout below) re-randomises. The band stays put — it comes from cN.
	P += vec3(uNebulaSeed * 13.1, uNebulaSeed * 7.7, uNebulaSeed * 19.3);
	float sd = uNebulaSeed;

	// (1) FLOW WARP — advects every later field so chunks/wisps shear organically.
	vec3 w1p = P * 0.55 + sd * 0.31;
	vec3 Q1 = vec3(starFbm3(w1p,                          2),
	                   starFbm3(w1p + vec3(19.3, 7.1, 3.7), 2),
	                   starFbm3(w1p + vec3(5.2, 1.9, 11.4), 2)) - 0.5;
	vec3 Q2 = vec3(0.0);
	if (hifi)
	{
		vec3 w2p = P * 1.10 + 3.1 * Q1 + sd * 1.7 + 41.0;
		Q2 = vec3(starFbm3(w2p,                            2),
		            starFbm3(w2p + vec3(27.6, 13.2, 8.8),  2),
		            starFbm3(w2p + vec3(3.3, 21.7, 5.1),   2)) - 0.5;
	}
	vec3 Pw = P + 0.90 * Q1 + 0.42 * Q2;   // advected → flowing structure
	vec3 Pc = P + 0.30 * Q1;               // steadier coord for the cluster gate
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
	vec3 Pp = P + 0.65 * Q1 + 0.30 * Q2;   // warped piece coords (organic silhouettes)
	float ero = starFbm3(Pw * 2.3 + 33.0, 2) - 0.375;
	float thr   = mix(0.72, 0.40, cover);    // piece radius: cover 0 tiny .. 1 huge
	float exist = smoothstep(mix(0.44, 0.10, cover), mix(0.74, 0.40, cover),
	                         starFbm3(Pc * 0.60 + sd * 0.60 + 60.0, hifi ? 4 : 3));
	float pd = worleyNoise3(Pp * 2.2 + sd * 17.0) + ero * 0.45 - thr;
	// Half-size satellite pieces between the big ones — ALL tiers (they carry
	// most of the visible piece count at mid coverage; one extra tap).
	pd = max(pd, (worleyNoise3(Pp * 4.4 + 91.0) + ero * 0.36 - thr - 0.05) * 0.92);
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
	vec3 Ps = P + 0.55 * Q1;
	float r1 = 1.0 - abs(4.0 * starFbm3(vec3(Ps.x, Ps.y * 0.25, Ps.z) * 3.2 + 210.0 + sd, 2) - 1.0);
	float wisp = smoothstep(0.35, 0.95, r1);
	if (hifi)
	{
		float r2 = 1.0 - abs(4.0 * starFbm3(vec3(Ps.x * 0.25, Ps.y, Ps.z) * 4.6 + 610.0 + sd, 2) - 1.0);
		wisp = max(wisp, smoothstep(0.40, 0.95, r2) * 0.85);
	}
	if (maxq)
	{
		float r3 = 1.0 - abs(4.0 * starFbm3(vec3(Pw.x, Pw.y * 0.30, Pw.z) * 7.3 + 950.0, 2) - 1.0);
		wisp = max(wisp, smoothstep(0.45, 0.95, r3) * 0.70 * aaFine);
	}
	float silk = clamp(stri * smoothstep(0.10, 0.45, wisp) * (0.35 + 0.65 * wisp) + wisp * 0.28, 0.0, 1.2);

	// (4) CRACKLE CAGE — the filament web: the VEINS (Adern) threading each
	// piece's interior, densest along the border, and the strands that bridge
	// the necks. Each scale is TWO independent single-iso families (nebIso)
	// whose loops CROSS → cellular walls, no concentric nesting. A high-freq
	// CRINKLE warp (High/Max) wiggles the walls at small scale.
	vec3 Pk = Pw;
	if (hifi)
		Pk += (vec3(starFbm3(Pw * 7.5 + 331.0, 1),
		              starFbm3(Pw * 7.5 + 337.7, 1),
		              starFbm3(Pw * 7.5 + 343.3, 1)) - 0.25) * 0.40;
	float nC    = starFbm3(Pk * 2.6 + 501.0 + sd * 0.5, 1);
	float cage  = nebIso(nC, 0.260, 0.014);
	cage        = max(cage, nebIso(starFbm3(Pk * 3.1 + 517.7, 1), 0.245, 0.012) * 0.90);
	cage       += nebIso(starFbm3(Pk * 5.3 + 622.0, 1), 0.262, 0.010) * 0.65;
	if (hifi)
		cage   += nebIso(starFbm3(Pk * 6.4 + 651.3 + sd * 0.8, 1), 0.248, 0.009) * 0.55;
	float cF = 0.0;
	if (hifi)
	{
		cF = nebIso(starFbm3(Pk * 10.7 + 743.0, 1), 0.255, 0.008);
		cage += cF * 0.55;
	}
	if (maxq)
		cage += nebIso(starFbm3(Pk * 20.6 + 864.0, 1), 0.258, 0.007) * 0.48 * aaFine;
	// TWO independent length-breakers fade walls in/out ALONG their run → broken
	// arcs and braids instead of closed onion rings around every noise extremum.
	float lenFade = smoothstep(0.30, 0.72, starFbm3(Pw * 2.6 + 777.0, hifi ? 2 : 1) * (hifi ? 1.6 : 3.2));
	lenFade *= smoothstep(0.20, 0.62, starFbm3(Pw * 1.15 + 888.0 + sd * 0.3, hifi ? 2 : 1) * (hifi ? 1.8 : 3.6));
	// Fine GRAIN → matte, fibrous texture on gas and filaments (anti-shiny).
	float grain = clamp(starFbm3(Pw * 6.5 + 400.0, hifi ? 2 : 1) * (hifi ? 2.0 : 4.0), 0.0, 1.4);
	cage *= lenFade * (0.62 + 0.48 * grain);
	// STRIPES — a sky-wide random pattern of long vein strands: stretched ridged
	// layers at crossing orientations. Computed for the WHOLE sky so the pattern
	// runs seamlessly from piece to piece — the pieces merely act as its alpha
	// mask in the composite below.
	// Thin flank ISO lines (not ridge peaks — those are fat at the noise mode)
	// on axis-stretched fbm → long crisp strands.
	float sA = starFbm3(vec3(Pw.x, Pw.y * 0.22, Pw.z) * 2.4 + 1300.0 + sd, 2);
	float stripes = nebIso(sA, 0.55, 0.018);
	if (hifi)
	{
		float sB = starFbm3(vec3(Pw.x * 0.22, Pw.y, Pw.z) * 2.9 + 1450.0, 2);
		stripes = max(stripes, nebIso(sB, 0.55, 0.016) * 0.85);
	}
	stripes *= 0.55 + 0.40 * grain;
	cage = max(cage, stripes);
	// Interior MOTTLE — mid-frequency patchiness so the gas inside a piece has
	// visible texture instead of a flat airbrush fill.
	float mott = clamp(starFbm3(Pw * 3.8 + 271.0 + sd, hifi ? 3 : 2) * 2.3, 0.0, 1.5);
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
	if (hifi)  beads  = pow(smoothstep(0.34, 0.16, worleyNoise3(Pk * 26.0 + sd * 31.0)), 2.0) * 0.9;
	if (maxq)  beads += pow(smoothstep(0.32, 0.14, worleyNoise3(Pk * 46.0 + 77.0)), 2.0) * 0.6;
	beads *= smoothstep(0.30, 0.90, cage) * (core * 0.8 + neck * 0.5 + border * 0.3) * aaFine;

	// (6) Max extras: dim BACK web across the halo zone (depth cue) + extra
	// fray strands riding the necks so the connections read fibrous.
	float back = 0.0, neckFray = 0.0;
	if (maxq)
	{
		back     = nebIso(starFbm3(Pw * 1.7 + 953.0 + sd * 0.7, 1), 0.252, 0.020) * skirt * lenFade;
		neckFray = cF * neck * lenFade;
	}

	// (7) DUST — thin reddening lanes (one-sided backlit rim, see below) + rare
	// THICK emissive amber concentrations sitting ON the pieces (MIRI's warm dust).
	float dLn = starFbm3(Pw * 0.85 + 130.0 + sd * 0.9, 2) - 0.55;
	float tau = (1.0 - smoothstep(0.015, 0.060, abs(dLn))) * (hifi ? 2.0 : 1.7);
	// ONE-SIDED edge band (n > iso only): a |n−iso| annulus would cross the noise
	// distribution's mode on the low side and flood the piece with warm rim.
	float rim = smoothstep(0.015, 0.045, dLn) * (1.0 - smoothstep(0.060, 0.100, dLn));
	float dustA = smoothstep(0.72, 0.94, starFbm3(Pc * 0.95 + 313.0 + sd * 0.4, hifi ? 3 : 2) + border * 0.08);
	dustA *= (0.45 + 0.55 * smoothstep(0.20, 0.60, core + border))
	       * mix(1.0, 0.55, smoothstep(0.60, 1.00, cover)); // high cover: don't drown the sky in amber
	tau += dustA * 1.5;                       // the thick patches also redden the gas behind

	// ---- shared astro composition (all tiers) ----
	// Region colour field: which piece leans colour-2 vs colour-3 on its veins.
	float h = clamp(starFbm3(P * 0.5 + 71.0 + sd * 5.0, maxq ? 4 : (hifi ? 3 : 2)) * 1.7 - 0.35, 0.0, 1.0);
	float regionW = smoothstep(0.12, 0.88, h);
	vec3 veilCol = nebColor;                                   // interior gas colour (colour 1)
	vec3 filBase = mix(uNebulaColor2, uNebulaColor3, regionW);         // vein/rim colours (colour 2 ↔ 3)
	vec3 hotCol  = vec3(1.02, 0.96, 0.80) * 0.85 + filBase * 0.25; // ionized crests → cream
	vec3 filCol  = mix(filBase, hotCol, clamp(ion, 0.0, 1.0));
	// Wavelength-dependent dust extinction: blue is extinguished first, so the lanes
	// silhouette the gas in brown/amber instead of flat grey (interstellar reddening).
	vec3 Td = exp(-tau * vec3(0.55, 1.05, 1.90));
	// High coverage floods the sky with gas — pull the interior gain down a little
	// so a full nebula sky keeps depth instead of washing to white (0.5 unchanged).
	float hiCov = 1.0 - 0.40 * smoothstep(0.60, 1.00, cover);
	// BACK→FRONT: faint halo fog + glowing neck haze (the connections), then the
	// solid piece — blue silk-layered interior that whitens toward the centre ...
	vec3 C = veilCol * (skirt * (1.0 - core) * 0.06)
	         + mix(veilCol, filBase, 0.30) * (neck * 0.09);
	C += mix(veilCol, filBase, 0.55) * (back * 0.22);            // (Max) dim web behind the pieces
	// Dark contrast ring just OUTSIDE the border: pinches the halo fog at the
	// silhouette so every piece reads as a clearly defined, solid form.
	float ring = smoothstep(-0.055, -0.014, pd) * (1.0 - smoothstep(-0.014, 0.000, pd));
	C *= 1.0 - ring * 0.45;
	// FILLED interior: the shape reads solid (high base fill), and its colour
	// varies WITHIN the user colour — a deep shade blended toward a pale tint
	// of colour 1 by the mottle — while silk/veins pattern the fill on top.
	vec3 innerDeep = veilCol * vec3(0.60, 0.70, 1.02);
	vec3 innerPale = veilCol * vec3(1.28, 1.16, 0.96) + vec3(0.05, 0.05, 0.06);
	vec3 innerCol  = mix(innerDeep, innerPale, clamp(mott * 0.75, 0.0, 1.0));
	C += innerCol * hiCov * (core * (0.62 + 0.50 * depth) * (0.68 + 0.32 * silk))
	   + vec3(0.88, 0.94, 1.06) * hiCov * (depth * depth * (0.12 + 0.30 * silk) * (0.60 + 0.40 * mott));
	C *= Td;                                                      // ... absorbed by the dust ...
	C += (uNebulaColor2 * vec3(1.10, 0.72, 0.45)) * (dustA * (0.45 + 0.40 * grain)); // thick amber glow
	// Backlit dust edges: warm translucent rim where a lane crosses the glow behind it.
	C += (filBase * 0.55 + vec3(0.30, 0.16, 0.06)) * (rim * 0.30 * (core * 0.9 + depth * 0.5));
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
	C *= (band * 1.05 + 0.05) * (1.0 - 0.90 * mwRift(cN));
	C *= 2.05 * intensity * smoothstep(0.0, 0.05, cover);   // smooth kill toward cover = 0
	float lum = dot(C, vec3(0.30, 0.59, 0.11));
	if (lum > 1e-5) C *= (lum / (1.0 + lum * 0.22)) / lum;
	float horizon = smoothstep(0.0, 0.16, dir.y);
	return max(C, vec3(0.0)) * (horizon * night);
}

// Per-curtain pseudo-random parameter: curtain index k, slot s → [0,1). EVERY property of a
// curtain (placement, heading, length, thickness, meander, height in the slab, brightness) is
// drawn from this, so no two curtains repeat. The previous version shared one meander function
// and gave every band the SAME heading (all along world-X) — which is exactly why they read as
// a stack of parallel stripes instead of a display.
float auroraRnd(float k, float s)
{
	return starHash(vec3(k * 0.7331 + 1.13, s * 1.9137 + 7.71, 19.37));
}

// Ridged "vein" field. The zero-set of the DIFFERENCE of two decorrelated noise fields is a
// network of thin branching filaments that close on themselves — the classic marble/vein
// pattern. Threading it through a curtain breaks the sheet into a braided web instead of one
// smooth wall, which is what gives an active display its net-like filigree up close.
float auroraWeb(vec2 p)
{
	float a = cloudNoise(p);
	float b = cloudNoise(p * 1.43 + vec2(37.2, 11.9));
	return smoothstep(0.30, 0.0, abs(a - b));       // 1 on a vein, 0 in between
}

// Clip the ray interval [t0,t1] against the slab |x0 + dx*t| <= w (x is any linear coordinate
// along the ray). Returns false when the ray misses it entirely. Two of these — one across the
// curtain, one along it — cut the march down to the ribbon's own oriented box.
bool auroraSlab(float x0, float dx, float w, inout float t0, inout float t1)
{
	if (abs(dx) < 1e-7) return abs(x0) <= w;
	float ta = (-w - x0) / dx, tb = (w - x0) / dx;
	t0 = max(t0, min(ta, tb));
	t1 = min(t1, max(ta, tb));
	return t1 > t0;
}

// Aurora borealis — WORLD-ANCHORED volumetric curtains, built like the 3D cloud slab so they
// are PLACED IN THE WORLD (real parallax as the camera moves) and have NO azimuth seam
// (sampled at world XZ, not atan-azimuth).
//
// Each curtain is an INDEPENDENT FINITE RIBBON with its own centre, heading, length, thickness,
// meander and height inside the slab — all from auroraRnd(). Headings are biased tangential to
// the ring the curtain sits on (real arcs align with the auroral oval) but spread by ±74°, so
// curtains genuinely CROSS one another. That crossing, plus the auroraWeb() vein filigree
// inside each ribbon, is what makes the net: a quiet aurora is a single E-W arc, but near
// magnetic midnight the oval breaks into folds (~20 km), curls (<10 km) and spirals, with N-S
// structures running across the E-W arcs — see earthsky.org "Forms of aurora" and Springer,
// "Physical Processes of Meso-Scale, Dynamic Auroral Forms". The three meander sines are that
// same scale hierarchy (arc sweep → folds → curls) with per-curtain amplitude/frequency/phase.
//
// Depth comes from three things beyond the plain perspective: curtains sit at DIFFERENT
// altitudes with different vertical extents, aerial perspective dims and cools them with
// distance, and the ray striation is sheared along the geomagnetic field line so all rays are
// parallel in 3D and converge perspectively on the magnetic zenith (the corona). Colour follows
// the real emission altitudes — magenta N2+ fringe on the bottom edge, green 557.7 nm oxygen
// body, diffuse red/violet 630 nm top. Motion is layered: the arc drifts, surges race along it,
// the fine rays stream sideways, and each curtain pulses on its own clock.
//
// The march is clipped PER CURTAIN to that ribbon's own oriented box (three auroraSlab calls —
// altitude band, across, along) instead of marching the whole envelope once for all bands: far
// fewer steps and a much tighter fit, which is what pays for the extra curtains, the vein noise
// and the per-curtain altitudes. The Gaussian sheet
// is pre-filtered against the step size (widen σ, renormalise to keep the line integral) so an
// under-sampled distant curtain BLURS instead of beating into moiré.
//
// PURELY EMISSIVE: additive, no Beer's law, no light-march — so curtain order is irrelevant and
// each one can be marched on its own. The soft rolloff keeps overlapping curtains from clipping
// to white (FragColor is LDR). Returns the emission to ADD to the sky BEFORE the clouds (so the
// nearer cloud layer occludes it).
vec3 applyAurora3D(vec3 dir, vec3 camPos, float time, float intensity, vec3 colBase, vec3 colTop)
{
	if (intensity <= 0.0) return vec3(0.0);
	dir = normalize(dir);
	if (dir.y < 0.02) return vec3(0.0);                         // horizon → ray never reaches the curtains
	float night = 1.0 - smoothstep(-0.20, -0.02, clamp(normalize(uSunDir).y, -0.3, 1.0));
	if (night <= 0.0) return vec3(0.0);

	// World-space altitude envelope. Height control drives the curtain ELEVATION (kept high so
	// the parallax stays subtle — aurora is near-infinite; lower = more exaggerated parallax).
	// Each curtain picks its OWN altitude and vertical extent inside this envelope: real
	// curtains are not all parked on one shelf, and that spread is a large part of the depth
	// read, so the global bracket only has to contain the union of them.
	float altitude = mix(1500.0, 7000.0, clamp(uAuroraHeight, 0.0, 1.0));
	float invY  = 1.0 / dir.y;
	float tNear = max(altitude * 0.72 * invY, 0.0);
	float tFar  = altitude * 3.75 * invY;
	// The layout reaches out to ~13.5 × altitude, so rays that leave the envelope beyond that
	// can never hit a curtain — clamp there rather than at some arbitrary huge distance.
	float layoutR = altitude * 13.5;
	tFar = min(tFar, layoutR * 2.2);
	if (tFar <= tNear) return vec3(0.0);

	float frag = clamp(uAuroraFragment, 0.0, 1.0);
	float jit  = skyIgn(gl_FragCoord.xy);
	vec2  o    = camPos.xz;                                     // ray origin in world XZ
	vec2  dxz  = dir.xz;                                        // ray XZ velocity (NOT unit — |dxz| = cos(elev))

	vec3 acc = vec3(0.0);
	for (int k = 0; k < 22; ++k)
	{
		float fk = float(k);
		// ── Layout: stratified ring radius (near-zenith … horizon, one curtain per stratum,
		//    jittered inside it) and a golden-angle azimuth so they never clump on one side.
		float rr     = (fk + auroraRnd(fk, 0.0)) / 22.0;
		float radius = altitude * (0.18 + 13.3 * rr * rr);
		float phi    = fk * 2.39996323 + auroraRnd(fk, 1.0) * 1.9;
		vec2  cen    = vec2(cos(phi), sin(phi)) * radius;
		// ── Heading: tangential to the ring (the oval runs E-W) ± 74°, so neighbouring curtains
		//    run at genuinely different angles and overlap into a mesh instead of stacking up.
		float head = phi + 1.5707963 + (auroraRnd(fk, 2.0) - 0.5) * 2.6;
		vec2  tang = vec2(cos(head), sin(head));
		vec2  nrm  = vec2(-tang.y, tang.x);
		// ── Everything scales with the ring radius: a curtain twice as far away must be twice
		//    as long, thick and snaky to keep the same apparent size on the dome.
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
		// ── This curtain's own altitude and vertical extent (see the envelope note above).
		float altK   = altitude * (0.72 + 0.62 * auroraRnd(fk, 14.0));
		float thickK = altK * (1.00 + 0.80 * auroraRnd(fk, 18.0));
		float hDec   = 4.6 - 2.4 * auroraRnd(fk, 15.0);         // how fast it fades upward
		float bright = 0.55 + 0.70 * auroraRnd(fk, 16.0);
		// ── Motion. A real display does three things at once: the whole arc drifts (the meander
		//    time terms), surges of brightness race ALONG the arc every few seconds, and the fine
		//    rays stream sideways. Speeds and directions are per-curtain, so the sky never pulses
		//    in unison. Pulsation is per-curtain, so it hoists out of the march entirely.
		float tph    = auroraRnd(fk, 19.0) * 6.2831853;
		float puls   = 0.80 + 0.20 * sin(time * (0.30 + 0.55 * auroraRnd(fk, 20.0)) + tph);
		float surgeF = (1.5 + 1.1 * auroraRnd(fk, 21.0)) / scl;             // surges per unit length
		float surgeV = (0.7 + 1.3 * auroraRnd(fk, 22.0)) * (auroraRnd(fk, 23.0) < 0.5 ? -1.0 : 1.0);
		float rayDrift = (0.5 + 1.1 * auroraRnd(fk, 24.0)) * (auroraRnd(fk, 25.0) < 0.5 ? -1.0 : 1.0);
		float fringeAmt = (0.35 + 0.55 * frag) * (0.45 + 0.55 * auroraRnd(fk, 26.0));
		// Per-curtain hue jitter, so a display is not one flat colour end to end.
		vec3 hueK = mix(vec3(0.92, 1.02, 0.94), vec3(1.07, 0.97, 1.06), auroraRnd(fk, 17.0));
		// Auroral rays follow the geomagnetic FIELD LINE, not local vertical. Every ray in the
		// sky is therefore parallel in 3D, and perspective makes them converge on the magnetic
		// zenith — that convergence IS the corona, and it is the strongest depth cue available
		// here. shearU is how far the field line slides along this curtain per unit of height.
		float shearU = dot(vec2(0.22, 0.13), tang);

		// ── Clip the ray to this ribbon's oriented box: its own altitude band, then
		//    |across| <= meander + 3σ, then |along| <= len.
		float u0 = dot(o - cen, tang), du = dot(dxz, tang);
		float v0 = dot(o - cen, nrm ), dv = dot(dxz, nrm );
		float halfW = a1 + a2 + a3 + sigma0 * 3.0;
		float t0 = tNear, t1 = tFar;
		if (!auroraSlab(-(altK + thickK * 0.5), dir.y, thickK * 0.5, t0, t1)) continue;
		if (!auroraSlab(v0, dv, halfW,                                t0, t1)) continue;
		if (!auroraSlab(u0, du, halfLen * 1.4,                        t0, t1)) continue;

		// Step so we get a few samples ACROSS the sheet. What matters is how fast the distance
		// to the centreline changes, and that has TWO parts: how fast the ray crosses the
		// curtain (dv) AND how fast the centreline itself slides away under it as the ray
		// travels along the curtain (the meander slope × du). Ignoring the second term leaves
		// near-edge-on rays badly undersampled — that is what rings distant curtains with moiré,
		// because those are exactly the rays with dv ≈ 0 but a large du. The slope is the RMS of
		// the three meander derivatives; scl cancels, so it is a pure number around 1.
		float mndSlope = sqrt(0.5 * (a1 * f1 * a1 * f1 + a2 * f2 * a2 * f2 + a3 * f3 * a3 * f3));
		float dRate = max(abs(dv) + mndSlope * abs(du), 0.02);  // |d| change per unit t
		int   Nk = int(clamp((t1 - t0) / (sigma0 * 1.1 / dRate), 6.0, 64.0));
		float ds = (t1 - t0) / float(Nk);
		float dAcross = ds * dRate;                             // how far d moves per step

		for (int i = 0; i < Nk; ++i)
		{
			float t   = t0 + (float(i) + jit) * ds;
			vec3  pos = camPos + dir * t;                       // WORLD position → real parallax
			float u   = dot(pos.xz - cen, tang);
			float e   = abs(u) / halfLen;
			if (e > 1.4) continue;
			// Density envelope toward the tips. This MUST decay smoothly to zero rather than
			// stop at a boundary: viewed near edge-on, the ray's chord through the ribbon jumps
			// from nothing to a long bright path across a hard |u| = halfLen plane, and that
			// plane projects to a straight line — the ribbon visibly tears off mid-sky. A flat
			// top with a steep-but-finite shoulder keeps the middle at full strength and reaches
			// ~0 by e = 1.2, so there is no plane to see.
			float e2 = e * e;
			float ends = exp(-3.0 * e2 * e2 * e2);
			float hf   = clamp((pos.y - camPos.y - altK) / thickK, 0.0, 1.0); // 0 = this curtain's foot
			// Vertical emission: sharp bright lower edge at this curtain's own foot, long
			// exponential fade upward (fall-streaks), and a soft top so the ribbon dissolves
			// instead of ending on a flat lid. The fade also steepens toward the tips, so an arc
			// thins out to a point rather than staying full height and then vanishing.
			float hDecE = hDec * (1.0 + 2.2 * (1.0 - ends));
			float Ev = smoothstep(0.0, 0.05, hf)
			         * exp(-hf * hDecE)
			         * (1.0 - smoothstep(0.62, 1.00, hf));
			if (Ev <= 0.002) continue;
			float v = dot(pos.xz - cen, nrm);
			// Centreline meander in this curtain's OWN frame: arc sweep + folds + curls, each
			// with its own amplitude, frequency, phase and drift speed.
			float mnd = a1 * sin(u * f1 + p1 + time * 0.21)
			          + a2 * sin(u * f2 + p2 - time * 0.33)
			          + a3 * sin(u * f3 + p3 + time * 0.52);
			float d = v - mnd;
			float distLOD = smoothstep(altitude * 1.5, layoutR, t);
			float sigmaG  = sigma0 * (1.0 + 0.8 * distLOD);
			// Pre-filter: never let the sheet be thinner than ~1.25 steps across, or the Gaussian
			// falls between samples and beats into moiré (and the per-pixel IGN jitter turns that
			// into visible dither). Widen it and renormalise so the line integral through the
			// sheet is unchanged — blur, not aliasing.
			float sigmaE = max(sigmaG, dAcross * 1.55);
			float sheet  = exp(-(d * d) / (2.0 * sigmaE * sigmaE)) * (sigmaG / sigmaE);
			if (sheet < 0.004) continue;
			// Ray/web structures are indexed by the FOOT of the field line through this sample,
			// not by the sample itself — that is what leans the whole striation along the field
			// and makes it converge toward the magnetic zenith instead of standing dead vertical.
			float uRay = u - hf * thickK * shearU;
			// Fine vertical RAY striation, streaming sideways along the curtain, and faded out
			// with distance so it does not alias into the far field. All detail frequencies are
			// in units of scl, so near and far curtains carry the SAME amount of structure —
			// absolute world frequencies would leave near ribbons smooth and alias far ones.
			// It also smooths out toward the top: the red 630 nm oxygen line up there has a ~110 s
			// radiative lifetime, so the high part of a curtain is genuinely diffuse, never rayed.
			float rays = 0.62 + 0.38 * cloudNoise(vec2(uRay / scl * 30.0 + fk * 17.0 - time * rayDrift,
			                                           hf * 2.2 + fk));
			rays = mix(rays, 1.0, max(distLOD, smoothstep(0.30, 0.80, hf)));
			// The braided vein web ON the curtain surface (along × height). This is what turns a
			// single ribbon into a net rather than a smooth wall; the fragmentation slider drives
			// how hard the gaps between the veins are punched out.
			float web = auroraWeb(vec2(uRay / scl * 6.0 + fk * 9.0 - time * 0.06 * rayDrift,
			                           hf * 1.3 + fk * 3.0 + time * 0.04));
			float weave = mix(1.0, 0.10 + 1.45 * web, 0.35 + 0.65 * frag);
			// Surges of brightness racing along the arc — the part that reads as "dancing".
			float surge = 0.70 + 0.55 * sin(u * surgeF - time * surgeV + tph);
			// Colour by altitude, following the real auroral spectrum: a magenta N2+ fringe on the
			// very bottom edge of an active curtain (427.8 nm plus N2 red, below ~100 km), the
			// green 557.7 nm oxygen body above it, and the diffuse red/violet 630 nm oxygen top
			// (>200 km). The fringe is derived from the user's top colour rather than hard-coded,
			// so it still tracks whatever palette the scene picked.
			vec3 cCol = mix(colBase, colTop, smoothstep(0.22, 0.70, hf));
			vec3 fringeCol = colTop * vec3(1.55, 0.62, 1.05) + vec3(0.10, 0.0, 0.06);
			cCol = mix(cCol, fringeCol, (1.0 - smoothstep(0.02, 0.19, hf)) * fringeAmt);
			cCol *= hueK;
			// Aerial perspective. The air between the viewer and a curtain tens of km away both
			// dims it and shifts it cool; without that every curtain reads as if it sat at the
			// same distance and the whole display goes flat. The trailing smoothstep takes the
			// contribution cleanly to zero before the tFar clamp so there is no cut.
			float aer  = exp(-t / (layoutR * 0.95));
			cCol = mix(cCol * vec3(0.58, 0.72, 1.02), cCol, aer);
			float fade = mix(0.35, 1.0, aer) * (1.0 - smoothstep(layoutR * 1.5, layoutR * 2.1, t));
			// Normalise by the curtain's OWN thickness, not the envelope: the line integral
			// through a Gaussian sheet is ∝ σ, so dividing by a shared height would make a fat
			// distant curtain many times brighter than a thin near one instead of just wider.
			acc += cCol * (sheet * Ev * rays * weave * ends * bright * puls * surge
			               * (ds / (sigma0 * 6.0)) * fade); // pure ADD (emissive, no extinction)
		}
	}
	// LUMINANCE-preserving rolloff (FragColor is LDR). A per-channel rolloff drives the
	// strongest channel into clipping first, so a saturated green curtain washes out to pale
	// mint long before it is actually bright — compressing on luminance instead holds the hue
	// all the way up, and only the genuinely hottest cores desaturate (which real bright aurora
	// does too). Same trick the nebula uses.
	// Each ray integrates through the WHOLE vertical colour ramp, so green and violet average
	// toward grey along it and the display reads as pale mint rather than the deep green/violet
	// the palette actually asks for. Push saturation back up about luminance (which the step
	// below then preserves) before compressing.
	float lum = dot(acc, vec3(0.30, 0.59, 0.11));
	acc = max(mix(vec3(lum), acc, 1.35), vec3(0.0));
	if (lum > 1e-5) acc *= (lum / (1.0 + lum * 0.80)) / lum;
	float horizonFade = clamp(dir.y * 8.0, 0.0, 1.0);          // mask the 1/dir.y blow-up
	return acc * (intensity * night * horizonFade * 1.50);
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

// Spectral helper (hue 0 = red … 0.78 ≈ violet) for the rainbow arc.
vec3 skyHsv(float h, float s, float v)
{
	vec3 p = abs(fract(h + vec3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
	return v * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), s);
}
// Primary + secondary rainbow centred on the anti-solar point (−sunDir). Only while it
// is raining (rainAmt) with the sun up but not too high. Subtle + additive; added before
// the cloud composite so clouds occlude it. Mirrors the Metal rainbow().
vec3 rainbow(vec3 dir, vec3 sunDir, float rainAmt)
{
	if (rainAmt <= 0.0) return vec3(0.0);
	dir    = normalize(dir);
	sunDir = normalize(sunDir);
	float day    = smoothstep(-0.02, 0.12, sunDir.y);
	float lowSun = 1.0 - smoothstep(0.45, 0.72, sunDir.y);
	float vis    = day * lowSun;
	if (vis <= 0.0 || dir.y < 0.0) return vec3(0.0);
	float ang = acos(clamp(dot(dir, -sunDir), -1.0, 1.0)) * 57.29578; // degrees from anti-solar point
	float pBand = smoothstep(39.6, 40.6, ang) * (1.0 - smoothstep(42.2, 43.2, ang));
	float tp    = clamp((ang - 40.5) / 1.9, 0.0, 1.0);
	vec3  cP    = skyHsv(0.78 * (1.0 - tp), 1.0, 1.0);
	float sBand = smoothstep(50.2, 51.0, ang) * (1.0 - smoothstep(53.3, 54.3, ang));
	float ts    = clamp((ang - 51.0) / 2.5, 0.0, 1.0);
	vec3  cS    = skyHsv(0.78 * ts, 1.0, 1.0) * 0.5;
	float horizon = smoothstep(0.0, 0.12, dir.y);
	return (cP * pBand + cS * sBand) * (vis * clamp(rainAmt, 0.0, 1.0) * horizon * 0.45);
}

// Crepuscular rays (god-rays). Cheap dome-cloud occlusion proxy: how much sunlight passes
// along direction d (1 = clear sky, 0 = blocked). Mirrors the dome cloud's perlin coverage
// gate (applyClouds) with the Worley billow approximated by its mean, so shafts line up
// with the visible dome clouds for a fraction of the cost. Mirrors the Metal godrayClear().
float godrayClear(vec3 d, float time, float coverage, vec3 wind)
{
	d = normalize(d);
	if (d.y < 0.05) return 1.0;                          // toward the horizon → no cloud slab hit
	float s    = kCloudBase / max(d.y, 1e-3);
	vec3  pos  = d * s;
	vec3  pp   = pos * kCloudScale + wind * time;
	float perlin = starFbm3(pp + vec3(0.0, time * 0.030, 0.0), 4);
	float lo   = mix(0.70, 0.22, clamp(coverage, 0.0, 1.0)); // same threshold as applyClouds
	float dens = smoothstep(lo, lo + 0.10, perlin * 0.5 + 0.275); // billow≈0.5 mean (sharper gap/cloud edge)
	return 1.0 - clamp(dens, 0.0, 1.0);
}
// Sun shafts: march from the view direction toward the sun, accumulating the clear-sky
// fraction so light streaks through the gaps between clouds. Gated to a cone around the
// sun by day, only with partial cloud cover. Additive, sun-coloured. Mirrors Metal crepuscular().
vec3 crepuscular(vec3 dir, vec3 sunDir, vec3 sunColor, float time,
                 float coverage, vec3 wind, float strength)
{
	if (strength <= 0.0) return vec3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float day = smoothstep(-0.02, 0.12, sunDir.y);
	if (day <= 0.0) return vec3(0.0);
	float ct = dot(dir, sunDir);
	if (ct < 0.15) return vec3(0.0);                     // near-sun cone (past ~80° contributes ~0)
	float coverGate = smoothstep(0.05, 0.35, coverage) * (1.0 - smoothstep(0.85, 1.0, coverage));
	if (coverGate <= 0.0) return vec3(0.0);              // need broken cloud for gaps
	const int GN = 8;
	float light = 0.0;
	vec3  d = dir;
	for (int i = 0; i < GN; ++i)
	{
		d = normalize(mix(d, sunDir, 0.12));             // step toward the sun
		light += godrayClear(d, time, coverage, wind);
	}
	light /= float(GN);
	float cone  = pow(clamp(ct, 0.0, 1.0), 2.0);         // falloff away from the sun (extends shaft reach)
	float shaft = light * cone * day * coverGate * clamp(strength, 0.0, 1.0);
	return sunColor * shaft * 0.55;
}

// Subtle moon glow: one soft luminous ring hugging the moon's disk — a gentle aureole so the
// moon reads as glowing rather than a flat cut-out. Deliberately understated (dezent), always
// present at night, cool white, and SHAPED BY THE PHASE: glows on the lit limb and fades across
// the terminator (a crescent glows only on its bright side, a full moon all around). NOT the
// wide 22° halo. Mirrors Metal moonCorona().
vec3 moonCorona(vec3 dir, vec3 sunDir, bool hasMoon, float moonPhase)
{
	if (!hasMoon) return vec3(0.0);
	dir = normalize(dir); sunDir = normalize(sunDir);
	float night = 1.0 - smoothstep(-0.10, 0.10, clamp(sunDir.y, -0.2, 1.0));
	if (night <= 0.0 || dir.y < 0.0) return vec3(0.0);
	vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
	if (dot(dir, moonDir) <= 0.0) return vec3(0.0);
	float vis = night * smoothstep(0.0, 0.04, dir.y) * smoothstep(0.0, 0.10, moonDir.y);
	if (vis <= 0.0) return vec3(0.0);
	const float kMoonR = 0.030;                                   // moon angular radius (matches moonDisk)
	float ang  = acos(clamp(dot(dir, moonDir), -1.0, 1.0));       // radians from moon centre
	float ring = exp(-((ang - kMoonR * 1.15) * (ang - kMoonR * 1.15)) / (0.016 * 0.016)); // soft ring at the limb
	// Phase shaping: build the moon-view frame (as moonDisk), take the outward direction of
	// this ring point, and light a just-inside-the-limb normal by the same sun direction L.
	vec3  right = normalize(cross(vec3(0.0, 1.0, 0.0), moonDir));
	vec3  up    = cross(moonDir, right);
	vec2  rad   = normalize(vec2(dot(dir, right), dot(dir, up)) + vec2(1e-6));
	float ph    = moonPhase * 6.2831853;
	vec3  L     = vec3(sin(ph), 0.0, -cos(ph));                   // sun direction across the disk (== moonDisk)
	vec3  Nlimb = normalize(vec3(rad * 0.85, 0.53));             // normal just inside the lit limb
	float lit   = smoothstep(0.0, 0.55, dot(Nlimb, L));          // 0 dark limb .. 1 lit limb
	return vec3(0.85, 0.90, 1.0) * (ring * lit * 0.17 * vis);    // dezent, phase-shaped
}

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
	// Cloud-shadow map pass: reuses this program (the noise texture + cloud
	// uniforms are already wired) but outputs only the slab transmittance.
	if (uCloudShadowPass > 0.5)
	{
		FragColor = vec4(vec3(cloudShadowTransmittance(gl_FragCoord.xy)), 1.0);
		return;
	}
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
			vec3 comp = (uCloudStyle == 1)
				? applyClouds3DReal(hazeSky, dir, uCameraPos, uSunDir, uTime, uCloudCoverage,
				                    uSunColor, uWind, uCloudHeight, T)
				: applyClouds3D(hazeSky, dir, uCameraPos, uSunDir, uTime, uCloudCoverage,
				                uSunColor, uWind, uCloudHeight, T);
			L = comp - hazeSky * T;                  // recover L
		}
		else
			L = applyClouds(vec3(0.0), dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, T);
		FragColor = vec4(L, T);
		return;
	}
	vec3 col  = skyColor(dir, uSunDir);
	// Star-free atmosphere base for the REALISTIC cloud path's ambient/twilight
	// fill: feeding the full `col` (stars/nebula/moon already added) into the
	// cloud march paints the star field ONTO the cloud bodies via the twilight
	// term. The clouds are lit by THIS instead; the celestial layer is then
	// occluded by the cloud transmittance at the composite (same recipe the
	// low-res pre-pass always used).
	vec3 atmoBase = col;
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
		col += shootingStars(dir, uSunDir, uTime, uShootingStars); // meteors (clouds occlude below)
	}
	// High thin cirrus sits highest (farthest), then contrails, then the cumulus layer
	// in front — so the lower clouds correctly occlude the thin upper layers.
	col  = cirrus(col, dir, uSunDir, uSunColor, uCirrus, uCirrusSeed, uTime, uWind.xz); // alpha-blended
	col  = contrails(col, dir, uSunDir, uContrails, uCloudCoverage); // alpha-blended into the sky
	col += rainbow(dir, uSunDir, uRainAmount);           // anti-solar arc while raining (clouds occlude it below)
	col += moonCorona(dir, uSunDir, true, uMoonPhase);   // subtle phase-shaped glow ring around the moon (moon always up in GL)
	float cloudT = 1.0;                                   // view-ray cloud transmittance
	if (uLowResClouds > 0.5)
	{
		// Low-res clouds: composite the upsampled (L, T) from the quarter-res pre-pass, which
		// this backend renders with the CURRENT camera (see DrawScene) so it lines up 1:1 with
		// the sky — no reprojection needed, no panning lag.
		vec4 lt = texture(uCloudTex, vNDC * 0.5 + 0.5);
		col = col * lt.a + lt.rgb;
		cloudT = lt.a;
	}
	else if (uCloudMode == 1 && uCloudStyle == 1)
	{
		// Clouds lit by the star-free atmosphere; stars/nebula/moon (in `col`)
		// are occluded by the transmittance instead of being painted onto the
		// cloud bodies. L = comp − atmoBase·T recovers the clouds' own light.
		vec3 comp = applyClouds3DReal(atmoBase, dir, uCameraPos, uSunDir, uTime, uCloudCoverage,
		                              uSunColor, uWind, uCloudHeight, cloudT);
		col = col * cloudT + (comp - atmoBase * cloudT);
	}
	else if (uCloudMode == 1)
		col = applyClouds3D(col, dir, uCameraPos, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, uCloudHeight, cloudT);
	else
		col = applyClouds(col, dir, uSunDir, uTime, uCloudCoverage, uSunColor, uWind, cloudT);
	// Re-add the sun, steeply occluded by cloud opacity so a solid cloud fully hides it.
	col += (sunGlareCol + sunBodyCol) * pow(cloudT, 2.5);
	// God-rays: sun shafts through cloud gaps. Scaled by cloudT so a cloud directly in front
	// dims the shaft (rays show in the clear air, not painted over the cloud).
	col += crepuscular(dir, uSunDir, uSunColor, uTime, uCloudCoverage, uWind, uGodRays) * cloudT;
	col += uFlash * vec3(0.85, 0.90, 1.0); // lightning lights up the sky/clouds
	FragColor = vec4(col, 1.0);
}
)GLSL";

// Shared analytic sky, injected (via the //#SKYFUNC# marker) into both the
// skybox FS and the scene FS so background and image-based ambient match. The
// sky's mood is driven by the sun's elevation (sunDir.y): a daytime blue sky
// warms and reddens at the horizon as the sun sets and dims into night.
inline constexpr const char* kSkyFuncGLSL = R"GLSL(
// ---- Physically-based single-scattering atmosphere (Rayleigh + Mie + ozone) ----
// Compact fixed-step single scatter for a ground-level camera: 12 view samples,
// each with a 5-sample sun-transmittance march. Sunset reddening, the blue hour
// and the horizon's pale saturation all EMERGE from the optical-depth integrals
// instead of hand-tuned gradient blends. Mirrored in MSL + the CPU IBL bakes —
// keep all four copies in sync.
// "No hit" returns a NEGATIVE near distance. That sign matters: the caller's
// sun-visibility test is `atmoRaySphere(p, sunDir, Rg).x > 0.0` → shadowed, and a
// ray that misses the planet entirely is the one case where the sun is certainly
// VISIBLE. A positive miss sentinel therefore marked every such sample shadowed,
// which is most of the sky the moment the sun nears the horizon — atmoScatter
// collapsed to exactly zero at sunY = 0 and the sky snapped to black at sunset.
// The other two callers only test for a hit IN FRONT, so a negative sentinel is
// correct for them too.
vec2 atmoRaySphere(vec3 ro, vec3 rd, float R)
{
	float b = dot(ro, rd);
	float c = dot(ro, ro) - R * R;
	float d = b * b - c;
	if (d < 0.0) return vec2(-1.0e9, -1.0e9);
	d = sqrt(d);
	return vec2(-b - d, -b + d);
}
vec3 atmoScatter(vec3 dir, vec3 sunDir)
{
	const float Rg = 6360.0e3, Ra = 6440.0e3;                // ground / atmosphere-top radius
	const vec3  bR = vec3(5.802e-6, 13.558e-6, 33.1e-6);     // Rayleigh scattering
	const float bM = 3.996e-6;                               // Mie scattering
	const vec3  bO = vec3(0.650e-6, 1.881e-6, 0.085e-6);     // ozone absorption
	const float HR = 8500.0, HM = 1200.0;                    // scale heights
	vec3 ro = vec3(0.0, Rg + 200.0, 0.0);
	vec2 tA = atmoRaySphere(ro, dir, Ra);
	if (tA.y <= 0.0) return vec3(0.0);
	float t0 = max(tA.x, 0.0), t1 = tA.y;
	vec2 tG = atmoRaySphere(ro, dir, Rg);
	if (tG.x > 0.0) t1 = min(t1, tG.x);                      // stop at the ground
	float ds = (t1 - t0) / 12.0;
	float mu = dot(dir, sunDir);
	float phR = 0.05968310 * (1.0 + mu * mu);                // Rayleigh phase 3/(16π)
	const float g = 0.76, g2 = g * g;
	float phM = 0.11936620 * ((1.0 - g2) * (1.0 + mu * mu)) /   // Cornette-Shanks
	            ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
	vec3  sumR = vec3(0.0), sumM = vec3(0.0);
	float odR = 0.0, odM = 0.0, odO = 0.0;                   // view-path optical depths
	for (int i = 0; i < 12; ++i)
	{
		vec3  p   = ro + dir * (t0 + (float(i) + 0.5) * ds);
		float hgt = length(p) - Rg;
		float dR  = exp(-hgt / HR) * ds;
		float dM  = exp(-hgt / HM) * ds;
		float dO  = max(0.0, 1.0 - abs(hgt - 25.0e3) / 15.0e3) * ds;  // ozone tent layer @25km
		odR += dR; odM += dM; odO += dO;
		if (atmoRaySphere(p, sunDir, Rg).x > 0.0) continue;  // sun below local horizon → shadowed
		float sl = atmoRaySphere(p, sunDir, Ra).y * 0.2;     // 5-sample sun march
		float sR = 0.0, sM = 0.0, sO = 0.0;
		for (int j = 0; j < 5; ++j)
		{
			vec3  q  = p + sunDir * ((float(j) + 0.5) * sl);
			float hq = length(q) - Rg;
			sR += exp(-hq / HR) * sl;
			sM += exp(-hq / HM) * sl;
			sO += max(0.0, 1.0 - abs(hq - 25.0e3) / 15.0e3) * sl;
		}
		vec3 tau = bR * (odR + sR) + (bM * 1.11) * (odM + sM) + bO * (odO + sO);
		vec3 tr  = exp(-tau);
		sumR += tr * dR;
		sumM += tr * dM;
	}
	vec3 L = (sumR * bR * phR + sumM * bM * phM) * 20.0;     // sun irradiance → engine exposure
	// Fake MULTIPLE scattering: single scatter alone leaves long grazing paths
	// yellow/dark at noon (the in-filled skylight is missing). Fill proportional
	// to how opaque the view path is, fading out toward sunset so dusk stays warm.
	vec3 Tcam = exp(-(bR * odR + (bM * 1.11) * odM + bO * odO));
	L += (vec3(1.0) - Tcam) * vec3(0.30, 0.42, 0.60) * (0.35 * smoothstep(0.0, 0.35, sunDir.y));
	return L;
}
vec3 skyColor(vec3 dir, vec3 sunDir)
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
	float day  = smoothstep(-0.10, 0.10, sunY);                 // 0 night → 1 day
	float dusk = smoothstep(-0.14, 0.04, sunY)
	           * (1.0 - smoothstep(0.04, 0.26, sunY));
	// Handed over to only once the twilight wedge below has faded, so the two
	// never leave a dark gap between them.
	float toNight = 1.0 - smoothstep(-0.34, -0.14, sunYd);      // twilight vs deep night

	// Physically-based base sky: day blue, sunset reddening and the blue hour all
	// come from the single-scattering integral above. Below-horizon rays reuse the
	// horizon colour (the ground-haze blend takes over there) — without the clamp a
	// hard navy "ocean band" appears where the ray hits the planet after a short path.
	vec3 sky = atmoScatter(normalize(vec3(dir.x, max(dir.y, 0.004), dir.z)), sunDir);

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
		vec2  sunAz = normalize(sunDir.xz + vec2(1e-5));
		vec2  dxz   = dir.xz;
		float hlen  = length(dxz);
		// Straight up and straight down have no azimuth; normalising a ~zero
		// vector snaps to an arbitrary fixed heading, which would put the full
		// sun-side glow on the poles. Fade to neutral instead.
		float toward = (hlen > 1e-4)
			? clamp(dot(dxz / hlen, sunAz) * 0.5 + 0.5, 0.0, 1.0) : 0.5;
		toward = mix(0.5, toward, smoothstep(0.0, 0.06, hlen));
		float el    = dir.y;                              // SIGNED — see `above`
		float band  = exp(-max(el, 0.0) * 5.2);           // hugs the horizon
		float climb = clamp(max(el, 0.0) * 3.4, 0.0, 1.0);// horizon → overhead
		// The wedge is a SKY term: below the horizon it hands over to the ground
		// blend. Clamping el to 0 instead gave every downward ray the peak
		// horizon glow, and the ground lit up like a desert in the middle of
		// nautical twilight.
		float above = smoothstep(-0.22, -0.01, el);
		vec3  warm  = vec3(1.00, 0.45, 0.17);
		vec3  mid   = vec3(0.62, 0.34, 0.52);
		vec3  cool  = vec3(0.20, 0.30, 0.62);
		vec3  col   = mix(mix(warm, mid, smoothstep(0.0, 0.50, climb)),
		                  cool, smoothstep(0.35, 1.0, climb));
		col = mix(cool, col, toward * toward);     // anti-sun side stays blue
		sky += col * (twi * above * (0.065 + 0.34 * band * toward * toward));
	}

	// Deep-night floor (the scattering term → 0 once the sun is far below the
	// horizon): faint blue gradient so night reflections aren't pitch black.
	float h = clamp(dir.y, 0.0, 1.0);
	sky += mix(vec3(0.006, 0.009, 0.024), vec3(0.003, 0.005, 0.015), h) * toNight;

	// Below the horizon: ease into ground haze. `sky` still holds the HORIZON
	// colour down here (the dir.y clamp above), so the haze is built out of it —
	// the old fixed grey sat brighter than a twilight sky and drew a hard bright
	// band across the horizon line, and it stayed grey while the sky went warm.
	vec3 ground = mix(sky * 0.32, vec3(0.24, 0.23, 0.21), day);
	sky = mix(sky, ground, smoothstep(0.0, -0.20, dir.y));

	// Sun aureole ON TOP of the physical Mie glow — just the tight glare blooms now;
	// the broad golden scatter comes from the Cornette-Shanks phase itself.
	vec3  sunTint = mix(vec3(1.0, 0.58, 0.24), vec3(1.0, 0.96, 0.88),
	                    smoothstep(0.0, 0.28, sunY));
	float s = max(dot(dir, sunDir), 0.0);
	float sunVis = max(day, dusk);
	float bloomDamp = mix(1.0, 0.28, dusk);                        // dimmer at dusk → no white blob
	sky += sunTint * (pow(s, 220.0)  * 0.9  * bloomDamp) * sunVis; // tight bloom
	sky += sunTint * (pow(s, 30.0)   * 0.12 * bloomDamp) * sunVis; // mid aureole

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

// Set-0 bindings of kSkyVulkanPrelude, and the HLSL registers D3D11/D3D12 pin
// them to (he::shaderc::compileHlslPinned) — the registers their sky pass has
// always bound: b0 = sky constants, t0/s0 = moon (clamp), t1/s1 = noise (wrap).
struct SkySlot { uint32_t binding; uint32_t hlslReg; };
inline constexpr SkySlot kSkySlotEnv  { 0, 0 };
inline constexpr SkySlot kSkySlotMoon { 1, 0 };
inline constexpr SkySlot kSkySlotNoise{ 2, 1 };

// Replaces kSkyFS's GL declaration block for the Vulkan/D3D build. The uniform
// block is HE::SkyFrameParams byte for byte (std140 = the HLSL cbuffer packing
// for a mat4 + 17 vec4), so the renderers upload it with one memcpy. Every GL
// uniform of kSkyFS maps onto its field below; the field meanings are the ones
// BuildSkyFrameParams writes (SkyFrameParams.h).
inline constexpr const char* kSkyVulkanPrelude = R"GLSL(#version 450
layout(location = 0) in  vec2 vNDC;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0, std140) uniform SkyEnv
{
	mat4 invViewProj;
	vec4 sunDir;
	vec4 sunColor;
	vec4 params;
	vec4 nebulaColor;
	vec4 auroraColor;
	vec4 wind;
	vec4 cameraPos;
	vec4 cloud;
	vec4 cloudTint;
	vec4 cirrus;
	vec4 nebulaColor2;
	vec4 nebulaColor3;
	vec4 auroraColorTop;
	vec4 starColor;
	vec4 star;
	vec4 star2;
	vec4 neb2;
} heSky;
layout(set = 0, binding = 1) uniform sampler2D uMoonTex;
layout(set = 0, binding = 2) uniform sampler3D uNoise;

// The noise volume is sampled inside every cloud raymarch, and those loops exit
// on the accumulated transmittance. An implicit-LOD texture() there becomes an
// HLSL Sample(), which FXC rejects in a loop with a data-dependent exit (it has
// to unroll to get gradients and cannot). The volume has one mip, so LOD 0 is
// exactly what texture() fetched anyway. The moon keeps its implicit LOD.
vec4 heSkyTexture(sampler2D s, vec2 uv) { return texture(s, uv); }
vec4 heSkyTexture(sampler3D s, vec3 p)  { return textureLod(s, p, 0.0); }
#define texture heSkyTexture

#define uInvViewProj        heSky.invViewProj
#define uSunDir             heSky.sunDir.xyz
#define uHasMoonTex         (heSky.sunDir.w > 0.5)
#define uMoonPhase          heSky.sunColor.w
#define uSunColor           heSky.sunColor.xyz
#define uTimeOfDay          heSky.params.x
#define uCloudCoverage      heSky.params.y
#define uTime               heSky.params.z
#define uAurora             heSky.params.w
#define uNebula             heSky.nebulaColor.w
#define uNebulaColor        heSky.nebulaColor.xyz
#define uAuroraColor        heSky.auroraColor.xyz
#define uMilkyWay           heSky.auroraColor.w
#define uWind               heSky.wind.xyz
#define uFlash              heSky.wind.w
#define uCameraPos          heSky.cameraPos.xyz
#define uCloudMode          int(heSky.cameraPos.w + 0.5)
#define uCloudHeight        heSky.cloud.x
#define uCloudDensity       heSky.cloud.y
#define uCloudFluffiness    heSky.cloud.z
#define uContrails          heSky.cloud.w
#define uCloudTint          heSky.cloudTint.xyz
#define uCirrus             heSky.cloudTint.w
#define uCirrusSeed         heSky.cirrus.x
#define uAuroraHeight       heSky.cirrus.y
#define uAuroraFragment     heSky.cirrus.z
#define uNebulaSeed         heSky.cirrus.w
#define uNebulaColor2       heSky.nebulaColor2.xyz
#define uNebulaHiFi         heSky.nebulaColor2.w
#define uNebulaColor3       heSky.nebulaColor3.xyz
#define uGodRays            heSky.nebulaColor3.w
#define uAuroraColorTop     heSky.auroraColorTop.xyz
#define uShootingStars      heSky.auroraColorTop.w
#define uStarColor          heSky.starColor.xyz
#define uStarBright         heSky.starColor.w
#define uStarSize           heSky.star.x
#define uStarSizeVar        heSky.star.y
#define uStarDensity        heSky.star.z
#define uStarGlow           heSky.star.w
#define uStarTwinkle        heSky.star2.x
#define uCloudQuality       int(heSky.star2.y + 0.5)
#define uRainAmount         heSky.star2.w
#define uNebulaCover        heSky.neb2.x
#define uCloudStyle         int(heSky.neb2.y + 0.5)
#define uCloudInterShadows  int(heSky.neb2.z + 0.5)
#define uCloudEvolution     heSky.neb2.w

// GL/Metal-only passes (see the header of SkyShaderSource.h): always off here.
// uCloudTex is read only behind uLowResClouds, so it aliases the moon sampler
// instead of costing a binding every backend would have to fill.
#define uLowResClouds       0.0
#define uCloudPrepass       0.0
#define uCloudShadowPass    0.0
#define uCloudShadowRegion  vec4(0.0)
#define uCloudTex           uMoonTex
)GLSL";

// The Vulkan-GLSL 4.50 sky fragment shader: kSkyVulkanPrelude, then the shared
// analytic sky, then kSkyFS from its //#SKYFUNC# marker on (the GL declaration
// block above the marker is what the prelude replaces). Empty if the marker is
// missing, which the callers treat like a failed compile.
inline std::string BuildSkyFragmentGLSL450()
{
	const std::string fs     = kSkyFS;
	const std::string marker = "//#SKYFUNC#";
	const size_t      at     = fs.find(marker);
	if (at == std::string::npos) return {};
	return std::string(kSkyVulkanPrelude) + kSkyFuncGLSL + fs.substr(at + marker.size());
}

// Fullscreen far-plane triangle for the cross-compiled sky pixel shader on
// D3D11/D3D12. Same geometry as kSkyVSHLSL (HlslSources.h), but the outputs are
// declared in the order SPIRV-Cross declares the pixel shader's inputs — vNDC
// (TEXCOORD0) first, SV_Position last. D3D links the two stages' signatures by
// register, so kSkyVSHLSL's order (SV_POSITION first) does not fit this PS.
inline constexpr const char* kSkyVSCrossHLSL = R"HLSL(
struct SkyCrossVSOut { float2 ndc : TEXCOORD0; float4 pos : SV_Position; };
SkyCrossVSOut VSSkyCross(uint vid : SV_VertexID)
{
    SkyCrossVSOut o;
    float x = (float)((vid & 1u) << 2u) - 1.0f;
    float y = (float)((vid & 2u) << 1u) - 1.0f;
    o.pos = float4(x, y, 1.0f, 1.0f); // z=1 = D3D far plane
    o.ndc = float2(x, y);
    return o;
}
)HLSL";

} // namespace HE::glsl
