#include "material/MaterialShaderLibrary.h"
#include <cstdint>

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "ShaderCompiler.h" // he::shaderc

#include <functional>

namespace HE
{
namespace
{
// ─── The one geometry-pass vertex assembler ──────────────────────────────────
// The shared standard drop-in vertex (moved out of MetalRenderer), plus the
// world-position-offset variant a node graph's vertex body is spliced into.
// These used to be three hand-written near-copies (two string literals + the
// body of buildCustomVertex), which meant the per-draw binding block and the
// fragment-stage interface had to be edited in three places to stay compatible.
// assembleVertex() emits all of them from one description:
//
//   VertexInput::Ssbo       Metal/Vulkan/D3D: vertex-pulls the interleaved
//                           32-byte VertexIn (pos3, normal3, uv2 = 8 floats) as a
//                           flat std430 float array indexed by gl_VertexIndex
//                           (mirrors the engine's `const device VertexIn*`
//                           binding). SSBOs are GL 4.3+, so this variant is NOT
//                           used for the macOS-GL (4.1) path.
//   VertexInput::Attributes GL-4.1-portable: real vertex attributes, so it
//                           compiles on a GLSL 410 core context. The GL backend
//                           feeds pos/normal/uv via a VAO (locations 0/1/2, the
//                           same interleaved 32-byte layout).
//
// Both read the same Uniforms UBO (std140, matching the engine's per-object
// Uniforms) and emit the same varyings, so a material fragment spliced onto
// either sees the same geometry-pass per-draw bindings and the same
// fragment-stage interface:
//   in  0 vNormal (vec3)  1 vColor (vec3)  2 vUV (vec2)  3 vWorldPos (vec3)
//   out 0 oColor  (vec4)
//
// NOTE ON WHITESPACE: the emitted text is byte-for-byte what the three
// hand-written variants produced, down to their differing `vec4 wp` alignment.
// This is shader SOURCE — a single stray character is a runtime shader-compile
// failure, which no unit test in this repo would catch — so the variants' small
// cosmetic quirks are reproduced rather than tidied up.
enum class VertexInput { Ssbo, Attributes };

// The world-position-offset splice: `declarations` goes between the U block and
// the varyings, `statements` is the graph body (it reads the varyings written
// above it and must leave a `vec3 heWpo`). Null = one of the standard variants.
struct WpoBody
{
    const char*        declarations;
    const std::string& statements;
};

std::string assembleVertex(VertexInput input, const WpoBody* wpo)
{
    const bool ssbo = (input == VertexInput::Ssbo);
    // The WPO variants rebind the attributes to pos/nrm/uv so the graph body and
    // the shared tail below read the same names on either data path.
    const char* pos = (ssbo || wpo) ? "pos" : "aPos";
    const char* nrm = (ssbo || wpo) ? "nrm" : "aNormal";
    const char* uv  = wpo ? "uv" : (ssbo ? "vec2(d[b + 6], d[b + 7])" : "aUV");
    // Alignment run after `vec4 wp` — cosmetic, kept per-variant (see the note above).
    const char* wpPad = wpo ? "   " : (ssbo ? "  " : " ");

    std::string src = "#version 450\n";
    if (ssbo)
        src += "layout(std430, set = 0, binding = 0) readonly buffer Verts { float d[]; };\n";
    else
        src += "layout(location = 0) in vec3 aPos;\n"
               "layout(location = 1) in vec3 aNormal;\n"
               "layout(location = 2) in vec2 aUV;\n";
    src += "layout(std140, set = 0, binding = 1) uniform U {\n"
           "    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;\n"
           "} u;\n";
    if (wpo) src += wpo->declarations;
    src += "layout(location = 0) out vec3 vNormal;\n"
           "layout(location = 1) out vec3 vColor;\n"
           "layout(location = 2) out vec2 vUV;\n"
           "layout(location = 3) out vec3 vWorldPos;\n"
           "void main() {\n";
    // Vertex fetch. The standard variants inline the UV into the vUV assignment;
    // the WPO variants want a named `uv` their body can read.
    if (ssbo)
    {
        src += "    int b = gl_VertexIndex * 8;\n"
               "    vec3 pos = vec3(d[b + 0], d[b + 1], d[b + 2]);\n"
               "    vec3 nrm = vec3(d[b + 3], d[b + 4], d[b + 5]);\n";
        if (wpo) src += "    vec2 uv  = vec2(d[b + 6], d[b + 7]);\n";
    }
    else if (wpo)
        src += "    vec3 pos = aPos;\n    vec3 nrm = aNormal;\n    vec2 uv = aUV;\n";
    src += "    vec4 wp";
    src += wpPad;
    src += "= u.model * vec4(";
    src += pos;
    src += ", 1.0);\n";
    if (!wpo) // the WPO tail writes gl_Position last, from the offset position
    {
        src += "    gl_Position = u.mvp * vec4(";
        src += pos;
        src += ", 1.0);\n";
    }
    src += "    vNormal   = mat3(u.model) * ";
    src += nrm;
    src += ";\n";
    src += "    vColor    = u.color.rgb;\n";
    src += "    vUV       = ";
    src += uv;
    src += ";\n";
    src += "    vWorldPos = wp.xyz;\n";
    if (wpo)
    {
        // The varyings are WRITTEN first so the body may read them by their usual
        // names; the world-space offset is mapped back to object space with the
        // transpose trick (exact for rigid transforms with uniform scale —
        // model^-1 ≈ model^T / |col0|²), so u.mvp keeps working.
        src += wpo->statements; // graph statements → `vec3 heWpo`
        src += "    vWorldPos += heWpo;\n"
               "    vec3 heObjWpo = (transpose(mat3(u.model)) * heWpo)\n"
               "                  / max(dot(u.model[0].xyz, u.model[0].xyz), 1e-8);\n"
               "    gl_Position = u.mvp * vec4(pos + heObjWpo, 1.0);\n";
    }
    src += "}\n";
    return src;
}

// The two standard variants: assembled once, then handed to the compiler as-is.
const std::string& standardVertexSource(VertexInput input)
{
    static const std::string kSsbo   = assembleVertex(VertexInput::Ssbo,       nullptr);
    static const std::string kAttrib = assembleVertex(VertexInput::Attributes, nullptr);
    return (input == VertexInput::Ssbo) ? kSsbo : kAttrib;
}

he::shaderc::Target toTarget(MaterialShaderLibrary::Backend b)
{
    using B = MaterialShaderLibrary::Backend;
    using T = he::shaderc::Target;
    switch (b)
    {
        case B::Metal:     return T::Msl;
        case B::HLSL:      return T::HlslSm50;
        case B::GLSL410:   return T::Glsl410;
        case B::GLSLES300: return T::GlslEs300;
        case B::SpirV:     return T::SpirvBinary;
    }
    return T::Msl;
}

MaterialShaderLibrary::Compiled toCompiled(he::shaderc::Result&& r)
{
    MaterialShaderLibrary::Compiled c;
    c.ok     = r.ok;
    c.source = std::move(r.source);
    c.spirv  = std::move(r.spirv);
    c.log    = std::move(r.log);
    return c;
}

// Standard-lit shader-library preamble, injected into every material fragment (after its
// #version). Provides the lighting UBO (matches HE::MaterialShaderLibrary::Lighting) and
// heLit() — the M2 "Standard Lit" shading a material calls instead of hand-rolling its own.
// Unused by raw fragments (glslang drops the UBO when heLit isn't called), so it's inert
// for the escape-hatch path. UBO (not SSBO) → GL-4.1 portable. Later: the node graph emits
// calls to these std-library functions.
//
// ─── SYNC: this preamble is the 6th copy of the built-in shading maths ───────
// Everything below (heGIIrradianceAt, heOctEncode, heCsmShadow, heApplyFog, the
// heLit* attenuation/PBR terms) is a deliberate hand-port of what each backend's
// built-in PBR shader already does. There is no generator: the copies are:
//
//   OpenGL   src/Backends/OpenGL/OpenGLRenderer.cpp — `kUnlitFS` (the lit scene
//            fragment shader, misnamed) + `GI_PROBE_OCT` / sampleDDGIIrradiance
//   Metal    src/Backends/Metal/MetalRenderer.mm — the scene MSL fragment +
//            `kGIProbeOctSizeShade` / sampleDDGIIrradiance
//   D3D11    src/Backends/D3D11/D3D11Renderer.cpp — `GI_PROBE_OCT` block
//   D3D12    src/Backends/D3D12/D3D12Renderer.cpp — `GI_PROBE_OCT` block
//   Vulkan   src/HE_Rendering/shaders/scene.frag — `GI_PROBE_OCT` /
//            giOctEncode / sampleDDGIIrradiance (that file carries its own
//            drift banner: it is a REDUCED port of the GL shader, but its GI
//            block is byte-for-byte and must stay so)
//
// WHAT BREAKS ON DRIFT: nothing fails to compile and no test elsewhere notices.
// A graph material and a built-in material standing side by side in the same
// scene simply disagree about indirect light / shadow / fog — different probe
// bleed, a shifted cascade edge, a different fog falloff — on the one backend
// that was edited. Reported as "the custom material looks dead next to the
// standard one", which is how the ambient/IBL/SSAO/fog gaps were found before.
//
// GUARD: tests/test_culling.cpp, "GI kernels: the constants the hand-kept copies
// must share" string-compares the shared constants and the heOctEncode /
// sampleDDGIIrradiance blocks of this preamble against scene.frag. It cannot see
// the four copies embedded as C++ string literals above — change one, change all
// six, and run that test.
constexpr const char* kLightingPreamble = R"(
layout(std140, set = 0, binding = 0) uniform HeLighting {
    vec4 sunDir;    // xyz = direction TO the sun (normalized); w = engine time (s)
    vec4 sunColor;  // rgb = sun radiance
    vec4 ambient;   // rgb = ambient / sky fill
    vec4 camPos;    // xyz = camera world position
    vec4 lightPos[8];    // xyz = position, w = type (0 dir / 1 point / 2 spot)
    vec4 lightDir[8];    // xyz = travel direction, w = cos(spot half angle)
    vec4 lightColor[8];  // rgb = colour, w = intensity
    vec4 lightParams[8]; // x = range
    vec4 counts;         // x = light count
    vec4 giParams;       // xy = viewport, z = GI masks valid (heLitP samples them)
    mat4 csmVP[3];       // CSM fallback: per-cascade light view-proj (conventions pre-baked)
    vec4 csmSplits;      // xyz = planar view-space far distances; w = cascade count (0 = off)
    vec4 camFwd;         // xyz = camera forward (planar cascade selection)
    mat4 localShadowVP[16]; // local (point/spot) shadow atlas view-projs (conventions pre-baked)
    vec4 fog;            // x = density, y = height falloff, z = heSkyEnv valid, w = heAO valid
    vec4 giGridOrigin;   // xyz = DDGI probe-grid origin, w = probe spacing
    vec4 giGridCounts;   // xyz = probes per axis, w = probes per atlas row
    vec4 giProbe;        // x = indirect intensity, y = probe atlases bound
    vec4 weather;        // x = wetness, y = snow amount
    vec4 ssr;            // y = SSR intensity, z = max roughness, w = 1 → skip ambSpec (deferred reflection pass)
    vec4 giRefl;         // x = ray-traced GI-reflection intensity (composite pass only), y = max roughness
} heLight;
// Screen-space ray-traced shadow masks (GI): sun visibility (.r) + local-light
// visibility (one channel per the first 4 point/spot lights). Bindings 10/11 —
// 8/9 belong to the WPO custom vertex's UBOs (kWpoUniforms). Bound to 1x1
// white when GI is off; heLight.giParams.z additionally gates the samples.
layout(set = 0, binding = 10) uniform sampler2D heGIShadow;
layout(set = 0, binding = 11) uniform sampler2D heGILocal;
// CSM depth array (binding 12): the SAME cascade shadow map the built-in PBR
// shaders sample, so graph materials are shadowed identically when GI is off.
// Backends without a cascade array bind a dummy and keep csmSplits.w = 0.
layout(set = 0, binding = 12) uniform sampler2DArray heCsm;
// Local (point/spot) shadow atlas (binding 13): the SAME 16-layer depth array
// the built-in PBR shaders sample. A light's base layer is lightParams[i].y-1
// (0 = no shadow), so unfilled Lighting blocks never sample this.
layout(set = 0, binding = 13) uniform sampler2DArray heLocalShadow;
// Procedural sky environment cubemap (binding 15 — 14 is the landscape
// weightmap): the SAME prefiltered sky the built-in PBR shaders use for
// image-based ambient. Graph materials had only a flat ambient constant, so
// they never picked up sky colour or reflections and read visibly "deader"
// than a built-in material beside them. heLight.fog.z gates the samples.
layout(set = 0, binding = 15) uniform samplerCube heSkyEnv;
// Screen-space ambient occlusion result (binding 16), the same SSAO/HBAO/GTAO
// buffer the built-in shaders read. Gated by heLight.fog.w.
layout(set = 0, binding = 16) uniform sampler2D heAO;
// ── DDGI probe irradiance ─────────────────────────────────────────────────────
// Octahedral probe atlases (bindings 17/18): the SAME two textures the built-in
// PBR shaders sample. Graph materials previously had no probe lookup at all — the
// ray-traced GI SHADOW reached them, but the indirect BOUNCE (colour bleeding off
// nearby lit surfaces) did not, so a custom material sat in flat ambient while a
// built-in one beside it picked up the room.
layout(set = 0, binding = 17) uniform sampler2D heGIIrradiance;
layout(set = 0, binding = 18) uniform sampler2D heGIVisibility;
// Signed-octahedral mapping (Meyer et al. 2010), direction -> texel UV. Mirror of
// the backends' octEncode; kept a copy here for the same reason they keep theirs:
// each shader string is its own compilation unit.
vec2 heOctEncode(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}
// Trilinear blend of the 8 probes around P, weighted by a soft backface term and
// a Chebyshev visibility test (suppresses leaking through thin occluders).
// Line-for-line the backends' sampleDDGIIrradiance — any drift here shows up as
// graph materials disagreeing with built-ins about indirect light.
vec3 heGIIrradianceAt(vec3 P, vec3 N) {
    int gx = int(heLight.giGridCounts.x);
    int gy = int(heLight.giGridCounts.y);
    int gz = int(heLight.giGridCounts.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
    const float kOct = 8.0;                 // must match kGIProbeOctSize
    int probesPerRow = max(1, int(heLight.giGridCounts.w));
    int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
    vec2 atlasSizeTexels = vec2(float(probesPerRow), float(probeRows)) * kOct;
    float spacing = max(heLight.giGridOrigin.w, 1e-4);

    vec3 gridSpace = (P - heLight.giGridOrigin.xyz) / spacing;
    vec3 base = floor(gridSpace);
    vec3 frac = gridSpace - base;

    vec3  sumColor  = vec3(0.0);
    float sumWeight = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        vec3 offs = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        vec3 cell = base + offs;
        if (any(lessThan(cell, vec3(0.0))) ||
            cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz)) continue;
        int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

        vec3 trilinear = mix(1.0 - frac, frac, offs);
        float weight = trilinear.x * trilinear.y * trilinear.z;
        if (weight <= 1e-5) continue;

        vec3  probePos   = heLight.giGridOrigin.xyz + cell * spacing;
        vec3  toProbe    = probePos - P;
        float dist       = max(length(toProbe), 1e-4);
        vec3  dirToProbe = toProbe / dist;

        // Soft backface term (never fully zero) — avoids the hard-cutoff seam at
        // the normal's tangent plane.
        weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

        int  tileX = probeIndex % probesPerRow;
        int  tileY = probeIndex / probesPerRow;
        vec2 tileOrigin = vec2(float(tileX), float(tileY)) * kOct;

        vec2 visUV = (tileOrigin + (heOctEncode(-dirToProbe) * 0.5 + 0.5) * kOct) / atlasSizeTexels;
        vec2 visSample = texture(heGIVisibility, visUV).rg;
        float mean = visSample.x, mean2 = visSample.y;
        float variance = abs(mean2 - mean * mean);
        float chebyshev = 1.0;
        if (dist > mean)
        {
            float d = dist - mean;
            chebyshev = variance / (variance + d * d);
            chebyshev = chebyshev * chebyshev * chebyshev; // sharpen, per the DDGI paper
        }
        weight *= max(chebyshev, 0.05);

        vec2 irrUV = (tileOrigin + (heOctEncode(N) * 0.5 + 0.5) * kOct) / atlasSizeTexels;
        sumColor  += texture(heGIIrradiance, irrUV).rgb * weight;
        sumWeight += weight;
    }
    return sumColor / max(sumWeight, 1e-4);
}
// Aerial perspective — mirror of the built-in applyFog(): an analytic
// exponential height-fog integral along the view ray, blended toward the sky in
// that direction. The built-ins evaluate the procedural sky function for the fog
// colour; the env cubemap is generated FROM that same sky, so sampling it along
// the ray reproduces the colour without pulling the sky library into every
// material. Without this, distant custom-material geometry stayed fully
// saturated while everything around it melted into the horizon.
vec3 heApplyFog(vec3 color, vec3 worldPos) {
    if (heLight.fog.x <= 0.0 || heLight.fog.z <= 0.5) return color;
    vec3  ray  = worldPos - heLight.camPos.xyz;
    float dist = length(ray);
    float k    = heLight.fog.y * ray.y;
    float t    = (abs(k) > 1e-4) ? (1.0 - exp(-k)) / k : 1.0; // mean height attenuation
    float optical = heLight.fog.x * dist * exp(-heLight.fog.y * heLight.camPos.y) * t;
    float f = 1.0 - exp(-optical);
    vec3 fogCol = texture(heSkyEnv, ray / max(dist, 1e-4)).rgb;
    return mix(color, fogCol, clamp(f, 0.0, 1.0));
}
// Screen-space GI sun-visibility for the current fragment (1 = fully lit).
// gl_FragCoord-based, so even the legacy worldPos-less heLit() can use it.
float heGISun() {
    if (heLight.giParams.z <= 0.5) return 1.0;
    return texture(heGIShadow, gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0))).r;
}
// CSM visibility — mirror of the built-in shaders' shadowFactor(): planar
// view-distance cascade pick, normal-offset + slope-scaled bias, 3x3 PCF with
// manual depth compare. The fill site pre-bakes clip conventions into csmVP.
float heCsmShadow(vec3 worldPos, vec3 n, vec3 L) {
    int count = int(heLight.csmSplits.w);
    if (count < 1) return 1.0;
    float viewDist = dot(worldPos - heLight.camPos.xyz, heLight.camFwd.xyz);
    int c = count - 1;
    if      (count > 0 && viewDist < heLight.csmSplits.x) c = 0;
    else if (count > 1 && viewDist < heLight.csmSplits.y) c = 1;
    else if (count > 2 && viewDist < heLight.csmSplits.z) c = 2;
    c = clamp(c, 0, 2);
    vec4 lp = heLight.csmVP[c] * vec4(worldPos + n * (0.06 * float(c + 1)), 1.0);
    vec3 p  = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    vec2 texel = 1.0 / vec2(textureSize(heCsm, 0).xy);
    if (p.z > 1.0 || any(lessThan(uv, texel)) || any(greaterThan(uv, vec2(1.0) - texel)))
        return 1.0;
    float ndl  = clamp(dot(n, L), 0.0, 1.0);
    float bias = clamp(0.0008 * tan(acos(ndl)), 0.0002, 0.02) * float(c + 1);
    float vis = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float cd = texture(heCsm, vec3(uv + vec2(x, y) * texel, float(c))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    return vis / 9.0;
}
// Local (point/spot) shadow lookup in the atlas — mirror of the built-in
// shaders' localShadowFactor(). Spot lights project into their single layer;
// point lights pick the cube face from the fragment→light major axis first.
// Clip conventions are pre-baked into localShadowVP by the fill site (like
// csmVP), so this shared GLSL uses uv = p.xy*0.5+0.5 and z in [0,1].
float heLocalShadowFactor(int i, vec3 worldPos, vec3 n) {
    int base = int(heLight.lightParams[i].y) - 1; // stored as layer+1; 0 = none
    if (base < 0) return 1.0;
    int layer = base;
    if (int(heLight.lightPos[i].w) == 1) { // point: major-axis cube-face pick
        vec3 d = worldPos - heLight.lightPos[i].xyz;
        vec3 a = abs(d);
        int face;
        if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
        else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
        else                               face = (d.z > 0.0) ? 4 : 5;
        layer = base + face;
    }
    vec3  toL = normalize(heLight.lightPos[i].xyz - worldPos);
    float ndl = clamp(dot(n, toL), 0.0, 1.0);
    vec4 lp = heLight.localShadowVP[layer] * vec4(worldPos + n * 0.02, 1.0);
    if (lp.w <= 0.0) return 1.0;             // behind the light's near plane
    vec3 p  = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    vec2 texel = 1.0 / vec2(textureSize(heLocalShadow, 0).xy);
    if (p.z > 1.0 || p.z < 0.0
        || any(lessThan(uv, texel)) || any(greaterThan(uv, vec2(1.0) - texel)))
        return 1.0;
    float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
    float vis = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float cd = texture(heLocalShadow, vec3(uv + vec2(x, y) * texel, float(layer))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    return vis / 9.0;
}
// Legacy sun-only shading — kept so precompiled material blobs and hand-written
// escape-hatch fragments that call heLit() keep working unchanged. Has no
// worldPos, so it can't project into the CSM — but the GI sun mask is pure
// screen-space, so ray-traced occlusion applies here too.
vec3 heLit(vec3 baseColor, vec3 N, float metallic, float roughness) {
    vec3  L    = normalize(heLight.sunDir.xyz);
    vec3  n    = normalize(N);
    float ndl  = max(dot(n, L), 0.0);
    float sh   = heGISun();
    // Same metallic/roughness split as heLitP + the built-in PBR shaders.
    float rough = clamp(roughness, 0.0, 1.0);
    float metal = clamp(metallic, 0.0, 1.0);
    vec3  diffuseColor = baseColor * (1.0 - metal);
    vec3  specColor    = mix(vec3(0.04), baseColor, metal);
    vec3  diff = diffuseColor * heLight.sunColor.rgb * ndl;
    vec3  amb  = diffuseColor * heLight.ambient.rgb;
    // cheap roughness-driven spec toward the sun (view ≈ +Z in this simple model)
    vec3  H    = normalize(L + vec3(0.0, 0.0, 1.0));
    float spec = pow(max(dot(n, H), 0.0), mix(128.0, 8.0, rough)) * mix(0.5, 0.03, rough);
    return amb + (diff + heLight.sunColor.rgb * specColor * spec) * sh;
}
// Full-scene-lights shading (M2 Standard Lit v2): ambient + all 8 window lights
// with the SAME attenuation/cone model as the built-in PBR shaders. Needs the
// fragment's world position (attenuation + view vector) — the graph codegen
// passes its vWorldPos varying.
vec3 heLitP(vec3 baseColor, vec3 N, float metallic, float roughness, vec3 worldPos,
            float specular, float ambientOcclusion) {
    vec3 n = normalize(N);
    vec3 V = normalize(heLight.camPos.xyz - worldPos);
    // Metallic/roughness split — IDENTICAL to the built-in PBR shaders (see the
    // OpenGL/Metal scene fragment): a metal has no diffuse lobe and tints its
    // specular with the base colour, a dielectric keeps a neutral 0.04 F0.
    // Graph materials used to ignore `metallic` completely (diffuse was always
    // the full baseColor, specular always white) and ran the roughness curve
    // BACKWARDS — mix(4,64,1-roughness) sharpens the highlight as roughness
    // rises — so a graph material never matched a built-in one beside it.
    // Weather ground response, identical to the built-in shaders: snow settles on
    // up-facing surfaces, wetness darkens and glosses the rest. Without it a
    // graph-material surface stayed bone dry in the middle of a rainstorm while
    // every built-in surface around it went wet.
    float snowMask = smoothstep(0.25, 0.75, clamp(n.y, 0.0, 1.0))
                   * clamp(heLight.weather.y, 0.0, 1.0);
    float wet      = clamp(heLight.weather.x, 0.0, 1.0) * (1.0 - snowMask);
    baseColor = mix(baseColor, vec3(0.90, 0.93, 0.97), snowMask);
    baseColor *= (1.0 - 0.30 * wet);

    float rough = clamp(roughness, 0.0, 1.0);
    rough = mix(rough, 0.08, wet);
    rough = mix(rough, 0.85, snowMask);
    float metal = clamp(metallic, 0.0, 1.0);
    // Specular drives the DIELECTRIC F0 (Unreal's convention: F0 = 0.08 * spec,
    // so the 0.5 default reproduces the built-in shaders' fixed 0.04). Metals
    // ignore it — their F0 is the base colour.
    float f0    = 0.08 * clamp(specular, 0.0, 1.0);
    vec3  diffuseColor = baseColor * (1.0 - metal);
    vec3  specColor    = mix(vec3(f0), baseColor, metal);
    specColor          = mix(specColor, vec3(0.08), wet);   // water-like F0
    float shininess    = mix(128.0, 8.0, rough);
    float specScale    = mix(0.5, 0.03, rough) + 0.25 * wet; // wet sheen
    // Image-based ambient from the procedural sky — the SAME construction as the
    // built-in PBR shaders: diffuse from the normal (clamped above the horizon so
    // a flat surface doesn't pick up the warm sunset band at noon), specular from
    // the reflection bent toward N by roughness as a crude prefilter.
    vec3 ambDiff = diffuseColor * heLight.ambient.rgb;   // fallback (no cubemap)
    vec3 ambSpec = vec3(0.0);
    if (heLight.fog.z > 0.5)
    {
        vec3 Rrough = normalize(mix(reflect(-V, n), n, rough));
        vec3 Nup    = normalize(vec3(n.x, max(n.y, 0.1), n.z));
        ambDiff = texture(heSkyEnv, Nup).rgb    * diffuseColor;
        // Fresnel (Schlick, roughness-aware, ssr-plan P4): grazing views boost
        // the specular IBL toward max(1-rough, F0) instead of the flat specColor.
        float NdV = clamp(dot(n, V), 0.0, 1.0);
        vec3 fresnelSpec = specColor
            + (max(vec3(1.0 - rough), specColor) - specColor) * pow(1.0 - NdV, 5.0);
        ambSpec = texture(heSkyEnv, Rrough).rgb * fresnelSpec * (1.0 - 0.6 * rough);
    }
    // Deferred SSR (docs/ssr-plan.md §4.5): the specular-IBL term moves into a
    // dedicated reflection pass AFTER the resolve — it mixes SSR hits against
    // the same cubemap sample and re-applies the same AO/fog factors. Zeroed
    // HERE (before the GI branch — both branches consume ambSpec) so nothing
    // is counted twice. Forward never sets ssr.w.
    if (heLight.ssr.w > 0.5) ambSpec = vec3(0.0);
    // Occlusion darkens ONLY the indirect term; direct lighting stays untouched,
    // same split as the built-in shaders' SSAO handling. The material's own
    // Ambient Occlusion pin multiplies on top of the screen-space result.
    float ssao = (heLight.fog.w > 0.5)
        ? texture(heAO, gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0))).r : 1.0;
    float ao = clamp(ambientOcclusion, 0.0, 1.0) * ssao;

    // With GI active the probe field REPLACES the sky's diffuse ambient (and AO is
    // bypassed entirely — the probes already carry occlusion); the specular IBL
    // term is kept either way, this GI slice being diffuse-only. Exactly the
    // branch the built-in shaders take. The flat ambient floor stays outside the
    // occlusion in BOTH branches (never-black guarantee).
    vec3 result;
    if (heLight.giProbe.y > 0.5)
        result = heGIIrradianceAt(worldPos, n) * diffuseColor * heLight.giProbe.x
               + ambSpec + heLight.ambient.rgb * diffuseColor;
    else
        result = (ambDiff * 0.35 + ambSpec) * ao + heLight.ambient.rgb * diffuseColor;
    int count = int(heLight.counts.x);
    int localIdx = 0; // counter over non-directional lights → local-mask channel
    for (int i = 0; i < count; ++i) {
        int   type  = int(heLight.lightPos[i].w);
        vec3  L;
        float atten = 1.0;
        if (type == 0) {
            L = normalize(-heLight.lightDir[i].xyz);
        } else {
            vec3  d    = heLight.lightPos[i].xyz - worldPos;
            float dist = max(length(d), 1e-4);
            L = d / dist;
            float range = max(heLight.lightParams[i].x, 1e-4);
            atten = clamp(1.0 - dist / range, 0.0, 1.0);
            atten *= atten;
            if (type == 2) {
                float c       = dot(-L, normalize(heLight.lightDir[i].xyz));
                float cosCone = heLight.lightDir[i].w;
                atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
            }
        }
        // Ray-traced screen-space shadows (GI): directional lights share the
        // temporally-accumulated sun mask, the first 4 local lights read their
        // channel of the hard-shadow mask — same convention as the built-in
        // PBR shaders, so graph materials receive the same shadowing. When the
        // GI masks are absent, directional lights fall back to the cascaded
        // shadow map (heCsmShadow) exactly like the built-in PBR pipeline.
        float sh = 1.0;
        if (heLight.giParams.z > 0.5)
        {
            vec2 giUV = gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0));
            if (type == 0)
            {
                sh = texture(heGIShadow, giUV).r;
            }
            else if (localIdx < 4)
            {
                vec4 lm = texture(heGILocal, giUV);
                sh = lm[localIdx];
            }
        }
        else if (type == 0)
        {
            sh = heCsmShadow(worldPos, n, L);
        }
        // Local (point/spot) lights: shadow-map the atlas layer the extractor
        // assigned (lightParams[i].y). min() with the GI mask above — the map
        // covers lights beyond the mask's first-4 window, the mask adds
        // ray-traced contact hardness when GI is on. Mirrors the built-in PBR.
        if (type != 0)
        {
            sh = min(sh, heLocalShadowFactor(i, worldPos, n));
            localIdx++;
        }
        float ndl  = max(dot(n, L), 0.0);
        vec3  H    = normalize(L + V);
        float spec = pow(max(dot(n, H), 0.0), shininess) * specScale;
        vec3  lc   = heLight.lightColor[i].rgb * heLight.lightColor[i].a;
        result += (diffuseColor * ndl + specColor * spec) * lc * atten * sh;
    }
    return result;
}
// Legacy 5-argument form: material blobs baked before the Specular / Ambient
// Occlusion output pins existed still call this, as do hand-written custom
// fragments. Defaults reproduce the old behaviour exactly (0.5 → F0 0.04).
vec3 heLitP(vec3 baseColor, vec3 N, float metallic, float roughness, vec3 worldPos) {
    return heLitP(baseColor, N, metallic, roughness, worldPos, 0.5, 1.0);
}
)";

