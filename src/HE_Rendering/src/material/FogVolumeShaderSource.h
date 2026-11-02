#pragma once
// Volumetric fog kernels (docs/volumetric-fog-plan.md §5, §6, §10), ONE GLSL-450
// source cross-compiled per backend by MaterialShaderLibrary (fogInject /
// fogIntegrate / fogApply). Private to MaterialShaderLibrary.cpp.
//
// Three programs, one shared constant block (HE::FogVolumeConstants, std140,
// 288 B, HorizonRendering/FogVolume.h — the offsets are static_assert-pinned there):
//
//   Inject    compute, one thread per froxel. Density (height formula x noise +
//             ground bank + precipitation haze) and in-scattering (sky ambient,
//             sun with its CSM + cloud shadow, the 8 window lights with their
//             atlas shadows), blended with the reprojected history. Written as
//             rgb = S (scattered radiance per metre), a = sigma_t (extinction 1/m).
//             Reuses the REAL kLightingPreamble, so heCsmShadow /
//             heLocalShadowFactor / heCloudShadowFactor are the surfaces' own
//             functions (one set of shadow maths, no seventh hand copy).
//   Integrate compute, one thread per column, front-to-back over the slices
//             (Hillaire, energy-conserving). Reads the inject volume with
//             texelFetch and writes (L, T) at the FAR edge of each slice with a
//             storage-image store only: no typed UAV load anywhere, so the same
//             source is legal on D3D11 FL11_0.
//   Apply     fragment, full screen, after the opaque scene + sky. Reads the
//             scene depth, reconstructs P through the backend's own
//             invViewProj line (the underwater pass' line, Thema 185 — never a
//             near-plane constant), samples the integrated volume and writes
//             rgb = Lv + Tv*Lf, a = Tv*Tf for the blend ONE, SRC_ALPHA.
//
// Conventions shared by all three (they must agree, the volume is addressed in
// screen space):
//   * Froxel (x, y) <-> uv = ((x + .5) / gx, (y + .5) / gy) with uv.y = 0 at the
//     TOP of the image, ndc = (uv.x*2-1, 1-uv.y*2) — Metal's fragment origin.
//   * Slice k <-> planar depth z(t) = c * ((1 + D/c)^t - 1), t = (k + .5) / N for
//     the inject volume. The integrated volume stores the value at the FAR edge
//     (t = (k+1)/N) in texel k, so a lookup at t reads w = t - .5/N and blends
//     toward the identity (0, 0, 0, 1) inside the first slice.
//   * Planar depth is the distance along camFwd (what the CSM cascade pick uses).

namespace HE::FogShaderSource
{

// Block shared by the three programs. Binding 36 is the new constants UBO.
inline constexpr const char* kFogVolBlock = R"GLSL(
layout(std140, set = 0, binding = 36) uniform HeFogVol {
    mat4 invViewProj;    // unjittered, GL-style clip, froxel/pixel -> world
    mat4 prevViewProj;   // unjittered previous frame (reprojection)
    vec4 camPos;         // xyz camera, w = D (volume range, m)
    vec4 camFwd;         // xyz view axis, w = c (slice distribution constant, m)
    vec4 grid;           // xyz grid size, w = 1 / slice count
    vec4 density;        // x density, y heightFalloff, z noiseAmount, w invNoiseScale
    vec4 bank;           // x bankDensity, y bankBaseY, z bankThickness, w precipExtinction
    vec4 noise;          // xyz wind-accumulated noise offset (m), w time (s)
    vec4 scatter;        // rgb albedo, w anisotropy g
    vec4 temporal;       // x slice jitter 0..1, y history alpha, z history valid, w frame index
    vec4 misc;           // x localLightScatter, y flash, z sun share of the sky ambient, w reserved
    vec4 pad;            // x = 1 -> parity witness (sun / local lights off), y sun gain, z local-light gain
} fv;
float heFogSliceDepth(float t) { return fv.camFwd.w * (pow(1.0 + fv.camPos.w / fv.camFwd.w, t) - 1.0); }
float heFogDepthSlice(float z)  { return log(1.0 + max(z, 0.0) / fv.camFwd.w) / log(1.0 + fv.camPos.w / fv.camFwd.w); }
// View-ray direction of pixel/column uv (any depth on the ray gives the same direction).
vec3 heFogRayDir(vec2 uv) {
    vec4 p = fv.invViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    return normalize(p.xyz / p.w - fv.camPos.xyz);
}
)GLSL";

// ─── Inject ───────────────────────────────────────────────────────────────────
// Body only: MaterialShaderLibrary assembles "#version", the compute stubs, the
// lighting preamble, then this. Resources: HeLighting (binding 0, preamble),
// HeFogVol (36), noise (35), history (37), output image (38).
inline constexpr const char* kInjectBody = R"GLSL(
layout(set = 0, binding = 35) uniform sampler3D heFogNoise;     // SkyNoise3D: R value noise, G Worley billows
layout(set = 0, binding = 37) uniform sampler3D heFogHistory;   // last frame's inject volume
layout(set = 0, binding = 38, rgba16f) uniform writeonly image3D heFogOut;
)GLSL";

