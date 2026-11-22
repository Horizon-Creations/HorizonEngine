#include "HorizonScene/TerrainGenerate.h"
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace TerrainGenerate
{

namespace
{
    // Weight of the rim blend at an offset (dx, dz) from the area's centre: 0 on
    // and outside the ellipse, easing up by smoothstep over `falloff` world units
    // measured inwards along the ray from the centre, 1 beyond that.
    //
    // With d the normalised elliptical distance, the rim on the ray through p
    // sits at |p| / d, so the distance left to it is |p| / d − |p|. For a circle
    // that is simply radius − r; for an ellipse it keeps the blend the same
    // width in metres all the way round, instead of squeezing it on the short
    // axis the way a blend in d would.
    //
    // Capped at the short radius: right at the centre the ray distance depends
    // on the direction (the short radius one way, the long one the other), so
    // uncapped a falloff wider than the short radius left the centre vertex at
    // half the height of its neighbours on the long axis. The cap makes it
    // continuous; the crest of such an ellipse is a level ridge along the long
    // axis, and the rim blend stays metre-exact wherever it is narrower.
    float rimWeight(float dx, float dz, float rx, float rz, float falloff)
    {
        const float ex = dx / rx, ez = dz / rz;
        const float d  = std::sqrt(ex * ex + ez * ez);
        if (d >= 1.0f) return 0.0f;
        if (falloff <= 0.0f) return 1.0f;

        const float shortR = std::min(rx, rz);
        float edge = shortR;   // the centre itself has no ray
        if (d > 1e-6f)
        {
            const float len = std::sqrt(dx * dx + dz * dz);
            edge = std::min(len / d - len, shortR);
        }
        const float t = std::clamp(edge / falloff, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float smooth01(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // Ridged multifractal over the same value noise as terrainFbm, 0..1: each
    // octave is folded at its middle, so its crest line is a sharp ridge, and an
    // octave only adds detail where the one before it was high (crests get
    // rougher, valley floors stay smooth). The coordinates are warped by two
    // low octaves first, or every ridge would run straight along the lattice.
    float ridgedRelief(int seed, float x, float z, int octaves, float frequency)
    {
        const float wx = terrainFbm(seed + 7919, x + 31.7f, z - 12.3f, 3, frequency * 0.5f, 2.0f, 0.5f) - 0.5f;
        const float wz = terrainFbm(seed + 104729, x - 5.1f, z + 47.9f, 3, frequency * 0.5f, 2.0f, 0.5f) - 0.5f;
        x += wx / frequency;
        z += wz / frequency;
        float sum = 0.0f, norm = 0.0f, amp = 1.0f, f = frequency, weight = 1.0f;
        for (int o = 0; o < std::clamp(octaves, 1, 12); ++o)
        {
            const float n = terrainFbm(seed + o * 101, x, z, 1, f, 2.0f, 0.5f);
            float r = 1.0f - std::fabs(2.0f * n - 1.0f);
            r *= r * weight;
            weight = std::clamp(r * 2.0f, 0.0f, 1.0f);
            sum += r * amp;
            norm += amp;
            amp *= 0.5f;
            f *= 2.0f;
        }
        return sum / norm;
    }

    // One step of a terrace: x in 0..1 across the step. p = 1 is the identity
    // (the slope as it was), a large p a level bench with a riser in its middle.
    float terraceStep(float x, float p)
    {
        return x < 0.5f ? 0.5f * std::pow(2.0f * x, p) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, p);
    }
}

Result mountain(TerrainComponent& tc, const Area& area, const Params& params)
{
    Result r;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return r;
    if (!(area.radiusX > 0.0f) || !(area.radiusZ > 0.0f)) return r;

    TerrainSculpt::ensureHeights(tc);
    const uint32_t res = tc.resolution;
    if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;

    r.ok = true;   // legal request from here on; it may still change nothing
    if (params.maxHeight == 0.0f) return r;

    const float halfX = tc.sizeX * 0.5f;
    const float halfZ = tc.sizeZ * 0.5f;
    const float stepX = tc.sizeX / static_cast<float>(res - 1);
    const float stepZ = tc.sizeZ / static_cast<float>(res - 1);

    const float rx = area.radiusX, rz = area.radiusZ;
    const float falloff   = std::max(0.0f, params.falloff);
    const float roughness = std::clamp(params.roughness, 0.0f, 1.0f);
    const float ridges    = std::clamp(params.ridges, 0.0f, 1.0f);
    // Noise coordinates are the offset in units of the larger radius, so
    // `frequency` counts features across the AREA, not across the terrain.
    const float noiseScale = 1.0f / std::max(rx, rz);

    // Vertex rect around the area's bounding box. Everything outside it has
    // weight 0 by construction, so a small mountain never walks the whole grid.
    auto toGrid = [](float v, float lo, float step) { return (v - lo) / step; };
    const int x0 = std::max(0, static_cast<int>(std::floor(toGrid(area.centerX - rx, -halfX, stepX))));
    const int x1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(area.centerX + rx, -halfX, stepX))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toGrid(area.centerZ - rz, -halfZ, stepZ))));
    const int z1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(area.centerZ + rz, -halfZ, stepZ))));
    if (x0 > x1 || z0 > z1) return r;

    // ── Pass 1: the raw shape, and its peak ─────────────────────────────────
    // Held until the peak is known, because maxHeight is a promise about the
    // tallest point of the formation — with roughness > 0 the noise decides
    // where that is and how high the unscaled shape gets.
    const int w = x1 - x0 + 1;
    const int hgt = z1 - z0 + 1;
    const auto at = [w, x0, z0](int xi, int zi) { return static_cast<size_t>(zi - z0) * w + (xi - x0); };
    std::vector<float> shape(static_cast<size_t>(w) * hgt, 0.0f);
    std::vector<float> rimW;   // the rim blend alone: what erosion may not pile material above
    const int erosionPasses = static_cast<int>(std::lround(std::clamp(params.erosion, 0.0f, 1.0f) * 32.0f));
    if (erosionPasses > 0 || params.paths > 0) rimW.assign(shape.size(), 0.0f);
    float shapeMax = 0.0f;
    for (int zi = z0; zi <= z1; ++zi)
    {
        const float dz = -halfZ + static_cast<float>(zi) * stepZ - area.centerZ;
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float dx = -halfX + static_cast<float>(xi) * stepX - area.centerX;
            const float rim = rimWeight(dx, dz, rx, rz, falloff);
            if (rim <= 0.0f) continue;

            float relief = 1.0f;
            if (roughness > 0.0f)
            {
                // Lacunarity and gain are the TerrainComponent defaults, so a
                // formation's relief reads like the seeded landscape around it.
                float n = terrainFbm(params.seed, dx * noiseScale, dz * noiseScale,
                                     params.octaves, params.frequency, 2.0f, 0.5f);
                if (ridges > 0.0f)
                    n += (ridgedRelief(params.seed, dx * noiseScale, dz * noiseScale, params.octaves, params.frequency) - n) * ridges;
                relief = (1.0f - roughness) + roughness * n;
            }
            const float s = rim * relief;
            shape[at(xi, zi)] = s;
            if (!rimW.empty()) rimW[at(xi, zi)] = rim;
            shapeMax = std::max(shapeMax, s);
        }
    }
    if (shapeMax <= 0.0f) return r;

    // ── Thermal erosion: what is steeper than scree slides down ─────────────
    // In metres (the shape times what it will be scaled by), or the same
    // setting would erode a tall mountain less than a low one. A vertex gives a
    // quarter of its excess over the talus slope to each lower neighbour inside
    // the area, all at once from the pass's start state; nothing is piled above
    // the rim blend, so the formation still meets the ground where it did.
    if (erosionPasses > 0)
    {
        const float metres = std::fabs(params.maxHeight) / shapeMax;
        const float talus = 0.84f;   // tan 40°
        std::vector<float> delta(shape.size());
        static constexpr int kDx[4] = { 1, -1, 0, 0 };
        static constexpr int kDz[4] = { 0, 0, 1, -1 };
        for (int pass = 0; pass < erosionPasses; ++pass)
        {
            std::fill(delta.begin(), delta.end(), 0.0f);
            for (int zi = z0; zi <= z1; ++zi)
                for (int xi = x0; xi <= x1; ++xi)
                {
                    const size_t c = at(xi, zi);
                    if (rimW[c] <= 0.0f) continue;
                    for (int k = 0; k < 4; ++k)
                    {
                        const int nx = xi + kDx[k], nz = zi + kDz[k];
                        if (nx < x0 || nx > x1 || nz < z0 || nz > z1) continue;
                        const size_t n = at(nx, nz);
                        if (rimW[n] <= 0.0f) continue;
                        const float dist = kDx[k] != 0 ? stepX : stepZ;
                        const float excess = (shape[c] - shape[n]) * metres - talus * dist;
                        if (excess <= 0.0f) continue;
                        const float move = 0.125f * excess / metres;
                        delta[c] -= move;
                        delta[n] += move;
                    }
                }
            for (size_t i = 0; i < shape.size(); ++i)
                if (rimW[i] > 0.0f) shape[i] = std::clamp(shape[i] + delta[i], 0.0f, rimW[i]);
        }
    }

    // ── Terraces: the height in steps ───────────────────────────────────────
    // On the normalised shape, so the lowest bench is the ground itself (the
    // formation still starts from nothing at the rim) and the top one the peak.
    if (params.terraces > 0)
    {
        shapeMax = 0.0f;
        for (float s : shape) shapeMax = std::max(shapeMax, s);
        if (shapeMax <= 0.0f) return r;
        const float steps = static_cast<float>(std::min(params.terraces, 256));
        const float sharp = std::clamp(params.terraceSharpness, 0.0f, 1.0f);
        const float p = 1.0f + sharp * sharp * 24.0f;
        for (float& s : shape)
        {
            if (s <= 0.0f) continue;
            const float u = s / shapeMax * steps;
            const float f = std::min(std::floor(u), steps - 1.0f);
            s = (f + terraceStep(u - f, p)) / steps * shapeMax;
        }
    }
    if (erosionPasses > 0 || params.terraces > 0)
    {
        shapeMax = 0.0f;
        for (float s : shape) shapeMax = std::max(shapeMax, s);
        if (shapeMax <= 0.0f) return r;
    }

    // ── Pass 2: add it, scaled so the peak is exactly maxHeight ─────────────
    const float scale = params.maxHeight / shapeMax;
    std::vector<float> before(shape.size());   // the ground as it was: what "changed" is measured against once paths cut into it
    float peak = 0.0f;
    for (int zi = z0; zi <= z1; ++zi)
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float s = shape[at(xi, zi)];
            float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
            before[at(xi, zi)] = h;
            if (s <= 0.0f) continue;
            const float add = s * scale;
            const float was = h;
            h += add;
            if (h == was) continue;
            if (std::fabs(add) > std::fabs(peak)) peak = add;
        }

    // ── Paths: routed from the summit, then cut into the ground ─────────────
    const int pathCount = std::clamp(params.paths, 0, 8);
    if (pathCount > 0 && w > 2 && hgt > 2)
    {
        const auto ground = [&](int xi, int zi) { return tc.sculptHeights[static_cast<size_t>(zi) * res + xi]; };
        // The search runs on every `stride`-th vertex: a large area would be a
        // million nodes, and a trail does not need the grid's last metre.
        const int stride = std::max(1, (std::max(w, hgt) + 95) / 96);
        // The summit: the formation's own highest vertex (its deepest, when it
        // digs). The nodes are laid so that it is one of them: a path ends ON
        // the peak, and cutting it in leaves the peak the height it was promised.
        int sx = -1, sz = -1;
        float best = 0.0f;
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
                if (shape[at(xi, zi)] > best) { best = shape[at(xi, zi)]; sx = xi; sz = zi; }
        const int ox = sx >= 0 ? (sx - x0) % stride : 0, oz = sz >= 0 ? (sz - z0) % stride : 0;
        const int nw = (w - 1 - ox) / stride + 1, nh = (hgt - 1 - oz) / stride + 1;
        const auto nodeX = [&](int i) { return x0 + ox + i * stride; };
        const auto nodeZ = [&](int j) { return z0 + oz + j * stride; };
        const auto inside = [&](int i, int j) { return shape[at(nodeX(i), nodeZ(j))] > 0.0f; };
        const int si = sx >= 0 ? (sx - x0 - ox) / stride : -1, sj = sx >= 0 ? (sz - z0 - oz) / stride : -1;

        if (si >= 0)
        {
            // One search from the summit serves every path: the cost of a step
            // is its length, times a penalty that grows with the square of how
            // far its grade is over the limit, the same uphill and downhill.
            //
            // A step goes to any node up to four away whose offset is in lowest
            // terms, not just to the eight neighbours: with eight directions the
            // gentlest climbing step across a flank is 22° off the contour at
            // best and 45° at worst, so on steep ground no traverse was within
            // the limit and the path went straight up. These 48 directions are
            // at most 14° apart. The area is convex, so a step between two nodes
            // inside it stays inside.
            std::vector<std::pair<int, int>> steps;
            for (int dj = -4; dj <= 4; ++dj)
                for (int di = -4; di <= 4; ++di)
                {
                    int a = std::abs(di), b = std::abs(dj);
                    while (b != 0) { const int t = a % b; a = b; b = t; }
                    if (a == 1) steps.push_back({ di, dj });
                }
            //
            // And a turn costs: a state of the search is a node AND the direction
            // it was reached in. Without that the cheapest way up a steep flank
            // is to turn at every step, a zigzag a few metres wide that keeps
            // every step gentle and that the smoothing below then draws as a
            // straight line up the hill. A full turn back costs as much as six
            // path widths of walking, so the path turns where a trail would.
            const int dirs = static_cast<int>(steps.size());
            std::vector<float> stepLen(steps.size()), dirX(steps.size()), dirZ(steps.size());
            for (size_t k = 0; k < steps.size(); ++k)
            {
                const float lx = static_cast<float>(steps[k].first * stride) * stepX, lz = static_cast<float>(steps[k].second * stride) * stepZ;
                stepLen[k] = std::sqrt(lx * lx + lz * lz);
                dirX[k] = lx / stepLen[k];
                dirZ[k] = lz / stepLen[k];
            }
            const float maxGrade = std::max(0.02f, params.pathMaxGrade);
            const float turnCost = 3.0f * std::max(params.pathWidth, static_cast<float>(stride) * std::max(stepX, stepZ));
            const size_t nodes = static_cast<size_t>(nw) * nh;
            std::vector<float> cost(nodes * steps.size(), std::numeric_limits<float>::max());
            std::vector<int32_t> parent(nodes * steps.size(), -1);
            using Entry = std::pair<float, int32_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
            const int32_t summit = sj * nw + si;
            for (int k = 0; k < dirs; ++k)   // the summit is left in any direction for free
            {
                cost[static_cast<size_t>(summit) * steps.size() + k] = 0.0f;
                open.push({ 0.0f, summit * dirs + k });
            }
            while (!open.empty())
            {
                const Entry e = open.top();
                open.pop();
                if (e.first > cost[static_cast<size_t>(e.second)]) continue;
                const int32_t node = e.second / dirs;
                const int from = e.second % dirs;
                const int ci = node % nw, cj = node / nw;
                const float ch = ground(nodeX(ci), nodeZ(cj));
                for (int k = 0; k < dirs; ++k)
                {
                    const int ni = ci + steps[static_cast<size_t>(k)].first, nj = cj + steps[static_cast<size_t>(k)].second;
                    if (ni < 0 || ni >= nw || nj < 0 || nj >= nh || !inside(ni, nj)) continue;
                    const float len = stepLen[static_cast<size_t>(k)];
                    const float over = std::max(0.0f, std::fabs(ground(nodeX(ni), nodeZ(nj)) - ch) / len / maxGrade - 1.0f);
                    const float bend = node == summit ? 0.0f
                        : (1.0f - (dirX[static_cast<size_t>(k)] * dirX[static_cast<size_t>(from)] + dirZ[static_cast<size_t>(k)] * dirZ[static_cast<size_t>(from)])) * turnCost;
                    const float c = e.first + len * (1.0f + 30.0f * over * over) + bend;
                    const int32_t n = (nj * nw + ni) * dirs + k;
                    if (c < cost[static_cast<size_t>(n)])
                    {
                        cost[static_cast<size_t>(n)] = c;
                        parent[static_cast<size_t>(n)] = e.second;
                        open.push({ c, n });
                    }
                }
            }

            // Bilinear ground height at a terrain-local position.
            const auto groundAt = [&](float x, float z)
            {
                const float gx = std::clamp((x + halfX) / stepX, 0.0f, static_cast<float>(res - 1));
                const float gz = std::clamp((z + halfZ) / stepZ, 0.0f, static_cast<float>(res - 1));
                const int ix = std::min(static_cast<int>(gx), static_cast<int>(res) - 2), iz = std::min(static_cast<int>(gz), static_cast<int>(res) - 2);
                const float fx = gx - static_cast<float>(ix), fz = gz - static_cast<float>(iz);
                const float a = ground(ix, iz) + (ground(ix + 1, iz) - ground(ix, iz)) * fx;
                const float b = ground(ix, iz + 1) + (ground(ix + 1, iz + 1) - ground(ix, iz + 1)) * fx;
                return a + (b - a) * fz;
            };

            // Each path starts where its ray from the centre meets the area: the
            // outermost node on it the search reached. The first ray's direction
            // comes from the seed, the others are spread evenly from it.
            const float turn = static_cast<float>(static_cast<uint32_t>(params.seed) * 2654435761u >> 8) / 16777216.0f * 6.2831853f;
            std::vector<std::vector<PathPoint>> lines;
            for (int pth = 0; pth < pathCount; ++pth)
            {
                const float ang = turn + 6.2831853f * static_cast<float>(pth) / static_cast<float>(pathCount);
                int32_t start = -1;
                for (float d = 0.97f; d > 0.15f && start < 0; d -= 0.02f)
                {
                    const float px = area.centerX + std::cos(ang) * rx * d, pz = area.centerZ + std::sin(ang) * rz * d;
                    const int i = static_cast<int>(std::lround((toGrid(px, -halfX, stepX) - static_cast<float>(x0 + ox)) / static_cast<float>(stride)));
                    const int j = static_cast<int>(std::lround((toGrid(pz, -halfZ, stepZ) - static_cast<float>(z0 + oz)) / static_cast<float>(stride)));
                    if (i < 0 || i >= nw || j < 0 || j >= nh || !inside(i, j) || j * nw + i == summit) continue;
                    // the cheapest of the directions the node was reached in
                    float least = std::numeric_limits<float>::max();
                    for (int k = 0; k < dirs; ++k)
                    {
                        const size_t st = (static_cast<size_t>(j) * nw + i) * steps.size() + k;
                        if (cost[st] < least) { least = cost[st]; start = static_cast<int32_t>(st); }
                    }
                }
                if (start < 0) continue;

                std::vector<PathPoint> line;
                for (int32_t n = start; n >= 0; n = parent[static_cast<size_t>(n)])
                {
                    PathPoint pt;
                    pt.x = -halfX + static_cast<float>(nodeX((n / dirs) % nw)) * stepX;
                    pt.z = -halfZ + static_cast<float>(nodeZ((n / dirs) / nw)) * stepZ;
                    line.push_back(pt);
                }
                // The grid's directions leave corners: two rounds of averaging
                // with the neighbours round them, the ends stay put.
                for (int round = 0; round < 2 && line.size() > 2; ++round)
                {
                    std::vector<PathPoint> s = line;
                    for (size_t i = 1; i + 1 < line.size(); ++i)
                    {
                        s[i].x = (line[i - 1].x + 2.0f * line[i].x + line[i + 1].x) * 0.25f;
                        s[i].z = (line[i - 1].z + 2.0f * line[i].z + line[i + 1].z) * 0.25f;
                    }
                    line.swap(s);
                }
                // The path's own height profile: the ground under it, evened out
                // along its length so the bench does not copy every bump.
                for (PathPoint& pt : line) pt.y = groundAt(pt.x, pt.z);
                for (int round = 0; round < 3 && line.size() > 2; ++round)
                {
                    std::vector<PathPoint> s = line;
                    for (size_t i = 1; i + 1 < line.size(); ++i) s[i].y = (line[i - 1].y + 2.0f * line[i].y + line[i + 1].y) * 0.25f;
                    line.swap(s);
                }
                if (line.size() >= 2) lines.push_back(std::move(line));
            }

            // Cut them in, all at once: every vertex takes the height of the
            // nearest point of any path, fully within half the width, blended
            // out over another half. Never outside the area, and faded towards a
            // blended rim so the first metres of a path leave the rim where it is.
            const float halfW = std::max(0.0f, params.pathWidth) * 0.5f;
            const float reach = halfW * 2.0f;
            if (!lines.empty() && reach > 0.0f)
            {
                std::vector<float> nearest(shape.size(), std::numeric_limits<float>::max()), target(shape.size(), 0.0f);
                for (const std::vector<PathPoint>& line : lines)
                    for (size_t i = 0; i + 1 < line.size(); ++i)
                    {
                        const PathPoint& a = line[i];
                        const PathPoint& b = line[i + 1];
                        const float abx = b.x - a.x, abz = b.z - a.z;
                        const float len2 = abx * abx + abz * abz;
                        const int bx0 = std::max(x0, static_cast<int>(std::floor(toGrid(std::min(a.x, b.x) - reach, -halfX, stepX))));
                        const int bx1 = std::min(x1, static_cast<int>(std::ceil(toGrid(std::max(a.x, b.x) + reach, -halfX, stepX))));
                        const int bz0 = std::max(z0, static_cast<int>(std::floor(toGrid(std::min(a.z, b.z) - reach, -halfZ, stepZ))));
                        const int bz1 = std::min(z1, static_cast<int>(std::ceil(toGrid(std::max(a.z, b.z) + reach, -halfZ, stepZ))));
                        for (int zi = bz0; zi <= bz1; ++zi)
                            for (int xi = bx0; xi <= bx1; ++xi)
                            {
                                const float px = -halfX + static_cast<float>(xi) * stepX, pz = -halfZ + static_cast<float>(zi) * stepZ;
                                const float t = len2 > 0.0f ? std::clamp(((px - a.x) * abx + (pz - a.z) * abz) / len2, 0.0f, 1.0f) : 0.0f;
                                const float qx = a.x + abx * t - px, qz = a.z + abz * t - pz;
                                const float d = std::sqrt(qx * qx + qz * qz);
                                const size_t c = at(xi, zi);
                                if (d < nearest[c]) { nearest[c] = d; target[c] = a.y + (b.y - a.y) * t; }
                            }
                    }
                for (int zi = z0; zi <= z1; ++zi)
                    for (int xi = x0; xi <= x1; ++xi)
                    {
                        const size_t c = at(xi, zi);
                        if (nearest[c] >= reach || shape[c] <= 0.0f) continue;
                        float wgt = nearest[c] <= halfW ? 1.0f : 1.0f - smooth01((nearest[c] - halfW) / halfW);
                        wgt *= std::clamp(rimW[c] * 8.0f, 0.0f, 1.0f);
                        float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
                        h += (target[c] - h) * wgt;
                    }
            }

            // What the caller gets: the ground as it is now, a point every path
            // width or so (never fewer than the two ends).
            const float spacing = std::max({ params.pathWidth, static_cast<float>(stride) * std::max(stepX, stepZ), 0.01f });
            for (std::vector<PathPoint>& line : lines)
            {
                std::vector<PathPoint> out;
                out.push_back(line.front());
                for (size_t i = 1; i + 1 < line.size(); ++i)
                {
                    const float ddx = line[i].x - out.back().x, ddz = line[i].z - out.back().z;
                    if (ddx * ddx + ddz * ddz >= spacing * spacing) out.push_back(line[i]);
                }
                out.push_back(line.back());
                for (PathPoint& pt : out) pt.y = groundAt(pt.x, pt.z);
                r.paths.push_back(std::move(out));
            }
        }
    }

    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    for (int zi = z0; zi <= z1; ++zi)
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
            if (h == before[at(xi, zi)]) continue;
            ++r.changed;
            mn = std::min(mn, h);
            mx = std::max(mx, h);
        }

    if (r.changed > 0)
    {
        // Only the chunks under the area have to be rebuilt; `dirty` would
        // rebuild all of them.
        const float mnX = area.centerX - rx, mxX = area.centerX + rx;
        const float mnZ = area.centerZ - rz, mxZ = area.centerZ + rz;
        if (tc.regionDirty)
        {
            tc.dirtyMinX = std::min(tc.dirtyMinX, mnX); tc.dirtyMaxX = std::max(tc.dirtyMaxX, mxX);
            tc.dirtyMinZ = std::min(tc.dirtyMinZ, mnZ); tc.dirtyMaxZ = std::max(tc.dirtyMaxZ, mxZ);
        }
        else
        {
            tc.dirtyMinX = mnX; tc.dirtyMaxX = mxX;
            tc.dirtyMinZ = mnZ; tc.dirtyMaxZ = mxZ;
            tc.regionDirty = true;
        }
        r.minHeight = mn;
        r.maxHeight = mx;
        r.peakAdded = peak;
    }
    return r;
}

} // namespace TerrainGenerate
