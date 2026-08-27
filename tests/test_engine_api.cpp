#include "doctest.h"
#include <HorizonScene/EngineApi.h>
#include <Types/TypeRegistry.h>
#include <HorizonGameServices.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/SaveStateComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonCode/HorizonCode.h>
#include <UIWidget/UIWidgetTree.h>
#include <DebugDraw/DebugDraw.h>
#include <Hpak/ProjectExporter.h>
#include <cstring>
#include <filesystem>
#include <glm/glm.hpp>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using HE::api::Ctx;
using HE::api::Value;
using P  = HorizonCode::PinType;
using NT = HorizonCode::NodeType;

namespace
{
    // Spawn an entity that carries a TransformComponent (createEntity alone does
    // not add one), so the transform api has something to read/write.
    HE::api::Entity spawnWithTransform(HorizonWorld& world, const glm::vec3& pos = glm::vec3(0.0f))
    {
        auto e = world.createEntity("ApiTest");
        TransformComponent tc;
        tc.position = pos;
        world.registry().emplace<TransformComponent>(e, tc);
        return static_cast<HE::api::Entity>(e);
    }
}

// ═══ Registry shape ═══════════════════════════════════════════════════════════

TEST_CASE("EngineApi: registry is populated and well-formed")
{
    const auto& reg = HE::api::registry();
    CHECK(reg.size() > 30);   // the promoted ScriptApi surface + the math library

    std::unordered_set<std::string> ids;
    for (const auto& fn : reg)
    {
        CHECK(fn.id != nullptr);
        CHECK(fn.category != nullptr);
        CHECK(fn.cppCall != nullptr);
        CHECK(fn.invoke != nullptr);
        CHECK(std::string(fn.id).length() > 0);
        // ids are unique
        CHECK(ids.insert(fn.id).second);
    }
}

TEST_CASE("EngineApi: cppCall names one real, distinct callee per row")
{
    // cppCall is staged input for the planned direct-call codegen and nothing
    // emits it yet, so only this test keeps it honest. Two rules:
    //   * it names a function in the HE::api tree (not a bare name, not a stale
    //     ScriptApi:: path), so a mechanical emitter can write it verbatim;
    //   * no two rows share a callee. Sharing means the shared function needs an
    //     argument that distinguishes the rows, and that argument is NOT in
    //     `params` — an emitter would generate a call with the wrong arity. That
    //     was real: scene.showZone/hideZone both named requestZoneVisible(int,
    //     bool) while declaring params {zone}, so both would have emitted
    //     requestZoneVisible(zone). They now name their own one-argument wrappers.
    std::unordered_map<std::string, std::string> calleeToId;   // cppCall → first id
    for (const auto& fn : HE::api::registry())
    {
        const std::string callee = fn.cppCall;
        INFO("row: " << fn.id << " → " << callee);
        CHECK(callee.rfind("HE::api::", 0) == 0);
        CHECK(callee.length() > std::strlen("HE::api::"));
        const auto [it, fresh] = calleeToId.emplace(callee, fn.id);
        INFO("also claimed by: " << it->second);
        CHECK(fresh);
    }
}

TEST_CASE("EngineApi: find() resolves ids, rejects unknown")
{
    CHECK(HE::api::find("transform.setPosition") != nullptr);
    CHECK(HE::api::find("math.clamp") != nullptr);
    CHECK(HE::api::find("does.not.exist") == nullptr);
    CHECK(HE::api::find("") == nullptr);
}

TEST_CASE("EngineApi: widget lifecycle is the built-in nodes, not the registry")
{
    // There used to be two halves of the same feature that could not be wired
    // together: the built-in Create Widget node hands its id out as a Ref, the
    // registry rows took an Int, and Ref/Int do not convert. The nodes won — Create
    // Widget picks its asset from a list, where a row could only take a typed path
    // — so the four lifecycle rows are gone and the rest speak Ref.
    for (const char* id : { "widget.create", "widget.destroy", "widget.show", "widget.hide" })
    {
        INFO("id: " << id);
        CHECK(HE::api::find(id) == nullptr);
    }

    // Only the ROWS went — HE::api::widget::create and friends are the C++ shape of
    // the same operations and keep their callers. What survives on the registry
    // takes the Ref a Create Widget node outputs, which is the whole point: as Int
    // these three had no source for the id at all once the create row was gone.
    const auto* z = HE::api::find("widget.setZOrder");
    REQUIRE(z != nullptr);
    REQUIRE(z->params.size() == 2);
    CHECK(z->params[0].type == P::Ref);
    CHECK(z->params[1].type == P::Int);      // the z-order itself is a plain number

    const auto* vis = HE::api::find("widget.isVisible");
    REQUIRE(vis != nullptr);
    REQUIRE(vis->params.size() == 1);
    CHECK(vis->params[0].type == P::Ref);

    const auto* call = HE::api::find("widget.callFunction");
    REQUIRE(call != nullptr);
    REQUIRE(call->params.size() == 2);
    CHECK(call->params[0].type == P::Ref);
    CHECK(call->params[1].type == P::String);
}

TEST_CASE("EngineApi: side-effect classification is correct")
{
    // Getters / queries / pure math are data nodes; setters / actions are exec.
    CHECK(HE::api::find("transform.getPosition")->isExec == false);
    CHECK(HE::api::find("transform.setPosition")->isExec == true);
    CHECK(HE::api::find("physics.raycast")->isExec == false);
    CHECK(HE::api::find("math.clamp")->isExec == false);
    CHECK(HE::api::find("entity.spawn")->isExec == true);
    CHECK(HE::api::find("log")->isExec == true);

    // Signatures carry typed params + results.
    const auto* setPos = HE::api::find("transform.setPosition");
    REQUIRE(setPos->params.size() == 2);
    CHECK(setPos->params[0].type == P::Int);      // entity
    CHECK(setPos->params[1].type == P::Vec3);     // position
    CHECK(setPos->results.empty());

    const auto* ray = HE::api::find("physics.raycast");
    REQUIRE(ray->results.size() == 5);
    CHECK(ray->results[0].type == P::Bool);       // hit
}

// ═══ Marshalling round-trips against a real world ═════════════════════════════

