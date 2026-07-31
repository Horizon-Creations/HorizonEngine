#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <functional>

// ── ScenePick ────────────────────────────────────────────────────────────────
// Ray → scene surface, against the exact objects a frame renders (the extracted
// RenderWorld), so what the cursor points at is what gets hit. The editor uses
// it as the collision probe behind drag-and-drop placement: an asset dropped into
// the viewport lands on the terrain/floor/table under the cursor instead of on an
// arbitrary plane.
//
// The intersection is triangle-exact, not box-exact: a bounding-box hit would
// float the asset above anything that isn't a filled cube, and terrain chunks
// have enormous boxes whose near face sits closer to the camera than the small
// mesh resting on the ground. Boxes are still the cheap reject in front of it.
//
// Mesh data is fetched through a caller-supplied lookup rather than a
// ContentManager dependency — the editor can then serve it straight from the
// bounds cache it already keeps for picking.

namespace HE::ScenePick
{

// Where one mesh asset's geometry lives, in whatever layout the caller has it:
// `positions` holds xyz at every `stride` floats (3 for the loose SoA form, 8 for
// the cooked interleaved one). `bounds` is optional (null = no cheap reject).
struct MeshGeometry
{
	const float*    positions   = nullptr;
	size_t          stride      = 3;
	size_t          vertexCount = 0;
	const uint32_t* indices     = nullptr;
	size_t          indexCount  = 0;
	const HE::AABB* bounds      = nullptr;
};

// Fills `out` for a mesh asset; false when the asset isn't available (not
// resident, wrong type) and the object should be skipped.
using MeshLookup = std::function<bool(const HE::UUID& meshAssetId, MeshGeometry& out)>;

struct SurfaceHit
{
	bool      hit      = false;
	glm::vec3 point    = {};      // world-space contact point
	glm::vec3 normal   = {};      // world-space surface normal, facing the ray
	float     distance = 0.0f;    // along `direction` as given (NOT re-normalised)
	uint32_t  entityId = 0;       // RenderObject::entityId of what was hit
};

// Nearest surface along the ray. `direction` need not be normalised — `distance`
// is measured in units of it, and `point` is origin + distance * direction.
HE_RENDERING_API SurfaceHit raycast(const RenderWorld& snapshot, const MeshLookup& lookup,
                                    const glm::vec3& origin, const glm::vec3& direction);

// Möller–Trumbore, exposed because the ray and the triangle may live in any
// shared space. `d` is not normalised, so `t` comes back in units of |d|.
HE_RENDERING_API bool rayTriangle(const glm::vec3& o, const glm::vec3& d,
                                  const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                  float& t);

} // namespace HE::ScenePick
