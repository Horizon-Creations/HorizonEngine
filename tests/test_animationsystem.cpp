#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/AnimationStateMachineSystem.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <HorizonScene/AnimationPreview.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/EngineApi.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationClipAsset defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationClipAsset default duration is zero")
{
    AnimationClipAsset clip;
    CHECK(clip.duration == 0.0f);
    CHECK(clip.channels.empty());
}

TEST_CASE("AnimationClipAsset registers and retrieves from ContentManager")
{
    ContentManager cm;
    AnimationClipAsset clip;
    clip.duration = 2.0f;
    clip.name     = "TestClip";

    const HE::UUID id = cm.registerAnimationClip(std::move(clip));
    REQUIRE(id != HE::UUID{});
    REQUIRE(cm.assetType(id) == HE::AssetType::AnimationClip);

    const AnimationClipAsset* got = cm.getAnimationClip(id);
    REQUIRE(got != nullptr);
    CHECK(got->duration == doctest::Approx(2.0f));
    CHECK(got->name == "TestClip");
}

TEST_CASE("AnimationClipAsset wrong-type lookup returns nullptr")
{
    ContentManager cm;
    // Register a static mesh and try to fetch it as an AnimationClip.
    StaticMeshAsset mesh;
    const HE::UUID id = cm.registerStaticMesh(std::move(mesh));
    CHECK(cm.getAnimationClip(id) == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimatorComponent defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimatorComponent defaults")
{
    AnimatorComponent an;
    CHECK(an.clipAssetId    == HE::UUID{});
    CHECK(an.playbackTime   == doctest::Approx(0.0f));
    CHECK(an.playbackSpeed  == doctest::Approx(1.0f));
    CHECK(an.looping        == true);
    CHECK(an.playing        == true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationSystem: playback time advance
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a 1-joint skeleton with identity IBM
static SkeletalMeshAsset makeOneBoneSkeletalMesh(const HE::UUID& meshId)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "testSkel";
    SkeletonJoint root;
    root.name   = "Root";
    root.parent = -1;
    root.inverseBindMatrix = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
    };
    sma.skeleton.push_back(root);
    return sma;
}

// Helper: build a translation-only AnimationClipAsset:
//   t=0 → (0,0,0), t=1 → (1,0,0)
static AnimationClipAsset makeTranslationClip(float duration = 1.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "translateX";

    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, duration };
    ch.values     = { 0.0f, 0.0f, 0.0f,   // t=0 → origin
                      1.0f, 0.0f, 0.0f };  // t=end → +X
    clip.channels.push_back(std::move(ch));
    return clip;
}

TEST_CASE("AnimationSystem advances playbackTime")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.5f);

    const auto& updatedAn = world.registry().get<AnimatorComponent>(e);
    CHECK(updatedAn.playbackTime == doctest::Approx(0.5f));
}

TEST_CASE("AnimationSystem loops when looping=true")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = true; an.playbackTime = 0.8f;
    world.addComponent(e, an);

    // Advance by 0.5 → wraps from 0.8+0.5=1.3 → 1.3 % 1.0 = 0.3
    AnimationSystem::update(world, cm, 0.5f);
    const auto& updatedAn = world.registry().get<AnimatorComponent>(e);
    CHECK(updatedAn.playbackTime == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(updatedAn.playing == true);
}

TEST_CASE("AnimationSystem stops at end when looping=false")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = false; an.playbackTime = 0.9f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.5f);
    const auto& updatedAn = world.registry().get<AnimatorComponent>(e);
    CHECK(updatedAn.playbackTime == doctest::Approx(1.0f));
    CHECK(updatedAn.playing == false);
}

TEST_CASE("AnimationSystem wraps a rewinding looping clip forward, never negative")
{
    // std::fmod returns a NEGATIVE remainder for a negative playbackSpeed, so the
    // wrap adds one duration back. Without that, playbackTime would go negative and
    // sampleClip would clamp to the first keyframe — the clip would look frozen
    // instead of playing backwards. Shared by all three clip-driven animators
    // (advancePlayback), so it is pinned here once.
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.looping = true;
    an.playbackTime = 0.1f; an.playbackSpeed = -1.0f;
    world.addComponent(e, an);

    // 0.1 - 0.25 = -0.15 → fmod = -0.15 → + 1.0 = 0.85
    AnimationSystem::update(world, cm, 0.25f);
    const auto& updatedAn = world.registry().get<AnimatorComponent>(e);
    CHECK(updatedAn.playbackTime == doctest::Approx(0.85f).epsilon(1e-4));
    CHECK(updatedAn.playing == true);
}

TEST_CASE("AnimationSystem skips update when playing=false")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.playing = false; an.playbackTime = 0.5f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.3f);
    const auto& updatedAn = world.registry().get<AnimatorComponent>(e);
    // Time must NOT change because playing=false
    CHECK(updatedAn.playbackTime == doctest::Approx(0.5f));
}

TEST_CASE("AnimationSystem skips entity with no clip")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; // clipAssetId == null UUID
    world.addComponent(e, an);

    // Should not crash; boneMatrices stays at default (1 identity)
    AnimationSystem::update(world, cm, 0.5f);
    const auto& updatedSmc = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(updatedSmc.boneMatrices.size() == 1);
    CHECK(updatedSmc.boneMatrices[0] == glm::mat4(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationSystem: bone matrix evaluation (translation channel)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationSystem writes identity bone at t=0 (translation channel)")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    // dt=0 so playbackTime stays 0
    AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.0f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);
    // At t=0 translation=(0,0,0) and IBM=identity → bone = identity
    CHECK(s.boneMatrices[0] == glm::mat4(1.0f));
}

