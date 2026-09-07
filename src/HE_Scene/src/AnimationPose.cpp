#include <HorizonScene/AnimationPose.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

namespace
{
// One joint of an override blend. Pulled out because the two ends have to be
// EXACT and a `if` per component inside the loop reads worse than this.
inline void overrideJoint(const JointTRS& a, const JointTRS& b, float t, JointTRS& out)
{
    if (t <= 0.0f) { out = a; return; }
    if (t >= 1.0f) { out = b; return; }
    out.translation = glm::mix(a.translation, b.translation, t);
    out.rotation    = glm::slerp(a.rotation,  b.rotation,    t);
    out.scale       = glm::mix(a.scale,       b.scale,       t);
}
} // namespace

void blendTRS(const std::vector<JointTRS>& a,
              const std::vector<JointTRS>& b,
              float                        alpha,
              std::vector<JointTRS>&       out)
{
    // max, not min: the shorter side contributes a default JointTRS rather than
    // truncating the result (see the header). A blend of two vectors of different
    // length used to lose the tail of the longer one without a word.
    const size_t count = std::max(a.size(), b.size());
    static const JointTRS kDefault{};
    out.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const JointTRS& ja = (i < a.size()) ? a[i] : kDefault;
        const JointTRS& jb = (i < b.size()) ? b[i] : kDefault;
        overrideJoint(ja, jb, alpha, out[i]);
    }
}

namespace HE {

void resolveBoneMask(const SkeletalMeshAsset& mesh, const BoneMask& mask,
                     std::vector<float>& outWeights)
{
    const size_t jointCount = mesh.skeleton.size();

    // No entries at all is the caller's "everything", not "nothing": an author
    // who made a mask asset and has not filled it in yet should see the layer,
    // not a layer that silently does nothing. A NAMED joint that does not exist
    // is the opposite case and is reported below.
    if (mask.entries.empty())
    {
        outWeights.assign(jointCount, 1.0f);
        return;
    }

    outWeights.assign(jointCount, 0.0f);
    for (const BoneMaskEntry& e : mask.entries)
    {
        bool found = false;
        for (size_t i = 0; i < jointCount; ++i)
        {
            if (mesh.skeleton[i].name != e.joint) continue;
            // A skeleton may carry the same joint name twice (two glTF nodes with
            // the same label under different parents). Both get the weight —
            // silently picking the first would make the mask depend on import
            // order, which is the very thing naming instead of indexing avoids.
            outWeights[i] = std::clamp(e.weight, 0.0f, 1.0f);
            found = true;
        }
        if (!found)
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Bone mask '%s' names joint '%s', which this skeleton does "
                            "not have — that joint contributes nothing (a mask never "
                            "falls back to affecting everything)",
                            mask.name.c_str(), e.joint.c_str());
    }
}

void applyLayerPose(const std::vector<JointTRS>& base,
                    const std::vector<JointTRS>& layer,
                    const std::vector<JointTRS>* ref,
                    const std::vector<float>*    maskWeights,
                    float weight, LayerBlendMode mode, int rootJoint,
                    std::vector<JointTRS>& out)
{
    const size_t count = std::max(base.size(), layer.size());
    static const JointTRS kDefault{};
    out.resize(count);

    const float layerWeight = std::clamp(weight, 0.0f, 1.0f);

    for (size_t i = 0; i < count; ++i)
    {
        const JointTRS& jb = (i < base.size())  ? base[i]  : kDefault;
        const JointTRS& jl = (i < layer.size()) ? layer[i] : kDefault;

        float w = layerWeight;
        if (maskWeights)
            w *= (i < maskWeights->size()) ? (*maskWeights)[i] : 0.0f;

        if (w <= 0.0f) { out[i] = jb; continue; }

        // The root's translation belongs to root motion, which has already taken
        // it out of the base pose. Writing it back from a layer moves the figure
        // twice — the same mistake lockRootJoint prevents one stage earlier.
        const bool lockRootTranslation = (rootJoint >= 0 && i == static_cast<size_t>(rootJoint));

        if (mode == LayerBlendMode::Override)
        {
            overrideJoint(jb, jl, w, out[i]);
            if (lockRootTranslation) out[i].translation = jb.translation;
            continue;
        }

        // Additive: the layer's difference against its reference pose, in the
        // joint's own local frame.
        const JointTRS& jr = (ref && i < ref->size()) ? (*ref)[i] : kDefault;

        // inverse(ref) * layer, applied on the RIGHT of the base rotation:
        // "what the layer did to the reference", said in the reference's frame.
        // The other order (layer * inverse(ref), on the left) says it in the
        // parent's frame, which tilts an arm raise by whatever the spine is
        // doing.
        const glm::quat deltaRot = glm::inverse(jr.rotation) * jl.rotation;
        // A delta below float noise is not a rotation, and saying so here is what
        // makes "an additive layer sampled AT its own reference pose changes
        // nothing" true to the bit rather than to five decimals. Without it,
        // inverse(q) * q is an identity that is off in the last place, and
        // base * that is a base that has drifted — every frame, cumulatively, for
        // a layer that is supposed to be doing nothing at all.
        const bool deltaIsIdentity = std::abs(std::abs(deltaRot.w) - 1.0f) <= 1e-7f;
        out[i].rotation = deltaIsIdentity
            ? jb.rotation
            : jb.rotation * glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), deltaRot, w);

        out[i].translation = lockRootTranslation
            ? jb.translation
            : jb.translation + (jl.translation - jr.translation) * w;
        // Additive scale is ADDITIVE, not multiplicative: at w = 0 the delta is
        // exactly 0 and the base passes through untouched. Multiplicatively the
        // neutral element would be 1 and the weighting a pow(), which nobody
        // wants to reason about at three in the morning.
        out[i].scale = jb.scale + (jl.scale - jr.scale) * w;
    }
}

} // namespace HE
