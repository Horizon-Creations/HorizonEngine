#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports a JSON material description (.hmat) into a MaterialAsset.
//
//   { "shader": "builtin/unlit", "textures": ["Textures/wood.hasset"] }
//
// Texture paths are asset paths relative to the content root.
class MaterialImporter {
public:
	// Returns the imported asset (already written to disk) or nullptr.
	static std::unique_ptr<MaterialAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir = {});
};
