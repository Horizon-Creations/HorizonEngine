#pragma once
#include <Types/Defines.h>
#include <nlohmann/json_fwd.hpp>
#include <glm/vec3.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace HE {

// ─── Project settings ─────────────────────────────────────────────────────────
// What the PROJECT is, as opposed to what the editor on this machine prefers
// (EditorConfig, config.json) and what one export run was told (ExportProfile).
// The shadows a game draws, the rate its physics steps at, the window it opens
// in: none of those may differ between two people's machines or between the
// preview and the shipped build, which is why they are neither editor
// preferences nor export options.
//
// WHY A FILE OF ITS OWN and not more keys in the .heproj: the .heproj is the
// manifest — identity, language, profiles — and it is rewritten by half a dozen
// panels for reasons that have nothing to do with these values. This file is
// only ever written by the Project Settings tab, so a diff of it is a diff of
// the settings; and the packaged build can carry it VERBATIM next to
// project.hcfg instead of growing that binary format a version per knob.
//
// WHY HE_Core: the same reason CollisionLayerConfig lives here. The editor
// writes it (HE_Tools/HE_Editor) and the runtime reads it (HE_Game via
// HorizonScene, which does not link HE_Tools); HE_Core is the one place both
// ends of the road can look at.
//
// Location: <ProjectRoot>/Config/ProjectSettings.json (projectSettingsPath()).
// Human-readable on purpose — it is meant to be reviewed in a merge request.
//
// ── The versioning bargain ────────────────────────────────────────────────────
// Default-constructed IS TODAY'S BEHAVIOUR, field by field: the shadow numbers
// are the constants RenderExtractor has always used, the physics rate is
// PhysicsWorld::kFixedDt, the render defaults say "take what the editor's
// Preferences say", which is what every export has done. So a project WITHOUT
// this file behaves exactly as before, and fromJson() reads a missing key as
// its default and ignores a key it does not know — a file written by a newer
// engine loads in an older one with the newer knobs simply not applied.
//
// `version` is written for the day the meaning of a key has to change (not its
// presence — that is handled by the defaults). Until then it is 1.
//
// ── Who reads what ────────────────────────────────────────────────────────────
//   game.title      the game's window title (GameApplication::GetConfig) and
//                   the export's display name (Info.plist, .desktop, .reg).
//   shadows         IRenderer::ShadowSettings, pushed per frame by editor + game.
//   physics         PhysicsWorld gravity + the fixed step both apps drive it at.
//   renderDefaults  the export dialog's config.json when useEditorSettings is off.
//   anticheat       the host's AntiCheatService (HorizonScene) — thresholds,
//                   what happens per level, the game's value rules.
// The exporter copies the file verbatim to <data>/Config/ProjectSettings.json
// (ExportSettings::projectSettingsFile); the packaged game loads it from there
// before its window opens.

struct HE_API ProjectGameSettings
{
    // What the game calls itself: the window title, the name a launcher shows.
    // Empty = the project's name, which is what every build has used so far.
    std::string title;

    // ── Splash ───────────────────────────────────────────────────────────────
    // A small always-on-top window that stands in for the game while it starts
    // — the same SplashScreen the editor opens, which until now only the editor
    // used: an exported game showed a black rectangle for as long as its
    // renderer took to come up. Off by default, because a splash that says
    // "Horizon Engine" inside somebody else's product is the engine advertising
    // itself, and a splash without a logo would draw exactly that. So the game
    // opens one only when `splashImage` names a picture.
    bool        splashEnabled = false;
    // PROJECT-relative path of a PNG ("Content/Splash.png", forward slashes).
    // The export copies it beside project.hcfg as Splash.png; the packaged game
    // reads it from there (GameApplication::GetConfig).
    std::string splashImage;
    // The small line under the title — a version, a studio, a tagline. Free
    // text and deliberately NOT the bundle version: that lives in the .heproj
    // and never reaches the runtime, and a subtitle somebody typed is honest.
    std::string splashSubtitle;
};

