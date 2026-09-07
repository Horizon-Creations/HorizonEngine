#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/AnimationPreview.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <cmath>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
//  Fixtures
//
//  Everything here is physics-free except the last two cases, which need a real
//  CharacterVirtual to show that the character stops when the clip does.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

// Two joints: a root and a child, both with an identity inverse bind matrix. The
// child exists so a test can tell "the root was locked" from "the whole pose was
// wiped", which a one-joint skeleton cannot.
SkeletalMeshAsset makeSkeleton(const HE::UUID& meshId, const char* rootName = "Root")
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "rootMotionSkel";
    const std::array<float, 16> identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    SkeletonJoint root;  root.name = rootName; root.parent = -1; root.inverseBindMatrix = identity;
    SkeletonJoint child; child.name = "Child"; child.parent =  0; child.inverseBindMatrix = identity;
    sma.skeleton.push_back(root);
    sma.skeleton.push_back(child);
    return sma;
}

// The root walks `distance` along +Z over the whole clip and nothing else moves.
AnimationClipAsset makeWalkClip(float duration = 1.0f, float distance = 2.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "walkZ";

    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, duration };
    ch.values     = { 0.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, distance };
    clip.channels.push_back(std::move(ch));
    return clip;
}

// A quarter circle: the root turns 90° about Y over the clip while stepping along
// the curve. `restTilt` is pre-multiplied onto every rotation key, which is how a
// Blender export looks (a constant -90° about X on the root). Both variants have
// to land in the same place — that is what tells a relative transform apart from
// a subtraction, and a frame-0 reference apart from an absolute one.
AnimationClipAsset makeQuarterTurnClip(float duration, int keys, float radius,
                                       const glm::quat& restTilt = glm::quat(1, 0, 0, 0))
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "quarterTurn";

    AnimationChannel t;  t.jointIndex = 0; t.path = AnimPathType::Translation;
    AnimationChannel r;  r.jointIndex = 0; r.path = AnimPathType::Rotation;

    for (int i = 0; i < keys; ++i)
    {
        const float u    = static_cast<float>(i) / static_cast<float>(keys - 1);
        const float time = u * duration;
        const float ang  = glm::radians(90.0f * u);

        // Centre of the arc at (radius, 0, 0); start at the origin facing +Z and
        // curve towards +X, which is what a 90° yaw turn traces out.
        const glm::vec3 p(radius - radius * std::cos(ang), 0.0f, radius * std::sin(ang));
        const glm::quat q = glm::angleAxis(ang, glm::vec3(0, 1, 0)) * restTilt;

        t.times.push_back(time);
        t.values.insert(t.values.end(), { p.x, p.y, p.z });
        r.times.push_back(time);
        r.values.insert(r.values.end(), { q.x, q.y, q.z, q.w });   // glTF order
    }
    clip.channels.push_back(std::move(t));
    clip.channels.push_back(std::move(r));
    return clip;
}

struct Rig
{
    ContentManager cm;
    HorizonWorld   world;
    HE::UUID       meshId;
    HE::UUID       clipId;
    entt::entity   entity = entt::null;
};

// One entity with a skeletal mesh, an AnimatorComponent on `clip` and a
// RootMotionComponent in `mode`.
std::unique_ptr<Rig> makeRig(AnimationClipAsset clip,
                             RootMotionComponent::Mode mode = RootMotionComponent::Mode::Transform,
                             bool looping = true)
{
    auto rig = std::make_unique<Rig>();
    rig->meshId = HE::UUID::generate();
    rig->cm.registerSkeletalMesh(makeSkeleton(rig->meshId));
    rig->clipId = rig->cm.registerAnimationClip(std::move(clip));

    rig->entity = rig->world.createEntity("Character");
    rig->world.addComponent(rig->entity, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = rig->meshId;
    rig->world.addComponent(rig->entity, smc);
    AnimatorComponent an; an.clipAssetId = rig->clipId; an.looping = looping;
    rig->world.addComponent(rig->entity, an);
    RootMotionComponent rm; rm.mode = mode;
    rig->world.addComponent(rig->entity, rm);
    return rig;
}

const AnimationClipAsset& clipOf(Rig& rig) { return *rig.cm.getAnimationClip(rig.clipId); }
const SkeletalMeshAsset&  meshOf(Rig& rig) { return *rig.cm.getSkeletalMesh(rig.meshId); }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  findRootJoint
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("findRootJoint: empty name takes the first joint without a parent")
{
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());
    CHECK(HE::findRootJoint(mesh, "") == 0);
}

