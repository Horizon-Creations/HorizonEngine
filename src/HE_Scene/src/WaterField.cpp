#include "HorizonScene/WaterField.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>

namespace HE::water
{

namespace
{
    constexpr int kSubRows = 8;   // samples along z per cell row in rasterizePolygon

    bool hasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    uint32_t clampRes(uint32_t r) { return std::clamp<uint32_t>(r == 0 ? kDefaultRes : r, 1u, kMaxRes); }

    // Terrain-local coordinate → cell units (fractional): cell k spans [k, k+1).
    double toCellX(const TerrainComponent& tc, double x) { return (x + tc.sizeX * 0.5) / tc.sizeX * tc.water.res; }
    double toCellZ(const TerrainComponent& tc, double z) { return (z + tc.sizeZ * 0.5) / tc.sizeZ * tc.water.res; }

    // Widen the field's dirty rectangle by a cell rectangle (inclusive).
    void markCells(TerrainComponent& tc, int x0, int z0, int x1, int z1)
    {
        Field& f = tc.water;
        const float cw = tc.sizeX / static_cast<float>(f.res);
        const float ch = tc.sizeZ / static_cast<float>(f.res);
        const float minX = -tc.sizeX * 0.5f + static_cast<float>(x0) * cw;
        const float minZ = -tc.sizeZ * 0.5f + static_cast<float>(z0) * ch;
        const float maxX = -tc.sizeX * 0.5f + static_cast<float>(x1 + 1) * cw;
        const float maxZ = -tc.sizeZ * 0.5f + static_cast<float>(z1 + 1) * ch;
        if (!f.dirty)
        {
            f.dirtyMinX = minX; f.dirtyMinZ = minZ; f.dirtyMaxX = maxX; f.dirtyMaxZ = maxZ;
            f.dirty = true;
        }
        else
        {
            f.dirtyMinX = std::min(f.dirtyMinX, minX); f.dirtyMinZ = std::min(f.dirtyMinZ, minZ);
            f.dirtyMaxX = std::max(f.dirtyMaxX, maxX); f.dirtyMaxZ = std::max(f.dirtyMaxZ, maxZ);
        }
        ++f.revision;
    }

    void markAll(TerrainComponent& tc)
    {
        Field& f = tc.water;
        f.dirtyMinX = -tc.sizeX * 0.5f; f.dirtyMaxX = tc.sizeX * 0.5f;
        f.dirtyMinZ = -tc.sizeZ * 0.5f; f.dirtyMaxZ = tc.sizeZ * 0.5f;
        f.dirty = true;
        ++f.revision;
    }

    // Tracks which cells an edit changed, for the Result and the dirty rectangle.
    struct Touched
    {
        uint32_t changed = 0;
        int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        void add(int x, int z)
        {
            if (changed++ == 0) { x0 = x1 = x; z0 = z1 = z; return; }
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            z0 = std::min(z0, z); z1 = std::max(z1, z);
        }
        Result finish(TerrainComponent& tc) const
        {
            if (changed) markCells(tc, x0, z0, x1, z1);
            Result r; r.ok = true; r.changed = changed;
            return r;
        }
    };

    // The 0..1 brush weight of a circle at distance `dist`: 1 inside the radius,
    // linear to 0 across the falloff.
    float circleWeight(float dist, float radius, float falloff)
    {
        if (dist <= radius) return 1.0f;
        if (falloff <= 0.0f) return 0.0f;
        return std::max(0.0f, 1.0f - (dist - radius) / falloff);
    }

    bool finite(float v) { return std::isfinite(v); }

    struct CircleCells { int x0, z0, x1, z1; bool any; };

    CircleCells circleCells(const TerrainComponent& tc, float x, float z, float outer)
    {
        const double res = tc.water.res;
        CircleCells c{};
        c.x0 = std::max(0, static_cast<int>(std::floor(toCellX(tc, x - outer))));
        c.x1 = std::min(static_cast<int>(res) - 1, static_cast<int>(std::ceil(toCellX(tc, x + outer))));
        c.z0 = std::max(0, static_cast<int>(std::floor(toCellZ(tc, z - outer))));
        c.z1 = std::min(static_cast<int>(res) - 1, static_cast<int>(std::ceil(toCellZ(tc, z + outer))));
        c.any = c.x0 <= c.x1 && c.z0 <= c.z1;
        return c;
    }