// Insert the preamble right after the material's #version directive (GLSL requires
// #version to be the first token). If the source has none, prepend one.
std::string injectPreamble(const std::string& src)
{
    const size_t vpos = src.find("#version");
    if (vpos == std::string::npos)
        return std::string("#version 450\n") + kLightingPreamble + src;
    size_t eol = src.find('\n', vpos);
    if (eol == std::string::npos) eol = src.size() - 1;
    return src.substr(0, eol + 1) + kLightingPreamble + src.substr(eol + 1);
}
} // namespace

namespace
{
// Blocks the WPO body may reference (Time = heLight.sunDir.w, params). Vertex-stage
// bindings 8/9 avoid the fragment slots; Metal pins them to vertex buffers 2/3.
constexpr const char* kWpoUniforms = R"(layout(std140, set = 0, binding = 8) uniform HeLighting {
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 camPos;
} heLight;
layout(std140, set = 0, binding = 9) uniform HeParams { vec4 v[16]; } heParams;
)";

// Noise helpers, duplicated for the vertex stage (the fragment injects its own copies).
// glslang dead-strips whatever the body doesn't call, so including them is free.
constexpr const char* kWpoNoise = R"(float heHash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float heValueNoise(vec2 p) { vec2 i = floor(p); vec2 f = fract(p); vec2 u2 = f * f * (3.0 - 2.0 * f); float a = heHash21(i); float b = heHash21(i + vec2(1.0, 0.0)); float cc = heHash21(i + vec2(0.0, 1.0)); float d = heHash21(i + vec2(1.0, 1.0)); return mix(mix(a, b, u2.x), mix(cc, d, u2.x), u2.y); }
float heFbm(vec2 p) { float v = 0.0; float a = 0.5; for (int i = 0; i < 4; i++) { v += a * heValueNoise(p); p *= 2.0; a *= 0.5; } return v; }
float heHash31(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float heValueNoise3(vec3 p) { vec3 i = floor(p); vec3 f = fract(p); vec3 u3 = f * f * (3.0 - 2.0 * f); float n000 = heHash31(i); float n100 = heHash31(i + vec3(1.0, 0.0, 0.0)); float n010 = heHash31(i + vec3(0.0, 1.0, 0.0)); float n110 = heHash31(i + vec3(1.0, 1.0, 0.0)); float n001 = heHash31(i + vec3(0.0, 0.0, 1.0)); float n101 = heHash31(i + vec3(1.0, 0.0, 1.0)); float n011 = heHash31(i + vec3(0.0, 1.0, 1.0)); float n111 = heHash31(i + vec3(1.0, 1.0, 1.0)); float x00 = mix(n000, n100, u3.x); float x10 = mix(n010, n110, u3.x); float x01 = mix(n001, n101, u3.x); float x11 = mix(n011, n111, u3.x); return mix(mix(x00, x10, u3.y), mix(x01, x11, u3.y), u3.z); }
float heFbm3(vec3 p) { float v = 0.0; float a = 0.5; for (int i = 0; i < 4; i++) { v += a * heValueNoise3(p); p *= 2.0; a *= 0.5; } return v; }
)";

