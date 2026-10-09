#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── Water on a landscape ────────────────────────────────────────────────────
// The one data model the water brush and the lake tool both edit, and the water
// surface (mesh, shore clipping) reads. Pure CPU, no ContentManager and no
// renderer, so "did that circle land where it was asked" is a question a test can
// put to a TerrainComponent on the stack — the same split TerrainPaint keeps.
//
// WHAT IT IS. A raster over the terrain's 0..1 UV range, like the weightmap and
// the foliage mask, with two values per cell:
//
//   coverage   0..255, how much of the cell is water. 255 inside, 0 outside, in
//              between on the shore: a polygon edge writes the exact share of the
//              cell it covers, a brush writes its falloff. Binary consumers (a
//              mesh that needs "water or not") cut at kWet; one that wants a
//              smooth shoreline interpolates the contour between cells.
//   owner      the id of the Body that cell belongs to, 0 = none.
//
// and a short list of BODIES. A body is one water surface: a level (Y, in the
// space of terrainHeightAt, i.e. terrain-local) and, for a lake, the spline it was
// made from. Level is per body, not per cell: a lake is flat by construction, and
// two bodies at different levels simply stand next to each other.
//
// SOURCE. Body::sourceSpline is the EntityIdComponent::id of the closed spline a
// lake was drawn from, so the lake tool can find the body again and reshape it
// (clearBody + addPolygon, same id). A body painted with the brush has none:
// "quellenlos". The two kinds share everything else, which is what makes them
// interoperable — a brush can extend a lake, an eraser can notch one, and a lake
// polygon can be drawn over painted water. The field stores the UUID and nothing
// else about the spline; prefab instantiation mints fresh entity ids, so a
// duplicated terrain + spline pair has to be re-linked by whoever duplicates it.
//
// IDS ARE STABLE. A body keeps its id for its lifetime, removing one never
// renumbers another, and an id is not handed out again until the counter has
// wrapped the whole uint16 range. Anything that remembers a body (a lake tool, a
// brush stroke) can hold the id across edits and undo.
//
// RESOLUTION. `res` cells per side, its own number (default 256, clamped 1..4096
// like weightRes), NOT the height-field resolution. The height field snaps to
// 2ⁿ+1 and resamples itself (TerrainSculpt::ensureHeights); water must not move
// when that happens, and since the grid lives in UV space it does not. The price:
// a consumer comparing water level against ground height samples the terrain at
// the cell it asks about (terrainHeightAt) instead of reading a vertex.
//
// CHUNKS. The terrain's chunk entities are rebuilt from heights, never saved; the
// water is saved with the terrain and a consumer that builds one surface per
// chunk asks cellRect / chunkCellRect for the cells of its chunk and anyWet to
// skip the empty ones. Edits set Field::dirty and the terrain-local XZ rectangle
// they touched (NOT TerrainComponent::regionDirty — a water edit must not
// regenerate terrain chunks) and bump Field::revision.
//
// SPACE. Everything public takes TERRAIN-LOCAL coordinates, [-sizeX/2, sizeX/2] ×
// [-sizeZ/2, sizeZ/2], like TerrainPaint. A caller with a world position subtracts
// the terrain entity's world position (HE::worldPositionOf), and a caller with a
// spline transforms the SAMPLES of its polyline, not the control points.
namespace HE::water
{
    inline constexpr uint32_t kDefaultRes = 256;
    inline constexpr uint32_t kMaxRes     = 4096;
    // Coverage at or above this counts as water for consumers that need a yes/no.
    inline constexpr uint8_t  kWet        = 128;
    inline constexpr uint16_t kNoBody     = 0;
    // How far above a body's level the ground may stand and still be under its
    // sheet (metres), see Field::shoreOvershoot.
    inline constexpr float    kDefaultShoreOvershoot = 0.05f;
    inline constexpr float    kMaxShoreOvershoot     = 10.0f;

    struct Body
    {
        uint16_t id    = kNoBody;
        float    level = 0.0f;    // terrain-local Y of the surface
        HE::UUID sourceSpline{};  // closed spline a lake came from; zero = brush
        bool     fromSpline() const { return sourceSpline != HE::UUID{}; }
    };

    struct Field
    {
        uint32_t res = kDefaultRes;
        // res² each, or both empty (nothing painted yet). cov == 0 ⇔ owner == 0.
        std::vector<uint8_t>  coverage;
        std::vector<uint16_t> owner;
        std::vector<Body>     bodies;          // creation order
        uint16_t              nextBodyId = 1;  // next id to try; see createBody

