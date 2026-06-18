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
    static std::vector<std::unique_ptr<AnimationClipAsset>> import(
        const std::filesystem::path& sourcePath);
};
