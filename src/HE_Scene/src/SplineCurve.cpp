#include "HorizonScene/SplineCurve.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace
{
    // Halvings per span before polyline() stops refining, whatever the tolerance
    // says. 2^12 segments is far past anything a screen can show; the cap is
    // what makes a tolerance of zero (or float noise at a tolerance of 1e-7)
    // slow and large instead of endless.
    constexpr int kMaxPolylineDepth = 12;

    inline float len2(const glm::vec3& v) { return glm::dot(v, v); }

    // Distance from p to the SEGMENT ab, not to the infinite line through it: a
    // curve that overshoots past an end of its chord is not near the chord there.
    float distanceToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
    {
        const glm::vec3 ab = b - a;
        const float l2 = len2(ab);
        const float f  = (l2 > 0.0f) ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + ab * f));
    }

    // Append the points of span[ua, ub] after `pa` (which the caller already
    // holds), ending with `pb`. The segment is accepted when the curve at a
    // quarter, a half and three quarters of it lies within `tol` of the chord;
    // the middle point alone would accept an S-shaped stretch that crosses its
    // own chord exactly at the midpoint.
    void refineSpan(const HE::spline::CatmullRomSpan& span, float ua, float ub,
                    const glm::vec3& pa, const glm::vec3& pb, float tol, int depth,
                    std::vector<glm::vec3>& out)
    {
        bool flat = true;
        if (depth < kMaxPolylineDepth)
        {
            for (const float f : { 0.25f, 0.5f, 0.75f })
            {
                if (distanceToSegment(span.at(glm::mix(ua, ub, f)), pa, pb) > tol)
                {
                    flat = false;
                    break;
                }
            }
        }
        if (flat) { out.push_back(pb); return; }

        const float     um = 0.5f * (ua + ub);
        const glm::vec3 pm = span.at(um);
        refineSpan(span, ua, um, pa, pm, tol, depth + 1, out);
        refineSpan(span, um, ub, pm, pb, tol, depth + 1, out);
    }
}

