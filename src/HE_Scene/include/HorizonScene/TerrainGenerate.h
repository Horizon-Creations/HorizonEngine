#pragma once
#include <cstdint>

struct TerrainComponent;

// ─── A mountain grown inside a chosen area, in one call ──────────────────────
// The fourth sibling of TerrainPaint, TerrainSculpt and TerrainHeightmap: pure
// CPU, no ContentManager and no renderer, so "did the ground rise inside the
// area and come back down to nothing at its edge" is a question a test can put
// to a component on the stack.
//
// Where TerrainSculpt is a brush — a disc of constant strength, dabbed again and
// again — this is an AREA operation: one elliptical footprint, one call, and the
// whole formation is there. The relief is the same fBm computeTerrainHeightField
// seeds a whole landscape with (terrainFbm), confined to the area and blended out
// towards its rim.
//
// The formation is ADDED to sculptHeights, never written over them: a landscape
// that was already sculpted, seeded or imported keeps every bump it had, and the
// mountain stands on top of it. Outside the area not a single vertex moves.
//
// Not here, deliberately: biomes, erosion, or anything that replaces the whole
// field (that is TerrainHeightmap's job).
//
// Everything is TERRAIN-LOCAL, like TerrainSculpt::apply: x and z run over
// [-sizeX/2, sizeX/2] × [-sizeZ/2, sizeZ/2] and heights are world-Y units. A
// caller holding a world position subtracts the terrain entity's world position
// first — read with HE::worldPositionOf, never off a worldMatrix.
namespace TerrainGenerate
{
    // The footprint: an axis-aligned ellipse around (centerX, centerZ) with
    // half-extents radiusX / radiusZ. A rectangle dragged in the viewport maps to
    // the ellipse inscribed in it. The formation never reaches past this rim.
    struct Area
    {
        float centerX = 0.0f;
        float centerZ = 0.0f;
        float radiusX = 0.0f;
        float radiusZ = 0.0f;
    };

    struct Params
    {
        // Height of the tallest point the formation ADDS, world units. The raw
        // shape is rescaled so its peak over the vertices actually on the terrain
        // lands exactly here — for an area hanging over the terrain's edge that
        // is the peak of the part that is on it. Negative digs a basin of the
        // same shape instead.
        float maxHeight = 30.0f;

        // Width of the blend to zero at the rim, WORLD UNITS (the same unit as
        // TerrainSculpt's falloff — not a 0..1 fraction). Measured inwards from
        // the rim along the ray from the centre; the weight eases in by
        // smoothstep over that distance, which is capped at the SHORT radius. On
        // a circle, a falloff at least as large as the radius turns the whole
        // area into one slope, a dome with its top at the centre; on a long
        // ellipse the same gives a level ridge along the long axis. A small
        // falloff gives a massif with steep flanks. 0 = hard edge.
        float falloff = 20.0f;

        // fBm detail, as in computeTerrainHeightField: `octaves` layers (1..12),
        // each at twice the frequency and half the amplitude of the last.
        int   octaves = 5;

        // How much of the height is fBm relief rather than the smooth profile,
        // 0..1. 0 = a clean dome/massif that follows the falloff exactly; 1 =
        // the profile fully modulated by the noise, peaks and saddles included.
        float roughness = 0.5f;

        // Noise features across the area's larger radius. Independent of the
        // terrain's size, so the same settings give the same-looking mountain in
        // a 100 m and a 4 km landscape.
        float frequency = 2.0f;

        // Same seed, same area, same params → the same heights, bit for bit.
        int   seed = 1;
    };

    struct Result
    {
        bool     ok        = false;  // false = the arguments could not be applied
        uint32_t changed   = 0;      // vertices whose height actually moved
        float    minHeight = 0.0f;   // over the changed vertices, AFTER the add
        float    maxHeight = 0.0f;
        float    peakAdded = 0.0f;   // the largest amount added (== Params::maxHeight
                                     // whenever anything changed)
    };

    // Grow one formation into `tc`.
    //
    // Calls TerrainSculpt::ensureHeights first, so a terrain that has never been
    // sculpted is valid input (a seeded one keeps its fBm shape underneath). Sets
    // the region-dirty rect to the area's bounding box so TerrainSystem rebuilds
    // only the chunks under it; `dirty` is NOT set (ensureHeights may still set
    // it when it has to snap the resolution to 2ⁿ+1).
    //
    // Returns ok = false for a degenerate terrain (non-positive size) or a
    // non-positive radius. An area entirely off the terrain, or maxHeight 0, is
    // ok = true with changed = 0 — a legal thing to ask, and nothing happened.
    Result mountain(TerrainComponent& tc, const Area& area, const Params& params);
}
