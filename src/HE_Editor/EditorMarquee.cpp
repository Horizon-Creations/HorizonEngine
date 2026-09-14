#include "EditorMarquee.h"
#include <glm/vec4.hpp>
#include <algorithm>

namespace EditorMarquee
{

Rect Rect::fromCorners(glm::vec2 a, glm::vec2 b)
{
	Rect r;
	r.min = glm::min(a, b);
	r.max = glm::max(a, b);
	return r;
}

bool project(const glm::mat4& viewProj, const glm::vec3& world, glm::vec2& outUv)
{
	const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
	if (clip.w <= 1e-6f) return false;
	// NDC x/y in [-1,1] → uv with y flipped, the inverse of what the picking
	// ray does with the click position.
	outUv.x = (clip.x / clip.w) * 0.5f + 0.5f;
	outUv.y = 0.5f - (clip.y / clip.w) * 0.5f;
	return true;
}

bool encloses(const glm::mat4& viewProj, const Rect& frame,
              const HE::AABB& localBox, const glm::mat4& model)
{
	glm::vec2 uv;
	if (project(viewProj, glm::vec3(model[3]), uv) && frame.contains(uv)) return true;
	if (!localBox.isValid()) return false;

	// Every corner has to land inside — one corner behind the camera or outside
	// the frame is enough for the box to fail, so there is no "screen box" to
	// build first.
	for (int i = 0; i < 8; ++i)
	{
		const glm::vec3 corner{
			(i & 1) ? localBox.max.x : localBox.min.x,
			(i & 2) ? localBox.max.y : localBox.min.y,
			(i & 4) ? localBox.max.z : localBox.min.z,
		};
		const glm::vec3 world = glm::vec3(model * glm::vec4(corner, 1.0f));
		if (!project(viewProj, world, uv) || !frame.contains(uv)) return false;
	}
	return true;
}

} // namespace EditorMarquee
