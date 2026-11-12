#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Vulkan graph-material descriptor set 0 — the ONE table VulkanRenderer's
// createMaterialResources turns into m_matSetLayout (and sizes the per-frame
// descriptor pool from), and he_tests reflects every node shader's SPIR-V
// against.
//
// Why a header of its own: a pipeline whose shader statically uses a binding
// the set layout lacks is invalid, and a pool that does not count a layout's
// descriptor type makes vkAllocateDescriptorSets fail — the draw loop then
// returns without drawing, so every graph material silently disappears. CI has
// no Vulkan device, so neither shows up there; the table is what the test can
// hold the renderer to.
//
// No <vulkan/vulkan.h> here: he_tests builds on macOS/Linux without the
// Vulkan SDK. Kind and stage bits are the engine's own; the stage values are
// VK_SHADER_STAGE_VERTEX_BIT / VK_SHADER_STAGE_FRAGMENT_BIT (static_assert'ed
// in VulkanRenderer.cpp).
//
// Clustered variant (Thema 117): MaterialShaderLibrary::fragmentClustered
// (SpirV) adds three std430 readonly SSBOs at set 0 bindings 24/25/26 (light
// array, grid, index list). The renderer writes the very SSBOs the built-in
// scene shader reads at scene-set bindings 10..12 (same HE::BuildClusterLights
// byte layout), so there is no second buffer set and no second build. The
// plain fragment() and baked pak blobs do not declare them; a layout binding a
// shader does not use is legal, so one layout serves both variants. They are
// the LAST entries, so the first kPreClusterBindingCount rows are the layout
// as it was before — the test's negative control.
//
// heLandscapeWeights (Thema 143): binding 14, the painted terrain's weightmap
// (Landscape Layer Blend). Its row brings the fragment stage to 17 combined
// image samplers, one over the spec minimum of maxPerStageDescriptorSamplers /
// …SampledImages (16). A device at that minimum cannot create the full layout
// at all, so the renderer then builds it WITHOUT this one row (landscapeWeightsFit)
// and draws a layer-blend material through the built-in path instead: losing
// the paint there beats losing every graph material.
//
// Displacement (Thema 207): MaterialShaderLibrary::tessControl/tessEval (SpirV)
// read the FRAGMENT's bindings — HeLighting 0 (both stages), U 1, heTex0 2,
// HeParams 3, heTexP0..3 4..7, heLandscapeWeights 14 (evaluation). Those rows
// carry the two tessellation bits; the renderer masks them out again on a device
// without the tessellationShader feature.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace HE::vkmat
{
enum class DescKind : uint8_t { UniformBuffer, CombinedImageSampler, StorageBuffer };

constexpr uint32_t kStageVertex   = 0x01; // VK_SHADER_STAGE_VERTEX_BIT
constexpr uint32_t kStageFragment = 0x10; // VK_SHADER_STAGE_FRAGMENT_BIT
constexpr uint32_t kStageTessControl = 0x02; // VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
constexpr uint32_t kStageTessEval    = 0x04; // VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
constexpr uint32_t kStageTess        = kStageTessControl | kStageTessEval;

struct Binding
{
	uint32_t binding;
	DescKind kind;
	uint32_t stages;
	// A UniformBuffer row bound as UNIFORM_BUFFER_DYNAMIC: the per-draw ring slot is the dynamic
	// offset, so ONE descriptor set serves every draw that shares its textures instead of one set
	// (and ~25 descriptor writes) per draw. The SPIR-V cannot tell the two apart.
	bool     dynamic = false;
};

constexpr uint32_t kClusterLightsBinding = 24; // HeClusterLights { vec4 clLights[]; }
constexpr uint32_t kClusterGridBinding   = 25; // HeClusterGrid   { uvec2 clGrid[]; }
constexpr uint32_t kClusterIdxBinding    = 26; // HeClusterIdx    { uint clIdx[]; }
constexpr uint32_t kLandscapeWeightsBinding = 14; // heLandscapeWeights (MaterialGraph.cpp)
constexpr uint32_t kFogVolumeBinding        = 34; // heFogVolume (MaterialShaderLibrary.cpp, HE_FOG_VOLUME)

inline constexpr Binding kBindings[] = {
	{  0, DescKind::UniformBuffer,        kStageFragment | kStageTess }, // HeLighting
	{  1, DescKind::UniformBuffer,        kStageVertex   | kStageTess, true }, // U (per object, dynamic offset)
	{  2, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heTex0
	{  3, DescKind::UniformBuffer,        kStageFragment | kStageTess, true }, // HeParams (dynamic offset)
	{  4, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heTexP0
	{  5, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heTexP1
	{  6, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heTexP2
	{  7, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heTexP3
	// 8/9 for the WPO custom vertex, which reads HeLighting/HeParams in the
	// VERTEX stage at those slots (MaterialShaderLibrary.cpp wpoDeclarations).
	// Harmless for the standard vertex, which references neither.
	{  8, DescKind::UniformBuffer,        kStageVertex   }, // HeLighting (WPO VS)
	{  9, DescKind::UniformBuffer,        kStageVertex,                true }, // HeParams   (WPO VS, dynamic offset)
	{ 10, DescKind::CombinedImageSampler, kStageFragment }, // heGIShadow (GI sun mask)
	{ 11, DescKind::CombinedImageSampler, kStageFragment }, // heGILocal  (GI local mask)
	{ 12, DescKind::CombinedImageSampler, kStageFragment }, // heCsm (CSM fallback, 2D array)
	// heSSRFwd (docs/ssr-cross-backend-plan.md B5 / §2.3 head 1): the
	// preamble's reflection cascade declares this sampler unconditionally and
	// heLight.ssr.x decides whether it is read.
	{ 31, DescKind::CombinedImageSampler, kStageFragment }, // heSSRFwd
	// heLocalShadow (sampler2DArray): the local (point/spot) shadow atlas the
	// built-in scene shader samples at binding 9. Gated by lightParams[i].y.
	{ 13, DescKind::CombinedImageSampler, kStageFragment }, // heLocalShadow
	// heLandscapeWeights: only a Landscape Layer Blend graph declares it, but
	// that SPIR-V uses it statically; without the row lavapipe crashes in the
	// draw (Thema 143). Bound per DRAW from the terrain's weightmap.
	{ kLandscapeWeightsBinding, DescKind::CombinedImageSampler, kStageFragment | kStageTess }, // heLandscapeWeights
	// The rest of the preamble's fixed samplers (Thema 120). Its SPIR-V uses
	// all of them statically, so the layout declares them even where the gate
	// never opens on this backend (a pipeline whose shader uses a binding its
	// layout lacks is invalid — it only ran by driver leniency before).
	//   15 heSkyEnv (cube)   — fog.z: the baked sky cube (SkyEnvBake.h, the
	//                          GL/Metal bake), white cube until the first bake
	//   16 heAO              — fog.w: this frame's blurred SSAO (scene binding 3's
	//                          image), white when SSAO did not run
	//   17 heGIIrradiance    — giProbe.y (FillMaterialGIProbe): the DDGI atlases,
	//   18 heGIVisibility      the SAME two images scene bindings 5/6 sample
	//   32 heGIReflFwd       — giRefl.z, no RT reflections here: white
	//   33 heCloudShadow     — cloudShadowB.x, never set here: white
	{ 15, DescKind::CombinedImageSampler, kStageFragment }, // heSkyEnv
	{ 16, DescKind::CombinedImageSampler, kStageFragment }, // heAO
	{ 17, DescKind::CombinedImageSampler, kStageFragment }, // heGIIrradiance
	{ 18, DescKind::CombinedImageSampler, kStageFragment }, // heGIVisibility
	{ 32, DescKind::CombinedImageSampler, kStageFragment }, // heGIReflFwd
	{ 33, DescKind::CombinedImageSampler, kStageFragment }, // heCloudShadow
	// heFogVolume (Thema 225 step 5): the integrated froxel fog volume (3D, RGBA16F),
	// read with texelFetch by heApplyFogT of a Translucent graph material. The SPIR-V
	// of every such material uses it statically, so the row is always in the layout;
	// a 1x1x1 3D dummy sits there on a frame without the volume (fogVol.w = 0).
	{ kFogVolumeBinding, DescKind::CombinedImageSampler, kStageFragment },
	// ── Thema 117: fragmentClustered's light lists (keep these LAST) ──
	{ kClusterLightsBinding, DescKind::StorageBuffer, kStageFragment },
	{ kClusterGridBinding,   DescKind::StorageBuffer, kStageFragment },
	{ kClusterIdxBinding,    DescKind::StorageBuffer, kStageFragment },
};

constexpr uint32_t kBindingCount           = sizeof(kBindings) / sizeof(kBindings[0]);
constexpr uint32_t kPreClusterBindingCount = kBindingCount - 3;

// Descriptors of one kind in one set — what the per-frame pool must hold per
// allocated set (times the sets it serves).
constexpr uint32_t countOf(DescKind kind, uint32_t bindingCount = kBindingCount)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < bindingCount; ++i)
		if (kBindings[i].kind == kind) ++n;
	return n;
}

// Rows bound as dynamic uniform buffers (bindings 1, 3, 9). vkCmdBindDescriptorSets takes their
// offsets in ascending binding order.
constexpr uint32_t dynamicCount(uint32_t bindingCount = kBindingCount)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < bindingCount; ++i)
		if (kBindings[i].dynamic) ++n;
	return n;
}
constexpr uint32_t kDynamicOffsetCount = 3;

// Combined image samplers the FRAGMENT stage sees — what the device's
// per-stage sampler and sampled-image limits are held against (a combined
// descriptor counts against both).
constexpr uint32_t fragmentSamplerCount(bool withLandscapeWeights = true, bool withFogVolume = true)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < kBindingCount; ++i)
		if (kBindings[i].kind == DescKind::CombinedImageSampler && (kBindings[i].stages & kStageFragment)
		    && (withLandscapeWeights || kBindings[i].binding != kLandscapeWeightsBinding)
		    && (withFogVolume || kBindings[i].binding != kFogVolumeBinding))
			++n;
	return n;
}

