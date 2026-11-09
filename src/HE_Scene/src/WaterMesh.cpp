#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"

#include <Physics/WaterWaves.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <queue>

namespace HE::water
{

namespace
{
    bool hasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    // Points closer than this are one point, a vertex this close to the line
    // through its neighbours is not a corner. A thousandth of a cell: far below
    // anything the lattice can resolve, far above double rounding on a long edge.
    double mergeTolerance(const Lattice& l) { return 1e-3 * std::min(l.cellW, l.cellH); }

    // b adds nothing between a and c: it sits on a, or within eps of the line a–c.
    bool redundant(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, double eps)
    {
        const glm::dvec2 ab = b - a;
        if (glm::length(ab) <= eps) return true;
        const glm::dvec2 ac = c - a;
        const double len = glm::length(ac);
        if (len <= eps) return false;                   // a spike out and back: keep, it is not a straight run
        return std::abs(ac.x * ab.y - ac.y * ab.x) / len <= eps;
    }

    // Merge duplicates and drop collinear vertices. A vertex on a straight shore
    // carries no information, and a lake with a hundred-metre straight bank would
    // otherwise cost a vertex per cell. Every decision is taken against vertices
    // that are KEPT (a stack), never against ones this very pass has already
    // thrown away: two coincident crossings across the start of the ring would
    // otherwise each be judged redundant by the other and a corner would vanish.
    void simplifyRing(Ring& ring, double eps)
    {
        Ring out;
        out.reserve(ring.size());
        for (const glm::dvec2& p : ring)
        {
            while (out.size() >= 2 && redundant(out[out.size() - 2], out.back(), p, eps)) out.pop_back();
            if (!out.empty() && glm::length(p - out.back()) <= eps) continue;
            out.push_back(p);
        }
        // The seam: the first and last kept vertices were judged without each other.
        for (bool changed = true; changed && out.size() >= 3; )
        {
            changed = false;
            if (redundant(out.back(), out.front(), out[1], eps)) { out.erase(out.begin()); changed = true; }
            else if (redundant(out[out.size() - 2], out.back(), out.front(), eps)) { out.pop_back(); changed = true; }
        }
        ring.swap(out);
    }

