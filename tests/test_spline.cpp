#include "doctest.h"
#include "TestFsUtil.h"
#include "InspectorPanel.h"

#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/SplineGeometry.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/SplineComponent.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

namespace fs = std::filesystem;
using namespace HE::spline;

namespace
{
    constexpr float kTwoPi = 6.28318530718f;

    // n points on a circle of radius r in the XZ plane, starting on +X and
    // running towards +Z (clockwise seen from above, +Y up).
    std::vector<glm::vec3> circlePoints(int n, float r)
    {
        std::vector<glm::vec3> p;
        for (int i = 0; i < n; ++i)
        {
            const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            p.push_back({ r * std::cos(a), 0.0f, r * std::sin(a) });
        }
        return p;
    }

    // Three-dimensional on purpose (y wanders too): a test that lives in one
    // plane cannot tell a curve that works in 3D from one that only works flat.
    std::vector<glm::vec3> wanderingCurve()
    {
        return { { 0.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, 1.0f }, { 3.0f, 0.5f, -2.0f },
                 { 6.0f, -1.0f, -1.0f }, { 7.0f, 0.0f, 2.0f } };
    }

    bool finite(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    float distanceToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
    {
        const glm::vec3 ab = b - a;
        const float l2 = glm::dot(ab, ab);
        const float f  = (l2 > 0.0f) ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + ab * f));
    }

    // How far the point is from the polyline (nearest segment).
    float distanceToPolyline(const glm::vec3& p, const std::vector<glm::vec3>& poly)
    {
        float best = 1e30f;
        for (size_t i = 0; i + 1 < poly.size(); ++i)
            best = std::min(best, distanceToSegment(p, poly[i], poly[i + 1]));
        return best;
    }

    float polylineLength(const std::vector<glm::vec3>& poly)
    {
        float len = 0.0f;
        for (size_t i = 0; i + 1 < poly.size(); ++i) len += glm::length(poly[i + 1] - poly[i]);
        return len;
    }

    void checkVec3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-5f)
    {
        CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
        CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
        CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1.0));
    }

    void checkSameSpline(const SplineComponent& a, const SplineComponent& b)
    {
        CHECK(a.closed == b.closed);
        REQUIRE(a.controlPoints.size() == b.controlPoints.size());
        for (size_t i = 0; i < a.controlPoints.size(); ++i)
            checkVec3(a.controlPoints[i], b.controlPoints[i], 1e-6f);
    }

    // The spline found on the entity with `points` control points. The tests
    // below author splines of different sizes so a loaded world can be matched
    // up without trusting entity order.
    const SplineComponent* splineWithPoints(HorizonWorld& world, size_t points)
    {
        for (auto [e, s] : world.registry().view<SplineComponent>().each())
            if (s.controlPoints.size() == points) return &s;
        return nullptr;
    }
}

// ── Sampling by parameter ────────────────────────────────────────────────────

TEST_CASE("An open spline passes through every control point at an integer parameter")
{
    const std::vector<glm::vec3> cps = wanderingCurve();
    const Curve c(cps, false);

    CHECK_FALSE(c.closed());
    CHECK(c.pointCount() == cps.size());
    CHECK(c.spanCount() == 4);
    CHECK(c.maxParam() == doctest::Approx(4.0f));

    // Exactly the control point, not merely near it: the ends of a span are
    // handed back directly rather than through the recursion's rounding.
    for (size_t i = 0; i < cps.size(); ++i)
        CHECK(c.position(static_cast<float>(i)) == cps[i]);

    // Open curves clamp: there is nothing before the first point or after the last.
    CHECK(c.position(-3.0f)  == cps.front());
    CHECK(c.position(99.0f)  == cps.back());
}

