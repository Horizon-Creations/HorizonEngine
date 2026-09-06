#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/CameraRigController.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/CameraShake.h>
#include <HorizonScene/EngineApi.h>   // the script rows, exercised as scripts reach them
#include <Application/Input.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

struct Rig {
    HorizonWorld world;
    Entity target = entt::null;
    Entity camera = entt::null;

    CameraRigComponent& rig()      { return world.registry().get<CameraRigComponent>(camera); }
    TransformComponent& camXform() { return world.registry().get<TransformComponent>(camera); }
    TransformComponent& tgtXform() { return world.registry().get<TransformComponent>(target); }

    // The context a script call arrives with. Going through HE::api rather than
    // straight at the component is deliberate for a handful of cases below: the
    // registry rows are what Lua, Python and HorizonCode actually reach, and
    // glue that nothing exercises is glue that rots. It lives here rather than
    // in a helper because api::Ctx is taken by non-const reference.
    HE::api::Ctx ctx;
    HE::api::Ctx& api() { ctx.world = &world; return ctx; }
};

// A target at the origin and a rig camera pointed straight ahead. pitch is zeroed
// (the component defaults to a slight downward tilt) so the expected positions
// below are plain axis arithmetic rather than trigonometry.
std::unique_ptr<Rig> makeRig(CameraRigComponent::Mode mode = CameraRigComponent::Mode::ThirdPerson)
{
    auto r = std::make_unique<Rig>();

    r->target = r->world.createEntity("Target");
    r->world.addComponent(r->target, TransformComponent{});

    r->camera = r->world.createEntity("Camera");
    r->world.addComponent(r->camera, TransformComponent{});
    CameraComponent cam; cam.isMain = true;
    r->world.addComponent(r->camera, cam);

    CameraRigComponent rig;
    rig.mode   = mode;
    rig.target = r->world.entityId(r->target);
    rig.yaw    = 0.0f;
    rig.pitch  = 0.0f;
    r->world.addComponent(r->camera, rig);

    return r;
}

constexpr MouseFrame kNoMouse{};

} // namespace

// ─── Placement ────────────────────────────────────────────────────────────────

TEST_CASE("CameraRig: third person puts the camera on a boom behind the pivot")
{
    auto r = makeRig(CameraRigComponent::Mode::ThirdPerson);
    r->rig().pivotOffset = { 0.0f, 1.6f, 0.0f };
    r->rig().armOffset   = { 0.4f, 0.0f, 0.0f };
    r->rig().armLength   = 4.0f;

    const auto f = HE::CameraRigController::update(r->world, kNoMouse);
    REQUIRE(f.driven);

    // Looking down -Z, so the boom trails on +Z.
    CHECK(r->camXform().position.x == doctest::Approx(0.4f));
    CHECK(r->camXform().position.y == doctest::Approx(1.6f));
    CHECK(r->camXform().position.z == doctest::Approx(4.0f));
}

TEST_CASE("CameraRig: first person puts the camera at the pivot")
{
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    r->rig().pivotOffset = { 0.0f, 1.7f, 0.0f };
    // Arm settings must be ignored in first person, not merely defaulted away.
    r->rig().armOffset = { 5.0f, 5.0f, 5.0f };
    r->rig().armLength = 9.0f;

    HE::CameraRigController::update(r->world, kNoMouse);

    CHECK(r->camXform().position.x == doctest::Approx(0.0f));
    CHECK(r->camXform().position.y == doctest::Approx(1.7f));
    CHECK(r->camXform().position.z == doctest::Approx(0.0f));
}

TEST_CASE("CameraRig: the camera follows the target when it moves")
{
    auto r = makeRig();
    r->rig().armLength   = 4.0f;
    r->rig().armOffset   = {};
    r->rig().pivotOffset = {};

    r->tgtXform().position = { 10.0f, 0.0f, -5.0f };
    HE::CameraRigController::update(r->world, kNoMouse);

    CHECK(r->camXform().position.x == doctest::Approx(10.0f));
    CHECK(r->camXform().position.z == doctest::Approx(-1.0f));
}

// ─── Look ─────────────────────────────────────────────────────────────────────

TEST_CASE("CameraRig: mouse motion turns the rig and the pitch clamps")
{
    auto r = makeRig();
    r->rig().sensitivity = 0.1f;

    MouseFrame m; m.dx = 10.0f; m.dy = 0.0f;
    HE::CameraRigController::update(r->world, m);
    CHECK(r->rig().yaw == doctest::Approx(-1.0f));

    // Far past the limit in one frame — a fast flick must clamp, not wrap.
    MouseFrame down; down.dy = 10000.0f;
    HE::CameraRigController::update(r->world, down);
    CHECK(r->rig().pitch == doctest::Approx(r->rig().pitchMin));

    MouseFrame up; up.dy = -10000.0f;
    HE::CameraRigController::update(r->world, up);
    CHECK(r->rig().pitch == doctest::Approx(r->rig().pitchMax));
}

