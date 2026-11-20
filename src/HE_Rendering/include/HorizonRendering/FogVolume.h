#pragma once
#include <Math/Math.h>
#include <Renderer/EnvironmentSettings.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// ─── Volumetric fog, the host side (docs/volumetric-fog-plan.md, Thema 225) ──
// What every backend needs identically: the froxel grid sizing, the exponential
// slice distribution (§4.2), the analytic height-fog integral the froxel volume
// has to match at its near/far seam (§7, the parity witness), the temporal
// history weight, the per-frame jitter sequence, the UBO layout the Inject pass
// uploads, and a CPU reference of the Integrate pass's column scan for tests.
// The three passes themselves (Inject/Integrate/Apply — compute or GL fragment-
// per-layer) are backend code and do not live here.
//
// Header-only for the same reason TemporalAA.h is: no .cpp, so a backend static
// lib can include it without linking HorizonRendering.
namespace HE
{

// ── Grid size (plan §4.1) ─────────────────────────────────────────────────────
// X and Z (slice count) are fixed per quality tier; Y follows the RENDER
// resolution's aspect ratio (render scale does not change the grid — only the
// window/viewport shape does). aspectWidthOverHeight <= 0 falls back to 16:9,
// the baseline window's own ratio.
inline glm::ivec3 fogGridFor(int quality, float aspectWidthOverHeight)
{
    struct Tier { int x; int z; };
    constexpr Tier kTiers[3] = { {96, 32}, {160, 64}, {192, 96} }; // Low, Medium, High
    const Tier& t = kTiers[std::clamp(quality, 0, 2)];
    const float a = (aspectWidthOverHeight > 1e-4f) ? aspectWidthOverHeight : (16.0f / 9.0f);
    const int   y = std::max(1, static_cast<int>(std::lround(static_cast<float>(t.x) / a)));
    return glm::ivec3(t.x, y, t.z);
}

// Convenience overload for a caller that has actual pixel dimensions (a
// backend's render target) rather than a bare ratio. Never divides by zero:
// renderHeight <= 0 falls back to 16:9 like the ratio overload.
inline glm::ivec3 fogGridFor(int quality, int renderWidth, int renderHeight)
{
    const float a = (renderHeight > 0) ? static_cast<float>(renderWidth) / static_cast<float>(renderHeight)
                                        : (16.0f / 9.0f);
    return fogGridFor(quality, a);
}

// ── Slice distribution (plan §4.2) ────────────────────────────────────────────
// Exponential with a linear-looking start: z(t) = c * ((1 + D/c)^t - 1), t in
// [0,1] mapping to slice boundaries t = k/N. c and D > 0 are assumed (callers
// clamp D to [16, min(512, shadowDistance)] before this; c is the fixed plan
// constant, 2 m, never user-facing).
inline float fogSliceToDepth(float t, float D, float c)
{
    if (c <= 0.0f) return t * D; // degenerate guard: linear fallback, never hit with c = 2
    return c * (std::pow(1.0f + D / c, t) - 1.0f);
}

// Inverse of fogSliceToDepth: the slice fraction t in [0,1] a world depth z
// falls into. Negative z (behind the camera plane) clamps to 0.
inline float fogDepthToSlice(float z, float D, float c)
{
    if (c <= 0.0f || D <= 0.0f) return 0.0f;
    const float denom = std::log(1.0f + D / c);
    if (denom <= 1e-8f) return 0.0f;
    const float zc = std::max(z, 0.0f) / c;
    return std::log(1.0f + zc) / denom;
}

// ── Analytic height-fog optical depth, a segment at a time ───────────────────
// The line integral of density * exp(-falloff * y) along a ray from distance a
// to b, where y(s) = camY + dirY * s and dirY is the Y component of a UNIT ray
// direction (not a raw displacement) — exactly what the built-in heApplyFog
// (MaterialShaderLibrary.cpp) and every backend's applyFog() evaluate from 0 to
// the full ray length. fogOpticalSegment(camY, dirY, 0, dist, density, falloff)
// reproduces that single-ray formula bit for bit (mean-height attenuation via
// the k -> 0 limit included); this generalises it to [a,b] for the volume's
// far-field continuation past D (plan §6.3: Apply picks up at dv, not at 0).
inline float fogOpticalSegment(float camY, float dirY, float a, float b, float density, float falloff)
{
    if (density <= 0.0f || b <= a) return 0.0f;
    const float base = density * std::exp(-falloff * camY);
    const float m     = falloff * dirY;
    if (std::abs(m) > 1e-4f)
        return base * (std::exp(-m * a) - std::exp(-m * b)) / m;
    return base * (b - a); // falloff ~ 0 or a near-horizontal ray: uniform density along the segment
}

// ── Temporal reprojection (plan §5.3) ─────────────────────────────────────────
// History blend weight for a time constant of 40 ms: short enough that a
// mis-reprojected froxel does not linger, long enough to denoise the 1-tap
// shadow/noise sampling. 200 FPS (dt=5ms) -> ~0.118, 60 FPS (dt=16.7ms) -> ~0.341.
inline constexpr float kFogHistoryTau = 0.040f;
inline float fogHistoryAlpha(float dtSeconds)
{
    if (dtSeconds <= 0.0f) return 1.0f; // no elapsed time: nothing to blend toward, reset-equivalent
    return 1.0f - std::exp(-dtSeconds / kFogHistoryTau);
}

// R1 low-discrepancy sequence (Martin Roberts), 16 positions in [0,1), used to
// jitter the froxel's z-sample within its slice. Independent of taaJitter
// (TemporalAA.h) — the volume reprojects on its own history, not the screen's.
inline float fogJitter(uint32_t frameIndex)
{
    constexpr float kInvPhi = 0.6180339887498949f; // 1 / golden ratio
    const uint32_t  n       = (frameIndex % 16u) + 1u;
    const float     x       = static_cast<float>(n) * kInvPhi;
    return x - std::floor(x);
}

// ── FogVolumeData (plan §2.2) ──────────────────────────────────────────────────
// What the extractor hands the backend each frame. Passive: no logic beyond
// what MakeFogVolumeData below computes once, from the pushed EnvironmentSettings
// plus the frame's capability/camera state. Defaults mirror EnvironmentComponent's
// (HorizonScene/Components/EnvironmentComponent.h) so a default-constructed
// FogVolumeData looks like a scene that never touched the fog fields.
struct FogVolumeData
{
    bool       enabled = false; // fogMode==1 && (density>0 || bankDensity>0) && backend capable && camera dry
    int        quality = 1;
    glm::ivec3 grid { 160, 90, 64 };
    float      distance = 96.0f;  // D, clamped to [16, min(512, shadowDistance)]
    float      sliceC   = 2.0f;   // plan's fixed slice-distribution constant, not user-facing
    float      density = 0.0f;
    float      heightFalloff = 0.1f;
    float      noiseAmount = 0.5f;
    float      invNoiseScale = 1.0f / 40.0f;
    float      bankDensity = 0.0f;
    float      bankBaseY = 0.0f;
    float      bankThickness = 4.0f;
    float      precipExtinction = 0.0f; // from rain/snowAmount, plan §6.3
    glm::vec3  albedo = glm::vec3(1.0f);
    float      anisotropy = 0.5f;
    float      localLightScatter = 1.0f;
    float      flash = 0.0f;
    float      sunGain   = 0.3f;  // radiance gain of the sun's in-scatter term (isotropic-normalised phase); HE_FOG_SUNGAIN overrides
    float      lightGain = 0.1f;  // same for the window lights; HE_FOG_LIGHTGAIN overrides
    bool       parity = false; // HE_FOG_PARITY=1: noise/bank/haze/sun/lights off — the volume must equal the analytic fog (plan §9)
};

// Builds a FogVolumeData from the frame's pushed EnvironmentSettings (the same
// struct makeEnvironmentSettings/EnvironmentPush fills from EnvironmentComponent)
// plus the three things EnvironmentSettings cannot know: whether the backend
// implements the feature at all, whether the camera is underwater this frame
// (the underwater post pass owns the view there instead, plan §11), and the
// render target's aspect ratio (the grid's Y size). shadowDistance clamps D so
// the volume never reaches past the directional-shadow cascades it samples.
inline FogVolumeData MakeFogVolumeData(const ::EnvironmentSettings& env,
                                       bool  backendSupportsVolumetricFog,
                                       bool  cameraUnderwater,
                                       float aspectWidthOverHeight,
                                       float shadowDistance)
{
    FogVolumeData out;
    out.quality          = std::clamp(env.fogQuality, 0, 2);
    out.grid              = fogGridFor(out.quality, aspectWidthOverHeight);
    const float maxD       = std::min(512.0f, shadowDistance > 0.0f ? shadowDistance : 512.0f);
    out.distance           = std::clamp(env.fogVolumeDistance, 16.0f, std::max(16.0f, maxD));
    out.density            = std::max(env.fogDensity, 0.0f);
    out.heightFalloff      = env.fogHeightFalloff;
    out.noiseAmount        = std::clamp(env.fogNoiseAmount, 0.0f, 1.0f);
    out.invNoiseScale      = 1.0f / std::max(env.fogNoiseScale, 1e-3f);
    out.bankDensity        = std::max(env.fogBankDensity, 0.0f);
    out.bankBaseY          = env.fogBankBaseY;
    out.bankThickness      = std::max(env.fogBankThickness, 0.0f);
    // Rain/snow haze, plan §6.3 — goes into the volume only, never the far field.
    out.precipExtinction   = 0.004f * std::max(env.rainAmount, 0.0f) + 0.008f * std::max(env.snowAmount, 0.0f);
    out.albedo             = env.fogAlbedo;
    out.anisotropy         = std::clamp(env.fogAnisotropy, -0.9f, 0.9f);
    out.localLightScatter  = std::max(env.fogLocalLightScatter, 0.0f);
    out.flash              = env.flash;
    // Debug switches for the headless witnesses (HE_DUMP scenes carry no fog settings of
    // their own): HE_FOG_MODE=<0|1> overrides the scene's fogMode, HE_FOG_PARITY=1 is the
    // parity witness of plan §9 — only the height-fog term is left in the volume, so the
    // result may be compared against mode 0 on graph-material pixels.
    if (const char* v = std::getenv("HE_FOG_SUNGAIN"); v && *v)   out.sunGain   = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("HE_FOG_LIGHTGAIN"); v && *v) out.lightGain = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("HE_FOG_DENSITY"); v && *v)   out.density   = std::max(static_cast<float>(std::atof(v)), 0.0f);
    int mode = env.fogMode;
    if (const char* v = std::getenv("HE_FOG_MODE"); v && *v) mode = std::atoi(v);
    if (const char* v = std::getenv("HE_FOG_PARITY"); v && *v && std::atoi(v) != 0)
    {
        out.parity            = true;
        out.noiseAmount       = 0.0f;
        out.bankDensity       = 0.0f;
        out.precipExtinction  = 0.0f;
        out.localLightScatter = 0.0f;
        out.flash             = 0.0f;
    }
    out.enabled            = (mode == 1)
                            && (out.density > 0.0f || out.bankDensity > 0.0f)
                            && backendSupportsVolumetricFog
                            && !cameraUnderwater;
    return out;
}

