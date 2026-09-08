#include "PoseFinalize.h"
#include "AnimationEval.h"
#include "PoseSource.h"
#include "NotifyCollect.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/AnimationIk.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/AnimationLayerComponent.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/IkComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <BoneMask/BoneMask.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace HE {

namespace {

// Rebuild the (mask, skeleton) resolution cache when it no longer describes what
// the component actually holds. Three things can invalidate it and only one of
// them (`masksDirty`) is a flag somebody has to remember to set — the other two
// are checked against reality, which is why a forgotten flag is a missed frame
// and not a mask that quietly affects the wrong joints for ever.
void refreshMaskCache(AnimationLayerComponent& lc, ContentManager& cm,
                      const SkeletalMeshAsset& mesh, HE::UUID meshId)
{
    const size_t n = lc.layers.size();
    bool stale = lc.masksDirty
              || !(lc.resolvedForMeshId == meshId)
              || lc.resolvedMasks.size() != n
              || lc.resolvedMaskIds.size() != n;
    for (size_t i = 0; !stale && i < n; ++i)
        if (!(lc.resolvedMaskIds[i] == lc.layers[i].maskId)) stale = true;
    if (!stale) return;

    lc.resolvedMasks.assign(n, {});
    lc.resolvedMaskIds.assign(n, HE::UUID{});
    for (size_t i = 0; i < n; ++i)
    {
        lc.resolvedMaskIds[i] = lc.layers[i].maskId;
        if (lc.layers[i].maskId == HE::UUID{}) continue;  // no mask = whole skeleton

        const BoneMaskAsset* asset = cm.getBoneMask(lc.layers[i].maskId);
        if (!asset)
        {
            // An unresolvable mask affects NOTHING, not everything. A missing
            // asset that opened the layer up to the whole skeleton would turn a
            // broken reference into a character overwritten from the neck down.
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Animation layer '%s': bone mask %016llx%016llx could not be "
                            "resolved — the layer affects nothing",
                            lc.layers[i].name.c_str(),
                            static_cast<unsigned long long>(lc.layers[i].maskId.hi),
                            static_cast<unsigned long long>(lc.layers[i].maskId.lo));
            lc.resolvedMasks[i].assign(mesh.skeleton.size(), 0.0f);
            continue;
        }

        BoneMask mask;
        if (!boneMaskFromJson(asset->json, mask))
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Animation layer '%s': bone mask asset '%s' is not readable — "
                            "the layer affects nothing",
                            lc.layers[i].name.c_str(), asset->name.c_str());
            lc.resolvedMasks[i].assign(mesh.skeleton.size(), 0.0f);
            continue;
        }
        resolveBoneMask(mesh, mask, lc.resolvedMasks[i]);
    }

    lc.resolvedForMeshId = meshId;
    lc.masksDirty        = false;
}

