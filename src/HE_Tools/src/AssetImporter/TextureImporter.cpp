#include "TextureImporter.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

// STB_IMAGE_STATIC keeps the symbols internal — the editor links its own
// stb_image implementation (stb_image_impl.cpp) and both end up in one binary.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

namespace
{
	std::unique_ptr<TextureAsset> fromPixels(stbi_uc* pixels, int w, int h)
	{
		if (!pixels)
			return nullptr;

		auto asset = std::make_unique<TextureAsset>();
		asset->type     = HE::AssetType::Texture;
		asset->width    = static_cast<size_t>(w);
		asset->height   = static_cast<size_t>(h);
		asset->channels = 4;
		asset->data.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
		stbi_image_free(pixels);
		return asset;
	}
}

std::unique_ptr<TextureAsset> TextureImporter::decodeFromMemory(
	const void* bytes, size_t size, const ImportSettings& settings)
{
	stbi_set_flip_vertically_on_load(settings.flipVertically ? 1 : 0);
	int w = 0, h = 0, comp = 0;
	stbi_uc* pixels = stbi_load_from_memory(
		static_cast<const stbi_uc*>(bytes), static_cast<int>(size),
		&w, &h, &comp, STBI_rgb_alpha);
	return fromPixels(pixels, w, h);
}

std::unique_ptr<TextureAsset> TextureImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir,
	const ImportSettings&        settings)
{
	stbi_set_flip_vertically_on_load(settings.flipVertically ? 1 : 0);
	int w = 0, h = 0, comp = 0;
	stbi_uc* pixels = stbi_load(sourcePath.string().c_str(), &w, &h, &comp, STBI_rgb_alpha);

	auto asset = fromPixels(pixels, w, h);
	if (!asset)
	{
		Logger::Log(Logger::LogLevel::Error,
			("TextureImporter: " + sourcePath.string() + ": " + stbi_failure_reason()).c_str());
		return nullptr;
	}

	asset->name = sourcePath.stem().string();
	asset->path = Importer::toAssetPath(relativeOutputDir / (asset->name + ".hasset"));

	if (!Importer::writeAsset(*asset, contentRoot))
		return nullptr;

	Logger::Log(Logger::LogLevel::Info,
		("TextureImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(w) + "x" + std::to_string(h) + ")").c_str());
	return asset;
}
