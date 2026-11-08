#include "Impostor.h"
#include "ImporterCommon.h"
#include "PbrMaterialImport.h"
#include "Diagnostics/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <glm/glm.hpp>

namespace Impostor
{
namespace
{

constexpr const char* kExt = ".hasset";

// The cut-out threshold of the impostor material. Low on purpose: a thin blade or a stem
// covers a third of a texel at the card's resolution, so its alpha is 0.3, and at 0.5
// the whole grass layer would vanish from the card (and further so in the mips the GPU
// builds). Coverage kept, edges a little fat, which a plant seen from far away wants.
constexpr float kAlphaCutoff = 0.2f;

// ── sRGB, only for tinting: a tint is a LINEAR multiplier and the texture holds
// encoded bytes, so a tint other than white goes through linear and back.
float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct Sample
{
    uint8_t r = 0, g = 0, b = 0;
    bool    covered = false;
};

// One view of the plant: an orthographic projection along a horizontal axis, a z-buffer
// at `grid` x `grid` samples. `axis` 0: looking along -Z (right = +X, nearest = largest
// z); 1: looking along +X (right = +Z, nearest = smallest x) — the two cards' faces
// whose normals are +Z and -X in makeCard, so what the card shows is what stood there.
struct View
{
    int                grid = 0;
    std::vector<float> depth;     // smaller = nearer
    std::vector<Sample> px;
    void init(int g)
    {
        grid = g;
        depth.assign(static_cast<size_t>(g) * g, std::numeric_limits<float>::max());
        px.assign(static_cast<size_t>(g) * g, Sample{});
    }
};

uint8_t tinted(uint8_t c, float tint, bool srgb)
{
    if (tint == 1.0f) return c;
    const float v = srgb ? linearToSrgb(srgbToLinear(static_cast<float>(c) / 255.0f) * tint)
                         : static_cast<float>(c) / 255.0f * tint;
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

void shade(const SectionSource& sec, float u, float v, Sample& out, bool& visible)
{
    visible = true;
    if (!sec.rgba || sec.width <= 0 || sec.height <= 0)
    {
        // No texture: the tint IS the colour (linear in, encoded out).
        out.r = tinted(255, sec.tint[0], true);
        out.g = tinted(255, sec.tint[1], true);
        out.b = tinted(255, sec.tint[2], true);
        return;
    }
    // Repeat addressing, nearest texel: the samples are denser than the texels for
    // any plant texture a card would be baked from, and the supersample averages.
    u -= std::floor(u);
    v -= std::floor(v);
    const int tx = std::min(static_cast<int>(u * static_cast<float>(sec.width)),  sec.width  - 1);
    const int ty = std::min(static_cast<int>(v * static_cast<float>(sec.height)), sec.height - 1);
    const uint8_t* t = sec.rgba + (static_cast<size_t>(ty) * sec.width + tx) * 4;
    if (sec.alphaTest && static_cast<float>(t[3]) < sec.cutoff * 255.0f)
    {
        visible = false;
        return;
    }
    out.r = tinted(t[0], sec.tint[0], sec.srgb);
    out.g = tinted(t[1], sec.tint[1], sec.srgb);
    out.b = tinted(t[2], sec.tint[2], sec.srgb);
}

struct Vtx
{
    float sx, sy, key;   // screen position in samples, depth key (smaller = nearer)
    float u, v;
};

void rasterise(View& view, const Vtx& a, const Vtx& b, const Vtx& c, const SectionSource& sec)
{
    const float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
    if (std::fabs(area) < 1e-9f) return;   // edge-on to this view: it covers nothing
    const float inv = 1.0f / area;
    const float minX = std::min({ a.sx, b.sx, c.sx }), maxX = std::max({ a.sx, b.sx, c.sx });
    const float minY = std::min({ a.sy, b.sy, c.sy }), maxY = std::max({ a.sy, b.sy, c.sy });
    const int x0 = std::max(0, static_cast<int>(std::floor(minX - 0.5f)));
    const int x1 = std::min(view.grid - 1, static_cast<int>(std::ceil(maxX - 0.5f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(minY - 0.5f)));
    const int y1 = std::min(view.grid - 1, static_cast<int>(std::ceil(maxY - 0.5f)));
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) * inv;
            const float w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) * inv;
            const float w2 = 1.0f - w0 - w1;
            constexpr float kEps = -1e-5f;
            if (w0 < kEps || w1 < kEps || w2 < kEps) continue;
            const float key = w0 * a.key + w1 * b.key + w2 * c.key;
            const size_t i = static_cast<size_t>(y) * view.grid + x;
            if (key >= view.depth[i]) continue;
            Sample s;
            bool visible = true;
            shade(sec, w0 * a.u + w1 * b.u + w2 * c.u, w0 * a.v + w1 * b.v + w2 * c.v, s, visible);
            if (!visible) continue;   // a cut-out texel writes neither colour nor depth
            s.covered = true;
            view.depth[i] = key;
            view.px[i]    = s;
        }
    }
}

