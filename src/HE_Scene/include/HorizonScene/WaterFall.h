#pragma once
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterBrush.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── The waterfall band between two terraces (Thema 232 Schritt 5) ───────────
// Water is flat per body (Field::Body::level), so a stroke down a steep slope is a
// staircase of flat sheets (brush::Params::cascade, WaterBrush.h). This is what joins
// the steps: where the edge of a body's sheet runs against the cells of ANOTHER body
// whose level is at least kFallMinDrop lower, a ribbon of water runs from the upper
// sheet's edge down to the lower sheet, with a foam apron laid on the lower sheet at
// its foot. Nothing is stored: the bands follow from the field (owner grid + levels),
// so a hand-painted pair of ponds at different levels that touch gets one too, a pair
// at the same level (or within the threshold) never does.
//
// SHAPE. One mesh per UPPER body (the upper sheet's edge is what the ribbon hangs
// from, and the same lattice cut is shared bit for bit with the sheet, so top and
// sheet meet without a crack). Per piece of the sheet's edge that faces a lower body
// one ribbon quad: the top follows the edge at the upper level, the bottom lies where
// the ground, followed straight DOWNHILL from the edge point, comes down to the lower
// body's level (kFallMaxRun at most, then wherever the ground is), so a water film
// hugs the slope instead of standing as a wall in front of it; on a slope too gentle
// to run along, or where the ground is already below the lower level, the ribbon is a
// vertical drop. The apron continues kFallApron metres beyond the foot, a hair above
// the lower sheet (no z-fight). A piece is only hung where its foot lands in the
// lower body's water (the first cell past it that the upper body does not hold is the
// lower body's): where the ground runs away from the painted water, e.g. down the sides
// of a hilltop a stroke began on, the ribbon would end on dry land metres away.
//
// LOOK. No material of its own: the surface goes through the water material (the flow
// variant, running downhill, so ripples and foam streak down the slope). The foam is
// the engine's own soft-shore foam, which the material reads from the vertex normal's
// tilt (shoreProximityAt): 1 at the foot of the ribbon, falling over the apron to the
// lower sheet's own value at its far edge (from the water depth there, as the sheet's
// mesh has it) and, at the top, the upper sheet's own value, so the band is clear
// where it leaves the sheet and white where it lands.
namespace HE::water
{
    // Smallest level difference that makes a band: the terrace step (WaterBrush.h).
    inline constexpr float kFallMinDrop  = brush::kTerraceMinDrop;
    // How far down the slope a ribbon is followed looking for the lower level (m).
    inline constexpr float kFallMaxRun   = 8.0f;
    // The foam apron beyond the foot of a ribbon (m).
    inline constexpr float kFallApron    = 0.6f;
    // The apron and the ribbon's foot stand this far above the lower sheet (m).
    inline constexpr float kFallLift     = 0.02f;
    // The speed of the water's pattern along the band (m/s).
    inline constexpr float kFallSpeed    = 3.0f;

    // Bodies that have a lower neighbour by at least kFallMinDrop: their sheet touches
    // (4-neighbour, both cells wet) cells of another body whose level is lower. Sorted,
    // unique. One pass over the grid; empty when no band is due anywhere.
    std::vector<uint16_t> fallUppers(const TerrainComponent& tc);

    // The band mesh of `upper` (its own lattice, box and level as buildSurface takes
    // them), in the same space as the sheet's: vertices relative to `out.center`, y
    // relative to the upper level, so the entity stands at (center.x, level, center.y).
    // `flow` = the mean downhill direction times kFallSpeed, terrain-local XZ (what the
    // material's flow knob wants after the landscape's rotation). `out.empty()` when the
    // body has no edge against a lower one. False for an empty lattice or a field without
    // a grid. `out.hash` covers the geometry: equal hash, same mesh.
    bool buildFalls(const TerrainComponent& tc, uint16_t upper, const Lattice& lattice, const CellRect& cells,
                    float level, const SurfaceOptions& opt, SurfaceMesh& out, glm::vec2& flow);
}