// The WPO variant's extra declarations: the blocks the body may reference, plus
// the noise helpers. Assembled into one string once, since assembleVertex splices
// `declarations` as a single blob.
const char* wpoDeclarations()
{
    static const std::string kDecls = std::string(kWpoUniforms) + kWpoNoise;
    return kDecls.c_str();
}

// Assemble the full canonical custom vertex around the graph body — see
// assembleVertex at the top of this file, which emits this and the two standard
// variants from one description.
std::string buildCustomVertex(const std::string& body, bool ssbo)
{
    const WpoBody wpo{ wpoDeclarations(), body };
    return assembleVertex(ssbo ? VertexInput::Ssbo : VertexInput::Attributes, &wpo);
}
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::customVertex(
    uint64_t bodyHash, const std::string& body, Backend backend)
{
    const uint64_t key = bodyHash ^ (0xC2B2AE3D27D4EB4FULL * (static_cast<uint64_t>(backend) + 1));
    if (auto it = m_cvertCache.find(key); it != m_cvertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // verts@0, U@1 (the geometry pass's fixed binds) + HeLighting/HeParams pinned to
        // vertex buffers 2/3, which the renderer binds for WPO materials.
        out = toCompiled(compileMslPinned(buildCustomVertex(body, /*ssbo=*/true), Stage::Vertex,
            { { Stage::Vertex, 0, 0, 0 }, { Stage::Vertex, 0, 1, 1 },
              { Stage::Vertex, 0, 8, 2 }, { Stage::Vertex, 0, 9, 3 } }));
    }
    else
    {
        out = toCompiled(compile(buildCustomVertex(body, /*ssbo=*/false), Stage::Vertex,
                                 toTarget(backend)));
    }
    return m_cvertCache.emplace(key, std::move(out)).first->second;
}

