#pragma once
#include "RenderObject.h"
#include "RenderWorld.h"
#include <glm/glm.hpp>
#include <cmath>
#include <cstdlib>

// ─── Which objects enter the GI acceleration structure, shared by all backends ─
// The GI kernels (sun shadow, probe bounce) walk every instance per ray, and the
// instance list used to be every shadow caster in the scene, UNCULLED: a 2000 m
// landscape of 256 chunks put 257 instances in front of every ray, while only a
// few dozen lie near the camera (landscape audit 2026-10-27, §6). Distant things
// matter little to the picture, so the list is thinned by distance:
//
//   keep   a caster whose bounding sphere reaches within `range` of the camera, and
//   keep   a caster that lies TOWARD the sun from the camera inside the same radius
//          of the camera-to-sun line — the only far geometry a receiver near the
//          camera can be shadowed by (sun rays are 10 km long).
//
// Not applied while GI reflections are on (a mirror shows the far scene, so the far
// scene has to stay traceable) and never to an object whose bounds are unresolved
// (invalid box = never culled, as everywhere else). The probe-grid fit uses the full
// object list, so culling never moves the grid.
//
// An object's mesh is already the LOD LODSystem picked for its camera distance, so a
// far caster that stays in is also a coarse one (fewer BVH triangles); only the
// instance COUNT needed this cut.
//
// HE_GI_CASTER_RANGE=<metres> overrides the default; 0 turns the cut off.
namespace HE
{

inline constexpr float kGiCasterDefaultRange = 500.0f;

inline float GiCasterRangeMeters()
{
	static const float r = []
	{
		if (const char* v = std::getenv("HE_GI_CASTER_RANGE"); v && *v) return static_cast<float>(std::atof(v));
		return kGiCasterDefaultRange;
	}();
	return r;
}

struct GiCasterFilter
{
	float     range   = 0.0f;       // <= 0: keep everything
	glm::vec3 camera{ 0.0f };
	bool      haveSun = false;
	glm::vec3 toSun{ 0.0f, 1.0f, 0.0f };

	static GiCasterFilter For(const RenderWorld& rw, bool reflectionsOn)
	{
		GiCasterFilter f;
		f.range = reflectionsOn ? 0.0f : GiCasterRangeMeters();
		f.camera = rw.camera.position;
		glm::vec3 color;
		f.haveSun = rw.dominantDirectionalLight(f.toSun, color);
		return f;
	}

	bool keeps(const RenderObject& obj) const
	{
		if (range <= 0.0f || !obj.worldBounds.isValid()) return true;
		const glm::vec3 d = obj.worldBounds.center() - camera;
		const float     r = glm::length(obj.worldBounds.extents());
		if (glm::length(d) - r <= range) return true;
		if (haveSun)
		{
			const float t = glm::dot(d, toSun);
			if (t > 0.0f && glm::length(d - t * toSun) - r <= range) return true;
		}
		return false;
	}
};

} // namespace HE
