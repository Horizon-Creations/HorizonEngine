#pragma once
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/HorizonWorld.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;
class PhysicsWorld;

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

    // The physics world spawned entities are given a body in (see spawn()).
    // Nullable, and null is not fatal — the host simply spawns bodiless
    // entities, which is what it did before it knew about physics at all.
    //
    // A BORROWED pointer the application owns, and deliberately NOT cleared by
    // end(): begin() calls end() first, and both applications build the physics
    // world BEFORE they start this host, so clearing it there would wipe the
    // pointer that was just handed over. The application therefore sets it every
    // time it replaces its physics world, and passes nullptr before destroying
    // one — otherwise the next spawn writes into freed memory.
    void setPhysicsWorld(PhysicsWorld* physics) { m_physics = physics; }

    // Bind every entity in `entities` that names a HorizonCode class — the
    // additive-zone counterpart of begin(), mirroring
    // ScriptContext::startScriptsFor. Without it a streamed-in zone's Entity
    // classes would never run: begin() only ever walked the world once.
    // Returns how many were bound.
    int bindFor(const std::vector<Entity>& entities);

    // Bind ONE already-existing entity to a class. Used by begin() and by
    // anything that adds a scripted entity mid-session. Returns 0 on failure
    // (no such asset, no graph). Fires Construct + BeginPlay.
    HorizonCode::InstanceId bind(Entity entity, const std::string& classPath);

    // Spawn a class that brings its OWN entity: instantiates the class asset's
    // component list (CHUNK_HCCP, a prefab-shaped subtree) into the world and
    // binds the new root to a fresh instance. Returns 0 on failure. `parent` may
    // be entt::null for the world root.
    //
    // `position` / `rotationEuler` (3 floats each, rotation in DEGREES) place the
    // new root, each independently of the other. nullptr means "leave it where
    // the class authored it" — which is why these are pointers and not values: a
    // zero vector is a placement, and a caller that never wired a location must
    // be able to say so. Applied BEFORE Construct and BeginPlay fire, so the
    // graph's first frame already sees where it stands. Spawning and moving
    // afterwards was always possible (entity.owned + transform.setPosition); it
    // just runs BeginPlay at the wrong place.
    //
    // The new subtree is also given its PHYSICS here, before Construct and
    // BeginPlay, when setPhysicsWorld() supplied a world. Same reason as the
    // placement, one step further: the first line of game logic a spawned thing
    // runs is routinely "am I grounded", "push me" or "what is under me", and
    // until this existed every one of those answered against a bodiless world.
    // The whole SUBTREE, not just the root — a PlayerCharacter arrives with
    // child entities that carry colliders of their own.
    struct Spawned { HorizonCode::InstanceId instance = 0; Entity entity = entt::null; };
    Spawned spawn(const std::string& classPath, Entity parent = entt::null,
                  const float* position = nullptr, const float* rotationEuler = nullptr);

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
    // is left alone — this is the reaper's path, where the entity is already
    // gone. The other direction (destroy the object, take its body with it)
    // is the apps' destroyObject service, which reads Runtime::ownedEntity.
    void unbind(HorizonCode::InstanceId instance);

    // The component list a class of `baseClass` STARTS with, in the same prefab
    // payload format the asset stores — a PlayerCharacter arrives with a
    // character controller and a collider rather than as a bare transform
    // nobody can move or hit.
    //
    // Built here rather than checked in as a blob literal so it follows the
    // components themselves: a field added to CharacterControllerComponent
    // shows up in the next class created, with no fixture to regenerate. Empty
    // for a base class with no body of its own (Object, PlayerController).
    static std::vector<uint8_t> defaultComponents(const std::string& baseClass);

private:
    HorizonCode::Runtime* m_runtime = nullptr;
    HorizonWorld*         m_world   = nullptr;
    ContentManager*       m_content = nullptr;
    PhysicsWorld*         m_physics = nullptr;   // borrowed; see setPhysicsWorld
    Map                                                     m_byEntity;
    std::unordered_map<HorizonCode::InstanceId, uint32_t>   m_byInstance;
    // Reused per frame so the tick pass can iterate a snapshot without an
    // allocation each time (see tick() for why it may not walk the live map).
    std::vector<HorizonCode::InstanceId>                    m_tickScratch;
};
