#include "HorizonRendering/RenderWorld.h"

void RenderWorld::clear()
{
	objects.clear();
	lights.clear();
	camera = CameraData{};
}
