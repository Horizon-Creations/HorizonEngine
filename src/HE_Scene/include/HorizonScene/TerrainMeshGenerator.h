#pragma once
#include <ContentManager/Assets.h>

struct TerrainComponent;

// CPU-only mesh generator. No GPU, no ContentManager dependency — freely testable.
// Returns a StaticMeshAsset built from the terrain parameters; caller registers it.
StaticMeshAsset generateTerrainMesh(const TerrainComponent& tc);
