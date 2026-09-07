#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <BoneMask/BoneMask.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/AnimationPose.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/AnimationLayerComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
//  Animation layers: bone masks, override and additive blending.
//
//  All physics-free. The observable output of the whole stage is
//  SkeletalMeshComponent::boneMatrices, so "the pose is unchanged" is checked by
//  comparing those matrices BIT FOR BIT against a run without the layer — the
//  strongest form of the claim, and the one that catches a slerp that is only
//  nearly the identity at weight 0.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

constexpr std::array<float, 16> kIdentity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

// Root(0) ─ Spine(1) ─ Arm(2)
//         └ Leg(3)
// Four joints so a mask can hit one branch and a test can see the other branch
// stay exactly where the base put it.
SkeletalMeshAsset makeSkeleton(const HE::UUID& meshId)
{
    SkeletalMeshAsset sma;
    sma.id   = meshId;
    sma.name = "layerSkel";
    auto joint = [&](const char* name, int parent)
    {
        SkeletonJoint j; j.name = name; j.parent = parent; j.inverseBindMatrix = kIdentity;
        sma.skeleton.push_back(j);
    };
    joint("Root",  -1);
    joint("Spine",  0);
    joint("Arm",    1);
    joint("Leg",    0);
    return sma;
}

// A clip that holds ONE joint at a constant rotation about Y for its whole
// duration. Constant on purpose: a test about blending should not also depend on
// where a playhead happens to be.
AnimationClipAsset makeRotationClip(const char* name, uint32_t jointIndex,
                                    float degrees, float duration = 1.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = name;

    const glm::quat q = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 1, 0));
    AnimationChannel ch;
    ch.jointIndex = jointIndex;
    ch.path       = AnimPathType::Rotation;
    ch.times      = { 0.0f, duration };
    ch.values     = { q.x, q.y, q.z, q.w,  q.x, q.y, q.z, q.w };  // glTF xyzw
    clip.channels.push_back(std::move(ch));
    return clip;
}

// The root walks `distance` along +Z over the whole clip.
AnimationClipAsset makeWalkClip(float duration = 1.0f, float distance = 2.0f)
{
    AnimationClipAsset clip;
    clip.duration = duration;
    clip.name     = "walkZ";

    AnimationChannel ch;
    ch.jointIndex = 0;
    ch.path       = AnimPathType::Translation;
    ch.times      = { 0.0f, duration };
    ch.values     = { 0.0f, 0.0f, 0.0f,  0.0f, 0.0f, distance };
    clip.channels.push_back(std::move(ch));
    return clip;
}

HE::UUID registerMask(ContentManager& cm, const HE::BoneMask& mask)
{
    BoneMaskAsset a;
    a.name = mask.name;
    a.json = HE::boneMaskToJson(mask);
    return cm.registerBoneMask(std::move(a));
}

// One entity: skeletal mesh + a plain AnimatorComponent on `baseClip`.
struct Rig
{
    ContentManager cm;
    HorizonWorld   world;
    HE::UUID       meshId;
    entt::entity   entity = entt::null;

    HE::UUID addClip(AnimationClipAsset c) { return cm.registerAnimationClip(std::move(c)); }

    const std::vector<glm::mat4>& bones()
    {
        return world.registry().get<SkeletalMeshComponent>(entity).boneMatrices;
    }
    AnimationLayerComponent& layers()
    {
        return world.registry().get<AnimationLayerComponent>(entity);
    }
};

std::unique_ptr<Rig> makeRig(AnimationClipAsset baseClip)
{
    auto rig = std::make_unique<Rig>();
    rig->meshId = HE::UUID::generate();
    rig->cm.registerSkeletalMesh(makeSkeleton(rig->meshId));
    const HE::UUID baseId = rig->cm.registerAnimationClip(std::move(baseClip));

    rig->entity = rig->world.createEntity("Character");
    rig->world.addComponent(rig->entity, TransformComponent{});
    SkeletalMeshComponent smc; smc.meshAssetId = rig->meshId;
    rig->world.addComponent(rig->entity, smc);
    AnimatorComponent an; an.clipAssetId = baseId; an.looping = true;
    rig->world.addComponent(rig->entity, an);
    return rig;
}

