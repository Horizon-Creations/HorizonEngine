#include "HorizonScene/WaterBrush.h"
#include "HorizonScene/TerrainMeshGenerator.h"   // terrainHeightAt
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace HE::water::brush
{

namespace
{
    bool hasArea(const TerrainComponent& tc) { return tc.sizeX > 0.0f && tc.sizeZ > 0.0f; }

    // The terrain vertices a dab of reach `outer` at (x, z) can move, with their
    // heights before it — the rect TerrainSculpt::lowerToFloor walks.
    struct Region
    {
        int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        std::vector<float> heights;   // (x1 − x0 + 1) · (z1 − z0 + 1), row-major
        bool empty() const { return heights.empty(); }
    };

    Region saveRegion(TerrainComponent& tc, float x, float z, float outer)
    {
        Region r;
        if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(outer) || outer <= 0.0f)
            return r;
        TerrainSculpt::ensureHeights(tc);    // what lowerToFloor does first; a no-op for it afterwards
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        r.x0 = std::max(0, static_cast<int>(std::floor((x - outer + halfX) / stepX)));
        r.x1 = std::min(res - 1, static_cast<int>(std::ceil((x + outer + halfX) / stepX)));
        r.z0 = std::max(0, static_cast<int>(std::floor((z - outer + halfZ) / stepZ)));
        r.z1 = std::min(res - 1, static_cast<int>(std::ceil((z + outer + halfZ) / stepZ)));
        if (r.x0 > r.x1 || r.z0 > r.z1) return Region{};
        for (int zi = r.z0; zi <= r.z1; ++zi)
            for (int xi = r.x0; xi <= r.x1; ++xi)
                r.heights.push_back(tc.sculptHeights[static_cast<size_t>(zi) * res + xi]);
        return r;
    }

    // ── Connected water (Thema 232) ──────────────────────────────────────────────
    // The bed the brush digs belongs to the WATER, not to one body of it: two flat bodies of one level that touch are one
    // sheet (WaterMesh.h flatGroupLeads), so their bed is dug from the outline of both, and a stream that runs into a lake
    // is one water with it, so neither end of the dig treats the border between them as a shore (a ridge there).
    // The body and every flat body of its level that touches it (a stream is its own group).
    std::vector<uint16_t> digGroup(const TerrainComponent& tc, uint16_t body)
    {
        std::vector<uint16_t> ids{ body };
        const Body* b = tc.water.findBody(body);
        if (!b || b->stream) return ids;
        const auto leads = flatGroupLeads(tc);
        const auto it = leads.find(body);
        const uint16_t lead = it != leads.end() ? it->second : body;
        ids.clear();
        ids.push_back(lead);
        for (const auto& kv : leads) if (kv.second == lead) ids.push_back(kv.first);
        if (std::find(ids.begin(), ids.end(), body) == ids.end()) ids.push_back(body);
        return ids;
    }

    // The bodies other than `ids` with a wet cell next to a wet cell of `ids`.
    std::vector<uint16_t> touchingBodies(const TerrainComponent& tc, const std::vector<uint16_t>& ids)
    {
        std::vector<uint16_t> out;
        const Field& f = tc.water;
        if (!f.allocated()) return out;
        const int res = static_cast<int>(f.res);
        auto mine = [&](uint16_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
        auto note = [&](uint16_t id) { if (id != kNoBody && !mine(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id); };
        for (int z = 0; z < res; ++z)
            for (int x = 0; x < res; ++x)
            {
                const size_t i = static_cast<size_t>(z) * res + x;
                if (f.owner[i] == kNoBody || f.coverage[i] < kWet) continue;
                const bool m = mine(f.owner[i]);
                for (const size_t j : { x + 1 < res ? i + 1 : i, z + 1 < res ? i + res : i })
                {
                    if (j == i || f.owner[j] == kNoBody || f.coverage[j] < kWet || f.owner[j] == f.owner[i]) continue;
                    if (m) note(f.owner[j]); else if (mine(f.owner[j])) note(f.owner[i]);
                }
            }
        return out;
    }

    bool buildGroupLattice(const TerrainComponent& tc, const std::vector<uint16_t>& ids, const CellRect& cells, Lattice& out,
                           const ShoreClip& clip)
    {
        return ids.size() > 1 ? buildLattice(tc, ids, cells, out, clip) : buildLattice(tc, ids.empty() ? kNoBody : ids[0], cells, out, clip);
    }

    // Whether any water other than `ids`' is at (x, z): ground there is that water's, not this dig's or bank's.
    bool otherWaterAt(const TerrainComponent& tc, const std::vector<uint16_t>& ids, double x, double z)
    {
        const uint16_t o = bodyAt(tc, static_cast<float>(x), static_cast<float>(z));
        return o != kNoBody && std::find(ids.begin(), ids.end(), o) == ids.end();
    }

    // Put back every vertex of `was` the dab lowered that `body`'s sheet does not
    // cover — the sheet as the mesh builds it (WaterMesh.h: the lattice of the
    // body's own cells, marching squares, the field's shore clipping at the body's
    // level), asked point by point with insideSheet. Putting a vertex back can
    // raise the ground under a lattice corner and take the sheet off a neighbour,
    // so it repeats until nothing moves (it only ever puts back, so it ends).
    // Returns the vertices still lowered.
    uint32_t keepUnderSheet(TerrainComponent& tc, uint16_t body, float level, const Region& was)
    {
        if (was.empty()) return 0;
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const int   w = was.x1 - was.x0 + 1;
        auto vx = [&](int xi) { return -halfX + static_cast<float>(xi) * stepX; };
        auto vz = [&](int zi) { return -halfZ + static_cast<float>(zi) * stepZ; };
        auto old = [&](int xi, int zi) { return was.heights[static_cast<size_t>(zi - was.z0) * w + (xi - was.x0)]; };
        auto cur = [&](int xi, int zi) -> float& { return tc.sculptHeights[static_cast<size_t>(zi) * res + xi]; };

        const ShoreClip clip{ tc.water.clipToGround, level, tc.water.shoreOvershoot };
        const CellRect cells = cellRect(tc, vx(was.x0), vz(was.z0), vx(was.x1), vz(was.z1));
        for (bool moved = true; moved; )
        {
            moved = false;
            Lattice lattice;
            const bool sheet = cells.valid() && buildGroupLattice(tc, digGroup(tc, body), cells, lattice, clip);
            for (int zi = was.z0; zi <= was.z1; ++zi)
                for (int xi = was.x0; xi <= was.x1; ++xi)
                {
                    float& h = cur(xi, zi);
                    if (h == old(xi, zi)) continue;
                    if (sheet && insideSheet(lattice, vx(xi), vz(zi))) continue;
                    h = old(xi, zi);
                    moved = true;
                }
        }
        uint32_t kept = 0;
        for (int zi = was.z0; zi <= was.z1; ++zi)
            for (int xi = was.x0; xi <= was.x1; ++xi)
                if (cur(xi, zi) != old(xi, zi)) ++kept;
        return kept;
    }

    // The bank (Thema 188 Schritt 4). the dig (digBed) leaves the ground next to the
    // water where it was, so on rising ground the pit ends in a wall as high as
    // the ground stood above the level (4 m at the end of a 50 m stroke up an 8 %
    // slope), its top a staircase of 1 m vertices against a shore drawn through
    // 4 m water cells. Ground outside the sheet that stands above
    //     bank(d) = foot + kBankSlope · d / (1 − d / reach)
    // is lowered to it, d = the distance to the shoreline (shoreSegments: the
    // contour itself, not the nearest vertex under it, so the staircase is gone).
    // foot = just above where the shore clipping would put the sheet onto the
    // ground (level + overshoot): the bank never sinks below the water's reach, so
    // it is dry land and never a pit, and the sheet never spreads onto it. At the
    // waterline it rises kBankSlope per metre, steepening to vertical at `reach`,
    // so a wall of any height meets the old ground without a step and nothing
    // beyond `reach` moves. A target set by the shore, never a share of the height
    // it lowers from: a dab repeated at one spot changes nothing more.
    constexpr float kBankSlope = 0.5f;    // rise per metre at the waterline
    constexpr float kBankLip   = 0.01f;   // metres above the shore clipping's limit
    constexpr int   kBankPasses = 3;      // see the shore clipping's crossing below
    constexpr float kBankTolerance = 0.001f;   // metres a pass must lower a vertex by to count

    double segmentDistance(const glm::dvec2& p, const ShoreSegment& s)
    {
        const glm::dvec2 ab = s.b - s.a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - s.a, ab) / len2, 0.0, 1.0) : 0.0;
        return glm::length(p - (s.a + ab * t));
    }

    // The distance to the shore at every vertex of a box of the height grid
    // (vertices fx0..fx1 × fz0..fz1), as a closest-segment field: exact next to each
    // segment, then handed on through two sweeps over the 8 neighbours, each vertex
    // measuring itself against the segment its neighbour found (O(vertices) per
    // pass, where a search per vertex cost hundreds of segment tests on a fine
    // grid). `nearest` is -1 where no segment was reached.
    struct ShoreField
    {
        int fx0 = 0, fz0 = 0, fw = 0, fh = 0;
        double halfX = 0, halfZ = 0, stepX = 1, stepZ = 1;
        std::vector<int32_t> nearest;
        std::vector<double>  best;
        size_t at(int i, int j) const { return static_cast<size_t>(j) * fw + i; }
        glm::dvec2 point(int i, int j) const
        {
            return glm::dvec2(-halfX + static_cast<double>(fx0 + i) * stepX, -halfZ + static_cast<double>(fz0 + j) * stepZ);
        }
    };

    ShoreField shoreField(const std::vector<ShoreSegment>& shore, int fx0, int fx1, int fz0, int fz1,
                          double halfX, double halfZ, double stepX, double stepZ)
    {
        ShoreField f;
        f.fx0 = fx0; f.fz0 = fz0; f.fw = fx1 - fx0 + 1; f.fh = fz1 - fz0 + 1;
        f.halfX = halfX; f.halfZ = halfZ; f.stepX = stepX; f.stepZ = stepZ;
        const int fw = f.fw, fh = f.fh;
        f.nearest.assign(static_cast<size_t>(fw) * fh, -1);
        f.best.assign(static_cast<size_t>(fw) * fh, std::numeric_limits<double>::infinity());
        auto offer = [&](int i, int j, int32_t s)
        {
            if (s < 0) return;
            const double d = segmentDistance(f.point(i, j), shore[static_cast<size_t>(s)]);
            const size_t k = f.at(i, j);
            if (d < f.best[k]) { f.best[k] = d; f.nearest[k] = s; }
        };
        for (int32_t s = 0; s < static_cast<int32_t>(shore.size()); ++s)
        {
            const glm::dvec2 a = glm::min(shore[s].a, shore[s].b), b = glm::max(shore[s].a, shore[s].b);
            const int i0 = std::max(0, static_cast<int>(std::floor((a.x + halfX) / stepX)) - 1 - fx0);
            const int i1 = std::min(fw - 1, static_cast<int>(std::ceil((b.x + halfX) / stepX)) + 1 - fx0);
            const int j0 = std::max(0, static_cast<int>(std::floor((a.y + halfZ) / stepZ)) - 1 - fz0);
            const int j1 = std::min(fh - 1, static_cast<int>(std::ceil((b.y + halfZ) / stepZ)) + 1 - fz0);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) offer(i, j, s);
        }
        auto from = [&](int i, int j, int ni, int nj)
        {
            if (ni >= 0 && ni < fw && nj >= 0 && nj < fh) offer(i, j, f.nearest[f.at(ni, nj)]);
        };
        for (int j = 0; j < fh; ++j)
        {
            for (int i = 0; i < fw; ++i)
            { from(i, j, i - 1, j); from(i, j, i - 1, j - 1); from(i, j, i, j - 1); from(i, j, i + 1, j - 1); }
            for (int i = fw - 1; i >= 0; --i) from(i, j, i + 1, j);
        }
        for (int j = fh - 1; j >= 0; --j)
        {
            for (int i = fw - 1; i >= 0; --i)
            { from(i, j, i + 1, j); from(i, j, i + 1, j + 1); from(i, j, i, j + 1); from(i, j, i - 1, j + 1); }
            for (int i = 0; i < fw; ++i) from(i, j, i - 1, j);
        }
        return f;
    }

    // Returns the vertices lowered.
    uint32_t shapeBank(TerrainComponent& tc, uint16_t body, float level, float x, float z, float outer)
    {
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res || !tc.water.allocated()) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const float reach = std::max(outer, 2.0f * std::max(stepX, stepZ));
        const float foot  = level + (tc.water.clipToGround ? tc.water.shoreOvershoot : 0.0f) + kBankLip;

        // The vertices within reach of the shore this dab can have moved, and a
        // lattice a reach wider, so every piece of shore they can see is on it.
        const float span = outer + reach;
        const int x0 = std::max(0, static_cast<int>(std::floor((x - span + halfX) / stepX)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((x + span + halfX) / stepX)));
        const int z0 = std::max(0, static_cast<int>(std::floor((z - span + halfZ) / stepZ)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((z + span + halfZ) / stepZ)));
        if (x0 > x1 || z0 > z1) return 0;
        const CellRect cells = cellRect(tc, x - span - reach, z - span - reach, x + span + reach, z + span + reach);
        if (!cells.valid()) return 0;
        const int w = x1 - x0 + 1;
        std::vector<float> was;
        was.reserve(static_cast<size_t>(w) * (z1 - z0 + 1));
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
                was.push_back(tc.sculptHeights[static_cast<size_t>(zi) * res + xi]);

        // Where the sheet's edge is the shore clipping (the cells are wet further
        // out, the ground stood too high), the edge sits where the ground's depth
        // under the water crosses zero along a lattice edge — and lowering the
        // bank's corner moves that crossing outward. Measured against the shore
        // before it moved, the bank stands too high (one pass left the first
        // vertex off the water 0.83 m up instead of 0.56), so it is shaped again
        // from the new shore, up to kBankPasses times or until nothing moves by
        // more than kBankTolerance. The crossing settles geometrically; passes
        // past the third moved the shore's wall by less than a centimetre in the
        // tests and cost a third of the dab each. The sheet only ever grows, so
        // no pass can uncover the pit.
        for (int pass = 0; pass < kBankPasses; ++pass)
        {
            Lattice lattice;
            const std::vector<uint16_t> group = digGroup(tc, body);
            if (!buildGroupLattice(tc, group, cells, lattice, ShoreClip{ tc.water.clipToGround, level, tc.water.shoreOvershoot }))
                break;
            // Only the shore within this dab's own circle: a wall can only have been
            // left where this dab dug, and the shore of the rest of the body (a lake
            // the stroke continues, its own banks) is not this dab's to reshape. Two
            // water cells wider: a lattice corner averages the four cells around it,
            // so the contour of the cells the circle painted runs up to a cell past
            // it. Leaving pieces out only ever measures a vertex farther from the
            // water, which keeps its bank higher, and a distance to a fixed set of
            // segments is continuous, so it leaves no step either.
            std::vector<ShoreSegment> shore = shoreSegments(lattice);
            const glm::dvec2 centre(x, z);
            const double shoreReach = outer + 2.0 * std::max(lattice.cellW, lattice.cellH);
            shore.erase(std::remove_if(shore.begin(), shore.end(),
                                       [&](const ShoreSegment& s) { return segmentDistance(centre, s) > shoreReach; }),
                        shore.end());
            if (shore.empty()) break;

            // bank(d) < h only for d below radiusFor(h − foot) (bank(d) = h solved
            // for d), never past `reach`. The highest ground near the dab says how
            // far from the shore anything can move at all.
            auto radiusFor = [&](double above) { return above * reach / (kBankSlope * reach + above); };
            float highest = 0.0f;
            for (int zi = z0; zi <= z1; ++zi)
                for (int xi = x0; xi <= x1; ++xi)
                    highest = std::max(highest, tc.sculptHeights[static_cast<size_t>(zi) * res + xi] - foot);
            if (!(highest > 0.0f)) break;
            const double band = radiusFor(highest) + std::max(stepX, stepZ);
            glm::dvec2 lo(1e30), hi(-1e30);
            for (const ShoreSegment& s : shore) { lo = glm::min(lo, glm::min(s.a, s.b)); hi = glm::max(hi, glm::max(s.a, s.b)); }
            const int fx0 = std::max(x0, static_cast<int>(std::floor((lo.x - band + halfX) / stepX)));
            const int fx1 = std::min(x1, static_cast<int>(std::ceil((hi.x + band + halfX) / stepX)));
            const int fz0 = std::max(z0, static_cast<int>(std::floor((lo.y - band + halfZ) / stepZ)));
            const int fz1 = std::min(z1, static_cast<int>(std::ceil((hi.y + band + halfZ) / stepZ)));
            if (fx0 > fx1 || fz0 > fz1) break;

            const ShoreField field = shoreField(shore, fx0, fx1, fz0, fz1, halfX, halfZ, stepX, stepZ);
            const int fw = field.fw, fh = field.fh;
            const std::vector<int32_t>& nearest = field.nearest;
            const std::vector<double>&  best    = field.best;
            auto at = [&](int i, int j) { return field.at(i, j); };
            auto point = [&](int i, int j) { return field.point(i, j); };

            bool moved = false;
            for (int j = 0; j < fh; ++j)
                for (int i = 0; i < fw; ++i)
                {
                    const size_t k = at(i, j);
                    if (nearest[k] < 0) continue;
                    float& h = tc.sculptHeights[static_cast<size_t>(fz0 + j) * res + (fx0 + i)];
                    const float above = h - foot;
                    if (!(above > 0.0f)) continue;
                    const double d = best[k];
                    if (!(d < radiusFor(above))) continue;
                    const float bank = foot + kBankSlope * static_cast<float>(d / (1.0 - d / reach));
                    if (!(bank < h - kBankTolerance)) continue;
                    const glm::dvec2 p = point(i, j);
                    if (insideSheet(lattice, p.x, p.y)) continue;   // under the water: the dig's, not the bank's
                    if (otherWaterAt(tc, group, p.x, p.y)) continue; // under a neighbouring water (a stream's channel): not this bank's
                    h = bank;
                    moved = true;
                }
            if (!moved) break;
        }

        uint32_t lowered = 0;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
            {
                if (tc.sculptHeights[static_cast<size_t>(zi) * res + xi] == was[static_cast<size_t>(zi - z0) * w + (xi - x0)])
                    continue;
                ++lowered;
                cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
            }
        if (lowered > 0)
            TerrainSculpt::markRegionDirty(tc,
                                           -halfX + static_cast<float>(cx0) * stepX - stepX,
                                           -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                                           -halfX + static_cast<float>(cx1) * stepX + stepX,
                                           -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
        return lowered;
    }

    // The bed (Thema 206 Schritt 9). Every vertex the body's sheet covers goes down
    // to
    //     level − depth,   depth = min(maxDepth, slope · d) · (1 + roughness · noise)
    // d = the distance to the shoreline (shoreSegments: the contour itself), so the
    // bed falls from the shore at `slope` metres per metre and is flat at maxDepth
    // from maxDepth / slope metres in — wherever the brush is, however many
    // strokes made the shore. A target set by the shore and the position, never a
    // share of the height it lowers from: a dab repeated at one spot digs nothing
    // more once it is there. Paced by `amount` (a vertex above the level goes to it
    // at once, or the shore clipping would take the sheet off it), never raised, and
    // only under the sheet (insideSheet, the lattice the mesh is built from), so
    // the pit is the water's shape and no dry ring is sunk below the level.
    // The noise is a function of the position alone (no random state): two equal
    // strokes dig equal beds, and it is a share of the depth, so it is nothing at
    // the shoreline and the shore keeps its line.
    constexpr float kMaxBedReach = 64.0f;   // metres past the dab the bed is brought up to date

    float hash01(int x, int z)
    {
        uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
    }
    float valueNoise(double px, double pz)
    {
        const double fx = std::floor(px), fz = std::floor(pz);
        const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
        float tx = static_cast<float>(px - fx), tz = static_cast<float>(pz - fz);
        tx = tx * tx * (3.0f - 2.0f * tx);
        tz = tz * tz * (3.0f - 2.0f * tz);
        const float a = hash01(ix, iz), b = hash01(ix + 1, iz), c = hash01(ix, iz + 1), d = hash01(ix + 1, iz + 1);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
    }
    // -1..1: a broad swell (≈ 7 m) and a finer one (≈ 2.5 m) on top.
    float bedNoise(double x, double z)
    {
        const float n = 0.65f * valueNoise(x / 7.0, z / 7.0) + 0.35f * valueNoise(x / 2.5 + 31.7, z / 2.5 - 17.3);
        return n * 2.0f - 1.0f;
    }

    // Returns the vertices lowered.
    uint32_t digBed(TerrainComponent& tc, uint16_t body, float level, float x, float z, float outer,
                    float maxDepth, float slope, float roughness, float amount)
    {
        if (!hasArea(tc)) return 0;
        TerrainSculpt::ensureHeights(tc);
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res || !tc.water.allocated()) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const float cap   = maxDepth / slope;
        const float reach = outer + std::min(cap, kMaxBedReach);
        const float span  = reach + 2.0f * std::max(stepX, stepZ);

        const int x0 = std::max(0, static_cast<int>(std::floor((x - reach + halfX) / stepX)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((x + reach + halfX) / stepX)));
        const int z0 = std::max(0, static_cast<int>(std::floor((z - reach + halfZ) / stepZ)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((z + reach + halfZ) / stepZ)));
        if (x0 > x1 || z0 > z1) return 0;
        // A lattice wider than the box by the cap: the shore a vertex measures
        // against can lie that far away.
        const CellRect cells = cellRect(tc, x - span - cap, z - span - cap, x + span + cap, z + span + cap);
        if (!cells.valid()) return 0;
        uint32_t lowered = 0;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        auto note = [&](int xi, int zi)
        {
            ++lowered;
            cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
            cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
        };
        const glm::dvec2 centre(x, z);

        // First the wet footprint (the cells alone, no shore clipping): ground above
        // the level inside it goes to the level at once. Left standing, it would
        // clip the very water that is to cover it, and the bed below could not see it.
        Lattice wet;
        {
            if (!buildGroupLattice(tc, digGroup(tc, body), cells, wet, ShoreClip{ false, level, 0.0f })) return 0;
            for (int zi = z0; zi <= z1; ++zi)
                for (int xi = x0; xi <= x1; ++xi)
                {
                    float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
                    if (!(h > level)) continue;
                    const glm::dvec2 p(-halfX + xi * static_cast<double>(stepX), -halfZ + zi * static_cast<double>(stepZ));
                    if (glm::length(p - centre) > reach || !insideSheet(wet, p.x, p.y)) continue;
                    h = level;
                    note(xi, zi);
                }
        }

        Lattice lattice;
        const std::vector<uint16_t> group = digGroup(tc, body);
        if (!buildGroupLattice(tc, group, cells, lattice, ShoreClip{ tc.water.clipToGround, level, tc.water.shoreOvershoot }))
            return lowered;
        // The distance is measured to the footprint's contour, not to the clipped
        // sheet's edge: that edge sits where the ground crosses the level and moves
        // outward as the bed below it is lowered, which would move every target
        // with it, dab after dab. The footprint is the intent and holds still.
        // A stream that runs into this water is one water with it: the border between them is no shore, so the bed does
        // not rise there into a ridge (the stream digs its own channel, see digChannel).
        std::vector<uint16_t> outlineIds = group;
        for (const uint16_t t : touchingBodies(tc, group))
            if (const Body* tb = tc.water.findBody(t); tb && tb->stream) outlineIds.push_back(t);
        Lattice outline;
        const bool joined = outlineIds.size() > group.size() && buildGroupLattice(tc, outlineIds, cells, outline, ShoreClip{ false, level, 0.0f });
        const std::vector<ShoreSegment> shore = shoreSegments(joined ? outline : wet);
        if (shore.empty()) return lowered;
        const ShoreField field = shoreField(shore, x0, x1, z0, z1, halfX, halfZ, stepX, stepZ);

        for (int j = 0; j < field.fh; ++j)
            for (int i = 0; i < field.fw; ++i)
            {
                const size_t k = field.at(i, j);
                if (field.nearest[k] < 0) continue;
                const glm::dvec2 p = field.point(i, j);
                if (glm::length(p - centre) > reach) continue;
                if (!insideSheet(lattice, p.x, p.y)) continue;
                if (otherWaterAt(tc, group, p.x, p.y)) continue;      // a stream's channel is the stream's
                const double base = std::min(static_cast<double>(maxDepth), static_cast<double>(slope) * field.best[k]);
                const float depth = std::max(0.0f, static_cast<float>(base) * (1.0f + roughness * bedNoise(p.x, p.y)));
                const float target = level - depth;
                const int xi = x0 + i, zi = z0 + j;
                float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
                if (!(target < h)) continue;
                h += (target - h) * amount;
                note(xi, zi);
            }
        if (lowered > 0)
            TerrainSculpt::markRegionDirty(tc,
                                           -halfX + static_cast<float>(cx0) * stepX - stepX,
                                           -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                                           -halfX + static_cast<float>(cx1) * stepX + stepX,
                                           -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
        return lowered;
    }

    // What the brush has moved at vertex (xi, zi), summed over every body's patch
    // (lowered ground is negative): the ground before the brush is the height now
    // minus this. A patch the eraser closed or a body that is gone no longer counts,
    // and ground the user sculpted since is "before" too.
    float brushMovedAt(const TerrainComponent& tc, int xi, int zi)
    {
        float sum = 0.0f;
        for (const Body& b : tc.water.bodies)
        {
            const GroundPatch& gp = b.brushPatch;
            if (gp.empty() || !patchFits(gp, tc.resolution)) continue;
            if (xi < gp.x0 || xi >= gp.x0 + gp.w || zi < gp.z0 || zi >= gp.z0 + gp.h) continue;
            sum += gp.delta[static_cast<size_t>(zi - gp.z0) * gp.w + (xi - gp.x0)];
        }
        return sum;
    }

    // The channel of a stream (Thema 232 Schritt 12, see WaterBrush.h): every vertex
    // inside the outline of the union of `bodies` (the stroke's steps) and within
    // `reach` of the dab goes down to
    //     (height − brushMovedAt) − min(maxDepth, slope · d) · (1 + roughness · noise),
    // d = the distance to that outline. Nothing is cut to a level, no bank is shaped:
    // the channel is nothing at the outline. Returns the vertices lowered.
    uint32_t digChannel(TerrainComponent& tc, const std::vector<uint16_t>& bodies, float x, float z, float outer,
                        float maxDepth, float slope, float roughness, float amount)
    {
        if (!hasArea(tc) || bodies.empty()) return 0;
        TerrainSculpt::ensureHeights(tc);
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res || !tc.water.allocated()) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const float cap   = maxDepth / slope;
        const float reach = outer + std::min(cap, kMaxBedReach);
        const float span  = reach + 2.0f * std::max(stepX, stepZ);
        const int x0 = std::max(0, static_cast<int>(std::floor((x - reach + halfX) / stepX)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((x + reach + halfX) / stepX)));
        const int z0 = std::max(0, static_cast<int>(std::floor((z - reach + halfZ) / stepZ)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((z + reach + halfZ) / stepZ)));
        if (x0 > x1 || z0 > z1) return 0;
        const CellRect cells = cellRect(tc, x - span - cap, z - span - cap, x + span + cap, z + span + cap);
        if (!cells.valid()) return 0;
        // The stream's outline: the footprint of all its steps as one sheet, unclipped
        // (the hidden cells between two pools are the stream's too; the channel runs on
        // under them, which is where the band is).
        Lattice stream;
        if (!buildLattice(tc, bodies, cells, stream, ShoreClip{})) return 0;
        // The outline the depth is measured to is the stream's together with the water it runs into (a lake it
        // ends in): the border between them is no bank, so the channel runs on at its full depth up to the lake.
        std::vector<uint16_t> outlineIds = bodies;
        for (const uint16_t t : touchingBodies(tc, bodies)) outlineIds.push_back(t);
        Lattice outline;
        const bool joined = outlineIds.size() > bodies.size() && buildLattice(tc, outlineIds, cells, outline, ShoreClip{});
        const std::vector<ShoreSegment> shore = shoreSegments(joined ? outline : stream);
        if (shore.empty()) return 0;
        const ShoreField field = shoreField(shore, x0, x1, z0, z1, halfX, halfZ, stepX, stepZ);
        const glm::dvec2 centre(x, z);
        uint32_t lowered = 0;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        for (int j = 0; j < field.fh; ++j)
            for (int i = 0; i < field.fw; ++i)
            {
                const size_t k = field.at(i, j);
                if (field.nearest[k] < 0) continue;
                const glm::dvec2 p = field.point(i, j);
                if (glm::length(p - centre) > reach) continue;
                if (!insideSheet(stream, p.x, p.y)) continue;
                const double base = std::min(static_cast<double>(maxDepth), static_cast<double>(slope) * field.best[k]);
                const float depth = std::max(0.0f, static_cast<float>(base) * (1.0f + roughness * bedNoise(p.x, p.y)));
                const int xi = x0 + i, zi = z0 + j;
                float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
                const float target = (h - brushMovedAt(tc, xi, zi)) - depth;
                if (!(target < h)) continue;
                h += (target - h) * amount;
                ++lowered;
                cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
            }
        if (lowered > 0)
            TerrainSculpt::markRegionDirty(tc,
                                           -halfX + static_cast<float>(cx0) * stepX - stepX,
                                           -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                                           -halfX + static_cast<float>(cx1) * stepX + stepX,
                                           -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
        return lowered;
    }

    // Add what a dab changed in `was`'s rect (height now minus height then) to the
    // body's patch, growing the patch's rect to hold it.
    void recordPatch(TerrainComponent& tc, uint16_t body, const Region& was)
    {
        Body* b = tc.water.findBody(body);
        if (!b || was.empty()) return;
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return;
        const int w = was.x1 - was.x0 + 1;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        for (int zi = was.z0; zi <= was.z1; ++zi)
            for (int xi = was.x0; xi <= was.x1; ++xi)
                if (tc.sculptHeights[static_cast<size_t>(zi) * res + xi] != was.heights[static_cast<size_t>(zi - was.z0) * w + (xi - was.x0)])
                {
                    cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                    cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
                }
        if (cx1 < 0) return;
        GroundPatch& gp = b->brushPatch;
        const bool have = !gp.empty() && patchFits(gp, tc.resolution);
        int nx0 = cx0, nz0 = cz0, nx1 = cx1, nz1 = cz1;
        if (have)
        {
            nx0 = std::min(nx0, gp.x0); nz0 = std::min(nz0, gp.z0);
            nx1 = std::max(nx1, gp.x0 + gp.w - 1); nz1 = std::max(nz1, gp.z0 + gp.h - 1);
        }
        GroundPatch m;
        m.res = tc.resolution; m.x0 = nx0; m.z0 = nz0; m.w = nx1 - nx0 + 1; m.h = nz1 - nz0 + 1;
        m.delta.assign(static_cast<size_t>(m.w) * m.h, 0.0f);
        if (have)
            for (int j = 0; j < gp.h; ++j)
                for (int i = 0; i < gp.w; ++i)
                    m.delta[static_cast<size_t>(gp.z0 + j - nz0) * m.w + (gp.x0 + i - nx0)] = gp.delta[static_cast<size_t>(j) * gp.w + i];
        for (int zi = cz0; zi <= cz1; ++zi)
            for (int xi = cx0; xi <= cx1; ++xi)
                m.delta[static_cast<size_t>(zi - nz0) * m.w + (xi - nx0)] +=
                    tc.sculptHeights[static_cast<size_t>(zi) * res + xi] - was.heights[static_cast<size_t>(zi - was.z0) * w + (xi - was.x0)];
        gp = std::move(m);
    }

    // The eraser's Close Bed: under the circle, every vertex of `gp` (a body's patch)
    // whose water is gone (`id` no longer owns it) gets back `amount` · weight of what
    // the brush dug there. Returns the vertices raised.
    uint32_t closeBedAt(TerrainComponent& tc, uint16_t id, GroundPatch& gp,
                        float x, float z, float radius, float falloff, float amount)
    {
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res || !(amount > 0.0f)) return 0;
        if (gp.empty() || !patchFits(gp, tc.resolution)) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const float rad = std::max(0.0f, radius), fall = std::max(0.0f, falloff);
        const float outer = rad + fall;
        uint32_t raised = 0;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        const int x0 = std::max(gp.x0, static_cast<int>(std::floor((x - outer + halfX) / stepX)));
        const int x1 = std::min(gp.x0 + gp.w - 1, static_cast<int>(std::ceil((x + outer + halfX) / stepX)));
        const int z0 = std::max(gp.z0, static_cast<int>(std::floor((z - outer + halfZ) / stepZ)));
        const int z1 = std::min(gp.z0 + gp.h - 1, static_cast<int>(std::ceil((z + outer + halfZ) / stepZ)));
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
            {
                float& dl = gp.delta[static_cast<size_t>(zi - gp.z0) * gp.w + (xi - gp.x0)];
                if (!(dl < 0.0f)) continue;
                const float vx = -halfX + static_cast<float>(xi) * stepX, vz = -halfZ + static_cast<float>(zi) * stepZ;
                const float dist = std::hypot(vx - x, vz - z);
                if (dist > outer) continue;
                const float weight = dist <= rad ? 1.0f : 1.0f - (dist - rad) / std::max(fall, 1e-6f);
                if (!(weight > 0.0f)) continue;
                // Still under water, this body's or another's (a stream's channel is recorded on
                // whichever step was painting, under the cells of its neighbours): its bed stays.
                if (bodyAt(tc, vx, vz) != kNoBody) continue;
                const float give = dl * std::min(1.0f, amount * weight);   // negative
                tc.sculptHeights[static_cast<size_t>(zi) * res + xi] -= give;
                dl -= give;
                ++raised;
                cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
            }
        if (raised > 0)
            TerrainSculpt::markRegionDirty(tc,
                                           -halfX + static_cast<float>(cx0) * stepX - stepX,
                                           -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                                           -halfX + static_cast<float>(cx1) * stepX + stepX,
                                           -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
        return raised;
    }
}

