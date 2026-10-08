// landscape_tex_gen — generates the PLACEHOLDER textures of the auto landscape
// material (Thema 158) as loose .hasset files (Texture), written via
// ContentManager::saveAsset so the byte layout is identical to imported textures.
//
// Usage:  landscape_tex_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Textures/Landscape). It is used verbatim as the
//   ContentManager content root, and each texture is saved under "<Name>.hasset".
//
//          landscape_tex_gen <output-dir> --pack <png-dir> [--size N]
//   packs REAL textures: reads <Layer>_<Map>.png (Albedo, Normal, Roughness, AO,
//   Height — the names docs/auto-landscape-material-textures.md §4.3 asks for; the
//   ambientCG / Poly Haven spellings Color, NormalGL, AmbientOcclusion,
//   Displacement are accepted too) from <png-dir>, writes the per-layer
//   T_Landscape_<Layer>_{Albedo,Normal,Mask}.hasset (Mask = R AO, G roughness,
//   B height; a missing AO is white, a missing height is mid-grey) and assembles
//   the three _Array textures. A layer with no files keeps its placeholder look,
//   scaled to the pack size, so the arrays stay one size. Point <output-dir> at
//   <YourProject>/Content/Engine/Textures/Landscape to override the shipped
//   placeholders for that one project, without touching the repo.
//
//          landscape_tex_gen <output-dir> --loose <png-dir> [--size N]
//   writes the maps --pack hides inside the packed _Mask as loose textures of their
//   own, T_Landscape_<Layer>_{Roughness,AO,Height}.hasset (linear, RGBA8 grey, one
//   mip like an imported PNG). _Albedo and _Normal are already loose per layer, so
//   they are not written a second time. Touches nothing --pack or the placeholders
//   wrote, and takes a UUID block of its own (kLooseIndexBase).
//
//          landscape_tex_gen <output-dir> --material
//   writes the auto landscape material M_AutoLandscape.hasset instead (Schritt 5,
//   HE::buildAutoLandscapeGraph) — run it on EditorDeps/EngineContent/Materials.
//
// Five layers (Grass, Dirt, Rock, Snow, WetGround) × three maps, in the packed
// shape docs/auto-landscape-material-textures.md §3 asks the real textures to end
// up in:
//   _Albedo  sRGB    4×4 checker of two tones of the layer colour, with a bright
//                    L marker in the bottom-left cell — shows whether a tile is
//                    repeated, offset or rotated (the texture-bombing witness)
//   _Normal  linear  tangent space, OpenGL convention (green = +Y), derived from
//                    the height below so it matches the mask
//   _Mask    linear  R = AO, G = roughness, B = height, A = 255
//
// Rows are stored BOTTOM-UP, the layout TextureImporter produces (it flips on
// load to the engine's bottom-left UV origin): row 0 is v = 0, and +y is the +V
// direction the Normal Map node's frame takes as "up". A real texture imported
// onto the same path therefore has the same orientation and normal sign.
//
// Output is deterministic: every texture is procedural and gets a stable,
// well-known UUID (see kTexBaseHi below), so re-running produces byte-identical
// files and materials referencing these survive a real texture being imported
// over them (the importer keeps the UUID already on disk).

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/TextureArrayBuild.h>
#include "TextureImporter.h"   // HorizonImporters — decodes the PNGs the way the editor's import does
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Types/UUID.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
constexpr float kPi = 3.14159265358979323846f;

// Well-known UUID base for the landscape placeholders. Same rules as mesh_gen's
// 0x100 / widget_gen's 0x200 / matfn_gen's 0x300: hi far below the version-4
// bit pattern and clear of the DefaultAssets sentinels.
constexpr uint64_t kTexBaseHi = 0x0000000000000400ULL;

// The loose Roughness / AO / Height maps (--loose) count on from kTexBaseHi + 0x20
// = 0x420. 0x400..0x411 are the 15 layer textures and 3 arrays, 0x412 is
// M_AutoLandscape (kAutoLandscapeMaterialId), 0x413..0x41F stay free for more
// landscape materials. Index = kLooseIndexBase + layer * 3 + map, per kLayers order
// (WetGround would get 0x42C..0x42E), so a layer added later never shifts one.
constexpr int kLooseIndexBase = 0x20;

