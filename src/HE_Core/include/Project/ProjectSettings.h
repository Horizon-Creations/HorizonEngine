#pragma once
#include <Types/Defines.h>
#include <nlohmann/json_fwd.hpp>
#include <glm/vec3.hpp>
#include <filesystem>
#include <string>

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
// NOTE: Schritt 1 was the FORMAT and the panel over it; Schritt 2 connected
// `shadows` to the renderer (see ProjectShadowSettings). The physics world and
// the exporter do not read their sections yet — that is the following steps'
// work, and each such page of the panel says so in its hint.

struct HE_API ProjectGameSettings
{
    // What the game calls itself: the window title, the name a launcher shows.
    // Empty = the project's name, which is what every build has used so far.
    std::string title;
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

struct HE_API ProjectSettings
{
    static constexpr int kVersion = 1;

    ProjectGameSettings    game;
    ProjectShadowSettings  shadows;
    ProjectPhysicsSettings physics;
    ProjectRenderDefaults  renderDefaults;

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