// Averages supersample^2 samples into one texel. Colour is the mean over the covered
// samples only (so a half-covered edge texel keeps the leaf's colour, not leaf + black),
// alpha the covered share.
void resolve(const View& view, int size, int ss, std::vector<uint8_t>& out, float& coverage)
{
    out.assign(static_cast<size_t>(size) * size * 4, 0);
    size_t opaque = 0;
    const int total = ss * ss;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            int n = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx)
                {
                    const Sample& s = view.px[static_cast<size_t>(y * ss + sy) * view.grid + (x * ss + sx)];
                    if (!s.covered) continue;
                    ++n; r += s.r; g += s.g; b += s.b;
                }
            if (n == 0) continue;
            uint8_t* o = &out[(static_cast<size_t>(y) * size + x) * 4];
            o[0] = static_cast<uint8_t>((r + n / 2) / n);
            o[1] = static_cast<uint8_t>((g + n / 2) / n);
            o[2] = static_cast<uint8_t>((b + n / 2) / n);
            o[3] = static_cast<uint8_t>((n * 255 + total / 2) / total);
            ++opaque;
        }
    coverage = static_cast<float>(opaque) / static_cast<float>(size * size);
}

// Bleeds the colour of covered texels into the transparent ones around them, `passes`
// texels deep, and gives the rest the plant's mean colour. The alpha stays 0 there; it
// is the RGB a bilinear fetch across the silhouette mixes in, which would otherwise be
// black (a dark outline) at every mip.
void dilate(std::vector<uint8_t>& px, int size, int passes)
{
    std::vector<uint8_t> filled(static_cast<size_t>(size) * size, 0);
    uint64_t sr = 0, sg = 0, sb = 0, count = 0;
    for (size_t i = 0; i < filled.size(); ++i)
        if (px[i * 4 + 3] > 0)
        {
            filled[i] = 1; sr += px[i * 4]; sg += px[i * 4 + 1]; sb += px[i * 4 + 2]; ++count;
        }
    if (count == 0) return;
    for (int pass = 0; pass < passes; ++pass)
    {
        std::vector<uint8_t> nextFilled = filled;
        std::vector<uint8_t> nextPx     = px;
        bool any = false;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const size_t i = static_cast<size_t>(y) * size + x;
                if (filled[i]) continue;
                int n = 0, r = 0, g = 0, b = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const size_t j = static_cast<size_t>(ny) * size + nx;
                        if (!filled[j]) continue;
                        ++n; r += px[j * 4]; g += px[j * 4 + 1]; b += px[j * 4 + 2];
                    }
                if (n == 0) continue;
                nextPx[i * 4]     = static_cast<uint8_t>(r / n);
                nextPx[i * 4 + 1] = static_cast<uint8_t>(g / n);
                nextPx[i * 4 + 2] = static_cast<uint8_t>(b / n);
                nextFilled[i] = 1;
                any = true;
            }
        px.swap(nextPx);
        filled.swap(nextFilled);
        if (!any) break;
    }
    const uint8_t mr = static_cast<uint8_t>(sr / count), mg = static_cast<uint8_t>(sg / count),
                  mb = static_cast<uint8_t>(sb / count);
    for (size_t i = 0; i < filled.size(); ++i)
        if (!filled[i]) { px[i * 4] = mr; px[i * 4 + 1] = mg; px[i * 4 + 2] = mb; }
}

std::string stemOf(const std::string& path)
{
    std::string stem = path;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const size_t n = std::char_traits<char>::length(kExt);
    if (stem.size() > n && stem.compare(stem.size() - n, n, kExt) == 0) stem.resize(stem.size() - n);
    return stem;
}

std::string folderOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

} // namespace

std::string pathFor(const std::string& plantPath, const char* suffix)
{
    return folderOf(plantPath) + stemOf(plantPath) + "_Impostor" + suffix + kExt;
}

