#include "HorizonRendering/LightPacking.h"
#include <algorithm>

namespace HE
{

PackedLightArray BuildPackedLightArray(const RenderWorld& rw)
{
	PackedLightArray out;
	for (const LightData& l : rw.lights)
	{
		if (out.count >= kMaxLightWindow) break;
		if ((l.type != 1 && l.type != 2) || l.intensity <= 0.0f) continue;
		out.posRange [out.count] = glm::vec4(l.position, std::max(l.range, 1e-4f));
		out.colorType[out.count] = glm::vec4(l.color * l.intensity, static_cast<float>(l.type));
		out.dirCos   [out.count] = glm::vec4(l.direction, l.spotAngleCos);
		++out.count;
	}
	return out;
}

PackedLocalShadowLights BuildMaskedLocalLights(const RenderWorld& rw)
{
	PackedLocalShadowLights out;
	int localCount = 0;
	const int windowCount = std::min(static_cast<int>(rw.lights.size()), kMaxLightWindow);
	for (int li = 0; li < windowCount; ++li)
	{
		const LightData& l = rw.lights[li];
		if (l.type == 0) continue;
		if (localCount < kMaxMaskedLocalLights)
			out.posRange[localCount] = glm::vec4(l.position, std::max(l.range, 1e-4f));
		++localCount;
	}
	out.count = std::min(localCount, kMaxMaskedLocalLights);
	return out;
}

} // namespace HE
