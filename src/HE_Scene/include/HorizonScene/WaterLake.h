#pragma once
#include <HorizonScene/WaterField.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/HorizonWorld.h>    // Entity, HorizonWorld
#include <Types/UUID.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <string>
#include <vector>

struct TerrainComponent;

// ─── The lake: a closed spline that owns a body of water ─────────────────────
// The second writer of the water model (WaterField.h) next to the brush
// (WaterBrush.h). A lake is a Body whose `sourceSpline` is a closed spline's
// entity id and whose `polygon` is that spline's outline, as last laid into the
// cells. Everything else — level, cells, shore clipping, the surface mesh — is the
// body any brush stroke paints into, which is what makes the two interoperable:
// a stroke that starts on a lake continues it, an eraser notches it, and the lake
// can be reshaped afterwards.
//
// Two layers in this file, like the rest of the water code:
//
//   - the PURE one on a TerrainComponent (create, reshape, dig, detach, extract
//     and adopt an outline): no registry, no ImGui, no content manager, so a test
//     can put a polygon to a component on the stack. Terrain-local XZ throughout,
//     as in WaterField.h.
//   - the ENTITY one on a HorizonWorld (the spline's polygon in the terrain's
//     space, create / reshape / dig by entity, the per-frame sync, converting a
//     brushed body into a lake). Used by the Lake panel, the engine API
//     (HE::api::water), HorizonCode and the MCP tool.
//
// ── WHAT EDITS TOUCH WHAT ────────────────────────────────────────────────────
//   Moving a spline point        the WATER only (reshape). Never the ground: a
//                                bank dug once stays where it was dug.
//   "Create Lake"                the ground (excavatePolygon, Floor mode) AND the
//                                water, in one call, one undo step for the caller.
//   "Dig Again"                  the ground only, from the lake's current outline,
//                                on an explicit action and nothing else.
//   Changing the level           the water only (water::setLevel).
//   Shore clipping              a landscape-wide setting (Field::clipToGround), not
//                                per lake: the Lake panel shows it, it is not a
//                                lake parameter.
//
// ── THE MERGE RULE: brush share over spline share ────────────────────────────
// Each lake has two shares of water. The SPLINE SHARE is what its outline says: the
// cells polygon covers. The BRUSH SHARE is the difference between that and the
// cells as they are now, and it is not stored: it is read off the cells at the
// moment the lake is reshaped, against the outline the cells were last laid from
// (Body::polygon, "old"). With oldR / newR the outline rasterised at the field's
// resolution (rasterizePolygon, anti-aliased coverage 0..255):
//
//     brush added   a cell the body owns with more coverage than oldR says there
//     brush erased  a cell with less coverage than oldR says, that the body still
//                   owns or that is dry now (a cell another body took is not an
//                   erase: it is that body's)
//
//     reshaped cells  =  ( newR  ∪  brush added )  −  brush erased
//
// where ∪ is the larger coverage and − takes the cell down to what the brush left
// there (0 for a full erase, a partial value for a soft one), and the body's
// outline becomes the new polygon. The consequences, which the tests assert one by
// one and which are the whole contract:
//
//   1. Shrinking the outline dries the area only the OLD outline covered.
//   2. Growing it absorbs brush water the new outline now covers; nothing doubles.
//   3. Brush water outside both outlines stays where it was painted.
//   4. A notch the eraser cut inside the outline stays a notch, and moves with
//      nothing: it is a place in the world, not part of the shape.
//   5. A cell the new outline covers properly (coverage >= kWet) is taken from
//      another body, as addPolygon does; the polygon is authoritative.
//   6. Re-running reshape with the same polygon changes nothing.
//
// What the rule cannot know: a notch outside the new outline is remembered only
// until the next reshape (once the cell is dry and outside, there is nothing left
// to compare against), and changing the field's resolution (setResolution)
// resamples the cells but not the baseline, so the next reshape after it can read
// a thin ring of "brush" along the old shore. Neither is a loss of water; both are
// the price of storing the outline instead of a second raster.
//
// ── CONVERTING A BRUSHED BODY ────────────────────────────────────────────────
// extractOutline reads the largest outer boundary of a body's cells (the same
// contour the surface mesh is built from), simplifies it to a handful of control
// points and says how many other pieces and islands it left out. The caller draws
// a closed spline through them; adopt then makes the body that spline's lake: the
// main piece is replaced by the spline's fill, so the lake reshapes cleanly from
// then on. What it changes about the picture, on purpose: the ragged painted edge
// becomes the smooth spline, islands inside are filled (carve them back with the
// eraser; an erase survives reshapes), and a separate piece farther than a cell
// from the main one is kept as brush water.
namespace HE::water::lake
{
    // ── Parameters ───────────────────────────────────────────────────────────
    struct Params
    {
        // Wasserspiegel. FromGround takes the lowest ground under the outline plus
        // `levelOffset`, a number takes `level` (terrain-local Y, the space of
        // Body::level and terrainHeightAt).
        bool  levelFromGround = true;
        float level           = 0.0f;
        float levelOffset     = 0.0f;
        // Tiefe: the bed lies this far below the level. Randverlauf: the width of
        // the bank that eases back into the old ground (metres, measured outward
        // from the outline; smoothstep).
        bool  dig   = true;
        float depth = 2.0f;
        float bank  = 4.0f;
    };

    // How finely a spline is flattened into the polygon that is laid into the
    // cells: a tenth of a water cell, clamped to 2 cm .. 50 cm. Finer than the grid
    // can show, so the outline is the curve as far as the water is concerned, and
    // coarse enough that dragging a point on a big landscape does not rasterise
    // thousands of edges per frame. One function for every caller, because the merge
    // rule only holds when the polygon that is laid and the polygon that is compared
    // later are made the same way.
    float polylineTolerance(const TerrainComponent& tc);