// Bit-for-bit, not approximately. "This layer changed nothing" is a claim about
// the bits or it is not a claim.
bool sameBits(const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b)
{
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(glm::mat4)) == 0;
}

int jointIndexOf(const SkeletalMeshAsset& mesh, const char* name)
{
    for (size_t i = 0; i < mesh.skeleton.size(); ++i)
        if (mesh.skeleton[i].name == name) return static_cast<int>(i);
    return -1;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  1. Mask resolution
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("bone mask: a named joint gets its weight and everything else gets 0")
{
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());

    HE::BoneMask mask;
    mask.name = "UpperBody";
    mask.entries.push_back({ "Spine", 1.0f });
    mask.entries.push_back({ "Arm",   0.5f });

    std::vector<float> w;
    HE::resolveBoneMask(mesh, mask, w);

    REQUIRE(w.size() == 4);
    CHECK(w[0] == doctest::Approx(0.0f));  // Root
    CHECK(w[1] == doctest::Approx(1.0f));  // Spine
    CHECK(w[2] == doctest::Approx(0.5f));  // Arm
    CHECK(w[3] == doctest::Approx(0.0f));  // Leg
}

TEST_CASE("bone mask: a name the skeleton does not have is 0, never 1")
{
    // The whole point of the rule. A mask that hits nothing must mean "this layer
    // does nothing"; falling back to "affects everything" would turn a typo into
    // a character overwritten from the neck down, with no message anywhere.
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());

    HE::BoneMask mask;
    mask.entries.push_back({ "Hips", 1.0f });   // no such joint

    std::vector<float> w;
    HE::resolveBoneMask(mesh, mask, w);

    REQUIRE(w.size() == 4);
    for (float v : w) CHECK(v == doctest::Approx(0.0f));
}

TEST_CASE("bone mask: an empty mask is 'everything', not 'nothing'")
{
    const SkeletalMeshAsset mesh = makeSkeleton(HE::UUID::generate());
    std::vector<float> w;
    HE::resolveBoneMask(mesh, HE::BoneMask{}, w);

    REQUIRE(w.size() == 4);
    for (float v : w) CHECK(v == doctest::Approx(1.0f));
}

TEST_CASE("bone mask: JSON round-trips names and weights, and drops nameless entries")
{
    HE::BoneMask mask;
    mask.name = "UpperBody";
    mask.entries.push_back({ "Spine", 1.0f });
    mask.entries.push_back({ "Arm",   0.25f });

    HE::BoneMask back;
    REQUIRE(HE::boneMaskFromJson(HE::boneMaskToJson(mask), back));
    CHECK(back.name == "UpperBody");
    REQUIRE(back.entries.size() == 2);
    CHECK(back.entries[0].joint == "Spine");
    CHECK(back.entries[1].weight == doctest::Approx(0.25f));

    // An entry with no joint name would resolve to nothing and warn about a joint
    // nobody named, so it never survives the read.
    HE::BoneMask stray;
    REQUIRE(HE::boneMaskFromJson(R"({"entries":[{"weight":1.0},{"joint":"Arm"}]})", stray));
    REQUIRE(stray.entries.size() == 1);
    CHECK(stray.entries[0].joint == "Arm");
}

// ─────────────────────────────────────────────────────────────────────────────
//  2. The resolution cache
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer masks: a second tick against the same mesh does not re-resolve")
{
    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));

    HE::BoneMask mask; mask.name = "Arm"; mask.entries.push_back({ "Arm", 1.0f });
    const HE::UUID maskId = registerMask(rig->cm, mask);

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.name = "upper"; l.clipId = layerClip; l.maskId = maskId; l.weight = 1.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    REQUIRE(rig->layers().resolvedMasks.size() == 1);
    CHECK_FALSE(rig->layers().masksDirty);

    // A sentinel no resolve would ever produce. It survives a tick exactly when
    // the cache was NOT rebuilt.
    rig->layers().resolvedMasks[0][0] = 0.125f;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().resolvedMasks[0][0] == doctest::Approx(0.125f));

    // A different skeleton is a different set of joint indices, so it must.
    const HE::UUID otherMesh = HE::UUID::generate();
    rig->cm.registerSkeletalMesh(makeSkeleton(otherMesh));
    rig->world.registry().get<SkeletalMeshComponent>(rig->entity).meshAssetId = otherMesh;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().resolvedMasks[0][0] == doctest::Approx(0.0f));
}

