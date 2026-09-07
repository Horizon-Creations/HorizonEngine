#pragma once
// Internal header: the last stage of every pose driver. The clip, blend and
// state-machine systems each produce a base pose and then all three end the same
// way — layer stack on top, forward kinematics, inverse bind, boneMatrices. That
// ending used to be one line (`composeBoneMatrices`) copied three times; this is
// the same line, with the layer stack behind it.
//
// It is not in AnimationEval.h because it needs HorizonWorld and ContentManager,
// which that header deliberately does not know — the same split, for the same
// reason, as RootMotionApply.h.
#include <HorizonScene/AnimationPose.h>
#include <HorizonScene/AnimationNotify.h>
#include <entt/entt.hpp>
#include <vector>

class HorizonWorld;
class ContentManager;
class PhysicsWorld;
struct SkeletalMeshComponent;

namespace HE {

// Clear the per-frame flags on every AnimationLayerComponent and IkComponent.
// Called once at the top of SceneSystems::tickAnimation, next to
// rootMotionBeginFrame and for the same reason: an entity may carry more than one
// pose driver, and the stage below has to run exactly once per frame or every
// layer playhead advances twice and every foot eases towards the ground at double
// speed.
void poseBeginFrame(HorizonWorld& world);

// Finish `localTRS` into `smc.boneMatrices`: apply the entity's layer stack (if
// it has one), then its IK (if it has any), then FK + IBM. Also sets smc.dirty.
//
// `localTRS` is taken by value-in-place — it is the driver's own scratch pose and
// both stages write through it.
//
// Root motion has ALREADY been extracted and applied by the time this runs, in
// all three drivers. That ordering is what lets the layer stage lock the root
// joint's translation (a layer must not put back the motion that was just taken
// out), and it is what the IK stage needs: in Mode::Transform the entity's
// TransformComponent is written by rootMotionApply, and that is the transform the
// foot ray has to be cast from. Solved before it, the feet would probe the ground
// at last frame's position and lag the body by a frame at every step edge.
//
// `physics` is the ground the feet look for, and it is the SAME pointer the
// drivers already carry for root motion (`rootMotion ? rootMotion->physics :
// nullptr`) — no new context, no new plumbing through SceneSystems. Null means no
// play session: the foot solvers are forced to weight 0 rather than solved
// against a guessed plane, because a preview that poses differently from the game
// is not a preview. Look-at runs either way; pointing a head at a target in the
// editor and SEEING it is the entire point of having it there.
//
// Called a second time on the same entity in the same frame, it does the FK and
// nothing else, and says so once in the log.
void poseFinalize(HorizonWorld& world, ContentManager& cm, float dt, entt::entity e,
                  const SkeletalMeshAsset& mesh, std::vector<JointTRS>& localTRS,
                  SkeletalMeshComponent& smc, PhysicsWorld* physics,
                  NotifyQueue* notifies);

// Report AnimationLayerComponents that nothing finalized this frame. Called once
// at the end of tickAnimation: a layer stack on an entity with no base driver is
// silently inert, and "my layers do nothing" has to have an answer somewhere.
void poseEndFrame(HorizonWorld& world);

} // namespace HE
