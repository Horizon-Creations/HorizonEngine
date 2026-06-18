#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/AudioListenerComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <glm/gtc/quaternion.hpp>

// Iterates entities with AudioSourceComponent and triggers playback for those
// marked playOnStart=true. Call this once when entering Play mode.
// Call updateSpatial() each frame to keep spatial source/listener positions current.
struct AudioSystem
{
    static void playOnStart(HorizonWorld& world, AudioEngine& engine,
                            ContentManager* content)
    {
        if (!engine.isInitialized()) return;
        auto& reg = world.registry();
        auto view = reg.view<AudioSourceComponent>();
        for (auto [entity, src] : view.each())
        {
            if (!src.playOnStart) continue;
            if (!content)         continue;
            const auto* asset = content->getAudio(src.assetId);
            if (!asset || asset->audioData.empty()) continue;

            if (src.spatial)
            {
                const auto* t = reg.try_get<TransformComponent>(entity);
                float x = t ? t->position.x : 0.0f;
                float y = t ? t->position.y : 0.0f;
                float z = t ? t->position.z : 0.0f;
                src.handle = engine.playSpatial(
                    asset->audioData, asset->sampleRate, asset->channels,
                    src.volume, src.pitch, src.loop,
                    x, y, z, src.innerRange, src.range, src.busName);
            }
            else
            {
                src.handle = engine.play(asset->audioData, asset->sampleRate,
                                          asset->channels, src.volume, src.pitch, src.loop,
                                          src.busName);
            }
        }
    }

    // Call each frame during play mode to keep listener + spatial source positions current.
    static void updateSpatial(HorizonWorld& world, AudioEngine& engine)
    {
        if (!engine.isInitialized()) return;
        auto& reg = world.registry();

        // Update listener from first AudioListenerComponent + TransformComponent
        auto listenerView = reg.view<AudioListenerComponent, TransformComponent>();
        for (auto [entity, listener, t] : listenerView.each())
        {
            // Derive forward/up from Euler rotation
            glm::quat q = glm::quat(glm::radians(t.rotation));
            glm::vec3 forward = q * glm::vec3(0.0f, 0.0f, -1.0f);
            glm::vec3 up      = q * glm::vec3(0.0f, 1.0f,  0.0f);
            engine.setListenerTransform(
                t.position.x, t.position.y, t.position.z,
                forward.x, forward.y, forward.z,
                up.x, up.y, up.z);
            break; // only first listener
        }

        // Update spatial source positions
        auto srcView = reg.view<AudioSourceComponent, TransformComponent>();
        for (auto [entity, src, t] : srcView.each())
        {
            if (!src.spatial || src.handle == 0) continue;
            engine.setSoundPosition(src.handle, t.position.x, t.position.y, t.position.z);
        }
    }
};
