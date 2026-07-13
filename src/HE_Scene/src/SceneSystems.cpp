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
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/AnimatorBlendComponent.h"
#include "HorizonScene/Components/AnimatorStateMachineComponent.h"
#include "HorizonScene/Components/PropertyAnimatorComponent.h"
#include "HorizonScene/Components/AudioSourceComponent.h"
#include "HorizonScene/Components/UIImageComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "ContentManager/ContentManager.h"
#include "Renderer/IRenderer.h"
#include "Diagnostics/Profiler.h"
#include <cmath>

namespace {
// Build the GPU precipitation parameters from the EnvironmentComponent (the single
// source of truth for rain/snow, whether set by a weather preset or by hand). Pushed
// every frame so the renderer reliably idles when `active` is false — no leftover pool.
void pushGpuParticleParams(HorizonWorld& world, IRenderer* renderer,
                           const glm::vec3& cameraPos, float dt, bool active, float time)
{
    IRenderer::GpuParticleParams gp;
    auto& reg = world.registry();
    const EnvironmentComponent* env = reg.try_get<EnvironmentComponent>(world.rootEntity());
    const float rain = env ? env->rainAmount : 0.0f;
    const float snow = env ? env->snowAmount : 0.0f;
    const float amount = std::max(rain, snow);
    if (active && env && amount > 0.001f)
    {
        const bool  isSnow = (snow > rain);
        const float wr     = glm::radians(env->windDirection);
        const float fall   = isSnow ? 2.0f : 18.0f;
        const float boxTop = 24.0f;
        // Pool cap comes from the WeatherComponent budget if present, else a default.
        int cap = isSnow ? 20000 : 20000;
        for (auto [e, wc] : reg.view<WeatherComponent>().each())
        { cap = isSnow ? wc.maxSnowParticles : wc.maxRainParticles; break; }
        gp.enabled     = true;
        gp.isSnow      = isSnow;
        gp.count       = cap;
        gp.dt          = dt;
        gp.time        = time;
        gp.cameraPos   = cameraPos;
        gp.windVec     = glm::vec3(std::sin(wr), 0.0f, -std::cos(wr)) * (env->windSpeed);
        gp.coverage    = amount;
        gp.fallSpeed   = fall;
        gp.lifeSpan    = (boxTop + 60.0f) / fall;
        gp.groundLevel = 0.0f;
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
    { HE_PROFILE_SCOPE_N("Terrain");               TerrainSystem::updateTerrains(world, cm, renderer); }
    { HE_PROFILE_SCOPE_N("Animation");             AnimationSystem::update(world, cm, dt); }
    { HE_PROFILE_SCOPE_N("AnimationBlend");        AnimationBlendSystem::update(world, cm, dt); }
    { HE_PROFILE_SCOPE_N("AnimationStateMachine"); AnimationStateMachineSystem::update(world, cm, dt); }
    { HE_PROFILE_SCOPE_N("PropertyAnimation");     PropertyAnimationSystem::update(world, cm, dt); }
    { HE_PROFILE_SCOPE_N("Navigation");            NavigationSystem::update(world, dt); }
    { HE_PROFILE_SCOPE_N("Weather");               WeatherSystem::update(world, dt, cameraPos, physics, gpuParticles); } // env clouds/fog/wind + precip
    if (renderer)
    {
        HE_PROFILE_SCOPE_N("GpuParticleParams");
        // Always push (active=gpuParticles) so the GPU pool reliably stops when the
        // toggle is off or precip hits zero. Clock for the respawn hash: weatherTime
        // when a WeatherComponent exists, else a steady fallback from the frame time.
        float clock = 0.0f;
        for (auto [e, wc] : world.registry().view<WeatherComponent>().each()) { clock = wc.weatherTime; break; }
        pushGpuParticleParams(world, renderer, cameraPos, dt, gpuParticles, clock);
    }
    { HE_PROFILE_SCOPE_N("ParticleSystem"); ParticleSystem::update(world, cm, dt, cameraPos, physics); } // camera-following precipitation volume (Phase 2)
    { HE_PROFILE_SCOPE_N("Foliage");        FoliageSystem::update(world); }
    { HE_PROFILE_SCOPE_N("LOD");            LODSystem::update(world, cameraPos); }
}

std::vector<HE::UUID> SceneSystems::collectAssetRefs(HorizonWorld& world)
{
    std::vector<HE::UUID> out;
    auto& reg = world.registry();
    auto add = [&](HE::UUID id) { if (id != HE::UUID{}) out.push_back(id); };

    for (auto [e, c] : reg.view<MeshComponent>().each())            add(c.meshAssetId);
    for (auto [e, c] : reg.view<MaterialComponent>().each())        add(c.materialAssetId);
    for (auto [e, c] : reg.view<SkeletalMeshComponent>().each())    add(c.meshAssetId);
    for (auto [e, c] : reg.view<ScriptComponent>().each())          add(c.scriptAssetId);
    for (auto [e, c] : reg.view<FoliageComponent>().each())         { add(c.meshAssetId); add(c.materialAssetId); }
    for (auto [e, c] : reg.view<ParticleSystemComponent>().each())  add(c.particleAssetId);
    for (auto [e, c] : reg.view<AnimatorComponent>().each())        add(c.clipAssetId);
    for (auto [e, c] : reg.view<AnimatorBlendComponent>().each())   { add(c.clipAId); add(c.clipBId); }
    for (auto [e, c] : reg.view<PropertyAnimatorComponent>().each()) add(c.clipId);
    for (auto [e, c] : reg.view<AudioSourceComponent>().each())     add(c.assetId);
    for (auto [e, c] : reg.view<UIImageComponent>().each())         add(c.materialAssetId);
    for (auto [e, c] : reg.view<TerrainComponent>().each())         add(c.heightmapTexture);
    for (auto [e, c] : reg.view<WeatherComponent>().each())         add(c.thunderSound);
    for (auto [e, c] : reg.view<LODComponent>().each())             for (const auto& lvl : c.levels) add(lvl.meshId);
    for (auto [e, c] : reg.view<AnimatorStateMachineComponent>().each()) for (const auto& st : c.states) add(st.clipId);

    return out;
}

size_t SceneSystems::preloadAssetRefs(HorizonWorld& world, ContentManager& cm)
{
    size_t resolved = 0;
    for (HE::UUID id : collectAssetRefs(world))
        if (cm.ensureResident(id))
            ++resolved;
    return resolved;
}
