#include "doctest.h"

#include "../src/HE_Editor/EditorSelection.h"
#include "../src/HE_Editor/EditorUndo.h"
#include "../src/HE_Editor/SplineEdit.h"
#include "../src/HE_Editor/ViewportOverlays.h"

#include <HorizonScene/Components/SplineComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/TransformHierarchy.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

// ─── The spline tool, without a window ───────────────────────────────────────
// A click on the picture adds a point, a click on the curve inserts one, Delete
// removes one, a drag moves one — and every one of those has to be exactly one
// step in the undo history that Ctrl+Z walks back through. The viewport only
// reads the mouse; these tests are the part that decides what the mouse means,
// against a real camera matrix and a real EditorUndo.

namespace
{
	using SplineEdit::View;

	// A camera 12 m up and 12 m back, looking at the origin, in a 1280×720
	// picture: the ground plane y = 0 fills most of it.
	View makeView(const glm::vec3& eye = { 0.0f, 12.0f, 12.0f })
	{
		View v;
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
		v.viewProj = proj * view;
		v.rectMin  = { 0.0f, 0.0f };
		v.rectSize = { 1280.0f, 720.0f };
		return v;
	}

	glm::vec2 pxOf(const View& v, const glm::vec3& world)
	{
		glm::vec2 px(-1.0f);
		REQUIRE(SplineEdit::project(v, world, px));
		return px;
	}

	// The world position under a pixel on y = 0 — what a click there means when
	// nothing is hit and the tool falls back to the ground plane.
	glm::vec3 groundUnder(const View& v, const glm::vec2& px)
	{
		const glm::mat4 inv = glm::inverse(v.viewProj);
		const float nx = (px.x - v.rectMin.x) / v.rectSize.x * 2.0f - 1.0f;
		const float ny = 1.0f - (px.y - v.rectMin.y) / v.rectSize.y * 2.0f;
		glm::vec4 a = inv * glm::vec4(nx, ny, -1.0f, 1.0f); a /= a.w;
		glm::vec4 b = inv * glm::vec4(nx, ny,  1.0f, 1.0f); b /= b.w;
		glm::vec3 out(0.0f);
		REQUIRE(SplineEdit::rayHitsPlaneY(glm::vec3(a), glm::vec3(b) - glm::vec3(a), 0.0f, out));
		return out;
	}

	SplineComponent& splineOf(HorizonWorld& w, Entity e) { return w.registry().get<SplineComponent>(e); }

	// What the Ctrl+Z / Ctrl+Y lambdas of EditorApplication do after a jump.
	void undoLikeTheEditor(EditorUndo& undo, EditorSelection& sel) { if (undo.undo()) sel.clear(); }
	void redoLikeTheEditor(EditorUndo& undo, EditorSelection& sel) { if (undo.redo()) sel.clear(); }

	// A world with an editor around it.
	struct Rig
	{
		HorizonWorld    world;
		EditorUndo      undo;
		EditorSelection sel;
		SplineEdit::Tool tool;
		View            view = makeView();
		Rig() { undo.setWorld(&world); }

		Entity spline() { return tool.activeSpline(world, sel); }
		SplineEdit::Tool::Click clickGround(const glm::vec3& at)
		{
			return tool.click(world, sel, &undo, view, pxOf(view, at), {});
		}
		void frame() { tool.sync(world, sel, undo.revision()); }
	};

	bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) { return glm::length(a - b) < eps; }
}

// ── Picking ──────────────────────────────────────────────────────────────────

