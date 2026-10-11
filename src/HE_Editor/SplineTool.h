#pragma once
#include "SplineEdit.h"
#include <glm/glm.hpp>

struct AppContext;
struct ImVec2;
class  EditorSelection;
class  HorizonWorld;

#ifdef HE_IMGUI_ENABLED
#include "EditorTransformGizmo.h"   // SnapProbe
#include "ViewportToolbar.h"        // State
#endif

// ── Spline mode in the Scene viewport ────────────────────────────────────────
// The editor mode (EditorMode::Spline) in which a click on the picture draws a
// line: the first click starts a spline where the mouse is on the ground, each
// further click adds a point, a click on the line inserts one between its
// neighbours, a click on a handle selects the point and puts the Move gizmo on
// it, Delete removes it, and every one of those is a single step of the undo
// history. What those clicks MEAN is SplineEdit.h; this file is the part that
// needs ImGui — reading the mouse and the keys, the gizmo, the hint over the
// picture — and the Quick Settings panel that replaces the usual content while
// the mode is on.
//
// Everything the tool remembers between frames is file-static in the .cpp (one
// Scene viewport has the mouse at a time), and none of it is an Entity: an undo
// rebuilds the world, and the tool finds its spline again by UUID.
namespace SplineTool
{
#ifdef HE_IMGUI_ENABLED
	// Drawn and handled inside the Scene window, in the spot where View mode
	// puts the entity gizmo. `itemClicked` is IsItemClicked(Left) for the
	// picture, asked by the caller while the picture is still the last item.
	// `ground` is the scene's surface probe (terrain included), asked only when
	// a click needs a place on the ground. Returns whether the tool is using
	// the mouse — the gizmo is hovered or dragged — so the caller leaves it
	// alone.
	bool updateInViewport(AppContext& ctx,
	                      const glm::mat4& view, const glm::mat4& proj,
	                      const ImVec2& rectMin, const ImVec2& rectMax,
	                      bool navigating, bool viewportHovered, bool itemClicked,
	                      const ViewportToolbar::State& toolbar,
	                      const SplineEdit::GroundProbe& ground,
	                      const EditorTransformGizmo::SnapProbe& snap);
#endif

	// The Delete and Esc keys, offered before the editor's own handling of them
	// (EditorUI.cpp runs them ahead of the Scene window). True when the tool took
	// the key: Delete with a point selected removes it instead of the entity,
	// Esc lets go of the point instead of emptying the selection. False in any
	// other mode, and whenever there is no point selected.
	bool deleteKey(AppContext& ctx);
	bool escapeKey(AppContext& ctx);

	// What the debug-line overlay needs to show the tool's state; the default
	// (nothing selected, nothing hovered) outside the mode.
	SplineEdit::GuideState guides(HorizonWorld& world, const EditorSelection& selection);

	// Body of the "Spline###Quick Settings" window while Spline mode is on. The
	// window's Begin/End stays with the editor shell, like the Landscape panel's.
	void renderPanel(AppContext& ctx);

	// Forget the tool's state (the mode was left, the project closed).
	void reset();
}
