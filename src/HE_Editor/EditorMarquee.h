#pragma once
#include <Math/AABB.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// ── Rubber-band selection: the rule, without the rubber band ────────────────
// The Scene viewport lets a left-drag over empty space draw a frame and
// selects what is inside it. Which objects count as "inside" is the whole
// question, and it is answered here so a unit test can ask it with a camera and
// a box instead of a window and a mouse. Deliberately ImGui-free.
//
// The rule: an object is inside the frame when its whole bounding box projects
// inside it, OR when its pivot (the origin of its transform) does. The first
// half is what a frame around a cluster of props means; the second half keeps a
// prop that sticks out of the viewport — or one whose box is far bigger than
// the thing you are aiming at — reachable by framing where it stands. Anything
// behind the camera is never inside.
namespace EditorMarquee
{
	// A screen rectangle in normalised viewport coordinates: (0,0) top-left,
	// (1,1) bottom-right, the same space the picking ray is unprojected from.
	// Built from the two drag corners in any order.
	struct Rect
	{
		glm::vec2 min{ 0.0f };
		glm::vec2 max{ 0.0f };
		static Rect fromCorners(glm::vec2 a, glm::vec2 b);
		bool contains(glm::vec2 p) const
		{
			return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
		}
	};

	// Project a world point through `viewProj` into the rect's space. Returns
	// false for a point behind the camera (clip.w <= 0), whose projection would
	// otherwise land somewhere plausible-looking and wrong.
	bool project(const glm::mat4& viewProj, const glm::vec3& world, glm::vec2& outUv);

	// The rule above, for one object: its LOCAL box under `model`, pivot at
	// model[3]. An invalid box only leaves the pivot test.
	bool encloses(const glm::mat4& viewProj, const Rect& frame,
	              const HE::AABB& localBox, const glm::mat4& model);
}