TEST_CASE("layer masks: swapping the mask asset re-resolves without anyone setting a flag")
{
    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));

    HE::BoneMask armMask; armMask.entries.push_back({ "Arm", 1.0f });
    HE::BoneMask legMask; legMask.entries.push_back({ "Leg", 1.0f });
    const HE::UUID armId = registerMask(rig->cm, armMask);
    const HE::UUID legId = registerMask(rig->cm, legMask);

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.clipId = layerClip; l.maskId = armId;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().resolvedMasks[0][2] == doctest::Approx(1.0f));  // Arm

    // Only the id changes — masksDirty is left alone deliberately, because the
    // point is that a writer who forgets it does not get a stale mask.
    rig->layers().layers[0].maskId = legId;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().resolvedMasks[0][2] == doctest::Approx(0.0f));  // Arm
    CHECK(rig->layers().resolvedMasks[0][3] == doctest::Approx(1.0f));  // Leg
}

// ─────────────────────────────────────────────────────────────────────────────
//  3-5. Override blending
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer: weight 0 leaves the base pose bit for bit")
{
    auto bare = makeRig(makeRotationClip("base", 1, 10.0f));
    SceneSystems::tickAnimation(bare->world, bare->cm, 0.1f);
    const std::vector<glm::mat4> reference = bare->bones();

    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));
    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 0.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    CHECK(sameBits(rig->bones(), reference));
}

TEST_CASE("layer: weight 1 with no mask is the layer pose, bit for bit")
{
    // What "the layer wins outright" has to mean: the same matrices a rig posed
    // by that clip alone would produce.
    auto solo = makeRig(makeRotationClip("arm", 2, 40.0f));
    SceneSystems::tickAnimation(solo->world, solo->cm, 0.1f);
    const std::vector<glm::mat4> reference = solo->bones();

    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));
    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 1.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    CHECK(sameBits(rig->bones(), reference));
}

TEST_CASE("layer: a mask keeps the layer off the joints it does not name")
{
    // The one that makes layers worth having: the arm follows the layer, the leg
    // is exactly what the base left there.
    auto bare = makeRig(makeRotationClip("base", 3, 25.0f));   // base moves the Leg
    SceneSystems::tickAnimation(bare->world, bare->cm, 0.1f);
    const std::vector<glm::mat4> baseOnly = bare->bones();

    auto solo = makeRig(makeRotationClip("arm", 2, 40.0f));
    SceneSystems::tickAnimation(solo->world, solo->cm, 0.1f);
    const std::vector<glm::mat4> layerOnly = solo->bones();

    auto rig = makeRig(makeRotationClip("base", 3, 25.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));
    HE::BoneMask mask; mask.name = "Arm"; mask.entries.push_back({ "Arm", 1.0f });
    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.clipId = layerClip; l.maskId = registerMask(rig->cm, mask); l.weight = 1.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    const SkeletalMeshAsset& mesh = *rig->cm.getSkeletalMesh(rig->meshId);
    const int leg = jointIndexOf(mesh, "Leg");
    const int arm = jointIndexOf(mesh, "Arm");
    REQUIRE(leg >= 0);
    REQUIRE(arm >= 0);

    CHECK(std::memcmp(&rig->bones()[leg], &baseOnly[leg],  sizeof(glm::mat4)) == 0);
    CHECK(std::memcmp(&rig->bones()[arm], &layerOnly[arm], sizeof(glm::mat4)) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  6-7. Additive blending
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer: an additive layer sampled at its own reference pose changes nothing")
{
    // The test that tells inverse(ref) * layer from layer * inverse(ref): with the
    // layer clip EQUAL to the reference, the delta is the identity either way, so
    // any result other than the base means the delta was not taken in the right
    // frame — or that a not-quite-identity slerp was let through.
    auto bare = makeRig(makeRotationClip("base", 1, 30.0f));
    SceneSystems::tickAnimation(bare->world, bare->cm, 0.1f);
    const std::vector<glm::mat4> reference = bare->bones();

    auto rig = makeRig(makeRotationClip("base", 1, 30.0f));
    // Constant clip: whatever time the playhead is at, the pose equals the pose at
    // additiveRefTime, so the delta is the identity.
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));
    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.clipId = layerClip;
    l.mode   = HE::LayerBlendMode::Additive;
    l.weight = 1.0f;
    l.additiveRefTime = 0.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    CHECK(sameBits(rig->bones(), reference));
}

