#include "FontImporter.h"
#include <algorithm>
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"
#include <fstream>

std::unique_ptr<FontAsset> FontImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir,
	int bakeSize)
{
	std::ifstream in(sourcePath, std::ios::binary | std::ios::ate);
	if (!in)
	{
		Logger::Log(Logger::LogLevel::Error,
			("FontImporter: cannot open " + sourcePath.string()).c_str());
		return nullptr;
	}
	const std::streamsize n = in.tellg();
	in.seekg(0);
	std::vector<uint8_t> bytes(static_cast<size_t>(std::max<std::streamsize>(0, n)));
	if (n > 0) in.read(reinterpret_cast<char*>(bytes.data()), n);
	if (bytes.empty())
	{
		Logger::Log(Logger::LogLevel::Error,
			("FontImporter: empty font file " + sourcePath.string()).c_str());
		return nullptr;
	}

	auto asset = std::make_unique<FontAsset>();
	asset->type     = HE::AssetType::Font;
	asset->name     = sourcePath.stem().string();
	asset->path     = Importer::toAssetPath(relativeOutputDir / (asset->name + ".hasset"));
	asset->fontData = std::move(bytes);
	asset->size     = bakeSize > 0 ? bakeSize : 48;

	if (!Importer::writeAsset(*asset, contentRoot))
		return nullptr;

	Logger::Log(Logger::LogLevel::Info,
		("FontImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(asset->fontData.size()) + " bytes, bake "
		 + std::to_string(asset->size) + "px)").c_str());
	return asset;
}