TEST_CASE("AnimationSystem interpolates translation bone matrix at t=0.5")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    AnimationClipAsset clip = makeTranslationClip(1.0f);
    const HE::UUID clipId   = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    // Pre-set time to 0.5; advance by 0 so it stays there
    AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.5f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);
    // At t=0.5 translation=(0.5,0,0), IBM=identity → translate(0.5,0,0)
    glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(0.5f));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(0.0f));
    CHECK(s.boneMatrices[0][3][2] == doctest::Approx(0.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationSystem: bone matrix evaluation (rotation channel)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationSystem slerps rotation channel (Y-axis 0 to 90 degrees)")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    // Clip: single rotation channel, joint 0
    // t=0 → identity quat (xyzw: 0 0 0 1)
    // t=1 → 90° around Y (glm xyzw: 0  sin(45°) 0  cos(45°))
    const float s45 = std::sin(glm::radians(45.0f));
    const float c45 = std::cos(glm::radians(45.0f));

    AnimationClipAsset clip;
    clip.duration = 1.0f;
    clip.name     = "rotY90";
    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Rotation;
    ch.times      = { 0.0f, 1.0f };
    ch.values     = {
        0.0f, 0.0f, 0.0f, 1.0f,   // identity (xyzw)
        0.0f, s45,  0.0f, c45,    // 90° Y (xyzw)
    };
    clip.channels.push_back(std::move(ch));
    const HE::UUID clipId = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.5f;
    world.addComponent(e, an);

    // Pre-set to t=0.5 and advance by 0 to keep it there
    AnimationSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);

    // At t=0.5, slerp identity→90°Y gives 45°Y rotation.
    // The [0][0] element of a Y-rotation matrix is cos(angle).
    const float cos45 = std::cos(glm::radians(45.0f));
    CHECK(s.boneMatrices[0][0][0] == doctest::Approx(cos45).epsilon(1e-4));
    // The diagonal [2][2] is also cos(45°) for a Y-rotation.
    CHECK(s.boneMatrices[0][2][2] == doctest::Approx(cos45).epsilon(1e-4));
    // Translation column should be zero (no translation channel, IBM=identity).
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(0.0f));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(0.0f));
    CHECK(s.boneMatrices[0][3][2] == doctest::Approx(0.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationSystem: two-joint parent-child skeleton
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationSystem applies FK across parent-child joints")
{
    ContentManager cm;
    HorizonWorld   world;

    // Skeleton: joint0 (root), joint1 (child of 0)
    // IBM of joint0 = identity, IBM of joint1 = identity
    const HE::UUID meshId = HE::UUID::generate();
    {
        SkeletalMeshAsset sma;
        sma.id   = meshId;
        sma.name = "twoJoint";
        SkeletonJoint root;
        root.name = "Root"; root.parent = -1;
        root.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        SkeletonJoint child;
        child.name = "Child"; child.parent = 0;
        child.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        sma.skeleton = { root, child };
        cm.registerSkeletalMesh(std::move(sma));
    }

    // Clip: translate joint0 by (1,0,0), translate joint1 by (0,1,0)
    AnimationClipAsset clip;
    clip.duration = 1.0f;
    clip.name     = "twoJointTest";
    {
        AnimationChannel ch0; ch0.jointIndex = 0; ch0.path = AnimPathType::Translation;
        ch0.times  = { 0.0f }; ch0.values = { 1.0f, 0.0f, 0.0f };
        AnimationChannel ch1; ch1.jointIndex = 1; ch1.path = AnimPathType::Translation;
        ch1.times  = { 0.0f }; ch1.values = { 0.0f, 1.0f, 0.0f };
        clip.channels = { ch0, ch1 };
    }
    const HE::UUID clipId = cm.registerAnimationClip(std::move(clip));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.0f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 2);

    // Joint0: world = translate(1,0,0), IBM = identity → bone[0] = translate(1,0,0)
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(1.0f));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(0.0f));

    // Joint1: world = translate(1,0,0) * translate(0,1,0) = translate(1,1,0)
    // IBM = identity → bone[1] = translate(1,1,0)
    CHECK(s.boneMatrices[1][3][0] == doctest::Approx(1.0f));
    CHECK(s.boneMatrices[1][3][1] == doctest::Approx(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimatorBlendComponent defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimatorBlendComponent defaults")
{
    AnimatorBlendComponent ab;
    CHECK(ab.clipAId       == HE::UUID{});
    CHECK(ab.clipBId       == HE::UUID{});
    CHECK(ab.blendAlpha    == doctest::Approx(0.0f));
    CHECK(ab.playbackTime  == doctest::Approx(0.0f));
    CHECK(ab.playbackSpeed == doctest::Approx(1.0f));
    CHECK(ab.looping       == true);
    CHECK(ab.playing       == true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationBlendSystem: integration tests
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a clip that keeps joint 0 at a constant translation
static AnimationClipAsset makeConstantClip(glm::vec3 pos, float duration = 1.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "const";
    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f };
    ch.values     = { pos.x, pos.y, pos.z };
    clip.channels.push_back(std::move(ch));
    return clip;
}

TEST_CASE("AnimationBlendSystem alpha=0 produces pure clip A result")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({2.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 4.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab; ab.clipAId = clipAId; ab.clipBId = clipBId; ab.blendAlpha = 0.0f;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("AnimationBlendSystem alpha=1 produces pure clip B result")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({2.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 4.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab; ab.clipAId = clipAId; ab.clipBId = clipBId; ab.blendAlpha = 1.0f;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(4.0f).epsilon(1e-4));
}

TEST_CASE("AnimationBlendSystem alpha=0.5 produces midpoint translation")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({2.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 4.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab; ab.clipAId = clipAId; ab.clipBId = clipBId; ab.blendAlpha = 0.5f;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.0f);
    const auto& s = world.registry().get<SkeletalMeshComponent>(e);
    REQUIRE(s.boneMatrices.size() == 1);
    // lerp(2,0, 0.5) = 1.0 on X; lerp(0,4, 0.5) = 2.0 on Y
    CHECK(s.boneMatrices[0][3][0] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(s.boneMatrices[0][3][1] == doctest::Approx(2.0f).epsilon(1e-4));
}

TEST_CASE("AnimationBlendSystem advances playbackTime")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({1.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 1.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab; ab.clipAId = clipAId; ab.clipBId = clipBId;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.4f);
    const auto& updatedAb = world.registry().get<AnimatorBlendComponent>(e);
    CHECK(updatedAb.playbackTime == doctest::Approx(0.4f));
}

TEST_CASE("AnimationBlendSystem skips when playing=false")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));

    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({1.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 1.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    AnimatorBlendComponent ab;
    ab.clipAId = clipAId; ab.clipBId = clipBId;
    ab.playing = false; ab.playbackTime = 0.3f;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.5f);
    const auto& updatedAb = world.registry().get<AnimatorBlendComponent>(e);
    CHECK(updatedAb.playbackTime == doctest::Approx(0.3f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimatorStateMachineComponent: default values
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AnimatorStateMachineComponent defaults")
{
    AnimatorStateMachineComponent sm;
    CHECK(sm.stateMachineAssetId == HE::UUID{});
    CHECK(sm.resolvedGraph.states.empty());
    CHECK(sm.resolvedGraph.transitions.empty());
    CHECK(sm.params.empty());
    CHECK(sm.currentStateName.empty());
    CHECK(sm.clipTime      == doctest::Approx(0.0f));
    CHECK(sm.playbackSpeed == doctest::Approx(1.0f));
    CHECK(!sm.inTransition);
    CHECK(sm.transitionElapsed  == doctest::Approx(0.0f));
    CHECK(sm.transitionDuration == doctest::Approx(0.2f));
    CHECK(sm.configDirty); // resolves to an empty graph on first update() until an asset is assigned
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationStateMachineSystem: integration helpers
// ─────────────────────────────────────────────────────────────────────────────
// Builds an AnimatorStateMachineAsset (Idle/Walk + one Idle->Walk transition
// gated on "speed") and registers it, returning a component that references it
// — the graph-authored equivalent of directly setting the old inline fields.
static AnimatorStateMachineComponent makeSimpleSM(
    ContentManager& cm, HE::UUID clipIdA, HE::UUID clipIdB,
    float transitionDuration = 0.2f)
{
    HE::AnimatorStateMachineGraph g;
    g.states.push_back({ 1, "Idle", clipIdA, HE::UUID{}, true, 0.0f, 0.0f });
    g.states.push_back({ 2, "Walk", clipIdB, HE::UUID{}, true, 0.0f, 0.0f });
    g.transitions.push_back({ "Idle", "Walk", "speed",
                               HE::TransitionOp::Greater, 0.5f, transitionDuration });
    g.startState = "Idle";

    AnimatorStateMachineAsset asset;
    asset.name      = "TestStateMachine";
    asset.graphJson = HE::animatorStateMachineToJson(g);
    const HE::UUID assetId = cm.registerAnimatorStateMachine(std::move(asset));

    AnimatorStateMachineComponent sm;
    sm.stateMachineAssetId = assetId;
    sm.params["speed"]     = 0.0f;
    sm.currentStateName    = "Idle";
    sm.playbackSpeed       = 1.0f;
    return sm;
}

TEST_CASE("AnimationStateMachineSystem plays current state clip")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({3.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 5.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    world.addComponent(e, makeSimpleSM(cm, clipAId, clipBId));

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& boneMatrices = world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
    REQUIRE(!boneMatrices.empty());
    // Bone 0 local translation should be X=3 (clip A / Idle state)
    CHECK(boneMatrices[0][3][0] == doctest::Approx(3.0f).epsilon(0.01f));
    CHECK(boneMatrices[0][3][1] == doctest::Approx(0.0f).epsilon(0.01f));
}

TEST_CASE("SceneSystems: animation is its own phase, so it can run after gameplay")
{
    // The split exists because a state machine reads what gameplay produced THIS
    // frame. The editor used to run the whole tick at the top of the frame —
    // before physics, before scripts — while the game ran it at the bottom, so
    // PIE animated a frame late and in the opposite order from the shipped game.
    //
    // What this pins down is the seam: the world phase must not pose skeletons,
    // and the animation phase must. Anything that folds them back together, or
    // quietly moves a driver from one to the other, fails here.
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({ 3.0f, 0.0f, 0.0f }));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({ 0.0f, 5.0f, 0.0f }));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    world.addComponent(e, makeSimpleSM(cm, clipAId, clipBId));

    // A fresh SkeletalMeshComponent already carries one IDENTITY matrix (the
    // bind-pose fallback), so "did anything pose this" is a question about the
    // values, not about the vector being empty.
    auto boneX = [&]{ return world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][0]; };
    REQUIRE(boneX() == doctest::Approx(0.0f));

    // The world phase leaves the skeleton alone.
    SceneSystems::tickWorld(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
    CHECK(boneX() == doctest::Approx(0.0f));

    // Whatever gameplay would have written, written here — this stands in for
    // the script or sync-graph that sets the parameter mid-frame.
    world.registry().get<AnimatorStateMachineComponent>(e).params["speed"] = 1.0f;

    // …and the animation phase picks it up in the SAME tick: the pose appears,
    // and the parameter set a moment ago has already started the transition.
    SceneSystems::tickAnimation(world, cm, 0.016f);
    const auto& sm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK((sm.inTransition || sm.currentStateName == "Walk"));
    CHECK(boneX() > 1.0f);   // Idle clip is X=3, barely blended toward Walk yet
}

TEST_CASE("EngineApi animator.* is what makes the state machine steerable")
{
    // Before these rows existed the FSM was authorable and unreachable: no
    // registry entry, no script binding, no node, no inspector field. A
    // parameter kept its asset default forever, so every authored transition sat
    // there waiting for a value nothing could deliver. This drives it end to end
    // through the same registry the four frontends dispatch.
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({ 3.0f, 0.0f, 0.0f }));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({ 0.0f, 5.0f, 0.0f }));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    world.addComponent(e, makeSimpleSM(cm, clipAId, clipBId, /*transitionDuration=*/0.0f));

    HE::api::Ctx ctx{ &world, nullptr, &cm };
    const auto eid = (HE::api::Entity)e;

    auto call = [&](const char* id, std::vector<HorizonCode::Value> a)
    { return HE::api::find(id)->invoke(ctx, a); };
    using V = HorizonCode::Value;

    // Reading an unset parameter is 0, not an error.
    CHECK(call("animator.getParam", { V::ofInt((int)eid), V::ofString("nope") })[0].f
          == doctest::Approx(0.0f));
    CHECK(call("animator.getState", { V::ofInt((int)eid) })[0].s == "Idle");

    // Set it the way gameplay code would, then let the animation phase run.
    call("animator.setParam", { V::ofInt((int)eid), V::ofString("speed"), V::ofFloat(1.0f) });
    CHECK(call("animator.getParam", { V::ofInt((int)eid), V::ofString("speed") })[0].f
          == doctest::Approx(1.0f));

    SceneSystems::tickAnimation(world, cm, 0.016f);
    // A zero-length transition lands immediately, so the state is observable
    // through the same API in the very next call.
    CHECK(call("animator.getState", { V::ofInt((int)eid) })[0].s == "Walk");

    // An entity without a state machine answers instead of crashing.
    entt::entity bare = world.createEntity();
    world.addComponent(bare, TransformComponent{});
    const auto bid = (int)(HE::api::Entity)bare;
    CHECK_NOTHROW(call("animator.setParam", { V::ofInt(bid), V::ofString("x"), V::ofFloat(1.0f) }));
    CHECK(call("animator.getParam", { V::ofInt(bid), V::ofString("x") })[0].f == doctest::Approx(0.0f));
    CHECK(call("animator.getState", { V::ofInt(bid) })[0].s == "");
}

TEST_CASE("AnimationStateMachineSystem transition fires when param exceeds threshold")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({3.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 5.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    auto sm = makeSimpleSM(cm, clipAId, clipBId, 0.4f);
    sm.params["speed"] = 1.0f; // exceeds 0.5 threshold → transition should fire
    world.addComponent(e, sm);

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& updatedSm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK(updatedSm.inTransition);
    CHECK(updatedSm.transitionTarget == "Walk");
}

TEST_CASE("AnimationStateMachineSystem does NOT transition when param below threshold")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({3.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 5.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    auto sm = makeSimpleSM(cm, clipAId, clipBId);
    sm.params["speed"] = 0.2f; // below threshold
    world.addComponent(e, sm);

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& updatedSm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK(!updatedSm.inTransition);
    CHECK(updatedSm.currentStateName == "Idle");
}

TEST_CASE("AnimationStateMachineSystem crossfade midpoint blends 50/50")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({2.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 4.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    auto sm = makeSimpleSM(cm, clipAId, clipBId, 0.4f);
    sm.params["speed"]     = 1.0f;
    sm.inTransition        = true;
    sm.transitionTarget    = "Walk";
    sm.transitionElapsed   = 0.2f; // halfway through 0.4s crossfade → alpha = 0.5
    sm.transitionDuration  = 0.4f;
    world.addComponent(e, sm);

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& boneMatrices = world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
    REQUIRE(!boneMatrices.empty());
    CHECK(boneMatrices[0][3][0] == doctest::Approx(1.0f).epsilon(0.02f)); // X: 2*0.5
    CHECK(boneMatrices[0][3][1] == doctest::Approx(2.0f).epsilon(0.02f)); // Y: 4*0.5
}

TEST_CASE("AnimationStateMachineSystem completes transition after full duration")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({2.0f, 0.0f, 0.0f}));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 4.0f, 0.0f}));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    auto sm = makeSimpleSM(cm, clipAId, clipBId, 0.2f);
    sm.params["speed"]    = 1.0f;
    sm.inTransition       = true;
    sm.transitionTarget   = "Walk";
    sm.transitionElapsed  = 0.19f;
    sm.transitionDuration = 0.2f;
    world.addComponent(e, sm);

    // One tick of dt=0.05 → transitionElapsed becomes 0.24 >= 0.2 → complete
    AnimationStateMachineSystem::update(world, cm, 0.05f);

    const auto& updatedSm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK(!updatedSm.inTransition);
    CHECK(updatedSm.currentStateName == "Walk");
}

TEST_CASE("AnimationStateMachineSystem advances clipTime")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({1.0f, 0.0f, 0.0f}, 2.0f));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({0.0f, 1.0f, 0.0f}, 2.0f));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    world.addComponent(e, makeSimpleSM(cm, clipAId, clipBId));

    AnimationStateMachineSystem::update(world, cm, 0.3f);

    const auto& updatedSm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK(updatedSm.clipTime == doctest::Approx(0.3f));
}