// Does the full layout fit the device's maxPerStageDescriptorSamplers /
// maxPerStageDescriptorSampledImages? The renderer drops the heFogVolume row first
// (its materials then compile without HE_FOG_VOLUME, see MaterialShaderLibrary::
// setSpirvFogVolume), and only then asks this about heLandscapeWeights: the layout
// WITHOUT that row (and with whatever fog decided) must fit, else it leaves the
// heLandscapeWeights row out too.
constexpr bool fogVolumeFit(uint32_t maxPerStageSamplers, uint32_t maxPerStageSampledImages)
{
	return fragmentSamplerCount(true, true) <= maxPerStageSamplers
	    && fragmentSamplerCount(true, true) <= maxPerStageSampledImages;
}
constexpr bool landscapeWeightsFit(uint32_t maxPerStageSamplers, uint32_t maxPerStageSampledImages,
                                   bool withFogVolume = true)
{
	return fragmentSamplerCount(true, withFogVolume) <= maxPerStageSamplers
	    && fragmentSamplerCount(true, withFogVolume) <= maxPerStageSampledImages;
}

static_assert(kBindingCount == 26, "material set 0: 23 canonical bindings + 3 cluster lists");
static_assert(countOf(DescKind::UniformBuffer) == 5);        // b0, b1, b3, b8, b9
static_assert(countOf(DescKind::CombinedImageSampler) == 18); // b2, b4-b7, b10-b18, b31-b34
static_assert(countOf(DescKind::StorageBuffer) == 3);         // b24-b26
static_assert(fragmentSamplerCount(true, true) == 18 && fragmentSamplerCount(true, false) == 17
              && fragmentSamplerCount(false, false) == 16,
              "without heLandscapeWeights AND heFogVolume the layout must fit the spec minimum of 16");
