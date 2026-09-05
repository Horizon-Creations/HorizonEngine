#pragma once
#include <HorizonCode/HorizonCode.h>          // HorizonCode::Value, PinType
#include <HorizonCode/HorizonCodeRuntime.h>   // Ctx::runtime (who is calling)
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class HorizonWorld;
class PhysicsWorld;
class ContentManager;
class AudioEngine;
class EntityHost;
struct DebugLine;      // HE_Core DebugDraw.h (renderer debug-line vertex pair)
struct HeSaveServices; // HorizonGameServices.h (global scope, C ABI)

// ── HE::api ──────────────────────────────────────────────────────────────────
// The single, engine-wide C++ gameplay API. Every scripting frontend reaches the
// engine through THIS surface — Lua, Python, the HorizonCode interpreter, and the
// future HorizonCode→C++ codegen — described once in a machine-readable registry
// (see the bottom of this header). One row per function feeds all consumers, so
// "completing HorizonCode" is filling the registry, not growing an enum + switch.
// See docs/horizoncode-cpp-codegen-plan.md and docs/horizoncode-completion-plan.md.
//
// Design shape (the constraint every entry obeys, so the same signature binds a
// std::function thunk in the interpreter AND a direct call in generated C++):
//   • free functions over an explicit Ctx — no hidden globals;
//   • plain value/handle types on the boundary — no engine internals, no
//     std::function, so both frontends bind the same signature;
//   • a fixed side-effect classification (isExec) per function.
//
// Tolerance: any Ctx handle may be null — getters then return neutral defaults and
// setters/actions are no-ops (matching the old ScriptApi's forgiving contract).
//
// NB: HorizonCode has no Vec3 pin type yet, so a vec3 (position/rotation/scale,
// ray hit point/normal) is carried on the wire in a Color value's xyz (w unused).
// A dedicated Vec3 pin can replace this later without touching the C++ signatures.
namespace HE::api {

using Value   = HorizonCode::Value;
using PinType = HorizonCode::PinType;
using Entity  = uint32_t;   // raw entt handle, as scripts see it (self.entityId)

// The host handles every call needs; bundle them once instead of threading three
// pointers through every signature. Any may be null (see the tolerance note).
struct Ctx
{
    HorizonWorld*   world   = nullptr;
    PhysicsWorld*   physics = nullptr;
    ContentManager* content = nullptr;
    AudioEngine*    audio   = nullptr;
    // WHO is calling. `self` is the HorizonCode instance whose graph made this
    // call (0 outside HorizonCode — Lua, Python and native game logic have no
    // instance), and `runtime` is where that id can be looked up.
    //
    // Everything else in this struct is a world-level service; these two are the
    // caller's identity, and they exist because "which entity am I on?" cannot be
    // answered from world state alone. Both null/0 is an ordinary state, not an
    // error: the affected rows then return their neutral default like every
    // other null-Ctx call.
    HorizonCode::Runtime* runtime = nullptr;
    uint32_t              self    = 0;
    // Which HorizonCode CLASS instance sits on a given entity. The runtime
    // alone cannot answer that: several instances may own the same entity (a
    // character's class and its animator's sync graph both do), so a reverse
    // lookup there would be ambiguous by construction. EntityHost holds the one
    // that means "the class on this entity", so that is who gets asked.
    // Null outside a play session — the affected row then returns 0, like every
    // other null-Ctx call.
    EntityHost*           entities = nullptr;
    // What "quit" means to the host. A callback rather than a handle because
    // HE_Scene has no Application to call, and because the answer differs per
    // host: the packaged game closes its window, the editor stops PIE. Unset is
    // an ordinary state, like every null handle above — app::quit then says so
    // and does nothing.
    std::function<void()> requestQuit;
    // How the host makes an OBJECT of a HorizonCode class — the same service the
    // built-in Create Object / Destroy Object nodes go through
    // (HorizonCode::Runtime::Services), so the text languages reach exactly what
    // a graph reaches. A callback rather than a handle for requestQuit's reason
    // (HE_Scene has no Application to ask) and, more importantly, because the
    // host does MORE than EntityHost::spawn: it resolves the class's engine base
    // (a class deriving from an Entity class is one), sends only Entity classes
    // through EntityHost, and registers a PlayerCharacter with the PlayerHost —
    // the only point at which the PlayerHost can learn a character exists, and
    // without it a project with no controller loses its input. Reaching past
    // this callback into EntityHost would skip all three.
    //
    // position/rotationEuler are 3 floats each, or nullptr for "place it as the
    // class authored it". A zero vector is NOT the same thing — that distinction
    // is what keeps every graph with an unwired Location pin spawning where it
    // always did (see HorizonCode::Context::createObject). Rotation is Euler
    // DEGREES.
    //
    // Unset is an ordinary state, like requestQuit above: entity::spawnClass
    // then logs and returns 0.
    //
    // New members still go at the END: the two applications build their Ctx by
    // positional aggregate init. The script frontends no longer do — they assign
    // member by member through one apiCtx() builder each, which is what stopped
    // them shipping a Ctx with half its fields defaulted (eleven audio rows and
    // every runtime row were silently dead from Lua and Python because five call
    // sites each filled in the first three members and left the rest).
    std::function<uint32_t(const std::string& classPath, const float* position,
                           const float* rotationEuler)> createObject;
    std::function<void(uint32_t objectId)> destroyObject;

    // The host's WINDOW, as the three things a script may do to it, plus one for
    // the frame. Callbacks for the same reason requestQuit is one: HE_Scene
    // cannot reach an HE::Application, and the answer differs per host (a
    // packaged app owns its window outright; the editor must not let a previewed
    // graph resize the editor). Unset = the row logs once and does nothing, like
    // every other unbound service here.
    std::function<void(const std::string&)>   setWindowTitle;
    std::function<void(uint32_t, uint32_t)>   setWindowSize;
    std::function<glm::vec2()>                windowSize;
    // Draw one more frame. An event-driven application (docs/he-apps-plan.md A2)
    // sleeps until something happens, and a script changing a widget is exactly
    // the kind of "something" that carries no OS event with it.
    std::function<void()> requestRedraw;
    // The tray icon (docs/he-apps-plan.md A7). Callbacks like the window rows
    // above, and unbound in the editor for the same reason: a previewed graph
    // must not put an icon in the menu bar of the machine somebody is working on.
    std::function<void(const std::string& tooltip)>                 showTray;
    std::function<void()>                                           hideTray;
    std::function<void(const std::string& id, const std::string&)>  addTrayItem;
    std::function<void()>                                           clearTrayMenu;
    // Starting with the machine. Bound where the application knows its own name
    // and where its executable is, which is the host and nowhere else.
    std::function<bool(bool enabled)>                               setAutostart;
    std::function<bool()>                                           autostart;
    // The menu bar. One callback per operation rather than a shared "apply this
    // whole menu" so a graph can build it row by row, which is how a graph
    // builds anything.
    std::function<void(const std::string& id, const std::string& label)>       addMenu;
    std::function<void(const std::string& menuId, const std::string& id,
                       const std::string& label)>                              addMenuItem;
    std::function<void(const std::string& menuId)>                             addMenuSeparator;
    std::function<void()>                                                      clearMenuBar;
    // A notification in the system's own notification centre. Bound in the host
    // because every platform answers it somewhere else — and unbound in the
    // editor, for the reason the tray is: a previewed graph must not put a
    // banner on the screen of somebody who is working.
    std::function<bool(const std::string& title, const std::string& body)>     notify;
    std::function<bool()>                                                      notifyAvailable;
};

// ── Debug ────────────────────────────────────────────────────────────────────
void log(Ctx&, const std::string& message);

// ── Entities: identity / lifecycle / query ───────────────────────────────────
namespace entity {
    std::string getName(Ctx&, Entity e);                              // "" if invalid
    Entity      spawn(Ctx&, Entity parent, const std::string& name);  // 0 on failure
    void        destroy(Ctx&, Entity e);
    float       distance(Ctx&, Entity a, Entity b);                   // -1 if either invalid
    Entity      findByName(Ctx&, const std::string& name);            // first match, 0 if none
    bool        exists(Ctx&, Entity e);
    // ── Spawning a CLASS: the furnished entity ───────────────────────────────
    // spawn() above makes a BARE entity — a name and a transform, nothing else.
    // These make what a game actually spawns: a projectile that arrives with
    // its mesh, its collider, its rigid body and its logic already running,
    // because an author furnished the class in the editor. They instantiate a
    // HorizonCode class through the host's Create Object service
    // (Ctx::createObject — see there for why it is not EntityHost directly).
    // Components, placement, physics and the logic binding all happen BEFORE
    // the class's Construct and BeginPlay run; EntityHost::spawn documents why
    // that order is not negotiable.
    //
    // They return the ENTITY, because that is what every other row here takes:
    // a spawn can be moved, pushed and read in the next statement. The way back
    // to the HorizonCode OBJECT — for Call Function, Bind Event or a Cast — is
    // entity.instance.
    //
    // SPACE: x/y/z is where the entity ends up, full stop. Both hosts spawn a
    // class UNPARENTED, so the pose written into its transform is a world pose
    // and there is no parent for "local" to be relative to. (The rule the rest
    // of this API obeys — a position paired with an ENTITY is local unless the
    // name says World — is about entities that already sit somewhere in a
    // hierarchy. A spawn does not yet.) Rotation is Euler DEGREES.
    // spawnClass leaves the rotation the class authored, spawnClassRotated
    // states both — which is also why there are two functions rather than one
    // with optional arguments: "as authored" and "zero degrees" are different
    // requests, and a defaulted 0,0,0 could not tell them apart.
    //
    // 0 = nothing spawned, and every way of getting there is logged: no host
    // service bound, an unknown class, or a class that is not an Entity class
    // (a plain Object has no entity to hand back — Create Object is the node
    // for those, and one created that way here is destroyed again rather than
    // left alive with nothing able to name it).
    Entity spawnClass(Ctx&, const std::string& classPath, float x, float y, float z);
    Entity spawnClassRotated(Ctx&, const std::string& classPath,
                             float x, float y, float z,
                             float rx, float ry, float rz);
    // Destroy a spawned OBJECT by its reference: the class instance, the entity
    // under it and that entity's physics bodies (the host's destroyObject owns
    // that teardown — see EditorApplication/GameApplication). The built-in
    // Destroy Object node calls the same service; this row is how Lua, Python
    // and generated C++ reach it.
    //
    // entity.destroy is the other half of the pair and NOT a synonym: it takes
    // an entity and removes it, after which EntityHost reaps the orphaned
    // instance on its next tick. Destroying the object is the direct route when
    // a reference is what you are holding.
    void destroyObject(Ctx&, uint32_t objectRef);
    // Deliberately NOT here: a general entity.addComponent. It reads like the
    // obvious companion to spawn(), and the game-readiness audit asks for it, so
    // this says why it is absent rather than overlooked. It needs two things
    // this API has no place for yet: a component TYPE registry (name → emplace,
    // for every component type the engine has), and a per-type way to hand over
    // parameters — a mesh component needs its asset, a rigid body its mass and
    // motion type, a light its colour and range. That is a project of its own,
    // with the component reflection the editor's Details panel already wants.
    // Meanwhile the case it would serve ("spawn a projectile that has a mesh
    // and a body") is what spawnClass covers, from a class an author furnished
    // once instead of a call site assembling it piece by piece every time.
    // ── which entity a HorizonCode object sits on ────────────────────────────
    // An Entity-class instance is BOUND to a scene entity by EntityHost; these
    // are how a graph gets from itself (or from another object reference) to
    // that entity, which is what every transform/physics/material call takes.
    // 0 = this object owns no entity (a plain Object, a widget, a level script).
    Entity      self(Ctx&);                     // the calling instance's entity
    Entity      owned(Ctx&, uint32_t objectRef);// another object's entity
    // The HorizonCode class instance sitting on `e` (0 when there is none) —
    // the inverse of `owned`. This is what turns "Get Owning Entity" into
    // something a Cast can take: Cast works on object references, and an entity
    // is not one until you ask which object is on it.
    uint32_t    instance(Ctx&, Entity e);
    // The class instance on the CALLER's own entity — `instance(self())` in one
    // step, and the one a sync graph almost always wants: it animates a
    // character and needs that character's class to read anything the author
    // put on it. Cast takes what this returns.
    uint32_t    selfObject(Ctx&);
    // Per-entity visibility: flips every renderable component the entity carries
    // (mesh, skeletal mesh, light, particles, foliage). getVisible reads the
    // first renderable found (true when the entity has none).
    void        setVisible(Ctx&, Entity e, bool visible);
    bool        getVisible(Ctx&, Entity e);
    // ── Savegame state (SaveStateComponent + the ACTIVE save) ────────────────
    // saveState writes the component-flagged attributes under the entity's
    // stable UUID into the active save; applySavedState applies every attribute
    // PRESENT there back onto the instance (partial by design). Both require
    // play mode (PIE/packaged — the SceneSerializer owns edit-mode persistence),
    // an active save, and an enabled SaveStateComponent — anything missing
    // fails LOUD (log + false). hasSavedState only needs an active save.
    bool saveState(Ctx&, Entity e);
    bool hasSavedState(Ctx&, Entity e);
    bool applySavedState(Ctx&, Entity e);
}

// ── Transform (Euler degrees for rotation) ───────────────────────────────────
namespace transform {
    glm::vec3 getPosition(Ctx&, Entity e);                    // default (0,0,0)
    // Sets the LOCAL position — the exact value getPosition reads back, and for
    // a child the offset inside its parent — and TELEPORTS an entity that has a
    // physics representation to the world position that local one puts it at.
    //
    // Both halves are needed: the physics step writes Jolt's pose back over
    // TransformComponent every frame, so a plain transform write on a body or a
    // character was undone within the same frame and looked like nothing
    // happened. Physics deals in world poses only, so the local→world conversion
    // happens on the way there — nothing else in this API asks the caller to
    // think about it. Velocity is kept; physics.setPositionAndReset is the call
    // that says otherwise.
    //
    // This is the ONE teleport: the flat script bindings (horizon.setPosition in
    // Lua and Python) route through here rather than writing the transform, so
    // "set the position" means the same thing in every language.
    void      setPosition(Ctx&, Entity e, const glm::vec3& p);
    glm::vec3 getRotation(Ctx&, Entity e);                    // default (0,0,0)
    // Sets the LOCAL rotation and teleports a physics entity for the same reason
    // setPosition does — the step writes Jolt's orientation back too, so a turned
    // body used to snap back within the frame. The entity's own position and its
    // velocity are left as they are, and the Euler triple that comes back out of
    // getRotation is the one passed in.
    void      setRotation(Ctx&, Entity e, const glm::vec3& r);
    glm::vec3 getScale(Ctx&, Entity e);                       // default (1,1,1)
    void      setScale(Ctx&, Entity e, const glm::vec3& s);

