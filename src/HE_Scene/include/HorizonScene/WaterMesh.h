#pragma once
#include <ContentManager/Assets.h>
#include <HorizonScene/WaterField.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <vector>

struct TerrainComponent;

// ─── The water surface as geometry ───────────────────────────────────────────
// Pure CPU, no ContentManager, no renderer, no entity: a TerrainComponent on the
// stack in, a StaticMeshAsset out, which is what lets a test ask "is the area of
// the mesh the area of the lake" without a window. WaterSurface.h is the part that
// puts the result into a world.
//
// FROM THE FIELD TO A MESH, in four steps (all per BODY: a body is one surface at
// one level, and its mesh is one draw):
//
//   1. LATTICE.  The field stores coverage per CELL; a contour wants values at the
//      CORNERS between cells. buildLattice averages the four cells around each
//      corner (a cell outside the terrain counts as its nearest edge cell, so water
//      reaches the terrain's border) and keeps only the body's own coverage. For an
//      edge that runs along the grid this puts the contour exactly where the
//      polygon's edge was: a cell half covered by an axis-aligned edge yields a
//      crossing in the middle of that cell.
//   2. CONTOURS. Marching squares at kContourIso over the lattice, with the crossing placed
//      by linear interpolation along each lattice edge. Every crossing belongs to
//      exactly one lattice edge, so the segments of neighbouring cells meet bit for
//      bit and every contour closes. Direction is fixed: water on the left, so an
//      outer boundary runs counter-clockwise (positive signedArea, XZ with x right
//      and z up) and the boundary of an island, a hole, clockwise. Ambiguous cells
//      (two opposite corners wet) are decided by the mean of the four corners.
//   3. POLYGONS. Each hole goes to the smallest outer boundary that contains it.
//   4. TRIANGLES. Ear clipping with hole bridging (`triangulate`), so a concave
//      shore, an island and two separate bays need nothing special. The mesh
//      therefore has about one triangle per contour vertex, not one per cell.
//
// SHORE CLIPPING (Thema 174 Schritt 5). With `clipToGround` the sheet exists only
// where the terrain stands below the body's level. The seam is step 1: next to the
// coverage the lattice carries a second array `g`, the depth of the water above the
// ground at each corner (level + overshoot − ground height, metres; ≥ 0 = the ground
// is under the sheet), and step 2 cuts BOTH: a corner is inside when its coverage is
// at the iso value AND its depth is not negative. Where an edge of the lattice runs
// from inside to outside the crossing sits at whichever of the two limits is met
// first, each placed by linear interpolation — on a planar bank the contour is
// exactly where the water meets the ground, on a curved one it is the chord across
// the lattice cell (the shore is only as fine as the water grid: a larger landscape
// wants a higher Field::res). Steps 3 and
// 4 do not know about the ground at all: a bank, an island the ground raises out of
// a lake and a pool in a hollow are contours, outer boundaries and holes, like any
// other. The same cells give the same sheet whether a brush or a lake tool drew them.
//
// SPACE. Contours are terrain-local XZ like every public WaterField function, in
// double so a long straight shore stays collinear to the last bit. The mesh is in
// the space of the entity that carries it: vertices relative to `Surface::center`
// (the middle of the body's box), y = 0, so the entity sits at (center.x, level,
// center.y) under the terrain.
namespace HE::water
{
    // The coverage at which the shore runs: exactly half. A straight edge that lies
    // on the grid gives corner values of exactly 127.5, so the contour then passes
    // through them and the polygon's edge is where it was drawn (kWet, 128, is the
    // "is this CELL water" cut and a different question).
    inline constexpr double kContourIso = 127.5;

    // The ground's say in the sheet (`Lattice::g`), per body.
    struct ShoreClip
    {
        bool  enabled  = false;
        float level    = 0.0f;                      // the body's level
        float overshoot = kDefaultShoreOvershoot;   // metres of ground above it that still count as under water
    };

    using Ring = std::vector<glm::dvec2>;

    // Shoelace area. > 0 when the ring runs counter-clockwise in (x, z) with z up
    // — the orientation of an outer boundary here; < 0 for a hole.
    double signedArea(const Ring& ring);

    // ── Triangulation ────────────────────────────────────────────────────────
    // rings[0] is the outer boundary, rings[1..] are holes inside it (disjoint
    // from each other and from the outer ring; touching is tolerated). No closing
    // duplicate vertex, any winding. The result is a list of triangles as indices
    // into the rings laid end to end (ring 0 first), each triangle counter-
    // clockwise in (x, z) — which is a downward normal; flip when you want +Y.
    //
    // For a simple polygon of n vertices with h holes the triangles cover it
    // exactly: n + 2h − 2 of them, areas summing to the polygon's area, none
    // overlapping. Degenerate input (fewer than three vertices, a non-finite
    // coordinate) gives an empty list; a self-intersecting ring gives *a*
    // triangulation without the exactness guarantee.
    std::vector<uint32_t> triangulate(const std::vector<Ring>& rings);