glm::vec2 slopeFlow(const TerrainComponent& tc, uint16_t body)
{
    const Field& f = tc.water;
    if (body == kNoBody || !f.allocated() || !hasArea(tc)) return { 0.0f, 0.0f };
    const int res = static_cast<int>(f.res);
    const glm::vec2 cell = cellSize(tc);
    // A 256² field of one lake is 65k cells; a few thousand samples average the
    // slope as well, so a wide body is walked on a stride.
    const uint32_t wet = f.wetCells(body);
    if (wet == 0) return { 0.0f, 0.0f };
    const uint32_t stride = std::max(1u, static_cast<uint32_t>(std::sqrt(static_cast<float>(wet) / 4096.0f)));

    double gx = 0.0, gz = 0.0;
    uint32_t n = 0;
    for (int cz = 0; cz < res; cz += static_cast<int>(stride))
        for (int cx = 0; cx < res; cx += static_cast<int>(stride))
        {
            const size_t i = static_cast<size_t>(cz) * res + cx;
            if (f.owner[i] != body || f.coverage[i] < kWet) continue;
            const glm::vec2 c = cellCenter(tc, cx, cz);
            const float hx = terrainHeightAt(tc, c.x + cell.x, c.y) - terrainHeightAt(tc, c.x - cell.x, c.y);
            const float hz = terrainHeightAt(tc, c.x, c.y + cell.y) - terrainHeightAt(tc, c.x, c.y - cell.y);
            if (!std::isfinite(hx) || !std::isfinite(hz)) continue;
            gx += hx / (2.0f * cell.x);
            gz += hz / (2.0f * cell.y);
            ++n;
        }
    if (n == 0) return { 0.0f, 0.0f };
    const float mx = static_cast<float>(gx / n), mz = static_cast<float>(gz / n);
    const float mag = std::sqrt(mx * mx + mz * mz);
    if (!(mag > 0.0f)) return { 0.0f, 0.0f };
    const float t = std::clamp((mag - kFlowStillSlope) / (kFlowFallSlope - kFlowStillSlope), 0.0f, 1.0f);
    const float speed = kFlowMaxSpeed * t * t * (3.0f - 2.0f * t);
    return { -mx / mag * speed, -mz / mag * speed };
}