    // The six above are LOCAL: what the entity's own transform holds, which for
    // a child is relative to its parent. These two are the world-space pair.
    //
    // For an entity with no parent the two answer the same thing, which is why
    // the distinction goes unnoticed until the first attached object: a weapon
    // parented to a hand, a chunk under a terrain, a character parented to a
    // moving platform. Asking getPosition there answers "where it sits inside
    // its parent", which is almost never the question a graph is asking.
    //
    // Composed by walking the parent chain at the moment of the call rather
    // than reading TransformComponent::worldMatrix — see TransformHierarchy.h
    // for why that cached matrix is not trustworthy from gameplay code.
    glm::vec3 getWorldPosition(Ctx&, Entity e);               // default (0,0,0)
    // Puts the entity AT `p`, whatever its parents do, and teleports a physics
    // entity there for the same reason setPosition does.
    //
    // One conversion per side, not a detour through setPosition: the transform
    // gets `p` with the parent's offset removed, physics gets `p` unchanged
    // (physics already speaks world). Converting to local and back through the
    // local setter would be the same value twice through a matrix and its
    // inverse — and this is the one call whose entire job is landing a parented
    // entity exactly where the caller said.
    void      setWorldPosition(Ctx&, Entity e, const glm::vec3& p);
}

// ── Physics (queries, forces, and the character-controller helpers) ──────────
// Everything here is a no-op / neutral default without a PhysicsWorld in the
// Ctx. WITH one, a call that cannot land (no body on that entity, wrong motion
// type) says so in the log rather than doing nothing quietly — see
// PhysicsWorld, which owns those rules.
namespace physics {
    struct RaycastHit {
        bool      hit = false;
        Entity    entity = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float     distance = 0.0f;
    };
    RaycastHit raycast(Ctx&, const glm::vec3& origin, const glm::vec3& dir, float maxDist);
    // A sphere is what a ray is not: it has width, so it does not slip through
    // the corner the caller would have hit. Same hit shape as raycast, but it
    // ignores triggers — a sweep asks what would BLOCK it, and a trigger blocks
    // nothing.
    RaycastHit sphereCast(Ctx&, const glm::vec3& origin, const glm::vec3& dir,
                          float radius, float maxDist);
    // Every entity in range, in one call: the query an explosion and a melee
    // swing are both built from. Empty without physics.
    std::vector<Entity> overlapSphere(Ctx&, const glm::vec3& center, float radius);

    // Pushing a rigid body around. A force is continuous and has to be applied
    // every frame, an impulse lands once, a torque spins. All three need a
    // DYNAMIC rigid body on the entity and answer false when there is none.
    bool addForce(Ctx&, Entity e, const glm::vec3& force);
    bool addImpulse(Ctx&, Entity e, const glm::vec3& impulse);
    bool addTorque(Ctx&, Entity e, const glm::vec3& torque);

    // Velocity in m/s — ONE pair for characters and rigid bodies. It addresses
    // the character controller when the entity has one and the rigid body
    // otherwise, which is precisely what setVelocity did while it was
    // character-only: an existing project keeps its behaviour, and a crate
    // becomes steerable through the call that was already there. The full
    // argument lives on PhysicsWorld::setVelocity.
    void       setVelocity(Ctx&, Entity e, const glm::vec3& v);
    glm::vec3  getVelocity(Ctx&, Entity e);
    bool       isGrounded(Ctx&, Entity e);

    // TELEPORT — where the entity IS, not a push towards it. Both write Jolt
    // directly and mirror the value into the transform in the same call, so the
    // camera, the render extraction and every script that reads the position
    // afterwards see one pose instead of the old one until the next step.
    // Nothing sweeps on the way: a target inside geometry stays inside it.
    //
    // `position` is LOCAL, exactly like transform.setPosition's — a deliberate
    // decision, because the two are documented below as the same operation
    // reached from two directions, and one of them meaning something else for
    // parented entities would make that sentence a trap. The rule for this whole
    // API is: a position paired with an ENTITY is local unless the name says
    // World. (PhysicsWorld underneath speaks world poses only; the conversion
    // lives in EngineApi.cpp, at the one boundary that knows both.)
    //
    // The gap that leaves, named rather than hidden: raycast/sphereCast report
    // WORLD points, so "teleport onto what I hit" needs
    // transform.setWorldPosition for a parented entity. There is no world+reset
    // variant — a graph that needs both writes the world position first and
    // stops the linear velocity with setVelocity(0) (which is less than
    // ...AndReset does: that one zeroes the angular velocity as well).
    //
    // Rotation is left alone, and the character controller wins over the rigid
    // body when an entity has both — the full argument for both rules lives on
    // PhysicsWorld::setPosition.
    //
    // ...AndReset zeroes the velocity as well, which is what a respawn wants: a
    // player put back at a checkpoint should not arrive carrying the fall that
    // killed them.
    //
    // Both answer false (and log) for an entity with no body and no character.
    // transform.setPosition is the call for those — and it performs this same
    // teleport, from the same local position, for the entities that do have
    // physics, so a graph that only ever moves things one way keeps working.
    bool setPosition(Ctx&, Entity e, const glm::vec3& position);
    bool setPositionAndReset(Ctx&, Entity e, const glm::vec3& position);

    // Does this entity have a body or a character controller at all? The guard
    // to ask before pushing, and the one honest answer to "why did my impulse do
    // nothing" — false without a PhysicsWorld too.
    bool hasPhysics(Ctx&, Entity e);

