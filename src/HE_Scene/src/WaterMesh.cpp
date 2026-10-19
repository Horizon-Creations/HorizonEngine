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
    void refineShore(const Lattice& L, float overshoot, std::vector<glm::dvec2>& P,
                     std::vector<float>& E, std::vector<uint32_t>& T)
    {
        constexpr float  kTolerance   = 0.03f;      // of e, i.e. 3 cm of water depth
        constexpr int    kMaxPasses   = 24;
        constexpr size_t kMaxVertices = 400000;
        const double hmin  = 0.5 * std::min(L.cellW, L.cellH);
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
                        it = bad.emplace(key, std::abs(eAt((a + b) * 0.5) - 0.5f * (E[v[k]] + E[v[(k + 1) % 3]])) > kTolerance).first;
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
bool buildLattice(const TerrainComponent& tc, uint16_t body, const CellRect& cells, Lattice& out,
                  const ShoreClip& clip)
{
    out = Lattice{};
    const Field& f = tc.water;
    if (!f.allocated() || !cells.valid() || !hasArea(tc)) return false;

    const int res = static_cast<int>(f.res);
    out.x0 = cells.x0 - 1;
    out.z0 = cells.z0 - 1;
    out.w  = cells.x1 - cells.x0 + 4;     // corners x0−1 … x1+2
    out.h  = cells.z1 - cells.z0 + 4;
    out.cellW = static_cast<double>(tc.sizeX) / res;
    out.cellH = static_cast<double>(tc.sizeZ) / res;
    out.originX = -static_cast<double>(tc.sizeX) * 0.5;
    out.originZ = -static_cast<double>(tc.sizeZ) * 0.5;
    out.minX = out.originX; out.maxX = -out.originX;
    out.minZ = out.originZ; out.maxZ = -out.originZ;
    out.v.assign(static_cast<size_t>(out.w) * out.h, 0.0f);

    // A cell outside the terrain is its nearest edge cell, so water that reaches
    // the border goes on reaching it; a corner outside the terrain stays 0, which
    // is what closes the contour (it is clamped back onto the border afterwards).
    auto cell = [&](int x, int z) -> float
    {
        x = std::clamp(x, 0, res - 1);
        z = std::clamp(z, 0, res - 1);
        const size_t i = static_cast<size_t>(z) * res + x;
        return f.owner[i] == body ? static_cast<float>(f.coverage[i]) : 0.0f;
    };
    for (int j = 0; j < out.h; ++j)
        for (int i = 0; i < out.w; ++i)
        {
            const int cx = out.x0 + i, cz = out.z0 + j;
            if (cx < 0 || cx > res || cz < 0 || cz > res) continue;
            out.v[static_cast<size_t>(j) * out.w + i] =
                0.25f * (cell(cx - 1, cz - 1) + cell(cx, cz - 1) + cell(cx - 1, cz) + cell(cx, cz));
        }

    // The ground, where it can matter: at a corner that is water and at the four
    // corners an edge leads to from it. Nothing else is ever looked at (an edge
    // needs an inside corner), so a lake in a large box costs its own corners and
    // a noise landscape is not sampled once per corner of empty land.
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
    // Only when on, so a mesh without the soft shore keeps the hash it always had.
    if (opt.shoreFade && !l.g.empty()) { h = fnv(h, static_cast<uint32_t>(1)); h = fnv(h, opt.shoreOvershoot); }
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
    if (!buildLattice(tc, body, cells, lattice, opt.clipFor(b->level))) return false;
    return buildSurface(tc, lattice, cells, b->level, opt, out);
}

bool buildSurface(const TerrainComponent& tc, const Lattice& lattice, const CellRect& cells,
                  float level, const SurfaceOptions& opt, SurfaceMesh& out)
{
    (void)tc;
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
    const bool fade = opt.shoreFade && lattice.clipped();
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
            refineShore(lattice, opt.shoreOvershoot, pts, prox, tris);
        }

        const uint32_t base = static_cast<uint32_t>(m.vertices.size() / 3);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec2& v = pts[k];
            const float x = static_cast<float>(v.x - center.x);
            const float z = static_cast<float>(v.y - center.y);
            m.vertices.push_back(x);
            m.vertices.push_back(0.0f);
            m.vertices.push_back(z);
            // +Y; with the soft shore a tilt in x says how shallow the water is here
            // (kShoreNormalGain, Physics/WaterWaves.h). The material reads it back.
            const float e = fade ? prox[k] : 0.0f;
            m.normals.push_back(e * HE::water::kShoreNormalGain);
            m.normals.push_back(1.0f);
            m.normals.push_back(0.0f);
            if (e > 0.0f) ++out.shoreVertices;
            // World coordinates, so a second body (or terrain) beside this one
            // continues the pattern instead of restarting it.
            m.uvs.push_back(static_cast<float>((v.x + opt.uvOrigin.x) * invTile));
            m.uvs.push_back(static_cast<float>((v.y + opt.uvOrigin.y) * invTile));
            bmin[0] = std::min(bmin[0], x); bmax[0] = std::max(bmax[0], x);
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

    // A flat box has no volume; a hair of thickness keeps every consumer that
    // wants a real AABB (culling, picking) from throwing it out.
    bmin[1] = -0.05f;
    bmax[1] =  0.05f;
    std::memcpy(m.boundsMin, bmin, sizeof(bmin));
    std::memcpy(m.boundsMax, bmax, sizeof(bmax));
    out.triangles = static_cast<uint32_t>(m.indices.size() / 3);
    return true;
}

} // namespace HE::water
