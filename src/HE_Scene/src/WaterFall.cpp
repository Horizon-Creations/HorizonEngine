#include "HorizonScene/WaterFall.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"

#include <Physics/WaterWaves.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>

namespace HE::water
{

namespace
{
    bool hasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    uint64_t fnv(uint64_t h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    // The cell a terrain-local point is in, or -1 off the grid.
    int cellIndexAt(const TerrainComponent& tc, double x, double z)
    {
        const int res = static_cast<int>(tc.water.res);
        const double fx = (x + 0.5 * tc.sizeX) / (tc.sizeX / res);
        const double fz = (z + 0.5 * tc.sizeZ) / (tc.sizeZ / res);
        if (!(fx >= 0.0 && fz >= 0.0)) return -1;
        const int cx = static_cast<int>(fx), cz = static_cast<int>(fz);
        if (cx >= res || cz >= res) return -1;
        return cz * res + cx;
    }

    // The foot of the ribbon that hangs from the sheet's edge at P.
    struct Foot
    {
        glm::dvec2 at{ 0.0, 0.0 };    // where the ribbon lands
        glm::dvec2 dir{ 0.0, 0.0 };   // unit, the way the apron goes on (downhill, or the edge's outward normal)
        float      y = 0.0f;          // terrain-local height of the foot
        bool       ok = false;
    };

    // Straight downhill from `p` until the ground is at or below `lowLevel`, at most kFallMaxRun metres.
    // The edge's outward normal `n` stands in for "downhill" where the ground is level or runs sideways
    // along the edge: a vertical drop there.
    Foot footOf(const TerrainComponent& tc, const glm::dvec2& p, const glm::dvec2& n, float lowLevel, float step)
    {
        Foot f;
        const float h  = std::max(step, 1e-3f);
        const float hx = terrainHeightAt(tc, static_cast<float>(p.x) + h, static_cast<float>(p.y)) -
                         terrainHeightAt(tc, static_cast<float>(p.x) - h, static_cast<float>(p.y));
        const float hz = terrainHeightAt(tc, static_cast<float>(p.x), static_cast<float>(p.y) + h) -
                         terrainHeightAt(tc, static_cast<float>(p.x), static_cast<float>(p.y) - h);
        glm::dvec2 down = n;
        bool runs = false;
        if (std::isfinite(hx) && std::isfinite(hz))
        {
            const glm::dvec2 g(hx / (2.0f * h), hz / (2.0f * h));
            const double len = glm::length(g);
            if (len > 0.05)
            {
                const glm::dvec2 d = -g / len;
                if (glm::dot(d, n) > 0.2) { down = d; runs = true; }
            }
        }
        f.dir = down;
        const float lift = lowLevel + kFallLift;
        float ground = terrainHeightAt(tc, static_cast<float>(p.x), static_cast<float>(p.y));
        f.at = p; f.y = lift; f.ok = true;
        if (!runs || !std::isfinite(ground) || ground <= lowLevel) return f;      // a vertical drop
        float prevT = 0.0f, prevH = ground;
        for (float t = step; ; t += step)
        {
            const float tt = std::min(t, kFallMaxRun);
            const glm::dvec2 q = p + down * static_cast<double>(tt);
            const float gh = terrainHeightAt(tc, static_cast<float>(q.x), static_cast<float>(q.y));
            if (!std::isfinite(gh)) return f;
            if (gh <= lowLevel)
            {
                const float span = prevH - gh;
                const float k = span > 1e-6f ? (prevH - lowLevel) / span : 1.0f;
                f.at = p + down * static_cast<double>(prevT + (tt - prevT) * std::clamp(k, 0.0f, 1.0f));
                f.y  = lift;
                return f;
            }
            prevT = tt; prevH = gh;
            if (tt >= kFallMaxRun)
            {
                f.at = q;
                f.y  = std::max(lift, gh);       // the ground never comes down this far: land on it
                return f;
            }
        }
    }
}

std::vector<uint16_t> fallUppers(const TerrainComponent& tc)
{
    std::vector<uint16_t> out;
    const Field& f = tc.water;
    if (!f.allocated() || f.bodies.size() < 2) return out;
    std::unordered_map<uint16_t, float> level;
    for (const Body& b : f.bodies) level[b.id] = b.level;
    const int res = static_cast<int>(f.res);
    const auto look = [&](size_t a, size_t b) {
        const uint16_t oa = f.owner[a], ob = f.owner[b];
        if (oa == ob || oa == kNoBody || ob == kNoBody) return;
        if (f.coverage[a] < kWet || f.coverage[b] < kWet) return;
        const auto la = level.find(oa), lb = level.find(ob);
        if (la == level.end() || lb == level.end()) return;
        if (la->second - lb->second >= kFallMinDrop) out.push_back(oa);
        else if (lb->second - la->second >= kFallMinDrop) out.push_back(ob);
    };
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x)
        {
            const size_t i = static_cast<size_t>(z) * res + x;
            if (x + 1 < res) look(i, i + 1);
            if (z + 1 < res) look(i, i + res);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool buildFalls(const TerrainComponent& tc, uint16_t upper, const Lattice& lattice, const CellRect& cells,
                float level, const SurfaceOptions& opt, SurfaceMesh& out, glm::vec2& flow)
{
    out = SurfaceMesh{};
    out.level = level;
    out.cells = cells;
    flow = glm::vec2(0.0f);
    if (lattice.empty() || !tc.water.allocated() || !hasArea(tc)) return false;

    const Field& f = tc.water;
    const glm::vec2 cs = cellSize(tc);
    const double probeStep = std::min<double>(cs.x, cs.y), probeNear = 0.75 * probeStep;
    constexpr int kProbeCells = 8;
    const float step = 0.25f * std::min(cs.x, cs.y);

    struct Piece { glm::dvec2 a, b; glm::dvec2 n; float low; };      // n: unit, out of the water
    std::vector<Piece> pieces;
    for (const ShoreSegment& s : shoreSegments(lattice))
    {
        const glm::dvec2 d = s.b - s.a;
        const double len = glm::length(d);
        if (!(len > 1e-9)) continue;
        // Which side is the sheet: shoreSegments does not orient a cell's single crossing (only a saddle's),
        // so ask the sheet's own inside test one sliver to either side. Both or neither: not an edge to hang
        // a ribbon from (a degenerate sliver in a corner).
        glm::dvec2 n(d.y / len, -d.x / len);
        const glm::dvec2 mid = (s.a + s.b) * 0.5;
        const double sliver = 0.2 * std::min<double>(cs.x, cs.y);
        const bool inFront = insideSheet(lattice, mid.x + n.x * sliver, mid.y + n.y * sliver);
        const bool inBack  = insideSheet(lattice, mid.x - n.x * sliver, mid.y - n.y * sliver);
        if (inFront == inBack) continue;
        if (inFront) n = -n;                 // n now points out of the water
        // The edge of the sheet is where the upper body's coverage falls to half, which a brush's soft rim
        // puts a few cells short of the neighbour's solid cells: walk outward past empty cells and the upper
        // body's own faint ones, and the first cell of ANOTHER body (at any coverage: the rim is the body's
        // too) is the neighbour.
        for (int k = 0; k < kProbeCells; ++k)
        {
            const glm::dvec2 q = mid + n * (probeNear + k * probeStep);
            const int ci = cellIndexAt(tc, q.x, q.y);
            if (ci < 0) break;
            const uint16_t owner = f.owner[static_cast<size_t>(ci)];
            if (owner == kNoBody || owner == upper) continue;
            const Body* lower = f.findBody(owner);
            if (!lower || !(level - lower->level >= kFallMinDrop)) break;
            pieces.push_back({ s.a, s.b, n, lower->level });
            break;
        }
    }
    if (pieces.empty()) return true;

    // Feet are keyed by the edge point and the level they land on, so two pieces that share a lattice
    // crossing (bit for bit) share the foot, and the ribbon is watertight along the edge.
    std::map<std::tuple<double, double, float>, Foot> feet;
    const auto footAt = [&](const glm::dvec2& p, const glm::dvec2& n, float low) -> const Foot& {
        const auto key = std::make_tuple(p.x, p.y, low);
        auto it = feet.find(key);
        if (it == feet.end()) it = feet.emplace(key, footOf(tc, p, n, low, step)).first;
        return it->second;
    };

    // Positions first (the entity stands at the middle of their box), then the mesh.
    struct Quad { glm::dvec2 ta, tb; Foot fa, fb; };
    std::vector<Quad> quads;
    glm::dvec2 lo(1e300), hi(-1e300);
    glm::dvec2 flowSum(0.0);
    const auto grow = [&](const glm::dvec2& p) { lo = glm::min(lo, p); hi = glm::max(hi, p); };
    for (const Piece& pc : pieces)
    {
        const double len = glm::length(pc.b - pc.a);
        const glm::dvec2& n = pc.n;
        Quad q;
        q.ta = pc.a; q.tb = pc.b;
        q.fa = footAt(pc.a, n, pc.low);
        q.fb = footAt(pc.b, n, pc.low);
        if (!q.fa.ok || !q.fb.ok) continue;
        grow(q.ta); grow(q.tb);
        grow(q.fa.at + q.fa.dir * static_cast<double>(kFallApron));
        grow(q.fb.at + q.fb.dir * static_cast<double>(kFallApron));
        grow(q.fa.at); grow(q.fb.at);
        flowSum += (q.fa.dir + q.fb.dir) * (0.5 * len);
        quads.push_back(q);
    }
    if (quads.empty()) return true;

    const glm::dvec2 center = (lo + hi) * 0.5;
    out.center = glm::vec2(static_cast<float>(center.x), static_cast<float>(center.y));

    StaticMeshAsset& m = out.mesh;
    m.type = HE::AssetType::StaticMesh;
    m.name = "water_fall";
    m.path = "mem://water_fall";
    const double invTile = 1.0 / std::max(1e-6f, opt.uvMetersPerTile);
    float bmin[3] = {  1e30f,  1e30f,  1e30f };
    float bmax[3] = { -1e30f, -1e30f, -1e30f };

    // One vertex: terrain-local XZ, height in terrain-local Y, foam e (the soft shore's shore proximity,
    // read back by the material off the normal's tilt, exactly as on the sheet).
    const auto vertex = [&](const glm::dvec2& p, float y, float e) {
        const float x = static_cast<float>(p.x - center.x), z = static_cast<float>(p.y - center.y);
        const float yl = y - level;
        m.vertices.push_back(x); m.vertices.push_back(yl); m.vertices.push_back(z);
        m.normals.push_back(e * HE::water::kShoreNormalGain * opt.shoreTiltScale);
        m.normals.push_back(1.0f);
        m.normals.push_back(0.0f);
        m.uvs.push_back(static_cast<float>((p.x + opt.uvOrigin.x) * invTile));
        m.uvs.push_back(static_cast<float>((p.y + opt.uvOrigin.y) * invTile));
        bmin[0] = std::min(bmin[0], x);  bmax[0] = std::max(bmax[0], x);
        bmin[1] = std::min(bmin[1], yl); bmax[1] = std::max(bmax[1], yl);
        bmin[2] = std::min(bmin[2], z);  bmax[2] = std::max(bmax[2], z);
        return static_cast<uint32_t>(m.vertices.size() / 3 - 1);
    };
    const auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {   // a-b along the top, c-d along the bottom
        m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
    };

    for (const Quad& q : quads)
    {
        const float eA = shoreProximityAt(lattice, q.ta.x, q.ta.y, opt.shoreOvershoot);
        const float eB = shoreProximityAt(lattice, q.tb.x, q.tb.y, opt.shoreOvershoot);
        const uint32_t ta = vertex(q.ta, level, eA), tb = vertex(q.tb, level, eB);
        const uint32_t ba = vertex(q.fa.at, q.fa.y, 1.0f), bb = vertex(q.fb.at, q.fb.y, 1.0f);
        quad(ta, tb, ba, bb);
        // The apron goes on from the foot, flat, the foam fading into the lower sheet.
        const uint32_t pa = vertex(q.fa.at + q.fa.dir * static_cast<double>(kFallApron), q.fa.y, 0.0f);
        const uint32_t pb = vertex(q.fb.at + q.fb.dir * static_cast<double>(kFallApron), q.fb.y, 0.0f);
        quad(ba, bb, pa, pb);
    }

    // A flat strip would have no volume; a hair keeps culling and picking from throwing it out.
    bmin[1] -= 0.05f; bmax[1] += 0.05f;
    std::memcpy(m.boundsMin, bmin, sizeof(bmin));
    std::memcpy(m.boundsMax, bmax, sizeof(bmax));
    out.triangles = static_cast<uint32_t>(m.indices.size() / 3);
    out.contours  = static_cast<uint32_t>(quads.size());     // ribbons, not contours: the diagnostic count

    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &level, sizeof(level));
    h = fnv(h, m.vertices.data(), m.vertices.size() * sizeof(float));
    h = fnv(h, m.normals.data(), m.normals.size() * sizeof(float));
    h = fnv(h, &out.center, sizeof(out.center));
    out.hash = h;

    const double fl = glm::length(flowSum);
    if (fl > 1e-9) flow = glm::vec2(static_cast<float>(flowSum.x / fl), static_cast<float>(flowSum.y / fl)) * kFallSpeed;
    return true;
}

} // namespace HE::water
