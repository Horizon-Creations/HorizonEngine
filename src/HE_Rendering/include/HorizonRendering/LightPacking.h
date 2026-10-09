#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <material/MaterialShaderLibrary.h> // MaterialShaderLibrary::Lighting (the graph-material light ABI)
#include <Renderer/EnvironmentSettings.h>   // FillMaterialWind's source
#include <Math/Math.h>
#include <cstdint>
#include <vector>

// ─── Shared GPU light packing ─────────────────────────────────────────────────
// All three packings below were duplicated verbatim in the backends — the first
// two once per backend, FillMaterialLightWindow seven times (OpenGL and Metal
// each had a scene copy and a UI copy). Their OUTPUT IS OBSERVABLE: the shaders
// index the arrays positionally, so the order in which lights are written, the
// clamp, and the attenuation encoding are part of the contract with the
// GLSL/MSL/HLSL — not implementation detail.
namespace HE
{

// The engine's per-frame light window. Everything downstream (direct shading, GI
// probe bounce, local shadow-mask channels) works on the FIRST kMaxLightWindow
// lights of RenderWorld::lights, in extractor order. The extractor puts shining
// directional lights first, then point/spot, then zero-intensity directionals
// (RenderExtractor.cpp, orderLightWindow), so a crowd of local lights can never
// push the sun or moon out of the window.
inline constexpr int kMaxLightWindow = 8;

// Local (point/spot) lights that get a ray-traced shadow mask channel. The mask
// texture is RGBA, hence four.
inline constexpr int kMaxMaskedLocalLights = 4;

// ── GI probe-update local lights ──────────────────────────────────────────────
// Point/spot lights feeding the one-bounce probe estimate — a scene keyed by point
// lights otherwise converges to pitch-black probes. Same 8-light window the scene
// pass binds for direct shading.
//
// Note the two different scans: this one SKIPS directional lights and stops after
// 8 ACCEPTED lights, whereas BuildMaskedLocalLights (below) walks the first 8
// lights of the window and counts within them. They are deliberately different and
// were identical in all five backends.
struct PackedLightArray
{
	glm::vec4 posRange  [kMaxLightWindow] = {}; // xyz = world position, w = max(range, 1e-4)
	glm::vec4 colorType [kMaxLightWindow] = {}; // xyz = color * intensity, w = LightType (1 point, 2 spot)
	glm::vec4 dirCos    [kMaxLightWindow] = {}; // xyz = light travel direction, w = cos(half angle)
	int       count = 0;                        // populated entries; shaders read it as sunDirRadius.w
};

HE_RENDERING_API PackedLightArray BuildPackedLightArray(const RenderWorld& rw);

// ── Ray-traced local shadow-mask lights ───────────────────────────────────────
// First 4 local (point/spot) lights of the same 8-light window the scene shader
// iterates — the fragment shader's channel index is a plain counter over
// type != 0 in the SAME order, so count every non-directional light exactly like
// that loop does and fill the first 4 slots. (Hence `count` can exceed 4 during
// the scan; it is clamped only when written out.)
struct PackedLocalShadowLights
{
	glm::vec4 posRange[kMaxMaskedLocalLights] = {}; // xyz = position, w = max(range, 1e-4)
	int       count = 0;                            // already clamped to kMaxMaskedLocalLights
};

HE_RENDERING_API PackedLocalShadowLights BuildMaskedLocalLights(const RenderWorld& rw);

// ── Graph-material (heLitP) light window ──────────────────────────────────────
// Fills the light half of MaterialShaderLibrary::Lighting — the block every
// backend binds for node-graph materials. This is a THIRD, deliberately
// different scan from the two above: it walks the first kMaxLightWindow lights
// of RenderWorld::lights and writes ALL of them, directional included and
// without an intensity filter, because heLitP()'s loop is a plain
// `for (i < counts.x)` over that same window and its light-type branch expects
// slot i to be light i. Do not "fix" it to match BuildPackedLightArray.
//
// localShadowsActive = the local (point/spot) shadow atlas is bound and valid
// this frame. Only then is lightParams[i].y written as shadowLayer + 1 (0 = the
// light casts no local shadow); every pass that has no atlas — UI quads,
// previews, and the backends with no local-shadow support at all — passes false
// and leaves the field zeroed, which is what the preamble reads as "none".
//
// Fields OTHER than the light window (sun, ambient, camPos, GI, fog, CSM,
// localShadowVP, weather) stay at the call sites: they differ per backend and
// per pass, and several are clip-space-convention-baked.
HE_RENDERING_API void FillMaterialLightWindow(const RenderWorld&               rw,
                                              MaterialShaderLibrary::Lighting& out,
                                              bool                             localShadowsActive);

// ── Graph-material wind (the Wind / Wind Sway nodes) ──────────────────────────
// Writes the environment wind into the spare .w channels of the Lighting
// prefix: sunColor.w / ambient.w = the unit world-space direction the wind
// blows TOWARD (x, z), camPos.w = its strength (EnvironmentSettings::windSpeed,
// the same number the weather system drives for the clouds).
//
// Same compass as CloudWindVector (0° = toward -Z / north, clockwise), so grass
// leans the way the clouds drift, but WITHOUT its 0.025 cloud-scroll factor:
// the material decides what a strength of 1 means in metres.
//
// Unlike the fields the doc above leaves at the call sites, wind is a pure
// world-space vector with no clip-space convention, so one helper serves all
// five backends. Call it wherever sunDir.w (the Time input) is written — every
// pass that animates a graph material. Passes that pin time to 0 (previews)
// leave the wind at 0 too.
HE_RENDERING_API void FillMaterialWind(const ::EnvironmentSettings&     env,
                                       MaterialShaderLibrary::Lighting& out);

// ── Graph-material DDGI probe field (Lighting v2.5) ──────────────────────────
// Writes giGridOrigin / giGridCounts / giProbe — the SAME grid the built-in
// shaders' GI uniforms carry, so heLitP's indirect diffuse (heGIIrradianceAt)
// samples the probe atlases exactly like sampleDDGIIrradiance does:
//   giGridOrigin = (origin, spacing), giGridCounts = (counts, probesPerRow),
//   giProbe.x    = indirect intensity, giProbe.y = the gate.
//
// atlasesBound = GI shades this frame AND both probe atlases (irradiance +
// visibility) sit on the material pass's heGIIrradiance / heGIVisibility
// slots. Only then is the gate 1; otherwise heLitP takes its flat-ambient
// branch and never samples the atlases, whatever is bound there. The grid
// values are written either way (harmless behind a 0 gate, and identical to
// what GL and Metal did inline before this helper existed).
HE_RENDERING_API void FillMaterialGIProbe(MaterialShaderLibrary::Lighting& out,
                                          const glm::vec3&                 gridOrigin,
                                          float                            probeSpacing,
                                          const glm::ivec3&                gridCounts,
                                          int                              probesPerRow,
                                          float                            indirectIntensity,
                                          bool                             atlasesBound);

// ── Clustered lighting (plan P7, cross-backend) ───────────────────────────────
// Lifts the 8-light window for point/spot lights: every local light is
// scattered on the CPU into a screen-tile × log-depth-slice grid, and the
// fragment shader shades only its own cluster's list. The window then carries
// DIRECTIONAL lights only (BuildDirectionalLightWindow) — a local light must
// live in exactly one of the two, or it is counted twice.
//
// One builder for every backend: the forward built-in shaders of
// D3D11/D3D12/Vulkan consume it through structured buffers / SSBOs, Metal's
// deferred resolve and its forward graph materials (fragmentClustered) through
// fragment buffers 4/5/6 (MetalRenderer::BuildFrameClusterLights).
//
// Buffer contract (positional — the shaders index by it):
//   lights  : 4 vec4 per light
//             [0] xyz position,        w type (1 point, 2 spot)
//             [1] xyz direction,       w cos(spot half angle)
//             [2] rgb colour,          w intensity
//             [3] x range, y local-shadow atlas base layer + 1 (0 = none),
//                 z ray-traced GI local-mask channel + 1 (0 = none), w 0
//   grid    : kClusterCount × {offset into `indices`, count}
//   indices : light indices, cluster after cluster
//   params  : x/y/z = grid dims, w = gridZ / log(far / near)
//   camFwd  : xyz = camera forward (the slice's depth axis), w = near
// Screen cells are picked from a TOP-LEFT-origin uv (Metal, D3D SV_Position
// and Vulkan gl_FragCoord all agree on that); the scatter projects with the
// GL-convention camera matrices (no clip fix) and flips v itself. OpenGL's
// gl_FragCoord is BOTTOM-left: a GL fill site passes bottomLeftOrigin = true
// and the flip is skipped, so the shared shader's cell pick stays unchanged.
inline constexpr int   kClusterGridX       = 16;
inline constexpr int   kClusterGridY       = 9;
inline constexpr int   kClusterGridZ       = 24;
inline constexpr int   kClusterCount       = kClusterGridX * kClusterGridY * kClusterGridZ;
inline constexpr float kClusterNear        = 0.1f;
inline constexpr float kClusterFar         = 1000.0f;
inline constexpr int   kMaxClusteredLights = 256;
// Hard cap on the flattened index list: the D3D12/Vulkan rings are sized once
// (kMaxClusterIndices × 4 bytes per frame in flight). A light that would
// overflow it is dropped from the lists — never silently truncated per cell,
// which would light a surface on one tile and not on its neighbour.
inline constexpr int   kMaxClusterIndices  = 65536;

struct ClusterLightBuild
{
	std::vector<glm::vec4>  lights;   // 4 × vec4 per light, never empty (one zero vec4 when no light)
	std::vector<glm::uvec2> grid;     // kClusterCount entries
	std::vector<uint32_t>   indices;  // never empty (one 0 when no light)
	glm::vec4 params  = glm::vec4(0.0f); // x/y/z grid dims, w slice scale — x == 0 → clustering off
	glm::vec4 camFwd  = glm::vec4(0.0f); // xyz camera forward, w near
	int       lightCount   = 0;          // lights admitted to the lists
	int       droppedLights = 0;         // over kMaxClusteredLights or kMaxClusterIndices
};

// localShadowsActive: the local atlas is rendered + bound this frame (else the
// layer lane stays 0 = no shadow). giMasksValid: the ray-traced local mask is
// bound this frame; the channel is assigned with BuildMaskedLocalLights' exact
// scan (first 4 local lights of the first-8 window, extractor order), so a
// cluster light keeps the channel the mask kernel rendered for it.
HE_RENDERING_API ClusterLightBuild BuildClusterLights(const RenderWorld& rw,
                                                      bool               localShadowsActive,
                                                      bool               giMasksValid,
                                                      bool               bottomLeftOrigin = false);

// Hands a cluster build's grid to the graph-material lighting block — the
// gate + grid of MaterialShaderLibrary::fragmentClustered's heLitP
// (Lighting::clusterParams / clusterCamFwd). Touches nothing else: the light
// WINDOW stays as FillMaterialLightWindow wrote it (the clustered shader skips
// the window's point/spot slots itself, a plain one still needs them). Only a
// fill site that also binds the three cluster buffers may call this; every
// other site leaves the gate at 0.
HE_RENDERING_API void FillMaterialClusterParams(const ClusterLightBuild&         build,
                                                MaterialShaderLibrary::Lighting& out);

// The light window that goes with a cluster build: directional lights only,
// first kMaxLightWindow of them in extractor order, in the built-in scene
// shaders' PerFrame encoding (lightParams.y = atlas layer, -1 = none — unused
// for directional lights, kept for the memcpy'd struct layout).
struct DirectionalLightWindow
{
	glm::vec4 pos   [kMaxLightWindow] = {}; // xyz position, w type (always 0)
	glm::vec4 dir   [kMaxLightWindow] = {}; // xyz direction, w cos(spot) (0)
	glm::vec4 color [kMaxLightWindow] = {}; // rgb colour, w intensity
	glm::vec4 params[kMaxLightWindow] = {}; // x range, y -1
	int       count = 0;
};

HE_RENDERING_API DirectionalLightWindow BuildDirectionalLightWindow(const RenderWorld& rw);

// The CLUSTERED deferred resolve's light window (Thema 150): rewrites the
// window half of a graph-material Lighting block to the directional lights
// only (same pick as BuildDirectionalLightWindow), unused slots zeroed,
// counts.x = their number. The resolve is injected WITHOUT HE_CLUSTERED, so its
// heLitP walks the whole window, and heClusterLighting adds every point/spot
// light from the lists on top — a full window would count them twice. Metal's
// EncodeClusterData does the same in place. Fill a COPY: the forward-routed and
// transparent graph materials of the same frame still need the full window.
HE_RENDERING_API void FillMaterialDirectionalWindow(const RenderWorld&               rw,
                                                   MaterialShaderLibrary::Lighting& out);

} // namespace HE
