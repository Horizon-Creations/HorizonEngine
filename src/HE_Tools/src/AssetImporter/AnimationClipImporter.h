#pragma once
#include <filesystem>
#include <memory>
#include <vector>
#include "ContentManager/Assets.h"

// Imports all animations embedded in a glTF 2.0 file (.gltf / .glb).
// One AnimationClipAsset is returned per cgltf_animation; channel joint indices
// match the skin.joints[] order from the first skin found in the file.
class AnimationClipImporter {
public:
    // Parse only — nothing is written to disk.
    static std::vector<std::unique_ptr<AnimationClipAsset>> import(
        const std::filesystem::path& sourcePath);

    struct WriteResult {
        int written = 0;  // clips successfully written as .hasset
        int failed  = 0;  // clips that parsed but could not be written
    };

    // Parses and writes one .hasset per animation into
    // <contentRoot>/<relativeOutputDir>, named "<gltf stem>_<clip>.hasset".
    // The name is derived deterministically from the source so a re-import lands
    // on the same file and Importer::writeAsset can reuse its UUID — scene and
    // state-machine references to the clip survive.
    static WriteResult importAndWrite(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& contentRoot,
        const std::filesystem::path& relativeOutputDir = {});
};
