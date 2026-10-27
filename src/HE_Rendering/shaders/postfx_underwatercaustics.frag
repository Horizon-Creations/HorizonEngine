// Underwater caustics post pass (Thema 185 Schritt 3). GLSL twin of HlslSources.h's
// kUnderwaterCausticsHLSL and of the GL/Metal caustic shaders — see the comment above
// kUnderwaterCausticsHLSL for the derivation. Per pixel the depth names a world position
// P; the sun's ray through P, bent by Snell's law, pierces the flat surface plane at S; the
// engine water's own wave trains give the surface's curvature there, and 1/|det| of the
// light's surface->floor mapping is how much it is squeezed. Since Thema 205 Schritt 2 ten
// fine chop trains ride on the engine's six, the focal distance is fixed and the depth only
// fades the net out (the GL twin carries the reasoning). Output is for a
// ONE/ONE_MINUS_SRC_ALPHA blend onto the HDR scene colour (rgb = the light the lines gain,
// a = the share the cells between them lose, so dst' = rgb + dst * (1 - a)), drawn right
// BEFORE the fog pass in the same render pass so the fog swallows the net with distance
// like everything else.
//
// WAVE TRAINS. The D3D/GL/Metal backends splice the engine water's three trains into their
// shader source at run time (HE::causticsWaveTrainsList(), HorizonRendering/
// UnderwaterCausticsShared.h). This file is compiled offline by glslc, so it cannot: the
// twelve numbers below (dirDeg, speed, length, steepness per train, from kWaveA/B/C in
// Physics/WaterWaves.h) are a COPY. Keep them in step by hand: the test "the Vulkan shader's
// wave-train literal is HE::causticsWaveTrainsList()" (tests/test_underwater_camera.cpp)
// reads the kTrains line below and fails when it drifts from the function.
//
// PUSH CONSTANTS: 128 bytes, the most every Vulkan device guarantees (lavapipe has exactly
// that). The HLSL twin carries a 16-byte depthParams too (uv.y sign, ndc-z scale/bias); on
// Vulkan those are the constants 1 / 1 / 0 (the same ones the decal, SSR and deferred-resolve
// reconstructions use), so they are baked into the clip-space line below instead of pushed.
// invViewProj is therefore the inverse of kVulkanClipFix * projection * view — the matrix the
// scene was rasterised with — which is what makes (uv * 2 - 1, depth) a valid Vulkan NDC point.
#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uDepth;
layout(push_constant) uniform PC
{
    mat4 invViewProj;  // inverse(kVulkanClipFix * projection * view)
    vec4 camSurface;   // xyz = camera world pos, w = surface Y
    vec4 sun;          // xyz = direction TOWARD the sun (unit)
    vec4 sunColor;     // rgb = sun radiance (0 at night)
    vec4 caustics;     // x = intensity (faded in), y = scale, z = wave clock (s), w = depth where the net is gone (m)
} pc;

const float kTrains[12] = float[12](20.0, 1.0, 14.0, 0.0700000003, 335.0, 0.800000012, 5.0, 0.100000001, 70.0, 0.5, 1.60000002, 0.0900000036);
const float kTwoPi = 6.28318530718;
const float kEta   = 1.33;
const int   kChopTrains = 10;     // short trains on top of the engine's six (see the GL twin)
const float kChopSteep  = 0.05;
const float kFocal      = 6.0;    // m the bent light travels to the floor, whatever the real depth
const float kGain       = 0.05;   // line brightness per unit of squeeze above 1
const float kMaxLine    = 0.2;    // soft ceiling of a line's light (x sun colour): bright cores bend towards it instead of clipping to white
const float kFineScale  = 1.8;    // the whole net this much finer than the water's own wavelengths: real caustics are small filaments, not metre-wide cells
const float kTimeScale  = 0.75;   // the net's clock runs this much of the water's own: the lines slid too fast against the swell
const float kDark       = 0.3;    // how much of the cells' light the lines take at intensity >= 1
const float kCore       = 0.45;    // share of the cap's squeeze a line must pass to light up (Thema 205 step 6)