bool MaterialShaderLibrary::resolveShaders(const ContentManager& cm, const UUID& materialId,
                                           uint64_t& hashOut, std::string& fragOut,
                                           std::string& vertBodyOut) const
{
    if (!resolveFragment(cm, materialId, hashOut, fragOut)) return false;
    vertBodyOut.clear();
    if (const MaterialAsset* mat = cm.getMaterial(materialId))
        vertBodyOut = mat->customShaderVertGlsl;
    if (!vertBodyOut.empty()) // fold the vertex into the pipeline key
        hashOut ^= std::hash<std::string>{}(vertBodyOut) * 0x9E3779B97F4A7C15ULL;
    return true;
}

bool MaterialShaderLibrary::resolveGBufferShaders(const ContentManager& cm, const UUID& materialId,
                                                  uint64_t& hashOut, std::string& fragOut,
                                                  std::string& vertBodyOut) const
{
    if (materialId == UUID{}) return false;
    const MaterialAsset* mat = cm.getMaterial(materialId);
    // Only materials that HAVE a forward custom shader get a G-buffer variant —
    // a material without one renders built-in PBR, which the deferred path
    // covers with its own built-in G-buffer pipeline.
    if (!mat || mat->customShaderFragGlsl.empty() || mat->customShaderGBufGlsl.empty())
        return false;
    fragOut = mat->customShaderGBufGlsl;
    hashOut = std::hash<std::string>{}(fragOut);
    vertBodyOut = mat->customShaderVertGlsl;
    if (!vertBodyOut.empty()) // fold the vertex into the pipeline key (like resolveShaders)
        hashOut ^= std::hash<std::string>{}(vertBodyOut) * 0x9E3779B97F4A7C15ULL;
    return true;
}

bool MaterialShaderLibrary::resolveFragment(const ContentManager& cm, const UUID& materialId,
                                            uint64_t& hashOut, std::string& glslOut) const
{
    if (materialId == UUID{}) return false;
    const MaterialAsset* mat = cm.getMaterial(materialId);
    if (!mat || mat->customShaderFragGlsl.empty()) return false;
    glslOut = mat->customShaderFragGlsl;
    hashOut = std::hash<std::string>{}(glslOut);
    return true;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::standardVertex(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_vertCache.find(key); it != m_vertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // SSBO vertex-pull, pinned so the vertex buffer lands at [[buffer(0)]] and Uniforms
        // at [[buffer(1)]] — the exact bind points the Metal geometry loop issues per draw.
        out = toCompiled(compileMslPinned(standardVertexSource(VertexInput::Ssbo), Stage::Vertex,
            { { Stage::Vertex, 0, 0, 0 }, { Stage::Vertex, 0, 1, 1 } }));
    }
    else
    {
        // GL/D3D/Vulkan: attribute-based vertex so macOS-GL (4.1, no SSBO) can compile it.
        out = toCompiled(compile(standardVertexSource(VertexInput::Attributes), Stage::Vertex,
                                 toTarget(backend)));
    }
    return m_vertCache.emplace(key, std::move(out)).first->second;
}

namespace
{
// Attribute-less screen-space quad vertex for materials on in-game UI quads.
// Same varyings + U bind point as the standard vertex, so any cached material
// fragment links against it unchanged (see the header for the U field layout).
constexpr const char* kUIVertex = R"(#version 450
layout(std140, set = 0, binding = 1) uniform U {
    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;
} u;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
void main() {
    vec2 c[4] = vec2[](vec2(0.0,0.0), vec2(1.0,0.0), vec2(0.0,1.0), vec2(1.0,1.0));
    vec2 corner = c[gl_VertexIndex];
    vec4 rect = u.model[0];
    vec4 uvr  = u.model[1];
    vec2 vp   = max(u.model[2].xy, vec2(1.0));
    vec2 sp   = rect.xy + corner * rect.zw;
    gl_Position = vec4(sp.x / vp.x * 2.0 - 1.0,
                       1.0 - sp.y / vp.y * 2.0, 0.0, 1.0);
    vNormal   = vec3(0.0, 0.0, 1.0);
    vColor    = u.color.rgb;
    vUV       = mix(uvr.xy, uvr.zw, corner);
    vWorldPos = vec3(sp, 0.0);
}
)";
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::uiVertex(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_uiVertCache.find(key); it != m_uiVertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // Pin U to vertex buffer 1 — the same slot the mesh path uses, so the
        // UI pass binds its repurposed U block at a familiar index.
        out = toCompiled(compileMslPinned(kUIVertex, Stage::Vertex,
            { { Stage::Vertex, 0, 1, 1 } }));
    }
    else
    {
        out = toCompiled(compile(kUIVertex, Stage::Vertex, toTarget(backend)));
    }
    return m_uiVertCache.emplace(key, std::move(out)).first->second;
}