TEST_CASE("AnimationStateMachineSystem migrates a legacy inline-graph entity into a real asset")
{
    ContentManager cm;
    HorizonWorld   world;
    auto& reg = world.registry();

    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({3.0f, 0.0f, 0.0f}));

    auto e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);

    // Simulate what SceneSerializer::load does for a pre-asset-format scene: no
    // stateMachineAssetId, legacy graph staged instead (see AnimatorStateMachine-
    // Component::LegacyConfig).
    AnimatorStateMachineComponent sm;
    sm.legacy.hasData = true;
    sm.legacy.states.push_back({ 1, "Idle", clipAId, HE::UUID{}, true, 5.0f, 5.0f });
    sm.legacy.currentStateName = "Idle";
    reg.emplace<AnimatorStateMachineComponent>(e, sm);

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& updatedSm = reg.get<AnimatorStateMachineComponent>(e);
    CHECK(!updatedSm.legacy.hasData);                       // migrated exactly once
    CHECK(updatedSm.stateMachineAssetId != HE::UUID{});      // now points at a real asset
    REQUIRE(cm.getAnimatorStateMachine(updatedSm.stateMachineAssetId) != nullptr);
    REQUIRE(updatedSm.resolvedGraph.states.size() == 1);
    CHECK(updatedSm.resolvedGraph.states[0].name == "Idle");
    CHECK(updatedSm.resolvedGraph.states[0].x == doctest::Approx(5.0f));
    CHECK(updatedSm.currentStateName == "Idle");

    // The migrated asset actually drives playback (not just a dangling reference).
    const auto& boneMatrices = reg.get<SkeletalMeshComponent>(e).boneMatrices;
    REQUIRE(!boneMatrices.empty());
    CHECK(boneMatrices[0][3][0] == doctest::Approx(3.0f).epsilon(0.01f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  PropertyAnimClipAsset + PropertyAnimationSystem
// ─────────────────────────────────────────────────────────────────────────────
#include <HorizonScene/Components/PropertyAnimatorComponent.h>
#include <HorizonScene/PropertyAnimationSystem.h>

static PropertyAnimClipAsset makePropClip(PropTarget target,
                                           float fromV, float toV,
                                           float duration = 1.0f)
{
    PropertyAnimClipAsset clip;
    clip.duration = duration;
    clip.name     = "propClip";
    PropertyAnimChannel ch;
    ch.target = target;
    ch.times  = { 0.0f, duration };
    ch.values = { fromV, toV };
    clip.channels.push_back(std::move(ch));
    return clip;
}

TEST_CASE("PropertyAnimatorComponent defaults")
{
    PropertyAnimatorComponent pa;
    CHECK(pa.clipId       == HE::UUID{});
    CHECK(pa.playbackTime  == doctest::Approx(0.0f));
    CHECK(pa.playbackSpeed == doctest::Approx(1.0f));
    CHECK(pa.looping       == true);
    CHECK(pa.playing       == true);
}

TEST_CASE("PropertyAnimClipAsset registers and retrieves from ContentManager")
{
    ContentManager cm;
    PropertyAnimClipAsset clip;
    clip.duration = 2.0f;
    clip.name     = "TestPropClip";

    const HE::UUID id = cm.registerPropertyAnimClip(std::move(clip));
    REQUIRE(id != HE::UUID{});
    const PropertyAnimClipAsset* got = cm.getPropertyAnimClip(id);
    REQUIRE(got != nullptr);
    CHECK(got->duration == doctest::Approx(2.0f));
    CHECK(got->name == "TestPropClip");
}

TEST_CASE("PropertyAnimationSystem animates TransformComponent PosX")
{
    ContentManager cm;
    HorizonWorld   world;

    // Clip: PosX from 0 to 5 over 1 second
    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::PosX, 0.0f, 5.0f, 1.0f));

    entt::entity e = world.createEntity();
    TransformComponent tc; tc.position = {}; tc.rotation = {}; tc.scale = glm::vec3(1.0f);
    world.addComponent(e, tc);
    PropertyAnimatorComponent pa;
    pa.clipId = clipId; pa.playbackTime = 0.5f; // midpoint → PosX should be 2.5
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.0f);

    const auto& updTc = world.registry().get<TransformComponent>(e);
    CHECK(updTc.position.x == doctest::Approx(2.5f).epsilon(0.01f));
}