TEST_CASE("layer: additive at half weight is half the DELTA, not half the absolute pose")
{
    // Base holds the Arm at 20°, the layer's clip at 60°, its reference at 0°.
    //   additive: 20 + 0.5 · (60 − 0) = 50°
    //   absolute: mix(20, 60, 0.5)    = 40°
    // The two answers differ, which is the only reason this case says anything.
    auto rig = makeRig(makeRotationClip("base", 2, 20.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("armAdd", 2, 60.0f));
    const HE::UUID refClip   = rig->addClip(makeRotationClip("armRef", 2, 0.0f));

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.clipId            = layerClip;
    l.additiveRefClipId = refClip;
    l.mode              = HE::LayerBlendMode::Additive;
    l.weight            = 0.5f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);

    const SkeletalMeshAsset& mesh = *rig->cm.getSkeletalMesh(rig->meshId);
    const int arm = jointIndexOf(mesh, "Arm");
    REQUIRE(arm >= 0);

    // Arm's parents (Root, Spine) are untouched by every clip here, so its model
    // matrix IS its local rotation.
    const glm::quat got = glm::quat_cast(glm::mat3(rig->bones()[arm]));
    const float gotDegrees = glm::degrees(2.0f * std::atan2(std::abs(got.y), std::abs(got.w)));

    CHECK(gotDegrees == doctest::Approx(50.0f).epsilon(0.001));
    CHECK(gotDegrees != doctest::Approx(40.0f).epsilon(0.001));
}