    // World gravity in m/s². Rigid bodies only — a character controller falls
    // by its own component's gravity value.
    void      setGravity(Ctx&, const glm::vec3& g);
    glm::vec3 getGravity(Ctx&);
}

// ── Animator (the state machine's parameters) ────────────────────────────────
// The state machine READS these and never writes them: a transition compares,
// nothing more. Gameplay code is the only writer, which is what makes the FSM
// steerable at all — until this existed, a parameter kept the value its asset
// defaulted to, forever, and every authored transition was unreachable.
//
// Values live on the entity's AnimatorStateMachineComponent, so two characters
// sharing one asset hold their own. Unknown entity or no state machine: setting
// is a no-op, reading gives 0 / "".
namespace animator {
    void        setParam(Ctx&, Entity e, const std::string& name, float value);
    float       getParam(Ctx&, Entity e, const std::string& name);   // 0 when unset
    std::string getState(Ctx&, Entity e);                            // "" when none
}

// ── Particles: firing an effect ──────────────────────────────────────────────
// A Particle System component used to have one control, an inspector checkbox
// that could turn it OFF. Nothing could turn one on, nothing could fire one, and
// a non-looping emitter shut itself down before its first particle existed — so
// there was no muzzle flash, no impact dust and no explosion in the engine, only
// scenery that had been running since the scene loaded.
//
// The intended shape of a hit effect is a spawned one: a class carrying a
// non-looping emitter with `destroyWhenFinished`, created at the impact point
// with Spawn Class. It plays and then takes its own entity with it, so a graph
// firing a hundred of them leaks nothing and counts nothing down by hand.
namespace particle {
    // Fire (or re-fire) the emitter from the start. A one-shot that has already
    // run needs this to run again — it is a restart, not a resume.
    void play(Ctx&, Entity e);
    // Emit nothing more; the particles already out live their lifetime. NOT the
    // same as hiding it (which keeps it simulating) or clearing `playing` (which
    // would freeze the cloud where it is).
    void stop(Ctx&, Entity e);
    // Whether the emitter is still running — either still emitting, or with
    // particles still alive. False once a one-shot has completely finished.
    bool isPlaying(Ctx&, Entity e);
    // `count` particles at once, ignoring the emit rate. Answers how many were
    // actually made: the emitter's maxParticles is a real cap, and a burst that
    // silently produced eight of the thirty asked for would be unexplainable
    // from the graph.
    int  burst(Ctx&, Entity e, int count);
}

// ── Movement: what a character is doing ──────────────────────────────────────
// READS only, and that is the whole design. These are the questions an animator
// asks — how fast, on the ground, which way relative to where I face — and the
// answers are derived from the character controller on the spot rather than
// stored a second time next to it.
//
// Split from `locomotion` below on purpose: this group is what a sync graph may
// call, and a sync graph runs in the ANIMATION phase. Anything that moves a
// character from there would be a transform write after physics has already run,
// which is the ordering bug the animation split exists to prevent.
namespace movement {
    float     speed(Ctx&, Entity e);          // horizontal metres/second
    float     verticalSpeed(Ctx&, Entity e);  // signed; negative = falling
    bool      isGrounded(Ctx&, Entity e);
    glm::vec3 velocity(Ctx&, Entity e);
    // Travel direction in the character's OWN frame: +1 forward, -1 back for
    // the first, +1 right for the second. This is what a 2D blend space wants,
    // and computing it here keeps the trigonometry out of every graph.
    float     forwardAmount(Ctx&, Entity e);
    float     rightAmount(Ctx&, Entity e);
}

// ── Locomotion: telling a character what to do ───────────────────────────────
// The write half. Belongs in a PlayerCharacter's graph, in the gameplay phase —
// deliberately NOT in a sync graph's palette.
namespace locomotion {
    void move(Ctx&, Entity e, const glm::vec3& direction);  // world space, length 0..1
    void look(Ctx&, Entity e, float yawDegrees, float pitchDegrees);
    void setMaxSpeed(Ctx&, Entity e, float metresPerSecond);
    void setOrientToMovement(Ctx&, Entity e, bool on);
    // Leave the ground. The grounded test and the jump both read the character
    // controller's own state, so `if movement.isGrounded then locomotion.jump()`
    // can never disagree with the engine — which is why there is no second
    // "canJump" row: the one that already exists IS the answer.
    //
    // Returns whether the character actually left the ground, so
    // `if (jump()) playSound()` does the obvious thing. False mid-air is an
    // ORDINARY answer and stays silent: a player holds the button, and a warning
    // per frame would bury the log. Only a call with nothing to act on — no
    // character controller on the entity — says so.
    //
    // jump() takes the speed the author tuned on the component
    // (CharacterControllerComponent::jumpSpeed); jumpWith() overrides it for the
    // one call, which is what a charged jump or a low hop through a gap needs.
    bool jump(Ctx&, Entity e);
    bool jumpWith(Ctx&, Entity e, float metresPerSecond);
}

// ── Navigation: sending an agent across the NavMesh ──────────────────────────
// The whole script surface of the pathfinder. Until these rows existed, an
// author could bake a NavMesh in the editor and then had exactly two ImGui
// buttons to start an agent with — nothing in Lua, Python or HorizonCode, and
// so no reactive AI of any kind and nothing moving at all in a packaged game.
//
// Every row addresses the entity's NavAgentComponent; NavigationSystem does the
// walking, and it steers a character controller rather than writing the
// transform, so an agent collides and falls like the player does.
//
// SPACE — decided here, once, for the whole group: a nav target is a WORLD
// position. That is a NAMED EXCEPTION to this API's otherwise universal rule
// that a position paired with an ENTITY is local unless the name says World, and
// it is deliberate: the NavMesh is baked in world space and hands its waypoints
// back in world space, so a point on it has nothing whatever to do with the
// agent's parent. Rebasing a destination onto whatever the agent happens to be
// parented to would send it somewhere nobody asked for, and every source a
// destination realistically comes from — another entity's world position, a
// raycast hit, a patrol marker — is already world. (entity.spawnClass carries
// the same exception for the same kind of reason.) The row is `moveTo` and not
// `moveToWorld` because the suffix exists to separate a world row from a LOCAL
// twin, and this group has none to be confused with: there is exactly one way to
// name a nav destination. This paragraph is what the suffix would have said.
//
// Nothing here is silent about a missing service: no agent component, no baked
// NavMesh, no route — each says which, throttled, because these are polled every
// frame and the failure that reads as "navigation is broken" is the one that
// said nothing.
namespace nav {
    // Plan a route to a WORLD point and start walking it. The search happens in
    // this call, not on the next tick, so the answer is a real one: false (and a
    // log line saying which) when the entity has no NavAgentComponent, when the
    // scene has no baked NavMesh, when either end is off it, or when no route
    // connects them. An author branches on that and barks instead of watching an
    // NPC stand still wondering why.
    //
    // A false answer leaves the agent STOPPED, and its targetPos set to the place
    // it could not reach — an NPC told to go somewhere new must not keep walking
    // to the old place as if nothing had been said, and the destination on record
    // is what the Inspector then shows the author.
    bool  moveTo(Ctx&, Entity e, float x, float y, float z);   // x/y/z are WORLD
    // Give up the current route. The agent stops where it stands; whatever
    // velocity NavigationSystem was writing is unwound by NavigationSystem on its
    // next tick, so a stopped agent does not glide on.
    void  stop(Ctx&, Entity e);
    bool  isMoving(Ctx&, Entity e);
    // Is there a route to follow? NOT the same question as isMoving, and the pair
    // is how "walking" is told apart from "was sent somewhere it cannot reach":
    // isMoving is the order that was given, this is whether there turned out to
    // be a way.
    bool  hasPath(Ctx&, Entity e);
    // Metres left along the remaining waypoints (not the straight line to the
    // target — a route around a wall is longer than the crow flies). -1 when
    // there is no path to measure, which is a different answer from 0: 0 is
    // arrival.
    float remainingDistance(Ctx&, Entity e);
    // Walking speed in m/s. Per agent, so a fleeing NPC and a patrolling one
    // share a class and not a pace. Negative is clamped to 0.
    void  setSpeed(Ctx&, Entity e, float metresPerSecond);
    // Deliberately NOT here yet: a random reachable point in a radius, which is
    // what a patrol or a wander behaviour actually wants ("go somewhere near
    // here, forever"). It needs a Detour query of its own on NavigationSystem
    // (dtNavMeshQuery::findRandomPointAroundCircle) rather than a rearrangement
    // of these rows, so it is named as the next step instead of guessed at here.
}

// ── Materials (node-graph param by name) ─────────────────────────────────────
namespace material {
    glm::vec4 getParam(Ctx&, Entity e, const std::string& name);                       // (0,0,0,0)
    bool      setParam(Ctx&, Entity e, const std::string& name, const glm::vec4& v);   // false if none
}

// ── Entity UI (entities carrying UI components) ──────────────────────────────
namespace ui {
    std::string getText(Ctx&, Entity e);
    void        setText(Ctx&, Entity e, const std::string& text);
    glm::vec4   getColor(Ctx&, Entity e);                     // default (1,1,1,1)
    void        setColor(Ctx&, Entity e, const glm::vec4& c);
    bool        getVisible(Ctx&, Entity e);
    void        setVisible(Ctx&, Entity e, bool visible);
    glm::vec2   getPosition(Ctx&, Entity e);
    void        setPosition(Ctx&, Entity e, const glm::vec2& p);
    glm::vec2   getSize(Ctx&, Entity e);
    void        setSize(Ctx&, Entity e, const glm::vec2& s);
    bool        setMaterialParam(Ctx&, Entity e, const std::string& name, const glm::vec4& v);
    // Is the pointer over an interactive UI element this frame (a live widget's
    // button, slider, text field…)? The one question a game has to ask before
    // acting on a click of its own: a press on "Start" must not also shoot.
    // False without a world, and while the mouse is captured for FPS look.
    bool        pointerOverUI(Ctx&);
}

// ── Live widgets (WidgetManager — exist OUTSIDE the entity world) ────────────
// Only the last three have a registry row. A widget's lifecycle is the job of the
// built-in Create/Show/Hide/Destroy Widget nodes (Create Widget picks its asset
// from a list, where a registry row could only take a typed path), and those nodes
// call the host's own hooks rather than coming through here. The first four stay
// anyway, as the C++ shape of the same operations — but be honest about what
// that means today: only create() has callers (two in tests), and destroy/show/
// hide have none at all since their rows went. They are kept, not used, so
// whoever finds them looking dead is looking correctly.
namespace widget {
    int  create(Ctx&, const std::string& path);   // 0 on failure
    void destroy(Ctx&, int id);
    void show(Ctx&, int id);
    void hide(Ctx&, int id);
    // The id doubles as the widget's runtime Ref (widget id == scriptId), which is
    // what the registry rows below take — see the "Live widgets" block in the .cpp.
    void setZOrder(Ctx&, int id, int z);
    bool isVisible(Ctx&, int id);
    bool callFunction(Ctx&, int id, const std::string& fn);   // PUBLIC fns only
    // Build the interface while it runs. addChild grafts a widget asset under a
    // NAMED element and returns the new instance (0 = not found), which is the
    // handle Set External / Call External / Bind Event take — a row is an object
    // like any other. Without these three a list of arbitrary length cannot be
    // written at all: every element would have to exist in the designer first.
    int  addChild(Ctx&, int id, const std::string& parentName, const std::string& assetPath);
    bool removeChild(Ctx&, int id, int childId);
    int  clearChildren(Ctx&, int id, const std::string& parentName);

    // ── Lists (docs/he-apps-plan.md B2) ─────────────────────────────────────
    // The one thing addChild cannot do well: ten thousand rows. A ListView is
    // given a COUNT and a row template and realizes only what fits on screen, so
    // this group is deliberately thin — say how many items there are, answer
    // OnRowBind for the rows it puts up, and reach a row when you need to.
    bool setListCount(Ctx&, int id, const std::string& listName, int count);
    int  listCount(Ctx&, int id, const std::string& listName);
    // The live row instance showing that item — 0 when it is scrolled out. The
    // handle Set External / Call External take, exactly like addChild's.
    int  listRow(Ctx&, int id, const std::string& listName, int index);
    bool refreshList(Ctx&, int id, const std::string& listName);
    bool setListSelected(Ctx&, int id, const std::string& listName, int index, bool selected);
    int  listSelected(Ctx&, int id, const std::string& listName);   // -1 = none
    bool scrollListToItem(Ctx&, int id, const std::string& listName, int index);

    // ── Animation (docs/he-apps-plan.md B8) ─────────────────────────────────
    // Move a property of one element to a value over `seconds`, along a named
    // curve ("Linear", "Out Quad", "Out Back"… — HE::uiEaseName). Three rows
    // rather than one: a number, a colour and a point are three different pins
    // in a graph, and a row taking "some value" would be a pin nobody can wire.
    //
    // On a themed element it is a script write stretched over time and follows
    // the same rule as any other: while it runs it wins, and the theme reclaims
    // the property at its next apply.
    bool animate(Ctx&, int id, const std::string& element, const std::string& prop,
                 float to, float seconds, const std::string& easing);
    bool animateColor(Ctx&, int id, const std::string& element, const std::string& prop,
                      const glm::vec4& to, float seconds, const std::string& easing);
    bool animateVec2(Ctx&, int id, const std::string& element, const std::string& prop,
                     const glm::vec2& to, float seconds, const std::string& easing);
    // How many were stopped. Empty `prop` stops everything on that element; the
    // value stays where it got to, because a stop is not a rewind.
    int  stopAnimation(Ctx&, int id, const std::string& element, const std::string& prop);