    // ── Pure, on a TerrainComponent ──────────────────────────────────────────
    // Lowest terrain height at the polygon's vertices (terrain-local). The default
    // water level of a new lake.
    float lowestGround(const TerrainComponent& tc, const std::vector<glm::vec2>& polygon);

    // A body for `spline` with `polygon` laid into the cells at `level`; when the
    // spline already has one, the same as reshape + setLevel. Returns the body id,
    // kNoBody for an unusable polygon (fewer than three vertices, no area, wholly
    // off the terrain) or a null spline id — nothing is created then.
    uint16_t create(TerrainComponent& tc, const HE::UUID& spline,
                    const std::vector<glm::vec2>& polygon, float level);

    // Lay a new outline into the lake, merging the brush share (see above). Not ok
    // for an unknown body or an unusable polygon; the cells are left as they were.
    // `changed` counts cells that moved. Sets the field's dirty rectangle, never
    // the terrain's.
    Result reshape(TerrainComponent& tc, uint16_t body, const std::vector<glm::vec2>& polygon);

    // The ground under the lake's outline goes down to `level − depth`, with a bank
    // of `bank` metres (TerrainSculpt::excavatePolygon, Floor mode: never raises).
    // Explicit by design: reshape never calls it.
    TerrainSculpt::Result dig(TerrainComponent& tc, uint16_t body, float depth, float bank);

    // The body keeps its water and level but stops being a lake: no spline, no
    // outline. For a lake whose spline is gone.
    bool detach(TerrainComponent& tc, uint16_t body);

    // ── Brush water → lake ───────────────────────────────────────────────────
    struct Outline
    {
        bool ok = false;                  // false: no water, or too small to hold a spline
        std::vector<glm::vec2> points;    // simplified ring, counter-clockwise, no closing duplicate
        uint32_t pieces  = 0;             // outer boundaries the body has (1 = a single lake)
        uint32_t islands = 0;             // holes inside the ring that was chosen
        float    tolerance = 0.0f;        // the simplification tolerance that fitted `maxPoints`
    };
    // The outline of the body's largest piece. `tolerance` (metres, <= 0 = one
    // water cell) is the start of the simplification; it grows until the ring fits
    // `maxPoints` (clamped to 3..256).
    Outline extractOutline(const TerrainComponent& tc, uint16_t body, float tolerance = 0.0f,
                           uint32_t maxPoints = 32);

    // Make `body` (a brush body) the lake of `spline`, whose polygon is `polygon`:
    // the body's main piece is replaced by the polygon's fill (the piece
    // extractOutline described), the rest of its water is kept. Not ok for a
    // body that already is a lake, an unusable polygon or a null spline id.
    Result adopt(TerrainComponent& tc, uint16_t body, const HE::UUID& spline,
                 const std::vector<glm::vec2>& polygon);

    // ── Entity layer ─────────────────────────────────────────────────────────
    // The spline's outline in the terrain's local XZ: the adaptive polyline of the
    // curve (kPolylineTolerance), moved by inverse(terrain world) * spline world,
    // both composed on the spot (HE::worldMatrixOf — the stored matrices are a
    // frame old). False when either is not an entity of the right kind, or the
    // spline is not a closed shape of at least three points.
    bool polygonOf(HorizonWorld& world, Entity spline, Entity terrain, std::vector<glm::vec2>& out);

    // The landscape and body that `spline` is the lake of. entt::null / kNoBody if none.
    struct Link { Entity terrain = entt::null; uint16_t body = kNoBody; };
    Link linkOf(HorizonWorld& world, Entity spline);

    struct Created
    {
        bool        ok = false;
        std::string error;       // set when !ok, a sentence for the user
        uint16_t    body = kNoBody;
        float       level = 0.0f;       // terrain-local
        uint32_t    cells = 0;          // water cells the outline put down
        uint32_t    ground = 0;         // terrain vertices lowered by the excavation
    };
    // The "Create Lake" action: level from the parameters, then (with `dig`) the
    // excavation, then the water. The ground is dug FIRST and the level is read
    // before that, so "lowest ground under the outline" means the ground as it was.
    // One call for the caller to wrap in one undo step (EditorUndo::snapshotNow).
    // Refused if the spline already is a lake (use reshape / dig).
    Created create(HorizonWorld& world, Entity terrain, Entity spline, const Params& p);

    // The lake's water follows its spline now (what the per-frame sync does).
    Result reshape(HorizonWorld& world, Entity spline);
    // "Dig Again": the ground under the lake's current outline. ok = false when the
    // spline is not a lake.
    TerrainSculpt::Result dig(HorizonWorld& world, Entity spline, float depth, float bank);
    bool setLevel(HorizonWorld& world, Entity spline, float level);
    // The water of the lake is gone and so is the link; the spline entity stays.
    bool remove(HorizonWorld& world, Entity spline);

    // A brushed body becomes a lake: a closed spline entity "Lake" (a child of the
    // landscape, control points in its space at the water level) is created through
    // the extracted outline and adopted. Returns the spline, entt::null on failure.
    // The caller takes the undo step first.
    struct Converted
    {
        Entity      spline = entt::null;
        std::string error;
        uint32_t    pieces = 0, islands = 0, points = 0;
    };
    Converted convertBody(HorizonWorld& world, Entity terrain, uint16_t body, const std::string& name = "Lake");

    // The per-frame hook (TerrainSystem::updateTerrains, before the surfaces are
    // rebuilt): every lake whose spline moved since the last call is reshaped. A
    // lake seen for the first time only records the spline's fingerprint, so a
    // load or an undo reshapes nothing. Returns how many lakes were reshaped.
    uint32_t syncSplines(HorizonWorld& world);
}