    bool bodyEq(const Body& a, const Body& b)
    {
        return a.id == b.id && a.level == b.level && a.sourceSpline == b.sourceSpline;
    }
    bool bodiesEq(const std::vector<Body>& a, const std::vector<Body>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (!bodyEq(a[i], b[i])) return false;
        return true;
    }

    // Cell i of a field as (owner, coverage), zero when the grid is not allocated.
    uint16_t ownerOf(const Field& f, size_t i) { return f.allocated() ? f.owner[i] : 0; }
    uint8_t  covOf(const Field& f, size_t i)   { return f.allocated() ? f.coverage[i] : 0; }

    void putVarint(std::vector<uint8_t>& out, uint32_t v)
    {
        while (v >= 0x80) { out.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
        out.push_back(static_cast<uint8_t>(v));
    }
    bool getVarint(const uint8_t* d, size_t n, size_t& at, uint32_t& v)
    {
        v = 0;
        for (int shift = 0; shift <= 28; shift += 7)
        {
            if (at >= n) return false;
            const uint8_t b = d[at++];
            v |= static_cast<uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;   // more than five bytes: not a count we wrote
    }
}

// ── Field ────────────────────────────────────────────────────────────────────

const Body* Field::findBody(uint16_t id) const
{
    if (id == kNoBody) return nullptr;
    for (const Body& b : bodies) if (b.id == id) return &b;
    return nullptr;
}
Body* Field::findBody(uint16_t id)
{
    return const_cast<Body*>(static_cast<const Field*>(this)->findBody(id));
}
const Body* Field::findBySource(const HE::UUID& spline) const
{
    if (spline == HE::UUID{}) return nullptr;
    for (const Body& b : bodies) if (b.sourceSpline == spline) return &b;
    return nullptr;
}

uint16_t Field::createBody(float level, const HE::UUID& sourceSpline)
{
    // Walk forward from the counter to the first id nobody holds. Without a
    // wrap that is nextBodyId itself; after 65535 creations it skips live ones.
    uint32_t id = nextBodyId == 0 ? 1u : nextBodyId;
    for (uint32_t tried = 0; tried < 65535u; ++tried)
    {
        if (!findBody(static_cast<uint16_t>(id)))
        {
            Body b;
            b.id = static_cast<uint16_t>(id);
            b.level = level;
            b.sourceSpline = sourceSpline;
            bodies.push_back(b);
            nextBodyId = static_cast<uint16_t>(id == 65535u ? 1u : id + 1u);
            return b.id;
        }
        id = id == 65535u ? 1u : id + 1u;
    }
    return kNoBody;
}

uint32_t Field::wetCells(uint16_t id) const
{
    if (!allocated()) return 0;
    uint32_t n = 0;
    for (size_t i = 0; i < coverage.size(); ++i)
        if (coverage[i] >= kWet && (id == kNoBody || owner[i] == id)) ++n;
    return n;
}

bool sameContent(const Field& a, const Field& b)
{
    if (a.res != b.res || a.nextBodyId != b.nextBodyId || !bodiesEq(a.bodies, b.bodies)) return false;
    const size_t n = static_cast<size_t>(a.res) * a.res;
    if (a.allocated() && b.allocated()) return a.coverage == b.coverage && a.owner == b.owner;
    for (size_t i = 0; i < n; ++i)
        if (covOf(a, i) != covOf(b, i) || ownerOf(a, i) != ownerOf(b, i)) return false;
    return true;
}

// ── Grid ─────────────────────────────────────────────────────────────────────

bool ensureGrid(TerrainComponent& tc)
{
    Field& f = tc.water;
    f.res = clampRes(f.res);
    if (!hasArea(tc)) return false;
    if (f.allocated()) return true;
    const size_t n = static_cast<size_t>(f.res) * f.res;
    f.coverage.assign(n, 0);
    f.owner.assign(n, 0);
    return true;
}

void setResolution(TerrainComponent& tc, uint32_t res)
{
    Field& f = tc.water;
    res = clampRes(res);
    if (res == f.res) return;
    if (!f.allocated())
    {
        f.coverage.clear(); f.owner.clear();
        f.res = res;
        return;
    }
    const uint32_t old = f.res;
    const size_t n = static_cast<size_t>(res) * res;
    std::vector<uint8_t>  cov(n);
    std::vector<uint16_t> own(n);
    for (uint32_t z = 0; z < res; ++z)
    {
        const uint32_t sz = std::min<uint32_t>(old - 1, static_cast<uint32_t>((static_cast<double>(z) + 0.5) * old / res));
        for (uint32_t x = 0; x < res; ++x)
        {
            const uint32_t sx = std::min<uint32_t>(old - 1, static_cast<uint32_t>((static_cast<double>(x) + 0.5) * old / res));
            cov[static_cast<size_t>(z) * res + x] = f.coverage[static_cast<size_t>(sz) * old + sx];
            own[static_cast<size_t>(z) * res + x] = f.owner[static_cast<size_t>(sz) * old + sx];
        }
    }
    f.res = res;
    f.coverage = std::move(cov);
    f.owner = std::move(own);
    if (hasArea(tc)) markAll(tc);
}

void clearAll(TerrainComponent& tc)
{
    Field& f = tc.water;
    const bool had = !f.pristine();
    f.res = kDefaultRes;
    f.coverage.clear(); f.coverage.shrink_to_fit();
    f.owner.clear();    f.owner.shrink_to_fit();
    f.bodies.clear();
    f.nextBodyId = 1;
    if (had && hasArea(tc)) markAll(tc);
}

namespace
{
    // Dry every cell `id` owns; reports the cells it changed.
    Touched dryCellsOf(TerrainComponent& tc, uint16_t id)
    {
        Touched t;
        Field& f = tc.water;
        if (!f.allocated()) return t;
        const int res = static_cast<int>(f.res);
        for (int z = 0; z < res; ++z)
            for (int x = 0; x < res; ++x)
            {
                const size_t i = static_cast<size_t>(z) * res + x;
                if (f.owner[i] != id) continue;
                if (f.coverage[i] == 0 && f.owner[i] == 0) continue;
                f.coverage[i] = 0; f.owner[i] = 0;
                t.add(x, z);
            }
        return t;
    }
}

Result clearBody(TerrainComponent& tc, uint16_t id)
{
    Field& f = tc.water;
    if (!f.findBody(id) || !hasArea(tc)) return {};
    return dryCellsOf(tc, id).finish(tc);
}

Result removeBody(TerrainComponent& tc, uint16_t id)
{
    Field& f = tc.water;
    if (!f.findBody(id) || !hasArea(tc)) return {};
    const Result r = dryCellsOf(tc, id).finish(tc);
    f.bodies.erase(std::remove_if(f.bodies.begin(), f.bodies.end(),
                                  [id](const Body& b) { return b.id == id; }), f.bodies.end());
    return r;
}

bool setLevel(TerrainComponent& tc, uint16_t id, float level)
{
    Body* b = tc.water.findBody(id);
    if (!b || !finite(level) || !hasArea(tc)) return false;
    if (b->level == level) return true;
    b->level = level;
    markAll(tc);    // the whole surface of the body moves, not a rectangle of cells
    return true;
}

uint32_t pruneEmptyBodies(TerrainComponent& tc)
{
    Field& f = tc.water;
    std::vector<bool> used(65536, false);
    if (f.allocated())
        for (size_t i = 0; i < f.owner.size(); ++i)
            if (f.coverage[i] > 0) used[f.owner[i]] = true;
    const size_t before = f.bodies.size();
    f.bodies.erase(std::remove_if(f.bodies.begin(), f.bodies.end(),
                                  [&](const Body& b) { return !used[b.id] && !b.fromSpline(); }),
                   f.bodies.end());
    return static_cast<uint32_t>(before - f.bodies.size());
}

// ── Circle ───────────────────────────────────────────────────────────────────

Result addCircle(TerrainComponent& tc, uint16_t body, float x, float z,
                 float radius, float falloff, float strength)
{
    if (!finite(x) || !finite(z) || !finite(radius) || !finite(falloff) || !finite(strength)) return {};
    radius = std::max(0.0f, radius);
    falloff = std::max(0.0f, falloff);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float outer = radius + falloff;
    if (!tc.water.findBody(body) || !hasArea(tc) || outer <= 0.0f || strength <= 0.0f) return {};
    if (!ensureGrid(tc)) return {};

    Field& f = tc.water;
    const int res = static_cast<int>(f.res);
    const CircleCells cc = circleCells(tc, x, z, outer);
    Touched t;
    if (cc.any)
    {
        for (int cz = cc.z0; cz <= cc.z1; ++cz)
            for (int cx = cc.x0; cx <= cc.x1; ++cx)
            {
                const glm::vec2 c = cellCenter(tc, cx, cz);
                const float dist = std::sqrt((c.x - x) * (c.x - x) + (c.y - z) * (c.y - z));
                if (dist > outer) continue;
                const float a = std::clamp(strength * circleWeight(dist, radius, falloff), 0.0f, 1.0f);
                if (a <= 0.0f) continue;

                const size_t i = static_cast<size_t>(cz) * res + cx;
                const uint8_t oc = f.coverage[i];
                const uint8_t nc = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(std::lround(oc + (255.0f - oc) * a)), 0, 255));
                if (nc == oc) continue;
                // A dry cell becomes the brush's; an occupied one keeps its owner.
                f.coverage[i] = nc;
                if (f.owner[i] == kNoBody) f.owner[i] = body;
                t.add(cx, cz);
            }
    }
    return t.finish(tc);
}

Result removeCircle(TerrainComponent& tc, float x, float z, float radius, float falloff,
                    float strength, uint16_t onlyBody)
{
    if (!finite(x) || !finite(z) || !finite(radius) || !finite(falloff) || !finite(strength)) return {};
    radius = std::max(0.0f, radius);
    falloff = std::max(0.0f, falloff);
    strength = std::clamp(strength, 0.0f, 1.0f);
    const float outer = radius + falloff;
    if (!hasArea(tc) || outer <= 0.0f || strength <= 0.0f) return {};
    if (onlyBody != kNoBody && !tc.water.findBody(onlyBody)) return {};
    if (!ensureGrid(tc)) return {};

    Field& f = tc.water;
    const int res = static_cast<int>(f.res);
    const CircleCells cc = circleCells(tc, x, z, outer);
    Touched t;
    if (cc.any)
        for (int cz = cc.z0; cz <= cc.z1; ++cz)
            for (int cx = cc.x0; cx <= cc.x1; ++cx)
            {
                const size_t i = static_cast<size_t>(cz) * res + cx;
                if (f.coverage[i] == 0) continue;
                if (onlyBody != kNoBody && f.owner[i] != onlyBody) continue;
                const glm::vec2 c = cellCenter(tc, cx, cz);
                const float dist = std::sqrt((c.x - x) * (c.x - x) + (c.y - z) * (c.y - z));
                if (dist > outer) continue;
                const float a = std::clamp(strength * circleWeight(dist, radius, falloff), 0.0f, 1.0f);
                if (a <= 0.0f) continue;

                const int drop = static_cast<int>(std::lround(255.0f * a));
                const uint8_t nc = static_cast<uint8_t>(std::max(0, static_cast<int>(f.coverage[i]) - drop));
                if (nc == f.coverage[i]) continue;
                f.coverage[i] = nc;
                if (nc == 0) f.owner[i] = kNoBody;
                t.add(cx, cz);
            }
    const Result r = t.finish(tc);
    if (r.changed) pruneEmptyBodies(tc);
    return r;
}

// ── Polygon ──────────────────────────────────────────────────────────────────

Raster rasterizePolygon(const std::vector<glm::vec2>& polygon, float sizeX, float sizeZ, uint32_t res)
{
    Raster out;
    if (polygon.size() < 3 || res == 0 || !finite(sizeX) || !finite(sizeZ) || sizeX <= 0.0f || sizeZ <= 0.0f)
        return out;
    res = std::min(res, kMaxRes);

    // To cell space, in double: a lake on a 4 km terrain at 4096 cells must not
    // lose its shore to float rounding.
    std::vector<glm::dvec2> p(polygon.size());
    double minU = 1e300, maxU = -1e300, minV = 1e300, maxV = -1e300;
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        if (!finite(polygon[i].x) || !finite(polygon[i].y)) return out;
        p[i].x = (static_cast<double>(polygon[i].x) + sizeX * 0.5) / sizeX * res;
        p[i].y = (static_cast<double>(polygon[i].y) + sizeZ * 0.5) / sizeZ * res;
        minU = std::min(minU, p[i].x); maxU = std::max(maxU, p[i].x);
        minV = std::min(minV, p[i].y); maxV = std::max(maxV, p[i].y);
    }
    const int ires = static_cast<int>(res);
    if (maxU <= 0.0 || minU >= ires || maxV <= 0.0 || minV >= ires) return out;
    const int x0 = std::max(0, static_cast<int>(std::floor(minU)));
    const int x1 = std::min(ires - 1, static_cast<int>(std::ceil(maxU)) - 1);
    const int z0 = std::max(0, static_cast<int>(std::floor(minV)));
    const int z1 = std::min(ires - 1, static_cast<int>(std::ceil(maxV)) - 1);
    if (x0 > x1 || z0 > z1) return out;

