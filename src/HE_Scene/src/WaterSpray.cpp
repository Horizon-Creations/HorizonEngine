#include "HorizonScene/WaterSpray.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace HE::water
{

float sprayStrength(float drop, float speed)
{
    if (!(drop >= kSprayMinDrop) || !(speed >= kSprayMinSpeed)) return 0.0f;
    const float h = std::clamp((drop - kSprayMinDrop) / (kSprayFullDrop - kSprayMinDrop), 0.0f, 1.0f);
    const float v = std::clamp((speed - kSprayMinSpeed) / (kStreamMaxSpeed - kSprayMinSpeed), 0.0f, 1.0f);
    // The fall counts most: a long drop that ends in a slow pool still throws a lot, a fast shallow slide little.
    return std::clamp(0.15f + 0.65f * h + 0.2f * v, 0.0f, 1.0f);
}

float fallStrength(float height)
{
    if (!(height >= kFallMinHeight)) return 0.0f;
    return std::clamp(0.2f + 0.8f * (height - kFallMinHeight) / (kFallFullHeight - kFallMinHeight), 0.0f, 1.0f);
}

namespace
{
    constexpr float kGravity = 9.8f;

    // The water at a terrain-local point: its surface height and how fast it runs; false where there is none.
    bool waterAt(const TerrainComponent& tc, float x, float z, float& surfaceY, float& speed)
    {
        const uint16_t id = bodyAt(tc, x, z);
        const Body* b = tc.water.findBody(id);
        if (!b) return false;
        const float ground = terrainHeightAt(tc, x, z);
        if (b->stream)
        {
            if (!std::isfinite(ground)) return false;
            surfaceY = streamSurfaceAt(tc, *b, x, z);
            const auto pool = streamPoolOf(tc, b->id);
            const float poolHere = pool ? streamPoolAt(*pool, tc, x, z) : 0.0f;
            speed = poolHere > kStreamPoolStill ? 0.0f : glm::length(streamFlowAt(tc, x, z));   // a pool is calm
        }
        else
        {
            if (tc.water.clipToGround && std::isfinite(ground) && ground > b->level + tc.water.shoreOvershoot) return false;
            surfaceY = b->level;
            speed = glm::length(b->physics.flow);     // a lake or pond: the current it was given, none by default
        }
        return true;
    }

    bool hasStream(const TerrainComponent& tc)
    {
        const Field& f = tc.water;
        if (!f.allocated() || tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return false;
        return std::any_of(f.bodies.begin(), f.bodies.end(), [](const Body& b) { return b.stream; });
    }

    // ── The faces ───────────────────────────────────────────────────────────────
    // How steep the ground falls ALONG the current `d` at `p` (m/m, a central difference kFallReadAlong either side):
    // what makes the water fall. The steepest way down in any direction (streamSlopeAt) would also read the banks of the
    // stream's own channel, and every stroke down a 45 degree ramp would have a "face" along its edges.
    constexpr float kFallReadAlong = 0.75f;
    float slopeAlong(const TerrainComponent& tc, const glm::vec2& p, const glm::vec2& d)
    {
        const glm::vec2 a = p - d * kFallReadAlong, b = p + d * kFallReadAlong;
        const float ha = terrainHeightAt(tc, a.x, a.y), hb = terrainHeightAt(tc, b.x, b.y);
        return std::isfinite(ha) && std::isfinite(hb) ? (ha - hb) / (2.0f * kFallReadAlong) : 0.0f;
    }

    // Every `stride`-th cell of the water grid (about kFallLipSpacing apart) that holds water; for a stream, which way and
    // how fast it runs there and whether the ground along it is steep enough for the water to fall.
    struct FaceGrid
    {
        int   w = 0, stride = 1;
        float spacing = 1.0f;
        std::vector<uint8_t>   steep;   // 0 no water, 1 water, 2 a stream falling (>= kFallSlope along its current)
        std::vector<glm::vec2> pos;     // terrain-local XZ of the sample (the cell's centre)
        std::vector<glm::vec2> dir;     // unit direction of the current (0 where it does not run)
        std::vector<float>     speed;   // m/s
    };

    FaceGrid sampleFaces(const TerrainComponent& tc)
    {
        const Field& f = tc.water;
        FaceGrid g;
        const int res = static_cast<int>(f.res);
        const glm::vec2 cell = cellSize(tc);
        const float cmax = std::max(cell.x, cell.y);
        g.stride  = std::max(1, static_cast<int>(std::lround(kFallLipSpacing / cmax)));
        g.spacing = static_cast<float>(g.stride) * cmax;
        g.w       = (res + g.stride - 1) / g.stride;
        g.steep.assign(static_cast<size_t>(g.w) * g.w, 0);
        g.pos.assign(g.steep.size(), glm::vec2(0.0f));
        g.dir.assign(g.steep.size(), glm::vec2(0.0f));
        g.speed.assign(g.steep.size(), 0.0f);
        // A pool is still water whatever the ground under it: looked up once per body, not per sample.
        std::unordered_map<uint16_t, std::shared_ptr<const StreamPool>> pools;
        for (int gz = 0; gz < g.w; ++gz)
            for (int gx = 0; gx < g.w; ++gx)
            {
                const int cx = gx * g.stride, cz = gz * g.stride;
                const size_t i = static_cast<size_t>(cz) * res + cx;
                if (f.owner[i] == kNoBody || f.coverage[i] < kWet) continue;
                const Body* b = f.findBody(f.owner[i]);
                if (!b) continue;
                const glm::vec2 c = cellCenter(tc, cx, cz);
                const size_t k = static_cast<size_t>(gz) * g.w + gx;
                g.pos[k]   = c;
                g.steep[k] = 1;
                if (!b->stream) continue;   // a lake: water (a stream may leave it over an edge), but level, not running
                auto it = pools.find(b->id);
                if (it == pools.end()) it = pools.emplace(b->id, streamPoolOf(tc, b->id)).first;
                if (it->second && streamPoolAt(*it->second, tc, c.x, c.y) > kStreamPoolStill) continue;
                const glm::vec2 v = streamFlowAt(tc, c.x, c.y);
                const float sp = glm::length(v);
                if (!(sp > 0.1f)) continue;
                g.dir[k]   = v / sp;
                g.speed[k] = sp;
                if (slopeAlong(tc, c, g.dir[k]) >= kFallSlope) g.steep[k] = 2;
            }
        return g;
    }

    // The sample a terrain-local point lies on, -1 off the grid.
    int sampleAt(const FaceGrid& g, const TerrainComponent& tc, const glm::vec2& p)
    {
        const glm::vec2 cell = cellSize(tc);
        const float fx = (p.x + 0.5f * tc.sizeX) / cell.x - 0.5f, fz = (p.y + 0.5f * tc.sizeZ) / cell.y - 0.5f;
        if (!std::isfinite(fx) || !std::isfinite(fz)) return -1;
        const int gx = static_cast<int>(std::lround(fx / static_cast<float>(g.stride)));
        const int gz = static_cast<int>(std::lround(fz / static_cast<float>(g.stride)));
        if (gx < 0 || gz < 0 || gx >= g.w || gz >= g.w) return -1;
        return gz * g.w + gx;
    }

    // The faces: the samples where the water falls, connected through their eight neighbours. face[k] is the face a
    // sample belongs to, -1 for none; the count is returned.
    int connectFaces(const FaceGrid& g, std::vector<int>& face)
    {
        const int w = g.w;
        face.assign(g.steep.size(), -1);
        int count = 0;
        std::vector<int> stack;
        for (int k = 0; k < w * w; ++k)
        {
            if (g.steep[static_cast<size_t>(k)] != 2 || face[static_cast<size_t>(k)] >= 0) continue;
            face[static_cast<size_t>(k)] = count;
            stack.assign(1, k);
            while (!stack.empty())
            {
                const int c = stack.back();
                stack.pop_back();
                const int cx = c % w, cz = c / w;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = cx + dx, nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= w || nz >= w) continue;
                        const size_t nk = static_cast<size_t>(nz) * w + nx;
                        if (g.steep[nk] == 2 && face[nk] < 0) { face[nk] = count; stack.push_back(static_cast<int>(nk)); }
                    }
            }
            ++count;
        }
        return count;
    }

    // A lip with what cutting a face's edge into straight pieces reads: its place across and along the face's current,
    // how far down the face it lands and at what height.
    struct EdgeLip
    {
        FallLip lip;
        float   t = 0.0f, s = 0.0f;   // along the face's across axis / its current, m
        float   faceRun = 0.0f;       // m along its own current from the edge to where the face eases off
        float   landY = 0.0f;         // terrain-local ground height there
    };
    struct Face
    {
        std::vector<EdgeLip> lips;   // ordered across the current
        float tallest = 0.0f;
    };

    // The top edge of every face: per kFallLipSpacing of its width (a bin across its mean current) the uppermost sample of
    // the face whose upstream neighbour is not part of it, and on it the exact place where the ground turns steeper than
    // kFallSlope. Faces without a lip that falls kFallMinHeight are none. Tallest first.
    std::vector<Face> findFaces(const TerrainComponent& tc)
    {
        std::vector<Face> faces;
        if (!hasStream(tc)) return faces;
        const FaceGrid g = sampleFaces(tc);
        std::vector<int> face;
        const int count = connectFaces(g, face);
        if (count == 0) return faces;

        // The candidates: the face's samples with water upstream that is not part of the face — water that comes OVER
        // the edge. Where the stream begins on the face itself (the top of a stroke) nothing falls over anything.
        struct Candidate { int face; glm::vec2 c; glm::vec2 d; float speed; };
        std::vector<Candidate> cands;
        for (size_t k = 0; k < face.size(); ++k)
        {
            if (face[k] < 0) continue;
            const glm::vec2 c = g.pos[k], d = g.dir[k];
            const int u = sampleAt(g, tc, c - d * g.spacing);
            if (u < 0 || g.steep[static_cast<size_t>(u)] == 0 || face[static_cast<size_t>(u)] == face[k]) continue;
            cands.push_back({ face[k], c, d, g.speed[k] });
        }

        std::vector<std::vector<const Candidate*>> byFace(static_cast<size_t>(count));
        for (const Candidate& c : cands) byFace[static_cast<size_t>(c.face)].push_back(&c);
        const float walkStep = std::min(g.spacing, 0.75f);
        for (const auto& fc : byFace)
        {
            if (fc.empty()) continue;
            glm::vec2 mean(0.0f);
            for (const Candidate* c : fc) mean += c->d;
            const glm::vec2 D = glm::length(mean) > 1e-5f ? glm::normalize(mean) : fc.front()->d;
            const glm::vec2 A(-D.y, D.x);
            // The uppermost candidate in each bin across the current: a candidate below another one of the same bin is
            // a step inside the face (a dip the slope reads), not its edge.
            std::unordered_map<int, const Candidate*> top;
            for (const Candidate* c : fc)
            {
                const int bin = static_cast<int>(std::lround(glm::dot(c->c, A) / g.spacing));
                auto it = top.find(bin);
                if (it == top.end() || glm::dot(c->c, D) < glm::dot(it->second->c, D)) top[bin] = c;
            }
            Face out;
            for (const auto& [bin, cand] : top)
            {
                const glm::vec2 c = cand->c, d = cand->d;
                const glm::vec2 u = c - d * g.spacing;
                // The lip is the EDGE: where the ground along the current, read over half a metre, gets steeper than
                // kFallSlope, bisected between the sample upstream and this one (or found just below it). The curtain
                // starts there, flush along the whole edge, not a sample's step down the face.
                auto steep = [&](const glm::vec2& q) {
                    const glm::vec2 a = q - d * 0.25f, b = q + d * 0.25f;
                    const float ha = terrainHeightAt(tc, a.x, a.y), hb = terrainHeightAt(tc, b.x, b.y);
                    return std::isfinite(ha) && std::isfinite(hb) && (ha - hb) / 0.5f >= kFallSlope;
                };   // read over half a metre: the edge itself, not the sample's metre around it
                const glm::vec2 edge = [&] {
                    glm::vec2 lo = u, hi = c;
                    if (steep(lo)) return c;
                    if (!steep(hi))
                    {
                        glm::vec2 q = c;
                        for (int k = 0; k < 8 && !steep(q); ++k) q += d * 0.25f;
                        if (!steep(q)) return c;
                        lo = q - d * 0.25f; hi = q;
                    }
                    for (int k = 0; k < 10; ++k)
                    {
                        const glm::vec2 m = 0.5f * (lo + hi);
                        (steep(m) ? hi : lo) = m;
                    }
                    return hi;
                }();
                // Down the face along the current to where it eases off below kFallSlope: there the water lands.
                glm::vec2 p = c;
                for (int s = 0; s < 200; ++s)
                {
                    const glm::vec2 w = streamFlowAt(tc, p.x, p.y);
                    const float ws = glm::length(w);
                    if (!(ws > 0.1f)) break;
                    if (slopeAlong(tc, p, w / ws) < kFallSlope) break;
                    p += w / ws * walkStep;
                }
                const float topY = terrainHeightAt(tc, edge.x, edge.y), footY = terrainHeightAt(tc, p.x, p.y);
                if (!std::isfinite(topY) || !std::isfinite(footY)) continue;
                const float height = topY - footY;
                const float strength = fallStrength(height);
                if (!(strength > 0.0f)) continue;
                float surf = 0.0f, sq = 0.0f;
                glm::vec2 at = edge;
                if (!waterAt(tc, at.x, at.y, surf, sq)) { at = c; if (!waterAt(tc, c.x, c.y, surf, sq)) continue; }
                EdgeLip e;
                e.lip     = { at, d, surf, cand->speed, height, strength, p, 0u };
                e.t       = glm::dot(at, A);
                e.s       = glm::dot(at, D);
                e.faceRun = std::max(0.0f, glm::dot(p - at, d));
                e.landY   = footY;
                out.lips.push_back(e);
                out.tallest = std::max(out.tallest, height);
            }
            if (out.lips.empty()) continue;
            std::sort(out.lips.begin(), out.lips.end(), [](const EdgeLip& a, const EdgeLip& b) { return a.t < b.t; });
            faces.push_back(std::move(out));
        }
        std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
            if (a.tallest != b.tallest) return a.tallest > b.tallest;
            const glm::vec2 pa = a.lips.front().lip.pos, pb = b.lips.front().lip.pos;
            return pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y;
        });
        for (size_t i = 0; i < faces.size(); ++i)
            for (EdgeLip& e : faces[i].lips) e.lip.face = static_cast<uint32_t>(i);
        return faces;
    }

    // One face's edge cut into straight pieces [first, last] of its lips: at a gap of kFallLineGap, then (Douglas-Peucker)
    // wherever a lip lies off the straight line between its piece's ends — along the current or in height by more than
    // kFallLineTolerance, in where it lands by more than kFallLandTolerance. A cut leaves at least three lips each side: a
    // stray lip is a ripple in the edge, not a curtain of its own.
    std::vector<std::pair<size_t, size_t>> cutEdge(const std::vector<EdgeLip>& lips)
    {
        std::vector<std::pair<size_t, size_t>> runs, out;
        size_t start = 0;
        for (size_t i = 1; i <= lips.size(); ++i)
            if (i == lips.size() || lips[i].t - lips[i - 1].t > kFallLineGap) { runs.emplace_back(start, i - 1); start = i; }
        for (const auto& run : runs)
        {
            std::vector<std::pair<size_t, size_t>> stack{ run };
            std::vector<std::pair<size_t, size_t>> pieces;
            while (!stack.empty())
            {
                const auto [a, b] = stack.back();
                stack.pop_back();
                size_t worst = a;
                float worstDev = 1.0f;
                const float span = lips[b].t - lips[a].t;
                for (size_t k = a + 1; k < b; ++k)
                {
                    const float f = span > 1e-5f ? (lips[k].t - lips[a].t) / span : 0.0f;
                    auto off = [&](float va, float vb, float vk, float tol) { return std::abs(vk - (va + (vb - va) * f)) / tol; };
                    const float dev = std::max({ off(lips[a].s, lips[b].s, lips[k].s, kFallLineTolerance),
                                                 off(lips[a].lip.surfaceY, lips[b].lip.surfaceY, lips[k].lip.surfaceY, kFallLineTolerance),
                                                 off(lips[a].faceRun, lips[b].faceRun, lips[k].faceRun, kFallLandTolerance),
                                                 off(lips[a].landY, lips[b].landY, lips[k].landY, kFallLandTolerance) });
                    if (dev > worstDev && k - a + 1 >= 3 && b - k >= 3) { worstDev = dev; worst = k; }
                }
                if (worst == a) { pieces.emplace_back(a, b); continue; }
                stack.emplace_back(a, worst);
                stack.emplace_back(worst + 1, b);
            }
            std::sort(pieces.begin(), pieces.end());
            out.insert(out.end(), pieces.begin(), pieces.end());
        }
        return out;
    }

    // One piece of edge as a line, and where its curtain lands.
    FallLine lineOf(const TerrainComponent& tc, const std::vector<EdgeLip>& lips, size_t a, size_t b, float spacing)
    {
        FallLine f;
        glm::vec2 dir(0.0f);
        float faceRun = 0.0f;
        for (size_t k = a; k <= b; ++k)
        {
            const FallLip& m = lips[k].lip;
            dir        += m.dir;
            f.surfaceY += m.surfaceY;
            f.speed    += m.speed;
            f.height   += m.height;
            faceRun    += lips[k].faceRun;
        }
        const float cnt = static_cast<float>(b - a + 1);
        f.dir      = glm::length(dir) > 1e-5f ? glm::normalize(dir) : lips[a].lip.dir;
        f.across   = glm::vec2(-f.dir.y, f.dir.x);
        f.surfaceY /= cnt;
        f.speed    /= cnt;
        f.height   /= cnt;
        faceRun    /= cnt;
        f.strength = fallStrength(f.height);
        const glm::vec2 origin = lips[a].lip.pos;
        float lo = 1e30f, hi = -1e30f, along = 0.0f;
        for (size_t k = a; k <= b; ++k)
        {
            const float s = glm::dot(lips[k].lip.pos - origin, f.across);
            lo = std::min(lo, s); hi = std::max(hi, s);
            along += glm::dot(lips[k].lip.pos - origin, f.dir);
        }
        f.center    = origin + f.across * (0.5f * (lo + hi)) + f.dir * (along / cnt);
        f.halfWidth = 0.5f * (hi - lo) + 0.5f * spacing;
        f.lips      = static_cast<uint32_t>(b - a + 1);
        f.slope     = f.height / std::max(faceRun, 0.25f);
        // Down a rounded wall the water rides the face to its foot; over a cliff it is thrown further than the face is
        // deep. Whichever is further is where it lands.
        const float fallTime = std::sqrt(2.0f * std::max(f.height, 0.5f) / kGravity);
        f.run      = std::max(faceRun, std::clamp(f.speed, 1.0f, 4.0f) * fallTime);
        f.landing  = f.center + f.dir * f.run;
        float surf = 0.0f, sq = 0.0f;
        if (waterAt(tc, f.landing.x, f.landing.y, surf, sq)) f.landingY = surf;
        else
        {
            const float ground = terrainHeightAt(tc, f.landing.x, f.landing.y);
            f.landingY = std::isfinite(ground) ? ground : f.surfaceY - f.height;
        }
        return f;
    }
}

