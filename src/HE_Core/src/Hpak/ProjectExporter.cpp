#include <Hpak/ProjectExporter.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/Aes256Gcm.h>
#include <Types/UUID.h>
#include <filesystem>
#include <cstring>

ExportResult ProjectExporter::exportProject(
    const std::filesystem::path& contentDir,
    const std::string&           projectName,
    const std::string&           startupSceneName,
    const std::filesystem::path& outputDir,
    const ExportSettings&        settings,
    const std::vector<uint8_t>&  startupSceneBinary)
{
    std::error_code ec;

    std::filesystem::create_directories(outputDir, ec);
    if (ec) return {false, "Cannot create output dir: " + ec.message(), 0};

    Hpak::PackSettings packSettings;
    // Map the export "compress" toggle to the best codec available at build time
    // (zstd preferred for ship builds, LZ4 otherwise). Store when off.
    if (settings.compress)
    {
#if defined(HE_HAVE_ZSTD)
        packSettings.codec = Hpak::Codec::Zstd;
#elif defined(HE_HAVE_LZ4)
        packSettings.codec = Hpak::Codec::LZ4;
#else
        packSettings.codec = Hpak::Codec::Store;
#endif
    }
    packSettings.encrypt = settings.encrypt;
    if (settings.encrypt)
    {
        // Ship a fresh random 256-bit key in project.hcfg (deriving from a
        // passphrase buys nothing when the key ships with the game anyway).
        if (!Hpak::randomBytes(packSettings.key, 32))
            return {false, "Crypto backend unavailable — cannot encrypt", 0};
    }

    HpakWriter packer;
    const int added = packer.addDirectory(contentDir, packSettings);

    // Pack the startup scene as a binary entry INTO the pak (if the caller
    // serialized one), under a fresh UUID recorded in the hcfg. Same codec +
    // encryption as the assets. Must happen before write().
    HE::UUID sceneUuid{};
    if (!startupSceneBinary.empty())
    {
        sceneUuid = HE::UUID::generate();
        packer.addEntry(sceneUuid, startupSceneBinary, packSettings);
    }

    const std::string hpakFilename = projectName + ".hpak";
    if (!packer.write((outputDir / hpakFilename).string()))
        return {false, "Failed to write " + hpakFilename, 0};

    // Loose startup-scene fallback: only when no binary scene was packed.
    std::string sceneFile;
    if (startupSceneBinary.empty() && !startupSceneName.empty())
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
    cfg.encrypted = settings.encrypt;
    if (settings.encrypt) std::memcpy(cfg.encKey, packSettings.key, 32);
    if (!startupSceneBinary.empty())
    {
        cfg.hasPackedScene = true;
        std::memcpy(cfg.startupSceneUuid,      &sceneUuid.hi, 8);
        std::memcpy(cfg.startupSceneUuid + 8,  &sceneUuid.lo, 8);
    }

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
