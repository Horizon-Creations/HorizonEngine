#pragma once
#include <Math/Math.h>
#include <vector>

// ─── Spline: an open line or a closed shape ──────────────────────────────────
// A general-purpose curve through a handful of control points, for the editor's
// spline tool and for everything that wants "a line or an outline in the world":
// a lake shore, a path, a fence line. The component is only the AUTHORED DATA;
// it renders nothing and builds nothing. Sampling, arc length and the
// polyline approximation live in HE::spline::Curve (SplineCurve.h), which takes
// the points and the flag and has no idea what an entity is.
//
// The curve is a centripetal Catmull-Rom spline THROUGH the control points —
// the same one a rope uses (SplineGeometry.h), so a point you drag is a point
// the curve passes through, and there are no tangent handles to author.
struct SplineComponent
{
    // Control points in the entity's LOCAL space. Fewer than two give a curve
    // with no extent (a single point, or nothing at all) rather than an error:
    // the editor adds points one at a time, and every state on the way has to
    // be a valid component.
    std::vector<glm::vec3> controlPoints;

    // A closed spline joins its last point back to its first, with the curve
    // smooth across that join. Needs at least three points to mean anything;
    // with fewer the curve is treated as open (Curve::closed() says which), but
    // the flag stays as authored so adding the third point closes it without
    // the user having to flip it again.
    bool closed = false;
};