    const int w = x1 - x0 + 1, h = z1 - z0 + 1;
    std::vector<uint8_t> cov(static_cast<size_t>(w) * h, 0);
    std::vector<double> acc(w), diff(w + 1), xs;
    bool any = false;

    for (int row = z0; row <= z1; ++row)
    {
        std::fill(acc.begin(), acc.end(), 0.0);
        std::fill(diff.begin(), diff.end(), 0.0);
        for (int s = 0; s < kSubRows; ++s)
        {
            const double y = row + (s + 0.5) / kSubRows;
            xs.clear();
            for (size_t i = 0, n = p.size(); i < n; ++i)
            {
                const glm::dvec2& a = p[i];
                const glm::dvec2& b = p[(i + 1) % n];
                // Half-open on y so a vertex exactly on the scanline counts once.
                if ((a.y <= y) == (b.y <= y)) continue;
                xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2)       // even-odd: between crossing 2k and 2k+1
            {
                const double xa = std::clamp(xs[k], static_cast<double>(x0), static_cast<double>(x1 + 1)) - x0;
                const double xb = std::clamp(xs[k + 1], static_cast<double>(x0), static_cast<double>(x1 + 1)) - x0;
                if (xb <= xa) continue;
                const int ia = static_cast<int>(std::floor(xa)), ib = static_cast<int>(std::floor(xb));
                if (ia == ib) { acc[ia] += xb - xa; continue; }
                acc[ia] += (ia + 1) - xa;
                if (ib > ia + 1) { diff[ia + 1] += 1.0; diff[ib] -= 1.0; }
                if (ib < w) acc[ib] += xb - ib;
            }
        }
        double run = 0.0;
        for (int c = 0; c < w; ++c)
        {
            run += diff[c];
            const double v = (acc[c] + run) / kSubRows;
            const int q = std::clamp(static_cast<int>(std::lround(v * 255.0)), 0, 255);
            cov[static_cast<size_t>(row - z0) * w + c] = static_cast<uint8_t>(q);
            any = any || q > 0;
        }
    }
    if (!any) return out;
    out.x0 = x0; out.z0 = z0; out.w = w; out.h = h;
    out.coverage = std::move(cov);
    return out;
}