        // ── Shore clipping (Thema 174 Schritt 5) ─────────────────────────────
        // The surface mesh stops where the ground comes up through the water: a
        // body's sheet only exists where the terrain stands below its level (plus
        // the overshoot) AND the field says "water". It is one setting for the
        // whole landscape and it holds for brushed and lake water alike, because
        // the clip sits in the mesh builder, not in the cells: the cells are the
        // intent (what the user painted or drew), the sheet is what is left of it
        // after the ground has had its say. A hill that is sculpted into a lake
        // later therefore takes the water off its top without touching the field,
        // and digging it away brings the water back.
        //   clipToGround    false = the sheet is the footprint of the cells at
        //                   the body's level, over rising ground too.
        //   shoreOvershoot  metres of ground ABOVE the level that still count as
        //                   under the water. A little margin keeps the sheet's rim
        //                   under the terrain's own surface instead of a hair
        //                   short of it (the terrain mesh is not the height field
        //                   between its vertices once LODs decimate it), so no
        //                   dry-looking line separates the water from the bank.
        //                   Vertical, so on a gentle bank it moves the rim
        //                   further than on a steep one. 0 = exactly where the
        //                   level meets the ground.
        bool  clipToGround   = true;
        float shoreOvershoot = kDefaultShoreOvershoot;

        // ── Runtime, never serialised ────────────────────────────────────────
        // An edit that changed cells sets `dirty`, widens the terrain-local XZ
        // rectangle it touched and bumps `revision`. The water surface clears
        // `dirty` once it has rebuilt; `revision` is for caches that would
        // rather compare numbers than watch a flag. A loaded field starts dirty
        // over the whole terrain (the surface is rebuilt, never saved).
        bool     dirty = false;
        float    dirtyMinX = 0.0f, dirtyMinZ = 0.0f, dirtyMaxX = 0.0f, dirtyMaxZ = 0.0f;
        uint64_t revision = 0;

        bool allocated() const
        {
            const size_t n = static_cast<size_t>(res) * res;
            return n > 0 && coverage.size() == n && owner.size() == n;
        }
        // Nothing to save: no body, no grid, the default resolution.
        bool pristine() const
        {
            return bodies.empty() && coverage.empty() && owner.empty() && res == kDefaultRes &&
                   clipToGround && shoreOvershoot == kDefaultShoreOvershoot;
        }

        const Body* findBody(uint16_t id) const;
        Body*       findBody(uint16_t id);
        // The lake drawn from `spline`, or null.
        const Body* findBySource(const HE::UUID& spline) const;

        // A new, empty body. Returns its id, or kNoBody when all 65535 ids are
        // taken. The id counter only moves forward, so a body removed a moment
        // ago does not come back under a new meaning.
        uint16_t createBody(float level, const HE::UUID& sourceSpline = {});

        // Cells with coverage >= kWet owned by `id` (any owner for kNoBody).
        uint32_t wetCells(uint16_t id = kNoBody) const;
    };

    // Same bodies (ids, levels, sources, counter), same cells and same shore
    // clipping settings. An unallocated
    // grid equals an allocated all-zero one of the same resolution; the runtime
    // dirty state is ignored.
    bool sameContent(const Field& a, const Field& b);

    // ── Editing ──────────────────────────────────────────────────────────────
    struct Result
    {
        bool     ok      = false;  // false = the arguments could not be applied
        uint32_t changed = 0;      // cells whose coverage or owner actually moved
    };

    // Allocate the grid (all dry) at tc.water.res, clamped to 1..kMaxRes. No-op
    // when a correctly sized grid exists; a wrongly sized one is dropped. False
    // for a terrain with no area.
    bool ensureGrid(TerrainComponent& tc);

    // Change the resolution, keeping the shape: nearest-cell resample of both
    // rasters, so a body never appears or vanishes by resampling alone (except
    // where the new grid is too coarse to hold a sliver). Clamped like ensureGrid.
    void setResolution(TerrainComponent& tc, uint32_t res);

    // Water gone: grid, bodies and counter back to the default.
    void clearAll(TerrainComponent& tc);

    // Dry up every cell of `id` and keep the body, so the lake tool can reshape it
    // (clearBody, then addPolygon) under the same id and source.
    Result clearBody(TerrainComponent& tc, uint16_t id);

    // Dry up every cell of `id` and forget the body.
    Result removeBody(TerrainComponent& tc, uint16_t id);

    // Move a body's surface. The cells do not change, so the dirty rectangle is
    // the whole terrain. False for an unknown body or a non-finite level.
    bool setLevel(TerrainComponent& tc, uint16_t id, float level);

