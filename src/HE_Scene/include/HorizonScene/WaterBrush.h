#pragma once
#include <HorizonScene/WaterField.h>
#include <cstdint>

struct TerrainComponent;

// ─── The water brush, one stroke at a time ───────────────────────────────────
// What the Landscape tool's Water mode does with the mouse, without the mouse:
// pure CPU, no ImGui, no ContentManager, so "did that drag leave water where it
// was dragged, at the level it should have" is a question a test (or the
// HE_DUMP_WATERBRUSH witness) can put to a TerrainComponent on the stack. The
// editor (TerrainTools.cpp) owns the cursor, the pacing and the undo step; this
// owns what a stroke means for the water model (WaterField.h) and the ground.
//
// A STROKE is begin(), any number of dab()s, end(). It paints into ONE body:
//
//   - pressed on water that is already there (a lake, or earlier brush water),
//     the stroke CONTINUES that body, at its level. That is what makes the brush
//     extend a lake instead of starting a second body with a level of its own next
//     to it, and it is the interoperability with the lake tool: same model, same
//     bodies, so a brush can widen a lake and an eraser can notch one.
//   - pressed on dry ground, it starts a NEW body ("brush water", no source
//     spline) at the stroke's level.
//
// The level is taken once, when the stroke begins, and held: dragging uphill does
// not tilt the water. Either the number in Params::level or, with levelFromGround,
// the terrain under the first point plus Params::levelOffset. All terrain-local,
// the space of Body::level and terrainHeightAt (a landscape standing at y = 300 has
// a level of 0.4 for a surface at 300.4). Water AT the ground's height is a sliver
// at best — the shore clipping (WaterField.h) removes the sheet where the ground
// stands above the level — so the offset is what makes a click on flat ground show:
// the first point is then that far under water.
//
// ERASING (Params::erase, stroke fixed at begin()) takes coverage out of every
// body under the circle and creates nothing.
//
// DIGGING (Params::dig, painting only) lowers the ground under the same circle
// toward `level − digDepth` (TerrainSculpt::lowerToFloor): a bed of that depth
// below the surface with a bank that eases back into the old ground. It never
// raises, and repeating the dab converges instead of digging without end.
//
// PACING is the caller's: Params::amount is the share (0..1) one dab moves each
// cell toward wet (or dry), the same number addCircle/removeCircle take as their
// strength, and the blend of the dig. 1 = one dab does all of it.
//
// UNDO is the caller's, as with every brush: take the editor's undo step once,
// then begin(). The whole stroke is the field plus (digging) the sculpt heights,
// and both ride the scene serializer. A Stroke holds a body id and a level, never
// an entity: undo rebuilds every entity.
namespace HE::water::brush
{
    struct Params
    {
        float radius  = 10.0f;     // full-strength inner radius (m)
        float falloff = 5.0f;      // transition width outside it (m)
        float amount  = 1.0f;      // 0..1 per dab, see PACING

        bool  erase = false;       // read at begin(); a stroke is all one or all the other

        bool  levelFromGround = true;
        float level       = 0.0f;  // used without levelFromGround (terrain-local Y)
        float levelOffset = 0.3f;  // above the ground under the first point

        bool  dig      = false;
        float digDepth = 1.0f;     // bed this far below the stroke's level (m)
    };

    struct Stroke
    {
        bool     active  = false;
        bool     erase   = false;
        uint16_t body    = kNoBody;   // 0 for an erasing stroke
        float    level   = 0.0f;
        bool     created = false;     // this stroke made `body` (false = it continued one)
        uint32_t dabs    = 0;
    };

    // What a stroke that began at (x, z) would paint into, for the cursor and the
    // panel: the body it would continue (kNoBody = a new one) and its level.
    struct Target
    {
        uint16_t body      = kNoBody;
        float    level     = 0.0f;
        bool     continues = false;
    };
    Target targetAt(const TerrainComponent& tc, float x, float z, const Params& p);

    // Start a stroke at terrain-local (x, z): resolve the body (continue or create)
    // and fix the level. Paints nothing — the caller dabs, including at this point.
    // Ends a stroke still running first. A terrain with no area, or a non-finite
    // point, leaves the stroke inactive.
    void begin(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p);

    struct Dab
    {
        bool     ok     = false;   // false: no stroke, or its body is gone (undo ran under it)
        uint32_t cells  = 0;       // water cells whose coverage or owner moved
        uint32_t ground = 0;       // terrain vertices lowered (digging)
    };
    // One dab at terrain-local (x, z). Sets the water field's dirty rectangle and,
    // when it dug, the terrain's region-dirty rectangle (never TerrainComponent::dirty
    // unless TerrainSculpt::ensureHeights had to snap the resolution).
    Dab dab(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p);

    // Finish: forget a brush body the stroke made and left without a single cell
    // (a press off the terrain), and reset the stroke. Safe on an inactive one.
    void end(Stroke& s, TerrainComponent& tc);
}
