#include "HorizonScene/EngineApi.h"
#include <Diagnostics/Logger.h>
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
bool hasQuality(const Values& v) { return v.qualityPreset.has_value() || !v.quality.empty(); }

// What the player's graphics choice comes to: the project's values (the host's
// base), the preset over them, the single values over that. `enabled` is on
// whenever the player chose anything, because the project's graphics block is
// only followed while it is; `preset` says what the numbers read as now.
struct EffectiveQuality
{
    HE::ProjectGraphics       graphics;
    HE::ProjectShadowSettings shadows;
};
EffectiveQuality effectiveQuality()
{
    const State& s = state();
    EffectiveQuality e{ s.host.baseGraphics, s.host.baseShadows };
    if (s.values.qualityPreset)
        if (HE::QualityPreset p; HE::qualityPresetFromName(*s.values.qualityPreset, p))
            HE::applyQualityPreset(p, e.graphics, e.shadows);
    for (const auto& [name, v] : s.values.quality)
        HE::setQualityField(e.graphics, e.shadows, name, v);
    if (hasQuality(s.values)) e.graphics.enabled = true;
    e.graphics.preset = HE::detectQualityPreset(e.graphics, e.shadows);
    return e;
}
void applyQuality()
{
    const State& s = state();
    if (!s.host.applyQuality) return;
    const EffectiveQuality e = effectiveQuality();
    s.host.applyQuality(e.graphics, e.shadows);
}

// The camera values have no hook: the rig controller reads values() itself,
// every frame, so there is nothing to push.
// Graphics are pushed only when the player has a choice now or had one a moment
// ago (so a reset hands the project's values back); a player who never touched
// them leaves the application's own graphics exactly as they were.
void applyAll(const std::map<std::string, float>& volumesBefore, bool hadQuality)
{
    applyDeadzone();
    applyVSync();
    applyFullscreen();
    applyVolumes(volumesBefore);
    if (hadQuality || hasQuality(state().values)) applyQuality();
}
// Replace the held values and apply the difference.
void replace(Values v)
{
    const std::map<std::string, float> before = std::move(state().values.volumes);
    const bool hadQuality = hasQuality(state().values);
    state().values = std::move(v);
    applyAll(before, hadQuality);
}

// A quality value in the form the store keeps it: the field's own clamp and
// rounding, so a hand-edited 1.4 for a three-level field cannot reach the row.
bool normalisedQuality(const std::string& field, float value, float& out)
{
    HE::ProjectGraphics g;
    HE::ProjectShadowSettings sh;
    return HE::setQualityField(g, sh, field, value) && HE::getQualityField(g, sh, field, out);
}

Values sanitized(Values v)
{
    if (v.stickDeadzone)         v.stickDeadzone         = clampDeadzone(*v.stickDeadzone);
    if (v.stickSensitivityScale) v.stickSensitivityScale = clampScale(*v.stickSensitivityScale);
    for (auto& [bus, vol] : v.volumes) vol = clampVolume(vol);
    // Graphics: a preset name the engine knows (never "Custom"), canonical spelling;
    // single values only for fields that exist, in range.
    if (v.qualityPreset)
    {
        HE::QualityPreset p;
        if (HE::qualityPresetFromName(*v.qualityPreset, p) && p != HE::QualityPreset::Custom)
            v.qualityPreset = HE::qualityPresetName(p);
        else
            v.qualityPreset.reset();
    }
    for (auto it = v.quality.begin(); it != v.quality.end();)
    {
        float n = 0.0f;
        if (normalisedQuality(it->first, it->second, n)) { it->second = n; ++it; }
        else it = v.quality.erase(it);
    }
    return v;
}

bool isEmpty(const Values& v)
{
    return !v.stickDeadzone && !v.stickSensitivityScale && !v.stickInvertY
        && !v.vsync && !v.fullscreen && v.volumes.empty() && !hasQuality(v);
}
} // namespace

void install(Host host)
{
    State& s = state();
    s.host      = std::move(host);
    s.installed = true;
    applyAll({}, false);   // buses the player never set are the project's already
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
    if (v.qualityPreset) j["qualityPreset"] = *v.qualityPreset;
    if (!v.quality.empty())
    {
        nlohmann::json q = nlohmann::json::object();
        for (const auto& [field, value] : v.quality) q[field] = value;
        j["quality"] = std::move(q);
    }
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
    // The graphics choice: sanitized() below drops a name or field the engine
    // does not know, so a file from a newer build loses only what it cannot read.
    if (auto it = j.find("qualityPreset"); it != j.end() && it->is_string())
        out.qualityPreset = it->get<std::string>();
    if (auto it = j.find("quality"); it != j.end() && it->is_object())
        for (const auto& [field, value] : it->items())
            if (value.is_number())
                if (const float f = value.get<float>(); std::isfinite(f)) out.quality[field] = f;
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

bool setQualityPreset(const std::string& name)
{
    HE::QualityPreset p;
    if (!HE::qualityPresetFromName(name, p))
    {
        HE_LOG_WARN(Script, "settings.setQualityPreset: \"%s\" is not a preset (Low, Medium, High, Ultra)",
                    name.c_str());
        return false;
    }
    // Custom is what single values read as; choosing it keeps the values as they are.
    if (p == HE::QualityPreset::Custom) return true;
    State& s = state();
    s.values.qualityPreset = HE::qualityPresetName(p);
    s.values.quality.clear();   // a preset chosen after single values replaces them
    applyQuality();
    return true;
}

std::string qualityPreset()
{
    return HE::qualityPresetName(effectiveQuality().graphics.preset);
}

std::vector<std::string> qualityPresetNames()
{
    std::vector<std::string> names;
    for (int i = 0; i < HE::kQualityPresetCount; ++i)
        names.push_back(HE::qualityPresetName(static_cast<HE::QualityPreset>(i)));
    return names;
}

bool setQualityValue(const std::string& field, float value)
{
    if (!usable(value, "settings.setQualityValue")) return false;
    float n = 0.0f;
    if (!normalisedQuality(field, value, n))
    {
        HE_LOG_WARN(Script, "settings.setQualityValue: \"%s\" is not a quality field (see qualityFields)",
                    field.c_str());
        return false;
    }
    state().values.quality[field] = n;
    applyQuality();
    return true;
}

float qualityValue(const std::string& field)
{
    const EffectiveQuality e = effectiveQuality();
    float v = 0.0f;
    if (!HE::getQualityField(e.graphics, e.shadows, field, v))
        HE_LOG_WARN(Script, "settings.qualityValue: \"%s\" is not a quality field (see qualityFields)",
                    field.c_str());
    return v;
}

std::vector<std::string> qualityFields()
{
    std::vector<std::string> names;
    for (int i = 0; i < HE::qualityFieldCount(); ++i) names.push_back(HE::qualityFieldName(i));
    return names;
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
