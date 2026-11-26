#pragma once
#include <cstdint>
#include <vector>

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
// On request (Params, all off by default) the relief is ridged, eroded, cut into
// terraces, and trails are routed up it and cut into the ground.
//
// Not here, deliberately: biomes, water erosion, or anything that replaces the
// whole field (that is TerrainHeightmap's job).
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

        // ── The realistic half. Every default below is "off": a caller that
        // sets none of them gets the formation this function always made. ──

        // How much of the relief is RIDGED noise (sharp crests, V-shaped
        // valleys, the coordinates warped so the ridgelines wander) rather
        // than the round fBm, 0..1. Only matters with roughness > 0.
        float ridges = 0.0f;

        // Thermal erosion, 0..1: slopes steeper than a scree slope (about 40°)
        // shed material downhill, a pass per 1/32 of the value. Softens the
        // noise's spikes into the debris fans a real flank has. The formation
        // never grows past its rim blend, so the rim stays where it was.
        float erosion = 0.0f;

        // Terracing: the height is quantised into this many steps, level
        // benches with risers between them (0 = off). `terraceSharpness` 0..1
        // decides how much of a step is bench: 0 leaves the slope as it was,
        // 1 is a staircase.
        int   terraces = 0;
        float terraceSharpness = 0.7f;

        // Paths: this many trails (0..8) from the rim up to the summit, spread
        // round the area, each routed so that it climbs no steeper than
        // `pathMaxGrade` (rise over run) wherever the ground allows: on a steep
        // flank that makes it traverse and turn back. Each is cut into the
        // ground as a bench `pathWidth` world units wide, level across, blended
        // into the slope over another half width on either side. A path never
        // leaves the area. Their lines come back in Result::paths.
        int   paths = 0;
        float pathWidth = 4.0f;
        float pathMaxGrade = 0.3f;
    };

    // A point of a path: terrain-local x / z like everything here, y the height
    // of the ground under it AFTER the call (sculptHeights' unit).
    struct PathPoint { float x = 0.0f, y = 0.0f, z = 0.0f; };

    struct Result
    {
        // The paths that were cut (Params::paths), rim first, summit last, a
        // point every path width or so: what a caller turns into a spline.
        // Fewer than asked for when the area is mostly off the terrain.
        std::vector<std::vector<PathPoint>> paths;

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