TEST_CASE("findRootJoint: a named joint is found by its name")
{
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());
    CHECK(HE::findRootJoint(mesh, "Child") == 1);
    CHECK(HE::findRootJoint(mesh, "Root")  == 0);
}

TEST_CASE("findRootJoint: an unknown name is -1, not a silent fallback")
{
    // A typo has to be distinguishable from "the clip carries no motion", or the
    // only symptom is a character that does not move and no reason why.
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());
    CHECK(HE::findRootJoint(mesh, "Hips") == -1);
}

TEST_CASE("findRootJoint: a skeleton with no root at all is -1")
{
    SkeletalMeshAsset mesh;
    CHECK(HE::findRootJoint(mesh, "") == -1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  extractRootMotion: the span arithmetic
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("extractRootMotion: half the clip is half the distance")
{
    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    const HE::RootMotionOptions opt;

    const HE::RootMotionDelta half = HE::extractRootMotion(clip, 0, 0.0f, 0.5f, opt);
    CHECK(half.translation.z == doctest::Approx(1.0f));
    CHECK(half.translation.x == doctest::Approx(0.0f));

    const HE::RootMotionDelta full = HE::extractRootMotion(clip, 0, 0.0f, 1.0f, opt);
    CHECK(full.translation.z == doctest::Approx(2.0f));
}

TEST_CASE("extractRootMotion: an empty span is the identity")
{
    const AnimationClipAsset clip = makeWalkClip();
    const HE::RootMotionOptions opt;
    const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.3f, 0.3f, opt);
    CHECK(d.translation.z  == doctest::Approx(0.0f));
    CHECK(d.yawDegrees     == doctest::Approx(0.0f));
}

TEST_CASE("extractRootMotion: a span across the loop edge sums both parts")
{
    // 0.8 → 1.3 on a 1 s clip is 0.2 s of this lap plus 0.3 s of the next. The
    // wrong answer here is a jump BACK to the start of the clip, which is what a
    // pair of wrapped playheads produces.
    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    const HE::RootMotionOptions opt;

    const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.8f, 1.3f, opt);
    CHECK(d.translation.z == doctest::Approx(1.0f).epsilon(1e-4));   // 0.5 s of 2 m/s
}

TEST_CASE("extractRootMotion: one and a half laps in one frame is one and a half laps")
{
    // The case that cannot be recovered from two wrapped playheads at all: it
    // looks exactly like half a frame.
    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    const HE::RootMotionOptions opt;

    const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.0f, 1.5f, opt);
    CHECK(d.translation.z == doctest::Approx(3.0f).epsilon(1e-4));
}

TEST_CASE("extractRootMotion: rewinding is the forward delta, inverted")
{
    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    const HE::RootMotionOptions opt;

    const HE::RootMotionDelta fwd = HE::extractRootMotion(clip, 0, 0.2f, 0.7f, opt);
    const HE::RootMotionDelta bwd = HE::extractRootMotion(clip, 0, 0.7f, 0.2f, opt);
    CHECK(bwd.translation.z == doctest::Approx(-fwd.translation.z));
}

TEST_CASE("extractRootMotion: rewinding across the loop edge wraps backwards")
{
    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    const HE::RootMotionOptions opt;

    const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.2f, -0.3f, opt);
    CHECK(d.translation.z == doctest::Approx(-1.0f).epsilon(1e-4));
}

TEST_CASE("extractRootMotion: the extract flags gate each axis")
{
    AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    // Give the root a vertical component too.
    clip.channels[0].values = { 0.0f, 0.0f, 0.0f,
                                0.0f, 1.0f, 2.0f };

    HE::RootMotionOptions opt;                       // Y off by default
    HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.0f, 1.0f, opt);
    CHECK(d.translation.y == doctest::Approx(0.0f));
    CHECK(d.translation.z == doctest::Approx(2.0f));

    opt.extractTranslationY = true;
    d = HE::extractRootMotion(clip, 0, 0.0f, 1.0f, opt);
    CHECK(d.translation.y == doctest::Approx(1.0f));

    opt.extractTranslationXZ = false;
    d = HE::extractRootMotion(clip, 0, 0.0f, 1.0f, opt);
    CHECK(d.translation.z == doctest::Approx(0.0f));
    CHECK(d.translation.y == doctest::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  extractRootMotion: the curve
//
//  This is the pair that tells a relative transform apart from a subtraction, and
//  a frame-0 reference apart from the root's raw local frame. Without them both
//  errors only show up in the game, as a character drifting sideways out of every
//  turn or climbing out of the floor.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
// Walk the clip in `steps` even frames, composing the per-frame deltas the way the
// entity does: turn by the accumulated yaw, then translate.
void integrate(const AnimationClipAsset& clip, int steps, const HE::RootMotionOptions& opt,
               glm::vec3& outPos, float& outYaw)
{
    outPos = glm::vec3(0.0f);
    outYaw = 0.0f;
    for (int i = 0; i < steps; ++i)
    {
        const float a = clip.duration * static_cast<float>(i)     / static_cast<float>(steps);
        const float b = clip.duration * static_cast<float>(i + 1) / static_cast<float>(steps);
        const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, a, b, opt);
        outPos += glm::angleAxis(glm::radians(outYaw), glm::vec3(0, 1, 0)) * d.translation;
        outYaw += d.yawDegrees;
    }
}
} // namespace

