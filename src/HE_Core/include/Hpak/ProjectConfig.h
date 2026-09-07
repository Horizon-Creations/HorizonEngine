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

    // ── The theme the application boots with (docs/he-apps-plan.md D1) ───────
    // Content-relative path of a Theme asset; empty = the engine's built-in
    // default. It travels HERE and not in config.json, for the same reason
    // appMode does: config.json is settings a PLAYER may edit, this is what the
    // project IS.
    std::string  theme;
    // "System", "Light" or "Dark" — what was ASKED for, not what it resolved to.
    // Empty reads as System. Storing the resolved value would ship whatever mode
    // the author's machine happened to be in as everyone's fixed mode.
    std::string  themeMode;

    // ── What a script in this build may reach outside it (plan, Block C) ─────
    // Three doors, and all three shut unless the project said otherwise. Stored
    // straight (not negated like advancedShaderEffects above) because "off" is
    // what every build written before them meant AND what they should default
    // to: the negated trick is for a flag whose honest default is on.
    //
    // allowFiles is about what a SCRIPT may name on its own. A path the person
    // using the app picked in a file dialog is granted for that session either
    // way — the choosing is the permission (HE::api::fs::grantPath).
    bool         allowFiles     = false;
    bool         allowProcesses = false;
    bool         allowNetwork   = false;   // reserved for `http` (Welle 3)

    // Which scripts the font atlas carries beyond Latin (a HE::UIFontScripts
    // mask). Rides in the same flags word — one bit per script, stored straight,
    // so a build written before this reads back as the base set, which is what
    // it had.
    std::uint32_t fontScripts = 0;

    // The weight the UI text is drawn in. Stored NEGATED in the flags word (like
    // advancedShaderEffects above): a build written before this has the bit clear
    // and must read back as bold, which is what it drew.
    bool         fontWeightBold = true;

    // What the application calls itself to the system it runs on (the same
    // string as CFBundleIdentifier). Resolved at export, never derived again
    // here: an autostart entry filed under a different name than the bundle is
    // an entry nobody can find later. Empty in a build made before this existed
    // — and empty on purpose for a GAME whose id is exactly what its name
    // derives to, because filling it pushes the file to v5 and an older runtime
    // bundle rejects every version it does not know (see the writer).
    std::string  bundleId;
};

class HE_API ProjectConfigLoader {
public:
    static bool save(const std::filesystem::path& dir, const ProjectConfig& config);
    static bool load(const std::filesystem::path& dir, ProjectConfig& outConfig);
};
