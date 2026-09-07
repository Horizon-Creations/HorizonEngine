#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationIk.h>
#include <HorizonScene/AnimationPose.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  Inverse kinematics: the solvers, with no ECS and no physics anywhere.
//
//  Everything here talks to HE::AnimationIk directly, on a skeleton built in the
//  test. That is the whole reason the solvers are pure functions of (skeleton,
//  pose, target): a two-bone chain's convergence is arithmetic, and arithmetic
//  should not need a world, a content manager and a physics step to be asked a
//  question.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

constexpr std::array<float, 16> kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

void addJoint(SkeletalMeshAsset& m, const char* name, int parent)
{
    SkeletonJoint j; j.name = name; j.parent = parent; j.inverseBindMatrix = kIdentity;
    m.skeleton.push_back(j);
}

// Hip(0) ─ Knee(1) ─ Foot(2). The hip stands at y = 1, each bone is half a metre,
// so the straight leg puts the foot on y = 0 and the whole chain reaches 1 m.
SkeletalMeshAsset makeLeg()
{
    SkeletalMeshAsset m;
    m.name = "leg";
    addJoint(m, "Hip",  -1);
    addJoint(m, "Knee",  0);
    addJoint(m, "Foot",  1);
    return m;
}

// The bind pose of makeLeg, as local TRS.
std::vector<JointTRS> legPose()
{
    std::vector<JointTRS> p(3);
    p[0].translation = { 0.0f, 1.0f,  0.0f };
    p[1].translation = { 0.0f, -0.5f, 0.0f };
    p[2].translation = { 0.0f, -0.5f, 0.0f };
    return p;
}

// A leg bent by `degrees` at the knee: the hip turns one way, the knee twice the
// other, so the foot stays directly under the hip and only the KNEE swings out —
// forwards for a positive angle, backwards for a negative one. Exactly the pair
// of poses the pole-vector question needs.
std::vector<JointTRS> bentLeg(float degrees)
{
    std::vector<JointTRS> p = legPose();
    const float a = glm::radians(degrees);
    p[0].rotation = glm::angleAxis( a,          glm::vec3(1, 0, 0));
    p[1].rotation = glm::angleAxis(-2.0f * a,   glm::vec3(1, 0, 0));
    return p;
}

glm::vec3 posOf(const std::vector<glm::mat4>& model, int j) { return glm::vec3(model[j][3]); }

// Root(0) ─ Spine(1) ─ Neck(2) ─ Head(3), the head at y = 1.5.
SkeletalMeshAsset makeSpine()
{
    SkeletalMeshAsset m;
    m.name = "spine";
    addJoint(m, "Root",  -1);
    addJoint(m, "Spine",  0);
    addJoint(m, "Neck",   1);
    addJoint(m, "Head",   2);
    return m;
}

std::vector<JointTRS> spinePose()
{
    std::vector<JointTRS> p(4);
    p[1].translation = { 0.0f, 1.0f, 0.0f };
    p[2].translation = { 0.0f, 0.3f, 0.0f };
    p[3].translation = { 0.0f, 0.2f, 0.0f };
    return p;
}

float quatAngleDegrees(const glm::quat& q)
{
    const float w = glm::clamp(std::fabs(q.w), 0.0f, 1.0f);
    return glm::degrees(2.0f * std::acos(w));
}

bool sameTRS(const JointTRS& a, const JointTRS& b)
{
    return a.translation == b.translation && a.scale == b.scale
        && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y
        && a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}

} // namespace

TEST_CASE("Two-bone IK: a reachable target puts the foot exactly on it")
{
    const SkeletalMeshAsset mesh = makeLeg();
    std::vector<JointTRS>   pose = bentLeg(20.0f);
    std::vector<glm::mat4>  model;
    composeModelMatrices(mesh, pose, model);

    // Well inside the 1 m the chain reaches, and off to one side so the solve is
    // not a straight-line special case.
    const glm::vec3 target(0.15f, 0.30f, -0.25f);
    CHECK(HE::solveTwoBoneIk(mesh, 0, 1, 2, target, 1.0f, pose, model));

    const glm::vec3 foot = posOf(model, 2);
    CHECK(glm::length(foot - target) < 1.0e-4f);

    // And the model matrices the solver left behind are the ones a fresh FK on
    // the same pose would produce — otherwise the next stage reads stale frames.
    std::vector<glm::mat4> fresh;
    composeModelMatrices(mesh, pose, fresh);
    CHECK(glm::length(glm::vec3(fresh[2][3]) - foot) < 1.0e-5f);
}

