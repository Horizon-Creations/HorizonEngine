#include "HorizonScene/LODSystem.h"
#include <cstdint>
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/TransformHierarchy.h"   // worldPositionOf — see update()
#include <Diagnostics/EngineProfiler.h>
#include <glm/glm.hpp>

// Finer-level switch happens at this fraction of the level's maxDistance (see update()).
static constexpr float kLodFinerHysteresis = 0.95f;

void LODSystem::update(HorizonWorld& world, const glm::vec3& cameraPos)
{
    auto& registry = world.registry();

    // The last parent's world matrix. LOD entities come in runs of siblings (a
    // terrain's chunk grid is created in one go), and nothing in this function
    // moves a transform, so the parent chain is composed once per run instead of
    // once per chunk (SCN-402). Exact: worldMatrixOf folds the chain top-down
    // from the identity, so worldMatrixOf(parent) * local is the same product,
    // in the same order, that worldMatrixOf(child) would compute.
    entt::entity parentSeen     = entt::null;
    bool         parentComposed = false;   // parentWorld holds parentSeen's matrix
    glm::mat4    parentWorld { 1.0f };

    auto view = registry.view<LODComponent, MeshComponent>();
    for (auto [entity, lod, mesh] : view.each())
    {
        if (lod.levels.empty()) continue;

        // Use the WORLD position, not the local one — terrain chunks are children,
        // so their local position is a per-chunk offset; only the world position
        // gives the right per-chunk camera distance.
        //
        // Composed from the parent chain rather than read out of tc.worldMatrix.
        // That matrix is only as fresh as the last propagateTransforms, and NOTHING
        // in SceneSystems::tickWorld runs one: for an entity created this frame it
        // is still the identity. Terrain is exactly that case, and it is not a
        // corner one — TerrainSystem builds the chunk entities at the TOP of
        // tickWorld and LOD picks their level at the BOTTOM of the same tick, so a
        // fresh chunk grid would measure every chunk from the world origin and
        // settle on one level for all of them, then pop a frame later.
        //
        // Identity-safe: an entity without a TransformComponent answers (0,0,0),
        // the same fallback this had before.
        const auto*        h      = registry.try_get<HierarchyComponent>(entity);
        const entt::entity parent = h ? h->parent : entt::null;
        glm::vec3 pos;
        if (parent == entt::null || parent == world.rootEntity())
        {
            // Top level: the walk would be this one product (identity times the
            // local matrix), so it is written out instead of walked.
            const auto* tc = registry.try_get<TransformComponent>(entity);
            pos = tc ? glm::vec3((glm::mat4(1.0f) * HE::cachedLocalMatrix(*tc))[3]) : glm::vec3(0.0f);
        }
        else if (parent != parentSeen)
        {
            // A new parent may be a one-off (LOD entities of many parents,
            // interleaved): walking the chain as before costs nothing extra, and
            // the parent's matrix is only composed once a second sibling follows.
            pos = HE::worldPositionOf(world, entity);
            parentSeen     = parent;
            parentComposed = false;
        }
        else
        {
            if (!parentComposed)
            {
                parentWorld    = HE::worldMatrixOf(world, parent);   // identity for an invalid one, as in the walk
                parentComposed = true;
            }
            const auto* tc = registry.try_get<TransformComponent>(entity);
            pos = tc ? glm::vec3((parentWorld * HE::cachedLocalMatrix(*tc))[3]) : glm::vec3(parentWorld[3]);
        }

        const float dist = glm::distance(cameraPos, pos);

        uint8_t  chosen   = static_cast<uint8_t>(lod.levels.size() - 1);
        HE::UUID chosenId = lod.levels[chosen].meshId;
        if (lod.refinedMeshId != HE::UUID{} && dist <= lod.refinedMaxDistance)
        {
            chosen   = LODComponent::kRefined;   // finer than levels[0]
            chosenId = lod.refinedMeshId;
        }
        else
        {
            for (uint8_t i = 0; i < static_cast<uint8_t>(lod.levels.size()); ++i)
            {
                if (dist <= lod.levels[i].maxDistance)
                {
                    chosen   = i;
                    chosenId = lod.levels[i].meshId;
                    break;
                }
            }
        }
        // Hysteresis towards the FINER level: a camera hovering at a ring boundary
        // flipped the chunk (and, through the seam stitcher, its four neighbours) back
        // and forth, and every flip re-copies, re-uploads and re-builds a BVH. Going
        // finer needs the camera a few percent inside the boundary; going coarser stays
        // exact, so no level is ever kept past its own maxDistance by more than the
        // band. Leaving the refined level and entering it are not touched.
        if (chosen != LODComponent::kRefined && lod.current < lod.levels.size() && chosen < lod.current &&
            lod.refinedMeshId == HE::UUID{} && dist > lod.levels[chosen].maxDistance * kLodFinerHysteresis)
        {
            chosen   = lod.current;
            chosenId = lod.levels[chosen].meshId;
        }
        if (lod.stitchedMeshId != HE::UUID{} && lod.stitchedLevel == chosen)
            chosenId = lod.stitchedMeshId;

        if (lod.current != chosen || mesh.meshAssetId != chosenId)
        {
            meshStreamCounters().lodSwitches.fetch_add(1, std::memory_order_relaxed);
            lod.current        = chosen;
            mesh.meshAssetId   = chosenId;
            mesh.dirty         = true;
        }
    }
}
