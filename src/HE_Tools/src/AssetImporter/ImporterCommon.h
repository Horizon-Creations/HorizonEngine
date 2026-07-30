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

	// True when the glTF/GLB at `sourcePath` declares at least one skin, i.e. it
	// must be routed to SkeletalMeshImporter — MeshImporter ignores JOINTS_0 /
	// WEIGHTS_0 and would silently produce bind-pose geometry registered as a
	// StaticMesh. Every import entry point (asset_compiler and both editor import
	// paths) asks this one function so they cannot drift apart again.
	// Only the JSON is parsed, no buffers are loaded; anything that fails to parse
	// answers false so the caller falls back to the static path and reports that
	// importer's error instead of a second, redundant one here.
	bool gltfHasSkin(const std::filesystem::path& sourcePath);
}