constexpr int kSize  = 128;          // texels per side
constexpr int kCells = 4;            // checker cells per side
constexpr int kCell  = kSize / kCells;

struct Layer
{
    const char* name;
    uint8_t     albedo[3];   // sRGB base tone; the checker's second tone is 80 % of it
    float       roughness;   // mask G
    float       heightBase;  // mask B = base + amp * bump
    float       heightAmp;   // negative = hollows instead of hills
    float       bumpPower;   // < 1 flattens the bump top into a block
};

// Order = UUID index order; append only, never insert (the index IS the UUID).
const Layer kLayers[] = {
    { "Grass",     {  70, 135,  50 }, 0.85f, 0.20f,  0.60f, 1.0f  },
    { "Dirt",      { 115,  82,  52 }, 0.90f, 0.30f,  0.40f, 1.0f  },
    { "Rock",      { 118, 124, 135 }, 0.70f, 0.10f,  0.85f, 0.35f },
    { "Snow",      { 225, 230, 238 }, 0.60f, 0.40f,  0.15f, 1.0f  },
    { "WetGround", {  48,  54,  64 }, 0.15f, 0.70f, -0.50f, 1.0f  },
};

enum Map { Albedo = 0, Normal = 1, Mask = 2, MapCount = 3 };
const char* const kMapNames[MapCount] = { "Albedo", "Normal", "Mask" };

uint8_t toByte(float v)
{
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// Height in [0,1]: one smooth bump per checker cell, periodic over the texture
// (kSize is a whole number of cells), so the map tiles without a seam.
float heightAt(const Layer& L, int x, int y)
{
    x = (x % kSize + kSize) % kSize;
    y = (y % kSize + kSize) % kSize;
    const float fx = (static_cast<float>(x % kCell) + 0.5f) / kCell;
    const float fy = (static_cast<float>(y % kCell) + 0.5f) / kCell;
    const float b  = std::pow(std::sin(kPi * fx) * std::sin(kPi * fy), L.bumpPower);
    return std::clamp(L.heightBase + L.heightAmp * b, 0.0f, 1.0f);
}

// The L marker in cell (0,0), bottom-left in UV space: a vertical bar up the
// left and a shorter foot along the bottom — asymmetric under every rotation
// and mirror, so the image tells which way a bombed tile was turned.
bool inMarker(int x, int y)
{
    const bool bar  = x >= 4 && x < 9  && y >= 4 && y < 26;
    const bool foot = x >= 4 && x < 19 && y >= 4 && y < 9;
    return bar || foot;
}

std::vector<uint8_t> makeMap(const Layer& L, Map map)
{
    std::vector<uint8_t> px(static_cast<size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            uint8_t* p = &px[(static_cast<size_t>(y) * kSize + x) * 4];
            const float h = heightAt(L, x, y);
            switch (map)
            {
            case Albedo:
            {
                if (inMarker(x, y))
                {
                    p[0] = 255; p[1] = 210; p[2] = 0;
                }
                else
                {
                    const bool  dark = ((x / kCell) + (y / kCell)) % 2 != 0;
                    const float k    = dark ? 0.8f : 1.0f;
                    for (int c = 0; c < 3; ++c)
                        p[c] = static_cast<uint8_t>(std::lround(L.albedo[c] * k));
                }
                p[3] = 255;
                break;
            }
            case Normal:
            {
                // Central differences in texel units, wrapping (seamless). +x = +U,
                // +y = +V (rows are bottom-up), so a slope rising towards +V tilts
                // the normal towards -Y: on the far side of a bump's peak G > 128.
                const float strength = 4.0f;
                const float dx = (heightAt(L, x + 1, y) - heightAt(L, x - 1, y)) * 0.5f * strength;
                const float dy = (heightAt(L, x, y + 1) - heightAt(L, x, y - 1)) * 0.5f * strength;
                float n[3] = { -dx, -dy, 1.0f };
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int c = 0; c < 3; ++c)
                    p[c] = toByte(n[c] / len * 0.5f + 0.5f);
                p[3] = 255;
                break;
            }
            case Mask:
            {
                // AO darkens the low ground between the bumps, never below 0.65.
                p[0] = toByte(0.65f + 0.35f * h);
                p[1] = toByte(L.roughness);
                p[2] = toByte(h);
                p[3] = 255;
                break;
            }
            default: break;
            }
        }
    return px;
}