Baked bake(const StaticMeshAsset& mesh, const std::vector<SectionSource>& sources, const Settings& settings)
{
    Baked out;
    if (mesh.cooked || mesh.vertices.size() < 9 || mesh.indices.size() < 3)
    {
        out.error = "The mesh has no triangles the bake can read (a packed mesh cannot be baked).";
        return out;
    }
    const size_t vertexCount = mesh.vertices.size() / 3;
    const bool   hasUvs      = mesh.uvs.size() == vertexCount * 2;

    std::vector<MeshSection> sections = mesh.sections;
    if (sections.empty())
    {
        MeshSection all;
        all.indexCount = static_cast<uint32_t>(mesh.indices.size());
        sections.push_back(all);
    }
    if (sources.empty())
    {
        out.error = "No material to take the plant's colour from.";
        return out;
    }

    const int size = std::clamp(settings.size, 16, 2048);
    const int ss   = std::clamp(settings.supersample, 1, 6);

    // ── The plant's box: the card is as wide as the plant is far from its pivot ──
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const MeshSection& sec : sections)
    {
        const size_t begin = std::min<size_t>(sec.indexOffset, mesh.indices.size());
        const size_t end   = std::min<size_t>(begin + sec.indexCount, mesh.indices.size());
        for (size_t i = begin; i < end; ++i)
        {
            const uint32_t g = mesh.indices[i];
            if (g >= vertexCount) continue;
            const glm::vec3 p(mesh.vertices[g * 3], mesh.vertices[g * 3 + 1], mesh.vertices[g * 3 + 2]);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    if (!(hi.y > lo.y))
    {
        out.error = "The mesh has no height to bake.";
        return out;
    }
    float half = std::max({ std::fabs(lo.x), std::fabs(hi.x), std::fabs(lo.z), std::fabs(hi.z) });
    half *= 1.01f;   // a texel of margin: the border of the atlas is not the plant's edge
    if (!(half > 1e-4f))
    {
        out.error = "The mesh has no width to bake.";
        return out;
    }
    const float y0 = lo.y, y1 = lo.y + (hi.y - lo.y) * 1.01f;
    const float spanY = y1 - y0;

    // ── Rasterise both views ─────────────────────────────────────────────────
    const int grid = size * ss;
    View views[2];
    views[0].init(grid);
    views[1].init(grid);
    const float gx = static_cast<float>(grid) / (2.0f * half);
    const float gy = static_cast<float>(grid) / spanY;
    for (size_t si = 0; si < sections.size(); ++si)
    {
        const MeshSection& sec = sections[si];
        const SectionSource& src = sources[std::min(si, sources.size() - 1)];
        const size_t begin = std::min<size_t>(sec.indexOffset, mesh.indices.size());
        const size_t end   = std::min<size_t>(begin + sec.indexCount, mesh.indices.size());
        for (size_t i = begin; i + 2 < end; i += 3)
        {
            const uint32_t id[3] = { mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2] };
            if (id[0] >= vertexCount || id[1] >= vertexCount || id[2] >= vertexCount) continue;
            Vtx a[3], b[3];
            for (int k = 0; k < 3; ++k)
            {
                const float x = mesh.vertices[id[k] * 3], y = mesh.vertices[id[k] * 3 + 1], z = mesh.vertices[id[k] * 3 + 2];
                const float u = hasUvs ? mesh.uvs[id[k] * 2]     : 0.0f;
                const float v = hasUvs ? mesh.uvs[id[k] * 2 + 1] : 0.0f;
                a[k] = { (x + half) * gx, (y - y0) * gy, -z, u, v };   // along -Z: nearest = largest z
                b[k] = { (z + half) * gx, (y - y0) * gy,  x, u, v };   // along +X: nearest = smallest x
            }
            rasterise(views[0], a[0], a[1], a[2], src);
            rasterise(views[1], b[0], b[1], b[2], src);
        }
    }

    // ── Resolve, bleed, join into the atlas ──────────────────────────────────
    out.width  = size * 2;
    out.height = size;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (int v = 0; v < 2; ++v)
    {
        std::vector<uint8_t> tex;
        resolve(views[v], size, ss, tex, out.coverage[v]);
        dilate(tex, size, std::max(0, settings.dilate));
        for (int y = 0; y < size; ++y)
            std::copy_n(&tex[static_cast<size_t>(y) * size * 4], static_cast<size_t>(size) * 4,
                        &out.rgba[(static_cast<size_t>(y) * out.width + static_cast<size_t>(v) * size) * 4]);
    }
    if (out.coverage[0] <= 0.0f && out.coverage[1] <= 0.0f)
    {
        out.error = "Nothing of the plant is visible (every texel is cut out?).";
        out.rgba.clear();
        return out;
    }
    out.halfWidth = half;
    out.y0 = y0;
    out.y1 = y1;
    return out;
}

