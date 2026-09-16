#include "HorizonRendering/ScenePick.h"
#include <cmath>
#include <limits>

namespace HE::ScenePick
{

bool rayTriangle(const glm::vec3& o, const glm::vec3& d,
                 const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                 float& t)
{
	const glm::vec3 e1 = b - a, e2 = c - a;
	const glm::vec3 p   = glm::cross(d, e2);
	const float     det = glm::dot(e1, p);
	if (std::abs(det) < 1e-9f) return false;   // ray parallel to the triangle's plane
	const float     invDet = 1.0f / det;
	const glm::vec3 tv = o - a;
	const float     u  = glm::dot(tv, p) * invDet;
	if (u < 0.0f || u > 1.0f) return false;
	const glm::vec3 q = glm::cross(tv, e1);
	const float     v = glm::dot(d, q) * invDet;
	if (v < 0.0f || u + v > 1.0f) return false;
	t = glm::dot(e2, q) * invDet;
	return t > 1e-6f;                          // strictly in front of the origin
}

SurfaceHit raycast(const RenderWorld& snapshot, const MeshLookup& lookup,
                   const glm::vec3& origin, const glm::vec3& direction,
                   const ObjectFilter& filter)
{
	SurfaceHit best;
	float      bestT = std::numeric_limits<float>::max();
	if (!lookup) return best;

	for (const RenderObject& obj : snapshot.objects)
	{
		if (obj.meshAssetId == HE::UUID{}) continue;
		if (filter && !filter(obj)) continue;
		MeshGeometry geo;
		if (!lookup(obj.meshAssetId, geo)) continue;
		if (!geo.positions || !geo.indices || geo.indexCount < 3 || geo.vertexCount == 0) continue;

		// Ray → object space. The SAME world direction vector is transformed for
		// every object, so each object-space `t` is still measured in world ray
		// parameters — comparing hits across differently-scaled objects is then a
		// plain float comparison, with no per-object renormalisation.
		const glm::mat4 invModel = glm::inverse(obj.transform);
		const glm::vec3 o = glm::vec3(invModel * glm::vec4(origin, 1.0f));
		const glm::vec3 d = glm::vec3(invModel * glm::vec4(direction, 0.0f));

		if (geo.bounds && geo.bounds->isValid())
		{
			float tBox = 0.0f;
			// A miss on the box is a miss on every triangle in it; a box entry
			// beyond the best hit so far cannot improve on it either.
			if (!geo.bounds->intersectRay(o, d, tBox) || tBox > bestT) continue;
		}

		auto vertexAt = [&](uint32_t i) {
			const float* p = geo.positions + static_cast<size_t>(i) * geo.stride;
			return glm::vec3(p[0], p[1], p[2]); };

		for (size_t i = 0; i + 2 < geo.indexCount; i += 3)
		{
			const uint32_t i0 = geo.indices[i], i1 = geo.indices[i + 1], i2 = geo.indices[i + 2];
			if (i0 >= geo.vertexCount || i1 >= geo.vertexCount || i2 >= geo.vertexCount)
				break; // index buffer doesn't match the vertex array — don't read past it
			const glm::vec3 a = vertexAt(i0), b = vertexAt(i1), c = vertexAt(i2);
			float t = 0.0f;
			if (!rayTriangle(o, d, a, b, c, t) || t >= bestT) continue;
			bestT         = t;
			best.hit      = true;
			best.entityId = obj.entityId;
			// Object → world for the normal: the inverse-transpose, so non-uniform
			// scale tilts the surface without tilting its normal the wrong way.
			const glm::vec3 nLocal = glm::cross(b - a, c - a);
			best.normal = glm::normalize(glm::vec3(glm::transpose(invModel) * glm::vec4(nLocal, 0.0f)));
		}
	}

	if (best.hit)
	{
		best.distance = bestT;
		best.point    = origin + bestT * direction;
		if (glm::dot(best.normal, direction) > 0.0f) best.normal = -best.normal; // face the ray
	}
	return best;
}

namespace
{
	// World point → picture coordinates. False when the point is behind the
	// camera (w <= 0): such a vertex has no place on the screen and must not be
	// compared as if it had one.
	bool project(const glm::mat4& viewProj, const glm::vec2& rectMin, const glm::vec2& rectSize,
	             const glm::vec3& world, glm::vec2& outScreen, float& outDepth)
	{
		const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
		if (clip.w <= 1e-6f) return false;
		const float nx = clip.x / clip.w, ny = clip.y / clip.w;
		outScreen = { rectMin.x + (nx * 0.5f + 0.5f) * rectSize.x,
		              rectMin.y + (0.5f - ny * 0.5f) * rectSize.y };
		outDepth  = clip.w;
		return true;
	}
}

VertexHit nearestVertex(const RenderWorld& snapshot, const MeshLookup& lookup,
                        const glm::mat4& viewProj,
                        const glm::vec2& rectMin, const glm::vec2& rectSize,
                        const glm::vec2& screen, float radiusPx,
                        const ObjectFilter& filter)
{
	VertexHit best;
	if (!lookup || radiusPx <= 0.0f) return best;
	float bestD2    = radiusPx * radiusPx;
	float bestDepth = std::numeric_limits<float>::max();

	for (const RenderObject& obj : snapshot.objects)
	{
		if (obj.meshAssetId == HE::UUID{}) continue;
		if (filter && !filter(obj)) continue;
		MeshGeometry geo;
		if (!lookup(obj.meshAssetId, geo)) continue;
		if (!geo.positions || geo.vertexCount == 0) continue;

		// Cheap reject: the object's box, projected. If every corner is in
		// front of the camera and the 2D rectangle they span stays clear of the
		// cursor's radius, no vertex inside can be closer. A corner behind the
		// camera makes the rectangle meaningless, and the object is then simply
		// walked.
		if (geo.bounds && geo.bounds->isValid())
		{
			glm::vec2 lo( std::numeric_limits<float>::max());
			glm::vec2 hi(-std::numeric_limits<float>::max());
			bool allInFront = true;
			for (int i = 0; i < 8 && allInFront; ++i)
			{
				const glm::vec3 corner((i & 1) ? geo.bounds->max.x : geo.bounds->min.x,
				                       (i & 2) ? geo.bounds->max.y : geo.bounds->min.y,
				                       (i & 4) ? geo.bounds->max.z : geo.bounds->min.z);
				glm::vec2 s; float depth;
				if (!project(viewProj, rectMin, rectSize,
				             glm::vec3(obj.transform * glm::vec4(corner, 1.0f)), s, depth))
					allInFront = false;
				else { lo = glm::min(lo, s); hi = glm::max(hi, s); }
			}
			if (allInFront && (screen.x < lo.x - radiusPx || screen.x > hi.x + radiusPx ||
			                   screen.y < lo.y - radiusPx || screen.y > hi.y + radiusPx))
				continue;
		}

		const glm::mat4 toClip = viewProj * obj.transform;
		for (size_t i = 0; i < geo.vertexCount; ++i)
		{
			const float* p = geo.positions + i * geo.stride;
			const glm::vec3 local(p[0], p[1], p[2]);
			glm::vec2 s; float depth;
			if (!project(toClip, rectMin, rectSize, local, s, depth)) continue;
			const glm::vec2 d  = s - screen;
			const float     d2 = d.x * d.x + d.y * d.y;
			// Strictly closer on screen wins; the same pixel goes to the nearer
			// vertex, so a shared edge seen edge-on snaps to its front.
			if (d2 > bestD2 || (d2 == bestD2 && depth >= bestDepth)) continue;
			bestD2          = d2;
			bestDepth       = depth;
			best.hit        = true;
			best.point      = glm::vec3(obj.transform * glm::vec4(local, 1.0f));
			best.distancePx = std::sqrt(d2);
			best.entityId   = obj.entityId;
		}
	}
	return best;
}

} // namespace HE::ScenePick