    // Shore clipping on or off and its overshoot (Field::clipToGround,
    // shoreOvershoot), every surface of the landscape rebuilt. The overshoot is
    // clamped to 0..kMaxShoreOvershoot; a non-finite one keeps the old value.
    // Writing the two members directly also works for a surface that exists, but
    // not for a body whose sheet is clipped away completely (it has no entity to
    // notice): this is the call that always reaches it.
    void setShoreClip(TerrainComponent& tc, bool clipToGround, float shoreOvershoot);

    // The GROUND moved: heights were sculpted, generated or the landscape
    // rebuilt. A clipped sheet follows the ground, so the rectangle (terrain-
    // local, `whole` = everything) is marked dirty for the water surface, grown
    // by one terrain cell because the ground is bilinear between its vertices.
    // Does nothing when there is no water or no clipping. Called by TerrainSystem
    // where it regenerates chunks; never sets TerrainComponent::dirty or
    // regionDirty (that direction would loop).
    void noteGroundChanged(TerrainComponent& tc, bool whole,
                           float minX = 0.0f, float minZ = 0.0f, float maxX = 0.0f, float maxZ = 0.0f);

    // Forget bodies that own no cell and have no source: what a brush leaves
    // behind when its water is wiped out. A lake keeps its body — its spline can
    // bring the water back. Returns how many were removed.
    uint32_t pruneEmptyBodies(TerrainComponent& tc);

    // ── Circle with falloff ──────────────────────────────────────────────────
    // The weight of a cell is 1 inside `radius` and falls linearly to 0 across
    // `falloff` outside it (TerrainPaint's), measured to the cell CENTRE;
    // a = clamp(strength · weight, 0, 1).
    //
    // addCircle moves coverage toward 255 by a: cov += (255 − cov) · a, rounded.
    // At strength 1 a cell inside the radius is fully wet at once, one in the
    // falloff gets round(255·a) from dry. It paints `body`'s water and takes
    // over only cells that are DRY; a cell another body already owns keeps its
    // owner and just gets wetter. A brush extends what is there, it never
    // steals. False for an unknown body, a terrain with no area, a zero
    // radius + falloff or a non-finite argument.
    //
    // removeCircle lowers coverage by round(255 · a) and clears the owner at 0.
    // The inverse of addCircle: the same circle at the same strength undoes an
    // add onto dry ground byte for byte. `onlyBody` (not kNoBody) restricts it to
    // that body's cells. Bodies left empty and without a source are pruned.
    Result addCircle(TerrainComponent& tc, uint16_t body, float x, float z,
                     float radius, float falloff, float strength = 1.0f);
    Result removeCircle(TerrainComponent& tc, float x, float z,
                        float radius, float falloff, float strength = 1.0f,
                        uint16_t onlyBody = kNoBody);

    // ── Polygon ──────────────────────────────────────────────────────────────
    // A polygon in terrain-local XZ (x → glm::vec2::x, z → ::y). NO duplicate
    // closing vertex: the polyline of a closed spline ends where it began
    // (Curve::polyline), and polygonFromPolyline drops that last point. Even-odd
    // fill, so any winding and concave shapes work, and a self-crossing outline
    // leaves its overlap dry. Fewer than three vertices, a non-finite one, or no
    // area give an empty result; the part outside the terrain is clipped.
    //
    // Edges are anti-aliased: a cell on the boundary gets the share of it the
    // polygon covers (exact across, eight samples along).
    struct Raster
    {
        int x0 = 0, z0 = 0, w = 0, h = 0;   // cell rectangle, in cells
        std::vector<uint8_t> coverage;      // w·h, row-major; empty = nothing
        bool empty() const { return coverage.empty(); }
    };
    Raster rasterizePolygon(const std::vector<glm::vec2>& polygon,
                            float sizeX, float sizeZ, uint32_t res);

    // The lake: every cell the polygon covers becomes `body`'s, coverage rising
    // to the polygon's (max with what was there). The polygon is authoritative,
    // unlike a brush: a cell it covers at kWet or more is taken from whatever
    // body had it; a shore cell it only grazes is left to its owner.
    // False for an unknown body or an empty polygon.
    Result addPolygon(TerrainComponent& tc, uint16_t body, const std::vector<glm::vec2>& polygon);

    // Water out of the polygon: coverage × (1 − polygon coverage), the owner
    // cleared at 0. `onlyBody` as in removeCircle.
    Result removePolygon(TerrainComponent& tc, const std::vector<glm::vec2>& polygon,
                         uint16_t onlyBody = kNoBody);