std::string assetName(const Layer& L, int map)
{
    return std::string("T_Landscape_") + L.name + "_" + kMapNames[map];
}

// The three texture ARRAYS (Thema 158, Schritt 3): one per map kind, slice k =
// kLayers[k], so a Texture Array Sample node reads all five layers through one
// heTexP slot. Assembled from the per-layer .hasset files ON DISK — the
// placeholders this tool just wrote, or real textures imported over the same
// paths later — with a baked mip chain, so every backend samples the same
// levels. UUID index 15 + map (after the 15 per-layer textures).
int writeArrays(const std::string& outDir)
{
    const int layerCount = static_cast<int>(std::size(kLayers));
    int ok = 0;
    for (int map = 0; map < MapCount; ++map)
    {
        const std::string name = std::string("T_Landscape_") + kMapNames[map] + "_Array";
        ContentManager src(outDir);
        // Load every slice first, THEN take pointers: a load may move the ones
        // already handed out.
        std::vector<decltype(src.loadAsset(std::string()))> ids;
        for (const Layer& L : kLayers)
            ids.push_back(src.loadAsset(assetName(L, map) + ".hasset"));
        std::vector<const TextureAsset*> slices;
        for (const auto& id : ids)
            slices.push_back(src.getTexture(id));

        TextureAsset a;
        std::string err;
        if (!HE::buildTextureArray(slices, a, /*bakeMips=*/true, &err))
        {
            std::fprintf(stderr, "  %s: %s\n", name.c_str(), err.c_str());
            continue;
        }
        a.type = HE::AssetType::Texture;
        a.name = name;
        a.path = name + ".hasset";
        a.id   = HE::UUID{ kTexBaseHi + static_cast<uint64_t>(layerCount * MapCount + map),
                           0x0000000000000001ULL };
        ContentManager cm(outDir);
        if (!cm.saveAsset(a))
        {
            std::fprintf(stderr, "  FAILED to write %s.hasset\n", name.c_str());
            continue;
        }
        ContentManager check(outDir);
        const TextureAsset* t = check.getTexture(check.loadAsset(a.path));
        if (!t || t->id != a.id || t->layers != static_cast<uint32_t>(layerCount) ||
            t->mipLevels != a.mipLevels || t->srgb != a.srgb || t->data != a.data ||
            !HE::textureArrayPayloadValid(*t))
        {
            std::fprintf(stderr, "  %s.hasset does NOT read back as written\n", name.c_str());
            continue;
        }
        std::printf("  %-30s %ux%u x%u slices, %u mips %s\n", name.c_str(), a.width, a.height,
                    a.layers, a.mipLevels, a.srgb ? "sRGB" : "linear");
        ++ok;
    }
    return ok;
}

// ─── Packing real textures ───────────────────────────────────────────────────
struct Image
{
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8, rows bottom-up (TextureImporter's layout)
    bool valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h * 4; }
};

std::string lowered(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool loadImage(const std::filesystem::path& file, Image& out)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TextureImporter::ImportSettings settings;
    settings.flipVertically = true;   // the layout the placeholders and every imported texture have
    const auto asset = TextureImporter::decodeFromMemory(bytes.data(), bytes.size(), settings);
    if (!asset) return false;
    out.w = static_cast<int>(asset->width);
    out.h = static_cast<int>(asset->height);
    out.px = asset->data;
    return out.valid();
}

// Bilinear, edge-clamped. Only used when the files of one set differ in size (a
// 1K map among 2K ones) or to bring a placeholder up to the pack size.
Image resized(const Image& src, int w, int h)
{
    if (src.w == w && src.h == h) return src;
    Image dst;
    dst.w = w; dst.h = h;
    dst.px.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
    {
        const float fy = (static_cast<float>(y) + 0.5f) * src.h / h - 0.5f;
        const int   y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src.h - 1);
        const int   y1 = std::min(y0 + 1, src.h - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (int x = 0; x < w; ++x)
        {
            const float fx = (static_cast<float>(x) + 0.5f) * src.w / w - 0.5f;
            const int   x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src.w - 1);
            const int   x1 = std::min(x0 + 1, src.w - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            for (int c = 0; c < 4; ++c)
            {
                const auto at = [&](int xx, int yy) {
                    return static_cast<float>(src.px[(static_cast<size_t>(yy) * src.w + xx) * 4 + c]); };
                const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
                const float bot = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
                dst.px[(static_cast<size_t>(y) * w + x) * 4 + c] = toByte((top * (1.0f - ty) + bot * ty) / 255.0f);
            }
        }
    }
    return dst;
}