TEST_CASE("CameraRig: stick look is a rate — dt-scaled, unlike the mouse")
{
    auto r = makeRig();
    r->rig().stickSensitivity = 90.0f;   // quarter turn per second at full tilt

    HE::CameraLookInput look;
    look.stickX = 1.0f;
    look.dt     = 0.5f;
    HE::CameraRigController::update(r->world, look);
    CHECK(r->rig().yaw == doctest::Approx(-45.0f)); // 90°/s * 0.5s, mouse sign convention

    // Same deflection, half the dt → half the turn. THE property that
    // separates a rate from a displacement.
    auto r2 = makeRig();
    r2->rig().stickSensitivity = 90.0f;
    look.dt = 0.25f;
    HE::CameraRigController::update(r2->world, look);
    CHECK(r2->rig().yaw == doctest::Approx(-22.5f));
}

TEST_CASE("CameraRig: stick pitch follows mouse convention and invert flips it")
{
    auto r = makeRig();
    r->rig().stickSensitivity = 90.0f;
    r->rig().pitch = 0.0f;

    HE::CameraLookInput look;
    look.stickY = 1.0f;   // SDL: stick pushed DOWN
    look.dt     = 0.1f;
    HE::CameraRigController::update(r->world, look);
    CHECK(r->rig().pitch == doctest::Approx(-9.0f)); // looks down, like mouse dy+

    r->rig().pitch = 0.0f;
    r->rig().stickInvertY = true;
    HE::CameraRigController::update(r->world, look);
    CHECK(r->rig().pitch == doctest::Approx(9.0f));
}

TEST_CASE("CameraRig: mouse and stick combine in one update")
{
    auto r = makeRig();
    r->rig().sensitivity      = 0.1f;
    r->rig().stickSensitivity = 90.0f;

    HE::CameraLookInput look;
    look.mouse.dx = 10.0f;   // -1° via mouse
    look.stickX   = 1.0f;    // -9° via stick
    look.dt       = 0.1f;
    HE::CameraRigController::update(r->world, look);
    CHECK(r->rig().yaw == doctest::Approx(-10.0f));
}

TEST_CASE("CameraRig: yaw stays in (-180, 180] instead of winding up")
{
    auto r = makeRig();
    r->rig().sensitivity = 1.0f;

    MouseFrame m; m.dx = -100.0f;   // +100 degrees per call
    for (int i = 0; i < 20; ++i)
        HE::CameraRigController::update(r->world, m);

    CHECK(r->rig().yaw <= 180.0f);
    CHECK(r->rig().yaw >  -180.0f);
}

// ─── Rotation coupling ────────────────────────────────────────────────────────

TEST_CASE("CameraRig: Free leaves the target's rotation alone")
{
    auto r = makeRig();
    r->rig().targetYaw = CameraRigComponent::TargetYaw::Free;
    r->tgtXform().rotation = { 0.0f, 33.0f, 0.0f };

    MouseFrame m; m.dx = 100.0f;
    HE::CameraRigController::update(r->world, m);

    CHECK(r->tgtXform().rotation.y == doctest::Approx(33.0f));
}

TEST_CASE("CameraRig: Follow turns the target with the camera")
{
    auto r = makeRig();
    r->rig().targetYaw   = CameraRigComponent::TargetYaw::Follow;
    r->rig().sensitivity = 0.5f;

    MouseFrame m; m.dx = -60.0f;   // +30 degrees
    HE::CameraRigController::update(r->world, m);

    CHECK(r->rig().yaw             == doctest::Approx(30.0f));
    CHECK(r->tgtXform().rotation.y == doctest::Approx(30.0f));
}

TEST_CASE("CameraRig: Follow writes yaw only, never pitch or roll")
{
    // The target's pitch/roll must survive byte-for-byte. Re-expressing them
    // through a quaternion round trip would pick an equivalent-but-different
    // representation near ±90° and flip the character while the player looks up.
    auto r = makeRig();
    r->rig().targetYaw = CameraRigComponent::TargetYaw::Follow;
    r->tgtXform().rotation = { 87.5f, 0.0f, -12.25f };

    MouseFrame m; m.dy = -10000.0f;   // drive pitch hard into its limit
    HE::CameraRigController::update(r->world, m);

    CHECK(r->tgtXform().rotation.x == 87.5f);
    CHECK(r->tgtXform().rotation.z == -12.25f);
}