    // ── Lattice and contours ─────────────────────────────────────────────────
    // Coverage 0..255 at the corners of the cells: corner (i, j) is the lower
    // corner of cell (i, j), at terrain-local (−size/2 + i·cellW, −size/2 + j·cellH).
    // Covers the body's cell box plus a margin of dry corners all round, so every
    // contour closes inside it; the margin corners outside the terrain are 0.
    struct Lattice
    {
        int x0 = 0, z0 = 0;                  // global index of the first corner (may be −1)
        int w = 0, h = 0;                    // corner count per axis
        std::vector<float> v;                // w·h, row-major, 0..255
        // The ground (empty = no clipping): w·h, row-major, METRES of water above
        // the ground at the corner, level + overshoot − terrainHeightAt. A corner
        // only counts as water when this is ≥ 0. Filled where it can matter (a
        // corner with water next to it); everywhere else a large positive value.
        // A non-finite ground height reads as far above the water.
        std::vector<float> g;
        bool  clipped() const { return !g.empty(); }
        float groundAt(int i, int j) const { return g[static_cast<size_t>(j) * w + i]; }
        double originX = 0.0, originZ = 0.0; // position of corner (0, 0)
        double cellW = 1.0, cellH = 1.0;
        double minX = 0.0, maxX = 0.0, minZ = 0.0, maxZ = 0.0;   // the terrain
        float at(int i, int j) const { return v[static_cast<size_t>(j) * w + i]; }
        glm::dvec2 position(int i, int j) const
        {
            return { originX + (x0 + i) * cellW, originZ + (z0 + j) * cellH };
        }
        bool empty() const { return v.empty(); }
    };

    // The cells of the body (coverage > 0, owner == body), as one box. Invalid when
    // the body has none.
    CellRect bodyCells(const TerrainComponent& tc, uint16_t body);
    // Every body's box in one pass over the grid, in the order of Field::bodies;
    // an entry is invalid for a body with no water.
    std::vector<CellRect> allBodyCells(const TerrainComponent& tc);

    // False when the field has no grid, the box is invalid or the terrain has no
    // area. `clip` enabled fills `Lattice::g` from the terrain's height field
    // (terrainHeightAt; the terrain is only read, never changed).
    bool buildLattice(const TerrainComponent& tc, uint16_t body, const CellRect& cells, Lattice& out,
                      const ShoreClip& clip = {});

    struct Contour
    {
        Ring   points;                       // terrain-local XZ, clamped to the terrain
        double area = 0.0;                   // signed; > 0 outer boundary, < 0 hole
        bool   hole() const { return area < 0.0; }
    };
    // Closed contours at `iso` (and, on a clipped lattice, at depth 0 of `g`), in a
    // deterministic order (by where they start on the lattice). Loops smaller than a fiftieth of a cell are dropped; consecutive
    // duplicates and collinear points are merged. A convex corner of a grid-aligned
    // shape is cut diagonally across its cell (half a cell of area) — the price of
    // a lattice contour, invisible on a shore that is not a rectangle.
    std::vector<Contour> extractContours(const Lattice& lattice, double iso = kContourIso);

    // An outer boundary with the islands inside it.
    struct Polygon
    {
        Ring outer;
        std::vector<Ring> holes;
    };
    std::vector<Polygon> groupContours(const std::vector<Contour>& contours);

    // ── The mesh ─────────────────────────────────────────────────────────────
    struct SurfaceOptions
    {
        // World XZ of the terrain's local origin. UVs are WORLD coordinates, so
        // two bodies (and two terrains) side by side tile one texture seamlessly:
        // uv = (terrain-local xz + uvOrigin) / uvMetersPerTile.
        glm::vec2 uvOrigin{ 0.0f, 0.0f };
        float     uvMetersPerTile = 1.0f;
        // Shore clipping against the ground. Off here, on in the field
        // (Field::clipToGround): WaterSurface passes the field's settings, a caller
        // that wants only the footprint of the cells gets exactly that.
        bool      clipToGround = false;
        float     shoreOvershoot = kDefaultShoreOvershoot;
        ShoreClip clipFor(float level) const { return { clipToGround, level, shoreOvershoot }; }
    };

    struct Surface
    {
        StaticMeshAsset mesh;                // SoA, normal +Y everywhere, y = 0
        glm::vec2 center{ 0.0f, 0.0f };      // terrain-local XZ the vertices are relative to
        float     level = 0.0f;              // the body's level when built
        CellRect  cells;                     // the body's cell box when built
        uint32_t  contours = 0;              // closed contours found (outer boundaries + holes)
        uint32_t  polygons = 0;              // outer boundaries
        uint32_t  triangles = 0;
        uint64_t  hash = 0;                  // of everything the mesh was built from
        bool empty() const { return mesh.indices.empty(); }
    };

    // Hash of the lattice (ground included) and the options: equal hash, same mesh. Lets the caller
    // skip a rebuild when an edit near a body did not change it.
    uint64_t surfaceHash(const Lattice& lattice, float level, const SurfaceOptions& opt);

    // Lattice → contours → polygons → triangles → mesh. `surface.empty()` when the
    // body has no wet area (even though the call succeeded). False for an unknown
    // body or a field without a grid.
    bool buildSurface(const TerrainComponent& tc, uint16_t body, const SurfaceOptions& opt, Surface& out);
    // The same from a lattice the caller already has (and the box and level it came with).
    bool buildSurface(const TerrainComponent& tc, const Lattice& lattice, const CellRect& cells,
                      float level, const SurfaceOptions& opt, Surface& out);
}