struct HE_API ProjectShadowSettings
{
    // Directional-light cascaded shadow maps (Metal + OpenGL). Defaults are the
    // constants RenderExtractor::extract has always fit its cascades with.
    // The road to the renderer: the editor (and the packaged game) push these
    // as IRenderer::ShadowSettings every frame; the backend hands distance /
    // count / lambda / resolution to its RenderExtractor and the bias pair to
    // its shaders. Backends without cascades (D3D11/D3D12/Vulkan, still on one
    // whole-scene map) ignore the push and draw as they always have.
    float distance     = 250.0f;   // metres of shadow coverage from the camera
    int   cascadeCount = 3;        // 1..kMaxCascades
    int   resolution   = 2048;     // texels per cascade edge, a power of two
    float splitLambda  = 0.5f;     // 0 = uniform splits, 1 = logarithmic
    // The receiver-side depth bias, as the CSM shaders (GL kUnlitFS, Metal
    // shadowFactor, the material library's heCsmShadow) spell it:
    //   bias = clamp(slopeBias * tan(acos(N·L)), minBias, 0.02) * (cascade + 1)
    // Defaults are the literals those shaders carried before this existed.
    float slopeBias    = 0.0008f;
    float minBias      = 0.0002f;

    // 3, not ShadowData::kMaxCascades (4): every cascade consumer — the GL
    // shader's CSM_CASCADES, Metal's SceneUniforms::cascadeVP[3], the
    // material library's append-only Lighting::csmVP[3] — is built for three,
    // and the panel must not offer a fourth the renderer would silently drop.
    // A fourth cascade is a renderer change, and this ceiling moves with it.
    static constexpr int   kMaxCascades   = 3;
    static constexpr int   kMinResolution = 256;
    static constexpr int   kMaxResolution = 8192;
    static constexpr float kMaxDistance   = 5000.0f;
};

struct HE_API ProjectPhysicsSettings
{
    // The fixed step, as a RATE in Hz rather than a dt in seconds: 60 is exact
    // and 1/60 is not, and a rate is what somebody means when they tune this.
    // Default is what PhysicsWorld::kFixedDt has always been.
    int       fixedHz = 60;
    glm::vec3 gravity = glm::vec3(0.0f, -9.81f, 0.0f);   // m/s², rigid bodies

    static constexpr int kMinHz = 10;
    static constexpr int kMaxHz = 480;

    // The step length both applications hand to HE::advanceFixedSteps. ONE
    // function so the editor's preview and the packaged game cannot round the
    // same rate two ways; clamped so a hand-edited "0" cannot become infinity.
    float fixedDt() const
    {
        const int hz = fixedHz < kMinHz ? kMinHz : (fixedHz > kMaxHz ? kMaxHz : fixedHz);
        return 1.0f / static_cast<float>(hz);
    }
};

struct HE_API ProjectRenderDefaults
{
    // true (default) = the packaged build boots with whatever the exporting
    // editor's Preferences held, which is what every export has done and what
    // a project that never opened this page keeps doing. false = the window
    // and backend below are the project's word, on every machine it is
    // exported from.
    bool        useEditorSettings = true;
    int         windowWidth       = 1280;
    int         windowHeight      = 720;
    std::string windowMode        = "Fullscreen";   // Windowed | Fullscreen | Borderless
    bool        vsync             = true;
    // Renderer backend name as RendererFactory spells it ("Metal", "OpenGL",
    // "Vulkan", "D3D11", "D3D12"); empty = the platform's own default, which is
    // a different answer from naming one the target might not have.
    std::string backend;

    static constexpr int kMinWindowEdge = 320;
    static constexpr int kMaxWindowEdge = 16384;
};