    bool pointInRing(const Ring& ring, const glm::dvec2& p)
    {
        bool inside = false;
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
        {
            const glm::dvec2& a = ring[i];
            const glm::dvec2& b = ring[j];
            if ((a.y > p.y) != (b.y > p.y) &&
                p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
        return inside;
    }

    // One lattice cell as marching squares sees it: which of its corners are inside
    // the sheet and where along each edge the inside ends. The one rule both the
    // contours (extractContours) and the point test (insideSheet) are cut by, so
    // what a brush digs under and what the mesh covers cannot drift apart.
    struct CellCut
    {
        glm::dvec2 corner[4];   // counter-clockwise from the lower left (x right, z up)
        bool       in[4] = { false, false, false, false };
        glm::dvec2 cross[4];    // on edge e (corner e → e+1), when in[e] != in[e+1]
        int        crossings = 0;
        bool       joined = false;   // saddle (4 crossings): the wet corners meet through the middle
    };

    CellCut cutCell(const Lattice& L, int i, int j, double iso)
    {
        CellCut cut;
        const bool clip = L.clipped() && L.g.size() == L.v.size();
        // Corners counter-clockwise from the lower left (x right, z up).
        const double c[4] = { L.at(i, j), L.at(i + 1, j), L.at(i + 1, j + 1), L.at(i, j + 1) };
        // The ground's depth under the water at the same corners; 1 when the
        // lattice is not clipped, so that its sign never says "outside".
        const double d[4] = { clip ? L.groundAt(i, j)         : 1.0, clip ? L.groundAt(i + 1, j)         : 1.0,
                              clip ? L.groundAt(i + 1, j + 1) : 1.0, clip ? L.groundAt(i, j + 1)         : 1.0 };
        const bool   wet[4] = { c[0] >= iso, c[1] >= iso, c[2] >= iso, c[3] >= iso };
        const bool   dry[4] = { d[0] >= 0.0, d[1] >= 0.0, d[2] >= 0.0, d[3] >= 0.0 };   // ground under the sheet
        for (int k = 0; k < 4; ++k) cut.in[k] = wet[k] && dry[k];
        cut.corner[0] = L.position(i, j);         cut.corner[1] = L.position(i + 1, j);
        cut.corner[2] = L.position(i + 1, j + 1); cut.corner[3] = L.position(i, j + 1);
        if (cut.in[0] == cut.in[1] && cut.in[1] == cut.in[2] && cut.in[2] == cut.in[3]) return cut;

        for (int e = 0; e < 4; ++e)
        {
            const int a = e, b = (e + 1) & 3;
            if (cut.in[a] == cut.in[b]) continue;
            // Where along a→b the inside ends: the coverage reaches the iso
            // value at tCov, the ground comes up through the water at tGround.
            // Leaving the inside the first of them counts, entering it the last.
            double tCov = -1.0, tGround = -1.0;
            if (wet[a] != wet[b]) tCov = (iso - c[a]) / (c[b] - c[a]);
            if (dry[a] != dry[b]) tGround = d[a] / (d[a] - d[b]);
            double t = 0.5;                                // cannot be missing: the corners differ in one of the two
            if (cut.in[a]) { t = 2.0; if (tCov >= 0.0) t = std::min(t, tCov); if (tGround >= 0.0) t = std::min(t, tGround); }
            else           { t = -1.0; if (tCov >= 0.0) t = std::max(t, tCov); if (tGround >= 0.0) t = std::max(t, tGround); }
            t = std::clamp(t, 0.0, 1.0);
            cut.cross[e] = cut.corner[a] + (cut.corner[b] - cut.corner[a]) * t;
            ++cut.crossings;
        }
        // A saddle: two opposite corners wet. The mean of the corners says whether
        // the wet ones are joined through the middle; on a clipped lattice the
        // ground says it too: the middle must be under the sheet as well.
        cut.joined = (c[0] + c[1] + c[2] + c[3]) * 0.25 >= iso &&
                     (d[0] + d[1] + d[2] + d[3]) * 0.25 >= 0.0;
        return cut;
    }

    // Distance from p to the segment a–b.
    double segmentDistance(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
    {
        const glm::dvec2 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        return glm::length(p - (a + ab * t));
    }

    // Inside the ring or on its outline (within eps).
    bool inOrOnRing(const Ring& ring, const glm::dvec2& p, double eps)
    {
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            if (segmentDistance(p, ring[j], ring[i]) <= eps) return true;
        return pointInRing(ring, p);
    }

    uint64_t fnv(uint64_t h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
    template <typename T> uint64_t fnv(uint64_t h, const T& v) { return fnv(h, &v, sizeof(T)); }

    // ── Soft shore ───────────────────────────────────────────────────────────
    // e at one lattice corner: 1 up to the waterline (the drawn sheet's depth ≤ 0),
    // 0 from kShoreFadeDepth on. A corner nobody sampled holds the far-deep sentinel
    // and reads 0; a ground that was not a number or stands above the water reads 1.
    float cornerProximity(const Lattice& L, int i, int j, float overshoot)
    {
        i = std::clamp(i, 0, L.w - 1);
        j = std::clamp(j, 0, L.h - 1);
        const float g = L.groundAt(i, j);
        if (!(g == g)) return 1.0f;
        const float depth = g - overshoot;
        return std::clamp(1.0f - depth / HE::water::kShoreFadeDepth, 0.0f, 1.0f);
    }

    // An edge of the triangle soup by its end POINTS, not its vertex indices: the two
    // copies a hole's bridge leaves of one corner are two vertices on the same point,
    // and the triangles on either side of the bridge must split it together or the
    // sheet cracks. Ordered, so a→b and b→a are one edge.
    using EdgeKey = std::array<double, 4>;
    EdgeKey edgeKey(const glm::dvec2& a, const glm::dvec2& b)
    {
        const bool swap = b.x < a.x || (b.x == a.x && b.y < a.y);
        return swap ? EdgeKey{ b.x, b.y, a.x, a.y } : EdgeKey{ a.x, a.y, b.x, b.y };
    }
    struct EdgeHash
    {
        size_t operator()(const EdgeKey& k) const
        {
            uint64_t h = 1469598103934665603ull;
            for (double d : k)
            {
                uint64_t u;
                std::memcpy(&u, &d, sizeof(u));
                h ^= u + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            }
            return static_cast<size_t>(h);
        }
    };

    // Splits the triangles near the shore until a straight line between vertices
    // follows e. The error of an edge is how far e at its middle is from the mean of its
    // ends; of a triangle, how far e at its centre is from the mean of its corners. An
    // edge that is too coarse is split at its middle (a triangle takes the split of one,
    // two or three of its edges, neighbours always agreeing, so there is no T-junction),
    // and the passes repeat. A planar bank, whose e is exactly linear in position until
    // it saturates, costs only the splits that bring a vertex to the place where it does:
    // O(log) per edge, not a grid. Never finer than hmin, the scale e itself has (it is
    // bilinear on the lattice cells), and never past maxVertices.
    // `maxLen` > 0 (a stream): an edge longer than that is split too, whatever e does, so the sheet can follow
    // the ground it is draped on; the shore's own limit then gives way to it.
    void refineShore(const Lattice& L, float overshoot, std::vector<glm::dvec2>& P,
                     std::vector<float>& E, std::vector<uint32_t>& T, double maxLen = 0.0)
    {
        constexpr float  kTolerance   = 0.03f / HE::water::kShoreFadeDepth;   // of e, i.e. 3 cm of water depth
        constexpr int    kMaxPasses   = 24;
        constexpr size_t kMaxVertices = 400000;
        double hmin = 0.5 * std::min(L.cellW, L.cellH);
        if (maxLen > 0.0) hmin = std::min(hmin, 0.5 * maxLen);   // an edge up to maxLen stays, anything longer may split
        const double hmin2 = hmin * hmin;
        auto eAt = [&](const glm::dvec2& p) { return shoreProximityAt(L, p.x, p.y, overshoot); };

        std::unordered_map<EdgeKey, bool, EdgeHash> bad;      // verdict per edge, kept across passes
        for (int pass = 0; pass < kMaxPasses; ++pass)
        {
            std::unordered_set<EdgeKey, EdgeHash> split;
            for (size_t t = 0; t + 2 < T.size(); t += 3)
            {
                const uint32_t v[3] = { T[t], T[t + 1], T[t + 2] };
                double longest = 0.0;
                EdgeKey longestKey{};
                for (int k = 0; k < 3; ++k)
                {
                    const glm::dvec2& a = P[v[k]];
                    const glm::dvec2& b = P[v[(k + 1) % 3]];
                    const glm::dvec2 d = b - a;
                    const double len2 = glm::dot(d, d);
                    if (len2 <= hmin2 * 4.0) continue;               // halves would be below hmin
                    const EdgeKey key = edgeKey(a, b);
                    auto it = bad.find(key);
                    if (it == bad.end())
                        it = bad.emplace(key, (maxLen > 0.0 && len2 > maxLen * maxLen) ||
                                              std::abs(eAt((a + b) * 0.5) - 0.5f * (E[v[k]] + E[v[(k + 1) % 3]])) > kTolerance).first;
                    if (it->second) split.insert(key);
                    if (len2 > longest) { longest = len2; longestKey = key; }
                }
                if (longest <= 0.0) continue;
                // The inside of the triangle: a flat e between three equal corners can
                // still hide a dip, and the longest edge is the one to cut for it.
                const glm::dvec2 c = (P[v[0]] + P[v[1]] + P[v[2]]) / 3.0;
                if (std::abs(eAt(c) - (E[v[0]] + E[v[1]] + E[v[2]]) / 3.0f) > kTolerance)
                    split.insert(longestKey);
            }
            if (split.empty() || P.size() + split.size() > kMaxVertices) break;

            std::unordered_map<EdgeKey, uint32_t, EdgeHash> mid;
            mid.reserve(split.size());
            for (const EdgeKey& k : split)
            {
                const glm::dvec2 m((k[0] + k[2]) * 0.5, (k[1] + k[3]) * 0.5);
                mid.emplace(k, static_cast<uint32_t>(P.size()));
                P.push_back(m);
                E.push_back(eAt(m));
            }
            auto midOf = [&](uint32_t a, uint32_t b) -> int64_t
            {
                const auto it = mid.find(edgeKey(P[a], P[b]));
                return it == mid.end() ? -1 : static_cast<int64_t>(it->second);
            };

            std::vector<uint32_t> out;
            out.reserve(T.size() + split.size() * 6);
            auto emit = [&](uint32_t a, uint32_t b, uint32_t c) { out.push_back(a); out.push_back(b); out.push_back(c); };
            for (size_t t = 0; t + 2 < T.size(); t += 3)
            {
                uint32_t v[3] = { T[t], T[t + 1], T[t + 2] };
                int64_t  m[3] = { midOf(v[0], v[1]), midOf(v[1], v[2]), midOf(v[2], v[0]) };   // on edge k → k+1
                const int n = (m[0] >= 0) + (m[1] >= 0) + (m[2] >= 0);
                if (n == 0) { emit(v[0], v[1], v[2]); continue; }
                // Rotate so the split edges are the first ones; the winding is kept.
                int r = 0;
                if (n == 1) r = m[0] >= 0 ? 0 : (m[1] >= 0 ? 1 : 2);
                else if (n == 2) r = m[2] < 0 ? 0 : (m[0] < 0 ? 1 : 2);
                const uint32_t a = v[r], b = v[(r + 1) % 3], c = v[(r + 2) % 3];
                const int64_t  mab = m[r], mbc = m[(r + 1) % 3], mca = m[(r + 2) % 3];
                if (n == 1)
                {
                    emit(a, static_cast<uint32_t>(mab), c);
                    emit(static_cast<uint32_t>(mab), b, c);
                }
                else if (n == 2)
                {
                    // ab and bc are split: the corner at b, then the quad a–mab–mbc–c on its shorter diagonal.
                    const uint32_t p = static_cast<uint32_t>(mab), q = static_cast<uint32_t>(mbc);
                    emit(p, b, q);
                    const glm::dvec2 d1 = P[a] - P[q], d2 = P[p] - P[c];
                    if (glm::dot(d1, d1) <= glm::dot(d2, d2)) { emit(a, p, q); emit(a, q, c); }
                    else                                      { emit(a, p, c); emit(p, q, c); }
                }
                else
                {
                    const uint32_t p = static_cast<uint32_t>(mab), q = static_cast<uint32_t>(mbc), s = static_cast<uint32_t>(mca);
                    emit(a, p, s); emit(p, b, q); emit(s, q, c); emit(p, q, s);
                }
            }
            T.swap(out);
        }
    }
}

// ── Streams ──────────────────────────────────────────────────────────────────
bool joinReaches(const Body& me, const Body& other)
{
    if (me.id == other.id) return false;
    if (me.stream != other.stream) return me.stream;
    if (me.stream) return other.id > me.id;
    return other.id > me.id && std::abs(other.level - me.level) <= kJoinLevelTolerance;
}

std::unordered_map<uint16_t, uint16_t> flatGroupLeads(const TerrainComponent& tc)
{
    std::unordered_map<uint16_t, uint16_t> out;
    const Field& f = tc.water;
    if (!f.allocated() || f.bodies.size() < 2) return out;
    std::unordered_map<uint16_t, uint16_t> parent;
    for (const Body& b : f.bodies) if (!b.stream) parent[b.id] = b.id;
    if (parent.size() < 2) return out;
    auto find = [&](uint16_t a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    auto join = [&](uint16_t a, uint16_t b)
    {
        const Body* ba = f.findBody(a);
        const Body* bb = f.findBody(b);
        if (!ba || !bb || ba->stream || bb->stream) return;
        if (!(std::abs(ba->level - bb->level) <= kJoinLevelTolerance)) return;
        const uint16_t ra = find(a), rb = find(b);
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
    };
    const int res = static_cast<int>(f.res);
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x)
        {
            const size_t i = static_cast<size_t>(z) * res + x;
            if (f.owner[i] == kNoBody || f.coverage[i] < kWet) continue;
            if (x + 1 < res)
            {
                const size_t j = i + 1;
                if (f.owner[j] != kNoBody && f.owner[j] != f.owner[i] && f.coverage[j] >= kWet) join(f.owner[i], f.owner[j]);
            }
            if (z + 1 < res)
            {
                const size_t j = i + res;
                if (f.owner[j] != kNoBody && f.owner[j] != f.owner[i] && f.coverage[j] >= kWet) join(f.owner[i], f.owner[j]);
            }
        }
    for (const auto& kv : parent)
    {
        const uint16_t r = find(kv.first);
        if (r != kv.first) out[kv.first] = r;
    }
    return out;
}

uint64_t streamJoinKeyOf(const Field& f)
{
    uint64_t h = 1469598103934665603ull;
    for (const Body& b : f.bodies)
        if (!b.stream) { h = fnv(h, b.id); h = fnv(h, b.level); }
    return h;
}

glm::vec2 groundGradient(const TerrainComponent& tc, float x, float z, float step)
{
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return { 0.0f, 0.0f };
    if (!(step > 0.0f))
        step = tc.sizeX / static_cast<float>(std::max(2u, tc.resolution) - 1u);
    step = std::max(step, 1.0e-3f);
    const float hx = terrainHeightAt(tc, x + step, z) - terrainHeightAt(tc, x - step, z);
    const float hz = terrainHeightAt(tc, x, z + step) - terrainHeightAt(tc, x, z - step);
    if (!std::isfinite(hx) || !std::isfinite(hz)) return { 0.0f, 0.0f };
    return { hx / (2.0f * step), hz / (2.0f * step) };
}

glm::vec2 streamSlopeAt(const TerrainComponent& tc, float x, float z)
{
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return { 0.0f, 0.0f };
    const float r = std::max(kStreamGradientStep * 0.5f, 1.0e-3f);
    const float h0 = terrainHeightAt(tc, x, z);
    if (!std::isfinite(h0)) return { 0.0f, 0.0f };
    constexpr int kDirs = 16;
    float drop[kDirs];
    glm::vec2 dir[kDirs];
    float best = 0.0f;
    for (int k = 0; k < kDirs; ++k)
    {
        const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(kDirs);
        dir[k] = { std::cos(a), std::sin(a) };
        const float h = terrainHeightAt(tc, x + dir[k].x * r, z + dir[k].y * r);
        drop[k] = std::isfinite(h) ? (h0 - h) / r : 0.0f;
        best = std::max(best, drop[k]);
    }
    if (!(best > 0.0f)) return { 0.0f, 0.0f };
    // The direction: the steep ones, weighted toward the steepest (never a direction that goes up).
    glm::vec2 sum(0.0f);
    for (int k = 0; k < kDirs; ++k)
        if (drop[k] > 0.0f) { const float w = std::pow(drop[k] / best, 6.0f); sum += dir[k] * w; }
    const float len = glm::length(sum);
    if (!(len > 1.0e-6f)) return { 0.0f, 0.0f };
    return -(sum / len) * best;
}

glm::vec2 slopeToFlow(const glm::vec2& g)
{
    const float mag = std::sqrt(g.x * g.x + g.y * g.y);
    if (!(mag > 0.0f) || !std::isfinite(mag)) return { 0.0f, 0.0f };
    const float t = std::clamp((mag - kStreamStillSlope) / (kStreamFallSlope - kStreamStillSlope), 0.0f, 1.0f);
    const float speed = kStreamMaxSpeed * t * t * (3.0f - 2.0f * t);
    return { -g.x / mag * speed, -g.y / mag * speed };
}

float brushDugAt(const TerrainComponent& tc, float x, float z)
{
    const int res = static_cast<int>(tc.resolution);
    if (res < 2 || !hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return 0.0f;
    bool any = false;
    for (const Body& b : tc.water.bodies) any = any || !b.brushPatch.empty();
    if (!any) return 0.0f;
    const float stepX = tc.sizeX / static_cast<float>(res - 1), stepZ = tc.sizeZ / static_cast<float>(res - 1);
    const float fx = (x + tc.sizeX * 0.5f) / stepX, fz = (z + tc.sizeZ * 0.5f) / stepZ;
    const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(i0), tz = fz - static_cast<float>(j0);
    auto at = [&](int i, int j) -> float
    {
        float sum = 0.0f;
        for (const Body& b : tc.water.bodies)
        {
            const GroundPatch& gp = b.brushPatch;
            if (gp.empty() || !patchFits(gp, tc.resolution)) continue;
            if (i < gp.x0 || i >= gp.x0 + gp.w || j < gp.z0 || j >= gp.z0 + gp.h) continue;
            sum += gp.delta[static_cast<size_t>(j - gp.z0) * gp.w + (i - gp.x0)];
        }
        return std::max(0.0f, -sum);                     // lowered ground is negative
    };
    const float d = (at(i0, j0) * (1.0f - tx) + at(i0 + 1, j0) * tx) * (1.0f - tz) +
                    (at(i0, j0 + 1) * (1.0f - tx) + at(i0 + 1, j0 + 1) * tx) * tz;
    return std::isfinite(d) ? d : 0.0f;
}

float streamFillAt(const TerrainComponent& tc, float x, float z)
{
    return std::max(0.0f, brushDugAt(tc, x, z) - kStreamFreeboard);
}

std::shared_ptr<const StreamPool> streamPoolOf(const TerrainComponent& tc, uint16_t body)
{
    struct Entry { const TerrainComponent* tc; uint16_t body; uint64_t revision, ground; const float* heights; size_t count;
                   std::shared_ptr<const StreamPool> pool; };
    static std::mutex mutex;
    static std::vector<Entry> cache;      // a handful: one per stream being looked at
    const Field& f = tc.water;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Entry& e : cache)
            if (e.tc == &tc && e.body == body && e.revision == f.revision && e.ground == tc.builtFieldVersion &&
                e.heights == tc.sculptHeights.data() && e.count == tc.sculptHeights.size())
                return e.pool;
    }
    auto pool = std::make_shared<StreamPool>();
    const CellRect cells = bodyCells(tc, body);
    if (f.allocated() && cells.valid() && hasArea(tc))
    {
        const int res = static_cast<int>(f.res);
        CellRect box = cells;
        box.x0 = std::max(0, box.x0 - 1); box.z0 = std::max(0, box.z0 - 1);
        box.x1 = std::min(res - 1, box.x1 + 1); box.z1 = std::min(res - 1, box.z1 + 1);
        const int w = box.x1 - box.x0 + 1, h = box.z1 - box.z0 + 1;
        pool->box = box; pool->w = w; pool->h = h;
        pool->depth.assign(static_cast<size_t>(w) * h, 0.0f);
        std::vector<float> ground(static_cast<size_t>(w) * h, 0.0f), level(ground.size(), 0.0f);
        std::vector<uint8_t> wet(ground.size(), 0), seen(ground.size(), 0);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
            {
                const size_t k = static_cast<size_t>(j) * w + i;
                const size_t c = static_cast<size_t>(box.z0 + j) * res + (box.x0 + i);
                wet[k] = f.owner[c] == body && f.coverage[c] >= kWet;
                if (!wet[k]) continue;
                const glm::vec2 cc = cellCenter(tc, box.x0 + i, box.z0 + j);
                const float g = terrainHeightAt(tc, cc.x, cc.y);
                ground[k] = std::isfinite(g) ? g : 0.0f;
            }
        // Priority flood from the water's own edge: a cell is reached at the lowest level water can drain to from it.
        using Item = std::pair<float, uint32_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
            {
                const size_t k = static_cast<size_t>(j) * w + i;
                if (!wet[k]) continue;
                const bool edge = i == 0 || j == 0 || i == w - 1 || j == h - 1 ||
                                  !wet[k - 1] || !wet[k + 1] || !wet[k - static_cast<size_t>(w)] || !wet[k + static_cast<size_t>(w)];
                if (!edge) continue;
                seen[k] = 1; level[k] = ground[k];
                open.push({ ground[k], static_cast<uint32_t>(k) });
            }
        while (!open.empty())
        {
            const Item top = open.top(); open.pop();
            const int i = static_cast<int>(top.second % static_cast<uint32_t>(w)), j = static_cast<int>(top.second / static_cast<uint32_t>(w));
            const int ni[4] = { i - 1, i + 1, i, i }, nj[4] = { j, j, j - 1, j + 1 };
            for (int n = 0; n < 4; ++n)
            {
                if (ni[n] < 0 || nj[n] < 0 || ni[n] >= w || nj[n] >= h) continue;
                const size_t k = static_cast<size_t>(nj[n]) * w + ni[n];
                if (!wet[k] || seen[k]) continue;
                seen[k] = 1;
                level[k] = std::max(ground[k], top.first);
                open.push({ level[k], static_cast<uint32_t>(k) });
            }
        }
        for (size_t k = 0; k < ground.size(); ++k)
            if (wet[k] && seen[k]) pool->depth[k] = std::max(0.0f, level[k] - ground[k]);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back({ &tc, body, f.revision, tc.builtFieldVersion, tc.sculptHeights.data(), tc.sculptHeights.size(), pool });
    return pool;
}

float streamPoolAt(const StreamPool& pool, const TerrainComponent& tc, float x, float z)
{
    if (pool.depth.empty() || !hasArea(tc)) return 0.0f;
    const glm::vec2 cell = cellSize(tc);
    // Cell centres sit at (index + 0.5) cells from the terrain's corner.
    const float fx = (x + tc.sizeX * 0.5f) / cell.x - 0.5f - static_cast<float>(pool.box.x0);
    const float fz = (z + tc.sizeZ * 0.5f) / cell.y - 0.5f - static_cast<float>(pool.box.z0);
    const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(i0), tz = fz - static_cast<float>(j0);
    auto at = [&](int i, int j) -> float {
        i = std::clamp(i, 0, pool.w - 1); j = std::clamp(j, 0, pool.h - 1);
        return pool.depth[static_cast<size_t>(j) * pool.w + i];
    };
    const float d = (at(i0, j0) * (1.0f - tx) + at(i0 + 1, j0) * tx) * (1.0f - tz) + (at(i0, j0 + 1) * (1.0f - tx) + at(i0 + 1, j0 + 1) * tx) * tz;
    return std::isfinite(d) ? std::max(0.0f, d) : 0.0f;
}

float streamSurfaceAt(const TerrainComponent& tc, const Body& body, float x, float z)
{
    const float ground = terrainHeightAt(tc, x, z);
    const auto pool = streamPoolOf(tc, body.id);
    const float poolDepth = pool ? streamPoolAt(*pool, tc, x, z) : 0.0f;
    return (std::isfinite(ground) ? ground : body.level) + std::max(body.streamDepth + poolDepth, streamFillAt(tc, x, z));
}

glm::vec2 streamFlowAt(const TerrainComponent& tc, float x, float z)
{
    return slopeToFlow(streamSlopeAt(tc, x, z));
}

// ── Cells of a body ──────────────────────────────────────────────────────────
CellRect bodyCells(const TerrainComponent& tc, uint16_t body)
{
    CellRect r;
    const Field& f = tc.water;
    if (!f.allocated() || body == kNoBody) return r;
    const int res = static_cast<int>(f.res);
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x)
        {
            const size_t i = static_cast<size_t>(z) * res + x;
            if (f.owner[i] != body) continue;
            if (!r.valid()) { r.x0 = r.x1 = x; r.z0 = r.z1 = z; continue; }
            r.x0 = std::min(r.x0, x); r.x1 = std::max(r.x1, x);
            r.z0 = std::min(r.z0, z); r.z1 = std::max(r.z1, z);
        }
    return r;
}

std::vector<CellRect> allBodyCells(const TerrainComponent& tc)
{
    const Field& f = tc.water;
    std::vector<CellRect> out(f.bodies.size());
    if (!f.allocated() || f.bodies.empty()) return out;

    uint16_t top = 0;
    for (const Body& b : f.bodies) top = std::max(top, b.id);
    std::vector<int32_t> slot(static_cast<size_t>(top) + 1, -1);
    for (size_t i = 0; i < f.bodies.size(); ++i) slot[f.bodies[i].id] = static_cast<int32_t>(i);

    const int res = static_cast<int>(f.res);
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x)
        {
            const uint16_t o = f.owner[static_cast<size_t>(z) * res + x];
            if (o == kNoBody || o > top || slot[o] < 0) continue;
            CellRect& r = out[static_cast<size_t>(slot[o])];
            if (!r.valid()) { r.x0 = r.x1 = x; r.z0 = r.z1 = z; continue; }
            r.x0 = std::min(r.x0, x); r.x1 = std::max(r.x1, x);
            r.z0 = std::min(r.z0, z); r.z1 = std::max(r.z1, z);
        }
    return out;
}

// ── Lattice ──────────────────────────────────────────────────────────────────
namespace
{
    // buildLattice for whatever `owned(owner)` says is the sheet's water: one body,
    // or the union of a stroke's steps.
    template <class Owned>
    bool buildLatticeOf(const TerrainComponent& tc, const Owned& owned, const CellRect& cells, Lattice& out,
                        const ShoreClip& clip);
}

bool buildLattice(const TerrainComponent& tc, uint16_t body, const CellRect& cells, Lattice& out,
                  const ShoreClip& clip)
{
    return buildLatticeOf(tc, [body](uint16_t owner) { return owner == body; }, cells, out, clip);
}

bool buildLattice(const TerrainComponent& tc, const std::vector<uint16_t>& bodies, const CellRect& cells,
                  Lattice& out, const ShoreClip& clip)
{
    return buildLatticeOf(tc, [&bodies](uint16_t owner) {
        return owner != kNoBody && std::find(bodies.begin(), bodies.end(), owner) != bodies.end();
    }, cells, out, clip);
}

namespace
{
template <class Owned>
bool buildLatticeOf(const TerrainComponent& tc, const Owned& owned, const CellRect& cells, Lattice& out,
                    const ShoreClip& clip)
{
    out = Lattice{};
    const Field& f = tc.water;
    if (!f.allocated() || !cells.valid() || !hasArea(tc)) return false;

    const int res = static_cast<int>(f.res);
    // A stream reaches one cell into the water next to it (below), so its margin of dry corners is one wider.
    const int pad = (clip.streamDepth > 0.0f || clip.joinBody != kNoBody) ? 2 : 1;
    out.x0 = cells.x0 - pad;
    out.z0 = cells.z0 - pad;
    out.w  = cells.x1 - cells.x0 + 2 + 2 * pad;     // corners x0−pad … x1+1+pad (x0−1 … x1+2 for pad 1)
    out.h  = cells.z1 - cells.z0 + 2 + 2 * pad;
    out.cellW = static_cast<double>(tc.sizeX) / res;
    out.cellH = static_cast<double>(tc.sizeZ) / res;
    out.originX = -static_cast<double>(tc.sizeX) * 0.5;
    out.originZ = -static_cast<double>(tc.sizeZ) * 0.5;
    out.minX = out.originX; out.maxX = -out.originX;
    out.minZ = out.originZ; out.maxZ = -out.originZ;
    out.v.assign(static_cast<size_t>(out.w) * out.h, 0.0f);
    std::vector<uint8_t> strip(out.v.size(), 0);         // corners next to a cell reached into (joining)

    // A cell outside the terrain is its nearest edge cell, so water that reaches
    // the border goes on reaching it; a corner outside the terrain stays 0, which
    // is what closes the contour (it is clamped back onto the border afterwards).
    // A stream (ShoreClip::streamDepth) reaches ONE cell into any other water that touches it: where it runs into a
    // lake the two sheets' soft edges would otherwise leave a gap between them (each thins out at its own
    // border). The stream's film goes on under the lake's first cell, and the lake's edge is over it.
    const Body* me = clip.joinBody != kNoBody ? f.findBody(clip.joinBody) : nullptr;
    const bool joinsOthers = me != nullptr;
    std::vector<int8_t> reaches(65536, -1);             // per other body: -1 unknown, 0 no, 1 yes
    auto reachesInto = [&](uint16_t other) -> bool
    {
        int8_t& r = reaches[other];
        if (r < 0)
        {
            const Body* ob = f.findBody(other);
            r = (ob && joinReaches(*me, *ob)) ? 1 : 0;
        }
        return r == 1;
    };
    auto ownCell = [&](int x, int z) -> bool
    {
        if (x < 0 || x >= res || z < 0 || z >= res) return false;
        const size_t i = static_cast<size_t>(z) * res + x;
        return f.coverage[i] != 0 && owned(f.owner[i]);
    };
    bool viaJoin = false;                                // set by cell(): this value came from a neighbour's cell
    auto cell = [&](int x, int z) -> float
    {
        x = std::clamp(x, 0, res - 1);
        z = std::clamp(z, 0, res - 1);
        const size_t i = static_cast<size_t>(z) * res + x;
        if (owned(f.owner[i])) return static_cast<float>(f.coverage[i]);
        if (joinsOthers && f.owner[i] != kNoBody && f.coverage[i] >= kWet && reachesInto(f.owner[i]))
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dz) && ownCell(x + dx, z + dz) && f.coverage[static_cast<size_t>(z + dz) * res + (x + dx)] >= kWet)
                    {
                        viaJoin = true;
                        return 255.0f;
                    }
        return 0.0f;
    };
    for (int j = 0; j < out.h; ++j)
        for (int i = 0; i < out.w; ++i)
        {
            const int cx = out.x0 + i, cz = out.z0 + j;
            if (cx < 0 || cx > res || cz < 0 || cz > res) continue;
            viaJoin = false;
            out.v[static_cast<size_t>(j) * out.w + i] =
                0.25f * (cell(cx - 1, cz - 1) + cell(cx, cz - 1) + cell(cx - 1, cz) + cell(cx, cz));
            if (viaJoin) strip[static_cast<size_t>(j) * out.w + i] = 1;
        }

    // The ground, where it can matter: at a corner that is water and at the four
    // corners an edge leads to from it. Nothing else is ever looked at (an edge
    // needs an inside corner), so a lake in a large box costs its own corners and
    // a noise landscape is not sampled once per corner of empty land.
    if (clip.streamDepth > 0.0f)
    {
        // A stream: the ground never clips it. `g` is how deep the water LOOKS at a corner (the soft
        // shore reads it as e): the visual depth in the middle, nothing at the edge, over the first
        // half cell of coverage that is more than the contour's. A corner outside the water reads 0
        // (the waterline), so the edge between the two thins out instead of ending in a step.
        // groundY keeps the ground at the corners the sheet is made of.
        constexpr float kFar = 1.0e6f;
        out.g.assign(out.v.size(), kFar);
        out.groundY.assign(out.v.size(), 0.0f);
        const auto streamPool = clip.joinBody != kNoBody ? streamPoolOf(tc, clip.joinBody) : nullptr;
        auto sample = [&](int i, int j)
        {
            const size_t k = static_cast<size_t>(j) * out.w + i;
            if (out.g[k] != kFar) return;
            const double iso = kContourIso;
            const float v = out.v[k];
            float t = 0.0f;
            if (v >= iso) { t = static_cast<float>((v - iso) / (255.0 - iso)); t = t * t * (3.0f - 2.0f * t); }
            const glm::dvec2 p = out.position(i, j);
            const float x = static_cast<float>(std::clamp(p.x, out.minX, out.maxX));
            const float z = static_cast<float>(std::clamp(p.y, out.minZ, out.maxZ));
            // A filled channel is as deep as it was dug (up to what the shore ramp can show): the real depth, never less than the look.
            const float poolHere = streamPool ? streamPoolAt(*streamPool, tc, x, z) : 0.0f;
            out.g[k] = clip.overshoot + std::max(std::max(kStreamVisualDepth * t, streamFillAt(tc, x, z) * t), poolHere);
            const float ground = terrainHeightAt(tc, x, z);
            out.groundY[k] = std::isfinite(ground) ? ground : 0.0f;
        };
        for (int j = 0; j < out.h; ++j)
            for (int i = 0; i < out.w; ++i)
            {
                if (out.at(i, j) < static_cast<float>(kContourIso)) continue;
                sample(i, j);
                if (i > 0)         sample(i - 1, j);
                if (i + 1 < out.w) sample(i + 1, j);
                if (j > 0)         sample(i, j - 1);
                if (j + 1 < out.h) sample(i, j + 1);
            }
        return true;
    }
    if (clip.enabled)
    {
        constexpr float kFar = 1.0e6f;                 // "deep": not a limit anywhere
        out.g.assign(out.v.size(), kFar);
        auto sample = [&](int i, int j)
        {
            float& d = out.g[static_cast<size_t>(j) * out.w + i];
            if (d != kFar) return;
            const glm::dvec2 p = out.position(i, j);
            const float x = static_cast<float>(std::clamp(p.x, out.minX, out.maxX));
            const float z = static_cast<float>(std::clamp(p.y, out.minZ, out.maxZ));
            const float ground = terrainHeightAt(tc, x, z);
            d = std::isfinite(ground) ? clip.level + clip.overshoot - ground : -kFar;
            // In the strip a body reaches into a neighbour's water, its depth thins out toward the strip's far edge,
            // so the sheet fades out there (the soft shore reads it) while the neighbour's fades in: no cut edge, no seam.
            if (strip[static_cast<size_t>(j) * out.w + i] && d > clip.overshoot)
            {
                float t = std::clamp((out.v[static_cast<size_t>(j) * out.w + i] - static_cast<float>(kContourIso)) /
                                     (255.0f - static_cast<float>(kContourIso)), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                d = clip.overshoot + (d - clip.overshoot) * t;
            }
            // A depth of exactly kFar would read as "not sampled yet"; nothing real is that deep.
            if (d == kFar) d = kFar * 0.5f;
        };
        for (int j = 0; j < out.h; ++j)
            for (int i = 0; i < out.w; ++i)
            {
                if (out.at(i, j) < static_cast<float>(kContourIso)) continue;
                sample(i, j);
                if (i > 0)         sample(i - 1, j);
                if (i + 1 < out.w) sample(i + 1, j);
                if (j > 0)         sample(i, j - 1);
                if (j + 1 < out.h) sample(i, j + 1);
            }
    }
    return true;
}
} // namespace

// ── Contours ─────────────────────────────────────────────────────────────────
std::vector<Contour> extractContours(const Lattice& L, double iso)
{
    std::vector<Contour> result;
    if (L.w < 2 || L.h < 2 || L.v.size() != static_cast<size_t>(L.w) * L.h) return result;

    // A crossing is identified by the lattice edge it sits on: even keys are the
    // edge from corner (i, j) to (i + 1, j), odd keys the edge up to (i, j + 1).
    // Both cells that share an edge compute the same key, so their segments meet.
    auto hKey = [&](int i, int j) -> int64_t { return (static_cast<int64_t>(j) * L.w + i) * 2; };
    auto vKey = [&](int i, int j) -> int64_t { return (static_cast<int64_t>(j) * L.w + i) * 2 + 1; };

    std::unordered_map<int64_t, glm::dvec2> points;
    std::unordered_map<int64_t, int64_t>    next;      // crossing → the crossing the contour goes to

    struct Crossing { int64_t key; bool exit; };

    for (int j = 0; j + 1 < L.h; ++j)
        for (int i = 0; i + 1 < L.w; ++i)
        {
            const CellCut cut = cutCell(L, i, j, iso);
            if (cut.crossings == 0) continue;
            // Edge e runs from corner e to corner e+1.
            const int64_t keys[4] = { hKey(i, j), vKey(i + 1, j), hKey(i, j + 1), vKey(i, j) };

            Crossing xs[4];
            int n = 0;
            for (int e = 0; e < 4; ++e)
            {
                if (cut.in[e] == cut.in[(e + 1) & 3]) continue;
                points[keys[e]] = cut.cross[e];
                // Walking the cell counter-clockwise, a crossing from a wet corner
                // to a dry one is where the water's boundary LEAVES the cell edge.
                xs[n++] = { keys[e], cut.in[e] };
            }

            if (n == 2)
            {
                // One piece of contour: from the exit to the entry.
                if (xs[0].exit) next[xs[0].key] = xs[1].key;
                else            next[xs[1].key] = xs[0].key;
            }
            else if (n == 4)
            {
                // A saddle: each exit continues to the next entry when the wet
                // corners are joined through the middle (a hexagon of water), to
                // the previous one when they are apart (two corner triangles).
                for (int k = 0; k < 4; ++k)
                    if (xs[k].exit)
                        next[xs[k].key] = xs[cut.joined ? (k + 1) & 3 : (k + 3) & 3].key;
            }
        }

    std::vector<int64_t> starts;
    starts.reserve(next.size());
    for (const auto& kv : next) starts.push_back(kv.first);
    std::sort(starts.begin(), starts.end());           // the same lake always gives the same mesh

    const double eps = mergeTolerance(L);
    const double minArea = 0.02 * L.cellW * L.cellH;
    std::unordered_set<int64_t> seen;
    for (const int64_t start : starts)
    {
        if (seen.count(start)) continue;
        Ring ring;
        int64_t cur = start;
        bool closed = false;
        for (size_t guard = 0; guard <= next.size(); ++guard)
        {
            seen.insert(cur);
            ring.push_back(points[cur]);
            const auto it = next.find(cur);
            if (it == next.end()) break;                // cannot happen; discard rather than guess
            cur = it->second;
            if (cur == start) { closed = true; break; }
        }
        if (!closed) continue;

        // The part of a contour that left the terrain (the margin corners outside it
        // are dry) goes back onto the border, where the water ends.
        for (glm::dvec2& p : ring)
        {
            p.x = std::clamp(p.x, L.minX, L.maxX);
            p.y = std::clamp(p.y, L.minZ, L.maxZ);
        }
        simplifyRing(ring, eps);
        if (ring.size() < 3) continue;

        Contour c;
        c.area = signedArea(ring);
        if (std::abs(c.area) < minArea) continue;
        c.points = std::move(ring);
        result.push_back(std::move(c));
    }
    return result;
}

bool insideSheet(const Lattice& L, double x, double z, double iso)
{
    if (L.w < 2 || L.h < 2 || L.v.size() != static_cast<size_t>(L.w) * L.h) return false;
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    const double fi = (x - L.originX) / L.cellW - L.x0;
    const double fj = (z - L.originZ) / L.cellH - L.z0;
    if (fi < 0.0 || fj < 0.0 || fi > L.w - 1 || fj > L.h - 1) return false;
    // The cell the point is in; on the last corner line, the cell before it.
    const int i = std::min(static_cast<int>(std::floor(fi)), L.w - 2);
    const int j = std::min(static_cast<int>(std::floor(fj)), L.h - 2);
    const CellCut cut = cutCell(L, i, j, iso);
    if (cut.crossings == 0) return cut.in[0];

    const glm::dvec2 p(x, z);
    const double eps = mergeTolerance(L);
    if (cut.crossings == 4 && !cut.joined)
    {
        // Two corner triangles: each wet corner with the crossings on its two edges.
        for (int k = 0; k < 4; ++k)
            if (cut.in[k] && inOrOnRing({ cut.cross[(k + 3) & 3], cut.corner[k], cut.cross[k] }, p, eps))
                return true;
        return false;
    }
    // The water side of the cell: its inside corners and the crossings between
    // them, in order round the cell — one piece of contour cuts off the rest, a
    // joined saddle leaves a hexagon.
    Ring piece;
    for (int k = 0; k < 4; ++k)
    {
        if (cut.in[k]) piece.push_back(cut.corner[k]);
        if (cut.in[k] != cut.in[(k + 1) & 3]) piece.push_back(cut.cross[k]);
    }
    return piece.size() >= 3 && inOrOnRing(piece, p, eps);
}

std::vector<ShoreSegment> shoreSegments(const Lattice& L, double iso)
{
    std::vector<ShoreSegment> out;
    if (L.w < 2 || L.h < 2 || L.v.size() != static_cast<size_t>(L.w) * L.h) return out;
    for (int j = 0; j + 1 < L.h; ++j)
        for (int i = 0; i + 1 < L.w; ++i)
        {
            const CellCut cut = cutCell(L, i, j, iso);
            if (cut.crossings == 0) continue;
            // The crossings in edge order and whether each leaves the water, as
            // extractContours pairs them.
            int  e[4];
            bool exit[4];
            int  n = 0;
            for (int k = 0; k < 4; ++k)
                if (cut.in[k] != cut.in[(k + 1) & 3]) { e[n] = k; exit[n] = cut.in[k]; ++n; }
            if (n == 2)
                out.push_back({ cut.cross[e[0]], cut.cross[e[1]] });
            else if (n == 4)
                for (int k = 0; k < 4; ++k)
                    if (exit[k])
                        out.push_back({ cut.cross[e[k]], cut.cross[e[cut.joined ? (k + 1) & 3 : (k + 3) & 3]] });
        }
    return out;
}

std::vector<Polygon> groupContours(const std::vector<Contour>& contours)
{
    std::vector<Polygon> polys;
    std::vector<double>  areas;
    for (const Contour& c : contours)
        if (!c.hole())
        {
            polys.push_back({ c.points, {} });
            areas.push_back(c.area);
        }
    for (const Contour& c : contours)
    {
        if (!c.hole()) continue;
        // The smallest outer boundary that encloses it: with islands inside lakes
        // inside islands, that is the one whose water the hole is cut out of.
        int best = -1;
        for (size_t k = 0; k < polys.size(); ++k)
            if (pointInRing(polys[k].outer, c.points[0]) && (best < 0 || areas[k] < areas[static_cast<size_t>(best)]))
                best = static_cast<int>(k);
        if (best >= 0) polys[static_cast<size_t>(best)].holes.push_back(c.points);
    }
    return polys;
}

// ── Mesh ─────────────────────────────────────────────────────────────────────
namespace
{
    // The lattice's coverage (0..255) at a terrain-local point, bilinear between corners; 0 off the lattice.
    float coverageAt(const Lattice& L, double x, double z)
    {
        if (L.w < 2 || L.h < 2) return 0.0f;
        const double u = std::clamp((x - L.originX) / L.cellW - L.x0, 0.0, static_cast<double>(L.w - 1));
        const double v = std::clamp((z - L.originZ) / L.cellH - L.z0, 0.0, static_cast<double>(L.h - 1));
        const int i = std::min(static_cast<int>(u), L.w - 2);
        const int j = std::min(static_cast<int>(v), L.h - 2);
        const float tx = static_cast<float>(u - i), tz = static_cast<float>(v - j);
        const float c00 = L.at(i, j), c10 = L.at(i + 1, j), c01 = L.at(i, j + 1), c11 = L.at(i + 1, j + 1);
        return (c00 + (c10 - c00) * tx) * (1.0f - tz) + (c01 + (c11 - c01) * tx) * tz;
    }
}

uint64_t surfaceHash(const Lattice& l, float level, const SurfaceOptions& opt)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, l.x0); h = fnv(h, l.z0); h = fnv(h, l.w); h = fnv(h, l.h);
    h = fnv(h, l.originX); h = fnv(h, l.originZ); h = fnv(h, l.cellW); h = fnv(h, l.cellH);
    h = fnv(h, level);
    h = fnv(h, opt.uvOrigin.x); h = fnv(h, opt.uvOrigin.y); h = fnv(h, opt.uvMetersPerTile);
    if (!l.v.empty()) h = fnv(h, l.v.data(), l.v.size() * sizeof(float));
    // A lake over a hill that was sculpted is a different sheet with the same cells.
    h = fnv(h, static_cast<uint64_t>(l.g.size()));
    if (!l.g.empty()) h = fnv(h, l.g.data(), l.g.size() * sizeof(float));
    // A stream is made of the ground it lies on, and of the turn its slope vector is baked with.
    if (opt.streamDepth > 0.0f)
    {
        h = fnv(h, opt.streamDepth); h = fnv(h, opt.streamTurn.x); h = fnv(h, opt.streamTurn.y);
        h = fnv(h, opt.streamTurn.z); h = fnv(h, opt.streamTurn.w); h = fnv(h, opt.shoreTiltScale);
        h = fnv(h, opt.streamBody); h = fnv(h, opt.streamJoinKey);
        if (!l.groundY.empty()) h = fnv(h, l.groundY.data(), l.groundY.size() * sizeof(float));
    }
    // Only when on, so a mesh without the soft shore keeps the hash it always had.
    if (opt.shoreFade && !l.g.empty())
    {
        h = fnv(h, static_cast<uint32_t>(1)); h = fnv(h, opt.shoreOvershoot);
        h = fnv(h, opt.shoreTiltScale);   // the landscape's own scale bakes into the tilt
    }
    return h;
}

