#include "HorizonScene/TrailSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/SceneSerializer.h"   // encodeBase64 / decodeBase64
#include <Diagnostics/Log.h>
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

namespace TrailSystem
{
Runtime& runtime(HorizonWorld& world)
{
    auto& ctx = world.registry().ctx();
    if (auto* r = ctx.find<Runtime>()) return *r;
    return ctx.emplace<Runtime>();
}

Runtime* tryRuntime(HorizonWorld& world)
{
    return world.registry().ctx().find<Runtime>();
}

namespace
{
HE::TrailWeather weatherOf(const WeatherComponent* w)
{
    HE::TrailWeather tw;
    if (w)
    {
        tw.precip    = w->curPrecip;
        tw.snowing   = w->curPrecipType == PrecipType::Snow;
        tw.raining   = w->curPrecipType == PrecipType::Rain;
        tw.windSpeed = w->curWindSpeed;
        tw.wetness   = w->curWetness;
    }
    return tw;
}
} // namespace

std::string toJson(HorizonWorld& world)
{
    const Runtime* rt = tryRuntime(world);
    nlohmann::json j;
    j["v"]     = 1;
    j["clock"] = rt ? rt->clock : 0.0;
    // Nothing trodden is a document too (loading it clears the world's trails: a save written on clean ground).
    if (!rt || rt->field.pageCount() == 0) { j["data"] = ""; return j.dump(); }
    const std::vector<uint8_t> blob = rt->field.serialize();
    j["data"]  = SceneSerializer::encodeBase64(blob.data(), blob.size());
    return j.dump();
}

bool fromJson(HorizonWorld& world, const std::string& json, double elapsedSeconds, const WeatherComponent* weather)
{
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1 || !j.contains("data") || !j["data"].is_string()) return false;
    const std::string data = j["data"].get<std::string>();
    Runtime& rt = runtime(world);
    if (data.empty()) { rt.field.clear(); rt.clock = j.value("clock", 0.0); return true; }
    const std::vector<uint8_t> blob = SceneSerializer::decodeBase64(data);
    // Into the live field itself (its version counter goes on: the clipmap and the refined chunks see every page as
    // changed); a refused document leaves it as it was.
    if (!rt.field.deserialize(blob.data(), blob.size())) return false;
    rt.field.catchUp(elapsedSeconds, weatherOf(weather), rt.settings);
    rt.clock = rt.field.clock();               // the healing clock goes on from where the save was written
    return true;
}

void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
            const glm::vec3& cameraPos, float dt, const WeatherComponent* weather)
{
    Runtime& rt = runtime(world);
    rt.clock += static_cast<double>(dt);
    const HE::TrailWeather tw = weatherOf(weather);
    rt.field.tick(rt.clock, tw, rt.settings);
    if (rt.clipmap.update(cm, renderer, rt.field, cameraPos) && !rt.logged && rt.field.pageCount() > 0)
    {
        rt.logged = true;   // once: the first time trodden ground reaches the texture
        HE_LOG_INFO(Scene, "TrailSystem: %zu page(s), %zu trodden texels uploaded to the clipmap texture",
                    rt.field.pageCount(), rt.field.activeTexels());
    }
}
} // namespace TrailSystem
