#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include <Diagnostics/Log.h>
#include <Diagnostics/Profiler.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace HE {

namespace {

    // One recursion, one multiply: the full walk and the subtree pass of the
    // incremental one both go through propagateFrom, out of line, so the product
    // that makes a world matrix is compiled once and cannot be contracted or
    // scheduled differently in one caller than in the other. That is what lets the
    // two be compared bit for bit.
#if defined(_MSC_VER)
#  define HE_TH_NOINLINE __declspec(noinline)
#else
#  define HE_TH_NOINLINE __attribute__((noinline))
#endif

    PropagateTestSwitches g_switches;

    // What one pass wrote.
    struct Work
    {
        size_t recomputed = 0;   // world matrices written
        size_t rebuilt    = 0;   // local matrices rebuilt: entities whose position/rotation/scale moved
    };

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
    const glm::mat4& refreshLocal(TransformComponent& t, Work& work)
    {
        if (!localCacheMatches(t))
        {
            t.localCache         = localMatrix(t);
            t.localCachePosition = t.position;
            t.localCacheRotation = t.rotation;
            t.localCacheScale    = t.scale;
            t.localCacheValid    = true;
            ++work.rebuilt;
        }
        return t.localCache;
    }

    HE_TH_NOINLINE void propagateFrom(entt::registry& reg, entt::entity e, const glm::mat4& parentWorld,
                                      Work& work)
    {
        glm::mat4 world = parentWorld;
        if (auto* t = reg.try_get<TransformComponent>(e))
        {
            world          = parentWorld * refreshLocal(*t, work);
            t->worldMatrix = world;
            t->dirty       = false;
            ++work.recomputed;
        }
        if (auto* h = reg.try_get<HierarchyComponent>(e))
            for (entt::entity child : h->children)
                propagateFrom(reg, child, world, work);
    }

    // The whole hierarchy, then the entities that hang off nothing.
    Work walkAll(entt::registry& reg, entt::entity root)
    {
        Work work;
        propagateFrom(reg, root, glm::mat4(1.0f), work);

        // Entities outside the root hierarchy (no HierarchyComponent)
        for (auto [e, t] : reg.view<TransformComponent>(entt::exclude<HierarchyComponent>).each())
        {
            t.worldMatrix = refreshLocal(t, work);
            t.dirty       = false;
            ++work.recomputed;
        }
        return work;
    }

    // Per registry, kept in its context: the worlds the tests build are many and
    // independent, and so is the editor's play copy.
    struct State
    {
        // The structureEpoch the stored world matrices are good for; 0 = no pass yet
        // (HorizonWorld starts its epoch at 1).
        uint64_t structureEpoch = 0;
        // Something wrote matrices or links by hand: the next pass walks everything.
        bool     forceFull  = false;
        // The last scan found so much changed that scanning first only added to the
        // walk. Skip the scan until a walk finds the world calm again.
        bool     preferFull = false;
        PropagateStats last;
        // Scratch for the scan, kept so a quiet world allocates nothing.
        std::vector<entt::entity> flagged;

        // A node that loses its TransformComponent while it still has children
        // changes what those are parented to (a child takes the grandparent's matrix),
        // and nothing about the children themselves says so. A node that is being
        // destroyed has no live children left (HorizonWorld destroys the subtree
        // bottom-up), so that case costs nothing here.
        void onTransformDestroyed(entt::registry& reg, entt::entity e)
        {
            if (g_switches.ignoreTransformRemoval) return;
            const auto* h = reg.try_get<HierarchyComponent>(e);
            if (!h) return;
            for (entt::entity child : h->children)
                if (reg.valid(child)) { forceFull = true; return; }
        }
    };

    State& stateOf(entt::registry& reg)
    {
        if (State* s = reg.ctx().find<State>()) return *s;
        State& s = reg.ctx().emplace<State>();
        // Not a scoped connection: the state lives exactly as long as the registry,
        // and a connection object that outlives the signal it points into is worse
        // than a delegate nobody disconnects.
        reg.on_destroy<TransformComponent>().connect<&State::onTransformDestroyed>(s);
        return s;
    }

    // The scan's test is a byte compare: position/rotation/scale lie side by side, and so do
    // the three values the cache was built from, so 36 bytes against 36 bytes say whether the
    // cache still describes the transform. A quarter to a third cheaper than the six float compares
    // of localCacheMatches (a standalone loop over 50k to 200k components), and it is what the scan
    // spends its time on. It can only say "changed" where localCacheMatches would say "unchanged" (+0 against
    // -0 differ in bytes, not in value; the matrix that results is the same), never the other way
    // round, except for a NaN, which compares unequal to itself by value and equal by bytes: the
    // full walk rebuilds such a local matrix on every pass and gets the same bits each time, so
    // leaving it alone changes nothing.
    static_assert(offsetof(TransformComponent, rotation) == offsetof(TransformComponent, position) + sizeof(glm::vec3) &&
                  offsetof(TransformComponent, scale)    == offsetof(TransformComponent, rotation) + sizeof(glm::vec3) &&
                  offsetof(TransformComponent, localCacheRotation) == offsetof(TransformComponent, localCachePosition) + sizeof(glm::vec3) &&
                  offsetof(TransformComponent, localCacheScale)    == offsetof(TransformComponent, localCacheRotation) + sizeof(glm::vec3),
                  "the scan compares the transform and its cache key as one block each");

