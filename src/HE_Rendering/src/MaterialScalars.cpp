#include "HorizonRendering/MaterialScalars.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <JobSystem/JobSystem.h>

#include <algorithm>

namespace HE
{

bool resolveMaterialScalars(const ContentManager* cm, const HE::UUID& materialId,
                            glm::vec3& outBaseColor, float& outMetallic,
                            float& outRoughness, float& outOpacity)
{
	if (materialId == HE::UUID{} || !cm) return false;
	const MaterialAsset* mat = cm->getMaterial(materialId);
	if (!mat) return false; // not loaded yet — caller keeps defaults
	outBaseColor = { mat->baseColor[0], mat->baseColor[1], mat->baseColor[2] };
	outMetallic  = mat->metallic;
	outRoughness = mat->roughness;
	outOpacity   = mat->blendMode == 2 ? std::min(mat->opacity, 0.998f) : mat->opacity;
	return true;
}

namespace
{
template <class T>
void resolveInto(const ContentManager* cm, T& x)
{
	resolveMaterialScalars(cm, x.materialAssetId, x.baseColor, x.metallic, x.roughness, x.opacity);
}

void resolveObject(const ContentManager* cm, RenderObject& obj)
{
	resolveInto(cm, obj);
	for (RenderSection& sec : obj.sections) resolveInto(cm, sec);
}
} // namespace

void resolveWorldMaterialScalars(RenderWorld& world, const ContentManager* cm)
{
	if (!cm) return;
	// The objects' materials are a few hundred under tens of thousands of objects: blocks of them
	// on the workers (the content manager's getters only read, and nothing loads while the thread
	// that pumps the loaders waits in parallel_for), each with a direct-mapped memo of the scalars
	// of the materials it has seen. Same values as asking per object.
	constexpr size_t kBlock = 2048;
	const size_t     n = world.objects.size();
	if (n >= 2 * kBlock)
	{
		struct Memo { HE::UUID id; glm::vec3 color; float metallic, roughness, opacity; bool known = false, valid = false; };
		const size_t nBlocks = (n + kBlock - 1) / kBlock;
		parallel_for(nBlocks, [&](size_t b) {
			Memo memo[64];
			const size_t from = b * kBlock, to = std::min(n, from + kBlock);
			auto resolve = [&](auto& x)
			{
				if (x.materialAssetId == HE::UUID{}) return;
				Memo& m = memo[std::hash<HE::UUID>{}(x.materialAssetId) & 63u];
				if (!(m.valid && m.id == x.materialAssetId))
				{
					m.id = x.materialAssetId; m.valid = true;
					m.known = resolveMaterialScalars(cm, x.materialAssetId, m.color, m.metallic, m.roughness, m.opacity);
				}
				if (m.known) { x.baseColor = m.color; x.metallic = m.metallic; x.roughness = m.roughness; x.opacity = m.opacity; }
			};
			for (size_t i = from; i < to; ++i)
			{
				RenderObject& obj = world.objects[i];
				resolve(obj);
				for (RenderSection& sec : obj.sections) resolve(sec);
			}
		}, "MaterialScalars", 1);
	}
	else
		for (RenderObject& obj : world.objects) resolveObject(cm, obj);
	for (SkinnedRenderObject& obj : world.skinnedObjects) resolveObject(cm, obj);
}

} // namespace HE
