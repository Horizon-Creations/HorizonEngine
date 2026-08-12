#include "FontImporter.h"
#include <algorithm>
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"
#include <fstream>

std::unique_ptr<FontAsset> FontImporter::import(
	const std::filesystem::path&   sourcePath,
	const std::filesystem::path&   contentRoot,
	const std::filesystem::path&   relativeOutputDir,
	int                            bakeSize,
	const Importer::OutputTargets& outputs)
{
	std::ifstream in(sourcePath, std::ios::binary | std::ios::ate);
	if (!in)
	{
		HE_LOG_ERROR(Tool, "%s",
			("FontImporter: cannot open " + sourcePath.string()).c_str());
		return nullptr;
	}
	const std::streamsize n = in.tellg();
	in.seekg(0);
	std::vector<uint8_t> bytes(static_cast<size_t>(std::max<std::streamsize>(0, n)));
	if (n > 0) in.read(reinterpret_cast<char*>(bytes.data()), n);
	if (bytes.empty())
	{
		HE_LOG_ERROR(Tool, "%s",
			("FontImporter: empty font file " + sourcePath.string()).c_str());
		return nullptr;
	}

	const auto out = Importer::resolveOutput(outputs.asset, relativeOutputDir,
	                                         sourcePath.stem().string());

	auto asset = std::make_unique<FontAsset>();
	asset->type     = HE::AssetType::Font;
	asset->name     = out.name;
	asset->path     = out.path;
	asset->fontData = std::move(bytes);
	asset->size     = bakeSize > 0 ? bakeSize : 48;

	if (!Importer::writeAsset(*asset, contentRoot, sourcePath))
		return nullptr;

	HE_LOG_INFO(Tool, "%s",
		("FontImporter: " + sourcePath.filename().string() + " -> " + asset->path
		 + " (" + std::to_string(asset->fontData.size()) + " bytes, bake "
		 + std::to_string(asset->size) + "px)").c_str());
	return asset;
}