// ── FogVolumeConstants (plan §2.4) ────────────────────────────────────────────
// The Inject/Integrate pass's per-frame UBO, std140, 288 bytes. Licht, CSM and
// local-atlas matrices do NOT live here — the Inject pass binds the ordinary
// HeLighting UBO alongside this one and calls the same heCsmShadow /
// heLocalShadowFactor / heCloudShadowFactor the surfaces do (plan §2.4).
struct FogVolumeConstants
{
    glm::mat4 invViewProj  { 1.0f };  // offset 0:   unjittered, froxel -> world
    glm::mat4 prevViewProj { 1.0f };  // offset 64:  unjittered, previous frame (reprojection)
    glm::vec4 camPos       { 0.0f };  // offset 128: xyz camera, w = D
    glm::vec4 camFwd       { 0.0f };  // offset 144: xyz view axis, w = c
    glm::vec4 grid         { 0.0f };  // offset 160: xyz grid size, w = 1 / slice count
    glm::vec4 density      { 0.0f };  // offset 176: x density, y heightFalloff, z noiseAmount, w invNoiseScale
    glm::vec4 bank         { 0.0f };  // offset 192: x bankDensity, y bankBaseY, z bankThickness, w precipExtinction
    glm::vec4 noise        { 0.0f };  // offset 208: xyz wind-accumulated noise offset, w time (noise evolution)
    glm::vec4 scatter      { 0.0f };  // offset 224: rgb albedo, w anisotropy g
    glm::vec4 temporal     { 0.0f };  // offset 240: x jitter 0..1, y history alpha, z history valid (0 = reset), w frame index
    glm::vec4 misc         { 0.0f };  // offset 256: x localLightScatter, y flash, z sun share (0.5, plan constant), w underwater gate
    glm::vec4 pad          { 0.0f };  // offset 272: reserved
};
static_assert(sizeof(FogVolumeConstants) == 288, "FogVolumeConstants must match the plan's std140 layout (§2.4)");
static_assert(offsetof(FogVolumeConstants, invViewProj)  == 0);
static_assert(offsetof(FogVolumeConstants, prevViewProj) == 64);
static_assert(offsetof(FogVolumeConstants, camPos)       == 128);
static_assert(offsetof(FogVolumeConstants, camFwd)       == 144);
static_assert(offsetof(FogVolumeConstants, grid)         == 160);
static_assert(offsetof(FogVolumeConstants, density)      == 176);
static_assert(offsetof(FogVolumeConstants, bank)         == 192);
static_assert(offsetof(FogVolumeConstants, noise)        == 208);
static_assert(offsetof(FogVolumeConstants, scatter)      == 224);
static_assert(offsetof(FogVolumeConstants, temporal)     == 240);
static_assert(offsetof(FogVolumeConstants, misc)         == 256);
static_assert(offsetof(FogVolumeConstants, pad)          == 272);

