#include "MaterialImporter.h"

std::unique_ptr<MaterialAsset> MaterialImporter::import(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& outputDir)
{
    (void)sourcePath;
    (void)outputDir;
    return nullptr;
}