TEST_CASE("PropertyAnimationSystem animates TransformComponent RotY")
{
    ContentManager cm;
    HorizonWorld   world;

    // Clip: RotY from 0 to 90 over 1 second
    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::RotY, 0.0f, 90.0f, 1.0f));

    entt::entity e = world.createEntity();
    TransformComponent tc; tc.position = {}; tc.rotation = {}; tc.scale = glm::vec3(1.0f);
    world.addComponent(e, tc);
    PropertyAnimatorComponent pa;
    pa.clipId = clipId; pa.playbackTime = 1.0f; pa.looping = false; // end → RotY = 90
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.0f);

    const auto& updTc = world.registry().get<TransformComponent>(e);
    CHECK(updTc.rotation.y == doctest::Approx(90.0f).epsilon(0.01f));
}

TEST_CASE("PropertyAnimationSystem advances playbackTime")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::PosX, 0.0f, 1.0f, 2.0f));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    PropertyAnimatorComponent pa; pa.clipId = clipId;
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.7f);

    const auto& updPa = world.registry().get<PropertyAnimatorComponent>(e);
    CHECK(updPa.playbackTime == doctest::Approx(0.7f));
}

TEST_CASE("PropertyAnimationSystem loops playback")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::PosX, 0.0f, 1.0f, 1.0f));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    PropertyAnimatorComponent pa;
    pa.clipId = clipId; pa.playbackTime = 0.8f; pa.looping = true;
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.5f); // 0.8 + 0.5 = 1.3 → fmod 1.0 = 0.3

    const auto& updPa = world.registry().get<PropertyAnimatorComponent>(e);
    CHECK(updPa.playbackTime == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(updPa.playing      == true);
}

