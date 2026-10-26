#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  SceneSystems::tickAnimation cost per skinned character (Thema 238, Schritt 5).
//
//  The CPU audits had no animated scene, so the per-frame cost of the animation
//  stage was never measured. This bench builds N characters on a humanoid-sized
//  skeleton (65 joints, every joint with rotation keys, the root with translation)
//  and times the tick. The timing only runs with HE_ANIM_BENCH=1 (it would make
//  the suite slow and its numbers mean nothing on a loaded CI box); without it the
//  case checks that the rig animates, so the bench cannot silently rot.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

constexpr std::array<float, 16> kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
constexpr int kJoints = 65;   // a Mixamo-style humanoid
constexpr int kKeys   = 31;   // one second at 30 Hz

SkeletalMeshAsset makeHumanoid(const HE::UUID& meshId)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "benchHumanoid";
    for (int i = 0; i < kJoints; ++i)
    {
        SkeletonJoint j;
        j.name   = "J" + std::to_string(i);
        // A chain of short limbs off the spine: depth about 6, like a real rig.
        j.parent = i == 0 ? -1 : (i < 6 ? i - 1 : 1 + (i % 5));
        j.inverseBindMatrix = kIdentity;
        sma.skeleton.push_back(j);
    }
    return sma;
}

AnimationClipAsset makeFullBodyClip()
{
    AnimationClipAsset clip;
    clip.duration = 1.0f;
    clip.name     = "benchWalk";
    for (int j = 0; j < kJoints; ++j)
    {
        AnimationChannel ch;
        ch.jointIndex = static_cast<uint32_t>(j);
        ch.path       = AnimPathType::Rotation;
        for (int k = 0; k < kKeys; ++k)
        {
            const float t = static_cast<float>(k) / (kKeys - 1);
            const glm::quat q = glm::angleAxis(0.4f * std::sin(6.2831853f * t + j), glm::vec3(1, 0, 0));
            ch.times.push_back(t);
            ch.values.insert(ch.values.end(), { q.x, q.y, q.z, q.w });
        }
        clip.channels.push_back(std::move(ch));
    }
    AnimationChannel root;
    root.jointIndex = 0;
    root.path       = AnimPathType::Translation;
    for (int k = 0; k < kKeys; ++k)
    {
        const float t = static_cast<float>(k) / (kKeys - 1);
        root.times.push_back(t);
        root.values.insert(root.values.end(), { 0.0f, 0.05f * std::sin(12.566f * t), 0.0f });
    }
    clip.channels.push_back(std::move(root));
    return clip;
}

struct Crowd
{
    ContentManager cm;
    HorizonWorld   world;
    std::vector<entt::entity> entities;
};

void populate(Crowd& c, int count)
{
    const HE::UUID meshId = HE::UUID::generate();
    c.cm.registerSkeletalMesh(makeHumanoid(meshId));
    const HE::UUID clipId = c.cm.registerAnimationClip(makeFullBodyClip());
    for (int i = 0; i < count; ++i)
    {
        const entt::entity e = c.world.createEntity("Character");
        c.world.addComponent(e, TransformComponent{});
        SkeletalMeshComponent smc; smc.meshAssetId = meshId;
        c.world.addComponent(e, smc);
        AnimatorComponent an; an.clipAssetId = clipId; an.looping = true;
        an.playbackTime = 0.013f * static_cast<float>(i);   // characters out of phase
        c.world.addComponent(e, an);
        c.entities.push_back(e);
    }
}

bool benchEnabled()
{
    const char* v = std::getenv("HE_ANIM_BENCH");
    return v && *v && *v != '0';
}

} // namespace

TEST_CASE("animation bench: a full-body clip moves every joint of every character")
{
    Crowd c;
    populate(c, 4);
    SceneSystems::tickAnimation(c.world, c.cm, 1.0f / 60.0f);
    for (entt::entity e : c.entities)
    {
        const auto& bones = c.world.registry().get<SkeletalMeshComponent>(e).boneMatrices;
        REQUIRE(bones.size() == static_cast<size_t>(kJoints));
        CHECK(bones[kJoints - 1] != glm::mat4(1.0f));
    }
}

TEST_CASE("animation bench: tickAnimation per frame over the crowd size (HE_ANIM_BENCH=1)")
{
    if (!benchEnabled()) return;
    for (int count : { 1, 50, 200, 500, 1000 })
    {
        Crowd c;
        populate(c, count);
        for (int i = 0; i < 30; ++i) SceneSystems::tickAnimation(c.world, c.cm, 1.0f / 60.0f);   // warm caches
        std::vector<double> ms;
        for (int i = 0; i < 240; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            SceneSystems::tickAnimation(c.world, c.cm, 1.0f / 60.0f);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        const double p50 = ms[ms.size() / 2], p95 = ms[ms.size() * 95 / 100];
        std::printf("anim-bench characters=%d joints=%d p50=%.3f ms p95=%.3f ms per-char=%.2f us\n",
                    count, kJoints, p50, p95, 1000.0 * p50 / count);
    }
}
