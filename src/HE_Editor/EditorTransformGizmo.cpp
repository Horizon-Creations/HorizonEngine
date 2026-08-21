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

bool manipulate(HorizonWorld& world, Entity entity,
                const glm::mat4& view, const glm::mat4& proj,
                const ImVec2& rectMin, const ImVec2& rectMax,
                const ViewportToolbar::State& tb, bool enabled,
                EditorUndo* undo, bool* outChanged)
{
	if (outChanged) *outChanged = false;
	auto& registry = world.registry();
	if (entity == entt::null || !registry.valid(entity)) return false;
	auto* t = registry.try_get<TransformComponent>(entity);
	if (!t) return false;

	ImGuizmo::Enable(enabled);
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(rectMin.x, rectMin.y, rectMax.x - rectMin.x, rectMax.y - rectMin.y);

	// For rotation, optionally drop ImGuizmo's outer screen-space ring (rotate
	// about the view axis) — it's the confusing white circle.
	ImGuizmo::OPERATION effectiveOp = tb.op;
	if (tb.op == ImGuizmo::ROTATE && !tb.rotateScreenRing)
		effectiveOp = ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z;

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

	// Undo session: one entry per drag. The pre-state is taken on the frame the
	// drag STARTS — ImGuizmo activates a handle without moving it (the motion
	// branch runs from the next frame on), so the world is still untouched here.
	// It used to hang off IsOver()+MouseClicked BEFORE Manipulate, which answers
	// from the context of the previous frame: after a camera move the handle the
	// click actually landed on was not the one that test saw, and the drag then
	// went onto the stack with no pre-state at all. capturePre() serializes the
	// WHOLE world (expensive with terrain), so it must stay on this one edge and
	// never run per frame.
	if (undo)
	{
		if (ImGuizmo::IsUsing() && !s_wasUsing) { undo->capturePre(); undo->stashPre(); }
		if (!ImGuizmo::IsUsing() && s_wasUsing) undo->commitPending();
	}
	s_wasUsing = ImGuizmo::IsUsing();

	if (ImGuizmo::IsUsing())
	{
		// world → local: divide out the parent's world matrix.
		glm::mat4 parentWorld(1.0f);
		if (auto* h = registry.try_get<HierarchyComponent>(entity);
		    h && h->parent != entt::null)
			if (auto* pt = registry.try_get<TransformComponent>(h->parent))
				parentWorld = pt->worldMatrix;
		const glm::mat4 local = glm::inverse(parentWorld) * gizmoWorld;

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

} // namespace EditorTransformGizmo
#endif // HE_IMGUI_ENABLED
