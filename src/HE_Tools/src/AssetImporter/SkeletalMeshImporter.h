#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"
#include "ImporterCommon.h"   // Importer::OutputTargets

// Imports a skinned/skeletal glTF 2.0 mesh (.gltf / .glb) into a SkeletalMeshAsset.
// Parses the first skin found: joint hierarchy, inverse bind matrices,
// JOINTS_0 and WEIGHTS_0 per-vertex attributes (4 influences per vertex).
// Falls back to static import if no skin is present.
//
// If the glTF references a base-color texture, it is imported alongside the mesh
// and a MaterialAsset referencing it is generated; the mesh's MREF then points at
// that material — same as MeshImporter.
class SkeletalMeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals = true;
        float uniformScale    = 1.0f;
        bool  importMaterials = true;
    };

    // `outputs` pins the mesh and its two sidecars onto files that already exist
    // (a re-import of assets the user renamed); empty fields are named after the
    // source, which is what a first import does. Note the primary output is
    // "<stem>_skeletal.hasset", so outputs.asset is a whole path, not a stem.
    static std::unique_ptr<SkeletalMeshAsset> import(
        const std::filesystem::path&   sourcePath,
        const std::filesystem::path&   contentRoot,
        const std::filesystem::path&   relativeOutputDir,
        const ImportSettings&          settings,
        const Importer::OutputTargets& outputs = {});

    static std::unique_ptr<SkeletalMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir = {})
    { return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
