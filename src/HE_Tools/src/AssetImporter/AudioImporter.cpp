#include "AudioImporter.h"

std::unique_ptr<AudioAsset> AudioImporter::import(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& outputDir,
    const ImportSettings& settings)
{
    (void)sourcePath;
    (void)outputDir;
    (void)settings;
    return nullptr;
}
