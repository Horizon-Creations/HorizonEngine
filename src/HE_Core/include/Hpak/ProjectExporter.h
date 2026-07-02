#pragma once
#include <Types/Defines.h>
#include <filesystem>
#include <functional>
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
    // Glob patterns (relative to contentDir, forward slashes) for assets to skip
    // when packing — e.g. "Debug/*", "*_test.hasset". See Hpak::PackSettings.
    std::vector<std::string> excludePatterns;
    // Incremental packing: reuse the stored (compressed + encrypted) bytes of
    // unchanged assets from the previous export at the same outputDir, keyed by
    // the hash of each asset's rewritten blob (persisted in a
    // <name>.hpak.manifest sidecar). Falls back to a full pack whenever the
    // previous pak/manifest is missing, mismatched, or the pack settings
    // (codec/level/encrypt/key) changed. With encrypt, the previous export's
    // key is reused (read from its project.hcfg) so entries stay verbatim.
    bool incremental = true;
    // Optional progress callback: (assetsDone, assetsTotal, currentFile). Invoked
    // from whatever thread runs exportProject — the caller must make it
    // thread-safe when exporting on a worker thread.
    std::function<void(int, int, const std::string&)> progress;
};

struct HE_API ExportResult {
    bool        success          = false;
    std::string errorMessage;
    int         assetsPacked     = 0;
    int         binaryFilesCopied = 0; // game exe + dylibs, 0 when gameRuntimeDir empty
    int         assetsReused     = 0;  // carried verbatim from the previous pak (incremental)
};

// ─── Export target platforms ──────────────────────────────────────────────────
// The pak + project.hcfg are platform-neutral; the target only decides which
// game-runtime binaries ship and (in the editor) the output sub-folder. Host =
// the platform the editor is running on, served from <base>/../Game as always;
// cross-targets are served from <base>/../GameRuntimes/<Name>/ — a prebuilt
// runtime bundle the user drops there (built on that platform / CI).
enum class ExportPlatform : uint8_t { Host = 0, Windows, MacOS, Linux };

HE_API const char*    exportPlatformName(ExportPlatform p);   // "Host"/"Windows"/"macOS"/"Linux"
HE_API ExportPlatform exportPlatformFromName(const std::string& name); // unknown → Host
HE_API std::filesystem::path resolveRuntimeDir(const std::filesystem::path& editorBaseDir,
                                               ExportPlatform p);

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
