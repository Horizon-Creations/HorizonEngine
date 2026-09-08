#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <BlendSpace/BlendSpace.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/AnimationPose.h>
#include <HorizonScene/AnimationPreview.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

// ─────────────────────────────────────────────────────────────────────────────
//  Blend spaces: 1D and 2D weighting, N-way pose mixing, and the shared PHASE
//  that keeps clips of different length in step.
//
//  All physics-free. The weighting half is pure arithmetic and is checked
//  directly; the playhead half is checked through a state machine, because that
//  is the thing whose `clipTime` changes meaning depending on the state it is in.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

constexpr std::array<float, 16> kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

// Root(0) ─ Spine(1) ─ Arm(2)
SkeletalMeshAsset makeSkeleton(const HE::UUID& meshId)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "bsSkel";
    auto joint = [&](const char* name, int parent)
    {
        SkeletonJoint j; j.name = name; j.parent = parent; j.inverseBindMatrix = kIdentity;
        sma.skeleton.push_back(j);
    };
    joint("Root",  -1);
    joint("Spine",  0);
    joint("Arm",    1);
    return sma;
}

// The Arm rotates linearly from 0° to `degrees` about Y over the clip. LINEAR on
// purpose: the phase tests read the pose back and ask WHERE in its own cycle each
// clip was sampled, which only works if the pose says so.
AnimationClipAsset makeRampClip(const char* name, float degrees, float duration)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = name;

    const glm::quat q0 = glm::angleAxis(0.0f, glm::vec3(0, 1, 0));
    const glm::quat q1 = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 1, 0));
    AnimationChannel ch;
    ch.jointIndex = 2;
    ch.path       = AnimPathType::Rotation;
    ch.times      = { 0.0f, duration };
    ch.values     = { q0.x, q0.y, q0.z, q0.w,  q1.x, q1.y, q1.z, q1.w };  // glTF xyzw
    clip.channels.push_back(std::move(ch));
    return clip;
}

// A clip holding ONE joint at a constant rotation for its whole duration.
AnimationClipAsset makeHoldClip(const char* name, uint32_t joint, float degrees,
                                float duration = 1.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = name;

    const glm::quat q = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 1, 0));
    AnimationChannel ch;
    ch.jointIndex = joint;
    ch.path       = AnimPathType::Rotation;
    ch.times      = { 0.0f, duration };
    ch.values     = { q.x, q.y, q.z, q.w,  q.x, q.y, q.z, q.w };
    clip.channels.push_back(std::move(ch));
    return clip;
}

// The root walks `distance` along +Z over the whole clip.
AnimationClipAsset makeWalkClip(const char* name, float duration, float distance)
{
    AnimationClipAsset clip;
    clip.duration      = duration;
    clip.name          = name;
    clip.hasRootMotion = true;

    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, duration };
    ch.values     = { 0.0f, 0.0f, 0.0f,  0.0f, 0.0f, distance };
    clip.channels.push_back(std::move(ch));
    return clip;
}

HE::BlendSpace oneD(std::initializer_list<std::pair<HE::UUID, float>> samples,
                    const char* paramX = "Speed")
{
    HE::BlendSpace s;
    s.kind   = HE::BlendSpaceKind::OneD;
    s.paramX = paramX;
    for (const auto& [id, x] : samples)
    {
        HE::BlendSpaceSample e; e.clipId = id; e.x = x;
        s.samples.push_back(e);
    }
    return s;
}

HE::UUID registerSpace(ContentManager& cm, const HE::BlendSpace& s)
{
    BlendSpaceAsset a;
    a.name = s.name.empty() ? "space" : s.name;
    a.json = HE::blendSpaceToJson(s);
    return cm.registerBlendSpace(std::move(a));
}

float sum(const std::vector<float>& v)
{
    return std::accumulate(v.begin(), v.end(), 0.0f);
}

// The Y rotation the Arm ended up with, in degrees. The pose tests all read this:
// it is one number and it says exactly where in its cycle a clip was sampled.
float armYawDegrees(const std::vector<glm::mat4>& bones)
{
    const glm::quat q = glm::quat_cast(glm::mat3(bones[2]));
    return glm::degrees(2.0f * std::atan2(q.y, q.w));
}

