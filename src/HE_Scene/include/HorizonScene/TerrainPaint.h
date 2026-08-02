#pragma once
#include <cstdint>

struct TerrainComponent;

// ─── Landscape layer painting ────────────────────────────────────────────────
// Brush operations on TerrainComponent::layerWeights (RGBA8, channel = layer).
// Pure CPU, no ContentManager / renderer — freely testable, mirroring how
// TerrainMeshGenerator keeps the heightfield maths out of the editor.
namespace TerrainPaint
{
    // Allocate the weightmap if it doesn't exist yet: every texel fully on layer
    // 0, which is what an unpainted landscape already renders as (the default
    // 1x1 weightmap is (1,0,0,0)), so turning painting on never changes the look.
    // No-op when a correctly sized map is already present.
    void ensureWeightmap(TerrainComponent& tc);

    // Paint `layer` at terrain-LOCAL (x, z) — the same space the sculpt brushes
    // use, i.e. [-sizeX/2, sizeX/2] × [-sizeZ/2, sizeZ/2].
    //
    // radius      full-strength inner radius, world units
    // falloff     transition width outside it; strength falls linearly to 0
    // strength    0..1 per call — how far each texel moves toward "all `layer`"
    //
    // The touched texels stay normalised (weights sum to 255): the painted layer
    // gains, the others give up the same amount proportionally. That is what
    // makes painting reversible by simply painting a different layer over it.
    //
    // Sets tc.weightsDirty so TerrainSystem re-uploads the texture, and widens
    // the sculpt region-dirty rect so callers can reuse it. Returns false when
    // the layer index is out of range or the terrain has no weightmap.
    bool paint(TerrainComponent& tc, float localX, float localZ,
               int layer, float radius, float falloff, float strength);
}