TEST_CASE("extractRootMotion: a 90 degree turn lands where the artist put it")
{
    const AnimationClipAsset clip = makeQuarterTurnClip(1.0f, 33, 2.0f);
    const HE::RootMotionOptions opt;

    glm::vec3 pos; float yaw = 0.0f;
    integrate(clip, 32, opt, pos, yaw);

    // The clip's own end point: (r, 0, r) with a 90° heading.
    CHECK(pos.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(pos.z == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(pos.y == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(yaw   == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("extractRootMotion: a Blender-tilted root turns the same and does not climb")
{
    // The same curve with a constant -90° X on every rotation key, which is what
    // comes out of Blender. Un-rotating the displacement by the root's RAW frame
    // would turn each horizontal step into a vertical one — the character climbs.
    // Referencing frame 0 cancels the tilt.
    const glm::quat tilt = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));
    const AnimationClipAsset clip = makeQuarterTurnClip(1.0f, 33, 2.0f, tilt);
    const HE::RootMotionOptions opt;

    glm::vec3 pos; float yaw = 0.0f;
    integrate(clip, 32, opt, pos, yaw);

    CHECK(pos.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(pos.z == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(pos.y == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(yaw   == doctest::Approx(90.0f).epsilon(0.01));
}

TEST_CASE("extractRootMotion: extractYaw off leaves the turn in the pose")
{
    const AnimationClipAsset clip = makeQuarterTurnClip(1.0f, 33, 2.0f);
    HE::RootMotionOptions opt; opt.extractYaw = false;

    const HE::RootMotionDelta d = HE::extractRootMotion(clip, 0, 0.0f, 0.5f, opt);
    CHECK(d.yawDegrees == doctest::Approx(0.0f));
    CHECK(d.translation.z != doctest::Approx(0.0f));   // it still travels
}

// ─────────────────────────────────────────────────────────────────────────────
//  blendRootMotion
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blendRootMotion: half and half is the mean of both deltas")
{
    HE::RootMotionDelta a; a.translation = { 0.0f, 0.0f, 2.0f }; a.yawDegrees = 10.0f;
    HE::RootMotionDelta b; b.translation = { 0.0f, 0.0f, 6.0f }; b.yawDegrees = 30.0f;

    const HE::RootMotionDelta m = HE::blendRootMotion(a, b, 0.5f);
    CHECK(m.translation.z == doctest::Approx(4.0f));
    CHECK(m.yawDegrees    == doctest::Approx(20.0f));

    CHECK(HE::blendRootMotion(a, b, 0.0f).translation.z == doctest::Approx(2.0f));
    CHECK(HE::blendRootMotion(a, b, 1.0f).translation.z == doctest::Approx(6.0f));
}

TEST_CASE("blendRootMotion: yaw takes the short way round")
{
    HE::RootMotionDelta a; a.yawDegrees =  179.0f;
    HE::RootMotionDelta b; b.yawDegrees = -179.0f;
    const HE::RootMotionDelta m = HE::blendRootMotion(a, b, 0.5f);
    // Two degrees apart, not three hundred and fifty-eight.
    CHECK(std::abs(m.yawDegrees) == doctest::Approx(180.0f).epsilon(1e-4));
}

// ─────────────────────────────────────────────────────────────────────────────
//  The root lock
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("root lock: the posed root stands still while the child keeps its offset")
{
    // Not compared against the bind pose: a Blender root's bind-local rotation is
    // not the identity, so "the lock worked" cannot mean "the root is identity".
    // It is compared against the SAME clip posed at two different times: with the
    // lock on, the root's bone matrix must not move between them.
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    auto& reg = rig->world.registry();

    AnimationSystem::update(rig->world, rig->cm, 0.25f);
    const glm::mat4 earlyRoot  = reg.get<SkeletalMeshComponent>(rig->entity).boneMatrices[0];
    const glm::mat4 earlyChild = reg.get<SkeletalMeshComponent>(rig->entity).boneMatrices[1];

    AnimationSystem::update(rig->world, rig->cm, 0.5f);
    const glm::mat4 lateRoot = reg.get<SkeletalMeshComponent>(rig->entity).boneMatrices[0];

    CHECK(glm::length(glm::vec3(lateRoot[3]) - glm::vec3(earlyRoot[3]))
          == doctest::Approx(0.0f).epsilon(1e-4));
    // The pose is locked, not wiped: the child is still where the skeleton puts it.
    CHECK(glm::length(glm::vec3(earlyChild[3]) - glm::vec3(earlyRoot[3]))
          == doctest::Approx(0.0f).epsilon(1e-4));   // child sits on the root in this rig
}

TEST_CASE("root lock: without a RootMotionComponent the root still travels")
{
    // The counter-test to the one above — otherwise it would also pass if the
    // clip simply never moved anything.
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID clipId = cm.registerAnimationClip(makeWalkClip(1.0f, 2.0f));

    entt::entity e = world.createEntity("Plain");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.25f);
    const glm::mat4 early = world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0];
    AnimationSystem::update(world, cm, 0.5f);
    const glm::mat4 late  = world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0];

    CHECK(glm::length(glm::vec3(late[3]) - glm::vec3(early[3])) > 0.5f);
}

TEST_CASE("root lock: FirstFrame parks the root on its own start, not on the origin")
{
    AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    clip.channels[0].values = { 0.0f, 0.0f, 5.0f,     // starts at z = 5
                                0.0f, 0.0f, 7.0f };
    auto rig = makeRig(std::move(clip));
    auto& reg = rig->world.registry();
    reg.get<RootMotionComponent>(rig->entity).options.lock = HE::RootMotionLock::FirstFrame;

    AnimationSystem::update(rig->world, rig->cm, 0.4f);
    const glm::mat4 root = reg.get<SkeletalMeshComponent>(rig->entity).boneMatrices[0];
    CHECK(root[3].z == doctest::Approx(5.0f).epsilon(1e-4));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Applying it: Transform mode
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Transform mode: the entity walks the clip's distance")
{
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    HE::RootMotionContext ctx;   // no physics: the Transform mode needs none

    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.25f, nullptr, &ctx);

    const auto& t = rig->world.registry().get<TransformComponent>(rig->entity);
    CHECK(t.position.z == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(t.position.x == doctest::Approx(0.0f).epsilon(1e-3));
}

TEST_CASE("Transform mode: a rotated entity walks where it is facing")
{
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    rig->world.registry().get<TransformComponent>(rig->entity).rotation.y = 90.0f;
    HE::RootMotionContext ctx;

    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.25f, nullptr, &ctx);

    // Facing +X after a 90° yaw: the clip's +Z becomes the world's +X.
    const auto& t = rig->world.registry().get<TransformComponent>(rig->entity);
    CHECK(t.position.x == doctest::Approx(2.0f).epsilon(1e-2));
    CHECK(t.position.z == doctest::Approx(0.0f).epsilon(1e-2));
}

TEST_CASE("Transform mode: a parented entity lands at the right WORLD position")
{
    // The world/local border, nailed down. The delta is a world offset and
    // TransformComponent::position is local, so a parent that is moved and turned
    // has to come out of the arithmetic — and worldMatrix cannot be read for it,
    // because nothing propagates transforms during the animation phase.
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));

    entt::entity parent = rig->world.createEntity("Parent");
    TransformComponent pt;
    pt.position = { 10.0f, 0.0f, 0.0f };
    pt.rotation = { 0.0f, 90.0f, 0.0f };
    rig->world.addComponent(parent, pt);
    REQUIRE(rig->world.reparentEntity(rig->entity, parent));

    const glm::vec3 before = HE::worldPositionOf(rig->world, rig->entity);
    HE::RootMotionContext ctx;
    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.25f, nullptr, &ctx);
    const glm::vec3 after = HE::worldPositionOf(rig->world, rig->entity);

    // The child inherits the parent's 90° yaw, so its own forward is world +X.
    CHECK((after - before).x == doctest::Approx(2.0f).epsilon(1e-2));
    CHECK((after - before).z == doctest::Approx(0.0f).epsilon(1e-2));
}