Target targetAt(const TerrainComponent& tc, float x, float z, const Params& p)
{
    Target t;
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return t;
    if (!p.erase)
    {
        const uint16_t under = bodyAt(tc, x, z);
        if (const Body* b = tc.water.findBody(under))
        {
            t.body      = b->id;
            t.level     = b->level;
            t.continues = true;
            return t;
        }
    }
    const float level = p.levelFromGround ? terrainHeightAt(tc, x, z) + p.levelOffset : p.level;
    t.level = std::isfinite(level) ? level : 0.0f;
    return t;
}

void begin(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p)
{
    if (s.active) end(s, tc);
    s = Stroke{};
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return;

    s.erase = p.erase;
    if (p.erase)
    {
        s.active = true;
        return;
    }
    Target t = targetAt(tc, x, z, p);
    // Follow Slope paints a stream of its own even when pressed on water that is already there (a lake it starts in):
    // the stroke's dry cells beyond it become a stream that joins that water (WaterMesh.h joinReaches), and the
    // water it started in is left as it is. Continuing a flat body down a slope would hang a level sheet over the ground.
    if (p.followSlope && t.continues)
    {
        t.continues = false;
        t.body = kNoBody;
        const float ground = terrainHeightAt(tc, x, z);
        t.level = std::isfinite(ground) ? ground + lipDepth(p) : t.level;
    }
    s.level = t.level;
    if (t.continues)
        s.body = t.body;
    else
    {
        s.body = tc.water.createBody(t.level);
        s.created = s.body != kNoBody;
    }
    s.active = s.body != kNoBody;
    if (s.active) s.bodies.push_back(s.body);
    if (s.active && p.autoFlow) setFlowAuto(tc, s.body, true);
    // Follow Slope: a stroke is a stream, all the way (on level ground it stands still).
    if (s.active && p.followSlope && s.created)
    {
        setStream(tc, s.body, true, lipDepth(p));
        setFlowAuto(tc, s.body, true);
    }
}

