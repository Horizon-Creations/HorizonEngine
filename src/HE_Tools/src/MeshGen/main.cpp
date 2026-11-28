// mesh_gen — generates the engine's built-in default primitive meshes as loose
// .hasset files (StaticMesh), written via ContentManager::saveAsset so the byte
// layout is identical to editor-authored meshes.
//
// Usage:  mesh_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Meshes). It is used verbatim as the ContentManager
//   content root, and each mesh is saved under "<Name>.hasset".
//
// Output is deterministic: geometry is procedural and every mesh gets a stable,
// well-known UUID (see kMeshBaseHi below), so re-running produces byte-identical
// files — no spurious churn, and scene references to these meshes survive.
//
// All primitives are centered at the origin, sized to a nominal 1 unit (radius
// 0.5 / edge 1), with outward normals, sensible UVs, and CCW-from-outside
// winding (enforced by fixWinding()).

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <Types/UUID.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr float kPi = 3.14159265358979323846f;

// Well-known UUID base for the default meshes. hi stays far below the version-4
// bit pattern UUID::generate() enforces (hi & 0xF000 == 0x4000) and clear of the
// DefaultAssets sentinels (hi 1..7), so these never collide with either.
constexpr uint64_t kMeshBaseHi = 0x0000000000000100ULL;

// The entries code names by UUID (DefaultAssets.h) sit at a fixed index below.
static_assert(HE::kEngineCrossCardMeshId.hi == kMeshBaseHi + 8 && HE::kEngineCrossCardMeshId.lo == 1,
              "CrossCard is entry 8 of the list in main()");

struct Mesh
{
    std::vector<float>    pos;   // xyz flat
    std::vector<float>    nrm;   // xyz flat
    std::vector<float>    uv;    // uv flat
    std::vector<uint32_t> idx;   // triangle list

