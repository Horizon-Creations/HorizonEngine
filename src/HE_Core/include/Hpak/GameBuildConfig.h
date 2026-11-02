#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <filesystem>
#include <string>

// ─── Values compiled into the game (Thema 211, step 4) ───────────────────────
// An SDK export compiles HorizonGame itself, so what the prebuilt runtime has to
// read from project.hcfg and Config/ProjectSettings.json at start — pak name,
// startup scene, flags, the pak key — can instead be a constant of the binary.
// The exporter writes those constants as a generated header, HEGameBuildConfig.h
// (generateGameBuildConfigHeader below); the game is built with
// -DHE_GAME_BUILD_CONFIG=<that file> and hands its one constant to the resolvers
// at the bottom, which prefer it over the files.
//
// A game built WITHOUT the header (the prebuilt runtime every export copies
// today) passes nullptr and reads the files exactly as before: the key from the
// block patched into its executable (HE_Game/src/EmbeddedPakKey.h), else from
// project.hcfg.
//
// Same trust model as the patched block: the key ships inside the executable
// either way. It is stored XOR-masked so it is not one contiguous run of bytes
// in the binary or in the header, which is obfuscation against casual pak
// ripping, not a security boundary. Nothing here ever logs key bytes.
//
// Deliberately light: the generated header includes this one, so it names
// ProjectConfig/ProjectSettings by declaration only.

struct ProjectConfig;
namespace HE { struct ProjectSettings; }

namespace HE
{

// Bumped whenever GameBuildConfig changes shape. The generated header asserts it
// at compile time, so a header from another engine version fails the build with a
// message instead of initialising the wrong fields.
inline constexpr int kGameBuildConfigFormat = 1;

// Everything project.hcfg carries plus the ProjectSettings JSON. Plain pointers
// and arrays so the generated header can spell it as one constexpr aggregate
// with designated initializers: keep the member order, the generated file
// depends on it.
struct GameBuildConfig
{
    int           format = 0;               // kGameBuildConfigFormat at generation
    const char*   buildProfile = "";        // "dev" / "release" the export asked for (informational)

    const char*   projectName = "";
    const char*   hpakFilename = "";
    const char*   mainSceneName = "";       // loose startup .hescene, "" = none
    const char*   defaultSaveTemplate = "";
    const char*   theme = "";
    const char*   themeMode = "";
    const char*   bundleId = "";

    bool          enableModSupport = false;
    bool          devTools = false;
    bool          encrypted = false;
    bool          hasPackedScene = false;
    bool          horizonCodeCompiled = false;
    bool          appMode = false;
    bool          advancedShaderEffects = true;
    bool          allowFiles = false;
    bool          allowProcesses = false;
    bool          allowNetwork = false;
    std::uint32_t fontScripts = 0;
    bool          fontWeightBold = true;

    std::uint8_t  startupSceneUuid[16] = {};
    // key = pakKeyMasked ^ pakKeyMask, byte for byte. All zero when !encrypted.
    std::uint8_t  pakKeyMasked[32] = {};
    std::uint8_t  pakKeyMask[32] = {};

    // The same JSON shapes project.hcfg and the .heproj carry. "" = the default
    // (everything collides / no buses / default project settings).
    const char*   collisionLayersJson = "";
    const char*   audioBusesJson = "";
    const char*   projectSettingsJson = "";
};

// ─── Exporter side ───────────────────────────────────────────────────────────

// The text of HEGameBuildConfig.h for `cfg` (its encKey is the pak key when
// cfg.encrypted) and `settings` (nullptr = defaults). `keyMask` fixes the mask
// for tests and reproducible output; nullptr draws a random one, and an empty
// string comes back if no random source is available while a key needs masking.
HE_API std::string generateGameBuildConfigHeader(const ProjectConfig&   cfg,
                                                 const ProjectSettings* settings,
                                                 const char*            buildProfile,
                                                 const std::uint8_t*    keyMask = nullptr);

// generateGameBuildConfigHeader written to `file` (directories created).
HE_API bool writeGameBuildConfigHeader(const std::filesystem::path& file,
                                       const ProjectConfig&         cfg,
                                       const ProjectSettings*       settings,
                                       const char*                  buildProfile,
                                       const std::uint8_t*          keyMask = nullptr);

// The pak key a header written by the generator carries, unmasked, for the
// incremental export that must reuse the previous pak's key (an SDK-built
// executable has no patched block to read it back from). False for a missing
// file, an unencrypted config or a header this generator did not write.
HE_API bool readGameBuildConfigPakKey(const std::filesystem::path& file, std::uint8_t outKey[32]);

// ─── Runtime side ────────────────────────────────────────────────────────────

// The compiled values as the ProjectConfig the game consumes (key unmasked into
// encKey). Malformed JSON tails leave their defaults, as the hcfg reader does.
HE_API void projectConfigFromBuildConfig(const GameBuildConfig& compiled, ProjectConfig& out);

enum class ProjectConfigSource { None, ProjectFile, Compiled };
HE_API const char* projectConfigSourceName(ProjectConfigSource s);

// THE entry point for the game's project configuration: the compiled values when
// there are any (project.hcfg is then not read at all), else <dir>/project.hcfg,
// else None (no project: the game runs without a pak).
HE_API ProjectConfigSource resolveProjectConfig(const GameBuildConfig*       compiled,
                                                const std::filesystem::path& dir,
                                                ProjectConfig&               out);

// Config/ProjectSettings.json with the same precedence. Compiled: the header's
// JSON ("" = defaults), the file is not read. Otherwise loadProjectSettings with
// its contract (missing file = defaults and true, damaged file = false).
HE_API bool resolveProjectSettings(const GameBuildConfig*       compiled,
                                   const std::filesystem::path& dir,
                                   ProjectSettings&             out);

enum class PakKeySource { None, Compiled, EmbeddedBlock, ProjectFile };
HE_API const char* pakKeySourceName(PakKeySource s);

// Which key mounts the pak, in order: the compiled-in key (cfg came from the
// header), the block the exporter patched into a prebuilt executable
// (`embeddedKey`, nullptr when the block was never patched), the key in
// project.hcfg. `outKey` is nullptr for an unencrypted pak. `cfg` must outlive
// the use of `outKey` (it points into cfg.encKey for the first and last case).
HE_API PakKeySource selectPakKey(ProjectConfigSource   source,
                                 const ProjectConfig&  cfg,
                                 const std::uint8_t*   embeddedKey,
                                 const std::uint8_t*&  outKey);

} // namespace HE
