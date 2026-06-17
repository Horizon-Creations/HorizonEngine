#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <ContentManager/ContentManager.h>

// Iterates entities with AudioSourceComponent and triggers playback for those
// marked playOnStart=true. Call this once when entering Play mode.
struct AudioSystem
{
    static void playOnStart(HorizonWorld& world, AudioEngine& engine,
                            ContentManager* content)
    {
        if (!engine.isInitialized()) return;
        auto view = world.registry().view<AudioSourceComponent>();
        for (auto [entity, src] : view.each())
        {
            if (!src.playOnStart) continue;
            if (!content)         continue;
            const auto* asset = content->getAudio(src.assetId);
            if (!asset || asset->audioData.empty()) continue;
            engine.play(asset->audioData, asset->sampleRate, asset->channels,
                        src.volume, src.pitch, src.loop);
        }
    }
};