namespace
{
// ─── Deferred lighting resolve ───────────────────────────────────────────────
// Fullscreen fragment that reconstructs the surface from the G-buffer and calls
// the SAME heLitP / heApplyFog the material fragments use — the preamble is
// injected by injectPreamble like for every material, so there is exactly ONE
// shading implementation per backend and the two paths cannot drift apart
// (docs/deferred-renderer-plan.md §4.2/§8).
//
// uv comes from gl_FragCoord / heLight.giParams.xy (the viewport, already part
// of the lighting ABI); world position from the sampled depth via HeResolve.
// Backend clip conventions (uv origin, NDC z range) are folded into
// HeResolve.depthParams by the fill site, so this canonical GLSL stays
// convention-free — same trick as csmVP/localShadowVP.
// Assembled by buildDeferredResolveSource below — one text for four variants
// ({sampled, tile} × {8-light, clustered}), so the shared math can never drift
// between them.
std::string buildDeferredResolveSource(bool tile, bool clustered)
{
    std::string src = "#version 450\n";
    // Output: attachment 0 (own pass) or attachment 4 (single pass, behind the
    // four G-buffer attachments).
    src += tile ? "layout(location = 4) out vec4 oColor;\n"
                : "layout(location = 0) out vec4 oColor;\n";
    if (tile)
        src +=
            "layout(input_attachment_index = 0, set = 0, binding = 19) uniform subpassInput heGB0;\n"
            "layout(input_attachment_index = 1, set = 0, binding = 20) uniform subpassInput heGB1;\n"
            "layout(input_attachment_index = 2, set = 0, binding = 21) uniform subpassInput heGB2;\n"
            "layout(input_attachment_index = 3, set = 0, binding = 22) uniform subpassInput heGBDepth;\n";
    else
        src +=
            "layout(set = 0, binding = 19) uniform sampler2D heGB0;     // rgb BaseColor, a Metallic\n"
            "layout(set = 0, binding = 20) uniform sampler2D heGB1;     // rg oct normal, b Roughness, a Specular\n"
            "layout(set = 0, binding = 21) uniform sampler2D heGB2;     // rgb Emissive (HDR), a Material-AO\n"
            "layout(set = 0, binding = 22) uniform sampler2D heGBDepth; // scene depth (world-pos reconstruction)\n";
    src += R"(layout(std140, set = 0, binding = 23) uniform HeResolve {
    mat4 invViewProj;
    vec4 depthParams;   // x = uv→NDC y sign, y/z = NDC-z scale/bias, w = debug view
    vec4 clusterParams; // x/y/z = cluster grid dims, w = gridZ / log(far/near)
    vec4 clusterCamFwd; // xyz = camera forward (view-z), w = cluster near plane
} heResolve;
// Inverse of the preamble's heOctEncode (signed octahedral, Meyer et al. 2010).
// Lives here rather than in the preamble so the drift-guarded preamble blocks
// stay byte-identical to the backends' copies.
vec3 heOctDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
)";
    if (clustered)
        src += R"(// ── Clustered lighting (plan P7) ─────────────────────────────────────────────
// All point/spot lights live in per-cluster lists; heLight's window carries
// ONLY directional lights in this variant. 4 vec4 per light: posType, dirSpot,
// colorIntensity (w = intensity), params (x = range, y = atlas layer + 1).
layout(std430, set = 0, binding = 24) readonly buffer HeClusterLights { vec4 clLights[]; };
layout(std430, set = 0, binding = 25) readonly buffer HeClusterGrid   { uvec2 clGrid[]; };
layout(std430, set = 0, binding = 26) readonly buffer HeClusterIdx    { uint clIdx[]; };
// Local (point/spot) atlas shadow with EXPLICIT light data — line-for-line the
// preamble's heLocalShadowFactor, which indexes the 8-light window instead.
float heClusterShadow(vec4 posType, vec4 params, vec3 worldPos, vec3 n) {
    int base = int(params.y) - 1; // stored as layer+1; 0 = none
    if (base < 0) return 1.0;
    int layer = base;
    if (int(posType.w) == 1) { // point: major-axis cube-face pick
        vec3 d = worldPos - posType.xyz;
        vec3 a = abs(d);
        int face;
        if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
        else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
        else                               face = (d.z > 0.0) ? 4 : 5;
        layer = base + face;
    }
    vec3  toL = normalize(posType.xyz - worldPos);
    float ndl = clamp(dot(n, toL), 0.0, 1.0);
    vec4 lp = heLight.localShadowVP[layer] * vec4(worldPos + n * 0.02, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 p  = lp.xyz / lp.w;
    vec2 suv = p.xy * 0.5 + 0.5;
    vec2 texel = 1.0 / vec2(textureSize(heLocalShadow, 0).xy);
    if (p.z > 1.0 || p.z < 0.0
        || any(lessThan(suv, texel)) || any(greaterThan(suv, vec2(1.0) - texel)))
        return 1.0;
    float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
    float vis = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float cd = texture(heLocalShadow, vec3(suv + vec2(x, y) * texel, float(layer))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    return vis / 9.0;
}
// Point/spot shading over this fragment's cluster list. The weather/material
// split and the per-light model mirror heLitP's loop body exactly — the
// clustered-off A/B is the guard. GI local ray masks are not applied to
// cluster lights (v1 limitation; the atlas shadow still is).
vec3 heClusterLighting(vec3 P, vec3 Nin, vec3 baseColor, float metallic,
                       float roughness, float specular, vec2 uv) {
    vec3 n = normalize(Nin);
    vec3 V = normalize(heLight.camPos.xyz - P);
    float snowMask = smoothstep(0.25, 0.75, clamp(n.y, 0.0, 1.0))
                   * clamp(heLight.weather.y, 0.0, 1.0);
    float wet      = clamp(heLight.weather.x, 0.0, 1.0) * (1.0 - snowMask);
    vec3 bc = mix(baseColor, vec3(0.90, 0.93, 0.97), snowMask);
    bc *= (1.0 - 0.30 * wet);
    float rough = clamp(roughness, 0.0, 1.0);
    rough = mix(rough, 0.08, wet);
    rough = mix(rough, 0.85, snowMask);
    float metal = clamp(metallic, 0.0, 1.0);
    float f0    = 0.08 * clamp(specular, 0.0, 1.0);
    vec3  diffuseColor = bc * (1.0 - metal);
    vec3  specColor    = mix(vec3(f0), bc, metal);
    specColor          = mix(specColor, vec3(0.08), wet);
    float shininess    = mix(128.0, 8.0, rough);
    float specScale    = mix(0.5, 0.03, rough) + 0.25 * wet;

    float nearZ = max(heResolve.clusterCamFwd.w, 1e-4);
    float viewZ = max(dot(P - heLight.camPos.xyz, heResolve.clusterCamFwd.xyz), nearZ);
    int gx = int(heResolve.clusterParams.x);
    int gy = int(heResolve.clusterParams.y);
    int gz = int(heResolve.clusterParams.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return vec3(0.0);
    int cz = clamp(int(log(viewZ / nearZ) * heResolve.clusterParams.w), 0, gz - 1);
    int cx = clamp(int(uv.x * float(gx)), 0, gx - 1);
    int cy = clamp(int(uv.y * float(gy)), 0, gy - 1);
    uvec2 cell = clGrid[(cz * gy + cy) * gx + cx];
    vec3 result = vec3(0.0);
    for (uint k = 0u; k < cell.y; ++k) {
        uint li = clIdx[cell.x + k] * 4u;
        vec4 posType = clLights[li + 0u];
        vec4 dirSpot = clLights[li + 1u];
        vec4 colInt  = clLights[li + 2u];
        vec4 params  = clLights[li + 3u];
        vec3  d    = posType.xyz - P;
        float dist = max(length(d), 1e-4);
        vec3  L = d / dist;
        float range = max(params.x, 1e-4);
        float atten = clamp(1.0 - dist / range, 0.0, 1.0);
        atten *= atten;
        if (posType.w > 1.5) { // spot cone
            float c       = dot(-L, normalize(dirSpot.xyz));
            float cosCone = dirSpot.w;
            atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
        }
        if (atten <= 0.0) continue;
        float sh = heClusterShadow(posType, params, P, n);
        // Ray-traced GI local mask: params.z carries this light's channel + 1
        // (assigned by the CPU scatter with heLitP's exact first-4-of-window
        // scan), min()-combined with the atlas shadow like heLitP's loop.
        if (heLight.giParams.z > 0.5) {
            int ch = int(params.z) - 1;
            if (ch >= 0) {
                vec4 lm = texture(heGILocal, uv);
                sh = min(sh, lm[ch]);
            }
        }
        float ndl  = max(dot(n, L), 0.0);
        vec3  H    = normalize(L + V);
        float spec = pow(max(dot(n, H), 0.0), shininess) * specScale;
        result += (diffuseColor * ndl + specColor * spec) * colInt.rgb * colInt.a * atten * sh;
    }
    return result;
}
)";
    src += "void main() {\n"
           "    vec2 uv = gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0));\n";
    if (tile)
        src += "    float d = subpassLoad(heGBDepth).r;\n"
               "    if (d >= 1.0) discard;                          // background → the sky pass fills it\n"
               "    vec4 g0 = subpassLoad(heGB0);\n"
               "    vec4 g1 = subpassLoad(heGB1);\n"
               "    vec4 g2 = subpassLoad(heGB2);\n";
    else
        src += "    float d = texture(heGBDepth, uv).r;\n"
               "    if (d >= 1.0) discard;                          // background → the sky pass fills it\n"
               "    vec4 g0 = texture(heGB0, uv);\n"
               "    vec4 g1 = texture(heGB1, uv);\n"
               "    vec4 g2 = texture(heGB2, uv);\n";
    src += R"(    vec4 clip = vec4(uv.x * 2.0 - 1.0,
                     (uv.y * 2.0 - 1.0) * heResolve.depthParams.x,
                     d * heResolve.depthParams.y + heResolve.depthParams.z, 1.0);
    vec4 wp = heResolve.invViewProj * clip;
    vec3 P  = wp.xyz / max(wp.w, 1e-8);
    vec3 N  = heOctDecode(g1.rg * 2.0 - 1.0);
    int dbg = int(heResolve.depthParams.w);
    if (dbg == 1) { oColor = vec4(g0.rgb, 1.0); return; }               // BaseColor
    if (dbg == 2) { oColor = vec4(N * 0.5 + 0.5, 1.0); return; }        // Normal
    if (dbg == 3) { oColor = vec4(g1.b, g1.a, g0.a, 1.0); return; }     // Rough/Spec/Metal
    if (dbg == 4) { oColor = vec4(g2.rgb, 1.0); return; }               // Emissive
    vec3 lit = heLitP(g0.rgb, N, g0.a, g1.b, P, g1.a, g2.a);
)";
    if (clustered)
        src += "    lit += heClusterLighting(P, N, g0.rgb, g0.a, g1.b, g1.a, uv);\n";
    src += "    oColor = vec4(heApplyFog(lit + g2.rgb, P), 1.0);\n"
           "}\n";
    return src;
}