Result addPolygon(TerrainComponent& tc, uint16_t body, const std::vector<glm::vec2>& polygon)
{
    if (!tc.water.findBody(body) || !hasArea(tc) || !ensureGrid(tc)) return {};
    Field& f = tc.water;
    const Raster r = rasterizePolygon(polygon, tc.sizeX, tc.sizeZ, f.res);
    if (r.empty()) return {};

    const int res = static_cast<int>(f.res);
    Touched t;
    for (int j = 0; j < r.h; ++j)
        for (int k = 0; k < r.w; ++k)
        {
            const uint8_t p = r.coverage[static_cast<size_t>(j) * r.w + k];
            if (p == 0) continue;
            const int cx = r.x0 + k, cz = r.z0 + j;
            const size_t i = static_cast<size_t>(cz) * res + cx;
            const uint16_t ow = f.owner[i];
            const uint8_t oc = f.coverage[i];
            // The polygon owns what it covers properly; a shore cell it only
            // grazes stays with whoever holds it.
            if (ow != kNoBody && ow != body && p < kWet) continue;
            const uint8_t nc = std::max(oc, p);
            if (nc == oc && ow == body) continue;
            f.coverage[i] = nc;
            f.owner[i] = body;
            t.add(cx, cz);
        }
    return t.finish(tc);
}