StaticMeshAsset makeCard(float halfWidth, float y0, float y1, bool doubleFaced)
{
    StaticMeshAsset m;
    m.type = HE::AssetType::StaticMesh;
    constexpr float kSide = 0.45f, kUp = 0.893f;   // unit length: 0.45^2 + 0.893^2 = 1.0
    struct Plane { float ax, az, u0, u1; };               // the quad's horizontal axis, its atlas half
    const Plane planes[2] = { { 1.0f, 0.0f, 0.0f, 0.5f }, { 0.0f, 1.0f, 0.5f, 1.0f } };
    for (const Plane& p : planes)
        for (int side = doubleFaced ? 0 : 1; side < 2; ++side)
        {
            const float s  = side == 0 ? 1.0f : -1.0f;
            // The face's normal tilted towards the sky: a card is lit as a bush canopy, not as a
            // wall, so its two sides do not go dark or glare with the sun's side (what a
            // foliage impostor does everywhere). The winding below still follows the face.
            const float nx = -p.az * s * kSide, nz = p.ax * s * kSide;
            const uint32_t base = static_cast<uint32_t>(m.vertices.size() / 3);
            const float px[4] = { -halfWidth * p.ax,  halfWidth * p.ax,  halfWidth * p.ax, -halfWidth * p.ax };
            const float py[4] = { y0, y0, y1, y1 };
            const float pz[4] = { -halfWidth * p.az,  halfWidth * p.az,  halfWidth * p.az, -halfWidth * p.az };
            const float pu[4] = { p.u0, p.u1, p.u1, p.u0 };
            const float pv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int k = 0; k < 4; ++k)
            {
                m.vertices.insert(m.vertices.end(), { px[k], py[k], pz[k] });
                m.normals.insert(m.normals.end(), { nx, kUp, nz });
                m.uvs.insert(m.uvs.end(), { pu[k], pv[k] });
            }
            // a, b, c, d counter-clockwise seen along the normal (side 0), reversed for side 1.
            if (side == 0) m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            else           m.indices.insert(m.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });
        }
    MeshSection all;
    all.indexOffset = 0;
    all.indexCount  = static_cast<uint32_t>(m.indices.size());
    m.sections.push_back(all);
    return m;
}

Result write(const Baked& baked, const std::string& plantPath, const std::filesystem::path& contentRoot)
{
    Result r;
    if (!baked.ok()) { r.error = baked.error.empty() ? "Nothing was baked." : baked.error; return r; }
    const std::string stem = stemOf(plantPath);

    TextureAsset tex;
    tex.type     = HE::AssetType::Texture;
    tex.name     = stem + "_Impostor";
    tex.path     = pathFor(plantPath, "");
    tex.width    = static_cast<uint32_t>(baked.width);
    tex.height   = static_cast<uint32_t>(baked.height);
    tex.channels = 4;
    tex.srgb     = true;
    tex.data     = baked.rgba;
    if (!Importer::writeAsset(tex, contentRoot)) { r.error = "Failed to write " + tex.path; return r; }
    r.texturePath = tex.path;
    r.texture     = tex.id;

    const std::string folder = folderOf(plantPath);
    r.materialPath = Importer::writeCutoutMaterial(stem + "_ImpostorMat", r.texturePath, 0.9f, kAlphaCutoff, contentRoot,
                                                   std::filesystem::path(folder), &r.material);
    if (r.materialPath.empty()) { r.error = "Failed to write the impostor material."; return r; }

    StaticMeshAsset card = makeCard(baked.halfWidth, baked.y0, baked.y1, std::getenv("HE_IMPOSTOR_SINGLE") == nullptr);
    card.name = stem + "_ImpostorCard";
    card.path = pathFor(plantPath, "Card");
    if (!Importer::writeAsset(card, contentRoot)) { r.error = "Failed to write " + card.path; return r; }
    r.meshPath = card.path;
    r.mesh     = card.id;
    return r;
}

} // namespace Impostor
