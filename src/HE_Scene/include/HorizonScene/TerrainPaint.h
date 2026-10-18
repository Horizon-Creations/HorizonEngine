#pragma once
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── Landscape layer painting ────────────────────────────────────────────────
// Brush operations on TerrainComponent::layerWeights (layers 0..3) and
// layerWeights2 (layers 4..7), RGBA8, channel = layer.
// Pure CPU, no ContentManager / renderer — freely testable, mirroring how
// TerrainMeshGenerator keeps the heightfield maths out of the editor.
namespace TerrainPaint
{
    // Allocate the weightmap if it doesn't exist yet: every texel fully on layer
    // 0, which is what an unpainted landscape already renders as (the default
    // 1x1 weightmap is (1,0,0,0)), so turning painting on never changes the look.
    // No-op when a correctly sized map is already present. A second page that
    // does not match the (re)allocated first one is dropped — its weights
    // belong to texels that no longer exist.
    void ensureWeightmap(TerrainComponent& tc);

    // Paint `layer` (0 .. kTerrainMaxLayers-1) at terrain-LOCAL (x, z) — the
    // same space the sculpt brushes use, i.e. [-sizeX/2, sizeX/2] × [-sizeZ/2, sizeZ/2].
    //
    // radius      full-strength inner radius, world units
    // falloff     transition width outside it; strength falls linearly to 0
    // strength    0..1 per call — how far each texel moves toward "all `layer`"
    //
    // The touched texels stay normalised (weights sum to 255 across both pages):
    // the painted layer gains, the others give up the same amount proportionally.
    // That is what makes painting reversible by simply painting a different
    // layer over it. The second page is allocated (all zero) by the first stroke
    // on layer 4+; until then strokes on 0..3 behave exactly as with four layers.
    //
    // Sets tc.weightsDirty so TerrainSystem re-uploads the texture, and widens
    // the sculpt region-dirty rect so callers can reuse it. Returns false when
    // the layer index is out of range or the terrain has no weightmap.
    bool paint(TerrainComponent& tc, float localX, float localZ,
               int layer, float radius, float falloff, float strength);

    // Every texel fully on `layer` - "start the landscape from this layer". Allocates the
    // weightmap if there is none (and the second page for layers 4..7); a first-page
    // layer clears the second page, whose weights would otherwise outvote it. Sets
    // tc.weightsDirty. False for a layer outside 0 .. kTerrainMaxLayers-1.
    bool fillLayer(TerrainComponent& tc, int layer);

    // The kTerrainMaxLayers weights of texel (tx, tz), layers 4..7 zero when
    // there is no second page. False (out untouched) when the terrain has no
    // weightmap or the texel is outside it.
    bool texelWeights(const TerrainComponent& tc, uint32_t tx, uint32_t tz, uint8_t* out);

    // True when the second page exists, matches the first and carries any
    // weight at all — i.e. when the landscape actually uses layers 4..7.
    bool usesSecondPage(const TerrainComponent& tc);

    // The RGBA8 texture the shader samples as heLandscapeWeights. Without a used
    // second page it is layerWeights itself, weightRes × weightRes — byte for
    // byte what every landscape uploaded before there were eight layers. With
    // one it is 2·weightRes × weightRes: each row is the first page's row
    // followed by the second page's, so layers 0..3 sit in the left half and
    // 4..7 in the right half of ONE texture. The material shader tells the two
    // shapes apart by the texture's aspect ratio (MaterialGraph codegen).
    // Empty when the terrain has no valid weightmap.
    std::vector<uint8_t> buildWeightTexture(const TerrainComponent& tc,
                                            uint32_t& width, uint32_t& height);
}
