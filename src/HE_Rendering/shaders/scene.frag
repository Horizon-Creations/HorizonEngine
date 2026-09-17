#version 450

// ─── DRIFT WARNING: reduced second copy of the GL scene shader ───────────────
// This file is NOT the reference implementation. The engine's lit scene shader
// lives twice:
//
//   reference : src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp
//               `kUnlitFS`      — the lit (misnamed) scene fragment shader
//               `kSkyFuncGLSL`  — the shared analytic sky, spliced in at the
//                                 `//#SKYFUNC#` marker (Metal mirrors both in MSL)
//   this file : a hand-maintained, feature-reduced port for the Vulkan backend
//
// They are edited independently, so the GL/Metal scene shader has moved ahead.
// Verified against the GL source (audit 1a); everything below is present in
// `kUnlitFS` / `kSkyFuncGLSL` and MISSING here:
//
//   * atmoScatter / atmoRaySphere — the single-scattering atmosphere. The
//     `skyColor()` below is the OLD hand-tuned gradient (the same reduced copy
//     as in sky.frag), and it feeds BOTH the ambient IBL and the fog colour, so
//     ambient light and fog tint differ from GL/Metal, not just the background.
//   * (CLOSED, Thema 35 / Schritt 5) Cascaded shadow maps — `cascadeVP` /
//     `cascadeSplits` / `cameraFwd` / `shadowBias` in the Frame block and the
//     `shadowFactor()` below are the GLSL twin of GL's computeShadow(): planar
//     view-Z cascade pick, normal-offset + slope-scaled bias, 3×3 PCF over a
//     sampler2DArray, and the shadowEnabled.y cascade tint. Still MISSING from
//     GL's version: nothing — but the two are hand-kept copies, so a change to
//     GL's computeShadow() is NOT automatically visible here.
//   * (CLOSED, Thema 35 / Schritt 7) Point/spot shadow maps — GL's
//     uLocalShadowMap atlas + uLocalShadowVP + `localShadowFactor()` are here
//     as `uLocalShadowMap` (binding 9) / `localShadowVP` / `localShadowFactor()`,
//     min-combined with the uGILocal mask when GI is active. Hand-kept copy
//     like the cascades above.
//   * uSkyEnv — the baked skyColor cubemap GL samples for ambient diffuse and
//     specular. This file evaluates skyColor() analytically per fragment.
//   * uAmbient — the flat ambient fill (never-black floor / overcast term).
//     RenderWorld::ambient never reaches this shader.
//   * uWetness / uSnow — the weather surface response (wet darken + gloss, snow
//     cover on up-facing surfaces).
//
// Deliberately NOT drift: the GI block (giOctEncode / sampleDDGIIrradiance /
// uGIShadow / uGILocal) is a byte-for-byte port and must stay in sync with
// gi_probe.comp and the GL/Metal originals — and with `kLightingPreamble` in
// src/HE_Rendering/src/material/MaterialShaderLibrary.cpp, which is what graph
// materials are shaded with. tests/test_culling.cpp's "GI kernels: the constants
// the hand-kept copies must share" string-compares this block against that one.
//
// Audit 1a decision: DOCUMENT the drift, do not port. Until it is generated from
// one source, a change to the GL scene shader is NOT automatically visible on
// Vulkan.
//
// CLOSED drift (ssr-plan P4, reversed 2026-09-06 by
// docs/ssr-cross-backend-plan.md §3.3 / checkpoint B5): this file used to weight
// the specular IBL with a flat F0 while heLitP/GL/Metal/D3D11/D3D12 used the
// roughness-aware Schlick Fresnel, and the banner said "document, do not port".
// The SSR cascade below mixes INTO that specular term, so leaving the weight
// different would have made the same mirror change appearance between a
// built-in material and a graph material standing next to it. Both the Fresnel
// and the cascade are ported now — a DELIBERATE change to Vulkan's built-in
// shading, visible even with SSR switched off.
// ─────────────────────────────────────────────────────────────────────────────

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform Frame {
    vec4  cameraPos;
    ivec4 lightCount;
    vec4  lightPos[8];
    vec4  lightDir[8];
    vec4  lightColor[8];
    vec4  lightParams[8];
    // Cascaded shadow maps — must match FrameUBOData exactly: per-cascade
    // light view-proj (kVulkanClipFix pre-applied on the CPU), the planar
    // view-space far distance of each cascade (w = cascade count), the
    // camera forward the pick measures along (w = 1 / shadow map size) and
    // the project ShadowSettings receiver bias (x = slope factor, y = minimum).
    mat4  cascadeVP[3];
    vec4  cascadeSplits;
    vec4  cameraFwd;
    vec4  shadowBias;
    ivec4 shadowEnabled; // x = 0/1, y = 1 → tint fragments by cascade (debug)
    vec4  sunDir;   // xyz = sun direction
    vec4  fog;      // x=fogDensity, y=fogHeightFalloff
    vec4  viewport; // x=W, y=H, z=ssaoEnabled(1.0), w=unused — must match FrameUBOData exactly
    vec4  giGridOrigin; // xyz = probe grid origin, w = spacing
    vec4  giGridCounts; // xyz = probe counts, w = probesPerRow
    vec4  giParams;     // x = indirectIntensity, y = giEnabled(1.0), zw = 0
    // Forward SSR (docs/ssr-cross-backend-plan.md B5): x = 1 → a trace ran and
    // uSSRFwd holds it, y = intensity, z = max roughness, w = 0. Same four lanes
    // heLitP reads out of Lighting::ssr — must match FrameUBOData exactly.
    vec4  ssrParams;
    // Local (point/spot) shadow atlas — must match FrameUBOData exactly: one
    // light view-proj per atlas layer (kVulkanClipFix pre-applied), 16 layers
    // = spot 1 / point 6 cube faces. lightParams[i].y is the light's base
    // layer (-1 = casts no local shadow). localShadowParams.x = 1 / atlas size.
    mat4  localShadowVP[16];
    vec4  localShadowParams;
} uf;