    uint32_t addVertex(float px, float py, float pz,
                       float nx, float ny, float nz,
                       float u, float v)
    {
        const uint32_t i = static_cast<uint32_t>(pos.size() / 3);
        pos.insert(pos.end(), { px, py, pz });
        nrm.insert(nrm.end(), { nx, ny, nz });
        uv.insert(uv.end(),  { u, v });
        return i;
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) { idx.insert(idx.end(), { a, b, c }); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { tri(a, b, c); tri(a, c, d); }
};

// Flip any triangle whose geometric (winding) normal opposes the interpolated
// vertex normal, guaranteeing outward CCW winding regardless of how each
// primitive parametrization happened to order its verts.
void fixWinding(Mesh& m)
{
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
    {
        const uint32_t i0 = m.idx[t], i1 = m.idx[t + 1], i2 = m.idx[t + 2];
        const float* p0 = &m.pos[i0 * 3]; const float* p1 = &m.pos[i1 * 3]; const float* p2 = &m.pos[i2 * 3];
        const float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        const float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        const float g[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        const float n[3] = { m.nrm[i0*3]+m.nrm[i1*3]+m.nrm[i2*3],
                             m.nrm[i0*3+1]+m.nrm[i1*3+1]+m.nrm[i2*3+1],
                             m.nrm[i0*3+2]+m.nrm[i1*3+2]+m.nrm[i2*3+2] };
        if (g[0]*n[0] + g[1]*n[1] + g[2]*n[2] < 0.0f)
            m.idx[t + 1] = i2, m.idx[t + 2] = i1;
    }
}

// ── Primitive builders ─────────────────────────────────────────────────────────

Mesh makeCube()
{
    Mesh m;
    // Six faces, hard per-face normals (24 verts). Matches ContentManager's
    // built-in DefaultCube geometry.
    struct Face { float n[3], u[3], v[3]; };
    const Face faces[6] = {
        {{ 1,0,0},{0,0,-1},{0,1,0}}, {{-1,0,0},{0,0,1},{0,1,0}},
        {{0, 1,0},{1,0,0},{0,0,1}},  {{0,-1,0},{1,0,0},{0,0,-1}},
        {{0,0, 1},{1,0,0},{0,1,0}},  {{0,0,-1},{-1,0,0},{0,1,0}},
    };
    for (const Face& f : faces)
    {
        const float c[3] = { f.n[0]*0.5f, f.n[1]*0.5f, f.n[2]*0.5f };
        uint32_t v[4];
        int k = 0;
        for (int sv = -1; sv <= 1; sv += 2)
            for (int su = -1; su <= 1; su += 2)
            {
                const float uu = static_cast<float>(su) * 0.5f, vv = static_cast<float>(sv) * 0.5f;
                v[k++] = m.addVertex(
                    c[0] + f.u[0]*uu + f.v[0]*vv,
                    c[1] + f.u[1]*uu + f.v[1]*vv,
                    c[2] + f.u[2]*uu + f.v[2]*vv,
                    f.n[0], f.n[1], f.n[2],
                    static_cast<float>(su)*0.5f + 0.5f, static_cast<float>(sv)*0.5f + 0.5f);
            }
        m.quad(v[0], v[1], v[3], v[2]);
    }
    return m;
}

Mesh makePlane()
{
    // 1×1 ground plane in XZ, facing +Y (single quad).
    Mesh m;
    const uint32_t a = m.addVertex(-0.5f, 0, -0.5f, 0,1,0, 0,0);
    const uint32_t b = m.addVertex( 0.5f, 0, -0.5f, 0,1,0, 1,0);
    const uint32_t c = m.addVertex( 0.5f, 0,  0.5f, 0,1,0, 1,1);
    const uint32_t d = m.addVertex(-0.5f, 0,  0.5f, 0,1,0, 0,1);
    m.quad(a, b, c, d);
    return m;
}

Mesh makeQuad()
{
    // 1×1 quad in XY, facing +Z (sprites/billboards).
    Mesh m;
    const uint32_t a = m.addVertex(-0.5f,-0.5f, 0, 0,0,1, 0,0);
    const uint32_t b = m.addVertex( 0.5f,-0.5f, 0, 0,0,1, 1,0);
    const uint32_t c = m.addVertex( 0.5f, 0.5f, 0, 0,0,1, 1,1);
    const uint32_t d = m.addVertex(-0.5f, 0.5f, 0, 0,0,1, 0,1);
    m.quad(a, b, c, d);
    return m;
}

Mesh makeCrossCard()
{
    // The far detail level of a foliage layer (HE::kEngineCrossCardMeshId): two
    // upright 1x1 quads crossed at right angles. Unlike every primitive above the
    // pivot is at the BASE (y 0..1): a foliage instance's origin is the ground,
    // and a centred card would stand half in it. Each plane carries both faces, so
    // it shows from every side with a single-sided material too; every face maps
    // the full 0..1 picture, v = 1 at the top.
    Mesh m;
    struct Plane { float ax, az; };              // the quad's horizontal axis
    const Plane planes[2] = { { 1.0f, 0.0f }, { 0.0f, 1.0f } };
    for (const Plane& p : planes)
        for (int side = 0; side < 2; ++side)
        {
            const float s  = side == 0 ? 1.0f : -1.0f;
            const float nx = -p.az * s, nz = p.ax * s;   // the axis turned a quarter about Y, per side
            const uint32_t a = m.addVertex(-0.5f * p.ax, 0.0f, -0.5f * p.az, nx, 0, nz, 0, 0);
            const uint32_t b = m.addVertex( 0.5f * p.ax, 0.0f,  0.5f * p.az, nx, 0, nz, 1, 0);
            const uint32_t c = m.addVertex( 0.5f * p.ax, 1.0f,  0.5f * p.az, nx, 0, nz, 1, 1);
            const uint32_t d = m.addVertex(-0.5f * p.ax, 1.0f, -0.5f * p.az, nx, 0, nz, 0, 1);
            m.quad(a, b, c, d);                          // fixWinding faces it along its normal
        }
    return m;
}

Mesh makeSphere(int sectors = 32, int rings = 16, float r = 0.5f)
{
    Mesh m;
    for (int i = 0; i <= rings; ++i)
    {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(rings); // 0=top
        const float y = std::cos(phi), sp = std::sin(phi);
        for (int j = 0; j <= sectors; ++j)
        {
            const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(sectors);
            const float x = sp * std::cos(theta), z = sp * std::sin(theta);
            m.addVertex(x*r, y*r, z*r, x, y, z,
                        static_cast<float>(j)/sectors, static_cast<float>(i)/rings);
        }
    }
    const int stride = sectors + 1;
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < sectors; ++j)
        {
            const uint32_t a = i*stride + j, b = a + stride;
            m.quad(a, a+1, b+1, b);
        }
    return m;
}

Mesh makeCylinder(int sectors = 32, float r = 0.5f, float h = 1.0f)
{
    Mesh m;
    const float y0 = -h*0.5f, y1 = h*0.5f;
    // Side wall (duplicated seam vert for clean UVs).
    for (int j = 0; j <= sectors; ++j)
    {
        const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(sectors);
        const float x = std::cos(theta), z = std::sin(theta);
        const float u = static_cast<float>(j)/sectors;
        m.addVertex(x*r, y0, z*r, x, 0, z, u, 0);
        m.addVertex(x*r, y1, z*r, x, 0, z, u, 1);
    }
    for (int j = 0; j < sectors; ++j)
    {
        const uint32_t a = j*2;
        m.quad(a, a+1, a+3, a+2);
    }
    // Caps (triangle fans around a center vertex).
    auto cap = [&](float y, float ny)
    {
        const uint32_t center = m.addVertex(0, y, 0, 0, ny, 0, 0.5f, 0.5f);
        const uint32_t first = static_cast<uint32_t>(m.pos.size()/3);
        for (int j = 0; j <= sectors; ++j)
        {
            const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(sectors);
            const float x = std::cos(theta), z = std::sin(theta);
            m.addVertex(x*r, y, z*r, 0, ny, 0, x*0.5f+0.5f, z*0.5f+0.5f);
        }
        for (int j = 0; j < sectors; ++j)
            m.tri(center, first+j, first+j+1);
    };
    cap(y1,  1.0f);
    cap(y0, -1.0f);
    return m;
}

Mesh makeCone(int sectors = 32, float r = 0.5f, float h = 1.0f)
{
    Mesh m;
    const float y0 = -h*0.5f, apexY = h*0.5f;
    // Slant sides: the apex normal is ill-defined, so give each side triangle its
    // own apex vertex carrying that segment's outward slant normal.
    const float slant = std::sqrt(r*r + h*h);
    const float ny = r / slant;      // vertical component of the outward normal
    const float nr = h / slant;      // radial component
    for (int j = 0; j < sectors; ++j)
    {
        const float t0 = 2.0f*kPi*static_cast<float>(j)/sectors;
        const float t1 = 2.0f*kPi*static_cast<float>(j+1)/sectors;
        const float tm = 0.5f*(t0+t1);
        const float x0 = std::cos(t0), z0 = std::sin(t0);
        const float x1 = std::cos(t1), z1 = std::sin(t1);
        const uint32_t a = m.addVertex(x0*r, y0, z0*r, x0*nr, ny, z0*nr, static_cast<float>(j)/sectors, 0);
        const uint32_t b = m.addVertex(x1*r, y0, z1*r, x1*nr, ny, z1*nr, static_cast<float>(j+1)/sectors, 0);
        const uint32_t c = m.addVertex(0, apexY, 0, std::cos(tm)*nr, ny, std::sin(tm)*nr, (static_cast<float>(j)+0.5f)/sectors, 1);
        m.tri(a, b, c);
    }
    // Base cap.
    const uint32_t center = m.addVertex(0, y0, 0, 0, -1, 0, 0.5f, 0.5f);
    const uint32_t first = static_cast<uint32_t>(m.pos.size()/3);
    for (int j = 0; j <= sectors; ++j)
    {
        const float theta = 2.0f*kPi*static_cast<float>(j)/sectors;
        const float x = std::cos(theta), z = std::sin(theta);
        m.addVertex(x*r, y0, z*r, 0, -1, 0, x*0.5f+0.5f, z*0.5f+0.5f);
    }
    for (int j = 0; j < sectors; ++j)
        m.tri(center, first+j, first+j+1);
    return m;
}

Mesh makeCapsule(int sectors = 32, int capRings = 8, float r = 0.5f, float cyl = 1.0f)
{
    // Cylinder of height `cyl` capped by a hemisphere of radius r on each end
    // (total height = cyl + 2r). Built as a stack of latitude rings: the top
    // hemisphere [phi 0..pi/2] centered at +halfCyl, then the bottom hemisphere
    // [phi pi/2..pi] centered at -halfCyl. The equator ring is duplicated (once
    // per hemisphere), so the quad band between the two — both radius r, both
    // with horizontal normals — is exactly the cylinder wall.
    Mesh m;
    const float halfCyl = cyl*0.5f;

    struct Ring { float sp, cp, centerY; };
    std::vector<Ring> rings;
    for (int k = 0; k <= capRings; ++k) // top hemisphere: phi 0..pi/2
    {
        const float phi = (kPi*0.5f) * static_cast<float>(k)/capRings;
        rings.push_back({ std::sin(phi), std::cos(phi), +halfCyl });
    }
    for (int k = 0; k <= capRings; ++k) // bottom hemisphere: phi pi/2..pi
    {
        const float phi = kPi*0.5f + (kPi*0.5f) * static_cast<float>(k)/capRings;
        rings.push_back({ std::sin(phi), std::cos(phi), -halfCyl });
    }

    const int rows = static_cast<int>(rings.size());
    for (int i = 0; i < rows; ++i)
    {
        const Ring& rg = rings[i];
        for (int j = 0; j <= sectors; ++j)
        {
            const float theta = 2.0f*kPi*static_cast<float>(j)/sectors;
            const float x = rg.sp*std::cos(theta), z = rg.sp*std::sin(theta);
            m.addVertex(x*r, rg.centerY + rg.cp*r, z*r, x, rg.cp, z,
                        static_cast<float>(j)/sectors, static_cast<float>(i)/(rows-1));
        }
    }
    const int stride = sectors + 1;
    for (int i = 0; i < rows-1; ++i)
        for (int j = 0; j < sectors; ++j)
        {
            const uint32_t a = i*stride + j, b = a + stride;
            m.quad(a, a+1, b+1, b);
        }
    return m;
}

Mesh makeTorus(int sectors = 32, int sides = 16, float R = 0.5f, float rr = 0.2f)
{
    Mesh m;
    for (int i = 0; i <= sectors; ++i)
    {
        const float u = 2.0f*kPi*static_cast<float>(i)/sectors;
        const float cu = std::cos(u), su = std::sin(u);
        for (int j = 0; j <= sides; ++j)
        {
            const float v = 2.0f*kPi*static_cast<float>(j)/sides;
            const float cv = std::cos(v), sv = std::sin(v);
            const float x = (R + rr*cv)*cu, y = rr*sv, z = (R + rr*cv)*su;
            m.addVertex(x, y, z, cv*cu, sv, cv*su,
                        static_cast<float>(i)/sectors, static_cast<float>(j)/sides);
        }
    }
    const int stride = sides + 1;
    for (int i = 0; i < sectors; ++i)
        for (int j = 0; j < sides; ++j)
        {
            const uint32_t a = i*stride + j, b = a + stride;
            m.quad(a, a+1, b+1, b);
        }
    return m;
}

// ── Landscape layer meshes (Thema 180) ────────────────────────────────────────
// What a landscape layer scatters (FoliageLayer): a tuft of grass, a pebble, a
// boulder, a drift of snow. Like the cross card they sit with their pivot at the
// ground (a scattered instance's origin is the terrain surface) and their foot a
// little BELOW it, so a stone on a slope does not hover on its downhill side.
// All noise is a hash of the position, never a random stream: the same call gives
// the same bytes, and the detail levels of one shape sample the SAME field, only
// sparser, so a boulder keeps its silhouette when its level changes.

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

// 0..1 from up to three integers.
float rand01(uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
    return static_cast<float>(hash32(a * 0x9E3779B1u ^ hash32(b + 0x85EBCA6Bu ^ hash32(c + 0xC2B2AE35u))) & 0xFFFFFFu)
         / 16777216.0f;
}

// Smooth value noise, -1..1, over a 3D lattice.
float valueNoise(float x, float y, float z, uint32_t seed)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const auto s = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float tx = s(x - fx), ty = s(y - fy), tz = s(z - fz);
    const auto at = [&](int dx, int dy, int dz) {
        return rand01(seed ^ static_cast<uint32_t>(ix + dx) * 73856093u,
                      static_cast<uint32_t>(iy + dy) * 19349663u, static_cast<uint32_t>(iz + dz) * 83492791u) * 2.0f - 1.0f;
    };
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(at(0,0,0), at(1,0,0), tx), lerp(at(0,1,0), at(1,1,0), tx), ty),
                lerp(lerp(at(0,0,1), at(1,0,1), tx), lerp(at(0,1,1), at(1,1,1), tx), ty), tz);
}

