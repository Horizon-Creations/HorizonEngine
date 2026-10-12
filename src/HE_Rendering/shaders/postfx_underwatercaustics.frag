// Underwater caustics post pass (Thema 185 Schritt 3). GLSL twin of HlslSources.h's
// kUnderwaterCausticsHLSL and of the GL/Metal caustic shaders — see the comment above
// kUnderwaterCausticsHLSL for the derivation. Per pixel the depth names a world position
// P; the sun's ray through P, bent by Snell's law, pierces the flat surface plane at S; the
// engine water's own wave trains give the surface's curvature there, and 1/|det| of the
// light's surface->floor mapping is how much it is squeezed. Output is LIGHT for a
// ONE/ONE_MINUS_SRC_ALPHA blend onto the HDR scene colour (rgb = the added radiance, a = 0,
// so dst' = dst + rgb), drawn right BEFORE the fog pass in the same render pass so the fog
// swallows the net with distance like everything else.
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
    vec4 caustics;     // x = intensity (faded in), y = scale, z = wave clock (s), w = fade depth (m)
} pc;

const float kTrains[12] = float[12](20.0, 1.0, 14.0, 0.0700000003, 335.0, 0.800000012, 5.0, 0.100000001, 70.0, 0.5, 1.60000002, 0.0900000036);
const float kTwoPi = 6.28318530718;
const float kEta   = 1.33;

void main()
{
    float d = texture(uDepth, vUV).r;
    vec4 clip = vec4(vUV * 2.0 - 1.0, d, 1.0);   // Vulkan NDC: y down (= uv.y), depth 0..1 (= d)
    vec4 wp = pc.invViewProj * clip;
    vec3 P = wp.xyz / max(wp.w, 1e-8);
    vec3 dPx = dFdx(P);   // before any early-out: derivatives are undefined after divergent flow
    vec3 dPy = dFdy(P);

    float depth = pc.camSurface.w - P.y;                 // metres below the flat surface
    float fadeDepth = pc.caustics.w;
    vec3 L = pc.sun.xyz;
    if (d >= 1.0 || depth <= 0.0 || depth > 5.0 * fadeDepth || L.y <= 0.02)
    {
        outColor = vec4(0.0);                            // sky, dry, too deep, or no sun
        return;
    }

    // The sun's ray inside the water (Snell: the horizontal part shrinks by 1/eta),
    // followed back up to the surface plane.
    vec2  hz   = L.xz / kEta;
    float cosT = sqrt(max(1.0 - dot(hz, hz), 0.0));
    vec3  Lr   = vec3(hz.x, cosT, hz.y);
    vec2  S    = P.xz + hz / max(cosT, 0.1) * depth;
    // The material bends its wave phase with a slow FBM field; a cheap two-sine stand-in
    // does the same here, so the lattice of the plane trains does not read as a grid.
    S += 1.6 * vec2(sin(0.43 * S.y + 0.31 * pc.caustics.z), sin(0.37 * S.x - 0.27 * pc.caustics.z));

    // Curvature of the wave surface at S, summed over the engine water's trains.
    // h = (steepness / k) sin(k (d.p - speed t)); H = -steepness k sin(.) d d^T.
    // Trains 0-2 are the engine water's own; 3-5 are their companions (turned 57 degrees,
    // 0.73x the length, 0.6x the steepness, a touch faster), as in the other backends.
    float mxx = 0.0, mzz = 0.0, mxz = 0.0;
    for (int t = 0; t < 6; ++t)
    {
        int   b    = t % 3;
        float comp = t >= 3 ? 1.0 : 0.0;
        float rad  = radians(kTrains[b * 4] + comp * 57.0);
        vec2  dv   = vec2(cos(rad), sin(rad));
        float k    = kTwoPi / (kTrains[b * 4 + 2] * mix(1.0, 0.73, comp)) * pc.caustics.y;
        float stp  = kTrains[b * 4 + 3] * mix(1.0, 0.6, comp);
        float spd  = kTrains[b * 4 + 1] * mix(1.0, 1.15, comp);
        float c    = stp * k * sin(k * (dot(dv, S) - spd * pc.caustics.z));
        mxx -= c * dv.x * dv.x;
        mzz -= c * dv.y * dv.y;
        mxz -= c * dv.x * dv.y;
    }
    // How far the bend moves the light on its way down; the reference depth added to the
    // real one stands for the fine chop the material shades on top of its trains.
    float kd  = (kEta - 1.0) / kEta * (0.6 * depth + 2.0);
    float det = (1.0 + kd * mxx) * (1.0 + kd * mzz) - kd * kd * mxz * mxz;
    // Only real focusing counts (1/|det| well above 1), graded so a pool has a core and a falloff.
    float x   = max(1.0 / max(abs(det), 0.1) - 2.0, 0.0);
    float net = x / (x + 2.0);

    // Surfaces that face the refracted sun catch it; a wall barely does.
    vec3 n = normalize(cross(dPx, dPy));
    if (dot(n, pc.camSurface.xyz - P) < 0.0) n = -n;
    float ndl   = clamp(dot(n, Lr), 0.0, 1.0);
    float atten = exp(-depth / fadeDepth) * smoothstep(0.0, 0.4, depth);
    outColor = vec4(pc.sunColor.rgb * (pc.caustics.x * net * ndl * atten), 0.0);
}