    bool cacheKeyHolds(const TransformComponent& t)
    {
        const char* base = reinterpret_cast<const char*>(&t);   // the whole component, so the 36 bytes are in bounds
        return t.localCacheValid
            && std::memcmp(base + offsetof(TransformComponent, position),
                           base + offsetof(TransformComponent, localCachePosition),
                           3 * sizeof(glm::vec3)) == 0;
    }

    // The scan: every entity whose world matrix cannot be trusted. Marks them
    // dirty (the mark the subtree pass below reads and clears) and lists them.
    void scan(entt::registry& reg, State& st)
    {
        std::vector<entt::entity>& flagged = st.flagged;
        flagged.clear();
        auto& storage = reg.storage<TransformComponent>();
        if (g_switches.ignoreValueCompare)
        {
            for (auto [e, t] : storage.each())
                if (t.dirty) flagged.push_back(e);
            return;
        }
        for (auto [e, t] : storage.each())
            if (t.dirty || !cacheKeyHolds(t))
            {
                t.dirty = true;
                flagged.push_back(e);
            }
    }

    // How far up a parent chain is followed. The chain is as deep as the hierarchy
    // and a cycle cannot exist (reparentEntity refuses one), but a damaged scene is
    // not worth an endless loop.
    constexpr int kMaxChain = 1 << 16;

    // One flagged entity: recompute it and everything under it — unless an ancestor
    // is flagged as well (that one's pass writes this entity too), or it hangs off
    // nothing the full walk reaches (the full walk does not touch it either).
    //
    // The matrix the subtree starts from is the stored world matrix of the nearest
    // ancestor that has a transform. It is current: that ancestor is not flagged,
    // and nothing above it is either (the climb below looks at all of them), so
    // nothing above it changed since the pass that wrote it.
    void recomputeFlagged(entt::registry& reg, entt::entity root, entt::entity c, Work& work)
    {
        TransformComponent* t = reg.try_get<TransformComponent>(c);
        if (!t || !t->dirty) return;   // already done, by a flagged ancestor's pass

        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(c);
        if (!h)
        {
            // Outside the hierarchy: local IS world.
            t->worldMatrix = refreshLocal(*t, work);
            t->dirty       = false;
            ++work.recomputed;
            return;
        }

        glm::mat4 parentWorld(1.0f);
        bool haveParent = false;
        bool reached    = (c == root);
        entt::entity cur = h->parent;
        for (int hops = 0; !reached && hops < kMaxChain; ++hops)
        {
            if (cur == entt::null || !reg.valid(cur)) return;   // not under the root
            if (const TransformComponent* ct = reg.try_get<TransformComponent>(cur))
            {
                if (ct->dirty) return;   // a flagged ancestor: its pass covers this entity
                if (!haveParent) { parentWorld = ct->worldMatrix; haveParent = true; }
            }
            if (cur == root) { reached = true; break; }
            const HierarchyComponent* ch = reg.try_get<HierarchyComponent>(cur);
            if (!ch) return;
            cur = ch->parent;
        }
        if (!reached) return;

        propagateFrom(reg, c, parentWorld, work);
    }

    // Past this share of flagged entities the scan has stopped paying for itself.
    constexpr size_t kFlaggedDivisor = 2;   // more than 1/2 flagged
    // A walk that rebuilt no more than this share of the local matrices found the
    // world calm: scan again from the next call on.
    constexpr size_t kCalmDivisor = 4;      // at most 1/4 rebuilt

    bool envOn(const char* name)
    {
        const char* v = std::getenv(name);
        return v && *v && std::atoi(v) != 0;
    }

    // HE_PROPAGATE_FULL=1: every pass walks the whole hierarchy, as before the scan
    // existed. The way to tell whether a stale transform is the incremental pass, and a
    // same-binary A/B for the measurements.
    bool alwaysFull()
    {
        static const bool on = envOn("HE_PROPAGATE_FULL");
        return on;
    }

    // HE_PROPAGATE_VERIFY=1: after every incremental pass, walk the whole hierarchy as
    // well and compare every world matrix by its bits; a difference is logged and
    // aborts. Slow (a snapshot of all matrices and a full walk per call), for runs that
    // have to prove the skipping right on real content: the whole test suite with the
    // variable set is a check of every call the tests make.
    bool verifyOn()
    {
        static const bool on = envOn("HE_PROPAGATE_VERIFY");
        // A test that switched a detector off wants the pass to be wrong.
        return on && !g_switches.ignoreStructureEpoch && !g_switches.ignoreInvalidation
                  && !g_switches.ignoreValueCompare && !g_switches.ignoreTransformRemoval
                  && !g_switches.suspendVerify;
    }