// Directional CSM depth array — one layer per cascade, nearest + clamp-to-
// border(white) sampler: a PCF tap must read ONE texel's depth, and a tap
// that leaves the map reads "lit".
layout(set = 0, binding = 1) uniform sampler2DArray uShadowMap;
// Local (point/spot) shadow atlas — 16-layer depth array, same nearest +
// clamp-to-border(white) sampler as the cascades. Binding 9: next free slot
// after the forward-SSR result on 8.
layout(set = 0, binding = 9) uniform sampler2DArray uLocalShadowMap;

// Per-draw PBR material scalars uploaded via vkCmdUpdateBuffer before each draw.
layout(set = 0, binding = 2) uniform MatUBO {
    vec4 baseColorMet;  // rgb = baseColor, a = metallic
    // x = roughness, y = opacity, z = hasTexture (0/1),
    // w = 1 when the object IGNORES shadows (MeshComponent::receivesShadow ==
    //     false). Inverted on purpose: a fill site that leaves the lane at 0
    //     keeps shadowing rather than silently losing every shadow.
    vec4 roughPad;
} mat_ubo;

// Per-mesh base-color texture (set = 2, binding = 0), bound per draw. Untextured
// meshes bind a 1x1 white default, and roughPad.z = 0 selects the flat colour, so
// sampling is always safe. Set index 2 is shared by the scene + skinned pipelines
// (skinned keeps its bones UBO at set 1).
layout(set = 2, binding = 0) uniform sampler2D uAlbedo;

// SSAO occlusion texture (1x1 white when SSAO is disabled so ao = 1.0).
// Binding 3 must match the scene descriptor set layout in VulkanRenderer.cpp.
layout(set = 0, binding = 3) uniform sampler2D uAO;

// ── Ray-traced GI inputs (software ray tracing, gi_*.comp) ───────────────────
// Half-res screen-space shadow mask + DDGI probe atlases. Bound to 1x1 white
// fallbacks when GI is off (giParams.y == 0 → never sampled). The atlases live
// in VK_IMAGE_LAYOUT_GENERAL (imageStore targets), so their descriptors are
// written with GENERAL — legal for sampling.
layout(set = 0, binding = 4) uniform sampler2D uGIShadow;
layout(set = 0, binding = 5) uniform sampler2D uGIIrr;
layout(set = 0, binding = 6) uniform sampler2D uGIVis;
// Per-pixel local (point/spot) light visibility mask — 1 channel per light
// (first 4, counter over non-directional lights), written by the shadow kernel.
layout(set = 0, binding = 7) uniform sampler2D uGILocal;