float groundSlope(const TerrainComponent& tc, float x, float z)
{
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return 0.0f;
    const glm::vec2 cell = cellSize(tc);
    const float hx = terrainHeightAt(tc, x + cell.x, z) - terrainHeightAt(tc, x - cell.x, z);
    const float hz = terrainHeightAt(tc, x, z + cell.y) - terrainHeightAt(tc, x, z - cell.y);
    if (!std::isfinite(hx) || !std::isfinite(hz)) return 0.0f;
    const float gx = hx / (2.0f * cell.x), gz = hz / (2.0f * cell.y);
    return std::sqrt(gx * gx + gz * gz);
}

float lipDepth(const Params& p)
{
    if (!p.levelFromGround) return kStreamDepthFixedLevel;
    return std::isfinite(p.levelOffset) ? std::max(kMinLipDepth, p.levelOffset) : kStreamDepthFixedLevel;
}


Dab dab(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p)
{
    Dab d;
    if (!s.active || !hasArea(tc)) return d;

    if (s.erase)
    {
        // Nothing painted yet: removeCircle would allocate an all-dry grid just to
        // take nothing out of it, and that grid makes the scene non-pristine.
        if (!tc.water.allocated()) { d.ok = true; ++s.dabs; return d; }
        // removeCircle forgets a body it left without a cell, and its patch with it:
        // take the patches along first, close with them, give the survivors the rest.
        std::vector<std::pair<uint16_t, GroundPatch>> patches;
        if (p.closeBed)
            for (const Body& b : tc.water.bodies)
                if (!b.brushPatch.empty()) patches.emplace_back(b.id, b.brushPatch);
        const Result r = removeCircle(tc, x, z, p.radius, p.falloff, p.amount);
        d.ok = r.ok;
        d.cells = r.changed;
        for (auto& [id, patch] : patches)
        {
            // A body that is gone has no water left for the bed to stay under, and
            // nothing will ever come back to it: it is closed at full strength.
            const bool gone = tc.water.findBody(id) == nullptr;
            d.ground += closeBedAt(tc, id, patch, x, z, p.radius, p.falloff, gone ? 1.0f : p.amount);
            if (Body* b = tc.water.findBody(id)) b->brushPatch = std::move(patch);
        }
        ++s.dabs;
        return d;
    }

    // Undo (or anything else that replaced the field) can take the body away
    // mid-stroke; painting into a body that is gone would have no owner to give.
    if (!tc.water.findBody(s.body)) return d;
    const Result r = addCircle(tc, s.body, x, z, p.radius, p.falloff, p.amount);
    d.ok    = r.ok;
    d.cells = r.changed;
    if (std::isfinite(x) && std::isfinite(z)) { s.last = { x, z }; s.hasLast = true; }
    // A stream always lies in a channel it digs (Thema 232): its sheet is the ground lifted by its depth,
    // so on an undug slope the water would stand that far over the hillside. The channel is a little
    // deeper than the water and its banks are steep (kStreamBankSlope), so the water's surface stays at or
    // under the old ground from its edge inward. Dig Bed, when on, can make it deeper and the banks gentler.
    const Body* streamBody = tc.water.findBody(s.body);
    const bool dugStream = streamBody && streamBody->stream;
    if (dugStream || (p.dig && p.digDepth > 0.0f && std::isfinite(p.digDepth)))
    {
        // The bed follows the shore (Thema 206 Schritt 9, see digBed): the body keeps
        // the settings it was dug with. The reach is the brush's, plus the stretch of
        // bed the shore's new position can have changed.
        BrushBedParams want{ true, p.digDepth, p.bedSlope, p.bedRoughness };
        if (!p.dig || !(p.digDepth > 0.0f)) want = { true, 0.0f, 0.0f, 0.0f };
        if (dugStream)
        {
            want.maxDepth = std::max(want.maxDepth, kStreamChannelFactor * streamBody->streamDepth);
            want.slope    = std::max(want.slope, kStreamBankSlope);
        }
        const BrushBedParams bed = clampBrushBed(want);
        setBrushBed(tc, s.body, bed);
        const float outer = std::max(0.0f, p.radius) + std::max(0.0f, p.falloff);
        const float amount = std::clamp(std::isfinite(p.amount) ? p.amount : 0.0f, 0.0f, 1.0f);
        const float reach = outer + std::min(bed.maxDepth / bed.slope, kMaxBedReach);
        const float cellStep = std::max(tc.sizeX, tc.sizeZ) / static_cast<float>(std::max(2u, tc.resolution) - 1u);
        // Everything the dab can move, the bank (out to twice the brush) included:
        // what the eraser may give back later.
        const Region everything = saveRegion(tc, x, z, std::max(reach, 2.0f * outer) + 4.0f * cellStep);
        const Body* cur = tc.water.findBody(s.body);
        if (cur && cur->stream)
        {
            // A stream's bed is a channel dug from its outline (Thema 232): no bank, no cut to a level.
            // The sheet lies on the ground, so it settles into the channel by itself.
            std::vector<uint16_t> streams{ s.body };       // and the streams that run into this one: one channel
            for (const uint16_t t : touchingBodies(tc, streams))
                if (const Body* tb = tc.water.findBody(t); tb && tb->stream) streams.push_back(t);
            const uint32_t cut = digChannel(tc, streams, x, z, outer, bed.maxDepth, bed.slope, bed.roughness, amount);
            d.ok = true;
            d.ground = cut;
        }
        else
        {
            const Region was = saveRegion(tc, x, z, reach + 2.0f * cellStep);
            const uint32_t cut = digBed(tc, s.body, s.level, x, z, outer, bed.maxDepth, bed.slope, bed.roughness, amount);
            // The pit is the water's shape and never more (Thema 188): whatever the dig
            // lowered that the sheet, as the mesh builds it, does not cover goes back.
            const uint32_t dug = cut > 0 ? keepUnderSheet(tc, s.body, s.level, was) : 0u;
            d.ok = true;
            // Where that left a wall of ground above the water, it becomes a bank down
            // to the waterline (Thema 188 Schritt 4), dry land that never reaches the water.
            d.bank   = (dug > 0 || d.cells > 0) ? shapeBank(tc, s.body, s.level, x, z, outer) : 0u;
            d.ground = dug + d.bank;
        }
        recordPatch(tc, s.body, everything);
        if (d.ground > 0) ++tc.water.revision;      // the ground under the water moved: pools and fills are measured again
    }
    ++s.dabs;
    return d;
}