// ─── Target resolution ────────────────────────────────────────────────────────

TEST_CASE("CameraRig: an empty target id falls back to the possessed player")
{
    auto r = makeRig();
    r->rig().target = HE::UUID{};        // "follow whoever the player is"
    r->rig().armLength = 4.0f;
    r->rig().armOffset = {};
    r->rig().pivotOffset = {};
    r->tgtXform().position = { 0.0f, 0.0f, 7.0f };

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, r->target);
    REQUIRE(f.driven);
    CHECK(f.target == r->target);
    CHECK(r->camXform().position.z == doctest::Approx(11.0f));
}

TEST_CASE("CameraRig: no target means not driven, so the caller can fall back")
{
    auto r = makeRig();
    r->rig().target = HE::UUID{};

    const auto f = HE::CameraRigController::update(r->world, kNoMouse);  // no fallback
    CHECK_FALSE(f.driven);
    CHECK(f.camera == r->camera);
    CHECK((f.target == entt::null));
}

TEST_CASE("CameraRig: a scene without a rig camera is not driven")
{
    HorizonWorld world;
    Entity cam = world.createEntity("PlainCamera");
    world.addComponent(cam, TransformComponent{});
    world.addComponent(cam, CameraComponent{});

    const auto f = HE::CameraRigController::update(world, kNoMouse);
    CHECK_FALSE(f.driven);
    CHECK((f.camera == entt::null));
}

TEST_CASE("CameraRig: isMain decides between two rig cameras")
{
    auto r = makeRig();
    // The first-created camera is not main; this one is.
    Entity second = r->world.createEntity("SecondCamera");
    r->world.addComponent(second, TransformComponent{});
    CameraComponent cam; cam.isMain = true;
    r->world.addComponent(second, cam);
    r->world.addComponent(second, CameraRigComponent{});

    r->world.registry().get<CameraComponent>(r->camera).isMain = false;

    CHECK(HE::CameraRigController::findRigCamera(r->world.registry()) == second);
}

// ─── Parented cameras ─────────────────────────────────────────────────────────

TEST_CASE("CameraRig: a camera parented to its own target still lands in the right place")
{
    // This is the shape a PlayerCharacter class ships: the camera is a CHILD of
    // the character it follows. The rig computes a world pose and stores it in
    // the camera's PARENT space, so a non-identity parent has to be divided back
    // out — and here the parent is the very thing that moved.
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 4.0f;

    REQUIRE(r->world.reparentEntity(r->camera, r->target));
    r->tgtXform().position = { 10.0f, 0.0f, -5.0f };

    const auto f = HE::CameraRigController::update(r->world, kNoMouse);
    REQUIRE(f.driven);

    // What matters is where the camera ends up in the WORLD; the local transform
    // is just how it is stored.
    const glm::vec3 worldPos = glm::vec3(r->camXform().worldMatrix[3]);
    CHECK(worldPos.x == doctest::Approx(10.0f));
    CHECK(worldPos.z == doctest::Approx(-1.0f));   // 4 behind the target on +Z
}

TEST_CASE("CameraRig: a rotated parent does not drag the camera with it")
{
    // The failure this guards: forgetting the parent-space conversion puts the
    // world pose into a local slot, and the parent's rotation then applies on
    // top — the boom swings out by the character's yaw. With coupling on, the
    // parent's yaw IS the camera's yaw, so the error doubles the rotation.
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 4.0f;
    r->rig().targetYaw   = CameraRigComponent::TargetYaw::Follow;
    r->rig().sensitivity = 1.0f;

    REQUIRE(r->world.reparentEntity(r->camera, r->target));

    MouseFrame m; m.dx = -90.0f;             // yaw 90° — target follows
    HE::CameraRigController::update(r->world, m);

    REQUIRE(r->tgtXform().rotation.y == doctest::Approx(90.0f));
    // yaw 90° looks down -X, so the boom trails on +X, not on +Z.
    const glm::vec3 worldPos = glm::vec3(r->camXform().worldMatrix[3]);
    CHECK(worldPos.x == doctest::Approx(4.0f).epsilon(0.01));
    CHECK(worldPos.z == doctest::Approx(0.0f).epsilon(0.01));
}

// ─── Boom collision ───────────────────────────────────────────────────────────

namespace {

// A static box, for putting walls in a rig's way.
Entity addWall(HorizonWorld& world, glm::vec3 centre, glm::vec3 half, bool isTrigger = false)
{
    Entity e = world.createEntity("Wall");
    TransformComponent t; t.position = centre;
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(e, rb);
    ColliderComponent col;
    col.shape       = ColliderShape::Box;
    col.halfExtents = half;
    col.isTrigger   = isTrigger;
    world.addComponent(e, col);
    return e;
}

} // namespace

