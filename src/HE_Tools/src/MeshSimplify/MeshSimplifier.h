#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

// Thin wrapper over meshoptimizer's simplifier (vendored, FetchContent, tool-only —
// never linked into HorizonGame). Produces LOD meshes from a source mesh; nothing in
// this file reads or writes a .hasset, and nothing in the importer or editor calls it
// yet (Thema 331, step 2). The vertex buffer meshoptimizer itself works on is left
// unchanged; this class always compacts the result afterwards so an unused vertex
// never survives into the returned Mesh.
class MeshSimplifier {
public:
    struct Vertex {
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec2 uv{0.0f};

        bool operator==(const Vertex&) const = default;
    };

    struct Mesh {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;   // triangle list, size is a multiple of 3
    };

    struct Settings {
        // Target expressed both ways; whichever gives the smaller index count wins,
        // so a caller can bound either the ratio or an absolute triangle budget.
        float  targetRatio  = 0.5f;          // fraction of the input triangle count, (0..1]
        size_t targetTriangleCount = SIZE_MAX; // absolute cap; SIZE_MAX = unused

        float  targetError    = 0.02f;  // relative to mesh extents unless absoluteError
        bool   absoluteError   = false;

        bool   lockMeshBorder  = true;   // meshopt_SimplifyLockBorder: freeze open edges
        bool   pruneComponents = false;  // meshopt_SimplifyPrune: allow whole disconnected
                                          // pieces (e.g. a leaf card) to disappear; implies
                                          // lockMeshBorder is ignored (the two are mutually
                                          // exclusive in meshoptimizer's own option set)

        float  normalWeight = 1.0f;  // attribute weight fed to the attribute-aware pass
        float  uvWeight     = 0.5f;

        // Thin, already-sparse geometry (a single leaf/blade card, a cross-card) must
        // not be reduced below this many triangles by ratio/error alone — see
        // FoliageSettings::minCardTriangleCount for the per-card version of this floor.
        size_t minTriangleCount = 0;
    };

    struct Result {
        Mesh   mesh;
        float  resultError      = 0.0f;  // as reported by meshoptimizer (relative/absolute per Settings::absoluteError)
        bool   hitTargetExactly = false; // false when topology/error stopped it short
    };

    // General-purpose LOD step. UV-seam vertices (same position, different UV — the
    // "UV-Naehte" of the step) are locked in place; normals and UVs feed the
    // attribute-aware error metric so e.g. a hard-shaded edge resists collapsing before
    // a smooth one does. Deterministic: same input and settings always give the same
    // output (meshoptimizer's simplifier has no internal randomness or threading).
    static Result simplify(const Mesh& input, const Settings& settings);

    // Foliage mode: alpha-tested leaf/blade cards. A candidate LOD is only accepted if
    // its alpha-weighted visible area (see computeAlphaWeightedArea) is still at least
    // `minSilhouetteCoverage` of LOD0's — the per-triangle proxy for "Silhouette und
    // sichtbare Blattflaeche je Stufe", not a real rendered silhouette (there is no
    // rasterizer in this tool yet; a candidate that passes the area check can still
    // read as a different shape up close). When it isn't, the target backs off
    // (less aggressive ratio) and retries, up to maxRetries, before returning the best
    // candidate found. lockMeshBorder in `base` is ignored: foliage cards are
    // disconnected quads where every vertex sits on a border, so locking it would
    // freeze the mesh at ~100%; pruneComponents is forced on instead.
    struct FoliageSettings {
        Settings base;
        const uint8_t* alphaMask  = nullptr;  // row-major, 1 byte/texel, 0 = cut, 255 = opaque
        int            alphaWidth  = 0;
        int            alphaHeight = 0;
        float          minSilhouetteCoverage = 0.92f;
        int            maxRetries            = 4;
        // Never prune a mesh down below this many triangles overall, on top of
        // Settings::minTriangleCount (the two are the same floor; this name is the
        // one the step text uses for it — "fuer duenne Karten nicht unter eine
        // Mindestgroesse gehen").
        size_t minCardTriangleCount = 2;
    };

    struct FoliageResult : Result {
        float lod0AlphaArea    = 0.0f;
        float resultAlphaArea  = 0.0f;
        float coverageRatio    = 1.0f;   // resultAlphaArea / lod0AlphaArea
        int   retriesUsed      = 0;
        bool  metCoverageGoal  = true;
    };

    static FoliageResult simplifyFoliage(const Mesh& lod0, const FoliageSettings& settings);

    // Sum over all triangles of (world-space area * alpha sampled at the UV centroid,
    // nearest-neighbor, clamped). The cheap proxy used as the foliage coverage metric;
    // exposed so tests and callers can reason about it directly.
    static float computeAlphaWeightedArea(const Mesh& mesh, const uint8_t* alphaMask, int width, int height);
};
