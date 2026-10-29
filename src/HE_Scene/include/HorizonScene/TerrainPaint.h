#pragma once
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── Landscape layer painting ────────────────────────────────────────────────
// Brush operations on TerrainComponent::layerWeights (layers 0..3),
// layerWeights2 (layers 4..7) and layerWeights3 (layers 8..11), RGBA8, channel = layer.
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
    // The touched texels stay normalised (weights sum to 255 across all pages):
    // the painted layer gains, the others give up the same amount proportionally.
    // That is what makes painting reversible by simply painting a different
    // layer over it. The second page is allocated (all zero) by the first stroke
    // on layer 4+, the third by the first on layer 8+; until then strokes on 0..3
    // (or 0..7) behave exactly as with four (or eight) layers.
    //
    // Sets tc.weightsDirty so TerrainSystem re-uploads the texture, and widens
    // the sculpt region-dirty rect so callers can reuse it. Returns false when
    // the layer index is out of range or the terrain has no weightmap.
    bool paint(TerrainComponent& tc, float localX, float localZ,
               int layer, float radius, float falloff, float strength);

    // Hand `layer`'s weight back to layer 0 ("Auto") under the brush, and leave every other
    // layer as it is - the eraser of ONE layer, where painting layer 0 would also thin out
    // whatever else was painted there. Same radius / falloff / strength as paint(); never
    // allocates a page (a layer whose page does not exist has nothing to give back). False
    // for layer 0, a group share, or when nothing under the brush held any of `layer`.
    bool eraseLayer(TerrainComponent& tc, float localX, float localZ,
                    int layer, float radius, float falloff, float strength);

    // Every texel fully on `layer` - "start the landscape from this layer". Allocates the
    // weightmap if there is none (and the pages below `layer` for layers 4..11); a
    // layer on a lower page clears the higher ones, whose weights would otherwise
    // outvote it. Sets tc.weightsDirty. False for a layer outside 0 .. kTerrainMaxLayers-1.
    bool fillLayer(TerrainComponent& tc, int layer);

    // The kTerrainMaxLayers weights of texel (tx, tz), the layers of a missing page
    // zero. False (out untouched) when the terrain has no weightmap or the texel is
    // outside it.
    bool texelWeights(const TerrainComponent& tc, uint32_t tx, uint32_t tz, uint8_t* out);

    // ── Mixed texture groups (TerrainComponent::mixGroup) ─────────────────────
    // Weight layer 6 + k is group k's SHARE when tc.mixGroup[k] >= 0. A share is not a
    // paint layer: layers 0..5 keep their own 255-sum without it, paint()/fillLayer()
    // leave the share bytes alone and refuse to paint a reserved layer, and the group
    // brush below is the only thing that writes one. The terrain's own group is the
    // remainder, so a terrain with no share painted is one group exactly as before.
    // Bit k of the result = layer 6 + k is a share.
    int  reservedLayers(const TerrainComponent& tc);
    bool isReservedLayer(const TerrainComponent& tc, int layer);

    // Slot 0 = the terrain's own group, 1 / 2 = mixGroup[0] / [1]. The three shares of
    // texel (tx, tz), summing to 1; slots without a mix group are 0. The own group
    // takes 1 - s1 - s2, and two shares that add past 1 are scaled to fit. False (out
    // untouched) when there is no weightmap or the texel is outside it; an unpainted
    // terrain is {1, 0, 0}.
    bool groupShares(const TerrainComponent& tc, uint32_t tx, uint32_t tz, float* out3);

    // Set mixGroup[k] (group >= 0) or clear it (-1). Taking the layer over folds what
    // it held (paint a custom material put there) into layer 0 so the texel still sums
    // to 255; clearing drops the share, the texels fall back to the own group. A group
    // already used by the other slot is refused, as is a slot outside 0..1. True when
    // anything changed.
    bool setMixGroup(TerrainComponent& tc, int k, int group);

    // The group brush: move each touched texel's three shares toward "all slot", with
    // the same radius / falloff / strength as paint(). Allocates the weightmap and the
    // second page. False for a slot that is no group of this terrain (0 is always one).
    bool paintGroup(TerrainComponent& tc, float localX, float localZ,
                    int slot, float radius, float falloff, float strength);

    // Every texel fully in group `slot`; layers 0..5 stay as they are.
    bool fillGroup(TerrainComponent& tc, int slot);

    // The manual stroke of a mixed terrain: texture `layer` of the group `slot`, i.e.
    // the layer weights AND the group share move together. slot < 0 or a terrain with
    // no mix group is a plain paint().
    bool paintInGroup(TerrainComponent& tc, float localX, float localZ,
                      int layer, int slot, float radius, float falloff, float strength);

    // True when the second page exists, matches the first and carries any
    // weight at all — i.e. when the landscape actually uses layers 4..7.
    bool usesSecondPage(const TerrainComponent& tc);

    // The same for the third page (layers 8..11).
    bool usesThirdPage(const TerrainComponent& tc);

    // How many pages the uploaded texture carries: 1, 2 or 3 (the highest page in
    // use; a used third page carries the second one too, zeros if it is empty).
    int  weightPageCount(const TerrainComponent& tc);

    // The RGBA8 texture the shader samples as heLandscapeWeights. Without a used
    // second page it is layerWeights itself, weightRes × weightRes — byte for
    // byte what every landscape uploaded before there were eight layers. With
    // one it is 2·weightRes × weightRes: each row is the first page's row
    // followed by the second page's, so layers 0..3 sit in the left half and
    // 4..7 in the right half of ONE texture; a used third page makes it
    // 3·weightRes × weightRes with layers 8..11 in the last third. The material
    // shader tells the shapes apart by the texture's aspect ratio (MaterialGraph
    // codegen). Empty when the terrain has no valid weightmap.
    std::vector<uint8_t> buildWeightTexture(const TerrainComponent& tc,
                                            uint32_t& width, uint32_t& height);
}