TEST_CASE("Between control points the spline is the same curve a rope builds")
{
    // One definition of the curve: HE::spline::Curve and sampleCatmullRom both
    // run CatmullRomSpan, so a rope and a spline through the same points
    // cannot drift apart.
    const std::vector<glm::vec3> cps = wanderingCurve();
    const Curve c(cps, false);

    const int steps = 7;
    const std::vector<glm::vec3> rope = sampleCatmullRom(cps, steps);
    const std::vector<glm::vec3> ours = c.sample(steps);

    REQUIRE(ours.size() == rope.size());
    REQUIRE(ours.size() == (cps.size() - 1) * static_cast<size_t>(steps) + 1);
    for (size_t i = 0; i < ours.size(); ++i)
        checkVec3(ours[i], rope[i], 1e-5f);

    // And position(t) is that curve, not a table lookup of it: a fraction in
    // between two samples agrees with the sample taken at that parameter.
    for (size_t i = 0; i < ours.size(); ++i)
        checkVec3(c.position(static_cast<float>(i) / static_cast<float>(steps)), ours[i], 1e-5f);
}

TEST_CASE("A closed spline wraps: parameter n is point 0 again")
{
    const std::vector<glm::vec3> cps = circlePoints(8, 4.0f);
    const Curve c(cps, true);

    REQUIRE(c.closed());
    CHECK(c.spanCount() == 8);
    CHECK(c.maxParam() == doctest::Approx(8.0f));

    for (size_t i = 0; i < cps.size(); ++i)
        CHECK(c.position(static_cast<float>(i)) == cps[i]);

    // Round the loop once and you are where you were — the point of "closed".
    CHECK(c.position(8.0f) == cps[0]);
    checkVec3(c.position(8.0f + 3.3f), c.position(3.3f));
    checkVec3(c.position(-0.5f), c.position(7.5f));
    checkVec3(c.position(2.0f * 8.0f + 1.25f), c.position(1.25f));
}

TEST_CASE("A closed spline is smooth across the join, an open one is not closed by it")
{
    const std::vector<glm::vec3> cps = circlePoints(8, 4.0f);
    const Curve closed(cps, true);

    // The direction just before each control point and just after it agree,
    // INCLUDING point 0 where the loop closes. A closing span built from the
    // wrong neighbours shows up here as a kink and nowhere else.
    for (int k = 0; k < 8; ++k)
    {
        const float t = static_cast<float>(k);
        const glm::vec3 before = closed.tangent(t - 1e-3f);
        const glm::vec3 after  = closed.tangent(t + 1e-3f);
        INFO("control point ", k);
        CHECK(glm::dot(before, after) > 0.999f);
    }

    // Negative control: the same points as an OPEN curve do not wrap at all —
    // the end is point 7 and stays there.
    const Curve open(cps, false);
    CHECK(open.position(8.0f) == cps.back());
    CHECK(open.spanCount() == 7);
}

TEST_CASE("The closed flag needs three points to mean anything")
{
    const Curve two({ { 0.0f, 0.0f, 0.0f }, { 3.0f, 4.0f, 0.0f } }, true);
    CHECK_FALSE(two.closed());              // the line between them, not a loop
    CHECK(two.spanCount() == 1);
    CHECK(two.length() == doctest::Approx(5.0f).epsilon(1e-4));

    const Curve three({ { 0.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 4.0f } }, true);
    CHECK(three.closed());
    CHECK(three.spanCount() == 3);
}

TEST_CASE("Fewer than two control points have no extent and no NaN")
{
    const Curve none(std::vector<glm::vec3>{}, false);
    CHECK(none.spanCount() == 0);
    CHECK(none.length() == 0.0f);
    CHECK(finite(none.position(0.5f)));
    CHECK(finite(none.tangent(0.5f)));
    CHECK(none.polyline(0.1f).empty());
    CHECK(none.sample(8).empty());

    const glm::vec3 only(1.0f, 2.0f, 3.0f);
    const Curve one({ only }, true);
    CHECK_FALSE(one.closed());
    CHECK(one.spanCount() == 0);
    CHECK(one.length() == 0.0f);
    CHECK(one.position(0.0f) == only);
    CHECK(one.position(7.0f) == only);
    CHECK(one.positionAtLength(5.0f) == only);
    CHECK(finite(one.tangent(0.0f)));
    REQUIRE(one.polyline(0.1f).size() == 1);
    CHECK(one.polyline(0.1f)[0] == only);
}

