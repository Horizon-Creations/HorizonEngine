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
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <cstring>
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
// it has one), then its IK (if it has any), then FK + IBM. Also sets smc.dirty and
// clears smc.poseKey (see PoseKey below).
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

// The animator gate (SCN-102). A driver whose pose inputs are the same as the
// ones it posed the bones from last time skips sampling, FK and IBM and leaves
// smc.boneMatrices standing: a paused animator, an editor with dt 0, a state
// machine resting in the last frame of a non-looping clip.
//
// Only the pose is skipped. Playheads advance, notifies are collected, sync graphs
// and transitions run exactly as before.
//
// The protocol: poseFinalize clears smc.poseKey; the driver stores its key after
// the call. So a key survives only while the driver that set it is the last writer
// of the bones, and a second driver on the entity, a Sequence or anything else
// that writes them through poseFinalize invalidates it without knowing about it.
// A writer that bypasses poseFinalize (CinematicPreview) clears it by hand.
//
// The key is a hash, not a struct, because the three drivers depend on different
// inputs. Asset pointers and the addresses of the channel and joint arrays go in
// next to the UUIDs: a hot-reloaded clip keeps its UUID but not its arrays.
class PoseKey
{
public:
    explicit PoseKey(uint64_t driverTag) { mix(driverTag); }

    PoseKey& id(const UUID& u)     { mix(u.hi); mix(u.lo); return *this; }
    PoseKey& ptr(const void* p)    { mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p))); return *this; }
    PoseKey& count(uint64_t n)     { mix(n); return *this; }
    // By bits: the pose is only the same when the time is the same, -0 included.
    PoseKey& real(float f)         { uint32_t b; std::memcpy(&b, &f, sizeof b); mix(b); return *this; }

    // Never 0, which means "no pose to keep".
    uint64_t value() const { return m_h ? m_h : 1; }

private:
    void mix(uint64_t v)
    {
        // splitmix64 over the running value: a bijection per step, so two inputs
        // that differ in one word cannot cancel out.
        uint64_t z = (m_h ^ v) + 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        m_h = z ^ (z >> 31);
    }
    uint64_t m_h = 0;
};

// Whether `e`'s pose comes from its driver alone. Root motion, a layer stack and
// IK each feed the bones from something the driver's key does not see (the root
// lock, the layers' own playheads, the ground and the look-at target), so an
// entity with any of them is posed every frame as before.
bool poseMayBeKept(const entt::registry& reg, entt::entity e);

} // namespace HE
