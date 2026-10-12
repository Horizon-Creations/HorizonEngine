#pragma once
#include <glm/vec2.hpp>
#include <cstdint>
#include <limits>
#include <vector>

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

    // Widen TerrainComponent's region-dirty rect (terrain-local XZ) to cover
    // [mnX, mxX] × [mnZ, mxZ], same merge as every op in this file. For a caller
    // that edits sculptHeights directly, outside apply/lowerToFloor/excavatePolygon
    // (WaterLake.cpp's ground-patch restore, moving a lake's own dig when its
    // transform moves: see WaterLake.h).
    void markRegionDirty(TerrainComponent& tc, float mnX, float mnZ, float mxX, float mxZ);

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

    // ── Lowering toward a floor under a brush ────────────────────────────────
    // What a water brush that digs does: ground above `floor` eases down toward
    // it, ground at or below it is left alone, so it never raises and never fills
    // a pit in. Per vertex, with the brush weight w (1 inside `radius`, linear to
    // 0 across `falloff`, like apply) and f = clamp(w · blend, 0, 1):
    //     h > floor  →  h + f · (floor − h)        (exactly `floor` when f == 1)
    // Stateless, so a dab repeated at one spot converges on the floor instead of
    // digging without end, and the bank (the falloff ring) meets the old ground
    // without a step. `blend` is the caller's pacing: 1 = one dab does all of it,
    // a held button passes a fraction per frame.
    //
    // `wetLevel`, finite (Thema 174 Schritt 13): the water's own level, so a dab
    // never lowers the ground below it where a SINGLE dab's own circle would not
    // paint that ground wet — the circle's weight at half strength (0.5, the
    // circle's own coverage-wet boundary: `WaterField::addCircle` reaches a
    // coverage half of 255 at the same weight, for one full-strength dab) is the
    // line. Below it (w < 0.5) the CEILING is `wetLevel`, whatever `floor` says;
    // from there to w = 1 it eases linearly to `floor`, same as the weight itself
    // eases. The result is the LOWER of that ceiling-blend and the plain old
    // blend toward `floor` (never raised past either): on flat ground that is the
    // ceiling (the old blend alone would dig too far); on ground ABOVE the level
    // (an upward slope under the stroke) the plain blend alone would often UNDER-
    // dig — at w = 0.5 it only reaches halfway down, short of the level, which
    // would leave this exact lattice corner un-wet right where the paint just
    // wet it, "water stops too early" on a slope. Taking the lower of the two
    // reaches the level in one dab at w = 0.5 on any starting height, up or down.
    // Without this cap entirely, a dab's falloff ring reaches well past where its
    // own paint ever turns the coverage wet (coverage needs w ≥ 0.5, the dig used
    // to reach all the way to `floor` as soon as w > 0): a dry ring, sunk below
    // the water's own level, around every stroke — the water brush's share of the
    // bug `WaterLake.cpp`'s outward bank had (see ExcavateParams::bankIn).
    // NaN (the default) disables this and reproduces the old, unbounded dig bit
    // for bit — every caller before Schritt 13 passed none. The cap is per dab,
    // not per the stroke's accumulated coverage: a point several dabs have nudged
    // toward wet without any one of them alone reaching w ≥ 0.5 there stays capped
    // a little shallow rather than risk the dry ring the other way; that is a
    // shallower shore, never a moat.
    //
    // Same contract as apply: terrain-local coordinates, ensureHeights first, the
    // region-dirty rect (vertices that moved, one grid step wider) and not `dirty`,
    // undo is the caller's. ok = false for a degenerate terrain, a non-positive
    // radius + falloff or a non-finite argument; a dab off the terrain is ok with
    // changed = 0.
    Result lowerToFloor(TerrainComponent& tc, float localX, float localZ,
                        float radius, float falloff, float floorY, float blend,
                        float wetLevel = std::numeric_limits<float>::quiet_NaN());

    // ── Excavation under a polygon ───────────────────────────────────────────
    // The ground inside a polygon goes down to a floor, with a soft bank around
    // it. What a lake bed is made of, and anything else that wants a pit of a
    // given shape: a pond, a moat, a foundation.
    //
    //   Floor   `amount` is the height of the bottom (terrain-local Y, like the
    //           heights). Ground above it is lowered to it; ground already below
    //           it stays where it is — an excavation never raises, so a pit that
    //           is deeper than asked for is not filled in.
    //   Dig     `amount` is a depth: every vertex under the polygon goes down by
    //           that much, the shape of the ground is kept. Negative = 0.
    enum class ExcavateMode : uint8_t { Floor, Dig };

    struct ExcavateParams
    {
        ExcavateMode mode    = ExcavateMode::Floor;
        float        amount  = 0.0f;
        // Width of the bank, world units, measured OUTWARD from the polygon's
        // outline: the weight is 1 on and inside the outline and eases to 0 over
        // this distance (smoothstep, so the bank meets the old ground without a
        // crease — the brush's linear ramp would leave a visible edge on a pit
        // this wide). 0 = a hard edge. Beyond it the ground is not touched at all.
        float        falloff = 2.0f;
        // Width of a second bank, world units, measured INWARD from the outline
        // instead of outward. Within it, Floor mode's clamp eases from
        // `edgeHeight` (right on the outline) down to `amount` (this far in and
        // beyond); Dig mode's depth eases from 0 (right on the outline) up to the
        // full depth the same way, `edgeHeight` unused. 0 = disabled, the whole
        // inside is `amount` (or the full depth) as `falloff` alone describes.
        //
        // Floor mode only, with this > 0, ALSO changes what the OUTWARD `falloff`
        // bank eases toward: `edgeHeight` instead of `amount`. Both banks then
        // reach the exact same height right on the outline (`edgeHeight`) and the
        // ground never has a seam there — without this, `falloff` alone eases
        // outward toward `amount` (the DEEP floor), so ground just outside the
        // outline could sink to the SAME depth as the lake bed, well below the
        // water's own level, with no water ever laid over it there (a dry moat
        // around the shape, outside what the caller drew — the bug this pairing
        // exists to fix, Thema 174 Schritt 13 first pass) — OR, with `falloff` set
        // to 0 to dodge that (the same schritt's own first attempt), ground just
        // inside drops to `edgeHeight` while ground one vertex outside stays at
        // its old height, whatever that was: a cliff on any ground that was not
        // already flat at the outline (second pass). Set both to the SAME width
        // for a lake's "Bank": the ground only ever eases DOWN where it stands
        // above `edgeHeight`, continuously inward to the bed and outward back up
        // to the old ground, and nothing anywhere sinks below the water's level
        // without the water (laid over the same polygon) reaching it too.
        float        bankIn     = 0.0f;
        float        edgeHeight = 0.0f;   // Floor mode only
    };

    // The polygon is terrain-local XZ (x → glm::vec2::x, z → ::y), no closing
    // duplicate, even-odd fill, any winding — the object water::polygonFromPolyline
    // returns, so a lake feeds the SAME polygon to the water and to this. Fewer
    // than three points, a non-finite one, no area or a non-finite amount/falloff
    // give ok = false and change nothing. The part outside the terrain is
    // clipped; a polygon entirely off it is ok = true with changed = 0.
    //
    // Heights are computed per terrain VERTEX: a vertex inside the outline gets
    // exactly the floor (or exactly its old height minus the depth), one beyond
    // the bank keeps its old bits. A vertex on the outline counts as inside, up to
    // rounding on a slanted edge. With `bankIn` set (Floor mode), a vertex inside
    // the outline but within `bankIn` of it is eased toward `edgeHeight` instead —
    // see ExcavateParams::bankIn.
    //
    // Like apply: calls ensureHeights first, writes sculptHeights only, and marks
    // the region-dirty rect (the vertices that moved, plus one grid step so the
    // chunks that share a border vertex both rebuild). `dirty` stays as it was.
    // TerrainSystem rebuilds every LOD level of each chunk the rect touches and
    // hands the same rect to the water surface for its shore clipping.
    //
    // UNDO IS THE CALLER'S, as with apply: take the editor's undo step
    // (EditorUndo::snapshotNow) once, then call this once. sculptHeights is saved
    // as raw float bytes, so the snapshot gives the old ground back bit for bit,
    // including the case where this call baked the noise field into sculptHeights
    // or snapped the resolution.
    //
    // Result: changed = vertices whose height moved; min/max over the vertices the
    // polygon and its bank reach, AFTER the edit; centerHeight = the ground at the
    // centre of the polygon's bounding box afterwards.
    Result excavatePolygon(TerrainComponent& tc, const std::vector<glm::vec2>& polygon,
                           const ExcavateParams& params);
}
