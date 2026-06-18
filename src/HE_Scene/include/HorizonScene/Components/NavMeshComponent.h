#pragma once
#include <Math/Math.h>
#include <vector>
#include <memory>
#include <cstdint>

// Forward-declarations so component code doesn't drag in all of Recast/Detour.
struct dtNavMesh;
struct dtNavMeshQuery;

struct NavMeshConfig {
    float cellSize        = 0.3f;
    float cellHeight      = 0.2f;
    float walkableHeight  = 2.0f;
    float walkableClimb   = 0.9f;
    float walkableRadius  = 0.6f;
    float maxSlope        = 45.0f;
    float maxEdgeLen      = 12.0f;
    float maxSimplification = 1.3f;
    float minRegionArea   = 8.0f;
    float mergeRegionArea = 20.0f;
    float detailSampleDist = 6.0f;
    float detailMaxError  = 1.0f;
};

// Input geometry for NavMesh baking.  Vertices are world-space XYZ triples;
// triangles are indices into that array (3 ints per tri, CCW winding).
struct NavMeshGeometry {
    std::vector<float> verts;  // 3 floats per vertex
    std::vector<int>   tris;   // 3 ints per triangle
};

struct NavMeshComponent {
    NavMeshConfig    config;
    NavMeshGeometry  geometry;  // source geometry — set before calling bake

    // Runtime (not serialized): owning pointers to Recast/Detour objects.
    // Null until bake() succeeds.
    std::shared_ptr<dtNavMesh>      navMesh;
    std::shared_ptr<dtNavMeshQuery> navQuery;

    bool isDirty = true; // rebuild needed
};
