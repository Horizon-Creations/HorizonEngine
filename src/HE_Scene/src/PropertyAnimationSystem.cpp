#include <HorizonScene/PropertyAnimationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <algorithm>
#include <cmath>

// Sample a scalar keyframe track via linear interpolation.
static float sampleChannel(const PropertyAnimChannel& ch, float t)
{
    if (ch.times.empty() || ch.values.empty()) return 0.0f;
    if (t <= ch.times.front()) return ch.values.front();
    if (t >= ch.times.back())  return ch.values.back();

    // Binary search for bracket
    auto it = std::lower_bound(ch.times.begin(), ch.times.end(), t);
    const size_t hi = static_cast<size_t>(it - ch.times.begin());
    const size_t lo = hi - 1;

    const float tLo = ch.times[lo], tHi = ch.times[hi];
    const float alpha = (t - tLo) / (tHi - tLo);
    return ch.values[lo] + alpha * (ch.values[hi] - ch.values[lo]);
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

        pa.playbackTime += dt * pa.playbackSpeed;
        if (pa.looping)
        {
            pa.playbackTime = std::fmod(pa.playbackTime, clip->duration);
            if (pa.playbackTime < 0.0f) pa.playbackTime += clip->duration;
        }
        else
        {
            if (pa.playbackTime >= clip->duration)
            {
                pa.playbackTime = clip->duration;
                pa.playing      = false;
            }
        }

        const float t = pa.playbackTime;

        TransformComponent* tc = reg.try_get<TransformComponent>(e);
        MaterialComponent*  mc = reg.try_get<MaterialComponent>(e);

        for (const auto& ch : clip->channels)
        {
            const float v = sampleChannel(ch, t);
            switch (ch.target)
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
                    switch (ch.target)
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
            }
        }
    }
}
