#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity, HorizonWorld
#include <HorizonScene/Components/SplineComponent.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>

class EditorSelection;
class EditorUndo;

// ── The spline tool, without a window ────────────────────────────────────────
// What the Scene viewport's Spline mode does to the world: a click adds a
// point, a click on the curve inserts one, Delete removes one, a drag moves
// one, and every one of those is a single entry in the undo history. None of
// it needs ImGui, so none of it lives with ImGui: SplineTool.cpp reads the
// mouse and the keys and calls in here, and "does Ctrl+Z take the point back
// and put the spline back in the Details panel" is a question a test can ask
// with a registry, a camera matrix and an EditorUndo.
//
// Points are stored in the entity's LOCAL space (SplineComponent), the mouse
// and the gizmo speak WORLD. Every conversion goes through HE::worldMatrixOf —
// composed on the spot, never the stored matrix, which is a frame old — and the
// curve is sampled in local space and the SAMPLES moved to world: the spline is
// centripetal Catmull-Rom, which a non-uniform scale would bend if the control
// points were moved first (SplineCurve.h).
namespace SplineEdit
{
	// Samples per span for everything that draws or hit-tests the curve. One
	// number, so the line on screen and the line a click is measured against are
	// the same line.
	constexpr int   kSamplesPerSpan      = 16;
	// How close, in picture pixels, the mouse has to be to grab a handle, and to
	// count as "on the curve". The handle wins: its radius is the bigger one.
	constexpr float kPointPickRadiusPx   = 9.0f;
	constexpr float kSegmentPickRadiusPx = 6.0f;
	// A click that would land further out than this (the horizon, a plane the
	// ray meets at the far end of the world) is no click on the ground.
	constexpr float kMaxGroundDistance   = 5000.0f;

	// The picture the mouse is over: the matrix it was drawn with and the screen
	// rectangle it fills (ImGui screen space, y down).
	struct View
	{
		glm::mat4 viewProj{ 1.0f };
		glm::vec2 rectMin{ 0.0f };
		glm::vec2 rectSize{ 1.0f };
	};

	// World point → screen pixel. False behind the camera.
	bool project(const View& view, const glm::vec3& world, glm::vec2& outPx);

	// ── Picking ──────────────────────────────────────────────────────────────
	struct Pick
	{
		enum class Kind { None, Point, Segment };
		Kind      kind = Kind::None;
		// Point: the control point. Segment: unused (-1).
		int       index = -1;
		// Segment: where in the control point list a new point goes (after the
		// span's first point; the closing span of a closed spline appends) and
		// the spot on the curve, in the spline's LOCAL space.
		int       insertIndex = -1;
		glm::vec3 local{ 0.0f };
		float     distancePx = 0.0f;
	};

	// What the mouse is on: a control point first, the curve second, else
	// nothing. `model` is the spline entity's world matrix.
	Pick pick(const SplineComponent& spline, const glm::mat4& model, const View& view,
	          const glm::vec2& mouse,
	          float pointRadiusPx   = kPointPickRadiusPx,
	          float segmentRadiusPx = kSegmentPickRadiusPx);

	// The ray through `mouse`, meeting the horizontal plane at height y. False
	// for a ray parallel to it, one that points away from it, or one that meets it
	// beyond kMaxGroundDistance.
	bool rayHitsPlaneY(const glm::vec3& origin, const glm::vec3& dir, float y, glm::vec3& out);