// One entity with a state machine, one state, and whatever pose source the test
// puts on it.
struct Rig
{
    ContentManager cm;
    HorizonWorld   world;
    HE::UUID       meshId;
    HE::UUID       graphId;
    entt::entity   entity = entt::null;

    AnimatorStateMachineComponent& sm()
    {
        return world.registry().get<AnimatorStateMachineComponent>(entity);
    }
    const std::vector<glm::mat4>& bones()
    {
        return world.registry().get<SkeletalMeshComponent>(entity).boneMatrices;
    }
};

std::unique_ptr<Rig> makeRig(const HE::AnimatorStateMachineGraph& graph)
{
    auto rig = std::make_unique<Rig>();
    rig->meshId = HE::UUID::generate();
    rig->cm.registerSkeletalMesh(makeSkeleton(rig->meshId));

    AnimatorStateMachineAsset asset;
    asset.name      = "graph";
    asset.graphJson = HE::animatorStateMachineToJson(graph);
    rig->graphId    = rig->cm.registerAnimatorStateMachine(std::move(asset));

    rig->entity = rig->world.createEntity("Character");
    rig->world.addComponent(rig->entity, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = rig->meshId;
    rig->world.addComponent(rig->entity, smc);
    AnimatorStateMachineComponent sm; sm.stateMachineAssetId = rig->graphId;
    rig->world.addComponent(rig->entity, sm);
    return rig;
}

int countOf(const HE::NotifyQueue& q, const char* name)
{
    int n = 0;
    for (const auto& ev : q) if (ev.name == name) ++n;
    return n;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  1–3. 1D weighting
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("1D blend space: standing exactly on a sample gives it all the weight")
{
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));
    const HE::UUID walk = cm.registerAnimationClip(makeHoldClip("walk", 2, 10.0f, 1.0f));
    const HE::UUID jog  = cm.registerAnimationClip(makeHoldClip("jog",  2, 20.0f, 1.0f));
    const HE::UUID run  = cm.registerAnimationClip(makeHoldClip("run",  2, 30.0f, 1.0f));

    const HE::BlendSpace s = oneD({ {walk, 0.0f}, {jog, 1.0f}, {run, 2.0f} });

    std::vector<float> w;
    HE::blendSpaceWeights(s, 1.0f, 0.0f, w);
    CHECK(w[0] == doctest::Approx(0.0f));
    CHECK(w[1] == doctest::Approx(1.0f));
    CHECK(w[2] == doctest::Approx(0.0f));

    // And the pose is that clip's, to the bit: a mix of one is not a mix.
    std::vector<glm::mat4> mixed, direct;
    AnimationPreview::evaluateBlendSpacePose(*cm.getSkeletalMesh(meshId), cm, s, 1.0f, 0.0f, 0.5f, mixed);
    AnimationPreview::evaluateClipPose(*cm.getSkeletalMesh(meshId), *cm.getAnimationClip(jog), 0.5f, direct);
    REQUIRE(mixed.size() == direct.size());
    CHECK(std::memcmp(mixed.data(), direct.data(), mixed.size() * sizeof(glm::mat4)) == 0);
}

TEST_CASE("1D blend space: between two samples the weights are linear in x and sum to 1")
{
    const HE::UUID a = HE::UUID::generate(), b = HE::UUID::generate(), c = HE::UUID::generate();
    const HE::BlendSpace s = oneD({ {a, 0.0f}, {b, 2.0f}, {c, 6.0f} });

    std::vector<float> w;
    HE::blendSpaceWeights(s, 0.5f, 0.0f, w);
    CHECK(w[0] == doctest::Approx(0.75f));
    CHECK(w[1] == doctest::Approx(0.25f));
    CHECK(w[2] == doctest::Approx(0.0f));
    CHECK(sum(w) == doctest::Approx(1.0f));

    // The far pair, so this is bracketing and not "the two nearest to zero".
    HE::blendSpaceWeights(s, 5.0f, 0.0f, w);
    CHECK(w[0] == doctest::Approx(0.0f));
    CHECK(w[1] == doctest::Approx(0.25f));
    CHECK(w[2] == doctest::Approx(0.75f));
    CHECK(sum(w) == doctest::Approx(1.0f));
}

