#include "SplineEdit.h"
#include "EditorSelection.h"
#include "EditorUndo.h"
#include "PreviewPick.h"                              // screenRay — the same ray the other picks use
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/TransformHierarchy.h>          // worldMatrixOf
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>

namespace SplineEdit
{

namespace
{
	bool finite(const glm::vec3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	// The matrix that takes a world point into the spline's own space. Not
	// every matrix has an inverse (a scale of zero flattens the spline to a
	// point): such an entity cannot be edited by pointing at the world, and the
	// edit is refused rather than written as NaN.
	bool toLocal(HorizonWorld& world, Entity e, const glm::vec3& worldPos, glm::vec3& out)
	{
		const glm::mat4 model = HE::worldMatrixOf(world, e);
		if (std::abs(glm::determinant(model)) < 1e-12f) return false;
		out = glm::vec3(glm::inverse(model) * glm::vec4(worldPos, 1.0f));
		return finite(out);
	}

	SplineComponent* splineOf(HorizonWorld& world, Entity e)
	{
		auto& reg = world.registry();
		return (e != entt::null && reg.valid(e)) ? reg.try_get<SplineComponent>(e) : nullptr;
	}

	// Closest point to `p` on the segment a-b, as the fraction along it.
	float closestOnSegment(const glm::vec2& a, const glm::vec2& b, const glm::vec2& p)
	{
		const glm::vec2 ab = b - a;
		const float len2 = glm::dot(ab, ab);
		if (len2 < 1e-12f) return 0.0f;
		return std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
	}
}

bool project(const View& view, const glm::vec3& world, glm::vec2& outPx)
{
	const glm::vec4 clip = view.viewProj * glm::vec4(world, 1.0f);
	if (clip.w <= 1e-6f) return false;
	outPx = glm::vec2(view.rectMin.x + (clip.x / clip.w * 0.5f + 0.5f) * view.rectSize.x,
	                  view.rectMin.y + (0.5f - clip.y / clip.w * 0.5f) * view.rectSize.y);
	return true;
}

Pick pick(const SplineComponent& spline, const glm::mat4& model, const View& view,
          const glm::vec2& mouse, float pointRadiusPx, float segmentRadiusPx)
{
	Pick best;

	// A handle first: it sits ON the curve, so the curve under it must not win.
	float bestPx = pointRadiusPx;
	for (size_t i = 0; i < spline.controlPoints.size(); ++i)
	{
		glm::vec2 px;
		if (!project(view, glm::vec3(model * glm::vec4(spline.controlPoints[i], 1.0f)), px)) continue;
		const float d = glm::length(px - mouse);
		if (d <= bestPx)
		{
			bestPx          = d;
			best.kind       = Pick::Kind::Point;
			best.index      = static_cast<int>(i);
			best.distancePx = d;
		}
	}
	if (best.kind == Pick::Kind::Point) return best;

	// Then the curve: the same samples the overlay draws, moved to world and
	// projected one chord at a time. The index of the chord says which span it
	// belongs to, which is where the new control point goes.
	const HE::spline::Curve curve(spline);
	if (curve.spanCount() == 0) return best;
	const std::vector<glm::vec3> samples = curve.sample(kSamplesPerSpan);

	bestPx = segmentRadiusPx;
	size_t bestChord = 0;
	float  bestU     = 0.0f;
	bool   found     = false;
	glm::vec2 prev;
	bool      prevOk = project(view, glm::vec3(model * glm::vec4(samples.front(), 1.0f)), prev);
	for (size_t i = 1; i < samples.size(); ++i)
	{
		glm::vec2 cur;
		const bool curOk = project(view, glm::vec3(model * glm::vec4(samples[i], 1.0f)), cur);
		if (prevOk && curOk)
		{
			const float u = closestOnSegment(prev, cur, mouse);
			const float d = glm::length(prev + (cur - prev) * u - mouse);
			if (d <= bestPx)
			{
				bestPx    = d;
				bestChord = i - 1;
				bestU     = u;
				found     = true;
			}
		}
		prev   = cur;
		prevOk = curOk;
	}
	if (!found) return best;

	// Chord i covers t in [i, i+1] / kSamplesPerSpan, t counting spans.
	const float t    = (static_cast<float>(bestChord) + bestU) / static_cast<float>(kSamplesPerSpan);
	const int   span = std::min(static_cast<int>(std::floor(t)), curve.spanCount() - 1);
	best.kind        = Pick::Kind::Segment;
	best.insertIndex = span + 1;
	best.local       = curve.position(t);
	best.distancePx  = bestPx;
	return best;
}

bool rayHitsPlaneY(const glm::vec3& origin, const glm::vec3& dir, float y, glm::vec3& out)
{
	if (std::abs(dir.y) < 1e-6f) return false;
	const float t = (y - origin.y) / dir.y;
	if (!(t > 0.0f)) return false;
	if (t * glm::length(dir) > kMaxGroundDistance) return false;
	out = origin + dir * t;
	return finite(out);
}

// ── Edits ────────────────────────────────────────────────────────────────────

Entity create(HorizonWorld& world, EditorUndo* undo, const glm::vec3& worldPos)
{
	if (!finite(worldPos)) return entt::null;
	if (undo) undo->snapshotNow("Add Spline");
	const Entity e = world.createEntity("Spline");
	auto& reg = world.registry();
	TransformComponent t;
	t.position = worldPos;           // a child of the world root: local is world
	reg.emplace<TransformComponent>(e, t);
	SplineComponent s;
	s.controlPoints.push_back(glm::vec3(0.0f));
	reg.emplace<SplineComponent>(e, std::move(s));
	return e;
}

int appendPoint(HorizonWorld& world, Entity e, EditorUndo* undo, const glm::vec3& worldPos)
{
	SplineComponent* s = splineOf(world, e);
	glm::vec3 local;
	if (!s || !toLocal(world, e, worldPos, local)) return -1;
	if (undo) undo->snapshotNow("Add Spline Point");
	s->controlPoints.push_back(local);
	return static_cast<int>(s->controlPoints.size()) - 1;
}

int insertPoint(HorizonWorld& world, Entity e, EditorUndo* undo, int index, const glm::vec3& local)
{
	SplineComponent* s = splineOf(world, e);
	if (!s || !finite(local)) return -1;
	const int count = static_cast<int>(s->controlPoints.size());
	if (index < 0 || index > count) return -1;
	if (undo) undo->snapshotNow("Insert Spline Point");
	s->controlPoints.insert(s->controlPoints.begin() + index, local);
	return index;
}

bool removePoint(HorizonWorld& world, Entity e, EditorUndo* undo, int index)
{
	SplineComponent* s = splineOf(world, e);
	if (!s || index < 0 || index >= static_cast<int>(s->controlPoints.size())) return false;
	if (undo) undo->snapshotNow("Delete Spline Point");
	s->controlPoints.erase(s->controlPoints.begin() + index);
	return true;
}

bool setClosed(HorizonWorld& world, Entity e, EditorUndo* undo, bool closed)
{
	SplineComponent* s = splineOf(world, e);
	if (!s || s->closed == closed) return false;
	if (undo) undo->snapshotNow(closed ? "Close Spline" : "Open Spline");
	s->closed = closed;
	return true;
}

bool movePoint(HorizonWorld& world, Entity e, int index, const glm::vec3& worldPos)
{
	SplineComponent* s = splineOf(world, e);
	glm::vec3 local;
	if (!s || index < 0 || index >= static_cast<int>(s->controlPoints.size())) return false;
	if (!toLocal(world, e, worldPos, local)) return false;
	s->controlPoints[static_cast<size_t>(index)] = local;
	return true;
}

// ── The tool ─────────────────────────────────────────────────────────────────

Entity Tool::activeSpline(HorizonWorld& world, const EditorSelection& selection) const
{
	const Entity e = selection.primary();
	const auto& reg = world.registry();
	return (e != entt::null && reg.valid(e) && reg.all_of<SplineComponent>(e)) ? e : entt::null;
}

void Tool::sync(HorizonWorld& world, EditorSelection& selection, std::uint64_t undoRevision)
{
	auto& reg = world.registry();
	const bool historyMoved     = undoRevision != m_lastRevision;
	const bool selectionTouched = selection.revision() != m_lastSelRevision;

	Entity active = activeSpline(world, selection);
	if (active == entt::null)
	{
		if (m_hadActive && selection.empty())
		{
			if (historyMoved)
			{
				// Undo and redo clear the selection and rebuild every entity. The
				// spline that was being edited is the same one by UUID: select it
				// again, or the tool would lose its target at the very moment the
				// user is stepping back through what they drew. When the step took
				// the spline away altogether (the undo of its first click), the
				// memory stays, and a redo that brings it back finds it again.
				const Entity back = world.findByEntityId(m_activeId);
				if (back != entt::null && reg.all_of<SplineComponent>(back))
				{
					selection.set(back);
					active = back;
				}
			}
			else if (selectionTouched)
				reset();   // the user deselected: let go
		}
		else if (!selection.empty())
			reset();       // something else is selected
	}

	if (active == entt::null)
		clearHover();
	else
	{
		const HE::UUID id = world.entityId(active);
		if (id != m_activeId) m_selectedPoint = -1;
		m_activeId  = id;
		m_hadActive = true;
		// The list may be shorter than it was: an undo took the point away.
		const int count = static_cast<int>(reg.get<SplineComponent>(active).controlPoints.size());
		if (m_selectedPoint >= count) m_selectedPoint = -1;
	}

	m_lastRevision    = undoRevision;
	m_lastSelRevision = selection.revision();
}

void Tool::reset()
{
	m_activeId      = {};
	m_selectedPoint = -1;
	m_hadActive     = false;
	m_moveStashed   = false;
	clearHover();
}

void Tool::clearHover()
{
	m_hoveredPoint = -1;
	m_hasInsert    = false;
}

void Tool::hover(HorizonWorld& world, const EditorSelection& selection, const View& view,
                 const glm::vec2& mouse)
{
	clearHover();
	const Entity active = activeSpline(world, selection);
	if (active == entt::null) return;
	const Pick p = pick(world.registry().get<SplineComponent>(active),
	                    HE::worldMatrixOf(world, active), view, mouse);
	if (p.kind == Pick::Kind::Point)
		m_hoveredPoint = p.index;
	else if (p.kind == Pick::Kind::Segment)
	{
		m_hasInsert   = true;
		m_insertLocal = p.local;
	}
}

bool Tool::groundUnder(HorizonWorld& world, Entity active, const View& view, const glm::vec2& mouse,
                       const GroundProbe& probe, glm::vec3& out) const
{
	glm::vec3 origin, dir;
	if (!PreviewPick::screenRay(view.viewProj, view.rectMin, view.rectSize, mouse, origin, dir))
		return false;
	if (probe && probe(origin, dir, out)) return true;

	// No surface under the mouse: the height of what is already drawn, so a line
	// started over open sky continues level instead of diving to the origin.
	float y = 0.0f;
	if (active != entt::null)
	{
		const glm::mat4 model = HE::worldMatrixOf(world, active);
		const auto& pts = world.registry().get<SplineComponent>(active).controlPoints;
		y = pts.empty() ? model[3].y : (model * glm::vec4(pts.back(), 1.0f)).y;
	}
	return rayHitsPlaneY(origin, dir, y, out);
}

Tool::Click Tool::click(HorizonWorld& world, EditorSelection& selection, EditorUndo* undo,
                        const View& view, const glm::vec2& mouse, const GroundProbe& probe)
{
	auto& reg = world.registry();
	const Entity active = activeSpline(world, selection);

	if (active != entt::null)
	{
		const Pick p = pick(reg.get<SplineComponent>(active), HE::worldMatrixOf(world, active),
		                    view, mouse);
		if (p.kind == Pick::Kind::Point)
		{
			m_selectedPoint = p.index;
			return Click::SelectedPoint;
		}
		if (p.kind == Pick::Kind::Segment)
		{
			const int at = insertPoint(world, active, undo, p.insertIndex, p.local);
			if (at >= 0)
			{
				m_selectedPoint = at;
				return Click::InsertedPoint;
			}
		}
	}

	// Another spline under the mouse is a different target, not a new point.
	Entity other = entt::null;
	Pick   otherPick;
	for (auto [e, s] : reg.view<SplineComponent>().each())
	{
		if (e == active) continue;
		const Pick p = pick(s, HE::worldMatrixOf(world, e), view, mouse);
		if (p.kind != Pick::Kind::None &&
		    (other == entt::null || p.distancePx < otherPick.distancePx))
		{
			other     = e;
			otherPick = p;
		}
	}
	if (other != entt::null)
	{
		selection.set(other);
		m_activeId      = world.entityId(other);
		m_hadActive     = true;
		m_selectedPoint = otherPick.kind == Pick::Kind::Point ? otherPick.index : -1;
		return Click::SelectedSpline;
	}

	glm::vec3 ground;
	if (!groundUnder(world, active, view, mouse, probe, ground)) return Click::Nothing;

	if (active == entt::null)
	{
		const Entity created = create(world, undo, ground);
		if (created == entt::null) return Click::Nothing;
		selection.set(created);
		m_activeId      = world.entityId(created);
		m_hadActive     = true;
		m_selectedPoint = 0;
		return Click::CreatedSpline;
	}

	const int at = appendPoint(world, active, undo, ground);
	if (at < 0) return Click::Nothing;
	m_selectedPoint = at;
	return Click::AddedPoint;
}

bool Tool::deleteSelectedPoint(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo)
{
	if (m_selectedPoint < 0) return false;
	const Entity active = activeSpline(world, selection);
	if (active == entt::null) return false;
	if (!removePoint(world, active, undo, m_selectedPoint)) return false;
	// The neighbour takes over, so a second Delete keeps eating the line instead
	// of falling through to the entity behind it.
	const int count = static_cast<int>(world.registry().get<SplineComponent>(active).controlPoints.size());
	m_selectedPoint = count == 0 ? -1 : std::min(m_selectedPoint, count - 1);
	return true;
}

bool Tool::escape()
{
	if (m_selectedPoint < 0) return false;
	m_selectedPoint = -1;
	return true;
}

bool Tool::toggleClosed(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo)
{
	const Entity active = activeSpline(world, selection);
	if (active == entt::null) return false;
	return setClosed(world, active, undo, !world.registry().get<SplineComponent>(active).closed);
}

void Tool::beginMove(EditorUndo* undo)
{
	m_moveStashed = false;
	if (undo) undo->capturePre();
}

bool Tool::moveSelectedPoint(HorizonWorld& world, const EditorSelection& selection, EditorUndo* undo,
                             const glm::vec3& worldPos)
{
	const Entity active = activeSpline(world, selection);
	if (active == entt::null || m_selectedPoint < 0) return false;
	// The gizmo reports "in use" on the frame it grabs the handle, before
	// anything has moved (and the float round trip through the gizmo's matrix is
	// good to about a tenth of a millimetre at a hundred metres). A grab that goes
	// nowhere must not start an entry.
	const SplineComponent* s = splineOf(world, active);
	glm::vec3 target;
	if (!s || m_selectedPoint >= static_cast<int>(s->controlPoints.size()) ||
	    !toLocal(world, active, worldPos, target))
		return false;
	if (glm::all(glm::lessThan(glm::abs(target - s->controlPoints[static_cast<size_t>(m_selectedPoint)]),
	                           glm::vec3(1e-4f))))
		return false;
	if (!m_moveStashed)
	{
		if (undo) undo->stashPre("Move Spline Point");
		m_moveStashed = true;
	}
	return movePoint(world, active, m_selectedPoint, worldPos);
}

void Tool::endMove(EditorUndo* undo)
{
	if (undo && m_moveStashed) undo->commitPending();
	m_moveStashed = false;
}

GuideState Tool::guides(HorizonWorld& world, const EditorSelection& selection) const
{
	GuideState g;
	g.active = activeSpline(world, selection);
	if (g.active == entt::null) return g;
	g.selectedPoint = m_selectedPoint;
	g.hoveredPoint  = m_hoveredPoint;
	g.hasInsert     = m_hasInsert;
	if (m_hasInsert)
		g.insertWorld = glm::vec3(HE::worldMatrixOf(world, g.active) * glm::vec4(m_insertLocal, 1.0f));
	return g;
}

}
