#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports a TTF/OTF file into a FontAsset — the raw font bytes are stored so the
// runtime can bake an atlas at the asset's `size`. UI text elements reference the
// resulting .hasset to render with that font.
class FontImporter {
public:
	// `bakeSize` = the pixel size the runtime bakes this font's atlas at.
	static std::unique_ptr<FontAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir = {},
		int bakeSize = 48);
};