TEST_CASE("CameraRig: the boom stops at a wall instead of going through it")
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 8.0f;
    r->rig().collisionRadius = 0.25f;

    // The boom trails on +Z; put a wall across it at z = 3.5.
    addWall(r->world, { 0.0f, 0.0f, 4.0f }, { 4.0f, 4.0f, 0.5f });

    PhysicsWorld phys;
    phys.initialize(r->world);

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, entt::null, &phys);
    REQUIRE(f.driven);
    CHECK(f.occluded);
    CHECK(r->camXform().position.z == doctest::Approx(3.5f - 0.25f).epsilon(0.05f));
}

TEST_CASE("CameraRig: no physics world means the boom keeps its full length")
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 8.0f;
    addWall(r->world, { 0.0f, 0.0f, 4.0f }, { 4.0f, 4.0f, 0.5f });

    const auto f = HE::CameraRigController::update(r->world, kNoMouse);   // physics = nullptr
    REQUIRE(f.driven);
    CHECK_FALSE(f.occluded);
    CHECK(r->camXform().position.z == doctest::Approx(8.0f));
}

TEST_CASE("CameraRig: collision off keeps the boom's full length")
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 8.0f;
    r->rig().collision   = false;
    addWall(r->world, { 0.0f, 0.0f, 4.0f }, { 4.0f, 4.0f, 0.5f });

    PhysicsWorld phys;
    phys.initialize(r->world);

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, entt::null, &phys);
    CHECK_FALSE(f.occluded);
    CHECK(r->camXform().position.z == doctest::Approx(8.0f));
}

TEST_CASE("CameraRig: the boom ignores the target's own collider")
{
    // Otherwise the very first sweep hits the character it is following and the
    // camera collapses into its head — and with the frozen kinematic proxy, it
    // would do that again whenever the player walks past their spawn point.
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 6.0f;

    // Give the target a body sitting right where the sweep starts.
    RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
    r->world.addComponent(r->target, rb);
    ColliderComponent col;
    col.shape = ColliderShape::Capsule; col.radius = 0.35f; col.height = 1.8f;
    r->world.addComponent(r->target, col);

    PhysicsWorld phys;
    phys.initialize(r->world);

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, entt::null, &phys);
    REQUIRE(f.driven);
    CHECK_FALSE(f.occluded);
    CHECK(r->camXform().position.z == doctest::Approx(6.0f));
}

TEST_CASE("CameraRig: a trigger volume does not pull the camera in")
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 8.0f;
    addWall(r->world, { 0.0f, 0.0f, 4.0f }, { 4.0f, 4.0f, 0.5f }, /*isTrigger=*/true);

    PhysicsWorld phys;
    phys.initialize(r->world);

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, entt::null, &phys);
    CHECK_FALSE(f.occluded);
    CHECK(r->camXform().position.z == doctest::Approx(8.0f));
}

TEST_CASE("CameraRig: first person never sweeps")
{
    // The camera is at the pivot, so there is nothing to sweep — and a wall the
    // player is standing against must not shove the view somewhere else.
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    r->rig().pivotOffset = { 0.0f, 1.7f, 0.0f };
    addWall(r->world, { 0.0f, 0.0f, 0.0f }, { 4.0f, 4.0f, 4.0f });   // pivot is inside it

    PhysicsWorld phys;
    phys.initialize(r->world);

    const auto f = HE::CameraRigController::update(r->world, kNoMouse, entt::null, &phys);
    REQUIRE(f.driven);
    CHECK_FALSE(f.occluded);
    CHECK(r->camXform().position.y == doctest::Approx(1.7f));
}

// ─── First-person mesh hiding ─────────────────────────────────────────────────

TEST_CASE("CameraRig: first person hides the target mesh and gives it back")
{
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    MeshComponent mesh;                      // visible + castsShadow by default
    r->world.addComponent(r->target, mesh);

    HE::CameraRigController::update(r->world, kNoMouse);
    const auto& m = r->world.registry().get<MeshComponent>(r->target);
    CHECK_FALSE(m.visible);
    CHECK(m.castsShadow);                    // still casts a shadow, just not drawn

    // Switching to third person must give the mesh back — otherwise the player
    // is left invisible with nothing to point at.
    r->rig().mode = CameraRigComponent::Mode::ThirdPerson;
    HE::CameraRigController::update(r->world, kNoMouse);
    CHECK(m.visible);
}

