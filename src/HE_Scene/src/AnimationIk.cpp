#include <HorizonScene/AnimationIk.h>

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace HE {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// glm/gtx/norm.hpp is behind GLM_ENABLE_EXPERIMENTAL and nothing else in this
// tree switches that on. A squared length is one dot product.
inline float length2(const glm::vec3& v) { return glm::dot(v, v); }

// The rotation half of a model matrix. Columns are normalised first: a joint
// carrying scale would otherwise hand quat_cast a matrix that is not a rotation,
// and the quaternion that comes back out of that is not one either.
glm::quat modelRotation(const glm::mat4& m)
{
    glm::vec3 x(m[0]), y(m[1]), z(m[2]);
    const float lx = glm::length(x), ly = glm::length(y), lz = glm::length(z);
    if (lx < 1e-8f || ly < 1e-8f || lz < 1e-8f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::mat3 r(x / lx, y / ly, z / lz);
    return glm::normalize(glm::quat_cast(r));
}

inline glm::vec3 originOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// The shortest rotation taking unit `a` to unit `b`. glm::rotation exists, but
// its antiparallel case picks an axis from the world axes and this one picks it
// from `a`, which is the difference between a foot that flips and a foot that
// turns.
glm::quat rotationBetween(const glm::vec3& a, const glm::vec3& b)
{
    const float d = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (d > 1.0f - 1e-7f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -1.0f + 1e-7f)
    {
        glm::vec3 axis = glm::cross(a, glm::vec3(1.0f, 0.0f, 0.0f));
        if (length2(axis) < 1e-8f) axis = glm::cross(a, glm::vec3(0.0f, 1.0f, 0.0f));
        return glm::angleAxis(kPi, glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}

bool validJoint(const SkeletalMeshAsset& mesh, int j)
{
    return j >= 0 && static_cast<size_t>(j) < mesh.skeleton.size();
}

// Turn one joint by `delta`, expressed in MODEL space, and bring the model
// matrices below it back in line.
//
// A local rotation lives in the parent's frame, so a model-space turn has to be
// conjugated into it: local' = inv(R_parent) · delta · R_parent · local. Writing
// `delta` straight onto the local rotation is the bug where a character facing
// away from the origin bends its knee sideways.
void rotateJointModel(const SkeletalMeshAsset& mesh, int j, const glm::quat& delta,
                      std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    if (static_cast<size_t>(j) >= localTRS.size()) return;
    const int32_t  parent = mesh.skeleton[static_cast<size_t>(j)].parent;
    const glm::quat Rp = (parent < 0) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                      : modelRotation(model[static_cast<size_t>(parent)]);
    localTRS[static_cast<size_t>(j)].rotation =
        glm::normalize(glm::inverse(Rp) * delta * Rp * localTRS[static_cast<size_t>(j)].rotation);
    refreshModelSubtree(mesh, localTRS, j, model);
}

// An orthonormal frame built around a forward direction, in whatever space
// `forward` was given in. `up` is +Y unless forward IS +Y, in which case +Z
// stands in — a rig whose head looks straight up its own spine is not a rig
// anybody has, but a normalize(cross(f, f)) would be a NaN in the pose.
struct AimFrame
{
    glm::vec3 f, r, u;
};

AimFrame aimFrame(const glm::vec3& forward)
{
    AimFrame fr;
    fr.f = glm::normalize(forward);
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(glm::dot(fr.f, up)) > 0.99f) up = glm::vec3(0.0f, 0.0f, 1.0f);
    fr.r = glm::normalize(glm::cross(fr.f, up));
    fr.u = glm::cross(fr.r, fr.f);
    return fr;
}

} // namespace

int findJointByName(const SkeletalMeshAsset& mesh, const std::string& name)
{
    if (name.empty()) return -1;
    for (size_t i = 0; i < mesh.skeleton.size(); ++i)
        if (mesh.skeleton[i].name == name) return static_cast<int>(i);
    return -1;
}

bool solveTwoBoneIk(const SkeletalMeshAsset& mesh, int hip, int knee, int foot,
                    const glm::vec3& targetModel, float weight,
                    std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    if (weight <= 0.0f) return false;
    if (!validJoint(mesh, hip) || !validJoint(mesh, knee) || !validJoint(mesh, foot)) return false;
    // A chain, or nothing. Handed three joints that are not hip → knee → foot the
    // solver would still produce numbers, and they would be a leg tied in a knot.
    if (mesh.skeleton[static_cast<size_t>(foot)].parent != knee) return false;
    if (mesh.skeleton[static_cast<size_t>(knee)].parent != hip) return false;
    if (localTRS.size() < mesh.skeleton.size()) return false;
    if (model.size() != mesh.skeleton.size()) composeModelMatrices(mesh, localTRS, model);

    const glm::vec3 A = originOf(model[static_cast<size_t>(hip)]);
    const glm::vec3 B = originOf(model[static_cast<size_t>(knee)]);
    const glm::vec3 C = originOf(model[static_cast<size_t>(foot)]);

    // The foot is already there. Not "solve with a tiny correction" — return, so
    // the pose comes out of this call the same bits it went in with.
    if (length2(targetModel - C) < kIkNoopDistance * kIkNoopDistance) return false;

    const float a = glm::length(B - A);   // thigh
    const float b = glm::length(C - B);   // shin
    if (a < 1e-6f || b < 1e-6f) return false;

    const JointTRS origHip  = localTRS[static_cast<size_t>(hip)];
    const JointTRS origKnee = localTRS[static_cast<size_t>(knee)];

    const glm::vec3 toTarget = targetModel - A;
    const float     reach    = glm::length(toTarget);
    if (reach < 1e-6f) return false;
    // Reachable range of a two-bone chain. The upper clamp is what makes an
    // out-of-range target straighten the leg (cos of the knee angle lands on
    // exactly -1) instead of overextending it.
    const float d = glm::clamp(reach, std::fabs(a - b) + 1e-4f, a + b);

    const glm::vec3 ab = B - A, ac = C - A;
    glm::vec3       axis(0.0f);
    bool            haveAxis = false;
    if (length2(ab) > 1e-12f && length2(ac) > 1e-12f)
    {
        const glm::vec3 cr = glm::cross(glm::normalize(ac), glm::normalize(ab));
        if (glm::length(cr) > 1e-4f) { axis = glm::normalize(cr); haveAxis = true; }
    }

    if (haveAxis)
    {
        // Law of cosines, twice: what the hip and the knee angles ARE against
        // what they have to be for the foot to sit `d` from the hip. Applied
        // about the plane the pose is already bent in, so which way the knee
        // points is the animator's decision and not the solver's.
        const glm::vec3 nab = glm::normalize(ab), nac = glm::normalize(ac);
        const float alpha0 = std::acos(glm::clamp(glm::dot(nab, nac), -1.0f, 1.0f));
        const float alpha1 = std::acos(glm::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f));
        const glm::vec3 nba = glm::normalize(A - B), nbc = glm::normalize(C - B);
        const float beta0 = std::acos(glm::clamp(glm::dot(nba, nbc), -1.0f, 1.0f));
        const float beta1 = std::acos(glm::clamp((a * a + b * b - d * d) / (2.0f * a * b), -1.0f, 1.0f));

        rotateJointModel(mesh, hip,  glm::angleAxis(alpha1 - alpha0, axis), localTRS, model);
        rotateJointModel(mesh, knee, glm::angleAxis(beta1  - beta0,  axis), localTRS, model);
    }

    // Now the chain is the right LENGTH; point it at the target.
    const glm::vec3 Cnow = originOf(model[static_cast<size_t>(foot)]);
    const glm::vec3 from = Cnow - A;
    if (length2(from) > 1e-12f)
        rotateJointModel(mesh, hip,
                         rotationBetween(glm::normalize(from), toTarget / reach),
                         localTRS, model);

    if (weight < 1.0f)
    {
        localTRS[static_cast<size_t>(hip)].rotation =
            glm::slerp(origHip.rotation,  localTRS[static_cast<size_t>(hip)].rotation,  weight);
        localTRS[static_cast<size_t>(knee)].rotation =
            glm::slerp(origKnee.rotation, localTRS[static_cast<size_t>(knee)].rotation, weight);
        refreshModelSubtree(mesh, localTRS, hip, model);
    }
    return true;
}

void alignFootToNormal(const SkeletalMeshAsset& mesh, int foot,
                       const glm::vec3& normalModel, const glm::vec3& upModel,
                       const glm::vec3& forwardModel,
                       float maxPitchDegrees, float maxRollDegrees, float weight,
                       std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    if (weight <= 0.0f || !validJoint(mesh, foot)) return;
    if (length2(normalModel) < 1e-12f || length2(upModel) < 1e-12f) return;
    if (localTRS.size() < mesh.skeleton.size()) return;
    if (model.size() != mesh.skeleton.size()) composeModelMatrices(mesh, localTRS, model);

    const glm::vec3 up = glm::normalize(upModel);
    glm::vec3 fwd = forwardModel - up * glm::dot(forwardModel, up);
    if (length2(fwd) < 1e-12f) return;
    fwd = glm::normalize(fwd);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, up));
    const glm::vec3 back  = glm::cross(right, up);          // right-handed with up

    const glm::vec3 n = glm::normalize(normalModel);
    const float ny = glm::dot(n, up);
    // A normal pointing away from the character's up is a ceiling, not a floor.
    if (ny <= 0.0f) return;

    // Pitch tips the toe, roll tips the ankle sideways, and each gets its own
    // limit because a foot has different amounts of each to give.
    const float pitch = std::atan2(glm::dot(n, back), ny);
    const float roll  = std::atan2(-glm::dot(n, right), ny);
    const float maxP  = glm::radians(std::max(0.0f, maxPitchDegrees));
    const float maxR  = glm::radians(std::max(0.0f, maxRollDegrees));
    const float p     = glm::clamp(pitch, -maxP, maxP);
    const float r     = glm::clamp(roll,  -maxR, maxR);
    if (std::fabs(p) < 1e-6f && std::fabs(r) < 1e-6f) return;

    glm::quat delta = glm::angleAxis(p, right) * glm::angleAxis(r, back);
    if (weight < 1.0f)
        delta = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta, weight);
    rotateJointModel(mesh, foot, delta, localTRS, model);
}

