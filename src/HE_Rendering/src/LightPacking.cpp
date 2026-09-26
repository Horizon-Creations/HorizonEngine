#include "HorizonRendering/LightPacking.h"
#include <algorithm>
#include <cmath>

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

void FillMaterialWind(const ::EnvironmentSettings& env, MaterialShaderLibrary::Lighting& out)
{
	const float rad = glm::radians(env.windDirection);
	out.sunColor[3] = std::sin(rad);
	out.ambient[3]  = -std::cos(rad);
	out.camPos[3]   = std::max(env.windSpeed, 0.0f);
}

ClusterLightBuild BuildClusterLights(const RenderWorld& rw,
                                     bool               localShadowsActive,
                                     bool               giMasksValid)
{
	ClusterLightBuild out;
	out.grid.assign(kClusterCount, glm::uvec2(0u, 0u));

	const glm::mat4 viewProj = rw.camera.projection * rw.camera.view;
	const glm::vec3 camPos   = rw.camera.position;
	const glm::vec3 camFwd   = -glm::normalize(glm::vec3(glm::inverse(rw.camera.view)[2]));
	const float sliceScale   = static_cast<float>(kClusterGridZ) / std::log(kClusterFar / kClusterNear);

	// Per-cell light lists, flattened below. A few hundred lights × a few
	// touched cells each — rebuilt fresh every frame, no GPU sync hazards.
	std::vector<std::vector<uint32_t>> cells(kClusterCount);
	size_t indexTotal = 0;

	// GI local-mask channel bookkeeping: the mask covers the first
	// kMaxMaskedLocalLights NON-directional lights of the first-kMaxLightWindow
	// window, counted exactly like BuildMaskedLocalLights / heLitP's localIdx.
	int extractorIndex = -1;
	int windowLocalIdx = 0;
	for (const LightData& l : rw.lights)
	{
		++extractorIndex;
		int maskChannel = -1;
		if (l.type != 0 && extractorIndex < kMaxLightWindow)
		{
			if (giMasksValid && windowLocalIdx < kMaxMaskedLocalLights) maskChannel = windowLocalIdx;
			++windowLocalIdx;
		}
		if (l.type == 0) continue; // directional stays in the window
		if (out.lightCount >= kMaxClusteredLights) { ++out.droppedLights; continue; }

		const float range = std::max(l.range, 1e-4f);
		// Depth slice span along the camera forward.
		const float viewZ = glm::dot(l.position - camPos, camFwd);
		const float zMin  = viewZ - range, zMax = viewZ + range;
		if (zMax < kClusterNear || zMin > kClusterFar) continue; // outside the grid
		auto slice = [&](float z) {
			return std::clamp(static_cast<int>(std::log(std::max(z, kClusterNear) / kClusterNear)
			                                   * sliceScale), 0, kClusterGridZ - 1);
		};
		const int z0 = slice(zMin), z1 = slice(zMax);

		// Screen rect from the 8 corners of the world-space bounding box. A
		// corner at/behind the near plane makes the projection unusable →
		// conservatively cover the whole screen for that light.
		float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
		bool fullRect = false;
		for (int c = 0; c < 8 && !fullRect; ++c)
		{
			const glm::vec3 corner = l.position + range * glm::vec3(
				(c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f);
			const glm::vec4 clip = viewProj * glm::vec4(corner, 1.0f);
			if (clip.w <= kClusterNear) { fullRect = true; break; }
			// Top-left uv origin (Metal/D3D/Vulkan fragment coordinates) — flip
			// v so the scatter and the shader's cluster pick agree.
			const float u = clip.x / clip.w * 0.5f + 0.5f;
			const float v = 1.0f - (clip.y / clip.w * 0.5f + 0.5f);
			u0 = std::min(u0, u); u1 = std::max(u1, u);
			v0 = std::min(v0, v); v1 = std::max(v1, v);
		}
		int x0 = 0, x1 = kClusterGridX - 1, y0 = 0, y1 = kClusterGridY - 1;
		if (!fullRect)
		{
			if (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f) continue; // off-screen
			x0 = std::clamp(static_cast<int>(u0 * kClusterGridX), 0, kClusterGridX - 1);
			x1 = std::clamp(static_cast<int>(u1 * kClusterGridX), 0, kClusterGridX - 1);
			y0 = std::clamp(static_cast<int>(v0 * kClusterGridY), 0, kClusterGridY - 1);
			y1 = std::clamp(static_cast<int>(v1 * kClusterGridY), 0, kClusterGridY - 1);
		}
		const size_t touched = static_cast<size_t>(z1 - z0 + 1)
		                     * static_cast<size_t>(y1 - y0 + 1)
		                     * static_cast<size_t>(x1 - x0 + 1);
		if (indexTotal + touched > static_cast<size_t>(kMaxClusterIndices))
		{
			++out.droppedLights; // whole light or nothing — see kMaxClusterIndices
			continue;
		}
		indexTotal += touched;

		const uint32_t li = static_cast<uint32_t>(out.lightCount++);
		out.lights.push_back(glm::vec4(l.position, static_cast<float>(l.type)));
		out.lights.push_back(glm::vec4(l.direction, l.spotAngleCos));
		out.lights.push_back(glm::vec4(l.color, l.intensity));
		out.lights.push_back(glm::vec4(range,
			(localShadowsActive && l.shadowLayer >= 0) ? static_cast<float>(l.shadowLayer + 1) : 0.0f,
			static_cast<float>(maskChannel + 1),
			0.0f));
		for (int z = z0; z <= z1; ++z)
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
					cells[(z * kClusterGridY + y) * kClusterGridX + x].push_back(li);
	}

	// Flatten: per-cluster {offset, count} + one index list.
	out.indices.reserve(indexTotal);
	for (int cIdx = 0; cIdx < kClusterCount; ++cIdx)
	{
		out.grid[cIdx] = glm::uvec2(static_cast<uint32_t>(out.indices.size()),
		                            static_cast<uint32_t>(cells[cIdx].size()));
		out.indices.insert(out.indices.end(), cells[cIdx].begin(), cells[cIdx].end());
	}
	if (out.lights.empty())  out.lights.push_back(glm::vec4(0.0f)); // never a 0-byte buffer
	if (out.indices.empty()) out.indices.push_back(0u);

	out.params = glm::vec4(static_cast<float>(kClusterGridX), static_cast<float>(kClusterGridY),
	                       static_cast<float>(kClusterGridZ), sliceScale);
	out.camFwd = glm::vec4(camFwd, kClusterNear);
	return out;
}

DirectionalLightWindow BuildDirectionalLightWindow(const RenderWorld& rw)
{
	DirectionalLightWindow out;
	for (const LightData& l : rw.lights)
	{
		if (l.type != 0) continue;
		if (out.count >= kMaxLightWindow) break;
		out.pos   [out.count] = glm::vec4(l.position,  0.0f);
		out.dir   [out.count] = glm::vec4(l.direction, l.spotAngleCos);
		out.color [out.count] = glm::vec4(l.color,     l.intensity);
		out.params[out.count] = glm::vec4(l.range, -1.0f, 0.0f, 0.0f);
		++out.count;
	}
	return out;
}

} // namespace HE