TEST_CASE("Transform: world position walks the parent chain, local does not")
{
    HorizonWorld world;
    Ctx c{ &world, nullptr, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // A parent moved 10 along X and turned a quarter turn about Y, with a child
    // sitting 2 in FRONT of it. The rotation is what makes this test worth
    // having: a parent that is only translated would pass even if the code
    // simply added the two positions together.
    const auto parent = spawnWithTransform(world);
    const auto child  = spawnWithTransform(world);
    REQUIRE(world.reparentEntity(static_cast<entt::entity>(child),
                                 static_cast<entt::entity>(parent)));

    call("transform.setPosition", { Value::ofInt((int)parent), Value::ofVec3({ 10.0f, 0.0f, 0.0f }) });
    call("transform.setRotation", { Value::ofInt((int)parent), Value::ofVec3({ 0.0f, 90.0f, 0.0f }) });
    call("transform.setPosition", { Value::ofInt((int)child),  Value::ofVec3({ 0.0f, 0.0f, 2.0f }) });

    // Local is unchanged by any of that — it is the offset inside the parent.
    const Value local = call("transform.getPosition", { Value::ofInt((int)child) })[0];
    CHECK(local.v3.z == doctest::Approx(2.0f));
    CHECK(local.v3.x == doctest::Approx(0.0f));

    // World: +Z rotated 90° about Y lands on +X, so the child stands 2 further
    // along X than its parent.
    const Value w = call("transform.getWorldPosition", { Value::ofInt((int)child) })[0];
    CHECK(w.type == P::Vec3);
    CHECK(w.v3.x == doctest::Approx(12.0f).epsilon(0.001));
    CHECK(w.v3.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(w.v3.z == doctest::Approx(0.0f).epsilon(0.001));

    // NOT propagated first, on purpose: worldMatrix is stale until someone runs
    // propagateTransforms, and a graph asking mid-frame must still get the truth.
    CHECK(world.registry().get<TransformComponent>(static_cast<entt::entity>(child)).worldMatrix
          == glm::mat4(1.0f));

    // Writing a world position puts the entity there, whatever its parent does.
    call("transform.setWorldPosition", { Value::ofInt((int)child), Value::ofVec3({ 0.0f, 5.0f, 0.0f }) });
    const Value back = call("transform.getWorldPosition", { Value::ofInt((int)child) })[0];
    CHECK(back.v3.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(back.v3.y == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(back.v3.z == doctest::Approx(0.0f).epsilon(0.001));

    // An unparented entity answers the same in both spaces — which is why the
    // distinction goes unnoticed until the first attached object.
    const auto lone = spawnWithTransform(world);
    call("transform.setPosition", { Value::ofInt((int)lone), Value::ofVec3({ 3.0f, 4.0f, 5.0f }) });
    CHECK(call("transform.getWorldPosition", { Value::ofInt((int)lone) })[0].v3
          == glm::vec3(3.0f, 4.0f, 5.0f));
}

// ═══ The local↔world boundary, with a real physics world behind it ════════════
//
// The four cases below are the ones nothing covered: every other transform test
// runs with a null-physics Ctx, and every physics test goes at PhysicsWorld
// directly. The boundary lives in between — HE::api::transform speaks LOCAL
// (TransformComponent's space), PhysicsWorld speaks WORLD ("EVERY pose this
// class exchanges is a WORLD pose"), and EngineApi.cpp is the only place that
// knows both. Nothing converted there until now, so a PARENTED physics entity
// put its body at the local value and its transform at inv(parent) * value:
// two different wrong places from one call.
//
// A rotated parent is what makes these tests worth having. With a parent that is
// only translated, the composed position and the sum of the two positions agree,
// so broken math passes.

namespace
{
    // A child with a body of its own, parented to `parent`. Static rather than
    // dynamic on purpose: these tests are about WHERE a teleport puts the body,
    // and a body that also falls would blur the answer with gravity.
    HE::api::Entity spawnParentedBody(HorizonWorld& world, HE::api::Entity parent,
                                      const glm::vec3& localPos)
    {
        auto e = world.createEntity("ParentedBody");
        TransformComponent t;
        t.position = localPos;
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        RigidBodyComponent rb;
        rb.type = RigidBodyType::Static;
        world.addComponent(e, rb);
        REQUIRE(world.reparentEntity(e, static_cast<entt::entity>(parent)));
        return static_cast<HE::api::Entity>(e);
    }

    // A top-level dynamic box, the "no parent" half of each pair below.
    HE::api::Entity spawnLoneDynamicBody(HorizonWorld& world, const glm::vec3& pos)
    {
        auto e = world.createEntity("LoneBody");
        TransformComponent t;
        t.position = pos;
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        RigidBodyComponent rb;
        rb.type = RigidBodyType::Dynamic;
        rb.mass = 1.0f;
        world.addComponent(e, rb);
        return static_cast<HE::api::Entity>(e);
    }
}

TEST_CASE("Transform: setPosition on a PARENTED physics entity places the body in world space")
{
    HorizonWorld world;

    // Parent 10 along X and turned a quarter turn about Y, exactly the shape the
    // hierarchy test above uses: a child sitting 2 in front of it (local +Z)
    // therefore stands 2 further along X (world +X), at (12, 0, 0).
    const auto parent = spawnWithTransform(world, { 10.0f, 0.0f, 0.0f });
    world.registry().get<TransformComponent>(static_cast<entt::entity>(parent)).rotation =
        { 0.0f, 90.0f, 0.0f };
    const auto child = spawnParentedBody(world, parent, { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(child)));

    Ctx c{ &world, &phys, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    call("transform.setPosition", { Value::ofInt((int)child), Value::ofVec3({ 0.0f, 0.0f, 2.0f }) });

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // Where the child is DRAWN — the composed world position — is where its
    // collider has to be.
    const auto atWorld = phys.raycast({ 12.0f, 50.0f, 0.0f }, down, 100.0f);
    REQUIRE(atWorld.hit);
    CHECK(atWorld.entityId == static_cast<uint32_t>(child));

    // And nothing is at the RAW LOCAL value. This is the half that fails without
    // the conversion: PhysicsWorld reads what it is handed as a world pose, so
    // an unconverted local (0,0,2) parked the body here, 12 m from its mesh.
    CHECK_FALSE(phys.raycast({ 0.0f, 50.0f, 2.0f }, down, 100.0f).hit);

    // Read back in the space it was written in. Exact rather than approximate on
    // purpose: teleportToLocalPose puts the caller's local values back after the
    // body move, so the round trip does not drift through a matrix and its
    // inverse. Asked BEFORE any step() — a body's own pose only overwrites the
    // transform when the simulation runs.
    const Value back = call("transform.getPosition", { Value::ofInt((int)child) })[0];
    CHECK(back.v3.x == doctest::Approx(0.0f));
    CHECK(back.v3.y == doctest::Approx(0.0f));
    CHECK(back.v3.z == doctest::Approx(2.0f));
}

TEST_CASE("Transform: setWorldPosition on a PARENTED physics entity lands both halves on the spot")
{
    // The regression this pins down. setWorldPosition converts world→local and
    // used to hand the result to transform::setPosition — which, once that one
    // started teleporting the body, read the already-converted value as a world
    // one again. The single function whose entire job is hierarchy-correct
    // placement was the one that misplaced parented entities, and it did so
    // WORSE than the committed tree does.
    HorizonWorld world;

    const auto parent = spawnWithTransform(world, { 10.0f, 0.0f, 0.0f });
    world.registry().get<TransformComponent>(static_cast<entt::entity>(parent)).rotation =
        { 0.0f, 90.0f, 0.0f };
    const auto child = spawnParentedBody(world, parent, { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(child)));

    Ctx c{ &world, &phys, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    const glm::vec3 target{ 5.0f, 0.0f, -7.0f };
    call("transform.setWorldPosition", { Value::ofInt((int)child), Value::ofVec3(target) });

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // The body is at the world position the caller named. Nothing else is
    // acceptable here — "world" is in the name.
    const auto atTarget = phys.raycast({ 5.0f, 50.0f, -7.0f }, down, 100.0f);
    REQUIRE(atTarget.hit);
    CHECK(atTarget.entityId == static_cast<uint32_t>(child));

    // And NOT at the double-converted place. inverse(parent) applied to the
    // target is (7, 0, -5) — the local position the transform legitimately gets,
    // and the exact spot the body used to be dumped at when that local value was
    // passed on to a setter that reads world.
    CHECK_FALSE(phys.raycast({ 7.0f, 50.0f, -5.0f }, down, 100.0f).hit);

    // Both spaces agree with the two questions asked of them: world is the
    // target, local is the target expressed inside the parent.
    const Value w = call("transform.getWorldPosition", { Value::ofInt((int)child) })[0];
    CHECK(w.v3.x == doctest::Approx(5.0f).epsilon(0.001));
    CHECK(w.v3.z == doctest::Approx(-7.0f).epsilon(0.001));
    const Value l = call("transform.getPosition", { Value::ofInt((int)child) })[0];
    CHECK(l.v3.x == doctest::Approx(7.0f).epsilon(0.001));
    CHECK(l.v3.z == doctest::Approx(-5.0f).epsilon(0.001));
}

TEST_CASE("Transform: setPosition on an UNPARENTED physics entity is unchanged by the conversion")
{
    // The other side of the boundary, and the reason it can ship: with no parent
    // above it every conversion is the identity, so the overwhelming majority of
    // scenes behave exactly as they did.
    //
    // Not written as a pure "nothing changed" check, because that would pass
    // without any of this code existing. It asserts the thing that IS new for a
    // top-level entity too: the teleport reaches the body, so the move survives
    // the step instead of being overwritten by Jolt's own pose.
    HorizonWorld world;
    const auto box = spawnLoneDynamicBody(world, { 0.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    Ctx c{ &world, &phys, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // Fall a while first, so the body's own idea of where it is has drifted far
    // from where the teleport wants it.
    for (int i = 0; i < 120; ++i)
        phys.step(world, 1.0f / 60.0f);
    REQUIRE(world.registry().get<TransformComponent>(static_cast<entt::entity>(box)).position.y < 0.0f);

    call("transform.setPosition", { Value::ofInt((int)box), Value::ofVec3({ 3.0f, 50.0f, -4.0f }) });

    // Immediately readable, in the spelling it was written in.
    const Value back = call("transform.getPosition", { Value::ofInt((int)box) })[0];
    CHECK(back.v3.x == doctest::Approx(3.0f));
    CHECK(back.v3.y == doctest::Approx(50.0f));
    CHECK(back.v3.z == doctest::Approx(-4.0f));

    // …and it is where the COLLIDER is, not just where the mesh is drawn.
    const auto hit = phys.raycast({ 3.0f, 60.0f, -4.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(hit.hit);
    CHECK(hit.entityId == static_cast<uint32_t>(box));

    // The step is the judge: without the body move it writes the falling pose
    // back over the transform and the set means nothing.
    phys.step(world, 1.0f / 60.0f);
    const auto& tr = world.registry().get<TransformComponent>(static_cast<entt::entity>(box));
    CHECK(tr.position.y > 49.0f);
    CHECK(tr.position.x == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(tr.position.z == doctest::Approx(-4.0f).epsilon(0.02));
}

TEST_CASE("Transform: setWorldPosition on an UNPARENTED physics entity is the same operation")
{
    // Top-level: local IS world, so this and the case above must agree. The
    // assertion that matters is again that the body followed — setWorldPosition
    // reaches physics on its own path rather than through setPosition, so its
    // teleport needs its own coverage.
    HorizonWorld world;
    const auto box = spawnLoneDynamicBody(world, { 0.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    Ctx c{ &world, &phys, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    for (int i = 0; i < 120; ++i)
        phys.step(world, 1.0f / 60.0f);
    REQUIRE(world.registry().get<TransformComponent>(static_cast<entt::entity>(box)).position.y < 0.0f);

    call("transform.setWorldPosition", { Value::ofInt((int)box), Value::ofVec3({ -6.0f, 40.0f, 8.0f }) });

    const auto hit = phys.raycast({ -6.0f, 60.0f, 8.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(hit.hit);
    CHECK(hit.entityId == static_cast<uint32_t>(box));

    phys.step(world, 1.0f / 60.0f);
    const auto& tr = world.registry().get<TransformComponent>(static_cast<entt::entity>(box));
    CHECK(tr.position.y > 39.0f);
    CHECK(tr.position.x == doctest::Approx(-6.0f).epsilon(0.02));
    CHECK(tr.position.z == doctest::Approx(8.0f).epsilon(0.02));

    // Both spaces answer the same for a top-level entity — the property that
    // lets a whole codebase confuse the two and never notice.
    const Value w = call("transform.getWorldPosition", { Value::ofInt((int)box) })[0];
    const Value l = call("transform.getPosition", { Value::ofInt((int)box) })[0];
    CHECK(w.v3.x == doctest::Approx(l.v3.x));
    CHECK(w.v3.y == doctest::Approx(l.v3.y));
    CHECK(w.v3.z == doctest::Approx(l.v3.z));
}

TEST_CASE("Transform: setRotation on a dynamic body survives the next step")
{
    // The rotation axis of the same defect. The physics loop writes Jolt's
    // orientation back over TransformComponent every step, so turning a dynamic
    // body from a script was undone in the frame it happened — a door that
    // swings open and is shut again before anyone sees it.
    //
    // There is no rotation-only teleport, so this goes through setTransform with
    // the position the entity already has; the test therefore also checks the
    // entity did not move as a side effect of being turned.
    HorizonWorld world;
    const auto box = spawnLoneDynamicBody(world, { 0.0f, 20.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    Ctx c{ &world, &phys, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // In the air and awake — a sleeping body is not written back at all, which
    // would make this pass for a reason that has nothing to do with the fix.
    for (int i = 0; i < 10; ++i)
        phys.step(world, 1.0f / 60.0f);
    const float yBefore =
        world.registry().get<TransformComponent>(static_cast<entt::entity>(box)).position.y;
    REQUIRE(yBefore < 20.0f);

    call("transform.setRotation", { Value::ofInt((int)box), Value::ofVec3({ 0.0f, 37.0f, 0.0f }) });

    // Nothing torques a free-falling box, so one step must leave the orientation
    // where it was put.
    phys.step(world, 1.0f / 60.0f);

    const Value r = call("transform.getRotation", { Value::ofInt((int)box) })[0];
    CHECK(r.v3.x == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(r.v3.y == doctest::Approx(37.0f).epsilon(0.01));
    CHECK(r.v3.z == doctest::Approx(0.0f).epsilon(0.01));

    // Turned, not teleported: it is still falling from where it was.
    const auto& tr = world.registry().get<TransformComponent>(static_cast<entt::entity>(box));
    CHECK(tr.position.x == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(tr.position.z == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(tr.position.y < yBefore);
}

TEST_CASE("EngineApi: transform round-trips through the registry")
{
    HorizonWorld world;
    Ctx c{ &world, nullptr, nullptr };
    auto e = spawnWithTransform(world);

    // set via the registry thunk — deliberately handing over a COLOR value, the
    // way a graph authored before Vec3 existed still does. The argument readers
    // go by the value's own type, so the legacy shape has to keep working.
    HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt((int)e), Value::ofColor(glm::vec4(1.0f, 2.0f, 3.0f, 0.0f)) });

    // read back via the registry thunk
    auto out = HE::api::find("transform.getPosition")->invoke(c, { Value::ofInt((int)e) });
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == P::Vec3);
    CHECK(out[0].v3.x == doctest::Approx(1.0f));
    CHECK(out[0].v3.y == doctest::Approx(2.0f));
    CHECK(out[0].v3.z == doctest::Approx(3.0f));

    // …and the current shape, a Vec3 value, lands in the same place.
    HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt((int)e), Value::ofVec3({ 4.0f, 5.0f, 6.0f }) });
    out = HE::api::find("transform.getPosition")->invoke(c, { Value::ofInt((int)e) });
    CHECK(out[0].v3 == glm::vec3(4.0f, 5.0f, 6.0f));

    HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt((int)e), Value::ofVec3({ 1.0f, 2.0f, 3.0f }) });

    // and via the typed C++ api directly — same result
    const glm::vec3 p = HE::api::transform::getPosition(c, e);
    CHECK(p.x == doctest::Approx(1.0f));
    CHECK(p.z == doctest::Approx(3.0f));
}

TEST_CASE("EngineApi: entity spawn + distance via the api")
{
    HorizonWorld world;
    Ctx c{ &world, nullptr, nullptr };

    auto a = spawnWithTransform(world, glm::vec3(0.0f));
    auto b = spawnWithTransform(world, glm::vec3(3.0f, 4.0f, 0.0f));
    CHECK(HE::api::entity::distance(c, a, b) == doctest::Approx(5.0f));

    // spawn through the registry, then it should have a valid name
    auto out = HE::api::find("entity.spawn")->invoke(c, { Value::ofInt(0), Value::ofString("Fresh") });
    REQUIRE(out.size() == 1);
    const auto spawned = (HE::api::Entity)out[0].i;
    CHECK(HE::api::entity::getName(c, spawned) == "Fresh");
}

// ═══ Math library ═════════════════════════════════════════════════════════════

TEST_CASE("EngineApi: pure math thunks compute correctly")
{
    Ctx c{};   // math needs no world

    auto call1 = [&](const char* id, float x) {
        return HE::api::find(id)->invoke(c, { Value::ofFloat(x) })[0].f; };
    auto call2 = [&](const char* id, float a, float b) {
        return HE::api::find(id)->invoke(c, { Value::ofFloat(a), Value::ofFloat(b) })[0].f; };

    CHECK(call1("math.sqrt", 16.0f) == doctest::Approx(4.0f));
    CHECK(call1("math.abs", -3.0f) == doctest::Approx(3.0f));
    CHECK(call1("math.floor", 2.9f) == doctest::Approx(2.0f));
    CHECK(call1("math.sign", -7.0f) == doctest::Approx(-1.0f));
    CHECK(call2("math.pow", 2.0f, 10.0f) == doctest::Approx(1024.0f));
    CHECK(call2("math.max", 3.0f, 8.0f) == doctest::Approx(8.0f));
    CHECK(call2("math.mod", 7.0f, 0.0f) == doctest::Approx(0.0f));   // div-by-zero guard

    auto clamp = HE::api::find("math.clamp")->invoke(c,
        { Value::ofFloat(5.0f), Value::ofFloat(0.0f), Value::ofFloat(3.0f) });
    CHECK(clamp[0].f == doctest::Approx(3.0f));

    auto lerp = HE::api::find("math.lerp")->invoke(c,
        { Value::ofFloat(0.0f), Value::ofFloat(10.0f), Value::ofFloat(0.5f) });
    CHECK(lerp[0].f == doctest::Approx(5.0f));

    auto dist = HE::api::find("math.distance")->invoke(c,
        { Value::ofVec2(glm::vec2(0.0f)), Value::ofVec2(glm::vec2(3.0f, 4.0f)) });
    CHECK(dist[0].f == doctest::Approx(5.0f));
}

// ═══ Null-Ctx tolerance ═══════════════════════════════════════════════════════

TEST_CASE("EngineApi: null handles return neutral defaults, never crash")
{
    Ctx c{};   // no world / physics / content

    CHECK(HE::api::entity::getName(c, 1) == "");
    CHECK(HE::api::transform::getPosition(c, 1) == glm::vec3(0.0f));
    CHECK(HE::api::transform::getScale(c, 1) == glm::vec3(1.0f));   // scale default (1,1,1)
    CHECK(HE::api::widget::create(c, "x") == 0);
    CHECK(HE::api::physics::isGrounded(c, 1) == false);

    // through the registry: a setter on a null world is a no-op, a getter defaults
    CHECK_NOTHROW(HE::api::find("transform.setPosition")->invoke(c,
        { Value::ofInt(1), Value::ofColor(glm::vec4(9.0f)) }));
    auto name = HE::api::find("entity.getName")->invoke(c, { Value::ofInt(1) });
    REQUIRE(name.size() == 1);
    CHECK(name[0].s == "");

    // log with a null ctx must be safe too
    CHECK_NOTHROW(HE::api::find("log")->invoke(c, { Value::ofString("hello from a null ctx") }));
}

// ═══ Physics surface ══════════════════════════════════════════════════════════

TEST_CASE("Physics: forces, velocity, overlap and gravity are on the registry")
{
    using HE::api::find;

    // Actions are exec nodes, questions are pure data nodes — a graph that got
    // this wrong would either re-roll a push per pin read or need an exec wire
    // to ask a question.
    for (const char* id : { "physics.addForce", "physics.addImpulse",
                            "physics.addTorque", "physics.setGravity" })
    {
        INFO("row: " << id);
        const auto* row = find(id);
        REQUIRE(row != nullptr);
        CHECK(row->isExec);
    }
    for (const char* id : { "physics.getVelocity", "physics.getGravity",
                            "physics.overlapSphere", "physics.sphereCast" })
    {
        INFO("row: " << id);
        const auto* row = find(id);
        REQUIRE(row != nullptr);
        CHECK_FALSE(row->isExec);
    }

    // setVelocity keeps the exact signature it shipped with: a result pin added
    // here would change every graph that already carries the node.
    const auto* setVel = find("physics.setVelocity");
    REQUIRE(setVel != nullptr);
    REQUIRE(setVel->params.size() == 2);
    CHECK(setVel->results.empty());

    const auto* overlap = find("physics.overlapSphere");
    REQUIRE(overlap->results.size() == 1);
    CHECK(overlap->results[0].isArray);
    CHECK(overlap->results[0].type == P::Int);

    const auto* sweep = find("physics.sphereCast");
    REQUIRE(sweep->params.size() == 4);      // origin, direction, radius, maxDistance
    REQUIRE(sweep->results.size() == 5);     // same hit shape as raycast
    CHECK(sweep->results[0].type == P::Bool);

    // Lua and Python reach the whole group through horizon.physics.* — the flat
    // bindings only ever had raycast/setVelocity/isGrounded.
    CHECK(HE::api::isScriptGroup("physics"));
}

TEST_CASE("Physics: every call is neutral without a PhysicsWorld")
{
    Ctx c{};   // no world, no physics

    CHECK_FALSE(HE::api::physics::addForce(c, 1, glm::vec3(1.0f)));
    CHECK_FALSE(HE::api::physics::addImpulse(c, 1, glm::vec3(1.0f)));
    CHECK_FALSE(HE::api::physics::addTorque(c, 1, glm::vec3(1.0f)));
    CHECK(HE::api::physics::getVelocity(c, 1) == glm::vec3(0.0f));
    CHECK(HE::api::physics::getGravity(c) == glm::vec3(0.0f));
    CHECK(HE::api::physics::overlapSphere(c, glm::vec3(0.0f), 5.0f).empty());
    CHECK_FALSE(HE::api::physics::sphereCast(c, glm::vec3(0.0f), glm::vec3(0, 0, 1), 1.0f, 10.0f).hit);
    CHECK_NOTHROW(HE::api::physics::setGravity(c, glm::vec3(0.0f, -1.0f, 0.0f)));
    CHECK_NOTHROW(HE::api::physics::setVelocity(c, 1, glm::vec3(1.0f)));

    // …and through the registry thunks, where the array result must still come
    // back as an empty ARRAY rather than a scalar zero.
    auto out = HE::api::find("physics.overlapSphere")->invoke(c,
        { Value::ofVec3(glm::vec3(0.0f)), Value::ofFloat(3.0f) });
    REQUIRE(out.size() == 1);
    CHECK(out[0].isArray);
    CHECK(out[0].items.empty());

    auto pushed = HE::api::find("physics.addImpulse")->invoke(c,
        { Value::ofInt(1), Value::ofVec3(glm::vec3(0.0f, 1.0f, 0.0f)) });
    REQUIRE(pushed.size() == 1);
    CHECK(pushed[0].b == false);
}

// ═══ Quitting + the UI pointer verdict ════════════════════════════════════════

TEST_CASE("App: quit calls the host's handler, and only warns without one")
{
    const auto* row = HE::api::find("app.quit");
    REQUIRE(row != nullptr);
    CHECK(row->isExec);
    CHECK(row->params.empty());
    CHECK(row->results.empty());

    // Nobody bound a handler (a test rig, an editor tool window): the call has
    // to stay a no-op, because the alternative is a graph that can kill a
    // process the host never offered to end.
    Ctx unbound{};
    CHECK_NOTHROW(row->invoke(unbound, {}));

    int quits = 0;
    Ctx c{};
    c.requestQuit = [&quits]{ ++quits; };
    row->invoke(c, {});
    CHECK(quits == 1);
    HE::api::app::quit(c);   // and the direct C++ call is the same thing
    CHECK(quits == 2);
}

TEST_CASE("UI: pointerOverUI answers the last widget hit test")
{
    ContentManager cm;
    {
        HE::UIWidgetTree tree;
        const int btn = tree.add(HE::UIWidgetType::Button);
        HE::uiSetAnchorPreset(*tree.find(btn), 0);   // top-left
        tree.find(btn)->pivotX = 0.0f; tree.find(btn)->pivotY = 0.0f;
        tree.find(btn)->posX   = 0.0f; tree.find(btn)->posY   = 0.0f;
        tree.find(btn)->sizeX  = 200.0f; tree.find(btn)->sizeY = 50.0f;
        UIWidgetAsset a;
        a.treeJson = HE::uiWidgetTreeToJson(tree);
        a.path     = "mem://api_pointer.hasset";
        cm.registerWidget(std::move(a));
    }

    HorizonWorld world;
    Ctx c{ &world, nullptr, &cm };
    REQUIRE(HE::api::widget::create(c, "mem://api_pointer.hasset") != 0);

    const auto* row = HE::api::find("ui.pointerOverUI");
    REQUIRE(row != nullptr);
    CHECK(row->isExec == false);        // a per-frame snapshot, like the input rows
    auto over = [&]{ return row->invoke(c, {})[0].b; };

    CHECK(over() == false);             // no pointer seen yet

    // The authored canvas is 1920x1080 and stretches onto the same viewport, so
    // canvas units and pixels line up: (100,25) is inside the button.
    world.widgets().processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true);
    CHECK(over() == true);
    world.widgets().processPointer(1920.0f, 1080.0f, 1900.0f, 1000.0f, false, true);
    CHECK(over() == false);

    // Captured mouse (FPS look) means no cursor at all, so nothing is over the UI.
    world.widgets().processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, false);
    CHECK(over() == false);

    // …and a torn-down world leaves no stale verdict behind.
    world.widgets().processPointer(1920.0f, 1080.0f, 100.0f, 25.0f, false, true);
    REQUIRE(over() == true);
    world.clear();
    CHECK(over() == false);

    Ctx none{};
    CHECK(HE::api::ui::pointerOverUI(none) == false);
}

// ═══ EngineCall node: interpreter routes through the registry ═════════════════

namespace
{
    namespace HC = HorizonCode;

    // A Context whose callApi dispatches to the HE::api registry against `api`,
    // with a local variable store for Get/SetVariable.
    HC::Context makeApiContext(HE::api::Ctx& api, std::unordered_map<std::string, Value>& vars)
    {
        HC::Context ctx;
        ctx.getVariable = [&vars](const std::string& n){ auto it = vars.find(n); return it != vars.end() ? it->second : Value{}; };
        ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
        ctx.callApi = [&api](const std::string& id, const std::vector<Value>& args) -> std::vector<Value> {
            const HE::api::ApiFn* fn = HE::api::find(id);
            return fn ? fn->invoke(api, args) : std::vector<Value>{};
        };
        return ctx;
    }
}

TEST_CASE("EngineCall: an exec engine call runs and feeds its result downstream")
{
    HorizonWorld world;
    HE::api::Ctx api{ &world, nullptr, nullptr };

    HC::Graph g;
    // Event "Run" (exec out at pin 0)
    HC::Node ev; ev.type = NT::Event; ev.s = "Run";
    const int evId = g.addNode(ev);
    // ConstInt 0 (parent) + ConstString "Made" (name)
    HC::Node ci; ci.type = NT::ConstInt; ci.f[0] = 0.0f;   const int ciId = g.addNode(ci);
    HC::Node cs; cs.type = NT::ConstString; cs.s = "Made"; const int csId = g.addNode(cs);
    // EngineCall entity.spawn (exec): pins [execIn 0][execOut 1][parent 2][name 3][entity out 4]
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "entity.spawn"; ec.hasArg = true;
    ec.params  = { { "parent", P::Int }, { "name", P::String } };
    ec.results = { { "entity", P::Int } };
    const int ecId = g.addNode(ec);
    // SetVariable "spawned" (Int): pins [execIn 0][execOut 1][Value 2][Value out 3]
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "spawned"; sv.propType = P::Int;
    const int svId = g.addNode(sv);

    REQUIRE(g.connect(evId, 0, ecId, 0));   // event exec → spawn exec-in
    REQUIRE(g.connect(ciId, 0, ecId, 2));   // 0 → parent
    REQUIRE(g.connect(csId, 0, ecId, 3));   // "Made" → name
    REQUIRE(g.connect(ecId, 1, svId, 0));   // spawn exec-out → setVar exec-in
    REQUIRE(g.connect(ecId, 4, svId, 2));   // spawned entity → setVar value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx = makeApiContext(api, vars);
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    // The variable holds the spawned entity id, and the world knows that entity.
    REQUIRE(vars.count("spawned") == 1);
    const auto spawned = (HE::api::Entity)vars["spawned"].i;
    CHECK(spawned != 0);
    CHECK(HE::api::entity::getName(api, spawned) == "Made");
}

TEST_CASE("EngineCall: a pure engine call evaluates on demand (no exec pin)")
{
    HE::api::Ctx api{};   // math needs no world

    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    // Three constants feeding clamp(5, 0, 3)
    HC::Node x;  x.type  = NT::ConstFloat; x.f[0]  = 5.0f; const int xId  = g.addNode(x);
    HC::Node lo; lo.type = NT::ConstFloat; lo.f[0] = 0.0f; const int loId = g.addNode(lo);
    HC::Node hi; hi.type = NT::ConstFloat; hi.f[0] = 3.0f; const int hiId = g.addNode(hi);
    // Pure EngineCall math.clamp: no exec pins → dataIns [x 0][lo 1][hi 2], dataOut [result 3]
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "math.clamp"; ec.hasArg = false;
    ec.params  = { { "x", P::Float }, { "lo", P::Float }, { "hi", P::Float } };
    ec.results = { { "result", P::Float } };
    const int ecId = g.addNode(ec);
    // SetVariable "r": pins [execIn 0][execOut 1][Value 2][Value out 3]
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "r"; sv.propType = P::Float;
    const int svId = g.addNode(sv);

    REQUIRE(g.connect(evId, 0, svId, 0));   // event exec → setVar exec-in
    REQUIRE(g.connect(xId,  0, ecId, 0));   // 5 → x
    REQUIRE(g.connect(loId, 0, ecId, 1));   // 0 → lo
    REQUIRE(g.connect(hiId, 0, ecId, 2));   // 3 → hi
    REQUIRE(g.connect(ecId, 3, svId, 2));   // clamp result → setVar value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx = makeApiContext(api, vars);
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("r") == 1);
    CHECK(vars["r"].f == doctest::Approx(3.0f));
}

TEST_CASE("EngineCall: signatureOf reflects isExec + mirrored params/results")
{
    HC::Node exec; exec.type = NT::EngineCall; exec.hasArg = true;
    exec.params = { { "entity", P::Int }, { "position", P::Color } };
    const auto es = HC::signatureOf(exec);
    CHECK(es.execIns.size() == 1);          // exec node has flow pins
    CHECK(es.execOuts.size() == 1);
    CHECK(es.dataIns.size() == 2);

    HC::Node pure; pure.type = NT::EngineCall; pure.hasArg = false;
    pure.params  = { { "x", P::Float } };
    pure.results = { { "result", P::Float } };
    const auto ps = HC::signatureOf(pure);
    CHECK(ps.execIns.empty());              // pure node is a compact data chip
    CHECK(ps.execOuts.empty());
    CHECK(ps.dataIns.size() == 1);
    CHECK(ps.dataOuts.size() == 1);
}

TEST_CASE("EngineCall: round-trips through JSON (type by name + mirrored signature)")
{
    HC::Graph g;
    HC::Node ec; ec.type = NT::EngineCall; ec.s = "transform.setPosition"; ec.hasArg = true;
    ec.params = { { "entity", P::Int }, { "position", P::Color } };
    g.addNode(ec);

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.nodes.size() == 1);
    CHECK(loaded.nodes[0].type == NT::EngineCall);
    CHECK(loaded.nodes[0].s == "transform.setPosition");
    CHECK(loaded.nodes[0].hasArg == true);            // isExec survives
    REQUIRE(loaded.nodes[0].params.size() == 2);
    CHECK(loaded.nodes[0].params[1].type == P::Color);
}

// ═══ Random library (seeded, bounded, reproducible) ══════════════════════════

TEST_CASE("Random: seeded → reproducible; ranges bounded; chance extremes")
{
    Ctx c{};   // no engine state needed
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // Same seed → same sequence.
    call("random.seed", { Value::ofInt(1234) });
    const float a0 = call("random.value", {})[0].f;
    const float a1 = call("random.value", {})[0].f;
    call("random.seed", { Value::ofInt(1234) });
    CHECK(call("random.value", {})[0].f == doctest::Approx(a0));
    CHECK(call("random.value", {})[0].f == doctest::Approx(a1));

    // value() ∈ [0, 1).
    for (int i = 0; i < 200; ++i)
    { const float v = call("random.value", {})[0].f; CHECK(v >= 0.0f); CHECK(v < 1.0f); }

    // Degenerate ranges collapse to the endpoint.
    CHECK(call("random.range",    { Value::ofFloat(5.0f), Value::ofFloat(5.0f) })[0].f == doctest::Approx(5.0f));
    CHECK(call("random.rangeInt", { Value::ofInt(7),      Value::ofInt(7) })[0].i == 7);

    // rangeInt is inclusive on both ends; swaps reversed bounds.
    for (int i = 0; i < 200; ++i)
    { const int v = call("random.rangeInt", { Value::ofInt(6), Value::ofInt(1) })[0].i; CHECK(v >= 1); CHECK(v <= 6); }

    // chance(1) always true, chance(0) always false.
    CHECK(call("random.chance", { Value::ofFloat(1.0f) })[0].b == true);
    CHECK(call("random.chance", { Value::ofFloat(0.0f) })[0].b == false);
}

// ═══ Time / frame clock ═══════════════════════════════════════════════════════

TEST_CASE("Time: advancing the clock reflects through the registry")
{
    Ctx c{};
    auto call = [&](const char* id){ return HE::api::find(id)->invoke(c, {}); };

    HE::api::time::reset();
    CHECK(call("time.frameCount")[0].i == 0);
    CHECK(call("time.elapsed")[0].f == doctest::Approx(0.0f));

    HE::api::time::advance(0.5f);
    HE::api::time::advance(0.25f);
    CHECK(call("time.deltaTime")[0].f  == doctest::Approx(0.25f)); // last dt
    CHECK(call("time.elapsed")[0].f    == doctest::Approx(0.75f)); // accumulated
    CHECK(call("time.frameCount")[0].i == 2);

    HE::api::time::reset();
    CHECK(call("time.elapsed")[0].f    == doctest::Approx(0.0f));
    CHECK(call("time.frameCount")[0].i == 0);
}

TEST_CASE("Time: the time scale dilates delta and elapsed, never the raw frame time")
{
    Ctx c{};
    auto call    = [&](const char* id){ return HE::api::find(id)->invoke(c, {}); };
    auto setScale = [&](float s){ HE::api::find("time.setTimeScale")->invoke(c, { Value::ofFloat(s) }); };

    HE::api::time::reset();
    CHECK(call("time.timeScale")[0].f == doctest::Approx(1.0f)); // play never starts scaled

    // Fast-forward: gameplay sees 2× the time, the app's own frame time is untouched.
    setScale(2.0f);
    HE::api::time::advance(0.5f);
    CHECK(call("time.deltaTime")[0].f         == doctest::Approx(1.0f));
    CHECK(call("time.unscaledDeltaTime")[0].f == doctest::Approx(0.5f));
    CHECK(call("time.elapsed")[0].f           == doctest::Approx(1.0f)); // scaled, like deltaTime

    // Pause: the game clock stands still while frames keep coming.
    setScale(0.0f);
    HE::api::time::advance(0.5f);
    CHECK(call("time.deltaTime")[0].f         == doctest::Approx(0.0f));
    CHECK(call("time.unscaledDeltaTime")[0].f == doctest::Approx(0.5f));
    CHECK(call("time.elapsed")[0].f           == doctest::Approx(1.0f)); // did not move
    CHECK(call("time.frameCount")[0].i        == 2);                     // frames still count

    // Clamped in the C++ setter, so every frontend inherits the same bounds.
    setScale(-3.0f);
    CHECK(call("time.timeScale")[0].f == doctest::Approx(0.0f));
    setScale(1000.0f);
    CHECK(call("time.timeScale")[0].f == doctest::Approx(HE::api::time::kMaxTimeScale));

    // A play-start reset lifts a pause: nobody may inherit the last session's scale.
    HE::api::time::reset();
    CHECK(call("time.timeScale")[0].f == doctest::Approx(1.0f));
    HE::api::time::advance(0.25f);
    CHECK(call("time.deltaTime")[0].f == doctest::Approx(0.25f));
}

// ═══ Input snapshot ═══════════════════════════════════════════════════════════

TEST_CASE("Input: the pushed snapshot reflects through the registry")
{
    Ctx c{};
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    HE::api::input::clear();
    CHECK(call("input.keyDown", { Value::ofString("W") })[0].b == false);

    HE::api::input::setKeysDown({ "W", "Space" });
    HE::api::input::setMouse({ 12.0f, 34.0f }, { 1.0f, -2.0f }, (1u << 0) | (1u << 2), 3.0f);

    CHECK(call("input.keyDown", { Value::ofString("W") })[0].b     == true);
    CHECK(call("input.keyDown", { Value::ofString("Space") })[0].b == true);
    CHECK(call("input.keyDown", { Value::ofString("A") })[0].b     == false);
    CHECK(call("input.mouseButton", { Value::ofInt(0) })[0].b == true);   // left
    CHECK(call("input.mouseButton", { Value::ofInt(1) })[0].b == false);  // right
    CHECK(call("input.mouseButton", { Value::ofInt(2) })[0].b == true);   // middle

    const Value pos = call("input.mousePosition", {})[0];
    CHECK(pos.v2.x == doctest::Approx(12.0f));
    CHECK(pos.v2.y == doctest::Approx(34.0f));
    const Value d = call("input.mouseDelta", {})[0];
    CHECK(d.v2.x == doctest::Approx(1.0f));
    CHECK(d.v2.y == doctest::Approx(-2.0f));
    CHECK(call("input.scrollDelta", {})[0].f == doctest::Approx(3.0f));

    HE::api::input::clear();
}

TEST_CASE("Input mode: three exec rows, a readable name, and GameAndUI by default")
{
    Ctx c{};
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // The default matters more than it looks: it is what every project that
    // never touches the mode gets, and it has to be the behaviour that existed
    // before modes did, or upgrading changes a game nobody edited.
    HE::api::input::setMode(HE::api::input::Mode::GameAndUI);
    CHECK(HE::api::input::mode() == HE::api::input::Mode::GameAndUI);
    CHECK(call("input.mode", {})[0].s == "GameAndUI");

    call("input.setModeUIOnly", {});
    CHECK(HE::api::input::mode() == HE::api::input::Mode::UIOnly);
    CHECK(call("input.mode", {})[0].s == "UIOnly");

    call("input.setModeGameOnly", {});
    CHECK(HE::api::input::mode() == HE::api::input::Mode::GameOnly);
    CHECK(call("input.mode", {})[0].s == "GameOnly");

    call("input.setModeGameAndUI", {});
    CHECK(HE::api::input::mode() == HE::api::input::Mode::GameAndUI);

    // The mode is a decision, the snapshot is this frame's readings. Pushing a
    // frame must not undo the decision — the reason the mode does not live in
    // Snapshot, and the regression this check exists for.
    call("input.setModeUIOnly", {});
    HE::api::input::clear();
    CHECK(HE::api::input::mode() == HE::api::input::Mode::UIOnly);

    HE::api::input::setMode(HE::api::input::Mode::GameAndUI);
}

TEST_CASE("Input: the pushed gamepad state reflects through the registry")
{
    Ctx c{};
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    HE::api::input::clear();
    CHECK(call("input.gamepadConnected", {})[0].b == false);
    CHECK(call("input.gamepadButton", { Value::ofString("a") })[0].b == false);
    CHECK(call("input.gamepadAxis", { Value::ofString("leftx") })[0].f == doctest::Approx(0.0f));

    // SDL axis order: leftx, lefty, rightx, righty, lefttrigger, righttrigger.
    float axes[6] = { 0.5f, -0.25f, 0.0f, 0.0f, 1.0f, 0.0f };
    bool  buttons[16] = {};
    buttons[0]  = true; // "a" (south)
    buttons[11] = true; // "dpup"
    HE::api::input::setGamepad(true, axes, 6, buttons, 16);

    CHECK(call("input.gamepadConnected", {})[0].b == true);
    CHECK(call("input.gamepadButton", { Value::ofString("a") })[0].b    == true);
    CHECK(call("input.gamepadButton", { Value::ofString("dpup") })[0].b == true);
    CHECK(call("input.gamepadButton", { Value::ofString("b") })[0].b    == false);
    CHECK(call("input.gamepadButton", { Value::ofString("not_a_button") })[0].b == false);
    CHECK(call("input.gamepadAxis", { Value::ofString("leftx") })[0].f == doctest::Approx(0.5f));
    CHECK(call("input.gamepadAxis", { Value::ofString("lefty") })[0].f == doctest::Approx(-0.25f));
    CHECK(call("input.gamepadAxis", { Value::ofString("lefttrigger") })[0].f == doctest::Approx(1.0f));
    CHECK(call("input.gamepadAxis", { Value::ofString("bogus") })[0].f == doctest::Approx(0.0f));

    // clear() wipes the pad too — a stale stick must not survive a session end.
    HE::api::input::clear();
    CHECK(call("input.gamepadConnected", {})[0].b == false);
    CHECK(call("input.gamepadAxis", { Value::ofString("leftx") })[0].f == doctest::Approx(0.0f));
}

// ═══ Transform value type ═════════════════════════════════════════════════════

TEST_CASE("Transform: ConstTransform flows through Set as a Transform value")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    HC::Node ct; ct.type = NT::ConstTransform;
    ct.tpos = { 1.0f, 2.0f, 3.0f }; ct.trot = { 10.0f, 20.0f, 30.0f }; ct.tscl = { 4.0f, 5.0f, 6.0f };
    const int ctId = g.addNode(ct);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "t"; sv.propType = P::Transform;
    const int svId = g.addNode(sv);
    REQUIRE(g.connect(evId, 0, svId, 0));   // exec
    REQUIRE(g.connect(ctId, 0, svId, 2));   // ConstTransform out → SetVariable value

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("t") == 1);
    CHECK(vars["t"].type == P::Transform);
    CHECK(vars["t"].tpos.x == doctest::Approx(1.0f));
    CHECK(vars["t"].trot.y == doctest::Approx(20.0f));
    CHECK(vars["t"].tscl.z == doctest::Approx(6.0f));
}

TEST_CASE("Transform: ConstTransform round-trips through JSON")
{
    HC::Graph g;
    HC::Node ct; ct.type = NT::ConstTransform;
    ct.tpos = { 1.0f, 2.0f, 3.0f }; ct.trot = { 4.0f, 5.0f, 6.0f }; ct.tscl = { 7.0f, 8.0f, 9.0f };
    g.addNode(ct);

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.nodes.size() == 1);
    CHECK(loaded.nodes[0].type == NT::ConstTransform);
    CHECK(loaded.nodes[0].tpos.y == doctest::Approx(2.0f));
    CHECK(loaded.nodes[0].trot.z == doctest::Approx(6.0f));
    CHECK(loaded.nodes[0].tscl.x == doctest::Approx(7.0f));
}

TEST_CASE("Transform: a Transform variable defaults to identity")
{
    HC::Variable v; v.name = "xf"; v.type = P::Transform;
    const Value d = HC::variableDefaultValue(v);
    CHECK(d.type == P::Transform);
    CHECK(d.tpos.x == doctest::Approx(0.0f));
    CHECK(d.trot.x == doctest::Approx(0.0f));
    CHECK(d.tscl.x == doctest::Approx(1.0f));
    CHECK(d.tscl.z == doctest::Approx(1.0f));
}

TEST_CASE("Transform: an edited variable default seeds the value + survives JSON")
{
    HC::Graph g;
    HC::Variable v; v.name = "spawnAt"; v.type = P::Transform;
    v.tpos = { 5.0f, 6.0f, 7.0f }; v.trot = { 0.0f, 90.0f, 0.0f }; v.tscl = { 2.0f, 2.0f, 2.0f };
    g.variables.push_back(v);

    const Value d = HC::variableDefaultValue(g.variables[0]);
    CHECK(d.tpos.x == doctest::Approx(5.0f));
    CHECK(d.trot.y == doctest::Approx(90.0f));
    CHECK(d.tscl.z == doctest::Approx(2.0f));

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.variables.size() == 1);
    CHECK(loaded.variables[0].tpos.z == doctest::Approx(7.0f));
    CHECK(loaded.variables[0].trot.y == doctest::Approx(90.0f));
    CHECK(loaded.variables[0].tscl.x == doctest::Approx(2.0f));
}

// ═══ Array variables + array-op nodes ═════════════════════════════════════════

TEST_CASE("Array: an array variable defaults to empty; round-trips through JSON")
{
    HC::Graph g;
    HC::Variable v; v.name = "nums"; v.type = P::Int; v.isArray = true;
    g.variables.push_back(v);

    const Value d = HC::variableDefaultValue(v);
    CHECK(d.isArray == true);
    CHECK(d.type == P::Int);
    CHECK(d.items.empty());

    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.variables.size() == 1);
    CHECK(loaded.variables[0].isArray == true);
    CHECK(loaded.variables[0].type == P::Int);
}

TEST_CASE("Array: connect rejects an array pin joined to a scalar pin")
{
    HC::Graph g;
    HC::Node mk; mk.type = NT::ArrayMake; mk.propType = P::Int; const int mkId = g.addNode(mk);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "n"; sv.propType = P::Int; /* isArray=false */
    const int svId = g.addNode(sv);
    // mk out (Int[]) → SetVariable value (Int scalar) must be rejected.
    CHECK(g.connect(mkId, 0, svId, 2) == false);
    // But an Int[] SetVariable accepts it.
    HC::Node sa; sa.type = NT::SetVariable; sa.s = "a"; sa.propType = P::Int; sa.isArray = true;
    const int saId = g.addNode(sa);
    CHECK(g.connect(mkId, 0, saId, 2) == true);
}

TEST_CASE("Array: Make → Add chain → Length + Get evaluate correctly")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    auto konst = [&](int val){ HC::Node c; c.type = NT::ConstInt; c.f[0] = (float)val; return g.addNode(c); };
    const int c10 = konst(10), c20 = konst(20), c30 = konst(30), ci1 = konst(1);
    auto arrNode = [&](NT t){ HC::Node n; n.type = t; n.propType = P::Int; return g.addNode(n); };
    const int mk  = arrNode(NT::ArrayMake);
    const int a1  = arrNode(NT::ArrayAdd);
    const int a2  = arrNode(NT::ArrayAdd);
    const int a3  = arrNode(NT::ArrayAdd);
    const int len = arrNode(NT::ArrayLength);
    const int get = arrNode(NT::ArrayGet);
    HC::Node sL; sL.type = NT::SetVariable; sL.s = "len"; sL.propType = P::Int; const int sLId = g.addNode(sL);
    HC::Node sE; sE.type = NT::SetVariable; sE.s = "el";  sE.propType = P::Int; const int sEId = g.addNode(sE);

    // Build the array: []→[10]→[10,20]→[10,20,30]. Add pins: array-in 0, value-in 1, array-out 2.
    REQUIRE(g.connect(mk, 0, a1, 0)); REQUIRE(g.connect(c10, 0, a1, 1));
    REQUIRE(g.connect(a1, 2, a2, 0)); REQUIRE(g.connect(c20, 0, a2, 1));
    REQUIRE(g.connect(a2, 2, a3, 0)); REQUIRE(g.connect(c30, 0, a3, 1));
    // Length (out pin 1) + Get index 1 (array-in 0, index-in 1, out pin 2).
    REQUIRE(g.connect(a3, 2, len, 0));
    REQUIRE(g.connect(a3, 2, get, 0)); REQUIRE(g.connect(ci1, 0, get, 1));
    // Drive via exec: Run → set len → set el.
    REQUIRE(g.connect(evId, 0, sLId, 0)); REQUIRE(g.connect(len, 1, sLId, 2));
    REQUIRE(g.connect(sLId, 1, sEId, 0)); REQUIRE(g.connect(get, 2, sEId, 2));

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    REQUIRE(vars.count("len") == 1);
    REQUIRE(vars.count("el") == 1);
    CHECK(vars["len"].i == 3);   // three elements
    CHECK(vars["el"].i == 20);   // element at index 1
}

TEST_CASE("Array: Set / Insert / Remove / Contains / IndexOf evaluate correctly")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    auto konst = [&](int val){ HC::Node c; c.type = NT::ConstInt; c.f[0] = (float)val; return g.addNode(c); };
    auto arrNode = [&](NT t){ HC::Node n; n.type = t; n.propType = P::Int; return g.addNode(n); };
    auto setVar = [&](const char* name){ HC::Node s; s.type = NT::SetVariable; s.s = name; s.propType = P::Int; return g.addNode(s); };

    // Base array [10, 20, 30].
    const int mk = arrNode(NT::ArrayMake);
    const int a1 = arrNode(NT::ArrayAdd), a2 = arrNode(NT::ArrayAdd), a3 = arrNode(NT::ArrayAdd);
    const int c10 = konst(10), c20 = konst(20), c30 = konst(30);
    REQUIRE(g.connect(mk, 0, a1, 0)); REQUIRE(g.connect(c10, 0, a1, 1));
    REQUIRE(g.connect(a1, 2, a2, 0)); REQUIRE(g.connect(c20, 0, a2, 1));
    REQUIRE(g.connect(a2, 2, a3, 0)); REQUIRE(g.connect(c30, 0, a3, 1));

    // Set [1] = 99 → [10,99,30]; Insert 55 at 0 → [55,10,99,30]; Remove [3] → [55,10,99].
    const int st_ = arrNode(NT::ArraySet);    // pins: arr 0, idx 1, val 2, out 3
    const int ins = arrNode(NT::ArrayInsert); // pins: arr 0, idx 1, val 2, out 3
    const int rem = arrNode(NT::ArrayRemove); // pins: arr 0, idx 1, out 2
    const int c0 = konst(0), c1 = konst(1), c3 = konst(3), c55 = konst(55), c99 = konst(99);
    REQUIRE(g.connect(a3, 2, st_, 0)); REQUIRE(g.connect(c1, 0, st_, 1)); REQUIRE(g.connect(c99, 0, st_, 2));
    REQUIRE(g.connect(st_, 3, ins, 0)); REQUIRE(g.connect(c0, 0, ins, 1)); REQUIRE(g.connect(c55, 0, ins, 2));
    REQUIRE(g.connect(ins, 3, rem, 0)); REQUIRE(g.connect(c3, 0, rem, 1));

    // Probe the result: len, element 2 (=99), IndexOf 55 (=0), Contains 30 (removed → false).
    const int len = arrNode(NT::ArrayLength);   // arr 0, out 1
    const int get = arrNode(NT::ArrayGet);      // arr 0, idx 1, out 2
    const int idx = arrNode(NT::ArrayIndexOf);  // arr 0, val 1, out 2
    const int has = arrNode(NT::ArrayContains); // arr 0, val 1, out 2
    const int c2 = konst(2);
    REQUIRE(g.connect(rem, 2, len, 0));
    REQUIRE(g.connect(rem, 2, get, 0)); REQUIRE(g.connect(c2,  0, get, 1));
    REQUIRE(g.connect(rem, 2, idx, 0)); REQUIRE(g.connect(c55, 0, idx, 1));
    REQUIRE(g.connect(rem, 2, has, 0)); REQUIRE(g.connect(c30, 0, has, 1));

    const int sLen = setVar("len"), sEl = setVar("el"), sIdx = setVar("idx");
    HC::Node sb; sb.type = NT::SetVariable; sb.s = "has"; sb.propType = P::Bool; const int sHas = g.addNode(sb);
    REQUIRE(g.connect(evId, 0, sLen, 0)); REQUIRE(g.connect(len, 1, sLen, 2));
    REQUIRE(g.connect(sLen, 1, sEl, 0));  REQUIRE(g.connect(get, 2, sEl, 2));
    REQUIRE(g.connect(sEl, 1, sIdx, 0));  REQUIRE(g.connect(idx, 2, sIdx, 2));
    REQUIRE(g.connect(sIdx, 1, sHas, 0)); REQUIRE(g.connect(has, 2, sHas, 2));

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    CHECK(vars["len"].i == 3);       // [55, 10, 99]
    CHECK(vars["el"].i  == 99);      // element 2
    CHECK(vars["idx"].i == 0);       // 55 is first
    CHECK(vars["has"].b == false);   // 30 was removed
}

TEST_CASE("Array: For Each runs the body per element and Done afterwards")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    auto konst = [&](int val){ HC::Node c; c.type = NT::ConstInt; c.f[0] = (float)val; return g.addNode(c); };
    auto arrNode = [&](NT t){ HC::Node n; n.type = t; n.propType = P::Int; return g.addNode(n); };

    // Array [5, 6, 7].
    const int mk = arrNode(NT::ArrayMake);
    const int a1 = arrNode(NT::ArrayAdd), a2 = arrNode(NT::ArrayAdd), a3 = arrNode(NT::ArrayAdd);
    const int c5 = konst(5), c6 = konst(6), c7 = konst(7);
    REQUIRE(g.connect(mk, 0, a1, 0)); REQUIRE(g.connect(c5, 0, a1, 1));
    REQUIRE(g.connect(a1, 2, a2, 0)); REQUIRE(g.connect(c6, 0, a2, 1));
    REQUIRE(g.connect(a2, 2, a3, 0)); REQUIRE(g.connect(c7, 0, a3, 1));

    // ForEach pins: execIn 0, Body 1, Done 2, Array-in 3, Element-out 4, Index-out 5.
    const int fe = arrNode(NT::ForEach);
    REQUIRE(g.connect(evId, 0, fe, 0));
    REQUIRE(g.connect(a3, 2, fe, 3));

    // Body: el = Element (Int), then sum = sum + el (Add is Float-typed; the Int
    // element coerces through the Float read of the variable).
    HC::Node se; se.type = NT::SetVariable; se.s = "el"; se.propType = P::Int; const int seId = g.addNode(se);
    HC::Node gv; gv.type = NT::GetVariable; gv.s = "sum"; gv.propType = P::Float; const int gvId = g.addNode(gv);
    HC::Node ge; ge.type = NT::GetVariable; ge.s = "el";  ge.propType = P::Float; const int geId = g.addNode(ge);
    HC::Node ad; ad.type = NT::Add; const int adId = g.addNode(ad);           // A 0, B 1, out 2
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "sum"; sv.propType = P::Float; const int svId = g.addNode(sv);
    REQUIRE(g.connect(fe, 1, seId, 0));       // Body exec → set el
    REQUIRE(g.connect(fe, 4, seId, 2));       // Element → el
    REQUIRE(g.connect(seId, 1, svId, 0));     // then → set sum
    REQUIRE(g.connect(gvId, 0, adId, 0));     // sum → A
    REQUIRE(g.connect(geId, 0, adId, 1));     // el → B
    REQUIRE(g.connect(adId, 2, svId, 2));     // sum + el → sum

    // Done: set "done" = index count sentinel 1.
    HC::Node sd; sd.type = NT::SetVariable; sd.s = "done"; sd.propType = P::Float; const int sdId = g.addNode(sd);
    HC::Node c1f; c1f.type = NT::ConstFloat; c1f.f[0] = 1.0f; const int c1fId = g.addNode(c1f);
    REQUIRE(g.connect(fe, 2, sdId, 0));
    REQUIRE(g.connect(c1fId, 0, sdId, 2));

    std::unordered_map<std::string, Value> vars;
    vars["sum"] = Value::ofFloat(0.0f);
    std::string history;
    HC::Context ctx;
    ctx.getVariable = [&vars](const std::string& n){ auto it = vars.find(n); return it != vars.end() ? it->second : Value{}; };
    ctx.setVariable = [&vars, &history](const std::string& n, const Value& v){
        vars[n] = v;
        history += n + "=" + std::to_string(v.type == HorizonCode::PinType::Int ? (float)v.i : v.f) + ";"; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);

    INFO("history: ", history);
    CHECK(vars["sum"].f == doctest::Approx(18.0f));   // 5 + 6 + 7
    CHECK(vars["done"].f == doctest::Approx(1.0f));   // Done fired after the loop
}

TEST_CASE("Array: default slots seed the variable and survive JSON")
{
    HC::Graph g;
    HC::Variable v; v.name = "nums"; v.type = P::Int; v.isArray = true;
    v.defaultItems.push_back(Value::ofInt(7));
    v.defaultItems.push_back(Value::ofInt(8));
    v.defaultItems.push_back(Value::ofInt(9));
    g.variables.push_back(v);

    // The default seeds a filled array (not an empty one anymore).
    const Value d = HC::variableDefaultValue(g.variables[0]);
    CHECK(d.isArray == true);
    REQUIRE(d.items.size() == 3);
    CHECK(d.items[0].i == 7);
    CHECK(d.items[2].i == 9);

    // Slots round-trip through JSON (typed per element).
    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.variables.size() == 1);
    REQUIRE(loaded.variables[0].defaultItems.size() == 3);
    CHECK(loaded.variables[0].defaultItems[1].i == 8);

    // A Transform-element array round-trips its 9 components per slot.
    HC::Graph g2;
    HC::Variable t; t.name = "xfs"; t.type = P::Transform; t.isArray = true;
    Value xf = Value::ofTransform({ 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 });
    t.defaultItems.push_back(xf);
    g2.variables.push_back(t);
    HC::Graph loaded2;
    REQUIRE(HC::fromJson(HC::toJson(g2), loaded2));
    REQUIRE(loaded2.variables[0].defaultItems.size() == 1);
    CHECK(loaded2.variables[0].defaultItems[0].trot.y == doctest::Approx(5.0f));
    CHECK(loaded2.variables[0].defaultItems[0].tscl.z == doctest::Approx(9.0f));
}

TEST_CASE("Array: For Each adopts the wired array's element type + class")
{
    HC::Graph g;
    // A String array source and a fresh (Float-default) ForEach.
    HC::Node mk; mk.type = NT::ArrayMake; mk.propType = P::String; const int mkId = g.addNode(mk);
    HC::Node fe; fe.type = NT::ForEach; /* propType defaults Float */ const int feId = g.addNode(fe);
    // Pre-wire the Element output (Float) somewhere — it must drop on retype.
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "x"; sv.propType = P::Float; const int svId = g.addNode(sv);
    REQUIRE(g.connect(feId, 4, svId, 2));   // Element (Float) → set x (Float)

    // Without adoption a String[] → Float[] connect is rejected.
    CHECK(g.connect(mkId, 0, feId, 3) == false);

    // With adoption the ForEach retypes to String, the stale Element link drops,
    // and the connect succeeds.
    HC::adoptForEachElementType(g, mkId, 0, feId, 3);
    CHECK(g.findNode(feId)->propType == P::String);
    CHECK(g.connect(mkId, 0, feId, 3) == true);
    bool elementLinkAlive = false;
    for (const auto& l : g.links) if (l.srcNode == feId && l.srcPin == 4) elementLinkAlive = true;
    CHECK(elementLinkAlive == false);

    // Object arrays carry the element class onto the ForEach (member menus).
    HC::Graph g2;
    HC::Variable ov; ov.name = "objs"; ov.type = P::Ref; ov.isArray = true;
    ov.className = "Classes/Enemy.hasset";
    g2.variables.push_back(ov);
    HC::Node gv; gv.type = NT::GetVariable; gv.s = "objs"; gv.propType = P::Ref; gv.isArray = true;
    const int gvId = g2.addNode(gv);
    HC::Node fe2; fe2.type = NT::ForEach; const int fe2Id = g2.addNode(fe2);
    HC::adoptForEachElementType(g2, gvId, 0, fe2Id, 3);   // GetVariable dataOut = pin 0
    CHECK(g2.findNode(fe2Id)->propType == P::Ref);
    CHECK(g2.findNode(fe2Id)->s == "Classes/Enemy.hasset");
    CHECK(g2.connect(gvId, 0, fe2Id, 3) == true);
}

// ═══ String / Camera / Environment / Entity-query / Audio groups ══════════════

TEST_CASE("String: the registry's string library evaluates correctly")
{
    Ctx c{};
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };
    auto S = [](const char* s){ return Value::ofString(s); };

    CHECK(call("string.length",    { S("hello") })[0].i == 5);
    CHECK(call("string.substring", { S("hello world"), Value::ofInt(6), Value::ofInt(5) })[0].s == "world");
    CHECK(call("string.contains",  { S("hello"), S("ell") })[0].b == true);
    CHECK(call("string.find",      { S("hello"), S("lo") })[0].i == 3);
    CHECK(call("string.find",      { S("hello"), S("xyz") })[0].i == -1);
    CHECK(call("string.replace",   { S("a-b-c"), S("-"), S("+") })[0].s == "a+b+c");
    CHECK(call("string.toUpper",   { S("MiXeD") })[0].s == "MIXED");
    CHECK(call("string.toLower",   { S("MiXeD") })[0].s == "mixed");
    CHECK(call("string.trim",      { S("  pad  ") })[0].s == "pad");
    CHECK(call("string.startsWith",{ S("hello"), S("he") })[0].b == true);
    CHECK(call("string.endsWith",  { S("hello"), S("lo") })[0].b == true);
    CHECK(call("string.toNumber",  { S("3.5") })[0].f == doctest::Approx(3.5f));
    CHECK(call("string.toNumber",  { S("nope") })[0].f == doctest::Approx(0.0f));
}

TEST_CASE("Camera + Environment: registry knobs reach the world's components")
{
    HorizonWorld world;
    auto camE = world.createEntity("Cam");
    TransformComponent tc; tc.position = { 1, 2, 3 };
    world.registry().emplace<TransformComponent>(camE, tc);
    CameraComponent cc; cc.isMain = true; cc.fovDegrees = 60.0f;
    world.registry().emplace<CameraComponent>(camE, cc);
    auto envE = world.createEntity("Env");
    world.registry().emplace<EnvironmentComponent>(envE);

    Ctx c{ &world, nullptr, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    // Camera transform + fov round-trip through the registry. The setter is fed a
    // COLOR on purpose — that is the shape a pre-Vec3 graph still sends.
    CHECK(call("camera.getPosition", {})[0].v3.y == doctest::Approx(2.0f));
    call("camera.setPosition", { Value::ofColor({ 9, 8, 7, 0 }) });
    CHECK(world.registry().get<TransformComponent>(camE).position.x == doctest::Approx(9.0f));
    call("camera.setFov", { Value::ofFloat(90.0f) });
    CHECK(call("camera.getFov", {})[0].f == doctest::Approx(90.0f));

    // Environment knobs write into the EnvironmentComponent.
    call("env.setTimeOfDay",     { Value::ofFloat(0.25f) });
    call("env.setCloudCoverage", { Value::ofFloat(0.8f) });
    call("env.setFogDensity",    { Value::ofFloat(0.1f) });
    call("env.setWindSpeed",     { Value::ofFloat(4.0f) });
    const auto& env = world.registry().get<EnvironmentComponent>(envE);
    CHECK(env.timeOfDay == doctest::Approx(0.25f));
    CHECK(env.cloudCoverage == doctest::Approx(0.8f));
    CHECK(env.fogDensity == doctest::Approx(0.1f));
    CHECK(call("env.getWindSpeed", {})[0].f == doctest::Approx(4.0f));

    // The X-list generation covers EVERY component field and all four kinds:
    // float (above), bool, int and RGB colour. A colour stays a Color pin even
    // though it is three floats — it wants the editor's colour picker, not three
    // number fields — and it reads back OPAQUE.
    call("env.setNebulaCoverage", { Value::ofFloat(0.9f) });
    CHECK(env.nebulaCoverage == doctest::Approx(0.9f));
    CHECK(call("env.getNebulaCoverage", {})[0].f == doctest::Approx(0.9f));
    call("env.setDayNightCycle", { Value::ofBool(true) });
    CHECK(env.dayNightCycle == true);
    CHECK(call("env.getDayNightCycle", {})[0].b == true);
    call("env.setNebulaQuality", { Value::ofInt(2) });
    CHECK(env.nebulaQuality == 2);
    CHECK(call("env.getNebulaQuality", {})[0].i == 2);
    call("env.setNebulaColor2", { Value::ofColor({ 0.1f, 0.2f, 0.3f, 0.0f }) });
    CHECK(env.nebulaColor2.y == doctest::Approx(0.2f));
    CHECK(call("env.getNebulaColor2", {})[0].col.z == doctest::Approx(0.3f));
    CHECK(call("env.getNebulaColor2", {})[0].col.w == doctest::Approx(1.0f));
    // Registry sanity: one Get + one Set row per field, all in "Environment".
    int envRows = 0;
    for (const auto& fn : HE::api::registry())
        if (std::string(fn.id).rfind("env.", 0) == 0) ++envRows;
    CHECK(envRows == 2 * (39 + 6 + 4 + 9));   // float + bool + int + colour fields
}

TEST_CASE("Entity query: findByName + exists through the registry")
{
    HorizonWorld world;
    auto hero = world.createEntity("Hero");
    Ctx c{ &world, nullptr, nullptr };
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };

    CHECK((uint32_t)call("entity.findByName", { Value::ofString("Hero") })[0].i == (uint32_t)hero);
    CHECK(call("entity.findByName", { Value::ofString("Nobody") })[0].i == 0);
    CHECK(call("entity.exists", { Value::ofInt((int)(uint32_t)hero) })[0].b == true);
    CHECK(call("entity.exists", { Value::ofInt(123456) })[0].b == false);
}

TEST_CASE("Audio: null-tolerant without an engine; headless engine answers queries")
{
    // No engine bound → everything no-ops / returns neutral.
    Ctx none{};
    auto calln = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(none, a); };
    CHECK(calln("audio.play", { Value::ofString("Sounds/x.hasset") })[0].i == 0);
    CHECK(calln("audio.isPlaying", { Value::ofInt(1) })[0].b == false);
    CHECK_NOTHROW(calln("audio.stopAll", {}));

    // Headless engine (no device): bus + query paths run without touching assets.
    AudioEngine engine;
    REQUIRE(engine.init(/*noDevice=*/true));
    Ctx c{}; c.audio = &engine;
    auto call = [&](const char* id, std::vector<Value> a){ return HE::api::find(id)->invoke(c, a); };
    CHECK_NOTHROW(call("audio.setBusVolume", { Value::ofString("SFX"), Value::ofFloat(0.5f) }));
    CHECK(call("audio.isPlaying", { Value::ofInt(42) })[0].b == false);
    CHECK_NOTHROW(call("audio.stopAll", {}));
    engine.shutdown();
}

// ═══ Debug draw queue ═════════════════════════════════════════════════════════

TEST_CASE("Debug draw: timed primitives live for their duration, then expire")
{
    HE::api::debug::clear();
    HE::api::debug::line({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, 0.0f);   // one frame
    HE::api::debug::sphere({ 0, 0, 0 }, 1.0f, { 0, 1, 0 }, 0.5f);        // half a second
    std::vector<DebugLine> out;
    HE::api::debug::collect(0.1f, out);
    const size_t firstFrame = out.size();
    CHECK(firstFrame > 1);                        // the line + the sphere's segments

    out.clear();
    HE::api::debug::collect(0.1f, out);
    CHECK(out.size() == firstFrame - 1);          // the 0s line expired, sphere lives

    out.clear();
    HE::api::debug::collect(1.0f, out);           // sphere still alive THIS collect
    CHECK(out.size() == firstFrame - 1);
    out.clear();
    HE::api::debug::collect(0.0f, out);           // now everything has expired
    CHECK(out.empty());
}

// ═══ fs sandbox + save store ══════════════════════════════════════════════════

TEST_CASE("fs: sandboxed I/O works inside the root and rejects escapes")
{
    const auto root = std::filesystem::temp_directory_path() / "he_api_fs_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    HE::api::fs::setSandboxRoot(root.string());

    CHECK(HE::api::fs::writeText("notes/hello.txt", "hi there"));
    CHECK(HE::api::fs::exists("notes/hello.txt"));
    CHECK(HE::api::fs::readText("notes/hello.txt") == "hi there");
    CHECK(HE::api::fs::makeDir("sub/dir"));
    CHECK(HE::api::fs::remove("notes/hello.txt"));
    CHECK(!HE::api::fs::exists("notes/hello.txt"));

    // Escapes are rejected outright.
    CHECK(!HE::api::fs::writeText("../outside.txt", "nope"));
    CHECK(!HE::api::fs::writeText("a/../../outside.txt", "nope"));
    CHECK(!HE::api::fs::writeText("/tmp/abs.txt", "nope"));
    CHECK(HE::api::fs::readText("../secret").empty());

    std::filesystem::remove_all(root, ec);
}

// A ContentManager holding one in-memory SaveGameTemplate ("mem://tpl") with
// hp (Float, 100), title (String, "Rookie"), hardcore (Bool), stats (Struct →
// a registered PlayerStats def). Shared by the save-v2 cases below.
namespace
{
struct SaveTestRig
{
    ContentManager cm;
    std::filesystem::path root;
    static constexpr const char* kStatsDef = "Content/T/SaveStats.hasset";

    SaveTestRig() : root(std::filesystem::temp_directory_path() / "he_api_save_test")
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        HE::api::fs::setSandboxRoot(root.string());
        HE::api::save::close();

        HE::StructDef stats;
        stats.name = "SaveStats"; stats.assetPath = kStatsDef;
        {
            HE::StructField lvl; lvl.name = "level"; lvl.type = HorizonCode::PinType::Int;
            lvl.defaultValue = HE::api::Value::ofInt(1);
            stats.fields = { lvl };
        }
        HE::TypeRegistry::instance().registerStruct(stats);

        HE::StructDef tpl;
        {
            HE::StructField hp; hp.name = "hp"; hp.type = HorizonCode::PinType::Float;
            hp.defaultValue = HE::api::Value::ofFloat(100.0f);
            HE::StructField title; title.name = "title"; title.type = HorizonCode::PinType::String;
            title.defaultValue = HE::api::Value::ofString("Rookie");
            HE::StructField hc; hc.name = "hardcore"; hc.type = HorizonCode::PinType::Bool;
            HE::StructField st; st.name = "stats"; st.type = HorizonCode::PinType::Struct;
            st.typeName = kStatsDef;
            tpl.fields = { hp, title, hc, st };
        }
        SaveGameTemplateAsset a;
        a.name = "MainTemplate";
        a.path = "mem://save_template";
        a.json = HE::TypeRegistry::structToJson(tpl);
        cm.registerSaveGameTemplate(std::move(a));
        HE::api::save::setDefaultTemplate("mem://save_template");
    }
    ~SaveTestRig()
    {
        HE::api::save::close();
        HE::api::save::setDefaultTemplate("");
        HE::TypeRegistry::instance().removeType(kStatsDef);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
} // namespace

TEST_CASE("save v2: create seeds template defaults; typed access validates loud")
{
    SaveTestRig rig;
    namespace save = HE::api::save;

    // No active save → every accessor fails to its default.
    CHECK(save::activeId().empty());
    CHECK(!save::setNumber("hp", 1.0f));
    CHECK(save::getNumber("hp", -7.0f) == doctest::Approx(-7.0f));

    // Ids are filenames — reject anything outside [A-Za-z0-9_-].
    CHECK(!save::create("../evil", &rig.cm));
    CHECK(!save::create("has space", &rig.cm));
    CHECK(!save::create("", &rig.cm));

    REQUIRE(save::create("slot1", &rig.cm));
    CHECK(save::activeId() == "slot1");
    CHECK(save::fields() == std::vector<std::string>{ "hp", "title", "hardcore", "stats" });

    // Template defaults are live before any set.
    CHECK(save::getNumber("hp", 0) == doctest::Approx(100.0f));
    CHECK(save::getString("title", "") == "Rookie");
    CHECK(save::getBool("hardcore", true) == false);
    const HE::api::Value stats = save::getStructV("stats");
    REQUIRE(stats.type == HorizonCode::PinType::Struct);
    REQUIRE(stats.items.size() == 1);
    CHECK(stats.items[0].i == 1);

    // Unknown fields and type mismatches fail loud, never write.
    CHECK(!save::setNumber("mana", 5.0f));
    CHECK(!save::setNumber("title", 5.0f));
    CHECK(!save::setString("hp", "nope"));

    // Struct writes validate the definition.
    HE::api::Value wrong; wrong.type = HorizonCode::PinType::Struct;
    wrong.typeName = "Content/T/Other.hasset";
    CHECK(!save::setStructV("stats", wrong));
}

TEST_CASE("save v2: write/load round-trip through Saves/<id>.json, list/exists/delete")
{
    SaveTestRig rig;
    namespace save = HE::api::save;

    REQUIRE(save::create("run-42", &rig.cm));
    CHECK(save::setNumber("hp", 37.5f));
    CHECK(save::setString("title", "Veteran"));
    CHECK(save::setBool("hardcore", true));
    HE::api::Value stats = save::getStructV("stats");
    stats.items[0] = HE::api::Value::ofInt(9);
    CHECK(save::setStructV("stats", stats));
    CHECK(save::setEntityState("uuid-1", "{\"x\":1.5}"));

    CHECK(!save::exists("run-42"));
    REQUIRE(save::write());
    CHECK(save::exists("run-42"));
    CHECK(save::list() == std::vector<std::string>{ "run-42" });

    // A fresh load restores fields, struct contents and entity state.
    save::close();
    CHECK(save::activeId().empty());
    REQUIRE(save::load("run-42", &rig.cm));
    CHECK(save::getNumber("hp", 0) == doctest::Approx(37.5f));
    CHECK(save::getString("title", "") == "Veteran");
    CHECK(save::getBool("hardcore", false) == true);
    CHECK(save::getStructV("stats").items[0].i == 9);
    CHECK(save::hasEntityState("uuid-1"));
    CHECK(!save::hasEntityState("uuid-2"));
    CHECK(save::entityState("uuid-1").find("1.5") != std::string::npos);

    // Loading a save that does not exist fails loud.
    CHECK(!save::load("nope", &rig.cm));

    CHECK(save::remove("run-42"));
    CHECK(!save::exists("run-42"));
    CHECK(save::list().empty());
}

TEST_CASE("save v2: Set and Map fields survive the disk round trip IN ORDER")
{
    // A save is a StructDef, so a container field goes through the same encoder
    // every struct field does. What is worth asserting is the ORDER: written as
    // a JSON object, a map's keys would come back alphabetised (nlohmann's
    // object is a sorted std::map) and the whole insertion-order contract would
    // be quietly broken by persistence alone.
    namespace save = HE::api::save;
    using P = HorizonCode::PinType;
    using CK = HorizonCode::ContainerKind;
    const char* kBagDef = "Content/T/SaveBag.hasset";
    const auto root = std::filesystem::temp_directory_path() / "he_api_save_ctr_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    HE::api::fs::setSandboxRoot(root.string());
    save::close();

    HE::StructDef bag;
    bag.name = "SaveBag"; bag.assetPath = kBagDef;
    {
        HE::StructField tags;
        tags.name = "tags"; tags.type = P::String;
        tags.isArray = true; tags.container = CK::Set;
        tags.defaultValue.isArray = true; tags.defaultValue.container = CK::Set;
        HE::StructField ammo;
        ammo.name = "ammo"; ammo.type = P::Int;
        ammo.isArray = true; ammo.container = CK::Map; ammo.keyType = P::String;
        ammo.defaultValue.isArray = true; ammo.defaultValue.container = CK::Map;
        bag.fields = { tags, ammo };
    }
    HE::TypeRegistry::instance().registerStruct(bag);

    ContentManager cm;
    {
        HE::StructDef tpl;
        HE::StructField b; b.name = "bag"; b.type = P::Struct; b.typeName = kBagDef;
        tpl.fields = { b };
        SaveGameTemplateAsset a;
        a.name = "BagTemplate";
        a.path = "mem://save_bag_template";
        a.json = HE::TypeRegistry::structToJson(tpl);
        cm.registerSaveGameTemplate(std::move(a));
        save::setDefaultTemplate("mem://save_bag_template");
    }

    REQUIRE(save::create("bagrun", &cm));
    {
        HE::api::Value v = save::getStructV("bag");
        REQUIRE(v.items.size() == 2);
        v.items[0] = HorizonCode::Value::ofSet(P::String);
        v.items[0].items = { HorizonCode::Value::ofString("zeta"),
                             HorizonCode::Value::ofString("alpha"),
                             HorizonCode::Value::ofString("zeta") };   // a duplicate
        v.items[1] = HorizonCode::Value::ofMap(P::String, P::Int);
        v.items[1].keys  = { HorizonCode::Value::ofString("zeta"),
                             HorizonCode::Value::ofString("alpha") };
        v.items[1].items = { HorizonCode::Value::ofInt(9), HorizonCode::Value::ofInt(1) };
        REQUIRE(save::setStructV("bag", v));
    }
    REQUIRE(save::write());
    save::close();
    REQUIRE(save::load("bagrun", &cm));

    const HE::api::Value back = save::getStructV("bag");
    REQUIRE(back.items.size() == 2);
    // The set kept its first-occurrence order and dropped the duplicate on the
    // way back in — a hand-edited save cannot smuggle one past the decoder.
    CHECK(back.items[0].kind() == CK::Set);
    REQUIRE(back.items[0].items.size() == 2);
    CHECK(back.items[0].items[0].s == "zeta");
    CHECK(back.items[0].items[1].s == "alpha");
    // The map came back in the order it was written, NOT alphabetised.
    CHECK(back.items[1].kind() == CK::Map);
    REQUIRE(back.items[1].keys.size() == 2);
    CHECK(back.items[1].keys[0].s == "zeta");
    CHECK(back.items[1].keys[1].s == "alpha");
    REQUIRE(back.items[1].items.size() == 2);
    CHECK(back.items[1].items[0].i == 9);
    CHECK(back.items[1].items[1].i == 1);

    save::close();
    save::setDefaultTemplate("");
    HE::TypeRegistry::instance().removeType(kBagDef);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("entity save-state: guarded round-trip through the active save")
{
    SaveTestRig rig;
    namespace save = HE::api::save;

    HorizonWorld world;
    HE::api::Ctx c{ &world, nullptr, &rig.cm };
    const auto e = world.createEntity("Hero");
    auto& reg = world.registry();
    {
        TransformComponent t;
        t.position = { 1.0f, 2.0f, 3.0f };
        t.rotation = { 0.0f, 90.0f, 0.0f };
        reg.emplace<TransformComponent>(e, t);
    }
    const HE::api::Entity handle = (HE::api::Entity)e;

    // Every guard fails LOUD before anything works: edit mode, then no active
    // save, then no component.
    save::setPlayMode(false);
    CHECK(!HE::api::entity::saveState(c, handle));
    save::setPlayMode(true);
    CHECK(!HE::api::entity::saveState(c, handle));          // no active save
    REQUIRE(save::create("run", &rig.cm));
    CHECK(!HE::api::entity::saveState(c, handle));          // no SaveStateComponent
    reg.emplace<SaveStateComponent>(e);

    CHECK(!HE::api::entity::hasSavedState(c, handle));
    REQUIRE(HE::api::entity::saveState(c, handle));
    CHECK(HE::api::entity::hasSavedState(c, handle));

    // Mutate, then re-apply — the saved state wins.
    reg.get<TransformComponent>(e).position = { 9.0f, 9.0f, 9.0f };
    REQUIRE(HE::api::entity::applySavedState(c, handle));
    CHECK(reg.get<TransformComponent>(e).position.x == doctest::Approx(1.0f));
    CHECK(reg.get<TransformComponent>(e).rotation.y == doctest::Approx(90.0f));

    // The entity section survives the disk round-trip keyed by the stable UUID.
    REQUIRE(save::write());
    save::close();
    REQUIRE(save::load("run", &rig.cm));
    CHECK(HE::api::entity::hasSavedState(c, handle));
    reg.get<TransformComponent>(e).position = { 5.0f, 5.0f, 5.0f };
    REQUIRE(HE::api::entity::applySavedState(c, handle));
    CHECK(reg.get<TransformComponent>(e).position.z == doctest::Approx(3.0f));

    save::setPlayMode(false);
}

// The game-side receiver, exactly as a GameLogic library defines it — the test
// binary plays the "loaded library" role here.
HE_IMPLEMENT_ENGINE_SERVICES()

TEST_CASE("save services: the C-ABI table drives the full save path for C++ GameLogic")
{
    SaveTestRig rig;

    HorizonWorld world;
    HorizonWorld* worldPtr = &world;
    const auto e = world.createEntity("Hero");
    world.registry().emplace<TransformComponent>(e);
    world.registry().emplace<SaveStateComponent>(e);
    HE::api::save::setPlayMode(true);

    // Before injection every wrapper is a safe no-op default.
    HE_SetEngineServices(nullptr);
    CHECK(!he::save::available());
    CHECK(!he::save::create("slot1"));
    CHECK(he::save::activeId().empty());

    HE::api::SaveServicesBinding binding;
    binding.world   = [&worldPtr]() { return worldPtr; };
    binding.content = &rig.cm;
    HeSaveServices table{};
    HE::api::fillSaveServices(table, &binding);
    HE_SetEngineServices(&table);
    REQUIRE(he::save::available());

    // An ABI mismatch is rejected, not half-used.
    HeSaveServices wrong = table;
    wrong.abiVersion = HE_SAVE_ABI_VERSION + 1;
    HE_SetEngineServices(&wrong);
    CHECK(!he::save::available());
    HE_SetEngineServices(&table);

    REQUIRE(he::save::create("cpp-run"));
    CHECK(he::save::activeId() == "cpp-run");
    CHECK(he::save::fields() ==
          std::vector<std::string>({ "hp", "title", "hardcore", "stats" }));
    CHECK(he::save::setNumber("hp", 12.0f));
    CHECK(he::save::getNumber("hp") == doctest::Approx(12.0f));
    CHECK(he::save::setString("title", "Native"));
    CHECK(he::save::getString("title") == "Native");
    // Struct fields cross as JSON text.
    CHECK(he::save::setStructJson("stats", "{\"__type\":\"Content/T/SaveStats.hasset\",\"level\":7}"));
    CHECK(he::save::getStructJson("stats").find("7") != std::string::npos);

    // Entity save-state through the table, entity found by name.
    const uint32_t handle = he::entity::findByName("Hero");
    CHECK(handle == (uint32_t)e);
    REQUIRE(he::entity::saveState(handle));
    CHECK(he::entity::hasSavedState(handle));
    world.registry().get<TransformComponent>(e).position.x = 5.0f;
    REQUIRE(he::entity::applySavedState(handle));
    CHECK(world.registry().get<TransformComponent>(e).position.x == doctest::Approx(0.0f));

    // Disk round-trip through the table.
    REQUIRE(he::save::write());
    he::save::close();
    CHECK(he::save::activeId().empty());
    REQUIRE(he::save::load("cpp-run"));
    CHECK(he::save::getNumber("hp") == doctest::Approx(12.0f));
    CHECK(he::save::list() == std::vector<std::string>({ "cpp-run" }));
    CHECK(he::save::remove("cpp-run"));

    HE_SetEngineServices(nullptr);
    HE::api::save::setPlayMode(false);
}

// ═══ Scene transition requests + packed-scene UUIDs + additive tracking ═══════

TEST_CASE("scene: requests queue in order; zone ids are unique")
{
    (void)HE::api::scene::takeRequests();   // drain leftovers
    HE::api::scene::load("Scenes/B.hescene");
    const int z1 = HE::api::scene::loadAdditive("Scenes/Zone1.hescene");
    const int z2 = HE::api::scene::loadAdditive("Scenes/Zone2.hescene");
    HE::api::scene::unloadZone(z1);
    CHECK(z1 != z2);

    const auto reqs = HE::api::scene::takeRequests();
    REQUIRE(reqs.size() == 4);
    CHECK(reqs[0].kind == 0); CHECK(reqs[0].path == "Scenes/B.hescene");
    CHECK(reqs[1].kind == 1); CHECK(reqs[1].zone == z1);
    CHECK(reqs[2].kind == 1); CHECK(reqs[2].zone == z2);
    CHECK(reqs[3].kind == 2); CHECK(reqs[3].zone == z1);
    CHECK(HE::api::scene::takeRequests().empty());   // drained
}

TEST_CASE("scene: zone table drives queries, visibility and position")
{
    HE::api::scene::clearZones();
    HorizonWorld world;
    // A fake zone: a root + two mesh children.
    auto root = world.createEntity("ZoneRoot");
    auto m1 = world.createEntity("M1");
    auto m2 = world.createEntity("M2");
    world.registry().emplace<TransformComponent>(root);
    world.registry().emplace<MeshComponent>(m1);
    world.registry().emplace<MeshComponent>(m2);

    HE::api::scene::ZoneInfo info;
    info.path = "Scenes/Zone.hescene";
    info.root = (uint32_t)root;
    info.entities = { (uint32_t)root, (uint32_t)m1, (uint32_t)m2 };
    HE::api::scene::noteZoneLoaded(7, std::move(info));

    Ctx c{ &world, nullptr, nullptr };
    // Queries.
    const auto zones = HE::api::scene::loadedZones();
    REQUIRE(zones.size() == 1);
    CHECK(zones[0] == 7);
    CHECK(HE::api::scene::zoneScene(7) == "Scenes/Zone.hescene");
    CHECK(HE::api::scene::zoneScene(99).empty());

    // Position: read + move the whole zone via its root.
    CHECK(HE::api::scene::zonePosition(c, 7).x == doctest::Approx(0.0f));
    HE::api::scene::setZonePosition(c, 7, { 100, 0, 50 });
    CHECK(world.registry().get<TransformComponent>(root).position.x == doctest::Approx(100.0f));
    CHECK(HE::api::scene::zonePosition(c, 7).z == doctest::Approx(50.0f));

    // Visibility: hide flips every mesh in the zone, show restores.
    HE::api::scene::setZoneVisible(c, 7, false);
    CHECK(world.registry().get<MeshComponent>(m1).visible == false);
    CHECK(world.registry().get<MeshComponent>(m2).visible == false);
    HE::api::scene::setZoneVisible(c, 7, true);
    CHECK(world.registry().get<MeshComponent>(m1).visible == true);

    HE::api::scene::noteZoneUnloaded(7);
    CHECK(HE::api::scene::loadedZones().empty());
}

TEST_CASE("scene: hidden flag rides the requests; registry rows expose zones as arrays")
{
    (void)HE::api::scene::takeRequests();
    HE::api::scene::load("Scenes/Next.hescene", /*hidden=*/true);
    const int z = HE::api::scene::loadAdditive("Scenes/Zone.hescene", /*hidden=*/true);
    HE::api::scene::activate();
    const auto reqs = HE::api::scene::takeRequests();
    REQUIRE(reqs.size() == 3);
    CHECK(reqs[0].kind == 0); CHECK(reqs[0].hidden == true);
    CHECK(reqs[1].kind == 1); CHECK(reqs[1].hidden == true); CHECK(reqs[1].zone == z);
    CHECK(reqs[2].kind == 3);

    // loadedZones through the REGISTRY returns an Int array Value.
    HE::api::scene::clearZones();
    HE::api::scene::noteZoneLoaded(3, { "A.hescene", 0, {} });
    HE::api::scene::noteZoneLoaded(5, { "B.hescene", 0, {} });
    Ctx c{};
    const auto res = HE::api::find("scene.loadedZones")->invoke(c, {});
    REQUIRE(res.size() == 1);
    CHECK(res[0].isArray == true);
    CHECK(res[0].type == P::Int);
    REQUIRE(res[0].items.size() == 2);
    CHECK(res[0].items[0].i == 3);
    CHECK(res[0].items[1].i == 5);
    // …and the descriptor marks the result pin as an array (editor pins follow).
    CHECK(HE::api::find("scene.loadedZones")->results[0].isArray == true);
    HE::api::scene::clearZones();
}

TEST_CASE("scene: queued show/move requests + additive placement ride the queue")
{
    (void)HE::api::scene::takeRequests();
    const int z = HE::api::scene::loadAdditive("Scenes/Z.hescene", /*hidden=*/true, { 100, 0, 50 });
    HE::api::scene::requestZoneVisible(z, true);        // Show Zone (queued)
    HE::api::scene::requestZonePosition(z, { 5, 6, 7 });// Set Zone Position (queued)
    const auto reqs = HE::api::scene::takeRequests();
    REQUIRE(reqs.size() == 3);
    CHECK(reqs[0].kind == 1);
    CHECK(reqs[0].pos.x == doctest::Approx(100.0f));    // placement rides the load
    CHECK(reqs[1].kind == 4); CHECK(reqs[1].zone == z); CHECK(reqs[1].flag == true);
    CHECK(reqs[2].kind == 5); CHECK(reqs[2].pos.z == doctest::Approx(7.0f));
}

TEST_CASE("Visibility: zone hiding + entity.setVisible cover every renderable type")
{
    HE::api::scene::clearZones();
    HorizonWorld world;
    auto e = world.createEntity("Multi");
    world.registry().emplace<MeshComponent>(e);
    world.registry().emplace<SkeletalMeshComponent>(e);
    world.registry().emplace<LightComponent>(e);
    world.registry().emplace<ParticleSystemComponent>(e);

    Ctx c{ &world, nullptr, nullptr };
    // Per-entity toggle through the registry flips all renderables at once.
    HE::api::find("entity.setVisible")->invoke(c, { Value::ofInt((int)(uint32_t)e), Value::ofBool(false) });
    CHECK(world.registry().get<MeshComponent>(e).visible == false);
    CHECK(world.registry().get<SkeletalMeshComponent>(e).visible == false);
    CHECK(world.registry().get<LightComponent>(e).visible == false);
    CHECK(world.registry().get<ParticleSystemComponent>(e).visible == false);
    CHECK(HE::api::find("entity.getVisible")->invoke(c, { Value::ofInt((int)(uint32_t)e) })[0].b == false);

    // Zone-level show restores them all.
    HE::api::scene::ZoneInfo info;
    info.root = (uint32_t)e;
    info.entities = { (uint32_t)e };
    HE::api::scene::noteZoneLoaded(11, std::move(info));
    HE::api::scene::setZoneVisible(c, 11, true);
    CHECK(world.registry().get<SkeletalMeshComponent>(e).visible == true);
    CHECK(world.registry().get<LightComponent>(e).visible == true);
    CHECK(world.registry().get<ParticleSystemComponent>(e).visible == true);
    HE::api::scene::clearZones();
}

TEST_CASE("EngineCall: array-typed result pins mirror onto the node signature")
{
    HC::Node n; n.type = NT::EngineCall; n.s = "scene.loadedZones"; n.hasArg = false;
    n.results.push_back({ "zones", P::Int, /*isArray=*/true });
    const auto sig = HC::signatureOf(n);
    REQUIRE(sig.dataOuts.size() == 1);
    CHECK(sig.dataOuts[0].isArray == true);   // wires straight into For Each / array ops

    // The flag survives the params/results JSON round-trip.
    HC::Graph g; g.addNode(n);
    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    REQUIRE(loaded.nodes.size() == 1);
    REQUIRE(loaded.nodes[0].results.size() == 1);
    CHECK(loaded.nodes[0].results[0].isArray == true);
}

TEST_CASE("sceneUuidForPath: deterministic, path-sensitive, separator-normalized")
{
    const HE::UUID a  = sceneUuidForPath("Scenes/Level1.hescene");
    const HE::UUID a2 = sceneUuidForPath("Scenes/Level1.hescene");
    const HE::UUID b  = sceneUuidForPath("Scenes/Level2.hescene");
    const HE::UUID w  = sceneUuidForPath("Scenes\\Level1.hescene");   // Windows separators
    CHECK(a.hi == a2.hi); CHECK(a.lo == a2.lo);
    CHECK((a.hi != b.hi || a.lo != b.lo));
    CHECK(a.hi == w.hi);  CHECK(a.lo == w.lo);
}

TEST_CASE("SceneSerializer: additive load reports the created entities")
{
    // Author a small scene and snapshot it.
    HorizonWorld src;
    src.createEntity("A");
    src.createEntity("B");
    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(src, bytes));

    // Merge it additively into a world that already has content.
    HorizonWorld dst;
    dst.createEntity("Existing");
    std::vector<Entity> created;
    REQUIRE(ser.loadAdditiveFromMemory(dst, bytes, &created));
    CHECK(created.size() >= 2);   // A + B (+ the merged scene's sub-root)
    for (Entity e : created) CHECK(dst.registry().valid(e));
}

// ═══ Real audio playback (guarded: passes silently where no device exists) ═════

TEST_CASE("Audio: real-device playback of a generated tone (skipped without a device)")
{
    AudioEngine engine;
    if (!engine.init(/*noDevice=*/false))
    {
        MESSAGE("no audio device — playback smoke skipped");
        return;
    }
    // 100ms 440Hz sine, mono int16 @ 44100.
    const int rate = 44100, samples = rate / 10;
    std::vector<uint8_t> pcm(samples * 2);
    for (int i = 0; i < samples; ++i)
    {
        const float s = std::sin(2.0f * 3.14159265f * 440.0f * (float)i / (float)rate);
        const int16_t v = (int16_t)(s * 12000.0f);
        std::memcpy(&pcm[i * 2], &v, 2);
    }
    const uint64_t h = engine.play(pcm, rate, 1, 0.3f);
    CHECK(h != 0);
    CHECK(engine.isPlaying(h));
    engine.stop(h);
    CHECK(!engine.isPlaying(h));
    engine.shutdown();
}

// ═══ Inline pin defaults (constants without literal nodes) ════════════════════

TEST_CASE("Pin defaults: unwired inputs use their inline default; wires win")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    // Add node (A + B, both unwired): A defaults to 4, B to 2.5 → 6.5.
    HC::Node ad; ad.type = NT::Add; const int adId = g.addNode(ad);
    g.findNode(adId)->pinDefaults[0] = Value::ofFloat(4.0f);
    g.findNode(adId)->pinDefaults[1] = Value::ofFloat(2.5f);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "r"; sv.propType = P::Float; const int svId = g.addNode(sv);
    REQUIRE(g.connect(evId, 0, svId, 0));
    REQUIRE(g.connect(adId, 2, svId, 2));

    // Wire A from a literal — the wire must beat the default.
    HC::Node cf; cf.type = NT::ConstFloat; cf.f[0] = 10.0f; const int cfId = g.addNode(cf);
    REQUIRE(g.connect(cfId, 0, adId, 0));

    std::unordered_map<std::string, Value> vars;
    HC::Context ctx;
    ctx.setVariable = [&vars](const std::string& n, const Value& v){ vars[n] = v; };
    HC::Runner runner(g, ctx);
    runner.fireEvent("Run", 0);
    CHECK(vars["r"].f == doctest::Approx(12.5f));   // wired 10 + default 2.5

    // Defaults survive the JSON round-trip (typed per entry).
    HC::Graph loaded;
    REQUIRE(HC::fromJson(HC::toJson(g), loaded));
    const HC::Node* addLoaded = loaded.findNode(adId);
    REQUIRE(addLoaded);
    REQUIRE(addLoaded->pinDefaults.count(1) == 1);
    CHECK(addLoaded->pinDefaults.at(1).f == doctest::Approx(2.5f));
}

// ═══ Duplicate nodes (editor Duplicate command) ═══════════════════════════════

TEST_CASE("duplicateNodes clones the set + internal links, skips Event/FunctionEntry")
{
    HC::Graph g;
    HC::Node ev; ev.type = NT::Event; ev.s = "Run"; const int evId = g.addNode(ev);
    HC::Node cf; cf.type = NT::ConstFloat; cf.f[0] = 5.0f; const int cfId = g.addNode(cf);
    HC::Node sv; sv.type = NT::SetVariable; sv.s = "x"; sv.propType = P::Float; const int svId = g.addNode(sv);
    REQUIRE(g.connect(evId, 0, svId, 0));   // external exec (event → set)
    REQUIRE(g.connect(cfId, 0, svId, 2));   // internal data (const → set), both cloned

    const size_t linksBefore = g.links.size();
    const std::vector<int> fresh = HC::duplicateNodes(g, { evId, cfId, svId });

    // Event skipped; two clones with fresh ids and offset positions.
    REQUIRE(fresh.size() == 2);
    CHECK(fresh[0] != cfId);
    const HC::Node* cfClone = g.findNode(fresh[0]);
    const HC::Node* svClone = g.findNode(fresh[1]);
    REQUIRE(cfClone); REQUIRE(svClone);
    CHECK(cfClone->type == NT::ConstFloat);
    CHECK(cfClone->f[0] == doctest::Approx(5.0f));       // payload rides along
    CHECK(cfClone->x == doctest::Approx(28.0f));          // offset from 0
    CHECK(svClone->s == "x");

    // Exactly ONE new link: the internal const→set. The event→set exec link is
    // NOT cloned (the event was skipped, and externals stay on the originals).
    CHECK(g.links.size() == linksBefore + 1);
    bool cloneLink = false;
    for (const auto& l : g.links)
        if (l.srcNode == fresh[0] && l.dstNode == fresh[1]) cloneLink = true;
    CHECK(cloneLink == true);
}

// ── The engine classes' inherited member surface ────────────────────────────

TEST_CASE("every engine base-class member names a real registry function")
{
    // The member table (HorizonCode.h engineClasses) spells its members as
    // HE::api registry IDS, because the editor turns a picked member into an
    // Engine Call node pre-wired to the reference the menu was opened on — which
    // is what makes "inherited members" cost no dispatch machinery at all.
    //
    // Those ids are strings in a table one module away from the registry, so a
    // typo would be perfectly silent: the member would just never appear. This
    // is the test that makes it loud.
    for (const auto& c : HorizonCode::engineClasses())
    {
        for (const auto& m : c.members)
        {
            INFO("class ", c.name, " member ", m.label, " -> ", m.apiId);
            const HE::api::ApiFn* fn = HE::api::find(m.apiId);
            REQUIRE(fn != nullptr);
            // The target parameter has to exist AND be a reference — the editor
            // wires the object into it without asking.
            REQUIRE(m.targetParam >= 0);
            REQUIRE(m.targetParam < (int)fn->params.size());
            CHECK(fn->params[m.targetParam].type == HorizonCode::PinType::Ref);
        }
    }
}

TEST_CASE("possession is one controller per character, both ways round")
{
    using namespace HE::api;
    player::clear();

    CHECK(player::possessed(1) == 0u);
    CHECK(player::controllerOf(2) == 0u);

    player::possess(1, 2);
    CHECK(player::possessed(1) == 2u);
    CHECK(player::controllerOf(2) == 1u);

    // A second controller taking the same character must take it AWAY from the
    // first — leaving both entries would make controllerOf answer with whichever
    // the map happened to hash first.
    player::possess(3, 2);
    CHECK(player::possessed(3) == 2u);
    CHECK(player::possessed(1) == 0u);
    CHECK(player::controllerOf(2) == 3u);

    player::unpossess(3);
    CHECK(player::possessed(3) == 0u);
    CHECK(player::controllerOf(2) == 0u);

    // controller()/character() are the "just give me the player" shorthands and
    // read from the session's registered controllers.
    player::setControllers({ 7 });
    player::possess(7, 8);
    CHECK(player::controller() == 7u);
    CHECK(player::character() == 8u);

    player::clear();
    CHECK(player::controller() == 0u);
    CHECK(player::character() == 0u);
}