// Fullscreen triangle with NO varyings — the resolve fragment reads gl_FragCoord,
// so nothing needs to cross the stage boundary.
constexpr const char* kFullscreenVS = R"(#version 450
void main() {
    float x = float((gl_VertexIndex & 1) << 2) - 1.0;
    float y = float((gl_VertexIndex & 2) << 1) - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)";
} // namespace

namespace
{
// Shared compile for the four resolve variants. Metal pins: everything the
// preamble binds on its scene-pass slots, HeResolve on fragment buffer 3, the
// G-buffer as texture slots 0..3 (sampled variant only — the tile variant
// framebuffer-fetches [[color(0..3)]] and consumes no samplers), and the
// clustered SSBOs on fragment buffers 4/5/6. Total samplers stay well inside
// Metal's 16.
MaterialShaderLibrary::Compiled compileResolveVariant(
    const std::string& injected, MaterialShaderLibrary::Backend backend,
    bool tile, bool clustered)
{
    using namespace he::shaderc;
    using Backend = MaterialShaderLibrary::Backend;
    if (backend != Backend::Metal)
    {
        // GL keeps the sampled non-clustered resolve (no SSBOs in GL 4.1, no
        // framebuffer fetch); the tile/clustered variants are Metal-only.
        if (tile || clustered) return {};
        return toCompiled(compile(injected, Stage::Fragment, toTarget(backend)));
    }
    std::vector<MslPin> pins = {
        { Stage::Fragment, 0, 0, static_cast<uint32_t>(MaterialShaderLibrary::kMetalLightingBufferIndex) },
        { Stage::Fragment, 0, 23, 3 },    // HeResolve UBO → fragment buffer 3
        { Stage::Fragment, 0, 10, 9 },    // GI sun mask (as in fragment())
        { Stage::Fragment, 0, 11, 10 },   // GI local mask
        { Stage::Fragment, 0, 12, 11 },   // CSM array
        { Stage::Fragment, 0, 13, 12 },   // local shadow atlas
        { Stage::Fragment, 0, 15, 14 },   // sky env cubemap
        { Stage::Fragment, 0, 16, 15 },   // screen-space AO
        { Stage::Fragment, 0, 17, 6 },    // DDGI irradiance atlas
        { Stage::Fragment, 0, 18, 7 },    // DDGI visibility atlas
    };
    if (!tile)
    {
        pins.push_back({ Stage::Fragment, 0, 19, 0 }); // heGB0 → texture/sampler 0
        pins.push_back({ Stage::Fragment, 0, 20, 1 }); // heGB1 → 1
        pins.push_back({ Stage::Fragment, 0, 21, 2 }); // heGB2 → 2
        pins.push_back({ Stage::Fragment, 0, 22, 3 }); // heGBDepth → 3
    }
    if (clustered)
    {
        pins.push_back({ Stage::Fragment, 0, 24, 4 }); // light array → buffer 4
        pins.push_back({ Stage::Fragment, 0, 25, 5 }); // cluster grid → buffer 5
        pins.push_back({ Stage::Fragment, 0, 26, 6 }); // light index list → buffer 6
    }
    MslOptions opts;
    opts.framebufferFetchSubpasses = tile;
    return toCompiled(compileMslPinned(injected, Stage::Fragment, pins, opts));
}
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::deferredResolve(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_resolveCache.find(key); it != m_resolveCache.end()) return it->second;
    Compiled out = compileResolveVariant(
        injectPreamble(buildDeferredResolveSource(/*tile=*/false, /*clustered=*/false)),
        backend, false, false);
    return m_resolveCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::deferredResolveClustered(Backend backend)
{
    const int key = static_cast<int>(backend) + 64;
    if (auto it = m_resolveCache.find(key); it != m_resolveCache.end()) return it->second;
    Compiled out = compileResolveVariant(
        injectPreamble(buildDeferredResolveSource(/*tile=*/false, /*clustered=*/true)),
        backend, false, true);
    return m_resolveCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::deferredResolveTile(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_resolveTileCache.find(key); it != m_resolveTileCache.end()) return it->second;
    Compiled out = compileResolveVariant(
        injectPreamble(buildDeferredResolveSource(/*tile=*/true, /*clustered=*/false)),
        backend, true, false);
    return m_resolveTileCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::deferredResolveTileClustered(Backend backend)
{
    const int key = static_cast<int>(backend) + 64;
    if (auto it = m_resolveTileCache.find(key); it != m_resolveTileCache.end()) return it->second;
    Compiled out = compileResolveVariant(
        injectPreamble(buildDeferredResolveSource(/*tile=*/true, /*clustered=*/true)),
        backend, true, true);
    return m_resolveTileCache.emplace(key, std::move(out)).first->second;
}

namespace
{
// ─── SSR trace (docs/ssr-plan.md §4.2, deferred variant of §4.5) ─────────────
// World-space linear march + binary refine against the G-buffer depth; the hit
// samples the CURRENT frame's resolved HDR colour — no history, no reprojection,
// no ghosting. Confidence folds edge fade × facing × roughness fade × distance.
// Standalone canonical GLSL (no lighting preamble — nothing here shades).
constexpr const char* kSSRTraceFS = R"(#version 450
layout(location = 0) out vec4 oSSR;
layout(set = 0, binding = 19) uniform sampler2D heSceneColor; // resolved HDR (opaque, pre-sky)
layout(set = 0, binding = 20) uniform sampler2D heGB1;        // rg oct normal, b roughness
layout(set = 0, binding = 22) uniform sampler2D heGBDepth;    // NDC depth (R32F G-buffer target)
layout(std140, set = 0, binding = 23) uniform HeSSRTrace {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 camPos;  // xyz camera world position
    vec4 camFwd;  // xyz camera forward
    vec4 cfg;     // x maxDistance, y thickness, z maxRoughness, w stepCount
    vec4 conv;    // x ndc-y sign, y depth scale, z depth bias, w edge-fade width
    vec4 vp;      // xy = trace-target size in pixels
} heSSR;
vec3 heOctDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
float heIgn(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }
vec3 heWorldAt(vec2 uv, float d) {
    vec4 c = vec4(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * heSSR.conv.x,
                  d * heSSR.conv.y + heSSR.conv.z, 1.0);
    vec4 w = heSSR.invViewProj * c;
    return w.xyz / max(w.w, 1e-8);
}
float heViewZ(vec3 p) { return dot(p - heSSR.camPos.xyz, heSSR.camFwd.xyz); }
void main() {
    vec2 uv = gl_FragCoord.xy / max(heSSR.vp.xy, vec2(1.0));
    float d = texture(heGBDepth, uv).r;
    if (d >= 1.0) { oSSR = vec4(0.0); return; }             // background
    vec4 g1 = texture(heGB1, uv);
    float rough = clamp(g1.b, 0.0, 1.0);
    float roughFade = 1.0 - smoothstep(heSSR.cfg.z * 0.7, heSSR.cfg.z, rough);
    if (roughFade <= 0.0) { oSSR = vec4(0.0); return; }
    vec3 P = heWorldAt(uv, d);
    vec3 N = heOctDecode(g1.rg * 2.0 - 1.0);
    vec3 V = normalize(P - heSSR.camPos.xyz);
    vec3 R = reflect(V, N);
    // Rays back toward the camera cannot be resolved in screen space.
    float facing = smoothstep(0.0, 0.2, dot(R, heSSR.camFwd.xyz));
    if (facing <= 0.0) { oSSR = vec4(0.0); return; }
    int   steps   = int(heSSR.cfg.w);
    float maxDist = heSSR.cfg.x;
    float dt      = maxDist / float(steps);
    float t       = dt * (0.5 + heIgn(gl_FragCoord.xy));    // jittered start
    float prevT   = 0.0;
    vec2  hitUV   = vec2(-1.0);
    bool  hit     = false;
    for (int i = 0; i < steps; ++i)
    {
        vec3 q = P + R * t;
        vec4 clip = heSSR.viewProj * vec4(q, 1.0);
        if (clip.w <= 0.0) break;
        vec3 ndc = clip.xyz / clip.w;
        vec2 quv = vec2(ndc.x * 0.5 + 0.5, (ndc.y * heSSR.conv.x) * 0.5 + 0.5);
        if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
        float sceneD = texture(heGBDepth, quv).r;
        if (sceneD < 1.0)
        {
            float rayZ   = heViewZ(q);
            float sceneZ = heViewZ(heWorldAt(quv, sceneD));
            if (rayZ > sceneZ + 0.01)
            {
                // Fell behind geometry — thickness test, then binary refine.
                if (rayZ - sceneZ < heSSR.cfg.y + dt)
                {
                    float t0 = prevT, t1 = t;
                    for (int b = 0; b < 5; ++b)
                    {
                        float tm = 0.5 * (t0 + t1);
                        vec3 qm = P + R * tm;
                        vec4 cm = heSSR.viewProj * vec4(qm, 1.0);
                        vec3 nm = cm.xyz / max(cm.w, 1e-6);
                        vec2 um = vec2(nm.x * 0.5 + 0.5, (nm.y * heSSR.conv.x) * 0.5 + 0.5);
                        float dm = texture(heGBDepth, um).r;
                        if (heViewZ(qm) > heViewZ(heWorldAt(um, dm))) { t1 = tm; hitUV = um; }
                        else t0 = tm;
                    }
                    if (hitUV.x < 0.0) hitUV = quv;
                    hit = true;
                }
                break;
            }
        }
        prevT = t;
        t += dt;
    }
    if (!hit) { oSSR = vec4(0.0); return; }
    vec2 ef = min(hitUV, vec2(1.0) - hitUV);
    float edge     = smoothstep(0.0, max(heSSR.conv.w, 1e-3), min(ef.x, ef.y));
    float distFade = 1.0 - 0.5 * clamp(t / maxDist, 0.0, 1.0);
    oSSR = vec4(texture(heSceneColor, hitUV).rgb, edge * facing * roughFade * distFade);
}
)";

// ─── SSR composite (docs/ssr-plan.md §4.5) ───────────────────────────────────
// Additive fullscreen pass that supplies the specular-IBL term the resolve
// skipped (heLight.ssr.w): sky cubemap mixed against the SSR hit, then heLitP's
// exact AO gating and the fog transmittance the resolve already applied to
// everything else. Compiled WITH the lighting preamble (heLight/heSkyEnv/heAO).
constexpr const char* kSSRCompositeFS = R"(#version 450
layout(location = 0) out vec4 oColor; // blended ONE/ONE onto the resolved HDR
layout(set = 0, binding = 19) uniform sampler2D heGB0;
layout(set = 0, binding = 20) uniform sampler2D heGB1;
layout(set = 0, binding = 21) uniform sampler2D heGB2;
layout(set = 0, binding = 22) uniform sampler2D heGBDepth;
layout(set = 0, binding = 27) uniform sampler2D heSSRTex;      // near-sharp (one 5-tap pass)
layout(set = 0, binding = 28) uniform sampler2D heSSRTexRough; // wide second blur (High tier); below High the renderer binds heSSRTex here → lerp is a no-op
layout(set = 0, binding = 29) uniform sampler2D heGIRefl;      // ray-traced GI reflections (rgb radiance, a confidence); dummy + giRefl.x = 0 when inactive
layout(set = 0, binding = 30) uniform sampler2D heGIReflRough; // wide second blur (quality High); below High the renderer binds heGIRefl here → lerp is a no-op
layout(std140, set = 0, binding = 23) uniform HeResolve {
    mat4 invViewProj;
    vec4 depthParams;
    vec4 clusterParams;
    vec4 clusterCamFwd;
} heResolve;
vec3 heOctDecode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}
void main() {
    vec2 uv = gl_FragCoord.xy / max(heLight.giParams.xy, vec2(1.0));
    float d = texture(heGBDepth, uv).r;
    if (d >= 1.0) discard;
    vec4 g0 = texture(heGB0, uv);
    vec4 g1 = texture(heGB1, uv);
    vec4 g2 = texture(heGB2, uv);
    vec4 clip = vec4(uv.x * 2.0 - 1.0,
                     (uv.y * 2.0 - 1.0) * heResolve.depthParams.x,
                     d * heResolve.depthParams.y + heResolve.depthParams.z, 1.0);
    vec4 wp = heResolve.invViewProj * clip;
    vec3 P  = wp.xyz / max(wp.w, 1e-8);
    vec3 n  = heOctDecode(g1.rg * 2.0 - 1.0);
    vec3 V  = normalize(heLight.camPos.xyz - P);
    // ── SYNC: heLitP's weather/material split + ambSpec term, line-for-line —
    // this IS the term heLitP skipped (ssr.w), so the two must stay identical
    // (SSR-off deferred vs forward is the guard).
    float snowMask = smoothstep(0.25, 0.75, clamp(n.y, 0.0, 1.0))
                   * clamp(heLight.weather.y, 0.0, 1.0);
    float wet      = clamp(heLight.weather.x, 0.0, 1.0) * (1.0 - snowMask);
    vec3 baseColor = mix(g0.rgb, vec3(0.90, 0.93, 0.97), snowMask);
    baseColor *= (1.0 - 0.30 * wet);
    float rough = clamp(g1.b, 0.0, 1.0);
    rough = mix(rough, 0.08, wet);
    rough = mix(rough, 0.85, snowMask);
    float metal = clamp(g0.a, 0.0, 1.0);
    float f0    = 0.08 * clamp(g1.a, 0.0, 1.0);
    vec3  specColor = mix(vec3(f0), baseColor, metal);
    specColor       = mix(specColor, vec3(0.08), wet);
    vec3 ambSpec = vec3(0.0);
    if (heLight.fog.z > 0.5)
    {
        vec3 Rrough  = normalize(mix(reflect(-V, n), n, rough));
        vec3 envSpec = texture(heSkyEnv, Rrough).rgb;
        // Ray-traced GI reflections UNDER the SSR mix (gi-reflections-plan §6):
        // the traced result replaces the cubemap wherever a scene ray hit, and
        // a confident screen-space hit still wins below — so SSR supplies the
        // sharp on-screen detail and the traced pass fills its off-screen gaps.
        // Glossy roughness lerp mirrors the SSR pair right below (below
        // quality High both samplers hold the same texture → no-op).
        vec4 gg0 = texture(heGIRefl, uv);
        vec4 gg1 = texture(heGIReflRough, uv);
        vec4 gg  = mix(gg0, gg1, smoothstep(0.0, max(heLight.giRefl.y, 1e-3), rough));
        envSpec = mix(envSpec, gg.rgb, gg.a * heLight.giRefl.x);
        // Glossy lerp (ssr-plan §4.3 v2): mirror-like surfaces read the
        // near-sharp result, rough ones the wide second blur — the mip-chain
        // substitute. Below quality High both samplers hold the same texture.
        vec4 r0 = texture(heSSRTex, uv);
        vec4 r1 = texture(heSSRTexRough, uv);
        vec4 r  = mix(r0, r1, smoothstep(0.0, max(heLight.ssr.z, 1e-3), rough));
        envSpec = mix(envSpec, r.rgb, r.a * heLight.ssr.y); // SSR hit over the cubemap
        float NdV = clamp(dot(n, V), 0.0, 1.0);
        vec3 fresnelSpec = specColor
            + (max(vec3(1.0 - rough), specColor) - specColor) * pow(1.0 - NdV, 5.0);
        ambSpec = envSpec * fresnelSpec * (1.0 - 0.6 * rough);
    }
    // AO exactly as heLitP applies it: the non-GI branch multiplies ambSpec by
    // material-AO × screen-space AO; the GI branch adds it unoccluded.
    if (heLight.giProbe.y <= 0.5)
    {
        float ssao = (heLight.fog.w > 0.5) ? texture(heAO, uv).r : 1.0;
        ambSpec *= clamp(g2.a, 0.0, 1.0) * ssao;
    }
    // Fog transmittance (mirror of heApplyFog's factor): the resolve fogged the
    // rest of the lighting — an additive term must scale by (1 − f) = e^-optical.
    if (heLight.fog.x > 0.0 && heLight.fog.z > 0.5)
    {
        vec3  ray  = P - heLight.camPos.xyz;
        float dist = length(ray);
        float k    = heLight.fog.y * ray.y;
        float tt   = (abs(k) > 1e-4) ? (1.0 - exp(-k)) / k : 1.0;
        float optical = heLight.fog.x * dist * exp(-heLight.fog.y * heLight.camPos.y) * tt;
        ambSpec *= exp(-optical);
    }
    oColor = vec4(ambSpec, 1.0);
}
)";