TEST_CASE("A non-finite parameter or length reads as zero instead of poisoning the result")
{
    const Curve c(wanderingCurve(), false);
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(c.position(nan) == c.position(0.0f));
    CHECK(c.position(inf) == c.position(0.0f));
    CHECK(finite(c.tangent(nan)));
    CHECK(c.paramAtLength(nan) == 0.0f);

    const Curve loop(circlePoints(6, 2.0f), true);
    CHECK(finite(loop.position(nan)));
    CHECK(finite(loop.position(inf)));
}

// ── Tangents ─────────────────────────────────────────────────────────────────

TEST_CASE("The tangent is the unit direction the curve actually moves in")
{
    // Against the numerical derivative of position(), on a curve that bends in
    // all three axes and on a closed one, through control points and between them.
    for (const bool closed : { false, true })
    {
        const Curve c(closed ? circlePoints(7, 3.0f) : wanderingCurve(), closed);
        const float end = c.maxParam();
        for (int i = 0; i <= 24; ++i)
        {
            const float t = end * static_cast<float>(i) / 24.0f;
            const glm::vec3 tan = c.tangent(t);
            INFO("closed=", closed, " t=", t);
            CHECK(glm::length(tan) == doctest::Approx(1.0f).epsilon(1e-4));

            const float h = 2e-3f;
            const glm::vec3 fd = glm::normalize(c.position(t + h) - c.position(t - h));
            // An open curve clamps at its ends, so there the difference is one-sided;
            // still the same direction.
            CHECK(glm::dot(tan, fd) > 0.995f);
        }
    }
}

TEST_CASE("A circle's tangent is perpendicular to its radius")
{
    const Curve c(circlePoints(12, 5.0f), true);
    for (int i = 0; i < 40; ++i)
    {
        const float t = c.maxParam() * static_cast<float>(i) / 40.0f;
        const glm::vec3 p = c.position(t);
        const glm::vec3 radial = glm::normalize(glm::vec3(p.x, 0.0f, p.z));
        INFO("t=", t);
        CHECK(std::fabs(glm::dot(c.tangent(t), radial)) < 0.02f);
        // The points run from +X towards +Z, which seen from above (+Y) is
        // clockwise: cross(X, Z) = -Y, so the tangent turns the radial the
        // negative way round. A curve walked backwards would flip this sign.
        CHECK(glm::cross(radial, c.tangent(t)).y < -0.9f);
    }
}

TEST_CASE("Coincident control points leave a finite curve and a finite tangent")
{
    // A point dropped twice in the editor is the common way to get a span with
    // no direction of its own.
    const Curve dup({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                      { 1.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f } }, false);
    CHECK(dup.length() == doctest::Approx(2.0f).epsilon(1e-3));
    for (int i = 0; i <= 12; ++i)
    {
        const float t = dup.maxParam() * static_cast<float>(i) / 12.0f;
        CHECK(finite(dup.position(t)));
        const glm::vec3 tan = dup.tangent(t);
        CHECK(finite(tan));
        CHECK(glm::length(tan) == doctest::Approx(1.0f).epsilon(1e-4));
    }
    for (const glm::vec3& p : dup.polyline(0.01f)) CHECK(finite(p));

    // Every point in one place: a curve with no length and no direction, not a NaN.
    const glm::vec3 spot(2.0f, 3.0f, 4.0f);
    const Curve all({ spot, spot, spot }, true);
    CHECK(all.length() == 0.0f);
    CHECK(all.position(1.5f) == spot);
    CHECK(all.positionAtLength(10.0f) == spot);
    CHECK(all.tangent(0.5f) == glm::vec3(0.0f, 0.0f, 1.0f));
}

// ── Sampling by arc length ───────────────────────────────────────────────────

