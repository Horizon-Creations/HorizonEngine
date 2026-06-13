#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports glTF 2.0 (.gltf / .glb) into a StaticMeshAsset. All primitives of
// all scene nodes are baked into one mesh (the engine has no submesh concept
// yet); node world transforms are applied to the vertex data.
//
// If the glTF references a base-color texture, it is imported alongside the
// mesh and a MaterialAsset referencing it is generated; the mesh's MREF then
// points at that material.
class MeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals    = true;   // when the source has none
        float uniformScale       = 1.0f;
        bool  importMaterials    = true;
    };

    // Returns the imported mesh (already written to disk) or nullptr.
    static std::unique_ptr<StaticMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir,
        const ImportSettings&        settings);

    static std::unique_ptr<StaticMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir = {})
    { return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