static_assert(!landscapeWeightsFit(16, 16, false) && landscapeWeightsFit(17, 17, false));
static_assert(!fogVolumeFit(17, 17) && fogVolumeFit(18, 18));
static_assert(countOf(DescKind::StorageBuffer, kPreClusterBindingCount) == 0,
              "the cluster lists must be the last rows (negative control slices them off)");

// ── Deferred lighting resolve, set 0 (Thema 150, docs/deferred-renderer-plan.md
// §10.3/§10.5) ─────────────────────────────────────────────────────────────────
// MaterialShaderLibrary::deferredResolve[Clustered](SpirV) keeps the canonical
// GLSL bindings (no pin): the lighting preamble's HeLighting and fixed samplers
// at the SAME numbers as the material set above, plus the resolve's own inputs —
// the G-buffer at 19..22 (heGB0..2, heGBDepth = the R32F GB3 target, plan §10.5
// way B) and HeResolve at 23 — and, in the clustered variant only, the light
// lists at 24..26. It declares neither the per-object U / HeParams nor heTex0 /
// heTexP0..3 / heLandscapeWeights, so it gets a layout of its own: the renderer
// builds VulkanRenderer::m_resolveSetLayout from this table and he_tests
// reflects both resolve variants against it (set, binding, kind AND stage).
// 15 combined samplers in the fragment stage — inside the spec minimum of 16,
// so no device needs a fallback here.
constexpr uint32_t kResolveGB0Binding   = 19; // heGB0: rgb BaseColor, a Metallic   (R8G8B8A8_SRGB)
constexpr uint32_t kResolveGB1Binding   = 20; // heGB1: rg oct normal, b Rough, a Spec (RGBA16F)
constexpr uint32_t kResolveGB2Binding   = 21; // heGB2: rgb Emissive, a Material-AO  (RGBA16F)
constexpr uint32_t kResolveDepthBinding = 22; // heGBDepth: GB3, gl_FragCoord.z      (R32F)
constexpr uint32_t kResolveUboBinding   = 23; // HeResolve