TEST_CASE("PropertyAnimationSystem stops at end when not looping")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::PosX, 0.0f, 1.0f, 1.0f));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    PropertyAnimatorComponent pa;
    pa.clipId = clipId; pa.playbackTime = 0.9f; pa.looping = false;
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.5f); // over end

    const auto& updPa = world.registry().get<PropertyAnimatorComponent>(e);
    CHECK(updPa.playbackTime == doctest::Approx(1.0f));
    CHECK(updPa.playing      == false);
}

TEST_CASE("PropertyAnimationSystem skips when playing=false")
{
    ContentManager cm;
    HorizonWorld   world;

    const HE::UUID clipId = cm.registerPropertyAnimClip(
        makePropClip(PropTarget::PosX, 0.0f, 1.0f, 1.0f));

    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    PropertyAnimatorComponent pa;
    pa.clipId = clipId; pa.playbackTime = 0.4f; pa.playing = false;
    world.addComponent(e, pa);

    PropertyAnimationSystem::update(world, cm, 0.5f);

    const auto& updPa = world.registry().get<PropertyAnimatorComponent>(e);
    CHECK(updPa.playbackTime == doctest::Approx(0.4f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationPreview::evaluateClipPose (Skeletal Mesh Editor clip-scrub preview)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("AnimationPreview::evaluateClipPose matches the ECS-driven pose at the same time")
{
    // Same fixtures as the ECS-driven AnimationSystem tests above, but evaluated
    // directly against the assets — this is exactly what the Skeletal Mesh
    // Editor's clip-scrub preview does (no entity involved).
    const SkeletalMeshAsset  mesh = makeOneBoneSkeletalMesh(HE::UUID::generate());
    const AnimationClipAsset clip = makeTranslationClip(1.0f);

    std::vector<glm::mat4> bones;
    AnimationPreview::evaluateClipPose(mesh, clip, 0.5f, bones);

    REQUIRE(bones.size() == 1);
    // makeTranslationClip goes from (0,0,0) at t=0 to (1,0,0) at t=duration,
    // identity IBM → the bone matrix IS the joint's world translation at t.
    CHECK(bones[0][3][0] == doctest::Approx(0.5f));
    CHECK(bones[0][3][1] == doctest::Approx(0.0f));
    CHECK(bones[0][3][2] == doctest::Approx(0.0f));
}

TEST_CASE("AnimationPreview::evaluateClipPose at t=0 is the bind pose (identity, identity IBM)")
{
    const SkeletalMeshAsset  mesh = makeOneBoneSkeletalMesh(HE::UUID::generate());
    const AnimationClipAsset clip = makeTranslationClip(1.0f);

    std::vector<glm::mat4> bones;
    AnimationPreview::evaluateClipPose(mesh, clip, 0.0f, bones);

    REQUIRE(bones.size() == 1);
    CHECK(bones[0] == glm::mat4(1.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Audit SCN-101/102/103: keyframe search, reused pose buffers, the pose gate
// ─────────────────────────────────────────────────────────────────────────────

#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <algorithm>
#include <string>

namespace
{
// The linear walk the sampler used before SCN-103, kept as the reference.
size_t findBracketLinear(const std::vector<float>& times, float t)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < times.size(); ++i)
    {
        if (times[i + 1] >= t) { k = i; return k; }
        k = i + 1;
    }
    return k;
}

// What applyChannel computes for a translation channel, on the reference bracket.
glm::vec3 referenceTranslation(const AnimationChannel& ch, float t)
{
    if (ch.times.empty()) return glm::vec3(0.0f);
    const size_t k = findBracketLinear(ch.times, t);
    float alpha = 0.0f;
    if (k + 1 < ch.times.size())
    {
        const float span = ch.times[k + 1] - ch.times[k];
        if (span > 0.0f) alpha = std::clamp((t - ch.times[k]) / span, 0.0f, 1.0f);
    }
    const glm::vec3 v0(ch.values[k * 3], ch.values[k * 3 + 1], ch.values[k * 3 + 2]);
    const glm::vec3 v1 = (k + 1 < ch.times.size())
        ? glm::vec3(ch.values[(k + 1) * 3], ch.values[(k + 1) * 3 + 1], ch.values[(k + 1) * 3 + 2])
        : v0;
    return glm::mix(v0, v1, alpha);
}

// A chain of `joints` joints, each the child of the one before, identity IBMs.
SkeletalMeshAsset makeChainSkeletalMesh(const HE::UUID& meshId, size_t joints)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "chain";
    for (size_t i = 0; i < joints; ++i)
    {
        SkeletonJoint j;
        j.name   = "J" + std::to_string(i);
        j.parent = static_cast<int32_t>(i) - 1;
        j.inverseBindMatrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        sma.skeleton.push_back(j);
    }
    return sma;
}

// One constant translation key on ONE joint; every other joint keeps its default.
AnimationClipAsset makeJointClip(uint32_t joint, glm::vec3 pos)
{
    AnimationClipAsset clip = makeConstantClip(pos);
    clip.channels[0].jointIndex = joint;
    return clip;
}

const glm::mat4 kSentinel(7.0f);

entt::entity addSkinned(HorizonWorld& world, const HE::UUID& meshId)
{
    entt::entity e = world.createEntity();
    world.addComponent(e, TransformComponent{ .position = {}, .rotation = {}, .scale = glm::vec3(1.0f) });
    SkeletalMeshComponent smc; smc.meshAssetId = meshId;
    world.addComponent(e, smc);
    return e;
}

void plantSentinel(HorizonWorld& world, entt::entity e)
{
    auto& smc = world.registry().get<SkeletalMeshComponent>(e);
    for (auto& m : smc.boneMatrices) m = kSentinel;
    smc.dirty = false;
}

bool sentinelSurvived(HorizonWorld& world, entt::entity e)
{
    const auto& smc = world.registry().get<SkeletalMeshComponent>(e);
    return !smc.dirty && !smc.boneMatrices.empty() && smc.boneMatrices[0] == kSentinel;
}

size_t countFires(const HE::NotifyQueue& q, const char* name)
{
    return static_cast<size_t>(std::count_if(q.begin(), q.end(),
        [&](const HE::AnimationNotifyEvent& ev) { return ev.name == name; }));
}
} // namespace

TEST_CASE("SCN-103: the binary keyframe search samples exactly what the linear walk did")
{
    const SkeletalMeshAsset mesh = makeOneBoneSkeletalMesh(HE::UUID::generate());

    // A fixed LCG: reproducible, and no <random> distribution whose output
    // differs between standard libraries.
    uint32_t seed = 0x2545F491u;
    auto next = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };

    // Both sides of the walk/binary-search cutoff (64 keys, AnimationEval.cpp).
    std::vector<size_t> sizes;
    for (size_t n = 0; n <= 40; ++n) sizes.push_back(n);
    for (size_t n : { 63u, 64u, 65u, 100u, 300u }) sizes.push_back(n);

    for (size_t n : sizes)
    {
        AnimationClipAsset clip;
        clip.duration = 1.0f;
        AnimationChannel ch;
        ch.jointIndex = 0;
        ch.path       = AnimPathType::Translation;
        float t = 0.0f;
        for (size_t k = 0; k < n; ++k)
        {
            // Every fourth step repeats the previous key's time: duplicates are
            // where "the FIRST key at or after t" actually decides something.
            if (k > 0 && next() % 4 != 0) t += 0.01f + static_cast<float>(next() % 100) / 1000.0f;
            ch.times.push_back(t);
            ch.values.insert(ch.values.end(), { static_cast<float>(k + 1),
                                                static_cast<float>(k * k),
                                                -static_cast<float>(k) });
        }
        clip.channels.push_back(ch);

        std::vector<float> probes = { -1.0f, 0.0f, t, t + 1.0f, std::nanf("") };
        for (float key : ch.times) { probes.push_back(key); probes.push_back(key + 0.0005f); }
        for (int i = 0; i < 64; ++i) probes.push_back(static_cast<float>(next() % 1000) / 1000.0f * (t + 0.2f) - 0.1f);

        for (float p : probes)
        {
            CAPTURE(n);
            CAPTURE(p);
            std::vector<glm::mat4> bones;
            AnimationPreview::evaluateClipPose(mesh, clip, p, bones);
            REQUIRE(bones.size() == 1);
            const glm::vec3 want = referenceTranslation(ch, p);
            // Neighbouring keys differ by at least 1 in some component, so a
            // wrong bracket is far outside this; the tolerance only absorbs FMA
            // contraction differing between this file and the engine's.
            CHECK(bones[0][3][0] == doctest::Approx(want.x).epsilon(1e-5));
            CHECK(bones[0][3][1] == doctest::Approx(want.y).epsilon(1e-5));
            CHECK(bones[0][3][2] == doctest::Approx(want.z).epsilon(1e-5));
        }
    }
}

TEST_CASE("SCN-101: reused pose buffers start every entity from the defaults, whatever its size")
{
    // Two skeletons of different sizes whose clips key DIFFERENT joints. A scratch
    // buffer that kept the previous entity's values would leave that joint in the
    // second pose; which entity goes first is up to entt, so each one has a joint
    // only the other one keys.
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID bigMesh   = HE::UUID::generate();
    const HE::UUID smallMesh = HE::UUID::generate();
    cm.registerSkeletalMesh(makeChainSkeletalMesh(bigMesh, 4));
    cm.registerSkeletalMesh(makeChainSkeletalMesh(smallMesh, 2));
    const HE::UUID clipJoint0 = cm.registerAnimationClip(makeJointClip(0, { 5.0f, 0.0f, 0.0f }));
    const HE::UUID clipJoint1 = cm.registerAnimationClip(makeJointClip(1, { 0.0f, 3.0f, 0.0f }));

    auto expectPose = [&](entt::entity e, const HE::UUID& meshId, const HE::UUID& clipId)
    {
        std::vector<glm::mat4> want;
        AnimationPreview::evaluateClipPose(*cm.getSkeletalMesh(meshId), *cm.getAnimationClip(clipId),
                                           0.25f, want);
        const auto& got = world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
        REQUIRE(got.size() == want.size());
        for (size_t i = 0; i < got.size(); ++i) CHECK(got[i] == want[i]);
    };

    SUBCASE("clip driver")
    {
        const entt::entity a = addSkinned(world, bigMesh);
        const entt::entity b = addSkinned(world, smallMesh);
        AnimatorComponent anA; anA.clipAssetId = clipJoint0; anA.playbackTime = 0.25f;
        AnimatorComponent anB; anB.clipAssetId = clipJoint1; anB.playbackTime = 0.25f;
        world.addComponent(a, anA);
        world.addComponent(b, anB);
        for (int frame = 0; frame < 2; ++frame)
        {
            AnimationSystem::update(world, cm, 0.0f);
            expectPose(a, bigMesh, clipJoint0);
            expectPose(b, smallMesh, clipJoint1);
        }
    }
    SUBCASE("state machine")
    {
        const entt::entity a = addSkinned(world, bigMesh);
        const entt::entity b = addSkinned(world, smallMesh);
        auto smA = makeSimpleSM(cm, clipJoint0, clipJoint1); smA.clipTime = 0.25f;
        auto smB = makeSimpleSM(cm, clipJoint1, clipJoint0); smB.clipTime = 0.25f;
        world.addComponent(a, smA);
        world.addComponent(b, smB);
        AnimationStateMachineSystem::update(world, cm, 0.0f);
        expectPose(a, bigMesh, clipJoint0);
        expectPose(b, smallMesh, clipJoint1);
    }
}

TEST_CASE("SCN-102: a clip animator whose playhead stands still keeps its bones")
{
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipId = cm.registerAnimationClip(makeTranslationClip(1.0f));

    const entt::entity e = addSkinned(world, meshId);
    AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.5f;
    world.addComponent(e, an);

    AnimationSystem::update(world, cm, 0.0f);   // the first pose is always computed
    CHECK(world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][0] == doctest::Approx(0.5f));

    // Same clip, same time: nothing to do, and nothing is marked dirty.
    plantSentinel(world, e);
    AnimationSystem::update(world, cm, 0.0f);
    CHECK(sentinelSurvived(world, e));

    // playbackSpeed 0 is the same standstill with a real dt.
    world.registry().get<AnimatorComponent>(e).playbackSpeed = 0.0f;
    AnimationSystem::update(world, cm, 0.016f);
    CHECK(sentinelSurvived(world, e));

    SUBCASE("scrubbing the playhead poses again")
    {
        world.registry().get<AnimatorComponent>(e).playbackTime = 0.25f;
        AnimationSystem::update(world, cm, 0.0f);
        CHECK(world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][0] == doctest::Approx(0.25f));
    }
    SUBCASE("a moving playhead poses every frame")
    {
        world.registry().get<AnimatorComponent>(e).playbackSpeed = 1.0f;
        AnimationSystem::update(world, cm, 0.1f);
        CHECK(world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][0] == doctest::Approx(0.6f));
    }
    SUBCASE("another clip poses again")
    {
        const HE::UUID other = cm.registerAnimationClip(makeConstantClip({ 0.0f, 2.0f, 0.0f }));
        world.registry().get<AnimatorComponent>(e).clipAssetId = other;
        AnimationSystem::update(world, cm, 0.0f);
        CHECK(world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][1] == doctest::Approx(2.0f));
    }
    SUBCASE("root motion turns the gate off, and its removal does not leave a stale key")
    {
        world.registry().emplace<RootMotionComponent>(e);
        AnimationSystem::update(world, cm, 0.0f);
        CHECK_FALSE(sentinelSurvived(world, e));
        plantSentinel(world, e);
        AnimationSystem::update(world, cm, 0.0f);
        CHECK_FALSE(sentinelSurvived(world, e));

        world.registry().remove<RootMotionComponent>(e);
        plantSentinel(world, e);
        AnimationSystem::update(world, cm, 0.0f);   // posed: the last key was 0
        CHECK_FALSE(sentinelSurvived(world, e));
        plantSentinel(world, e);
        AnimationSystem::update(world, cm, 0.0f);   // and now kept again
        CHECK(sentinelSurvived(world, e));
    }
    SUBCASE("a cleared key (another writer of the bones) poses again")
    {
        world.registry().get<SkeletalMeshComponent>(e).poseKey = 0;
        AnimationSystem::update(world, cm, 0.0f);
        CHECK_FALSE(sentinelSurvived(world, e));
    }
}

