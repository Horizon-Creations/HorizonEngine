#include "HorizonScene/TrailSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include <Diagnostics/Log.h>
#include <entt/entt.hpp>

namespace TrailSystem
{
Runtime& runtime(HorizonWorld& world)
{
    auto& ctx = world.registry().ctx();
    if (auto* r = ctx.find<Runtime>()) return *r;
    return ctx.emplace<Runtime>();
}

void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
            const glm::vec3& cameraPos, float dt, const WeatherComponent* weather)
{
    Runtime& rt = runtime(world);
    rt.clock += static_cast<double>(dt);
    HE::TrailWeather tw;
    if (weather)
    {
        tw.precip    = weather->curPrecip;
        tw.snowing   = weather->curPrecipType == PrecipType::Snow;
        tw.raining   = weather->curPrecipType == PrecipType::Rain;
        tw.windSpeed = weather->curWindSpeed;
        tw.wetness   = weather->curWetness;
    }
    rt.field.tick(rt.clock, tw, rt.settings);
    if (rt.clipmap.update(cm, renderer, rt.field, cameraPos) && !rt.logged && rt.field.pageCount() > 0)
    {
        rt.logged = true;   // once: the first time trodden ground reaches the texture
        HE_LOG_INFO(Scene, "TrailSystem: %zu page(s), %zu trodden texels uploaded to the clipmap texture",
                    rt.field.pageCount(), rt.field.activeTexels());
    }
}
} // namespace TrailSystem
