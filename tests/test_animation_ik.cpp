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
