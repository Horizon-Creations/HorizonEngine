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
    // AES-256-GCM key for the pak. NOTE: this ships with the game, so it is
    // obfuscation against casual ripping, not a security boundary. The runtime
    // reads it here and hands it to ContentManager::mountPak() (GameApplication).
    // Only populated as a fallback: when the exporter could patch the key block
    // into the game executable it stays all-zero and just `encrypted` is set.
    bool         encrypted = false;
    uint8_t      encKey[32] = {};
    // Startup scene packed into the .hpak as a binary (CBOR) entry. When
    // hasPackedScene is set, the runtime reads startupSceneUuid from the mounted
    // pak and deserializes it, instead of loading a loose mainSceneName file.
    bool         hasPackedScene = false;
    uint8_t      startupSceneUuid[16] = {};
    // The export compiled HorizonCode to native C++ (HorizonCodeGen library
    // shipped beside the executable). Purely diagnostic: the runtime falls back
    // to the interpreter either way, but a missing/rejected library becomes a
    // LOUD warning instead of a silent slowdown.
    bool         horizonCodeCompiled = false;
    // Content-relative path of the project's default SaveGameTemplate asset —
    // what save.create() bases a new save on (see HE::api::save). Empty = the
    // project defined none (v2 configs load as empty).
    std::string  defaultSaveTemplate;

    // ── Application projects (docs/he-apps-plan.md, A0/A1) ───────────────────
    // appMode: this build is an APPLICATION, not a game. No world, no physics,
    // no scene, no default camera — the widget tree the GameInstance creates in
    // OnInit is the whole picture, and the frame loop only draws when something
    // changed. Rides in the existing flags word, so the file format does not
    // move and an older runtime simply ignores the bit.
    bool         appMode = false;
    // advancedShaderEffects: may this project use material graphs on its UI
    // ("Schicht 1")? Off means the editor never offered a material asset, so
    // nothing references one, and the runtime can be built without a shader
    // compiler. Default TRUE — a game always has them, which is also why the
    // flags bit stores the NEGATION: a config written before this existed has
    // the bit clear and must read back as enabled.
    bool         advancedShaderEffects = true;
};

class HE_API ProjectConfigLoader {
public:
    static bool save(const std::filesystem::path& dir, const ProjectConfig& config);
    static bool load(const std::filesystem::path& dir, ProjectConfig& outConfig);
};
