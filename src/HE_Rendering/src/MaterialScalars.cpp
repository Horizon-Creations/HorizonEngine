#include "HorizonRendering/MaterialScalars.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

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
	for (RenderObject& obj : world.objects) resolveObject(cm, obj);
	for (SkinnedRenderObject& obj : world.skinnedObjects) resolveObject(cm, obj);
}

} // namespace HE