float shoreProximityAt(const Lattice& L, double x, double z, float overshoot)
{
    if (!L.clipped() || L.w < 2 || L.h < 2) return 0.0f;
    const double u = std::clamp((x - L.originX) / L.cellW - L.x0, 0.0, static_cast<double>(L.w - 1));
    const double v = std::clamp((z - L.originZ) / L.cellH - L.z0, 0.0, static_cast<double>(L.h - 1));
    const int i = std::min(static_cast<int>(u), L.w - 2);
    const int j = std::min(static_cast<int>(v), L.h - 2);
    const float tx = static_cast<float>(u - i), tz = static_cast<float>(v - j);
    const float e00 = cornerProximity(L, i,     j,     overshoot), e10 = cornerProximity(L, i + 1, j,     overshoot);
    const float e01 = cornerProximity(L, i,     j + 1, overshoot), e11 = cornerProximity(L, i + 1, j + 1, overshoot);
    return (e00 + (e10 - e00) * tx) * (1.0f - tz) + (e01 + (e11 - e01) * tx) * tz;
}

bool buildSurface(const TerrainComponent& tc, uint16_t body, const SurfaceOptions& opt, SurfaceMesh& out)
{
    out = SurfaceMesh{};
    const Body* b = tc.water.findBody(body);
    if (!b || !tc.water.allocated()) return false;
    const CellRect cells = bodyCells(tc, body);
    if (!cells.valid()) { out.level = b->level; return true; }       // a body with no water: nothing to draw
    Lattice lattice;
    SurfaceOptions bodyOpt = opt;
    bodyOpt.joinBody = b->id;
    if (b->stream) { bodyOpt.streamDepth = b->streamDepth; bodyOpt.streamBody = b->id; bodyOpt.streamJoinKey = streamJoinKeyOf(tc.water); }
    if (!buildLattice(tc, body, cells, lattice, bodyOpt.clipFor(b->level))) return false;
    return buildSurface(tc, lattice, cells, b->level, bodyOpt, out);
}