TEST_CASE("The length of a circle is about 2 pi r")
{
    for (const float r : { 1.0f, 5.0f, 40.0f })
    {
        for (const int n : { 8, 16, 32 })
        {
            const Curve c(circlePoints(n, r), true);
            const float expected = kTwoPi * r;
            INFO("r=", r, " n=", n);
            // 8 points on a circle is a coarse spline; even so it is within 1 %.
            CHECK(c.length() == doctest::Approx(expected).epsilon(n >= 16 ? 0.002 : 0.01));
        }
    }

    // Negative control: the SAME points as an open curve stop one span short, so
    // a closing span that was not counted would pass the checks above only by
    // being close enough — it is 1/16th of the circle here, far outside 0.2 %.
    const Curve open(circlePoints(16, 5.0f), false);
    const Curve closed(circlePoints(16, 5.0f), true);
    CHECK(open.length() < closed.length() * 0.97f);
    CHECK(open.length() == doctest::Approx(kTwoPi * 5.0f * 15.0f / 16.0f).epsilon(0.01));
}

TEST_CASE("The length of a straight line is right however unevenly it is spaced")
{
    const Curve c({ { 0.0f, 0.0f, 0.0f }, { 10.0f, 0.0f, 0.0f }, { 30.0f, 0.0f, 0.0f } }, false);
    CHECK(c.length() == doctest::Approx(30.0f).epsilon(1e-4));

    // Linear in distance, across the border between the two spans. The position
    // is interpolated inside a table chord (1/32 of a span), so it is good to a
    // fraction of a millimetre per metre here, not to the last float bit; a
    // wrong chord would be off by decimetres.
    for (const float s : { 0.0f, 1.0f, 7.5f, 10.0f, 12.0f, 25.0f, 30.0f })
    {
        INFO("s=", s);
        checkVec3(c.positionAtLength(s), { s, 0.0f, 0.0f }, 1e-3f);
        checkVec3(c.tangentAtLength(s), { 1.0f, 0.0f, 0.0f }, 1e-4f);
    }

    // Open curves clamp.
    CHECK(c.positionAtLength(-5.0f) == c.position(0.0f));
    CHECK(c.positionAtLength(100.0f) == c.position(c.maxParam()));
}

TEST_CASE("Equal steps in arc length are equal distances; equal steps in t are not")
{
    // Control points spaced very unevenly, so the parameter runs fast over the
    // long span and slowly over the short ones.
    const std::vector<glm::vec3> cps = {
        { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.0f, 0.2f }, { 1.0f, 0.0f, 0.0f },
        { 9.0f, 0.0f, 1.0f }, { 9.5f, 0.0f, 0.0f } };
    const Curve c(cps, false);
    const float total = c.length();
    REQUIRE(total > 5.0f);

    const int steps = 60;
    const float ds = total / static_cast<float>(steps);
    float minChord = 1e30f, maxChord = 0.0f;
    glm::vec3 prev = c.positionAtLength(0.0f);
    for (int i = 1; i <= steps; ++i)
    {
        const glm::vec3 cur = c.positionAtLength(ds * static_cast<float>(i));
        const float chord = glm::length(cur - prev);
        minChord = std::min(minChord, chord);
        maxChord = std::max(maxChord, chord);
        prev = cur;
    }
    // A chord is never longer than its arc, and on this curve not much shorter.
    CHECK(maxChord <= ds * 1.01f);
    CHECK(minChord >= ds * 0.90f);

    // The same number of steps in t: wildly uneven.
    float tMin = 1e30f, tMax = 0.0f;
    prev = c.position(0.0f);
    for (int i = 1; i <= steps; ++i)
    {
        const glm::vec3 cur = c.position(c.maxParam() * static_cast<float>(i) / static_cast<float>(steps));
        const float chord = glm::length(cur - prev);
        tMin = std::min(tMin, chord);
        tMax = std::max(tMax, chord);
        prev = cur;
    }
    CHECK(tMax > tMin * 3.0f);
}