// One layer, onto `pose`. Advances the layer's own playhead, collects its
// notifies and blends its clip on top.
void applyOneLayer(entt::entity e, AnimationLayerComponent& lc,
                   AnimationLayerComponent::Layer& layer,
                   const std::vector<float>* maskWeights, ContentManager& cm, float dt,
                   const SkeletalMeshAsset& mesh, int rootJoint,
                   const std::unordered_map<std::string, float>* params,
                   std::vector<JointTRS>& pose, NotifyQueue* notifies)
{
    const bool wantsBlendSpace =
        layer.source == AnimationLayerComponent::Layer::Source::BlendSpace;

    // A layer has no parameters of its own. It borrows the entity's state-machine
    // parameters when there is a state machine, which is the map a script already
    // writes "Speed" into — and 0 on both axes otherwise, which the clamp turns
    // into the leftmost sample rather than into nothing at all.
    if (wantsBlendSpace && !params)
        HE_LOG_THROTTLE(Animation, Warning, 10.0,
                        "Entity %u: animation layer '%s' uses a blend space but the entity "
                        "has no state machine to read its parameters from — both axes read 0",
                        static_cast<uint32_t>(e), layer.name.c_str());

    const PoseSource src = makePoseSource(cm, lc.blendSpaces,
                                          wantsBlendSpace ? HE::UUID{} : layer.clipId,
                                          wantsBlendSpace ? layer.blendSpaceId : HE::UUID{},
                                          params);
    if (!src.valid())
    {
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: animation layer '%s' has no usable pose source — "
                        "the layer contributes nothing",
                        static_cast<uint32_t>(e), layer.name.c_str());
        return;
    }

    const bool  looping = src.looping(layer.looping);
    const float period  = src.period();
    const float rate    = src.rate();

    // Unwrapped, exactly as the drivers capture it: the span (tPrev, tEnd] with
    // its direction and its laps intact. Read off the WRAPPED playhead afterwards
    // it would be neither. In the playhead's own unit — seconds for a clip, phase
    // for a blend space — which is why the speed goes through `rate`.
    const float tPrev = layer.playbackTime;
    const float tEnd  = layer.playing ? tPrev + dt * layer.playbackSpeed * rate : tPrev;
    if (layer.playing)
        advancePlayback(layer.playbackTime, layer.playing,
                        layer.playbackSpeed * rate, looping, period, dt);

    // A layer fires its notifies as soon as it has ANY weight — not past
    // kNotifyDominanceAlpha. That threshold answers a different question: the two
    // sides of a crossfade are ALTERNATIVES (walk and run), and letting both fire
    // gives two footsteps per step. A layer is not an alternative to the base, it
    // is an addition to it: "magazine drops" on the reload layer and "footstep"
    // on the base are two events and both belong in the queue.
    //
    // A layer at weight 0 is still walked past (dominant=false), or it would hoard
    // its frame-0 notify and dump it the instant the weight came up.
    //
    // Root motion is the BASE's business. A layer whose clip carries motion in its
    // root simply does not contribute it — a null RootMotionComponent is handed in
    // so nothing is extracted, and the root's translation is pinned by
    // applyLayerPose below.
    std::vector<JointTRS> layerTRS;
    RootMotionDelta       unused;
    bool                  unusedHave = false;
    src.evaluate(e, mesh, looping, tPrev, tEnd, layer.playbackTime,
                 /*rmc=*/nullptr, /*dominant=*/layer.weight > 0.0f,
                 layer.notifiesPrimed, notifies, layerTRS, unused, unusedHave);

    std::vector<JointTRS> refTRS;
    const std::vector<JointTRS>* ref = nullptr;
    if (layer.mode == LayerBlendMode::Additive)
    {
        // Only a CLIP source has "its own clip at t = 0". An additive blend-space
        // layer without a named reference has nothing to take a difference
        // against, so it falls back to the identity pose — which is what a layer
        // authored against the bind pose means anyway.
        const AnimationClipAsset* clip = src.clip;
        // The reference the difference is taken against: another clip if one was
        // named, otherwise this layer's own clip at additiveRefTime — which is
        // "the pose this animation starts from", the ordinary way an additive
        // clip is authored.
        const AnimationClipAsset* refClip = clip;
        if (layer.additiveRefClipId != HE::UUID{})
            if (const AnimationClipAsset* c = cm.getAnimationClip(layer.additiveRefClipId))
                refClip = c;
        refTRS.assign(mesh.skeleton.size(), JointTRS{});
        if (refClip) sampleClip(*refClip, layer.additiveRefTime, refTRS);
        ref = &refTRS;
    }

    std::vector<JointTRS> out;
    applyLayerPose(pose, layerTRS, ref, maskWeights, layer.weight, layer.mode, rootJoint, out);
    pose = std::move(out);
}

// ── IK ───────────────────────────────────────────────────────────────────────

// How far the entity may move in one frame before the smoothing is treated as
// meaningless. A script that puts a character on the other side of the map does
// not want its feet easing across the gap for the next half second — and there is
// no other way to tell that apart from walking, because both are "the transform
// changed".
constexpr float kTeleportDistance = 2.0f;

