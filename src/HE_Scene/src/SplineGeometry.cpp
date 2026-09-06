#include "HorizonScene/SplineGeometry.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kEps = 1e-6f;

    // Squared length without glm::length's sqrt — used everywhere a comparison
    // against an epsilon is all that is wanted.
    inline float len2(const glm::vec3& v) { return glm::dot(v, v); }

    inline glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback)
    {
        const float l2 = len2(v);
        return (l2 > kEps * kEps) ? v * (1.0f / std::sqrt(l2)) : fallback;
    }

    // Per-point tangents of a polyline: central differences inside, one-sided at
    // the ends. A pair of coincident points would give a zero tangent, so those
    // reuse the previous one instead of handing a NaN to the frame walk.
    std::vector<glm::vec3> polylineTangents(const std::vector<glm::vec3>& p)
    {
        const size_t n = p.size();
        std::vector<glm::vec3> t(n, glm::vec3(0.0f, 0.0f, 1.0f));
        if (n < 2) return t;

        glm::vec3 last(0.0f, 0.0f, 1.0f);
        for (size_t i = 0; i < n; ++i)
        {
            glm::vec3 d;
            if      (i == 0)     d = p[1] - p[0];
            else if (i == n - 1) d = p[n - 1] - p[n - 2];
            else                 d = p[i + 1] - p[i - 1];
            last = safeNormalize(d, last);
            t[i] = last;
        }
        // The first tangent was computed before `last` held anything meaningful;
        // if p[0] and p[1] coincide it is still the placeholder, so take the
        // first tangent that is real.
        if (len2(p[1] - p[0]) <= kEps * kEps)
            for (size_t i = 1; i < n; ++i)
                if (len2(p[i] - p[i - 1]) > kEps * kEps) { t[0] = t[i]; break; }
        return t;
    }

    // A unit vector perpendicular to `t`, seeded from `hint`. Falls back to the
    // world axis least parallel to the tangent when the hint is unusable — which
    // is what makes a perfectly vertical rope with the default (0,1,0) hint work
    // without the caller knowing anything about it.
    glm::vec3 perpendicularTo(const glm::vec3& t, const glm::vec3& hint)
    {
        glm::vec3 h = hint;
        if (len2(h) <= kEps * kEps) h = glm::vec3(0.0f, 1.0f, 0.0f);
        h = glm::normalize(h);
        glm::vec3 n = h - t * glm::dot(t, h);
        if (len2(n) <= 1e-8f)
        {
            const float ax = std::fabs(t.x), ay = std::fabs(t.y), az = std::fabs(t.z);
            const glm::vec3 axis = (ax <= ay && ax <= az) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                 : (ay <= az)             ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                          : glm::vec3(0.0f, 0.0f, 1.0f);
            n = axis - t * glm::dot(t, axis);
        }
        return glm::normalize(n);
    }
}

