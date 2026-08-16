#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <Types/UUID.h>

class HorizonWorld;
class ContentManager;
class IRenderer;
class PhysicsWorld;
class AnimatorHost;

namespace SceneSystems
{
    // Collect every asset UUID referenced by the world's components (mesh, material,
    // skeletal mesh, script, foliage, particles, animation clips, audio, UI image,
    // terrain heightmap, weather sound, LOD levels, state-machine states). Used by
    // the game runtime as the SEED for reference-graph streaming: only these roots
    // (and their baked transitive dependencies) are streamed, so unused assets in
    // the pak are never loaded. Duplicates are fine — the loader coalesces.
    std::vector<HE::UUID> collectAssetRefs(HorizonWorld& world);

    // Synchronously make every scene-referenced asset resident (collectAssetRefs →
    // ContentManager::ensureResident, which resolves from mounted paks or the disk
    // registry). Used by the editor after a scene load so mesh/material component
    // UUIDs render immediately after a reload/restart. Returns how many resolved.
    size_t preloadAssetRefs(HorizonWorld& world, ContentManager& cm);

    // ── The frame, in two phases ─────────────────────────────────────────────
    // These used to be one `tick`. They are split because ANIMATION HAS TO RUN
    // AFTER GAMEPLAY: a state machine reads values that gameplay code produced
    // this frame (speed, grounded, …), so running it first means it animates
    // last frame's world. The editor did exactly that — its single tick sat at
    // the TOP of the frame, before physics, before scripts — while the game ran
    // the same call at the BOTTOM. A preview that ticks in a different order
    // than the shipped game is not a preview.
    //
    // Both apps now run: gameplay → tickWorld → tickAnimation → extraction.
    // tickAnimation must stay ahead of extraction, which consumes the bone
    // matrices; moving it later only trades one frame of lag for another.

    // The world's own systems: terrain regen, navigation, weather, particles,
    // foliage and LOD. Not gated on play mode — the editor previews these while
    // authoring, which is the point of them.
    //
    // physics (optional) enables real precipitation collision via the weather
    // system's ground-height grid (downward raycasts); nullptr falls back to a
    // flat ground plane. gpuParticles = the resolved "GPU weather particles"
    // setting (toggle AND backend support). When true the CPU precipitation pool
    // is skipped and the renderer is handed the emission parameters instead.
    void tickWorld(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                   const glm::vec3& cameraPos, float dt, const PhysicsWorld* physics = nullptr,
                   bool gpuParticles = false);

    // Everything that poses a skeleton or drives a property: clip playback,
    // two-clip blend, the animator state machine, property animation. Also not
    // gated on play mode — an authored clip animates in the editor viewport.
    //
    // `sync` carries the state machines' sync graphs. It is passed down rather
    // than ticked separately because each graph must fire immediately before the
    // transitions it feeds; see AnimationStateMachineSystem::update. nullptr
    // outside a play session, where those graphs deliberately do not run.
    void tickAnimation(HorizonWorld& world, ContentManager& cm, float dt,
                       AnimatorHost* sync = nullptr);
}
