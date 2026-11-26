#pragma once
#include <Math/Math.h>
#include <cstring>
#include <material/MaterialShaderLibrary.h>

// ─── The deferred resolve's world position, camera-relative (topic 355, step 3) ──────────────────────────────────────────────
// The resolve builds a surface's world position as invViewProj * clip, divided by w. With the camera far from the world's
// origin the full inverse holds numbers of the size camera * (1 / near) in its last columns, and the GPU's float product has to
// cancel them down to a position: with the editor camera 70 km out (origin 0) a pixel 400 m away came back 3.4 m too low. The
// tile ground casts into the shadow cascades, so it shadowed itself in that wedge (the black tiles of Catania view 5).
// Here the matrix has the camera's translation taken out, so its numbers stay the size of the view distance, and the
// translation travels as ResolveUniforms::camOrigin, which the shader adds AFTER the divide (a position in float is exact to
// about 8 mm at 70 km, which is all that is left).
//
// Header-only like ClipSpace.h: the backend static libraries reuse this include path without linking HorizonRendering.
// A backend that does not call this keeps the absolute matrix and camOrigin 0, as before.
namespace HE
{

// `viewProj` is the matrix the G-buffer's depth was rasterised with (jitter included), `camera` the position its view matrix
// looks from. viewProj * translate(camera) is the same matrix with a view that has no translation. Inverted in double: the
// float inverse of the small-number matrix is fine, but there is no reason to take its rounding on top.
inline void setResolveCameraRelative(MaterialShaderLibrary::ResolveUniforms& ru, const glm::mat4& viewProj, const glm::vec3& camera)
{
	const glm::dmat4 rel = glm::dmat4(viewProj) * glm::translate(glm::dmat4(1.0), glm::dvec3(camera));
	const glm::mat4 inv(glm::inverse(rel));
	std::memcpy(ru.invViewProj, &inv[0][0], sizeof(ru.invViewProj));
	ru.camOrigin[0] = camera.x;
	ru.camOrigin[1] = camera.y;
	ru.camOrigin[2] = camera.z;
}

} // namespace HE
