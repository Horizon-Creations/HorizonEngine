#include "SplineTool.h"

#include "EditorApplication.h"           // AppContext
#include "EditorHelp.h"                  // the "Spline Tool/" scope
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "EditorWidgets.h"
#include <HorizonScene/Components/EditorLockComponent.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterLake.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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
	// A drag is live: the key is spent on nothing rather than passed on, or the
	// whole spline entity would go while the point is still in the user's hand.
	if (s_tool.dragLive()) return true;
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
		// A drag that was cut off (Play pressed, the mode switched) still owns an
		// undo capture: commit it before the state goes, or it would surface in
		// the history of the next unrelated drag.
		s_tool.endMove(ctx.undoSys);
		reset();
		return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	HorizonWorld& world = *ctx.world;

	// The tool is only fed while the Scene window runs. A gap means the mode was
	// off, or another tab was in front: nothing from before is to be trusted.
	const int frame = ImGui::GetFrameCount();
	if (frame - s_lastFrame > 1)
	{
		s_tool.endMove(ctx.undoSys);
		s_tool.reset();
	}
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

#ifdef HE_IMGUI_ENABLED
namespace
{
	namespace L = HE::water::lake;

	// The Lake section's memory: the numbers the next lake is made with (kept
	// between lakes, like the brush sizes), and the last thing the section did,
	// for the spline it happened to.
	L::Params  s_lake;
	std::string s_lakeNote;
	HE::UUID   s_lakeNoteFor{};
	bool       s_levelLive = false;     // a drag of the level is running: its undo step is taken
	bool       s_clipLive  = false;

	void say(AppContext& ctx, Entity spline, std::string text)
	{
		s_lakeNote    = std::move(text);
		s_lakeNoteFor = ctx.world->entityId(spline);
	}

	// A DragFloat whose whole drag is ONE undo step: the snapshot is taken on the
	// first frame that changes the value, before it is written. `live` remembers
	// that the step is already taken until the drag lets go.
	bool dragOneStep(AppContext& ctx, const char* label, float* v, float speed, float lo, float hi,
	                 const char* fmt, const char* undoLabel, bool& live)
	{
		float tmp = *v;
		const bool changed = ImGui::DragFloat(label, &tmp, speed, lo, hi, fmt);
		if (changed)
		{
			if (!live && ctx.undoSys) ctx.undoSys->snapshotNow(undoLabel);
			live = true;
			*v = tmp;
		}
		if (ImGui::IsItemDeactivated()) live = false;
		return changed;
	}

	// Shore clipping is the landscape's setting, not the lake's: the sheet of every
	// body on it, brushed or drawn, stops where the ground comes up. Shown here
	// because it decides how a lake meets its bank.
	void clippingRows(AppContext& ctx, Entity terrain)
	{
		auto* tc = ctx.world->registry().try_get<TerrainComponent>(terrain);
		if (!tc) return;
		bool clip = tc->water.clipToGround;
		if (EditorWidgets::checkbox("Clip To Ground##lake", &clip))
		{
			if (ctx.undoSys) ctx.undoSys->snapshotNow("Shore Clipping");
			HE::water::setShoreClip(*tc, clip, tc->water.shoreOvershoot);
		}
		EditorWidgets::helpForLabel("Clip To Ground##lake");
		ImGui::BeginDisabled(!tc->water.clipToGround);
		float over = tc->water.shoreOvershoot;
		if (dragOneStep(ctx, "Shore Overshoot##lake", &over, 0.01f, 0.0f, HE::water::kMaxShoreOvershoot,
		                "%.2f m", "Shore Clipping", s_clipLive))
			HE::water::setShoreClip(*tc, tc->water.clipToGround, over);
		EditorWidgets::helpForLabel("Shore Overshoot##lake");
		ImGui::EndDisabled();
	}