// Names → indices, against one skeleton. Same shape and the same staleness rules
// as the mask cache above: the flag is the fast path, but the mesh id and the
// list lengths are checked against reality, so a forgotten flag costs a frame
// rather than pointing an IK chain at the wrong joints for ever.
void refreshIkJoints(IkComponent& ic, const SkeletalMeshAsset& mesh, HE::UUID meshId)
{
    const bool stale = ic.jointsDirty
                    || !(ic.resolvedForMeshId == meshId)
                    || ic.resolvedFeet.size()  != ic.feet.size()
                    || ic.resolvedChain.size() != ic.lookAt.chain.size();
    if (!stale) return;

    ic.resolvedFeet.assign(ic.feet.size(), glm::ivec3(-1));
    for (size_t i = 0; i < ic.feet.size(); ++i)
    {
        const auto& f = ic.feet[i];
        // An empty name is "not configured yet" — the state every foot is in for
        // the seconds between pressing Add Foot and typing into it. Left to fall
        // through it would log a missing joint '' every five seconds at exactly
        // the moment the user is looking for a real message.
        if (f.footJoint.empty()) continue;
        const int foot = findJointByName(mesh, f.footJoint);
        if (foot < 0)
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Foot IK: joint '%s' is not in skeleton '%s' — this foot is off",
                            f.footJoint.c_str(), mesh.name.c_str());
            continue;
        }
        // Empty names mean "the two joints above the foot", which is right for
        // every humanoid rig. Named explicitly for the ones it is not — a
        // digitigrade leg has a joint in between and bending the wrong two is a
        // dog walking on its ankles.
        const int knee = f.kneeJoint.empty() ? mesh.skeleton[static_cast<size_t>(foot)].parent
                                             : findJointByName(mesh, f.kneeJoint);
        const int hip  = (knee < 0) ? -1
                       : (f.hipJoint.empty() ? mesh.skeleton[static_cast<size_t>(knee)].parent
                                             : findJointByName(mesh, f.hipJoint));
        if (knee < 0 || hip < 0)
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Foot IK on '%s': no knee/hip above it — this foot is off",
                            f.footJoint.c_str());
            continue;
        }
        ic.resolvedFeet[i] = glm::ivec3(hip, knee, foot);
    }

    // The pelvis is whatever both legs hang off. Taken from the first solved
    // leg's hip rather than searched for: a rig where the two hips have different
    // parents is not a biped, and dropping one of them would tear it in half.
    ic.resolvedPelvis = findJointByName(mesh, ic.pelvisJoint);
    if (ic.resolvedPelvis < 0 && ic.pelvisJoint.empty())
        for (const glm::ivec3& r : ic.resolvedFeet)
            if (r.x >= 0) { ic.resolvedPelvis = mesh.skeleton[static_cast<size_t>(r.x)].parent; break; }

    // Kept index-parallel to lookAt.chain, -1 and all, so the weights beside it
    // still line up after a name that does not resolve.
    ic.resolvedChain.assign(ic.lookAt.chain.size(), -1);
    for (size_t i = 0; i < ic.lookAt.chain.size(); ++i)
    {
        ic.resolvedChain[i] = findJointByName(mesh, ic.lookAt.chain[i]);
        if (ic.resolvedChain[i] < 0)
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Look-at IK: joint '%s' is not in skeleton '%s' — it is skipped",
                            ic.lookAt.chain[i].c_str(), mesh.name.c_str());
    }

    ic.resolvedForMeshId = meshId;
    ic.jointsDirty       = false;
}

// One exponential step towards `target`, framerate-independent. Not `+= (t-s)*k*dt`,
// which overshoots and then rings at any dt a slow frame produces.
inline float easeTowards(float current, float target, float speed, float dt)
{
    if (speed <= 0.0f) return target;
    return current + (target - current) * (1.0f - std::exp(-speed * dt));
}

