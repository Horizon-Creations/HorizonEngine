#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports a skinned/skeletal glTF 2.0 mesh (.gltf / .glb) into a SkeletalMeshAsset.
// Parses the first skin found: joint hierarchy, inverse bind matrices,
// JOINTS_0 and WEIGHTS_0 per-vertex attributes (4 influences per vertex).
// Falls back to static import if no skin is present.
class SkeletalMeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals = true;
        float uniformScale    = 1.0f;
        bool  importMaterials = true;
    };

    static std::unique_ptr<SkeletalMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir,
        const ImportSettings&        settings);

    static std::unique_ptr<SkeletalMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir = {})
    { return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