Result removePolygon(TerrainComponent& tc, const std::vector<glm::vec2>& polygon, uint16_t onlyBody)
{
    if (!hasArea(tc)) return {};
    if (onlyBody != kNoBody && !tc.water.findBody(onlyBody)) return {};
    if (!ensureGrid(tc)) return {};
    Field& f = tc.water;
    const Raster r = rasterizePolygon(polygon, tc.sizeX, tc.sizeZ, f.res);
    if (r.empty()) return {};

    const int res = static_cast<int>(f.res);
    Touched t;
    for (int j = 0; j < r.h; ++j)
        for (int k = 0; k < r.w; ++k)
        {
            const uint8_t p = r.coverage[static_cast<size_t>(j) * r.w + k];
            if (p == 0) continue;
            const int cx = r.x0 + k, cz = r.z0 + j;
            const size_t i = static_cast<size_t>(cz) * res + cx;
            const uint8_t oc = f.coverage[i];
            if (oc == 0) continue;
            if (onlyBody != kNoBody && f.owner[i] != onlyBody) continue;
            const uint8_t nc = static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(oc * (255 - p) / 255.0f)), 0, 255));
            if (nc == oc) continue;
            f.coverage[i] = nc;
            if (nc == 0) f.owner[i] = kNoBody;
            t.add(cx, cz);
        }
    const Result res2 = t.finish(tc);
    if (res2.changed) pruneEmptyBodies(tc);
    return res2;
}