    // The component embedded in the slot of that name, as a REFERENCE (0 = no
    // such slot, or nothing in it). A component is another instance, so this is
    // the handle its functions, its events and its public variables are reached
    // through — the same one listRow hands out for a list row.
    uint32_t childRef(Ctx&, int id, const std::string& element);

    // ── Authored clips ──────────────────────────────────────────────────────
    // The animations the widget carries, made in the designer's timeline. By
    // NAME, because the name is what the timeline shows and what a graph can
    // type. An embedded component's clips are found too — a page can play the
    // animation its component brought with it.
    //
    // In a GRAPH, an empty Widget pin means the widget whose graph is calling —
    // playing one's own animation is the overwhelming case and should not need
    // a wire. That substitution sits on the scripting edge (the registry's
    // third post-pass), so these C++ functions still read `id` 0 as widget 0.
    // `direction` is a UIAnimDirection name ("Forward", "Backward",
    // "Ping Pong"); an unknown one plays forwards rather than nothing.
    // `restore` puts the properties the clip drove back the way they were when
    // it FINISHES — not when it is stopped, and never for a loop.
    bool playAnimation(Ctx&, int id, const std::string& clip, bool restore = false,
                       const std::string& direction = {});
    bool playAnimationLooped(Ctx&, int id, const std::string& clip, bool loop,
                             const std::string& direction = {});
    int  stopAnimationClip(Ctx&, int id, const std::string& clip);
    bool isPlayingAnimation(Ctx&, int id, const std::string& clip);
    // Everything moving in the widget, clips and single properties both — what
    // a screen being torn down reaches for without naming what it started.
    int  stopAllAnimations(Ctx&, int id);
    // Stop everything and put every property an animation ever touched back the
    // way it was before it did. Returns how many were put back.
    int  restoreOriginalState(Ctx&, int id);

    // ── Layers (docs/he-apps-plan.md B4) ────────────────────────────────────
    // A dialog, a menu, a context menu. All three are "input belongs to this
    // until it lets go"; they differ in whether the screen behind is dimmed and
    // in what makes them leave.
    void showModal(Ctx&, int id);
    // In render-target pixels, the space every coordinate here is in. The
    // pointer variant is the one a context menu wants and needs no numbers.
    void openPopup(Ctx&, int id, float x, float y);
    void openPopupAtPointer(Ctx&, int id);
    bool closeTopLayer(Ctx&);          // false = nothing was open
}

// ── Theme (docs/he-apps-plan.md D1) ──────────────────────────────────────────
// What the whole application looks like, in one place. Every element bound to a
// colour ROLE re-resolves the moment either of these is called, which is what
// makes "follow the system" or a Preferences switch one line instead of a
// reload.
namespace theme {
    bool        set(Ctx&, const std::string& assetPath);   // false = not found / unreadable
    void        setMode(Ctx&, const std::string& mode);    // "Light", "Dark" or "System"
    std::string mode(Ctx&);        // what it RESOLVED to: "Light" or "Dark"
    std::string preference(Ctx&);  // what was ASKED for: + "System"
    // The reader's text size (B10): a factor on every authored font size and on
    // nothing else. Clamped to 0.5 … 3, and fontScale gives back what it was
    // clamped TO, not what was asked for.
    void        setFontScale(Ctx&, float scale);
    float       fontScale(Ctx&);
}

// ── Cursor (host-app hook) ───────────────────────────────────────────────────
namespace cursor {
    void setVisible(Ctx&, bool show);
}

// ── Application (host-app hook) ──────────────────────────────────────────────
// Ending the session is the other half of a main menu, and the half no script
// could reach: without this a shipped game has no way to close itself. Goes
// through Ctx::requestQuit — see there for why the host supplies it.
namespace app {
    void quit(Ctx&);
    // The window an application lives in. All four are no-ops (with one warning)
    // when the host bound no window — see the callbacks on Ctx.
    void      setTitle(Ctx&, const std::string& title);
    void      setSize(Ctx&, int width, int height);
    glm::vec2 size(Ctx&);                 // logical points; (0,0) when unbound
    void      requestRedraw(Ctx&);        // draw one more frame (event-driven apps)

    // ── The tray (plan A7) ──────────────────────────────────────────────────
    // An icon in the system's tray / menu bar, with a menu of the application's
    // own entries. showTray puts it there (again with a new tooltip if it is
    // already up), hideTray takes it away, and the menu is built by adding
    // entries to it.
    //
    // An entry is an ID and a LABEL, not just a label: clicking one fires
    // OnTrayItem with the id, so translating the menu does not silently rewire
    // what its entries do.
    void showTray(Ctx&, const std::string& tooltip);
    void hideTray(Ctx&);
    void addTrayItem(Ctx&, const std::string& id, const std::string& label);
    void clearTrayMenu(Ctx&);

    // ── Starting with the machine (plan A7) ─────────────────────────────────
    // Behind the "Run other programs" permission, and not because it runs one
    // now: it asks the SYSTEM to run one, every login, without anybody being
    // there. That is the same door, and a project that has not opened it should
    // not be able to arrange it.
    void setAutostart(Ctx&, bool enabled);
    bool autostart(Ctx&);

    // ── The menu bar (plan A6) ──────────────────────────────────────────────
    // Built, not authored: addMenu opens a menu, addMenuItem fills it, and
    // clearMenuBar starts over — which is how a menu usually changes, as a set.
    // Choosing an entry fires OnMenuItem with its id, the same shape the tray
    // has.
    //
    // WHERE it appears is the platform's answer, not the application's: Windows
    // and Linux get a strip drawn along the top of the window, macOS gets the
    // system bar next to the Apple symbol (HE_Game/AppMacMenu) and no strip.
    // Same calls, same ids, same event — only WidgetManager::menuBarHeight()
    // tells them apart, and on macOS it is 0 because the bar is not in the
    // window to leave room for.
    void addMenu(Ctx&, const std::string& id, const std::string& label);
    void addMenuItem(Ctx&, const std::string& menuId, const std::string& id,
                     const std::string& label);
    void addMenuSeparator(Ctx&, const std::string& menuId);
    void clearMenuBar(Ctx&);

    // ── Notifications (plan C, Welle 3) ─────────────────────────────────────
    // A banner in the system's notification centre — what an application says
    // when it has finished something the person is no longer watching. It is
    // NOT a dialog: nothing waits for it, nothing comes back from it, and the
    // system decides whether it is shown at all (Do Not Disturb, the user's
    // settings for this app, a full screen).
    //
    // Ungated on purpose, unlike the file and process rows. A notification
    // cannot read anything, reach anywhere or start anything; the worst it can
    // do is be annoying, and that is what the system's own per-app switch is
    // for.
    //
    // True means "handed to the system", never "somebody read it".
    bool notify(Ctx&, const std::string& title, const std::string& body);
    // Can this build put one there at all? False in the editor (nothing is
    // bound), and on a Linux without notify-send. Worth asking once rather than
    // discovering it per notification.
    bool notifyAvailable(Ctx&);
}

// ── Clipboard ────────────────────────────────────────────────────────────────
// The system clipboard, as text. Already wired into the focused TextInput for
// Ctrl+C/X/V; this is the same clipboard for a graph that wants to put something
// there itself ("Copy result", "Paste from clipboard").
namespace clipboard {
    std::string getText(Ctx&);                    // "" when empty or unavailable
    void        setText(Ctx&, const std::string& text);
    bool        hasText(Ctx&);
}

// ── Native dialogs ───────────────────────────────────────────────────────────
// The blocking, OS-drawn kind. An application that has to tell the user
// something before it can go on ("unsaved changes", "that file is not readable")
// needs one that is not made of widgets, because a widget dialog cannot exist
// before the widget tree does.
namespace dialog {
    // Kind: 0 = info, 1 = warning, 2 = error. Anything else is treated as info.
    void message(Ctx&, const std::string& title, const std::string& text, int kind);
    // Returns true for the FIRST button (the affirmative one). The two labels
    // are given rather than fixed, so a graph can ask "Save"/"Discard" instead of
    // only ever "Yes"/"No".
    bool confirm(Ctx&, const std::string& title, const std::string& text,
                 const std::string& affirmative, const std::string& negative);

    // ── Picking a file or a folder ───────────────────────────────────────────
    // The native pickers, and the ONE mechanism by which a script gets at a path
    // outside its sandbox: whatever comes back is handed to fs::grantPath, so
    // the very next fs call on it works with no permission set anywhere. That is
    // the plan's model — the choosing is the permission.
    //
    // Synchronous, like message/confirm above and unlike SDL's own file dialogs,
    // which answer through a callback on some other thread. A graph pin cannot
    // hold a continuation, so these wait: they pump SDL's event queue (which is
    // what the pickers need on Linux) WITHOUT dispatching it, so the app's own
    // loop still sees every event afterwards and no script re-enters mid-frame.
    // The frame is blocked meanwhile, which is exactly what a modal is.
    //
    // "" means the person cancelled, which is a normal answer and not an error.
    // `filter` is a description and a semicolon-separated extension list in one
    // string — "Text files:txt;md" — or empty for "anything". One string because
    // a graph pin is one value, and a filter nobody can type is a filter nobody
    // uses.
    // No title: SDL's pickers do not take one, the platform supplies its own,
    // and a pin that does nothing is worse here than anywhere else — pin INDICES
    // are what a saved graph stores, so removing a dead one later would rewire
    // every graph past it. Cheaper to not have it.
    std::string openFile(Ctx&, const std::string& filter);
    std::string saveFile(Ctx&, const std::string& filter);
    std::string pickFolder(Ctx&);
}

// ── Camera (the world's main camera: isMain, else the first CameraComponent) ──
namespace camera {
    glm::vec3 getPosition(Ctx&);
    void      setPosition(Ctx&, const glm::vec3& p);
    glm::vec3 getRotation(Ctx&);                      // euler degrees
    void      setRotation(Ctx&, const glm::vec3& r);
    float     getFov(Ctx&);                           // degrees; 0 when no camera
    void      setFov(Ctx&, float degrees);

    // ── Camera rig (CameraRigComponent on the main camera) ───────────────────
    // No-ops / zeros when the main camera carries no rig, so a graph written
    // against a rig does not have to guard every call.
    void  setRigMode(Ctx&, int mode);          // 0 = first person, 1 = third person
    int   getRigMode(Ctx&);
    void  setRigTarget(Ctx&, int entityId);    // 0 / invalid = follow the possessed player
    void  setArmLength(Ctx&, float length);
    float getArmLength(Ctx&);
    void  setTargetYawMode(Ctx&, int mode);    // 0 = free, 1 = follow camera
    int   getTargetYawMode(Ctx&);