float noiseFbm(float x, float y, float z, uint32_t seed, int octaves)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = 1.0f;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * valueNoise(x * f, y * f, z * f, seed + static_cast<uint32_t>(o) * 101u);
        norm += amp; amp *= 0.5f; f *= 2.0f;
    }
    return sum / norm;
}

// A unit icosphere subdivided `levels` times (shared vertices: 12, 42, 162, 642 ...).
void makeIcoDirections(int levels, std::vector<float>& dirs, std::vector<uint32_t>& tris)
{
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const float base[12][3] = { {-1,t,0},{1,t,0},{-1,-t,0},{1,-t,0}, {0,-1,t},{0,1,t},{0,-1,-t},{0,1,-t},
                                {t,0,-1},{t,0,1},{-t,0,-1},{-t,0,1} };
    dirs.clear(); tris.clear();
    for (const auto& v : base)
    {
        const float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        dirs.insert(dirs.end(), { v[0]/l, v[1]/l, v[2]/l });
    }
    tris = { 0,11,5, 0,5,1, 0,1,7, 0,7,10, 0,10,11,  1,5,9, 5,11,4, 11,10,2, 10,7,6, 7,1,8,
             3,9,4, 3,4,2, 3,2,6, 3,6,8, 3,8,9,  4,9,5, 2,4,11, 6,2,10, 8,6,7, 9,8,1 };
    for (int l = 0; l < levels; ++l)
    {
        std::vector<uint32_t> next;
        std::vector<std::pair<uint64_t, uint32_t>> cache;   // (edge key, midpoint), searched linearly: tiny meshes
        const auto mid = [&](uint32_t a, uint32_t b) {
            const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            for (const auto& c : cache) if (c.first == key) return c.second;
            float p[3];
            for (int k = 0; k < 3; ++k) p[k] = dirs[a*3+k] + dirs[b*3+k];
            const float len = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            const uint32_t i = static_cast<uint32_t>(dirs.size() / 3);
            dirs.insert(dirs.end(), { p[0]/len, p[1]/len, p[2]/len });
            cache.emplace_back(key, i);
            return i;
        };
        for (size_t f = 0; f < tris.size(); f += 3)
        {
            const uint32_t a = tris[f], b = tris[f+1], c = tris[f+2];
            const uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            next.insert(next.end(), { a,ab,ca,  b,bc,ab,  c,ca,bc,  ab,bc,ca });
        }
        tris = std::move(next);
    }
}

