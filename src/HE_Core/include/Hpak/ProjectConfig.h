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
    std::string  mainSceneName;         // filename of startup .hescene (empty = none)
    uint8_t      projectUuidBytes[16] = {}; // PBKDF2 salt for key derivation
    bool         enableModSupport = false;
};

class HE_API ProjectConfigLoader {
public:
    static bool save(const std::filesystem::path& dir, const ProjectConfig& config);
    static bool load(const std::filesystem::path& dir, ProjectConfig& outConfig);
};