TEST_CASE("1D blend space: outside the sample range it CLAMPS, it does not extrapolate")
{
    // A character at speed 12 must not move its legs twice as fast as the fastest
    // sample was authored for. The edge sample takes everything.
    const HE::UUID a = HE::UUID::generate(), b = HE::UUID::generate();
    const HE::BlendSpace s = oneD({ {a, 1.0f}, {b, 4.0f} });

    std::vector<float> w;
    HE::blendSpaceWeights(s, -50.0f, 0.0f, w);
    CHECK(w[0] == doctest::Approx(1.0f));
    CHECK(w[1] == doctest::Approx(0.0f));

    HE::blendSpaceWeights(s, 12.0f, 0.0f, w);
    CHECK(w[0] == doctest::Approx(0.0f));
    CHECK(w[1] == doctest::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  4. 2D gradient band
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("2D blend space: gradient band is exactly 1 on a sample and 1/3 at a symmetric centroid")
{
    HE::BlendSpace s;
    s.kind = HE::BlendSpaceKind::TwoD;
    s.paramX = "X"; s.paramY = "Y";
    // An equilateral triangle centred on the origin.
    const float r = 1.0f;
    for (int k = 0; k < 3; ++k)
    {
        const float ang = glm::radians(90.0f + 120.0f * static_cast<float>(k));
        HE::BlendSpaceSample e;
        e.clipId = HE::UUID::generate();
        e.x = r * std::cos(ang);
        e.y = r * std::sin(ang);
        s.samples.push_back(e);
    }

    std::vector<float> w;
    for (size_t i = 0; i < 3; ++i)
    {
        HE::blendSpaceWeights(s, s.samples[i].x, s.samples[i].y, w);
        CHECK(w[i] == doctest::Approx(1.0f));
        CHECK(sum(w) == doctest::Approx(1.0f));
    }

    HE::blendSpaceWeights(s, 0.0f, 0.0f, w);
    for (size_t i = 0; i < 3; ++i) CHECK(w[i] == doctest::Approx(1.0f / 3.0f));
    CHECK(sum(w) == doctest::Approx(1.0f));

    // And anywhere else it still sums to 1 — the property the mix depends on.
    for (float x = -1.0f; x <= 1.0f; x += 0.37f)
        for (float y = -1.0f; y <= 1.0f; y += 0.41f)
        {
            HE::blendSpaceWeights(s, x, y, w);
            CHECK(sum(w) == doctest::Approx(1.0f));
        }
}

// ─────────────────────────────────────────────────────────────────────────────
//  5. The four-sample cap
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("2D blend space: at most four samples survive, renormalised to 1")
{
    HE::BlendSpace s;
    s.kind = HE::BlendSpaceKind::TwoD;
    // Six samples on a circle around the origin: the gradient band gives every
    // one of them a share there, which is exactly the case the cap exists for.
    for (int k = 0; k < 6; ++k)
    {
        const float ang = glm::radians(60.0f * static_cast<float>(k));
        HE::BlendSpaceSample e;
        e.clipId = HE::UUID::generate();
        e.x = std::cos(ang);
        e.y = std::sin(ang);
        s.samples.push_back(e);
    }

    std::vector<float> w;
    // Off-centre, so the six weights differ and the cap has an unambiguous order
    // to cut on.
    HE::blendSpaceWeights(s, 0.13f, 0.07f, w);

    int nonZero = 0;
    for (float v : w) if (v > 0.0f) ++nonZero;
    CHECK(nonZero <= static_cast<int>(HE::kBlendSpaceMaxActiveSamples));
    CHECK(nonZero == 4);
    CHECK(sum(w) == doctest::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  10. N-way mixing
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blendPosesN: two samples reproduce blendTRS exactly, three interpolate between all of them")
{
    JointTRS a, b, c;
    a.translation = glm::vec3(0.0f);
    b.translation = glm::vec3(4.0f, 0.0f, 0.0f);
    c.translation = glm::vec3(0.0f, 8.0f, 0.0f);
    a.rotation = glm::angleAxis(glm::radians(0.0f),  glm::vec3(0, 1, 0));
    b.rotation = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0));
    c.rotation = glm::angleAxis(glm::radians(80.0f), glm::vec3(0, 1, 0));

    // Dyadic weights on purpose: w1/(w0+w1) is only EXACTLY w1 when w0+w1 is
    // exactly 1.0f, and 0.25/0.75 is exact in binary where 0.3/0.7 is not. A
    // bit-for-bit claim has to be given weights that can carry it.
    {
        const std::vector<std::vector<JointTRS>> poses = { {a}, {b} };
        const std::vector<float> w = { 0.75f, 0.25f };
        std::vector<JointTRS> got, want;
        HE::blendPosesN(poses, w, got);
        blendTRS({a}, {b}, 0.25f, want);
        REQUIRE(got.size() == 1);
        CHECK(std::memcmp(&got[0], &want[0], sizeof(JointTRS)) == 0);
    }

    // Order does not decide the answer: the chain is walked heaviest-first no
    // matter which way round the caller listed them.
    {
        const std::vector<float> w1 = { 0.75f, 0.25f };
        const std::vector<float> w2 = { 0.25f, 0.75f };
        std::vector<JointTRS> g1, g2;
        HE::blendPosesN({ {a}, {b} }, w1, g1);
        HE::blendPosesN({ {b}, {a} }, w2, g2);
        CHECK(std::memcmp(&g1[0], &g2[0], sizeof(JointTRS)) == 0);
    }

    // THREE samples — more than two, which is the whole point of a blend tree.
    // Equal thirds put the translation at the mean of the three.
    {
        const std::vector<std::vector<JointTRS>> poses = { {a}, {b}, {c} };
        const std::vector<float> w = { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
        std::vector<JointTRS> got;
        HE::blendPosesN(poses, w, got);
        REQUIRE(got.size() == 1);
        CHECK(got[0].translation.x == doctest::Approx(4.0f / 3.0f));
        CHECK(got[0].translation.y == doctest::Approx(8.0f / 3.0f));
        // The rotation lands between the outer two, and past the middle one's own
        // 40° is impossible for a convex mix of 0/40/80.
        const float yaw = glm::degrees(2.0f * std::atan2(got[0].rotation.y, got[0].rotation.w));
        CHECK(yaw > 0.0f);
        CHECK(yaw < 80.0f);
    }

    // A sample at weight 0 is skipped, not blended with alpha 0 — the result is
    // bit-identical to leaving it out.
    {
        std::vector<JointTRS> withZero, without;
        HE::blendPosesN({ {a}, {b}, {c} }, { 0.75f, 0.25f, 0.0f }, withZero);
        HE::blendPosesN({ {a}, {b} },      { 0.75f, 0.25f },       without);
        CHECK(std::memcmp(&withZero[0], &without[0], sizeof(JointTRS)) == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  6–7. Phase, not absolute time
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blend space runs on a shared PHASE: clips of different length are read at the same fraction")
{
    // The test that separates a blend space from the old two-clip blend. Walk is
    // 1.2 s and run is 0.8 s. At phase 0.5 the walk must be read at 0.6 s and the
    // run at 0.4 s — both exactly halfway. Wrapped against their own durations on
    // one absolute clock they would be at 0.5 s and 0.5 s, i.e. 42 % and 63 %
    // through their cycles, and the feet cross.
    ContentManager cm;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeSkeleton(meshId));

    // Two ramps with the same total travel, so "how far through its own cycle"
    // reads straight off the pose.
    const HE::UUID walk = cm.registerAnimationClip(makeRampClip("walk", 100.0f, 1.2f));
    const HE::UUID run  = cm.registerAnimationClip(makeRampClip("run",  100.0f, 0.8f));

    const HE::BlendSpace s = oneD({ {walk, 0.0f}, {run, 1.0f} });

    // Each sample alone at phase 0.5 must read 50° — half of its own ramp,
    // whatever its duration is.
    std::vector<glm::mat4> bones;
    AnimationPreview::evaluateBlendSpacePose(*cm.getSkeletalMesh(meshId), cm, s, 0.0f, 0.0f, 0.5f, bones);
    CHECK(armYawDegrees(bones) == doctest::Approx(50.0f).epsilon(0.001));
    AnimationPreview::evaluateBlendSpacePose(*cm.getSkeletalMesh(meshId), cm, s, 1.0f, 0.0f, 0.5f, bones);
    CHECK(armYawDegrees(bones) == doctest::Approx(50.0f).epsilon(0.001));

    // And the 50/50 mix of two poses that are both at 50° is 50°, not something
    // in between two different places in two cycles.
    std::vector<float> w;
    AnimationPreview::evaluateBlendSpacePose(*cm.getSkeletalMesh(meshId), cm, s, 0.5f, 0.0f, 0.5f, bones, &w);
    CHECK(w[0] == doctest::Approx(0.5f));
    CHECK(w[1] == doctest::Approx(0.5f));
    CHECK(armYawDegrees(bones) == doctest::Approx(50.0f).epsilon(0.001));

    // Sampled at ABSOLUTE 0.5 s the two would disagree — which is what makes the
    // check above a real one and not a tautology.
    std::vector<glm::mat4> wAbs, rAbs;
    AnimationPreview::evaluateClipPose(*cm.getSkeletalMesh(meshId), *cm.getAnimationClip(walk), 0.5f, wAbs);
    AnimationPreview::evaluateClipPose(*cm.getSkeletalMesh(meshId), *cm.getAnimationClip(run),  0.5f, rAbs);
    CHECK(armYawDegrees(wAbs) != doctest::Approx(armYawDegrees(rAbs)));
}

TEST_CASE("blend space: the weighted duration is what one lap costs, and the phase wraps at 1")
{
    ContentManager cm;
    const HE::UUID walk = cm.registerAnimationClip(makeRampClip("walk", 90.0f, 1.2f));
    const HE::UUID run  = cm.registerAnimationClip(makeRampClip("run",  90.0f, 0.8f));

    const HE::BlendSpace s = oneD({ {walk, 0.0f}, {run, 1.0f} });
    const std::vector<float> w = { 0.5f, 0.5f };
    const std::vector<float> d = { 1.2f, 0.8f };
    CHECK(HE::blendSpaceWeightedDuration(s, w, d) == doctest::Approx(1.0f));

    // speedScale divides: a sample declared twice as fast costs half the seconds.
    HE::BlendSpace fast = s;
    fast.samples[0].speedScale = 2.0f;
    CHECK(HE::blendSpaceWeightedDuration(fast, w, d) == doctest::Approx(0.5f * 0.6f + 0.5f * 0.8f));

    // A sample whose clip is missing (duration 0) contributes nothing.
    const std::vector<float> halfMissing = { 1.2f, 0.0f };
    CHECK(HE::blendSpaceWeightedDuration(s, w, halfMissing) == doctest::Approx(0.6f));

    // And in the state machine: after exactly one weighted duration the phase is
    // back where it started.
    HE::AnimatorStateMachineGraph g;
    HE::AnimationState st; st.id = 1; st.name = "Locomotion"; st.looping = true;
    g.states.push_back(st);
    auto rig = makeRig(g);

    // The clips a space names have to live in the manager the rig ticks against.
    const HE::UUID w2 = rig->cm.registerAnimationClip(makeRampClip("walk", 90.0f, 1.2f));
    const HE::UUID r2 = rig->cm.registerAnimationClip(makeRampClip("run",  90.0f, 0.8f));
    const HE::UUID spaceId = registerSpace(rig->cm, oneD({ {w2, 0.0f}, {r2, 1.0f} }));
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].blendSpaceId = spaceId;
        AnimatorStateMachineAsset* a = rig->cm.getAnimatorStateMachineMutable(rig->graphId);
        REQUIRE(a != nullptr);
        a->graphJson = HE::animatorStateMachineToJson(g2);
    }

    rig->sm().params["Speed"] = 0.5f;   // 50/50 → weighted duration 1.0 s
    for (int i = 0; i < 10; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    // Ten frames of 0.1 s is exactly one lap: the phase is back at (near) 0.
    const float p = rig->sm().clipTime;
    CHECK((p < 1e-3f || p > 1.0f - 1e-3f));

    // Half a lap in, the phase says 0.5 and not "0.5 seconds".
    rig->sm().clipTime = 0.0f;
    for (int i = 0; i < 5; ++i)
        SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->sm().clipTime == doctest::Approx(0.5f).epsilon(0.01));
}

// ─────────────────────────────────────────────────────────────────────────────
//  8. Root motion out of a blend space
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blend space root motion: the delta is the weighted mean of each sample's OWN span")
{
    // Two walks over the same phase span but different clip lengths and different
    // travel. Each sample's delta comes out of its own seconds; the mix uses the
    // pose weights, so the figure cannot speed up exactly where the blend works.
    HE::AnimatorStateMachineGraph g;
    HE::AnimationState st; st.id = 1; st.name = "Walk"; st.looping = true;
    g.states.push_back(st);
    auto rig = makeRig(g);

    const HE::UUID slow = rig->cm.registerAnimationClip(makeWalkClip("slow", 1.0f, 2.0f));
    const HE::UUID fast = rig->cm.registerAnimationClip(makeWalkClip("fast", 1.0f, 6.0f));
    HE::BlendSpace s = oneD({ {slow, 0.0f}, {fast, 1.0f} });
    const HE::UUID spaceId = registerSpace(rig->cm, s);
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].blendSpaceId = spaceId;
        rig->cm.getAnimatorStateMachineMutable(rig->graphId)->graphJson =
            HE::animatorStateMachineToJson(g2);
    }

    RootMotionComponent rm;
    rm.mode = RootMotionComponent::Mode::Transform;
    rm.options.rootJointName = "Root";
    rig->world.addComponent(rig->entity, rm);

    rig->sm().params["Speed"] = 0.5f;   // 50/50
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    // Both clips are 1 s long, so 0.1 s of phase is 0.1 s of each; the deltas are
    // 0.2 and 0.6, and the 50/50 mean is 0.4.
    const auto& got = rig->world.registry().get<RootMotionComponent>(rig->entity);
    CHECK(got.lastDelta.z == doctest::Approx(0.4f).epsilon(0.02));

    // The root translation is LOCKED out of the pose — a blend space must not
    // leave the motion it just extracted in the mesh either.
    const glm::mat4 root = rig->bones()[0];
    CHECK(root[3][2] == doctest::Approx(0.0f).epsilon(0.001));
}

// ─────────────────────────────────────────────────────────────────────────────
//  9. Notifies: only the heaviest sample fires
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blend space notifies: only the heaviest sample fires, and a tie goes to the lower index")
{
    HE::AnimatorStateMachineGraph g;
    HE::AnimationState st; st.id = 1; st.name = "Locomotion"; st.looping = true;
    g.states.push_back(st);
    auto rig = makeRig(g);

    auto notifyClip = [&](const char* clipName, const char* notifyName)
    {
        AnimationClipAsset c = makeHoldClip(clipName, 2, 10.0f, 1.0f);
        AnimationNotify n; n.name = notifyName; n.time = 0.0f;
        c.notifies.push_back(std::move(n));
        return rig->cm.registerAnimationClip(std::move(c));
    };
    const HE::UUID a = notifyClip("a", "stepA");
    const HE::UUID b = notifyClip("b", "stepB");

    const HE::UUID spaceId = registerSpace(rig->cm, oneD({ {a, 0.0f}, {b, 1.0f} }));
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].blendSpaceId = spaceId;
        rig->cm.getAnimatorStateMachineMutable(rig->graphId)->graphJson =
            HE::animatorStateMachineToJson(g2);
    }

    // Heavier on b: only b's notify comes out.
    rig->sm().params["Speed"] = 0.8f;
    HE::NotifyQueue q;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.05f, nullptr, nullptr, &q);
    CHECK(countOf(q, "stepB") == 1);
    CHECK(countOf(q, "stepA") == 0);

    // A dead-even tie goes to the lower index, and stays there frame after frame.
    auto rig2 = makeRig(g);
    const HE::UUID a2 = [&]{
        AnimationClipAsset c = makeHoldClip("a", 2, 10.0f, 1.0f);
        AnimationNotify n; n.name = "stepA"; n.time = 0.5f; c.notifies.push_back(n);
        return rig2->cm.registerAnimationClip(std::move(c));
    }();
    const HE::UUID b2 = [&]{
        AnimationClipAsset c = makeHoldClip("b", 2, 10.0f, 1.0f);
        AnimationNotify n; n.name = "stepB"; n.time = 0.5f; c.notifies.push_back(n);
        return rig2->cm.registerAnimationClip(std::move(c));
    }();
    const HE::UUID spaceId2 = registerSpace(rig2->cm, oneD({ {a2, 0.0f}, {b2, 1.0f} }));
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].blendSpaceId = spaceId2;
        rig2->cm.getAnimatorStateMachineMutable(rig2->graphId)->graphJson =
            HE::animatorStateMachineToJson(g2);
    }
    rig2->sm().params["Speed"] = 0.5f;   // exactly 50/50
    HE::NotifyQueue q2;
    for (int i = 0; i < 30; ++i)
        SceneSystems::tickAnimation(rig2->world, rig2->cm, 0.05f, nullptr, nullptr, &q2);
    CHECK(countOf(q2, "stepA") >= 1);
    CHECK(countOf(q2, "stepB") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  11–12. The state machine's side of it
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationState: a set blendSpaceId wins over clipId, and an old graph loads unchanged")
{
    HE::AnimatorStateMachineGraph g;
    HE::AnimationState st; st.id = 1; st.name = "Loco"; st.looping = true;
    g.states.push_back(st);
    auto rig = makeRig(g);

    // The clip on the state rotates the ARM by 60°; the blend space rotates the
    // SPINE by 30°. Which joint moved says which source won.
    const HE::UUID armClip = rig->cm.registerAnimationClip(makeHoldClip("arm", 2, 60.0f));
    const HE::UUID spineClip = rig->cm.registerAnimationClip(makeHoldClip("spine", 1, 30.0f));
    const HE::UUID spaceId = registerSpace(rig->cm, oneD({ {spineClip, 0.0f} }));
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].clipId       = armClip;
        g2.states[0].blendSpaceId = spaceId;
        rig->cm.getAnimatorStateMachineMutable(rig->graphId)->graphJson =
            HE::animatorStateMachineToJson(g2);
    }
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.05f);
    const glm::quat spine = glm::quat_cast(glm::mat3(rig->bones()[1]));
    CHECK(glm::degrees(2.0f * std::atan2(spine.y, spine.w)) == doctest::Approx(30.0f).epsilon(0.01));

    // A graph JSON written before blend spaces existed has no such key at all,
    // and must read back with a null id and its clip intact.
    const std::string legacy =
        R"({"version":1,"startState":"Loco","states":[{"id":1,"name":"Loco",)"
        R"("clipId":null,"looping":true,"x":0.0,"y":0.0}],"transitions":[],"defaultParams":{}})";
    HE::AnimatorStateMachineGraph parsed;
    REQUIRE(HE::animatorStateMachineFromJson(legacy, parsed));
    REQUIRE(parsed.states.size() == 1);
    CHECK(parsed.states[0].blendSpaceId == HE::UUID{});
    CHECK(parsed.states[0].name == "Loco");

    // Round trip: what the writer emits, the reader gives back.
    HE::AnimationState round;
    HE::AnimationState src; src.id = 7; src.name = "S"; src.blendSpaceId = HE::UUID::generate();
    REQUIRE(HE::animationStateFromJson(HE::animationStateToJson(src), round));
    CHECK(round.blendSpaceId == src.blendSpaceId);
}