std::vector<glm::vec2> polygonFromPolyline(const std::vector<glm::vec3>& polyline,
                                           const glm::mat4& toTerrainLocal)
{
    std::vector<glm::vec2> poly;
    poly.reserve(polyline.size());
    for (const glm::vec3& v : polyline)
    {
        const glm::vec4 q = toTerrainLocal * glm::vec4(v, 1.0f);
        poly.emplace_back(q.x, q.z);
    }
    // A closed Curve::polyline ends where it began; a polygon must not.
    if (poly.size() >= 2)
    {
        const glm::vec2 d = poly.back() - poly.front();
        if (std::abs(d.x) < 1e-4f && std::abs(d.y) < 1e-4f) poly.pop_back();
    }
    return poly;
}

// ── Reading ──────────────────────────────────────────────────────────────────

glm::vec2 cellSize(const TerrainComponent& tc)
{
    const float r = static_cast<float>(std::max<uint32_t>(1, tc.water.res));
    return { tc.sizeX / r, tc.sizeZ / r };
}

glm::vec2 cellCenter(const TerrainComponent& tc, int cx, int cz)
{
    const glm::vec2 s = cellSize(tc);
    return { -tc.sizeX * 0.5f + (static_cast<float>(cx) + 0.5f) * s.x,
             -tc.sizeZ * 0.5f + (static_cast<float>(cz) + 0.5f) * s.y };
}

float coverageAt(const TerrainComponent& tc, float x, float z)
{
    const Field& f = tc.water;
    if (!f.allocated() || !hasArea(tc) || !finite(x) || !finite(z)) return 0.0f;
    if (std::abs(x) > tc.sizeX * 0.5f || std::abs(z) > tc.sizeZ * 0.5f) return 0.0f;
    const int res = static_cast<int>(f.res);
    const double fu = toCellX(tc, x) - 0.5, fv = toCellZ(tc, z) - 0.5;
    const int ix = static_cast<int>(std::floor(fu)), iz = static_cast<int>(std::floor(fv));
    const float tx = static_cast<float>(fu - ix), tz = static_cast<float>(fv - iz);
    auto at = [&](int cx, int cz) {
        cx = std::clamp(cx, 0, res - 1); cz = std::clamp(cz, 0, res - 1);
        return static_cast<float>(f.coverage[static_cast<size_t>(cz) * res + cx]);
    };
    const float a = at(ix, iz) * (1.0f - tx) + at(ix + 1, iz) * tx;
    const float b = at(ix, iz + 1) * (1.0f - tx) + at(ix + 1, iz + 1) * tx;
    return (a * (1.0f - tz) + b * tz) / 255.0f;
}

uint16_t bodyAt(const TerrainComponent& tc, float x, float z)
{
    const Field& f = tc.water;
    if (!f.allocated() || !hasArea(tc) || !finite(x) || !finite(z)) return kNoBody;
    if (std::abs(x) > tc.sizeX * 0.5f || std::abs(z) > tc.sizeZ * 0.5f) return kNoBody;
    const int res = static_cast<int>(f.res);
    const int cx = std::clamp(static_cast<int>(std::floor(toCellX(tc, x))), 0, res - 1);
    const int cz = std::clamp(static_cast<int>(std::floor(toCellZ(tc, z))), 0, res - 1);
    const size_t i = static_cast<size_t>(cz) * res + cx;
    return f.coverage[i] >= kWet ? f.owner[i] : kNoBody;
}

bool levelAt(const TerrainComponent& tc, float x, float z, float& level)
{
    const Body* b = tc.water.findBody(bodyAt(tc, x, z));
    if (!b) return false;
    level = b->level;
    return true;
}

