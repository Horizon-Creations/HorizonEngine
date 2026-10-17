#pragma once
#include "HorizonScene/SplineGeometry.h"
#include "HorizonScene/Components/SplineComponent.h"
#include <Math/Math.h>
#include <vector>

// ── A spline you can ask questions ───────────────────────────────────────────
// The read side of SplineComponent: control points and a closed flag in, a curve
// out that can be evaluated at a parameter, walked by distance, and flattened to
// a polyline. Free of registry, renderer and editor — the spline tool, the lake
// shore and a preview draw all want these same answers, and only this way are
// they testable without a world.
//
// The curve is the centripetal Catmull-Rom one of SplineGeometry.h
// (HE::spline::CatmullRomSpan), so it passes THROUGH every control point and is
// the same curve a rope builds. Everything here is in the space the control
// points are in — LOCAL space for a SplineComponent. Transform the SAMPLES, not
// the control points: centripetal Catmull-Rom is invariant under rotation,
// translation and uniform scale but not under a non-uniform scale (the knot
// spacing follows the distances), so moving the points first can bend the curve.
//
// A Curve is a snapshot. It copies the points and builds its span and length
// tables in the constructor; edit the component and build a new one.
namespace HE::spline
{
    class Curve
    {
    public:
        Curve() = default;
        Curve(std::vector<glm::vec3> controlPoints, bool closed);
        explicit Curve(const SplineComponent& spline) : Curve(spline.controlPoints, spline.closed) {}

        // ── Shape ────────────────────────────────────────────────────────────
        // Whether the curve really is closed: the flag AND at least three points.
        // Two points cannot enclose anything, so a "closed" pair is the open line
        // between them.
        bool   closed()     const { return m_closed; }
        size_t pointCount() const { return m_points.size(); }

        // The pieces between control points: n - 1 open, n closed, 0 below two points.
        int    spanCount()  const { return static_cast<int>(m_spans.size()); }

        // ── Sampling by parameter ────────────────────────────────────────────
        // t counts spans: the integer part picks the span, the fraction runs
        // along it, so t = k is exactly control point k and t = spanCount() is the
        // last point (open) or the first again (closed). It is NOT arc length —
        // equal steps in t cover unequal distances where the curve bends; use the
        // *AtLength functions for even spacing. Open curves clamp t to
        // [0, spanCount()], closed ones wrap around. A non-finite t reads as 0.
        float     maxParam() const { return static_cast<float>(m_spans.size()); }
        glm::vec3 position(float t) const;

        // Unit direction of travel at t. Where the curve has none (every point in
        // one place, a single point) it falls back to the chord through the
        // coincident pair's neighbours, and then to +Z — never to a NaN.
        glm::vec3 tangent(float t) const;

        // ── Sampling by arc length ───────────────────────────────────────────
        // Total length, from a table of 32 chords per span: within about 0.1 % of
        // the true length on a smooth curve, and exact for a straight one. A
        // closed curve includes the span that closes it.
        float length() const { return m_cumulative.empty() ? 0.0f : m_cumulative.back(); }

        // The parameter t at distance s along the curve from the first point.
        // Open curves clamp s to [0, length()], closed ones wrap around, so
        // walking a loop twice is just s running past length().
        //
        // Read off the length table, so t is interpolated linearly inside one
        // chord (1/32 of a span) and a position found this way is off from the
        // exact distance by a small fraction of a chord's length (measured: 0.2 mm
        // at s = 1 m along a straight 10 m span).
        float     paramAtLength(float s) const;
        glm::vec3 positionAtLength(float s) const { return position(paramAtLength(s)); }
        glm::vec3 tangentAtLength(float s)  const { return tangent(paramAtLength(s)); }

        // ── Polylines ────────────────────────────────────────────────────────
        // `samplesPerSpan` evenly spaced steps in t per span (clamped to >= 1),
        // spanCount() * samplesPerSpan + 1 points, through every control point.
        // The same points sampleCatmullRom gives for the same open curve.
        std::vector<glm::vec3> sample(int samplesPerSpan) const;

        // An adaptive polyline: as few segments as keep the curve within
        // `tolerance` of it. Each span is split in half, recursively, until the
        // curve at a quarter, a half and three quarters of the segment lies
        // within `tolerance` of the segment itself — so a straight stretch costs
        // one segment however long it is, and a tight bend costs many. That is a
        // sampled bound, not a proof: a wiggle between the three test points can
        // exceed it. The recursion is capped (12 halvings, 4096 segments per
        // span), so a tolerance of zero is slow and large rather than endless.
        //
        // Passes through every control point. A CLOSED curve returns a ring that
        // ends where it began — first == last, like sample() — so a plain line
        // strip draws it closed and the summed segments are its length. A
        // consumer that wants distinct polygon vertices drops the last point.
        // Below two control points: the points, verbatim.
        std::vector<glm::vec3> polyline(float tolerance) const;

    private:
        // 32 chords per span in the arc-length table. Fixed: the table is only
        // ever read back through paramAtLength/length, never exposed.
        static constexpr int kLutPerSpan = 32;

        // Which span and where in it: t clamped or wrapped, split into an index
        // and a fraction in [0, 1].
        void locate(float t, int& span, float& u) const;

        std::vector<glm::vec3>      m_points;
        bool                        m_closed = false;
        std::vector<CatmullRomSpan> m_spans;
        std::vector<float>          m_cumulative;   // arc length at each of spanCount * kLutPerSpan + 1 entries
    };
}
