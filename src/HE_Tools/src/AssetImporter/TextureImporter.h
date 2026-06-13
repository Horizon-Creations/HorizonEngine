#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"

// Imports PNG/JPG/TGA/BMP/… (anything stb_image reads) into a TextureAsset
// and writes it as <contentRoot>/<relativeOutputDir>/<stem>.hasset.
class TextureImporter {
public:
	struct ImportSettings {
		bool flipVertically = true;  // match GL-style bottom-left UV origin
	};

	// Returns the imported asset (already written to disk) or nullptr.
	static std::unique_ptr<TextureAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir,
		const ImportSettings&        settings);

	static std::unique_ptr<TextureAsset> import(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& contentRoot,
		const std::filesystem::path& relativeOutputDir = {})
	{ return import(sourcePath, contentRoot, relativeOutputDir, ImportSettings{}); }

	// Decode-only variant used by the mesh importer for embedded textures.
	static std::unique_ptr<TextureAsset> decodeFromMemory(
		const void* bytes, size_t size, const ImportSettings& settings);

	static std::unique_ptr<TextureAsset> decodeFromMemory(const void* bytes, size_t size)
	{ return decodeFromMemory(bytes, size, ImportSettings{}); }
};