TEST_CASE("SplineEdit::pick prefers a handle, then the curve, else nothing")
{
	const View v = makeView();
	SplineComponent s;
	s.controlPoints = { { -4, 0, 0 }, { 0, 0, 3 }, { 4, 0, 0 } };
	const glm::mat4 id(1.0f);

	// On the middle handle, a few pixels off: the handle, not the curve under it.
	const SplineEdit::Pick onHandle = SplineEdit::pick(s, id, v, pxOf(v, s.controlPoints[1]) + glm::vec2(3.0f, 2.0f));
	CHECK(onHandle.kind == SplineEdit::Pick::Kind::Point);
	CHECK(onHandle.index == 1);

	// Halfway along the first span, well clear of every handle: the curve, and
	// the new point goes between control points 0 and 1.
	const HE::spline::Curve curve(s);
	const glm::vec3 mid = curve.position(0.5f);
	const SplineEdit::Pick onCurve = SplineEdit::pick(s, id, v, pxOf(v, mid));
	REQUIRE(onCurve.kind == SplineEdit::Pick::Kind::Segment);
	CHECK(onCurve.insertIndex == 1);
	CHECK(near(onCurve.local, mid, 0.15f));   // on the curve, not on the chord

	// …and on the second span the index follows.
	const SplineEdit::Pick onSecond = SplineEdit::pick(s, id, v, pxOf(v, curve.position(1.5f)));
	REQUIRE(onSecond.kind == SplineEdit::Pick::Kind::Segment);
	CHECK(onSecond.insertIndex == 2);

	// Open sky well away from everything.
	CHECK(SplineEdit::pick(s, id, v, glm::vec2(20.0f, 20.0f)).kind == SplineEdit::Pick::Kind::None);
}

TEST_CASE("SplineEdit::pick: the closing span of a closed spline inserts at the end")
{
	const View v = makeView();
	SplineComponent s;
	s.closed = true;
	s.controlPoints = { { -3, 0, -3 }, { 3, 0, -3 }, { 3, 0, 3 }, { -3, 0, 3 } };
	const HE::spline::Curve curve(s);
	REQUIRE(curve.spanCount() == 4);

	// t = 3.5 is halfway along the span from the last point back to the first.
	const SplineEdit::Pick p = SplineEdit::pick(s, glm::mat4(1.0f), v, pxOf(v, curve.position(3.5f)));
	REQUIRE(p.kind == SplineEdit::Pick::Kind::Segment);
	CHECK(p.insertIndex == 4);   // after the last point: an append
}

TEST_CASE("SplineEdit::pick reads the world: a moved, scaled spline is picked where it stands")
{
	const View v = makeView();
	SplineComponent s;
	s.controlPoints = { { -1, 0, 0 }, { 1, 0, 0 } };
	// local (1,0,0) → world (5 + 2, 0, 0)
	const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))
	                      * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
	const SplineEdit::Pick p = SplineEdit::pick(s, model, v, pxOf(v, glm::vec3(7.0f, 0.0f, 0.0f)));
	CHECK(p.kind == SplineEdit::Pick::Kind::Point);
	CHECK(p.index == 1);
	// The local position of the unscaled point would be 6 m away on screen.
	CHECK(SplineEdit::pick(s, glm::mat4(1.0f), v, pxOf(v, glm::vec3(7.0f, 0.0f, 0.0f))).kind
	      == SplineEdit::Pick::Kind::None);
}

TEST_CASE("SplineEdit::rayHitsPlaneY refuses a ray that does not meet the plane")
{
	glm::vec3 out(0.0f);
	CHECK(SplineEdit::rayHitsPlaneY({ 0, 10, 0 }, { 0, -1, 0 }, 2.0f, out));
	CHECK(near(out, { 0, 2, 0 }));
	CHECK_FALSE(SplineEdit::rayHitsPlaneY({ 0, 10, 0 }, { 1, 0, 0 }, 2.0f, out));    // parallel
	CHECK_FALSE(SplineEdit::rayHitsPlaneY({ 0, 10, 0 }, { 0, 1, 0 }, 2.0f, out));    // points away
	CHECK_FALSE(SplineEdit::rayHitsPlaneY({ 0, 1, 0 }, { 1, -0.0001f, 0 }, 0.0f, out)); // meets it at the horizon
}

// ── The edits, one history entry each ────────────────────────────────────────

