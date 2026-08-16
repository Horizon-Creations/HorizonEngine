#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/HorizonWorld.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;

// ── AnimatorHost ─────────────────────────────────────────────────────────────
// Owns the SYNC GRAPH instances: one per entity whose state-machine asset
// carries one. The graph's whole job is to read the character it animates and
// write the state machine's parameters, once per frame — Unreal's AnimBP
// EventGraph, in the role that matters.
//
// Sibling of EntityHost and PlayerHost, built the same way and holding the same
// kind of table. It does NOT own the runtime; the application passes its
// GameInstanceHost runtime so sync instances share the same services and latent
// update as every other graph.
//
// ── Who fires it, and why not this class ─────────────────────────────────────
// The per-frame firing does NOT happen in a tick() of this host. It happens
// inside AnimationStateMachineSystem, immediately before that entity's
// transitions are evaluated. That is the whole point: a parameter written by
// the sync graph has to be visible to the transition it feeds IN THE SAME
// FRAME, and a separate host tick would be a fourth thing to keep ordered
// across two applications — exactly the divergence that made the editor animate
// a frame late (see SceneSystems::tickAnimation).
//
// So: this class owns bind/reap, and the animation phase owns WHEN.
//
// ── Play only ────────────────────────────────────────────────────────────────
// The animation phase runs ungated in the editor so authored clips animate
// while you work. The sync graph does not: it reads gameplay state that only
// exists during play. Outside a play session no instance is bound and the
// parameters keep their authored defaults — which is exactly how state machines
// behaved before this existed.
class AnimatorHost
{
public:
    // Bind every entity in `world` whose state machine has a sync graph. Call
    // once per play session, after content and the runtime's services exist.
    // `runtime` and `world` must outlive the session.
    void begin(HorizonCode::Runtime& runtime, HorizonWorld& world, ContentManager& cm);

    // Bind ONE entity, or return the instance it already has. Returns 0 when the
    // entity has no state machine, the asset has no sync graph, or the graph
    // does not parse. Safe to call every frame — that is how an entity that
    // gains a state machine mid-session gets picked up.
    HorizonCode::InstanceId bind(Entity entity);

    // Drop one entity's instance (the entity itself is left alone). Used when a
    // component goes away or its asset changes underneath.
    void unbind(Entity entity);

    // Fire the sync graph for one entity, then copy its declared Float variables
    // into that entity's state-machine parameters. Called by
    // AnimationStateMachineSystem, once per animated entity, before its
    // transitions are evaluated. No-op when the host is not running or the
    // entity has no sync instance.
    //
    // ── Variables ARE the parameters ─────────────────────────────────────────
    // A sync graph declares what the machine reacts to, in the same place it
    // computes it — declare `speed` as a Float variable, Set it from whatever
    // the character is doing, and the transitions see it. Unreal works the same
    // way: an Animation Blueprint's variables are what its AnimGraph reads.
    //
    // The States side's default params stay useful as DEFAULTS and as what the
    // editor shows, but they are no longer the registry of what exists.
    //
    // Precedence: the copy happens AFTER the graph ran, so a variable wins over
    // an animator.setParam call made inside the same graph for the same name.
    // (setParam remains the path for Lua/Python, which have no sync graph.)
    void fireUpdate(Entity entity, float dt);

    // Destroy every instance and drop all state. Idempotent.
    void end();

    bool   running() const { return m_runtime != nullptr; }
    size_t count() const { return m_byEntity.size(); }

private:
    HorizonCode::Runtime* m_runtime = nullptr;
    HorizonWorld*         m_world   = nullptr;
    ContentManager*       m_content = nullptr;
    std::unordered_map<uint32_t, HorizonCode::InstanceId> m_byEntity;
    // Which asset each binding was made from, so an entity that swaps state
    // machines rebinds instead of running the previous asset's sync graph.
    std::unordered_map<uint32_t, HE::UUID> m_assetOf;
    // The graph's Float variable names, resolved once at bind. Per frame the
    // copy-back reads exactly these — walking a full variable snapshot every
    // frame for every animated character is not what that call is for.
    std::unordered_map<uint32_t, std::vector<std::string>> m_paramsOf;
};
