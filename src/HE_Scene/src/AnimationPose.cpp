#include <HorizonScene/AnimationPose.h>
#include <Diagnostics/Log.h>

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

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

namespace
{
// The one place a JointTRS becomes a matrix. Shared by the full FK and the
// subtree refresh so the two cannot drift apart — a refresh that composed T*R*S
// in a different order would move a solved limb by a rounding error every frame.
inline glm::mat4 localMatrix(const JointTRS& trs)
{
    const glm::mat4 T = glm::translate(glm::mat4(1.0f), trs.translation);
    const glm::mat4 R = glm::mat4_cast(trs.rotation);
    const glm::mat4 S = glm::scale(glm::mat4(1.0f), trs.scale);
    return T * R * S;
}
} // namespace

void composeModelMatrices(const SkeletalMeshAsset&     mesh,
                          const std::vector<JointTRS>& localTRS,
                          std::vector<glm::mat4>&      outModel)
{
    const size_t jointCount = mesh.skeleton.size();
    outModel.resize(jointCount);
    for (size_t i = 0; i < jointCount; ++i)
    {
        const JointTRS  trs   = (i < localTRS.size()) ? localTRS[i] : JointTRS{};
        const glm::mat4 local = localMatrix(trs);
        const int32_t parent  = mesh.skeleton[i].parent;
        outModel[i] = (parent < 0) ? local : outModel[static_cast<size_t>(parent)] * local;
    }
}

void applyInverseBind(const SkeletalMeshAsset&      mesh,
                      const std::vector<glm::mat4>& model,
                      std::vector<glm::mat4>&       outBoneMatrices)
{
    const size_t jointCount = mesh.skeleton.size();
    outBoneMatrices.resize(jointCount);
    for (size_t i = 0; i < jointCount; ++i)
    {
        glm::mat4 ibm;
        std::memcpy(&ibm, mesh.skeleton[i].inverseBindMatrix.data(), sizeof(glm::mat4));
        const glm::mat4 m = (i < model.size()) ? model[i] : glm::mat4(1.0f);
        outBoneMatrices[i] = m * ibm;
    }
}

void refreshModelSubtree(const SkeletalMeshAsset&     mesh,
                         const std::vector<JointTRS>& localTRS,
                         int                          root,
                         std::vector<glm::mat4>&      model)
{
    const size_t jointCount = mesh.skeleton.size();
    if (root < 0 || static_cast<size_t>(root) >= jointCount) return;
    if (model.size() != jointCount) { composeModelMatrices(mesh, localTRS, model); return; }

    // "Below root" is decided by a flag that spreads from parent to child in one
    // forward sweep — legal because parents come first. A recursive walk would
    // need the child lists this skeleton does not store.
    std::vector<char> dirty(jointCount, 0);
    dirty[static_cast<size_t>(root)] = 1;
    for (size_t i = static_cast<size_t>(root); i < jointCount; ++i)
    {
        const int32_t parent = mesh.skeleton[i].parent;
        if (!dirty[i])
        {
            if (parent >= 0 && dirty[static_cast<size_t>(parent)]) dirty[i] = 1;
            else continue;
        }
        const JointTRS  trs   = (i < localTRS.size()) ? localTRS[i] : JointTRS{};
        const glm::mat4 local = localMatrix(trs);
        model[i] = (parent < 0) ? local : model[static_cast<size_t>(parent)] * local;
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

// ── Blend spaces ─────────────────────────────────────────────────────────────

namespace {

// Keep at most kBlendSpaceMaxActiveSamples non-zero weights and renormalise the
// survivors to sum 1. Ties are broken by the lower index (stable_sort), so the
// set that survives — and with it the sample that fires notifies — cannot
// flicker between two frames that computed the same weights.
void capAndNormalise(std::vector<float>& w)
{
    std::vector<size_t> order;
    order.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] > 0.0f) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return w[a] > w[b]; });

    for (size_t k = kBlendSpaceMaxActiveSamples; k < order.size(); ++k)
        w[order[k]] = 0.0f;

    float sum = 0.0f;
    for (float v : w) sum += v;
    if (sum <= 0.0f) return;   // nothing survived; the caller sees all-zero
    for (float& v : w) v /= sum;
}