glm::vec2 lookAtAngles(const SkeletalMeshAsset& mesh, const std::vector<int>& chain,
                       const std::vector<glm::mat4>& model, const glm::vec3& targetModel,
                       const glm::vec3& forwardLocal,
                       float maxYawDegrees, float maxPitchDegrees)
{
    if (chain.empty() || model.size() != mesh.skeleton.size()) return glm::vec2(0.0f);
    const int root = chain.front(), end = chain.back();
    if (!validJoint(mesh, root) || !validJoint(mesh, end)) return glm::vec2(0.0f);
    if (length2(forwardLocal) < 1e-12f) return glm::vec2(0.0f);

    // Measured against the BODY, not against the head: "45° of yaw" has to mean
    // the same thing whatever the neck was already doing, or the clamp would
    // wander with the animation it is supposed to be limiting.
    const int32_t parent = mesh.skeleton[static_cast<size_t>(root)].parent;
    const glm::quat Rref = (parent < 0) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                        : modelRotation(model[static_cast<size_t>(parent)]);

    const glm::vec3 headPos = originOf(model[static_cast<size_t>(end)]);
    const glm::vec3 toTarget = targetModel - headPos;
    if (length2(toTarget) < 1e-12f) return glm::vec2(0.0f);

    const glm::vec3 d  = glm::inverse(Rref) * glm::normalize(toTarget);
    const AimFrame  fr = aimFrame(forwardLocal);
    const float fx = glm::dot(d, fr.r), fy = glm::dot(d, fr.u), fz = glm::dot(d, fr.f);

    const float yaw   = std::atan2(-fx, fz);
    const float pitch = std::atan2(fy, std::sqrt(fx * fx + fz * fz));
    const float maxY  = glm::radians(std::max(0.0f, maxYawDegrees));
    const float maxP  = glm::radians(std::max(0.0f, maxPitchDegrees));
    return glm::vec2(glm::degrees(glm::clamp(yaw,   -maxY, maxY)),
                     glm::degrees(glm::clamp(pitch, -maxP, maxP)));
}

