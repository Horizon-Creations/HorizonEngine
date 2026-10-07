// landscape_tex_gen — generates the PLACEHOLDER textures of the auto landscape
// material (Thema 158) as loose .hasset files (Texture), written via
// ContentManager::saveAsset so the byte layout is identical to imported textures.
//
// Usage:  landscape_tex_gen <output-dir>
//   <output-dir> is the folder the .hasset files are written into (e.g.
//   EditorDeps/EngineContent/Textures/Landscape). It is used verbatim as the
//   ContentManager content root, and each texture is saved under "<Name>.hasset".
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

// Well-known UUID base for the landscape placeholders. Same rules as mesh_gen's
// 0x100 / widget_gen's 0x200 / matfn_gen's 0x300: hi far below the version-4
// bit pattern and clear of the DefaultAssets sentinels.
constexpr uint64_t kTexBaseHi = 0x0000000000000400ULL;

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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: landscape_tex_gen <output-dir> [--arrays-only]\n"
                             "  --arrays-only  only (re)assemble the three _Array textures from the\n"
                             "                 per-layer files already in <output-dir>\n");
        return 2;
    }
    const std::string outDir = argv[1];
    const bool arraysOnly = argc > 2 && std::string(argv[2]) == "--arrays-only";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

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