TEST_CASE("CameraRig: retargeting in first person gives the old mesh back")
{
    // The rig remembers WHICH entity it hid, not merely that it hid something.
    // With a flag, retargeting would look like "already hiding" — the old target
    // would stay invisible for good and the new one would be drawn straight
    // through the camera.
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    r->world.addComponent(r->target, MeshComponent{});

    Entity other = r->world.createEntity("Other");
    r->world.addComponent(other, TransformComponent{});
    r->world.addComponent(other, MeshComponent{});

    HE::CameraRigController::update(r->world, kNoMouse);
    CHECK_FALSE(r->world.registry().get<MeshComponent>(r->target).visible);

    r->rig().target = r->world.entityId(other);
    HE::CameraRigController::update(r->world, kNoMouse);

    CHECK(r->world.registry().get<MeshComponent>(r->target).visible);
    CHECK_FALSE(r->world.registry().get<MeshComponent>(other).visible);
}

TEST_CASE("CameraRig: a target that goes away gets its mesh back")
{
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    r->world.addComponent(r->target, MeshComponent{});

    HE::CameraRigController::update(r->world, kNoMouse);
    CHECK_FALSE(r->world.registry().get<MeshComponent>(r->target).visible);

    // Point the rig at an id nothing owns — the target is gone as far as it knows.
    r->rig().target = HE::UUID{ 0xDEADull, 0xBEEFull };
    const auto f = HE::CameraRigController::update(r->world, kNoMouse);

    CHECK_FALSE(f.driven);
    CHECK(r->world.registry().get<MeshComponent>(r->target).visible);
}

TEST_CASE("CameraRig: hideTargetMesh off leaves the mesh alone")
{
    auto r = makeRig(CameraRigComponent::Mode::FirstPerson);
    r->world.addComponent(r->target, MeshComponent{});
    r->rig().hideTargetMesh = false;

    HE::CameraRigController::update(r->world, kNoMouse);
    CHECK(r->world.registry().get<MeshComponent>(r->target).visible);
}

// ─── Lag ──────────────────────────────────────────────────────────────────────

namespace {

// A rig whose camera sits exactly ON the pivot — no boom, no shoulder offset —
// so the camera's position IS the lagged pivot and the assertions below are
// about the smoother rather than about trigonometry.
std::unique_ptr<Rig> makeLagRig()
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 0.0f;
    r->rig().lag.enabled = true;
    return r;
}

// n frames of nothing but time passing.
void stepTime(Rig& r, float dt, int n)
{
    HE::CameraLookInput look;
    look.dt = dt;
    for (int i = 0; i < n; ++i)
        HE::CameraRigController::update(r.world, look);
}

} // namespace

TEST_CASE("CameraRig: lag is framerate-independent")
{
    // THE property. `lerp(a, b, speed * dt)` is tuned at one refresh rate and is
    // a different camera at another; `1 - exp(-speed * dt)` leaves exactly
    // exp(-speed * T) of the gap after T seconds however that second was cut up.
    //
    // Sampled MID-transition, at 0.1 s, on purpose: run it out to a full second
    // and both formulations have converged to within a hair of the target, and
    // the test passes for the broken one too.
    auto run = [](int steps, float dt) {
        auto r = makeLagRig();
        r->rig().lag.positionSpeed = 10.0f;
        r->rig().lag.maxDistance   = 100.0f;   // out of the way of the clamp
        r->rig().lag.snapDistance  = 100.0f;   // out of the way of the snap

        stepTime(*r, dt, 1);                   // first frame sets, at the origin
        r->tgtXform().position = { 1.0f, 0.0f, 0.0f };
        stepTime(*r, dt, steps);
        return r->camXform().position.x;
    };

    const float at60  = run(6,  1.0f / 60.0f);    // 0.1 s
    const float at120 = run(12, 1.0f / 120.0f);   // the same 0.1 s

    // 1 - exp(-10 * 0.1) = 1 - e^-1
    CHECK(at60  == doctest::Approx(0.63212f).epsilon(0.002));
    CHECK(at120 == doctest::Approx(0.63212f).epsilon(0.002));
    CHECK(at60  == doctest::Approx(at120).epsilon(0.002));

    // And it really was mid-flight, not a hard set dressed up as smoothing.
    CHECK(at60 > 0.1f);
    CHECK(at60 < 0.9f);
}

TEST_CASE("CameraRig: the first frame of a rig sets, it does not ease in")
{
    // Otherwise every level start opens with the camera flying in from the world
    // origin.
    auto r = makeLagRig();
    r->rig().armLength = 4.0f;
    r->tgtXform().position = { 0.0f, 0.0f, 10.0f };

    stepTime(*r, 1.0f / 60.0f, 1);

    CHECK(r->camXform().position.z == doctest::Approx(14.0f));
}