// <Layer>_<Alias>.<ext>, case-insensitive; the first alias that exists wins.
std::filesystem::path findMap(const std::map<std::string, std::filesystem::path>& files,
                              const char* layer, std::initializer_list<const char*> aliases)
{
    static const char* const kExt[] = { "png", "jpg", "jpeg", "tga", "bmp" };
    for (const char* alias : aliases)
        for (const char* ext : kExt)
        {
            const auto it = files.find(lowered(std::string(layer) + "_" + alias + "." + ext));
            if (it != files.end()) return it->second;
        }
    return {};
}

bool saveLayerTexture(const std::string& outDir, const std::string& name, int index,
                      int w, int h, bool srgb, std::vector<uint8_t> data)
{
    ContentManager cm(outDir);
    TextureAsset a;
    a.type      = HE::AssetType::Texture;
    a.name      = name;
    a.path      = name + ".hasset";
    a.id        = HE::UUID{ kTexBaseHi + static_cast<uint64_t>(index), 0x0000000000000001ULL };
    a.width     = static_cast<size_t>(w);
    a.height    = static_cast<size_t>(h);
    a.channels  = 4;
    a.mipLevels = 1;   // level 0 only, like TextureImporter; the arrays bake their own chain
    a.format    = TextureFormat::RGBA8;
    a.srgb      = srgb;
    a.data      = std::move(data);
    if (!cm.saveAsset(a))
    {
        std::fprintf(stderr, "  FAILED to write %s.hasset\n", name.c_str());
        return false;
    }
    ContentManager check(outDir);
    const TextureAsset* t = check.getTexture(check.loadAsset(a.path));
    if (!t || t->id != a.id || t->srgb != a.srgb || t->data != a.data)
    {
        std::fprintf(stderr, "  %s.hasset does NOT read back as written\n", name.c_str());
        return false;
    }
    return true;
}