// ── Forward screen-space reflections ─────────────────────────────────────────
// The half-res trace result (rgb = reflected radiance, a = confidence), sampled
// per gl_FragCoord like uAO. Bound to a 1x1 TRANSPARENT BLACK when no trace ran
// — ssrParams.x is 0 then, and even a stray sample would add nothing.
layout(set = 0, binding = 8) uniform sampler2D uSSRFwd;

// Signed-octahedral mapping (direction → texel UV) — must match gi_probe.comp's
// octDecode and the GL/Metal octEncode byte-for-byte.
vec2 giOctEncode(vec3 n)
{
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

const int GI_PROBE_OCT = 8; // must match the renderer-side atlas tiling

// DDGI probe sampling — trilinear over the 8 surrounding probes × soft
// backface × Chebyshev visibility. Direct port of the GL/Metal
// sampleDDGIIrradiance (same v1 notes: raw per-direction radiance).
vec3 sampleDDGIIrradiance(vec3 P, vec3 N)
{
    int gx = int(uf.giGridCounts.x), gy = int(uf.giGridCounts.y), gz = int(uf.giGridCounts.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
    int probesPerRow = max(1, int(uf.giGridCounts.w));
    int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
    vec2 atlasSizeTexels = vec2(float(probesPerRow), float(probeRows)) * float(GI_PROBE_OCT);
    float spacing = max(uf.giGridOrigin.w, 1e-4);

    vec3 gridSpace = (P - uf.giGridOrigin.xyz) / spacing;
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

        vec3 probePos   = uf.giGridOrigin.xyz + cell * spacing;
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

// ── Procedural sky ────────────────────────────────────────────────────────────
vec3 skyColor(vec3 dir, vec3 sunDir)
{
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    float sunY = clamp(sunDir.y, -0.2, 1.0);
    float day  = smoothstep(-0.10, 0.10, sunY);
    float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));
    vec3 zenithDay  = vec3(0.08, 0.28, 0.72);
    vec3 horizDay   = vec3(0.42, 0.62, 0.88);
    vec3 zenithNite = vec3(0.003, 0.005, 0.015);
    vec3 horizNite  = vec3(0.006, 0.009, 0.024);
    vec3 zenith  = mix(zenithNite, zenithDay, day);
    vec3 horizon = mix(horizNite,  horizDay,  day);
    vec2  sunAz  = normalize(sunDir.xz + vec2(1e-5));
    float toward = dot(normalize(dir.xz + vec2(1e-5)), sunAz) * 0.5 + 0.5;
    toward = pow(clamp(toward, 0.0, 1.0), 1.5);
    vec3  duskHoriz = mix(vec3(0.52, 0.30, 0.52), vec3(1.20, 0.50, 0.16), toward);
    horizon = mix(horizon, duskHoriz, dusk);
    zenith  = mix(zenith,  vec3(0.20, 0.16, 0.40), dusk * 0.6);
    float h    = clamp(dir.y, 0.0, 1.0);
    float grad = pow(1.0 - h, 2.5);
    vec3 sky = mix(zenith, horizon, grad);
    float band = pow(1.0 - h, 8.0) * toward;
    sky += vec3(1.25, 0.62, 0.26) * (band * dusk * 0.8);
    vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
    sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));
    vec3  sunTint = mix(vec3(1.0, 0.42, 0.20), vec3(1.0, 0.96, 0.88), smoothstep(0.0, 0.25, sunY));
    float s = max(dot(dir, sunDir), 0.0);
    float sunVis = max(day, dusk);
    sky += sunTint * (pow(s, 1800.0) * 14.0) * day;
    sky += sunTint * (pow(s, 180.0)  * 2.2) * sunVis;
    sky += sunTint * (pow(s, 22.0)   * 0.7) * sunVis;
    sky += vec3(1.0, 0.5, 0.25) * (pow(s, 5.0) * 0.5) * dusk;
    float night   = 1.0 - day;
    vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
    float m       = max(dot(dir, moonDir), 0.0);
    sky += vec3(0.80, 0.86, 1.00) * (pow(m, 60.0) * 0.05) * night;
    sky += vec3(0.015, 0.018, 0.030) * night;
    return sky;
}

