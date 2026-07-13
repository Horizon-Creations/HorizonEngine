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
    };

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