std::vector<FallLip> findFallLips(const TerrainComponent& tc)
{
    std::vector<FallLip> out;
    for (const Face& f : findFaces(tc))
        for (const EdgeLip& e : f.lips) out.push_back(e.lip);
    std::stable_sort(out.begin(), out.end(), [](const FallLip& a, const FallLip& b) { return a.height > b.height; });
    return out;
}

std::vector<Fall> findFalls(const TerrainComponent& tc)
{
    std::vector<FallLine> lines;
    {
        const std::vector<Face> faces = findFaces(tc);
        if (faces.empty()) return {};
        const glm::vec2 cell = cellSize(tc);
        const float cmax = std::max(cell.x, cell.y);
        const float spacing = static_cast<float>(std::max(1, static_cast<int>(std::lround(kFallLipSpacing / cmax)))) * cmax;
        for (const Face& f : faces)
        {
            const size_t first = lines.size();
            for (const auto& [a, b] : cutEdge(f.lips)) lines.push_back(lineOf(tc, f.lips, a, b, spacing));
            // The pieces of one edge start at one height when the edge is level (its lips within a metre): a piece is
            // cut where the water LANDS elsewhere, and its own mean would put a step into an even top.
            float lo = 1e30f, hi = -1e30f, sum = 0.0f;
            for (const EdgeLip& e : f.lips) { lo = std::min(lo, e.lip.surfaceY); hi = std::max(hi, e.lip.surfaceY); sum += e.lip.surfaceY; }
            if (hi - lo <= 2.0f * kFallLineTolerance)
                for (size_t i = first; i < lines.size(); ++i) lines[i].surfaceY = sum / static_cast<float>(f.lips.size());
        }
    }
    // A piece whose edge lies under a higher piece's curtain — between that edge and where it lands, within its width —
    // is part of that fall (the face eased off for a moment and steepened again): its curtain is already falling there.
    std::vector<Fall> out;
    for (const FallLine& b : lines)
    {
        bool under = false;
        for (const FallLine& a : lines)
        {
            if (&a == &b || !(a.surfaceY > b.surfaceY + 0.5f) || glm::dot(a.dir, b.dir) < 0.7f) continue;
            const glm::vec2 d = b.center - a.center;
            const float along = glm::dot(d, a.dir), across = std::abs(glm::dot(d, a.across));
            under = under || (along > 0.0f && along < a.run && across <= a.halfWidth);
        }
        if (under) continue;
        Fall fall;
        fall.line = b;
        SprayFoot& ft = fall.foot;
        ft.pos       = b.landing;
        ft.dir       = b.dir;
        ft.surfaceY  = b.landingY;
        ft.speed     = std::sqrt(2.0f * kGravity * std::max(b.height, 0.0f));   // what it lands with
        ft.drop      = b.height;
        ft.strength  = sprayStrength(ft.drop, ft.speed);
        ft.halfWidth = b.halfWidth;
        out.push_back(fall);
    }
    std::stable_sort(out.begin(), out.end(), [](const Fall& a, const Fall& b) { return a.line.height > b.line.height; });
    if (out.size() > kFallMaxLines) out.resize(kFallMaxLines);
    return out;
}

