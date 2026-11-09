#pragma once
#include <HorizonScene/WaterField.h>
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── The water brush, one stroke at a time ───────────────────────────────────
// What the Landscape tool's Water mode does with the mouse, without the mouse:
// pure CPU, no ImGui, no ContentManager, so "did that drag leave water where it
// was dragged, at the level it should have" is a question a test (or the
// HE_DUMP_WATERBRUSH witness) can put to a TerrainComponent on the stack. The
// editor (TerrainTools.cpp) owns the cursor, the pacing and the undo step; this
// owns what a stroke means for the water model (WaterField.h) and the ground.
//
// A STROKE is begin(), any number of dab()s, end(). It paints into ONE body (but
// see Params::cascade: dragged down a steep slope it can step into new bodies):
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
// DIGGING (Params::dig, painting only) shapes a BED under the water (Thema 206
// Schritt 9) from the SHORE, not from the brush: every vertex the body's sheet
// covers (WaterMesh.h insideSheet: the lattice, marching squares and shore
// clipping the mesh is built with) goes down to
//     level − min(maxDepth, slope · d),   d = the distance to the shoreline,
// so the bed falls from the bank at `slope` metres per metre and is flat at
// `maxDepth` (Params::digDepth) from maxDepth / slope metres in. Painting on or
// moving the shore outward makes d larger and the next dab digs the newly inner
// ground down the same way, up to kMaxBedReach past the dab. `bedRoughness` (0..1)
// is a share of that depth that a fixed noise (a function of the position only)
// adds or takes away: nothing at the shoreline, a little variation on the floor.
// Ground above the level inside the sheet goes to the level at once (it would
// otherwise clip the very water that is to cover it). It never raises, digs only
// under the water, and is a function of the shore: repeating a dab converges
// instead of digging without end, and no dry ring sinks below the level.
// On rising ground that alone leaves a WALL where the pit ends, so the dig then
// shapes a BANK (Thema 188 Schritt 4): ground outside the sheet is lowered to a
// slope that starts just above the water's reach at the shoreline and rises to meet
// the old ground within the brush's radius + falloff of the shore. Dry land, never
// below the water, never raised.
// The settings the bed was dug with are kept on the body (Body::brushBed, saved), and
// so is what the digs moved (Body::brushPatch, the sum of every dab's change in
// height). With Params::closeBed the ERASER uses it: ground under water it has just
// dried goes back toward where it stood before the brush dug it (by `amount`, weighted
// by the brush profile), and the patch shrinks by what was given back. Ground the
// user sculpted afterwards keeps that change; only the brush's own share is returned.
//
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
        float digDepth = 1.0f;     // Max Depth: the bed never goes deeper below the stroke's level (m)
        float bedSlope = kDefaultBedSlope;   // Slope: bed sinks this many metres per metre from the shore
        bool  closeBed = false;    // erasing also gives the ground the brush dug under the erased water back
        float bedRoughness = 0.0f; // Bed Roughness 0..1: share of the depth a fixed noise adds or takes

        // Flow From Slope (Thema 232): switches Body::flowAuto on for the body this
        // stroke paints into, so end() derives its current from the ground (slopeFlow).
        // Off never switches it OFF: a body that already follows the ground keeps
        // doing so, and one that does not keeps its own physics.flow untouched.
        bool  autoFlow = false;

        // Terraces (Thema 232 Schritte 4 und 11, docs/water-flow-2026-11-09.md): water
        // stays flat per body, so a stroke dragged DOWN a slope would only grow an
        // ever deeper wedge, a sheet hanging in the air over the falling ground. With
        // this on, the stroke paints a LADDER of flat steps, one body each, every one
        // kTerraceDrop below the one above (the first is the stroke's level). Where
        // the ground under the brush is steeper than kTerraceSlope, every cell the
        // dab wets goes to the HIGHEST step that holds it at most lipDepth deep
        // (kTerraceDepth, or Level Offset when that is deeper): a step is a shallow
        // pool whose downhill lip stands at most that high over the ground, and the
        // ground between its lip and the next step's waterline belongs, hidden under
        // the slope, to the step below, which is where the waterfall band runs
        // (WaterFall.h). The next step is made once the front half of the brush
        // (the way it moves) reaches ground deeper than that under the lowest step;
        // a cliff makes several at once, those that get no cell go at end(). Cells
        // move only between steps this stroke made; a body it continued, and every
        // other body, keep their cells. Level ground and gentle slopes never step,
        // so a pond on them is still one body; the steep banks of a pond do step.
        // Off by default: "one stroke, one body" is the rule everywhere else.
        bool  cascade = false;
    };

    // Terrace thresholds, calibrated on the hill witness (HE_DUMP_WATERLAKE=hill,
    // docs/water-flow-2026-11-09.md, Schritt 11): a step every metre or so made a
    // staircase of hovering plates, few long falls with shallow pools read as water
    // running down the slope.
    inline constexpr float kTerraceDepth   = 0.5f;    // m: the most water a step holds at its downhill lip (at least)
    inline constexpr float kTerraceDrop    = 2.0f;    // m between two steps of one stroke
    inline constexpr float kTerraceSlope   = 0.3f;    // m/m of ground under the brush (about 17 degrees) from which a stroke steps
    inline constexpr float kTerraceMinDrop = 0.4f;    // m: smallest level difference that hangs a waterfall band (WaterFall.h)
    static_assert(kTerraceDrop >= kTerraceMinDrop, "two steps of a stroke must always be far enough apart for a band");

    // The lip depth of a cascade stroke's steps: kTerraceDepth, or the stroke's
    // Level Offset when it asks for deeper water (levelFromGround only).
    float lipDepth(const Params& p);

    // Steepness of the terrain at terrain-local (x, z): |∇h| by central differences
    // one water cell apart. 0 for a landscape with no area or a non-finite point.
    float groundSlope(const TerrainComponent& tc, float x, float z);

    // Candidate thresholds (nothing measured yet, docs/water-flow-2026-11-09.md §5):
    // below kFlowStillSlope the water stands, from kFlowFallSlope on it runs at
    // kFlowMaxSpeed; between them the speed follows a smoothstep.
    inline constexpr float kFlowStillSlope = 0.03f;   // m/m, about 1.7 degrees
    inline constexpr float kFlowFallSlope  = 0.7f;    // m/m, about 35 degrees
    inline constexpr float kFlowMaxSpeed   = 3.0f;    // m/s

    // The current the ground under a body's wet cells asks for: the downhill
    // direction, i.e. minus the mean of the terrain's height gradient over the wet
    // cells (central differences one water cell apart, terrainHeightAt), at a speed
    // of kFlowMaxSpeed · smoothstep(kFlowStillSlope, kFlowFallSlope, |mean|).
    // Terrain-local XZ as BodyPhysics::flow (z → .y). A bowl averages to ~0, so a
    // pond stands without a special case. Zero for a body with no wet cell.
    glm::vec2 slopeFlow(const TerrainComponent& tc, uint16_t body);

    struct Stroke
    {
        bool     active  = false;
        bool     erase   = false;
        uint16_t body    = kNoBody;   // 0 for an erasing stroke
        float    level   = 0.0f;
        bool     created = false;     // this stroke made its FIRST body (false = it continued one)
        uint32_t dabs    = 0;
        // Params::cascade: every body the stroke has painted into, in order (the
        // first one, then one per terrace). `body` and `level` are the last of them.
        // Every entry after the first was made by this stroke.
        std::vector<uint16_t> bodies;
        uint32_t terraces = 0;        // steps made so far (bodies.size() - 1)
        // Where the previous dab went (Params::cascade: the way the brush moves
        // decides which half of it may make the next step).
        bool      hasLast = false;
        glm::vec2 last{ 0.0f, 0.0f };
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
        uint32_t ground = 0;       // terrain vertices lowered (digging), the bank's included
        uint32_t bank   = 0;       // ... of them shaved into the bank, outside the water and above it
        bool     terrace = false;  // this dab made a new step (Params::cascade), or several: it painted into the lowest
    };
    // One dab at terrain-local (x, z). Sets the water field's dirty rectangle and,
    // when it dug, the terrain's region-dirty rectangle (never TerrainComponent::dirty
    // unless TerrainSculpt::ensureHeights had to snap the resolution).
    Dab dab(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p);

    // Finish: forget a brush body the stroke made and left without a single cell
    // (a press off the terrain), and reset the stroke. Safe on an inactive one.
    void end(Stroke& s, TerrainComponent& tc);
}