TEST_CASE("SplineEdit: every edit is exactly one undo entry and undo restores it")
{
	Rig r;
	const Entity e = SplineEdit::create(r.world, &r.undo, { 2, 0, 3 });
	REQUIRE((e != entt::null));
	CHECK(r.undo.undoDepth() == 1);
	CHECK(r.undo.undoLabel() == "Add Spline");
	REQUIRE(splineOf(r.world, e).controlPoints.size() == 1);
	CHECK(near(glm::vec3(HE::worldMatrixOf(r.world, e)[3]), { 2, 0, 3 }));

	CHECK(SplineEdit::appendPoint(r.world, e, &r.undo, { 6, 0, 3 }) == 1);
	CHECK(SplineEdit::appendPoint(r.world, e, &r.undo, { 6, 0, 8 }) == 2);
	CHECK(r.undo.undoDepth() == 3);
	CHECK(SplineEdit::insertPoint(r.world, e, &r.undo, 1, { 2, 0, 1 }) == 1);
	CHECK(r.undo.undoDepth() == 4);
	CHECK(r.undo.undoLabel() == "Insert Spline Point");
	CHECK(splineOf(r.world, e).controlPoints.size() == 4);
	CHECK(SplineEdit::removePoint(r.world, e, &r.undo, 0));
	CHECK(r.undo.undoDepth() == 5);
	CHECK(SplineEdit::setClosed(r.world, e, &r.undo, true));
	CHECK(r.undo.undoDepth() == 6);
	CHECK(r.undo.undoLabel() == "Close Spline");

	// Back through it, step by step: the final state, then each earlier one.
	auto spline = [&]() -> SplineComponent& {
		return r.world.registry().get<SplineComponent>(r.world.registry().view<SplineComponent>().front());
	};
	REQUIRE(r.undo.undo());   // un-close
	CHECK_FALSE(spline().closed);
	CHECK(spline().controlPoints.size() == 3);
	REQUIRE(r.undo.undo());   // un-remove: the first point is back
	CHECK(spline().controlPoints.size() == 4);
	CHECK(near(spline().controlPoints[0], { 0, 0, 0 }));
	REQUIRE(r.undo.undo());   // un-insert
	CHECK(spline().controlPoints.size() == 3);
	REQUIRE(r.undo.undo());
	REQUIRE(r.undo.undo());
	CHECK(spline().controlPoints.size() == 1);
	REQUIRE(r.undo.undo());   // the entity itself
	CHECK(r.world.registry().view<SplineComponent>().empty());
	CHECK_FALSE(r.undo.canUndo());

	// …and forward again to the state after the last edit.
	for (int i = 0; i < 6; ++i) REQUIRE(r.undo.redo());
	REQUIRE(r.world.registry().view<SplineComponent>().size() == 1);
	CHECK(spline().closed);
	CHECK(spline().controlPoints.size() == 3);
	CHECK(near(spline().controlPoints[0], { 2, 0, 1 }));
}

TEST_CASE("SplineEdit: an edit that does not happen leaves no history entry")
{
	Rig r;
	const Entity e = SplineEdit::create(r.world, &r.undo, { 0, 0, 0 });
	const size_t depth = r.undo.undoDepth();

	CHECK(SplineEdit::insertPoint(r.world, e, &r.undo, 5, { 1, 0, 0 }) == -1);   // past the end
	CHECK(SplineEdit::insertPoint(r.world, e, &r.undo, -1, { 1, 0, 0 }) == -1);
	CHECK_FALSE(SplineEdit::removePoint(r.world, e, &r.undo, 3));
	CHECK_FALSE(SplineEdit::removePoint(r.world, e, &r.undo, -1));
	CHECK_FALSE(SplineEdit::setClosed(r.world, e, &r.undo, false));              // already open
	CHECK((SplineEdit::appendPoint(r.world, entt::null, &r.undo, { 1, 0, 0 }) == -1));
	CHECK(SplineEdit::appendPoint(r.world, r.world.rootEntity(), &r.undo, { 1, 0, 0 }) == -1);   // no spline
	CHECK(r.undo.undoDepth() == depth);

	// A flattened spline cannot be edited by pointing at the world: no inverse.
	r.world.registry().get<TransformComponent>(e).scale = glm::vec3(0.0f);
	CHECK(SplineEdit::appendPoint(r.world, e, &r.undo, { 1, 0, 0 }) == -1);
	CHECK(r.undo.undoDepth() == depth);
}

