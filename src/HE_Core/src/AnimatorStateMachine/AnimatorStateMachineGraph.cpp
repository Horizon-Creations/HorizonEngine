#include "AnimatorStateMachine/AnimatorStateMachineGraph.h"

#include <nlohmann/json.hpp>

namespace HE
{

std::string animatorStateMachineToJson(const AnimatorStateMachineGraph& g)
{
    nlohmann::json j;
    j["version"]    = 1;
    j["startState"] = g.startState;

    for (const auto& s : g.states)
        j["states"].push_back({ { "id", s.id }, { "name", s.name },
                                 { "clipId", { { "hi", s.clipId.hi }, { "lo", s.clipId.lo } } },
                                 { "looping", s.looping }, { "x", s.x }, { "y", s.y } });
    if (g.states.empty()) j["states"] = nlohmann::json::array();

    for (const auto& t : g.transitions)
        j["transitions"].push_back({ { "fromState", t.fromState }, { "toState", t.toState },
                                      { "paramName", t.paramName }, { "op", static_cast<int>(t.op) },
                                      { "threshold", t.threshold }, { "duration", t.duration } });
    if (g.transitions.empty()) j["transitions"] = nlohmann::json::array();

    nlohmann::json params = nlohmann::json::object();
    for (const auto& [k, v] : g.defaultParams) params[k] = v;
    j["defaultParams"] = params;

    return j.dump();
}

bool animatorStateMachineFromJson(const std::string& json, AnimatorStateMachineGraph& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    AnimatorStateMachineGraph g;
    g.startState = j.value("startState", std::string());

    for (const auto& sj : j.value("states", nlohmann::json::array()))
    {
        AnimationState s;
        s.id = sj.value("id", 0);
        s.name = sj.value("name", std::string());
        if (auto c = sj.find("clipId"); c != sj.end())
        { s.clipId.hi = c->value("hi", uint64_t(0)); s.clipId.lo = c->value("lo", uint64_t(0)); }
        s.looping = sj.value("looping", true);
        s.x = sj.value("x", 0.0f);
        s.y = sj.value("y", 0.0f);
        g.states.push_back(std::move(s));
    }

    for (const auto& tj : j.value("transitions", nlohmann::json::array()))
    {
        AnimationTransition t;
        t.fromState = tj.value("fromState", std::string());
        t.toState   = tj.value("toState",   std::string());
        t.paramName = tj.value("paramName", std::string());
        t.op        = static_cast<TransitionOp>(tj.value("op", 0));
        t.threshold = tj.value("threshold", 0.5f);
        t.duration  = tj.value("duration",  0.2f);
        g.transitions.push_back(std::move(t));
    }

    if (auto p = j.find("defaultParams"); p != j.end() && p->is_object())
        for (auto it = p->begin(); it != p->end(); ++it)
            g.defaultParams[it.key()] = it.value().get<float>();

    out = std::move(g);
    return true;
}

} // namespace HE