namespace HE::spline
{

std::vector<glm::vec3> sampleCatmullRom(const std::vector<glm::vec3>& controlPoints,
                                        int samplesPerSpan)
{
    const size_t n = controlPoints.size();
    if (n < 2) return controlPoints;

    const int steps = std::max(1, samplesPerSpan);

    // Mirrored phantom points so the first and last span are defined by four
    // points like every other span.
    auto cp = [&](std::ptrdiff_t i) -> glm::vec3
    {
        if (i < 0)  return controlPoints[0] + (controlPoints[0] - controlPoints[1]);
        if (i >= static_cast<std::ptrdiff_t>(n))
            return controlPoints[n - 1] + (controlPoints[n - 1] - controlPoints[n - 2]);
        return controlPoints[static_cast<size_t>(i)];
    };

    // Centripetal knot spacing: t_{k+1} = t_k + |p_{k+1} - p_k|^0.5.
    auto knot = [](float t, const glm::vec3& a, const glm::vec3& b)
    {
        return t + std::sqrt(std::sqrt(std::max(len2(b - a), 0.0f)));
    };

    std::vector<glm::vec3> out;
    out.reserve((n - 1) * static_cast<size_t>(steps) + 1);
    out.push_back(controlPoints[0]);

    for (size_t s = 0; s + 1 < n; ++s)
    {
        const glm::vec3 p0 = cp(static_cast<std::ptrdiff_t>(s) - 1);
        const glm::vec3 p1 = cp(static_cast<std::ptrdiff_t>(s));
        const glm::vec3 p2 = cp(static_cast<std::ptrdiff_t>(s) + 1);
        const glm::vec3 p3 = cp(static_cast<std::ptrdiff_t>(s) + 2);

        const float t0 = 0.0f;
        const float t1 = knot(t0, p0, p1);
        const float t2 = knot(t1, p1, p2);
        const float t3 = knot(t2, p2, p3);

        // Coincident control points collapse a knot interval; the Barry-Goldman
        // recursion would divide by zero, so that span is simply a straight line.
        const bool degenerate = (t1 - t0) < kEps || (t2 - t1) < kEps || (t3 - t2) < kEps;

        for (int k = 1; k <= steps; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(steps);
            if (degenerate) { out.push_back(glm::mix(p1, p2, u)); continue; }

            const float t = glm::mix(t1, t2, u);
            const glm::vec3 a1 = ((t1 - t) / (t1 - t0)) * p0 + ((t - t0) / (t1 - t0)) * p1;
            const glm::vec3 a2 = ((t2 - t) / (t2 - t1)) * p1 + ((t - t1) / (t2 - t1)) * p2;
            const glm::vec3 a3 = ((t3 - t) / (t3 - t2)) * p2 + ((t - t2) / (t3 - t2)) * p3;
            const glm::vec3 b1 = ((t2 - t) / (t2 - t0)) * a1 + ((t - t0) / (t2 - t0)) * a2;
            const glm::vec3 b2 = ((t3 - t) / (t3 - t1)) * a2 + ((t - t1) / (t3 - t1)) * a3;
            out.push_back(((t2 - t) / (t2 - t1)) * b1 + ((t - t1) / (t2 - t1)) * b2);
        }
    }
    return out;
}

std::vector<glm::vec3> resampleByArcLength(const std::vector<glm::vec3>& points, int count)
{
    if (points.size() < 2 || count < 2) return points;

    // Cumulative arc length of the input polyline.
    std::vector<float> acc(points.size(), 0.0f);
    for (size_t i = 1; i < points.size(); ++i)
        acc[i] = acc[i - 1] + glm::length(points[i] - points[i - 1]);

    const float total = acc.back();
    if (total <= kEps) return points;   // every point in the same place

    std::vector<glm::vec3> out;
    out.reserve(static_cast<size_t>(count));
    out.push_back(points.front());

    size_t seg = 0;
    for (int i = 1; i < count - 1; ++i)
    {
        const float target = total * (static_cast<float>(i) / static_cast<float>(count - 1));
        while (seg + 2 < points.size() && acc[seg + 1] < target) ++seg;
        const float span = acc[seg + 1] - acc[seg];
        const float u    = (span > kEps) ? (target - acc[seg]) / span : 0.0f;
        out.push_back(glm::mix(points[seg], points[seg + 1], u));
    }
    out.push_back(points.back());
    return out;
}

std::vector<Frame> buildFrames(const std::vector<glm::vec3>& points, const glm::vec3& upHint)
{
    std::vector<Frame> frames;
    if (points.size() < 2) return frames;

    const std::vector<glm::vec3> tangents = polylineTangents(points);
    frames.resize(points.size());

    frames[0].position  = points[0];
    frames[0].tangent   = tangents[0];
    frames[0].normal    = perpendicularTo(tangents[0], upHint);
    frames[0].binormal  = glm::cross(tangents[0], frames[0].normal);
    frames[0].arcLength = 0.0f;

    // Double-reflection parallel transport (Wang et al. 2008): reflect the
    // reference vector through the segment, then through the tangent change.
    // Two cheap reflections instead of a rotation, and no accumulation of
    // per-step normalisation error.
    for (size_t i = 1; i < points.size(); ++i)
    {
        const glm::vec3 v1 = points[i] - points[i - 1];
        const float     c1 = len2(v1);
        glm::vec3 r = frames[i - 1].normal;
        glm::vec3 t = frames[i - 1].tangent;

        if (c1 > kEps * kEps)
        {
            const glm::vec3 rL = r - (2.0f / c1) * glm::dot(v1, r) * v1;
            const glm::vec3 tL = t - (2.0f / c1) * glm::dot(v1, t) * v1;
            const glm::vec3 v2 = tangents[i] - tL;
            const float     c2 = len2(v2);
            r = (c2 > kEps * kEps) ? (rL - (2.0f / c2) * glm::dot(v2, rL) * v2) : rL;
        }

        // Re-orthogonalise against the new tangent: the reflections keep the
        // angle, floating point does not keep it exactly.
        glm::vec3 n = r - tangents[i] * glm::dot(tangents[i], r);
        n = (len2(n) > 1e-10f) ? glm::normalize(n) : perpendicularTo(tangents[i], frames[i - 1].normal);

        frames[i].position  = points[i];
        frames[i].tangent   = tangents[i];
        frames[i].normal    = n;
        frames[i].binormal  = glm::cross(tangents[i], n);
        frames[i].arcLength = frames[i - 1].arcLength + glm::length(v1);
    }
    return frames;
}

std::vector<Frame> sampleSpline(const std::vector<glm::vec3>& controlPoints,
                                int samplesPerSpan, const glm::vec3& upHint)
{
    if (controlPoints.size() < 2) return {};
    const std::vector<glm::vec3> dense = sampleCatmullRom(controlPoints, samplesPerSpan);
    const std::vector<glm::vec3> even  = resampleByArcLength(dense, static_cast<int>(dense.size()));
    return buildFrames(even, upHint);
}

MeshData buildTube(const std::vector<Frame>& frames, const TubeParams& params)
{
    MeshData mesh;
    if (frames.size() < 2) return mesh;

    const int    seg    = std::max(3, params.radialSegments);
    const int    ring   = seg + 1;                       // + seam vertex (u: 0 … 1)
    const float  radius = std::max(0.0f, params.radius);
    const float  total  = frames.back().arcLength;
    const size_t rings  = frames.size();

    mesh.positions.reserve(rings * static_cast<size_t>(ring) * 3);
    mesh.normals  .reserve(rings * static_cast<size_t>(ring) * 3);
    mesh.uvs      .reserve(rings * static_cast<size_t>(ring) * 2);
    mesh.indices  .reserve((rings - 1) * static_cast<size_t>(seg) * 6);

    for (const Frame& f : frames)
    {
        const float v = (params.uvTileLength > 0.0f)
            ? f.arcLength / params.uvTileLength
            : ((total > kEps) ? f.arcLength / total : 0.0f);

        for (int j = 0; j <= seg; ++j)
        {
            const float a  = (static_cast<float>(j) / static_cast<float>(seg)) * 6.28318530718f;
            const glm::vec3 dir = f.normal * std::cos(a) + f.binormal * std::sin(a);
            const glm::vec3 p   = f.position + dir * radius;

            mesh.positions.insert(mesh.positions.end(), { p.x, p.y, p.z });
            mesh.normals  .insert(mesh.normals  .end(), { dir.x, dir.y, dir.z });
            mesh.uvs      .insert(mesh.uvs      .end(), { static_cast<float>(j) / static_cast<float>(seg), v });
            mesh.bounds.expand(p);
        }
    }

    for (size_t i = 0; i + 1 < rings; ++i)
    {
        const uint32_t a = static_cast<uint32_t>(i * static_cast<size_t>(ring));
        const uint32_t b = static_cast<uint32_t>((i + 1) * static_cast<size_t>(ring));
        for (int j = 0; j < seg; ++j)
        {
            const uint32_t a0 = a + static_cast<uint32_t>(j), a1 = a0 + 1;
            const uint32_t b0 = b + static_cast<uint32_t>(j), b1 = b0 + 1;
            mesh.indices.insert(mesh.indices.end(), { a0, b0, b1, a0, b1, a1 });
        }
    }
    return mesh;
}

MeshData buildRibbon(const std::vector<RibbonSection>& sections, const RibbonParams& params)
{
    MeshData mesh;
    if (sections.size() < 2) return mesh;

    std::vector<glm::vec3> pts;
    pts.reserve(sections.size());
    for (const RibbonSection& s : sections) pts.push_back(s.position);

    // The frames are built even for a camera-aligned band: they are the fallback
    // for the degenerate case where the curve runs straight at the camera and
    // cross(tangent, toCamera) collapses.
    const std::vector<Frame> frames = buildFrames(pts, params.upHint);
    if (frames.size() != sections.size()) return mesh;

    const size_t n = sections.size();
    const size_t lanes = params.twoSided ? 2u : 1u;
    mesh.positions.reserve(n * 2 * lanes * 3);
    mesh.normals  .reserve(n * 2 * lanes * 3);
    mesh.uvs      .reserve(n * 2 * lanes * 2);
    mesh.indices  .reserve((n - 1) * 6 * lanes);

    std::vector<glm::vec3> width(n), normal(n);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::vec3 t = frames[i].tangent;
        glm::vec3 w = frames[i].binormal;
        if (params.cameraAligned)
        {
            const glm::vec3 toCam = params.cameraPos - sections[i].position;
            const glm::vec3 c     = glm::cross(t, toCam);
            if (len2(c) > 1e-10f) w = glm::normalize(c);
        }
        width[i]  = w;
        normal[i] = safeNormalize(glm::cross(w, t), frames[i].normal);
    }

