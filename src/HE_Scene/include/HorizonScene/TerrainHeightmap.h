#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct TerrainComponent;
struct TextureAsset;

// ─── Greyscale heightmap → TerrainComponent::sculptHeights ───────────────────
// The third sibling of TerrainSculpt and TerrainPaint: pure CPU, no
// ContentManager and no renderer, so "does a black-to-white gradient come out
// as a ramp from base to base+range" is a question a test can put to a
// component on the stack.
//
// Three ways in, one path through. `importPixels` is the whole algorithm:
// grey values (0..1) laid edge-to-edge over the landscape and resampled onto
// the terrain's own vertex grid. `importTexture` feeds it a project texture
// asset (the thing `TerrainComponent::heightmapTexture` points at), and
// `importFile` feeds it a file straight off the disk — which is the only way
// 16-bit precision gets in: project textures are cooked to 8 bits, so a
// mountain range 500 m tall would step in two-metre terraces through the
// asset slot, and in 8 mm through a 16-bit PNG.
//
// Orientation: image row 0 (the top of the picture) lands at the terrain's
// -Z edge, image column 0 at -X, so a heightmap seen in an image viewer is the
// landscape seen from above with +X to the right and +Z DOWN the screen — the
// same way the terrain's own UVs run (TerrainMeshGenerator, v = z). `flipZ`
// mirrors that for tools that export their maps bottom-up.
//
// The result REPLACES the sculpt (sculptHeights is the master field, see
// computeTerrainHeightField); the noise settings and any brushwork before the
// import are gone. Painted layer weights and the foliage mask are left alone —
// they are laid over the same 0..1 footprint and stay put.
namespace TerrainHeightmap
{
    // Raw pixel view. `bytesPerChannel` is 1 (8-bit) or 2 (16-bit, native
    // byte order — what stb_image and a .r16 give). One or two channels are
    // read as grey (+alpha, ignored); three or four are converted to luma, so a
    // colour picture dropped in by mistake still gives a sensible height rather
    // than "the red channel".
    struct Source
    {
        const uint8_t* pixels          = nullptr;
        uint32_t       width           = 0;
        uint32_t       height          = 0;
        uint32_t       channels        = 0;
        uint32_t       bytesPerChannel = 1;
    };

    struct Options
    {
        // World-Y of a black pixel. A white pixel lands at baseHeight + range.
        float baseHeight = 0.0f;
        // Height span between black and white, world units. 0 = use the
        // terrain's own heightScale, which is what the Inspector's "Height
        // Scale" then means for an imported landscape: the same knob for a
        // seeded and an imported one.
        float range = 0.0f;
        // Mirror the image top-to-bottom before laying it over the terrain.
        bool  flipZ = false;
        // Set the terrain's vertex resolution to the image's own (the larger
        // side, snapped to the 2ⁿ+1 the chunk builder uses, capped at 513 like
        // the Inspector's slider) instead of resampling onto the current grid.
        // The natural choice for a map made for THIS landscape; off by default
        // so an import never silently multiplies the vertex count.
        bool  adoptResolution = false;
    };

    struct Result
    {
        bool        ok = false;
        std::string error;                 // set when ok == false, in user words
        uint32_t    sourceWidth  = 0;      // what was read, for the status line
        uint32_t    sourceHeight = 0;
        uint32_t    sourceBits   = 0;      // 8 or 16
        float       minHeight = 0.0f;      // over the written field
        float       maxHeight = 0.0f;
    };

    // Resample `src` onto the terrain's vertex grid and write sculptHeights.
    // Snaps the resolution the way TerrainSculpt::ensureHeights does, so the
    // heights land on the grid the chunks will actually sample. Going down
    // (a 4k map onto 129 vertices) box-averages the image under each vertex
    // rather than point-sampling it, so a fine ridge line thins out instead of
    // turning into aliasing noise; going up is bilinear. Sets tc.dirty.
    //
    // Fails (ok = false, nothing written) for an empty or degenerate source,
    // an unsupported channel/byte layout, or a terrain with no area.
    Result importPixels(TerrainComponent& tc, const Source& src, const Options& opts);

    // A project texture asset: level 0 of `tex.data`, 8 bits per channel. A
    // block-compressed texture (BC7/BC3/ASTC) is refused — it would have to be
    // decoded first, and a heightmap cooked lossy is the wrong source anyway.
    Result importTexture(TerrainComponent& tc, const TextureAsset& tex, const Options& opts);

    // Level 0 of a texture asset as grey values 0..1, row-major width×height,
    // read the way importTexture reads a heightmap (luma for colour). Used for
    // the tessellation's displacement map. Returns false and leaves `out`
    // empty for a block-compressed texture or one without pixel data.
    bool greyFromTexture(const TextureAsset& tex, std::vector<float>& out);

    // An image file. PNG (8 and 16-bit), PGM (8 and 16-bit), plus whatever else
    // stb_image reads at 8 bits (JPEG, BMP, TGA, PSD…). `.r16` / `.raw` are
    // taken as headerless 16-bit little-endian, square, with the side length
    // derived from the file size — the World Machine / Unreal interchange
    // shape — and refused when the size is not a perfect square of shorts.
    Result importFile(TerrainComponent& tc, const std::string& path, const Options& opts);
}
