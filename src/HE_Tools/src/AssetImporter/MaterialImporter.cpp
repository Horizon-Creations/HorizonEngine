#include "MaterialImporter.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"
#include <nlohmann/json.hpp>
#include <fstream>

std::unique_ptr<MaterialAsset> MaterialImporter::import(
	const std::filesystem::path& sourcePath,
	const std::filesystem::path& contentRoot,
	const std::filesystem::path& relativeOutputDir)
{
	std::ifstream f(sourcePath);
	if (!f.is_open())
	{
		Logger::Log(Logger::LogLevel::Error,
			("MaterialImporter: cannot open " + sourcePath.string()).c_str());
		return nullptr;
	}

	nlohmann::json j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded() || !j.is_object())
	{
		Logger::Log(Logger::LogLevel::Error,
			("MaterialImporter: invalid JSON in " + sourcePath.string()).c_str());
		return nullptr;
	}

	auto asset = std::make_unique<MaterialAsset>();
	asset->type       = HE::AssetType::Material;
	asset->name       = sourcePath.stem().string();
	asset->path       = Importer::toAssetPath(relativeOutputDir / (asset->name + ".hasset"));
	asset->shaderPath = j.value("shader", std::string{"builtin/unlit"});
	for (const auto& t : j.value("textures", nlohmann::json::array()))
		if (t.is_string())
			asset->texturePaths.push_back(t.get<std::string>());

	// Optional PBR scalars (metallic-roughness). Default to the asset's own.
	if (auto bc = j.find("baseColor"); bc != j.end() && bc->is_array() && bc->size() >= 3)
		for (int i = 0; i < 3; ++i)
			asset->baseColor[i] = (*bc)[i].get<float>();
	asset->metallic  = j.value("metallic",  asset->metallic);
	asset->roughness = j.value("roughness", asset->roughness);
	asset->opacity   = j.value("opacity",   asset->opacity);

	if (!Importer::writeAsset(*asset, contentRoot))
		return nullptr;

	Logger::Log(Logger::LogLevel::Info,
		("MaterialImporter: " + sourcePath.filename().string() + " -> " + asset->path).c_str());
	return asset;
}