    auto emit = [&](bool flipped)
    {
        const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
        for (size_t i = 0; i < n; ++i)
        {
            const float h = std::max(0.0f, sections[i].halfWidth);
            const glm::vec3 l = sections[i].position - width[i] * h;
            const glm::vec3 r = sections[i].position + width[i] * h;
            const glm::vec3 nrm = flipped ? -normal[i] : normal[i];
            mesh.positions.insert(mesh.positions.end(), { l.x, l.y, l.z, r.x, r.y, r.z });
            mesh.normals  .insert(mesh.normals  .end(), { nrm.x, nrm.y, nrm.z, nrm.x, nrm.y, nrm.z });
            mesh.uvs      .insert(mesh.uvs      .end(), { 0.0f, sections[i].v, 1.0f, sections[i].v });
            mesh.bounds.expand(l);
            mesh.bounds.expand(r);
        }
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const uint32_t l0 = base + static_cast<uint32_t>(i * 2), r0 = l0 + 1;
            const uint32_t l1 = l0 + 2,                              r1 = l0 + 3;
            if (flipped) mesh.indices.insert(mesh.indices.end(), { l0, r1, r0, l0, l1, r1 });
            else         mesh.indices.insert(mesh.indices.end(), { l0, r0, r1, l0, r1, l1 });
        }
    };

    emit(false);
    if (params.twoSided) emit(true);
    return mesh;
}

std::vector<float> interleave(const MeshData& mesh)
{
    const size_t n = mesh.vertexCount();
    std::vector<float> out;
    out.reserve(n * 8);
    for (size_t i = 0; i < n; ++i)
    {
        out.insert(out.end(), { mesh.positions[i * 3 + 0], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2],
                                mesh.normals  [i * 3 + 0], mesh.normals  [i * 3 + 1], mesh.normals  [i * 3 + 2],
                                mesh.uvs      [i * 2 + 0], mesh.uvs      [i * 2 + 1] });
    }
    return out;
}

}  // namespace HE::spline
