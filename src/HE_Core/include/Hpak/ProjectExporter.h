#pragma once
#include <Types/Defines.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

struct HE_API ExportSettings {
    bool    compress = true;
    bool    encrypt  = false;
    // When set, the shipped game scans a Mods/ folder next to the executable and
    // mounts every .hpak there as an overlay on the base pak (same UUID replaces,
    // new UUID adds — including the packed startup scene).
    bool    enableModSupport = false;
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
        const std::string&           startupSceneName, // fallback loose scene filename (e.g. "Main.hescene")
        const std::filesystem::path& outputDir,
        const ExportSettings&        settings = {},
        // Startup scene pre-serialized to binary (CBOR via SceneSerializer::saveToMemory)
        // by the caller (which has HorizonScene). When non-empty it is packed INTO the
        // .hpak under a generated UUID and referenced from project.hcfg, and the loose
        // scene copy is skipped. Empty → fall back to copying the loose startupSceneName.
        const std::vector<uint8_t>&  startupSceneBinary = {}
    );
};