// ─── Anti-cheat ───────────────────────────────────────────────────────────────
// What the host's AntiCheatService (docs/anti-cheat-plan.md §4.4) is told about
// this project. It lives here and not in an asset because it is project-wide
// and belongs in a merge request next to the physics rate: a kick threshold is
// a decision about the game, not about one scene.
//
// The spellings the file uses, at namespace scope rather than as static
// members of the exported structs below: an array a DLL boundary has to hand
// out by address is a different thing from the scalar constants the other
// structs carry, and a string literal copied per module is exactly as good —
// every use compares or copies the text, never the pointer.
inline constexpr int         kAntiCheatLevelCount = 3;
inline constexpr const char* kAntiCheatLevels[kAntiCheatLevelCount] = { "suspect", "confirmed", "hard" };
inline constexpr int         kAntiCheatResponseCount = 6;   // bits in Response, Log first
inline constexpr const char* kAntiCheatResponseNames[kAntiCheatResponseCount] =
    { "log", "event", "telemetry", "flag", "kick", "ban" };

// A value rule (§3.4): a number the engine does not know — damage, loot, a
// currency delta — that the game declares once and has checked with one call
// (`anticheat.check("Damage", value, player)`). Range, then rate per source.
struct HE_API ProjectAntiCheatRule
{
    std::string name;                 // what the game's check() call names
    float       min          = 0.0f;  // inclusive range a single value may take
    float       max          = 0.0f;
    float       maxPerSecond = 0.0f;  // summed over one second per source; 0 = unchecked
    // What a violation counts as: "suspect" | "confirmed" | "hard". Hard is for
    // the values where a single violation is proof (currency 0 → 10^9); the
    // other two feed the decaying score like any engine observation.
    std::string level = "suspect";

    // Index into kAntiCheatLevels, or 0 for a spelling the file does not know.
    int levelIndex() const;
};

struct HE_API ProjectAntiCheatSettings
{
    // ── Switch ───────────────────────────────────────────────────────────────
    // Off = the absence of the service; the host behaves byte-for-byte as it
    // did before anti-cheat existed. That is the default, so a project that
    // never opened the page ships nothing new.
    bool enabled = false;
    // Compare the exe/dylib/pak hashes a guest sends at join against the host's
    // own (§3.5). True by default: the plan's recommended setup is what ticking
    // `enabled` should give, and this flag does nothing while `enabled` is off.
    bool integrityCheck = true;

    // ── Limits (§3.3) ────────────────────────────────────────────────────────
    // Accepted simulated time may outrun wall time by this fraction before it
    // counts as a stretched clock. 0.15 = 15 %.
    float tolerance = 0.15f;
    // Seconds the dt ratio is measured over. Seconds and not frames, because a
    // burst after a stall delivers thirty commands in one frame whose dt sums
    // to exactly the time the host waited.
    float windowSec = 3.0f;
    // Inputs per second beyond which commands are dropped — 4× a 60 Hz client,
    // because bursts after a stall arrive all at once.
    int maxInputsPerSecond = 240;

    // ── Score (§3.6) ─────────────────────────────────────────────────────────
    float scoreHalfLifeSec = 30.0f;   // score halves every this many seconds
    float scoreSuspect     = 5.0f;    // score >= this is Suspect
    float scoreConfirmed   = 20.0f;   // score >= this is Confirmed; >= suspect

    // ── Policy (§5.2, §5.3) ──────────────────────────────────────────────────
    // What the host does on its own when a connection reaches a level, as a
    // set of responses. A game handler may replace it per report; without one
    // this is what happens. Log is in every set and cannot be taken out — a
    // level change is always a line in Cat::AntiCheat — so `[]` in the file
    // means "log only", which is the observation mode for calibrating the
    // thresholds on real players before the first kick (§5.3).
    enum Response : std::uint32_t
    {
        Log       = 1u << 0,
        Event     = 1u << 1,   // OnCheatDetected in every frontend + EventBus
        Telemetry = 1u << 2,   // report into the upload queue (needs telemetryUrl)
        Flag      = 1u << 3,   // marked for review, no game effect
        Kick      = 1u << 4,   // NetSession::disconnect after a notice
        Ban       = 1u << 5,   // kick + refused for the rest of this session
        AllResponses = Log | Event | Telemetry | Flag | Kick | Ban,
    };
    // Defaults are the plan's: no kick on the two score levels, whose
    // thresholds are unmeasured starting values; a kick on Hard, which no
    // hitch, burst or clock can produce.
    std::uint32_t policySuspect   = Log | Event | Telemetry;
    std::uint32_t policyConfirmed = Log | Event | Telemetry;
    std::uint32_t policyHard      = Log | Event | Telemetry | Kick;

