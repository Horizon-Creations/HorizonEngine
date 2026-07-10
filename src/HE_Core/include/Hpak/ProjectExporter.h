#pragma once
#include <Types/Defines.h>
#include <Types/UUID.h>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
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
    // Transcode RGBA8 textures to ASTC 4x4 at pack time (needs an ASTC encoder in
    // the build). Only meaningful for targets whose GPU samples ASTC (Apple-
    // Silicon Metal); the editor sets it accordingly.
    bool astcTextures = false;
    // macOS only: emit a <projectName>.app bundle instead of a flat folder —
    // executable + engine dylibs in Contents/MacOS, pak/hcfg/GameLogic in
    // Contents/Resources (where SDL_GetBasePath resolves inside a bundle), a
    // generated Info.plist, and an ad-hoc codesign of the whole bundle. Only
    // honoured when the runtime binaries are macOS binaries; ignored otherwise.
    bool appBundle = false;
    // Optional progress callback: (assetsDone, assetsTotal, currentFile). Invoked
    // from whatever thread runs exportProject — the caller must make it
    // thread-safe when exporting on a worker thread.
    std::function<void(int, int, const std::string&)> progress;
    // Precompile node-graph material shaders into the pak for these graphics backends
    // (bitmask of 1u << HE::RendererBackend). 0 → no precompile (runtime cross-compile).
    // `compileShaderVariants` is supplied by the editor (it links the shader compiler):
    // (fragment GLSL, backend bitmask) → PSHD-encoded bytes.
    uint32_t shaderBackends = 0;
    std::function<std::vector<uint8_t>(const std::string&, const std::string&, uint32_t)> compileShaderVariants;
};

struct HE_API ExportResult {
    bool        success          = false;
    std::string errorMessage;
    int         assetsPacked     = 0;
    int         binaryFilesCopied = 0; // game exe + dylibs, 0 when gameRuntimeDir empty
    int         assetsReused     = 0;  // carried verbatim from the previous pak (incremental)
    // Encryption key was patched into the copied game executable (no key in
    // project.hcfg). False with encrypt when the runtime binary carries no key
    // block (legacy runtime) — then the key ships in the hcfg as before.
    bool        keyEmbedded      = false;
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

// Locate a COMPLETE runtime bundle (a directory that actually contains
// HorizonGame / HorizonGame.exe) for the target platform. resolveRuntimeDir
// only names the canonical location, which breaks when the editor runs from a
// build tree instead of the deploy layout — this walks upward from
// editorBaseDir (a few levels) checking <dir>/Game (Host) resp.
// <dir>/GameRuntimes/<Name>, plus <dir>/out/deploy/... at each level.
// Returns an empty path when no bundle with a game executable is found.
HE_API std::filesystem::path findRuntimeBundle(const std::filesystem::path& editorBaseDir,
                                               ExportPlatform p);

// ─── Embedded pak key ─────────────────────────────────────────────────────────
// The game executable carries a 64-byte patchable key block (see
// HE_Game/src/EmbeddedPakKey.h): magic[24] | hasKey | pad[7] | key[32].
// patchEmbeddedPakKey writes hasKey=1 + key into every occurrence of the magic
// in `binary` (re-signing Mach-O files on macOS afterwards) and returns the
// number of blocks patched (0 = no block, -1 = I/O error).
// readEmbeddedPakKey extracts a previously patched key (for incremental
// exports, where project.hcfg intentionally no longer contains it).
HE_API int  patchEmbeddedPakKey(const std::filesystem::path& binary, const uint8_t key[32]);
HE_API bool readEmbeddedPakKey(const std::filesystem::path& binary, uint8_t outKey[32]);

// Deterministic pak-entry UUID for a scene, derived from its PROJECT-RELATIVE
// path (forward slashes). The exporter keys every packed scene with it and the
// game runtime derives the same UUID from scene.load("<path>") — no lookup
// table ships. FNV-1a 64 over the path (hi) and over the reversed path (lo).
HE_API HE::UUID sceneUuidForPath(const std::string& projectRelPath);

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
        const std::vector<uint8_t>&  startupSceneBinary = {},
        // Every OTHER project scene, pre-serialized to CBOR, keyed by its project-
        // relative path. Packed under sceneUuidForPath(path) so the game runtime can
        // scene.load("<path>") them for level transitions.
        const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extraScenes = {}
    );
};
