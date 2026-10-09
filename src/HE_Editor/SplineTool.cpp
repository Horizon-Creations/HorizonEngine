#include "SplineTool.h"

#include "EditorApplication.h"           // AppContext
#include "EditorHelp.h"                  // the "Spline Tool/" scope
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorWidgets.h"
#include <HorizonScene/Components/EditorLockComponent.h>
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/TransformHierarchy.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SplineTool
{

// The tool's memory between frames. One Scene viewport has the mouse at a time,
// so one set of statics is enough (the same arrangement as TerrainTools).
static SplineEdit::Tool s_tool;
static int              s_lastFrame = -10;   // the last frame the viewport ran the tool

namespace
{
	bool modeOn(const AppContext& ctx)
	{
		return ctx.editorConfig.mode == EditorMode::Spline && ctx.world && !ctx.isPlaying;
	}

	// A spline locked in the Outliner can be selected and read, not edited by
	// pointing: same rule as the entity gizmo.
	bool locked(AppContext& ctx, Entity e)
	{
		return e != entt::null && ctx.world->registry().all_of<EditorLockComponent>(e);
	}

	void noteEdited(AppContext& ctx, Entity e)
	{
		if (e != entt::null && ctx.noteEntityEdited) ctx.noteEntityEdited(e);
	}

	float worldLength(HorizonWorld& world, Entity e)
	{
		const SplineComponent& s = world.registry().get<SplineComponent>(e);
		const HE::spline::Curve curve(s);
		if (curve.spanCount() == 0) return 0.0f;
		const glm::mat4 model = HE::worldMatrixOf(world, e);
		const std::vector<glm::vec3> pts = curve.sample(SplineEdit::kSamplesPerSpan);
		float len = 0.0f;
		for (size_t i = 1; i < pts.size(); ++i)
			len += glm::length(glm::vec3(model * glm::vec4(pts[i], 1.0f)) -
			                   glm::vec3(model * glm::vec4(pts[i - 1], 1.0f)));
		return len;
	}
}

void reset()
{
	s_tool.reset();
	s_lastFrame = -10;
}

SplineEdit::GuideState guides(HorizonWorld& world, const EditorSelection& selection)
{
	return s_tool.guides(world, selection);
}

bool deleteKey(AppContext& ctx)
{
	if (!modeOn(ctx) || !s_tool.ownsDeleteKey()) return false;
	const Entity active = s_tool.activeSpline(*ctx.world, ctx.selection);
	if (active == entt::null || locked(ctx, active)) return false;
	if (!s_tool.deleteSelectedPoint(*ctx.world, ctx.selection, ctx.undoSys)) return false;
	noteEdited(ctx, active);
	return true;
}

bool escapeKey(AppContext& ctx)
{
	return modeOn(ctx) && s_tool.escape();
}

#ifdef HE_IMGUI_ENABLED

bool updateInViewport(AppContext& ctx,
                      const glm::mat4& view, const glm::mat4& proj,
                      const ImVec2& rectMin, const ImVec2& rectMax,
                      bool navigating, bool viewportHovered, bool itemClicked,
                      const ViewportToolbar::State& toolbar,
                      const SplineEdit::GroundProbe& ground,
                      const EditorTransformGizmo::SnapProbe& snap)
{
	if (!modeOn(ctx))
	{
		reset();
		return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	HorizonWorld& world = *ctx.world;

	// The tool is only fed while the Scene window runs. A gap means the mode was
	// off, or another tab was in front: nothing from before is to be trusted.
	const int frame = ImGui::GetFrameCount();
	if (frame - s_lastFrame > 1) s_tool.reset();
	s_lastFrame = frame;
	s_tool.sync(world, ctx.selection, ctx.undoSys ? ctx.undoSys->revision() : 0);

	const Entity active = s_tool.activeSpline(world, ctx.selection);
	const bool   editable = active != entt::null && !locked(ctx, active);

	// ── The Move gizmo on the selected point ────────────────────────────────
	bool gizmoActive = false;
	if (editable && s_tool.selectedPoint() >= 0)
	{
		const SplineComponent& s = world.registry().get<SplineComponent>(active);
		const glm::mat4 model = HE::worldMatrixOf(world, active);
		glm::vec3 where(model * glm::vec4(s.controlPoints[static_cast<size_t>(s_tool.selectedPoint())], 1.0f));
		EditorTransformGizmo::PointDrag drag;
		gizmoActive = EditorTransformGizmo::manipulatePoint(
			where, view, proj, rectMin, rectMax, toolbar,
			/*enabled=*/!navigating && !io.KeyAlt, drag, snap);
		if (drag.started) s_tool.beginMove(ctx.undoSys);
		if (drag.moved && s_tool.moveSelectedPoint(world, ctx.selection, ctx.undoSys, where))
			noteEdited(ctx, active);
		if (drag.ended) s_tool.endMove(ctx.undoSys);
	}

	const SplineEdit::View sv{ proj * view,
	                           { rectMin.x, rectMin.y },
	                           { rectMax.x - rectMin.x, rectMax.y - rectMin.y } };

	// ── Hover: which handle, or where on the line, a click would act on ────
	const ImVec2 mouse = ImGui::GetMousePos();
	if (viewportHovered && !navigating && !gizmoActive && !io.KeyAlt && !ImGui::IsAnyMouseDown() && editable)
		s_tool.hover(world, ctx.selection, sv, { mouse.x, mouse.y });
	else
		s_tool.clearHover();

	// ── A click ─────────────────────────────────────────────────────────────
	// Armed on the press, decided on the release: a press that turns into a drag
	// is the camera's or the gizmo's, not a point. Alt+LMB is the orbit.
	static bool   s_pressArmed = false;
	static bool   s_dragged    = false;
	static ImVec2 s_pressPos{};
	if (itemClicked && !gizmoActive && !navigating && !io.KeyAlt)
	{
		s_pressArmed = true;
		s_dragged    = false;
		s_pressPos   = mouse;
	}
	if (s_pressArmed && (gizmoActive || navigating || io.KeyAlt))
		s_pressArmed = false;
	if (s_pressArmed)
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) s_dragged = true;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			s_pressArmed = false;
			// A locked spline is read, not edited: its handles stay inert and a
			// click does not start a second line on top of it.
			if (!s_dragged && (active == entt::null || editable))
			{
				const auto result = s_tool.click(world, ctx.selection, ctx.undoSys, sv,
				                                 { s_pressPos.x, s_pressPos.y }, ground);
				using C = SplineEdit::Tool::Click;
				if (result == C::InsertedPoint || result == C::AddedPoint || result == C::CreatedSpline)
					noteEdited(ctx, s_tool.activeSpline(world, ctx.selection));
			}
		}
	}

	// ── The hint, over the picture ──────────────────────────────────────────
	{
		const char* line = "Spline: click the ground to start a line";
		if (active != entt::null)
			line = !editable
			     ? "Spline: locked in the Outliner"
			     : "Spline: click to add a point  |  click the line to insert  |  Del removes the point  |  Esc lets go";
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 size = ImGui::CalcTextSize(line);
		const ImVec2 at(rectMin.x + 12.0f, rectMax.y - size.y - 14.0f);
		dl->AddRectFilled({ at.x - 6.0f, at.y - 3.0f }, { at.x + size.x + 6.0f, at.y + size.y + 3.0f },
		                  IM_COL32(12, 14, 18, 170), 3.0f);
		dl->AddText(at, IM_COL32(230, 236, 232, 235), line);
	}

	return gizmoActive;
}