TEST_CASE("Two-bone IK: an unreachable target straightens the leg instead of tearing it")
{
    const SkeletalMeshAsset mesh = makeLeg();
    std::vector<JointTRS>   pose = bentLeg(25.0f);
    std::vector<glm::mat4>  model;
    composeModelMatrices(mesh, pose, model);

    // Five metres away: the chain reaches one.
    const glm::vec3 target(0.0f, 1.0f, -5.0f);
    CHECK(HE::solveTwoBoneIk(mesh, 0, 1, 2, target, 1.0f, pose, model));

    const glm::vec3 hip  = posOf(model, 0);
    const glm::vec3 knee = posOf(model, 1);
    const glm::vec3 foot = posOf(model, 2);

    // Straight: the angle at the knee between shin and thigh is 180°.
    const float kneeAngle = glm::degrees(std::acos(glm::clamp(
        glm::dot(glm::normalize(hip - knee), glm::normalize(foot - knee)), -1.0f, 1.0f)));
    CHECK(kneeAngle == doctest::Approx(180.0f).epsilon(0.001f));

    // Not overextended — the two bones are still half a metre each.
    CHECK(glm::length(foot - hip) == doctest::Approx(1.0f).epsilon(0.001f));
    // And pointing AT the target rather than back at the animated pose.
    const glm::vec3 want = glm::normalize(target - hip);
    const glm::vec3 got  = glm::normalize(foot - hip);
    CHECK(glm::dot(want, got) == doctest::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("Two-bone IK: the bend plane comes out of the pose, not out of a fixed axis")
{
    // THE test that separates a pole vector read off the current knee from one
    // nailed to a world axis. Same skeleton, same target, two poses that differ
    // only in which way the knee already points — and the knee has to stay on
    // its own side in both. A fixed pole gets one of the two right and flips the
    // other one inside out, which on a real rig is a leg bending backwards.
    const SkeletalMeshAsset mesh = makeLeg();
    const glm::vec3 target(0.0f, 0.25f, -0.10f);

    std::vector<JointTRS>  fwdPose = bentLeg(30.0f);
    std::vector<glm::mat4> fwdModel;
    composeModelMatrices(mesh, fwdPose, fwdModel);
    const float kneeZBefore = posOf(fwdModel, 1).z;
    REQUIRE(kneeZBefore < -0.01f);                    // knee forward (-Z is forward)
    REQUIRE(HE::solveTwoBoneIk(mesh, 0, 1, 2, target, 1.0f, fwdPose, fwdModel));
    CHECK(posOf(fwdModel, 1).z < 0.0f);

    std::vector<JointTRS>  bwdPose = bentLeg(-30.0f);
    std::vector<glm::mat4> bwdModel;
    composeModelMatrices(mesh, bwdPose, bwdModel);
    REQUIRE(posOf(bwdModel, 1).z > 0.01f);            // knee backward
    REQUIRE(HE::solveTwoBoneIk(mesh, 0, 1, 2, target, 1.0f, bwdPose, bwdModel));
    CHECK(posOf(bwdModel, 1).z > 0.0f);

    // Both reached the same target from opposite bends.
    CHECK(glm::length(posOf(fwdModel, 2) - target) < 1.0e-4f);
    CHECK(glm::length(posOf(bwdModel, 2) - target) < 1.0e-4f);
}

TEST_CASE("Two-bone IK: a target the foot already sits on changes nothing, bit for bit")
{
    const SkeletalMeshAsset mesh = makeLeg();
    std::vector<JointTRS>   pose = bentLeg(18.0f);
    std::vector<glm::mat4>  model;
    composeModelMatrices(mesh, pose, model);

    const std::vector<JointTRS> before = pose;
    const glm::vec3 foot = posOf(model, 2);

    CHECK_FALSE(HE::solveTwoBoneIk(mesh, 0, 1, 2, foot, 1.0f, pose, model));
    for (size_t i = 0; i < pose.size(); ++i) CHECK(sameTRS(pose[i], before[i]));

    // Weight 0 is the other way of asking for nothing, with the same answer.
    CHECK_FALSE(HE::solveTwoBoneIk(mesh, 0, 1, 2, glm::vec3(0.3f, 0.4f, 0.0f), 0.0f, pose, model));
    for (size_t i = 0; i < pose.size(); ++i) CHECK(sameTRS(pose[i], before[i]));
}

TEST_CASE("Foot placement: dropping the pelvis keeps both legs inside their reach")
{
    // Two legs off one pelvis, the right foot standing 20 cm lower than the left
    // — a stair edge. Without the drop the lower leg has to reach 20 cm further
    // than it has bone, and it overextends; with it, the pelvis takes the
    // difference and both legs stay bent.
    SkeletalMeshAsset mesh;
    mesh.name = "biped";
    addJoint(mesh, "Pelvis",   -1);   // 0
    addJoint(mesh, "HipL",      0);   // 1
    addJoint(mesh, "KneeL",     1);   // 2
    addJoint(mesh, "FootL",     2);   // 3
    addJoint(mesh, "HipR",      0);   // 4
    addJoint(mesh, "KneeR",     4);   // 5
    addJoint(mesh, "FootR",     5);   // 6

    std::vector<JointTRS> pose(7);
    pose[0].translation = { 0.0f,  1.0f, 0.0f };
    pose[1].translation = { -0.1f, 0.0f, 0.0f };
    pose[2].translation = { 0.0f, -0.5f, 0.0f };
    pose[3].translation = { 0.0f, -0.5f, 0.0f };
    pose[4].translation = { 0.1f,  0.0f, 0.0f };
    pose[5].translation = { 0.0f, -0.5f, 0.0f };
    pose[6].translation = { 0.0f, -0.5f, 0.0f };
    // A little bend in both knees, so there is a plane to solve in.
    pose[1].rotation = glm::angleAxis(glm::radians( 12.0f), glm::vec3(1, 0, 0));
    pose[2].rotation = glm::angleAxis(glm::radians(-24.0f), glm::vec3(1, 0, 0));
    pose[4].rotation = glm::angleAxis(glm::radians( 12.0f), glm::vec3(1, 0, 0));
    pose[5].rotation = glm::angleAxis(glm::radians(-24.0f), glm::vec3(1, 0, 0));

    std::vector<glm::mat4> model;
    composeModelMatrices(mesh, pose, model);

    // The ground each foot found: the left where it stands, the right 20 cm down.
    const glm::vec3 targetL = posOf(model, 3);
    const glm::vec3 targetR = posOf(model, 6) - glm::vec3(0.0f, 0.2f, 0.0f);
    const float     drop    = -0.2f;   // the most negative of the two offsets

    HE::offsetJointModel(mesh, 0, glm::vec3(0.0f, drop, 0.0f), pose, model);
    CHECK(posOf(model, 0).y == doctest::Approx(0.8f).epsilon(0.001f));

    // The targets are ABSOLUTE ground points and do NOT move with the pelvis:
    // pulling them down as well would put the lower leg exactly back where it
    // could not reach, which is the whole thing the drop exists to prevent.
    //
    // Which is why only the LEFT leg has work to do here. The drop carried the
    // right foot the whole 20 cm down to its own ground, so its solve is the
    // no-op branch; the left foot went down with it and has to climb back.
    CHECK(HE::solveTwoBoneIk(mesh, 1, 2, 3, targetL, 1.0f, pose, model));
    CHECK_FALSE(HE::solveTwoBoneIk(mesh, 4, 5, 6, targetR, 1.0f, pose, model));

    CHECK(glm::length(posOf(model, 3) - targetL) < 1.0e-4f);
    CHECK(glm::length(posOf(model, 6) - targetR) < 1.0e-4f);

    // Both legs still bent, neither straightened out to its limit.
    CHECK(glm::length(posOf(model, 3) - posOf(model, 1)) < 0.999f);
    CHECK(glm::length(posOf(model, 6) - posOf(model, 4)) < 0.999f);
}

TEST_CASE("Look-at: straight ahead turns nothing, past the limit clamps, and the chain shares it")
{
    const SkeletalMeshAsset mesh = makeSpine();
    const std::vector<int>  chain = { 1, 2, 3 };
    const glm::vec3         fwd(0.0f, 0.0f, -1.0f);

    std::vector<JointTRS>  pose = spinePose();
    std::vector<glm::mat4> model;
    composeModelMatrices(mesh, pose, model);
    const glm::vec3 head = posOf(model, 3);

    SUBCASE("a target dead ahead is no rotation at all")
    {
        const glm::vec2 a = HE::lookAtAngles(mesh, chain, model, head + fwd * 5.0f,
                                             fwd, 70.0f, 45.0f);
        CHECK(a.x == doctest::Approx(0.0f).epsilon(0.001f));
        CHECK(a.y == doctest::Approx(0.0f).epsilon(0.001f));

        const std::vector<JointTRS> before = pose;
        CHECK_FALSE(HE::applyLookAt(mesh, chain, {}, a, fwd, 1.0f, pose, model));
        for (size_t i = 0; i < pose.size(); ++i) CHECK(sameTRS(pose[i], before[i]));
    }

    SUBCASE("90 degrees of yaw is clamped to the limit, not delivered")
    {
        const glm::vec2 a = HE::lookAtAngles(mesh, chain, model,
                                             head + glm::vec3(-5.0f, 0.0f, 0.0f),
                                             fwd, 70.0f, 45.0f);
        CHECK(a.x == doctest::Approx(70.0f).epsilon(0.001f));
        CHECK(a.y == doctest::Approx(0.0f).epsilon(0.001f));

        // The other side clamps the other way — a limit that only holds on one
        // side is a sign error waiting for a target to walk past the character.
        const glm::vec2 b = HE::lookAtAngles(mesh, chain, model,
                                             head + glm::vec3(5.0f, 0.0f, 0.0f),
                                             fwd, 70.0f, 45.0f);
        CHECK(b.x == doctest::Approx(-70.0f).epsilon(0.001f));
    }

    SUBCASE("pitch has its own limit")
    {
        const glm::vec2 a = HE::lookAtAngles(mesh, chain, model,
                                             head + glm::vec3(0.0f, 5.0f, -0.001f),
                                             fwd, 70.0f, 45.0f);
        CHECK(a.y == doctest::Approx(45.0f).epsilon(0.001f));
    }

    SUBCASE("the chain weights split the turn in the ratio they were given")
    {
        const glm::vec2 angles(30.0f, 0.0f);
        CHECK(HE::applyLookAt(mesh, chain, { 0.5f, 0.3f, 0.2f }, angles, fwd, 1.0f,
                              pose, model));
        CHECK(quatAngleDegrees(pose[1].rotation) == doctest::Approx(15.0f).epsilon(0.005f));
        CHECK(quatAngleDegrees(pose[2].rotation) == doctest::Approx( 9.0f).epsilon(0.005f));
        CHECK(quatAngleDegrees(pose[3].rotation) == doctest::Approx( 6.0f).epsilon(0.005f));

        // And the sum of the three IS the whole turn: the head ends up looking
        // 30° off, not 30° minus whatever three slerps lost on the way.
        const glm::vec3 look = glm::vec3(model[3] * glm::vec4(fwd, 0.0f));
        const float yaw = glm::degrees(std::atan2(-look.x, -look.z));
        CHECK(yaw == doctest::Approx(30.0f).epsilon(0.005f));
    }

    SUBCASE("weight 0 is not a slerp by zero, it is a return")
    {
        const std::vector<JointTRS> before = pose;
        CHECK_FALSE(HE::applyLookAt(mesh, chain, {}, glm::vec2(30.0f, 10.0f), fwd, 0.0f,
                                    pose, model));
        for (size_t i = 0; i < pose.size(); ++i) CHECK(sameTRS(pose[i], before[i]));
    }
}

TEST_CASE("Foot alignment: the sole follows the ground, up to the clamp")
{
    const SkeletalMeshAsset mesh = makeLeg();
    const glm::vec3 up(0, 1, 0), fwd(0, 0, -1);

    SUBCASE("flat ground is no rotation")
    {
        std::vector<JointTRS>  pose = legPose();
        std::vector<glm::mat4> model;
        composeModelMatrices(mesh, pose, model);
        const std::vector<JointTRS> before = pose;
        HE::alignFootToNormal(mesh, 2, up, up, fwd, 30.0f, 20.0f, 1.0f, pose, model);
        for (size_t i = 0; i < pose.size(); ++i) CHECK(sameTRS(pose[i], before[i]));
    }

    SUBCASE("a slope inside the limit is followed")
    {
        std::vector<JointTRS>  pose = legPose();
        std::vector<glm::mat4> model;
        composeModelMatrices(mesh, pose, model);
        // 15° of pitch: the normal leans back along +Z.
        const glm::vec3 n = glm::angleAxis(glm::radians(15.0f), glm::vec3(1, 0, 0)) * up;
        HE::alignFootToNormal(mesh, 2, n, up, fwd, 30.0f, 20.0f, 1.0f, pose, model);
        const glm::vec3 sole = glm::vec3(model[2] * glm::vec4(up, 0.0f));
        CHECK(glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(sole), n), -1.0f, 1.0f)))
              == doctest::Approx(0.0f).epsilon(0.01f));
    }

    SUBCASE("a wall does not stand the foot up on end")
    {
        std::vector<JointTRS>  pose = legPose();
        std::vector<glm::mat4> model;
        composeModelMatrices(mesh, pose, model);
        // 80° of pitch, against a 30° limit.
        const glm::vec3 n = glm::angleAxis(glm::radians(80.0f), glm::vec3(1, 0, 0)) * up;
        HE::alignFootToNormal(mesh, 2, n, up, fwd, 30.0f, 20.0f, 1.0f, pose, model);
        CHECK(quatAngleDegrees(pose[2].rotation) == doctest::Approx(30.0f).epsilon(0.01f));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  The stage: IkComponent inside the animation phase, driven by the real
//  SceneSystems::tickAnimation. Everything above answered "does the arithmetic
//  converge"; this half answers "does it run at the right moment, on the right
//  transform, and does it stay out of the way when it has no ground to stand on".
// ─────────────────────────────────────────────────────────────────────────────

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/IkComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/SceneSerializer.h>
#include <memory>

namespace
{

// Pelvis(0) ┬ HipL(1) ─ KneeL(2) ─ FootL(3)
//           └ HipR(4) ─ KneeR(5) ─ FootR(6)
SkeletalMeshAsset makeBiped(const HE::UUID& id)
{
    SkeletalMeshAsset m;
    m.id = id; m.name = "biped";
    addJoint(m, "Pelvis", -1);
    addJoint(m, "HipL",    0);
    addJoint(m, "KneeL",   1);
    addJoint(m, "FootL",   2);
    addJoint(m, "HipR",    0);
    addJoint(m, "KneeR",   4);
    addJoint(m, "FootR",   5);
    addJoint(m, "Spine",   0);   // 7
    addJoint(m, "Neck",    7);   // 8
    addJoint(m, "Head",    8);   // 9
    return m;
}

void constantTranslation(AnimationClipAsset& c, uint32_t joint, glm::vec3 v, float dur)
{
    AnimationChannel ch;
    ch.jointIndex = joint;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, dur };
    ch.values     = { v.x, v.y, v.z, v.x, v.y, v.z };
    c.channels.push_back(std::move(ch));
}

void constantRotation(AnimationClipAsset& c, uint32_t joint, float degX, float dur)
{
    const glm::quat q = glm::angleAxis(glm::radians(degX), glm::vec3(1, 0, 0));
    AnimationChannel ch;
    ch.jointIndex = joint;
    ch.path       = AnimPathType::Rotation;
    ch.times      = { 0.0f, dur };
    ch.values     = { q.x, q.y, q.z, q.w,  q.x, q.y, q.z, q.w };
    c.channels.push_back(std::move(ch));
}

// A standing pose, constant over the clip: both knees bent 12°, both feet ~2 cm
// above the character's own origin. Constant on purpose — a test about IK should
// not also depend on where a playhead happens to be.
AnimationClipAsset makeStandClip()
{
    AnimationClipAsset c;
    c.duration = 1.0f;
    c.name     = "stand";
    constantTranslation(c, 0, { 0.0f,  1.0f,  0.0f }, c.duration);
    constantTranslation(c, 1, { -0.1f, 0.0f,  0.0f }, c.duration);
    constantTranslation(c, 2, { 0.0f, -0.5f,  0.0f }, c.duration);
    constantTranslation(c, 3, { 0.0f, -0.5f,  0.0f }, c.duration);
    constantTranslation(c, 4, { 0.1f,  0.0f,  0.0f }, c.duration);
    constantTranslation(c, 5, { 0.0f, -0.5f,  0.0f }, c.duration);
    constantTranslation(c, 6, { 0.0f, -0.5f,  0.0f }, c.duration);
    constantTranslation(c, 7, { 0.0f,  0.2f,  0.0f }, c.duration);
    constantTranslation(c, 8, { 0.0f,  0.3f,  0.0f }, c.duration);
    constantTranslation(c, 9, { 0.0f,  0.2f,  0.0f }, c.duration);
    constantRotation(c, 1,  12.0f, c.duration);
    constantRotation(c, 2, -24.0f, c.duration);
    constantRotation(c, 4,  12.0f, c.duration);
    constantRotation(c, 5, -24.0f, c.duration);
    return c;
}

struct BipedRig
{
    ContentManager cm;
    HorizonWorld   world;
    HE::UUID       meshId;
    entt::entity   entity = entt::null;

    const std::vector<glm::mat4>& bones()
    {
        return world.registry().get<SkeletalMeshComponent>(entity).boneMatrices;
    }
    // Where a joint sits in WORLD space, read back out of the bone matrices. The
    // inverse bind matrices are all identity in these skeletons, so a bone matrix
    // IS the joint's model frame.
    glm::vec3 jointWorld(int j)
    {
        const auto& t = world.registry().get<TransformComponent>(entity);
        return glm::vec3(glm::translate(glm::mat4(1.0f), t.position) *
                         glm::vec4(glm::vec3(bones()[j][3]), 1.0f));
    }
};

std::unique_ptr<BipedRig> makeBipedRig(glm::vec3 at = glm::vec3(0.0f))
{
    auto rig = std::make_unique<BipedRig>();
    rig->meshId = HE::UUID::generate();
    rig->cm.registerSkeletalMesh(makeBiped(rig->meshId));
    const HE::UUID clipId = rig->cm.registerAnimationClip(makeStandClip());

    rig->entity = rig->world.createEntity("Character");
    TransformComponent t; t.position = at;
    rig->world.addComponent(rig->entity, t);
    SkeletalMeshComponent smc; smc.meshAssetId = rig->meshId;
    rig->world.addComponent(rig->entity, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = true;
    rig->world.addComponent(rig->entity, an);
    return rig;
}

// The two feet, wired to the joints of makeBiped.
IkComponent standingFeet()
{
    IkComponent ik;
    IkComponent::FootIk l; l.footJoint = "FootL";
    IkComponent::FootIk r; r.footJoint = "FootR";
    ik.feet = { l, r };
    return ik;
}

// A static box whose TOP face sits at `topY`, twenty metres across.
Entity buildFloor(HorizonWorld& world, float topY, glm::vec3 rotationDegrees = glm::vec3(0.0f),
                  glm::vec3 at = glm::vec3(0.0f))
{
    Entity floor = world.createEntity("Floor");
    TransformComponent t;
    t.position = { at.x, topY - 0.5f, at.z };
    t.rotation = rotationDegrees;
    world.addComponent(floor, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(floor, rb);
    ColliderComponent col; col.shape = ColliderShape::Box; col.halfExtents = { 10.0f, 0.5f, 10.0f };
    world.addComponent(floor, col);
    return floor;
}

} // namespace

TEST_CASE("IK without physics poses exactly like no IK at all")
{
    // The preview's case, and the strongest form of the claim: not "close to",
    // BIT FOR BIT. A foot solved against a guessed ground plane would look
    // plausible in the editor and be wrong in the game, which is the one thing a
    // preview may never be.
    auto plain = makeBipedRig();
    SceneSystems::tickAnimation(plain->world, plain->cm, 1.0f / 60.0f);
    const std::vector<glm::mat4> want = plain->bones();

    auto withIk = makeBipedRig();
    withIk->world.addComponent(withIk->entity, standingFeet());
    SceneSystems::tickAnimation(withIk->world, withIk->cm, 1.0f / 60.0f);
    const std::vector<glm::mat4>& got = withIk->bones();

    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                CHECK(got[i][c][r] == want[i][c][r]);

    // And the smoothing state stayed at rest, so switching into play mode does
    // not ease out of an offset that was never real.
    const auto& ic = withIk->world.registry().get<IkComponent>(withIk->entity);
    CHECK(ic.feet[0].smoothedOffset == 0.0f);
    CHECK_FALSE(ic.feet[0].primed);
}

TEST_CASE("Look-at runs without physics, because that is the whole point of it")
{
    auto plain = makeBipedRig();
    SceneSystems::tickAnimation(plain->world, plain->cm, 1.0f / 60.0f);
    const std::vector<glm::mat4> want = plain->bones();

    auto rig = makeBipedRig();
    IkComponent ik = standingFeet();
    ik.lookAt.enabled      = true;
    ik.lookAt.chain        = { "Spine", "Neck", "Head" };
    ik.lookAt.chainWeights = { 0.2f, 0.3f, 0.5f };
    ik.lookAt.targetWorld  = { -3.0f, 1.7f, 0.0f };   // 90° to the left, so it clamps
    rig->world.addComponent(rig->entity, ik);
    SceneSystems::tickAnimation(rig->world, rig->cm, 1.0f / 60.0f);

    // The head TURNED and the legs did not. Turned, not moved: the head sits
    // directly above the neck, so a yaw about the neck leaves its position exactly
    // where it was — reading a position here would be a test that passes on a
    // look-at that does nothing.
    const glm::vec3 look = glm::normalize(glm::vec3(rig->bones()[9] * glm::vec4(0, 0, -1, 0)));
    CHECK(glm::degrees(std::atan2(-look.x, -look.z)) == doctest::Approx(70.0f).epsilon(0.01f));
    CHECK(glm::length(glm::vec3(rig->bones()[3][3]) - glm::vec3(want[3][3])) < 1.0e-6f);

    // First frame SNAPS rather than easing in from zero — a character that spawns
    // looking forward and then swings its head round over half a second is not
    // what "look at this" means.
    const auto& ic = rig->world.registry().get<IkComponent>(rig->entity);
    CHECK(ic.lookAt.primed);
    CHECK(ic.lookAt.smoothedAngles.x == doctest::Approx(70.0f).epsilon(0.01f));
}

TEST_CASE("Foot IK on real ground: both feet land, and one that finds nothing gives up")
{
    auto rig = makeBipedRig();
    buildFloor(rig->world, 0.0f);
    rig->world.addComponent(rig->entity, standingFeet());

    PhysicsWorld phys;
    phys.initialize(rig->world);
    HE::RootMotionContext ctx{ &phys };

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 90; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, dt, nullptr, &ctx);

    // The stand pose puts the feet ~2 cm above the character's origin; on a floor
    // at y = 0 the pelvis comes down by that much and the soles reach the ground.
    CHECK(rig->jointWorld(3).y == doctest::Approx(0.0f).epsilon(0.02f));
    CHECK(rig->jointWorld(6).y == doctest::Approx(0.0f).epsilon(0.02f));

    SUBCASE("a foot over a hole keeps the other one on the ground")
    {
        // The floor moves out from under the LEFT foot only (it stands at
        // x = -0.1; the slab now starts at x = +0.05).
        auto rig2 = makeBipedRig();
        buildFloor(rig2->world, 0.0f, glm::vec3(0.0f), glm::vec3(10.05f, 0.0f, 0.0f));
        rig2->world.addComponent(rig2->entity, standingFeet());

        PhysicsWorld phys2;
        phys2.initialize(rig2->world);
        HE::RootMotionContext ctx2{ &phys2 };
        for (int i = 0; i < 90; ++i)
            SceneSystems::tickAnimation(rig2->world, rig2->cm, dt, nullptr, &ctx2);

        const auto& ic = rig2->world.registry().get<IkComponent>(rig2->entity);
        CHECK_FALSE(ic.feet[0].primed);                 // left foot: nothing under it
        CHECK(ic.feet[0].smoothedOffset == 0.0f);
        CHECK(ic.feet[1].primed);                       // right foot: still solved
        CHECK(rig2->jointWorld(6).y == doctest::Approx(0.0f).epsilon(0.02f));
    }
}

TEST_CASE("Foot IK eases onto a step instead of jumping onto it")
{
    // The stair-edge case. The ground under the character rises 20 cm between one
    // frame and the next, which is exactly what a real step does to a downward
    // ray, and the foot must not teleport with it.
    auto rig = makeBipedRig();
    buildFloor(rig->world, 0.0f);
    rig->world.addComponent(rig->entity, standingFeet());

    PhysicsWorld phys;
    phys.initialize(rig->world);
    HE::RootMotionContext ctx{ &phys };

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, dt, nullptr, &ctx);
    const float settled = rig->jointWorld(3).y;
    CHECK(settled == doctest::Approx(0.0f).epsilon(0.02f));

    // A second slab 20 cm up, right where the character stands.
    buildFloor(rig->world, 0.2f);
    phys.initialize(rig->world);

    SceneSystems::tickAnimation(rig->world, rig->cm, dt, nullptr, &ctx);
    const float afterOne = rig->jointWorld(3).y;
    // Moved towards the step, but nowhere near all the way: interpSpeed 10 over
    // one 60th of a second is about 15 % of the distance.
    CHECK(afterOne > settled + 0.005f);
    CHECK(afterOne < settled + 0.15f);

    for (int i = 0; i < 60; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, dt, nullptr, &ctx);
    CHECK(rig->jointWorld(3).y == doctest::Approx(0.2f).epsilon(0.05f));
}