// Smooth vertex normals from the faces (area weighted), for geometry whose
// vertices were moved after the fact.
void recomputeNormals(Mesh& m)
{
    std::fill(m.nrm.begin(), m.nrm.end(), 0.0f);
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
    {
        const uint32_t i0 = m.idx[t], i1 = m.idx[t+1], i2 = m.idx[t+2];
        const float* p0 = &m.pos[i0*3]; const float* p1 = &m.pos[i1*3]; const float* p2 = &m.pos[i2*3];
        const float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        const float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        const float g[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        for (uint32_t i : { i0, i1, i2 }) for (int k = 0; k < 3; ++k) m.nrm[i*3+k] += g[k];
    }
    for (size_t v = 0; v < m.nrm.size() / 3; ++v)
    {
        float* n = &m.nrm[v*3];
        const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-12f) { n[0] /= len; n[1] /= len; n[2] /= len; }
        else { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
    }
}

struct Stone
{
    float sx, sy, sz;       // size of the body in metres (full extent along each axis)
    float roughness;        // radial noise amplitude, as a share of the radius
    float frequency;        // noise cells across the body
    int   cuts;             // flat faces cut into it (a broken boulder), 0 = a rounded pebble
    float cutDepth;         // how far inside the radius a cut plane sits (0..1 of the radius)
    float foot;             // share of the body's height that sits below the ground
    uint32_t seed;
};

// A noise-pushed icosphere, flattened to the stone's proportions, with planar cuts.
// `levels` is the subdivision (the detail level), `octaves` the noise detail.
Mesh makeStone(const Stone& s, int levels, int octaves)
{
    std::vector<float> dirs; std::vector<uint32_t> tris;
    makeIcoDirections(levels, dirs, tris);

    // The cut planes: directions from the seed, never from straight below (that cut
    // is in the ground anyway) and mostly from the sides and above.
    float cutN[4][3];
    const int cuts = std::min(s.cuts, 4);
    for (int c = 0; c < cuts; ++c)
    {
        const float az = 6.2831853f * rand01(s.seed, 1000u + c);
        const float el = (0.15f + 0.85f * rand01(s.seed, 2000u + c)) * 1.5707963f;
        cutN[c][0] = std::cos(az) * std::cos(el);
        cutN[c][1] = std::sin(el);
        cutN[c][2] = std::sin(az) * std::cos(el);
    }

    Mesh m;
    const size_t vc = dirs.size() / 3;
    for (size_t v = 0; v < vc; ++v)
    {
        const float dx = dirs[v*3], dy = dirs[v*3+1], dz = dirs[v*3+2];
        const float r = 0.5f * (1.0f + s.roughness * noiseFbm(dx * s.frequency, dy * s.frequency, dz * s.frequency, s.seed, octaves));
        float p[3] = { dx * r * s.sx, dy * r * s.sy, dz * r * s.sz };   // r is ~0.5: the extent is sx, sy, sz
        for (int c = 0; c < cuts; ++c)
        {
            // The plane sits `cutDepth` of the ellipsoid's reach along the cut's direction.
            const float hx = 0.5f * s.sx * cutN[c][0], hy = 0.5f * s.sy * cutN[c][1], hz = 0.5f * s.sz * cutN[c][2];
            const float d = s.cutDepth * std::sqrt(hx*hx + hy*hy + hz*hz);
            const float over = p[0]*cutN[c][0] + p[1]*cutN[c][1] + p[2]*cutN[c][2] - d;
            if (over > 0.0f) for (int k = 0; k < 3; ++k) p[k] -= cutN[c][k] * over;
        }
        // Sit on the ground: the body's centre is half a height up, less the foot.
        p[1] += s.sy * (0.5f - s.foot);
        m.addVertex(p[0], p[1], p[2], dx, dy, dz, p[0] / s.sx + 0.5f, p[2] / s.sz + 0.5f);
    }
    m.idx = std::move(tris);
    recomputeNormals(m);
    return m;
}

// A low drift: a flattened dome, flat underneath, sunk into the ground.
Mesh makeDrift(int levels, int octaves)
{
    std::vector<float> dirs; std::vector<uint32_t> tris;
    makeIcoDirections(levels, dirs, tris);
    Mesh m;
    const uint32_t seed = 0x5C0DEu;
    for (size_t v = 0; v < dirs.size() / 3; ++v)
    {
        const float dx = dirs[v*3], dy = dirs[v*3+1], dz = dirs[v*3+2];
        const float r = 0.5f * (1.0f + 0.22f * noiseFbm(dx * 1.6f, dy * 1.6f, dz * 1.6f, seed, octaves));
        const float x = dx * r * 1.2f, z = dz * r * 0.9f;     // ~1.2 m by 0.9 m
        const float y = std::max(dy * r * 0.5f, -0.04f);      // ~0.25 m high, the underside a flat plate
        m.addVertex(x, y, z, dx, dy, dz, x / 1.2f + 0.5f, z / 0.9f + 0.5f);
    }
    m.idx = std::move(tris);
    recomputeNormals(m);
    return m;
}

// A tuft: `blades` grass blades, each a tapering, bent strip of `segments` quads, a
// face in front and one behind (own vertices, opposite normals) so it shows from
// both sides under a single-sided material. About 0.5 m tall, 0.2 m across.
Mesh makeTuft(int blades, int segments)
{
    const uint32_t seed = 0x7F7Fu;
    Mesh m;
    for (int b = 0; b < blades; ++b)
    {
        const float heading = 6.2831853f * (static_cast<float>(b) + 0.6f * rand01(seed, b, 1)) / static_cast<float>(blades);
        const float radius  = 0.01f + 0.07f * rand01(seed, b, 2);
        const float height  = 0.28f + 0.22f * rand01(seed, b, 3);
        const float width   = 0.035f + 0.02f * rand01(seed, b, 4);
        const float lean    = 0.08f + 0.20f * rand01(seed, b, 5);
        const float yaw     = 6.2831853f * rand01(seed, b, 6);
        const float ox = std::cos(heading), oz = std::sin(heading);       // outward, where the tip leans
        const float ax = std::cos(yaw),     az = std::sin(yaw);           // across the blade
        for (int side = 0; side < 2; ++side)
        {
            const float sgn = side == 0 ? 1.0f : -1.0f;
            uint32_t row[2][64];
            for (int r = 0; r <= segments; ++r)
            {
                const float t  = static_cast<float>(r) / static_cast<float>(segments);
                const float cx = ox * radius + ox * lean * t * t, cy = height * t, cz = oz * radius + oz * lean * t * t;
                const float hw = 0.5f * width * (1.0f - 0.85f * t);
                // d(centre)/dt, then the strip's normal = across x tangent.
                float tx = ox * lean * 2.0f * t, ty = height, tz = oz * lean * 2.0f * t;
                const float tl = std::sqrt(tx*tx + ty*ty + tz*tz); tx /= tl; ty /= tl; tz /= tl;
                float nx = 0.0f * tz - az * ty, ny = az * tx - ax * tz, nz = ax * ty - 0.0f * tx;
                const float nl = std::sqrt(nx*nx + ny*ny + nz*nz); nx = sgn * nx / nl; ny = sgn * ny / nl; nz = sgn * nz / nl;
                row[0][r] = m.addVertex(cx - ax * hw, cy, cz - az * hw, nx, ny, nz, 0.0f, t);
                row[1][r] = m.addVertex(cx + ax * hw, cy, cz + az * hw, nx, ny, nz, 1.0f, t);
            }
            for (int r = 0; r < segments; ++r)
                m.quad(row[0][r], row[1][r], row[1][r+1], row[0][r+1]);   // fixWinding faces it along its normal
        }
    }
    return m;
}

// A bunch of strips for the plants of the Forest and Grassland sets (Fern, Tall Grass, Flower
// Tuft). Same construction as the tuft: a bent strip per blade, a face in front and one behind.
// What differs: `drop` bends the blade over (a fern frond arches, a stem stands), `leaf` gives the
// strip a belly instead of a taper, `yawSpread` how far a strip may turn from "flat to the
// outside" (pi: any way, like grass), `heads` a flower on every second blade. Blade 0 always
// has the full height and a fixed place so the detail levels, which have fewer blades, keep the
// bunch's height; the foot sits 2 cm below the ground like the stones do.
struct Bunch
{
    float radius, height, width, lean, drop;
    bool  leaf;
    float yawSpread;
    bool  heads;
    float headRadius;
    uint32_t seed;
};

Mesh makeBunch(const Bunch& s, int blades, int segments)
{
    Mesh m;
    for (int b = 0; b < blades; ++b)
    {
        const float heading = 6.2831853f * (static_cast<float>(b) + 0.4f * (b == 0 ? 0.0f : rand01(s.seed, b, 1))) / static_cast<float>(blades);
        const float radius  = s.radius * (b == 0 ? 1.0f : 0.4f + 0.6f * rand01(s.seed, b, 2));
        const float height  = s.height * (b == 0 ? 1.0f : 0.7f + 0.3f * rand01(s.seed, b, 3));
        const float width   = s.width  * (0.85f + 0.3f * rand01(s.seed, b, 4));
        const float lean    = s.lean   * (0.7f + 0.6f * rand01(s.seed, b, 5));
        const float yaw     = heading + 1.5707963f + s.yawSpread * (rand01(s.seed, b, 6) * 2.0f - 1.0f);
        const float ox = std::cos(heading), oz = std::sin(heading);
        const float ax = std::cos(yaw),     az = std::sin(yaw);
        float tipC[3] = {}, tipN[3] = {};
        for (int side = 0; side < 2; ++side)
        {
            const float sgn = side == 0 ? 1.0f : -1.0f;
            uint32_t row[2][64];
            for (int r = 0; r <= segments; ++r)
            {
                const float t  = static_cast<float>(r) / static_cast<float>(segments);
                const float cx = ox * (radius + lean * t * t), cy = -0.02f + height * (t - s.drop * t * t), cz = oz * (radius + lean * t * t);
                const float prof = s.leaf ? std::max(0.10f, std::sin(3.1415927f * std::pow(t, 0.7f))) : 1.0f - 0.85f * t;
                const float hw = 0.5f * width * prof;
                float tx = ox * lean * 2.0f * t, ty = height * (1.0f - 2.0f * s.drop * t), tz = oz * lean * 2.0f * t;
                const float tl = std::sqrt(tx*tx + ty*ty + tz*tz); tx /= tl; ty /= tl; tz /= tl;
                float nx = 0.0f * tz - az * ty, ny = az * tx - ax * tz, nz = ax * ty - 0.0f * tx;
                const float nl = std::sqrt(nx*nx + ny*ny + nz*nz); nx = sgn * nx / nl; ny = sgn * ny / nl; nz = sgn * nz / nl;
                row[0][r] = m.addVertex(cx - ax * hw, cy, cz - az * hw, nx, ny, nz, 0.0f, t);
                row[1][r] = m.addVertex(cx + ax * hw, cy, cz + az * hw, nx, ny, nz, 1.0f, t);
                if (side == 0 && r == segments) { tipC[0] = cx; tipC[1] = cy; tipC[2] = cz; tipN[0] = nx; tipN[1] = ny; tipN[2] = nz; }
            }
            for (int r = 0; r < segments; ++r)
                m.quad(row[0][r], row[1][r], row[1][r+1], row[0][r+1]);
        }
        if (s.heads && (b % 2) == 0)
        {
            // A flower: a six-sided disc at the tip, facing up and a little outward, both faces.
            const float up[3] = { ox * 0.3f, 1.0f, oz * 0.3f };
            const float ul = std::sqrt(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
            const float n[3] = { up[0]/ul, up[1]/ul, up[2]/ul };
            const float vx = n[1]*az - n[2]*0.0f, vy = n[2]*ax - n[0]*az, vz = n[0]*0.0f - n[1]*ax;   // n x across
            for (int face = 0; face < 2; ++face)
            {
                const float f = face == 0 ? 1.0f : -1.0f;
                const uint32_t c = m.addVertex(tipC[0], tipC[1], tipC[2], f*n[0], f*n[1], f*n[2], 0.5f, 0.5f);
                uint32_t ring[7];
                for (int k = 0; k <= 6; ++k)
                {
                    const float a = 6.2831853f * static_cast<float>(k % 6) / 6.0f;
                    const float cu = std::cos(a), cv = std::sin(a);
                    ring[k] = m.addVertex(tipC[0] + s.headRadius * (cu * ax + cv * vx), tipC[1] + s.headRadius * cv * vy,
                                          tipC[2] + s.headRadius * (cu * az + cv * vz), f*n[0], f*n[1], f*n[2],
                                          0.5f + 0.5f * cu, 0.5f + 0.5f * cv);
                }
                for (int k = 0; k < 6; ++k) m.tri(c, ring[k], ring[k+1]);
            }
            (void)tipN;
        }
    }
    return m;
}

// A tree stump: a flared trunk piece with a rough top, the base sunk 6 cm. Noise is a function
// of the angle and height, so every detail level (fewer sectors and rings) wears the same bark.
Mesh makeStump(int sectors, int rings, int octaves)
{
    const uint32_t seed = 0x57A3Fu;
    const float y0 = -0.06f, y1 = 0.42f;
    Mesh m;
    const auto radiusAt = [&](float ang, float t) {
        const float flare = 1.0f + 0.55f * std::exp(-5.0f * t);
        return 0.26f * flare * (1.0f + 0.14f * noiseFbm(std::cos(ang) * 1.8f, t * 2.5f, std::sin(ang) * 1.8f, seed, octaves));
    };
    for (int r = 0; r <= rings; ++r)
    {
        const float t = static_cast<float>(r) / static_cast<float>(rings);
        const float y = y0 + (y1 - y0) * t;
        for (int j = 0; j <= sectors; ++j)
        {
            const float ang = 6.2831853f * static_cast<float>(j % sectors) / static_cast<float>(sectors);
            const float rad = radiusAt(ang, t) * (r == rings ? 0.92f : 1.0f);
            const float cu = std::cos(ang), cv = std::sin(ang);
            m.addVertex(cu * rad, y, cv * rad, cu * 0.98058f, 0.19612f, cv * 0.98058f,   // (cu, 0.2, cv) normalised
                        static_cast<float>(j) / static_cast<float>(sectors), t * 0.6f);
        }
    }
    const int stride = sectors + 1;
    for (int r = 0; r < rings; ++r)
        for (int j = 0; j < sectors; ++j)
        {
            const uint32_t a = r * stride + j;
            m.quad(a, a + stride, a + stride + 1, a + 1);
        }
    // The top: a fan around a centre sunk a little (a weathered, slightly dished cut).
    const uint32_t c = m.addVertex(0.0f, y1 - 0.025f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f);
    const uint32_t top = static_cast<uint32_t>(m.pos.size() / 3);
    for (int j = 0; j <= sectors; ++j)
    {
        const float ang = 6.2831853f * static_cast<float>(j % sectors) / static_cast<float>(sectors);
        const float rad = radiusAt(ang, 1.0f) * 0.92f;
        m.addVertex(std::cos(ang) * rad, y1, std::sin(ang) * rad, 0.0f, 1.0f, 0.0f, 0.5f + 0.5f * std::cos(ang), 0.5f + 0.5f * std::sin(ang));
    }
    for (int j = 0; j < sectors; ++j) m.tri(c, top + j, top + j + 1);
    fixWinding(m);
    return m;
}

// A fallen log: a rough cylinder lying along x, 1.6 m long, 17 cm of radius, sunk 3 cm. One end
// is cut square, the other ragged; the noise is a function of the position along the log.
Mesh makeLog(int sectors, int rings, int octaves)
{
    const uint32_t seed = 0x1066Au;
    const float halfLen = 0.8f, cy = 0.14f;
    Mesh m;
    const auto radiusAt = [&](float ang, float x) {
        const float taper = 1.0f - 0.12f * (x / halfLen * 0.5f + 0.5f);
        return 0.17f * taper * (1.0f + 0.10f * noiseFbm(x * 2.2f, std::cos(ang) * 1.6f, std::sin(ang) * 1.6f, seed, octaves));
    };
    for (int r = 0; r <= rings; ++r)
    {
        const float x = -halfLen + 2.0f * halfLen * static_cast<float>(r) / static_cast<float>(rings);
        for (int j = 0; j <= sectors; ++j)
        {
            const float ang = 6.2831853f * static_cast<float>(j % sectors) / static_cast<float>(sectors);
            const float rad = radiusAt(ang, x);
            const float cu = std::cos(ang), cv = std::sin(ang);
            m.addVertex(x, cy + cu * rad, cv * rad, 0.0f, cu, cv, x / (2.0f * halfLen) * 4.0f, static_cast<float>(j) / static_cast<float>(sectors));
        }
    }
    const int stride = sectors + 1;
    for (int r = 0; r < rings; ++r)
        for (int j = 0; j < sectors; ++j)
        {
            const uint32_t a = r * stride + j;
            m.quad(a, a + stride, a + stride + 1, a + 1);
        }
    for (int end = 0; end < 2; ++end)
    {
        const float x = end == 0 ? -halfLen : halfLen, nx = end == 0 ? -1.0f : 1.0f;
        const uint32_t c = m.addVertex(x + nx * (end == 0 ? 0.0f : 0.03f), cy, 0.0f, nx, 0.0f, 0.0f, 0.5f, 0.5f);
        const uint32_t first = static_cast<uint32_t>(m.pos.size() / 3);
        for (int j = 0; j <= sectors; ++j)
        {
            const float ang = 6.2831853f * static_cast<float>(j % sectors) / static_cast<float>(sectors);
            const float rad = radiusAt(ang, x);
            const float cu = std::cos(ang), cv = std::sin(ang);
            m.addVertex(x, cy + cu * rad, cv * rad, nx, 0.0f, 0.0f, 0.5f + 0.5f * cu, 0.5f + 0.5f * cv);
        }
        for (int j = 0; j < sectors; ++j) m.tri(c, first + j, first + j + 1);
    }
    fixWinding(m);
    return m;
}

// A ribbed tube swept along a path (a cactus column or arm). `pts` runs from the base (or from
// inside the trunk, for an arm) to the tip; the last point gets a flat cap. The frame is carried
// along the path (no twist at the elbow), the ribs are a function of the angle, so every detail
// level (fewer sectors) wears the same shape.
struct PathPt { float p[3]; float r; };

void addRibbedTube(Mesh& m, const std::vector<PathPt>& path, int sectors, int ribs)
{
    const size_t n = path.size();
    float nx[3] = {}, tprev[3] = {};
    const uint32_t first = static_cast<uint32_t>(m.pos.size() / 3);
    for (size_t i = 0; i < n; ++i)
    {
        const PathPt& a = path[i == 0 ? 0 : i - 1];
        const PathPt& b = path[i + 1 < n ? i + 1 : i];
        float t[3] = { b.p[0] - a.p[0], b.p[1] - a.p[1], b.p[2] - a.p[2] };
        const float tl = std::sqrt(t[0]*t[0] + t[1]*t[1] + t[2]*t[2]);
        t[0] /= tl; t[1] /= tl; t[2] /= tl;
        if (i == 0)
        {
            const float ref[3] = { std::fabs(t[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(t[0]) < 0.9f ? 0.0f : 1.0f, 0.0f };
            nx[0] = ref[0]; nx[1] = ref[1]; nx[2] = ref[2];
        }
        // Parallel transport: the previous normal, made perpendicular to the new tangent.
        const float d = nx[0]*t[0] + nx[1]*t[1] + nx[2]*t[2];
        nx[0] -= d * t[0]; nx[1] -= d * t[1]; nx[2] -= d * t[2];
        const float nl = std::sqrt(nx[0]*nx[0] + nx[1]*nx[1] + nx[2]*nx[2]);
        nx[0] /= nl; nx[1] /= nl; nx[2] /= nl;
        const float bx[3] = { t[1]*nx[2] - t[2]*nx[1], t[2]*nx[0] - t[0]*nx[2], t[0]*nx[1] - t[1]*nx[0] };
        for (int j = 0; j <= sectors; ++j)
        {
            const float ang = 6.2831853f * static_cast<float>(j % sectors) / static_cast<float>(sectors);
            const float cu = std::cos(ang), cv = std::sin(ang);
            const float rad = path[i].r * (1.0f + 0.10f * std::cos(static_cast<float>(ribs) * ang));
            const float dir[3] = { cu * nx[0] + cv * bx[0], cu * nx[1] + cv * bx[1], cu * nx[2] + cv * bx[2] };
            m.addVertex(path[i].p[0] + dir[0] * rad, path[i].p[1] + dir[1] * rad, path[i].p[2] + dir[2] * rad,
                        dir[0], dir[1], dir[2], static_cast<float>(j) / static_cast<float>(sectors),
                        static_cast<float>(i) / static_cast<float>(n - 1));
        }
        tprev[0] = t[0]; tprev[1] = t[1]; tprev[2] = t[2];
    }
    const uint32_t stride = static_cast<uint32_t>(sectors + 1);
    for (uint32_t r = 0; r + 1 < n; ++r)
        for (uint32_t j = 0; j < static_cast<uint32_t>(sectors); ++j)
        {
            const uint32_t a = first + r * stride + j;
            m.quad(a, a + stride, a + stride + 1, a + 1);
        }
    // The cap: a fan around the tip point, facing along the last tangent.
    const PathPt& tip = path[n - 1];
    const uint32_t c = m.addVertex(tip.p[0], tip.p[1], tip.p[2], tprev[0], tprev[1], tprev[2], 0.5f, 0.5f);
    const uint32_t ring = first + static_cast<uint32_t>(n - 1) * stride;
    const uint32_t cap = static_cast<uint32_t>(m.pos.size() / 3);
    for (uint32_t j = 0; j <= static_cast<uint32_t>(sectors); ++j)
        m.addVertex(m.pos[(ring + j) * 3], m.pos[(ring + j) * 3 + 1], m.pos[(ring + j) * 3 + 2],
                    tprev[0], tprev[1], tprev[2], 0.5f + 0.5f * std::cos(6.2831853f * j / sectors),
                    0.5f + 0.5f * std::sin(6.2831853f * j / sectors));
    for (uint32_t j = 0; j < static_cast<uint32_t>(sectors); ++j) m.tri(c, cap + j, cap + j + 1);
}

// A dome closing a tube that ends at `top` going up: three rings of a quarter circle.
void domeUp(std::vector<PathPt>& path, float x, float y, float z, float r)
{
    for (float a : { 0.5f, 1.0f, 1.3f })
        path.push_back({ { x, y + r * std::sin(a), z }, r * std::cos(a) });
}

// A small saguaro: a 0.95 m ribbed column with two arms that leave it sideways and turn up (the
// layer's scale range makes the big ones; the mesh stays under the 1.2 m every layer mesh keeps to).
// The foot sits 6 cm below the ground; the arms start inside the trunk so no seam shows.
Mesh makeCactus(int sectors, int segments)
{
    Mesh m;
    const float k = 0.6f, R = 0.15f * k, H = 1.55f * k;
    std::vector<PathPt> trunk;
    for (int i = 0; i <= segments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        trunk.push_back({ { 0.0f, -0.06f + (H + 0.06f) * t, 0.0f }, R * (1.0f - 0.08f * t) });
    }
    domeUp(trunk, 0.0f, H, 0.0f, R * 0.92f);
    addRibbedTube(m, trunk, sectors, 8);

    struct Arm { float heading, base, reach, rise, r; };
    for (const Arm& a : { Arm{ 0.35f, 0.65f * k, 0.34f * k, 0.50f * k, 0.085f * k }, Arm{ 3.65f, 0.95f * k, 0.28f * k, 0.34f * k, 0.075f * k } })
    {
        const float ox = std::cos(a.heading), oz = std::sin(a.heading);
        const float bend = 0.11f * k;
        std::vector<PathPt> arm;
        for (float d : { 0.0f, 0.5f, 1.0f })
            arm.push_back({ { ox * (0.04f + (a.reach - 0.04f) * d), a.base, oz * (0.04f + (a.reach - 0.04f) * d) }, a.r });
        for (float th : { 0.4f, 0.8f, 1.2f, 1.5708f })
            arm.push_back({ { ox * (a.reach + bend * std::sin(th)), a.base + bend * (1.0f - std::cos(th)), oz * (a.reach + bend * std::sin(th)) }, a.r });
        const float x = ox * (a.reach + bend), z = oz * (a.reach + bend), y0 = a.base + bend;
        const int up = std::max(1, segments / 2);
        for (int i = 1; i <= up; ++i)
            arm.push_back({ { x, y0 + a.rise * static_cast<float>(i) / static_cast<float>(up), z }, a.r });
        domeUp(arm, x, y0 + a.rise, z, a.r * 0.92f);
        addRibbedTube(m, arm, std::max(6, sectors - 2), 6);
    }
    fixWinding(m);
    return m;
}

// The stone shapes of the other sets: a rounder, bigger boulder (Forest), a flat chip (Grassland).
const Stone kMossRock = { 1.10f, 0.62f, 0.95f, 0.16f, 1.6f, 1, 0.90f, 0.22f, 0x30551u };
const Stone kScree    = { 0.34f, 0.13f, 0.24f, 0.28f, 2.0f, 2, 0.80f, 0.25f, 0x5C4EEu };
// The Desert set (step 6): a wide low sandstone block with a few flat cuts, a thin arching dune grass.
const Stone kSandstone = { 1.30f, 0.62f, 1.05f, 0.14f, 1.4f, 3, 0.86f, 0.20f, 0x5A7D5u };

//                              radius height width  lean  drop  leaf  yawSpread heads headR seed
const Bunch kFern       = {  0.04f, 0.50f, 0.15f, 0.45f, 0.45f, true,  0.35f,   false, 0.0f,  0xFE12u };
const Bunch kTallGrass  = {  0.09f, 0.85f, 0.04f, 0.14f, 0.0f,  false, 3.1416f, false, 0.0f,  0x7A11u };
const Bunch kFlowerTuft = {  0.07f, 0.32f, 0.04f, 0.07f, 0.0f,  false, 3.1416f, true,  0.045f, 0xF10Eu };
const Bunch kDuneGrass  = {  0.05f, 0.60f, 0.025f, 0.22f, 0.30f, false, 3.1416f, false, 0.0f,  0xD07Eu };

// The layer meshes, one entry per detail level (level 0 first). Each kind's levels are
// consecutive in the table in main() so their UUIDs are consecutive too.
const Stone kRockA   = { 1.00f, 0.70f, 0.85f, 0.30f, 1.8f, 3, 0.82f, 0.18f, 0xA11CEu };
const Stone kRockB   = { 0.85f, 0.95f, 0.75f, 0.34f, 2.1f, 4, 0.80f, 0.15f, 0xB0B0Fu };
const Stone kRockC   = { 1.15f, 0.50f, 0.95f, 0.26f, 1.5f, 2, 0.85f, 0.20f, 0xC0FFEu };
const Stone kPebble  = { 0.16f, 0.09f, 0.13f, 0.14f, 2.0f, 0, 1.00f, 0.25f, 0x9EB1Eu };

// Sanity-check generated geometry: finite positions, unit-length normals, valid
// indices, non-degenerate triangles. Returns a human-readable problem or "" if OK.
// Also fills bounds for the caller to print.
std::string verify(const Mesh& m, float bmin[3], float bmax[3])
{
    const size_t vc = m.pos.size() / 3;
    if (vc == 0 || m.idx.empty()) return "empty mesh";
    if (m.nrm.size() != m.pos.size()) return "normal/position count mismatch";
    if (m.uv.size() != vc * 2)        return "uv/vertex count mismatch";
    if (m.idx.size() % 3 != 0)        return "index count not a multiple of 3";

    for (int k = 0; k < 3; ++k) { bmin[k] = 1e30f; bmax[k] = -1e30f; }
    for (size_t v = 0; v < vc; ++v)
    {
        for (int k = 0; k < 3; ++k)
        {
            const float p = m.pos[v*3+k];
            if (!std::isfinite(p)) return "non-finite position";
            bmin[k] = std::min(bmin[k], p);
            bmax[k] = std::max(bmax[k], p);
        }
        const float nx = m.nrm[v*3], ny = m.nrm[v*3+1], nz = m.nrm[v*3+2];
        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len < 0.99f || len > 1.01f) return "non-unit normal";
    }
    for (uint32_t i : m.idx)
        if (i >= vc) return "index out of range";
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
        if (m.idx[t] == m.idx[t+1] || m.idx[t+1] == m.idx[t+2] || m.idx[t] == m.idx[t+2])
            return "degenerate triangle";
    return "";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: mesh_gen <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    ContentManager cm(outDir);

    const std::vector<std::pair<const char*, Mesh>> meshes = {
        { "Cube",     makeCube() },
        { "Sphere",   makeSphere() },
        { "Cylinder", makeCylinder() },
        { "Cone",     makeCone() },
        { "Plane",    makePlane() },
        { "Quad",     makeQuad() },
        { "Capsule",  makeCapsule() },
        { "Torus",    makeTorus() },
        // Append only: the index is the UUID, and scenes refer to these by it.
        { "CrossCard", makeCrossCard() },
        // Landscape layer meshes (DefaultAssets.h), each kind's detail levels in a row.
        { "Tuft",           makeTuft(9, 4) },
        { "TuftLod1",       makeTuft(5, 2) },
        { "TuftLod2",       makeTuft(3, 1) },
        { "Pebble",         makeStone(kPebble, 2, 3) },
        { "PebbleLod1",     makeStone(kPebble, 1, 1) },
        { "RockA",          makeStone(kRockA, 3, 3) },
        { "RockALod1",      makeStone(kRockA, 2, 2) },
        { "RockALod2",      makeStone(kRockA, 1, 1) },
        { "RockB",          makeStone(kRockB, 3, 3) },
        { "RockBLod1",      makeStone(kRockB, 2, 2) },
        { "RockBLod2",      makeStone(kRockB, 1, 1) },
        { "RockC",          makeStone(kRockC, 3, 3) },
        { "RockCLod1",      makeStone(kRockC, 2, 2) },
        { "RockCLod2",      makeStone(kRockC, 1, 1) },
        { "SnowDrift",      makeDrift(3, 3) },
        { "SnowDriftLod1",  makeDrift(2, 1) },
        // Landscape 3.0 (Thema 233): the Forest and Grassland sets. Append only, ids 0x119..0x12A.
        { "Fern",           makeBunch(kFern, 8, 5) },
        { "FernLod1",       makeBunch(kFern, 5, 3) },
        { "FernLod2",       makeBunch(kFern, 4, 2) },
        { "ForestStump",    makeStump(14, 6, 3) },
        { "ForestStumpLod1", makeStump(8, 3, 2) },
        { "FallenLog",      makeLog(12, 8, 3) },
        { "FallenLogLod1",  makeLog(8, 4, 2) },
        { "MossRock",       makeStone(kMossRock, 3, 3) },
        { "MossRockLod1",   makeStone(kMossRock, 2, 2) },
        { "MossRockLod2",   makeStone(kMossRock, 1, 1) },
        { "TallGrass",      makeBunch(kTallGrass, 11, 5) },
        { "TallGrassLod1",  makeBunch(kTallGrass, 7, 3) },
        { "TallGrassLod2",  makeBunch(kTallGrass, 4, 2) },
        { "FlowerTuft",     makeBunch(kFlowerTuft, 9, 4) },
        { "FlowerTuftLod1", makeBunch(kFlowerTuft, 6, 2) },
        { "FlowerTuftLod2", makeBunch(kFlowerTuft, 4, 1) },
        { "ScreeChip",      makeStone(kScree, 2, 2) },
        { "ScreeChipLod1",  makeStone(kScree, 1, 1) },
        // The Desert set (step 6), ids 0x12B..0x132.
        { "DuneGrass",      makeBunch(kDuneGrass, 12, 5) },
        { "DuneGrassLod1",  makeBunch(kDuneGrass, 8, 3) },
        { "DuneGrassLod2",  makeBunch(kDuneGrass, 5, 2) },
        { "Cactus",         makeCactus(14, 8) },
        { "CactusLod1",     makeCactus(8, 4) },
        { "Sandstone",      makeStone(kSandstone, 3, 3) },
        { "SandstoneLod1",  makeStone(kSandstone, 2, 2) },
        { "SandstoneLod2",  makeStone(kSandstone, 1, 1) },
    };
    // The entries code names by UUID sit at a fixed index: the entry index is the id.
    const auto indexOf = [&](const char* name) {
        for (size_t i = 0; i < meshes.size(); ++i) if (std::string(meshes[i].first) == name) return static_cast<int>(i);
        return -1;
    };
    for (const auto& [name, id] : { std::pair<const char*, HE::UUID>{ "Tuft", HE::kEngineTuftMeshId },
                                    { "Pebble", HE::kEnginePebbleMeshId }, { "RockA", HE::kEngineRockAMeshId },
                                    { "RockB", HE::kEngineRockBMeshId },   { "RockC", HE::kEngineRockCMeshId },
                                    { "SnowDrift", HE::kEngineSnowDriftMeshId },
                                    { "Fern", HE::kEngineFernMeshId },     { "ForestStump", HE::kEngineForestStumpMeshId },
                                    { "FallenLog", HE::kEngineFallenLogMeshId }, { "MossRock", HE::kEngineMossRockMeshId },
                                    { "TallGrass", HE::kEngineTallGrassMeshId }, { "FlowerTuft", HE::kEngineFlowerTuftMeshId },
                                    { "ScreeChip", HE::kEngineScreeChipMeshId },
                                    { "DuneGrass", HE::kEngineDuneGrassMeshId }, { "Cactus", HE::kEngineCactusMeshId },
                                    { "Sandstone", HE::kEngineSandstoneMeshId } })
        if (id.lo != 1 || id.hi != kMeshBaseHi + static_cast<uint64_t>(indexOf(name)))
        {
            std::fprintf(stderr, "mesh_gen: DefaultAssets.h names %s at the wrong index\n", name);
            return 3;
        }

    int index = 0, ok = 0;
    for (const auto& [name, meshConst] : meshes)
    {
        Mesh mesh = meshConst;
        fixWinding(mesh);

        float bmin[3], bmax[3];
        if (const std::string problem = verify(mesh, bmin, bmax); !problem.empty())
        {
            std::fprintf(stderr, "  %-9s INVALID: %s\n", name, problem.c_str());
            ++index;
            continue;
        }

        StaticMeshAsset a;
        a.type      = HE::AssetType::StaticMesh;
        a.name      = name;
        a.path      = std::string(name) + ".hasset";
        a.id        = HE::UUID{ kMeshBaseHi + static_cast<uint64_t>(index), 0x0000000000000001ULL };
        a.vertices  = std::move(mesh.pos);
        a.normals   = std::move(mesh.nrm);
        a.uvs       = std::move(mesh.uv);
        a.indices   = std::move(mesh.idx);

        if (cm.saveAsset(a))
        {
            std::printf("  %-9s %6zu verts %6zu tris  bounds [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
                        name, a.vertices.size()/3, a.indices.size()/3,
                        bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);
            ++ok;
        }
        else
            std::fprintf(stderr, "  FAILED to write %s.hasset\n", name);
        ++index;
    }

    std::printf("mesh_gen: wrote %d/%zu meshes to %s\n", ok, meshes.size(), outDir.c_str());
    return ok == static_cast<int>(meshes.size()) ? 0 : 1;
}