// ─────────────────────────────────────────────────────────────────────────────
//  8. The root rule
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer: a layer cannot write back the root translation root motion took out")
{
    // Base clip walks the root along +Z and a RootMotionComponent takes that
    // motion out of the pose. A full-coverage override layer whose own clip also
    // walks the root would otherwise put it straight back, and the character would
    // travel twice: once as an entity and once as a sliding mesh.
    auto rig = makeRig(makeWalkClip(1.0f, 2.0f));
    const HE::UUID layerClip = rig->addClip(makeWalkClip(1.0f, 8.0f));

    RootMotionComponent rm; rm.mode = RootMotionComponent::Mode::Transform;
    rig->world.addComponent(rig->entity, rm);

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 1.0f;  // no mask
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    SceneSystems::tickAnimation(rig->world, rig->cm, 0.25f);

    // RootMotionLock::Zero puts the root's translation back at the clip's frame-0
    // value, which is the origin here. The layer must not have moved it.
    const glm::vec3 rootPos = glm::vec3(rig->bones()[0][3]);
    CHECK(rootPos.z == doctest::Approx(0.0f));
    CHECK(rootPos.x == doctest::Approx(0.0f));
    CHECK(rootPos.y == doctest::Approx(0.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  9. Once per entity per frame
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer: two drivers on one entity advance the layer playhead once")
{
    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeRotationClip("arm", 2, 40.0f));

    // A second driver. Nothing stops an entity carrying both today; what must not
    // happen is the layer stage running twice.
    AnimatorBlendComponent ab;
    ab.clipAId = layerClip;
    ab.clipBId = layerClip;
    rig->world.addComponent(rig->entity, ab);

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 1.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().layers[0].playbackTime == doctest::Approx(0.1f));

    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f);
    CHECK(rig->layers().layers[0].playbackTime == doctest::Approx(0.2f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  10. Notifies from a layer
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
AnimationClipAsset makeNotifyClip(const char* notifyName, float at, float duration = 1.0f)
{
    AnimationClipAsset clip = makeRotationClip("notifier", 2, 15.0f, duration);
    AnimationNotify n;
    n.name = notifyName;
    n.time = at;
    clip.notifies.push_back(std::move(n));
    return clip;
}

int countOf(const HE::NotifyQueue& q, const char* name)
{
    int n = 0;
    for (const auto& ev : q) if (ev.name == name) ++n;
    return n;
}
} // namespace

TEST_CASE("layer notifies: any weight above zero fires, alongside the base's own")
{
    // Deliberately NOT gated on kNotifyDominanceAlpha. That threshold is for two
    // ALTERNATIVES (walk vs run) where both firing gives two footsteps per step. A
    // layer is an addition, not an alternative: "magazine drops" on the reload
    // layer and "footstep" on the base are two events and both belong in the queue.
    auto rig = makeRig(makeNotifyClip("footstep", 0.05f));
    const HE::UUID layerClip = rig->addClip(makeNotifyClip("magazine", 0.05f));

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 0.2f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    HE::NotifyQueue q;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f, nullptr, nullptr, &q);

    CHECK(countOf(q, "footstep") == 1);
    CHECK(countOf(q, "magazine") == 1);
}

TEST_CASE("layer notifies: weight 0 stays silent but does not hoard its first frame")
{
    auto rig = makeRig(makeRotationClip("base", 1, 10.0f));
    const HE::UUID layerClip = rig->addClip(makeNotifyClip("magazine", 0.0f));

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l; l.clipId = layerClip; l.weight = 0.0f;
    lc.layers.push_back(l);
    rig->world.addComponent(rig->entity, lc);

    HE::NotifyQueue q;
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f, nullptr, nullptr, &q);
    CHECK(countOf(q, "magazine") == 0);
    CHECK(rig->layers().layers[0].notifiesPrimed);

    // Weight comes up. The frame-0 notify is BEHIND the playhead now and must not
    // come out — a layer that saved it up would fire a reload sound at the moment
    // somebody faded the layer in, one full clip out of place.
    rig->layers().layers[0].weight = 1.0f;
    q.clear();
    SceneSystems::tickAnimation(rig->world, rig->cm, 0.1f, nullptr, nullptr, &q);
    CHECK(countOf(q, "magazine") == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  11. blendTRS on unequal vectors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("blendTRS: the result is as long as the LONGER side, not the shorter")
{
    // It used to resize to min(), which silently deleted the tail of the longer
    // side. Nothing noticed while every caller passed two equal-length vectors —
    // and a layer blended against a shorter base is exactly the caller that would
    // not have.
    std::vector<JointTRS> a(3);
    a[2].translation = glm::vec3(1.0f, 2.0f, 3.0f);
    std::vector<JointTRS> b(1);

    std::vector<JointTRS> out;
    blendTRS(a, b, 0.0f, out);
    REQUIRE(out.size() == 3);
    CHECK(out[2].translation.x == doctest::Approx(1.0f));

    // At alpha 1 the missing side contributes the DEFAULT joint, not the value it
    // does not have.
    blendTRS(a, b, 1.0f, out);
    REQUIRE(out.size() == 3);
    CHECK(out[2].translation.x == doctest::Approx(0.0f));
    CHECK(out[2].scale.x       == doctest::Approx(1.0f));
}

TEST_CASE("blendTRS: alpha 0 and 1 are the ends, exactly")
{
    std::vector<JointTRS> a(1), b(1);
    a[0].rotation = glm::angleAxis(glm::radians(13.0f), glm::vec3(0, 1, 0));
    b[0].rotation = glm::angleAxis(glm::radians(77.0f), glm::vec3(1, 0, 0));

    std::vector<JointTRS> out;
    blendTRS(a, b, 0.0f, out);
    CHECK(std::memcmp(&out[0], &a[0], sizeof(JointTRS)) == 0);
    blendTRS(a, b, 1.0f, out);
    CHECK(std::memcmp(&out[0], &b[0], sizeof(JointTRS)) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  12. Serialisation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("layer serialisation: the authored fields survive, the runtime ones do not")
{
    HorizonWorld world;
    const entt::entity e = world.createEntity("Character");
    world.addComponent(e, TransformComponent{});

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.name              = "UpperBody";
    l.mode              = HE::LayerBlendMode::Additive;
    l.clipId            = HE::UUID::generate();
    l.maskId            = HE::UUID::generate();
    l.additiveRefClipId = HE::UUID::generate();
    l.additiveRefTime   = 0.25f;
    l.weight            = 0.75f;
    l.playbackTime      = 0.4f;
    l.playbackSpeed     = 1.5f;
    l.looping           = false;
    l.playing           = false;
    l.notifiesPrimed    = true;               // runtime: must NOT come back
    lc.layers.push_back(l);
    lc.resolvedForMeshId = HE::UUID::generate();  // runtime: must NOT come back
    lc.resolvedMasks.push_back({ 1.0f, 0.0f });
    world.addComponent(e, lc);

    SceneSerializer ser;
    std::vector<uint8_t> blob;
    REQUIRE(ser.saveToMemory(world, blob));

    HorizonWorld back;
    REQUIRE(ser.loadFromMemory(back, blob));

    entt::entity loaded = entt::null;
    for (auto [ent, c] : back.registry().view<AnimationLayerComponent>().each()) { loaded = ent; break; }
    REQUIRE(loaded != entt::entity{entt::null});
    const auto& got = back.registry().get<AnimationLayerComponent>(loaded);

    REQUIRE(got.layers.size() == 1);
    CHECK(got.layers[0].name              == "UpperBody");
    CHECK(got.layers[0].mode              == HE::LayerBlendMode::Additive);
    CHECK(got.layers[0].clipId            == l.clipId);
    CHECK(got.layers[0].maskId            == l.maskId);
    CHECK(got.layers[0].additiveRefClipId == l.additiveRefClipId);
    CHECK(got.layers[0].additiveRefTime   == doctest::Approx(0.25f));
    CHECK(got.layers[0].weight            == doctest::Approx(0.75f));
    CHECK(got.layers[0].playbackTime      == doctest::Approx(0.4f));
    CHECK(got.layers[0].playbackSpeed     == doctest::Approx(1.5f));
    CHECK(got.layers[0].looping           == false);
    CHECK(got.layers[0].playing           == false);

    // The playhead priming and the mask cache are re-established on the next tick
    // and would be a lie in the file: the cache belongs to a skeleton the loading
    // project may not even have.
    CHECK_FALSE(got.layers[0].notifiesPrimed);
    CHECK(got.resolvedForMeshId == HE::UUID{});
    CHECK(got.resolvedMasks.empty());
    CHECK(got.masksDirty);
}

TEST_CASE("layer asset refs: every clip and every mask a layer names is collected")
{
    // What is not listed here is not packed, and a layer whose mask did not travel
    // would go from "upper body only" to "affects nothing" in the packaged build.
    HorizonWorld world;
    const entt::entity e = world.createEntity("Character");
    world.addComponent(e, TransformComponent{});

    AnimationLayerComponent lc;
    AnimationLayerComponent::Layer l;
    l.clipId            = HE::UUID::generate();
    l.maskId            = HE::UUID::generate();
    l.additiveRefClipId = HE::UUID::generate();
    lc.layers.push_back(l);
    world.addComponent(e, lc);

    const std::vector<HE::UUID> refs = SceneSystems::collectAssetRefs(world);
    auto has = [&](HE::UUID id)
    { for (HE::UUID r : refs) if (r == id) return true; return false; };

    CHECK(has(l.clipId));
    CHECK(has(l.maskId));
    CHECK(has(l.additiveRefClipId));
}