	// The scene surface under a ray (the viewport's triangle-exact probe, terrain
	// included). Optional: without one, or on a miss, the tool falls back to a
	// horizontal plane through the spline's last point.
	using GroundProbe = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, glm::vec3& out)>;

	// ── Edits ────────────────────────────────────────────────────────────────
	// Every edit that is a click or a key takes the undo snapshot itself, BEFORE
	// it changes anything, and takes it only once it knows the edit will happen —
	// a refused edit leaves no empty entry. `undo` may be null (a scratch world).

	// A new, selectable entity "Spline" at `worldPos` with one control point at
	// its own origin. entt::null if the world is gone.
	Entity create(HorizonWorld& world, EditorUndo* undo, const glm::vec3& worldPos);
	// Append a point at the end; returns its index, -1 if the entity has no spline.
	int    appendPoint(HorizonWorld& world, Entity e, EditorUndo* undo, const glm::vec3& worldPos);
	// Insert at `index` (0..count; count appends), `local` in the spline's space.
	// Returns the index the point ended up at, -1 on a bad index or entity.
	int    insertPoint(HorizonWorld& world, Entity e, EditorUndo* undo, int index, const glm::vec3& local);
	// Remove one point. The spline stays even when it was the last one: every
	// state on the way is a valid component (SplineComponent.h).
	bool   removePoint(HorizonWorld& world, Entity e, EditorUndo* undo, int index);
	// Join the last point back to the first, or open the loop again.
	bool   setClosed(HorizonWorld& world, Entity e, EditorUndo* undo, bool closed);

	// Move a point to a world position. No undo of its own: a move is a drag, and
	// a drag is ONE history entry however many frames it runs (Tool::beginMove).
	// False when the entity, the index or the position is no use.
	bool   movePoint(HorizonWorld& world, Entity e, int index, const glm::vec3& worldPos);

	// What the overlay needs to draw the tool's state next to the spline itself.
	struct GuideState
	{
		Entity    active        = entt::null;   // the spline being edited
		int       selectedPoint = -1;
		int       hoveredPoint  = -1;
		bool      hasInsert     = false;        // the mouse is on the curve …
		glm::vec3 insertWorld{ 0.0f };          // … and a click would add a point here
	};

	// ── The tool's state ─────────────────────────────────────────────────────
	// What survives from frame to frame while Spline mode is on: which point is
	// selected, which one the mouse is over. Everything it holds is an index or a
	// UUID, never an Entity — an undo rebuilds the world and every handle with it.
	class Tool
	{
	public:
		enum class Click
		{
			Nothing,          // missed: no ground under the mouse
			SelectedPoint,    // grabbed a control point of the active spline
			InsertedPoint,    // added a point on the curve
			AddedPoint,       // added a point at the end
			CreatedSpline,    // started a new spline
			SelectedSpline,   // switched to another spline
		};

		// Once per frame while the mode is on, before anything else: keeps the
		// state coherent with a world that undo, redo or a scene load may have
		// replaced. `undoRevision` is EditorUndo::revision().
		//
		// Undo and redo clear the selection (their handles are gone). When the
		// selection empties in the very frame the history moved, that was a
		// history jump and the spline that was being edited is selected again
		// by its UUID (if the step took it away altogether, the tool waits for
		// a redo to bring it back); when it empties without the history moving,
		// the user deselected and the tool lets go.
		void sync(HorizonWorld& world, EditorSelection& selection, std::uint64_t undoRevision);

		// The spline being edited: the primary selection if it has a SplineComponent.
		Entity activeSpline(HorizonWorld& world, const EditorSelection& selection) const;

		// Mouse over the picture, no button: refreshes the hover and the preview.
		void hover(HorizonWorld& world, const EditorSelection& selection, const View& view,
		           const glm::vec2& mouse);
		void clearHover();

		// A click on the picture. Priority: a handle of the active spline, its
		// curve, another spline, the ground.
		Click click(HorizonWorld& world, EditorSelection& selection, EditorUndo* undo,
		            const View& view, const glm::vec2& mouse, const GroundProbe& probe);

		// Delete: removes the selected point. False when none is selected — the
		// key then belongs to whoever else wants it (entity delete).
		bool deleteSelectedPoint(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo);
		// Esc: lets go of the selected point. False when none was selected.
		bool escape();
		// Flip the closed flag of the active spline.
		bool toggleClosed(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo);
		// Forget everything (the mode was left, a project was closed).
		void reset();

		// A drag of the selected point (the gizmo). beginMove captures the world
		// before anything has moved, moveSelectedPoint writes and stashes that
		// capture as the history entry on the first frame that really changes
		// something, endMove commits it. A drag that never moved the point leaves
		// no entry behind.
		void beginMove(EditorUndo* undo);
		bool moveSelectedPoint(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo,
		                       const glm::vec3& worldPos);
		void endMove(EditorUndo* undo);

		bool ownsDeleteKey() const { return m_selectedPoint >= 0; }
		int  selectedPoint() const { return m_selectedPoint; }
		void selectPoint(int index) { m_selectedPoint = index; }

		// The overlay's view of the state for this frame.
		GuideState guides(HorizonWorld& world, const EditorSelection& selection) const;

	private:
		bool groundUnder(HorizonWorld& world, Entity active, const View& view, const glm::vec2& mouse,
		                 const GroundProbe& probe, glm::vec3& out) const;

		HE::UUID      m_activeId{};            // the spline last edited, for the history jump
		int           m_selectedPoint = -1;
		int           m_hoveredPoint  = -1;
		bool          m_hasInsert     = false;
		glm::vec3     m_insertLocal{ 0.0f };   // local to the active spline
		bool          m_hadActive     = false; // an active spline at the last sync
		std::uint64_t m_lastRevision  = 0;
		std::uint64_t m_lastSelRevision = 0;
		bool          m_moveStashed   = false; // the drag has written something
	};
}
