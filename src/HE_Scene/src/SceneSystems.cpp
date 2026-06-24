#include "HorizonScene/SceneSystems.h"
#include "HorizonScene/TerrainSystem.h"
#include "HorizonScene/AnimationSystem.h"
#include "HorizonScene/AnimationBlendSystem.h"
#include "HorizonScene/AnimationStateMachineSystem.h"
#include "HorizonScene/PropertyAnimationSystem.h"
#include "HorizonScene/NavigationSystem.h"
#include "HorizonScene/WeatherSystem.h"
#include "HorizonScene/ParticleSystem.h"
#include "HorizonScene/FoliageSystem.h"
#include "HorizonScene/LODSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "Renderer/IRenderer.h"
#include <cmath>

namespace {
// Build the GPU precipitation parameters from the (first) WeatherComponent's blended
// output. Mirrors the CPU volume's constants so the two paths look the same.
void pushGpuParticleParams(HorizonWorld& world, IRenderer* renderer,
                           const glm::vec3& cameraPos, float dt)
{
    IRenderer::GpuParticleParams gp;
    auto& reg = world.registry();
    const WeatherComponent* w = nullptr;
    for (auto [e, wc] : reg.view<WeatherComponent>().each()) { w = &wc; break; }
    if (w && w->curPrecipType != PrecipType::None && w->curPrecip > 0.001f)
    {
        const bool  isSnow  = (w->curPrecipType == PrecipType::Snow);
        float windDirDeg = 30.0f;
        if (auto* env = reg.try_get<EnvironmentComponent>(world.rootEntity()))
            windDirDeg = env->windDirection;
        const float wr     = glm::radians(windDirDeg);
        const float fall   = isSnow ? 2.0f : 18.0f;
        const float boxTop = 24.0f;
        gp.enabled     = true;
        gp.isSnow      = isSnow;
        gp.count       = isSnow ? w->maxSnowParticles : w->maxRainParticles;
        gp.dt          = dt;
        gp.time        = w->weatherTime;
        gp.cameraPos   = cameraPos;
        gp.windVec     = glm::vec3(std::sin(wr), 0.0f, -std::cos(wr)) * w->curWindSpeed;
        gp.coverage    = w->curPrecip;
        gp.fallSpeed   = fall;
        gp.lifeSpan    = (boxTop + 60.0f) / fall;
        gp.groundLevel = w->groundLevel;
        gp.boxHalf     = 16.0f;
        gp.boxTop      = boxTop;
    }
    renderer->SetGpuParticleParams(gp);   // gp.enabled == false → renderer idles / clears
}
} // namespace

void SceneSystems::tick(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                        const glm::vec3& cameraPos, float dt, const PhysicsWorld* physics,
                        bool gpuParticles)
{
    TerrainSystem::updateTerrains(world, cm, renderer);
    AnimationSystem::update(world, cm, dt);
    AnimationBlendSystem::update(world, cm, dt);
    AnimationStateMachineSystem::update(world, cm, dt);
    PropertyAnimationSystem::update(world, cm, dt);
    NavigationSystem::update(world, dt);
    WeatherSystem::update(world, dt, cameraPos, physics, gpuParticles); // env clouds/fog/wind + precip
    if (gpuParticles && renderer)
        pushGpuParticleParams(world, renderer, cameraPos, dt);          // hand rain/snow to the GPU
    ParticleSystem::update(world, dt, cameraPos); // camera-following precipitation volume (Phase 2)
    FoliageSystem::update(world);
    LODSystem::update(world, cameraPos);
}