// ── Cook-Torrance BRDF ────────────────────────────────────────────────────────
const float PI = 3.14159265;
float D_GGX(float NdH, float a2) { float d = NdH*NdH*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float G_Schlick(float NdX, float k) { return NdX/(NdX*(1.0-k)+k); }
vec3  F_Schlick(float VdH, vec3 F0) { return F0+(1.0-F0)*pow(1.0-VdH,5.0); }

vec3 BRDF(vec3 L, vec3 V, vec3 N, vec3 base, float met, float rough)
{
    float a = rough*rough; float a2 = a*a;
    float k = (rough+1.0); k = k*k/8.0;
    vec3  H = normalize(L+V);
    float NdL = max(dot(N,L),0.0);
    float NdV = max(dot(N,V),0.0001);
    float NdH = max(dot(N,H),0.0);
    float VdH = max(dot(V,H),0.0);
    vec3 F0 = mix(vec3(0.04), base, met);
    vec3 F  = F_Schlick(VdH, F0);
    float D = D_GGX(NdH, a2);
    float G = G_Schlick(NdV,k)*G_Schlick(NdL,k);
    vec3 spec = D*F*G / max(4.0*NdV*NdL, 1e-6);
    vec3 kd = (1.0-F)*(1.0-met);
    return (kd*base/PI + spec)*NdL;
}

// Cascaded shadows — the GLSL twin of GL's computeShadow() and of the Metal /
// D3D11 / D3D12 shadowFactor(): pick the first cascade whose far distance
// covers the fragment (by PLANAR camera-forward distance, the same measure the
// extractor split the cascades with — euclidean distance would push screen-edge
// pixels into a too-coarse cascade), project into that cascade's light clip,
// normal-offset + slope-scaled bias scaled by cascade, 3×3 PCF over its layer
// of the depth array. outCascade returns the chosen index for the debug tint.
// UV convention: kVulkanClipFix (y flip + [0,1] depth) is baked into cascadeVP
// and the slices were rendered under the same matrix, so uv = p.xy*0.5+0.5
// with no further flip — exactly the old single-map lookup.
float shadowFactor(vec3 worldPos, vec3 N, vec3 L, out int outCascade)
{
    outCascade = 0;
    if (uf.shadowEnabled.x == 0) return 1.0;
    float viewDist = dot(worldPos - uf.cameraPos.xyz, uf.cameraFwd.xyz);
    int count = int(uf.cascadeSplits.w);
    int c = (count > 0) ? count - 1 : 0;
    if      (count > 0 && viewDist < uf.cascadeSplits.x) c = 0;
    else if (count > 1 && viewDist < uf.cascadeSplits.y) c = 1;
    else if (count > 2 && viewDist < uf.cascadeSplits.z) c = 2;
    c = clamp(c, 0, 2);
    outCascade = c;

    // Normal-offset bias scaled by cascade — coarser (farther) cascades have
    // larger texels and need a bigger offset to avoid acne.
    vec4 lp = uf.cascadeVP[c] * vec4(worldPos + N * (0.06 * float(c + 1)), 1.0);
    vec3 p  = lp.xyz / lp.w;               // z already [0,1] (kVulkanClipFix)
    vec2 uv = p.xy * 0.5 + 0.5;
    float texel = uf.cameraFwd.w;          // 1 / shadow map size
    // Reject one texel inside the border so the 3×3 kernel never reads outside
    // this cascade (border texels → edge fringes).
    if (p.z > 1.0 || any(lessThan(uv, vec2(texel))) || any(greaterThan(uv, vec2(1.0 - texel))))
        return 1.0;
    float ndl  = clamp(dot(N, L), 0.0, 1.0);
    float bias = clamp(uf.shadowBias.x * tan(acos(ndl)), uf.shadowBias.y, 0.02) * float(c + 1);
    float vis = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float cd = texture(uShadowMap, vec3(uv + vec2(x, y) * texel, float(c))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    // No direct-light floor in shadow (GL/Metal/D3D agree): ambient + IBL
    // already provide the indirect minimum; a floor bleeds sun colour into shadow.
    return vis / 9.0;
}

// Point/spot shadow lookup in the local shadow atlas — the GLSL twin of Metal's
// localShadowFactor() and the D3D11/D3D12 function of the same name. Spot
// lights project into their single perspective layer; point lights first pick
// the cube face from the fragment→light vector's major axis (faces stored as 6
// consecutive array layers, +X −X +Y −Y +Z −Z), then project into that face's
// layer. Same 3×3 PCF and normal-offset bias family as the directional CSM
// above; the receiver bias is fixed (the project ShadowSettings pair tunes the
// cascades only). Same UV convention as shadowFactor(): kVulkanClipFix is
// baked into localShadowVP, so uv = p.xy*0.5+0.5 with no further flip.
float localShadowFactor(int i, vec3 worldPos, vec3 N)
{
    // localShadowParams.y = atlas rendered this frame. First gate on purpose:
    // a zero-initialised Frame fill has lightParams.y = 0, which would
    // otherwise read as "layer 0" and sample a never-rendered layer.
    if (uf.localShadowParams.y < 0.5) return 1.0;
    int base = int(uf.lightParams[i].y);
    if (base < 0) return 1.0;
    int layer = base;
    if (int(uf.lightPos[i].w) == 1) // point: major-axis cube-face pick
    {
        vec3 d = worldPos - uf.lightPos[i].xyz;
        vec3 a = abs(d);
        int face;
        if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
        else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
        else                               face = (d.z > 0.0) ? 4 : 5;
        layer = base + face;
    }
    vec3  toL = normalize(uf.lightPos[i].xyz - worldPos);
    float ndl = clamp(dot(N, toL), 0.0, 1.0);
    vec4 lp = uf.localShadowVP[layer] * vec4(worldPos + N * 0.02, 1.0);
    if (lp.w <= 0.0) return 1.0;           // behind the light's near plane
    vec3 p  = lp.xyz / lp.w;               // z in [0,1] (kVulkanClipFix)
    vec2 uv = p.xy * 0.5 + 0.5;
    float texel = uf.localShadowParams.x;  // 1 / atlas size
    if (p.z > 1.0 || p.z < 0.0
        || any(lessThan(uv, vec2(texel))) || any(greaterThan(uv, vec2(1.0 - texel))))
        return 1.0;
    float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
    float vis = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float cd = texture(uLocalShadowMap, vec3(uv + vec2(x, y) * texel, float(layer))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    return vis / 9.0;
}

void main()
{
    // A base-color texture (flagged by roughPad.z) replaces the flat colour, mirroring
    // GL/Metal/D3D11. PBR scalars still come from the material UBO.
    vec3  base  = (mat_ubo.roughPad.z > 0.5) ? texture(uAlbedo, vUV).rgb
                                             : mat_ubo.baseColorMet.rgb;
    float met   = mat_ubo.baseColorMet.a;
    float rough = max(mat_ubo.roughPad.x, 0.04);
    vec3  N     = normalize(vNormal);

    if (uf.lightCount.x == 0)
    {
        vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
        float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        FragColor  = vec4(base * diff, mat_ubo.roughPad.y);
        return;
    }

    vec3 V      = normalize(uf.cameraPos.xyz - vWorldPos);
    vec3 Nup    = normalize(vec3(N.x, max(N.y, 0.1), N.z));
    vec3 Rrough = normalize(mix(reflect(-V, N), N, rough));
    vec3 F0     = mix(vec3(0.04), base, met);
    vec3 kd     = (1.0 - F0) * (1.0 - met);
    vec3 ambDiff = skyColor(Nup,    uf.sunDir.xyz) * base * kd;
    // FORWARD reflection cascade (sky → SSR). SYNC: this is the heLitP twin in
    // MaterialShaderLibrary.cpp and the same stage GL's kUnlitFS and Metal's
    // fragmentMain carry — a graph material next to a built-in one must mix the
    // sources identically, or the same mirror changes appearance with the
    // material type. There is no ray-traced-GI-reflection stage between the two
    // here: this backend has none, and its gate would be a constant zero.
    // The trace carries no per-pixel roughness (the half-res pre-pass has no
    // material data), so the roughness fade lives HERE, with the exact shading
    // value.
    vec3 envSpec = skyColor(Rrough, uf.sunDir.xyz);
    if (uf.ssrParams.x > 0.5)
    {
        vec4  sr   = texture(uSSRFwd, gl_FragCoord.xy / uf.viewport.xy);
        float fade = 1.0 - smoothstep(uf.ssrParams.z * 0.7, uf.ssrParams.z, rough);
        envSpec = mix(envSpec, sr.rgb, sr.a * uf.ssrParams.y * fade);
    }
    // Fresnel (Schlick, roughness-aware, ssr-plan P4): grazing views boost the
    // specular IBL toward max(1-rough, F0) instead of the flat F0. See the
    // banner at the top — this replaced the documented drift.
    float NdV = clamp(dot(N, V), 0.0, 1.0);
    vec3  fresnelSpec = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
    vec3 ambSpec = envSpec * fresnelSpec;
    // AO only darkens the ambient (sky IBL) terms; direct BRDF lights are unaffected.
    // When SSAO is disabled, uf.viewport.z == 0 and we use a 1x1 white fallback texture
    // so ao = 1.0 and the ambient result is unchanged.
    float ao = (uf.viewport.z > 0.5)
        ? texture(uAO, gl_FragCoord.xy / uf.viewport.xy).r
        : 1.0;
    // GI replaces the AO-gated diffuse IBL with probe-sampled indirect diffuse
    // (specular IBL kept — the GI slice is diffuse-only), mirroring GL/Metal.
    vec3 result = (uf.giParams.y > 0.5)
        ? sampleDDGIIrradiance(vWorldPos, N) * base * kd * uf.giParams.x
              + ambSpec * (1.0 - 0.6 * rough)
        : ao * (ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * rough));

    int giLocalIdx = 0; // counter over non-directional lights → local-mask channel
    int dbgCascade = 0; // cascade chosen by the directional shadow (debug tint)
    for (int i = 0; i < uf.lightCount.x; ++i)
    {
        int   type  = int(uf.lightPos[i].w);
        vec3  L;
        float atten = 1.0;
        if (type == 0)
        {
            L = normalize(-uf.lightDir[i].xyz);
        }
        else
        {
            vec3  d    = uf.lightPos[i].xyz - vWorldPos;
            float dist = max(length(d), 1e-4);
            L = d / dist;
            float range = max(uf.lightParams[i].x, 1e-4);
            atten = clamp(1.0 - dist / range, 0.0, 1.0);
            atten *= atten;
            if (type == 2)
            {
                float c       = dot(-L, normalize(uf.lightDir[i].xyz));
                float cosCone = uf.lightDir[i].w;
                atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
            }
        }
        float sh = 1.0;
        if (type == 0)
        {
            // GI replaces the shadow map entirely when active: ray-traced mask
            // sampled at the same screen-space UV convention as uAO.
            if (uf.giParams.y > 0.5) sh = texture(uGIShadow, gl_FragCoord.xy / uf.viewport.xy).r;
            else                     sh = shadowFactor(vWorldPos, N, L, dbgCascade);
        }
        else
        {
            // Local (point/spot) lights: shadow-mapped when the light casts
            // shadows (lightParams.y = atlas base layer, set by the extractor).
            // When GI is active the ray-traced hard mask (first 4 local lights)
            // is combined in via min() — the map covers lights the mask can't.
            // Mirrors Metal/GL/D3D11/D3D12.
            sh = localShadowFactor(i, vWorldPos, N);
            if (uf.giParams.y > 0.5 && giLocalIdx < 4)
                sh = min(sh, texture(uGILocal, gl_FragCoord.xy / uf.viewport.xy)[giLocalIdx]);
            giLocalIdx++;
        }
        // "Receives Shadow" off: the object is lit as if nothing occluded it.
        // After BOTH branches so it covers the shadow map and the GI masks alike.
        if (mat_ubo.roughPad.w > 0.5) sh = 1.0;
        result += BRDF(L, V, N, base, met, rough) * uf.lightColor[i].rgb * uf.lightColor[i].w * atten * sh;
    }
    // Atmospheric fog
    if (uf.fog.x > 0.0) {
        vec3  ray  = vWorldPos - uf.cameraPos.xyz;
        float dist = max(length(ray), 1e-4);
        float k    = uf.fog.y * ray.y;
        float ta   = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
        float opt  = uf.fog.x * dist * exp(-uf.fog.y * uf.cameraPos.y) * ta;
        float f    = 1.0 - exp(-opt);
        vec3  fogCol = skyColor(ray/dist, uf.sunDir.xyz);
        result = mix(result, fogCol, clamp(f, 0.0, 1.0));
    }
    // Debug: tint each fragment by its shadow cascade (red / green / blue /
    // yellow) so the cascade split placement is verifiable at a glance.
    // Mirrors GL, Metal and D3D (IRenderer::SetShadowDebug).
    if (uf.shadowEnabled.y != 0 && uf.shadowEnabled.x != 0)
    {
        const vec3 tint[4] = vec3[4](vec3(1.0, 0.4, 0.4), vec3(0.4, 1.0, 0.4),
                                     vec3(0.4, 0.6, 1.0), vec3(1.0, 1.0, 0.4));
        result *= tint[min(dbgCascade, 3)];
    }
    FragColor = vec4(result, mat_ubo.roughPad.y);
}