TEST_CASE("Walking a closed spline by distance wraps around")
{
    const float r = 3.0f;
    const Curve c(circlePoints(16, r), true);
    const float L = c.length();

    checkVec3(c.positionAtLength(0.0f), c.positionAtLength(L), 1e-4f);
    checkVec3(c.positionAtLength(0.37f * L), c.positionAtLength(1.37f * L), 1e-3f);
    checkVec3(c.positionAtLength(-0.25f * L), c.positionAtLength(0.75f * L), 1e-3f);

    // Half way round is the other side of the circle.
    const glm::vec3 start = c.positionAtLength(0.0f);
    const glm::vec3 half  = c.positionAtLength(0.5f * L);
    CHECK(glm::length(half - start) == doctest::Approx(2.0f * r).epsilon(0.01));
    // A quarter of the way is a quarter turn: chord = r * sqrt(2).
    CHECK(glm::length(c.positionAtLength(0.25f * L) - start) == doctest::Approx(r * 1.41421356f).epsilon(0.01));
}

TEST_CASE("paramAtLength is monotonic and spans the whole curve")
{
    for (const bool closed : { false, true })
    {
        const Curve c(closed ? circlePoints(9, 2.0f) : wanderingCurve(), closed);
        const float L = c.length();
        CHECK(c.paramAtLength(0.0f) == doctest::Approx(0.0f).epsilon(1e-6));
        if (!closed) CHECK(c.paramAtLength(L) == doctest::Approx(c.maxParam()).epsilon(1e-6));

        float last = -1.0f;
        for (int i = 0; i <= 200; ++i)
        {
            const float t = c.paramAtLength(L * static_cast<float>(i) / 200.0f * (closed ? 0.9999f : 1.0f));
            INFO("closed=", closed, " i=", i);
            CHECK(t >= last);
            last = t;
        }
        CHECK(last == doctest::Approx(c.maxParam()).epsilon(0.01));
    }
}

// ── Polylines ────────────────────────────────────────────────────────────────

TEST_CASE("sample() on a closed spline ends exactly where it began")
{
    const std::vector<glm::vec3> cps = circlePoints(6, 2.0f);
    const Curve c(cps, true);
    const std::vector<glm::vec3> s = c.sample(5);

    REQUIRE(s.size() == 6u * 5u + 1u);
    CHECK(s.front() == s.back());
    for (size_t i = 0; i < cps.size(); ++i)
        CHECK(s[i * 5] == cps[i]);
}

TEST_CASE("The polyline stays within the tolerance of the curve")
{
    const Curve c(wanderingCurve(), false);
    const std::vector<glm::vec3> reference = c.sample(400);

    size_t previousCount = 0;
    for (const float tol : { 0.25f, 0.05f, 0.01f, 0.002f })
    {
        const std::vector<glm::vec3> poly = c.polyline(tol);
        INFO("tolerance ", tol, " -> ", poly.size(), " points");
        REQUIRE(poly.size() >= 2);

        // Through the ends, and every control point.
        CHECK(poly.front() == c.position(0.0f));
        CHECK(poly.back()  == c.position(c.maxParam()));
        for (int k = 0; k <= c.spanCount(); ++k)
        {
            const glm::vec3 cp = c.position(static_cast<float>(k));
            bool found = false;
            for (const glm::vec3& p : poly) found = found || (p == cp);
            CHECK_MESSAGE(found, "control point ", k, " is not a vertex of the polyline");
        }

        // The guarantee is sampled at three points per segment, so allow a
        // little slack between them; what must hold is that it is the right
        // order of magnitude, not 5x off.
        float worst = 0.0f;
        for (const glm::vec3& p : reference) worst = std::max(worst, distanceToPolyline(p, poly));
        CHECK(worst <= tol * 1.5f);

        // Tighter tolerance, more points.
        CHECK(poly.size() > previousCount);
        previousCount = poly.size();
    }
}

