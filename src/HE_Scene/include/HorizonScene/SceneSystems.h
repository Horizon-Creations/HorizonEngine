#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <Types/UUID.h>

class HorizonWorld;
class ContentManager;
class IRenderer;
class PhysicsWorld;

namespace SceneSystems
{
    // Collect every asset UUID referenced by the world's components (mesh, material,
    // skeletal mesh, script, foliage, particles, animation clips, audio, UI image,
    // terrain heightmap, weather sound, LOD levels, state-machine states). Used by
    // the game runtime as the SEED for reference-graph streaming: only these roots
    // (and their baked transitive dependencies) are streamed, so unused assets in
    // the pak are never loaded. Duplicates are fine — the loader coalesces.
    std::vector<HE::UUID> collectAssetRefs(HorizonWorld& world);

    // Runs the always-on visual / gameplay systems for one frame: terrain regen,
    // skeletal animation (clip / blend / state-machine / property), navigation,
    // weather, particles, foliage and LOD. Shared by the editor (preview every frame)
    // and the standalone game runtime so weather & friends behave identically in both.
    // Physics, scripts and audio are stepped separately by the caller — the editor gates
    // those on play-mode; the game runs them every frame.
    // physics (optional) enables real precipitation collision via the weather system's
    // ground-height grid (downward raycasts); nullptr falls back to a flat ground plane.
    // gpuParticles = the resolved "GPU weather particles" setting (toggle AND backend
    // support). When true the CPU precipitation pool is skipped and the renderer is
    // handed the emission parameters to simulate + draw rain/snow on the GPU instead.
    void tick(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
              const glm::vec3& cameraPos, float dt, const PhysicsWorld* physics = nullptr,
              bool gpuParticles = false);
}