    // Where the rig is looking. getRigYaw is what makes coupled rotation usable:
    // a character that turns with the camera has to move relative to it, and
    // this is the value that makes "forward" mean forward instead of sideways.
    float getRigYaw(Ctx&);
    float getRigPitch(Ctx&);
    void  addYawPitch(Ctx&, float dYaw, float dPitch);
}

// ── Environment (the world's EnvironmentComponent) ───────────────────────────
// EVERY EnvironmentComponent field is scriptable. The X-lists below are the
// single source of truth: one row per field generates the typed get/set pair
// declared here, the implementation, the registry rows — which are what the
// HorizonCode interpreter, the editor's add-menu AND the generated-C++ path
// (hc::callApi by id) consume — and the editor display names. Adding a field
// to the component = adding one row to the matching list.
//   X(member, Name, "Display")  →  env::get<Name>/set<Name>, ids "env.get<Name>"…
// vec3 colours ride the wire as Color values (xyz, w unused), like camera/ui.
#define HE_ENV_FIELDS_FLOAT(X) \
    X(timeOfDay,           TimeOfDay,           "Time Of Day") \
    X(cycleSeconds,        CycleSeconds,        "Day Cycle Seconds") \
    X(sunIntensity,        SunIntensity,        "Sun Intensity") \
    X(moonIntensity,       MoonIntensity,       "Moon Intensity") \
    X(moonPhase,           MoonPhase,           "Moon Phase") \
    X(moonCycleDays,       MoonCycleDays,       "Moon Cycle Days") \
    X(cloudCoverage,       CloudCoverage,       "Cloud Coverage") \
    X(windDirection,       WindDirection,       "Wind Direction") \
    X(windSpeed,           WindSpeed,           "Wind Speed") \
    X(cloudHeight,         CloudHeight,         "Cloud Height") \
    X(cloudShadowStrength, CloudShadowStrength, "Cloud Shadow Strength") \
    X(cloudEvolution,      CloudEvolution,      "Cloud Evolution") \
    X(cloudDensity,        CloudDensity,        "Cloud Density") \
    X(cloudFluffiness,     CloudFluffiness,     "Cloud Fluffiness") \
    X(contrailAmount,      ContrailAmount,      "Contrail Amount") \
    X(cirrusAmount,        CirrusAmount,        "Cirrus Amount") \
    X(cirrusSeed,          CirrusSeed,          "Cirrus Seed") \
    X(godRays,             GodRays,             "God Rays") \
    X(shootingStars,       ShootingStars,       "Shooting Stars") \
    X(lensFlare,           LensFlare,           "Lens Flare") \
    X(fogDensity,          FogDensity,          "Fog Density") \
    X(fogHeightFalloff,    FogHeightFalloff,    "Fog Height Falloff") \
    X(rainAmount,          RainAmount,          "Rain Amount") \
    X(snowAmount,          SnowAmount,          "Snow Amount") \
    X(wetness,             Wetness,             "Wetness") \
    X(flash,               Flash,               "Lightning Flash") \
    X(auroraIntensity,     AuroraIntensity,     "Aurora Intensity") \
    X(milkyWayIntensity,   MilkyWayIntensity,   "Milky Way Intensity") \
    X(nebulaIntensity,     NebulaIntensity,     "Nebula Intensity") \
    X(nebulaSeed,          NebulaSeed,          "Nebula Seed") \
    X(nebulaCoverage,      NebulaCoverage,      "Nebula Coverage") \
    X(auroraHeight,        AuroraHeight,        "Aurora Height") \
    X(auroraFragmentation, AuroraFragmentation, "Aurora Fragmentation") \
    X(starBrightness,      StarBrightness,      "Star Brightness") \
    X(starSize,            StarSize,            "Star Size") \
    X(starSizeVariation,   StarSizeVariation,   "Star Size Variation") \
    X(starGlow,            StarGlow,            "Star Glow") \
    X(starTwinkle,         StarTwinkle,         "Star Twinkle") \
    X(starDensity,         StarDensity,         "Star Density")
#define HE_ENV_FIELDS_BOOL(X) \
    X(dayNightCycle,       DayNightCycle,       "Day Night Cycle") \
    X(autoAdvance,         AutoAdvance,         "Auto Advance Time") \
    X(moonPhaseAuto,       MoonPhaseAuto,       "Moon Phase Auto") \
    X(cloudShadows,        CloudShadows,        "Cloud Shadows") \
    X(cloudInterShadows,   CloudInterShadows,   "Cloud Inter Shadows") \
    X(lowResClouds,        LowResClouds,        "Low Res Clouds")
#define HE_ENV_FIELDS_INT(X) \
    X(cloudMode,           CloudMode,           "Cloud Mode") \
    X(cloudQuality,        CloudQuality,        "Cloud Quality") \
    X(cloudStyle,          CloudStyle,          "Cloud Style") \
    X(nebulaQuality,       NebulaQuality,       "Nebula Quality")
#define HE_ENV_FIELDS_COLOR(X) \
    X(sunColor,            SunColor,            "Sun Color") \
    X(moonColor,           MoonColor,           "Moon Color") \
    X(cloudTint,           CloudTint,           "Cloud Tint") \
    X(nebulaColor,         NebulaColor,         "Nebula Color 1") \
    X(nebulaColor2,        NebulaColor2,        "Nebula Color 2") \
    X(nebulaColor3,        NebulaColor3,        "Nebula Color 3") \
    X(auroraColor,         AuroraColor,         "Aurora Color") \
    X(auroraColorTop,      AuroraColorTop,      "Aurora Color Top") \
    X(starColor,           StarColor,           "Star Color")

namespace env {
#define HE_ENV_DECL_FLOAT(m, Name, disp) float     get##Name(Ctx&); void set##Name(Ctx&, float);
#define HE_ENV_DECL_BOOL(m, Name, disp)  bool      get##Name(Ctx&); void set##Name(Ctx&, bool);
#define HE_ENV_DECL_INT(m, Name, disp)   int       get##Name(Ctx&); void set##Name(Ctx&, int);
#define HE_ENV_DECL_COLOR(m, Name, disp) glm::vec3 get##Name(Ctx&); void set##Name(Ctx&, const glm::vec3&);
HE_ENV_FIELDS_FLOAT(HE_ENV_DECL_FLOAT)
HE_ENV_FIELDS_BOOL(HE_ENV_DECL_BOOL)
HE_ENV_FIELDS_INT(HE_ENV_DECL_INT)
HE_ENV_FIELDS_COLOR(HE_ENV_DECL_COLOR)
#undef HE_ENV_DECL_FLOAT
#undef HE_ENV_DECL_BOOL
#undef HE_ENV_DECL_INT
#undef HE_ENV_DECL_COLOR
}

// ── Audio (Ctx.audio — the app's AudioEngine; null → no-ops) ─────────────────
namespace audio {
    // Play an audio ASSET (content-relative .hasset path). Returns a handle
    // (0 on failure) for stop/isPlaying.
    int  play(Ctx&, const std::string& path, float volume, float pitch, bool loop);
    int  playAt(Ctx&, const std::string& path, const glm::vec3& pos,
                float volume, float pitch, bool loop, float minDist, float maxDist);
    void stop(Ctx&, int handle);
    void stopAll(Ctx&);
    bool isPlaying(Ctx&, int handle);
    void setBusVolume(Ctx&, const std::string& bus, float volume);
    void setSoundPosition(Ctx&, int handle, const glm::vec3& pos); // move a spatial sound
}

// ── Debug draw (process-global timed queue; the app drains it each frame) ─────
// Submissions live for `seconds` (0 = exactly one frame). collect() advances the
// timers and appends the alive primitives as line segments — the app forwards
// them to IRenderer::SetDebugLines.
namespace debug {
    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color, float seconds);
    void sphere(const glm::vec3& center, float radius, const glm::vec3& color, float seconds);
    void box(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color, float seconds);
    void clear();
    // App hook: advance by dt, append alive segments to `out` (DebugLine from
    // HE_Core DebugDraw.h), drop expired entries.
    void collect(float dt, std::vector<DebugLine>& out);
}

// ── What a script may reach outside its own project (docs/he-apps-plan.md C) ──
// Three doors, declared in the .heproj and carried into the packaged build's
// project.hcfg. All THREE are shut unless a project says otherwise, which is what
// keeps every project written before them behaving exactly as it did.
//
// This is a one-way door in the plan's own risk list, so it is stated once, here:
//
//   The permission is about what a SCRIPT may name on its own. It is not about
//   what a PERSON may choose. A path the user picked in a file dialog is granted
//   for the rest of the session whatever `files` says — the choosing IS the
//   permission, and that is the whole model the plan asks for ("der Dialog
//   ERTEILT den Pfad, dann ist er frei"). `files` is the blanket that lets a
//   script name an absolute path with nobody having picked it.
//
// The editor is gated by the same block as the shipped app rather than by a
// looser rule of its own. A preview that may delete a stranger's directory while
// the export may not is the worse of the two failures: the damage happens on the
// author's machine, to the author's files, before anybody could have shipped it.
namespace perm {
    struct Grants
    {
        bool files     = false;   // fs beyond the sandbox, without a dialog
        bool processes = false;   // process.run / process.openUrl
        bool network    = false;  // reserved for `http` (Welle 3); nothing reads it yet
    };
    // App hook, called at project load and at app boot. Replacing the whole
    // struct rather than setting flags one at a time: a half-applied permission
    // set is a state nobody should be able to observe.
    void          set(const Grants& g);
    const Grants& get();
    // "May this row run?" — logs once per row name when it may not, because a
    // script that silently does nothing is the hardest failure to diagnose and
    // the likeliest one here (the answer is a project setting, not the code).
    bool allowed(bool granted, const char* row);
}

// ── Sandboxed file I/O (fs) + save-game store (save) ─────────────────────────
// Paths are RELATIVE to a per-project sandbox root the app sets (editor: the
// project's Saved/ dir; game: the per-user pref dir). Absolute paths and ".."
// are rejected there — a script can never walk out of the sandbox by accident.
//
// An ABSOLUTE path is a second, deliberate route, open only when the project
// grants `perm::files` or when the path was GRANTED at runtime by somebody
// picking it in a dialog (see grantPath). Both are checked in the one place that
// turns a script's string into a real path, so a row added later cannot forget.
namespace fs {
    void        setSandboxRoot(const std::string& absDir);  // app hook (created on demand)
    std::string sandboxRoot();
    // Open an absolute path (or a whole directory) to the scripts for the rest of
    // this session. Called by the file dialogs with whatever the user picked, and
    // by nothing else — a row that granted its own argument would be a permission
    // model that permits everything.
    void        grantPath(const std::string& absPath);
    // Every grant so far, for a host that wants to show or persist them. Grants
    // are session-scoped on purpose: a project that needs a path every time
    // should ask for `perm::files`, not accumulate a list nobody can audit.
    const std::vector<std::string>& grantedPaths();
    void        clearGrants();
    bool        writeText(const std::string& rel, const std::string& text);
    std::string readText(const std::string& rel);            // "" when missing/invalid
    bool        exists(const std::string& rel);
    bool        remove(const std::string& rel);               // files only
    bool        makeDir(const std::string& rel);