inline constexpr const char* kInjectMain = R"GLSL(
// Henyey-Greenstein, normalised so an isotropic phase is 1 (the surfaces' diffuse has no 1/pi either).
float heFogPhase(float g, float c) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-3), 1.5);
}
// Two octaves of the Worley billows, the second one drifting against the first.
float heFogNoise01(vec3 p) {
    vec3 q = (p + fv.noise.xyz) * fv.density.w;
    float a = textureLod(heFogNoise, q, 0.0).g;
    float b = textureLod(heFogNoise, q * 2.07 + vec3(0.37, 0.11, -fv.noise.w * 0.006), 0.0).g;
    // G is about 0.48 +- 0.15 (p05 0.20, p95 0.77, measured on BuildSkyNoise3D): stretch to 0..1.
    return clamp((0.62 * a + 0.38 * b - 0.20) / 0.60, 0.0, 1.0);
}
void main() {
    ivec3 f = ivec3(gl_GlobalInvocationID);
    ivec3 g = ivec3(fv.grid.xyz);
    if (f.x >= g.x || f.y >= g.y || f.z >= g.z) return;
    vec2  uv   = (vec2(f.xy) + 0.5) / fv.grid.xy;
    vec3  dir  = heFogRayDir(uv);
    float t    = (float(f.z) + fv.temporal.x) * fv.grid.w;
    float z    = heFogSliceDepth(t);
    float cosT = max(dot(dir, fv.camFwd.xyz), 1e-3);
    vec3  pos  = fv.camPos.xyz + dir * (z / cosT);
    float seam = 1.0 - smoothstep(0.8 * fv.camPos.w, fv.camPos.w, z);

    // ── Density (sigma_t, 1/m) ───────────────────────────────────────────────
    float n      = heFogNoise01(pos);
    float base   = fv.density.x * exp(clamp(-fv.density.y * pos.y, -30.0, 30.0));
    float height = base * max(0.0, 1.0 + fv.density.z * (2.0 * n - 1.0));
    float bank   = 0.0;
    if (fv.bank.x > 0.0 && fv.bank.z > 0.0 && pos.y >= fv.bank.y) {
        float top = fv.bank.y + fv.bank.z;
        bank = fv.bank.x * (1.0 - smoothstep(top - 0.5 * fv.bank.z * (0.5 + n), top, pos.y));
    }
    float sigT = height + bank + fv.bank.w;

    vec4 cur = vec4(0.0);
    if (sigT > 1e-6) {
        // ── In-scattering ────────────────────────────────────────────────────
        // Ambient: the sky colour in this column's direction (the SAME heSkyEnv the
        // far field and the graph materials use), partly occluded by the sun's
        // shadow — kSunShare of it vanishes in shadow, so a shaft has a dark side.
        vec3 fogCol = (heLight.fog.z > 0.5) ? textureLod(heSkyEnv, dir, 0.0).rgb : heLight.ambient.rgb;
        vec3 L      = normalize(heLight.sunDir.xyz);
        bool parity = fv.pad.x > 0.5;
        float sunVis = 1.0;
        if (!parity)
            sunVis = heCsmShadow(pos, L, L) * heCloudShadowFactor(pos, L);
        vec3 sigS = fv.scatter.rgb * sigT;
        vec3 S = sigT * fogCol * mix(1.0, sunVis, fv.misc.z * seam);
        if (!parity) {
            float cosSun = dot(dir, L);
            S += sigS * heLight.sunColor.rgb * (heFogPhase(fv.scatter.w, cosSun) * sunVis * seam * fv.pad.y);
            // The window lights, shaped exactly like heLitP's (range falloff, spot cone).
            int count = int(heLight.counts.x);
            for (int i = 0; i < count && i < 8; ++i) {
                int type = int(heLight.lightPos[i].w);
                if (type == 0) continue;               // directional window entries are the sun's business
                vec3  d    = heLight.lightPos[i].xyz - pos;
                float dist = max(length(d), 1e-4);
                vec3  Ll   = d / dist;
                float range = max(heLight.lightParams[i].x, 1e-4);
                float att = clamp(1.0 - dist / range, 0.0, 1.0);
                att *= att;
                if (type == 2) {
                    float cc = dot(-Ll, normalize(heLight.lightDir[i].xyz));
                    float cosCone = heLight.lightDir[i].w;
                    att *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), cc);
                }
                if (att <= 0.0) continue;
                float vis = heLocalShadowFactor(i, pos, Ll);
                vec3  lc  = heLight.lightColor[i].rgb * heLight.lightColor[i].a;
                S += sigS * lc * (att * vis * fv.misc.x * heFogPhase(fv.scatter.w, dot(dir, Ll)) * fv.pad.z);
            }
            S += sigS * heLight.ambient.rgb * fv.misc.y;     // lightning flash lifts the fog
        }
        cur = vec4(S, sigT);
    }

    // ── Temporal reprojection ────────────────────────────────────────────────
    if (fv.temporal.z > 0.5) {
        vec4 pc = fv.prevViewProj * vec4(pos, 1.0);
        if (pc.w > 1e-3) {
            vec2  ndc = pc.xy / pc.w;
            vec3  hu  = vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, heFogDepthSlice(pc.w));
            if (all(greaterThanEqual(hu, vec3(0.0))) && all(lessThanEqual(hu, vec3(1.0)))) {
                vec4 h = textureLod(heFogHistory, hu, 0.0);
                cur = mix(h, cur, fv.temporal.y);
            }
        }
    }
    imageStore(heFogOut, f, cur);
}
)GLSL";