    void verifyAgainstFullWalk(HorizonWorld& world, State& st)
    {
        entt::registry& reg = world.registry();
        std::vector<std::pair<entt::entity, glm::mat4>> kept;
        kept.reserve(reg.storage<TransformComponent>().size());
        for (auto [e, t] : reg.view<TransformComponent>().each())
            kept.emplace_back(e, t.worldMatrix);

        // The walk below must not change what the next call does.
        const PropagateStats last = st.last;
        const bool prefer = st.preferFull;
        walkAll(reg, world.rootEntity());
        st.last       = last;
        st.preferFull = prefer;

        size_t bad = 0;
        entt::entity first = entt::null;
        for (const auto& [e, m] : kept)
            if (std::memcmp(&m, &reg.get<TransformComponent>(e).worldMatrix, sizeof(glm::mat4)) != 0)
            {
                if (bad == 0) first = e;
                ++bad;
            }
        if (bad == 0) return;

        const unsigned id = static_cast<unsigned>(entt::to_integral(first));
        HE_LOG_ERROR(World, "propagateTransforms: %zu of %zu world matrices differ from the full walk "
                            "(first: entity %u; scanned %d, flagged %zu, recomputed %zu)",
                     bad, kept.size(), id, st.last.scanned ? 1 : 0, st.last.flagged, st.last.recomputed);
        std::fprintf(stderr, "propagateTransforms: %zu of %zu world matrices differ from the full walk "
                             "(first: entity %u) - HE_PROPAGATE_VERIFY\n", bad, kept.size(), id);
        std::abort();
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

void setPropagateTestSwitches(const PropagateTestSwitches& switches)
{
    g_switches = switches;
}

void invalidateWorldMatrices(HorizonWorld& world)
{
    if (g_switches.ignoreInvalidation) return;
    stateOf(world.registry()).forceFull = true;
}

PropagateStats lastPropagateStats(HorizonWorld& world)
{
    return stateOf(world.registry()).last;
}

void propagateTransformsFull(HorizonWorld& world)
{
    HE_PROFILE_SCOPE_N("Transforms::full");
    entt::registry& reg = world.registry();
    State& st = stateOf(reg);

    const Work work = walkAll(reg, world.rootEntity());

    st.structureEpoch = world.structureEpoch();
    st.forceFull      = false;
    st.last           = PropagateStats{};
    st.last.transforms = reg.storage<TransformComponent>().size();
    st.last.recomputed = work.recomputed;
    st.last.full       = true;
}

void propagateTransforms(HorizonWorld& world)
{
    HE_PROFILE_SCOPE_N("Transforms::propagate");
    entt::registry& reg = world.registry();
    State& st = stateOf(reg);
    // A pass that was not a plain full walk is what gets checked.
    struct Verify
    {
        HorizonWorld& world; State& st;
        ~Verify() { if (verifyOn() && !st.last.full) verifyAgainstFullWalk(world, st); }
    } verify{ world, st };
    const entt::entity root = world.rootEntity();
    const size_t transforms = reg.storage<TransformComponent>().size();

    // The links or the set of entities changed since the matrices were written, a
    // caller wrote matrices by hand, or there is no pass to build on: everything.
    const bool structureMoved = !g_switches.ignoreStructureEpoch
                             && st.structureEpoch != world.structureEpoch();
    if (st.structureEpoch == 0 || structureMoved || st.forceFull || alwaysFull())
    {
        propagateTransformsFull(world);
        return;
    }

    // A world the scan keeps finding mostly changed: walk it, and see whether it has
    // calmed down. (The walk rebuilds a local matrix exactly where the scan would
    // have flagged one.)
    if (st.preferFull)
    {
        HE_PROFILE_SCOPE_N("Transforms::full");
        const Work work = walkAll(reg, root);
        st.preferFull = work.rebuilt * kCalmDivisor > transforms;
        st.last = PropagateStats{};
        st.last.transforms = transforms;
        st.last.recomputed = work.recomputed;
        st.last.full       = true;
        return;
    }

    {
        HE_PROFILE_SCOPE_N("Transforms::scan");
        scan(reg, st);
    }
    PropagateStats stats;
    stats.transforms = transforms;
    stats.flagged    = st.flagged.size();
    stats.scanned    = true;

    if (st.flagged.empty())
    {
        st.last = stats;
        return;
    }

    if (st.flagged.size() * kFlaggedDivisor > transforms)
    {
        // Most of the world moved: the walk is cheaper than a climb per entity.
        HE_PROFILE_SCOPE_N("Transforms::full");
        const Work work = walkAll(reg, root);
        st.preferFull = true;
        stats.recomputed = work.recomputed;
        stats.full       = true;
        st.last = stats;
        return;
    }

    {
        HE_PROFILE_SCOPE_N("Transforms::subtrees");
        Work work;
        for (entt::entity c : st.flagged)
            recomputeFlagged(reg, root, c, work);
        stats.recomputed = work.recomputed;
    }
    st.last = stats;
}

} // namespace HE