    // ── The rest of what an application needs from a filesystem ──────────────
    // isDir answers about the same path `exists` does, separately, because
    // "there is something there" and "I may list it" are different questions and
    // one bool cannot carry both.
    bool        isDir(const std::string& path);
    // Bytes, and seconds since the epoch — the same clock `datetime` speaks, so
    // a file's age is a subtraction and not a second time format to learn.
    // -1 when the path is unreachable, which no real file can be.
    double      size(const std::string& path);
    double      modified(const std::string& path);
    // The immediate children of a directory, names only, sorted. Names rather
    // than full paths so joining stays the caller's decision, and sorted because
    // a directory's own order is the filesystem's business and differs between
    // two machines showing "the same" list.
    std::vector<std::string> list(const std::string& dir);
    bool        rename(const std::string& from, const std::string& to);
    // Files only, and it does NOT overwrite: a copy that silently replaced the
    // destination would be the one row here that can destroy data the caller
    // never named.
    bool        copy(const std::string& from, const std::string& to);

    // ── Watching (plan C, the last open row of the fs line) ──────────────────
    // `watch` names a file or a directory and hands back a handle; from then on
    // the application fires OnFileChanged with the PATH whenever that file, or
    // an immediate child of that directory, appears, disappears or changes. 0
    // means it did not start — an unreachable path (the same refusal every row
    // here gives) or too many watches already.
    //
    // The path comes back in the CALLER'S spelling. A script that watched the
    // relative "docs" is told about "docs/notes.txt", never about the absolute
    // path underneath: an absolute path is exactly what `resolved` would refuse
    // to open again unless the project granted files, so the event would carry
    // a name its own receiver could not read.
    //
    // The event carries one value, and "what happened" is three (appeared,
    // vanished, changed). The path is the one that fits, and `exists`, `size`
    // and `modified` answer the rest — the same trade the HTTP ticket makes.
    //
    // Polling, not an OS notifier, and not on a thread. The sandbox root, the
    // grants and the permission bits are process-wide statics that no lock
    // guards; a watcher thread calling `resolved` would race every dialog. The
    // frame already wakes on its own heartbeat (100 ms), so the scan sits there
    // and costs a stat per watch every `kWatchIntervalSeconds`.
    //
    // A directory watch is ONE level deep. Recursion would turn a watch on a
    // home folder into a full-disk walk every second, and a script that wants a
    // tree can watch the branches it cares about.
    int         watch(const std::string& path);
    void        unwatch(int handle);

    // ── Host side (not script rows) ──────────────────────────────────────────
    // Called once per frame by the application. Does the actual stat work at
    // most every kWatchIntervalSeconds; everything else is an early return.
    void        pollWatches(double dtSeconds);
    // The frame loop drains this and fires OnFileChanged, for the same reason it
    // drains finished HTTP tickets: firing a graph belongs in the frame.
    bool        takeChange(std::string& path);
    // Drop every watch. Belongs wherever clearGrants() is called — a watch that
    // outlived its grant would keep reporting a path the script may no longer
    // open.
    void        clearWatches();
    // Longest a change may go unnoticed. One second is the editor's own file
    // cadence rounded down, and the number the plan's stopgap ("fs.modified in a
    // Delay loop") already taught scripts to expect.
    inline constexpr double kWatchIntervalSeconds = 1.0;
    // Beyond this many watches the row refuses instead of quietly scanning
    // forever, and beyond this many entries a directory is watched for its own
    // existence only. Both are the same bargain: a script cannot turn a
    // one-line call into an unbounded per-second cost.
    inline constexpr int    kMaxWatches   = 32;
    inline constexpr size_t kMaxWatchEntries = 4096;
}

// ── Running another program (process) ────────────────────────────────────────
// Straight onto HE::Proc, which is a real subprocess API — argv vector rather
// than a shell string, stdout and stderr apart, honest exit codes, a timeout.
// Every row here needs `perm::processes`; without it they log once and return
// their neutral answer.
//
// run() is SYNCHRONOUS and therefore takes a timeout with a real default: a
// graph node that never returns freezes the frame it was called on, and an
// application that hangs on a subprocess looks exactly like one that crashed.
namespace process {
    struct RunResult
    {
        bool        ok = false;      // ran to completion and exited zero
        int         exitCode = -1;
        std::string out, err;
    };
    RunResult run(Ctx&, const std::string& exe, const std::vector<std::string>& args,
                  double timeoutSeconds);
    // Hand a URL (or a file) to whatever the desktop opens it with. The one row
    // here an ordinary application really wants — "open the manual", "show this
    // in the file manager" — and the only one that reaches outside without
    // running anything the caller named.
    bool        openUrl(Ctx&, const std::string& url);
    // Where the OS would find `exe`, or "" — so a script can say "you need git"
    // instead of failing to launch it.
    std::string which(Ctx&, const std::string& exe);
}

// ── HTTP (asynchronous; plan C, Welle 3) ─────────────────────────────────────
// A request is STARTED here and answered later: `get`/`post` hand back a ticket
// number and return immediately, the work happens on one worker thread, and the
// finished ticket arrives at the application as OnHttpResponse. Then the readers
// below say what came back. Blocking would have been half a line of code and the
// wrong half — the frame loop is what draws the window, and a script that waits
// ten seconds for a server is a program somebody force-quits.
//
// Why a ticket and not the answer: an event carries ONE value, and a response is
// four (ok, status, body, error). The ticket is the one thing that fits, and it
// is also what tells two requests in flight apart.
//
// The transport is HE::Net::httpsRequest, which delegates to NSURLSession,
// WinHTTP or libcurl — certificate validation is exactly the thing not to write
// by hand. http:// and https:// both work.
//
// Behind the project's "Network access" permission (perm::network). A refused
// call returns 0 and logs once; 0 is never a valid ticket, so "did it start" is
// the same question everywhere.
namespace http {
    // Start a request. Returns the ticket, or 0 when the permission is missing,
    // the URL is empty, or this build has no TLS backend at all.
    int  get(Ctx&, const std::string& url);
    // `contentType` empty means application/json — the one an app posting from a
    // graph almost always wants, and spelling it out every time is a way to get
    // it wrong once.
    int  post(Ctx&, const std::string& url, const std::string& contentType,
              const std::string& body);

    // Readers. Unpermissioned on purpose: they answer about a request this
    // process already made, and a gate here would only hide the answer from the
    // code that was allowed to ask the question.
    bool        done(Ctx&, int ticket);    // false while in flight or unknown
    bool        ok(Ctx&, int ticket);      // reached the server and got an answer
    int         status(Ctx&, int ticket);  // HTTP status, 0 when there is none
    std::string body(Ctx&, int ticket);
    std::string error(Ctx&, int ticket);   // "" unless ok is false
    // Drop a finished response. Optional politeness: the table keeps the last 32
    // and evicts the oldest by itself, so a long-running app cannot grow through
    // it. Reading a forgotten (or evicted) ticket answers like an unknown one.
    void        forget(Ctx&, int ticket);
    // Does this build have a TLS backend? False on a Linux built without libcurl,
    // where every request fails — a thing an application should be able to say
    // out loud rather than discover per request.
    bool        available(Ctx&);

    // ── Host side (not script rows) ──────────────────────────────────────────
    // The frame loop takes finished tickets out of here and fires OnHttpResponse.
    // Delivery is the host's because firing a graph belongs in the frame, not on
    // the worker — the same rule the tray's clicks follow.
    bool takeFinished(int& ticket);
    // Stop the worker and join it. Called at shutdown; a request already in
    // flight still has to end, so this waits up to the request timeout.
    void shutdown();
}

// ── JSON ─────────────────────────────────────────────────────────────────────
// Reading and writing JSON text, addressed by a dotted PATH: "user.name",
// "items[2].id", "" for the document itself. Text in, text out, because that is
// what a typed-pin graph can carry — an in-memory document type would need a
// handle, a lifetime and a way to leak one.
//
// Every getter takes the value to return when the path is missing, the type is
// wrong or the text does not parse. None of them throw and none of them log:
// asking a document whether it has something is a normal thing to do.
namespace json {
    std::string getString(Ctx&, const std::string& text, const std::string& path,
                          const std::string& fallback);
    double      getNumber(Ctx&, const std::string& text, const std::string& path, double fallback);
    bool        getBool  (Ctx&, const std::string& text, const std::string& path, bool fallback);
    bool        has      (Ctx&, const std::string& text, const std::string& path);
    // Elements at `path` when it names an array, else 0. Lets a graph walk
    // "items[0]", "items[1]", … without guessing where to stop.
    int         count    (Ctx&, const std::string& text, const std::string& path);
    // Setters return the WHOLE document as new text. Missing intermediate
    // objects are created; a path through something that is not an object (or
    // text that does not parse) returns the input unchanged.
    std::string setString(Ctx&, const std::string& text, const std::string& path,
                          const std::string& value);
    std::string setNumber(Ctx&, const std::string& text, const std::string& path, double value);
    std::string setBool  (Ctx&, const std::string& text, const std::string& path, bool value);
}

// ── Preferences ──────────────────────────────────────────────────────────────
// Small persistent settings: window position, last folder, "don't show this
// again". Deliberately NOT the save system — that one is shaped by a
// SaveGameTemplate asset and belongs to a game's progress, while these are
// key/value scraps an application accumulates. Stored as one JSON file inside
// the same sandbox the fs group uses, written on every change so a crash cannot
// lose more than the last setting.
namespace prefs {
    std::string getString(Ctx&, const std::string& key, const std::string& fallback);
    double      getNumber(Ctx&, const std::string& key, double fallback);
    bool        getBool  (Ctx&, const std::string& key, bool fallback);
    void        setString(Ctx&, const std::string& key, const std::string& value);
    void        setNumber(Ctx&, const std::string& key, double value);
    void        setBool  (Ctx&, const std::string& key, bool value);
    bool        has      (Ctx&, const std::string& key);
    bool        remove   (Ctx&, const std::string& key);
    void        clear    (Ctx&);
}

// ── Date and time ────────────────────────────────────────────────────────────
// The WALL clock, unlike the time group, which is the game's. An application
// showing "last saved 14:32" needs the one that keeps running when the game is
// paused and matches what the operating system says.
namespace datetime {
    // Seconds since the Unix epoch, as a double so it survives a Float pin.
    double      now(Ctx&);
    // strftime format ("%Y-%m-%d %H:%M:%S" and friends), in LOCAL time.
    std::string format(Ctx&, double epochSeconds, const std::string& fmt);
    // Individual fields of the local time, since a graph pulling one number out
    // of a formatted string is a parser nobody wanted to write.
    int         year  (Ctx&, double epochSeconds);
    int         month (Ctx&, double epochSeconds);   // 1..12
    int         day   (Ctx&, double epochSeconds);   // 1..31
    int         hour  (Ctx&, double epochSeconds);   // 0..23
    int         minute(Ctx&, double epochSeconds);   // 0..59
    int         second(Ctx&, double epochSeconds);   // 0..60 (leap second)
    int         weekday(Ctx&, double epochSeconds);  // 0 = Sunday
}
// ── Savegames: one ACTIVE, template-shaped save document ─────────────────────
// A save is { id, templateRef, fields, entities }: its fields are declared by a
// SaveGameTemplate asset (typed, with defaults — so scripts never have to
// remember what lives where; save.fields() and the editor's field dropdowns
// enumerate them), and serialized entity state (SaveStateComponent) is appended
// under entity UUIDs. Exactly one save is active at a time.
//
// create()/load() and every field accessor work purely IN MEMORY — only
// write()/list()/exists()/remove() touch disk (Saves/<id>.json under the fs
// sandbox; write is atomic temp+rename). Ids are restricted to
// [A-Za-z0-9_-]+ so they round-trip as filenames. Failures are LOUD: a call
// without an active save, an unknown field, or a type mismatch logs and
// returns false / the default — never silently.
namespace save {
    // App hooks.
    void setDefaultTemplate(const std::string& contentRelPath); // editor: .heproj; game: hcfg
    std::string defaultTemplate();