TEST_CASE("A straight stretch costs one segment however long it is")
{
    const Curve line({ { 0.0f, 0.0f, 0.0f }, { 50.0f, 0.0f, 0.0f }, { 120.0f, 0.0f, 0.0f } }, false);
    const std::vector<glm::vec3> poly = line.polyline(0.001f);
    REQUIRE(poly.size() == 3);   // start, the middle control point, end
    CHECK(poly[1] == glm::vec3(50.0f, 0.0f, 0.0f));
}

TEST_CASE("A closed polyline ends where it began and encloses the right area")
{
    const float r = 4.0f;
    const Curve c(circlePoints(16, r), true);
    const std::vector<glm::vec3> ring = c.polyline(0.005f);

    REQUIRE(ring.size() > 16);
    CHECK(ring.front() == ring.back());

    // Shoelace in the XZ plane. The sign depends on the winding, so compare
    // magnitudes.
    double twiceArea = 0.0;
    for (size_t i = 0; i + 1 < ring.size(); ++i)
        twiceArea += static_cast<double>(ring[i].x) * ring[i + 1].z - static_cast<double>(ring[i + 1].x) * ring[i].z;
    const double area = std::fabs(twiceArea) * 0.5;
    CHECK(area == doctest::Approx(3.14159265 * r * r).epsilon(0.01));

    // The summed segments are the curve's length, to within the polyline's own
    // (inscribed, so slightly short) error.
    CHECK(polylineLength(ring) == doctest::Approx(c.length()).epsilon(0.005));
    CHECK(polylineLength(ring) <= c.length() * 1.001f);
}

TEST_CASE("A tolerance of zero terminates, and a negative one is the same")
{
    const Curve c(wanderingCurve(), false);
    const std::vector<glm::vec3> zero = c.polyline(0.0f);
    const std::vector<glm::vec3> neg  = c.polyline(-1.0f);

    // Capped at 4096 segments per span.
    CHECK(zero.size() <= static_cast<size_t>(c.spanCount()) * 4096u + 1u);
    CHECK(zero.size() > 100);
    CHECK(neg.size() == zero.size());
    for (const glm::vec3& p : zero) CHECK(finite(p));
}

// ── Scene format ─────────────────────────────────────────────────────────────

namespace
{
    // Four splines that differ in point count, so they can be told apart after a
    // load: an open one, a closed one, a closed flag on too few points (the flag
    // must survive even though Curve ignores it), and an empty one.
    void populateSplines(HorizonWorld& world)
    {
        auto& reg = world.registry();
        {
            const Entity e = world.createEntity("Path");
            SplineComponent s;
            s.controlPoints = wanderingCurve();
            reg.emplace<SplineComponent>(e, s);
        }
        {
            const Entity e = world.createEntity("Shore");
            SplineComponent s;
            s.controlPoints = circlePoints(8, 6.0f);
            s.closed        = true;
            reg.emplace<SplineComponent>(e, s);
        }
        {
            const Entity e = world.createEntity("Stub");
            SplineComponent s;
            s.controlPoints = { { 1.0f, 2.0f, 3.0f }, { -4.5f, 0.25f, 1.0e-3f } };
            s.closed        = true;
            reg.emplace<SplineComponent>(e, s);
        }
        {
            const Entity e = world.createEntity("Empty");
            reg.emplace<SplineComponent>(e, SplineComponent{});
        }
    }
}

