#include "HorizonRendering/RenderWorld.h"

void RenderWorld::clear()
{
	objects.clear();
	skinnedObjects.clear();
	lights.clear();
	decals.clear();
	uiObjects.clear();
	particleBatches.clear();
	landscapes.clear();
	camera = CameraData{};
	shadow = ShadowData{};
	sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);
	ambient      = glm::vec3(0.03f, 0.035f, 0.05f);
}

// See the header for the full "why" — this pick is load-bearing for GI shadows,
// the probe bounce and every backend's material sun uniform.
bool RenderWorld::dominantDirectionalLight(glm::vec3& towardOut,
                                           glm::vec3& colorIntensityOut) const
{
	const LightData* best = nullptr;
	for (const LightData& l : lights)
		if (l.type == 0 && l.intensity > 0.0f && (!best || l.intensity > best->intensity))
			best = &l;
	if (!best || glm::dot(best->direction, best->direction) < 1e-8f)
	{
		towardOut         = glm::normalize(sunDirection);
		colorIntensityOut = glm::vec3(0.0f);
		return false;
	}
	towardOut         = -glm::normalize(best->direction); // LightData.direction = light travel direction
	colorIntensityOut = best->color * best->intensity;
	return true;
}
