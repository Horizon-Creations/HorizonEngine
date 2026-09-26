#pragma once
#include <filesystem>
#include <memory>
#include "ContentManager/Assets.h"
#include "ImporterCommon.h"   // Importer::OutputTargets

// Imports PNG/JPG/TGA/BMP/… (anything stb_image reads) into a TextureAsset
// and writes it as <contentRoot>/<relativeOutputDir>/<stem>.hasset — or onto the
// file `outputs.asset` names, when a re-import has one to hit.
class TextureImporter {
public:
	struct ImportSettings {
		bool flipVertically = true;  // match GL-style bottom-left UV origin
		// Colour data (base colour, emissive) vs. data (normal, ORM, masks). Recorded
		// on the asset as TextureAsset::srgb, kept through the pack cook, and picked
		// up by all five backends (GL, Metal, D3D11, D3D12, Vulkan) as the sRGB
		// pixel format (hardware decode to linear on sample).
		// This default is only the struct's: the policy lives with the callers. The
		// glTF importer knows per slot; Importer::importSource (editor import,
		// asset_compiler) takes the user's choice or Importer::suggestTextureSrgb,
		// and a reimport keeps what the asset already carries.
		bool srgb = false;
	};

	// Returns the imported asset (already written to disk) or nullptr.
	static std::unique_ptr<TextureAsset> import(
		const std::filesystem::path&   sourcePath,
		const std::filesystem::path&   contentRoot,
		const std::filesystem::path&   relativeOutputDir,
		const ImportSettings&          settings,
		const Importer::OutputTargets& outputs = {});

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
