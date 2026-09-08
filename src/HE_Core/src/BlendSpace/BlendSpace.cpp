#include "BlendSpace/BlendSpace.h"

#include <GraphCommon/GraphJson.h>
#include <nlohmann/json.hpp>

namespace HE
{

std::string blendSpaceToJson(const BlendSpace& s)
{
    nlohmann::json j;
    j["version"] = 1;
    j["name"]    = s.name;
    j["kind"]    = static_cast<int>(s.kind);
    j["paramX"]  = s.paramX;
    j["paramY"]  = s.paramY;
    j["minX"]    = s.minX;
    j["maxX"]    = s.maxX;
    j["minY"]    = s.minY;
    j["maxY"]    = s.maxY;
    j["looping"] = s.looping;

    nlohmann::json samples = nlohmann::json::array();
    for (const BlendSpaceSample& e : s.samples)
        samples.push_back({ { "clipId", HE::graph::uuidToJson(e.clipId) },
                            { "x", e.x }, { "y", e.y },
                            { "speedScale", e.speedScale } });
    j["samples"] = std::move(samples);

    return j.dump(2);
}

bool blendSpaceFromJson(const std::string& json, BlendSpace& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    BlendSpace s;
    s.name = j.value("name", std::string());
    // Guarded, not a blind cast — see blendSpaceKindFromInt's note in the header.
    s.kind    = blendSpaceKindFromInt(j.value("kind", 0));
    s.paramX  = j.value("paramX", std::string());
    s.paramY  = j.value("paramY", std::string());
    s.minX    = j.value("minX", 0.0f);
    s.maxX    = j.value("maxX", 1.0f);
    s.minY    = j.value("minY", 0.0f);
    s.maxY    = j.value("maxY", 1.0f);
    s.looping = j.value("looping", true);

    for (const auto& ej : j.value("samples", nlohmann::json::array()))
    {
        BlendSpaceSample e;
        if (auto c = ej.find("clipId"); c != ej.end()) e.clipId = HE::graph::uuidFromJson(*c);
        e.x = ej.value("x", 0.0f);
        e.y = ej.value("y", 0.0f);
        // A sample whose speedScale is 0 or negative would divide the weighted
        // duration by zero (or run the phase backwards for everyone else, which
        // is not what a per-sample tempo fix means). Fall back to 1 rather than
        // dropping the sample: the clip is still a legitimate pose to blend.
        e.speedScale = ej.value("speedScale", 1.0f);
        if (!(e.speedScale > 0.0f)) e.speedScale = 1.0f;
        s.samples.push_back(std::move(e));
    }

    out = std::move(s);
    return true;
}

} // namespace HE