CellRect cellRect(const TerrainComponent& tc, float minX, float minZ, float maxX, float maxZ)
{
    CellRect r;
    const Field& f = tc.water;
    if (!f.allocated() || !hasArea(tc) || !finite(minX) || !finite(minZ) || !finite(maxX) || !finite(maxZ))
        return r;
    if (maxX < minX) std::swap(minX, maxX);
    if (maxZ < minZ) std::swap(minZ, maxZ);
    const int res = static_cast<int>(f.res);
    const int lx = static_cast<int>(std::floor(toCellX(tc, minX)));
    const int lz = static_cast<int>(std::floor(toCellZ(tc, minZ)));
    const int hx = std::max(lx, static_cast<int>(std::ceil(toCellX(tc, maxX))) - 1);
    const int hz = std::max(lz, static_cast<int>(std::ceil(toCellZ(tc, maxZ))) - 1);
    if (hx < 0 || hz < 0 || lx >= res || lz >= res) return r;
    r.x0 = std::max(0, lx); r.z0 = std::max(0, lz);
    r.x1 = std::min(res - 1, hx); r.z1 = std::min(res - 1, hz);
    return r;
}

CellRect chunkCellRect(const TerrainComponent& tc, uint32_t chunksPerSide, uint32_t cx, uint32_t cz)
{
    CellRect r;
    const Field& f = tc.water;
    if (!f.allocated() || chunksPerSide == 0 || cx >= chunksPerSide || cz >= chunksPerSide) return r;
    const double res = f.res, n = chunksPerSide;
    // Cell i belongs to the chunk its centre, (i + 0.5) / res, falls in.
    auto first = [&](uint32_t c) { return static_cast<int>(std::ceil(c * res / n - 0.5)); };
    r.x0 = std::max(0, first(cx));     r.x1 = std::min<int>(f.res - 1, first(cx + 1) - 1);
    r.z0 = std::max(0, first(cz));     r.z1 = std::min<int>(f.res - 1, first(cz + 1) - 1);
    if (!r.valid()) r = CellRect{};
    return r;
}

bool anyWater(const TerrainComponent& tc, const CellRect& rect, bool wetOnly)
{
    const Field& f = tc.water;
    if (!f.allocated() || !rect.valid()) return false;
    const int res = static_cast<int>(f.res);
    const uint8_t floorCov = wetOnly ? kWet : 1;
    for (int z = std::max(0, rect.z0); z <= std::min(res - 1, rect.z1); ++z)
        for (int x = std::max(0, rect.x0); x <= std::min(res - 1, rect.x1); ++x)
            if (f.coverage[static_cast<size_t>(z) * res + x] >= floorCov) return true;
    return false;
}

// ── Delta ────────────────────────────────────────────────────────────────────

bool Delta::sameBodies() const
{
    return nextIdBefore == nextIdAfter && bodiesEq(bodiesBefore, bodiesAfter);
}

bool makeDelta(const Field& before, const Field& after, Delta& out)
{
    if (before.res != after.res) return false;
    Delta d;
    d.res = before.res;
    d.bodiesBefore = before.bodies; d.bodiesAfter = after.bodies;
    d.nextIdBefore = before.nextBodyId; d.nextIdAfter = after.nextBodyId;
    if (before.allocated() || after.allocated())
    {
        const size_t n = static_cast<size_t>(before.res) * before.res;
        for (size_t i = 0; i < n; ++i)
        {
            const uint16_t ob = ownerOf(before, i), oa = ownerOf(after, i);
            const uint8_t  cb = covOf(before, i),   ca = covOf(after, i);
            if (ob == oa && cb == ca) continue;
            CellChange c;
            c.index = static_cast<uint32_t>(i);
            c.ownerBefore = ob; c.ownerAfter = oa;
            c.covBefore = cb;   c.covAfter = ca;
            d.cells.push_back(c);
        }
    }
    out = std::move(d);
    return true;
}

namespace
{
    bool replay(TerrainComponent& tc, const Delta& d, bool forward)
    {
        Field& f = tc.water;
        if (f.res != d.res) return false;
        for (const CellChange& c : d.cells)
            if (c.index >= static_cast<size_t>(d.res) * d.res) return false;   // not ours: apply nothing
        if (!d.cells.empty() && !ensureGrid(tc)) return false;

        const int res = static_cast<int>(f.res);
        Touched t;
        for (const CellChange& c : d.cells)
        {
            f.coverage[c.index] = forward ? c.covAfter : c.covBefore;
            f.owner[c.index]    = forward ? c.ownerAfter : c.ownerBefore;
            t.add(static_cast<int>(c.index % res), static_cast<int>(c.index / res));
        }
        f.bodies = forward ? d.bodiesAfter : d.bodiesBefore;
        f.nextBodyId = forward ? d.nextIdAfter : d.nextIdBefore;
        if (hasArea(tc))
        {
            if (t.changed) markCells(tc, t.x0, t.z0, t.x1, t.z1);
            // A level that changed moves a surface no cell records.
            if (!bodiesEq(d.bodiesBefore, d.bodiesAfter)) markAll(tc);
        }
        return true;
    }
}

