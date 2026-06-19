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

void SceneSystems::tick(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                        const glm::vec3& cameraPos, float dt, const PhysicsWorld* physics)
{
    TerrainSystem::updateTerrains(world, cm, renderer);
    AnimationSystem::update(world, cm, dt);
    AnimationBlendSystem::update(world, cm, dt);
    AnimationStateMachineSystem::update(world, cm, dt);
    PropertyAnimationSystem::update(world, cm, dt);
    NavigationSystem::update(world, dt);
    WeatherSystem::update(world, dt, cameraPos, physics); // env clouds/fog/wind + precipitation
    ParticleSystem::update(world, dt, cameraPos); // camera-following precipitation volume (Phase 2)
    FoliageSystem::update(world);
    LODSystem::update(world, cameraPos);
}
