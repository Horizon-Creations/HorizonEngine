#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/HorizonWorld.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;

// ── EntityHost ───────────────────────────────────────────────────────────────
// Runs the HorizonCode classes that live ON scene entities — the Entity branch
// of the class taxonomy (HorizonCode.h engineClasses()). Sibling of PlayerHost,
// built the same way ScriptContext::startWorldScripts builds the Lua/Python
// side, and driven from the same places in both apps.
//
// A class reaches an entity through the ORDINARY ScriptComponent: its
// scriptAssetId may name a HorizonCodeClass asset instead of a .lua/.py script.
// There is deliberately no second "code on this entity" component — one slot,
// one walk, and the branch is on the asset's TYPE, which the ContentManager
// answers from the UUID alone.
//
// The host does NOT own the runtime: the application passes its GameInstanceHost
// runtime so entity instances share its services (widgets, createObject, engine
// calls) and its latent-node update, exactly like PlayerHost.
class EntityHost
{
public:
    // Walk `world` for entities carrying a ScriptComponent that names a
    // HorizonCode class, create one instance each (compiled backend first,
    // interpreted graph fallback) and fire Construct + BeginPlay on them.
    // Call once per play session, after content + the runtime's services exist.
    // `runtime` and `world` must outlive this host's session.
    void begin(HorizonCode::Runtime& runtime, HorizonWorld& world, ContentManager& cm);

    // Bind ONE already-existing entity to a class. Used by begin() and by
    // anything that adds a scripted entity mid-session. Returns 0 on failure
    // (no such asset, no graph). Fires Construct + BeginPlay.
    HorizonCode::InstanceId bind(Entity entity, const std::string& classPath);

    // Spawn a class that brings its OWN entity: instantiates the class asset's
    // component list (CHUNK_HCCP, a prefab-shaped subtree) into the world and
    // binds the new root to a fresh instance. Returns 0 on failure. `parent` may
    // be entt::null for the world root.
    struct Spawned { HorizonCode::InstanceId instance = 0; Entity entity = entt::null; };
    Spawned spawn(const std::string& classPath, Entity parent = entt::null);

    // Per-frame: fire Tick on every entity instance, and reap the ones whose
    // entity has gone away (see the lifetime rule in the .cpp). No-op when not
    // running.
    void tick(float dt);

    // Destroy every instance (fires Destruct) and drop all state. Idempotent;
    // begin() may be called again for the next session.
    void end();

    bool   running() const { return m_runtime != nullptr; }
    size_t count() const { return m_byEntity.size(); }

    // The instance on an entity / the entity under an instance (0 / entt::null
    // when there is none). The collision dispatch and the entity.* API rows read
    // these.
    HorizonCode::InstanceId instanceOf(Entity entity) const;
    Entity                  entityOf(HorizonCode::InstanceId instance) const;

    // entity id → instance, in the shape CollisionSystem::dispatch wants.
    using Map = std::unordered_map<uint32_t, HorizonCode::InstanceId>;
    const Map& instances() const { return m_byEntity; }

    // Destroy one binding: fires Destruct and removes the instance. The ENTITY
    // is left alone — the caller decides whether it goes too (see destroyObject
    // in the .cpp, which is the path that takes both).
    void unbind(HorizonCode::InstanceId instance);

private:
    HorizonCode::Runtime* m_runtime = nullptr;
    HorizonWorld*         m_world   = nullptr;
    ContentManager*       m_content = nullptr;
    Map                                                     m_byEntity;
    std::unordered_map<HorizonCode::InstanceId, uint32_t>   m_byInstance;
};
