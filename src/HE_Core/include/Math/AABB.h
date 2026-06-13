#pragma once
#include <glm/glm.hpp>
#include <limits>

namespace HE
{

// Axis-aligned bounding box. Default-constructed it is *inverted* (empty) so
// expand() can be used to build it incrementally.
struct AABB
{
	glm::vec3 min{  std::numeric_limits<float>::max() };
	glm::vec3 max{ -std::numeric_limits<float>::max() };

	bool isValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }

	void expand(const glm::vec3& p)
	{
		min = glm::min(min, p);
		max = glm::max(max, p);
	}

	void expand(const AABB& other)
	{
		if (!other.isValid()) return;
		expand(other.min);
		expand(other.max);
	}

	glm::vec3 center()  const { return (min + max) * 0.5f; }
	glm::vec3 extents() const { return (max - min) * 0.5f; }

	// Builds the box from a flat xyz position array (3 floats per vertex).
	static AABB fromPositions(const float* xyz, size_t vertexCount)
	{
		AABB box;
		for (size_t i = 0; i < vertexCount; ++i)
			box.expand({ xyz[i*3+0], xyz[i*3+1], xyz[i*3+2] });
		return box;
	}

	// World-space AABB of this box under an affine transform (8-corner method).
	AABB transformed(const glm::mat4& m) const
	{
		AABB out;
		if (!isValid()) return out;
		for (int i = 0; i < 8; ++i)
		{
			const glm::vec3 corner{
				(i & 1) ? max.x : min.x,
				(i & 2) ? max.y : min.y,
				(i & 4) ? max.z : min.z,
			};
			out.expand(glm::vec3(m * glm::vec4(corner, 1.0f)));
		}
		return out;
	}

	// Slab test. On hit returns true and writes the entry distance along the
	// ray (>= 0) to tOut. `dir` does not need to be normalised.
	bool intersectRay(const glm::vec3& origin, const glm::vec3& dir, float& tOut) const
	{
		float tMin = 0.0f;
		float tMax = std::numeric_limits<float>::max();
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::abs(dir[axis]) < 1e-9f)
			{
				// Ray parallel to the slab — must already be inside it
				if (origin[axis] < min[axis] || origin[axis] > max[axis])
					return false;
				continue;
			}
			const float inv = 1.0f / dir[axis];
			float t0 = (min[axis] - origin[axis]) * inv;
			float t1 = (max[axis] - origin[axis]) * inv;
			if (t0 > t1) std::swap(t0, t1);
			tMin = std::max(tMin, t0);
			tMax = std::min(tMax, t1);
			if (tMin > tMax)
				return false;
		}
		tOut = tMin;
		return true;
	}
};

} // namespace HE