#endif // HE_IMGUI_ENABLED

void renderPanel(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.world) return;
	HorizonWorld& world = *ctx.world;
	// Buttons and the Closed switch read as "Spline" in the undo history; the
	// clicks in the viewport label themselves ("Add Spline Point", …).
	EditorUndo::Context undoScope(ctx.undoSys, "Spline");
	EditorWidgets::WrapText wrapPanel;
	HE::Ed::Help::Scope helpScope("Spline Tool");

	const Entity active = s_tool.activeSpline(world, ctx.selection);
	ImGui::SeparatorText("Spline");
	if (active == entt::null)
	{
		ImGui::TextDisabled("No spline selected");
		EditorWidgets::hint("Click in the viewport to start a line on the ground, or select a "
		                    "spline in the Outliner to edit it.");
		return;
	}

	SplineComponent& s = world.registry().get<SplineComponent>(active);
	const bool editable = !locked(ctx, active);
	if (const auto* name = world.registry().try_get<NameComponent>(active))
		ImGui::TextUnformatted(name->name.c_str());
	char info[96];
	std::snprintf(info, sizeof info, "%zu point%s, %.1f m", s.controlPoints.size(),
	              s.controlPoints.size() == 1 ? "" : "s", worldLength(world, active));
	ImGui::TextDisabled("%s", info);
	if (!editable)
	{
		EditorWidgets::hint("Locked in the Outliner: unlock it to edit.");
		return;
	}

	bool closed = s.closed;
	if (EditorWidgets::checkbox("Closed", &closed))
	{
		s_tool.toggleClosed(world, ctx.selection, ctx.undoSys);
		noteEdited(ctx, active);
	}
	EditorWidgets::helpForLabel("Closed");
	if (closed && s.controlPoints.size() < 3)
		EditorWidgets::hint("A closed line needs at least three points; with fewer it stays open.");

	ImGui::Spacing();
	const bool hasPoint = s_tool.selectedPoint() >= 0;
	if (hasPoint)
	{
		ImGui::TextDisabled("Point %d selected", s_tool.selectedPoint());
		if (EditorWidgets::dangerButton("Delete Point"))
		{
			s_tool.deleteSelectedPoint(world, ctx.selection, ctx.undoSys);
			noteEdited(ctx, active);
		}
		EditorWidgets::helpForLabel("Delete Point");
	}
	else
		ImGui::TextDisabled("Click a point to select it");

	ImGui::Spacing();
	if (EditorWidgets::button("New Spline"))
		ctx.selection.clear();   // nothing selected: the next click starts a fresh line
	EditorWidgets::helpForLabel("New Spline");

	ImGui::Spacing();
	ImGui::SeparatorText("Controls");
	ImGui::TextDisabled("Click: add a point");
	ImGui::TextDisabled("Click the line: insert a point");
	ImGui::TextDisabled("Drag the gizmo: move the point");
	ImGui::TextDisabled("Del: delete the point");
	ImGui::TextDisabled("Esc: let go of the point");
#else
	(void)ctx;
#endif
}

}