TEST_CASE("SplineEdit::appendPoint stores the point in the spline's own space")
{
	Rig r;
	const Entity e = SplineEdit::create(r.world, &r.undo, { 10, 0, 0 });
	auto& t = r.world.registry().get<TransformComponent>(e);
	t.scale    = glm::vec3(2.0f);
	t.rotation = glm::vec3(0.0f, 90.0f, 0.0f);

	REQUIRE(SplineEdit::appendPoint(r.world, e, &r.undo, { 10, 0, -4 }) == 1);
	const glm::vec3 local = splineOf(r.world, e).controlPoints[1];
	// Whatever it is locally, the world matrix must take it back to where it was clicked.
	CHECK(near(glm::vec3(HE::worldMatrixOf(r.world, e) * glm::vec4(local, 1.0f)), { 10, 0, -4 }, 1e-3f));
	// And it is not the world position written raw.
	CHECK_FALSE(near(local, { 10, 0, -4 }, 0.5f));

	REQUIRE(SplineEdit::movePoint(r.world, e, 1, { 14, 0, 0 }));
	CHECK(near(glm::vec3(HE::worldMatrixOf(r.world, e) * glm::vec4(splineOf(r.world, e).controlPoints[1], 1.0f)),
	           { 14, 0, 0 }, 1e-3f));
	CHECK_FALSE(SplineEdit::movePoint(r.world, e, 9, { 0, 0, 0 }));
}

// ── The tool: clicks ─────────────────────────────────────────────────────────

TEST_CASE("Tool: clicks build a spline, one undo step each, and Ctrl+Z takes them back")
{
	Rig r;
	using C = SplineEdit::Tool::Click;

	// Nothing selected: the first click starts a spline there and selects it.
	CHECK(r.clickGround({ -4, 0, -2 }) == C::CreatedSpline);
	REQUIRE((r.spline() != entt::null));
	CHECK(r.undo.undoDepth() == 1);
	CHECK(r.tool.selectedPoint() == 0);

	CHECK(r.clickGround({ 0, 0, -2 }) == C::AddedPoint);
	CHECK(r.clickGround({ 4, 0, 2 }) == C::AddedPoint);
	CHECK(r.undo.undoDepth() == 3);
	REQUIRE(splineOf(r.world, r.spline()).controlPoints.size() == 3);
	CHECK(r.tool.selectedPoint() == 2);

	// The points land where the mouse was (the ground plane, y = 0).
	const glm::mat4 model = HE::worldMatrixOf(r.world, r.spline());
	CHECK(near(glm::vec3(model * glm::vec4(splineOf(r.world, r.spline()).controlPoints[1], 1.0f)), { 0, 0, -2 }, 0.05f));

	// A click on the curve inserts instead of appending.
	const HE::spline::Curve curve(splineOf(r.world, r.spline()));
	const glm::vec3 onCurve = glm::vec3(model * glm::vec4(curve.position(0.5f), 1.0f));
	CHECK(r.tool.click(r.world, r.sel, &r.undo, r.view, pxOf(r.view, onCurve), {}) == C::InsertedPoint);
	CHECK(r.undo.undoDepth() == 4);
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 4);
	CHECK(r.tool.selectedPoint() == 1);   // the new point is the selected one

	// A click on a handle selects it and writes nothing.
	const glm::vec3 handle = glm::vec3(model * glm::vec4(splineOf(r.world, r.spline()).controlPoints[3], 1.0f));
	CHECK(r.tool.click(r.world, r.sel, &r.undo, r.view, pxOf(r.view, handle), {}) == C::SelectedPoint);
	CHECK(r.tool.selectedPoint() == 3);
	CHECK(r.undo.undoDepth() == 4);

	// Back through the whole drawing, with the selection cleared the way the
	// editor clears it, and the tool finding its spline again every time.
	for (int expected = 3; expected >= 1; --expected)
	{
		undoLikeTheEditor(r.undo, r.sel);
		CHECK(r.sel.empty());
		r.frame();
		REQUIRE((r.spline() != entt::null));   // reselected by UUID
		CHECK(splineOf(r.world, r.spline()).controlPoints.size() == static_cast<size_t>(expected));
	}
	// The last step removes the spline itself; then there is nothing to find.
	undoLikeTheEditor(r.undo, r.sel);
	r.frame();
	CHECK((r.spline() == entt::null));
	CHECK(r.world.registry().view<SplineComponent>().empty());

	// …and redo brings the first point back, selected again.
	redoLikeTheEditor(r.undo, r.sel);
	r.frame();
	REQUIRE((r.spline() != entt::null));
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 1);
}

