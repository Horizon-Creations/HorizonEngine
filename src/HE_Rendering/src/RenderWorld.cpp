#include "HorizonRendering/RenderWorld.h"

void RenderWorld::clear()
{
	objects.clear();
	lights.clear();
	camera = CameraData{};
	shadow = ShadowData{};
	sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);
}
