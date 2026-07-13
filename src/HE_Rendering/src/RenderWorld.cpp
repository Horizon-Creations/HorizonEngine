#include "HorizonRendering/RenderWorld.h"

void RenderWorld::clear()
{
	objects.clear();
	skinnedObjects.clear();
	lights.clear();
	uiObjects.clear();
	particleBatches.clear();
	camera = CameraData{};
	shadow = ShadowData{};
	sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);
	ambient      = glm::vec3(0.03f, 0.035f, 0.05f);
}
