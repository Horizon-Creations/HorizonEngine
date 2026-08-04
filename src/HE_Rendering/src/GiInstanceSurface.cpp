#include "HorizonRendering/GiInstanceSurface.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

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
	s.albedo   *= slotValue(ma->approxBaseColorSlot, ma->approxBaseColor);
	s.emissive  = slotValue(ma->approxEmissiveSlot,  ma->approxEmissive);
	return s;
}

} // namespace HE
