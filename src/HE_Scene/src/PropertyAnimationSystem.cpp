#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "AnimationEval.h"   // advancePlayback — shared playhead rule

#include <algorithm>
#include <cmath>

bool PropertyAnimationSystem::isStepTarget(PropTarget t)
{
    return t == PropTarget::Visible;
}

// Sample a scalar keyframe track via linear interpolation.
float PropertyAnimationSystem::sampleChannel(const PropertyAnimChannel& ch, float t)
{
    if (ch.times.empty() || ch.values.empty()) return 0.0f;
    if (t <= ch.times.front()) return ch.values.front();
    if (t >= ch.times.back())  return ch.values.back();

    if (isStepTarget(ch.target))
    {
        // The last key at or before t: a key's value holds from its own instant
        // on, so landing exactly on an "off" key is already off.
        auto it = std::upper_bound(ch.times.begin(), ch.times.end(), t);
        return ch.values[static_cast<size_t>(it - ch.times.begin()) - 1];
    }

    // Binary search for bracket
    auto it = std::lower_bound(ch.times.begin(), ch.times.end(), t);
    const size_t hi = static_cast<size_t>(it - ch.times.begin());
    const size_t lo = hi - 1;

    const float tLo = ch.times[lo], tHi = ch.times[hi];
    const float alpha = (t - tLo) / (tHi - tLo);
    return ch.values[lo] + alpha * (ch.values[hi] - ch.values[lo]);
}

void PropertyAnimationSystem::advance(float& playbackTime, bool& playing,
                                      float playbackSpeed, bool looping, float duration, float dt)
{
    advancePlayback(playbackTime, playing, playbackSpeed, looping, duration, dt);
}

void PropertyAnimationSystem::applyAt(HorizonWorld& world, ContentManager& cm, entt::entity e,
                                      const PropertyAnimClipAsset& clip, float t)
{
    for (const auto& ch : clip.channels)
        applyChannel(world, cm, e, ch.target, sampleChannel(ch, t));
}

void PropertyAnimationSystem::applyChannel(HorizonWorld& world, ContentManager& cm, entt::entity e,
                                           PropTarget target, float v)
{
    auto& reg = world.registry();
    TransformComponent* tc = reg.try_get<TransformComponent>(e);
    MaterialComponent*  mc = reg.try_get<MaterialComponent>(e);

    switch (target)
    {
        // Transform channels
        case PropTarget::PosX:   if (tc) { tc->position.x = v; tc->dirty = true; } break;
        case PropTarget::PosY:   if (tc) { tc->position.y = v; tc->dirty = true; } break;
        case PropTarget::PosZ:   if (tc) { tc->position.z = v; tc->dirty = true; } break;
        case PropTarget::RotX:   if (tc) { tc->rotation.x = v; tc->dirty = true; } break;
        case PropTarget::RotY:   if (tc) { tc->rotation.y = v; tc->dirty = true; } break;
        case PropTarget::RotZ:   if (tc) { tc->rotation.z = v; tc->dirty = true; } break;
        case PropTarget::ScaleX: if (tc) { tc->scale.x    = v; tc->dirty = true; } break;
        case PropTarget::ScaleY: if (tc) { tc->scale.y    = v; tc->dirty = true; } break;
        case PropTarget::ScaleZ: if (tc) { tc->scale.z    = v; tc->dirty = true; } break;

        // Material channels — written directly to the shared MaterialAsset
        case PropTarget::MatColorR:
        case PropTarget::MatColorG:
        case PropTarget::MatColorB:
        case PropTarget::MatMetallic:
        case PropTarget::MatRoughness:
        case PropTarget::MatOpacity:
        {
            if (!mc) break;
            MaterialAsset* ma = cm.getMaterialMutable(mc->materialAssetId);
            if (!ma) break;
            switch (target)
            {
                case PropTarget::MatColorR:    ma->baseColor[0] = v; break;
                case PropTarget::MatColorG:    ma->baseColor[1] = v; break;
                case PropTarget::MatColorB:    ma->baseColor[2] = v; break;
                case PropTarget::MatMetallic:  ma->metallic     = v; break;
                case PropTarget::MatRoughness: ma->roughness    = v; break;
                case PropTarget::MatOpacity:   ma->opacity      = v; break;
                default: break;
            }
            mc->dirty = true;
            break;
        }

        // The base FOV, not fovOffset: the offset belongs to the camera rig's
        // FOV kick, and a keyed value added on top of a kick would drift.
        case PropTarget::CameraFov:
            if (auto* cam = reg.try_get<CameraComponent>(e)) cam->fovDegrees = v;
            break;

        // Every draw flag the entity has. Not InactiveComponent — that switches
        // off the script and the collider too, and an actor hidden for a shot
        // still has to be there when it reappears.
        case PropTarget::Visible:
        {
            const bool on = v >= 0.5f;
            if (auto* c = reg.try_get<MeshComponent>(e))           c->visible = on;
            if (auto* c = reg.try_get<SkeletalMeshComponent>(e))   c->visible = on;
            if (auto* c = reg.try_get<LightComponent>(e))          c->visible = on;
            if (auto* c = reg.try_get<ParticleSystemComponent>(e)) c->visible = on;
            if (auto* c = reg.try_get<RopeComponent>(e))           c->visible = on;
            if (auto* c = reg.try_get<TrailComponent>(e))          c->visible = on;
            break;
        }
    }
}

void PropertyAnimationSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<PropertyAnimatorComponent>();

    for (auto [e, pa] : view.each())
    {
        if (!pa.playing) continue;

        const PropertyAnimClipAsset* clip = cm.getPropertyAnimClip(pa.clipId);
        if (!clip || clip->duration <= 0.0f) continue;

        advancePlayback(pa.playbackTime, pa.playing,
                        pa.playbackSpeed, pa.looping, clip->duration, dt);

        applyAt(world, cm, e, *clip, pa.playbackTime);
    }
}