TEST_CASE("CameraRig: a jump past snapDistance sets, one just under it smooths")
{
    auto r = makeLagRig();
    r->rig().lag.positionSpeed = 10.0f;
    r->rig().lag.snapDistance  = 5.0f;
    r->rig().lag.maxDistance   = 100.0f;
    stepTime(*r, 1.0f / 60.0f, 1);

    // A respawn six metres away — the camera must be there, not travelling.
    r->tgtXform().position = { 0.0f, 0.0f, 6.0f };
    stepTime(*r, 1.0f / 60.0f, 1);
    CHECK(r->camXform().position.z == doctest::Approx(6.0f));

    // Four metres is a fast run, not a teleport, and is smoothed.
    auto s = makeLagRig();
    s->rig().lag.positionSpeed = 10.0f;
    s->rig().lag.snapDistance  = 5.0f;
    s->rig().lag.maxDistance   = 100.0f;
    stepTime(*s, 1.0f / 60.0f, 1);

    s->tgtXform().position = { 0.0f, 0.0f, 4.0f };
    stepTime(*s, 1.0f / 60.0f, 1);
    CHECK(s->camXform().position.z > 0.1f);
    CHECK(s->camXform().position.z < 3.9f);
}

TEST_CASE("CameraRig: maxDistance caps how far the camera can be left behind")
{
    auto r = makeLagRig();
    r->rig().lag.positionSpeed = 2.0f;      // deliberately sluggish
    r->rig().lag.maxDistance   = 2.0f;
    r->rig().lag.snapDistance  = 1000.0f;   // the cap must hold on its own
    stepTime(*r, 1.0f / 60.0f, 1);

    // A target moving far faster than the smoother could ever follow.
    for (int i = 1; i <= 30; ++i)
    {
        r->tgtXform().position = { 0.0f, 0.0f, static_cast<float>(i) * 5.0f };
        stepTime(*r, 1.0f / 60.0f, 1);
        const float trail = r->tgtXform().position.z - r->camXform().position.z;
        CHECK(trail <= doctest::Approx(2.0f).epsilon(0.001));
    }
}

TEST_CASE("CameraRig: yaw lag crosses ±180 the short way")
{
    auto r = makeLagRig();
    r->rig().lag.rotationSpeed = 10.0f;
    r->rig().yaw = 179.0f;
    stepTime(*r, 1.0f / 60.0f, 1);           // sets armYaw to 179
    REQUIRE(r->rig().armYaw == doctest::Approx(179.0f));

    // Two degrees across the seam. Folding the difference into (-180, 180] makes
    // that a two-degree move; not folding it sends the boom 358° the other way.
    r->rig().yaw = -179.0f;
    stepTime(*r, 1.0f / 60.0f, 1);

    CHECK(r->rig().armYaw > 179.0f);
    CHECK(r->rig().armYaw < 181.0f);

    // And the LOOK direction is not smoothed at all — that is the whole point of
    // keeping armYaw separate from yaw. With lag off the two are equal, so this
    // is the one assertion that can tell them apart.
    CHECK(r->camXform().rotation.y == doctest::Approx(-179.0f));
    CHECK(r->rig().yaw             == doctest::Approx(-179.0f));
}

TEST_CASE("CameraRig: snap() sets on the next frame instead of easing")
{
    // The third hard-set case: a script that teleports its character knows
    // before the rig could possibly work it out.
    auto r = makeLagRig();
    r->rig().lag.positionSpeed = 10.0f;
    r->rig().lag.snapDistance  = 1000.0f;   // would smooth all the way
    r->rig().lag.maxDistance   = 1000.0f;
    stepTime(*r, 1.0f / 60.0f, 1);

    r->tgtXform().position = { 0.0f, 0.0f, 50.0f };
    r->rig().snap();
    stepTime(*r, 1.0f / 60.0f, 1);

    CHECK(r->camXform().position.z == doctest::Approx(50.0f));
}

TEST_CASE("CameraRig: lag is off by default and off means today's pose exactly")
{
    // The guarantee under which the solved-pose rebuild was allowed to happen:
    // at default settings the rig is bit-for-bit what it was before lag existed.
    auto r = makeRig();
    CHECK_FALSE(r->rig().lag.enabled);

    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 4.0f;
    r->rig().sensitivity = 1.0f;
    stepTime(*r, 1.0f / 60.0f, 1);

    // A teleport is followed in the very same frame, with no easing anywhere.
    r->tgtXform().position = { 7.0f, 0.0f, -3.0f };
    stepTime(*r, 1.0f / 60.0f, 1);
    CHECK(r->camXform().position.x == doctest::Approx(7.0f));
    CHECK(r->camXform().position.z == doctest::Approx(1.0f));

    // And the boom direction is the look direction, not a smoothed copy of it.
    MouseFrame m; m.dx = -45.0f;             // yaw +45°
    HE::CameraRigController::update(r->world, m);
    CHECK(r->rig().armYaw   == doctest::Approx(r->rig().yaw));
    CHECK(r->rig().armPitch == doctest::Approx(r->rig().pitch));
}

