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

void FillMaterialLightWindow(const RenderWorld&               rw,
                             MaterialShaderLibrary::Lighting& out,
                             bool                             localShadowsActive)
{
	const int lc = std::min(static_cast<int>(rw.lights.size()), kMaxLightWindow);
	for (int li = 0; li < lc; ++li)
	{
		const LightData& ld = rw.lights[li];
		out.lightPos[li][0] = ld.position.x;  out.lightPos[li][1] = ld.position.y;
		out.lightPos[li][2] = ld.position.z;  out.lightPos[li][3] = static_cast<float>(ld.type);
		out.lightDir[li][0] = ld.direction.x; out.lightDir[li][1] = ld.direction.y;
		out.lightDir[li][2] = ld.direction.z; out.lightDir[li][3] = ld.spotAngleCos;
		out.lightColor[li][0] = ld.color.r;   out.lightColor[li][1] = ld.color.g;
		out.lightColor[li][2] = ld.color.b;   out.lightColor[li][3] = ld.intensity;
		out.lightParams[li][0] = ld.range;
		// y = local shadow atlas base layer + 1 (0 = none) — the +1 keeps
		// zero-initialised Lighting fills (previews, UI) safe.
		out.lightParams[li][1] = (localShadowsActive && ld.shadowLayer >= 0)
			? static_cast<float>(ld.shadowLayer + 1) : 0.0f;
	}
	out.counts[0] = static_cast<float>(lc);
}

} // namespace HE