// The sun's share of the sky-colour "ambient" fog source that vanishes in
// shadow (plan §5.2). Fixed, not user-facing — without it, volume shadows would
// only ever read "less bright", never darker than the horizon colour itself.
inline constexpr float kFogSunShare = 0.5f;

// Per-frame temporal/noise-drift state the backend carries between frames
// (like a TAA history index) — the extractor does not own this, FillFogVolumeConstants
// just needs it handed in.
struct FogVolumeTemporalState
{
    float    jitter01     = 0.0f; // fogJitter(frameIndex)
    float    historyAlpha = 1.0f; // fogHistoryAlpha(dt); 1 = full reset
    bool     historyValid = false;
    uint32_t frameIndex   = 0;
    glm::vec3 noiseOffset = glm::vec3(0.0f); // wind-accumulated (plan §6.2)
    float    noiseTime    = 0.0f;
};

inline FogVolumeConstants FillFogVolumeConstants(const FogVolumeData& fog,
                                                 const glm::mat4& invViewProj,
                                                 const glm::mat4& prevViewProj,
                                                 const glm::vec3& camPos,
                                                 const glm::vec3& camFwd,
                                                 const FogVolumeTemporalState& temporal,
                                                 float underwaterGate)
{
    FogVolumeConstants c;
    c.invViewProj  = invViewProj;
    c.prevViewProj = prevViewProj;
    c.camPos       = glm::vec4(camPos, fog.distance);
    c.camFwd       = glm::vec4(camFwd, fog.sliceC);
    c.grid         = glm::vec4(static_cast<float>(fog.grid.x), static_cast<float>(fog.grid.y),
                               static_cast<float>(fog.grid.z), 1.0f / std::max(1.0f, static_cast<float>(fog.grid.z)));
    c.density      = glm::vec4(fog.density, fog.heightFalloff, fog.noiseAmount, fog.invNoiseScale);
    c.bank         = glm::vec4(fog.bankDensity, fog.bankBaseY, fog.bankThickness, fog.precipExtinction);
    c.noise        = glm::vec4(temporal.noiseOffset, temporal.noiseTime);
    c.scatter      = glm::vec4(fog.albedo, fog.anisotropy);
    c.temporal     = glm::vec4(temporal.jitter01, temporal.historyAlpha, temporal.historyValid ? 1.0f : 0.0f,
                               static_cast<float>(temporal.frameIndex));
    c.misc         = glm::vec4(fog.localLightScatter, fog.flash, fog.parity ? 0.0f : kFogSunShare, underwaterGate);
    c.pad          = glm::vec4(fog.parity ? 1.0f : 0.0f, fog.sunGain, fog.lightGain, 0.0f);
    return c;
}