// ─── Camera shake ─────────────────────────────────────────────────────────────

namespace {

// A rig sitting on its own pivot, so the camera's position IS the shake offset
// and every assertion below is about the shake rather than about a boom.
std::unique_ptr<Rig> makeShakeRig()
{
    auto r = makeRig();
    r->rig().pivotOffset = {};
    r->rig().armOffset   = {};
    r->rig().armLength   = 0.0f;
    return r;
}

// A shake with everything spelled out, so a test that cares about one number
// does not silently inherit a default for the other five.
HE::ShakeInstance shakeOf(float pos, float rot, float frequency, float duration)
{
    HE::ShakeInstance s;
    s.posAmplitude = glm::vec3(pos);
    s.rotAmplitude = glm::vec3(rot);
    s.frequency    = frequency;
    s.duration     = duration;
    s.blendIn      = 0.0f;   // no fade, so an amplitude means that amplitude
    s.blendOut     = 0.0f;
    return s;
}

} // namespace

TEST_CASE("CameraShake: the same shake at the same elapsed gives the same offset")
{
    // The property that makes a shake testable at all, and the reason it is
    // value noise off an integer hash rather than a random number generator:
    // no state travels between the two runs below except `elapsed`.
    std::array<HE::ShakeInstance, HE::kMaxCameraShakes> a{};
    std::array<HE::ShakeInstance, HE::kMaxCameraShakes> b{};
    HE::ShakeInstance s = shakeOf(0.5f, 3.0f, 11.0f, 10.0f);
    s.id   = 1;
    s.seed = 4242u;
    a[0] = s;
    b[0] = s;

    HE::ShakeOffset last;
    for (int i = 0; i < 20; ++i)
        last = HE::evaluateShakes(a, 1.0f / 60.0f);

    // The second run in ONE step of the same total time would land on a
    // different elapsed only if the evaluation carried hidden state. It does
    // not, so twenty sixtieths and the same twenty sixtieths agree exactly.
    HE::ShakeOffset again;
    for (int i = 0; i < 20; ++i)
        again = HE::evaluateShakes(b, 1.0f / 60.0f);

    CHECK(again.position.x == last.position.x);
    CHECK(again.position.y == last.position.y);
    CHECK(again.position.z == last.position.z);
    CHECK(again.rotationDegrees.x == last.rotationDegrees.x);
    CHECK(again.rotationDegrees.z == last.rotationDegrees.z);

    // And it was actually shaking, not sitting at zero and agreeing about that.
    CHECK(glm::length(last.position) > 1e-4f);

    // Same elapsed, different seed: a different offset. Otherwise the seed is
    // decoration and every shake in a scene moves in lockstep.
    CHECK(HE::shakeNoise(1u, 3.25f) != HE::shakeNoise(2u, 3.25f));
}

TEST_CASE("CameraShake: a finished shake contributes exactly zero and frees its slot")
{
    // "Exactly", not "almost": the slot is freed BEFORE the sum, so the last
    // frame of a shake is 0.0f and not the final epsilon of its fade-out. An
    // epsilon left standing is a camera permanently a hair off centre.
    auto r = makeShakeRig();
    const uint32_t h = r->rig().playShake(shakeOf(0.5f, 5.0f, 12.0f, 0.25f));
    REQUIRE(h != 0);

    stepTime(*r, 1.0f / 60.0f, 5);            // 0.083 s — well inside
    CHECK(glm::length(r->camXform().position) > 1e-4f);

    // Generously past the end: float steps do not land on 0.25 exactly, and the
    // assertion is about what happens AFTER the end, not about the last frame.
    stepTime(*r, 1.0f / 60.0f, 30);

    CHECK(r->camXform().position.x == 0.0f);
    CHECK(r->camXform().position.y == 0.0f);
    CHECK(r->camXform().position.z == 0.0f);
    CHECK(r->camXform().rotation.z == 0.0f);
    for (const auto& s : r->rig().shakes)
        CHECK(s.id == 0);
}

