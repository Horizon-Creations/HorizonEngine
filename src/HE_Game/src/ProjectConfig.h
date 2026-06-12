#pragma once
#include <string>
#include <filesystem>
#include <cstdint>

// project.hcfg is a small binary file placed next to MyGame.exe.
// It is NOT encrypted — it contains only non-secret metadata.
// The AES key is derived at runtime from:
//   secret  = HE_PROJECT_SECRET (injected at compile time via CMake define)
//   salt    = projectUuidBytes from this config
struct ProjectConfig {
    std::string  projectName;
    std::string  hpakFilename;       // e.g. "MyGame.hpak"
    std::string  mainSceneName;      // UUID string of the startup scene
    uint8_t      projectUuidBytes[16]; // used as PBKDF2 salt for key derivation
    bool         enableModSupport = false;
};

class ProjectConfigLoader {
public:
    // Reads project.hcfg from the same directory as the executable.
    static bool load(const std::filesystem::path& exeDir, ProjectConfig& outConfig);
    static bool save(const std::filesystem::path& exeDir, const ProjectConfig& config);
};