TEST_CASE("Splines survive a scene round trip, in JSON and in binary")
{
    HorizonWorld world;
    populateSplines(world);

    for (const SerializeFormat format : { SerializeFormat::JSON, SerializeFormat::Binary })
    {
        const fs::path file = fs::temp_directory_path() /
            (format == SerializeFormat::JSON ? "he_test_spline.hescene" : "he_test_spline.hebin");
        SceneSerializer ser;
        REQUIRE(ser.save(world, file, format));

        HorizonWorld loaded;
        REQUIRE(ser.load(loaded, file, format));
        he_test::removeQuiet(file);

        INFO("format ", format == SerializeFormat::JSON ? "JSON" : "Binary");
        CHECK(loaded.registry().view<SplineComponent>().size() == 4);
        for (const size_t n : { size_t(5), size_t(8), size_t(2), size_t(0) })
        {
            const SplineComponent* before = splineWithPoints(world, n);
            const SplineComponent* after  = splineWithPoints(loaded, n);
            REQUIRE(before != nullptr);
            REQUIRE_MESSAGE(after != nullptr, "no spline with ", n, " points after loading");
            checkSameSpline(*before, *after);
        }
        // The flag as authored, on the two-point spline where it has no effect.
        CHECK(splineWithPoints(loaded, 2)->closed);
        CHECK_FALSE(splineWithPoints(loaded, 5)->closed);
    }
}

TEST_CASE("Splines survive the undo snapshot")
{
    HorizonWorld world;
    populateSplines(world);

    SceneSerializer ser;
    std::vector<uint8_t> snapshot;
    REQUIRE(ser.saveToMemory(world, snapshot));

    HorizonWorld restored;
    REQUIRE(ser.loadFromMemory(restored, snapshot));
    for (const size_t n : { size_t(5), size_t(8), size_t(2), size_t(0) })
    {
        const SplineComponent* before = splineWithPoints(world, n);
        const SplineComponent* after  = splineWithPoints(restored, n);
        REQUIRE(before != nullptr);
        REQUIRE(after  != nullptr);
        checkSameSpline(*before, *after);
    }
}

TEST_CASE("Duplicating or copying an entity copies its spline, independently")
{
    // Duplicate (Ctrl+D) and Copy/Paste both go through serializeSubtree and
    // instantiatePrefab (EditorApplication::duplicateSelectedEntity,
    // copySelectedEntity).
    HorizonWorld world;
    auto& reg = world.registry();
    SceneSerializer ser;

    const Entity parent = world.createEntity("Lake");
    const Entity child  = world.createEntity("Shore");
    REQUIRE(world.reparentEntity(child, parent));
    SplineComponent authored;
    authored.controlPoints = circlePoints(5, 3.0f);
    authored.closed        = true;
    reg.emplace<SplineComponent>(child, authored);

    const std::vector<uint8_t> blob = ser.serializeSubtree(world, parent);
    REQUIRE_FALSE(blob.empty());
    const Entity copy = ser.instantiatePrefab(world, blob, entt::null);
    REQUIRE((copy != entt::null));

    // The copy has a spline child of its own, carrying the same shape.
    const SplineComponent* copied = nullptr;
    Entity copiedOwner = entt::null;
    for (auto [e, s] : reg.view<SplineComponent>().each())
        if (e != child) { copied = &s; copiedOwner = e; }
    REQUIRE(copied != nullptr);
    REQUIRE(reg.view<SplineComponent>().size() == 2);
    checkSameSpline(authored, *copied);

    // Independent data, not a shared reference: editing the copy leaves the
    // original alone.
    reg.get<SplineComponent>(copiedOwner).controlPoints.push_back({ 9.0f, 9.0f, 9.0f });
    reg.get<SplineComponent>(copiedOwner).closed = false;
    checkSameSpline(authored, reg.get<SplineComponent>(child));
}

TEST_CASE("A spline is a single-entity patch, so collaboration and undo can move it")
{
    HorizonWorld world;
    auto& reg = world.registry();
    SceneSerializer ser;

    const Entity src = world.createEntity("Src");
    SplineComponent authored;
    authored.controlPoints = wanderingCurve();
    authored.closed        = true;
    reg.emplace<SplineComponent>(src, authored);

    const Entity dst = world.createEntity("Dst");
    REQUIRE(ser.applyEntityComponents(world, dst, ser.serializeEntityComponents(world, src)));
    REQUIRE(reg.all_of<SplineComponent>(dst));
    checkSameSpline(authored, reg.get<SplineComponent>(dst));
}