TEST_CASE("transition into a blend-space state starts it at phase 0")
{
    HE::AnimatorStateMachineGraph g;
    HE::AnimationState idle; idle.id = 1; idle.name = "Idle"; idle.looping = true;
    HE::AnimationState loco; loco.id = 2; loco.name = "Loco"; loco.looping = true;
    g.states = { idle, loco };
    g.startState = "Idle";
    HE::AnimationTransition t;
    t.fromState = "Idle"; t.toState = "Loco"; t.paramName = "Go";
    t.op = HE::TransitionOp::Greater; t.threshold = 0.5f; t.duration = 0.2f;
    g.transitions.push_back(t);

    auto rig = makeRig(g);
    const HE::UUID idleClip = rig->cm.registerAnimationClip(makeHoldClip("idle", 2, 0.0f, 1.0f));
    const HE::UUID locoClip = rig->cm.registerAnimationClip(makeRampClip("loco", 90.0f, 2.0f));
    const HE::UUID spaceId  = registerSpace(rig->cm, oneD({ {locoClip, 0.0f} }));
    {
        HE::AnimatorStateMachineGraph g2 = g;
        g2.states[0].clipId       = idleClip;
        g2.states[1].blendSpaceId = spaceId;
        rig->cm.getAnimatorStateMachineMutable(rig->graphId)->graphJson =
            HE::animatorStateMachineToJson(g2);
    }

    // Run the idle for a while, so the outgoing playhead is far from 0 and a
    // transition that reused it would be obvious.
    for (int i = 0; i < 7; ++i) SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->sm().clipTime > 0.1f);

    rig->sm().params["Go"] = 1.0f;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.05f);
    REQUIRE(rig->sm().inTransition);
    // One frame in: the incoming phase is 0.05 s / 2.0 s = 0.025, i.e. it started
    // at 0 and not wherever the crossfade clock or the outgoing playhead stood.
    CHECK(rig->sm().transitionPlayhead == doctest::Approx(0.025f).epsilon(0.01));

    // And when the crossfade completes, the phase carries over as a PHASE — a
    // number in [0, 1), not the 0.2 s the crossfade took.
    for (int i = 0; i < 5; ++i) SceneSystems::tickAnimation(rig->world, rig->cm, 0.05f);
    CHECK_FALSE(rig->sm().inTransition);
    CHECK(rig->sm().currentStateName == "Loco");
    CHECK(rig->sm().clipTime >= 0.0f);
    CHECK(rig->sm().clipTime < 1.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Serialisation of the asset itself
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blend space JSON: round trip, unknown kind falls back, bad speedScale is repaired")
{
    HE::BlendSpace s;
    s.name = "Strafe";
    s.kind = HE::BlendSpaceKind::TwoD;
    s.paramX = "Dir"; s.paramY = "Speed";
    s.minX = -180.0f; s.maxX = 180.0f; s.minY = 0.0f; s.maxY = 6.0f;
    s.looping = false;
    HE::BlendSpaceSample e; e.clipId = HE::UUID::generate(); e.x = -90.0f; e.y = 3.0f; e.speedScale = 1.5f;
    s.samples.push_back(e);

    HE::BlendSpace back;
    REQUIRE(HE::blendSpaceFromJson(HE::blendSpaceToJson(s), back));
    CHECK(back.name == "Strafe");
    CHECK(back.kind == HE::BlendSpaceKind::TwoD);
    CHECK(back.paramX == "Dir");
    CHECK(back.paramY == "Speed");
    CHECK(back.minX == doctest::Approx(-180.0f));
    CHECK(back.maxY == doctest::Approx(6.0f));
    CHECK_FALSE(back.looping);
    REQUIRE(back.samples.size() == 1);
    CHECK(back.samples[0].clipId == s.samples[0].clipId);
    CHECK(back.samples[0].x == doctest::Approx(-90.0f));
    CHECK(back.samples[0].speedScale == doctest::Approx(1.5f));

    // A kind this build does not have must not become an enum with no enumerator.
    HE::BlendSpace odd;
    REQUIRE(HE::blendSpaceFromJson(R"({"kind":99,"samples":[]})", odd));
    CHECK(odd.kind == HE::BlendSpaceKind::OneD);

    // A speedScale of 0 would divide the weighted duration by zero.
    HE::BlendSpace zero;
    REQUIRE(HE::blendSpaceFromJson(R"({"samples":[{"x":0,"speedScale":0}]})", zero));
    REQUIRE(zero.samples.size() == 1);
    CHECK(zero.samples[0].speedScale == doctest::Approx(1.0f));
}