// Feet onto the ground, then the head onto its target. In that order, because
// the pelvis drop moves the spine's parent and a look-at solved before it would
// be aiming from a head that is about to move.
void applyIk(HorizonWorld& world, entt::entity e, IkComponent& ic,
             const SkeletalMeshAsset& mesh, HE::UUID meshId, PhysicsWorld* physics,
             float dt, std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    refreshIkJoints(ic, mesh, meshId);

    // worldMatrixOf and NOT TransformComponent::worldMatrix: inside the animation
    // phase nothing has run propagateTransforms yet, so that field is a frame old
    // — and for anything spawned this frame it is the identity. Four separate
    // systems have been bitten by it.
    const glm::mat4 entityWorld = HE::worldMatrixOf(world, e);
    const glm::mat4 toModel     = glm::inverse(entityWorld);
    const glm::vec3 entityPos(entityWorld[3]);

    const bool teleported = !ic.hasLastEntityPos
                         || glm::length(entityPos - ic.lastEntityPos) > kTeleportDistance;
    ic.lastEntityPos    = entityPos;
    ic.hasLastEntityPos = true;

    // The character's own up and forward, in model space. NOT a hardcoded +Y: a
    // Blender export carries a constant -90° X on its root, so model-space up
    // there is not where anybody guesses, and a pelvis dropped along the wrong
    // axis sinks the character sideways into the floor.
    const glm::vec3 upModel  = glm::normalize(glm::vec3(toModel * glm::vec4(0.0f, 1.0f,  0.0f, 0.0f)));
    const glm::vec3 fwdModel =                glm::vec3(toModel * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));

    // ── Feet ────────────────────────────────────────────────────────────────
    struct FootSolve
    {
        glm::ivec3 joints{ -1 };
        glm::vec3  targetModel{ 0.0f };
        glm::vec3  normalModel{ 0.0f };
        float      weight = 0.0f;
        bool       align  = false;
        float      maxPitch = 0.0f, maxRoll = 0.0f;
    };
    std::vector<FootSolve> toSolve;
    float drop = 0.0f;   // the deepest (most negative) offset any foot wants

    for (size_t i = 0; i < ic.feet.size(); ++i)
    {
        auto& f = ic.feet[i];
        const glm::ivec3 idx = (i < ic.resolvedFeet.size()) ? ic.resolvedFeet[i] : glm::ivec3(-1);
        // No physics, no ground, no foot IK — see the header. The smoothing state
        // is reset with it so that stepping back into play mode does not ease out
        // of an offset the character stopped believing in.
        if (!physics || f.weight <= 0.0f || idx.z < 0)
        {
            f.smoothedOffset = 0.0f;
            f.primed         = false;
            continue;
        }

        const glm::vec3 footModel(model[static_cast<size_t>(idx.z)][3]);
        const glm::vec3 footWorld(entityWorld * glm::vec4(footModel, 1.0f));

        // Ignoring the character's own body: without that the ray finds the
        // capsule the foot is inside of and every foot pins itself to its owner.
        const PhysicsWorld::RaycastHit hit =
            physics->raycast(footWorld + glm::vec3(0.0f, f.traceUp, 0.0f),
                             glm::vec3(0.0f, -1.0f, 0.0f),
                             std::max(0.0f, f.traceUp + f.traceDown),
                             static_cast<uint32_t>(e));
        if (!hit.hit)
        {
            // In the air. Weight to 0 rather than a target guessed at — the other
            // leg keeps its ground.
            f.smoothedOffset = 0.0f;
            f.primed         = false;
            continue;
        }

        const float raw = (hit.point.y + f.footHeightOffset) - footWorld.y;
        if (!f.primed || teleported) { f.smoothedOffset = raw; f.primed = true; }
        else f.smoothedOffset = easeTowards(f.smoothedOffset, raw, f.interpSpeed, dt);

        FootSolve s;
        s.joints      = idx;
        s.weight      = std::min(f.weight, 1.0f);
        s.targetModel = glm::vec3(toModel * glm::vec4(footWorld.x,
                                                      footWorld.y + f.smoothedOffset,
                                                      footWorld.z, 1.0f));
        s.align       = f.alignToNormal;
        s.normalModel = glm::vec3(toModel * glm::vec4(hit.normal, 0.0f));
        s.maxPitch    = f.maxPitchDegrees;
        s.maxRoll     = f.maxRollDegrees;
        toSolve.push_back(s);

        drop = std::min(drop, f.smoothedOffset);
    }

    // The drop, before any leg is solved. On a stair edge the low foot's target
    // is further down than its leg is long; lowering the whole body by that much
    // is what keeps it from straightening out, and the targets deliberately do
    // NOT come down with it — they are ground, and ground does not move.
    if (ic.adjustPelvis && ic.resolvedPelvis >= 0 && !toSolve.empty() && drop < -1e-5f)
        HE::offsetJointModel(mesh, ic.resolvedPelvis, upModel * drop, localTRS, model);

    for (const FootSolve& s : toSolve)
    {
        HE::solveTwoBoneIk(mesh, s.joints.x, s.joints.y, s.joints.z,
                           s.targetModel, s.weight, localTRS, model);
        if (s.align)
            HE::alignFootToNormal(mesh, s.joints.z, s.normalModel, upModel, fwdModel,
                                  s.maxPitch, s.maxRoll, s.weight, localTRS, model);
    }

    // ── Look-at ─────────────────────────────────────────────────────────────
    auto& la = ic.lookAt;
    if (!la.enabled || la.weight <= 0.0f || la.chain.empty())
    {
        la.primed = false;
        return;
    }

    glm::vec3 targetWorld = la.targetWorld;
    if (la.targetEntityId != HE::UUID{})
    {
        const Entity t = world.findByEntityId(la.targetEntityId);
        if (t == entt::null)
        {
            HE_LOG_THROTTLE(Animation, Warning, 10.0,
                            "Entity %u: look-at target entity is not in this scene — "
                            "the head keeps looking where the animation put it",
                            static_cast<uint32_t>(e));
            la.primed = false;
            return;
        }
        targetWorld = HE::worldPositionOf(world, t);
    }

    // The unresolved links drop out here and their weights with them, so a typo
    // in one joint name costs that link's share and not the whole turn.
    std::vector<int>   chain;
    std::vector<float> chainWeights;
    for (size_t i = 0; i < ic.resolvedChain.size(); ++i)
    {
        if (ic.resolvedChain[i] < 0) continue;
        chain.push_back(ic.resolvedChain[i]);
        chainWeights.push_back(i < la.chainWeights.size() ? la.chainWeights[i] : 1.0f);
    }
    if (chain.empty()) { la.primed = false; return; }

    const glm::vec3 targetModel(toModel * glm::vec4(targetWorld, 1.0f));
    const glm::vec2 want = HE::lookAtAngles(mesh, chain, model, targetModel, la.forwardLocal,
                                            la.maxYawDegrees, la.maxPitchDegrees);
    if (!la.primed || teleported) { la.smoothedAngles = want; la.primed = true; }
    else
    {
        la.smoothedAngles.x = easeTowards(la.smoothedAngles.x, want.x, la.interpSpeed, dt);
        la.smoothedAngles.y = easeTowards(la.smoothedAngles.y, want.y, la.interpSpeed, dt);
    }

    HE::applyLookAt(mesh, chain, chainWeights, la.smoothedAngles, la.forwardLocal,
                    std::min(la.weight, 1.0f), localTRS, model);
}

} // namespace