    // Lifecycle. create() seeds the fields from the template's defaults
    // (explicit `templatePath` empty → the project default; none configured →
    // fail). load() reads Saves/<id>.json, resolves the template it names and
    // re-validates every stored field against it (unknown/missing template =
    // loud failure). Both replace the previously active save.
    bool create(const std::string& id, ::ContentManager* cm,
                const std::string& templatePath = {});
    bool load(const std::string& id, ::ContentManager* cm);
    bool write();                    // persist the active save (atomic)
    void close();                    // drop the active save (PIE stop / project switch)
    std::string activeId();          // "" = none

    // Disk queries (independent of the active save).
    std::vector<std::string> list(); // ids of every save under Saves/
    bool exists(const std::string& id);
    bool remove(const std::string& id);

    // Template introspection: the active save's field names.
    std::vector<std::string> fields();

    // Typed field access, validated against the template. getNumber/setNumber
    // cover Float, Int and Enum fields (int-backed); Vec2/Color/Transform
    // fields are reachable by wrapping them in a struct field.
    bool        setNumber(const std::string& field, float v);
    float       getNumber(const std::string& field, float def);
    bool        setString(const std::string& field, const std::string& v);
    std::string getString(const std::string& field, const std::string& def);
    bool        setBool(const std::string& field, bool v);
    bool        getBool(const std::string& field, bool def);
    bool               setStructV(const std::string& field, const HorizonCode::Value& v);
    HorizonCode::Value getStructV(const std::string& field);
    // JSON text form of a Struct field (the save file's own encoding) — the
    // C++ GameLogic boundary, which has no Value type. Same validation.
    bool        setStructJson(const std::string& field, const std::string& json);
    std::string getStructJson(const std::string& field);      // "" on failure

    // Entity-state section (SaveStateComponent, see entity.saveState /
    // applySavedState): an opaque JSON object per entity UUID.
    bool        setEntityState(const std::string& uuid, const std::string& json);
    std::string entityState(const std::string& uuid);   // "" = none
    bool        hasEntityState(const std::string& uuid);

    // Play-mode gate for the entity-state API (the SceneSerializer owns
    // edit-mode persistence): set by the editor's PIE toggle and the game.
    void setPlayMode(bool inPlay);
    bool inPlayMode();
}

// ── C++ GameLogic services (HorizonGameServices.h) ───────────────────────────
// Fill the C-ABI table a GameLogic library receives via HE_SetEngineServices.
// `binding` must outlive the table's use (the app owns both); world resolves
// per call so scene switches stay transparent.
struct SaveServicesBinding
{
    std::function<HorizonWorld*()> world;   // may return null (calls then no-op loud)
    ContentManager*                content = nullptr;
};
void fillSaveServices(::HeSaveServices& out, SaveServicesBinding* binding);

// ── Scene transitions (process-global request queue; the app executes) ────────
// load() requests a full deferred world switch at a safe frame boundary;
// loadAdditive() streams another scene INTO the running world (returns a zone id
// so it can be unloaded again) — together they give seamless level transitions:
// additively load the next zone, move the player, unload the zone behind.
// `hidden` defers presentation: a hidden zone loads with its meshes invisible
// until showZone; a hidden level PRELOADS in the background and swaps in on
// activate(). Zone queries (loaded zones, their scene + position) read a
// process-global zone table the app maintains after executing requests.
// Editor PIE consumes the requests with a notice (game-runtime feature).
namespace scene {
    void load(const std::string& scenePath, bool hidden = false);
    // `position` places the zone's root when it loads (unwired/zero = as
    // authored — the merge root is a fresh identity entity, so zero is a no-op).
    int  loadAdditive(const std::string& scenePath, bool hidden = false,
                      const glm::vec3& position = glm::vec3(0.0f)); // → zone id
    void unloadZone(int zone);
    void activate();   // swap in the level preloaded with load(path, hidden=true)
    // Queued variants of show/hide/move — they order correctly with a load
    // requested the SAME frame (the direct setters below only see zones that
    // already finished loading).
    void requestZoneVisible(int zone, bool visible);
    // Show Zone / Hide Zone as two callees rather than one with a bool: the
    // registry rows below expose them as separate functions, and ApiFn::cppCall
    // has to name a function whose signature matches that row's params exactly
    // (a shared requestZoneVisible would drop the bool that tells them apart).
    void showZone(int zone);
    void hideZone(int zone);
    void requestZonePosition(int zone, const glm::vec3& p);

    // ── Zone queries / control (direct; operate on the app-maintained table) ──
    std::vector<int>         loadedZones();
    std::string              zoneScene(int zone);            // "" unknown
    glm::vec3                zonePosition(Ctx&, int zone);   // the zone root's position
    // Moves the zone root, then REBUILDS the zone's physics bodies from where
    // their entities now are: a body is baked once and nothing re-derives it
    // when an ancestor moves, so without the rebuild a moved zone would render
    // at its new place and collide at its old one. Costs the velocity of any
    // dynamic body in the zone — the rebuild tears the old one down first.
    void                     setZonePosition(Ctx&, int zone, const glm::vec3& p); // move the whole zone
    void                     setZoneVisible(Ctx&, int zone, bool visible);        // flip its meshes
    // Every scene the game can load: the packed scene index in shipped builds,
    // a project scan (.hescene, project-relative) in dev builds.
    std::vector<std::string> availableScenes(Ctx&);
    bool                     hasPendingLevel();              // a hidden load awaits activate()

    // ── App hooks ─────────────────────────────────────────────────────────────
    // What a queued Request asks the app to do. The app dispatch (game runtime and
    // editor PIE) switches on Request::kindOf() instead of repeating the raw numbers.
    enum class RequestKind : int
    {
        Switch       = 0,  // full world switch — or, with hidden, a background PRELOAD
        Additive     = 1,  // additive zone load
        UnloadZone   = 2,  // unload an additive zone
        Activate     = 3,  // present the level preloaded by Switch + hidden
        ZoneVisible  = 4,  // show/hide a zone (queued so it orders after a load)
        ZonePosition = 5,  // move a zone (queued so it orders after a load)
    };
    struct Request
    {
        // Stays `int` on the wire: the numbering is the queue's public contract
        // (test_engine_api.cpp asserts it) and RequestKind is the typed view of it.
        int         kind = 0;
        std::string path;
        int         zone = 0;
        bool        hidden = false;                 // load: defer presentation
        bool        flag = false;                   // ZoneVisible: target visibility
        glm::vec3   pos{ 0.0f };                    // Additive: placement; ZonePosition: target
        RequestKind kindOf() const { return static_cast<RequestKind>(kind); }
    };
    std::vector<Request> takeRequests();
    struct ZoneInfo { std::string path; uint32_t root = 0; std::vector<uint32_t> entities; };
    void            noteZoneLoaded(int zone, ZoneInfo info);
    void            noteZoneUnloaded(int zone);
    void            clearZones();          // world switched — zones are gone
    const ZoneInfo* zoneInfo(int zone);    // nullptr unknown
    void            notePendingLevel(bool pending);
    // The well-known scene-index name (packed under sceneUuidForPath(this)).
    inline const char* kSceneIndexEntry = "__scene_index__";
}

// ── String library (pure; complements the Concat/ToString nodes) ─────────────
// C++ namespace `str`, registry ids "string.*" (namespace `string` would shadow
// std::string in this header's users).
namespace str {
    int         length(const std::string& s);
    // Exact, case-sensitive. The node HorizonCode was missing: its Equals is
    // Float (the interpreter compares |a-b| < 1e-6), so two strings arriving
    // there both coerce to 0 and every id equals every other one. Anything that
    // routes on a String — On Menu Item, On Tray Item, a dropped file's name —
    // needs this to branch at all. Case-insensitive is toLower on both sides,
    // which is a decision the caller should have to make visible.
    bool        equals(const std::string& a, const std::string& b);
    std::string substring(const std::string& s, int start, int count); // clamped
    bool        contains(const std::string& s, const std::string& needle);
    int         find(const std::string& s, const std::string& needle);   // -1 if absent
    std::string replace(const std::string& s, const std::string& from, const std::string& to); // all
    std::string toUpper(const std::string& s);   // ASCII
    std::string toLower(const std::string& s);   // ASCII
    std::string trim(const std::string& s);      // ASCII whitespace both ends
    bool        startsWith(const std::string& s, const std::string& prefix);
    bool        endsWith(const std::string& s, const std::string& suffix);
    float       toNumber(const std::string& s);  // 0 when unparsable
}

// ── Math library (pure; no engine state) ─────────────────────────────────────
// Beyond HorizonCode's built-in Add/Sub/Mul/Div/compare operator nodes — the
// standard-library functions gameplay needs. Deterministic, so codegen inlines
// them and the interpreter may freely re-evaluate.
namespace math {
    float sin(float x);
    float cos(float x);
    float tan(float x);
    float sqrt(float x);
    float abs(float x);
    float floor(float x);
    float ceil(float x);
    float round(float x);
    float sign(float x);                         // -1 / 0 / 1
    float pow(float base, float exp);
    float mod(float a, float b);                 // 0 if b == 0
    float atan2(float y, float x);
    // Angle conversion, named the GLSL way: radians() RETURNS radians.
    // Everything the engine hands out as an angle is in DEGREES (rotations,
    // camera yaw/pitch, FOV) while sin/cos/tan take radians, so without these
    // every graph that does trigonometry on an engine angle has to multiply by
    // a magic 0.0174533.
    float radians(float degrees);
    float degrees(float radians);
    float min(float a, float b);
    float max(float a, float b);
    float clamp(float x, float lo, float hi);
    float lerp(float a, float b, float t);
    float length(const glm::vec2& v);
    float distance(const glm::vec2& a, const glm::vec2& b);
    // 3D versions of the same, plus what a vector type is actually for. The
    // suffix marks the width, except for cross — a cross product only exists in
    // 3D, so there is nothing to disambiguate it from.
    float     length3(const glm::vec3& v);
    float     distance3(const glm::vec3& a, const glm::vec3& b);
    glm::vec3 normalize3(const glm::vec3& v);            // zero vector stays zero
    float     dot3(const glm::vec3& a, const glm::vec3& b);
    glm::vec3 cross(const glm::vec3& a, const glm::vec3& b);
}

// ── Random (seeded PRNG; process-global state) ───────────────────────────────
// Stateful — each draw advances the generator — so in HorizonCode these are exec
// nodes (isExec) that cache one value per run, not pure data chips (which would
// re-roll on every pin read). seed() makes a run reproducible, so the codegen
// parity harness can pin a seed and compare. No engine state → no Ctx.
namespace random {
    void  seed(uint32_t s);
    float value();                       // uniform [0, 1)
    float range(float min, float max);   // uniform [min, max)  (swaps if min > max)
    int   rangeInt(int min, int max);    // uniform [min, max]  inclusive
    bool  chance(float p);               // true with probability p (clamped 0..1)
}

