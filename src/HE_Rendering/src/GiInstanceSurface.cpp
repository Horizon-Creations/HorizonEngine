#include "HorizonRendering/GiInstanceSurface.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <algorithm>

namespace HE
{

GiInstanceSurface giInstanceSurface(const RenderObject& obj, const ContentManager* cm)
{
	GiInstanceSurface s;
	s.albedo = obj.baseColor * glm::vec3(obj.instanceTint);
	if (!cm || obj.materialAssetId == HE::UUID{}) return s;
	const MaterialAsset* ma = cm->getMaterial(obj.materialAssetId);
	if (!ma) return s;
	if (ma->customShaderFragGlsl.empty())
	{
		// Plain material: the asset's own PBR scalars ARE the surface.
		s.albedo *= glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2]);
		s.metallic  = ma->metallic;
		s.roughness = ma->roughness;
		return s;
	}
	// Node-graph material: the CPU fold of its BaseColor/Emissive pins. A pin
	// driven by a Param node folds to a SLOT INDEX instead of a constant — read
	// the live slot value so editor slider drags and per-entity overrides show
	// up in the bounce/reflection immediately.
	s.metallic  = ma->approxMetallic;   // constants-only fold (no live slot)
	s.roughness = ma->approxRoughness;
	auto slotValue = [&](int32_t slot, const float fallback[3]) -> glm::vec3
	{
		if (slot >= 0)
		{
			const size_t base = static_cast<size_t>(slot) * 4;
			if (obj.paramOverride.size() >= base + 3)
				return { obj.paramOverride[base], obj.paramOverride[base + 1],
				         obj.paramOverride[base + 2] };
			if (ma->shaderParamData.size() >= base + 3)
				return { ma->shaderParamData[base], ma->shaderParamData[base + 1],
				         ma->shaderParamData[base + 2] };
		}
		return { fallback[0], fallback[1], fallback[2] };
	};
	// A Landscape Layer Blend BaseColor has no single folded colour — the shader
	// picks per texel from the terrain's paint. Blend the per-layer folds by that
	// terrain's AVERAGE weights (carried on the object) so a landscape painted
	// all-grass reflects grass, not the average of every layer it declares.
	// Unpainted objects carry { 1, 0, 0, 0 } = layer 0, which is exactly what the
	// shader's 1×1 default weightmap resolves to.
	if (ma->approxLayerCount > 0)
	{
		const int   n = std::min(ma->approxLayerCount, 4);
		glm::vec3   blend(0.0f);
		float       wsum = 0.0f;
		for (int i = 0; i < n; ++i)
		{
			const float w = obj.landscapeLayerWeights[i];
			if (w <= 0.0f) continue;
			blend += glm::vec3(ma->approxLayerColor[i][0], ma->approxLayerColor[i][1],
			                   ma->approxLayerColor[i][2]) * w;
			wsum  += w;
		}
		// No weight on any DECLARED layer (paint that only touches channels the
		// material doesn't use) → the layer average the fold already computed.
		s.albedo *= (wsum > 1e-4f) ? blend / wsum
		                           : glm::vec3(ma->approxBaseColor[0], ma->approxBaseColor[1],
		                                       ma->approxBaseColor[2]);
		s.emissive = slotValue(ma->approxEmissiveSlot, ma->approxEmissive);
		return s;
	}
	s.albedo   *= slotValue(ma->approxBaseColorSlot, ma->approxBaseColor);
	s.emissive  = slotValue(ma->approxEmissiveSlot,  ma->approxEmissive);
	return s;
}

} // namespace HE
