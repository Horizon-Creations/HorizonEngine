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

    // Every texel fully on `layer` - "start the landscape from this layer". Allocates the
    // weightmap if there is none (and the pages below `layer` for layers 4..11); a
    // layer on a lower page clears the higher ones, whose weights would otherwise
    // outvote it. Sets tc.weightsDirty. False for a layer outside 0 .. kTerrainMaxLayers-1.
    bool fillLayer(TerrainComponent& tc, int layer);

    // The kTerrainMaxLayers weights of texel (tx, tz), the layers of a missing page
    // zero. False (out untouched) when the terrain has no weightmap or the texel is
    // outside it.
    bool texelWeights(const TerrainComponent& tc, uint32_t tx, uint32_t tz, uint8_t* out);

    // ── Mixed texture groups (TerrainComponent::groupMix) ─────────────────────
    // With groupMix on, weight layers 8..11 hold the SHARE of group 0..3 (the
    // AutoLandscapeGroupId) of every texel, summing to 255, and are not paint layers: the
    // layers 0..7 keep their own 255-sum without them, paint()/fillLayer() leave the share
    // bytes alone and refuse to paint a reserved layer, and the group brush below is the
    // only thing that writes one. Off, everything is as before.
    inline constexpr int kGroupCount           = 4;
    inline constexpr int kGroupShareFirstLayer = 8;
    // 0xF while groupMix is on (bit g = group g's share layer is reserved), else 0.
    int  reservedLayers(const TerrainComponent& tc);
    bool isReservedLayer(const TerrainComponent& tc, int layer);

    // Turn the mix on: every texel starts fully in `baseGroup` (the terrain's own, so the
    // look does not change), and what a custom material painted on layers 8..11 is folded
    // into layer 0. False if it was on already.
    bool enableGroupMix(TerrainComponent& tc, int baseGroup);
    // Turn it off: the shares are dropped, layers 8..11 are plain paint layers again.
    bool disableGroupMix(TerrainComponent& tc);

    // The four shares of texel (tx, tz) by group id, summing to 1. A terrain without a mix is
    // {1, 0, 0, 0}. False (out untouched) when there is no weightmap or the texel is outside.
    bool groupShares(const TerrainComponent& tc, uint32_t tx, uint32_t tz, float* out4);

    // The group brush: move each touched texel's shares toward "all `group`", with the same
    // radius / falloff / strength as paint(). False when the mix is off or `group` is no group.
    bool paintGroup(TerrainComponent& tc, float localX, float localZ,
                    int group, float radius, float falloff, float strength);

    // Every texel fully in `group`; layers 0..7 stay as they are.
    bool fillGroup(TerrainComponent& tc, int group);

    // The manual stroke of a mixed terrain: texture `layer` of `group`, i.e. the layer weights
    // AND the group share move together. group < 0 or a terrain without a mix is a plain paint().
    bool paintInGroup(TerrainComponent& tc, float localX, float localZ,
                      int layer, int group, float radius, float falloff, float strength);

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