void end(Stroke& s, TerrainComponent& tc)
{
    // A press that landed nowhere (off the landscape, a brush of no size)
    // leaves a body with no cell and no source: nothing would ever clean it up.
    // The first body is the stroke's own only if it made it; every later one is. A later
    // body goes with no WET cell (kWet): rim cells alone draw no sheet.
    for (size_t k = 0; s.active && k < s.bodies.size(); ++k)
    {
        const uint16_t id = s.bodies[k];
        if ((k == 0 && !s.created) || !tc.water.findBody(id)) continue;
        const uint8_t needs = k == 0 ? uint8_t{ 1 } : kWet;
        bool owns = false;
        const Field& f = tc.water;
        if (f.allocated())
            for (size_t i = 0; i < f.owner.size() && !owns; ++i)
                owns = f.owner[i] == id && f.coverage[i] >= needs;
        if (!owns) removeBody(tc, id);
    }
    // The current follows the ground only for a body that asked for it (flowAuto);
    // for any other physics.flow is the user's number and stays as it is. Computed
    // once per stroke, from the cells the stroke left, not per dab.
    if (s.active && !s.erase)
        for (const uint16_t id : s.bodies)
            if (const Body* b = tc.water.findBody(id); b && b->flowAuto)
            {
                BodyPhysics ph = b->physics;
                ph.flow = slopeFlow(tc, id);
                setPhysics(tc, id, ph);
            }
    // A press that never wet a cell (or an eraser that dried the last one) leaves
    // an allocated all-dry grid behind, and a grid makes the scene non-pristine:
    // hand it back, so such a stroke costs the file nothing. Only when there is no
    // body at all; a lake keeps its (possibly dry) cells' grid for its spline.
    Field& f = tc.water;
    if (s.active && f.bodies.empty() && f.allocated() &&
        std::none_of(f.coverage.begin(), f.coverage.end(), [](uint8_t c) { return c != 0; }))
    {
        f.coverage.clear(); f.coverage.shrink_to_fit();
        f.owner.clear();    f.owner.shrink_to_fit();
    }
    s = Stroke{};
}

} // namespace HE::water::brush
