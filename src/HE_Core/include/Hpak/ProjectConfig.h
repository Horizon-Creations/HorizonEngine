#pragma once
#include <Types/Defines.h>
#include <string>
#include <filesystem>
#include <cstdint>

// Runtime configuration for a packaged game.
// Stored as "project.hcfg" next to the game executable.
struct HE_API ProjectConfig {
    std::string  projectName;
    std::string  hpakFilename;          // e.g. "MyGame.hpak"
    std::string  mainSceneName;         // fallback: filename of a loose startup .hescene (empty = none)
    uint8_t      projectUuidBytes[16] = {}; // legacy (unused by the AES-GCM path)
    bool         enableModSupport = false;
    // AES-256 key for the pak. NOTE: this ships with the game, so it is
    // obfuscation against casual ripping, not a security boundary. The runtime
    // reads it here and hands it to ContentManager::loadPak().
    bool         encrypted = false;
    uint8_t      encKey[32] = {};
    // Startup scene packed into the .hpak as a binary (CBOR) entry. When
    // hasPackedScene is set, the runtime reads startupSceneUuid from the mounted
    // pak and deserializes it, instead of loading a loose mainSceneName file.
    bool         hasPackedScene = false;
    uint8_t      startupSceneUuid[16] = {};
};

class HE_API ProjectConfigLoader {
public:
    static bool save(const std::filesystem::path& dir, const ProjectConfig& config);
    static bool load(const std::filesystem::path& dir, ProjectConfig& outConfig);
};