int packReal(const std::string& outDir, const std::string& pngDir, int requestedSize)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(pngDir, ec))
    {
        std::fprintf(stderr, "landscape_tex_gen: %s is not a folder\n", pngDir.c_str());
        return 1;
    }
    std::map<std::string, fs::path> files;
    for (const auto& e : fs::directory_iterator(pngDir, ec))
        if (e.is_regular_file()) files[lowered(e.path().filename().string())] = e.path();

    struct Found { Image albedo, normal, rough, ao, height; bool any = false; };
    std::vector<Found> found(std::size(kLayers));

    // Pack size: --size, else the first colour map found.
    int size = requestedSize;
    for (size_t li = 0; li < std::size(kLayers); ++li)
    {
        const char* name = kLayers[li].name;
        Found& f = found[li];
        const auto load = [&](Image& img, std::initializer_list<const char*> aliases, const char* what) {
            const fs::path p = findMap(files, name, aliases);
            if (p.empty()) return;
            if (!loadImage(p, img))
            {
                std::fprintf(stderr, "  %s: could not read %s\n", what, p.string().c_str());
                return;
            }
            f.any = true;
            std::printf("  %-10s %-9s %s (%dx%d)\n", name, what, p.filename().string().c_str(), img.w, img.h);
            if (size <= 0) size = img.w;
        };
        load(f.albedo, { "Albedo", "Color", "BaseColor", "Diffuse" }, "Albedo");
        load(f.normal, { "Normal", "NormalGL" },                      "Normal");
        load(f.rough,  { "Roughness" },                               "Roughness");
        load(f.ao,     { "AO", "AmbientOcclusion" },                  "AO");
        load(f.height, { "Height", "Displacement" },                  "Height");
    }
    if (size <= 0)
    {
        std::fprintf(stderr, "landscape_tex_gen: no <Layer>_<Map>.png found in %s\n"
                             "  expected e.g. Grass_Albedo.png, Grass_Normal.png, Grass_Roughness.png,\n"
                             "  Grass_AO.png, Grass_Height.png (layers: Grass Dirt Rock Snow WetGround)\n",
                     pngDir.c_str());
        return 1;
    }
    std::printf("landscape_tex_gen: packing at %dx%d\n", size, size);

    int ok = 0, total = 0;
    int index = 0;
    for (size_t li = 0; li < std::size(kLayers); ++li)
    {
        const Layer& L = kLayers[li];
        const Found& f = found[li];
        std::vector<uint8_t> albedo, normal, mask;
        const size_t n = static_cast<size_t>(size) * size * 4;
        if (!f.any)
        {
            // No files for this layer: its placeholder look at the pack size, so the
            // arrays stay one size and the layer is still recognisable.
            std::printf("  %-10s (no files — placeholder)\n", L.name);
            const auto up = [&](Map m) {
                Image p; p.w = kSize; p.h = kSize; p.px = makeMap(L, m);
                return resized(p, size, size).px; };
            albedo = up(Albedo); normal = up(Normal); mask = up(Mask);
        }
        else
        {
            const auto fit = [&](const Image& img, const char* what) {
                if (img.valid() && (img.w != size || img.h != size))
                    std::printf("  %-10s %-9s resized %dx%d -> %dx%d\n", L.name, what, img.w, img.h, size, size);
                return img.valid() ? resized(img, size, size) : Image{}; };
            const Image a = fit(f.albedo, "Albedo"), nm = fit(f.normal, "Normal"),
                        r = fit(f.rough, "Roughness"), ao = fit(f.ao, "AO"), hg = fit(f.height, "Height");
            albedo.assign(n, 255);
            normal.resize(n);
            mask.resize(n);
            for (size_t i = 0; i < n; i += 4)
            {
                if (a.valid())  { albedo[i] = a.px[i]; albedo[i + 1] = a.px[i + 1]; albedo[i + 2] = a.px[i + 2]; }
                else            { albedo[i] = L.albedo[0]; albedo[i + 1] = L.albedo[1]; albedo[i + 2] = L.albedo[2]; }
                if (nm.valid()) { normal[i] = nm.px[i]; normal[i + 1] = nm.px[i + 1]; normal[i + 2] = nm.px[i + 2]; }
                else            { normal[i] = 128; normal[i + 1] = 128; normal[i + 2] = 255; }   // flat
                normal[i + 3] = 255;
                mask[i]     = ao.valid() ? ao.px[i] : 255;                      // R = AO (white = unoccluded)
                mask[i + 1] = r.valid()  ? r.px[i]  : toByte(L.roughness);      // G = roughness
                mask[i + 2] = hg.valid() ? hg.px[i] : 128;                      // B = height
                mask[i + 3] = 255;
            }
            if (!f.albedo.valid()) std::printf("  %-10s Albedo    missing — flat layer colour\n", L.name);
            if (!f.normal.valid()) std::printf("  %-10s Normal    missing — flat normal\n", L.name);
            if (!f.rough.valid())  std::printf("  %-10s Roughness missing — placeholder roughness\n", L.name);
            if (!f.ao.valid())     std::printf("  %-10s AO        missing — white\n", L.name);
            if (!f.height.valid()) std::printf("  %-10s Height    missing — mid-grey\n", L.name);
        }
        total += 3;
        ok += saveLayerTexture(outDir, assetName(L, Albedo), index++, size, size, true,  std::move(albedo)) ? 1 : 0;
        ok += saveLayerTexture(outDir, assetName(L, Normal), index++, size, size, false, std::move(normal)) ? 1 : 0;
        ok += saveLayerTexture(outDir, assetName(L, Mask),   index++, size, size, false, std::move(mask))   ? 1 : 0;
    }
    std::printf("landscape_tex_gen: wrote %d/%d layer textures to %s\n", ok, total, outDir.c_str());
    const int arrays = writeArrays(outDir);
    std::printf("landscape_tex_gen: wrote %d/%d texture arrays to %s\n", arrays, MapCount, outDir.c_str());
    return ok == total && arrays == MapCount ? 0 : 1;
}

