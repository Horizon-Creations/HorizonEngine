#pragma once
#include <filesystem>
#include <memory>

// Imports any Assimp-supported format (FBX, OBJ, GLTF, …) into a MeshAsset.
// Outputs a .hasset file alongside the source file.
class MeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals    = true;
        bool  generateTangents   = true;
        bool  mergeSubMeshes     = false;
        float uniformScale       = 1.0f;
    };

};
