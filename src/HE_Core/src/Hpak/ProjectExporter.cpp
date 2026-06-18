#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakFormat.h>
#include <filesystem>
#include <cstring>

ExportResult ProjectExporter::exportProject(
    const std::filesystem::path& contentDir,
    const std::string&           projectName,
    const std::string&           startupSceneName,
    const std::filesystem::path& outputDir,
    const ExportSettings&        settings)
{
    std::error_code ec;

    std::filesystem::create_directories(outputDir, ec);
    if (ec) return {false, "Cannot create output dir: " + ec.message(), 0};

    Hpak::PackSettings packSettings;
    packSettings.compress = settings.compress;
    packSettings.encrypt  = settings.encrypt;
    if (settings.encrypt) std::memcpy(packSettings.key, settings.key, 32);

    HpakWriter packer;
    const int added = packer.addDirectory(contentDir, packSettings);

    const std::string hpakFilename = projectName + ".hpak";
    if (!packer.write((outputDir / hpakFilename).string()))
        return {false, "Failed to write " + hpakFilename, 0};

    // Copy startup scene (non-fatal if missing)
    std::string sceneFile;
    if (!startupSceneName.empty())
    {
        const auto sceneSrc = contentDir / startupSceneName;
        sceneFile = std::filesystem::path(startupSceneName).filename().string();
        if (std::filesystem::exists(sceneSrc, ec))
            std::filesystem::copy_file(sceneSrc, outputDir / sceneFile,
                std::filesystem::copy_options::overwrite_existing, ec);
    }

    ProjectConfig cfg;
    cfg.projectName   = projectName;
    cfg.hpakFilename  = hpakFilename;
    cfg.mainSceneName = sceneFile;
    std::memset(cfg.projectUuidBytes, 0, 16);

    if (!ProjectConfigLoader::save(outputDir, cfg))
        return {false, "Failed to write project.hcfg", 0};

    // Copy game runtime binaries (executable + dylibs) so the export is runnable
    int binaryCopied = 0;
    if (!settings.gameRuntimeDir.empty() && std::filesystem::exists(settings.gameRuntimeDir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(settings.gameRuntimeDir, ec))
        {
            if (!entry.is_regular_file()) continue;
            std::filesystem::copy_file(entry.path(), outputDir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) ++binaryCopied;
        }
    }

    return {true, "", added, binaryCopied};
}