TEST_CASE("SCN-102: a blend animator keeps its bones only while time AND alpha stand still")
{
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({ 2.0f, 0.0f, 0.0f }));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({ 0.0f, 4.0f, 0.0f }));

    const entt::entity e = addSkinned(world, meshId);
    AnimatorBlendComponent ab; ab.clipAId = clipAId; ab.clipBId = clipBId; ab.blendAlpha = 0.0f;
    world.addComponent(e, ab);

    AnimationBlendSystem::update(world, cm, 0.0f);
    plantSentinel(world, e);
    AnimationBlendSystem::update(world, cm, 0.0f);
    CHECK(sentinelSurvived(world, e));

    // A script moving the weight on a paused timeline is a new pose.
    world.registry().get<AnimatorBlendComponent>(e).blendAlpha = 1.0f;
    AnimationBlendSystem::update(world, cm, 0.0f);
    const auto& bones = world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
    CHECK(bones[0][3][0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(bones[0][3][1] == doctest::Approx(4.0f).epsilon(1e-4));
}

TEST_CASE("SCN-102: a resting state machine keeps its bones; a transition poses again")
{
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId  = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    const HE::UUID clipAId = cm.registerAnimationClip(makeConstantClip({ 3.0f, 0.0f, 0.0f }));
    const HE::UUID clipBId = cm.registerAnimationClip(makeConstantClip({ 0.0f, 5.0f, 0.0f }));

    const entt::entity e = addSkinned(world, meshId);
    world.addComponent(e, makeSimpleSM(cm, clipAId, clipBId, 0.2f));

    AnimationStateMachineSystem::update(world, cm, 0.0f);
    plantSentinel(world, e);
    AnimationStateMachineSystem::update(world, cm, 0.0f);
    CHECK(sentinelSurvived(world, e));

    // The parameter fires Idle -> Walk: every crossfade frame is posed.
    world.registry().get<AnimatorStateMachineComponent>(e).params["speed"] = 1.0f;
    AnimationStateMachineSystem::update(world, cm, 0.0f);
    CHECK(world.registry().get<AnimatorStateMachineComponent>(e).inTransition);
    CHECK_FALSE(sentinelSurvived(world, e));
    plantSentinel(world, e);
    AnimationStateMachineSystem::update(world, cm, 0.0f);
    CHECK_FALSE(sentinelSurvived(world, e));

    // Through the crossfade; then at rest in Walk, and kept again.
    AnimationStateMachineSystem::update(world, cm, 0.25f);
    CHECK(world.registry().get<AnimatorStateMachineComponent>(e).currentStateName == "Walk");
    world.registry().get<AnimatorStateMachineComponent>(e).playbackSpeed = 0.0f;
    AnimationStateMachineSystem::update(world, cm, 0.016f);
    CHECK(world.registry().get<SkeletalMeshComponent>(e).boneMatrices[0][3][1] == doctest::Approx(5.0f).epsilon(1e-4));
    plantSentinel(world, e);
    AnimationStateMachineSystem::update(world, cm, 0.016f);
    CHECK(sentinelSurvived(world, e));
}

TEST_CASE("SCN-102: a skipped pose still fires the notifies its playhead swept over")
{
    // A looping 1 s clip advanced by exactly one lap lands on the same playhead:
    // the pose is the same, so the gate skips it, but the lap crossed every
    // notify on the clip and each one has to fire, once per lap.
    ContentManager cm;
    HorizonWorld   world;
    const HE::UUID meshId = HE::UUID::generate();
    cm.registerSkeletalMesh(makeOneBoneSkeletalMesh(meshId));
    AnimationClipAsset clip = makeTranslationClip(1.0f);
    clip.notifies.push_back({ "Step", 0.5f, 0.0f });
    const HE::UUID clipId = cm.registerAnimationClip(std::move(clip));
    const HE::UUID idleId = cm.registerAnimationClip(makeConstantClip({ 0.0f, 0.0f, 0.0f }));

    SUBCASE("state machine")
    {
        const entt::entity e = addSkinned(world, meshId);
        auto sm = makeSimpleSM(cm, clipId, idleId);
        sm.clipTime = 0.25f;
        world.addComponent(e, sm);
        for (int lap = 0; lap < 3; ++lap)
        {
            HE::NotifyQueue q;
            if (lap > 0) plantSentinel(world, e);
            AnimationStateMachineSystem::update(world, cm, 1.0f, nullptr, nullptr, &q);
            CHECK(world.registry().get<AnimatorStateMachineComponent>(e).clipTime == 0.25f);
            if (lap > 0) CHECK(sentinelSurvived(world, e));
            CHECK(countFires(q, "Step") == 1);
        }
    }
    SUBCASE("clip driver")
    {
        const entt::entity e = addSkinned(world, meshId);
        AnimatorComponent an; an.clipAssetId = clipId; an.playbackTime = 0.25f;
        world.addComponent(e, an);
        for (int lap = 0; lap < 3; ++lap)
        {
            HE::NotifyQueue q;
            if (lap > 0) plantSentinel(world, e);
            AnimationSystem::update(world, cm, 1.0f, nullptr, &q);
            CHECK(world.registry().get<AnimatorComponent>(e).playbackTime == 0.25f);
            if (lap > 0) CHECK(sentinelSurvived(world, e));
            CHECK(countFires(q, "Step") == 1);
        }
    }
}
