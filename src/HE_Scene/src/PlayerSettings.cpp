#include "HorizonScene/EngineApi.h"
#include <Diagnostics/Logger.h>
#include <Renderer/IRenderer.h>   // HE::AAMethod
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

// The player's settings (Thema 85, Schritt 5). The store, its persistence and
// the rows that change it, in one file rather than spread through EngineApi.cpp
// beside the groups the rows are filed under: the rows share one state, one
// clamp table and one "apply" path, and reading them together is the point.
namespace HE::api {

namespace settings {
namespace {

struct State
{
    Values values;
    Host   host;
    bool   installed = false;
};
State& state() { static State s; return s; }

// The ranges, in one place: the setters clamp to them and so does the loader,
// so a hand-edited Prefs.json cannot put a value in that the menu never could.
constexpr float kDeadzoneMax = 0.9f;
constexpr float kScaleMin    = 0.05f;
constexpr float kScaleMax    = 10.0f;
constexpr float kVolumeMax   = 2.0f;
constexpr int   kFrameGenMax = 4;

float clampDeadzone(float v) { return std::clamp(v, 0.0f, kDeadzoneMax); }
float clampScale(float v)    { return std::clamp(v, kScaleMin, kScaleMax); }
float clampVolume(float v)   { return std::clamp(v, 0.0f, kVolumeMax); }

// NaN is refused rather than clamped: std::clamp passes it straight through,
// and a NaN deadzone turns every stick into a stick that never moves.
bool usable(float v, const char* row)
{
    if (std::isfinite(v)) return true;
    HE_LOG_WARN(Script, "%s: not a number — ignored", row);
    return false;
}

float effectiveDeadzone()
{
    const State& s = state();
    return s.values.stickDeadzone.value_or(s.host.stickDeadzone);
}
bool effectiveVSync()
{
    const State& s = state();
    return s.values.vsync.value_or(s.host.vsync);
}
bool effectiveFullscreen()
{
    const State& s = state();
    return s.values.fullscreen.value_or(s.host.fullscreen);
}

void applyDeadzone()
{
    if (const Host& h = state().host; h.applyStickDeadzone) h.applyStickDeadzone(effectiveDeadzone());
}
void applyVSync()
{
    if (const Host& h = state().host; h.applyVSync) h.applyVSync(effectiveVSync());
}
void applyFullscreen()
{
    if (const Host& h = state().host; h.applyFullscreen) h.applyFullscreen(effectiveFullscreen());
}
void applyVolume(const std::string& bus)
{
    const State& s = state();
    if (!s.host.applyVolume) return;
    const auto it = s.values.volumes.find(bus);
    s.host.applyVolume(bus, it != s.values.volumes.end() ? std::optional<float>(it->second)
                                                         : std::nullopt);
}
// Every bus the player set before OR sets now: one that is gone from the new
// set has to be handed back to the project, not left at the old choice.
void applyVolumes(const std::map<std::string, float>& before)
{
    for (const auto& [bus, v] : before) applyVolume(bus);
    for (const auto& [bus, v] : state().values.volumes)
        if (!before.count(bus)) applyVolume(bus);
}
// The camera values have no hook: the rig controller reads values() itself,
// every frame, so there is nothing to push.
void applyAll(const std::map<std::string, float>& volumesBefore)
{
    applyDeadzone();
    applyVSync();
    applyFullscreen();
    applyVolumes(volumesBefore);
}
// Replace the held values and apply the difference.
void replace(Values v)
{
    const std::map<std::string, float> before = std::move(state().values.volumes);
    state().values = std::move(v);
    applyAll(before);
}

Values sanitized(Values v)
{
    if (v.stickDeadzone)         v.stickDeadzone         = clampDeadzone(*v.stickDeadzone);
    if (v.stickSensitivityScale) v.stickSensitivityScale = clampScale(*v.stickSensitivityScale);
    for (auto& [bus, vol] : v.volumes) vol = clampVolume(vol);
    if (v.frameGeneration) v.frameGeneration = std::clamp(*v.frameGeneration, 1, kFrameGenMax);
    return v;
}

bool isEmpty(const Values& v)
{
    return !v.stickDeadzone && !v.stickSensitivityScale && !v.stickInvertY
        && !v.vsync && !v.fullscreen && v.volumes.empty()
        && !v.upscaler && !v.upscalerMode && !v.frameGeneration;
}

template <std::size_t N>
bool listed(const char* const (&names)[N], const std::string& name)
{
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
GraphicsSupport support()
{
    const Host& h = state().host;
    return h.graphicsSupport ? h.graphicsSupport() : GraphicsSupport{};
}
// The AAMethod of a kUpscalers name; Off has none.
int methodOf(const std::string& name)
{
    using M = HE::AAMethod;
    if (name == "MetalFX") return static_cast<int>(M::MetalFX);
    if (name == "FSR")     return static_cast<int>(M::FSR);
    if (name == "DLSS")    return static_cast<int>(M::DLSS);
    if (name == "XeSS")    return static_cast<int>(M::XeSS);
    return -1;
}
// 1 / the per-axis ratio ffx_upscale.h gives each FFX_UPSCALE_QUALITY_MODE.
float scaleOf(const std::string& mode)
{
    if (mode == "Quality")          return 1.0f / 1.5f;
    if (mode == "Balanced")         return 1.0f / 1.7f;
    if (mode == "Performance")      return 0.5f;
    if (mode == "UltraPerformance") return 1.0f / 3.0f;
    return 1.0f;
}
std::string effectiveUpscalerName()
{
    const State& s = state();
    return s.values.upscaler.value_or(s.host.upscaler);
}
} // namespace

void install(Host host)
{
    State& s = state();
    s.host      = std::move(host);
    s.installed = true;
    applyAll({});   // buses the player never set are the project's already
}

void uninstall()
{
    State& s = state();
    s.host      = Host{};
    s.installed = false;
}

bool installed() { return state().installed; }

const Values& values() { return state().values; }

void set(const Values& v) { replace(sanitized(v)); }

std::string toJson(const Values& v)
{
    nlohmann::json j = nlohmann::json::object();
    if (v.stickDeadzone)         j["stickDeadzone"]         = *v.stickDeadzone;
    if (v.stickSensitivityScale) j["stickSensitivityScale"] = *v.stickSensitivityScale;
    if (v.stickInvertY)          j["stickInvertY"]          = *v.stickInvertY;
    if (v.vsync)                 j["vsync"]                 = *v.vsync;
    if (v.fullscreen)            j["fullscreen"]            = *v.fullscreen;
    if (!v.volumes.empty())
    {
        nlohmann::json vol = nlohmann::json::object();
        for (const auto& [bus, gain] : v.volumes) vol[bus] = gain;
        j["volumes"] = std::move(vol);
    }
    if (v.upscaler)              j["upscaler"]              = *v.upscaler;
    if (v.upscalerMode)          j["upscalerMode"]          = *v.upscalerMode;
    if (v.frameGeneration)       j["frameGeneration"]       = *v.frameGeneration;
    return j.dump();
}

bool fromJson(const std::string& json, Values& out)
{
    out = Values{};
    if (json.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return false;

    // A key of the wrong type is skipped, not fatal: one damaged value must not
    // cost the player every other setting they made.
    auto num = [&](const char* key, std::optional<float>& dst)
    {
        if (auto it = j.find(key); it != j.end() && it->is_number())
            if (const float f = it->get<float>(); std::isfinite(f)) dst = f;
    };
    auto flag = [&](const char* key, std::optional<bool>& dst)
    {
        if (auto it = j.find(key); it != j.end() && it->is_boolean()) dst = it->get<bool>();
    };
    num("stickDeadzone", out.stickDeadzone);
    num("stickSensitivityScale", out.stickSensitivityScale);
    flag("stickInvertY", out.stickInvertY);
    flag("vsync", out.vsync);
    flag("fullscreen", out.fullscreen);
    if (auto it = j.find("volumes"); it != j.end() && it->is_object())
        for (const auto& [bus, gain] : it->items())
            if (!bus.empty() && gain.is_number())
                if (const float f = gain.get<float>(); std::isfinite(f)) out.volumes[bus] = f;
    // A name this build does not know is dropped; one this MACHINE cannot run
    // is kept (the player may come back to the GPU it was chosen on) and the
    // getters answer the fallback meanwhile.
    auto name = [&](const char* key, const auto& names, std::optional<std::string>& dst)
    {
        if (auto it = j.find(key); it != j.end() && it->is_string())
            if (const std::string s = it->get<std::string>(); listed(names, s)) dst = s;
    };
    name("upscaler", kUpscalers, out.upscaler);
    name("upscalerMode", kUpscalerModes, out.upscalerMode);
    if (auto it = j.find("frameGeneration"); it != j.end() && it->is_number_integer())
        out.frameGeneration = it->get<int>();
    out = sanitized(std::move(out));
    return true;
}

bool load()
{
    Ctx ctx;
    Values v;
    const bool ok = fromJson(prefs::getString(ctx, kPrefsKey, ""), v);
    if (!ok)
        HE_LOG_WARN(Script, "settings: the saved \"%s\" preference is not a JSON object — "
                    "starting from the project's settings", kPrefsKey);
    set(v);
    return ok;
}

bool save()
{
    // Nowhere to write (edit mode, a test without a sandbox): say so rather
    // than answering true for a file that was never written.
    if (fs::sandboxRoot().empty()) return false;
    Ctx ctx;
    if (isEmpty(values()))
        prefs::remove(ctx, kPrefsKey);
    else
        prefs::setString(ctx, kPrefsKey, toJson(values()));
    return true;
}

void resetToDefaults() { replace(Values{}); }

void setVolume(const std::string& bus, float volume)
{
    if (!usable(volume, "settings.setVolume")) return;
    const std::string name = bus.empty() ? std::string(kMaster) : bus;
    state().values.volumes[name] = clampVolume(volume);
    applyVolume(name);
}

float volume(const std::string& bus)
{
    const std::string name = bus.empty() ? std::string(kMaster) : bus;
    const State& s = state();
    if (auto it = s.values.volumes.find(name); it != s.values.volumes.end()) return it->second;
    return s.host.currentVolume ? s.host.currentVolume(name) : 1.0f;
}

std::vector<std::string> upscalers()
{
    const GraphicsSupport g = support();
    std::vector<std::string> out{ "Off" };
    if (g.fsr)     out.push_back("FSR");
    if (g.dlss)    out.push_back("DLSS");
    if (g.xess)    out.push_back("XeSS");
    if (g.metalFX) out.push_back("MetalFX");
    return out;
}

bool isUpscalerAvailable(const std::string& name)
{
    const std::vector<std::string> all = upscalers();
    return std::find(all.begin(), all.end(), name) != all.end();
}

void setUpscaler(const std::string& name)
{
    if (!isUpscalerAvailable(name))
    {
        HE_LOG_WARN(Script, "settings.setUpscaler: \"%s\" is not available here — ignored "
                    "(settings.upscalers lists what is)", name.c_str());
        return;
    }
    state().values.upscaler = name;
}

std::string upscaler()
{
    const std::string n = effectiveUpscalerName();
    return isUpscalerAvailable(n) ? n : std::string("Off");
}

std::vector<std::string> upscalerModes()
{
    if (upscalers().size() <= 1) return {};          // no upscaler, no modes
    if (!support().renderScale) return { "NativeAA" };
    return { std::begin(kUpscalerModes), std::end(kUpscalerModes) };
}

void setUpscalerMode(const std::string& mode)
{
    const std::vector<std::string> modes = upscalerModes();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end())
    {
        HE_LOG_WARN(Script, "settings.setUpscalerMode: \"%s\" is not available here — ignored "
                    "(settings.upscalerModes lists what is)", mode.c_str());
        return;
    }
    state().values.upscalerMode = mode;
}

std::string upscalerMode()
{
    const State& s = state();
    std::string m = s.values.upscalerMode.value_or("");
    if (m.empty())   // never chosen: the mode nearest the project's render scale
    {
        float best = 1e9f;
        for (const char* c : kUpscalerModes)
            if (const float d = std::fabs(scaleOf(c) - s.host.renderScale); d < best) { best = d; m = c; }
    }
    const std::vector<std::string> modes = upscalerModes();
    return std::find(modes.begin(), modes.end(), m) != modes.end() ? m : std::string("NativeAA");
}

// The most frames per rendered one available NOW: frame generation belongs to the upscaler — FSR's with FSR, NVIDIA
// DLSS-G with DLSS (up to 4x on GPUs with multi frame generation).
static int frameGenerationMax()
{
    const std::string u = upscaler();
    if (u == "FSR")  return std::clamp(support().maxFrameGeneration, 1, kFrameGenMax);
    if (u == "DLSS") return std::clamp(support().maxFrameGenerationDLSS, 1, kFrameGenMax);
    return 1;
}

std::vector<int> frameGenerationModes()
{
    std::vector<int> out;
    for (int n = 2; n <= frameGenerationMax(); ++n) out.push_back(n);
    return out;
}

bool isFrameGenerationAvailable() { return frameGenerationMax() > 1; }

void setFrameGeneration(int multiplier)
{
    if (multiplier > 1 && multiplier > frameGenerationMax())
    {
        HE_LOG_WARN(Script, "settings.setFrameGeneration: %dx is not available here — ignored "
                    "(settings.frameGenerationModes lists what is; it needs FSR or DLSS as the upscaler)", multiplier);
        return;
    }
    state().values.frameGeneration = std::max(multiplier, 1);
}

int frameGeneration()
{
    const int n = state().values.frameGeneration.value_or(1);
    return n <= frameGenerationMax() ? n : 1;
}

int aaMethodFor(int configuredMethod)
{
    const std::optional<std::string>& chosen = state().values.upscaler;
    if (!chosen) return configuredMethod;
    if (*chosen != "Off") return methodOf(*chosen);   // the renderer's fallback chain covers a missing one
    // Off over a project that ships an upscaler: TAA, its non-upscaling sibling.
    return configuredMethod >= static_cast<int>(HE::AAMethod::MetalFX) ? static_cast<int>(HE::AAMethod::TAA)
                                                                       : configuredMethod;
}

const char* upscalerNameOf(int aaMethod)
{
    for (const char* n : kUpscalers)
        if (methodOf(n) == aaMethod) return n;
    return "Off";
}

float renderScaleFor(float configuredScale)
{
    const std::optional<std::string>& mode = state().values.upscalerMode;
    if (!mode || effectiveUpscalerName() == "Off") return configuredScale;
    return scaleOf(*mode);
}
} // namespace settings

// ── The rows filed under their own groups ────────────────────────────────────
namespace input {
void setStickDeadzone(float deadzone)
{
    if (!settings::usable(deadzone, "input.setStickDeadzone")) return;
    settings::state().values.stickDeadzone = settings::clampDeadzone(deadzone);
    settings::applyDeadzone();
}
float stickDeadzone() { return settings::effectiveDeadzone(); }
} // namespace input

namespace camera {
void setStickSensitivityScale(float scale)
{
    if (!settings::usable(scale, "camera.setStickSensitivityScale")) return;
    settings::state().values.stickSensitivityScale = settings::clampScale(scale);
}
float stickSensitivityScale() { return settings::values().stickSensitivityScale.value_or(1.0f); }
void  setStickInvertY(bool invert) { settings::state().values.stickInvertY = invert; }
bool  stickInvertY() { return settings::values().stickInvertY.value_or(false); }
} // namespace camera

namespace app {
void setVSync(bool enabled)
{
    settings::state().values.vsync = enabled;
    settings::applyVSync();
}
bool vsync() { return settings::effectiveVSync(); }
void setFullscreen(bool fullscreen)
{
    settings::state().values.fullscreen = fullscreen;
    settings::applyFullscreen();
}
bool isFullscreen() { return settings::effectiveFullscreen(); }
} // namespace app

} // namespace HE::api