// ─── SSR blur (docs/ssr-plan.md §4.3, P4) ────────────────────────────────────
// Separable 5-tap Gaussian (1 4 6 4 1 / 16) over the half-res trace result,
// run twice (horizontal, then vertical) before the composite. Taps are weighted
// by their confidence so miss pixels (a = 0, black rgb) never darken hit edges;
// the blurred confidence itself feathers the transition to the cubemap fallback.
constexpr const char* kSSRBlurFS = R"(#version 450
layout(location = 0) out vec4 oSSR;
layout(set = 0, binding = 19) uniform sampler2D heSSRIn;
layout(std140, set = 0, binding = 23) uniform HeSSRBlur {
    vec4 dir; // xy = one-texel UV step along the blur axis, zw = 1 / target size
} heBlur;
void main() {
    vec2 uv = gl_FragCoord.xy * heBlur.dir.zw;
    float w[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);
    vec3  rgb  = vec3(0.0);
    float conf = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec4 s = texture(heSSRIn, uv + heBlur.dir.xy * float(i - 2));
        rgb  += s.rgb * s.a * w[i];
        conf += s.a * w[i];
    }
    oSSR = vec4(conf > 1e-4 ? rgb / conf : vec3(0.0), conf);
}
)";
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::ssrTrace(Backend backend)
{
    const int key = static_cast<int>(backend) * 4;
    if (auto it = m_ssrCache.find(key); it != m_ssrCache.end()) return it->second;
    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
        out = toCompiled(compileMslPinned(kSSRTraceFS, Stage::Fragment,
            { { Stage::Fragment, 0, 23, 0 },    // HeSSRTrace UBO → fragment buffer 0
              { Stage::Fragment, 0, 19, 0 },    // scene colour → texture/sampler 0
              { Stage::Fragment, 0, 20, 1 },    // GB1 → 1
              { Stage::Fragment, 0, 22, 2 } }));// depth → 2
    else
        out = toCompiled(compile(kSSRTraceFS, Stage::Fragment, toTarget(backend)));
    return m_ssrCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::ssrComposite(Backend backend)
{
    const int key = static_cast<int>(backend) * 4 + 1;
    if (auto it = m_ssrCache.find(key); it != m_ssrCache.end()) return it->second;
    using namespace he::shaderc;
    const std::string injected = injectPreamble(kSSRCompositeFS);
    Compiled out;
    if (backend == Backend::Metal)
        out = toCompiled(compileMslPinned(injected, Stage::Fragment,
            { { Stage::Fragment, 0, 0, static_cast<uint32_t>(kMetalLightingBufferIndex) },
              { Stage::Fragment, 0, 23, 3 },    // HeResolve UBO → fragment buffer 3
              { Stage::Fragment, 0, 19, 0 },    // GB0 → 0
              { Stage::Fragment, 0, 20, 1 },    // GB1 → 1
              { Stage::Fragment, 0, 21, 2 },    // GB2 → 2
              { Stage::Fragment, 0, 22, 3 },    // depth → 3
              { Stage::Fragment, 0, 27, 4 },    // SSR result (near-sharp) → 4
              { Stage::Fragment, 0, 28, 5 },    // SSR result (wide blur) → 5
              { Stage::Fragment, 0, 29, 6 },    // ray-traced GI reflections → 6
              { Stage::Fragment, 0, 30, 7 },    // GI reflections (wide blur) → 7
              { Stage::Fragment, 0, 15, 14 },   // sky env cubemap (scene-pass slot)
              { Stage::Fragment, 0, 16, 15 } }));// screen-space AO (scene-pass slot)
    else
        out = toCompiled(compile(injected, Stage::Fragment, toTarget(backend)));
    return m_ssrCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::ssrBlur(Backend backend)
{
    const int key = static_cast<int>(backend) * 4 + 2;
    if (auto it = m_ssrCache.find(key); it != m_ssrCache.end()) return it->second;
    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
        out = toCompiled(compileMslPinned(kSSRBlurFS, Stage::Fragment,
            { { Stage::Fragment, 0, 23, 0 },    // HeSSRBlur UBO → fragment buffer 0
              { Stage::Fragment, 0, 19, 0 } }));// trace result → texture/sampler 0
    else
        out = toCompiled(compile(kSSRBlurFS, Stage::Fragment, toTarget(backend)));
    return m_ssrCache.emplace(key, std::move(out)).first->second;
}

namespace
{
// ─── Deferred decals (Metal tile mode) ───────────────────────────────────────
// Unit-cube projector, rasterized inside the G-buffer pass. Front faces are
// culled by the encoder (camera-inside-box safe) and there is no depth test —
// the box-space clip against the reconstructed world position decides.
constexpr const char* kDecalVS = R"(#version 450
layout(std140, set = 0, binding = 23) uniform HeDecal {
    mat4 viewProj;
    mat4 model;
    mat4 invModel;
    mat4 invViewProj;
    vec4 color;
    vec4 params;
    vec4 vp;
} heDecal;
void main() {
    // 36-vertex unit cube [-0.5, 0.5]³ as a triangle list from gl_VertexIndex.
    const int idx[36] = int[36](
        0,1,2, 2,1,3,  4,6,5, 5,6,7,   // -Z, +Z
        0,4,1, 1,4,5,  2,3,6, 6,3,7,   // -Y, +Y
        0,2,4, 4,2,6,  1,5,3, 3,5,7);  // -X, +X
    int c = idx[gl_VertexIndex];
    vec3 corner = vec3(float(c & 1), float((c >> 1) & 1), float((c >> 2) & 1)) - 0.5;
    gl_Position = heDecal.viewProj * heDecal.model * vec4(corner, 1.0);
}
)";

constexpr const char* kDecalFS = R"(#version 450
layout(location = 0) out vec4 oGB0; // alpha-blended, writeMask RGB (metallic in .a stays)
layout(input_attachment_index = 3, set = 0, binding = 22) uniform subpassInput heGBDepth;
layout(set = 0, binding = 19) uniform sampler2D heDecalTex;
layout(std140, set = 0, binding = 23) uniform HeDecal {
    mat4 viewProj;
    mat4 model;
    mat4 invModel;
    mat4 invViewProj;
    vec4 color;   // rgba tint
    vec4 params;  // x hasTexture, y ndc-y sign, z depth scale, w depth bias
    vec4 vp;      // xy viewport
} heDecal;
void main() {
    float d = subpassLoad(heGBDepth).r;
    if (d >= 1.0) discard;                       // background
    vec2 uv = gl_FragCoord.xy / max(heDecal.vp.xy, vec2(1.0));
    vec4 clip = vec4(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * heDecal.params.y,
                     d * heDecal.params.z + heDecal.params.w, 1.0);
    vec4 wp = heDecal.invViewProj * clip;
    vec3 P  = wp.xyz / max(wp.w, 1e-8);
    vec3 lp = (heDecal.invModel * vec4(P, 1.0)).xyz;
    if (any(greaterThan(abs(lp), vec3(0.5)))) discard; // outside the projector box
    vec4 c = heDecal.color;
    if (heDecal.params.x > 0.5)
        c *= texture(heDecalTex, lp.xz + 0.5);   // projected along the box's local Y
    if (c.a <= 0.001) discard;
    oGB0 = c;
}
)";
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::decalVertex(Backend backend)
{
    const int key = static_cast<int>(backend) * 2;
    if (auto it = m_decalCache.find(key); it != m_decalCache.end()) return it->second;
    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
        out = toCompiled(compileMslPinned(kDecalVS, Stage::Vertex,
            { { Stage::Vertex, 0, 23, 0 } })); // HeDecal UBO → vertex buffer 0
    else
        out = toCompiled(compile(kDecalVS, Stage::Vertex, toTarget(backend)));
    return m_decalCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::decalFragment(Backend backend)
{
    const int key = static_cast<int>(backend) * 2 + 1;
    if (auto it = m_decalCache.find(key); it != m_decalCache.end()) return it->second;
    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        MslOptions opts;
        opts.framebufferFetchSubpasses = true; // heGBDepth → [[color(3)]]
        out = toCompiled(compileMslPinned(kDecalFS, Stage::Fragment,
            { { Stage::Fragment, 0, 23, 0 },    // HeDecal UBO → fragment buffer 0
              { Stage::Fragment, 0, 19, 0 } },  // decal texture → texture/sampler 0
            opts));
    }
    // Non-Metal backends have no framebuffer-fetch decal path in v1.
    return m_decalCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::fullscreenVertex(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_fsVertCache.find(key); it != m_fsVertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
        out = toCompiled(compileMslPinned(kFullscreenVS, Stage::Vertex, {}));
    else
        out = toCompiled(compile(kFullscreenVS, Stage::Vertex, toTarget(backend)));
    return m_fsVertCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::fragment(
    uint64_t sourceHash, const std::string& glsl, Backend backend)
{
    // Mix the source hash with the backend so each backend gets its own cache slot
    // without the two ever colliding.
    const uint64_t key = sourceHash ^ (0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(backend) + 1));
    if (auto it = m_fragCache.find(key); it != m_fragCache.end()) return it->second;

    using namespace he::shaderc;
    const std::string injected = injectPreamble(glsl); // adds the lighting UBO + heLit()
    Compiled out;
    if (backend == Backend::Metal)
    {
        // ── Fragment sampler budget ──────────────────────────────────────────
        // Metal caps a fragment stage at 16 SAMPLERS, i.e. indices 0..15, and the
        // material pipeline is currently AT that limit:
        //   0      heTex0 (legacy/mesh texture)
        //   1-4    heTexP0..3 (node-graph project textures)
        //   6,7    DDGI irradiance / visibility  (shared with the scene pass)
        //   9,10   GI sun + local shadow masks
        //   11     CSM array          12  local point/spot shadow atlas
        //   13     landscape weightmap
        //   14     sky env cubemap    15  screen-space AO
        // A new sampler MUST reuse one of these (or a scene-pass slot the shared
        // encoder already binds, as 6/7 do) — pinning a 17th makes the whole
        // pipeline fail to build, and the renderer then falls back to built-in
        // PBR for EVERY graph material without anything looking obviously broken.
        //
        // Pin the lighting UBO to the fragment slot the engine binds it at (buffer 1;
        // SceneUniforms occupies fragment buffer 0 in the scene pass), and the material
        // texture (set 0, binding 2 in canonical GLSL) to texture/sampler 0 — the slot the
        // geometry loop already binds per draw (material/mesh texture + linear sampler).
        out = toCompiled(compileMslPinned(injected, Stage::Fragment,
            { { Stage::Fragment, 0, 0, static_cast<uint32_t>(kMetalLightingBufferIndex) },
              { Stage::Fragment, 0, 2, 0 },     // legacy/mesh texture → texture/sampler 0
              { Stage::Fragment, 0, 3, 2 },     // HeParams UBO → fragment buffer 2
              // Node-graph project textures heTexP0..3 (GLSL binding 4..7) → MSL
              // texture/sampler 1..4. Pinned unconditionally (harmless when unused).
              { Stage::Fragment, 0, 4, 1 },
              { Stage::Fragment, 0, 5, 2 },
              { Stage::Fragment, 0, 6, 3 },
              { Stage::Fragment, 0, 7, 4 },
              // GI screen-space shadow masks (preamble bindings 10/11) → MSL
              // texture/sampler 9/10 — clear of the material textures (0-4) AND
              // the standard pipeline's GI slots (5-8), so both pipelines can
              // share one encoder without rebinding.
              { Stage::Fragment, 0, 10, 9 },
              { Stage::Fragment, 0, 11, 10 },
              // CSM depth array for the GI-off fallback (preamble binding 12)
              // → MSL texture/sampler 11 (next free slot after the GI masks).
              { Stage::Fragment, 0, 12, 11 },
              // Local (point/spot) shadow atlas (preamble binding 13) → MSL
              // texture/sampler 12 — the SAME pin the scene passes bind the
              // atlas at for the built-in shaders, so no per-draw rebinding.
              { Stage::Fragment, 0, 13, 12 },
              // Landscape layer weightmap (preamble binding 14) → MSL
              // texture/sampler 13. Bound PER DRAW from the terrain chunk's
              // parent landscape (not per material), so two landscapes can share
              // one material and still carry their own paint.
              { Stage::Fragment, 0, 14, 13 },
              // Sky environment cubemap (preamble binding 15) → MSL 14, and the
              // screen-space AO result (binding 16) → MSL 15. Both are per-FRAME
              // state pinned once on the encoder, like the GI/CSM slots above.
              { Stage::Fragment, 0, 15, 14 },
              { Stage::Fragment, 0, 16, 15 },
              // DDGI probe atlases (bindings 17/18) → MSL 6/7, NOT 16/17: Metal
              // caps a fragment stage at 16 SAMPLERS (0..15), and slots 0-4 +
              // 9-15 are already spoken for — pinning these any higher makes the
              // whole material pipeline fail to build, silently dropping every
              // graph material back to the built-in PBR shader.
              // 6/7 are where the SCENE pass already binds these exact two
              // atlases for the built-in shaders, and both pipelines share one
              // encoder, so the material pipeline just reads them in place.
              { Stage::Fragment, 0, 17, 6 },
              { Stage::Fragment, 0, 18, 7 } }));
    }
    else
    {
        out = toCompiled(compile(injected, Stage::Fragment, toTarget(backend)));
    }
    return m_fragCache.emplace(key, std::move(out)).first->second;
}
} // namespace HE