// The maps the packed _Mask hides (Thema 177, Schritt 6): Roughness, AO and Height of
// every layer that has a PNG for them, as loose linear RGBA8 textures. Read the way
// --pack reads them (loadImage: same decoder, same flip), so a loose map holds
// exactly the bytes the _Mask carries in its G, R and B channel.
int writeLoose(const std::string& outDir, const std::string& pngDir, int requestedSize)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(pngDir, ec))
    {
        std::fprintf(stderr, "landscape_tex_gen: %s is not a folder\n", pngDir.c_str());
        return 1;
    }
    std::map<std::string, fs::path> files;
    for (const auto& e : fs::directory_iterator(pngDir, ec))
        if (e.is_regular_file()) files[lowered(e.path().filename().string())] = e.path();

    // Name = the suffix of the asset; the file spellings are the ones --pack accepts.
    struct LooseMap { const char* name; const char* alias; const char* alias2; };
    const LooseMap kLoose[] = {
        { "Roughness", "Roughness", "Roughness" },
        { "AO",        "AO",        "AmbientOcclusion" },
        { "Height",    "Height",    "Displacement" },
    };

    int ok = 0, total = 0;
    for (size_t li = 0; li < std::size(kLayers); ++li)
        for (size_t k = 0; k < std::size(kLoose); ++k)
        {
            const fs::path p = findMap(files, kLayers[li].name, { kLoose[k].alias, kLoose[k].alias2 });
            if (p.empty()) continue;
            ++total;
            Image img;
            if (!loadImage(p, img))
            {
                std::fprintf(stderr, "  %s %s: could not read %s\n", kLayers[li].name, kLoose[k].name,
                             p.string().c_str());
                continue;
            }
            if (requestedSize > 0 && (img.w != requestedSize || img.h != requestedSize))
            {
                std::printf("  %-10s %-9s resized %dx%d -> %dx%d\n", kLayers[li].name, kLoose[k].name,
                            img.w, img.h, requestedSize, requestedSize);
                img = resized(img, requestedSize, requestedSize);
            }
            const std::string name = std::string("T_Landscape_") + kLayers[li].name + "_" + kLoose[k].name;
            const int index = kLooseIndexBase + static_cast<int>(li) * static_cast<int>(std::size(kLoose)) +
                              static_cast<int>(k);
            if (saveLayerTexture(outDir, name, index, img.w, img.h, /*srgb=*/false, std::move(img.px)))
            {
                std::printf("  %-34s %s (%dx%d)\n", name.c_str(), p.filename().string().c_str(), img.w, img.h);
                ++ok;
            }
        }
    std::printf("landscape_tex_gen: wrote %d/%d loose maps to %s\n", ok, total, outDir.c_str());
    return total > 0 && ok == total ? 0 : 1;
}