	// The lake of a closed spline: made here, reshaped by moving its points,
	// dug again on request.
	void lakeSection(AppContext& ctx, Entity spline)
	{
		HorizonWorld& world = *ctx.world;
		auto& reg = world.registry();
		ImGui::Spacing();
		ImGui::SeparatorText("Lake");

		const L::Link link = L::linkOf(world, spline);
		const bool isLake = link.body != HE::water::kNoBody;
		const Entity terrain = isLake ? link.terrain : L::landscapeUnder(world, spline);
		if (terrain == entt::null)
		{
			ImGui::TextDisabled("Not over a landscape");
			EditorWidgets::hint("Draw the closed shape over a landscape to make a lake of it.");
			return;
		}
		if (const auto* n = reg.try_get<NameComponent>(terrain))
			ImGui::TextDisabled("Landscape: %s", n->name.c_str());

		if (!isLake)
		{
			const std::string why = L::whyNot(world, terrain, spline);
			if (!why.empty()) EditorWidgets::hint("%s", why.c_str());

			EditorWidgets::checkbox("From Ground##lake", &s_lake.levelFromGround);
			EditorWidgets::helpForLabel("From Ground##lake");
			if (s_lake.levelFromGround)
			{
				ImGui::DragFloat("Above Ground##lake", &s_lake.levelOffset, 0.05f, -20.0f, 50.0f, "%.2f m");
				EditorWidgets::helpForLabel("Above Ground##lake");
			}
			else
			{
				ImGui::DragFloat("Level##lake", &s_lake.level, 0.1f, -10000.0f, 10000.0f, "%.2f m");
				EditorWidgets::helpForLabel("Level##lake");
			}
			EditorWidgets::checkbox("Dig Bed##lake", &s_lake.dig);
			EditorWidgets::helpForLabel("Dig Bed##lake");
			ImGui::BeginDisabled(!s_lake.dig);
			ImGui::DragFloat("Depth##lake", &s_lake.depth, 0.05f, 0.0f, 200.0f, "%.2f m");
			EditorWidgets::helpForLabel("Depth##lake");
			ImGui::DragFloat("Bank##lake", &s_lake.bank, 0.1f, 0.0f, 500.0f, "%.1f m");
			EditorWidgets::helpForLabel("Bank##lake");
			ImGui::EndDisabled();
			s_lake.levelOffset = std::clamp(s_lake.levelOffset, -20.0f, 50.0f);
			s_lake.depth = std::max(0.0f, s_lake.depth);
			s_lake.bank  = std::max(0.0f, s_lake.bank);
			clippingRows(ctx, terrain);

			ImGui::BeginDisabled(!why.empty());
			if (EditorWidgets::primaryButton("Create Lake", ImVec2(-1.0f, 0.0f)))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow("Create Lake");
				const L::Created c = L::create(world, terrain, spline, s_lake);
				if (c.ok)
				{
					char line[160];
					std::snprintf(line, sizeof line, "Lake made: level %.2f m, %u cells of water%s", c.level, c.cells,
					              c.ground ? ", bed dug" : "");
					say(ctx, spline, line);
					noteEdited(ctx, terrain);
					noteEdited(ctx, spline);
				}
				else
					say(ctx, spline, c.error);
			}
			ImGui::EndDisabled();
			EditorWidgets::helpForLabel("Create Lake");
		}
		else
		{
			auto* tc = reg.try_get<TerrainComponent>(terrain);
			HE::water::Body* body = tc ? tc->water.findBody(link.body) : nullptr;
			if (!body) return;
			ImGui::TextDisabled("%u cells of water", tc->water.wetCells(body->id));

			float level = body->level;
			if (dragOneStep(ctx, "Level##lake", &level, 0.05f, -10000.0f, 10000.0f, "%.2f m", "Lake Level", s_levelLive))
			{
				HE::water::setLevel(*tc, body->id, level);
				noteEdited(ctx, terrain);
			}
			EditorWidgets::helpForLabel("Level##lake");
			clippingRows(ctx, terrain);

			ImGui::Spacing();
			ImGui::DragFloat("Depth##lake", &s_lake.depth, 0.05f, 0.0f, 200.0f, "%.2f m");
			EditorWidgets::helpForLabel("Depth##lake");
			ImGui::DragFloat("Bank##lake", &s_lake.bank, 0.1f, 0.0f, 500.0f, "%.1f m");
			EditorWidgets::helpForLabel("Bank##lake");
			s_lake.depth = std::max(0.0f, s_lake.depth);
			s_lake.bank  = std::max(0.0f, s_lake.bank);
			if (EditorWidgets::button("Dig Again", ImVec2(-1.0f, 0.0f)))
			{
				// The world as it is now, kept aside: pressed on a bed that is already
				// as deep as asked, nothing moves, and an undo step for that would be a
				// step that appears to do nothing.
				std::vector<uint8_t> before;
				if (ctx.undoSys) SceneSerializer().saveToMemory(world, before);
				const TerrainSculpt::Result r = L::dig(world, spline, s_lake.depth, s_lake.bank);
				if (r.ok && r.changed > 0)
				{
					if (ctx.undoSys && !before.empty()) ctx.undoSys->pushSnapshot(std::move(before), "Dig Lake");
					noteEdited(ctx, terrain);
				}
				char line[96];
				std::snprintf(line, sizeof line, r.changed > 0 ? "Bed dug: %u vertices lowered"
				                                               : "The bed is already that deep", r.changed);
				say(ctx, spline, line);
			}
			EditorWidgets::helpForLabel("Dig Again");
			EditorWidgets::hint("Moving the points reshapes the water only. The ground is dug again "
			                    "only when you press Dig Again.");

			if (EditorWidgets::dangerButton("Remove Lake"))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow("Remove Lake");
				L::remove(world, spline);
				say(ctx, spline, "Lake removed, the ground stays as it is");
				noteEdited(ctx, terrain);
			}
			EditorWidgets::helpForLabel("Remove Lake");
		}

		if (!s_lakeNote.empty() && s_lakeNoteFor == world.entityId(spline))
			EditorWidgets::hint("%s", s_lakeNote.c_str());
	}
}
#endif

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

	// A closed shape can become a lake (and one that is a lake shows its controls).
	if (closed && s.controlPoints.size() >= 3)
		lakeSection(ctx, active);
	else if (HE::water::lake::linkOf(world, active).body != HE::water::kNoBody)
		lakeSection(ctx, active);     // a lake that was opened or cut down: its water waits, its controls stay
	else
		EditorWidgets::hint("Close the line with three or more points to make a lake of it.");

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