TEST_CASE("Tool: the surface probe wins over the ground plane")
{
	Rig r;
	// A "hill" at height 3 everywhere.
	const SplineEdit::GroundProbe hill = [](const glm::vec3& o, const glm::vec3& d, glm::vec3& out)
	{
		return SplineEdit::rayHitsPlaneY(o, d, 3.0f, out);
	};
	REQUIRE(r.tool.click(r.world, r.sel, &r.undo, r.view, { 640.0f, 400.0f }, hill)
	        == SplineEdit::Tool::Click::CreatedSpline);
	CHECK(HE::worldPositionOf(r.world, r.spline()).y == doctest::Approx(3.0f).epsilon(1e-4));

	// A probe that misses falls back to the level of the last point, not to y = 0.
	const SplineEdit::GroundProbe miss = [](const glm::vec3&, const glm::vec3&, glm::vec3&) { return false; };
	REQUIRE(r.tool.click(r.world, r.sel, &r.undo, r.view, { 800.0f, 450.0f }, miss)
	        == SplineEdit::Tool::Click::AddedPoint);
	const glm::mat4 model = HE::worldMatrixOf(r.world, r.spline());
	CHECK((model * glm::vec4(splineOf(r.world, r.spline()).controlPoints[1], 1.0f)).y
	      == doctest::Approx(3.0f).epsilon(1e-3));
}

TEST_CASE("Tool: a click in the sky does nothing and is not an undo step")
{
	Rig r;
	// The top edge of the picture looks over the horizon: no ground there.
	r.view = makeView({ 0.0f, 1.0f, 12.0f });
	CHECK(r.tool.click(r.world, r.sel, &r.undo, r.view, { 640.0f, 5.0f }, {}) == SplineEdit::Tool::Click::Nothing);
	CHECK(r.undo.undoDepth() == 0);
	CHECK(r.world.registry().view<SplineComponent>().empty());
}

TEST_CASE("Tool: another spline under the mouse becomes the active one")
{
	Rig r;
	const Entity a = SplineEdit::create(r.world, &r.undo, { -6, 0, 0 });
	SplineEdit::appendPoint(r.world, a, &r.undo, { -3, 0, 0 });
	const Entity b = SplineEdit::create(r.world, &r.undo, { 3, 0, 0 });
	SplineEdit::appendPoint(r.world, b, &r.undo, { 6, 0, 0 });
	r.sel.set(a);
	r.frame();
	const size_t depth = r.undo.undoDepth();

	// b's first handle, while a is active.
	CHECK(r.tool.click(r.world, r.sel, &r.undo, r.view, pxOf(r.view, { 3, 0, 0 }), {})
	      == SplineEdit::Tool::Click::SelectedSpline);
	CHECK(r.spline() == b);
	CHECK(r.tool.selectedPoint() == 0);
	CHECK(r.undo.undoDepth() == depth);   // selecting is not editing
}

// ── Delete, Esc, close ───────────────────────────────────────────────────────

TEST_CASE("Tool: Delete removes the selected point and falls through when none is selected")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 0, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	REQUIRE(splineOf(r.world, r.spline()).controlPoints.size() == 3);
	const size_t depth = r.undo.undoDepth();

	// Point 2 is selected (the last click): Delete takes it, the neighbour is selected.
	CHECK(r.tool.ownsDeleteKey());
	CHECK(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));
	CHECK(r.undo.undoDepth() == depth + 1);
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 2);
	CHECK(r.tool.selectedPoint() == 1);
	CHECK(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));
	CHECK(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));   // the last one: the spline stays, empty
	CHECK(splineOf(r.world, r.spline()).controlPoints.empty());
	CHECK(r.tool.selectedPoint() == -1);

	// Nothing selected any more: the key is not the tool's.
	CHECK_FALSE(r.tool.ownsDeleteKey());
	CHECK_FALSE(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));
	CHECK(r.world.registry().valid(r.spline()));

	// Undo brings the points back, one per step.
	undoLikeTheEditor(r.undo, r.sel); r.frame();
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 1);
	undoLikeTheEditor(r.undo, r.sel); r.frame();
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 2);
}

TEST_CASE("Tool: Esc lets go of the point first, the selection second")
{
	Rig r;
	r.clickGround({ 0, 0, 0 });
	REQUIRE(r.tool.selectedPoint() == 0);
	CHECK(r.tool.escape());                 // had a point: consumed
	CHECK(r.tool.selectedPoint() == -1);
	CHECK_FALSE(r.tool.escape());           // none: the editor's Esc (deselect) takes over
}

