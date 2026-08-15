#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/CameraRigController.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
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
