#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace HE {

namespace {

    bool localCacheMatches(const TransformComponent& t)
    {
        return t.localCacheValid
            && t.localCachePosition == t.position
            && t.localCacheRotation == t.rotation
            && t.localCacheScale    == t.scale;
    }

    // The local matrix through the cache: rebuilt (and the cache refilled) only
    // when position/rotation/scale differ from what it was built from. Only
    // propagateTransforms WRITES the cache — it is the one place that already
    // writes worldMatrix, so it adds no writer the per-entity queries race with.
    const glm::mat4& refreshLocal(TransformComponent& t)
    {
        if (!localCacheMatches(t))
        {
            t.localCache         = localMatrix(t);
            t.localCachePosition = t.position;
            t.localCacheRotation = t.rotation;
            t.localCacheScale    = t.scale;
            t.localCacheValid    = true;
        }
        return t.localCache;
    }

    void propagateFrom(entt::registry& reg, entt::entity e, const glm::mat4& parentWorld)
    {
        glm::mat4 world = parentWorld;
        if (auto* t = reg.try_get<TransformComponent>(e))
        {
            world          = parentWorld * refreshLocal(*t);
            t->worldMatrix = world;
            t->dirty       = false;
        }
        if (auto* h = reg.try_get<HierarchyComponent>(e))
            for (entt::entity child : h->children)
                propagateFrom(reg, child, world);
    }

} // namespace

glm::mat4 localMatrix(const TransformComponent& t)
{
    glm::quat q = glm::quat(glm::radians(t.rotation));
    return glm::translate(glm::mat4(1.0f), t.position)
         * glm::mat4_cast(q)
         * glm::scale(glm::mat4(1.0f), t.scale);
}

glm::mat4 cachedLocalMatrix(const TransformComponent& t)
{
    return localCacheMatches(t) ? t.localCache : localMatrix(t);
}

glm::mat4 worldMatrixOf(HorizonWorld& world, entt::entity e)
{
    entt::registry& reg = world.registry();
    if (!reg.valid(e)) return glm::mat4(1.0f);

    // Collect the chain first, then compose from the top down. Composing on the
    // way up would need the inverse order and would not match propagateFrom's
    // parentWorld * local — and "the same maths written twice" is exactly the
    // drift localMatrix exists to prevent.
    //
    // On the stack for any realistic depth: LODSystem asks this for every LOD
    // entity every tick, and a heap vector per call was a measurable part of it
    // (baseline §4.5). Deeper chains spill into the vector.
    constexpr size_t kInlineDepth = 32;
    const TransformComponent* inlineChain[kInlineDepth];
    std::vector<const TransformComponent*> spill;
    size_t depth = 0;
    for (entt::entity cur = e; cur != entt::null && reg.valid(cur); )
    {
        const TransformComponent* t = reg.try_get<TransformComponent>(cur);
        if (depth < kInlineDepth) inlineChain[depth] = t;
        else
        {
            if (spill.empty()) spill.assign(inlineChain, inlineChain + kInlineDepth);
            spill.push_back(t);
        }
        ++depth;
        const auto* h = reg.try_get<HierarchyComponent>(cur);
        entt::entity parent = h ? h->parent : entt::null;
        // The world root carries no transform of its own; stopping here also
        // ends the walk for anything parented to it.
        if (parent == entt::null || parent == world.rootEntity()) break;
        cur = parent;
    }

    const TransformComponent* const* chain = spill.empty() ? inlineChain : spill.data();
    glm::mat4 m(1.0f);
    for (size_t i = depth; i-- > 0; )
        if (const TransformComponent* t = chain[i])
            m = m * cachedLocalMatrix(*t);
    return m;
}

glm::vec3 worldPositionOf(HorizonWorld& world, entt::entity e)
{
    return glm::vec3(worldMatrixOf(world, e)[3]);
}

glm::vec3 localPositionForWorld(HorizonWorld& world, entt::entity e, const glm::vec3& worldPos)
{
    entt::registry& reg = world.registry();
    if (!reg.valid(e)) return worldPos;

    const auto* h = reg.try_get<HierarchyComponent>(e);
    const entt::entity parent = h ? h->parent : entt::null;
    if (parent == entt::null || parent == world.rootEntity() || !reg.valid(parent))
        return worldPos;   // nothing above it: local IS world

    const glm::mat4 inv = glm::inverse(worldMatrixOf(world, parent));
    return glm::vec3(inv * glm::vec4(worldPos, 1.0f));
}

void propagateTransforms(HorizonWorld& world)
{
    entt::registry& reg = world.registry();

    propagateFrom(reg, world.rootEntity(), glm::mat4(1.0f));

    // Entities outside the root hierarchy (no HierarchyComponent)
    for (auto [e, t] : reg.view<TransformComponent>(entt::exclude<HierarchyComponent>).each())
    {
        t.worldMatrix = refreshLocal(t);
        t.dirty       = false;
    }
}

} // namespace HE