// 1D: the bracketing pair in x, interpolated linearly, clamped at both ends.
void weights1D(const BlendSpace& space, float x, std::vector<float>& w)
{
    const size_t n = space.samples.size();
    std::vector<size_t> byX(n);
    for (size_t i = 0; i < n; ++i) byX[i] = i;
    // Stable, so samples sharing an x keep their authored order and the pick
    // below is the same every frame.
    std::stable_sort(byX.begin(), byX.end(),
                     [&](size_t a, size_t b) { return space.samples[a].x < space.samples[b].x; });

    if (x <= space.samples[byX.front()].x) { w[byX.front()] = 1.0f; return; }
    if (x >= space.samples[byX.back()].x)  { w[byX.back()]  = 1.0f; return; }

    for (size_t k = 0; k + 1 < n; ++k)
    {
        const float x0 = space.samples[byX[k]].x;
        const float x1 = space.samples[byX[k + 1]].x;
        if (!(x >= x0 && x <= x1)) continue;
        const float span = x1 - x0;
        if (span <= 0.0f) { w[byX[k]] = 1.0f; return; }  // two samples on one x
        const float t = (x - x0) / span;
        w[byX[k]]     = 1.0f - t;
        w[byX[k + 1]] = t;
        return;
    }
}

// 2D gradient band. For each sample i, walk every OTHER sample j and ask how far
// the query point has travelled from i towards j, measured along i→j and
// normalised by |i→j|²; the smallest "1 minus that" over all j is i's weight.
//
// At a sample point every term is 1 for that sample (it has not travelled at
// all) and <= 0 for the others, which is where "exactly 1.0 on a sample" comes
// from without a special case. Two samples in the SAME place would divide by
// zero, so that pair is skipped — they then simply share the weight.
void weights2D(const BlendSpace& space, float x, float y, std::vector<float>& w)
{
    const size_t n = space.samples.size();
    for (size_t i = 0; i < n; ++i)
    {
        const glm::vec2 pi(space.samples[i].x, space.samples[i].y);
        const glm::vec2 pq = glm::vec2(x, y) - pi;
        float wi = 1.0f;
        for (size_t j = 0; j < n; ++j)
        {
            if (j == i) continue;
            const glm::vec2 ij = glm::vec2(space.samples[j].x, space.samples[j].y) - pi;
            const float len2 = glm::dot(ij, ij);
            if (len2 <= 0.0f) continue;   // coincident samples: no direction to travel
            wi = std::min(wi, 1.0f - glm::dot(pq, ij) / len2);
            if (wi <= 0.0f) break;
        }
        w[i] = std::max(wi, 0.0f);
    }
}

} // namespace

void blendSpaceWeights(const BlendSpace& space, float x, float y,
                       std::vector<float>& outWeights)
{
    const size_t n = space.samples.size();
    outWeights.assign(n, 0.0f);
    if (n == 0) return;
    if (n == 1) { outWeights[0] = 1.0f; return; }

    if (space.kind == BlendSpaceKind::OneD) weights1D(space, x, outWeights);
    else                                    weights2D(space, x, y, outWeights);

    capAndNormalise(outWeights);
}

float blendSpaceWeightedDuration(const BlendSpace& space,
                                 const std::vector<float>& weights,
                                 const std::vector<float>& durations)
{
    float total = 0.0f;
    const size_t n = std::min(space.samples.size(), std::min(weights.size(), durations.size()));
    for (size_t i = 0; i < n; ++i)
    {
        if (weights[i] <= 0.0f || durations[i] <= 0.0f) continue;
        // speedScale is guarded to > 0 by blendSpaceFromJson; a hand-built
        // BlendSpace that skipped the parser gets the same guard here rather than
        // a division by zero.
        const float s = (space.samples[i].speedScale > 0.0f) ? space.samples[i].speedScale : 1.0f;
        total += weights[i] * durations[i] / s;
    }
    return total;
}

void blendSpaceEvalOrder(const std::vector<float>& weights, std::vector<size_t>& outOrder)
{
    outOrder.clear();
    for (size_t i = 0; i < weights.size(); ++i)
        if (weights[i] > 0.0f) outOrder.push_back(i);
    std::stable_sort(outOrder.begin(), outOrder.end(),
                     [&](size_t a, size_t b) { return weights[a] > weights[b]; });
}

void blendPosesN(const std::vector<std::vector<JointTRS>>& poses,
                 const std::vector<float>&                 weights,
                 std::vector<JointTRS>&                    out)
{
    std::vector<size_t> order;
    blendSpaceEvalOrder(weights, order);
    // Only poses that actually exist can anchor the chain.
    order.erase(std::remove_if(order.begin(), order.end(),
                               [&](size_t i) { return i >= poses.size(); }),
                order.end());

    if (order.empty()) { out.clear(); return; }

    out = poses[order[0]];
    float accW = weights[order[0]];
    for (size_t k = 1; k < order.size(); ++k)
    {
        const size_t i = order[k];
        const float  wi = weights[i];
        const float  denom = accW + wi;
        if (denom <= 0.0f) continue;
        std::vector<JointTRS> tmp;
        blendTRS(out, poses[i], wi / denom, tmp);
        out  = std::move(tmp);
        accW = denom;
    }
}

} // namespace HE