// ── Integrate, as a CPU reference (plan §6.1, for tests) ──────────────────────
// One column (near -> far slice order), front-to-back, energy-conserving
// (Hillaire). S = scattered radiance per metre (sigma_s * L_in), sigmaT =
// extinction per metre; dz = the slice's ray-length (already divided by
// dot(dir, camFwd) if the caller needs planar-depth slices converted). Returns
// one (L, T) pair PER SLICE, valued at that slice's far edge — exactly what
// fogIntegrated stores.
struct FogFroxelSample
{
    glm::vec3 S { 0.0f };
    float     sigmaT = 0.0f;
};

struct FogIntegratedSample
{
    glm::vec3 L { 0.0f };
    float     T = 1.0f;
};

inline std::vector<FogIntegratedSample> FogIntegrateColumnCPU(const std::vector<FogFroxelSample>& column,
                                                               const std::vector<float>& sliceLength)
{
    std::vector<FogIntegratedSample> out(column.size());
    glm::vec3 L(0.0f);
    float     T = 1.0f;
    for (size_t k = 0; k < column.size(); ++k)
    {
        const float dz     = (k < sliceLength.size()) ? sliceLength[k] : 0.0f;
        const float sigmaT = column[k].sigmaT;
        const glm::vec3& S = column[k].S;
        const float ext     = std::exp(-sigmaT * dz);
        const glm::vec3 Sint = (S - S * ext) / std::max(sigmaT, 1e-5f);
        L += T * Sint;
        T *= ext;
        out[k] = { L, T };
    }
    return out;
}

} // namespace HE
