#include "Physics/CollisionLayers.h"

#include <nlohmann/json.hpp>

#include <cstdlib>

namespace HE {

namespace {

bool inRange(int index)
{
    return index >= 0 && index < CollisionLayerConfig::kCount;
}

} // namespace

CollisionLayerConfig::CollisionLayerConfig()
{
    // Everything collides with everything. This is not a placeholder — it is the
    // contract: a project that never opened the layer page must simulate exactly
    // as it did before layers existed.
    for (int a = 0; a < kCount; ++a)
        for (int b = 0; b < kCount; ++b)
            matrix[a][b] = true;

    // The presets get their names here rather than being special-cased in the UI,
    // so a log line and a combo box say the same word without either of them
    // knowing about presets.
    names[kDefault]   = "Default";
    names[kPlayer]    = "Player";
    names[kTrigger]   = "Trigger";
    names[kCharacter] = "Character";
    names[kTerrain]   = "Terrain";
}

std::string CollisionLayerConfig::layerName(int index) const
{
    if (inRange(index) && !names[index].empty())
        return names[index];
    return "Layer " + std::to_string(index);
}

void CollisionLayerConfig::setLayerName(int index, const std::string& name)
{
    if (inRange(index))
        names[index] = name;
}

bool CollisionLayerConfig::collides(int a, int b) const
{
    if (!inRange(a) || !inRange(b))
        return true;   // an unknown layer blocks nothing — see the header
    return matrix[a][b];
}

void CollisionLayerConfig::setCollides(int a, int b, bool value)
{
    if (!inRange(a) || !inRange(b))
        return;
    matrix[a][b] = value;
    matrix[b][a] = value;   // symmetry, enforced here so no caller can forget it
}

bool CollisionLayerConfig::isDefault() const
{
    CollisionLayerConfig fresh;
    for (int a = 0; a < kCount; ++a)
    {
        if (names[a] != fresh.names[a])
            return false;
        for (int b = 0; b < kCount; ++b)
            if (!matrix[a][b])
                return false;
    }
    return true;
}

void CollisionLayerConfig::toJson(nlohmann::json& out) const
{
    out = nlohmann::json::object();

    // Names: only the ones that differ from the preset defaults, keyed by index
    // as a string. An array would force sixteen entries for a project that
    // renamed one layer.
    const CollisionLayerConfig fresh;
    nlohmann::json named = nlohmann::json::object();
    for (int i = 0; i < kCount; ++i)
        if (names[i] != fresh.names[i])
            named[std::to_string(i)] = names[i];
    if (!named.empty())
        out["names"] = std::move(named);

    // Only the blocked pairs, and only once per pair (a <= b) — the matrix is
    // symmetric, so writing both halves would double the file for nothing and
    // create a way for a hand-edited file to contradict itself.
    nlohmann::json blocked = nlohmann::json::array();
    for (int a = 0; a < kCount; ++a)
        for (int b = a; b < kCount; ++b)
            if (!matrix[a][b])
                blocked.push_back(nlohmann::json::array({ a, b }));
    if (!blocked.empty())
        out["blocked"] = std::move(blocked);
}

void CollisionLayerConfig::fromJson(const nlohmann::json& in)
{
    // Start from a fresh config every time, so loading a project twice cannot
    // leave the previous project's blocked pairs behind, and so a missing or
    // malformed block means "the defaults" rather than "whatever was here".
    *this = CollisionLayerConfig{};
    if (!in.is_object())
        return;

    if (auto it = in.find("names"); it != in.end() && it->is_object())
    {
        for (auto entry = it->begin(); entry != it->end(); ++entry)
        {
            if (!entry.value().is_string())
                continue;
            const int index = std::atoi(entry.key().c_str());
            if (inRange(index))
                names[index] = entry.value().get<std::string>();
        }
    }

    if (auto it = in.find("blocked"); it != in.end() && it->is_array())
    {
        for (const auto& pair : *it)
        {
            if (!pair.is_array() || pair.size() != 2 ||
                !pair[0].is_number_integer() || !pair[1].is_number_integer())
                continue;
            // setCollides writes both cells, so a file that only names one half
            // of a pair (hand-edited, or written by a future asymmetric format)
            // still loads as a symmetric matrix.
            setCollides(pair[0].get<int>(), pair[1].get<int>(), false);
        }
    }
}

} // namespace HE