    // The polygon a closed polyline encloses: the points projected onto XZ after
    // `toTerrainLocal`, and the closing duplicate (last == first) dropped. The
    // transform is the caller's: spline-local → world → terrain-local.
    std::vector<glm::vec2> polygonFromPolyline(const std::vector<glm::vec3>& polyline,
                                               const glm::mat4& toTerrainLocal = glm::mat4(1.0f));

    // ── Reading ──────────────────────────────────────────────────────────────
    // Coverage 0..1 at (x, z), bilinear between cell centres. 0 off the terrain
    // or without a grid.
    float coverageAt(const TerrainComponent& tc, float x, float z);

    // The body whose water the point is in (nearest cell, coverage >= kWet), or
    // kNoBody.
    uint16_t bodyAt(const TerrainComponent& tc, float x, float z);

    // Surface height at (x, z); false when the point is dry.
    bool levelAt(const TerrainComponent& tc, float x, float z, float& level);

    // Cells in a rectangle, inclusive both ends, clamped to the grid.
    struct CellRect
    {
        int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        bool valid() const { return x1 >= x0 && z1 >= z0; }
    };
    // The cells a terrain-local rectangle touches. Invalid when it misses the
    // grid or the field has none.
    CellRect cellRect(const TerrainComponent& tc, float minX, float minZ, float maxX, float maxZ);
    // The cells of chunk (cx, cz) in a grid of chunksPerSide² chunks, each
    // covering 1/chunksPerSide of the terrain per axis (the TerrainChunkComponent
    // layout). Cells on a chunk border belong to the chunk their centre is in.
    CellRect chunkCellRect(const TerrainComponent& tc, uint32_t chunksPerSide, uint32_t cx, uint32_t cz);
    // Any cell in the rectangle with coverage > 0 (or >= kWet with wetOnly).
    bool anyWater(const TerrainComponent& tc, const CellRect& rect, bool wetOnly = false);
    // Centre of a cell, terrain-local XZ, and its size.
    glm::vec2 cellCenter(const TerrainComponent& tc, int cx, int cz);
    glm::vec2 cellSize(const TerrainComponent& tc);

    // ── Undo ─────────────────────────────────────────────────────────────────
    // The editor's undo is a whole-world snapshot (EditorUndo), and the water is
    // part of that snapshot through the scene serializer, so Ctrl+Z needs nothing
    // from this section. A Delta is the sparse form for a caller that wants "what
    // did this stroke change" without keeping the world: only the cells that
    // moved (with both values) and the body lists.
    struct CellChange
    {
        uint32_t index = 0;                       // z·res + x
        uint16_t ownerBefore = 0, ownerAfter = 0;
        uint8_t  covBefore = 0,   covAfter = 0;
    };
    struct Delta
    {
        uint32_t res = 0;
        std::vector<CellChange> cells;
        std::vector<Body> bodiesBefore, bodiesAfter;
        uint16_t nextIdBefore = 1, nextIdAfter = 1;
        // Same bodies, same id counter before and after.
        bool sameBodies() const;
        bool empty() const { return cells.empty() && sameBodies(); }
    };
    // before → after. False (out untouched) when the resolutions differ: a
    // resample moves every cell and is not a stroke, keep a snapshot for it.
    bool makeDelta(const Field& before, const Field& after, Delta& out);
    // Replay / take back, marking the cells (or, when a body changed, the whole
    // terrain) dirty. False (nothing touched) when the resolution differs or a
    // cell index lies outside the grid.
    bool applyDelta(TerrainComponent& tc, const Delta& d);
    bool revertDelta(TerrainComponent& tc, const Delta& d);

    // ── Persistence (SceneSerializer) ────────────────────────────────────────
    // The cells as a run-length stream: per run a varint count, the owner as 2
    // bytes little-endian, the coverage byte. A mostly dry grid is a handful of
    // bytes, a lake interior a single run. Empty when no cell is wet.
    std::vector<uint8_t> encodeCells(const Field& f);
    // The inverse, into `f` at its current `res`. False (cells left empty) for a
    // stream that is truncated, overruns the grid or does not fill it exactly.
    bool decodeCells(Field& f, const uint8_t* data, size_t size);
    // Make a freshly loaded field safe: resolution clamped, a mis-sized grid
    // dropped, bodies with id 0 / a duplicate id / a non-finite level removed,
    // cells owned by an unknown body dried, cov == 0 ⇔ owner == 0 restored, the
    // id counter moved past every id, and the field marked dirty over `tc`'s
    // whole area. Never leaves a state later code can index out of bounds with.
    void sanitize(TerrainComponent& tc);
}
