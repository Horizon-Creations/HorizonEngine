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
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace HE::vkmat
{
enum class DescKind : uint8_t { UniformBuffer, CombinedImageSampler, StorageBuffer };

constexpr uint32_t kStageVertex   = 0x01; // VK_SHADER_STAGE_VERTEX_BIT
constexpr uint32_t kStageFragment = 0x10; // VK_SHADER_STAGE_FRAGMENT_BIT

struct Binding
{
	uint32_t binding;
	DescKind kind;
	uint32_t stages;
};

constexpr uint32_t kClusterLightsBinding = 24; // HeClusterLights { vec4 clLights[]; }
constexpr uint32_t kClusterGridBinding   = 25; // HeClusterGrid   { uvec2 clGrid[]; }
constexpr uint32_t kClusterIdxBinding    = 26; // HeClusterIdx    { uint clIdx[]; }

inline constexpr Binding kBindings[] = {
	{  0, DescKind::UniformBuffer,        kStageFragment }, // HeLighting
	{  1, DescKind::UniformBuffer,        kStageVertex   }, // U (per object)
	{  2, DescKind::CombinedImageSampler, kStageFragment }, // heTex0
	{  3, DescKind::UniformBuffer,        kStageFragment }, // HeParams
	{  4, DescKind::CombinedImageSampler, kStageFragment }, // heTexP0
	{  5, DescKind::CombinedImageSampler, kStageFragment }, // heTexP1
	{  6, DescKind::CombinedImageSampler, kStageFragment }, // heTexP2
	{  7, DescKind::CombinedImageSampler, kStageFragment }, // heTexP3
	// 8/9 for the WPO custom vertex, which reads HeLighting/HeParams in the
	// VERTEX stage at those slots (MaterialShaderLibrary.cpp wpoDeclarations).
	// Harmless for the standard vertex, which references neither.
	{  8, DescKind::UniformBuffer,        kStageVertex   }, // HeLighting (WPO VS)
	{  9, DescKind::UniformBuffer,        kStageVertex   }, // HeParams   (WPO VS)
	{ 10, DescKind::CombinedImageSampler, kStageFragment }, // heGIShadow (GI sun mask)
	{ 11, DescKind::CombinedImageSampler, kStageFragment }, // heGILocal  (GI local mask)
	{ 12, DescKind::CombinedImageSampler, kStageFragment }, // heCsm (CSM fallback, 2D array)
	// heSSRFwd (docs/ssr-cross-backend-plan.md B5 / §2.3 head 1): the
	// preamble's reflection cascade declares this sampler unconditionally and
	// heLight.ssr.x decides whether it is read. (Bindings 15-18, 32 and 33 of
	// the preamble are still not in this layout — a pre-existing gap, not an
	// SSR or a cluster one.)
	{ 31, DescKind::CombinedImageSampler, kStageFragment }, // heSSRFwd
	// heLocalShadow (sampler2DArray): the local (point/spot) shadow atlas the
	// built-in scene shader samples at binding 9. Gated by lightParams[i].y.
	{ 13, DescKind::CombinedImageSampler, kStageFragment }, // heLocalShadow
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

static_assert(kBindingCount == 18, "material set 0: 15 canonical bindings + 3 cluster lists");
static_assert(countOf(DescKind::UniformBuffer) == 5);        // b0, b1, b3, b8, b9
static_assert(countOf(DescKind::CombinedImageSampler) == 10); // b2, b4-b7, b10-b13, b31
static_assert(countOf(DescKind::StorageBuffer) == 3);         // b24-b26
static_assert(countOf(DescKind::StorageBuffer, kPreClusterBindingCount) == 0,
              "the cluster lists must be the last rows (negative control slices them off)");
} // namespace HE::vkmat