TEST_CASE("Foot IK on a slope: both feet reach the surface and the ankles stay inside their limits")
{
    // 30° of tilt about Z: the two feet stand at different heights, each one's
    // normal leans sideways, and the slope is steeper than the 20° roll limit —
    // so the clamp has something to do rather than being merely satisfied.
    constexpr float kSlopeDegrees = 30.0f;
    constexpr float dt = 1.0f / 60.0f;

    auto run = [](bool align)
    {
        auto rig = makeBipedRig();
        buildFloor(rig->world, 0.0f, glm::vec3(0.0f, 0.0f, kSlopeDegrees));
        IkComponent ik = standingFeet();
        for (auto& f : ik.feet) { f.alignToNormal = align; f.maxRollDegrees = 20.0f; }
        rig->world.addComponent(rig->entity, ik);

        auto phys = std::make_shared<PhysicsWorld>();
        phys->initialize(rig->world);
        HE::RootMotionContext ctx{ phys.get() };
        for (int i = 0; i < 120; ++i)
            SceneSystems::tickAnimation(rig->world, rig->cm, dt, nullptr, &ctx);
        // The physics world has to outlive the rig it indexed into.
        return std::make_pair(std::move(rig), phys);
    };

    auto [flatFoot, physA] = run(false);
    auto [tilted,   physB] = run(true);

    // The slab is rotated about its own centre, so its top face does NOT pass
    // through the origin — the plane is worked out from the geometry rather than
    // assumed, which is the difference between testing the IK and testing a guess
    // about where the floor ended up.
    const glm::quat rot   = glm::angleAxis(glm::radians(kSlopeDegrees), glm::vec3(0, 0, 1));
    const glm::vec3 n     = rot * glm::vec3(0, 1, 0);
    const glm::vec3 p0    = glm::vec3(0.0f, -0.5f, 0.0f) + rot * glm::vec3(0.0f, 0.5f, 0.0f);
    auto groundY = [&](const glm::vec3& p) { return p0.y - (n.x * (p.x - p0.x)) / n.y; };

    const glm::vec3 footL = tilted->jointWorld(3);
    const glm::vec3 footR = tilted->jointWorld(6);
    CHECK(footL.y == doctest::Approx(groundY(footL)).epsilon(0.05f));
    CHECK(footR.y == doctest::Approx(groundY(footR)).epsilon(0.05f));
    // And they really are at different heights, or the two checks above would
    // pass on a flat floor too.
    CHECK(std::fabs(footL.y - footR.y) > 0.05f);

    // The ankle rotation is what the CLAMP limits, and a clamp on a delta has to
    // be measured as a delta: the animated pose already tilts the foot, and the
    // limit is on what the ground ADDS to it, not on where it ends up.
    //
    // Not asserted as exactly 20° either. The limit is on the roll about the
    // character's forward axis; the total angle between two sole normals is that
    // roll composed with the pitch the animation already had, and composing two
    // rotations about different axes does not add their angles. What has to hold
    // is that the ground moved the ankle a long way and still never past the line.
    for (int j : { 3, 6 })
    {
        const glm::vec3 before =
            glm::normalize(glm::vec3(flatFoot->bones()[j] * glm::vec4(0, 1, 0, 0)));
        const glm::vec3 after =
            glm::normalize(glm::vec3(tilted->bones()[j] * glm::vec4(0, 1, 0, 0)));
        const float added = glm::degrees(std::acos(glm::clamp(glm::dot(before, after), -1.0f, 1.0f)));
        CHECK(added > 10.0f);          // the ground was followed
        CHECK(added <= 20.01f);        // and the 30° it asked for was refused
    }
}

