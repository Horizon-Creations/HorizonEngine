#include "Audio/AudioBusConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace HE {

const AudioBusDef* AudioBusConfig::find(const std::string& name) const
{
    for (const AudioBusDef& b : buses)
        if (b.name == name) return &b;
    return nullptr;
}

AudioBusDef* AudioBusConfig::find(const std::string& name)
{
    for (AudioBusDef& b : buses)
        if (b.name == name) return &b;
    return nullptr;
}

bool AudioBusConfig::add(const std::string& name, float volume)
{
    if (name.empty() || find(name)) return false;
    buses.push_back({ name, std::max(0.0f, volume) });
    return true;
}

bool AudioBusConfig::remove(const std::string& name)
{
    const auto it = std::find_if(buses.begin(), buses.end(),
                                 [&](const AudioBusDef& b) { return b.name == name; });
    if (it == buses.end()) return false;
    buses.erase(it);
    return true;
}

bool AudioBusConfig::isDefault() const
{
    return buses.empty() && masterVolume == 1.0f && spatialBackend == AudioSpatialBackend::Miniaudio
        && !steam.any();
}

void AudioBusConfig::toJson(nlohmann::json& out) const
{
    out["master"] = masterVolume;
    if (spatialBackend == AudioSpatialBackend::SteamAudio)
        out["spatial"] = "steamaudio";
    if (steam.any())
    {
        nlohmann::json js = nlohmann::json::object();
        if (steam.airAbsorption) js["airAbsorption"] = true;
        if (steam.occlusion)     js["occlusion"]     = true;
        if (steam.reflections)   js["reflections"]   = true;
        if (steam.pathing)       js["pathing"]       = true;
        out["steam"] = std::move(js);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const AudioBusDef& b : buses)
    {
        nlohmann::json jb = { { "name", b.name }, { "volume", b.volume } };
        // Only an authored EQ: no bands and switched on is "none", and a key
        // that says nothing would change every existing project file.
        if (!b.eq.bands.empty() || !b.eq.enabled)
            b.eq.toJson(jb["eq"]);
        arr.push_back(std::move(jb));
    }
    out["buses"] = std::move(arr);
}

void AudioBusConfig::fromJson(const nlohmann::json& in)
{
    *this = AudioBusConfig{};
    if (!in.is_object()) return;
    if (in.contains("master") && in["master"].is_number())
        masterVolume = std::max(0.0f, in["master"].get<float>());
    if (in.contains("spatial") && in["spatial"].is_string() && in["spatial"].get<std::string>() == "steamaudio")
        spatialBackend = AudioSpatialBackend::SteamAudio;
    if (in.contains("steam") && in["steam"].is_object())
    {
        auto flag = [&](const char* key) {
            const nlohmann::json& s = in["steam"];
            return s.contains(key) && s[key].is_boolean() && s[key].get<bool>();
        };
        steam.airAbsorption = flag("airAbsorption");
        steam.occlusion     = flag("occlusion");
        steam.reflections   = flag("reflections");
        steam.pathing       = flag("pathing");
    }
    if (!in.contains("buses") || !in["buses"].is_array()) return;
    for (const nlohmann::json& b : in["buses"])
    {
        if (!b.is_object() || !b.contains("name") || !b["name"].is_string()) continue;
        const float volume = b.contains("volume") && b["volume"].is_number()
                                 ? b["volume"].get<float>() : 1.0f;
        // add() drops an empty or repeated name, so a hand-edited file cannot
        // produce two groups the mixer would show as one.
        if (add(b["name"].get<std::string>(), volume))
            if (const auto eq = b.find("eq"); eq != b.end())
                buses.back().eq.fromJson(*eq);
    }
}

} // namespace HE