TEST_CASE("Tool: closing toggles the flag, with undo, and the third point makes it a loop")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	const size_t depth = r.undo.undoDepth();

	CHECK(r.tool.toggleClosed(r.world, r.sel, &r.undo));
	CHECK(splineOf(r.world, r.spline()).closed);
	CHECK(r.undo.undoDepth() == depth + 1);
	// Two points cannot enclose anything: the flag stays, the curve stays open.
	CHECK_FALSE(HE::spline::Curve(splineOf(r.world, r.spline())).closed());
	r.clickGround({ 0, 0, 5 });
	CHECK(HE::spline::Curve(splineOf(r.world, r.spline())).closed());

	CHECK(r.tool.toggleClosed(r.world, r.sel, &r.undo));
	CHECK_FALSE(splineOf(r.world, r.spline()).closed);
	undoLikeTheEditor(r.undo, r.sel); r.frame();
	CHECK(splineOf(r.world, r.spline()).closed);

	// Nothing active: nothing to toggle.
	r.sel.clear();
	r.frame();
	CHECK_FALSE(r.tool.toggleClosed(r.world, r.sel, &r.undo));
}

// ── Dragging a point ─────────────────────────────────────────────────────────

TEST_CASE("Tool: a drag is one undo entry however many frames it runs")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	const size_t depth = r.undo.undoDepth();
	const glm::vec3 before = splineOf(r.world, r.spline()).controlPoints[1];

	r.tool.beginMove(&r.undo);
	for (int i = 1; i <= 10; ++i)
		CHECK(r.tool.moveSelectedPoint(r.world, r.sel, &r.undo, { 4.0f, 0.0f, 0.5f * i }));
	r.tool.endMove(&r.undo);

	CHECK(r.undo.undoDepth() == depth + 1);
	CHECK(r.undo.undoLabel() == "Move Spline Point");
	CHECK(near(splineOf(r.world, r.spline()).controlPoints[1], { before.x, before.y, before.z + 5.0f }, 0.05f));

	undoLikeTheEditor(r.undo, r.sel); r.frame();
	CHECK(near(splineOf(r.world, r.spline()).controlPoints[1], before, 1e-3f));
	redoLikeTheEditor(r.undo, r.sel); r.frame();
	CHECK(near(splineOf(r.world, r.spline()).controlPoints[1], { before.x, before.y, before.z + 5.0f }, 0.05f));
}

TEST_CASE("Tool: Delete during a live drag is refused, and the drag is still one entry")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	r.clickGround({ 4, 0, 6 });
	const size_t depth = r.undo.undoDepth();
	REQUIRE(r.tool.selectedPoint() == 2);

	r.tool.beginMove(&r.undo);
	CHECK(r.tool.dragLive());
	CHECK(r.tool.moveSelectedPoint(r.world, r.sel, &r.undo, { 4, 0, 9 }));
	// The key arrives mid-drag: nothing is deleted, the selected point stays.
	CHECK_FALSE(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));
	CHECK(splineOf(r.world, r.spline()).controlPoints.size() == 3);
	CHECK(r.tool.selectedPoint() == 2);
	CHECK(r.tool.moveSelectedPoint(r.world, r.sel, &r.undo, { 4, 0, 11 }));
	r.tool.endMove(&r.undo);
	CHECK_FALSE(r.tool.dragLive());

	CHECK(r.undo.undoDepth() == depth + 1);   // the drag, and only the drag
	undoLikeTheEditor(r.undo, r.sel); r.frame();
	REQUIRE(splineOf(r.world, r.spline()).controlPoints.size() == 3);
	// Back at the pre-drag spot: the point is where it was clicked (z = 6 world).
	const glm::mat4 model = HE::worldMatrixOf(r.world, r.spline());
	CHECK(near(glm::vec3(model * glm::vec4(splineOf(r.world, r.spline()).controlPoints[2], 1.0f)), { 4, 0, 6 }, 0.05f));
	// And with the drag over, Delete works again.
	r.tool.selectPoint(2);
	CHECK(r.tool.deleteSelectedPoint(r.world, r.sel, &r.undo));
}

