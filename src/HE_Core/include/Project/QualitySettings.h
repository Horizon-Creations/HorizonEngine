#pragma once
#include <Types/Defines.h>
#include <cstdint>
#include <string>

class GlobalState;   // Diagnostics/GlobalState.h; only readGraphicsFromConfig names it

// ─── Quality: shadows, graphics, presets ──────────────────────────────────────
// The part of the project settings (ProjectSettings.h) that decides how the game
// LOOKS and what it costs, in a header of its own with no JSON in it. WHY: the
// script API (HorizonScene/EngineApi.h, settings::Host) hands these structs to the
// application, and EngineApi.h is part of the engine SDK that generated game code
// compiles against; ProjectSettings.h needs nlohmann's json_fwd, which that header
// set does not carry. ProjectSettings.h includes this file, so nothing that used
// either of them has to change.
namespace HE {

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

// ─── Graphics ─────────────────────────────────────────────────────────────────
// How the game LOOKS: anti-aliasing, post-processing, reflections, global
// illumination and the render path. Until now these lived in the editor's own
// config.json — one set for every project on the machine — and the exporter
// wrote that set into every build ("the graphics half is the editor's live
// configuration"). Two projects therefore shared their bloom, and what a game
// looked like depended on who exported it. Here they travel with the project.
//
// `enabled` false (the default) IS that old behaviour, so a project without
// this block ships exactly what it always did: the exporting editor's values,
// and the editor's viewport previews with the user's own. `enabled` true makes
// the values below the project's word — the export writes them, and the
// viewport follows them unless the user chose "my own values" for the preview
// (Preferences ▸ Viewport Quality).
//
// One list, expanded where a struct, a comparison, the file and the editor's
// config need it, so a field is added in exactly one place. Columns: type,
// member (also the key in the file), default, the EditorConfig member it
// mirrors, lowest and highest accepted value (ignored for bool).
#define HE_PROJECT_GRAPHICS_FIELDS(X) \
    /* Quality */ \
    X(int,   renderPath,                 1,     RenderPath,                0.0,    1.0) \
    X(int,   antiAliasing,               1,     AntiAliasing,              0.0,    4.0) \
    X(float, aaSharpness,                0.35f, AASharpness,               0.0,    1.0) \
    X(float, renderScale,                1.0f,  RenderScale,               0.25,   2.0) \
    X(bool,  specularAA,                 true,  SpecularAA,                0.0,    1.0) \
    X(float, specularAAStrength,         1.0f,  SpecularAAStrength,        0.0,    2.0) \
    X(bool,  occlusionCulling,           false, OcclusionCulling,          0.0,    1.0) \
    X(bool,  gpuParticles,               true,  GpuParticles,              0.0,    1.0) \
    /* Post-processing */ \
    X(bool,  bloomEnabled,               true,  BloomEnabled,              0.0,    1.0) \
    X(float, bloomThreshold,             1.0f,  BloomThreshold,            0.0,    10.0) \
    X(float, bloomIntensity,             0.6f,  BloomIntensity,            0.0,    5.0) \
    X(bool,  ssaoEnabled,                true,  SSAOEnabled,               0.0,    1.0) \
    X(float, ssaoRadius,                 0.5f,  SSAORadius,                0.01,   10.0) \
    X(float, ssaoIntensity,              1.0f,  SSAOIntensity,             0.0,    1.0) \
    X(int,   ssaoMethod,                 0,     SSAOMethod,                0.0,    2.0) \
    X(bool,  dofEnabled,                 false, DoFEnabled,                0.0,    1.0) \
    X(float, dofFocusDistance,           10.0f, DoFFocusDistance,          0.1,    1000.0) \
    X(float, dofFocusRange,              4.0f,  DoFFocusRange,             0.0,    1000.0) \
    X(float, dofAperture,                2.8f,  DoFAperture,               1.0,    22.0) \
    X(bool,  motionBlurEnabled,          false, MotionBlurEnabled,         0.0,    1.0) \
    X(float, motionBlurIntensity,        0.5f,  MotionBlurIntensity,       0.0,    2.0) \
    X(float, motionBlurMax,              24.0f, MotionBlurMax,             0.0,    128.0) \
    /* Reflections and global illumination */ \
    X(bool,  ssrEnabled,                 false, SSREnabled,                0.0,    1.0) \
    X(float, ssrIntensity,               1.0f,  SSRIntensity,              0.0,    2.0) \
    X(float, ssrMaxRoughness,            0.6f,  SSRMaxRoughness,           0.0,    1.0) \
    X(int,   ssrQuality,                 1,     SSRQuality,                0.0,    2.0) \
    X(bool,  giEnabled,                  false, GlobalIlluminationEnabled, 0.0,    1.0) \
    X(float, giIndirectIntensity,        1.0f,  GIIndirectIntensity,       0.0,    5.0) \
    X(float, giLightRadius,              0.5f,  GILightRadius,             0.0,    10.0) \
    X(int,   giShadowQuality,            1,     GIShadowQuality,           0.0,    2.0) \
    X(bool,  giReflectionsEnabled,       false, GIReflectionsEnabled,      0.0,    1.0) \
    X(float, giReflIntensity,            1.0f,  GIReflIntensity,           0.0,    2.0) \
    X(float, giReflMaxRoughness,         0.6f,  GIReflMaxRoughness,        0.0,    1.0) \
    X(int,   giReflQuality,              1,     GIReflQuality,             0.0,    2.0) \
    X(int,   giReflBounces,              1,     GIReflBounces,             1.0,    4.0) \
    X(bool,  giReflBlur,                 false, GIReflBlur,                0.0,    1.0) \
    /* Asset detail (qualityLevel: 0 Low, 1 Medium, 2 High; High IS today) */ \
    X(int,   textureQuality,             2,     TextureQuality,            0.0,    2.0) \
    X(int,   materialQuality,            2,     MaterialQuality,           0.0,    2.0)

// ─── Quality presets ──────────────────────────────────────────────────────────
// Named bundles over the fields above and the shadow settings: one word instead
// of a dozen sliders, for the editor's quality page and for a game's graphics
// menu (HE::api). A preset is NOT a second set of values — it is a function that
// writes the cost-driving fields of ProjectGraphics + ProjectShadowSettings, so
// every existing consumer (viewport, export, game) keeps reading the same
// members and a preset change reaches them without a new road.
//
//   Custom   the values are whatever they are; the default, because a project
//            written before presets existed has no tag and must load untouched.
//   Low..Ultra   applyQualityPreset() wrote the table in ProjectSettings.cpp.
//
// The tag is a LABEL for the values, never their source: ProjectGraphics::preset
// says what the values were last set from, and detectQualityPreset() says what
// they are now. ProjectSettings::clamp() reconciles the two, so a hand-edited
// file or a single slider moved after a preset reads as Custom, not as a preset
// it no longer matches.
//
// Only fields that cost frame time or memory are in a preset (anti-aliasing,
// render scale, SSAO/SSR on-off, SSR and GI quality, GI reflections, shadow
// resolution/cascades/distance, texture and material quality). What the picture
// LOOKS like at equal cost — bloom, depth of field, motion blur, biases, render
// path — is left to the project. Backends that lack a level ignore it
// (docs/engine-quality-settings-2026-11-05.md has the per-backend table).
enum class QualityPreset : std::uint8_t { Low, Medium, High, Ultra, Custom };
inline constexpr int kQualityPresetCount = 5;   // including Custom

// "Low" / "Medium" / "High" / "Ultra" / "Custom" — the spelling the file, the
// editor and the script API use.
HE_API const char*   qualityPresetName(QualityPreset p);
// Case-insensitive. False (and `out` untouched) for any other text.
HE_API bool          qualityPresetFromName(const std::string& name, QualityPreset& out);

struct HE_API ProjectGraphics
{
    bool enabled = false;
#define HE_X(T, name, def, editorName, lo, hi) T name = def;
    HE_PROJECT_GRAPHICS_FIELDS(HE_X)
#undef HE_X
    // What the values were last set from (see QualityPreset). Not in the field
    // list on purpose: it has no EditorConfig twin, it is project data only.
    QualityPreset preset = QualityPreset::Custom;
};

// Fills every graphics field from the config.json keys the editor's Preferences
// write and the export dialog carries into a build (the field list's `EditorConfig`
// column IS the key), a missing key reading as the field's default. `enabled` and
// `preset` are left alone. The packaged game does this once at start and then
// pushes `ProjectSettings::graphics` to the renderer every frame, so a script that
// changes the struct (applyQualityPreset) is what the next frame draws — where
// until now every frame re-read the keys and there was nothing to change.
HE_API void readGraphicsFromConfig(ProjectGraphics& g, const GlobalState& cfg);

// Writes the preset's values into `g` and `shadows`. A named preset also turns
// `g.enabled` on — the project's graphics block only reaches the viewport and
// the export while it is on, so choosing "High" with it off would change
// nothing anyone could see. Custom changes nothing at all.
HE_API void          applyQualityPreset(QualityPreset p, ProjectGraphics& g, ProjectShadowSettings& shadows);
// The preset whose values `g` and `shadows` hold right now, Custom when none.
// `g.enabled` and `g.preset` are not looked at.
HE_API QualityPreset detectQualityPreset(const ProjectGraphics& g, const ProjectShadowSettings& shadows);

// ── The quality fields by name ────────────────────────────────────────────────
// The single regulators the presets are made of (and a few more of the same
// areas: bloom, depth of field, motion blur), addressed by a stable name so the
// script API, the editor's quality page and a hardware benchmark share one list
// instead of each spelling thirty members. Bools travel as 0/1, ints as their
// number. Names are the member names of ProjectGraphics (antiAliasing, renderScale,
// ssaoEnabled, ssrQuality, giEnabled, textureQuality, materialQuality …) and, for
// the shadow settings, shadowDistance / shadowCascades / shadowResolution.
HE_API int         qualityFieldCount();
HE_API const char* qualityFieldName(int index);   // nullptr when out of range
// False (and `out` untouched) for a name that is not a quality field.
HE_API bool        getQualityField(const ProjectGraphics& g, const ProjectShadowSettings& shadows,
                                   const std::string& name, float& out);
// Clamps into the field's range (ints rounded, bools by >= 0.5, the shadow
// resolution to a power of two) and writes it; neither `enabled` nor the preset
// tag is touched. False for an unknown name or a value that is not a number.
HE_API bool        setQualityField(ProjectGraphics& g, ProjectShadowSettings& shadows,
                                   const std::string& name, float value);

} // namespace HE
