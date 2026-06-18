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
    CHECK(sm.states.empty());
    CHECK(sm.transitions.empty());
    CHECK(sm.params.empty());
    CHECK(sm.currentStateName.empty());
    CHECK(sm.clipTime      == doctest::Approx(0.0f));
    CHECK(sm.playbackSpeed == doctest::Approx(1.0f));
    CHECK(!sm.inTransition);
    CHECK(sm.transitionElapsed  == doctest::Approx(0.0f));
    CHECK(sm.transitionDuration == doctest::Approx(0.2f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnimationStateMachineSystem: integration helpers
// ─────────────────────────────────────────────────────────────────────────────
static AnimatorStateMachineComponent makeSimpleSM(
    HE::UUID clipIdA, HE::UUID clipIdB,
    float transitionDuration = 0.2f)
{
    AnimatorStateMachineComponent sm;
    sm.states.push_back({"Idle",  clipIdA, true});
    sm.states.push_back({"Walk",  clipIdB, true});
    sm.transitions.push_back({"Idle", "Walk", "speed",
                               TransitionOp::Greater, 0.5f, transitionDuration});
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
    world.addComponent(e, makeSimpleSM(clipAId, clipBId));

    AnimationStateMachineSystem::update(world, cm, 0.0f);

    const auto& boneMatrices = world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
    REQUIRE(!boneMatrices.empty());
    // Bone 0 local translation should be X=3 (clip A / Idle state)
    CHECK(boneMatrices[0][3][0] == doctest::Approx(3.0f).epsilon(0.01f));
    CHECK(boneMatrices[0][3][1] == doctest::Approx(0.0f).epsilon(0.01f));
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
    auto sm = makeSimpleSM(clipAId, clipBId, 0.4f);
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
    auto sm = makeSimpleSM(clipAId, clipBId);
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
    auto sm = makeSimpleSM(clipAId, clipBId, 0.4f);
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
    auto sm = makeSimpleSM(clipAId, clipBId, 0.2f);
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
    world.addComponent(e, makeSimpleSM(clipAId, clipBId));

    AnimationStateMachineSystem::update(world, cm, 0.3f);

    const auto& updatedSm = world.registry().get<AnimatorStateMachineComponent>(e);
    CHECK(updatedSm.clipTime == doctest::Approx(0.3f));
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