TEST_CASE("Transform mode: a turning clip turns the entity too")
{
    auto rig = makeRig(makeQuarterTurnClip(1.0f, 33, 2.0f), RootMotionComponent::Mode::Transform,
                       /*looping=*/false);
    HE::RootMotionContext ctx;
    for (int i = 0; i < 32; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 1.0f / 32.0f, nullptr, &ctx);

    const auto& t = rig->world.registry().get<TransformComponent>(rig->entity);
    CHECK(t.rotation.y == doctest::Approx(90.0f).epsilon(0.02));
    CHECK(t.position.x == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(t.position.z == doctest::Approx(2.0f).epsilon(0.05));
}

// ─────────────────────────────────────────────────────────────────────────────
//  The edit-mode gate
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("no context: the entity stays put but the pose is identical")
{
    // The editor's case. A preview that poses differently from the game is not a
    // preview, so the lock has to happen either way — only the moving is gated.
    auto applied = makeRig(makeWalkClip(1.0f, 2.0f));
    auto gated   = makeRig(makeWalkClip(1.0f, 2.0f));

    HE::RootMotionContext ctx;
    for (int i = 0; i < 3; ++i)
    {
        SceneSystems::tickAnimation(applied->world, applied->cm, 0.2f, nullptr, &ctx);
        SceneSystems::tickAnimation(gated->world,   gated->cm,   0.2f, nullptr, nullptr);
    }

    CHECK(gated->world.registry().get<TransformComponent>(gated->entity).position.z
          == doctest::Approx(0.0f));
    CHECK(applied->world.registry().get<TransformComponent>(applied->entity).position.z > 1.0f);

    const auto& a = applied->world.registry().get<SkeletalMeshComponent>(applied->entity);
    const auto& g = gated->world.registry().get<SkeletalMeshComponent>(gated->entity);
    REQUIRE(a.boneMatrices.size() == g.boneMatrices.size());
    for (size_t i = 0; i < a.boneMatrices.size(); ++i)
        CHECK(glm::length(glm::vec3(a.boneMatrices[i][3]) - glm::vec3(g.boneMatrices[i][3]))
              == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("mode Off: nothing is applied and nothing is locked")
{
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f), RootMotionComponent::Mode::Off);
    HE::RootMotionContext ctx;
    for (int i = 0; i < 3; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.2f, nullptr, &ctx);

    CHECK(rig->world.registry().get<TransformComponent>(rig->entity).position.z
          == doctest::Approx(0.0f));
    // The root is still travelling in the pose — adding the component off is not
    // itself a change of behaviour.
    const auto& smc = rig->world.registry().get<SkeletalMeshComponent>(rig->entity);
    CHECK(smc.boneMatrices[0][3].z > 0.5f);
}

TEST_CASE("a clip that says it carries no root motion is left alone")
{
    AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    clip.hasRootMotion = false;
    auto rig = makeRig(std::move(clip));

    HE::RootMotionContext ctx;
    for (int i = 0; i < 3; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.2f, nullptr, &ctx);

    CHECK(rig->world.registry().get<TransformComponent>(rig->entity).position.z
          == doctest::Approx(0.0f));
    CHECK(rig->world.registry().get<SkeletalMeshComponent>(rig->entity).boneMatrices[0][3].z > 0.5f);
}

TEST_CASE("an unknown root joint name suspends root motion instead of guessing")
{
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    rig->world.registry().get<RootMotionComponent>(rig->entity).options.rootJointName = "Hips";

    HE::RootMotionContext ctx;
    for (int i = 0; i < 3; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.2f, nullptr, &ctx);

    CHECK(rig->world.registry().get<TransformComponent>(rig->entity).position.z
          == doctest::Approx(0.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  The other two drivers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blend animator: the two clips' deltas mix with the same alpha as the pose")
{
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID slowId = cm.registerAnimationClip(makeWalkClip(1.0f, 2.0f));
    const HE::UUID fastId = cm.registerAnimationClip(makeWalkClip(1.0f, 6.0f));

    entt::entity e = world.createEntity("Blender");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab;
    ab.clipAId = slowId; ab.clipBId = fastId; ab.blendAlpha = 0.5f;
    world.addComponent(e, ab);
    RootMotionComponent rm; rm.mode = RootMotionComponent::Mode::Transform;
    world.addComponent(e, rm);

    HE::RootMotionContext ctx;
    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(world, cm, 0.25f, nullptr, &ctx);

    // Halfway between a 2 m clip and a 6 m clip.
    CHECK(world.registry().get<TransformComponent>(e).position.z
          == doctest::Approx(4.0f).epsilon(1e-3));
}

TEST_CASE("state machine: a crossfade mixes both playheads' deltas")
{
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID slowId = cm.registerAnimationClip(makeWalkClip(1.0f, 2.0f));
    const HE::UUID fastId = cm.registerAnimationClip(makeWalkClip(1.0f, 6.0f));

    HE::AnimatorStateMachineGraph g;
    HE::AnimationState walk; walk.name = "Walk"; walk.clipId = slowId; walk.looping = true;
    HE::AnimationState run;  run.name  = "Run";  run.clipId  = fastId; run.looping  = true;
    g.states = { walk, run };
    g.startState = "Walk";
    HE::AnimationTransition tr;
    tr.fromState = "Walk"; tr.toState = "Run";
    tr.paramName = "speed"; tr.op = HE::TransitionOp::Greater; tr.threshold = 0.5f;
    tr.duration  = 1.0f;
    g.transitions = { tr };
    g.defaultParams["speed"] = 0.0f;

    AnimatorStateMachineAsset asset;
    asset.name      = "Locomotion";
    asset.graphJson = HE::animatorStateMachineToJson(g);
    const HE::UUID smId = cm.registerAnimatorStateMachine(std::move(asset));

    entt::entity e = world.createEntity("Fsm");
    world.addComponent(e, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorStateMachineComponent sm; sm.stateMachineAssetId = smId;
    world.addComponent(e, sm);
    RootMotionComponent rm; rm.mode = RootMotionComponent::Mode::Transform;
    world.addComponent(e, rm);

    HE::RootMotionContext ctx;

    // One second in Walk alone: 2 m.
    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(world, cm, 0.25f, nullptr, &ctx);
    const float afterWalk = world.registry().get<TransformComponent>(e).position.z;
    CHECK(afterWalk == doctest::Approx(2.0f).epsilon(1e-3));

    // Now cross-fade to Run over one second. The alpha ramps 0 → 1, so the
    // distance is the integral between the two clips: more than the 2 m of Walk
    // alone, less than the 6 m of Run alone.
    world.registry().get<AnimatorStateMachineComponent>(e).params["speed"] = 1.0f;
    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(world, cm, 0.25f, nullptr, &ctx);
    const float crossfaded = world.registry().get<TransformComponent>(e).position.z - afterWalk;
    CHECK(crossfaded > 2.0f);
    CHECK(crossfaded < 6.0f);
}

TEST_CASE("two pose drivers on one entity apply one delta, not two")
{
    // The bone matrices of the second driver win, which is merely odd. Two deltas
    // would ADD, and a character walking at double speed for no visible reason is
    // a much longer afternoon.
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    AnimatorBlendComponent ab;
    ab.clipAId = rig->clipId; ab.clipBId = rig->clipId; ab.blendAlpha = 0.5f;
    rig->world.addComponent(rig->entity, ab);

    HE::RootMotionContext ctx;
    for (int i = 0; i < 4; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.25f, nullptr, &ctx);

    CHECK(rig->world.registry().get<TransformComponent>(rig->entity).position.z
          == doctest::Approx(2.0f).epsilon(1e-3));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Character controller mode (real physics)
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
Entity buildFloor(HorizonWorld& world)
{
    Entity floor = world.createEntity("Floor");
    TransformComponent t; t.position = { 0.0f, -0.5f, 0.0f }; t.scale = { 40.0f, 1.0f, 40.0f };
    world.addComponent(floor, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(floor, rb);
    ColliderComponent col; col.shape = ColliderShape::Box; col.halfExtents = { 20.0f, 0.5f, 20.0f };
    world.addComponent(floor, col);
    return floor;
}
} // namespace

TEST_CASE("CharacterController mode: the figure walks and gravity keeps the vertical")
{
    ContentManager cm;
    HorizonWorld   world;
    buildFloor(world);

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID clipId = cm.registerAnimationClip(makeWalkClip(1.0f, 2.0f));

    entt::entity e = world.createEntity("Character");
    TransformComponent t; t.position = { 0.0f, 1.0f, 0.0f };
    world.addComponent(e, t);
    world.addComponent(e, CharacterControllerComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId;
    world.addComponent(e, an);
    RootMotionComponent rm; rm.mode = RootMotionComponent::Mode::CharacterController;
    world.addComponent(e, rm);

    PhysicsWorld phys;
    phys.initialize(world);
    HE::RootMotionContext ctx{ &phys };

    constexpr float dt = PhysicsWorld::kFixedDt;
    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, dt);
        SceneSystems::tickAnimation(world, cm, dt, nullptr, &ctx);
    }

    const auto& tc = world.registry().get<TransformComponent>(e);
    // One second of a 2 m/s clip. Generous bounds: a real character controller
    // is doing collision resolution, not integrating a straight line.
    CHECK(tc.position.z > 1.0f);
    CHECK(tc.position.z < 2.5f);
    // The vertical stayed the controller's: it settled onto the floor rather than
    // keeping the 1 m it was spawned at.
    CHECK(tc.position.y < 1.0f);
}

TEST_CASE("CharacterController mode: a clip that ends stops the figure instead of gliding")
{
    // Three things line up into "the character slides away for ever": Jolt HOLDS
    // the last velocity, MovementSystem only overwrites it for entities that have
    // a MovementComponent (this one has none), and the animator stops writing the
    // moment a non-looping clip clamps. This is the test for the frame that pays
    // the debt back.
    ContentManager cm;
    HorizonWorld   world;
    buildFloor(world);

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID clipId = cm.registerAnimationClip(makeWalkClip(0.5f, 2.0f));

    entt::entity e = world.createEntity("Character");
    TransformComponent t; t.position = { 0.0f, 1.0f, 0.0f };
    world.addComponent(e, t);
    world.addComponent(e, CharacterControllerComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = false;
    world.addComponent(e, an);
    RootMotionComponent rm; rm.mode = RootMotionComponent::Mode::CharacterController;
    world.addComponent(e, rm);

    PhysicsWorld phys;
    phys.initialize(world);
    HE::RootMotionContext ctx{ &phys };

    constexpr float dt = PhysicsWorld::kFixedDt;
    for (int i = 0; i < 40; ++i)   // the 0.5 s clip is long over by now
    {
        phys.step(world, dt);
        SceneSystems::tickAnimation(world, cm, dt, nullptr, &ctx);
    }
    const float settled = world.registry().get<TransformComponent>(e).position.z;
    CHECK(settled > 0.5f);   // it did walk while the clip ran

    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, dt);
        SceneSystems::tickAnimation(world, cm, dt, nullptr, &ctx);
    }
    CHECK(world.registry().get<TransformComponent>(e).position.z
          == doctest::Approx(settled).epsilon(1e-2));
    CHECK(world.registry().get<CharacterControllerComponent>(e).velocity.z
          == doctest::Approx(0.0f).epsilon(1e-2));
}

// ─────────────────────────────────────────────────────────────────────────────
//  The editor's preview: the same arithmetic, asked about a whole clip at once
//
//  These are what makes the line drawn under a selected figure trustworthy. A
//  preview that answers differently from the tick is worse than none: it is a
//  wrong answer with a picture attached.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("root motion path: a straight walk comes out straight, and the right length")
{
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(meshId);
    REQUIRE(mesh != nullptr);

    const AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    HE::RootMotionOptions opt;   // first root joint, XZ + yaw

    std::vector<glm::vec3> path;
    AnimationPreview::rootMotionPath(*mesh, clip, opt, 16, path);

    REQUIRE(path.size() == 17);                       // the origin plus one per span
    CHECK(path.front() == glm::vec3(0.0f));           // the path starts where the character is
    CHECK(path.back().z == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(path.back().x == doctest::Approx(0.0f).epsilon(1e-4));

    for (size_t i = 1; i < path.size(); ++i)
        CHECK(path[i].z >= path[i - 1].z);            // never doubles back
}

TEST_CASE("root motion path: a quarter turn curves, and the tilt does not change where it ends")
{
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(meshId);
    REQUIRE(mesh != nullptr);

    const glm::quat tilt = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));
    HE::RootMotionOptions opt;

    std::vector<glm::vec3> flat, tilted;
    std::vector<float>     yaw;
    AnimationPreview::rootMotionPath(*mesh, makeQuarterTurnClip(1.0f, 33, 2.0f), opt, 32, flat, &yaw);
    AnimationPreview::rootMotionPath(*mesh, makeQuarterTurnClip(1.0f, 33, 2.0f, tilt), opt, 32, tilted);

    REQUIRE(flat.size() == 33);
    REQUIRE(yaw.size()  == flat.size());
    // A quarter circle of radius 2 starting at the origin facing +Z ends at
    // (2, 0, 2) — and the heading has turned by 90°.
    CHECK(flat.back().x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(flat.back().z == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(yaw.back()    == doctest::Approx(90.0f).epsilon(0.02));
    CHECK(yaw.front()   == doctest::Approx(0.0f));

    // The Blender rest tilt cancels: it is a constant on the root, and the delta
    // is referenced against frame 0. A preview that climbed here would be showing
    // exactly the bug the reference cures.
    CHECK(tilted.back().x == doctest::Approx(flat.back().x).epsilon(0.02));
    CHECK(tilted.back().y == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(tilted.back().z == doctest::Approx(flat.back().z).epsilon(0.02));
}

TEST_CASE("root motion path: the three answers that mean 'no path'")
{
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(meshId);
    REQUIRE(mesh != nullptr);

    std::vector<glm::vec3> path;
    HE::RootMotionOptions opt;

    // The per-clip switch off — the same gate rootMotionSampleClip applies, so
    // the preview goes dark exactly when the game would stop moving.
    AnimationClipAsset off = makeWalkClip(1.0f, 2.0f);
    off.hasRootMotion = false;
    AnimationPreview::rootMotionPath(*mesh, off, opt, 16, path);
    CHECK(path.empty());

    // A joint name that matches nothing. Not "the first root anyway": a typo has
    // to look different from a clip that carries nothing.
    opt.rootJointName = "NoSuchBone";
    AnimationPreview::rootMotionPath(*mesh, makeWalkClip(1.0f, 2.0f), opt, 16, path);
    CHECK(path.empty());

    // A clip with no length.
    opt.rootJointName.clear();
    AnimationPreview::rootMotionPath(*mesh, makeWalkClip(0.0f, 2.0f), opt, 16, path);
    CHECK(path.empty());
}

TEST_CASE("root motion path: the preview ends where a ticked entity actually arrives")
{
    // The whole point of the line. Two routes to the same number: the editor's
    // one-shot integration over the clip, and sixty frames of the real tick.
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f), RootMotionComponent::Mode::Transform,
                       /*looping=*/false);

    const SkeletalMeshAsset*  mesh = rig->cm.getSkeletalMesh(rig->meshId);
    const AnimationClipAsset* clip = rig->cm.getAnimationClip(rig->clipId);
    REQUIRE(mesh != nullptr);
    REQUIRE(clip != nullptr);

    const HE::RootMotionOptions opt;
    std::vector<glm::vec3> path;
    AnimationPreview::rootMotionPath(*mesh, *clip, opt, 64, path);
    REQUIRE(path.size() > 1);

    // No PhysicsWorld: Transform mode writes the transform itself and only ever
    // reaches for the physics to hand a character controller back its velocity.
    HE::RootMotionContext ctx{};
    for (int i = 0; i < 80; ++i)   // the 1 s clip is over well before this
        SceneSystems::tickAnimation(rig->world, rig->cm, 1.0f / 60.0f, nullptr, &ctx);

    const glm::vec3 arrived = rig->world.registry().get<TransformComponent>(rig->entity).position;
    CHECK(arrived.z == doctest::Approx(path.back().z).epsilon(0.02));
    CHECK(arrived.x == doctest::Approx(path.back().x).epsilon(0.02));
}

TEST_CASE("preview pose: the lock parks the root, and the per-clip switch turns it off")
{
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(meshId);
    REQUIRE(mesh != nullptr);

    AnimationClipAsset clip = makeWalkClip(1.0f, 2.0f);
    HE::RootMotionOptions opt;   // Lock::Zero

    std::vector<glm::mat4> raw, locked;
    AnimationPreview::evaluateClipPose(*mesh, clip, 1.0f, raw);
    AnimationPreview::evaluateClipPoseLocked(*mesh, clip, 1.0f, opt, locked);

    REQUIRE(raw.size()    == mesh->skeleton.size());
    REQUIRE(locked.size() == mesh->skeleton.size());
    // Unlocked, the root carries the whole walk; locked, it stands at the origin
    // — which is what "animates in place" means, and what the path beside it is
    // then the missing half of.
    CHECK(raw[0][3].z    == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(locked[0][3].z == doctest::Approx(0.0f).epsilon(1e-4));

    // The clip's own switch turns the lock off too: the two halves have to agree,
    // or the mesh would animate in place with no path to explain where it went.
    clip.hasRootMotion = false;
    std::vector<glm::mat4> unswitched;
    AnimationPreview::evaluateClipPoseLocked(*mesh, clip, 1.0f, opt, unswitched);
    CHECK(unswitched[0][3].z == doctest::Approx(2.0f).epsilon(1e-3));
}
