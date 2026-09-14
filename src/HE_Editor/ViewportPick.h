#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity, entt::registry
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <glm/glm.hpp>
#include <functional>

class RenderWorld;

// ── Click picking in the Scene window ────────────────────────────────────────
// "Which entity did the user click?" for the Scene viewport, answered against
// the RenderWorld snapshot the frame was drawn from — the same list the picture
// came out of, so what is on screen and what a click hits can never disagree.
//
// It used to be a lambda inside ViewportPanel::render, where nothing could ask
// it anything. It is a module now because the list it walks changed: the
// RenderExtractor pushes a billboard quad for every light, camera and audio
// source under the editor camera (its editor icons), and "a click on the lamp
// symbol selects the lamp" is a claim about geometry that a test should make,
// not a pair of eyes. Deliberately ImGui-free, like PreviewPick: ray + boxes in,
// an entity out.
//
// The rule, unchanged from the lambda: every object in the snapshot is tested
// with its LOCAL box under its own transform (exact for rotated objects); the
// nearest mesh hit wins. Terrain is a category of its own — a landscape chunk's
// box is huge and loose, its near face sits closer to the camera than a small
// prop resting on it, so terrain only answers when nothing else is under the
// cursor, and then it is the owning Landscape entity, never a raw chunk.
namespace ViewportPick
{
	// Local box for an object whose mesh asset the lookup cannot resolve: the
	// built-in fallback cube's own box, the shape such an object is drawn as.
	// Shared with the F-key framing so a click and a focus agree on a size.
	const HE::AABB& fallbackBox();

	// Local-space bounds of a mesh asset, or nullptr when it is not readable
	// (the object then measures as fallbackBox()). ViewportPanel serves this out
	// of its AABB cache; a test hands in whatever it likes.
	using BoxLookup = std::function<const HE::AABB*(const HE::UUID& meshId)>;

	// Nearest entity along a world-space ray, mesh before terrain. entt::null
	// when the ray hits nothing. `reg` is consulted only to tell terrain apart
	// from everything else.
	Entity pick(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes,
	            const glm::vec3& rayOrigin, const glm::vec3& rayDir);

	// The same for a screen position: `mouse` and `rectMin` in the same
	// coordinates (ImGui screen space, y down), `rectSize` the picture's size in
	// those units, `viewProj` the matrix it was drawn with.
	Entity pickAtScreen(const RenderWorld& snapshot, entt::registry& reg, const BoxLookup& boxes,
	                    const glm::mat4& viewProj, const glm::vec2& rectMin,
	                    const glm::vec2& rectSize, const glm::vec2& mouse);
}
