#include "EditorTransformGizmo.h"

#ifdef HE_IMGUI_ENABLED
#include "EditorUndo.h"
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_inverse.hpp>

namespace EditorTransformGizmo
{

void handleOperationKeys(ViewportToolbar::State& tb, bool hovered, bool navigating)
{
	// Not while flying — W/A/S/D drive the camera then — and not while a text
	// field has the keyboard.
	if (!hovered || navigating || ImGui::GetIO().WantTextInput) return;
	if (ImGui::IsKeyPressed(ImGuiKey_W)) tb.op = ImGuizmo::TRANSLATE;
	if (ImGui::IsKeyPressed(ImGuiKey_E)) tb.op = ImGuizmo::ROTATE;
	if (ImGui::IsKeyPressed(ImGuiKey_R)) tb.op = ImGuizmo::SCALE;
}

namespace
{

// The ImGuizmo frame setup both paths share, and the operation actually
// handed to it: for rotation, optionally drop ImGuizmo's outer screen-space
// ring (rotate about the view axis) — it's the confusing white circle.
ImGuizmo::OPERATION beginFrame(const ImVec2& rectMin, const ImVec2& rectMax,
                               const glm::mat4& proj,
                               const ViewportToolbar::State& tb, bool enabled)
{
	ImGuizmo::Enable(enabled);
	// Read off the projection rather than asked: an orthographic editor view
	// (Top/Front/Side) has no perspective w, and ImGuizmo sizes and hit-tests
	// its handles differently for that.
	ImGuizmo::SetOrthographic(proj[3][3] != 0.0f);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(rectMin.x, rectMin.y, rectMax.x - rectMin.x, rectMax.y - rectMin.y);

	ImGuizmo::OPERATION effectiveOp = tb.op;
	if (tb.op == ImGuizmo::ROTATE && !tb.rotateScreenRing)
		effectiveOp = ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z;
	return effectiveOp;
}

// The parent's world matrix, which is what a world-space result is divided by
// to land in the entity's own (parent-relative) TransformComponent.
glm::mat4 parentWorldOf(entt::registry& registry, Entity entity)
{
	if (auto* h = registry.try_get<HierarchyComponent>(entity);
	    h && h->parent != entt::null)
		if (auto* pt = registry.try_get<TransformComponent>(h->parent))
			return pt->worldMatrix;
	return glm::mat4(1.0f);
}

// Undo session: one entry per drag. The pre-state is taken on the frame the
// drag STARTS — ImGuizmo activates a handle without moving it (the motion
// branch runs from the next frame on), so the world is still untouched here.
// It used to hang off IsOver()+MouseClicked BEFORE Manipulate, which answers
// from the context of the previous frame: after a camera move the handle the
// click actually landed on was not the one that test saw, and the drag then
// went onto the stack with no pre-state at all. capturePre() serializes the
// WHOLE world (expensive with terrain), so it must stay on this one edge and
// never run per frame.
void undoEdges(EditorUndo* undo, bool wasUsing, ImGuizmo::OPERATION op)
{
	if (!undo) return;
	// The row the history window shows for this drag: which handle it was.
	const unsigned bits = static_cast<unsigned>(op);
	const char* label = (bits & static_cast<unsigned>(ImGuizmo::TRANSLATE)) ? "Move"
	                  : (bits & static_cast<unsigned>(ImGuizmo::ROTATE))    ? "Rotate"
	                  : (bits & (static_cast<unsigned>(ImGuizmo::SCALE) |
	                             static_cast<unsigned>(ImGuizmo::SCALEU)))  ? "Scale" : "Transform";
	if (ImGuizmo::IsUsing() && !wasUsing) { undo->capturePre(); undo->stashPre(label); }
	if (!ImGuizmo::IsUsing() && wasUsing) undo->commitPending();
}

// ── One entity ──────────────────────────────────────────────────────────────
bool manipulateOne(HorizonWorld& world, Entity entity,
                   const glm::mat4& view, const glm::mat4& proj,
                   const ImVec2& rectMin, const ImVec2& rectMax,
                   const ViewportToolbar::State& tb, bool enabled,
                   EditorUndo* undo, bool* outChanged)
{
	auto& registry = world.registry();
	auto* t = registry.try_get<TransformComponent>(entity);
	if (!t) return false;

	const ImGuizmo::OPERATION effectiveOp = beginFrame(rectMin, rectMax, proj, tb, enabled);

	// While a drag is in progress the gizmo works on the matrix IT produced last
	// frame, NOT on the scene graph's freshly recomposed worldMatrix. The round
	// trip TRS -> worldMatrix -> decompose -> TRS is not an identity:
	// DecomposeMatrixToComponents extracts an Euler triple that is only
	// *equivalent* to the authored one, so each frame handed the gizmo a slightly
	// different matrix and the values visibly jittered mid-drag.
	static bool      s_wasUsing = false;
	static glm::mat4 s_world(1.0f);
	glm::mat4 gizmoWorld = s_wasUsing ? s_world : t->worldMatrix;
	// Snapping quantises the drag to the increment of whichever operation is
	// armed, or moves freely when activeSnap() hands back nullptr.
	ImGuizmo::Manipulate(&view[0][0], &proj[0][0],
	                     effectiveOp, tb.mode, &gizmoWorld[0][0],
	                     nullptr, tb.activeSnap());
	s_world = gizmoWorld;

	// One undo entry per drag (see undoEdges).
	undoEdges(undo, s_wasUsing, effectiveOp);
	s_wasUsing = ImGuizmo::IsUsing();

	if (ImGuizmo::IsUsing())
	{
		// world → local: divide out the parent's world matrix.
		const glm::mat4 local = glm::inverse(parentWorldOf(registry, entity)) * gizmoWorld;

		float pos[3], rot[3], scale[3];
		ImGuizmo::DecomposeMatrixToComponents(&local[0][0], pos, rot, scale);
		// Write back ONLY the channels this operation manipulates. A scale drag
		// used to overwrite rotation with the re-extracted (equivalent but
		// different) Euler triple and vice versa — visible as a value that jumps
		// the moment you touch an unrelated handle.
		const unsigned opBits = static_cast<unsigned>(effectiveOp);
		if (opBits & static_cast<unsigned>(ImGuizmo::TRANSLATE))
			t->position = { pos[0], pos[1], pos[2] };
		if (opBits & static_cast<unsigned>(ImGuizmo::ROTATE))
			t->rotation = { rot[0], rot[1], rot[2] };
		if (opBits & (static_cast<unsigned>(ImGuizmo::SCALE) |
		              static_cast<unsigned>(ImGuizmo::SCALEU)))
			t->scale = { scale[0], scale[1], scale[2] };
		t->dirty = true;
		if (outChanged) *outChanged = true;
	}

	return ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

// ── Several entities ────────────────────────────────────────────────────────
// One gizmo at the group's common pivot; every drag is applied as a WORLD-SPACE
// delta to each member. The delta is measured against the state at the START
// of the drag, member by member, and never accumulated frame to frame: a
// per-frame decompose-and-recompose would drift exactly the way the single
// path's comment describes, only across N objects at once.
//
// What each operation writes is not "the operation's channel" as in the
// single path — turning a group about a shared pivot MOVES its members, and so
// does scaling it. Position is therefore written by every operation; rotation
// by a rotate; scale by a scale. Scale is applied as a factor to each member's
// own scale rather than decomposed from the world result: a non-uniform scale
// along the gizmo's axes turns a rotated member's matrix into a sheared one,
// which a TRS component cannot hold and a decompose would silently mangle.
// Uniform scaling — the common case — is exact either way.
struct GroupMember
{
	Entity    entity;
	glm::mat4 startWorld;   // worldMatrix when the drag began
	glm::vec3 startScale;   // the member's own scale when the drag began
};

bool manipulateGroup(HorizonWorld& world, const std::vector<Entity>& members,
                     const glm::mat4& view, const glm::mat4& proj,
                     const ImVec2& rectMin, const ImVec2& rectMax,
                     const ViewportToolbar::State& tb, bool enabled,
                     EditorUndo* undo, bool* outChanged)
{
	auto& registry = world.registry();
	const ImGuizmo::OPERATION effectiveOp = beginFrame(rectMin, rectMax, proj, tb, enabled);

	static bool                     s_wasUsing = false;
	static glm::mat4                s_gizmoStart(1.0f); // handed to ImGuizmo on the drag's first frame
	static glm::mat4                s_gizmo(1.0f);      // what ImGuizmo made of it, latched
	static std::vector<GroupMember> s_members;

	// IsUsing() here still answers for the previous frame. The second test is
	// for a drag the single path finished (a peer deleted all but one member
	// mid-drag): our own flag is stale then, and the latched matrix with it.
	if (!s_wasUsing || !ImGuizmo::IsUsing())
	{
		// Not dragging: rebuild the gizmo from the live selection every frame.
		// Pivot = centroid of the members' world positions; orientation = the
		// last member's (the primary, the one the user clicked last) in Local
		// space, the world axes otherwise. Scale is always 1 — it is the
		// group's, not any member's, and a scale drag reads the factor off it.
		glm::vec3 centroid(0.0f);
		float     count = 0.0f;
		const TransformComponent* primary = nullptr;
		for (const Entity e : members)
			if (const auto* t = registry.try_get<TransformComponent>(e))
			{
				centroid += glm::vec3(t->worldMatrix[3]);
				count    += 1.0f;
				primary   = t;
			}
		if (count < 2.0f || !primary) return false;
		centroid /= count;

		glm::mat4 basis(1.0f);
		if (tb.mode == ImGuizmo::LOCAL)
		{
			// The primary's world rotation with its scale divided out; a
			// degenerate axis (scale 0) falls back to the world axes.
			for (int c = 0; c < 3; ++c)
			{
				const glm::vec3 axis(primary->worldMatrix[c]);
				const float     len = glm::length(axis);
				if (len < 1e-6f) { basis = glm::mat4(1.0f); break; }
				basis[c] = glm::vec4(axis / len, 0.0f);
			}
		}
		basis[3]     = glm::vec4(centroid, 1.0f);
		s_gizmoStart = basis;
		s_gizmo      = basis;
	}

	ImGuizmo::Manipulate(&view[0][0], &proj[0][0],
	                     effectiveOp, tb.mode, &s_gizmo[0][0],
	                     nullptr, tb.activeSnap());

	// The drag's first frame: latch where everything is. ImGuizmo has not moved
	// the matrix yet on this frame (see undoEdges), so the world is the start.
	if (ImGuizmo::IsUsing() && !s_wasUsing)
	{
		s_members.clear();
		for (const Entity e : members)
			if (const auto* t = registry.try_get<TransformComponent>(e))
				s_members.push_back({ e, t->worldMatrix, t->scale });
	}
	undoEdges(undo, s_wasUsing, effectiveOp);
	s_wasUsing = ImGuizmo::IsUsing();

	if (ImGuizmo::IsUsing())
	{
		const unsigned opBits   = static_cast<unsigned>(effectiveOp);
		const bool     rotating = (opBits & static_cast<unsigned>(ImGuizmo::ROTATE)) != 0;
		const bool     scaling  = (opBits & (static_cast<unsigned>(ImGuizmo::SCALE) |
		                                     static_cast<unsigned>(ImGuizmo::SCALEU))) != 0;
		// World-space delta from the drag's start to now. For a scale this is
		// T·R·S·R⁻¹·T⁻¹ — a scale about the pivot along the gizmo's axes —
		// and inv(start)·now is the bare S, which is where the factor comes
		// from (ImGuizmo scales in the matrix's own axes: SCALE forces LOCAL).
		const glm::mat4 delta = s_gizmo * glm::inverse(s_gizmoStart);
		glm::vec3 factor(1.0f);
		if (scaling)
		{
			const glm::mat4 local = glm::inverse(s_gizmoStart) * s_gizmo;
			factor = { glm::length(glm::vec3(local[0])),
			           glm::length(glm::vec3(local[1])),
			           glm::length(glm::vec3(local[2])) };
		}

		for (const GroupMember& m : s_members)
		{
			if (!registry.valid(m.entity)) continue;
			auto* t = registry.try_get<TransformComponent>(m.entity);
			if (!t) continue;
			const glm::mat4 worldNow = delta * m.startWorld;
			const glm::mat4 local    = glm::inverse(parentWorldOf(registry, m.entity)) * worldNow;

			float pos[3], rot[3], scale[3];
			ImGuizmo::DecomposeMatrixToComponents(&local[0][0], pos, rot, scale);
			t->position = { pos[0], pos[1], pos[2] };
			if (rotating) t->rotation = { rot[0], rot[1], rot[2] };
			if (scaling)  t->scale    = m.startScale * factor;
			t->dirty = true;
		}
		if (outChanged) *outChanged = true;
	}
	else
		s_members.clear();

	return ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

} // namespace

bool manipulate(HorizonWorld& world, Entity entity,
                const glm::mat4& view, const glm::mat4& proj,
                const ImVec2& rectMin, const ImVec2& rectMax,
                const ViewportToolbar::State& tb, bool enabled,
                EditorUndo* undo, bool* outChanged)
{
	if (outChanged) *outChanged = false;
	if (entity == entt::null || !world.registry().valid(entity)) return false;
	return manipulateOne(world, entity, view, proj, rectMin, rectMax, tb, enabled,
	                     undo, outChanged);
}

bool manipulate(HorizonWorld& world, const std::vector<Entity>& entities,
                const glm::mat4& view, const glm::mat4& proj,
                const ImVec2& rectMin, const ImVec2& rectMax,
                const ViewportToolbar::State& tb, bool enabled,
                EditorUndo* undo, bool* outChanged)
{
	if (outChanged) *outChanged = false;
	auto& registry = world.registry();
	// Only what can be moved counts — a selected entity without a Transform
	// (or one a peer just deleted) must not turn one movable entity into a
	// "group" of one at a pivot that is its own position anyway.
	std::vector<Entity> movable;
	movable.reserve(entities.size());
	for (const Entity e : entities)
		if (e != entt::null && registry.valid(e) && registry.all_of<TransformComponent>(e))
			movable.push_back(e);
	if (movable.empty()) return false;
	if (movable.size() == 1)
		return manipulateOne(world, movable.front(), view, proj, rectMin, rectMax, tb,
		                     enabled, undo, outChanged);
	return manipulateGroup(world, movable, view, proj, rectMin, rectMax, tb, enabled,
	                       undo, outChanged);
}

} // namespace EditorTransformGizmo
#endif // HE_IMGUI_ENABLED