TEST_CASE("Tool: grabbing a handle without moving it leaves no history entry")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	const size_t depth = r.undo.undoDepth();

	// The gizmo reports "in use" on the frame it takes hold, at the point's own position.
	const glm::mat4 model = HE::worldMatrixOf(r.world, r.spline());
	const glm::vec3 where = glm::vec3(model * glm::vec4(splineOf(r.world, r.spline()).controlPoints[1], 1.0f));
	r.tool.beginMove(&r.undo);
	CHECK_FALSE(r.tool.moveSelectedPoint(r.world, r.sel, &r.undo, where));
	r.tool.endMove(&r.undo);
	CHECK(r.undo.undoDepth() == depth);
}

// ── The tool across frames ───────────────────────────────────────────────────

TEST_CASE("Tool::sync: a deselect lets go, an undo that clears the selection does not")
{
	Rig r;
	r.clickGround({ 0, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	r.frame();
	REQUIRE(r.tool.selectedPoint() == 1);

	// The user deselects (Esc): the history did not move, so the tool forgets.
	r.sel.clear();
	r.frame();
	CHECK((r.spline() == entt::null));
	CHECK(r.tool.selectedPoint() == -1);
	// A later edit elsewhere must not drag the spline back into the selection.
	r.undo.snapshotNow("something else");
	r.frame();
	CHECK(r.sel.empty());
}

TEST_CASE("Tool::sync: a selected point past the end after an undo is dropped")
{
	Rig r;
	r.clickGround({ 0, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	r.clickGround({ 8, 0, 0 });
	r.frame();
	REQUIRE(r.tool.selectedPoint() == 2);

	undoLikeTheEditor(r.undo, r.sel);   // the third point is gone, the index is stale
	r.frame();
	REQUIRE((r.spline() != entt::null));
	CHECK(r.tool.selectedPoint() == -1);
	CHECK(r.tool.guides(r.world, r.sel).selectedPoint == -1);
}

TEST_CASE("Tool::sync: a different selected entity resets the point")
{
	Rig r;
	r.clickGround({ 0, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	r.frame();
	const Entity other = r.world.createEntity("Other");
	r.sel.set(other);
	r.frame();
	CHECK((r.spline() == entt::null));
	CHECK(r.tool.selectedPoint() == -1);
	CHECK_FALSE(r.tool.ownsDeleteKey());
}

TEST_CASE("Tool::hover marks a handle or the spot on the curve, and the guides carry it")
{
	Rig r;
	r.clickGround({ -4, 0, 0 });
	r.clickGround({ 4, 0, 0 });
	r.clickGround({ 4, 0, 6 });
	const glm::mat4 model = HE::worldMatrixOf(r.world, r.spline());
	const auto& pts = splineOf(r.world, r.spline()).controlPoints;

	r.tool.hover(r.world, r.sel, r.view, pxOf(r.view, glm::vec3(model * glm::vec4(pts[1], 1.0f))));
	SplineEdit::GuideState g = r.tool.guides(r.world, r.sel);
	CHECK(g.active == r.spline());
	CHECK(g.hoveredPoint == 1);
	CHECK_FALSE(g.hasInsert);

	const HE::spline::Curve curve(splineOf(r.world, r.spline()));
	r.tool.hover(r.world, r.sel, r.view, pxOf(r.view, glm::vec3(model * glm::vec4(curve.position(0.5f), 1.0f))));
	g = r.tool.guides(r.world, r.sel);
	CHECK(g.hoveredPoint == -1);
	REQUIRE(g.hasInsert);
	CHECK(near(g.insertWorld, glm::vec3(model * glm::vec4(curve.position(0.5f), 1.0f)), 0.15f));

	r.tool.clearHover();
	g = r.tool.guides(r.world, r.sel);
	CHECK_FALSE(g.hasInsert);
}

// ── The line on screen ───────────────────────────────────────────────────────

namespace
{
	namespace O = HE::Ed::ViewportOverlays;

	int linesWithColour(const DebugDrawBuffer& b, const glm::vec3& c)
	{
		int n = 0;
		for (const DebugLine& l : b.lines())
			if (glm::length(l.color - c) < 1e-4f) ++n;
		return n;
	}
}

TEST_CASE("appendSplineGuides: a selected spline is a line with a box on every point")
{
	HorizonWorld world;
	EditorSelection sel;
	const Entity e = SplineEdit::create(world, nullptr, { 0, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 4, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 4, 0, 4 });
	sel.set(e);

	DebugDrawBuffer buf;
	O::appendSplineGuides(world, sel, {}, { 0, 10, 10 }, buf);
	// Two spans of the curve, three handle boxes of twelve edges.
	CHECK(linesWithColour(buf, O::kSplineColor) == 2 * SplineEdit::kSamplesPerSpan);
	CHECK(linesWithColour(buf, O::kSplineStartColor) == 12);    // the first point
	CHECK(linesWithColour(buf, O::kSplineHandleColor) == 24);   // the other two
	CHECK(buf.lines().size() == static_cast<size_t>(2 * SplineEdit::kSamplesPerSpan + 36));

	// Not selected: the line stays (a spline is otherwise invisible), the handles go.
	sel.clear();
	DebugDrawBuffer dim;
	O::appendSplineGuides(world, sel, {}, { 0, 10, 10 }, dim);
	CHECK(dim.lines().size() == static_cast<size_t>(2 * SplineEdit::kSamplesPerSpan));
	CHECK(linesWithColour(dim, O::kSplineDimColor) == 2 * SplineEdit::kSamplesPerSpan);
}

TEST_CASE("appendSplineGuides: a closed spline's ring ends where it began")
{
	HorizonWorld world;
	EditorSelection sel;
	const Entity e = SplineEdit::create(world, nullptr, { 0, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 5, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 5, 0, 5 });
	SplineEdit::appendPoint(world, e, nullptr, { 0, 0, 5 });
	SplineEdit::setClosed(world, e, nullptr, true);

	DebugDrawBuffer buf;
	O::appendSplineGuides(world, sel, {}, { 0, 10, 10 }, buf);
	// Four spans, the closing one included.
	REQUIRE(buf.lines().size() == static_cast<size_t>(4 * SplineEdit::kSamplesPerSpan));
	CHECK(near(buf.lines().front().start, buf.lines().back().end, 1e-4f));
}

TEST_CASE("appendSplineGuides: the selected and hovered handle are marked, the curve is in world space")
{
	HorizonWorld world;
	EditorSelection sel;
	const Entity e = SplineEdit::create(world, nullptr, { 0, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 4, 0, 0 });
	SplineEdit::appendPoint(world, e, nullptr, { 4, 0, 4 });
	// Under a moved, scaled parent the points stand where the parent puts them.
	const Entity parent = world.createEntity("Parent");
	TransformComponent pt; pt.position = { 100, 0, 0 }; pt.scale = glm::vec3(2.0f);
	world.registry().emplace<TransformComponent>(parent, pt);
	world.reparentEntity(e, parent);
	sel.set(e);

	SplineEdit::GuideState state;
	state.active        = e;
	state.selectedPoint = 1;
	state.hoveredPoint  = 2;
	state.hasInsert     = true;
	state.insertWorld   = { 104, 0, 0 };
	DebugDrawBuffer buf;
	O::appendSplineGuides(world, sel, state, { 100, 10, 10 }, buf);

	CHECK(linesWithColour(buf, O::kSplineSelectedColor) == 12);
	CHECK(linesWithColour(buf, O::kSplineHoverColor) == 12 + 3);   // the box and the insert cross

	const glm::vec3 first = HE::worldPositionOf(world, e);
	bool curveStartsAtEntity = false;
	for (const DebugLine& l : buf.lines())
		if (glm::length(l.color - O::kSplineColor) < 1e-4f && near(l.start, first, 1e-3f)) curveStartsAtEntity = true;
	CHECK(curveStartsAtEntity);
	CHECK(first.x > 99.0f);   // not the local origin

	// Screen-constant: the same handle drawn from twice as far is twice as big.
	auto boxSize = [&](const glm::vec3& viewer) {
		DebugDrawBuffer b;
		O::appendSplineGuides(world, sel, {}, viewer, b);
		float lo = 1e30f, hi = -1e30f;
		for (const DebugLine& l : b.lines())
			if (glm::length(l.color - O::kSplineStartColor) < 1e-4f)
				for (const glm::vec3& p : { l.start, l.end }) { lo = std::min(lo, p.x); hi = std::max(hi, p.x); }
		return hi - lo;
	};
	CHECK(boxSize({ 100, 40, 0 }) == doctest::Approx(boxSize({ 100, 20, 0 }) * 2.0f).epsilon(0.05));
}
