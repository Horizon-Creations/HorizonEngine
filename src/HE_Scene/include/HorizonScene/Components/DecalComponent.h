#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>

// Projected decal (deferred render path, docs/deferred-renderer-plan.md P7
// follow-up): the entity's transform is a unit-cube projector — everything the
// box overlaps gets `color` (optionally × `textureId`) alpha-blended into the
// G-buffer's base colour, projected along the box's local Y axis. v1 renders on
// Metal in the deferred tile mode only; forward paths ignore the component.
struct DecalComponent
{
	glm::vec4 color     = glm::vec4(1.0f); // rgb tint, a = opacity
	float     roughness = 0.5f;            // reserved (v1 does not touch GB1)
	HE::UUID  textureId;                   // optional texture asset ({} = flat colour)
};