TEST_CASE("IK serialisation: the authored fields survive, the runtime ones do not")
{
    HorizonWorld world;
    const entt::entity e = world.createEntity("Character");
    world.addComponent(e, TransformComponent{});

    IkComponent ik;
    IkComponent::FootIk f;
    f.footJoint        = "FootL";
    f.kneeJoint        = "KneeL";
    f.hipJoint         = "HipL";
    f.weight           = 0.8f;
    f.traceUp          = 0.4f;
    f.traceDown        = 0.7f;
    f.footHeightOffset = 0.06f;
    f.alignToNormal    = false;
    f.maxPitchDegrees  = 25.0f;
    f.maxRollDegrees   = 15.0f;
    f.interpSpeed      = 12.0f;
    f.smoothedOffset   = -0.13f;   // runtime: must NOT come back
    f.primed           = true;     // runtime: must NOT come back
    ik.feet.push_back(f);
    ik.adjustPelvis = false;
    ik.pelvisJoint  = "Pelvis";
    ik.lookAt.enabled        = true;
    ik.lookAt.chain          = { "Spine", "Neck", "Head" };
    ik.lookAt.chainWeights   = { 0.2f, 0.3f, 0.5f };
    ik.lookAt.targetEntityId = HE::UUID::generate();
    ik.lookAt.targetWorld    = { 1.0f, 2.0f, 3.0f };
    ik.lookAt.forwardLocal   = { 0.0f, 0.0f, 1.0f };
    ik.lookAt.weight         = 0.6f;
    ik.lookAt.maxYawDegrees  = 55.0f;
    ik.lookAt.maxPitchDegrees = 35.0f;
    ik.lookAt.interpSpeed    = 6.0f;
    ik.lookAt.smoothedAngles = { 12.0f, 3.0f };   // runtime: must NOT come back
    ik.lookAt.primed         = true;              // runtime: must NOT come back
    ik.resolvedForMeshId     = HE::UUID::generate();
    ik.resolvedPelvis        = 4;
    world.addComponent(e, ik);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(world, blob));

    HorizonWorld back;
    REQUIRE(ser.loadFromMemory(back, blob));

    entt::entity loaded = entt::null;
    for (auto [ent, c] : back.registry().view<IkComponent>().each()) { loaded = ent; break; }
    REQUIRE(loaded != entt::entity{entt::null});
    const auto& got = back.registry().get<IkComponent>(loaded);

    REQUIRE(got.feet.size() == 1);
    CHECK(got.feet[0].footJoint        == "FootL");
    CHECK(got.feet[0].kneeJoint        == "KneeL");
    CHECK(got.feet[0].hipJoint         == "HipL");
    CHECK(got.feet[0].weight           == doctest::Approx(0.8f));
    CHECK(got.feet[0].traceUp          == doctest::Approx(0.4f));
    CHECK(got.feet[0].traceDown        == doctest::Approx(0.7f));
    CHECK(got.feet[0].footHeightOffset == doctest::Approx(0.06f));
    CHECK(got.feet[0].alignToNormal    == false);
    CHECK(got.feet[0].maxPitchDegrees  == doctest::Approx(25.0f));
    CHECK(got.feet[0].maxRollDegrees   == doctest::Approx(15.0f));
    CHECK(got.feet[0].interpSpeed      == doctest::Approx(12.0f));
    CHECK(got.adjustPelvis             == false);
    CHECK(got.pelvisJoint              == "Pelvis");
    CHECK(got.lookAt.enabled           == true);
    REQUIRE(got.lookAt.chain.size()        == 3);
    REQUIRE(got.lookAt.chainWeights.size() == 3);
    CHECK(got.lookAt.chain[1]          == "Neck");
    CHECK(got.lookAt.chainWeights[2]   == doctest::Approx(0.5f));
    CHECK(got.lookAt.targetEntityId    == ik.lookAt.targetEntityId);
    CHECK(got.lookAt.targetWorld.z     == doctest::Approx(3.0f));
    CHECK(got.lookAt.forwardLocal.z    == doctest::Approx(1.0f));
    CHECK(got.lookAt.weight            == doctest::Approx(0.6f));
    CHECK(got.lookAt.maxYawDegrees     == doctest::Approx(55.0f));
    CHECK(got.lookAt.maxPitchDegrees   == doctest::Approx(35.0f));
    CHECK(got.lookAt.interpSpeed       == doctest::Approx(6.0f));

    // The runtime half came back at its defaults. `primed` above all: loaded as
    // true, the first frame after a load would EASE the foot in from an offset
    // measured against a ground in a different session.
    CHECK(got.feet[0].smoothedOffset   == 0.0f);
    CHECK_FALSE(got.feet[0].primed);
    CHECK(got.lookAt.smoothedAngles.x  == 0.0f);
    CHECK_FALSE(got.lookAt.primed);
    CHECK(got.resolvedForMeshId        == HE::UUID{});
    CHECK(got.resolvedPelvis           == -1);
    CHECK(got.jointsDirty);
    CHECK_FALSE(got.hasLastEntityPos);
}
