#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <Types/UUID.h>
#include <Math/Math.h>

class ContentManager; // global namespace (HE_Core's ContentManager is not namespaced)

// ─── Per-draw PBR material scalars ────────────────────────────────────────────
// GL and Metal resolve baseColor/metallic/roughness/opacity per DrawCall from
// dc.materialAssetId (their ResolveMaterialParams) and classify opaque vs.
// blended only afterwards. D3D11, D3D12 and Vulkan draw with the scalars the
// DrawCall carries and split through RenderSorter::partitionByOpacity BEFORE
// any draw runs, so the scalars have to be right on the RenderWorld already:
// per object, per material slot (a per-slot override is another material than
// the whole mesh's) and per skinned object. GeometryPass then copies them into
// every DrawCall / SkinnedDrawCall it records.
//
// Before this existed, those three backends resolved the WHOLE-MESH material
// onto the object only: a slot overridden with another material drew with the
// whole mesh's colour and opacity class, a skinned mesh with a material
// override always drew white, and a Translucent (blendMode 2) material at
// opacity 1 landed in the opaque pass.
namespace HE
{

// ResolveMaterialParams, shared: false (outputs untouched) when the id is null,
// `cm` is null or the material is not loaded yet — the caller keeps its
// defaults. A Translucent blend mode forces the sorted alpha-blend pass even at
// opacity 1 (clamped to 0.998), exactly like GL/Metal: the material's own
// output alpha then does the actual blending.
HE_RENDERING_API bool resolveMaterialScalars(const ContentManager* cm, const HE::UUID& materialId,
                                             glm::vec3& outBaseColor, float& outMetallic,
                                             float& outRoughness, float& outOpacity);

// Every object, every material slot of a multi-section object and every
// skinned object (and its slots) gets the scalars of ITS OWN material. An
// entry whose material does not resolve keeps what it has (the RenderObject /
// RenderSection defaults). Call once per frame after extraction, before the
// render graph runs.
HE_RENDERING_API void resolveWorldMaterialScalars(RenderWorld& world, const ContentManager* cm);

} // namespace HE