    // ── Telemetry (§3.7) ─────────────────────────────────────────────────────
    // Where reports are POSTed. Empty = no telemetry, which is the default;
    // this is the engine's own setting and NOT the scripts' "Network access"
    // permission, on purpose.
    std::string telemetryUrl;

    // ── Rules (§3.4) ─────────────────────────────────────────────────────────
    std::vector<ProjectAntiCheatRule> rules;

    // Ranges clamp() holds the numbers in. Wide on purpose: these are sanity
    // bounds against a hand-edited file, not tuning advice.
    static constexpr float kMaxTolerance          = 2.0f;
    static constexpr float kMinWindowSec          = 0.5f;
    static constexpr float kMaxWindowSec          = 60.0f;
    static constexpr int   kMinInputsPerSecond    = 1;
    static constexpr int   kMaxInputsPerSecond    = 10000;
    static constexpr float kMinHalfLifeSec        = 0.1f;
    static constexpr float kMaxHalfLifeSec        = 3600.0f;
    static constexpr float kMaxScoreThreshold     = 1.0e6f;
    static constexpr float kMaxRuleValue          = 1.0e9f;
    static constexpr int   kMaxRules              = 256;
};

struct HE_API ProjectSettings
{
    static constexpr int kVersion = 1;

    ProjectGameSettings      game;
    ProjectShadowSettings    shadows;
    ProjectPhysicsSettings   physics;
    ProjectRenderDefaults    renderDefaults;
    ProjectAntiCheatSettings antiCheat;

    // True when nothing differs from a fresh construction. The saver uses it to
    // leave a project that never touched its settings WITHOUT a file, so an old
    // project opened once does not acquire one nobody asked for.
    bool isDefault() const;

    // Every value, every time — this file is meant to be read by a person, and
    // a person reading it should see what the knobs are, not guess which ones
    // were left out because they happened to be default.
    void toJson(nlohmann::json& out) const;
    // Missing key = default, unknown key = ignored, wrong type = default,
    // numbers clamped into the ranges the panel enforces. Never throws.
    void fromJson(const nlohmann::json& in);

    // Bring every number back into range. fromJson() does this itself; the
    // panel calls it after a typed edit so a "0" in the resolution field
    // cannot reach the file.
    void clamp();

    bool operator==(const ProjectSettings& o) const;
    bool operator!=(const ProjectSettings& o) const { return !(*this == o); }
};

// <projectRoot>/Config/ProjectSettings.json.
HE_API std::filesystem::path projectSettingsPath(const std::filesystem::path& projectRoot);

// Reads the file into `out`. A MISSING file is a success that leaves `out`
// default-constructed — that is the state of every project made before this
// existed. False only for a file that is there and cannot be read or parsed;
// `out` is then left untouched, so a damaged file never silently becomes a
// reset on the next save.
HE_API bool loadProjectSettings(const std::filesystem::path& projectRoot, ProjectSettings& out);

// Writes the file — through a temp file and a rename, like the .heproj — or,
// for a default-constructed `settings` with NO file on disk yet, writes
// nothing and answers true (see ProjectSettings::isDefault). An existing file
// is always rewritten, defaults included: once a project has one, "back to
// default" is a change somebody made and a diff should show it.
HE_API bool saveProjectSettings(const std::filesystem::path& projectRoot, const ProjectSettings& settings);

} // namespace HE