TEST_CASE("CameraShake: an endless shake runs until it is stopped")
{
    // duration <= 0 is the "engine rumble while the vehicle is running" case,
    // and it is the whole reason playShake hands back a handle.
    auto r = makeShakeRig();
    const int h = HE::api::camera::playShake(r->api(), 0.5f, 5.0f, 12.0f, 0.0f);
    REQUIRE(h > 0);

    stepTime(*r, 1.0f / 60.0f, 600);          // ten seconds
    CHECK(glm::length(r->camXform().position) > 1e-4f);

    HE::api::camera::stopShake(r->api(), h);
    stepTime(*r, 1.0f / 60.0f, 1);
    CHECK(r->camXform().position.x == 0.0f);
    CHECK(r->camXform().position.z == 0.0f);

    // A handle that has already been spent is a no-op, not a crash — a script
    // that keeps one across a level load needs it to be.
    HE::api::camera::stopShake(r->api(), h);
    HE::api::camera::stopShake(r->api(), 9999);

    // And Stop All takes the ones nobody kept a handle for.
    HE::api::camera::playShake(r->api(), 0.5f, 5.0f, 12.0f, 0.0f);
    HE::api::camera::playShake(r->api(), 0.5f, 5.0f, 12.0f, 0.0f);
    stepTime(*r, 1.0f / 60.0f, 5);
    CHECK(glm::length(r->camXform().position) > 1e-4f);
    HE::api::camera::stopAllShakes(r->api());
    stepTime(*r, 1.0f / 60.0f, 1);
    CHECK(r->camXform().position.x == 0.0f);
    CHECK(r->camXform().position.z == 0.0f);
}

TEST_CASE("CameraShake: in a shortened frame the shake spends the clearance and no more")
{
    // The sweep stops the boom one collision radius short of the wall, and that
    // radius is the only room the shake has to spend before it pushes the near
    // plane through the surface.
    const float kRadius = 0.25f;
    const float kWallZ  = 4.0f, kWallHalf = 0.5f;
    const glm::vec3 stopped{ 0.0f, 0.0f, kWallZ - kWallHalf - kRadius };

    auto walled = makeRig();
    walled->rig().pivotOffset     = {};
    walled->rig().armOffset       = {};
    walled->rig().armLength       = 8.0f;
    walled->rig().collisionRadius = kRadius;
    addWall(walled->world, { 0.0f, 0.0f, kWallZ }, { 4.0f, 4.0f, kWallHalf });
    PhysicsWorld phys;
    phys.initialize(walled->world);

    // The twin: the same rig and the same shake with no wall in front of it, to
    // prove the amplitude really was big enough to break the clamp if nothing
    // held it. Without this the test passes on a shake that never moved.
    auto open = makeRig();
    open->rig().pivotOffset     = {};
    open->rig().armOffset       = {};
    open->rig().armLength       = 8.0f;
    open->rig().collisionRadius = kRadius;

    // Amplitude far larger than the clearance, so the clamp has real work.
    const HE::ShakeInstance s = shakeOf(2.0f, 0.0f, 15.0f, 0.0f);
    REQUIRE(walled->rig().playShake(s) == open->rig().playShake(s));  // same handle → same seed

    bool openBrokeOut = false;
    for (int i = 0; i < 120; ++i)
    {
        HE::CameraLookInput look; look.dt = 1.0f / 60.0f;
        HE::CameraRigController::update(walled->world, look, entt::null, &phys);
        HE::CameraRigController::update(open->world,   look);

        // A hair of tolerance for the sweep's own precision, not for the clamp.
        CHECK(glm::distance(walled->camXform().position, stopped) <= kRadius + 0.02f);

        if (glm::distance(open->camXform().position, glm::vec3(0.0f, 0.0f, 8.0f)) > kRadius)
            openBrokeOut = true;
    }
    CHECK(openBrokeOut);
}

TEST_CASE("CameraShake: a shake moves the camera without touching the rig's input state")
{
    // yaw and pitch are the LOOK direction and the value the Follow coupling
    // writes to the character. A shake that leaked into them would turn the
    // player's aim, and — being additive — would wind them up frame after frame.
    auto r = makeShakeRig();
    r->rig().yaw   = 30.0f;
    r->rig().pitch = -12.0f;
    r->rig().playShake(shakeOf(0.4f, 8.0f, 14.0f, 0.0f));

    stepTime(*r, 1.0f / 60.0f, 300);

    CHECK(r->rig().yaw   == doctest::Approx(30.0f));
    CHECK(r->rig().pitch == doctest::Approx(-12.0f));

    // But the camera itself did move and did roll — roll being the axis the rig
    // never otherwise uses, so a non-zero one can only have come from the shake.
    CHECK(glm::length(r->camXform().position) > 1e-4f);
    CHECK(r->camXform().rotation.z != doctest::Approx(0.0f));
}