// The auto landscape MATERIAL (Thema 158, Schritt 5): HE::buildAutoLandscapeGraph
// saved as M_AutoLandscape.hasset with the fixed kAutoLandscapeMaterialId. Only the
// graph is the source; the baked GLSL and the parameter layout come from the
// ContentManager's own regenerate path, so the file is exactly what the material
// editor would save. Its texture paths point at the engine arrays ("Engine/…"),
// which do not have to exist in <output-dir>.
bool writeMaterial(const std::string& outDir)
{
    const char* kFile = "M_AutoLandscape.hasset";
    const HE::AutoLandscapeGraph built = HE::buildAutoLandscapeGraph();
    MaterialAsset am;
    am.type          = HE::AssetType::Material;
    am.name          = "M_AutoLandscape";
    am.path          = kFile;
    am.id            = HE::kAutoLandscapeMaterialId;
    am.nodeGraphJson = HE::materialGraphToJson(built.graph);

    ContentManager cm(outDir);
    const HE::UUID id = cm.registerMaterial(std::move(am));
    cm.regenerateMaterialFromGraph(id);
    MaterialAsset* m = cm.getMaterialMutable(id);
    if (!m || m->customShaderFragGlsl.empty())
    {
        std::fprintf(stderr, "  M_AutoLandscape: the graph did not compile\n");
        return false;
    }
    if (!cm.saveAsset(*m))
    {
        std::fprintf(stderr, "  FAILED to write %s\n", kFile);
        return false;
    }
    ContentManager check(outDir);
    const MaterialAsset* t = check.getMaterial(check.loadAsset(kFile));
    if (!t || t->id != HE::kAutoLandscapeMaterialId || t->nodeGraphJson != m->nodeGraphJson ||
        t->customShaderFragGlsl != m->customShaderFragGlsl || t->graphParamNames != m->graphParamNames)
    {
        std::fprintf(stderr, "  %s does NOT read back as written\n", kFile);
        return false;
    }
    std::printf("  %-30s %zu nodes, %zu params, %zu textures, array mask %u\n", kFile,
                built.graph.nodes.size(), t->graphParamNames.size(), t->graphTexturePaths.size(),
                HE::matGlslTextureArrayMask(t->customShaderFragGlsl));
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: landscape_tex_gen <output-dir> [--arrays-only | --material | --pack <png-dir> [--size N] | --loose <png-dir> [--size N]]\n"
                             "  --pack         pack real <Layer>_<Map>.png files from <png-dir> (see the header)\n"
                             "  --loose        write the Roughness / AO / Height PNGs of <png-dir> as loose\n"
                             "                 textures of their own (see the header)\n"
                             "  --arrays-only  only (re)assemble the three _Array textures from the\n"
                             "                 per-layer files already in <output-dir>\n"
                             "  --material     write the auto landscape material M_AutoLandscape.hasset\n"
                             "                 into <output-dir> (EditorDeps/EngineContent/Materials)\n");
        return 2;
    }
    const std::string outDir = argv[1];
    const bool arraysOnly = argc > 2 && std::string(argv[2]) == "--arrays-only";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    if (argc > 3 && std::string(argv[2]) == "--pack")
    {
        int size = 0;
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--size") size = std::atoi(argv[i + 1]);
        return packReal(outDir, argv[3], size);
    }

    if (argc > 3 && std::string(argv[2]) == "--loose")
    {
        int size = 0;
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--size") size = std::atoi(argv[i + 1]);
        return writeLoose(outDir, argv[3], size);
    }

    if (argc > 2 && std::string(argv[2]) == "--material")
    {
        const bool ok = writeMaterial(outDir);
        std::printf("landscape_tex_gen: %s the auto landscape material to %s\n",
                    ok ? "wrote" : "FAILED to write", outDir.c_str());
        return ok ? 0 : 1;
    }

    if (arraysOnly)
    {
        const int arrays = writeArrays(outDir);
        std::printf("landscape_tex_gen: wrote %d/%d texture arrays to %s\n", arrays, MapCount, outDir.c_str());
        return arrays == MapCount ? 0 : 1;
    }

    ContentManager cm(outDir);

    const int total = static_cast<int>(std::size(kLayers)) * MapCount;
    int index = 0, ok = 0;
    for (const Layer& L : kLayers)
        for (int map = 0; map < MapCount; ++map, ++index)
        {
            const std::string name = assetName(L, map);

            TextureAsset a;
            a.type      = HE::AssetType::Texture;
            a.name      = name;
            a.path      = name + ".hasset";
            a.id        = HE::UUID{ kTexBaseHi + static_cast<uint64_t>(index), 0x0000000000000001ULL };
            a.width     = kSize;
            a.height    = kSize;
            a.channels  = 4;
            a.mipLevels = 1;             // level 0 only, like TextureImporter (GL/Metal mip at upload, D3D/Vulkan do not)
            a.format    = TextureFormat::RGBA8;
            a.srgb      = (map == Albedo);
            a.data      = makeMap(L, static_cast<Map>(map));

            if (!cm.saveAsset(a))
            {
                std::fprintf(stderr, "  FAILED to write %s.hasset\n", name.c_str());
                continue;
            }

            // Read it back through a fresh ContentManager: the file on disk, not
            // the copy in memory, is what the editor and the packer will see.
            ContentManager check(outDir);
            const TextureAsset* t = check.getTexture(check.loadAsset(a.path));
            if (!t || t->id != a.id || t->width != kSize || t->height != kSize ||
                t->srgb != a.srgb || t->data != a.data)
            {
                std::fprintf(stderr, "  %s.hasset does NOT read back as written\n", name.c_str());
                continue;
            }
            std::printf("  %-30s %dx%d %s\n", name.c_str(), kSize, kSize,
                        a.srgb ? "sRGB" : "linear");
            ++ok;
        }

    std::printf("landscape_tex_gen: wrote %d/%d textures to %s\n", ok, total, outDir.c_str());
    const int arrays = writeArrays(outDir);
    std::printf("landscape_tex_gen: wrote %d/%d texture arrays to %s\n", arrays, MapCount, outDir.c_str());
    return ok == total && arrays == MapCount ? 0 : 1;
}