void poseBeginFrame(HorizonWorld& world)
{
    for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
        lc.finalizedThisFrame = false;
    for (auto [e, ic] : world.registry().view<IkComponent>().each())
        ic.finalizedThisFrame = false;
}

void poseFinalize(HorizonWorld& world, ContentManager& cm, float dt, entt::entity e,
                  const SkeletalMeshAsset& mesh, std::vector<JointTRS>& localTRS,
                  SkeletalMeshComponent& smc, PhysicsWorld* physics,
                  NotifyQueue* notifies)
{
    auto& reg = world.registry();
    auto* lc  = reg.try_get<AnimationLayerComponent>(e);

    if (lc && !lc->layers.empty())
    {
        if (lc->finalizedThisFrame)
        {
            // The entity carries a second pose driver. The last one still wins on
            // boneMatrices (that wart is older than this file), but the layer
            // stage must not run twice: two runs would advance every layer
            // playhead by 2·dt and queue every layer notify twice.
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u carries more than one animation driver — its layer "
                            "stack ran with the first one and is skipped here; the pose "
                            "you see is the last driver's, unlayered",
                            static_cast<uint32_t>(e));
        }
        else
        {
            lc->finalizedThisFrame = true;

            // The root joint's translation is off limits to layers, but only when
            // something actually took it: without a RootMotionComponent nothing
            // was extracted and nothing can be applied twice, so pinning the root
            // there would delete motion the base pose is legitimately carrying.
            int rootJoint = -1;
            if (const auto* rm = reg.try_get<RootMotionComponent>(e))
                rootJoint = findRootJoint(mesh, rm->options.rootJointName);

            refreshMaskCache(*lc, cm, mesh, smc.meshAssetId);

            // A blend-space layer needs numbers on its axes and has none of its
            // own. It reads the entity's state-machine parameters — the map a
            // script already writes "Speed" into for the base pose — so a layer
            // and the state driving it agree on what the character is doing.
            const auto* smForParams = reg.try_get<AnimatorStateMachineComponent>(e);
            const std::unordered_map<std::string, float>* params =
                smForParams ? &smForParams->params : nullptr;

            for (size_t i = 0; i < lc->layers.size(); ++i)
            {
                const std::vector<float>* weights =
                    (lc->layers[i].maskId == HE::UUID{} || i >= lc->resolvedMasks.size())
                        ? nullptr : &lc->resolvedMasks[i];
                applyOneLayer(e, *lc, lc->layers[i], weights, cm, dt, mesh, rootJoint,
                              params, localTRS, notifies);
            }
        }
    }

    // The FK, in its two halves, with IK in between. An entity with no IkComponent
    // — or one whose feet are all off the ground and whose look-at is disabled —
    // runs composeModelMatrices and applyInverseBind back to back, which IS
    // composeBoneMatrices: the same numbers, the same bits, no tolerance in the
    // claim that IK you did not ask for costs you nothing.
    std::vector<glm::mat4> model;
    composeModelMatrices(mesh, localTRS, model);

    if (auto* ic = reg.try_get<IkComponent>(e))
    {
        if (ic->finalizedThisFrame)
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u carries more than one animation driver — its IK ran "
                            "with the first one and is skipped here",
                            static_cast<uint32_t>(e));
        }
        else if (!ic->feet.empty() || ic->lookAt.enabled)
        {
            ic->finalizedThisFrame = true;
            applyIk(world, e, *ic, mesh, smc.meshAssetId, physics, dt, localTRS, model);
        }
    }

    applyInverseBind(mesh, model, smc.boneMatrices);
    smc.dirty = true;
}

void poseEndFrame(HorizonWorld& world)
{
    for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
    {
        if (lc.finalizedThisFrame || lc.layers.empty()) continue;
        // Nothing posed this entity, so there was no base pose to lay anything on.
        // Falling back to the bind pose here would make this a fourth writer of
        // boneMatrices for a case nobody builds on purpose; saying so is enough.
        HE_LOG_THROTTLE(Animation, Warning, 10.0,
                        "Entity %u has animation layers but no animator driving it — "
                        "layers are laid ON a base pose and there is none, so they do "
                        "nothing", static_cast<uint32_t>(e));
    }
    for (auto [e, ic] : world.registry().view<IkComponent>().each())
    {
        if (ic.finalizedThisFrame || (ic.feet.empty() && !ic.lookAt.enabled)) continue;
        // Same answer as above, one stage later: IK adjusts a pose and there is
        // none to adjust. "My character's feet do nothing" gets a line rather
        // than a fourth writer of boneMatrices.
        HE_LOG_THROTTLE(Animation, Warning, 10.0,
                        "Entity %u has IK but no animator driving it — IK corrects a pose "
                        "and there is none, so it does nothing", static_cast<uint32_t>(e));
    }
}

} // namespace HE