bool buildSurface(const TerrainComponent& tc, const Lattice& lattice, const CellRect& cells,
                  float level, const SurfaceOptions& opt, SurfaceMesh& out)
{
    out = SurfaceMesh{};
    out.level = level;
    out.cells = cells;
    if (lattice.empty()) return false;
    out.hash = surfaceHash(lattice, level, opt);

    const std::vector<Contour> contours = extractContours(lattice);
    const std::vector<Polygon> polys    = groupContours(contours);
    out.contours = static_cast<uint32_t>(contours.size());
    out.polygons = static_cast<uint32_t>(polys.size());
    if (polys.empty()) return true;

    // The deepest water, at the water corners (the lattice's ground is water over the ground plus the
    // overshoot there; every water corner is sampled). Between corners the ground can dip a little
    // further, which the material's margin covers.
    if (opt.streamDepth > 0.0f && !lattice.groundY.empty())
        out.deepest = opt.streamDepth;     // the film over the ground; no camera is "in" it below that
    else if (lattice.clipped())
    {
        float deepest = 0.0f;
        for (size_t i = 0; i < lattice.v.size(); ++i)
            if (lattice.v[i] >= static_cast<float>(kContourIso) && lattice.g[i] < 1.0e5f)
                deepest = std::max(deepest, lattice.g[i] - opt.shoreOvershoot);
        out.deepest = deepest;
    }

    // The vertices are relative to the middle of the body's box, so a lake far
    // from the terrain's centre keeps small coordinates and the entity's own
    // position says where it is (and sorts, and culls, by it).
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const Polygon& p : polys)
        for (const glm::dvec2& v : p.outer)
        {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    const glm::dvec2 center = (lo + hi) * 0.5;
    out.center = glm::vec2(static_cast<float>(center.x), static_cast<float>(center.y));

    StaticMeshAsset& m = out.mesh;
    m.type = HE::AssetType::StaticMesh;
    m.name = "water_surface";
    m.path = "mem://water_surface";

    const double invTile = 1.0 / std::max(1e-6f, opt.uvMetersPerTile);
    // A stream is draped on the ground: its heights come from the terrain, its soft edge from the lattice.
    const bool stream = opt.streamDepth > 0.0f && !lattice.groundY.empty();
    const bool fade = (opt.shoreFade || stream) && lattice.clipped();
    float deepestFill = 0.0f;                          // a stream: the thickest water, for `deepest`
    const auto meshPool = stream && opt.streamBody != kNoBody ? streamPoolOf(tc, opt.streamBody) : nullptr;
    float bmin[3] = {  1e30f,  1e30f,  1e30f };
    float bmax[3] = { -1e30f, -1e30f, -1e30f };

    for (const Polygon& poly : polys)
    {
        std::vector<Ring> rings;
        rings.reserve(1 + poly.holes.size());
        rings.push_back(poly.outer);
        for (const Ring& h : poly.holes) rings.push_back(h);

        std::vector<uint32_t> tris = triangulate(rings);
        if (tris.empty()) continue;

        // The polygon's vertices, the rings laid end to end like the triangles index
        // them, and with the soft shore the shore proximity of each.
        std::vector<glm::dvec2> pts;
        for (const Ring& r : rings) pts.insert(pts.end(), r.begin(), r.end());
        std::vector<float> prox;
        if (fade)
        {
            prox.reserve(pts.size());
            for (const glm::dvec2& v : pts) prox.push_back(shoreProximityAt(lattice, v.x, v.y, opt.shoreOvershoot));
            // A stream is cut down to the terrain's own grid, so a straight line between two vertices is the ground
            // between them (the sheet is the ground lifted, vertex by vertex).
            double maxLen = 0.0;
            if (stream)
            {
                const double spacing = static_cast<double>(tc.sizeX) / static_cast<double>(std::max(2u, tc.resolution) - 1u);
                maxLen = std::clamp(std::min(std::min(lattice.cellW, lattice.cellH), spacing), 0.4, 3.0);
            }
            refineShore(lattice, opt.shoreOvershoot, pts, prox, tris, maxLen);
        }

        const uint32_t base = static_cast<uint32_t>(m.vertices.size() / 3);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec2& v = pts[k];
            const float x = static_cast<float>(v.x - center.x);
            const float z = static_cast<float>(v.y - center.y);
            float y = 0.0f;
            glm::vec2 uv(static_cast<float>((v.x + opt.uvOrigin.x) * invTile),
                         static_cast<float>((v.y + opt.uvOrigin.y) * invTile));
            if (stream)
            {
                // The ground under the vertex, the film's depth over it (thinning to the lift at the edge:
                // the lattice coverage is the iso value there), relative to the entity's height.
                const float lx = static_cast<float>(std::clamp(v.x, lattice.minX, lattice.maxX));
                const float lz = static_cast<float>(std::clamp(v.y, lattice.minZ, lattice.maxZ));
                float ground = terrainHeightAt(tc, lx, lz);
                if (!std::isfinite(ground)) ground = level;
                const float cov = coverageAt(lattice, v.x, v.y);
                float t = std::clamp((cov - static_cast<float>(kContourIso)) / (255.0f - static_cast<float>(kContourIso)), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                // The film, a pool the hollow fills to its spill point, or the dug channel filled to the old ground.
                const float poolHere = meshPool ? streamPoolAt(*meshPool, tc, lx, lz) : 0.0f;
                const float thick = std::max(poolHere + opt.streamDepth * t, streamFillAt(tc, lx, lz) * t);
                deepestFill = std::max(deepestFill, thick);
                y = ground + kStreamLift + thick - level;
                // Over another body's cells (the lake it runs into) it lies just under that water's surface.
                if (const uint16_t other = bodyAt(tc, lx, lz); other != kNoBody && other != opt.streamBody)
                    if (const Body* ob = tc.water.findBody(other); ob && !ob->stream)
                        y = std::max(ground + kStreamLift - level, std::min(y, ob->level - 0.03f - level));
                // The ground's slope in WORLD axes (dimensionless): the material draws the sheet's lean from
                // it and runs the current down it. The landscape's turn, then its y/x scale.
                const glm::vec2 gl = streamSlopeAt(tc, lx, lz);
                uv = glm::vec2((opt.streamTurn.x * gl.x + opt.streamTurn.z * gl.y) * opt.shoreTiltScale,
                               (opt.streamTurn.y * gl.x + opt.streamTurn.w * gl.y) * opt.shoreTiltScale);
                // A pool is still and level: no current, no lean (fading in over its first decimetres).
                uv *= std::clamp(1.0f - poolHere / (3.0f * kStreamPoolStill), 0.0f, 1.0f);
            }
            m.vertices.push_back(x);
            m.vertices.push_back(y);
            m.vertices.push_back(z);
            // +Y; with the soft shore a tilt in x says how shallow the water is here
            // (kShoreNormalGain, Physics/WaterWaves.h). The material reads it back.
            // shoreTiltScale pre-compensates for the landscape's own non-uniform scale,
            // see SurfaceOptions above. A stream keeps +Y too: its lean goes into the UV, because the
            // material takes the wave normal as the mesh normal minus its tilt and would throw a lean away.
            const float e = fade ? prox[k] : 0.0f;
            m.normals.push_back(e * HE::water::kShoreNormalGain * opt.shoreTiltScale);
            m.normals.push_back(1.0f);
            m.normals.push_back(0.0f);
            if (e > 0.0f) ++out.shoreVertices;
            // World coordinates, so a second body (or terrain) beside this one
            // continues the pattern instead of restarting it.
            m.uvs.push_back(uv.x);
            m.uvs.push_back(uv.y);
            bmin[0] = std::min(bmin[0], x); bmax[0] = std::max(bmax[0], x);
            bmin[1] = std::min(bmin[1], y); bmax[1] = std::max(bmax[1], y);
            bmin[2] = std::min(bmin[2], z); bmax[2] = std::max(bmax[2], z);
        }

        // The triangulator returns counter-clockwise in (x, z), i.e. facing down;
        // the surface faces up, like the terrain's (bl, tl, br).
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            const uint32_t a = tris[t], bI = tris[t + 1], c = tris[t + 2];
            m.indices.push_back(base + a);
            m.indices.push_back(base + c);
            m.indices.push_back(base + bI);
        }
    }
    if (m.indices.empty()) return true;
    if (stream) out.deepest = std::max(opt.streamDepth, deepestFill);

    // A flat box has no volume; a hair of thickness keeps every consumer that
    // wants a real AABB (culling, picking) from throwing it out. A stream's box is its real height range.
    if (stream) { bmin[1] -= 0.05f; bmax[1] += 0.05f; }
    else        { bmin[1] = -0.05f; bmax[1] = 0.05f; }
    std::memcpy(m.boundsMin, bmin, sizeof(bmin));
    std::memcpy(m.boundsMax, bmax, sizeof(bmax));
    out.triangles = static_cast<uint32_t>(m.indices.size() / 3);
    return true;
}

} // namespace HE::water
