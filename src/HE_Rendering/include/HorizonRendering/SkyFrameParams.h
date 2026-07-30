#pragma once
#include "../HE_RENDERING_API.h"
#include <Renderer/IRenderer.h>
#include <Math/Math.h>

// ─── Backend-agnostic sky constant block ──────────────────────────────────────
// Every backend used to re-translate the same ~40 EnvironmentSettings fields into
// its own sky constant buffer by hand — five hand-maintained mappings that could
// (and did) drift; D3D11/D3D12 both had the cloud wind's Z sign inverted. The
// layout mirrors the MSL SkyParams struct (mat4 + 17 float4) because that is the
// richest of the five; backends whose sky shader takes a smaller constant buffer
// read the named fields they need out of this instead of memcpy'ing the whole
// thing (Vulkan, D3D11 and D3D12 all do that — their UBO/CB is a subset).
//
// STATE OF THE MIGRATION — 4 of 5 backends translate through this. ONLY Metal
// copies the struct wholesale; do not assume the others do:
//   Metal   memcpy of the whole struct (the layout IS the MSL SkyParams).
//   Vulkan  reads 15 named fields into its own 160-byte SkyUBOData and memcpies
//           THAT. A blanket copy of this 336-byte struct would misalign every
//           offset past invViewProj (VulkanRenderer.cpp, the sky UBO fill).
//   D3D11   reads the 12 named fields its smaller SkyCB has (D3D11Renderer.cpp,
//           D3D11RendererImpl::drawSky).
//   D3D12   the same 12 plus the nebula pair its shader has and D3D11's lacks
//           (D3D12Renderer.cpp, D3D12RendererImpl::drawSky).
//   OpenGL  DOES NOT. Its sky program uses loose uniforms, not a UBO, so there is
//           no POD to memcpy into — each field must be pushed with its own
//           glUniform* call against a cached location. It therefore still maps
//           its ~54 sky uniforms by hand, in the
//           `if (m_skyProgram && GetEnvironment().skyEnabled)` block of
//           OpenGLRenderer::DrawScene. A NEW FIELD MUST BE ADDED THERE TOO or it
//           silently has no effect on OpenGL.
namespace HE
{

// Cloud drift vector from the user's wind controls.
//
// windDirection is the compass direction the clouds drift TOWARD (degrees, 0 =
// toward -Z / north, increasing clockwise); windSpeed scales the rate. The 0.025
// factor turns the UI speed into world-units/sec — without it the clouds scroll
// ~40x too fast.
//
// The result is a world-space vector and is NOT clip-space dependent: every
// backend's sky shader uses it the same way (`pos * kCloudScale + wind * time`),
// so no backend may flip a component for its own NDC convention.
HE_RENDERING_API glm::vec3 CloudWindVector(const IRenderer::EnvironmentSettings& env);

// Per-frame values the environment cannot supply (camera/time/resource state).
struct SkyFrameInputs
{
	glm::mat4 invViewProj    = glm::mat4(1.0f);
	glm::vec3 sunDir         = glm::vec3(0.0f, 1.0f, 0.0f); // direction TOWARD the sun
	glm::vec3 cameraPos      = glm::vec3(0.0f);
	float     time           = 0.0f;   // animation clock, seconds
	bool      hasMoonTexture = false;  // a moon texture is bound (else the disk is flat-shaded)
	// True when the sky pass should composite the quarter-res cloud pre-pass
	// buffer instead of raymarching clouds inline. NOT simply env.lowResClouds:
	// the pre-pass target may be missing, and the pre-pass itself always passes
	// true. See the star2.z note below.
	bool      lowResClouds   = false;
};

// std140/MSL-compatible POD. Field comments give the packing; the sky shaders
// index these by name (p.star2.y etc.), so the ORDER is part of the contract.
struct SkyFrameParams
{
	glm::mat4 invViewProj = glm::mat4(1.0f);
	glm::vec4 sunDir      = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f); // xyz = toward sun, w = has-moon flag
	glm::vec4 sunColor    = glm::vec4(1.0f);                   // xyz = sun colour, w = lunar phase
	glm::vec4 params      = glm::vec4(0.0f); // x = timeOfDay (cloud scroll), y = coverage, z = wall-clock time, w = aurora
	glm::vec4 nebulaColor = glm::vec4(0.36f, 0.60f, 1.00f, 0.5f); // xyz = colour, w = nebula intensity
	glm::vec4 auroraColor = glm::vec4(0.25f, 0.95f, 0.50f, 0.6f); // xyz = colour, w = milky-way intensity
	glm::vec4 wind        = glm::vec4(0.0f); // xyz = horizontal cloud drift (world units / s); w = lightning flash
	// ── Night-sky / cloud overhaul ──────────────────────────────────────────
	glm::vec4 cameraPos      = glm::vec4(0.0f);                      // xyz = camera world pos, w = cloudMode (0 dome / 1 3D)
	glm::vec4 cloud          = glm::vec4(200.0f, 1.0f, 0.6f, 0.0f);  // height, density, fluffiness, contrailAmount
	glm::vec4 cloudTint      = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);    // xyz = tint, w = cirrusAmount
	glm::vec4 cirrus         = glm::vec4(0.0f, 0.18f, 0.4f, 0.0f);   // cirrusSeed, auroraHeight, auroraFragmentation, nebulaSeed
	glm::vec4 nebulaColor2   = glm::vec4(1.00f, 0.60f, 0.28f, 1.0f); // xyz = colour 2, w = nebula quality 0/1/2
	glm::vec4 nebulaColor3   = glm::vec4(0.90f, 0.30f, 0.16f, 0.0f); // xyz = colour 3, w = god-ray strength
	glm::vec4 auroraColorTop = glm::vec4(0.62f, 0.26f, 0.95f, 0.0f); // xyz = aurora top colour, w = meteor frequency
	glm::vec4 starColor      = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);    // xyz = tint, w = brightness
	glm::vec4 star           = glm::vec4(1.0f, 0.5f, 0.5f, 1.0f);    // size, sizeVariation, density, glow
	// x = starTwinkle, y = cloudQuality, z = composite the low-res cloud buffer,
	// w = rainAmount (drives the rainbow). z/w are read ONLY by the full sky
	// fragment shader; the quarter-res cloud pre-pass shader ignores both.
	glm::vec4 star2          = glm::vec4(0.6f, 0.0f, 0.0f, 0.0f);
	glm::vec4 neb2           = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f);    // x = nebulaCoverage
};
// Byte layout must stay identical to the MSL SkyParams (mat4 + 17 x float4): the
// whole struct is uploaded with a single setFragmentBytes/memcpy. Guard against
// silent drift.
static_assert(sizeof(SkyFrameParams) == 64 + 17 * 16, "SkyFrameParams must match the MSL SkyParams layout (336 bytes)");

// EnvironmentSettings → sky-constants translation for the four backends listed at
// the top of this header. OpenGL is NOT one of them — see that note.
HE_RENDERING_API SkyFrameParams BuildSkyFrameParams(const IRenderer::EnvironmentSettings& env,
                                                    const SkyFrameInputs&                 in);

} // namespace HE
