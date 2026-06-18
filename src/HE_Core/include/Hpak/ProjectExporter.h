#pragma once
#include <Types/Defines.h>
#include <filesystem>
#include <string>
#include <cstdint>

struct HE_API ExportSettings {
    bool    compress = true;
    bool    encrypt  = false;
    uint8_t key[32]  = {};
    // When non-empty, all files in this directory (the game runtime binaries:
    // executable + dylibs/DLLs) are copied into outputDir so the export is
    // self-contained and runnable on the target machine.
    std::filesystem::path gameRuntimeDir;
};

struct HE_API ExportResult {
    bool        success          = false;
    std::string errorMessage;
    int         assetsPacked     = 0;
    int         binaryFilesCopied = 0; // game exe + dylibs, 0 when gameRuntimeDir empty
};

// Packs a project's content directory into a distributable output folder:
//   • All .hasset files → projectName.hpak (with optional LZ4 + encryption)
//   • The startup .hescene file → copied next to the pak
//   • project.hcfg written describing the pak + startup scene
class HE_API ProjectExporter {
public:
    static ExportResult exportProject(
        const std::filesystem::path& contentDir,
        const std::string&           projectName,
        const std::string&           startupSceneName, // filename only, e.g. "Main.hescene"
        const std::filesystem::path& outputDir,
        const ExportSettings&        settings = {}
    );
};
