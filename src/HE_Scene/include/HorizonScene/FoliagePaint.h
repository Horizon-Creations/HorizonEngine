#pragma once
#include <cstdint>

struct FoliageComponent;
struct TerrainComponent;

// ─── Foliage density painting ────────────────────────────────────────────────
// Brush operations on FoliageComponent::densityMask (one byte per texel, 255 =
// full density, 0 = excluded). Pure CPU, no ContentManager / renderer — freely
// testable, the same split TerrainPaint keeps for the layer weights.
//
// The mask is laid over the TERRAIN the layer scatters on, which is why every
// call takes the TerrainComponent too: it says how big the landscape is, and
// with it which texel a terrain-local (x, z) lands in.
namespace FoliagePaint
{
    // Allocate the mask if it doesn't exist yet: every texel at 255, which is
    // what an unpainted layer already scatters as, so turning painting on never
    // changes the layout. No-op when a correctly sized mask is already present.
    void ensureMask(FoliageComponent& fol);

    // Drop the mask: back to a uniform scatter over the whole landscape.
    void clearMask(FoliageComponent& fol);

    // Set every texel to `value` (0..1). 1 = everything grows, 0 = nothing does;
    // the way to start from "bare, then paint the meadows in".
    void fillMask(FoliageComponent& fol, float value);

    // Paint at terrain-LOCAL (x, z) — the same space the sculpt and layer
    // brushes use, i.e. [-sizeX/2, sizeX/2] × [-sizeZ/2, sizeZ/2].
    //
    // radius      full-strength inner radius, world units
    // falloff     transition width outside it; strength falls linearly to 0
    // strength    0..1 per call — how far each texel moves toward `target`
    // target      0..1 — the density the brush paints toward: 1 grows the full
    //             layer density, 0 erases (that is the exclusion brush), and
    //             anything between thins the scatter to that fraction
    //
    // Sets fol.dirty so FoliageSystem re-scatters on its next update. Returns
    // false when the terrain has no area or the brush covers no texel.
    bool paint(FoliageComponent& fol, const TerrainComponent& tc,
               float localX, float localZ,
               float radius, float falloff, float strength, float target);

    // The mask's value at terrain-local (x, z), 0..1, bilinearly filtered so a
    // soft brush edge thins the scatter smoothly rather than in texel steps.
    // 1 when the layer has no mask (uniform scatter).
    float sample(const FoliageComponent& fol, const TerrainComponent& tc,
                 float localX, float localZ);

    // Mean of the mask over the whole landscape, 0..1 (1 without a mask). What
    // fraction of the uniform instance count the painted layer will keep.
    float coverage(const FoliageComponent& fol);
}
