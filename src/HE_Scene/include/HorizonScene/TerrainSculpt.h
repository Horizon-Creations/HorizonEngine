#pragma once
#include <cstdint>

struct TerrainComponent;

// ─── Landscape height brushes, one shot at a time ────────────────────────────
// The sibling of TerrainPaint: brush operations on TerrainComponent::sculptHeights,
// pure CPU, no ContentManager and no renderer — so "did that raise land where it
// was asked, and nowhere else" is a question a test can put to a component on the
// stack.
//
// The interactive Landscape brush (TerrainTools.cpp) is NOT this. It is paced by
// dt, carries stroke-scoped state (the Flatten target and the Ramp start are
// captured when the mouse goes down) and runs a Ramp corridor between two points
// this file has no notion of. What lives here is the one-shot form the same maths
// takes when the caller is not a hand on a mouse: an external MCP client, a test,
// a script. The per-op blend factors are deliberately the ones TerrainTools uses,
// so the two cannot drift into looking different at the same settings.
//
// Everything is TERRAIN-LOCAL, like TerrainPaint::paint: x and z run over
// [-sizeX/2, sizeX/2] × [-sizeZ/2, sizeZ/2] and heights are world-Y units. A
// caller holding a world position subtracts the terrain entity's world position
// first — and reads it with HE::worldPositionOf, never off a worldMatrix, which
// is a frame old for anything created this frame.
namespace TerrainSculpt
{
    // What the brush does to the heights under it.
    enum class Op : uint8_t
    {
        Raise,     // h += weight * amount            (amount = world units)
        Lower,     // h -= weight * amount
        Set,       // h moves to `amount` by weight   (amount = target height)
        Flatten,   // like Set, but the target is the height under the brush centre
        Smooth,    // blends toward the 3×3 neighbourhood mean by weight * amount
        Roughen,   // adds a stable per-vertex hash in ±amount, scaled by weight
    };

    // Wire names, for anything that takes the op as a string. `opFromName`
    // returns false for an unknown one rather than picking a default: a typo
    // that silently raised the ground would be discovered by looking at it.
    const char* opName(Op op);
    bool        opFromName(const char* name, Op& out);

    // Bake the master height field into sculptHeights if it is not there yet, so
    // a brush has something to edit. Uses computeTerrainHeightField, which is the
    // documented precedence (sculpt > noise > flat) — a freshly seeded terrain
    // therefore keeps its fBm shape and the brush edits THAT, instead of flattening
    // the landscape on first touch.
    //
    // Also snaps `resolution` to the 2ⁿ+1 the chunk builder will snap it to anyway
    // (TerrainSystem::updateTerrains), resampling any heights that already exist.
    // Doing it here rather than leaving it to the next regen is what keeps a
    // caller's numbers meaningful: heights written at 128 would otherwise be
    // resampled to 129 behind its back, and the vertex it addressed would no
    // longer be the vertex it read back.
    void ensureHeights(TerrainComponent& tc);

    struct Result
    {
        bool     ok       = false;   // false = the arguments could not be applied
        uint32_t changed  = 0;       // vertices whose height actually moved
        float    minHeight = 0.0f;   // over the touched vertices, AFTER the brush
        float    maxHeight = 0.0f;
        float    centerHeight = 0.0f;// height at (localX, localZ) afterwards
    };

    // One brush dab at terrain-local (localX, localZ).
    //
    //   radius    full-strength inner radius, world units
    //   falloff   transition width outside it; the weight falls linearly to 0
    //   amount    per-op, see the Op comments above
    //
    // Calls ensureHeights first, so a terrain that has never been sculpted is
    // valid input. Sets the region-dirty rect (terrain-local XZ, brush extent) so
    // TerrainSystem regenerates only the chunks under the brush rather than all of
    // them; `dirty` is deliberately NOT set, which is the difference between a
    // dab and a rebuild.
    //
    // Returns ok = false for a degenerate terrain (non-positive size) or a
    // non-positive radius+falloff. A dab that lands entirely off the terrain is
    // ok = true with changed = 0 — it is a legal thing to ask and nothing happened.
    Result apply(TerrainComponent& tc, float localX, float localZ, Op op,
                 float radius, float falloff, float amount);
}