inline constexpr Binding kResolveBindings[] = {
	{  0, DescKind::UniformBuffer,        kStageFragment }, // HeLighting (the resolve's own block)
	{ 10, DescKind::CombinedImageSampler, kStageFragment }, // heGIShadow
	{ 11, DescKind::CombinedImageSampler, kStageFragment }, // heGILocal
	{ 12, DescKind::CombinedImageSampler, kStageFragment }, // heCsm
	{ 13, DescKind::CombinedImageSampler, kStageFragment }, // heLocalShadow
	{ 15, DescKind::CombinedImageSampler, kStageFragment }, // heSkyEnv
	{ 16, DescKind::CombinedImageSampler, kStageFragment }, // heAO
	{ 17, DescKind::CombinedImageSampler, kStageFragment }, // heGIIrradiance
	{ 18, DescKind::CombinedImageSampler, kStageFragment }, // heGIVisibility
	{ kResolveGB0Binding,   DescKind::CombinedImageSampler, kStageFragment },
	{ kResolveGB1Binding,   DescKind::CombinedImageSampler, kStageFragment },
	{ kResolveGB2Binding,   DescKind::CombinedImageSampler, kStageFragment },
	{ kResolveDepthBinding, DescKind::CombinedImageSampler, kStageFragment },
	{ kResolveUboBinding,   DescKind::UniformBuffer,        kStageFragment },
	{ 31, DescKind::CombinedImageSampler, kStageFragment }, // heSSRFwd
	{ 32, DescKind::CombinedImageSampler, kStageFragment }, // heGIReflFwd
	{ 33, DescKind::CombinedImageSampler, kStageFragment }, // heCloudShadow
	// ── the clustered resolve's light lists (keep these LAST, as above) ──
	{ kClusterLightsBinding, DescKind::StorageBuffer, kStageFragment },
	{ kClusterGridBinding,   DescKind::StorageBuffer, kStageFragment },
	{ kClusterIdxBinding,    DescKind::StorageBuffer, kStageFragment },
};

constexpr uint32_t kResolveBindingCount           = sizeof(kResolveBindings) / sizeof(kResolveBindings[0]);
constexpr uint32_t kResolvePreClusterBindingCount = kResolveBindingCount - 3;

constexpr uint32_t resolveCountOf(DescKind kind, uint32_t bindingCount = kResolveBindingCount)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < bindingCount; ++i)
		if (kResolveBindings[i].kind == kind) ++n;
	return n;
}

static_assert(kResolveBindingCount == 20, "resolve set 0: 17 canonical bindings + 3 cluster lists");
static_assert(resolveCountOf(DescKind::UniformBuffer) == 2);          // b0, b23
static_assert(resolveCountOf(DescKind::CombinedImageSampler) == 15);  // b10-b13, b15-b22, b31-b33
static_assert(resolveCountOf(DescKind::StorageBuffer) == 3);          // b24-b26
static_assert(resolveCountOf(DescKind::StorageBuffer, kResolvePreClusterBindingCount) == 0,
              "the cluster lists must be the last rows (the 8-light resolve uses none of them)");

