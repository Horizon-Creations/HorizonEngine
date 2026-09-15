#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"
#include "ImporterCommon.h"   // Importer::OutputTargets

// Imports glTF 2.0 (.gltf / .glb) — and, with HE_HAVE_ASSIMP, FBX / OBJ /
// COLLADA (.fbx / .obj / .dae, see AssimpMeshImport) — into a StaticMeshAsset.
// All primitives of all scene nodes are baked into one vertex/index buffer with
// node world transforms applied; primitives sharing a source material become
// one material SECTION (MeshSection, chunk MSEC), so a multi-material file
// keeps every material on the geometry it was authored on.
//
// Every source material — glTF's and, through AssimpMaterialImport, the Assimp
// formats' — is imported as a MaterialAsset (with its textures) and bound to its
// section; the mesh's MREF points at section 0's material.
class MeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals    = true;   // when the source has none
        float uniformScale       = 1.0f;
        bool  importMaterials    = true;
    };

    // Returns the imported mesh (already written to disk) or nullptr.
    // `outputs` pins the mesh and its two sidecars onto files that already exist
    // (a re-import of assets the user renamed); empty fields are named after the
    // source, which is what a first import does.
    static std::unique_ptr<StaticMeshAsset> import(
        const std::filesystem::path&   sourcePath,
        const std::filesystem::path&   contentRoot,
        const std::filesystem::path&   relativeOutputDir,
        const ImportSettings&          settings,
        const Importer::OutputTargets& outputs = {});

    static std::unique_ptr<StaticMeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir = {})
    { return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }
};
