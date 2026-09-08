#pragma once
// Internal header: the entity half of root motion, shared by the clip, blend and
// state-machine systems. It is not in AnimationEval.h because it needs
// HorizonWorld and PhysicsWorld, which that header deliberately does not know.
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include "AnimationEval.h"   // JointTRS — the lock happens in the sampled pose
#include <entt/entt.hpp>
#include <vector>

class HorizonWorld;

namespace HE {

// Extract one clip's delta over the span (tPrev, tEnd] and take its root motion
// back out of `localTRS`. True when a delta was produced; false leaves everything
// untouched (root motion off for the entity, clip carries none, no root joint).
//
// The non-looping clamp lives here rather than in three call sites: a playhead
// that has just stopped at the end would otherwise hand the helper a span running
// past the clip, and the round decomposition would read that as a lap.
bool rootMotionSampleClip(entt::entity e, const RootMotionComponent& rm,
                          const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                          float tPrev, float tEnd, bool looping,
                          std::vector<JointTRS>& localTRS, RootMotionDelta& outDelta);

// Clear the per-frame flags on every RootMotionComponent. Called once at the top
// of SceneSystems::tickAnimation, before any driver has produced a delta.
void rootMotionBeginFrame(HorizonWorld& world);

// Record `delta` on the component and, with a context, move the entity by it.
//
// ctx == nullptr means the delta was extracted and the root was locked, but
// nothing moves: the editor authoring case. The pose is identical either way,
// which is the point — a preview that poses differently from the game is not one.
void rootMotionApply(HorizonWorld& world, RootMotionContext* ctx, entt::entity e,
                     RootMotionComponent& rm, const RootMotionDelta& delta, float dt);

// Settle the horizontal velocity of any character that was driven last frame and
// is not being driven this one. Called once at the end of tickAnimation.
void rootMotionEndFrame(HorizonWorld& world, RootMotionContext* ctx);

} // namespace HE