// ── Time / frame (process-global clock; the app advances it once per frame) ───
// Getters are pure (constant within a frame) → pure data nodes in HorizonCode.
// deltaTime() is SCALED by the time scale (Unity semantics): a script that
// already integrates against it slows down, speeds up and pauses for free, and
// the app loops read this ONE value for every gameplay tick. Anything that must
// keep running while the game is paused (a pause menu, debug primitives) uses
// the app's raw frame dt or unscaledDeltaTime() instead.
namespace time {
    void  advance(float dtSeconds);      // app hook: called once per rendered frame (RAW dt)
    void  reset();                       // app hook: zero on play-start (also restores scale 1)
    float deltaTime();                   // last frame's dt (seconds), SCALED
    float unscaledDeltaTime();           // last frame's dt as the app measured it
    float elapsed();                     // scaled seconds since reset
    int   frameCount();                  // frames since reset
    // 0 = paused, 1 = normal, up to kMaxTimeScale. Clamped HERE so every
    // frontend (Lua, Python, HorizonCode, C++) inherits the same bounds.
    void  setTimeScale(float scale);
    float timeScale();
    inline constexpr float kMaxTimeScale = 5.0f;
    // "The game is paused" as ONE predicate rather than a `== 0.0f` spelled out
    // at every gate (input dispatch, latent flow). No registry row: a script
    // that wants to ask already has timeScale().
    inline bool isPaused() { return timeScale() <= 0.0f; }
}

// ── Player possession (process-global table; PlayerHost owns it) ─────────────
// The PlayerController is the engine's central point of contact for a player:
// input events reach IT, and it decides what to do with them. Possessing a
// PlayerCharacter tells the engine to forward those same events to the character
// as well — the controller keeps handling them either way, with or without a
// character, so a controller is never made passive by possessing one.
//
// The table is process-global for the same reason input/time/random are: it is
// per-session state with exactly one owner (PlayerHost) and no world of its own
// — a controller outlives any particular scene. Refs are HorizonCode instance
// handles; 0 means "none", and a handle whose instance is gone reads as none.
namespace player {
    void     possess(uint32_t controller, uint32_t character);
    void     unpossess(uint32_t controller);
    uint32_t possessed(uint32_t controller);     // what this controller drives
    uint32_t controllerOf(uint32_t character);   // who drives this character
    uint32_t controller();                       // the (first) player controller
    uint32_t character();                        // what it possesses (0 = none)
    // App hooks: PlayerHost registers the session's controllers so controller()
    // has something to answer, and clears the table when the session ends.
    void     setControllers(const std::vector<uint32_t>& controllers);
    void     clear();
}

// ── Input (process-global snapshot; the app pushes it each frame) ─────────────
// Key names follow SDL scancode names ("W", "Space", "Left", "Escape", …) so the
// query side stays SDL-free here while the app populates it from real devices.
// Getters are pure (constant within a frame) → pure data nodes in HorizonCode.
namespace input {
    // App hooks (populate the snapshot).
    void setMouse(const glm::vec2& pos, const glm::vec2& delta, uint32_t buttonMask, float scroll);
    void setKeysDown(const std::vector<std::string>& downKeyNames);
    void clear();
    // Push the current SDL keyboard/mouse state into the snapshot above, so the
    // input.* registry nodes and scripts poll fresh values this frame. Mouse delta +
    // scroll are left at 0 here to avoid consuming SDL's relative-motion accumulator
    // the free-fly camera controller uses; position + buttons + keys (by SDL scancode
    // name, e.g. "W"/"Space") are polled. Called once per frame by whichever app owns
    // the SDL window — the packaged game every frame, the editor only while playing
    // (edit mode leaves the snapshot untouched).
    // `dx`/`dy` is the frame's mouse MOVEMENT. It has to be handed in: the rest
    // of this snapshot is polled (SDL_GetMouseState), and a movement cannot be —
    // it only exists as motion events, accumulated by Input. Until this took a
    // parameter it was hardcoded to zero, which is why the "Mouse Delta" node
    // has always answered (0,0). Pass 0,0 where the mouse is not the caller's
    // to give (the editor outside play mode).
    void pushSdlSnapshot(float dx = 0.0f, float dy = 0.0f);
    // App hook: the merged gamepad state for this frame. PUSHED, not polled —
    // Input (HE_Core) owns the SDL devices, and this snapshot deliberately has
    // no second opinion about them (the mouse-delta ownership conflict that
    // pushSdlSnapshot documents above is not getting a gamepad sibling).
    // `axes` are the DEADZONE-FILTERED values (what gameplay wants; a script
    // reading a resting stick sees exactly 0.0). Raw values stay queryable in
    // C++ via Input::gamepad() for calibration UI.
    void setGamepad(bool connected,
                    const float* axes, size_t axisCount,
                    const bool* buttons, size_t buttonCount);
    // Script queries.
    bool      keyDown(const std::string& name);
    bool      mouseButton(int index);    // 0 = left, 1 = right, 2 = middle
    glm::vec2 mousePosition();
    glm::vec2 mouseDelta();
    float     scrollDelta();
    // Gamepad queries. Names are SDL's mapping-string tables, Xbox layout:
    // buttons "a"/"b"/"x"/"y"/"leftshoulder"/"dpup"/…, axes "leftx"/"lefty"/
    // "rightx"/"righty"/"lefttrigger"/"righttrigger". Sticks read -1..+1
    // (SDL convention: Y positive DOWNWARD), triggers 0..1.
    bool  gamepadConnected();
    bool  gamepadButton(const std::string& name);
    float gamepadAxis(const std::string& name);

    // ── Input routing: whose input this frame is ─────────────────────────────
    // The switch a PlayerController flips between "the game is being played"
    // and "a menu is up". Three states, the same three Unreal names:
    //
    //   GameOnly   the UI still DRAWS, but receives nothing: no hover, no
    //              click, no wheel, no focus navigation, no text entry.
    //              Gameplay gets every input unmasked.
    //   GameAndUI  both. A pointer over an interactive element belongs to the
    //              UI and is masked out of gameplay; everything else passes
    //              through. This is the default and it is exactly what the
    //              engine did before modes existed, so no project changes
    //              behaviour by upgrading.
    //   UIOnly     the UI has it all. Gameplay reads no keys, no mouse
    //              buttons, no gamepad. Mouse POSITION still reads true: a
    //              widget graph asking where the pointer is should get an
    //              answer, and position alone drives nothing.
    //
    // The ONE exception in UIOnly is an input action whose author ticked
    // "run while paused". Without it the key that opens a menu could not close
    // it, and that flag already means "this action still works when the game is
    // stopped" — the same question, so it gets the same answer rather than a
    // second flag that can disagree with the first.
    //
    // The cursor is deliberately not part of this. cursor.setVisible exists and
    // the two are separate decisions: a cutscene with no cursor is still
    // GameOnly, and a pause menu may want the cursor its game already showed.
    enum class Mode : std::uint8_t { GameOnly = 0, GameAndUI = 1, UIOnly = 2 };
    // Host + C++ side. The apps read mode() every frame to route; setMode is
    // what the three script functions below land on, and what an app calls to
    // put the mode back to the default when a play session begins or ends.
    void setMode(Mode m);
    Mode mode();

    // Script side. Three functions rather than one taking a number, because
    // that is what a node palette can read: "Set Input Mode: UI Only" says
    // what it does, "Set Input Mode(2)" does not.
    void setModeGameOnly();
    void setModeGameAndUI();
    void setModeUIOnly();
    // "GameOnly" / "GameAndUI" / "UIOnly" — for a debug readout, and so a graph
    // can branch on the state it did not set itself.
    std::string modeName();
}

// ── Machine-readable registry ─────────────────────────────────────────────────
// One ApiFn per function. The interpreter looks a function up by `id` and calls
// `invoke`; the editor builds its add-menu from `category`/`params`/`results`;
// codegen emits the generic `hc::callApi(ctx, "<id>", …)` thunk, which lands on
// the same `invoke`. This is the single source of truth.

struct ApiParam
{
    const char* name;
    PinType     type;
    bool        isArray = false;
    // "Left at 0, this parameter means the caller itself." True for the leading
    // `entity` of every row that acts ON an entity — set in a post-pass when the
    // table is built (see registry()), never by hand at a row, so the rule cannot
    // be true for one character verb and forgotten at the next.
    //
    // Two readers besides the dispatcher: the graph editor draws such a pin as
    // "Self" instead of a zero, and the generated node reference says so in
    // words. Both ask this flag rather than keeping their own list.
    bool        selfDefault = false;
};

struct ApiFn
{
    const char* id;          // stable identifier, e.g. "transform.setPosition"
    const char* category;    // add-menu group, e.g. "Transform"
    bool        isExec;      // true = side-effecting (exec node); false = pure data node
    std::vector<ApiParam> params;    // typed inputs (in call order)
    std::vector<ApiParam> results;   // typed outputs (in return order)
    // Fully-qualified C++ callee this row stands for. Staged input for a planned
    // codegen migration (emit the direct call instead of routing through
    // hc::callApi) — nothing emits it today, so it is only as correct as the
    // invariant test in test_engine_api.cpp keeps it: one distinct callee per row,
    // its signature matching `params` in order.
    const char* cppCall;
    // Marshalling thunk: HorizonCode Values in → typed C++ call → Values out.
    // Missing/extra args are tolerated (defaults fill in), mirroring the API's
    // null-Ctx forgiveness.
    std::function<std::vector<Value>(Ctx&, const std::vector<Value>&)> invoke;
    // Human-readable editor name ("Sine", "Set Position") — what menus and node
    // titles show; `id` stays the stable machine identifier. Assigned in a post-
    // pass when the table is built (trailing member so rows stay positional).
    const char* displayName = nullptr;
};

// The full table (built once). Order is stable and grouped by category.
const std::vector<ApiFn>& registry();
// Look up a single entry by id; nullptr if unknown.
const ApiFn* find(const std::string& id);

// True for a registry id's namespace ("math" of "math.clamp") that the text
// scripting frontends expose as horizon.<group>.<fn>. Lua (ScriptContext) and
// Python (PyScriptBackend) MUST agree on this, or the same script works in one
// language and not the other — hence one list, not one per backend. The flat
// gameplay functions keep their ergonomic hand-written bindings until ScriptApi
// is inverted onto HE::api. NB: a packed vec3 (Color) param spreads as 4 numbers
// (x, y, z, _) on this path. Widening the surface = adding a name to the list.
bool isScriptGroup(std::string_view group);

// Is `apiId`'s group ("player.character" → "player") in `allowed`? An EMPTY list
// means yes — no restriction, which is what every general-purpose caller wants
// and what the whole engine did before restrictions existed.
//
// It lives here rather than in the editor because it is a statement about the
// API, and because two different menus have to reach the same verdict: the
// add-node palette and the drag-off-a-pin menu. Filtering one and not the other
// gives a restriction you can simply drag around.
bool groupAllowed(std::string_view apiId, const std::vector<const char*>& allowed);

} // namespace HE::api