namespace HE::spline
{

Curve::Curve(std::vector<glm::vec3> controlPoints, bool closed)
    : m_points(std::move(controlPoints))
{
    const size_t n = m_points.size();
    m_closed = closed && n >= 3;
    if (n < 2) return;

    const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(n);
    const size_t spans = m_closed ? n : n - 1;

    // The neighbours of a span: wrapped around when closed, mirrored phantom
    // points past the ends when open (the same ones sampleCatmullRom uses), so
    // the first and last span are defined by four points like every other.
    auto cp = [&](std::ptrdiff_t i) -> glm::vec3
    {
        if (m_closed) return m_points[static_cast<size_t>(((i % count) + count) % count)];
        if (i < 0)      return m_points[0] + (m_points[0] - m_points[1]);
        if (i >= count) return m_points[n - 1] + (m_points[n - 1] - m_points[n - 2]);
        return m_points[static_cast<size_t>(i)];
    };

    m_spans.reserve(spans);
    for (size_t s = 0; s < spans; ++s)
    {
        const std::ptrdiff_t i = static_cast<std::ptrdiff_t>(s);
        m_spans.emplace_back(cp(i - 1), cp(i), cp(i + 1), cp(i + 2));
    }

    // Arc-length table: the chord length up to each of kLutPerSpan evenly spaced
    // steps in t per span. Summed in double — a long closed loop adds thousands
    // of small chords, and the float error of that sum would otherwise be the
    // largest error in the table.
    m_cumulative.reserve(spans * static_cast<size_t>(kLutPerSpan) + 1);
    m_cumulative.push_back(0.0f);
    double acc = 0.0;
    for (const CatmullRomSpan& span : m_spans)
    {
        glm::vec3 prev = span.p1;
        for (int k = 1; k <= kLutPerSpan; ++k)
        {
            // The last chord ends on the control point itself, not on at(1)'s
            // rounding of it, so the table has no seam at span borders.
            const glm::vec3 cur = (k == kLutPerSpan)
                ? span.p2
                : span.at(static_cast<float>(k) / static_cast<float>(kLutPerSpan));
            acc += static_cast<double>(glm::length(cur - prev));
            m_cumulative.push_back(static_cast<float>(acc));
            prev = cur;
        }
    }
}

void Curve::locate(float t, int& span, float& u) const
{
    const int   count = static_cast<int>(m_spans.size());
    const float maxT  = static_cast<float>(count);

    if (!std::isfinite(t)) t = 0.0f;
    if (m_closed)
    {
        t = std::fmod(t, maxT);
        if (t < 0.0f) t += maxT;
    }
    else
    {
        t = std::clamp(t, 0.0f, maxT);
    }

    // t == maxT (the end of an open curve, or a closed wrap that rounded up to
    // it) lands past the last span: it is that span's far end, u = 1.
    int i = static_cast<int>(std::floor(t));
    if (i >= count) i = count - 1;
    if (i < 0)      i = 0;
    span = i;
    u    = std::clamp(t - static_cast<float>(i), 0.0f, 1.0f);
}

glm::vec3 Curve::position(float t) const
{
    if (m_spans.empty()) return m_points.empty() ? glm::vec3(0.0f) : m_points.front();

    int span; float u;
    locate(t, span, u);
    const CatmullRomSpan& s = m_spans[static_cast<size_t>(span)];
    // The ends of a span are control points: hand those back exactly rather than
    // through the recursion's rounding, so t = k really is point k.
    if (u <= 0.0f) return s.p1;
    if (u >= 1.0f) return s.p2;
    return s.at(u);
}

glm::vec3 Curve::tangent(float t) const
{
    const glm::vec3 fallback(0.0f, 0.0f, 1.0f);
    if (m_spans.empty()) return fallback;

    int span; float u;
    locate(t, span, u);
    const CatmullRomSpan& s = m_spans[static_cast<size_t>(span)];

    const glm::vec3 d = s.derivative(u);
    if (len2(d) > 1e-12f) return glm::normalize(d);

    // No direction of its own (a span between coincident points): borrow the
    // way the neighbours around it are heading.
    const glm::vec3 chord = s.p3 - s.p0;
    if (len2(chord) > 1e-12f) return glm::normalize(chord);
    return fallback;
}

float Curve::paramAtLength(float s) const
{
    const float total = length();
    if (m_spans.empty() || total <= 1e-6f) return 0.0f;

    if (!std::isfinite(s)) s = 0.0f;
    if (m_closed)
    {
        s = std::fmod(s, total);
        if (s < 0.0f) s += total;
    }
    else
    {
        s = std::clamp(s, 0.0f, total);
    }

    // First table entry beyond s; the chord before it holds s. upper_bound (not
    // lower_bound) so a run of equal entries — a stretch of coincident points —
    // is stepped over rather than divided by.
    const auto it = std::upper_bound(m_cumulative.begin(), m_cumulative.end(), s);
    size_t hi = static_cast<size_t>(it - m_cumulative.begin());
    if (hi == 0) hi = 1;
    if (hi >= m_cumulative.size()) hi = m_cumulative.size() - 1;

    const float a = m_cumulative[hi - 1];
    const float b = m_cumulative[hi];
    const float f = (b > a) ? glm::clamp((s - a) / (b - a), 0.0f, 1.0f) : 0.0f;
    return (static_cast<float>(hi - 1) + f) / static_cast<float>(kLutPerSpan);
}

std::vector<glm::vec3> Curve::sample(int samplesPerSpan) const
{
    if (m_spans.empty()) return m_points;

    const int steps = std::max(1, samplesPerSpan);
    std::vector<glm::vec3> out;
    out.reserve(m_spans.size() * static_cast<size_t>(steps) + 1);
    out.push_back(m_points.front());
    for (const CatmullRomSpan& span : m_spans)
    {
        for (int k = 1; k <= steps; ++k)
        {
            // Exact at the span's far end, so a closed curve really ends on the
            // point it started from.
            out.push_back(k == steps ? span.p2
                                     : span.at(static_cast<float>(k) / static_cast<float>(steps)));
        }
    }
    return out;
}

std::vector<glm::vec3> Curve::polyline(float tolerance) const
{
    if (m_spans.empty()) return m_points;

    const float tol = std::max(tolerance, 0.0f);
    std::vector<glm::vec3> out;
    out.push_back(m_points.front());
    for (const CatmullRomSpan& span : m_spans)
        refineSpan(span, 0.0f, 1.0f, span.p1, span.p2, tol, 0, out);
    return out;
}

}  // namespace HE::spline
