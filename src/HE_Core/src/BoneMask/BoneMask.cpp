#include "BoneMask/BoneMask.h"

#include <GraphCommon/GraphJson.h>
#include <nlohmann/json.hpp>

namespace HE
{

std::string boneMaskToJson(const BoneMask& m)
{
    nlohmann::json j;
    j["version"] = 1;
    j["name"]    = m.name;

    nlohmann::json entries = nlohmann::json::array();
    for (const BoneMaskEntry& e : m.entries)
        entries.push_back({ { "joint", e.joint }, { "weight", e.weight } });
    j["entries"] = std::move(entries);

    return j.dump(2);
}

bool boneMaskFromJson(const std::string& json, BoneMask& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    BoneMask m;
    m.name = j.value("name", std::string());
    for (const auto& ej : j.value("entries", nlohmann::json::array()))
    {
        BoneMaskEntry e;
        e.joint = ej.value("joint", std::string());
        // An entry with no joint name is not a mask entry, it is a stray object —
        // and keeping it would give the resolver a name that matches nothing and
        // therefore an "unknown joint" warning about a joint nobody named.
        if (e.joint.empty()) continue;
        e.weight = ej.value("weight", 1.0f);
        m.entries.push_back(std::move(e));
    }

    out = std::move(m);
    return true;
}

} // namespace HE