bool applyLookAt(const SkeletalMeshAsset& mesh, const std::vector<int>& chain,
                 const std::vector<float>& chainWeights, const glm::vec2& anglesDegrees,
                 const glm::vec3& forwardLocal, float weight,
                 std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    if (weight <= 0.0f || chain.empty()) return false;
    if (localTRS.size() < mesh.skeleton.size()) return false;
    if (length2(forwardLocal) < 1e-12f) return false;
    const float yaw = glm::radians(anglesDegrees.x), pitch = glm::radians(anglesDegrees.y);
    // Nothing to turn. Return rather than slerp by zero — see kIkNoopDistance.
    if (std::fabs(yaw) < 1e-6f && std::fabs(pitch) < 1e-6f) return false;
    for (int j : chain) if (!validJoint(mesh, j)) return false;
    if (model.size() != mesh.skeleton.size()) composeModelMatrices(mesh, localTRS, model);

    const int32_t parent = mesh.skeleton[static_cast<size_t>(chain.front())].parent;
    const glm::quat Rref = (parent < 0) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                                        : modelRotation(model[static_cast<size_t>(parent)]);

    const AimFrame fr = aimFrame(forwardLocal);
    const glm::quat qRef  = glm::angleAxis(yaw, fr.u) * glm::angleAxis(pitch, fr.r);
    // One model-space turn, shared by every link. Same axis everywhere is what
    // makes the fractions below add up to the whole angle exactly: rotations
    // about one axis commute, so the chain's end lands where it would have if a
    // single joint had taken all of it.
    const glm::quat delta = Rref * qRef * glm::inverse(Rref);

    std::vector<float> w(chain.size(), 1.0f);
    if (chainWeights.size() == chain.size())
        for (size_t i = 0; i < w.size(); ++i) w[i] = std::max(0.0f, chainWeights[i]);
    float sum = 0.0f;
    for (float v : w) sum += v;
    if (sum <= 0.0f) { w.assign(chain.size(), 1.0f); sum = static_cast<float>(chain.size()); }
    for (float& v : w) v /= sum;

    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    bool changed = false;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const float t = w[i] * weight;
        if (t <= 0.0f) continue;
        rotateJointModel(mesh, chain[i], glm::slerp(identity, delta, t), localTRS, model);
        changed = true;
    }
    return changed;
}

void offsetJointModel(const SkeletalMeshAsset& mesh, int joint, const glm::vec3& deltaModel,
                      std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model)
{
    if (!validJoint(mesh, joint) || length2(deltaModel) < 1e-12f) return;
    if (localTRS.size() < mesh.skeleton.size()) return;
    if (model.size() != mesh.skeleton.size()) composeModelMatrices(mesh, localTRS, model);

    const int32_t parent = mesh.skeleton[static_cast<size_t>(joint)].parent;
    const glm::vec3 deltaLocal =
        (parent < 0) ? deltaModel
                     : glm::vec3(glm::inverse(model[static_cast<size_t>(parent)]) *
                                 glm::vec4(deltaModel, 0.0f));
    localTRS[static_cast<size_t>(joint)].translation += deltaLocal;
    refreshModelSubtree(mesh, localTRS, joint, model);
}

} // namespace HE
