#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"
#include "ImporterCommon.h"   // Importer::OutputTargets

// Imports a JSON material description (.hmat) into a MaterialAsset.
//
//   { "shader": "builtin/unlit", "textures": ["Textures/wood.hasset"] }
//
// Texture paths are asset paths relative to the content root.
class MaterialImporter {
public:
	// Returns the imported asset (already written to disk) or nullptr.
	// `outputs.asset` pins the output onto a file that already exists (a re-import
	// of an asset the user renamed); empty means "named after the source".
	static std::unique_ptr<MaterialAsset> import(
		const std::filesystem::path&   sourcePath,
		const std::filesystem::path&   contentRoot,
		const std::filesystem::path&   relativeOutputDir = {},
		const Importer::OutputTargets& outputs = {});
};