// ─── Integrate ────────────────────────────────────────────────────────────────
// Self-contained: the column scan is pure maths, no lighting preamble.
inline constexpr const char* kIntegrateSource = R"GLSL(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
)GLSL";

inline constexpr const char* kIntegrateMain = R"GLSL(
layout(set = 0, binding = 37) uniform texture3D heFogInjected;
layout(set = 0, binding = 38, rgba16f) uniform writeonly image3D heFogOut;
void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    ivec3 g  = ivec3(fv.grid.xyz);
    if (xy.x >= g.x || xy.y >= g.y) return;
    vec2  uv   = (vec2(xy) + 0.5) / fv.grid.xy;
    vec3  dir  = heFogRayDir(uv);
    float cosT = max(dot(dir, fv.camFwd.xyz), 1e-3);
    vec3  L = vec3(0.0);
    float T = 1.0;
    float zPrev = 0.0;
    for (int k = 0; k < g.z; ++k) {
        float zNext = heFogSliceDepth(float(k + 1) * fv.grid.w);
        float dz    = (zNext - zPrev) / cosT;            // ray length through this slice
        zPrev = zNext;
        vec4  s     = texelFetch(heFogInjected, ivec3(xy, k), 0);
        float ext   = exp(-s.a * dz);
        vec3  Sint  = (s.rgb - s.rgb * ext) / max(s.a, 1e-5);
        L += T * Sint;
        T *= ext;
        imageStore(heFogOut, ivec3(xy, k), vec4(L, T));
    }
}
)GLSL";

// ─── Apply ────────────────────────────────────────────────────────────────────
inline constexpr const char* kApplySource = R"GLSL(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 34) uniform sampler3D heFogIntegrated;
layout(set = 0, binding = 15) uniform samplerCube heSkyEnv;
layout(set = 0, binding = 39) uniform texture2D heFogDepth;
layout(location = 0) out vec4 oColor;
)GLSL";

inline constexpr const char* kApplyMain = R"GLSL(
void main() {
    ivec2 px   = ivec2(gl_FragCoord.xy);
    vec2  uv   = (vec2(px) + 0.5) / vec2(textureSize(heFogDepth, 0));
    float d    = texelFetch(heFogDepth, px, 0).r;
    bool  sky  = d >= 0.999999;
    // The backend's own depth reconstruction (the underwater pass' line).
    vec4  wp   = fv.invViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, d, 1.0);
    vec3  P    = wp.xyz / max(wp.w, 1e-8);
    vec3  ray  = P - fv.camPos.xyz;
    float dist = max(length(ray), 1e-4);
    vec3  dir  = ray / dist;
    float D    = fv.camPos.w;
    float z    = sky ? D : dist * dot(dir, fv.camFwd.xyz);   // planar depth; the sky sees the whole volume
    float zc   = min(z, D);
    float N    = fv.grid.z;
    float t    = heFogDepthSlice(zc);
    vec4  vol  = textureLod(heFogIntegrated, vec3(uv, max(t - 0.5 / N, 0.5 / N)), 0.0);
    vol = mix(vec4(0.0, 0.0, 0.0, 1.0), vol, min(t * N, 1.0));
    float Tf = 1.0;
    vec3  Lf = vec3(0.0);
    if (!sky && z > D) {
        // Past the volume the analytic height fog continues from where the volume
        // ended: its optical depth over [dv, dist] along the same ray.
        float dv   = D * dist / z;
        float dens = fv.density.x;
        float fall = fv.density.y;
        float b0   = dens * exp(clamp(-fall * fv.camPos.y, -30.0, 30.0));
        float m    = fall * dir.y;
        float opt  = (abs(m) > 1e-4) ? b0 * (exp(-m * dv) - exp(-m * dist)) / m : b0 * (dist - dv);
        Tf = exp(-max(opt, 0.0));
        Lf = textureLod(heSkyEnv, dir, 0.0).rgb * (1.0 - Tf);
    }
    oColor = vec4(vol.rgb + vol.a * Lf, vol.a * Tf);
}
)GLSL";

} // namespace HE::FogShaderSource
