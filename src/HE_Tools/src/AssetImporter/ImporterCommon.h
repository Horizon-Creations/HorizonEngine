#pragma once
#include <filesystem>
#include <string>
#include "ContentManager/Assets.h"

// Shared plumbing for all asset importers.
//
// Importers fill a RuntimeAsset-derived struct and hand it to writeAsset().
// `asset.path` must be set to the path relative to the content root
// (forward slashes) — the same string the ContentManager later uses to load
// the asset, and the same string other assets use to reference it
// (e.g. MaterialAsset::texturePaths).
namespace Importer
{
	// Writes <contentRoot>/<asset.path> as a .hasset file, creating parent
	// directories as needed. If the target file already exists, its META UUID
	// is reused so scene/asset references survive a re-import.
	bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot);

	// Normalises a relative path to forward slashes (asset reference form).
	std::string toAssetPath(const std::filesystem::path& relativePath);
}
