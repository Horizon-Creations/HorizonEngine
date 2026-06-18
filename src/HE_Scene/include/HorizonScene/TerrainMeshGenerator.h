#pragma once
#include <ContentManager/Assets.h>

struct TerrainComponent;

// CPU-only mesh generator. No GPU, no ContentManager dependency — freely testable.
// Returns a StaticMeshAsset built from the terrain parameters; caller registers it.
StaticMeshAsset generateTerrainMesh(const TerrainComponent& tc);

// Sample terrain height (world-space Y) at a terrain-local (x, z) position.
// localX in [-sizeX/2, sizeX/2], localZ in [-sizeZ/2, sizeZ/2].
// Used by FoliageSystem to place instances on the terrain surface.
float terrainHeightAt(const TerrainComponent& tc, float localX, float localZ);
