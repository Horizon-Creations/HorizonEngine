#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/AudioListenerComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/AudioOcclusion.h>
#include <HorizonScene/AudioRoom.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/EntityActive.h>   // the "Active" switch: no sound from what is off
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
        const HE::ActiveFilter active(reg);
        for (auto [entity, src] : view.each())
        {
            if (!src.playOnStart) continue;
            if (active.off(entity)) continue;   // switched off: silent, like it is unseen
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
                    *asset, src.volume, src.pitch, src.loop,
                    x, y, z, src.innerRange, src.range, src.busName,
                    src.attenuation, src.rolloffFactor);
            }
            else
            {
                src.handle = engine.play(*asset, src.volume, src.pitch, src.loop,
                                          src.busName);
            }
        }
    }

    // Call each frame during play mode to keep listener + spatial source positions current.
    // `physics` (optional) is what the Occlusion option casts its rays in.
    static void updateSpatial(HorizonWorld& world, AudioEngine& engine, const PhysicsWorld* physics = nullptr)
    {
        if (!engine.isInitialized()) return;
        auto& reg = world.registry();
        glm::vec3 listenerPos(0.0f);
        uint32_t  listenerEntity = PhysicsWorld::kNoEntity;

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
            listenerPos    = t.position;
            listenerEntity = static_cast<uint32_t>(entity);
            break; // only first listener
        }

        // The room's reverb, a few times a second (26 rays; a room does not change faster).
        if (physics && engine.steamOptions().reflections &&
            engine.spatialBackend() == HE::AudioSpatialBackend::SteamAudio)
        {
            static uint32_t s_roomTick = 0;
            if (s_roomTick++ % 6 == 0)
            {
                const HE::RoomEstimate room = HE::estimateRoom(listenerPos,
                    [&](const glm::vec3& from, const glm::vec3& dir, float maxDist) {
                        const PhysicsWorld::RaycastHit h = physics->raycast(from, dir, maxDist, listenerEntity);
                        if (!h.hit) return -1.0f;
                        const auto* c = reg.try_get<ColliderComponent>(static_cast<entt::entity>(h.entityId));
                        return (c && c->isTrigger) ? -1.0f : h.distance;
                    });
                engine.setRoomReverb(room.reverbTimes, room.wet);
            }
        }

        // Update spatial source positions
        auto srcView = reg.view<AudioSourceComponent, TransformComponent>();
        for (auto [entity, src, t] : srcView.each())
        {
            if (!src.spatial || src.handle == 0) continue;
            engine.setSoundPosition(src.handle, t.position.x, t.position.y, t.position.z);
            if (physics && engine.steamOptions().occlusion &&
                engine.spatialBackend() == HE::AudioSpatialBackend::SteamAudio)
            {
                const uint32_t self = static_cast<uint32_t>(entity);
                const HE::OcclusionResult occ = HE::estimateOcclusion(
                    listenerPos, t.position,
                    [&](const glm::vec3& from, const glm::vec3& dir, float maxDist) {
                        const PhysicsWorld::RaycastHit h = physics->raycast(from, dir, maxDist, listenerEntity);
                        if (!h.hit || h.entityId == self) return false;
                        // A trigger volume is a sensor: sound goes through it.
                        const auto* c = reg.try_get<ColliderComponent>(static_cast<entt::entity>(h.entityId));
                        return !(c && c->isTrigger);
                    });
                engine.setSoundOcclusion(src.handle, occ.visibility, occ.transmission);
            }
        }
    }
};