TEST_CASE("The Details panel's Copy and Paste carry a spline as text")
{
    HorizonWorld world;
    auto& reg = world.registry();
    SceneSerializer ser;

    const Entity src = world.createEntity("Src");
    SplineComponent authored;
    authored.controlPoints = circlePoints(6, 2.5f);
    authored.closed        = true;
    reg.emplace<SplineComponent>(src, authored);

    const std::string text = ser.exportComponentText(world, src, "spline");
    REQUIRE_FALSE(text.empty());
    CHECK(SceneSerializer::componentKeyOfText(text) == "spline");

    const Entity dst = world.createEntity("Dst");
    REQUIRE_FALSE(reg.all_of<SplineComponent>(dst));
    CHECK(ser.importComponentText(world, dst, text));
    REQUIRE(reg.all_of<SplineComponent>(dst));
    checkSameSpline(authored, reg.get<SplineComponent>(dst));
}

TEST_CASE("A spline block with missing keys loads as the defaults")
{
    HorizonWorld world;
    SceneSerializer ser;
    const Entity e = world.createEntity("Sparse");

    // A file from before "closed" existed, or hand-edited: only what is written
    // is set, the rest is what a fresh component has.
    nlohmann::json patch;
    patch["spline"] = { { "points", nlohmann::json::array({ nlohmann::json::array({ 1, 2, 3 }) }) } };
    REQUIRE(ser.applyEntityComponents(world, e, nlohmann::json::to_cbor(patch)));
    const SplineComponent* s = world.registry().try_get<SplineComponent>(e);
    REQUIRE(s != nullptr);
    CHECK_FALSE(s->closed);
    REQUIRE(s->controlPoints.size() == 1);
    checkVec3(s->controlPoints[0], { 1.0f, 2.0f, 3.0f });

    nlohmann::json empty;
    empty["spline"] = nlohmann::json::object();
    REQUIRE(ser.applyEntityComponents(world, e, nlohmann::json::to_cbor(empty)));
    CHECK(world.registry().get<SplineComponent>(e).controlPoints.empty());
    CHECK_FALSE(world.registry().get<SplineComponent>(e).closed);
}

TEST_CASE("The spline key is registered everywhere a component key has to be")
{
    // The hand-kept tables, none of which the compiler ties to the save path:
    // the loader's known keys, the remove and reset table, and the Details
    // label table. (kComponentScopes needs no entry until a help row exists.)
    CHECK(SceneSerializer::isKnownComponentKey("spline"));

    const char* key = InspectorPanel::componentKeyForLabel("Spline");
    REQUIRE(key != nullptr);
    CHECK(std::strcmp(key, "spline") == 0);
    const char* label = InspectorPanel::componentLabelForKey("spline");
    REQUIRE(label != nullptr);
    CHECK(std::strcmp(label, "Spline") == 0);

    HorizonWorld world;
    auto& reg = world.registry();
    const Entity e = world.createEntity("Tagged");
    SplineComponent authored;
    authored.controlPoints = circlePoints(4, 1.0f);
    authored.closed        = true;
    reg.emplace<SplineComponent>(e, authored);

    // Reset to default puts back a fresh component: no points, open.
    REQUIRE(SceneSerializer::resetComponentByKey(world, e, "spline"));
    REQUIRE(reg.all_of<SplineComponent>(e));
    CHECK(reg.get<SplineComponent>(e).controlPoints.empty());
    CHECK_FALSE(reg.get<SplineComponent>(e).closed);

    // Remove takes it away, once.
    CHECK(SceneSerializer::removeComponentByKey(world, e, "spline"));
    CHECK_FALSE(reg.all_of<SplineComponent>(e));
    CHECK_FALSE(SceneSerializer::removeComponentByKey(world, e, "spline"));

    // And a reset never adds one the entity does not carry.
    CHECK_FALSE(SceneSerializer::resetComponentByKey(world, e, "spline"));
    CHECK_FALSE(reg.all_of<SplineComponent>(e));
}