std::vector<FallLine> findFallLines(const TerrainComponent& tc)
{
    std::vector<FallLine> out;
    for (const Fall& f : findFalls(tc)) out.push_back(f.line);
    return out;
}

std::vector<SprayFoot> findRunoutFeet(const TerrainComponent& tc, const std::vector<Fall>& falls)
{
    std::vector<SprayFoot> out;
    if (!hasStream(tc)) return out;
    const Field& f = tc.water;
    const int res = static_cast<int>(f.res);
    const glm::vec2 cell = cellSize(tc);
    const float probe = std::max(1.5f, 1.5f * std::max(cell.x, cell.y));
    const int stride = 2;
    const float spacing = static_cast<float>(stride) * std::max(cell.x, cell.y);

    // Every sample where fast water arrives at calm water, having dropped kFallMinHeight on the way.
    std::vector<SprayFoot> found;
    for (int cz = 0; cz < res; cz += stride)
        for (int cx = 0; cx < res; cx += stride)
        {
            const size_t i = static_cast<size_t>(cz) * res + cx;
            if (f.owner[i] == kNoBody || f.coverage[i] < kWet) continue;
            const Body* b = f.findBody(f.owner[i]);
            if (!b || !b->stream) continue;
            const glm::vec2 c = cellCenter(tc, cx, cz);
            if (const auto pool = streamPoolOf(tc, b->id); pool && streamPoolAt(*pool, tc, c.x, c.y) > kStreamPoolStill) continue;
            const glm::vec2 v = streamFlowAt(tc, c.x, c.y);
            const float sp = glm::length(v);
            if (sp < kSprayMinSpeed) continue;
            const glm::vec2 d = v / sp;
            // Downstream: water goes on (the stream, a lake, a pool) and is calm there.
            const glm::vec2 q = c + d * probe;
            float surf = 0.0f, sq = 0.0f;
            if (!waterAt(tc, q.x, q.y, surf, sq)) continue;
            if (!(sq <= kSprayCalmRatio * sp)) continue;
            // The drop: walk back up the current while it stays fast, the ground's height there minus at the foot.
            glm::vec2 p = c;
            float top = terrainHeightAt(tc, p.x, p.y);
            for (int s = 0; s < 32; ++s)
            {
                const glm::vec2 u = streamFlowAt(tc, p.x, p.y);
                const float us = glm::length(u);
                if (us < 1.0f) break;
                p -= u / us * 1.5f;
                const float h = terrainHeightAt(tc, p.x, p.y);
                if (std::isfinite(h)) top = std::max(top, h);
            }
            const float foot = terrainHeightAt(tc, q.x, q.y);
            if (!std::isfinite(top) || !std::isfinite(foot)) continue;
            const float drop = top - foot;
            // Lower than a fall: the material's white water is the splash, a cloud of drops would be too much.
            if (!(drop >= kFallMinHeight)) continue;
            const float strength = sprayStrength(drop, sp);
            if (!(strength > 0.0f)) continue;
            found.push_back({ q, d, surf, sp, drop, strength, 0.0f });
        }

    // Where the water meets the calm water is a LINE across it: neighbouring samples that arrive the same way are one foot.
    const size_t n = found.size();
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i) parent[i] = i;
    auto root = [&](size_t i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (glm::dot(found[i].dir, found[j].dir) >= 0.7f && glm::length(found[i].pos - found[j].pos) <= 2.5f * spacing)
                parent[root(j)] = root(i);
    std::unordered_map<size_t, std::vector<const SprayFoot*>> groups;
    for (size_t i = 0; i < n; ++i) groups[root(i)].push_back(&found[i]);
    for (const auto& [r, g] : groups)
    {
        SprayFoot ft;
        glm::vec2 dir(0.0f), pos(0.0f);
        for (const SprayFoot* m : g)
        {
            dir += m->dir; pos += m->pos;
            ft.surfaceY += m->surfaceY; ft.speed += m->speed;
            ft.drop = std::max(ft.drop, m->drop);
        }
        const float cnt = static_cast<float>(g.size());
        ft.dir = glm::length(dir) > 1e-5f ? glm::normalize(dir) : g.front()->dir;
        ft.pos = pos / cnt;
        ft.surfaceY /= cnt;
        ft.speed /= cnt;
        ft.strength = sprayStrength(ft.drop, ft.speed);
        const glm::vec2 across(-ft.dir.y, ft.dir.x);
        float lo = 0.0f, hi = 0.0f;
        for (const SprayFoot* m : g)
        {
            const float s = glm::dot(m->pos - ft.pos, across);
            lo = std::min(lo, s); hi = std::max(hi, s);
        }
        ft.halfWidth = 0.5f * (hi - lo) + 0.5f * spacing;
        // A fall lands here, or the water is still falling here: that fall's foot is the spray.
        bool fallen = false;
        for (const Fall& fl : falls)
        {
            const glm::vec2 fa(-fl.line.dir.y, fl.line.dir.x);
            const glm::vec2 d = ft.pos - fl.line.center;
            const float along = glm::dot(d, fl.line.dir), side = std::abs(glm::dot(d, fa));
            fallen = fallen || (along >= -1.0f && along <= fl.line.run + 4.0f && side <= fl.line.halfWidth + 2.0f);
        }
        if (!fallen) out.push_back(ft);
    }
    std::sort(out.begin(), out.end(), [](const SprayFoot& a, const SprayFoot& b) {
        if (a.strength != b.strength) return a.strength > b.strength;
        return a.pos.x != b.pos.x ? a.pos.x < b.pos.x : a.pos.y < b.pos.y;
    });
    return out;
}

std::vector<SprayFoot> findSprayFeet(const TerrainComponent& tc)
{
    const std::vector<Fall> falls = findFalls(tc);
    std::vector<SprayFoot> out = findRunoutFeet(tc, falls);
    for (const Fall& f : falls) out.push_back(f.foot);
    std::stable_sort(out.begin(), out.end(), [](const SprayFoot& a, const SprayFoot& b) { return a.strength > b.strength; });
    return out;
}

} // namespace HE::water