bool applyDelta(TerrainComponent& tc, const Delta& d)  { return replay(tc, d, true); }
bool revertDelta(TerrainComponent& tc, const Delta& d) { return replay(tc, d, false); }

// ── Persistence ──────────────────────────────────────────────────────────────

std::vector<uint8_t> encodeCells(const Field& f)
{
    std::vector<uint8_t> out;
    if (!f.allocated()) return out;
    const size_t n = f.coverage.size();
    bool anyCell = false;
    for (size_t i = 0; i < n && !anyCell; ++i) anyCell = f.coverage[i] != 0;
    if (!anyCell) return out;

    size_t i = 0;
    while (i < n)
    {
        size_t j = i + 1;
        while (j < n && f.coverage[j] == f.coverage[i] && f.owner[j] == f.owner[i]) ++j;
        putVarint(out, static_cast<uint32_t>(j - i));
        out.push_back(static_cast<uint8_t>(f.owner[i] & 0xFF));
        out.push_back(static_cast<uint8_t>(f.owner[i] >> 8));
        out.push_back(f.coverage[i]);
        i = j;
    }
    return out;
}

bool decodeCells(Field& f, const uint8_t* data, size_t size)
{
    f.coverage.clear();
    f.owner.clear();
    if (f.res == 0 || f.res > kMaxRes || (!data && size)) return false;
    const size_t n = static_cast<size_t>(f.res) * f.res;
    std::vector<uint8_t>  cov(n);
    std::vector<uint16_t> own(n);
    size_t at = 0, filled = 0;
    while (at < size)
    {
        uint32_t count = 0;
        if (!getVarint(data, size, at, count) || count == 0 || size - at < 3) return false;
        if (count > n - filled) return false;               // overruns the grid
        const uint16_t o = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
        const uint8_t  c = data[at + 2];
        at += 3;
        std::fill_n(cov.begin() + filled, count, c);
        std::fill_n(own.begin() + filled, count, o);
        filled += count;
    }
    if (filled != n) return false;                          // truncated
    f.coverage = std::move(cov);
    f.owner = std::move(own);
    return true;
}

void sanitize(TerrainComponent& tc)
{
    Field& f = tc.water;
    f.res = clampRes(f.res);

    // Bodies: id 0, a repeated id or a level that is not a number cannot be kept.
    std::vector<Body> kept;
    kept.reserve(f.bodies.size());
    for (const Body& b : f.bodies)
    {
        if (b.id == kNoBody || !finite(b.level)) continue;
        bool dup = false;
        for (const Body& k : kept) dup = dup || k.id == b.id;
        if (!dup) kept.push_back(b);
    }
    f.bodies = std::move(kept);
    uint32_t next = f.nextBodyId == 0 ? 1u : f.nextBodyId;
    for (const Body& b : f.bodies) if (b.id >= next) next = b.id == 65535u ? 1u : b.id + 1u;
    f.nextBodyId = static_cast<uint16_t>(next);

    // Cells: both rasters or neither, then every cell consistent.
    const size_t n = static_cast<size_t>(f.res) * f.res;
    if (f.coverage.size() != n || f.owner.size() != n) { f.coverage.clear(); f.owner.clear(); }
    if (!f.coverage.empty())
    {
        std::vector<bool> known(65536, false);
        for (const Body& b : f.bodies) known[b.id] = true;
        bool anyCell = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (!known[f.owner[i]] || f.coverage[i] == 0) { f.owner[i] = 0; f.coverage[i] = 0; }
            anyCell = anyCell || f.coverage[i] != 0;
        }
        if (!anyCell) { f.coverage.clear(); f.owner.clear(); }
    }

    // Dirty only for something there is to show: a field that merely carries a
    // non-default resolution has no surface to rebuild.
    if ((!f.bodies.empty() || f.allocated()) && hasArea(tc)) markAll(tc);
}

} // namespace HE::water