// ── Volumetric fog passes, set 0 (Thema 225 step 5, docs/volumetric-fog-plan.md) ───────────
// MaterialShaderLibrary::fogInject / fogIntegrate / fogApply (Backend::SpirV) keep the canonical
// bindings of FogVolumeShaderSource.h (no pins), so each pass' descriptor-set layout is a table
// like the two above: VulkanRenderer::createFogPipelines turns them into the three layouts and
// he_tests (test_fog_volume.cpp) reflects each module's STATICALLY used descriptors against them.
// The inject kernel pulls in the whole lighting preamble, so it declares every fixed sampler of
// it (10/11/16/17/18/31/32/33 are GI masks, AO, probes, SSR, GI reflections and the cloud shadow:
// white defaults, never sampled with their gate at 0) next to the ones it really reads.
// Integrate and Apply fetch the depth / the inject volume with texelFetch on SEPARATE images
// (GL_EXT_samplerless_texture_functions): SampledImage, no sampler.
enum class FogDesc : uint8_t { Ubo, Cis, SampledImage, StorageImage };

constexpr uint32_t kStageCompute = 0x20; // VK_SHADER_STAGE_COMPUTE_BIT

struct FogBinding
{
	uint32_t binding;
	FogDesc  kind;
	uint32_t stages;
};

inline constexpr FogBinding kFogInjectBindings[] = {
	{  0, FogDesc::Ubo,          kStageCompute }, // HeLighting
	{ 10, FogDesc::Cis,          kStageCompute }, // heGIShadow   (white)
	{ 11, FogDesc::Cis,          kStageCompute }, // heGILocal    (white)
	{ 12, FogDesc::Cis,          kStageCompute }, // heCsm        (2D array)
	{ 13, FogDesc::Cis,          kStageCompute }, // heLocalShadow (2D array)
	{ 15, FogDesc::Cis,          kStageCompute }, // heSkyEnv     (cube)
	{ 16, FogDesc::Cis,          kStageCompute }, // heAO         (white)
	{ 17, FogDesc::Cis,          kStageCompute }, // heGIIrradiance (white)
	{ 18, FogDesc::Cis,          kStageCompute }, // heGIVisibility (white)
	{ 31, FogDesc::Cis,          kStageCompute }, // heSSRFwd     (white)
	{ 32, FogDesc::Cis,          kStageCompute }, // heGIReflFwd  (white)
	{ 33, FogDesc::Cis,          kStageCompute }, // heCloudShadow (white)
	{ 35, FogDesc::Cis,          kStageCompute }, // heFogNoise   (3D, 64^3)
	{ 36, FogDesc::Ubo,          kStageCompute }, // HeFogVol
	{ 37, FogDesc::Cis,          kStageCompute }, // heFogHistory (3D)
	{ 38, FogDesc::StorageImage, kStageCompute }, // heFogOut     (3D, writeonly)
};
inline constexpr FogBinding kFogIntegrateBindings[] = {
	{ 36, FogDesc::Ubo,          kStageCompute }, // HeFogVol
	{ 37, FogDesc::SampledImage, kStageCompute }, // heFogInjected (3D, texelFetch)
	{ 38, FogDesc::StorageImage, kStageCompute }, // heFogOut      (3D, writeonly)
};
inline constexpr FogBinding kFogApplyBindings[] = {
	{ 15, FogDesc::Cis,          kStageFragment }, // heSkyEnv (cube): the far field's colour
	{ 34, FogDesc::Cis,          kStageFragment }, // heFogIntegrated (3D, trilinear)
	{ 36, FogDesc::Ubo,          kStageFragment }, // HeFogVol
	{ 39, FogDesc::SampledImage, kStageFragment }, // heFogDepth (scene depth, texelFetch)
};
constexpr uint32_t kFogInjectBindingCount    = sizeof(kFogInjectBindings) / sizeof(kFogInjectBindings[0]);
constexpr uint32_t kFogIntegrateBindingCount = sizeof(kFogIntegrateBindings) / sizeof(kFogIntegrateBindings[0]);
constexpr uint32_t kFogApplyBindingCount     = sizeof(kFogApplyBindings) / sizeof(kFogApplyBindings[0]);

constexpr uint32_t fogCountOf(const FogBinding* t, uint32_t n, FogDesc kind)
{
	uint32_t c = 0;
	for (uint32_t i = 0; i < n; ++i)
		if (t[i].kind == kind) ++c;
	return c;
}
// Per frame slot the pool must hold one set of each pass: its descriptors of each kind.
static_assert(kFogInjectBindingCount == 16 && kFogIntegrateBindingCount == 3 && kFogApplyBindingCount == 4);
static_assert(fogCountOf(kFogInjectBindings, kFogInjectBindingCount, FogDesc::Cis) == 13,
              "13 combined samplers in the compute stage: inside the spec minimum of 16");
} // namespace HE::vkmat