void main()
{
    float d = texture(uDepth, vUV).r;
    vec4 clip = vec4(vUV * 2.0 - 1.0, d, 1.0);   // Vulkan NDC: y down (= uv.y), depth 0..1 (= d)
    vec4 wp = pc.invViewProj * clip;
    vec3 P = wp.xyz / max(wp.w, 1e-8);
    vec3 dPx = dFdx(P);   // before any early-out: derivatives are undefined after divergent flow
    vec3 dPy = dFdy(P);

    // Past this one early-out (the same for every pixel) the shader runs for every pixel, sky
    // and dry ones included, and selects the result away at the end: the screen derivatives
    // of S below need uniform flow. A sky pixel's P is parked at the camera.
    vec3 L = pc.sun.xyz;
    if (L.y <= 0.02)
    {
        outColor = vec4(0.0);                            // no sun
        return;
    }
    bool hit = d < 1.0;                                  // the sky has no floor to catch the net
    P = hit ? P : pc.camSurface.xyz;
    float depth = pc.camSurface.w - P.y;                 // metres below the flat surface
    float fadeDepth = pc.caustics.w;

    // The sun's ray inside the water (Snell: the horizontal part shrinks by 1/eta),
    // followed back up to the surface plane.
    vec2  hz   = L.xz / kEta;
    float cosT = sqrt(max(1.0 - dot(hz, hz), 0.0));
    vec3  Lr   = vec3(hz.x, cosT, hz.y);
    vec2  S    = P.xz + hz / max(cosT, 0.1) * depth;
    // The material bends its wave phase with a slow FBM field; a cheap two-sine stand-in
    // does the same here, so the lattice of the plane trains does not read as a grid.
    S += 1.6 * vec2(sin(0.43 * S.y + 0.31 * (pc.caustics.z * kTimeScale)), sin(0.37 * S.x - 0.27 * (pc.caustics.z * kTimeScale)));

    // Curvature of the wave surface at S, summed over the trains.
    // h = (steepness / k) sin(k (d.p - speed t)); H = -steepness k sin(.) d d^T.
    // Trains 0-2 are the engine water's own; 3-5 are their companions (turned 57 degrees,
    // 0.73x the length, 0.6x the steepness, a touch faster); 6+ the fine chop the net's
    // lines come from (1.2 m down to 0.4 m, golden-angle directions), as in the other backends.
    float mxx = 0.0, mzz = 0.0, mxz = 0.0;
    vec2  gxx = vec2(0.0), gzz = vec2(0.0), gxz = vec2(0.0);   // their gradients over S
    for (int t = 0; t < 6 + kChopTrains; ++t)
    {
        float deg, len, stp, spd;
        if (t < 6)
        {
            int   b    = t % 3;
            float comp = t >= 3 ? 1.0 : 0.0;
            deg = kTrains[b * 4] + comp * 57.0;
            len = kTrains[b * 4 + 2] * mix(1.0, 0.73, comp);
            stp = kTrains[b * 4 + 3] * mix(1.0, 0.6, comp);
            spd = kTrains[b * 4 + 1] * mix(1.0, 1.15, comp);
        }
        else
        {
            float j = float(t - 5);
            deg = 20.0 + 137.508 * j;
            len = 1.2 * pow(1.0 / 3.0, (j - 1.0) / float(kChopTrains - 1));
            stp = kChopSteep;
            spd = sqrt(9.81 / kTwoPi * len);
        }
        float rad = radians(deg);
        vec2  dv  = vec2(cos(rad), sin(rad));
        float k   = kTwoPi / len * (pc.caustics.y * kFineScale);
        float ph  = k * (dot(dv, S) - spd * (pc.caustics.z * kTimeScale));
        float c   = stp * k * sin(ph);
        vec2  dc  = (stp * k * k * cos(ph)) * dv;
        mxx -= c * dv.x * dv.x;   gxx -= dc * dv.x * dv.x;
        mzz -= c * dv.y * dv.y;   gzz -= dc * dv.y * dv.y;
        mxz -= c * dv.x * dv.y;   gxz -= dc * dv.x * dv.y;
    }
    // A fixed focal distance, not the real depth: the fold lines are as crisp in a hand's
    // depth of water as further down.
    float kd   = (kEta - 1.0) / kEta * kFocal;
    float det  = (1.0 + kd * mxx) * (1.0 + kd * mzz) - kd * kd * mxz * mxz;
    vec2  gdet = kd * ((1.0 + kd * mzz) * gxx + (1.0 + kd * mxx) * gzz) - 2.0 * kd * kd * mxz * gxz;
    // Depth fade (full in the shallows, gone at fadeDepth, a short ramp at the waterline);
    // the cap on the squeeze softens with depth and keeps the fold lines at least 2 px wide.
    // Only the core of a fold lights up, where J passes kCore of the way to the cap (Thema 205
    // step 6: the whole |det| < 1 band read as lines 26 % of the floor, the photo has 15-18 %):
    // dDet is det's change per pixel (exact gradient over S times the pixel's smooth step in
    // S, floored by that step against the chop's ~10 / m since det's peaks have no gradient).
    // Where 2 px is out of reach the net fades instead of aliasing.
    vec2  dSx   = dFdx(S);
    vec2  dSy   = dFdy(S);
    float dDet  = max(abs(dot(gdet, dSx)) + abs(dot(gdet, dSy)),
                      10.0 * (pc.caustics.y * kFineScale) * (length(dSx) + length(dSy)));
    float tD    = clamp(depth / fadeDepth, 0.0, 1.0);
    float fade  = (1.0 - tD * tD * (3.0 - 2.0 * tD)) * smoothstep(0.0, 0.15, depth);
    float capAA = 2.0 / max(dDet, 1e-4);
    float cap   = min(mix(10.0, 2.0, tD), capAA);
    float J     = 1.0 / max(abs(det), 1.0 / cap);

    // Surfaces that face the refracted sun catch it; a wall barely does.
    vec3 n = normalize(cross(dPx, dPy));
    if (dot(n, pc.camSurface.xyz - P) < 0.0) n = -n;
    float w = pc.caustics.x * clamp(dot(n, Lr), 0.0, 1.0) * fade * smoothstep(1.0, 2.0, capAA);
    vec4 net = vec4(pc.sunColor.rgb * (kMaxLine * (1.0 - exp(-(w * kGain * pow(max(J - 1.0 - kCore * (cap - 1.0), 0.0), 1.5)) / kMaxLine))),
                    kDark * min(w, 1.0) * max(1.0 - J, 0.0));
    outColor = hit ? net : vec4(0.0);
}
