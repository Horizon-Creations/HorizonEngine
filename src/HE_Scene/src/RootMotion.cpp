#include <HorizonScene/RootMotion.h>
#include "AnimationEval.h"      // internal: JointTRS, sampleJoint — same src/ tree
#include <Diagnostics/Log.h>

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace
{

// A frame that spans more laps than this is not a frame, it is a hang or a
// nonsense playbackSpeed. Decomposing it honestly would loop for as long as the
// number is big; the guard stops and says so.
constexpr int kMaxRounds = 64;

// The yaw of a rotation, read off the direction it points +Z. Same convention the
// entity side uses to read its own facing out of a world matrix, so the two can
// never disagree about which way round the angle goes.
float yawOf(const glm::quat& q)
{
    const glm::vec3 f = q * glm::vec3(0.0f, 0.0f, 1.0f);
    const float planar = std::sqrt(f.x * f.x + f.z * f.z);
    if (planar < 1e-6f) return 0.0f;   // pointing straight up/down: no yaw to read
    return glm::degrees(std::atan2(f.x, f.z));
}

glm::quat yawQuat(float degrees)
{
    return glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
}

// The delta over ONE segment that does not cross the clip's edge.
//
// The displacement is put into the character's frame at `a` by un-rotating it with
// the HEADING at `a` — the turning the root has done since frame 0 — and not with
// the root's raw rotation. On a Blender export the root carries a constant -90° X,
// and un-rotating a horizontal step by that tilt makes it vertical: the character
// climbs out of the floor. Referencing frame 0 cancels any constant tilt, and for
// a root that starts at identity the two are the same expression.
HE::RootMotionDelta segmentDelta(const AnimationClipAsset& clip, uint32_t rootJoint,
                                 float a, float b, const glm::quat& invRest)
{
    const JointTRS A = sampleJoint(clip, rootJoint, a);
    const JointTRS B = sampleJoint(clip, rootJoint, b);

    HE::RootMotionDelta d;
    d.translation = yawQuat(-yawOf(A.rotation * invRest)) * (B.translation - A.translation);
    // Mesh-space relative rotation: a constant tilt cancels between the two, so
    // this needs no rest reference of its own.
    d.yawDegrees = yawOf(B.rotation * glm::inverse(A.rotation));
    return d;
}

// Chain a segment onto what is already accumulated. Deltas are transforms, so they
// COMPOSE — the next segment's translation is expressed in the frame the previous
// ones already turned into. Adding them instead is the bug that makes a figure
// drift sideways out of every curve.
void composeInto(HE::RootMotionDelta& total, const HE::RootMotionDelta& seg)
{
    total.translation += yawQuat(total.yawDegrees) * seg.translation;
    total.yawDegrees  += seg.yawDegrees;
}

} // namespace

int HE::findRootJoint(const SkeletalMeshAsset& mesh, const std::string& name)
{
    if (!name.empty())
    {
        for (size_t i = 0; i < mesh.skeleton.size(); ++i)
            if (mesh.skeleton[i].name == name) return static_cast<int>(i);
        return -1;   // named and not found: the caller reports it, it is not a default
    }
    for (size_t i = 0; i < mesh.skeleton.size(); ++i)
        if (mesh.skeleton[i].parent < 0) return static_cast<int>(i);
    return -1;
}

HE::RootMotionDelta HE::extractRootMotion(const AnimationClipAsset& clip, int rootJoint,
                                          float tPrev, float tEnd,
                                          const RootMotionOptions& opt)
{
    RootMotionDelta out;
    if (rootJoint < 0 || clip.duration <= 0.0f || tPrev == tEnd) return out;

    const uint32_t  joint   = static_cast<uint32_t>(rootJoint);
    const float     D       = clip.duration;
    const glm::quat invRest = glm::inverse(sampleJoint(clip, joint, 0.0f).rotation);

    // The playhead is always inside the clip; the far end is what may leave it.
    float a         = std::clamp(tPrev, 0.0f, D);
    float e         = a + (tEnd - tPrev);
    int   rounds    = 0;
    bool  exhausted = false;

    if (e >= a)
    {
        while (e > D)
        {
            if (rounds++ >= kMaxRounds) { exhausted = true; break; }
            composeInto(out, segmentDelta(clip, joint, a, D, invRest));
            e -= D;
            a  = 0.0f;
        }
        composeInto(out, segmentDelta(clip, joint, a, std::min(e, D), invRest));
    }
    else
    {
        while (e < 0.0f)
        {
            if (rounds++ >= kMaxRounds) { exhausted = true; break; }
            composeInto(out, segmentDelta(clip, joint, a, 0.0f, invRest));
            e += D;
            a  = D;
        }
        composeInto(out, segmentDelta(clip, joint, a, std::max(e, 0.0f), invRest));
    }

    if (exhausted)
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Root motion span (%.3f s .. %.3f s) covers more than %d laps of a "
                        "%.3f s clip — the rest is dropped. A frame that long is a stall or "
                        "a runaway playback speed, not motion anyone authored.",
                        static_cast<double>(tPrev), static_cast<double>(tEnd),
                        kMaxRounds, static_cast<double>(D));

    // Filtered last, deliberately: the composition above needs the clip's real
    // turning to place each segment, even when the entity is not going to be
    // turned by it.
    if (!opt.extractTranslationXZ) { out.translation.x = 0.0f; out.translation.z = 0.0f; }
    if (!opt.extractTranslationY)  { out.translation.y = 0.0f; }
    if (!opt.extractYaw)           { out.yawDegrees    = 0.0f; }
    return out;
}

HE::RootMotionDelta HE::blendRootMotion(const RootMotionDelta& a, const RootMotionDelta& b,
                                        float alpha)
{
    RootMotionDelta o;
    o.translation = glm::mix(a.translation, b.translation, alpha);
    // Shortest way round, so mixing 179° with -179° is two degrees and not
    // three hundred and fifty-eight.
    float d = b.yawDegrees - a.yawDegrees;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    o.yawDegrees = a.yawDegrees + d * alpha;
    return o;
}

void lockRootJoint(std::vector<JointTRS>& localTRS, int rootJoint,
                   const AnimationClipAsset& clip, const HE::RootMotionOptions& opt)
{
    if (rootJoint < 0 || static_cast<size_t>(rootJoint) >= localTRS.size()) return;

    JointTRS&      root  = localTRS[static_cast<size_t>(rootJoint)];
    const JointTRS first = sampleJoint(clip, static_cast<uint32_t>(rootJoint), 0.0f);

    const glm::vec3 target = (opt.lock == HE::RootMotionLock::FirstFrame)
        ? first.translation : glm::vec3(0.0f);

    if (opt.extractTranslationXZ) { root.translation.x = target.x; root.translation.z = target.z; }
    if (opt.extractTranslationY)  { root.translation.y = target.y; }

    if (opt.lock != HE::RootMotionLock::TranslationOnly && opt.extractYaw)
    {
        // Take the turning back out and leave everything else — the constant tilt
        // a Blender root carries stays, which is why this is not "set the rotation
        // to identity". Testing against the bind pose would fail for the same
        // reason: a bind-local root is rarely identity.
        const float heading = yawOf(root.rotation * glm::inverse(first.rotation));
        root.rotation = yawQuat(-heading) * root.rotation;
    }
}
