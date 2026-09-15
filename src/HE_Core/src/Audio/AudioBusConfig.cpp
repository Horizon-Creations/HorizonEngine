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
    return buses.empty() && masterVolume == 1.0f;
}

void AudioBusConfig::toJson(nlohmann::json& out) const
{
    out["master"] = masterVolume;
    nlohmann::json arr = nlohmann::json::array();
    for (const AudioBusDef& b : buses)
        arr.push_back({ { "name", b.name }, { "volume", b.volume } });
    out["buses"] = std::move(arr);
}

void AudioBusConfig::fromJson(const nlohmann::json& in)
{
    *this = AudioBusConfig{};
    if (!in.is_object()) return;
    if (in.contains("master") && in["master"].is_number())
        masterVolume = std::max(0.0f, in["master"].get<float>());
    if (!in.contains("buses") || !in["buses"].is_array()) return;
    for (const nlohmann::json& b : in["buses"])
    {
        if (!b.is_object() || !b.contains("name") || !b["name"].is_string()) continue;
        const float volume = b.contains("volume") && b["volume"].is_number()
                                 ? b["volume"].get<float>() : 1.0f;
        // add() drops an empty or repeated name, so a hand-edited file cannot
        // produce two groups the mixer would show as one.
        add(b["name"].get<std::string>(), volume);
    }
}

} // namespace HE
